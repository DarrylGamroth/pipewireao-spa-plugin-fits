# PipeWireAO FITS plugin

This repository provides `api.fits.source`, a CFITSIO-backed PipeWireAO source
for complete FITS planes and simulated row-block camera readout. Include it in
deployments that need repeatable file-backed camera or vector input.

The repository is an independent build unit. It depends on PipeWireAO, the
public headers installed by
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
