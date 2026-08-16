// test_accuracy.cpp — empirical accuracy validation for rp_accum and log_add.
//
// Measures how far the library's accumulation strategies drift from a
// high-precision reference, asserts *generous* sanity bounds, and PRINTS the
// observed maximum errors. The printed table is a deliverable: those numbers
// feed the analytical worst-case bound work (intent doc, Deliverable 1,
// "stated worst-case error bound under cancellation").
//
// Reference: double-double (compensated) summation in the LINEAR domain,
// Knuth TwoSum / Dekker style (~106 significant bits).
//
// SCOPE LIMIT (deliberate): the double-double reference sums *linear* doubles,
// so it only works for terms representable in double range. All scenarios here
// restrict magnitudes to roughly |log_abs| <= 230 (~1e100). The beyond-range
// regime (the library's raison d'etre) is covered by the existing correctness
// tests (test_rp_accum.cpp, test_log_math.cpp), not by this file.
//
// All randomness is fixed-seed std::mt19937_64 — runs are deterministic on a
// given platform/standard library.
#include "test_common.h"
#include <logrange/log_math.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

using namespace logrange;

#include "dd_sum.h" // the reference, shared with bound_search.cpp

// ---------------------------------------------------------------------------
// Formal-bound helpers (header error contract, rp_accum v0.2):
//   rel err <= cond * (3k + 4) * u,   u = 2^-53,
// where k counts rescale events — adds that strictly raise the running max
// of log_abs after the first nonzero term. Recomputed here from the input
// data (the accumulator deliberately carries no instrumentation counters).
// ---------------------------------------------------------------------------
static const double U = 0x1p-53; // unit roundoff

static std::size_t count_rescales(const std::vector<log_value>& terms) {
  std::size_t k = 0;
  double m = -std::numeric_limits<double>::infinity();
  for (const log_value& v : terms) {
    if (v.is_zero()) continue;
    if (v.log_abs > m) {
      if (m != -std::numeric_limits<double>::infinity()) ++k;
      m = v.log_abs;
    }
  }
  return k;
}

// Mass-weighted mean insertion depth: sum|x_i|*(m_i - L_i) / sum|x_i|, with
// m_i the running reference when term i was added. This is the term the
// original cond*(3k+4)*u form omitted — see log_math.h and bound_search.cpp.
static double weighted_depth(const std::vector<log_value>& terms) {
  dd_sum mass, wdepth;
  double m = -std::numeric_limits<double>::infinity();
  for (const log_value& v : terms) {
    if (v.is_zero()) continue;
    if (v.log_abs > m) m = v.log_abs;
    const double w = std::fabs(v.to_linear());
    mass.add(w);
    wdepth.add(w * (m - v.log_abs));
  }
  return (mass.value() > 0.0) ? wdepth.value() / mass.value() : 0.0;
}

// The contract from log_math.h. The reduction contributes BOTH addends of
// m_log + log|net|: log|net| rounds at its own magnitude, and the addition
// rounds at |log|S||. Charging only the second was refuted 2026-08-16 at
// 1.99x by bound_search's family E, so `peak` is required here rather than
// optional — without it this helper asserts a known-false claim.
static double formal_bound(double cond, std::size_t k, double depth,
                           double log_abs_sum, double peak) {
  const double lognet = std::fabs(log_abs_sum - peak);
  return cond * (3.0 * static_cast<double>(k) + 4.0 + depth) * U +
         (std::fabs(log_abs_sum) + lognet) * U;
}

// The running reference m_log at the end of the accumulation: the largest
// log_abs among the nonzero terms.
static double peak_log_abs(const std::vector<log_value>& terms) {
  double m = -std::numeric_limits<double>::infinity();
  for (const log_value& v : terms)
    if (!v.is_zero() && v.log_abs > m) m = v.log_abs;
  return m;
}

// ---------------------------------------------------------------------------
// Observed-error table. Rows are printed as scenarios run; the header goes
// out first from main(). This output is a deliverable, not debug noise.
// ---------------------------------------------------------------------------
static void report_row(const char* name, std::size_t n, double observed, double bound) {
  std::printf("  %-52s %9zu   %11.3e   %11.3e\n", name, n, observed, bound);
}

