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
action — `-ffast-math` / `-funsafe-math-optimizations`.

It is **never** read as evidence that special values may be discarded.
Every clause in sections 2–4 applies identically on both grant paths. The
attribute unlocks nothing beyond the reassociation grant itself.

Caveat, stated because it is real: under `-ffast-math` the surrounding IR
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

- **strictfp / constrained** — the rewrite changes both the number and the
  operands of the FP operations. A dynamic rounding mode gives a different
  result, and the exception record differs. Constrained intrinsics carry
  per-operation rounding and exception metadata that the plain
  `fadd`/`fmul`/`fsub` and `llvm.exp`/`llvm.log`/`llvm.maxnum` emitted here
  cannot express.
- **denormal** — a non-IEEE denormal mode (flush-to-zero, preserve-sign)
  changes *which* intermediate values become zero. The rewrite's
  intermediates are `exp()` of differences against a running maximum, a
  different set of values entirely from the source loop's `exp(x_i)`, so
  which of them flush is not preservable.

## 3. Structural match

Innermost loop with preheader, unique latch, unique exiting block, unique
exit block; exactly one FP phi in the header, of type `double`, initialized
to constant `0.0` from the preheader; backedge update a plain
`fadd(phi, X)`; `X` an accepted `exp` call through nothing but
`fpext`/`fptrunc`; the call argument loop-varying; phi and update with no
other in-loop users; at least one out-of-loop user of the sum; the update
dominating the exit branch.

## 4. Source-level `exp`/`expf` is not accepted

Only `llvm.exp.*` is accepted. A direct call to `exp`/`expf` is declined
with `DECLINE-ERRNO,<file>,<line>,<function>,external-exp-call`.

This is the errno contract, not a shape preference. The rewrite deletes N
source `exp` evaluations and emits 2N different exponentials plus a `log`,
so every errno write and FP-exception flag the source loop performed is
gone or different. For a conforming program with `math_errno` in force,
that is observable behaviour.

`llvm.exp` is exactly the marker that errno is already unobservable. Clang
emits it only when errno cannot be read — measured, LLVM 21, on
`pass/test_softmax.c`: at `-O1` the source `s += exp(x[i])` emits
`call double @exp`; adding `-fno-math-errno` emits `llvm.exp.f64`.
Restricting to the intrinsic therefore *is* the errno contract.

Consequence for callers: the kernel must be compiled `-fno-math-errno` (or
`-ffast-math`) to be eligible at all.

**Extension point, deliberately not taken.** A direct external `exp`/`expf`
call MAY be accepted once the IR itself proves the call has no observable
memory or errno effect — the call site's memory effects excluding writes to
errno memory and to inaccessible memory. LLVM 21 models this explicitly: an
errno-writing `exp` declaration carries `memory(errnomem: write)`.
Implementing it requires its own accept and decline tests. Until those
exist, external calls are declined unconditionally.

## 5. What the transform preserves, and what it does not

### Finite rounding MAY change — intentionally

The accumulation algorithm changes. `s += exp(x_i)` becomes streaming
logsumexp with rescaling; the operations, their order, and their operands
all differ. Finite results are therefore not bitwise identical.

Measured on the reference kernel (n=1000, ~N(0,1)): 1.37e-15 relative,
against a 1e-12 bound. That is a permitted, expected difference and the
whole point of the transform — it is what buys back the range.

This is the one thing the reassociation grant in section 1 pays for. It is
the only thing it pays for.

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
poisons the accumulator — matching the linear loop.

Every row is tested in `pass/test_softmax.c`, against constants **and**
against an independent max-shift reference oracle.

### Not preserved, by construction

errno, FP exception flags, rounding-mode dependence, denormal flushing
behaviour. Sections 2 and 4 exist so that no eligible program can observe
any of them.
