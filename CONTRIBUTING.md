# Conventions

Applies to anyone working in this repo, human or agent. Two sections: how
claims are made, and how they are written.

## Evidence

The product is a set of checkable claims. A claim that cannot be checked is
worse than no claim, because it reads as established.

**A test that cannot fail is not a test.** Break the thing it guards, watch it
fail, restore it. Every gate in this repo has been negative-tested that way,
and three were found to be checking nothing: `coverage.c` had no runner, the
`propagate=div` test asserted a log line and no number, and the first version
of the rewrite pass passed every test while never wiring in its replacement.

**Write the failing test first.** If a new assertion passes before the fix, it
is not testing the fix.

**A failed build leaves the old binary on disk.** Check build exit status, not
just test output. A `-Werror` failure once produced a green run against stale
code.

**Fixed scenarios can only fail to refute a universal claim.** Three stated
error bounds survived six fixed scenarios each and were refuted within minutes
by `tests/bound_search.cpp`, which searches for counterexamples. Bounds get
searched, not sampled.

**Measure the mechanism before naming it.** Several claims here were right
about the outcome and wrong about the cause: FMA contraction was blamed for a
14x accuracy change that came from the test corpus, and the mid-loop-read
guard was blamed on memory-carried accumulators when the accumulator was in a
register. Read the IR, run the differential, then write the explanation.

**Published numbers must be reproducible from committed tooling.** The
rejection census was first produced by a throwaway build on one machine, which
made it unreproducible. It is now `./run_study.sh rejects`.

**Collect records serially.** Running `opt` under `xargs -P4` interleaved
records onto shared lines and undercounted. The census asserts that every
emitted line carries a record tag.

**Retract, do not reword.** When a claim turns out wrong, say so and leave the
correction visible. `CHANGELOG.md` carries old and new values for every
contract that moved.

## Writing

Applies to docs, commit messages, and code comments.

Terse and technical. Cut sentences that carry no fact. Lead with numbers. One
claim per block, bold label on its own line. No editorializing ("worth
noting", "the real win", "which is the point"). No hedging ("plausibly",
"arguably", "essentially"). No cross-reference filler ("as mentioned above").
No em-dash asides. No emoji. Plain words over tool jargon. State contradictions
in a labeled block. Describe process only when it changed a conclusion.

Section headings and definition lists may use a dash as a separator. That is
not an aside.

## Where the contracts live

`include/logrange/log_math.h` states the runtime's error contracts and is the
version source of truth. `pass/ELIGIBILITY.md` is normative for the rewrite
pass; `pass/PROTOTYPE.md` is its design narrative and measured record, and
loses to ELIGIBILITY.md on conflict. `matcher/METHODOLOGY.md` fixes the study
rules, and `matcher/RESULTS.md` reports against them.

## Build

Library: `cmake -S . -B build`, `cmake --build build --config Release`,
`ctest --test-dir build -C Release`. Five suites.

Matcher and pass: WSL, LLVM 21, install in `SETUP.md`. `matcher/run_study.sh
selftest | coverage | rejects`, `bash matcher/test_scan.sh`, and `bash
pass/run_pass_test.sh`. All four are gated in CI by
`.github/workflows/llvm-tooling.yml`. WSL `/tmp` does not persist between
separate `wsl.exe` invocations; keep state under `~/logrange-pass` or
`~/logrange-study` and do multi-step work in one invocation.
