/* emitted_bound_search.c — adversarial search against an error bound for the
 * code the rewrite pass EMITS (shipping-posture condition 4).
 *
 * This is not a model of the emitted code. It calls softmax_denom_rw, the
 * object the pass actually rewrote, linked by run_pass_test.sh. A bound
 * derived against a hand-written replica would bound the replica.
 *
 * WHAT IS BEING BOUNDED
 *
 * The kernel is `for (i) s += exp(x[i])`. The pass replaces it with the
 * maxnum-based streaming state (PROTOTYPE.md, "The maxnum-based derivation"):
 *
 *     m = -inf, s = 0
 *     per term t:  newm = max(m, t)
 *                  dm   = (m oeq newm) ? 0 : m - newm
 *                  dt   = (t oeq newm) ? 0 : t - newm
 *                  s    = s*exp(dm) + exp(dt);  m = newm
 *     result:      exp(m + log(s))
 *
 * Term for term this is pos_accum's arithmetic. When t <= m the guard makes
 * dm exactly 0, exp(0) is exactly 1.0, and s*1.0 is exact, so the plain
 * accumulate step rounds exactly once, as pos_accum's does. When t > m it is
 * pos_accum's rescale: one exp, one multiply, one add of 1.0.
 *
 * Two assumptions the runtime does not need are therefore load-bearing here,
 * and are asserted at startup rather than trusted: exp(0.0) == 1.0 exactly,
 * and x*1.0 == x. Under a merely-1-ulp exp(0) the guarded branch would add
 * n*u that pos_accum never pays.
 *
 * CANDIDATE BOUND, inherited from pos_accum's (n + 3k + 3 + D)*u + |log|S||*u
 * with one term added for the final exp() that pos_accum does not perform:
 *
 *     rel err  <=  (n + 3k + 4 + D)*u  +  (|log|S|| + |log|net||)*u
 *
 * net = S/exp(m) is the scaled sum the reduction takes the log of. That
 * second reduction term was added 2026-08-16, after it refuted rp_accum's
 * contract at 1.99x: m + log(net) has TWO addends and each rounds at its own
 * magnitude, while |log|S|| charges only the addition's result. It does not
 * bind here, because |log|net|| <= log n and the n*u term dominates log n,
 * and it is carried so the statement is correct rather than unfalsified.
 * Family E7 measures it rather than assuming it.
 *
 * SCOPE. The emitted code returns a linear double, so the result must be a
 * normal double: |log|S|| < 709.78 caps the reduction term near 710u, a
 * ceiling the runtime's log_value form does not have. Trials whose reference
 * leaves that range are skipped and counted, not silently dropped.
 *
 * WEIGHTED CANDIDATE. A bounded constant weight is rewritten as
 * exp(t + fl(log w)) rather than w*exp(t), which keeps the state's ceiling at
 * n. That is NOT the same error path as the unweighted form and the existing
 * candidate does not cover it, so it gets its own bound and its own search
 * rather than inheriting the runtime's add_scaled term by analogy:
 *
 *     rel err  <=  (n + 3k + 4 + D)*u + (|log|S|| + |log|net||)*u
 *                  + (|log w| + max_i |t'_i|)*u
 *
 * Two sources, neither present unweighted. fl(log w) differs from log w by up
 * to |log w|*u under the <=1-ulp libm assumption this file already makes for
 * exp(), and exp carries that straight through as a relative error on every
 * term. And t' = fl(x_i + fl(log w)) rounds at its own magnitude, so exp(t')
 * inherits |t'|*u — where the unweighted exponent is a loaded value and
 * rounds not at all. max_i is used rather than a mass weighting because the
 * term is charged once per term regardless of that term's share.
 *
 * REFERENCE FLOOR. expl() at 64-bit mantissa, Kahan-summed. All terms are
 * positive, so per-term relative errors average rather than accumulate and
 * the reference carries ~2^-64 relative, about 0.001u. Ratios below ~0.01
 * are not evidence. Ratios reported here are far above that.
 *
 * RESULT, 2026-08-16. Held across 7285 trials, worst observed/bound 0.99.
 * Family E7 is the one that refuted the runtime's rp_accum contract at 1.99x
 * on the same day; here it reaches 0.99 at worst like everything else, which
 * confirms rather than assumes that the n*u term absorbs it.
 * The binding case is E2/E6: a large cluster one depth below a dominant
 * term, where every add of the running sum rounds the same direction and the
 * observed error is essentially the classical (n-1)u of uncompensated
 * summation. The ratio saturates just under 1 from below and stops climbing
 * as N grows past 2048, which is what that mechanism predicts: the bound
 * charges n*u for an error whose worst case is (n-1)*u.
 *
 * The same run reports the form WITHOUT the reduction term, which is
 * exceeded on 321 of those trials at up to 39x. The reduction term is not
 * inherited from pos_accum by analogy; it is required here by measurement.
 *
 * DERIVED LIMIT, not searched. The classical relative error of recursive
 * summation is (n-1)u/(1-(n-1)u), which exceeds the (n+4)u this bound
 * charges once (n-1)(n+4)u > 5, i.e. n > ~2.1e8 terms. That is where the
 * neglected second-order term eats the constant. The search reaches n =
 * 16385, four orders below it. Like the runtime's, this is a first-order
 * bound and this is the n at which "first-order" starts to matter.
 *
 * Exit 0 if every ratio against the candidate is <= 1, 1 otherwise. A ratio
 * over 1 refutes the bound; that is the point of the file.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define U 0x1p-53

double softmax_denom_rw(const double *x, int n);   /* rewritten by the pass */
double softmax_denom_orig(const double *x, int n); /* the linear original */

