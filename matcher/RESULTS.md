# Matcher hit-rate study — results

*2026-08-15. Answers intent v0.3 success criterion 4 and the Deliverable 2
gate. Method and decision rule were fixed in advance in METHODOLOGY.md; raw
per-loop data is committed under `data/`. Environment: WSL2 Ubuntu,
clang 21.1.6 / LLVM 21.1.8, matcher as an opt plugin.*

## Headline numbers

| codebase | innermost FP loops | hits | hit rate | transcendental | const trip |
|---|---|---|---|---|---|
| GSL 2.8 (659 modules) | 2501 | 783 | 31.3% | 5 | 127 |
| darknet (62 modules) | 283 | 19 | 6.7% | 3 | 2 |
| libsvm (1 module) | 75 | 12 | 16.0% | 0 | 0 |
| **total** | **2859** | **814** | **28.5%** | **8** | — |

*Re-scanned 2026-08-17 after recognition moved from a hand-written spine walk
to LLVM's `RecurrenceDescriptor`. Previous totals were **783 hits / 27.4% /
7 transcendental** (GSL 753, libsvm 11). The denominator, the 5 HIGH findings,
and every risk grade are unchanged; the delta is +32 hits and −1, measured
per-loop and reported in [DELTA.md](DELTA.md) with the paired records in
`data/raw-delta.txt`.*

*Re-scanned 2026-08-15 after the matcher correction described under "The rule
that excluded the marquee shape". Pre-correction totals were 781 hits /
5 transcendental / 3 HIGH; the two recovered hits carry `exp-sum` in their
reasons, so either set is recoverable from `data/`.*

The matcher passed its labeled ground-truth gate before any of these numbers
were collected, and the scan is deterministic across repeat runs. The gate was
`selftest.c` at 4 hits / 6 FP loops / 1 transcendental for the original scan,
and 5 / 7 / 2 for the re-scan above, which is the same corpus plus the
`softmax_denom` case the `nMul` correction admitted.

## Transcendental-chain hits

Eight transcendental-chain hits across six source lines. Two were recovered
by the `nMul` correction below and are marked; one by the recognizer change.

- `darknet/src/blas.c:315`, `softmax` and `softmax_cpu`: the softmax
  denominator `sum += exp(input[i] - largest)`, named as a target shape in
  the intent doc.
- `darknet/examples/go.c:562`, `pick_move`. Recovered by the correction.
- `gsl/randist/dirichlet.c:147`, `gsl_ran_dirichlet_pdf` and `_lnpdf`:
  likelihood products via exp, the mixture-likelihood family.
- `gsl/filter/gaussian.c:205`, Gaussian kernel construction.
- `gsl/specfunc/zeta.c:757`, `gsl_sf_hzeta_e`. Recovered by the correction.
- `gsl/specfunc/ellint.c`, `gsl_sf_ellint_RD_e`: a `sqrt` chain in the
  Carlson elliptic-integral series, graded MED. Recovered by the recognizer
  change — the update is conditionally executed, which the old spine walk
  could not trace past the merge phi.

The plain-chain bulk is dot products, matmul/trmv/gemm inner loops, sums of
squares, covariance triple-products, FFT twiddle accumulation, and special-
function series (Bessel, hypergeometric, Chebyshev-error accumulators).

## Profitability triage

Each HIT carries a static risk verdict, the gate in front of any rewrite:
"would this reduction actually underflow?" **HIGH** = exp-family call
(exp/expm1/exp2/pow) in the product chain, a factor whose magnitude the
exponent controls; **MED** = deep chain (nmul ≥ 4, many multiplied factors
compounding magnitude) or log-family inputs multiplied together (nmul ≥ 2);
**LOW** otherwise. Per-hit reasons are in `data/`; the selftest gate also
asserts `mixture_likelihood` triages HIGH with `exp-chain`.

| codebase | hits | HIGH | MED | LOW |
|---|---|---|---|---|
| GSL 2.8 | 783 | 2 | 55 | 726 |
| darknet | 19 | 3 | 0 | 16 |
| libsvm | 12 | 0 | 2 | 10 |
| **total** | **814** | **5** | **57** | **752** |

