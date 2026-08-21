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
Confirm the mutation moved the measured quantity: editing `LOOP,` to `LOOP,X`
still matches `^LOOP,`, and the check stayed green.

**Write the failing test first.** If a new assertion passes before the fix, it
is not testing the fix.

**Assert the property, not a list you maintain.** Two hand-maintained lists
that agree are not a check. `run_pass_test.sh` named 4 rewritten functions and
4 untouched controls with nothing tying the sets, and a relaxed weight clause
then rewrote `s += 0.5*exp(x)` with the `0.5` dropped, every assertion green.
The rewritten set is now derived from the tool's output.

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

**A mechanism named in prose needs a gate.** `PROTOTYPE.md` and the pass source
said the dead original chain is left for later DCE. `dce` removes nothing: the
orphan is a loop-carried cycle and every instruction in it has a use. `adce`
took 34 `exp` calls to 28, and was gated.

**A gate on the workaround is not a gate on the thing.** That `adce` step then
became the supported pipeline and was asserted there, so the posture condition
it stood in for was satisfied by an artifact describing itself. Closed
2026-08-21: the pass deletes the chain itself, and the gate requires `dce` and
`adce` over its output to return the IR unchanged.

**A figure is derived or it is wrong.** `0 w*exp(t) sites in 2859 loops`
reached four files under the weaker rule that numbers be reproducible from
committed tooling. A number hand-copied into prose is an independent assertion
from that moment. Every published corpus figure now has one executable
derivation over committed evidence, gated on every push:
`matcher/run_study.sh figures`, diffed against `matcher/data/FIGURES.txt`.

**A figure must name its population.** `0 sites` is not a measurement without
its denominator and filter. That census is gated on `expChain` and runs after
the hit, so its population was 5, and it cannot see a loop rejected upstream.

**A correction is a claim.** Verify it to the standard the original should have
met, and quote the text being corrected. Two corrections here were wrong: a
handoff was called backwards on `-ffp-contract` when it was correct, and a
replacement figure said 3 of 5 rows carried `nMul=0` when 2 do.

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

## Where a sentence goes

**Intent states what must be true, not what happened.** `logrange_intent.md`
is present tense: the aim, the deliverables, the criteria, the posture, and
the reasoning behind them. It carries no dates, no commit SHAs, and no
measured numbers.

The rest routes elsewhere. `CHANGELOG.md` carries what changed, with old and
new values. `TODO.md` carries what is open, and what closed it. The
measurement docs — `matcher/RESULTS.md`, `matcher/DELTA.md`, `BENCHMARKS.md` —
carry figures, derived from committed evidence.

**The test: a date, a commit SHA, or a measured figure means the sentence
belongs somewhere else.** Gated in `llvm-tooling.yml`.

Not every number is a figure. The intent doc legitimately carries domain
constants — `|log| ~ 700`, terms near −800, ~745 log-units below the reference
— and spec quantities like a 1000-term mixture. Those state what must be true.
A *measured result* is what does not belong, and in practice it reads as a
number next to study vocabulary: hits, loops, sites, rows, percentages.

The gate is a floor, not a ceiling. It cannot see a bare count or a duration
("gained 31", "for two months"), and both were caught by reading. Apply the
rule; the grep only stops the obvious regressions.

This rule exists because the intent doc grew a 94-line work log indexing
records that live in six other files, and one of its hand-copied figures was
two revisions stale while CI gated the same number elsewhere. A numbered
status list invites appending; nothing in the file said not to.

## Build

Library: `cmake -S . -B build`, `cmake --build build --config Release`,
`ctest --test-dir build -C Release`. Five suites.

Matcher and pass: WSL, LLVM 21, install in `SETUP.md`. `matcher/run_study.sh
selftest | coverage | figures | rejects | weights`, `bash matcher/test_scan.sh`,
and `bash pass/run_pass_test.sh`. Five are gated in CI by
`.github/workflows/llvm-tooling.yml`; `figures` needs no corpus and no plugin,
which is why it runs there. `rejects` and `weights` need harvested bitcode and
do not. WSL `/tmp` does not persist between separate `wsl.exe` invocations;
keep state under `~/logrange-pass` or `~/logrange-study` and do multi-step work
in one invocation.
