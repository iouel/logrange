/* test_softmax.c — end-to-end test for pass/LogRewritePass.cpp ("log-rewrite").
 *
 * One source file, compiled three times (driven by run_pass_test.sh):
 *   1. -DKERNEL with per-function renames             -> baseline kernel object
 *   2. -DKERNEL with the same renames to *_rw        -> identical IR, then run
 *      through opt-21 -passes='loop-simplify,lcssa,log-rewrite<force>' before codegen
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

/* Mixture likelihood: the weighted spine. At the harness's flags clang emits
 * llvm.fmuladd(w, exp(t), phi); at -ffp-contract=off and =fast it emits
 * fmul + fadd. Both forms must be MATCHED and then DECLINED: folding w into
 * the term makes the state accumulate sum(w_i * exp(t_i - m)), which reaches
 * sum|w_i| and has no ceiling, where the unweighted state is at most n.
 * Measured on the emitted state machine at n=2: w=(1e308,1e308),
 * t=(-700,-700) drives the state to inf while the linear loop gives 19719.4.
 * Corpus evidence for the spine's rarity is thin and was published as
 * stronger than it is: 0 w*exp(t) multiplies among the 5 exp-carrying
 * reductions the matcher accepted, not among 2859 loops. Derivation:
 * matcher/run_study.sh figures. */
double weighted_sum(const double *w, const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += w[i] * exp(x[i]);
  return s;
}

/* Constant weight: provably positive, provably bounded, and REWRITTEN as of
 * 2026-08-17 by folding log(w) into the exponent (WeightPlan). Until then it
 * was declined, because nothing downstream read the weight and one that
 * slipped through was silently DROPPED — wrong by a factor of w at every
 * magnitude, not merely unbounded at extreme ones.
 *
 * This kernel exists because its absence made the suite green while the pass
 * miscompiled: relaxing the clause to `Weight && !isa<ConstantFP>(Weight)`
 * left the REWRITE count at 4, both expected declines in place, and every
 * assertion passing. Nothing in the corpus had a constant weight.
 *
 * Its assertion is therefore on the VALUE, not on the rewrite's presence, and
 * 0.5 is chosen so a dropped weight doubles the result — a failure no
 * tolerance can absorb. */
double const_weight_sum(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += 0.5 * exp(x[i]);
  return s;
}

/* A second constant, far from 1 and not a power of two, so a fold that
 * hardcodes or mis-signs log(w) cannot pass by coincidence. log(1e6) is
 * 13.8155…, well away from log(0.5) = -0.6931…. */
double const_weight_big(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += 1e6 * exp(x[i]);
  return s;
}

/* Negative constant weight: still DECLINED, and not as a harder version of
 * the bounded case. exp cannot represent a negative term, so this needs the
 * signed pos/neg representation rp_accum uses, where cancellation is part of
 * the representation rather than of the sum. Declined by its own name. */
double neg_weight_sum(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += -2.0 * exp(x[i]);
  return s;
}

/* f32 source widened into a double accumulator: clang emits llvm.exp.f32 and
 * an fpext, and the pass rewrites it through the fpext path in classifyTerm.
 * This is a REWRITING path, and it had no test of any kind until 2026-08-17.
 *
 * Its orig/rw agreement is float-level, not double-level, and that is not a
 * defect in the rewrite: the original computes (double)expf(x_i), a
 * float-precision exponential, while the rewritten state computes exp() in
 * double on the exactly-widened argument. The rewrite is the more accurate of
 * the two. The error contract in ELIGIBILITY.md section 5 bounds the
 * rewritten code against the exact sum, which is the assertion below. */
