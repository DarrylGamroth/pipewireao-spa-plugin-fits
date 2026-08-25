# Reference Shack-Hartmann to ALPAO system

This repository contains an executable vertical slice from raw detector pixels
to an ALPAO deformable-mirror command. It is a reference integration and
latency oracle, not a telescope-specific control configuration.

```text
GRAY16_LE detector frame
  -> ordinary scheduled buffer
  -> api.calculon.pixel-calibration
  -> org.calculon.ao.calibrated-pixels/1 F32 [height, width]
  -> api.calculon.shwfs-controller
       RegionExtractionPlan
       ShackHartmannPlan
       PreparedGemv
       LeakyIntegratorPlan
       PdmCommandPlan
  -> org.calculon.ao.demanded-pdm-command/1 F32 [actuator]
  -> api.alpao.command-normalization
  -> org.pipewireao.alpao.normalized-actuator-command/1 F64 [actuator]
  -> ordinary scheduled buffer
  -> api.alpao.sink
  -> ASDK
```

The controller is deliberately fused. Region pixels, slopes, reconstruction
results, and controller state are private storage, not intra-node SPA ports.
This gives one transactional state-commit boundary and permits a future fused
backend without changing the public port schemas. Separate diagnostic
factories can expose region pixels and slopes when a graph requires those
observables; they are not required in the strict control island.

## Immutable construction configuration

`api.calculon.shwfs-controller` requires:

| Key | Meaning |
|---|---|
| `api.calculon.detector-size` | `widthxheight` detector extent. |
| `api.calculon.detector-rate` | Positive `numerator/denominator` sample rate. |
| `api.calculon.detector-profile` | Exact lowercase SHA-256 interpretation profile. |
| `api.calculon.region-size` | Common `widthxheight` subaperture extent. |
| `api.calculon.region-origins` | Semicolon-separated zero-based `row,column` origins in semantic subaperture order. |
| `api.calculon.actuator-count` | Positive physical-DM actuator extent. |
| `api.calculon.reconstruction-matrix-path` | Raw little-endian F32 row-major matrix with shape `[actuator_count, 2 * region_count]`. |
| `api.calculon.coordinate-scale` | Positive coordinate unit per detector pixel. |
| `api.calculon.pixel-threshold` | Nonnegative absolute pixel threshold. |
| `api.calculon.flux-threshold` | Nonnegative subaperture flux threshold. |
| `api.calculon.controller-gain` | Finite leaky-integrator gain. |
| `api.calculon.controller-pole` | Finite pole in `[0, 1]`. |
| `api.calculon.command-minimum` | Inclusive physical-command lower limit. |
| `api.calculon.command-maximum` | Inclusive physical-command upper limit. |

The matrix is read and validated during factory construction. All numerical
plans, state, scratch, and buffer-registration state are prepared before
`Start`; warmed pixel calibration, control, and normalization callbacks are
verified to perform no heap allocation.

`api.alpao.command-normalization` requires the detector rate, ALPAO profile,
actuator count, and `api.alpao.command-scale`. Version 1 uses the explicit
reference conversion `normalized = demanded / command-scale` and rejects a
non-finite or out-of-range result. A deployed mirror profile should replace
the scalar scale with its calibrated profile-bound conversion artifact.

## Correctness and failure checks

`spa-calculon-full-system` dynamically loads all three processing factories and
the ALPAO sink. Each adjacent pair receives one format produced by SPA format
intersection, so schema, profile, element type, shape, layout, and rate are
link constraints rather than parallel assumptions. It then shares SPA buffers
between the negotiated ports and drives this sequence:

1. An asymmetric two-subaperture GRAY16 frame crosses an ordinary buffer link
   and is calibrated.
2. The fused controller extracts regions, measures nonzero slopes,
   reconstructs eight physical commands, integrates them, and applies
   actuator bounds.
3. The ALPAO adapter converts the physical F32 vector to the exact F64 ALPAO
   schema and profile.
4. The vector crosses an ordinary buffer link and is accepted by the ALPAO mock
   sink.
5. A warmed algorithm cycle is checked for zero heap allocations.

The same executable has also been run against ASDK's `SIM001` simulator. The
mock test remains the deterministic CI gate; simulator and capture execution
are environment-dependent qualification gates.

