# Scientific graph and structural-transform architecture

Status: accepted and implemented on 2026-08-29

Decision: PWAO-PLUGIN-002

## Ownership boundary

Scientific algorithms use the PipeWireAO ndarray filter-graph (FGN) ABI.
Calculon owns its schemas, declarations, prepared numerical state, workspaces,
and the production `libcalculon-fgn.so` bundle. Pixel calibration, wavefront
measurement, reconstruction, controller integration, and PDM command creation
are no longer duplicated as native SPA factories in this repository.

This repository retains only operations that belong to a transport or device
boundary:

- `api.ndarray.frame-assembly` converts immutable row-block artifacts into one
  complete ndarray artifact.
- `api.ndarray.video-view` exposes a packed raw-video frame as an equivalent
  ndarray artifact without interpreting pixel values.
- `api.hnu240.decoder` maps the Nüvü HNü240 Camera Link carrier into detector
  pixels.
- ALPAO's `command-normalization-f32-f64` FGN operator converts physical PDM
  commands into its normalized device-command schema.
- `api.alpao.sink` owns the final SPA and ASDK device boundary.

The reusable `pipewireao-spa-node` Rust crate contains the native SPA ABI
boundary shared by frame assembly, the HNü240 decoder, and the HERMES decoder.
It catches panics before they cross C and uses a non-waiting callback gate.
Rust is appropriate for these stateful byte-layout transforms; choosing Rust
does not make them scientific Calculon algorithms.

## Exact formats

Every ndarray boundary matches element type, shape, packed layout, optional
rate, and optional semantic schema exactly. Absence is significant. Device,
transport, calibration, and deployment identities are carried by node
properties, parameters, artifacts, and deployment manifests rather than an
opaque format field.

The FGN ABI carries scientific operations inside one filter-chain node. This
avoids a separate PipeWire buffer handoff for every Calculon operation while
retaining explicit schemas and independently declared operators.

## Generic frame assembly

`api.ndarray.frame-assembly` is schema-configured and byte-preserving. Its
construction keys are:

| Key | Meaning |
| --- | --- |
| `api.ndarray.frame-size` | Positive complete `WIDTHxHEIGHT` extent. |
| `api.ndarray.frame-rate` | Optional positive complete-frame `NUM/DEN` rate. |
| `api.ndarray.row-block-rows` | Positive block height smaller than and dividing `HEIGHT`. |
| `api.ndarray.row-block-schema` | Optional exact input semantic schema. |
| `api.ndarray.frame-schema` | Optional exact output semantic schema. |
| `api.ndarray.element-type` | Any standard fixed-width SPA ndarray element type. |
| `api.ndarray.layout` | `row-major` or `column-major`. |

The input advertises `[block_rows,width]` and complete `[height,width]`
alternatives. The output is the complete alternative. `SPA_META_Header.seq`
identifies the frame, `offset` is the first block row, and `MARKER` identifies
the last block. Only contiguous rows from one sequence are published. A gap,
overlap, unexpected sequence, or invalid marker abandons the partial frame and
marks the next complete output `DISCONT`.

A complete input frame passes through directly when the negotiated buffers
share storage. Otherwise the node performs one direct copy. Row-block assembly
uses one preallocated workspace and publishes nothing until the final block.

## Generic raw-video view

`api.ndarray.video-view` is the complete-frame bridge into FGN. It accepts an
exact packed `GRAY8` or `GRAY16_LE` frame and publishes the identical bytes as
a row-major U8 or U16 ndarray. Construction fixes the frame size, frame rate,
output schema, and video format. Compatible SPA buffer
allocation forwards shared storage; the fallback copies each packed row once.
The adapter does not normalize, calibrate, byte-swap, unpack, or change pixel
values.

## Nüvü decoder

`api.hnu240.decoder` remains a camera-specific native SPA transform. It accepts
the exact `hnu240-cl-full-8x8-v1` carrier profile as `GRAY8` 1408 by 131 and
discards the ten leading non-active carrier rows and final per-tap overscan line
before publishing the active detector area as `GRAY16_LE` 240 by 240. The
decoder owns carrier-byte decoding and pixel rearrangement only. Camera control
remains in `CLProtocol_hnu240`, and the camera or frame-grabber driver publishes
the raw carrier unchanged.
`api.ndarray.video-view` then provides the raw-detector ndarray schema expected
by Calculon FGN pixel calibration.

The detector has eight 16-bit outputs, but its Camera Link Full transport is a
custom eight-tap, 8-bit packing with the Z channel unused. A generic Mono16 tap
geometry therefore cannot replace the Nüvü transform. The exact packing,
frame-grabber implications, and hardware acceptance checks are maintained in
the [HNü240 Camera Link carrier note](hnu240-camera-link-carrier.md).

The progressive form consumes immutable carrier row blocks from the native
Aravis GigE Vision receiver and converts each admitted active-row quantum into
one U16 detector readout block with logical shape `[8, N, 60]`. Carrier rows
and detector readout regions are different representations: the Aravis source
owns committed GVSP payload progress, the Nüvü transform owns carrier decoding
and removal of pipeline and overscan content, and the prepared detector
readout mapping owns detector-coordinate placement and direction. The
scientific graph never receives overscan pixels.

An HNü240 carrier row block is not a raw detector-pixel row block. The Aravis
source must retain an exact Nüvü-owned carrier schema until this transform
produces `org.calculon.ao.raw-pixel-readout-block/1`; transport identity stays
in source and decoder configuration. The current
Aravis fixed block height must divide the advertised carrier height; a complete
131-row carrier therefore permits only one-row blocks. A production profile
must either configure an iPORT carrier-row window or admit an exact Aravis
row-selection and batching contract. The converter may process active rows as
they arrive, but it must withhold the terminal active output until the camera
frame is known to be complete and valid. It then consumes and discards the
overscan row. The carrier schema's exact public identifier remains to be
admitted with that format contract; this document does not invent one.

## ALPAO command boundary

The ALPAO-owned FGN operator consumes
`org.calculon.ao.demanded-pdm-command/1` F32 vectors and publishes
`org.pipewireao.alpao.normalized-actuator-command/1` F64 vectors. Its
construction profile is deployment identity and must match the sink's
configured identity before the graph is admitted; it is not part of either
port format. The operator divides by the positive configured command scale and rejects
non-finite or out-of-range normalized values. A future calibrated conversion
can replace the scalar implementation without transferring ownership to
Calculon.

## Retired native factories

The following native SPA factories were removed:

- `api.calculon.pixel-calibration`
- `api.calculon.shwfs-controller`
- `api.calculon.frame-assembly`
- `api.alpao.command-normalization`

Existing profiling records remain historical evidence for the retired
implementation. New scientific graph work and performance qualification belong
in `calculon-algorithms` and its FGN bundle.
