/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Broadcom NetXtreme BCM570x ("Tigon3") Gigabit Ethernet controller.
 *
 * A generic PCI network controller that OpenBSD's bge, Linux's tg3 and IRIX's
 * tg all drive as a conventional descriptor-ring Gigabit MAC with an integrated
 * copper PHY. The SGI IRIS Release 2 Gigabit board is one instantiation of this
 * chip, distinguished only by its PCI identity (a Broadcom primary vendor/device
 * plus an SGI subsystem identity), port count and media -- all QOM properties,
 * not bespoke code. The single-port BCM5701 and the dual-port dual-MAC
 * BCM5704/5704S are `model` property variants of this one type.
 *
 * Modelled so far: the PCI device and its configurable four-tuple; the 64 KiB
 * BAR0 (a register file at 0x0000-0x7FFF and a 32 KiB on-chip SRAM window at
 * 0x8000-0xFFFF); the three ways the driver reaches chip state -- the BAR
 * directly, an indirect register window through PCI config 0x78/0x80, and an
 * indirect memory window through config 0x7C/0x84; the chip id in MISC_CTL; the
 * global reset with its on-chip firmware-ready handshake at SRAM 0x0B50; and the
 * self-clearing *_MODE reset bits. The MII/PHY path, the ring engines,
 * interrupts and the dual-port device arrive in later commits; a write to a
 * register with no modelled behaviour logs LOG_UNIMP and is stored so a
 * read-back still round-trips.
 *
 * Written from docs/datasheets/bcm570x.md (the reviewed register/DMA/interrupt
 * contract) and docs/library/nic-on-bridge-conventions.md (the shared byte-swap
 * and architecture convention), plus the public OpenBSD bge and in-tree QEMU
 * device sources; no IRIX vendor source was read.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_BCM570X "bcm570x"
typedef struct Bcm570xState Bcm570xState;
DECLARE_INSTANCE_CHECKER(Bcm570xState, BCM570X, TYPE_BCM570X)

/* Default four-tuple: the single-port SGI IRIS Release 2 board (bcm5701). */
#define BCM570X_VENDOR_ID           0x14e4  /* Broadcom */
#define BCM570X_DEVICE_ID           0x1645  /* BCM5701 */
#define BCM570X_SUBSYS_VENDOR_ID    0x10a9  /* SGI */
#define BCM570X_SUBSYS_ID           0x8010

/*
 * BAR0 is 64 KiB: a register file at 0x0000-0x7FFF and a 32 KiB window into
 * on-chip SRAM at 0x8000-0xFFFF whose base within SRAM is set through config
 * 0x7C. Both are 32-bit-accessed. The register file and the SRAM are each
 * 32 KiB.
 */
#define BCM570X_REG_FILE_SIZE       0x8000u
#define BCM570X_SRAM_SIZE           0x8000u
#define BCM570X_BAR0_SIZE           0x10000u
#define BCM570X_REG_FILE_WORDS      (BCM570X_REG_FILE_SIZE / 4)
#define BCM570X_SRAM_WORDS          (BCM570X_SRAM_SIZE / 4)

/*
 * The indirect-access config registers (docs/datasheets/bcm570x.md, "three
 * access paths"). The base register selects an offset; a read or write of the
 * data register then reaches that offset in the register file (0x78/0x80) or in
 * on-chip SRAM (0x7C/0x84). The memory-window base, masked to a 32 KiB
 * boundary, also positions the BAR0 window at 0x8000.
 */
#define BCM570X_PCI_REG_BASEADDR    0x78
#define BCM570X_PCI_MEMWIN_BASEADDR 0x7c
#define BCM570X_PCI_REG_DATA        0x80
#define BCM570X_PCI_MEMWIN_DATA     0x84

/*
 * Config 0x68 (misc host control). Bits 31:16 read the ASIC revision -- the
 * driver takes its whole chip id from this field (bge_chipid = MISC_CTL >> 16,
 * ASIC revision = chipid >> 12), so it must decode to the presented part. The
 * low 16 control bits are writable and behaviourless for now.
 */
#define BCM570X_PCI_MISC_CTL        0x68
#define BCM570X_CHIPID_BCM5701      0x0105  /* B5 stepping; ASIC revision 0x0 */
#define BCM570X_CHIPID_BCM5704      0x2003  /* A3 stepping; ASIC revision 0x2 */

