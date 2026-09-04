# Calibrated pixel row-block schema, version 1

Status: implemented for row-block pixel calibration and frame assembly

Schema identifier:
`org.calculon.ao.calibrated-pixel-row-block/1`

## Meaning and format

A buffer contains one complete immutable contiguous group of calibrated
detector rows. Values have the same scientific meaning as the corresponding
rows of a complete
`org.calculon.ao.calibrated-pixels/1` frame. Row-block granularity changes
scheduling and artifact size, not the calibration equation.

For detector extent `[height, width]` and block height `N`:

```text
mediaType    = application
mediaSubtype = ndarray
schema       = org.calculon.ao.calibrated-pixel-row-block/1
elementType  = F32_LE
shape        = [N, width]
layout       = ROW_MAJOR
rate         = frame_rate * height / N
```

The schema is the complete payload-interpretation identifier. The active
calibration generation is parameter or artifact state and is recorded outside
the ndarray format.

`N` is positive, smaller than `height`, divides `height`, and remains fixed
for the negotiated stream. The payload contains `N * width` F32 values in
row-major order. The normal chunk stride is `width * sizeof(float)`.

Every buffer uses ordinary `SPA_IO_Buffers` and is complete when published.

## Frame and block identity

Every block has `SPA_META_Header`:

| Header field | Required meaning |
| --- | --- |
| `seq` | Stable source-frame identity shared by every block in the frame |
| `offset` | Zero-based detector row of the block's first row |
| `MARKER` | Set exactly when `offset + N == height` |
| `DISCONT` | The frame follows loss, abort, invalid input, or an incomplete frame |
| `CORRUPTED` | Input corruption propagated from the raw block |
| `pts` | Timestamp propagated from the raw artifact |

Blocks occur at offsets `0, N, 2N, ... height-N`. A consumer must not infer
frame membership from arrival time or graph-cycle number.

## Calibration and assembly

Calculon FGN pixel calibration accepts either a complete raw-detector ndarray
or the exact raw row-block schema. Complete packed raw video first passes
through `api.ndarray.video-view`; the adapter changes only the structural
format and never changes pixel values. In block mode calibration consumes one
raw block for each calibrated block.

An `api.ndarray.frame-assembly` instance configured with this input schema and
`org.calculon.ao.calibrated-pixels/1` as its output schema accepts only the next
expected offset for one sequence. A gap, overlap, changed sequence,
out-of-range block, or invalid marker abandons the partial frame. The next
complete output carries `DISCONT`. No partial frame is published.

Assembly copies each block once into a preallocated frame. Consumers that
accept this schema may branch before assembly. Complete-frame algorithms,
telemetry, GUI, and recorders normally branch after assembly.
