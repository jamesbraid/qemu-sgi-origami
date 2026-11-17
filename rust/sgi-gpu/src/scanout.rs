// SPDX-License-Identifier: BSD-3-Clause
//! VPro scan-out on the GPU: `odyssey_resolve_row()` for a whole frame into a `SgiGpuFrame`.
//! A worker thread uploads the table snapshot and changed id rows, submits and waits; `done` runs there.

use std::ffi::c_void;
use std::sync::mpsc::{channel, Sender};
use std::sync::Arc;
use std::thread::JoinHandle;

use iris_wgpu::wgpu::Gpu;

use crate::device::{from_c, SgiGpu};
use crate::frame::SgiGpuFrame;
use crate::plane::SgiGpuPlane;

const LUT_WORDS: usize = 0x2000;
const DISPMODES: usize = 32;
const OVERLAY_MODES: usize = 8;
const CURSOR_WORDS: usize = 0x100;
/// Words ahead of the lookup RAM in the table buffer (shaders/scanout_vpro.wgsl).
const HEAD_WORDS: usize = 16;
const TABLE_WORDS: usize = HEAD_WORDS + LUT_WORDS + DISPMODES + OVERLAY_MODES + CURSOR_WORDS;

const SHADER: &str = include_str!("shaders/scanout_vpro.wgsl");

/// The board state one compose reads, as the C side snapshots it
/// (`SgiGpuVproTables` in include/hw/display/sgi-gpu-scanout.h).
#[repr(C)]
#[derive(Clone)]
pub struct SgiGpuVproTables {
    pub width: u32,
    pub height: u32,
    pub displayed_buffer: u32,
    pub lut: [u32; LUT_WORDS],
    pub dispmode: [u32; DISPMODES],
    pub overlay_mode: [u32; OVERLAY_MODES],
    pub cursor: [u32; CURSOR_WORDS],
}

/// Rows of the id and overlay planes the CPU changed since the last compose.
pub struct IdRows {
    pub y: u32,
    pub did: Vec<u8>,
    pub overlay: Vec<u8>,
}

/// The GPU side of one board's scan-out: pipeline, tables, id planes.
pub struct VproCompose {
    gpu: Arc<Gpu>,
    layout: wgpu::BindGroupLayout,
    pipeline: wgpu::ComputePipeline,
    tables: wgpu::Buffer,
    ids: wgpu::Buffer,
    overlay: wgpu::Buffer,
    /// Pixels a row of the planes holds, and of the id planes in bytes.
    plane_width: u32,
    plane_height: u32,
}

fn storage(binding: u32, read_only: bool) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only }, has_dynamic_offset: false, min_binding_size: None },
        count: None,
    }
}

