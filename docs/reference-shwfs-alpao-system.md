# Reference Shack-Hartmann to ALPAO system

The maintained reference graph uses Calculon's ndarray filter-graph bundle for
scientific processing and this repository for the ALPAO device boundary.

```text
detector ndarray
  -> module-ndarray-filter-chain
       Calculon pixel calibration
       Shack-Hartmann measurement
       reconstruction
       controller integration
       PDM command construction
       ALPAO command-normalization-f32-f64
  -> org.pipewireao.alpao.normalized-actuator-command/1 F64 [actuator]
  -> api.alpao.sink
  -> ASDK
```

Calculon's FGN graph and declarations are maintained in
`calculon-algorithms`. The operations execute inside one outer PipeWire filter
node, so their explicit scientific schemas do not require a separate
PipeWire-buffer handoff at each algorithm boundary.

The final ALPAO operator is loaded from
`lib/pipewire-ao/filter-graph/libalpao-fgn.so` with label
`command-normalization-f32-f64`. It requires:

| Key | Meaning |
| --- | --- |
| `actuator_count` | Positive actuator-vector extent. |
| `command_scale` | Positive physical-command value corresponding to normalized magnitude one. |
| `profile` | Exact lowercase SHA-256 ALPAO interpretation profile. |
| `rate_numerator` | Positive demanded-command rate numerator. |
| `rate_denominator` | Positive demanded-command rate denominator. |

The operator consumes `org.calculon.ao.demanded-pdm-command/1` F32 values and
publishes `org.pipewireao.alpao.normalized-actuator-command/1` F64 values. Both
ports carry the same immutable profile. The normalized output has no declared
rate because the ALPAO sink treats command arrival and the interface's DEv7
`daqFreq` conversion setting as separate concepts.

The sink then validates schema, vector extent, profile, finiteness, and the
`[-1,+1]` range before calling ASDK. The mock sink and direct FGN test are the
deterministic host-side gates. Physical mirror timing and failure recovery
still require hardware qualification.

The former `spa-calculon-full-system` executable and native Calculon SPA
factories were retired when this graph moved to FGN. Historical profiling
records remain under `docs/benchmark-data`; they do not qualify the current FGN
deployment.