/* Bounded-constant-weight kernels, rewritten by folding log(w) into the
 * exponent. Searched separately because the fold adds error the unweighted
 * candidate does not budget for — see WEIGHTED CANDIDATE below. */
double const_weight_sum_rw(const double *x, int n); /* w = 0.5 */
double const_weight_big_rw(const double *x, int n); /* w = 1e6 */

/* The pass's export hook. The consuming link must define it. */
double __logrange_logsum = 0.0;

/* ------------------------------------------------------------------ trials */

#define MAXN 32768
static double buf[MAXN];

struct verdict {
  int n, k;
  double depth, outmag;
  double obs_lin;    /* relative error of the emitted linear result */
  double obs_log;    /* absolute error of the exported log form, = relative */
  double lognet;     /* |log|net||, the reduction's other addend */
  double bound_nored;/* (n + 3k + 4 + D)*u, the plausible-but-wrong form */
  double bound_full; /* the candidate, with the reduction term */
  double ratio_nored, ratio_full, ratio_log;
  int skipped;
};

/* Kahan over expl(). Positive terms only, so this is the reference. */
static long double ref_sum(const double *x, int n) {
  long double s = 0.0L, c = 0.0L;
  for (int i = 0; i < n; ++i) {
    long double y = expl((long double)x[i]) - c;
    long double t = s + y;
    c = (t - s) - y;
    s = t;
  }
  return s;
}

static struct verdict evaluate(const double *x, int n) {
  struct verdict v;
  double m = -INFINITY;
  int k = 0, first = 1;
  long double wsum = 0.0L, wdepth = 0.0L;

  for (int i = 0; i < n; ++i) {
    if (x[i] > m) {
      if (!first) ++k;
      m = x[i];
    }
    first = 0;
    long double w = expl((long double)x[i]);
    wsum   += w;
    wdepth += w * (long double)(m - x[i]);
  }

  const long double truth = ref_sum(x, n);
  double peak = -INFINITY;
  for (int i = 0; i < n; ++i) if (x[i] > peak) peak = x[i];
  v.n      = n;
  v.k      = k;
  v.depth  = (double)(wdepth / wsum);
  v.outmag = fabs((double)logl(truth));
  v.skipped = 0;

  /* The emitted code returns a linear double. Outside the normal range the
   * relative-error statement is not about the algorithm, it is about
   * overflow. Skip and count. */
  const long double amax = 8.98846567431158e307L;   /* 2^1023 */
  const long double amin = 2.2250738585072014e-308L; /* 2^-1022 */
  if (!(truth > amin && truth < amax)) {
    v.skipped = 1;
    v.obs_lin = v.obs_log = v.ratio_nored = v.ratio_full = v.ratio_log = 0.0;
    v.bound_nored = v.bound_full = 0.0;
    return v;
  }

