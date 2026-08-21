# Road to 1.0

What "1.0" means here: the **header is the product** (intent Deliverable 1).
It must be trustworthy on a machine we've never seen. The tooling (matcher,
diagnostic, pass) ships alongside at whatever maturity it honestly has, each
labeled. Items ordered roughly by how much they'd embarrass us if skipped.

**1.0 shipped 2026-08-21.** Every item marked blocking it is closed, and the
last one that blocked calling the diagnostic shippable closed on 2026-08-17.
What remains below is the tooling tier, which ships as beta with its gaps
stated, plus post-1.0 work that was never a release blocker. Nothing here is
outstanding against 1.0; the file is kept as the record of how it was reached
and as the tracker for what comes after.

Sections are not strictly in execution order. The bound review ran first, on
the reasoning that it could move the error contract and the rest of 1.0 is
written on top of it. **It did move the contract.** The float decision
followed it, since that is a statement about the scope of the corrected
bound.

## Blocking 1.0 — library

- [x] **License file.** MIT, added 2026-08-15.
- [x] **Second toolchain, in CI.** Done 2026-08-15. CI matrix is now
      windows-msvc + ubuntu-gcc + ubuntu-clang. The gcc/clang flag branch
      (`-Wall -Wextra -Werror -ffp-contract=off`) needed no code fixes:
      clean under gcc 15.2 and clang 21.1. test_accuracy holds on glibc with
      6×–5300× slack (against the corrected bound), so its 1-ulp `exp()`
      assumption survives a second libm (BENCHMARKS.md, "Second toolchain").
      Verified green on the runners (gcc 13.3, clang 18.1) as well as locally
      (gcc 15.2, clang 21.1); the accuracy table is bit-identical across all
      four.
- [x] **Version identity in the header.** Done 2026-08-15.
      `LOGRANGE_VERSION_MAJOR/_MINOR/_PATCH`, ordered `LOGRANGE_VERSION`, and
      `LOGRANGE_VERSION_STRING` in log_math.h, plus CHANGELOG.md. The header
      is the source of truth: CMake parses it (the hardcoded `project(VERSION
      0.1)` had already drifted a release behind), and test_log_math checks
      the string against the numeric parts. Both guards negative-tested.
- [x] **Float support decision.** Decided 2026-08-15: **double only, by
      design**, stated in the header's Precision block. The reasoning is the
      bound's: every constant in the contract is double-specific. u = 2⁻⁵³,
      the ~745 log-unit vanishing window (subnormal floor 2⁻¹⁰⁷⁴), and now
      the depth term D that the same window caps. Float is not a typedef; it
      needs the bound re-derived at u = 2⁻²⁴ with a ~103 log-unit window
      (2⁻¹⁴⁹) and an accuracy reference finer than the tests' double-double.
      The rewrite pass already declines float accumulators, so double-only is
      coherent across the toolchain. Callers with float data widen at the
      accumulator boundary. Implementing a float variant stays optional and
      is now explicitly out of scope for 1.0.
- [x] **Second-machine benchmark run.** Done 2026-08-15, run `31865095928`
      via `.github/workflows/bench.yml` (workflow_dispatch only; timings
      never run on push). The windows-latest runner reported a **0.00%**
      noise floor, better than the author's machine, because the harness's
      pinning and priority raising work there and MSVC matches the published
      flag set. Hardware was the only variable.
      Criterion 1 is bit-identical across three configurations
      (−792.643769699630184, |error| 0). Criterion 3 reproduces (2.26× vs
      2.2×).
      *Found doing it:* criterion 2's margin is hardware-dependent, not a
      constant. `stream_lse`/`pos_accum` is 3.7× here and 5.27× on the
      runner, because `pos_accum` barely moved between machines (+4%) while
      `stream_lse` slowed 49%: one `exp` per term versus `exp` + `log1p`,
      so slower transcendentals punish the textbook stream twice. The
      durable claim is the direction and its reason, margin 1.5×–5.3×
      observed. BENCHMARKS.md says so rather than implying 3.7× travels.
- [x] **Install/packaging story.** Done 2026-08-15. `cmake --install` rules,
      an exported `LogRange::logrange` target, and a config package, so
      `find_package(LogRange 0.2 CONFIG REQUIRED)` works. Verified in both
      consumption modes on MSVC and gcc: installed-prefix and vendored
      `add_subdirectory` (the latter builds no tests and does not leak
      `-Werror`). Version compatibility is `SameMinorVersion`, since pre-1.0
      the error contract can move between minors and has.
      The README quickstart is now `examples/quickstart`, built and run
      against the *installed* package by CI on all three legs, so the docs
      cannot drift from what works.
      *Found while doing it, then corrected:* I first read a 14× accuracy
      change under `-ffp-contract=fast` as FMA contraction damaging the
      compensation, and exported the flag to consumers on that basis. Wrong.
      On fixed inputs the accumulator is bit-identical with contraction on or
      off: there is no multiply-add pair in the compensation path to fuse.
      The 14× was `test_accuracy`'s corpus being *generated* with
      multiply-add expressions that contract, producing different data; the
      uncompensated `log_add` fold moved too, which should have been the
      tell. What breaks the accumulator is reassociating math (4.9e-6
      relative under `-ffast-math`), so the header now refuses to compile
      under it and no flag is imposed on consumers.

## Blocking 1.0 — honesty debts

- [x] **Bound review pass.** Done 2026-08-15, and the bound did not survive.
      The cond·(3k+4)·u form is refuted: `tests/bound_search.cpp` finds 151
      of 400 random inputs over it (worst 15.8×) and a constructed
      counterexample at 5.8× with cond=1, k=0. Two terms were missing: the
      rounding of exp's *argument* (mass-weighted mean depth D) and the
      final m_log + log|net| reduction (|log|S||·u, which never touches
      cond and dominates at the extreme magnitudes this library targets).
      New contract: cond·(3k+4+D)·u + |log|S||·u, worst observed/bound 0.85
      across the search. Header, CHANGELOG, BENCHMARKS and test_accuracy
      updated together.
      *Open at the time of this pass, all since closed by the three items
      below:* the two originally flagged questions were not the ones that
      broke it. Pos/neg rescale-error correlation and the n·u² threshold
      were unexamined, `pos_accum`'s (n+3k+3)·u had no review, and the
      derivation had no independent read.

      *Why it ran first, kept as the record of the call.* The bound is what
      this library claims that every hand-rolled logsumexp lacks; it is the
      product. The header contract, README, CHANGELOG and BENCHMARKS.md all
      inherit from it, so a bound that moves moves them with it. This one
      moved. Work finished before the review might have needed redoing; work
      finished after it will not.
