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
RENAME_ORIG="-Dsoftmax_denom=softmax_denom_orig -Dsoftmax_full=softmax_full_orig \
            -Dsoftmax_add=softmax_add_orig -Dsoftmax_sum_div=softmax_sum_div_orig \
            -Dplain_sum=plain_sum_orig -Ddot_sum=dot_sum_orig \
            -Dinvariant_exp_sum=invariant_exp_sum_orig \
            -Dweighted_sum=weighted_sum_orig \
            -Dconst_weight_sum=const_weight_sum_orig \
            -Dexpf_widened=expf_widened_orig"
RENAME_RW="-Dsoftmax_denom=softmax_denom_rw -Dsoftmax_full=softmax_full_rw \
           -Dsoftmax_add=softmax_add_rw -Dsoftmax_sum_div=softmax_sum_div_rw \
           -Dplain_sum=plain_sum_rw -Ddot_sum=dot_sum_rw \
           -Dinvariant_exp_sum=invariant_exp_sum_rw \
           -Dweighted_sum=weighted_sum_rw \
           -Dconst_weight_sum=const_weight_sum_rw \
           -Dexpf_widened=expf_widened_rw"
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
# LOWER BOUND on the rewritten set: these shapes must not stop being
# rewritten. A regression that narrows the match would otherwise be invisible,
# because a loop that is no longer rewritten trivially agrees with itself.
for fn in softmax_denom_rw softmax_full_rw softmax_add_rw softmax_sum_div_rw \
          expf_widened_rw; do
  grep -q "^REWRITE,.*,$fn,HIGH,exp-chain;exp-sum$" "$WORK/rewrite.log" \
    || { echo "FAIL: rewrite missing for $fn"; exit 1; }
done
# UPPER BOUND, and this is the contract rather than a list: whatever the pass
# rewrote must have a semantic assertion behind it. The set is derived from
# the pass's own output, so a newly rewritten function cannot enter the build
# without also entering the harness. Checked after section 4 runs the binary,
# at the "semantic coverage" block below.
#
# This replaces a hardcoded "exactly 4 REWRITE lines". That count was
# satisfiable while the pass miscompiled: relaxing the weight clause to admit
# constant weights left the count at 4 and every other assertion green,
# because no constant-weight kernel existed to be counted. The count pinned
# the implementation; this pins the property.
NREW="$(grep -c '^REWRITE,' "$WORK/rewrite.log" || true)"
[ "$NREW" -ge 5 ] \
  || { echo "FAIL: expected at least 5 REWRITE lines, got $NREW"; exit 1; }
# On the intended build flags nothing should be declined for a SAFETY reason.
# A DECLINE-FPENV or DECLINE-ERRNO line here means -fno-math-errno or the FP
# environment regressed.
#
# This was a flat "zero DECLINE- lines" until the fmuladd widening, which made
# two scope declines expected on this build (asserted individually below). It
# is an allow-list rather than a relaxed count so that a *new* decline still
# fails: the count check below pins the total, so a third decline of any kind
# trips it even if it carries one of the allowed tags.
[ "$(grep -cE '^DECLINE-(FPENV|ERRNO),' "$WORK/rewrite.log" || true)" = "0" ] \
  || { echo "FAIL: unexpected safety decline on the reference build"; exit 1; }
[ "$(grep -c '^DECLINE-' "$WORK/rewrite.log" || true)" = "3" ] \
  || { echo "FAIL: expected exactly 3 DECLINE lines on the reference build, got $(grep -c '^DECLINE-' "$WORK/rewrite.log" || true)"; exit 1; }
grep -q '^CONSUMER-DECLINE,.*,softmax_denom_rw,not-fdiv$' "$WORK/rewrite.log" \
  || { echo "FAIL: standalone denominator consumer shape was not logged"; exit 1; }
