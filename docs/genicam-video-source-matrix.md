# GenICam Video/Source matrix

This matrix defines the GUI-visible GenICam control contract shared by the
eGrabber and BGAPI2 `Video/Source` adapters. Vendor discovery, exception
containment, buffer ownership, and acquisition calls remain inside each SDK
adapter.

| Contract | eGrabber | BGAPI2 | Evidence |
| --- | --- | --- | --- |
| Canonical property name | `genicam.<NodeName>` | `genicam.<NodeName>` | Connected source control enumeration |
| Scalar node types | Boolean, Integer, Float, Enumeration, String | Boolean, Integer, Float, Enumeration, String | Connected camera and source tests |
| Category tree | `SPA_PROP_INFO_group` | `SPA_PROP_INFO_group` | Connected camera metadata tests and GUI parser tests |
| Visibility | Beginner, Expert, Guru, Invisible | Beginner, Expert, Guru, Invisible | Connected camera metadata tests and GUI filtering tests |
| Hover help | Tooltip, then Description | Tooltip, then Description | Connected feature discovery |
| Enum write | Published string label or numeric index | Published string label or numeric index | Connected source tests write `PixelFormat` by label |
| Integer/Float write PODs | Native wide type and SPA Int/Float compatibility | Native wide type and SPA Int/Float compatibility | Source parser tests and connected control writes |
| Live scalar write | Allowed when the node is currently writable | Allowed when the node is currently writable | Connected source test writes an advertised scalar after Start |
| Live `OffsetX`/`OffsetY` | Non-payload-layout control | Non-payload-layout control | Shared policy test; connected source test attempts writable `OffsetX` after Start |
| `Width`/`Height`/`PixelFormat` | Stopped acquisition and released pool | Stopped acquisition and released pool | Connected source tests reject Width with a pool and accept it after release |
| Acquisition lifecycle | Start, Pause, Suspend | Start, Pause, Suspend | Connected pause/restart source tests |
| Complete-frame formats | Mono8, unpacked Mono10/12/14/16 | Mono8, unpacked Mono10/12/14/16 | Connected format and capture tests |
| Frame metadata | Header and Acquisition v2 | Header and Acquisition v2 | Connected ten-frame source tests |
| Queue compatibility | Ordinary `SPA_IO_Buffers` output | Ordinary `SPA_IO_Buffers` output | Direct pool/capture tests and queue integration contract |

The common code is intentionally limited to stable GenICam policy. It currently
owns payload-layout classification. A future shared feature catalog may own SPA
POD serialization and validation, but it must use adapter callbacks for dynamic
access state, ranges, enum entries, and reads/writes. It must not wrap vendor
node objects or SDK lifecycle calls.

Adapter-specific capabilities are not parity defects. eGrabber additionally
supports its qualified row-block and DMA-BUF paths. BGAPI2 additionally supports
its callback/SPSC completion boundary and selectable `poll` or `eventfd`
readiness. Those features depend on their respective transport contracts and do
not change the common GenICam control surface.

## Validation configurations

| Configuration | Purpose |
| --- | --- |
| Shared software test | Exact payload-layout and live-control classification |
| eGrabber connected capture | Native eGrabber control and frame lifecycle |
| BGAPI2 with Euresys Gigelink CTI | BGAPI2 adapter with Euresys producer, `poll` and `eventfd` |
| BGAPI2 with Baumer GigE CTI | Same BGAPI2 contract with the Baumer producer |

Run the focused matrix from `pipewireao-spa-plugins`:

```sh
meson test -C build-cameras --print-errorlogs \
  spa-genicam-feature spa-egrabber-feature \
  spa-bgapi2-camera-euresys spa-bgapi2-capture-euresys \
  spa-bgapi2-capture-eventfd-euresys \
  spa-bgapi2-camera-baumer spa-bgapi2-capture-baumer
```
