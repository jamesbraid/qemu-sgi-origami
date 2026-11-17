// SPDX-License-Identifier: BSD-3-Clause
//! The process-wide GPU: opened on a worker thread, ordered fences, and the
//! open/ready/adapter/lost half of `include/hw/display/sgi-gpu.h`.

use std::collections::VecDeque;
use std::ffi::{c_char, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::atomic::{AtomicU64, AtomicU8, Ordering};
use std::sync::{Arc, Once, OnceLock};

use iris_wgpu::wgpu::{Gpu, GpuOptions};
use parking_lot::Mutex;

const OPENING: u8 = 0;
const READY: u8 = 1;
const UNAVAILABLE: u8 = 2;

static GPU: OnceLock<SgiGpu> = OnceLock::new();
static START: Once = Once::new();

/// Submission order of the fences handed out, oldest first.
#[derive(Default)]
struct Fences {
    last: u64,
    pending: VecDeque<(u64, wgpu::SubmissionIndex)>,
}

pub struct SgiGpu {
    state: AtomicU8,
    gpu: OnceLock<Arc<Gpu>>,
    adapter: OnceLock<CString>,
    /// Why the device is unusable: the open failure or the first loss.
    failure: OnceLock<CString>,
    fences: Mutex<Fences>,
    completed: Arc<AtomicU64>,
}

impl SgiGpu {
    fn new() -> SgiGpu {
        SgiGpu {
            state: AtomicU8::new(OPENING),
            gpu: OnceLock::new(),
            adapter: OnceLock::new(),
            failure: OnceLock::new(),
            fences: Mutex::new(Fences::default()),
            completed: Arc::new(AtomicU64::new(0)),
        }
    }

    fn open_device(&self) {
        let mut opts = GpuOptions { software: env_mode() == "software", ..GpuOptions::default() };
        // Frames are exported as D3D12 shared buffers; iris prefers Vulkan.
        if cfg!(windows) {
            opts.backend = "dx12".into();
        }
        let opened = catch_unwind(AssertUnwindSafe(|| Gpu::new(&opts)))
            .unwrap_or_else(|_| Err("panic while opening the GPU".into()));
        match opened {
            Ok(gpu) => {
                let name = CString::new(gpu.caps.adapter.replace('\0', " ")).unwrap_or_default();
                let _ = self.adapter.set(name);
                eprintln!("sgi-gpu: {}", gpu.caps);
                let _ = self.gpu.set(gpu);
                self.state.store(READY, Ordering::Release);
            }
            Err(e) => {
                eprintln!("sgi-gpu: unavailable: {e}");
                self.fail(&e);
                self.state.store(UNAVAILABLE, Ordering::Release);
            }
        }
    }

    fn fail(&self, msg: &str) {
        let _ = self.failure.set(CString::new(msg.replace('\0', " ")).unwrap_or_default());
    }

    pub(crate) fn is_ready(&self) -> bool {
        self.state.load(Ordering::Acquire) == READY
    }

    /// The device, once ready and until it is lost.
    pub(crate) fn gpu(&self) -> Option<&Arc<Gpu>> {
        if !self.is_ready() || self.lost().is_some() {
            return None;
        }
        self.gpu.get()
    }

    /// The failure message once the device has failed to open or been lost.
    pub(crate) fn lost(&self) -> Option<&CString> {
        if let Some(gpu) = self.gpu.get() {
            if let Err(e) = gpu.check() {
                self.fail(&e);
            }
        }
        self.failure.get()
    }

    /// A labelled command encoder for work to hand to `submit`.
    pub(crate) fn encoder(&self, label: &str) -> Option<wgpu::CommandEncoder> {
        self.gpu().map(|g| g.encoder(label))
    }

    /// Submit `encoder` behind everything submitted so far and return its
    /// fence. Fences complete in submission order and start at 1.
    pub(crate) fn submit(&self, encoder: wgpu::CommandEncoder) -> Option<u64> {
        let gpu = self.gpu()?;
        let mut f = self.fences.lock();
        let index = gpu.submit(encoder);
        let finished = self.completed.load(Ordering::Acquire);
        while f.pending.front().is_some_and(|p| p.0 <= finished) {
            f.pending.pop_front();
        }
        f.last += 1;
        let fence = f.last;
        f.pending.push_back((fence, index));
        let done = Arc::clone(&self.completed);
        // Registered under the lock, so it covers this submission and
        // every one before it.
        gpu.on_done(move || {
            done.fetch_max(fence, Ordering::AcqRel);
        });
        Some(fence)
    }

    pub(crate) fn fence_done(&self, fence: u64) -> bool {
        if fence == 0 || self.completed.load(Ordering::Acquire) >= fence {
            return true;
        }
        if fence > self.fences.lock().last {
            return false;
        }
        if let Some(gpu) = self.gpu.get() {
            gpu.poll();
        }
        self.completed.load(Ordering::Acquire) >= fence
    }

    pub(crate) fn fence_wait(&self, fence: u64) -> bool {
        if fence == 0 || self.completed.load(Ordering::Acquire) >= fence {
            return true;
        }
        let Some(gpu) = self.gpu.get() else { return false };
        let index = {
            let mut f = self.fences.lock();
            if fence > f.last {
                return false;
            }
            while f.pending.front().is_some_and(|p| p.0 < fence) {
                f.pending.pop_front();
            }
            match f.pending.front() {
                Some(p) if p.0 == fence => p.1.clone(),
                _ => return self.completed.load(Ordering::Acquire) >= fence,
            }
        };
        if let Err(e) = gpu.wait(Some(index)) {
            eprintln!("sgi-gpu: wait for fence {fence}: {e}");
            self.fail(&e);
            return false;
        }
        self.completed.fetch_max(fence, Ordering::AcqRel);
        true
    }
}

/// `SGI_GPU`: `off` disables the back end, `software` prefers lavapipe.
fn env_mode() -> String {
    std::env::var("SGI_GPU").unwrap_or_default().to_ascii_lowercase()
}

pub(crate) fn from_c(gpu: *mut SgiGpu) -> Option<&'static SgiGpu> {
    // SAFETY: SgiGpu pointers come from sgi_gpu_open, a leaked static.
    unsafe { gpu.as_ref() }
}

