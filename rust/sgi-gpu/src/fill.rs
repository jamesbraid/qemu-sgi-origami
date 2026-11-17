// SPDX-License-Identifier: BSD-3-Clause
//! Flat filled triangles and clears on a one-word-per-pixel plane, for boards
//! on the shared rasterizer's rules. Vertices and colour arrive already
//! snapped and stored, so the kernel is the same for every board.

use std::sync::OnceLock;

use iris_wgpu::wgpu::library::I64;
use iris_wgpu::wgpu::shader::Source;
use iris_wgpu::wgpu::{Fragment, Gpu};

use crate::plane::SgiGpuPlane;

const KERNEL: Fragment = Fragment::new("fill.wgsl", include_str!("shaders/fill.wgsl"));

/// Words before a job's records, and in each record (see fill.wgsl).
const HEADER: usize = 8;
const RECORD: usize = 16;
/// Triangles a dispatch loops over for each pixel.
const CHUNK: usize = 32;

#[repr(C)]
pub struct SgiGpuFillTri {
    pub x: [i32; 3],
    pub y: [i32; 3],
    pub colour: u32,
    pub write_mask: u32,
    pub clip_x0: i32,
    pub clip_y0: i32,
    pub clip_x1: i32,
    pub clip_y1: i32,
}

#[repr(C)]
pub struct SgiGpuFillClear {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
    pub value: u32,
    pub write_mask: u32,
}

struct Kernels {
    layout: wgpu::BindGroupLayout,
    tris: wgpu::ComputePipeline,
    clear: wgpu::ComputePipeline,
}

static KERNELS: OnceLock<Result<Kernels, String>> = OnceLock::new();

fn entry(binding: u32, read_only: bool) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only },
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    }
}

fn build(gpu: &Gpu) -> Result<Kernels, String> {
    let layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("fill"),
        entries: &[entry(0, true), entry(1, false)],
    });
    let src = Source::new("fill").library(I64).body(KERNEL);
    Ok(Kernels {
        tris: src.compute_pipeline(&gpu.device, "tris", &[&layout])?,
        clear: src.compute_pipeline(&gpu.device, "clear", &[&layout])?,
        layout,
    })
}

fn kernels(gpu: &Gpu) -> Result<&'static Kernels, String> {
    KERNELS.get_or_init(|| build(gpu)).as_ref().map_err(Clone::clone)
}

/// The words of a job over `bx` (x0, y0, x1, y1), already within the plane.
fn header(count: usize, plane: &SgiGpuPlane, sub: u32, bx: [i32; 4]) -> Vec<u32> {
    let mut w = vec![0u32; HEADER];
    w[0] = count as u32;
    w[1] = plane.width();
    w[2] = sub;
    for (i, v) in bx.iter().enumerate() {
        w[4 + i] = *v as u32;
    }
    w
}

/// Run a kernel over `bx` with the job `words`; the fence, or 0 on failure.
fn run(plane: &SgiGpuPlane, clear: bool, words: &[u32], bx: [i32; 4]) -> u64 {
    let dev = plane.device();
    let Some(gpu) = dev.gpu() else { return 0 };
    let k = match kernels(gpu) {
        Ok(k) => k,
        Err(e) => {
            eprintln!("sgi-gpu: fill kernel: {e}");
            return 0;
        }
    };
    let job = gpu.buffer("fill job", (words.len() * 4) as u64, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST);
    gpu.write(&job, 0, bytemuck::cast_slice(words));
    let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("fill"),
        layout: &k.layout,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: job.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: plane.buffer().as_entire_binding() },
        ],
    });
    let Some(mut enc) = dev.encoder("fill") else { return 0 };
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("fill"), timestamp_writes: None });
        pass.set_pipeline(if clear { &k.clear } else { &k.tris });
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(((bx[2] - bx[0] + 1) as u32).div_ceil(8), ((bx[3] - bx[1] + 1) as u32).div_ceil(8), 1);
    }
    dev.submit(enc).unwrap_or(0)
}

