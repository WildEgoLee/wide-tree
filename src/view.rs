//! TreeView: projection of visible rows over an immutable snapshot.
//!
//! Authoritative data is the expanded bitmap.
//! Segment array + prefix represents the currently visible structure.
//!
//! This version implements the observable contract (reset, expand/collapse/reveal,
//! getVisibleRows, rowOf, isVisible, RowRef validity). Partial splice is
//! approximated by full re-projection for correctness; the sparse index
//! acceleration can be layered on later.

use crate::snapshot::TreeSnapshot;
use crate::types::*;
use std::sync::Arc;

#[derive(Debug, Clone, Default)]
pub struct ViewConfig {
    /// When true, the real root is not a row. Its direct children are depth 0.
    pub hide_root: bool,
}

#[derive(Debug, Clone)]
struct Segment {
    parent: NodeId,
    begin: u32,
    end: u32,
    depth: u32,
}

struct ExpandBitmap {
    words: Vec<u64>,
    count: u32,
}

impl ExpandBitmap {
    fn new(node_count: u32) -> Self {
        let words = ((node_count as usize) + 63) / 64;
        Self {
            words: vec![0; words],
            count: 0,
        }
    }

    fn test(&self, i: u32) -> bool {
        let w = (i / 64) as usize;
        if w >= self.words.len() {
            return false;
        }
        let b = i % 64;
        (self.words[w] & (1u64 << b)) != 0
    }

    fn set(&mut self, i: u32) {
        let w = (i / 64) as usize;
        if w >= self.words.len() {
            return;
        }
        let b = i % 64;
        let mask = 1u64 << b;
        if (self.words[w] & mask) == 0 {
            self.words[w] |= mask;
            self.count += 1;
        }
    }

    fn clear(&mut self, i: u32) {
        let w = (i / 64) as usize;
        if w >= self.words.len() {
            return;
        }
        let b = i % 64;
        let mask = 1u64 << b;
        if (self.words[w] & mask) != 0 {
            self.words[w] &= !mask;
            self.count = self.count.saturating_sub(1);
        }
    }

    fn bytes(&self) -> u64 {
        self.words.len() as u64 * 8
    }
}

/// One view over an immutable snapshot.
pub struct TreeView {
    snap: Option<Arc<TreeSnapshot>>,
    hide_root: bool,
    revision: u64,
    bits: ExpandBitmap,
    segments: Vec<Segment>,
    prefix: Vec<RowIndex>,
    row_count: RowCount,
}

impl Default for TreeView {
    fn default() -> Self {
        Self::new()
    }
}

impl TreeView {
    pub fn new() -> Self {
        Self {
            snap: None,
            hide_root: false,
            revision: 0,
            bits: ExpandBitmap::new(0),
            segments: Vec::new(),
            prefix: Vec::new(),
            row_count: 0,
        }
    }

    pub fn reset(&mut self, snapshot: Arc<TreeSnapshot>, config: ViewConfig) -> ViewResult {
        let old_rev = self.revision;
        let old_count = self.row_count;
        self.snap = Some(snapshot);
        self.hide_root = config.hide_root;
        let nc = self.snap.as_ref().unwrap().node_count();
        self.bits = ExpandBitmap::new(nc);
        self.rebuild_projection();
        self.revision = old_rev + 1;
        ViewResult {
            status: Status::Ok,
            change: ProjectionChange {
                old_revision: old_rev,
                new_revision: self.revision,
                old_row_count: old_count,
                new_row_count: self.row_count,
                kind: ChangeKind::Reset,
                splice: RowSplice {
                    first: 0,
                    removed: old_count,
                    inserted: self.row_count,
                },
                icon_rows: Vec::new(),
            },
        }
    }

    pub fn snapshot(&self) -> Option<&Arc<TreeSnapshot>> {
        self.snap.as_ref()
    }

    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn row_count(&self) -> RowCount {
        self.row_count
    }

    pub fn hide_root(&self) -> bool {
        self.hide_root
    }

    pub fn is_expanded(&self, id: NodeId) -> bool {
        let Some(snap) = &self.snap else {
            return false;
        };
        if !snap.contains(id) {
            return false;
        }
        self.bits.test(id)
    }