impl VproCompose {
    /// For planes of `plane_width` x `plane_height` words.
    pub fn new(gpu: &Arc<Gpu>, plane_width: u32, plane_height: u32) -> Result<VproCompose, String> {
        if plane_width == 0 || plane_height == 0 || plane_width % 4 != 0 {
            return Err(format!("scanout: bad plane size {plane_width} x {plane_height}"));
        }
        let layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("vpro scanout"),
            entries: &[storage(0, true), storage(1, false), storage(2, true), storage(3, true), storage(4, true), storage(5, true)],
        });
        let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("vpro scanout"), source: wgpu::ShaderSource::Wgsl(SHADER.into()) });
        let playout = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor { label: Some("vpro scanout"), bind_group_layouts: &[Some(&layout)], immediate_size: 0 });
        let pipeline = gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("vpro scanout"),
            layout: Some(&playout),
            module: &module,
            entry_point: Some("scanout"),
            compilation_options: Default::default(),
            cache: None,
        });
        let usage = wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST;
        let id_bytes = plane_width as u64 * plane_height as u64;
        Ok(VproCompose {
            gpu: Arc::clone(gpu),
            layout,
            pipeline,
            tables: gpu.buffer("vpro scanout tables", TABLE_WORDS as u64 * 4, usage),
            ids: gpu.buffer("vpro scanout ids", id_bytes.next_multiple_of(4), usage),
            overlay: gpu.buffer("vpro scanout overlay", id_bytes.next_multiple_of(4), usage),
            plane_width,
            plane_height,
        })
    }

    /// Upload the id rows, then the tables, then compose into `frame` and
    /// submit. Returns the submission to wait for.
    pub fn compose(&self, t: &SgiGpuVproTables, rows: &[IdRows], buf0: &wgpu::Buffer, buf1: &wgpu::Buffer, frame: &SgiGpuFrame) -> Result<wgpu::SubmissionIndex, String> {
        let (w, h) = (t.width.min(self.plane_width).min(frame.width()), t.height.min(self.plane_height).min(frame.height()));
        for r in rows {
            let len = self.plane_width as usize;
            if r.y >= self.plane_height || r.did.len() % len != 0 || r.did.len() != r.overlay.len() || r.y as usize + r.did.len() / len > self.plane_height as usize {
                return Err("scanout: id rows out of range".into());
            }
            let at = r.y as u64 * len as u64;
            self.gpu.write(&self.ids, at, &r.did);
            self.gpu.write(&self.overlay, at, &r.overlay);
        }
        let mut words = vec![0u32; TABLE_WORDS];
        words[..6].copy_from_slice(&[w, h, frame.stride_words(), t.displayed_buffer, self.plane_width, self.plane_width]);
        let mut at = HEAD_WORDS;
        for part in [&t.lut[..], &t.dispmode[..], &t.overlay_mode[..], &t.cursor[..]] {
            words[at..at + part.len()].copy_from_slice(part);
            at += part.len();
        }
        self.gpu.write(&self.tables, 0, bytemuck::cast_slice(&words));
        let group = self.gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("vpro scanout"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: self.tables.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: frame.buffer().as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: buf0.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 3, resource: buf1.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 4, resource: self.ids.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 5, resource: self.overlay.as_entire_binding() },
            ],
        });
        let mut enc = self.gpu.encoder("vpro scanout");
        {
            let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("vpro scanout"), timestamp_writes: None });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &group, &[]);
            pass.dispatch_workgroups(w.div_ceil(8), h.div_ceil(8), 1);
        }
        let index = self.gpu.submit(enc);
        self.gpu.check()?;
        Ok(index)
    }

    /// Compose and wait for the GPU: the host-fence mode.
    pub fn compose_wait(&self, t: &SgiGpuVproTables, rows: &[IdRows], buf0: &wgpu::Buffer, buf1: &wgpu::Buffer, frame: &SgiGpuFrame) -> Result<(), String> {
        let index = self.compose(t, rows, buf0, buf1, frame)?;
        self.gpu.wait(Some(index))
    }
}

// ---- the worker and the C interface (include/hw/display/sgi-gpu-scanout.h) ----

struct Raw<T>(*mut T);
// SAFETY: the C side keeps each pointee alive until the job's `done` ran or
// `sgi_gpu_vpro_scanout_quiesce` returned.
unsafe impl<T> Send for Raw<T> {}

enum Job {
    Compose { tables: Box<SgiGpuVproTables>, rows: Vec<IdRows>, planes: [Raw<SgiGpuPlane>; 2], frame: Raw<SgiGpuFrame>, done: DoneFn, opaque: Raw<c_void> },
    Quiesce(Sender<()>),
}

type DoneFn = unsafe extern "C" fn(opaque: *mut c_void, ok: bool);

pub struct SgiGpuVproScanout {
    compose: Arc<VproCompose>,
    jobs: Option<Sender<Job>>,
    worker: Option<JoinHandle<()>>,
}

fn run(compose: Arc<VproCompose>, jobs: std::sync::mpsc::Receiver<Job>) {
    for job in jobs {
        match job {
            Job::Compose { tables, rows, planes, frame, done, opaque } => {
                // SAFETY: see Raw.
                let ok = unsafe {
                    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        let (b0, b1) = ((*planes[0].0).buffer(), (*planes[1].0).buffer());
                        compose.compose_wait(&tables, &rows, b0, b1, &*frame.0)
                    }));
                    match r {
                        Ok(Ok(())) => true,
                        Ok(Err(e)) => {
                            eprintln!("sgi-gpu: scanout compose: {e}");
                            false
                        }
                        Err(_) => {
                            eprintln!("sgi-gpu: scanout compose panicked");
                            false
                        }
                    }
                };
                // SAFETY: the C callback is valid for `opaque`.
                unsafe { done(opaque.0, ok) };
            }
            Job::Quiesce(ack) => {
                let _ = ack.send(());
            }
        }
    }
}

