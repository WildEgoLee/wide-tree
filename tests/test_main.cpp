#include "widetree/widetree.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failed = 0;

void fail(int line, const char* expr) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", "tests/test_main.cpp", line, expr);
  ++g_failed;
}

template <typename A, typename B>
void failEq(int line, const char* expr, A a, B b) {
  std::fprintf(stderr, "FAIL %s:%d: %s (%llu != %llu)\n", "tests/test_main.cpp", line, expr,
               static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
  ++g_failed;
}

#define CHECK(cond) \
  do {              \
    if (!(cond)) {  \
      fail(__LINE__, #cond); \
    }               \
  } while (0)

#define CHECK_EQ(a, b)                              \
  do {                                              \
    const auto _va = (a);                           \
    const auto _vb = (b);                           \
    if (!(_va == _vb)) {                            \
      failEq(__LINE__, #a " == " #b, _va, _vb);     \
    }                                               \
  } while (0)

using namespace widetree;

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok:
      return "Ok";
    case Status::NoChange:
      return "NoChange";
    case Status::NoSnapshot:
      return "NoSnapshot";
    case Status::InvalidNode:
      return "InvalidNode";
    case Status::InvalidRowRef:
      return "InvalidRowRef";
    case Status::StaleRowRef:
      return "StaleRowRef";
    case Status::StaleSnapshot:
      return "StaleSnapshot";
  }
  return "?";
}

void checkStatus(int line, Status got, Status want) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %d: status %s != %s\n", line, statusName(got), statusName(want));
    ++g_failed;
  }
}

void checkOk(const TreeView& view, int line) {
  if (const char* error = view.checkInvariants()) {
    std::fprintf(stderr, "FAIL %d: invariant: %s\n", line, error);
    ++g_failed;
  }
}

std::shared_ptr<const TreeSnapshot> mustBuild(TreeBuilder& builder, int line) {
  BuildResult built = builder.build();
  if (!built) {
    std::fprintf(stderr, "FAIL %d: build %s (%s)\n", line, built.error.code,
                 built.error.detail.c_str());
    ++g_failed;
  }
  return built.snapshot;
}

std::vector<NodeId> oracleRows(const TreeSnapshot& snap, const TreeView& view) {
  std::vector<NodeId> rows;
  const auto walk = [&](auto&& self, NodeId id) -> void {
    rows.push_back(id);
    if (!view.isExpanded(id)) {
      return;
    }
    const NodeTopology& topo = snap.topology(id);
    for (std::uint32_t i = 0; i < topo.childCount; ++i) {
      self(self, snap.child(id, i));
    }
  };
  if (!view.hideRoot()) {
    walk(walk, snap.root());
  } else {
    const NodeTopology& topo = snap.topology(snap.root());
    for (std::uint32_t i = 0; i < topo.childCount; ++i) {
      walk(walk, snap.child(snap.root(), i));
    }
  }
  return rows;
}

void expectRows(const TreeView& view, const TreeSnapshot& snap, int line) {
  const std::vector<NodeId> want = oracleRows(snap, view);
  CHECK_EQ(view.rowCount(), static_cast<RowCount>(want.size()));
  std::vector<VisibleRow> got(want.size());
  const std::size_t n = view.getVisibleRows(0, got);
  if (n != want.size()) {
    failEq(line, "visible row fill", n, want.size());
    return;
  }
  for (std::size_t i = 0; i < want.size(); ++i) {
    if (got[i].id != want[i] || got[i].row != i) {
      std::fprintf(stderr, "FAIL %d: row %zu id %u != %u\n", line, i, got[i].id, want[i]);
      ++g_failed;
      return;
    }
    if (got[i].expanded != view.isExpanded(want[i])) {
      fail(line, "expanded flag");
      return;
    }
    const auto located = view.rowOf(want[i]);
    if (!located || *located != i) {
      fail(line, "rowOf");
      return;
    }
  }
}

