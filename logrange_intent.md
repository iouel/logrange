
# LogRange — Project Intent

*Draft v0.3 — post-salvage. Incorporates the seed runtime inherited from NativeConv, its known defects, and lessons from that project's benchmarks and build history.*

---

## Aim

Build a small, well-specified log-domain accumulation runtime — and, if it earns it, a compiler pass — that rescues sum-of-products computations whose intermediate magnitudes exceed floating-point range.

---

## The Problem

Some computations are structurally doomed in linear floating point. Not pure products — those can be rescued cheaply by tracking an exponent counter alongside a normalized mantissa, no transcendental calls. The target here is sums of products:

```
total = Σᵢ  (product of many small/large factors)
```

Each product term underflows or overflows individually, and unlike a pure product, the *sum* forces all terms onto a common scale before combining — the point where linear representation runs out of range entirely.

The known fix is to hold each term as a logarithm and combine with logsumexp. It works, it is standard, and it is applied by hand, inconsistently, only where a programmer anticipated the failure.

## Honest Cost Model

Log residency is not a speedup and this project does not claim one. Each incoming term costs a `log()`; each addition in the sum costs a logsumexp (an `exp` and a `log1p`). Against a linear loop that would not overflow, this is slower.

What is bought for that price: the computation **finishes with a correct answer**. In the regime this project targets, the linear version returns 0.0, ∞, or NaN. Slower-but-right versus fast-but-wrong.

One genuine accuracy point in log's favor: absolute error in the log domain corresponds to *relative* error in the linear domain, so a long log-space accumulation maintains relative accuracy across many terms.

## The Seed

The project does not start from zero. It inherits ~120 useful lines from the predecessor project (NativeConv), validated by a passing test suite before extraction:

- `log_value` — signed log representation `{sign, log_abs}`, with `-inf` encoding zero.
- `logsumexp2`, `log_add`, `log_mul`, `log_div` — pairwise log-domain arithmetic including the signs-differ cancellation path.
- `rp_accum` — a reference-exponent accumulator that scales positive and negative term sums against a moving maximum, paying one `exp` per term and deferring the final `log` to reduction. This is cheap and accurate.

The seed carried **two known defects**, found in review and confirmed present in the extracted file — both resolved in the v0.1 refactor:

1. **`logsumexp2` silently absorbed infinities and swallowed NaN.** `if (!isfinite(a)) return b;` meant `logsumexp2(+inf, x)` returned `x` and a NaN input disappeared rather than propagating — a clear contract violation.
2. **The `pos == neg` cancellation reset was a silent precision cliff.** When the scaled sums compared equal at double precision, the accumulator reset to true zero — but equal doubles mean the residual is unresolved, not absent.

Two further seed observations, no action required: small terms more than ~745 log-units below the reference exponent scale to 0.0 and vanish (acceptable for a sum, must be *stated*), and `pos`/`neg` sum overflow is genuinely impossible under the guard logic.

## Deliverable 1 — The Runtime (the load-bearing artifact)

A single C header providing **signed log-domain accumulation**: values carried as `{sign, log_abs}`, accumulated with cancellation-aware logsumexp, with documented error bounds and defined behavior at every boundary.

Requirements:

- Positive-only fast path and a signed general path, separately usable.
- Exact preservation of IEEE edge semantics at the boundary: a NaN term yields NaN out, signs of zero handled deliberately, no silent absorption of infinities. *(Seed defect 1 violated this; fixed in the v0.1 refactor.)*
- A stated worst-case error bound under cancellation — the property every hand-rolled version lacks. *(Seed defect 2 was an unstated bound decision; documented at the reset site in the v0.1 refactor.)*
- Benchmarked honestly against: the naive linear loop, exponent-tracking (for the pure-product case, where exponent-tracking *should win* — publishing that number is part of being trustworthy), and hand-written logsumexp.
- **The benchmark harness must be trustworthy before its numbers are.** Warmup runs, pinned cores, reported variance. The predecessor's harness showed 8x run-to-run swings on identical binaries; this one does not — measured floor ~1%.

This header is independently useful with zero compiler machinery, and it is the fallback deliverable if everything downstream stalls.

## Deliverable 2 — The Pass (conditional)

An LLVM pass that recognizes sum-of-products reductions at IR level and rewrites them to log-domain accumulation, converting once at the edges of the loop nest rather than per operation.

Preconditions, stated plainly:

- **Legality requires reassociation permission.** FP reductions cannot be reordered without fast-math flags or a pragma; opt-in is not a courtesy here, it is what makes the transform legal. This is explicit and enforced.
- **Semantics preservation is exact.** The rewrite must match linear behavior on NaN propagation and exceptional inputs, and must decline to fire on any loop it cannot prove has the required structure.
- **Hit rate is measured before the rewrite is built.** Milestone: write the matcher only, run it over real numeric codebases, count. If real-world sum-of-products loops are too gnarled to recognize, the pass is not built.

Prior art boundary: LLVM's loop-idiom pass proves the *shape* of this transform is acceptable compiler behavior; Herbie rewrites expressions, not loops; FPChecker already occupies the *detection* and *diagnostics* space.

## Stretch Goal — End-to-End Log-Form Propagation

*Extends Deliverable 2 and is not required by it. The runtime ships without this, and so does the first compiler release.*

The pass prototype computes the rescued result in log form but exports it through a side global. Converting that result back to linear representation immediately loses the numerical rescue in the regime where the original computation underflows.

The question is whether the compiler can propagate the log form into downstream consumers where the algebra permits it, without source-level changes. The first target is the softmax family:

```
s = Σᵢ exp(xᵢ)
y = exp(xⱼ) / s
```

where the transformed computation retains

```
L = log(Σᵢ exp(xᵢ))
y = exp(xⱼ - L)
```

instead of reconstructing `s` in linear floating point.

This is not a proposal to compile arbitrary programs into log space. Log-domain arithmetic and log-sum-exp are established techniques; the research question is whether a compiler can introduce the representation selectively at a range-unsafe reduction and carry it through compatible consumers far enough that the numerical rescue survives to the observable result.

**First milestone.** One real softmax computation carries the log representation from the denominator, through its natural consumer, to the final observable result.

**Stopping rule.** Propagation may stop where no safe or profitable log-domain representation exists. An end-to-end transformation on real code establishes the technique as a viable compiler capability; a clear propagation boundary establishes where the diagnostic takes over instead. A negative result that fixes that boundary is a valid outcome and a valid place to stop.

**Precondition on implementation.** This section gets the treatment the other deliverables have before any code is written: target transformation and legality conditions, success criteria and tests, representative positive and negative controls, profitability conditions, known propagation boundaries, and the outcomes that justify continuing or stopping. The failure mode to avoid is a sequence of ad hoc propagation cases with no research question and no stopping rule.

## Fallback Product — The Diagnostic

If the pass proves impractical, the same analysis supports a lint: *"this reduction will leave representable range for inputs like X — consider log-domain accumulation, here is the header."* Less glorious than a rewrite, and more honest if the rewrite does not pay.

## Shipping Posture — decided 2026-08-16

*The second half of step 9. The first half — driving the pass from the matcher's HIGH-risk triage — landed 2026-08-15 in commit `afee8d0`. This section decides what ships and under what label. Tracked in TODO.md, "Tooling — ships as beta, gaps stated".*

**Decision.** 1.0 ships four artifacts under three labels. `include/logrange/log_math.h` is **the product**: version 1.0, stable API, stated error contract, packaged. `matcher/diagnose.sh` is the **diagnostic, beta**: usable, gaps enumerated in its own doc, exit codes stable enough to gate CI. `matcher/` is the diagnostic's engine and the study instrument, shipped as a **research tool** at the same maturity as the diagnostic and not separately supported. `pass/` is a **labeled prototype**: opt-in, not installed, not in the package, and outside 1.0's support surface. The diagnostic is the front door.

**The counts that decide it.** 2859 innermost FP loops were scanned across GSL 2.8, darknet and libsvm. 783 carry the sum-of-products shape (27.4%). 5 carry a static range signal at HIGH — 0.17% of loops scanned, 0.6% of shape hits — and those 5 rows are 4 distinct source lines, darknet's `blas.c:315` counted twice because it is matched in two functions. (RESULTS.md's prose still says "two source sites"; that sentence predates the `nMul` correction that took HIGH from 3 rows to 5, and is stale.) The pass rewrites one shape and has been verified only on its own kernel: no site from the study has been rewritten end to end, and two of the four — the Gaussian kernel's `deep-chain` and anything with a multiply in the term — are outside what it matches. A rewrite firing on shape would touch hundreds of benign-range dot products and buy each one a transcendental per term against no range problem; a lint that names 5 sites and points at the header is proportionate to what was measured. RESULTS.md reached this conclusion from the same data and called it likely. This section makes it the decision and states what it costs.

