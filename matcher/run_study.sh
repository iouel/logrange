#!/usr/bin/env bash
# run_study.sh — the matcher hit-rate study, end to end (WSL/Linux).
#
#   ./run_study.sh selftest   build plugin, run the labeled ground truth,
#                             assert expected counts (the validation gate)
#   ./run_study.sh figures    derive every published corpus figure from the
#                             COMMITTED matcher/data/raw-*.txt and diff against
#                             matcher/data/FIGURES.txt. Needs no corpus and no
#                             plugin, so it runs in CI. This is the only place
#                             a published count is computed.
#   ./run_study.sh <name>     harvest bitcode for one target codebase
#                             (expects it cloned at $WORK/<name>) and count
#   ./run_study.sh report     aggregate all counts into the summary table
#
# The selftest MUST pass before any codebase numbers are collected
# (METHODOLOGY.md: the gate comes first, numbers second).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HOME/logrange-study}"
PLUGIN="$HERE/build/SopMatcher.so"
OPT="${OPT:-opt}"
CLANG="${CLANG:-clang}"
export CLANG

build_plugin() {
  cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$HERE/build" >/dev/null
}

# Loop canonicalization the matcher's LLVM recognizer requires, and which a
# real mid-pipeline pass would already have. Both are unstated preconditions of
# RecurrenceDescriptor::AddReductionVar:
#   loop-simplify — it reads the reduction start value via
#     Phi->getIncomingValueForBlock(L->getLoopPreheader()) with no null check.
#     With assertions off that is an out-of-bounds read, not a diagnostic.
#   lcssa         — it inspects out-of-loop users to find the exit instruction.
#     Without LCSSA it declined 28 reductions on this corpus that it accepts
#     with it (matcher/DELTA.md).
# Measured as a no-op for the legacy recognizer: 2859 loops / 783 hits / 5 HIGH
# with and without, on all three codebases.
CANON_PASSES="loop-simplify,lcssa"

