# LogRange — Benchmark & Accuracy Results

*First full run, 2026-08-15. Answers intent v0.3 First Action step 4 and success
criteria 1–3. Raw per-cell data: `bench_results.csv` (regenerate with
`bench_logrange`, Release build; not committed — numbers below are the record).*

## Provenance

- AMD Ryzen 7 5800X, Windows 11 Home, MSVC 19.44 (`/O2 /fp:precise /W4 /WX`)
- Thread pinned to core 0, HIGH_PRIORITY_CLASS, warmup before every measurement
- Fixed-seed inputs (mt19937_64) — identical binaries see identical data
- Reported per cell: min and median ns/term over 9–31 repetitions, spread = (p90−p10)/median

## Noise floor (gates everything below)

The identical streaming-logsumexp kernel registered twice, n = 10⁴:
medians differ by **0.98%** (per-cell spreads mostly 0.02–0.2, worst ~0.6 on
small-n cells). The predecessor's harness showed 8× swings; this one supports
percent-level claims. **Deltas ≲ 1–2% are not evidence; the ratios cited below
are 1.9×–4× and clear the floor comfortably.**

## Success criteria — verdicts

**1. Underflowing mixture returns the right answer where linear returns 0.0 — ✅**
1000 terms, each ~e⁻⁸⁰⁰ (individually below the exp-underflow limit ~e⁻⁷⁴⁵):
linear loop returns exactly `0.0`; `rp_accum` returns log-magnitude
−792.643769699630184, matching the analytic max-shift value **bit-for-bit**.

**2. Overhead vs hand-written logsumexp within noise — ✅, exceeded**
The library is not merely within noise of the hand-rolled streaming-logsumexp
loop; it is substantially faster (ns/term, median, n = 10⁶):

| shape | stream_lse (hand-rolled) | pos_accum (fast path) | rp_accum (signed) |
|---|---|---|---|
| uniform (log_abs ~ N(0,1)) | 24.7 | **6.1** (4.1× faster) | 7.3 (3.4× faster) |
| wide (log_abs ~ U[−600,600]) | 24.9 | **13.3** (1.9× faster) | 14.2 (1.8× faster) |

This is the reference-exponent design doing what it claimed: one `exp` per term
with the `log` deferred to reduction, versus the textbook stream's `exp` +
`log1p` every term. The wide shape narrows the gap because new-maximum rescales
force extra work; it still wins. Ratios are stable from n = 10² through 10⁶.

**3. Exponent-tracking beats this runtime on pure products — ✅, published**
Pure product of lognormal factors, n = 10⁶ (ns/term, median): exponent-tracking
(frexp + int64 counter) **3.3**, log-domain product 7.3, naive linear 0.63
(fastest, but leaves double range around n ~ 10⁵ — by design). Products are
exponent-tracking's territory; this library's case is sums.

## Honest cost (intent: "slower-but-right versus fast-but-meaningless")

Against the naive linear loop on benign inputs (uniform shape, n = 10⁶):
linear 3.4 ns/term, pos_accum 6.1 (1.8×), rp_accum 7.3 (2.1×). That is the
price of finishing with a correct answer in the regime where linear returns
0.0/inf/NaN. Cancellation-heavy inputs cost rp_accum nothing extra (7.0 ns/term).

## Accuracy vs double-double reference (`test_accuracy`)

| scenario | n | observed | contract bound |
|---|---|---|---|
| long positive sum (rel err) | 10⁶ | 1.5e-14 | 1e-8 |
| heavy cancellation, cond = 2.3e9 (rel err) | 2×10⁴ | 4.8e-6 | 0.47 |
| same data, sequential `log_add` fold | 2×10⁴ | 2.8e-8 | 0.47 |
| magnitude staircase (log-abs err) | 461 | 0.0 | 1e-12 |
| forced pos==neg resets, k=20 (abs err) | 147 | 1.0e64 | 4.4e65 (k·A·eps) |

Long sums behave like ~√n·ε, far under the O(n·ε) worst case. The documented
reset contract holds with ~44× margin.

**Open question for the error-bound work:** on heavy cancellation, rp_accum's
error is ~cond·10ε — respectable — but a plain `log_add` fold on identical data
was **~170× more accurate** (2.8e-8 vs 4.8e-6). One dataset, one shape, and the
fold costs more per term; still, this is the intent doc's "the seed may
mislead" risk with a number attached. The analytical bound work (next step)
must either explain the gap, close it (Kahan-compensated pos/neg is the obvious
candidate), or document when to prefer which primitive.

## Next

Per intent: formalize the worst-case cancellation bound in the header, decide
on compensated partial sums, then the Deliverable 2 matcher study.
