# SPDX-License-Identifier: BSD-3-Clause
# vcpkg's x64-mingw-static-release. GCC 15 makes incompatible pointer types
# an error, which GLib at the pinned vcpkg commit still trips over.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_ENV_PASSTHROUGH PATH)
set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS -Wno-error=incompatible-pointer-types)
# vcpkg requires C++ flags whenever C flags are set.
set(VCPKG_CXX_FLAGS "")