- [x] **Bound review, second pass — pos_accum.** Done 2026-08-15, and it
      broke worse than rp_accum did: (n+3k+3)·u fails on 119 of 400 random
      inputs, worst **34.9×**. Every violation is the final-reduction term
      (|log|S||·u), which does not grow with n and has no cond to hide
      behind. Four terms near e⁶⁹⁰ budget 7u against ~500u of real error.
      New contract (n+3k+3+D)·u + |log|S||·u, worst 0.79 across the search.
      The predicted argument-rounding failure did *not* materialize here:
      D ~ ln n < n, so the n·u term already covers it, confirmed by depth
      clusters that never exceed 0.22 of even the old bound.
      Also closed a gap this exposed: `test_pos_accum` asserted behavior but
      never the contract, so the bound had never been machine-checked at
      all. It now asserts it, and the assertion was verified to fail under
      the old form.
- [x] **Bound review, third pass — the rest of the header's claims.** Done
      2026-08-15. The prior was right again: `log_mul`/`log_div` were
      documented as **"exact in log domain"**, and they are not. The mapping
      is exact, the floating-point add implementing it rounds. Measured
      1024u on a product of log-magnitude 1024.
      The root cause of all three refutations is now stated once, at
      `log_value`, where it belongs: **the representation has a precision
      floor of |log|x||·u that grows with magnitude**, ~13 significant
      digits, not 16, at the |log|x|| ~ 700 range this library targets.
      Every reduction ending in `m_log + log(...)` inherits it, which is why
      one undocumented fact broke two independent accumulator contracts.
      `logsumexp2` now states a bound for the first time; the round trip
      through `log_value` is measured at 512u.
      The two originally flagged questions both close by inspection, no
      change needed: pos/neg rescale errors bounded independently and added
      is conservative under any correlation (triangle inequality on
      pos − neg), and the O(n·u²) term does not reach u until n ~ 10¹⁶
      terms, which is not addressable.
- [x] **Independent read.** Done 2026-08-15. The read agrees that the
      repaired `D` term and the representation/final-reduction term close
      real omissions. No counterexample to either corrected accumulator
      contract was found, after reading the header, implementation,
      adversarial harness, fixed tests, reset test, public docs, packaging,
      and a broader repository search.

      *Correction made during the read:* the reset's documented
      `Σ Aⱼ·u` absolute loss does **not** invalidate rp_accum's relative
      contract. A reset at scale Aⱼ implies cancelling mass of at least about
      2Aⱼ, so `cond = Σ|xᵢ|/|S|` already contributes at least about
      `2Aⱼ/|S|`; the existing `cond·(3k+4+D)·u` budget (coefficient at least
      4u) conservatively covers the reset loss when expressed relatively.
      The separate absolute wording is therefore explanatory, not an
      unbudgeted error source.

      *Non-blocking proof-tightening left:* state that reset-coverage argument
      explicitly, so the mixed relative/absolute presentation cannot be
      misread; describe the published contracts as first-order bounds under
      their stated 1-ulp `exp()`, vanishing-window, and neglected higher-order
      assumptions; and make the final-reduction explanation more precise about
      how the representation-floor term and conditioning cover cancellation in
      `m_log + log(|net|)`. These are clarity/proof-presentation tasks, not
      evidence that the corrected bounds fail.
- [x] **BENCHMARKS.md refresh at 1.0 flags.** Done 2026-08-15. Nothing
      required a refresh: every header edit since v0.2 is preprocessor or
      comment (version macros, fast-math guard), the bench source is
      untouched, and the flags are the same four, moved from directory scope
      onto a target. Re-run anyway to confirm rather than assert. Ratios
      reproduce (3.75×/3.70× vs 3.7× published; 2.56×/2.54× vs 2.5×).
      *Found doing it:* absolute medians move up to ~8% between runs on this
      machine while the harness reported 0.15–2.35% floors, because the
      floor compares two identical kernels **within** one run and says
      nothing about run-to-run reproducibility. BENCHMARKS.md now scopes the
      "±1%" claim accordingly. This also sets the bar for the second-machine
      run: a cross-machine delta under ~8% is not evidence of anything.

## Tooling — ships as beta, gaps stated

- [x] **Wire triage → pass** (intent Shipping Posture). Done 2026-08-15, after the task
      turned out to be blocked on a matcher bug. The pass now computes the
      matcher's risk verdict for its matched loop, declines below `min-risk`
      (default HIGH), and logs the verdict on every rewrite. Both branches
      are tested, including that a misspelled parameter is refused rather
      than silently reverting to the default.
      *The blocker:* the matcher could not verdict the pass's shape at all.
      Its `nMul >= 1` rule excluded plain sums of `exp`, the softmax
      denominator and this project's marquee shape, so scanning
      `pass/test_softmax.c` produced zero hits; darknet's softmax had
      matched only because it divides by a temperature. Fixed and the study
      re-run: 783 hits against 781, HIGH 5 against 3 (matcher/RESULTS.md,
      "The rule that excluded the marquee shape").
      *Honest limit, closed 2026-08-17:* because the pass's only shape
      required an `exp` call, its verdict was always HIGH and the gate could
      not decline a real input. The spine widening below made it load-bearing:
      `dot_sum` verdicts LOW and is refused at the default threshold.
