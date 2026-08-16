// bench_main.cpp — LogRange benchmark: contenders and workloads
// (intent v0.3, Deliverable 1).
//
// SUM benchmark — all contenders consume the same pre-generated log-domain
// terms {log_abs, sign}:
//   linear     — converts each term to linear via exp() and accumulates in a
//                plain double. Degrades to 0/inf on extreme inputs by design;
//                it is the failure mode under study, not a strawman.
//   stream_lse — hand-rolled streaming logsumexp, positive-only: what an
//                expert writes inline today. One exp + one log1p per term,
//                deliberately NOT calling the library (success criterion 2
//                compares the library against exactly this).
//   pos_accum  — logrange::pos_accum via add_log, positive-only fast path.
//                The like-for-like criterion-2 comparison against stream_lse.
//   rp_accum   — logrange::rp_accum, signed path.
//
// PRODUCT benchmark — all contenders consume the same pre-generated *linear*
// factors. The pure product is the case where exponent tracking should WIN;
// publishing that number is success criterion 3:
//   exp_track  — normalized mantissa (frexp) + int64 binary exponent counter.
//   log_prod   — log-domain: one log() per factor, summed.
//   linear     — plain running product; leaves double range by design.
//
// Run order: noise floor first (it gates every later judgment), then the
// correctness demo (success criterion 1), then the timing tables.
//
// Usage: bench_logrange [csv_path] [smoke]
//   csv_path defaults to bench_results.csv in cwd; "smoke" caps trip counts
//   at 1000 with low reps — proves the harness runs, produces no citable
//   numbers.

#include "bench_common.h"
#include <logrange/log_math.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Sum contenders. Signature fixed by bench::measure: (log_abs, sign, n).
// ---------------------------------------------------------------------------

double k_linear_sum(const double* la, const double* sg, std::size_t n) {
  double acc = 0.0;
  for (std::size_t i = 0; i < n; ++i) acc += sg[i] * std::exp(la[i]);
  return acc;
}

// Positive-only streaming logsumexp. First-term bootstrap falls out of the
// math: s starts at -inf, so exp(s - x) == 0 and s becomes x exactly.
double k_stream_lse(const double* la, const double* sg, std::size_t n) {
  (void)sg; // positive-only contender: signs are all +1 on its workloads
  double s = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < n; ++i) {
    const double x = la[i];
    if (x > s) {
      s = x + std::log1p(std::exp(s - x));
    } else {
      s = s + std::log1p(std::exp(x - s));
    }
  }
  return s;
}

double k_pos_accum(const double* la, const double* sg, std::size_t n) {
  (void)sg; // positive-only contender: signs are all +1 on its workloads
  logrange::pos_accum acc;
  for (std::size_t i = 0; i < n; ++i) acc.add_log(la[i]);
  return acc.to_log_value().log_abs;
}

double k_rp_accum(const double* la, const double* sg, std::size_t n) {
  logrange::rp_accum acc;
  for (std::size_t i = 0; i < n; ++i) {
    logrange::log_value v;
    v.log_abs = la[i];
    v.sign    = sg[i];
    acc.add(v);
  }
  return acc.to_log_value().log_abs;
}

// ---------------------------------------------------------------------------
// Product contenders. Input array holds linear factors (sign included);
// the second pointer is unused. Each returns the product's log-magnitude
// (times the tracked sign for the two sign-tracking contenders, so the sign
// bookkeeping stays live under the optimizer).
// ---------------------------------------------------------------------------

// Exponent tracking: mantissa renormalized to [0.5, 1) every step via frexp,
// overflow-proof at zero transcendental cost per term. frexp is exact — it
// only splits out the binary exponent — so this matches the naive product
// bit-for-bit while it is still in range.
double k_prod_exptrack(const double* fa, const double* sg, std::size_t n) {
  (void)sg;
  double m = 1.0;         // normalized mantissa, sign carried within
  long long e = 0;        // accumulated binary exponent
  for (std::size_t i = 0; i < n; ++i) {
    m *= fa[i];
    int k = 0;
    m = std::frexp(m, &k);
    e += k;
  }
  // log|prod| = log|m| + e*log(2); multiply by the sign to keep it observed.
  const double sign = (m < 0.0) ? -1.0 : 1.0;
  return sign * (std::log(std::fabs(m)) + static_cast<double>(e) * 0.6931471805599453);
}

