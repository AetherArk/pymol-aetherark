# PyMOL Native Core Migration — Development Plan

> **Status:** Proposal / design sketch
> **Goal:** Transformative performance in load time, interactive selection/measurement,
> and large-scene rendering — *without* a big-bang rewrite. Strangler-fig migration of
> the compute and data cores to Rust behind stable C++ interfaces, keeping the Python
> API, file-format front-ends, and chemistry heuristics intact.

## Guiding principles

1. **Never break the seam.** Every phase ships behind an existing C++ interface. If a
   phase is abandoned, the C++ implementation still works. No phase blocks a release.
2. **Benchmark before and after.** Each module gets a criterion/Google-benchmark harness
   and a representative corpus (see "Benchmark corpus" below) *before* it is touched.
3. **One module at a time.** Migrate leaf modules first (fewest callers), work up the
   layer stack: `layer0` → `layer2` → `layer3`.
4. **Keep chemistry knowledge in place.** The accumulated edge-case logic (parsers'
   format quirks, `AtomInfoAssignParameters`, ss assignment, settings) is the crown
   jewel — port mechanically, do not "improve" during the port.

## Language / toolchain call

- **Rust** for compute core, atom store, and (later) renderer.
  - `rayon` for data-parallelism (replaces the OpenMP critical-section pattern).
  - `std::simd` / `wide` for distance kernels.
  - `cxx` for the C++↔Rust FFI bridge (typed, no hand-written `extern "C"` glue).
  - `wgpu` (phase 4) for a native + WASM renderer.
- **Not** Zig (thinner scientific ecosystem), **not** a managed language (GC + FFI
  overhead defeats the purpose), **not** a ground-up C++ rewrite (memory pain without the
  concurrency/safety upside).

## Benchmark corpus (build once, reuse every phase)

- Small: a single ~2k-atom PDB (e.g. a lysozyme).
- Medium: a ~100k-atom assembly (viral capsid subunit / ribosome fragment).
- Large: a ~1M-atom system.
- Trajectory: multi-thousand-frame CRD/DCD over a medium system (exercises the parser).
- Dense: a solvated box (exercises connectivity / neighbor search worst case).

Track: wall-clock load, `select`/`within` latency, `distance`/polar-contact build,
whole-scene frame time, and RSS.

---

## Phase 1 — Prove the FFI seam (weeks)

**Objective:** De-risk the entire toolchain (build system, ABI, CI, packaging) with a
small, reversible, easily benchmarked module *before* betting anything large.

**Target module: fixed-width float parsing** — the `ncopy`→`sscanf("%f")` pattern found
throughout the parsers:
- CRD/trajectory coords: `layer2/ObjectMolecule.cpp:1046`, `:1360`
- TOP coords: `:2927`
- ~25 more sites (see grep for `sscanf(cc, "%f"` in `layer2/ObjectMolecule.cpp`).

**Why this first:** self-contained, no state, trivially correct-by-comparison, and it
delivers a real win (10–50× on the parse kernel) so the POC is not throwaway.

**Deliverables:**
1. A `native-core/` Rust crate with a `cxx` bridge exposing:
   ```rust
   // parse a fixed-width float field in place; no locale, no format-string reparse
   fn parse_fixed_f32(bytes: &[u8], offset: usize, width: usize) -> f32;
   // batch variant for a whole coordinate line
   fn parse_coord_line(bytes: &[u8], stride: usize, out: &mut [f32]) -> usize;
   ```
2. CMake integration: `corrosion` or a custom command driving `cargo build`, linking the
   staticlib into the existing build. Wire into `setup.py` and the GitHub Actions matrix
   (Linux/macOS/Windows — note this repo builds on Windows).
3. Replace the coordinate `sscanf` sites behind a compile-time flag
   (`-DPYMOL_NATIVE_CORE`) so the C++ path remains the fallback.
4. Differential test: parse the entire benchmark corpus both ways, assert bit-identical
   (or ULP-bounded) coordinates.

**Exit criteria:** CI green on all three platforms; parser benchmark shows the win;
toggling the flag off restores the old path. **This phase decides whether the whole plan
is viable** — if the toolchain fight is too costly on Windows, we learn it here for weeks
of cost, not years.

---

## Phase 2 — Rust compute core (quarters)

**Objective:** Move the numerical/logic hot paths to Rust behind their current C++
signatures. These are self-contained with clean inputs/outputs.

Migrate in dependency order (leaf-first):

### 2a. Spatial hash — `layer0/Map.cpp`
The neighbor-search substrate for connectivity, `within`, distances, surfaces, ramps.
- Port `MapType` construction + the express-list neighbor iteration (`MapEIter`).
- **Value beyond speed:** replaces the raw pointer arithmetic through express tables
  (the file is littered with `"this doesn't look safe, but it is"` comments — a standing
  maintenance liability) with bounds-checked, safe Rust.
- Keep `MapEIter`'s C++ iterator interface so callers (`ObjectMoleculeConnect`,
  `SelectorGetInterstateVector`, `SelectorMapMaskVDW`) are unchanged.

### 2b. Connectivity — `ObjectMoleculeConnect` (`layer2/ObjectMolecule2.cpp:3701`)
- Currently OpenMP-parallel but funnels every discovered bond through
  `#pragma omp critical` (`:3805`). Replace with `rayon` + **per-thread bond vectors
  merged after the parallel region** — removes the contention entirely and makes the
  `violations`/`cnt` bookkeeping cleaner.