/* Register-file offsets with modelled behaviour. */
#define BCM570X_MAC_MODE            0x0400
#define BCM570X_HCC_MODE            0x3c00
#define BCM570X_MISC_CFG            0x6804

/*
 * Bit 0 of a *_MODE register is a self-clearing reset the driver sets and then
 * polls for clear. Bit 0 of MISC_CFG (RESET_CORE_CLOCKS) triggers the chip's
 * global reset instead.
 */
#define BCM570X_MODE_RESET          0x00000001u
#define BCM570X_MISCCFG_RESET_CORE_CLOCKS 0x00000001u

/*
 * The firmware-ready handshake. The driver writes the magic to on-chip SRAM
 * 0x0B50 before a global reset and polls there afterwards for its complement,
 * which is how the on-chip bootcode signals initialisation complete.
 */
#define BCM570X_SRAM_FW_MBOX        0x0b50
#define BCM570X_FW_MAGIC            0x4b657654u
#define BCM570X_FW_HANDSHAKE_DONE   (~BCM570X_FW_MAGIC)     /* 0xB49A89AB */

struct Bcm570xState {
    PCIDevice parent_obj;

    MemoryRegion bar0;

    NICState *nic;
    NICConf conf;

    /* PCI identity, defaulting to the SGI board; overridable per instance. */
    char *model;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;

    /* The two indirect-window base registers (config 0x78 / 0x7C). */
    uint32_t reg_win_base;
    uint32_t mem_win_base;

    /* Backing storage for the register file and the on-chip SRAM. */
    uint32_t regfile[BCM570X_REG_FILE_WORDS];
    uint32_t sram[BCM570X_SRAM_WORDS];
};

static uint32_t bcm570x_regfile_read(Bcm570xState *s, uint32_t off)
{
    return s->regfile[(off & (BCM570X_REG_FILE_SIZE - 1)) >> 2];
}

static void bcm570x_regfile_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    s->regfile[(off & (BCM570X_REG_FILE_SIZE - 1)) >> 2] = val;
}

static uint32_t bcm570x_sram_read(Bcm570xState *s, uint32_t off)
{
    return s->sram[(off & (BCM570X_SRAM_SIZE - 1)) >> 2];
}

static void bcm570x_sram_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    s->sram[(off & (BCM570X_SRAM_SIZE - 1)) >> 2] = val;
}

/* The chip id MISC_CTL[31:16] presents, from which the driver reads the part. */
static uint16_t bcm570x_chip_id(Bcm570xState *s)
{
    switch (s->device_id) {
    case 0x1648:    /* BCM5704 copper */
    case 0x16a8:    /* BCM5704S SerDes */
        return BCM570X_CHIPID_BCM5704;
    default:        /* BCM5701 single-port, the SGI board default */
        return BCM570X_CHIPID_BCM5701;
    }
}

/*
 * A global reset (the driver's write of MISC_CFG.RESET_CORE_CLOCKS, by either
 * access path) returns the register file and the on-chip SRAM to their reset
 * values and leaves the firmware-ready complement in the SRAM handshake
 * mailbox, which is how the on-chip bootcode signals the driver it is up. The
 * PCI config the driver saves and restores by hand -- the chip id in MISC_CTL
 * among it -- is untouched.
 */
static void bcm570x_global_reset(Bcm570xState *s)
{
    memset(s->regfile, 0, sizeof(s->regfile));
    memset(s->sram, 0, sizeof(s->sram));
    bcm570x_sram_write(s, BCM570X_SRAM_FW_MBOX, BCM570X_FW_HANDSHAKE_DONE);
}

/*
 * Register-file writes, reached identically from the BAR and from the indirect
 * register window, so a side effect fires whichever path the driver uses -- the
 * 5700/5701 issue the global reset through the indirect window, not the BAR.
 */
static void bcm570x_reg_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    off &= (BCM570X_REG_FILE_SIZE - 1);

    switch (off) {
    case BCM570X_MISC_CFG:
        if (val & BCM570X_MISCCFG_RESET_CORE_CLOCKS) {
            bcm570x_global_reset(s);
            return;
        }
        break;
    case BCM570X_MAC_MODE:
    case BCM570X_HCC_MODE:
        /* The reset bit self-clears; the rest of the write persists. */
        val &= ~BCM570X_MODE_RESET;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm570x: write 0x%08x to register 0x%04x has no "
                      "modelled effect yet\n", val, off);
        break;
    }
    bcm570x_regfile_write(s, off, val);
}

