# Aravis camera source

`api.aravis.source` can use either an Aravis GenTL stream or the native Aravis
GigE Vision stream. It remains an experimental backend and is not yet part of
the qualified PipeWireAO RTC camera set. Measurements with the Euresys
Gigelink GenTL producer showed substantially higher completion-poll cost than
the direct eGrabber and BGAPI2 integrations; those measurements do not apply
to the native receiver.

Select the stream implementation with `api.aravis.transport`:

- `auto` opens the device through Aravis' normal interface selection.
- `gentl` opens it specifically through the Aravis GenTL interface.
- `native-gv` opens it specifically through the Aravis GigE Vision interface.

Complete-frame mode remains the default. Native GV also supports copied,
complete row-block ndarray output:

```text
api.aravis.transport = native-gv
api.aravis.output-mode = row-block
api.aravis.row-block-rows = 8
api.aravis.detector-profile = detector-profile-id
```

Row-block mode requires a positive block height smaller than and dividing the
camera height. It currently accepts only packed `Mono8` or unpacked
little-endian `Mono10`/`Mono12`/`Mono14`/`Mono16` image payloads with no
padding, chunks, or multipart layout. The Aravis receive thread publishes a
contiguous committed payload prefix only after copying packet data. The SPA
node copies complete rows from its private camera allocation into ordinary
immutable `org.calculon.ao.raw-pixel-row-block/1` buffers. The receive thread
does no image processing.

The progressive implementation was derived from the existing Aravis packet
tracker, buffer ownership, fake camera, and tests. It does not depend on EMVA
specification text. Native GV row-block operation is functionally tested with
the Aravis fake camera but is not yet qualified on an iPORT or under packet
loss.

## Pleora iPORT serial control boundary

The iPORT node map exposes serial configuration such as bulk mode, parity, and
baud rate. Public Pleora material describes byte transmission separately:
`PvDeviceSerialPort` sends through the device command link and receives through
the device messaging/event channel. It is not documented as a TCP serial
socket or as ordinary reads and writes to GenApi value nodes.

For that reason this tree does not yet implement an Aravis CLProtocol serial
bridge. A clean implementation needs public documentation or independently
captured behavior for port discovery, transmit commands, receive event IDs and
payload framing, timeouts, and buffer limits. Useful public starting points
are Pleora's [serial-port-link bridge article](https://supportcenter.pleora.com/articles/Knowledge/Establishing-a-Serial-Bridge-through-a-Serial-Port-Link-KBase)
and [CLProtocol camera-bridge article](https://supportcenter.pleora.com/articles/Knowledge_Base_Article/Establishing-a-Camera-Bridge-with-a-CLProtocol-DLL-and-GenICam-CLProtocol-KBase).

Build it explicitly against Aravis 0.10:

```console
meson setup build-aravis -Daravis=enabled
meson compile -C build-aravis
meson test -C build-aravis spa-aravis-factory --print-errorlogs
```

## Isolated fake-camera tests

Do not run the Aravis fake GigE Vision camera directly on a development host
that is also connected to real GigE Vision cameras. Discovery and control use
host networking and can interfere with another consumer such as eGrabber.

`scripts/run-aravis-fake-gv-isolated.py` creates a private user and network
namespace, brings up only its loopback interface, starts the simulator on
`127.0.0.1`, runs one test, and always stops the simulator. The namespace has
no physical interface on which it could send or receive discovery traffic:

```console
scripts/run-aravis-fake-gv-isolated.py \
  /path/to/aravis-build/src/arv-fake-gv-camera-0.10 -- \
  build-aravis/spa/plugins/aravis/spa-aravis-camera-test \
  Aravis-Fake-GV01 native-gv
```

The harness requires Linux, `unshare`, `ip`, and enabled unprivileged user
namespaces. A failure to create the namespace is a test-environment failure;
the harness does not fall back to the host network.

Hardware tests are built but are not registered as automatic tests because a
device identifier and GenTL producer environment are site-specific:

```console
GENICAM_GENTL64_PATH=/path/to/cti-directory \
  build-aravis/spa/plugins/aravis/spa-aravis-camera-test DEVICE-ID

GENICAM_GENTL64_PATH=/path/to/cti-directory \
  build-aravis/spa/plugins/aravis/spa-aravis-capture-test \
  build-aravis/spa/plugins/aravis/libspa-aravis.so DEVICE-ID
```

`spa-aravis-completion-benchmark` remains available for controlled comparison
work. Results from it must not be presented as qualification of the supported
eGrabber or BGAPI2 paths.
