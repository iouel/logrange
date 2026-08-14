// log_math.h — LogRange seed runtime, v0.1
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
// Known, documented limitations (accepted for v0.1, revisit with error analysis):
//   - Terms more than ~745 log-units below the accumulator's reference
//     exponent scale to 0.0 and contribute nothing. Correct for sums whose
//     result is dominated by the largest terms; stated here so it is a
//     contract, not a surprise.
//   - rp_accum's pos/neg partial sums are plain uncompensated doubles and
//     accrue standard O(n·eps) rounding error over long accumulations.
//     A stated worst-case bound is future work (intent doc, Deliverable 1).

#pragma once
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
// Error contract (v0.1, to be formalized per intent Deliverable 1):
//   - pos/neg are uncompensated sums: O(n·eps) relative error in the
//     scaled domain, which maps to O(n·eps) absolute error in log_abs.
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
  double neg   = 0.0;             // sum of scaled negative terms

  void clear() { m_log = detail::NEG_INF; pos = 0.0; neg = 0.0; }

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
      const double scale = std::exp(m_log - v.log_abs);
      pos *= scale; neg *= scale;
      m_log = v.log_abs;
    }
    const double r = std::exp(v.log_abs - m_log);
    (v.sign >= 0.0 ? pos : neg) += r;

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
    if (pos == neg) clear();
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
    const double net = pos - neg;
    if (net == 0.0) return out;                  // zero (see reset note)
    out.sign    = (net > 0.0) ? 1.0 : -1.0;
    out.log_abs = m_log + std::log(std::fabs(net));
    return out;
  }

private:
  void poison() { m_log = detail::POS_INF; pos = detail::QNAN; neg = detail::QNAN; }
};

} // namespace logrange