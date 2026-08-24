# Implementation status

This ledger records what the first `api.alpao.sink` slice implements and what
its automated evidence can support.

| ID | Requirement | Implementation | Evidence | Status |
| --- | --- | --- | --- | --- |
| ALPAO-001 | Expose one ordinary SPA sink factory. | `plugin.c`, `sink.c` | Production and mock factory tests load and enumerate `api.alpao.sink`. | Verified |
| ALPAO-002 | Require exact schema, structural format, actuator count, and profile before start. | `validate_format()` | Mock test rejects unequal schema and profile and accepts the advertised format. | Verified |
| ALPAO-003 | Consume one latest-buffer input without a private queue. | `spa_buffer_latest` input in `sink.c` | Mock test submits, processes, and reclaims fixed-pool buffers. | Verified |
| ALPAO-004 | Reject malformed or non-normalized commands before the backend and return their leases. | `process_command()` and `process()` | Mock test rejects `1.5` with `-ERANGE` and reclaims the same buffer. | Verified |
| ALPAO-005 | Make proprietary SDK integration optional. | `alpao-sdk` Meson feature | SDK-disabled build and test contain no `libasdk` dependency. | Verified |
| ALPAO-006 | Open ASDK only on start, verify `NbOfActuator`, reset, send, reset, and release. | `backend-asdk.c` | ASDK simulator test with synthetic `SIM001` configuration. | Simulator verified |
| ALPAO-007 | Do not redistribute SDK or mirror configuration. | Build options and generated test fixture | Repository inspection and ignored build trees. | Verified for current tree |
| ALPAO-008 | Keep the repeated wrapper bounded and allocation-free. | Fixed pool, fixed scan, direct payload validation | Source review; no allocator or logging call in `process()` or `process_command()`. ASDK internals are not qualified. | Wrapper reviewed |
| ALPAO-009 | Provide a safe physical-device lifecycle and strict RTC qualification. | Reset lifecycle is implemented. | Requires connected-device failure and timing tests. | Not verified |
| ALPAO-010 | Define canonical profile serialization and fingerprints. | Exact opaque matching only. | Requires normative manifest and byte-stable vectors. | Open |
| ALPAO-011 | Allow a deployment to set the DEv7 `daqFreq` interface conversion rate without advertising it as command cadence. | Optional `api.alpao.daq-frequency` startup property and `asdkSet("daqFreq", ...)` | Mock contract test covers accepted and out-of-range configuration. The ASDK simulator does not implement `daqFreq`; connected-interface verification remains required. | Implemented; hardware not verified |
| ALPAO-012 | Qualify host latency through the ASDK command-packing boundary without requiring a network or physical mirror. | Optional 468-actuator capture-interface integration test and benchmark | Correlates SPA process timestamps with sequenced `AITC` capture timestamps, checks frame size/order/loss, and supports closed-loop, fixed-rate, burst, affinity, memory locking, and `SCHED_FIFO` runs. | Host harness implemented; controlled-host results required |

The ASDK simulator uses a synthetic eight-actuator binary configuration created
by `alpao-binary-config` and the ASDK `sim` interface. It discards commands and
therefore cannot support claims about electronics, actuator order, physical
motion, command latency, or timing bounds.

With ASDK 4.01.12, LeakSanitizer reports the same 1,200-byte, three-allocation
retention for both the simulator test and the capture-interface test.
AddressSanitizer and UndefinedBehaviorSanitizer complete the capture test when
leak detection is disabled. This evidence does not attribute the retained SDK
state to the capture benchmark, and it does not qualify ASDK as leak-free.

## Camera sources

| ID | Requirement | Implementation | Evidence | Status |
| --- | --- | --- | --- | --- |
| CAMERA-001 | Build vendor camera sources outside the PipeWireAO fork using only public SPA headers. | `spa/plugins/egrabber`, `spa/plugins/bgapi2`, and optional Meson SDK features | Clean out-of-tree debug-optimized build through `libspa-ao-0.2`; no source include overrides. | Verified |
| CAMERA-002 | Preserve the existing eGrabber factory, discovery, complete-frame, progressive, metadata, controls, and fan-out behavior. | Migrated eGrabber plugin and qualification harness | Ten eGrabber unit/factory/device/capture tests pass with the connected Euresys Gigelink camera. | Verified for complete Gigelink; progressive hardware remains open |
| CAMERA-003 | Preserve BGAPI2 complete-frame behavior with both qualified producers and expose startup device discovery. | BGAPI2 manager, per-camera device, source, and callback/SPSC camera adapter | Factory, manager/device chain, adapter, and ten-frame source tests pass through Euresys Gigelink and Baumer BGAPI2 2.16.1 CTIs. Startup discovery selects by serial when the producer supplies it and otherwise preserves exact transport coordinates. | Verified |
| CAMERA-004 | Preserve daemon loading without duplicating factories between repositories. | Factory names and install paths remain `egrabber/libspa-egrabber` and `bgapi2/libspa-bgapi2`; PipeWireAO retains loader mappings only. | PipeWireAO commit `277d66864` removes both in-tree implementations and SDK options. An isolated daemon loaded the out-of-tree eGrabber DSO and completed capture, retained-lease fan-out, and live join/leave. | Verified |
| CAMERA-005 | Quiesce an RTC camera source before its final link and announced buffers are dismantled. | PipeWireAO commit `5dd08ebd1` synchronously stops an RTC node before destroying its final runnable link while preserving active fan-out links. | The core regression covers live fan-out, non-final subscriber removal, and final-link quiescence. All 48 core tests and all 17 eGrabber/BGAPI2 tests pass; four consecutive connected Gigelink host qualifications complete capture, retained-lease fan-out, live join/leave, final teardown, and daemon-health checks. | Verified |

## Calculon pixel calibration

| ID | Requirement | Implementation | Evidence | Status |
| --- | --- | --- | --- | --- |
| CAL-001 | Expose one ordinary SPA transform factory. | Rust `calculon-spa-node` adapter and `pixel_calibration.rs` | C test loads the release cdylib and enumerates `api.calculon.pixel-calibration`. | Verified |
| CAL-002 | Negotiate exact raw, artifact, and calibrated formats. | Four fixed format constraints | Rust POD tests and C ABI metadata checks cover type, schema, profile, shape, layout, and rate. | Verified |
| CAL-003 | Use the authoritative Calculon numerical implementation. | `PixelCalibrationPlan<f32, u16>` dependency | Locked Calculon tests plus C ABI result check for a selected flat/background pair. | Verified |
| CAL-004 | Activate flat and background calibration atomically. | Bounded immutable plane stores and one `Props` transaction | C ABI test ingests two sequenced planes and selects the pair before processing. | Verified |
| CAL-005 | Preserve synchronous transform back pressure and complete-frame publication. | Standard `SPA_IO_Buffers`, reserve/commit/publish sequence | C ABI test verifies input consumption, output status, chunk layout, values, and Header propagation. | Verified |
| CAL-006 | Keep repeated processing bounded, nonblocking, and allocation-free. | Fail-fast atomic callback gate, fixed storage, one buffer per port per callback | Gate unit test; C ABI test interposes the glibc allocator around a warmed process call and observes zero allocations. | Verified on current Linux test host |
| CAL-007 | Define canonical detector-profile serialization. | Exact opaque matching only | Requires a normative manifest and byte-stable vectors. | Open |
| CAL-008 | Establish latency distributions and release gates for representative detector sizes. | No dedicated benchmark yet | Requires warmed p50/p99/p99.9 runs with environment capture. | Open |
