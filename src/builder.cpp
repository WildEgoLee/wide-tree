#include "widetree/builder.hpp"

#include <atomic>
#include <utility>

namespace widetree {
namespace {

std::atomic<std::uint64_t> g_nextSnapshotId{1};

}  // namespace

TreeBuilder::TreeBuilder(std::uint32_t nodeCount) : nodeCount_(nodeCount) {
  if (nodeCount_ == 0 || nodeCount_ > kMaxNodeCount) {
    fail("bad_node_count", "node count must be in [1, kMaxNodeCount]");
    nodeCount_ = 0;
    return;
  }
  childCount_.assign(nodeCount_, 0);
  info_.assign(nodeCount_, NodeInfo{});
}

void TreeBuilder::setRoot(NodeId root) {
  if (failed_) {
    return;
  }
  if (!contains(root)) {
    fail("bad_root", "root is outside the node id range");
    return;
  }
  if (hasRoot_) {
    fail("multiple_roots", "v1 publishes exactly one root");
    return;
  }
  hasRoot_ = true;
  root_ = root;
}

void TreeBuilder::setLabel(NodeId id, std::string_view label) {
  if (failed_) {
    return;
  }
  if (!contains(id)) {
    fail("bad_node", "setLabel id is outside the node id range");
    return;
  }
  const std::uint64_t begin = labels_.size();
  if (begin > 0xFFFFFFFFu || begin + label.size() > 0xFFFFFFFFu) {
    fail("overflow", "string pool exceeds uint32 offsets");
    return;
  }
  labels_.append(label.data(), label.size());
  info_[id].labelBegin = static_cast<std::uint32_t>(begin);
  info_[id].labelLength = static_cast<std::uint32_t>(label.size());
}

void TreeBuilder::setPayload(NodeId id, std::uint64_t payload) {
  if (failed_) {
    return;
  }
  if (!contains(id)) {
    fail("bad_node", "setPayload id is outside the node id range");
    return;
  }
  info_[id].payload = payload;
}

void TreeBuilder::addEdge(NodeId parent, NodeId child) {
  addChildRange(parent, child, 1);
}

void TreeBuilder::addChildRange(NodeId parent, NodeId firstChild, std::uint32_t count) {
  if (failed_) {
    return;
  }
  if (count == 0) {
    fail("empty_range", "child range count must be non-zero");
    return;
  }
  if (!contains(parent)) {
    fail("bad_parent", "parent is outside the node id range");
    return;
  }
  const std::uint64_t last =
      static_cast<std::uint64_t>(firstChild) + static_cast<std::uint64_t>(count) - 1u;
  if (last >= nodeCount_) {
    fail("bad_child", "child range leaves the node id range");
    return;
  }
  const std::uint64_t sum =
      static_cast<std::uint64_t>(childCount_[parent]) + static_cast<std::uint64_t>(count);
  if (sum > 0xFFFFFFFFu) {
    fail("overflow", "parent child count exceeds uint32");
    return;
  }
  childCount_[parent] = static_cast<std::uint32_t>(sum);
  ops_.push_back(FillOp{parent, firstChild, count});
}

bool TreeBuilder::fail(const char* code, std::string detail) {
  if (!failed_) {
    failed_ = true;
    error_.code = code;
    error_.detail = std::move(detail);
  }
  return false;
}

BuildResult TreeBuilder::build() {
  BuildResult result;
  if (failed_) {
    result.error = error_;
    return result;
  }
  if (!hasRoot_) {
    fail("missing_root", "setRoot was not called");
    result.error = error_;
    return result;
  }

  std::uint64_t edgeTotal = 0;
  for (std::uint32_t i = 0; i < nodeCount_; ++i) {
    edgeTotal += childCount_[i];
    if (edgeTotal > 0xFFFFFFFFu) {
      fail("overflow", "total child slots exceed uint32");
      result.error = error_;
      return result;
    }
  }

  std::vector<NodeTopology> topo(nodeCount_);
  std::vector<NodeId> children(static_cast<std::size_t>(edgeTotal));
  std::vector<std::uint32_t> cursor(nodeCount_);
  std::uint32_t offset = 0;
  for (std::uint32_t i = 0; i < nodeCount_; ++i) {
    topo[i].parent = kInvalidNode;
    topo[i].rowInParent = 0;
    topo[i].childBegin = offset;
    topo[i].childCount = childCount_[i];
    cursor[i] = offset;
    offset += childCount_[i];
  }
  topo[root_].parent = kInvalidNode;

  constexpr std::uint8_t kUnassigned = 0;
  constexpr std::uint8_t kAssigned = 1;
  std::vector<std::uint8_t> assigned(nodeCount_, kUnassigned);
  assigned[root_] = kAssigned;

  for (const FillOp& op : ops_) {
    for (std::uint32_t k = 0; k < op.count; ++k) {
      const NodeId child = op.firstChild + k;
      if (child == op.parent) {
        fail("self_loop", "a node cannot parent itself");
        result.error = error_;
        return result;
      }
      if (child == root_) {
        fail("root_has_parent", "the root cannot be someone's child");
        result.error = error_;
        return result;
      }
      if (assigned[child] == kAssigned) {
        fail("multiple_parents", "a node is owned by more than one parent");
        result.error = error_;
        return result;
      }
      const std::uint32_t slot = cursor[op.parent]++;
      const std::uint32_t ordinal = slot - topo[op.parent].childBegin;
      children[slot] = child;
      topo[child].parent = op.parent;
      topo[child].rowInParent = ordinal;
      assigned[child] = kAssigned;
    }
  }

  for (std::uint32_t i = 0; i < nodeCount_; ++i) {
    if (cursor[i] != topo[i].childBegin + topo[i].childCount) {
      fail("overflow", "child cursor did not land on the planned end");
      result.error = error_;
      return result;
    }
    const std::uint64_t end =
        static_cast<std::uint64_t>(topo[i].childBegin) + topo[i].childCount;
    if (end > children.size()) {
      fail("overflow", "child slice exceeds the children array");
      result.error = error_;
      return result;
    }
  }

  // Parent-pointer cycles, including components the root never reaches.
  // 0 unseen, 1 on the current walk, 2 known to reach the root without a cycle.
  std::vector<std::uint8_t> color(nodeCount_, 0);
  std::vector<NodeId> path;
  path.reserve(64);
  for (std::uint32_t start = 0; start < nodeCount_; ++start) {
    if (color[start] == 2) {
      continue;
    }
    path.clear();
    NodeId x = start;
    bool cycle = false;
    while (true) {
      if (x == root_) {
        color[root_] = 2;
        break;
      }
      if (color[x] == 2) {
        break;
      }
      if (color[x] == 1) {
        cycle = true;
        break;
      }
      if (assigned[x] != kAssigned || topo[x].parent == kInvalidNode ||
          topo[x].parent >= nodeCount_) {
        break;
      }
      color[x] = 1;
      path.push_back(x);
      x = topo[x].parent;
    }
    if (cycle) {
      fail("cycle", "parent pointers contain a cycle");
      result.error = error_;
      return result;
    }
    for (NodeId id : path) {
      color[id] = 2;
    }
  }

  // Reachability is defined by the child arrays, starting at the root.
  std::vector<std::uint8_t> seen(nodeCount_, 0);
  std::vector<NodeId> walk;
  walk.reserve(64);
  walk.push_back(root_);
  seen[root_] = 1;
  std::uint32_t visited = 0;
  while (!walk.empty()) {
    const NodeId id = walk.back();
    walk.pop_back();
    ++visited;
    const NodeTopology& t = topo[id];
    for (std::uint32_t i = 0; i < t.childCount; ++i) {
      const NodeId child = children[t.childBegin + i];
      if (child >= nodeCount_ || seen[child]) {
        fail("cycle", "child walk revisited a node");
        result.error = error_;
        return result;
      }
      seen[child] = 1;
      walk.push_back(child);
    }
  }
  if (visited != nodeCount_) {
    fail("unreachable", "not every node is reachable from the root");
    result.error = error_;
    return result;
  }

  for (std::uint32_t i = 0; i < nodeCount_; ++i) {
    const std::uint64_t end =
        static_cast<std::uint64_t>(info_[i].labelBegin) + info_[i].labelLength;
    if (end > labels_.size()) {
      fail("bad_label", "label slice is outside the string pool");
      result.error = error_;
      return result;
    }
  }

  auto snapshot = std::shared_ptr<TreeSnapshot>(new TreeSnapshot());
  snapshot->id_ = g_nextSnapshotId.fetch_add(1, std::memory_order_relaxed);
  snapshot->root_ = root_;
  snapshot->topo_ = std::move(topo);
  snapshot->info_ = std::move(info_);
  snapshot->children_ = std::move(children);
  snapshot->labels_ = std::move(labels_);
  failed_ = true;
  error_ = BuildError{"already_built", "build() publishes a snapshot once"};
  result.snapshot = std::move(snapshot);
  return result;
}

}  // namespace widetree
