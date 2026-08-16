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
// Reference, and what it can resolve. Until 2026-08-16 this file summed
// exp(L_i) computed in plain double and then collapsed the dd accumulator
// with value(), which put TWO floors under every measurement: ~1u from
// double exp() per term (measured since: worst 0.99u), and up to u/2 from
// "truth" being a double at the moment of comparison. The second is
// structural and no libm improvement removes it. Ratios below ~1.3 were
// unreadable, which is what this comment used to say.
//
// Both are gone. dd_exp.h computes exp in double-double with its own
// argument reduction and series, assuming nothing about libm, and the
// reference stays wide through the subtraction. Terms are scaled by the
// peak's binary exponent so the dominant ones carry the full 106 bits.
// dd_exp_selftest() runs identity checks that need no external constant and
// this file refuses to report if they do not pass. Measured resolution:
// ~1e-30 relative, about 1e-14 u.
#include "test_common.h"
#include "dd_sum.h"
#include "dd_exp.h"
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
  double outmag = 0.0;   // |log|S||, the reduction's stated lever arm
  // |log|net||, the OTHER addend of m_log + log|net|. When the two nearly
  // cancel, |log|S|| goes to zero while this one does not, and it is this
  // one that sets log(net)'s absolute rounding error.
  double lognet = 0.0;
  double fixed2 = 0.0;   // corrected again: + |log|net||*u
  double ratio_fixed2 = 0.0;
  double observed = 0.0;
  // The same error, measured on what the accumulator REPRESENTS rather than
  // on to_linear()'s output. to_linear() is one more exp(), ~1u, and the
  // contract is written about out.log_abs. Keeping both columns is the only
  // way to tell a refuted contract from a measurement charging it for a
  // rounding it never promised.
  double observed_repr = 0.0;
  double bound = 0.0;    // v0.2 stated contract
  double fixed = 0.0;    // corrected contract
  double ratio = 0.0;
  double ratio_fixed = 0.0;
  double ratio_repr = 0.0;
};

// The peak log-magnitude, which sets the reference's scaling bias.
static double peak_of(const std::vector<log_value>& terms) {
  double peak = NINF;
  for (const log_value& v : terms)
    if (!v.is_zero() && v.log_abs > peak) peak = v.log_abs;
  return peak;
}

// Run the terms through rp_accum and score them against the stated contract.
static verdict evaluate(const std::vector<log_value>& terms) {
  rp_accum acc;
  const int bias = dd_exp_bias(peak_of(terms));
  dd ref{}, mass{}, weighted_depth{};
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
    // |term|, in the scaled domain: exp(log_abs) * 2^-bias.
    const dd lin = dd_exp_scaled(v.log_abs, bias);
    ref  = dd_add(ref, v.sign < 0.0 ? dd_neg(lin) : lin);
    mass = dd_add(mass, lin);
    // Depth at insertion: how far below the running reference this term sat
    // when its exp() argument was formed. That difference is what rounds.
    weighted_depth = dd_add(weighted_depth, dd_mul_d(lin, m - v.log_abs));
  }

  verdict r;
  r.n        = terms.size();
  r.k        = k;
  r.cond     = dd_to_double(mass) / std::fabs(dd_to_double(ref));
  r.depth    = dd_to_double(weighted_depth) / dd_to_double(mass);
  // log|S| = log|ref| + bias*ln2, the bias being an exact power of two.
  const double logS_signed = dd_log_abs(ref) + bias * dd_ln2().hi;
  r.outmag   = std::fabs(logS_signed);
  // net = S / exp(m_log): the scaled sum the reduction takes the log of.
  r.lognet   = std::fabs(logS_signed - m);
  const log_value out = acc.to_log_value();
  const double got = out.to_linear();
  // Exact: got is normal and so is got*2^-bias, by construction of bias.
  r.observed = dd_rel_err(std::ldexp(got, -bias), ref);
  // What the accumulator represents, exponentiated exactly instead of by
  // to_linear(): isolates the accumulator from the final conversion.
  {
    const dd repr = dd_exp_scaled(out.log_abs, bias);
    const dd d = dd_sub(out.sign < 0.0 ? dd_neg(repr) : repr, ref);
    r.observed_repr = std::fabs(dd_to_double(d) / dd_to_double(ref));
  }
  r.bound    = r.cond * (3.0 * static_cast<double>(k) + 4.0) * U;
  // Corrected contract: the argument-rounding term rides with the mass and is
  // amplified by cond like every other coefficient perturbation; the final
  // reduction m_log + log|net| rounds once, in log space, and lands on the
  // linear result directly rather than through cond.
  r.fixed    = r.cond * (3.0 * static_cast<double>(k) + 4.0 + r.depth) * U +
               r.outmag * U;
  r.fixed2       = r.fixed + r.lognet * U;
  r.ratio        = r.observed / r.bound;
  r.ratio_fixed  = r.observed / r.fixed;
  r.ratio_fixed2 = r.observed / r.fixed2;
  r.ratio_repr   = r.observed_repr / r.fixed;
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
  double peak = NINF;
  for (double L : logs)
    if (L != NINF && L > peak) peak = L;
  const int bias = dd_exp_bias(peak);
  dd ref{}, mass{}, weighted_depth{};
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
    const dd lin = dd_exp_scaled(L, bias);
    ref  = dd_add(ref, lin);
    mass = dd_add(mass, lin);
    weighted_depth = dd_add(weighted_depth, dd_mul_d(lin, m - L));
  }

  verdict r;
  r.n      = n;
  r.k      = k;
  r.cond   = 1.0; // positive-only: sum|x_i| == |sum x_i| by construction
  r.depth  = dd_to_double(weighted_depth) / dd_to_double(mass);
  const double logS_signed = dd_log_abs(ref) + bias * dd_ln2().hi;
  r.outmag = std::fabs(logS_signed);
  r.lognet = std::fabs(logS_signed - m);
  const log_value out = acc.to_log_value();
  const double got = out.to_linear();
  r.observed = dd_rel_err(std::ldexp(got, -bias), ref);
  {
    const dd repr = dd_exp_scaled(out.log_abs, bias);
    r.observed_repr =
        std::fabs(dd_to_double(dd_sub(repr, ref)) / dd_to_double(ref));
  }
  r.bound    = (static_cast<double>(n) + 3.0 * static_cast<double>(k) + 3.0) * U;
  r.fixed    = (static_cast<double>(n) + 3.0 * static_cast<double>(k) + 3.0 +
                r.depth) * U + r.outmag * U;
  r.fixed2       = r.fixed + r.lognet * U;
  r.ratio        = r.observed / r.bound;
  r.ratio_fixed  = r.observed / r.fixed;
  r.ratio_fixed2 = r.observed / r.fixed2;
  r.ratio_repr   = r.observed_repr / r.fixed;
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

