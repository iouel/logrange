# Changelog

Versions identify the header, which is the product. `LOGRANGE_VERSION_*` in
`include/logrange/log_math.h` is the source of truth; CMake parses it.
Ordered comparisons use `LOGRANGE_VERSION` (MAJOR·10000 + MINOR·100 + PATCH),
so `#if LOGRANGE_VERSION >= 200` means 0.2.0 or newer.

Pre-1.0: the error contract can still change between minor versions. What
will not change silently is the contract's *existence* — any bound that moves
is recorded here with its old and new values.

## Unreleased

**Changed — the error contract moved. Old and new values below.**

The 0.2.0 contract was `rel err ≤ cond·(3k+4)·u`. It was refuted, not
adjusted: `tests/bound_search.cpp` searches for counterexamples and found
151 of 400 random inputs violating that form, worst case 15.8× over. A
constructed counterexample reaches 5.8× at `cond = 1, k = 0`, where the old
bound is bare `4u`. Two terms were missing.

- **Argument rounding.** The derivation charged each term `2u` for its
  `exp()`. It ignored the rounding of `exp()`'s *argument*: `fl(L_i − m_i)`
  differs from the exact difference by up to `u·d_i` where `d_i = m_i − L_i`,
  and `exp` converts that into a relative error of the same size, so a term
  costs `(d_i + 2)·u` — the `2u` is unchanged and still sits inside the `+4`,
  and `d_i·u` is what was missing. Terms at equal depth share one rounding
  error coherently, with nothing to cancel it. Enters as `D`, the
  mass-weighted mean insertion depth, where depth is the gap in *log space*
  below the running reference (~`ln n` for ordinary data, capped by the ~745
  vanishing window since deeper terms scale to zero and leave the sum).
- **Final reduction.** `out.log_abs = m_log + log|net|` rounds to within
  `u·|log|S||`, and absolute error in log space is relative error in linear
  space. Invisible for sums near 1; at `log|S| ~ 700` — the regime this
  library exists for — it is ~700u on its own, and it never touches `cond`.
  This is what most of the random refutations were hitting.

New: `rel err ≤ cond·(3k + 4 + D)·u + |log|S||·u`. The search scores every
input against both forms; the new one was exceeded zero times out of 400,
worst observed/bound 0.85, so it is tight enough to stay falsifiable rather
than padded until nothing can reach it.

**`pos_accum` was refuted the same way, harder.** Old: `(n+3k+3)·u`. It fails
on 119 of 400 random inputs, worst **34.9×**, and every violation is the same
final-reduction term — `m_log + log(sum)` rounds to `u·|log|S||`, which does
not grow with `n` and has no `cond` to hide behind. Four terms near `e⁶⁹⁰`
budget `7u` against ~500u of real error. New:
`(n + 3k + 3 + D)·u + |log|S||·u`, worst 0.79 across the search.

`D` is carried there for symmetry but is **not** the binding term: making `D`
large takes ~`e^D` terms at depth `D`, so `D ~ ln n < n` and the `n·u` term
already covers it. Measured, not assumed — depth clusters aimed at
`pos_accum` never exceed 0.22 of even the old bound.

This bound had also never been machine-checked. `test_pos_accum` asserted
behavior only; it now asserts the contract, and that assertion was verified
to fail under the old form. `pos_accum`'s `(n+3k+3)·u` is unreviewed and unchanged.

Bounds that moved in `test_accuracy` (observed values did not change):
n=10⁶ positive sum 7.4e-15 → 1.0e-14; heavy cancellation 1.6e-5 → 1.6e-5;
shuffled 5.7e-6 → 6.4e-6.

**Decided**
- **Double only, by design.** Stated in the header's Precision block rather
  than left as an unmentioned limit. Every constant in the error contract is
  double-specific (u = 2⁻⁵³, the ~745 log-unit vanishing window, and the new
  D term that window caps), so a float variant means re-deriving the bound at
  u = 2⁻²⁴ with a ~103 log-unit window and a finer accuracy reference — not a
  typedef. The rewrite pass already declines float accumulators. Callers with
  float data widen at the accumulator boundary.

**Added**
- `tests/bound_search.cpp` (ctest: `bound_search`), the adversarial search.
  Constructed families for coherent argument rounding, cancellation, and the
  `k = n−1` worst case, plus 400 randomized trials over size, magnitude,
  depth spread, sign mix, and ordering. Scores every input against the
  contract and fails if anything exceeds it.
- `tests/dd_sum.h` — the double-double reference, factored out of
  test_accuracy so both suites agree on what "truth" means, plus
  `two_diff_err` for measuring argument-subtraction loss exactly.
- `LOGRANGE_VERSION_MAJOR` / `_MINOR` / `_PATCH`, `LOGRANGE_VERSION`,
  `LOGRANGE_VERSION_STRING`. A vendored copy can now be identified.
- CI matrix: ubuntu-gcc and ubuntu-clang alongside windows-msvc. The
  gcc/clang flag branch (`-Wall -Wextra -Werror -ffp-contract=off`) had never
  run for the library tests; it needed no code changes.
- `test_accuracy` verified on glibc. The formal bound assumes `exp()` within
  1 ulp and every published number came from msvcrt. All scenarios hold, with
  4.5×–5100× slack; the table is bit-identical across gcc 13.3, gcc 15.2,
  clang 18.1, and clang 21.1. See BENCHMARKS.md, "Second toolchain".

**Fixed**
- CMake project version was hardcoded `0.1` while the header shipped 0.2
  behavior. CMake now parses the header instead of carrying its own copy.
- Header banner said v0.1 and called the worst-case bound "future work"; the
  formal contract has been stated at the accumulators since 0.2.0.

## 0.2.0 — 2026-08-15

**Changed**
- `rp_accum` pos/neg partial sums are Neumaier-compensated. Up to 5000× more
  accurate under cancellation and insensitive to input ordering, at ~2–3
  ns/term (9.6 vs 7.3 on the uniform shape). `pos_accum` stays uncompensated:
  positive-only sums have no cancellation to amplify.

**Added**
- Formal worst-case error bound as a header contract, machine-checked in
  `test_accuracy` against a double-double reference:
  `rp_accum` ≤ cond·(3k+4)·u, `pos_accum` ≤ (n+3k+3)·u, u = 2⁻⁵³, k = rescale
  events. Observed sits 5–1000× under the bound on every scenario.
- `pos_accum`, the positive-only fast path: 6.5 ns/term at n = 10⁶ versus
  23.9 for a hand-rolled streaming logsumexp.
- Benchmark harness with a measured noise floor (~1%), pinned core, fixed
  seeds. BENCHMARKS.md carries the numbers and the cancellation
  investigation that produced the compensated accumulator.
- LLVM tooling, prototype-grade and shipped as such: matcher and risk triage
  (matcher/), the `diagnose.sh` lint, and the `log-rewrite` pass (pass/).

## 0.1.0 — 2026-08-15

Extracted from NativeConv and refactored. `log_value`, `logsumexp2`,
`log_add`, `log_mul`, `log_div`, `rp_accum`.

**Fixed** (both defects inherited from the seed)
- `logsumexp2` absorbed infinities and swallowed NaN: `if (!isfinite(a))
  return b;` made `logsumexp2(+inf, x)` return `x`. NaN and ±inf now
  propagate per IEEE, including `inf + (-inf)` → NaN.
- The `pos == neg` cancellation reset was an undocumented precision cliff.
  Kept, but stated as an explicit error-bound decision at the reset site:
  discards residual up to |largest term|·eps per reset event.
