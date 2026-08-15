// The README "Use" snippet, verbatim, wrapped in a main() that checks the
// answer. Compiled against the INSTALLED package, so this fails if the
// install rules, the exported target, or the README drift apart.
//
// The data is the case the library exists for: 1000 terms of e^-800. Each
// one underflows a double on its own (the limit is ~e^-745), so a linear
// accumulation returns exactly 0.0. The correct answer is
// log(1000 * e^-800) = -800 + log(1000) = -793.0922034432...
#include <logrange/log_math.h>

#include <cmath>
#include <cstdio>
#include <vector>

int main() {
  std::vector<double> log_terms(1000, -800.0);

  logrange::pos_accum acc;
  for (double log_term : log_terms) acc.add_log(log_term);
  logrange::log_value total = acc.to_log_value();

  const double expect = -800.0 + std::log(1000.0);
  const double err    = std::fabs(total.log_abs - expect);

  std::printf("log total   = %.12f\n", total.log_abs);
  std::printf("expected    = %.12f\n", expect);
  std::printf("linear loop = %g (underflowed)\n", 1000.0 * std::exp(-800.0));
  std::printf("version     = %s\n", LOGRANGE_VERSION_STRING);

  // Loose: this checks that the package works, not the error bound. The
  // bound has its own suites (test_accuracy, bound_search).
  if (!(err < 1e-12)) {
    std::printf("FAIL: log_abs off by %g\n", err);
    return 1;
  }
  std::puts("quickstart passed");
  return 0;
}
