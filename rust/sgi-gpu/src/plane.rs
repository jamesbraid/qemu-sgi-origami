// SPDX-License-Identifier: BSD-3-Clause
//! Planes: one 32-bit word per pixel in a GPU storage buffer, with row-band
//! upload and blocking readback.

use std::sync::Arc;

use sgi_gpu_core::{Gpu, GpuMemory};
use parking_lot::Mutex;

use crate::device::{from_c, SgiGpu};

pub struct SgiGpuPlane {
    dev: &'static SgiGpu,
    gpu: Arc<Gpu>,
    width: u32,
    height: u32,
    buffer: wgpu::Buffer,
    mem: Mutex<GpuMemory>,
}

impl SgiGpuPlane {
    fn new(dev: &'static SgiGpu, width: u32, height: u32) -> Result<SgiGpuPlane, String> {
        let gpu = Arc::clone(dev.gpu().ok_or("the GPU is not ready")?);
        let bytes = u64::from(width) * u64::from(height) * 4;
        let mem = GpuMemory::new(&gpu, "sgi-gpu plane", bytes)?;
        Ok(SgiGpuPlane { dev, gpu, width, height, buffer: mem.buffer.clone(), mem: Mutex::new(mem) })
    }

    pub(crate) fn device(&self) -> &'static SgiGpu {
        self.dev
    }

    pub(crate) fn width(&self) -> u32 {
        self.width
    }

    pub(crate) fn height(&self) -> u32 {
        self.height
    }

    /// The storage buffer, `width * height` words, row-major; for kernels.
    pub(crate) fn buffer(&self) -> &wgpu::Buffer {
        &self.buffer
    }

    fn band(&self, y: u32, rows: u32) -> Result<(u64, u64), String> {
        match y.checked_add(rows) {
            Some(end) if end <= self.height => {
                let row = u64::from(self.width) * 4;
                Ok((u64::from(y) * row, u64::from(rows) * row))
            }
            _ => Err(format!("rows {y}+{rows} outside a plane of {} rows", self.height)),
        }
    }

    /// Queue `rows` rows from `words` (`stride` words apart) at row `y`; the
    /// next submission sees them.
    pub(crate) fn write_rows(&self, y: u32, rows: u32, words: &[u32], stride: usize) -> Result<(), String> {
        let (offset, _) = self.band(y, rows)?;
        let w = self.width as usize;
        if rows == 0 || w == 0 {
            return Ok(());
        }
        if stride < w || words.len() < (rows as usize - 1) * stride + w {
            return Err("write source too small for its rows".into());
        }
        if stride == w {
            self.gpu.write(&self.buffer, offset, bytemuck::cast_slice(&words[..rows as usize * w]));
        } else {
            let packed: Vec<u32> = (0..rows as usize).flat_map(|r| words[r * stride..][..w].iter().copied()).collect();
            self.gpu.write(&self.buffer, offset, bytemuck::cast_slice(&packed));
        }
        Ok(())
    }

    /// Wait for everything submitted so far and return `rows` rows at `y`.
    pub(crate) fn read_rows(&self, y: u32, rows: u32) -> Result<Vec<u32>, String> {
        let (offset, len) = self.band(y, rows)?;
        let bytes = self.mem.lock().read(offset, len)?;
        Ok(bytemuck::pod_collect_to_vec(&bytes))
    }
}

/// A `width` x `height` plane of zeroed words; NULL if the GPU is not ready.
#[no_mangle]
pub extern "C" fn sgi_gpu_plane_new(gpu: *mut SgiGpu, width: u32, height: u32) -> *mut SgiGpuPlane {
    let Some(dev) = from_c(gpu) else { return std::ptr::null_mut() };
    match SgiGpuPlane::new(dev, width, height) {
        Ok(p) => Box::into_raw(Box::new(p)),
        Err(e) => {
            eprintln!("sgi-gpu: plane {width}x{height}: {e}");
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn sgi_gpu_plane_free(plane: *mut SgiGpuPlane) {
    if !plane.is_null() {
        // SAFETY: the pointer came from sgi_gpu_plane_new and is freed once.
        drop(unsafe { Box::from_raw(plane) });
    }
}

#[no_mangle]
pub extern "C" fn sgi_gpu_plane_write(plane: *mut SgiGpuPlane, y: u32, rows: u32, words: *const u32, stride_words: usize) -> bool {
    // SAFETY: the caller passes a live plane and `rows` rows at `words`.
    let Some(p) = (unsafe { plane.as_ref() }) else { return false };
    if p.dev.gpu().is_none() || (rows > 0 && words.is_null()) {
        return false;
    }
    let len = if rows == 0 { 0 } else { (rows as usize - 1).saturating_mul(stride_words).saturating_add(p.width as usize) };
    // SAFETY: the C contract: `words` holds `rows` rows `stride_words` apart.
    let src = if len == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(words, len) } };
    match p.write_rows(y, rows, src, stride_words) {
        Ok(()) => true,
        Err(e) => {
            eprintln!("sgi-gpu: plane write: {e}");
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn sgi_gpu_plane_read(plane: *mut SgiGpuPlane, y: u32, rows: u32, words: *mut u32, stride_words: usize) -> bool {
    // SAFETY: the caller passes a live plane and room for `rows` rows.
    let Some(p) = (unsafe { plane.as_ref() }) else { return false };
    if p.dev.gpu().is_none() || (rows > 0 && words.is_null()) {
        return false;
    }
    let w = p.width as usize;
    if rows > 1 && stride_words < w {
        return false;
    }
    let data = match p.read_rows(y, rows) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("sgi-gpu: plane read: {e}");
            return false;
        }
    };
    for r in 0..rows as usize {
        // SAFETY: the C contract: `words` holds `rows` rows `stride_words` apart.
        let dst = unsafe { std::slice::from_raw_parts_mut(words.add(r * stride_words), w) };
        dst.copy_from_slice(&data[r * w..][..w]);
    }
    true
}
