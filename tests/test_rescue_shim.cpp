// test_rescue_shim.cpp — the rescue instrument's controls, at shim level.
//
// The shim is pure numerics: chain replay, double-double truth, a
// double-precision log-reference, and the shipped accumulator. None of that
// needs LLVM, so it is tested here as a ctest on every CI leg rather than only
// under WSL. The instrumentation pass that feeds it is gated separately.
//
// THREE CONTROLS, and the third is not folded into the others.
//
//   positive   mixture near -800: range failure, rescued
//   negative   well-conditioned dot product: no failure, NOT rescued
//   pre-exp    the recorded log-magnitude of mixture's term IS -800,
//              not -inf
//
// The third exists because a probe that reconstructs log(0.0) returns -inf,
// which would still look like a rescue in the positive control while being the
// exact defect the symbolic replay exists to avoid. It has to be asserted
// against the value.
//
// A probe that cannot produce "not rescued" cannot tell a safe site from a
// broken instrument, which is the same defect as a gate that cannot fail.

#include <cmath>
#include <cstdio>
#include <limits>

extern "C" {
void lr_site(int id, const char *loc, const char *chain, int nleaves,
             int accum_bits, double t_site);
void lr_leaf(int id, int slot, double v);
void lr_term(int id, double linear_term);
void lr_exec(int id, double linear_result);
double lr_last_term_log(int id);
long lr_rescue_count(int id);
long lr_exec_count(int id);
long lr_trunc_collapse(int id);
void lr_reset_all(void);
}

namespace {

int failures = 0;

void check(bool ok, const char *label) {
  std::printf("%s,%s\n", ok ? "PASS" : "FAIL", label);
  if (!ok) failures++;
}

void check_near(double got, double want, double tol, const char *label) {
  const bool ok = std::fabs(got - want) <= tol;
  std::printf("%s,%s (got %.17g want %.17g)\n", ok ? "PASS" : "FAIL", label,
              got, want);
  if (!ok) failures++;
}

// s += w[i] * exp(logp[i]) — coverage.c's `mixture`, the marquee shape.
// Postfix: leaf0 = w, leaf1 = logp.
const char *kMixtureChain = "L0 L1 EXP MUL";

// s += x[i] * y[i] — an ordinary dot product.
const char *kDotChain = "L0 L1 MUL";

} // namespace

