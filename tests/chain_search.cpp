// chain_search.cpp — does log-form propagation win over a CHAIN?
//
// The stretch goal's case for propagation is chains. At a single conversion
// it loses by construction and is measured losing: 64 of 64 trials, 1.29x to
// 9.31x behind linear re-conversion (pass/PROTOTYPE.md). `t - L` carries
// u*|t - L| where a re-conversion carries one rounding. The claim is that
// following a computation across several operations amortizes that, because
// the alternative pays a materialization at every step.
//
// That claim is about ARITHMETIC, not about LLVM. It does not need a pass, a
// plugin, or a lattice to test, and testing it first is the point: the
// lattice exists to make chains possible, so building it to find out whether
// chains are worth having has the dependency backwards.
//
// DELIBERATELY A REPLICA. pass/emitted_bound_search.c opens by refusing to be
// one: "A bound derived against a hand-written replica would bound the
// replica." That rule is right for a bound on emitted code and does not apply
// here. This file asks whether log-domain chain arithmetic wins at all, not
// what the pass emits. Nothing here bounds the pass. A propagation rule that
// ships must be searched against the object the pass emits, in that file.
//
// PRE-REGISTERED, before the first run. Fixing the rule after the numbers
// exist is choosing the number first, which is how the existing 15x ceiling
// came to sit 8% above its own measured worst case.
//
//   Accuracy gate. At N = 3 there must be a declared band over which the
//   propagated form beats per-step materialize/reconvert at EVERY sampled
//   trial, and the advantage must still be present at N = 4, 6, 8. Every
//   trial, not a majority: a percentage invites fitting the band to the
//   result.
//
//   Bands, declared here, no others admitted later:
//     benign  |L| <= 5      where linear works and propagation offers least
//     mid     50..300       ordinary likelihood work
//     rescue  600..750      where the linear path returns 0.0 or NaN
//
//   Inputs are drawn by fixed-seed PRNG inside a band. No constructed
//   adversarial inputs, no ordering tricks, no hand-placed cancellation.
//   Those belong to a bound search, not to a viability question.
//
//   Per-step budget, derived before the sweep and never fitted to it. The
//   propagated form forms L1 = t - L0 and then L_{k+1} = L_k + lw_k, each
//   rounding at its own magnitude, and materializes once:
//
//       rel err  <=  u * ( |L1| + sum_k |L_{k+1}| )  +  u
//
//   The trailing u is the single final exp() under the 1-ulp libm assumption
//   the rest of this project already makes.
//
// AVAILABILITY IS NOT A WIN, AND THIS IS THE TRAP THIS FILE EXISTS TO AVOID.
// In the rescue band the linear and reconvert paths do not produce a worse
// number, they produce no number: exp(-800) is 0.0 and the chain is 0/0. A
// trial where the comparison target is unavailable is counted in its own
// bucket and can NEVER satisfy the accuracy gate. Conflating "the other path
// has no answer" with "propagation is more accurate" would manufacture a
// result, and the gate would become a test that cannot fail.
//
// EXIT CODE IS NOT THE VERDICT. This is a measurement, not a contract. It
// exits non-zero only when the harness itself is broken: the double-double
// reference fails its identities, or the trial accounting does not add up.
// The viability verdict prints as VERDICT and does not turn CI red, because
// the pre-registered outcome of a refutation is "publish and stop", not
// "leave the build red forever". Same shape as the cross-loop census, which
// shipped while the signal it measured was declined.
//
// REFERENCE IS DOUBLE-DOUBLE, NOT long double. MSVC's long double is 64-bit,
// and windows-msvc is a CI leg, so expl() would give no reference at all
// there. tests/dd_exp.h computes exp in double-double with its own argument
// reduction, assumes nothing about libm, and is validated by identities that
// need no external constant. Resolution ~1e-30 relative against long
// double's ~5.4e-20.
//
// SCOPE: MULTIPLICATIVE CHAINS ONLY. The chain here is one divide followed by
// multiplies, which is `fdiv -> fsub` then `fmul -> fadd`: the vocabulary
// Phase 2 was going to implement first. The third rule, `fadd -> logsumexp`,
// is NOT measured. A linear addition can cancel catastrophically where the
// log form cannot, so a win could still live there, and nothing in this file
// speaks to it. Measuring that is a different question, not a second attempt
// at this one.
//
// PRNG IS HAND-ROLLED UNIFORM. std::uniform_real_distribution is not
// reproducible across standard libraries (BENCHMARKS.md, "Second toolchain"),
// and this file's numbers are meant to be the same on all three CI legs.
// bound_search uses the distribution deliberately: a universal bound is
// refuted by any counterexample, so varying data helps it. A verdict that
// must agree across legs cannot use it.

