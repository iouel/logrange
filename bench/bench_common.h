// bench_common.h — LogRange benchmark harness core (intent v0.3, Deliverable 1,
// First Action step 4).
//
// The harness must be trustworthy before its numbers are. Concretely:
//   - The measuring thread is pinned to one core and raised to high priority
//     (Windows; no-op elsewhere — pin_and_prioritize() reports failure so the
//     run is labeled unpinned rather than silently pretending).
//   - Warmup batches run before any timed sample.
//   - Each cell takes R timed samples and reports min, median, and spread,
//     where spread = (p90 - p10) / median. R scales down as trip count grows
//     so the full run stays around a minute of wall time.
//   - The noise floor is MEASURED, not assumed: bench_main registers one
//     identical kernel twice and prints their delta before any comparison.
//     The predecessor's harness had 8x run-to-run swings, which made
//     "within noise" claims unfalsifiable.
//   - PRNG is fixed-seed mt19937_64: identical binaries see identical inputs.
//   - Kernel results pass through a volatile sink (no dead-code elimination)
//     and kernels receive their input pointers through volatile cells (the
//     optimizer cannot prove repeated calls loop-invariant and hoist them).

#pragma once

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace bench {

// Fixed seed: reproducibility is a harness requirement, not a convenience.
// `stream` decorrelates workloads without introducing run-to-run variation.
constexpr std::uint64_t RNG_SEED = 0x10c9a19e5eedULL;

inline std::mt19937_64 make_rng(std::uint64_t stream) {
  return std::mt19937_64(RNG_SEED ^ (stream * 0x9e3779b97f4a7c15ULL));
}

// Pin the current thread to core 0 and raise process/thread priority.
// Returns false if any step failed, or always on non-Windows (no-op there);
// the caller must surface that so unpinned numbers are labeled as such.
inline bool pin_and_prioritize() {
#ifdef _WIN32
  const bool cls  = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
  const bool prio = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
  const bool aff  = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1)) != 0;
  return cls && prio && aff;
#else
  return false;
#endif
}

// Volatile sink: every kernel's result is stored here once per call, so the
// computation is observable and cannot be dead-code-eliminated. A volatile
// double write is sufficient under MSVC and GCC/Clang at /O2 / -O2.
inline volatile double sink = 0.0;

// Repetition policy. Sample size (inner * n terms per timed sample) is held
// near-constant so steady_clock granularity (~100ns on Windows) is always
// orders of magnitude below a sample; R shrinks as n grows to bound total
// wall time. Smoke mode exists to prove the harness end-to-end, not to
// produce citable numbers.
struct config {
  bool smoke = false;

  std::size_t max_n() const { return smoke ? 1000u : 1000000u; }

  int reps(std::size_t n) const {
    if (smoke) return 5;
    if (n <= 10000)  return 31;
    if (n <= 100000) return 15;
    return 9;
  }

  // Batch the kernel so each timed sample covers ~2^20 terms (2^14 in smoke).
  int inner(std::size_t n) const {
    const std::size_t target = smoke ? (std::size_t{1} << 14) : (std::size_t{1} << 20);
    return static_cast<int>((std::max)(std::size_t{1}, target / n));
  }

  int warmup_batches() const { return smoke ? 1 : 3; }
};

// Per-cell timing summary, all in ns/term. min is the least-noise estimate;
// median is the headline number; spread is the honesty number.
struct stats {
  double min_ns = 0.0;
  double med_ns = 0.0;
  double p10_ns = 0.0;
  double p90_ns = 0.0;
  double spread() const { return med_ns > 0.0 ? (p90_ns - p10_ns) / med_ns : 0.0; }
};

inline stats summarize(std::vector<double> ns) {
  std::sort(ns.begin(), ns.end());
  const std::size_t r = ns.size();
  auto quantile = [&](double p) {
    return ns[static_cast<std::size_t>(p * static_cast<double>(r - 1) + 0.5)];
  };
  stats s;
  s.min_ns = ns.front();
  s.med_ns = (r % 2 != 0) ? ns[r / 2] : 0.5 * (ns[r / 2 - 1] + ns[r / 2]);
  s.p10_ns = quantile(0.10);
  s.p90_ns = quantile(0.90);
  return s;
}

// Time kernel(log_abs, sign, n) -> double over cfg.reps(n) samples of
// cfg.inner(n) calls each, after warmup. The input pointers are re-fetched
// through volatile cells before every call: without this, an optimizer that
// proves the kernel pure could compute it once and replay the stored result,
// timing nothing. `sign` may be null for kernels that ignore it.
template <class K>
stats measure(K&& kernel, const double* log_abs, const double* sign,
              std::size_t n, const config& cfg) {
  const int inner = cfg.inner(n);
  const int reps  = cfg.reps(n);

  const double* volatile vla = log_abs;
  const double* volatile vsg = sign;

  auto batch = [&]() {
    for (int i = 0; i < inner; ++i) {
      const double* la = vla;
      const double* sg = vsg;
      sink = kernel(la, sg, n);
    }
  };

  for (int w = 0; w < cfg.warmup_batches(); ++w) batch();

  std::vector<double> ns_per_term(static_cast<std::size_t>(reps));
  for (int r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    batch();
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    ns_per_term[static_cast<std::size_t>(r)] =
        total_ns / (static_cast<double>(inner) * static_cast<double>(n));
  }
  return summarize(std::move(ns_per_term));
}

} // namespace bench
