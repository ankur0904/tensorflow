/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/tensorflow/transforms/cluster_ops_by_policy.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project

#define DEBUG_TYPE "cluster-ops-by-policy"

namespace mlir {
namespace TFDevice {

// -------------------------------------------------------------------------- //
// ValueConstraint.
// -------------------------------------------------------------------------- //

ValueConstraint Merge(ValueConstraint a, ValueConstraint b) {
  return a > b ? a : b;
}

raw_ostream &operator<<(raw_ostream &os, const ValueConstraint &constraint) {
  auto str = [](ValueConstraint constraint) -> StringRef {
    switch (constraint) {
      case ValueConstraint::kRank:
        return "rank";
      case ValueConstraint::kShape:
        return "shape";
      case ValueConstraint::kValue:
        return "value";
      default:
        llvm_unreachable("unknown value constraint");
    }
  };

  os << str(constraint);
  return os;
}

// -------------------------------------------------------------------------- //
// ValuesConstraintSet.
// -------------------------------------------------------------------------- //

void ValuesConstraintSet::Insert(ValueRange values,
                                 ValueConstraint constraint) {
  for (Value value : values) Insert(value, constraint);
}

std::pair<ValueConstraint, bool> ValuesConstraintSet::Insert(
    Value value, ValueConstraint constraint) {
  auto emplaced = constraints_.try_emplace(value, constraint);
  ValueConstraint persisted = emplaced.first->getSecond();

  // We've just inserted a new constraint for the value.
  if (emplaced.second) return {persisted, true};

  // Update existing constraint with a new one.
  auto merged = Merge(constraint, persisted);
  return {constraints_[value] = merged, merged != persisted};
}

void ValuesConstraintSet::Walk(
    llvm::function_ref<void(Value, ValueConstraint)> walk) const {
  for (auto &kv : constraints_) walk(kv.getFirst(), kv.getSecond());
}

Optional<ValueConstraint> ValuesConstraintSet::GetConstraint(
    Value value) const {
  auto it = constraints_.find(value);
  if (it == constraints_.end()) return None;
  return it->getSecond();
}

bool ValuesConstraintSet::HasConstraint(Value value) const {
  return GetConstraint(value).hasValue();
}

void ValuesConstraintSet::MergeAll(const ValuesConstraintSet &other) {
  other.Walk([this](Value value, ValueConstraint constraint) {
    Insert(value, constraint);
  });
}

ValuesConstraintSet &ValuesConstraintSet::Reset() {
  constraints_.clear();
  return *this;
}

size_t ValuesConstraintSet::Size() const { return constraints_.size(); }

bool ValuesConstraintSet::Empty() const { return constraints_.empty(); }

// -------------------------------------------------------------------------- //
// Discovering clusters of operations based on the policy.
// -------------------------------------------------------------------------- //

namespace {
constexpr char kDeviceAttr[] = "device";

// A type that abstracts over types that have uses accessible via `getUses`.
using Source = PointerUnion<Operation *, BlockArgument *>;

// We use union-find algorithm to build clusters of connected operations based
// on the user provided policy. If an operation can be clustered (one of the
// user provided policies accepts it under given constraints), it will become
// a "member" that will participate in the union-find cluster construction.
//
// A block argument can also become a member (or even a root member), however
// only operations will become a part of the outline `tf_device.cluster`, block
// arguments will stay as block arguments, and will later become cluster
// function inputs.
struct Member {
  Member(unsigned root, Source source, Operation *insertion_point,
         ValuesConstraintSet constraints = {})
      : root(root),
        source(source),
        insertion_point(insertion_point),
        constraints(constraints) {}

  unsigned root;
  Source source;

  // After construction:
  //  For basic block argument source this will be a first operation in the
  //  basic block, and for operations it will be an operation iself.
  //
  // During the union-find cluster formation:
  //  The root member will have the location in the basic block, where the
  //  cluster operation will be inserted. We use the location of the last
  //  operation in the cluster, so that during cluster construction we can
  //  ensure that all operands are above the insertion point, and all users are
  //  below the insertion point.
  //
  // Example:
  //
  //   %0 = "clustered_op"(...)
  //   %1 = "non_clustered_op"(...)
  //   %2 = "clustered_op"(%1)              <<<--- insert cluster here
  //   %3 = "cluster_result_user"(%1, %2)
  //
  // By using `%2` location as an insertion point we ensure that all operands
  // (%1 in this example) dominate the cluster operation, and that the cluster
  // operation dominates all the users (%3 in this example).
  Operation *insertion_point;

