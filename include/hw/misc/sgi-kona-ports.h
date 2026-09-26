/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Data only: no functions, MemoryRegion or reset phase. The generic
 * command-port framer consumes the roles and selector tables declared here.
 * At this revision, the identity-only device has no FIFO, mailbox or
 * interrupt. All three port MMIO placements remain deferred.
 *
 * The kernel FIFO has candidate offsets +0x7c and +0x80007c. The IO6
 * firmware's one-time low-window +0x7c write is a separate operation. FIFO
 * placement is unresolved. Textport commands at +0xc10000 begin with sync
 * word 1, then command code and arguments. The owner strips the sync word
 * before passing the command to the selector-first framer.
 *
 * The userspace doorbell is at guest-process VA 0x4000. Candidate device
 * windows are +0x80000, +0x810000 and +0x10000. A +0x10000 base is
 * suggestive but unestablished; no doorbell aperture is selected here.
 *
 * The following selectors are omitted because their fixed argument count
 * or selector-first order is not established. Omission does not imply
 * that real hardware ignores them:
 * - FIFO 0x1fd puts its selector last, after literal 1 and a polled/read
 *   argument, which is incompatible with this selector-first framer.
 * - FIFO 0x1f3 has conflicting counts: four words or no arguments. FIFO
 *   0x172 has two halfword-suffixed fields or no arguments across uses.
 * - FIFO 0x100 also appears on the separate +0x8003c0/+0x8003d0 indirect
 *   register pair. Its 0x100-0x105 block is known, but no count is
 *   confirmed for this port.
 * - FIFO 0x1eb is inferred to be DMA-related; its count is unestablished.
 * - FIFO 0x1ee, 0x1f9 and 0x1f2 have stated words and counts, but unknown
 *   selector/argument order. They remain excluded pending confirmation.
 * - Doorbell 0x0 or 0x500, 0x1a-0x1f, 0x8c, 0x90 and 0x39 have unknown
 *   argument counts.
 * - Doorbell 0x119 has no shown argument in one use; the related 0x1d6
 *   and 0x1ca entries each state one word. Its evidence is inconsistent.
 * - Textport 0xa takes six sign-extended halfwords and a packed 1-bpp
 *   bitmap. The owner frames this variable-length transfer outside the
 *   fixed-count sgi_command_port_write() path.
 *
 * Retained raw captures can under-report packet length. Table counts use
 * checked prose rather than raw captures alone. Incomplete captures cannot
 * establish selector-last order for excluded entries. Unknown counts and
 * word order remain unknown.
 */
#ifndef HW_MISC_SGI_KONA_PORTS_H
#define HW_MISC_SGI_KONA_PORTS_H

#include "hw/misc/sgi-command-port.h"

typedef enum SGIKonaPort {
    SGI_KONA_PORT_FIFO,
    SGI_KONA_PORT_TEXTPORT,
    SGI_KONA_PORT_DOORBELL,
} SGIKonaPort;

/*
 * Five fixed-count, selector-first kernel FIFO entries. See the unresolved
 * FIFO placement and excluded-selector limitations above.
 */
extern const SGICommandPortSelector sgi_kona_fifo_selectors[];
extern const unsigned sgi_kona_fifo_num_selectors;

/*
 * Textport +0xc10000 selectors follow sync word 1. The owner strips that
 * word before framing the command.
 */
extern const SGICommandPortSelector sgi_kona_textport_selectors[];
extern const unsigned sgi_kona_textport_num_selectors;

/*
 * OpenGL doorbell selectors at guest-process VA 0x4000. The backing
 * device aperture remains unresolved.
 */
extern const SGICommandPortSelector sgi_kona_doorbell_selectors[];
extern const unsigned sgi_kona_doorbell_num_selectors;

#endif /* HW_MISC_SGI_KONA_PORTS_H */
