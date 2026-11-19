// SPDX-License-Identifier: BSD-3-Clause
// Ported from src/wgpu/device.rs in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
//! The adapter, device and queue the boards share, with the limits the
//! raster kernels need and the errors wgpu reports asynchronously.

use std::fmt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
#[cfg(feature = "present")]
use std::sync::OnceLock;
use std::time::Duration;

use parking_lot::Mutex;

/// The window system's display connection, when iris opens a window.
///
/// wgpu's GL backend binds an instance to an EGL display when it is created:
/// with none it takes Mesa's surfaceless platform, which computes but cannot
/// present, so a board's device could never draw the window. Vulkan, Metal
/// and DX12 ignore it. Headless and windowless CI runs never set it, and
/// their instances open without one, as before.
#[cfg(feature = "present")]
static DISPLAY: OnceLock<winit::event_loop::OwnedDisplayHandle> = OnceLock::new();

#[cfg(feature = "present")]
type Display = winit::event_loop::OwnedDisplayHandle;
#[cfg(not(feature = "present"))]
type Display = ();

/// Bind every instance opened from now on to `display`: the event loop's,
/// so the window's surface is on the same connection. Set it before the
/// machine is built, so a board's device can present. The first call wins.
#[cfg(feature = "present")]
pub fn set_display(display: winit::event_loop::OwnedDisplayHandle) {
    let _ = DISPLAY.set(display);
}

/// How to open the GPU. All fields have defaults; `backend` is the only
/// one users set.
#[derive(Clone, Debug)]
pub struct GpuOptions {
    /// `auto`, `vulkan`, `metal`, `dx12` or `gl`.
    pub backend: String,
    /// Prefer the discrete adapter.
    pub high_performance: bool,
    /// Ask for the optional features the kernels can use when present
    /// (subgroups, 64-bit integers and atomics, timestamps).
    pub optional_features: bool,
    /// Prefer a software adapter (lavapipe, SwiftShader) when one exists:
    /// what CI runs the goldens on.
    pub software: bool,
}

impl Default for GpuOptions {
    fn default() -> Self {
        GpuOptions {
            backend: "auto".into(),
            high_performance: true,
            optional_features: true,
            software: false,
        }
    }
}

/// What the device turned out to have, for logs and for kernels that
/// branch on it.
#[derive(Clone, Debug)]
pub struct Caps {
    pub backend: wgpu::Backend,
    pub adapter: String,
    pub device_type: wgpu::DeviceType,
    pub driver: String,
    pub max_storage_binding: u64,
    pub max_buffer: u64,
    pub max_workgroups: u32,
    pub subgroups: bool,
    pub int64: bool,
    pub int64_atomic_min_max: bool,
    pub timestamps: bool,
}

impl fmt::Display for Caps {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:?} {} ({:?}, {}) storage {} MiB buffer {} MiB groups {} subgroups {} int64 {} int64-atomics {} timestamps {}",
            self.backend,
            self.adapter,
            self.device_type,
            self.driver,
            self.max_storage_binding >> 20,
            self.max_buffer >> 20,
            self.max_workgroups,
            self.subgroups,
            self.int64,
            self.int64_atomic_min_max,
            self.timestamps,
        )
    }
}

/// One GPU for the process. Boards hold an `Arc<Gpu>`; they share the
/// queue, so work submitted by two boards is ordered by submission.
pub struct Gpu {
    /// The instance and adapter the device came from: a window's surface
    /// for presenting has to come from the same instance, and the adapter
    /// says whether it can present to it (`src/wgpu/present.rs`).
    pub instance: wgpu::Instance,
    pub adapter: wgpu::Adapter,
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
    pub caps: Caps,
    /// The first asynchronous error (validation outside an error scope, or
    /// device loss), shared by every `Gpu` on the device. Checked by
    /// `check()`; a kernel bug shows up here.
    failure: Arc<Failure>,
    /// The readback buffers `read` lends, shared by every `Gpu` on the device.
    readback: Arc<Readback>,
    /// Writes waiting for the next submission (`write`).
    uploads: Mutex<Uploads>,
}

/// The first error on a device, which stays, and the machine's reaction to
/// it, run once.
struct Failure {
    /// Set after `error`, so the write path asks with one load.
    failed: AtomicBool,
    error: Mutex<Option<String>>,
    fatal: Arc<dyn Fn(&str) + Send + Sync>,
}

