# PipeWireAO FITS plugin

[![CI](https://github.com/DarrylGamroth/pipewireao-spa-plugin-fits/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/DarrylGamroth/pipewireao-spa-plugin-fits/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/github/DarrylGamroth/pipewireao-spa-plugin-fits/branch/main/graph/badge.svg)](https://app.codecov.io/github/DarrylGamroth/pipewireao-spa-plugin-fits)

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

## CI and coverage

GitHub Actions builds and tests against PipeWireAO 4.0.5 on Debian 13.
FITS cube I/O, the factory, and the SPA source with generated FITS fixtures.

CI requires more than 90% line coverage for the code compiled in this
configuration. The workflow saves the LCOV report as an artifact and uses
GitHub OIDC to upload coverage to Codecov. Fork pull requests run the
same local gate.

With the workflow dependencies installed, run the same native tests locally.

```sh
CC=gcc CXX=g++ meson setup build-coverage -Dtests=enabled -Db_coverage=true \
  -Dfits=enabled
meson test -C build-coverage --print-errorlogs
```

Collect a native coverage report from that build.

```sh
gcovr --root . --lcov build-coverage/coverage-c.info --print-summary build-coverage
```
