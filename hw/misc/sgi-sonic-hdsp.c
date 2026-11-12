/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic Solutions HDSP PCI audio card: the host faces.
 *
 * Function 0 is the i960 Rx's PCI-to-PCI bridge unit (a QEMU PCI bridge, so
 * the host's type 1 cycles reach the private secondary bus through the PCI
 * core) and owns the whole board. Function 1 is the primary ATU, whose BAR0
 * is the messaging unit and the window onto the i960's local bus. The four
 * DSP56301 host-port functions sit on the secondary bus. The processor and
 * its units are in sgi-sonic-hdsp-iop.c.
 *
 * Hardware reference: docs/system/mips/sgi/hardware/sonic-hdsp.rst.
 *
 * Stage 1 stubs the DSP side: the four DSP56300 cores are instantiated,
 * reset and given their memories, but nothing runs them (no bootstrap ROM,
 * no ESSI, SCI, timers or DMA), the interrupt-out lines stay low, and there
 * is no converter box or audio output.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/misc/sgi-sonic-hdsp.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define SONIC_VENDOR_ID     0x1263
#define SONIC_HDSP_ATU_DEV  0x05dc
#define BAR0_SIZE           (128 * MiB)

#define DSP_VENDOR_ID       0x1057      /* hardwired, DSP56301 CDID/CVID */
#define DSP_DEVICE_ID       0x1801
#define DSP_BAR_SIZE        (64 * KiB)

#define DSP_HSTR_TRDY       (1u << 0)
#define DSP_HSTR_HTRQ       (1u << 1)
#define DSP_HSTR_HRRQ       (1u << 2)

/* The retry bit of the bridge unit's extended control register. */
bool sonic_hdsp_retry(SonicHDSPState *h)
{
    return PCI_DEVICE(h)->config[0x40] & 0x04;
}

/* ------------------------------------------------------------------ */
/* Function 1: the ATU and its BAR0                                   */
/* ------------------------------------------------------------------ */

static uint64_t atu_bar_read(void *opaque, hwaddr off, unsigned size)
{
    SonicHDSPATUState *a = opaque;
    SonicHDSPState *h = a->hub;
    uint32_t local;

    if (off < 0x1000) {
        return sonic_hdsp_mu_host_read(h, off, size);
    }
    /* The Rx rule: the limit's bits cleared, ORed with the translate value. */
    local = ((uint32_t)off & ~h->atu_x[0]) | h->atu_x[1];
    return sonic_hdsp_local_read(h, local, size);
}

static void atu_bar_write(void *opaque, hwaddr off, uint64_t val,
                          unsigned size)
{
    SonicHDSPATUState *a = opaque;
    SonicHDSPState *h = a->hub;

    if (off < 0x1000) {
        sonic_hdsp_mu_host_write(h, off, val, size);
        return;
    }
    sonic_hdsp_local_write(h, ((uint32_t)off & ~h->atu_x[0]) | h->atu_x[1],
                           val, size);
    sonic_hdsp_iop_kick(h);
}

static const MemoryRegionOps atu_bar_ops = {
    .read = atu_bar_read,
    .write = atu_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static uint32_t atu_config_read(PCIDevice *d, uint32_t addr, int len)
{
    SonicHDSPState *h = SGI_SONIC_HDSP_ATU(d)->hub;

    if (sonic_hdsp_retry(h)) {
        h->config_retries++;
        return UINT32_MAX >> (32 - 8 * len);
    }
    return pci_default_read_config(d, addr, len);
}

static void atu_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                             int len)
{
    SonicHDSPState *h = SGI_SONIC_HDSP_ATU(d)->hub;

    if (sonic_hdsp_retry(h)) {
        h->config_retries++;
        return;
    }
    pci_default_write_config(d, addr, val, len);
}