  __logrange_logsum = 0.0;
  const double got = softmax_denom_rw(x, n);
  const double lg  = __logrange_logsum;

  /* |log|net||: net = S/exp(peak) is what the reduction takes the log of.
   * Added 2026-08-16 after this term refuted rp_accum's contract at 1.99x.
   * It does not bind here (|log|net|| <= log n, and the n*u term dominates),
   * and is carried so the statement is correct rather than unfalsified. */
  v.lognet = (double)fabsl(logl(truth) - (long double)peak);
  v.obs_lin = (double)(fabsl((long double)got - truth) / truth);
  /* Absolute error in log space is relative error in linear space. */
  v.obs_log = (double)fabsl((long double)lg - logl(truth));

  v.bound_nored = ((double)n + 3.0 * (double)k + 4.0 + v.depth) * U;
  v.bound_full  = v.bound_nored + (v.outmag + v.lognet) * U;
  v.ratio_nored = v.obs_lin / v.bound_nored;
  v.ratio_full  = v.obs_lin / v.bound_full;
  v.ratio_log   = v.obs_log / v.bound_full;
  return v;
}

/* Weighted variant. Same emitted state machine, one extra fadd per term
 * folding fl(log w) into the exponent. Calls the object the pass rewrote, as
 * the unweighted path does — a bound derived against a replica would bound
 * the replica. */
static struct verdict evaluate_w(const double *x, int n, double w,
                                 double (*fn)(const double *, int)) {
  struct verdict v = {0};
  double m = -INFINITY;
  int k = 0, first = 1;
  long double wsum = 0.0L, wdepth = 0.0L;
  /* The same constant the pass embedded: run_pass_test.sh asserts the pass's
   * emitted bit pattern equals the host libm's log(w), so this is it. */
  const double LT = log(w);
  double maxtp = 0.0;

  for (int i = 0; i < n; ++i) {
    const double tp = x[i] + LT; /* t', in double, exactly as emitted */
    if (fabs(tp) > maxtp) maxtp = fabs(tp);
    if (tp > m) {
      if (!first) ++k;
      m = tp;
    }
    first = 0;
    long double e = expl((long double)tp);
    wsum   += e;
    wdepth += e * (long double)(m - tp);
  }

  /* Reference: the mathematical sum w * Σexp(x_i), not Σexp(t'_i). The
   * difference between them IS the error being bounded. */
  const long double truth = (long double)w * ref_sum(x, n);
  v.n = n;
  v.k = k;
  v.depth = (double)(wdepth / wsum);
  v.outmag = fabs((double)logl(truth));
  v.skipped = 0;

  const long double amax = 8.98846567431158e307L;
  const long double amin = 2.2250738585072014e-308L;
  if (!(truth > amin && truth < amax)) {
    v.skipped = 1;
    return v;
  }

  __logrange_logsum = 0.0;
  const double got = fn(x, n);
  const double lg = __logrange_logsum;

  double peak = -INFINITY;
  for (int i = 0; i < n; ++i) if (x[i] + LT > peak) peak = x[i] + LT;
  v.lognet = (double)fabsl(logl(truth) - (long double)peak);
  v.obs_lin = (double)(fabsl((long double)got - truth) / truth);
  v.obs_log = (double)fabsl((long double)lg - logl(truth));

  /* Unweighted candidate, i.e. this bound with the two weight terms removed.
   * Scored on every trial so their necessity is MEASURED, the same way the
   * reduction term's is: if this never exceeded 1, the weight terms would be
   * padding and the bound would be overstating what the fold costs. */
  const double bound_noweight =
      ((double)n + 3.0 * (double)k + 4.0 + v.depth) * U
      + (v.outmag + v.lognet) * U;
  v.bound_nored = bound_noweight;
  v.bound_full = bound_noweight + (fabs(LT) + maxtp) * U;
  v.ratio_nored = v.obs_lin / v.bound_nored;
  v.ratio_full = v.obs_lin / v.bound_full;
  v.ratio_log = v.obs_log / v.bound_full;
  return v;
}

