// SPDX-License-Identifier: BSD-3-Clause
//! SGI graphics GPU back end: the C interface in `include/hw/display/sgi-gpu.h`
//! and `sgi-gpu-vpro.h`, on iris's wgpu layer.

// The pub(crate) API is for the modules below as they are filled in.
#[allow(dead_code)]
mod device;
#[allow(dead_code)]
mod plane;

mod fill;
mod frame;
#[cfg(all(test, target_os = "linux"))]
mod frame_viewer_test;
mod scanout;
mod vpro;
