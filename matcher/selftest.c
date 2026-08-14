/* selftest.c — labeled ground truth for the matcher. Compiled at the study's
 * exact flags; run_study.sh asserts the matcher's output matches the labels
 * below BEFORE any real-codebase numbers are collected. If this gate fails,
 * no study numbers exist that day.
 *
 * Expected: 4 HITs (dot, sop3, mixture_likelihood, fsub_reduction)
 *           2 examined-but-miss (plain_sum, midread)
 *           integer_loop not examined at all.
 */
#include <math.h>
#include <stddef.h>

/* HIT: the canonical dot product. */
double dot(const double *a, const double *b, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

/* HIT: three-factor product chain with an intermediate add. */
double sop3(const double *a, const double *b, const double *c, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += a[i] * b[i] * c[i] + a[i] * 0.5;
  return s;
}

/* HIT (transcendental): the mixture-likelihood shape the project targets. */
double mixture_likelihood(const double *w, const double *logp, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += w[i] * exp(logp[i]);
  return s;
}

/* HIT: fsub accumulator (s -= a*b is still a sum-of-products reduction). */
double fsub_reduction(const double *a, const double *b, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s -= a[i] * b[i];
  return s;
}

/* MISS (examined): plain sum, no product in the chain. Rescuable without
 * logsumexp; deliberately out of scope. */
double plain_sum(const double *a, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += a[i];
  return s;
}

/* MISS (examined): accumulator read mid-loop — rewriting would change the
 * value observed by out[i]. */
double midread(const double *a, const double *b, double *out, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) {
    s += a[i] * b[i];
    out[i] = s; /* prefix sums: the mid-loop read */
  }
  return s;
}

/* Not examined: integer-only loop. */
long integer_loop(const long *a, size_t n) {
  long s = 0;
  for (size_t i = 0; i < n; ++i) s += a[i] * 3;
  return s;
}
