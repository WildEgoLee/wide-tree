//! Acceptance-style checks inspired by the C++ v1 contract.

use wide_tree::{TreeBuilder, TreeView, ViewConfig, Status};

#[test]
fn wide_fanout_expand_and_jump() {
    let n = 1001u32;
    let mut b = TreeBuilder::new(n);
    b.set_root(0);
    b.add_child_range(0, 1, 1000);
    for i in 0..n { b.set_label(i, &format!("n{i}")); }
    let snap = b.build().snapshot.expect("build");

    let mut view = TreeView::new();
    view.reset(snap, ViewConfig::default());
    assert_eq!(view.row_count(), 1);

    let root = view.row_ref(0).unwrap();
    let exp = view.expand_at(root);
    assert_eq!(exp.status, Status::Ok);
    assert_eq!(view.row_count(), 1001);

    let mut buf = vec![wide_tree::VisibleRow {
        row: 0, id: 0, depth: 0, ordinal_in_parent: 0,
        child_count: 0, expanded: false, label: String::new(), payload: 0,
    }; 10];
    let got = view.get_visible_rows(990, &mut buf);
    assert!(got > 0);
    assert_eq!(buf[0].id, 990);

    let root2 = view.row_ref(0).unwrap();
    let col = view.collapse_at(root2);
    assert_eq!(col.status, Status::Ok);
    assert_eq!(view.row_count(), 1);
    assert!(view.check_invariants().is_none());
}

#[test]
fn hide_root_mode() {
    let mut b = TreeBuilder::new(4);
    b.set_root(0);
    b.add_child_range(0, 1, 3);
    let snap = b.build().snapshot.unwrap();

    let mut view = TreeView::new();
    view.reset(snap, ViewConfig { hide_root: true });
    assert_eq!(view.row_count(), 3);
    assert!(view.is_visible(1));
    assert!(!view.is_visible(0));
}

#[test]
fn repeated_expand_collapse_stable() {
    let mut b = TreeBuilder::new(5);
    b.set_root(0);
    b.add_child_range(0, 1, 4);
    let snap = b.build().snapshot.unwrap();

    let mut view = TreeView::new();
    view.reset(snap, ViewConfig::default());

    for _ in 0..10 {
        let r = view.row_ref(0).expect("root row");
        view.expand_at(r);
        assert!(view.row_count() > 1);
        let r = view.row_ref(0).expect("root row");
        view.collapse_at(r);
        assert_eq!(view.row_count(), 1);
    }
    assert!(view.check_invariants().is_none());
}

#[test]
fn reveal_makes_deep_node_visible() {
    let mut b = TreeBuilder::new(3);
    b.set_root(0);
    b.add_edge(0, 1);
    b.add_edge(1, 2);
    let snap = b.build().snapshot.unwrap();

    let mut view = TreeView::new();
    view.reset(snap, ViewConfig::default());
    assert!(!view.is_visible(2));

    let res = view.reveal(2);
    assert_eq!(res.status, Status::Ok);
    assert!(view.is_visible(2));
    assert!(view.is_expanded(0));
    assert!(view.is_expanded(1));
    assert!(!view.is_expanded(2));
    assert!(view.check_invariants().is_none());
}