// ---------------------------------------------------------------------------
// Scenario 1 — long positive accumulation.
// n in {1e3, 1e5, 1e6}, log_abs ~ normal(0, 3), all positive. rp_accum's
// pos partial sum is Neumaier-compensated (v0.2), so the residual error is
// the per-term exp() rounding, not summation drift. Generous assert
// (predates compensation, kept as a regression tripwire): rel < n * 1e-14.
// ---------------------------------------------------------------------------
static void test_long_positive_sum() {
  const std::size_t sizes[] = {1000, 100000, 1000000};
  for (std::size_t n : sizes) {
    std::mt19937_64 rng(0xC0FFEEULL + n);
    std::normal_distribution<double> logmag(0.0, 3.0);

    rp_accum acc;
    dd_sum ref;
    std::vector<log_value> terms; // kept so the depth term can be recomputed
    terms.reserve(n);
    std::size_t k = 0; // rescale events, tracked from the data
    double m = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
      log_value v;
      v.log_abs = logmag(rng);
      v.sign = 1.0;
      terms.push_back(v);
      if (v.log_abs > m) { if (i > 0) ++k; m = v.log_abs; }
      acc.add(v);
      ref.add(std::exp(v.log_abs)); // same linear term the accumulator sees
    }
    const double got   = acc.to_log_value().to_linear();
    const double truth = ref.value();
    const double rel   = std::fabs(got - truth) / std::fabs(truth);
    // Positive-only sum: cond == 1 exactly. Assert the header's formal bound.
    const double bound = formal_bound(1.0, k, weighted_depth(terms),
                                      ref.log_abs(), peak_log_abs(terms));

    char label[64];
    std::snprintf(label, sizeof label, "rp_accum long +sum n=%zu (rel err, k=%zu)", n, k);
    report_row(label, n, rel, bound);
    NC_CHECK(rel < bound);
  }
}

// ---------------------------------------------------------------------------
// Scenarios 2 & 5 share one deterministic data set: heavy cancellation.
// Pairs (+x, -x*(1+delta)), delta log-uniform in [1e-12, 1e-8], plus a
// residual-carrying tail of small positive terms. The true sum is tiny
// relative to the term magnitudes; the condition number
//   cond = sum|x_i| / |sum x_i|
// ties the observed error to the conditioning in the printout.
//
// The reference sums v.to_linear() for each stored log_value — exactly the
// linear values the log encoding holds — so this measures ACCUMULATION error,
// not the (separate) error of encoding a linear input into log form.
// ---------------------------------------------------------------------------
struct cancel_data {
  std::vector<log_value> terms;
  double truth = 0.0; // dd sum of the terms' linear values
  double cond  = 0.0; // sum|x_i| / |truth|
};