- The chemistry predicate `is_distance_bonded` and PDB residue heuristics port verbatim.

### 2c. Selection evaluation — `layer3/Selector.cpp`
- `SelectorGetInterstateVector` (`:4704`) and `SelectorMapMaskVDW` (`:4755`): the
  `distance` / `get_distance` / polar-contact / VDW-mask engines. Also fixes the
  oversized scratch buffers (allocate to selection size, not `table_size`).
- Defer `SelectorUpdateTable` itself (`:7066`) to phase 3 — it's coupled to the store.

### 2d. Parsers (remainder)
- Complete the parser migration started in phase 1 (full record parsing, not just floats).

**Deliverables per module:** Rust implementation, `cxx` bridge, differential test against
the C++ path over the corpus, benchmark delta, flag-gated cutover.

**Exit criteria:** compute-core modules pass differential tests; measurable wins on
connectivity (dense corpus) and distance/select (large corpus); OpenMP critical section
gone.

---

## Phase 3 — Columnar (SoA) atom store (quarters)

**Objective:** Replace the array-of-`AtomInfoType`-structs layout with a
struct-of-arrays / dataframe-style store. This is the **foundational** data change that
everything above ultimately sits on.

**Current state:** `layer2/AtomInfo.h` — `AtomInfoType` is ~120–150 B/atom (many 4-byte
fields: `color`, `id`, `flags`, `temp1` [explicitly marked "kludge fields - to remove"],
`unique_id`, `rank`, `prop_id`, `priority`, `customType`, `elec_radius`, `selEntry`, 7×
`lexidx_t`). Every full-table scan strides through fat structs, pulling cache lines full
of fields it doesn't need.

**Target:** parallel column arrays (hot columns dense and contiguous; cold columns —
`anisou` [already lazy], `elec_radius`, custom settings — in side tables keyed by
`unique_id`).

**Payoffs:**
- Cache- and SIMD-friendly scans → `SelectorUpdateTable` (`Selector.cpp:7096`, full
  realloc + fill on nearly every selection op) and selection eval vectorize almost for
  free. Also add the dirty-flag / generation-counter so the table only rebuilds when an
  object's atom count or relevant state actually changed.
- **Zero-copy NumPy views** of coordinates and per-atom columns for the Python API —
  a large win for scripting workflows on its own (no per-call copy).
- Smaller RSS; drop the self-described kludge field `temp1`, audit `prop_id`/`customType`.

**Sequencing:** do this *after* 2a–2c so the Rust compute core is the natural first
consumer of the columnar store, and the FFI/build story is already proven.

**Risk:** highest coupling of any phase — `AtomInfoType` is referenced pervasively. Land
it behind accessor methods first (migrate call sites to `ai.color()` / `store.color(a)`
style accessors *while still struct-backed*), then flip the backing store to columns.
The accessor migration is mechanical and shippable independently of the flip.

**Exit criteria:** column store passes all existing tests; selection/scan benchmarks
improve on large corpus; NumPy zero-copy path exposed and tested; RSS down on the 1M-atom
corpus.

---

## Phase 4 — GPU-driven renderer (years) — NOTED, NOT SCHEDULED

> Highest performance ceiling for "tons of molecules," largest and riskiest effort.
> **Deliberately last** — only after the data layer is modern and the Rust + FFI
> integration pattern is battle-tested through phases 1–3.

**Sketch (for future planning, not commitment):**
- Current path: CGO → VBOs with some impostor shaders (`layer2/RepSphere.cpp` and
  friends) — good, but still largely CPU-orchestrated per object.
- Target: GPU-resident, GPU-driven renderer — persistent buffers, instanced impostor
  spheres/cylinders, compute-shader culling, indirect draws — moving whole-scene
  rendering of millions of atoms from CPU-bound to GPU-bound (plausibly 10–100× on large
  scenes).
- `wgpu` (Rust) pairs with the Rust core and runs native + WASM from one codebase,
  opening browser deployment as a bonus.
- **Prerequisite investigation (do during phase 3):** audit `RepInvalidate` granularity —
  does a recolor/visibility change trigger a full geometry rebuild, or a cheap VBO subdata
  update? If the former, that's a large win available *within the current renderer* and
  worth doing regardless of phase 4.

---

## Timeline (rough)

| Phase | Scope | Effort | Ships value independently? |
|------|-------|--------|----------------------------|
| 1 | Float parser + FFI seam | weeks | Yes — faster loads, proves toolchain |
| 2 | Rust compute core (Map, connect, select, parse) | quarters | Yes — per module |
| 3 | Columnar atom store + NumPy zero-copy | quarters | Yes — accessor migration, then flip |
| 4 | GPU-driven renderer | years | Yes — but earned, not started first |

**Transformative-per-effort winner:** Phases 1–3 together (a Rust SoA compute core). The
GPU renderer is transformative but should be earned. You can stop after any phase and be
strictly ahead.

## Open questions to resolve before phase 1

- Windows build story with `cargo` in CI — the repo's primary dev/build target is
  Windows; validate `corrosion`/MSVC linking early (this is the likeliest toolchain
  blocker).
- MSRV / Rust edition pin and vendoring policy for reproducible builds.
- Where the FFI-boundary ownership lives (who allocates coordinate buffers — C++ arena or
  Rust `Vec` handed across).
- Differential-test harness: exact vs. ULP-bounded float comparison policy.
