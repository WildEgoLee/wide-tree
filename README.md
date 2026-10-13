# wide-tree — Rust port (branch `rust/port`)

This branch contains a Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules (id range, single parent, cycles, reachability, label bounds).
- `TreeView` implements the observable v1 contract:
  - `reset`, `expand_at`, `collapse_at`, `reveal`
  - `get_visible_rows`, `row_of`, `is_visible`, `is_expanded`, `row_ref`
  - `RowRef` staleness checks
  - Segment array + prefix rebuilt from the expand bitmap (full re-projection for correctness)
- Basic smoke tests in `tests/smoke.rs`.

Partial splice (only updating the affected suffix of the segment array) and the sparse parent index from the C++ implementation are not yet present. Full re-projection is correct but not the asymptotic optimum for large S. The C++ code on `main` remains the performance reference.

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

- Snapshot is `Arc<TreeSnapshot>` (shared, immutable).
- Expand preference lives in a bitmap owned by the view.
- Segment array and prefix are rebuilt on expand/collapse/reveal to match the contract's emission order.

## Next steps

1. Incremental segment splice (`spliceExpand` / `spliceCollapse`) to avoid full rebuilds.
2. Sparse parent index for faster restore on large expanded sets.
3. Acceptance tests matching the C++ suite (1M children, segment stability, jump to row 800000).
4. Criterion benchmarks.

The original C++ sources stay on `main` for comparison.
