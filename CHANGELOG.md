# Changelog

Versions identify the header, which is the product. `LOGRANGE_VERSION_*` in
`include/logrange/log_math.h` is the source of truth; CMake parses it.
Ordered comparisons use `LOGRANGE_VERSION` (MAJOR·10000 + MINOR·100 + PATCH),
so `#if LOGRANGE_VERSION >= 200` means 0.2.0 or newer.

Pre-1.0: the error contract can still change between minor versions. What
will not change silently is the contract's *existence*: any bound that moves
is recorded here with its old and new values.

## Unreleased

**`pass/` no longer reimplements reduction analysis either.** The matcher
stopped on 2026-08-17; the pass followed the same day. `isReductionPHI`
filtered to `FAdd`/`FMulAdd` identifies the accumulator, `getRecurrenceStartValue`
and `getLoopExitInstr` replace hand-read incoming values, and the phi/update
clean-use pair is gone.

**Measured identical.** Pre- and post-migration runs of `run_pass_test.sh` in
isolated worktrees produce the same assertions, the same rewrite and decline
records, and byte-identical `INFO` values — every accuracy ratio and the
bound-search worst case (0.99 over 7285 trials). Nothing about what the pass
rewrites changed.

Kept deliberately: the stricter clauses are rewrite legality, not recognition,
and `isReductionPHI` does not imply them — single FP phi in the header, double
only, constant-zero start, unique exiting and exit block. `soleInLoopUser`
survives for one use, the `fmul` in `fadd(phi, fmul(W, X))`, which is a
term-side node no reduction analysis has an opinion about.

**The supported pass pipeline is now
`-passes='loop-simplify,lcssa,log-rewrite<force>,adce'`.** Normative in
`ELIGIBILITY.md` section 0. Both prefixes are required and they fail
differently: without `loop-simplify` the pass reports `not-loop-simplified`,
with it but without `lcssa` it reports `not-lcssa`, and neither rewrites
anything. A new `DECLINE-PIPELINE` record names whichever is missing, because
the failure mode is a clean empty report that reads as "no eligible loop".

Two things about that guard are worth recording, since both were wrong first.
It was placed after the preheader/latch null checks, where it is unreachable
for the case it exists to name — an un-canonicalized loop has no preheader and
bails there silently. And it checked only loop-simplify form, which on this
kernel is the half that was already satisfied. `run_pass_test.sh` now asserts
both tiers and the clean case, mutation-tested by swapping the expected
reasons.

**`logrange_intent.md` states intent again: 282 lines to 148.** Removed a
94-line "First Action — status" work log that indexed records living in
`BENCHMARKS.md`, `matcher/RESULTS.md`, `pass/PROTOTYPE.md`, `matcher/DELTA.md`,
this file and `TODO.md`. One of its hand-copied figures — "781 hits across
2859 innermost FP loops (27%)" — was two revisions stale (781 → 783 → 814)
while CI gated the same number elsewhere, which is the argument against
keeping a second copy. The only fact lost is commit `afee8d0`, which `git log`
holds better.

"Shipping Posture" keeps the decision and its reasoning, in present tense; its
six pass conditions and their closure status move to `TODO.md`, where every
other tracker lives. The Stretch Goal's struck-through criterion and status
stamps are gone; the refutation and its sweep are recorded below under 0.3.0.

New rule in `CONTRIBUTING.md`, "Where a sentence goes", and a seventh CI gate
enforcing it: no dates, commit SHAs, or measured figures in the intent doc.
The gate deliberately does not reject all numbers — domain constants and spec
quantities are what intent is made of.

**Restored `logrange_intent.md`'s "Deliverable 2 — The Pass".** Deleted in
`074749f` under the summary "removed redundant content". Nine files cite it,
and three quoted sentences that then existed nowhere in the tree. Reinstated
verbatim, plus the prior-art boundary it carried — LLVM's loop-idiom pass,
Herbie, FPChecker — which is the rule the recognizer change below follows and
which was missing from the tree for the whole period the duplication grew.

**Reduction recognition moved to LLVM's `RecurrenceDescriptor`.** The matcher
had reimplemented an analysis the loop vectorizer already performs. It no
longer does; what stays hand-written is the term walk and the risk grading,
which no compiler provides.

