// SPDX-License-Identifier: BSD-3-Clause
// Ported from src/wgpu/tests/mod.rs in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
//! End-to-end tests of the common layer on a test board: flat triangles
//! into a u32 plane, checked word for word against a CPU rasteriser with
//! the same arithmetic. They need a GPU (or lavapipe) and skip without
//! one.

use std::sync::Arc;

use bytemuck::{Pod, Zeroable};

use super::batch::{ArchShaders, Batch, BatchPipelines, Full};
use super::device::{Gpu, GpuOptions};
use super::memory::GpuMemory;
use super::shader::Fragment;
use super::tiles::{TileGrid, MAX_PRIMS};

mod softfloat;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Pod, Zeroable)]
struct Rec {
    x: [i32; 3],
    y: [i32; 3],
    colour: u32,
    slot: u32,
}

// WGSL lays the record out as x0,y0,x1,y1,x2,y2: keep the Rust side in
// the same order.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Pod, Zeroable)]
struct RecWgsl {
    v: [i32; 6],
    colour: u32,
    slot: u32,
}

impl Rec {
    fn wgsl(&self) -> RecWgsl {
        RecWgsl {
            v: [self.x[0], self.y[0], self.x[1], self.y[1], self.x[2], self.y[2]],
            colour: self.colour,
            slot: self.slot,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Pod, Zeroable)]
struct Slot {
    mask: u32,
    pad: [u32; 3],
}

const FLAT: Fragment = Fragment::new("flat.wgsl", include_str!("flat.wgsl"));

struct Board {
    gpu: Arc<Gpu>,
    pipelines: BatchPipelines,
    batch: Batch<RecWgsl, Slot>,
    vram: GpuMemory,
    vram_group: wgpu::BindGroup,
    width: u32,
    height: u32,
    stride: u32,
}

fn gpu() -> Option<Arc<Gpu>> {
    match Gpu::new(&GpuOptions::default(), Box::new(|_| {})) {
        Ok(g) => Some(g),
        Err(e) => {
            if std::env::var_os("IRIS_REQUIRE_GPU").is_some_and(|v| v == "1") { panic!("no GPU: {e}") }
            eprintln!("skipped: {e}");
            None
        }
    }
}

impl Board {
    fn new(gpu: Arc<Gpu>, width: u32, height: u32) -> Board {
        let stride = (width + 15) & !15;
        let layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("flat vram"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let pipelines = BatchPipelines::new(
            &gpu,
            &ArchShaders {
                label: "flat",
                raster: &[FLAT],
                bin_accept: None,
                library: &[],
                defines: &[("WIDTH", width), ("HEIGHT", height), ("STRIDE", stride)],
                layouts: &[&layout],
            },
        )
        .unwrap_or_else(|e| panic!("{e}"));
        let vram = GpuMemory::new(&gpu, "flat vram", stride as u64 * height as u64 * 4).unwrap();
        let vram_group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("flat vram"),
            layout: &layout,
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: vram.buffer.as_entire_binding() }],
        });
        let batch = Batch::new(&gpu, &pipelines, TileGrid::new(width, height), 64);
        Board { gpu, pipelines, batch, vram, vram_group, width, height, stride }
    }

    fn flush(&mut self) {
        self.batch.flush(&self.pipelines, &[&self.vram_group]);
    }

    fn plane(&mut self) -> Vec<u32> {
        let bytes = self.vram.read_all().unwrap();
        bytemuck::cast_slice(&bytes).to_vec()
    }
}

/// The CPU side of flat.wgsl.
struct Reference {
    plane: Vec<u32>,
    width: u32,
    height: u32,
    stride: u32,
}

fn edge(ax: i32, ay: i32, bx: i32, by: i32, px: i32, py: i32) -> i32 {
    (bx - ax) * (py - ay) - (by - ay) * (px - ax)
}

fn inside(e: i32, dx: i32, dy: i32) -> bool {
    e > 0 || (e == 0 && ((dy == 0 && dx > 0) || dy < 0))
}