/* ---------------------------------------------------------------- families */

/* Where the cluster family came closest, so E6 can refine there instead of
 * trusting that a fixed grid found the maximum. Non-cluster families clear
 * cluster_N so they cannot claim a cluster's coordinates. */
static double best_d = 8.0;
static int best_N = 256;
static double cluster_d = 0.0;
static int cluster_N = 0;

/* E1 — few terms at extreme magnitude. n*u is tiny and |log|S|| is ~700:
 * the shortest path to the term that refuted pos_accum's earlier form. */
static int fam_magnitude(double peak, int n) {
  cluster_N = 0;
  for (int i = 0; i < n; ++i) buf[i] = peak;
  return n;
}

/* E2 — one dominant term plus a cluster sharing one depth. The cluster's
 * argument roundings are coherent, which is the mechanism that refuted
 * rp_accum's (3k+4)*u. Expected to be covered by n*u here; measured. */
static int fam_depth_cluster(double depth, int N) {
  buf[0] = 10.5;
  for (int i = 0; i < N; ++i) buf[i + 1] = 10.5 - depth;
  cluster_d = depth;
  cluster_N = N;
  return N + 1;
}

/* E3 — ascending, k == n-1, with a tuned jump. This is the family the
 * candidate is least likely to survive. Each rescale forms exp(m - t) whose
 * ARGUMENT rounds to u*J for a jump of J log-units, and 3k*u charges nothing
 * for J. The surviving weight of that error is the old mass's share of the
 * new total, ~s/(s + e^J), so J ~ ln(n) should be the worst case rather than
 * J large. Nothing in the derivation says the product stays under 3u. */
static int fam_ascending(double base, double jump, int n) {
  cluster_N = 0;
  for (int i = 0; i < n; ++i) buf[i] = base + jump * (double)i;
  return n;
}

/* E4 — random over the two axes that set the budget: term count, and how far
 * the result sits from 1.0. */
static unsigned long long rng_state = 0x243F6A8885A308D3ull;
static double urand(void) {
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return (double)((rng_state >> 11) & 0x1FFFFFFFFFFFFFull) / 9007199254740992.0;
}
static int fam_random(void) {
  cluster_N = 0;
  const int n       = 1 + (int)(urand() * 1500.0);
  const double peak = -690.0 + urand() * 1380.0;
  const double span = urand() * 40.0;
  for (int i = 0; i < n; ++i) buf[i] = peak - urand() * span;
  /* Half the trials ascending, which turns the spread into k rescales. */
  if (urand() < 0.5)
    for (int i = 1; i < n; ++i)
      if (buf[i] < buf[i - 1]) {
        const double t = buf[i];
        buf[i] = buf[i - 1];
        buf[i - 1] = t;
      }
  return n;
}

/* E7 - n equal terms at L = -log(n): net = n exactly, |log|S|| = 0, k = 0,
 * D = 0. This is the family that refuted rp_accum's contract, run here to
 * confirm the n*u term covers it for the emitted code rather than assuming
 * so. */
static int fam_equal_normalized(int n) {
  const double L = -log((double)n);
  for (int i = 0; i < n; ++i) buf[i] = L;
  return n;
}

/* ------------------------------------------------------------------ report */

static double worst_full = 0.0, worst_nored = 0.0, worst_log = 0.0;
static int violations = 0, nored_violations = 0, skipped = 0, trials = 0;
static char worst_label[128] = "none";
static char nored_label[128] = "none";

