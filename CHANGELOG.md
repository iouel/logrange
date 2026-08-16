# Changelog

Versions identify the header, which is the product. `LOGRANGE_VERSION_*` in
`include/logrange/log_math.h` is the source of truth; CMake parses it.
Ordered comparisons use `LOGRANGE_VERSION` (MAJOR·10000 + MINOR·100 + PATCH),
so `#if LOGRANGE_VERSION >= 200` means 0.2.0 or newer.

Pre-1.0: the error contract can still change between minor versions. What
will not change silently is the contract's *existence*: any bound that moves
is recorded here with its old and new values.

## Unreleased

**Changed: the error contract moved again. `rp_accum`'s corrected form was
itself refuted, at 1.99x.**

Old (2026-08-15): `cond*(3k + 4 + D)*u + |log|S||*u`
New (2026-08-16): `cond*(3k + 4 + D)*u + (|log|S|| + |log|net||)*u`

where `net = S / exp(m_log)` is the scaled sum the reduction takes the log of.
`|log|net|| <= log n` for positive sums, `<= log(n*cond)` in general.

The reduction is `out.log_abs = m_log + log|net|`, and absolute error in log
space is relative error in linear space, so **both** addends' roundings land
on the result. The old form charged `u*|log|S||`, which is the rounding of the
addition's *result*, and ignored that `log|net|` is computed to a relative `u`
and therefore an absolute `u*|log|net||`. The two addends cancel exactly when
the sum is near 1, and there `|log|S||` goes to zero while `|log|net||` does
not.

This is the same defect as the one corrected on 2026-08-15, one level up:
charging for a result's rounding while ignoring an input's magnitude. That
one was per-term (`2u` for a term's `exp` ignoring its argument, which became
`D`). This one is the final reduction.

- **The witness.** `bound_search.cpp` family E: `n` equal positive terms at
  `L = -log(n)`. Then `net = n` exactly, integer adds below 2^53 are exact so
  there is no summation error at all, `cond = 1`, `k = 0`, `D = 0`, and
  `|log|S|| = 0` — the entire old budget is `4u`. At n = 166463 the measured
  error is **7.97u**, a ratio of **1.99**. Worst against the new form: 0.50.
- **`pos_accum` is not refuted** (worst 0.80) and carries the new term anyway,
  for correctness rather than because it binds: `|log|net|| <= log n` and its
  `n*u` term already dominates `log n`. `rp_accum` is exposed precisely
  because Neumaier compensation removed its `n*u` term, leaving a flat `7u`
  budget at `k=1, cond=1`.
- **Found by asking what a rescale really costs**, while deriving the emitted
  code's bound. That question led to a plateau-then-step family, which came in
  at 1.16 — inside the old reference's noise. Family E is the clean maximizer
  the plateau was approximating.

**Changed: the search's reference no longer assumes anything about libm.**

`bound_search.cpp` built its reference from `std::exp()` in plain double and
then collapsed the double-double accumulator with `value()`. Two floors: ~1u
per term, and up to u/2 from "truth" being a *double* at the moment of
comparison. The second is structural — no libm improvement removes it — and
it was undocumented.

`tests/dd_exp.h` computes exp in double-double with its own argument reduction
and Taylor series, and the reference stays wide through the subtraction. Terms
are scaled by the peak's binary exponent, because `ldexp` on both words drives
the low word subnormal near the bottom of the range and silently degrades the
pair to ~61 bits. Validated by identities needing no external constant
(`exp(0) == 1` exactly, `exp(ln2) == 2`, `exp(x)exp(-x) == 1`,
`exp(a)exp(b) == exp(a+b)`); `bound_search` refuses to report if they fail.
Resolution: ~1e-30 relative, about 1e-14 u.

Two assumptions became measurements: `std::exp` is worst **0.99u** over 20001
points in [-700, 700], and `expl` is worst **5.8e-4 u**. The first confirms
the floor the old file claimed.

Honest scope: family E's 1.99 would have been visible against the old
reference too. The family is what found the defect; the reference is what
makes the marginal cases readable and the 0.50 worst-case trustworthy.

**Changed: the error contract moved.**

