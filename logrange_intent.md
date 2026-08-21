
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

**The prior-art boundary is a build rule, not just a citation.** Borrow the compiler analysis that is already solved and validated; spend this project's complexity only on what is genuinely different. The one thing here that no compiler does is decide whether a reduction will *lose its answer to floating-point range* — that is the whole project, and it is worth doing carefully. Deciding whether a loop *is* a reduction is not: LLVM's `RecurrenceDescriptor` answers it, the loop vectorizer depends on it being right, and this repository has no business being its second authority.

The rule has been broken in both artifacts that recognize reductions, while the boundary paragraph above was missing from the tree, and both now delegate to `AddReductionVar`. `matcher/DELTA.md` has the accounting for the first; TODO.md and CHANGELOG.md for the second. **When this project reimplements compiler analysis, that is a bug in the plan, not an achievement.**

## Stretch Goal — End-to-End Log-Form Propagation (CLOSED, refuted)

*Extends Deliverable 2 and is not required by it. The runtime ships without this, and so does the first compiler release.*

**Closed as refuted. The numerical premise was tested and rejected, and the stopping rule below fired at the first rule rather than at a frontier.** Both remaining vocabulary rules were measured before implementation and neither beats plain linear on accuracy: each step costs a rounding proportional to the log-magnitude it crosses, so the gap grows with chain length instead of amortizing. The lattice, the second and third rewrite rules, and the general dataflow are **not built** and will not be.

**What this bought.** The specification below describes a compiler-analysis subsystem: an SSA lattice, transfer functions per opcode, a materialization frontier, and a legality oracle. Refuting the premise before building it is the result. A wall reached by implementation would have cost the subsystem and reported the same thing.

**What remains, and stays narrow.** `propagate=div` covers one consumer shape and is kept at that scope. Its value is **availability, not accuracy**: a correct finite answer where the linear path returns 0.0 or NaN. It is not a first step toward the rest of this section.

**Reopening needs a different hypothesis, not a better implementation.** A new empirical observation that changes the premise, not a further attempt at these rules. `pass/CHAINS.md` has the accounting, `tests/chain_search.cpp` is the instrument, and the rest of this section is kept as the specification the measurement was taken against.

**The problem.** Converting the rescued log form back to linear loses the rescue at the final step: for inputs near −800, `m + log(s) ≈ −792.6` is a healthy double while `exp(m + log(s))` is 0.0. A side global is the escape hatch for a consumer the pass cannot rewrite, and it is last-rewrite-wins. The rescue requires propagating the log form into downstream consumers instead. This section states the target transformation, the legality rule, the success criteria, and the stopping conditions.

**The design.** One rule, stated once: **never hand-place a conversion.** Mark the rescued value as log-form and push the representation outward; materialize back to linear only where no rewrite applies.

