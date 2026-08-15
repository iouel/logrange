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

# Toolchain pinned to 21 on BOTH halves. Previously this script drove
# unversioned `clang` against `opt-21`: the IR producer and the IR consumer
# could drift to different LLVM major versions on any machine with more than
# one clang installed, and the failure mode is a confusing bitcode-version
# error or, worse, a silent shape change.
CLANG=clang-21
OPT=opt-21

mkdir -p "$WORK"

echo "== 1. build plugin =="
# Delete the old .so first. A failed build otherwise leaves the previous
# plugin on disk and every check below silently exercises stale code — this
# project has been bitten by exactly that.
rm -f "$BUILD/LogRewrite.so"
cmake -S "$PASSDIR" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > "$WORK/cmake.log"
cmake --build "$BUILD" -j >> "$WORK/cmake.log"
[ -f "$BUILD/LogRewrite.so" ] \
  || { echo "FAIL: plugin did not build"; tail -40 "$WORK/cmake.log"; exit 1; }
echo "plugin: $BUILD/LogRewrite.so"

echo "== 2. compile kernels to IR (identical source, two names) =="
# -fno-math-errno is REQUIRED, not a tuning knob. The pass matches only
# llvm.exp.* (its errno contract: replacing the loop changes errno-visible
# behaviour, and the intrinsic is exactly the marker that errno is already
# unobservable). Without this flag clang emits `call double @exp` at -O1 and
# the kernel does not match at all — it is declined as DECLINE-ERRNO.
# Section 3b asserts that decline directly.
KFLAGS="-O1 -g -fno-discard-value-names -fno-math-errno"
RENAME_ORIG="-Dsoftmax_denom=softmax_denom_orig -Dplain_sum=plain_sum_orig \
             -Ddot_sum=dot_sum_orig -Dinvariant_exp_sum=invariant_exp_sum_orig"
RENAME_RW="-Dsoftmax_denom=softmax_denom_rw -Dplain_sum=plain_sum_rw \
           -Ddot_sum=dot_sum_rw -Dinvariant_exp_sum=invariant_exp_sum_rw"
$CLANG $KFLAGS -DKERNEL $RENAME_ORIG \
      -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/kernel_orig.ll"
$CLANG $KFLAGS -DKERNEL $RENAME_RW \
      -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/kernel_rw.ll"

echo "== 3. rewrite (force=1: the explicit reassociation grant) =="
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
       -passes='log-rewrite<force>' -verify-each \
       -S "$WORK/kernel_rw.ll" -o "$WORK/kernel_rw_opt.ll" \
       2> "$WORK/rewrite.log" || { cat "$WORK/rewrite.log"; exit 1; }
cat "$WORK/rewrite.log"
# Exactly ONE rewrite: softmax_denom_rw fires, the three negative-control
# loops in the same module must be declined.
NREW="$(grep -c '^REWRITE,' "$WORK/rewrite.log" || true)"
[ "$NREW" = "1" ] \
  || { echo "FAIL: expected exactly 1 REWRITE line, got $NREW"; exit 1; }
grep -q '^REWRITE,.*,softmax_denom_rw,HIGH,exp-chain;exp-sum$' "$WORK/rewrite.log" \
  || { echo "FAIL: rewrite fired on the wrong function, or without its risk verdict"; exit 1; }
# On the intended build flags nothing should be declined for a safety reason.
# A DECLINE line here means -fno-math-errno or the FP environment regressed.
[ "$(grep -c '^DECLINE-' "$WORK/rewrite.log" || true)" = "0" ] \
  || { echo "FAIL: unexpected safety decline on the reference build"; exit 1; }

# The profitability gate (intent step 9) must be able to DECLINE, not just
# permit. For this shape the verdict is HIGH by construction, so raising the
# threshold above HIGH is the only way to exercise the refusal path — do it,
# rather than ship a gate whose decline branch has never run.
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
       -passes='log-rewrite<force;min-risk=none>' \
       -S "$WORK/kernel_rw.ll" -o /dev/null \
       2> "$WORK/gate.log" || { cat "$WORK/gate.log"; exit 1; }
