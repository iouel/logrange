# LogRange

A small, well-specified C++17 header-only library for **signed log-domain
accumulation**: correct sums of terms whose magnitudes exceed floating-point
range, where linear arithmetic silently degrades to 0.0, inf, or NaN.

Values are carried as `{sign, log|x|}`. The library provides pairwise
log-domain arithmetic (`logsumexp2`, `log_add`, `log_mul`, `log_div`) and two
reference-exponent accumulators — `pos_accum` (positive-only fast path) and
`rp_accum` (signed, cancellation-aware) — with documented error contracts and
IEEE-faithful edge semantics: NaN in means NaN out, infinities propagate,
zeros are handled deliberately.

Slower than a linear loop, always (each term costs an `exp`). What that buys:
the computation finishes with a correct answer in the regime where the linear
loop returns garbage. Measured numbers, including the cases where other
approaches win, are in [BENCHMARKS.md](BENCHMARKS.md).

## Use

Header-only: add `include/` to your include path.

```c++
#include <logrange/log_math.h>

logrange::pos_accum acc;
for (double log_term : log_terms) acc.add_log(log_term);  // e.g. log-likelihoods
logrange::log_value total = acc.to_log_value();           // {sign, log_abs}
```

## Build & test

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Benchmarks (Release only; prints a measured noise floor first):

```
./build/Release/bench_logrange
```

## Beyond the header

- `matcher/` — LLVM opt plugin that recognizes sum-of-products reductions in
  real code, with a three-codebase hit-rate study ([RESULTS.md](matcher/RESULTS.md))
  and profitability triage (781 shape hits → 3 genuinely underflow-prone sites).
- `matcher/diagnose.sh` — the range lint: plain-English findings pointing at
  this header as the fix ([DIAGNOSTIC.md](matcher/DIAGNOSTIC.md)).
- `pass/` — prototype LLVM pass rewriting the softmax-denominator idiom to
  streaming logsumexp at IR level, behind explicit opt-in
  ([PROTOTYPE.md](pass/PROTOTYPE.md)).

The matcher and pass build on Linux/WSL against LLVM 21 (`llvm-dev`); the
library and tests need only a C++17 compiler.

## Project documents

- [logrange_intent.md](logrange_intent.md) — aims, honest cost model, deliverables, status
- [BENCHMARKS.md](BENCHMARKS.md) — measured results against the success criteria
- [matcher/METHODOLOGY.md](matcher/METHODOLOGY.md) — study rules, fixed before counting
- [BASELINE.md](BASELINE.md) — historical predecessor numbers (do not cite)
