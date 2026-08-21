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
// ===========================================================================
// EXPERIMENT 2: fadd -> logsumexp. PRE-REGISTERED BEFORE IMPLEMENTATION.
// ===========================================================================
//
// Experiment 1 above refutes the chain hypothesis for MULTIPLICATIVE chains.
// The third vocabulary rule is untested, and it is the one with a structural
// argument: a linear addition can cancel catastrophically, and a summation
// can lose small terms or leave range entirely, where a log form does not.
// This is a separate question, not a second attempt at the first one.
//
// H, the hypothesis under test. For ADDITIVE chains there is a declared band
// and family where staying in log form beats materializing and adding
// linearly, on accuracy, at every sampled trial for N >= 3.
//
// FALSIFICATION STANCE. H gets its best shot: the families are the ones where
// linear addition is classically worst, and the log side is scored at the
// better of two implementations rather than one. H then survives only if it
// wins EVERYWHERE in some cell. A single loss in a cell kills that cell.
//
//   families, declared here
//     flat    terms in a narrow +-2 window, all positive
//     wide    terms spread across the whole band, all positive. The textbook
//             argument for logsumexp: dynamic range a linear sum cannot hold
//     cancel  signed, half positive, magnitudes clustered so the sum nearly
//             cancels. Where linear summation is classically worst
//
//   bands: the same three as experiment 1, for the same reasons.
//   N in {3, 4, 6, 8, 16}. A sum of one term is degenerate, so the ladder
//   starts at 3 and reaches further than the multiplicative one: additive
//   chains are naturally longer.
//
// FORMS COMPARED
//   linear    v = sum of +-exp(l_i), in double. What code runs today.
//   logfold   fold logrange::log_add pairwise. The straight-line model: a
//             rewritten `fadd` chain is pairwise, not a loop reduction.
//   logacc    pos_accum (positive families) or rp_accum (cancel), the
//             runtime's accumulators. Included because Phase 4 makes
//             log_math.h the semantic reference for what a lowering emits,
//             and because scoring the log side at its best is what makes a
//             refutation mean something.
//   reference double-double.
//
// THE COMPARISON IS APPLES TO APPLES, AND THAT NEEDED AN ARGUMENT.
// The linear form is scored by RELATIVE error of its linear value. The log
// forms are scored by ABSOLUTE error of their log-magnitude. Those are the
// same quantity: absolute error in the log domain IS relative error in the
// linear domain, which is the property the intent cites in log's favour.
// Scoring the log form by materializing it would charge propagation for the
// exp() it exists to avoid, and scoring the linear form by taking its log
// would charge it for a conversion it never performs. Neither side pays for
// a step the real scenario does not contain.
//
// AVAILABILITY IS STILL NOT A WIN. A trial where the linear form returns 0,
// inf, NaN, or a subnormal while the reference is finite and normal goes to
// the unavailable bucket and can never satisfy the gate. Same rule as
// experiment 1, for the same reason.
//
// THE REFERENCE NEEDED A LOG IT DID NOT HAVE. dd_exp.h's dd_log_abs computes
// log(|hi|) + log1p(lo/hi) in plain double, so its absolute error is ~u*|log|,
// the same order as the quantity being measured. A local Newton refinement
// gives the reference ~1e-32 absolute instead. It is local to this file
// rather than added to dd_exp.h, because that header is shared with
// test_accuracy and bound_search and changing it would put their published
// numbers back in question.
//
// CONTINGENCY. The compiler-propagation branch is gated on this result.
// H refuted means the lattice, propagate=mul, and the remaining vocabulary
// are not built, and the wall is the deliverable.
//
// PRNG IS HAND-ROLLED UNIFORM. std::uniform_real_distribution is not
// reproducible across standard libraries (BENCHMARKS.md, "Second toolchain"),
// and this file's numbers are meant to be the same on all three CI legs.
// bound_search uses the distribution deliberately: a universal bound is
// refuted by any counterexample, so varying data helps it. A verdict that
// must agree across legs cannot use it.

#include "dd_exp.h"
#include "dd_sum.h"
#include <logrange/log_math.h>
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

// ---------------------------------------------------------------------------
// Experiment 2: fadd -> logsumexp.
// ---------------------------------------------------------------------------

// log(x) for a double-double x, refined by one Newton step against dd_exp.
//
//   y = y0 + (x*exp(-y0) - 1),  y0 = double log(x)
//
// x*exp(-y0) is 1 + delta with |delta| ~ u, and log(1+delta) = delta - delta^2/2
// with the quadratic term ~5e-33. So one step lands near 1e-32 absolute, well
// under the ~1e-16 quantities this experiment compares.
dd dd_log_refined(dd x) {
  const double y0 = std::log(dd_to_double(x));
  const dd delta = dd_sub(dd_mul(x, dd_exp(-y0)), dd{1.0, 0.0});
  return dd_add(dd{y0, 0.0}, delta);
}

