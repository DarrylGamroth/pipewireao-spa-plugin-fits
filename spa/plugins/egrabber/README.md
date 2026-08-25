# eGrabber SPA plugin

This optional plugin is the native PipeWireAO integration for Euresys
eGrabber. Enable it with `-Degrabber=enabled`; use
`-Degrabber-prefix=PATH` when the SDK is not installed under
`/opt/euresys/egrabber`.

The plugin exports:

- `api.egrabber.enum.manager` for one-shot device discovery;
- `api.egrabber.device` for manager-to-node object creation; and
- `api.egrabber.source` for scheduled camera capture.

It supports Euresys `gigelink`, `grablink`, and `coaxlink` producer
aliases, or an explicit CTI path. Only Euresys producers are qualified.

## Execution model

The source is a regular PipeWire graph driver. It reports
`SPA_NODE_FLAG_POLL_DRIVER` and must run on a configured busy-spin data loop.
The eGrabber CallbackOnDemand API exposes no readiness file descriptor, so
`process()` calls the SDK's no-timeout event dispatch and returns
`SPA_STATUS_OK` when no work is present.

Unlike the complete-frame BGAPI2 and timed FITS sources, eGrabber does not
offer a readiness property. Both `frame` and `row-block` output use polling in
the current CallbackOnDemand implementation. A future ordinary-readiness
profile would require a qualified SDK notification mechanism; output artifact
granularity alone does not make one available.

There is no private capture thread, private RTC data loop, latest-value
transport, or changing public buffer. Downstream nodes use ordinary
`SPA_IO_Buffers` and normal scheduler dependencies.

A source can publish one of two exact output contracts.

## Complete-frame mode

```ini
api.egrabber.output-mode = frame
```

`frame` is the default. The output is standard raw video for supported
monochrome camera formats:

- `Mono8` as `GRAY8`;
- `Mono10`, `Mono12`, `Mono14`, or `Mono16` as `GRAY16_LE`.

The negotiated SPA buffers are announced directly to eGrabber. The camera owns
a slot until full completion; the source then publishes that same slot. This is
the zero-payload-copy mode.

Mapped `MemPtr` and `MemFd` storage are supported. Complete-frame DMA-BUF
can be offered when eGrabber, a local DRM render node, and negotiated
`SPA_META_SyncTimeline` support it. Explicit-sync DMA-BUF remains limited to
one active subscriber.

## Row-block mode

```ini
api.egrabber.output-mode = row-block
api.egrabber.row-block-rows = 8
api.egrabber.detector-profile = detector-profile-id
```

Row-block mode requires:

- a detector profile;
- positive `row-block-rows` that divides the detector height;
- qualified `StartOfCameraReadout` and filled-size observations; and
- CPU-readable host camera storage.

The camera DMA fills a private aligned host buffer. The source rounds filled
size down to complete `N`-row quanta and copies one new quantum into an
ordinary output buffer. Each public buffer is a complete U16 ndarray:

```text
schema = org.calculon.ao.raw-pixel-row-block/1
shape = [N, width]
elementType = U16_LE
layout = ROW_MAJOR
rate = frame_rate * height / N
```

The final block is withheld until terminal camera completion validates the
payload and supplies final flags and timestamp. If output storage is exhausted
or the frame becomes invalid, the source abandons the remaining blocks and
marks the next frame discontinuous.

The private camera slot is recycled only after the final block is copied or the
frame is abandoned. No consumer owns the in-progress camera allocation.
Row-block mode does not publish DMA-BUF.

See [Scheduled nodes and row-block ndarrays](../../../docs/scheduled-node-migration.md)
and [Raw pixel row-block schema](../../../docs/schemas/raw-pixel-row-block-1.md).

## Buffer identity and metadata

Both modes negotiate `SPA_META_Header` and Version 2
`SPA_META_Acquisition`.

Complete frames use the normal frame sequence and completion timestamp. Row
blocks use:

| Header field | Meaning |
| --- | --- |
| `seq` | frame or acquisition identity shared by all blocks |
| `offset` | first detector row in the block |
| `MARKER` | final block only |
| `DISCONT` | first block after loss, abort, invalid layout, or restart |
| `CORRUPTED` | corrupt terminal camera result |
| `pts` | completion timestamp when known; early blocks may be invalid |

