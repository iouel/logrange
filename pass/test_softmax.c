/* test_softmax.c — end-to-end test for pass/LogRewritePass.cpp ("log-rewrite").
 *
 * One source file, compiled three times (driven by run_pass_test.sh):
 *   1. -DKERNEL with per-function renames             -> baseline kernel object
 *   2. -DKERNEL with the same renames to *_rw        -> identical IR, then run
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

/* The HIGH-risk shape from the matcher study: the softmax denominator.
 * softmax_denom is renamed per compilation via -D (see header comment). */
double softmax_denom(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  return s;
}

/* Full softmax: denominator loop + normalize divide in one function.
 * This is the stretch-goal milestone shape (ELIGIBILITY.md section 6). */
void softmax_full(const double *x, double *out, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  for (int i = 0; i < n; ++i)
    out[i] = exp(x[i]) / s;
}

/* Near-miss consumer: uses the sum in an fadd, not an fdiv. */
void softmax_add(const double *x, double *out, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  for (int i = 0; i < n; ++i)
    out[i] = exp(x[i]) + s;
}

/* Near-miss consumer: divide present, divisor is the sum, but the
 * numerator is a plain load (not an llvm.exp call). Must be declined with
 * numerator-not-exp by propagate=div. */
void softmax_plain_div(const double *x, double *out, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  for (int i = 0; i < n; ++i)
    out[i] = x[i] / s;
}

/* Near-miss consumer: divide present, but the rewritten sum is not the
 * divisor. */
void softmax_sum_div(const double *x, double *out, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  for (int i = 0; i < n; ++i)
    out[i] = s / exp(x[i]);
}

/* Negative controls — in-scope-looking loops the pass must DECLINE
 * (run_pass_test.sh asserts only the softmax-denominator family rewrites, and
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
extern void softmax_full_orig(const double *x, double *out, int n);
extern void softmax_full_rw(const double *x, double *out, int n);
extern void softmax_full_prop(const double *x, double *out, int n);
extern void softmax_add_orig(const double *x, double *out, int n);
extern void softmax_add_rw(const double *x, double *out, int n);
extern void softmax_sum_div_orig(const double *x, double *out, int n);
extern void softmax_sum_div_rw(const double *x, double *out, int n);
extern void softmax_plain_div_orig(const double *x, double *out, int n);
extern void softmax_plain_div_rw(const double *x, double *out, int n);
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
 * log(sum exp(x_i)) = M + log(sum exp(x_i - M)),  M = max_i x_i.
 *
 * The special cases are handled BEFORE the shift, because the shift itself
 * is what breaks on infinities: x_i - M is inf - inf = NaN whenever M is
 * infinite and x_i == M. The earlier version of this function did the shift
 * unconditionally and therefore returned NaN for all-(-inf) input and for
 * any input containing +inf — i.e. it was not an oracle at all in exactly
 * the cases the pass's infinity guard exists to get right, which is why the
 * infinity tests below assert against constants as well.
 *
 * Order of the special cases matters and follows the linear loop being
 * modelled, `s += exp(x_i)`:
 *   NaN anywhere    -> some term is exp(NaN) = NaN, and NaN + anything is
 *                      NaN, so the sum is NaN and log(NaN) is NaN. Checked
 *                      first: NaN beats +inf here, because the linear loop
 *                      adds a NaN term whatever else it has added.
 *   +inf present    -> that term is exp(+inf) = +inf; every other term is
 *                      non-negative and non-NaN, so the sum is +inf.
 *   all -inf        -> every term is exp(-inf) = 0, the sum is exactly 0.0,
 *                      and log(0) = -inf. Also the n == 0 (zero-trip) case:
 *                      the empty sum is 0.0 and its log is -inf, which is
 *                      what the loop below returns for m = -inf, s = 0.
 * Remaining -inf entries are ordinary zero terms: M is then finite, so
 * x_i - M = -inf is a well-defined exponent and exp gives exactly 0. */
static double ref_logsumexp(const double *x, int n) {
  int i;
  for (i = 0; i < n; ++i)
    if (isnan(x[i])) return NAN;
  for (i = 0; i < n; ++i)
    if (x[i] == INFINITY) return INFINITY;

  double m = -INFINITY;
  for (i = 0; i < n; ++i)
    if (x[i] > m) m = x[i];
  if (m == -INFINITY) return -INFINITY; /* all terms -inf, or n == 0 */

  double s = 0.0;   /* m is finite here, so every x_i - m is well defined */
  for (i = 0; i < n; ++i)
    s += exp(x[i] - m);
  return m + log(s);
}

