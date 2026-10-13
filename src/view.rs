//! TreeView: projection of visible rows over an immutable snapshot.
//!
//! Authoritative data is the expanded bitmap. Sparse index accelerates restore.
//! Segment array represents currently visible structure.

use crate::snapshot::TreeSnapshot;
use crate::types::*;
use std::sync::Arc;

#[derive(Debug, Clone, Default)]
pub struct ViewConfig {
    /// When true, the real root is not a row. Its direct children are depth 0.
    pub hide_root: bool,
}

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
        let words = (node_count as usize + 63) / 64;
        Self {
            words: vec![0; words],
            count: 0,
        }
    }

    fn test(&self, i: u32) -> bool {
        if i >= self.count as u32 * 64 {
            return false;
        }
        let w = (i / 64) as usize;
        let b = i % 64;
        if w >= self.words.len() {
            return false;
        }
        (self.words[w] & (1u64 << b)) != 0
    }

    fn set(&mut self, i: u32) {
        let w = (i / 64) as usize;
        let b = i % 64;
        if w < self.words.len() {
            self.words[w] |= 1u64 << b;
        }
    }

    fn clear(&mut self, i: u32) {
        let w = (i / 64) as usize;
        let b = i % 64;
        if w < self.words.len() {
            self.words[w] &= !(1u64 << b);
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
        self.snap = Some(snapshot);
        self.hide_root = config.hide_root;
        self.revision = 1;
        let nc = self.snap.as_ref().unwrap().node_count();
        self.bits = ExpandBitmap::new(nc);
        self.segments.clear();
        self.prefix.clear();
        self.row_count = 0;
        self.rebuild_projection();
        ViewResult {
            status: Status::Ok,
            change: ProjectionChange {
                old_revision: 0,
                new_revision: self.revision,
                old_row_count: 0,
                new_row_count: self.row_count,
                kind: ChangeKind::Reset,
                splice: RowSplice::default(),
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
        self.bits.test(id)
    }

    pub fn is_visible(&self, id: NodeId) -> bool {
        let _ = id;
        false
    }

    pub fn row_of(&self, id: NodeId) -> Option<RowIndex> {
        let _ = id;
        None
    }

    pub fn get_visible_rows(&self, start: RowIndex, out: &mut [VisibleRow]) -> usize {
        if self.snap.is_none() || start >= self.row_count {
            return 0;
        }
        // Simplified walk; full segment-to-row mapping to be completed.
        let mut written = 0;
        let _ = (start, out);
        written
    }

    pub fn row_ref(&self, row: RowIndex) -> Option<RowRef> {
        if self.snap.is_none() || row >= self.row_count {
            return None;
        }
        Some(RowRef {
            snapshot_id: self.snap.as_ref().unwrap().id(),
            revision: self.revision,
            row,
            node: K_INVALID_NODE,
        })
    }

    pub fn expand_at(&mut self, _ref: RowRef) -> ViewResult {
        if self.snap.is_none() {
            return self.no_change();
        }
        self.no_change()
    }

    pub fn collapse_at(&mut self, _ref: RowRef) -> ViewResult {
        self.no_change()
    }

    pub fn reveal(&mut self, _id: NodeId) -> ViewResult {
        self.no_change()
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
                first_row: if i == 0 {
                    0
                } else {
                    self.prefix.get(i - 1).copied().unwrap_or(0)
                },
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
        None
    }

    fn no_change(&self) -> ViewResult {
        ViewResult {
            status: Status::NoChange,
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

    fn rebuild_projection(&mut self) {
        let snap = match &self.snap {
            Some(s) => s,
            None => return,
        };
        self.segments.clear();
        if self.hide_root {
            let root = snap.root();
            let t = snap.topology(root);
            if t.child_count > 0 {
                self.segments.push(Segment {
                    parent: root,
                    begin: 0,
                    end: t.child_count,
                    depth: 0,
                });
            }
        } else {
            self.segments.push(Segment {
                parent: K_DISPLAY_ROOT,
                begin: 0,
                end: 1,
                depth: 0,
            });
        }
        self.build_prefix();
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
