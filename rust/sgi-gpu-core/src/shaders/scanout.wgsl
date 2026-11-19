// SPDX-License-Identifier: BSD-3-Clause
// Ported from src/wgpu/shaders/scanout.wgsl in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
// The scanout pass driver: one lane per screen pixel, the board's
// `scanout_pixel(x, y) -> u32` (0x00RRGGBB) into a frame of `stride`
// pixels a row, stored as 0xAABBGGRR with alpha set: red in the low byte,
// the order of the CPU compositors' frames and of the window's texture, so
// nothing between the kernel and the screen swaps bytes. Group 0 is the frame; group 1 binding 0 is the board's
// display slot, declared by the board's fragment; groups 2 and up are the
// board's memory, bound as its raster kernels bind it.

struct ScanoutHeader {
    width: u32,
    height: u32,
    stride: u32,
    pad: u32,
}

@group(0) @binding(0) var<uniform> scan: ScanoutHeader;
@group(0) @binding(1) var<storage, read_write> scan_out: array<u32>;

@compute @workgroup_size(8, 8)
fn scanout(@builtin(global_invocation_id) g: vec3<u32>) {
    if g.x >= scan.width || g.y >= scan.height {
        return;
    }
    let p = scanout_pixel(g.x, g.y);
    scan_out[g.y * scan.stride + g.x] = 0xff000000u | ((p & 0xffu) << 16u) | (p & 0xff00u) | ((p >> 16u) & 0xffu);
}