The 0.2.0 contract was `rel err ≤ cond·(3k+4)·u`. It was refuted, not
adjusted: `tests/bound_search.cpp` found 151 of 400 random inputs violating
that form, worst case 15.8× over. A constructed counterexample reaches 5.8× at
`cond = 1, k = 0`, where the old bound is bare `4u`. Two terms were missing.

- **Argument rounding.** The derivation charged each term `2u` for its `exp()`
  and ignored the rounding of `exp()`'s *argument*. `fl(L_i − m_i)` differs
  from the exact difference by up to `u·d_i` where `d_i = m_i − L_i`, and
  `exp` converts that into a relative error of the same size, so a term costs
  `(d_i + 2)·u`. The `2u` is unchanged and still sits inside the `+4`; `d_i·u`
  is what was missing. Terms at equal depth share one rounding error
  coherently, with nothing to cancel it. It enters as `D`, the mass-weighted
  mean insertion depth, where depth is the gap in *log space* below the
  running reference: ~`ln n` for ordinary data, capped by the ~745 vanishing
  window since deeper terms scale to zero and leave the sum.
- **Final reduction.** `out.log_abs = m_log + log|net|` rounds to within
  `u·|log|S||`, and absolute error in log space is relative error in linear
  space. Invisible for sums near 1. At `log|S| ~ 700`, the regime this library
  exists for, it is ~700u on its own, and it never touches `cond`. Most of the
  random refutations were hitting this.

New: `rel err ≤ cond·(3k + 4 + D)·u + |log|S||·u`. The search scores every
input against both forms; the new one was exceeded zero times out of 400,
worst observed/bound 0.85.

**`pos_accum` was refuted the same way, harder.**

Old: `(n+3k+3)·u`. It fails on 119 of 400 random inputs, worst **34.9×**.
Every violation is the same final-reduction term: `m_log + log(sum)` rounds to
`u·|log|S||`, which does not grow with `n` and has no `cond` to hide behind.
Four terms near `e⁶⁹⁰` budget `7u` against ~500u of real error. New:
`(n + 3k + 3 + D)·u + |log|S||·u`, worst 0.79 across the search.

`D` is carried there for symmetry but is **not** the binding term: making `D`
large takes ~`e^D` terms at depth `D`, so `D ~ ln n < n` and the `n·u` term
already covers it. Measured, not assumed: depth clusters aimed at `pos_accum`
never exceed 0.22 of even the old bound.

This bound had also never been machine-checked. `test_pos_accum` asserted
behavior only; it now asserts the contract, and that assertion was verified
to fail under the old form.

Bounds that moved in `test_accuracy` (observed values did not change):
n=10⁶ positive sum 7.4e-15 → 1.0e-14; heavy cancellation 1.6e-5 → 1.6e-5;
shuffled 5.7e-6 → 6.4e-6.

**Added: an error contract for the code the rewrite pass emits. Tooling
only.**

The runtime shipped three machine-checked bounds; the emitted streaming state
had one measurement, 1.37e-15 on one benign case, and no bound. Now:

    rel err  <=  (n + 3k + 4 + D)*u  +  |log|S||*u

Normative in `pass/ELIGIBILITY.md`. `pass/emitted_bound_search.c` searches it
against the object the pass actually rewrote, not against a replica, and
`run_pass_test.sh` fails on any violation. Held across 7285 trials, worst
observed/bound 0.99. Gate negative-tested by halving the bound: 1261
violations, worst 1.98.

- **The derivation is `pos_accum`'s plus `1u` for the final `exp`, and the
  inheritance is conditional.** The emitted form is branchless and multiplies
  every iteration where `pos_accum` multiplies only on a rescale. It matches
  term for term only because `exp(0)` is exactly `1.0` and `s*1.0` is exact.
  A merely-1-ulp `exp(0)` would cost `n*u` the runtime never pays. Both are
  asserted at startup.
- **The binding case is not the one predicted.** `3k*u` charges nothing for
  the size of a reference jump, the omission that refuted `rp_accum`, but
  ascending families reach only 0.23: after a jump of `J` the old sum's share
  of the total is `s/(s + e^J)`, so the error that survives is a fraction of
  a `u`. The tight case is a one-depth cluster where the running sum's
  roundings align, giving the classical `(n-1)u` of uncompensated summation.
  The ratio plateaus at 0.990 across N = 2048 through 16384.
