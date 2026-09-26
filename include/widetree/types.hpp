#pragma once

// Visible-row projection for an ultra-wide tree.
//
// v1 freezes three constraints: immutable snapshots, fixed child order, and
// fixed row height. Collapsing a node clears only that node's expanded
// preference; descendant preferences stay. Sorting, filtering, and async
// paging are not part of the core — callers that need them should rebuild or
// return ChangeKind::Reset rather than teaching the projection about them.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace widetree {

using NodeId = std::uint32_t;
using RowIndex = std::uint64_t;
using RowCount = std::uint64_t;

// Not a real NodeId. Display-root segments use kDisplayRoot as their parent.
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;
inline constexpr NodeId kDisplayRoot = 0xFFFFFFFEu;
// Builder rejects counts that would collide with the sentinels above.
inline constexpr NodeId kMaxNodeCount = 0xFFFFFFFDu;

// v1 child sequences are exactly the snapshot order. The enumerator exists so
// a later sort/filter pass has a named boundary instead of a silent fork
// inside TreeView.
enum class ChildOrderPolicy : std::uint8_t { Fixed = 0 };

enum class Status : std::uint8_t {
  Ok = 0,
  NoChange,
  NoSnapshot,
  InvalidNode,
  InvalidRowRef,
  StaleRowRef,
  StaleSnapshot,
};

enum class ChangeKind : std::uint8_t {
  NoChange = 0,
  Splice,
  Reset,
};

struct RowSplice {
  RowIndex first = 0;
  RowCount removed = 0;
  RowCount inserted = 0;
};

// Returned to the caller. TreeView does not invoke user callbacks, so a
// listener cannot re-enter expand/collapse before the commit finishes.
//
// iconRows are previously visible rows whose expand glyph flipped. Inserted
// rows carry their own glyphs; do not assume the splice alone refreshes the
// parent row.
//
// ChangeKind::Reset means every outstanding RowRef is dead. Ignore splice.
struct ProjectionChange {
  std::uint64_t oldRevision = 0;
  std::uint64_t newRevision = 0;
  RowCount oldRowCount = 0;
  RowCount newRowCount = 0;
  ChangeKind kind = ChangeKind::NoChange;
  RowSplice splice{};
  std::vector<RowIndex> iconRows;
};

struct ViewResult {
  Status status = Status::NoSnapshot;
  ProjectionChange change{};
};

// Identifies a visible row. Invalid after the snapshot id or the projection
// revision changes. Hover, selection, and other paint-only state must not
// bump the projection revision.
struct RowRef {
  std::uint64_t snapshotId = 0;
  std::uint64_t revision = 0;
  RowIndex row = 0;
  NodeId node = kInvalidNode;
};

struct VisibleRow {
  RowIndex row = 0;
  NodeId id = kInvalidNode;
  std::uint32_t depth = 0;
  std::uint32_t ordinalInParent = 0;
  std::uint32_t childCount = 0;
  bool expanded = false;
  std::string_view label{};
  std::uint64_t payload = 0;
};

struct SegmentInfo {
  NodeId parent = kInvalidNode;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::uint32_t depth = 0;
  RowIndex firstRow = 0;
};

struct MemoryReport {
  std::uint64_t snapshotBytes = 0;
  std::uint64_t bitmapBytes = 0;
  std::uint64_t indexPayloadBytes = 0;
  std::uint64_t indexNodeEstimateBytes = 0;
  std::uint64_t segmentBytes = 0;
  std::uint64_t prefixBytes = 0;
  std::uint32_t segmentCount = 0;
  std::uint32_t indexedParentCount = 0;
  std::uint32_t expandedCount = 0;
};

struct BuildError {
  const char* code = "";
  std::string detail;
};

}  // namespace widetree
