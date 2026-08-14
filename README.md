# LogRange

Correct sums when the terms leave floating-point range.

**Status:** v0.2, pre-1.0 — the header is complete and benchmarked; the
compiler tooling is a working prototype. Gaps are tracked in [TODO.md](TODO.md).

## What it is

Some sums are structurally doomed in linear floating point: mixture
likelihoods, forward-algorithm recursions, softmax denominators — anything
summing terms that individually underflow or overflow. The linear loop
returns 0.0, inf, or NaN, silently.

LogRange is a small C++17 header-only library for **signed log-domain
accumulation**. Values are carried as `{sign, log|x|}`; the header provides
pairwise arithmetic (`logsumexp2`, `log_add`, `log_mul`, `log_div`) and two
reference-exponent accumulators — `pos_accum` (positive-only fast path) and
`rp_accum` (signed, compensated, cancellation-aware) — with **stated
worst-case error bounds** and IEEE-faithful edge semantics: NaN in means NaN
out, infinities propagate, zeros are handled deliberately.

The honest cost: each term pays an `exp()`, so this is always slower than a
linear loop (~2–3×, measured). What that buys is a correct answer in the
regime where the linear loop returns garbage. Numbers — including the cases
where other approaches win — are in [BENCHMARKS.md](BENCHMARKS.md).

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

Benchmarks (Release only; prints its measured noise floor first):

```
./build/Release/bench_logrange
```

## Repository map

| path | what | maturity |
|---|---|---|
| `include/logrange/log_math.h` | the library — the product | benchmarked, formal error bound |
| `tests/` | unit, contract, and accuracy suites (double-double reference) | run in CI |
| `bench/` | trustworthy-by-construction benchmark harness | see BENCHMARKS.md |
| `matcher/` | LLVM plugin recognizing sum-of-products reductions + 3-codebase hit-rate study | study published in [RESULTS.md](matcher/RESULTS.md) |
| `matcher/diagnose.sh` | the range lint: flags underflow-prone reductions, points here as the fix | working ([DIAGNOSTIC.md](matcher/DIAGNOSTIC.md)) |
| `pass/` | prototype LLVM pass rewriting softmax-style sums to streaming logsumexp, opt-in | verified prototype ([PROTOTYPE.md](pass/PROTOTYPE.md)) |

The matcher and pass build on Linux/WSL against LLVM 21 (`llvm-dev`); the
library and tests need only a C++17 compiler.

## Documents

- [logrange_intent.md](logrange_intent.md) — aims, honest cost model, deliverables, status ladder
- [BENCHMARKS.md](BENCHMARKS.md) — measured results against the success criteria
- [matcher/METHODOLOGY.md](matcher/METHODOLOGY.md) — study rules, fixed before counting
- [TODO.md](TODO.md) — road to 1.0
- [BASELINE.md](BASELINE.md) — historical predecessor numbers (do not cite)