#include "dd_exp.h"
#include "dd_sum.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

namespace {

constexpr double kU = 1.1102230246251565e-16; // 2^-53

// Uniform in [lo, hi) from the raw generator. 53 bits, exact, and identical
// on every standard library.
double uniform(std::mt19937_64 &rng, double lo, double hi) {
  const double x = static_cast<double>(rng() >> 11) * 0x1.0p-53;
  return lo + (hi - lo) * x;
}

struct Band {
  const char *name;
  double lo; // magnitude, applied negative
  double hi;
  bool signed_both; // benign spans both signs
};

const Band kBands[] = {
    {"benign", 0.0, 5.0, true},
    {"mid", 50.0, 300.0, false},
    {"rescue", 600.0, 750.0, false},
};

const int kSteps[] = {1, 3, 4, 6, 8};
constexpr int kTrialsPerCell = 256;

// One trial's inputs. All exact doubles, so the reference carries no input
// error and every difference measured is the chain's own.
struct Trial {
  double t;                // numerator log-magnitude
  double l0;               // denominator log-magnitude
  std::vector<double> lw;  // per-step weight log-magnitudes, N-1 of them
};

Trial draw(std::mt19937_64 &rng, const Band &b, int n) {
  Trial tr;
  const double a = uniform(rng, b.lo, b.hi);
  const double c = uniform(rng, b.lo, b.hi);
  if (b.signed_both) {
    tr.t = (rng() & 1) ? a : -a;
    tr.l0 = (rng() & 1) ? c : -c;
  } else {
    tr.t = -a;
    tr.l0 = -c;
  }
  // Weights stay modest whatever the band: a chain of rescue-magnitude
  // weights would run the result out of range for reasons that have nothing
  // to do with the representation question.
  for (int k = 0; k < n - 1; ++k) tr.lw.push_back(uniform(rng, -5.0, 5.0));
  return tr;
}

// 1. Linear throughout. What real code does after one materialization.
double chain_linear(const Trial &tr) {
  double v = std::exp(tr.t) / std::exp(tr.l0);
  for (double w : tr.lw) v *= std::exp(w);
  return v;
}

// 2. Materialize and reconvert at every step: log -> linear -> log.
double chain_reconvert(const Trial &tr) {
  double v = std::exp(tr.t) / std::exp(tr.l0);
  for (double w : tr.lw) v = std::exp(std::log(v) + w);
  return v;
}

// 3. Stay in log form, materialize once at the end.
double chain_staylog(const Trial &tr) {
  double l = tr.t - tr.l0;
  for (double w : tr.lw) l += w;
  return std::exp(l);
}

// The pre-registered budget for form 3.
double budget_staylog(const Trial &tr) {
  double l = tr.t - tr.l0;
  double acc = std::fabs(l);
  for (double w : tr.lw) {
    l += w;
    acc += std::fabs(l);
  }
  return kU * acc + kU;
}

// Exact log-magnitude of the result, in double-double. Inputs are exact, so
// this is the exact answer up to the pair's ~106 bits.
dd exact_log(const Trial &tr) {
  dd r = dd_sub(dd{tr.t, 0.0}, dd{tr.l0, 0.0});
  for (double w : tr.lw) r = dd_add(r, dd{w, 0.0});
  return r;
}

bool usable(double x) {
  return std::isfinite(x) && x != 0.0 && std::fabs(x) >= (std::numeric_limits<double>::min)();
}

struct Cell {
  int trials = 0;
  int available = 0;   // both staylog and reconvert produced a usable value
  int unavailable = 0; // reconvert had no answer where the reference does
  int staylog_wins = 0;
  int staylog_losses = 0;
  int ties = 0;
  int identical = 0; // rec and stl are the same double
  int vs_linear_compared = 0;
  int vs_linear_wins = 0;
  double vs_linear_ratio_max = 0.0;
  int unresolved = 0; // difference below the instrument's resolution
  double ratio_min = std::numeric_limits<double>::infinity();
  double ratio_max = 0.0;
  double budget_worst = 0.0;
  int linear_unavailable = 0;
};

// Resolution of the double-double reference, relative. Anything closer than
// this is not a measured difference. DERIVED from the instrument at startup
// (dd_exp_selftest's worst identity deviation, times a safety factor), never
// hardcoded: a threshold picked by hand decides how many trials are called
// ties, and the first version of this file set it 6 orders above what the
// reference can actually resolve.
double g_resolution = 0.0;

Cell run_cell(const Band &b, int n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  Cell c;
  for (int i = 0; i < kTrialsPerCell; ++i) {
    const Trial tr = draw(rng, b, n);
    c.trials++;

    const dd rlog = exact_log(tr);
    // Scale the comparison so the reference keeps its full width: at
    // log-magnitude -745 the low word would go subnormal and the pair would
    // silently degrade to ~61 bits (dd_exp.h, "WHY THE BIAS EXISTS").
    const int bias = dd_exp_bias(rlog.hi);
    const dd ref = dd_mul(dd_exp_scaled(rlog.hi, bias), dd_exp(rlog.lo));
    if (!usable(dd_to_double(ref))) continue; // reference itself out of range

    const double lin = chain_linear(tr);
    const double rec = chain_reconvert(tr);
    const double stl = chain_staylog(tr);

    if (!usable(lin)) c.linear_unavailable++;

    // ldexp by -bias is exact for a normal input, and meaningless for one
    // that already underflowed. Unusable values go to the unavailable bucket
    // rather than through the comparison.
    if (!usable(rec) || !usable(stl)) {
      c.unavailable++;
      continue;
    }
    c.available++;
    // Why ties happen, measured rather than inferred: a tie is usually the
    // two forms landing on the SAME double, not two different values whose
    // errors happen to match.
    if (rec == stl) c.identical++;

    const double e_rec = dd_rel_err(std::ldexp(rec, -bias), ref);
    const double e_stl = dd_rel_err(std::ldexp(stl, -bias), ref);

    // Against the real-world baseline too. `reconvert` is the form the design
    // argues with; `linear` is what code actually runs after one
    // materialization, and a win over reconvert that is a loss to linear is
    // not a reason to build anything.
    if (usable(lin)) {
      const double e_lin = dd_rel_err(std::ldexp(lin, -bias), ref);
      c.vs_linear_compared++;
      if (e_stl < e_lin) c.vs_linear_wins++;
      if (e_stl > 0.0 && e_lin > 0.0)
        c.vs_linear_ratio_max = (std::max)(c.vs_linear_ratio_max, e_stl / e_lin);
    }

    const double budget = budget_staylog(tr);
    if (budget > 0.0) c.budget_worst = (std::max)(c.budget_worst, e_stl / budget);

    if (std::fabs(e_stl - e_rec) < g_resolution) {
      c.unresolved++;
      c.ties++;
      continue;
    }
    if (e_stl == e_rec) {
      c.ties++;
      continue;
    }
    if (e_stl < e_rec) c.staylog_wins++;
    else c.staylog_losses++;

    if (e_stl > 0.0 && e_rec > 0.0) {
      const double ratio = e_stl / e_rec; // < 1 means propagation is better
      c.ratio_min = (std::min)(c.ratio_min, ratio);
      c.ratio_max = (std::max)(c.ratio_max, ratio);
    }
  }
  return c;
}

} // namespace

