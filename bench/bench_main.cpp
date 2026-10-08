#include "widetree/widetree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

std::uint64_t g_sink = 0;

void fail(const char* message) {
  std::fprintf(stderr, "bench failed: %s\n", message);
  std::exit(1);
}

using Clock = std::chrono::steady_clock;

double usSince(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double medianOf(std::vector<double> samples) {
  if (samples.empty()) {
    fail("no samples");
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

std::shared_ptr<const widetree::TreeSnapshot> mustBuild(widetree::TreeBuilder& builder) {
  widetree::BuildResult built = builder.build();
  if (!built) {
    std::fprintf(stderr, "build %s (%s)\n", built.error.code, built.error.detail.c_str());
    std::exit(1);
  }
  return built.snapshot;
}

// Direct children are ids [1, fanout]. `splits` of them, at ordinals i*(fanout/splits),
// each own one leaf. Reachable expanded records after opening the root and those
// children equal splits+1, and the segment count is exactly twice that.
struct Wide {
  std::shared_ptr<const widetree::TreeSnapshot> snapshot;
  widetree::TreeView view;
  std::uint32_t fanout = 0;
  std::uint32_t splits = 0;
  double buildUs = 0;
};

Wide makeWide(std::uint32_t fanout, std::uint32_t splits) {
  if (fanout < 1 || (splits > 0 && fanout / splits < 1)) {
    fail("fanout cannot place the requested splits");
  }
  const std::uint32_t nodes = fanout + 1u + splits;
  const auto started = Clock::now();
  widetree::TreeBuilder builder(nodes);
  builder.setRoot(0);
  builder.addChildRange(0, 1, fanout);
  const std::uint32_t step = splits == 0 ? 1u : fanout / splits;
  for (std::uint32_t i = 0; i < splits; ++i) {
    const widetree::NodeId parent = 1u + i * step;
    const widetree::NodeId leaf = fanout + 1u + i;
    builder.addEdge(parent, leaf);
  }
  Wide wide;
  wide.snapshot = mustBuild(builder);
  wide.fanout = fanout;
  wide.splits = splits;
  wide.buildUs = usSince(started);
  if (wide.view.reset(wide.snapshot).status != widetree::Status::Ok) {
    fail("reset");
  }
  if (wide.view.expandAt(*wide.view.rowRef(0)).status != widetree::Status::Ok) {
    fail("expand root");
  }
  for (std::uint32_t i = 0; i < splits; ++i) {
    const widetree::NodeId parent = 1u + i * step;
    const auto row = wide.view.rowOf(parent);
    if (!row) {
      fail("split child is not visible");
    }
    if (wide.view.expandAt(*wide.view.rowRef(*row)).status != widetree::Status::Ok) {
      fail("expand split");
    }
  }
  const auto memory = wide.view.memory();
  const std::uint32_t reachable = splits + 1u;
  if (memory.expandedCount != reachable) {
    fail("reachable expanded count");
  }
  if (memory.segmentCount != reachable * 2u) {
    std::fprintf(stderr, "segments %u expanded %u\n", memory.segmentCount, memory.expandedCount);
    fail("segment count for wide shape");
  }
  return wide;
}

Wide makeChain(std::uint32_t nodes) {
  if (nodes < 2) {
    fail("chain");
  }
  const auto started = Clock::now();
  widetree::TreeBuilder builder(nodes);
  builder.setRoot(0);
  for (std::uint32_t id = 1; id < nodes; ++id) {
    builder.addEdge(id - 1, id);
  }
  Wide wide;
  wide.snapshot = mustBuild(builder);
  wide.fanout = 1;
  wide.splits = nodes - 2u;
  wide.buildUs = usSince(started);
  wide.view.reset(wide.snapshot);
  if (wide.view.reveal(nodes - 1).status != widetree::Status::Ok) {
    fail("reveal chain");
  }
  const auto memory = wide.view.memory();
  if (memory.segmentCount != nodes || memory.expandedCount != nodes - 1u) {
    fail("chain shape");
  }
  return wide;
}

struct QueryUs {
  double rows64 = 0;
  double rowRef = 0;
  double rowOf = 0;
  int iters = 0;
};

QueryUs measureQueries(widetree::TreeView& view, widetree::NodeId target, int trials) {
  auto once = [&] {
    widetree::VisibleRow window[64];
    const auto row = view.rowOf(target);
    if (!row) {
      fail("query target hidden");
    }
    g_sink += view.getVisibleRows(0, window);
    g_sink += view.getVisibleRows(*row, window);
    const auto ref = view.rowRef(*row);
    if (!ref || ref->node != target) {
      fail("rowRef");
    }
    g_sink += ref->node;
    g_sink += static_cast<std::uint64_t>(*row);
  };
  once();
  const double probe = [&] {
    const auto started = Clock::now();
    once();
    return usSince(started);
  }();
  int iters = probe <= 0.2 ? 20000 : static_cast<int>(8000.0 / probe);
  if (iters < 20) {
    iters = 20;
  }
  if (iters > 20000) {
    iters = 20000;
  }

  std::vector<double> rows64;
  std::vector<double> rowRef;
  std::vector<double> rowOf;
  rows64.reserve(static_cast<std::size_t>(trials));
  rowRef.reserve(static_cast<std::size_t>(trials));
  rowOf.reserve(static_cast<std::size_t>(trials));
  for (int trial = 0; trial < trials; ++trial) {
    {
      const auto started = Clock::now();
      for (int i = 0; i < iters; ++i) {
        widetree::VisibleRow window[64];
        g_sink += view.getVisibleRows(0, window);
        const auto mid = view.rowCount() / 2;
        g_sink += view.getVisibleRows(mid, window);
      }
      rows64.push_back(usSince(started) / (static_cast<double>(iters) * 2.0));
    }
    {
      const auto started = Clock::now();
      for (int i = 0; i < iters; ++i) {
        const auto ref = view.rowRef(view.rowCount() / 2);
        g_sink += ref ? ref->node : 0;
      }
      rowRef.push_back(usSince(started) / static_cast<double>(iters));
    }
    {
      const auto started = Clock::now();
      for (int i = 0; i < iters; ++i) {
        const auto row = view.rowOf(target);
        g_sink += row ? *row : 0;
      }
      rowOf.push_back(usSince(started) / static_cast<double>(iters));
    }
  }
  return QueryUs{medianOf(std::move(rows64)), medianOf(std::move(rowRef)), medianOf(std::move(rowOf)),
                 iters};
}

struct ToggleUs {
  double expand = 0;
  double collapse = 0;
  int iters = 0;
};

ToggleUs measureRootToggle(widetree::TreeView& view, int trials) {
  auto restoreExpanded = [&] {
    if (!view.isExpanded(view.snapshot()->root())) {
      if (view.expandAt(*view.rowRef(0)).status != widetree::Status::Ok) {
        fail("restore expand");
      }
    }
  };
  restoreExpanded();
  auto onePair = [&](double& collapseUs, double& expandUs) {
    const auto down = *view.rowRef(0);
    const auto c0 = Clock::now();
    if (view.collapseAt(down).status != widetree::Status::Ok) {
      fail("collapse");
    }
    collapseUs += usSince(c0);
    const auto up = *view.rowRef(0);
    const auto e0 = Clock::now();
    if (view.expandAt(up).status != widetree::Status::Ok) {
      fail("expand");
    }
    expandUs += usSince(e0);
    g_sink += view.rowCount();
  };
  {
    double ignoreC = 0;
    double ignoreE = 0;
    onePair(ignoreC, ignoreE);
  }
  const double probe = [&] {
    double c = 0;
    double e = 0;
    const auto started = Clock::now();
    onePair(c, e);
    return usSince(started);
  }();
  int iters = probe <= 1.0 ? 2000 : static_cast<int>(6000.0 / probe);
  if (iters < 6) {
    iters = 6;
  }
  if (iters > 2000) {
    iters = 2000;
  }
  std::vector<double> expandSamples;
  std::vector<double> collapseSamples;
  for (int trial = 0; trial < trials; ++trial) {
    double collapseUs = 0;
    double expandUs = 0;
    for (int i = 0; i < iters; ++i) {
      onePair(collapseUs, expandUs);
    }
    collapseSamples.push_back(collapseUs / static_cast<double>(iters));
    expandSamples.push_back(expandUs / static_cast<double>(iters));
  }
  return ToggleUs{medianOf(std::move(expandSamples)), medianOf(std::move(collapseSamples)), iters};
}

// Collapse then expand one visible node. The timers exclude rowOf and rowRef.
// When `expectDrop` is non-zero, the warmup collapse must remove exactly that
// many segments, so a "local" edit cannot silently become a root rebuild.
ToggleUs measureIdToggle(widetree::TreeView& view, widetree::NodeId id, int trials,
                          std::uint32_t expectDrop) {
  auto restoreExpanded = [&] {
    if (!view.isExpanded(id)) {
      const auto row = view.rowOf(id);
      if (!row) {
        fail("restore row");
      }
      if (view.expandAt(*view.rowRef(*row)).status != widetree::Status::Ok) {
        fail("restore expand");
      }
    }
  };
  restoreExpanded();
  const std::uint32_t before = view.memory().segmentCount;
  auto onePair = [&](double& collapseUs, double& expandUs, bool checkDrop) {
    const auto rowDown = view.rowOf(id);
    if (!rowDown) {
      fail("row before collapse");
    }
    const auto down = *view.rowRef(*rowDown);
    const auto c0 = Clock::now();
    if (view.collapseAt(down).status != widetree::Status::Ok) {
      fail("collapse");
    }
    collapseUs += usSince(c0);
    if (checkDrop) {
      const std::uint32_t mid = view.memory().segmentCount;
      if (before < mid || before - mid != expectDrop) {
        std::fprintf(stderr, "segments %u -> %u, expected drop %u\n", before, mid, expectDrop);
        fail("edit changed the wrong number of segments");
      }
    }
    const auto rowUp = view.rowOf(id);
    if (!rowUp) {
      fail("row before expand");
    }
    const auto up = *view.rowRef(*rowUp);
    const auto e0 = Clock::now();
    if (view.expandAt(up).status != widetree::Status::Ok) {
      fail("expand");
    }
    expandUs += usSince(e0);
    g_sink += view.rowCount();
  };
  {
    double ignoreC = 0;
    double ignoreE = 0;
    onePair(ignoreC, ignoreE, expectDrop != 0);
  }
  const double probe = [&] {
    double c = 0;
    double e = 0;
    const auto started = Clock::now();
    onePair(c, e, false);
    return usSince(started);
  }();
  int iters = probe <= 1.0 ? 2000 : static_cast<int>(6000.0 / probe);
  if (iters < 6) {
    iters = 6;
  }
  if (iters > 2000) {
    iters = 2000;
  }
  std::vector<double> expandSamples;
  std::vector<double> collapseSamples;
  for (int trial = 0; trial < trials; ++trial) {
    double collapseUs = 0;
    double expandUs = 0;
    for (int i = 0; i < iters; ++i) {
      onePair(collapseUs, expandUs, false);
    }
    collapseSamples.push_back(collapseUs / static_cast<double>(iters));
    expandSamples.push_back(expandUs / static_cast<double>(iters));
  }
  if (view.memory().segmentCount != before) {
    fail("edit toggle did not restore S");
  }
  return ToggleUs{medianOf(std::move(expandSamples)), medianOf(std::move(collapseSamples)), iters};
}

struct Row {
  const char* shape = "";
  std::uint32_t n = 0;
  std::uint32_t fanout = 0;
  std::uint32_t expanded = 0;
  std::uint32_t segments = 0;
  std::uint32_t indexedParents = 0;
  std::uint64_t bitmapBytes = 0;
  std::uint64_t indexEstimateBytes = 0;
  std::uint64_t segmentBytes = 0;
  double buildUs = 0;
  QueryUs query;
  ToggleUs toggle;
};

void printHeader() {
  const char* compiler = "unknown";
#if defined(__clang__)
  compiler = "clang";
#elif defined(_MSC_VER)
  compiler = "msvc";
#elif defined(__GNUC__)
  compiler = "gcc";
#endif
  std::printf("# v1 microbenchmark  compiler=%s  opt=-O2\n", compiler);
  std::printf("# median of 5 trials. expand_us and collapse_us are the mutation only; rowRef is not included.\n");
  std::printf("# K is memory.expandedCount. In these shapes every expanded node is reachable.\n");
  std::printf("# S is memory.segmentCount. There is no rowAt(); rowRef is the row->node locate.\n");
  std::printf("# edit-root toggles the root. edit-head/mid/tail toggle one expanded child;\n");
  std::printf("# that child drops exactly two segments. All four still rebuild the whole projection.\n");
  std::puts(
      "shape\tN\tfanout\tK\tS\tindexParents\tbitmapB\tindexEstB\tsegmentB\tbuild_us\t"
      "rows64_us\trowRef_us\trowOf_us\tquery_iters\texpand_us\tcollapse_us\ttoggle_iters");
}

void printRow(const Row& row) {
  std::printf("%s\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%.1f\t%.3f\t%.3f\t%.3f\t%d\t%.3f\t%.3f\t%d\n",
              row.shape, row.n, row.fanout, row.expanded, row.segments, row.indexedParents,
              static_cast<unsigned long long>(row.bitmapBytes),
              static_cast<unsigned long long>(row.indexEstimateBytes),
              static_cast<unsigned long long>(row.segmentBytes), row.buildUs, row.query.rows64,
              row.query.rowRef, row.query.rowOf, row.query.iters, row.toggle.expand, row.toggle.collapse,
              row.toggle.iters);
  std::fflush(stdout);
}

Row runWide(const char* shape, std::uint32_t fanout, std::uint32_t splits, widetree::NodeId target) {
  Wide wide = makeWide(fanout, splits);
  const auto memory = wide.view.memory();
  Row row;
  row.shape = shape;
  row.n = wide.snapshot->nodeCount();
  row.fanout = fanout;
  row.expanded = memory.expandedCount;
  row.segments = memory.segmentCount;
  row.indexedParents = memory.indexedParentCount;
  row.bitmapBytes = memory.bitmapBytes;
  row.indexEstimateBytes = memory.indexPayloadBytes + memory.indexNodeEstimateBytes;
  row.segmentBytes = memory.segmentBytes + memory.prefixBytes;
  row.buildUs = wide.buildUs;
  row.query = measureQueries(wide.view, target, 5);
  row.toggle = measureRootToggle(wide.view, 5);
  const auto after = wide.view.memory();
  if (after.segmentCount != row.segments || after.expandedCount != row.expanded) {
    fail("toggle did not restore K and S");
  }
  if (wide.view.checkInvariants() != nullptr) {
    fail(wide.view.checkInvariants());
  }
  return row;
}

Row runChain(std::uint32_t nodes) {
  Wide wide = makeChain(nodes);
  const auto memory = wide.view.memory();
  Row row;
  row.shape = "chain";
  row.n = nodes;
  row.fanout = 1;
  row.expanded = memory.expandedCount;
  row.segments = memory.segmentCount;
  row.indexedParents = memory.indexedParentCount;
  row.bitmapBytes = memory.bitmapBytes;
  row.indexEstimateBytes = memory.indexPayloadBytes + memory.indexNodeEstimateBytes;
  row.segmentBytes = memory.segmentBytes + memory.prefixBytes;
  row.buildUs = wide.buildUs;
  row.query = measureQueries(wide.view, nodes - 1, 5);
  row.toggle = measureRootToggle(wide.view, 5);
  if (wide.view.memory().segmentCount != row.segments) {
    fail("chain toggle changed S");
  }
  if (wide.view.checkInvariants() != nullptr) {
    fail(wide.view.checkInvariants());
  }
  return row;
}

// Root plus one expanded child at the head, middle, and tail of a split
// projection. S = 2 * (splits + 1). The local edit removes two segments;
// the root edit removes almost all of them. Same tree, four timers.
void runEdit(std::uint32_t fanout, std::uint32_t splits) {
  Wide wide = makeWide(fanout, splits);
  const auto memory = wide.view.memory();
  const std::uint32_t step = fanout / splits;
  const widetree::NodeId head = 1u;
  const widetree::NodeId mid = 1u + (splits / 2u) * step;
  const widetree::NodeId tail = 1u + (splits - 1u) * step;
  const QueryUs query = measureQueries(wide.view, tail, 5);
  const ToggleUs toggles[4] = {
      measureRootToggle(wide.view, 5),
      measureIdToggle(wide.view, head, 5, 2),
      measureIdToggle(wide.view, mid, 5, 2),
      measureIdToggle(wide.view, tail, 5, 2),
  };
  const char* shapes[4] = {"edit-root", "edit-head", "edit-mid", "edit-tail"};
  if (wide.view.memory().segmentCount != memory.segmentCount) {
    fail("edit shape did not restore S");
  }
  if (wide.view.checkInvariants() != nullptr) {
    fail(wide.view.checkInvariants());
  }
  for (int i = 0; i < 4; ++i) {
    Row row;
    row.shape = shapes[i];
    row.n = wide.snapshot->nodeCount();
    row.fanout = fanout;
    row.expanded = memory.expandedCount;
    row.segments = memory.segmentCount;
    row.indexedParents = memory.indexedParentCount;
    row.bitmapBytes = memory.bitmapBytes;
    row.indexEstimateBytes = memory.indexPayloadBytes + memory.indexNodeEstimateBytes;
    row.segmentBytes = memory.segmentBytes + memory.prefixBytes;
    row.buildUs = wide.buildUs;
    row.query = query;
    row.toggle = toggles[i];
    printRow(row);
  }
}

}  // namespace

int main() {
  printHeader();
  // N changes, K=1, S=2. Target is the middle child.
  const std::uint32_t wideFanout[] = {10000, 50000, 100000, 250000, 500000, 1000000, 2000000, 4000000};
  for (std::uint32_t fanout : wideFanout) {
    printRow(runWide("wide", fanout, 0, fanout / 2));
  }
  // N stays near 1e5. Expanded children share one parent, so S = 2K and the
  // sparse index is a single vector. Target is the last of those children.
  constexpr std::uint32_t splitFanout = 100000;
  const std::uint32_t splits[] = {10, 40, 100, 250, 500, 1000, 2000, 4000, 8000, 16000};
  for (std::uint32_t count : splits) {
    const std::uint32_t step = splitFanout / count;
    const widetree::NodeId target = 1u + (count - 1u) * step;
    printRow(runWide("split", splitFanout, count, target));
  }
  // Each expanded node is its own parent span. Collapse of the root does not
  // edit the index, but the candidate still copies it. The copy is two arrays.
  const std::uint32_t chains[] = {250, 1000, 2500, 5000, 10000, 20000, 40000};
  for (std::uint32_t nodes : chains) {
    printRow(runChain(nodes));
  }
  // Where a two-segment edit sits. Fanout stays strictly above the split
  // count so the root keeps a trailing run; S = 2*(splits+1): 8002, 32002, 128002.
  const std::uint32_t editFanout[] = {100000, 100000, 128000};
  const std::uint32_t editSplits[] = {4000, 16000, 64000};
  for (int i = 0; i < 3; ++i) {
    runEdit(editFanout[i], editSplits[i]);
  }
  std::printf("# sink %llu\n", static_cast<unsigned long long>(g_sink));
  return 0;
}
