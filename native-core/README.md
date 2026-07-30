# native-core (Phase 1)

Proof-of-concept for the [native-core migration plan](../docs/DEVELOPMENT_PLAN_native_core.md).
Phase 1's job is to **prove the Rust ↔ C++ (windows-msvc) build seam** on the
smallest useful module: a fast, locale-free fixed-width float parser that
replaces the `ncopy(cc, p, n)` + `sscanf(cc, "%f", &x)` pattern in the coordinate
parsers.

## What's here

| Path | Purpose |
|------|---------|
| `src/lib.rs` | `parse_f32` + the `#[cxx::bridge]` FFI surface, with Rust unit tests. |
| `../layer0/NativeParse.h` | The single C++ call-site shim. Dispatches to Rust when `PYMOL_NATIVE_CORE` is defined, else falls back to `sscanf`. |
| `verify/driver.cpp` | Standalone MSVC differential test: asserts `parse_f32` is bit-identical to `sscanf("%f")` **and** that the FFI links. |
| `verify/build_and_verify.cmd` | Builds the crate, runs `cargo test`, generates the cxx glue, compiles+links+runs the driver with MSVC 2019. |

## Verify the seam (no cmake / no full PyMOL build required)

```cmd
native-core\verify\build_and_verify.cmd
```

Expected tail:

```
checked=... failures=0
PASS: native parse_f32 is bit-identical to sscanf and links under MSVC.
```

## The contract

`parse_f32(field) -> (bool, value)` mirrors `sscanf(cc, "%f", out) == 1`:

* skips leading ASCII whitespace,
* parses the leading whitespace-delimited token as a float,
* returns `true` and writes `out` on success; `false` leaves `out` unchanged.

Fixed-width columns hold exactly one number per buffer, so "leading token" is
`sscanf`-equivalent on the inputs these call sites see.

## Wiring into the real build (Phase 1 exit gate)

The full extension build integrates the crate via
[corrosion](https://github.com/corrosion-rs/corrosion). Sketch (see
`../CMakeLists.txt` for the flag-gated block):

```cmake
option(PYMOL_NATIVE_CORE "Use the Rust native-core for hot parsers" OFF)
if(PYMOL_NATIVE_CORE)
  find_package(Corrosion REQUIRED)
  corrosion_import_crate(MANIFEST_PATH native-core/Cargo.toml)
  corrosion_add_cxxbridge(native_core_cxx CRATE native_core FILES src/lib.rs)
  target_link_libraries(${TARGET_NAME} native_core_cxx)
  target_compile_definitions(${TARGET_NAME} PRIVATE PYMOL_NATIVE_CORE)
endif()
```

`corrosion_add_cxxbridge` generates `native_core/src/lib.rs.h` (the include used
by `NativeParse.h`) and compiles the C++ glue into the target.

**Exit criteria for Phase 1:** `build_and_verify.cmd` passes on Windows, and the
flag-gated full build links on Linux/macOS/Windows in CI. Until the corrosion
block is exercised in CI, `PYMOL_NATIVE_CORE` stays **OFF** by default and the
`sscanf` fallback is the shipping path.

## Migrated call sites (Phase 1 scope)

`layer2/ObjectMolecule.cpp`: the two trajectory/CRD coordinate loops (`&f2`) and
the periodic-box/angle reads. Remaining `sscanf(cc, "%f", …)` sites in the other
parsers are the identical mechanical substitution and are follow-up work.