int main() {
  std::printf("chain_search — stretch goal Phase 1a, log-form chain viability\n\n");

  const double dd_worst = dd_exp_selftest();
  std::printf("INFO,dd_selftest,worst=%.3e\n", dd_worst);
  if (!(dd_worst < 1e-20)) {
    std::printf("FAIL,dd_reference_unusable,worst=%.3e\n", dd_worst);
    return 1; // harness broken: refuse to report, as bound_search does
  }
  // 100x the worst identity deviation the reference itself shows. Two
  // relative errors closer than this are one measurement, not two.
  g_resolution = dd_worst * 100.0;
  std::printf("INFO,resolution,derived=%.3e\n", g_resolution);

  int harness_fail = 0;
  constexpr int kNBands = static_cast<int>(sizeof(kBands) / sizeof(kBands[0]));
  constexpr int kNSteps = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

  // band -> N -> cell, kept whole so the gate reads the curve, not one point.
  Cell grid[kNBands][kNSteps];

  std::printf("\n%-7s %2s %6s %6s %7s %5s %5s %5s %9s %9s %7s %7s\n", "band",
              "N", "trials", "avail", "unavail", "win", "tie", "loss",
              "ratio-min", "ratio-max", "budget", "bitsame");

  for (int bi = 0; bi < kNBands; ++bi) {
    for (int si = 0; si < kNSteps; ++si) {
      // Seed depends on band and N so cells are independent and reproducible.
      const std::uint64_t seed =
          0x5eed0000ULL ^ (static_cast<std::uint64_t>(bi) << 32) ^
          (static_cast<std::uint64_t>(kSteps[si]) * 0x9e3779b97f4a7c15ULL);
      const Cell c = run_cell(kBands[bi], kSteps[si], seed);
      grid[bi][si] = c;

      std::printf("%-7s %2d %6d %6d %7d %5d %5d %5d %9.3g %9.3g %7.2f %7d\n",
                  kBands[bi].name, kSteps[si], c.trials, c.available,
                  c.unavailable, c.staylog_wins, c.ties, c.staylog_losses,
                  c.available ? c.ratio_min : 0.0,
                  c.available ? c.ratio_max : 0.0, c.budget_worst, c.identical);

      // Harness invariant: every trial is accounted for exactly once, or the
      // counts above are not a measurement of anything.
      if (c.available + c.unavailable > c.trials) {
        std::printf("FAIL,accounting,%s,N=%d\n", kBands[bi].name, kSteps[si]);
        harness_fail = 1;
      }
      if (c.staylog_wins + c.ties + c.staylog_losses != c.available) {
        std::printf("FAIL,win_tie_loss_does_not_sum_to_available,%s,N=%d\n",
                    kBands[bi].name, kSteps[si]);
        harness_fail = 1;
      }
    }
  }

  // Availability, reported separately and never counted as accuracy.
  std::printf("\n");
  for (int bi = 0; bi < kNBands; ++bi) {
    int lin_out = 0, rec_out = 0, avail = 0;
    for (int si = 0; si < kNSteps; ++si) {
      lin_out += grid[bi][si].linear_unavailable;
      rec_out += grid[bi][si].unavailable;
      avail += grid[bi][si].available;
    }
    std::printf("INFO,availability,%s,linear_no_answer=%d,reconvert_no_answer=%d,"
                "comparable=%d\n",
                kBands[bi].name, lin_out, rec_out, avail);
  }

  // Against the baseline real code actually runs.
  std::printf("\n");
  for (int bi = 0; bi < kNBands; ++bi) {
    for (int si = 0; si < kNSteps; ++si) {
      const Cell &c = grid[bi][si];
      std::printf("INFO,vs_linear,%s,N=%d,compared=%d,staylog_better=%d,"
                  "worst_ratio=%.3g\n",
                  kBands[bi].name, kSteps[si], c.vs_linear_compared,
                  c.vs_linear_wins, c.vs_linear_ratio_max);
    }
  }

  // The pre-registered budget, scored on every trial rather than asserted.
  double budget_worst = 0.0;
  for (int bi = 0; bi < kNBands; ++bi)
    for (int si = 0; si < kNSteps; ++si)
      budget_worst = (std::max)(budget_worst, grid[bi][si].budget_worst);
  std::printf("INFO,staylog_budget,worst_observed_over_bound=%.3f\n", budget_worst);
  if (budget_worst > 1.0)
    std::printf("INFO,staylog_budget_exceeded,the derived per-step term is "
                "refuted at %.3f\n",
                budget_worst);

  // The accuracy gate, evaluated exactly as pre-registered.
  int si3 = -1;
  for (int si = 0; si < kNSteps; ++si)
    if (kSteps[si] == 3) si3 = si;

  const char *winning_band = nullptr;
  for (int bi = 0; bi < kNBands && !winning_band; ++bi) {
    const Cell &c3 = grid[bi][si3];
    // "Every sampled trial", over the trials where a comparison exists.
    if (c3.available == 0 || c3.staylog_wins != c3.available) continue;
    bool holds_beyond = true;
    for (int si = 0; si < kNSteps; ++si) {
      if (kSteps[si] <= 3) continue;
      const Cell &c = grid[bi][si];
      if (c.available == 0 || c.staylog_wins != c.available) holds_beyond = false;
    }
    if (holds_beyond) winning_band = kBands[bi].name;
  }

  std::printf("\n");
  if (winning_band) {
    std::printf("VERDICT,chain_accuracy,PASS,band=%s\n", winning_band);
    std::printf("VERDICT,next,Phase 1b census, then the viability decision\n");
  } else {
    std::printf("VERDICT,chain_accuracy,FAIL,no band where propagation beats "
                "per-step reconversion at every trial for N>=3\n");
    std::printf("VERDICT,next,Phase 6 stop: publish the curve and the "
                "availability split\n");
    std::printf("VERDICT,mechanism,staylog pays u*|L| per step against "
                "linear's u, so the gap GROWS with chain length\n");
    std::printf("VERDICT,scope,multiplicative chains only; fadd -> logsumexp "
                "is unmeasured\n");
  }
  std::printf("VERDICT,exit_code_is_harness_health_only,not_the_verdict\n");

  return harness_fail;
}
