// log_math.h — LogRange runtime, v0.3.0
// Signed log-domain values and accumulation.
//
// Inherited from NativeConv; refactored per LogRange intent v0.3, Deliverable 1:
//   step 1: logsumexp2 edge semantics fixed (NaN propagates, +inf propagates,
//           -inf acts as log-zero identity). Previously NaN was swallowed and
//           +inf silently absorbed.
//   step 2: rp_accum pos==neg reset documented as an explicit error-bound
//           decision (see comment at the reset site).
//   step 3: predecessor baggage stripped — pinch helpers, approximation
//           toggles, polynomial fast paths, instrumentation counters.
//
// Representation: a real x is carried as {sign, log_abs} where
//   sign    ∈ {+1.0, -1.0}
//   log_abs = log(|x|);  -inf encodes x == 0;  +inf encodes |x| == inf.
//
// Precision: double only, by design. A scope decision, not an omission.
// Every constant in the error contract is double-specific — u = 2^-53, the
// ~745 log-unit vanishing window at the subnormal floor 2^-1074, and the
// mass-weighted depth term D that the same window caps. A float variant is
// therefore not a typedef: it needs the bound re-derived at u = 2^-24 with a
// ~103 log-unit window (2^-149, seven orders of magnitude coarser and a
// rescue range shrunk to a seventh), plus an accuracy reference finer than
// the double-double one the tests use. The compiler tooling already agrees:
// the rewrite pass declines float accumulators. Callers holding float data
// should widen at the accumulator boundary — the accumulation cost dominates
// the conversion.
//
// Known, documented limitations (accepted; bounds stated at the accumulators):
//   - Terms more than ~745 log-units below the accumulator's reference
//     exponent scale to 0.0 and contribute nothing. Correct for sums whose
//     result is dominated by the largest terms; stated here so it is a
//     contract, not a surprise.
//   - rp_accum's pos/neg partial sums are Neumaier-compensated (see the
//     struct comment for why and for the measured effect); pos_accum's
//     single sum is deliberately uncompensated — it is the speed path, and
//     positive-only sums have no cancellation to amplify its O(n·eps) error.
//     Worst-case bounds for both are stated at the accumulators below and
//     machine-checked in tests/test_accuracy.cpp.

#pragma once

// Fast-math is not survivable here, so it is refused rather than warned about.
// rp_accum's compensation recovers each addition's rounding error through
// algebraic identities ((sum - t) + x). Reassociating math folds those to
// zero and the accumulator silently degrades to an uncompensated sum.
// Measured on a 40000-term cancellation set: log-magnitude
// -7.36251563240462303 built normally, -7.36251072122148731 under
// -ffast-math — 4.9e-6 relative, nine orders past the stated contract.
//
// FMA contraction (-ffp-contract=fast) is a DIFFERENT flag and is harmless
// here: the compensation path holds no multiply-add pair to fuse, and results
// are bit-identical with contraction on. No flag is imposed on callers for it.
//
// Define LOGRANGE_ALLOW_FAST_MATH to proceed anyway; the error contract in
// this header then does not describe your build.
#if (defined(__FAST_MATH__) || defined(_M_FP_FAST)) && \
    !defined(LOGRANGE_ALLOW_FAST_MATH)
#error "LogRange: fast-math destroys rp_accum's error compensation (measured 4.9e-6 relative). Compile this translation unit without -ffast-math / /fp:fast, or define LOGRANGE_ALLOW_FAST_MATH to accept an uncompensated result."
#endif

// Version identity, so a vendored copy can be recognized in the wild.
// This header is the single source of truth: CMakeLists.txt parses these
// three macros rather than carrying its own copy of the number.
//   LOGRANGE_VERSION is ordered and comparable: MAJOR*10000 + MINOR*100 + PATCH.
//   #if LOGRANGE_VERSION >= 300   // 0.3.0 or newer
#define LOGRANGE_VERSION_MAJOR 0
#define LOGRANGE_VERSION_MINOR 3
#define LOGRANGE_VERSION_PATCH 0
#define LOGRANGE_VERSION \
  (LOGRANGE_VERSION_MAJOR * 10000 + LOGRANGE_VERSION_MINOR * 100 + \
   LOGRANGE_VERSION_PATCH)
#define LOGRANGE_VERSION_STRING "0.3.0"

