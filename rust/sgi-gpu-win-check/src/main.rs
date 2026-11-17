// SPDX-License-Identifier: BSD-3-Clause
//! Two-process check of `frame_win`: a viewer opens the exported buffers and fence and sums every pixel.
//! Usage: sgi-gpu-win-check W H FRAMES gpu|nowait [HEAVY]; nowait is the control without the fence wait.

mod frame;

use std::io::{BufRead, BufReader, Write};
use std::process::{Command, Stdio};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use frame::frame_win::Export;
use iris_wgpu::wgpu::{Gpu, GpuOptions};
use wgpu::hal::api::Dx12;
use windows::Win32::Foundation::{DuplicateHandle, DUPLICATE_SAME_ACCESS, HANDLE};
use windows::Win32::Graphics::Direct3D12::{ID3D12Fence, ID3D12Resource};
use windows::Win32::System::Threading::GetCurrentProcess;

const SLOTS: usize = 3;
const WARM: u32 = 30;

/// WARP unless WIN_CHECK_ADAPTER=auto (the runners have no other adapter).
fn open_gpu() -> Result<Arc<Gpu>, String> {
    let software = std::env::var("WIN_CHECK_ADAPTER").map_or(true, |v| v != "auto");
    Gpu::new(&GpuOptions { backend: "dx12".into(), software, ..GpuOptions::default() })
}

/// The pixel the producer paints, as (r, g, b).
fn pix(x: u32, y: u32, step: u32) -> [u32; 3] {
    [(x + step) & 255, y.wrapping_mul(3).wrapping_add(step.wrapping_mul(5)) & 255, (x ^ y ^ step) & 255]
}

/// Position-weighted sum of every pixel for each of the 256 steps: a plain
/// sum is the same for many steps, so a stale slot would pass it.
fn expected(w: u32, h: u32) -> Vec<u32> {
    let per = 256usize.div_ceil(8);
    let mut out = vec![0u32; 256];
    std::thread::scope(|s| {
        for (c, chunk) in out.chunks_mut(per).enumerate() {
            s.spawn(move || {
                for (i, o) in chunk.iter_mut().enumerate() {
                    let step = (c * per + i) as u32;
                    let mut acc = 0u32;
                    for y in 0..h {
                        for x in 0..w {
                            let p = pix(x, y, step);
                            acc = acc.wrapping_add(p[0].wrapping_mul(1 + (x & 63))).wrapping_add(p[1].wrapping_mul(1 + (y & 63))).wrapping_add(p[2].wrapping_mul(1 + ((x + y) & 63)));
                        }
                    }
                    *o = acc;
                }
            });
        }
    });
    out
}

fn shader_compose(w: u32, h: u32, heavy: u32) -> String {
    format!(
        r#"
struct P {{ step: u32, stride: u32 }}
@group(0) @binding(0) var<storage, read_write> o: array<u32>;
@group(0) @binding(1) var<uniform> p: P;
@compute @workgroup_size(16, 16)
fn main(@builtin(global_invocation_id) g: vec3u) {{
  if (g.x >= {w}u || g.y >= {h}u) {{ return; }}
  let r = (g.x + p.step) & 255u;
  let gg = (g.y * 3u + p.step * 5u) & 255u;
  let b = (g.x ^ g.y ^ p.step) & 255u;
  var s = g.x * 2654435761u + g.y;
  for (var i = 0u; i < {heavy}u; i += 1u) {{ s = s * 1664525u + 1013904223u; s ^= s >> 13u; }}
  var a = 0u;
  if (s == 0x9E3779B1u && g.x == 99999u) {{ a = 1u; }}
  o[g.y * p.stride + g.x] = (r << 16u) | (gg << 8u) | b | a;
}}
"#
    )
}

fn shader_sum(w: u32, stride: u32) -> String {
    format!(
        r#"
@group(0) @binding(0) var<storage, read> px: array<u32>;
@group(0) @binding(1) var<storage, read_write> res: array<atomic<u32>>;
@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg: vec3u, @builtin(local_invocation_id) li: vec3u) {{
  let y = wg.x;
  var s: u32 = 0u;
  for (var x = li.x; x < {w}u; x += 64u) {{
    let v = px[y * {stride}u + x];
    s += ((v >> 16u) & 255u) * (1u + (x & 63u)) + ((v >> 8u) & 255u) * (1u + (y & 63u)) + (v & 255u) * (1u + ((x + y) & 63u));
  }}
  atomicAdd(&res[0], s);
}}
"#
    )
}

