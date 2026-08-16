# ELIGIBILITY — what `log-rewrite` requires, and what it guarantees

*Normative. `pass/LogRewritePass.cpp` implements this contract;
`pass/run_pass_test.sh` tests it. Where this file and PROTOTYPE.md differ,
this file wins.*

A loop is eligible only if **every** clause below holds. There are no
fallbacks and no partial credit: a failed clause is a decline.

## 1. Explicit LogRange opt-in is required

Two independent grants, both required:

1. The pass runs only when named in `-passes`. It is registered in no
   default pipeline.
2. The function must additionally carry either the pass parameter `force`
   (`-passes='log-rewrite<force>'`) or the function attribute
   `"unsafe-fp-math"="true"`.

Reassociation permission is the caller's to give. The pass never
self-authorizes it.

### `"unsafe-fp-math"="true"` is an opt-in, and only an opt-in

It is retained as an alternate grant because it records a deliberate user
action: `-ffast-math` / `-funsafe-math-optimizations`.

It is **never** read as evidence that special values may be discarded.
Every clause in sections 2–4 applies identically on both grant paths. The
attribute unlocks nothing beyond the reassociation grant itself.

Caveat: under `-ffast-math` the surrounding IR
already carries `nnan`/`ninf` on operations this pass did not create. The
special-value guarantees in section 5 are preservation *relative to the
identically-flagged linear loop*, not an absolute guarantee about a program
that told the compiler NaNs do not occur.

### What `force` means

`force` means: *the caller explicitly grants the reassociation this
transform needs.*

It waives reassociation **proof**. It waives nothing else. `force` does not
override, and is not consulted by:

- the structural match conditions (section 3);
- the FP-environment requirements (section 2);
- the `llvm.exp`-only errno contract (section 4);
- special-value correctness (section 5).

`force` is a request for the transform, **not** a bypass for structural or
FP-environment safety. Three of the four conditions above are checked
before `force` is read at all.

## 2. The ordinary, default LLVM FP environment is required

Declined outright, at function granularity, before any loop is examined.
Each emits `DECLINE-FPENV,<file>,<line>,<function>,<reason>`:

| reason token | rejected when |
|---|---|
| `strictfp` | the function carries the `strictfp` attribute |
| `constrained-fp` | the function contains any `llvm.experimental.constrained.*` operation |
| `denormal-fp-math` | `denormal-fp-math` or `denormal-fp-math-f32` resolves to anything other than IEEE |

Why each is fatal rather than merely risky:

- **strictfp / constrained**: the rewrite changes both the number and the
  operands of the FP operations. A dynamic rounding mode gives a different
  result, and the exception record differs. Constrained intrinsics carry
  per-operation rounding and exception metadata that the plain
  `fadd`/`fmul`/`fsub` and `llvm.exp`/`llvm.log`/`llvm.maxnum` emitted here
  cannot express.
- **denormal**: a non-IEEE denormal mode (flush-to-zero, preserve-sign)
  changes *which* intermediate values become zero. The rewrite's
  intermediates are `exp()` of differences against a running maximum, a
  different set of values entirely from the source loop's `exp(x_i)`, so
  which of them flush is not preservable.

## 3. Structural match

Innermost loop with preheader, unique latch, unique exiting block, unique
exit block; exactly one FP phi in the header, of type `double`, initialized
to constant `0.0` from the preheader; the backedge update one of the three
spines in 3.1; `X` an accepted `exp` call through nothing but
`fpext`/`fptrunc`; the call argument loop-varying; phi and update with no
other in-loop users; at least one out-of-loop user of the sum; the update
dominating the exit branch.

### 3.1 Accepted spines, and which of them is rewritten

**Matched is not rewritten.** Three spines are matched; one is rewritten.

| spine | matched | rewritten |
|---|---|---|
| `fadd(phi, X)` | yes | **yes** |
| `fadd(phi, fmul(W, X))` | yes | no — declined by 3.3 |
| `llvm.fmuladd(W, X, phi)` | yes | no — declined by 3.3 |

The weighted spines are matched **in order to be declined with a stated
reason**. A shape the pass cannot rewrite and says nothing about is
indistinguishable, to a reader, from a shape it failed to recognize.

Both weighted forms are required because which one clang emits is
`-ffp-contract`, a flag the pass does not control. Measured on
`pass/test_softmax.c`, LLVM 21: `=on` (the default) emits `llvm.fmuladd`;
`=off` and `=fast` emit `fmul` + `fadd`. `run_pass_test.sh` compiles the
kernel at all three and asserts the same decline from each, and asserts the
uncontracted builds contain no `llvm.fmuladd` so the check cannot go vacuous.

