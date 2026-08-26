# First Light Imaging FliSdk camera source

`api.flisdk.source` captures raw monochrome images through the First Light
Imaging FliSdk 2.9.3 C API. Production hosts must install FliSdk and the
appropriate frame-grabber integration, normally under
`/opt/FirstLightImaging/FliSdk`, and complete the vendor's camera and grabber
configuration before constructing the node.

The source is a regular PipeWireAO polling driver. It disables the FliSdk ring
buffer and registers a `beforeCopy` new-image callback. That callback copies the
frame-grabber image directly into the next available SPA pool buffer. FliSdk
therefore does not make its usual ring-buffer copy, and the plugin makes the
single copy required to transfer ownership into the graph. If no SPA buffer is
available, the frame is dropped; the next published Header sequence exposes the
gap as a discontinuity.

Supported SDK pixel layouts are:

- Mono8 as `GRAY8`;
- the SDK's two-byte raw monochrome layout as `GRAY16_LE`.

The bytes are not processed or converted. `api.flisdk.pixel-sign` reports
`unsigned` or `signed` because the video format itself does not distinguish
those two FliSdk interpretations. Consumers that require unsigned scientific
samples must reject `signed` or apply an explicit conversion.

Factory properties:

| Property | Meaning |
| --- | --- |
| `api.flisdk.camera` | Exact camera name returned by FliSdk detection (required). |
| `api.flisdk.grabber` | Optional exact grabber name to select before camera setup. |
| `api.flisdk.readiness` | Must be `poll` when present. |
| `api.flisdk.pixel-sign` | Read-only node property: `unsigned` or `signed`. |
| `api.flisdk.control` | `none` (default) or `clprotocol`. |
| `api.flisdk.clprotocol-libraries` | Colon-separated CLProtocol provider files or directories. |
| `api.flisdk.clprotocol-device` | Optional short device-ID template, such as `CRED2`. |
| `api.flisdk.camera-serial` | Optional serial number that the GenApi `DeviceSerialNumber` node must report. |
| `api.flisdk.genapi-runtime` | Optional path to `libGenApiC_v3.so`. |
| `api.flisdk.control-timeout-ms` | CLProtocol operation timeout in milliseconds; default `1000`. |

## C-RED control through CLProtocol

Configure with a GenICam Reference Implementation to add CLProtocol support:

```console
meson setup build-flisdk \
  -Dflisdk=enabled \
  -Dgenicam-root=/opt/genicam \
  -Dflisdk-run=~/FliSdk_2_9_3_Ubuntu_20_04_NoGui.run
meson compile -C build-flisdk
```

Then construct the source with at least:

```ini
api.flisdk.camera = C-RED 2
api.flisdk.control = clprotocol
api.flisdk.clprotocol-libraries = /usr/local/lib/libCLProtocol_cred2.so
```

The node publishes the provider's scalar GenApi nodes as `genicam.<NodeName>`
entries in `SPA_PARAM_PropInfo` and `SPA_PARAM_Props`. Commands are write-only
boolean entries named `genicam-command.<NodeName>`; writing `true` executes the
command. For example, `genicam.AcquisitionFrameRate`, `genicam.ExposureTime`,
and `genicam-command.TriggerSoftware` use the same CRED-2 CLProtocol provider as
the eGrabber source.

FliSDK remains in `Full` mode and owns both acquisition and the camera serial
connection. The adapter turns each CLProtocol serial write/read transaction into
one `FliSerialCamera_sendCommand_V2` call, so it does not open a second serial
owner through eGrabber or CLAllSerial.

GenApi writes are accepted only while acquisition is paused. Changes that can
alter payload layout additionally require all SPA buffers to be released. After
such a change, the plugin applies the provider's `Width` and `Height` through
`FliSdk_setImageDimension_V2`, refreshes its SDK layout, clears the negotiated
format, and advertises new format and buffer parameters. The graph must then
negotiate the format and buffers again. A failed layout synchronization rolls
the camera node and FliSDK dimensions back to their previous values.

The FliSDK C command function does not accept a caller-provided timeout. The
timeout property still governs CLProtocol operations, but it cannot preempt a
vendor command already executing inside FliSDK.

Build against a normally installed SDK with:

```console
meson setup build-flisdk -Dflisdk=enabled
meson compile -C build-flisdk
```

`-Dflisdk-prefix=PATH` selects a nonstandard installed SDK prefix. For local
development only, the original bundle can supply headers, `libFliSdk.so`, and
its private runtime dependencies without running the installer:

```console
meson setup build-flisdk \
  -Dflisdk=enabled \
  -Dflisdk-run=~/FliSdk_2_9_3_Ubuntu_20_04_NoGui.run
meson compile -C build-flisdk
```

The extraction helper invokes the Makeself bundle only with `--noexec`, then
extracts the C/C++ payload into a checksum-keyed build directory. It never runs
`startup.sh`, `sudo`, or `FliInstaller`. The installed plugin still records the
normal host SDK location as its runtime search path. Building from the bundle
does not install a frame-grabber driver or its FliSdk integration.

The SDK-independent mock tests validate enumeration, property rejection,
format negotiation, SPA buffer ownership, metadata, publication, recycling,
and lifecycle. Connected-camera qualification must additionally verify camera
and grabber selection, raw signedness, trigger behavior, callback concurrency,
sequence continuity, and sustainable copy latency at the required frame rate.