grep -q '^CONSUMER-MATCH,.*,softmax_full_rw,fdiv-of-sum$' "$WORK/rewrite.log" \
  || { echo "FAIL: full softmax divide consumer did not log as a match"; exit 1; }
grep -q '^CONSUMER-DECLINE,.*,softmax_add_rw,not-fdiv$' "$WORK/rewrite.log" \
  || { echo "FAIL: fadd near-miss consumer did not log not-fdiv"; exit 1; }
grep -q '^CONSUMER-DECLINE,.*,softmax_sum_div_rw,not-the-sum$' "$WORK/rewrite.log" \
  || { echo "FAIL: wrong-divisor near-miss consumer did not log not-the-sum"; exit 1; }

# --- shape coverage: the fmuladd spine (posture condition 3) ----------------
# Both of these are MATCHED and then declined, which is the whole point. Until
# the match widened past fadd(phi, exp(t)) they were not matched at all, so
# the pass said nothing about them and the risk gate had no real input to
# refuse. Written before the implementation and observed failing.
#
# (a) The no-exp spine. dot_sum's update is llvm.fmuladd(a, b, phi) at these
#     flags — structurally identical to the mixture spine, the only difference
#     being that no operand reaches an exp. Its verdict is LOW, so the DEFAULT
#     threshold declines it. This is the first time the gate refuses a real
#     input rather than a synthetically raised threshold, which is what
#     condition 3 exists for.
grep -q '^DECLINE-RISK,.*,dot_sum_rw,LOW,below-min-HIGH$' "$WORK/rewrite.log" \
  || { echo "FAIL: fmuladd dot spine was not matched-and-declined by the gate"; exit 1; }
# (b) The weighted spine. Verdict is HIGH (an operand reaches exp), so the
#     gate passes it and the magnitude restriction is what must stop it: the
#     emitted state would accumulate sum(w_i * exp(t_i - m)), reaching
#     sum|w_i|, which overflows where the linear original does not.
grep -q '^DECLINE-WEIGHT,.*,weighted_sum_rw,unbounded-weight$' "$WORK/rewrite.log" \
  || { echo "FAIL: weighted spine was not matched-and-declined on magnitude"; exit 1; }
# (b2) A CONSTANT weight — provably positive, provably bounded — must decline
#      too. The refusal is structural: nothing reads the weight after the
#      clause, so a weight admitted here would be silently dropped and the
#      result wrong by a factor of w at every magnitude. This case is the one
#      that was missing when the suite went green against a pass that
#      miscompiled `s += 0.5*exp(x)`.
grep -q '^DECLINE-WEIGHT,.*,const_weight_sum_rw,unbounded-weight$' "$WORK/rewrite.log" \
  || { echo "FAIL: constant weight was not declined"; exit 1; }
# (c) The same two spines with the multiply NOT contracted. Whether clang
#     emits llvm.fmuladd or fmul + fadd is -ffp-contract, a flag the pass does
#     not control, so matching one form only would make coverage a property of
#     the caller's setting. Measured here rather than assumed: =on (the
#     default, used above) gives fmuladd, =off and =fast give fmul + fadd.
for fc in off fast; do
  $CLANG $KFLAGS -ffp-contract=$fc -DKERNEL $RENAME_RW \
        -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/contract_$fc.ll"
  if grep -q 'llvm.fmuladd' "$WORK/contract_$fc.ll"; then
    echo "FAIL: -ffp-contract=$fc still contracted; the check is vacuous"; exit 1
  fi
  $OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
         -passes='log-rewrite<force>' \
         -S "$WORK/contract_$fc.ll" -o /dev/null \
         2> "$WORK/contract_$fc.log" || { cat "$WORK/contract_$fc.log"; exit 1; }
  grep -q '^DECLINE-WEIGHT,.*,weighted_sum_rw,unbounded-weight$' "$WORK/contract_$fc.log" \
    || { echo "FAIL: uncontracted weighted spine missed at -ffp-contract=$fc"; exit 1; }
  grep -q '^DECLINE-RISK,.*,dot_sum_rw,LOW,below-min-HIGH$' "$WORK/contract_$fc.log" \
    || { echo "FAIL: uncontracted dot spine missed at -ffp-contract=$fc"; exit 1; }