/* SRAM offset the BAR0 window at 0x8000 exposes for a given BAR address. */
static uint32_t bcm570x_window_sram_off(Bcm570xState *s, hwaddr addr)
{
    uint32_t base = s->mem_win_base & ~(BCM570X_SRAM_SIZE - 1);

    return base + (uint32_t)(addr - BCM570X_REG_FILE_SIZE);
}

static uint64_t bcm570x_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm570xState *s = opaque;

    if (addr < BCM570X_REG_FILE_SIZE) {
        return bcm570x_regfile_read(s, addr);
    }
    return bcm570x_sram_read(s, bcm570x_window_sram_off(s, addr));
}

static void bcm570x_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Bcm570xState *s = opaque;

    if (addr < BCM570X_REG_FILE_SIZE) {
        bcm570x_reg_write(s, addr, val);
        return;
    }
    bcm570x_sram_write(s, bcm570x_window_sram_off(s, addr), val);
}

/*
 * The register file and the SRAM are 32-bit-accessed. impl.min/max pin the
 * width so the memory core never chops a wide access into repeated calls
 * (which would run a register's side effects twice) nor widens a narrow one.
 * The declared endianness is the working default (DEVICE_LITTLE_ENDIAN, the
 * ISP1040 precedent for these little-endian-native controllers behind the
 * Bridge); it is pinned against the byte-order round-trip qtest in a later
 * commit, not settled here.
 */
static const MemoryRegionOps bcm570x_bar0_ops = {
    .read = bcm570x_bar0_read,
    .write = bcm570x_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * The two indirect config-space windows. A 32-bit write to the base register
 * (0x78 / 0x7C) records the offset; a 32-bit access to the data register
 * (0x80 / 0x84) then reaches that offset in the register file or in SRAM. Every
 * other config access takes the default path.
 */
static void bcm570x_config_write(PCIDevice *dev, uint32_t addr, uint32_t val,
                                 int len)
{
    Bcm570xState *s = BCM570X(dev);

    if (len == 4) {
        switch (addr) {
        case BCM570X_PCI_REG_BASEADDR:
            s->reg_win_base = val;
            return;
        case BCM570X_PCI_MEMWIN_BASEADDR:
            s->mem_win_base = val;
            return;
        case BCM570X_PCI_REG_DATA:
            bcm570x_reg_write(s, s->reg_win_base, val);
            return;
        case BCM570X_PCI_MEMWIN_DATA:
            bcm570x_sram_write(s, s->mem_win_base, val);
            return;
        }
    }
    pci_default_write_config(dev, addr, val, len);
}

static uint32_t bcm570x_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    Bcm570xState *s = BCM570X(dev);

    if (len == 4) {
        switch (addr) {
        case BCM570X_PCI_REG_BASEADDR:
            return s->reg_win_base;
        case BCM570X_PCI_MEMWIN_BASEADDR:
            return s->mem_win_base;
        case BCM570X_PCI_REG_DATA:
            return bcm570x_regfile_read(s, s->reg_win_base);
        case BCM570X_PCI_MEMWIN_DATA:
            return bcm570x_sram_read(s, s->mem_win_base);
        }
    }
    return pci_default_read_config(dev, addr, len);
}

/*
 * The receive path is not modelled yet, so the frontend accepts no frames and
 * discards anything the backend hands it. Both arrive in the receive-ring
 * commit.
 */
static bool bcm570x_can_receive(NetClientState *nc)
{
    return false;
}

static ssize_t bcm570x_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    return size;
}

static NetClientInfo net_bcm570x_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bcm570x_can_receive,
    .receive = bcm570x_receive,
};