All HIGH-risk sites:

| site | function | reasons |
|---|---|---|
| darknet `src/blas.c:315` | `softmax` | exp-chain |
| darknet `src/blas.c:315` | `softmax_cpu` | exp-chain |
| darknet `examples/go.c:562` | `pick_move` | exp-chain;exp-sum |
| GSL `filter/gaussian.c:205` | `gsl_filter_gaussian_kernel` | exp-chain;deep-chain |
| GSL `specfunc/zeta.c:757` | `gsl_sf_hzeta_e` | exp-chain;exp-sum |

## The rule that excluded the marquee shape

Found 2026-08-15 while wiring the triage into the rewrite pass. It changes how
the darknet result should be read.

The matcher required `nMul >= 1`, at least one multiply or divide in the term
chain, on the reasoning that a plain sum is "rescuable without logsumexp".
That is true of `s += x[i]`. It is false of `s += exp(x[i] - max)`, which is
the softmax denominator, the shape the intent names and the shape `pass/`
rewrites. `exp` spans the whole representable range from its argument alone;
no multiply is needed for magnitude to compound.

**darknet's softmax matched by accident.**

Its source is `exp(input[i*stride]/temp - largest/temp)`, and the `/temp`
division is what supplied `nMul = 1`. Two variants compiled side by side,
identical underflow exposure:

| source | matcher verdict |
|---|---|
| `exp(x[i]/temp - largest/temp)` | HIT, HIGH, exp-chain |
| `exp(x[i] - largest)` | no hit at all |

A softmax without temperature scaling was invisible to the matcher, and so
was `pass/test_softmax.c`: the study's own rewrite prototype produced **zero
hits** when scanned.

Fixed by accepting a term whose magnitude an exponent controls regardless of
multiply count (`nMul >= 1 || expChain`), tagged `exp-sum` when it arrives
with no multiply. `plain_sum` stays a labeled miss: no product *and* no
exponent is genuinely out of scope. The selftest gate grew a `softmax_denom`
case and now asserts 5 hits / 7 FP loops / 2 transcendental.

**The headline barely moves; the actionable list grows by two thirds.**

783 hits against 781, 27.4% against 27.3% (the totals as they stood on
2026-08-15; see the headline note for the later recognizer change); the
decision rule is untouched. HIGH went from 3 sites to 5, and both recoveries
are genuine underflow candidates the old rule dropped silently.

Five rows across four source lines; darknet's softmax denominator accounts
for two of them, matched in two functions at the same `blas.c:315`. Of 814
shape hits, the static signal marks four source lines, 0.14% of the 2859
loops examined. That count supports a diagnostic flagging a handful of sites
over a rewrite touching hundreds. The MED tier is all deep-chain (libsvm's
`svm_train` 4–5-factor products; 55 GSL sites, none log-chain). The
`dirichlet.c:147` pair triages LOW under this rule: its chain is
`(alpha-1)*log(theta)`, a log-domain factor of linearly bounded magnitude,
not an unbounded exp factor (nmul = 1, `log-chain` recorded but below the MED
bar).

## Coverage against the shapes this project names

The `nMul` bug above was found by asking whether the matcher could see a
shape the project claims to target. `coverage.c` holds the shapes named in
the README, the intent, and METHODOLOGY, and `./run_study.sh coverage`
asserts the table below: every expected hit with its risk and reasons, and
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
| forward algorithm, `out[j] += ...` | yes (since 2026-08-17) | **LOW** |
| hand-written logsumexp | yes | HIGH exp-chain;exp-sum |
| kernel / weighted sum (libsvm family) | yes | LOW |
| product of likelihoods | no — correct, exponent-tracking's job | — |

One gap remains, and it is the correct one: a pure product belongs to
exponent-tracking, not here. `RecurKind::FMul` is filtered out deliberately.

**The forward algorithm as usually written was invisible until the recognizer
changed, and the reason was never the one first published here.**

