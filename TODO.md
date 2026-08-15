# Road to 1.0

What "1.0" means here: the **header is the product** (intent Deliverable 1) —
it must be trustworthy on a machine we've never seen. The tooling (matcher,
diagnostic, pass) ships alongside at whatever maturity it honestly has, each
labeled. Items ordered roughly by how much they'd embarrass us if skipped.

Sections are not strictly in execution order. The bound review ran first, on
the reasoning that it could move the error contract and the rest of 1.0 is
written on top of it. **It did move the contract** (see below), which is why
it ran first. The float decision followed it, since that is a statement about
the scope of the corrected bound. **Next up is the second-machine benchmark
run**, then packaging.

## Blocking 1.0 — library

- [x] **License file.** MIT, added 2026-08-15.
- [x] **Second toolchain, in CI.** Done 2026-08-15. CI matrix is now
      windows-msvc + ubuntu-gcc + ubuntu-clang. The gcc/clang flag branch
      (`-Wall -Wextra -Werror -ffp-contract=off`) needed no code fixes:
      clean under gcc 15.2 and clang 21.1. test_accuracy holds on glibc with
      6×–5300× slack (against the corrected bound), so its 1-ulp `exp()`
      assumption survives a second libm (BENCHMARKS.md, "Second toolchain").
      Verified green on the
      runners (gcc 13.3, clang 18.1) as well as locally (gcc 15.2, clang
      21.1); the accuracy table is bit-identical across all four.
- [x] **Version identity in the header.** Done 2026-08-15.
      `LOGRANGE_VERSION_MAJOR/_MINOR/_PATCH`, ordered `LOGRANGE_VERSION`, and
      `LOGRANGE_VERSION_STRING` in log_math.h, plus CHANGELOG.md. The header
      is the source of truth: CMake parses it (the hardcoded `project(VERSION
      0.1)` had already drifted a release behind), and test_log_math checks
      the string against the numeric parts. Both guards negative-tested.
- [x] **Float support decision.** Decided 2026-08-15: **double only, by
      design**, stated in the header's Precision block. The reasoning is the
      bound's: every constant in the contract is double-specific — u = 2⁻⁵³,
      the ~745 log-unit vanishing window (subnormal floor 2⁻¹⁰⁷⁴), and now
      the depth term D that the same window caps. Float is not a typedef; it
      needs the bound re-derived at u = 2⁻²⁴ with a ~103 log-unit window
      (2⁻¹⁴⁹) and an accuracy reference finer than the tests' double-double.
      The rewrite pass already declines float accumulators, so double-only is
      coherent across the toolchain. Callers with float data widen at the
      accumulator boundary. Implementing a float variant stays optional and
      is now explicitly out of scope for 1.0.
- [x] **Second-machine benchmark run.** Done 2026-08-15, run `31865095928`
      via `.github/workflows/bench.yml` (workflow_dispatch only — timings
      never run on push). The open question answered itself: the
      windows-latest runner reported a **0.00%** noise floor, better than the
      author's machine, because the harness's pinning and priority raising
      work there and MSVC matches the published flag set. Hardware was the
      only variable.
      Criterion 1 is bit-identical across three configurations
      (−792.643769699630184, |error| 0). Criterion 3 reproduces (2.26× vs
      2.2×).
      *Found doing it:* criterion 2's margin is hardware-dependent, not a
      constant. `stream_lse`/`pos_accum` is 3.7× here and 5.27× on the
      runner, because `pos_accum` barely moved between machines (+4%) while
      `stream_lse` slowed 49% — one `exp` per term versus `exp` + `log1p`,
      so slower transcendentals punish the textbook stream twice. The
      durable claim is the direction and its reason, margin 1.65×–5.3×
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
      off — there is no multiply-add pair in the compensation path to fuse.
      The 14× was `test_accuracy`'s corpus being *generated* with
      multiply-add expressions that contract, producing different data; the
      uncompensated `log_add` fold moved too, which should have been the
      tell. What genuinely breaks the accumulator is reassociating math
      (4.9e-6 relative under `-ffast-math`), so the header now refuses to
      compile under it and no flag is imposed on consumers.

## Blocking 1.0 — honesty debts

- [x] **Bound review pass.** Done 2026-08-15, and the bound did not survive.
      The cond·(3k+4)·u form is refuted: `tests/bound_search.cpp` finds 151
      of 400 random inputs over it (worst 15.8×) and a constructed
      counterexample at 5.8× with cond=1, k=0. Two terms were missing — the
      rounding of exp's *argument* (mass-weighted mean depth D) and the
      final m_log + log|net| reduction (|log|S||·u, which never touches
      cond and dominates at the extreme magnitudes this library targets).
      New contract: cond·(3k+4+D)·u + |log|S||·u, worst observed/bound 0.85
      across the search. Header, CHANGELOG, BENCHMARKS and test_accuracy
      updated together.
      *Still open:* the two originally flagged questions were not the ones
      that broke it. Pos/neg rescale-error correlation and the n·u²
      threshold remain unexamined, and `pos_accum`'s (n+3k+3)·u has had no
      review at all — bound_search only attacks rp_accum. An independent
      human read of the corrected derivation is still worth having; what
      exists now is an author's derivation with an adversarial search
      behind it, which is strictly better than six fixed scenarios but is
      not independent review.

      *Why it ran first, kept as the record of the call.* The bound is what
      this library claims that every hand-rolled logsumexp lacks; it is the
      product. The header contract, README, CHANGELOG and BENCHMARKS.md all
      inherit from it, so a bound that moves moves them with it — and this
      one moved. Work finished before the review might have needed redoing;
      work finished after it will not.
