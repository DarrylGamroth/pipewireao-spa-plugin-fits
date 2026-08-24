# Algorithm plugin architecture

Status: accepted boundary; raw-frame-to-ALPAO reference slice implemented

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

## Format and transport negotiation

An ndarray link is compatible only when its complete fixed format agrees:
`mediaType`, `mediaSubtype`, semantic `schema` and version, `elementType`,
`shape`, `layout`, interpretation `profile`, and `rate` where the stream is
clocked. The schema identifies what the values mean; it does not replace the
structural properties or the deployment profile. In particular, two arrays
with the same schema but different actuator order profiles are incompatible.

PipeWire may encode a fixed negotiated property as a SPA `Choice(None)`. The
adapter unwraps that representation before parsing but rejects every
unresolved choice. It then applies the same exact format constraint, including
schema and profile, as it does to a directly encoded fixed value.

The buffer I/O contract is negotiated separately from the ndarray format.
`SPA_IO_Buffers` and the PipeWireAO latest-buffer I/O types transport the same
negotiated payload; choosing one never changes or relaxes its schema. Regular
ports may retain their standard I/O endpoint while a latest-buffer link is
active. The active latest link takes precedence until it is removed.

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

Raw streaming inputs and algorithm outputs support standard `SPA_IO_Buffers`
and PipeWireAO latest-buffer I/O. The standard contract preserves synchronous
back pressure: one raw input is consumed only when one complete output can be
published. Under latest-buffer I/O, the adapter claims and completes one
lease per process call, so superseded frames do not create a private backlog.
Prepared artifacts continue to use standard buffers and are inspected at most
once per port per process call; there is no private queue.

## Fused Shack-Hartmann controller

`api.calculon.shwfs-controller` is the first fused strict-control factory. Its
public input is `org.calculon.ao.calibrated-pixels/1`; its public output is
`org.calculon.ao.demanded-pdm-command/1`. Region extraction,
Shack-Hartmann centroiding, reconstruction GEMV, leaky integration, and PDM
command limiting use their authoritative Calculon plans in private prepared
storage. They are not joined by intra-node SPA ports.

The construction dictionary supplies the exact detector and region geometry,
thresholds, controller parameters, actuator limits, and a profile-bound raw
little-endian F32 reconstruction matrix. The matrix is validated before the
node can start. One process callback is the transaction boundary for
controller state and demanded-command publication.

`api.alpao.command-normalization` is a separate device-boundary adapter. It
converts the physical F32 demanded-PDM schema to the exact F64 normalized
ALPAO schema and rejects values outside `[-1, +1]`. It is intentionally not a
Calculon algorithm: the conversion is owned by the selected mirror command
profile. The initial reference implementation uses one explicit scalar scale;
deployed profiles require their calibrated conversion artifact.

See `reference-shwfs-alpao-system.md` for the complete graph, construction
keys, correctness gate, simulator invocation, and latency boundary.

## Optional progressive RTC-island execution

Standard complete-buffer processing remains the default Calculon adapter mode.
The proposed PipeWireAO RTC-island executor may place a connected set of
progress-capable factories on one explicitly owned real-time duty-cycle loop.
This changes process ownership and readiness dispatch, not factory identity,
port visibility, negotiated schemas, or the authoritative Calculon plan.

Inside an island, an adapter maps the acquire-loaded committed byte prefix from
`SPA_META_Progressive` to Calculon semantic work units and calls
`InputProgress::process_range(previous..committed)`. An `OutputProgress` plan
may release-publish only the greatest prefix that it guarantees will remain
immutable. Complete-buffer operation invokes the same plan for the full input
extent and remains the compatibility and correctness oracle.

Progressive MVM input does not imply progressive MVM output. A cumulative MVM
partial sum can modify every output element when another slope arrives and is
therefore not an immutable output prefix. For a strict controller, the MVM and
the linear part of TFC/CLWC instead accumulate into private next-frame state:

```text
start:         working = leak * committed_controller_state
process_range: working += gain * R[:, range] * slopes[range]
finish:        apply terminal projection and safety, commit state, publish command
abort:         discard working and preserve committed state
```

CLWC is the transactional synchronization point. Only `finish` may advance
controller history or publish a mirror command. An incomplete, invalid,
superseded, or aborted input must take the `abort` path. With several required
HO, LO, or WFS inputs, the prepared workspace contains a bounded per-frame
completion ledger and commits only after all identities and configuration
generations match and every required input terminates successfully.

The first core scheduler proof of concept is intentionally test-only. The
direct reference slice already uses a latest-buffer source boundary, standard
complete-buffer calls between its three algorithms, and a latest-buffer sink
boundary. A scheduler-composed island still requires graph membership,
single-owner lifecycle, and fixed dispatch before it becomes a runtime claim.

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
