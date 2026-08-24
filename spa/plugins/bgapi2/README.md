# BGAPI2 SPA plugin

This optional plugin is the native PipeWireAO integration for GenICam
producers supported by Baumer GAPI. Enable it with `-Dbgapi2=enabled`; use
`-Dbgapi2-prefix=PATH` when the SDK is not installed under
`/opt/baumer-gapi-sdk-c`. A disabled build does not inspect or include the
proprietary SDK.

## Implemented slice

The `api.bgapi2.source` factory provides complete-frame capture with:

- an explicit GenTL producer path and optional interface, device, and stream
  indices;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
- immutable `SPA_NODE_FLAG_RTC_PROCESS` ownership;
- `SPA_IO_BuffersLatestLink` fan-out without graph-ready callbacks;
- mapped `MemPtr` or `MemFd` buffers announced directly to BGAPI2, with no
  image copy;
- `Mono8` and unpacked `Mono10`, `Mono12`, `Mono14`, and `Mono16` formats;
- dynamic GenICam Boolean, Integer, Float, Enumeration, and String controls
  through `SPA_PARAM_PropInfo` and `SPA_PARAM_Props`;
- fixed `SPA_META_Header` and initialized Version 1 `SPA_META_Acquisition`
  metadata; and
- synchronous acquisition stop and event-thread shutdown before pool teardown.

The required factory property is:

```text
api.bgapi2.producer=/absolute/path/to/producer.cti
```

The optional `api.bgapi2.interface-index`, `api.bgapi2.device-index`, and
`api.bgapi2.stream-index` properties select a device. By default the source
searches all interfaces and selects device and stream zero. The actual indices,
model, serial number, and producer path are published as node properties.

The plugin discovers scalar controls from the remote GenICam NodeMap instead of
maintaining a camera-specific list. Properties use canonical names such as
`genicam.ExposureTime` and `genicam.PixelFormat`; enumeration labels, numeric
ranges, descriptions, and current access state come from the camera. A write
contains exactly one typed property and is accepted only while acquisition is
stopped. Layout-changing controls also require all buffers to be released and
invalidate format and buffer negotiation. GenICam command nodes are not exposed
as persistent SPA properties.

The plugin uses `spa_image_source`, `spa_image_source_latest`, and
`spa_buffer_latest` directly. It has no `pw_stream`, libpipewire client,
private image pool, payload copy, or graph scheduling path.

The SPA node and transport adapter are C. `camera.cpp` is a narrow C++
containment boundary because BGAPI2 can propagate C++ GenApi exceptions through
its nominal C API. Every vendor call is caught before it can unwind through C;
the adapter otherwise exposes a C interface and does not use BGAPI2's C++ object
model.

## Completion ownership

BGAPI2's new-buffer event handler runs on a vendor-owned thread. It reads the
completed buffer's owner and dynamic frame metadata, then publishes one
fixed-size completion descriptor through SPA's cache-line-isolated SPSC ring.
The PipeWireAO RTC data loop is the sole consumer and remains responsible for
validating and publishing the frame.

The source intentionally has no direct-polling or progressive profile. The
camera-adapter benchmark retains timeout-zero `GetFilledBuffer` only as a
diagnostic comparison; both tested producers build error details on empty
polls, and Euresys allocates on that path.

The ring contains at most the 64 buffers allowed by `spa_image_source`; overflow
is a fatal acquisition error, not a lossy overwrite. An overflow would mean the
vendor delivered more unique completions than the announced pool can contain.
The event handler is installed at Start and synchronously removed after
acquisition stops on Pause or Suspend. Restart creates an empty completion
queue before the handler is enabled again.

Returning a subscriber lease still calls `BGAPI2_DataStream_QueueBuffer` from
the RTC owner. This preserves direct buffer ownership and avoids another bridge
thread, but the GenTL producer's queue implementation is part of the real-time
contract. Each process duty publishes one ready completion before returning
released leases. Queue latency therefore consumes future pool headroom instead
of preceding data that was already ready when the duty began.

## Timing and metadata