static void atu_realize(PCIDevice *d, Error **errp)
{
    SonicHDSPATUState *a = SGI_SONIC_HDSP_ATU(d);

    if (!a->hub) {
        error_setg(errp, "%s is created by %s", TYPE_SGI_SONIC_HDSP_ATU,
                   TYPE_SGI_SONIC_HDSP);
        return;
    }
    memory_region_init_io(&a->bar0, OBJECT(d), &atu_bar_ops, a,
                          "sgi-sonic-hdsp.bar0", BAR0_SIZE);
    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &a->bar0);
    /* Everything past the standard header is local-only. */
    memset(d->wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    /* The firmware rewrites the identity, so it cannot be checked on load. */
    pci_set_word(d->cmask + PCI_VENDOR_ID, 0);
    pci_set_word(d->cmask + PCI_DEVICE_ID, 0);
    pci_set_word(d->cmask + PCI_CLASS_DEVICE, 0);
    d->cmask[PCI_CLASS_PROG] = 0;
    d->cmask[PCI_REVISION_ID] = 0;
}

static const VMStateDescription vmstate_sonic_hdsp_atu = {
    .name = "sgi-sonic-hdsp-atu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SonicHDSPATUState),
        VMSTATE_END_OF_LIST()
    }
};

static void atu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = atu_realize;
    k->config_read = atu_config_read;
    k->config_write = atu_config_write;
    k->vendor_id = 0x8086;
    k->device_id = 0x1960;
    k->class_id = 0x0580;
    dc->desc = "Sonic HDSP primary ATU and messaging unit (function 1)";
    dc->vmsd = &vmstate_sonic_hdsp_atu;
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo atu_info = {
    .name = TYPE_SGI_SONIC_HDSP_ATU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SonicHDSPATUState),
    .class_init = atu_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* ------------------------------------------------------------------ */
/* The DSP56301 host-port functions on the secondary bus              */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* The i960's outbound configuration cycles on the secondary bus      */
/* ------------------------------------------------------------------ */

static PCIDevice *secondary_device(SonicHDSPState *h, uint32_t addr)
{
    PCIBus *sec = pci_bridge_get_sec_bus(PCI_BRIDGE(h));
    uint32_t idsel = addr & 0xfffff800u;
    unsigned line, devfn;

    if (!idsel || (idsel & (idsel - 1))) {
        return NULL;
    }
    line = ctz32(idsel);
    if (line < 16) {
        return NULL;            /* S_AD11 to S_AD15 are private to the chip */
    }
    devfn = ((line - 16) << 3) | ((addr >> 8) & 7);
    return sec->devices[devfn];
}

uint32_t sonic_hdsp_secondary_config_read(SonicHDSPState *h, uint32_t addr)
{
    PCIDevice *d = secondary_device(h, addr);

    if (!d) {
        return UINT32_MAX;
    }
    return pci_host_config_read_common(d, addr & 0xfc,
                                       PCI_CONFIG_SPACE_SIZE, 4);
}

void sonic_hdsp_secondary_config_write(SonicHDSPState *h, uint32_t addr,
                                       uint32_t val)
{
    PCIDevice *d = secondary_device(h, addr);

    if (d) {
        pci_host_config_write_common(d, addr & 0xfc, PCI_CONFIG_SPACE_SIZE,
                                     val, 4);
    }
}

/* ------------------------------------------------------------------ */
/* Function 0: the bridge unit                                        */
/* ------------------------------------------------------------------ */

static uint32_t hdsp_config_read(PCIDevice *d, uint32_t addr, int len)
{
    SonicHDSPState *h = SGI_SONIC_HDSP(d);

    if (sonic_hdsp_retry(h)) {
        h->config_retries++;
        return UINT32_MAX >> (32 - 8 * len);
    }
    return pci_default_read_config(d, addr, len);
}

static void hdsp_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                              int len)
{
    SonicHDSPState *h = SGI_SONIC_HDSP(d);

    if (sonic_hdsp_retry(h)) {
        h->config_retries++;
        return;
    }
    pci_bridge_write_config(d, addr, val, len);
}