/// Process-wide device, opened on a worker thread; NULL if disabled.
#[no_mangle]
pub extern "C" fn sgi_gpu_open() -> *mut SgiGpu {
    if env_mode() == "off" {
        return ptr::null_mut();
    }
    let dev = GPU.get_or_init(SgiGpu::new);
    START.call_once(|| {
        let spawned = std::thread::Builder::new().name("sgi-gpu-open".into()).spawn(|| {
            if let Some(d) = GPU.get() {
                d.open_device();
            }
        });
        if let Err(e) = spawned {
            dev.fail(&format!("cannot start the GPU open thread: {e}"));
            dev.state.store(UNAVAILABLE, Ordering::Release);
        }
    });
    ptr::from_ref(dev).cast_mut()
}

#[no_mangle]
pub extern "C" fn sgi_gpu_ready(gpu: *mut SgiGpu) -> bool {
    from_c(gpu).is_some_and(|g| g.is_ready())
}

/// Adapter name once ready; NULL before.
#[no_mangle]
pub extern "C" fn sgi_gpu_adapter_name(gpu: *mut SgiGpu) -> *const c_char {
    from_c(gpu).and_then(|g| g.adapter.get()).map_or(ptr::null(), |s| s.as_ptr())
}

/// The message of an open failure or device loss; NULL while the device is fine.
#[no_mangle]
pub extern "C" fn sgi_gpu_lost(gpu: *mut SgiGpu) -> *const c_char {
    from_c(gpu).and_then(|g| g.lost()).map_or(ptr::null(), |s| s.as_ptr())
}

#[no_mangle]
pub extern "C" fn sgi_gpu_fence_done(gpu: *mut SgiGpu, fence: u64) -> bool {
    from_c(gpu).is_some_and(|g| g.fence_done(fence))
}

#[no_mangle]
pub extern "C" fn sgi_gpu_fence_wait(gpu: *mut SgiGpu, fence: u64) -> bool {
    from_c(gpu).is_some_and(|g| g.fence_wait(fence))
}