static void run(const char *label, int n, int verbose) {
  struct verdict v = evaluate(buf, n);
  if (v.skipped) { ++skipped; return; }
  ++trials;
  if (v.ratio_full > worst_full) {
    worst_full = v.ratio_full;
    snprintf(worst_label, sizeof worst_label, "%s", label);
    if (cluster_N > 0) { best_d = cluster_d; best_N = cluster_N; }
  }
  if (v.ratio_nored > worst_nored) {
    worst_nored = v.ratio_nored;
    snprintf(nored_label, sizeof nored_label, "%s", label);
  }
  if (v.ratio_log > worst_log) worst_log = v.ratio_log;
  if (v.ratio_full > 1.0 || v.ratio_log > 1.0) ++violations;
  if (v.ratio_nored > 1.0) ++nored_violations;
  if (verbose || v.ratio_full > 1.0)
    printf("  %-34s %6d %5d %6.1f %6.1f %9.2e %9.2e %8.2f %8.2f\n", label, v.n,
           v.k, v.depth, v.outmag, v.obs_lin, v.obs_log, v.ratio_nored,
           v.ratio_full);
}

/* Weighted counters, kept apart so a weighted violation cannot be masked by
 * the unweighted margin, and so the two bounds are reported separately. */
static double worst_w = 0.0, worst_w_noweight = 0.0;
static int w_violations = 0, w_trials = 0, w_skipped = 0, w_noweight_exceeded = 0;
static char worst_w_label[128] = "none", w_noweight_label[128] = "none";

static void run_w(const char *label, int n, double w,
                  double (*fn)(const double *, int)) {
  struct verdict v = evaluate_w(buf, n, w, fn);
  if (v.skipped) { ++w_skipped; return; }
  ++w_trials;
  if (v.ratio_full > worst_w) {
    worst_w = v.ratio_full;
    snprintf(worst_w_label, sizeof worst_w_label, "%s", label);
  }
  if (v.ratio_nored > worst_w_noweight) {
    worst_w_noweight = v.ratio_nored;
    snprintf(w_noweight_label, sizeof w_noweight_label, "%s", label);
  }
  if (v.ratio_nored > 1.0) ++w_noweight_exceeded;
  if (v.ratio_full > 1.0 || v.ratio_log > 1.0) {
    ++w_violations;
    printf("  %-34s %6d %5d %6.1f %6.1f %9.2e %9.2e %8.2f %8.2f\n", label, v.n,
           v.k, v.depth, v.outmag, v.obs_lin, v.obs_log, v.ratio_nored,
           v.ratio_full);
  }
}

