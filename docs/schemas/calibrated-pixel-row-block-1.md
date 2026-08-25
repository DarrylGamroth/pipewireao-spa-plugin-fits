# Calibrated pixel row-block schema, version 1

Status: implemented for progressive eGrabber pixel calibration and frame
assembly

Schema identifier:
`org.calculon.ao.calibrated-pixel-row-block/1`

## Meaning and format

A buffer contains one complete, immutable, contiguous group of calibrated
detector rows. Its values have the same scientific meaning and profile as the
corresponding rows of a complete
`org.calculon.ao.calibrated-pixels/1` frame. The row block is a scheduling and
artifact granularity; it does not define a different calibration equation.

For detector extent `[height,width]` and configured block height `N`, the exact
negotiated ndarray format is:

```text
mediaType    = application
mediaSubtype = ndarray
schema       = org.calculon.ao.calibrated-pixel-row-block/1
elementType  = F32_LE
shape        = [N, width]
layout       = ROW_MAJOR
profile      = <the exact detector profile>
rate         = frame_rate * height / N
```

`N` is positive, is smaller than `height`, and divides `height`. It remains
fixed for the negotiated stream. The payload contains `N * width` F32 values
in row-major order. The normal chunk stride is
`width * sizeof(float)` bytes.

Every row-block buffer uses ordinary `SPA_IO_Buffers`. It is complete when
published and does not carry `SPA_META_Progressive`. Progressive visibility is
confined to the raw camera input lease before calibration.

## Frame and block identity

Every block has `SPA_META_Header`. Existing fields define its position in the
source frame:

| Header field | Required meaning |
| --- | --- |
| `seq` | Stable source-frame identity shared by every block in that frame. |
| `offset` | Zero-based detector row of the block's first row. |
| `MARKER` | Set exactly when `offset + N == height`. |
| `DISCONT` | The frame is discontinuous because input was lost, aborted, invalid, or followed an incomplete frame. |
| `pts` | Timestamp inherited from the source frame. |

Blocks for one frame are published at offsets `0, N, 2N, ... height-N`. A
consumer must not infer frame membership from arrival time or graph-cycle
number. Sequence, offset, and marker are the public identity contract.

## Loss, assembly, and ownership

Pixel calibration publishes at most one row block per graph cycle and retains
the raw progressive camera lease until it observes a terminal `COMPLETE` or
`ABORTED` state. It snapshots the selected flat/background calibration pair at
frame start, so all blocks for one sequence use one calibration generation.

`api.calculon.frame-assembly` accepts only the next expected offset for one
sequence. A gap, overlap, changed sequence, out-of-range block, or invalid
marker abandons the partial frame. The next complete output carries `DISCONT`.
No partial frame is published.

Assembly copies each block once into a preallocated contiguous frame. Consumers
that accept this schema may branch before assembly. Complete-frame algorithms,
telemetry, GUI, and recorders branch after assembly; non-real-time observers
should be isolated there with a bounded queue.
