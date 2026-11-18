# SPDX-License-Identifier: BSD-3-Clause
# arm64-linux-release, letting CCPIC through: sgi-package.py sets -fPIC for
# nettle, whose default -fpic overflows the GOT of QEMU's executable.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_ENV_PASSTHROUGH CCPIC)
