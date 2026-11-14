# SPDX-License-Identifier: BSD-3-Clause
# vcpkg's arm64-osx-release, built for the oldest macOS the release supports.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 15.0)
set(VCPKG_BUILD_TYPE release)
