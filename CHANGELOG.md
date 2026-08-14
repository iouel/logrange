# Changelog

Versions identify the header, which is the product. `LOGRANGE_VERSION_*` in
`include/logrange/log_math.h` is the source of truth; CMake parses it.
Ordered comparisons use `LOGRANGE_VERSION` (MAJOR·10000 + MINOR·100 + PATCH),
so `#if LOGRANGE_VERSION >= 200` means 0.2.0 or newer.

Pre-1.0: the error contract can still change between minor versions. What
will not change silently is the contract's *existence* — any bound that moves
is recorded here with its old and new values.

## Unreleased

**Added**
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
