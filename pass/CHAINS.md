# The chain wall — log-form propagation measured, and declined

*Answers the stretch goal's open question, "chains are the claim and are
unmeasured" (`TODO.md`). Both vocabulary rules were measured by hand, with no
pass and no lattice involved, and neither wins on accuracy. This file is the
measurement and the decision it forces.*

## The question

`propagate=div` carries the log form through one divide. The design's case for
going further is **chains**: following a computation across operations that
would otherwise materialize and re-convert at each step. The intent states the
mechanism against it in the same breath: *"Each step adds a term proportional
to the log-magnitude it crosses."*

At one conversion propagation loses, measured, 64 of 64 trials at 1.29x to
9.31x (`PROTOTYPE.md`). The open claim was that chains amortize that.

## The instrument

`tests/chain_search.cpp`, a ctest. No LLVM, no plugin, no corpus, so
`ctest -R chain_search` reproduces every figure below on any machine. That is
why no raw evidence file is committed beside this one: the tooling *is* the
derivation, where `matcher/`'s figures need a harvested corpus and therefore
need `data/raw-*.txt`.

Reference is double-double (`tests/dd_exp.h`), not `long double`: MSVC's
`long double` is 64-bit and windows-msvc is a CI leg, so `expl` would have
given no reference at all there. Identity selftest worst 1.683e-30.

**Both toolchains agree.** Every figure below is identical on msvcrt and on
glibc, except three win/tie counts in experiment 1 that shift by one in the
closest cells (benign N=1 and N=3, mid N=1). No headline figure moves.

## Experiment 1 — multiplicative chains. The wall.

One `fdiv` then `fmul`s: `fdiv -> fsub` and `fmul -> fadd`, the vocabulary the
implementation plan was to build first. 3840 trials, three declared magnitude
bands, N in {1, 3, 4, 6, 8}.

**Against plain linear, the propagated form gets worse as the chain grows.**
Worst observed ratio of propagated error to linear error:

| band | N=1 | N=3 | N=4 | N=6 | N=8 |
|---|---|---|---|---|---|
| benign | 488 | 154 | 3.76e3 | 1.17e3 | 300 |
| mid | 1.27e4 | 2.09e3 | 8.37e3 | 7.52e4 | 1.3e4 |
| rescue | 1.0 | 747 | 1.15e4 | 1.51e4 | **1.52e5** |

The rescue row is the hypothesis inverted: 1.0 at one step, five orders of
magnitude worse at eight. The mechanism is the one the intent names. The
propagated form pays `u*|L|` per step where the linear form pays `u`, so at
|L| ~ 700 that is ~700u a step, and it compounds with N instead of amortizing.

**Propagation does beat per-step materialize/reconvert, and that comparison is
worth little.** Rescue band, zero losses at N = 3, 4, 6, ratios down to
3.6e-16. The reconvert form is far worse than plain linear, so beating it
establishes nothing about whether to build a lattice. Measuring against linear
as well as against reconvert is what turned this result over.

**89-98% of trials at N >= 3 are bit-identical between the two forms**, and
that is measured rather than inferred: the `bitsame` column equals the `tie`
column in all 15 cells.

**The per-step budget, derived before the sweep and never fitted to it, holds.**

    rel err  <=  u * ( |L1| + sum_k |L_{k+1}| )  +  u

Worst observed/bound **0.984** across all 3840 trials, never exceeded.

## Experiment 2 — additive chains. The falsification.

`fadd -> logsumexp` is the rule with a structural argument: a linear addition
can cancel catastrophically, and a summation can lose small terms or leave
range, where a log form does not. Measured as a separate question with its own
pre-registered gate, not as a second attempt at the first one.

11520 trials. Three families chosen where linear addition is classically
worst: `flat` (narrow window), `wide` (spread across the band, the textbook
logsumexp argument), `cancel` (signed, near-cancelling). N in {3, 4, 6, 8, 16}.

