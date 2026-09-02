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
 * No peripheral is modelled. What is on the far side of the cable is a
 * character device the board hands the port, so what a guest prints comes
 * out somewhere a host can read it, and a port with no backend swallows the
 * bytes rather than stalling. Either way nothing holds a byte up: the
 * transmit FIFO never fills, the status register reports an empty cable,
 * and the DMA engine retires a context within the register write that arms
 * it, so its busy flag is never observable.
 *
 * A byte-wide cable with no peripheral has no way to hand a byte back, so
 * the read direction moves nothing and its contexts stay armed, which is
 * what the port does with nothing plugged into it.
 *
 * Retiring the context in the register file is not on its own enough for a
 * driver: IRIX programs a transfer, sleeps, and is still asleep when the
 * count reads consumed and the bytes are out. What wakes it is the service
 * interrupt, which the chip raises when the context carrying the last flag
 * reaches terminal count. Its own context interrupt does not do the job --
 * on a single-descriptor transfer the driver masks that source before it
 * waits.
 *
 * The active sense of the extended control register's two interrupt
 * controls is unresolved, so nothing here reads them to decide whether to
 * raise. What is used instead is the one thing the sources agree on: the
 * driver quiets the service condition by writing that register. So the
 * condition is set at terminal count and falls on the next write there,
 * which needs no polarity.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "hw/char/sgi-ioc3-parallel.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/dma.h"

static unsigned int sgi_ioc3_parallel_mode(SGIIOC3ParallelState *s)
{
    return (s->extended & SGI_IOC3_PAR_ECR_MODE) >>
           SGI_IOC3_PAR_ECR_MODE_SHIFT;
}

/*
 * The shared control register's low six bits, which are the engine's view
 * of the two context registers rather than storage. Both busy flags stay
 * clear: a context is retired inside the register write that completes its
 * setup, so no read can land while one is draining.
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

static void sgi_ioc3_parallel_set_service(SGIIOC3ParallelState *s, bool level)
{
    s->service = level;
    qemu_set_irq(s->irq[SGI_IOC3_PAR_IRQ_SERVICE], level);
}

/*
 * Drain one armed context: read its buffer out of memory through the
 * chip's own bus-master view -- never the physical-memory accessors, which
 * would walk around the Bridge translation the guest programmed -- put the
 * bytes on the cable, and retire the context.
 *
 * Retiring is the count field down to nothing outstanding and the arm flag
 * clear. The last flag is left as software loaded it: it describes the
 * descriptor rather than the engine's progress through it, and no source
 * says the engine takes it back.
 *
 * False on a refused fetch, which stops the walk: a chain whose first
 * buffer never arrived must not put the second one on the cable.
 */
