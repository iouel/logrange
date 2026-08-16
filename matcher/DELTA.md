# Recognizer delta — hand-written spine walk vs `RecurrenceDescriptor`

*2026-08-17. Measured on the same three codebases and the same harvested
bitcode as [RESULTS.md](RESULTS.md), with both recognizers in one pass
(`sop-matcher<diff>`) so the corpus is compiled once and the comparison is
paired per loop.*

The matcher's recognition half — "is this loop a floating-point reduction" —
was reimplemented in this repository while LLVM already ships
`RecurrenceDescriptor`, the analysis the loop vectorizer trusts. This file is
the measurement taken before deleting the local copy.

Everything downstream of recognition (`walkChain`, the sum-of-products gate,
the HIGH/MED/LOW triage) is shared by both paths and was not touched. That is
what makes a divergence interpretable: **every difference below is a
recognition difference.** Zero `both-differ` records were produced — wherever
both recognizers matched a loop, they graded it identically.

## Pipeline change

`RecurrenceDescriptor::AddReductionVar` has two preconditions that are not
stated in `IVDescriptors.h`. Both were found by measurement, not by reading:

| Precondition | How it surfaced | Consequence if absent |
|---|---|---|
| `loop-simplify` form | **Segfault** in `AddReductionVar` | It reads the start value via `Phi->getIncomingValueForBlock(L->getLoopPreheader())` with no null check. With assertions off that is an out-of-bounds read, not a diagnostic. |
| LCSSA form | 28 spurious rejections | It inspects out-of-loop users to identify the exit instruction. Without LCSSA it declined 28 reductions on this corpus that it accepts with it. |

Both scan pipelines now run `loop-simplify,lcssa` ahead of the matcher — the
canonicalization a real mid-pipeline pass would already have.

**This is a no-op for the legacy recognizer.** With the canonicalization added,
its output over all three codebases is byte-identical to the committed
`data/raw-darknet.txt`, `data/raw-gsl.txt`, and `data/raw-libsvm.txt`. The
denominator does not move: 2859 loops examined either way.

## Headline

| | loops | hits | HIGH | MED | LOW |
|---|---|---|---|---|---|
| legacy (published) | 2859 | 783 | 5 | 56 | 722 |
| `RecurrenceDescriptor` | 2859 | **814** | **5** | 57 | 752 |

Per codebase:

| | legacy loops/hits | LLVM loops/hits | net |
|---|---|---|---|
| darknet | 283 / 19 | 283 / 19 | 0 |
| GSL | 2501 / 753 | 2501 / 783 | +30 |
| libsvm | 75 / 11 | 75 / 12 | +1 |

**The five HIGH findings do not move.** Same five sites, same `nMul` values as
`data/FIGURES.txt`: `go.c:562 pick_move` (0), `blas.c:315 softmax` (1),
`blas.c:315 softmax_cpu` (1), `gaussian.c:205 gsl_filter_gaussian_kernel` (4),
`zeta.c:757 gsl_sf_hzeta_e` (0). Every gain and the single loss is LOW or MED.
darknet produced no divergence records at all.

## Divergence census

| verdict | count |
|---|---|
| `llvm-only` (LLVM matched, legacy did not) | 32 |
| `legacy-only` (legacy matched, LLVM did not) | 1 |
| `both-differ` (both matched, different grade) | **0** |
| `agree-recovered` (see *Chain recovery*) | 63 |

### The 32 gains

Two causes, both previously-documented misses:

**19 were rejected by the legacy spine walk (`spine`).** These are
*conditionally-executed* reductions — `if (cond) acc += term;`. The loop
header phi's backedge value is then a merge phi, and `spineToPhi` returns
false the moment it reaches a `PHI`. Handling this is routine for the
vectorizer. 14 of the 19 are the GSL weighted-statistics family
(`gsl_stats_wvariance`, `wsd`, `wtss`, and `_m`/`float`/`long_double`
variants); the rest are `coupling.c`, `discrete.c`, `ellint.c`, and libsvm's
`Solver::calculate_rho`.