fn dup_into(process: HANDLE, h: u64) -> Result<u64, String> {
    let mut out = HANDLE::default();
    // SAFETY: `h` is a live handle of this process and `process` the child.
    unsafe { DuplicateHandle(GetCurrentProcess(), HANDLE(h as *mut _), process, &mut out, 0, false, DUPLICATE_SAME_ACCESS) }.map_err(|e| format!("DuplicateHandle: {e}"))?;
    Ok(out.0 as u64)
}

fn pct(v: &mut [f64], p: f64) -> f64 {
    v.sort_by(|a, b| a.total_cmp(b));
    v.get(((v.len().max(1) - 1) as f64 * p).round() as usize).copied().unwrap_or(f64::NAN)
}

struct Tally {
    free: Vec<usize>,
    started: Vec<Option<Instant>>,
    lat: Vec<f64>,
    bad: u32,
    done: u32,
}

impl Tally {
    /// One `DONE slot seq ok` line from the viewer, with when it arrived.
    fn reap(&mut self, (line, at): (String, Instant)) -> Result<(), String> {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.first() != Some(&"DONE") || f.len() < 4 {
            return Err(format!("viewer: {line}"));
        }
        let slot: usize = f[1].parse().map_err(|_| "bad slot")?;
        self.lat.push(self.started[slot].map_or(0.0, |t| at.duration_since(t).as_secs_f64() * 1e3));
        self.bad += (f[3] != "1") as u32;
        self.done += 1;
        self.free.push(slot);
        Ok(())
    }
}

