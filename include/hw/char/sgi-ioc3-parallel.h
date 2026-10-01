/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI IOC3 parallel port register and device definitions. */
/*
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The port has two
 * faces in two different parts of the chip's window, so the device presents
 * two memory regions and the chip maps each at its own base. The project IOC3
 * register behavior is documented in docs/specs/sgi-ioc3.rst.
 */

#ifndef HW_CHAR_SGI_IOC3_PARALLEL_H
#define HW_CHAR_SGI_IOC3_PARALLEL_H

#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3_PARALLEL "sgi-ioc3-parallel"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3ParallelState, SGI_IOC3_PARALLEL)

/*
 * Where the chip puts each face. The DMA block sits in the main register
 * block above the sixteen pin registers and below the PS/2 controller; the
 * register file sits in the byte-wide SuperIO window below serial port B.
 * Kept here rather than with the chip's own offsets so that mapping the port
 * is one constant per region at the wiring site.
 */
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

/*
 * The four interrupt lines the port drives, in the order the chip's own
 * interrupt register carries them: the service interrupt that reports a whole
 * chain complete, one line per context, and a DMA fault. The chip maps them
 * to consecutive bits, so the wiring is index plus a base.
 */
#define SGI_IOC3_PAR_IRQ_SERVICE    0
#define SGI_IOC3_PAR_IRQ_CONTEXT    1   /* and CONTEXT + 1 for context B */
#define SGI_IOC3_PAR_IRQ_ERROR      3
#define SGI_IOC3_PAR_IRQS           4

/*
 * A context's count and control: seventeen count bits, so one context carries
 * up to 131071 bytes, then the arm flag and the flag marking this the last
 * context of a chain. Everything between is undefined and reads zero.
 */
#define SGI_IOC3_PAR_COUNT          0x0001ffffu
#define SGI_IOC3_PAR_COUNT_ARM      (1u << 30)
#define SGI_IOC3_PAR_COUNT_LAST     (1u << 31)
#define SGI_IOC3_PAR_COUNT_STORED   (SGI_IOC3_PAR_COUNT | \
                                     SGI_IOC3_PAR_COUNT_ARM | \
                                     SGI_IOC3_PAR_COUNT_LAST)

/*
 * The shared control register. Its low six bits are the engine's view of the
 * two context registers: armed, last and currently draining, context A then
 * context B. These bits are read-only here, not storage of their own.
 */
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

/*
 * The byte-wide IEEE-1284/ECP register file. Offsets 0x03 to 0x07 are the gap
 * a PC SuperIO chip fills with its EPP registers; no source names them for
 * this chip, so they are inside the window and answer nothing.
 */
#define SGI_IOC3_PAR_ECP_DATA       0x00
#define SGI_IOC3_PAR_ECP_STATUS     0x01
#define SGI_IOC3_PAR_ECP_CONTROL    0x02
#define SGI_IOC3_PAR_ECP_FIFO       0x08    /* config A in config mode */
#define SGI_IOC3_PAR_ECP_CONFIG_B   0x09
#define SGI_IOC3_PAR_ECP_EXTENDED   0x0a
#define SGI_IOC3_PAR_ECP_SIZE       (SGI_IOC3_PAR_ECP_EXTENDED + 1)

/*
 * The status register's peripheral lines. nFault is active low: a clear
 * level means a fault. The register names the rest without one, so this model
 * reports the levels an empty cable justifies: no fault, and nothing else.
 */
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
 * Extended control: two FIFO status bits, three control bits whose active
 * sense no source settles, and the mode field in the top three.
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
#define SGI_IOC3_PAR_MODE_CONFIG      7

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

    /*
     * The service condition, which the completion interrupt follows: set when
     * a chain reaches terminal count and quiet again once software writes the
     * extended control register.
     */
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
};

#endif /* HW_CHAR_SGI_IOC3_PARALLEL_H */