The weighted-statistics gain matters beyond its count: weighted spines are
exactly what the `WEIGHT` census exists to inspect, and the census had never
seen these loops.

**13 were rejected by the mid-loop-read guard (`cleanUses`).** These are
memory-carried accumulators, listed as a known blind spot in
[RESULTS.md](RESULTS.md) and `TODO.md`. `isReductionPHI` with a non-null
`ScalarEvolution` processes stores to loop-invariant addresses
(`IntermediateStore`), so the shape is recognized rather than refused. 11 are
in `cquad.c`; the others are `rksubs.c` and `spdgemv.c` — the latter a sparse
matrix-vector product, the canonical memory-carried sum of products.

The same mechanism flips `forward_step_mem` in `coverage.c` from a documented
`expect_miss` to a hit. That fixture is the textbook forward algorithm,
`out[j] += prev[i] * A[i*n+j]`, and it is the shape `logrange_intent.md` names
in its motivating problem. **This closes a stated blind spot**, and
`coverage.c`'s expectation table must be updated to record the closure rather
than re-pin the miss.

### The single loss

`coulomb.c:403` in `coulomb_FG0_series`, graded MED
(`deep-chain;unknown-trip`, `plain`).

The accumulator is `u_sum_err += 2.0 * GSL_DBL_EPSILON * abs_du` — a clean
sum of products, never read inside the loop, which is why the legacy guard
passed it. The enclosing loop carries a data-dependent convergence test ending
in `break`, so it has multiple exiting blocks and the reduction has no single
exit value. `AddReductionVar` declines rather than commit to one.

That is the conservative direction: LLVM refuses a loop whose partial sum
escapes by more than one path. For a lint the legacy acceptance was defensible;
for anything that rewrites, declining is correct. Recorded as a deliberate
loss, not a regression to fix.

### Chain recovery

`getReductionOpChain` declined to order the chain 63 times (61 GSL, 2 libsvm).
It answers a vectorizer question — can these operations be treated as in-loop
reduction steps — which is stricter than what the matcher needs. In every one
of those 63 cases `isReductionPHI` had already said yes, a generic cycle walk
recovered the term set, and the resulting verdict agreed with legacy.

So the fallback is load-bearing (63 hits depend on it) and never disagreed
with the hand-written walk where both ran.

## Negative control

The GSL weighted-variance file contains two superficially similar loops:

```c
wtss     += wi * delta * delta;                    /* sum of products     */
wvariance += (delta * delta - wvariance) * (wi/W); /* Welford: NOT a sum  */
```

The second reads the accumulator inside its own term and scales the running
value each iteration; a log-domain rewrite of it changes the answer. Compiled
at the study flags, `RecurrenceDescriptor` accepts the first and **rejects the
second**. The 19 spine-class gains are genuine sum-of-products reductions, not
the Welford update sitting beside them in the same file.

## What this measured, in one line

Delegating recognition costs one MED finding in a multi-exit loop, gains 31
across two documented blind spots, leaves all five HIGH findings and the
2859-loop denominator untouched, and never changes a risk grade.

## Reproduce

```bash
cd matcher && ./run_study.sh delta
```

Every number above is recomputed from `data/raw-delta.txt` and the headline is
asserted, so this file and its evidence cannot drift apart. Like
`./run_study.sh figures`, it reads only committed text — no corpus checkout,
no bitcode, no plugin build — so it runs in CI and a reader can check the
claims without reproducing the study.

**The measurement itself is not re-runnable, by design.** It required both
recognizers in one binary, and the hand-written one was deleted once this
comparison was published — that deletion being the point of the exercise. What
is committed instead is its paired output: 2859 `LOOP` records, the 783 `HIT`
records the old recognizer produced, and 96 `DIFF` records. That is enough to
recompute both columns and every cause breakdown. To re-derive it from source,
check out the commit that added this file, where both recognizers and
`sop-matcher<diff>` still exist.