int main() {
  std::printf("test_rescue_shim — R1 controls\n\n");

  // -------------------------------------------------------------------------
  // POSITIVE CONTROL. 100 terms at logp = -800, w = 1. Every exp(logp)
  // underflows to 0.0 in double, so the program's sum is exactly 0.0 while the
  // true sum is 100 * e^-800, finite and perfectly representable in log form.
  // -------------------------------------------------------------------------
  {
    lr_reset_all();
    lr_site(1, "control/positive.c,1,mixture", kMixtureChain, 2, 64, 1e-10);
    double linear = 0.0;
    for (int i = 0; i < 100; ++i) {
      const double w = 1.0, logp = -800.0;
      lr_leaf(1, 0, w);
      lr_leaf(1, 1, logp);
      const double term = w * std::exp(logp); // 0.0, and that is the point
      linear += term;
      lr_term(1, term);
    }
    lr_exec(1, linear);

    check(linear == 0.0, "positive_control_linear_underflows_to_zero");
    check(lr_exec_count(1) == 1, "positive_control_one_execution");
    check(lr_rescue_count(1) == 1, "positive_control_is_rescued");
  }

  // -------------------------------------------------------------------------
  // PRE-EXP SYMBOLIC CAPTURE. The dedicated one. The recorded log-magnitude
  // must be the pre-exp argument, -800, and NOT log(0.0) = -inf.
  // -------------------------------------------------------------------------
  {
    lr_reset_all();
    lr_site(2, "control/preexp.c,1,mixture", kMixtureChain, 2, 64, 1e-10);
    lr_leaf(2, 0, 1.0);
    lr_leaf(2, 1, -800.0);
    lr_term(2, 1.0 * std::exp(-800.0));
    const double lg = lr_last_term_log(2);

    // Both, not just isinf. Mutation-tested: forcing EXP to reconstruct from
    // the materialized value yields NaN rather than -inf, and an isinf-only
    // assertion passes straight through it. The value check below is what
    // actually caught the defect.
    check(!std::isinf(lg) && !std::isnan(lg),
          "preexp_not_reconstructed_from_collapsed_value");
    check_near(lg, -800.0, 1e-9, "preexp_log_magnitude_is_the_argument");

    // With a weight, the magnitude is log(w) + logp: still symbolic.
    lr_leaf(2, 0, 2.0);
    lr_leaf(2, 1, -800.0);
    lr_term(2, 2.0 * std::exp(-800.0));
    check_near(lr_last_term_log(2), -800.0 + std::log(2.0), 1e-9,
               "preexp_weight_folds_into_the_log_magnitude");
  }

  // -------------------------------------------------------------------------
  // NEGATIVE CONTROL. A well-conditioned dot product in ordinary range. The
  // linear path is fine, so nothing is rescued. A probe that cannot produce
  // this cannot distinguish a safe site from a broken instrument.
  // -------------------------------------------------------------------------
  {
    lr_reset_all();
    lr_site(3, "control/negative.c,1,dot", kDotChain, 2, 64, 1e-10);
    double linear = 0.0;
    for (int i = 0; i < 100; ++i) {
      const double x = 1.0 + 0.01 * i, y = 2.0 - 0.005 * i;
      lr_leaf(3, 0, x);
      lr_leaf(3, 1, y);
      const double term = x * y;
      linear += term;
      lr_term(3, term);
    }
    lr_exec(3, linear);

    check(std::isfinite(linear) && linear > 0.0, "negative_control_linear_is_fine");
    check(lr_exec_count(3) == 1, "negative_control_one_execution");
    check(lr_rescue_count(3) == 0, "negative_control_is_NOT_rescued");
  }

  // -------------------------------------------------------------------------
  // fptrunc is not transparent. A healthy double can be 0.0f after narrowing,
  // and the replay must apply that rounding rather than claiming a log term
  // where the accumulator received zero.
  //
  // THE BOUNDARY IS -103.28, NOT -100. float(exp(-100)) is 3.78e-44, a
  // representable float SUBNORMAL, not zero; measured, not assumed. Collapse
  // begins below log(1.4e-45) = -103.2799, so exp(-110) is the case that
  // actually narrows to zero. A test written at -100 would have passed only if
  // the shim were wrong about the subnormal range.
  // -------------------------------------------------------------------------
  {
    lr_reset_all();
    lr_site(4, "control/trunc.c,1,f32", "L0 EXP TRUNCF", 1, 32, 1e-10);
    lr_leaf(4, 0, -110.0);
    lr_term(4, static_cast<double>(static_cast<float>(std::exp(-110.0))));
    check(lr_trunc_collapse(4) == 1, "fptrunc_collapse_is_counted");
    check(std::isinf(lr_last_term_log(4)) && lr_last_term_log(4) < 0,
          "fptrunc_collapsed_term_is_zero_not_its_log_magnitude");

    // Just inside the subnormal range: narrowed, but not collapsed.
    lr_leaf(4, 0, -100.0);
    lr_term(4, static_cast<double>(static_cast<float>(std::exp(-100.0))));
    check(lr_trunc_collapse(4) == 1, "fptrunc_subnormal_is_not_a_collapse");

    // In range, the narrowing is a small perturbation and no collapse.
    lr_leaf(4, 0, -5.0);
    lr_term(4, static_cast<double>(static_cast<float>(std::exp(-5.0))));
    check(lr_trunc_collapse(4) == 1, "fptrunc_in_range_does_not_collapse");
    check_near(lr_last_term_log(4), -5.0, 1e-6,
               "fptrunc_in_range_keeps_the_magnitude");
  }

  // -------------------------------------------------------------------------
  // pow sign is resolved from the observed base and an integrality test on the
  // observed exponent, never from the parity of a float.
  // -------------------------------------------------------------------------
  {
    lr_reset_all();
    lr_site(5, "control/pow.c,1,p", "L0 L1 POW", 2, 64, 1e-10);

    lr_leaf(5, 0, 2.0); lr_leaf(5, 1, 10.0);
    lr_term(5, std::pow(2.0, 10.0));
    check_near(lr_last_term_log(5), std::log(1024.0), 1e-9,
               "pow_positive_base");

    // Negative base, non-integral exponent: the real result is NaN. The shim
    // records that rather than inventing a sign.
    lr_leaf(5, 0, -2.0); lr_leaf(5, 1, 0.5);
    lr_term(5, std::pow(-2.0, 0.5));
    check(std::isnan(lr_last_term_log(5)),
          "pow_negative_base_nonintegral_exponent_is_NaN_not_signed");

    // Negative base, integral exponent: sign is determined.
    lr_leaf(5, 0, -2.0); lr_leaf(5, 1, 3.0);
    lr_term(5, std::pow(-2.0, 3.0));
    check_near(lr_last_term_log(5), std::log(8.0), 1e-9,
               "pow_negative_base_integral_exponent");

    // A genuinely zero base is a real zero input, not a collapse.
    lr_leaf(5, 0, 0.0); lr_leaf(5, 1, 2.0);
    lr_term(5, 0.0);
    check(std::isinf(lr_last_term_log(5)) && lr_last_term_log(5) < 0,
          "pow_zero_base_is_a_real_zero");
  }

  // -------------------------------------------------------------------------
  // Signed cancellation stays in double-double. A + B with A ~ -B is where
  // log-domain arithmetic loses the quantity being established as truth.
  // -------------------------------------------------------------------------
  {
    lr_reset_all();
    lr_site(6, "control/cancel.c,1,c", "L0 L1 ADD", 2, 64, 1e-10);
    const double a = 1.0, b = -1.0 + 1e-15;
    lr_leaf(6, 0, a); lr_leaf(6, 1, b);
    lr_term(6, a + b);
    check_near(lr_last_term_log(6), std::log(std::fabs(a + b)), 1e-6,
               "cancellation_preserved_in_double_double");
  }

  std::printf("\n");
  if (failures == 0) std::printf("test_rescue_shim: PASS\n");
  else std::printf("test_rescue_shim: FAIL (%d)\n", failures);
  return failures > 0 ? 1 : 0;
}
