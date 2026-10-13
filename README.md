# wide-tree — Rust port (branch `rust/port`)

This branch contains a Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules.
- `TreeView` implements the observable v1 contract.
- **Incremental splice**: `expand_at`, `collapse_at` and `reveal` use the incremental path.
- **Sparse index**: parent span table + ordered ordinals. Maintained on expand/collapse/reveal. `collect_expanded` prefers the index when present so restore walks only expanded children.
- Tests: `tests/smoke.rs` + `tests/acceptance.rs` (including sparse-index population). All pass under `cargo test`.

The C++ code on `main` remains the performance reference for the fully optimized path (open-addressed hash, copy-on-write index payload).

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

- Snapshot is `Arc<TreeSnapshot>`.
- Expand preference lives in a bitmap owned by the view.
- Sparse index records which direct children of each parent are preferred expanded.
- Segment array is updated incrementally; prefix is rebuilt from the change point.

## Next steps

1. Larger acceptance tests (closer to 1M children).
2. Criterion benchmarks.
3. Optional: open-addressed parent lookup and copy-on-write index payload (closer to C++).

The original C++ sources stay on `main` for comparison.