#include <cmath>
#include <algorithm>
#include <limits>

namespace logrange {

namespace detail {
  constexpr double NEG_INF = -std::numeric_limits<double>::infinity();
  constexpr double POS_INF =  std::numeric_limits<double>::infinity();
  constexpr double QNAN    =  std::numeric_limits<double>::quiet_NaN();
} // namespace detail

// ---------------------------------------------------------------------------
// log_value — a real number in signed log representation.
//
// PRECISION FLOOR, and it grows with magnitude. log_abs is a double, so it
// sits on a grid of spacing ulp(log_abs). Since absolute error in log space
// is relative error in linear space, a value can be represented no better
// than
//
//   relative precision  ~  |log|x|| * u
//
// however it was computed. Near 1.0 that is invisible. At |log|x|| ~ 700 —
// the range this library exists to reach — it is ~512u ~ 5.7e-14, about 13
// significant decimal digits rather than 16. Measured, not asserted:
// bound_search reports 512u for a log_value(x).to_linear() round trip at
// |log|x|| ~ 540.
//
// This floor is a property of the REPRESENTATION, not of any one operation.
// Every reduction ending in `m_log + log(...)` inherits it, which is why the
// same term appears in both accumulator contracts below. It is the reason
// those two contracts were wrong until 2026-08-15: the floor was real and
// undocumented, so neither bound accounted for it.
//
// Callers who need more than 13 digits at extreme magnitudes need a wider
// log_abs, not a different accumulator.
// ---------------------------------------------------------------------------
struct log_value {
  double log_abs = detail::NEG_INF; // log(|x|); -inf == zero
  double sign    = 1.0;             // +1.0 or -1.0

  // Default: zero.
  log_value() = default;

  // Construct from a linear value. NaN input produces a NaN log_value.
  explicit log_value(double linear_val) {
    if (std::isnan(linear_val)) {
      log_abs = detail::QNAN;
      return;
    }
    sign = std::signbit(linear_val) ? -1.0 : 1.0;
    if (linear_val == 0.0) return;            // log_abs stays -inf
    if (std::isinf(linear_val)) {
      log_abs = detail::POS_INF;
      return;
    }
    log_abs = std::log(std::fabs(linear_val));
  }

  bool is_zero() const { return log_abs == detail::NEG_INF; }
  bool is_nan()  const { return std::isnan(log_abs); }
  bool is_inf()  const { return log_abs == detail::POS_INF; }