`SPA_META_Header.seq` uses the GenTL frame ID and marks a discontinuity when the
sequence is not consecutive. `SPA_META_Header.pts` is the local
`CLOCK_MONOTONIC` time immediately before publication. Acquisition metadata is
valid and initialized, but this version does not claim an exposure-start time,
hardware acquisition identity, clock mapping, or uncertainty.

## Qualification

The factory test verifies load and parameter validation without opening a
camera. Camera and source tests use four or eight external host buffers,
respectively. The source test captures ten frames, returns every subscriber
lease, pauses and restarts halfway through the run, and performs ordered
teardown.

On 2026-08-23 the factory, camera, and complete-frame source tests passed
against the connected 640x480 Mono8 GE34GM camera with the BGAPI2 2.16.1
runtime through both:

```text
/opt/euresys/egrabber/lib/x86_64/gigelink.cti
/opt/baumer-gapi-sdk-c/lib/libbgapi2_gige.cti
```

The second result is significant: the Baumer producer does operate this camera.
An earlier failure was caused by an uncaught GenApi exception from the Euresys
producer, which terminated the shared process before the Baumer case ran.

The separately installed BGAPI2 2.16.1 C and C++ packages contain byte-identical
`libbgapi2_genicam`, `libbgapi2_img`, and Baumer GigE CTI binaries. Both
`BGAPI2_DataStream_GetFilledBuffer` and the C++
`DataStreamEventControl::GetFilledBuffer` ultimately call the same internal
`CDataStreamObj::getFilledBuffer`; the C entry point is a direct checked tail
call, while the C++ entry point adds object guards, RTTI, and exception
translation. The C++ package therefore does not provide a different producer or
completion engine, and changing facades would not address producer allocations
or lifecycle behavior.

A ten-frame closed-loop `heaptrack` experiment compared three completion
strategies:

- timeout-zero `GetFilledBuffer` constructed producer error strings on every
  empty poll;
- `GetNumAwaitDelivery` correctly identified the output queue, but Euresys
  allocated twice in `DSGetInfo` per query, while Baumer did not; and
- the event-handler/SPSC implementation made no allocation under
  `bgapi2_camera_try_get_completion`, made no polling `GetFilledBuffer` or
  `GetNumAwaitDelivery` calls, and made no allocation in the SPSC callback
  handoff itself.

Euresys still allocated while the callback queried metadata for each actual
frame. Those calls now run on the vendor event thread. Both producers made an
allocation-bearing `DSQueueBuffer` call for each of the nine returned leases.
The source is therefore qualified for allocation-free empty RTC polling, but it
is not qualified for a strict zero-allocation BusySpin process path.

The experiment used a debug-optimized build without LTO. Whole-process totals
include SDK loading, camera discovery, XML parsing, and test setup, so they are
not frame-latency measurements. Tail latency and scheduling jitter still require
an open-loop, timestamped hardware run with CPU affinity, warmup, histograms,
and causal scheduler counters.

The completion microbenchmark removes setup and delivered frames from the
reported empty-dequeue samples. It first waits for and returns one frame, then
records 200,000 calls with `CLOCK_MONOTONIC_RAW` around each operation. The
back-to-back clock-pair distribution is reported beside the operation rather
than subtracted. Exact samples are sorted after acquisition stops; this build
does not require a C HdrHistogram dependency.

Three runs per producer and profile on CPU 15 with BGAPI2 2.16.1 produced:

| Producer | Profile | p50 | p99 | p99.9 |
| --- | --- | ---: | ---: | ---: |
| Euresys Gigelink | callback | 20 ns | 31 ns | 31 ns |
| Euresys Gigelink | polling | 2.41–2.49 us | 2.54–4.27 us | 10.0–11.4 us |
| Baumer GigE | callback | 20 ns | 31 ns | 31 ns |
| Baumer GigE | polling | 200–211 ns | 211–251 ns | 360–410 ns |