  // After construction:
  //   A set of constraints on the clustered operation operands that must be
  //   satisfied in order to add operation to the cluster. For basic block
  //   source this will be always empty.
  //
  // During the union-find cluster formation:
  //   The root member will have constraints merged from all of the cluster
  //   members.
  ValuesConstraintSet constraints;
};

using Members = llvm::SmallVector<Member>;

struct ClusteringState {
  // Storage backing an array based union-find algorithm for clustering. Index
  // in this vector is the member id.
  llvm::SmallVector<Member> members;

  // Mapping from the member operation (block argument) to the member id.
  llvm::SmallDenseMap<Source, unsigned> member_ids;

  // Puts `a` and `b` members into the same cluster if it is possible. Returns
  // success if union operation was completed successfully, otherwise returns
  // failure.
  //
  // Members can be clustered together:
  //   1. This will not break dominance property of the IR.
  //   2. New clustering policy constraints can be propagated through the
  //      already clustered operations.
  LogicalResult Union(unsigned a, unsigned b);

  bool IsMember(Operation *op) const;
  unsigned FindRoot(unsigned id);

  // Verifies that merging `src_root` cluster with a `dst_root` cluster, and
  // inserting it at `insertion_point` location will not break the dominance
  // property: all users of the `src_root` cluster results must be below the
  // insertion point in the block.
  LogicalResult VerifyDominanceProperty(unsigned src_root, unsigned dst_root,
                                        Operation *insertion_point);
};

}  // namespace

bool ClusteringState::IsMember(Operation *op) const {
  return member_ids.find(op) != member_ids.end();
}

unsigned ClusteringState::FindRoot(unsigned id) {
  if (members[id].root == id) return id;
  return members[id].root = FindRoot(members[id].root);
}

LogicalResult ClusteringState::VerifyDominanceProperty(
    unsigned src_root, unsigned dst_root, Operation *insertion_point) {
  // TODO(ezhulenev): Optimize this linear scan with a map lookup.
  for (auto &member : members) {
    unsigned root = FindRoot(member.root);
    if (root != src_root) continue;

    // Block arguments do not really participate in clustering, they are only
    // used to connect independent operation using the same argument.
    if (member.source.is<BlockArgument *>()) continue;

    Operation *op = member.source.dyn_cast<Operation *>();
    assert(op && "member operation must be not null");

    for (Operation *user : op->getUsers()) {
      // Skip users in other blocks.
      if (user->getBlock() != op->getBlock()) continue;

      // Skip users is in the `dst_root` or `src_root` clusters, if we'll merge
      // roots they'll become a single cluster and will not violate the
      // dominance property after that.
      auto it = member_ids.find(user);
      if (it != member_ids.end() && (FindRoot(it->getSecond()) == dst_root ||
                                     FindRoot(it->getSecond()) == src_root))
        continue;

      if (user->isBeforeInBlock(insertion_point)) {
        LLVM_DEBUG(llvm::dbgs()
                       << "  Failure: user is before the insertion point: "
                       << *user << "\n";);
        return failure();
      }
    }
  }

  return success();
}

LogicalResult ClusteringState::Union(unsigned a, unsigned b) {
  unsigned a_root = FindRoot(a);
  unsigned b_root = FindRoot(b);

  // Already members of the same cluster.
  if (a_root == b_root) return failure();

  // Verify that merging two clusters will not break dominance property.
  Operation *a_insertion_point = members[a_root].insertion_point;
  Operation *b_insertion_point = members[b_root].insertion_point;
  bool a_is_before_b = a_insertion_point->isBeforeInBlock(b_insertion_point);

  // Use clusters position in the block to select merging src and dst.
  unsigned src_root = a_is_before_b ? a_root : b_root;  // merge `src_root` ...
  unsigned dst_root = a_is_before_b ? b_root : a_root;  // ... into `dst_root`
  Operation *insertion_point =
      a_is_before_b ? b_insertion_point : a_insertion_point;

  // Print operations in the `root` cluster to debug stream.
  auto debug_clustered_ops = [&](unsigned root) {
    for (Member &member : members)
      if (FindRoot(member.root) == root) {
        if (auto *op = member.source.dyn_cast<Operation *>()) {
          llvm::dbgs() << "  " << *op << "\n";
        } else if (auto *arg = member.source.dyn_cast<BlockArgument *>()) {
          llvm::dbgs() << "  " << *arg;
        }
      }
  };
  (void)debug_clustered_ops;

  LLVM_DEBUG({
    llvm::dbgs() << "\n\n--- Try to merge cluster:\n";
    debug_clustered_ops(src_root);
    llvm::dbgs() << "\n--- With cluster:\n";
    debug_clustered_ops(dst_root);
    LLVM_DEBUG(llvm::dbgs() << "\n--- Diagnostics:\n");
  });

  // Check if merging `src_root` with `dst_root` will not violate SSA dominance
  // property (all operands before the cluster, all results after the cluster).
  if (failed(VerifyDominanceProperty(src_root, dst_root, insertion_point)))
    return failure();

  // TODO(ezhulenev): Verify that value constraints after merging clusters can
  // be satisfied.

  // Set `dst_root` as a new root for `src_root`.
  members[src_root].root = dst_root;
  // Update insertion point of the new root.
  members[dst_root].insertion_point = insertion_point;
  // Merge all constraints from `src_root` into `dst_root`.
  members[dst_root].constraints.MergeAll(members[src_root].constraints);

  LLVM_DEBUG(llvm::dbgs() << "  Clusters successfully merged\n");

  return success();
}

// Returns constrains on the operands specified by the clustering policy if the
// operation can be clustered (constraints could be empty). Otherwise return
// empty optional.
static Optional<ValuesConstraintSet> CanBeClustered(
    Operation *op, const ClusteringPolicySet &policies,
    const std::function<bool(Operation *op)> &filter) {
  // Check that op has no side effects. This guarantees that we will not
  // reorder side-effecting ops during cluster formation.
  if (!MemoryEffectOpInterface::hasNoEffect(op)) return llvm::None;

  // Operation rejected by the custom filter.
  if (filter && !filter(op)) return llvm::None;

  // Initially we do not have any constraints on the operation results.
  ValuesConstraintSet result_constraints;

  for (auto &policy : policies.policies()) {
    ValuesConstraintSet operands_constraints;
    if (succeeded(policy->MatchAndUpdateConstraints(op, result_constraints,
                                                    operands_constraints)))
      return operands_constraints;
  }

  return llvm::None;
}

// Compute initial clustering state based on the clustering polocy.
static ClusteringState InitializeClusteringState(
    Block *block, const ClusteringPolicySet &policies,
    const std::function<bool(Operation *op)> &filter) {
  ClusteringState state;

  // Create members for all block arguments.
  for (BlockArgument &arg : block->getArguments()) {
    if (!arg.getUsers().empty())
      state.members.emplace_back(state.members.size(), &arg, &block->front());
  }

  int num_bbarg_members = state.members.size();
  (void)num_bbarg_members;

  // Create members for operations that can be clustered based on the policy.
  for (Operation &op : block->getOperations()) {
    if (auto constraints = CanBeClustered(&op, policies, filter))
      state.members.emplace_back(state.members.size(), &op, &op, *constraints);
  }

  // Initialize mapping from the member operation (block argument) to the id.
  for (auto &tuple : llvm::enumerate(state.members)) {
    state.member_ids.try_emplace(tuple.value().source, tuple.index());
  }

  LLVM_DEBUG(llvm::dbgs() << "Found "
                          << (state.members.size() - num_bbarg_members)
                          << " clustering candidate operations in the block\n");

  return state;
}

// Users of the `source` that are candidates for clustering.
static llvm::SmallVector<Operation *> GetClusteringCandidates(
    const ClusteringState &state, Source source) {
  // Users of operation result must be in the same block and placed on the same
  // device.
  if (auto op = source.dyn_cast<Operation *>()) {
    auto range = llvm::make_filter_range(op->getUsers(), [&](Operation *user) {
      bool same_block = user->getBlock() == op->getBlock();
      bool same_device = op->getAttr(kDeviceAttr) == user->getAttr(kDeviceAttr);
      return same_block && same_device && state.IsMember(user);
    });
    return {range.begin(), range.end()};
  }

  // Users of block argument must be in the same block.
  if (auto arg = source.dyn_cast<BlockArgument *>()) {
    auto range = llvm::make_filter_range(arg->getUsers(), [&](Operation *user) {
      bool same_block = user->getBlock() == arg->getOwner();
      return same_block && state.IsMember(user);
    });
    return {range.begin(), range.end()};
  }

  llvm_unreachable("Unexpected type in the union.");
}

// Cluster members with their result users. Returns `true` if merged at least a
// pair of members into a new cluster.
static bool RunClusteringPass(ClusteringState &state) {
  bool clustered = false;

  for (auto &tuple : llvm::enumerate(state.members)) {
    size_t member_id = tuple.index();
    Member &member = tuple.value();

    llvm::SmallVector<Operation *> users =
        GetClusteringCandidates(state, member.source);

    // Process candidates according to their order in the block to minimize
    // the number of dominance property violations.
    llvm::sort(users, [](auto *a, auto *b) { return a->isBeforeInBlock(b); });

    for (Operation *user : users)
      if (succeeded(state.Union(member_id, state.member_ids.lookup(user))))
        clustered = true;
  }

  return clustered;
}

llvm::SmallVector<Cluster> FindClustersInTheBlock(
    Block *block, const ClusteringPolicySet &policies,
    std::function<bool(Operation *op)> filter) {
  // It is impossible to build a cluster in the empty block.
  if (block->empty()) return {};

  ClusteringState state = InitializeClusteringState(block, policies, filter);

  // Run clustering passes until the convergence. Limit the number of iterations
  // to guard from the infinite loop in presence of bugs.
  constexpr int max_iterations = 100;
  for (unsigned i = 0; i < max_iterations; ++i)
    if (!RunClusteringPass(state)) break;

  // Form clusters found by the union-find algorithm.
  llvm::DenseMap<unsigned, Cluster> root_clusters;

  for (Member &member : state.members) {
    unsigned root = state.FindRoot(member.root);
    Cluster &cluster = root_clusters.FindAndConstruct(root).getSecond();

    // If member is a root of the cluster, copy inferred constraints.
    if (state.FindRoot(member.root) == member.root)
      cluster.constraints = std::move(member.constraints);

    // Add operation to the cluster.
    if (auto op = member.source.dyn_cast<Operation *>())
      cluster.operations.emplace_back(op);
  }

  LLVM_DEBUG(llvm::dbgs() << "Found " << root_clusters.size() << " clusters\n");

  // Return found clusters through the output parameters.
  llvm::SmallVector<Cluster> clusters;
  for (auto &kv : root_clusters)
    clusters.emplace_back(std::move(kv.getSecond()));

  return clusters;
}

// -------------------------------------------------------------------------- //
// Create `tf_device.cluster` operation from the discovered ops cluster.
// -------------------------------------------------------------------------- //

tf_device::ClusterOp CreateClusterOp(Cluster &cluster, StringAttr policy) {
  // Find all the values that are used outside of the cluster. These values
  // will be returned from the created cluster operation.
  llvm::DenseSet<Operation *> in_cluster;
  for (Operation *op : cluster.operations) in_cluster.insert(op);

  llvm::SetVector<Value> return_values;
  llvm::SmallVector<Type> return_types;

  for (Operation *op : cluster.operations)
    for (OpOperand &use : op->getUses()) {
      // User is inside the cluster.
      if (in_cluster.contains(use.getOwner())) continue;
      // Do not return the same value multiple times.
      if (return_values.contains(use.get())) continue;

      return_values.insert(use.get());
      return_types.emplace_back(use.get().getType());
    }

  // Sort matched operations by their position in the block.
  llvm::sort(cluster.operations, [](Operation *a, Operation *b) -> bool {
    return a->isBeforeInBlock(b);
  });

  // Create tf_device::ClusterOp before the last operation in the block that
  // is a part of a match set.
  auto back = cluster.operations.back();
  auto loc = back->getLoc();
  OpBuilder builder(back);

  auto cluster_op =
      builder.create<tf_device::ClusterOp>(loc, return_types, policy);

  // Create block in cluster_op's region and move 'cluster.operations' into
  // it.
  auto block = builder.createBlock(&cluster_op.body());
  auto block_end = block->end();
  for (auto op : cluster.operations) op->moveBefore(block, block_end);

  // Add 'tf_device::ReturnOp' at the end of the block.
  builder.setInsertionPointToEnd(block);
  builder.create<tf_device::ReturnOp>(loc, return_values.getArrayRef());

  // Set device attribute
  if (auto device = back->getAttr(kDeviceAttr))
    cluster_op->setAttr(kDeviceAttr, device);

  // Update all users of the operations moved into the cluster region.
  for (auto tuple : llvm::zip(return_values, cluster_op.getResults())) {
    Value old_value = std::get<0>(tuple);
    Value new_value = std::get<1>(tuple);
    old_value.replaceUsesWithIf(new_value, [&](OpOperand &operand) -> bool {
      // Do not update users in the same cluster.
      return operand.getOwner()->getBlock() != block;
    });
  }

  return cluster_op;
}

// -------------------------------------------------------------------------- //
// Helper functions for value constraints propagations and analysis.
// -------------------------------------------------------------------------- //

mlir::LogicalResult PropagateValuesConstraints(
    mlir::Region &region, const ClusteringPolicySet &policies,
    ValuesConstraintSet &constraints) {
  // A set of constraints for operation results.
  llvm::DenseMap<Operation *, ValuesConstraintSet> op_results_constraints;

  // Use initial constraints to initialize op results constraints.
  for (std::pair<Value, ValueConstraint> pair : constraints) {
    Value value = pair.first;
    ValueConstraint constraint = pair.second;

    // Value must be defined by an operation.
    Operation *op = value.getDefiningOp();
    assert(op && "value must be defined by an operation");
    if (!op) return failure();

    // Operation must be in the region.
    Block *ancestorBlock = region.findAncestorBlockInRegion(*op->getBlock());
    assert(ancestorBlock && "operation must be in the region");
    if (!ancestorBlock) return failure();

    op_results_constraints[op].Insert(value, constraint);
  }

  // Keep a worklist of operations that need their constraints to be updated.
  llvm::SetVector<Operation *> worklist;
  region.walk([&](Operation *op) { worklist.insert(op); });  // process all ops

  while (!worklist.empty()) {
    Operation *op = worklist.pop_back_val();

    // Use results constraints to infer operands constraints.
    const ValuesConstraintSet &results = op_results_constraints[op];
    ValuesConstraintSet operands;

    // Walk through all policies until we find one that matches the operation.
    bool updated = false;
    for (auto &policy : policies.policies()) {
      auto matched =
          policy->MatchAndUpdateConstraints(op, results, operands.Reset());
      if (succeeded(matched)) {
        updated = true;
        break;
      }
    }

    // Signal a failure if could not propagate non-empty constraints on the
    // operation results to the operands.
    if (!updated && !results.Empty()) {
      op->emitError("failed to propagate results constraints");
      return failure();
    }

    // Update results constraints based on inferred operands constraints.
    operands.Walk([&](Value value, ValueConstraint constraint) {
      // Update constraint for a value.
      auto updated = constraints.Insert(value, constraint);
      if (!updated.second) return;

      // Maybe update constaint on the operation result, but do not follow
      // operations that are outside of the `region`.
      Operation *op = value.getDefiningOp();
      if (!op || !region.findAncestorBlockInRegion(*op->getBlock())) return;

      // Add updated operation to the worklist.
      auto inserted = op_results_constraints[op].Insert(value, updated.first);
      if (inserted.second) worklist.insert(op);
    });
  }

  return success();
}

void EmitValueConstraintsRemarks(const ValuesConstraintSet &constraints) {
  constraints.Walk([](Value value, ValueConstraint constraint) {
    for (OpOperand &operand : value.getUses())
      operand.getOwner()->emitRemark(
          llvm::formatv("operand #{0} constrained to: {1}",
                        operand.getOperandNumber(), constraint));
  });
}

void EmitInputsConstraintsRemarks(FuncOp func,
                                  const ValuesConstraintSet &constraints) {
  constraints.Walk([&](Value value, ValueConstraint constraint) {
    if (auto arg = value.dyn_cast<BlockArgument>())
      if (arg.getOwner() == &func.body().front())
        func.emitRemark(llvm::formatv("input #{0} constrained to: {1}",
                                      arg.getArgNumber(), constraint));
  });
}

LogicalResult InferFunctionBodyValuesConstraints(
    FuncOp func, ValuesConstraintSet &constraints) {
  for (unsigned i = 0; i < func.getNumResults(); ++i) {
    auto str = func.getResultAttrOfType<StringAttr>(i, "tf.constraint");
    if (!str) continue;

    ValueConstraint constraint = StringSwitch<ValueConstraint>(str.getValue())
                                     .Case("rank", ValueConstraint::kRank)
                                     .Case("shape", ValueConstraint::kShape)
                                     .Case("value", ValueConstraint::kValue);

    // Propagate constraints through function return operations.
    for (Block &block : func.body()) {
      ReturnOp ret = dyn_cast<ReturnOp>(block.back());
      if (ret) constraints.Insert(ret.getOperand(i), constraint);
    }
  }

  return success();
}

}  // namespace TFDevice
}  // namespace mlir
