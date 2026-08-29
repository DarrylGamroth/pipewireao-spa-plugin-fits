# Hamamatsu DCAM camera source

`api.hamamatsu.source` is a complete-frame SPA source backed by Hamamatsu
DCAM-API. Its initial target is the CoaXPress ORCA-Quest connected through an
Active Silicon FireBird frame grabber, but the adapter enumerates DCAM devices
and properties at runtime rather than hard-coding one camera model.

The FireBird hardware also supports GenTL. That does not make this camera a
GenICam device: Hamamatsu uses the frame grabber below DCAM-API and exposes its
own property model. This source therefore uses DCAM for both control and
capture. It does not claim that a FireBird GenTL consumer can safely share the
device with DCAM.

## Install and build

Install the vendor runtime on the host before using a physical camera. For the
CoaXPress/FireBird path, run the vendor installer with the `fbd` target. The
driver and runtime bundle used during development is
`DCAM-API_Lite_for_Linux_v26.6.7175`; DCAM-SDK4 supplies the headers.

An unpacked development build can use the local bundles directly:

```console
meson setup build-hamamatsu \
  -Dhamamatsu=enabled \
  -Dhamamatsu-sdk-prefix=$HOME/hamamatsu/Hamamatsu_DCAMSDK4_v25056964/dcamsdk4 \
  -Dhamamatsu-runtime-prefix=$HOME/hamamatsu/DCAM-API_Lite_for_Linux_v26.6.7175/api/runtime/x86_64
meson compile -C build-hamamatsu
meson test -C build-hamamatsu 'spa-hamamatsu*' --print-errorlogs
```

The build creates a non-destructive link to a versioned `libdcamapi.so` when
the unpacked runtime has no unversioned linker name. This is sufficient to
compile the plugin. Physical capture still needs the normal vendor runtime,
module configuration, FireBird driver, and device permissions.

## Factory properties

| Property | Meaning |
| --- | --- |
| `api.hamamatsu.device-index` | Zero-based DCAM device index; default `0`. |
| `api.hamamatsu.readiness` | Must be `poll` when supplied. |

DCAM `MONO8` maps to SPA `GRAY8`. Unpacked `MONO12` and `MONO16` map to
`GRAY16_LE`. Packed 12-bit and color formats are rejected until the plugin has
an exact SPA representation for them. DCAM frame-bundle mode is also rejected:
one SPA video buffer represents one image, while a DCAM bundle changes that
contract.

## Camera parameters

The source enumerates supported DCAM properties from the opened camera. It
preserves native value types, numeric ranges, enum labels, access state, and
units in `SPA_PARAM_PropInfo` and exposes current readable values through
`SPA_PARAM_Props`.

Common controls use SFNC-style names where the meaning is close enough:

- `hamamatsu.ExposureTime` (DCAM native unit: seconds)
- `hamamatsu.Width`, `Height`, `OffsetX`, and `OffsetY`
- `hamamatsu.PixelFormat`
- `hamamatsu.TriggerSource`
- `hamamatsu.DeviceTemperature` and `SensorCooling`

Other properties use a stable `hamamatsu.<CamelCaseDcamName>` form. Each
tooltip retains the original DCAM name, numeric property identifier, and unit.
This includes DCAM-native controls such as `hamamatsu.InternalFrameRate`,
`TriggerMode`, `TriggerActive`, and `TriggerPolarity`, whose meaning does not
exactly match similarly named SFNC nodes.
Software triggering is a command:
`hamamatsu-command.SoftwareTrigger=true`. It is available while acquisition is
running; ordinary feature writes are accepted only while stopped.

Layout-changing writes require all SPA buffers to be released. A successful
write clears the negotiated format so the graph must renegotiate dimensions,
stride, payload size, or pixel format.

## Buffer ownership

`dcambuf_attach` accepts application-allocated memory, but DCAM cycles through
the attached array as an overwrite ring. A downstream PipeWire client can hold
a published SPA buffer indefinitely, so attaching the SPA pool directly would
allow the camera to overwrite leased data.

This implementation asks DCAM to allocate its recommended 16-frame internal
capture ring. When a complete frame is available, `dcambuf_copyframe` copies
the newest frame into an available SPA buffer. This costs one host copy but
preserves PipeWire buffer ownership. The DCAM manual explicitly limits
`dcambuf_copyframe` to buffers created by `dcambuf_alloc`; it cannot be combined
with `dcambuf_attach`. Frames skipped because no SPA buffer was available
appear as a sequence discontinuity; completed frames are not marked corrupt.

## Qualification boundary

Mock tests cover factory validation, format negotiation, dynamic controls,
layout invalidation, metadata, buffer recycling, and pause/restart behavior.
The real backend is compiled against the supplied DCAM headers and runtime.
Physical ORCA-Quest/FireBird discovery, acquisition, trigger behavior,
timestamps, dropped-frame accounting, throughput, and restart recovery still
require camera hardware qualification.