**Retracted: corpus hit counts.**

Old: 2859 loops, **783** hits (27.4%), 7 transcendental — GSL 753, libsvm 11.
New: 2859 loops, **814** hits (28.5%), 8 transcendental — GSL 783, libsvm 12.

The denominator, the **5 HIGH findings on 4 source lines**, and every risk
grade are unchanged. Measured per-loop with both recognizers in one pass
before the old one was deleted: +32 hits, −1. Zero loops were graded
differently by the two. Full accounting in `matcher/DELTA.md`, paired records
in `matcher/data/raw-delta.txt`.

The 32 gains are conditionally-executed reductions (`if (c) acc += t;`, 19)
and accumulators mirrored to a loop-invariant address (13). The single loss is
`coulomb.c:403`, MED, whose loop has a data-dependent `break` and therefore no
unique reduction exit value; LLVM declines it, which is the conservative
direction.

**A documented blind spot closed.** `out[j] += prev[i] * A[i*n+j]` — the
forward algorithm as usually written, one of the three shapes the README names
— is now matched. `coverage.c`'s expectation moved from `expect_miss` to
`expect_hit`, so the closure cannot silently regress.

**Two scan-pipeline passes added.** `logrange-scan.sh` and `run_study.sh` now
run `loop-simplify,lcssa` ahead of the matcher. Both are unstated
preconditions of `AddReductionVar`: without the first it reads through a null
preheader (a segfault, not an assert, in a release build); without the second
it declined 28 reductions on this corpus. Verified byte-identical for the
previous recognizer before the swap.

**`REJECT` cause vocabulary changed.** `spine` and `cleanUses` are gone with
the code that produced them; causes are now `not-reduction`, `kind`,
`dirtyChain`, `noMulNoExp`. `./run_study.sh rejects` reports the new census
and fails if `not-simplified`, `chain`, or `no-terms` is ever non-zero.

**Published corpus figures are now derived from committed evidence, not
asserted in prose.** New rule in CONTRIBUTING.md, "A figure is derived or it
is wrong", and a fifth CI gate enforcing it.

