# PipeWireAO SPA plugins

This repository is the out-of-tree home for native SPA hardware plugins and
small development endpoints built for PipeWireAO. Each plugin is a loadable
shared object that exports ordinary SPA factories and uses PipeWireAO's
installed public SPA interfaces.

The supported device integrations are `api.alpao.sink`,
`api.egrabber.source`, `api.bgapi2.source`, `api.edtpdv.source`, and
`api.flisdk.source`, and `api.andor3.source`.
`api.fits.source` provides fixed-cadence vector and image-sequence playback and
a preloaded simulated camera row-readout mode. The sources use PipeWireAO-owned
image buffers and regular graph scheduling. eGrabber uses a configured polling
data loop; BGAPI2 and FITS can instead select ordinary eventfd or timerfd
readiness;
proprietary SDKs and CFITSIO remain optional build dependencies.
The optional `api.imagestreamio.source` and `api.imagestreamio.sink` factories
bridge ordinary ndarray buffers to milk ImageStreamIO shared-memory streams.
The `api.pyrtc.source` and `api.pyrtc.sink` factories bridge CPU-backed pyRTC
`ImageSHM` streams without embedding Python or NumPy.
Proprietary SDKs, drivers, device configuration files, calibration files, and
redistributable binaries do not belong in this repository.

The SDK-independent `api.pipewireao.discard` factory is a format-agnostic,
non-actuating follower for development and integration graphs. It is not a
simulated hardware device.

The accepted repository and interface boundary is recorded in
[Device plugin architecture](docs/device-plugin-architecture.md).
The GUI-visible contract shared by the supported GenICam adapters is tracked in
the [GenICam Video/Source matrix](docs/genicam-video-source-matrix.md).
The separate scientific-algorithm boundary is recorded in
[Algorithm plugin architecture](docs/algorithm-plugin-architecture.md).
The generic bounded handoff for isolating telemetry, GUI, and recorder graphs
is specified in [Bounded queue module](docs/queue.md).
The complete-frame and row-block topology is described in
[Scheduled node and row-block migration](docs/scheduled-node-migration.md).

## Build

The default build does not inspect or link ASDK:

```console
meson setup build -Dalpao-sdk=disabled
meson compile -C build
meson test -C build --print-errorlogs
```

The default build also produces the Rust `api.ndarray.video-view`,
`api.ndarray.frame-assembly`, and `api.hnu240.decoder` SPA factories plus the
C `api.pipewireao.discard` sink and ALPAO FGN operator. Scientific
Calculon operators are built from the adjacent `calculon-algorithms`
repository as `libcalculon-fgn.so`; this repository no longer compiles a
second native SPA implementation of them. Cargo builds the Rust transport
transforms, while Meson remains the build and installation entry point.

`libspa-ao-0.2` and `libpipewire-ao-0.3` must resolve to a PipeWireAO
installation. The SPA package must include the ndarray `schema` key. Enable a
development SDK tree explicitly:

```console
meson setup build-asdk \
  -Dalpao-sdk=enabled \
  -Dalpao-sdk-root=/path/to/alpao
meson compile -C build-asdk
```

Build both camera plugins against an installed PipeWireAO development package
with:

```console
meson setup build-cameras \
  -Degrabber=enabled \
  -Dbgapi2=enabled
meson compile -C build-cameras
meson test -C build-cameras --print-errorlogs \
  'spa-egrabber*' 'spa-bgapi2*'
```

`-Degrabber-prefix=PATH` and `-Dbgapi2-prefix=PATH` override the default SDK
locations. A development build may point Meson's `pkg_config_path` at a
PipeWireAO `build/meson-uninstalled` directory; no source include flags are
required.

Build the EDT PDV source against the normally installed `/opt/EDTpdv` SDK with
`-Dedtpdv=enabled`. For development without installing the vendor package,
`-Dedtpdv-deb=~/edtpdv_6.2.1_amd64.deb` extracts a checksum-keyed build-tree
SDK while keeping `/opt/EDTpdv` as the installed runtime location. See
[EDT PDV camera source](spa/plugins/edtpdv/README.md) for its factory
properties, supported raw formats, and qualification boundary.

Build the First Light Imaging source against the normally installed
`/opt/FirstLightImaging/FliSdk` SDK with `-Dflisdk=enabled`. For development,
`-Dflisdk-run=~/FliSdk_2_9_3_Ubuntu_20_04_NoGui.run` extracts only the C/C++
SDK payload without launching the vendor installer. See
[FliSdk camera source](spa/plugins/flisdk/README.md) for capture ownership,
factory properties, pixel signedness, and the hardware qualification boundary.