- [x] **Bound review, second pass — pos_accum.** Done 2026-08-15, and it
      broke worse than rp_accum did: (n+3k+3)·u fails on 119 of 400 random
      inputs, worst **34.9×**. Every violation is the final-reduction term
      (|log|S||·u), which does not grow with n and has no cond to hide
      behind — four terms near e⁶⁹⁰ budget 7u against ~500u of real error.
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
      documented as **"exact in log domain"**, and they are not — the
      mapping is exact, the floating-point add implementing it rounds.
      Measured 1024u on a product of log-magnitude 1024.
      The root cause of all three refutations is now stated once, at
      `log_value`, where it belongs: **the representation has a precision
      floor of |log|x||·u that grows with magnitude** — ~13 significant
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
- [x] **Independent read.** Done 2026-08-15. The independent read agrees
      that the repaired `D` term and the representation/final-reduction term
      close real omissions; no counterexample to either corrected accumulator
      contract was found after reading the header, implementation, adversarial
      harness, fixed tests, reset test, public docs, packaging, and broader
      repository search.

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
      "±1%" claim accordingly. This also sets the bar for the
      second-machine run below: a cross-machine delta under ~8% is not
      evidence of anything.

## Tooling — ships as beta, gaps stated

- [x] **Wire triage → pass** (intent step 9). Done 2026-08-15, after the task
      turned out to be blocked on a matcher bug. The pass now computes the
      matcher's risk verdict for its matched loop, declines below `min-risk`
      (default HIGH), and logs the verdict on every rewrite. Both branches
      are tested, including that a misspelled parameter is refused rather
      than silently reverting to the default.
      *The blocker, and the real finding:* the matcher could not verdict the
      pass's shape at all. Its `nMul >= 1` rule excluded plain sums of `exp`
      — the softmax denominator, this project's marquee shape — so scanning
      `pass/test_softmax.c` produced zero hits, and darknet's softmax had
      matched only because it divides by a temperature. Fixed and the study
      re-run: 783 hits against 781, HIGH 5 against 3 (matcher/RESULTS.md,
      "The rule that excluded the marquee shape").
      *Honest limit:* because the pass's only shape requires an `exp` call,
      its verdict is always HIGH and the gate cannot currently decline a real
      input. It becomes load-bearing when shape coverage widens.
- [ ] **Diagnostic ergonomics.** One-command entry point (point it at a
      compile_commands.json or a build dir) instead of the manual
      cc-bc.sh/run_study.sh two-step; package the three tools' WSL/LLVM-21
      requirement clearly.
- [ ] **Pass shape coverage.** fmuladd spines and fsub accumulators match in
      the *matcher* but the *pass* only rewrites plain `fadd(phi, exp(t))`.
      Extend or document per shape.
- [ ] **Matcher blind spots.** Memory-carried reductions and vectorized
      loops are documented misses; decide whether v1 chases either or the
      docs stay the answer. Now has evidence attached (`matcher/coverage.c`,
      RESULTS.md "Coverage against the shapes this project names"): the
      forward algorithm, a README-named target, is invisible in its usual
      `out[j] += ...` form because the accumulator is an array cell.
      Memory-carried reductions are no longer a hypothetical gap — one of
      the three motivating shapes lands in it.
- [ ] **Per-loop risk cannot see cross-loop decay.** Found 2026-08-15 by the
      coverage audit. When the matcher *can* see the forward algorithm (the
      register-accumulator form), the triage grades it LOW: `nMul = 1`, no
      transcendental. The underflow is real but lives in the outer loop —
      probabilities decay across time steps while each inner reduction looks
      unremarkable. So the diagnostic would not flag one of the shapes the
      README names as motivating the library. Either the risk rule gains a
      cross-loop signal (magnitude trend across an enclosing loop), or the
      docs stop implying the diagnostic covers this family. Decide before
      the diagnostic is called shippable.

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
- **Downstream log-form propagation** — the stretch goal, and the one item
  here with a research question attached. The rescue currently exits through
  the `__logrange_logsum` side global; the win is rewriting the *consumers*
  (softmax's divide → subtract) to use the log form. Full statement, first
  milestone, and stopping rule: `logrange_intent.md`, "Stretch Goal —
  End-to-End Log-Form Propagation". That section needs fleshing out to the
  standard of the other deliverables before any code is written. The
  diagnostic is the shipping front door either way, so nothing in 1.0 waits
  on this.
