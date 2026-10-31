/*
 * TI TSB12LV22 revision-01 standalone register model (SLLS290, July 1998).
 * PHY, ROM, bus, cycle scheduling, power transitions and DMA are unsupported.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_TSB12LV22 "tsb12lv22"
OBJECT_DECLARE_SIMPLE_TYPE(TSB12LV22State, TSB12LV22)

#define APERTURE 0x800
#define REG(s, off) ((s)->reg[(off) / 4])
#define HC_CONTROL 0x50
#define INT_EVENT 0x80
#define INT_MASK 0x88
#define IT_EVENT 0x90
#define IT_MASK 0x98
#define IR_EVENT 0xa0
#define IR_MASK 0xa8
#define LINK_CONTROL 0xe0
#define SOFT_RESET (1u << 16)
#define MASTER_ENABLE (1u << 31)
#define EVENT_WRITABLE 0x07fb033fu
#define EVENT_MASK 0x87fb03ffu

struct TSB12LV22State {
    PCIDevice parent_obj;
    MemoryRegion ohci;
    MemoryRegion extension;
    uint32_t reg[APERTURE / 4];
    bool guid_locked[2];
};

typedef struct Register {
    unsigned offset;
    unsigned clear;
    uint32_t writable;
} Register;

/* TI tables 20–46. Aliases share storage at the set address. */
static const Register registers[] = {
    { 0x000, 0, 0 }, { 0x004, 0, 0 }, { 0x008, 0, 0x00000fff },
    { 0x00c, 0, UINT32_MAX }, { 0x010, 0, UINT32_MAX },
    { 0x014, 0, 0 }, { 0x018, 0, UINT32_MAX }, { 0x01c, 0, 0 },
    { 0x020, 0, 0xf8fff0c0 }, { 0x024, 0, UINT32_MAX },
    { 0x028, 0, UINT32_MAX }, { 0x034, 0, 0xfffffc00 },
    { 0x038, 0, 0 }, { 0x03c, 0, 0 }, { 0x040, 0, 0 },
    { HC_CONTROL, 0x054, 0x40ce0000 },
    { 0x064, 0, 0xfffff800 }, { 0x068, 0, 0 },
    { 0x070, 0x074, UINT32_MAX }, { 0x078, 0x07c, UINT32_MAX },
    { INT_EVENT, 0x084, EVENT_WRITABLE },
    { INT_MASK, 0x08c, EVENT_MASK },
    { IT_EVENT, 0x094, 0xff }, { IT_MASK, 0x09c, 0xff },
    { IR_EVENT, 0x0a4, 0x0f }, { IR_MASK, 0x0ac, 0x0f },
    { 0x0dc, 0, 0 }, { LINK_CONTROL, 0x0e4, 0x00400600 },
    { 0x0e8, 0, 0x0000ffc0 }, { 0x0ec, 0, 0x00000fff },
    { 0x0f0, 0, UINT32_MAX },
    { 0x100, 0x104, UINT32_MAX }, { 0x108, 0x10c, UINT32_MAX },
    { 0x110, 0x114, UINT32_MAX }, { 0x118, 0x11c, UINT32_MAX },
    { 0x120, 0, 0 },
};

static bool lookup(unsigned offset, Register *r)
{
    unsigned i, base, stride;

    for (i = 0; i < ARRAY_SIZE(registers); i++) {
        *r = registers[i];
        if (offset == r->offset || (r->clear && offset == r->clear)) {
            return true;
        }
    }
    if (offset >= 0x180 && offset < 0x200) {
        stride = 0x20;
        base = offset & ~0x1f;
        r->writable = 0;
    } else if (offset >= 0x200 && offset < 0x280) {
        stride = 0x10;
        base = offset & ~0xf;
        r->writable = 0xffff0000;
    } else if (offset >= 0x400 && offset < 0x480) {
        stride = 0x20;
        base = offset & ~0x1f;
        r->writable = 0xf0000000;
    } else {
        return false;
    }
    r->offset = base;
    r->clear = base + 4;
    switch (offset % stride) {
    case 0:
    case 4:
        return true;
    case 0xc:
        r->offset = offset;
        r->clear = 0;
        r->writable = UINT32_MAX;
        return true;
    case 0x10:
        if (base >= 0x400) {
            r->offset = offset;
            r->clear = 0;
            r->writable = 0xf1ffff7f;
            return true;
        }
        break;
    }
    return false;
}

