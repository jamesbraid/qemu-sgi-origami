<!-- SPDX-License-Identifier: BSD-3-Clause -->

# SGI release packaging

`sgi-package.py` builds the programs the SGI machines need and packs them
into one archive per OS. Everything for it lives in this directory so that
merging upstream QEMU never touches it.

## Archive

`sgi-origami-<version>-<platform>.tar.gz` (Windows: `.zip`), with
`<platform>` one of `linux-x86_64`, `linux-arm64`, `macos-arm64`,
`macos-x86_64`, `windows-x86_64`, `windows-arm64` and
`<version>` the release tag without its `sgi-origami-` prefix, for example
`sgi-origami-11.1.50-1-linux-x86_64.tar.gz`. There is no wrapping directory:

```
bin/qemu-system-mips64[.exe]
bin/qemu-img[.exe]
bin/qemu-sgi-machine-init[.exe]
bin/*.dll                        Windows only
share/qemu/keymaps/
licenses/
```

It carries no firmware. `qemu-system-mips64 --version` ends with
`(sgi-origami <version>)`.

Release tags are `sgi-origami-<QEMU VERSION>-<n>`, where `<QEMU VERSION>` is
the tree's `VERSION` file and `<n>` counts releases on that line. The script
refuses a tag whose version differs from the tree's. Without `--tag` it
builds a development archive named after the commit.

The tag message is the release body. [RELEASING.md](../RELEASING.md) gives its
format.

### Licences and sources

Every library the archive contains is linked statically, under the licences
in `licenses/`:

- The vcpkg libraries, at the versions the manifest pins at the commit in
  `vcpkg-commit`: GLib 2.90.0, pixman 0.46.4, SDL 2.32.10, GnuTLS 3.8.13, with
  nettle 4.0, GMP 6.3.0, libtasn1 4.19.0, libidn2 2.3.7 and libunistring 1.2
  under it, and the smaller ones they pull in (zlib, libffi, PCRE2, libpng,
  gettext). Each library's copyright file is `licenses/<library>.copyright`.
  Where vcpkg's file leaves out a licence the library is offered under,
  `packaging/licenses/` holds the replacement (nettle and libunistring).
- The Rust crates of QEMU's Rust devices, linked statically from the
  `subprojects/` crate wraps: each crate's licence files are
  `licenses/<crate>-<version>.LICENSE*`, and the standard library's texts on
  Windows are `licenses/rust.*`.
- Linux: libstdc++, which the CHD library's C++ links with
  `-static-libstdc++`. Its texts are `licenses/libstdc++.copyright` and
  `licenses/libstdc++.GPL-3`.
- Windows: the MinGW runtime the programs are linked with, libstdc++, libgcc
  and winpthreads, whose texts are `licenses/libstdc++.*`, `licenses/libgcc.*`
  and `licenses/winpthreads.COPYING`. A runtime DLL the programs import is
  also copied into `bin/`. The Rust standard library the Rust devices link
  statically has its texts in `licenses/rust.*`.

Sources for each are the upstream tarballs the vcpkg ports download, with
URLs and SHA-512 checksums in each port's `portfile.cmake` (see
`https://github.com/microsoft/vcpkg/tree/<vcpkg-commit>/ports/<port>`). GCC's
source for libstdc++ and libgcc is at https://gcc.gnu.org/mirrors.html. The
MSYS2 packages
the Windows build installs are built from the recipes in
https://github.com/msys2/MINGW-packages, and winpthreads comes from
`mingw-w64-libraries/winpthreads` in https://github.com/mingw-w64/mingw-w64.

## Building

GLib, pixman, SDL 2 and GnuTLS (with the nettle, GMP and other libraries
it needs) come from vcpkg, built statically from `vcpkg.json`
at the commit in `vcpkg-commit`, with the triplets in `triplets/` (Linux
uses vcpkg's own `x64-linux-release`). QEMU's configure arguments are in
`configure-flags.json`, which `scripts/ci/sgi-check windows` also reads.
The libraries are linked statically, under the licences in the archive's
`licenses/`; their sources are the vcpkg ports at the commit in
`vcpkg-commit`. Each platform builds natively and needs network access for vcpkg and for
Meson's bundled libslirp and CHD library.

Linux, in a container, so the glibc floor is the image's (Debian 12, glibc
2.36). The image is multi-architecture, so the same commands build the
`linux-arm64` archive on an arm64 host:

```sh
docker build -t sgi-package -f packaging/debian-bookworm.docker packaging
docker run --rm -v "$PWD":/src -w /src sgi-package \
    python3 packaging/sgi-package.py --tag sgi-origami-11.1.50-1
```

macOS, on an Arm or Intel Mac (the archive matches the host) with the Xcode
command line tools, a Rust toolchain from rustup and
`brew install ninja pkgconf python autoconf autoconf-archive automake libtool`.
The build enables QEMU's Rust devices, which need `bindgen` on `PATH`:

```sh
cargo install bindgen-cli --version 0.73.2 --locked
python3 packaging/sgi-package.py
```

Windows, in an MSYS2 MINGW64 shell. QEMU cannot build with MSVC, so vcpkg
runs with a MinGW triplet and the MSYS2 compiler on `PATH`:

```sh
pacman -S --needed git base-devel autoconf-archive mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python \
    mingw-w64-x86_64-rust mingw-w64-x86_64-rust-bindgen mingw-w64-x86_64-clang
python packaging/sgi-package.py
```

Windows on Arm builds in an MSYS2 CLANGARM64 shell with the same steps and
the `mingw-w64-clang-aarch64-*` packages. vcpkg takes Clang from `PATH` through
`triplets/arm64-mingw-origami-toolchain.cmake`, and it runs its own x86-64
MSYS2 under emulation, so a build with a cold vcpkg cache takes hours; the
workflow saves the cache even when the build fails.

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
