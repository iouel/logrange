# LogRange — Benchmark & Accuracy Results

*Runs of 2026-08-15. Answers intent v0.3 First Action step 4 and success
criteria 1–3, and records the cancellation-accuracy investigation behind
v0.2's compensated `rp_accum`. Raw per-cell data: `bench_results.csv`,
regenerated with `bench_logrange`, Release build. It is not committed; the
numbers below are the record.*

## Provenance

- AMD Ryzen 7 5800X, Windows 11 Home, MSVC 19.44 (`/O2 /fp:precise /W4 /WX`)
- Thread pinned to core 0, HIGH_PRIORITY_CLASS, warmup before every measurement
- Fixed-seed inputs (mt19937_64): identical binaries see identical data
- Reported per cell: min and median ns/term over 9–31 repetitions, spread = (p90−p10)/median

## Noise floor (gates everything below)

**0.98%** and **0.27%**: the median gap between the identical
streaming-logsumexp kernel registered twice, n = 10⁴, across the two recorded
runs (per-cell spreads mostly 0.02–0.2). The predecessor's harness showed 8×
swings; this one supports percent-level claims. **Deltas ≲ 1–2% are not
evidence; the ratios cited below are 1.5×–3.8× and clear the floor.**

## Re-run of 2026-08-15 (same machine), and what the noise floor does not cover

Confirmation run after the packaging and contract work. Code paths are
unchanged since the numbers above: every edit to `log_math.h` since v0.2 is
preprocessor or comment (version macros, the fast-math guard), the bench
source is untouched, and the flag set is identical: the same four flags,
moved from directory scope onto a target.

**Ratios reproduce.**

stream_lse/pos_accum 3.75× and 3.70× across two runs against the 3.7×
published; stream_lse/rp_accum 2.56× and 2.54× against 2.5×; wide 1.81× and
1.80× against 1.8×. The success-criteria claims stand.

**Absolute ns/term move more than the floor suggests.**

Medians at n = 10⁶ varied up to ~8% between runs on this machine (wide
`rp_accum` 16.04 then 17.31; uniform `pos_accum` 6.45 then 6.92), while the
harness reported floors of 2.35%, 0.15% and 1.11% in those same runs.

**Scope limit, not a contradiction.**

The noise floor measures two identical kernels inside one run. It says nothing
about run-to-run reproducibility, where machine state enters. "Deltas ≲1–2%
are not evidence" holds for comparisons *within* a run; a comparison against a
number recorded on another day needs a wider envelope, about ±8% on this
hardware. The ratios are the durable claim; the absolutes are a record of one
run.

## Second machine (2026-08-15): the margin is hardware-dependent

Run `31865095928` (`.github/workflows/bench.yml`, on demand only). The
windows-latest leg is the comparable one: the harness pins and prioritizes on
Windows, and MSVC with the same flags matches how the numbers above were
taken, so hardware is the only variable that moves. Its self-reported noise
floor was **0.00%** (identical kernels, spreads 0.010/0.013), better than the
author's machine manages.

| n = 10⁶, median ns/term | Ryzen 5800X | GitHub windows-latest |
|---|---|---|
| uniform `stream_lse` | 23.9 | 35.56 |
| uniform `pos_accum` | 6.5 | 6.75 |
| uniform `rp_accum` | 9.6 | 11.64 |
| wide `stream_lse` | 25.7 | 36.50 |
| wide `pos_accum` | 14.5 | 18.15 |

**Criterion 1 reproduces exactly.**

Both machines, and the unpinned Linux leg too, return log-magnitude
−792.643769699630184 with `|error| = 0.000e+00` against the analytic
max-shift value. Bit-identical on three configurations.

**Criterion 2 holds, by a wider margin.**

The library beats hand-rolled streaming logsumexp on both machines, but not by
the same factor: uniform `stream_lse`/`pos_accum` is 3.7× on the Ryzen and
**5.27×** on the runner; wide is 1.8× against 2.01×.

The published 3.7× is therefore a property of that Ryzen, not a portable
constant. Mechanism: `pos_accum` barely moved between machines (6.5 → 6.75,
+4%) while `stream_lse` slowed by half (23.9 → 35.56, +49%). `pos_accum` pays
one `exp` per term; the textbook stream pays `exp` + `log1p`. Hardware with
relatively slower transcendentals punishes the stream twice, so the design's
advantage grows there. The durable claim is the direction and the reason, with
an observed margin of **1.5×–5.3×** across the eight measured cells: two
machines, uniform and wide shapes, `pos_accum` and `rp_accum`. The extremes
are wide `rp_accum` on the Ryzen (25.7/17.6) and uniform `pos_accum` on the
runner (35.56/6.75).

**Criterion 3 reproduces.**

Exponent-tracking still wins pure products: 4.73 vs 10.68 ns/term on the
runner (2.26×), against 3.3 vs 7.3 (2.2×) here.

