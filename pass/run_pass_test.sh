#!/usr/bin/env bash
# run_pass_test.sh — build the log-rewrite plugin, rewrite the softmax kernel,
# link straight + rewritten kernels into one binary, run the comparisons.
#
# Run inside WSL:   bash pass/run_pass_test.sh
# Build state lives under ~/logrange-pass (WSL /tmp does not persist across
# wsl.exe invocations, and building on /mnt/c is slow).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PASSDIR="$REPO/pass"
WORK="$HOME/logrange-pass"
BUILD="$WORK/build"
export CMAKE_PREFIX_PATH=/usr/lib/llvm-21

mkdir -p "$WORK"

echo "== 1. build plugin =="
cmake -S "$PASSDIR" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > "$WORK/cmake.log"
cmake --build "$BUILD" -j >> "$WORK/cmake.log"
echo "plugin: $BUILD/LogRewrite.so"

echo "== 2. compile kernels to IR (identical source, two names) =="
KFLAGS="-O1 -g -fno-discard-value-names"
RENAME_ORIG="-Dsoftmax_denom=softmax_denom_orig -Dplain_sum=plain_sum_orig \
             -Ddot_sum=dot_sum_orig -Dinvariant_exp_sum=invariant_exp_sum_orig"
RENAME_RW="-Dsoftmax_denom=softmax_denom_rw -Dplain_sum=plain_sum_rw \
           -Ddot_sum=dot_sum_rw -Dinvariant_exp_sum=invariant_exp_sum_rw"
clang $KFLAGS -DKERNEL $RENAME_ORIG \
      -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/kernel_orig.ll"
clang $KFLAGS -DKERNEL $RENAME_RW \
      -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/kernel_rw.ll"

echo "== 3. rewrite (force=1: the explicit reassociation grant) =="
opt-21 -load-pass-plugin="$BUILD/LogRewrite.so" \
       -passes='log-rewrite<force>' -verify-each \
       -S "$WORK/kernel_rw.ll" -o "$WORK/kernel_rw_opt.ll" \
       2> "$WORK/rewrite.log" || { cat "$WORK/rewrite.log"; exit 1; }
cat "$WORK/rewrite.log"
# Exactly ONE rewrite: softmax_denom_rw fires, the three negative-control
# loops in the same module must be declined.
NREW="$(grep -c '^REWRITE,' "$WORK/rewrite.log" || true)"
[ "$NREW" = "1" ] \
  || { echo "FAIL: expected exactly 1 REWRITE line, got $NREW"; exit 1; }
grep -q '^REWRITE,.*softmax_denom_rw$' "$WORK/rewrite.log" \
  || { echo "FAIL: rewrite fired on the wrong function"; exit 1; }
grep -q 'llvm.maxnum.f64' "$WORK/kernel_rw_opt.ll" \
  || { echo "FAIL: rewritten IR lacks the streaming state"; exit 1; }
# The replacement value must actually be CONSUMED, not just defined —
# definition line plus at least one use. (Regression guard: an early version
# left the original sum wired to the return; only the export hook worked.)
[ "$(grep -c '%lr\.sum' "$WORK/kernel_rw_opt.ll")" -ge 2 ] \
  || { echo "FAIL: linear replacement %lr.sum is defined but never used"; exit 1; }

echo "== 4. codegen, link, run =="
clang -O1 -c "$WORK/kernel_orig.ll"   -o "$WORK/kernel_orig.o"
clang -O1 -c "$WORK/kernel_rw_opt.ll" -o "$WORK/kernel_rw.o"
clang -O1 -c "$PASSDIR/test_softmax.c" -o "$WORK/main.o"
clang "$WORK/main.o" "$WORK/kernel_orig.o" "$WORK/kernel_rw.o" -lm \
      -o "$WORK/test_softmax"
"$WORK/test_softmax" | tee "$WORK/test.out"

grep -q '^OVERALL,PASS$' "$WORK/test.out" \
  || { echo "run_pass_test: FAIL"; exit 1; }
echo "run_pass_test: PASS"
