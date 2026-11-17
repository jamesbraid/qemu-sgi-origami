// SPDX-License-Identifier: BSD-3-Clause
//! macOS frames: the compose writes a Metal buffer made over an IOSurface's
//! memory, and the surface is lent to the viewer as a Mach port (display-shm
//! class 5). The producer waits for the GPU, so no event crosses.

use std::ffi::c_void;
use std::ptr::NonNull;

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2_metal::{MTLBuffer, MTLDevice, MTLResourceOptions};
use wgpu_hal::api::Metal;

use super::*;

/// 'BGRA': the bytes of an XRGB8888 word, X undefined.
const FOURCC_BGRA: u32 = 0x4247_5241;

type CFRef = *const c_void;

#[link(name = "IOSurface", kind = "framework")]
#[link(name = "CoreFoundation", kind = "framework")]
extern "C" {
    static kIOSurfaceWidth: CFRef;
    static kIOSurfaceHeight: CFRef;
    static kIOSurfaceBytesPerElement: CFRef;
    static kIOSurfaceBytesPerRow: CFRef;
    static kIOSurfacePixelFormat: CFRef;
    static kCFTypeDictionaryKeyCallBacks: c_void;
    static kCFTypeDictionaryValueCallBacks: c_void;
    static mach_task_self_: u32;
    fn CFDictionaryCreateMutable(a: CFRef, cap: isize, k: *const c_void, v: *const c_void) -> *mut c_void;
    fn CFDictionarySetValue(d: *mut c_void, k: CFRef, v: CFRef);
    fn CFNumberCreate(a: CFRef, t: i32, p: *const c_void) -> CFRef;
    fn CFRelease(r: CFRef);
    fn IOSurfaceCreate(props: CFRef) -> CFRef;
    fn IOSurfaceCreateMachPort(s: CFRef) -> u32;
    fn IOSurfaceGetBaseAddress(s: CFRef) -> *mut c_void;
    fn IOSurfaceGetBytesPerRow(s: CFRef) -> usize;
    fn IOSurfaceGetAllocSize(s: CFRef) -> usize;
    fn mach_port_deallocate(task: u32, name: u32) -> i32;
    fn getpagesize() -> i32;
}

fn number(v: i64) -> CFRef {
    // SAFETY: kCFNumberSInt64Type (4) reads eight bytes from `v`.
    unsafe { CFNumberCreate(std::ptr::null(), 4, &v as *const i64 as *const c_void) }
}

/// An IOSurface and the send right to it that the frame lends.
pub struct Export {
    surface: CFRef,
    port: u32,
}

// SAFETY: a CoreFoundation object and a port name; IOSurface calls are thread safe.
unsafe impl Send for Export {}
unsafe impl Sync for Export {}

impl Export {
    pub fn info(&self, width: u32, height: u32, stride_words: u32) -> SgiGpuExport {
        SgiGpuExport {
            kind: EXPORT_IOSURFACE,
            width,
            height,
            stride: stride_words * 4,
            fourcc: FOURCC_BGRA,
            modifier: 0,
            offset: 0,
            handle: self.port as i64,
            fence: -1,
            adapter_id: 0,
        }
    }