/// # Safety
/// `gpu` is a live `SgiGpu`.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_vpro_scanout_new(gpu: *mut SgiGpu, width: u32, height: u32) -> *mut SgiGpuVproScanout {
    let Some(gpu) = from_c(gpu).and_then(|g| g.gpu()) else { return std::ptr::null_mut() };
    match VproCompose::new(gpu, width, height) {
        Ok(c) => {
            let compose = Arc::new(c);
            let (tx, rx) = channel();
            let c2 = Arc::clone(&compose);
            let worker = std::thread::Builder::new().name("sgi-gpu scanout".into()).spawn(move || run(c2, rx));
            match worker {
                Ok(worker) => Box::into_raw(Box::new(SgiGpuVproScanout { compose, jobs: Some(tx), worker: Some(worker) })),
                Err(e) => {
                    eprintln!("sgi-gpu: scanout worker: {e}");
                    std::ptr::null_mut()
                }
            }
        }
        Err(e) => {
            eprintln!("sgi-gpu: {e}");
            std::ptr::null_mut()
        }
    }
}

/// Waits for every queued compose.
///
/// # Safety
/// `sc` is from `sgi_gpu_vpro_scanout_new`.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_vpro_scanout_free(sc: *mut SgiGpuVproScanout) {
    if sc.is_null() {
        return;
    }
    let mut sc = Box::from_raw(sc);
    sc.jobs = None;
    if let Some(w) = sc.worker.take() {
        let _ = w.join();
    }
}

/// Returns when every compose queued so far has called its `done`.
///
/// # Safety
/// `sc` is live.
#[no_mangle]
pub unsafe extern "C" fn sgi_gpu_vpro_scanout_quiesce(sc: *mut SgiGpuVproScanout) {
    let Some(sc) = sc.as_ref() else { return };
    let (ack, wait) = channel();
    if sc.jobs.as_ref().is_some_and(|j| j.send(Job::Quiesce(ack)).is_ok()) {
        let _ = wait.recv();
    }
}

/// Queue a compose of the planes into `frame`, copying `tables` and the id
/// rows now; false if nothing was queued. `done` runs on the worker thread.
///
/// # Safety
/// All pointers are live; the planes and the frame stay until `done` ran.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn sgi_gpu_vpro_scanout_compose(
    sc: *mut SgiGpuVproScanout,
    tables: *const SgiGpuVproTables,
    row_y: u32,
    row_count: u32,
    did: *const u8,
    overlay: *const u8,
    buf0: *mut SgiGpuPlane,
    buf1: *mut SgiGpuPlane,
    frame: *mut SgiGpuFrame,
    done: DoneFn,
    opaque: *mut c_void,
) -> bool {
    let (Some(sc), Some(tables)) = (sc.as_ref(), tables.as_ref()) else { return false };
    if buf0.is_null() || buf1.is_null() || frame.is_null() {
        return false;
    }
    let mut rows = Vec::new();
    if row_count > 0 {
        let len = row_count as usize * sc.compose.plane_width as usize;
        if did.is_null() || overlay.is_null() {
            return false;
        }
        rows.push(IdRows { y: row_y, did: std::slice::from_raw_parts(did, len).to_vec(), overlay: std::slice::from_raw_parts(overlay, len).to_vec() });
    }
    let job = Job::Compose { tables: Box::new(tables.clone()), rows, planes: [Raw(buf0), Raw(buf1)], frame: Raw(frame), done, opaque: Raw(opaque) };
    sc.jobs.as_ref().is_some_and(|j| j.send(job).is_ok())
}

#[cfg(test)]
mod tests {
    use super::*;
    use iris_wgpu::wgpu::GpuOptions;

    const W: u32 = 160;
    const H: u32 = 96;

    struct Rng(u64);

    impl Rng {
        fn next(&mut self) -> u32 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            (self.0 >> 16) as u32
        }

