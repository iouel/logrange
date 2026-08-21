/* instr_control.c — controls for the rescue instrument (RESCUE.md, R1).
 *
 * Compiled to bitcode, instrumented with sop-instrument, linked against
 * rescue_shim.cpp and run. Proves the probe fires in BOTH directions on real
 * IR, not just at shim level: a probe that cannot report "not rescued" cannot
 * tell a safe site from a broken instrument.
 *
 * Kept separate from coverage.c, which is the MATCHER's expectation table and
 * is asserted line by line by `run_study.sh coverage`. Adding driver code
 * there would change what that gate scans.
 */
#include <stddef.h>
#include <math.h>

/* POSITIVE CONTROL. The marquee shape. Every exp(logp) underflows to 0.0 in
 * double, so the linear sum is exactly 0.0 while the true sum is finite and
 * perfectly representable in log form. */
__attribute__((noinline)) double ctl_mixture(const double *w, const double *logp, size_t n) {
  double s = 0.0;
  size_t i;
  for (i = 0; i < n; ++i) s += w[i] * exp(logp[i]);
  return s;
}

/* NEGATIVE CONTROL. Ordinary dot product in ordinary range: nothing to
 * rescue. */
__attribute__((noinline)) double ctl_dot(const double *x, const double *y, size_t n) {
  double s = 0.0;
  size_t i;
  for (i = 0; i < n; ++i) s += x[i] * y[i];
  return s;
}

/* f32 accumulator with a narrowing exp, the blas.c:315 shape. Present so the
 * TRUNCF path is exercised on real IR rather than only in the shim's own
 * test. */
__attribute__((noinline)) float ctl_softmax_f32(const float *x, int n) {
  float s = 0.0f;
  int i;
  for (i = 0; i < n; ++i) s += (float)exp((double)x[i]);
  return s;
}

/* UNLOGIFIABLE CONTROL. sin() is accepted by the matcher's chain walk, so this
 * is a HIT, but it is not decomposable: taking log|sin(x)| would reconstruct
 * whatever sin already collapsed internally. The instrument must DECLINE this
 * with reason `opaque-call` rather than treat the call as a leaf.
 *
 * Added after a mutation test found the rule unguarded: relaxing opaque calls
 * to leaves left every other control green, because nothing here had an
 * undecomposable call in a term chain. */
__attribute__((noinline)) double ctl_opaque(const double *x, const double *y, size_t n) {
  double s = 0.0;
  size_t i;
  for (i = 0; i < n; ++i) s += sin(x[i]) * y[i];
  return s;
}

extern void lr_report(void);

int main(void) {
  enum { N = 100 };
  double w[N], logp[N], x[N], y[N];
  float fx[N];
  int i;

  for (i = 0; i < N; ++i) {
    w[i] = 1.0;
    logp[i] = -800.0;      /* below double's exp underflow limit (~-745) */
    x[i] = 1.0 + 0.01 * i;
    y[i] = 2.0 - 0.005 * i;
    fx[i] = -110.0f;       /* below float's subnormal floor (~-103.3) */
  }

  volatile double a = ctl_mixture(w, logp, N);
  volatile double b = ctl_dot(x, y, N);
  volatile double d = ctl_opaque(x, y, N);
  volatile float c = ctl_softmax_f32(fx, N);
  (void)a; (void)b; (void)c; (void)d;

  lr_report();
  return 0;
}
