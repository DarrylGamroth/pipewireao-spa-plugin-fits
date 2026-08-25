# eGrabber SPA plugin

This optional plugin is the native PipeWireAO integration for Euresys
eGrabber. Enable it with `-Degrabber=enabled`; use `-Degrabber-prefix=PATH`
when the SDK is not installed under `/opt/euresys/egrabber`. A disabled build
does not inspect or include the proprietary SDK.

`api.egrabber.producer` accepts the Euresys `gigelink`, `grablink`, and
`coaxlink` aliases or a CTI filesystem path passed to `EGenTL`. The loader is
therefore not syntactically limited to Euresys CTIs, but this plugin supports
and qualifies only the Euresys producers. Loading a third-party GenTL producer
does not imply that the eGrabber facade can open its devices. Progressive
operation specifically requires the Euresys Grablink or Coaxlink producer and
its custom `StartOfCameraReadout` event contract.

## Implemented slice

The `api.egrabber.source` factory currently provides complete-frame capture and
mapped-host progressive publication:

- `api.egrabber.enum.manager` one-shot startup discovery and one
  `api.egrabber.device` object per discovered camera;
- standard device-to-source object creation with stable selector, vendor,
  model, serial, user-ID, and transport properties;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
- explicit `Mono8` and little-endian `Mono10`, `Mono12`, `Mono14`, and
  `Mono16` format support without starting acquisition during source
  construction;
- typed scalar GenICam discovery and readback through `SPA_PARAM_PropInfo` and
  `SPA_PARAM_Props`;
- optional Grablink Camera Link control through a standard CLProtocol provider
  and GenApi C node map, including a distinct attached-camera serial check;
- `node.driver=true` with immutable `SPA_NODE_FLAG_POLL_DRIVER` execution on a
  configured busy-spin data loop;
- retained `SPA_IO_BuffersLatestLink` leases at the exceptional progressive
  camera boundary, with one regular graph cycle for each publication quantum;
- mapped `MemPtr` or `MemFd` buffers announced directly to eGrabber;
- optional StartOfCameraReadout progressive publication on Grablink and
  Coaxlink, with configurable fixed-row release publication from
  `BUFFER_INFO_SIZE_FILLED`, immutable active metadata, and explicit complete
  or aborted terminal state;
- optional complete-frame DMA-BUF announcement when both eGrabber and a local
  DRM render node support it, using negotiated `SPA_META_SyncTimeline` acquire
  and release points rather than implicit synchronization;
- fixed `SPA_META_Header` and Version 1 `SPA_META_Acquisition` publication;
- monotonic `SPA_META_Header.pts` mapping from the vendor camera timestamp,
  reset on every Start and marked discontinuous when the camera clock resets or
  departs from the local monotonic anchor;
- configurable acquisition domain, initial generation, and
  StartOfCameraReadout context selection for Grablink/Coaxlink sources; the
  source advances generation on restart or a non-increasing trigger sequence
  and mirrors a valid acquisition sequence into `SPA_META_Header.seq`;
- a fixed-capacity submission-order tracker that identifies the expected
  acquiring buffer in O(1), verifies completion order, and explicitly reclaims
  only an unclaimed visible submission when the camera has no queued buffer;
  and
- synchronous camera stop and buffer release before pool teardown.

The plugin uses `spa_image_source`, `spa_image_source_latest`, and
`spa_buffer_latest` directly. It has no `pw_stream`, libpipewire client,
private mailbox, payload copy, private capture thread, or private RTC
scheduler. The regular PipeWire scheduler orders its consumers. The optional
manager completes and releases its discovery objects before any source begins
acquisition.

`api.egrabber.progressive-rows` defaults to one. It must be positive and divide
the camera height. One source process call exposes at most one such row quantum,
even when DMA has filled farther. The normal frame stride remains one camera
row; only the progressive commit granularity is multiplied by this property.

The camera, control-backend, frame-layout, frame-sequence, pixel-format, and
optional CLProtocol code was migrated from the sibling `egrabber-pipewire`
implementation. The standalone application remains the behavior oracle until
the plugin reaches hardware parity. See
[eGrabber Camera Link control](../../../docs/egrabber-clprotocol.md) for the
build, property, identity, and qualification contract. `CallbackOnDemand`
dispatches synchronously from `process()`; callback
installation and removal occur only while the node is stopped, so the migrated
event bridge has no callback mutex and does not copy `std::function` callbacks
per event. The node tracks submitted buffers locally instead of querying the
SDK stream count. The tracker follows the vendor FIFO acquisition contract:
initial announcement and later recycling append one slot, completion must
consume the head, and StartOfCameraReadout probes only that head for progress.