    pub fn is_visible(&self, id: NodeId) -> bool {
        let Some(snap) = &self.snap else {
            return false;
        };
        if !snap.contains(id) {
            return false;
        }
        let root = snap.root();
        if id == root {
            return !self.hide_root;
        }
        let mut cur = id;
        let guard = snap.node_count() + 1;
        for _ in 0..guard {
            let parent = snap.topology(cur).parent;
            if parent == K_INVALID_NODE || !snap.contains(parent) {
                return false;
            }
            if self.hide_root && parent == root {
                return true;
            }
            if !self.bits.test(parent) {
                return false;
            }
            if parent == root {
                return true;
            }
            cur = parent;
        }
        false
    }

    pub fn row_of(&self, id: NodeId) -> Option<RowIndex> {
        if !self.is_visible(id) {
            return None;
        }
        let snap = self.snap.as_ref()?;
        let (seg_parent, ordinal) = if !self.hide_root && id == snap.root() {
            (K_DISPLAY_ROOT, 0)
        } else if self.hide_root && snap.topology(id).parent == snap.root() {
            (K_DISPLAY_ROOT, snap.topology(id).row_in_parent)
        } else {
            (snap.topology(id).parent, snap.topology(id).row_in_parent)
        };
        for (i, seg) in self.segments.iter().enumerate() {
            if seg.parent == seg_parent && ordinal >= seg.begin && ordinal < seg.end {
                let base = if i == 0 { 0 } else { self.prefix[i - 1] };
                return Some(base + (ordinal - seg.begin) as RowIndex);
            }
        }
        None
    }

    pub fn get_visible_rows(&self, start: RowIndex, out: &mut [VisibleRow]) -> usize {
        let Some(snap) = &self.snap else {
            return 0;
        };
        if out.is_empty() || start >= self.row_count {
            return 0;
        }
        let (mut seg_idx, mut ordinal) = match self.locate(start) {
            Some(v) => v,
            None => return 0,
        };
        let mut written = 0;
        let mut row = start;
        while written < out.len() && seg_idx < self.segments.len() {
            let seg = &self.segments[seg_idx];
            while ordinal < seg.end && written < out.len() {
                let id = self.child_at(seg.parent, ordinal);
                let topo = snap.topology(id);
                out[written] = VisibleRow {
                    row,
                    id,
                    depth: seg.depth,
                    ordinal_in_parent: ordinal,
                    child_count: topo.child_count,
                    expanded: self.bits.test(id),
                    label: snap.label(id).to_string(),
                    payload: snap.payload(id),
                };
                written += 1;
                row += 1;
                ordinal += 1;
            }
            seg_idx += 1;
            if seg_idx < self.segments.len() {
                ordinal = self.segments[seg_idx].begin;
            }
        }
        written
    }

    pub fn row_ref(&self, row: RowIndex) -> Option<RowRef> {
        let snap = self.snap.as_ref()?;
        if row >= self.row_count {
            return None;
        }
        let (seg_idx, ordinal) = self.locate(row)?;
        let node = self.child_at(self.segments[seg_idx].parent, ordinal);
        Some(RowRef {
            snapshot_id: snap.id(),
            revision: self.revision,
            row,
            node,
        })
    }

    pub fn expand_at(&mut self, r: RowRef) -> ViewResult {
        let id = match self.check_ref(&r) {
            Ok(id) => id,
            Err(status) => return self.error(status),
        };
        let snap = self.snap.as_ref().unwrap();
        if snap.topology(id).child_count == 0 || self.bits.test(id) {
            return self.no_change();
        }
        let old_rev = self.revision;
        let old_count = self.row_count;
        self.bits.set(id);
        self.rebuild_projection();
        self.revision += 1;
        let inserted = self.row_count.saturating_sub(old_count);
        self.finish_splice(old_rev, old_count, r.row + 1, 0, inserted, r.row)
    }

