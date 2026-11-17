// SPDX-License-Identifier: BSD-3-Clause
//! Frames: XRGB8888 scan-out pictures, lent as dma-bufs where the adapter exports one, else an
//! ordinary buffer of kind NONE. Platform `Export`s: Linux here, frame_win.rs, frame_macos.rs.

use std::ffi::c_int;
use std::sync::{Arc, Mutex};

use iris_wgpu::wgpu::{Gpu, Staging};

use crate::device::{from_c, SgiGpu};

pub const EXPORT_NONE: c_int = 0;
#[allow(dead_code)]
pub const EXPORT_DMABUF: c_int = 1;
#[allow(dead_code)]
pub const EXPORT_IOSURFACE: c_int = 2;
#[allow(dead_code)]
pub const EXPORT_WIN32: c_int = 3;

/// DRM_FORMAT_XRGB8888: our 0x00RRGGBB words, little-endian.
const FOURCC_XR24: u32 = 0x3432_5258;
/// Row pitch granularity: what viewers and importers accept for a linear
/// dma-buf.
const PITCH_ALIGN: u32 = 256;

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

impl SgiGpuExport {
    /// A frame that cannot be lent: the viewer gets shared memory.
    pub fn none(width: u32, height: u32) -> SgiGpuExport {
        SgiGpuExport { kind: EXPORT_NONE, width, height, stride: 0, fourcc: 0, modifier: 0, offset: 0, handle: -1, fence: -1, adapter_id: 0 }
    }
}

pub struct SgiGpuFrame {
    gpu: Arc<Gpu>,
    width: u32,
    height: u32,
    stride_words: u32,
    buffer: wgpu::Buffer,
    staging: Mutex<Staging>,
    export: Option<Export>,
}

impl SgiGpuFrame {
    pub fn new(gpu: &Arc<Gpu>, width: u32, height: u32) -> Result<SgiGpuFrame, String> {
        if width == 0 || height == 0 {
            return Err("frame: empty size".into());
        }
        let stride_words = width.next_multiple_of(PITCH_ALIGN / 4);
        let size = stride_words as u64 * height as u64 * 4;
        let (buffer, export) = match Export::new(gpu, width, height, stride_words) {
            Ok((buffer, e)) => (buffer, Some(e)),
            Err(why) => {
                if std::env::var_os("SGI_GPU_EXPORT_LOG").is_some() {
                    eprintln!("sgi-gpu: frame not exportable: {why}");
                }
                let usage = wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST;
                (gpu.buffer("sgi frame", size, usage), None)
            }
        };
        Ok(SgiGpuFrame { gpu: Arc::clone(gpu), width, height, stride_words, buffer, staging: Mutex::new(Staging::default()), export })
    }

    pub fn buffer(&self) -> &wgpu::Buffer {
        &self.buffer
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    /// Words from one row to the next.
    pub fn stride_words(&self) -> u32 {
        self.stride_words
    }

    pub fn export_info(&self) -> SgiGpuExport {
        match &self.export {
            Some(e) => e.info(self.width, self.height, self.stride_words),
            None => SgiGpuExport::none(self.width, self.height),
        }
    }

    /// Wait for the work submitted so far, then copy the picture into
    /// `out`, rows of `out_stride` words.
    pub fn read(&self, out: &mut [u32], out_stride: usize) -> Result<(), String> {
        let (w, h) = (self.width as usize, self.height as usize);
        if out_stride < w || out.len() < out_stride * (h - 1) + w {
            return Err("frame: destination too small".into());
        }
        let bytes = self.stride_words as u64 * self.height as u64 * 4;
        self.gpu.check()?;
        let raw = self.staging.lock().map_err(|_| "frame: staging poisoned")?.read(&self.gpu, &self.buffer, &[(0, bytes)])?;
        self.gpu.check()?;
        let words: &[u32] = bytemuck::cast_slice(&raw);
        for y in 0..h {
            out[y * out_stride..y * out_stride + w].copy_from_slice(&words[y * self.stride_words as usize..][..w]);
        }
        Ok(())
    }
}

impl Drop for SgiGpuFrame {
    fn drop(&mut self) {
        if let Some(e) = self.export.take() {
            // The memory outlives the buffer wgpu wraps around it: free it
            // once nothing on the GPU can still touch it.
            self.buffer.destroy();
            let _ = self.gpu.wait(None);
            e.free(&self.gpu);
        }
    }
}

#[cfg(target_os = "linux")]
use linux::Export;
#[cfg(windows)]
#[path = "frame_win.rs"]
mod frame_win;
#[cfg(windows)]
use frame_win::Export;
#[cfg(target_vendor = "apple")]
#[path = "frame_macos.rs"]
mod frame_macos;
#[cfg(target_vendor = "apple")]
use frame_macos::Export;
#[cfg(not(any(target_os = "linux", windows, target_vendor = "apple")))]
use other::Export;

#[cfg(target_os = "linux")]
mod linux {
    use super::*;
    use ash::vk;
    use wgpu_hal::api::Vulkan;

