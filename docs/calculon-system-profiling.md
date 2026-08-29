# Calculon full-system CPU and memory profiling

> Historical record: this document measures the retired native Calculon SPA
> factories. Current scientific processing uses the Calculon FGN bundle.

This record identifies the hot code and tests page residency for the reference
raw-image-to-ALPAO path. The decisive result is computational: enabling
Calculon's existing OxiBLAS backend reduces the median service time by 65.5%.
Prefaulting, locking, transparent-huge-page advice, and explicit huge pages do
not materially improve warmed service time on the test host.

The measurements support these implementation decisions:

- Build the Calculon SPA plugin with the `oxiblas` feature.
- Construct, initialize, and warm all algorithm state before the real-time
  interval.
- Apply memory locking as an explicit process/deployment policy when protection
  from reclaim is required. Do not add a node-specific memory mechanism.
- Do not require explicit HugeTLB pages for this workload. Reconsider them only
  after a different detector, reconstruction matrix, CPU, or kernel shows a
  measured translation bottleneck.
- Profile the real region map and science matrix before changing centroiding,
  region extraction, or controller storage.

## Measurement boundary

The executable directly dispatches the real SPA nodes in this order:

```text
GRAY16_LE 512x512
  -> pixel calibration
  -> F32 calibrated image
  -> fused SHWFS controller
       4096 tiled 8x8 regions
       8192 slopes
       dense 468x8192 F32 reconstruction storage
       leaky integrator and PDM bounds
  -> ALPAO command normalization
  -> mock ALPAO sink
```

The reconstruction allocation is 15,335,424 bytes. Its synthetic values are
sparse, but the row-major GEMV reads the complete dense allocation. The test is
therefore dimension- and memory-traffic-representative, but it is not a
scientific correctness profile.

The result is a closed-loop, direct-call service-time floor. It excludes
PipeWire scheduler wake-up time, a vendor camera, the ASDK/device boundary, an
open-loop arrival process, overload, and deadline misses. It must not be used
as an end-to-end deployment latency claim.

The main campaign used:

- AMD Ryzen 7 6800H, Linux 6.12.57 with `preempt=full`;
- CPU 14, `SCHED_FIFO` priority 88, no measured CPU migrations;
- an optimized release build with Rust debug information retained for
  callgraphs;
- 256 warm-up cycles;
- 2,000 recorded cycles per cell, three randomized repetitions;
- two GEMV implementations, three allocator page policies, three residency
  policies, and five PMU groups;
- 270 process runs and 540,000 raw latency samples in total. Each PMU group
  reruns the workload, so those samples are not one continuous distribution;
- 100% reported scheduling coverage for every counter group.

The raw latency samples, `perf stat` output, `/proc` snapshots, smaps, build
hashes, source diffs, commands, aggregate tables, and SHA-256 manifests are in
the [memory/PMU record](benchmark-data/calculon-system-memory-pmu-2026-08-25-record/).
Separate [cycle callgraphs](benchmark-data/calculon-system-cycles-2026-08-25-record/),
[cache-fill source counters](benchmark-data/calculon-system-fill-sources-2026-08-25-record/),
and a [locked page-fault callgraph](benchmark-data/calculon-system-page-faults-locked-2026-08-25-record/)
are also retained.

The main record packages its high-volume latency, raw counter, and smaps files
in `raw-artifacts.tar.zst`. The smaller records compress their raw latency CSVs
individually. `RAW_SHA256SUMS` preserves the pre-packaging file hashes, and each
record's `SHA256SUMS` verifies the committed files and archives.

## Hot functions and implemented optimization

The scalar callgraph attributes 83.03% of sampled user cycles to the scalar
`f32` dot product used by `PreparedGemv`. The SPA crate had disabled all
default Calculon features and consequently bypassed Calculon's existing
OxiBLAS backend. Enabling only the `oxiblas` feature selects the existing
AVX2/FMA implementation without changing buffer ownership, graph semantics,
algorithm state, or steady-state allocation.

Both DSOs used the same source and release settings. The scalar baseline was
built with `default-features = false` and no Calculon feature; the candidate
adds `features = ["oxiblas"]`. Exact DSO hashes are recorded in each campaign's
`environment.txt`.

The median of the three core-counter repetitions is:

