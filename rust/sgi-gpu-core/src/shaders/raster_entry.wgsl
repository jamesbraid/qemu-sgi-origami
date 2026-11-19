// SPDX-License-Identifier: BSD-3-Clause
// Ported from src/wgpu/shaders/raster_entry.wgsl in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
// The raster pass's entry point where the device has no subgroup
// operations: the lane is the invocation's index in the workgroup.

@compute @workgroup_size(64)
fn raster(@builtin(workgroup_id) wg: vec3<u32>, @builtin(local_invocation_index) index: u32) {
    raster_tile(wg, index);
}