    pub fn new(gpu: &Arc<Gpu>, width: u32, height: u32, stride_words: u32) -> Result<(wgpu::Buffer, Export), String> {
        if std::env::var_os("SGI_GPU_NO_EXPORT").is_some() {
            return Err("disabled by SGI_GPU_NO_EXPORT".into());
        }
        let stride = stride_words as usize * 4;
        let size = stride as u64 * height as u64;
        // SAFETY: the surface and port made here are kept in `Export` and
        // released in `free`; the buffer made over the surface's memory is
        // destroyed before that, with the GPU idle.
        unsafe {
            let dict = CFDictionaryCreateMutable(std::ptr::null(), 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            let mut owned = Vec::new();
            for (key, value) in [
                (kIOSurfaceWidth, width as i64),
                (kIOSurfaceHeight, height as i64),
                (kIOSurfaceBytesPerElement, 4),
                (kIOSurfaceBytesPerRow, stride as i64),
                (kIOSurfacePixelFormat, FOURCC_BGRA as i64),
            ] {
                let n = number(value);
                CFDictionarySetValue(dict, key, n);
                owned.push(n);
            }
            let surface = IOSurfaceCreate(dict as CFRef);
            for n in owned {
                CFRelease(n);
            }
            CFRelease(dict as CFRef);
            if surface.is_null() {
                return Err("IOSurfaceCreate failed".into());
            }
            let made = (|| {
                let alloc = IOSurfaceGetAllocSize(surface);
                let base = IOSurfaceGetBaseAddress(surface);
                let page = getpagesize() as usize;
                if IOSurfaceGetBytesPerRow(surface) != stride {
                    return Err(format!("IOSurface row is {} bytes, not {stride}", IOSurfaceGetBytesPerRow(surface)));
                }
                if base.is_null() || base as usize % page != 0 || alloc % page != 0 || (alloc as u64) < size {
                    return Err("IOSurface memory is not page aligned".to_string());
                }
                let device = gpu.device.as_hal::<Metal>().ok_or("not a Metal device")?.raw_device().clone();
                // Shared memory on Apple silicon: the buffer and a texture
                // over the same surface see the same bytes once the GPU is done.
                let raw: Retained<ProtocolObject<dyn MTLBuffer>> = device
                    .newBufferWithBytesNoCopy_length_options_deallocator(NonNull::new_unchecked(base), alloc, MTLResourceOptions::StorageModeShared, None)
                    .ok_or("newBufferWithBytesNoCopy failed")?;
                let port = IOSurfaceCreateMachPort(surface);
                if port == 0 {
                    return Err("IOSurfaceCreateMachPort failed".to_string());
                }
                Ok((raw, port))
            })();
            let (raw, port) = match made {
                Ok(v) => v,
                Err(e) => {
                    CFRelease(surface);
                    return Err(e);
                }
            };
            let buffer = gpu.device.create_buffer_from_hal::<Metal>(
                wgpu_hal::metal::Device::buffer_from_raw(raw, size),
                &wgpu::BufferDescriptor {
                    label: Some("sgi frame"),
                    size,
                    usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
                    mapped_at_creation: false,
                },
            );
            Ok((buffer, Export { surface, port }))
        }
    }

    /// After the buffer's destroy and a wait for the GPU.
    pub fn free(self, _gpu: &Gpu) {
        // SAFETY: the buffer is destroyed and the queue waited for; display-shm
        // holds its own send right to the port.
        unsafe {
            mach_port_deallocate(mach_task_self_, self.port);
            CFRelease(self.surface);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iris_wgpu::wgpu::GpuOptions;
    use objc2_io_surface::IOSurfaceRef;
    use objc2_metal::{MTLPixelFormat, MTLStorageMode, MTLTextureDescriptor, MTLTextureType, MTLTextureUsage};

    #[link(name = "IOSurface", kind = "framework")]
    extern "C" {
        fn IOSurfaceLookupFromMachPort(port: u32) -> CFRef;
    }

    fn word(x: u32, y: u32) -> u32 {
        (x * 3 + y * 5) & 0xff | (((x ^ y) & 0xff) << 8) | ((x.wrapping_mul(y) & 0xff) << 16)
    }

    /// What a viewer does: look the port up, make a texture over the
    /// surface, copy it out on the GPU and compare with what the compose wrote.
    #[test]
    fn a_viewer_samples_what_the_buffer_wrote() {
        let gpu = match Gpu::new(&GpuOptions::default()) {
            Ok(g) => g,
            Err(e) => {
                eprintln!("SKIPPED: no GPU for the frame test: {e}");
                return;
            }
        };
        let (w, h) = (64u32, 40u32);
        let frame = SgiGpuFrame::new(&gpu, w, h).unwrap();
        let info = frame.export_info();
        assert_eq!(info.kind, EXPORT_IOSURFACE);
        assert!(info.handle > 0 && info.stride % 256 == 0 && info.fourcc == FOURCC_BGRA);
        let stride_words = frame.stride_words() as usize;
        let mut words = vec![0u32; stride_words * h as usize];
        for y in 0..h {
            for x in 0..w {
                words[y as usize * stride_words + x as usize] = word(x, y);
            }
        }
        gpu.write(frame.buffer(), 0, bytemuck::cast_slice(&words));
        // The read flushes the write and waits for the GPU.
        let mut out = vec![0u32; (w * h) as usize];
        frame.read(&mut out, w as usize).unwrap();
        assert_eq!(out[1], word(1, 0));

        // SAFETY: the port is live for the frame's life; the texture is
        // dropped before it.
        unsafe {
            let surface = IOSurfaceLookupFromMachPort(info.handle as u32);
            assert!(!surface.is_null());
            let device = gpu.device.as_hal::<Metal>().unwrap().raw_device().clone();
            let desc = MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(MTLPixelFormat::BGRA8Unorm, w as usize, h as usize, false);
            desc.setUsage(MTLTextureUsage::ShaderRead);
            desc.setStorageMode(MTLStorageMode::Shared);
            let raw = device.newTextureWithDescriptor_iosurface_plane(&desc, &*(surface as *const IOSurfaceRef), 0).expect("texture over the IOSurface");
            let hal = wgpu_hal::metal::Device::texture_from_raw(raw, wgpu::TextureFormat::Bgra8Unorm, MTLTextureType::Type2D, 1, 1, wgpu_hal::CopyExtent { width: w, height: h, depth: 1 }, None);
            let tex = gpu.device.create_texture_from_hal::<Metal>(
                hal,
                &wgpu::TextureDescriptor {
                    label: None,
                    size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Bgra8Unorm,
                    usage: wgpu::TextureUsages::COPY_SRC,
                    view_formats: &[],
                },
                wgpu::TextureUses::COPY_SRC,
            );
            let rb = gpu.device.create_buffer(&wgpu::BufferDescriptor {
                label: None,
                size: (w * h * 4) as u64,
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            });
            let mut enc = gpu.device.create_command_encoder(&Default::default());
            enc.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo { texture: &tex, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                wgpu::TexelCopyBufferInfo { buffer: &rb, layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) } },
                wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            );
            gpu.queue.submit([enc.finish()]);
            rb.slice(..).map_async(wgpu::MapMode::Read, |_| {});
            gpu.device.poll(wgpu::PollType::Wait { submission_index: None, timeout: None }).unwrap();
            let got: Vec<u32> = bytemuck::cast_slice(&rb.slice(..).get_mapped_range().unwrap()).to_vec();
            for y in 0..h {
                for x in 0..w {
                    assert_eq!(got[(y * w + x) as usize], word(x, y), "pixel {x},{y}");
                }
            }
            drop(tex);
            CFRelease(surface);
        }
    }
}
