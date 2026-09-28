/* SPDX-License-Identifier: GPL-2.0-or-later */
/* InfiniteReality command-port selector tables. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-kona-ports.h"

const SGICommandPortSelector sgi_kona_fifo_selectors[] = {
    /*
     * KONAsetdisplaymode: selector, then 0, then the mode/timing argument,
     * then the fault-byte argument, the one entry in this table whose
     * selector-first word order is confirmed, not inferred.
     */
    { 0x1e6, 3 },
    { 0x1f4, 0 }, /* KONAvalidateclip / KONAschedswapbuf: bare */
    { 0x1f5, 0 }, /* kona_start_selectfeed: bare */
    { 0x1f6, 0 }, /* kona_finish_selectfeed: bare */
    { 0x1fc, 0 }, /* KONApcxswap: bare */
};
const unsigned sgi_kona_fifo_num_selectors =
    ARRAY_SIZE(sgi_kona_fifo_selectors);

const SGICommandPortSelector sgi_kona_textport_selectors[] = {
    { 0x1, 1 },
    { 0x2, 1 }, /* same shape as kona_setcolor */
    { 0x3, 4 },
    { 0x4, 2 },
    { 0x5, 4 },
    { 0x6, 2 }, /* same shape as kona_point */
    { 0x7, 2 },
    { 0x8, 2 }, /* cached screen size */
    { 0x9, 1 }, /* sync probe, magic 0x1234abcd */
    { 0xc, 1 }, /* graphics info, five reply words */
};
const unsigned sgi_kona_textport_num_selectors =
    ARRAY_SIZE(sgi_kona_textport_selectors);

const SGICommandPortSelector sgi_kona_doorbell_selectors[] = {
    { 0x71,  1 }, /* glEnd */
    { 0xc7,  1 }, /* glFlush */
    { 0xc6,  1 }, /* glFinish / glXSwapBuffers */
    { 0x1cc, 1 }, /* glFlushRasterSGIX */
    { 0x116, 4 }, /* GetSingleCompFromPipe */
    { 0x1d6, 1 }, /* __glkonResolveContext */
    { 0x1ca, 1 }, /* __glkonResolveContext */
};
const unsigned sgi_kona_doorbell_num_selectors =
    ARRAY_SIZE(sgi_kona_doorbell_selectors);