`spa-fits-calculon-ingress` supplies a scientist-facing source-boundary test.
It creates a two-frame FITS image cube, selects the source's
`video/raw GRAY16_LE` alternative, intersects it with the pixel-calibration
input constraint, crosses an ordinary buffer link, and verifies the calibrated
pixels. FITS is used for functional graph assembly; the synthetic in-memory
publisher remains the latency source so file access is not folded into the
control-path floor.

## Latency baseline

The Meson benchmark `spa-calculon-full-system-latency` is a closed-loop,
explicitly dispatched service-time baseline. It reports distributions for:

- ordinary source publication plus the pixel-calibration,
  fused-controller, and normalization SPA callbacks;
- ordinary buffer submission plus ALPAO sink/backend completion;
- the complete raw-frame-to-backend path.

It intentionally bypasses the regular PipeWire scheduler. Therefore it
measures a direct-call lower bound while preserving attribution between
ordinary ownership, SPA/Calculon work, and the device boundary. It is
not yet a scheduler-composed graph measurement. Scheduler, overload, and
open-loop deadline tests are separate experiments and must not be inferred
from this result.

Run the deterministic mock benchmark with:

```sh
meson test -C build-calculon --benchmark \
  spa-calculon-full-system-latency --print-errorlogs
```

Run the ASDK simulator version by setting `ACECFG` to the directory containing
`SIM001` and `SIM001.acfg`, adding the ASDK library directory to
`LD_LIBRARY_PATH`, and invoking `spa-calculon-full-system-test` with the
Calculon DSO, ALPAO ASDK DSO, `asdk`, and a positive sample count.

For the 468-actuator capture interface, use the existing
`spa-alpao-asdk-capture-latency` benchmark first. A representative 468-actuator
science matrix and detector/subaperture profile are required before claiming a
full 468-actuator scientific-system latency result.

## Dimension-representative CPU and memory profile

On 2026-08-25, the reference harness was expanded to run a synthetic 512x512
detector, 4096 tiled 8x8 regions, 8192 slopes, 468 actuators, and a dense
15,335,424-byte reconstruction allocation. A 270-cell PMU campaign compared
the scalar and OxiBLAS GEMV paths across default, prefaulted, and locked memory
and base, transparent, and explicit-huge-page allocator policies.

The scalar dot product consumed 83.03% of sampled cycles. Enabling Calculon's
existing OxiBLAS feature reduced median raw-frame-to-mock-sink service time
from 3.176 ms to 1.096 ms, a 2.90x speedup. Prefaulting and locking did not
materially change warmed service time. Explicit HugeTLB pages reduced L2 DTLB
misses by 99.5% but did not improve latency; the host's base policy already
gave the large allocation 38 MiB of transparent huge pages.

See [Calculon full-system CPU and memory profiling](calculon-system-profiling.md)
for the method, raw evidence, callgraphs, page-fault caveat, and deployment
recommendations. This remains a direct-call synthetic service-time result, not
a scheduler, device, scientific-validity, or deadline qualification.

## Local plumbing snapshot

On 2026-08-24, a release build on an AMD Ryzen 7 6800H running Linux
6.12.57 was exercised without scheduler or CPU-affinity controls. The workload
was intentionally small: a 4x2 image, two regions, four slopes, and a sparse
468-by-4 reconstruction matrix. These results qualify the plumbing only.

| Backend and samples | Algorithms p50 / p99 / p99.9 | Boundary p50 / p99 / p99.9 | Total p50 / p99 / p99.9 |
|---|---|---|---|
| Mock, 10,000 | 3.687 / 3.797 / 7.194 us | 0.361 / 0.371 / 0.421 us | 4.048 / 4.158 / 7.775 us |
| ASDK capture, 1,000 | 4.047 / 8.937 / 12.734 us | 28.163 / 61.525 / 66.785 us | 32.220 / 70.512 / 79.519 us |

The capture run received all 1,005 interface packets: 1,000 measured commands
plus five lifecycle packets. The ASDK boundary dominates this synthetic path;
the result does not predict a representative SHWFS reconstruction cost or an
open-loop deadline-miss rate.
