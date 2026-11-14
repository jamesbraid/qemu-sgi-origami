<!-- SPDX-License-Identifier: BSD-3-Clause -->

# SGI release packaging

`sgi-package.py` builds the programs the SGI machines need and packs them
into one archive per OS. Everything for it lives in this directory so that
merging upstream QEMU never touches it.

## Archive

`sgi-origami-<version>-<platform>.tar.gz` (Windows: `.zip`), with
`<platform>` one of `linux-x86_64`, `macos-arm64`, `windows-x86_64` and
`<version>` the release tag without its `sgi-origami-` prefix, for example
`sgi-origami-11.1.50-1-linux-x86_64.tar.gz`. There is no wrapping directory:

```
bin/qemu-system-mips64[.exe]
bin/qemu-img[.exe]
bin/qemu-sgi-machine-init[.exe]
bin/*.dll                        Windows only
share/keymaps/
licenses/
```

It carries no firmware. `qemu-system-mips64 --version` ends with
`(sgi-origami <version>)`.

Release tags are `sgi-origami-<QEMU VERSION>-<n>`, where `<QEMU VERSION>` is
the tree's `VERSION` file and `<n>` counts releases on that line. The script
refuses a tag whose version differs from the tree's. Without `--tag` it
builds a development archive named after the commit.

## Building

GLib, pixman and SDL 2 come from vcpkg, built statically from `vcpkg.json`
at the commit in `vcpkg-commit`, with the triplets in `triplets/` (Linux
uses vcpkg's own `x64-linux-release`). QEMU's configure arguments are in
`configure-flags.json`, which `scripts/ci/sgi-check windows` also reads.
Each platform builds natively and needs network access for vcpkg and for
Meson's bundled libslirp and CHD library.

Linux, in a container, so the glibc floor is the image's (Debian 12, glibc
2.36):

```sh
docker build -t sgi-package -f packaging/debian-bookworm.docker packaging
docker run --rm -v "$PWD":/src -w /src sgi-package \
    python3 packaging/sgi-package.py --tag sgi-origami-11.1.50-1
```

macOS, on an arm64 Mac with the Xcode command line tools and
`brew install ninja pkgconf python autoconf automake libtool`:

```sh
python3 packaging/sgi-package.py
```

Windows, in an MSYS2 MINGW64 shell. QEMU cannot build with MSVC, so vcpkg
runs with a MinGW triplet and the MSYS2 compiler on `PATH`:

```sh
pacman -S --needed git base-devel mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python
python packaging/sgi-package.py
```

The archive lands in `build-package/dist`. The script stops when an
executable needs a shared library that is neither in `bin/` nor one the host
provides. On Windows it copies the MinGW runtime DLLs the executables need
into `bin/` first. On Linux the host provides X11, ALSA, PulseAudio and udev libraries, which
QEMU links directly (SDL loads only Wayland at run time), so even a headless
host needs `libx11-6`, `libasound2`, `libpulse0` and `libudev1`.

## Tests

`python3 packaging/test_sgi_package.py` covers naming, the library check
and archive layout without building anything. `scripts/ci/sgi-check` runs
it with the other script tests.