        fn below(&mut self, n: u32) -> u32 {
            self.next() % n
        }
    }

    /// odyssey_resolve_row() and odyssey_cursor_row() in hw/misc/sgi-odyssey.c,
    /// written out again for one row.
    fn reference_row(t: &SgiGpuVproTables, did: &[u8], ovl: &[u8], b: [&[u32]; 2], y: usize, out: &mut [u32]) {
        let rgb = |w: u32| ((w >> 22) & 0xff) << 16 | ((w >> 12) & 0xff) << 8 | ((w >> 2) & 0xff);
        let lut = |e: usize| if e < LUT_WORDS { rgb(t.lut[e]) } else { 0 };
        let gamma = |c: u32, shift: u32| {
            let v = (c << 4) | (c >> 4);
            let i = if v < 0x200 { v } else if v < 0x400 { v / 2 + 0x100 } else { v / 4 + 0x200 };
            (t.lut[0x1a00 + i as usize] >> shift) & 0xff
        };
        let chan = |mode: u32, c: u32| match (mode >> 9) & 7 {
            4 => (c & 0xf0) | (c >> 4),
            3 => (c & 0xf8) | (c >> 5),
            _ => c,
        };
        let width = t.width.min(W) as usize;
        for x in 0..width {
            let at = y * W as usize + x;
            let main_id = ((did[at] >> 3) & 31) as usize;
            let word = b[((t.displayed_buffer >> main_id) & 1) as usize][at];
            let group = (t.dispmode[main_id] >> 4) & 31;
            let omode = t.overlay_mode[(did[at] & 7) as usize];
            out[x] = if omode & 1 != 0 && ovl[at] != 0 {
                lut(256 * ((omode >> 3) & 31) as usize + ovl[at] as usize)
            } else if group >= 26 {
                let mode = t.dispmode[main_id];
                gamma(chan(mode, word >> 24), 22) << 16 | gamma(chan(mode, (word >> 16) & 0xff), 12) << 8 | gamma(chan(mode, (word >> 8) & 0xff), 2)
            } else {
                lut(if group != 0 { 256 * group as usize + (word & 0xff) as usize } else { (word & 0xfff) as usize })
            };
        }
        let ctl = t.cursor[0x81];
        if ctl & 1 == 0 || ctl & 0x20 != 0 {
            return;
        }
        let (cx, cy) = ((t.cursor[0x80] >> 16) as u16 as i16 as i32, t.cursor[0x80] as u16 as i16 as i32);
        let row = y as i32 - cy;
        if ctl & 4 != 0 {
            let colour = rgb(t.cursor[0x91]);
            if row == 0 {
                out[..width].fill(colour);
            } else if cx >= 0 && cx < width as i32 {
                out[cx as usize] = colour;
            }
            return;
        }
        if !(0..32).contains(&row) {
            return;
        }
        for col in 0..32usize {
            let x = cx + col as i32;
            let plane = (t.cursor[0xc0 + row as usize * 2 + col / 16] >> ((col % 16) * 2)) & 3;
            let glyph = (t.cursor[row as usize * 4 + col / 8] >> ((col % 8) * 4)) & 0xf;
            if x < 0 || x >= width as i32 || plane != 3 || glyph == 0xf {
                continue;
            }
            out[x as usize] = rgb(t.cursor[0x90 + glyph as usize]);
        }
    }

    fn random_tables(r: &mut Rng, round: u32) -> SgiGpuVproTables {
        let mut t = SgiGpuVproTables { width: W, height: H, displayed_buffer: r.next(), lut: [0; LUT_WORDS], dispmode: [0; DISPMODES], overlay_mode: [0; OVERLAY_MODES], cursor: [0; CURSOR_WORDS] };
        t.lut.iter_mut().for_each(|w| *w = r.next() & 0x3fff_ffff);
        for m in &mut t.dispmode {
            // Index groups, the unverified group 25, gamma groups and every width field.
            let group = [0, 1, 7, 25, 26, 27, 31][r.below(7) as usize];
            *m = (r.next() & !0x1ff0) | group << 4 | r.below(8) << 9;
        }
        t.overlay_mode.iter_mut().for_each(|m| *m = r.next());
        t.cursor.iter_mut().for_each(|c| *c = r.next());
        // Cursor control: shown or not, crosshair or glyph, channel 1.
        t.cursor[0x81] = [0, 1, 1, 1, 5, 0x21, 1 | 4][round as usize % 7] | (r.next() & 0xc000_0010);
        let (x, y) = (r.below(W + 20) as i32 - 10, r.below(H + 20) as i32 - 10);
        t.cursor[0x80] = (x as u16 as u32) << 16 | y as u16 as u32;
        // The cursor's plane words are mostly 3 (opaque), some random.
        for i in 0..64 {
            t.cursor[0xc0 + i] = if r.below(4) == 0 { r.next() } else { 0xffff_ffff };
        }
        t
    }

    fn open() -> Option<(Arc<Gpu>, wgpu::Buffer, wgpu::Buffer)> {
        let gpu = match Gpu::new(&GpuOptions::default()) {
            Ok(g) => g,
            Err(e) => {
                eprintln!("SKIPPED: no GPU for the scanout parity test: {e}");
                return None;
            }
        };
        let usage = wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST;
        let size = W as u64 * H as u64 * 4;
        let (b0, b1) = (gpu.buffer("b0", size, usage), gpu.buffer("b1", size, usage));
        Some((gpu, b0, b1))
    }

    /// Random planes, ids and tables, composed on the GPU and compared with
    /// the CPU resolve word for word. Returns (differing, total).
    fn compare(gpu: &Arc<Gpu>, b0: &wgpu::Buffer, b1: &wgpu::Buffer, rounds: u32, spoil: bool) -> (usize, usize) {
        let compose = VproCompose::new(gpu, W, H).unwrap();
        let frame = SgiGpuFrame::new(gpu, W, H).unwrap();
        let mut r = Rng(0x9e37_79b9_7f4a_7c15);
        let (mut differing, mut total) = (0, 0);
        for round in 0..rounds {
            let mut t = random_tables(&mut r, round);
            let n = (W * H) as usize;
            let did: Vec<u8> = (0..n).map(|_| r.next() as u8).collect();
            let ovl: Vec<u8> = (0..n).map(|_| if r.below(3) == 0 { r.next() as u8 } else { 0 }).collect();
            let planes: Vec<Vec<u32>> = (0..2).map(|_| (0..n).map(|_| r.next() ^ r.next() << 16).collect()).collect();
            gpu.write(b0, 0, bytemuck::cast_slice(&planes[0]));
            gpu.write(b1, 0, bytemuck::cast_slice(&planes[1]));
            // Half the rounds upload the id rows in two pieces, as a partial change would.
            let half = (H / 2) as usize * W as usize;
            let rows = if round % 2 == 0 {
                vec![IdRows { y: 0, did: did.clone(), overlay: ovl.clone() }]
            } else {
                vec![IdRows { y: 0, did: did[..half].to_vec(), overlay: ovl[..half].to_vec() }, IdRows { y: H / 2, did: did[half..].to_vec(), overlay: ovl[half..].to_vec() }]
            };
            compose.compose_wait(&t, &rows, b0, b1, &frame).unwrap();
            let mut got = vec![0u32; n];
            frame.read(&mut got, W as usize).unwrap();
            if spoil {
                // Negative control: the reference sees a different lookup RAM.
                t.lut.iter_mut().for_each(|w| *w ^= 0x0040_0000);
            }
            let mut want = vec![0u32; W as usize];
            for y in 0..H as usize {
                reference_row(&t, &did, &ovl, [&planes[0], &planes[1]], y, &mut want);
                for x in 0..W as usize {
                    total += 1;
                    if got[y * W as usize + x] != want[x] {
                        if differing < 3 {
                            eprintln!("round {round} ({x}, {y}): gpu {:#08x} cpu {:#08x}", got[y * W as usize + x], want[x]);
                        }
                        differing += 1;
                    }
                }
            }
        }
        (differing, total)
    }

    #[test]
    fn matches_the_cpu_resolve_on_random_planes() {
        let Some((gpu, b0, b1)) = open() else { return };
        eprintln!("adapter: {}", gpu.caps);
        let (differing, total) = compare(&gpu, &b0, &b1, 21, false);
        assert_eq!(differing, 0, "{differing} of {total} words differ");
        let (spoiled, _) = compare(&gpu, &b0, &b1, 2, true);
        assert!(spoiled > 0, "the negative control found no difference: the comparison is not live");
    }
}