- [x] **Shipping posture** (intent Shipping Posture). Decided
      2026-08-16: **diagnostic-first**. Four artifacts, three labels. The
      header is *the product* at 1.0. `matcher/diagnose.sh` is the **front
      door**, labeled beta with its coverage gaps enumerated. `matcher/` is
      the diagnostic's engine and study instrument at the same maturity.
      `pass/` is a **labeled prototype**, opt-in, not installed, outside
      1.0's supported surface. Full statement and reasoning:
      `logrange_intent.md`, "Shipping Posture".
      *What drove it:* 5 HIGH rows in 2859 scanned loops (0.17%; 4 distinct
      source lines, 0.6% of the 814 shape hits). A lint naming 5 sites is
      proportionate; a rewrite firing on shape would touch hundreds of
      benign dot products and buy each a transcendental per term. Plus two
      facts about the pass specifically: its rescue is observable only
      through the `__logrange_logsum` side global (the linear replacement
      re-underflows at exactly the motivating inputs), and Deliverable 2's
      "semantics preservation is exact" precondition was unmet at the
      committed state this was written against (`-inf` term → NaN where
      linear gives 0; a fix was in flight in the tree). The posture is
      stated to hold either way.
      *Six conditions for the pass to lose the prototype label*, all
      post-1.0, none blocking: (1) `-inf` matches linear, asserted in
      `run_pass_test.sh`; (2) the rescue is observable without the side
      global (last-rewrite-wins is not shippable under any label); (3)
      shape coverage reaches `fmuladd` and `w[i]*exp(t)`, without which
      the risk gate cannot decline anything; (4) a stated error bound for
      the emitted streaming state; (5) the pass runs in CI on push, not as
      a manual WSL script (`.github/workflows/llvm-tooling.yml` was landing
      as this was written); (6) **the dead original chain is removed by the
      pass itself.** 1 and 2 are correctness/interface blockers, 3–6 are
      label blockers.
      *Condition 6 reworded 2026-08-21, then closed the same day.* It
      previously read "dead original removed **or a required DCE run
      documented**", and the second branch was taken: `PROTOTYPE.md` named
      `-passes='...,adce'` as the supported pipeline and `run_pass_test.sh`
      asserted `PASS,adce_removes_the_dead_original`. Ruled a placeholder, on
      the ground that a condition an artifact can satisfy by describing itself
      is not a condition, and rewritten to close only when the pass deletes
      what it orphaned. **It now does.** `RecursivelyDeleteDeadPHINode` runs at
      the end of each rewritten iteration, and the `adce` suffix is gone from
      the supported pipeline in ELIGIBILITY.md section 0.
      **34/2 `llvm.exp.f64`/`f32` before, 28/1 after**, which is what `adce`
      used to produce. The emitted-code bound search is byte-identical across
      the change, 7285 trials, worst 0.99.
      The gate changed shape rather than its numbers: a count is satisfiable by
      deleting the right quantity of the wrong thing, so it now requires `dce`
      and `adce` over the pass's output to return it unchanged. An update also
      stored to a loop-invariant cell is the one shape the cycle walk cannot
      start on; it prints `ORPHAN-KEPT` rather than passing silently,
      negative-tested by forcing the deletion to fail.
      *Status 2026-08-21: 1, 4, 5 and 6 closed; 2 closed for one consumer shape
      only; 3 literal-text-met but property open.* Condition 2 is
      closed only where `propagate=div` applies; every other consumer still
      exits through the side global, and last-rewrite-wins remains its stated
      defect. Condition 3's spines are matched, but the gate prevents no
      rewrite and cannot until the rewritable set exceeds the HIGH set, so
      the condition is restated rather than ticked: *it closes when the pass
      can soundly rewrite a shape that verdicts LOW or MED.* That needs a
      rewrite that does not require an `exp`. Bounded-weight support was the
      candidate named here until it landed 2026-08-17 for the constant case
      and did not close it: it widened which HIGH loops are rewritable and
      added no LOW or MED one. Adding further matched-but-declined shapes
      cannot close it either, however many are added. The inverted assertion in
      `run_pass_test.sh` turns red on the day this changes. None of this
      moves the posture: neither number that drove the decision (5 HIGH rows
      in 2859 loops; the rescue unobservable without the side global for
      every shape but one) changed.
      *If the `-inf` fix lands:* condition 1 closes and all three of
      Deliverable 2's stated preconditions are met, so the pass becomes
      blocked on product concerns rather than on preconditions. The
      posture does not change: neither number that drove it moves.
- [x] **Diagnostic coverage statements.** Done 2026-08-17. Both statements
      landed with the recognizer change: `README.md` no longer implies the
      diagnostic flags the forward algorithm (it says the shape is matched
      and grades LOW, and why), and `DIAGNOSTIC.md`'s "Scope limits" states
      measured selectivity. This was the last item marked as blocking the
      diagnostic being called shippable.
      *Original entry, kept for the record:*
      (blocks calling the diagnostic
      shippable, even as beta). The posture is only honest if the reader
      meets the gaps where they meet the tool. DIAGNOSTIC.md's "Scope
      limits" already carries the mechanical ones (shape-lint-not-range-
      proof, memory-carried, mirrored cell, cross-loop decay, vectorized,
      risk-is-an-ordering). Two statements are missing, and both live
      *outside this tracker's file ownership*. Proposed text is in the
      posture report, to be applied by whoever owns those files:
      * README.md names three motivating shapes; the diagnostic flags
        **two**. The forward algorithm is now *matched* in both forms
        (recognizer change, 2026-08-17) but grades LOW in both, because
        the magnitude decay lives in the enclosing loop. README must stop
        implying coverage: seeing a shape and flagging it are different
        claims, and only the second is what a reader wants.
      * DIAGNOSTIC.md must state measured selectivity, 5 HIGH in 814
        hits in 2859 loops, so a one-finding report can be read.
      *Retracted 2026-08-16.* This item also claimed RESULTS.md's triage prose
      still read "Three rows... the static signal marks two source sites". It
      does not, and has not since the style sweep in a1ec2d4; the bullet was
      the stale thing, not the file. RESULTS.md states 5 HIGH rows across 4
      distinct source lines in all four places it gives them, and every count
      in it re-verified against `data/raw-*.txt`: 2859 loops, 783 hits, 7
      transcendental rows across 5 source lines, 5 HIGH across 4
      (`blas.c:315` twice, `go.c:562`, `gaussian.c:205`, `zeta.c:757`).
      *One real staleness found doing that, fixed:* the header paragraph
      credited the re-scanned totals to the pre-correction gate, 4 hits / 6 FP
      loops / 1 transcendental. The re-scan ran under 5 / 7 / 2. Both are now
      stated with which scan each governed.