static uint32_t interrupt_events(TSB12LV22State *s)
{
    uint32_t events = REG(s, INT_EVENT);

    if (REG(s, IT_EVENT) & REG(s, IT_MASK)) {
        events |= 1 << 6;
    }
    if (REG(s, IR_EVENT) & REG(s, IR_MASK)) {
        events |= 1 << 7;
    }
    return events;
}

static void update_irq(TSB12LV22State *s)
{
    pci_set_irq(PCI_DEVICE(s),
                (REG(s, INT_MASK) & MASTER_ENABLE) &&
                (interrupt_events(s) & REG(s, INT_MASK)));
}

static void reset_registers(TSB12LV22State *s, bool soft)
{
    uint32_t hi = REG(s, 0x24), lo = REG(s, 0x28);
    uint32_t max_rec = soft ? REG(s, 0x20) & 0xf000 : 0xa000;

    /* TI's X reset positions deliberately start at zero, not silicon facts. */
    memset(s->reg, 0, sizeof(s->reg));
    REG(s, 0x00) = 0x00010000;
    REG(s, 0x14) = 0x80000000;
    REG(s, 0x1c) = 0x31333934;
    REG(s, 0x20) = max_rec | 2;
    REG(s, 0xe8) = 0xffc0;
    if (soft) {
        REG(s, 0x24) = hi;
        REG(s, 0x28) = lo;
    } else {
        memset(s->guid_locked, 0, sizeof(s->guid_locked));
    }
    update_irq(s);
}

static void unsupported(unsigned offset, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP, "tsb12lv22: unsupported operation at 0x%x"
                  " value 0x%" PRIx64 "\n", offset, value);
}

static bool access_valid(void *opaque, hwaddr addr, unsigned size,
                         bool is_write, MemTxAttrs attrs)
{
    if (size != 4 || (addr & 3)) {
        qemu_log_mask(LOG_UNIMP, "tsb12lv22: unsupported %s size %u"
                      " at 0x%" HWADDR_PRIx "\n",
                      is_write ? "write" : "read", size, addr);
        return false;
    }
    return true;
}

static uint64_t read_register(void *opaque, hwaddr addr, unsigned size)
{
    TSB12LV22State *s = opaque;
    Register r;
    uint32_t value;

    if (!lookup(addr, &r)) {
        unsupported(addr, 0);
        return 0;
    }
    value = r.offset == INT_EVENT ? interrupt_events(s) : REG(s, r.offset);
    if (addr == 0x84 || addr == 0x94 || addr == 0xa4) {
        value &= REG(s, r.offset + 8);
    }
    return value;
}

static void write_register(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    TSB12LV22State *s = opaque;
    Register r;
    uint32_t mask;

    if (!lookup(addr, &r)) {
        unsupported(addr, value);
        return;
    }
    if ((r.offset == HC_CONTROL) && addr == HC_CONTROL &&
        (value & SOFT_RESET)) {
        /* Completion is synchronous; PCI and GUID locks are preserved. */
        reset_registers(s, true);
        return;
    }
    if (addr == 0x24 || addr == 0x28) {
        unsigned index = (addr - 0x24) / 4;

        if (!s->guid_locked[index]) {
            REG(s, addr) = value;
            s->guid_locked[index] = true;
        }
        return;
    }
    if (r.offset == 0xec && (value & 0xc000)) {
        unsupported(addr, value);
        return;
    }
    if (r.offset == 0x04 || r.offset == 0x14 ||
        (r.offset >= 0x180 && r.clear && (value & 0x9000)) ||
        (r.offset == LINK_CONTROL && (value & 0x300000))) {
        unsupported(addr, value);
    }
    mask = r.writable;
    if (addr == HC_CONTROL) {
        /* programPhyEnable has clear access only (TI table 30). */
        mask &= ~(1 << 23);
    }
    if (r.clear) {
        if (addr == r.clear) {
            REG(s, r.offset) &= ~(value & mask);
        } else {
            REG(s, r.offset) |= value & mask;
        }
    } else {
        REG(s, r.offset) = (REG(s, r.offset) & ~mask) | (value & mask);
    }
    update_irq(s);
}

static uint64_t extension_read(void *opaque, hwaddr addr, unsigned size)
{
    unsupported(addr, 0);
    return 0;
}

static void extension_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    unsupported(addr, value);
}