**The log side was scored at its best**, the better of a pairwise
`log_add` fold and the runtime's own `pos_accum`/`rp_accum`. H then had to win
at *every* available trial in some family/band cell.

**H is refuted.** No cell qualifies. The band that motivates the whole project
is the worst one:

| family/band | N=3 | N=4 | N=6 | N=8 | N=16 |
|---|---|---|---|---|---|
| flat/rescue, wins of available | 0/188 | 1/198 | 0/187 | 0/201 | 0/183 |
| flat/rescue, best ratio seen | 6.4 | 0.297 | 7.05 | 4.17 | 2.63 |

The log form is never better in flat/rescue at N=3, and its *best* trial there
is 6.4x worse than linear. Worst ratio across the whole experiment is 1.43e8
(wide/mid, N=6).

**The one place the log form is competitive, it decays with N.** wide/mid wins
143 of 256 at N=3, 94 at N=6, and 24 at N=16. Chain length hurts the log form
in the additive vocabulary too.

## Contradiction, stated

The intent argues one genuine accuracy point in log's favour: *"absolute error
in the log domain corresponds to relative error in the linear domain, so a
long log-space accumulation maintains relative accuracy across many terms."*

That is true and it is not a reason to propagate. It is a statement about an
accumulation that is *already* in log form, which is what `pos_accum` and
`rp_accum` deliver and what their contracts bound. It does not say that moving
a computation which linear floating point can already perform into the log
domain improves it. Measured, doing so costs `u*|L|` a step. The runtime's
value stands; the propagation inference from it does not.

## What survives: availability, not accuracy

Both experiments separate this bucket strictly, and neither counts it toward
any gate. A trial where the linear form returns 0, inf, NaN or a subnormal
while the reference is finite is recorded as **unavailable**, never as a
propagation win.

- Experiment 1: 76 of 1280 rescue trials.
- Experiment 2: 55 to 128 of 256 per rescue cell.

That is the whole of propagation's measured value, and it is what
`propagate=div` already delivers for the one shape it covers: a correct finite
answer where the linear path has none. Conflating it with accuracy would have
made both gates tests that could not fail.

## Two harness defects, both of which would have published a wrong result

Recorded because each changed a conclusion, not as process narration.

**The tie column was computed and never printed.** Experiment 1's table read
"propagation rarely wins" when the truth was "the two forms usually agree
exactly". Fixed by printing ties and losses, and by adding a `bitsame` counter
that measures the explanation instead of assuming it.

**The reference was collapsed before the subtraction, and it produced a PASS.**
Experiment 2's first run reported H surviving in flat/rescue, 188 of 188 wins
at N=3. Every winning cell recorded *no ratio at all*, because the log-side
error was exactly 0 on every trial: the reference was collapsed to a double
and then subtracted in double, quantizing every sub-ulp error to zero, while
the linear side kept its subtraction wide. Two sides, two resolutions, and the
favoured one was the side under test. `dd_sum.h` states the rule for
`bound_search` and the reason transfers exactly. With the subtraction formed
in double-double the same cell reads **0 of 188**.

A result that arrives as a PASS on the hypothesis being tested deserves the
harder look, and this one did not survive it.

## Decision

**The compiler-propagation branch stops here.** Both vocabulary rules are
measured and neither wins on accuracy; the lattice, `propagate=mul`, and the
remaining rules are not built. `propagate=div` stays as it is: a labeled
prototype covering one consumer shape, whose value is availability and is
documented as such.

This changes no shipped artifact. `pass/` was already outside 1.0's supported
surface, the header is untouched, and the diagnostic remains the front door.

## What would reopen it

A regime where the log form's `u*|L|` per step is not the dominant cost. The
measurements point at one candidate and it is not a chain: inputs where the
linear form has no answer at all. That is availability, it is already covered
for one shape, and widening it is a question about *coverage*, not about
propagation depth.

Reopening on accuracy grounds needs a counterexample to the tables above,
produced by `ctest -R chain_search` on a machine that disagrees with both
libms tested here.
