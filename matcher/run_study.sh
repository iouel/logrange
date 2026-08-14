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
  local bcdir="$1" out="$2"
  : > "$out"
  local n=0
  for bc in "$bcdir"/*.bc; do
    [ -e "$bc" ] || continue
    "$OPT" -load-pass-plugin="$PLUGIN" -passes=sop-matcher \
      -disable-output "$bc" 2>> "$out" || true
    n=$((n + 1))
  done
  echo "scanned $n modules from $bcdir"
}

summarize() {
  local raw="$1" label="$2"
  local loops hits trans const_trip
  loops=$(grep -c '^LOOP,' "$raw" || true)
  hits=$(grep -c '^HIT,' "$raw" || true)
  trans=$(grep -c '^HIT,.*,transcendental$' "$raw" || true)
  const_trip=$(grep -c '^HIT,.*,constant,' "$raw" || true)
  printf '%-12s  fp-loops=%-5s hits=%-4s transcendental=%-3s const-trip=%s\n' \
    "$label" "$loops" "$hits" "$trans" "$const_trip"
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
  trans=$(grep -c 'transcendental$' "$WORK/raw-selftest.txt" || true)
  # Labels in selftest.c: 4 hits out of 6 examined FP loops, 1 transcendental.
  if [ "$hits" = 4 ] && [ "$loops" = 6 ] && [ "$trans" = 1 ]; then
    echo "SELFTEST PASS (4 hits / 6 fp-loops / 1 transcendental)"
  else
    echo "SELFTEST FAIL: hits=$hits (want 4) loops=$loops (want 6) trans=$trans (want 1)"
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
  echo "usage: run_study.sh selftest | <codebase-name> | report" >&2
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
