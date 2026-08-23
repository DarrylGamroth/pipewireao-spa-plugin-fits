# PipeWireAO SPA plugins

This repository is the out-of-tree home for native SPA hardware plugins built
for PipeWireAO. Each plugin is a loadable shared object that exports ordinary
SPA factories and uses PipeWireAO's installed public SPA interfaces.

The repository is documentation-only while its first plugin is designed. The
initial target is an ALPAO deformable-mirror sink. Proprietary SDKs, drivers,
device configuration files, calibration files, and redistributable binaries do
not belong in this repository.

The accepted repository and interface boundary is recorded in
[Device plugin architecture](docs/device-plugin-architecture.md).

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

## Planned layout

```text
docs/                         maintained contracts and qualification records
include/pipewireao-plugins/   shared plugin-owned public vocabulary, if needed
spa/plugins/alpao/            ALPAO SPA factories and optional SDK backend
tests/                        SDK-independent and hardware qualification tests
```

Algorithm packaging is a separate architecture decision. This repository does
not assume that numerical algorithms are SPA plugins merely because their
language-local API resembles `spa_node`.
