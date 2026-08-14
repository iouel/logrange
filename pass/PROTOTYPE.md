# pass/ — log-rewrite, the first rewrite prototype

*LogRange intent, step 8 — deliberately narrow: one shape, done honestly,
with verification. Everything in this directory is prototype-grade; the
matcher study (matcher/RESULTS.md) is the evidence base it stands on.*

## What it does

`LogRewritePass.cpp` is an LLVM 21 new-PM `opt` plugin (pipeline name
`log-rewrite`) that recognizes exactly one loop shape — the softmax
denominator idiom, the marquee hit of the matcher study:

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
| per iter | `s += exp(t)` | `newm = maxnum(m, t)`; `s = s*exp(m - newm) + exp(t - newm)`; `m = newm` |
| after loop | `s` | linear users get `exp(m + log(s))`; `m + log(s)` stored to `@__logrange_logsum` |

Matched precisely (all required, no fallbacks — misses are declines):
innermost loop, preheader + unique latch + unique exiting + unique exit
block; exactly one FP phi in the header, `double`, initialized to constant
`0.0`; backedge update a **plain `fadd(phi, X)`** (`fmuladd` out of scope);
`X` is a call to `exp`/`expf`/`llvm.exp` through nothing but
`fpext`/`fptrunc`; the call argument loop-varying; phi and update have no
other in-loop users (the matcher's mid-loop-read guard); at least one
out-of-loop user of the sum. One stderr line per rewrite:
`REWRITE,<file>,<line>,<function>`.

## The select-based (actually: maxnum-based) derivation

The textbook streaming update branches:

```
if t > m:  s = s*exp(m - t) + 1.0;  m = t      # rescale to new reference
else:      s = s + exp(t - m)                  # plain accumulate
```

Both branches are instances of one formula. With `newm = max(m, t)`:

```
s_new = s*exp(m - newm) + exp(t - newm)
```

- `t > m` ⇒ `newm = t`: `s*exp(m-t) + exp(0)` = `s*exp(m-t) + 1.0` — the rescale step.
- `t ≤ m` ⇒ `newm = m`: `s*exp(0) + exp(t-m)` = `s + exp(t-m)` — the accumulate step.

Both exponents are `≤ 0`, so neither `exp` can overflow. `llvm.maxnum`
plays the role of the select, so the body stays straight-line — **no CFG
surgery inside the loop** (the planned fallback to block-splitting was
never needed). The only CFG change is one `SplitEdge` on the exit edge, to
get a dedicated landing block for the final `exp(m + log(s))` and the
export store.

First iteration works by construction: `m = -inf, s = 0` gives
`newm = maxnum(-inf, t) = t`, `s*exp(-inf) = 0`, `exp(t-t) = 1` ⇒ `s = 1`.

NaN stickiness (verified by test c): `maxnum(m, NaN) = m` ignores the NaN,
but `exp(NaN - newm) = NaN` poisons `s`, and `s` stays NaN through every
later iteration (`NaN*e1 + e2 = NaN`); the final `exp(m + log(NaN))` is
NaN — matching the linear loop's propagation.

Zero-trip exit: `m = -inf, s = 0` ⇒ `m + log(s) = -inf`, `exp(-inf) = 0` —
the correct empty sum (in practice guarded loops bypass the exit and the
outside phi picks the constant `0.0` directly, untouched).

## Opt-in stance

Reassociation legality is the **caller's grant**, never the pass's
(intent Deliverable 2: "opt-in is not a courtesy here, it is what makes
the transform legal"). Three layers, all verified:

1. The pass only runs when named in `-passes` — for this prototype, that
   *is* the opt-in; it is registered in no default pipeline.
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

## The export hook, and why it exists

The linear replacement `exp(m + log(s))` equals the original sum in exact
arithmetic and agrees to ~1 ulp-scale relative error on benign inputs. But
in the rescue regime it **re-underflows at the very last step**: for
inputs near −800, `m + log(s) ≈ −792.6` is a perfectly healthy double
while `exp(−792.6)` is 0.0. The real win requires propagating the *log
form* downstream (e.g. into the softmax divide, which becomes a subtract).
That is future work; this prototype makes the log form observable through
a documented hook: each rewrite stores `m + log(s)` to the external global
`@__logrange_logsum` (created as an external declaration on demand — the
consuming link must define it; last rewrite executed wins). The test
harness defines that global and reads it after each call.

## Verified results

`bash pass/run_pass_test.sh` inside WSL (clang/opt 21, Ubuntu). Build
state lives in `~/logrange-pass` (WSL `/tmp` does not persist across
`wsl.exe` invocations). Two copies of the same kernel TU are linked under
different names via preprocessor renaming (`-Dsoftmax_denom=..._orig` /
`..._rw` — chosen over `objcopy --redefine-sym`: same effect, no binary
surgery, visible in the build commands). Output, verbatim:

```
== 1. build plugin ==
plugin: ~/logrange-pass/build/LogRewrite.so
== 2. compile kernels to IR (identical source, two names) ==
== 3. rewrite (force=1: the explicit reassociation grant) ==
REWRITE,test_softmax.c,29,softmax_denom_rw
== 4. codegen, link, run ==
INFO,benign,orig=1654.7821267630925,rw=1654.7821267630948,rel=1.37e-15,logsum=7.4114246336847733,logref=7.4114246336847733
PASS,benign_linear_agree_1e-12
PASS,benign_exported_logsum
INFO,rescue,orig=0,rw_linear=0,logsum=-792.61968910109306,logref=-792.61968910109306
PASS,rescue_orig_underflows_to_zero
PASS,rescue_exported_logsum_finite
PASS,rescue_exported_logsum_correct
INFO,rescue,rw_linear_underflows_too=1 (expected 1)
INFO,nan,orig=nan,rw=nan,logsum=nan
PASS,nan_orig_propagates
PASS,nan_rw_propagates
PASS,nan_exported_logsum_propagates
PASS,negctl_plain_sum_untouched
PASS,negctl_dot_sum_untouched
PASS,negctl_invariant_exp_untouched
OVERALL,PASS
run_pass_test: PASS
```

- **Benign** (n=1000, ~N(0,1)): linear results agree to 1.4e-15 relative
  (bound: 1e-12); exported logsum matches an independent max-shift
  reference.
- **Rescue** (values ≈ −800): the original sum is exactly 0.0 — the silent
  failure this project exists to repair — while the exported log form is
  −792.6197…, matching the reference to 1e-12 relative. The rw *linear*
  value is also 0.0, as predicted above.
- **NaN**: one NaN input poisons original, rewritten, and exported values.
- **Negative controls** (same module): plain sum, dot product, and
  loop-invariant-`exp` sum are declined — the script asserts *exactly one*
  `REWRITE` line and the harness checks their orig/rw results bit-identical.

A war story worth keeping: the first working version passed every test
while silently never wiring in the linear replacement — `SplitEdge`'s
LCSSA preservation inserts pass-through phis that stale-ify user lists
snapshotted before the split, and all three original scenarios happen to
produce identical linear values either way. The tell was the benign case's
relative error being *exactly* 0 (bitwise-identical results). Fixed by
redirecting users strictly after the split; the script now structurally
asserts the replacement value is consumed, and the benign case shows an
honest 1.37e-15.

## Limitations (honest list)

- **Single shape.** Plain `fadd` + direct `exp` only: no `fmuladd`, no
  `sum += w[i]*exp(t)` (the mixture-likelihood shape), no float
  accumulators, no nonzero initial value, no multi-exit loops, only one FP
  phi per loop, update must dominate the exit branch (rotated loops).
  Everything else is declined, by design.
- **No downstream log propagation.** The linear replacement re-underflows
  exactly when the rescue matters; the win is only observable through the
  export hook. Propagating `m + log(s)` into downstream users (softmax's
  divide → subtract) is the actual product and is future work.
- **The export hook is a prototype.** One process-global external symbol,
  last-rewrite-wins, and any module the pass rewrites must be linked
  against something defining `__logrange_logsum`.
- **`-inf` inputs produce NaN.** `t = -inf` while `m = -inf` gives
  `m - newm = -inf - -inf = NaN` in the rescale factor. The linear
  original would compute `exp(-inf) = 0` and carry on. A matcher-side
  guard (skip loops whose input can be −inf) or an explicit zero-guard in
  the emitted code would fix it; documented rather than solved here.
- **No profitability gating yet.** The matcher study's triage
  (matcher/RESULTS.md) found the abundant hits are mostly benign-range dot
  products; the rescue-worthy transcendental subset is small. This
  prototype fires on *every* matching shape once legality is granted —
  the HIGH/MED/LOW risk signal from the study is the intended gate and is
  not yet wired in.
- **The dead original is left in place.** The pass adds and redirects; it
  does not delete. The orphaned `phi`/`fadd`/`exp` chain feeds only itself
  and is left for later DCE/ADCE (harmless: it computes the original 0.0/
  NaN quietly alongside).
- **Accuracy is measured, not bounded.** The runtime header ships formal
  error bounds (intent step 6); the emitted streaming state has the same
  structure as `pos_accum`+rescale but no stated bound of its own yet.
  Observed: 1.4e-15 relative on the benign case.

## Files

| file | role |
|---|---|
| `LogRewritePass.cpp` | the plugin (`log-rewrite`, param `<force>`) |
| `CMakeLists.txt` | standalone plugin build, same pattern as `matcher/` |
| `test_softmax.c` | one-file kernel + harness, compiled three ways |
| `run_pass_test.sh` | full build → rewrite → link → run; ends `PASS` |