done
echo "PASS,weighted_spine_matched_under_all_contract_settings"

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
grep -q '^DECLINE-RISK,.*,softmax_full_rw,HIGH,below-min-NONE$' "$WORK/gate.log" \
  || { echo "FAIL: gate missed full-softmax decline"; exit 1; }
grep -q '^DECLINE-RISK,.*,softmax_add_rw,HIGH,below-min-NONE$' "$WORK/gate.log" \
  || { echo "FAIL: gate missed softmax-add decline"; exit 1; }
grep -q '^DECLINE-RISK,.*,softmax_sum_div_rw,HIGH,below-min-NONE$' "$WORK/gate.log" \
  || { echo "FAIL: gate missed wrong-divisor decline"; exit 1; }
[ "$(grep -c '^REWRITE,' "$WORK/gate.log" || true)" = "0" ] \
  || { echo "FAIL: gate declined but rewrote anyway"; exit 1; }
echo "PASS,gate_declines_above_threshold"

# --- the threshold relation, and what the gate does NOT do -----------------
# Two thresholds the suite never exercised. Both parser branches were dead:
# min-risk=low and min-risk=med could each have been mis-mapped to any tier
# and nothing would have failed.
for mr in low med; do
  $OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
         -passes="log-rewrite<force;min-risk=$mr>" \
         -S "$WORK/kernel_rw.ll" -o /dev/null \
         2> "$WORK/mr_$mr.log" || { cat "$WORK/mr_$mr.log"; exit 1; }
done
# dot_sum verdicts LOW. At min-risk=low it clears the gate and the weight
# clause refuses it one line later; at min-risk=med the gate refuses it. The
# two tags pin the threshold relation from both sides.
grep -q '^DECLINE-WEIGHT,.*,dot_sum_rw,unbounded-weight$' "$WORK/mr_low.log" \
  || { echo "FAIL: at min-risk=low, dot_sum should clear the gate and decline on weight"; exit 1; }
grep -q '^DECLINE-RISK,.*,dot_sum_rw,LOW,below-min-MED$' "$WORK/mr_med.log" \
  || { echo "FAIL: at min-risk=med, dot_sum should be declined by the gate"; exit 1; }
echo "PASS,threshold_relation_low_and_med"

# THE GATE IS REACHABLE BUT NOT LOAD-BEARING, asserted rather than described.
# Lowering the threshold below every verdict the pass can produce does not add
# a single rewrite, because eligibility (an llvm.exp term) and a HIGH verdict
# are the same predicate: every rewritable loop already verdicts HIGH, and
# every LOW loop is weighted and refused by the weight clause regardless.
#
# This assertion is inverted on purpose. It encodes a LIMITATION, so it turns
# red exactly when that limitation is fixed — when the rewritable set finally
# exceeds the HIGH set and the gate starts refusing a real rewrite. That is
# the event posture condition 3 is actually waiting for, and this is what will
# announce it instead of it having to be noticed.
if ! diff <(grep '^REWRITE,' "$WORK/rewrite.log") \
          <(grep '^REWRITE,' "$WORK/mr_low.log") > "$WORK/gatediff.txt"; then
  echo "FAIL: min-risk=low changed the rewrite set — the gate has become"
  echo "      load-bearing. This is progress, not a bug: update posture"
  echo "      condition 3 and this assertion together."
  cat "$WORK/gatediff.txt"
  exit 1
fi
echo "PASS,gate_is_reachable_but_not_load_bearing"

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

