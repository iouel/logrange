# Product scope: what ships to `iouel/logrange-runtime`

This document is an **extraction contract and provenance record**, not a
deletion request. Nothing listed here as `research-only` is being moved or
removed from this repository. It stays, in full, as the audit trail behind
the numbers and decisions in the runtime product.

`iouel/logrange` is the research, validation, audit-trail, and LLVM-prototype
repository. `iouel/logrange-runtime` is the supported, user-facing product
repository. This file classifies the content of this repository so that
extraction work — human or agent — has one place to check before copying,
citing, or leaving behind any given path. See [PRODUCT_REPO.md](PRODUCT_REPO.md)
for the repository-to-repository relationship, ownership boundaries, and
synchronization policy.

A machine-readable version of this same classification is in
[product-manifest.yml](product-manifest.yml).

## Categories

- **`ship`** — appropriate to copy (or port, in the case of tests) into
  `iouel/logrange-runtime`. This is the supported public surface: the header,
  its build/packaging plumbing, the tests that pin its API and error
  contract, the example a consumer would copy, and the license and
  changelog material a consumer needs.
- **`research-only`** — stays only in this repository. Adversarial search
  tooling, benchmark methodology and raw data, the LLVM matcher/diagnostic,
  the LLVM rewrite pass prototype, and the historical/process documents that
  explain how the product got here. None of this is part of the supported
  public contract, and copying it into the product repo would misrepresent
  its maturity or invite users to depend on unstable internals.
- **`reference/link`** — should be cited or linked from the product repo
  (e.g. "see the research repo for X") rather than copied wholesale. Typically
  applies to narrative documents that are useful context but not something the
  product repo should own or keep in sync verbatim.

## Classification

### Runtime library and packaging — `ship`

| path | classification | notes |
|---|---|---|
| `include/logrange/log_math.h` | ship | the product itself: `pos_accum`, `rp_accum`, pairwise log-domain ops. Copy verbatim; this repo's copy becomes the historical/reference copy once the product repo is the source of truth (see synchronization policy in PRODUCT_REPO.md). |
| `CMakeLists.txt` (root) | ship | must be re-derived, not copied verbatim: the product repo's build should only build/install/test the header and drop this repo's matcher/pass/bench subdirectory wiring. Use this file as the reference for install rules, version parsing (`LOGRANGE_VERSION_*`), and package config generation. |
| `cmake/` | ship | `LogRangeConfig.cmake.in` is the package-config template consumers use via `find_package(LogRange ...)`. Carries over with the CMake project. |
| `examples/quickstart/` | ship | the exact snippet quoted in the README's "Use" section, built and run against the installed package. This is the canonical "does install work" smoke test and belongs in the product repo's CI. |
| `LICENSE` | ship | required in any repository distributing the header. |
| Consumer-appropriate changelog material | ship (curated excerpt, not the full file) | `CHANGELOG.md` in this repo is a 60KB+ process/research log spanning matcher, pass, and bound-search history. The product repo needs a much shorter changelog: version, date, and user-visible API/behavior/error-contract changes only. See "Changelog material" below. |

### Runtime API/behavior tests — `ship`

| path | classification | notes |
|---|---|---|
| `tests/test_log_math.cpp` | ship | tests the pairwise arithmetic API (`logsumexp2`, `log_add`, `log_mul`, `log_div`) and edge semantics (NaN/inf/zero). This is the public contract test. |
| `tests/test_pos_accum.cpp` | ship | pins `pos_accum` behavior and its stated error bound at the API level. |
| `tests/test_rp_accum.cpp` | ship | pins `rp_accum` behavior, signed sums, and cancellation handling at the API level. |
| `tests/test_accuracy.cpp` | ship | fixed-scenario accuracy checks against a reference; this is the "does the shipped bound hold on known cases" regression suite a consumer-facing CI should keep running on every change. |
| `tests/test_common.h` | ship | shared test helpers used by the four suites above; needed to build them. |
| `tests/dd_exp.h`, `tests/dd_sum.h` | ship (test-support only) | double-double reference arithmetic used as the accuracy oracle in `test_accuracy.cpp`. Ship only as much as the accuracy suite needs; do not present as a general-purpose double-double library. |

### Research-only tests and adversarial tooling — `research-only`

