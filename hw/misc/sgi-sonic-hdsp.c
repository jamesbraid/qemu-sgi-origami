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
 * The DSP56301 functions, their peripherals, the converter-box link and the
 * sample clock are in sgi-sonic-hdsp-dsp.c.
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
#include "hw/misc/sgi-sonic-ada8824.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define SONIC_VENDOR_ID     0x1263
#define SONIC_HDSP_ATU_DEV  0x05dc
#define BAR0_SIZE           (128 * MiB)

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
    if (!sonic_hdsp_dsp_realize(h, errp)) {
        sonic_hdsp_iop_unrealize(h);
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
    sonic_hdsp_dsp_unrealize(h);
    sonic_hdsp_iop_unrealize(h);
}

static void hdsp_exit(PCIDevice *d)
{
    pci_bridge_exitfn(d);
    sonic_hdsp_dsp_unrealize(SGI_SONIC_HDSP(d));
    sonic_hdsp_iop_unrealize(SGI_SONIC_HDSP(d));
}

static void hdsp_reset(DeviceState *qdev)
{
    pci_bridge_reset(qdev);
    sonic_hdsp_iop_reset(SGI_SONIC_HDSP(qdev));
    sonic_hdsp_dsp_board_reset(SGI_SONIC_HDSP(qdev));
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
    sonic_hdsp_dsp_pre_save(opaque);
    return 0;
}

static int hdsp_post_load(void *opaque, int version_id)
{
    if (!sonic_hdsp_iop_state_valid(opaque)) {
        return -EINVAL;
    }
    sonic_hdsp_iop_post_load(opaque);
    sonic_hdsp_dsp_post_load(opaque);
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
        VMSTATE_UINT32_ARRAY(sync, SonicHDSPState, SONIC_HDSP_SYNC_WORDS),
        VMSTATE_UINT64(frame_count, SonicHDSPState),
        VMSTATE_INT64(mig_dsp_age, SonicHDSPState),
        VMSTATE_INT64(mig_frame_epoch_age, SonicHDSPState),
        VMSTATE_UINT64(dsp_ns_rem, SonicHDSPState),
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
    DEFINE_PROP_UINT32("sample-rate", SonicHDSPState, sample_rate, 48000),
    DEFINE_PROP_BOOL("x-essi1-chain", SonicHDSPState, essi1_chain, false),
    DEFINE_PROP_LINK("box", SonicHDSPState, box, TYPE_SGI_SONIC_ADA8824,
                     SonicAda8824State *),
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
    type_register_static(&hdsp_info);
}

type_init(sonic_hdsp_register_types)
