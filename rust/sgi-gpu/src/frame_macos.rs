// SPDX-License-Identifier: BSD-3-Clause
//! Frame export on macOS. Placeholder with the shape `frame.rs` documents:
//! no frame is exportable until this is filled in.

use std::sync::Arc;

use iris_wgpu::wgpu::Gpu;

use super::SgiGpuExport;

pub struct Export;

impl Export {
    pub fn new(_gpu: &Arc<Gpu>, _width: u32, _height: u32, _stride_words: u32) -> Result<(wgpu::Buffer, Export), String> {
        Err("frame export is not built on macOS yet".into())
    }

    pub fn info(&self, width: u32, height: u32, _stride_words: u32) -> SgiGpuExport {
        SgiGpuExport::none(width, height)
    }

    pub fn free(self, _gpu: &Gpu) {}
}