  // Convert back to linear. Overflows to ±inf when log_abs exceeds
  // log(DBL_MAX); underflows toward ±0 below log(DBL_MIN) — both are the
  // IEEE-consistent outcomes of exp().
  double to_linear() const {
    if (is_nan())  return detail::QNAN;
    if (is_zero()) return sign * 0.0;   // preserve signed zero
    return sign * std::exp(log_abs);
  }
};

// ---------------------------------------------------------------------------
// Multiplication / division — an add / a subtract in the log domain.
//
// The MAPPING is exact: multiplication is addition of logarithms, with no
// series and no cancellation. The ARITHMETIC is not. `a.log_abs + b.log_abs`
// is a floating-point add and rounds like any other:
//
//   error(log_abs)  <=  u * |log|a| + log|b||   (absolute, in log space)
//                    =  the representation floor for the result
//
// This header called these operations "exact" until 2026-08-15. They are
// exact only when the sum of the two log magnitudes is itself representable.
// bound_search measures 1024u of rounding on a product whose log-magnitude
// is 1024 — a value well outside double's linear range but perfectly
// ordinary as a log_value, which is the point of the type.
//
// No error accumulates beyond that single rounding: there is no cancellation
// path here, so a chain of n multiplications costs n roundings at the floor,
// not an amplified sum.
//
// NaN in either operand propagates via IEEE arithmetic on log_abs.
// Note: inf * zero and zero / zero yield log_abs = inf + (-inf) = NaN,
// matching IEEE linear semantics.
// ---------------------------------------------------------------------------
inline log_value log_mul(const log_value& a, const log_value& b) {
  log_value r;
  r.sign    = a.sign * b.sign;
  r.log_abs = a.log_abs + b.log_abs;
  return r;
}

inline log_value log_div(const log_value& a, const log_value& b) {
  log_value r;
  r.sign    = a.sign * b.sign;
  r.log_abs = a.log_abs - b.log_abs;
  return r;
}

// ---------------------------------------------------------------------------
// logsumexp2 — log(exp(a) + exp(b)) for scalar log-magnitudes.
//
// Accuracy: the returned log-magnitude carries
//
//   error  <=  u * |result|  +  (d + 3) * u,   d = |a - b|
//
// absolute in log space. The first term is the representation floor on the
// final `a + log1p(...)` add and dominates at large magnitudes (measured:
// 512u near |result| ~ 700). The second is the working arithmetic — the
// argument subtraction rounds to u*d, exp turns that into a relative error
// of the same size, and log1p contributes its own ulp. Both terms are
// checked in bound_search.
//
// Why the defect that refuted both accumulator contracts does not reach here.
// That defect needs two things at once: an addend computed with its own
// relative error, and that addend's magnitude unbounded relative to the
// result. `m_log + log|net|` had both, so charging only u*|log|S|| missed
// u*|log|net||. This add has the same shape, `a + log1p(...)`, and the first
// property, but not the second: the terms are ordered so the exp argument is
// <= 0, so `log1p(exp(-d))` lies in (0, log 2] and its rounding is at most
// 0.693u. The (d + 3)*u covers it with room to spare.
//
// `log_mul` and `log_div` are immune for a different reason: both their
// addends are supplied by the caller, so neither is computed here and neither
// carries an error to propagate. The one place in this header that still has
// both properties is `add_scaled`, which forms `v.log_abs + std::log(c)`;
// see TODO.md.
//
// Edge semantics (intent v0.3 requirement — NaN out for NaN in, no silent
// absorption of infinities):
//   NaN, x     -> NaN        (poison propagates)
//   +inf, x    -> +inf       (an infinite term dominates any sum)
//   -inf, x    -> x          (-inf is log(0); zero is the additive identity)
//   -inf, -inf -> -inf       (0 + 0 == 0)
//   +inf, +inf -> +inf
// ---------------------------------------------------------------------------
inline double logsumexp2(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return detail::QNAN;
  if (a == detail::POS_INF || b == detail::POS_INF) return detail::POS_INF;
  if (a == detail::NEG_INF) return b;
  if (b == detail::NEG_INF) return a;
  // Both finite. Order so the exp argument is <= 0 (no overflow possible).
  if (b > a) std::swap(a, b);
  return a + std::log1p(std::exp(b - a));
}

// ---------------------------------------------------------------------------
// log_add — signed addition: a + b in log representation.
//
// Same-sign terms combine via logsumexp2. Opposite-sign terms combine via
// log(|exp(la) - exp(lb)|) with the dominant magnitude's sign; exact
// cancellation at representation precision yields zero.
//
// Edge semantics:
//   NaN operand          -> NaN result
//   zero operand         -> other operand (additive identity)
//   inf + inf, same sign -> inf with that sign
//   inf + (-inf)         -> NaN  (matches IEEE linear semantics)
//   inf + finite         -> inf with inf's sign
// ---------------------------------------------------------------------------
inline log_value log_add(const log_value& a, const log_value& b) {
  // NaN poisons.
  if (a.is_nan() || b.is_nan()) { log_value r; r.log_abs = detail::QNAN; return r; }
  // Zero is the additive identity.
  if (a.is_zero()) return b;
  if (b.is_zero()) return a;

  // Infinities.
  if (a.is_inf() || b.is_inf()) {
    if (a.is_inf() && b.is_inf() && a.sign != b.sign) {
      log_value r; r.log_abs = detail::QNAN; return r;  // inf - inf
    }
    return a.is_inf() ? a : b;
  }

  log_value r;
  if (a.sign == b.sign) {
    r.sign    = a.sign;
    r.log_abs = logsumexp2(a.log_abs, b.log_abs);
    return r;
  }

  // Opposite signs: log(|exp(la) - exp(lb)|), sign of the larger magnitude.
  if (a.log_abs > b.log_abs) {
    r.sign    = a.sign;
    r.log_abs = a.log_abs + std::log1p(-std::exp(b.log_abs - a.log_abs));
  } else if (b.log_abs > a.log_abs) {
    r.sign    = b.sign;
    r.log_abs = b.log_abs + std::log1p(-std::exp(a.log_abs - b.log_abs));
  } else {
    // Equal magnitudes, opposite signs: exact zero.
    r.log_abs = detail::NEG_INF;
    r.sign    = 1.0;
    return r;
  }
  // log1p(-exp(d)) can produce -inf when d rounds to 0 (magnitudes equal at
  // double precision) — that is a legitimate zero, already encoded. It cannot
  // produce NaN for d < 0, so no scrub is needed here.
  return r;
}

// ---------------------------------------------------------------------------
// rp_accum — reference-exponent accumulator for signed log-domain sums.
//
// Design: maintain a reference log-magnitude m_log equal to the largest
// term's log_abs seen so far, and accumulate each term as a *linear* ratio
// exp(term.log_abs - m_log) into separate positive and negative partial
// sums. Cost is one exp() per term; the final log() is paid once at
// reduction. This differs from the textbook streaming logsumexp (one
// exp + one log1p per term) and keeps positive/negative mass separated,
// which makes cancellation observable rather than silent.
//
// Error contract (v0.2, formal). Definitions:
//   u    = unit roundoff = 2^-53 ~ 1.11e-16
//   cond = sum|x_i| / |sum x_i|  (condition number of the summation)
//   k    = rescale events: adds that strictly raise m_log after the first
//          term. Worst case n-1 (sorted ascending input); expected O(ln n)
//          for randomly ordered input.
//   rho  = pos==neg reset events; A_j = largest |term| before reset j.
//   D    = mass-weighted mean insertion depth
//          = sum_i |x_i| * (m_i - L_i) / sum_i |x_i|, where L_i is term i's
//          log_abs and m_i the reference when it was added (so m_i >= L_i).
//          "Depth" is a gap in LOG SPACE — how far below the running
//          reference the term sat when its exp() argument was formed — not a
//          position in any traversal. That is why it is capped by the
//          vanishing window: a term ~745 below the reference scales to 0.0
//          and leaves the sum. Bounded by ~ln(n) for ordinary data.
//   S    = the exact sum; log|S| is the result's own log-magnitude.
//   net  = S / exp(m_log), the scaled sum the reduction takes the log of.
//          |log|net|| <= log n for positive sums, and <= log(n*cond) in
//          general. This is the OTHER addend of m_log + log|net|, and it does
//          not vanish when that addition cancels.
// Assuming std::exp within 1 ulp (true of MSVC/glibc/libm current), and
// n < ~1e7 so O(n*u^2) terms are negligible:
//
//   WORST-CASE RELATIVE ERROR  <=  cond * (3k + 4 + D) * u
//                                  +  (|log|S|| + |log|net||) * u
//     + terms > ~745 log-units below the running m_log vanish entirely.
//
// This is a FIRST-ORDER bound. It holds under the assumptions stated above —
// std::exp within 1 ulp, the vanishing window, and O(n*u^2) and higher terms
// neglected — not as an unconditional inequality over all floating-point
// behavior. Those assumptions are listed so they can be checked, not buried.
//
// The reset loss is NOT a separate unbudgeted term. The per-reset discard of
// up to A_j (documented at the reset site) is already inside the relative
// budget above, and the epochs are what make that work: a reset fires only
// when pos == neg, and the reference term contributes a scaled ratio of 1 to
// one of them, so both are >= 1 and that epoch's mass is at least 2*A_j.
// clear() starts a fresh epoch, so epochs are disjoint and
//     sum_j A_j  <=  (1/2) sum_j mass_j  <=  (1/2) sum_i |x_i|
//                 =  (1/2) * cond * |S|,
// giving a relative reset contribution of at most cond*u/2 — inside the 4u
// coefficient already present, with a factor of 8 to spare, for ANY number of
// resets. Disjointness is the load-bearing step: without it the sum over
// resets would appear to grow without bound.
//
// Derivation. Each term's scaled ratio r_i = exp(L_i - m_i) carries 2u from
// exp() itself (1 ulp, per the assumption above) PLUS the rounding of its
// own argument: fl(L_i - m_i) differs from the exact difference by up to
// u*(m_i - L_i) = u*d_i, and exp turns that absolute argument error into a
// relative error of the same size. So a term costs (d_i + 2)*u, not the flat
// 2u this derivation first claimed. The 2u is unchanged and still sits in
// the +4 below; D is the d_i*u that was missing. Terms sharing one depth
// share one rounding error, coherently, with no cancellation between them;
// weighting by mass share gives the D term.
// Each rescale event multiplies the standing sums by a factor carrying
// <= 2u (exp) + u (multiply); Neumaier compensation makes summation itself
// contribute <= u regardless of length. That is the (3k + 4) part, on a mass
// of sum|x_i|, amplified by cond at the final subtraction.
// The final reduction adds a term that never touches cond. out.log_abs =
// m_log + log|net|, and absolute error in log space IS relative error in
// linear space, so BOTH addends' roundings land on the result:
//   - log|net| is computed to a relative u, hence an absolute u*|log|net||;
//   - the addition itself rounds to u*|log|S||.
// For a sum near 1 the second is invisible; at log|S| ~ 700, the regime this
// library exists for, it is ~700u on its own and dominates everything else.
//
// *The first was missing until 2026-08-16 and is why the previous form was
// refuted.* Charging only u*|log|S|| budgets the rounding of the addition's
// RESULT while ignoring the magnitude of its INPUTS, which is the same defect
// as charging 2u for a term's exp() while ignoring its argument. The two
// addends cancel exactly when the sum is near 1, and there |log|S|| goes to
// zero while |log|net|| does not. bound_search's family E is the witness: n
// equal positive terms at L = -log(n) give cond = 1, k = 0, D = 0 and
// |log|S|| = 0, so the old form budgets 4u, while |log|net|| = log n. At
// n = 166463 the measured error is 7.97u against that 4u, a ratio of 1.99.
//
// These two are a division of labor, not rival explanations of the same
// error. Cancellation in forming net = (pos - neg) + (pos_c - neg_c) is
// covered by the cond factor: cond is large exactly when net is small
// relative to the mass, and it multiplies every coefficient perturbation
// that reached net. The |log|S||*u term covers something else entirely —
// the single rounding of m_log + log|net| once net is already known, which
// is a property of writing the answer down in a double and is there even at
// cond == 1. One is error in computing the value; the other is error in
// representing it.
// Without compensation the summation term is O(n*u) and dominates — measured
// at ~300x worse on cond=2.3e9 data (BENCHMARKS.md; a log_add fold is NOT a
// fix: its apparent accuracy there was an ordering artifact that collapses
// under shuffling).
//
// Two questions this contract carried as open, now closed by inspection:
//   - pos/neg rescale errors are bounded independently and added. That is
//     conservative under ANY correlation between them: the result is
//     (pos - neg), and |e_pos - e_neg| <= |e_pos| + |e_neg| by the triangle
//     inequality. Correlation can only shrink the realized error; it cannot
//     push it past the sum of the separate bounds. No change needed.
//   - the O(n*u^2) small print. At n = 1e7 that term is ~1.2e-25 against
//     u = 1.1e-16 — nine orders down, and it does not reach u until n ~ 1e16
//     terms, which is not an addressable count. The caveat is real but far
//     looser than "n < ~1e7" suggests.
// Status: REFUTED TWICE, corrected twice. The 2026-08-15 form
//   cond*(3k+4+D)*u + |log|S||*u was refuted 2026-08-16 at 1.99x by family E
//   above, once bound_search's reference was rebuilt to resolve it: the old
//   reference summed double exp() and collapsed to a double, floors of ~1u
//   and u/2, and a 1.99x violation of a 4u budget was inside them. The
//   reference is now double-double throughout (tests/dd_exp.h, ~1e-14 u) and
//   the missing |log|net||*u is stated above. Worst observed/bound across the
//   search is 0.83 against the corrected form.
//   The earlier cond*(3k+4)*u form was REFUTED by tests/bound_search.cpp,
//   which found 151 of 400 random inputs violating THAT superseded form
//   (worst 15.8x) plus a constructed counterexample at 5.8x with cond == 1,
//   k == 0. The same sweep scores every input against both forms; the form
//   above was exceeded zero times out of 400, worst observed/bound 0.85
//   (0.82 before the sweep widened to +/-600 magnitudes and mixed signs, so
//   the wider attack did press it harder). It is still an
//   author's derivation, now with an adversarial search standing behind it
//   rather than six fixed scenarios; independent review is still open
//   (TODO.md). Pre-1.0 this contract can still move, and CHANGELOG.md
//   records old and new values when it does.
//   - Terms below m_log - ~745 vanish (exp underflow). See header comment.
//
// Edge behavior:
//   - Adding zero is a no-op.
//   - Adding NaN or ±inf log_abs poisons the accumulator: every subsequent
//     to_log_value() returns NaN. (A +inf term cannot be meaningfully
//     scaled against finite terms; sign information for inf-inf cases is
//     not tracked, so poison is the honest answer. Revisit if a use case
//     needs inf-dominant semantics.)
// ---------------------------------------------------------------------------
struct rp_accum {
  double m_log = detail::NEG_INF; // reference log-magnitude
  double pos   = 0.0;             // sum of scaled positive terms
  double pos_c = 0.0;             // Neumaier compensation for pos
  double neg   = 0.0;             // sum of scaled negative terms
  double neg_c = 0.0;             // Neumaier compensation for neg

