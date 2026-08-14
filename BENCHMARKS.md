# LogRange — Benchmark & Accuracy Results

*Runs of 2026-08-15. Answers intent v0.3 First Action step 4 and success
criteria 1–3, and records the cancellation-accuracy investigation that led to
v0.2's compensated `rp_accum`. Raw per-cell data: `bench_results.csv`
(regenerate with `bench_logrange`, Release build; not committed — numbers
below are the record).*

## Provenance

- AMD Ryzen 7 5800X, Windows 11 Home, MSVC 19.44 (`/O2 /fp:precise /W4 /WX`)
- Thread pinned to core 0, HIGH_PRIORITY_CLASS, warmup before every measurement
- Fixed-seed inputs (mt19937_64) — identical binaries see identical data
- Reported per cell: min and median ns/term over 9–31 repetitions, spread = (p90−p10)/median

## Noise floor (gates everything below)

The identical streaming-logsumexp kernel registered twice, n = 10⁴: medians
differed by **0.98%** and **0.27%** across the two recorded runs (per-cell
spreads mostly 0.02–0.2). The predecessor's harness showed 8× swings; this one
supports percent-level claims. **Deltas ≲ 1–2% are not evidence; the ratios
cited below are 1.5×–3.8× and clear the floor comfortably.**

## Success criteria — verdicts

**1. Underflowing mixture returns the right answer where linear returns 0.0 — ✅**
1000 terms, each ~e⁻⁸⁰⁰ (individually below the exp-underflow limit ~e⁻⁷⁴⁵):
linear loop returns exactly `0.0`; `rp_accum` returns log-magnitude
−792.643769699630184, matching the analytic max-shift value **bit-for-bit**.

**2. Overhead vs hand-written logsumexp within noise — ✅, exceeded**
The library is not merely within noise of the hand-rolled streaming-logsumexp
loop; it is faster (ns/term, median, n = 10⁶, v0.2 compensated rp_accum):

| shape | stream_lse (hand-rolled) | pos_accum (fast path) | rp_accum (signed, compensated) |
|---|---|---|---|
| uniform (log_abs ~ N(0,1)) | 23.9 | **6.5** (3.7× faster) | 9.6 (2.5× faster) |
| wide (log_abs ~ U[−600,600]) | 25.7 | **14.5** (1.8× faster) | 17.6 (1.5× faster) |

This is the reference-exponent design doing what it claimed: one `exp` per
term with the `log` deferred to reduction, versus the textbook stream's `exp`
+ `log1p` every term. Ratios are stable from n = 10² through 10⁶.
(Pre-compensation v0.1 rp_accum measured 7.3/14.2 ns/term on these shapes —
compensation costs ~2–3 ns/term and buys the accuracy documented below.)

**3. Exponent-tracking beats this runtime on pure products — ✅, published**
Pure product of lognormal factors, n = 10⁶ (ns/term, median): exponent-tracking
(frexp + int64 counter) **3.3**, log-domain product 7.3, naive linear 0.63
(fastest, but leaves double range around n ~ 10⁵ — by design). Products are
exponent-tracking's territory; this library's case is sums.

## Honest cost (intent: "slower-but-right versus fast-but-meaningless")

Against the naive linear loop on benign inputs (uniform shape, n = 10⁶):
linear 3.4 ns/term, pos_accum 6.5 (1.9×), rp_accum 9.6 (2.8×). That is the
price of finishing with a correct answer in the regime where the linear loop
returns 0.0/inf/NaN. Cancellation-heavy inputs cost rp_accum ~11.4 ns/term.

## Cancellation accuracy: the investigation behind v0.2

The first accuracy run flagged an anomaly: on a heavy-cancellation dataset
(10⁴ near-cancelling pairs plus a small tail, condition number 2.3×10⁹), a
plain sequential `log_add` fold was ~170× more accurate than v0.1 `rp_accum`
(2.8e-8 vs 4.8e-6 relative). Hypothesis: the fold looked good only because
cancelling pairs were *adjacent* — each pair annihilated at matched magnitude
before rounding error could accumulate — while rp_accum accumulates all
positive and all negative mass into two long uncompensated sums whose
accumulated error is amplified by cond at the final subtraction.

The ordering experiment (same data, three orderings, plus a
Neumaier-compensated rp_accum variant) confirmed it:

| ordering | rp_accum v0.1 (uncomp.) | rp_accum compensated | log_add fold |
|---|---|---|---|
| paired (as generated) | 4.8e-6 | 1.6e-8 | 2.8e-8 |
| shuffled | 2.5e-6 | 4.4e-10 | 4.1e-6 |
| separated (+ then −) | 2.3e-6 | 3.7e-8 | 2.3e-7 |

(cond·ε reference level: 5.2e-7.)

Conclusions, now encoded in code and tests (`test_accuracy` scenario 2b):

- **The fold's advantage was an ordering artifact.** Shuffled, it degrades ~150×
  and loses to even uncompensated rp_accum. Not a better algorithm.
- **Uncompensated rp_accum was ordering-insensitive but mediocre** (~cond·10ε).
- **Neumaier compensation wins in every ordering** — up to 5000× better than
  uncompensated, below the cond·ε level, at two additions per term. v0.2
  `rp_accum` ships with compensated pos/neg sums; `pos_accum` stays
  uncompensated because positive-only sums have no cancellation to amplify.

## Accuracy vs double-double reference (v0.2, `test_accuracy`)

| scenario | n | v0.1 observed | v0.2 observed | contract bound |
|---|---|---|---|---|
| long positive sum (rel err) | 10⁶ | 1.5e-14 | **9.3e-16** | 1e-8 |
| heavy cancellation, cond = 2.3e9 (rel err) | 2×10⁴ | 4.8e-6 | **1.6e-8** | 0.47 |
| same data shuffled | 2×10⁴ | — | **4.4e-10** | 0.47 |
| `log_add` fold, paired / shuffled | 2×10⁴ | 2.8e-8 | 2.8e-8 / 4.1e-6 | 0.47 |
| magnitude staircase (log-abs err) | 461 | 0.0 | 0.0 | 1e-12 |
| forced pos==neg resets, k=20 (abs err) | 147 | 1.0e64 | 1.0e64 | 4.4e65 (k·A·ε) |

Long sums are now ε-level and n-independent — the residual error is the
per-term `exp()` rounding, not summation drift. The documented reset contract
holds unchanged.

## Next

Formal statement of the worst-case cancellation bound in the header (the
empirics above say the target shape is cond·O(ε)), then the Deliverable 2
matcher study.
