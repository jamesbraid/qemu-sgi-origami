/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI IOC3 parallel port registers, DMA, and character backend. */
/*
 * IOC3 maps the DMA block and byte-wide ECP registers separately. See
 * docs/specs/sgi-ioc3.rst for the register contract.
 *
 * Output drains synchronously to a character backend; reverse transfers have
 * no peripheral and remain armed. Test mode uses the selected FIFO capacity.
 * The TL16PIR552 policy supplies its fixed configuration and test FIFO only,
 * leaving board interrupt and DMA wiring unspecified.
 *
 * The shared DMA model asserts service at terminal count and clears it on an
 * ECR write. Its interrupt-control polarity remains unresolved.
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

#define TL16_CNFGA_IMPLEMENTATION_ID (1u << 4)
#define TL16_CNFGA_RESERVED          0x0fu
#define TL16_CNFGB_IRQ7              (1u << 3)
#define TL16_CNFGB_DMA3              3u

struct SGIIOC3ParallelPart {
    uint16_t fifo_depth;
    uint8_t config_a;
    uint8_t config_b;
    bool readonly_config;
    bool overwrite_full;
};

static const SGIIOC3ParallelPart sgi_ioc3_parallel_parts[] = {
    [SGI_IOC3_PAR_PART_GENERIC] = { 0 },
    [SGI_IOC3_PAR_PART_TL16PIR552] = {
        .fifo_depth = SGI_IOC3_PAR_TL16_FIFO_DEPTH,
        .config_a = TL16_CNFGA_IMPLEMENTATION_ID | TL16_CNFGA_RESERVED,
        .config_b = TL16_CNFGB_IRQ7 | TL16_CNFGB_DMA3,
        .readonly_config = true,
        .overwrite_full = true,
    },
};

static unsigned int sgi_ioc3_parallel_mode(SGIIOC3ParallelState *s)
{
    return (s->extended & SGI_IOC3_PAR_ECR_MODE) >>
           SGI_IOC3_PAR_ECR_MODE_SHIFT;
}

/* Synchronous transfers leave the read-only busy flags clear. */
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
 * Use the chip's bus-master address space to honor Bridge translation.
 * A failed fetch leaves the context armed and stops the descriptor chain.
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

    if (s->count[context] & SGI_IOC3_PAR_COUNT_LAST) {
        sgi_ioc3_parallel_set_service(s, true);
    }

    return true;
}

/* Wait for both enables before draining context A, then context B. */
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

    /* No peripheral is attached; report no fault and no other input. */
    case SGI_IOC3_PAR_ECP_STATUS:
        return SGI_IOC3_PAR_STATUS_IDLE;

    case SGI_IOC3_PAR_ECP_CONTROL:
        return s->control;

    case SGI_IOC3_PAR_ECP_FIFO:
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_CONFIG) {
            return s->part->readonly_config ? s->part->config_a : s->config_a;
        }
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_TEST &&
            !fifo8_is_empty(&s->test_fifo)) {
            return fifo8_pop(&s->test_fifo);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-parallel: read of the empty FIFO\n");
        return 0;

    case SGI_IOC3_PAR_ECP_CONFIG_B:
        if (s->part->readonly_config &&
            sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_CONFIG) {
            /* IRQ routing is unknown; bit 6 reads as an idle-line sample. */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc3-parallel: TL16PIR552 CNFGB IRQ sample "
                          "is unmodeled\n");
            return s->part->config_b;
        }
        return s->config_b;

    case SGI_IOC3_PAR_ECP_EXTENDED:
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_TEST) {
            uint8_t status = s->extended;

            if (fifo8_is_empty(&s->test_fifo)) {
                status |= SGI_IOC3_PAR_ECR_FIFO_EMPTY;
            }
            if (s->fifo_depth && fifo8_is_full(&s->test_fifo)) {
                status |= SGI_IOC3_PAR_ECR_FIFO_FULL;
            }
            return status;
        }
        /* Other modes have no modeled peripheral to hold up the FIFO. */
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

    /* Preserve unspecified control bits on readback. */
    case SGI_IOC3_PAR_ECP_CONTROL:
        s->control = val;
        return;

    case SGI_IOC3_PAR_ECP_FIFO:
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_CONFIG) {
            if (s->part->readonly_config) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-ioc3-parallel: write to read-only "
                              "TL16PIR552 CNFGA\n");
                return;
            }
            s->config_a = val;
            return;
        }
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_TEST) {
            if (!s->fifo_depth) {
                return;
            }
            if (fifo8_is_full(&s->test_fifo)) {
                if (s->part->overwrite_full) {
                    /*
                     * TI leaves overrun readback unspecified; dropping the
                     * oldest byte is a model approximation.
                     */
                    fifo8_pop(&s->test_fifo);
                } else {
                    qemu_log_mask(LOG_UNIMP,
                                  "sgi-ioc3-parallel: write to a full "
                                  "test FIFO\n");
                    return;
                }
            }
            fifo8_push(&s->test_fifo, val);
            return;
        }
        {
            uint8_t byte = val;

            qemu_chr_fe_write_all(&s->chr, &byte, sizeof(byte));
        }
        return;

    case SGI_IOC3_PAR_ECP_CONFIG_B:
        if (s->part->readonly_config &&
            sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_CONFIG) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc3-parallel: write to read-only "
                          "TL16PIR552 CNFGB\n");
            return;
        }
        s->config_b = val;
        return;

    /*
     * Acknowledge service before starting DMA so a new completion can assert
     * it again. FIFO status bits are read-only.
     */
    case SGI_IOC3_PAR_ECP_EXTENDED:
        sgi_ioc3_parallel_set_service(s, false);
        s->extended = val & SGI_IOC3_PAR_ECR_STORED;
        if (sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_COMPAT ||
            sgi_ioc3_parallel_mode(s) == SGI_IOC3_PAR_MODE_BYTE) {
            fifo8_reset(&s->test_fifo);
        }
        sgi_ioc3_parallel_dma_run(s);
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3-parallel: write of 0x%" PRIx64 " to unnamed "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
}