    pub fn collapse_at(&mut self, r: RowRef) -> ViewResult {
        let id = match self.check_ref(&r) {
            Ok(id) => id,
            Err(status) => return self.error(status),
        };
        if !self.bits.test(id) {
            return self.no_change();
        }
        let old_rev = self.revision;
        let old_count = self.row_count;
        self.bits.clear(id);
        self.rebuild_projection();
        self.revision += 1;
        let removed = old_count.saturating_sub(self.row_count);
        self.finish_splice(old_rev, old_count, r.row + 1, removed, 0, r.row)
    }

    pub fn reveal(&mut self, id: NodeId) -> ViewResult {
        let Some(snap) = &self.snap else {
            return self.error(Status::NoSnapshot);
        };
        if !snap.contains(id) || (self.hide_root && id == snap.root()) {
            return self.error(Status::InvalidNode);
        }
        if self.is_visible(id) {
            return self.no_change();
        }
        let mut to_expand = Vec::new();
        let mut cur = id;
        let root = snap.root();
        let guard = snap.node_count() + 1;
        for _ in 0..guard {
            let parent = snap.topology(cur).parent;
            if parent == K_INVALID_NODE || !snap.contains(parent) {
                break;
            }
            if self.hide_root && parent == root {
                break;
            }
            if !self.bits.test(parent) {
                to_expand.push(parent);
            }
            if parent == root {
                break;
            }
            cur = parent;
        }
        if to_expand.is_empty() {
            return self.no_change();
        }
        let anchor = *to_expand.last().unwrap();
        let anchor_row = match self.row_of(anchor) {
            Some(r) => r,
            None => return self.error(Status::InvalidNode),
        };
        let old_rev = self.revision;
        let old_count = self.row_count;
        for &node in &to_expand {
            self.bits.set(node);
        }
        self.rebuild_projection();
        self.revision += 1;
        let inserted = self.row_count.saturating_sub(old_count);
        self.finish_splice(old_rev, old_count, anchor_row + 1, 0, inserted, anchor_row)
    }

    pub fn segments(&self) -> Vec<SegmentInfo> {
        self.segments
            .iter()
            .enumerate()
            .map(|(i, s)| SegmentInfo {
                parent: s.parent,
                begin: s.begin,
                end: s.end,
                depth: s.depth,
                first_row: if i == 0 { 0 } else { self.prefix[i - 1] },
            })
            .collect()
    }

    pub fn memory(&self) -> MemoryReport {
        MemoryReport {
            snapshot_bytes: self.snap.as_ref().map(|s| s.resident_bytes()).unwrap_or(0),
            bitmap_bytes: self.bits.bytes(),
            segment_bytes: (self.segments.len() * std::mem::size_of::<Segment>()) as u64,
            prefix_bytes: (self.prefix.len() * std::mem::size_of::<RowIndex>()) as u64,
            segment_count: self.segments.len() as u32,
            expanded_count: self.bits.count,
            ..Default::default()
        }
    }

