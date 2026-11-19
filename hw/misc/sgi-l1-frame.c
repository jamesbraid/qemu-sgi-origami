/* SPDX-License-Identifier: BSD-3-Clause */
/* The SGI L1 packet framing shared by the controller models. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1-frame.h"
#include "qemu/crc-ccitt.h"

void sgi_l1_frame_restart(SGIL1FrameState *f)
{
    f->in_frame = true;
    f->length = 0;
    f->escape = false;
    f->overflow = false;
}

bool sgi_l1_frame_parse(SGIL1FrameState *f, unsigned capacity, uint8_t byte)
{
    assert(capacity <= SGI_L1_FRAME_SIZE);
    if (byte == 0x7e) {
        /* A header and its check: the PROM's one-character echo is four. */
        if (f->in_frame && !f->overflow && !f->escape && f->length >= 3 &&
            crc_ccitt(0xffff, f->data, f->length) == 0xf0b8) {
            return true;
        }
        sgi_l1_frame_restart(f);
        return false;
    }
    if (!f->in_frame || f->overflow) {
        return false;
    }
    if (byte == 0x7d) {
        /* A second escape only re-arms the first. */
        f->escape = true;
        return false;
    }
    if (f->escape) {
        byte ^= 0x20;
        f->escape = false;
    }
    if (f->length >= capacity) {
        f->overflow = true;
        return false;
    }
    f->data[f->length++] = byte;
    return false;
}

unsigned sgi_l1_frame_encode(uint8_t *out, const uint8_t *payload,
                             unsigned length, bool escape_cr)
{
    uint16_t crc = ~crc_ccitt(0xffff, payload, length);
    unsigned n = 0;

    out[n++] = 0x7e;
    for (unsigned i = 0; i < length + 2; i++) {
        uint8_t byte = i < length ? payload[i] :
                       i == length ? crc : crc >> 8;
        /* Selected serial-multiplex startup escapes CR, but not XON/XOFF. */
        if (byte == 0x7e || byte == 0x7d || (byte == 0x0d && escape_cr)) {
            out[n++] = 0x7d;
            byte ^= 0x20;
        }
        out[n++] = byte;
    }
    out[n++] = 0x7e;
    return n;
}

bool sgi_l1_arguments_valid(const uint8_t *p, unsigned length, unsigned count)
{
    unsigned offset = 0;

    for (unsigned i = 0; i < count; i++) {
        uint8_t tag;
        if (offset == length) {
            return false;
        }
        tag = p[offset++];
        if (tag == 0) {
            if (length - offset < 4) {
                return false;
            }
            offset += 4;
        } else if (tag == 1) {
            const uint8_t *end = memchr(p + offset, 0, length - offset);
            if (!end) {
                return false;
            }
            offset = end - p + 1;
        } else if (tag & 0x80) {
            if (length - offset < (tag & 0x7f)) {
                return false;
            }
            offset += tag & 0x7f;
        } else {
            return false;
        }
    }
    return offset == length;
}
