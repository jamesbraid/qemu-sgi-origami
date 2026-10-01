/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The XIO widget layer: what every widget on an XIO link shares.
 *
 * Every widget begins its address space with the same register block,
 * the one Linux lays out as struct widget_cfg in asm/xtalk/xwidget.h:
 * identification, status, the error address pair, control, the request
 * timeout, the interrupt destination pair, the error command word, the
 * link configuration and the target flush, each a 32-bit register in the
 * data half of an eight-byte slot. Every widget also sends its interrupts
 * the same way: one doubleword write to the address the destination pair
 * names, the interrupt number in bits 7:0 and bit 8 set to raise it.
 *
 * What is shared lives here: the block's slot offsets, the decode of the
 * destination pair, the composition of a widget and an XIO address into
 * the fabric address a link carries, and the interrupt packet, which a
 * chip's error interrupt uses as much as its device interrupts. What
 * differs stays in each chip: which fields a register
 * holds, its reset value and its access rules come from that chip's own
 * specification, so the block's storage is the chip's, not this layer's.
 */

#ifndef HW_MIPS_SGI_XIO_WIDGET_H
#define HW_MIPS_SGI_XIO_WIDGET_H

#include "exec/hwaddr.h"
#include "exec/memattrs.h"
#include "system/memory.h"

/*
 * The block's eight-byte slots; the register is the slot's data half, at
 * the slot plus SGI_XIO_WIDGET_REG_DATA.
 */
#define SGI_XIO_WIDGET_ID               0x00
#define SGI_XIO_WIDGET_STATUS           0x08
#define SGI_XIO_WIDGET_ERR_UPPER        0x10
#define SGI_XIO_WIDGET_ERR_LOWER        0x18
#define SGI_XIO_WIDGET_CONTROL          0x20
#define SGI_XIO_WIDGET_REQ_TIMEOUT      0x28
#define SGI_XIO_WIDGET_INT_DEST_UPPER   0x30
#define SGI_XIO_WIDGET_INT_DEST_LOWER   0x38
#define SGI_XIO_WIDGET_ERR_CMD          0x40
#define SGI_XIO_WIDGET_LLP_CFG          0x48
#define SGI_XIO_WIDGET_TFLUSH           0x50
#define SGI_XIO_WIDGET_REG_DATA         4

/*
 * The interrupt destination pair. The upper register holds the vector in
 * bits 31:24, the target widget in 19:16 and address bits 47:32 in 15:0;
 * the lower holds address bits 31:0.
 */
#define SGI_XIO_INT_DEST_VECTOR_SHIFT   24
#define SGI_XIO_INT_DEST_WIDGET_SHIFT   16
#define SGI_XIO_INT_DEST_WIDGET_MASK    (0xfu << SGI_XIO_INT_DEST_WIDGET_SHIFT)
#define SGI_XIO_INT_DEST_ADDRESS_UPPER  0xffffu

typedef struct SGIXioIntDest {
    uint8_t vector;
    unsigned int widget;
    uint64_t address;
} SGIXioIntDest;

SGIXioIntDest sgi_xio_int_dest(uint32_t upper, uint32_t lower);

/* An interrupt packet's data: the number in bits 7:0, bit 8 to raise. */
#define SGI_XIO_INTERRUPT_SET           0x100

/*
 * The fabric address a (widget, 48-bit XIO address) pair travels at on a
 * link: the widget in bits 51:48. False, with a guest error naming who,
 * for a pair that does not fit.
 */
bool sgi_xio_compose(const char *who, unsigned int widget, uint64_t address,
                     hwaddr *packet);

/*
 * One interrupt packet into a fabric: interrupt number and raise or lower
 * at a composed fabric address.
 */
MemTxResult sgi_xio_send_interrupt(AddressSpace *fabric, hwaddr packet,
                                   uint8_t number, bool set);

#endif /* HW_MIPS_SGI_XIO_WIDGET_H */
