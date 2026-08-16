# LogRange

WIP
Correct sums when terms underflow or overflow in linear floating point.

**Status**

v0.3, pre-1.0. The header is complete and benchmarked; the compiler tooling is
a working prototype. Gaps are tracked in [TODO.md](TODO.md).

## What it is

Some sums fail in linear floating point wherever individual terms underflow or
overflow: mixture likelihoods, forward-algorithm recursions, softmax
denominators. A naive loop returns 0.0, inf, or NaN.

The header rescues all three. The diagnostic finds two: mixture likelihoods and
softmax denominators are flagged HIGH. The forward algorithm is flagged in
neither form it is usually written. `out[j] += ...` is rejected at the
mid-loop-read guard. The register-accumulator form is seen but graded LOW: its
underflow accumulates across the enclosing time-step loop while each inner
reduction looks unremarkable. See [DIAGNOSTIC.md](matcher/DIAGNOSTIC.md),
"Scope limits".

LogRange is a C++17 header-only library for signed log-domain accumulation.
Values are `{sign, log|x|}`; the header provides pairwise arithmetic
(`logsumexp2`, `log_add`, `log_mul`, `log_div`) and two accumulators:
- `pos_accum` — fast path for positive-only sums
- `rp_accum` — general case, signed, with cancellation handling

Both have stated worst-case error bounds and IEEE-compliant edge semantics:
NaN in → NaN out, infinities propagate, zeros handled explicitly.

**Cost**

~2–3× slower than a linear loop, since each term needs `exp()`. Produces
correct answers where linear fails. See [BENCHMARKS.md](BENCHMARKS.md) for
numbers.

## Use

```c++
#include <logrange/log_math.h>

logrange::pos_accum acc;
for (double log_term : log_terms) acc.add_log(log_term);
logrange::log_value total = acc.to_log_value();
```

That snippet is [examples/quickstart](examples/quickstart), compiled against
the installed package and checked against its analytic answer. CI builds and
runs it as a consumer on every platform, so this section cannot drift from what
works.

## Install

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build --config Release
cmake --install build --config Release
```

Then, from a consuming project:

```cmake
find_package(LogRange 0.3 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE LogRange::logrange)
```

Vendoring works too: `add_subdirectory(logrange)` gives the same target, builds
no tests, and does not impose this project's `-Werror` on yours.

**No compile flags are imposed on you.** One thing is refused rather than
imposed: the header will not compile under `-ffast-math` / `/fp:fast`.
Reassociating math folds away the algebraic identities `rp_accum` uses to
recover each addition's rounding error, and the accumulator degrades to an
uncompensated sum: 4.9e-6 relative on a cancellation set, nine orders past the
stated contract. That is a `#error`, with `LOGRANGE_ALLOW_FAST_MATH` to
override it if you accept an uncompensated result.

FMA contraction (`-ffp-contract=fast`) is a *different* flag and is fine: the
compensation path contains no multiply-add pair to fuse, and results are
bit-identical with it on.

Version compatibility is `SameMinorVersion`. Pre-1.0 the error contract can
move between minor versions, and it has.

Header-only, so vendoring the single header by hand also works. The flag above
is then your responsibility.

## Find the sums that need it

Point the diagnostic at a build directory and it names the reductions whose
terms may leave representable range:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
matcher/logrange-scan.sh build
```

Exit 1 on a HIGH finding, so it can gate CI. It configures nothing and builds
nothing of yours: it recompiles each unit in the compile database to bitcode
itself. Linux or WSL and LLVM 21 only ([SETUP.md](SETUP.md); `--check` reports
what is missing). Scope and known blind spots:
[DIAGNOSTIC.md](matcher/DIAGNOSTIC.md).

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
| `matcher/` | LLVM plugin + hit-rate study | beta, gaps stated — [RESULTS.md](matcher/RESULTS.md) |
| `matcher/logrange-scan.sh` | range lint, build dir in | beta, the front door — [DIAGNOSTIC.md](matcher/DIAGNOSTIC.md) |
| `pass/` | LLVM pass prototype, opt-in | prototype, not installed — [PROTOTYPE.md](pass/PROTOTYPE.md) |

Matcher and pass: Linux/WSL, LLVM 21 — [SETUP.md](SETUP.md).
Library and tests: C++17 compiler only.

## Documents

- [CONTRIBUTING.md](CONTRIBUTING.md) — evidence and writing conventions, read first
- [logrange_intent.md](logrange_intent.md) — aims, cost model, deliverables
- [BENCHMARKS.md](BENCHMARKS.md) — results
- [SETUP.md](SETUP.md) — WSL and LLVM 21, for the tooling only
- [matcher/METHODOLOGY.md](matcher/METHODOLOGY.md) — study rules
- [TODO.md](TODO.md) — road to 1.0
- [BASELINE.md](BASELINE.md) — historical numbers

## License

MIT — see [LICENSE](LICENSE). Vendor freely; keep the notice.