In the `fmuladd` and `fmul` forms the accumulator must be the **addend**. As
a multiplicand it is a product recurrence, not a sum. The `fmul` is a spine
node and takes the same clean-use discipline as the update: its only in-loop
user must be the add.

Of a product's two operands, `X` is the one reaching an `exp` and `W` is the
other. With no `exp` on either side the split is arbitrary and unused: that
verdict is LOW and 3.2 declines first.

### 3.2 No multiply and no `exp` is not a match

A backedge update with neither a multiply nor an `exp` in its term is **not
matched at all**, and is silent — not a decline. This is the matcher's
`noMulNoExp` rule, held to so the two tools agree on what is in scope: such a
reduction is rescuable without logsumexp. `plain_sum` in
`pass/test_softmax.c` is the named case on both sides.

### 3.3 A weighted term is declined

`DECLINE-WEIGHT,<file>,<line>,<function>,unbounded-weight`.

Folding `w_i` into the term makes the emitted state accumulate
`sum(w_i * exp(t_i - m))` rather than `sum(exp(t_i - m))`, and `w_i` is not
proven bounded. Every scaled term is at most `|w_i|`, so the state reaches
`sum|w_i|`, which has no ceiling — where the unweighted state is at most `n`
by construction, the exponents being `<= 0`. Measured on the emitted state
machine at `n = 2`: `w = (1e308, 1e308)`, `t = (-700, -700)` drives the state
to `inf` and the result to `inf`; the linear loop gives `19719.4`. No
reduction recovers a state that has already overflowed.

A second failure exists and is **not** the reason for this clause. The
emitted reduction is `exp(m + log(s))`, so `w = (1, -2)`, `t = (0, 0)` drives
the state to `-1` and its log to NaN where the linear loop gives `-1`. That
is a property of the reduction, not of the state, and it is fixable:
`copysign(exp(m + log|s|), s)` is correct for a negative sum. Recorded as
work a weighted rewrite would have to do.

**The clause is structural, and the token overstates it.** The check is
`if (Weight)` — presence, not magnitude. A provably positive, provably
bounded weight declines identically: `s += 0.5*exp(x[i])` logs
`unbounded-weight`, and `0.5` is not unbounded. The token is retained because
the durable reason for refusing the *general* case is magnitude, but a reader
should not infer that a magnitude analysis ran.

**What a leak would actually cost is worse than overflow.** No code reads
`Weight` after this clause, so a weighted spine admitted here is rewritten
with the weight **silently discarded** — the emitted state accumulates
`sum(exp(t_i - m))` and the result is wrong by a factor of `w` at every
magnitude, not merely at extreme ones. `const_weight_sum` in
`pass/test_softmax.c` exists for exactly this: relaxing the clause to
`Weight && !isa<ConstantFP>(Weight)` previously left the whole suite green
while the pass miscompiled `s += 0.5*exp(x)`.

Clause order is **normative**: the section 7 risk gate runs first, this
clause second. Reversed, this clause shadows the gate and the only
LOW-verdict input the pass matches never reaches it.

**Extension point, deliberately not taken.** A weight proven bounded — a
constant is the easy case — is rewritable. It needs the sign handling above,
the section 5 error contract re-derived (it is `pos_accum`'s, for unit
weights), and its own accept and decline tests. Measured yield before
building any of it: **0** `w*exp(t)` sites in 2859 corpus loops
(`matcher/run_study.sh weights`).

## 4. Source-level `exp`/`expf` is not accepted

Only `llvm.exp.*` is accepted. A direct call to `exp`/`expf` is declined
with `DECLINE-ERRNO,<file>,<line>,<function>,external-exp-call`.

This is the errno contract, not a shape preference. The rewrite deletes N
source `exp` evaluations and emits 2N different exponentials plus a `log`,
so every errno write and FP-exception flag the source loop performed is
gone or different. For a conforming program with `math_errno` in force,
that is observable behaviour.

`llvm.exp` is exactly the marker that errno is already unobservable. Clang
emits it only when errno cannot be read. Measured, LLVM 21, on
`pass/test_softmax.c`: at `-O1` the source `s += exp(x[i])` emits
`call double @exp`; adding `-fno-math-errno` emits `llvm.exp.f64`.
Restricting to the intrinsic therefore *is* the errno contract.

