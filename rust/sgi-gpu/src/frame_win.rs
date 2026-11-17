// SPDX-License-Identifier: BSD-3-Clause
//! Frame export on Windows. Placeholder with the shape `frame.rs` documents:
//! no frame is exportable until this is filled in.

use std::sync::Arc;

use iris_wgpu::wgpu::Gpu;

use super::{SgiGpuExport, EXPORT_NONE};

pub struct Export;

impl Export {
    pub fn new(_gpu: &Arc<Gpu>, _width: u32, _height: u32, _stride_words: u32) -> Result<(wgpu::Buffer, Export), String> {
        Err("frame export is not built on Windows yet".into())
    }

    pub fn info(&self, width: u32, height: u32, _stride_words: u32) -> SgiGpuExport {
        SgiGpuExport { kind: EXPORT_NONE, width, height, stride: 0, fourcc: 0, modifier: 0, offset: 0, handle: -1 }
    }

    pub fn free(self, _gpu: &Gpu) {}
}
