//! Basic smoke tests for the Rust port.

use wide_tree::{TreeBuilder, TreeView, ViewConfig};

#[test]
fn builder_rejects_bad_count() {
    let b = TreeBuilder::new(0);
    let r = b.build();
    assert!(!r.is_ok());
    assert_eq!(r.error.code, "bad_node_count");
}

#[test]
fn simple_tree_expand_collapse() {
    // root=0, children 1,2,3 ; node 1 has child 4
    let mut b = TreeBuilder::new(5);
    b.set_root(0);
    b.add_child_range(0, 1, 3); // 1,2,3
    b.add_edge(1, 4);
    b.set_label(0, "root");
    b.set_label(1, "a");
    b.set_label(4, "a1");
    let built = b.build();
    assert!(built.is_ok(), "{:?}", built.error);
    let snap = built.snapshot.unwrap();

    let mut view = TreeView::new();
    let res = view.reset(snap.clone(), ViewConfig::default());
    assert_eq!(res.status, wide_tree::Status::Ok);
    assert_eq!(view.row_count(), 1); // only root

    let root_ref = view.row_ref(0).unwrap();
    assert_eq!(root_ref.node, 0);

    let exp = view.expand_at(root_ref);
    assert_eq!(exp.status, wide_tree::Status::Ok);
    assert!(view.row_count() > 1);
    assert!(view.is_expanded(0));

    // collapse
    let root_ref2 = view.row_ref(0).unwrap();
    let col = view.collapse_at(root_ref2);
    assert_eq!(col.status, wide_tree::Status::Ok);
    assert_eq!(view.row_count(), 1);
    assert!(!view.is_expanded(0));
}

#[test]
fn stale_row_ref_rejected() {
    let mut b = TreeBuilder::new(2);
    b.set_root(0);
    b.add_edge(0, 1);
    let snap = b.build().snapshot.unwrap();
    let mut view = TreeView::new();
    view.reset(snap, ViewConfig::default());
    let r = view.row_ref(0).unwrap();
    // expand changes revision
    let _ = view.expand_at(r);
    let stale = view.expand_at(r); // same ref, now stale
    assert_eq!(stale.status, wide_tree::Status::StaleRowRef);
}
