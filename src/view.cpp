#include "widetree/view.hpp"

#include <algorithm>
#include <utility>

namespace widetree {
namespace {

constexpr std::uint64_t kMapNodeEstimate = 64;

ViewResult errorResult(Status status, std::uint64_t revision, RowCount rows) {
  ViewResult result;
  result.status = status;
  result.change.oldRevision = revision;
  result.change.newRevision = revision;
  result.change.oldRowCount = rows;
  result.change.newRowCount = rows;
  result.change.kind = ChangeKind::NoChange;
  return result;
}

}  // namespace

void TreeView::ExpandBitmap::reset(std::uint32_t nodeCount) {
  words_.assign((static_cast<std::size_t>(nodeCount) + 63u) / 64u, 0);
  count_ = 0;
}

bool TreeView::ExpandBitmap::test(std::uint32_t i) const noexcept {
  return ((words_[i >> 6] >> (i & 63u)) & 1ull) != 0;
}

void TreeView::ExpandBitmap::set(std::uint32_t i) noexcept {
  const std::uint64_t mask = 1ull << (i & 63u);
  std::uint64_t& word = words_[i >> 6];
  if ((word & mask) == 0) {
    word |= mask;
    ++count_;
  }
}

void TreeView::ExpandBitmap::clear(std::uint32_t i) noexcept {
  const std::uint64_t mask = 1ull << (i & 63u);
  std::uint64_t& word = words_[i >> 6];
  if ((word & mask) != 0) {
    word &= ~mask;
    --count_;
  }
}

ViewResult TreeView::reset(std::shared_ptr<const TreeSnapshot> snapshot, ViewConfig config) {
  if (!snapshot) {
    return errorResult(Status::NoSnapshot, revision_, rowCount_);
  }
  const std::uint64_t oldRevision = revision_;
  const RowCount oldCount = rowCount_;

  ExpandBitmap bits;
  bits.reset(snapshot->nodeCount());
  ExpandIndex index;
  TreeView scratch;
  scratch.snap_ = snapshot;
  scratch.hideRoot_ = config.hideRoot;
  std::vector<Segment> segments;
  scratch.project(bits, index, segments);
  std::vector<RowIndex> prefix;
  RowCount rows = 0;
  buildPrefix(segments, prefix, rows);

  snap_ = std::move(snapshot);
  hideRoot_ = config.hideRoot;
  bits_ = std::move(bits);
  index_ = std::move(index);
  segments_ = std::move(segments);
  prefix_ = std::move(prefix);
  rowCount_ = rows;
  ++revision_;

  ViewResult result;
  result.status = Status::Ok;
  result.change.oldRevision = oldRevision;
  result.change.newRevision = revision_;
  result.change.oldRowCount = oldCount;
  result.change.newRowCount = rowCount_;
  result.change.kind = ChangeKind::Reset;
  result.change.splice = RowSplice{0, oldCount, rowCount_};
  return result;
}

bool TreeView::isExpanded(NodeId id) const noexcept {
  if (!snap_ || !snap_->contains(id)) {
    return false;
  }
  return bits_.test(id);
}

bool TreeView::isVisible(NodeId id) const {
  if (!snap_ || !snap_->contains(id)) {
    return false;
  }
  const NodeId root = snap_->root();
  if (id == root) {
    return !hideRoot_;
  }
  const std::uint32_t guard = snap_->nodeCount() + 1u;
  NodeId cur = id;
  for (std::uint32_t steps = 0; steps < guard; ++steps) {
    const NodeId parent = snap_->topology(cur).parent;
    if (parent == kInvalidNode || !snap_->contains(parent)) {
      return false;
    }
    if (hideRoot_ && parent == root) {
      return true;
    }
    if (!bits_.test(parent)) {
      return false;
    }
    if (parent == root) {
      return true;
    }
    cur = parent;
  }
  return false;
}

std::optional<RowIndex> TreeView::rowOf(NodeId id) const {
  if (!isVisible(id)) {
    return std::nullopt;
  }
  NodeId segmentParent = kDisplayRoot;
  std::uint32_t ordinal = 0;
  if (!hideRoot_ && id == snap_->root()) {
    segmentParent = kDisplayRoot;
    ordinal = 0;
  } else if (hideRoot_ && snap_->topology(id).parent == snap_->root()) {
    segmentParent = kDisplayRoot;
    ordinal = snap_->topology(id).rowInParent;
  } else {
    segmentParent = snap_->topology(id).parent;
    ordinal = snap_->topology(id).rowInParent;
  }
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    const Segment& segment = segments_[i];
    if (segment.parent == segmentParent && ordinal >= segment.begin &&
        ordinal < segment.end) {
      return prefix_[i] + static_cast<RowIndex>(ordinal - segment.begin);
    }
  }
  return std::nullopt;
}