  void clear() { m_log = detail::NEG_INF; pos = pos_c = neg = neg_c = 0.0; }

  bool empty()    const { return m_log == detail::NEG_INF; }
  bool poisoned() const { return std::isnan(pos); }

  void add(const log_value& v) {
    if (poisoned()) return;
    if (v.is_nan() || v.is_inf()) { poison(); return; }
    if (v.is_zero()) return;

    if (empty()) {
      m_log = v.log_abs;
      (v.sign >= 0.0 ? pos : neg) = 1.0;
      return;
    }
    if (v.log_abs > m_log) {
      // New dominant term: rescale existing sums down to the new reference.
      // The compensation terms are linear in the sums, so they scale too.
      const double scale = std::exp(m_log - v.log_abs);
      pos *= scale; pos_c *= scale;
      neg *= scale; neg_c *= scale;
      m_log = v.log_abs;
    }
    const double r = std::exp(v.log_abs - m_log);
    if (v.sign >= 0.0) kb_add(pos, pos_c, r); else kb_add(neg, neg_c, r);

    // --- DOCUMENTED ERROR-BOUND DECISION (intent v0.3, seed defect 2) ----
    // When pos == neg at double precision, the true residual is not zero —
    // it is merely below this accumulator's resolution (< eps relative to
    // the scaled sums, i.e. < |largest term| * eps in linear terms). We
    // deliberately reset to exact zero anyway, for one reason: it re-arms
    // the reference exponent, so a subsequent tiny term (e.g. 1.0 after
    // ±1e100 cancel) is captured at full precision instead of vanishing
    // against a stale m_log ~ 230.
    //
    // The cost: any true residual of the cancelled prefix, of magnitude
    // up to |largest term| * eps, is discarded. This bounds the absolute
    // error of the final sum by max_i|term_i| * eps per reset event.
    // Callers for whom that residual matters need compensated partial
    // sums (future work); callers summing terms that genuinely cancel
    // (the common case this path serves) get strictly better behavior.
    // ---------------------------------------------------------------------
    // Compensated values compared: "equal at this accumulator's resolution"
    // must account for the low words, or the reset would fire on sums the
    // compensation can still tell apart.
    if (pos + pos_c == neg + neg_c) clear();
  }