| Build | Total p50 | Total p99 | Total p99.9 | Cycles/frame | Instructions/frame | IPC |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Scalar, base/default | 3.176 ms | 3.394 ms | 3.663 ms | 13.977 M | 25.242 M | 1.806 |
| OxiBLAS, base/default | 1.096 ms | 1.365 ms | 1.647 ms | 4.826 M | 12.381 M | 2.565 |

OxiBLAS is 2.90 times faster at p50. It retires 51.0% fewer instructions and
uses 65.5% fewer cycles. These are large, consistent effects. The p99.9 values
contain only about two observations per 2,000-sample repetition and are
descriptive, not a release gate.

After the change, the cycle callgraph attributes:

| Path | Sampled user cycles |
| --- | ---: |
| OxiBLAS AVX2 dot product | 46.86% |
| Shack-Hartmann centroiding | 23.32% |
| Fused-controller closure excluding named callees | 13.12% |
| glibc vector copy implementation | 8.07% |
| Pixel-calibration loop | 7.00% |

The copy samples include region extraction. A future candidate can avoid
materializing tiled regions by allowing centroiding to read strided calibrated
image views, but that is an algorithm API change and must be measured with the
real, potentially sparse region map. The dense GEMV remains the first target.
The source-fill counters show that OxiBLAS obtains a median 6.22 MB/frame from
local DRAM or I/O versus 1.34 MB/frame for the much slower scalar loop. This
shows that the faster implementation exposes substantially more memory demand;
it does not by itself prove that the stage is fully memory-bandwidth-bound.

## Prefaulting and locking

The three residency policies are:

- `default`: allocate, initialize, and warm normally;
- `prefault`: call `mlockall(MCL_CURRENT)` after construction and then
  `munlockall()`, providing a one-time residency pass without retaining locks;
- `locked`: call `mlockall(MCL_CURRENT | MCL_FUTURE)` before construction and
  retain the lock through the measured interval.

For OxiBLAS with the base allocator policy:

| Residency | Total p50 | Total p99.9 | Cycles/frame | Resident/locked mapping evidence |
| --- | ---: | ---: | ---: | --- |
| Default | 1.096 ms | 1.647 ms | 4.826 M | 56,856 KiB RSS; 0 KiB locked |
| Prefault | 1.098 ms | 1.620 ms | 4.836 M | 57,872 KiB RSS; lock released |
| Locked | 1.086 ms | 1.558 ms | 4.768 M | 57,872 KiB RSS; 55,748 KiB `Locked` |

The locked p50 is 0.85% below default, which is too small to distinguish from
run-to-run and counter perturbation. Locking is therefore not a throughput
optimization in this warmed test. Its purpose is to limit reclaim and swap
risk under deployment pressure, which this campaign did not create.

Every default-policy first cycle reported one minor fault. Every prefaulted or
locked first cycle reported zero. First-cycle service time did not improve
consistently, so the predictable fault was cheap on this host.

The warmed fault-counter group was not uniformly zero. Three of 54 cells
reported minor faults: 17, 2, and 27 respectively, with the 27-fault cell using
the locked policy. A follow-up locked OxiBLAS run counted approximately 35
fault events over 50,000 cycles (0.0007/frame). Its sampled fault instructions
were in matrix reads, calibrated-image accesses, and region copies. There were
no major faults. This evidence means:

- initialization, touching, warm-up, and locking remove the deterministic
  first-use fault;
- `mlockall` did not make this synthetic run observably fault-free;
- the data do not distinguish reclaim, copy-on-write, or other minor-fault
  causes, and no memory-pressure experiment was performed;
- a strict deployment must measure faults in the real daemon/client process
  and workload rather than infer a zero-fault guarantee from `mlockall`.

PipeWire already exposes process-level `mem.mlock-all = true` and buffer-level
`mem.allow-mlock = true`. A strict deployment should configure the appropriate
process, reserve and verify `RLIMIT_MEMLOCK`, confirm that locking succeeded,
finish all large allocations before entering the real-time interval, and keep
allocation out of warmed node callbacks. The benchmark host had a 4 GiB
memlock limit; this is not a portable assumption.

## Transparent and explicit huge pages

The page-policy labels describe the glibc allocator request:

| Label | `glibc.malloc.hugetlb` | Requested behavior |
| --- | ---: | --- |
| `base` | 0 | Disable allocator HugeTLB assistance. |
| `thp` | 1 | Apply transparent-huge-page advice to eligible allocations. |
| `hugetlb` | 2 | Request explicit HugeTLB-backed allocation. |

These meanings follow the
[glibc memory-allocation tunable documentation](https://sourceware.org/glibc/manual/latest/html_node/Memory-Allocation-Tunables.html).
`base` does not guarantee 4 KiB mappings because the kernel can still apply
transparent huge pages.

The host used an `always` transparent-huge-page policy and had 1,024 free
reserved 2 MiB HugeTLB pages. Smaps confirms the actual allocation:

| Policy, OxiBLAS/default residency | RSS | `AnonHugePages` | `Private_Hugetlb` |
| --- | ---: | ---: | ---: |
| Base | 56,856 KiB | 38,912 KiB | 0 KiB |
| THP advice | 56,880 KiB | 38,912 KiB | 0 KiB |
| Explicit HugeTLB | 2,632 KiB | 0 KiB | 38,912 KiB |

HugeTLB pages have separate accounting, so the 2,632 KiB RSS value is not the
complete working-set size. Base and THP-advice runs already obtained the same
38 MiB of anonymous huge pages; the advice could not improve that mapping.

Median core-counter results with default residency are:

| Build | Base p50 | THP-advice p50 | Explicit-HugeTLB p50 |
| --- | ---: | ---: | ---: |
| Scalar | 3.176 ms | 3.186 ms | 3.217 ms |
| OxiBLAS | 1.096 ms | 1.103 ms | 1.106 ms |

For OxiBLAS, explicit HugeTLB reduces median L2 DTLB misses from 102.74 to
0.52/frame, a 99.5% reduction, but increases p50 by 0.91%. The scalar result is
1.27% slower. Page translation is therefore not a limiting cost in this
configuration. Explicit HugeTLB also reserves non-pageable memory, complicates
capacity planning and failure handling, and produces no measured service-time
benefit here.

## Reproduction

Configure an optimized build with the Calculon and mock ALPAO plugins, retain
Rust debug information, and run:

```sh
CARGO_PROFILE_RELEASE_DEBUG=1 meson compile -C build-profile

scripts/profile-calculon-system.py \
  build-profile/spa/plugins/calculon/spa-calculon-full-system-test \
  build-profile/spa/plugins/alpao/libspa-alpao-mock.so \
  OUTPUT_DIRECTORY \
  --implementation oxiblas=build-profile/spa/plugins/calculon/libspa-calculon.so \
  --repetitions 3 --samples 2000 --warmup 256 \
  --cpu 14 --rt-priority 88 \
  --group core --group l1d --group l2 --group dtlb --group faults
```

The script refuses to overwrite a nonempty output directory, validates raw
sample identities and PMU scheduling coverage, records the exact mappings and
environment, and writes a SHA-256 manifest. Named AMD events are host-specific;
the runner should fail rather than silently substitute or multiplex counters on
an unsupported CPU.

## Validation

The optimized configuration passes:

```sh
meson test -C build-profile --print-errorlogs

PKG_CONFIG_PATH=/path/to/pipewire/build/meson-uninstalled \
  cargo test --workspace --release --locked

cargo test --manifest-path ../calculon-algorithms/Cargo.toml \
  --workspace --release --locked
```

The Meson suite includes the configurable full-system path and its warmed
zero-allocation assertion. The Calculon algorithm suite includes an OxiBLAS
row-major F32 reconstruction check against the scalar reference. All commands
passed on the recorded host.

## Remaining qualification

Before treating any number as an RTC release gate, repeat the measurement with:

- the deployed detector extent, region map, thresholds, and dense science
  matrix values;
- the real PipeWire scheduler, chosen poll/event readiness mode, and graph
  topology;
- the actual camera and ALPAO boundary;
- open-loop fixed-rate traffic at the required cadence, bursts, and overload;
- the final CPU isolation, IRQ placement, frequency, RT priority, and memory
  configuration;
- deliberate observer stalls and competing memory/CPU load;
- enough samples and independent runs to support the required p99.9/max claim.

This campaign identifies an implementation win and rules out huge pages as an
immediate optimization on one host. It does not establish a worst-case bound.
