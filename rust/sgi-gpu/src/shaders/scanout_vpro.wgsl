// SPDX-License-Identifier: BSD-3-Clause
// VPro scan-out compose, one lane per pixel: word for word odyssey_resolve_row() (0x00RRGGBB).
// Planes hold 0xRRGGBBAA; ids are the id and overlay bytes packed four to a word.

// Offsets into `tab`, in words: params, lookup RAM, display modes, overlay
// modes, cursor entries.
const T_PARAMS: u32 = 0u;
const T_LUT: u32 = 16u;
const T_DISPMODE: u32 = 8208u;    // 16 + 0x2000
const T_OVERLAY: u32 = 8240u;     // + 32
const T_CURSOR: u32 = 8248u;      // + 8

// params: width, height, frame stride (words), displayed buffer bits,
// plane stride (words), id stride (bytes).
const P_WIDTH: u32 = 0u;
const P_HEIGHT: u32 = 1u;
const P_STRIDE: u32 = 2u;
const P_DISPLAYED: u32 = 3u;
const P_PLANE_STRIDE: u32 = 4u;
const P_ID_STRIDE: u32 = 5u;

const GAMMA_BASE: u32 = 0x1a00u;
const GAMMA_GROUP: u32 = 26u;

@group(0) @binding(0) var<storage, read> tab: array<u32>;
@group(0) @binding(1) var<storage, read_write> frame: array<u32>;
@group(0) @binding(2) var<storage, read> buf0: array<u32>;
@group(0) @binding(3) var<storage, read> buf1: array<u32>;
@group(0) @binding(4) var<storage, read> ids: array<u32>;
@group(0) @binding(5) var<storage, read> ovl: array<u32>;

fn id_byte(i: u32) -> u32 {
    return (ids[i >> 2u] >> ((i & 3u) * 8u)) & 0xffu;
}

fn overlay_byte(i: u32) -> u32 {
    return (ovl[i >> 2u] >> ((i & 3u) * 8u)) & 0xffu;
}

// A lookup-RAM entry's ten-bit channels as the top eight bits of each.
fn rgb_word(word: u32) -> u32 {
    return (((word >> 22u) & 0xffu) << 16u) | (((word >> 12u) & 0xffu) << 8u) | ((word >> 2u) & 0xffu);
}

fn lut_rgb(entry: u32) -> u32 {
    if entry >= 0x2000u { return 0u; }
    return rgb_word(tab[T_LUT + entry]);
}

fn gamma(c: u32, shift: u32) -> u32 {
    let v = (c << 4u) | (c >> 4u);
    var t: u32;
    if v < 0x200u {
        t = v;
    } else if v < 0x400u {
        t = v / 2u + 0x100u;
    } else {
        t = v / 4u + 0x200u;
    }
    return (tab[T_LUT + GAMMA_BASE + t] >> shift) & 0xffu;
}

// A narrow visual's channel keeps its top bits, repeated down to fill the byte.
fn display_channel(mode: u32, c: u32) -> u32 {
    let w = (mode >> 9u) & 7u;
    if w == 4u { return (c & 0xf0u) | (c >> 4u); }
    if w == 3u { return (c & 0xf8u) | (c >> 5u); }
    return c;
}

fn cursor_row(x: u32, y: u32, width: u32, rgb: u32) -> u32 {
    let ctl = tab[T_CURSOR + 0x81u];
    if (ctl & 1u) == 0u || (ctl & 0x20u) != 0u { return rgb; }
    let pos = tab[T_CURSOR + 0x80u];
    let cx = bitcast<i32>(((pos >> 16u) ^ 0x8000u) - 0x8000u);
    let cy = bitcast<i32>(((pos & 0xffffu) ^ 0x8000u) - 0x8000u);
    let row = i32(y) - cy;
    if (ctl & 4u) != 0u {
        let colour = rgb_word(tab[T_CURSOR + 0x90u + 1u]);
        if row == 0 { return colour; }
        if cx >= 0 && cx < i32(width) && i32(x) == cx { return colour; }
        return rgb;
    }
    let col = i32(x) - cx;
    if row < 0 || row >= 32 || col < 0 || col >= 32 { return rgb; }
    let ur = u32(row);
    let uc = u32(col);
    let plane = (tab[T_CURSOR + 0xc0u + ur * 2u + uc / 16u] >> ((uc % 16u) * 2u)) & 3u;
    let glyph = (tab[T_CURSOR + ur * 4u + uc / 8u] >> ((uc % 8u) * 4u)) & 0xfu;
    if plane != 3u || glyph == 0xfu { return rgb; }
    return rgb_word(tab[T_CURSOR + 0x90u + glyph]);
}

@compute @workgroup_size(8, 8)
fn scanout(@builtin(global_invocation_id) g: vec3<u32>) {
    let width = tab[T_PARAMS + P_WIDTH];
    let height = tab[T_PARAMS + P_HEIGHT];
    if g.x >= width || g.y >= height { return; }
    let x = g.x;
    let y = g.y;
    let at = y * tab[T_PARAMS + P_ID_STRIDE] + x;
    let did = id_byte(at);
    let opixel = overlay_byte(at);
    let main_id = (did >> 3u) & 31u;
    let omode = tab[T_OVERLAY + (did & 7u)];
    let pat = y * tab[T_PARAMS + P_PLANE_STRIDE] + x;
    var rgb: u32;
    if (omode & 1u) != 0u && opixel != 0u {
        rgb = lut_rgb(256u * ((omode >> 3u) & 31u) + opixel);
    } else {
        var word: u32;
        if ((tab[T_PARAMS + P_DISPLAYED] >> main_id) & 1u) != 0u {
            word = buf1[pat];
        } else {
            word = buf0[pat];
        }
        let mode = tab[T_DISPMODE + main_id];
        let group = (mode >> 4u) & 31u;
        if group >= GAMMA_GROUP {
            rgb = (gamma(display_channel(mode, word >> 24u), 22u) << 16u)
                | (gamma(display_channel(mode, (word >> 16u) & 0xffu), 12u) << 8u)
                | gamma(display_channel(mode, (word >> 8u) & 0xffu), 2u);
        } else if group != 0u {
            rgb = lut_rgb(256u * group + (word & 0xffu));
        } else {
            rgb = lut_rgb(word & 0xfffu);
        }
    }
    frame[y * tab[T_PARAMS + P_STRIDE] + x] = cursor_row(x, y, width, rgb);
}