  // SCALING COST, and it is not covered by the contract above. This forms
  // log(c) and adds it, so the term enters carrying the rounding of log(c):
  //
  //   absolute error in log space  <=  ulp(|log c|)/2  <=  |log c| * u
  //
  // which is a relative error of the same size on that term. Measured, swept
  // over arbitrary c (bound_search family F): 8u at |log c| ~ 10, 64u at 100,
  // 256u at 400, 512u at 690, landing exactly on ulp(|log c|)/2 each time.
  // Against a single-term budget of 4u that is 128x at the top of the range.
  //
  // It is stated here rather than folded into the accumulator's bound because
  // it is the cost of ENTERING log space at that scale, not of the
  // accumulation: a caller writing add_log(v.log_abs + std::log(c)) by hand
  // pays exactly the same. The contract above covers what the accumulator
  // does with the terms it is given.
  //
  // The representation floor at log_value does NOT cover this. That floor is
  // |log|x||*u for the value being represented; here the scaled term's own
  // magnitude can be ~0 while the injected error is 512u, because it is set
  // by an INPUT's magnitude, not the result's. That is the same distinction
  // that made |log|net|| necessary in the reduction.
  //
  // If c * |x| is representable in linear, form it and use add() instead:
  // measured 1.9u independent of scale, against 512u here. Callers whose
  // product overflows or underflows have no cheaper route, and this is the
  // price.
  //
  // Add c * v for a linear scalar c > 0. (c <= 0 or NaN c poisons —
  // silently ignoring a bad scale would violate NaN-in/NaN-out.)
  void add_scaled(const log_value& v, double c) {
    if (poisoned()) return;
    if (std::isnan(c) || !(c > 0.0)) { poison(); return; }
    if (v.is_nan() || v.is_inf())    { poison(); return; }
    if (v.is_zero()) return;
    log_value scaled = v;
    scaled.log_abs = v.log_abs + std::log(c);
    add(scaled);
  }