Control writes are serialized on the SPA control path and are never performed
by `process()`. Version 1 accepts one scalar write per `SPA_PARAM_Props` object.
The node must be paused for every write. A layout-changing write also requires
the output pool to be released; after it succeeds, the old Format is
invalidated and EnumFormat, Format, and Buffers are marked serial so the host
must renegotiate before the next Start. Writes while running or while a layout
is still bound return `-EBUSY` without changing the camera.

## Current qualification

The factory test loads and enumerates the plugin without opening hardware. A
source retains `EGrabberDiscovery` only until its selected `EGrabber` has been
constructed; it does not retain exclusive discovery-list authority or capture
a probe frame during initialization. The device test discovers the connected
producer, verifies that `sync()` is only a completion barrier and emits no
duplicate object, and checks the standard manager-to-device-to-node property
chain; it skips when no camera is present. Manager construction performs one
synchronous startup inventory. It retains no discovery thread, timer, or SDK
discovery object afterward, and it does not rescan while sources are active.
The capture test skips when no selected camera is available. With the connected
Gigelink camera it
negotiates the live format, announces eight aligned
PipeWireAO-owned buffers, captures ten frames, validates Acquisition metadata,
returns every subscriber lease, and performs ordered teardown.

The capture test also accepts an optional CTI path for explicit producer
qualification. On 2026-08-23, eGrabber 26.06.1.23 loaded Baumer's BGAPI2 2.16.1
GigE Vision producer at
`/opt/baumer-gapi-sdk-c/lib/libbgapi2_gige.cti`, and enumerated its two
interfaces, but `EGrabberDiscovery` returned zero grabbers and zero cameras.
Constructing an index-selected `EGrabber` directly reached the producer's
`IFOpenDevice` with standard `DEVICE_ACCESS_CONTROL` and failed with
`GC_ERR_ACCESS_DENIED` (-1005), "Requested operation is not allowed." As a
control, the PipeWireAO BGAPI2 source then used the same CTI and connected
GE34GM camera to capture ten complete 640x480 Mono8 frames. This isolates the
failure to eGrabber/Baumer facade-producer interoperability rather than CTI
loading, network discovery, or camera availability. The eGrabber source must
therefore use an Euresys producer for this camera; the BGAPI2 source remains the
supported path for the Baumer producer.

The opt-in host qualification runs the standard factory through an isolated
PipeWireAO daemon:

```console
./spa/plugins/egrabber/qualify-host.py build-cameras ../pipewire/build
```

It starts two remote `pw_stream` input processes against one mapped pool. The
first process retains its initial lease while the second joins, captures, and
leaves; the first then continues. A later subscriber replaces the inactive
pool through normal SPA renegotiation. Finally, the harness destroys the source
while that subscriber retains a lease and verifies that its metadata and
payload remain unchanged, the source disappears, and the daemon remains
healthy. The harness removes its isolated runtime directory on success or
failure.

The 2026-08-23 migration qualification proved that the isolated daemon
loads this out-of-tree DSO and completes normal capture, retained-lease fan-out,
live join/leave, and final subscriber teardown. That run qualified the former
private RTC owner. The source now uses regular graph scheduling and a polling
data loop; the retained result remains device and transport evidence, while the
new scheduler lifecycle is covered by the core polled-driver tests and the
current plugin suite.

Gigelink is complete-only: `progressive=offer` falls back to complete frames and
`progressive=require` is rejected. Grablink/Coaxlink progressive behavior is
implemented against the vendor StartOfCameraReadout, acquiring-buffer, and
filled-size contract, but remains a hardware qualification item. Progressive
publication rejects DMA-BUF by design.

Grablink CLProtocol control compiles only when `genicam-root` is configured.
The optional host/provider integration test covers provider probing, GenApi
feature access, attached-camera serial mismatch, and serial cleanup after
construction failure. The physical Grablink serial path remains unqualified
because no Grablink board is connected.

Explicit-sync DMA-BUF is currently restricted to one active subscriber. The
standard SyncTimeline allocation has one release timeline, so it cannot safely
represent several independent asynchronous consumers of a shared fan-out
buffer. Mapped host buffers retain the progressive lease fan-out. A second live
subscriber is rejected before capture starts and cannot join a running
DMA-BUF source. Release readiness is queried without waiting on the polling path.
A slot whose release point has not been signalled remains locally held while
the bounded scan examines the rest of the pool; a late or failed subscriber
therefore produces pool starvation rather than blocking acquisition.

The complete and progressive process paths do not take application-owned
locks. Buffer completion, progress queries, and recycling are exclusively
owned by the source's polling data loop. Public Start, Pause, and Suspend pass
through the implementation-node state machine; Pause and Suspend remove the
poll source before they reach the SPA node. Configuration and teardown remain
on the stopped control path and retain their camera-facade mutex.

