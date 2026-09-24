/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_IO_H
#define HW_MISC_SGI_BEDROCK_IO_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_SGI_BEDROCK_IO "sgi-bedrock-io"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockIOState, SGI_BEDROCK_IO)

#define SGI_BEDROCK_IO_REG_SIZE 0x30010
#define SGI_BEDROCK_IO_PIO_SIZE UINT64_C(0x100000000)
/* A widget's crosstalk address: bit 47 selects registers over memory. */
#define SGI_BEDROCK_IO_INBOUND_SIZE (UINT64_C(1) << 48)

typedef struct SGIBedrockIOBTE {
    SGIBedrockIOState *io;
    uint64_t status;
    uint64_t source;
    uint64_t destination;
    uint64_t control;
    uint64_t notification;
    uint64_t interrupt;
    bool active;
    bool signal_pending;
    QEMUTimer *timer;
    QEMUBH *signal_bh;
} SGIBedrockIOBTE;

struct SGIBedrockIOState {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    MemoryRegion pio;
    MemoryRegion inbound;
    AddressSpace *dma_as;
    MemoryRegion *dma_mr;
    AddressSpace linked_dma_as;
    bool linked_dma_as_initialized;
    MemoryRegion *hub_mr;
    AddressSpace hub_as;
    bool hub_as_initialized;
    MemoryRegion *widget_mr[16];
    AddressSpace widget_as[16];
    bool widget_as_initialized[16];
    uint8_t local_region;
    uint8_t node_number;
    uint8_t revision;
    /*
     * Which crosstalk widget numbers a request arriving at the shared
     * inbound window may credibly have come from, one bit per widget.  The
     * window itself carries no source-widget field to read back, so all
     * candidates must be permitted before an unknown-source access proceeds.
     * This is not a substitute for per-packet source identity.  Defaults to
     * the two heads an XBridge presents to its host
     * port (0xe and 0xf; see hw/pci-host/sgi-xbridge.c CHIP_HEAD_E/F and
     * the Origin 300 model's xbridge_widgets), not widget 8, which nothing
     * in a Bedrock+XBridge machine is ever wired to.
     */
    uint16_t inbound_widget_mask;
    uint8_t inbound_device;
    uint64_t device_mask_reset;
    uint64_t widget_control;
    uint64_t local_protect;
    uint64_t outbound_access;
    uint64_t inbound_access;
    uint64_t device_mask;
    uint64_t link_status;
    uint64_t link_log;
    uint64_t interrupt_destination;
    uint64_t credit_timeout;
    uint64_t tail_timeout;
    uint64_t bte_control;
    uint64_t request_management;
    uint64_t request_control;
    uint64_t request_timeout;
    uint64_t request_prescaler;
    uint64_t misc_error_mask;
    uint64_t scratch[2];
    uint64_t itte[7];
    /*
     * The owning node, when this device is one of sgi-bedrock's children:
     * a plain pointer, as the local block's is, because this child's
     * lifetime is enclosed by its parent's. Scratch 0 writes are reported
     * to sgi_bedrock_presence_published(), which owns the processor
     * bookkeeping; see sgi-bedrock.h. NULL for a standalone device.
     */
    struct SGIBedrockState *bedrock;
    uint64_t request_buffer[9];
    uint64_t retained[96];
    SGIBedrockIOBTE bte[2];
};

void sgi_bedrock_io_set_dma_as(SGIBedrockIOState *s, AddressSpace *as);
MemTxResult sgi_bedrock_io_inbound_rw(SGIBedrockIOState *s, unsigned widget,
                                      unsigned device, hwaddr address,
                                      void *buffer, size_t size, bool write);
void sgi_bedrock_io_disable_device(SGIBedrockIOState *s, unsigned widget,
                                    unsigned device);

#endif
