# FITS sequence SPA source

`api.fits.source` publishes either complete FITS planes or a simulated camera
readout made of complete row-block ndarrays. It is a regular PipeWire graph
driver with selectable polling or timerfd readiness. Neither readiness mode
creates a private thread.

In complete-frame mode, CFITSIO reads each due plane directly into an ordinary
PipeWire output buffer. Buffered file access is the default. FITS values may
require byte-order, scaling, or type conversion, so mapping the file does not
make playback zero-copy. `mmap` remains available for measurement and for
deployments that prefer a stable virtual mapping; optional prefaulting touches
every file page during construction.

## Factory properties

| Property | Requirement | Meaning |
| --- | --- | --- |
| `api.fits.path` | required | FITS file path; extended-filename parsing is not used |
| `api.fits.hdu` | default `1` | one-based image HDU |
| `api.fits.sample-rank` | default `2` | FITS axes in one sample: `1` for vectors or `2` for images |
| `api.fits.rate` | required | positive `numerator/denominator` frame or vector rate |
| `api.fits.schema` | required | exact ndarray semantic schema |
| `api.fits.profile` | optional in frame mode, required in row mode | source deployment identity published as a node property; not part of ndarray negotiation |
| `api.fits.io-mode` | default `file` | `file` or private read-only `mmap` backing |
| `api.fits.prefault` | default `false` | touch mapped pages before startup; valid only with `mmap` |
| `api.fits.loop` | default `true` | wrap after the final plane |
| `api.fits.readiness` | default `poll` | `poll` for a busy-spin deadline probe or `timerfd` for ordinary data-loop readiness |
| `api.fits.output-mode` | default `frame` | `frame` for complete planes or `row-block` for simulated camera readout |
| `api.fits.row-block-rows` | default `1` | rows in each complete public block; used only in row mode |
| `api.fits.simulated-readout-time-ns` | required in row mode | positive interval from frame start through completion of the final detector row |

The file axes define repeated values without a separate shape property:

- `sample-rank=1` accepts `(elements)` or `(elements, samples)` and publishes a
  canonical row-major vector. A matching F64 ALPAO command file can negotiate
  directly with `api.alpao.sink`.
- `sample-rank=2` accepts `(width, height)` or `(width, height, frames)`. In
  frame mode it publishes the native FITS element type as a column-major
  ndarray and also offers raw `GRAY16_LE` video; CFITSIO converts directly into
  the selected output buffer.

Element type, shape, layout, rate, and schema are exact constraints. A
mismatched consumer is rejected during negotiation rather than at playback.
The configured profile is available to orchestration as node identity and must
be checked by deployment admission when compatibility depends on it.

## Simulated camera row readout

Row mode is a qualification source for camera pipelines such as the C-BLUE 1
and C-RED 2 paths. It is not a claim about either camera's measured readout
timing. A typical configuration is:

```ini
api.fits.sample-rank = 2
api.fits.output-mode = row-block
api.fits.row-block-rows = 22
api.fits.simulated-readout-time-ns = 250000
api.fits.rate = 4000/1
api.fits.schema = org.calculon.ao.raw-pixel-row-block/1
api.fits.profile = detector-profile-id
```

`api.fits.rate` remains the camera frame rate. The configured readout time must
not exceed one frame period. Block-completion deadlines are uniformly spaced
across that readout time; the advertised ndarray rate is
`frame_rate * height / row_block_rows`.

Row mode requires a rank-two image, the exact raw-pixel-row-block schema, a
nonempty source deployment identity, and a positive block height smaller than and
dividing the detector height. It publishes U16 little-endian row-major
`[row_block_rows, width]` ndarrays. Every block has standard Header metadata:

- `seq` is the simulated camera-frame sequence;
- `offset` is the first detector row;
- `MARKER` identifies the final block; and
- `pts` is the scheduled completion time of that block.

The complete FITS cube is converted to private U16 storage during construction;
the required storage is `frames * height * width * 2` bytes. No CFITSIO or file
operation occurs after `Start`. Rows whose deadlines pass while the graph is
working remain available in that private storage and are drained one block per
graph cycle. After a terminal block, the source skips whole obsolete frames to
the current camera frame and sets `DISCONT` on the next first block. If an
ordinary output buffer is unavailable, it abandons the current partial frame
and also resumes discontinuously. This preserves an open-loop camera schedule
without shifting arrival times to match processing speed.

The uniform schedule is intentionally simple. Qualification against a physical
camera must replace the configured duration with measured readout timing and
must account for its tap order, line direction, blanking, ROI, and transport
behavior.

## Cadence and overload

Start establishes a `CLOCK_MONOTONIC` epoch. In frame mode,
`SPA_META_Header.seq` is the scheduled plane sequence and `pts` is its absolute
release time. The first plane is due immediately. If processing, storage, or a
consumer lease is late, the source advances to the newest due plane; it never
emits a complete-frame catch-up burst. The first plane and the first plane after
skipped deadlines carry `DISCONT`.

A non-looping complete-frame source exposes the read-only Boolean
`fits.completed` through `SPA_PARAM_Props`. It becomes true only after the
final published buffer returns from downstream, so a lifecycle owner does not
interpret queueing the final frame as downstream completion. A new `Start`
resets it to false.

In `poll` readiness, the node reports `SPA_NODE_FLAG_POLL_DRIVER`. Each probe
performs one monotonic clock read. A due probe performs one bounded pool
acquisition and either a complete-frame CFITSIO read or a prepared row copy,
then returns `SPA_STATUS_HAVE_DATA` to start a regular graph cycle. The source
is not probed again until that cycle completes. If the ordinary output is still
held, the current sample or partial frame is abandoned and the next publication
is discontinuous.

In `timerfd` readiness, the node does not report `POLL_DRIVER`. An absolute
monotonic timerfd invokes the same publication path and calls the ordinary SPA
ready callback. If a previous buffer is still pending at a deadline, the source
does not spin on an already-expired timer: it marks a discontinuity and arms a
future release. This mode needs `DataLoop` and `DataSystem` SPA support and
fails with `ENOTSUP` when they are absent.

In frame mode, CFITSIO and filesystem service time remain part of the deployment
contract. Cache misses, page faults, storage faults, and library internals
prevent a generic strict-real-time claim. Row mode removes file service from
the repeated path, but still does not by itself prove a strict real-time system.

The source supports mapped `MemPtr` and `MemFd` pool buffers. It does not offer
DMA-BUF. Simulated row blocks are copied from private prepared storage, matching
the ordinary-buffer ownership boundary used by real eGrabber row readout.
