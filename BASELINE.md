# Predecessor baseline (NativeConv) — historical record only

Raw benchmark output inherited from the NativeConv project, kept as the
starting-point reference for LogRange. CAUTION: these numbers came from the
predecessor's harness, which showed 8x run-to-run swings on identical
binaries — the failure that made LogRange's measured-noise-floor requirement
a deliverable (see BENCHMARKS.md for trustworthy numbers). Do not cite these.

---

Bulk-step scaling sweeps:
NativeConv Benchmark Summary
Micro (single-run means):
  integrate linear: 3.033 us
  integrate log:    1.053 us (speedup=2.88x)
  converge linear:  0.207 us
  converge log:     1.809 us (speedup=0.11x)
Bulk 32-step (contraction):
  linear: 0.0025 us/step
  log:    0.0264 us/step
Scaling best (contraction plugin):
  Linear: best_us/step=0.0025 at size=192 (max_abs_err=6.000e-01)
  Log: best_us/step=0.0161 at size=16384 (max_abs_err=5.707e-01)
  Pinch: best_us/step=0.0021 at size=6144 (max_abs_err=5.805e-01)
  Relative: Log vs Linear = 0.16x
  Relative: Pinch vs Linear = 1.19x