std::size_t TreeView::getVisibleRows(RowIndex start, std::span<VisibleRow> out) const {
  if (!snap_ || out.empty() || start >= rowCount_) {
    return 0;
  }
  std::size_t segmentIndex = 0;
  std::uint32_t ordinal = 0;
  if (!locate(start, segmentIndex, ordinal)) {
    return 0;
  }
  std::size_t written = 0;
  RowIndex row = start;
  while (written < out.size() && segmentIndex < segments_.size()) {
    const Segment& segment = segments_[segmentIndex];
    while (ordinal < segment.end && written < out.size()) {
      const NodeId id = childAt(segment.parent, ordinal);
      const NodeTopology& topo = snap_->topology(id);
      VisibleRow& dst = out[written];
      dst.row = row;
      dst.id = id;
      dst.depth = segment.depth;
      dst.ordinalInParent = ordinal;
      dst.childCount = topo.childCount;
      dst.expanded = bits_.test(id);
      dst.label = snap_->label(id);
      dst.payload = snap_->payload(id);
      ++written;
      ++row;
      ++ordinal;
    }
    ++segmentIndex;
    if (segmentIndex < segments_.size()) {
      ordinal = segments_[segmentIndex].begin;
    }
  }
  return written;
}

std::optional<RowRef> TreeView::rowRef(RowIndex row) const {
  if (!snap_ || row >= rowCount_) {
    return std::nullopt;
  }
  std::size_t segmentIndex = 0;
  std::uint32_t ordinal = 0;
  if (!locate(row, segmentIndex, ordinal)) {
    return std::nullopt;
  }
  RowRef ref;
  ref.snapshotId = snap_->id();
  ref.revision = revision_;
  ref.row = row;
  ref.node = childAt(segments_[segmentIndex].parent, ordinal);
  return ref;
}

ViewResult TreeView::expandAt(RowRef ref) {
  NodeId id = kInvalidNode;
  if (const Status status = checkRef(ref, id); status != Status::Ok) {
    return errorResult(status, revision_, rowCount_);
  }
  if (snap_->topology(id).childCount == 0 || bits_.test(id)) {
    return noChange();
  }

  const std::uint64_t oldRevision = revision_;
  const RowCount oldCount = rowCount_;
  ExpandBitmap bits = bits_;
  ExpandIndex index = index_;
  bits.set(id);
  const NodeId parent = snap_->topology(id).parent;
  if (parent != kInvalidNode) {
    insertOrdinal(index, parent, snap_->topology(id).rowInParent);
  }
  Candidate candidate = candidateFrom(std::move(bits), std::move(index));
  const RowCount inserted = candidate.rowCount - oldCount;
  commit(candidate);
  return finishSplice(oldRevision, oldCount, ref.row + 1, 0, inserted, ref.row);
}

ViewResult TreeView::collapseAt(RowRef ref) {
  NodeId id = kInvalidNode;
  if (const Status status = checkRef(ref, id); status != Status::Ok) {
    return errorResult(status, revision_, rowCount_);
  }
  if (!bits_.test(id)) {
    return noChange();
  }

  const std::uint64_t oldRevision = revision_;
  const RowCount oldCount = rowCount_;
  ExpandBitmap bits = bits_;
  ExpandIndex index = index_;
  bits.clear(id);
  const NodeId parent = snap_->topology(id).parent;
  if (parent != kInvalidNode) {
    eraseOrdinal(index, parent, snap_->topology(id).rowInParent);
  }
  Candidate candidate = candidateFrom(std::move(bits), std::move(index));
  const RowCount removed = oldCount - candidate.rowCount;
  commit(candidate);
  return finishSplice(oldRevision, oldCount, ref.row + 1, removed, 0, ref.row);
}