static cancel_data make_cancel_data() {
  std::mt19937_64 rng(0xDECAFBADULL);
  std::normal_distribution<double> logmag(0.0, 3.0);
  std::uniform_real_distribution<double> log10delta(-12.0, -8.0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  const std::size_t npairs = 10000;
  const std::size_t ntail  = 100;

  cancel_data d;
  d.terms.reserve(2 * npairs + ntail);
  dd_sum s, sabs;
  auto push = [&](double linear) {
    const log_value v(linear);
    d.terms.push_back(v);
    const double back = v.to_linear();
    s.add(back);
    sabs.add(std::fabs(back));
  };

  for (std::size_t i = 0; i < npairs; ++i) {
    const double x     = std::exp(logmag(rng));
    const double delta = std::pow(10.0, log10delta(rng));
    push(x);
    push(-x * (1.0 + delta));
  }
  for (std::size_t i = 0; i < ntail; ++i) {
    push(1e-6 * unit(rng)); // residual-carrying tail
  }

  d.truth = s.value();
  d.cond  = sabs.value() / std::fabs(d.truth);
  return d;
}

// Scenario 2 — rp_accum under heavy cancellation.
// Generous assert: rel < cond * n * 1e-14.
static void test_heavy_cancellation(const cancel_data& d) {
  rp_accum acc;
  for (const log_value& v : d.terms) acc.add(v);

  const double got   = acc.to_log_value().to_linear();
  const double rel   = std::fabs(got - d.truth) / std::fabs(d.truth);
  const double bound = formal_bound(d.cond, count_rescales(d.terms),
                                    weighted_depth(d.terms),
                                    std::log(std::fabs(d.truth)),
                                    peak_log_abs(d.terms));

  char label[64];
  std::snprintf(label, sizeof label, "rp_accum heavy cancel (rel err, cond=%.1e)", d.cond);
  report_row(label, d.terms.size(), rel, bound);
  NC_CHECK(rel < bound);
}

// Scenario 5 — log_add sequential fold on the SAME data, so the two
// accumulation strategies' accuracies sit side by side in the table.
static void test_log_add_chain(const cancel_data& d) {
  log_value r; // zero identity
  for (const log_value& v : d.terms) r = log_add(r, v);

  const double got   = r.to_linear();
  const double rel   = std::fabs(got - d.truth) / std::fabs(d.truth);
  const double bound = d.cond * static_cast<double>(d.terms.size()) * 1e-14;

  char label[64];
  std::snprintf(label, sizeof label, "log_add fold heavy cancel (rel err, cond=%.1e)", d.cond);
  report_row(label, d.terms.size(), rel, bound);
  NC_CHECK(rel < bound);
}

// ---------------------------------------------------------------------------
// Scenario 2b — ordering sensitivity on the same cancellation data.
// Finding recorded 2026-08-15 (BENCHMARKS.md): a log_add fold's apparent
// accuracy advantage on this dataset was an ordering artifact — each +/- pair
// annihilated while adjacent. Shuffled, the fold degrades by ~2 orders while
// compensated rp_accum does not (it landed *better* shuffled than paired in
// the recorded run). This scenario pins both behaviors: rp_accum must stay
// order-robust, and the fold's fragility is documented data, not folklore.
// ---------------------------------------------------------------------------
static void test_ordering_sensitivity(const cancel_data& d) {
  std::vector<log_value> shuffled = d.terms;
  std::mt19937_64 rng(0x0B5C0DE0ULL);
  std::shuffle(shuffled.begin(), shuffled.end(), rng);

  rp_accum acc;
  for (const log_value& v : shuffled) acc.add(v);
  const double rp_rel = std::fabs(acc.to_log_value().to_linear() - d.truth) / std::fabs(d.truth);

  log_value r;
  for (const log_value& v : shuffled) r = log_add(r, v);
  const double fold_rel = std::fabs(r.to_linear() - d.truth) / std::fabs(d.truth);

  // rp_accum answers to the formal contract bound with k from THIS ordering;
  // the fold has no such contract — it keeps the old generous sanity bound.
  const double rp_bound   = formal_bound(d.cond, count_rescales(shuffled),
                                         weighted_depth(shuffled),
                                         std::log(std::fabs(d.truth)),
                                         peak_log_abs(shuffled));
  const double fold_bound = d.cond * static_cast<double>(d.terms.size()) * 1e-14;
  report_row("rp_accum heavy cancel SHUFFLED (rel err)", d.terms.size(), rp_rel, rp_bound);
  report_row("log_add fold heavy cancel SHUFFLED (rel err)", d.terms.size(), fold_rel, fold_bound);
  NC_CHECK(rp_rel < rp_bound);
  NC_CHECK(fold_rel < fold_bound);
}

// ---------------------------------------------------------------------------
// Scenario 3 — magnitude staircase. Integer log_abs from -230 to +230 in
// shuffled order: every time a new maximum arrives, rp_accum rescales its
// partial sums (the rescale-on-new-max path), so a shuffle exercises that
// path repeatedly. Compare log-magnitudes: absolute error in log_abs equals
// relative error of the linear result, and stays meaningful across the whole
// 460-log-unit span. Worst case over 5 shuffles is reported.
//
// Measurement-resolution note: the result's log_abs sits near 230.46, where
// one ulp is ~2.8e-14. An observed error of exactly 0.0 is a real result —
// both the accumulator and the reference rounded to the same double — and
// should be read as "below ulp(log_abs) resolution", not as suspiciously
// perfect. (True agreement here is ~1e-15 relative in the linear domain,
// well under half an ulp of the log value.)
// ---------------------------------------------------------------------------
static void test_magnitude_staircase() {
  std::mt19937_64 rng(0xABCDEF12ULL);
  std::vector<double> logs;
  for (int k = -230; k <= 230; ++k) logs.push_back(static_cast<double>(k));

  double worst = 0.0;
  for (int trial = 0; trial < 5; ++trial) {
    std::shuffle(logs.begin(), logs.end(), rng);
    rp_accum acc;
    dd_sum ref;
    for (double l : logs) {
      log_value v;
      v.log_abs = l;
      v.sign = 1.0;
      acc.add(v);
      ref.add(std::exp(l));
    }
    const double err = std::fabs(acc.to_log_value().log_abs - ref.log_abs());
    worst = (std::max)(worst, err);
  }
  report_row("rp_accum staircase (log-abs err, worst of 5)", logs.size(), worst, 1e-12);
  NC_CHECK(worst < 1e-12);
}

// ---------------------------------------------------------------------------
// Scenario 4 — reset-cost measurement.
// Force exact pos==neg resets by adding +A then -A (identical log_abs, so
// the scaled ratios are exactly 1.0 and pos == neg fires clear()), with a
// small ongoing sum interleaved. Each reset discards the in-flight small
// mass — the header's documented cost: up to |largest term| * eps per reset
// event. The small terms are sized (~1e62 against A = 1e80) so each block is
// genuinely swallowed by the +A add (block/A << eps/2) yet large enough that
// the observed discarded mass is within ~2 orders of the contract bound,
// making the comparison non-trivial.
// Assert: |result - true_sum| <= k_resets * A * eps, per the contract.
// ---------------------------------------------------------------------------
static void test_reset_cost() {
  std::mt19937_64 rng(0x5EED4E5EULL);
  std::uniform_real_distribution<double> mag(0.5, 1.5);
  const double eps       = std::numeric_limits<double>::epsilon();
  const double SCALE     = 1e62;  // small-term size; block sum / A stays < eps/2
  const double A         = 1e80;  // the exactly-cancelling large magnitude
  const std::size_t k_resets  = 20;
  const std::size_t per_block = 5;
  const std::size_t ntail     = 7;

  rp_accum acc;
  dd_sum all;  // true sum of every small term (the +A/-A pairs cancel exactly)
  dd_sum tail; // true sum of the survivors after the final reset
  std::size_t nterms = 0;

  for (std::size_t j = 0; j < k_resets; ++j) {
    for (std::size_t i = 0; i < per_block; ++i) {
      const log_value v(mag(rng) * SCALE);
      acc.add(v);
      all.add(v.to_linear());
      ++nterms;
    }
    acc.add(log_value(A));
    acc.add(log_value(-A));
    nterms += 2;
    NC_CHECK(acc.empty()); // the exact pos==neg reset must have fired
  }
  for (std::size_t i = 0; i < ntail; ++i) {
    const log_value v(mag(rng) * SCALE);
    acc.add(v);
    all.add(v.to_linear());
    tail.add(v.to_linear());
    ++nterms;
  }

  const double got      = acc.to_log_value().to_linear();
  const double abs_err  = std::fabs(got - all.value());
  const double contract = static_cast<double>(k_resets) * A * eps;

  char label[64];
  std::snprintf(label, sizeof label, "rp_accum reset cost (abs err vs contract, k=%zu)", k_resets);
  report_row(label, nterms, abs_err, contract);
  NC_CHECK(abs_err <= contract);

  // Benefit side: the survivors after the last reset must themselves be
  // accurate — the reset re-armed the reference exponent for them.
  const double tail_rel = std::fabs(got - tail.value()) / std::fabs(tail.value());
  NC_CHECK(tail_rel < 1e-12);
}

int main() {
  std::printf("observed accuracy vs double-double linear reference\n");
  std::printf("  %-52s %9s   %11s   %11s\n", "scenario", "n", "observed", "bound");

  test_long_positive_sum();
  const cancel_data d = make_cancel_data();
  test_heavy_cancellation(d);
  test_log_add_chain(d);
  test_ordering_sensitivity(d);
  test_magnitude_staircase();
  test_reset_cost();

  std::puts("test_accuracy passed");
  return 0;
}
