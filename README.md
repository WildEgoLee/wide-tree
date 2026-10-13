# wide-tree — Rust port (branch `rust/port`)

This branch contains a Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules.
- `TreeView` implements the observable v1 contract.
- **Incremental splice**: `expand_at`, `collapse_at` and `reveal` all use the incremental path (`spliceExpand` / `spliceCollapse`). Bits are set first on reveal so one splice emits the final subtree.
- Tests: `tests/smoke.rs` + `tests/acceptance.rs` (wide fan-out, hide-root, repeated expand/collapse, reveal). All pass under `cargo test`.

Sparse parent index is not yet present. The C++ code on `main` remains the performance reference for the optimized path.

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

- Snapshot is `Arc<TreeSnapshot>`.
- Expand preference lives in a bitmap owned by the view.
- Segment array is updated incrementally on expand/collapse/reveal; prefix is rebuilt from the change point.

## Next steps

1. Sparse parent index for faster restore on large expanded sets.
2. Larger acceptance tests (closer to 1M children).
3. Criterion benchmarks.

The original C++ sources stay on `main` for comparison.
