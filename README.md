# PipeWireAO SPA plugins

This repository is the out-of-tree home for native SPA hardware plugins built
for PipeWireAO. Each plugin is a loadable shared object that exports ordinary
SPA factories and uses PipeWireAO's installed public SPA interfaces.

The first implementation is `api.alpao.sink`, an ALPAO deformable-mirror sink
for normalized actuator commands. Its test suite has an SDK-independent mock,
and the installed plugin can optionally use the proprietary ALPAO SDK (ASDK).
Proprietary SDKs, drivers, device configuration files, calibration files, and
redistributable binaries do not belong in this repository.

The accepted repository and interface boundary is recorded in
[Device plugin architecture](docs/device-plugin-architecture.md).

## Build

The default build does not inspect or link ASDK:

```console
meson setup build -Dalpao-sdk=disabled
meson compile -C build
meson test -C build --print-errorlogs
```

`libspa-ao-0.2` must resolve to a PipeWireAO installation that includes the
ndarray `schema` and `profile` keys. Enable a development SDK tree explicitly:

```console
meson setup build-asdk \
  -Dalpao-sdk=enabled \
  -Dalpao-sdk-root=/path/to/alpao
meson compile -C build-asdk
```

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

## ALPAO sink

Factory construction requires these properties:

| Property | Value |
| --- | --- |
| `api.alpao.backend` | `asdk` in the installed plugin; the non-installed test plugin accepts `mock`. |
| `api.alpao.actuator-count` | Positive decimal actuator count. |
| `api.alpao.profile` | Lowercase `sha256:` identifier for the trusted command profile. |
| `api.alpao.serial` | Required by `asdk`; omitted by the test mock. |

The single input port accepts only the exact format documented in
[normalized actuator command schema](docs/schemas/alpao-normalized-actuator-command-1.md).
The node opens ASDK on `Start`, verifies `NbOfActuator`, resets the mirror, and
then consumes the latest submitted command. `Pause` and `Suspend` reset and
release the mirror.

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

The source repository containing a plugin is not part of its runtime identity.
An out-of-tree plugin remains a native SPA plugin when it builds against the
installed PipeWireAO SPA API and installs into the configured PipeWireAO SPA
plugin directory.

## Layout

```text
docs/                         maintained contracts and qualification records
include/pipewireao-plugins/   public C vocabulary for plugin factories
spa/plugins/alpao/            ALPAO SPA factories and optional SDK backend
```

Algorithm packaging is a separate architecture decision. This repository does
not assume that numerical algorithms are SPA plugins merely because their
language-local API resembles `spa_node`.
