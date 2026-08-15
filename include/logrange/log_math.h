// log_math.h — LogRange runtime, v0.2.0
// Signed log-domain values and accumulation.
//
// Inherited from NativeConv; refactored per LogRange intent v0.3, First Action:
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

// Version identity, so a vendored copy can be recognized in the wild.
// This header is the single source of truth: CMakeLists.txt parses these
// three macros rather than carrying its own copy of the number.
//   LOGRANGE_VERSION is ordered and comparable: MAJOR*10000 + MINOR*100 + PATCH.
//   #if LOGRANGE_VERSION >= 200   // 0.2.0 or newer
#define LOGRANGE_VERSION_MAJOR 0
#define LOGRANGE_VERSION_MINOR 2
#define LOGRANGE_VERSION_PATCH 0
#define LOGRANGE_VERSION \
  (LOGRANGE_VERSION_MAJOR * 10000 + LOGRANGE_VERSION_MINOR * 100 + \
   LOGRANGE_VERSION_PATCH)
#define LOGRANGE_VERSION_STRING "0.2.0"

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
// Multiplication / division — exact in log domain (an add / a subtract).
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
// Assuming std::exp within 1 ulp (true of MSVC/glibc/libm current), and
// n < ~1e7 so O(n*u^2) terms are negligible:
//
//   WORST-CASE RELATIVE ERROR  <=  cond * (3k + 4 + D) * u  +  |log|S|| * u
//     + (absolute) sum_j A_j * u          discarded by resets (see below)
//     + terms > ~745 log-units below the running m_log vanish entirely.
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
// The final reduction adds a term that never touches cond: out.log_abs =
// m_log + log|net| rounds to within u*|log|S|| in log space, and absolute
// error in log space IS relative error in linear space. For a sum near 1
// this is invisible; at log|S| ~ 700, the regime this library exists for,
// it is ~700u on its own and dominates everything else.
// Without compensation the summation term is O(n*u) and dominates — measured
// at ~300x worse on cond=2.3e9 data (BENCHMARKS.md; a log_add fold is NOT a
// fix: its apparent accuracy there was an ordering artifact that collapses
// under shuffling).
// Status: the earlier cond*(3k+4)*u form was REFUTED by tests/bound_search.cpp,
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
// Error contract (v0.2, formal; u and k as defined at rp_accum):
//
//   WORST-CASE RELATIVE ERROR  <=  (n + 3k + 3) * u
//
// The n*u term is the uncompensated running sum — kept deliberately: this
// is the speed path, positive terms cannot cancel, so cond == 1 and there
// is no amplification for compensation to suppress. Typical randomly-signed
// rounding lands near sqrt(n)*u. Callers needing epsilon-level accuracy on
// very long positive sums should use rp_accum (compensated) and pay the
// ~1.5x per-term cost.
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