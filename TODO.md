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
- [ ] **Second-machine benchmark run.** All numbers come from one Ryzen
      5800X. Criterion 2's "within noise" claim deserves one confirmation on
      different hardware (any second machine; the harness measures its own
      noise floor, so this is cheap to do credibly). The CI runners became
      that second machine when the Linux jobs landed. Open question is
      whether a shared cloud vCPU's noise floor is low enough for the
      harness to say anything — the harness reports its own spread, so it
      can answer that itself rather than being assumed either way.
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
      *Found while doing it:* the FP flag is contractual, not cosmetic.
      `rp_accum`'s compensation is written statement-per-step so its
      recovered rounding error cannot be fused away, and FMA contraction
      undoes that — measured 14× worse on heavy cancellation (2.7e-09 →
      3.7e-08), silently, and on by default wherever FMA is baseline. The
      exported target now carries `-ffp-contract=off` / `/fp:precise`.

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
- [ ] **Independent read.** The one part of the review that cannot be done
      from inside. Three stated bounds have now been refuted by search and
      corrected by the same author who wrote them; a second pair of eyes on
      the corrected derivations is the remaining honesty debt, and it is the
      only one left that a search harness cannot discharge.
- [ ] **BENCHMARKS.md refresh at 1.0 flags.** If anything above changes
      flags or code paths, the published numbers must be re-run, not edited.

## Tooling — ships as beta, gaps stated

- [ ] **Wire triage → pass** (intent step 9): the pass should consume the
      matcher's HIGH-risk verdict instead of rewriting every matched shape.
- [ ] **Diagnostic ergonomics.** One-command entry point (point it at a
      compile_commands.json or a build dir) instead of the manual
      cc-bc.sh/run_study.sh two-step; package the three tools' WSL/LLVM-21
      requirement clearly.
- [ ] **Pass shape coverage.** fmuladd spines and fsub accumulators match in
      the *matcher* but the *pass* only rewrites plain `fadd(phi, exp(t))`.
      Extend or document per shape.
- [ ] **Matcher blind spots.** Memory-carried reductions and vectorized
      loops are documented misses; decide whether v1 chases either or the
      docs stay the answer.

## Explicitly not blocking 1.0

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
