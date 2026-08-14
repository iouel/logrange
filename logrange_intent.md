
# LogRange — Project Intent

*Draft v0.3 — post-salvage. Incorporates the seed runtime inherited from NativeConv, its known defects, and lessons from that project's benchmarks and build history.*

---

## Aim

Build a small, well-specified log-domain accumulation runtime — and, if it earns it, a compiler pass — that rescues sum-of-products computations whose intermediate magnitudes exceed floating-point range, returning correct answers where linear arithmetic silently degrades to zero or infinity.

---

## The Problem

Some computations are structurally doomed in linear floating point. Not pure products — those can be rescued cheaply by tracking an exponent counter alongside a normalized mantissa, no transcendentals required. The doomed shape is **sums of extreme-magnitude terms**: mixture likelihoods, forward-algorithm recursions, partition functions, softmax denominators. Anything of the form

```
total = Σᵢ  (product of many small/large factors)
```

Each product term underflows or overflows individually, and unlike a pure product, the *sum* forces all terms onto a common scale before combining — the point where linear representation runs out of room. The result degrades through gradual underflow into subnormals, loses precision quietly, and finally lands on exact zero (or infinity), with no signal that anything went wrong.

The known fix is to hold each term as a logarithm and combine with logsumexp. It works, it is standard, and it is applied by hand, inconsistently, only where a programmer anticipated the failure. The fix is mechanical. It is not automated, and the primitive it relies on is re-implemented ad hoc in every codebase that needs it.

## Honest Cost Model

Log residency is not a speedup and this project does not claim one. Each incoming term costs a `log()`; each addition in the sum costs a logsumexp (an `exp` and a `log1p`). Against a linear loop that is a large constant-factor slowdown, always.

What is bought for that price: the computation **finishes with a correct answer**. In the regime this project targets, the linear version returns 0.0, ∞, or NaN. Slower-but-right versus fast-but-meaningless is the actual trade, and it should be stated as such.

One genuine accuracy point in log's favor: absolute error in the log domain corresponds to *relative* error in the linear domain, so a long log-space accumulation maintains relative accuracy across magnitudes where linear arithmetic cannot represent the values at all.

## The Seed

The project does not start from zero. It inherits ~120 useful lines from the predecessor project (NativeConv), validated by a passing test suite before extraction:

- `log_value` — signed log representation `{sign, log_abs}`, with `-inf` encoding zero.
- `logsumexp2`, `log_add`, `log_mul`, `log_div` — pairwise log-domain arithmetic including the signs-differ cancellation path.
- `rp_accum` — a reference-exponent accumulator that scales positive and negative term sums against a moving maximum, paying one `exp` per term and deferring the final `log` to reduction. This is a distinct design point from the textbook per-term-logsumexp stream: cheaper per term, with explicit pos/neg separation for cancellation visibility. Whether it is *better* is precisely the error-analysis question this project exists to answer.

The seed carried **two known defects**, found in review and confirmed present in the extracted file — both resolved in the v0.1 refactor:

1. **`logsumexp2` silently absorbed infinities and swallowed NaN.** `if (!isfinite(a)) return b;` meant `logsumexp2(+inf, x)` returned `x` and a NaN input disappeared rather than propagating — a direct violation of the runtime's IEEE edge-semantics requirement below. *Fixed: NaN poisons, +inf propagates, -inf acts as the log-zero identity, with tests written against the contract table.*
2. **The `pos == neg` cancellation reset was a silent precision cliff.** When the scaled sums compared equal at double precision, the accumulator reset to true zero — but equal doubles mean the true sum is *below the accumulator's resolution*, not zero, and the reset discarded that irrecoverably. *Now documented at the reset site as an explicit error-bound decision: residual up to |largest term| · eps discarded per reset event, in exchange for re-arming the reference exponent.*

Two further seed observations, no action required: small terms more than ~745 log-units below the reference exponent scale to 0.0 and vanish (acceptable for a sum, must be *stated*), and `pos`/`neg` are uncompensated linear sums accruing standard O(n·ε) error — the bound work is genuinely still ahead.

## Deliverable 1 — The Runtime (the load-bearing artifact)