impl Failure {
    fn set(&self, msg: String) {
        let first = {
            let mut e = self.error.lock();
            let first = e.is_none();
            if first {
                *e = Some(msg.clone());
                self.failed.store(true, Ordering::Release);
            }
            first
        };
        // Outside the lock: the machine's reaction may look at the device.
        if first {
            (self.fatal)(&msg);
        }
    }
}

/// Bytes a readback buffer holds: a whole 1280 x 1024 frame and the region
/// reads the boards make fit at once. Larger reads go through in pieces.
const READBACK_BYTES: u64 = 16 << 20;

/// The readback buffers of a device, lent to one `Gpu::read` at a time and
/// kept for the next, so a board that never reads holds none and boards that
/// read in turn share one.
#[derive(Default)]
struct Readback {
    free: Mutex<Vec<wgpu::Buffer>>,
}

/// Writes queued by `Gpu::write` and `Gpu::write_texture`: copies out of a
/// ring of staging buffers, recorded into one encoder that the next
/// `Gpu::submit` sends ahead of its own work. The ring is made on the first
/// write, and a writer that finds every slot in flight adds one for good.
struct Uploads {
    slots: Vec<UploadSlot>,
    /// The slot being written.
    cur: usize,
    encoder: Option<wgpu::CommandEncoder>,
}

struct UploadSlot {
    buffer: wgpu::Buffer,
    /// In host memory (`host::raw`): the mapping, which stays, and the
    /// memory. Null and None for a wgpu-mapped slot (other backends), which
    /// is unmapped while in flight and mapped again behind it.
    ptr: *mut u8,
    #[cfg(any(target_os = "linux", target_os = "windows"))]
    raw: Option<super::host::vk::RawMemory>,
    /// Mapped and writable from `cursor` on.
    mapped: bool,
    cursor: u64,
    /// Written since the last submission.
    used: bool,
    /// Unmapped by the submission being sent: to map again behind it.
    sent: bool,
    /// Set once the slot's last submission is done (the map callback, or
    /// the queue's work-done callback in host memory).
    ready: Arc<std::sync::atomic::AtomicBool>,
}

/// The ring: its first slots and their size. A batch's worst case is
/// about 6 MiB (4096 records of 352 bytes, the boxes, 1024 state slots and
/// the 4 MiB blit stage), so one slot holds a batch; four let four
/// submissions be in flight before the ring grows. A larger write (a
/// mirror upload) is split over slots.
const UPLOAD_SLOTS: usize = 4;
const UPLOAD_SLOT_BYTES: u64 = 8 << 20;

// SAFETY: the mapping is plain memory, reached only under the uploads lock.
unsafe impl Send for UploadSlot {}

impl UploadSlot {
    fn new(device: &wgpu::Device, i: usize) -> UploadSlot {
        let label = format!("upload ring {i}");
        let ready = Arc::new(std::sync::atomic::AtomicBool::new(false));
        if let Some((buffer, ptr, raw)) = super::host::raw(device, &label, UPLOAD_SLOT_BYTES, wgpu::BufferUsages::COPY_SRC) {
            #[cfg(not(any(target_os = "linux", target_os = "windows")))]
            let _ = raw;
            return UploadSlot {
                buffer,
                ptr,
                #[cfg(any(target_os = "linux", target_os = "windows"))]
                raw: Some(raw),
                mapped: true,
                cursor: 0,
                used: false,
                sent: false,
                ready,
            };
        }
        UploadSlot {
            buffer: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(&label),
                size: UPLOAD_SLOT_BYTES,
                usage: wgpu::BufferUsages::MAP_WRITE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: true,
            }),
            ptr: std::ptr::null_mut(),
            #[cfg(any(target_os = "linux", target_os = "windows"))]
            raw: None,
            mapped: true,
            cursor: 0,
            used: false,
            sent: false,
            ready,
        }
    }

    /// Store `bytes` at `at` of the mapped slot.
    fn store(&self, at: u64, bytes: &[u8]) {
        if self.ptr.is_null() {
            self.buffer.slice(at..at + bytes.len() as u64).get_mapped_range_mut().expect("a mapped upload slot").copy_from_slice(bytes);
        } else {
            // SAFETY: reserve() handed out [at, at + len) inside the slot,
            // which no submission in flight reads.
            unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), self.ptr.add(at as usize), bytes.len()) };
        }
    }
}

