# eGrabber Camera Link control

The eGrabber SPA plugin can control an attached Camera Link camera through a
standard CLProtocol provider when the Grablink GenTL device does not expose the
camera's GenApi node map. This path is optional. It does not affect Gigelink,
Coaxlink, or SDK-disabled builds.

The source opens the documented Grablink `DeviceModule` serial interface,
loads a CLProtocol 1.x provider, retrieves its GenApi XML, and constructs a node
map with the GenICam Reference Implementation C API. The resulting scalar
features use the same `SPA_PARAM_PropInfo` and `SPA_PARAM_Props` interface as
features obtained from eGrabber's `RemoteModule`. Both paths publish the
category path in `SPA_PROP_INFO_group` and GenICam visibility in
`SPA_PROP_INFO_visibility`.

## Build

Keep the complete GenICam `Reference Implementation` tree together so the
runtime can find its adjacent libraries. Enable the integration with the root
that contains that directory:

```console
meson setup build-cameras \
  -Degrabber=enabled \
  -Dgenicam-root=/opt/genicam/2025.10
meson compile -C build-cameras
```

The current build contract supports the Linux x86-64 runtime at
`Reference Implementation/bin/Linux64_x64/libGenApiC_v3.so`. A build without
`genicam-root` does not compile the serial adapter or CLProtocol backend and
has no GenApi runtime dependency.

The optional `clprotocol-test-backend` Meson option registers a host/provider
integration test. It exercises provider probing, XML node-map creation,
feature reads and writes, attached-camera serial checking, and cleanup after
construction failures. It does not exercise a Grablink board or its serial
transport.

## SPA properties

| Property | Meaning |
| --- | --- |
| `api.egrabber.control` | `auto`, `remote`, `clprotocol`, or `none`. `clprotocol` makes provider failure fatal. |
| `api.egrabber.clprotocol-libraries` | Colon-separated provider libraries or provider roots, tried in order. |
| `api.egrabber.clprotocol-device` | Optional provider device-ID template that restricts probing. |
| `api.egrabber.camera-serial` | Expected serial of the camera attached behind the selected Grablink port. A mismatch is fatal. |
| `api.egrabber.genapi-runtime` | Optional explicit `libGenApiC_v3.so` path. |
| `api.egrabber.control-timeout-ms` | Positive serial-operation timeout in milliseconds; default 1000. |

A provider root can contain a `Linux64_x64` directory with one or more
`libCLProtocol_*.so` files. The plugin also searches the legacy
`EGRABBER_CLPROTOCOL_LIBRARY` and standard `GENICAM_CLPROTOCOL` environment
variables. Explicit SPA properties are preferable because the manager copies
them through its device object into the source configuration.

`auto` uses CLProtocol only for a Grablink/Camera Link endpoint when a provider
is present. A failed automatic probe falls back to the eGrabber RemoteModule,
unless `api.egrabber.camera-serial` was specified. `clprotocol` never falls
back.

## Two serial identities

The transport serial and attached-camera serial identify different objects:

- `api.egrabber.serial` selects the Grablink GenTL device or port.
- `api.egrabber.camera-serial` verifies the camera attached to that selected
  port.

The source keeps `api.egrabber.serial` and the standard device serial stable as
the endpoint identity used by the manager-to-source chain. After a successful
CLProtocol probe, it publishes the provider-reported camera serial separately
as `api.egrabber.camera-serial`.

The startup manager can enumerate the Grablink endpoint identity supplied by
the GenTL producer. It cannot discover the attached camera serial without
opening that endpoint, opening its serial channel, selecting a matching
CLProtocol provider, and reading the provider's `DeviceSerialNumber` feature.
That check therefore occurs during source construction, after transport
selection. It is an identity constraint, not a second discovery selector.

This is the same lifecycle distinction exposed by BGAPI2 discovery on the
connected Euresys Gigelink producer: some producer metadata is populated only
after a bounded device open. BGAPI2 can enrich its startup snapshot with a
read-only open. Camera Link camera identity is different because it requires a
second protocol behind the already-selected frame-grabber endpoint.

## Qualification boundary

The provider and GenApi layer is host-tested with the C-RED2 provider. The
Grablink serial implementation follows the installed eGrabber serial script's
`SerialOperationSelector`, `SerialOperationExecute`, queue-size, access-buffer,
timeout, result, and baud-rate feature contract. No Grablink hardware is
available on the current host, so serial open/read/write, provider probing on a
physical camera, camera-serial verification, and control/layout synchronization
remain hardware qualification items.