    pub struct Export {
        fd: c_int,
        memory: vk::DeviceMemory,
        adapter_id: u64,
    }

    impl Export {
        pub fn info(&self, width: u32, height: u32, stride_words: u32) -> SgiGpuExport {
            SgiGpuExport {
                kind: EXPORT_DMABUF,
                width,
                height,
                stride: stride_words * 4,
                fourcc: FOURCC_XR24,
                modifier: 0,
                offset: 0,
                handle: self.fd as i64,
                fence: -1,
                adapter_id: self.adapter_id,
            }
        }

        pub fn new(gpu: &Arc<Gpu>, _width: u32, height: u32, stride_words: u32) -> Result<(wgpu::Buffer, Export), String> {
            let size = stride_words as u64 * height as u64 * 4;
            if std::env::var_os("SGI_GPU_NO_EXPORT").is_some() {
                return Err("disabled by SGI_GPU_NO_EXPORT".into());
            }
            let handle = vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT;
            // SAFETY: the raw handles are borrowed from wgpu's live device; what is made
            // here goes to wgpu or into `Export`, freed in `free` once the GPU is idle.
            unsafe {
                let hd = gpu.device.as_hal::<Vulkan>().ok_or("not a Vulkan device")?;
                let raw = hd.raw_device().clone();
                let inst = hd.shared_instance().raw_instance().clone();
                let pd = hd.raw_physical_device();
                drop(hd);
                let usage = vk::BufferUsageFlags::STORAGE_BUFFER | vk::BufferUsageFlags::TRANSFER_SRC | vk::BufferUsageFlags::TRANSFER_DST;
                let info = vk::PhysicalDeviceExternalBufferInfo::default().usage(usage).handle_type(handle);
                let mut props = vk::ExternalBufferProperties::default();
                inst.get_physical_device_external_buffer_properties(pd, &info, &mut props);
                let f = props.external_memory_properties.external_memory_features;
                if !f.contains(vk::ExternalMemoryFeatureFlags::EXPORTABLE) {
                    return Err(format!("dma-buf buffers are not exportable here (features {f:?})"));
                }
                let ext_mem = ash::khr::external_memory_fd::Device::new(&inst, &raw);
                let mut ebci = vk::ExternalMemoryBufferCreateInfo::default().handle_types(handle);
                let buf = raw
                    .create_buffer(&vk::BufferCreateInfo::default().size(size).usage(usage).sharing_mode(vk::SharingMode::EXCLUSIVE).push_next(&mut ebci), None)
                    .map_err(|e| format!("create_buffer: {e:?}"))?;
                let made = (|| {
                    let req = raw.get_buffer_memory_requirements(buf);
                    let ti = memory_type(&inst, pd, req.memory_type_bits).ok_or("no memory type")?;
                    let mut ema = vk::ExportMemoryAllocateInfo::default().handle_types(handle);
                    let mut ded = vk::MemoryDedicatedAllocateInfo::default().buffer(buf);
                    let mut mai = vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(ti).push_next(&mut ema);
                    if f.contains(vk::ExternalMemoryFeatureFlags::DEDICATED_ONLY) {
                        mai = mai.push_next(&mut ded);
                    }
                    let mem = raw.allocate_memory(&mai, None).map_err(|e| format!("allocate_memory: {e:?}"))?;
                    let bound = raw
                        .bind_buffer_memory(buf, mem, 0)
                        .map_err(|e| format!("bind_buffer_memory: {e:?}"))
                        .and_then(|_| ext_mem.get_memory_fd(&vk::MemoryGetFdInfoKHR::default().memory(mem).handle_type(handle)).map_err(|e| format!("vkGetMemoryFdKHR: {e:?}")));
                    match bound {
                        Ok(fd) => Ok((mem, fd)),
                        Err(e) => {
                            raw.free_memory(mem, None);
                            Err(e)
                        }
                    }
                })();
                let (memory, fd) = match made {
                    Ok(v) => v,
                    Err(e) => {
                        raw.destroy_buffer(buf, None);
                        return Err(e);
                    }
                };
                let hal_buf = wgpu_hal::vulkan::Buffer::from_raw(buf);
                let buffer = gpu.device.create_buffer_from_hal::<Vulkan>(
                    hal_buf,
                    &wgpu::BufferDescriptor {
                        label: Some("sgi frame"),
                        size,
                        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
                        mapped_at_creation: false,
                    },
                );
                Ok((buffer, Export { fd, memory, adapter_id: render_node(&inst, pd) }))
            }
        }

