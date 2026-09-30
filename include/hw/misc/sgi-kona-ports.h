/* SPDX-License-Identifier: GPL-2.0-or-later */
/* InfiniteReality command-port roles and selector tables. */
#ifndef HW_MISC_SGI_KONA_PORTS_H
#define HW_MISC_SGI_KONA_PORTS_H

#include "hw/misc/sgi-command-port.h"

/*
 * Data only: sgi-kona.c initializes a generic command-port framer for each
 * role. The model maps the textport at +0xc10000 with mailbox and interrupt
 * cause registers. FIFO and doorbell MMIO placement remains unknown.
 *
 * The kernel FIFO push port has candidate offsets +0x7c and +0x80007c. The
 * low-window +0x7c write is modeled separately. Textport writes begin with
 * sync word 1, followed by a command and its arguments; the owner strips the
 * sync word before framing the command.
 *
 * The userspace doorbell is modeled at guest VA 0x4000. Candidate device
 * windows include +0x80000, +0x810000, and +0x10000. The +0x10000 candidate
 * is unresolved.
 *
 * The following selectors are omitted because their count or word order is
 * unresolved. Omission does not imply that real hardware ignores them:
 * - FIFO 0x1fd places its selector after a literal and another argument.
 * - FIFO 0x1f3 and 0x172 have conflicting argument counts across uses.
 * - FIFO 0x100 also appears on an indirect register pair.
 * - FIFO 0x1eb has no established argument count.
 * - FIFO 0x1ee, 0x1f9, and 0x1f2 have unknown word order.
 * - Doorbell 0x0, 0x500, 0x1a-0x1f, 0x8c, 0x90, and 0x39 have unknown
 *   counts.
 * - Doorbell 0x119 has inconsistent argument evidence; 0x1d6 and 0x1ca
 *   each have one word in the maintained table.
 * - Textport 0xa is a variable-length bitmap transfer and is framed by its
 *   owner outside the fixed-count command-port path.
 */
typedef enum SGIKonaPort {
    SGI_KONA_PORT_FIFO,
    SGI_KONA_PORT_TEXTPORT,
    SGI_KONA_PORT_DOORBELL,
} SGIKonaPort;

/* Five fixed-count, selector-first kernel FIFO entries at +0x7c. */
extern const SGICommandPortSelector sgi_kona_fifo_selectors[];
extern const unsigned sgi_kona_fifo_num_selectors;

/* Textport +0xc10000 selectors follow sync word 1, stripped by the owner. */
extern const SGICommandPortSelector sgi_kona_textport_selectors[];
extern const unsigned sgi_kona_textport_num_selectors;

/* OpenGL 3D doorbell selectors at guest VA 0x4000. */
extern const SGICommandPortSelector sgi_kona_doorbell_selectors[];
extern const unsigned sgi_kona_doorbell_num_selectors;

#endif /* HW_MISC_SGI_KONA_PORTS_H */
