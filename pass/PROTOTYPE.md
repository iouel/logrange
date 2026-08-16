# pass/ — log-rewrite, the first rewrite prototype

*LogRange intent, step 8. Deliberately narrow: one shape, with verification.
Everything in this directory is prototype-grade; the matcher study
(matcher/RESULTS.md) is the evidence base.*

## What it does

`LogRewritePass.cpp` is an LLVM 21 new-PM `opt` plugin (pipeline name
`log-rewrite`) that recognizes exactly one loop shape: the softmax
denominator idiom, a HIGH-risk hit of the matcher study:

```c
double s = 0.0;
for (int i = 0; i < n; ++i)
    s += exp(x[i]);          /* every exp(x_i) can underflow/overflow while
                                log(sum) is comfortably representable */
```

and rewrites the accumulator to streaming logsumexp state:

| | linear original | rewritten state |
|---|---|---|
| init | `s = 0.0` | `m = -inf` (running max of `t_i`), `s = 0.0` (sum of `exp(t_i - m)`) |
| per iter | `s += exp(t)` | `newm = maxnum(m, t)`; `s = s*exp(dm) + exp(dt)`; `m = newm`; `dm = (m oeq newm) ? 0 : m - newm`, `dt` likewise |
| after loop | `s` | linear users get `exp(m + log(s))`; `m + log(s)` stored to `@__logrange_logsum` |

