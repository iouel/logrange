// rescue_shim.cpp — the rescue study's runtime half (matcher/RESCUE.md, R1).
//
// The instrument pass records, per reduction term, the LEAF values of the term
// chain plus a static postfix descriptor of the chain's shape. This file
// replays that chain and produces the four quantities the pre-registration
// names.
//
// WHY LEAVES AND A DESCRIPTOR, RATHER THAN THE TERM VALUE. Capturing the term
// cannot see the rescue case the diagnostic exists for. In
// `s += w[i] * exp(logp[i])` at logp ~ -800 the term is already 0.0 when the
// accumulator sees it, so linear, reference and log-reference would all read
// zero and the marquee site would score as no-failure. The chain is therefore
// replayed SYMBOLICALLY: log|exp(a)| is `a`, taken from a's leaves, and the
// collapsed exp result is never an input. Reconstructing log(0.0) = -inf after
// the fact reproduces the bug instead of fixing it.
//
// FOUR QUANTITIES, and two of them must not be conflated:
//
//   linear         what the program computed, recorded, not replayed
//   truth          the exact sum, streaming logsumexp in double-double
//   log-reference  the SAME computation in DOUBLE-precision log domain, an
//                  independent textbook implementation. Answers "is the log
//                  representation adequate", and must not be the runtime
//   shipped        pos_accum / rp_accum from include/logrange/log_math.h
//
// The log-reference is double precision on purpose. At double-double it would
// be exact by construction, the "improves by 100x" clause would always hold,
// and the rescue criterion would be vacuous.
//
// ---------------------------------------------------------------------------
// THE LOG-IFIABLE PREDICATE, as implemented here. Four rules are narrower than
// the first draft, each for a reason that would otherwise corrupt the ground
// truth.
//
//   exp(a)      -> a                    (a's LINEAR value, from its leaves)
//   exp2(a)     -> a * ln2
//   pow(a,b)    -> b * log|a|, sign resolved from the OBSERVED base and an
//                  integrality test on the OBSERVED exponent. Never inferred
//                  from the parity of a floating exponent: for negative base
//                  and non-integral exponent the result is NaN, not a signed
//                  magnitude, and this records it as NaN.
//   fmul/fdiv   -> log|a| +/- log|b|, signs multiplied
//   fneg        -> same magnitude, sign flipped
//   fadd/fsub   -> signed sum in DOUBLE-DOUBLE LINEAR form when both operands
//                  are representable, because A ~ B with A - B tiny is exactly
//                  where log-domain arithmetic loses the quantity being
//                  established as ground truth. Only when an operand is out of
//                  double range does this fall back to a log-domain
//                  logsumexp. The sign is never chosen by comparing child
//                  magnitudes in double.
//   fpext       -> pass through
//   fptrunc     -> NOT transparent. The destination rounding is applied.
//                  `float e = exp(a)` with a ~ -100 gives a healthy double and
//                  0.0f after the narrowing; treating the node as a
//                  pass-through would claim a log term of -100 where the term
//                  the accumulator receives is zero. A narrowing that takes a
//                  finite non-zero value to zero is counted as a
//                  TRUNC-COLLAPSE, so R3 can separate "the accumulation lost
//                  it" from "a narrowing foreclosed the rescue before the
//                  accumulation". Only the first is about log-domain
//                  accumulation.
//   leaf        -> constants, arguments and loads only
//
// A read-only CALL is not a leaf. Taking log|value| of a call result
// reconstructs whatever the callee already collapsed internally, which is the
// failure this design exists to prevent. Unwhitelisted calls make the site
// UNLOGIFIABLE, decided statically in the pass; no descriptor is emitted for
// them and this file never sees one.
//
// A leaf that is genuinely zero (an unvisited move in darknet's pick_move) is
// not a collapse. It is a real zero input, log = -inf, and correct.
//
// SCOPE: single-threaded. The study drives one process at a time.

#include "dd_exp.h"
#include "dd_sum.h"
#include <logrange/log_math.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

