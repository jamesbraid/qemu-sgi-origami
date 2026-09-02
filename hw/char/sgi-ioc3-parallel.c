/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the parallel port.
 *
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The port has two
 * faces and they sit far apart in the chip's window, so this device
 * presents two memory regions: a four-byte-wide DMA register block that
 * belongs in the main register block, and the byte-wide IEEE-1284/ECP
 * register file that belongs in the SuperIO window. The project tree's IOC3
 * datasheet is the specification. See docs/specs/sgi-ioc3.rst.
 *
 * What this models is the register contract, because that is all a source
 * establishes. IRIX's parallel driver probes for the port by reading the
 * extended control register and taking "transmit FIFO empty, not full" as
 * the port answering; without that the driver never attaches and opening
 * the device node fails. Everything past the probe -- the mode field, the
 * negotiation lines, the two DMA contexts -- is a register file a driver
 * writes and reads back.
 *
 * Nothing is on the cable. No peripheral is modelled, so bytes handed to
 * the port leave it and the transmit FIFO never fills, the status register
 * reports an empty cable, and the DMA engine never takes a context. That
 * also means the block raises none of the four interrupts the chip
 * reserves for it: no source drives one. The datasheet is explicit that
 * the active sense of the extended control register's two interrupt
 * controls is unresolved, so this model stores them and arms nothing,
 * rather than picking a polarity a guest would then be measured against.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/hwaddr.h"
#include "hw/char/sgi-ioc3-parallel.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

static unsigned int sgi_ioc3_parallel_mode(SGIIOC3ParallelState *s)
{
    return (s->extended & SGI_IOC3_PAR_ECR_MODE) >>
           SGI_IOC3_PAR_ECR_MODE_SHIFT;
}

/*
 * The shared control register's low six bits, which are the engine's view
 * of the two context registers rather than storage. Both busy flags stay
 * clear: the engine never takes a context, because there is nothing on the
 * cable for it to move bytes to.
 */
static uint32_t sgi_ioc3_parallel_dma_status(SGIIOC3ParallelState *s)
{
    uint32_t status = 0;
    unsigned int i;

    for (i = 0; i < SGI_IOC3_PAR_CONTEXTS; i++) {
        unsigned int shift = i * SGI_IOC3_PAR_DMA_STATUS_BITS;

        if (s->count[i] & SGI_IOC3_PAR_COUNT_ARM) {
            status |= SGI_IOC3_PAR_DMA_A_ARMED << shift;
        }
        if (s->count[i] & SGI_IOC3_PAR_COUNT_LAST) {
            status |= SGI_IOC3_PAR_DMA_A_LAST << shift;
        }
    }

    return status;
}

static uint64_t sgi_ioc3_parallel_dma_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(opaque);

    switch (addr) {
    case SGI_IOC3_PAR_CTXA_ADDR_HI:
        return s->addr_hi[0];
    case SGI_IOC3_PAR_CTXA_ADDR_LO:
        return s->addr_lo[0];
    case SGI_IOC3_PAR_CTXB_ADDR_HI:
        return s->addr_hi[1];
    case SGI_IOC3_PAR_CTXB_ADDR_LO:
        return s->addr_lo[1];

    /*
     * The count field reads back what is still outstanding. No transfer
     * ever starts here, so nothing has been subtracted from it and the
     * value is the one software loaded.
     */
    case SGI_IOC3_PAR_CTXA_COUNT:
        return s->count[0];
    case SGI_IOC3_PAR_CTXB_COUNT:
        return s->count[1];

    case SGI_IOC3_PAR_DMA_CONTROL:
        return s->dma_control | sgi_ioc3_parallel_dma_status(s);

    default:
        g_assert_not_reached();
    }
}