echo "== 3c. propagate=div: compile a _prop copy, run with propagate=div =="
RENAME_PROP="-Dsoftmax_denom=softmax_denom_prop -Dsoftmax_full=softmax_full_prop \
            -Dsoftmax_add=softmax_add_prop -Dsoftmax_sum_div=softmax_sum_div_prop \
            -Dplain_sum=plain_sum_prop -Ddot_sum=dot_sum_prop \
            -Dinvariant_exp_sum=invariant_exp_sum_prop \
            -Dweighted_sum=weighted_sum_prop \
            -Dconst_weight_sum=const_weight_sum_prop \
            -Dexpf_widened=expf_widened_prop"
$CLANG $KFLAGS -DKERNEL $RENAME_PROP \
      -S -emit-llvm "$PASSDIR/test_softmax.c" -o "$WORK/kernel_prop.ll"
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
       -passes='log-rewrite<force;propagate=div>' \
       -S "$WORK/kernel_prop.ll" -o "$WORK/kernel_prop_opt.ll" \
       2> "$WORK/prop.log" || { cat "$WORK/prop.log"; exit 1; }
cat "$WORK/prop.log"
# softmax_full_prop consumer must be rewritten.
grep -q '^PROPAGATE,.*,softmax_full_prop$' "$WORK/prop.log" \
  || { echo "FAIL: softmax_full_prop consumer not propagated"; exit 1; }
# Near-miss consumers must be declined with reason tokens.
grep -q '^DECLINE-PROP,.*,softmax_add_prop,not-fdiv$' "$WORK/prop.log" \
  || { echo "FAIL: softmax_add_prop not declined with not-fdiv"; exit 1; }
grep -q '^DECLINE-PROP,.*,softmax_sum_div_prop,not-the-sum$' "$WORK/prop.log" \
  || { echo "FAIL: softmax_sum_div_prop not declined with not-the-sum"; exit 1; }
echo "PASS,propagate_div_rewrites_softmax_full"

# An unrecognised propagate= value must be refused, not silently ignored.
if $OPT -load-pass-plugin="$BUILD/LogRewrite.so" \
        -passes='log-rewrite<force;propagate=divvv>' \
        -S "$WORK/kernel_prop.ll" -o /dev/null > /dev/null 2>&1; then
  echo "FAIL: unknown propagate= value was accepted"; exit 1
fi
echo "PASS,unknown_propagate_refused"

# force alone (no propagate=div) must NOT produce any PROPAGATE lines.
[ "$(grep -c '^PROPAGATE,' "$WORK/rewrite.log" || true)" = "0" ] \
  || { echo "FAIL: force alone caused consumer propagation"; exit 1; }
echo "PASS,force_alone_does_not_propagate"

echo "== 3d. matcher agreement, soundness direction only =="
# Every loop the pass REWRITES must be graded HIGH by the matcher.
#
# This is the ONE direction that is a safety property. "Profitability analysis
# in front of any rewrite" (intent, posture condition 3) is only meaningful if
# the rewrite cannot fire on something the triage would not have flagged.
#
# Full verdict EQUALITY is deliberately not asserted, because it is false and
# documented as false: the matcher's exp-family is wider (pow, exp2, expm1 by
# substring) and its nMul counts the whole term chain including inside the exp
# argument, so `s += a*b*c*d*e` is matcher-MED where this pass prints LOW.
# The divergence table is ELIGIBILITY.md 7.1. Asserting equality here would
# either fail on day one or force the pass to grow a chain walk it does not
# otherwise need.
#
# The matcher runs over kernel_rw.ll — the SAME IR the pass consumed, not a
# fresh compile of the same source. A second compile could differ (flags,
# clang version, contraction) and then a disagreement would be evidence about
# the build rather than about the two analyses.
rm -f "$WORK/matcher-build/SopMatcher.so"
cmake -S "$PASSDIR/../matcher" -B "$WORK/matcher-build" \
      -DCMAKE_BUILD_TYPE=Release > "$WORK/matcher-cmake.log"
