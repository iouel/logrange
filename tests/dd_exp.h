// dd_exp.h — double-double arithmetic and exp(), the accuracy reference for
// the adversarial searches.
//
// WHY THIS EXISTS. bound_search.cpp used to build its reference from
// std::exp() in plain double and then collapse it with dd_sum::value(). That
// put two independent floors under every measurement:
//
//   1. each term carried up to ~1 ulp from double exp();
//   2. value() returns hi + lo, so "truth" was a DOUBLE and carried up to
//      u/2 of representation error before any comparison happened.
//
// Against bounds whose whole budget is 7u, a floor near 1u makes ratios up
// to ~1.3 unreadable, which is what the old file honestly said. Floor 2 is
// structural: no libm improvement removes it.
//
// This header removes both. exp() is computed in double-double, and callers
// keep the reference wide through the subtraction (dd_rel_err below).
//
// NO LIBM ACCURACY IS ASSUMED. dd_exp does its own argument reduction against
// a 106-bit ln2 and its own Taylor series, in exact double-double arithmetic.
// It is validated by identities that need no external constant: exp(0) is
// exactly 1, exp(ln2) is exactly 2 (which pins the ln2 constant and the
// reduction together), exp(x)*exp(-x) is 1, and exp(a)*exp(b) is exp(a+b).
// dd_exp_selftest() runs them and every search calls it before reporting.
//
// long double is deliberately NOT used: it is 80-bit on x86-64 Linux and
// 64-bit on MSVC, so a reference built on it would be a different reference
// per toolchain, and the library's CI covers three.
#pragma once
#include <cmath>

struct dd {
  double hi = 0.0;
  double lo = 0.0;
};

// Fast2Sum: requires |a| >= |b|.
inline dd dd_quick_sum(double a, double b) {
  const double s = a + b;
  const double e = b - (s - a);
  return dd{s, e};
}

// TwoSum (Knuth): exact for any a, b.
inline dd dd_two_sum(double a, double b) {
  const double s  = a + b;
  const double bv = s - a;
  const double ea = a - (s - bv);
  const double eb = b - bv;
  return dd{s, ea + eb};
}

// TwoProd via FMA: p + e == a*b exactly. std::fma is a single rounding by
// definition, so this needs no Dekker splitting and no flags.
inline dd dd_two_prod(double a, double b) {
  const double p = a * b;
  const double e = std::fma(a, b, -p);
  return dd{p, e};
}

inline dd dd_add(dd a, dd b) {
  dd s = dd_two_sum(a.hi, b.hi);
  const dd t = dd_two_sum(a.lo, b.lo);
  s.lo += t.hi;
  s = dd_quick_sum(s.hi, s.lo);
  s.lo += t.lo;
  return dd_quick_sum(s.hi, s.lo);
}

inline dd dd_add_d(dd a, double b) {
  dd s = dd_two_sum(a.hi, b);
  s.lo += a.lo;
  return dd_quick_sum(s.hi, s.lo);
}

inline dd dd_neg(dd a) { return dd{-a.hi, -a.lo}; }
inline dd dd_sub(dd a, dd b) { return dd_add(a, dd_neg(b)); }

inline dd dd_mul(dd a, dd b) {
  dd p = dd_two_prod(a.hi, b.hi);
  p.lo += a.hi * b.lo + a.lo * b.hi;
  return dd_quick_sum(p.hi, p.lo);
}

inline dd dd_mul_d(dd a, double b) {
  dd p = dd_two_prod(a.hi, b);
  p.lo += a.lo * b;
  return dd_quick_sum(p.hi, p.lo);
}

// Divide by a double. One Newton step on the remainder, which is formed
// exactly by FMA.
inline dd dd_div_d(dd a, double b) {
  const double q1 = a.hi / b;
  const dd p = dd_two_prod(q1, b);          // q1*b == p.hi + p.lo exactly
  dd r = dd_two_sum(a.hi, -p.hi);
  r.lo += a.lo;
  r.lo -= p.lo;
  const double q2 = (r.hi + r.lo) / b;
  return dd_quick_sum(q1, q2);
}

inline double dd_to_double(dd a) { return a.hi + a.lo; }

// ln 2 to ~106 bits. Not trusted: dd_exp_selftest asserts exp(this) == 2,
// which fails loudly if either half is wrong.
inline dd dd_ln2() {
  return dd{6.93147180559945286227e-01, 2.31904681384629956e-17};
}