grep -q '^DECLINE-RISK,.*,softmax_denom_rw,HIGH,below-min-NONE$' "$WORK/gate.log" \
  || { echo "FAIL: gate did not decline when the threshold was raised"; exit 1; }
[ "$(grep -c '^REWRITE,' "$WORK/gate.log" || true)" = "0" ] \
  || { echo "FAIL: gate declined but rewrote anyway"; exit 1; }
echo "PASS,gate_declines_above_threshold"

# An unrecognised parameter must be refused, not silently ignored — a typo in
# min-risk that quietly reverted to the default would disable the gate.
if $OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
          -passes='log-rewrite<force;min-risk=hgih>' \
          -S "$WORK/kernel_rw.ll" -o /dev/null > /dev/null 2>&1; then
  echo "FAIL: misspelled parameter was accepted"; exit 1
fi
echo "PASS,unknown_parameter_refused"
grep -q 'llvm.maxnum.f64' "$WORK/kernel_rw_opt.ll" \
  || { echo "FAIL: rewritten IR lacks the streaming state"; exit 1; }
# The replacement value must actually be CONSUMED, not just defined —
# definition line plus at least one use. (Regression guard: an early version
# left the original sum wired to the return; only the export hook worked.)
[ "$(grep -c '%lr\.sum' "$WORK/kernel_rw_opt.ll")" -ge 2 ] \
  || { echo "FAIL: linear replacement %lr.sum is defined but never used"; exit 1; }

echo "== 3b. safety declines (force must NOT override any of these) =="
# Each check below was written against the previous build first and observed
# to FAIL there: the old pass rewrote the errno case and the denormal case,
# and emitted nothing at all for the strict-FP case.

# (i) errno contract: the SAME kernel source, compiled WITHOUT
#     -fno-math-errno, emits `call double @exp` and must be declined. This
#     is the check that makes -fno-math-errno load-bearing rather than
#     decorative, and it uses force — proving force does not waive it.
$CLANG -O1 -g -fno-discard-value-names -DKERNEL \
       -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/errno_kernel.ll"
grep -q 'call double @exp(' "$WORK/errno_kernel.ll" \
  || { echo "FAIL: expected an external @exp call without -fno-math-errno"; exit 1; }
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
     -passes='log-rewrite<force>' \
     -S "$WORK/errno_kernel.ll" -o /dev/null \
     2> "$WORK/errno.log" || { cat "$WORK/errno.log"; exit 1; }
[ "$(grep -c '^REWRITE,' "$WORK/errno.log" || true)" = "0" ] \
  || { echo "FAIL: external exp call was rewritten (errno contract broken)"; exit 1; }
grep -q '^DECLINE-ERRNO,.*,softmax_denom,external-exp-call$' "$WORK/errno.log" \
  || { echo "FAIL: external exp call not declined with the errno reason"; exit 1; }
echo "PASS,decline_external_exp_call"

# (ii) strict/constrained FP. A small TU compiled -ffp-model=strict carries
#      the strictfp function attribute AND llvm.experimental.constrained.*
#      operations. force must not rewrite it.
cat > "$WORK/strict_kernel.c" <<'EOF'
#include <math.h>
double softmax_denom_strict(const double *x, int n) {
  double s = 0.0;
  for (int i = 0; i < n; ++i)
    s += exp(x[i]);
  return s;
}
EOF
$CLANG -O1 -g -fno-discard-value-names -fno-math-errno -ffp-model=strict \
       -S -emit-llvm "$WORK/strict_kernel.c" -o "$WORK/strict.ll"
grep -q 'strictfp' "$WORK/strict.ll" \
  || { echo "FAIL: -ffp-model=strict did not produce a strictfp function"; exit 1; }
grep -q 'llvm.experimental.constrained' "$WORK/strict.ll" \
  || { echo "FAIL: -ffp-model=strict did not produce constrained intrinsics"; exit 1; }
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
     -passes='log-rewrite<force>' \
     -S "$WORK/strict.ll" -o /dev/null \
     2> "$WORK/strict.log" || { cat "$WORK/strict.log"; exit 1; }