Build the Andor SDK3 source against a normal host installation with
`-Dandor3=enabled`. For the unpacked development bundle in `~/andor3`, use
`-Dandor3-prefix=~/andor3`; the build creates a private runtime link farm for
the bundle's versioned-only libraries. See
[Andor SDK3 camera source](spa/plugins/andor3/README.md) for direct buffer
ownership, supported pixel encodings, typed `andor3.*` features, and the
qualification boundary.

The `api.aravis.source` comparison implementation is retained as an
experimental, opt-in plugin with `-Daravis=enabled`. It is not a supported RTC
camera backend: measured GenTL completion polling through Aravis and Euresys
Gigelink was slower than the direct eGrabber and BGAPI2 integrations. Its
native GigE Vision path also provides experimental copied row-block output for
progressive-readout evaluation. See
[Aravis comparison source](spa/plugins/aravis/README.md) for its scope and
manual test commands.

Enable or require the FITS source with `-Dfits=enabled`. Its default `file`
profile reads each scheduled plane through CFITSIO directly into a PipeWireAO
pool buffer. Its row mode preloads U16 frames and publishes scheduled ordinary
row-block ndarrays for camera-pipeline qualification. See
[FITS sequence source](spa/plugins/fits/README.md) for axis, schema, cadence,
`GRAY16_LE`, simulated readout, and optional mmap behavior.

Enable the ImageStreamIO bridge with `-Dimagestreamio=enabled`. An installation
outside `/usr/local` is selected with `-Dimagestreamio-prefix=PATH`. See the
[ImageStreamIO bridge](spa/plugins/imagestreamio/README.md) for stream
ownership, ndarray type and shape mapping, semaphore behavior, and the
intentional copy boundary.

The pyRTC bridge is built by default. See the
[pyRTC shared-memory bridge](spa/plugins/pyrtc/README.md) for `ImageSHM`
metadata compatibility, ownership, dtype and shape mapping, and pyRTC's CPU
snapshot-coherence limit.

Optional Camera Link control through Grablink, CLProtocol, and the GenICam
Reference Implementation is enabled with `-Dgenicam-root=PATH`. It supplies
CLProtocol/GenApi control to both the eGrabber and FliSDK camera sources. See
[eGrabber Camera Link control](docs/egrabber-clprotocol.md) for eGrabber properties,
and the [FliSDK camera source](spa/plugins/flisdk/README.md) for the FliSDK serial
adapter and its `api.flisdk.*` properties. The eGrabber guide also defines its
two-level serial identity and hardware qualification boundary.