The unpinned ubuntu leg ran too and labels itself `pinned core + high
priority: NO — numbers suspect`. Its floor was 0.10%, but it is a different
compiler on an unpinned shared vCPU, so it is recorded rather than compared:
uniform 25.37 / 5.20 / 6.75 for stream_lse / pos_accum / rp_accum.

## Success criteria — verdicts

**1. Underflowing mixture returns the right answer where linear returns 0.0: PASS**

1000 terms, each ~e⁻⁸⁰⁰ (individually below the exp-underflow limit ~e⁻⁷⁴⁵):
linear loop returns exactly `0.0`; `rp_accum` returns log-magnitude
−792.643769699630184, matching the analytic max-shift value **bit-for-bit**.

Read that as agreement to representation granularity, not to 16 digits. At
log-magnitude ≈ 792 one ulp is ~1.1e-13, so landing on the same double means
the two computations agree to ~512u relative in linear terms, the precision
floor documented at `log_value`. It is the correct answer to the last bit
`log_value` has; it is not a claim of 16-digit agreement.

**2. Overhead vs hand-written logsumexp within noise: PASS, exceeded**

The library is faster than the hand-rolled streaming-logsumexp loop, not
merely within noise of it (ns/term, median, n = 10⁶, v0.2 compensated
rp_accum):

| shape | stream_lse (hand-rolled) | pos_accum (fast path) | rp_accum (signed, compensated) |
|---|---|---|---|
| uniform (log_abs ~ N(0,1)) | 23.9 | **6.5** (3.7× faster) | 9.6 (2.5× faster) |
| wide (log_abs ~ U[−600,600]) | 25.7 | **14.5** (1.8× faster) | 17.6 (1.5× faster) |

Mechanism: the reference-exponent design spends one `exp` per term with the
`log` deferred to reduction, versus the textbook stream's `exp` + `log1p`
every term. Ratios are stable from n = 10² through 10⁶.
Pre-compensation v0.1 rp_accum measured 7.3/14.2 ns/term on these shapes:
compensation costs ~2–3 ns/term and buys the accuracy documented below.

**3. Exponent-tracking beats this runtime on pure products: PASS, published**

Pure product of lognormal factors, n = 10⁶ (ns/term, median): exponent-tracking
(frexp + int64 counter) **3.3**, log-domain product 7.3, naive linear 0.63
(fastest, but leaves double range around n ~ 10⁵, by design). Products are
exponent-tracking's territory; this library's case is sums.

## Cost (intent: "slower-but-right versus fast-but-meaningless")

Against the naive linear loop on benign inputs (uniform shape, n = 10⁶):
linear 3.4 ns/term, pos_accum 6.5 (1.9×), rp_accum 9.6 (2.8×). That is the
price of finishing with a correct answer in the regime where the linear loop
returns 0.0/inf/NaN. Cancellation-heavy inputs cost rp_accum ~11.4 ns/term.

## Cancellation accuracy: the investigation behind v0.2

On a heavy-cancellation dataset (10⁴ near-cancelling pairs plus a small tail,
condition number 2.3×10⁹), a plain sequential `log_add` fold was ~170× more
accurate than v0.1 `rp_accum` (2.8e-8 vs 4.8e-6 relative). Hypothesis: the
fold looked good only because cancelling pairs were *adjacent*, each pair
annihilating at matched magnitude before rounding error could accumulate,
while rp_accum accumulates all positive and all negative mass into two long
uncompensated sums whose accumulated error is amplified by cond at the final
subtraction.

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
- **Neumaier compensation wins in every ordering**, up to 5000× better than
  uncompensated, below the cond·ε level, at two additions per term. v0.2
  `rp_accum` ships with compensated pos/neg sums; `pos_accum` stays
  uncompensated because positive-only sums have no cancellation to amplify.

## The formal bound (v0.3 header contract)

Stated in `log_math.h` and machine-checked in `test_accuracy` (k recomputed
from the input data; the accumulator carries no instrumentation):

> **rp_accum worst-case relative error ≤ cond · (3k + 4 + D) · u + (|log|S|| + |log|net||) · u**,
> u = 2⁻⁵³, where k = rescale events (expected O(ln n) for random order,
> worst n−1), D = mass-weighted mean insertion depth, S = the exact sum;
> plus the ~745 log-unit vanishing contract. `pos_accum`:
> ≤ (n + 3k + 3 + D)·u + (|log|S|| + |log|net||)·u, no cond term (positive
> sums cannot cancel) — the n·u summation drift is the accepted price of the
> fast path. `net = S/exp(m_log)` is the scaled sum the final reduction takes
> the log of; that term was added 2026-08-16 after its absence refuted the
> rp_accum contract at 1.99×.

Both are **first-order** bounds, holding under the assumptions the header
states: `exp()` within 1 ulp, the vanishing window, and O(n·u²) and higher
terms neglected. They are not unconditional inequalities over all
floating-point behavior, and the header lists the assumptions so they can be
checked rather than trusted.

