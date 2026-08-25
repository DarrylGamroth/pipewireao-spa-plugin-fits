# FITS sequence SPA source

`api.fits.source` publishes one complete plane of a FITS array at a configured
fixed rate. It is a regular PipeWire graph driver with
`SPA_NODE_FLAG_POLL_DRIVER`: its configured busy-spin data loop checks the
monotonic deadline without a timer fd, sleep, eventfd, or private thread.

CFITSIO reads each due plane directly into an ordinary PipeWire output buffer.
Buffered file access is the default. FITS values may require byte-order,
scaling, or type conversion, so mapping the file does not make playback
zero-copy. `mmap` remains available for measurement and for deployments that
prefer a stable virtual mapping; optional prefaulting touches every file page
during construction.

## Factory properties

| Property | Requirement | Meaning |
| --- | --- | --- |
| `api.fits.path` | required | FITS file path; extended-filename parsing is not used |
| `api.fits.hdu` | default `1` | one-based image HDU |
| `api.fits.sample-rank` | default `2` | FITS axes in one sample: `1` for vectors or `2` for images |
| `api.fits.rate` | required | positive `numerator/denominator` samples per second |
| `api.fits.schema` | required | exact ndarray semantic schema |
| `api.fits.profile` | optional | exact ndarray interpretation profile |
| `api.fits.io-mode` | default `file` | `file` or private read-only `mmap` backing |
| `api.fits.prefault` | default `false` | touch mapped pages before startup; valid only with `mmap` |
| `api.fits.loop` | default `true` | wrap after the final plane |

The file axes define repeated values without a separate shape property:

- `sample-rank=1` accepts `(elements)` or `(elements, samples)` and publishes a
  canonical row-major vector. A matching F64 ALPAO command file can negotiate
  directly with `api.alpao.sink`.
- `sample-rank=2` accepts `(width, height)` or `(width, height, frames)` and
  publishes the native FITS element type as a column-major ndarray. It also
  offers raw `GRAY16_LE`; CFITSIO converts directly into the selected output
  buffer.

Element type, shape, layout, rate, schema, and profile are exact constraints.
A mismatched consumer is rejected during negotiation rather than at playback.

## Cadence and overload

Start establishes a `CLOCK_MONOTONIC` epoch. `SPA_META_Header.seq` is the
scheduled plane sequence and `pts` is its absolute release time. The first
plane is due immediately. If processing, storage, or a consumer lease is late,
the source advances to the newest due plane; it never emits a catch-up burst.
The first plane and the first plane after skipped deadlines carry `DISCONT`.

Each polling probe performs one monotonic clock read. A due probe performs one
bounded pool acquisition and one CFITSIO plane read, then returns
`SPA_STATUS_HAVE_DATA` to start a regular graph cycle. The source is not probed
again until that cycle completes. If the ordinary output is still held, the
sample is dropped and the next publication is discontinuous.

CFITSIO and filesystem service time remain part of the deployment contract.
Cache misses, page faults, storage faults, and library internals prevent a
generic strict-real-time claim even though scheduler activation is syscall-free.

The source supports mapped `MemPtr` and `MemFd` pool buffers. It does not offer
DMA-BUF or row-block output. Row-block behavior is implemented at the real
eGrabber boundary and in the Calculon calibration and assembly nodes.
