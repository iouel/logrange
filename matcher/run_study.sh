#!/usr/bin/env bash
# run_study.sh — the matcher hit-rate study, end to end (WSL/Linux).
#
#   ./run_study.sh selftest   build plugin, run the labeled ground truth,
#                             assert expected counts (the validation gate)
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

# Run the matcher over every .bc in a directory; write raw lines to $2.
scan() {
  local bcdir="$1" out="$2" passes="${3:-sop-matcher}"
  : > "$out"
  local n=0
  for bc in "$bcdir"/*.bc; do
    [ -e "$bc" ] || continue
    "$OPT" -load-pass-plugin="$PLUGIN" -passes="$passes" \
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
  cu="$WORK/raw-rejects-cleanuses.txt"
  grep '^REJECT,cleanUses' "$all" > "$cu" || true
  # Any line not starting with a known record tag means interleaved output.
  bad=$(grep -vcE '^(LOOP|HIT|REJECT),' "$all" || true)
  echo
  echo "== rejection census: $targets =="
  printf 'malformed lines (interleaving check)   %s\n' "$bad"
  printf 'cleanUses rejects                      %s\n' "$(wc -l < "$cu")"
  printf '  extra user is invariant-addr store   %s\n' "$(grep -c 'store-of-spine:invariant-addr' "$cu" || true)"
  printf '  extra user is varying-addr store     %s\n' "$(grep -c 'store-of-spine:varying-addr' "$cu" || true)"
  printf '  extra user is a non-store            %s\n' "$(grep -c 'other-user' "$cu" || true)"
  printf '  none on the update (phi/other spine) %s\n' "$(grep -vcE 'store-of-spine|other-user' "$cu" || true)"
  [ "$bad" = "0" ] || { echo "FAIL: interleaved output detected"; exit 1; }
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
  # Known gaps. These are asserted so the docs cannot quietly become wrong in
  # EITHER direction: if one starts hitting, RESULTS.md needs updating too.
  expect_miss forward_step_mem
  expect_miss likelihood_product
  if [ "$cov_fail" = 0 ]; then
    echo "COVERAGE PASS (5 named shapes seen; forward_step_mem and"\
         "likelihood_product still missed, as documented)"
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
"")
  echo "usage: run_study.sh selftest | coverage | rejects [name...] | <codebase-name> | report" >&2
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