`matcher/run_study.sh figures` computes every headline count from the
committed `matcher/data/raw-{darknet,gsl,libsvm}.txt` and diffs the result
against `matcher/data/FIGURES.txt`. It needs no corpus checkout, no bitcode
harvest and no plugin build, so it runs in CI — which is the point. The
previous rule ("published numbers must be reproducible from committed
tooling") was satisfied and did not help: the census was reproducible only on
a machine holding the corpus, and the number reached the docs by hand.

**Retracted: "0 `w*exp(t)` sites in 2859 corpus loops."**

Old: 0 sites in 2859 loops, used to argue the mixture spine is absent from
real code and that bounded-weight support is not worth building.
New: 0 `w*exp(t)` multiplies among the **5** exp-carrying reductions the
matcher accepted. The census is gated on `CI.expChain` and runs *after* the
`HIT` is emitted, so 2859 was never its denominator.

Of those 5, **2** have `nMul=0` and contain no multiply at all; the other 3
carry theirs inside the `exp` argument. The census cannot see any loop
rejected upstream, which at the time included the mirrored
`out[j] += w*exp(t)` form — where the shape most plausibly lives. (That form
is matched as of the recognizer change above, and the census still returns 0
over 814 hits.) n=5 does not settle whether the extension point is worth
taking, and TODO.md no longer claims it does. Corrected in four files.

Within a minute of existing, `figures` caught a second miscount in the
replacement text (3 `nMul=0` rows claimed, 2 actual).

**Corrected: the dead original chain needs `adce`, not `dce`.** Posture
condition 6. `PROTOTYPE.md` and `LogRewritePass.cpp` both said the orphaned
chain is left for "later DCE/ADCE". Plain `dce` removes nothing — the orphan
is a loop-carried cycle, so every instruction in it has a use. Measured:
26 `llvm.exp.f64` after the rewrite, 26 after `dce`, 22 after `adce`, one dead
exp per rewritten f64 loop. `-passes='log-rewrite<force>,adce'` is now the
documented supported pipeline and the drop is asserted in `run_pass_test.sh`.

**Pass test suite: every rewrite must be graded HIGH by the matcher.**

`run_pass_test.sh` section 3d builds the matcher plugin and runs it over
`kernel_rw.ll` — the same IR the pass consumed, not a fresh compile of the
same source, so a disagreement is evidence about the two analyses rather than
about two builds. Both tools print the backedge update's location, so
`(file, line, function)` is an exact join key; verified before the check was
written, all five rewrites join to a HIT at the identical line.

This is the soundness direction, and it is the one that makes "profitability
analysis in front of any rewrite" mean something: the rewrite cannot fire on a
loop the triage would not have flagged. Two failures reported separately — no
HIT at all, and a HIT graded below HIGH. Both negative-tested by mutating the
*matcher* (suppressing the `exp-sum` family's hits; dropping
`expChain => HIGH`), so the check is known to read real matcher output.

Verdict **equality** is deliberately not asserted. It is false and documented
as false in ELIGIBILITY.md 7.1: the matcher's exp-family is wider and its
`nMul` counts the whole term chain, so `s += a*b*c*d*e` is matcher-MED where
the pass prints LOW. Asserting equality would either fail immediately or force
the pass to grow a chain walk it does not otherwise need.

**Pass test suite: the rewritten set is now derived from the pass's output
and every member must carry a numeric assertion.**

The suite was a closed-world whitelist — "these four names are rewritten" and
"these names are bit-identical" were two independently hand-maintained lists
with nothing tying them. A function could enter the first without entering the
second.

**Found by mutation, and it was a wrong-answer miscompile, not a near miss.**
Relaxing the weight clause to `Weight && !isa<ConstantFP>(Weight)` — a
plausible refinement, since a constant weight *is* provably bounded — produced
a pass that rewrites `s += 0.5*exp(x[i])` **with the `0.5` silently dropped**,
because nothing reads the weight after that clause. The full gate stayed
green: `REWRITE` count 4, both expected declines, `OVERALL,PASS`, bound
`HELD` at 0.99. No constant-weight kernel existed to be counted.

Now: `run_pass_test.sh` reads the rewritten functions out of `rewrite.log` and
requires a passing `cover_<fn>_*` check in the harness for each one, with a
lower bound on the named shapes guarding the other direction. Corpus gained
`const_weight_sum` (the case that was missing), `expf_widened` (the f32/fpext
rewriting path, previously untested in any form), and the weighted overflow
witness as an executable test rather than a comment. The mutation above now
fails; so does dropping a `cover_` prefix.

**Corrected: the pass's risk verdict is not the matcher's rule.** Measured
divergences, all reachable: `s += a*b*c*d*e` is matcher-MED and prints LOW
here; `s += c*pow(a,b)` is matcher-HIGH and prints LOW; `s += pow(a,b)` and
`s += exp2(x)` are matcher-HIGH and silent here. Two causes — the matcher's
exp-family is wider (`pow`, `exp2`, `expm1` by substring), and its `nMul` is
counted over the whole term chain including inside the `exp` argument, where
this pass counts spine multiplies. MED is unreachable in *this pass's verdict
function* by construction; that is not the claim the docs made.

**Corrected: `exp-chain;exp-sum` is a constant, not a computed taxonomy.**
`exp-sum` means `nMul == 0` to the matcher and is wrong whenever the chain
carries a multiply.

**Restated: posture condition 3's literal text is met, its property is not.**
The gate is reachable — `dot_sum` verdicts LOW and is refused at the default
threshold — and it is not load-bearing. Eligibility requires an accepted `exp`
term and a HIGH verdict means the same thing, so `rewritable ⊆ HIGH`; every
LOW loop is weighted and the weight clause refuses it at any threshold. The
rewrite set at `min-risk=low` is identical to the default, now asserted. The
condition closes when the rewritable set exceeds the HIGH set, which needs
bounded-weight support, not more matched-and-declined shapes.

**Pass (prototype, outside 1.0's supported surface): shape coverage widened
to the weighted spines, and the profitability gate now declines a real
input.**

`log-rewrite` matches three backedge spines — `fadd(phi, X)`,
`fadd(phi, fmul(W, X))` and `llvm.fmuladd(W, X, phi)` — and rewrites only the
first. Which weighted form clang emits is `-ffp-contract`, a flag the pass
does not control: `=on` (default) gives `llvm.fmuladd`, `=off` and `=fast`
give `fmul` + `fadd`. All three are gated.

Weighted spines are **matched in order to be declined**,
`DECLINE-WEIGHT,...,unbounded-weight`. Rewriting one would make the state
accumulate `sum(w_i * exp(t_i - m))`, reaching `sum|w_i|`, which has no
ceiling — the unweighted state is at most `n`, its exponents being `<= 0`.

**The risk gate declines a real input at the default threshold for the first
time.** `dot_sum`'s `llvm.fmuladd(x, y, phi)` has no `exp` in its chain,
verdicts LOW, and is refused. Previously every matchable shape required an
`exp` call, so the verdict was HIGH by construction and the refusal path was
reachable only by raising `min-risk` synthetically. This closes "Shipping
Posture" condition 3. The posture itself does not move: neither number that
drove it changed.

Clause order is now normative (ELIGIBILITY.md 3.3): risk gate first, weight
clause second. Reversed, the weight clause shadows the gate and the only
LOW-verdict input never reaches it.

**Corrected: the weight-overflow witness published 2026-08-16 was wrong in
both constants.**

Old: `w=1e300, t=-700` overflows the state at `n>=2` while the linear sum is
~1e-5.
New: it does not overflow at `n=2` — the state reaches 2e300, and `sum|w_i|`
at that weight needs `n ~ 1.8e8` — and the linear sum is 1.97e-4.
Measured witness: `w=(1e308,1e308)`, `t=(-700,-700)` drives the state to
`inf` at `n=2` while the linear loop gives 19719.4.

The conclusion the old figures were offered for is unchanged: magnitude is
the constraint, and it is unbounded. Separately recorded and **not** the
reason for the decline: the emitted reduction `exp(m + log(s))` gives NaN for
a negative state, which is a property of the reduction rather than the state
and is fixable with `copysign` — consistent with the 2026-08-16 retraction of
the claim that negative weights break semantics outright.

**Documented: `add_scaled`'s scaling cost, which no contract term named.**

Both accumulators implement `add_scaled(v, c)` as
`add_log(v.log_abs + std::log(c))`. `std::log(c)` is computed, so the term
enters carrying `ulp(|log c|)/2 <= |log c|*u` absolute in log space, a
relative error of the same size on that term. Measured by sweep: 8u, 64u,
256u, 512u at `|log c|` = 10, 100, 400, 690, exactly `ulp(|log c|)/2` each
time. At the top of the range that is 128x the 4u a single term is budgeted.

**Stated at `add_scaled`, not folded into the accumulator bound.** A caller
writing `add_log(v.log_abs + std::log(c))` by hand pays the same, so this is
the cost of entering log space at that scale rather than of accumulating.
The contract continues to cover what the accumulator does with the terms it
is given. The claim `(|log c| + 4)*u` for a single scaled term is searched
and asserted by `bound_search` family F, worst 0.74.

The representation floor at `log_value` does not cover this. That floor is
`|log|x||*u` for the value being represented; here the scaled term's own
magnitude can be ~0 while the injected error is 512u, because it is set by an
INPUT's magnitude. Third instance of the class that produced `D` and then
`|log|net||`, and the last site in the header with that shape.

Caller guidance, measured: when `c*|x|` is representable, forming it and
using `add()` costs 1.9u independent of scale, against 512u here.

## 0.3.0 — 2026-08-16

**A minor bump, because the error contract moved.** Three times, in fact, all
within this release: `rp_accum`'s twice and `pos_accum`'s once. Compatibility
is `SameMinorVersion` precisely for this — a consumer pinned to 0.2 is pinned
to a contract that has since been refuted, and should not silently resolve to
this header. Callers who checked `LOGRANGE_VERSION >= 200` want `>= 300`.

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
  error is **7.97u**, a ratio of **1.99**. Worst against the new form: 0.83.
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

One assumption became a measurement: `std::exp` is worst **1.00u** against
`dd_exp` over 20001 points in [-700, 700], confirming the floor the old file
claimed. `bound_search` reports it on every run, so it is reproducible rather
than a figure from a scratch file.

Honest scope: family E's 1.99 would have been visible against the old
reference too. The family is what found the defect; the reference is what
makes the marginal cases readable and the 0.83 worst-case trustworthy.

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
