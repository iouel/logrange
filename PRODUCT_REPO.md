# `iouel/logrange` and `iouel/logrange-runtime`

This repository (`iouel/logrange`) split into two repositories:

- **`iouel/logrange`** (this repo) — the research, validation, audit-trail,
  and LLVM-prototype repository. Everything that produced and stress-tested
  the header's error contract lives here, in full, indefinitely.
- **[`iouel/logrange-runtime`](https://github.com/iouel/logrange-runtime)** —
  the supported, user-facing runtime product repository. It owns the public
  API, packaging, releases, and user-facing documentation for the LogRange
  header library.

If you are looking to **use** LogRange in a project, start at
`iouel/logrange-runtime`. If you are looking to understand **how** the
error bound, benchmarks, or diagnostic were derived and validated, or to
work on the matcher/pass tooling, you are in the right place already.

See [PRODUCT_SCOPE.md](PRODUCT_SCOPE.md) and
[product-manifest.yml](product-manifest.yml) for the path-by-path
classification behind this split.

## Ownership boundaries

### `iouel/logrange-runtime` owns

- The public C++17 API surface of `include/logrange/log_math.h`
  (`pos_accum`, `rp_accum`, `logsumexp2`, `log_add`, `log_mul`, `log_div`).
- Packaging: the CMake project, install rules, and `find_package(LogRange
  ...)` config.
- Releases and semantic versioning of the header (`LOGRANGE_VERSION_*`).
- User-facing documentation: install instructions, quickstart, API
  reference, and a changelog scoped to user-visible changes.
- The fixed, deterministic tests that pin the documented API and stated
  error contract (see PRODUCT_SCOPE.md, "Essential vs. non-essential").

### `iouel/logrange` (this repo) owns

- Derivations: how the error bounds for `pos_accum` and `rp_accum` were
  arrived at, and the reasoning behind each revision.
- Stress and adversarial validation: the counterexample-search tooling
  (`tests/bound_search.cpp`, `tests/chain_search.cpp`) that has twice
  refuted an earlier stated bound before the current one was accepted.
- Benchmark methodology and raw results, including cross-machine
  noise-floor analysis (`BENCHMARKS.md`, `bench/`).
- Diagnostics: the LLVM-based static matcher and its hit-rate study
  (`matcher/`).
- Matcher research and the associated scope/limitation documentation
  (`matcher/DIAGNOSTIC.md`, `matcher/METHODOLOGY.md`, `matcher/RESULTS.md`).
- LLVM pass prototypes: the narrow, opt-in rewrite pass and its documented
  eligibility limits (`pass/`), including the refuted end-to-end
  log-form-propagation stretch goal (`pass/CHAINS.md`).
- The historical/process record of how 1.0 was reached (`TODO.md`,
  `logrange_intent.md`, `BASELINE.md`, `CHANGELOG.md`).

## Source / extraction provenance

The extraction work for `iouel/logrange-runtime` is being carried out
against the state of this repository's **`projectsummary`** branch.

At the time this document was written, `projectsummary` pointed at:

```
b288b150abbba4a2cc8cbfc63ef102724e7803f6
```

That commit is recorded here as the branch tip observed from this
repository's own history at the time of this PR — it is *not* independently
confirmed to be the exact commit read by the separate agent bootstrapping
`iouel/logrange-runtime`, since that work happens outside this repository
and its state cannot be inspected from here.

To pin the extraction precisely:

1. In `iouel/logrange-runtime`, record (in its own provenance document or
   release notes) the exact `iouel/logrange` commit SHA its initial
   extraction was taken from, e.g. via a comment such as
   `Extracted from iouel/logrange@<sha>, branch projectsummary`.
2. If that SHA is not already recorded there, use
   `git log -1 --format=%H origin/projectsummary` against this repository
   at the time of extraction to determine it, rather than assuming the SHA
   above is still current — `projectsummary` may have moved since.
3. Do not invent or assume a SHA in either repository; if it cannot be
   determined, state that explicitly and re-pin it once the two
   repositories agree on a starting commit.

`iouel/logrange-runtime`'s source metadata (e.g. `LOGRANGE_VERSION_*`,
`project(... VERSION 1.0.0 ...)`) may already read `1.0.0` because that is
the version the extracted header carries in this repository. That source
version number is **not** the same claim as a published release: do not
assume `iouel/logrange-runtime` has a `v1.0.0` (or any) Git tag or GitHub
Release just because its source says `1.0.0`. Check that repository's own
tags/releases directly before citing one; if none exist, say so rather than
inferring a release from source metadata.

## Synchronization policy

- **Public runtime bug fixes** (anything affecting the header's documented
  API, behavior, or error contract) are applied in `iouel/logrange-runtime`
  first. They are then selectively reflected back into this repository —
  updating this repo's copy of `include/logrange/log_math.h`, its tests,
  and `CHANGELOG.md` — when doing so is needed to keep this repository's
  research fixtures, historical record, or documentation aligned with the
  shipped product.
- **Research discoveries that affect public behavior** (for example, a new
  counterexample found by `tests/bound_search.cpp` or `tests/chain_search.cpp`
  that changes the stated error contract) are ported to
  `iouel/logrange-runtime` via a reviewed pull request there, not by direct
  push. The discovery, its evidence, and the reasoning stay recorded here
  first, per this repository's evidence conventions (`CONTRIBUTING.md`).
- **Diagnostics, matcher, and pass work** stay in this repository. They may
  be proposed for adoption into `iouel/logrange-runtime` later, but only as
  an independent, reviewed decision with its own packaging and support
  commitment — not as an automatic consequence of research progress here.

## How to contribute changes in each category

- **Changes to the public API, its error contract, or packaging**: open a
  pull request against `iouel/logrange-runtime`. Bring supporting evidence
  (a failing test, a counterexample, or a benchmark) — see this repo's
  `CONTRIBUTING.md` for the evidence conventions those repositories share.
- **New adversarial evidence, benchmark methodology, or diagnostic/pass
  work**: open a pull request against this repository (`iouel/logrange`).
  If the finding changes the public contract, note in the PR description
  that a follow-up PR against `iouel/logrange-runtime` is needed, per the
  synchronization policy above.
- **Documentation describing the split itself** (this file,
  `PRODUCT_SCOPE.md`, `product-manifest.yml`): open a pull request against
  this repository; keep classifications and provenance information
  accurate as the two repositories evolve.