        /// After the buffer's destroy and a wait for the GPU.
        pub fn free(self, gpu: &Gpu) {
            // SAFETY: the buffer is destroyed and the queue waited for.
            unsafe {
                if let Some(hd) = gpu.device.as_hal::<Vulkan>() {
                    hd.raw_device().free_memory(self.memory, None);
                }
                libc::close(self.fd);
            }
        }
    }

    /// The render node's `dev_t`, which a viewer matches against its own
    /// adapter; 0 when the driver cannot say.
    fn render_node(inst: &ash::Instance, pd: vk::PhysicalDevice) -> u64 {
        // SAFETY: plain queries on a live physical device; the properties
        // chain is only used when the extension is advertised.
        unsafe {
            let has = inst
                .enumerate_device_extension_properties(pd)
                .is_ok_and(|v| v.iter().any(|e| e.extension_name_as_c_str().is_ok_and(|n| n == ash::ext::physical_device_drm::NAME)));
            if !has {
                return 0;
            }
            let mut drm = vk::PhysicalDeviceDrmPropertiesEXT::default();
            let mut props = vk::PhysicalDeviceProperties2::default().push_next(&mut drm);
            inst.get_physical_device_properties2(pd, &mut props);
            if drm.has_render == vk::TRUE {
                libc::makedev(drm.render_major as u32, drm.render_minor as u32)
            } else {
                0
            }
        }
    }

    fn memory_type(inst: &ash::Instance, pd: vk::PhysicalDevice, bits: u32) -> Option<u32> {
        // SAFETY: plain query on a live physical device.
        let mp = unsafe { inst.get_physical_device_memory_properties(pd) };
        let mut any = None;
        for i in 0..mp.memory_type_count {
            if bits & (1 << i) == 0 {
                continue;
            }
            if mp.memory_types[i as usize].property_flags.contains(vk::MemoryPropertyFlags::DEVICE_LOCAL) {
                return Some(i);
            }
            any.get_or_insert(i);
        }
        any
    }
}

#[cfg(not(any(target_os = "linux", windows, target_vendor = "apple")))]
mod other {
    use super::*;

    pub struct Export;

    impl Export {
        pub fn info(&self, width: u32, height: u32, _stride_words: u32) -> SgiGpuExport {
            SgiGpuExport::none(width, height)
        }

        pub fn new(_gpu: &Arc<Gpu>, _width: u32, _height: u32, _stride_words: u32) -> Result<(wgpu::Buffer, Export), String> {
            Err("no frame export on this platform".into())
        }

