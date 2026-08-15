// bound_search.cpp — adversarial search against the rp_accum error contract.
//
// test_accuracy.cpp asserts the bound on six fixed scenarios. The bound is a
// universal claim over all inputs, orderings and k, so fixed scenarios can
// only ever fail to refute it. This file tries to refute it: it constructs
// input families aimed at the derivation's weak points and reports the worst
// observed/bound ratio found. A ratio > 1 is a counterexample.
//
// The derivation (log_math.h) charges each term "<= 2u from its exp()". That
// accounts for exp's own rounding but not for the rounding of its ARGUMENT.
// The accumulator evaluates exp(fl(L_i - m)), and
//
//   fl(L - m) = (L - m) - e,   |e| <= (1/2) ulp(L - m) ~ u * d,  d = m - L
//
// so the scaled ratio carries a relative error of about (d + 1) * u, not 2u.
// The weight of that term is exp(-d), which damps deep terms, but a CLUSTER
// of terms sharing one depth shares one rounding error e, coherently: no
// cancellation across the cluster. Family A below builds exactly that.
//
// Reference caveat, stated because it bounds what this file can prove: the
// dd reference sums exp(L_i) computed in plain double, so it carries up to
// ~1u of its own relative error. Ratios below ~1.3 are not evidence. The
// counterexamples reported here are well clear of that floor.
#include "test_common.h"
#include "dd_sum.h"
#include <logrange/log_math.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

using namespace logrange;

static const double U    = 0x1p-53;
static const double NINF = -std::numeric_limits<double>::infinity();

struct verdict {
  std::size_t n = 0;
  std::size_t k = 0;
  double cond = 0.0;
  double depth = 0.0;    // mass-weighted mean insertion depth, D_eff
  double outmag = 0.0;   // |log|S||, the final reduction's lever arm
  double observed = 0.0;
  double bound = 0.0;    // v0.2 stated contract
  double fixed = 0.0;    // corrected contract
  double ratio = 0.0;
  double ratio_fixed = 0.0;
};

// Run the terms through rp_accum and score them against the stated contract.
static verdict evaluate(const std::vector<log_value>& terms) {
  rp_accum acc;
  dd_sum ref, mass, weighted_depth;
  double m = NINF;
  std::size_t k = 0;
  bool first = true;

  for (const log_value& v : terms) {
    if (v.is_zero()) continue;
    if (v.log_abs > m) {
      if (!first) ++k;
      m = v.log_abs;
    }
    first = false;
    acc.add(v);
    const double lin = v.to_linear();
    ref.add(lin);
    mass.add(std::fabs(lin));
    // Depth at insertion: how far below the running reference this term sat
    // when its exp() argument was formed. That difference is what rounds.
    weighted_depth.add(std::fabs(lin) * (m - v.log_abs));
  }

  verdict r;
  r.n        = terms.size();
  r.k        = k;
  const double truth = ref.value();
  r.cond     = mass.value() / std::fabs(truth);
  r.depth    = weighted_depth.value() / mass.value();
  r.outmag   = std::fabs(ref.log_abs());
  const double got = acc.to_log_value().to_linear();
  r.observed = std::fabs(got - truth) / std::fabs(truth);
  r.bound    = r.cond * (3.0 * static_cast<double>(k) + 4.0) * U;
  // Corrected contract: the argument-rounding term rides with the mass and is
  // amplified by cond like every other coefficient perturbation; the final
  // reduction m_log + log|net| rounds once, in log space, and lands on the
  // linear result directly rather than through cond.
  r.fixed    = r.cond * (3.0 * static_cast<double>(k) + 4.0 + r.depth) * U +
               r.outmag * U;
  r.ratio       = r.observed / r.bound;
  r.ratio_fixed = r.observed / r.fixed;
  return r;
}

// Pick the cluster's log_abs so that (L - peak) rounds as badly as it can.
// Stepping L by its own ulp moves the exact difference by far less than the
// difference's ulp, so a short sweep lands near the half-ulp worst case.
static double worst_rounding_log_abs(double peak, double depth, double* out_err) {
  double best = peak - depth, best_err = 0.0;
  double cand = peak - depth;
  for (int i = 0; i < 512; ++i) {
    const double e = std::fabs(two_diff_err(cand, peak));
    if (e > best_err) { best_err = e; best = cand; }
    cand = std::nextafter(cand, peak);
  }
  *out_err = best_err;
  return best;
}

