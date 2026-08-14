// Seed's three tests kept verbatim in spirit; poison-path and reset-contract
// coverage added.
#include "test_common.h"
#include <logrange/log_math.h>
#include <cmath>
#include <limits>
#include <algorithm>

using namespace logrange;

static inline bool approx(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (std::max)(1.0, (std::max)(std::fabs(a), std::fabs(b)));
}

// Kept from seed: cancellation reset re-arms the reference exponent.
static void test_near_cancellation() {
  rp_accum acc;
  acc.add(log_value(1e100));
  acc.add(log_value(-1e100));
  NC_CHECK(acc.to_log_value().to_linear() == 0.0);
  // The reset's entire justification: this 1.0 must survive at full precision.
  acc.add(log_value(1.0));
  NC_CHECK(approx(acc.to_log_value().to_linear(), 1.0, 1e-15));
}

// Kept from seed: 400 orders of magnitude in one sum.
static void test_dynamic_range_sum() {
  rp_accum acc;
  acc.add(log_value(1e-200));
  acc.add(log_value(1.0));
  acc.add(log_value(1e200));
  NC_CHECK(approx(acc.to_log_value().to_linear(), 1e200, 1e-12));
}

// Kept from seed: scaled adds at extreme magnitude.
static void test_scaled_add() {
  rp_accum acc;
  acc.add_scaled(log_value(2.0), 1e100);
  acc.add_scaled(log_value(3.0), 1e100);
  NC_CHECK(approx(acc.to_log_value().to_linear(), 5e100, 1e-12));
}

// New: the underflow-vs-linear headline case (intent success criterion 1,
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
  rp_accum acc;
  acc.add(term);
  acc.add(term); // sum = 2 * 0.5^1100
  log_value v = acc.to_log_value();
  NC_CHECK(!v.is_zero());
  NC_CHECK(approx(v.log_abs, 1100.0 * std::log(0.5) + std::log(2.0), 1e-12));
}

// New: poison paths. Every bad input must yield NaN out, permanently.
static void test_poison() {
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();
  { // NaN term
    rp_accum acc;
    acc.add(log_value(3.0));
    acc.add(log_value(NAN_));
    NC_CHECK(acc.poisoned());
    NC_CHECK(acc.to_log_value().is_nan());
    acc.add(log_value(1.0)); // sticky: later good terms don't un-poison
    NC_CHECK(acc.to_log_value().is_nan());
  }
  { // inf term
    rp_accum acc;
    acc.add(log_value(std::numeric_limits<double>::infinity()));
    NC_CHECK(acc.poisoned());
    NC_CHECK(acc.to_log_value().is_nan());
  }
  { // bad scale: NaN
    rp_accum acc;
    acc.add_scaled(log_value(2.0), NAN_);
    NC_CHECK(acc.poisoned());
  }
  { // bad scale: negative
    rp_accum acc;
    acc.add_scaled(log_value(2.0), -1.0);
    NC_CHECK(acc.poisoned());
  }
  { // bad scale: zero (contract says c > 0)
    rp_accum acc;
    acc.add_scaled(log_value(2.0), 0.0);
    NC_CHECK(acc.poisoned());
  }
}

// New: reset error contract. The documented cost of the pos==neg reset is
// a discarded residual up to |largest| * eps. Verify the *benefit* side
// (tiny successor survives) and that a NON-exactly-cancelling pair does
// NOT trigger the reset.
static void test_reset_contract() {
  { // Non-exact cancellation must keep the residual, not reset.
    rp_accum acc;
    acc.add(log_value(1.0 + 1e-9));
    acc.add(log_value(-1.0));
    log_value v = acc.to_log_value();
    NC_CHECK(!v.is_zero());
    NC_CHECK(v.sign == 1.0);
    NC_CHECK(approx(v.to_linear(), 1e-9, 1e-6));
  }
  { // Empty accumulator reduces to zero.
    rp_accum acc;
    NC_CHECK(acc.to_log_value().is_zero());
  }
  { // Adding zeros is a no-op, stays empty.
    rp_accum acc;
    acc.add(log_value(0.0));
    acc.add(log_value(-0.0));
    NC_CHECK(acc.empty());
    NC_CHECK(acc.to_log_value().is_zero());
  }
}

int main() {
  test_near_cancellation();
  test_dynamic_range_sum();
  test_scaled_add();
  test_underflow_rescue();
  test_poison();
  test_reset_contract();
  std::puts("test_rp_accum passed");
  return 0;
}
