// Old coverage kept; new edge-case coverage added for logsumexp2 and log_add
// (the hole that let defect 1 survive).
#include "test_common.h"
#include <logrange/log_math.h>
#include <cmath>
#include <limits>
#include <algorithm>

using namespace logrange;

static const double INF  = std::numeric_limits<double>::infinity();
static const double NINF = -std::numeric_limits<double>::infinity();

static inline bool approx_equal(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps * (std::max)(1.0, (std::max)(std::fabs(a), std::fabs(b)));
}

// ---------------------------------------------------------------- log_value
static void test_log_value_struct() {
  { // default zero
    log_value v;
    NC_CHECK(v.is_zero());
    NC_CHECK(v.sign == 1.0);
    NC_CHECK(v.to_linear() == 0.0);
  }
  { // explicit zero
    log_value v(0.0);
    NC_CHECK(v.is_zero());
    NC_CHECK(v.to_linear() == 0.0);
  }
  { // negative zero: sign preserved through round-trip
    log_value v(-0.0);
    NC_CHECK(v.is_zero());
    NC_CHECK(v.sign == -1.0);
    NC_CHECK(std::signbit(v.to_linear()));
  }
  { // positive value
    log_value v(10.0);
    NC_CHECK(approx_equal(v.log_abs, std::log(10.0)));
    NC_CHECK(v.sign == 1.0);
    NC_CHECK(approx_equal(v.to_linear(), 10.0));
  }
  { // negative value
    log_value v(-10.0);
    NC_CHECK(approx_equal(v.log_abs, std::log(10.0)));
    NC_CHECK(v.sign == -1.0);
    NC_CHECK(approx_equal(v.to_linear(), -10.0));
  }
  { // +inf
    log_value v(INF);
    NC_CHECK(v.is_inf());
    NC_CHECK(v.sign == 1.0);
    NC_CHECK(v.to_linear() == INF);
  }
  { // -inf
    log_value v(NINF);
    NC_CHECK(v.is_inf());
    NC_CHECK(v.sign == -1.0);
    NC_CHECK(v.to_linear() == NINF);
  }
  { // NaN in -> NaN carried -> NaN out
    log_value v(std::numeric_limits<double>::quiet_NaN());
    NC_CHECK(v.is_nan());
    NC_CHECK(std::isnan(v.to_linear()));
  }
}

// ---------------------------------------------------- logsumexp2 edge table
// Written directly against the contract comment in log_math.h.
static void test_logsumexp2_edges() {
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();

  // NaN poisons, either slot
  NC_CHECK(std::isnan(logsumexp2(NAN_, 3.0)));
  NC_CHECK(std::isnan(logsumexp2(3.0, NAN_)));
  NC_CHECK(std::isnan(logsumexp2(NAN_, NAN_)));
  NC_CHECK(std::isnan(logsumexp2(NAN_, INF)));   // NaN beats inf
  NC_CHECK(std::isnan(logsumexp2(NAN_, NINF)));

  // +inf dominates (this is the old absorption bug, inverted)
  NC_CHECK(logsumexp2(INF, 3.0)  == INF);
  NC_CHECK(logsumexp2(3.0, INF)  == INF);
  NC_CHECK(logsumexp2(INF, INF)  == INF);
  NC_CHECK(logsumexp2(INF, NINF) == INF);        // inf + 0 = inf

  // -inf is log(0), the additive identity
  NC_CHECK(logsumexp2(NINF, 3.0)  == 3.0);
  NC_CHECK(logsumexp2(3.0, NINF)  == 3.0);
  NC_CHECK(logsumexp2(NINF, NINF) == NINF);      // 0 + 0 = 0

  // Finite correctness: log(e^a + e^b)
  NC_CHECK(approx_equal(logsumexp2(std::log(2.0), std::log(3.0)), std::log(5.0)));
  NC_CHECK(approx_equal(logsumexp2(0.0, 0.0), std::log(2.0)));
  // Order independence
  NC_CHECK(logsumexp2(1.0, 5.0) == logsumexp2(5.0, 1.0));
  // Extreme spread: dominant term wins without overflow
  NC_CHECK(approx_equal(logsumexp2(700.0, -700.0), 700.0));
}

// ------------------------------------------------------------- log_mul/div
static void test_log_mul_div() {
  log_value a(2.0), b(-3.0);
  log_value c = log_mul(a, b);
  NC_CHECK(approx_equal(c.to_linear(), -6.0));
  log_value d = log_div(c, b); // (-6)/(-3) = 2
  NC_CHECK(approx_equal(d.to_linear(), 2.0));

  // inf * 0 = NaN (IEEE)
  NC_CHECK(log_mul(log_value(INF), log_value(0.0)).is_nan());
  // 0 / 0 = NaN
  NC_CHECK(log_div(log_value(0.0), log_value(0.0)).is_nan());
  // NaN propagates
  NC_CHECK(log_mul(log_value(std::numeric_limits<double>::quiet_NaN()), a).is_nan());
}

// ------------------------------------------------------------------ log_add
static void test_log_add() {
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();

  // Original sign-combination coverage (kept from seed tests)
  NC_CHECK(approx_equal(log_add(log_value(2.0),  log_value(3.0)).to_linear(),  5.0));
  NC_CHECK(approx_equal(log_add(log_value(-2.0), log_value(-3.0)).to_linear(), -5.0));
  NC_CHECK(approx_equal(log_add(log_value(5.0),  log_value(-3.0)).to_linear(),  2.0));
  NC_CHECK(approx_equal(log_add(log_value(3.0),  log_value(-5.0)).to_linear(), -2.0));
  NC_CHECK(log_add(log_value(5.0), log_value(-5.0)).to_linear() == 0.0);

  // Zero is the additive identity
  NC_CHECK(approx_equal(log_add(log_value(0.0), log_value(7.0)).to_linear(), 7.0));
  NC_CHECK(approx_equal(log_add(log_value(7.0), log_value(0.0)).to_linear(), 7.0));

  // NaN poisons
  NC_CHECK(log_add(log_value(NAN_), log_value(3.0)).is_nan());
  NC_CHECK(log_add(log_value(3.0),  log_value(NAN_)).is_nan());

  // Infinity semantics (new contract)
  NC_CHECK(log_add(log_value(INF),  log_value(3.0)).to_linear()  == INF);
  NC_CHECK(log_add(log_value(NINF), log_value(3.0)).to_linear()  == NINF);
  NC_CHECK(log_add(log_value(INF),  log_value(INF)).to_linear()  == INF);
  NC_CHECK(log_add(log_value(NINF), log_value(NINF)).to_linear() == NINF);
  NC_CHECK(log_add(log_value(INF),  log_value(NINF)).is_nan());  // inf - inf

  // Near-cancellation accuracy: (1+1e-10) - 1 = 1e-10 to relative precision
  {
    log_value big(1.0 + 1e-10), neg(-1.0);
    log_value r = log_add(big, neg);
    NC_CHECK(r.sign == 1.0);
    NC_CHECK(approx_equal(r.to_linear(), 1e-10, 1e-5)); // relative, cancellation-limited
  }
}

int main() {
  test_log_value_struct();
  test_logsumexp2_edges();
  test_log_mul_div();
  test_log_add();
  std::puts("test_log_math passed");
  return 0;
}