| path | classification | notes |
|---|---|---|
| `tests/bound_search.cpp` | research-only | adversarial counterexample search against the stated error bounds. This is exploration infrastructure that *produced* the current bound, not a fixed regression test; it belongs in the audit trail, not the product's CI gate. |
| `tests/chain_search.cpp` | research-only | adversarial search over log-domain propagation chains; supports the (refuted) stretch-goal investigation in `pass/CHAINS.md`. |
| `tests/test_rescue_shim.cpp` | research-only | tests `matcher/rescue_shim.cpp`, part of the diagnostic/rescue research path, not the header API. |

### Diagnostic, matcher, and pass — `research-only`

| path | classification | notes |
|---|---|---|
| `matcher/` (entire directory) | research-only | LLVM-based static diagnostic: plugin source, scan scripts, methodology, results, raw study data (`data/`, `testdata/`). Ships alongside the header today at "beta" maturity per the README, but is out of scope for the product repo unless/until it is independently adopted there as a supported tool with its own packaging and support commitment. |
| `pass/` (entire directory) | research-only | LLVM rewrite pass prototype: narrow, opt-in, explicitly outside the supported surface per `pass/PROTOTYPE.md`. Stays here. |
| `bench/` (entire directory) | research-only | benchmark harness and methodology. The product repo may eventually want a small, curated "does it still perform" smoke benchmark, but the full harness, methodology, and machine-to-machine noise-floor analysis stays here. |

### Research narrative, process, and historical documents — `research-only` or `reference/link`

| path | classification | notes |
|---|---|---|
| `TODO.md` | research-only | internal roadmap/process record ("road to 1.0"); not user-facing. |
| `logrange_intent.md` | reference/link | states the project's aims, cost model, and deliverables; useful context for *why* the API looks the way it does. Link to it from the product repo rather than duplicating it. |
| `BASELINE.md` | research-only | explicitly a historical record from a predecessor project, marked "do not cite". |
| `BENCHMARKS.md` | reference/link | detailed benchmark methodology and results belong here; the product repo's README should state the headline cost (`~2–3× slower than a linear loop`) and link here for the full study rather than reproducing it. |
| `CONTRIBUTING.md` | research-only | evidence/writing conventions for *this* repo's research process (adversarial search, refutation discipline). The product repo should have its own, much shorter contribution guide focused on API stability and semantic versioning. |
| `SETUP.md` | research-only | WSL/LLVM 21 setup is only needed for the matcher/pass tooling, which does not ship to the product repo. |
| `CHANGELOG.md` | research-only (source), curated excerpt ships | see "Consumer-appropriate changelog material" above. |

## Consumer-appropriate changelog material

`CHANGELOG.md` in this repository mixes user-visible header changes with
matcher/pass/bound-search research history. The product repo's changelog
should include only entries that changed the shipped header's API, behavior,
or error contract (e.g. the three pre-1.0 error-contract revisions and the
1.0 release itself), each rephrased to reference only the public surface.
This repo's `CHANGELOG.md` remains the authoritative, complete history.

## Essential vs. non-essential to the public runtime contract

**Essential** (must exist, in some form, in the product repo's CI):
`test_log_math.cpp`, `test_pos_accum.cpp`, `test_rp_accum.cpp`,
`test_accuracy.cpp`, and the `examples/quickstart/` install smoke test. These
are the fixed, deterministic checks that pin the documented API and error
contract.

**Not essential to the public contract, kept in research** (may still
inform it, or be ported later as independently-adopted tools):
`bound_search.cpp` and `chain_search.cpp` (adversarial, open-ended search —
their *findings* are reflected in the fixed tests and the stated bound, but
the search tools themselves are research infrastructure), the full `matcher/`
and `pass/` trees (LLVM tooling with a Linux/WSL + LLVM 21 dependency,
explicitly beta/prototype maturity), and the full `bench/` harness.

## How to use this document

- Building or updating `iouel/logrange-runtime`: consult the table above
  before copying a path. `ship` → copy/port. `reference/link` → cite it, do
  not duplicate. `research-only` → leave it here.
- Discovering an inaccuracy: this file should be corrected via PR, same as
  any other document in this repository.
- This document does not grant permission to delete or move anything out of
  this repository; it is a map for extraction, not a migration checklist.