        pub fn free(self, _gpu: &Gpu) {}
    }
}

// ---- C interface (include/hw/display/sgi-gpu.h) ----

fn guarded<T>(what: &str, f: impl FnOnce() -> T) -> Option<T> {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(v) => Some(v),
        Err(_) => {
            eprintln!("sgi-gpu: {what} panicked");
            None
        }
    }
}

/// # Safety
/// `gpu` is a live `SgiGpu` from `sgi_gpu_open`, or null.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_frame_new(gpu: *mut SgiGpu, width: u32, height: u32) -> *mut SgiGpuFrame {
    let Some(gpu) = from_c(gpu).and_then(|g| g.gpu()) else { return std::ptr::null_mut() };
    match guarded("sgi_gpu_frame_new", || SgiGpuFrame::new(gpu, width, height)) {
        Some(Ok(f)) => Box::into_raw(Box::new(f)),
        Some(Err(e)) => {
            eprintln!("sgi-gpu: {e}");
            std::ptr::null_mut()
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `frame` is from `sgi_gpu_frame_new` and no compose names it.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_frame_free(frame: *mut SgiGpuFrame) {
    if !frame.is_null() {
        let _ = guarded("sgi_gpu_frame_free", || drop(Box::from_raw(frame)));
    }
}

/// # Safety
/// `frame` is live and `out` points to a `SgiGpuExport`.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_frame_export(frame: *mut SgiGpuFrame, out: *mut SgiGpuExport) -> bool {
    let (Some(frame), false) = (frame.as_ref(), out.is_null()) else { return false };
    let info = frame.export_info();
    let ok = info.kind != EXPORT_NONE;
    *out = info;
    ok
}

/// # Safety
/// `frame` is live; `xrgb` has room for `height` rows of `stride_words`.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_frame_read(frame: *mut SgiGpuFrame, xrgb: *mut u32, stride_words: usize) -> bool {
    let Some(frame) = frame.as_ref() else { return false };
    if xrgb.is_null() || stride_words < frame.width() as usize {
        return false;
    }
    let len = stride_words * (frame.height() as usize - 1) + frame.width() as usize;
    let out = std::slice::from_raw_parts_mut(xrgb, len);
    match guarded("sgi_gpu_frame_read", || frame.read(out, stride_words)) {
        Some(Ok(())) => true,
        Some(Err(e)) => {
            eprintln!("sgi-gpu: {e}");
            false
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iris_wgpu::wgpu::GpuOptions;

    #[test]
    fn exports_a_frame_or_says_none() {
        let gpu = match Gpu::new(&GpuOptions::default()) {
            Ok(g) => g,
            Err(e) => {
                eprintln!("SKIPPED: no GPU for the frame test: {e}");
                return;
            }
        };
        let frame = SgiGpuFrame::new(&gpu, 100, 40).unwrap();
        let info = frame.export_info();
        eprintln!("adapter {}: export kind {} stride {} handle {} adapter_id {:#x}", gpu.caps.adapter, info.kind, info.stride, info.handle, info.adapter_id);
        assert_eq!((info.width, info.height), (100, 40));
        // Whatever this platform exports, or nothing where it cannot.
        if info.kind != EXPORT_NONE {
            assert!([EXPORT_DMABUF, EXPORT_IOSURFACE, EXPORT_WIN32].contains(&info.kind));
            assert!(info.handle >= 0 && info.stride >= 400 && info.fourcc == FOURCC_XR24);
            if info.kind == EXPORT_DMABUF {
                assert!(info.stride % PITCH_ALIGN == 0);
            }
        }
        // Either way the picture is there to read back.
        let mut out = vec![0xdead_beefu32; 100 * 40];
        let words = vec![0x0012_3456u32; frame.stride_words() as usize * 40];
        gpu.write(frame.buffer(), 0, bytemuck::cast_slice(&words));
        frame.read(&mut out, 100).unwrap();
        assert!(out.iter().all(|&w| w == 0x0012_3456));
    }
}