void testBuilderRejects() {
  {
    TreeBuilder builder(0);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "bad_node_count");
  }
  {
    TreeBuilder builder(2);
    builder.addEdge(0, 1);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "missing_root");
  }
  {
    TreeBuilder builder(3);
    builder.setRoot(0);
    builder.setRoot(1);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "multiple_roots");
  }
  {
    TreeBuilder builder(2);
    builder.setRoot(0);
    builder.addEdge(0, 0);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "self_loop");
  }
  {
    TreeBuilder builder(3);
    builder.setRoot(0);
    builder.addEdge(0, 1);
    builder.addEdge(0, 1);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "multiple_parents");
  }
  {
    TreeBuilder builder(3);
    builder.setRoot(0);
    builder.addEdge(1, 2);
    builder.addEdge(2, 1);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "cycle");
  }
  {
    TreeBuilder builder(3);
    builder.setRoot(0);
    builder.addEdge(0, 1);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "unreachable");
  }
  {
    TreeBuilder builder(2);
    builder.setRoot(0);
    builder.addEdge(0, 1);
    builder.addEdge(1, 0);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "root_has_parent");
  }
  {
    TreeBuilder builder(2);
    builder.setRoot(0);
    builder.addEdge(0, 5);
    BuildResult built = builder.build();
    CHECK(!built);
    CHECK(std::string(built.error.code) == "bad_child");
  }
}

