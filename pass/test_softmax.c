/* test_softmax.c — end-to-end test for pass/LogRewritePass.cpp ("log-rewrite").
 *
 * One source file, compiled three times (driven by run_pass_test.sh):
 *   1. -DKERNEL -Dsoftmax_denom=softmax_denom_orig   -> baseline kernel object
 *   2. -DKERNEL -Dsoftmax_denom=softmax_denom_rw     -> identical IR, then run
 *      through opt-21 -passes='log-rewrite<force>' before codegen
 *   3. (no -DKERNEL)                                 -> this main() harness
 * The -D renaming is how two copies of one function coexist in one binary.
 * (Chosen over objcopy --redefine-sym: same effect, no binary surgery, and
 * the preprocessor rename is visible right here in the build commands.)
 *
 * The rewritten kernel stores m + log(s) into the external global
 * __logrange_logsum (the pass's prototype export hook). This harness defines
 * that global and reads it after each call to the rewritten kernel — it is
 * the only place the rescue-case answer is representable, because the
 * linear replacement value exp(m + log(s)) re-underflows exactly when the
 * rescue matters (see PROTOTYPE.md).
 */

#ifdef KERNEL

#include <math.h>

/* The marquee shape from the matcher study: the softmax denominator.
 * softmax_denom is renamed per compilation via -D (see header comment). */
double softmax_denom(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  return s;
}

/* Negative controls — in-scope-looking loops the pass must DECLINE
 * (run_pass_test.sh asserts exactly one REWRITE line for this module, and
 * the harness checks orig/rw results are bit-identical). Renamed per
 * compilation via -D exactly like softmax_denom. */

/* Plain sum: term is a load, not an exp call. */
double plain_sum(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += x[i];
  return s;
}

/* Dot product: fmuladd/fmul update, not fadd(phi, exp(t)). */
double dot_sum(const double *x, const double *y, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += x[i] * y[i];
  return s;
}

/* exp of a loop-invariant argument: hoistable constant sum, not the
 * reduction this project targets. */
double invariant_exp_sum(double c, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(c);
  return s;
}

#else /* ---------------------------- harness ---------------------------- */

#include <math.h>
#include <stdio.h>

extern double softmax_denom_orig(const double *x, int n);
extern double softmax_denom_rw(const double *x, int n);
extern double plain_sum_orig(const double *x, int n);
extern double plain_sum_rw(const double *x, int n);
extern double dot_sum_orig(const double *x, const double *y, int n);
extern double dot_sum_rw(const double *x, const double *y, int n);
extern double invariant_exp_sum_orig(double c, int n);
extern double invariant_exp_sum_rw(double c, int n);

/* Written by the rewritten kernel on every call (pass export hook). */
double __logrange_logsum = 0.0;

/* Deterministic N(0,1)-ish values: SplitMix-style LCG + Box-Muller. */
static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;
static double urand(void) {
  rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
  return (double)(rng_state >> 11) * (1.0 / 9007199254740992.0);
}
static double nrand(void) {
  double u1 = urand(), u2 = urand();
  if (u1 < 1e-300) u1 = 1e-300;
  return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* Max-shift logsumexp reference, computed independently in the harness:
 * log(sum exp(x_i)) = M + log(sum exp(x_i - M)),  M = max_i x_i. */
static double ref_logsumexp(const double *x, int n) {
  double m = -INFINITY;
  for (int i = 0; i < n; ++i)
    if (x[i] > m) m = x[i];
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i] - m);
  return m + log(s);
}

static int fails = 0;
static void check(const char *name, int ok) {
  printf("%s,%s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++fails;
}

#define N 1000

int main(void) {
  static double x[N];
  int i;

  /* (a) Benign inputs: values ~ N(0,1). Original and rewritten must agree
   *     on the linear result to 1e-12 relative. */
  for (i = 0; i < N; ++i) x[i] = nrand();
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double rel = fabs(sr - so) / fabs(so);
    double lref = ref_logsumexp(x, N);
    printf("INFO,benign,orig=%.17g,rw=%.17g,rel=%.3g,logsum=%.17g,logref=%.17g\n",
           so, sr, rel, lg, lref);
    check("benign_linear_agree_1e-12", rel < 1e-12);
    check("benign_exported_logsum", fabs(lg - lref) <= 1e-12 * fabs(lref));
  }

  /* (b) Rescue case: values around -800. Every exp(x_i) underflows to 0.0,
   *     so the original sum is exactly 0.0 — the silent failure this
   *     project exists to repair. The rewritten kernel's exported log-form
   *     must match the harness's independent max-shift reference. */
  for (i = 0; i < N; ++i) x[i] = -800.0 + nrand();
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double lref = ref_logsumexp(x, N);
    printf("INFO,rescue,orig=%.17g,rw_linear=%.17g,logsum=%.17g,logref=%.17g\n",
           so, sr, lg, lref);
    check("rescue_orig_underflows_to_zero", so == 0.0);
    check("rescue_exported_logsum_finite", isfinite(lg));
    check("rescue_exported_logsum_correct",
          fabs(lg - lref) <= 1e-12 * fabs(lref));
    /* Honest note, not a check: the linear replacement value cannot carry
     * the rescue — exp(-793) still underflows. The win lives in the log
     * form; propagating it downstream is future work (PROTOTYPE.md). */
    printf("INFO,rescue,rw_linear_underflows_too=%d (expected 1)\n",
           sr == 0.0);
  }

  /* (c) NaN propagation: one NaN input must poison both versions. */
  for (i = 0; i < N; ++i) x[i] = nrand();
  x[123] = NAN;
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    printf("INFO,nan,orig=%g,rw=%g,logsum=%g\n", so, sr, lg);
    check("nan_orig_propagates", isnan(so));
    check("nan_rw_propagates", isnan(sr));
    check("nan_exported_logsum_propagates", isnan(lg));
  }

  /* (d) Negative controls: loops the pass must have declined. Both copies
   *     come from identical IR, so results must be bit-identical. */
  {
    static double y[N];
    for (i = 0; i < N; ++i) { x[i] = nrand(); y[i] = nrand(); }
    check("negctl_plain_sum_untouched",
          plain_sum_orig(x, N) == plain_sum_rw(x, N));
    check("negctl_dot_sum_untouched",
          dot_sum_orig(x, y, N) == dot_sum_rw(x, y, N));
    check("negctl_invariant_exp_untouched",
          invariant_exp_sum_orig(0.5, N) == invariant_exp_sum_rw(0.5, N));
  }

  printf(fails ? "OVERALL,FAIL\n" : "OVERALL,PASS\n");
  return fails ? 1 : 0;
}

#endif /* KERNEL */