Consequence for callers: the kernel must be compiled `-fno-math-errno` (or
`-ffast-math`) to be eligible at all.

**Extension point, deliberately not taken.** A direct external `exp`/`expf`
call MAY be accepted once the IR itself proves the call has no observable
memory or errno effect: the call site's memory effects excluding writes to
errno memory and to inaccessible memory. LLVM 21 models this explicitly: an
errno-writing `exp` declaration carries `memory(errnomem: write)`.
Implementing it requires its own accept and decline tests. Until those
exist, external calls are declined unconditionally.

## 5. What the transform preserves, and what it does not

### Finite rounding MAY change — intentionally

The accumulation algorithm changes. `s += exp(x_i)` becomes streaming
logsumexp with rescaling; the operations, their order, and their operands
all differ. Finite results are therefore not bitwise identical.

This is the one thing the reassociation grant in section 1 pays for. It is
the only thing it pays for. How much it may change is bounded:

### Error contract for the emitted code

For a rewritten `s += exp(x_i)` loop whose exact sum `S` is a normal double:

    WORST-CASE RELATIVE ERROR  <=  (n + 3k + 4 + D) * u
                                   +  (|log|S|| + |log|net||) * u

`u`, `k` and `D` are the runtime's, defined at `rp_accum` in
`include/logrange/log_math.h`: `k` counts adds that strictly raise the
running reference, `D` is the mass-weighted mean insertion depth. `n` is the
trip count. The exported log form `__logrange_logsum` satisfies the same
bound as an absolute error in log space, which is the same statement one
`exp` earlier.

`net = S / exp(m)` is the scaled sum the reduction takes the log of;
`|log|net|| <= log n` here, since every scaled term is at most 1. That term
was added 2026-08-16 after it refuted `rp_accum`'s contract at 1.99x: the
reduction's two addends both round at their own magnitudes, and `|log|S||`
alone charges only the addition's result. It does not bind for this code, for
the same reason it does not bind for `pos_accum` — the `n*u` term dominates
`log n` — and it is carried so the statement is correct rather than merely
unfalsified. Family E in `emitted_bound_search.c` measures it.

This is `pos_accum`'s `(n + 3k + 3 + D)*u + (|log|S|| + |log|net||)*u` plus `1u` for the
final `exp(m + log(s))` that the runtime does not perform. Term for term the
emitted arithmetic is `pos_accum`'s: under the `oeq` guard, `t <= m` gives
`dm` exactly `0`, `exp(0)` is exactly `1.0`, and `s*1.0` is exact, so the
accumulate step rounds once. **Those two exactness facts are load-bearing
here and are asserted at startup by the search**; a merely-1-ulp `exp(0)`
would add `n*u` the runtime never pays.

Three scope conditions, all of them real:

- **The result must be a normal double.** The emitted code returns a linear
  value, so `|log|S||` cannot exceed 709.78 and the reduction term is capped
  near `710u`. The runtime's `log_value` form has no such ceiling. Outside
  the normal range this contract says nothing; that is overflow, not
  accumulation error.
- **First-order, under the runtime's assumptions**: `exp` within 1 ulp, the
  ~745-log-unit vanishing window, higher-order terms neglected.
- **`n` up to ~2.1e8.** Derived, not searched: recursive summation's
  classical `(n-1)u/(1-(n-1)u)` exceeds the `(n+4)u` charged here once
  `(n-1)(n+4)u > 5`. Past that the neglected second-order term eats the
  constant.

Status: **held**, not proved. `pass/emitted_bound_search.c` searches it on
every `run_pass_test.sh` run and fails the gate on any violation. 7285
trials, worst observed/bound **0.99**, at a large one-depth cluster where the
running sum's roundings align and the observed error is the classical
`(n-1)u`. The same run scores the form *without* the reduction term, which is
exceeded on 321 trials at up to **39x**: that term is required by
measurement here, not inherited by analogy.

Measured on the reference kernel (n=1000, ~N(0,1)): 1.37e-15 relative,
against a 1e-12 test tolerance.

### Special values are PRESERVED — required

Not a quality goal. A failed clause here is a bug, not a tolerance.

| input | required behaviour |
|---|---|
| NaN anywhere | propagates; the result is NaN, including a NaN in the first position and NaN mixed with infinities |
| `+inf` present | the sum is `+inf`; exported log form is `+inf` |
| `-inf` terms | ordinary zero terms (`exp(-inf) = 0`); the finite result is unchanged |
| all terms `-inf` | the sum is exactly `0.0`; exported log form is `-inf` |
| signed zero | the empty and all-`-inf` sums are `+0.0`, as in the linear loop |
| zero trip count | the loop is bypassed; the sum is `0.0` and the export hook is not written |