A single C header providing **signed log-domain accumulation**: values carried as `{sign, log_abs}`, accumulated with cancellation-aware logsumexp, with documented error bounds and defined behavior for zeros, sign changes, NaN, and infinities. Grown from the seed, not rewritten.

Requirements:

- Positive-only fast path and a signed general path, separately usable.
- Exact preservation of IEEE edge semantics at the boundary: a NaN term yields NaN out, signs of zero handled deliberately, no silent absorption of infinities. *(Seed defect 1 violates this today; it is the first fix.)*
- A stated worst-case error bound under cancellation — the property every hand-rolled version lacks. *(Seed defect 2 is an unstated bound decision; it gets documented or redesigned as part of this work.)*
- Benchmarked honestly against: the naive linear loop, exponent-tracking (for the pure-product case, where exponent-tracking *should win* — publishing that number is part of being trustworthy), and a hand-written logsumexp loop.
- **The benchmark harness must be trustworthy before its numbers are.** Warmup runs, pinned cores, reported variance. The predecessor's harness showed 8x run-to-run swings on identical binaries; success criterion 2 below ("within noise") is unfalsifiable without a measured noise floor, so the harness is a deliverable, not an afterthought.

This header is independently useful with zero compiler machinery, and it is the fallback deliverable if everything downstream stalls.

## Deliverable 2 — The Pass (conditional)

An LLVM pass that recognizes sum-of-products reductions at IR level and rewrites them to log-domain accumulation, converting once at the edges of the loop nest rather than per operation.

Preconditions, stated plainly:

- **Legality requires reassociation permission.** FP reductions cannot be reordered without fast-math flags or a pragma; opt-in is not a courtesy here, it is what makes the transform legal. This ships behind an explicit flag. *The predecessor project committed this exact class of error — its v0.1 spec set `/fp:fast` on a library whose headline attribute was determinism, corrected to `/fp:precise` before ship. The pass must surface reassociation as explicit opt-in rather than silently commit it.*
- **Semantics preservation is a contract, not a vibe.** The rewrite must match linear behavior on NaN propagation and exceptional inputs, and must decline to fire on any loop it cannot prove has the target shape.
- **Hit rate is measured before the rewrite is built.** Milestone: write the matcher only, run it over real numeric codebases, count. If real-world sum-of-products loops are too gnarled to recognize (guards, early exits, unrolling), the project pivots to the diagnostic below and the runtime stands alone.

Prior art boundary: LLVM's loop-idiom pass proves the *shape* of this transform is acceptable compiler behavior; Herbie rewrites expressions, not loops; FPChecker already occupies the *detection* niche (LLVM-instrumented underflow/overflow reporting). The unoccupied slot is the **repair** — detection exists, automated log-domain rewriting does not, as far as searching has established.

## Fallback Product — The Diagnostic

If the pass proves impractical, the same analysis supports a lint: *"this reduction will leave representable range for inputs like X — consider log-domain accumulation, here is the header."* Less ambitious than FPChecker's runtime instrumentation but static, zero-overhead, and pointing at a concrete fix rather than a report. A modest but real artifact from work already done.

## Success Criteria

1. A 1000-term mixture likelihood whose terms individually underflow returns a finite log-magnitude accurate to stated bounds; the linear loop returns 0.0.
2. The runtime's overhead versus hand-written logsumexp is within noise — the header should cost nothing over what experts already write by hand — *where "noise" is the harness's measured floor, not an assumption*.
3. Exponent-tracking beats this runtime on pure products, and the benchmark says so. Scope honesty is a feature.
4. The matcher's hit rate on at least three real codebases is measured and published before any rewrite code exists.

## Risks

- **The idiom may be rare in matchable form.** Mitigated by measuring first (criterion 4).
- **Nobody asked for this.** True. Justified as: the primitive is re-implemented everywhere it's needed, badly; a specified version has stb-library economics — small, boring, load-bearing. Demand for the *pass* is speculative; demand for a *correct, bounded logsumexp accumulator* is at least evidenced by its constant reinvention.
- **Error analysis is the hard part.** The bound under cancellation is real numerical-analysis work, not plumbing. It is also the entire difference between this and every ad hoc version, so it cannot be cut. The seed's `pos == neg` reset is the first concrete instance: an implementation choice that *is* a bound decision, currently undocumented.
- **The seed may mislead.** Inherited code arrives with inherited assumptions; the rp_accum design is kept because it is interesting and plausible, not because it is proven. If the error analysis shows the textbook streaming logsumexp dominates it, the seed gets replaced and that result gets published too.