int main(void) {
  /* The guarded accumulate step is exact only if these hold. */
  if (exp(0.0) != 1.0)  { printf("FAIL,exp0_not_exactly_one\n"); return 1; }
  if (1.0 * 3.7 != 3.7) { printf("FAIL,multiply_by_one_not_exact\n"); return 1; }

  printf("adversarial search against the EMITTED code's error contract\n");
  printf("candidate: rel err <= (n + 3k + 4 + D)*u + |log|S||*u\n");
  printf("  ratio_nored drops the reduction term; ratio_full is the candidate\n\n");
  printf("  %-34s %6s %5s %6s %6s %9s %9s %8s %8s\n", "family", "n", "k", "D",
         "|logS|", "rel(lin)", "abs(log)", "nored", "full");

  printf("-- E1 magnitude ------------------------------------------------\n");
  const double peaks[] = {-690, -400, -100, 0, 100, 400, 690};
  for (unsigned p = 0; p < sizeof peaks / sizeof *peaks; ++p)
    for (int n = 1; n <= 64; n *= 8) {
      char lab[128];
      snprintf(lab, sizeof lab, "peak=%.0f n=%d", peaks[p], n);
      run(lab, fam_magnitude(peaks[p], n), 1);
    }

  /* Swept, not sampled: the depth that rounds worst is not a round number,
   * and the margin here is the thinnest in the file. 0.0137 is an
   * irrational-ish step so the grid does not land on exact binary values. */
  printf("-- E2 depth cluster, 3 x 200 depths ----------------------------\n");
  const int clus_n[] = {32, 256, 2048};
  for (unsigned i = 0; i < sizeof clus_n / sizeof *clus_n; ++i) {
    double worst_before = worst_full;
    char lab[128];
    for (int j = 0; j < 200; ++j) {
      const double d = 0.5 + 0.0137 * (double)j * 7.0;
      snprintf(lab, sizeof lab, "depth=%.3f N=%d", d, clus_n[i]);
      run(lab, fam_depth_cluster(d, clus_n[i]), 0);
    }
    printf("  N=%-6d 200 depths in [0.5, 19.7], worst full ratio now %.2f\n",
           clus_n[i], worst_full > worst_before ? worst_full : worst_before);
  }

  printf("-- E3 ascending, k = n-1 ---------------------------------------\n");
  const int asc_n[] = {4, 32, 256, 2048};
  for (unsigned i = 0; i < sizeof asc_n / sizeof *asc_n; ++i) {
    const int n = asc_n[i];
    /* Jump capped so the top term stays inside exp()'s range: the whole
     * ascent has to fit in the ~1380 log-units the double exponent spans. */
    const double jmax = 1370.0 / (double)(n - 1);
    for (int j = 0; j < 40; ++j) {
      const double jump = jmax * (double)(j + 1) / 40.0;
      char lab[128];
      snprintf(lab, sizeof lab, "jump=%.3f n=%d", jump, n);
      run(lab, fam_ascending(-685.0, jump, n), 0);
    }
    printf("  n=%-6d 40 jumps in (0, %.2f], worst full ratio now %.2f\n", n,
           jmax, worst_full);
  }

  /* E5 — many terms AND extreme magnitude, so the n*u and |log|S||*u terms
   * are both live. Either alone is well covered; the question is the sum. */
  printf("-- E5 large n at extreme magnitude -----------------------------\n");
  const int big_n[] = {512, 4096};
  const double big_peak[] = {-685.0, -300.0, 300.0, 685.0};
  for (unsigned i = 0; i < sizeof big_n / sizeof *big_n; ++i)
    for (unsigned p = 0; p < sizeof big_peak / sizeof *big_peak; ++p)
      for (int j = 0; j < 25; ++j) {
        const double d = 0.0137 * (double)j * 40.0;
        char lab[128];
        snprintf(lab, sizeof lab, "peak=%.0f n=%d depth=%.2f", big_peak[p],
                 big_n[i], d);
        cluster_N = 0;
        buf[0] = big_peak[p];
        for (int q = 1; q < big_n[i]; ++q) buf[q] = big_peak[p] - d;
        run(lab, big_n[i], 0);
      }
  printf("  200 combinations, worst full ratio now %.2f\n", worst_full);

  printf("-- E4 random, 4000 trials --------------------------------------\n");
  for (int i = 0; i < 4000; ++i) {
    char lab[128];
    snprintf(lab, sizeof lab, "random#%d", i);
    run(lab, fam_random(), 0);
  }
  printf("  worst full ratio now %.2f\n", worst_full);

  /* E6 — refine around wherever the sweeps came closest. A fixed grid that
   * stops at 0.98 has not established anything: the next grid point may be
   * over 1. This walks in from the worst cluster the sweeps found, on a
   * 0.002-wide depth grid and up to 8x the term count, and is the reason the
   * reported worst ratio is a searched maximum rather than a sampled one. */
  printf("-- E6 refinement around the worst cluster ----------------------\n");
  printf("  entering at depth=%.4f N=%d, ratio %.3f\n", best_d, best_N,
         worst_full);
  for (int mult = 1; mult <= 8; mult *= 2) {
    const int N = best_N * mult;
    if (N + 1 >= MAXN) break;
    for (int j = -250; j <= 250; ++j) {
      const double d = best_d + 0.002 * (double)j;
      if (d <= 0.0) continue;
      char lab[128];
      snprintf(lab, sizeof lab, "refine depth=%.4f N=%d", d, N);
      run(lab, fam_depth_cluster(d, N), 0);
    }
    printf("  N=%-6d 501 depths within +-0.5, worst full ratio now %.3f\n", N,
           worst_full);
  }

  printf("-- E7 reduction cancellation (the family that broke rp_accum) --\n");
  for (int i = 0; i < 300; ++i) {
    const int n = (int)(pow(20000.0, (double)i / 299.0)) + 1;
    char lab[128];
    snprintf(lab, sizeof lab, "equal-normalized n=%d", n);
    run(lab, fam_equal_normalized(n), 0);
  }
  printf("  300 sizes, worst full ratio now %.2f\n", worst_full);

  /* E8 — the weighted fold, against its own candidate. Reuses the shapes that
   * bind hardest above (extreme magnitude, ascending rescales, depth
   * clusters, equal-normalized) rather than inventing new ones: the weight
   * adds terms, it does not change which shapes stress the state machine.
   * Both weights are swept because a fold that used one hardcoded constant
   * would still be self-consistent on either alone. */
  printf("-- E8 bounded constant weight, w = 0.5 and 1e6 ------------------\n");
  {
    const double ws[] = {0.5, 1e6};
    double (*const fns[])(const double *, int) = {const_weight_sum_rw,
                                                  const_weight_big_rw};
    for (unsigned q = 0; q < 2; ++q) {
      char lab[128];
      /* magnitude */
      for (unsigned p = 0; p < sizeof peaks / sizeof *peaks; ++p)
        for (int n = 1; n <= 64; n *= 8) {
          snprintf(lab, sizeof lab, "w=%g peak=%.0f n=%d", ws[q], peaks[p], n);
          run_w(lab, fam_magnitude(peaks[p], n), ws[q], fns[q]);
        }
      /* depth clusters */
      for (unsigned i = 0; i < sizeof clus_n / sizeof *clus_n; ++i)
        for (int j = 0; j < 120; ++j) {
          const double d = 0.5 + 0.0137 * (double)j * 7.0;
          snprintf(lab, sizeof lab, "w=%g depth=%.3f N=%d", ws[q], d, clus_n[i]);
          run_w(lab, fam_depth_cluster(d, clus_n[i]), ws[q], fns[q]);
        }
      /* ascending, k = n-1 */
      for (unsigned i = 0; i < sizeof asc_n / sizeof *asc_n; ++i) {
        const int n = asc_n[i];
        const double jmax = 1370.0 / (double)(n - 1);
        for (int j = 0; j < 40; ++j) {
          const double jump = jmax * (double)(j + 1) / 40.0;
          snprintf(lab, sizeof lab, "w=%g jump=%.3f n=%d", ws[q], jump, n);
          run_w(lab, fam_ascending(-685.0, jump, n), ws[q], fns[q]);
        }
      }
      /* equal-normalized, the family that broke rp_accum */
      for (int i = 0; i < 200; ++i) {
        const int n = (int)(pow(20000.0, (double)i / 199.0)) + 1;
        snprintf(lab, sizeof lab, "w=%g equal-normalized n=%d", ws[q], n);
        run_w(lab, fam_equal_normalized(n), ws[q], fns[q]);
      }
      /* random */
      for (int i = 0; i < 1500; ++i) {
        const int n = fam_random();
        snprintf(lab, sizeof lab, "w=%g random#%d", ws[q], i);
        run_w(lab, n, ws[q], fns[q]);
      }
    }
  }
  printf("  weighted trials=%d skipped=%d worst=%.2f (%s)\n", w_trials,
         w_skipped, worst_w, worst_w_label);

  printf("\ntrials=%d skipped(out of double range)=%d violations=%d\n", trials,
         skipped, violations);
  printf("worst observed/bound: full=%.2f (%s)  log-export=%.2f\n", worst_full,
         worst_label, worst_log);
  printf("without the reduction term: worst=%.2f (%s), exceeded on %d of %d "
         "trials\n", worst_nored, nored_label, nored_violations, trials);
  printf("weighted fold: worst=%.2f (%s), exceeded on %d of %d trials\n",
         worst_w, worst_w_label, w_violations, w_trials);
  printf("  without the weight terms: worst=%.2f (%s), exceeded on %d of %d\n",
         worst_w_noweight, w_noweight_label, w_noweight_exceeded, w_trials);
  if (violations > 0 || w_violations > 0) {
    printf("REFUTED,%d,%.2f\n", violations + w_violations,
           worst_full > worst_w ? worst_full : worst_w);
    return 1;
  }
  printf("HELD,%d,%.2f\n", trials + w_trials,
         worst_full > worst_w ? worst_full : worst_w);
  return 0;
}
