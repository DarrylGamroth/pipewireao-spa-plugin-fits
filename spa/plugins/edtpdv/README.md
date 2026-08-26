# EDT PDV camera source

`api.edtpdv.source` captures raw monochrome images from an EDT PCI DV/PDV
frame grabber through the EDT PDV 6.2.1 C SDK. The host must install and
configure the EDT driver and SDK, normally under `/opt/EDTpdv`, and initialize
the selected camera with EDT's `initcam` tool before constructing the node.

The source is a regular PipeWireAO polling driver. One nonblocking
`edt_done_count()` probe is performed per graph cycle. When a frame completes,
the latest raw EDT DMA image is copied into an available SPA pool buffer. The
copy isolates EDT's continuously reused DMA ring from graph buffer ownership.
The node publishes `Header` and `Acquisition` metadata, marks sequence gaps as
discontinuities, and marks EDT timeouts or overruns as corrupted chunks.

Supported camera configurations are:

- 8-bit monochrome as `GRAY8`;
- unpacked 10-, 12-, 14-, or 16-bit monochrome as `GRAY16_LE`.

Packed pixels and EDT post-acquisition deinterleave transforms are not
supported. The source deliberately reads the raw completed DMA image, and it
rejects configurations whose pitch does not match an unpacked representation.

Factory properties:

| Property | Meaning |
| --- | --- |
| `api.edtpdv.device` | EDT interface name, normally `pdv` (required). |
| `api.edtpdv.unit` | Zero-based board unit (required). |
| `api.edtpdv.channel` | Zero-based board channel (required). |
| `api.edtpdv.ring-buffers` | EDT-owned DMA ring size, 2 through 64; default 4. |
| `api.edtpdv.readiness` | Must be `poll` when present. |

Build against a normally installed package with:

```console
meson setup build-edtpdv -Dedtpdv=enabled
meson compile -C build-edtpdv
```

`-Dedtpdv-prefix=PATH` selects a nonstandard installed SDK prefix. For local
development only, `-Dedtpdv-deb=/path/to/edtpdv_6.2.1_amd64.deb` extracts the
package into a checksum-keyed SDK directory in the build tree. The installed
plugin still records `/opt/EDTpdv` as its runtime search path for that package
layout.

The SDK-independent mock tests validate enumeration, property rejection, format
negotiation, buffer ownership, metadata, publication, recycling, and lifecycle.
Connected-camera qualification must additionally verify the selected camera
configuration, raw pixel layout, trigger behavior, timeout recovery, frame
sequence continuity, and sustainable copy latency.
