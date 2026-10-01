/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI IOC3 parallel port register and device definitions. */
/*
 * IOC3 maps the DMA and byte-wide ECP regions separately.
 * The register contract is documented in docs/specs/sgi-ioc3.rst.
 */

#ifndef HW_CHAR_SGI_IOC3_PARALLEL_H
#define HW_CHAR_SGI_IOC3_PARALLEL_H

#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3_PARALLEL "sgi-ioc3-parallel"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3ParallelState, SGI_IOC3_PARALLEL)

/* Offsets within the IOC3 register window. */
#define SGI_IOC3_PARALLEL_DMA_BASE  0x00080
#define SGI_IOC3_PARALLEL_ECP_BASE  0x20150

/* The DMA register block, four bytes to a register. */
#define SGI_IOC3_PAR_CTXA_ADDR_HI   0x00
#define SGI_IOC3_PAR_CTXA_ADDR_LO   0x04
#define SGI_IOC3_PAR_CTXA_COUNT     0x08
#define SGI_IOC3_PAR_DMA_CONTROL    0x0c
#define SGI_IOC3_PAR_CTXB_ADDR_HI   0x10
#define SGI_IOC3_PAR_CTXB_ADDR_LO   0x14
#define SGI_IOC3_PAR_CTXB_COUNT     0x18
#define SGI_IOC3_PAR_DMA_SIZE       (SGI_IOC3_PAR_CTXB_COUNT + 4)

#define SGI_IOC3_PAR_CONTEXTS       2

/* IOC3 maps these four interrupt lines to consecutive bits. */
#define SGI_IOC3_PAR_IRQ_SERVICE    0
#define SGI_IOC3_PAR_IRQ_CONTEXT    1   /* and CONTEXT + 1 for context B */
#define SGI_IOC3_PAR_IRQ_ERROR      3
#define SGI_IOC3_PAR_IRQS           4

/* Count, arm, and end-of-chain flags. Reserved bits read zero. */
#define SGI_IOC3_PAR_COUNT          0x0001ffffu
#define SGI_IOC3_PAR_COUNT_ARM      (1u << 30)
#define SGI_IOC3_PAR_COUNT_LAST     (1u << 31)
#define SGI_IOC3_PAR_COUNT_STORED   (SGI_IOC3_PAR_COUNT | \
                                     SGI_IOC3_PAR_COUNT_ARM | \
                                     SGI_IOC3_PAR_COUNT_LAST)

/* Read-only context status: armed, last, and busy for A, then B. */
#define SGI_IOC3_PAR_DMA_A_ARMED    (1u << 0)
#define SGI_IOC3_PAR_DMA_A_LAST     (1u << 1)
#define SGI_IOC3_PAR_DMA_A_BUSY     (1u << 2)
#define SGI_IOC3_PAR_DMA_STATUS_BITS 3
#define SGI_IOC3_PAR_DMA_STATUS     0x0000003fu

#define SGI_IOC3_PAR_DMA_FROM_PERIPHERAL (1u << 29)
#define SGI_IOC3_PAR_DMA_RESET      (1u << 30)   /* held, not a pulse */
#define SGI_IOC3_PAR_DMA_ENABLE     (1u << 31)
#define SGI_IOC3_PAR_DMA_STORED     (SGI_IOC3_PAR_DMA_FROM_PERIPHERAL | \
                                     SGI_IOC3_PAR_DMA_RESET | \
                                     SGI_IOC3_PAR_DMA_ENABLE)

/* Byte-wide ECP registers. EPP accesses at 0x03 to 0x07 are unimplemented. */
#define SGI_IOC3_PAR_ECP_DATA       0x00
#define SGI_IOC3_PAR_ECP_STATUS     0x01
#define SGI_IOC3_PAR_ECP_CONTROL    0x02
#define SGI_IOC3_PAR_ECP_FIFO       0x08    /* config A in config mode */
#define SGI_IOC3_PAR_ECP_CONFIG_B   0x09
#define SGI_IOC3_PAR_ECP_EXTENDED   0x0a
#define SGI_IOC3_PAR_ECP_SIZE       (SGI_IOC3_PAR_ECP_EXTENDED + 1)