**Why the diagnostic is the front door, beyond the counts.** Three reasons from the artifacts as they stand. First, the pass's rescue is not observable in the value the program computes: the linear replacement `exp(m + log(s))` re-underflows at exactly the inputs that motivate the rewrite, so the win exits only through the `__logrange_logsum` side global, which is last-rewrite-wins and requires the consuming link to define it. Second, Deliverable 2's second precondition — semantics preservation is exact — was unmet when this posture was written, and closing it took two fixes rather than the one anticipated: a `-inf` term produced NaN where the linear original produces 0, and separately the opt-in gate conflated permission to reassociate with permission to change `errno`, exception flags, rounding mode and denormal handling. Both are now closed (see the end of this section); the posture is stated to hold either way and does. Third, the profitability gate wired in on 2026-08-15 cannot decline any input the pass can match, because the single matched shape requires an `exp` call and therefore verdicts HIGH by construction; the gate is a tested mechanism with no reachable work to do. Against all three, the failure modes are asymmetric: a wrong HIGH costs a human ten minutes reading a loop, and a wrong rewrite costs a silently wrong answer under a flag the user set for unrelated reasons.

**Why the header is not the front door, given that it is the product.** The header requires the caller to already know which loop is in trouble. That knowledge is the thing the study shows is rare and unevenly held — 5 sites in 2859 loops, and the three codebases scanned are written by people who understand floating point. The diagnostic's entire output is a pointer at the header, so the shipping path is diagnostic finds the site, header fixes it, pass is an opt-in power tool for the narrow shape it has been verified on. The header remains the fallback deliverable and is independently useful with zero compiler machinery; front door is a claim about discovery, not about value.

**Conditions for the pass to ship as anything other than a labeled prototype.** Six, all post-1.0, all testable:

1. ~~`-inf` inputs produce the linear loop's result.~~ **Closed 2026-08-16, and wider than stated.** `-inf`, `+inf`, NaN mixed with infinities, and zero-trip all produce the linear loop's result, asserted by named cases against constants *and* against a corrected independent reference — the previous reference returned NaN for all-`-inf` and for `+inf`, so the first infinity tests were not reference-validated at all. NaN propagation is verified alongside, including NaN in the first position, since a guard that neutralizes `inf - inf` must not also neutralize NaN. Closing the precondition additionally required an `errno` contract nobody had costed: the rewrite deletes N source `exp` evaluations and emits 2N different ones plus a `log`, so the pass now matches only `llvm.exp.*` and declines `strictfp`, constrained-FP and non-IEEE denormal environments outright (`pass/ELIGIBILITY.md`). "Exact" is claimed for special values and observable FP environment, not for finite bit patterns — finite rounding changes are what the reassociation grant buys.
2. The rescue is observable without the side global: either one downstream consumer is rewritten so the log form reaches an observable result (the Stretch Goal's first milestone), or `__logrange_logsum` is replaced by an interface that is defined for more than one rewrite per process. Last-rewrite-wins is not shippable under any label.
3. Shape coverage extends past the single `fadd(phi, exp(t))` form to at least the `fmuladd` spine and `w[i]*exp(t)` — the mixture-likelihood shape the intent names — because until a matchable shape can verdict below HIGH, the profitability gate in front of the rewrite is decoration, and "profitability analysis required in front of any rewrite" is not actually being enforced by anything.
4. The emitted streaming state carries a stated error bound at the standard of the header's contracts. It currently has one measurement, 1.4e-15 relative on one benign case, and no bound.
5. The pass runs in CI on every push, on the same footing as the library tests, rather than as a manual WSL script. (In flight as this was written: `.github/workflows/llvm-tooling.yml` gates the matcher selftest, the coverage claims, and `run_pass_test.sh` on push to main and on pull requests. This condition closes when that is green and stays green.)
6. The dead original chain is removed, or the pass documents that a DCE run after it is part of the supported pipeline.

Conditions 1 and 2 are correctness and interface blockers. Conditions 3 through 6 are label blockers: they are what separates a prototype from a tool someone else can run. None of the six blocks 1.0, because 1.0 does not ship the pass as a product.

**What the diagnostic must state about its own coverage for this posture to be honest.** Its "Scope limits" section already carries the mechanical ones. It states that it is a source-shape lint and not a range proof, that memory-carried reductions and reductions mirrored to a fixed cell are uncovered, that per-loop risk cannot see magnitude decay across an enclosing loop, and that vectorized and unrolled forms are missed. What is not yet stated anywhere the reader will meet it: **the diagnostic covers two of the three shapes the README names as motivating this project.** Mixture likelihood and softmax denominator are flagged HIGH; the forward algorithm is not flagged in either of its forms — the `out[j] +=` form is rejected at the mid-loop-read guard, and the register-accumulator form is seen and graded LOW because the underflow lives in the outer loop. The README currently names all three without qualification. Either the README stops implying coverage or the risk rule gains a cross-loop signal; this decision takes the first branch for 1.0, because the second is the open item that the guard-refinement measurement already declined to fund. The diagnostic must also state its measured selectivity — 5 HIGH in 783 hits in 2859 loops — so a user reading a report of one finding knows whether that is normal.

**Contradictions, stated plainly.**

- The README names three motivating shapes. The diagnostic flags two. The forward algorithm is invisible in the form it is usually written and graded LOW in the form the matcher can see.
- The pass's risk gate is described as a gate and cannot decline any input it can match. Both branches are tested; only one is reachable.
- RESULTS.md's decision block says the shape survives real codebases and the pass proceeds, and the same block says shape-abundance is not profitability and the actionable set is 5 sites. Both are true of the same data. This posture resolves them by letting the pass proceed as a prototype rather than as a product.
- Deliverable 2 was written with three preconditions. All three are now met for the single matched shape: legality (opt-in, three enforced layers, with `force` narrowed to waive reassociation *proof* only), hit rate (measured before the rewrite was built, decision rule fixed in advance), and semantics preservation (special values exact; `errno`, exception flags, rounding mode and denormal behaviour unobservable to any eligible program). The pass existed for a day and a half while the second precondition was unmet, which is correct for a prototype and disqualifying for a release artifact. Worth recording rather than smoothing over: that precondition was believed to hinge only on `-inf`, and the `errno` defect surfaced afterwards — the artifact claimed conformance it did not have, twice, and both times the gap was found by checking the claim rather than by the tests failing.
- The LLVM tooling had no CI when this decision was taken, and the front door is the artifact with the least automated coverage while the library behind it has three toolchains green. Shipping the diagnostic as beta on that footing is a stated gap, not an oversight. It is condition 5's sibling and was being closed concurrently.

**What changed when the defects were fixed, 2026-08-16.** Condition 1 closed, and with it Deliverable 2's last outstanding precondition, so the pass is now blocked on product concerns — export interface, shape coverage, stated bound, CI, dead code — rather than on any precondition this document set. That is a real change in status and is recorded as one. It does not change the posture. The front door was decided by 5 sites in 2859 loops and by the rescue being unobservable outside the side global; neither moves when the NaN goes away. The pass stays a labeled prototype at 1.0 with condition 1 struck.

One cost the fix introduced, stated because it narrows reach: the pass now matches only `llvm.exp.*`, so a translation unit compiled without `-fno-math-errno` is declined outright. The pass therefore covers a strictly narrower set of real code than the matcher reports hits in. The way out is the documented extension point — accept a direct `exp` call when IR attributes prove it cannot write `errno` — which is unimplemented and needs its own accept and decline tests.

## Success Criteria

1. A 1000-term mixture likelihood whose terms individually underflow returns a finite log-magnitude accurate to stated bounds; the linear loop returns 0.0.
2. The runtime's overhead versus hand-written logsumexp is within noise — the header should cost nothing over what experts already write by hand — *where "noise" is the harness's measured floor (±1%)*.
3. Exponent-tracking beats this runtime on pure products, and the benchmark says so. Scope honesty is a feature.
4. The matcher's hit rate on at least three real codebases is measured and published before any rewrite code exists.

## Risks

- **The idiom may be rare in matchable form.** Mitigated by measuring first (criterion 4).
- **Nobody asked for this.** True. The primitive is re-implemented everywhere it's needed, often with corner-case defects; a specified version with published error bounds has clear value.
- **Error analysis is the hard part.** The bound under cancellation is real numerical-analysis work, not plumbing. It is also the entire difference between this and every ad hoc version, so it cannot be skipped.
- **The seed may mislead.** Inherited code arrives with inherited assumptions; the rp_accum design was kept because it is interesting and plausible, not because it was proven. *(Step 6 derived the worst-case bound, stated it as a header contract, and machine-checked it against a double-double reference. The derivation remains author-reviewed only; independent review and an attempt to falsify the bound by adversarial search are the next work item — TODO.md, "Bound review pass".)*

## First Action — status

Steps 1–3 complete (v0.1 refactor of the seed header):

1.  `logsumexp2` edge semantics fixed — NaN propagates, +inf propagates,
   -inf acts as log-zero identity. `log_add` matched, including
   inf + (-inf) → NaN per IEEE. Tests written against the contract table.
2.  The `pos == neg` reset documented as an explicit error-bound decision
   at the reset site: discards residual up to |largest term| · eps per reset
   event, in exchange for re-arming the reference exponent. Kept.
3.  Predecessor baggage stripped: pinch helpers, approximation toggles,
   polynomial paths, instrumentation counters. Namespace is now `logrange`.
   `rp_accum` poisoning is sticky and queryable; `add_scaled` poisons on
   invalid scale instead of silently ignoring.

4.  Benchmark harness built and run (see BENCHMARKS.md). Noise floor
   measured at ~1%; success criteria 1–3 all met — the underflowing mixture
   returns the exact answer where linear returns 0.0, the runtime beats
   hand-rolled logsumexp by 1.9–4.1× rather than merely matching it, and
   exponent-tracking wins pure products as predicted. A `pos_accum`
   positive-only fast path was added to the header (Deliverable 1's missing
   requirement), and an accuracy suite (test_accuracy) validates against a
   double-double reference.

5.  (empirical half) The 170× cancellation-accuracy gap explained and
   closed. The `log_add` fold's advantage was an ordering artifact (adjacent
   pairs annihilating at matched magnitude); shuffled, it lost to even the
   uncompensated accumulator. v0.2 `rp_accum` ships Neumaier-compensated
   pos/neg sums: up to 5000× more accurate under cancellation, robust to
   input ordering, ~2–3 ns/term — still 1.5–2.5× faster than hand-rolled
   logsumexp. `pos_accum` stays uncompensated (no cancellation to amplify).
   Full investigation: BENCHMARKS.md.