## First Action — status

Steps 1–3 complete (v0.1 refactor of the seed header):

1. ✅ `logsumexp2` edge semantics fixed — NaN propagates, +inf propagates,
   -inf acts as log-zero identity. `log_add` matched, including
   inf + (-inf) → NaN per IEEE. Tests written against the contract table.
2. ✅ The `pos == neg` reset documented as an explicit error-bound decision
   at the reset site: discards residual up to |largest term| · eps per reset
   event, in exchange for re-arming the reference exponent. Kept.
3. ✅ Predecessor baggage stripped: pinch helpers, approximation toggles,
   polynomial paths, instrumentation counters. Namespace is now `logrange`.
   `rp_accum` poisoning is sticky and queryable; `add_scaled` poisons on
   invalid scale instead of silently ignoring.

4. ✅ Benchmark harness built and run (see BENCHMARKS.md). Noise floor
   measured at ~1%; success criteria 1–3 all met — the underflowing mixture
   returns the exact answer where linear returns 0.0, the runtime beats
   hand-rolled logsumexp by 1.9–4.1× rather than merely matching it, and
   exponent-tracking wins pure products as predicted. A `pos_accum`
   positive-only fast path was added to the header (Deliverable 1's missing
   requirement), and an accuracy suite (test_accuracy) validates against a
   double-double reference.

5. ✅ (empirical half) The 170× cancellation-accuracy gap explained and
   closed. The `log_add` fold's advantage was an ordering artifact (adjacent
   pairs annihilating at matched magnitude); shuffled, it lost to even the
   uncompensated accumulator. v0.2 `rp_accum` ships Neumaier-compensated
   pos/neg sums: up to 5000× more accurate under cancellation, robust to
   input ordering, ~2–3 ns/term — still 1.5–2.5× faster than hand-rolled
   logsumexp. `pos_accum` stays uncompensated (no cancellation to amplify).
   Full investigation: BENCHMARKS.md.

6. ✅ Formal worst-case bound derived and stated as a header contract:
   rp_accum rel err ≤ cond·(3k+4)·u (k = rescale events, u = 2⁻⁵³);
   pos_accum ≤ (n+3k+3)·u. Machine-checked against measured data in
   test_accuracy — observed sits 5–1000× under the bound on every
   scenario. Deliverable 1 is functionally complete: the header now has
   the stated-error-bound property every hand-rolled version lacks.

7. ✅ Matcher hit-rate study complete (matcher/RESULTS.md): 781 hits
   across 2859 innermost FP loops in GSL, darknet, and libsvm (27%),
   including the softmax-denominator and Dirichlet-likelihood shapes by
   name. Recall audit clean; decision rule cleared decisively. Verdict:
   the pass prototype proceeds, with profitability analysis (range/
   underflow risk) required in front of any rewrite — the abundant hits
   are mostly benign-range dot products; the rescue-worthy transcendental
   subset is small but includes exactly the shapes this project targets.

8. ✅ Pass prototype (pass/): the softmax-denominator shape rewritten at
   IR level to streaming logsumexp, behind explicit opt-in (fast-math
   attribute or force parameter), verified end to end — benign inputs
   agree to 1.4e-15, the underflowing case exports a correct finite
   log-magnitude where the original returns 0.0, NaN propagates, and
   negative controls are untouched. Known limits in pass/PROTOTYPE.md:
   single shape, log form exported via a side global (downstream
   propagation of the log value is the real win and remains open), no
   profitability gating wired in yet.

Remaining:

9. Connect the pieces: drive the pass from the matcher's HIGH-risk
   triage (3 sites across 3 real codebases), propagate the log form to
   downstream users instead of a side global, and decide the shipping
   posture (diagnostic-first, with the pass as the power tool).