double expf_widened(const float *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += expf(x[i]);
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
extern double plain_sum_orig(const double *x, int n);
extern double plain_sum_rw(const double *x, int n);
extern double dot_sum_orig(const double *x, const double *y, int n);
extern double dot_sum_rw(const double *x, const double *y, int n);
extern double weighted_sum_orig(const double *w, const double *x, int n);
extern double weighted_sum_rw(const double *w, const double *x, int n);
extern double const_weight_sum_orig(const double *x, int n);
extern double const_weight_sum_rw(const double *x, int n);
extern double const_weight_big_orig(const double *x, int n);
extern double const_weight_big_rw(const double *x, int n);
extern double neg_weight_sum_orig(const double *x, int n);
extern double neg_weight_sum_rw(const double *x, int n);
extern double expf_widened_orig(const float *x, int n);
extern double expf_widened_rw(const float *x, int n);
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
    /* cover_<fn>_* is a contract, not a style: run_pass_test.sh derives the
     * set of rewritten functions from the pass's own output and requires a
     * passing cover_<fn>_ check for each one. A rewrite with no semantic
     * assertion behind it fails the gate. */
    check("cover_softmax_denom_rw_benign_1e-12", rel < 1e-12);
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
    /* Matched and declined, unlike plain_sum and invariant_exp, which are not
     * matched at all. A decline must leave the loop bit-identical too. */
    check("negctl_weighted_sum_untouched",
          weighted_sum_orig(y, x, N) == weighted_sum_rw(y, x, N));
    check("negctl_neg_weight_untouched",
          neg_weight_sum_orig(x, N) == neg_weight_sum_rw(x, N));
    check("negctl_invariant_exp_untouched",
          invariant_exp_sum_orig(0.5, N) == invariant_exp_sum_rw(0.5, N));
  }

  /* (d2) Bounded constant weights, which ARE rewritten. The assertion is on
   *      the value, never on the presence of a REWRITE record: the failure
   *      this guards against is a weight that is analysed and then not
   *      consumed, which leaves the rewrite in place and the answer wrong by
   *      exactly w. Both constants are chosen so that failure cannot hide —
   *      dropping 0.5 doubles the result, dropping 1e6 divides it by a
   *      million. Neither is within any tolerance of correct. */
  {
    double lo, hi, ro, rr;
    for (i = 0; i < N; ++i) x[i] = nrand();
    lo = const_weight_sum_orig(x, N);
    hi = const_weight_sum_rw(x, N);
    printf("INFO,const_weight,orig=%.17g,rw=%.17g,rel=%.3g\n", lo, hi,
           fabs(hi - lo) / fabs(lo));
    check("cover_const_weight_sum_rw_agrees_1e-12",
          fabs(hi - lo) <= 1e-12 * fabs(lo));
    /* The specific miscompile this file was written for: dropping 0.5
     * doubles the answer, which no tolerance absorbs. */
    check("const_weight_not_dropped", fabs(hi - 2.0 * lo) > 0.25 * fabs(lo));

    ro = const_weight_big_orig(x, N);
    rr = const_weight_big_rw(x, N);
    printf("INFO,const_weight_big,orig=%.17g,rw=%.17g,rel=%.3g\n", ro, rr,
           fabs(rr - ro) / fabs(ro));
    check("cover_const_weight_big_rw_agrees_1e-12",
          fabs(rr - ro) <= 1e-12 * fabs(ro));
    check("const_weight_big_not_dropped",
          fabs(rr - ro / 1e6) > 0.25 * fabs(ro));
    /* A fold that used ONE hardcoded weight for both kernels would still
     * agree with itself on each. The ratio between them is what catches
     * that: it must be the ratio of the weights, 1e6 / 0.5. */
    check("const_weight_ratio_is_the_weights",
          fabs(rr / hi - 1e6 / 0.5) <= 1e-9 * (1e6 / 0.5));
  }

  /* (n) The weighted spine's refusal, made EXECUTABLE.
   *
   *     Until now the reason for DECLINE-WEIGHT lived only in comments, and
   *     the suite asserted the fact of the refusal without ever evaluating
   *     the inputs the refusal exists for. These are those inputs: the linear
   *     loop is healthy (19719.4) while a state that folded w in would reach
   *     sum|w_i| = inf at n = 2 and reduce to inf.
   *
   *     It passes trivially while the decline holds. It stops passing the
   *     moment a weighted rewrite lands without magnitude handling — including
   *     an otherwise-correct one, which is the case the bit-identity control
   *     above cannot survive being updated for. */
  {
    double wv[2] = {1e308, 1e308};
    double xv[2] = {-700.0, -700.0};
    double wo = weighted_sum_orig(wv, xv, 2);
    double wr = weighted_sum_rw(wv, xv, 2);
    printf("INFO,weight_witness,orig=%.17g,rw=%.17g\n", wo, wr);
    check("weight_witness_linear_is_finite", isfinite(wo));
    check("weight_witness_rw_matches_linear", wo == wr);
  }

  /* (o) f32 widening into a double accumulator: the fpext path, which is a
   *     REWRITING path and had no coverage of any kind before 2026-08-17.
   *
   *     Two different tolerances, and the gap between them is the point. The
   *     original computes (double)expf(x_i) — a float-precision exponential,
   *     carrying ~2^-24 relative per term. The rewritten state widens the
   *     argument exactly and computes exp() in double. So the two agree only
   *     to float precision, and the REWRITE is the more accurate of the two.
   *     What the error contract bounds is the rewritten code against the
   *     exact sum, and that is asserted at 1e-12 against the double
   *     reference. Asserting orig/rw at 1e-12 would be asserting that the
   *     rewrite reproduces the original's float rounding, which it does not
   *     and should not. */
  {
    static float xf[N];
    static double xd[N];
    double so, sr, lg, lref, rel;
    for (i = 0; i < N; ++i) {
      xf[i] = (float)nrand();
      xd[i] = (double)xf[i];
    }
    so = expf_widened_orig(xf, N);
    sr = expf_widened_rw(xf, N);
    lg = __logrange_logsum;
    lref = ref_logsumexp(xd, N);
    rel = fabs(sr - so) / fabs(so);
    printf("INFO,expf_widened,orig=%.17g,rw=%.17g,rel=%.3g,"
           "logsum=%.17g,logref=%.17g\n", so, sr, rel, lg, lref);
    check("cover_expf_widened_rw_agree_float_1e-6", rel < 1e-6);
    check("cover_expf_widened_rw_matches_double_ref",
          fabs(lg - lref) <= 1e-12 * fabs(lref));
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
    check("cover_softmax_full_rw_agree_1e-12", rel < 1e-12);

    softmax_add_orig(x, out_orig, N);
    softmax_add_rw(x, out_rw, N);
    rel = max_rel_diff(out_orig, out_rw, N);
    printf("INFO,softmax_add,max_rel=%.3g\n", rel);
    check("cover_softmax_add_rw_agree_1e-12", rel < 1e-12);

    softmax_sum_div_orig(x, out_orig, N);
    softmax_sum_div_rw(x, out_rw, N);
    rel = max_rel_diff(out_orig, out_rw, N);
    printf("INFO,softmax_sum_div,max_rel=%.3g\n", rel);
    check("cover_softmax_sum_div_rw_agree_1e-12", rel < 1e-12);
  }

  /* (m0) Benign inputs through the propagated path, and the accuracy
   *      ranking the stretch goal's success criterion 2 asserts.
   *
   *      Three paths compute the same softmax: orig is pure linear; rw
   *      rewrites the sum then re-converts with exp(m + log(s)) and divides
   *      in linear; prop replaces the divide with exp(t - L). Reference is
   *      long double (64-bit mantissa on x86-64, 11 bits more than double),
   *      independent of all three.
   *
   *      Criterion 2 claims prop is MORE accurate than the linear
   *      re-conversion, not merely equal. Measured here rather than assumed:
   *      the subtract t - L carries absolute rounding u*|t - L|, which is
   *      relative error in the result, so the claim is not obviously true at
   *      benign magnitudes where the linear path is healthy. */
  {
    static double out_orig[N], out_rw2[N], out_prop2[N];
    long double ml, sl, Ll;
    double e_orig = 0.0, e_rw = 0.0, e_prop = 0.0;
    for (i = 0; i < N; ++i) x[i] = nrand();

    softmax_full_orig(x, out_orig, N);
    softmax_full_rw(x, out_rw2, N);
    softmax_full_prop(x, out_prop2, N);

    ml = -INFINITY;
    for (i = 0; i < N; ++i)
      if ((long double)x[i] > ml) ml = (long double)x[i];
    sl = 0.0L;
    for (i = 0; i < N; ++i) sl += expl((long double)x[i] - ml);
    Ll = ml + logl(sl);
    for (i = 0; i < N; ++i) {
      long double ref = expl((long double)x[i] - Ll);
      double eo = (double)(fabsl((long double)out_orig[i] - ref) / ref);
      double er = (double)(fabsl((long double)out_rw2[i] - ref) / ref);
      double ep = (double)(fabsl((long double)out_prop2[i] - ref) / ref);
      if (eo > e_orig) e_orig = eo;
      if (er > e_rw)   e_rw   = er;
      if (ep > e_prop) e_prop = ep;
    }
    printf("INFO,prop_benign,linear=%.3g,reconvert=%.3g,propagated=%.3g\n",
           e_orig, e_rw, e_prop);
    check("prop_benign_agrees_1e-12",
          max_rel_diff(out_orig, out_prop2, N) < 1e-12);
  }

  /* (m1) The accuracy RANKING, swept rather than sampled once.
   *
   *      A single seed at one spread is an extreme-value statistic (a max
   *      over n outputs) and is not enough to strike a stated criterion.
   *      Sweep spread and length, count how often each path wins, and
   *      report the reference's own error so the reader can see how much
   *      of the gap is real.
   *
   *      Reference error is estimated by recomputing the long-double
   *      reference with Kahan compensation: the difference between the two
   *      is an upper bound on what the reference itself contributes. */
  {
    static double out_o[N], out_r[N], out_p[N];
    const double spreads[] = {0.5, 1.0, 3.0, 8.0};
    int si, trial, len_i;
    const int lens[] = {100, 1000};
    int prop_beats_reconv = 0, trials = 0;
    double worst_ref_err = 0.0;
    double ratio_min = 1e300, ratio_max = 0.0;

    for (si = 0; si < 4; ++si) {
      for (len_i = 0; len_i < 2; ++len_i) {
        int n = lens[len_i];
        for (trial = 0; trial < 8; ++trial) {
          long double ml, sl, sl_k, c_k, Ll, Ll_k;
          double e_r = 0.0, e_p = 0.0;
          for (i = 0; i < n; ++i) x[i] = spreads[si] * nrand();

          softmax_full_orig(x, out_o, n);
          softmax_full_rw(x, out_r, n);
          softmax_full_prop(x, out_p, n);

          ml = -INFINITY;
          for (i = 0; i < n; ++i)
            if ((long double)x[i] > ml) ml = (long double)x[i];
          sl = 0.0L;
          for (i = 0; i < n; ++i) sl += expl((long double)x[i] - ml);
          /* Same sum, Kahan-compensated: the gap bounds reference error. */
          sl_k = 0.0L; c_k = 0.0L;
          for (i = 0; i < n; ++i) {
            long double y = expl((long double)x[i] - ml) - c_k;
            long double t = sl_k + y;
            c_k = (t - sl_k) - y;
            sl_k = t;
          }
          Ll = ml + logl(sl);
          Ll_k = ml + logl(sl_k);
          if (fabsl(Ll - Ll_k) > worst_ref_err)
            worst_ref_err = (double)fabsl(Ll - Ll_k);

          for (i = 0; i < n; ++i) {
            long double ref = expl((long double)x[i] - Ll_k);
            double er = (double)(fabsl((long double)out_r[i] - ref) / ref);
            double ep = (double)(fabsl((long double)out_p[i] - ref) / ref);
            if (er > e_r) e_r = er;
            if (ep > e_p) e_p = ep;
          }
          ++trials;
          if (e_p < e_r) ++prop_beats_reconv;
          if (e_r > 0.0) {
            double ratio = e_p / e_r;
            if (ratio < ratio_min) ratio_min = ratio;
            if (ratio > ratio_max) ratio_max = ratio;
          }
        }
      }
    }
    printf("INFO,prop_ranking,trials=%d,prop_wins=%d,ratio_min=%.2f,"
           "ratio_max=%.2f,ref_logL_err=%.2g\n",
           trials, prop_beats_reconv, ratio_min, ratio_max, worst_ref_err);
  }

  /* (m) Rescue regime, the stretch goal's first milestone. softmax_full_prop
   *     has its normalize
   *     divide rewritten to exp(t - L), where t is the pre-exp argument and
   *     L is the log-domain denominator carried through the loop-exit merge.
   *
   *     t is used DIRECTLY, not as log(numerator). The numerator is exp(t),
   *     which underflows to 0.0 at these inputs — log(0) = -inf would put
   *     the propagated result at 0 or NaN precisely where the rescue is the
   *     whole point. Correctness must not depend on a later InstCombine
   *     fold of log(exp(t)) -> t either; the pass emits the subtract on t.
   *
   *     The linear form is asserted broken on the same inputs, because a
   *     rescue that is not compared against the failure it repairs proves
   *     nothing. exp(x[i]) is 0.0 and the denominator is 0.0, so every
   *     output is 0.0/0.0 = NaN. */
  {
    static double out_prop[N];
    static double out_lin[N];
    int all_finite, lin_broken;
    double prop_sum;
    double lref;
    for (i = 0; i < N; ++i) x[i] = -800.0 + nrand();

    softmax_full_orig(x, out_lin, N);
    lin_broken = 1;
    for (i = 0; i < N; ++i)
      if (isfinite(out_lin[i])) { lin_broken = 0; break; }

    softmax_full_prop(x, out_prop, N);
    lref = ref_logsumexp(x, N);
    all_finite = 1;
    for (i = 0; i < N; ++i)
      if (!isfinite(out_prop[i])) { all_finite = 0; break; }
    prop_sum = 0.0;
    for (i = 0; i < N; ++i) prop_sum += out_prop[i];

    printf("INFO,prop_rescue,linear[0]=%g,logref=%.17g,sum=%.17g\n",
           out_lin[0], lref, prop_sum);
    check("prop_rescue_linear_is_broken", lin_broken);
    check("prop_rescue_all_finite", all_finite);
    check("prop_rescue_sums_to_one", fabs(prop_sum - 1.0) < 1e-10);
  }

  printf(fails ? "OVERALL,FAIL\n" : "OVERALL,PASS\n");
  return fails ? 1 : 0;
}

#endif /* KERNEL */