static int fails = 0;
static void check(const char *name, int ok) {
  printf("%s,%s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++fails;
}

static double max_rel_diff(const double *a, const double *b, int n) {
  double worst = 0.0;
  int i;
  for (i = 0; i < n; ++i) {
    double diff = fabs(a[i] - b[i]);
    double scale = fmax(fabs(a[i]), fabs(b[i]));
    double rel = scale == 0.0 ? diff : diff / scale;
    if (rel > worst) worst = rel;
  }
  return worst;
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
    double lref = ref_logsumexp(x, N);
    printf("INFO,nan,orig=%g,rw=%g,logsum=%g,logref=%g\n", so, sr, lg, lref);
    check("nan_orig_propagates", isnan(so));
    check("nan_rw_propagates", isnan(sr));
    check("nan_exported_logsum_propagates", isnan(lg));
    check("nan_matches_reference", isnan(lref) && isnan(lg));
  }

  /* (e) -inf inputs. A -inf log-magnitude encodes a zero term: exp(-inf)=0.
   *     Ordinary input, not pathological. The streaming update must not
   *     manufacture a NaN out of -inf - -inf. Leading -inf (x[0]) is the
   *     hard case: the running max is still -inf, so newm == m == t. */
  for (i = 0; i < N; ++i) x[i] = nrand();
  x[0] = -INFINITY;
  x[1] = -INFINITY;
  x[500] = -INFINITY;
  x[N - 1] = -INFINITY;
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double rel = fabs(sr - so) / fabs(so);
    double lref = ref_logsumexp(x, N);
    printf("INFO,neginf,orig=%.17g,rw=%.17g,rel=%.3g,logsum=%.17g,logref=%.17g\n",
           so, sr, rel, lg, lref);
    check("neginf_rw_finite", isfinite(sr));
    check("neginf_linear_agree_1e-12", rel < 1e-12);
    check("neginf_exported_logsum", fabs(lg - lref) <= 1e-12 * fabs(lref));
  }

  /* (f) All terms -inf: the sum is exactly 0.0 and its log is -inf. */
  for (i = 0; i < N; ++i) x[i] = -INFINITY;
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double lref = ref_logsumexp(x, N);
    printf("INFO,allneginf,orig=%.17g,rw=%.17g,logsum=%.17g,logref=%.17g\n",
           so, sr, lg, lref);
    check("allneginf_orig_zero", so == 0.0);
    check("allneginf_rw_zero", sr == 0.0);
    check("allneginf_exported_logsum_neginf", lg == -INFINITY);
    check("allneginf_matches_reference", lref == -INFINITY && lg == lref);
  }

  /* (g) NaN in the first position: the -inf guard must not swallow it.
   *     (Case (c) only covers a NaN arriving after the max is finite.) */
  for (i = 0; i < N; ++i) x[i] = nrand();
  x[0] = NAN;
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double lref = ref_logsumexp(x, N);
    printf("INFO,nan_first,orig=%g,rw=%g,logsum=%g,logref=%g\n",
           so, sr, lg, lref);
    check("nan_first_orig_propagates", isnan(so));
    check("nan_first_rw_propagates", isnan(sr));
    check("nan_first_exported_logsum_propagates", isnan(lg));
    check("nan_first_matches_reference", isnan(lref) && isnan(lg));
  }

  /* (h) +inf input: the other inf - inf face of the same guard (t = +inf
   *     makes newm = +inf, so t - newm would be NaN). Linear gives +inf. */
  for (i = 0; i < N; ++i) x[i] = nrand();
  x[7] = INFINITY;
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double lref = ref_logsumexp(x, N);
    printf("INFO,posinf,orig=%g,rw=%g,logsum=%g,logref=%g\n", so, sr, lg, lref);
    check("posinf_orig_is_inf", so == INFINITY);
    check("posinf_rw_is_inf", sr == INFINITY);
    check("posinf_exported_logsum_is_inf", lg == INFINITY);
    check("posinf_matches_reference", lref == INFINITY && lg == lref);
  }

  /* (i) NaN mixed WITH infinities: NaN must win, in both the kernel and the
   *     reference. This is the case that pins the reference's ordering —
   *     a +inf-first reference would report +inf and disagree with the
   *     linear loop, which adds a NaN term and stays NaN. */
  for (i = 0; i < N; ++i) x[i] = nrand();
  x[3] = -INFINITY;
  x[7] = INFINITY;
  x[400] = NAN;
  {
    double so = softmax_denom_orig(x, N);
    double sr = softmax_denom_rw(x, N);
    double lg = __logrange_logsum;
    double lref = ref_logsumexp(x, N);
    printf("INFO,nan_with_infs,orig=%g,rw=%g,logsum=%g,logref=%g\n",
           so, sr, lg, lref);
    check("nan_with_infs_orig_propagates", isnan(so));
    check("nan_with_infs_rw_propagates", isnan(sr));
    check("nan_with_infs_matches_reference", isnan(lref) && isnan(lg));
  }

  /* (j) Zero trip count. n = 0: the empty sum is exactly 0.0. The rewritten
   *     loop is bypassed entirely by its guard, so the rewritten exit block
   *     never executes and the export hook is NOT written — assert that
   *     rather than pretend otherwise. The reference's empty sum is
   *     log(0) = -inf, consistent with the streaming state m=-inf, s=0. */
  {
    double sentinel = 12345.0;
    __logrange_logsum = sentinel;
    double so = softmax_denom_orig(x, 0);
    double sr = softmax_denom_rw(x, 0);
    double lg = __logrange_logsum;
    double lref = ref_logsumexp(x, 0);
    printf("INFO,zerotrip,orig=%.17g,rw=%.17g,logsum=%.17g,logref=%.17g\n",
           so, sr, lg, lref);
    check("zerotrip_orig_zero", so == 0.0);
    check("zerotrip_rw_zero", sr == 0.0);
    check("zerotrip_bitwise_identical", so == sr && signbit(so) == signbit(sr));
    check("zerotrip_export_not_written", lg == sentinel);
    check("zerotrip_reference_neginf", lref == -INFINITY);
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

  /* (k) Consumer-shape kernels. The spike only logs their consumer IR shape;
   *     it does not rewrite the consumer, so orig/rw outputs must still
   *     agree closely. */
  {
    static double out_orig[N], out_rw[N];
    double rel;
    for (i = 0; i < N; ++i) x[i] = nrand();

    softmax_full_orig(x, out_orig, N);
    softmax_full_rw(x, out_rw, N);
    rel = max_rel_diff(out_orig, out_rw, N);
    printf("INFO,softmax_full,max_rel=%.3g\n", rel);
    check("softmax_full_rw_agree_1e-12", rel < 1e-12);

    softmax_add_orig(x, out_orig, N);
    softmax_add_rw(x, out_rw, N);
    rel = max_rel_diff(out_orig, out_rw, N);
    printf("INFO,softmax_add,max_rel=%.3g\n", rel);
    check("softmax_add_rw_agree_1e-12", rel < 1e-12);

    softmax_sum_div_orig(x, out_orig, N);
    softmax_sum_div_rw(x, out_rw, N);
    rel = max_rel_diff(out_orig, out_rw, N);
    printf("INFO,softmax_sum_div,max_rel=%.3g\n", rel);
    check("softmax_sum_div_rw_agree_1e-12", rel < 1e-12);

    softmax_plain_div_orig(x, out_orig, N);
    softmax_plain_div_rw(x, out_rw, N);
    rel = max_rel_diff(out_orig, out_rw, N);
    printf("INFO,softmax_plain_div,max_rel=%.3g\n", rel);
    check("softmax_plain_div_rw_agree_1e-12", rel < 1e-12);
  }

  /* (m) Rescue case for propagate=div: softmax_full_prop has its normalize
   *     divide rewritten to exp(x[i] - L) directly from the exp argument,
   *     not exp(log(exp(x[i])) - L) -- no downstream InstCombine fold is
   *     required for correctness.  On rescue-regime inputs the linear form
   *     yields 0.0/0.0 = NaN; the propagated form is finite and correct. */
  {
    static double out_prop[N];
    int all_finite;
    double prop_sum;
    double lref;
    for (i = 0; i < N; ++i) x[i] = -800.0 + nrand();
    softmax_full_prop(x, out_prop, N);
    lref = ref_logsumexp(x, N);
    all_finite = 1;
    for (i = 0; i < N; ++i)
      if (!isfinite(out_prop[i])) { all_finite = 0; break; }
    prop_sum = 0.0;
    for (i = 0; i < N; ++i) prop_sum += out_prop[i];
    printf("INFO,prop_rescue,logref=%.17g,sum=%.17g\n", lref, prop_sum);
    check("prop_rescue_all_finite", all_finite);
    check("prop_rescue_sums_to_one", fabs(prop_sum - 1.0) < 1e-10);
  }

  /* (n) Special-value coverage for the propagated exp(t) form.
   *     Each sub-case compares softmax_full_prop against the linear original
   *     (softmax_full_orig), or asserts the NaN/zero behaviour directly.
   *     The full-softmax input is N=2 for clarity; the denominator loop is
   *     still eligible and is rewritten. */
  {
    static double sv[2], out_sv_prop[2], out_sv_orig[2];

    /* n1: t = NaN in one slot. Both forms must yield NaN for that slot. */
    sv[0] = NAN; sv[1] = 1.0;
    softmax_full_orig(sv, out_sv_orig, 2);
    softmax_full_prop(sv, out_sv_prop, 2);
    printf("INFO,prop_sv_nan,orig=[%g,%g],prop=[%g,%g]\n",
           out_sv_orig[0], out_sv_orig[1], out_sv_prop[0], out_sv_prop[1]);
    check("prop_sv_nan_slot0_orig_nan", isnan(out_sv_orig[0]));
    check("prop_sv_nan_slot0_prop_nan", isnan(out_sv_prop[0]));

    /* n2: t = +inf in one slot. Linear gives +inf/+inf = NaN; propagated
     *     gives exp(+inf - +inf) = exp(NaN) = NaN. Both are NaN. */
    sv[0] = INFINITY; sv[1] = 1.0;
    softmax_full_orig(sv, out_sv_orig, 2);
    softmax_full_prop(sv, out_sv_prop, 2);
    printf("INFO,prop_sv_posinf,orig=[%g,%g],prop=[%g,%g]\n",
           out_sv_orig[0], out_sv_orig[1], out_sv_prop[0], out_sv_prop[1]);
    check("prop_sv_posinf_slot0_orig_nan", isnan(out_sv_orig[0]));
    check("prop_sv_posinf_slot0_prop_nan", isnan(out_sv_prop[0]));

    /* n3: t = -inf in one slot (zero numerator). Linear gives 0/s = 0;
     *     propagated gives exp(-inf - L) = exp(-inf) = +0.0. */
    sv[0] = -INFINITY; sv[1] = 1.0;
    softmax_full_orig(sv, out_sv_orig, 2);
    softmax_full_prop(sv, out_sv_prop, 2);
    printf("INFO,prop_sv_neginf,orig=[%g,%g],prop=[%g,%g]\n",
           out_sv_orig[0], out_sv_orig[1], out_sv_prop[0], out_sv_prop[1]);
    check("prop_sv_neginf_slot0_orig_zero", out_sv_orig[0] == 0.0);
    check("prop_sv_neginf_slot0_prop_zero", out_sv_prop[0] == 0.0);

    /* n4: all terms -inf (zero-over-zero). The denominator is exactly 0.0,
     *     the log form is -inf.  Linear gives 0.0/0.0 = NaN; propagated
     *     gives exp(-inf - (-inf)) = exp(NaN) = NaN.  Both are NaN. */
    sv[0] = -INFINITY; sv[1] = -INFINITY;
    softmax_full_orig(sv, out_sv_orig, 2);
    softmax_full_prop(sv, out_sv_prop, 2);
    printf("INFO,prop_sv_allneginf,orig=[%g,%g],prop=[%g,%g]\n",
           out_sv_orig[0], out_sv_orig[1], out_sv_prop[0], out_sv_prop[1]);
    check("prop_sv_allneginf_orig_nan", isnan(out_sv_orig[0]));
    check("prop_sv_allneginf_prop_nan", isnan(out_sv_prop[0]));
  }

  printf(fails ? "OVERALL,FAIL\n" : "OVERALL,PASS\n");
  return fails ? 1 : 0;
}

#endif /* KERNEL */