impl Uploads {
    fn new() -> Uploads {
        Uploads { slots: Vec::new(), cur: 0, encoder: None }
    }

    /// Room for `len` bytes (at most a slot): the slot and offset to write
    /// at. Moves to the next slot when the current one is full, and waits
    /// for it when it is still in flight.
    /// `bulk`: this write has already submitted part of itself, so slots
    /// in flight are its own and coming back; wait for one rather than
    /// grow the ring for a one-off large upload.
    /// None once the device has failed: its slots never come back.
    fn reserve(&mut self, gpu: &Gpu, len: u64, bulk: &mut bool) -> Option<(usize, u64)> {
        assert!(len <= UPLOAD_SLOT_BYTES, "an upload piece larger than a slot");
        if self.slots.is_empty() {
            self.slots.extend((0..UPLOAD_SLOTS).map(|i| UploadSlot::new(&gpu.device, i)));
        }
        loop {
            if gpu.check().is_err() {
                return None;
            }
            for s in &mut self.slots {
                if !s.mapped && s.ready.swap(false, std::sync::atomic::Ordering::Acquire) {
                    s.mapped = true;
                    s.cursor = 0;
                }
            }
            let s = &mut self.slots[self.cur];
            if s.mapped && s.cursor + len <= UPLOAD_SLOT_BYTES {
                let at = s.cursor;
                // Copies need 4-byte offsets, mapped ranges 8-byte ones.
                s.cursor = (at + len + 7) & !7;
                s.used = true;
                return Some((self.cur, at));
            }
            // The current slot is full or in flight: a mapped slot nothing
            // was written to yet; else send the writes queued so far, which
            // frees their slots once done; else, every slot being in flight,
            // wait for one within a large write, or add one for good.
            let n = self.slots.len();
            if let Some(k) = (1..n).map(|d| (self.cur + d) % n).find(|&k| self.slots[k].mapped && !self.slots[k].used) {
                self.cur = k;
            } else if self.slots.iter().any(|s| s.used) {
                gpu.submit_locked(self, None);
                *bulk = true;
            } else if *bulk {
                if let Err(e) = gpu.device.poll(wgpu::PollType::Wait { submission_index: None, timeout: Some(Duration::from_secs(30)) }) {
                    gpu.fail(&format!("wait for an upload slot: {e}"));
                    return None;
                }
            } else {
                self.slots.push(UploadSlot::new(&gpu.device, n));
                self.cur = n;
                log::info!("wgpu: upload ring grown to {} slots of {} MiB", n + 1, UPLOAD_SLOT_BYTES >> 20);
            }
        }
    }

    fn encoder(&mut self, device: &wgpu::Device) -> &mut wgpu::CommandEncoder {
        self.encoder.get_or_insert_with(|| device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("uploads") }))
    }
}

impl Drop for Gpu {
    /// The upload ring's host memory is ours to free, once the GPU is done
    /// with it.
    fn drop(&mut self) {
        if self.uploads.get_mut().slots.is_empty() {
            return;
        }
        let _ = self.device.poll(wgpu::PollType::Wait { submission_index: None, timeout: Some(Duration::from_secs(30)) });
        #[cfg(any(target_os = "linux", target_os = "windows"))]
        for s in &mut self.uploads.get_mut().slots {
            if let Some(raw) = s.raw.take() {
                s.buffer.destroy();
                raw.free();
            }
        }
    }
}