The clock-pair baseline was 20 ns p50 and 30 ns p99, so the callback result is
at this harness's measurement floor. These are not production qualification
numbers: the host was an AMD Ryzen 7 6800H running Linux 6.12.57 with
`preempt=full`, the `powersave` governor, SMT enabled, and no isolated core.
They do establish that callback empty-dequeue is negligible compared with
timeout-zero BGAPI2 polling on both producers.

The operation benchmark also measures `BGAPI2_DataStream_QueueBuffer` using
actual completed buffers. It waits for each completion outside the timed
boundary, performs 32 untimed frame/requeue warmups, and then records the queue
call without subtracting the clock-pair baseline. Three independent 1,000-frame
runs per producer on CPU 15 produced:

| Producer | p50 range | p99 range | Maximum range |
| --- | ---: | ---: | ---: |
| Euresys Gigelink | 1.13–1.18 us | 4.35–4.82 us | 8.12–11.0 us |
| Baumer GigE | 0.76–1.10 us | 3.43–8.21 us | 6.72–17.4 us |

The clock-pair baseline was 20 ns p50 and 30 ns p99. A 1,000-sample run does
not support a stable p99.9 claim, so the table reports p99 and the observed
maximum. The central cost is small compared with a 1 ms frame period, while the
run-to-run movement in the Baumer tail confirms that the producer call is not a
strict deterministic primitive. These service-time results do not include
completion waiting and do not replace open-loop end-to-end qualification. They
also do not justify adding a requeue handoff: a helper could move allocation off
the RTC owner, but it would add scheduling latency and another bounded queue
without removing the producer work.

Heaptrack attributes no allocation to callback-mode
`bgapi2_camera_try_get_completion` for either producer. Polling calls
`GetLastTLError` on every empty dequeue. Euresys reaches `GCGetLastError`; Baumer
reaches its CTI `GetLastError`; both construct `std::string` objects and allocate.
Under heaptrack the Baumer polling source made millions of allocations and
could not deliver ten frames before the three-second test deadline. Its faster
unprofiled polling time therefore does not make it a strict RTC path.

Repeated-process testing also found a Baumer CTI lifecycle nonconformance. After
one Baumer-only camera test closes cleanly, the next Baumer-only process opens
and starts the camera but receives no buffer. Every adapter stop, discard,
revoke, stream close, device close, interface close, system close, and release
call reports success. One Euresys Gigelink capture restores stream delivery, and
the next Baumer run then succeeds. The failure remains reproducible with the
2.16.1 runtime and Baumer CTI. This behavior is recorded as producer evidence;
the plugin does not add a vendor-specific stream-reset workaround.

The profiling commands were:

```console
heaptrack -o /tmp/bgapi2-euresys \
  build/spa/plugins/bgapi2/spa-bgapi2-capture-test \
  build/spa/plugins/bgapi2/libspa-bgapi2.so \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti
heaptrack_print /tmp/bgapi2-euresys.zst \
  -F /tmp/bgapi2-euresys.stacks --flamegraph-cost-type allocations
```

Repeat the same commands with the Baumer CTI to compare producers.

Run the operation benchmark with:

```console
taskset -c 15 build/spa/plugins/bgapi2/spa-bgapi2-completion-benchmark \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti callback 200000
taskset -c 15 build/spa/plugins/bgapi2/spa-bgapi2-completion-benchmark \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti polling 200000
taskset -c 15 build/spa/plugins/bgapi2/spa-bgapi2-completion-benchmark \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti queue 1000
```

## Remaining work

- Add manager and device factories for live camera discovery and reconciliation.
- Map a hardware or producer timestamp into the PipeWireAO acquisition clock
  contract before claiming exposure timing.
- Extend pixel-format coverage where a deterministic direct SPA mapping exists.
- Run open-loop latency and tail-jitter qualification at the intended camera
  rates and scheduling profiles.
- Resolve or formally exclude the Baumer CTI repeated-process stream failure
  before claiming that producer for unattended lifecycle operation.
- Reconsider a dedicated SDK-owning requeue agent only if open-loop end-to-end
  evidence shows that producer queue tails violate the deadline. The current
  service-time evidence does not justify its extra handoff and scheduling cost.
