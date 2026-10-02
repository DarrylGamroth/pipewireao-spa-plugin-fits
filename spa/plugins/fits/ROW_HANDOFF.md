# Row-block loan regression evidence

## Scope

Functional investigation on 2026-10-01, starting at FITS revision
`288bbeb70651` and public plugin-header revision `34c7a0429f36`.
The changes use public SPA IO, process and ready contracts. No PipeWire core
implementation, graph algorithms, additional copies or data threads changed.
This evidence concerns transport correctness; it does not qualify latency,
maximum rate or a physical camera.

## Confirmed defects and corrections

| ID | Defect | Correction |
| --- | --- | --- |
| FITS-ROW-001 | Process re-entry checked the next deadline before reporting a pending HAVE_DATA loan. It could hide the loan or abandon the remaining frame. | Return the existing loan before inspecting cadence; preserve its frame cursor and metadata. |
| FITS-ROW-002 | The host accepts ready output before downstream completes. NEED_DATA alone allowed another timer publication to overtake that graph cycle. | Keep one timed row cycle pending until its exact published buffer returns through process; disarm the timer meanwhile and rearm the original next deadline. The completion pass does not publish another row. |
| FITS-ROW-003 | Finite row playback never marked terminal completion pending. | Record the terminal buffer and expose fits.completed only after that loan returns; Start clears completion. |

Actual output-pool exhaustion retains partial-frame abandonment. Complete-frame
cadence and its overload behavior remain unchanged.

## Fail-before and pass-after

The same public source-to-discard probe used seven 352×352 U16 frames,
11 rows per block, 32 blocks per frame, 10 Hz and 2 ms simulated readout.
It placed source/sink together on CPU12 or separately on CPUs12/8, with
control on CPU14. The original source delivered 68/224 and 19/224 blocks.
Removing only process abandonment published all blocks but delivered 197/224
and 161/224; the timer-cycle defect remained.

The final production library, including the installed library, delivers
**224/224 blocks, 1,734,656 bytes, zero protocol errors and 224 sink process
calls** on both placements. The restored original handoff fails the focused
test's process-reentry HAVE_DATA assertion. All three Meson suites (factory,
cube and source) pass after correction. Tests include pending loans before and
after deadlines, NEED_DATA without a returned buffer ID, actual pool exhaustion,
metadata/pixel preservation, delayed terminal return and Start reset, in poll
and timerfd modes.

Investigative counters used bounded storage and one dump after callback removal.
They changed observed failure counts and are not timing measurements. They are
absent from production. The installed production plugin SHA-256 for this check
is `3f3fb042201afb2cf646fd76a468103d8f57609e0630eef065b332fe03e14e2a`.

## Downstream boundary

A matched Classic native scientific graph still delivered fewer commands
despite 224 published and returned source blocks, zero frame skips and zero
buffer shortages. A separate bounded discard trace showed missing initial
sequence 0, plus sequence 2 when a live reconstructor mutation was submitted;
terminal sequence 6 arrived. This establishes a downstream initialization or
adoption boundary, not another source defect. Deployment startup and scientific
publication-unit adoption must be tested separately from source loan delivery.
