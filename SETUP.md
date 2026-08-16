# Setup — the LLVM tooling

The library needs a C++17 compiler and nothing else. This file is only for the
three LLVM tools.

| tool | needs |
|---|---|
| `include/logrange/log_math.h` | C++17 compiler. Windows, Linux, macOS. Nothing here applies. |
| `matcher/logrange-scan.sh` (the diagnostic) | Linux or WSL, LLVM **21**, python3, cmake |
| `matcher/run_study.sh` (the study instrument) | same, minus python3 |
| `pass/run_pass_test.sh` (the rewrite prototype) | same, plus `clang-21` and `opt-21` resolvable by those exact names |

## Why LLVM 21 exactly

Both plugins are `opt` plugins, loaded into the running `opt` process, and a
plugin built against a different LLVM major version does not load at all. The
version is pinned end to end: CI installs 21 explicitly,
`pass/run_pass_test.sh` sets `CMAKE_PREFIX_PATH=/usr/lib/llvm-21` and calls
`clang-21` and `opt-21` by name, and `logrange-scan.sh` refuses to run when
`clang` and `opt` are not 21 (override with `LOGRANGE_LLVM_MAJOR` if you have
rebuilt the plugin against another release).

## Windows: get a Linux

From PowerShell, once:

```
wsl --install -d Ubuntu
```

Everything below runs inside that Ubuntu, not in PowerShell or Git Bash.
`logrange-scan.sh --check` reports the platform first for that reason.

## Install LLVM 21

Same source and same package list CI uses
(`.github/workflows/llvm-tooling.yml`), so a local box and the runner cannot
drift:

```bash
. /etc/os-release
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/apt-llvm-org.gpg
echo "deb [signed-by=/usr/share/keyrings/apt-llvm-org.gpg] https://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-21 main" \
  | sudo tee /etc/apt/sources.list.d/llvm-21.list
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  clang-21 llvm-21 llvm-21-dev zlib1g-dev libzstd-dev cmake python3
```

`apt.llvm.org` publishes per Ubuntu codename. If the install fails with "no
Release file", that codename has no LLVM 21 channel yet; use an older Ubuntu
image or a different LLVM major.

Then make the unversioned names resolve to 21:

```bash
export PATH=/usr/lib/llvm-21/bin:$PATH
```

Or leave `PATH` alone and set `CLANG=clang-21 OPT=opt-21` per run. Both work
for the matcher and the diagnostic; `pass/run_pass_test.sh` needs the
`-21`-suffixed binaries to exist either way.

## Verify

```bash
matcher/logrange-scan.sh --check
```

```
logrange-scan preflight (wanted: LLVM 21)
  platform   Linux 6.18.33.2-microsoft-standard-WSL2
  clang      LLVM 21 at /usr/bin/clang
  opt        LLVM 21 at /usr/bin/opt
  python3    Python 3.14.4
  cmake      cmake version 4.2.3
  plugin     built on demand
```

Exit 0 means the diagnostic will run. Any line that reads MISSING or names the
wrong LLVM version is the thing to fix; the tool prints the same table and
exits 2 rather than starting a scan it cannot finish.

## Producing a compile database

`logrange-scan.sh` reads `compile_commands.json`. Nothing is built from it: it
recompiles each unit to bitcode itself, at the study's flags, with clang. The
target's own compiler and flags do not have to be clang's.

| build system | how |
|---|---|
| cmake | `cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Configure only, no build needed. |
| meson, ninja | emitted into the build directory already |
| make | `bear -- make` (`sudo apt-get install bear`) |
| autotools | `./configure && bear -- make` |
| none of these | the two-step: build under `matcher/cc-bc.sh`, then `matcher/run_study.sh <name>`, then `matcher/diagnose.sh` on the raw file (METHODOLOGY.md) |

## Worked example: this repository

```bash
cmake -S . -B /tmp/lr -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
matcher/logrange-scan.sh /tmp/lr
```

2026-08-16: 6 translation units, 52 innermost FP loops, 3 HIGH findings, exit
1. All three are in `bench/bench_main.cpp`, and all three are the benchmark's
own deliberately-underflowing linear kernels, which is the correct answer.
`bench_main.cpp:302` is the plain sum of `exp` that carries `exp-sum`. The
counts track this repository's own sources and will move when they do;
`test_scan.sh` case 12 asserts the scan still completes and still finds
`k_linear_sum`.

## Known frictions

- **`/mnt/c` is slow.** A cmake configure of a Windows-side tree from WSL costs
  seconds. Keep scan output under `$HOME`, which the scripts already do.
- **WSL `/tmp` does not persist between separate `wsl.exe` invocations.** Do
  multi-step work in one invocation; the scripts keep state under
  `~/logrange-study` and `~/logrange-pass` for the same reason.
- **The plugin is rebuilt when older than its sources**, not on every run.
  `--rebuild` forces it. A failed build deletes the old plugin first, so a
  green run cannot be produced against stale code.