The ASDK simulator smoke test also uses
[alpao-binary-config](https://github.com/DarrylGamroth/alpao-binary-config) to
create a synthetic calibration in the build tree:

```console
meson configure build-asdk \
  -Dalpao-binary-config=/path/to/alpao-binary-config/tools/alpao_binary_config.py
meson test -C build-asdk --print-errorlogs
```

The simulator test loads configuration and exercises ASDK calls. It does not
validate electronics, timing, packets, or mirror motion.

An optional capture-interface integration test and benchmark exercise the
complete SPA-to-ASDK packing path with 468 actuators. They use the host-only
`libait_capture.so` supplied by `alpao-binary-config`, not an Ethernet or PEX
interface:

```console
meson setup build-capture --buildtype=release \
  -Dalpao-sdk=enabled \
  -Dalpao-sdk-root=/path/to/alpao \
  -Dalpao-binary-config=/path/to/alpao-binary-config/tools/alpao_binary_config.py \
  -Dalpao-capture-plugin-dir=/path/to/alpao-binary-config/interfaces/capture/build
meson test -C build-capture spa-alpao-asdk-capture --print-errorlogs
meson test -C build-capture --benchmark spa-alpao-asdk-capture-latency --verbose
```

See [ALPAO capture-interface benchmark](docs/alpao-capture-benchmark.md) for
the timestamp boundaries, fixed-rate and `SCHED_FIFO` commands, workload
matrix, raw results, and claim limits.

## ALPAO sink

Factory construction accepts these properties:

| Property | Value |
| --- | --- |
| `api.alpao.backend` | `asdk` in the installed plugin; the non-installed test plugin accepts `mock`. |
| `api.alpao.actuator-count` | Positive decimal actuator count. |
| `api.alpao.daq-frequency` | Optional DEv7 `daqFreq` override in integer hertz, from 1,000 through 20,000,000. |
| `api.alpao.profile` | Lowercase `sha256:` identifier for the trusted command profile. |
| `api.alpao.serial` | Required by `asdk`; omitted by the test mock. |

The single input port accepts only the exact format documented in
[normalized actuator command schema](docs/schemas/alpao-normalized-actuator-command-1.md).
The node opens ASDK on `Start`, verifies `NbOfActuator`, applies a configured
`daqFreq` override, resets the mirror, and then consumes ordinary scheduled
command buffers. Startup fails if ASDK rejects the requested frequency. `Pause` and
`Suspend` reset and release the mirror.

`api.alpao.daq-frequency` controls the supported ALPAO interface's digital to
analog conversion rate. It is not the producer command cadence and therefore
does not populate the ndarray `rate` property.

The plugin is implemented in C and uses the ASDK C wrapper (`asdkInit`,
`asdkSend`, and related functions). C is the default language for these SPA
plugins. C++ is reserved for vendor SDKs that require it, such as eGrabber.

The mock is required for contract tests, but it is compiled only into a
non-installed test plugin. The installed hardware plugin does not silently
discard actuator commands when ASDK is absent or unavailable.

The ALPAO-owned FGN library installs as
`pipewire-ao/filter-graph/libalpao-fgn.so`. Its
`command-normalization-f32-f64` operator converts physical F32 demanded PDM
commands into the normalized F64 command schema consumed by the sink. The
construction object requires `actuator_count`, positive `command_scale`, the
exact lowercase SHA-256 `profile`, and positive `rate_numerator` and
`rate_denominator` values. The profile is deployment configuration shared with
the sink, not an ndarray format field. Only the demanded-command input exposes
the graph activation rate. Processing rejects non-finite values and normalized
results outside `[-1,+1]`.

Example FGN node declaration:

```ini
{ type = ndarray
  name = alpao-normalization
  plugin = /path/to/libalpao-fgn.so
  label = command-normalization-f32-f64
  config = {
    actuator_count = 468
    command_scale = 1.0
    profile = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    rate_numerator = 2000
    rate_denominator = 1
  }
}
```

## Format-agnostic discard sink

`api.pipewireao.discard` is a one-port, non-driving SPA sink analogous to
`/dev/null`. Its `in` port accepts any fixated SPA `Format` object. It accepts
standard PipeWire buffer pools with any number of data blocks and any standard
memory type; the processing callback does not map, read, copy, or interpret
payload memory. Each `SPA_STATUS_HAVE_DATA` buffer is returned immediately as
`SPA_STATUS_NEED_DATA`.

The sink starts only after a format, buffer pool, and `SPA_IO_Buffers` area are
installed. `Pause` and `Suspend` stop consumption without discarding the
negotiated resources, so a later `Start` resumes processing. The sink has no
timer, driver, queue, background thread, or hardware authority.

The node exposes these cumulative, read-only `SPA_PARAM_Props` counters:

| Property name | Meaning |
| --- | --- |
| `discard.buffers` | Buffers returned since node construction. |
| `discard.data-blocks` | Data blocks described by those buffers. |
| `discard.bytes` | Sum of the chunk sizes advertised by those data blocks. |
| `discard.protocol-errors` | Invalid input buffer references rejected by the processing callback. |
| `discard.process-calls` | Processing callback calls, including calls with no input available. |

The counters saturate at `INT64_MAX` and are not reset by pause or restart.
They are snapshots obtained by ordinary parameter enumeration; the processing
callback does not emit metric events, and observing metrics cannot pace the
sink.

## Ownership boundary

- PipeWireAO owns generic transport and execution contracts, including native
  ndarray formats, acquisition metadata, semantic-schema negotiation, ordinary
  graph I/O, and selectable data-loop idle policies.
- This repository owns the optional `libpipewire-module-queue` topology adapter
  that applies an explicit finite capacity, overflow policy, and copy or lease
  storage boundary between producer and observer graphs.
- This repository owns vendor adapters, their device-native schemas, optional
  SDK integration, and hardware qualification.
- Scientific projects such as Calculon own their scientific schemas and any
  explicit conversion between canonical scientific quantities and a
  device-native command schema.
- Hardware vendors own their SDK ABI, device protocol, configuration, and
  calibration artifacts.

## Raw-video ndarray view

`api.ndarray.video-view` exposes an exact packed `GRAY8` or `GRAY16_LE` frame
as a row-major U8 or U16 ndarray with a configured schema. Compatible
SPA allocation uses the same storage; otherwise the adapter copies each packed
row once. It performs no pixel conversion. This is the structural bridge from
complete-frame camera sources and `api.hnu240.decoder` to Calculon FGN pixel
calibration.

## Ndarray frame assembly

`api.ndarray.frame-assembly` reconstructs complete frames from immutable
row-block ndarrays. It is schema-configured and byte-preserving, accepts every
standard fixed-width SPA element type in row-major or column-major layout, and
supports unclocked arrays with no semantic schema.
Complete input frames use shared-storage forwarding when available; row-block
assembly uses one preallocated workspace. Processing performs no steady-state
heap allocation.

Calculon pixel calibration, wavefront measurement, reconstruction, and control
now use the ndarray FGN bundle in `calculon-algorithms`. See the architecture
document for the ownership split and frame-assembly properties.

## HNü240 Camera Link decoder

The `api.hnu240.decoder` factory keeps the Nüvü pixel unpacking downstream of
the camera driver. It accepts the exact raw Camera Link carrier as `GRAY8`,
1408 by 131, discards the ten leading non-active carrier rows and final per-tap
overscan line, and publishes the active `GRAY16_LE` detector area as 240 by 240.
Construction requires
`api.hnu240.frame-rate` and
`api.hnu240.transport-profile=hnu240-cl-full-8x8-v1`; the profile prevents the
decoder from silently accepting a different tap layout.

The eight CCD220 outputs do not appear on Camera Link as conventional 16-bit
taps. The camera uses a custom eight-tap, 8-bit carrier with 16 unused bytes in
every 64-byte packing group. Standard `1X8` plus `Mono16` frame-grabber
configuration cannot replace the camera-specific unpacker. The
[HNü240 Camera Link carrier note](docs/hnu240-camera-link-carrier.md) records
the packing authority, EDT and iPORT implications, comparison with camstack's
OCAM configuration, and hardware validation checklist.

The progressive path will consume carrier row blocks from the native Aravis
GigE Vision receiver and publish acquisition-ordered U16 detector readout
blocks with shape `[8, N, 60]`. The Nüvü transform owns carrier-byte decoding,
non-active carrier removal, overscan removal, and tap order. The prepared
detector readout mapping owns placement and direction in the active 240 by 240
detector area; complete-frame assembly remains optional downstream.

The carrier row block is device-native encoded data, not a raw detector-pixel
row block. Aravis must therefore preserve a Nüvü-owned carrier schema until the
decoder produces the raw-pixel detector readout block; the selected transport
configuration remains deployment identity outside the ndarray format. If
the iPORT advertises the complete 131-row carrier, Aravis's current fixed block
height can only be one row because it must divide the height. Production use
needs either an iPORT carrier-row window or an admitted Aravis row-selection
and batching contract. In either case, the decoder withholds its final active
block until terminal carrier validity is known, then discards the overscan
content rather than publishing it.

The corresponding `CLProtocol_hnu240` project supplies camera control and the
GenApi node map. The `EDTpdvGenTL` producer supplies raw EDT DMA frames to the
existing BGAPI2 source. Neither layer embeds this decoder.

The source repository containing a plugin is not part of its runtime identity.
An out-of-tree plugin remains a native SPA plugin when it builds against the
installed PipeWireAO SPA API and installs into the configured PipeWireAO SPA
plugin directory.

## Layout

```text
docs/                         maintained contracts and qualification records
include/pipewireao-plugins/   public C vocabulary for plugin factories
src/modules/                  out-of-tree PipeWire topology modules
spa/plugins/alpao/            ALPAO SPA factories and optional SDK backend
spa/plugins/egrabber/         Euresys camera manager, device, and source factories
spa/plugins/bgapi2/           Baumer GAPI camera source factory
spa/plugins/edtpdv/           EDT PCI DV/PDV Camera Link source factory
spa/plugins/flisdk/           First Light Imaging FliSdk camera source factory
spa/plugins/andor3/           Andor SDK3 camera source factory
spa/plugins/aravis/           experimental Aravis GenTL/native-GV comparison source
spa/plugins/fits/             CFITSIO vector and image-sequence source factory
spa/plugins/imagestreamio/    ImageStreamIO ndarray source and sink factories
spa/plugins/ndarray/          generic video-view and frame-assembly factories
spa/plugins/nuvu/             Nüvü camera decoder SPA factory and tests
spa/plugins/pyrtc/            pyRTC ImageSHM ndarray source and sink factories
crates/pipewireao-spa-node/   reusable Rust SPA ABI adapter
crates/ndarray-spa-plugin/    generic ndarray transport transform
crates/nuvu-spa-plugin/       HNü240 carrier decoder
```