impl Reference {
    fn draw(&mut self, r: &Rec, mask: u32) {
        let (x, y) = (r.x, r.y);
        let x0 = (x.iter().copied().min().unwrap() >> 4).max(0);
        let x1 = ((x.iter().copied().max().unwrap() + 15) >> 4).min(self.width as i32 - 1);
        let y0 = (y.iter().copied().min().unwrap() >> 4).max(0);
        let y1 = ((y.iter().copied().max().unwrap() + 15) >> 4).min(self.height as i32 - 1);
        for py in y0..=y1 {
            for px in x0..=x1 {
                let (sx, sy) = (px * 16 + 8, py * 16 + 8);
                let e0 = edge(x[1], y[1], x[2], y[2], sx, sy);
                let e1 = edge(x[2], y[2], x[0], y[0], sx, sy);
                let e2 = edge(x[0], y[0], x[1], y[1], sx, sy);
                if inside(e0, x[2] - x[1], y[2] - y[1])
                    && inside(e1, x[0] - x[2], y[0] - y[2])
                    && inside(e2, x[1] - x[0], y[1] - y[0])
                {
                    let i = (py as u32 * self.stride + px as u32) as usize;
                    self.plane[i] = (self.plane[i] & !mask) | (r.colour & mask);
                }
            }
        }
    }
}

/// A deterministic generator.
struct Lcg(u64);

impl Lcg {
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as u32
    }

    fn range(&mut self, lo: i32, hi: i32) -> i32 {
        lo + (self.next() % (hi - lo) as u32) as i32
    }
}

/// A triangle with positive orientation, vertices in 1/16 pixel around
/// the target (some outside it), of a size drawn from `max`.
fn triangle(rng: &mut Lcg, width: u32, height: u32, max: i32) -> Rec {
    let cx = rng.range(-32, width as i32 + 32) * 16;
    let cy = rng.range(-32, height as i32 + 32) * 16;
    let mut r = Rec::default();
    for i in 0..3 {
        r.x[i] = cx + rng.range(-max, max);
        r.y[i] = cy + rng.range(-max, max);
    }
    if edge(r.x[0], r.y[0], r.x[1], r.y[1], r.x[2], r.y[2]) < 0 {
        r.x.swap(1, 2);
        r.y.swap(1, 2);
    }
    r.colour = rng.next();
    r
}

fn bbox(r: &Rec) -> (i32, i32, i32, i32) {
    (
        r.x.iter().copied().min().unwrap() >> 4,
        r.y.iter().copied().min().unwrap() >> 4,
        (r.x.iter().copied().max().unwrap() + 15) >> 4,
        (r.y.iter().copied().max().unwrap() + 15) >> 4,
    )
}

fn compare(gpu_plane: &[u32], reference: &Reference, what: &str) {
    let mut bad = 0;
    for y in 0..reference.height {
        for x in 0..reference.width {
            let i = (y * reference.stride + x) as usize;
            if gpu_plane[i] != reference.plane[i] {
                if bad < 8 {
                    eprintln!("{what}: ({x},{y}) gpu {:08x} cpu {:08x}", gpu_plane[i], reference.plane[i]);
                }
                bad += 1;
            }
        }
    }
    assert_eq!(bad, 0, "{what}: {bad} pixels differ");
}

fn run(width: u32, height: u32, batches: usize, per_batch: usize, max: i32, seed: u64) {
    let Some(gpu) = gpu() else { return };
    let mut board = Board::new(gpu, width, height);
    let mut reference = Reference { plane: vec![0; (board.stride * height) as usize], width, height, stride: board.stride };
    let mut rng = Lcg(seed);
    let masks = [0xffff_ffff, 0x00ff_ffff, 0xff00_ff00, 0x0000_00ff];
    for b in 0..batches {
        let mut slots = Vec::new();
        for m in masks {
            slots.push(board.batch.slot(Slot { mask: m, pad: [0; 3] }).unwrap());
        }
        for _ in 0..per_batch {
            let mut r = triangle(&mut rng, width, height, max);
            let s = (rng.next() % 4) as usize;
            r.slot = slots[s];
            let (x0, y0, x1, y1) = bbox(&r);
            match board.batch.prim(r.wgsl(), x0, y0, x1, y1) {
                Ok(_) => reference.draw(&r, masks[s]),
                Err(Full) => panic!("batch full at {per_batch}"),
            }
        }
        board.flush();
        if b % 4 == 3 {
            let plane = board.plane();
            compare(&plane, &reference, &format!("after batch {b}"));
        }
    }
    let plane = board.plane();
    compare(&plane, &reference, "final");
    board.gpu.check().unwrap();
}

