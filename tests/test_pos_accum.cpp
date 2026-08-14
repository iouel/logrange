// pos_accum: the positive-only fast path. Mirrors test_rp_accum.cpp's
// coverage minus cancellation (structurally impossible here), plus the
// negative-term contract violation, the add_log raw path, and a cross-check
// against rp_accum on random positive data.
#include "test_common.h"
#include <logrange/log_math.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <random>

using namespace logrange;

static inline bool approx(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (std::max)(1.0, (std::max)(std::fabs(a), std::fabs(b)));
}

// 400 orders of magnitude in one sum (rp_accum's test, positive-only).
static void test_dynamic_range_sum() {
  pos_accum acc;
  acc.add(log_value(1e-200));
  acc.add(log_value(1.0));
  acc.add(log_value(1e200));
  NC_CHECK(approx(acc.to_log_value().to_linear(), 1e200, 1e-12));
}

// Scaled adds at extreme magnitude.
static void test_scaled_add() {
  pos_accum acc;
  acc.add_scaled(log_value(2.0), 1e100);
  acc.add_scaled(log_value(3.0), 1e100);
  NC_CHECK(approx(acc.to_log_value().to_linear(), 5e100, 1e-12));
}

// The underflow-vs-linear headline case (intent success criterion 1,
// miniature). Product of 1100 terms of 0.5 underflows linear doubles hard
// (2^-1100 is below even the subnormal range, 2^-1074); accumulate
// log-domain terms and recover log-magnitude.
static void test_underflow_rescue() {
  // Build one term = 0.5^1100 in log form: log_abs = 1100*log(0.5)
  log_value term;
  term.log_abs = 1100.0 * std::log(0.5);
  term.sign = 1.0;
  // Linear arithmetic cannot hold this value:
  NC_CHECK(std::exp(term.log_abs) == 0.0);
  // The accumulator can:
  pos_accum acc;
  acc.add(term);
  acc.add(term); // sum = 2 * 0.5^1100
  log_value v = acc.to_log_value();
  NC_CHECK(!v.is_zero());
  NC_CHECK(v.sign == 1.0);
  NC_CHECK(approx(v.log_abs, 1100.0 * std::log(0.5) + std::log(2.0), 1e-12));
}

// Poison paths. Every bad input must yield NaN out, permanently. This adds
// the path rp_accum doesn't have: a negative nonzero term is a contract
// violation here, not a sign to track.
static void test_poison() {
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();
  { // NaN term
    pos_accum acc;
    acc.add(log_value(3.0));
    acc.add(log_value(NAN_));
    NC_CHECK(acc.poisoned());
    NC_CHECK(acc.to_log_value().is_nan());
    acc.add(log_value(1.0)); // sticky: later good terms don't un-poison
    NC_CHECK(acc.to_log_value().is_nan());
  }
  { // inf term
    pos_accum acc;
    acc.add(log_value(std::numeric_limits<double>::infinity()));
    NC_CHECK(acc.poisoned());
    NC_CHECK(acc.to_log_value().is_nan());
  }
  { // negative nonzero term via add(): contract violation
    pos_accum acc;
    acc.add(log_value(3.0));
    acc.add(log_value(-1.0));
    NC_CHECK(acc.poisoned());
    NC_CHECK(acc.to_log_value().is_nan());
  }
  { // negative nonzero term via add_scaled(): same contract
    pos_accum acc;
    acc.add_scaled(log_value(-2.0), 1.0);
    NC_CHECK(acc.poisoned());
  }
  { // NaN via add_log raw path
    pos_accum acc;
    acc.add_log(NAN_);
    NC_CHECK(acc.poisoned());
  }
  { // +inf via add_log raw path
    pos_accum acc;
    acc.add_log(std::numeric_limits<double>::infinity());
    NC_CHECK(acc.poisoned());
    acc.add_log(0.0); // sticky
    NC_CHECK(acc.to_log_value().is_nan());
  }
  { // bad scale: NaN
    pos_accum acc;
    acc.add_scaled(log_value(2.0), NAN_);
    NC_CHECK(acc.poisoned());
  }
  { // bad scale: negative
    pos_accum acc;
    acc.add_scaled(log_value(2.0), -1.0);
    NC_CHECK(acc.poisoned());
  }
  { // bad scale: zero (contract says c > 0)
    pos_accum acc;
    acc.add_scaled(log_value(2.0), 0.0);
    NC_CHECK(acc.poisoned());
  }
}

// add_log is the same accumulation as add(log_value) minus validation:
// identical inputs must produce bit-identical state and reduction.
static void test_add_log_agreement() {
  const double terms[] = { std::log(3.0), -450.0, 460.55, 0.0, -1.5, 230.2 };
  pos_accum via_value, via_raw;
  for (double la : terms) {
    log_value v;
    v.log_abs = la;
    v.sign = 1.0;
    via_value.add(v);
    via_raw.add_log(la);
  }
  NC_CHECK(via_value.m_log == via_raw.m_log);
  NC_CHECK(via_value.sum == via_raw.sum);
  NC_CHECK(via_value.to_log_value().log_abs == via_raw.to_log_value().log_abs);
}

// Empty/zero contract: empty reduces to zero; zero terms (either sign of
// zero, and -inf via the raw path) are no-ops and leave the accumulator
// empty. Negative zero is zero, not a contract violation.
static void test_empty_and_zero() {
  { // Empty accumulator reduces to zero.
    pos_accum acc;
    NC_CHECK(acc.to_log_value().is_zero());
  }
  { // Adding zeros is a no-op, stays empty.
    pos_accum acc;
    acc.add(log_value(0.0));
    acc.add(log_value(-0.0));
    acc.add_log(-std::numeric_limits<double>::infinity());
    NC_CHECK(acc.empty());
    NC_CHECK(!acc.poisoned());
    NC_CHECK(acc.to_log_value().is_zero());
  }
  { // clear() returns a used accumulator to empty.
    pos_accum acc;
    acc.add(log_value(2.0));
    acc.clear();
    NC_CHECK(acc.empty());
    NC_CHECK(acc.to_log_value().is_zero());
  }
}

// On positive-only data the fast path and the signed path compute the same
// sum: same reference-exponent scheme, same uncompensated linear
// accumulation. A few hundred random terms across ~430 orders of magnitude,
// fixed seed.
static void test_agrees_with_rp_accum() {
  std::mt19937_64 rng(0xC0FFEEULL);
  std::uniform_real_distribution<double> log_dist(-500.0, 500.0);
  pos_accum fast;
  rp_accum general;
  for (int i = 0; i < 300; ++i) {
    const double la = log_dist(rng);
    fast.add_log(la);
    log_value v;
    v.log_abs = la;
    v.sign = 1.0;
    general.add(v);
  }
  const log_value a = fast.to_log_value();
  const log_value b = general.to_log_value();
  NC_CHECK(!a.is_zero() && !b.is_zero());
  NC_CHECK(a.sign == 1.0 && b.sign == 1.0);
  NC_CHECK(approx(a.log_abs, b.log_abs, 1e-12));
}

int main() {
  test_dynamic_range_sum();
  test_scaled_add();
  test_underflow_rescue();
  test_poison();
  test_add_log_agreement();
  test_empty_and_zero();
  test_agrees_with_rp_accum();
  std::puts("test_pos_accum passed");
  return 0;
}