An optional 16-byte acquisition domain and vendor event context establish
physical-acquisition identity on qualified Grablink/Coaxlink hardware. The
generation is assigned by the shared acquisition control plane, not by the
source lifecycle. The source retains the last observed trigger sequence across
Pause and Start. It fails closed on a duplicate or reset sequence instead of
inventing a host-local generation, and mirrors a valid acquisition sequence
into Header `seq`. Recreate or reconfigure every participating source with a
new shared generation before a trigger counter can reset or be reused.

The current device timestamp mapping is host-local and is not a qualified PTP
exposure-start mapping. The source therefore leaves the cross-host Version 2
PTP fields invalid.

## Factory properties

| Property | Default | Meaning |
| --- | --- | --- |
| `api.egrabber.producer` | `gigelink` | producer alias or CTI path |
| `api.egrabber.serial` | none | select discovered device serial |
| `api.egrabber.user-id` | none | select discovered user ID |
| `api.egrabber.interface-index` | `0` | fallback interface index |
| `api.egrabber.device-index` | `0` | fallback device index |
| `api.egrabber.stream-index` | `0` | stream index |
| `api.egrabber.buffer-count` | `8` | camera slots; at least two |
| `api.egrabber.output-mode` | `frame` | `frame` or `row-block` |
| `api.egrabber.row-block-rows` | `1` | rows per complete block |
| `api.egrabber.detector-profile` | none | required in row-block mode |
| `api.egrabber.control` | `auto` | `auto`, `remote`, `clprotocol`, or `none` |
| `api.egrabber.control-timeout-ms` | `1000` | positive control timeout |
| `api.egrabber.acquisition-domain` | none | nonzero 128-bit hexadecimal ID |
| `api.egrabber.acquisition-generation` | `0` | externally assigned shared generation |
| `api.egrabber.acquisition-sequence-context` | `0` | event context 1, 2, or 3; zero disables identity |

CLProtocol properties are documented in
[eGrabber Camera Link control](../../../docs/egrabber-clprotocol.md).

## Controls and lifecycle

Scalar GenICam controls are enumerated with `SPA_PARAM_PropInfo` and read or
written with `SPA_PARAM_Props`. Writes require a paused node. A layout-changing
write also requires the output pool to be released; afterward the host must
renegotiate Format and Buffers.

`Start` resets sequence and timestamp state, installs callbacks, announces
buffers, and starts acquisition. `Pause` or `Suspend` first removes the node
from its polling loop, then stops the camera. Buffer release and source
destruction synchronously stop acquisition and recycle or free all slots.

The source polling loop is the sole owner of event dispatch, completion,
filled-size observation, publication, and recycling. Control and pool mutation
remain on the stopped control path.

## Qualification

Automated tests cover:

- option validation and factory enumeration;
- manager/device/source property propagation;
- buffer layout, sequence, acquisition key, timestamp, and DMA-sync helpers;
- camera-backed complete-frame capture through ordinary `SPA_IO_Buffers`;
- ten-frame Header and Acquisition validation;
- pause, format removal, pool reuse, layout control, and teardown; and
- rejection of row-block mode on a producer without the required readout
  contract.

The host client now uses an ordinary `pw_stream`. Holding its first buffer
tests ordinary lease stability, not a lossy fan-out promise. A slow observer
must be isolated with `libpipewire-module-queue`, normally capacity one,
`drop-oldest`, and copy storage.

## Remaining hardware qualification

- Exercise row-block readout on physical Grablink/Coaxlink hardware, including
  partial rows, terminal validation, loss, overlap, pause, restart, and device
  failure.
- Qualify acquisition-domain context and exposure timing on that hardware.
- Qualify physical Camera Link CLProtocol serial behavior.
- Qualify complete-frame DMA-BUF and SyncObj timelines.
- Measure fixed-arrival ready-to-process and end-to-end p50, p99, p99.9, and
  maximum latency on pinned physical cores.
- Bound or remove allocations made inside actual vendor event delivery and
  buffer-information/requeue calls before claiming strict busy-spin behavior.

The connected Gigelink path remains the complete-frame hardware regression
case.
