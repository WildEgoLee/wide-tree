#pragma once

#include "widetree/snapshot.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace widetree {

struct BuildResult {
  std::shared_ptr<const TreeSnapshot> snapshot;
  BuildError error{};
  explicit operator bool() const noexcept { return static_cast<bool>(snapshot); }
};

// Single-shot builder. v1 publishes one root. A future forest is the same
// snapshot shape with several roots hung off the display root; it is rejected
// here so rowOf stays single-valued.
//
// Edges are recorded in call order. Per parent, that order is the permanent
// child order. Input is counted, prefixed, then filled once — nodes do not
// each own a vector.
class TreeBuilder {
 public:
  explicit TreeBuilder(std::uint32_t nodeCount);

  void setRoot(NodeId root);
  void setLabel(NodeId id, std::string_view label);
  void setPayload(NodeId id, std::uint64_t payload);
  void addEdge(NodeId parent, NodeId child);
  // Children are the contiguous ids [first, first + count).
  void addChildRange(NodeId parent, NodeId firstChild, std::uint32_t count);

  BuildResult build();

 private:
  struct FillOp {
    NodeId parent = kInvalidNode;
    NodeId firstChild = kInvalidNode;
    std::uint32_t count = 0;
  };

  bool fail(const char* code, std::string detail);
  bool contains(NodeId id) const noexcept { return id < nodeCount_; }

  std::uint32_t nodeCount_ = 0;
  bool hasRoot_ = false;
  NodeId root_ = kInvalidNode;
  bool failed_ = false;
  BuildError error_{};
  std::vector<std::uint32_t> childCount_;
  std::vector<NodeInfo> info_;
  std::string labels_;
  std::vector<FillOp> ops_;
};

}  // namespace widetree
