//! Single-shot builder for immutable TreeSnapshot.

use crate::snapshot::{NodeInfo, NodeTopology, TreeSnapshot};
use crate::types::{BuildError, NodeId, K_INVALID_NODE, K_MAX_NODE_COUNT};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

static NEXT_SNAPSHOT_ID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug)]
pub struct BuildResult {
    pub snapshot: Option<Arc<TreeSnapshot>>,
    pub error: BuildError,
}

impl BuildResult {
    pub fn is_ok(&self) -> bool {
        self.snapshot.is_some()
    }
}

struct FillOp {
    parent: NodeId,
    first_child: NodeId,
    count: u32,
}

/// Single-shot builder. v1 publishes one root.
pub struct TreeBuilder {
    node_count: u32,
    has_root: bool,
    root: NodeId,
    failed: bool,
    error: BuildError,
    child_count: Vec<u32>,
    info: Vec<NodeInfo>,
    labels: String,
    ops: Vec<FillOp>,
}

impl TreeBuilder {
    pub fn new(node_count: u32) -> Self {
        if node_count == 0 || node_count > K_MAX_NODE_COUNT {
            return Self {
                node_count: 0,
                has_root: false,
                root: K_INVALID_NODE,
                failed: true,
                error: BuildError {
                    code: "bad_node_count".into(),
                    detail: "node count must be in [1, kMaxNodeCount]".into(),
                },
                child_count: Vec::new(),
                info: Vec::new(),
                labels: String::new(),
                ops: Vec::new(),
            };
        }
        Self {
            node_count,
            has_root: false,
            root: K_INVALID_NODE,
            failed: false,
            error: BuildError::default(),
            child_count: vec![0; node_count as usize],
            info: vec![NodeInfo::default(); node_count as usize],
            labels: String::new(),
            ops: Vec::new(),
        }
    }

    fn fail(&mut self, code: &str, detail: String) {
        if !self.failed {
            self.failed = true;
            self.error = BuildError {
                code: code.into(),
                detail,
            };
        }
    }

    pub fn set_root(&mut self, root: NodeId) {
        if self.failed {
            return;
        }
        if root >= self.node_count {
            self.fail("bad_root", "root is outside the node id range".into());
            return;
        }
        if self.has_root {
            self.fail("multiple_roots", "v1 publishes exactly one root".into());
            return;
        }
        self.has_root = true;
        self.root = root;
    }

    pub fn set_label(&mut self, id: NodeId, label: &str) {
        if self.failed {
            return;
        }
        if id >= self.node_count {
            self.fail("bad_node", "setLabel id is outside the node id range".into());
            return;
        }
        let begin = self.labels.len() as u64;
        if begin > 0xFFFF_FFFF || begin + label.len() as u64 > 0xFFFF_FFFF {
            self.fail("overflow", "string pool exceeds uint32 offsets".into());
            return;
        }
        self.labels.push_str(label);
        self.info[id as usize].label_begin = begin as u32;
        self.info[id as usize].label_length = label.len() as u32;
    }

    pub fn set_payload(&mut self, id: NodeId, payload: u64) {
        if self.failed {
            return;
        }
        if id >= self.node_count {
            self.fail("bad_node", "setPayload id is outside the node id range".into());
            return;
        }
        self.info[id as usize].payload = payload;
    }

    pub fn add_edge(&mut self, parent: NodeId, child: NodeId) {
        self.add_child_range(parent, child, 1);
    }

    pub fn add_child_range(&mut self, parent: NodeId, first_child: NodeId, count: u32) {
        if self.failed {
            return;
        }
        if count == 0 {
            self.fail("empty_range", "child range count must be non-zero".into());
            return;
        }
        if parent >= self.node_count {
            self.fail("bad_parent", "parent is outside the node id range".into());
            return;
        }
        let last = first_child as u64 + count as u64 - 1;
        if last >= self.node_count as u64 {
            self.fail("bad_child", "child range leaves the node id range".into());
            return;
        }
        let sum = self.child_count[parent as usize] as u64 + count as u64;
        if sum > 0xFFFF_FFFF {
            self.fail("overflow", "parent child count exceeds uint32".into());
            return;
        }
        self.child_count[parent as usize] = sum as u32;
        self.ops.push(FillOp {
            parent,
            first_child,
            count,
        });
    }