ViewResult TreeView::reveal(NodeId id) {
  if (!snap_) {
    return errorResult(Status::NoSnapshot, revision_, rowCount_);
  }
  if (!snap_->contains(id) || (hideRoot_ && id == snap_->root())) {
    return errorResult(Status::InvalidNode, revision_, rowCount_);
  }
  if (isVisible(id)) {
    return noChange();
  }

  // Nearest collapsed ancestor first. The back() entry is the outermost one,
  // which is the only toggled node already on screen.
  std::vector<NodeId> toExpand;
  NodeId cur = id;
  const NodeId root = snap_->root();
  const std::uint32_t guard = snap_->nodeCount() + 1u;
  for (std::uint32_t steps = 0; steps < guard; ++steps) {
    const NodeId parent = snap_->topology(cur).parent;
    if (parent == kInvalidNode || !snap_->contains(parent)) {
      break;
    }
    if (hideRoot_ && parent == root) {
      break;
    }
    if (!bits_.test(parent)) {
      toExpand.push_back(parent);
    }
    if (parent == root) {
      break;
    }
    cur = parent;
  }
  if (toExpand.empty()) {
    return noChange();
  }

  const NodeId anchor = toExpand.back();
  const std::optional<RowIndex> anchorRow = rowOf(anchor);
  if (!anchorRow) {
    return errorResult(Status::InvalidNode, revision_, rowCount_);
  }

  const std::uint64_t oldRevision = revision_;
  const RowCount oldCount = rowCount_;
  ExpandBitmap bits = bits_;
  ExpandIndex index = index_;
  for (NodeId node : toExpand) {
    bits.set(node);
    const NodeId parent = snap_->topology(node).parent;
    if (parent != kInvalidNode) {
      insertOrdinal(index, parent, snap_->topology(node).rowInParent);
    }
  }
  Candidate candidate = candidateFrom(std::move(bits), std::move(index));
  const RowCount inserted = candidate.rowCount - oldCount;
  commit(candidate);
  return finishSplice(oldRevision, oldCount, *anchorRow + 1, 0, inserted, *anchorRow);
}

std::vector<SegmentInfo> TreeView::segments() const {
  std::vector<SegmentInfo> out;
  out.reserve(segments_.size());
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    const Segment& segment = segments_[i];
    out.push_back(SegmentInfo{segment.parent, segment.begin, segment.end, segment.depth,
                              prefix_[i]});
  }
  return out;
}

MemoryReport TreeView::memory() const {
  MemoryReport report;
  if (snap_) {
    report.snapshotBytes = snap_->residentBytes();
  }
  report.bitmapBytes = bits_.bytes();
  report.segmentCount = static_cast<std::uint32_t>(segments_.size());
  report.segmentBytes = segments_.size() * sizeof(Segment);
  report.prefixBytes = prefix_.size() * sizeof(RowIndex);
  report.indexedParentCount = static_cast<std::uint32_t>(index_.ordinals.size());
  report.expandedCount = bits_.count();
  for (const auto& entry : index_.ordinals) {
    report.indexPayloadBytes += entry.second.size() * sizeof(std::uint32_t);
    report.indexNodeEstimateBytes += kMapNodeEstimate;
    report.indexNodeEstimateBytes +=
        entry.second.capacity() * sizeof(std::uint32_t) + sizeof(entry.second);
  }
  return report;
}

const char* TreeView::checkInvariants() const {
  if (!snap_) {
    return segments_.empty() && rowCount_ == 0 ? nullptr : "projection without snapshot";
  }
  if (prefix_.size() != segments_.size() + 1) {
    return "prefix length";
  }
  if (prefix_.empty() || prefix_[0] != 0 || prefix_.back() != rowCount_) {
    return "prefix bounds";
  }
  RowIndex expect = 0;
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    const Segment& segment = segments_[i];
    if (segment.end <= segment.begin) {
      return "empty segment";
    }
    if (prefix_[i] != expect) {
      return "prefix mismatch";
    }
    const RowCount len = static_cast<RowCount>(segment.end - segment.begin);
    expect += len;
    if (i > 0) {
      const Segment& prev = segments_[i - 1];
      if (prev.parent == segment.parent && prev.depth == segment.depth &&
          prev.end == segment.begin) {
        return "adjacent segments can merge";
      }
    }
    if (segment.parent == kDisplayRoot) {
      if (segment.depth != 0) {
        return "display segment depth";
      }
      const std::uint32_t limit =
          hideRoot_ ? snap_->topology(snap_->root()).childCount : 1u;
      if (segment.end > limit) {
        return "display segment range";
      }
    } else {
      if (!snap_->contains(segment.parent)) {
        return "segment parent";
      }
      if (!bits_.test(segment.parent)) {
        return "segment parent is not expanded";
      }
      if (segment.end > snap_->topology(segment.parent).childCount) {
        return "segment range";
      }
      if (segment.depth != depthOf(segment.parent) + 1u) {
        return "segment depth";
      }
    }
  }
  if (expect != rowCount_) {
    return "row count";
  }

  for (const auto& entry : index_.ordinals) {
    const NodeId parent = entry.first;
    if (!snap_->contains(parent)) {
      return "index parent";
    }
    const auto& ordinals = entry.second;
    if (ordinals.empty()) {
      return "empty index entry";
    }
    for (std::size_t i = 0; i < ordinals.size(); ++i) {
      if (i > 0 && ordinals[i] <= ordinals[i - 1]) {
        return "index order";
      }
      if (ordinals[i] >= snap_->topology(parent).childCount) {
        return "index ordinal";
      }
      const NodeId child = snap_->child(parent, ordinals[i]);
      if (!bits_.test(child)) {
        return "index child is not expanded";
      }
    }
  }

  for (std::uint32_t id = 0; id < snap_->nodeCount(); ++id) {
    if (!bits_.test(id)) {
      continue;
    }
    const NodeId parent = snap_->topology(id).parent;
    if (parent == kInvalidNode) {
      continue;
    }
    const auto it = index_.ordinals.find(parent);
    if (it == index_.ordinals.end()) {
      return "expanded node missing from index";
    }
    const auto& ordinals = it->second;
    const std::uint32_t ordinal = snap_->topology(id).rowInParent;
    if (!std::binary_search(ordinals.begin(), ordinals.end(), ordinal)) {
      return "expanded ordinal missing from index";
    }
  }
  return nullptr;
}

