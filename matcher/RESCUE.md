# Do static risk grades predict rescue-worthy numerical failure?

**Status: pre-registration. No results. No site has been run.**

This file fixes the rules before the counting starts, in the same way
`METHODOLOGY.md` did for the hit-rate study. Everything below is frozen at the
commit that adds it. Results append; they do not edit.

## The proposition

Not "does dynamic failure occur". That is answerable and uninteresting: some
site somewhere loses an answer. The proposition is whether the static tiers
**rank** it.

> HIGH carries a materially higher rate of rescue-worthy failure than MED, and
> MED than LOW.

The diagnostic's whole output is an ordering of which sites deserve human eyes
first (`DIAGNOSTIC.md`: "Risk levels rank static evidence, nothing more"). This
study asks whether that ordering corresponds to anything observable.

The current predicate is shape-based: `expChain` gives HIGH, `deepChain` or
`logChain && nMul >= 2` gives MED, else LOW. `RESULTS.md`'s precision audit
checks that hits are genuine sum-of-products reductions, which audits
**recognition**. Nothing has ever checked **rescue-worthiness**.

## Scope limits, stated before results exist

**The HIGH tier cannot be validated by this study. It can only fail to be
falsified.**

HIGH is 5 rows across 4 source lines, and it is a census of what this corpus
produced, not a sample from a population. Two of the five are the same source
line, `darknet/src/blas.c:315`, matched in `softmax` and `softmax_cpu`. The
effective number of independent HIGH observations is 4, and arguably fewer.

A Fisher exact test on 5/5 against 0/20 returns p = 0.000019, and that number
would be misleading on its own: the test assumes observations sampled
exchangeably from a population, and the HIGH set is neither sampled nor
independent. **A clean R3 result must not be read as establishing that HIGH
predicts failure.** It establishes that HIGH was not caught failing to, on
four independent sites in one corpus.

Generalising the HIGH tier needs sites from outside this corpus. That is a
different study and is not this one.

**What this study can do:** measure the MED and LOW tiers against ground truth
at a stated power, characterise the failure classes, and refute the ordering if
the ordering is wrong.

## Rescue-worthy, defined quantitatively

Two failure kinds, kept separate and never summed. `0 -> finite` and a `1e-12`
relative error are not the same event.

```
range failure     linear is zero or non-finite while the reference is finite
                  and non-zero

accuracy failure  linear relative error exceeds T_site, the pre-registered
                  numerical tolerance for that site's reference calculation

rescue-worthy     (range failure OR accuracy failure)
                  AND the log-reference improves the error by at least 100x
```

**T_site is independent of LogRange, by construction.** Fixed before results,
from these sources in order:

1. **The host library's own declared tolerance.** GSL states this directly: its
   module tests carry `TEST_TOL0`..`TEST_TOL6` (2, 16, 256, 2048, 16384,
   131072, 1048576 times `DBL_EPSILON`), and `gsl_sf_result` carries an `err`
   field. That is the library's statement of what the computation owes.
2. **Otherwise `T_default = 1e-10`** relative, about 4.5e5·u: six significant
   digits lost.

**A tolerance is not an expected value.** This study refuses GSL's expected
values as ground truth, because they encode what GSL believes about its own
outputs. A tolerance is a different object: what error the computation is
permitted. That is what an accuracy threshold needs, and it must not come from
a LogRange figure.

**Why not the runtime's bound.** An earlier draft defined accuracy failure as
exceeding the runtime's published worst-case bound. That is circular: the bound
states what LogRange's algorithm guarantees, not what makes a linear
computation unacceptable, and revising the bound later would retroactively
change what this study counted as a failure. The bound is recorded per row as
an explanatory comparison, never as the threshold that produces the verdict.

## Both severity and frequency

Worst-case alone is satisfiable by a manufactured corner. A site sound across
its meaningful domain with one absurd input is not HIGH.

```
worst_error       worst observed over the declared stress family
failure_fraction  fraction of the family that is rescue-worthy
```

The ranking question is asked of both. A tier that wins on one and loses on the
other has not been shown to predict anything, and R3 says so rather than
reporting the flattering column.

Input sensitivity is a rule, not a metric: one passing input never demotes a
site.

## What "materially higher" means, fixed here

**Test:** one-sided Fisher exact between adjacent tiers, alpha = 0.05,
computed by `matcher/rescue_power.sh`, the same script that produced the table
below. R3 reports the p-value and a rate for every tier.

**The study is only powered to detect a large separation.** At 20 against 20:

| lower tier | smallest higher tier that separates | difference | p |
|---|---|---|---|
| 0/20 | 5/20 | 25 points | 0.0236 |
| 2/20 | 8/20 | 30 points | 0.0324 |
| 4/20 | 10/20 | 30 points | 0.0479 |
| 6/20 | 13/20 | 35 points | 0.0281 |

