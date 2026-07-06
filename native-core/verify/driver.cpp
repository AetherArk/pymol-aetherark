// Phase-1 verification driver.
//
// This is a STANDALONE differential test that proves two things at once:
//   1. the Rust <-> C++ FFI seam actually compiles and links under MSVC
//      (the real Windows toolchain risk Phase 1 exists to retire), and
//   2. parse_f32 is bit-identical to sscanf("%f") on a representative corpus
//      of fixed-width coordinate fields.
//
// It is built by native-core/verify/build_and_verify.ps1 (cl.exe + the
// cxx-generated glue + native_core.lib). It does NOT need cmake or the full
// PyMOL build, so the seam can be validated in isolation.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "src/lib.rs.h" // cxx-generated header (path matches cxxbridge self-include)

static bool native_parse(const char* s, float* out)
{
  return pymol_native::parse_f32(
      rust::Slice<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)),
      *out);
}

static bool reference_parse(const char* s, float* out)
{
  return std::sscanf(s, "%f", out) == 1;
}

static bool bits_equal(float a, float b)
{
  std::uint32_t ba, bb;
  std::memcpy(&ba, &a, 4);
  std::memcpy(&bb, &b, 4);
  return ba == bb;
}

int main()
{
  // Representative fixed-width columns as produced by ncopy() at the call sites.
  const char* corpus[] = {
      "  12.345", "12.345  ", " -7.890 ", "   0.000", "-999.999", "1234.567",
      "+1.5", ".5", "1.", "1e-3", "1.5E2", "-.25", "  -0.00",
      // failures (sscanf returns 0):
      "", "    ", "abc", "*",
  };

  int failures = 0;
  int checked = 0;

  for (const char* s : corpus) {
    float rv = 0.0f, nv = 0.0f;
    bool r_ok = reference_parse(s, &rv);
    bool n_ok = native_parse(s, &nv);
    ++checked;

    if (r_ok != n_ok) {
      std::printf("MISMATCH ok: %-10s sscanf=%d native=%d\n", s, r_ok, n_ok);
      ++failures;
      continue;
    }
    if (r_ok) {
      // NaN compares unequal to itself; treat matching NaN-ness as equal.
      bool eq = bits_equal(rv, nv) || (std::isnan(rv) && std::isnan(nv));
      if (!eq) {
        std::printf("MISMATCH val: %-10s sscanf=%.9g native=%.9g\n", s, rv, nv);
        ++failures;
      }
    }
  }

  // Dense sweep across the coordinate range at %8.3f precision.
  for (int i = -500000; i <= 500000; i += 137) {
    float v = i / 1000.0f;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%8.3f", v);
    float rv = 0.0f, nv = 0.0f;
    bool r_ok = reference_parse(buf, &rv);
    bool n_ok = native_parse(buf, &nv);
    ++checked;
    if (r_ok != n_ok || (r_ok && !bits_equal(rv, nv))) {
      std::printf("MISMATCH sweep: %-10s sscanf=%.9g native=%.9g\n", buf, rv, nv);
      ++failures;
    }
  }

  std::printf("checked=%d failures=%d\n", checked, failures);
  if (failures == 0) {
    std::printf("PASS: native parse_f32 is bit-identical to sscanf and links under MSVC.\n");
    return 0;
  }
  std::printf("FAIL\n");
  return 1;
}
