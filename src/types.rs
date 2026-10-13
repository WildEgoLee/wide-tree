//! Core types mirroring the C++ v1 contract.

use std::fmt;

pub type NodeId = u32;
pub type RowIndex = u64;
pub type RowCount = u64;

/// Not a real NodeId. Display-root segments use kDisplayRoot as their parent.
pub const K_INVALID_NODE: NodeId = 0xFFFF_FFFF;
pub const K_DISPLAY_ROOT: NodeId = 0xFFFF_FFFE;
/// Builder rejects counts that would collide with the sentinels above.
pub const K_MAX_NODE_COUNT: NodeId = 0xFFFF_FFFD;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Status {
    Ok = 0,
    NoChange,
    NoSnapshot,
    InvalidNode,
    InvalidRowRef,
    StaleRowRef,
    StaleSnapshot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u8)]
pub enum ChangeKind {
    #[default]
    NoChange = 0,
    Splice,
    Reset,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct RowSplice {
    pub first: RowIndex,
    pub removed: RowCount,
    pub inserted: RowCount,
}

/// Returned to the caller. TreeView does not invoke user callbacks.
#[derive(Debug, Clone, Default)]
pub struct ProjectionChange {
    pub old_revision: u64,
    pub new_revision: u64,
    pub old_row_count: RowCount,
    pub new_row_count: RowCount,
    pub kind: ChangeKind,
    pub splice: RowSplice,
    pub icon_rows: Vec<RowIndex>,
}

#[derive(Debug, Clone)]
pub struct ViewResult {
    pub status: Status,
    pub change: ProjectionChange,
}

/// Identifies a visible row. Invalid after the snapshot id or the projection revision changes.
#[derive(Debug, Clone, Copy)]
pub struct RowRef {
    pub snapshot_id: u64,
    pub revision: u64,
    pub row: RowIndex,
    pub node: NodeId,
}

impl Default for RowRef {
    fn default() -> Self {
        Self {
            snapshot_id: 0,
            revision: 0,
            row: 0,
            node: K_INVALID_NODE,
        }
    }
}

#[derive(Debug, Clone)]
pub struct VisibleRow {
    pub row: RowIndex,
    pub id: NodeId,
    pub depth: u32,
    pub ordinal_in_parent: u32,
    pub child_count: u32,
    pub expanded: bool,
    pub label: String, // owned for simplicity in port; original used string_view into snapshot
    pub payload: u64,
}

#[derive(Debug, Clone)]
pub struct SegmentInfo {
    pub parent: NodeId,
    pub begin: u32,
    pub end: u32,
    pub depth: u32,
    pub first_row: RowIndex,
}

#[derive(Debug, Clone, Default)]
pub struct MemoryReport {
    pub snapshot_bytes: u64,
    pub bitmap_bytes: u64,
    pub index_payload_bytes: u64,
    pub index_node_estimate_bytes: u64,
    pub segment_bytes: u64,
    pub prefix_bytes: u64,
    pub segment_count: u32,
    pub indexed_parent_count: u32,
    pub expanded_count: u32,
}

#[derive(Debug, Clone, Default)]
pub struct BuildError {
    pub code: String,
    pub detail: String,
}

impl fmt::Display for BuildError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code, self.detail)
    }
}
