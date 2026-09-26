#pragma once

#include "widetree/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace widetree {

// 16 bytes. One record per node, indexed by NodeId.
struct NodeTopology {
  NodeId parent = kInvalidNode;
  std::uint32_t rowInParent = 0;
  std::uint32_t childBegin = 0;
  std::uint32_t childCount = 0;
};
static_assert(sizeof(NodeTopology) == 16, "NodeTopology must stay 16 bytes");

// 16 bytes. Labels are slices of the snapshot string pool.
struct NodeInfo {
  std::uint32_t labelBegin = 0;
  std::uint32_t labelLength = 0;
  std::uint64_t payload = 0;
};
static_assert(sizeof(NodeInfo) == 16, "NodeInfo must stay 16 bytes");

// Immutable topology. Published only after the builder has checked id range,
// single parent, self-loops, cycles, reachability, and label bounds.
// A node has one tree position: rowOf(NodeId) has at most one answer.
// This is a tree, not a DAG. Multi-location business objects need their own
// display identity; do not reuse one NodeId for two positions.
class TreeSnapshot {
 public:
  std::uint64_t id() const noexcept { return id_; }
  std::uint32_t nodeCount() const noexcept {
    return static_cast<std::uint32_t>(topo_.size());
  }
  NodeId root() const noexcept { return root_; }

  bool contains(NodeId id) const noexcept { return id < nodeCount(); }

  const NodeTopology& topology(NodeId id) const { return topo_[id]; }
  const NodeInfo& info(NodeId id) const { return info_[id]; }

  std::string_view label(NodeId id) const {
    const NodeInfo& n = info_[id];
    return std::string_view(labels_.data() + n.labelBegin, n.labelLength);
  }

  std::uint64_t payload(NodeId id) const { return info_[id].payload; }

  NodeId child(NodeId parent, std::uint32_t ordinal) const {
    const NodeTopology& t = topo_[parent];
    return children_[t.childBegin + ordinal];
  }

  // Resident bytes of the published arrays (sizes, not capacities).
  std::uint64_t residentBytes() const noexcept {
    return sizeof(TreeSnapshot) + topo_.size() * sizeof(NodeTopology) +
           info_.size() * sizeof(NodeInfo) + children_.size() * sizeof(NodeId) +
           labels_.size();
  }

  TreeSnapshot(const TreeSnapshot&) = delete;
  TreeSnapshot& operator=(const TreeSnapshot&) = delete;
  TreeSnapshot(TreeSnapshot&&) = delete;
  TreeSnapshot& operator=(TreeSnapshot&&) = delete;

 private:
  friend class TreeBuilder;
  TreeSnapshot() = default;

  std::uint64_t id_ = 0;
  NodeId root_ = kInvalidNode;
  std::vector<NodeTopology> topo_;
  std::vector<NodeInfo> info_;
  std::vector<NodeId> children_;
  std::string labels_;
};

}  // namespace widetree
