// test_rescale_compensation.cpp
//
// Diagnostic for the unresolved rp_accum rescale question.
//
// Hypothesis under test:
//   A rescale currently does
//
//       pos   *= scale;
//       pos_c *= scale;
//
//   and the stated error accounting charges only one multiplication error
//   for the standing positive accumulator. Because pos_c contains the
//   information recovered by Neumaier compensation, independently scaling it
//   may introduce an additional first-order error that is not represented by
//   the current budget.
//
// This test deliberately:
//   1. creates a nonzero Neumaier compensation term;
//   2. performs exactly one upward-reference rescale;
//   3. measures the state immediately after that rescale;
//   4. compares the measured state against a double-double reference;
//
// It then searches a family of inputs and reports the worst rescale error
// in units of u.
//
// This is a DIAGNOSTIC, not a proof of the bound. In particular, it does not
// assert that the current implementation is wrong. It is intended to answer:
//
//   "Does a live compensation term make the one-rescale error materially
//    larger than the one-u multiplication budget?"
//
// The test uses the repository's dd_sum reference, as test_accuracy.cpp and
// bound_search.cpp do.

#include "test_common.h"
#include "dd_sum.h"
#include <logrange/log_math.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <limits>
#include <vector>

using namespace logrange;

static constexpr double U = 0x1p-53;

// ---------------------------------------------------------------------------
// High-precision shadow of the POSITIVE scaled state.
//
// We only care about the state represented by
//
//     pos + pos_c
//
// immediately before and after the rescale.
//
// dd_sum gives us a ~106-bit reference for that quantity.
//
// ---------------------------------------------------------------------------

struct shadow {
  dd_sum exact;
  double m_log = -std::numeric_limits<double>::infinity();

  void add_exact(double log_abs) {
    exact.add(std::exp(log_abs));
  }

  double exact_scaled(double new_m) const {
    // The exact positive linear sum at the old reference is
    //
//      sum(exp(L_i - m_log))
    //
// so after changing the reference to new_m it is
    //
//      sum(exp(L_i - new_m)).
    //
// Calculate that directly in long double inside dd_sum's representable
    // range; all cases below stay near ordinary magnitudes.
    dd_sum scaled;

    const double old_m = m_log;
    const double factor = old_m - new_m;

    // We do not have the original terms here, so this helper is not used for
    // arbitrary state reconstruction. The caller constructs the exact scaled
    // reference directly from its term list.
    (void)scaled;
    (void)factor;

    return 0.0;
  }
};

// ---------------------------------------------------------------------------
// One test case.
//
// Terms before the jump are all positive and deliberately chosen to create
// Neumaier compensation. The final term is larger and therefore causes
// exactly one rescale.
//
// We keep the input terms so the exact state at the new reference can be
// evaluated independently.
// ---------------------------------------------------------------------------

struct case_result {
  int n_small = 0;
  double J = 0.0;

  double pos_before = 0.0;
  double comp_before = 0.0;
  double exact_before = 0.0;

  double pos_after = 0.0;
  double comp_after = 0.0;
  double exact_after = 0.0;

  double state_error = 0.0;
  double state_error_u = 0.0;
  double ratio_to_1u = 0.0;

  double final_error = 0.0;
  double final_error_u = 0.0;
};

