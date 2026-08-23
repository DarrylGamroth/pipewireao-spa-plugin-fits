# ALPAO capture-interface benchmark

Status: host software qualification implemented; physical-device timing remains
unverified

## Question and boundary

The benchmark answers this question:

> How long does a 468-actuator normalized command take to pass through the
> ALPAO SPA sink and ASDK's calibration and packing path on this host?

It runs the production `api.alpao.sink` ASDK backend with the host-only
`libait_capture.so` interface from `alpao-binary-config`. The harness records
`CLOCK_MONOTONIC` immediately before and after `spa_node_process()`. The
capture interface records the same clock after ASDK has produced the logical
command words and immediately before its one nonblocking `sendmsg`.

The reported boundaries are:

| Metric | Start | End | Included |
| --- | --- | --- | --- |
| `service` | Actual SPA process entry | SPA process return | SPA validation, C wrapper, ASDK calibration and packing, capture publication, and unwind. |
| `process_to_capture` | Actual SPA process entry | Capture timestamp | SPA validation, C wrapper, ASDK calibration and packing, and capture-header preparation; excludes capture `sendmsg`. |
| `schedule_lateness` | Scheduled arrival | Actual SPA process entry | Generator and host scheduling delay. |
| `scheduled_to_capture` | Scheduled arrival | Capture timestamp | Scheduling plus the software path to the interface boundary. |

This boundary does not include PEX, Ethernet, Gigabit Ethernet, electronics,
DAC conversion, mirror settling, or optical response. The capture interface
accepts `daqFreq`, but does not emulate its physical timing. A capture-backed
frequency sweep must not be described as an interface-speed measurement.

## Build

Build the capture interface first:

```console
make -C /path/to/alpao-binary-config/interfaces/capture
```

Configure an optimized benchmark build. The capture integration remains
optional and ordinary SDK-disabled builds do not inspect either external tree.

```console
meson setup build-capture --buildtype=release \
  -Dalpao-sdk=enabled \
  -Dalpao-sdk-root=/path/to/alpao \
  -Dalpao-binary-config=/path/to/alpao-binary-config/tools/alpao_binary_config.py \
  -Dalpao-capture-plugin-dir=/path/to/alpao-binary-config/interfaces/capture/build
meson compile -C build-capture
```

The build generates an isolated `CAP468` identity calibration and matching
capture `.acfg` in its private build directory. It does not copy or modify a
physical mirror configuration.

Run the short correctness integration test and the closed-loop benchmark:

```console
meson test -C build-capture spa-alpao-asdk-capture --print-errorlogs
meson test -C build-capture --benchmark \
  spa-alpao-asdk-capture-latency --verbose
```

The integration test requires one structurally valid capture record for every
measured SPA command. Sequence gaps, malformed packets, changed frame sizes,
timestamps outside their associated SPA call, or receiver errors fail the test.

## Fixed-rate and real-time runs

The executable supports a schedule-preserving open-loop arrival model. A late
command does not move later deadlines, so stalls appear as scheduler lateness
and deadline misses instead of reducing the offered rate.

Set `ACECFG` to the directory containing the generated `CAP468` pair and put
ASDK plus the capture interface on `LD_LIBRARY_PATH`. For example, a 20 kHz run
with separate physical cores is:

```console
export ACECFG="$PWD/build-capture/spa/plugins/alpao/"
export LD_LIBRARY_PATH="/path/to/alpao/Linux/Lib/x64:/path/to/alpao-binary-config/interfaces/capture/build"

build-capture/spa/plugins/alpao/spa-alpao-capture-benchmark \
  --warmup 10000 --samples 100000 --period-ns 50000 \
  --producer-cpu 4 --collector-cpu 6 --rt-priority 80 --mlock \
  --raw-output capture-20khz.csv \
  build-capture/spa/plugins/alpao/libspa-alpao.so
```

Select CPUs from the process's allowed affinity and prefer different physical
cores. The capture receiver has no producer dependency because publication is
nonblocking, so it remains non-real-time. A requested affinity, memory lock, or
`SCHED_FIFO` setting fails explicitly when the host does not permit it.
The producer returns to `SCHED_OTHER` immediately after the measured loop,
before SDK shutdown, sorting, reporting, or CSV output.

`--burst-size N` schedules `N` arrivals at the same deadline and spaces bursts
by `N * period-ns`, preserving the average offered rate. Use `--allow-drops`
only for intentional saturation or overload experiments; the report still
counts missing capture frames.

## Workload matrix

Use the same optimized build, seed, placement, and environment for comparisons.
Repeat each row independently and retain its CSV.

| Region | Example arguments | Purpose |
| --- | --- | --- |
| First use | Always reported separately | Page faults and cold ASDK state. |
| Closed loop | `--period-ns 0` | Minimum concurrency-one service time. |
| Light | `--period-ns 1000000` | Wake-up and scheduler baseline at 1 kHz. |
| Target | Deployment command period | Validate the intended cadence. |
| Burst | `--period-ns 50000 --burst-size 8` | Transient headroom at a 20 kHz average rate. |
| Near saturation | Period just above closed-loop p99 | Locate tail growth. |
| Overload | Period below sustainable service time with `--allow-drops` | Deadline and observer-loss behavior. |
| Recovery | Repeat the target row after overload | Confirm tails and correctness recover. |

The harness reports p50, p90, p99, p99.9 only when the sample count supports
them, plus maximum, first-use latency, clock-read floor, deadline misses,
capture packets, and missing frames. `--raw-output` writes per-sample timestamps
after timing completes and refuses to replace an existing file.

Record the source revisions and dirty state of PipeWireAO, this repository,
`alpao-binary-config`, and the SDK artifact alongside the raw CSV. Also retain
the exact command, compiler and optimization status printed by the harness,
CPU topology and affinity, kernel, CPU governor, and competing load. Shared CI
is appropriate for correctness and gross-regression checks, but latency gates
require a controlled host and repeated-run dispersion.
