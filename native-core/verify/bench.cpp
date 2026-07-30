// Phase-1 microbenchmark: native parse_f32 (via the cxx shim, exactly as the
// migrated call sites invoke it) vs the original sscanf("%f").
//
// Models the real hot path: ncopy() has already produced a NUL-terminated
// fixed-width coordinate field; we parse it to a float. We time a large number
// of such parses over a representative corpus, accumulate a checksum to defeat
// dead-code elimination, and report ns/field and speedup (median of several
// repetitions).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "src/lib.rs.h" // cxx-generated header

// Mirrors layer0/NativeParse.h's NativeParseFloat() body exactly.
static inline bool native_parse(const char* s, float* out)
{
  return pymol_native::parse_f32(
      rust::Slice<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)),
      *out);
}

static inline bool sscanf_parse(const char* s, float* out)
{
  return std::sscanf(s, "%f", out) == 1;
}

// Build a representative corpus of 8-wide coordinate columns, formatted the way
// PDB/CRD writers produce them and ncopy() hands them over.
static std::vector<std::string> make_corpus(size_t n)
{
  std::vector<std::string> v;
  v.reserve(n);
  // pseudo-random but deterministic spread over a realistic coordinate range
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < n; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    double r = (double)(state >> 11) / (double)(1ull << 53); // [0,1)
    float val = (float)((r * 1000.0) - 500.0);               // [-500,500)
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%8.3f", val); // 8-wide, 3 decimals
    v.emplace_back(buf);
  }
  return v;
}

template <typename F>
static double time_ns_per_op(F&& parse, const std::vector<std::string>& corpus,
    int passes, volatile double& sink)
{
  using clock = std::chrono::steady_clock;
  double acc = 0.0;
  auto t0 = clock::now();
  for (int p = 0; p < passes; ++p) {
    for (const auto& s : corpus) {
      float out = 0.0f;
      if (parse(s.c_str(), &out))
        acc += out;
    }
  }
  auto t1 = clock::now();
  sink += acc; // defeat DCE
  double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  return ns / (double(passes) * double(corpus.size()));
}

int main(int argc, char** argv)
{
  size_t corpus_n = 100000;
  int passes = 40;    // corpus_n * passes = total field parses per timed run
  int reps = 5;       // repetitions; report median
  if (argc > 1) corpus_n = std::strtoull(argv[1], nullptr, 10);
  if (argc > 2) passes = std::atoi(argv[2]);

  auto corpus = make_corpus(corpus_n);
  const long long total = (long long)corpus_n * passes;

  volatile double sink = 0.0;

  // Warm up both paths (page-in, branch predictor, instruction cache).
  time_ns_per_op(sscanf_parse, corpus, 2, sink);
  time_ns_per_op(native_parse, corpus, 2, sink);

  std::vector<double> s_times, n_times;
  for (int r = 0; r < reps; ++r) {
    s_times.push_back(time_ns_per_op(sscanf_parse, corpus, passes, sink));
    n_times.push_back(time_ns_per_op(native_parse, corpus, passes, sink));
  }
  std::sort(s_times.begin(), s_times.end());
  std::sort(n_times.begin(), n_times.end());
  double s = s_times[reps / 2];
  double n = n_times[reps / 2];

  std::printf("corpus=%zu passes=%d total_parses=%lld reps=%d (median)\n",
      corpus_n, passes, total, reps);
  std::printf("  sscanf(\"%%f\")   : %8.2f ns/field  (%6.1f M fields/s)\n",
      s, 1000.0 / s);
  std::printf("  native parse_f32: %8.2f ns/field  (%6.1f M fields/s)\n",
      n, 1000.0 / n);
  std::printf("  speedup         : %5.2fx  (%.1f%% faster)\n", s / n,
      (s - n) / s * 100.0);
  std::printf("  [checksum sink=%.3f]\n", (double)sink);
  return 0;
}