void testSmallTreeSemantics() {
  // 0
  //  ├─ 1
  //  ├─ 2
  //  │   ├─ 4
  //  │   │   └─ 6
  //  │   └─ 5
  //  └─ 3
  TreeBuilder builder(7);
  builder.setRoot(0);
  builder.setLabel(0, "root");
  builder.setLabel(2, "mid");
  builder.setPayload(6, 42);
  builder.addEdge(0, 1);
  builder.addEdge(0, 2);
  builder.addEdge(0, 3);
  builder.addEdge(2, 4);
  builder.addEdge(2, 5);
  builder.addEdge(4, 6);
  const auto snap = mustBuild(builder, __LINE__);
  if (!snap) {
    return;
  }
  CHECK_EQ(snap->child(0, 1), 2u);
  CHECK(snap->label(2) == "mid");
  CHECK_EQ(snap->payload(6), 42ull);
  CHECK_EQ(sizeof(NodeTopology), 16ull);
  CHECK_EQ(sizeof(NodeInfo), 16ull);

  TreeView view;
  ViewResult reset = view.reset(snap);
  checkStatus(__LINE__, reset.status, Status::Ok);
  CHECK(reset.change.kind == ChangeKind::Reset);
  CHECK_EQ(view.rowCount(), 1ull);
  checkOk(view, __LINE__);
  expectRows(view, *snap, __LINE__);

  const std::uint64_t rev0 = view.revision();
  RowRef rootRef = *view.rowRef(0);
  ViewResult leaf = view.expandAt(rootRef);
  // root is not a leaf; expand it.
  checkStatus(__LINE__, leaf.status, Status::Ok);
  CHECK(leaf.change.kind == ChangeKind::Splice);
  CHECK_EQ(leaf.change.splice.first, 1ull);
  CHECK_EQ(leaf.change.splice.removed, 0ull);
  CHECK_EQ(leaf.change.splice.inserted, 3ull);
  CHECK_EQ(leaf.change.iconRows.size(), 1ull);
  CHECK_EQ(view.revision(), rev0 + 1);
  expectRows(view, *snap, __LINE__);

  ViewResult again = view.expandAt(*view.rowRef(0));
  checkStatus(__LINE__, again.status, Status::NoChange);
  CHECK_EQ(view.revision(), rev0 + 1);

  // Leaf expand does not bump the revision.
  RowRef leafRef = *view.rowRef(1);
  CHECK_EQ(leafRef.node, 1u);
  const std::uint64_t revLeaf = view.revision();
  ViewResult leafExpand = view.expandAt(leafRef);
  checkStatus(__LINE__, leafExpand.status, Status::NoChange);
  CHECK_EQ(view.revision(), revLeaf);

  // Expand the middle node, then its child that still has a descendant.
  RowRef midRef = *view.rowRef(2);
  CHECK_EQ(midRef.node, 2u);
  checkStatus(__LINE__, view.expandAt(midRef).status, Status::Ok);
  RowRef deepRef = *view.rowRef(3);
  CHECK_EQ(deepRef.node, 4u);
  checkStatus(__LINE__, view.expandAt(deepRef).status, Status::Ok);
  expectRows(view, *snap, __LINE__);
  checkOk(view, __LINE__);

  const auto segsOpen = view.segments();
  // display [0,1), parent [0,2) includes node 2, node2 [0,1) includes node 4,
  // node4 [0,1), node2 [1,2), parent [2,3).
  CHECK_EQ(segsOpen.size(), 6ull);
  CHECK(segsOpen[1].parent == 0 && segsOpen[1].begin == 0 && segsOpen[1].end == 2);
  CHECK(segsOpen[2].parent == 2 && segsOpen[2].begin == 0 && segsOpen[2].end == 1);
  CHECK(segsOpen[4].parent == 2 && segsOpen[4].begin == 1 && segsOpen[4].end == 2);
  CHECK(segsOpen[5].parent == 0 && segsOpen[5].begin == 2 && segsOpen[5].end == 3);

  // Collapse the middle node. Its two child segments must disappear and the
  // parent pieces [0,2) + [2,3) must merge. Descendant preferences stay.
  RowRef midNow = *view.rowRef(*view.rowOf(2));
  const std::uint64_t revBeforeCollapse = view.revision();
  ViewResult collapsed = view.collapseAt(midNow);
  checkStatus(__LINE__, collapsed.status, Status::Ok);
  CHECK_EQ(collapsed.change.splice.removed, 3ull);
  CHECK_EQ(collapsed.change.splice.inserted, 0ull);
  CHECK(view.isExpanded(4));
  CHECK(!view.isVisible(4));
  CHECK(!view.isExpanded(2));
  CHECK(view.isVisible(2));
  const auto segsMerged = view.segments();
  CHECK_EQ(segsMerged.size(), 2ull);
  CHECK(segsMerged[1].parent == 0 && segsMerged[1].begin == 0 && segsMerged[1].end == 3);
  checkOk(view, __LINE__);

  ViewResult collapseAgain = view.collapseAt(*view.rowRef(*view.rowOf(2)));
  checkStatus(__LINE__, collapseAgain.status, Status::NoChange);
  CHECK_EQ(view.revision(), revBeforeCollapse + 1);

  // Re-expand restores the descendant preference.
  checkStatus(__LINE__, view.expandAt(*view.rowRef(*view.rowOf(2))).status, Status::Ok);
  CHECK(view.isExpanded(4));
  CHECK(view.isVisible(6));
  CHECK_EQ(view.segments().size(), segsOpen.size());
  expectRows(view, *snap, __LINE__);

  // Collapse the root, then reveal the deep leaf: ancestors open, target does not.
  checkStatus(__LINE__, view.collapseAt(*view.rowRef(0)).status, Status::Ok);
  CHECK(!view.isVisible(6));
  CHECK(view.isExpanded(2));
  CHECK(view.isExpanded(4));
  const std::uint64_t revReveal = view.revision();
  ViewResult revealed = view.reveal(6);
  checkStatus(__LINE__, revealed.status, Status::Ok);
  CHECK(revealed.change.kind == ChangeKind::Splice);
  CHECK_EQ(revealed.change.splice.removed, 0ull);
  CHECK(revealed.change.splice.inserted > 0);
  CHECK(!view.isExpanded(6));
  CHECK(view.isVisible(6));
  CHECK(view.isExpanded(0));
  CHECK_EQ(view.revision(), revReveal + 1);
  ViewResult revealAgain = view.reveal(6);
  checkStatus(__LINE__, revealAgain.status, Status::NoChange);
  CHECK_EQ(view.revision(), revReveal + 1);
  expectRows(view, *snap, __LINE__);

  // Stale RowRef, mismatched node id, stale snapshot.
  RowRef live = *view.rowRef(0);
  checkStatus(__LINE__, view.expandAt(live).status, Status::NoChange);  // already open
  RowRef stale = live;
  checkStatus(__LINE__, view.collapseAt(live).status, Status::Ok);
  checkStatus(__LINE__, view.expandAt(stale).status, Status::StaleRowRef);
  CHECK_EQ(view.revision(), revReveal + 2);

  checkStatus(__LINE__, view.expandAt(*view.rowRef(0)).status, Status::Ok);
  RowRef mismatch = *view.rowRef(0);
  mismatch.node = 3;
  checkStatus(__LINE__, view.collapseAt(mismatch).status, Status::InvalidRowRef);

  TreeBuilder otherBuilder(1);
  otherBuilder.setRoot(0);
  otherBuilder.setLabel(0, "solo");
  const auto other = mustBuild(otherBuilder, __LINE__);
  RowRef foreign = *view.rowRef(0);
  TreeView replacement;
  replacement.reset(other);
  // Same view, different snapshot.
  checkStatus(__LINE__, view.reset(other).status, Status::Ok);
  foreign.revision = view.revision();
  checkStatus(__LINE__, view.expandAt(foreign).status, Status::StaleSnapshot);

  // Independent views over one snapshot.
  TreeView a;
  TreeView b;
  a.reset(snap);
  b.reset(snap);
  checkStatus(__LINE__, a.expandAt(*a.rowRef(0)).status, Status::Ok);
  CHECK_EQ(b.rowCount(), 1ull);
  CHECK(a.isExpanded(0));
  CHECK(!b.isExpanded(0));
}