Status TreeView::checkRef(RowRef ref, NodeId& outId) const {
  if (!snap_) {
    return Status::NoSnapshot;
  }
  if (ref.snapshotId != snap_->id()) {
    return Status::StaleSnapshot;
  }
  if (ref.revision != revision_) {
    return Status::StaleRowRef;
  }
  if (ref.row >= rowCount_) {
    return Status::InvalidRowRef;
  }
  std::size_t segmentIndex = 0;
  std::uint32_t ordinal = 0;
  if (!locate(ref.row, segmentIndex, ordinal)) {
    return Status::InvalidRowRef;
  }
  const NodeId id = childAt(segments_[segmentIndex].parent, ordinal);
  if (id != ref.node) {
    return Status::InvalidRowRef;
  }
  outId = id;
  return Status::Ok;
}

NodeId TreeView::childAt(NodeId segmentParent, std::uint32_t ordinal) const {
  if (segmentParent == kDisplayRoot) {
    if (!hideRoot_) {
      return snap_->root();
    }
    return snap_->child(snap_->root(), ordinal);
  }
  return snap_->child(segmentParent, ordinal);
}

bool TreeView::locate(RowIndex row, std::size_t& segmentIndex,
                      std::uint32_t& ordinal) const {
  if (row >= rowCount_ || segments_.empty()) {
    return false;
  }
  std::size_t lo = 0;
  std::size_t hi = segments_.size();
  while (lo + 1 < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (prefix_[mid] <= row) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const Segment& segment = segments_[lo];
  const RowIndex delta = row - prefix_[lo];
  if (delta >= static_cast<RowIndex>(segment.end - segment.begin)) {
    return false;
  }
  segmentIndex = lo;
  ordinal = segment.begin + static_cast<std::uint32_t>(delta);
  return true;
}

void TreeView::project(const ExpandBitmap& bits, const ExpandIndex& index,
                       std::vector<Segment>& out) const {
  out.clear();
  if (!snap_) {
    return;
  }
  const NodeId root = snap_->root();
  std::uint32_t rootOrdinalStorage = 0;

  std::vector<Frame> stack;
  stack.reserve(64);
  Frame top{};
  top.segmentParent = kDisplayRoot;
  top.depth = 0;
  if (!hideRoot_) {
    top.childCount = 1;
    if (bits.test(root)) {
      top.ords = &rootOrdinalStorage;
      top.ordCount = 1;
    }
  } else {
    top.childCount = snap_->topology(root).childCount;
    const auto it = index.ordinals.find(root);
    if (it != index.ordinals.end() && !it->second.empty()) {
      top.ords = it->second.data();
      top.ordCount = static_cast<std::uint32_t>(it->second.size());
    }
  }
  stack.push_back(top);

  while (!stack.empty()) {
    const Frame cur = stack.back();
    if (cur.nextExp >= cur.ordCount) {
      if (cur.begin < cur.childCount) {
        out.push_back(Segment{cur.segmentParent, cur.begin, cur.childCount, cur.depth});
      }
      stack.pop_back();
      continue;
    }

    const std::uint32_t ordinal = cur.ords[cur.nextExp];
    stack.back().nextExp = cur.nextExp + 1;
    if (ordinal < cur.begin || ordinal >= cur.childCount) {
      continue;
    }
    const std::uint32_t end = ordinal + 1;
    if (cur.begin < end) {
      out.push_back(Segment{cur.segmentParent, cur.begin, end, cur.depth});
    }
    stack.back().begin = end;

    const NodeId child = childAt(cur.segmentParent, ordinal);
    if (child == kInvalidNode || !snap_->contains(child)) {
      continue;
    }
    const std::uint32_t kids = snap_->topology(child).childCount;
    if (!bits.test(child) || kids == 0) {
      continue;
    }
    Frame childFrame{};
    childFrame.segmentParent = child;
    childFrame.depth = cur.depth + 1;
    childFrame.childCount = kids;
    const auto it = index.ordinals.find(child);
    if (it != index.ordinals.end() && !it->second.empty()) {
      childFrame.ords = it->second.data();
      childFrame.ordCount = static_cast<std::uint32_t>(it->second.size());
    }
    stack.push_back(childFrame);
  }
}

void TreeView::buildPrefix(const std::vector<Segment>& segments,
                           std::vector<RowIndex>& prefix, RowCount& rowCount) {
  prefix.resize(segments.size() + 1);
  prefix[0] = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const RowCount len =
        static_cast<RowCount>(segments[i].end) - static_cast<RowCount>(segments[i].begin);
    prefix[i + 1] = prefix[i] + len;
  }
  rowCount = prefix.back();
}

