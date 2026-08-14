# Matcher hit-rate study — results

*2026-08-15. Answers intent v0.3 success criterion 4 and the Deliverable 2
gate. Method and decision rule were fixed in advance in METHODOLOGY.md; raw
per-loop data is committed under `data/`. Environment: WSL2 Ubuntu,
clang 21.1.6 / LLVM 21.1.8, matcher as an opt plugin.*

## Headline numbers

| codebase | innermost FP loops | hits | hit rate | transcendental | const trip |
|---|---|---|---|---|---|
| GSL 2.8 (659 modules) | 2501 | 752 | 30.1% | 3 | 116 |
| darknet (62 modules) | 283 | 18 | 6.4% | 2 | 1 |
| libsvm (1 module) | 75 | 11 | 14.7% | 0 | 0 |
| **total** | **2859** | **781** | **27.3%** | **5** | — |

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
| GSL 2.8 | 752 | 1 | 54 | 697 |
| darknet | 18 | 2 | 0 | 16 |
| libsvm | 11 | 0 | 2 | 9 |
| **total** | **781** | **3** | **56** | **722** |

All HIGH-risk sites:

| site | function | reasons |
|---|---|---|
| darknet `src/blas.c:315` | `softmax` | exp-chain |
| darknet `src/blas.c:315` | `softmax_cpu` | exp-chain |
| GSL `filter/gaussian.c:205` | `gsl_filter_gaussian_kernel` | exp-chain;deep-chain |

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