#[test]
fn small_triangles_match_the_reference() {
    run(320, 200, 8, 500, 16 * 12, 1);
}

#[test]
fn large_overlapping_triangles_keep_order() {
    run(640, 480, 6, 200, 16 * 400, 2);
}

#[test]
fn odd_sized_target_and_offscreen_boxes() {
    run(333, 211, 4, 300, 16 * 60, 3);
}

#[test]
fn full_batches_flush_and_continue() {
    let Some(gpu) = gpu() else { return };
    let (width, height) = (256u32, 256u32);
    let mut board = Board::new(gpu, width, height);
    let mut reference = Reference { plane: vec![0; (board.stride * height) as usize], width, height, stride: board.stride };
    let mut rng = Lcg(7);
    let mut pushed = 0u32;
    let mut flushes = 0;
    while pushed < MAX_PRIMS * 2 + 17 {
        let slot = match board.batch.slot(Slot { mask: !0, pad: [0; 3] }) {
            Ok(s) => s,
            Err(Full) => {
                board.flush();
                flushes += 1;
                continue;
            }
        };
        let mut r = triangle(&mut rng, width, height, 16 * 20);
        r.slot = slot;
        let (x0, y0, x1, y1) = bbox(&r);
        match board.batch.prim(r.wgsl(), x0, y0, x1, y1) {
            Ok(_) => {
                reference.draw(&r, !0);
                pushed += 1;
            }
            Err(Full) => {
                board.flush();
                flushes += 1;
            }
        }
    }
    board.flush();
    assert!(flushes >= 2, "expected the recorder to fill up, flushed {flushes} times");
    let plane = board.plane();
    compare(&plane, &reference, "after refills");
}

#[test]
#[ignore = "requires a conformant host GPU"]
fn bounded_transactional_batches_keep_order() {
    let options = GpuOptions {
        backend: std::env::var("WGPU_TEST_BACKEND").unwrap_or_else(|_| "auto".into()),
        ..GpuOptions::default()
    };
    let gpu = Gpu::new(&options, Box::new(|_| {})).expect("initialize GPU for bounded batches");
    let mut board = Board::new(Arc::clone(&gpu), 32, 32);
    board.batch = Batch::new_bounded(
        &gpu,
        &board.pipelines,
        TileGrid::new(32, 32),
        1,
        std::num::NonZeroUsize::new(1).unwrap(),
    );
    let mut reference = Reference {
        plane: vec![0; (board.stride * board.height) as usize],
        width: board.width,
        height: board.height,
        stride: board.stride,
    };
    let mut builds = 0;
    for draw in 0..32u32 {
        let r = Rec {
            x: [0, 31 * 16, 0],
            y: [0, 0, 31 * 16],
            colour: draw.wrapping_mul(0x1020_4081),
            slot: 0,
        };
        let mask = if draw % 2 == 0 { 0xFF00_00FF } else { 0x00FF_FF00 };
        let (x0, y0, x1, y1) = bbox(&r);
        loop {
            let result = board.batch.prim_with_slot(
                Slot { mask, pad: [0; 3] },
                x0, y0, x1, y1,
                |slot| {
                    builds += 1;
                    let mut record = r.wgsl();
                    record.slot = slot;
                    record
                },
            );
            match result {
                Ok(Some(_)) => break,
                Ok(None) => panic!("test primitive unexpectedly offscreen"),
                Err(Full) => {
                    assert_eq!((board.batch.len(), board.batch.slots_used()), (1, 1));
                    assert!(board.batch.try_flush(&board.pipelines, &[&board.vram_group])
                        .expect("submit bounded batch").is_some());
                    assert_eq!(board.batch.sets(), 1);
                }
            }
        }
        reference.draw(&r, mask);
    }
    assert!(board.batch.try_flush(&board.pipelines, &[&board.vram_group])
        .expect("submit final bounded batch").is_some());
    assert_eq!(board.batch.sets(), 1);
    assert_eq!(board.batch.flushes, 32);
    assert_eq!(builds, 32, "a full retry must not rebuild a record");
    let plane = board.plane();
    compare(&plane, &reference, "bounded transactional batches");
    board.gpu.check().unwrap();
}

#[test]
fn empty_flush_is_a_no_op() {
    let Some(gpu) = gpu() else { return };
    let mut board = Board::new(gpu, 64, 64);
    assert!(board.batch.flush(&board.pipelines, &[&board.vram_group]).is_none());
    assert!(board.plane().iter().all(|&w| w == 0));
}

