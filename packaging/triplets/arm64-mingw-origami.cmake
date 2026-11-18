# SPDX-License-Identifier: BSD-3-Clause
# vcpkg's arm64-mingw-static-release, with Clang from MSYS2's CLANGARM64
# environment, which has no GCC for vcpkg's own MinGW toolchain to find.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_ENV_PASSTHROUGH PATH enable_hardware_acceleration)
set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/arm64-mingw-origami-toolchain.cmake")
# GnuTLS's arm64 assembly is ELF-only; its configure reads this variable,
# which sgi-package.py sets to no.
# Clang 16 and later make incompatible pointer types an error, which GLib at
# the pinned vcpkg commit still trips over.
set(VCPKG_C_FLAGS -Wno-error=incompatible-pointer-types)
# vcpkg requires C++ flags whenever C flags are set.
set(VCPKG_CXX_FLAGS "")