void testHideRootDoesNotForceRootOpen() {
  TreeBuilder builder(4);
  builder.setRoot(0);
  builder.addEdge(0, 1);
  builder.addEdge(0, 2);
  builder.addChildRange(2, 3, 1);
  const auto snap = mustBuild(builder, __LINE__);
  if (!snap) {
    return;
  }
  TreeView view;
  view.reset(snap, ViewConfig{true});
  CHECK(!view.isExpanded(0));
  CHECK(!view.isVisible(0));
  CHECK(view.isVisible(1));
  CHECK_EQ(view.rowCount(), 2ull);
  checkStatus(__LINE__, view.reveal(0).status, Status::InvalidNode);
  checkStatus(__LINE__, view.expandAt(*view.rowRef(1)).status, Status::Ok);
  CHECK(!view.isExpanded(0));
  CHECK(view.isExpanded(2));
  CHECK(view.isVisible(3));
  expectRows(view, *snap, __LINE__);
  checkOk(view, __LINE__);

  checkStatus(__LINE__, view.collapseAt(*view.rowRef(*view.rowOf(2))).status, Status::Ok);
  CHECK(!view.isExpanded(0));
  CHECK(!view.isVisible(3));
  checkStatus(__LINE__, view.expandAt(*view.rowRef(*view.rowOf(2))).status, Status::Ok);
  CHECK(view.isVisible(3));
  CHECK(!view.isExpanded(0));
  CHECK(!view.isExpanded(3));
}

void testRandomAgainstOracle() {
  constexpr std::uint32_t kNodes = 48;
  TreeBuilder builder(kNodes);
  builder.setRoot(0);
  for (std::uint32_t id = 1; id < kNodes; ++id) {
    builder.addEdge((id - 1) / 2, id);
    builder.setLabel(id, "n");
  }
  const auto snap = mustBuild(builder, __LINE__);
  if (!snap) {
    return;
  }
  for (int hide = 0; hide < 2; ++hide) {
    TreeView view;
    view.reset(snap, ViewConfig{hide == 1});
    std::uint32_t rng = 0xC0FFEEu + static_cast<std::uint32_t>(hide) * 17u;
    auto next = [&]() {
      rng = rng * 1664525u + 1013904223u;
      return rng;
    };
    for (int step = 0; step < 400; ++step) {
      const std::uint32_t roll = next() % 10u;
      if (roll < 4 && view.rowCount() > 0) {
        const RowIndex row = next() % view.rowCount();
        const RowRef ref = *view.rowRef(row);
        const Status status = view.expandAt(ref).status;
        CHECK(status == Status::Ok || status == Status::NoChange);
      } else if (roll < 8 && view.rowCount() > 0) {
        const RowIndex row = next() % view.rowCount();
        const RowRef ref = *view.rowRef(row);
        const Status status = view.collapseAt(ref).status;
        CHECK(status == Status::Ok || status == Status::NoChange);
      } else {
        const NodeId id = next() % kNodes;
        const Status status = view.reveal(id).status;
        CHECK(status == Status::Ok || status == Status::NoChange || status == Status::InvalidNode);
      }
      checkOk(view, __LINE__);
      expectRows(view, *snap, __LINE__);
      if (g_failed > 20) {
        return;
      }
    }
  }
}

void testDeepChainUsesHeapStack() {
  constexpr std::uint32_t kNodes = 20000;
  TreeBuilder builder(kNodes);
  builder.setRoot(0);
  for (std::uint32_t id = 1; id < kNodes; ++id) {
    builder.addEdge(id - 1, id);
  }
  const auto snap = mustBuild(builder, __LINE__);
  if (!snap) {
    return;
  }
  TreeView view;
  view.reset(snap);
  const ViewResult revealed = view.reveal(kNodes - 1);
  checkStatus(__LINE__, revealed.status, Status::Ok);
  CHECK_EQ(view.rowCount(), static_cast<RowCount>(kNodes));
  CHECK(!view.isExpanded(kNodes - 1));
  CHECK(view.isExpanded(kNodes - 2));
  CHECK(view.isVisible(kNodes - 1));
  checkOk(view, __LINE__);
  VisibleRow tail[1];
  CHECK_EQ(view.getVisibleRows(kNodes - 1, tail), 1ull);
  CHECK_EQ(tail[0].id, kNodes - 1);
  CHECK_EQ(tail[0].depth, kNodes - 1);
}