cmake --build "$WORK/matcher-build" -j >> "$WORK/matcher-cmake.log"
[ -f "$WORK/matcher-build/SopMatcher.so" ] \
  || { echo "FAIL: matcher plugin did not build"; tail -40 "$WORK/matcher-cmake.log"; exit 1; }
$OPT -load-pass-plugin="$WORK/matcher-build/SopMatcher.so" -passes=sop-matcher \
     -disable-output "$WORK/kernel_rw.ll" 2> "$WORK/matcher_hits.log"
# Both tools print the location of the backedge update instruction, so
# (file,line,function) is an exact join key. Verified before this was written:
# all five REWRITE lines join to a HIT at the identical line.
while IFS=, read -r _tag mfile mline mfn _rest; do
  hit_risk="$(awk -F, -v f="$mfile" -v l="$mline" -v n="$mfn" \
    '$1=="HIT" && $2==f && $3==l && $4==n {print $9}' "$WORK/matcher_hits.log")"
  if [ -z "$hit_risk" ]; then
    echo "FAIL: $mfn ($mfile:$mline) was rewritten but the matcher emits no HIT"
    echo "      for it. The pass rewrote a loop its own triage does not"
    echo "      recognise as a candidate at all."
    exit 1
  fi
  if [ "$hit_risk" != "HIGH" ]; then
    echo "FAIL: $mfn ($mfile:$mline) was rewritten but the matcher grades it"
    echo "      $hit_risk, not HIGH. Profitability analysis is no longer in"
    echo "      front of the rewrite."
    exit 1
  fi
done < <(grep '^REWRITE,' "$WORK/rewrite.log")
echo "PASS,every_rewrite_is_matcher_high"

# --- the dead original chain, and which pass actually removes it -----------
# Posture condition 6. The pass adds and redirects; it never deletes, so the
# original phi/fadd/exp chain survives, feeding only itself.
#
# ADCE specifically, not DCE, and that distinction was wrong in two files
# until 2026-08-17. The orphan is a loop-carried CYCLE — phi feeds update,
# update feeds phi — so every instruction in it has a use and plain DCE cannot
# get started. Asserted here rather than described, because "later DCE/ADCE
# may remove it" is exactly the kind of unchecked mechanism claim this repo
# keeps finding to be wrong.
DEAD_BEFORE="$(grep -c 'llvm.exp.f64' "$WORK/kernel_rw_opt.ll" || true)"
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" -passes='dce' \
     -S "$WORK/kernel_rw_opt.ll" -o "$WORK/after_dce.ll"
$OPT -load-pass-plugin="$BUILD/LogRewrite.so" -passes='adce' \
     -S "$WORK/kernel_rw_opt.ll" -o "$WORK/after_adce.ll"
DEAD_DCE="$(grep -c 'llvm.exp.f64' "$WORK/after_dce.ll" || true)"
DEAD_ADCE="$(grep -c 'llvm.exp.f64' "$WORK/after_adce.ll" || true)"
echo "INFO,dead_chain,after_rewrite=$DEAD_BEFORE,after_dce=$DEAD_DCE,after_adce=$DEAD_ADCE"
[ "$DEAD_DCE" = "$DEAD_BEFORE" ] \
  || { echo "FAIL: plain dce removed something; the documented reason ADCE is"
       echo "      required (a dead loop-carried cycle) no longer holds"; exit 1; }
# One dead llvm.exp.f64 per rewritten f64 loop. expf_widened is f32, so it is
# not counted here; derive the expectation rather than hardcoding it.
NF64="$(grep -c '^REWRITE,' "$WORK/rewrite.log" || true)"
NF32="$(grep -c '^REWRITE,.*,expf_widened_rw,' "$WORK/rewrite.log" || true)"
EXPECT=$((DEAD_BEFORE - (NF64 - NF32)))
[ "$DEAD_ADCE" = "$EXPECT" ] \
  || { echo "FAIL: adce left $DEAD_ADCE llvm.exp.f64 calls, expected $EXPECT"
       echo "      (one dead exp per rewritten f64 loop)"; exit 1; }