// Evaluate a deliberately compensated prefix, then one rescale.
//
// The prefix values are chosen as nearby floating-point numbers around a
// common magnitude. Repeating exactly identical values often produces little
// or no compensation, so we use a deterministic alternating pattern whose
// additions have known low-word loss.
static case_result run_case(int n_small, double small_log, double J) {
  case_result out;
  out.n_small = n_small;
  out.J = J;

  rp_accum acc;

  // Preserve the exact linear values represented by the log_values.
  std::vector<double> terms;
  terms.reserve(static_cast<std::size_t>(n_small) + 1);

  // A small deterministic perturbation pattern. The values remain at the
  // same log scale but are not identical, making a live compensation term
  // much easier to obtain.
  //
  // Start from exp(small_log), then use adjacent representable values.
  const double base = std::exp(small_log);

  for (int i = 0; i < n_small; ++i) {
    double x;

    switch (i & 3) {
      case 0:
        x = base;
        break;
      case 1:
        x = std::nextafter(base, std::numeric_limits<double>::infinity());
        break;
      case 2:
        x = base;
        break;
      default:
        x = std::nextafter(base, 0.0);
        break;
    }

    log_value v(x);
    acc.add(v);
    terms.push_back(v.log_abs);
  }

  // We specifically want a live Neumaier low word before the rescale.
  out.pos_before = acc.pos;
  out.comp_before = acc.pos_c;

  // The exact scaled mass at the OLD reference.
  dd_sum old_scaled;
  for (double L : terms)
    old_scaled.add(std::exp(L));

  out.exact_before = old_scaled.value();

  // Force exactly one upward reference change.
  const double dominant_log = small_log + J;

  log_value dominant;
  dominant.sign = 1.0;
  dominant.log_abs = dominant_log;

  acc.add(dominant);

  // Exact value represented at the NEW reference.
  //
  // Each prefix term becomes exp(L - dominant_log), and the dominant term
  // contributes exactly 1.
  dd_sum new_scaled;
  for (double L : terms)
    new_scaled.add(std::exp(L - dominant_log));
  new_scaled.add(1.0);

  out.exact_after = new_scaled.value();

  out.pos_after = acc.pos;
  out.comp_after = acc.pos_c;

  const double got_state = acc.pos + acc.pos_c;
  out.state_error =
      std::fabs(got_state - out.exact_after) /
      std::fabs(out.exact_after);

  out.state_error_u = out.state_error / U;
  out.ratio_to_1u = out.state_error_u;

  // Also measure the eventual linear result. This includes anything that
  // happens after the rescale, so it is secondary evidence only.
  const double got_final = acc.to_log_value().to_linear();

  // Exact total represented by the same input terms in ordinary linear space.
  dd_sum exact_final;
  for (double L : terms)
    exact_final.add(std::exp(L));
  exact_final.add(std::exp(dominant_log));

  const double truth_final = exact_final.value();

  out.final_error =
      std::fabs(got_final - truth_final) /
      std::fabs(truth_final);

  out.final_error_u = out.final_error / U;

  return out;
}

// ---------------------------------------------------------------------------
// Search.
//
// We care especially about:
//   - a live compensation term before the rescale;
//   - one rescale only;
//   - J around the unresolved J=2 case;
//   - enough prefix terms to build a substantial low word.
//
// ---------------------------------------------------------------------------

static void run_search() {
  const int sizes[] = {
      8, 16, 32, 64, 128, 256, 512, 1024,
      2048, 4096, 8192, 16384
  };

  double worst_u = 0.0;
  double worst_final_u = 0.0;
  case_result worst{};
  case_result worst_final{};

  std::printf(
      "rp_accum compensated-rescale diagnostic\n"
      "----------------------------------------\n"
      "u = %.17g\n\n",
      U);

  std::printf(
      "  %-8s %-8s %-14s %-14s %-12s %-12s %-12s\n",
      "N", "J", "comp_before", "state_err/u",
      "final_err/u", "state_ratio", "live_comp");

  for (int n : sizes) {
    for (int j = 0; j <= 24; ++j) {
      const double J = 1.0 + 0.125 * static_cast<double>(j);

      const case_result r =
          run_case(n, 10.5 - J, J);

      const bool live = r.comp_before != 0.0;

      std::printf(
          "  %-8d %-8.3f % .6e %12.4f %12.4f %12.4f %12s\n",
          r.n_small,
          r.J,
          r.comp_before,
          r.state_error_u,
          r.final_error_u,
          r.ratio_to_1u,
          live ? "yes" : "no");

      if (live && r.state_error_u > worst_u) {
        worst_u = r.state_error_u;
        worst = r;
      }

      if (live && r.final_error_u > worst_final_u) {
        worst_final_u = r.final_error_u;
        worst_final = r;
      }
    }
  }

  std::printf("\nWorst immediate post-rescale state:\n");
  std::printf(
      "  N               = %d\n"
      "  J               = %.17g\n"
      "  comp_before     = %.17g\n"
      "  state error/u   = %.9f\n"
      "  final error/u   = %.9f\n",
      worst.n_small,
      worst.J,
      worst.comp_before,
      worst.state_error_u,
      worst.final_error_u);

  std::printf("\nWorst eventual result:\n");
  std::printf(
      "  N               = %d\n"
      "  J               = %.17g\n"
      "  comp_before     = %.17g\n"
      "  state error/u   = %.9f\n"
      "  final error/u   = %.9f\n",
      worst_final.n_small,
      worst_final.J,
      worst_final.comp_before,
      worst_final.state_error_u,
      worst_final.final_error_u);

  // This test is diagnostic. Do not turn "interesting" observations into a
  // correctness assertion. The only hard assertions are that the experiment
  // actually exercised a live compensation state and produced finite values.
  NC_CHECK(worst_u > 0.0);
  NC_CHECK(worst.comp_before != 0.0);
  NC_CHECK(std::isfinite(worst.state_error));
  NC_CHECK(std::isfinite(worst.final_error));
}

int main() {
  run_search();
  return 0;
}