enum class Family { Flat, Wide, Cancel };

const char *family_name(Family f) {
  switch (f) {
  case Family::Flat: return "flat";
  case Family::Wide: return "wide";
  case Family::Cancel: return "cancel";
  }
  return "?";
}

struct AddTrial {
  std::vector<double> l;    // per-term log-magnitudes, exact doubles
  std::vector<double> sgn;  // +1 or -1
};

AddTrial draw_add(std::mt19937_64 &rng, const Band &b, Family f, int n) {
  AddTrial tr;
  // Anchor the family inside the band, then place terms relative to it.
  const double anchor = uniform(rng, b.lo, b.hi);
  const double base = b.signed_both ? ((rng() & 1) ? anchor : -anchor) : -anchor;
  for (int i = 0; i < n; ++i) {
    double li = base;
    if (f == Family::Wide) {
      // Spread across the band's own width: the dynamic range a linear sum
      // is classically unable to hold.
      li = base - uniform(rng, 0.0, (b.hi - b.lo) + 1.0);
    } else {
      li = base + uniform(rng, -2.0, 2.0);
    }
    tr.l.push_back(li);
    if (f == Family::Cancel) {
      // Half negative, clustered magnitudes: the sum nearly cancels and the
      // condition number is large.
      tr.sgn.push_back((i % 2 == 0) ? 1.0 : -1.0);
    } else {
      tr.sgn.push_back(1.0);
    }
  }
  return tr;
}

double add_linear(const AddTrial &tr) {
  double v = 0.0;
  for (std::size_t i = 0; i < tr.l.size(); ++i) v += tr.sgn[i] * std::exp(tr.l[i]);
  return v;
}

// Pairwise fold with the shipped log_add: the straight-line rewrite model.
logrange::log_value add_logfold(const AddTrial &tr) {
  logrange::log_value acc; // zero
  for (std::size_t i = 0; i < tr.l.size(); ++i) {
    logrange::log_value t;
    t.log_abs = tr.l[i];
    t.sign = tr.sgn[i];
    acc = logrange::log_add(acc, t);
  }
  return acc;
}

// The runtime's accumulators, which is the log side at its best.
logrange::log_value add_logacc(const AddTrial &tr, Family f) {
  if (f == Family::Cancel) {
    logrange::rp_accum acc;
    for (std::size_t i = 0; i < tr.l.size(); ++i) {
      logrange::log_value t;
      t.log_abs = tr.l[i];
      t.sign = tr.sgn[i];
      acc.add(t);
    }
    return acc.to_log_value();
  }
  logrange::pos_accum acc;
  for (double li : tr.l) acc.add_log(li);
  return acc.to_log_value();
}

// Exact sum and its exact log-magnitude, in double-double.
void add_reference(const AddTrial &tr, int bias, dd &sum_scaled, dd &log_exact) {
  dd s{0.0, 0.0};
  for (std::size_t i = 0; i < tr.l.size(); ++i) {
    dd term = dd_exp_scaled(tr.l[i], bias);
    if (tr.sgn[i] < 0.0) term = dd_neg(term);
    s = dd_add(s, term);
  }
  sum_scaled = s;
  // log|S| = log|S_scaled| + bias*ln2, kept wide.
  dd labs = s;
  if (dd_to_double(labs) < 0.0) labs = dd_neg(labs);
  log_exact = dd_add(dd_log_refined(labs), dd_mul_d(dd_ln2(), static_cast<double>(bias)));
}

struct AddCell {
  int trials = 0;
  int available = 0;
  int unavailable = 0;
  int log_wins = 0;   // best log form strictly better than linear
  int log_losses = 0;
  int ties = 0;
  double ratio_max = 0.0; // best-log error / linear error; < 1 means log wins
  double ratio_min = std::numeric_limits<double>::infinity();
};