/// A pipeline that fails to build makes the submission that uses it fail,
/// and wgpu cannot wait for a failed submission. The wait reports the build
/// error, not the missing submission.
#[test]
fn a_refused_submission_reports_why() {
    let Some(gpu) = gpu() else { return };
    let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("refused"),
        source: wgpu::ShaderSource::Wgsl("@compute @workgroup_size(1) fn main() { let x: u32 = 1.5; }".into()),
    });
    let pipeline = gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("refused"),
        layout: None,
        module: &module,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });
    let mut encoder = gpu.encoder("refused");
    {
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor::default());
        pass.set_pipeline(&pipeline);
        pass.dispatch_workgroups(1, 1, 1);
    }
    let index = gpu.submit(encoder);
    let err = gpu.wait(Some(index)).unwrap_err();
    assert!(err.starts_with("wgpu error"), "{err}");
}

/// A device whose fatal callback counts its calls.
fn counted() -> Option<(Arc<Gpu>, Arc<std::sync::atomic::AtomicUsize>)> {
    let calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let seen = Arc::clone(&calls);
    match Gpu::new(&GpuOptions::default(), Box::new(move |_| { seen.fetch_add(1, std::sync::atomic::Ordering::SeqCst); })) {
        Ok(g) => Some((g, calls)),
        Err(e) => {
            if std::env::var_os("IRIS_REQUIRE_GPU").is_some_and(|v| v == "1") { panic!("no GPU: {e}") }
            eprintln!("skipped: {e}");
            None
        }
    }
}

fn storage(gpu: &Gpu, label: &str) -> wgpu::Buffer {
    gpu.buffer(label, 64, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC)
}

/// Two boards on one device and queue, each with an upload ring of its own:
/// a board's writes reach the GPU ahead of its own next submission, in the
/// order it made them, whichever board submits first.
#[test]
fn boards_on_one_shared_device_keep_their_own_order() {
    let Some((a, _)) = counted() else { return };
    let b = a.share();
    assert!(a.device == b.device, "one device");
    let (ba, bb) = (storage(&a, "a"), storage(&b, "b"));
    // B writes twice to the same words, A once; B submits first, so A's
    // writes ride its own submission and B's later write wins on B's buffer.
    b.write(&bb, 0, &[1u8; 16]);
    a.write(&ba, 0, &[7u8; 16]);
    b.write(&bb, 0, &[2u8; 16]);
    b.submit(b.encoder("b"));
    a.submit(a.encoder("a"));
    assert_eq!(a.read_vec(&ba, &[(0, 16)]).unwrap(), vec![7u8; 16]);
    assert_eq!(b.read_vec(&bb, &[(0, 16)]).unwrap(), vec![2u8; 16]);
}

/// Readbacks borrow one staging buffer between them: ranges come back in the
/// order asked, a mis-sized `out` is refused, a read larger than a staging
/// buffer goes through in pieces, and two boards on one device reuse the
/// buffer the other made.
#[test]
fn reads_are_ranged_counted_and_share_one_staging_buffer() {
    let Some((a, _)) = counted() else { return };
    let b = a.share();
    let src = a.buffer("src", 64, wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC);
    let bytes: Vec<u8> = (0..64).collect();
    a.write(&src, 0, &bytes);
    let got = a.read_vec(&src, &[(48, 8), (4, 4)]).unwrap();
    assert_eq!(got, [&bytes[48..56], &bytes[4..8]].concat());
    assert!(a.read(&src, &[(0, 8)], &mut [0u8; 4]).is_err(), "out is the ranges' length");
    // The other board's read lends the same buffer.
    assert_eq!(b.read_vec(&src, &[(0, 4)]).unwrap(), &bytes[..4]);

    // Bigger than one staging buffer: 20 MiB in two pieces.
    let big_len = 20u64 << 20;
    let big = a.buffer("big", big_len, wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC);
    let pattern: Vec<u8> = (0..big_len as usize).map(|i| (i % 251) as u8).collect();
    a.write(&big, 0, &pattern);
    let mut out = vec![0u8; big_len as usize];
    a.read(&big, &[(0, big_len)], &mut out).unwrap();
    assert!(out == pattern, "a read in pieces is the whole buffer");
}