static void sgi_ioc3_parallel_dma_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(opaque);

    switch (addr) {
    case SGI_IOC3_PAR_CTXA_ADDR_HI:
        s->addr_hi[0] = val;
        return;
    case SGI_IOC3_PAR_CTXA_ADDR_LO:
        s->addr_lo[0] = val;
        return;
    case SGI_IOC3_PAR_CTXB_ADDR_HI:
        s->addr_hi[1] = val;
        return;
    case SGI_IOC3_PAR_CTXB_ADDR_LO:
        s->addr_lo[1] = val;
        return;

    case SGI_IOC3_PAR_CTXA_COUNT:
        s->count[0] = val & SGI_IOC3_PAR_COUNT_STORED;
        return;
    case SGI_IOC3_PAR_CTXB_COUNT:
        s->count[1] = val & SGI_IOC3_PAR_COUNT_STORED;
        return;

    /*
     * Direction, the state-machine reset the driver holds high then low,
     * and the engine enable. The six status bits below them belong to the
     * contexts, so a write cannot assert one from this side.
     */
    case SGI_IOC3_PAR_DMA_CONTROL:
        s->dma_control = val & SGI_IOC3_PAR_DMA_STORED;
        return;

    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps sgi_ioc3_parallel_dma_ops = {
    .read = sgi_ioc3_parallel_dma_read,
    .write = sgi_ioc3_parallel_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t sgi_ioc3_parallel_ecp_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(opaque);

    switch (addr) {
    case SGI_IOC3_PAR_ECP_DATA:
        return s->data;

    /*
     * The lines the peripheral drives, and nothing is on the cable. Only
     * nFault's level is pinned by a source -- clear means a fault -- so an
     * empty cable reports no fault and leaves the rest clear: no
     * peripheral selected, no paper error, and never busy, which is the
     * level a driver waiting for the printer would otherwise wait on
     * forever.
     */
    case SGI_IOC3_PAR_ECP_STATUS:
        return SGI_IOC3_PAR_STATUS_IDLE;

    case SGI_IOC3_PAR_ECP_CONTROL:
        return s->control;

    case SGI_IOC3_PAR_ECP_FIFO:
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_CONFIG) {
            return s->config_a;
        }
        /*
         * The FIFO in every other mode, and it is always empty: nothing
         * on the cable ever hands a byte back. The shipping driver is
         * output-only, so a read here is software reading a queue its own
         * status register just told it was empty.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-parallel: read of the empty FIFO\n");
        return 0;

    case SGI_IOC3_PAR_ECP_CONFIG_B:
        return s->config_b;

    case SGI_IOC3_PAR_ECP_EXTENDED:
        /* Empty and not full, over software's own control bits. */
        return s->extended | SGI_IOC3_PAR_ECR_FIFO_EMPTY;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3-parallel: read of unnamed register 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void sgi_ioc3_parallel_ecp_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(opaque);

    switch (addr) {
    case SGI_IOC3_PAR_ECP_DATA:
        s->data = val;
        return;

    case SGI_IOC3_PAR_ECP_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-parallel: write of 0x%" PRIx64 " to the "
                      "read-only status register\n", val);
        return;

    /*
     * The whole byte is stored. The datasheet names bits 0 to 3 and bit 5
     * and is silent on the other three, so masking them away would be this
     * model asserting they read as zero; reading back what was written
     * asserts nothing.
     */
    case SGI_IOC3_PAR_ECP_CONTROL:
        s->control = val;
        return;

    case SGI_IOC3_PAR_ECP_FIFO:
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_CONFIG) {
            s->config_a = val;
            return;
        }
        /*
         * The byte goes out on the data lines. Nothing is on the cable to
         * hold it up, so it leaves within the write and the FIFO stays
         * empty: a driver polling for room before each byte never waits,
         * and one polling for the queue to drain finishes on its first
         * look.
         */
        return;

    case SGI_IOC3_PAR_ECP_CONFIG_B:
        s->config_b = val;
        return;

    /* The two FIFO flags are hardware status and are not writable. */
    case SGI_IOC3_PAR_ECP_EXTENDED:
        s->extended = val & SGI_IOC3_PAR_ECR_STORED;
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3-parallel: write of 0x%" PRIx64 " to unnamed "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
}

/*
 * Byte-wide, and the implementation width says so: an undeclared maximum
 * would take one four-byte access apart into four calls, running the FIFO
 * write four times over.
 */
static const MemoryRegionOps sgi_ioc3_parallel_ecp_ops = {
    .read = sgi_ioc3_parallel_ecp_read,
    .write = sgi_ioc3_parallel_ecp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void sgi_ioc3_parallel_init(Object *obj)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->dma_mmio, obj, &sgi_ioc3_parallel_dma_ops, s,
                          "sgi-ioc3.parallel-dma", SGI_IOC3_PAR_DMA_SIZE);
    sysbus_init_mmio(sbd, &s->dma_mmio);

    memory_region_init_io(&s->ecp_mmio, obj, &sgi_ioc3_parallel_ecp_ops, s,
                          "sgi-ioc3.parallel-ecp", SGI_IOC3_PAR_ECP_SIZE);
    sysbus_init_mmio(sbd, &s->ecp_mmio);
}

/*
 * Every register resets to zero. The extended control register's reset
 * value of 0x01 comes out of that on its own: its stored half is zero, so
 * the mode field reads compatibility and the three control bits read
 * clear, and the FIFO-empty flag the driver's probe looks for is supplied
 * by the read rather than stored.
 *
 * The datasheet marks the control and configuration registers' reset
 * values unverified. Zero is this model's choice for them.
 */
static void sgi_ioc3_parallel_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(obj);

    memset(s->addr_hi, 0, sizeof(s->addr_hi));
    memset(s->addr_lo, 0, sizeof(s->addr_lo));
    memset(s->count, 0, sizeof(s->count));
    s->dma_control = 0;

    s->data = 0;
    s->control = 0;
    s->config_a = 0;
    s->config_b = 0;
    s->extended = 0;
}

static const VMStateDescription vmstate_sgi_ioc3_parallel = {
    .name = "sgi-ioc3-parallel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(addr_hi, SGIIOC3ParallelState,
                             SGI_IOC3_PAR_CONTEXTS),
        VMSTATE_UINT32_ARRAY(addr_lo, SGIIOC3ParallelState,
                             SGI_IOC3_PAR_CONTEXTS),
        VMSTATE_UINT32_ARRAY(count, SGIIOC3ParallelState,
                             SGI_IOC3_PAR_CONTEXTS),
        VMSTATE_UINT32(dma_control, SGIIOC3ParallelState),
        VMSTATE_UINT8(data, SGIIOC3ParallelState),
        VMSTATE_UINT8(control, SGIIOC3ParallelState),
        VMSTATE_UINT8(config_a, SGIIOC3ParallelState),
        VMSTATE_UINT8(config_b, SGIIOC3ParallelState),
        VMSTATE_UINT8(extended, SGIIOC3ParallelState),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_ioc3_parallel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 parallel port";
    dc->vmsd = &vmstate_sgi_ioc3_parallel;
    /* Instantiated only by the IOC3, which maps both of its regions. */
    dc->user_creatable = false;
    rc->phases.hold = sgi_ioc3_parallel_reset_hold;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sgi_ioc3_parallel_types[] = {
    {
        .name = TYPE_SGI_IOC3_PARALLEL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIIOC3ParallelState),
        .instance_init = sgi_ioc3_parallel_init,
        .class_init = sgi_ioc3_parallel_class_init,
    },
};

DEFINE_TYPES(sgi_ioc3_parallel_types)
