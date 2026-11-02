/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The link between an L1 controller model and a stand-in for an L2.
 *
 * This is the project's own format, not a hardware wire. The real link is a
 * USB bulk pair that carries one router segment per transfer; a stream
 * socket has no transfer boundary, so each message is a one-byte kind
 * followed by the body. The body of a segment message is the segment as the
 * L1 sends it in a bulk transfer: its first two bytes are its length, big
 * endian and counting themselves, so the stream delimits itself.
 *
 * The field offsets are those of the router segment in the L1 and L2 sheets
 * of the project's datasheets (docs/system/mips/sgi/hardware/l1.rst, "L2
 * link").
 */
#ifndef HW_MISC_SGI_L1_L2_LINK_H
#define HW_MISC_SGI_L1_L2_LINK_H

/* Message kinds. Other values are reserved and dropped by the L1. */
#define SGI_L1_L2_KIND_SEGMENT  1

/* A segment holds at most 1,024 bytes; the first seven offsets are fixed. */
#define SGI_L1_L2_SEGMENT_MAX   1024
#define SGI_L1_L2_SEGMENT_MIN   6

/* Field offsets within a segment. */
#define SGI_L1_L2_SEG_LENGTH    0   /* 2 bytes, includes itself */
#define SGI_L1_L2_SEG_FORMAT    2   /* 1 byte, 2 as the L1 builds it */
#define SGI_L1_L2_SEG_FLAGS     3   /* 1 byte, meaning not established */
#define SGI_L1_L2_SEG_SEQ_LOW   4   /* 1 byte */
#define SGI_L1_L2_SEG_COUNT     5   /* bit 7 first segment, 6:0 remaining */
#define SGI_L1_L2_SEG_CLASS     6   /* 1 byte */
#define SGI_L1_L2_SEG_PORT      7   /* 1 byte, meaning not established */
#define SGI_L1_L2_SEG_DEST      8   /* 4 bytes */
#define SGI_L1_L2_SEG_SOURCE    12  /* 4 bytes */
#define SGI_L1_L2_SEG_NDESC     16  /* 1 byte */
#define SGI_L1_L2_SEG_SEQ_HIGH  17  /* 1 byte */
#define SGI_L1_L2_SEG_CODE      18  /* 2 bytes */
#define SGI_L1_L2_SEG_DESC      20  /* eight bytes per descriptor */

#define SGI_L1_L2_FIRST_SEGMENT 0x80

/*
 * Descriptor types. An integer is "00 00 0000 value32". A string is
 * "0a segment length16 offset32" with the length counting the NUL and the
 * offset counting 16 bytes that are not on the wire (L1 sheet, chunk
 * transport rows). The type of a binary descriptor is not established by
 * the sheets: SGI_L1_L2_DESC_BLOB is the value this tree uses for it, laid
 * out like a string without the NUL.
 */
#define SGI_L1_L2_DESC_INTEGER  0x00
#define SGI_L1_L2_DESC_STRING   0x0a
#define SGI_L1_L2_DESC_BLOB     0x0b
#define SGI_L1_L2_DESC_PREFIX   0x10

/*
 * The L2 daemon's recurring configuration request and the task 14
 * endpoint the L1 answers it from (L2 sheet, "The daemon's recurring
 * request" and "The reply, and the route it creates").
 */
#define SGI_L1_L2_CONFIG_DEST   UINT32_C(0x0ffff00e)
#define SGI_L1_L2_CONFIG_TASK   0x0e
#define SGI_L1_L2_CONFIG_SELECTOR      6
#define SGI_L1_L2_CONFIG_SELECTOR_A    2
#define SGI_L1_L2_CONFIG_SELECTOR_B    13
#define SGI_L1_L2_CONFIG_REPLY_KIND    7
#define SGI_L1_L2_RECORD_SIZE   0x70

/* Packet classes. */
#define SGI_L1_L2_CLASS_REQUEST  0
#define SGI_L1_L2_CLASS_RESPONSE 1
#define SGI_L1_L2_CLASS_EVENT    2

#endif /* HW_MISC_SGI_L1_L2_LINK_H */
