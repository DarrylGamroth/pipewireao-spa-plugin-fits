# PipeWireAO SPA plugins

This repository is the out-of-tree home for native SPA hardware plugins built
for PipeWireAO. Each plugin is a loadable shared object that exports ordinary
SPA factories and uses PipeWireAO's installed public SPA interfaces.

The device integrations are `api.alpao.sink`, `api.egrabber.source`, and
`api.bgapi2.source`. `api.fits.source` provides fixed-cadence vector
and image-sequence playback from FITS arrays. The sources use PipeWireAO-owned
image buffers and RTC node execution directly; proprietary SDKs and CFITSIO
remain optional build dependencies.
Proprietary SDKs, drivers, device configuration files, calibration files, and
redistributable binaries do not belong in this repository.

The accepted repository and interface boundary is recorded in
[Device plugin architecture](docs/device-plugin-architecture.md).
The separate scientific-algorithm boundary is recorded in
[Algorithm plugin architecture](docs/algorithm-plugin-architecture.md).

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

`libspa-ao-0.2` must resolve to a PipeWireAO installation that includes the
ndarray `schema` and `profile` keys. Enable a development SDK tree explicitly:

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

Enable or require the FITS source with `-Dfits=enabled`. Its default `file`
profile reads each scheduled plane through CFITSIO directly into a PipeWireAO
pool buffer. See [FITS sequence source](spa/plugins/fits/README.md) for axis,
schema, cadence, progressive downstream-test output, `GRAY16_LE`, and optional
mmap behavior.

Optional Camera Link control through Grablink, CLProtocol, and the GenICam
Reference Implementation is enabled with `-Dgenicam-root=PATH`. See
[eGrabber Camera Link control](docs/egrabber-clprotocol.md) for its properties,
two-level serial identity, and hardware qualification boundary.

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
`daqFreq` override, resets the mirror, and then consumes the latest submitted
command. Startup fails if ASDK rejects the requested frequency. `Pause` and
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
  ndarray formats, semantic-schema and profile negotiation, latest-buffer I/O,
  and RTC-owned SPA-node execution.
- This repository owns vendor adapters, their device-native schemas, optional
  SDK integration, and hardware qualification.
- Scientific projects such as Calculon own their scientific schemas and any
  explicit conversion between canonical scientific quantities and a
  device-native command schema.
- Hardware vendors own their SDK ABI, device protocol, configuration, and
  calibration artifacts.

## Calculon pixel calibration

The `api.calculon.pixel-calibration` factory converts exact `GRAY16_LE` raw
detector frames into `F32_LE` calibrated-pixel ndarrays. Optional flat and
background artifact ports use Calculon-owned schemas and standard Header
sequence numbers; a node `Props` update activates a complete pair atomically.
The factory uses standard `SPA_IO_Buffers` back pressure and performs no
steady-state heap allocation in `process`.

The adapter and algorithm are Rust. The exported shared object is nevertheless
an ordinary C SPA plugin. See the architecture document for port formats,
factory properties, language rationale, and validation boundary.

The source repository containing a plugin is not part of its runtime identity.
An out-of-tree plugin remains a native SPA plugin when it builds against the
installed PipeWireAO SPA API and installs into the configured PipeWireAO SPA
plugin directory.

## Layout

```text
docs/                         maintained contracts and qualification records
include/pipewireao-plugins/   public C vocabulary for plugin factories
spa/plugins/alpao/            ALPAO SPA factories and optional SDK backend
spa/plugins/egrabber/         Euresys camera manager, device, and source factories
spa/plugins/bgapi2/           Baumer GAPI camera source factory
spa/plugins/fits/             CFITSIO vector and image-sequence source factory
spa/plugins/calculon/         Calculon SPA factory build and C ABI tests
crates/calculon-spa-node/     reusable Rust SPA ABI adapter
crates/calculon-spa-plugins/  Rust algorithm factories
```
