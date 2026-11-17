// SPDX-License-Identifier: BSD-3-Clause
// VPro flat triangle and clear kernels on a one-word-per-pixel plane: exact 64-bit edge tests at
// 1/256 pixel with sgi_soft_owns' tie rule; later triangles win; stores merge under the write mask.
//
// Job words: count, width, height, 0, then the box x0 y0 x1 y1, then records
// of 16 words: x0 x1 x2, y0 y1 y2, colour, mask, clip x0 y0 x1 y1, 3 spare.
// A clear's record: colour, mask.

#include "i64.wgsl"

@group(0) @binding(0) var<storage, read> job: array<u32>;
@group(0) @binding(1) var<storage, read_write> plane: array<u32>;

const HEADER: u32 = 8u;
const RECORD: u32 = 16u;

fn ji(i: u32) -> i32 {
    return bitcast<i32>(job[i]);
}

// Whether edge a to b owns the pixel centres on it.
fn owns(ax: i32, ay: i32, bx: i32, by: i32) -> bool {
    return by < ay || (by == ay && bx < ax);
}

// Whether the edge a to b keeps the centre (cx, cy): a positive value, or
// zero on an edge that owns its centres.
fn keeps(ax: i32, ay: i32, bx: i32, by: i32, cx: i32, cy: i32) -> bool {
    let e = i64_sub(i64_mul_i32(bx - ax, cy - ay), i64_mul_i32(by - ay, cx - ax));
    if i64_is_neg(e) {
        return false;
    }
    if (e.lo | e.hi) != 0u {
        return true;
    }
    return owns(ax, ay, bx, by);
}

@compute @workgroup_size(8, 8)
fn tris(@builtin(global_invocation_id) gid: vec3<u32>) {
    let px = ji(4u) + i32(gid.x);
    let py = ji(5u) + i32(gid.y);
    if px > ji(6u) || py > ji(7u) {
        return;
    }
    let cx = px * 256 + 128;
    let cy = py * 256 + 128;
    var word = 0u;
    var hit = false;
    for (var i = 0u; i < job[0]; i++) {
        let r = HEADER + i * RECORD;
        if px < ji(r + 8u) || py < ji(r + 9u) || px > ji(r + 10u) || py > ji(r + 11u) {
            continue;
        }
        let x0 = ji(r);
        let x1 = ji(r + 1u);
        let x2 = ji(r + 2u);
        let y0 = ji(r + 3u);
        let y1 = ji(r + 4u);
        let y2 = ji(r + 5u);
        if keeps(x1, y1, x2, y2, cx, cy) && keeps(x2, y2, x0, y0, cx, cy) && keeps(x0, y0, x1, y1, cx, cy) {
            if !hit {
                word = plane[u32(py) * job[1] + u32(px)];
                hit = true;
            }
            let mask = job[r + 7u];
            word = (word & ~mask) | (job[r + 6u] & mask);
        }
    }
    if hit {
        plane[u32(py) * job[1] + u32(px)] = word;
    }
}

@compute @workgroup_size(8, 8)
fn clear(@builtin(global_invocation_id) gid: vec3<u32>) {
    let px = ji(4u) + i32(gid.x);
    let py = ji(5u) + i32(gid.y);
    if px > ji(6u) || py > ji(7u) {
        return;
    }
    let at = u32(py) * job[1] + u32(px);
    let mask = job[HEADER + 1u];
    plane[at] = (plane[at] & ~mask) | (job[HEADER] & mask);
}