The per-reset discard (≤ Σ Aⱼ·u absolute) is **not** a separate error source
on top of these. Reset epochs are disjoint and each carries mass ≥ 2Aⱼ, so
Σ Aⱼ ≤ ½·cond·|S| and the reset contribution is at most cond·u/2, already
inside the 4u coefficient, for any number of resets. Derivation in
`log_math.h`.

Both forms were corrected after adversarial search refuted them: rp_accum's
cond·(3k+4)·u at 15.8×, pos_accum's (n+3k+3)·u at 34.9×. See CHANGELOG.md for
old and new values and the mechanisms they missed. The bound column below is
the corrected form.

## Accuracy vs double-double reference (v0.2, `test_accuracy`)

| scenario | n | v0.1 observed | v0.2 observed | corrected bound |
|---|---|---|---|---|
| long positive sum (rel err), k=21 | 10⁶ | 1.5e-14 | **9.3e-16** | 1.0e-14 |
| heavy cancellation, cond = 2.3e9 (rel err) | 2×10⁴ | 4.8e-6 | **1.6e-8** | 1.6e-5 |
| same data shuffled | 2×10⁴ | — | **4.4e-10** | 6.4e-6 |
| `log_add` fold, paired / shuffled (no contract) | 2×10⁴ | 2.8e-8 | 2.8e-8 / 4.1e-6 | — |
| magnitude staircase (log-abs err) | 461 | 0.0 | 0.0 | (below resolution) |
| forced pos==neg resets, k=20 (abs err) | 147 | 1.0e64 | 1.0e64 | 4.4e65 (k·A·u) |

Long sums are ε-level and n-independent: the residual error is the per-term
`exp()` rounding, not summation drift.

These six scenarios never exceeded even the old bound, which is the limitation
of fixed scenarios: they can only fail to refute a universal claim.
`bound_search` attacks the same claim directly and does refute it — twice now.
Under the current bound, observed sits **6.3×–5291× under** on these
scenarios (re-measured 2026-08-16 against the double-double `exp` reference)
and **0.83×** at the worst point the search could construct. The 0.85 figure
published before 2026-08-16 was the worst against the form that has since
been refuted at 1.99×.

Both new terms are small here, so these scenarios missed both mechanisms
rather than just one. On the n=10⁶ row the bound moved 7.438e-15 →
9.966e-15, so D + |log|S|| ≈ 22.8: about 18 of that is |log|S|| (the sum is
~9e7) and 4–5 is D, because `logmag ~ N(0,3)` keeps every term within a few
log units of the running reference. The search reaches the mechanisms by going
where these scenarios do not: depth clusters for D, and output magnitudes out
to |log|S|| ~ 500 for the reduction term.

## Second toolchain: the bound on glibc

The bound assumes `exp()` accurate to within 1 ulp. Every number above comes
from msvcrt, so that assumption was untested on a libm this project does not
control. Run of 2026-08-15, WSL2 Ubuntu, gcc 15.2.0 and clang 21.1.8,
`-Wall -Wextra -Werror -ffp-contract=off`, Release:

- All four suites pass under both compilers. Zero warnings under `-Werror`;
  the gcc/clang flag branch of CMakeLists needed no code changes.
- Every accuracy scenario lands under its formal bound.
- CI reproduces the table cell-for-cell on ubuntu-latest with gcc 13.3.0 and
  clang 18.1.3 (run 31842013920). Four compiler versions across two majors
  each, one set of numbers.

| scenario | n | glibc observed | corrected bound | slack |
|---|---|---|---|---|
| long positive sum, k=13 | 10³ | 0.0 | 6.2e-15 | — |
| long positive sum, k=11 | 10⁵ | 1.2e-16 | 6.3e-15 | 53× |
| long positive sum, k=21 | 10⁶ | 1.7e-15 | 1.0e-14 | 6.0× |
| heavy cancellation, cond = 1.8e9 | 2×10⁴ | 2.7e-9 | 1.4e-5 | 5300× |
| same data shuffled | 2×10⁴ | 1.1e-8 | 6.9e-6 | 650× |
| `log_add` fold, paired / shuffled | 2×10⁴ | 4.9e-8 / 1.7e-6 | (no contract) | — |
| magnitude staircase (log-abs err) | 461 | 0.0 | 1e-12 | — |
| forced pos==neg resets, k=20 (abs err) | 147 | 1.0e64 | 4.4e65 | 44× |

**These cells are not comparable one-to-one with the msvcrt table above.**

`std::mt19937_64` is reproducible across implementations but
`normal_distribution` and `uniform_real_distribution` are not, so the same
seeds generate different datasets: the cancellation scenario draws cond =
1.8e9 here versus 2.3e9 on Windows. What transfers is the verdict, the
property under test: the bound holds on both libms, with 6×–5300× slack on
glibc. No evidence of `exp()` exceeding the assumed 1 ulp.

## Next

Nothing outstanding in this file. The second-machine run landed 2026-08-15
(run `31865095928`) and is recorded above. Open benchmark work, if any, is
tracked in TODO.md.