    pub fn check_invariants(&self) -> Option<&'static str> {
        if self.snap.is_none() {
            return if self.segments.is_empty() && self.row_count == 0 {
                None
            } else {
                Some("projection without snapshot")
            };
        }
        if self.prefix.len() != self.segments.len() {
            return Some("prefix length");
        }
        let mut expect = 0u64;
        for (i, seg) in self.segments.iter().enumerate() {
            if seg.end <= seg.begin {
                return Some("empty segment");
            }
            let base = if i == 0 { 0 } else { self.prefix[i - 1] };
            if base != expect {
                return Some("prefix mismatch");
            }
            expect += (seg.end - seg.begin) as u64;
        }
        if expect != self.row_count {
            return Some("row count");
        }
        None
    }

    fn error(&self, status: Status) -> ViewResult {
        ViewResult {
            status,
            change: ProjectionChange {
                old_revision: self.revision,
                new_revision: self.revision,
                old_row_count: self.row_count,
                new_row_count: self.row_count,
                kind: ChangeKind::NoChange,
                ..Default::default()
            },
        }
    }

    fn no_change(&self) -> ViewResult {
        self.error(Status::NoChange)
    }

    fn finish_splice(
        &self,
        old_rev: u64,
        old_count: RowCount,
        first: RowIndex,
        removed: RowCount,
        inserted: RowCount,
        icon: RowIndex,
    ) -> ViewResult {
        ViewResult {
            status: Status::Ok,
            change: ProjectionChange {
                old_revision: old_rev,
                new_revision: self.revision,
                old_row_count: old_count,
                new_row_count: self.row_count,
                kind: ChangeKind::Splice,
                splice: RowSplice {
                    first,
                    removed,
                    inserted,
                },
                icon_rows: vec![icon],
            },
        }
    }

    fn check_ref(&self, r: &RowRef) -> Result<NodeId, Status> {
        let Some(snap) = &self.snap else {
            return Err(Status::NoSnapshot);
        };
        if r.snapshot_id != snap.id() {
            return Err(Status::StaleSnapshot);
        }
        if r.revision != self.revision {
            return Err(Status::StaleRowRef);
        }
        if r.row >= self.row_count {
            return Err(Status::InvalidRowRef);
        }
        let (seg_idx, ordinal) = self.locate(r.row).ok_or(Status::InvalidRowRef)?;
        let id = self.child_at(self.segments[seg_idx].parent, ordinal);
        if id != r.node {
            return Err(Status::InvalidRowRef);
        }
        Ok(id)
    }

    fn child_at(&self, segment_parent: NodeId, ordinal: u32) -> NodeId {
        let snap = self.snap.as_ref().unwrap();
        if segment_parent == K_DISPLAY_ROOT {
            if !self.hide_root {
                return snap.root();
            }
            return snap.child(snap.root(), ordinal);
        }
        snap.child(segment_parent, ordinal)
    }

    fn locate(&self, row: RowIndex) -> Option<(usize, u32)> {
        if row >= self.row_count || self.segments.is_empty() {
            return None;
        }
        let mut lo = 0usize;
        let mut hi = self.segments.len();
        while lo + 1 < hi {
            let mid = lo + (hi - lo) / 2;
            let base = if mid == 0 { 0 } else { self.prefix[mid - 1] };
            if base <= row {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        let seg = &self.segments[lo];
        let base = if lo == 0 { 0 } else { self.prefix[lo - 1] };
        let ordinal = seg.begin + (row - base) as u32;
        if ordinal >= seg.end {
            return None;
        }
        Some((lo, ordinal))
    }

    fn rebuild_projection(&mut self) {
        let Some(snap) = &self.snap else {
            return;
        };
        self.segments.clear();
        if self.hide_root {
            self.emit_expanded(snap.root(), 0);
        } else {
            self.segments.push(Segment {
                parent: K_DISPLAY_ROOT,
                begin: 0,
                end: 1,
                depth: 0,
            });
            if self.bits.test(snap.root()) {
                self.emit_expanded(snap.root(), 1);
            }
        }
        self.build_prefix();
    }

    fn emit_expanded(&mut self, parent: NodeId, child_depth: u32) {
        let child_count = {
            let snap = self.snap.as_ref().unwrap();
            snap.topology(parent).child_count
        };
        if child_count == 0 {
            return;
        }
        // Collect expanded (ordinal, child) pairs first to avoid holding borrows.
        let mut expanded = Vec::new();
        for ord in 0..child_count {
            let child = {
                let snap = self.snap.as_ref().unwrap();
                snap.child(parent, ord)
            };
            if self.bits.test(child) {
                expanded.push((ord, child));
            }
        }
        let mut begin = 0u32;
        for &(ord, child) in &expanded {
            if begin <= ord {
                self.segments.push(Segment {
                    parent,
                    begin,
                    end: ord + 1,
                    depth: child_depth,
                });
            }
            self.emit_expanded(child, child_depth + 1);
            begin = ord + 1;
        }
        if begin < child_count {
            self.segments.push(Segment {
                parent,
                begin,
                end: child_count,
                depth: child_depth,
            });
        }
    }

    fn build_prefix(&mut self) {
        self.prefix.clear();
        let mut acc = 0u64;
        for seg in &self.segments {
            acc += (seg.end - seg.begin) as u64;
            self.prefix.push(acc);
        }
        self.row_count = acc;
    }
}