impl Gpu {
    /// Open the GPU `opts` asks for. Fails with the reasons each candidate
    /// backend gave when none works. `fatal` runs once, on the first error
    /// the device reports (or `fail` is given), from whichever thread saw it.
    pub fn new(opts: &GpuOptions, fatal: Box<dyn Fn(&str) + Send + Sync>) -> Result<Arc<Gpu>, String> {
        let fatal: Arc<dyn Fn(&str) + Send + Sync> = Arc::from(fatal);
        // wgpu's own `WGPU_BACKEND` (a comma list: vulkan, metal, dx12, gl)
        // overrides `[graphics] gpu_backend`.
        let candidates = match wgpu::Backends::from_env() {
            Some(backends) => {
                eprintln!("iris: wgpu backend from WGPU_BACKEND ({backends:?}), not backend={}", opts.backend);
                vec![("WGPU_BACKEND", backends)]
            }
            None => candidates(&opts.backend)?,
        };
        let mut failures = Vec::new();
        for (name, backends) in candidates {
            // With the window's display first; without, should the backend
            // refuse it (an EGL with no X11 or Wayland platform), the device
            // still computes and its frames are read back for the window.
            #[cfg(feature = "present")]
            let displays = DISPLAY.get().map(Some).into_iter().chain([None]);
            #[cfg(not(feature = "present"))]
            let displays = [None];
            for display in displays {
                match Self::open(backends, opts, display, Arc::clone(&fatal)) {
                    Ok(gpu) => {
                        log::info!("wgpu: {}", gpu.caps);
                        return Ok(gpu);
                    }
                    Err(e) => {
                        let how = if display.is_some() { " (with the window's display)" } else { "" };
                        failures.push(format!("{name}{how}: {e}"));
                    }
                }
            }
        }
        Err(format!("no usable GPU for backend={}: {}", opts.backend, failures.join("; ")))
    }

