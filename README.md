# LogRange

Correct sums when terms underflow or overflow in linear floating point.

**Status:** v0.2, pre-1.0 — the header is complete and benchmarked; the
compiler tooling is a working prototype. Gaps are tracked in [TODO.md](TODO.md).

## What it is

Some sums fail in linear floating point: mixture likelihoods, forward-algorithm
recursions, softmax denominators — anything where individual terms underflow or
overflow. A naive loop returns 0.0, inf, or NaN.

LogRange is a C++17 header-only library for signed log-domain accumulation.
Values are `{sign, log|x|}`; the header provides pairwise arithmetic
(`logsumexp2`, `log_add`, `log_mul`, `log_div`) and two accumulators:
- `pos_accum` — fast path for positive-only sums
- `rp_accum` — general case, signed, with cancellation handling

Both have stated worst-case error bounds and IEEE-compliant edge semantics:
NaN in → NaN out, infinities propagate, zeros handled explicitly.

**Cost vs. benefit:** ~2–3× slower than a linear loop (each term needs `exp()`),
but produces correct answers where linear fails. See
[BENCHMARKS.md](BENCHMARKS.md) for numbers.

## Use

Header-only: add `include/` to your include path.

```c++
#include <logrange/log_math.h>

logrange::pos_accum acc;
for (double log_term : log_terms) acc.add_log(log_term);
logrange::log_value total = acc.to_log_value();
```

## Build & test

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Benchmarks (Release only):

```
./build/Release/bench_logrange
```

## Repository map

| path | what | maturity |
|---|---|---|
| `include/logrange/log_math.h` | library (the product) | benchmarked, formal error bound |
| `tests/` | unit, contract, accuracy suites | run in CI |
| `bench/` | benchmark harness | see BENCHMARKS.md |
| `matcher/` | LLVM plugin + hit-rate study | [RESULTS.md](matcher/RESULTS.md) |
| `matcher/diagnose.sh` | range lint | [DIAGNOSTIC.md](matcher/DIAGNOSTIC.md) |
| `pass/` | LLVM pass prototype, opt-in | [PROTOTYPE.md](pass/PROTOTYPE.md) |

Matcher and pass: Linux/WSL, LLVM 21.
Library and tests: C++17 compiler only.

## Documents

- [logrange_intent.md](logrange_intent.md) — aims, cost model, deliverables
- [BENCHMARKS.md](BENCHMARKS.md) — results
- [matcher/METHODOLOGY.md](matcher/METHODOLOGY.md) — study rules
- [TODO.md](TODO.md) — road to 1.0
- [BASELINE.md](BASELINE.md) — historical numbers

## License

MIT — see [LICENSE](LICENSE). Vendor freely; keep the notice.