Derive with `./rescue_power.sh mde 20 20`.

**Consequences, accepted in advance.** A true difference smaller than about 25
to 35 percentage points will not be detected. 3/20 against 6/20 gives p = 0.225
and is **not** a ranking, however much it looks like one. A null result means
"not detected at n = 20", never "no difference exists", and R3 must use that
wording.

Raising n is the only fix and it costs driver work, which is why the sample is
40 and why this limit is declared rather than discovered.

## Threshold sensitivity, reported alongside the headline

The 100x rescue margin and `T_default = 1e-10` are **declared, not derived**.
Both are load-bearing for what counts as rescue-worthy at all.

The shim records raw quantities per trial: linear error, log-reference error,
shipped-runtime error, `T_site`, and the non-finite flags. Classification is
therefore **post-processing**, and re-classifying at other thresholds costs
nothing and requires no re-run.

**R3 must report the tier rates over this grid, not only at the registered
point:**

| parameter | registered | also reported |
|---|---|---|
| rescue margin | 100x | 10x, 1000x |
| `T_default` | 1e-10 | 1e-8, 1e-12 |

The registered values are the headline. The grid is published beside them so
the reader can see how much the verdict depends on the choice. **If the
ordering survives only at the registered point, R3 says that in those words.**

## Ground truth is generated, not borrowed

GSL's module tests are the initial exerciser. Inputs are then perturbed and
swept toward the regime the matcher claims to detect, because a test suite
exercises what its authors thought to check, not the numerical corner a range
lint predicts. The stress family per site is declared before results are read.

## Three quantities, two of which must not be conflated

| quantity | answers |
|---|---|
| linear vs high-precision reference | did the linear path lose the answer |
| **log-reference**: ideal log-domain evaluation, high precision | is the log representation adequate |
| shipped `pos_accum` / `rp_accum` | does this project's implementation recover it |

**"log-reference" is a name, not a shorthand.** An independent high-precision
evaluation in the log domain. Not `pos_accum`, not `rp_accum`, not any shipped
code. The rescue-worthy definition is written against it for that reason.

The primary comparison is the first against the second. The third is reported
beside them and never substituted: a shortfall in `pos_accum` is a fact about
`pos_accum`, not about whether the log representation holds the answer.
`pos_accum` and `rp_accum` are different algorithms and are reported
separately.

The chain study nearly lost this distinction and published a PASS built on it
(`pass/CHAINS.md`, "Two harness defects").

## Sampling frame

The 814 shape hits.

- **HIGH: all 5.** Census, with the limit stated above.
- **MED: 20, LOW: 20**, fixed-seed draw from the committed `data/raw-*.txt`.
- The 40 MED/LOW sites split **50/50 into development and held-out before any
  is looked at**. The held-out half opens only in R4.

## The change rule. All three required.

1. **Reproducible mismatch** across a meaningful set of independently selected
   sites, not one counterexample.
2. **Systematic cause**: a specific static property explains the observed
   failures better than the current rule.
3. **Out-of-sample improvement**: the replacement rule is fixed first, then run
   on the held-out half, and must improve discrimination rather than relocate
   the errors.

**HIGH is not redefined as "dynamic test proved failure."** That collapses the
predictor into the ground truth. The matcher stays a static prediction; this
study validates it.

**A change affecting HIGH cannot be justified from the five HIGH sites alone**,
per the scope limit above. It needs sites from outside this corpus.

## Stopping criterion

Complete when all three are answered:

1. Do HIGH sites carry a materially higher rate of rescue-worthy failure than
   MED and LOW, by the test fixed above?
2. What classes of false positive and false negative remain?
3. Is there a static signal explaining those errors well enough to justify a
   change?

If 1 is yes and 2/3 produce no clean improvement, the matcher does not change,
and that is the published result.

## The freeze

**After first sight of results, none of the following may change:**
instrumentation, thresholds, stress families, sampling, tier assignment, or the
statistical test.

Any change to any of them creates a **new pre-registration**, and the affected
results are re-collected under it. Leaving the grading formally untouched while
tuning input generation toward a preferred answer is the failure mode this
closes. It is a live risk: this project has twice published figures that did
not survive a second look, and once reported a PASS produced entirely by a
measurement artifact.

## Phases

| phase | what | changes the matcher |
|---|---|---|
| R0 | this file | no |
| R1 | the instrument, with both controls | no |
| R2 | drivers and stress inputs | no |
| R3 | run, publish rates and classes | **no, by construction** |
| R4 | rule change, only if all three conditions hold | possibly |

## Reproduce

```
./rescue_power.sh mde 20 20            # the detectability table above
./rescue_power.sh p <a> 20 <c> 20      # any tier comparison
```

Neither needs a corpus, a plugin, or LLVM.