// Family A — one dominant term, then N terms sharing a single depth whose
// argument subtraction rounds worst. All positive, so cond == 1 exactly and
// the bound reduces to 4u with k == 0: nothing but the coefficient is on
// trial. The cluster carries N*exp(-depth) of the mass against the peak's 1.
static std::vector<log_value> family_depth_cluster(double depth, std::size_t N,
                                                   double* out_argerr) {
  const double peak = 10.5;
  const double L    = worst_rounding_log_abs(peak, depth, out_argerr);

  std::vector<log_value> terms;
  terms.reserve(N + 1);
  log_value top;
  top.sign = 1.0;
  top.log_abs = peak;   // added first, so k == 0 for everything after it
  terms.push_back(top);
  for (std::size_t i = 0; i < N; ++i) {
    log_value v;
    v.sign = 1.0;
    v.log_abs = L;
    terms.push_back(v);
  }
  return terms;
}

// Family B — family A plus a near-cancelling negative term, to check that the
// coefficient error rides through the cond amplification rather than being
// an artifact of the cond == 1 case.
static std::vector<log_value> family_cluster_cancel(double depth, std::size_t N,
                                                    double rel_residual) {
  double argerr = 0.0;
  std::vector<log_value> terms = family_depth_cluster(depth, N, &argerr);

  dd_sum s;
  for (const log_value& v : terms) s.add(v.to_linear());
  log_value neg;
  neg.sign = -1.0;
  neg.log_abs = s.log_abs() + std::log1p(-rel_residual);
  terms.push_back(neg);
  return terms;
}

// Family C — ascending order, the k == n-1 worst case for rescales. This is
// the case the 3k term was written for; included so the search covers the
// derivation's own stated worst case and not only its blind spot.
static std::vector<log_value> family_ascending(std::size_t n, double span) {
  std::vector<log_value> terms;
  terms.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    log_value v;
    v.sign = 1.0;
    v.log_abs = -span + 2.0 * span * (static_cast<double>(i) /
                                      static_cast<double>(n - 1));
    terms.push_back(v);
  }
  return terms;
}

// Family D — blind randomized sweep, to catch shapes the constructed families
// did not anticipate and to put the corrected bound under the same attack as
// the one it replaces. Varies size, peak magnitude (which drives the |log|S||
// reduction term), depth spread, sign mix, and ordering.
static std::vector<log_value> family_random(std::mt19937_64& rng) {
  std::uniform_int_distribution<int> size_pick(1000, 60000);
  std::uniform_real_distribution<double> peak_pick(-600.0, 600.0);
  std::uniform_real_distribution<double> spread_pick(0.5, 30.0);
  std::uniform_real_distribution<double> negfrac_pick(0.0, 0.5);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  const std::size_t n   = static_cast<std::size_t>(size_pick(rng));
  const double peak     = peak_pick(rng);
  const double spread   = spread_pick(rng);
  const double negfrac  = negfrac_pick(rng);
  std::uniform_real_distribution<double> depth(0.0, spread);

  std::vector<log_value> terms;
  terms.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    log_value v;
    v.sign = (unit(rng) < negfrac) ? -1.0 : 1.0;
    v.log_abs = peak - depth(rng);
    terms.push_back(v);
  }
  if (unit(rng) < 0.5) std::shuffle(terms.begin(), terms.end(), rng);
  else if (unit(rng) < 0.5)
    std::sort(terms.begin(), terms.end(),
              [](const log_value& a, const log_value& b) {
                return a.log_abs < b.log_abs; // ascending: maximise k
              });
  return terms;
}

// ---------------------------------------------------------------------------
// pos_accum. Stated contract: (n + 3k + 3) * u — no cond term (positive sums
// cannot cancel) and no reduction term. Its reduction is the same shape as
// rp_accum's, out.log_abs = m_log + log(sum), so the same |log|S||*u lands on
// it; unlike rp_accum there is no cond to hide behind, and unlike the n*u
// term it does not grow with n. A handful of terms at extreme magnitude is
// therefore the shortest path to a counterexample.
//
// The D term is a different story here and is measured, not assumed: making D
// large needs ~e^D terms at depth D, so D ~ ln(n) < n, and pos_accum's n*u
// term already covers it. The families below test that rather than trust it.
// ---------------------------------------------------------------------------
static verdict evaluate_pos(const std::vector<double>& logs) {
  pos_accum acc;
  dd_sum ref, mass, weighted_depth;
  double m = NINF;
  std::size_t k = 0, n = 0;
  bool first = true;

  for (double L : logs) {
    if (L == NINF) continue;
    if (L > m) {
      if (!first) ++k;
      m = L;
    }
    first = false;
    ++n;
    acc.add_log(L);
    const double lin = std::exp(L);
    ref.add(lin);
    mass.add(lin);
    weighted_depth.add(lin * (m - L));
  }

  verdict r;
  r.n      = n;
  r.k      = k;
  r.cond   = 1.0; // positive-only: sum|x_i| == |sum x_i| by construction
  r.depth  = weighted_depth.value() / mass.value();
  r.outmag = std::fabs(ref.log_abs());
  const double truth = ref.value();
  const double got   = acc.to_log_value().to_linear();
  r.observed = std::fabs(got - truth) / std::fabs(truth);
  r.bound    = (static_cast<double>(n) + 3.0 * static_cast<double>(k) + 3.0) * U;
  r.fixed    = (static_cast<double>(n) + 3.0 * static_cast<double>(k) + 3.0 +
                r.depth) * U + r.outmag * U;
  r.ratio       = r.observed / r.bound;
  r.ratio_fixed = r.observed / r.fixed;
  return r;
}

