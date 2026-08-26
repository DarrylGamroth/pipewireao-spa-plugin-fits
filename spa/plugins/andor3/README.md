# Andor SDK3 camera source

`api.andor3.source` is a complete-frame SPA source backed directly by Andor
SDK3. It queues PipeWireAO pool memory with `AT_QueueBuffer`; SDK3 does not own
or replace the image buffers. Each data pointer must be 8-byte aligned and each
data block must be at least `ImageSizeBytes` bytes.

## Build

SDK3 is normally installed on the host. Enable the source and select its
prefix with:

```console
meson setup build-andor3 \
  -Dandor3=enabled \
  -Dandor3-prefix=$HOME/andor3
meson compile -C build-andor3
meson test -C build-andor3 'spa-andor3*' --print-errorlogs
```

The build accepts the unpacked SDK layout used by `~/andor3` (`inc` and
`x86_64`) and conventional `include` and `lib` directories. Some unpacked
SDK3 distributions contain only versioned shared objects even though the core
loads camera modules by unversioned names. Meson creates a non-destructive
`andor3-runtime` link farm in the build directory for development and tests.
Production deployment should use the vendor's normal host installation, or
provide that runtime directory through the service's library search path.

## Factory properties

| Property | Meaning |
| --- | --- |
| `api.andor3.device-index` | Zero-based SDK3 device index; default `0`. |
| `api.andor3.readiness` | Must be `poll` when supplied. |

The source advertises `Mono8` as `GRAY8`, and unpacked `Mono12` or `Mono16` as
`GRAY16_LE`. Packed and color encodings are rejected because they do not have
an exact raw SPA representation in this plugin. The negotiated buffer size is
the SDK3 payload size, while the published image chunk is `AOIStride *
AOIHeight`; this leaves room for SDK metadata when it is enabled.

## Camera parameters

SDK3 does not expose feature-name enumeration. The adapter probes a catalog
from the SDK3 feature reference and publishes only features implemented by the
opened camera:

- Scalar and enum features use `andor3.<SDK3Feature>`, for example
  `andor3.ExposureTime`, `andor3.FrameRate`, `andor3.TriggerMode`, and
  `andor3.SensorCooling`.
- Commands use `andor3-command.<SDK3Feature>`, for example
  `andor3-command.SoftwareTrigger=true`.

Values retain SDK3's native type, enum labels, limits, feature identifier, and
unit. In particular, SDK3 `ExposureTime` is expressed in seconds. This surface
is intentionally not named `genicam.*`: SDK3 resembles a GenICam node map but
is not a GenApi implementation, and similarly named features can have
different units.

Feature writes are accepted only while acquisition is stopped. Layout-changing
writes also require all SPA buffers to be released; a successful write clears
the current format so the graph must renegotiate the new dimensions, stride,
payload, or pixel encoding.

## Qualification boundary

The automated SDK test opens the vendor `SIMCAM CMOS`, exercises typed feature
access, queues two caller-owned buffers, acquires a frame, stops, flushes, and
revokes the buffers. The mock plugin additionally verifies factory properties,
format negotiation, feature and command writes, layout invalidation, metadata,
lease recycling, and pause/restart behavior.

The SPA Header sequence is currently a completion counter and its PTS is host
monotonic time. Camera timestamps and frame identifiers require parsing SDK3
metadata and are not claimed here. Physical camera transport, dropped-frame
behavior, failure recovery, latency, and real-time suitability remain hardware
qualification work.
