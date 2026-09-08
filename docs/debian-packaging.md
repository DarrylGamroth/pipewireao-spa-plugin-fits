# Debian and Ubuntu packages

This repository produces one binary package for each independently installable
integration. It remains one source repository and uses one upstream version for
all packages.

| Component | Binary package | Optional dependency boundary |
| --- | --- | --- |
| `core` | `pipewireao-spa-plugins-core` | None beyond the PipeWireAO host and normal system libraries |
| `dev` | `pipewireao-spa-plugins-dev` | PipeWireAO development package |
| `fits` | `pipewireao-spa-plugin-fits` | CFITSIO |
| `aravis` | `pipewireao-spa-plugin-aravis` | Aravis 0.10 |
| `imagestreamio` | `pipewireao-spa-plugin-imagestreamio` | ImageStreamIO |
| `alpao` | `pipewireao-spa-plugin-alpao` | ALPAO FGN operator and ASDK sink |
| `egrabber` | `pipewireao-spa-plugin-egrabber` | Euresys eGrabber |
| `bgapi2` | `pipewireao-spa-plugin-bgapi2` | Baumer GAPI2 |
| `edtpdv` | `pipewireao-spa-plugin-edtpdv` | EDT PDV |
| `flisdk` | `pipewireao-spa-plugin-flisdk` | First Light Imaging FliSdk |
| `andor3` | `pipewireao-spa-plugin-andor3` | Andor SDK3 |
| `hamamatsu` | `pipewireao-spa-plugin-hamamatsu` | Hamamatsu DCAM-API |
| `hermes` | `pipewireao-spa-plugin-hermes` | MPD HERMES and Opal Kelly FrontPanel |

Every optional integration package depends on the exact-version core package.
The core package includes the SDK-independent HERMES decoder, but the HERMES
source remains in the vendor package.
The ALPAO package contains both the FGN command-normalization operator and the
ASDK-backed deformable-mirror sink.

The package builder creates deployment-oriented binary `.deb` files. It is not
a Debian archive source package and does not replace a future `debian/` policy
submission.

## Private repository boundary

Do not add SDK installers, headers, libraries, repository credentials, signing
keys, or license files to this repository or to a public container registry.
Repackage each SDK only when its license permits that use, and publish those
packages to a private APT repository.

An SDK development package should contain the headers and link inputs. Its
runtime package should contain the libraries needed on the deployment host.
Linked libraries must have Debian `shlibs` or `symbols` metadata so
`dpkg-shlibdeps` can derive versioned dependencies. The package builder requires
an explicit SDK runtime dependency for every vendor component as well. This
covers header-only or dynamically loaded SDKs that ELF inspection cannot see.

The resulting vendor plugin packages should normally be published alongside
the corresponding SDK packages in the private repository. Public repositories
can contain `core`, `dev`, and integrations whose complete dependency chain is
redistributable.

## Package builder

List the available components:

```console
python3 packaging/deb/build.py --list-components
```

The builder configures every optional Meson feature explicitly. A core build
therefore cannot acquire CFITSIO or a vendor SDK merely because it happens to
be installed on the build host. It installs only the component's Meson tag,
uses `dpkg-shlibdeps` for ELF dependencies, and creates a package with normalized
ownership, permissions, and timestamps.

For example, after installing the PipeWireAO development package:

```console
python3 packaging/deb/build.py \
  --component core \
  --host-dependency 'pipewire-ao (>= 1.7)' \
  --maintainer 'Deployment Team <packages@example.org>' \
  --output-dir dist/local
```

Replace `pipewire-ao` with the actual binary package that supplies the
PipeWireAO runtime and plugin loader. The builder requires this declaration
because most SPA plugins consume the host ABI without a dynamic link that
`dpkg-shlibdeps` could discover.

A vendor build must also name its private SDK runtime package. Paths and other
Meson settings remain explicit:

```console
python3 packaging/deb/build.py \
  --component egrabber \
  --host-dependency 'pipewire-ao (>= 1.7)' \
  --maintainer 'Deployment Team <packages@example.org>' \
  --extra-depends 'euresys-egrabber-runtime (= 25.02.0-1)' \
  --meson-option 'egrabber-prefix=/opt/euresys/egrabber' \
  --output-dir dist/local
```

