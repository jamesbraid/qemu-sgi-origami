// SPDX-License-Identifier: BSD-3-Clause
//! Exportable frames (frame owner). Stub: no frame is ever available.

use std::ffi::c_int;

use crate::device::SgiGpu;

pub struct SgiGpuFrame;

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
}

#[no_mangle]
pub extern "C" fn sgi_gpu_frame_new(_gpu: *mut SgiGpu, _width: u32, _height: u32) -> *mut SgiGpuFrame {
    std::ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn sgi_gpu_frame_free(_frame: *mut SgiGpuFrame) {}

#[no_mangle]
pub extern "C" fn sgi_gpu_frame_export(_frame: *mut SgiGpuFrame, _out: *mut SgiGpuExport) -> bool {
    false
}

#[no_mangle]
pub extern "C" fn sgi_gpu_frame_read(_frame: *mut SgiGpuFrame, _xrgb: *mut u32, _stride_words: usize) -> bool {
    false
}