bool segmentIs(const SegmentInfo& segment, NodeId parent, std::uint32_t begin, std::uint32_t end,
               std::uint32_t depth) {
  return segment.parent == parent && segment.begin == begin && segment.end == end &&
         segment.depth == depth;
}

void testMillionChildAcceptance() {
  constexpr std::uint32_t kFanout = 1000000;
  constexpr NodeId kRoot = 0;
  constexpr NodeId kB = 11;        // ordinal 10
  constexpr NodeId kC = 800001;    // ordinal 800000
  constexpr NodeId kG = 1000001;   // first child of B
  constexpr NodeId kG0 = 1000002;
  constexpr NodeId kG1 = 1000003;
  constexpr NodeId kB1 = 1000004;
  constexpr NodeId kB2 = 1000005;
  constexpr NodeId kB3 = 1000006;
  constexpr NodeId kC0 = 1000007;
  constexpr NodeId kC1 = 1000008;
  constexpr NodeId kC2 = 1000009;
  constexpr std::uint32_t kNodes = 1000010;

  TreeBuilder builder(kNodes);
  builder.setRoot(kRoot);
  builder.setLabel(kRoot, "A");
  builder.setLabel(kB, "B");
  builder.setLabel(kC, "C");
  builder.setLabel(kG, "G");
  builder.addChildRange(kRoot, 1, kFanout);
  builder.addEdge(kB, kG);
  builder.addEdge(kB, kB1);
  builder.addEdge(kB, kB2);
  builder.addEdge(kB, kB3);
  builder.addEdge(kG, kG0);
  builder.addEdge(kG, kG1);
  builder.addEdge(kC, kC0);
  builder.addEdge(kC, kC1);
  builder.addEdge(kC, kC2);

  const auto builtAt = std::chrono::steady_clock::now();
  const auto snap = mustBuild(builder, __LINE__);
  if (!snap) {
    return;
  }
  const auto builtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - builtAt)
                           .count();
  CHECK_EQ(snap->nodeCount(), kNodes);
  CHECK_EQ(snap->topology(kRoot).childCount, kFanout);
  CHECK_EQ(snap->child(kRoot, 10), kB);
  CHECK_EQ(snap->child(kRoot, 800000), kC);
  const MemoryReport before = [&] {
    TreeView probe;
    probe.reset(snap);
    return probe.memory();
  }();
  std::printf("snapshot resident %.2f MB, build %lld ms\n",
              static_cast<double>(before.snapshotBytes) / (1024.0 * 1024.0),
              static_cast<long long>(builtMs));
  CHECK(before.snapshotBytes > 30ull * 1024ull * 1024ull);
  CHECK(before.snapshotBytes < 48ull * 1024ull * 1024ull);

  TreeView view;
  view.reset(snap);
  CHECK_EQ(view.rowCount(), 1ull);
  CHECK(!view.isExpanded(kRoot));

  const auto expandRoot = view.expandAt(*view.rowRef(0));
  checkStatus(__LINE__, expandRoot.status, Status::Ok);
  CHECK_EQ(expandRoot.change.splice.first, 1ull);
  CHECK_EQ(expandRoot.change.splice.removed, 0ull);
  CHECK_EQ(expandRoot.change.splice.inserted, static_cast<RowCount>(kFanout));
  CHECK_EQ(view.rowCount(), static_cast<RowCount>(kFanout) + 1ull);
  CHECK_EQ(view.segments().size(), 2ull);
  checkOk(view, __LINE__);

  VisibleRow screen[100];
  CHECK_EQ(view.getVisibleRows(0, screen), 100ull);
  CHECK_EQ(screen[0].id, kRoot);
  CHECK_EQ(screen[0].depth, 0u);
  CHECK(screen[0].label == "A");
  CHECK_EQ(screen[1].id, 1u);
  CHECK_EQ(screen[11].id, kB);
  CHECK(screen[11].label == "B");
  CHECK_EQ(screen[11].ordinalInParent, 10u);

  VisibleRow around[5];
  CHECK_EQ(view.getVisibleRows(800000, around), 5ull);
  // Row 0 is A, so row 800000 is ordinal 799999 (id 800000). C is the next row.
  CHECK_EQ(around[0].id, 800000u);
  CHECK_EQ(around[1].id, kC);
  CHECK_EQ(*view.rowOf(kC), 800001ull);

  // Textbook restore shape: expand B and C only. G stays collapsed.
  const RowRef bRef = *view.rowRef(*view.rowOf(kB));
  const ViewResult expandB = view.expandAt(bRef);
  checkStatus(__LINE__, expandB.status, Status::Ok);
  CHECK_EQ(expandB.change.splice.first, bRef.row + 1);
  CHECK_EQ(expandB.change.splice.inserted, 4ull);
  CHECK_EQ(expandB.change.iconRows[0], bRef.row);

  const RowRef cRef = *view.rowRef(*view.rowOf(kC));
  const ViewResult expandC = view.expandAt(cRef);
  checkStatus(__LINE__, expandC.status, Status::Ok);
  CHECK_EQ(expandC.change.splice.inserted, 3ull);

  const auto split = view.segments();
  // Display row for A, then the five pieces from the spec:
  //   [0, 11) includes B, B's children, [11, 800001) includes C,
  //   C's children, [800001, 1000000).
  CHECK_EQ(split.size(), 6ull);
  CHECK(segmentIs(split[0], kDisplayRoot, 0, 1, 0));
  CHECK(segmentIs(split[1], kRoot, 0, 11, 1));
  CHECK(segmentIs(split[2], kB, 0, 4, 2));
  CHECK(segmentIs(split[3], kRoot, 11, 800001, 1));
  CHECK(segmentIs(split[4], kC, 0, 3, 2));
  CHECK(segmentIs(split[5], kRoot, 800001, kFanout, 1));
  checkOk(view, __LINE__);

  // Expanding G splits B's segment. G's preference must survive a later collapse of A.
  const RowRef gRef = *view.rowRef(*view.rowOf(kG));
  checkStatus(__LINE__, view.expandAt(gRef).status, Status::Ok);
  CHECK_EQ(view.segments().size(), 8ull);
  const auto restoredShape = view.segments();

  // B sits in the middle of A's child run. Collapsing B drops its descendant
  // segments and merges the two A-segments on either side. G stays preferred.
  const RowRef bOpen = *view.rowRef(*view.rowOf(kB));
  const ViewResult collapseB = view.collapseAt(bOpen);
  checkStatus(__LINE__, collapseB.status, Status::Ok);
  CHECK_EQ(collapseB.change.splice.removed, 6ull);  // 4 children + G's 2
  CHECK_EQ(collapseB.change.splice.inserted, 0ull);
  CHECK(!view.isExpanded(kB));
  CHECK(view.isExpanded(kG));
  CHECK(!view.isVisible(kG));
  CHECK(view.isExpanded(kC));
  const auto merged = view.segments();
  CHECK_EQ(merged.size(), 4ull);
  CHECK(segmentIs(merged[0], kDisplayRoot, 0, 1, 0));
  CHECK(segmentIs(merged[1], kRoot, 0, 800001, 1));
  CHECK(segmentIs(merged[2], kC, 0, 3, 2));
  CHECK(segmentIs(merged[3], kRoot, 800001, kFanout, 1));

  checkStatus(__LINE__, view.expandAt(*view.rowRef(*view.rowOf(kB))).status, Status::Ok);
  CHECK(view.isExpanded(kG));
  CHECK(view.isVisible(kG0));
  CHECK_EQ(view.segments().size(), restoredShape.size());

  const MemoryReport hot = view.memory();
  const std::uint32_t segmentsBefore = hot.segmentCount;
  const std::uint32_t expandedBefore = hot.expandedCount;
  const std::uint32_t parentsBefore = hot.indexedParentCount;
  const RowRef staleRoot = *view.rowRef(0);
  CHECK_EQ(staleRoot.node, kRoot);

  const auto repeatAt = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) {
    const ViewResult down = view.collapseAt(*view.rowRef(0));
    checkStatus(__LINE__, down.status, Status::Ok);
    CHECK_EQ(view.rowCount(), 1ull);
    CHECK(view.isExpanded(kB));
    CHECK(view.isExpanded(kG));
    CHECK(view.isExpanded(kC));
    CHECK(!view.isVisible(kB));
    const ViewResult up = view.expandAt(*view.rowRef(0));
    checkStatus(__LINE__, up.status, Status::Ok);
    CHECK_EQ(up.change.splice.removed, 0ull);
    CHECK(up.change.splice.inserted > kFanout);
  }
  const auto repeatUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - repeatAt)
                             .count();
  const MemoryReport after = view.memory();
  std::printf("100 collapse/expand cycles of the million-child root: %lld us, segments %u\n",
              static_cast<long long>(repeatUs), after.segmentCount);
  CHECK_EQ(after.segmentCount, segmentsBefore);
  CHECK_EQ(after.expandedCount, expandedBefore);
  CHECK_EQ(after.indexedParentCount, parentsBefore);
  CHECK_EQ(view.segments().size(), restoredShape.size());
  CHECK(view.isVisible(kG1));
  CHECK(view.isVisible(kC2));
  checkOk(view, __LINE__);
  CHECK(repeatUs < 2000000);

  checkStatus(__LINE__, view.collapseAt(staleRoot).status, Status::StaleRowRef);
  RowRef wrongNode = *view.rowRef(0);
  wrongNode.node = kB;
  checkStatus(__LINE__, view.collapseAt(wrongNode).status, Status::InvalidRowRef);
  const std::uint64_t rev = view.revision();
  checkStatus(__LINE__, view.expandAt(*view.rowRef(0)).status, Status::NoChange);
  CHECK_EQ(view.revision(), rev);

  // Hidden root: the display sequence IS A's children, and A's bit stays false.
  TreeView hidden;
  hidden.reset(snap, ViewConfig{true});
  CHECK(!hidden.isExpanded(kRoot));
  CHECK_EQ(hidden.rowCount(), static_cast<RowCount>(kFanout));
  CHECK_EQ(hidden.segments().size(), 1ull);
  CHECK(segmentIs(hidden.segments()[0], kDisplayRoot, 0, kFanout, 0));
  VisibleRow hiddenScreen[1];
  CHECK_EQ(hidden.getVisibleRows(800000, hiddenScreen), 1ull);
  CHECK_EQ(hiddenScreen[0].id, kC);
  CHECK_EQ(hiddenScreen[0].depth, 0u);

  checkStatus(__LINE__, hidden.expandAt(*hidden.rowRef(10)).status, Status::Ok);
  checkStatus(__LINE__, hidden.expandAt(*hidden.rowRef(*hidden.rowOf(kC))).status, Status::Ok);
  CHECK(!hidden.isExpanded(kRoot));
  const auto hiddenSplit = hidden.segments();
  CHECK_EQ(hiddenSplit.size(), 5ull);
  CHECK(segmentIs(hiddenSplit[0], kDisplayRoot, 0, 11, 0));
  CHECK(segmentIs(hiddenSplit[1], kB, 0, 4, 1));
  CHECK(segmentIs(hiddenSplit[2], kDisplayRoot, 11, 800001, 0));
  CHECK(segmentIs(hiddenSplit[3], kC, 0, 3, 1));
  CHECK(segmentIs(hiddenSplit[4], kDisplayRoot, 800001, kFanout, 0));
  checkOk(hidden, __LINE__);

  // Collapsing is impossible for the hidden root; collapsing nothing else,
  // fold B and the two display pieces around it must merge.
  checkStatus(__LINE__, hidden.collapseAt(*hidden.rowRef(*hidden.rowOf(kB))).status, Status::Ok);
  const auto hiddenMerged = hidden.segments();
  CHECK_EQ(hiddenMerged.size(), 3ull);
  CHECK(segmentIs(hiddenMerged[0], kDisplayRoot, 0, 800001, 0));
  CHECK(segmentIs(hiddenMerged[1], kC, 0, 3, 1));
  CHECK(segmentIs(hiddenMerged[2], kDisplayRoot, 800001, kFanout, 0));
  CHECK(hidden.isExpanded(kG) == false);
  CHECK(!hidden.isExpanded(kRoot));
}