The `oeq`-guarded exponent differences exist for this and may not be
removed. `oeq` specifically: a NaN operand is never `oeq` to the running
maximum, so the subtraction survives, `exp(NaN) = NaN`, and NaN still
poisons the accumulator, matching the linear loop.

Every row is tested in `pass/test_softmax.c`, against constants **and**
against an independent max-shift reference oracle.

### Not preserved, by construction

errno, FP exception flags, rounding-mode dependence, denormal flushing
behaviour. Sections 2 and 4 exist so that no eligible program can observe
any of them.

## 6. Log-form propagation into consumers (the stretch goal, first form)

*This section governs rewriting the loop's **consumers**, not the loop.
Nothing in it is reachable unless sections 1–5 already passed: propagation
is layered on a successful rewrite, never attempted on its own. It
implements the first milestone of `logrange_intent.md`, "Stretch Goal —
End-to-End Log-Form Propagation", and closes "Shipping Posture" condition 2
(the rescue observable without the `__logrange_logsum` side global).*

### 6.1 What propagation is

A rewritten loop produces the log-domain value `L = m + log(s)`, the
log-magnitude of the sum, live in the replacement block before any `exp()`
is applied. Propagation rewrites an eligible consumer of the linear sum to
consume `L` directly, and **deletes the `exp(L)` materialization for every
rewritten use**. The rescue is observable precisely when materialization is
removed: in the regime this project targets, `exp(L)` re-underflows to
`0.0` while `L` itself is a healthy double.

Propagation is intra-function only. A value that crosses a function
boundary (return, call argument, store to memory that escapes) is a
frontier, and the materialization stays.

### 6.2 Opt-in: per consumer-form, named, refused if misspelled

Propagation is a **separate grant** from the loop rewrite's reassociation
grant. Neither `force` nor `"unsafe-fp-math"="true"` reaches it. Each
consumer form is enabled by its own pass parameter, semicolon-separated
inside `<>`:

| parameter | consumer form rewritten |
|---|---|
| `propagate=div` | `fdiv(llvm.exp(t), sum)` → `exp(t - L)` |

An unrecognized `propagate=` value is **refused**, exactly as an
unrecognized `min-risk=` value is: the pipeline fails to parse rather than
silently reverting to no propagation. With no `propagate=` parameter, no
consumer is touched; the default is the section 1–5 behaviour.

Why a separate grant: the loop rewrite changes rounding only in the
accumulation; a consumer rewrite changes rounding in *another* operation
(the divide becomes a subtract). Those are two different things to permit,
and the caller grants them separately. `force` grants reassociation of the
sum; it says nothing about rewriting a division downstream of it.

### 6.3 Eligible consumer: the divide form, all clauses required

A consumer instruction is eligible for `propagate=div` only if **every**
clause holds. A failed clause is a decline, logged as
`DECLINE-PROP,<file>,<line>,<function>,<reason>`; it is never a fallback
and never a partial rewrite.