- `fdiv(x, s)` where `s` is log-form `L` → `fsub(x_log, L)` (softmax's divide becomes a subtract).
- `fmul` → `fadd` of log-magnitudes.
- `fadd` of two log-form values → logsumexp.
- A use that no rule covers (a store to memory, a call argument, a comparison, an op outside the vocabulary) forces an `exp()` materialization at that point.

Adjacent `exp(log(x))` pairs fold on contact. The log region grows to its natural frontier and stops there.

**The lattice.** A three-point value lattice over SSA values: `Linear` (default), `Log` (rescued form), `Conflict` (meets an unknown use). Transfer functions rewrite instructions in the vocabulary; every other instruction is a meet that forces materialization. This is the standard dataflow shape, not an invention.

**The legality oracle.** The matcher's risk analysis answers the question the lattice cannot: *is it safe to materialize here?* At a frontier in the rescue regime (|log| ~ 700), `exp()` provably re-underflows, so materialization is refused and the log region extends. One analysis does two jobs: gating the original rewrite and proving boundary conversions safe.

**Legality is the caller's grant, wider than the first rewrite's.** The original transform needed reassociation permission; propagation additionally requires *value* rewrites (`fdiv` → `fsub`) whose results are not bit-identical on benign inputs. Each propagation step is a separate, named opt-in; a miss is a decline, not a fallback.

**Prior art.** The pattern is established; the application to range rescue is not:

- *Q/DQ propagation* (ONNX Runtime, TensorRT): rewrite rules push quantize/dequantize nodes apart across ops with quantized equivalents, folding `DQ→Q` pairs; the dequantize lands where pushing stops. Same shape as pushing `log`/`exp` apart.
- *TAFFO*: an LLVM-based tool that propagates representation annotations (fixed-point) through SSA, inserting conversions at boundaries. The architecture transfers; the representation differs.
- *Logarithmic Number Systems*: multiply = add, divide = subtract, add = logsumexp, proven at scale in hardware. The lesson: log-domain arithmetic is easy when the representation is a first-class type and hard when it is a convention smuggled through a type system that does not know it. That is why the export hook is a global.

**What this project has that prior art lacked:** a lowering target that is specified. Q/DQ lowers to int8 hardware; TAFFO lowers to fixed-point C. Log-form propagation lowers adds to `logrange::log_add`, with the stated, adversarially-tested error bounds. The runtime stops being a library you call and becomes the codegen backend for the pass.

**First milestone.** One real softmax computation, denominator loop and normalize divide in the same function, carries the log representation from the denominator, through the divide, to the final observable result. Verified end-to-end: benign inputs agree to ~1 ulp, the underflowing case returns a correct finite log-probability where the original returns 0.0.

**Success criteria.** (1) The milestone above passes. (2) Propagation stays within 15x the linear re-conversion at a single conversion, returns a correct finite result where the linear path returns NaN, and carries a measured accuracy budget on a multi-step chain before any accuracy claim is made for it. (3) The legality grant is stated per rewrite and honored: no propagation fires without it.

**Why criterion 2 is shaped that way.** An earlier form required the propagated result to be *more* accurate than the linear re-conversion. That is false at a single conversion, and false by construction rather than by accident: `t - L` carries `u·|t - L|` where the re-conversion carries one rounding, so propagation loses precisely where it has least to offer. The case for propagation is **chains** — following computation across operations that would otherwise materialize and re-convert at each step — and that case cannot be measured while the vocabulary is one rule. CHANGELOG.md carries the refutation and its sweep.

**Stopping rule.** Propagation stops where no safe or profitable log-domain representation exists. A single end-to-end transformation on real code establishes the technique; a documented wall, the point where the lattice meets a use it cannot rewrite and the materialization is provably lossy, is also a deliverable. If the frontier is immediately outside the first loop on every real codebase, the answer is "diagnostic-first was correct" and that is published.

**Explicitly out of scope.** Interprocedural propagation, arbitrary consumer shapes, and any change to the IR type system. The first milestone is intra-function. The `__logrange_logsum` global remains the escape hatch for the prototype; the milestone replaces it for one named consumer, not for the general case.

## The Diagnostic

The same analysis supports a lint: *"this reduction will leave representable range for inputs like X — consider log-domain accumulation, here is the header."* Less glorious than a rewrite, and more honest where the rewrite does not pay. It was specified as the fallback if the pass proved impractical. It is what ships in front, and the Shipping Posture below states why.

## Shipping Posture

*What ships and under what label. The conditions, their status, and the dated record of what closed them are in TODO.md, "Tooling — ships as beta, gaps stated", and CHANGELOG.md.*

**Decision.** 1.0 ships four artifacts under three labels. `include/logrange/log_math.h` is **the product**: stable API, stated error contract, packaged. `matcher/logrange-scan.sh` is the **diagnostic, beta**: a build directory in, a report out, gaps enumerated in its own doc, exit codes stable enough to gate CI. `matcher/` is the diagnostic's engine, its `diagnose.sh` renderer, and the study instrument, shipped as a **research tool** at the same maturity as the diagnostic and not separately supported. `pass/` is a **labeled prototype**: opt-in, not installed, not in the package, and outside 1.0's support surface. The diagnostic is the front door.

**What decides it.** Only a small fraction of the loops carrying the sum-of-products shape carry a static range signal, and the rescue-worthy set is a handful of source lines. A lint that names those and points at the header is proportionate to that; a rewrite firing on shape alone is not, because it would touch every benign-range dot product and buy each one a transcendental per term against no range problem. The measured figures behind this are in matcher/RESULTS.md, derived from committed evidence rather than restated here.

**Why the diagnostic is the front door.** The pass's rescue is not observable in the value the program computes for most shapes: the linear replacement `exp(m + log(s))` re-underflows at exactly the inputs that motivate the rewrite, so the win exits through a side global that is last-rewrite-wins and requires the consuming link to define it. Propagation into a consumer closes that for one shape and no others. Beyond reach, the failure modes are asymmetric: a wrong HIGH costs a human ten minutes reading a loop, and a wrong rewrite costs a silently wrong answer under a flag the user set for unrelated reasons.

**Why the header is not the front door, given that it is the product.** The header requires the caller to already know which loop is in trouble. That knowledge is what the study shows is rare and unevenly held, in codebases written by people who understand floating point. The diagnostic's entire output is a pointer at the header, so the shipping path is diagnostic finds the site, header fixes it, pass is an opt-in power tool for the narrow shape it has been verified on. The header remains the fallback deliverable and is independently useful with zero compiler machinery; front door is a claim about discovery, not about value.

**The pass ships as a labeled prototype until its conditions are met.** They separate a prototype from a tool someone else can run, and none of them blocks 1.0, because 1.0 does not ship the pass as a product. The conditions and their status are tracked in TODO.md.

**Contradictions, stated plainly.**

- The README names three motivating shapes. The diagnostic flags two. The forward algorithm is matched in both the forms it is usually written and grades LOW in both, because its underflow accumulates across an enclosing loop and risk is judged one loop at a time.
- The pass's risk gate is described as a gate and cannot decline any input it can match: eligibility requires an accepted `exp` call, and a HIGH verdict means the same thing, so the rewritable set is a subset of the HIGH set by construction. Both branches are tested; only one is reachable. The gate becomes load-bearing when the pass can soundly rewrite a shape that verdicts LOW or MED.
- matcher/RESULTS.md's decision block says the shape survives real codebases and the pass proceeds, and the same block says shape-abundance is not profitability. Both are true of the same data. This posture resolves them by letting the pass proceed as a prototype rather than as a product.
- The pass matches only `llvm.exp.*`, so a translation unit compiled without `-fno-math-errno` is declined outright. It therefore covers a strictly narrower set of real code than the matcher reports hits in. The way out is the documented extension point: accept a direct `exp` call when IR attributes prove it cannot write `errno`.
- The diagnostic is the front door and is the artifact with the least automated coverage relative to the library behind it. Shipping it as beta on that footing is a stated gap, not an oversight.

## Success Criteria

1. A 1000-term mixture likelihood whose terms individually underflow returns a finite log-magnitude accurate to stated bounds; the linear loop returns 0.0.
2. The runtime's overhead versus hand-written logsumexp is within noise — the header should cost nothing over what experts already write by hand — *where "noise" is the harness's measured floor (±1%)*.
3. Exponent-tracking beats this runtime on pure products, and the benchmark says so. Scope honesty is a feature.
4. The matcher's hit rate on at least three real codebases is measured and published before any rewrite code exists.

## Risks

- **The idiom may be rare in matchable form.** Mitigated by measuring first (criterion 4).
- **Nobody asked for this.** True. The primitive is re-implemented everywhere it's needed, often with corner-case defects; a specified version with published error bounds has clear value.
- **Error analysis is the hard part.** The bound under cancellation is real numerical-analysis work, not plumbing. It is also the entire difference between this and every ad hoc version, so it cannot be skipped.
- **The seed may mislead.** Inherited code arrives with inherited assumptions; the rp_accum design was kept because it is interesting and plausible, not because it was proven. A bound is a claim about every input, so it is searched for counterexamples and read by someone other than its author before it is published as a contract. TODO.md carries which passes have run and what they refuted.

