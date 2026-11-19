// SPDX-License-Identifier: BSD-3-Clause
//! A second Vulkan device imports a frame's dma-buf and checks it holds the composed picture, and
//! times a GPU slot against shared memory. Needs an exporting GPU; otherwise it says so and passes.

use std::os::fd::RawFd;
use std::sync::Arc;
use std::time::Instant;

use ash::vk;
use sgi_gpu_core::{Gpu, GpuOptions};

use crate::frame::{SgiGpuFrame, EXPORT_DMABUF};
use crate::scanout::{IdRows, SgiGpuVproTables, VproCompose};

const W: u32 = 1280;
const H: u32 = 1024;

/// Position-weighted sum of a picture's words, rows of `stride` words.
fn checksum(words: &[u32], w: usize, h: usize, stride: usize) -> u64 {
    let mut sum = 0u64;
    for y in 0..h {
        for x in 0..w {
            sum = sum.wrapping_add((words[y * stride + x] as u64).wrapping_mul((y * w + x) as u64 % 65521 + 1));
        }
    }
    sum
}

struct Viewer {
    entry: ash::Entry,
    instance: ash::Instance,
    pd: vk::PhysicalDevice,
    device: ash::Device,
    queue: vk::Queue,
    family: u32,
}

impl Viewer {
    fn new() -> Result<Viewer, String> {
        // SAFETY: plain Vulkan setup on the system loader; destroyed in Drop.
        unsafe {
            let entry = ash::Entry::load().map_err(|e| format!("vulkan loader: {e}"))?;
            let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_1);
            let instance = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).map_err(|e| format!("create_instance: {e}"))?;
            let pd = instance.enumerate_physical_devices().map_err(|e| format!("{e}"))?.into_iter().next().ok_or("no physical device")?;
            let family = instance.get_physical_device_queue_family_properties(pd).iter().position(|q| q.queue_flags.contains(vk::QueueFlags::TRANSFER | vk::QueueFlags::COMPUTE)).ok_or("no queue")? as u32;
            let exts = [ash::khr::external_memory_fd::NAME.as_ptr(), ash::ext::external_memory_dma_buf::NAME.as_ptr()];
            let prio = [1.0f32];
            let qinfo = [vk::DeviceQueueCreateInfo::default().queue_family_index(family).queue_priorities(&prio)];
            let device = instance.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&qinfo).enabled_extension_names(&exts), None).map_err(|e| format!("create_device: {e}"))?;
            let queue = device.get_device_queue(family, 0);
            Ok(Viewer { entry, instance, pd, device, queue, family })
        }
    }

    fn memory_type(&self, bits: u32, want: vk::MemoryPropertyFlags) -> Option<u32> {
        // SAFETY: query on a live physical device.
        let mp = unsafe { self.instance.get_physical_device_memory_properties(self.pd) };
        (0..mp.memory_type_count).find(|&i| bits & (1 << i) != 0 && mp.memory_types[i as usize].property_flags.contains(want))
    }

    /// Import `fd` (consumed) as a buffer of `size` bytes and copy it out
    /// through a transfer, as a viewer would read the lent frame.
    fn read(&self, fd: RawFd, size: u64) -> Result<Vec<u32>, String> {
        // SAFETY: every object made here is destroyed before returning; the
        // fd is handed to the driver by the import.
        unsafe {
            let d = &self.device;
            let h = vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT;
            let fdp = ash::khr::external_memory_fd::Device::new(&self.instance, d);
            let mut fprops = vk::MemoryFdPropertiesKHR::default();
            fdp.get_memory_fd_properties(h, fd, &mut fprops).map_err(|e| format!("memory fd properties: {e}"))?;
            let mut ebci = vk::ExternalMemoryBufferCreateInfo::default().handle_types(h);
            let src = d.create_buffer(&vk::BufferCreateInfo::default().size(size).usage(vk::BufferUsageFlags::TRANSFER_SRC).push_next(&mut ebci), None).map_err(|e| format!("create_buffer: {e}"))?;
            let req = d.get_buffer_memory_requirements(src);
            let ti = self.memory_type(req.memory_type_bits & fprops.memory_type_bits, vk::MemoryPropertyFlags::empty()).ok_or("no importable memory type")?;
            let mut import = vk::ImportMemoryFdInfoKHR::default().handle_type(h).fd(fd);
            let mut ded = vk::MemoryDedicatedAllocateInfo::default().buffer(src);
            let src_mem = d.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(ti).push_next(&mut import).push_next(&mut ded), None).map_err(|e| format!("import: {e}"))?;
            d.bind_buffer_memory(src, src_mem, 0).map_err(|e| format!("{e}"))?;

            let dst = d.create_buffer(&vk::BufferCreateInfo::default().size(size).usage(vk::BufferUsageFlags::TRANSFER_DST), None).map_err(|e| format!("{e}"))?;
            let dreq = d.get_buffer_memory_requirements(dst);
            let dti = self.memory_type(dreq.memory_type_bits, vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT).ok_or("no host memory")?;
            let dst_mem = d.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(dreq.size).memory_type_index(dti), None).map_err(|e| format!("{e}"))?;
            d.bind_buffer_memory(dst, dst_mem, 0).map_err(|e| format!("{e}"))?;

            let pool = d.create_command_pool(&vk::CommandPoolCreateInfo::default().queue_family_index(self.family), None).map_err(|e| format!("{e}"))?;
            let cb = d.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(pool).command_buffer_count(1)).map_err(|e| format!("{e}"))?[0];
            d.begin_command_buffer(cb, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT)).map_err(|e| format!("{e}"))?;
            // Take the buffer from the producer's queue family, as a viewer does.
            let acquire = [vk::BufferMemoryBarrier::default()
                .dst_access_mask(vk::AccessFlags::TRANSFER_READ)
                .src_queue_family_index(vk::QUEUE_FAMILY_EXTERNAL)
                .dst_queue_family_index(self.family)
                .buffer(src)
                .size(vk::WHOLE_SIZE)];
            d.cmd_pipeline_barrier(cb, vk::PipelineStageFlags::TOP_OF_PIPE, vk::PipelineStageFlags::TRANSFER, vk::DependencyFlags::empty(), &[], &acquire, &[]);
            d.cmd_copy_buffer(cb, src, dst, &[vk::BufferCopy::default().size(size)]);
            d.end_command_buffer(cb).map_err(|e| format!("{e}"))?;
            let fence = d.create_fence(&vk::FenceCreateInfo::default(), None).map_err(|e| format!("{e}"))?;
            let cbs = [cb];
            d.queue_submit(self.queue, &[vk::SubmitInfo::default().command_buffers(&cbs)], fence).map_err(|e| format!("{e}"))?;
            d.wait_for_fences(&[fence], true, 10_000_000_000).map_err(|e| format!("wait: {e}"))?;
            let ptr = d.map_memory(dst_mem, 0, size, vk::MemoryMapFlags::empty()).map_err(|e| format!("{e}"))?;
            let mut out = vec![0u32; size as usize / 4];
            std::ptr::copy_nonoverlapping(ptr as *const u32, out.as_mut_ptr(), out.len());
            d.unmap_memory(dst_mem);
            d.destroy_fence(fence, None);
            d.destroy_command_pool(pool, None);
            d.destroy_buffer(dst, None);
            d.free_memory(dst_mem, None);
            d.destroy_buffer(src, None);
            d.free_memory(src_mem, None);
            Ok(out)
        }
    }
}

