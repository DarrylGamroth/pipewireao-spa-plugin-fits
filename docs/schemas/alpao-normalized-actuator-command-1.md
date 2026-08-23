# ALPAO normalized actuator command schema, version 1

Status: experimental; the payload contract is implemented, while canonical
profile construction remains to be specified

Schema identifier:
`org.pipewireao.alpao.normalized-actuator-command/1`

## Meaning

Each element is the normalized command for one actuator accepted by the ALPAO
deformable-mirror SDK. Every value SHALL be finite and in the inclusive range
`[-1.0, +1.0]`. Zero is the neutral normalized command. This schema does not
represent metres, micrometres, surface displacement, or wavefront displacement.

The negotiated format SHALL contain exactly:

```text
mediaType    = application
mediaSubtype = ndarray
schema       = org.pipewireao.alpao.normalized-actuator-command/1
elementType  = F64_LE
shape        = [actuator-count]
layout       = ROW_MAJOR
profile      = sha256:<64 lowercase hexadecimal digits>
```

`shape` SHALL have rank one. Its extent SHALL equal the configured actuator
count and, for the ASDK backend, the `NbOfActuator` value read when the mirror
is opened. Version 1 does not advertise `rate`: no command cadence has yet been
qualified as part of this contract.

An ALPAO interface `daqFreq` setting controls digital to analog conversion and
does not state how frequently a producer submits command buffers. It is exposed
as device configuration and SHALL NOT be encoded as this format's `rate`.

## Profile

The profile fixes the association between vector index and physical actuator,
including every calibration or configuration choice needed to interpret the
normalized vector. A consumer SHALL compare the complete profile string and
SHALL reject a missing or unequal value before opening the device.

Version 1 currently treats the supplied `sha256:` value as an exact, trusted,
opaque identifier. The canonical profile manifest, byte serialization, and
fingerprint test vectors remain open work. Until those are specified, a
deployment SHALL NOT claim that independently computed fingerprints are
interoperable merely because both use SHA-256.

The mirror serial is not the profile. A serial may select configuration, but
it does not replace a negotiated command interpretation.

## Buffer and lifecycle behavior

The payload SHALL be one contiguous, naturally aligned block. The chunk size
SHALL be exactly `8 * actuator-count` bytes and its stride SHALL be either zero
or eight bytes. A sink SHALL reject a malformed, non-finite, or out-of-range
command without passing it to ASDK and SHALL still return the claimed buffer
lease.

The sink uses PipeWireAO latest-buffer input. If several commands arrive before
the sink claims one, the transport retains the newest command and accounts for
the replaced submission. This is the overload policy; the plugin does not add
another queue.

The ASDK backend SHALL verify the actuator count and establish the zero command
before accepting buffers. Pause, suspend, and normal teardown SHALL reset and
release the mirror. A simulator success demonstrates SDK and configuration
path operation only; it does not qualify a physical device or strict real-time
behavior.