- **Two scope conditions the runtime's contract does not carry.** The emitted
  code returns a linear double, so `|log|S||` cannot exceed 709.78 and the
  reduction term is capped near `710u`. And the first-order form requires `n`
  under ~2.1e8, derived from where recursive summation's second-order term
  eats the constant.
- **The reduction term is measured, not inherited.** Each run also scores the
  form without it: exceeded on 321 of 7285 trials at up to 39x.

Shipping-posture condition 4 closes. Four of six are now closed; shape
coverage and the dead original chain remain.

**Added: `matcher/logrange-scan.sh`, one command from a build to a report.
Tooling only.**

The diagnostic was a three-step: build the target under `cc-bc.sh`, run
`run_study.sh <name>`, run `diagnose.sh` on the raw file. It now takes a build
directory or a `compile_commands.json`, and neither builds the target nor
requires that clang can build it: each unit in the database is recompiled to
bitcode at the study's flags, with a second attempt using preprocessor and
language flags only for units clang rejects outright. `--check` preflights the
toolchain. `SETUP.md` packages the WSL and LLVM 21 requirement for all three
tools. `matcher/test_scan.sh` is CI gate 4.

- **An incomplete scan exits 2, not 0.** A unit that fails to compile would
  otherwise produce a short report that reads as a clean bill of health. The
  tally above every report states units compiled, failed, skipped, deduped
  and recovered. `--allow-compile-failures` accepts a partial scan
  deliberately.
- **One source listed by two cmake targets is scanned once.** Counting its
  loops twice inflates every number in the report.

**Fixed: `diagnose.sh` printed `flagged: exp-sum`.** The matcher has emitted
the `exp-sum` reason token since 2026-08-15, the fix that admitted plain sums
of `exp`; the renderer had no sentence for it and fell through to its
unknown-token passthrough. Found by pointing the new front door at this
repository, whose `bench/bench_main.cpp:302` is exactly that shape.
`testdata/fixture-raw.txt` claimed to exercise every report branch and had no
runner. It does now: `test_scan.sh` case 11 extracts the token list from
`SumOfProductsMatcher.cpp` and fails when the fixture or the renderer misses
one.

**Added: `propagate=div`, the stretch goal's first milestone. Tooling only.**

The pass can carry the log form out of the accumulation loop and into the
softmax normalize divide, so the rescued value reaches an observable result
without the `__logrange_logsum` side global. Behind its own named grant;
`force` alone does not trigger it, and an unrecognized `propagate=` value is
refused rather than ignored.

`fdiv(llvm.exp(t), sum)` becomes `exp(t - L)`. Measured at inputs near −800,
where the linear form computes `0.0/0.0` and every output is NaN: the
propagated form returns probabilities summing to 1.0000000000000262.

- **The divisor's log form travels through the loop-exit merge.** clang
  guards the accumulation loop, so the consumer divides by an LCSSA phi
  merging the rewritten sum with `0.0` from the zero-trip path, and no block
  dominates the divide. Requiring one declined the only shape the milestone
  is about. A value's log form is now derived structurally, which is the
  design's phi transfer function: `LogSum` for the rewritten sum, `log(c)`
  for a constant with `0.0 → -inf`, a parallel phi for a phi, decline
  otherwise. `-inf` on the bypass reproduces the linear answers:
  `x/0 = +inf` against `exp(t + inf)`, and `0/0 = NaN` against `exp(NaN)`.
- **The numerator is not re-logged.** `exp(t)` underflows to `0.0` at these
  inputs, so `log(numerator)` was `log(0) = -inf` and the propagated result
  collapsed at exactly the inputs it exists to rescue. The pre-`exp` argument
  `t` is used directly, so correctness does not rest on a later InstCombine
  fold of `log(exp(t)) → t`.
- **Accuracy at one conversion: 1.33x to 13.9x behind the linear
  re-conversion, 64 of 64 swept trials.** Sweep: spreads 0.5/1/3/8, lengths
  100 and 1000, eight seeds each, long-double reference with error 1.7e-18.
  `t - L` carries `u·|t - L|`; re-conversion carries one rounding. This is
  the case where propagation has least to offer, and it measures one
  conversion, not a chain. Chains are unmeasured: the vocabulary is one
  rule. The stretch goal's success criterion asserted the opposite ranking
  and is amended in `logrange_intent.md`.
