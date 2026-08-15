# Matcher hit-rate study — methodology

*Intent v0.3, Deliverable 2 precondition: "Hit rate is measured before the
rewrite is built. Write the matcher only, run it over real numeric codebases,
count." This file fixes the rules before the counting starts, so the numbers
cannot be adjusted after the fact.*

## What counts as a hit

An innermost natural loop that computes a **floating-point sum-of-products
reduction**, i.e. all of:

1. A loop-carried `phi` of floating-point type (the accumulator) with exactly
   one in-loop update.
2. The update is `fadd` (or `fsub`) whose other operand chains through at
   least one `fmul` (possibly via intermediate `fadd`/`fsub`/`fneg`/casts),
   with all chain operations inside the loop. (`total += a*b`, `total +=
   a*b*c`, `total += x[i]*y[i] + z[i]*w[i]` all count; `total += x[i]` alone
   does not: that is a plain sum, rescuable without logsumexp.)
3. The accumulator's only in-loop consumers are the update chain itself
   (no mid-loop reads that would change semantics under rewriting).
4. No calls with side effects inside the loop body along the reduction path
   (readnone/readonly intrinsics like `llvm.fmuladd`, `fabs`, `exp`, `log`
   are allowed and recorded; `exp` in the product chain is a strong signal
   of the likelihood-style computation this project targets).

Recorded per hit: function name, source location (from debug info),
trip-count kind (constant / runtime / unknown), the chain depth, and whether
the product chain contains transcendental calls.

## What is deliberately NOT required

- **Reassociation legality.** The matcher fires on shape alone. The pass
  would require explicit fast-math/pragma opt-in (intent: legality is the
  caller's grant, not the matcher's inference); measuring how many shape-hits
  additionally carry `reassoc` flags in shipped builds is a separate column,
  not a bar for counting.
- **Profitability.** No range analysis here. Hit rate answers the go/no-go
  question, "does the shape survive real codebases in recognizable form?",
  not "is each hit worth rewriting?".

## Pipeline

1. Compile each target to bitcode with clang at `-O1 -g` with
   `-fno-vectorize -fno-slp-vectorize -fno-unroll-loops`
   (`-O1` gets mem2reg/instcombine canonicalization the matcher relies on,
   while the disables keep reductions in scalar recognizable form, the same
   position a mid-pipeline pass would occupy; vectorized/unrolled forms are
   recorded as a known blind spot rather than chased in v0).
2. Run the matcher as an `opt` plugin over every module; aggregate counts.
3. Report per codebase: total innermost FP loops examined, hits, hits with
   transcendental chains, hits with constant trip counts, and misses that a
   human audit of a random sample says *should* have hit (matcher recall
   check, sample of 20).

## Target codebases (three, per intent success criterion 4)

Chosen for: plain C/C++, builds with clang out of the box, genuinely numeric,
and containing the likelihood/kernel/softmax shapes this project targets.

1. **GSL** (GNU Scientific Library): statistics, randist, linalg. Classic
   hand-written numeric C.
2. **libsvm**: kernel evaluations are sum-of-products over feature vectors;
   probability outputs involve log-domain code already.
3. **darknet**: softmax/loss/convolution loops in plain C; the
   softmax-denominator shape is success-criterion territory.

(If one fails to build to bitcode cleanly inside the WSL environment, the
fallback pool is: libpng's filter heuristics (rejected: integer-dominated),
Stan math (rejected: header-template-heavy, slow IR), NumPy core loops.)

## Decision rule (fixed in advance)

Per intent: if recognizable hits are widespread, the pass proceeds to a
prototype; if hits are rare or mangled beyond recognition, the project pivots
to the diagnostic lint and the runtime stands alone. Concrete line: fewer
than ~10 genuine hits across all three codebases, or recall below ~50% on
the audit sample, argues for the pivot. The numbers get published either way.
