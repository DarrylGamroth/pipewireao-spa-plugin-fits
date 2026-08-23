# ALPAO implementation status

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

The ASDK simulator uses a synthetic eight-actuator binary configuration created
by `alpao-binary-config` and the ASDK `sim` interface. It discards commands and
therefore cannot support claims about electronics, actuator order, physical
motion, command latency, or timing bounds.
