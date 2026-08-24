# Algorithm plugin architecture

Status: accepted boundary; first pixel-calibration slice implemented

Decision: PWAO-PLUGIN-002

## Repository boundary

Algorithm implementations and their SPA adapters have separate ownership:

- The PipeWireAO fork owns generic graph execution, scheduling, SPA buffer
  contracts, and the structural `application/ndarray` format.
- `calculon-algorithms` owns scientific operations, physical-unit semantics,
  semantic schema identifiers, prepared plans, workspaces, and numerical tests.
  It has no dependency on PipeWire or SPA.
- This repository owns loadable SPA factories that negotiate formats, validate
  buffers, translate lifecycle calls, and invoke a Calculon plan.

The adapter does not copy an algorithm implementation into the plugin
repository. One tested Calculon plan remains the numerical authority whether
it is called from a SPA graph or directly from Rust.

## Language boundary

Calculon algorithms and their SPA factories use Rust. Rust provides explicit
buffer and state ownership without garbage collection, while keeping the
numerical implementation and adapter in one language. The SPA ABI remains C:
the loadable library exports `spa_handle_factory_enum`, C method tables, and
ordinary SPA POD and buffer contracts.

The reusable `calculon-spa-node` crate contains the unsafe ABI boundary. It
must remain small and must catch panics before they cross C. Its callback gate
never waits: an overlapping or reentrant callback fails with `-EBUSY`.
Repeated processing performs bounded work and allocates no heap memory after
node preparation.

C remains the default for device plugins backed by a C SDK. C++ is used only
where an implementation dependency requires C++, such as eGrabber. Language
choice is local to a plugin and does not change its SPA runtime identity.

## Pixel-calibration factory

`api.calculon.pixel-calibration` applies the authoritative Calculon equation:

```text
calibrated[i] = flat[i] * (f32(raw[i]) - background[i])
```

Raw and background values are detector ADU, `flat` is PDE/ADU, and calibrated
output is PDE. The factory has these fixed ports:

Factory construction requires:

| Property | Value |
| --- | --- |
| `api.calculon.detector-size` | Positive `WIDTHxHEIGHT` pair |
| `api.calculon.detector-rate` | Positive `NUM/DEN` frame rate |
| `api.calculon.detector-profile` | Exact lowercase detector-profile fingerprint |

| Direction and ID | Role | Negotiated format |
| --- | --- | --- |
| input 0 | raw detector frame | `video/raw`, `GRAY16_LE`, exact size and rate |
| input 1 | prepared flat calibration | F32 ndarray, `org.calculon.ao.flat-calibration/1`, no rate |
| input 2 | prepared background calibration | F32 ndarray, `org.calculon.ao.background-calibration/1`, no rate |
| output 0 | calibrated detector frame | F32 ndarray, `org.calculon.ao.calibrated-pixels/1`, raw-frame rate |

All ndarray ports require `[height, width]`, `ROW_MAJOR`, and the same exact
detector profile. The profile is currently a trusted lowercase
`sha256:<64-hex-digits>` identifier; canonical profile serialization remains a
separate specification task.

Flat and background inputs are optional prepared-artifact streams. Each
accepted artifact requires a standard SPA Header sequence number. The node
retains a bounded number of immutable planes. A `Props` update selects the
flat and background sequence numbers together, so a complete calibration pair
becomes active atomically between frame callbacks. Sequence `-1` selects the
identity plane: one for flat and zero for background.

The raw and output streams use standard `SPA_IO_Buffers`. This is a synchronous
transform, so back pressure is preserved: one raw input is consumed only when
one complete output can be published. Prepared artifacts are inspected at
most once per port per process call; there is no private queue.

## Build boundary

Meson builds and installs the Rust `cdylib` in PipeWireAO's SPA plugin
directory. Cargo resolves the algorithms crate from the sibling
`calculon-algorithms` repository during local development. Once both
repositories have canonical remotes and releases, this path dependency should
be replaced by a locked released dependency while retaining a local Cargo
patch workflow for joint development.

The C ABI test loads the release library with `dlopen`, enumerates and
initializes the factory, verifies all advertised metadata, rejects incomplete
construction data and undersized buffers, ingests a calibration pair, selects
it atomically, checks the calibrated values and Header propagation, and
interposes the system allocator to assert zero steady-state process
allocations.
