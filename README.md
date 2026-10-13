# wide-tree — Rust port (branch `rust/port`)

This branch contains a Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules.
- `TreeView` implements the observable v1 contract.
- **Incremental splice**: `expand_at` and `collapse_at` now use `spliceExpand` / `spliceCollapse` (split host segment, emit/remove subtree, merge adjacent runs). `reveal` still falls back to full rebuild.
- Tests: `tests/smoke.rs` + `tests/acceptance.rs` (wide fan-out, hide-root, repeated expand/collapse stability). All pass under `cargo test`.

Sparse parent index is not yet present. The C++ code on `main` remains the performance reference for the optimized path.

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

- Snapshot is `Arc<TreeSnapshot>`.
- Expand preference lives in a bitmap owned by the view.
- Segment array is updated incrementally on expand/collapse; prefix is rebuilt from the change point.

## Next steps

1. Make `reveal` also incremental (or share the same splice path).
2. Sparse parent index for faster restore on large expanded sets.
3. Larger acceptance tests (closer to 1M children).
4. Criterion benchmarks.

The original C++ sources stay on `main` for comparison.
