# PipeWireAO SPA plugins

This repository is the out-of-tree home for native SPA hardware plugins built
for PipeWireAO. Each plugin is a loadable shared object that exports ordinary
SPA factories and uses PipeWireAO's installed public SPA interfaces.

The supported device integrations are `api.alpao.sink`,
`api.egrabber.source`, `api.bgapi2.source`, `api.edtpdv.source`, and
`api.flisdk.source`, and `api.andor3.source`.
`api.fits.source` provides fixed-cadence vector and image-sequence playback and
a preloaded simulated camera row-readout mode. The sources use PipeWireAO-owned
image buffers and regular graph scheduling. eGrabber uses a configured polling
data loop; BGAPI2 and FITS can instead select ordinary eventfd or timerfd
readiness;
proprietary SDKs and CFITSIO remain optional build dependencies.
Proprietary SDKs, drivers, device configuration files, calibration files, and
redistributable binaries do not belong in this repository.

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

The default build also produces `api.calculon.pixel-calibration` as a Rust
SPA factory. During local development, the Cargo workspace expects the
`calculon-algorithms` repository beside this repository. Cargo is used for the
Rust crates; Meson remains the build and installation entry point for the
loadable plugin set.

`libspa-ao-0.2` and `libpipewire-ao-0.3` must resolve to a PipeWireAO
installation. The SPA package must include the ndarray `schema` and `profile`
keys. Enable a development SDK tree explicitly:

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

## Ownership boundary

- PipeWireAO owns generic transport and execution contracts, including native
  ndarray formats, acquisition metadata, semantic-schema and profile
  negotiation, ordinary graph I/O, and selectable data-loop idle policies.
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

## Calculon pixel calibration

The `api.calculon.pixel-calibration` factory converts exact `GRAY8`,
`GRAY16_LE`, or `GRAY16_BE` raw detector frames into `F32_LE`
calibrated-pixel ndarrays. Optional flat and
background artifact ports use Calculon-owned schemas and standard Header
sequence numbers; a node `Props` update activates a complete pair atomically.
Complete-frame operation uses standard `SPA_IO_Buffers`. With
`api.calculon.row-block-rows=N`, its raw port accepts complete U16
`[N,width]` ndarray blocks and publishes complete F32 `[N,width]` blocks.
`api.calculon.frame-assembly` reconstructs complete frames for the remaining
algorithms and observers. Frame assembly is schema-configured and byte
preserving: it accepts any prepared rank-two ndarray with a standard
fixed-width element type in row-major or column-major layout, including
unclocked arrays with no semantic schema or interpretation profile. Both
factories perform no steady-state heap allocation in `process`.

The adapter and algorithm are Rust. The exported shared object is nevertheless
an ordinary C SPA plugin. See the architecture document for port formats,
factory properties, language rationale, and validation boundary.

## HNü240 Camera Link decoder

The `api.hnu240.decoder` factory keeps the Nüvü pixel unpacking downstream of
the camera driver. It accepts the exact raw Camera Link carrier as `GRAY8`,
1408 by 131, and publishes `GRAY16_LE`, 240 by 242. The two extra output rows
are the detector overscan rows. Construction requires
`api.hnu240.frame-rate` and
`api.hnu240.transport-profile=hnu240-cl-full-8x8-v1`; the profile prevents the
decoder from silently accepting a different tap layout.

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
spa/plugins/calculon/         Calculon SPA factory build and C ABI tests
crates/calculon-spa-node/     reusable Rust SPA ABI adapter
crates/calculon-spa-plugins/  Rust algorithm factories
```