static void bcm570x_realize(PCIDevice *dev, Error **errp)
{
    Bcm570xState *s = BCM570X(dev);
    DeviceState *ds = DEVICE(dev);
    uint8_t *pci_conf = dev->config;

    if (s->model && strcmp(s->model, "bcm5701") != 0 &&
        strcmp(s->model, "bcm5704") != 0) {
        error_setg(errp, "bcm570x: unknown model '%s' "
                   "(expected bcm5701 or bcm5704)", s->model);
        return;
    }

    /*
     * The PCI four-tuple IRIX tg requires. The Broadcom primary vendor/device
     * sit at 0x00/0x02 and the SGI subsystem identity in the standard PCI
     * subsystem fields: subsystem vendor at 0x2C, subsystem id at 0x2E. (bge's
     * BGE_PCI_SUBSYS/BGE_PCI_SUBVID header macro names label these offsets
     * backwards, but if_bge.c reads them the standard way -- the low half of
     * the 0x2C dword is the subsystem vendor -- so standard placement is what
     * the silicon and IRIX both use.)
     */
    pci_config_set_vendor_id(pci_conf, s->vendor_id);
    pci_config_set_device_id(pci_conf, s->device_id);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, s->subsystem_vendor_id);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, s->subsystem_id);
    pci_conf[PCI_INTERRUPT_PIN] = 0x01;     /* INTA */

    /*
     * MISC_CTL[31:16] is the chip id, which the driver reads its whole identity
     * from. Keep it read-only across reset; leave the low 16 control bits
     * writable so the driver's byte-order and interrupt-mask writes stick.
     */
    pci_set_long(pci_conf + BCM570X_PCI_MISC_CTL,
                 (uint32_t)bcm570x_chip_id(s) << 16);
    pci_set_word(dev->wmask + BCM570X_PCI_MISC_CTL, 0xffff);

    memory_region_init_io(&s->bar0, OBJECT(s), &bcm570x_bar0_ops, s,
                          "bcm570x-bar0", BCM570X_BAR0_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_bcm570x_info, &s->conf,
                          object_get_typename(OBJECT(dev)), ds->id,
                          &ds->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void bcm570x_reset_hold(Object *obj, ResetType type)
{
    Bcm570xState *s = BCM570X(obj);

    /*
     * A cold reset zeroes the register file, the on-chip SRAM and the
     * indirect-window bases. The firmware-ready complement is arranged only by
     * the driver-triggered global reset (bcm570x_global_reset), which is the
     * one the driver writes the magic for and then polls; the chip id in
     * MISC_CTL lives in PCI config space and survives either.
     */
    memset(s->regfile, 0, sizeof(s->regfile));
    memset(s->sram, 0, sizeof(s->sram));
    s->reg_win_base = 0;
    s->mem_win_base = 0;
}

static const VMStateDescription vmstate_bcm570x = {
    .name = "bcm570x",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Bcm570xState),
        VMSTATE_UINT32_ARRAY(regfile, Bcm570xState, BCM570X_REG_FILE_WORDS),
        VMSTATE_UINT32_ARRAY(sram, Bcm570xState, BCM570X_SRAM_WORDS),
        VMSTATE_UINT32(reg_win_base, Bcm570xState),
        VMSTATE_UINT32(mem_win_base, Bcm570xState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm570x_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", Bcm570xState, vendor_id,
                       BCM570X_VENDOR_ID),
    DEFINE_PROP_UINT16("device-id", Bcm570xState, device_id,
                       BCM570X_DEVICE_ID),
    DEFINE_PROP_UINT16("subsystem-vendor-id", Bcm570xState,
                       subsystem_vendor_id, BCM570X_SUBSYS_VENDOR_ID),
    DEFINE_PROP_UINT16("subsystem-id", Bcm570xState, subsystem_id,
                       BCM570X_SUBSYS_ID),
    DEFINE_PROP_STRING("model", Bcm570xState, model),
    DEFINE_NIC_PROPERTIES(Bcm570xState, conf),
};

static void bcm570x_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = bcm570x_realize;
    pc->config_read = bcm570x_config_read;
    pc->config_write = bcm570x_config_write;
    pc->vendor_id = BCM570X_VENDOR_ID;
    pc->device_id = BCM570X_DEVICE_ID;
    pc->revision = 0x00;
    pc->class_id = PCI_CLASS_NETWORK_ETHERNET;

    rc->phases.hold = bcm570x_reset_hold;
    dc->vmsd = &vmstate_bcm570x;
    dc->desc = "Broadcom NetXtreme BCM570x Gigabit Ethernet";
    device_class_set_props(dc, bcm570x_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bcm570x_types[] = {
    {
        .name = TYPE_BCM570X,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(Bcm570xState),
        .class_init = bcm570x_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(bcm570x_types)
