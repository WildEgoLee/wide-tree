# wide-tree — Rust port (branch `rust/port`)

This branch contains an initial Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules (id range, single parent, cycles, reachability, label bounds).
- `TreeView` has the public surface and data layout skeleton (expanded bitmap, segments, prefix).  
  Full segment-splice expand/collapse and sparse index from the C++ `view.cpp` are not yet complete; the methods currently return `NoChange` or placeholders.  
  The C++ implementation remains the reference for the exact projection algorithm.

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

The ownership model of Rust maps cleanly onto the immutable snapshot + mutable projection design:

- Snapshot is `Arc<TreeSnapshot>` (shared, immutable).
- Expand preference lives in a bitmap owned by the view.
- Segment array and prefix are rebuilt or spliced on expand/collapse.

Next steps for a complete port:

1. Faithful translation of `project`, `spliceExpand`, `spliceCollapse`, and the sparse parent index.
2. Property-based tests matching the C++ acceptance suite (1M children, segment stability, stale `RowRef`).
3. Criterion benchmarks corresponding to `bench/bench_main.cpp`.

The original C++ sources stay on `main` for comparison.

Original C++ README content is preserved in git history.
