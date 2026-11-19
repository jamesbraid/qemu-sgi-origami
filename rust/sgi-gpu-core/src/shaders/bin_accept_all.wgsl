// SPDX-License-Identifier: BSD-3-Clause
// Ported from src/wgpu/shaders/bin_accept_all.wgsl in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
// The default bin hook: every tile of the box.
fn bin_accept(p: u32, tx0: u32, ty0: u32, tx1: u32, ty1: u32) -> bool {
    return true;
}