  // Reduce to a single log_value. NaN if poisoned.
  log_value to_log_value() const {
    log_value out;
    if (poisoned()) { out.log_abs = detail::QNAN; return out; }
    if (empty())    return out;                  // zero
    // High words first, then the compensation difference — the low words are
    // where the cancellation accuracy lives.
    const double net = (pos - neg) + (pos_c - neg_c);
    if (net == 0.0) return out;                  // zero (see reset note)
    out.sign    = (net > 0.0) ? 1.0 : -1.0;
    out.log_abs = m_log + std::log(std::fabs(net));
    return out;
  }

private:
  // Neumaier update: sum += x with the rounding error captured in comp.
  // Statement-per-step so the compensation cannot be fused away; /fp:precise
  // (or -ffp-contract=off) preserves the identities.
  static void kb_add(double& sum, double& comp, double x) {
    const double t = sum + x;
    if (std::fabs(sum) >= std::fabs(x)) {
      const double lost = (sum - t) + x;
      comp += lost;
    } else {
      const double lost = (x - t) + sum;
      comp += lost;
    }
    sum = t;
  }

  void poison() { m_log = detail::POS_INF; pos = detail::QNAN; neg = detail::QNAN; }
};

// ---------------------------------------------------------------------------
// pos_accum — reference-exponent accumulator for positive-only log-domain
// sums (intent Deliverable 1: the positive-only fast path).
//
// Same design as rp_accum — reference log-magnitude m_log tracking the
// largest term, one exp() per term into a linear scaled sum, one log() at
// reduction — minus everything sign-related. Relative to rp_accum this drops:
//   - sign tracking (one branch and one store per add),
//   - the separate neg partial sum (cancellation cannot occur, so there is
//     no cancellation visibility to preserve),
//   - the pos == neg cancellation reset and its per-event error decision.
// That is the entire source of the speedup; the numerics are otherwise
// identical.
//
// Contract: terms are nonnegative. A negative-signed nonzero term is a
// contract violation and poisons — silently absorbing it (or folding it in)
// would hide a bug at the call site. Zero terms (either sign of zero) are
// the additive identity and are no-ops.
//
// Error contract (formal; u, k, D and S as defined at rp_accum):
//
//   WORST-CASE RELATIVE ERROR  <=  (n + 3k + 3 + D) * u
//                                  +  (|log|S|| + |log|net||) * u
//
// First-order, under the same stated assumptions as rp_accum's (1-ulp exp,
// the vanishing window, higher-order terms neglected).
//
// The n*u term is the uncompensated running sum — kept deliberately: this
// is the speed path, positive terms cannot cancel, so cond == 1 and there
// is no amplification for compensation to suppress. Typical randomly-signed
// rounding lands near sqrt(n)*u. Callers needing epsilon-level accuracy on
// very long positive sums should use rp_accum (compensated) and pay the
// ~1.5x per-term cost.
//
// The reduction terms are the same ones rp_accum carries: out.log_abs =
// m_log + log(sum), where log(sum) rounds at its own magnitude |log|net||
// and the addition rounds at |log|S||. Absolute error in log space is
// relative error in linear space. There is no cond here for either to hide
// behind, and neither grows with n — so at small n and large magnitude they
// are the entire error. Four terms near e^690 budget (n+3k+3)*u = 7u under
// the old form against ~500u of real error.
//
// |log|net||*u is carried for correctness, not because it binds here:
// |log|net|| <= log n for positive sums, and the n*u term already dominates
// log n. It is what refuted rp_accum, which is Neumaier-compensated and so
// has no n*u term to absorb it. Family E measures this rather than assuming
// it: pos_accum reaches 0.80 of its bound where rp_accum reaches 1.99 of the
// uncorrected one.
//
// D is carried for symmetry with rp_accum and is NOT the binding term here.
// Making D large takes ~e^D terms at depth D, so D ~ ln(n) < n and the n*u
// term already covers it. Measured rather than assumed: bound_search's P2
// family drives depth clusters at this accumulator and never exceeds 0.22 of
// even the old bound.
//
// Status: the earlier (n+3k+3)*u form was REFUTED by tests/bound_search.cpp —
//   119 of 400 random inputs exceed it, worst 34.9x, and every violation is
//   the missing reduction term. The form above holds across that search at
//   worst 0.79 at the time, 0.80 re-measured 2026-08-16 against the
//   double-double reference that replaced the plain-double one. Note this
//   bound had never been machine-checked before
//   2026-08-15: test_pos_accum asserted behavior, not the contract. Both
//   now check it.
//   - Terms below m_log - ~745 vanish (exp underflow). See header comment.
//
// Edge behavior (mirrors rp_accum):
//   - Adding zero (log_abs == -inf) is a no-op.
//   - Adding NaN or +inf log_abs poisons the accumulator: sticky, queryable
//     via poisoned(), every subsequent to_log_value() returns NaN.
//
// add_log(log_abs) is the raw fast path for callers who never materialize a
// log_value; add(log_value) validates sign and forwards to it. Both are part
// of the interface.
// ---------------------------------------------------------------------------
struct pos_accum {
  double m_log = detail::NEG_INF; // reference log-magnitude
  double sum   = 0.0;             // sum of scaled terms