| clause | requirement | reason token if failed |
|---|---|---|
| denominator | the divisor is the rewritten sum (the loop's final value, post-LCSSA), not the running sum | `not-the-sum` |
| operator | `fdiv`, operand 1 is the sum; `fmul` by a reciprocal is *not* matched (a different rounding and a different NaN/inf profile) | `not-fdiv` |
| type | `double` | `not-double` |
| numerator | the dividend is an `llvm.exp.*` call (possibly through `fpext`/`fptrunc`) whose argument is `double`; the pre-`exp` argument is what the rewrite consumes (see 6.4) | `numerator-not-exp` |
| log form of the divisor | a log form of the divisor exists on **every** path reaching the consumer, derived by the rule in 6.3.1 | `no-log-form` |
| placement | that log form dominates the consumer | `log-form-not-available` |
| uniqueness-of-form | the rewritten value is not also consumed by an ineligible use that *shares this instruction's* result (an instruction is rewritten whole or not at all) | `shared-result` |

### 6.3.1 Deriving the divisor's log form

`L` is not simply the `LogSum` computed on the loop's exit edge. clang guards
the accumulation loop, so the value the consumer divides by is an LCSSA phi
merging the rewritten sum with the constant `0.0` from the zero-trip path, and
no single block dominates the consumer. Requiring one is why an earlier
version declined this shape, the milestone case, as `not-dominated`.

The log form is therefore derived structurally, by the lattice's phi
transfer function:

| divisor is | log form |
|---|---|
| the rewritten sum | `LogSum`, computed on the exit edge |
| `ConstantFP c`, `c > 0` | `ConstantFP log(c)` |
| `ConstantFP 0.0` | `-inf` |
| `ConstantFP` negative or NaN | decline; no real logarithm |
| a phi outside the loop | a parallel phi in the same block, of its operands' log forms |
| anything else | decline |

The parallel phi is memoized before its operands are visited, so a cyclic phi
network terminates rather than recursing forever. On the guarded softmax this
emits exactly one extra phi:

```llvm
%lr.logphi = phi double [ 0xFFF0000000000000, %entry ], [ %lr.logsum, %lr.exit ]
```

`-inf` on the zero-trip path is not a special case bolted on: it is the log
form of the `0.0` the linear code carries there, and it reproduces the linear
result. Finite numerator over a zero sum gives `x/0 = +inf`, and
`exp(t - (-inf)) = exp(+inf) = +inf`. A zero numerator gives `0/0 = NaN`, and
`exp(-inf + inf) = exp(NaN) = NaN`.

### 6.4 The rewrite and its algebra

```
y = fdiv(llvm.exp(t), s)     where L is the divisor's log form
  becomes
y = llvm.exp( t - L )
```

This is the softmax divide becoming a subtract in the log domain.

The numerator **must** be an `llvm.exp` call, and the rewrite consumes its
pre-`exp` argument `t` directly. Emitting `log(numerator) - L` instead would
be wrong in exactly the regime this exists for: the numerator is `exp(t)`,
which underflows to `0.0` at rescue-regime inputs, so `log(numerator)` is
`log(0) = -inf` and the propagated result collapses to `0` or NaN.
Correctness must not depend on a later InstCombine fold of
`log(exp(t)) -> t` either. The pass emits the subtract on `t` itself, so the
transform is correct in the IR it produces rather than in the IR someone
else might canonicalize it into.

### 6.5 What propagation preserves

Finite rounding changes **more** than in the loop rewrite, and that is
permitted and stated: the divide is replaced by a subtract, so the result
is not bitwise identical to `fdiv` even on benign inputs. This is the
second thing the (separate) propagation grant pays for.

Special values are preserved relative to the identically-flagged linear
form, with one row added to the section-5 table's family:

| input | required behaviour |
|---|---|
| sum underflows (the rescue regime) | consumer yields a **finite, correct** value where the linear form yields `0.0` or NaN-from-`0/0`; the linear program cannot produce this |
| NaN in numerator or any sum term | result is NaN |
| numerator `0.0` | `log(0) = -inf`; `-inf - L` with finite `L` is `-inf`; `exp(-inf) = +0.0`, matching linear `0/s` |
| numerator `+inf` / sum `+inf` | `inf/inf = NaN` linear; `log(inf) - inf = inf - inf` is guarded to NaN by the same `oeq` discipline as section 5: NaN, matching linear |

The rescue row is tested against the harness's independent max-shift
reference on a full softmax (denominator loop plus normalize divide in one
function), asserting correct finite probabilities where the original
returns all-zero/NaN. NaN, ±inf, and zero-numerator rows are tested as
constants and against the reference.

### 6.6 Not preserved, and the stopping rule

errno, FP exception flags, rounding-mode dependence and denormal flushing
remain unpreserved for the whole function (sections 2 and 4 already screen
these out before any loop is seen).

Propagation stops at the first use with no eligible rewrite: the
materialization `exp(L)` is kept there and the log region ends. A consumer
the lattice cannot rewrite is a decline with a reason token, never a
best-effort transform. The general Linear/Log/Conflict dataflow over
arbitrary consumers is the stretch goal's second step and is **out of
scope** for this section; if the frontier turns out to sit immediately
outside the first loop on real code, that is a documented result, not a
failure to be patched over.

## 7. Profitability gate

*Numbered last, ordered third. The gate runs on a loop that has passed
sections 1–4, before section 3.3's weight clause and before any section 6
propagation. It was undocumented here until 2026-08-17.*

Shape is not profitability. The matcher study found the abundant hits are
benign-range dot products, where a log rewrite costs a transcendental per
term and buys nothing; the rescue-worthy subset is 5 HIGH rows in 2859 loops
(`matcher/RESULTS.md`). The risk verdict is what separates them.

### 7.1 The verdict

**An accepted `exp` call in the term gives HIGH**, because `exp(t)` spans the
whole range from `t` alone. Otherwise LOW.

#### This is not the matcher's rule

An earlier version of this section claimed it was. It is not, and the
divergences are reachable on ordinary code. Both measured, LLVM 21:

| loop | matcher | this pass |
|---|---|---|
| `s += a*b*c*d*e` | MED `deep-chain` | `DECLINE-RISK,LOW` |
| `s += log(a)*log(b)*c` | MED `log-chain` | `DECLINE-RISK,LOW` |
| `s += c*pow(a,b)` | HIGH `exp-chain` | `DECLINE-RISK,LOW` |
| `s += pow(a,b)`, `s += exp2(x)` | HIGH | silent (not matched) |
| `s += exp(a*b*c*d)` | HIGH `exp-chain` | REWRITE `exp-chain;exp-sum` |

Two independent causes:

- **exp-family is wider in the matcher.** `isExpFamilyName` matches
  `exp`/`pow` by substring, so `pow`, `exp2` and `expm1` all grade HIGH there.
  Section 4's errno contract restricts this pass to `exp`/`expf`/`llvm.exp`.
  Where the pass matches such a loop anyway (because it carries a weight) it
  prints LOW for a loop the matcher calls HIGH.
- **`nMul` is a different quantity.** The matcher's `walkChain` counts
  multiplies over the **whole term chain**, including inside the `exp`
  argument, and counts `fdiv` as a multiply. This pass counts spine
  multiplies only, of which there is at most one.

**MED is unreachable in this pass by construction** — the verdict function
computes only `Low` and `High`. That is a narrower claim than "no matched
loop can be graded MED", which is false: the matcher grades the first two
rows above MED.

#### The reason list is a constant

Every `REWRITE` line prints `exp-chain;exp-sum` unconditionally. `exp-chain`
is always correct for a rewritten shape. `exp-sum` means `nMul == 0` to the
matcher and is **wrong whenever the chain contains a multiply or divide**, as
in the last row. Computing it would need the chain walk this pass
deliberately does not have.

### 7.2 The threshold

Parameter `min-risk=<low|med|high|none>`, default `high`. A verdict below the
threshold is declined:

    DECLINE-RISK,<file>,<line>,<function>,<verdict>,below-min-<threshold>

An unrecognized value is **refused** — the pipeline fails to parse rather
than silently reverting to the default, which would disable the gate on a
typo.

`none` is ordered above `high` deliberately: it is a threshold no verdict can
reach, so `min-risk=none` means "decline everything".

### 7.3 The gate is reachable, and it is not load-bearing

`dot_sum` in `pass/test_softmax.c` — `llvm.fmuladd(x, y, phi)`, no `exp` — is
matched by 3.1, verdicts LOW, and is refused at the **default** threshold.
That is reachability, and it is new as of the 3.1 widening.

**It does not follow that the gate gates anything.** From the implementation:

    rewritable   ⟺  EK == Intrinsic     (the eligibility guard)
    verdict HIGH ⟺  EK != No            (the verdict function)

The same predicate. Every rewritable loop verdicts HIGH, so no rewrite is
ever below the threshold. And every LOW loop is weighted — 3.2 guarantees
`Weight` is non-null whenever there is no `exp` — so 3.3 refuses it at *any*
threshold. Measured: at `min-risk=low` the LOW inputs clear the gate and log
`DECLINE-WEIGHT` instead. **The gate selects which reason token is printed;
it never changes whether a rewrite happens.**

`run_pass_test.sh` asserts this directly: the set of `REWRITE` lines at
`min-risk=low` is identical to the set at the default threshold. The
assertion is inverted on purpose — it encodes the limitation, so it turns red
exactly when the limitation is fixed.

**What would close it.** The gate becomes load-bearing when the rewritable
set exceeds the HIGH set — that is, when the pass can soundly rewrite a shape
that verdicts LOW or MED. Bounded-weight support (3.3's extension point) is
the natural candidate: `s += 0.5*x[i]` verdicts LOW and would be rewritable,
so the gate would refuse an actual rewrite. Adding matched-but-declined
shapes cannot close it, however many are added.

Both branches are gated in `run_pass_test.sh`, and the decline branch is
negative-tested two ways: hardcoding the verdict HIGH, and reordering 3.3
ahead of this section. Each makes the `dot_sum` assertion fail.
