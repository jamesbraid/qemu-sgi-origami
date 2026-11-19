// SPDX-License-Identifier: BSD-3-Clause
// Ported from src/wgpu/memory.rs in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
//! A board's memory on the GPU: one storage buffer nothing on the CPU
//! maps. Writes go up through the queue; reads come back through the
//! device's lent staging buffers (`Gpu::read`). Both are ordered against the kernels by submission
//! order, so a readback issued after a batch sees that batch.

use std::sync::Arc;

use super::device::Gpu;

pub struct GpuMemory {
    gpu: Arc<Gpu>,
    pub buffer: wgpu::Buffer,
    size: u64,
}

impl GpuMemory {
    /// `size` bytes, zeroed, usable as read-write storage and as a copy
    /// source and destination.
    pub fn new(gpu: &Arc<Gpu>, label: &str, size: u64) -> Result<GpuMemory, String> {
        if size > gpu.caps.max_storage_binding {
            return Err(format!(
                "{label}: {size} bytes do not fit one storage binding of {} bytes",
                gpu.caps.max_storage_binding
            ));
        }
        let buffer = gpu.buffer(
            label,
            size,
            wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
        );
        Ok(GpuMemory { gpu: Arc::clone(gpu), buffer, size })
    }

    pub fn size(&self) -> u64 {
        self.size
    }

    /// Queue a write of `bytes` at `offset`. Lands before anything
    /// submitted after it.
    pub fn write(&self, offset: u64, bytes: &[u8]) {
        debug_assert!(offset + bytes.len() as u64 <= self.size);
        self.gpu.write(&self.buffer, offset, bytes);
    }

    /// Zero `len` bytes at `offset`, ordered like a write.
    pub fn clear(&self, offset: u64, len: u64) {
        let mut enc = self.gpu.encoder("memory clear");
        enc.clear_buffer(&self.buffer, offset, Some(len));
        self.gpu.submit(enc);
    }

    /// Read `len` bytes at `offset`, waiting for every submission so far.
    /// `offset` and `len` are rounded out to 4 bytes.
    pub fn read(&self, offset: u64, len: u64) -> Result<Vec<u8>, String> {
        let start = offset & !3;
        let end = (offset + len + 3) & !3;
        let mut out = self.read_ranges(&[(start, end - start)])?;
        let skip = (offset - start) as usize;
        out.drain(..skip);
        out.truncate(len as usize);
        Ok(out)
    }

    /// Read several ranges (offset, length; each 4-aligned) in one copy
    /// and one wait, concatenated in order.
    pub fn read_ranges(&self, ranges: &[(u64, u64)]) -> Result<Vec<u8>, String> {
        let mut out = vec![0u8; ranges.iter().map(|r| r.1).sum::<u64>() as usize];
        self.gpu.read(&self.buffer, ranges, &mut out)?;
        Ok(out)
    }

    /// The whole memory.
    pub fn read_all(&self) -> Result<Vec<u8>, String> {
        self.read(0, self.size)
    }

    pub fn gpu(&self) -> &Arc<Gpu> {
        &self.gpu
    }
}
