# ImageStreamIO bridge

This optional plugin connects PipeWireAO ndarray ports to the upstream milk
[ImageStreamIO](https://github.com/milk-org/ImageStreamIO) shared-memory ABI.
It exports two ordinary SPA factories:

- `api.imagestreamio.source` reads an existing ImageStreamIO stream and
  publishes PipeWire buffers;
- `api.imagestreamio.sink` writes PipeWire buffers to an ImageStreamIO stream.

ImageStreamIO remains the authority for its file layout, version checks,
metadata, semaphores, and stream counters. The plugin does not reimplement or
vendor that ABI. The checked-out upstream library uses the MIT License, which
is compatible with this repository.

## Build

Install ImageStreamIO and enable the feature:

```console
meson setup build-isio \
  -Dimagestreamio=enabled \
  -Dimagestreamio-prefix=/opt/ImageStreamIO
meson compile -C build-isio
meson test -C build-isio --print-errorlogs 'spa-imagestreamio*'
```

The prefix must contain `include/ImageStreamIO/ImageStreamIO.h` and
`lib/libImageStreamIO.so` or `lib64/libImageStreamIO.so`. The default prefix is
`/usr/local`.

## Factory properties

Both factories require:

| Property | Meaning |
| --- | --- |
| `api.imagestreamio.name` | ImageStreamIO stream name, not a filesystem path. |
| `api.imagestreamio.schema` | Exact ndarray semantic schema published or accepted by the node. |
| `api.imagestreamio.profile` | Optional deployment annotation published on the node; it does not participate in ndarray format negotiation. |

The source always attaches. It additionally accepts
`api.imagestreamio.semaphore=auto` or a non-negative explicit semaphore index.
`auto` is the default. Each running source claims a free stream semaphore,
flushes stale posts, publishes one initial snapshot, and then publishes on
counter changes or semaphore posts.

The sink accepts `api.imagestreamio.access=create|attach`:

- `create` is the default. The selected ndarray format determines the stream
  type and dimensions. The node owns the stream and removes it during teardown.
  It refuses to start if any filesystem entry already occupies the upstream
  stream pathname. This guard is important because the upstream creation API
  otherwise replaces an existing CPU stream.
- `attach` opens an existing stream during construction, advertises its exact
  format, never removes it, and requires this node to be the sole writer.

## Array mapping

The SPA side is a contiguous, row-major ndarray. ImageStreamIO stores the
horizontal dimension as `size[0]`, so a two-dimensional `width` by `height`
stream maps to ndarray shape `[height, width]` without transposing or
reordering its bytes. Rank-one arrays retain shape `[length]`.

The bridge supports these exact storage mappings:

| ImageStreamIO | SPA ndarray |
| --- | --- |
| `UINT8`, `INT8` | `U8`, `I8` |
| `UINT16`, `INT16` | `U16_LE`, `I16_LE` |
| `UINT32`, `INT32` | `U32_LE`, `I32_LE` |
| `UINT64`, `INT64` | `U64_LE`, `I64_LE` |
| `FLT16`, `FLT32`, `FLT64` | `F16_LE`, `F32_LE`, `F64_LE` |
| `CPLX32`, `CPLX64` | `COMPLEX_F32_LE`, `COMPLEX_F64_LE` |

CPU-backed rank-one and rank-two streams are supported in both directions. A
rank-three source is accepted only when its metadata marks the third axis as a
circular or temporal axis; each update publishes the latest two-dimensional
slice. Plain three-dimensional volumes are rejected because treating a volume
as either one sample or a sequence requires an explicit schema decision.
GPU-backed streams are not yet supported.

## Scheduling and ownership

The source is a non-blocking poll driver. `process()` uses `sem_trywait`, so it
never blocks a graph thread. It performs a bounded coherent-copy retry around
the upstream `write`, `cnt0`, and `cnt1` fields. If a producer changes the
selected slice during a copy, that graph cycle publishes nothing and retries
on a later cycle.

The sink copies one complete input buffer into the shared-memory array, then
uses `ImageStreamIO_UpdateIm` to update `cnt0`, timestamps, and semaphores. It
does not publish partial updates. Attached mode assumes the surrounding system
has assigned exactly one writer; ImageStreamIO's `write` byte is not a robust
multi-writer lock.

PipeWire buffers cannot be announced directly to ImageStreamIO, and
ImageStreamIO memory cannot be transferred into a PipeWire pool. Both
directions therefore contain one deliberate payload copy. The bridge carries
the ImageStreamIO `cnt0` value as the source Header sequence. It leaves Header
PTS invalid because ImageStreamIO acquisition timestamps use an absolute clock
domain that cannot be assumed to match the active PipeWire graph clock.

Keywords, GPU IPC, partial writes, automatic relinking after replacement, and
plain rank-three volumes remain outside the initial contract.