# Run the matcher over every .bc in a directory; write raw lines to $2.
scan() {
  local bcdir="$1" out="$2" passes="${3:-sop-matcher}"
  : > "$out"
  local n=0
  for bc in "$bcdir"/*.bc; do
    [ -e "$bc" ] || continue
    "$OPT" -load-pass-plugin="$PLUGIN" -passes="$CANON_PASSES,$passes" \
      -disable-output "$bc" 2>> "$out" || true
    n=$((n + 1))
  done
  echo "scanned $n modules from $bcdir"
}

summarize() {
  local raw="$1" label="$2"
  local loops hits trans const_trip high med low
  loops=$(grep -c '^LOOP,' "$raw" || true)
  hits=$(grep -c '^HIT,' "$raw" || true)
  trans=$(grep -c '^HIT,.*,transcendental,' "$raw" || true)
  const_trip=$(grep -c '^HIT,.*,constant,' "$raw" || true)
  # Risk is the second-to-last column; reasons (last) never contain commas.
  high=$(grep -c '^HIT,.*,HIGH,[^,]*$' "$raw" || true)
  med=$(grep -c '^HIT,.*,MED,[^,]*$' "$raw" || true)
  low=$(grep -c '^HIT,.*,LOW,[^,]*$' "$raw" || true)
  printf '%-12s  fp-loops=%-5s hits=%-4s transcendental=%-3s const-trip=%-4s high=%-3s med=%-4s low=%s\n' \
    "$label" "$loops" "$hits" "$trans" "$const_trip" "$high" "$med" "$low"
}

case "${1:-}" in
figures)
  # Every corpus figure this project publishes, DERIVED from committed
  # evidence rather than asserted in prose.
  #
  # Why this exists. The counts were hand-copied into five markdown files (32
  # occurrences) and into pass source comments, and one of them was wrong in a
  # way that survived review and shipped: "0 w*exp(t) sites in 2859 loops".
  # The weight census is gated on `Weights && CI.expChain` and runs AFTER the
  # HIT is emitted, so its population is the exp-chain hits — 5 — not 2859.
  # The number was not merely stale, it named the wrong denominator, and no
  # amount of care in prose prevents that recurring.
  #
  # Reads ONLY matcher/data/raw-*.txt, which are committed. No corpus
  # checkout, no bitcode harvest, no plugin build: this runs anywhere,
  # including CI, which is what makes the figures reproducible rather than
  # attested. Regenerates the canonical block and diffs it against the
  # committed copy, so a change in evidence is a reviewable diff.
  data="$HERE/data"
  for f in darknet gsl libsvm; do
    [ -f "$data/raw-$f.txt" ] \
      || { echo "FAIL: missing committed evidence $data/raw-$f.txt" >&2; exit 1; }
  done
  mkdir -p "$WORK"
  gen="$WORK/FIGURES.txt"
  all="$WORK/figures-all.txt"
  cat "$data"/raw-darknet.txt "$data"/raw-gsl.txt "$data"/raw-libsvm.txt > "$all"

  loops=$(grep -c '^LOOP,' "$all" || true)
  hits=$(grep -c '^HIT,' "$all" || true)
  trans=$(grep -c '^HIT,.*,transcendental,' "$all" || true)
  # The weight census's actual population: SumOfProductsMatcher.cpp gates it on
  # CI.expChain, after the HIT. Named here so the denominator cannot be
  # misread as `hits` or `loops` again.
  expchain=$(grep -c '^HIT,.*exp-chain' "$all" || true)
  high=$(grep -c '^HIT,.*,HIGH,[^,]*$' "$all" || true)
  highlines=$(grep '^HIT,.*,HIGH,[^,]*$' "$all" | cut -d, -f2,3 | sort -u | wc -l)
  # nMul is field 7. An exp-chain hit with nMul=0 has no multiply anywhere in
  # its term chain, so w*exp(t) is impossible for it — provable from this text
  # alone, without the IR or the census.
  nomul=$(awk -F, '$1=="HIT" && $10 ~ /exp-chain/ && $7==0' "$all" | wc -l)
  rate=$(awk -v h="$hits" -v l="$loops" 'BEGIN{printf "%.1f", 100*h/l}')

  {
    echo "# GENERATED by matcher/run_study.sh figures — do not hand-edit."
    echo "# Derived from committed matcher/data/raw-{darknet,gsl,libsvm}.txt."
    echo "# No corpus or bitcode harvest required; runs anywhere, including CI."
    echo
    echo "per codebase"
    for f in darknet gsl libsvm; do summarize "$data/raw-$f.txt" "$f"; done
    echo
    echo "aggregate"
    printf '  loops scanned                 %s\n' "$loops"
    printf '  shape hits                    %s   (%s%%)\n' "$hits" "$rate"
    printf '    transcendental              %s\n' "$trans"
    printf '    exp-chain                   %s\n' "$expchain"
    printf '  HIGH findings                 %s   on %s distinct source lines\n' \
      "$high" "$highlines"
    echo
    echo "exp-chain population — everything the weight census can inspect"
    awk -F, '$1=="HIT" && $10 ~ /exp-chain/ {printf "  %-46s nMul=%s\n", $2":"$3" "$4, $7}' "$all"
    printf '  of these, nMul=0 (no multiply at all)       %s\n' "$nomul"
  } > "$gen"

  cat "$gen"
  if ! diff -u "$data/FIGURES.txt" "$gen"; then
    echo
    echo "FAIL: derived figures differ from matcher/data/FIGURES.txt."
    echo "      The evidence moved. Review the diff above, then commit the"
    echo "      regenerated file together with any prose it invalidates."
    exit 1
  fi
  echo
  echo "FIGURES OK (derived from committed evidence; $loops loops, $hits hits,"\
       "$expchain exp-chain, $high HIGH)"
  ;;
selftest)
  build_plugin
  mkdir -p "$WORK/bc-selftest"
  "$CLANG" -O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
    -emit-llvm -c "$HERE/selftest.c" -o "$WORK/bc-selftest/selftest.bc"
  scan "$WORK/bc-selftest" "$WORK/raw-selftest.txt"
  cat "$WORK/raw-selftest.txt"
  hits=$(grep -c '^HIT,' "$WORK/raw-selftest.txt" || true)
  loops=$(grep -c '^LOOP,' "$WORK/raw-selftest.txt" || true)
  trans=$(grep -c ',transcendental,' "$WORK/raw-selftest.txt" || true)
  # Labels in selftest.c: 5 hits out of 7 examined FP loops, 2 transcendental;
  # the mixture_likelihood hit must triage HIGH with exp-chain in its reasons,
  # and softmax_denom (plain sum of exp, no multiply) must be HIGH with
  # exp-sum — the shape the nMul >= 1 rule used to drop.
  ml_ok=0
  grep '^HIT,.*,mixture_likelihood,' "$WORK/raw-selftest.txt" \
    | grep ',HIGH,' | grep -q 'exp-chain' && ml_ok=1
  sd_ok=0
  grep '^HIT,.*,softmax_denom,' "$WORK/raw-selftest.txt" \
    | grep ',HIGH,' | grep -q 'exp-sum' && sd_ok=1
  if [ "$hits" = 5 ] && [ "$loops" = 7 ] && [ "$trans" = 2 ] && [ "$ml_ok" = 1 ] \
     && [ "$sd_ok" = 1 ]; then
    echo "SELFTEST PASS (5 hits / 7 fp-loops / 2 transcendental / mixture_likelihood HIGH exp-chain / softmax_denom HIGH exp-sum)"
  else
    echo "SELFTEST FAIL: hits=$hits (want 5) loops=$loops (want 7) trans=$trans (want 2) mixture-high=$ml_ok (want 1) softmax-exp-sum=$sd_ok (want 1)"
    exit 1
  fi
  ;;
rejects)
  # Rejection-cause census, backing the table in RESULTS.md under "The rule
  # that excluded the marquee shape" / the mid-loop-read guard discussion.
  # Serial by construction (scan() runs one opt at a time): an earlier
  # ad-hoc version used xargs -P4, whose concurrent writes to one stderr
  # interleaved records and undercounted (451/20/6/133/291 against the
  # serial 460/23/8/135/294).
  build_plugin
  shift || true
  targets="${*:-darknet gsl libsvm}"
  all="$WORK/raw-rejects-all.txt"
  : > "$all"
  for name in $targets; do
    bcdir="$WORK/bc-$name"
    [ -d "$bcdir" ] || { echo "no harvested bitcode at $bcdir; skipping" >&2; continue; }
    scan "$bcdir" "$WORK/raw-rejects-$name.txt" 'sop-matcher<explain>'
    cat "$WORK/raw-rejects-$name.txt" >> "$all"
  done
  # Any line not starting with a known record tag means interleaved output.
  bad=$(grep -vcE '^(LOOP|HIT|REJECT|WEIGHT|DIFF|WARN),' "$all" || true)
  cause() { grep -c "^REJECT,$1," "$all" || true; }
  echo
  echo "== rejection census: $targets =="
  printf 'malformed lines (interleaving check)   %s\n' "$bad"
  printf 'REJECT records                         %s\n' "$(grep -c '^REJECT,' "$all" || true)"
  printf '  not-reduction  (LLVM: not a reduction) %s\n' "$(cause not-reduction)"
  printf '  kind           (reduction, wrong kind) %s\n' "$(cause kind)"
  printf '  dirtyChain     (op outside allowed set)%s\n' "$(cause dirtyChain)"
  printf '  noMulNoExp     (plain sum, out of scope)%s\n' "$(cause noMulNoExp)"
  # These three should stay at zero. Non-zero means the scan pipeline lost its
  # canonicalization (see CANON_PASSES) or getReductionOpChain declined AND the
  # generic cycle walk also failed — either is a pipeline bug, not a finding.
  printf '  not-simplified (MUST be 0)             %s\n' "$(cause not-simplified)"
  printf '  chain          (MUST be 0)             %s\n' "$(cause chain)"
  printf '  no-terms       (MUST be 0)             %s\n' "$(cause no-terms)"
  [ "$bad" = "0" ] || { echo "FAIL: interleaved output detected"; exit 1; }
  for z in not-simplified chain no-terms; do
    [ "$(cause $z)" = "0" ] || { echo "FAIL: $z should be unreachable"; exit 1; }
  done
  ;;
delta)
  # Every figure in DELTA.md, DERIVED from committed evidence.
  #
  # The measurement itself is not re-runnable: it needed both recognizers in
  # one binary, and the hand-written one was deleted once the comparison was
  # published. What is committed instead is its paired output,
  # data/raw-delta.txt — 2859 LOOP + 783 HIT (the old recognizer's stream) +
  # 96 DIFF records — which is enough to recompute both columns. Same contract
  # as `figures`: reads only committed text, needs no corpus, no bitcode and
  # no plugin, so a reader can check DELTA.md without reproducing the study.
  d="$HERE/data/raw-delta.txt"
  [ -f "$d" ] || { echo "FAIL: missing committed evidence $d" >&2; exit 1; }
  n() { grep -c "$1" "$d" || true; }
  loops=$(n '^LOOP,'); leghits=$(n '^HIT,')
  lonly=$(n '^DIFF,llvm-only,'); gonly=$(n '^DIFF,legacy-only,')
  differ=$(n '^DIFF,both-differ,'); recov=$(n '^DIFF,agree-recovered,')
  llvmhits=$((leghits - gonly + lonly))
  echo "== recognizer delta (derived from data/raw-delta.txt) =="
  printf 'loops examined (both recognizers)      %s\n' "$loops"
  printf 'hits, hand-written spine walk          %s\n' "$leghits"
  printf 'hits, RecurrenceDescriptor             %s\n' "$llvmhits"
  printf '  gained                               %s\n' "$lonly"
  printf '    was rejected at the spine walk     %s\n' "$(n '^DIFF,llvm-only,.*legacy=miss:spine')"
  printf '    was rejected at the mid-loop guard %s\n' "$(n '^DIFF,llvm-only,.*legacy=miss:cleanUses')"
  printf '  lost                                 %s\n' "$gonly"
  printf 'graded differently where both matched  %s\n' "$differ"
  printf 'chain recovered by the cycle fallback  %s\n' "$recov"
  bad=$(grep -vcE '^(LOOP|HIT|REJECT|WEIGHT|DIFF|WARN),' "$d" || true)
  [ "$bad" = "0" ] || { echo "FAIL: $bad malformed lines in $d"; exit 1; }
  # DELTA.md's central claim: recognition changed, grading did not. If this
  # ever fires, the two recognizers were not in fact sharing evaluate() and
  # every gain/loss attribution in that file is unsound.
  [ "$differ" = "0" ] || { echo "FAIL: a shared match was graded differently"; exit 1; }
  # The published headline. Asserted, not just printed, so DELTA.md and this
  # evidence cannot drift apart silently.
  [ "$loops/$leghits/$llvmhits" = "2859/783/814" ] || {
    echo "FAIL: derived $loops/$leghits/$llvmhits, DELTA.md publishes 2859/783/814"
    exit 1; }
  echo "DELTA OK (2859 loops, 783 -> 814 hits, +$lonly/-$gonly, 0 regrades)"
  ;;
xloop)
  # Cross-loop feedback census, backing matcher/XLOOP.md.
  #
  # Counts hits whose result is stored into an object their own terms load
  # from, across the enclosing loop — the structural precondition for the
  # forward algorithm's decay. Measured, NOT graded: the promotion this was
  # built to justify was declined on exactly these numbers, so the census
  # exists and the risk rule does not read it.
  #
  # Carries a positive control for the same reason the weight census does: a
  # broken probe returning zero must not read as a corpus zero. coverage.c's
  # forward_full_flat and forward_full_swap are the two detectable forms, and
  # the swap form is there because an identity-only rule misses it.
  build_plugin
  shift || true
  targets="${*:-darknet gsl libsvm}"
  all="$WORK/raw-xloop-all.txt"
  : > "$all"
  for name in $targets; do
    bcdir="$WORK/bc-$name"
    [ -d "$bcdir" ] || { echo "no harvested bitcode at $bcdir; skipping" >&2; continue; }
    scan "$bcdir" "$WORK/raw-xloop-$name.txt" 'sop-matcher<xloop>'
    cat "$WORK/raw-xloop-$name.txt" >> "$all"
  done
  bad=$(grep -vcE '^(LOOP|HIT|REJECT|WEIGHT|DIFF|WARN|XLOOP),' "$all" || true)
  echo
  echo "== cross-loop feedback census: $targets =="
  printf 'malformed lines (interleaving check)   %s
' "$bad"
  printf 'hits                                   %s
' "$(grep -c '^HIT,' "$all" || true)"
  printf 'of those, cross-loop feedback          %s
' "$(grep -c '^XLOOP,' "$all" || true)"
  printf '  graded HIGH                          %s
' "$(grep -c '^XLOOP,.*,HIGH$' "$all" || true)"
  printf '  graded MED                           %s
' "$(grep -c '^XLOOP,.*,MED$' "$all" || true)"
  printf '  graded LOW                           %s
' "$(grep -c '^XLOOP,.*,LOW$' "$all" || true)"
  printf 'of those, transcendental               %s
'     "$(grep '^XLOOP,' "$all" | cut -d, -f2,3 | sort -u | while IFS= read -r k; do grep -h "^HIT,$k," "$all"; done | grep -c ',transcendental,' || true)"
  [ "$bad" = "0" ] || { echo "FAIL: interleaved output detected"; exit 1; }

  # Positive control.
  mkdir -p "$WORK/bc-coverage"
  "$CLANG" -O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops     -emit-llvm -c "$HERE/coverage.c" -o "$WORK/bc-coverage/coverage.bc"
  scan "$WORK/bc-coverage" "$WORK/raw-xloop-coverage.txt" 'sop-matcher<xloop>'
  for fn in forward_full_flat forward_full_swap; do
    grep -q "^XLOOP,.*,$fn," "$WORK/raw-xloop-coverage.txt"       || { echo "FAIL: positive control $fn produced no XLOOP record";            echo "The corpus count above is not evidence; the probe is broken.";            exit 1; }
  done
  echo "XLOOP OK (both control forms fire; corpus count above is a measurement)"
  ;;
weights)
  # Weight census, backing the scope decision for the pass's shape coverage.
  # The mixture spine w * exp(t) can only be rewritten when the weight's
  # magnitude is provably safe: the emitted state accumulates
  # sum(w_i * exp(t_i - m)), which reaches sum|w_i| and can overflow where the
  # linear original does not. Before implementing that, count how often the
  # spine occurs at all.
  build_plugin
  shift || true
  targets="${*:-darknet gsl libsvm}"
  all="$WORK/raw-weights-all.txt"
  : > "$all"
  for name in $targets; do
    bcdir="$WORK/bc-$name"
    [ -d "$bcdir" ] || { echo "no harvested bitcode at $bcdir; skipping" >&2; continue; }
    scan "$bcdir" "$WORK/raw-weights-$name.txt" 'sop-matcher<weights>'
    cat "$WORK/raw-weights-$name.txt" >> "$all"
  done
  echo
  echo "== weight census: $targets =="
  printf 'hits                                   %s
' "$(grep -c '^HIT,' "$all" || true)"
  printf '  of those, transcendental             %s
' "$(grep -c '^HIT,.*,transcendental,' "$all" || true)"
  printf 'w * exp(t) multiplies found            %s
' "$(grep -c '^WEIGHT,' "$all" || true)"
  echo "kinds:"
  # `|| true`: under `set -euo pipefail` a grep with no matches kills the
  # script, which is exactly the case this census expects to hit.
  { grep '^WEIGHT,' "$all" || true; } | cut -d, -f2 | sort | uniq -c     | sort -rn | sed 's/^/  /'
  # A census that silently reports 0 forever looks exactly like a census that
  # found nothing. coverage.c's `mixture` IS the w * exp(t) spine, so scanning
  # it proves the probe fires before the corpus zero above is believed.
  mkdir -p "$WORK/bc-coverage"
  "$CLANG" -O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops     -emit-llvm -c "$HERE/coverage.c" -o "$WORK/bc-coverage/coverage.bc"
  scan "$WORK/bc-coverage" "$WORK/raw-weights-coverage.txt" 'sop-matcher<weights>'
  ctrl=$(grep -c '^WEIGHT,' "$WORK/raw-weights-coverage.txt" || true)
  echo
  echo "positive control (coverage.c mixture): $ctrl WEIGHT record(s)"
  if [ "$ctrl" -lt 1 ]; then
    echo "FAIL: the census found nothing in a file that contains the shape."
    echo "The corpus zero above is not evidence; the probe is broken."
    exit 1
  fi
  echo "WEIGHTS OK (probe fires; corpus count above is a measurement)"
  ;;
coverage)
  # Standing check for the coverage claims in RESULTS.md ("Coverage against
  # the shapes this project names"). selftest.c guards the matcher's labeled
  # ground truth; this guards the separate claim that the shapes this project
  # SAYS it targets are or are not seen. Without it, that table is prose that
  # can rot silently — which is what it was until 2026-08-15.
  build_plugin
  mkdir -p "$WORK/bc-coverage"
  "$CLANG" -O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
    -emit-llvm -c "$HERE/coverage.c" -o "$WORK/bc-coverage/coverage.bc"
  scan "$WORK/bc-coverage" "$WORK/raw-coverage.txt"
  cat "$WORK/raw-coverage.txt"
  cov_fail=0
  # expect_hit <function> <RISK> <reasons>
  expect_hit() {
    grep -q "^HIT,.*,$1,.*,$2,$3\$" "$WORK/raw-coverage.txt" \
      || { echo "COVERAGE FAIL: $1 should hit as $2/$3"; cov_fail=1; }
  }
  # expect_miss <function> — examined as a loop, but not a hit
  expect_miss() {
    grep -q "^LOOP,.*,$1\$" "$WORK/raw-coverage.txt" \
      || { echo "COVERAGE FAIL: $1 was not even examined"; cov_fail=1; }
    grep -q "^HIT,.*,$1," "$WORK/raw-coverage.txt" \
      && { echo "COVERAGE FAIL: $1 should not hit"; cov_fail=1; }
    return 0
  }
  expect_hit  mixture           HIGH exp-chain
  expect_hit  softmax_denom     HIGH 'exp-chain;exp-sum'
  expect_hit  manual_lse        HIGH 'exp-chain;exp-sum'
  expect_hit  forward_step_reg  LOW  none
  expect_hit  kernel_sum        LOW  none
  # Memory-carried accumulator: `out[j] += prev[i] * A[i*n+j]`, where the
  # accumulator IS the array cell. This was a documented blind spot until
  # 2026-08-17, when recognition moved to RecurrenceDescriptor, which
  # processes stores to loop-invariant addresses (IntermediateStore).
  # Asserted as a HIT so the closure cannot silently regress.
  expect_hit  forward_step_mem  LOW  none
  # The forward algorithm WITH its enclosing time-step loop, in both the forms
  # real code writes it. Seen, and graded LOW — which is the open gap, not the
  # desired answer: the decay that makes this family underflow lives in the
  # outer loop and per-loop risk cannot see it. Asserted at the wrong verdict
  # deliberately, so a cross-loop signal turns these red on the day it lands
  # and cannot ship without this table being updated with it.
  expect_hit  forward_full_flat  LOW  none
  expect_hit  forward_full_swap  LOW  none
  # Still a known gap, and asserted so the docs cannot quietly become wrong in
  # EITHER direction: a pure product is exponent-tracking's job, not this
  # project's. RecurKind::FMul is filtered out for exactly this reason.
  expect_miss likelihood_product
  if [ "$cov_fail" = 0 ]; then
    echo "COVERAGE PASS (8 named shapes seen incl. memory-carried and both"\
         "full forward-algorithm forms at LOW; likelihood_product still"\
         "missed, as documented)"
  else
    echo "COVERAGE FAIL: RESULTS.md 'Coverage against the shapes this project names' is stale"
    exit 1
  fi
  ;;
report)
  echo "== matcher hit-rate study =="
  for raw in "$WORK"/raw-*.txt; do
    [ -e "$raw" ] || continue
    summarize "$raw" "$(basename "$raw" .txt | sed 's/^raw-//')"
  done
  ;;
frame)
  # R2 frame census (matcher/RESCUE.md, "Sampling frame"). Which of the 814
  # hits can be decomposed symbolically, by tier. STATIC only: sop-instrument
  # runs under -disable-output, so nothing is linked or executed.
  #
  # The frame is the log-ifiable population, so this number decides what the
  # study can generalise to. RESCUE.md requires the exclusion rate PER TIER,
  # because a frame restriction falling unevenly across tiers biases the
  # comparison the study exists to make.
  build_plugin
  shift || true
  targets="${*:-darknet gsl libsvm}"
  all="$WORK/raw-frame-all.txt"
  : > "$all"
  for name in $targets; do
    bcdir="$WORK/bc-$name"
    [ -d "$bcdir" ] || { echo "no harvested bitcode at $bcdir; skipping" >&2; continue; }
    scan "$bcdir" "$WORK/raw-frame-$name.txt" 'sop-instrument'
    cat "$WORK/raw-frame-$name.txt" >> "$all"
  done

  echo
  echo "== frame census: $targets =="
  printf '%-6s %8s %8s %8s %10s\n' "tier" "hits" "logifiable" "excluded" "excl-rate"
  for tier in HIGH MED LOW; do
    inst=$(grep -c "^INSTRUMENT,.*,$tier," "$all" || true)
    unl=$(grep -c "^UNLOGIFIABLE,.*,$tier\$" "$all" || true)
    tot=$((inst + unl))
    if [ "$tot" -gt 0 ]; then
      rate=$(awk -v u="$unl" -v t="$tot" 'BEGIN{printf "%.1f%%", 100*u/t}')
    else
      rate="n/a"
    fi
    printf '%-6s %8s %8s %8s %10s\n' "$tier" "$tot" "$inst" "$unl" "$rate"
  done
  echo
  echo "top decline reasons:"
  grep '^UNLOGIFIABLE,' "$all" | awk -F, '{print $(NF-1)}' | sort | uniq -c |
    sort -rn | head -10
  ;;
instrument)
  # The rescue instrument's controls, on real IR (matcher/RESCUE.md, R1).
  #
  # Instruments matcher/instr_control.c, links it against rescue_shim.cpp and
  # runs it. The probe must fire in BOTH directions: a positive control that
  # is rescued and a negative control that is NOT. One direction cannot tell a
  # safe site from a broken instrument, which is the same defect as a gate that
  # cannot fail.
  build_plugin
  IW="$WORK/instr"
  mkdir -p "$IW"
  REPO="$(cd "$HERE/.." && pwd)"

  "$CLANG" -O1 -g -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
    -emit-llvm -c "$HERE/instr_control.c" -o "$IW/ctl.bc"
  "$OPT" -load-pass-plugin="$HERE/build/SopMatcher.so" \
    -passes=loop-simplify,lcssa,sop-instrument "$IW/ctl.bc" -o "$IW/ctl_i.bc" \
    2> "$IW/instrument.log"
  cat "$IW/instrument.log"

  # The shim is C++ (dd_exp.h, log_math.h), the target is C: link with clang++.
  CXX="${CLANG}++"
  command -v "$CXX" > /dev/null 2>&1 || CXX="clang++"
  "$CXX" -O1 -I "$REPO/include" -I "$REPO/tests" \
    -c "$HERE/rescue_shim.cpp" -o "$IW/shim.o"
  "$CXX" -O1 "$IW/ctl_i.bc" "$IW/shim.o" -lm -o "$IW/ctl"
  "$IW/ctl" > "$IW/run.log"
  cat "$IW/run.log"

  fail=0
  # The three descriptors are asserted verbatim. They are what the shim's own
  # test hand-writes, so a drift between the static predicate and the replay
  # shows up here rather than as a wrong study result.
  grep -q '^INSTRUMENT,.*,ctl_mixture,HIGH,2,L0 L1 EXP MUL$' "$IW/instrument.log" \
    || { echo "FAIL: mixture descriptor is not 'L0 L1 EXP MUL'"; fail=1; }
  grep -q '^INSTRUMENT,.*,ctl_dot,LOW,2,L0 L1 MUL$' "$IW/instrument.log" \
    || { echo "FAIL: dot descriptor is not 'L0 L1 MUL'"; fail=1; }
  grep -q '^INSTRUMENT,.*,ctl_softmax_f32,HIGH,1,L0 EXT EXP TRUNCF$' "$IW/instrument.log" \
    || { echo "FAIL: f32 descriptor is not 'L0 EXT EXP TRUNCF'"; fail=1; }

  # An undecomposable in-loop call must DECLINE, not become a leaf: taking
  # log|sin(x)| reconstructs whatever sin already collapsed internally, which
  # is the failure the symbolic replay exists to prevent. This assertion was
  # added after a mutation (opaque calls -> leaves) left every other control
  # green, because nothing here had an undecomposable call in a term chain.
  grep -q '^UNLOGIFIABLE,.*,ctl_opaque,opaque-call,' "$IW/instrument.log" \
    || { echo "FAIL: opaque call was not declined"; fail=1; }
  if grep -q '^INSTRUMENT,.*,ctl_opaque,' "$IW/instrument.log"; then
    echo "FAIL: opaque call was instrumented anyway"; fail=1
  fi

  # Positive control: rescued, and by a RANGE failure.
  grep -qE '^INSTR,.*,ctl_mixture,exec=1,rescue=1,.*,range=1,' "$IW/run.log" \
    || { echo "FAIL: positive control not rescued via a range failure"; fail=1; }
  # Negative control: NOT rescued. This is the direction a broken probe fakes.
  grep -qE '^INSTR,.*,ctl_dot,exec=1,rescue=0,' "$IW/run.log" \
    || { echo "FAIL: negative control was rescued; the probe cannot decline"; fail=1; }
  # fptrunc: the narrowing collapsed every term, and that is NOT a rescue.
  # It foreclosed the answer before the accumulation ever saw it, which is a
  # different finding from "log-domain accumulation would have helped".
  grep -qE '^INSTR,.*,ctl_softmax_f32,exec=1,rescue=0,.*,trunc_collapse=100,' "$IW/run.log" \
    || { echo "FAIL: f32 narrowing not counted as a collapse, or counted as a rescue"; fail=1; }

  # The sensitivity grid must be RECORDED, not just promised. RESCUE.md
  # requires R3 to report tier rates across margin x T_default and says that
  # costs nothing because classification is post-processing; that is only true
  # if the run keeps enough to reclassify. One INSTRGRID line per site, with
  # the registered cell equal to the headline rescue count by construction.
  for fn in ctl_mixture ctl_dot ctl_softmax_f32; do
    grep -q "^INSTRGRID,.*,$fn,exec=1,t_declared=0,g=" "$IW/run.log" \
      || { echo "FAIL: no grid record for $fn"; fail=1; }
  done
  # Cell [1][1] is the 5th of the nine, margin-major.
  reg=$(grep '^INSTRGRID,.*,ctl_mixture,' "$IW/run.log" | sed 's/.*g=//' | cut -d: -f5)
  head=$(grep '^INSTR,.*,ctl_mixture,' "$IW/run.log" | sed 's/.*rescue=//' | cut -d, -f1)
  [ "$reg" = "$head" ] \
    || { echo "FAIL: registered grid cell $reg != headline rescue $head"; fail=1; }

  [ "$fail" = "0" ] || { echo "INSTRUMENT CONTROLS FAILED"; exit 1; }
  echo "INSTRUMENT OK (both directions fire; descriptors match the shim's test;"
  echo "               grid recorded, registered cell == headline)"
  ;;
"")
  echo "usage: run_study.sh selftest | coverage | figures | instrument | weights [name...] | rejects [name...] | <codebase-name> | report" >&2
  exit 2
  ;;
*)
  name="$1"
  src="$WORK/$name"
  [ -d "$src" ] || { echo "clone the target at $src first" >&2; exit 1; }
  build_plugin
  export BC_DIR="$WORK/bc-$name"
  mkdir -p "$BC_DIR"
  echo "Build $name with CC=$HERE/cc-bc.sh (see METHODOLOGY.md), e.g.:"
  echo "  cd $src && make CC=$HERE/cc-bc.sh -k"
  echo "Then rerun: run_study.sh $name  — it scans whatever bitcode exists."
  scan "$BC_DIR" "$WORK/raw-$name.txt"
  summarize "$WORK/raw-$name.txt" "$name"
  ;;
esac