- The test asserts the linear path is broken on the rescue inputs. Without
  that, it checked only the propagated output, and a numerically wrong
  transform would have passed.

**Changed: the rewrite pass's eligibility contract narrowed.**

Tooling only; the header is unaffected. `pass/` is a labeled prototype, not
installed and not packaged, so this changes no shipped interface. It is
recorded because it retracts a documented stance and closes a stated
precondition.

- **Infinite terms produced NaN.** The streaming update's exponents are
  differences against the running max, and `x - x` is NaN when `x` is
  infinite. Two reachable cases: `t = -inf` arriving while the max is still
  `-inf` (a zero term: `exp(-inf) = 0`, ordinary input, the documented one)
  and `t = +inf` (undocumented, same root cause). Each difference is now
  `(x oeq newm) ? 0.0 : x - newm`: 4 instructions per iteration against 2
  `exp` calls, and no finite result moves. `oeq` is load-bearing: a NaN
  operand is never equal to `newm`, so NaN still propagates.
- **Errno.** PROTOTYPE.md previously stated that libm `errno` behaviour was
  irrelevant. **Withdrawn.** That is true of `matcher/`, which recognizes
  shapes and observes nothing; it does not transfer to a pass that *replaces*
  the computation. The rewrite deletes N source `exp` evaluations and emits
  2N different exponentials plus a `log`. The pass now matches **only
  `llvm.exp.*`** and declines direct `exp`/`expf` with `DECLINE-ERRNO`.
  Measured basis on LLVM 21: `-O1` emits `call double @exp`;
  `-O1 -fno-math-errno` emits `llvm.exp.f64`. The intrinsic is the marker
  that errno is already unobservable.
- **FP environment.** `strictfp` functions, functions containing
  `llvm.experimental.constrained.*`, and non-IEEE `denormal-fp-math` modes
  are declined with `DECLINE-FPENV`.
- **`force` narrowed** to waive reassociation *proof* and nothing else: not
  the structural match, not the FP-environment screen, not the errno
  contract, not special-value correctness. `"unsafe-fp-math"="true"` is
  retained as an alternate opt-in only, never as evidence that special values
  may be discarded.
- **New `pass/ELIGIBILITY.md`**, normative; PROTOTYPE.md is the design
  narrative and measured record, and ELIGIBILITY.md wins on conflict.
- **Fixed: the harness's reference oracle.** `ref_logsumexp()` applied the
  max-shift unconditionally and returned NaN for all-`-inf` and for any input
  containing `+inf`. The infinity assertions added earlier the same day were
  therefore constant-based only, not reference-validated. Now handles NaN,
  `+inf` and all-`-inf` before the shift.
- **Harness.** Requires `-fno-math-errno` (without it the kernel no longer
  matches), pins `clang-21` beside `opt-21`, and deletes the plugin before
  building so a failed build cannot leave stale code under test.

This closes Deliverable 2's "semantics preservation is exact" precondition
for the one shape the pass matches. Finite rounding differences remain
permitted and intentional, and are what the reassociation grant buys:
1.37e-15 relative measured against a 1e-12 bound. Special-value differences
are forbidden.

**Changed: bound presentation, after an independent read.**

No counterexample to either corrected contract was found; these are
presentation fixes, not corrections to the bounds.

- Both forms are now labelled **first-order**, holding under the stated
  assumptions (1-ulp `exp()`, the vanishing window, neglected O(n·u²)),
  rather than reading as unconditional inequalities.
- The per-reset `Σ Aⱼ·u` discard is no longer presented as a separate
  absolute term alongside a relative bound. It is already covered: reset
  epochs are disjoint and each carries mass ≥ 2Aⱼ, so Σ Aⱼ ≤ ½·cond·|S| and
  the relative contribution is ≤ cond·u/2, inside the existing 4u
  coefficient, for any number of resets. The disjointness step keeps it from
  growing with the reset count, and is stated explicitly.
- `cond` and the `|log|S||` term are described as a division of labor rather
  than competing explanations: `cond` covers cancellation in *forming* `net`,
  `|log|S||·u` covers the single rounding of `m_log + log|net|` once `net` is
  known. Error in computing the value versus error in representing it.