impl Drop for Viewer {
    fn drop(&mut self) {
        // SAFETY: nothing of the viewer's is in use.
        unsafe {
            self.device.destroy_device(None);
            self.instance.destroy_instance(None);
        }
        let _ = &self.entry;
    }
}

fn cpu_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: plain clock read.
    unsafe { libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

fn percentile(v: &mut [f64], p: f64) -> f64 {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[((v.len() - 1) as f64 * p) as usize]
}

#[test]
fn a_viewer_imports_the_lent_frame_and_the_routes_are_timed() {
    let gpu: Arc<Gpu> = match Gpu::new(&GpuOptions::default(), Box::new(|_| {})) {
        Ok(g) => g,
        Err(e) => {
            eprintln!("SKIPPED: no GPU: {e}");
            return;
        }
    };
    let frame = SgiGpuFrame::new(&gpu, W, H).unwrap();
    let info = frame.export_info();
    if info.kind != EXPORT_DMABUF {
        eprintln!("SKIPPED: {} cannot export a dma-buf; the frame is read back instead", gpu.caps.adapter);
        return;
    }
    let viewer = Viewer::new().expect("a viewer device");
    let compose = VproCompose::new(&gpu, W, H).unwrap();
    let usage = wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST;
    let size = W as u64 * H as u64 * 4;
    let (b0, b1) = (gpu.buffer("b0", size, usage), gpu.buffer("b1", size, usage));
    // Plane words that differ pixel to pixel, through colormap group 0.
    let plane: Vec<u32> = (0..W * H).map(|i| i.wrapping_mul(2654435761)).collect();
    gpu.write(&b0, 0, bytemuck::cast_slice(&plane));
    let mut t = SgiGpuVproTables { width: W, height: H, displayed_buffer: 0, lut: [0; 0x2000], dispmode: [0; 32], overlay_mode: [0; 8], cursor: [0; 0x100] };
    t.lut.iter_mut().enumerate().for_each(|(i, w)| *w = (i as u32).wrapping_mul(0x9e37_79b1) & 0x3fff_ffff);
    let rows = [IdRows { y: 0, did: vec![0; (W * H) as usize], overlay: vec![0; (W * H) as usize] }];
    compose.compose_wait(&t, &rows, &b0, &b1, &frame).unwrap();

    // The viewer's import must show what the producer reads back.
    let mut readback = vec![0u32; (W * H) as usize];
    frame.read(&mut readback, W as usize).unwrap();
    let want = checksum(&readback, W as usize, H as usize, W as usize);
    let fd = unsafe { libc::dup(info.handle as RawFd) };
    assert!(fd >= 0);
    let seen = viewer.read(fd, info.stride as u64 * H as u64).expect("import and read");
    let got = checksum(&seen, W as usize, H as usize, info.stride as usize / 4);
    assert_eq!(got, want, "the viewer's import differs from the readback");
    assert_ne!(want, 0);

    // Timing: producer side per frame, GPU slot against readback and a memfd.
    let frames = 300;
    let memfd = unsafe { libc::memfd_create(c"shm-slot".as_ptr(), 0) };
    assert!(memfd >= 0 && unsafe { libc::ftruncate(memfd, size as i64) } == 0);
    let map = unsafe { libc::mmap(std::ptr::null_mut(), size as usize, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED, memfd, 0) } as *mut u32;
    assert!(map as isize != -1);
    let mut verified = 0;
    for (name, shm) in [("gpu-slot", false), ("shm", true)] {
        let (mut lat, mut cpu) = (Vec::new(), 0u64);
        for i in 0..frames + 30 {
            // One row of the plane changes each frame, so each compose is new.
            gpu.write(&b0, 0, bytemuck::cast_slice(&vec![i as u32 ^ 0x5a5a_5a5a; W as usize]));
            let (c0, t0) = (cpu_ns(), Instant::now());
            compose.compose_wait(&t, &[], &b0, &b1, &frame).unwrap();
            if shm {
                frame.read(&mut readback, W as usize).unwrap();
                // SAFETY: the map is `size` bytes.
                unsafe { std::ptr::copy_nonoverlapping(readback.as_ptr(), map, readback.len()) };
            }
            let (dt, dc) = (t0.elapsed().as_secs_f64() * 1e3, cpu_ns() - c0);
            if i >= 30 {
                lat.push(dt);
                cpu += dc;
            }
            // The viewer checks some frames: the imported memory is the frame.
            if !shm && i % 50 == 0 {
                frame.read(&mut readback, W as usize).unwrap();
                let fd = unsafe { libc::dup(info.handle as RawFd) };
                let seen = viewer.read(fd, info.stride as u64 * H as u64).unwrap();
                assert_eq!(checksum(&seen, W as usize, H as usize, info.stride as usize / 4), checksum(&readback, W as usize, H as usize, W as usize), "frame {i}");
                verified += 1;
            }
        }
        eprintln!(
            "TIMING {name} {W}x{H} on {}: producer latency p50 {:.2} ms p95 {:.2} ms, producer CPU {:.2} ms/frame ({frames} frames)",
            gpu.caps.adapter,
            percentile(&mut lat.clone(), 0.5),
            percentile(&mut lat, 0.95),
            cpu as f64 / frames as f64 / 1e6
        );
    }
    eprintln!("viewer verified {verified} frames of the lent route");
    // SAFETY: undo the map and fd made above.
    unsafe {
        libc::munmap(map as *mut _, size as usize);
        libc::close(memfd);
    }
}
