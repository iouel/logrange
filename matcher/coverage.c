/* Coverage audit: the shapes this project SAYS it targets, run past the
 * matcher to see which it can actually see.
 *
 * Sources for the list: README ("mixture likelihoods, forward-algorithm
 * recursions, softmax denominators"), logrange_intent.md (sums of products
 * whose terms individually underflow), matcher/METHODOLOGY.md (likelihood /
 * kernel / softmax shapes).
 *
 * Same method that found the nMul bug: state the target, compile it, ask the
 * matcher. */
#include <math.h>
#include <stddef.h>

/* 1. Mixture likelihood — intent success criterion 1, named directly. */
double mixture(const double *w, const double *logp, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += w[i] * exp(logp[i]);
  return s;
}

/* 2. Softmax denominator, textbook form (no temperature). */
double softmax_denom(const double *x, size_t n, double largest) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += exp(x[i] - largest);
  return s;
}

/* 3. Forward algorithm, one time step. README names this. The accumulator
 *    is a REGISTER here (a[j] written once after the inner loop). */
void forward_step_reg(const double *prev, const double *A, const double *B,
                      double *out, size_t n, size_t j) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += prev[i] * A[i * n + j];
  out[j] = s * B[j];
}

/* 4. Forward algorithm as it is usually written: the accumulator IS the
 *    array cell. Memory-carried — a documented blind spot until 2026-08-17,
 *    when recognition moved to LLVM's RecurrenceDescriptor, which handles
 *    stores to loop-invariant addresses. Now a hit; see matcher/DELTA.md. */
void forward_step_mem(const double *prev, const double *A, double *out,
                      size_t n) {
  for (size_t j = 0; j < n; ++j) {
    out[j] = 0.0;
    for (size_t i = 0; i < n; ++i) out[j] += prev[i] * A[i * n + j];
  }
}

/* 5. Product of likelihoods — the pure-product case the intent says belongs
 *    to exponent-tracking, not here. Should NOT be a sum-of-products hit. */
double likelihood_product(const double *p, size_t n) {
  double prod = 1.0;
  for (size_t i = 0; i < n; ++i) prod *= p[i];
  return prod;
}

/* 6. Log-sum-exp written out by hand — someone who already knows the trick.
 *    Rewriting this is pointless; the matcher seeing it is still useful for
 *    the diagnostic ("you did this by hand, here is a tested version"). */
double manual_lse(const double *x, size_t n) {
  double m = -INFINITY;
  for (size_t i = 0; i < n; ++i) if (x[i] > m) m = x[i];
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += exp(x[i] - m);
  return m + log(s);
}

/* 7. Weighted kernel sum — libsvm-style, the third named family. */
double kernel_sum(const double *alpha, const double *k, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += alpha[i] * k[i];
  return s;
}

/* 8 and 9. The forward algorithm WITH its enclosing time-step loop.
 *
 * Cases 3 and 4 above are one time step each, which is why neither can
 * exercise a cross-loop risk signal: the magnitude decay that makes this
 * family underflow lives in the loop these two do not contain. Added
 * 2026-08-17 so the shape README names is present in the form that actually
 * underflows, and so the open item has a concrete target.
 *
 * BOTH are asserted as HITS AT LOW, which is the current, wrong-for-the-user
 * verdict: the matcher sees the reduction and grades each inner iteration
 * unremarkable, because risk is judged one loop at a time. When a cross-loop
 * signal lands, these turn red and the coverage table must be updated with
 * them — which is the point of asserting a gap rather than describing one
 * (TODO.md, "Per-loop risk cannot see cross-loop decay").
 *
 * Two forms, because they are different DETECTION problems and a rule that
 * handles only the first would look finished:
 *   8. one buffer indexed by time step — the store and the load share an
 *      underlying object, which a cheap proxy can see.
 *   9. two buffers swapped each step — the textbook form. The store and the
 *      load reach different objects through a rotating pointer pair, so the
 *      same proxy does not see it without following the swap. */
void forward_full_flat(const double *A, const double *B, const int *obs,
                       double *al, size_t n, size_t T) {
  for (size_t t = 1; t < T; ++t)
    for (size_t j = 0; j < n; ++j) {
      double s = 0.0;
      for (size_t i = 0; i < n; ++i) s += al[(t - 1) * n + i] * A[i * n + j];
      al[t * n + j] = s * B[obs[t] * n + j];
    }
}

void forward_full_swap(const double *A, const double *B, const int *obs,
                       double *buf0, double *buf1, size_t n, size_t T) {
  double *prev = buf0, *cur = buf1;
  for (size_t t = 1; t < T; ++t) {
    for (size_t j = 0; j < n; ++j) {
      double s = 0.0;
      for (size_t i = 0; i < n; ++i) s += prev[i] * A[i * n + j];
      cur[j] = s * B[obs[t] * n + j];
    }
    { double *tmp = prev; prev = cur; cur = tmp; }
  }
}
