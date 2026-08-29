# HNü240 Camera Link carrier

This note records the distinction between the HNü240 detector geometry and
its Camera Link transport. It prevents an eight-output CCD220 from being
mistaken for a conventional eight-tap, 16-bit Camera Link camera.

## Authoritative packing

Section 2.2.2 and Tables 3 and 4 of Beaulieu et al., [*Electron multiplying
CCDs for sensitive wavefront sensing at 3k frames per
second*](https://doi.org/10.1117/12.2626240), define the HNü240 Camera Link
packing.

The CCD220 has eight physical outputs. Each output produces 14-bit detector
values represented in 16-bit words, but Camera Link Full cannot carry eight
16-bit taps in the camera's selected mode. The HNü240 therefore presents the
physical Camera Link interface as eight 8-bit taps. Four detector outputs are
packed onto the 24-bit X channel, four onto the 24-bit Y channel, and the
16-bit Z channel is unused.

The packing state repeats every eight Camera Link clocks:

- one group occupies 64 bytes in frame-grabber memory;
- one group contains three samples from each of eight detector taps;
- 24 detector samples, or 48 bytes, are meaningful;
- 16 carrier bytes are unused;
- the state machine restarts at every carrier row, so rows can be decoded
  independently.

The complete carrier profile currently used by PipeWireAO is 1408 by 131 U8
components. A 1408-byte row contains 22 groups, hence 66 samples per detector
tap. The first six horizontal samples per tap are non-active, leaving 60
active samples per tap. Ten leading carrier rows are treated as non-active,
the following 120 rows contain active detector samples, and the terminal row
contains the per-tap overscan that PipeWireAO discards.

The paper describes eight lines as part of the internal pipeline latency while
the deployed 1408 by 131 profile has ten leading non-active carrier rows.
Those statements are not assumed to identify the same physical quantity. The
profile's ten-row exclusion must be confirmed against a real HNü240 capture
before it is generalized to another firmware or acquisition mode.

## Consequences for frame grabbers

A standard eight-tap, 16-bit configuration does not describe the HNü240 wire
format. In particular, `DeviceTapGeometry=1X8` combined with
`PixelFormat=Mono16` would interpret carrier bytes as detector pixels and is
not a substitute for the Table 4 unpacking.

For an EDT board, the known data-path portion of a corresponding configuration
is approximately:

```text
width:              1408
height:             131
depth:              8
extdepth:           8
CL_DATA_PATH_NORM:  77
htaps:              8
```

This fragment is descriptive, not an installable EDT camera configuration.
Camera timing, FVAL/LVAL handling, ROI behavior, serial initialization, and
firmware-specific settings have not been verified. A runnable configuration
must not be published until those fields have been established on hardware.

The camstack [OCAM2K EDT
configuration](https://github.com/scexao-org/camstack/blob/0196c1b67591817920524d5005a8f1dc533a62ae/conf/edt_fg_conf/ocam_full.cfg)
is a useful architectural comparison. It also captures a CCD220-derived
eight-tap carrier as 8-bit data and runs a [downstream detector
mapping](https://github.com/scexao-org/camstack/blob/0196c1b67591817920524d5005a8f1dc533a62ae/ocamdecode/gen_ocamdecode_maps.py).
It is not an HNü240 configuration: OCAM's 1056-byte row can be viewed as 528
contiguous 16-bit components, whereas the HNü240's unused Z-channel positions
make a direct U8-to-U16 cast incorrect. The camstack
`nuvu_kalao_16bit.cfg` file is also not applicable because it describes a
conventional single-tap, 16-bit Nüvü mode.

For a Pleora iPORT CL-Ten, an eight-tap geometry may correctly describe the
physical 8-bit Camera Link acquisition. It does not establish a Mono16 image
or remove the HNü240 packing. The exact choice between the iPORT's `1X8` and
`1X8_1Y` entries, and the resulting GVSP byte order, remain hardware
validation items. The receiver must expose the original 1408 by 131 carrier
unless a camera-specific iPORT firmware transform is demonstrated.

## PipeWireAO boundary

The admitted software boundary remains:

```text
Camera Link frame grabber or iPORT
    -> unchanged 1408 x 131 U8 HNü240 carrier
    -> api.hnu240.decoder
    -> acquisition-ordered [8, N, 60] U16 detector readout blocks
    -> detector readout plan and optional 240 x 240 assembly
```

`GRAY8` is currently the SPA storage format for the complete carrier; it does
not claim that the carrier is a Mono8 detector image. Construction therefore
requires the exact `hnu240-cl-full-8x8-v1` transport profile. Generic video or
pixel operators must not consume the carrier before the Nüvü transform has
decoded it.

The Nüvü transform owns Table 4 byte unpacking, tap identity, exclusion of
non-active horizontal samples and carrier rows, and overscan removal. The
prepared detector readout plan owns detector-coordinate placement and the
horizontal and vertical direction of each region. Camera and frame-grabber
drivers remain camera-agnostic and publish the carrier unchanged.

## Optional future optimization

A camera-specific FPGA transform could compact a carrier row from 1408 bytes
to 1056 bytes by removing unused channel positions while retaining all 66
samples per tap. If it also removed the six non-active samples per tap, the row
would contain 960 bytes. These are optional optimizations, not standard tap
geometry operations, and would require a new, explicitly versioned transport
profile.

Before accepting either an EDT or iPORT hardware profile:

1. Capture a known frame as an unchanged byte carrier.
2. Confirm the 1408 by 131 dimensions and row stride.
3. Compare one 64-byte group against Table 4 for all 24 samples.
4. Confirm the ten leading non-active rows, 120 active rows, and terminal
   overscan row for the selected camera firmware and mode.
5. Confirm tap placement and direction against a spatially identifiable test
   image.
6. Add a literal Table 4 golden vector that does not construct its input from
   the decoder's own byte-offset table.