// P4 — ascending, k == n-1. pos_accum's 3k*u term had never been searched at
// the input that maximizes k: P1 and P2 hold the reference fixed after the
// first term, and P3's random ordering leaves k at its incidental O(ln n).
static std::vector<double> pos_family_ascending(std::size_t n, double jump,
                                                double base) {
  std::vector<double> logs;
  logs.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    logs.push_back(base + jump * static_cast<double>(i));
  return logs;
}

// P5 / C2 — plateau then step, normalized so the total is ~1.0.
//
// This is the family that exposes what a rescale really costs. The rescale
// forms exp(m_log - log_abs), whose ARGUMENT is off by up to u*J for a jump
// of J log-units; the derivation charges 2u+u for the whole factor. The
// error that survives is the old sum's mass share after the jump,
// s/(s + e^J), so the contribution is J*s/(s+e^J)*u, maximized where
// s = e^J(J-1) — which takes ~e^J terms to build. Hence: a plateau of N
// terms to grow s, then one step of J.
//
// The normalization matters. With the plateau at 0 the total is ~e^J and
// |log|S||*u pays for the jump on its own, hiding the mechanism. Shifting so
// the total is 1.0 removes that cover and leaves the flat (3k+4)*u budget.
static std::vector<double> family_plateau_step(std::size_t N, double J) {
  const double shift = -std::log(static_cast<double>(N) + std::exp(J));
  std::vector<double> logs(N, shift);
  logs.push_back(shift + J);
  return logs;
}

// Family E — n equal terms at L = -log(n). The cleanest maximizer of the
// mechanism the plateau family exposed, and the one that isolates it:
//
//   net = n exactly (each scaled term is exp(0) = 1, and integer adds below
//   2^53 are exact), so there is NO summation error at all;
//   m_log = -log(n), and log(net) = +log(n), so the final reduction is a
//   near-total cancellation and |log|S|| = 0;
//   k = 0, D = 0, cond = 1, so the entire stated budget is 4u.
//
// Everything that remains is the rounding of log(net) and m_log, each of
// absolute size |log n|*u. The stated contract charges for neither, because
// it charges |log|S||*u and |log|S|| is zero here by construction.
static std::vector<double> family_equal_normalized(std::size_t n) {
  return std::vector<double>(n, -std::log(static_cast<double>(n)));
}

static std::vector<log_value> to_positive_terms(const std::vector<double>& logs) {
  std::vector<log_value> terms;
  terms.reserve(logs.size());
  for (double L : logs) {
    log_value v;
    v.sign = 1.0;
    v.log_abs = L;
    terms.push_back(v);
  }
  return terms;
}

