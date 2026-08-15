# Matcher hit-rate study — results

*2026-08-15. Answers intent v0.3 success criterion 4 and the Deliverable 2
gate. Method and decision rule were fixed in advance in METHODOLOGY.md; raw
per-loop data is committed under `data/`. Environment: WSL2 Ubuntu,
clang 21.1.6 / LLVM 21.1.8, matcher as an opt plugin.*

## Headline numbers

| codebase | innermost FP loops | hits | hit rate | transcendental | const trip |
|---|---|---|---|---|---|
| GSL 2.8 (659 modules) | 2501 | 753 | 30.1% | 4 | 117 |
| darknet (62 modules) | 283 | 19 | 6.7% | 3 | 2 |
| libsvm (1 module) | 75 | 11 | 14.7% | 0 | 0 |
| **total** | **2859** | **783** | **27.4%** | **7** | — |

*Re-scanned 2026-08-15 after a matcher correction described under "The rule
that excluded the marquee shape" below. Pre-correction totals were 781 hits /
5 transcendental / 3 HIGH; the two recovered hits carry `exp-sum` in their
reasons, so either set is recoverable from `data/`.*

The matcher passed its labeled ground-truth gate (selftest.c: 4 hits / 6 FP
loops / 1 transcendental, exact) before any of these numbers were collected,
and the scan is deterministic across repeat runs.

## Transcendental-chain hits

The five transcendental-chain hits are the target shapes:

- `darknet/src/blas.c:315` — **`softmax_cpu`**: the softmax denominator
  `sum += exp(input[i] - largest)`, named as a target shape in the intent doc.
- `gsl/randist/dirichlet.c:147` — `gsl_ran_dirichlet_pdf` / `_lnpdf`:
  likelihood products via exp — the mixture-likelihood family.
- `gsl/filter/gaussian.c:205` — Gaussian kernel construction.

The plain-chain bulk is dot products, matmul/trmv/gemm inner loops, sums of
squares, covariance triple-products, FFT twiddle accumulation, and special-
function series (Bessel, hypergeometric, Chebyshev-error accumulators).

## Profitability triage

Each HIT now carries a static risk verdict — the gate in front of any
rewrite: "would this reduction actually underflow?" **HIGH** = exp-family
call (exp/expm1/exp2/pow) in the product chain, a factor whose magnitude the
exponent controls; **MED** = deep chain (nmul ≥ 4, many multiplied factors
compounding magnitude) or log-family inputs multiplied together (nmul ≥ 2);
**LOW** otherwise. Per-hit reasons are in `data/`; the selftest gate now also
asserts `mixture_likelihood` triages HIGH with `exp-chain`.

| codebase | hits | HIGH | MED | LOW |
|---|---|---|---|---|
| GSL 2.8 | 753 | 2 | 54 | 697 |
| darknet | 19 | 3 | 0 | 16 |
| libsvm | 11 | 0 | 2 | 9 |
| **total** | **783** | **5** | **56** | **722** |

All HIGH-risk sites:

| site | function | reasons |
|---|---|---|
| darknet `src/blas.c:315` | `softmax` | exp-chain |
| darknet `src/blas.c:315` | `softmax_cpu` | exp-chain |
| darknet `examples/go.c:562` | `pick_move` | exp-chain;exp-sum |
| GSL `filter/gaussian.c:205` | `gsl_filter_gaussian_kernel` | exp-chain;deep-chain |
| GSL `specfunc/zeta.c:757` | `gsl_sf_hzeta_e` | exp-chain;exp-sum |

## The rule that excluded the marquee shape

Found 2026-08-15 while wiring the triage into the rewrite pass, and it
changes how the darknet result should be read.

The matcher required `nMul >= 1` — at least one multiply or divide in the term
chain — on the reasoning that a plain sum is "rescuable without logsumexp".
That is true of `s += x[i]`. It is false of `s += exp(x[i] - max)`, which is
the softmax denominator, the shape the intent names and the shape `pass/`
rewrites. `exp` spans the whole representable range from its argument alone;
no multiply is needed for magnitude to compound.

**darknet's softmax matched by accident.** Its source is
`exp(input[i*stride]/temp - largest/temp)`, and the `/temp` division is what
supplied `nMul = 1`. Two variants compiled side by side, identical underflow
exposure:

| source | matcher verdict |
|---|---|
| `exp(x[i]/temp - largest/temp)` | HIT, HIGH, exp-chain |
| `exp(x[i] - largest)` | no hit at all |

A softmax without temperature scaling was invisible to the matcher, and so
was `pass/test_softmax.c` — the study's own rewrite prototype produced **zero
hits** when scanned.

Fixed by accepting a term whose magnitude an exponent controls regardless of
multiply count (`nMul >= 1 || expChain`), tagged `exp-sum` when it arrives
with no multiply. `plain_sum` stays a labeled miss: no product *and* no
exponent is genuinely out of scope. The selftest gate grew a `softmax_denom`
case and now asserts 5 hits / 7 FP loops / 2 transcendental.

**The headline barely moves; the actionable list grows by two thirds.**
783 hits against 781, 27.4% against 27.3% — the decision rule is untouched.
But HIGH went from 3 sites to 5, and both recoveries are genuine underflow
candidates the old rule dropped silently.

Three rows, one shared source line (darknet's softmax denominator, matched
in two functions). Of 781 shape-hits, the static signal marks two source
sites: the softmax-denominator idiom and a Gaussian-kernel construction.
That count supports a diagnostic flagging a handful of sites over a rewrite
touching hundreds. The MED tier is all deep-chain (libsvm's `svm_train`
4–5-factor products; 54 GSL sites, none log-chain). The `dirichlet.c:147`
pair triages LOW under this rule:
its chain is `(alpha-1)*log(theta)` — a log-domain factor of linearly
bounded magnitude, not an unbounded exp factor (nmul = 1, `log-chain`
recorded but below the MED bar).

## Coverage against the shapes this project names

The `nMul` bug above was found by asking whether the matcher could see a
shape the project claims to target. `coverage.c` holds the shapes named in
the README, the intent, and METHODOLOGY, and `./run_study.sh coverage`
asserts the table below — every expected hit with its risk and reasons, and
every documented miss. It fails if a claim drifts in either direction: a
named shape that stops being seen, or a documented gap that starts hitting
and leaves this section stale.

(That gate was added 2026-08-15, after this section had been published for a
day describing `coverage.c` as a standing check when nothing ran it.)

| named target | matcher sees it | verdict |
|---|---|---|
| mixture likelihood `w[i]*exp(logp[i])` | yes | HIGH exp-chain |
| softmax denominator, textbook form | yes | HIGH exp-chain;exp-sum |
| forward algorithm, register accumulator | yes | **LOW** |
| forward algorithm, `out[j] += ...` | **no** | rejected at the mid-loop-read guard |
| hand-written logsumexp | yes | HIGH exp-chain;exp-sum |
| kernel / weighted sum (libsvm family) | yes | LOW |
| product of likelihoods | no — correct, exponent-tracking's job | — |

Two gaps, and the second is not the documented one.

**The forward algorithm as usually written is invisible, and the reason is
not the one first published here.** An earlier version of this section
attributed it to the memory-carried blind spot. That was wrong. `out[j] +=
...` does not leave the accumulator in memory: LLVM promotes it to a register
phi exactly as in the register-accumulator version, and keeps a store
mirroring it back to `out[j]` on every iteration.

The shape reaches the matcher and is rejected by the **mid-loop-read guard** —
the update then has two in-loop users, the phi and that store, so `cleanUses`
fails. Reproduce with the matcher's explain mode, which names the check that
turned each candidate away:

```
opt-21 -load-pass-plugin=SopMatcher.so -passes='sop-matcher<explain>' \
       -disable-output coverage.bc
```

```
REJECT,cleanUses,coverage.c,44,forward_step_mem,store-of-spine:invariant-addr
REJECT,cleanUses,selftest.c,66,midread,store-of-spine:varying-addr
```

The guard is right to exist — `midread` is a prefix sum whose intermediate
values are genuinely observed — but it currently cannot tell that case from an
accumulator merely mirrored to a fixed cell. A differential set isolates what
does and does not trip it: a loop containing a store that does *not* store the
accumulator still hits, so the guard keys on extra in-loop users of the spine
value, and among those, `isLoopInvariant` on the store address separates the
mirroring case from the prefix-sum case. That makes invariance a candidate
condition for refining the guard, tested on four hand-written variants at
`-O1`; it is not evidence that a refined rule is sound in general, and alias
analysis is still required before accepting any such store.

**Refining the guard was measured and declined for v1.** Scanning the same
corpus with `./run_study.sh rejects`, which runs explain mode over the
harvested bitcode and prints this census:

| `cleanUses` rejects | 460 |
|---|---|
| extra user is an invariant-address store — what a refinement would admit | **23 (5.0%)** |
| extra user is a varying-address store — correctly rejected | 8 |
| extra user is a non-store | 135 |
| no extra user on the update; rejected via the phi or another spine node | 294 (64%) |

The 23 are `gsl_spblas_dgemv`, `gsl_eigen_nonsymmv`,
`genv_get_right_eigenvectors`, `cquad`, `steffen_eval_integ`, darknet's
`mean_cpu`, `forward_avgpool_layer`, `backward_batchnorm_layer` and similar —
no `exp` in any chain, so all would grade LOW and `diagnose.sh` would print
them only as a count. Nor would the refinement recover the shape that prompted
it: none of these three codebases contains forward-algorithm code, so
`coverage.c`'s case is synthetic. Decision and revisit conditions in TODO.md.

*Reproducibility.* These numbers were first produced by a throwaway
instrumented build on one machine, which made them unreproducible — the
instrumentation is now committed as the `explain` parameter and the census as
`./run_study.sh rejects`, and re-running it returns 460/23/8/135/294 exactly.
Explain mode is silent when off: the selftest and coverage gates and the
committed `data/raw-*.txt` are byte-identical with and without it.

The count must stay serial. A first version ran `opt` under `xargs -P4`, whose
concurrent writes to one stderr interleaved records and undercounted
(451/20/6/133/291 against 460/23/8/135/294). `run_study.sh` scans serially, so
the published hit counts were never exposed to this; the `rejects` subcommand
also asserts that no emitted line lacks a record tag, which is what
interleaving produces.

**When the matcher can see it, the triage grades it LOW.** The register-
accumulator version is a `nMul = 1` plain product chain with no
transcendental, so the rule returns LOW — and the diagnostic would not flag
it. That is not a rule bug so much as a limit of what one loop can show:
the forward algorithm underflows because probabilities decay across time
steps, and each individual inner reduction looks unremarkable. Static
per-loop risk cannot see a magnitude trend that lives in the outer loop.
Worth stating plainly, because "the diagnostic flags the shapes we care
about" is exactly the claim a reader would assume.

## Audit (per METHODOLOGY.md)

- **Precision** (random hits, source eyeballed): 6/8 confirmed genuine
  sum-of-products reductions; 2 unresolvable by source display because
  inlining attributes the IR to a different file — not established as false
  positives, recorded as unverified.
- **Recall** (random no-hit FP loops): 8/8 correct rejections — elementwise
  updates, multiplicative recurrences (`d = y2*d - dd + c[j]`, which
  log-domain arithmetic could not rescue anyway), and complex in-place BLAS
  solves, which are **memory-carried** reductions.
- **Known blind spots**, stated: memory-carried accumulators (array cell
  updated in place) are out of scope for the v0 matcher; vectorized/unrolled
  forms were deliberately suppressed at compile time (`-fno-vectorize
  -fno-slp-vectorize -fno-unroll-loops`).

A find-phase bug was caught mid-study by the "more permissive matcher
produced fewer hits" contradiction: chain rejection was being reset by
sibling traversal (last-op-wins). All published numbers postdate the fix;
pre-fix scans were discarded.

## Decision (rule fixed in advance: <10 hits or recall <50% → pivot)

**781 hits and clean recall clear the gate. The shape survives real
codebases in recognizable form. The pass prototype proceeds.**

Qualifier the numbers force: shape-abundance is not profitability. The bulk
of hits are benign-range dot products where a log rewrite would only cost
speed. The rescue-worthy subset (transcendental chains, likelihood/softmax
shapes) is 5 sites, and includes the softmax-denominator idiom named in the
intent. The pass needs the profitability/range analysis in front of the
rewrite. Diagnostic-first (flag the 5, not the 781) is the likely shipping
shape.

## Reproduce

```
./run_study.sh selftest                  # gate must pass first
# clone/untar targets into ~/logrange-study/<name>, build via cc-bc.sh:
#   libsvm:  make CXX=cc-bc.sh            (CLANG=clang++)
#   darknet: make CC=cc-bc.sh -k
#   gsl:     ./configure CC=cc-bc.sh && make -k
./run_study.sh <name>                    # scan harvested bitcode
./run_study.sh report
```