fn producer(w: u32, h: u32, frames: u32, gpu_wait: bool, heavy: u32) -> Result<String, String> {
    let gpu = open_gpu()?;
    let stride = w.next_multiple_of(64);
    let mut bufs = vec![];
    let mut exports = vec![];
    for _ in 0..SLOTS {
        let (buf, export) = Export::new(&gpu, w, h, stride)?;
        bufs.push(buf);
        exports.push(export);
    }
    let infos: Vec<_> = exports.iter().map(|e| e.info(w, h, stride)).collect();
    let mut child = Command::new(std::env::current_exe().map_err(|e| e.to_string())?)
        .args(["viewer", &w.to_string(), &h.to_string(), &stride.to_string()])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("spawn viewer: {e}"))?;
    let mut to_viewer = child.stdin.take().ok_or("no viewer stdin")?;
    let (tx, rx) = mpsc::channel::<(String, Instant)>();
    let out = child.stdout.take().ok_or("no viewer stdout")?;
    std::thread::spawn(move || {
        for line in BufReader::new(out).lines().map_while(Result::ok) {
            if tx.send((line, Instant::now())).is_err() {
                break;
            }
        }
    });
    let process = {
        use std::os::windows::io::AsRawHandle;
        HANDLE(child.as_raw_handle())
    };
    let mut setup = "SETUP".to_string();
    for i in &infos {
        setup += &format!(" {} {}", dup_into(process, i.handle as u64)?, dup_into(process, i.fence as u64)?);
    }
    writeln!(to_viewer, "{setup}").map_err(|e| e.to_string())?;
    let (ready, _) = rx.recv_timeout(Duration::from_secs(120)).map_err(|_| "viewer sent no READY")?;
    if !ready.starts_with("READY") {
        return Err(format!("viewer: {ready}"));
    }
    let luid_same = ready == format!("READY {}", infos[0].adapter_id);

    let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor { label: None, source: wgpu::ShaderSource::Wgsl(shader_compose(w, h, heavy).into()) });
    let pipe = gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor { label: None, layout: None, module: &module, entry_point: Some("main"), compilation_options: Default::default(), cache: None });
    let params = gpu.buffer("params", 16, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    let binds: Vec<wgpu::BindGroup> = bufs
        .iter()
        .map(|b| {
            gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None,
                layout: &pipe.get_bind_group_layout(0),
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: b.as_entire_binding() }, wgpu::BindGroupEntry { binding: 1, resource: params.as_entire_binding() }],
            })
        })
        .collect();
    let table = expected(w, h);

    let period = Duration::from_micros(16_667);
    let mut t = Tally { free: (0..SLOTS).collect(), started: vec![None; SLOTS], lat: vec![], bad: 0, done: 0 };
    let total = frames + WARM;
    let mut next = Instant::now();
    for seq in 1..=total {
        while t.free.is_empty() {
            t.reap(rx.recv_timeout(Duration::from_secs(120)).map_err(|_| "viewer went quiet")?)?;
        }
        while let Ok(line) = rx.try_recv() {
            t.reap(line)?;
        }
        let slot = t.free.remove(0);
        let step = seq & 255;
        t.started[slot] = Some(Instant::now());
        gpu.write(&params, 0, &[step.to_le_bytes(), stride.to_le_bytes(), [0; 4], [0; 4]].concat());
        let mut enc = gpu.encoder("compose");
        {
            let mut pass = enc.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipe);
            pass.set_bind_group(0, &binds[slot], &[]);
            pass.dispatch_workgroups(w.div_ceil(16), h.div_ceil(16), 1);
        }
        gpu.submit(enc);
        let value = exports[slot].signal(&gpu)?;
        writeln!(to_viewer, "FRAME {slot} {seq} {value} {} {}", table[step as usize], gpu_wait as u8).map_err(|e| e.to_string())?;
        next += period;
        match next.checked_duration_since(Instant::now()) {
            Some(rest) => std::thread::sleep(rest),
            None => next = Instant::now(),
        }
    }
    while t.done < total {
        t.reap(rx.recv_timeout(Duration::from_secs(120)).map_err(|_| "viewer went quiet at the end")?)?;
    }
    writeln!(to_viewer, "STOP").map_err(|e| e.to_string())?;
    let _ = child.wait();
    for b in &bufs {
        b.destroy();
    }
    gpu.wait(None)?;
    for e in exports {
        e.free(&gpu);
    }
    // The warm-up frames come first in `lat`; drop them.
    let mut l = t.lat.split_off(WARM as usize);
    Ok(format!(
        "{{\"w\":{w},\"h\":{h},\"sync\":\"{}\",\"heavy\":{heavy},\"frames\":{total},\"mismatch\":{},\"luid_same\":{luid_same},\"lat_p50_ms\":{:.2},\"lat_p95_ms\":{:.2},\"adapter\":\"{}\"}}",
        if gpu_wait { "gpu" } else { "nowait" },
        t.bad,
        pct(&mut l, 0.5),
        pct(&mut l, 0.95),
        gpu.caps.adapter
    ))
}

