// SPDX-License-Identifier: BSD-3-Clause
//! The items rust/sgi-gpu/src/frame.rs gives its platform module.

use std::ffi::c_int;

pub const EXPORT_WIN32: c_int = 3;
pub const FOURCC_XR24: u32 = 0x3432_5258;

#[repr(C)]
pub struct SgiGpuExport {
    pub kind: c_int,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub fourcc: u32,
    pub modifier: u64,
    pub offset: u64,
    pub handle: i64,
    pub fence: i64,
    pub adapter_id: u64,
}

#[path = "../../sgi-gpu/src/frame_win.rs"]
pub mod frame_win;