  void clear() { m_log = detail::NEG_INF; sum = 0.0; }

  bool empty()    const { return m_log == detail::NEG_INF; }
  bool poisoned() const { return std::isnan(sum); }

  // Raw fast path: add a term given directly as log|x|.
  //   NaN or +inf poisons; -inf (zero) is a no-op.
  void add_log(double log_abs) {
    if (poisoned()) return;
    if (std::isnan(log_abs) || log_abs == detail::POS_INF) { poison(); return; }
    if (log_abs == detail::NEG_INF) return;

    if (empty()) {
      m_log = log_abs;
      sum   = 1.0;
      return;
    }
    if (log_abs > m_log) {
      // New dominant term: rescale the existing sum down to the new reference.
      sum *= std::exp(m_log - log_abs);
      m_log = log_abs;
    }
    sum += std::exp(log_abs - m_log);
  }

  // Validated path: negative-signed nonzero terms poison (contract
  // violation — see struct comment); zero is a no-op regardless of sign.
  void add(const log_value& v) {
    if (poisoned()) return;
    if (v.is_nan() || v.is_inf()) { poison(); return; }
    if (v.is_zero()) return;
    if (v.sign < 0.0) { poison(); return; }
    add_log(v.log_abs);
  }

  // Scaling cost: log(c) rounds, so the term enters carrying up to
  // ulp(|log c|)/2 <= |log c|*u, relative, and the accumulator's contract
  // does not cover it. Stated in full at rp_accum::add_scaled.
  //
  // Add c * v for a linear scalar c > 0. (c <= 0 or NaN c poisons —
  // silently ignoring a bad scale would violate NaN-in/NaN-out.)
  void add_scaled(const log_value& v, double c) {
    if (poisoned()) return;
    if (std::isnan(c) || !(c > 0.0)) { poison(); return; }
    if (v.is_nan() || v.is_inf())    { poison(); return; }
    if (v.is_zero()) return;
    if (v.sign < 0.0) { poison(); return; }
    add_log(v.log_abs + std::log(c));
  }

  // Reduce to a single log_value. NaN if poisoned; zero if empty; the
  // result's sign is always +1.
  log_value to_log_value() const {
    log_value out;
    if (poisoned()) { out.log_abs = detail::QNAN; return out; }
    if (empty())    return out;                  // zero
    out.sign    = 1.0;
    out.log_abs = m_log + std::log(sum);
    return out;
  }

private:
  void poison() { m_log = detail::POS_INF; sum = detail::QNAN; }
};

} // namespace logrange