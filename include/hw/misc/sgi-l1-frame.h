/* SPDX-License-Identifier: BSD-3-Clause */
/* The SGI L1 packet framing: flags, escapes and a CCITT check. */
#ifndef HW_MISC_SGI_L1_FRAME_H
#define HW_MISC_SGI_L1_FRAME_H

/* 256 payload bytes plus the two check bytes: the firmware's receive cap. */
#define SGI_L1_FRAME_SIZE 258

typedef struct SGIL1FrameState {
    uint8_t data[SGI_L1_FRAME_SIZE];
    uint32_t length;
    bool in_frame;
    bool escape;
    bool overflow;
} SGIL1FrameState;

/*
 * True when @byte closed a frame with a good check: it is data[0..length),
 * check bytes included, until the caller calls sgi_l1_frame_restart().
 * @capacity is at most SGI_L1_FRAME_SIZE.
 */
bool sgi_l1_frame_parse(SGIL1FrameState *f, unsigned capacity, uint8_t byte);
void sgi_l1_frame_restart(SGIL1FrameState *f);

/*
 * Encode into @out, 2 * (length + 2) + 2 bytes. CR is escaped on the
 * multiplexed serial link only.
 */
unsigned sgi_l1_frame_encode(uint8_t *out, const uint8_t *payload,
                             unsigned length, bool escape_cr);

/* Reject malformed typed arguments before any command changes state. */
bool sgi_l1_arguments_valid(const uint8_t *p, unsigned length,
                            unsigned count);

#endif