AddCell run_add_cell(const Band &b, Family f, int n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  AddCell c;
  for (int i = 0; i < kTrialsPerCell; ++i) {
    const AddTrial tr = draw_add(rng, b, f, n);
    c.trials++;

    double peak = tr.l[0];
    for (double li : tr.l) peak = (std::max)(peak, li);
    const int bias = dd_exp_bias(peak);

    dd sum_scaled{0.0, 0.0}, log_exact{0.0, 0.0};
    add_reference(tr, bias, sum_scaled, log_exact);
    const double sref = dd_to_double(sum_scaled);
    if (!std::isfinite(sref) || sref == 0.0) continue; // no reference to score

    const double lin = add_linear(tr);
    const logrange::log_value lf = add_logfold(tr);
    const logrange::log_value la = add_logacc(tr, f);

    if (!usable(lin)) {
      c.unavailable++;
      continue;
    }
    c.available++;

    // Linear: relative error of the linear value, scaled exactly by 2^-bias.
    const double e_lin = dd_rel_err(std::ldexp(lin, -bias), sum_scaled);

    // Log: absolute error of the log-magnitude, which is the same quantity.
    //
    // The subtraction is formed in double-double and only the small result is
    // collapsed. Collapsing the reference FIRST and subtracting in double
    // quantizes every sub-ulp error to exactly 0, which is not a measurement
    // of accuracy but of whether two doubles happen to be the same one. The
    // first version of this experiment did that and reported a PASS built
    // entirely on those zeros: every winning cell recorded no ratio at all,
    // because e_log was 0 on every trial. dd_sum.h states the rule the other
    // way round for bound_search, and for the same reason.
    const double e_fold =
        std::fabs(dd_to_double(dd_sub(dd{lf.log_abs, 0.0}, log_exact)));
    const double e_acc =
        std::fabs(dd_to_double(dd_sub(dd{la.log_abs, 0.0}, log_exact)));
    const double e_log = (std::min)(e_fold, e_acc); // the log side at its best

    if (std::fabs(e_log - e_lin) < g_resolution) {
      c.ties++;
      continue;
    }
    if (e_log < e_lin) c.log_wins++;
    else c.log_losses++;

    if (e_log > 0.0 && e_lin > 0.0) {
      const double ratio = e_log / e_lin;
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
  // =========================================================================
  // Experiment 2: fadd -> logsumexp. Gate as pre-registered in the header.
  // =========================================================================
  const Family kFamilies[] = {Family::Flat, Family::Wide, Family::Cancel};
  const int kAddSteps[] = {3, 4, 6, 8, 16};
  constexpr int kNFam = 3;
  constexpr int kNAddSteps = 5;

  std::printf("\n=== experiment 2: fadd -> logsumexp ===\n");
  std::printf("\n%-7s %-7s %2s %6s %6s %7s %5s %5s %5s %9s %9s\n", "family",
              "band", "N", "trials", "avail", "unavail", "win", "tie", "loss",
              "ratio-min", "ratio-max");

  AddCell agrid[kNFam][kNBands][kNAddSteps];
  for (int fi = 0; fi < kNFam; ++fi) {
    for (int bi = 0; bi < kNBands; ++bi) {
      for (int si = 0; si < kNAddSteps; ++si) {
        const std::uint64_t seed =
            0xadd00000ULL ^ (static_cast<std::uint64_t>(fi) << 40) ^
            (static_cast<std::uint64_t>(bi) << 32) ^
            (static_cast<std::uint64_t>(kAddSteps[si]) * 0x9e3779b97f4a7c15ULL);
        const AddCell c =
            run_add_cell(kBands[bi], kFamilies[fi], kAddSteps[si], seed);
        agrid[fi][bi][si] = c;
        std::printf("%-7s %-7s %2d %6d %6d %7d %5d %5d %5d %9.3g %9.3g\n",
                    family_name(kFamilies[fi]), kBands[bi].name, kAddSteps[si],
                    c.trials, c.available, c.unavailable, c.log_wins, c.ties,
                    c.log_losses, c.available ? c.ratio_min : 0.0,
                    c.available ? c.ratio_max : 0.0);
        if (c.log_wins + c.ties + c.log_losses != c.available) {
          std::printf("FAIL,add_accounting,%s,%s,N=%d\n",
                      family_name(kFamilies[fi]), kBands[bi].name,
                      kAddSteps[si]);
          harness_fail = 1;
        }
      }
    }
  }

  const char *add_cell = nullptr;
  static char add_cell_buf[64];
  for (int fi = 0; fi < kNFam && !add_cell; ++fi) {
    for (int bi = 0; bi < kNBands && !add_cell; ++bi) {
      bool all = true;
      for (int si = 0; si < kNAddSteps; ++si) {
        const AddCell &c = agrid[fi][bi][si];
        if (c.available == 0 || c.log_wins != c.available) all = false;
      }
      if (all) {
        std::snprintf(add_cell_buf, sizeof(add_cell_buf), "%s/%s",
                      family_name(kFamilies[fi]), kBands[bi].name);
        add_cell = add_cell_buf;
      }
    }
  }

  std::printf("\n");
  if (add_cell) {
    std::printf("VERDICT,add_accuracy,PASS,cell=%s\n", add_cell);
    std::printf("VERDICT,propagation_branch,PROCEEDS,H survived its "
                "falsification\n");
  } else {
    std::printf("VERDICT,add_accuracy,FAIL,no family/band where the log form "
                "beats linear at every trial for N>=3\n");
    std::printf("VERDICT,propagation_branch,STOPS,both vocabulary rules "
                "measured and neither wins on accuracy\n");
  }
  std::printf("VERDICT,exit_code_is_harness_health_only,not_the_verdict\n");

  return harness_fail;
}
