/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC — the registers a guest reads before it has a console.
 *
 * The Hub is four units behind one 8 MB register window: a processor interface,
 * a memory and directory controller, an I/O interface and a network interface.
 * This model implements the registers an operating system touches on its way to
 * switching its console away from the firmware, which is a small and specific
 * set, and reports anything else rather than answering it. The rest arrives
 * with the interrupt path and the crossbar.
 *
 * Two of these registers decide things a guest can never recover from getting
 * wrong. The processor-number register tells it which of a node's two
 * processors it is, and so which copy of every per-processor register to use.
 * The network-interface status register tells it whether the machine numbers
 * nodes in M mode or N mode, and every node-relative address it forms
 * afterwards depends on the answer. Both must read zero on an unprogrammed
 * Origin 200, which is what their reset values say.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-hub.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

/* Processor interface, at the base of the window. */
#define HUB_PI_REGION_PRESENT   0x000018  /* which memory regions exist */
#define HUB_PI_PROCESSOR_NUMBER 0x000020  /* which processor is reading */
#define HUB_PI_CALIAS_SIZE      0x000028  /* size of the cached alias */
#define HUB_PI_INT_PEND0        0x000098  /* pending interrupts, first bank */
#define HUB_PI_INT_PEND1        0x0000a0  /* pending interrupts, second bank */
#define HUB_PI_INT_MASK0_A      0x0000a8  /* first bank reaching processor A */
#define HUB_PI_INT_MASK1_A      0x0000b0  /* second bank reaching processor A */
#define HUB_PI_INT_MASK0_B      0x0000b8  /* first bank reaching processor B */
#define HUB_PI_INT_MASK1_B      0x0000c0  /* second bank reaching processor B */

/* Network interface. */
#define HUB_NI_STATUS           0x600000  /* status, revision and identity */

/*
 * The status register's fields. Only three of them matter to a booting guest,
 * and all three read zero out of reset: the machine is in M mode, this Hub is
 * node zero, and no network link has come up because a single-node machine has
 * none. The region-size bit resets set, selecting one region per node.
 */
#define NI_STATUS_MORE_NODES    (1ULL << 18)
#define NI_STATUS_REGION_SIZE   (1ULL << 17)
#define NI_STATUS_NODE_SHIFT    8
#define NI_STATUS_NODE_MASK     (0x1ffULL << NI_STATUS_NODE_SHIFT)
#define NI_STATUS_REVISION      (2ULL << 4)

#define NI_STATUS_RESET         (NI_STATUS_REGION_SIZE | NI_STATUS_REVISION)
#define NI_STATUS_WRITABLE      (NI_STATUS_MORE_NODES | \
                                 NI_STATUS_REGION_SIZE | \
                                 NI_STATUS_NODE_MASK)

/* The cached-alias size is a four-bit code, and resets to the largest. */
#define PI_CALIAS_SIZE_MASK     0xf
#define PI_CALIAS_SIZE_RESET    15

static uint64_t sgi_hub_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIHubState *s = opaque;

    switch (addr) {
    case HUB_PI_REGION_PRESENT:
        return s->pi_region_present;
    case HUB_PI_PROCESSOR_NUMBER:
        /*
         * Which processor of the pair is reading. One processor per node for
         * now, so always the first; when a second exists this has to answer
         * differently depending on which one asked, which the register window
         * alone cannot tell.
         */
        return 0;
    case HUB_PI_CALIAS_SIZE:
        return s->pi_calias_size;
    case HUB_PI_INT_PEND0:
        return s->pi_int_pend[0];
    case HUB_PI_INT_PEND1:
        return s->pi_int_pend[1];
    case HUB_PI_INT_MASK0_A:
        return s->pi_int_mask[0];
    case HUB_PI_INT_MASK1_A:
        return s->pi_int_mask[1];
    case HUB_PI_INT_MASK0_B:
        return s->pi_int_mask[2];
    case HUB_PI_INT_MASK1_B:
        return s->pi_int_mask[3];
    case HUB_NI_STATUS:
        return s->ni_status;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: read of unimplemented register 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void sgi_hub_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    SGIHubState *s = opaque;

    switch (addr) {
    case HUB_PI_REGION_PRESENT:
        s->pi_region_present = val;
        break;
    case HUB_PI_CALIAS_SIZE:
        s->pi_calias_size = val & PI_CALIAS_SIZE_MASK;
        break;
    case HUB_PI_INT_MASK0_A:
        s->pi_int_mask[0] = val;
        break;
    case HUB_PI_INT_MASK1_A:
        s->pi_int_mask[1] = val;
        break;
    case HUB_PI_INT_MASK0_B:
        s->pi_int_mask[2] = val;
        break;
    case HUB_PI_INT_MASK1_B:
        s->pi_int_mask[3] = val;
        break;
    case HUB_NI_STATUS:
        /*
         * Firmware assigns node numbers and chooses the region size through
         * this register, so those fields are writable; the link state and the
         * chip's identity are what the hardware reports and are not.
         */
        s->ni_status = (s->ni_status & ~NI_STATUS_WRITABLE) |
                       (val & NI_STATUS_WRITABLE);
        break;
    case HUB_PI_PROCESSOR_NUMBER:
    case HUB_PI_INT_PEND0:
    case HUB_PI_INT_PEND1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to unimplemented "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps sgi_hub_ops = {
    .read = sgi_hub_read,
    .write = sgi_hub_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void sgi_hub_reset_hold(Object *obj, ResetType type)
{
    SGIHubState *s = SGI_HUB(obj);

    s->pi_region_present = ~0ULL;
    s->pi_calias_size = PI_CALIAS_SIZE_RESET;
    memset(s->pi_int_pend, 0, sizeof(s->pi_int_pend));
    memset(s->pi_int_mask, 0, sizeof(s->pi_int_mask));
    s->ni_status = NI_STATUS_RESET |
                   ((uint64_t)s->nasid << NI_STATUS_NODE_SHIFT);
}

static void sgi_hub_realize(DeviceState *dev, Error **errp)
{
    SGIHubState *s = SGI_HUB(dev);

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_hub_ops, s,
                          "sgi-hub.regs", SGI_HUB_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
}

static const VMStateDescription vmstate_sgi_hub = {
    .name = "sgi-hub",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(pi_region_present, SGIHubState),
        VMSTATE_UINT32(pi_calias_size, SGIHubState),
        VMSTATE_UINT64_ARRAY(pi_int_pend, SGIHubState, 2),
        VMSTATE_UINT64_ARRAY(pi_int_mask, SGIHubState, 4),
        VMSTATE_UINT64(ni_status, SGIHubState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_hub_properties[] = {
    DEFINE_PROP_UINT32("nasid", SGIHubState, nasid, 0),
};

static void sgi_hub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI SN0 Hub ASIC";
    dc->realize = sgi_hub_realize;
    dc->vmsd = &vmstate_sgi_hub;
    device_class_set_props(dc, sgi_hub_properties);
    rc->phases.hold = sgi_hub_reset_hold;
}

static const TypeInfo sgi_hub_types[] = {
    {
        .name = TYPE_SGI_HUB,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIHubState),
        .class_init = sgi_hub_class_init,
    },
};

DEFINE_TYPES(sgi_hub_types)
