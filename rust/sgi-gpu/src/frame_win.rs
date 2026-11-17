// SPDX-License-Identifier: BSD-3-Clause
//! Frame export on Windows: a D3D12 shared buffer and shared fence with NT handles that display-shm
//! duplicates into the viewer. Needs a D3D12 device; otherwise the frame stays an ordinary buffer.
#![allow(dead_code)] // signal() is for the compose, once viewers wait on the fence

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use iris_wgpu::wgpu::Gpu;
use wgpu::hal::api::Dx12;
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{CloseHandle, GENERIC_ALL, HANDLE};
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};

use super::{SgiGpuExport, EXPORT_WIN32, FOURCC_XR24};

pub struct Export {
    fence: ID3D12Fence,
    handle: HANDLE,
    fence_handle: HANDLE,
    luid: u64,
    /// Serialises "next value, stage, submit" so the fence only goes up.
    signals: Mutex<()>,
    value: AtomicU64,
}

// SAFETY: D3D12 objects are free-threaded and the handles are plain values.
unsafe impl Send for Export {}
unsafe impl Sync for Export {}

/// An unnamed NT handle for a shareable D3D12 object.
///
/// # Safety
/// `raw` is a live device and `obj` one of its resources or fences.
unsafe fn share(raw: &ID3D12Device, obj: &impl Interface, what: &str) -> Result<HANDLE, String> {
    let child: ID3D12DeviceChild = obj.cast().map_err(|e| format!("{what}: {e}"))?;
    raw.CreateSharedHandle(&child, None, GENERIC_ALL.0, PCWSTR::null()).map_err(|e| format!("CreateSharedHandle({what}): {e}"))
}

impl Export {
    /// The compose buffer, `stride_words` x `height` words, made shareable.
    pub fn new(gpu: &Arc<Gpu>, _width: u32, height: u32, stride_words: u32) -> Result<(wgpu::Buffer, Export), String> {
        let size = stride_words as u64 * height as u64 * 4;
        // SAFETY: the raw device is cloned (a COM reference) and the wgpu
        // borrow ends before anything else runs.
        let raw = unsafe { gpu.device.as_hal::<Dx12>().ok_or("not a D3D12 device")?.raw_device().clone() };
        // SAFETY: plain D3D12 creation calls with descriptors built here; the
        // buffer is handed to wgpu, the rest is kept in `Export`.
        unsafe {
            let desc = D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
                Width: size,
                Height: 1,
                DepthOrArraySize: 1,
                MipLevels: 1,
                Format: DXGI_FORMAT_UNKNOWN,
                SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
                Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                Flags: D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                ..Default::default()
            };
            let heap = D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() };
            let mut res: Option<ID3D12Resource> = None;
            raw.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, None, &mut res)
                .map_err(|e| format!("CreateCommittedResource(SHARED) {size} bytes: {e}"))?;
            let res = res.ok_or("CreateCommittedResource returned nothing")?;
            let fence: ID3D12Fence = raw.CreateFence(0, D3D12_FENCE_FLAG_SHARED).map_err(|e| format!("CreateFence(SHARED): {e}"))?;
            let handle = share(&raw, &res, "buffer")?;
            let fence_handle = match share(&raw, &fence, "fence") {
                Ok(h) => h,
                Err(e) => {
                    let _ = CloseHandle(handle);
                    return Err(e);
                }
            };
            let luid = raw.GetAdapterLuid();
            let hal = wgpu::hal::dx12::Device::buffer_from_raw(res, size);
            let buffer = gpu.device.create_buffer_from_hal::<Dx12>(
                hal,
                &wgpu::BufferDescriptor {
                    label: Some("sgi frame"),
                    size,
                    usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                },
            );
            Ok((
                buffer,
                Export { fence, handle, fence_handle, luid: (u64::from(luid.HighPart as u32) << 32) | u64::from(luid.LowPart), signals: Mutex::new(()), value: AtomicU64::new(0) },
            ))
        }
    }

    /// What the viewer is told: the adapter's LUID is HighPart << 32 | LowPart.
    /// Both handles stay ours; the caller duplicates what it lends.
    pub fn info(&self, width: u32, height: u32, stride_words: u32) -> SgiGpuExport {
        SgiGpuExport {
            kind: EXPORT_WIN32,
            width,
            height,
            stride: stride_words * 4,
            fourcc: FOURCC_XR24,
            modifier: 0,
            offset: 0,
            handle: self.handle.0 as i64,
            fence: self.fence_handle.0 as i64,
            adapter_id: self.luid,
        }
    }

    /// Signal the shared fence behind the compose's submit and return its value;
    /// an empty submission carries the signal so no other submitter passes it.
    pub fn signal(&self, gpu: &Gpu) -> Result<u64, String> {
        let _order = self.signals.lock().map_err(|_| "frame: signal lock poisoned")?;
        let value = self.value.fetch_add(1, Ordering::AcqRel) + 1;
        // SAFETY: the hal queue borrow ends before the submit below.
        unsafe { gpu.queue.as_hal::<Dx12>().ok_or("not a D3D12 queue")?.add_signal_fence(self.fence.clone(), value) };
        gpu.queue.submit(std::iter::empty());
        Ok(value)
    }

    /// After the buffer is destroyed and the GPU idle.
    pub fn free(self, _gpu: &Gpu) {}
}

impl Drop for Export {
    fn drop(&mut self) {
        // SAFETY: both handles are ours and closed once; the resource is wgpu's
        // buffer's, so closing a handle frees nothing.
        unsafe {
            let _ = CloseHandle(self.handle);
            let _ = CloseHandle(self.fence_handle);
        }
    }
}