- [x] **Pass eligibility contract** (intent Deliverable 2, precondition 2).
      Done 2026-08-16. The old gate (`"unsafe-fp-math"="true"` or `<force>`)
      conflated reassociation permission with permission to change errno,
      FP exception flags, rounding mode, denormal handling and special
      values. Four restrictions now, none overridable by `force`, which
      waives reassociation *proof* and nothing else: **`llvm.exp.*` only**
      (measured: at `-O1` clang emits `call double @exp`, with
      `-fno-math-errno` it emits `llvm.exp.f64`, so the intrinsic *is* the
      errno contract, since the rewrite deletes N source `exp` evaluations
      and emits 2N different ones plus a `log`); **`strictfp` rejected**;
      **constrained-FP ops rejected**; **non-IEEE `denormal-fp-math`
      rejected**. Contract in `pass/ELIGIBILITY.md` (normative; PROTOTYPE.md
      is the narrative and measured record).
      Four decline tests, each observed failing against the previous build
      first: it rewrote both the errno case and the denormal case under
      `force`, and emitted nothing at all for strict-FP.
      *Consequence:* the harness needs `-fno-math-errno` or the kernel no
      longer matches; it now also pins `clang-21` beside `opt-21` (it fed
      unversioned `clang` into `opt-21` before) and deletes the plugin
      before building, so a failed build cannot leave stale code under test.
      *Honest limit:* the pass now covers a strictly narrower set of real
      TUs than the matcher reports hits in. The way out is the documented
      extension point: accept a direct `exp` when IR attributes prove no
      errno effect. Unimplemented, and it needs accept and decline tests.
- [x] **Pass reference oracle corrected.** Done 2026-08-16.
      `ref_logsumexp()` in `pass/test_softmax.c` applied the max-shift
      unconditionally, so `x_i - M` was `inf - inf`: it returned NaN for
      all-`-inf` and for any input containing `+inf`. The infinity tests
      added hours earlier asserted against constants only and were
      therefore **not** reference-validated, which is weaker than they
      read. Fixed by handling NaN, `+inf` and all-`-inf` before the shift;
      constants kept as belt and braces. Added NaN-mixed-with-infinities
      (which pins the ordering: NaN beats `+inf`, matching the linear loop)
      and a zero-trip case asserting the export global is *not* written.
- [x] **Diagnostic ergonomics.** Done 2026-08-16. `matcher/logrange-scan.sh`
      takes a build directory or a `compile_commands.json` and prints the
      report: preflight, plugin build, per-unit bitcode at the study's flags,
      matcher, `diagnose.sh`. The target is never built and need not be
      buildable by clang. `matcher/test_scan.sh` is gate 4 in CI, 12 cases,
      and was negative-tested by three mutations (drop the compile-failure
      counter, drop entry dedup, grade LOW as HIGH), each caught by the
      intended assertion. WSL/LLVM-21 packaging is `SETUP.md`, with
      `logrange-scan.sh --check` as the machine-readable half.
      *Three failure modes the gate exists for, all found by building it:*
      a unit that fails to compile would have produced a short report that
      reads as a clean one (now exit 2, `--allow-compile-failures` to
      override); one source listed by two cmake targets would have been
      counted twice; and a unit clang rejects over a gcc-only flag it did
      not need would have been dropped silently (now retried with
      preprocessor and language flags only, and the retry is counted).
      *Found by pointing it at this repository:* the report printed
      `flagged: exp-sum`, `diagnose.sh`'s passthrough for a reason token it
      has no sentence for. The matcher started emitting `exp-sum` on
      2026-08-15 and the renderer was never taught it.
      `testdata/fixture-raw.txt` said it exercised "every report branch" and
      had no runner, which is the third fixture in this repo found checking
      nothing. Sentence added; case 11 now extracts the token list from
      `SumOfProductsMatcher.cpp` and fails when the fixture or the renderer
      misses one.
- [x] **Emitted-code error bound** (posture condition 4). Done 2026-08-16.
      `rel err <= (n + 3k + 4 + D)*u + (|log|S|| + |log|net||)*u`, normative in
      `pass/ELIGIBILITY.md`, searched by `pass/emitted_bound_search.c` against
      the object the pass actually rewrote, gated in `run_pass_test.sh`.
      **Held across 7285 trials, worst observed/bound 0.99.** The gate was
      negative-tested by halving the bound: 1261 violations, worst 1.98, exit
      1.
      *The derivation is pos_accum's plus 1u for the final `exp`, and that
      inheritance is not free.* The branchless guarded form matches
      `pos_accum` term for term only because `exp(0)` is exactly `1.0` and
      `s*1.0` is exact. Under a merely-1-ulp `exp(0)` the emitted code would
      pay `n*u` the runtime never pays. Both facts are asserted at startup
      rather than trusted.
      *The expected refutation did not materialize.* `3k*u` charges nothing
      for the size of a reference jump, the same omission that refuted
      `rp_accum`; ascending families reach only 0.23. Mass is why: after a
      jump of `J` the old sum's share is `s/(s + e^J)`, so the surviving
      error is `J*s/(s+e^J)*u`, maximized near `J ~ ln n` and worth a
      fraction of a `u` against a `3u` budget. The binding case is instead a
      one-depth cluster where the running sum's roundings align, giving the
      classical `(n-1)u`; the ratio approaches 1 from below and plateaus at
      0.990 for N = 2048, 4096, 8192 and 16384.
      *Two things the runtime's contract does not have.* The emitted code
      returns a linear double, so `|log|S||` cannot exceed 709.78 and the
      reduction term is capped near `710u`. And the first-order form needs
      `n` under ~2.1e8, derived from `(n-1)(n+4)u > 5`, where recursive
      summation's second-order term eats the constant. The search reaches
      n=16385, four orders below it.
      *The reduction term is required here by measurement.* Every run also
      scores the form without it: exceeded on 321 of 7285 trials, worst 39x.