static void report(const char* label, const verdict& v) {
  std::printf("  %-38s %8zu %5zu %6.1f %6.1f %7.1f %9.2e %6.2f %6.2f %6.2f\n",
              label, v.n, v.k, v.depth, v.outmag, v.lognet, v.observed,
              v.ratio_fixed, v.ratio_repr, v.ratio_fixed2);
}

int main() {
  // The reference judges every claim below, so it is checked first, by
  // identities needing no external constant (dd_exp.h). A search reporting
  // against a broken reference is worse than no search.
  {
    const double dev = dd_exp_selftest();
    std::printf("reference selftest: worst identity deviation %.3e (%.2e u)\n",
                dev, dev / U);
    if (!(dev < 1e-25)) {
      std::printf("FAIL: dd_exp reference does not satisfy its identities\n");
      return 1;
    }
  }

  std::printf("adversarial search against rp_accum's error contract\n");
  std::printf("  stated:    cond*(3k+4)*u\n");
  std::printf("  corrected: cond*(3k+4+D)*u + |log|S||*u   "
              "(D = mass-weighted mean depth)\n\n");
  std::printf("  %-40s %8s %5s %8s %6s %6s %9s %7s %7s\n", "family", "n", "k",
              "cond", "D", "|logS|", "observed", "stated", "fixed");

  verdict worst, worst_fixed, worst_fixed2;
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
    if (v.ratio_fixed2 > worst_fixed2.ratio_fixed2) worst_fixed2 = v;
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

  // --- Family C2: plateau then step, normalized -----------------------------
  // rp_accum is the accumulator with something to lose here. It is Neumaier
  // compensated, so it carries no n*u term to absorb a rescale's argument
  // rounding: at k=1, cond=1 and |log|S|| ~ 0 its entire budget is a flat 7u.
  for (std::size_t N : {std::size_t(1000), std::size_t(100000)}) {
    for (double J : {2.0, 6.0, 10.0, 14.0}) {
      char label[96];
      std::snprintf(label, sizeof label, "C2 plateau N=%zu J=%.0f", N, J);
      consider(label, evaluate(to_positive_terms(family_plateau_step(N, J))));
    }
  }

  // --- Family E: the reduction's cancellation, isolated --------------------
  // Swept, not sampled. The error here is the rounding of log(net) and of the
  // final add, and whether a given n rounds badly is luck; a handful of round
  // n values finds a fraction of the worst case. 400 sizes, geometric.
  {
    verdict e_worst;
    std::size_t e_at = 0;
    for (int i = 0; i < 400; ++i) {
      const double t = static_cast<double>(i) / 399.0;
      const std::size_t n =
          static_cast<std::size_t>(std::llround(std::pow(200000.0, t))) + 1;
      const verdict v = evaluate(to_positive_terms(family_equal_normalized(n)));
      if (v.ratio_fixed > e_worst.ratio_fixed) { e_worst = v; e_at = n; }
    }
    char label[96];
    std::snprintf(label, sizeof label, "E equal-normalized (worst of 400, n=%zu)",
                  e_at);
    consider(label, e_worst);
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

  verdict pworst, pworst_fixed, pworst_fixed2;
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
    if (v.ratio_fixed2 > pworst_fixed2.ratio_fixed2) pworst_fixed2 = v;
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

  // P4 — ascending, the k-maximizing input this file never had. The jump is
  // capped so the whole ascent fits inside exp()'s range.
  for (std::size_t n : {std::size_t(64), std::size_t(4096)}) {
    for (int j = 1; j <= 12; ++j) {
      const double jmax = 1370.0 / static_cast<double>(n - 1);
      const double jump = jmax * static_cast<double>(j) / 12.0;
      char label[96];
      std::snprintf(label, sizeof label, "P4 ascending n=%zu jump=%.3f", n, jump);
      pconsider(label, evaluate_pos(pos_family_ascending(n, jump, -685.0)));
    }
  }

  // P5 — plateau then step, normalized so |log|S|| cannot pay for the jump.
  for (std::size_t N : {std::size_t(1000), std::size_t(100000)}) {
    for (double J : {2.0, 6.0, 10.0, 14.0}) {
      char label[96];
      std::snprintf(label, sizeof label, "P5 plateau N=%zu J=%.0f", N, J);
      pconsider(label, evaluate_pos(family_plateau_step(N, J)));
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

  // --- representation floor -----------------------------------------------
  // Both accumulator bounds were broken by the same |log|S||*u term, which is
  // not an accumulator defect: log_abs is a double, so a value of magnitude
  // e^700 sits on a grid of spacing ulp(700) ~ 1.1e-13 and cannot be
  // represented to better than ~512u relative however it was computed. The
  // pairwise ops inherit it. These checks need no reference: the rounding
  // error of a single add is recovered exactly by TwoSum.
  std::printf("\nrepresentation floor (log_abs is a double)\n");
  std::printf("  %-40s %10s %10s %12s\n", "claim under test", "worst err",
              "in u", "rel linear");

  // log_mul / log_div: r.log_abs = fl(la +/- lb). Documented as "exact in log
  // domain". The mapping is exact; the arithmetic implementing it is not.
  double mul_worst = 0.0, mul_at = 0.0;
  bool mul_bound_holds = true;
  for (int i = 0; i < 4000; ++i) {
    const double la = -700.0 + 0.35 * i;
    for (int j = 0; j < 8; ++j) {
      const double lb = std::nextafter(400.0 + 13.7 * j, 1e9);
      const double e = std::fabs(two_sum_err(la, lb));
      if (e > mul_worst) { mul_worst = e; mul_at = la + lb; }
      if (e > U * std::fabs(la + lb)) mul_bound_holds = false;
    }
  }
  std::printf("  %-40s %10.3e %10.1f %12.3e\n", "log_mul exact (refuted)",
              mul_worst, mul_worst / U, mul_worst);

  // logsumexp2: the final a + log1p(exp(b-a)) lands on the same grid.
  double lse_worst = 0.0;
  bool lse_bound_holds = true;
  for (int i = 0; i < 4000; ++i) {
    double a = -700.0 + 0.35 * i;
    double b = a - 0.5 - 0.001 * i;
    if (b > a) std::swap(a, b);
    const double t = std::log1p(std::exp(b - a));
    const double e = std::fabs(two_sum_err(a, t));
    if (e > lse_worst) lse_worst = e;
    if (e > U * std::fabs(a + t)) lse_bound_holds = false;
  }
  std::printf("  %-40s %10.3e %10.1f %12.3e\n", "logsumexp2 final add",
              lse_worst, lse_worst / U, lse_worst);

  // Round trip log_value(x).to_linear(): log then exp, each landing on the
  // same grid. Sample x with ARBITRARY mantissas — seeding with x = exp(l)
  // for representable l makes the trip exact by construction and reports a
  // meaningless 0.0, since log then recovers l exactly.
  double rt_worst = 0.0, rt_at = 0.0;
  bool rt_bound_holds = true;
  std::uniform_real_distribution<double> mantissa(1.0, 2.0);
  for (int e = -1000; e < 1000; ++e) {
    for (int t = 0; t < 20; ++t) {
      const double x = std::ldexp(mantissa(rng), e);
      if (!(x > 0.0) || std::isinf(x)) continue;
      const double back = log_value(x).to_linear();
      if (!(back > 0.0) || std::isinf(back)) continue;
      const double rel = std::fabs(back - x) / x;
      if (rel > rt_worst) { rt_worst = rel; rt_at = std::log(x); }
      // Corrected claim: the trip costs |log|x||*u, plus exp's own ulp.
      if (rel > (std::fabs(std::log(x)) + 2.0) * U) rt_bound_holds = false;
    }
  }
  std::printf("  %-40s %10.3e %10.1f %12.3e\n", "log_value round trip",
              rt_worst, rt_worst / U, rt_worst);
  std::printf("\n  worst log_mul rounding at log|product| = %.0f; "
              "worst round trip at log|x| = %.0f\n", mul_at, rt_at);
  // Each contract claims observed <= bound for every input. The reference
  // resolves to ~1e-14 u, so anything above 1 is a refutation outright.
  //
  // ratio_fixed is the 2026-08-15 form, cond*(3k+4+D)*u + |log|S||*u. It is
  // REFUTED, at 1.99x, by family E: n equal positive terms at L = -log(n),
  // where cond = 1, k = 0, D = 0 and |log|S|| = 0, so the whole budget is 4u.
  // The reduction m_log + log(net) is a near-total cancellation there, and
  // log(net) rounds at ITS OWN magnitude, |log|net|| = log n = 12.0, which
  // |log|S|| does not see. Asserting the refuted form here would be asserting
  // something known false, so the assertion is on the corrected form.
  NC_CHECK(worst_fixed2.ratio_fixed2 <= 1.0);
  NC_CHECK(pworst_fixed2.ratio_fixed2 <= 1.0);
  // ...and the refutation is pinned, so nobody restores the old form quietly.
  NC_CHECK(worst_fixed.ratio_fixed > 1.0);
  // The corrected pairwise claim: error <= u * |result in log space|.
  NC_CHECK(mul_bound_holds);
  NC_CHECK(lse_bound_holds);
  // And the old claim really is false, so nobody restores it: log_mul rounds.
  NC_CHECK(mul_worst > 0.0);
  NC_CHECK(rt_bound_holds);
  // The round trip is not free either. If this ever reads 0, the sampling has
  // regressed to x = exp(representable), which cannot fail by construction.
  NC_CHECK(rt_worst > 0.0);
  std::puts("bound_search passed");
  return 0;
}