// A value carried through the replay. The log form is always present; the
// linear form is present only while it fits in double range, and that is what
// makes exact signed cancellation available where it matters.
struct LogVal {
  dd lg{kNegInf, 0.0}; // log|value|
  int sgn = 0;         // +1, -1, or 0 for an exact zero
  bool nan = false;
  bool lin_ok = false;
  dd lin{0.0, 0.0};

  static LogVal from_double(double v) {
    LogVal r;
    if (std::isnan(v)) { r.nan = true; return r; }
    r.lin_ok = std::isfinite(v);
    r.lin = dd{v, 0.0};
    if (v == 0.0) { r.sgn = 0; r.lg = dd{kNegInf, 0.0}; return r; }
    r.sgn = (v < 0.0) ? -1 : 1;
    if (std::isinf(v)) { r.lg = dd{-kNegInf, 0.0}; return r; }
    r.lg = dd_log_refined(dd{std::fabs(v), 0.0});
    return r;
  }

  bool is_zero() const { return !nan && sgn == 0; }
};

// Materialize the linear value from the log form, when it fits.
void refresh_linear(LogVal &v) {
  if (v.nan || v.lin_ok) return;
  const double l = dd_to_double(v.lg);
  if (l > 709.0 || l < -745.0) { v.lin_ok = false; return; }
  v.lin = dd_exp(l);
  if (v.sgn < 0) v.lin = dd_neg(v.lin);
  v.lin_ok = true;
}

void set_from_linear(LogVal &v, dd x) {
  const double h = dd_to_double(x);
  if (std::isnan(h)) { v.nan = true; return; }
  v.lin = x;
  v.lin_ok = std::isfinite(h);
  if (h == 0.0) { v.sgn = 0; v.lg = dd{kNegInf, 0.0}; return; }
  v.sgn = (h < 0.0) ? -1 : 1;
  dd a = x;
  if (v.sgn < 0) a = dd_neg(a);
  v.lg = dd_log_refined(a);
}

// Signed combine for fadd/fsub. Exact in double-double when both operands are
// representable; that is the path that preserves cancellation.
LogVal combine_add(const LogVal &a, const LogVal &b, bool subtract,
                   bool &used_log_fallback) {
  LogVal r;
  if (a.nan || b.nan) { r.nan = true; return r; }
  LogVal bb = b;
  if (subtract) bb.sgn = -bb.sgn;

  if (a.lin_ok && bb.lin_ok) {
    set_from_linear(r, dd_add(a.lin, bb.lin));
    return r;
  }
  used_log_fallback = true;
  // Out of double range on at least one side: logsumexp in the log domain,
  // scaled to the larger magnitude so nothing overflows.
  if (a.is_zero()) return bb;
  if (bb.is_zero()) return a;
  const double la = dd_to_double(a.lg), lb = dd_to_double(bb.lg);
  const double m = (la > lb) ? la : lb;
  const dd ea = dd_exp_scaled(dd_to_double(dd_sub(a.lg, dd{m, 0.0})), 0);
  const dd eb = dd_exp_scaled(dd_to_double(dd_sub(bb.lg, dd{m, 0.0})), 0);
  dd s = dd_add(a.sgn < 0 ? dd_neg(ea) : ea, bb.sgn < 0 ? dd_neg(eb) : eb);
  const double sh = dd_to_double(s);
  if (sh == 0.0) { r.sgn = 0; r.lg = dd{kNegInf, 0.0}; return r; }
  r.sgn = (sh < 0.0) ? -1 : 1;
  if (r.sgn < 0) s = dd_neg(s);
  r.lg = dd_add(dd{m, 0.0}, dd_log_refined(s));
  r.lin_ok = false;
  return r;
}

// Apply destination rounding for fptrunc. Returns true if the narrowing
// collapsed a finite non-zero value to zero.
bool apply_trunc_f32(LogVal &v) {
  if (v.nan || v.is_zero()) return false;
  const double l = dd_to_double(v.lg);
  // float: max ~ 3.40e38 (log ~ 88.7), min subnormal ~ 1.4e-45 (log ~ -103.3)
  if (l > 88.72) { v.lg = dd{-kNegInf, 0.0}; v.lin_ok = false; return false; }
  if (l < -103.28) {
    v.sgn = 0;
    v.lg = dd{kNegInf, 0.0};
    v.lin_ok = true;
    v.lin = dd{0.0, 0.0};
    return true; // collapsed
  }
  refresh_linear(v);
  const float f = static_cast<float>(dd_to_double(v.lin));
  const bool collapsed = (f == 0.0f);
  set_from_linear(v, dd{static_cast<double>(f), 0.0});
  return collapsed;
}

