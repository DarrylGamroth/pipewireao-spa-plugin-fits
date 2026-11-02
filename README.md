# PipeWireAO FITS plugin

This repository provides `api.fits.source`, a CFITSIO-backed PipeWireAO source
for complete FITS planes and simulated row-block camera readout. Include it in
deployments that need repeatable file-backed camera or vector input.

The repository is an independent build unit. It requires PipeWireAO 3.0.0 or
later, the public headers installed by
[`pipewireao-spa-plugins-core`](https://github.com/DarrylGamroth/pipewireao-spa-plugins-core),
and CFITSIO. It has no vendor SDK dependency.

## Build and stage

```console
meson setup build --prefix=/usr -Dfits=enabled
meson compile -C build
meson test -C build 'spa-fits*' --print-errorlogs
DESTDIR="$PWD/stage" meson install -C build
```

The staged tree can be copied into a target root filesystem or consumed by a
site-specific image or package build. See
[`spa/plugins/fits/README.md`](spa/plugins/fits/README.md) for source properties,
formats, cadence, and overload behavior.

## Container and package recipes

The Docker Bake file offers `debian-13-deploy` and `ubuntu-26-04-deploy` image
targets. `debian-13-package` and `ubuntu-26-04-package` export `.deb` files when
that deployment format is wanted. The supplied image recipes use the package
artifact internally; Meson builds are not restricted to Debian or Ubuntu.

The container build must be given authorized sources for PipeWireAO and the
core development files through its build arguments or BuildKit secrets.

## Independent metadata compatibility

The `codex/independent-spa-metadata` migration starts at `8c862cb` and retains
plugin version 0.1.0 pending coordinated release. It uses the core image-frame
helper and tests from core revision `86e8544`, requires native 3.0.0, and keeps
FITS publication Header-only. Four focused Meson tests pass against the staged
3.0 SDK, including complete-frame/row-block source checks and truthful monotonic
Header labels. No device, simulator or service was started.