static void hdsp_realize(PCIDevice *d, Error **errp)
{
    ERRP_GUARD();
    SonicHDSPState *h = SGI_SONIC_HDSP(d);
    PCIBus *bus = pci_get_bus(d);
    PCIBus *sec;
    PCIDevice *f1;

    sonic_hdsp_iop_realize(h, errp);
    if (*errp) {
        return;
    }
    pci_bridge_initfn(d, TYPE_PCI_BUS);
    d->config[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_MULTI_FUNCTION;
    /* The bridge's own configuration beyond the header is the chip's. */
    memset(d->wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    pci_set_word(d->cmask + PCI_VENDOR_ID, 0);
    pci_set_word(d->cmask + PCI_DEVICE_ID, 0);
    pci_set_word(d->cmask + PCI_CLASS_DEVICE, 0);
    d->cmask[PCI_CLASS_PROG] = 0;
    d->cmask[PCI_REVISION_ID] = 0;
    d->cmask[PCI_HEADER_TYPE] = 0;
    /* The bridge's subsystem id sits where a capability pointer would. */
    d->cmask[PCI_CAPABILITY_LIST] = 0;

    /* Function 1, the ATU, shares the slot. */
    f1 = pci_new_multifunction(PCI_DEVFN(PCI_SLOT(d->devfn), 1),
                               TYPE_SGI_SONIC_HDSP_ATU);
    h->atu = SGI_SONIC_HDSP_ATU(f1);
    h->atu->hub = h;
    object_property_add_child(OBJECT(h), "atu", OBJECT(f1));
    if (!pci_realize_and_unref(f1, bus, errp)) {
        goto fail;
    }

    sec = pci_bridge_get_sec_bus(PCI_BRIDGE(h));
    for (unsigned n = 0; n < SONIC_HDSP_NUM_DSP; n++) {
        PCIDevice *dsp = pci_new(PCI_DEVFN(1 + n, 0), TYPE_SGI_SONIC_HDSP_DSP);
        g_autofree char *name = g_strdup_printf("dsp[%u]", n);

        h->dsp[n] = SGI_SONIC_HDSP_DSP(dsp);
        h->dsp[n]->hub = h;
        h->dsp[n]->index = n;
        object_property_add_child(OBJECT(h), name, OBJECT(dsp));
        if (!pci_realize_and_unref(dsp, sec, errp)) {
            goto fail;
        }
    }
    return;

fail:
    pci_bridge_exitfn(d);
    sonic_hdsp_iop_unrealize(h);
}

static void hdsp_exit(PCIDevice *d)
{
    pci_bridge_exitfn(d);
    sonic_hdsp_iop_unrealize(SGI_SONIC_HDSP(d));
}

static void hdsp_reset(DeviceState *qdev)
{
    pci_bridge_reset(qdev);
    sonic_hdsp_iop_reset(SGI_SONIC_HDSP(qdev));
}

static void hdsp_instance_init(Object *obj)
{
    /* A multifunction device from the start: no multifunction=on needed. */
    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;
    object_property_add_uint64_ptr(obj, "config-retries",
                                   &SGI_SONIC_HDSP(obj)->config_retries,
                                   OBJ_PROP_FLAG_READ);
}

static int hdsp_pre_save(void *opaque)
{
    sonic_hdsp_iop_pre_save(opaque);
    return 0;
}

static int hdsp_post_load(void *opaque, int version_id)
{
    if (!sonic_hdsp_iop_state_valid(opaque)) {
        return -EINVAL;
    }
    sonic_hdsp_iop_post_load(opaque);
    return 0;
}

static const VMStateDescription vmstate_sonic_hdsp_dma = {
    .name = "sgi-sonic-hdsp/dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccr, SonicHDSPDMA),
        VMSTATE_UINT32(csr, SonicHDSPDMA),
        VMSTATE_UINT32(dar, SonicHDSPDMA),
        VMSTATE_UINT32(ndar, SonicHDSPDMA),
        VMSTATE_UINT32(padr, SonicHDSPDMA),
        VMSTATE_UINT32(puadr, SonicHDSPDMA),
        VMSTATE_UINT32(ladr, SonicHDSPDMA),
        VMSTATE_UINT32(bcr, SonicHDSPDMA),
        VMSTATE_UINT32(dcr, SonicHDSPDMA),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sonic_hdsp_timer = {
    .name = "sgi-sonic-hdsp/timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(trr, SonicHDSPTimer),
        VMSTATE_UINT32(tcr, SonicHDSPTimer),
        VMSTATE_UINT32(tmr, SonicHDSPTimer),
        VMSTATE_INT64(start_ns, SonicHDSPTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sonic_hdsp = {
    .name = "sgi-sonic-hdsp",
    .version_id = 1,
    .minimum_version_id = 1,
    .priority = MIG_PRI_PCI_BUS,
    .pre_save = hdsp_pre_save,
    .post_load = hdsp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent_obj, SonicHDSPState),
        VMSTATE_I960(cpu, SonicHDSPState),
        VMSTATE_UINT8_ARRAY(flash, SonicHDSPState, SONIC_HDSP_FLASH_SIZE),
        VMSTATE_BOOL(fl_loading, SonicHDSPState),
        VMSTATE_BOOL(fl_committing, SonicHDSPState),
        VMSTATE_BOOL(fl_protect, SonicHDSPState),
        VMSTATE_BOOL(fl_arm_protect, SonicHDSPState),
        VMSTATE_BOOL(fl_armed, SonicHDSPState),
        VMSTATE_UINT8(fl_seq, SonicHDSPState),
        VMSTATE_UINT32(fl_sector, SonicHDSPState),
        VMSTATE_UINT32(fl_last, SonicHDSPState),
        VMSTATE_UINT32(fl_count, SonicHDSPState),
        VMSTATE_UINT8_ARRAY(fl_data, SonicHDSPState, 256),
        VMSTATE_UINT8_ARRAY(fl_loaded, SonicHDSPState, 32),
        VMSTATE_INT64(fl_deadline_ns, SonicHDSPState),
        VMSTATE_INT64(fl_busy_until_ns, SonicHDSPState),
        VMSTATE_BOOL(fl_toggle, SonicHDSPState),
        VMSTATE_UINT32_ARRAY(atu_x, SonicHDSPState, 30),
        VMSTATE_UINT32_ARRAY(imr, SonicHDSPState, 2),
        VMSTATE_UINT32_ARRAY(omr, SonicHDSPState, 2),
        VMSTATE_UINT32(idr, SonicHDSPState),
        VMSTATE_UINT32(iisr, SonicHDSPState),
        VMSTATE_UINT32(iimr, SonicHDSPState),
        VMSTATE_UINT32(odr, SonicHDSPState),
        VMSTATE_UINT32(oisr, SonicHDSPState),
        VMSTATE_UINT32(oimr, SonicHDSPState),
        VMSTATE_UINT32(mucr, SonicHDSPState),
        VMSTATE_UINT32(qbar, SonicHDSPState),
        VMSTATE_UINT32_ARRAY(qptr, SonicHDSPState, 8),
        VMSTATE_UINT32(iar, SonicHDSPState),
        VMSTATE_UINT32(arsr, SonicHDSPState),
        VMSTATE_UINT32(awr, SonicHDSPState),
        VMSTATE_BOOL(nmi_latched, SonicHDSPState),
        VMSTATE_STRUCT_ARRAY(dma, SonicHDSPState, 3, 1,
                             vmstate_sonic_hdsp_dma, SonicHDSPDMA),
        VMSTATE_UINT32_ARRAY(mc, SonicHDSPState, 16),
        VMSTATE_UINT32_ARRAY(lba, SonicHDSPState, 2),
        VMSTATE_UINT32(reg_1610, SonicHDSPState),
        VMSTATE_STRUCT_ARRAY(timer, SonicHDSPState, 2, 1,
                             vmstate_sonic_hdsp_timer, SonicHDSPTimer),
        VMSTATE_UINT32_ARRAY(i2c, SonicHDSPState, 5),
        VMSTATE_UINT32_ARRAY(core_mmr, SonicHDSPState, 64),
        VMSTATE_UINT32_ARRAY(lmcon, SonicHDSPState, 16),
        VMSTATE_UINT8(glue_reset, SonicHDSPState),
        VMSTATE_UINT8(glue_cmd, SonicHDSPState),
        VMSTATE_UINT8(glue_intreq, SonicHDSPState),
        VMSTATE_UINT8_ARRAY(glue_enable, SonicHDSPState, 2),
        VMSTATE_UINT8(glue_080000, SonicHDSPState),
        VMSTATE_UINT8(glue_038000, SonicHDSPState),
        VMSTATE_UINT8_ARRAY(uart_regs, SonicHDSPState, 16),
        VMSTATE_UINT8_ARRAY(uart_rx, SonicHDSPState, 16),
        VMSTATE_UINT8(uart_rx_n, SonicHDSPState),
        VMSTATE_UINT8_ARRAY(eeprom, SonicHDSPState, 1024),
        VMSTATE_UINT8(i2c_state, SonicHDSPState),
        VMSTATE_UINT16(i2c_ptr, SonicHDSPState),
        VMSTATE_UINT64(config_retries, SonicHDSPState),
        VMSTATE_INT64_ARRAY(mig_timer_age, SonicHDSPState, 2),
        VMSTATE_INT64(mig_deadline_left, SonicHDSPState),
        VMSTATE_INT64(mig_busy_left, SonicHDSPState),
        VMSTATE_BOOL(irq_level, SonicHDSPState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property hdsp_properties[] = {
    DEFINE_PROP_STRING("flash-image", SonicHDSPState, flash_image),
    DEFINE_PROP_BOOL("flash-writable", SonicHDSPState, flash_writable, false),
    DEFINE_PROP_BOOL("flash-persist", SonicHDSPState, flash_persist, false),
    DEFINE_PROP_SIZE("dram-size", SonicHDSPState, dram_size, 64 * MiB),
    DEFINE_PROP_CHR("iop-console", SonicHDSPState, iop_console),
    DEFINE_PROP_UINT32("nv-word", SonicHDSPState, nv_word,
                       SONIC_HDSP_NV_WORD_DEFAULT),
    DEFINE_PROP_UINT32("iop-clock", SonicHDSPState, iop_clock, 66000000),
    DEFINE_PROP_UINT32("dsp-clock", SonicHDSPState, dsp_clock, 100000000),
};

static void hdsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = hdsp_realize;
    k->exit = hdsp_exit;
    k->config_read = hdsp_config_read;
    k->config_write = hdsp_config_write;
    k->vendor_id = 0x8086;
    k->device_id = 0x0960;
    k->class_id = PCI_CLASS_BRIDGE_PCI;
    dc->desc = "Sonic Solutions HDSP audio card (i960 Rx I/O processor)";
    dc->vmsd = &vmstate_sonic_hdsp;
    device_class_set_legacy_reset(dc, hdsp_reset);
    device_class_set_props(dc, hdsp_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo hdsp_info = {
    .name = TYPE_SGI_SONIC_HDSP,
    .parent = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(SonicHDSPState),
    .instance_init = hdsp_instance_init,
    .class_init = hdsp_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void sonic_hdsp_register_types(void)
{
    type_register_static(&atu_info);
    type_register_static(&dsp_info);
    type_register_static(&hdsp_info);
}

type_init(sonic_hdsp_register_types)
