/* benign.c — fixture for test_scan.sh: a scan whose exit code must be 0.
 *
 * Two examined FP loops, one hit, no range signal:
 *   dot        HIT, graded LOW (sum of products, no transcendental)
 *   plain_sum  examined, misses (no product and no exponent in the chain)
 *
 * Expected report: loops examined 2, hits 1, HIGH 0, MED 0, LOW 1.
 */
#include <stddef.h>

double dot(const double *a, const double *b, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

double plain_sum(const double *a, size_t n) {
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) s += a[i];
  return s;
}
