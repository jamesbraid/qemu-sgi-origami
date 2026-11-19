// SPDX-License-Identifier: BSD-3-Clause
// Ported from crates/sgi-gpu-core/build.rs in jamesbraid/iris, commit 7d772b436 (tag qemu-pin-20261118-core).
// Marks a build of the crate itself, as opposed to iris compiling the same files.
fn main() {
    println!("cargo::rustc-check-cfg=cfg(sgi_gpu_core)");
    println!("cargo::rustc-cfg=sgi_gpu_core");
}