/* nFault is active low. The model reports no fault and no other input. */
#define SGI_IOC3_PAR_STATUS_ATTENTION (1u << 2)
#define SGI_IOC3_PAR_STATUS_NFAULT    (1u << 3)
#define SGI_IOC3_PAR_STATUS_SELECT    (1u << 4)
#define SGI_IOC3_PAR_STATUS_PAPER     (1u << 5)
#define SGI_IOC3_PAR_STATUS_NACK      (1u << 6)
#define SGI_IOC3_PAR_STATUS_BUSY      (1u << 7)
#define SGI_IOC3_PAR_STATUS_IDLE      SGI_IOC3_PAR_STATUS_NFAULT

/* The output control lines, which software drives one step at a time. */
#define SGI_IOC3_PAR_CTRL_NSTROBE   (1u << 0)
#define SGI_IOC3_PAR_CTRL_NAUTOFEED (1u << 1)
#define SGI_IOC3_PAR_CTRL_NINIT     (1u << 2)
#define SGI_IOC3_PAR_CTRL_NSELECTIN (1u << 3)
#define SGI_IOC3_PAR_CTRL_FROM_PERIPHERAL (1u << 5)

/*
 * FIFO status, service/DMA/nFault controls, and the mode field.
 * The generic model leaves interrupt-control polarity unresolved.
 */
#define SGI_IOC3_PAR_ECR_FIFO_EMPTY  (1u << 0)
#define SGI_IOC3_PAR_ECR_FIFO_FULL   (1u << 1)
#define SGI_IOC3_PAR_ECR_SERVICE_INT (1u << 2)
#define SGI_IOC3_PAR_ECR_DMA_ENABLE  (1u << 3)
#define SGI_IOC3_PAR_ECR_NFAULT_INT  (1u << 4)
#define SGI_IOC3_PAR_ECR_MODE        0xe0u
#define SGI_IOC3_PAR_ECR_MODE_SHIFT  5
#define SGI_IOC3_PAR_ECR_STORED      (SGI_IOC3_PAR_ECR_SERVICE_INT | \
                                      SGI_IOC3_PAR_ECR_DMA_ENABLE | \
                                      SGI_IOC3_PAR_ECR_NFAULT_INT | \
                                      SGI_IOC3_PAR_ECR_MODE)

/* The standard Extended Capabilities Port mode encoding. */
#define SGI_IOC3_PAR_MODE_COMPAT      0     /* the reset mode */
#define SGI_IOC3_PAR_MODE_BYTE        1
#define SGI_IOC3_PAR_MODE_COMPAT_FIFO 2     /* what the driver selects */
#define SGI_IOC3_PAR_MODE_ECP         3
#define SGI_IOC3_PAR_MODE_TEST        6
#define SGI_IOC3_PAR_MODE_CONFIG      7

#define SGI_IOC3_PAR_PART_GENERIC     0
#define SGI_IOC3_PAR_PART_TL16PIR552 1
#define SGI_IOC3_PAR_TL16_FIFO_DEPTH 16

typedef struct SGIIOC3ParallelPart SGIIOC3ParallelPart;

struct SGIIOC3ParallelState {
    SysBusDevice parent_obj;

    MemoryRegion dma_mmio;
    MemoryRegion ecp_mmio;

    /* The parent's bus-master view, linked before realize. */
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    /* What the cable reaches, if a board was given one. */
    CharFrontend chr;

    qemu_irq irq[SGI_IOC3_PAR_IRQS];

    /* Service remains asserted from terminal count until an ECR write. */
    bool service;

    uint32_t addr_hi[SGI_IOC3_PAR_CONTEXTS];
    uint32_t addr_lo[SGI_IOC3_PAR_CONTEXTS];
    uint32_t count[SGI_IOC3_PAR_CONTEXTS];
    uint32_t dma_control;

    uint8_t data;
    uint8_t control;
    uint8_t config_a;
    uint8_t config_b;
    uint8_t extended;
    uint16_t parallel_part;
    uint16_t fifo_depth;
    const SGIIOC3ParallelPart *part;
    Fifo8 test_fifo;
};

#endif /* HW_CHAR_SGI_IOC3_PARALLEL_H */
