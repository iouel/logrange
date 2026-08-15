# LogRange

WIP
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

```c++
#include <logrange/log_math.h>

logrange::pos_accum acc;
for (double log_term : log_terms) acc.add_log(log_term);
logrange::log_value total = acc.to_log_value();
```

That exact snippet, compiled against the installed package and checked
against its analytic answer, is [examples/quickstart](examples/quickstart) —
CI builds and runs it as a consumer on every platform, so this section cannot
drift from what works.

## Install

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build --config Release
cmake --install build --config Release
```

Then, from a consuming project:

```cmake
find_package(LogRange 0.2 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE LogRange::logrange)
```

Vendoring works too — `add_subdirectory(logrange)` gives the same target,
builds no tests, and does not impose this project's `-Werror` on yours.

**The imported target carries a compile flag, on purpose.** `-ffp-contract=off`
(`/fp:precise` on MSVC) is part of the error contract, not a style choice:
`rp_accum`'s compensation is written statement-per-step so the rounding error
it recovers cannot be fused away, and FMA contraction undoes that. Measured on
the heavy-cancellation scenario, contraction costs 14× accuracy — 2.7e-09
becomes 3.7e-08 — silently, and by default on any target where FMA is in the
baseline. Opt out with `-DLOGRANGE_PROPAGATE_FP_FLAGS=OFF` if you must; the
contract is then yours to re-establish.

Version compatibility is `SameMinorVersion`, deliberately strict: pre-1.0 the
error contract can move between minor versions, and it has.

Header-only, so vendoring the single header by hand also works — but then the
flag above is your responsibility.

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