// P1 — few terms, extreme magnitude. n*u is tiny, |log|S|| is ~700.
// Magnitudes stay under exp()'s overflow so the linear comparison is valid
// and the reference carries no log() of its own.
static std::vector<double> pos_family_magnitude(double peak, std::size_t n) {
  return std::vector<double>(n, peak);
}

// P2 — depth cluster, the mechanism that broke rp_accum, against a bound that
// grows with n. Expected to be covered by n*u; measured to be sure.
static std::vector<double> pos_family_depth_cluster(double depth, std::size_t N) {
  const double peak = 10.5;
  double argerr = 0.0;
  const double L = worst_rounding_log_abs(peak, depth, &argerr);
  std::vector<double> logs;
  logs.reserve(N + 1);
  logs.push_back(peak);
  for (std::size_t i = 0; i < N; ++i) logs.push_back(L);
  return logs;
}

// P3 — random sweep over the two axes that matter: how many terms (which
// sets the n*u budget) and how far the result sits from 1.0.
static std::vector<double> pos_family_random(std::mt19937_64& rng) {
  std::uniform_int_distribution<int> size_pick(1, 400);
  std::uniform_real_distribution<double> peak_pick(-690.0, 690.0);
  std::uniform_real_distribution<double> spread_pick(0.0, 20.0);
  const std::size_t n  = static_cast<std::size_t>(size_pick(rng));
  const double peak    = peak_pick(rng);
  const double spread  = spread_pick(rng);
  std::uniform_real_distribution<double> depth(0.0, spread);
  std::vector<double> logs;
  logs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) logs.push_back(peak - depth(rng));
  return logs;
}

static void report(const char* label, const verdict& v) {
  std::printf("  %-40s %8zu %5zu %8.1e %6.1f %6.1f %9.2e %7.2f %7.2f\n", label,
              v.n, v.k, v.cond, v.depth, v.outmag, v.observed, v.ratio,
              v.ratio_fixed);
}

