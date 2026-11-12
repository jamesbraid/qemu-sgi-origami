/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic Solutions HDSP PCI audio card: the DSP56301 host-port functions.
 *
 * Each DSP is a PCI function on the i960's private secondary bus: its HI32
 * host port, wrapped around the DSP56300 interpreter in hw/coproc/dsp56300.
 * The host faces and the bridge are in sgi-sonic-hdsp.c.
 *
 * Hardware reference: docs/system/mips/sgi/hardware/sonic-hdsp.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/misc/sgi-sonic-hdsp.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define DSP_VENDOR_ID       0x1057      /* hardwired, DSP56301 CDID/CVID */
#define DSP_DEVICE_ID       0x1801
#define DSP_BAR_SIZE        (64 * KiB)

#define DSP_HSTR_TRDY       (1u << 0)
#define DSP_HSTR_HTRQ       (1u << 1)
#define DSP_HSTR_HRRQ       (1u << 2)

static uint32_t dsp_ext_read(void *opaque, DSP56300Space space, uint32_t addr)
{
    SonicHDSPDSPState *s = opaque;
    uint32_t off = addr - SONIC_DSP_EXT_BASE;

    if (off < SONIC_DSP_EXT_WORDS) {
        if (space == DSP56300_SPACE_P) {
            return s->ext_p[off];
        }
        if (space == DSP56300_SPACE_Y) {
            return s->ext_y[off];
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u read of %c:0x%06x\n",
                  s->index, "XYP"[space], addr);
    return 0;
}

static void dsp_ext_write(void *opaque, DSP56300Space space, uint32_t addr,
                          uint32_t val)
{
    SonicHDSPDSPState *s = opaque;
    uint32_t off = addr - SONIC_DSP_EXT_BASE;

    if (off < SONIC_DSP_EXT_WORDS) {
        if (space == DSP56300_SPACE_P) {
            s->ext_p[off] = val & 0xffffff;
            return;
        }
        if (space == DSP56300_SPACE_Y) {
            s->ext_y[off] = val & 0xffffff;
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u write of %c:0x%06x = "
                  "0x%06x\n", s->index, "XYP"[space], addr, val);
}

static const DSP56300Ops dsp_ops = {
    .read = dsp_ext_read,
    .write = dsp_ext_write,
};

static uint32_t dsp_hstr(SonicHDSPDSPState *s)
{
    uint32_t v = 0;

    if (!s->tx_count) {
        v |= DSP_HSTR_TRDY;
    }
    if (s->tx_count < SONIC_DSP_FIFO_DEPTH) {
        v |= DSP_HSTR_HTRQ;
    }
    if (s->rx_count) {
        v |= DSP_HSTR_HRRQ;
    }
    return v;
}

static uint64_t dsp_bar_read(void *opaque, hwaddr off, unsigned size)
{
    SonicHDSPDSPState *s = opaque;
    uint32_t v = 0;

    if (s->in_reset) {
        return UINT32_MAX >> (32 - 8 * size);
    }
    switch (off & ~3) {
    case 0x10:
        v = s->hctr;
        break;
    case 0x14:
        v = dsp_hstr(s);
        break;
    case 0x18:
        v = s->hcvr;
        break;
    default:
        if (off >= 0x1c) {
            /* Receive data port: the oldest word the DSP wrote. */
            if (s->rx_count) {
                v = s->rx_fifo[0];
                memmove(s->rx_fifo, s->rx_fifo + 1,
                        (--s->rx_count) * sizeof(uint32_t));
            }
        }
        break;
    }
    return v >> ((off & 3) * 8);
}

static void dsp_bar_write(void *opaque, hwaddr off, uint64_t val,
                          unsigned size)
{
    SonicHDSPDSPState *s = opaque;

    if (s->in_reset) {
        return;
    }
    switch (off & ~3) {
    case 0x10:
        s->hctr = val;
        break;
    case 0x14:
        break;                  /* HSTR is read-only from the host */
    case 0x18:
        s->hcvr = (val & 0x80ff);   /* HC, HV and HNMI; nothing clears HC */
        break;
    default:
        if (off >= 0x1c && s->tx_count < SONIC_DSP_FIFO_DEPTH) {
            s->tx_fifo[s->tx_count++] = val & 0xffffff;
        }
        break;
    }
}

static const MemoryRegionOps dsp_bar_ops = {
    .read = dsp_bar_read,
    .write = dsp_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* A held-in-reset or hidden chip does not answer a configuration cycle. */
static bool dsp_answers(SonicHDSPDSPState *s)
{
    if (s->in_reset || !s->pci_mode) {
        return false;       /* in reset, or the HI32 is not yet in PCI mode */
    }
    return s->hub->iop_access ||
           sonic_hdsp_dsp_visible(s->hub, PCI_SLOT(PCI_DEVICE(s)->devfn));
}

static uint32_t dsp_config_read(PCIDevice *d, uint32_t addr, int len)
{
    if (!dsp_answers(SGI_SONIC_HDSP_DSP(d))) {
        return UINT32_MAX >> (32 - 8 * len);
    }
    return pci_default_read_config(d, addr, len);
}

static void dsp_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                             int len)
{
    if (!dsp_answers(SGI_SONIC_HDSP_DSP(d))) {
        return;
    }
    pci_default_write_config(d, addr, val, len);
}

static void dsp_clear(SonicHDSPDSPState *s)
{
    memset(s->pmem, 0, sizeof(s->pmem));
    memset(s->xmem, 0, sizeof(s->xmem));
    memset(s->ymem, 0, sizeof(s->ymem));
    memset(s->ext_p, 0, sizeof(s->ext_p));
    memset(s->ext_y, 0, sizeof(s->ext_y));
    s->hctr = 0;
    s->hcvr = 0;
    s->tx_count = 0;
    s->rx_count = 0;
}

/*
 * The glue's reset byte: entering reset clears the core, its memories and its
 * host port; leaving it releases the core. The core has nothing to run in
 * this stage (no bootstrap ROM), so release only lets the host port answer.
 */
void sonic_hdsp_dsp_reset_line(SonicHDSPState *h, unsigned n, bool held)
{
    SonicHDSPDSPState *s = h->dsp[n];

    if (!s) {
        return;
    }
    if (held) {
        dsp_clear(s);
        dsp56300_reset(&s->core, 0xff0000);
    }
    s->in_reset = held;
    /*
     * The bootstrap ROM (not modelled yet) would put the HI32 in PCI target
     * mode; until it exists, release does, so the function answers.
     */
    s->pci_mode = !held;
}

static void dsp_realize(PCIDevice *d, Error **errp)
{
    SonicHDSPDSPState *s = SGI_SONIC_HDSP_DSP(d);

    if (!s->hub) {
        error_setg(errp, "%s is created by %s", TYPE_SGI_SONIC_HDSP_DSP,
                   TYPE_SGI_SONIC_HDSP);
        return;
    }
    memory_region_init_io(&s->bar0, OBJECT(d), &dsp_bar_ops, s,
                          "sgi-sonic-hdsp.dsp-host-port", DSP_BAR_SIZE);
    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_config_set_interrupt_pin(d->config, 1);
    /* CSID reads 0 until the DSP's own boot program writes it. */
    pci_set_word(d->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(d->config + PCI_SUBSYSTEM_ID, 0);
    dsp56300_init(&s->core, &dsp_ops, s);
    dsp56300_set_memory(&s->core, DSP56300_SPACE_P, s->pmem,
                        SONIC_DSP_PMEM_WORDS);
    dsp56300_set_memory(&s->core, DSP56300_SPACE_X, s->xmem,
                        SONIC_DSP_XMEM_WORDS);
    dsp56300_set_memory(&s->core, DSP56300_SPACE_Y, s->ymem,
                        SONIC_DSP_YMEM_WORDS);
    s->in_reset = true;
}

static void dsp_reset(DeviceState *dev)
{
    SonicHDSPDSPState *s = SGI_SONIC_HDSP_DSP(dev);

    dsp_clear(s);
    dsp56300_reset(&s->core, 0xff0000);
    s->pci_mode = false;
    s->in_reset = true;     /* held until the glue's reset byte releases it */
}

static int dsp_post_load(void *opaque, int version_id)
{
    SonicHDSPDSPState *s = opaque;

    if (s->tx_count > SONIC_DSP_FIFO_DEPTH ||
        s->rx_count > SONIC_DSP_FIFO_DEPTH) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sonic_hdsp_dsp = {
    .name = "sgi-sonic-hdsp-dsp",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dsp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SonicHDSPDSPState),
        VMSTATE_DSP56300(core, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(pmem, SonicHDSPDSPState, SONIC_DSP_PMEM_WORDS),
        VMSTATE_UINT32_ARRAY(xmem, SonicHDSPDSPState, SONIC_DSP_XMEM_WORDS),
        VMSTATE_UINT32_ARRAY(ymem, SonicHDSPDSPState, SONIC_DSP_YMEM_WORDS),
        VMSTATE_UINT32_ARRAY(ext_p, SonicHDSPDSPState, SONIC_DSP_EXT_WORDS),
        VMSTATE_UINT32_ARRAY(ext_y, SonicHDSPDSPState, SONIC_DSP_EXT_WORDS),
        VMSTATE_UINT32(hctr, SonicHDSPDSPState),
        VMSTATE_UINT32(hcvr, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(tx_fifo, SonicHDSPDSPState,
                             SONIC_DSP_FIFO_DEPTH),
        VMSTATE_UINT32(tx_count, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(rx_fifo, SonicHDSPDSPState,
                             SONIC_DSP_FIFO_DEPTH),
        VMSTATE_UINT32(rx_count, SonicHDSPDSPState),
        VMSTATE_BOOL(in_reset, SonicHDSPDSPState),
        VMSTATE_BOOL(pci_mode, SonicHDSPDSPState),
        VMSTATE_END_OF_LIST()
    }
};

static void dsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = dsp_realize;
    k->config_read = dsp_config_read;
    k->config_write = dsp_config_write;
    k->vendor_id = DSP_VENDOR_ID;
    k->device_id = DSP_DEVICE_ID;
    k->class_id = 0x0480;
    dc->desc = "Sonic HDSP DSP56301 host-port function (secondary bus)";
    dc->vmsd = &vmstate_sonic_hdsp_dsp;
    device_class_set_legacy_reset(dc, dsp_reset);
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo dsp_info = {
    .name = TYPE_SGI_SONIC_HDSP_DSP,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SonicHDSPDSPState),
    .class_init = dsp_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void sonic_hdsp_dsp_register_types(void)
{
    type_register_static(&dsp_info);
}

type_init(sonic_hdsp_dsp_register_types)