// Streaming signed logsumexp in double-double: the truth.
struct DDLse {
  double m = kNegInf;
  dd s{0.0, 0.0};
  bool nan = false;

  void add(const LogVal &t) {
    if (nan) return;
    if (t.nan) { nan = true; return; }
    if (t.is_zero()) return;
    const double l = dd_to_double(t.lg);
    if (l > m) {
      if (m != kNegInf) {
        const double d = m - l;
        s = dd_mul(s, d < -745.0 ? dd{0.0, 0.0} : dd_exp(d));
      }
      m = l;
    }
    const double d = l - m;
    dd e = (d < -745.0) ? dd{0.0, 0.0} : dd_exp(d);
    if (t.sgn < 0) e = dd_neg(e);
    s = dd_add(s, e);
  }

  bool finite_nonzero() const {
    return !nan && m != kNegInf && dd_to_double(s) != 0.0 && std::isfinite(m);
  }
  // log|sum|, kept wide.
  dd log_abs() const {
    dd a = s;
    if (dd_to_double(a) < 0.0) a = dd_neg(a);
    return dd_add(dd{m, 0.0}, dd_log_refined(a));
  }
  int sign() const { return dd_to_double(s) < 0.0 ? -1 : 1; }
};

// The same algorithm at DOUBLE precision: the log-reference. Independent of
// pos_accum and rp_accum on purpose.
struct DoubleLse {
  double m = kNegInf, s = 0.0;
  bool nan = false;

  void add(const LogVal &t) {
    if (nan) return;
    if (t.nan) { nan = true; return; }
    if (t.is_zero()) return;
    const double l = dd_to_double(t.lg);
    if (l > m) {
      if (m != kNegInf) s *= std::exp(m - l);
      m = l;
    }
    s += (t.sgn < 0 ? -1.0 : 1.0) * std::exp(l - m);
  }
  double log_abs() const { return m + std::log(std::fabs(s)); }
};

// ---------------------------------------------------------------------------
// THE SENSITIVITY GRID (matcher/RESCUE.md, "Threshold sensitivity").
//
// The 100x rescue margin and T_default = 1e-10 are declared, not derived, and
// both decide what counts as rescue-worthy at all. RESCUE.md requires R3 to
// publish tier rates across the grid beside the registered point, and says
// that costs nothing because classification is post-processing.
//
// That is only true if the recording keeps enough to reclassify. Aggregating
// at one threshold would have made the requirement unsatisfiable without
// re-running the corpus, so every execution is scored at all nine cells as it
// happens. Still O(1) per site: nine counters, no per-execution rows.
//
// THE REGISTERED POINT IS CELL [1][1], and n_rescue is taken FROM the grid
// rather than computed beside it, so the headline and the grid cannot drift.
//
// T_default applies ONLY to sites with no host-declared tolerance. Where the
// host declares one (a GSL TEST_TOL* or gsl_sf_result.err), that value is used
// in all nine cells and the T axis moves nothing for that site. R3 reports how
// many sites fall back, so a flat grid is distinguishable from a robust one.
// ---------------------------------------------------------------------------
constexpr double kMargins[3] = {10.0, 100.0, 1000.0};
constexpr double kTDefaults[3] = {1e-8, 1e-10, 1e-12};
constexpr int kRegMargin = 1; // 100x
constexpr int kRegT = 1;      // 1e-10

struct Site {
  int id = -1;
  std::string loc;
  std::vector<std::string> ops; // postfix descriptor
  int nleaves = 0;
  int accum_bits = 64;
  double t_site = 1e-10;
  bool t_declared = false; // host-declared tolerance, or the default