int main() {
  std::printf("adversarial search against rp_accum's error contract\n");
  std::printf("  stated:    cond*(3k+4)*u\n");
  std::printf("  corrected: cond*(3k+4+D)*u + |log|S||*u   "
              "(D = mass-weighted mean depth)\n\n");
  std::printf("  %-40s %8s %5s %8s %6s %6s %9s %7s %7s\n", "family", "n", "k",
              "cond", "D", "|logS|", "observed", "stated", "fixed");

  verdict worst, worst_fixed;
  const char* worst_label = "(none)";
  static char worst_buf[96];

  auto consider = [&](const char* label, const verdict& v) {
    report(label, v);
    if (v.ratio > worst.ratio) {
      worst = v;
      std::snprintf(worst_buf, sizeof worst_buf, "%s", label);
      worst_label = worst_buf;
    }
    if (v.ratio_fixed > worst_fixed.ratio_fixed) worst_fixed = v;
  };

  // --- Family A: coherent argument rounding in a single-depth cluster ------
  const double depths[] = {4.0, 8.0, 10.5, 12.0, 14.0};
  const std::size_t sizes[] = {10000, 1000000};
  for (double d : depths) {
    for (std::size_t N : sizes) {
      double argerr = 0.0;
      const std::vector<log_value> terms = family_depth_cluster(d, N, &argerr);
      char label[96];
      std::snprintf(label, sizeof label, "A depth=%.1f N=%zu (argerr %.1fu)", d,
                    N, argerr / U);
      consider(label, evaluate(terms));
    }
  }

  // --- Family B: the same coefficient error under cond amplification -------
  const double residuals[] = {1e-6, 1e-9};
  for (double res : residuals) {
    const std::vector<log_value> terms = family_cluster_cancel(10.5, 1000000, res);
    char label[96];
    std::snprintf(label, sizeof label, "B cluster+cancel residual=%.0e", res);
    consider(label, evaluate(terms));
  }

  // --- Family C: the derivation's own worst case for k ---------------------
  const std::size_t asc_sizes[] = {1000, 100000};
  for (std::size_t n : asc_sizes) {
    const std::vector<log_value> terms = family_ascending(n, 12.0);
    char label[96];
    std::snprintf(label, sizeof label, "C ascending n=%zu", n);
    consider(label, evaluate(terms));
  }

  // --- Family D: blind random sweep, scored against BOTH contracts --------
  std::mt19937_64 rng(0xB0DEADULL);
  verdict d_worst_stated, d_worst_fixed;
  const int trials = 400;
  int refutations = 0;
  for (int trial = 0; trial < trials; ++trial) {
    const verdict v = evaluate(family_random(rng));
    if (!(v.observed >= 0.0) || v.bound <= 0.0) continue; // skip degenerate
    if (v.ratio > 1.0) ++refutations;
    if (v.ratio > d_worst_stated.ratio) d_worst_stated = v;
    if (v.ratio_fixed > d_worst_fixed.ratio_fixed) d_worst_fixed = v;
  }
  consider("D random (worst vs stated)", d_worst_stated);
  consider("D random (worst vs corrected)", d_worst_fixed);
  std::printf("\n  family D: %d/%d random inputs refute the stated bound\n",
              refutations, trials);

  std::printf("\nrp_accum worst vs stated contract:    %.2f on %s\n",
              worst.ratio, worst_label);
  std::printf("  observed %.3e vs stated %.3e\n", worst.observed, worst.bound);
  std::printf("rp_accum worst vs corrected contract: %.2f\n",
              worst_fixed.ratio_fixed);

  // --- pos_accum ----------------------------------------------------------
  std::printf("\npos_accum\n");
  std::printf("  stated:    (n+3k+3)*u\n");
  std::printf("  corrected: (n+3k+3+D)*u + |log|S||*u\n\n");
  std::printf("  %-40s %8s %5s %8s %6s %6s %9s %7s %7s\n", "family", "n", "k",
              "cond", "D", "|logS|", "observed", "stated", "fixed");

  verdict pworst, pworst_fixed;
  const char* pworst_label = "(none)";
  static char pworst_buf[96];
  auto pconsider = [&](const char* label, const verdict& v) {
    report(label, v);
    if (v.ratio > pworst.ratio) {
      pworst = v;
      std::snprintf(pworst_buf, sizeof pworst_buf, "%s", label);
      pworst_label = pworst_buf;
    }
    if (v.ratio_fixed > pworst_fixed.ratio_fixed) pworst_fixed = v;
  };

  const double peaks[] = {690.0, 300.0, 50.0, 1.0};
  for (double peak : peaks) {
    char label[96];
    std::snprintf(label, sizeof label, "P1 magnitude peak=%.0f n=4", peak);
    pconsider(label, evaluate_pos(pos_family_magnitude(peak, 4)));
  }
  for (double d : {8.0, 12.0}) {
    for (std::size_t N : {std::size_t(10000), std::size_t(1000000)}) {
      char label[96];
      std::snprintf(label, sizeof label, "P2 depth=%.1f N=%zu", d, N);
      pconsider(label, evaluate_pos(pos_family_depth_cluster(d, N)));
    }
  }

  verdict p_rand_stated, p_rand_fixed;
  int p_refutations = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const verdict v = evaluate_pos(pos_family_random(rng));
    if (v.bound <= 0.0) continue;
    if (v.ratio > 1.0) ++p_refutations;
    if (v.ratio > p_rand_stated.ratio) p_rand_stated = v;
    if (v.ratio_fixed > p_rand_fixed.ratio_fixed) p_rand_fixed = v;
  }
  pconsider("P3 random (worst vs stated)", p_rand_stated);
  pconsider("P3 random (worst vs corrected)", p_rand_fixed);
  std::printf("\n  P3: %d/400 random inputs refute the stated bound\n",
              p_refutations);

  std::printf("\npos_accum worst vs stated contract:    %.2f on %s\n",
              pworst.ratio, pworst_label);
  std::printf("  observed %.3e vs stated %.3e\n", pworst.observed,
              pworst.bound);
  std::printf("pos_accum worst vs corrected contract: %.2f\n",
              pworst_fixed.ratio_fixed);

  // Each contract claims observed <= bound for every input. Anything above 1
  // (clear of the ~1.3 reference floor documented at the top) is a refutation.
  NC_CHECK(worst_fixed.ratio_fixed <= 1.0);
  NC_CHECK(pworst_fixed.ratio_fixed <= 1.0);
  std::puts("bound_search passed");
  return 0;
}
