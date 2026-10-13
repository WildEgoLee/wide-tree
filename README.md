# wide-tree — Rust port (branch `rust/port`)

This branch contains a Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules.
- `TreeView` implements the observable v1 contract.
- **Incremental splice**: `expand_at`, `collapse_at` and `reveal` use the incremental path.
- **Sparse index**: parent span table + ordered ordinals + open-addressed parent lookup (power-of-two capacity, linear probing). Rebuilt on structural changes to the span table. `collect_expanded` prefers the index.
- Tests: `tests/smoke.rs` + `tests/acceptance.rs`. All pass under `cargo test`.

Copy-on-write sharing of the index payload is not yet present. The C++ code on `main` remains the full performance reference.

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

- Snapshot is `Arc<TreeSnapshot>`.
- Expand preference lives in a bitmap owned by the view.
- Sparse index records which direct children of each parent are preferred expanded; open-addressed table gives near-constant parent lookup.
- Segment array is updated incrementally; prefix is rebuilt from the change point.

## Next steps

1. Larger acceptance tests (closer to 1M children).
2. Criterion benchmarks.
3. Optional: copy-on-write index payload (closer to C++).

The original C++ sources stay on `main` for comparison.