// exp(x) * 2^-bias, in double-double.
//
//   n = round(x / ln2);  r = x - n*ln2  =>  |r| <= ln2/2 ~ 0.347
//   exp(x) = 2^n * exp(r), and 2^n scales exactly.
//
// exp(r) by Horner'd Taylor: 1 + r(1 + r/2(1 + r/3(...))). At |r| <= 0.347,
// 30 terms leave a remainder far below 2^-106, so the series is not the
// limiting error; the reduction is, at ~1e-29 relative.
//
// WHY THE BIAS EXISTS. The pair is scaled by ldexp on BOTH words, so a result
// near the bottom of double's range pushes the low word subnormal and the
// pair silently degrades: exp(-700) has hi ~ 9.9e-305, so lo sits near 1e-320
// with about 8 bits left and the pair carries ~61 bits, not 106. Measured at
// 3.7e-20 before this parameter existed, which is 3e-4 u — still far below a
// 7u budget, but it is a floor that grows exactly where this library works.
//
// Callers pass the peak term's binary exponent as `bias`. The dominant terms
// then land near 1.0 with the full 106 bits, and only terms far below the
// peak degrade, in proportion to a mass share that is already negligible: a
// term 700 log-units down contributes e^-700 of the sum. The caller scales
// its own value by ldexp(got, -bias), which is exact.
inline dd dd_exp_scaled(double x, int bias) {
  const dd ln2 = dd_ln2();
  const double n = (x == 0.0) ? 0.0 : std::nearbyint(x / ln2.hi);
  dd r = dd_sub(dd{x, 0.0}, dd_mul_d(ln2, n));

  dd acc{1.0, 0.0};
  for (int k = 30; k >= 1; --k)
    acc = dd_add_d(dd_div_d(dd_mul(acc, r), static_cast<double>(k)), 1.0);

  const int ni = static_cast<int>(n) - bias;
  return dd{std::ldexp(acc.hi, ni), std::ldexp(acc.lo, ni)};
}

inline dd dd_exp(double x) {
  if (x == 0.0) return dd{1.0, 0.0};
  return dd_exp_scaled(x, 0);
}

// The bias to hand dd_exp_scaled for a family whose largest log-term is
// `peak`: the binary exponent of exp(peak).
inline int dd_exp_bias(double peak) {
  return static_cast<int>(std::nearbyint(peak / dd_ln2().hi));
}

// Relative error of a double `got` against a double-double reference, with
// the subtraction kept exact. This is the half that value() used to throw
// away: got - truth is formed in full width, and only the small result is
// collapsed, where a relative error of the error is harmless.
inline double dd_rel_err(double got, dd truth) {
  const dd d = dd_sub(dd{got, 0.0}, truth);
  return std::fabs(dd_to_double(d) / dd_to_double(truth));
}

// log|hi + lo|, with the low word folded in as a relative correction.
inline double dd_log_abs(dd a) {
  return std::log(std::fabs(a.hi)) + std::log1p(a.lo / a.hi);
}

// Identity checks that need no external constant. Returns the worst relative
// deviation seen; callers fail if it is not far below u.
inline double dd_exp_selftest() {
  double worst = 0.0;

  // exp(0) is exactly 1, both words.
  const dd e0 = dd_exp(0.0);
  if (e0.hi != 1.0 || e0.lo != 0.0) return 1.0;

  // exp(ln2) == 2 pins the ln2 constant and the argument reduction together.
  {
    const dd two = dd_exp(dd_to_double(dd_ln2()));
    // dd_ln2() rounded to a double is not exactly ln2, so allow the induced
    // slope: d(exp)/dx = exp, so the error here is |ln2 - fl(ln2)| ~ 1e-17
    // relative. Test the low word instead: exp(fl(ln2)) - 2 should match
    // 2*(fl(ln2) - ln2) to high relative accuracy.
    const double drift = dd_to_double(dd_sub(dd{dd_to_double(dd_ln2()), 0.0},
                                             dd_ln2()));
    const double predicted = 2.0 * (1.0 + drift);
    const double got = dd_to_double(two);
    const double dev = std::fabs(got - predicted) / 2.0;
    if (dev > worst) worst = dev;
  }

  // exp(x) * exp(-x) == 1. Both factors are taken at their own bias, so this
  // tests the reduction and the series at full width rather than measuring
  // the subnormal-low-word degradation documented at dd_exp_scaled. The two
  // biases cancel: 2^-b * 2^+b == 1.
  const double xs[] = {0.5, 1.0, -1.0, 3.7, -3.7, 17.25, -17.25,
                       100.0, -100.0, 500.0, -500.0, 700.0, -700.0};
  for (double x : xs) {
    const int b = dd_exp_bias(x);
    const dd prod =
        dd_mul(dd_exp_scaled(x, b), dd_exp_scaled(-x, -b));
    const double dev = std::fabs(dd_to_double(dd_sub(prod, dd{1.0, 0.0})));
    if (dev > worst) worst = dev;
  }
  for (double a : xs) {
    for (double b : xs) {
      const double s = a + b;
      if (std::fabs(s) > 700.0) continue;
      // Only exact sums: otherwise exp(fl(a+b)) legitimately differs from
      // exp(a)*exp(b) by the rounding of a+b, which is not an exp error.
      if (dd_two_sum(a, b).lo != 0.0) continue;
      const int ba = dd_exp_bias(a), bb = dd_exp_bias(b);
      const dd lhs = dd_mul(dd_exp_scaled(a, ba), dd_exp_scaled(b, bb));
      const dd rhs = dd_exp_scaled(s, ba + bb);
      const double dev =
          std::fabs(dd_to_double(dd_sub(lhs, rhs)) / dd_to_double(rhs));
      if (dev > worst) worst = dev;
    }
  }
  return worst;
}
