# Road to 1.0

What "1.0" means here: the **header is the product** (intent Deliverable 1) —
it must be trustworthy on a machine we've never seen. The tooling (matcher,
diagnostic, pass) ships alongside at whatever maturity it honestly has, each
labeled. Items ordered roughly by how much they'd embarrass us if skipped.

## Blocking 1.0 — library

- [ ] **License file.** The repo has none, so nobody can legally use the
      header. Owner's call: MIT or BSD-3 fit the stb-library economics the
      intent cites; Apache-2.0 if patent language matters. One file, pick one.
- [ ] **Second toolchain, in CI.** Everything is verified under MSVC only;
      the gcc/clang flag branch in CMakeLists has never actually run for the
      library tests (the WSL clang work only built the matcher/pass). Add a
      ubuntu-latest CI job, fix what `-Wall -Wextra -Werror` surfaces, and
      re-run test_accuracy there — the formal bound assumes exp() within
      1 ulp, and that assumption should be *checked* on glibc, not assumed.
- [ ] **Version identity in the header.** `LOGRANGE_VERSION` macros and a
      CHANGELOG.md, so a vendored copy can be identified in the wild.
- [ ] **Float support decision.** Everything is double-only. Either add
      `log_value_f`/templates or state "double only, by design" in the
      header contract. Deciding is the deliverable; implementing is optional.
- [ ] **Second-machine benchmark run.** All numbers come from one Ryzen
      5800X. Criterion 2's "within noise" claim deserves one confirmation on
      different hardware (any second machine; the harness measures its own
      noise floor, so this is cheap to do credibly).
- [ ] **Install/packaging story.** `cmake --install` rules + config so
      `find_package(LogRange)` works; verify the README quickstart compiles
      as written from a clean clone.

## Blocking 1.0 — honesty debts

- [ ] **Bound review pass.** The (3k+4)·u derivation is a sketch reviewed by
      its author. Re-derive it cold (or have someone else read it) before
      the contract is called 1.0. Special attention: rescale-error
      correlation between pos and neg (currently bounded independently) and
      the n·u² small-print threshold.
- [ ] **BENCHMARKS.md refresh at 1.0 flags.** If anything above changes
      flags or code paths, the published numbers must be re-run, not edited.

## Tooling — ships as beta, gaps stated

- [ ] **Wire triage → pass** (intent step 9): the pass should consume the
      matcher's HIGH-risk verdict instead of rewriting every matched shape.
- [ ] **Downstream log propagation.** The rescue currently exits through the
      `__logrange_logsum` side global; the real win is rewriting the
      *consumers* (e.g. softmax's divide) to use the log form. This is the
      hard remaining compiler work and it is allowed to miss 1.0 — the
      diagnostic is the shipping front door.
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
