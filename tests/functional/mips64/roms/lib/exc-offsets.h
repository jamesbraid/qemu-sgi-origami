/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Byte offsets into the exception record, shared by the assembly exception
 * handler and the C tests that read it. rom.h declares the matching C struct
 * and checks every offset against this file at compile time, so the two views
 * cannot drift apart silently.
 *
 * Every slot is eight bytes, including the ones holding 32-bit registers, so
 * that the handler can use one load and one store width throughout.
 */

#ifndef ROM_EXC_OFFSETS_H
#define ROM_EXC_OFFSETS_H

/* Exceptions taken since the last reset. */
#define EXC_COUNT       0
#define EXC_VECTOR      8       /* which entry point ran; ROM_VEC_* below */
#define EXC_RESUME      16      /* where to continue; cleared once it is used */
#define EXC_CAUSE       24
#define EXC_STATUS      32
#define EXC_EPC         40
#define EXC_BADVADDR    48
#define EXC_CONTEXT     56
#define EXC_XCONTEXT    64
#define EXC_ENTRYHI     72
#define EXC_ERROREPC    80
/* Nonzero: refill the TLB and retry the access. */
#define EXC_REFILL      88
#define EXC_ENTRYLO0    96      /* the even-page half written by that refill */
#define EXC_ENTRYLO1    104     /* the odd-page half written by that refill */
#define EXC_PAGEMASK    112     /* the page size written by that refill */
/* Status to install before resuming, if nonzero. */
#define EXC_STATUS_SET  120
#define EXC_JUMP        128     /* nonzero: resume by a jump, not an ERET */
#define EXC_SIZE        136

/*
 * Vector identifiers. The low two bits say which of the four architectural
 * entry points ran; bit 2 says whether it was reached with the
 * boot-exception-vector bit of the Status register set (the entry points this
 * ROM carries at 0xbfc00200) or clear (the copies at 0x80000000).
 */
#define ROM_VEC_TLB_REFILL      0
#define ROM_VEC_XTLB_REFILL     1
#define ROM_VEC_CACHE_ERROR     2
#define ROM_VEC_GENERAL         3
#define ROM_VEC_BOOTSTRAP       4

#endif /* ROM_EXC_OFFSETS_H */