echo "PASS,adce_removes_the_dead_original,$DEAD_BEFORE->$DEAD_ADCE"

echo "== 4. codegen, link, run =="
$CLANG -O1 -c "$WORK/kernel_orig.ll"      -o "$WORK/kernel_orig.o"
$CLANG -O1 -c "$WORK/kernel_rw_opt.ll"    -o "$WORK/kernel_rw.o"
$CLANG -O1 -c "$WORK/kernel_prop_opt.ll"  -o "$WORK/kernel_prop.o"
$CLANG -O1 -c "$PASSDIR/test_softmax.c" -o "$WORK/main.o"
$CLANG "$WORK/main.o" "$WORK/kernel_orig.o" "$WORK/kernel_rw.o" "$WORK/kernel_prop.o" -lm \
      -o "$WORK/test_softmax"
"$WORK/test_softmax" | tee "$WORK/test.out"

grep -q '^OVERALL,PASS$' "$WORK/test.out" \
  || { echo "run_pass_test: FAIL"; exit 1; }

# == semantic coverage: every rewrite must be backed by a numeric assertion ==
#
# The set of rewritten functions is derived from the pass's own output, not
# listed here. For each one the harness must carry a PASSING check named
# cover_<fn>_*. A rewrite that nothing validates numerically fails the gate.
#
# Why this exists: before 2026-08-17 the rewritten set and the semantically
# validated set were two independently hand-maintained lists that happened to
# coincide. Nothing tied them, so a function could enter the first without
# entering the second. Demonstrated: relaxing the weight clause to admit
# constant weights produced a pass that rewrote `s += 0.5*exp(x)` while
# silently dropping the 0.5, and the entire suite stayed green.
COVERED=0
for fn in $(grep '^REWRITE,' "$WORK/rewrite.log" | cut -d, -f4 | sort -u); do
  grep -q "^PASS,cover_${fn}_" "$WORK/test.out" \
    || { echo "FAIL: $fn was rewritten but has no passing cover_${fn}_* check"
         echo "      (every rewritten function needs a numeric differential"
         echo "       in test_softmax.c; see the cover_<fn>_ convention)"; exit 1; }
  COVERED=$((COVERED + 1))
done
[ "$COVERED" -ge 5 ] \
  || { echo "FAIL: semantic coverage loop saw only $COVERED rewrites"; exit 1; }
echo "PASS,every_rewrite_semantically_covered,$COVERED"

# The weighted refusal's REASON, not just its fact: the witness inputs where
# a weight-folding state overflows while the linear loop stays healthy.
grep -q '^PASS,weight_witness_rw_matches_linear$' "$WORK/test.out" \
  || { echo "FAIL: weighted overflow witness did not run or did not hold"; exit 1; }

# == 5. the emitted code's error bound ==
#
# Links the SAME rewritten object against a search driver. Bounds get
# searched, not sampled (CONTRIBUTING.md): three stated bounds in this repo
# were refuted by exactly this method, two of them within minutes.
echo "== 5. emitted-code error bound search =="
$CLANG -O1 -c "$PASSDIR/emitted_bound_search.c" -o "$WORK/ebs.o"
$CLANG "$WORK/ebs.o" "$WORK/kernel_orig.o" "$WORK/kernel_rw.o" -lm \
      -o "$WORK/emitted_bound_search"
if "$WORK/emitted_bound_search" | tee "$WORK/bound.out"; then
  grep -q '^HELD,' "$WORK/bound.out" \
    || { echo "FAIL: search neither HELD nor REFUTED"; exit 1; }
  echo "PASS,emitted_bound_held"
else
  echo "FAIL: the emitted code's error bound was refuted"
  grep '^REFUTED,' "$WORK/bound.out" || true
  exit 1
fi

echo "run_pass_test: PASS"
