// SPDX-License-Identifier: BSD-3-Clause
//! VPro (Odyssey) drawing (VPro owner). Stub: every call fails with fence 0.

use crate::plane::SgiGpuPlane;

#[repr(C)]
pub struct SgiGpuVproTri {
    pub x: [i32; 3],
    pub y: [i32; 3],
    pub colour: u32,
    pub write_mask: u32,
    pub clip_x0: i32,
    pub clip_y0: i32,
    pub clip_x1: i32,
    pub clip_y1: i32,
}

#[repr(C)]
pub struct SgiGpuVproClear {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
    pub value: u32,
    pub write_mask: u32,
}

#[no_mangle]
pub extern "C" fn sgi_gpu_vpro_tris(_colour: *mut SgiGpuPlane, _tris: *const SgiGpuVproTri, _count: usize) -> u64 {
    0
}

#[no_mangle]
pub extern "C" fn sgi_gpu_vpro_clear(_plane: *mut SgiGpuPlane, _clear: *const SgiGpuVproClear) -> u64 {
    0
}