The first version of this section attributed it to the memory-carried blind
spot. That was wrong: `out[j] += ...` does not leave the accumulator in
memory — LLVM promotes it to a register phi exactly as in the
register-accumulator version, and keeps a store mirroring it back to `out[j]`
on every iteration. The shape reached the matcher and was rejected by the
hand-written **mid-loop-read guard**, because the update had two in-loop
users: the phi and that store.

Both the guard and the refinement question it raised are now moot.
`RecurrenceDescriptor::isReductionPHI`, given a `ScalarEvolution`, processes
stores to loop-invariant addresses as part of the reduction
(`IntermediateStore`) and distinguishes them from a genuinely observed
running value. `midread` — a prefix sum whose intermediates are read — is
still correctly rejected, and `selftest.c` asserts it.

That closes what was, for two days, this project's most-discussed open
question: whether to refine the guard using `isLoopInvariant` on the store
address. The answer was to stop maintaining a guard the compiler already
implements. The 23-site census that informed the decline is preserved in
[DELTA.md](DELTA.md); 13 of those sites are now hits.

*Rejection causes under the current recognizer*, from `./run_study.sh rejects`
over the same corpus:

| REJECT records | 2069 |
|---|---|
| `not-reduction` — LLVM declines the phi | 1814 |
| `noMulNoExp` — plain sum, deliberately out of scope | 138 |
| `dirtyChain` — an op outside the allowed set in the term chain | 79 |
| `kind` — a reduction, but `FMul`/min/max rather than `FAdd`/`FMulAdd` | 38 |

`not-simplified`, `chain`, and `no-terms` are all zero and the census fails if
any is not: the first would mean the scan pipeline lost its canonicalization,
the others that chain recovery failed. Both are pipeline bugs, not findings.

*Reproducibility.* These numbers were first produced by a throwaway
instrumented build on one machine, which made them unreproducible. The
instrumentation is now committed as the `explain` parameter and the census as
`./run_study.sh rejects`. Explain mode is silent when off: the selftest and
coverage gates and the committed `data/raw-*.txt` are byte-identical with and
without it.

The count must stay serial. A first version ran `opt` under `xargs -P4`, whose
concurrent writes to one stderr interleaved records and undercounted
(451/20/6/133/291 against 460/23/8/135/294). `run_study.sh` scans serially, so
the published hit counts were never exposed to this; the `rejects` subcommand
also asserts that no emitted line lacks a record tag, which is what
interleaving produces.

**When the matcher can see it, the triage grades it LOW.**

The register-accumulator version is a `nMul = 1` plain product chain with no
transcendental, so the rule returns LOW, and the diagnostic would not flag
it. This is a limit of what one loop can show, not a rule bug: the forward
algorithm underflows because probabilities decay across time steps, and each
individual inner reduction looks unremarkable. Static per-loop risk cannot see
a magnitude trend that lives in the outer loop.

## Audit (per METHODOLOGY.md)

- **Precision** (random hits, source eyeballed): 6/8 confirmed genuine
  sum-of-products reductions; 2 unresolvable by source display because
  inlining attributes the IR to a different file. Not established as false
  positives; recorded as unverified.
- **Recall** (random no-hit FP loops): 8/8 correct rejections: elementwise
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

**814 hits and clean recall clear the gate. The shape survives real
codebases in recognizable form. The pass prototype proceeds.**

The gate was cleared on 781 hits before the `nMul` correction, 783 after it,
and 814 after recognition moved to `RecurrenceDescriptor`. The rule was
"fewer than ~10 hits"; none of these three numbers is near it, and the
verdict has never depended on which one is current.

Qualifier the numbers force: shape-abundance is not profitability. The bulk
of hits are benign-range dot products where a log rewrite would only cost
speed. The rescue-worthy subset is 5 HIGH findings on 4 source lines — a
count that has not moved across any of those three scans — and includes the
softmax-denominator idiom named in the intent. The pass needs the
profitability/range analysis in front of the rewrite. Diagnostic-first
(flag the 5, not the 814) is the likely shipping shape.

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
