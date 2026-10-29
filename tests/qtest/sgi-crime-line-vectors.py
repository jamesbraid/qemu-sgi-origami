#!/usr/bin/env -S uv run --script
# SPDX-License-Identifier: BSD-3-Clause
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Regenerate independent GL line fixtures from rational and Decimal geometry."""
from decimal import Decimal as D, localcontext
from fractions import Fraction as F
from pathlib import Path

S = 1 << 20
EPS = F(1, 1 << 96)
WIDTHS = (0, 1, 15, 16, 17, 31, 32, 48, 64, 65, 65535)
DIRECTIONS = ((5, 0), (5, 2), (2, 5), (0, 5), (-2, 5), (-5, 2),
              (-5, 0), (-5, -2), (-2, -5), (0, -5), (2, -5),
              (5, -2), (5, 5), (-5, 5), (-5, -5), (5, -5))


def nonaa(a, b, halfwidth):
    if a == b:
        return [0] * 16
    width = max(1, (halfwidth + 16) // 32)
    xmajor = abs(b[0] - a[0]) >= abs(b[1] - a[1])
    ca = [F(a[0], 64) + F(1, 2), F(1, 2) - F(a[1], 64)]
    cb = [F(b[0], 64) + F(1, 2), F(1, 2) - F(b[1], 64)]
    minor = 1 if xmajor else 0
    ca[minor] -= F(width - 1, 2)
    cb[minor] -= F(width - 1, 2)
    ca[0] -= EPS
    cb[0] -= EPS
    ca[1] -= EPS * EPS
    cb[1] -= EPS * EPS
    result = set()
    # Finite fixture coordinates/denominators bound every nonzero boundary
    # separation above 2^-60; this exact rational perturbation selects ties only.
    xs = range(int(min(ca[0], cb[0])) - 3, int(max(ca[0], cb[0])) + 4)
    ys = range(int(-max(ca[1], cb[1])) - 3, int(-min(ca[1], cb[1])) + 4)
    for y in ys:
        for x in xs:
            center = (F(x) + F(1, 2), F(1, 2) - y)
            if sum(abs(cb[k] - center[k]) for k in (0, 1)) < F(1, 2):
                continue
            lower, upper = F(0), F(1)
            valid = True
            for sx, sy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
                initial = sx * (ca[0] - center[0]) + sy * (ca[1] - center[1])
                delta = sx * (cb[0] - ca[0]) + sy * (cb[1] - ca[1])
                if not delta:
                    if initial >= F(1, 2):
                        valid = False
                        break
                elif delta > 0:
                    upper = min(upper, (F(1, 2) - initial) / delta)
                else:
                    lower = max(lower, (F(1, 2) - initial) / delta)
            if valid and lower < upper:
                for offset in range(width):
                    pixel = (x, y - offset) if xmajor else (x + offset, y)
                    if 0 <= pixel[0] < 16 and 0 <= pixel[1] < 16:
                        result.add(pixel)
    return [sum(1 << x for x in range(16) if (x, y) in result) for y in range(16)]


def endpoints(a, b, origin, slope, ideal, cs, ce):
    major = 0 if abs(b[0] - a[0]) >= abs(b[1] - a[1]) else 1
    minor = 1 - major
    direction = 1 if b[major] > a[major] else -1
    k0 = direction * ((direction * a[major] + 32) // 64)
    k1 = direction * (-(-(direction * b[major] + 32) // 64) - 1)
    start = 64 * k0 + direction * (32 - cs)
    end = 64 * k1 + direction * (ce - 32)
    if direction * (end - start) <= 0:
        return None
    reference = a[minor] * 16384 + slope * (64 * k0 - a[major])
    base = 64 * (ideal + 16384 * origin[minor])
    k, rem = divmod(reference - base, 4194304)
    k += 2 * rem > 4194304
    j14 = ideal + 16384 * origin[minor] + 65536 * k
    p = [0, 0]
    q = [0, 0]
    p[major], q[major] = 16384 * start, 16384 * end
    p[minor] = 64 * j14 + slope * (start - 64 * k0)
    q[minor] = 64 * j14 + slope * (end - 64 * k0)
    return tuple(p), tuple(q)


def area_reference(p, q, pixel):
    # Construct the perpendicular rectangle at 90 decimal digits, then clip
    # against the axis-aligned pixel. Production instead clips the square
    # against primitive-direction planes and adds a triangle fan.
    with localcontext() as context:
        context.prec = 90
        a = tuple(D(c) / S for c in p)
        b = tuple(D(c) / S for c in q)
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = (dx * dx + dy * dy).sqrt()
        nx, ny = -dy / length, dx / length
        polygon = [(a[0] + nx, a[1] + ny), (b[0] + nx, b[1] + ny),
                   (b[0] - nx, b[1] - ny), (a[0] - nx, a[1] - ny)]
        for axis, side in ((0, -1), (0, 1), (1, -1), (1, 1)):
            if not polygon:
                return D(0)
            boundary = D(pixel[axis]) + D(side) / 2
            clipped = []
            previous = polygon[-1]
            fp = (previous[axis] - boundary) * -side
            for current in polygon:
                fc = (current[axis] - boundary) * -side
                if (fp >= 0) != (fc >= 0):
                    ratio = fp / (fp - fc)
                    clipped.append(tuple(previous[k] + ratio *
                                         (current[k] - previous[k]) for k in (0, 1)))
                if fc >= 0:
                    clipped.append(current)
                previous, fp = current, fc
            polygon = clipped
        if len(polygon) < 3:
            return D(0)
        value = abs(sum(polygon[i][0] * polygon[(i + 1) % len(polygon)][1] -
                        polygon[i][1] * polygon[(i + 1) % len(polygon)][0]
                        for i in range(len(polygon)))) / 2
        return min(D(1), value)


def generate(destination):
    output = ["/* SPDX-License-Identifier: BSD-3-Clause */",
              "/* Generated by sgi-crime-line-vectors.py; public geometric rules only. */",
              "#ifndef SGI_CRIME_LINE_VECTORS_H", "#define SGI_CRIME_LINE_VECTORS_H", "",
              "typedef struct GLNonAAVector {", "    int a[2], b[2];",
              "    unsigned halfwidth;", "    uint16_t rows[16];", "} GLNonAAVector;", "",
              "static const GLNonAAVector gl_nonaa_vectors[] = {"]
    vectors = []
    for direction in DIRECTIONS:
        for reverse in (False, True):
            for width in WIDTHS:
                a = (8 * 64, 8 * 64)
                b = tuple(a[k] + 64 * direction[k] for k in (0, 1))
                if reverse:
                    a, b = b, a
                vectors.append((a, b, width))
            for fraction in ((31, 32), (32, 31), (1, 63), (63, 1)):
                a = tuple(8 * 64 + fraction[k] for k in (0, 1))
                b = (a[0] + 64 * direction[0] + 17,
                     a[1] + 64 * direction[1] - 13)
                if reverse:
                    a, b = b, a
                for width in (32, 48):
                    vectors.append((a, b, width))
    vectors += [((512, 512), (512, 512), w) for w in WIDTHS]
    for a, b, width in vectors:
        rows = nonaa(a, b, width)
        output += [f"    {{ {{ {a[0]}, {a[1]} }}, {{ {b[0]}, {b[1]} }}, {width}, {{",
                   "        " + ", ".join(f"0x{r:04x}" for r in rows[:8]) + ",",
                   "        " + ", ".join(f"0x{r:04x}" for r in rows[8:]) + ",",
                   "    } },"]
    output += ["};", "", "typedef struct GLAAVector {", "    int a[2], b[2], origin[2];",
               "    int slope;", "    unsigned ideal, cs, ce;", "    int x, y;",
               "    double area;", "    unsigned encoded;", "    bool boundary;",
               "} GLAAVector;", "", "static const GLAAVector gl_aa_vectors[] = {"]
    packets = []
    for slope in (-16384, -9458, -8192, -1, 0, 1, 8192, 9458, 16384):
        for transpose in (False, True):
            a = (144, 224)
            b = (496, 224 + slope * 352 // 16384)
            if transpose:
                a, b = (a[1], a[0]), (b[1], b[0])
            major = 0 if abs(b[0] - a[0]) >= abs(b[1] - a[1]) else 1
            minor = 1 - major
            k0 = (a[major] + 32) // 64
            ideal = (a[minor] * 256 + slope * (64 * k0 - a[major]) // 64) % 65536
            for caps in ((1, 64), (32, 32), (64, 1), (64, 64)):
                packets.append((a, b, (-4096, -4096), slope, ideal, *caps))
    packets += [((0, 96), (192, 96), (0, 0), 0, r, 32, 32)
                for r in (24511, 24512, 24513, 24573, 24575, 24576, 24577)]
    packets += [((0, 0), (192, -111), (0, -4), -9458, 0, 32, 32),
                ((192, -111), (0, 0), (0, -4), -9458, 37162, 32, 32),
                ((6, 32), (13, 32), (-4, -4), 8192, 8192, 64, 1),
                ((32, 32), (96, 64), (-4, -4), 8192, 12288, 64, 64)]
    count = 0
    for a, b, origin, slope, ideal, cs, ce in packets:
        ends = endpoints(a, b, origin, slope, ideal, cs, ce)
        if ends is None:
            continue
        p, q = ends
        points = {(0, 0), (1, 0)}
        for endpoint in (p, q, ((p[0] + q[0]) // 2, (p[1] + q[1]) // 2)):
            center = (endpoint[0] // S, endpoint[1] // S)
            points.update((center[0] + dx, center[1] + dy)
                          for dx, dy in ((0, 0), (-1, 0), (0, -1), (1, 1)))
        for x, y in sorted(points):
            area = area_reference(p, q, (x, y))
            encoded = min(255, int(area * 256))
            boundary = min(abs(area * 256 - (area * 256).to_integral_value()),
                           D(1)) <= D(256) / (1 << 32)
            output += [f"    {{ {{ {a[0]}, {a[1]} }}, {{ {b[0]}, {b[1]} }},",
                       f"      {{ {origin[0]}, {origin[1]} }}, "
                       f"{slope}, {ideal}, {cs}, {ce},",
                       f"      {x}, {y}, {float(area).hex()}, {encoded}, " +
                       ("true" if boundary else "false") + " },"]
            count += 1
    output += ["};", "", "#endif", ""]
    destination.write_text("\n".join(output))
    print(f"{len(vectors)} non-AA vectors; {count} high-precision AA samples")


if __name__ == "__main__":
    generate(Path(__file__).with_name("sgi-crime-line-vectors.h"))