double k_prod_log(const double* fa, const double* sg, std::size_t n) {
  (void)sg;
  double s = 0.0;
  double sign = 1.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = fa[i];
    if (v < 0.0) sign = -sign;
    s += std::log(std::fabs(v));
  }
  return sign * s;
}

double k_prod_linear(const double* fa, const double* sg, std::size_t n) {
  (void)sg;
  double p = 1.0;
  for (std::size_t i = 0; i < n; ++i) p *= fa[i];
  return p;
}

// ---------------------------------------------------------------------------
// Workloads. Deterministic (fixed seed per stream); generated once at the
// largest trip count, smaller counts reuse the prefix.
// ---------------------------------------------------------------------------

struct workload {
  std::vector<double> log_abs;
  std::vector<double> sign;
};

// Shape 1: log_abs ~ N(0,1), all positive. Benign magnitudes; measures pure
// per-term cost with no range pressure.
workload gen_uniform(std::size_t n) {
  auto rng = bench::make_rng(1);
  std::normal_distribution<double> d(0.0, 1.0);
  workload w;
  w.log_abs.resize(n);
  w.sign.assign(n, 1.0);
  for (auto& x : w.log_abs) x = d(rng);
  return w;
}

// Shape 2: log_abs uniform in [-600, 600], all positive. Terms individually
// overflow/underflow in linear; the linear contender returns inf here by
// design (timing still measures its loop).
workload gen_wide(std::size_t n) {
  auto rng = bench::make_rng(2);
  std::uniform_real_distribution<double> d(-600.0, 600.0);
  workload w;
  w.log_abs.resize(n);
  w.sign.assign(n, 1.0);
  for (auto& x : w.log_abs) x = d(rng);
  return w;
}

// Shape 3: sign-alternating matched-magnitude pairs (heavy cancellation).
// The negative twin sits 1e-9 log-units above its partner: each pair cancels
// ~9 digits deep without collapsing to exactly zero, which would fire
// rp_accum's pos==neg reset every two terms and measure the reset path
// instead of accumulation. stream_lse is positive-only and skips this shape.
workload gen_cancel(std::size_t n) {
  auto rng = bench::make_rng(3);
  std::normal_distribution<double> d(0.0, 1.0);
  workload w;
  w.log_abs.resize(n);
  w.sign.resize(n);
  for (std::size_t i = 0; i < n; i += 2) {
    const double la = d(rng);
    w.log_abs[i] = la;
    w.sign[i]    = 1.0;
    if (i + 1 < n) {
      w.log_abs[i + 1] = la + 1e-9;
      w.sign[i + 1]    = -1.0;
    }
  }
  return w;
}

// Product factors: |f| = exp(N(0,2)), random sign. The running product's
// log-magnitude random-walks with sd ~ 2*sqrt(n), so the naive product
// leaves double range around n ~ 1e5 — by design.
std::vector<double> gen_factors(std::size_t n) {
  auto rng = bench::make_rng(4);
  std::normal_distribution<double> mag(0.0, 2.0);
  std::bernoulli_distribution neg(0.5);
  std::vector<double> f(n);
  for (auto& v : f) v = (neg(rng) ? -1.0 : 1.0) * std::exp(mag(rng));
  return f;
}

// ---------------------------------------------------------------------------
// Reporting: aligned table to stdout, machine-readable CSV to csv_path.
// ---------------------------------------------------------------------------

FILE* open_csv(const char* path) {
#ifdef _WIN32
  FILE* f = nullptr;
  return (fopen_s(&f, path, "w") == 0) ? f : nullptr;
#else
  return std::fopen(path, "w");
#endif
}