/// A fence for a call with nothing to draw, so the caller can tell it from failure.
fn nothing(plane: &SgiGpuPlane) -> u64 {
    let dev = plane.device();
    dev.encoder("fill nothing").and_then(|e| dev.submit(e)).unwrap_or(0)
}

/// `bx` within the plane, or None when nothing of it is left.
fn within(plane: &SgiGpuPlane, mut bx: [i32; 4]) -> Option<[i32; 4]> {
    bx[0] = bx[0].max(0);
    bx[1] = bx[1].max(0);
    bx[2] = bx[2].min(plane.width() as i32 - 1);
    bx[3] = bx[3].min(plane.height() as i32 - 1);
    (bx[0] <= bx[2] && bx[1] <= bx[3]).then_some(bx)
}

fn tris(plane: &SgiGpuPlane, sub: u32, list: &[SgiGpuFillTri]) -> u64 {
    let mut fence = 0;
    for chunk in list.chunks(CHUNK) {
        let mut bx = [i32::MAX, i32::MAX, i32::MIN, i32::MIN];
        let mut words = Vec::new();
        let mut used = 0;
        for t in chunk {
            let Some(c) = within(plane, [t.clip_x0, t.clip_y0, t.clip_x1, t.clip_y1]) else { continue };
            // The pixels the triangle can reach: its vertices' box and its clip.
            let lo = |a: &[i32; 3]| a.iter().min().copied().unwrap_or(0);
            let hi = |a: &[i32; 3]| a.iter().max().copied().unwrap_or(0);
            let reach = [
                c[0].max(lo(&t.x).div_euclid(1 << sub)),
                c[1].max(lo(&t.y).div_euclid(1 << sub)),
                c[2].min(hi(&t.x).div_euclid(1 << sub)),
                c[3].min(hi(&t.y).div_euclid(1 << sub)),
            ];
            if reach[0] > reach[2] || reach[1] > reach[3] {
                continue;
            }
            let mut rec = [0u32; RECORD];
            for i in 0..3 {
                rec[i] = t.x[i] as u32;
                rec[3 + i] = t.y[i] as u32;
            }
            rec[6] = t.colour;
            rec[7] = t.write_mask;
            for (i, v) in c.iter().enumerate() {
                rec[8 + i] = *v as u32;
            }
            bx = [bx[0].min(reach[0]), bx[1].min(reach[1]), bx[2].max(reach[2]), bx[3].max(reach[3])];
            words.extend_from_slice(&rec);
            used += 1;
        }
        if used == 0 {
            continue;
        }
        let mut job = header(used, plane, sub, bx);
        job.extend_from_slice(&words);
        fence = run(plane, false, &job, bx);
        if fence == 0 {
            return 0;
        }
    }
    if fence == 0 {
        nothing(plane)
    } else {
        fence
    }
}

/// Queue the triangles, vertices at 2^-`sub_bits` pixel, on `colour`; the
/// fence of the last, 0 on failure.
#[no_mangle]
pub extern "C" fn sgi_gpu_fill_tris(colour: *mut SgiGpuPlane, sub_bits: u32, tris_in: *const SgiGpuFillTri, count: usize) -> u64 {
    // SAFETY: the caller passes a live plane and `count` triangles.
    let Some(plane) = (unsafe { colour.as_ref() }) else { return 0 };
    if count == 0 || tris_in.is_null() || !(1..=8).contains(&sub_bits) {
        return 0;
    }
    // SAFETY: as above.
    let list = unsafe { std::slice::from_raw_parts(tris_in, count) };
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| tris(plane, sub_bits, list))).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn sgi_gpu_fill_clear(plane: *mut SgiGpuPlane, clear: *const SgiGpuFillClear) -> u64 {
    // SAFETY: the caller passes a live plane and a clear.
    let (Some(plane), Some(c)) = (unsafe { plane.as_ref() }, unsafe { clear.as_ref() }) else { return 0 };
    let Some(bx) = within(plane, [c.x0, c.y0, c.x1, c.y1]) else {
        return std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| nothing(plane))).unwrap_or(0);
    };
    let mut job = header(1, plane, 0, bx);
    job.extend_from_slice(&[c.value, c.write_mask]);
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run(plane, true, &job, bx))).unwrap_or(0)
}
