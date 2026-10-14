# wide-tree — Rust port (branch `rust/port`)

This branch contains a Rust port of the immutable ultra-wide tree visible-row projection.

## Status

- Core types, `TreeSnapshot`, and `TreeBuilder` are ported with the same validation rules.
- `TreeView` implements the observable v1 contract.
- **Incremental splice**: `expand_at`, `collapse_at` and `reveal` use the incremental path.
- **Sparse index**: parent span table + ordered ordinals + open-addressed parent lookup.
- **Copy-on-write**: index payload is held in `Arc`. First mutation unshares via `write_index` if the strong count is greater than 1.
- Tests: `tests/smoke.rs` + `tests/acceptance.rs`. All pass under `cargo test`.

The C++ code on `main` remains the full performance reference.

## Build & test

```sh
cargo test
cargo build --release
```

## Design notes

- Snapshot is `Arc<TreeSnapshot>`.
- Expand preference lives in a bitmap owned by the view.
- Sparse index is `Arc<ExpandIndex>`; unmodified parents share the payload until the first edit.
- Segment array is updated incrementally; prefix is rebuilt from the change point.

## Next steps

1. Larger acceptance tests (closer to 1M children).
2. Criterion benchmarks.

The original C++ sources stay on `main` for comparison.