An eight-second `heaptrack` capture on 2026-08-23 used the connected 640x480
Mono8 Gigelink camera, eight mapped host buffers, and ten completed frames. The
original completion callback made 1,730 allocation calls. Removing composite
`BufferInfo`, using negotiated static layout fields, querying only ten dynamic
buffer fields, and remembering unsupported optional commands reduced that to
393 calls. Calling the public scalar `EGenTL::dsGetBufferInfo` interface instead
was measured and rejected: it made 3,100 callback allocations in the same
test. Replacing the progressive pool scan with the fixed submission-order
tracker left the callback count at 393. The profile commands were:

```console
meson test -C build --print-errorlogs \
  --wrapper='heaptrack -o /tmp/pwao-egrabber-profile' spa-egrabber-capture
heaptrack_print /tmp/pwao-egrabber-profile.zst \
  -F /tmp/pwao-egrabber-allocations.stacks \
  --flamegraph-cost-type allocations
```

PipeWireAO `64301ed9c` removed that empty-poll obstacle. The source now calls
the SDK's no-timeout `processEventFilter` overload, which returns normally when
no event is queued and invokes the enabled callbacks synchronously when work is
present. Callback exceptions are retained and rethrown after the vendor C
callback returns; they do not unwind through C. No timeout, readiness query,
helper thread, or private handoff remains.

A comparable eight-second `heaptrack` run reduced allocations attributed to
`process_event` from 1,276,059 to 1,053 and total test allocations from
1,425,092 to 150,086. The remaining 1,053 calls occur on actual vendor event
delivery and frame handling, not at empty-poll frequency. Three alternating
`perf stat` runs compared `b3e3ff9b1` with `64301ed9c`, both GCC 14.2
debug-optimized builds without LTO:

| Median counter | Pending-count baseline | No-timeout callback | Change |
| --- | ---: | ---: | ---: |
| Task CPU | 1,014.45 ms | 640.00 ms | -36.9% |
| Cycles | 4.296 billion | 2.604 billion | -39.4% |
| Instructions | 9.933 billion | 4.627 billion | -53.4% |
| Branches | 2.135 billion | 0.880 billion | -58.8% |
| Context switches | 2,576 | 2,390 | -7.2% |
| CPU migrations | 51 | 51 | unchanged |

The environment was Linux 6.12.57 on an AMD Ryzen 7 6800H, one eight-core
socket with SMT, one NUMA node, and the `amd-pstate-epp` `powersave` governor.
Each run captured the same ten live frames. This experiment demonstrates
allocation and CPU-work reduction for the polling implementation; it is not an
open-loop frame-latency or tail-jitter qualification.

After RTC lifecycle serialization and removal of the progress/recycle locks, a
second clean-build capture reproduced the same 1,098 allocations under the
whole `process()` call: 1,053 under actual event delivery and 45 while nine
buffers were queued again. Of the event-delivery calls, 393 were under the ten
dynamic buffer-metadata queries for each of ten completed frames; the remaining
660 were in the Euresys event bridge and Gigelink producer. The 45 recycle calls
consisted of 18 Euresys `NewBufferData` boxing allocations and 27 allocations
inside the producer's `DSQueueBuffer`. Removing the application mutex therefore
does not change the allocation count, as expected. Public `Buffer::push` is
retained because its `NewBufferData` owner fields are documented as internal;
bypassing its camera-owner dispatch to call `DSQueueBuffer` directly would rely
on an unsupported vendor representation.

The eGrabber CallbackOnDemand API exposes no readiness file descriptor. The
plugin therefore has no honest eventfd readiness source and does not add a
helper thread or private handoff merely to synthesize one. It uses a configured
PipeWire busy-spin data loop and `SPA_NODE_FLAG_POLL_DRIVER`; the uncontended
source probe and graph activation path performs no kernel polling syscall.

## Remaining qualification

- Qualify acquisition-domain identity on Grablink/Coaxlink hardware and
  physical exposure-start mapping and uncertainty. The current completion-time
  anchor restores generic Header PTS behavior but does not claim an
  `SPA_META_Acquisition.exposure_start` value.
- Qualify StartOfCameraReadout progressive publication on Grablink/Coaxlink
  hardware, including partial-row, completion, incomplete-frame, restart, and
  cancellation behavior.
- Qualify Grablink Camera Link serial open/read/write, CLProtocol provider
  probing, attached-camera serial verification, and layout synchronization on
  physical hardware.
- Qualify complete-frame DMA-BUF and SyncObj timeline behavior on supported
  Grablink/Coaxlink hardware. The connected Gigelink device cannot exercise
  this path.
- Remove or bound actual-event allocations from the vendor callback and dynamic
  buffer-information paths before admitting the eGrabber process function to a
  strict BusySpin deployment. The application-owned process path is lock-free,
  but the connected Gigelink producer still allocates while delivering actual
  events, querying buffer information, and queuing recycled buffers.
- Keep the standalone application only as a physical Grablink/Coaxlink
  progressive and DMA-BUF behavior oracle until those paths are qualified in
  the SPA plugin.
