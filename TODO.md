# Road to 1.0

What "1.0" means here: the **header is the product** (intent Deliverable 1) —
it must be trustworthy on a machine we've never seen. The tooling (matcher,
diagnostic, pass) ships alongside at whatever maturity it honestly has, each
labeled. Items ordered roughly by how much they'd embarrass us if skipped.

Sections are not strictly in execution order. **Next up is the bound review**
under "honesty debts", ahead of the remaining library items: it can move the
error contract, and the rest of 1.0 is written on top of that contract.

## Blocking 1.0 — library

- [x] **License file.** MIT, added 2026-08-15.
- [x] **Second toolchain, in CI.** Done 2026-08-15. CI matrix is now
      windows-msvc + ubuntu-gcc + ubuntu-clang. The gcc/clang flag branch
      (`-Wall -Wextra -Werror -ffp-contract=off`) needed no code fixes:
      clean under gcc 15.2 and clang 21.1. test_accuracy holds on glibc with
      4.5×–5100× slack, so the bound's 1-ulp `exp()` assumption survives a
      second libm (BENCHMARKS.md, "Second toolchain"). Verified green on the
      runners (gcc 13.3, clang 18.1) as well as locally (gcc 15.2, clang
      21.1); the accuracy table is bit-identical across all four.
- [x] **Version identity in the header.** Done 2026-08-15.
      `LOGRANGE_VERSION_MAJOR/_MINOR/_PATCH`, ordered `LOGRANGE_VERSION`, and
      `LOGRANGE_VERSION_STRING` in log_math.h, plus CHANGELOG.md. The header
      is the source of truth: CMake parses it (the hardcoded `project(VERSION
      0.1)` had already drifted a release behind), and test_log_math checks
      the string against the numeric parts. Both guards negative-tested.
- [ ] **Float support decision.** Everything is double-only. Either add
      `log_value_f`/templates or state "double only, by design" in the
      header contract. Deciding is the deliverable; implementing is optional.
      *Rests on the bound review, and is close to settled by it.* The
      contract is written in double-specific constants: u = 2⁻⁵³, the ~745
      log-unit vanishing window (the subnormal floor at 2⁻¹⁰⁷⁴), and
      test_accuracy's double-double reference. Float is not a typedef — it
      means re-deriving at u = 2⁻²⁴, re-basing that reference, and a rescue
      window of ~103 log units (2⁻¹⁴⁹). The pass already declines float
      accumulators, so double-only is also the coherent answer for the
      toolchain. Decide in the same pass that restates the bound.
- [ ] **Second-machine benchmark run.** All numbers come from one Ryzen
      5800X. Criterion 2's "within noise" claim deserves one confirmation on
      different hardware (any second machine; the harness measures its own
      noise floor, so this is cheap to do credibly). The CI runners became
      that second machine when the Linux jobs landed. Open question is
      whether a shared cloud vCPU's noise floor is low enough for the
      harness to say anything — the harness reports its own spread, so it
      can answer that itself rather than being assumed either way.
- [ ] **Install/packaging story.** `cmake --install` rules + config so
      `find_package(LogRange)` works; verify the README quickstart compiles
      as written from a clean clone.

## Blocking 1.0 — honesty debts

- [ ] **Bound review pass. Do this before the remaining library items.** The
      (3k+4)·u derivation is a sketch reviewed by its author. Re-derive it
      cold before the contract is called 1.0. Special attention:
      rescale-error correlation between pos and neg (currently bounded
      independently) and the n·u² small-print threshold.

      *Why it goes first.* The bound is the thing this library claims that
      every hand-rolled logsumexp lacks; it is the product. The header
      contract, README, CHANGELOG, and BENCHMARKS.md all inherit from it, so
      a bound that moves moves them with it. Work finished before the review
      may have to be redone; work finished after it will not.

      *Why re-derivation alone is the weak form.* It is the author
      reproducing the author's own reasoning, blind in the same places —
      including, specifically, the two items flagged above. Pair it with
      adversarial search. `test_accuracy` asserts six fixed scenarios, while
      the bound is a universal claim over all inputs, orderings, and k.
      Sweep condition number, rescale count, term ordering, and pos/neg
      correlation structure to maximize observed/bound, and report the worst
      ratio found. Failing to break the bound across a wide search is
      evidence independent of whether the algebra is right; breaking it is
      the most valuable single result available here.
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