Matched precisely (all required, no fallbacks; misses are declines):
innermost loop, preheader + unique latch + unique exiting + unique exit
block; exactly one FP phi in the header, `double`, initialized to constant
`0.0`; backedge update a **plain `fadd(phi, X)`** (`fmuladd` out of scope);
`X` is a call to **`llvm.exp.*`** through nothing but `fpext`/`fptrunc`;
the call argument loop-varying; phi and update have no other in-loop users
(the matcher's mid-loop-read guard); at least one out-of-loop user of the
sum. One stderr line per rewrite: `REWRITE,<file>,<line>,<function>`.

The normative contract is **`pass/ELIGIBILITY.md`**. This file is the
design narrative and the measured record; where the two differ,
ELIGIBILITY.md wins.

## The maxnum-based derivation

The textbook streaming update branches:

```
if t > m:  s = s*exp(m - t) + 1.0;  m = t      # rescale to new reference
else:      s = s + exp(t - m)                  # plain accumulate
```

Both branches are instances of one formula. With `newm = max(m, t)`:

```
s_new = s*exp(m - newm) + exp(t - newm)
```

- `t > m` ⇒ `newm = t`: `s*exp(m-t) + exp(0)` = `s*exp(m-t) + 1.0`, the rescale step.
- `t ≤ m` ⇒ `newm = m`: `s*exp(0) + exp(t-m)` = `s + exp(t-m)`, the accumulate step.

Both exponents are `≤ 0`, so neither `exp` can overflow. `llvm.maxnum`
plays the role of the select, so the body stays straight-line: **no CFG
surgery inside the loop**. The planned fallback to block-splitting was
never needed. The only CFG change is one `SplitEdge` on the exit edge, for
a dedicated landing block holding the final `exp(m + log(s))` and the
export store.

First iteration works by construction: `m = -inf, s = 0` gives
`newm = maxnum(-inf, t) = t`, `s*exp(-inf) = 0`, `exp(t-t) = 1` ⇒ `s = 1`.

### The infinity guard

The raw differences `m - newm` and `t - newm` are `inf - inf = NaN` in two
reachable cases: `t = -inf` while `m` is still `-inf` (a zero term,
`exp(-inf) = 0`, ordinary input), and `t = +inf`. The emitted code
therefore computes each difference as

```
d = (x oeq newm) ? 0.0 : x - newm
```

4 extra instructions per iteration (2 `fcmp` + 2 `select`), against 2 `exp`
calls. `0.0` is the correct exponent whenever `x == newm`, finite or not, so
the guard changes no finite result: the benign relative error is unchanged
at 1.37e-15.

`oeq` is required. A NaN `t` is never equal to `newm`, so the subtract survives, `exp(NaN) = NaN`, and `s` is still
poisoned: NaN propagation is preserved, including a NaN in the first
position where `newm` is `-inf` (test g). `m` is never NaN
(`maxnum(-inf, NaN) = -inf`), so its guard only fires on genuine equality.

All terms `-inf`: `newm = -inf`, `s = k` after k iterations, and
`exp(newm + log(k)) = exp(-inf) = 0`, the linear sum exactly, with the
exported log form `-inf`. A later finite `t` rescales that state away through
`exp(-inf - t) = 0`.

NaN stickiness (verified by tests c and g): `maxnum(m, NaN) = m` ignores the NaN,
but `exp(NaN - newm) = NaN` poisons `s`, and `s` stays NaN through every
later iteration (`NaN*e1 + e2 = NaN`); the final `exp(m + log(NaN))` is
NaN, matching the linear loop's propagation.

Zero-trip exit: `m = -inf, s = 0` ⇒ `m + log(s) = -inf`, `exp(-inf) = 0`,
the correct empty sum. In practice guarded loops bypass the exit and the
outside phi picks the constant `0.0` directly, untouched.

## Opt-in stance

Reassociation legality is the **caller's grant**, never the pass's
(intent Deliverable 2: "opt-in is not a courtesy here, it is what makes
the transform legal"). Three layers, all verified:

1. The pass only runs when named in `-passes`. For this prototype that is
   the opt-in; it is registered in no default pipeline.
2. Even when named, it declines every function unless the function carries
   `"unsafe-fp-math"="true"` (what `-ffast-math` /
   `-funsafe-math-optimizations` set) **or** the pass parameter form
   `-passes='log-rewrite<force>'` was used. Verified: plain `log-rewrite`
   on the un-annotated test kernel performs 0 rewrites; the same kernel
   compiled `-ffast-math` rewrites without `force`.
3. New instructions carry **no fast-math flags**: the grant covers the
   structural reassociation performed here, not further FP relaxation of
   the emitted logsumexp code.

The test drives `force` (not `-ffast-math`) deliberately: fast-math's
`nnan` would make the NaN-propagation check meaningless.

### Exact semantics of the opt-in

`force` means *the caller explicitly grants the reassociation this
transform needs*. It waives reassociation **proof**, and nothing else. It
does not override the structural match, the FP-environment screen, the
`llvm.exp`-only errno contract, or special-value correctness. Three of
those four are checked before `force` is read at all.

`"unsafe-fp-math"="true"` is **kept** as an alternate opt-in, and is only
that: a record of a deliberate user action standing in for the grant. It is
never read as evidence that special values may be discarded. It unlocks
nothing that `force` does not, and neither unlocks anything below.

### The default-FP-environment restriction

The pass declines outright, at function granularity, before looking at any
loop, emitting `DECLINE-FPENV,<file>,<line>,<function>,<reason>`:

| reason | condition | why it is fatal |
|---|---|---|
| `strictfp` | function has the `strictfp` attribute | the rewrite changes the count and the operands of the FP operations; a dynamic rounding mode gives a different result and the exception record differs |
| `constrained-fp` | any `llvm.experimental.constrained.*` operation | per-operation rounding/exception metadata that the plain `fadd`/`fmul`/`fsub` and `llvm.exp`/`llvm.log`/`llvm.maxnum` emitted here cannot express |
| `denormal-fp-math` | `denormal-fp-math` or `denormal-fp-math-f32` is not IEEE | flush-to-zero changes *which* intermediates become zero, and the rewrite's intermediates are a different set of values entirely |

None of the three is overridable by `force`.

### Errno, and the retraction of the earlier stance

This document previously asserted that libm `errno` behaviour was
irrelevant. **That claim is withdrawn.** It was borrowed from the matcher,
where it is correct: `matcher/SumOfProductsMatcher.cpp` only recognizes
shapes, and recognition observes nothing. It does not transfer to this
pass, which *replaces* the computation.

The rewrite deletes N source `exp` evaluations and emits 2N different
exponentials plus a `log`. Every errno write and exception flag the source
loop performed is gone or different. For a conforming program with
`math_errno` in force, that is observable behaviour, so errno is a
legality question for the pass, not a shape question.

The pass therefore matches **only `llvm.exp.*`**. A direct `exp`/`expf`
call is declined with
`DECLINE-ERRNO,<file>,<line>,<function>,external-exp-call`.

This is the errno contract itself, not an arbitrary narrowing: clang emits
the intrinsic exactly when errno is already unobservable. Measured, LLVM
21, on this kernel:

```
$ clang-21 -O1 -DKERNEL -S -emit-llvm pass/test_softmax.c
  %call = tail call double @exp(double noundef %0)

$ clang-21 -O1 -fno-math-errno -DKERNEL -S -emit-llvm pass/test_softmax.c
  %1 = tail call double @llvm.exp.f64(double %0)
```

Source-level `exp`/`expf` may be accepted later, but only when IR
attributes prove the call has no observable memory or errno effect (LLVM 21
models this: an errno-writing declaration carries `memory(errnomem: write)`).
That extension point is documented in the pass and left unimplemented: it
needs its own accept and decline tests.

### Harness consequence: `-fno-math-errno` is now required

`run_pass_test.sh` compiles the kernels with `-fno-math-errno`. Without it
clang emits `call double @exp` at `-O1` and the kernel **does not match at
all**. The script asserts that directly: it compiles the same source
without the flag and requires `DECLINE-ERRNO` and zero rewrites.

The script also now pins `clang-21` alongside `opt-21`. It previously drove
unversioned `clang` into `opt-21`: on any machine with more than one clang
installed, the IR producer and consumer could differ by major version.

### Finite rounding vs. special values

Two categories, and the distinction is the safety argument:

- **Finite rounding differences: permitted and intentional.** The
  accumulation algorithm changes, so finite results are not bitwise
  identical. Measured 1.37e-15 relative on the benign case (test tolerance
  1e-12), and bounded since 2026-08-16 by
  `(n + 3k + 4 + D)*u + (|log|S|| + |log|net||)*u` (ELIGIBILITY.md, "Error contract for the
  emitted code"). This is what the reassociation grant pays for, and the only
  thing it pays for.
- **Special-value differences: forbidden.** NaN, `+inf`, `-inf`, signed
  zero and zero-trip behaviour are preserved exactly. These are not
  tolerances; a difference here is a bug. The `oeq`-guarded exponent
  differences exist for this and may not be removed.

## Status of Deliverable 2's "semantics preservation is exact" precondition

**Closed.**

Two defects stood in the way. Both are now shut:

1. **`-inf` / `+inf` produced NaN.** Closed by the `oeq`-guarded exponent
   differences. Tested against constants *and*, since the oracle fix,
   against an independent reference.
2. **errno was observable.** The larger of the two. Open until the
   `llvm.exp`-only restriction landed. The old opt-in gate
   (`"unsafe-fp-math"="true"` or `force`) conflated permission to
   reassociate with permission to change errno, exception flags, rounding
   mode, denormal handling and special values. That is not a sufficient
   contract; while it was the only gate, the precondition could not be
   claimed however good the infinity handling was.

With the restriction in place, plus the FP-environment screen, the
precondition holds **for the shape the pass matches**, under the contract in
`pass/ELIGIBILITY.md`: finite rounding may change (that is what the
reassociation grant buys); errno, exception flags, rounding mode and
denormal behaviour cannot be observed by any eligible program; and NaN,
`±inf`, signed zero and zero-trip behaviour are preserved exactly.

Scope: "exact" is claimed for special values and for observable FP
environment, not for finite bit patterns, and only for the one loop shape
in section *What it does*.

## The export hook, and why it exists

The linear replacement `exp(m + log(s))` equals the original sum in exact
arithmetic and agrees to ~1 ulp-scale relative error on benign inputs. In
the rescue regime it **re-underflows at the very last step**: for inputs
near −800, `m + log(s) ≈ −792.6` is a healthy double while `exp(−792.6)`
is 0.0. Carrying the *log form* downstream avoids that last step. Each
rewrite therefore also stores `m + log(s)` to the external global
`@__logrange_logsum`, created as an external declaration on demand: the
consuming link must define it, and the last rewrite executed wins. The test
harness defines that global and reads it after each call.

The global is no longer the only route for every shape. With
`propagate=div`, the softmax normalize divide is rewritten to `exp(t - L)`
and the rescued value reaches `out[]` directly: see
ELIGIBILITY.md section 6. That covers one consumer shape. Everything else
still exits through the global, and last-rewrite-wins remains its stated
defect.

## Consumer-shape logging spike (section 6 reconnaissance)

The first stretch-goal spike now records, but does not rewrite, the IR shape
of consumers reached from a rewritten denominator sum after LCSSA/exit-edge
plumbing:

- the full-softmax kernel at `-O1 -fno-math-errno` reaches an LCSSA phi and
  then an `fdiv` with the sum in operand 1, logged as
  `CONSUMER-MATCH,...,fdiv-of-sum`;
- the `exp(x[i]) + s` near miss logs `CONSUMER-DECLINE,...,not-fdiv`;
- the `s / exp(x[i])` near miss logs
  `CONSUMER-DECLINE,...,not-the-sum`;
- the standalone denominator still logs its return consumer as
  `CONSUMER-DECLINE,...,not-fdiv`.

Without `propagate=div` this is measurement only: the pass rewires every
out-of-loop use to the linear replacement plus the export hook, and logs the
consumer shape it saw. With `propagate=div`, a consumer classified
`fdiv-of-sum` is rewritten to `exp(t - L)`; everything else is declined with
a reason token. Contract in ELIGIBILITY.md section 6.

## Verified results

`bash pass/run_pass_test.sh` inside WSL (clang/opt 21, Ubuntu). Build
state lives in `~/logrange-pass` (WSL `/tmp` does not persist across
`wsl.exe` invocations). Two copies of the same kernel TU are linked under
different names via preprocessor renaming (`-Dsoftmax_denom=..._orig` /
`..._rw`), chosen over `objcopy --redefine-sym`: same effect, no binary
surgery, visible in the build commands. Output, verbatim:

```
== 1. build plugin ==
plugin: ~/logrange-pass/build/LogRewrite.so
== 2. compile kernels to IR (identical source, two names) ==
== 3. rewrite (force=1: the explicit reassociation grant) ==
REWRITE,pass/test_softmax.c,29,softmax_denom_rw,HIGH,exp-chain;exp-sum
CONSUMER-DECLINE,pass/test_softmax.c,30,softmax_denom_rw,not-fdiv
REWRITE,pass/test_softmax.c,38,softmax_full_rw,HIGH,exp-chain;exp-sum
CONSUMER-MATCH,pass/test_softmax.c,40,softmax_full_rw,fdiv-of-sum
REWRITE,pass/test_softmax.c,47,softmax_add_rw,HIGH,exp-chain;exp-sum
CONSUMER-DECLINE,pass/test_softmax.c,49,softmax_add_rw,not-fdiv
REWRITE,pass/test_softmax.c,57,softmax_sum_div_rw,HIGH,exp-chain;exp-sum
CONSUMER-DECLINE,pass/test_softmax.c,59,softmax_sum_div_rw,not-the-sum
PASS,gate_declines_above_threshold
PASS,unknown_parameter_refused
== 3b. safety declines (force must NOT override any of these) ==
PASS,decline_external_exp_call
PASS,decline_strictfp_under_force
PASS,decline_constrained_fp_under_force
PASS,decline_denormal_env_under_force
== 3c. propagate=div: compile a _prop copy, run with propagate=div ==
REWRITE,pass/test_softmax.c,29,softmax_denom_prop,HIGH,exp-chain;exp-sum
DECLINE-PROP,pass/test_softmax.c,30,softmax_denom_prop,not-fdiv
REWRITE,pass/test_softmax.c,38,softmax_full_prop,HIGH,exp-chain;exp-sum
PROPAGATE,pass/test_softmax.c,40,softmax_full_prop
REWRITE,pass/test_softmax.c,47,softmax_add_prop,HIGH,exp-chain;exp-sum
DECLINE-PROP,pass/test_softmax.c,49,softmax_add_prop,not-fdiv
REWRITE,pass/test_softmax.c,57,softmax_sum_div_prop,HIGH,exp-chain;exp-sum
DECLINE-PROP,pass/test_softmax.c,59,softmax_sum_div_prop,not-the-sum
PASS,propagate_div_rewrites_softmax_full
PASS,unknown_propagate_refused
PASS,force_alone_does_not_propagate
== 4. codegen, link, run ==
INFO,benign,orig=1654.7821267630925,rw=1654.7821267630948,rel=1.37e-15,logsum=7.4114246336847733,logref=7.4114246336847733
PASS,benign_linear_agree_1e-12
PASS,benign_exported_logsum
INFO,rescue,orig=0,rw_linear=0,logsum=-792.61968910109306,logref=-792.61968910109306
PASS,rescue_orig_underflows_to_zero
PASS,rescue_exported_logsum_finite
PASS,rescue_exported_logsum_correct
INFO,rescue,rw_linear_underflows_too=1 (expected 1)
INFO,nan,orig=nan,rw=nan,logsum=nan,logref=nan
PASS,nan_orig_propagates
PASS,nan_rw_propagates
PASS,nan_exported_logsum_propagates
PASS,nan_matches_reference
INFO,neginf,orig=1645.583106855303,rw=1645.5831068553036,rel=4.15e-16,logsum=7.4058500726414902,logref=7.4058500726414902
PASS,neginf_rw_finite
PASS,neginf_linear_agree_1e-12
PASS,neginf_exported_logsum
INFO,allneginf,orig=0,rw=0,logsum=-inf,logref=-inf
PASS,allneginf_orig_zero
PASS,allneginf_rw_zero
PASS,allneginf_exported_logsum_neginf
PASS,allneginf_matches_reference
INFO,nan_first,orig=nan,rw=nan,logsum=nan,logref=nan
PASS,nan_first_orig_propagates
PASS,nan_first_rw_propagates
PASS,nan_first_exported_logsum_propagates
PASS,nan_first_matches_reference
INFO,posinf,orig=inf,rw=inf,logsum=inf,logref=inf
PASS,posinf_orig_is_inf
PASS,posinf_rw_is_inf
PASS,posinf_exported_logsum_is_inf
PASS,posinf_matches_reference
INFO,nan_with_infs,orig=nan,rw=nan,logsum=nan,logref=nan
PASS,nan_with_infs_orig_propagates
PASS,nan_with_infs_rw_propagates
PASS,nan_with_infs_matches_reference
INFO,zerotrip,orig=0,rw=0,logsum=12345,logref=-inf
PASS,zerotrip_orig_zero
PASS,zerotrip_rw_zero
PASS,zerotrip_bitwise_identical
PASS,zerotrip_export_not_written
PASS,zerotrip_reference_neginf
PASS,negctl_plain_sum_untouched
PASS,negctl_dot_sum_untouched
PASS,negctl_invariant_exp_untouched
INFO,softmax_full,max_rel=1.64e-15
PASS,softmax_full_rw_agree_1e-12
INFO,softmax_add,max_rel=1.6e-15
PASS,softmax_add_rw_agree_1e-12
INFO,softmax_sum_div,max_rel=1.65e-15
PASS,softmax_sum_div_rw_agree_1e-12
INFO,prop_benign,linear=2.26e-16,reconvert=1.63e-15,propagated=2.42e-15
PASS,prop_benign_agrees_1e-12
INFO,prop_ranking,trials=64,prop_wins=0,ratio_min=1.33,ratio_max=13.92,ref_logL_err=1.7e-18
INFO,prop_rescue,linear[0]=-nan,logref=-792.55102758943792,sum=0.99999999999996403
PASS,prop_rescue_linear_is_broken
PASS,prop_rescue_all_finite
PASS,prop_rescue_sums_to_one
OVERALL,PASS
run_pass_test: PASS
```

- **Benign** (n=1000, ~N(0,1)): linear results agree to 1.4e-15 relative
  (bound: 1e-12); exported logsum matches an independent max-shift
  reference.
- **Rescue** (values ≈ −800): the original sum is exactly 0.0; the exported
  log form is −792.6197…, matching the reference to 1e-12 relative. The rw
  *linear* value is also 0.0.
- **NaN**: one NaN input poisons original, rewritten, and exported values,
  in the first position (`m` still `-inf`) as well as mid-stream.
- **`-inf`** (4 of 1000 terms, including `x[0]` and `x[999]`): linear results
  agree to 4.15e-16 relative, exported logsum matches the reference. All 1000
  terms `-inf`: both sums exactly 0.0, exported logsum `-inf`.
- **`+inf`**: both sums `+inf`, exported logsum `+inf`.
- **NaN mixed with infinities**: NaN wins, in kernel and reference alike.
- **Zero trip count** (`n = 0`): both sums exactly `0.0` and bit-identical;
  the export hook is *not* written (the loop guard bypasses the rewritten
  exit block entirely). The harness asserts this against a sentinel.
- **Safety declines** (section 3b, all under `force`): external `exp` call,
  `strictfp`, constrained ops with `strictfp` stripped, and
  `-fdenormal-fp-math=preserve-sign` each produce 0 rewrites and the
  expected reason token.
- **Negative controls** (same module): plain sum, dot product, and
  loop-invariant-`exp` sum are declined. The script asserts *exactly one*
  `REWRITE` line; the harness checks their orig/rw results bit-identical.

### The reference oracle was wrong on infinities, and is now fixed

`ref_logsumexp()` in `test_softmax.c` did the max-shift unconditionally.
The shift is exactly what breaks on infinities: `x_i - M` is `inf - inf`
whenever `M` is infinite and `x_i == M`. Measured, old version:

```
old_ref all_neg_inf  = -nan   (linear loop gives log(0) = -inf)
old_ref pos_inf      = -nan   (linear loop gives log(inf) = inf)
old_ref nan_with_inf = -nan   (linear loop gives nan)
old_ref zero_trip    = -inf   (correct)
```

So the infinity tests added earlier were asserting against constants only,
**not** reference-validated. The reference now handles NaN, `+inf` and
all-`-inf` before the shift, in that order (NaN beats `+inf`, because the
linear loop adds a NaN term whatever else it has added). The constant-based
assertions are kept; the infinity cases now check both.

The first working version passed every test while never wiring in the
linear replacement. `SplitEdge`'s LCSSA preservation inserts pass-through
phis that stale-ify user lists snapshotted before the split, and all three
original scenarios produce identical linear values either way. The tell was
the benign case's relative error being *exactly* 0 (bitwise-identical
results). Fixed by redirecting users strictly after the split. The script
now structurally asserts the replacement value is consumed, and the benign
case shows 1.37e-15.

## The emitted code's error bound

Stated normatively in ELIGIBILITY.md. This is how it was arrived at and what
the search found.

**Derivation is inheritance, and that had to be checked rather than assumed.**
Term for term the emitted state machine performs `pos_accum`'s arithmetic.
The guarded form looks like it does strictly more work: `s*exp(dm)` runs on
every iteration where `pos_accum` multiplies only on a rescale. It costs
nothing, because `t <= m` makes `dm` exactly `0`, `exp(0)` is exactly `1.0`,
and `s*1.0` is exact. So the accumulate step rounds once, as the runtime's
does. That makes two IEEE exactness facts load-bearing for the emitted code
that the runtime never leans on, and `emitted_bound_search.c` asserts both at
startup instead of trusting them. Under a merely-1-ulp `exp(0)` the branchless
form would pay `n*u` that `pos_accum` does not.

What the emitted code adds is the final `exp(m + log(s))`, one rounding:
`(n + 3k + 3 + D)*u` becomes `(n + 3k + 4 + D)*u`, reduction terms unchanged.
Both reduction terms are inherited, including the `|log|net||*u` added on
2026-08-16 when it refuted `rp_accum` at 1.99x; it does not bind here because
`|log|net|| <= log n` and the `n*u` term dominates.

**What the search found.** 7285 trials, worst observed/bound 0.99.

- The binding family is a large cluster one depth below a dominant term. Every
  add of the running sum rounds the same direction, so the observed error is
  essentially the classical `(n-1)u` of uncompensated summation against a
  bound charging `n*u`. The ratio approaches 1 from below and *stops climbing*
  past N=2048: refinement at 4096, 8192 and 16384 terms all report 0.990. That
  plateau is the mechanism confirming itself, not the search running out of
  ideas.
- The expected refutation path did not materialize. `3k*u` charges nothing for
  the *size* of a reference jump, and each rescale forms `exp(m - t)` whose
  argument rounds to `u*J` for a jump of `J` log-units — the same omission
  that refuted `rp_accum`. Ascending families reach only 0.23. The reason is
  mass: after a jump of `J` the old sum's share of the total is `s/(s + e^J)`,
  so the surviving error is `J*s/(s + e^J)*u`, maximized near `J ~ ln n` and
  worth a fraction of a `u` against a `3u` per-rescale budget.
- **The reduction term is required here by measurement, not by analogy.**
  Every run also scores the form without it. It is exceeded on 321 of the 7285
  trials, worst 39x.

**One limit is derived rather than searched.** Recursive summation's classical
`(n-1)u/(1-(n-1)u)` exceeds the `(n+4)u` charged here once `(n-1)(n+4)u > 5`,
i.e. `n > ~2.1e8`. The search reaches n=16385, four orders below that. This is
a first-order bound and that is the `n` at which "first-order" begins to bite.

**What this does not close.** The bound covers the one shape the pass rewrites.
`propagate=div` moves the log form into a consumer and carries its own error,
measured at 1.33x-13.9x behind linear re-conversion at one conversion and
unmeasured over chains; nothing here bounds a chain.

## Limitations

- **Single shape.** Plain `fadd` + `llvm.exp` only: no `fmuladd`, no
  `sum += w[i]*exp(t)` (the mixture-likelihood shape), no float
  accumulators, no nonzero initial value, no multi-exit loops, only one FP
  phi per loop, update must dominate the exit branch (rotated loops).
  Everything else is declined, by design.
- **`-fno-math-errno` required at the call site.** Source-level `exp`/`expf`
  is declined outright, so a kernel compiled without the flag is not
  eligible at all. This is the errno contract, not a defect. It does mean
  the pass covers a strictly narrower set of real translation units than the
  matcher reports hits in. The attribute-proved extension point is the way
  out, and is unimplemented.
- **Default FP environment required.** `strictfp`, any constrained-FP
  operation, and any non-IEEE denormal mode are declined at function
  granularity. No mechanism, including `force`, overrides this.
- **Downstream propagation covers one consumer shape.** `propagate=div`
  rewrites `fdiv(llvm.exp(t), sum)` to `exp(t - L)`, so the softmax normalize
  divide reaches an observable result without the export hook. Every other
  consumer still takes the linear replacement, which re-underflows exactly
  when the rescue matters, leaving the export hook as the only route. The
  remaining vocabulary (`fmul` → `fadd`, `fadd` → logsumexp) is unimplemented,
  so no log region larger than a single divide can form.
- **The export hook is a prototype.** One process-global external symbol,
  last-rewrite-wins, and any module the pass rewrites must be linked
  against something defining `__logrange_logsum`.
- **Profitability gating is wired, and cannot decline anything yet.** The
  pass computes the same risk verdict the matcher does and refuses to rewrite
  below `min-risk` (default HIGH), logging
  `REWRITE,<file>,<line>,<fn>,HIGH,exp-chain;exp-sum` or
  `DECLINE-RISK,...,below-min-<tier>`. The one shape it matches requires
  an `exp` call, so the verdict is HIGH by construction and no reachable
  input is below the threshold. Both branches are tested (`min-risk=none`
  exercises the refusal path). It starts doing useful work only when shape
  coverage widens to the `fmuladd` and `w[i]*exp(t)` forms, which the
  matcher already grades MED and LOW.
- **The dead original is left in place.** The pass adds and redirects; it
  does not delete. The orphaned `phi`/`fadd`/`exp` chain feeds only itself
  and is left for later DCE/ADCE. Harmless: it computes the original
  0.0/NaN alongside.
- ~~**Accuracy is measured, not bounded.**~~ Closed 2026-08-16. The emitted
  code now carries `(n + 3k + 4 + D)*u + (|log|S|| + |log|net||)*u`, normative in
  ELIGIBILITY.md and searched by `emitted_bound_search.c` on every gate run.
  See below.

## Files

| file | role |
|---|---|
| `ELIGIBILITY.md` | **normative** contract: requirements and guarantees |
| `emitted_bound_search.c` | adversarial search against the emitted code's error bound |
| `LogRewritePass.cpp` | the plugin (`log-rewrite`, param `<force>`) |
| `CMakeLists.txt` | standalone plugin build, same pattern as `matcher/` |
| `test_softmax.c` | one-file kernel + harness, compiled three ways |
| `run_pass_test.sh` | full build → rewrite → link → run; ends `PASS` |
| `PROTOTYPE.md` | this file: design narrative and measured record |