- [x] **`add_scaled` injects `|log c|·u` that no contract term names.**
      Closed 2026-08-16. **Decided: a documented scaling cost stated at
      `add_scaled`, not a change to the accumulator contract.** A caller
      writing `add_log(v.log_abs + std::log(c))` by hand pays exactly the
      same, so it is the price of entering log space at that scale, not of
      the accumulation. Folding it into the headline bound would misattribute
      it.
      *Measured, swept:* the injected error is `ulp(|log c|)/2`, landing on
      8u, 64u, 256u, 512u at `|log c|` = 10, 100, 400, 690 — exact powers of
      two, the signature of a half-ulp effect. At the top of the range that is
      **128x** the 4u a single term gets. The stated claim
      `(|log c| + 4)*u` holds at worst **0.74**, searched and asserted by
      `bound_search` family F.
      *The representation floor does not cover it*, which is what made this
      a real gap rather than a restatement: that floor is `|log|x||*u` for
      the value being represented, and here the scaled term's own magnitude
      is ~0 while the injected error is 512u. It is set by an **input's**
      magnitude, not the result's — the third instance of the class that
      required `D` and then `|log|net||`.
      *Caller guidance, measured:* when `c*|x|` is representable, forming it
      and using `add()` costs **1.9u independent of scale**, against 512u
      here. Callers whose product overflows have no cheaper route.
      *Two measurement traps, both now pinned by the gate.* The first probe
      built `c = exp(-L)` and measured 0.6u, three orders under prediction.
      `exp` and `log` are self-consistent round trips, so `log(exp(-m))`
      lands within `u` of the double `-m`, which at `|log c| ~ 690` is 1e-3
      of an ulp: the rounding being hunted is hidden by construction. The
      second version swept `c` but still built it through `exp()` and
      measured the same. Only an arbitrary mantissa exposes it. Mutation-
      tested both ways: dropping `|log c|` from the claim fails the
      assertion, and rebuilding `c` via `exp()` collapses the ratio to 0.04
      and trips the "the cost is real" check.
      *Original entry, kept for the record. It carried its own `- [ ]` until
      2026-08-17, which made a closed item register as open in any scan for
      unchecked boxes:*
      ~~**`add_scaled` injects `|log c|·u` that no contract term names.**~~ Both
      accumulators implement `add_scaled(v, c)` as
      `add_log(v.log_abs + std::log(c))`. `std::log(c)` is computed, so it
      carries an absolute error of `|log c|·u`, which lands directly in that
      term's log-magnitude and therefore as a relative error of the same size
      on the term. At `c = 1e-300` that is 690u on the term. Nothing in either
      contract covers it: `cond·(3k+4+D)·u` is the accumulator's own
      arithmetic, and `(|log|S|| + |log|net||)·u` is the final reduction.
      *Two defensible readings, and choosing between them is the work.*
      Either the contract covers what the accumulator does with the terms it
      is **given**, in which case one sentence at `add_scaled` stating that
      terms enter carrying `|log c|·u` closes it; or `add_scaled` is part of
      the accumulator's surface, in which case the contract needs a term and
      `bound_search` needs a family driving `c` to extremes.
      *Not speculative.* This is the same structure that refuted the contract
      on 2026-08-16: an addend that is itself computed, whose magnitude is
      unbounded relative to the result, charged nowhere.
      *Scope, established rather than assumed.* That defect needs both
      properties at once, and the rest of the header does not have them.
      `logsumexp2` computes an addend but it is bounded by `log 2` by
      construction, so the `(d+3)·u` covers it. `log_mul` and `log_div` take
      both addends from the caller, so neither is computed. The round trip
      already charges `|log|x||·u`, the input's magnitude. Stated at
      `logsumexp2` in the header so the enumeration is not re-derived.