The dependency names and versions above illustrate the contract; use the names
assigned by the private repository. Pass `--library-dir` when a linked SDK
library is outside its embedded runtime search path and cannot otherwise be
resolved by `dpkg-shlibdeps`.

## Container package targets

[`Dockerfile.package`](../packaging/containers/Dockerfile.package) builds a
single component and exports only its `.deb`. The separately named
`pipewire-rs` build context satisfies the repository's current Rust path
dependency without copying that source here or fetching an unpinned revision.

The maintained distribution targets are Debian 13 and Ubuntu 26.04 LTS:

```console
export COMPONENT=core
export APT_BUILD_PACKAGES='pipewire-ao-dev'
export HOST_DEPENDENCY='pipewire-ao (>= 1.7)'
export MAINTAINER='Deployment Team <packages@example.org>'
docker buildx bake debian-13-package
docker buildx bake ubuntu-26-04-package
```

Artifacts are written beneath `dist/debian-13/COMPONENT` and
`dist/ubuntu-26.04/COMPONENT`.

For a vendor component, supply its development packages and explicit runtime
dependency through environment variables understood by `docker-bake.hcl`:

```console
export COMPONENT=egrabber
export APT_BUILD_PACKAGES='pipewire-ao-dev euresys-egrabber-dev'
export HOST_DEPENDENCY='pipewire-ao (>= 1.7)'
export MAINTAINER='Deployment Team <packages@example.org>'
export EXTRA_DEPENDS_JSON='["euresys-egrabber-runtime (= 25.02.0-1)"]'
export MESON_OPTIONS_JSON='["egrabber-prefix=/opt/euresys/egrabber"]'
```

The private repository is provided with optional BuildKit secrets:

- `apt_sources`: a deb822 `.sources` file;
- `apt_auth`: an APT `auth.conf` fragment; and
- `apt_keyring`: the repository's dearmored signing key.

The `.sources` file should refer to
`/etc/apt/keyrings/pipewireao-private.gpg` in its `Signed-By` field. Pass the
files without adding them to the build context:

```console
docker buildx build \
  --build-context pipewire-rs=../pipewire-rs \
  --secret id=apt_sources,src=/secure/pipewireao.sources \
  --secret id=apt_auth,src=/secure/pipewireao.auth.conf \
  --secret id=apt_keyring,src=/secure/pipewireao-private.gpg \
  --build-arg COMPONENT="$COMPONENT" \
  --build-arg APT_BUILD_PACKAGES="$APT_BUILD_PACKAGES" \
  --build-arg HOST_DEPENDENCY="$HOST_DEPENDENCY" \
  --build-arg MAINTAINER="$MAINTAINER" \
  --build-arg EXTRA_DEPENDS_JSON="$EXTRA_DEPENDS_JSON" \
  --build-arg MESON_OPTIONS_JSON="$MESON_OPTIONS_JSON" \
  --target packages \
  --output type=local,dest="dist/private/$COMPONENT" \
  --file packaging/containers/Dockerfile.package .
```

BuildKit secret mounts keep repository credentials and signing keys out of
container layers. SDK files are still present in the private builder's
intermediate build state, so vendor builds and their build cache must run on
trusted infrastructure and must not be published.

## Deployment image targets

[`Dockerfile.deploy`](../packaging/containers/Dockerfile.deploy) installs the
resulting package and resolves its declared dependencies from APT. The Bake
targets compose the package build directly into a runtime image:

```console
docker buildx bake debian-13-deploy
docker buildx bake ubuntu-26-04-deploy
```

The images are tagged `pipewireao-spa-COMPONENT:debian-13` and
`pipewireao-spa-COMPONENT:ubuntu-26.04`. Optional vendor driver or deployment
packages can be supplied through `APT_RUNTIME_PACKAGES`.

Publish the exact-version core package before deploying an optional component,
because APT must resolve that dependency. A deployment image proves package
installation and dependency closure; it does not constitute connected-camera,
mirror, timing, driver, or hardware validation.

Inspect a produced artifact before publication:

```console
dpkg-deb --info dist/DISTRIBUTION/COMPONENT/*.deb
dpkg-deb --contents dist/DISTRIBUTION/COMPONENT/*.deb
lintian dist/DISTRIBUTION/COMPONENT/*.deb
```
