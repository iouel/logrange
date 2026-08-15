# diagnose.sh — the diagnostic lint

*Intent v0.3 "Fallback Product": a static lint that says "this reduction may
leave representable range — consider log-domain accumulation, here is the
header." It turns raw matcher scan output into a human report and a CI
signal. Zero runtime overhead.*

## Producing input

Two steps, both in WSL/Linux (details in METHODOLOGY.md and RESULTS.md):

1. Build a target to bitcode via the wrapper: `make CC=cc-bc.sh` (or
   `CXX=cc-bc.sh` / `./configure CC=cc-bc.sh`), after `./run_study.sh
   selftest` has passed the gate.
2. `./run_study.sh <name>` scans the harvested bitcode and writes the raw
   file (`data/raw-<name>.txt`) that this tool consumes.

## Usage

```
./diagnose.sh [--all] <raw-file>...
```

Findings are grouped HIGH → MED → LOW, one block per site: location,
function, a plain-English reason built from the matcher's tokens, and
trip-count/chain context. LOW findings (shape match, no range signal) are
summarized as a count unless `--all` is passed.

Exit codes:

- `0` — no HIGH findings (MED/LOW may exist)
- `1` — at least one HIGH finding (so the tool can gate CI)
- `2` — usage error: no input, unknown option, unreadable file

Raw lines other than `LOOP,...` / `HIT,...` records are ignored. HIT lines
in the old 8-column format (no risk/reasons columns) are skipped with a
warning rather than misread; regenerate the scan instead.

## Scope limits

- **Selective by design.** Across the study corpus (GSL 2.8, darknet,
  libsvm), 2859 innermost FP loops produced 783 shape hits (27.4%) and
  **5 HIGH findings on 4 source lines** (0.17% of loops). One HIGH finding in
  a large codebase is the expected outcome, not a sign the scan failed. The
  bulk of the shape hits are benign-range dot products, summarized as a LOW
  count.
- **Two of the three shapes the README names as motivating this project are
  covered.** Mixture likelihood and the softmax denominator triage HIGH. The
  forward algorithm does not, in either form it is usually written: see the
  two bullets below and RESULTS.md, "Coverage against the shapes this project
  names". `coverage.c` plus `./run_study.sh coverage` assert that table in
  both directions, so the gap cannot silently close or widen.
- **This is a source-shape lint, not a range proof.** It reports that a
  reduction has the sum-of-products shape plus static risk signals
  (exp/log in the chain, deep factor chains, unbounded trip counts). It
  does not know the input data; a HIGH site can be benign in practice and
  a LOW site can overflow on adversarial inputs. FPChecker-style runtime
  instrumentation occupies the dynamic-detection niche; this tool is the
  cheap static complement.
- **Memory-carried reductions are not covered.** An accumulator that stays in
  memory never reaches the matcher (METHODOLOGY.md, known blind spots) and so
  never reaches this report.
- **Nor are reductions mirrored to a fixed cell**, for a different reason.
  `out[j] += ...` *does* get promoted to a register accumulator, but LLVM
  keeps a store writing it back each iteration, and the mid-loop-read guard
  rejects any update with a second in-loop user. The forward algorithm, one
  of the three shapes the README names as motivating this project, is usually
  written that way and goes unreported (RESULTS.md, "Coverage against the
  shapes this project names").

  Admitting that case was considered and declined for v1 on measured yield:
  23 of 460 such rejects across the study corpus (5.0%), every one a
  linear-algebra or mean reduction with no `exp` in its chain, hence LOW and
  summarized as a count rather than reported. Revisit only alongside
  alias/dependence analysis and cross-loop risk modelling; the latter is what
  would make these sites worth printing (TODO.md).
- **Risk is judged one loop at a time.** A reduction whose magnitude decays
  across an *enclosing* loop (the forward algorithm again, probabilities
  shrinking over time steps) has unremarkable inner iterations and grades
  LOW. This lint will not flag it even when the matcher can see it.
- **Vectorized/unrolled loops are not covered.** The study pipeline
  deliberately compiles with `-fno-vectorize -fno-slp-vectorize
  -fno-unroll-loops`; scans of ordinary optimized builds will miss
  reductions the vectorizer has already restructured.
- Risk levels rank static evidence, nothing more: they are an ordering of
  which sites deserve human eyes first, not probabilities.

## The fix it points at

`include/logrange/log_math.h`: `pos_accum` for positive-term sums,
`rp_accum` for signed sums, both with stated worst-case error bounds
(header contract). BENCHMARKS.md has the cost numbers.
