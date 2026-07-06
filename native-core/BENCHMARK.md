# Phase 1 benchmark — float parsing

**What is measured:** the coordinate-field float-parse kernel that the migrated
call sites use — `native parse_f32` (Rust, via the cxx shim exactly as
`NativeParseFloat()` calls it, including `strlen` + UTF-8 validation + the FFI
call) vs the original `sscanf(cc, "%f")`. Same NUL-terminated 8-wide coordinate
fields (`%8.3f`), checksum-verified identical results.

**Machine:** Windows 11, MSVC 2019 BuildTools (`cl /O2 /MD`), Rust 1.96 release
(`opt-level=3`, LTO). Reproduce with `native-core/verify/build_and_bench.cmd`.

## Result (median of 5 reps, three independent runs)

| Path | ns / field | M fields / s |
|------|-----------:|-------------:|
| `sscanf("%f")` (original) | ~165 | ~6.0 |
| `native parse_f32` (this change) | ~46 | ~21.5 |
| **Speedup** | **~3.5×** | **~72% faster** |

Runs: 3.55× / 3.61× / 3.52× — variance is noise.

## What this means end-to-end

Each atom contributes 3 coordinate fields. The kernel saves ~119 ns/field
(~357 ns/atom-worth-of-coordinates). Translating to the **coordinate-parsing
portion** of a load:

| Workload | Coordinate fields | `sscanf` | native | saved |
|----------|------------------:|---------:|-------:|------:|
| 1M-atom model (single) | 3.0 M | ~0.50 s | ~0.14 s | ~0.36 s |
| 100k atoms × 1,000 frames | 3.0×10⁸ | ~50 s | ~14 s | ~36 s |
| 100k atoms × 10,000 frames | 3.0×10⁹ | ~495 s (8.3 min) | ~138 s (2.3 min) | ~357 s (~6 min) |

These are **parse-kernel times**, not whole-file load times. For Amber CRD/RST
trajectories (the migrated parsers), which are almost entirely coordinate
columns, the kernel is a large share of load time. For PDB, per-atom record
construction is a bigger share, so the whole-file share is smaller — the kernel
speedup is real either way; its weight depends on format.

## Why it's faster

`sscanf("%f")` re-parses its format string and consults the C locale on every
single call. `parse_f32` does none of that — it skips whitespace, takes the
leading token, and uses Rust's correctly-rounded float parser.

## Headroom (not yet taken)

The ~46 ns includes per-call `strlen` + UTF-8 validation + slice construction in
the shim. A Phase-2 width-aware API (`parse_fixed_f32(bytes, offset, width)` from
the plan) would drop the `strlen` and validation and batch fields per line,
pushing this further. The 3.5× here is the conservative, as-shipped number.
