# Raw pixel row-block schema, version 1

Status: implemented for eGrabber row-block output and Calculon pixel calibration

Schema identifier:
`org.calculon.ao.raw-pixel-row-block/1`

## Meaning and format

One buffer contains a complete immutable contiguous group of uncalibrated
detector rows copied from the camera readout. For detector extent
`[height, width]` and block height `N`, the exact ndarray format is:

```text
mediaType    = application
mediaSubtype = ndarray
schema       = org.calculon.ao.raw-pixel-row-block/1
elementType  = U16_LE
shape        = [N, width]
layout       = ROW_MAJOR
profile      = detector profile
rate         = frame_rate * height / N
```

`N` is positive, smaller than `height`, divides `height`, and remains fixed
for the negotiated stream. The payload contains `N * width` U16 values in
row-major order. Its normal chunk stride is `width * sizeof(uint16_t)`.

Every buffer uses ordinary `SPA_IO_Buffers`. It is complete and immutable when
published. The private camera frame from which it was copied may still be
receiving later rows, but that allocation is not exposed through this schema.

## Frame and block identity

Every block has `SPA_META_Header`:

| Header field | Required meaning |
| --- | --- |
| `seq` | Stable frame or acquisition identity shared by every block in the frame |
| `offset` | Zero-based detector row of this block's first row |
| `MARKER` | Set exactly when `offset + N == height` |
| `DISCONT` | Set on the first block after a lost, aborted, invalid, or abandoned frame |
| `CORRUPTED` | Set on the terminal block when camera completion reports corrupt data |
| `pts` | Camera completion timestamp when known; an early block may use `SPA_TIME_INVALID` |

Offsets are `0, N, 2N, ... height-N`. Consumers identify membership from
`seq`, `offset`, and `MARKER`, not arrival time.

When configured, `SPA_META_Acquisition` is copied to each block so semantic
joins can match physical acquisition identity.

## Loss and ownership

eGrabber copies one complete row quantum into a reusable ordinary output
buffer. It recycles the private camera frame only after its final block is
copied or the frame is abandoned. If no output buffer is available, it drops
the remaining blocks and marks the next frame discontinuous.

A consumer owns an ordinary complete-buffer lease. It never owns or observes
the camera's in-progress allocation.