[ "$(grep -c '^REWRITE,' "$WORK/strict.log" || true)" = "0" ] \
  || { echo "FAIL: force rewrote a strict-FP function"; exit 1; }
grep -q '^DECLINE-FPENV,.*,softmax_denom_strict,strictfp$' "$WORK/strict.log" \
  || { echo "FAIL: strict-FP function not declined with the strictfp reason"; exit 1; }
echo "PASS,decline_strictfp_under_force"

# (iii) constrained operations on their own, with the strictfp attribute
#       stripped — proving the constrained-op scan is a real second check and
#       not a side effect of the attribute test. Derived from the clang
#       output above rather than hand-written, so it stays representative.
#       (Removing strictfp can empty an attribute group; refill it, else the
#       IR does not parse.)
sed -e 's/ strictfp//g' -e 's/strictfp //g' "$WORK/strict.ll" \
  | sed -e 's/^attributes \(#[0-9]*\) = { }$/attributes \1 = { nounwind }/' \
  > "$WORK/constrained.ll"
[ "$(grep -c 'strictfp' "$WORK/constrained.ll" || true)" = "0" ] \
  || { echo "FAIL: strictfp not fully stripped from the constrained-only TU"; exit 1; }
grep -q 'llvm.experimental.constrained' "$WORK/constrained.ll" \
  || { echo "FAIL: constrained-only TU lost its constrained intrinsics"; exit 1; }
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
     -passes='log-rewrite<force>' \
     -S "$WORK/constrained.ll" -o /dev/null \
     2> "$WORK/constrained.log" || { cat "$WORK/constrained.log"; exit 1; }
[ "$(grep -c '^REWRITE,' "$WORK/constrained.log" || true)" = "0" ] \
  || { echo "FAIL: force rewrote a function containing constrained FP ops"; exit 1; }
grep -q '^DECLINE-FPENV,.*,softmax_denom_strict,constrained-fp$' "$WORK/constrained.log" \
  || { echo "FAIL: constrained FP ops not declined with the constrained-fp reason"; exit 1; }
echo "PASS,decline_constrained_fp_under_force"

# (iv) non-default denormal environment. -fdenormal-fp-math=preserve-sign
#      stamps "denormal-fp-math"="preserve-sign,preserve-sign" on every
#      function; which intermediates flush to zero is not preservable when
#      the intermediates themselves change.
$CLANG $KFLAGS -fdenormal-fp-math=preserve-sign -DKERNEL \
       -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/denorm.ll"
grep -q '"denormal-fp-math"="preserve-sign' "$WORK/denorm.ll" \
  || { echo "FAIL: -fdenormal-fp-math=preserve-sign left no attribute"; exit 1; }
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
     -passes='log-rewrite<force>' \
     -S "$WORK/denorm.ll" -o /dev/null \
     2> "$WORK/denorm.log" || { cat "$WORK/denorm.log"; exit 1; }
[ "$(grep -c '^REWRITE,' "$WORK/denorm.log" || true)" = "0" ] \
  || { echo "FAIL: force rewrote a non-default denormal-mode function"; exit 1; }
grep -q '^DECLINE-FPENV,.*,softmax_denom,denormal-fp-math$' "$WORK/denorm.log" \
  || { echo "FAIL: denormal mode not declined with the denormal-fp-math reason"; exit 1; }
echo "PASS,decline_denormal_env_under_force"

echo "== 4. codegen, link, run =="
$CLANG -O1 -c "$WORK/kernel_orig.ll"   -o "$WORK/kernel_orig.o"
$CLANG -O1 -c "$WORK/kernel_rw_opt.ll" -o "$WORK/kernel_rw.o"
$CLANG -O1 -c "$PASSDIR/test_softmax.c" -o "$WORK/main.o"
$CLANG "$WORK/main.o" "$WORK/kernel_orig.o" "$WORK/kernel_rw.o" -lm \
      -o "$WORK/test_softmax"
"$WORK/test_softmax" | tee "$WORK/test.out"

grep -q '^OVERALL,PASS$' "$WORK/test.out" \
  || { echo "run_pass_test: FAIL"; exit 1; }
echo "run_pass_test: PASS"