6.  Formal worst-case bound derived and stated as a header contract:
   rp_accum rel err ≤ cond·(3k+4)·u (k = rescale events, u = 2⁻⁵³);
   pos_accum ≤ (n+3k+3)·u. Machine-checked against measured data in
   test_accuracy — observed sits 5–1000× under the bound on every
   scenario. Deliverable 1 is functionally complete: the header now has
   the stated-error-bound property every hand-rolled version lacks.

7.  Matcher hit-rate study complete (matcher/RESULTS.md): 781 hits
   across 2859 innermost FP loops in GSL, darknet, and libsvm (27%),
   including the softmax-denominator and Dirichlet-likelihood shapes by
   name. Recall audit clean; decision rule cleared decisively. Verdict:
   the pass prototype proceeds, with profitability analysis (range/
   underflow risk) required in front of any rewrite — the abundant hits
   are mostly benign-range dot products; the rescue-worthy transcendental
   subset is small but includes exactly the shapes this project targets.

8.  Pass prototype (pass/): the softmax-denominator shape rewritten at
   IR level to streaming logsumexp, behind explicit opt-in (fast-math
   attribute or force parameter), verified end to end — benign inputs
   agree to 1.4e-15, the underflowing case exports a correct finite
   log-magnitude where the original returns 0.0, NaN propagates, and
   negative controls are untouched. Known limits in pass/PROTOTYPE.md:
   single shape, log form exported via a side global (propagating it to
   downstream users is the Stretch Goal), no profitability gating wired
   in yet.

9.  Both halves decided. **Triage wired** 2026-08-15 (`afee8d0`): the
   pass computes the matcher's risk verdict for its matched loop and
   declines below `min-risk` (default HIGH). The task was blocked on a
   matcher bug — an `nMul >= 1` rule that excluded plain sums of `exp`,
   i.e. the softmax denominator, this project's marquee shape and the
   only shape the pass rewrites. Fixed and the study re-run: 783 hits
   against 781, HIGH 5 sites against 3. **Shipping posture decided**
   2026-08-16: diagnostic-first. The header ships as the product at 1.0,
   `diagnose.sh` as the front door labeled beta with its coverage gaps
   enumerated, the matcher as its engine, and `pass/` as a labeled
   prototype outside the supported surface. Driven by 5 HIGH sites in
   2859 scanned loops (0.17%), by the pass's rescue being observable
   only through the `__logrange_logsum` side global, and by Deliverable
   2's semantics-preservation precondition being unmet (`-inf` → NaN).
   Six named conditions for the pass to lose the prototype label, and
   the coverage statements the diagnostic owes its reader, are in
   "Shipping Posture" above; the tracker entry is in TODO.md.

Remaining: nothing blocking. Open tooling gaps are tracked in TODO.md
under "Tooling — ships as beta, gaps stated". Propagating the log form to
downstream users instead of the side global is the Stretch Goal above,
deliberately outside step 9.