static const MemoryRegionOps ohci_ops = {
    .read = read_register,
    .write = write_register,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true, .accepts = access_valid },
    /* Preserve the original width/alignment until access_valid rejects it. */
    .impl = { .min_access_size = 4, .max_access_size = 8,
              .unaligned = true },
};

static const MemoryRegionOps extension_ops = {
    .read = extension_read,
    .write = extension_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true, .accepts = access_valid },
    /* Preserve the original width/alignment until access_valid rejects it. */
    .impl = { .min_access_size = 4, .max_access_size = 8,
              .unaligned = true },
};

static void config_write(PCIDevice *dev, uint32_t addr, uint32_t value, int len)
{
    unsigned i;

    if ((addr <= 0x48 && addr + len > 0x48) &&
        ((value >> (8 * (0x48 - addr))) & 3)) {
        unsupported(0x48, value);
    }
    pci_default_write_config(dev, addr, value, len);
    for (i = 0; i < 4; i++) {
        dev->config[0x2c + i] = dev->config[0xf8 + i];
    }
}

static void reset_hold(Object *obj, ResetType type)
{
    TSB12LV22State *s = TSB12LV22(obj);
    PCIDevice *dev = PCI_DEVICE(obj);

    dev->config[PCI_LATENCY_TIMER] = 0;
    pci_set_long(dev->config + 0xf4, 0);
    pci_set_long(dev->config + 0xf8, 0);
    pci_set_long(dev->config + 0x2c, 0);
    pci_set_long(dev->config + 0xfc, 0x1010);
    pci_set_word(dev->config + 0x48, 0);
    reset_registers(s, false);
}

static int post_load(void *opaque, int version_id)
{
    update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_tsb12lv22 = {
    .name = TYPE_TSB12LV22,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, TSB12LV22State),
        VMSTATE_UINT32_ARRAY(reg, TSB12LV22State, APERTURE / 4),
        VMSTATE_BOOL_ARRAY(guid_locked, TSB12LV22State, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void realize(PCIDevice *dev, Error **errp)
{
    TSB12LV22State *s = TSB12LV22(dev);

    memset(dev->wmask, 0, PCI_CONFIG_SPACE_SIZE);
    memset(dev->w1cmask, 0, PCI_CONFIG_SPACE_SIZE);
    pci_set_word(dev->wmask + PCI_COMMAND, 0x0156);
    pci_set_word(dev->w1cmask + PCI_STATUS, 0xf900);
    dev->wmask[PCI_CACHE_LINE_SIZE] = 0xff;
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;
    dev->wmask[PCI_INTERRUPT_LINE] = 0xff;
    pci_set_word(dev->config + PCI_STATUS, 0x0210);
    dev->config[PCI_CLASS_PROG] = 0x10;
    dev->config[PCI_INTERRUPT_PIN] = 1;
    /* Explicit no-ROM approximation, TI p.12 prose rather than its diagram. */
    pci_set_word(dev->config + PCI_MIN_GNT, 0x0402);
    if (pci_add_capability(dev, PCI_CAP_ID_PM, 0x44, 8, errp) < 0) {
        return;
    }
    pci_set_word(dev->config + 0x46, 0x4001);
    pci_set_word(dev->wmask + 0x48, 0x0100);
    pci_set_word(dev->w1cmask + 0x48, 0x8000);
    pci_set_long(dev->wmask + 0xf4, 0x86);
    pci_set_long(dev->wmask + 0xf8, UINT32_MAX);
    pci_set_long(dev->wmask + 0xfc, 0x3131b1b1);
    memory_region_init_io(&s->ohci, OBJECT(s), &ohci_ops, s,
                          "tsb12lv22-ohci", APERTURE);
    memory_region_init_io(&s->extension, OBJECT(s), &extension_ops, s,
                          "tsb12lv22-extension", APERTURE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ohci);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->extension);
}

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    pc->realize = realize;
    pc->config_write = config_write;
    pc->vendor_id = 0x104c;
    pc->device_id = 0x8009;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_SERIAL_FIREWIRE;
    dc->desc = "TI TSB12LV22 standalone registers (no PHY or DMA)";
    dc->vmsd = &vmstate_tsb12lv22;
    rc->phases.hold = reset_hold;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo type_info = {
    .name = TYPE_TSB12LV22,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(TSB12LV22State),
    .class_init = class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { }
    },
};

static void register_types(void)
{
    type_register_static(&type_info);
}
type_init(register_types)
