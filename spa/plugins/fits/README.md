# FITS sequence SPA source

`api.fits.source` publishes one plane of a FITS array at a configured fixed
rate. It is a native SPA source: it uses PipeWireAO's image-source pool,
latest-buffer fan-out, live link lifecycle, and RTC-owned node execution. It
does not create an application queue or a private thread.

CFITSIO reads each due plane directly into an acquired PipeWireAO buffer.
Ordinary buffered file access is the default. This is preferable for
sequential playback because FITS values still require byte-order, scaling, or
type conversion before publication; mapping a FITS file does not make those
planes zero-copy. `mmap` remains available for measurement and for deployments
that prefer a stable virtual mapping. Its optional prefault pass touches every
file page during source construction.

## Factory properties

| Property | Requirement | Meaning |
| --- | --- | --- |
| `api.fits.path` | required | FITS file path. Extended-filename parsing is not used. |
| `api.fits.hdu` | default `1` | One-based image HDU. |
| `api.fits.sample-rank` | default `2` | Number of FITS axes in one published sample: `1` for vectors or `2` for images. |
| `api.fits.rate` | required | Positive `numerator/denominator` samples per second; an integer means `/1`. The period must be at least one nanosecond. |
| `api.fits.schema` | required | Exact ndarray semantic schema. |
| `api.fits.profile` | optional | Exact ndarray interpretation or calibration profile. |
| `api.fits.io-mode` | default `file` | `file` for normal CFITSIO access or `mmap` for a CFITSIO memory file backed by a private read-only mapping. |
| `api.fits.prefault` | default `false` | Touch every mapped page before startup; valid only with `io-mode=mmap`. |
| `api.fits.loop` | default `true` | Wrap to the first plane after the final plane. |

The file axes define the repeated values without a separate shape property:

- `sample-rank=1` accepts `(elements)` or `(elements, samples)`. It publishes a
  canonical row-major vector. A `Float64` file with the ALPAO command schema,
  matching actuator count, and matching profile can therefore negotiate
  directly with `api.alpao.sink`.
- `sample-rank=2` accepts `(width, height)` or `(width, height, frames)`. It
  publishes the native FITS element type with shape `(width, height)` and
  column-major layout, preserving FITS axis order. It also offers raw video
  `GRAY16_LE`; CFITSIO performs the conversion directly into the selected
  output buffer.

The ndarray format always contains the configured rate and schema. It contains
the profile when configured. Element type, shape, layout, rate, schema, and
profile are exact negotiated constraints, so a downstream plugin rejects a
mismatched file before playback starts.

## Cadence and overload

Start establishes a `CLOCK_MONOTONIC` epoch. `SPA_META_Header.seq` is the
scheduled plane sequence and `pts` is its absolute scheduled release time. The
first plane is due immediately. If processing, storage, or a consumer lease is
late, the source advances directly to the newest due plane; it never emits a
catch-up burst and never backpressures the cadence. The first plane and a plane
following skipped deadlines carry `SPA_META_HEADER_FLAG_DISCONT`.

The repeated path performs one monotonic clock read and, when a plane is due,
one bounded pool acquisition and one CFITSIO plane read. CFITSIO and filesystem
latency are source I/O, analogous to a camera SDK call, and are isolated on the
node's `SPA_NODE_FLAG_RTC_PROCESS` data loop. Neither ordinary file I/O nor a
memory mapping is claimed to be strict real-time: cache misses, page faults,
filesystem faults, and CFITSIO internals must be qualified for the selected
storage and rate.

The source supports mapped `MemPtr` and `MemFd` pool buffers. It does not offer
DMA-BUF or progressive publication; a stored FITS plane has no useful
progressive acquisition boundary.