**Fixed: the installed package's config comment.** `LogRangeConfig.cmake.in`
still told consumers the imported target carries `-ffp-contract=off` /
`/fp:precise`. That propagation was removed when the FMA diagnosis was
corrected, but the template shipping inside the install tree was not updated
with it.

**Changed: `log_mul` / `log_div` were documented as exact. They are not.**

The mapping is exact (multiplication is addition of logarithms); the
arithmetic implementing it is a floating-point add and rounds. Old claim:
"exact in log domain". New: `error(log_abs) ≤ u·|log|a| + log|b||`, exact
only when that sum is representable. Measured at 1024u on a product of
log-magnitude 1024, outside double's linear range but an ordinary
`log_value`.

**Added: the precision floor, stated once at `log_value`.**

This is the root cause of all three refuted bounds, and it was never written
down. `log_abs` is a double, so a value is representable no better than
`|log|x||·u` relative, *however it was computed*. Near 1.0 that is invisible;
at `|log|x|| ~ 700` it is ~512u, about 13 significant decimal digits rather
than 16. Every reduction ending in `m_log + log(...)` inherits it, which is
why one undocumented fact broke two independent accumulator contracts.
Measured: 512u on a `log_value(x).to_linear()` round trip.

`logsumexp2` now states an accuracy bound for the first time:
`u·|result| + (d+3)·u` with `d = |a−b|`.

Two long-open questions closed by inspection, no code or contract change:
pos/neg rescale errors bounded independently and summed is conservative under
any correlation (triangle inequality on `pos − neg`), and the `O(n·u²)` term
does not reach `u` until ~10¹⁶ terms.

**Added: install rules and a config package.**

`cmake --install` now installs the header and a config package, so consumers
can `find_package(LogRange 0.2 CONFIG REQUIRED)` and link
`LogRange::logrange`. Vendoring via `add_subdirectory` gives the same target,
builds no tests, and no longer leaks this project's `-Werror` into the
consumer's build (the strict flags moved off the library target onto a
private one). Compatibility is `SameMinorVersion`: pre-1.0 the error contract
can move between minors, so treating 0.2 and 0.9 as interchangeable would be
the wrong promise.

`examples/quickstart` is the README snippet compiled against the *installed*
package and checked against its analytic answer. CI installs to a temp prefix
and builds it as a consumer on all three legs.

**Added: the header refuses to compile under fast-math.**

`-ffast-math` / `/fp:fast` folds away the algebraic identities `rp_accum` uses
to recover each addition's rounding error, degrading it to an uncompensated
sum. Measured on a 40000-term cancellation set: log-magnitude
−7.36251563240462303 normally, −7.36251072122148731 under `-ffast-math`, a
relative error of 4.9e-6, nine orders past the contract. Detected via
`__FAST_MATH__` / `_M_FP_FAST`; `LOGRANGE_ALLOW_FAST_MATH` overrides. CI
asserts both that the guard fires and that the override still builds.

**Correction to an earlier entry in this same release.** A previous version of
this changelog and README claimed the exported target must carry
`-ffp-contract=off` because FMA contraction cost 14× accuracy. That was wrong
and the flag has been removed from the consumer-facing target. On fixed
inputs the accumulator is **bit-identical** with contraction on or off,
structurally so: the compensation path holds no multiply-add pair to fuse.
The apparent 14× came from `test_accuracy`'s cancellation corpus being
*generated* with multiply-add expressions that contract differently, changing
the dataset. The tell was there and was missed: the uncompensated `log_add`
fold rows moved too, and nothing in that algorithm could have been affected by
compensation damage.

`-ffp-contract=off` is still pinned on this project's own builds, for
reproducibility of that corpus rather than for correctness: it keeps the
accuracy table bit-identical across gcc 13/15 and clang 18/21.
`LOGRANGE_PROPAGATE_FP_FLAGS` is gone; it existed only to serve the wrong
diagnosis, and while it existed, setting it to OFF silently disarmed the flag
in this project's own test suite.

**Decided**
- **Double only, by design.** Stated in the header's Precision block rather
  than left as an unmentioned limit. Every constant in the error contract is
  double-specific (u = 2⁻⁵³, the ~745 log-unit vanishing window, and the new
  D term that window caps), so a float variant means re-deriving the bound at
  u = 2⁻²⁴ with a ~103 log-unit window and a finer accuracy reference, not a
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