- [x] **Settled 2026-08-16, and it found a fourth refuted contract — but not
      the one this item predicted.** The rescale argument rounding is real and
      is now documented, but it is *covered*: mass damping and, at large J,
      the `|log|S||` term. What is not covered is the **final reduction's
      other addend**. `out.log_abs = m_log + log|net|` charged `u*|log|S||`,
      the rounding of the addition's result, and ignored that `log|net|` is
      computed to a relative `u` and so an absolute `u*|log|net||`. The two
      cancel exactly when the sum is near 1, and there `|log|S||` is zero
      while `|log|net||` is not.
      *Witness:* `bound_search` family E, n equal positive terms at
      `L = -log(n)`. `net = n` exactly, no summation error at all, `cond = 1`,
      `k = 0`, `D = 0`, `|log|S|| = 0`, so the old budget is `4u`. At
      n = 166463 the error is **7.97u**, ratio **1.99**. New form
      `cond*(3k+4+D)*u + (|log|S|| + |log|net||)*u` holds at worst 0.83.
      `pos_accum` is not refuted (0.80): `|log|net|| <= log n` and its `n*u`
      term dominates. `rp_accum` is exposed because Neumaier compensation
      removed that `n*u`.
      *The reference was rebuilt on the way* (`tests/dd_exp.h`): double-double
      exp assuming nothing about libm, validated by identities needing no
      external constant, and kept wide through the subtraction. The old one
      had two floors, ~1u from double `exp()` per term and u/2 from `truth`
      being a double at comparison — the second structural and undocumented.
      Resolution went from ~1u to ~1e-14 u. Two assumptions became
      a measurement: `std::exp` worst 1.00u against `dd_exp`, reported by
      `bound_search` on every run.
      *Stated honestly:* family E's 1.99 would have been visible against the
      old reference. The family is what found it; the reference is what made
      the marginal 1.16 plateau case readable and the 0.83 trustworthy.
      *Original entry, kept because the prediction was wrong in an
      instructive way:*
      *The documentation defect is certain.* `log_math.h` states "Each
      rescale event multiplies the standing sums by a factor carrying <= 2u
      (exp) + u (multiply)". The rescale's `exp` argument is `m_log -
      log_abs`, of magnitude `J`, so `fl()` of it is off by up to `u*J` and
      the factor carries `u*J + u`, not `2u`. This is the **identical
      omission that refuted the per-term accounting** (charging `2u` for a
      term's `exp` while ignoring its argument rounding, which became `D`),
      still present in the sibling clause about rescales.
      *Why `pos_accum` is safe, and why that argument does not transfer.*
      The surviving share of a rescale's error is the old sum's mass share
      after the jump, `s/(s + e^J)`, so the contribution is
      `J*s/(s+e^J)*u`, maximized where `s = e^J(J-1)` at `(J-1)*u`. Building
      `s` that large takes `~e^J` terms, so `J <~ ln n` and `pos_accum`'s
      `n*u` term swamps it. Measured: a plateau-then-step family reaches
      only **0.02** of `pos_accum`'s bound at n up to 1e6.
      **`rp_accum` is Neumaier-compensated and therefore has no `n*u` term to
      absorb it.** Its budget at `k=1, cond=1` is a flat `7u`.
      *What is not established.* The same family, shifted so the total is
      ~1.0 and the `|log|S||*u` term cannot pay for the jump, reaches
      **1.14** of `rp_accum`'s bound at N=1e5, J=2: 8u observed against 7u.
      That is inside `bound_search.cpp`'s own stated reference floor, ~1u,
      because the dd reference sums `exp(L_i)` computed in plain double. It
      is also not the hypothesized mechanism, which would give 2u at J=2.
      **So this is neither a refutation nor a clean pass; the instrument
      cannot resolve it.**
      *The way to settle it, and it is cheap.* Give `bound_search.cpp` the
      reference `pass/emitted_bound_search.c` already uses: `expl()` at a
      64-bit mantissa, compensated. Positive-term relative errors average
      rather than accumulate, putting the floor near `0.001u` instead of
      `1u`. Then add the two families `pos_accum` has never been searched
      with: ascending (`k = n-1`) and plateau-then-step. Today `pos_accum`
      is searched by magnitude, depth-cluster and random only, and random
      ordering leaves `k` at its incidental `O(ln n)`, so the `3k*u` term
      has never been searched at the input that maximizes it.
      *Consequence either way:* if the bounds hold at the finer floor, the
      published worst-observed figures (0.85 and 0.79) move slightly and the
      derivation text still needs the `u*J` correction. If one exceeds 1.0
      clear of the floor, that is a fourth refuted contract.
- [~] **Pass shape coverage — fmuladd and `w[i]*exp(t)`.** Done 2026-08-17.
      **Posture condition 3's literal text is met; its property is not.**
      Audited the same day and the closure did not survive: the gate is
      *reachable* (a matched shape verdicts LOW and is refused at the default
      threshold) but not *load-bearing* (no rewrite is prevented by it, and
      none can be). Eligibility requires an accepted `exp` term and a HIGH
      verdict means the same thing, so `rewritable ⊆ HIGH` by construction;
      every LOW loop is weighted and the weight clause refuses it at any
      threshold. Measured: the rewrite set at `min-risk=low` is identical to
      the default, now asserted. Condition 3 restated under "Tooling — ships
      as beta" above to name its own success: the gate closes when the
      rewritable set exceeds the HIGH set, which needs bounded-weight
      support, not more
      matched-and-declined shapes. Left open on that basis.
      *Design constraint recorded 2026-08-17, before any of it is built.*
      Four stages stay separate: (1) `RecurrenceDescriptor` — is it a
      reduction; (2) this pass — does the term decompose as `exp(t)` or
      `W*exp(t)`; (3) weight analysis — is `W` provably acceptable; (4) the
      rewrite — does the emitted code consume `W`. Moving stage 1 to LLVM
      changed nothing about 2–4 and must not be read as having made them
      safer: `RecurKind::FMulAdd` says the update is `fmuladd(a, b, phi)`
      and nothing about which operand is the weight or whether the other
      reaches an `exp`. Stage 4 has no natural enforcement and is exactly
      where the `s += 0.5*exp(x)` bug lived, so the accept path must make an
      unconsumed weight unrepresentable — the rewrite takes `W` as a
      required input — rather than relying on a test to catch it.
      `ELIGIBILITY.md` 3.3 carries the table.
      *Bounded constant weights implemented 2026-08-17, and the condition
      still does not close.* A positive finite constant is now folded as
      `exp(t + fl(log w))`; the four stages are linked by `WeightPlan`, whose
      `exponent()` is the emission's only route to the value it
      exponentiates, so an analysed weight cannot fail to be consumed —
      structure, not a test. Section 5 gained two searched terms,
      `(|log w| + max|t'|)*u`, held at 0.96 over 4482 trials and shown
      necessary (without them: 19 exceedances, worst 1.53x).
      **What it did NOT do is make the gate load-bearing.** The rewrite
      requires an accepted `exp`, and any loop with one verdicts HIGH, so
      `rewritable ⊆ HIGH` still holds by construction and the min-risk
      equality assertion still passes. An earlier plan for this item
      predicted that assertion would turn red; it does not, and the
      prediction was wrong about why. Closing needs a rewritable shape that
      verdicts below HIGH, which means a rewrite that does not require an
      `exp` — not more weight coverage.
      *What the audit also found, all fixed 2026-08-17:* the suite was a
      closed-world whitelist — "these 4 names are rewritten" and "these names
      are bit-identical" were two independent hand-maintained lists with
      nothing tying them, so relaxing the weight clause to admit constant
      weights produced a pass that rewrote `s += 0.5*exp(x)` with the `0.5`
      silently dropped **while the entire suite stayed green**. The rewritten
      set is now derived from the pass's own output and each member must
      carry a passing `cover_<fn>_*` numeric assertion.
      The pass now *matches* three
      spines — `fadd(phi, X)`, `fadd(phi, fmul(W, X))` and
      `llvm.fmuladd(W, X, phi)` — and rewrites only the first. Both weighted
      forms are needed because which one clang emits is `-ffp-contract`, a
      flag the pass does not control (measured: `=on` gives `fmuladd`, `=off`
      and `=fast` give `fmul` + `fadd`); the gate compiles the kernel all
      three ways and asserts the uncontracted builds contain no `fmuladd`, so
      the check cannot go vacuous.
      *The point of matching what it will not rewrite:* a shape the pass says
      nothing about reads exactly like a shape it failed to recognize, and
      sends the next reader to the matcher instead of to the contract. So
      weighted spines are declined `DECLINE-WEIGHT,...,unbounded-weight`.
      The risk gate now declines a real input at the default threshold:
      `dot_sum`'s `llvm.fmuladd(x, y, phi)` has no `exp` in its chain,
      verdicts LOW, and is refused. Previously every matchable shape required
      an `exp`, so the verdict was HIGH by construction and only a
      synthetically raised `min-risk` could exercise the refusal path. This
      is reachability only — see the property note above.
      *Order is load-bearing and is now normative* (ELIGIBILITY.md 3.3): risk
      gate first, weight clause second. Reversed, the weight clause shadows
      the gate and `dot_sum` never reaches it — negative-tested, and it fails
      the `dot_sum` assertion exactly as predicted. Three further mutations
      caught: verdict hardcoded HIGH, `noMulNoExp` screen removed (adds two
      declines), `fmuladd` spine unmatched (removes both).
      *Corrected while doing it.* The magnitude witness published on
      2026-08-16 was wrong in both constants: `w=1e300, t=-700` does **not**
      overflow at `n>=2` (the state reaches 2e300, and `sum|w_i|` needs
      `n ~ 1.8e8` at that weight), and the linear sum there is 1.97e-4, not
      ~1e-5. Measured witness: `w=(1e308,1e308)`, `t=(-700,-700)` drives the
      state to `inf` while the linear loop gives 19719.4.
      *Also recorded, not used as the reason:* the emitted reduction
      `exp(m + log(s))` gives NaN for a negative state (`w=(1,-2)`,
      `t=(0,0)`), but that is a property of the reduction and is fixable with
      `copysign` — consistent with the 2026-08-16 retraction. Magnitude is the
      durable constraint, since no reduction recovers an overflowed state.
      *Still open from the original item:* fsub accumulators.
      *Yield evidence, corrected 2026-08-17 and weaker than published:* the
      census found 0 `w*exp(t)` multiplies, but among the **5** exp-carrying
      reductions the matcher accepted — not among 2859 loops, which is how it
      was stated in four files. It is gated on `expChain` and runs after the
      `HIT`, so it cannot see loops rejected upstream, including the mirrored
      `out[j] += w*exp(t)` form that `cleanUses` rejects. n=5 does not settle
      whether the extension point is worth taking; deciding that needs the
      census moved ahead of the rejection filters. Derivation:
      `matcher/run_study.sh figures`.
- [x] **`pass/` reimplemented the reduction analysis the matcher stopped
      reimplementing.** Audited and left 2026-08-17, done the same day.
      `isReductionPHI` filtered to `FAdd`/`FMulAdd` now identifies the
      accumulator; `getRecurrenceStartValue` and `getLoopExitInstr` replace
      the hand-read incoming values; the phi/update clean-use pair is gone.
      **Measured identical**: pre- and post-migration runs of
      `run_pass_test.sh` in isolated worktrees give the same 65 assertions,
      the same rewrite and decline records, and byte-identical `INFO` values
      including every accuracy ratio and the bound-search worst case (0.99
      over 7285 trials).
      *Two things deliberately kept.* The stricter clauses are rewrite
      legality, not recognition, and `isReductionPHI` does not imply them:
      single FP phi in the header, double only, constant-zero start, unique
      exiting and exit block. And `soleInLoopUser` survives for one use — the
      `fmul` in `fadd(phi, fmul(W, X))` is a term-side node, not part of the
      reduction chain, so no reduction analysis has an opinion about who else
      reads it.
      *Cost:* the pass inherits the matcher's pipeline preconditions. The
      supported pipeline became
      `-passes='loop-simplify,lcssa,log-rewrite<force>,adce'`, normative in
      ELIGIBILITY.md section 0. Without the prefix the pass declined every
      loop and reported nothing — found immediately, because the gate failed
      with `FAIL: rewrite missing for softmax_denom_rw`.
      *The `adce` suffix was dropped 2026-08-21*, when the pass started
      deleting the chain it orphans (posture condition 6, above). The
      `loop-simplify,lcssa` prefix stays. It is a precondition of the
      recognizer, not of the transform, which is what this item is about.
- [ ] **Matcher blind spots.** Vectorized loops remain a documented miss;
      decide whether v1 chases them or the docs stay the answer.
      *Correction 2026-08-15:* this item briefly claimed the forward
      algorithm lands in the memory-carried gap. It does not. An
      instrumented matcher shows `out[j] += ...` is promoted to a register
      accumulator and then rejected by the **mid-loop-read guard**, because
      LLVM keeps a store mirroring the value to `out[j]` each iteration and
      the update ends up with two in-loop users. Genuinely memory-carried
      reductions remain a separate, still-open gap; this shape is a
      guard-precision problem, tracked in the next item.
      *Closed in part 2026-08-17:* the mirrored-cell half is gone. Moving
      recognition to `RecurrenceDescriptor` admitted 13 such sites across
      the corpus, `coverage.c`'s `forward_step_mem` among them; all grade
      LOW. A reduction whose accumulator is never promoted to a register at
      all is still a miss, and vectorized loops still are. matcher/DELTA.md.
- [x] **Mid-loop-read guard precision — decided 2026-08-15: not for v1;
      moot 2026-08-17, the guard no longer exists.** `RecurrenceDescriptor`
      distinguishes an invariant-address mirror from an observed running
      value as part of recognizing the reduction, so the refinement below
      was never implemented and no longer needs to be. `midread` is still
      correctly rejected and `selftest.c` still asserts it. The measured
      yield below is preserved because it is what justified declining, and
      13 of those 23 sites are now hits (matcher/DELTA.md).
      The guard rejects any update with a second in-loop user, catching both
      prefix sums (`midread`, correctly: the intermediates are observed) and
      accumulators merely mirrored to a fixed cell (`out[j] += ...`). A
      differential set at `-O1` showed `L->isLoopInvariant` on the store
      address separates the two, so admitting the mirrored case looked
      feasible. Measuring the yield first killed it.
      Over the study corpus, serially and reproducibly:
      **23 of 460 cleanUses rejects, 5.0%**, would be admitted. All are
      linear-algebra and mean reductions (`gsl_spblas_dgemv`,
      `gsl_eigen_nonsymmv`, `genv_get_right_eigenvectors`, `cquad`,
      `steffen_eval_integ`, darknet's `mean_cpu`, `forward_avgpool_layer`,
      `backward_batchnorm_layer`) with no `exp` in any chain, so all would
      grade LOW, which `diagnose.sh` prints only as a count. It would also
      not recover the shape that prompted it: there is no forward-algorithm
      code in GSL, darknet or libsvm, so `coverage.c`'s case is synthetic.
      Meanwhile 294 of 460 rejects (64%) come from extra users on the phi or
      another spine node and are untouched by this refinement.
      Verdict: alias-analysis machinery for a 5% slice yielding LOW-risk
      sites the diagnostic does not print. Not worth it for v1. Mirrored
      accumulators are documented as outside diagnostic coverage instead
      (DIAGNOSTIC.md), and `coverage.c` stays as the standing check.
      *Revisit only if* the project later grows alias/dependence analysis and
      cross-loop risk modelling. The second is what would make these sites
      worth reporting at all, and it is still open.
- [ ] **Per-loop risk cannot see cross-loop decay.** Found 2026-08-15 by the
      coverage audit. When the matcher *can* see the forward algorithm (the
      register-accumulator form), the triage grades it LOW: `nMul = 1`, no
      transcendental. The underflow is real but lives in the outer loop:
      probabilities decay across time steps while each inner reduction looks
      unremarkable. So the diagnostic would not flag one of the shapes the
      README names as motivating the library. Either the risk rule gains a
      cross-loop signal (magnitude trend across an enclosing loop), or the
      docs stop implying the diagnostic covers this family. Decide before
      the diagnostic is called shippable.
      *Decided 2026-08-16 by the shipping posture: the docs branch, for
      1.0.* A cross-loop signal is the same analysis the guard-refinement
      measurement already declined to fund, and it is the item that would
      make those 23 sites worth printing, so it stays open as the
      revisit condition, not as 1.0 work. The doc change itself is the
      README bullet under "Diagnostic coverage statements" above.

      *Scoped 2026-08-17, and one assumption it rested on turned out wrong.*
      The fixtures now exist: `coverage.c` carries `forward_full_flat` and
      `forward_full_swap`, the shape WITH its enclosing time-step loop, both
      asserted as hits **at LOW** — the current, wrong-for-the-user verdict.
      Cases 3 and 4 are one time step each and never could have exercised
      this, so until now there was no target to fail against. These turn red
      the day a signal lands, which is the point of asserting the gap.

      **The candidate rule.** For an innermost reduction in a loop with a
      parent: the result is stored to an object that a term in the same nest
      loads from on a later parent iteration. `getUnderlyingObject` on the
      store and the load is the cheap proxy; full alias analysis stays out of
      scope, as it did for the guard refinement.

      **Do this before writing any of it, because the ordering is not
      recoverable afterwards: decide whether the signal promotes LOW to MED
      or LOW to HIGH.** It changes the published HIGH count, and picking the
      tier once the figures exist is choosing the number first. METHODOLOGY's
      own rule — fix the rule, then count — applies to this exactly.

      **The yield estimate above is wrong and was wrong in the safe
      direction.** "No forward-algorithm code in the corpus, so this recovers
      nothing" reasons from the *shape*; the proxy keys on a *pattern* —
      output feeding the next outer iteration — that is much broader.
      Iterative solvers, power iteration, Jacobi and Gauss-Seidel all match
      it, and GSL is full of them. So the population is unmeasured and
      plausibly large, this is not a quiet change, and it needs the full
      delta treatment (`matcher/DELTA.md`'s pattern: both rules over the same
      loops, publish, then regenerate `FIGURES.txt`).

      **Cost, having looked.** `walkChain` treats `Load` as a leaf and
      discards the address, which is the one fact the rule needs, so the term
      walk has to start collecting them. Roughly 60–100 lines plus that
      threading, then the evidence ceremony. Half a day for the code; the
      tier decision and the delta are the larger half.

      *Measured and declined 2026-08-21. See `matcher/XLOOP.md`.* The rule
      was built and fixed in advance, then run: **231 of 814 hits (28%) carry
      cross-loop feedback**, 223 LOW and 8 MED, and **none is
      transcendental**. 192 of the 231 are `cblas_*` triangular routines; the
      rest are cquad, FFT transforms and Householder/QR. Not one is a decaying
      recursion — they are in-place linear algebra, which reads and writes one
      buffer across its outer loop exactly as the forward algorithm does.
      The rule detects **feedback**, which is the precondition for decay and
      not evidence of it. Both promotions fail on those numbers: LOW→HIGH
      takes HIGH from 5 to 236 and refutes the selectivity `DIAGNOSTIC.md`
      advertises; LOW→MED takes MED from 57 to 280, and `diagnose.sh` prints
      MED individually where LOW is a count. A narrowing was looked for and
      needs SCEV analysis of access ranges — outside this cost tier, the same
      boundary the guard-refinement decision set.
      **What shipped is the census, not the signal:** `run_study.sh xloop`,
      evidence in `data/raw-xloop.txt`, `HIT` stream byte-identical with the
      census on or off, no published figure moved. The detection code and both
      fixtures stay, so revisiting costs a rule change rather than a rebuild.
      *Revisit condition:* a signal that separates decay from feedback — a
      magnitude-trend argument over the parent's induction, which is a much
      larger analysis than the one measured.
      *Not blocking 1.0 either way* — the diagnostic ships with the gap
      stated, which is the branch the posture already took.

## Explicitly not blocking 1.0

- [x] **Formal-bound presentation cleanup.** Done 2026-08-15, in
      `log_math.h`, `BENCHMARKS.md` and `CHANGELOG.md` together. Both forms
      are labelled first-order under their stated assumptions; the reset
      discard is shown to be inside the existing budget rather than listed
      beside it; and `cond` versus `|log|S||·u` is stated as a division of
      labor (error in computing the value versus error in representing it).
      *One step added while writing it up:* the reset argument needs epoch
      **disjointness** to hold. `Σ Aⱼ ≤ ½·cond·|S|` follows because each
      reset's epoch is disjoint from the next (clear() starts a fresh one),
      so the Aⱼ sum telescopes into the total mass. Without that step the
      sum over ρ resets appears to grow without bound and the coverage
      argument does not close.
- [x] **Installed-package comment cleanup.** Done 2026-08-15. The template
      described flag propagation that was removed when the FMA diagnosis was
      corrected; the shipped file now matches CMakeLists.txt and README.md,
      and says what actually protects the contract (the fast-math #error).
- LLVM version breadth (21-only is fine for a research tool).
- Windows-native LLVM builds of matcher/pass (WSL is the supported path).
- The 8 GSL "unverified" precision-audit rows (inlining artifacts; sampled,
  documented, not worth chasing).
- [ ] **Stretch goal: end-to-end log-form propagation.** Specified
      2026-08-16 in logrange_intent.md, "Stretch Goal — End-to-End Log-Form
      Propagation": a three-point SSA lattice (Linear / Log / Conflict) that
      never hand-places a conversion, with the matcher's risk analysis as
      the legality oracle for where materialization is safe. Stopping rule
      and prior art (Q/DQ, TAFFO, LNS) stated there.
      *First milestone passing 2026-08-16.* `propagate=div` rewrites the
      softmax normalize divide to `exp(t - L)`, carrying the log form
      through the loop-exit merge. At inputs near −800 the linear form is
      all NaN and the propagated form sums to 1.0000000000000262, with no
      side global read. That closes **condition 2** of the shipping posture
      for this one shape.
      *What the design still lacks:*
      — the vocabulary is one rule. `fmul → fadd` and `fadd → logsumexp`
        are unimplemented, so the log region cannot grow past a single
        divide and the "frontier" the design describes has never formed.
      — the legality oracle is not wired. Nothing consults the risk
        analysis to refuse a materialization that would re-underflow;
        propagation simply does not happen anywhere else.
      — success criterion (2) is refuted as written. 64 of 64 swept trials
        put propagation 1.33x to 13.9x behind the linear re-conversion,
        long-double reference error 1.7e-18. The criterion compares one
        conversion against one re-conversion, which is where propagation
        has least to offer. Chains are the claim and are unmeasured,
        because the vocabulary is one rule. Amended in the intent.
        Each step adds a term proportional to the log-magnitude it
        crosses, so a chain must carry a measured accuracy budget rather
        than an assumption that log form is free.
      The diagnostic is the shipping front door either way, so nothing in
      1.0 waits on this.