  std::vector<double> leaves;

  // per execution
  DDLse truth;
  DoubleLse logref;
  logrange::rp_accum shipped;
  bool exec_nan = false;
  long terms_this_exec = 0;

  // aggregates
  long n_exec = 0, n_rescue = 0, n_range = 0, n_acc = 0;
  long n_rescue_grid[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  long n_trunc_collapse = 0, n_nan_term = 0, n_log_fallback = 0;
  double worst_err = 0.0;
  double last_term_log = 0.0; // for the pre-exp control

  void reset_exec() {
    truth = DDLse();
    logref = DoubleLse();
    shipped.clear();
    exec_nan = false;
    terms_this_exec = 0;
  }
};

std::vector<Site> g_sites;

Site *find(int id) {
  for (auto &s : g_sites)
    if (s.id == id) return &s;
  return nullptr;
}

std::vector<std::string> split_ws(const char *s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char *p = s; *p; ++p) {
    if (*p == ' ') {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur += *p;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Replay the chain. Returns the term as a LogVal.
LogVal replay(Site &S, bool &collapsed, bool &log_fallback) {
  std::vector<LogVal> st;
  for (const std::string &op : S.ops) {
    if (op[0] == 'L') {
      const int slot = std::atoi(op.c_str() + 1);
      st.push_back(LogVal::from_double(
          slot < static_cast<int>(S.leaves.size()) ? S.leaves[slot] : 0.0));
    } else if (op == "MUL" || op == "DIV") {
      LogVal b = st.back(); st.pop_back();
      LogVal a = st.back(); st.pop_back();
      LogVal r;
      if (a.nan || b.nan) { r.nan = true; }
      else if (op == "MUL" && (a.is_zero() || b.is_zero())) { r.sgn = 0; r.lg = dd{kNegInf, 0.0}; r.lin_ok = true; r.lin = dd{0.0, 0.0}; }
      else if (op == "DIV" && b.is_zero()) { r.nan = a.is_zero(); if (!r.nan) { r.sgn = a.sgn; r.lg = dd{-kNegInf, 0.0}; } }
      else {
        r.lg = (op == "MUL") ? dd_add(a.lg, b.lg) : dd_sub(a.lg, b.lg);
        r.sgn = a.sgn * b.sgn;
        r.lin_ok = false;
        refresh_linear(r);
      }
      st.push_back(r);
    } else if (op == "ADD" || op == "SUB") {
      LogVal b = st.back(); st.pop_back();
      LogVal a = st.back(); st.pop_back();
      st.push_back(combine_add(a, b, op == "SUB", log_fallback));
    } else if (op == "NEG") {
      LogVal a = st.back(); st.pop_back();
      a.sgn = -a.sgn;
      if (a.lin_ok) a.lin = dd_neg(a.lin);
      st.push_back(a);
    } else if (op == "EXP" || op == "EXP2") {
      LogVal a = st.back(); st.pop_back();
      LogVal r;
      if (a.nan) { r.nan = true; st.push_back(r); continue; }
      refresh_linear(a);
      // log|exp(a)| IS a. The exp is never evaluated, which is the whole
      // point: exp(-800) is 0.0 and would destroy the quantity.
      dd arg = a.lin_ok ? a.lin : dd{a.sgn < 0 ? kNegInf : -kNegInf, 0.0};
      if (op == "EXP2") arg = dd_mul(arg, dd_ln2());
      r.lg = arg;
      r.sgn = 1;
      r.lin_ok = false;
      refresh_linear(r);
      st.push_back(r);
    } else if (op == "POW") {
      LogVal b = st.back(); st.pop_back();
      LogVal a = st.back(); st.pop_back();
      LogVal r;
      refresh_linear(b);
      if (a.nan || b.nan || !b.lin_ok) { r.nan = true; st.push_back(r); continue; }
      const double e = dd_to_double(b.lin);
      if (a.is_zero()) {
        // pow(0, e): 0 for e > 0, inf for e < 0, 1 for e == 0.
        if (e > 0.0) { r.sgn = 0; r.lg = dd{kNegInf, 0.0}; }
        else if (e < 0.0) { r.sgn = 1; r.lg = dd{-kNegInf, 0.0}; }
        else { r.sgn = 1; r.lg = dd{0.0, 0.0}; }
      } else if (a.sgn > 0) {
        r.lg = dd_mul_d(a.lg, e);
        r.sgn = 1;
      } else {
        // Negative base: a sign exists only for an integral exponent.
        // Otherwise the real result is NaN, and that is recorded, not guessed.
        if (e != std::floor(e)) { r.nan = true; st.push_back(r); continue; }
        r.lg = dd_mul_d(a.lg, e);
        r.sgn = (std::fmod(std::fabs(e), 2.0) == 0.0) ? 1 : -1;
      }
      r.lin_ok = false;
      refresh_linear(r);
      st.push_back(r);
    } else if (op == "EXT") {
      // fpext widens: no value change.
    } else if (op == "TRUNCF") {
      LogVal a = st.back(); st.pop_back();
      if (apply_trunc_f32(a)) collapsed = true;
      st.push_back(a);
    }
  }
  if (st.empty()) return LogVal();
  return st.back();
}

} // namespace

extern "C" {

void lr_site(int id, const char *loc, const char *chain, int nleaves,
             int accum_bits, double t_site, int t_declared) {
  if (find(id)) return;
  Site s;
  s.id = id;
  s.loc = loc;
  s.ops = split_ws(chain);
  s.nleaves = nleaves;
  s.accum_bits = accum_bits;
  s.t_site = t_site;
  s.t_declared = t_declared != 0;
  s.leaves.assign(nleaves > 0 ? nleaves : 1, 0.0);
  s.reset_exec();
  g_sites.push_back(s);
}

void lr_leaf(int id, int slot, double v) {
  Site *s = find(id);
  if (!s) return;
  if (slot >= 0 && slot < static_cast<int>(s->leaves.size())) s->leaves[slot] = v;
}

// One term of the reduction. `linear_term` is what the program computed and is
// recorded, never replayed.
void lr_term(int id, double linear_term) {
  Site *s = find(id);
  if (!s) return;
  (void)linear_term;
  bool collapsed = false, fallback = false;
  LogVal t = replay(*s, collapsed, fallback);
  if (collapsed) s->n_trunc_collapse++;
  if (fallback) s->n_log_fallback++;
  if (t.nan) { s->n_nan_term++; s->exec_nan = true; }
  // NaN is reported as NaN, not as the default -inf log. A term the replay
  // refuses to sign (negative base, non-integral exponent) must be
  // distinguishable from a term that is genuinely zero.
  s->last_term_log = t.nan ? std::numeric_limits<double>::quiet_NaN()
                           : (t.is_zero() ? kNegInf : dd_to_double(t.lg));
  s->truth.add(t);
  s->logref.add(t);
  if (!t.nan && !t.is_zero()) {
    logrange::log_value lv;
    lv.log_abs = dd_to_double(t.lg);
    lv.sign = t.sgn < 0 ? -1.0 : 1.0;
    s->shipped.add(lv);
  }
  s->terms_this_exec++;
}

// End of one execution of the reduction. `linear_result` is the program's own
// accumulator value.
void lr_exec(int id, double linear_result) {
  Site *s = find(id);
  if (!s) { return; }
  if (s->terms_this_exec == 0) { s->reset_exec(); return; }
  s->n_exec++;

  const bool truth_ok = s->truth.finite_nonzero();
  if (!truth_ok) { s->reset_exec(); return; } // nothing to score against

  const dd tlg = s->truth.log_abs();
  const double tl = dd_to_double(tlg);

  bool range_fail = false;
  double err_lin = 0.0;
  if (!std::isfinite(linear_result) || linear_result == 0.0) {
    range_fail = true;
    err_lin = std::numeric_limits<double>::infinity();
  } else {
    const double d = std::log(std::fabs(linear_result)) - tl;
    const int slin = linear_result < 0.0 ? -1 : 1;
    if (d > 700.0) err_lin = std::numeric_limits<double>::infinity();
    else if (d < -700.0) err_lin = 1.0;
    else err_lin = std::fabs(slin * s->truth.sign() * std::exp(d) - 1.0);
  }

  // Absolute error in log space IS relative error in linear space.
  const double err_logref = std::fabs(s->logref.log_abs() - tl);
  const logrange::log_value sv = s->shipped.to_log_value();
  const double err_shipped = std::fabs(sv.log_abs - tl);
  (void)err_shipped;

  // Score every cell of the grid on this execution. A site with a
  // host-declared tolerance uses it in all nine, so its T axis is flat by
  // construction rather than by accident.
  for (int mi = 0; mi < 3; ++mi) {
    for (int ti = 0; ti < 3; ++ti) {
      const double T = s->t_declared ? s->t_site : kTDefaults[ti];
      const bool afail = !range_fail && err_lin > T;
      if ((range_fail || afail) && err_logref * kMargins[mi] <= err_lin)
        s->n_rescue_grid[mi][ti]++;
    }
  }

  const double T_reg = s->t_declared ? s->t_site : kTDefaults[kRegT];
  const bool acc_fail = !range_fail && err_lin > T_reg;
  if (range_fail) s->n_range++;
  if (acc_fail) s->n_acc++;

  // Taken FROM the grid, not computed beside it: the headline and the grid
  // cannot disagree.
  s->n_rescue = s->n_rescue_grid[kRegMargin][kRegT];
  if (std::isfinite(err_lin) && err_lin > s->worst_err) s->worst_err = err_lin;

  s->reset_exec();
}

void lr_report(void) {
  for (const auto &s : g_sites) {
    const double frac = s.n_exec ? static_cast<double>(s.n_rescue) / s.n_exec : 0.0;
    std::printf("INSTR,%s,exec=%ld,rescue=%ld,frac=%.4f,worst=%.3e,"
                "range=%ld,acc=%ld,trunc_collapse=%ld,nan_terms=%ld,"
                "log_fallback=%ld,t_declared=%d\n",
                s.loc.c_str(), s.n_exec, s.n_rescue, frac, s.worst_err,
                s.n_range, s.n_acc, s.n_trunc_collapse, s.n_nan_term,
                s.n_log_fallback, s.t_declared ? 1 : 0);
    // The grid, margin-major: m10 then m100 then m1000, each over
    // T = 1e-8, 1e-10, 1e-12. Cell [1][1] is the registered point and equals
    // the rescue= field above by construction.
    std::printf("INSTRGRID,%s,exec=%ld,t_declared=%d,"
                "g=%ld:%ld:%ld:%ld:%ld:%ld:%ld:%ld:%ld\n",
                s.loc.c_str(), s.n_exec, s.t_declared ? 1 : 0,
                s.n_rescue_grid[0][0], s.n_rescue_grid[0][1], s.n_rescue_grid[0][2],
                s.n_rescue_grid[1][0], s.n_rescue_grid[1][1], s.n_rescue_grid[1][2],
                s.n_rescue_grid[2][0], s.n_rescue_grid[2][1], s.n_rescue_grid[2][2]);
  }
}

// Testing hooks. The pre-exp control asserts against the VALUE, not the
// verdict: a probe that reconstructs log(0.0) returns -inf and would still
// look like a rescue in the positive control.
double lr_last_term_log(int id) {
  Site *s = find(id);
  return s ? s->last_term_log : 0.0;
}
long lr_rescue_count(int id) { Site *s = find(id); return s ? s->n_rescue : -1; }
// Grid cell, margin index x T index, both 0..2. Lets the controls assert the
// grid is LIVE rather than nine copies of one number.
long lr_rescue_grid(int id, int mi, int ti) {
  Site *s = find(id);
  if (!s || mi < 0 || mi > 2 || ti < 0 || ti > 2) return -1;
  return s->n_rescue_grid[mi][ti];
}
long lr_exec_count(int id) { Site *s = find(id); return s ? s->n_exec : -1; }
long lr_trunc_collapse(int id) { Site *s = find(id); return s ? s->n_trunc_collapse : -1; }
void lr_reset_all(void) { g_sites.clear(); }

} // extern "C"