/* Byte implementation width avoids repeated register effects. */
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
    if (s->parallel_part >= ARRAY_SIZE(sgi_ioc3_parallel_parts)) {
        error_setg(errp, TYPE_SGI_IOC3_PARALLEL " has unknown parallel-part "
                   "%u", s->parallel_part);
        return;
    }
    s->part = &sgi_ioc3_parallel_parts[s->parallel_part];
    if (s->part->fifo_depth) {
        if (s->fifo_depth && s->fifo_depth != s->part->fifo_depth) {
            error_setg(errp, TYPE_SGI_IOC3_PARALLEL " TL16PIR552 requires a "
                       "16-byte test FIFO");
            return;
        }
        s->fifo_depth = s->part->fifo_depth;
    }
    fifo8_create(&s->test_fifo, s->fifo_depth);
    address_space_init(&s->dma_as, s->dma_mr, "sgi-ioc3-parallel-dma");
}

static void sgi_ioc3_parallel_unrealize(DeviceState *dev)
{
    SGIIOC3ParallelState *s = SGI_IOC3_PARALLEL(dev);

    address_space_destroy(&s->dma_as);
    fifo8_destroy(&s->test_fifo);
}

/*
 * FIFO flags are derived on read. Zero is the generic model's choice for
 * control and configuration registers with unverified reset values.
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
    fifo8_reset(&s->test_fifo);

    sgi_ioc3_parallel_set_service(s, false);
}

static int sgi_ioc3_parallel_post_load(void *opaque, int version_id)
{
    SGIIOC3ParallelState *s = opaque;

    if (s->test_fifo.num > s->test_fifo.capacity ||
        (s->fifo_depth ? s->test_fifo.head >= s->fifo_depth :
         s->test_fifo.head)) {
        return -EINVAL;
    }
    qemu_set_irq(s->irq[SGI_IOC3_PAR_IRQ_SERVICE], s->service);
    return 0;
}

static bool sgi_ioc3_parallel_has_test_fifo(void *opaque, int version_id)
{
    return version_id >= 3;
}

static const VMStateDescription vmstate_sgi_ioc3_parallel = {
    .name = "sgi-ioc3-parallel",
    .version_id = 4,
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
        VMSTATE_UINT16_EQUAL_V(parallel_part, SGIIOC3ParallelState, 4),
        VMSTATE_UINT16_EQUAL_V(fifo_depth, SGIIOC3ParallelState, 3),
        VMSTATE_FIFO8_TEST(test_fifo, SGIIOC3ParallelState,
                           sgi_ioc3_parallel_has_test_fifo),
        VMSTATE_BOOL(service, SGIIOC3ParallelState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_parallel_properties[] = {
    DEFINE_PROP_CHR("chardev", SGIIOC3ParallelState, chr),
    DEFINE_PROP_UINT16("parallel-part", SGIIOC3ParallelState, parallel_part,
                       SGI_IOC3_PAR_PART_GENERIC),
    DEFINE_PROP_UINT16("fifo-depth", SGIIOC3ParallelState, fifo_depth, 0),
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
