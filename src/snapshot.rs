//! Immutable topology snapshot.

use crate::types::NodeId;
use std::sync::Arc;

/// 16 bytes equivalent. One record per node, indexed by NodeId.
#[derive(Debug, Clone, Copy, Default)]
pub struct NodeTopology {
    pub parent: NodeId,
    pub row_in_parent: u32,
    pub child_begin: u32,
    pub child_count: u32,
}

/// 16 bytes equivalent. Labels are slices of the snapshot string pool.
#[derive(Debug, Clone, Copy, Default)]
pub struct NodeInfo {
    pub label_begin: u32,
    pub label_length: u32,
    pub payload: u64,
}

/// Immutable topology. Published only after the builder has checked id range,
/// single parent, self-loops, cycles, reachability, and label bounds.
/// A node has one tree position.
#[derive(Debug)]
pub struct TreeSnapshot {
    id: u64,
    root: NodeId,
    topo: Vec<NodeTopology>,
    info: Vec<NodeInfo>,
    children: Vec<NodeId>,
    labels: String,
}

impl TreeSnapshot {
    pub(crate) fn new(
        id: u64,
        root: NodeId,
        topo: Vec<NodeTopology>,
        info: Vec<NodeInfo>,
        children: Vec<NodeId>,
        labels: String,
    ) -> Arc<Self> {
        Arc::new(Self {
            id,
            root,
            topo,
            info,
            children,
            labels,
        })
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn node_count(&self) -> u32 {
        self.topo.len() as u32
    }

    pub fn root(&self) -> NodeId {
        self.root
    }

    pub fn contains(&self, id: NodeId) -> bool {
        id < self.node_count()
    }

    pub fn topology(&self, id: NodeId) -> &NodeTopology {
        &self.topo[id as usize]
    }

    pub fn info(&self, id: NodeId) -> &NodeInfo {
        &self.info[id as usize]
    }

    pub fn label(&self, id: NodeId) -> &str {
        let n = &self.info[id as usize];
        let begin = n.label_begin as usize;
        let end = begin + n.label_length as usize;
        &self.labels[begin..end]
    }

    pub fn payload(&self, id: NodeId) -> u64 {
        self.info[id as usize].payload
    }

    pub fn child(&self, parent: NodeId, ordinal: u32) -> NodeId {
        let t = &self.topo[parent as usize];
        self.children[(t.child_begin + ordinal) as usize]
    }

    pub fn resident_bytes(&self) -> u64 {
        std::mem::size_of::<Self>() as u64
            + self.topo.len() as u64 * std::mem::size_of::<NodeTopology>() as u64
            + self.info.len() as u64 * std::mem::size_of::<NodeInfo>() as u64
            + self.children.len() as u64 * std::mem::size_of::<NodeId>() as u64
            + self.labels.len() as u64
    }
}