void TreeView::insertOrdinal(ExpandIndex& index, NodeId parent, std::uint32_t ordinal) {
  std::vector<std::uint32_t>& ordinals = index.ordinals[parent];
  const auto it = std::lower_bound(ordinals.begin(), ordinals.end(), ordinal);
  if (it == ordinals.end() || *it != ordinal) {
    ordinals.insert(it, ordinal);
  }
}

void TreeView::eraseOrdinal(ExpandIndex& index, NodeId parent, std::uint32_t ordinal) {
  const auto found = index.ordinals.find(parent);
  if (found == index.ordinals.end()) {
    return;
  }
  std::vector<std::uint32_t>& ordinals = found->second;
  const auto it = std::lower_bound(ordinals.begin(), ordinals.end(), ordinal);
  if (it != ordinals.end() && *it == ordinal) {
    ordinals.erase(it);
  }
  if (ordinals.empty()) {
    index.ordinals.erase(found);
  }
}

TreeView::Candidate TreeView::candidateFrom(ExpandBitmap bits, ExpandIndex index) const {
  Candidate candidate;
  candidate.bits = std::move(bits);
  candidate.index = std::move(index);
  project(candidate.bits, candidate.index, candidate.segments);
  buildPrefix(candidate.segments, candidate.prefix, candidate.rowCount);
  return candidate;
}

void TreeView::commit(Candidate& candidate) {
  bits_ = std::move(candidate.bits);
  index_ = std::move(candidate.index);
  segments_ = std::move(candidate.segments);
  prefix_ = std::move(candidate.prefix);
  rowCount_ = candidate.rowCount;
  ++revision_;
}

ViewResult TreeView::finishSplice(std::uint64_t oldRevision, RowCount oldCount, RowIndex first,
                                  RowCount removed, RowCount inserted, RowIndex iconRow) {
  ViewResult result;
  result.status = Status::Ok;
  result.change.oldRevision = oldRevision;
  result.change.newRevision = revision_;
  result.change.oldRowCount = oldCount;
  result.change.newRowCount = rowCount_;
  result.change.kind = ChangeKind::Splice;
  result.change.splice = RowSplice{first, removed, inserted};
  result.change.iconRows.push_back(iconRow);
  return result;
}

ViewResult TreeView::noChange() const {
  ViewResult result;
  result.status = Status::NoChange;
  result.change.oldRevision = revision_;
  result.change.newRevision = revision_;
  result.change.oldRowCount = rowCount_;
  result.change.newRowCount = rowCount_;
  result.change.kind = ChangeKind::NoChange;
  return result;
}

std::uint32_t TreeView::depthOf(NodeId id) const {
  if (!hideRoot_ && id == snap_->root()) {
    return 0;
  }
  std::uint32_t depth = 0;
  NodeId cur = id;
  const NodeId root = snap_->root();
  const std::uint32_t guard = snap_->nodeCount() + 1u;
  for (std::uint32_t steps = 0; steps < guard; ++steps) {
    const NodeId parent = snap_->topology(cur).parent;
    if (parent == kInvalidNode) {
      break;
    }
    ++depth;
    if (hideRoot_ && parent == root) {
      // The real root is not a row, so a direct child is depth 0 and `depth`
      // was just incremented to 1. Undo that.
      return depth - 1u;
    }
    if (parent == root) {
      return depth;
    }
    cur = parent;
  }
  return depth;
}

}  // namespace widetree
