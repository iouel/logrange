# Cross-loop feedback — measured, and declined as a risk signal

*Answers the open item "Per-loop risk cannot see cross-loop decay"
(`TODO.md`). The rule was built, fixed in advance, measured on the study
corpus, and **not adopted**. This file is the measurement and the reason.*

## The question

Risk is graded one loop at a time. The forward algorithm therefore grades LOW:
each inner reduction is an unremarkable `nMul=1` product sum, while the decay
that makes the family underflow lives in the enclosing time-step loop. One of
the three shapes the README names as motivating this project is seen and not
flagged.

## The rule, fixed before counting

For an innermost reduction whose loop has a parent: **the result is stored
into an object that the reduction's own terms load from**, across the parent.

```
for (t)                          <- parent
  for (j)
    for (i) s += buf[i]*A[..];   <- terms load buf
    out[j] = s;                  <- ... and the result lands in buf next round
```

Underlying-object identity is the proxy; full alias/dependence analysis stays
out of scope, as it did when the mid-loop-read guard refinement was declined.
Buffer rotation is resolved through phis, so the textbook two-buffer swap is
detected and not just the single-array form — `forward_full_swap` in
`coverage.c` exists to keep an identity-only rule from looking finished, and
the first version of the rule failed it.

Reproduce: `./run_study.sh xloop`. Evidence: `data/raw-xloop.txt`.

## What the corpus contains

| | |
|---|---|
| hits | 814 |
| carrying cross-loop feedback | **231 (28%)** |
| of those graded HIGH | 0 |
| graded MED | 8 |
| graded LOW | 223 |
| of those transcendental | **0** |

By codebase: GSL 229, darknet 2, libsvm 0.

What they are:

| | |
|---|---|
| `cblas_*` triangular routines (`ztrmm`, `ctrsv`, `ztpmv`, …) | 192 |
| `gsl_integration_cquad` | 11 |
| FFT transforms (`gsl_fft_real*`, `halfcomplex*`) | 16 |
| Householder / QR (`HH_svx`, `householder_mh`, `qr_solve`) | 6 |
| everything else | 6 |

**Not one is a decaying recursion.** They are in-place linear algebra: a
triangular solve reads and writes the same buffer across its outer loop, and
so does an FFT butterfly and a Householder reflection.

## Why it was declined

**The rule detects feedback. Feedback is the structural precondition for
decay, not evidence of it.** A power iteration that renormalises every step
feeds back and does not decay; a triangular solve feeds back and has no
particular underflow tendency. The static proxy cannot tell those from the
forward algorithm, and both candidate promotions fail on the numbers:

- **LOW → HIGH** takes HIGH from 5 to **236**. `DIAGNOSTIC.md` advertises
  selectivity as the design property — "one HIGH finding in a large codebase
  is the expected outcome". A 47x increase, 83% of it BLAS, refutes that claim
  rather than extending it.
- **LOW → MED** takes MED from 57 to **280**, and `diagnose.sh` prints MED
  sites individually where LOW is summarised as a count. A GSL report would
  grow from 57 printed blocks to 280, mostly triangular solves.

A narrowing was looked for and not found at this cost tier. The forward
algorithm reads the *whole* fed-back buffer per output element where BLAS
reads a slice, but separating those needs SCEV analysis of access ranges,
which is well outside the cheap-proxy scope this project set for exactly this
question.

## What ships

The census, not the signal. `sop-matcher<xloop>` emits `XLOOP` records;
nothing in the risk grading reads `crossLoop`, and the `HIT` stream is
byte-identical to the committed `data/raw-*.txt` with the census on or off.
No published figure moves.

`coverage.c`'s `forward_full_flat` and `forward_full_swap` stay asserted at
**LOW** — the verdict a reader does not want — so the gap remains stated where
it is met rather than described in prose.

## Revisit condition

A signal that separates decay from feedback. The natural candidate is a
magnitude-trend argument over the parent loop's induction, which is a
different and much larger analysis than the one measured here. Until then the
docs branch stands: the diagnostic states that per-loop risk cannot see
cross-loop decay, and the README does not claim the forward algorithm is
flagged.