/// The presenter's unpack kernel reads a scan-out frame's 0xAABBGGRR words
/// as the texture's R, G, B and A.
#[cfg(feature = "present")]
#[test]
fn the_unpack_kernel_reads_abgr_words() {
    let Some(gpu) = gpu() else { return };
    let entry = |binding, ty| wgpu::BindGroupLayoutEntry { binding, visibility: wgpu::ShaderStages::COMPUTE, ty, count: None };
    let layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("unpack test"),
        entries: &[
            entry(0, wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only: true }, has_dynamic_offset: false, min_binding_size: None }),
            entry(1, wgpu::BindingType::StorageTexture { access: wgpu::StorageTextureAccess::WriteOnly, format: wgpu::TextureFormat::Rgba8Unorm, view_dimension: wgpu::TextureViewDimension::D2 }),
            entry(2, wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }),
        ],
    });
    let pipeline = super::shader::Source::new("unpack test").body(super::present::UNPACK).compute_pipeline(&gpu.device, "unpack", &[&layout]).unwrap();
    // 64 pixels a row make the texture copy's 256-byte row pitch.
    let words: Vec<u32> = (0..64u32 * 2).map(|i| 0xff00_0000 | (i * 3) << 16 | (i * 5 & 0xff) << 8 | (i * 7 & 0xff)).collect();
    let src = gpu.buffer("unpack src", words.len() as u64 * 4, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST);
    gpu.write(&src, 0, bytemuck::cast_slice(&words));
    let size = gpu.buffer("unpack size", 16, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    gpu.write(&size, 0, bytemuck::cast_slice(&[64u32, 2, 0, 0]));
    let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some("unpack out"),
        size: wgpu::Extent3d { width: 64, height: 2, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = texture.create_view(&Default::default());
    let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("unpack test"),
        layout: &layout,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: src.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&view) },
            wgpu::BindGroupEntry { binding: 2, resource: size.as_entire_binding() },
        ],
    });
    let out = gpu.buffer("unpack readback", 64 * 2 * 4, wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC);
    let mut enc = gpu.encoder("unpack test");
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("unpack test"), timestamp_writes: None });
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(8, 1, 1);
    }
    enc.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo { texture: &texture, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::TexelCopyBufferInfo { buffer: &out, layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(256), rows_per_image: Some(2) } },
        wgpu::Extent3d { width: 64, height: 2, depth_or_array_layers: 1 },
    );
    gpu.submit(enc);
    let got = gpu.read_vec(&out, &[(0, 64 * 2 * 4)]).unwrap();
    // 0xAABBGGRR in memory is the bytes R, G, B, A.
    assert!(got == bytemuck::cast_slice::<u32, u8>(&words), "texture bytes are the words' bytes");
}

/// A loss is every board's, once: the callback runs one time for two boards,
/// and each board's calls report the same error from then on.
#[test]
fn device_loss_is_sticky_and_fatal_for_every_board() {
    let Some((a, calls)) = counted() else { return };
    let b = a.share();
    let buffer = storage(&b, "b");
    assert!(a.check().is_ok() && b.check().is_ok());
    a.device.destroy();
    // The loss callback runs from a poll.
    let first = a.wait(None).unwrap_err();
    assert!(first.contains("lost"), "{first}");
    assert_eq!(b.check().unwrap_err(), first, "the other board sees the same error");
    assert_eq!(b.wait(None).unwrap_err(), first, "and it stays");
    // A write on a lost device is dropped, not a panic, and reports nothing
    // new: the error is the one already recorded.
    b.write(&buffer, 0, &[0u8; 16]);
    b.submit(b.encoder("after loss"));
    assert_eq!(a.share().check().unwrap_err(), first, "a board opened later shares it");
    assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 1);
}

/// A failure the device did not report itself is the same kind of failure:
/// `fail` records it, runs the callback once and later errors do not
/// replace it.
#[test]
fn a_refused_readback_is_fatal_once_and_not_replaced() {
    let Some((a, calls)) = counted() else { return };
    let b = a.share();
    b.fail("readback refused");
    a.fail("a second failure");
    assert_eq!(a.check().unwrap_err(), "readback refused");
    assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst), 1);
    // The boards' calls now report it rather than carry on.
    assert_eq!(a.wait(None).unwrap_err(), "readback refused");
}