// Parent ids are not insertion order: the root id sits between its children,
// and expanded ordinals land at the front, middle, and back of a span.
// Collapsing then deletes a span that is not the last one.
void testPackedIndexMiddleEdits() {
  // 5
  //  ├─ 1
  //  │   └─ 2
  //  │       └─ 0
  //  ├─ 8
  //  │   └─ 4
  //  │       └─ 9
  //  └─ 3
  //      └─ 6
  //          └─ 7
  constexpr NodeId kRoot = 5;
  TreeBuilder builder(10);
  builder.setRoot(kRoot);
  builder.addEdge(kRoot, 1);
  builder.addEdge(kRoot, 8);
  builder.addEdge(kRoot, 3);
  builder.addEdge(1, 2);
  builder.addEdge(2, 0);
  builder.addEdge(8, 4);
  builder.addEdge(4, 9);
  builder.addEdge(3, 6);
  builder.addEdge(6, 7);
  const auto snap = mustBuild(builder, __LINE__);
  if (!snap) {
    return;
  }

  for (int hide = 0; hide < 2; ++hide) {
    TreeView view;
    view.reset(snap, ViewConfig{hide == 1});
    const NodeId order[] = {8, 4, 1, 2, 3, 6};
    for (NodeId id : order) {
      if (!view.isVisible(id)) {
        checkStatus(__LINE__, view.reveal(id).status, Status::Ok);
      }
      const auto row = view.rowOf(id);
      CHECK(row.has_value());
      if (!row) {
        return;
      }
      checkStatus(__LINE__, view.expandAt(*view.rowRef(*row)).status, Status::Ok);
      checkOk(view, __LINE__);
    }
    expectRows(view, *snap, __LINE__);
    CHECK(view.memory().indexedParentCount >= 4u);

    // Drop the first parent span (node 2 is the only expanded child of 1).
    checkStatus(__LINE__, view.collapseAt(*view.rowRef(*view.rowOf(2))).status, Status::Ok);
    CHECK(view.isExpanded(0) == false);
    CHECK(!view.isVisible(0));
    CHECK(view.isExpanded(1));
    checkOk(view, __LINE__);
    expectRows(view, *snap, __LINE__);

    // Middle ordinal under the root: 8 sits between 1 and 3. Its descendant
    // preference (4) stays, so 4's span outlives 8's visibility.
    const auto parentsBefore = view.memory().indexedParentCount;
    checkStatus(__LINE__, view.collapseAt(*view.rowRef(*view.rowOf(8))).status, Status::Ok);
    CHECK(!view.isExpanded(8));
    CHECK(view.isExpanded(4));
    CHECK(!view.isVisible(4));
    CHECK(!view.isVisible(9));
    checkOk(view, __LINE__);
    expectRows(view, *snap, __LINE__);

    checkStatus(__LINE__, view.expandAt(*view.rowRef(*view.rowOf(8))).status, Status::Ok);
    CHECK(view.isExpanded(4));
    CHECK(view.isVisible(9));
    CHECK_EQ(view.memory().indexedParentCount, parentsBefore);
    checkOk(view, __LINE__);
    expectRows(view, *snap, __LINE__);

    // Collapse the root side and reveal the deep leaf. Ancestors open, 9 does not.
    if (hide == 0) {
      checkStatus(__LINE__, view.collapseAt(*view.rowRef(0)).status, Status::Ok);
    } else {
      // Hide-root has no root row. Fold the depth-0 parents only; doing it
      // from a snapshot of ids avoids collapsing a child the parent just hid.
      const NodeId top[] = {1, 8, 3};
      for (NodeId id : top) {
        if (!view.isVisible(id) || !view.isExpanded(id)) {
          continue;
        }
        checkStatus(__LINE__, view.collapseAt(*view.rowRef(*view.rowOf(id))).status, Status::Ok);
      }
    }
    CHECK(!view.isVisible(9));
    CHECK(view.isExpanded(4));
    checkStatus(__LINE__, view.reveal(9).status, Status::Ok);
    CHECK(view.isVisible(9));
    CHECK(!view.isExpanded(9));
    CHECK(view.isExpanded(4));
    CHECK(view.isExpanded(8));
    checkOk(view, __LINE__);
    expectRows(view, *snap, __LINE__);
  }
}

}  // namespace

int main() {
  testBuilderRejects();
  testSmallTreeSemantics();
  testHideRootDoesNotForceRootOpen();
  testRandomAgainstOracle();
  testDeepChainUsesHeapStack();
  testPackedIndexMiddleEdits();
  testMillionChildAcceptance();
  if (g_failed != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failed);
    return 1;
  }
  std::puts("all checks passed");
  return 0;
}
