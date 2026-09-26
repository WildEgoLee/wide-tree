#pragma once

#include "widetree/snapshot.hpp"

#include <map>
#include <optional>
#include <span>
#include <vector>

namespace widetree {

struct ViewConfig {
  // When true, the real root is not a row. Its direct children are depth 0.
  // The root's expanded bit is left alone — hiding the root does not force it
  // open. Reset clears preferences; hideRoot is fixed until the next reset.
  bool hideRoot = false;
};

// One view over an immutable snapshot.
//
// Three records stay distinct:
//   * expanded bitmap  — authoritative expand *preference*
//   * sparse index     — acceleration for restoring a parent
//   * segment array    — the rows that are actually visible
//
// isExpanded(id) is the preference. It does not mean id's children are on
// screen. isVisible(id) answers that, and v1 may scan segments (not O(1)).
//
// There is no expand(NodeId). Hidden nodes are reached with reveal(), which
// opens ancestors only and does not expand the target itself.
class TreeView {
 public:
  TreeView() = default;

  ViewResult reset(std::shared_ptr<const TreeSnapshot> snapshot, ViewConfig config = {});

  std::shared_ptr<const TreeSnapshot> snapshot() const { return snap_; }
  std::uint64_t revision() const noexcept { return revision_; }
  RowCount rowCount() const noexcept { return rowCount_; }
  bool hideRoot() const noexcept { return hideRoot_; }

  // Preference, not current visibility of the children.
  bool isExpanded(NodeId id) const noexcept;
  // v1 walks ancestors and may scan segments. Not an O(1) promise.
  bool isVisible(NodeId id) const;
  std::optional<RowIndex> rowOf(NodeId id) const;

  // Writes up to out.size() rows starting at `start`. No intermediate buffer.
  std::size_t getVisibleRows(RowIndex start, std::span<VisibleRow> out) const;

  std::optional<RowRef> rowRef(RowIndex row) const;

  ViewResult expandAt(RowRef ref);
  ViewResult collapseAt(RowRef ref);
  // Opens the ancestor path so `id` is visible. Does not expand `id`.
  ViewResult reveal(NodeId id);

  // Diagnostics. Not on the zero-allocation query path.
  std::vector<SegmentInfo> segments() const;
  MemoryReport memory() const;
  // nullptr when the projection matches the v1 invariants.
  const char* checkInvariants() const;

 private:
  struct Segment {
    NodeId parent = kDisplayRoot;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t depth = 0;
  };

  struct ExpandBitmap {
    void reset(std::uint32_t nodeCount);
    bool test(std::uint32_t i) const noexcept;
    void set(std::uint32_t i) noexcept;
    void clear(std::uint32_t i) noexcept;
    std::uint32_t count() const noexcept { return count_; }
    std::uint64_t bytes() const noexcept {
      return words_.size() * sizeof(std::uint64_t);
    }

    std::vector<std::uint64_t> words_;
    std::uint32_t count_ = 0;
  };

  // Parent node -> child ordinals that currently prefer expanded, in order.
  // Only parents with at least one such child have an entry.
  struct ExpandIndex {
    std::map<NodeId, std::vector<std::uint32_t>> ordinals;
  };

  struct Frame {
    NodeId segmentParent = kDisplayRoot;
    std::uint32_t depth = 0;
    std::uint32_t begin = 0;
    std::uint32_t nextExp = 0;
    std::uint32_t childCount = 0;
    const std::uint32_t* ords = nullptr;
    std::uint32_t ordCount = 0;
  };

  struct Candidate {
    ExpandBitmap bits;
    ExpandIndex index;
    std::vector<Segment> segments;
    std::vector<RowIndex> prefix;
    RowCount rowCount = 0;
  };

  Status checkRef(RowRef ref, NodeId& outId) const;
  NodeId childAt(NodeId segmentParent, std::uint32_t ordinal) const;
  bool locate(RowIndex row, std::size_t& segmentIndex, std::uint32_t& ordinal) const;
  void project(const ExpandBitmap& bits, const ExpandIndex& index,
               std::vector<Segment>& out) const;
  static void buildPrefix(const std::vector<Segment>& segments,
                          std::vector<RowIndex>& prefix, RowCount& rowCount);
  static void insertOrdinal(ExpandIndex& index, NodeId parent, std::uint32_t ordinal);
  static void eraseOrdinal(ExpandIndex& index, NodeId parent, std::uint32_t ordinal);
  Candidate candidateFrom(ExpandBitmap bits, ExpandIndex index) const;
  void commit(Candidate& candidate);
  ViewResult finishSplice(std::uint64_t oldRevision, RowCount oldCount, RowIndex first,
                          RowCount removed, RowCount inserted, RowIndex iconRow);
  ViewResult noChange() const;
  std::uint32_t depthOf(NodeId id) const;

  std::shared_ptr<const TreeSnapshot> snap_;
  bool hideRoot_ = false;
  std::uint64_t revision_ = 0;
  ExpandBitmap bits_{};
  ExpandIndex index_{};
  std::vector<Segment> segments_;
  std::vector<RowIndex> prefix_;
  RowCount rowCount_ = 0;
};

}  // namespace widetree