    pub fn build(mut self) -> BuildResult {
        if self.failed {
            return BuildResult {
                snapshot: None,
                error: self.error,
            };
        }
        if !self.has_root {
            self.fail("missing_root", "setRoot was not called".into());
            return BuildResult {
                snapshot: None,
                error: self.error,
            };
        }

        let mut edge_total: u64 = 0;
        for &c in &self.child_count {
            edge_total += c as u64;
            if edge_total > 0xFFFF_FFFF {
                self.fail("overflow", "total child slots exceed uint32".into());
                return BuildResult {
                    snapshot: None,
                    error: self.error,
                };
            }
        }

        let mut topo = vec![NodeTopology::default(); self.node_count as usize];
        let mut children = vec![K_INVALID_NODE; edge_total as usize];
        let mut cursor = vec![0u32; self.node_count as usize];
        let mut offset = 0u32;
        for i in 0..self.node_count {
            topo[i as usize].parent = K_INVALID_NODE;
            topo[i as usize].row_in_parent = 0;
            topo[i as usize].child_begin = offset;
            topo[i as usize].child_count = self.child_count[i as usize];
            cursor[i as usize] = offset;
            offset += self.child_count[i as usize];
        }

        let mut assigned = vec![false; self.node_count as usize];
        assigned[self.root as usize] = true;

        for op in &self.ops {
            for k in 0..op.count {
                let child = op.first_child + k;
                if child == op.parent {
                    self.fail("self_loop", "a node cannot parent itself".into());
                    return BuildResult {
                        snapshot: None,
                        error: self.error,
                    };
                }
                if child == self.root {
                    self.fail("root_has_parent", "the root cannot be someone's child".into());
                    return BuildResult {
                        snapshot: None,
                        error: self.error,
                    };
                }
                if assigned[child as usize] {
                    self.fail(
                        "multiple_parents",
                        "a node is owned by more than one parent".into(),
                    );
                    return BuildResult {
                        snapshot: None,
                        error: self.error,
                    };
                }
                let slot = cursor[op.parent as usize];
                cursor[op.parent as usize] += 1;
                let ordinal = slot - topo[op.parent as usize].child_begin;
                children[slot as usize] = child;
                topo[child as usize].parent = op.parent;
                topo[child as usize].row_in_parent = ordinal;
                assigned[child as usize] = true;
            }
        }

        // Cycle detection and reachability
        let mut color = vec![0u8; self.node_count as usize];
        for start in 0..self.node_count {
            if color[start as usize] == 2 {
                continue;
            }
            let mut path = Vec::new();
            let mut x = start;
            let mut cycle = false;
            loop {
                if x == self.root {
                    color[self.root as usize] = 2;
                    break;
                }
                if color[x as usize] == 2 {
                    break;
                }
                if color[x as usize] == 1 {
                    cycle = true;
                    break;
                }
                if !assigned[x as usize] || topo[x as usize].parent == K_INVALID_NODE {
                    break;
                }
                color[x as usize] = 1;
                path.push(x);
                x = topo[x as usize].parent;
            }
            if cycle {
                self.fail("cycle", "parent pointers contain a cycle".into());
                return BuildResult {
                    snapshot: None,
                    error: self.error,
                };
            }
            for id in path {
                color[id as usize] = 2;
            }
        }

        let mut seen = vec![false; self.node_count as usize];
        let mut walk = vec![self.root];
        seen[self.root as usize] = true;
        let mut visited = 0u32;
        while let Some(id) = walk.pop() {
            visited += 1;
            let t = &topo[id as usize];
            for i in 0..t.child_count {
                let child = children[(t.child_begin + i) as usize];
                if child >= self.node_count || seen[child as usize] {
                    self.fail("cycle", "child walk revisited a node".into());
                    return BuildResult {
                        snapshot: None,
                        error: self.error,
                    };
                }
                seen[child as usize] = true;
                walk.push(child);
            }
        }
        if visited != self.node_count {
            self.fail("unreachable", "not every node is reachable from the root".into());
            return BuildResult {
                snapshot: None,
                error: self.error,
            };
        }

        for i in 0..self.node_count {
            let n = &self.info[i as usize];
            let end = n.label_begin as u64 + n.label_length as u64;
            if end > self.labels.len() as u64 {
                self.fail("bad_label", "label slice is outside the string pool".into());
                return BuildResult {
                    snapshot: None,
                    error: self.error,
                };
            }
        }

        let id = NEXT_SNAPSHOT_ID.fetch_add(1, Ordering::Relaxed);
        let snapshot = TreeSnapshot::new(
            id,
            self.root,
            topo,
            self.info,
            children,
            self.labels,
        );

        BuildResult {
            snapshot: Some(snapshot),
            error: BuildError::default(),
        }
    }
}