static bool sgi_ioc3_parallel_dma_drain(SGIIOC3ParallelState *s,
                                        unsigned int context)
{
    uint64_t addr = ((uint64_t)s->addr_hi[context] << 32) |
                    s->addr_lo[context];
    uint32_t count = s->count[context] & SGI_IOC3_PAR_COUNT;
    g_autofree uint8_t *buf = NULL;

    if (count) {
        buf = g_malloc(count);
        if (dma_memory_read(&s->dma_as, addr, buf, count,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            /*
             * A refused fetch leaves the context armed. On the chip this
             * raises the port's PCI-error interrupt and the driver recovers
             * the residual from this register, which is the value it needs
             * to find here.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc3-parallel: context %u buffer at 0x%"
                          PRIx64 " is unreadable\n", context, addr);
            qemu_irq_pulse(s->irq[SGI_IOC3_PAR_IRQ_ERROR]);
            return false;
        }
        qemu_chr_fe_write_all(&s->chr, buf, count);
    }

    s->count[context] &= ~(SGI_IOC3_PAR_COUNT | SGI_IOC3_PAR_COUNT_ARM);

    /* The context's own source latches; the driver clears it by hand. */
    qemu_irq_pulse(s->irq[SGI_IOC3_PAR_IRQ_CONTEXT + context]);

    /*
     * Terminal count: the chain ends here, so the service condition comes
     * up and stays up until software quiets it. This is the interrupt a
     * driver blocked in a write is waiting for.
     */
    if (s->count[context] & SGI_IOC3_PAR_COUNT_LAST) {
        sgi_ioc3_parallel_set_service(s, true);
    }

    return true;
}

/*
 * Two enables, and the driver sets them in two different registers: the
 * engine's own in the shared control register and the DMA path in the
 * extended control register. Running on one of them alone would start a
 * transfer the driver has not finished setting up, so every write that can
 * complete the pair comes through here and the pair is tested afresh.
 *
 * Context A before context B, which is the order software chains them in.
 */
static void sgi_ioc3_parallel_dma_run(SGIIOC3ParallelState *s)
{
    unsigned int i;

    if (!(s->dma_control & SGI_IOC3_PAR_DMA_ENABLE) ||
        (s->dma_control & SGI_IOC3_PAR_DMA_RESET) ||
        !(s->extended & SGI_IOC3_PAR_ECR_DMA_ENABLE)) {
        return;
    }

    for (i = 0; i < SGI_IOC3_PAR_CONTEXTS; i++) {
        if (!(s->count[i] & SGI_IOC3_PAR_COUNT_ARM)) {
            continue;
        }

        /* Nothing on the cable has a byte to hand back. */
        if (s->dma_control & SGI_IOC3_PAR_DMA_FROM_PERIPHERAL) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc3-parallel: no peripheral to read context "
                          "%u from\n", i);
            return;
        }

        if (!sgi_ioc3_parallel_dma_drain(s, i)) {
            return;
        }
    }
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
     * The count field reads back what is still outstanding: the value
     * software loaded until the engine drains the context, nothing after.
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
        sgi_ioc3_parallel_dma_run(s);
        return;
    case SGI_IOC3_PAR_CTXB_COUNT:
        s->count[1] = val & SGI_IOC3_PAR_COUNT_STORED;
        sgi_ioc3_parallel_dma_run(s);
        return;

    /*
     * Direction, the state-machine reset the driver holds high then low,
     * and the engine enable. The six status bits below them belong to the
     * contexts, so a write cannot assert one from this side.
     */
    case SGI_IOC3_PAR_DMA_CONTROL:
        s->dma_control = val & SGI_IOC3_PAR_DMA_STORED;
        sgi_ioc3_parallel_dma_run(s);
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
         * The byte goes out on the data lines, to whatever the board put
         * on the far side of the cable. Nothing there holds it up, so it
         * leaves within the write and the FIFO stays empty: a driver
         * polling for room before each byte never waits, and one polling
         * for the queue to drain finishes on its first look.
         */
        {
            uint8_t byte = val;

            qemu_chr_fe_write_all(&s->chr, &byte, sizeof(byte));
        }
        return;

    case SGI_IOC3_PAR_ECP_CONFIG_B:
        s->config_b = val;
        return;

    /*
     * The two FIFO flags are hardware status and are not writable. The
     * write also quiets the service condition, which is how the driver
     * acknowledges a completion -- before the store and the engine pass,
     * so that a transfer this same write sets going can raise it again.
     */
    case SGI_IOC3_PAR_ECP_EXTENDED:
        sgi_ioc3_parallel_set_service(s, false);
        s->extended = val & SGI_IOC3_PAR_ECR_STORED;
        sgi_ioc3_parallel_dma_run(s);
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
    unsigned int i;

    memory_region_init_io(&s->dma_mmio, obj, &sgi_ioc3_parallel_dma_ops, s,
                          "sgi-ioc3.parallel-dma", SGI_IOC3_PAR_DMA_SIZE);
    sysbus_init_mmio(sbd, &s->dma_mmio);

    memory_region_init_io(&s->ecp_mmio, obj, &sgi_ioc3_parallel_ecp_ops, s,
                          "sgi-ioc3.parallel-ecp", SGI_IOC3_PAR_ECP_SIZE);
    sysbus_init_mmio(sbd, &s->ecp_mmio);

    for (i = 0; i < SGI_IOC3_PAR_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void sgi_ioc3_parallel_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(dev);

    if (!s->dma_mr) {
        error_setg(errp, TYPE_SGI_IOC3_PARALLEL " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sgi-ioc3-parallel-dma");
}

static void sgi_ioc3_parallel_unrealize(DeviceState *dev)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(dev);

    address_space_destroy(&s->dma_as);
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

    sgi_ioc3_parallel_set_service(s, false);
}

/*
 * The service condition is state a migration has to carry: it is what a
 * driver blocked in a write is waiting on, and a stream without it lands
 * the guest back in the wait with nothing left to end it.
 */
static int sgi_ioc3_parallel_post_load(void *opaque, int version_id)
{
    SGIIOC3ParallelState *s = opaque;

    qemu_set_irq(s->irq[SGI_IOC3_PAR_IRQ_SERVICE], s->service);
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc3_parallel = {
    .name = "sgi-ioc3-parallel",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = sgi_ioc3_parallel_post_load,
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
        VMSTATE_BOOL(service, SGIIOC3ParallelState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_parallel_properties[] = {
    DEFINE_PROP_CHR("chardev", SGIIOC3ParallelState, chr),
    DEFINE_PROP_LINK("dma-memory", SGIIOC3ParallelState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void sgi_ioc3_parallel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 parallel port";
    dc->realize = sgi_ioc3_parallel_realize;
    dc->unrealize = sgi_ioc3_parallel_unrealize;
    dc->vmsd = &vmstate_sgi_ioc3_parallel;
    /* Instantiated only by the IOC3, which maps both of its regions. */
    dc->user_creatable = false;
    rc->phases.hold = sgi_ioc3_parallel_reset_hold;
    device_class_set_props(dc, sgi_ioc3_parallel_properties);
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