    #[cfg_attr(not(feature = "present"), allow(unused_variables))]
    fn open(backends: wgpu::Backends, opts: &GpuOptions, display: Option<&Display>, fatal: Arc<dyn Fn(&str) + Send + Sync>) -> Result<Arc<Gpu>, String> {
        // The rest of wgpu's environment applies as well: the instance flags
        // (WGPU_DEBUG, WGPU_VALIDATION, ...) and the backend options
        // (WGPU_GL_FENCE_BEHAVIOR, WGPU_GLES_MINOR_VERSION, WGPU_DX12_COMPILER, ...).
        #[cfg(feature = "present")]
        let desc = match display {
            Some(d) => wgpu::InstanceDescriptor::new_with_display_handle(Box::new(d.clone())),
            None => wgpu::InstanceDescriptor::new_without_display_handle(),
        };
        #[cfg(not(feature = "present"))]
        let desc = wgpu::InstanceDescriptor::new_without_display_handle();
        let mut desc = desc.with_env();
        desc.backends = backends;
        let instance = wgpu::Instance::new(desc);
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: if opts.high_performance {
                wgpu::PowerPreference::HighPerformance
            } else {
                wgpu::PowerPreference::None
            },
            force_fallback_adapter: opts.software,
            compatible_surface: None,
            ..Default::default()
        }))
        .map_err(|e| format!("request adapter: {e}"))?;
        let info = adapter.get_info();
        let have = adapter.features();
        let limits = adapter.limits();

        // Features: only what is there. Kernels branch on `caps`.
        let mut features = wgpu::Features::empty();
        if opts.optional_features {
            for f in [
                wgpu::Features::SUBGROUP,
                wgpu::Features::SHADER_INT64,
                wgpu::Features::SHADER_INT64_ATOMIC_MIN_MAX,
                wgpu::Features::TIMESTAMP_QUERY,
                wgpu::Features::TIMESTAMP_QUERY_INSIDE_ENCODERS,
            ] {
                if have.contains(f) {
                    features |= f;
                }
            }
        }
        // Limits: the adapter's own for buffer sizes, so a 128 MiB board
        // memory is one binding; the defaults elsewhere.
        let mut required = wgpu::Limits::default();
        required.max_storage_buffer_binding_size = limits.max_storage_buffer_binding_size;
        required.max_buffer_size = limits.max_buffer_size;
        required.max_storage_buffers_per_shader_stage =
            limits.max_storage_buffers_per_shader_stage.min(16).max(required.max_storage_buffers_per_shader_stage);
        required.max_compute_workgroups_per_dimension = limits.max_compute_workgroups_per_dimension;
        required.max_compute_invocations_per_workgroup = limits.max_compute_invocations_per_workgroup;
        required.max_compute_workgroup_storage_size = limits.max_compute_workgroup_storage_size;

        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("IRIS wgpu raster"),
            required_features: features,
            required_limits: required,
            // Small blocks for what is suballocated: the default (256 MiB
            // of device memory, 64 MiB of mappable memory a block) more than
            // doubled what a board took. Large buffers (a board's memory)
            // get blocks of their own either way.
            memory_hints: wgpu::MemoryHints::Manual { suballocated_device_memory_block_size: (4 << 20)..(32 << 20) },
            ..Default::default()
        }))
        .map_err(|e| format!("request device on {}: {e}", info.name))?;

        let failure = Arc::new(Failure { failed: AtomicBool::new(false), error: Mutex::new(None), fatal });
        let sink = Arc::clone(&failure);
        device.on_uncaptured_error(Arc::new(move |e| {
            let msg = format!("wgpu error: {e}");
            log::error!("{msg}");
            sink.set(msg);
        }));
        let sink = Arc::clone(&failure);
        device.set_device_lost_callback(move |reason, msg| {
            let msg = format!("wgpu device lost ({reason:?}): {msg}");
            log::error!("{msg}");
            sink.set(msg);
        });

        let caps = Caps {
            backend: info.backend,
            adapter: info.name.clone(),
            device_type: info.device_type,
            driver: format!("{} {}", info.driver, info.driver_info),
            max_storage_binding: limits.max_storage_buffer_binding_size,
            max_buffer: limits.max_buffer_size,
            max_workgroups: limits.max_compute_workgroups_per_dimension,
            subgroups: features.contains(wgpu::Features::SUBGROUP),
            int64: features.contains(wgpu::Features::SHADER_INT64),
            int64_atomic_min_max: features.contains(wgpu::Features::SHADER_INT64_ATOMIC_MIN_MAX),
            timestamps: features.contains(wgpu::Features::TIMESTAMP_QUERY),
        };
        let uploads = Mutex::new(Uploads::new());
        Ok(Arc::new(Gpu { instance, adapter, device, queue, caps, failure, readback: Arc::default(), uploads }))
    }

    /// The first asynchronous error, if any. Kernels that misbehave are
    /// reported here, not at the call that queued them.
    pub fn check(&self) -> Result<(), String> {
        if !self.failure.failed.load(Ordering::Acquire) {
            return Ok(());
        }
        Err(self.failure.error.lock().clone().unwrap_or_default())
    }

    /// Record `msg` as the device's error, if it has none yet, and raise the
    /// fatal callback. For a failure the device did not report itself (a
    /// refused readback, a wait that timed out): the caller has no result it
    /// can substitute, so the machine stops rather than carry on with a
    /// stale mirror.
    pub fn fail(&self, msg: &str) {
        log::error!("wgpu: {msg}");
        self.failure.set(msg.to_string());
    }

    /// Another handle on the same device and queue, with an upload ring of
    /// its own (made on its first write). The error, and so the fatal
    /// callback, is shared: a loss is every board's.
    pub fn share(self: &Arc<Self>) -> Arc<Gpu> {
        Arc::new(Gpu {
            instance: self.instance.clone(),
            adapter: self.adapter.clone(),
            device: self.device.clone(),
            queue: self.queue.clone(),
            caps: self.caps.clone(),
            failure: Arc::clone(&self.failure),
            readback: Arc::clone(&self.readback),
            uploads: Mutex::new(Uploads::new()),
        })
    }

    /// Submit one command buffer, after the writes queued so far.
    pub fn submit(&self, encoder: wgpu::CommandEncoder) -> wgpu::SubmissionIndex {
        let mut u = self.uploads.lock();
        let index = self.submit_locked(&mut u, Some(encoder));
        drop(u);
        // The ring's slots come back as their maps complete, which a poll
        // reports.
        let _ = self.device.poll(wgpu::PollType::Poll);
        index
    }

    /// Submit the queued writes and then `encoder`; unmap the slots the
    /// writes used and map them again behind the submission.
    fn submit_locked(&self, u: &mut Uploads, encoder: Option<wgpu::CommandEncoder>) -> wgpu::SubmissionIndex {
        let up = u.encoder.take().map(|e| e.finish());
        for s in &mut u.slots {
            if s.used {
                if s.ptr.is_null() {
                    s.buffer.unmap();
                }
                s.mapped = false;
                s.used = false;
                s.sent = true;
            }
        }
        let index = self.queue.submit(up.into_iter().chain(encoder.map(|e| e.finish())));
        for s in &mut u.slots {
            if std::mem::take(&mut s.sent) {
                let ready = Arc::clone(&s.ready);
                if !s.ptr.is_null() {
                    self.queue.on_submitted_work_done(move || ready.store(true, std::sync::atomic::Ordering::Release));
                    continue;
                }
                s.buffer.slice(..).map_async(wgpu::MapMode::Write, move |r| {
                    if r.is_ok() {
                        ready.store(true, std::sync::atomic::Ordering::Release);
                    }
                });
            }
        }
        index
    }

    /// Queue a write of `bytes` at `offset` of `target`, as
    /// `Queue::write_buffer` does: it lands before the work of the next
    /// `submit`. Offset and length must be multiples of 4.
    /// After a device failure it does nothing: a lost device's mapped slots
    /// stop mapping, and every call that returns a result reports the failure.
    pub fn write(&self, target: &wgpu::Buffer, offset: u64, bytes: &[u8]) {
        let mut u = self.uploads.lock();
        let mut bulk = false;
        for (i, piece) in bytes.chunks(UPLOAD_SLOT_BYTES as usize).enumerate() {
            let Some((slot, at)) = u.reserve(self, piece.len() as u64, &mut bulk) else { return };
            let buffer = u.slots[slot].buffer.clone();
            u.slots[slot].store(at, piece);
            u.encoder(&self.device).copy_buffer_to_buffer(&buffer, at, target, offset + i as u64 * UPLOAD_SLOT_BYTES, piece.len() as u64);
        }
    }

    /// Queue a write of `height` rows of `width` texels (4 bytes each,
    /// `stride` texels apart in `rows`) to the top-left of `texture`, as
    /// `Queue::write_texture` does.
    pub fn write_texture(&self, texture: &wgpu::Texture, rows: &[u32], stride: usize, width: u32, height: u32) {
        let pitch = (4 * width as u64).next_multiple_of(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT as u64);
        let per_piece = (UPLOAD_SLOT_BYTES / pitch).max(1) as u32;
        let mut u = self.uploads.lock();
        let mut bulk = false;
        let mut y0 = 0;
        while y0 < height {
            let n = per_piece.min(height - y0);
            let Some((slot, at)) = u.reserve(self, n as u64 * pitch, &mut bulk) else { return };
            let buffer = u.slots[slot].buffer.clone();
            for r in 0..n as usize {
                let src = &rows[(y0 as usize + r) * stride..][..width as usize];
                u.slots[slot].store(at + r as u64 * pitch, bytemuck::cast_slice(src));
            }
            u.encoder(&self.device).copy_buffer_to_texture(
                wgpu::TexelCopyBufferInfo { buffer: &buffer, layout: wgpu::TexelCopyBufferLayout { offset: at, bytes_per_row: Some(pitch as u32), rows_per_image: Some(n) } },
                wgpu::TexelCopyTextureInfo { texture, mip_level: 0, origin: wgpu::Origin3d { x: 0, y: y0, z: 0 }, aspect: wgpu::TextureAspect::All },
                wgpu::Extent3d { width, height: n, depth_or_array_layers: 1 },
            );
            y0 += n;
        }
    }

    /// Block until `submission` has finished (or everything, with None).
    pub fn wait(&self, submission: Option<wgpu::SubmissionIndex>) -> Result<(), String> {
        let waited = self.device.poll(wgpu::PollType::Wait { submission_index: submission, timeout: Some(Duration::from_secs(30)) });
        // wgpu cannot wait for a submission it refused (a pipeline that
        // failed to build, a lost device); the error that refused it is
        // the one to report.
        self.check()?;
        if let Err(e) = waited {
            // A wait that fails leaves the work in an unknown state.
            self.fail(&format!("wait for GPU: {e}"));
        }
        self.check()
    }

    /// Read `ranges` (offset, length; each 4-aligned, inside `src`) of `src`
    /// into `out`, concatenated in order; `out` must be exactly their total
    /// length. Waits for every submission so far, so the bytes include all
    /// work queued before the call, and changes `out` only for the pieces
    /// that arrived. `src` needs `COPY_SRC`.
    /// A read that fails drops its staging buffer instead of returning it.
    pub fn read(&self, src: &wgpu::Buffer, ranges: &[(u64, u64)], out: &mut [u8]) -> Result<(), String> {
        let span: u64 = ranges.iter().map(|r| r.1).sum();
        if span != out.len() as u64 {
            return Err(format!("readback: {} bytes asked for, room for {}", span, out.len()));
        }
        if span == 0 {
            return Ok(());
        }
        self.check()?;
        let staging = self.readback.free.lock().pop().unwrap_or_else(|| {
            self.buffer("readback", READBACK_BYTES, wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST)
        });
        self.read_through(&staging, src, ranges, out)?;
        self.readback.free.lock().push(staging);
        self.check()
    }

    /// As `read`, into a new vector.
    #[cfg(test)]
    pub(crate) fn read_vec(&self, src: &wgpu::Buffer, ranges: &[(u64, u64)]) -> Result<Vec<u8>, String> {
        let mut out = vec![0u8; ranges.iter().map(|r| r.1).sum::<u64>() as usize];
        self.read(src, ranges, &mut out)?;
        Ok(out)
    }

    fn read_through(&self, staging: &wgpu::Buffer, src: &wgpu::Buffer, ranges: &[(u64, u64)], out: &mut [u8]) -> Result<(), String> {
        let mut ranges = ranges.iter().copied().peekable();
        let mut rest: Option<(u64, u64)> = None;
        let mut done = 0usize;
        loop {
            // Fill the buffer with as much of the ranges as fits.
            let mut enc = self.encoder("readback");
            let mut at = 0;
            while at < READBACK_BYTES {
                let Some((off, len)) = rest.take().or_else(|| ranges.next()) else { break };
                debug_assert!(off % 4 == 0 && len % 4 == 0 && off + len <= src.size());
                let n = len.min(READBACK_BYTES - at);
                enc.copy_buffer_to_buffer(src, off, staging, at, n);
                at += n;
                if n < len {
                    rest = Some((off + n, len - n));
                }
            }
            if at == 0 {
                return Ok(());
            }
            let submission = self.submit(enc);
            let (tx, rx) = std::sync::mpsc::sync_channel(1);
            staging.slice(..at).map_async(wgpu::MapMode::Read, move |r| {
                let _ = tx.send(r);
            });
            let piece = (|| {
                self.wait(Some(submission))?;
                rx.recv().map_err(|e| format!("readback callback: {e}"))?.map_err(|e| format!("map readback: {e}"))?;
                let view = staging.slice(..at).get_mapped_range().map_err(|e| format!("readback range: {e}"))?;
                out[done..done + at as usize].copy_from_slice(&view);
                Ok::<(), String>(())
            })();
            // A wait that timed out leaves the map pending: wgpu refuses any
            // submission that uses a buffer not idle. Unmap aborts it, and
            // the caller drops the buffer.
            staging.unmap();
            piece?;
            done += at as usize;
            if rest.is_none() && ranges.peek().is_none() {
                return Ok(());
            }
        }
    }

    /// Let callbacks run without blocking.
    pub fn poll(&self) {
        let _ = self.device.poll(wgpu::PollType::Poll);
    }

    /// Run `f` after every submission so far has finished. The callback
    /// runs on a wgpu thread or inside a later `poll`; keep it short.
    pub fn on_done(&self, f: impl FnOnce() + Send + 'static) {
        self.queue.on_submitted_work_done(f);
    }

    /// A command encoder with a label.
    pub fn encoder(&self, label: &str) -> wgpu::CommandEncoder {
        self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some(label) })
    }

    /// A buffer of `size` bytes with `usage`, zeroed.
    pub fn buffer(&self, label: &str, size: u64, usage: wgpu::BufferUsages) -> wgpu::Buffer {
        self.device.create_buffer(&wgpu::BufferDescriptor { label: Some(label), size, usage, mapped_at_creation: false })
    }
}

fn candidates(backend: &str) -> Result<Vec<(&'static str, wgpu::Backends)>, String> {
    Ok(match backend {
        "vulkan" => vec![("vulkan", wgpu::Backends::VULKAN)],
        "metal" => vec![("metal", wgpu::Backends::METAL)],
        "dx12" => vec![("dx12", wgpu::Backends::DX12)],
        "gl" => vec![("gl", wgpu::Backends::GL)],
        "auto" if cfg!(target_os = "macos") => vec![("metal", wgpu::Backends::METAL)],
        "auto" if cfg!(target_os = "windows") => {
            vec![("vulkan", wgpu::Backends::VULKAN), ("dx12", wgpu::Backends::DX12)]
        }
        "auto" => vec![("vulkan", wgpu::Backends::VULKAN), ("gl", wgpu::Backends::GL)],
        other => return Err(format!("backend={other}: want auto, vulkan, metal, dx12 or gl")),
    })
}
