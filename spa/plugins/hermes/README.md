# MPD HERMES FrontPanel SPA plugins

This directory contains two SPA factories for the MPD HERMES detector:

- `api.hermes.source` captures raw FrontPanel block-pipe batches directly into
  buffers announced by the SPA host.
- `api.hermes.decoder` converts each raw batch into ordered counter images.

The source deliberately does not decode or transpose pixels. This keeps camera
I/O separate from downstream image processing and lets one batch represent one
adaptive-optics control update.

## Buffer ownership and scheduling

The source announces the host's SPA buffers, queues them in a bounded ring, and
passes the selected buffer directly to `okFrontPanel_ReadFromBlockPipeOut`.
There is no per-batch allocation or intermediate payload copy in the plugin.
The HERMES SDK is still used for camera discovery, configuration, start, and
stop; its constructor may allocate private memory that is not used by this
streaming path.

`api.hermes.source` is a poll driver. Its `process()` method performs one exact
block-pipe read and can block until the requested batch is available. It should
run on a dedicated data-plane thread. When no announced buffer is available,
capture does not overwrite a buffer owned by the graph.

For a 100 kframe/s camera rate and a 2 kHz deformable-mirror update rate, set
`api.hermes.frames-per-buffer=50` and `api.hermes.batch-rate=2000/1`. The batch
rate is a graph timing contract; the current SDK does not report a measured
camera frame rate through this plugin.

## Source configuration

| Key | Default | Meaning |
| --- | --- | --- |
| `api.hermes.device-id` | empty | FrontPanel device ID; empty selects the SDK default |
| `api.hermes.camera-mode` | `normal` | `normal` or `advanced` HERMES mode |
| `api.hermes.exposure-clocks` | `1040` | Integration duration in HERMES clock units |
| `api.hermes.integrated-frames` | `1` | Sensor frames integrated into each reported frame |
| `api.hermes.counters` | `1` | Counter planes per camera frame, from 1 through 3 |
| `api.hermes.force-8bit` | `true` | Request the SDK's eight-bit output mode |
| `api.hermes.half-array` | `false` | Select the 32 by 32 half-array profile; requires one counter |
| `api.hermes.signed-data` | `false` | Signed counter data is rejected by the current schemas |
| `api.hermes.bits-per-pixel` | `8` | Expected wire representation, `8` or `16` |
| `api.hermes.frames-per-buffer` | `50` | Camera frames in one SPA batch |
| `api.hermes.batch-rate` | `2000/1` | Nominal batches per second |

The expected bit depth must agree with `HermesIs16Bit` after the settings are
applied. A mismatch fails initialization instead of silently publishing the
wrong schema.

## Format contracts

The raw source publishes the exact schema
`org.pipewireao.hermes.frontpanel-raw-batch/1` as a row-major U8 ndarray with
shape:

```text
[frames-per-buffer, counters, raw-plane-bytes]
```

`raw-plane-bytes` is 2048 or 4096 for the full array and 1024 or 2048 for the
half array, depending on bit depth. U16 carrier bytes are little-endian.

The decoder accepts that exact schema and publishes
`org.pipewireao.hermes.counter-frame-batch/1` with shape:

```text
[frames-per-buffer, counters, height, width]
```

The decoded element type is U8 or U16_LE. The full-array shape is 32 by 64; the
half-array shape is 32 by 32. Input and output must use the same one of these
profiles:

- `hermes-full-frontpanel-u8-v1`
- `hermes-full-frontpanel-u16le-v1`
- `hermes-half-frontpanel-u8-v1`
- `hermes-half-frontpanel-u16le-v1`

## SDK compatibility

The public HERMES SDK does not expose its FrontPanel handle. The source isolates
one compatibility shim that reads the handle from the Linux x86-64 SDK object
layout and then uses the public FrontPanel C ABI for streaming. The supported
`libHermes.so` has GNU build ID
`2ce43284a3064e9f69d4e42ea1044a04cc4e48b3`. A different SDK build must be
validated before use; the offsets are not a vendor ABI.
The source checks this build ID before reading the private handle and returns
`ELIBBAD` for a different binary.

The implementation has mock-source and decoder permutation tests. It has not
yet been validated with physical HERMES hardware.