struct reporter {
  FILE* csv = nullptr;

  void header() const {
    std::printf("%-7s %-9s %-10s %9s %5s %7s %13s %13s %8s\n",
                "bench", "shape", "contender", "n", "reps", "inner",
                "ns/term(min)", "ns/term(med)", "spread");
    std::fprintf(csv, "bench,shape,contender,n,reps,inner,"
                      "ns_per_term_min,ns_per_term_median,"
                      "ns_per_term_p10,ns_per_term_p90,spread\n");
  }

  void row(const char* bench_name, const char* shape, const char* who,
           std::size_t n, const bench::config& cfg, const bench::stats& st) const {
    std::printf("%-7s %-9s %-10s %9zu %5d %7d %13.2f %13.2f %8.3f\n",
                bench_name, shape, who, n, cfg.reps(n), cfg.inner(n),
                st.min_ns, st.med_ns, st.spread());
    std::fprintf(csv, "%s,%s,%s,%zu,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                 bench_name, shape, who, n, cfg.reps(n), cfg.inner(n),
                 st.min_ns, st.med_ns, st.p10_ns, st.p90_ns, st.spread());
  }
};

} // namespace

int main(int argc, char** argv) {
  const char* csv_path = (argc > 1) ? argv[1] : "bench_results.csv";
  bench::config cfg;
  cfg.smoke = (argc > 2 && std::strcmp(argv[2], "smoke") == 0);

  const bool pinned = bench::pin_and_prioritize();

  FILE* csv = open_csv(csv_path);
  if (!csv) {
    std::fprintf(stderr, "cannot open CSV output: %s\n", csv_path);
    return 1;
  }
  reporter rep{csv};

  std::printf("LogRange benchmark harness — %s mode\n", cfg.smoke ? "SMOKE (no citable numbers)" : "full");
  std::printf("pinned core + high priority: %s\n", pinned ? "yes" : "NO — numbers suspect");
  std::printf("spread = (p90-p10)/median over reps; inputs fixed-seed deterministic\n\n");

  const std::size_t all_trips[] = {10, 100, 1000, 10000, 100000, 1000000};
  std::vector<std::size_t> trips;
  for (std::size_t n : all_trips)
    if (n <= cfg.max_n()) trips.push_back(n);

  rep.header();

  // --- Noise floor: run FIRST, printed FIRST -------------------------------
  // The identical kernel (stream_lse, uniform shape) registered twice. Any
  // delta between A and B is pure harness/machine noise. Every "within
  // noise" judgment on later rows must clear this measured number.
  {
    const std::size_t n = (std::min)(cfg.max_n(), std::size_t{10000});
    workload w = gen_uniform(n);
    const bench::stats a = bench::measure(k_stream_lse, w.log_abs.data(), w.sign.data(), n, cfg);
    const bench::stats b = bench::measure(k_stream_lse, w.log_abs.data(), w.sign.data(), n, cfg);
    rep.row("noise", "uniform", "lse_A", n, cfg, a);
    rep.row("noise", "uniform", "lse_B", n, cfg, b);
    const double delta = std::fabs(a.med_ns - b.med_ns) / (std::min)(a.med_ns, b.med_ns);
    std::printf("\nNOISE FLOOR: identical kernels' medians differ by %.2f%% "
                "(spreads %.3f / %.3f).\n"
                "Contender deltas at or below this level are not evidence.\n\n",
                delta * 100.0, a.spread(), b.spread());
  }

  // --- Correctness demo (success criterion 1) ------------------------------
  // 1000 terms with log_abs ~ N(-800, 1): each term is ~e^-800, far below
  // the double underflow limit (exp underflows to 0 below ~-745.13), so the
  // linear loop sums exact zeros. Expected log-sum computed analytically by
  // max-shift: m + log(sum exp(la_i - m)) — exact to double rounding because
  // every shifted exponential is O(1).
  {
    const std::size_t n = 1000;
    auto rng = bench::make_rng(5);
    std::normal_distribution<double> d(-800.0, 1.0);
    std::vector<double> la(n), sg(n, 1.0);
    for (auto& x : la) x = d(rng);

    const double linear = k_linear_sum(la.data(), sg.data(), n);
    const double got    = k_rp_accum(la.data(), sg.data(), n);

    double m = la[0];
    for (std::size_t i = 1; i < n; ++i) m = (std::max)(m, la[i]);
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += std::exp(la[i] - m);
    const double expected = m + std::log(s);

    std::printf("CORRECTNESS: 1000-term mixture, each term ~e^-800 (underflows alone)\n");
    std::printf("  linear loop:      %.17g   <- degraded to zero\n", linear);
    std::printf("  rp_accum log_abs: %.15f\n", got);
    std::printf("  expected log_abs: %.15f  (analytic max-shift)\n", expected);
    std::printf("  |error|:          %.3e\n\n", std::fabs(got - expected));
  }

  // --- SUM benchmark -------------------------------------------------------
  {
    const workload uni = gen_uniform(cfg.max_n());
    const workload wid = gen_wide(cfg.max_n());
    const workload can = gen_cancel(cfg.max_n());

    for (std::size_t n : trips) {
      rep.row("sum", "uniform", "linear",     n, cfg, bench::measure(k_linear_sum, uni.log_abs.data(), uni.sign.data(), n, cfg));
      rep.row("sum", "uniform", "stream_lse", n, cfg, bench::measure(k_stream_lse, uni.log_abs.data(), uni.sign.data(), n, cfg));
      rep.row("sum", "uniform", "pos_accum",  n, cfg, bench::measure(k_pos_accum,  uni.log_abs.data(), uni.sign.data(), n, cfg));
      rep.row("sum", "uniform", "rp_accum",   n, cfg, bench::measure(k_rp_accum,   uni.log_abs.data(), uni.sign.data(), n, cfg));
    }
    std::printf("\n");
    for (std::size_t n : trips) {
      rep.row("sum", "wide", "linear",     n, cfg, bench::measure(k_linear_sum, wid.log_abs.data(), wid.sign.data(), n, cfg));
      rep.row("sum", "wide", "stream_lse", n, cfg, bench::measure(k_stream_lse, wid.log_abs.data(), wid.sign.data(), n, cfg));
      rep.row("sum", "wide", "pos_accum",  n, cfg, bench::measure(k_pos_accum,  wid.log_abs.data(), wid.sign.data(), n, cfg));
      rep.row("sum", "wide", "rp_accum",   n, cfg, bench::measure(k_rp_accum,   wid.log_abs.data(), wid.sign.data(), n, cfg));
    }
    std::printf("(linear on 'wide' returns inf — expected; timing measures the loop)\n\n");
    // Cancellation shape: stream_lse is positive-only and does not run here.
    for (std::size_t n : trips) {
      rep.row("sum", "cancel", "linear",   n, cfg, bench::measure(k_linear_sum, can.log_abs.data(), can.sign.data(), n, cfg));
      rep.row("sum", "cancel", "rp_accum", n, cfg, bench::measure(k_rp_accum,   can.log_abs.data(), can.sign.data(), n, cfg));
    }
    std::printf("\n");
  }

  // --- PURE PRODUCT benchmark (success criterion 3) ------------------------
  {
    const std::vector<double> fac = gen_factors(cfg.max_n());
    for (std::size_t n : trips) {
      rep.row("product", "lognormal", "exp_track", n, cfg, bench::measure(k_prod_exptrack, fac.data(), nullptr, n, cfg));
      rep.row("product", "lognormal", "log_prod",  n, cfg, bench::measure(k_prod_log,      fac.data(), nullptr, n, cfg));
      rep.row("product", "lognormal", "linear",    n, cfg, bench::measure(k_prod_linear,   fac.data(), nullptr, n, cfg));
    }
    std::printf("(linear product over/underflows at large n — expected; exp_track stays exact)\n");
  }

  std::printf("\nCSV written: %s\n", csv_path);
  std::fclose(csv);
  return 0;
}