fn viewer(w: u32, h: u32, stride: u32) -> Result<(), String> {
    // The viewer is an ordinary wgpu device: it knows nothing of iris.
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor { backends: wgpu::Backends::DX12, ..wgpu::InstanceDescriptor::new_without_display_handle() });
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions { force_fallback_adapter: true, ..Default::default() })).map_err(|e| format!("request_adapter: {e:?}"))?;
    let (dev, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor { required_limits: adapter.limits(), ..Default::default() })).map_err(|e| format!("request_device: {e:?}"))?;
    // SAFETY: the hal device borrow ends with the clone.
    let raw = unsafe { dev.as_hal::<Dx12>().ok_or("not D3D12")?.raw_device().clone() };
    let stdin = std::io::stdin();
    let mut lines = stdin.lock().lines().map_while(Result::ok);
    let setup = lines.next().ok_or("no SETUP")?;
    let f: Vec<u64> = setup.split_whitespace().skip(1).map(|v| v.parse().map_err(|_| format!("bad SETUP field {v}"))).collect::<Result<_, _>>()?;
    let size = stride as u64 * h as u64 * 4;
    let mut slots: Vec<(wgpu::Buffer, ID3D12Fence)> = vec![];
    for pair in f.chunks(2) {
        let (mut res, mut fence): (Option<ID3D12Resource>, Option<ID3D12Fence>) = (None, None);
        // SAFETY: the producer duplicated these handles into this process.
        unsafe {
            raw.OpenSharedHandle(HANDLE(pair[0] as *mut _), &mut res).map_err(|e| format!("OpenSharedHandle(buffer): {e}"))?;
            raw.OpenSharedHandle(HANDLE(pair[1] as *mut _), &mut fence).map_err(|e| format!("OpenSharedHandle(fence): {e}"))?;
        }
        let (res, fence) = (res.ok_or("no buffer")?, fence.ok_or("no fence")?);
        // SAFETY: the resource is a buffer of `size` bytes the producer made.
        let buffer = unsafe {
            let hal = wgpu::hal::dx12::Device::buffer_from_raw(res, size);
            dev.create_buffer_from_hal::<Dx12>(hal, &wgpu::BufferDescriptor { label: None, size, usage: wgpu::BufferUsages::STORAGE, mapped_at_creation: false })
        };
        slots.push((buffer, fence));
    }
    // SAFETY: plain query on a live device.
    let luid = unsafe { raw.GetAdapterLuid() };
    println!("READY {}", (u64::from(luid.HighPart as u32) << 32) | u64::from(luid.LowPart));
    let module = dev.create_shader_module(wgpu::ShaderModuleDescriptor { label: None, source: wgpu::ShaderSource::Wgsl(shader_sum(w, stride).into()) });
    let pipe = dev.create_compute_pipeline(&wgpu::ComputePipelineDescriptor { label: None, layout: None, module: &module, entry_point: Some("main"), compilation_options: Default::default(), cache: None });
    let res = dev.create_buffer(&wgpu::BufferDescriptor { label: None, size: 16, usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false });
    let rb = dev.create_buffer(&wgpu::BufferDescriptor { label: None, size: 16, usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false });
    let binds: Vec<wgpu::BindGroup> = slots
        .iter()
        .map(|(b, _)| {
            dev.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None,
                layout: &pipe.get_bind_group_layout(0),
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: b.as_entire_binding() }, wgpu::BindGroupEntry { binding: 1, resource: res.as_entire_binding() }],
            })
        })
        .collect();
    for line in lines {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.first() == Some(&"STOP") {
            break;
        }
        let n = |i: usize| -> Result<u64, String> { f.get(i).and_then(|v| v.parse().ok()).ok_or(format!("bad line {line}")) };
        let (slot, seq, value, want, wait) = (n(1)? as usize, n(2)?, n(3)?, n(4)?, n(5)? == 1);
        if wait {
            // SAFETY: the hal queue borrow ends at the end of the statement.
            unsafe { queue.as_hal::<Dx12>().ok_or("not D3D12")?.add_wait_fence(slots[slot].1.clone(), value) };
        }
        let mut enc = dev.create_command_encoder(&Default::default());
        enc.clear_buffer(&res, 0, None);
        {
            let mut pass = enc.begin_compute_pass(&Default::default());
            pass.set_pipeline(&pipe);
            pass.set_bind_group(0, &binds[slot], &[]);
            pass.dispatch_workgroups(h, 1, 1);
        }
        enc.copy_buffer_to_buffer(&res, 0, &rb, 0, 16);
        queue.submit([enc.finish()]);
        let sl = rb.slice(..);
        sl.map_async(wgpu::MapMode::Read, |_| {});
        dev.poll(wgpu::PollType::Wait { submission_index: None, timeout: None }).map_err(|e| format!("poll: {e:?}"))?;
        let got = u32::from_le_bytes(sl.get_mapped_range().map_err(|e| format!("map: {e:?}"))?[0..4].try_into().unwrap());
        rb.unmap();
        println!("DONE {slot} {seq} {}", (got as u64 == want) as u8);
    }
    Ok(())
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let num = |i: usize, d: u32| a.get(i).and_then(|v| v.parse().ok()).unwrap_or(d);
    if a.get(1).map(String::as_str) == Some("viewer") {
        if let Err(e) = viewer(num(2, 0), num(3, 0), num(4, 0)) {
            println!("ERR {e}");
            std::process::exit(2);
        }
        return;
    }
    let gpu_wait = a.get(4).map(String::as_str) != Some("nowait");
    match producer(num(1, 1280), num(2, 1024), num(3, 300), gpu_wait, num(5, 0)) {
        Ok(line) => {
            println!("{line}");
            if gpu_wait && !line.contains("\"mismatch\":0,") {
                std::process::exit(1);
            }
        }
        Err(e) => {
            println!("{{\"ok\":false,\"error\":{e:?}}}");
            std::process::exit(2);
        }
    }
}
