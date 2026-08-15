// dd_sum.h — double-double compensated summation, the accuracy reference.
//
// Shared by test_accuracy.cpp (fixed scenarios) and bound_search.cpp (the
// adversarial search). One copy so the two cannot drift apart and disagree
// about what "truth" means.
//
// TwoSum (Knuth): s = fl(a+b) plus the exact rounding error e, valid for any
// a, b. Operations are kept in separate statements through named temporaries
// so the compiler cannot fuse or reorder the compensation; /fp:precise
// (MSVC) / -ffp-contract=off (GCC/Clang) preserve the identities. There are
// no multiplies here, so FMA contraction is not a concern either.
#pragma once
#include <cmath>

struct dd_sum {
  double hi = 0.0;
  double lo = 0.0;

  void add(double x) {
    // TwoSum(hi, x) -> (s, e) with s + e == hi + x exactly.
    const double s   = hi + x;
    const double bv  = s - hi;
    const double ea  = hi - (s - bv);
    const double eb  = x - bv;
    const double e   = ea + eb;
    // Fold the exact error into the low word, then renormalize (Fast2Sum;
    // |s| >= |lo2| holds because lo2 is far below s's last bit).
    const double lo2 = lo + e;
    const double h2  = s + lo2;
    const double t   = h2 - s;
    const double l2  = lo2 - t;
    hi = h2;
    lo = l2;
  }

  double value() const { return hi + lo; }

  // log(|sum|) with the low word folded in as a relative correction:
  // |hi + lo| = |hi| * (1 + lo/hi), so log|sum| = log|hi| + log1p(lo/hi).
  double log_abs() const { return std::log(std::fabs(hi)) + std::log1p(lo / hi); }
};

// Exact rounding error of a - b: returns e such that (a - b) + e is the exact
// difference, i.e. e = (a - b) - fl(a - b). Used to measure how much accuracy
// the argument subtraction (log_abs - m_log) loses before exp() ever runs.
inline double two_diff_err(double a, double b) {
  const double s  = a - b;
  const double bv = a - s;
  const double ea = a - (s + bv);
  const double eb = bv - b;
  return ea + eb;
}
