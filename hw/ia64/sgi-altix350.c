/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SGI Altix 350: one or two Madison processors, a SHub 1, and an IO9 base
 * I/O card (PIC, IOC4, ISP12160, optional BCM5701). Transactions the model
 * does not describe are refused and logged instead of answered with a guess.
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "system/blockdev.h"
#include "net/net.h"
#include "net/util.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "chardev/char-fe.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/rtc.h"
#include "hw/ia64/sapic.h"
#include "hw/ia64/sgi-shub.h"
#include "hw/ia64/sgi-shub-uart.h"
#include "system/reset.h"
#include "hw/misc/sgi-l1-sn2.h"
#include "hw/misc/sgi-xio-link.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/misc/sgi-ioc4.h"
#include "hw/scsi/qlogic-isp.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "hw/mips/sgi/sgi-board-mac.h"

#define TYPE_ALTIX350_MACHINE MACHINE_TYPE_NAME("altix350")
OBJECT_DECLARE_SIMPLE_TYPE(Altix350State, ALTIX350_MACHINE)

#define ALTIX350_FLASH_BASE 0xffa00000ULL
#define ALTIX350_FLASH_SIZE (6 * MiB)
/* The node's 16550 is device 4 on the SHub junk bus (offset 0x7fed40000). */
#define ALTIX350_UART_JUNK_BUS_DEVICE 4

#define ALTIX350_TOD_SIZE 0x8000
/* The node's place in the rack: the L1 reports it and SH_SCRATCH2 holds it. */
#define ALTIX350_RACK 1
#define ALTIX350_BAY 1
#define ALTIX350_PARTITION 0

struct Altix350State {
    MachineState parent_obj;
    MemoryRegion hole;
    MemoryRegion flash_reset;
    MemoryRegion ram_cacheable;
    MemoryRegion ram_local;
    MemoryRegion tod;
    uint8_t tod_ram[ALTIX350_TOD_SIZE];
    SGIShubState shub;
    DeviceState *l1;
    DeviceState *uart;
    SGIXioLink pic_link;
    DeviceState *pic;
    /* mac= and emu-id=: the NIC's station address when -nic gives none. */
    char *mac;
    SGIBoardIdentityInputs identity;
};

static const VMStateDescription vmstate_altix350_tod = {
    .name = "altix350/tod",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER(tod_ram, Altix350State),
        VMSTATE_END_OF_LIST()
    }
};

/* Local compatibility space above 4 GiB is a hole: a load completes with 0. */
static MemTxResult altix350_hole_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    static unsigned logged;

    *data = 0;
    /* The first few only: a kernel's table walk can touch it often. */
    if (logged < 32) {
        logged++;
        qemu_log_mask(LOG_UNIMP, "altix350: load of %u bytes at 0x%" HWADDR_PRIx
                      " in the compatibility-space hole reads zero "
                      "(approximation)\n", size, (hwaddr)(1ULL << 32) + addr);
    }
    return MEMTX_OK;
}

static MemTxResult altix350_hole_write(void *opaque, hwaddr addr,
                                       uint64_t data, unsigned size,
                                       MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "altix350: unsupported write at 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 "\n", addr, size, data);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps altix350_hole_ops = {
    .read_with_attrs = altix350_hole_read,
    .write_with_attrs = altix350_hole_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

/*
 * The IOC4's byte bus carries the PROM's time-of-day chip: 32 KiB of RAM with
 * the clock in the top eight bytes, read as host UTC.
 */
static unsigned altix350_tod_bcd(unsigned value)
{
    return ((value / 10) % 10) << 4 | value % 10;
}

static uint64_t altix350_tod_read(void *opaque, hwaddr addr, unsigned size)
{
    Altix350State *s = opaque;
    unsigned reg = addr;
    struct tm tm;

    if (reg < 0x7ff9 || reg == 0x7ffc) {
        return s->tod_ram[reg];
    }
    qemu_get_timedate(&tm, 0);
    switch (reg) {
    case 0x7ff9:
        return altix350_tod_bcd(tm.tm_sec);
    case 0x7ffa:
        return altix350_tod_bcd(tm.tm_min);
    case 0x7ffb:
        return altix350_tod_bcd(tm.tm_hour);
    case 0x7ffd:
        return altix350_tod_bcd(tm.tm_mday);
    case 0x7ffe:
        return altix350_tod_bcd(tm.tm_mon + 1);
    default:
        /* Years since 1970, the PROM's base; tm_year counts from 1900. */
        return altix350_tod_bcd(tm.tm_year - 70);
    }
}

static void altix350_tod_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Altix350State *s = opaque;
    unsigned reg = addr;

    if (reg < 0x7ff9 || reg == 0x7ffc) {
        s->tod_ram[reg] = value;
    } else {
        qemu_log_mask(LOG_UNIMP, "altix350: write to the time-of-day clock "
                      "register 0x%x ignored\n", reg);
    }
}

static const MemoryRegionOps altix350_tod_ops = {
    .read = altix350_tod_read,
    .write = altix350_tod_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/* The riser's PIC and its crossbow: XIO host on the SHub, windows 0 and 15. */
static void altix350_add_pic(Altix350State *s)
{
    SGIShubState *shub = &s->shub;
    SGIPICState *pic;
    PCIDevice *isp, *ioc4, *nic;
    DeviceState *head;
    NICInfo *nd;
    g_autofree char *text = NULL;
    MACAddr address;

    s->pic = qdev_new(TYPE_SGI_PIC);
    pic = SGI_PIC(s->pic);
    object_property_add_child(OBJECT(s), "pic", OBJECT(pic));
    object_unref(OBJECT(pic));
    object_property_set_str(OBJECT(pic), "compatibility-profile",
                            "pic-rev3-shub-compat", &error_abort);
    sgi_xio_link_init(&s->pic_link, OBJECT(s), "pic-link", OBJECT(shub), 0,
                      OBJECT(pic), 0);
    /* Each call exits through error_fatal on failure. */
    sgi_pic_enable_pxbow_experiment(pic, 1ULL << 32, &error_fatal);
    sgi_xio_link_connect(&s->pic_link, &error_fatal);
    sgi_pic_attach_pxbow_packet_endpoint(
        pic, sgi_xio_port_packet_face(OBJECT(shub), 0), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(pic), &error_fatal);
    sgi_xio_link_connect(&s->pic_link, &error_fatal);
    memory_region_add_subregion(sgi_shub_widget_window(shub, 0), 0,
                                sgi_pic_pxbow_widget_zero(pic));
    memory_region_add_subregion(sgi_shub_widget_window(shub, 15), 0,
                                sgi_pic_routed_widget(pic, 15, &error_fatal));
    /* PROM slot 0 (device 1) of head 0: the IOC4 the PROM scans for. */
    ioc4 = pci_new(PCI_DEVFN(1, 0), TYPE_SGI_IOC4);
    qdev_prop_set_bit(DEVICE(ioc4), "attach-compat", true);
    memory_region_init_io(&s->tod, OBJECT(s), &altix350_tod_ops, s,
                          "altix350.tod", ALTIX350_TOD_SIZE);
    vmstate_register_any(NULL, &vmstate_altix350_tod, s);
    object_property_set_link(OBJECT(ioc4), "clock", OBJECT(&s->tod),
                             &error_abort);
    pci_realize_and_unref(ioc4, sgi_pic_pci_bus(pic, 0), &error_fatal);
    /* The ATA channel takes the first two "-drive if=ide" drives. */
    sgi_ioc4_ata_init_drives(ioc4, drive_get(IF_IDE, 0, 0),
                             drive_get(IF_IDE, 0, 1));
    /* The IO9's SCSI adapter, in window 3 (device 3) of head 0. */
    isp = pci_new(PCI_DEVFN(3, 0), TYPE_QLOGIC_ISP12160);
    /* The EFI driver stores bit 3 and requires it to read back. */
    qdev_prop_set_bit(DEVICE(isp), "cfg1-sxp-select", true);
    pci_realize_and_unref(isp, sgi_pic_pci_bus(pic, 0), &error_fatal);
    /* The IO9 NIC, device 4, exists only when a "-nic" names a bcm570x. */
    if (qemu_find_nic_info("bcm570x", false, NULL)) {
        nic = pci_new(PCI_DEVFN(4, 0), "bcm570x");
        qdev_prop_set_string(DEVICE(nic), "model", "bcm5701");
        /* The PIC is a PCI-X bus: the driver wants the PCI-X capability. */
        qdev_prop_set_bit(DEVICE(nic), "pci-x", true);
        /* tg3 learns of link changes from the status block. */
        qdev_prop_set_bit(DEVICE(nic), "link-change-status", true);
        /* Linux's tg3 needs a power-management capability to probe. */
        qdev_prop_set_bit(DEVICE(nic), "pm-cap", true);
        nd = qemu_find_nic_info("bcm570x", true, NULL);
        text = sgi_board_mac(s->mac, &s->identity.id, 0, &error_fatal);
        net_parse_macaddr(address.a, text);
        if (nd && !nd->mac_generated &&
            memcmp(nd->macaddr.a, address.a, sizeof(address.a))) {
            error_setg(&error_fatal, "altix350: -nic mac= conflicts with the "
                       "machine's address %s; set the machine's mac= instead",
                       text);
        }
        qemu_configure_nic_device(DEVICE(nic), true, NULL);
        qdev_prop_set_macaddr(DEVICE(nic), "mac", address.a);
        pci_realize_and_unref(nic, sgi_pic_pci_bus(pic, 0), &error_fatal);
    }
    /*
     * Each device's INTA goes to PIC input device - 1, the convention the
     * Origin 350's IO9 uses. The Altix 350's wiring is unmeasured.
     */
    head = sgi_pic_head_device(pic, 0);
    qdev_connect_gpio_out_named(head, "pci-intx", 1 * PCI_NUM_PINS,
                                qdev_get_gpio_in(head, 0));
    qdev_connect_gpio_out_named(head, "pci-intx", 3 * PCI_NUM_PINS,
                                qdev_get_gpio_in(head, 2));
    qdev_connect_gpio_out_named(head, "pci-intx", 4 * PCI_NUM_PINS,
                                qdev_get_gpio_in(head, 3));
}

static void altix350_init(MachineState *machine)
{
    Altix350State *s = ALTIX350_MACHINE(machine);
    MemoryRegion *memory = get_system_memory();
    g_autofree char *filename = NULL;
    g_autofree char *image = NULL;
    gsize image_len = 0;

    if (machine->ram_size != 2 * GiB) {
        error_setg(&error_fatal,
                   "altix350: the machine requires exactly 2 GiB RAM");
    }
    if (machine->kernel_filename || machine->initrd_filename ||
        (machine->kernel_cmdline && machine->kernel_cmdline[0])) {
        error_setg(&error_fatal,
                   "altix350: direct kernel loading is not supported");
    }
    if (!machine->firmware) {
        error_setg(&error_fatal,
                   "altix350: -bios must name the unmodified SHub 1 PROM");
    }
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!filename) {
        error_setg(&error_fatal, "altix350: cannot find -bios file '%s'",
                   machine->firmware);
    }
    if (get_image_size(filename, NULL) != ALTIX350_FLASH_SIZE) {
        error_setg(&error_fatal,
                   "altix350: -bios requires a 6 MiB SHub 1 PROM image");
    }
    if (!machine->cpu_type) {
        error_setg(&error_fatal,
                   "altix350: an Itanium 2 CPU model is required");
    }

    /* The family only picks the rule's carry check; no serial is modelled. */
    sgi_board_identity_resolve(&s->identity, SGI_BOARD_FAMILY_TEXT, s->mac,
                               &error_fatal);

    memory_region_init_io(&s->hole, OBJECT(machine), &altix350_hole_ops, s,
                          "altix350.compat-hole", 7ULL << 32);
    memory_region_add_subregion_overlap(memory, 1ULL << 32, &s->hole, -1);

    object_initialize_child(OBJECT(machine), "shub", &s->shub, TYPE_SGI_SHUB);
    object_property_set_link(OBJECT(&s->shub), "dma-memory", OBJECT(memory),
                             &error_abort);
    qdev_prop_set_uint64(DEVICE(&s->shub), "scratch2-reset",
                         sgi_l1_sn2_identity_word(ALTIX350_RACK, ALTIX350_BAY,
                                                  ALTIX350_PARTITION));
    sysbus_realize(SYS_BUS_DEVICE(&s->shub), &error_fatal);
    memory_region_add_subregion(memory, SGI_SHUB_LOCAL_MMR_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->shub),
                                                       0));
    /* The device moves this window when the firmware sets its NASID. */
    memory_region_add_subregion(memory, SGI_SHUB_GLOBAL_MMR_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->shub),
                                                       1));

    /* The SN2 L1 answers the PROM's requests; its link is the node's 16550. */
    s->l1 = qdev_new(TYPE_SGI_L1_SN2);
    object_property_add_child(OBJECT(machine), "l1", OBJECT(s->l1));
    /* The PROM's console text leaves through the L1 to the first -serial. */
    if (serial_hd(0)) {
        qdev_prop_set_chr(s->l1, "chardev", serial_hd(0));
    }
    qdev_prop_set_uint32(s->l1, "cpus", machine->smp.cpus);
    qdev_prop_set_uint32(s->l1, "rack", ALTIX350_RACK);
    qdev_prop_set_uint32(s->l1, "bay", ALTIX350_BAY);
    qdev_prop_set_uint32(s->l1, "partition", ALTIX350_PARTITION);
    qdev_realize_and_unref(s->l1, NULL, &error_fatal);
    /* The node's L1 is released with it, after the SHub has been reset. */
    qemu_register_resettable(OBJECT(s->l1));
    s->uart = qdev_new(TYPE_SGI_SHUB_UART);
    object_property_add_child(OBJECT(machine), "uart", OBJECT(s->uart));
    qdev_prop_set_uint32(s->uart, "baudbase", 115200);
    qdev_prop_set_chr(s->uart, "chardev", sgi_l1_sn2_link(SGI_L1_SN2(s->l1)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->uart), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->uart), 0,
                       qdev_get_gpio_in_named(DEVICE(&s->shub), "uart-irq", 0));
    sgi_shub_map_junk_bus(&s->shub, ALTIX350_UART_JUNK_BUS_DEVICE,
                          sysbus_mmio_get_region(SYS_BUS_DEVICE(s->uart), 0),
                          &error_fatal);

    /* SAL_B builds its own early stack table at 0x1000000. */
    memory_region_add_subregion(memory, 0, machine->ram);
    altix350_add_pic(s);
    /* The same node memory in the cacheable address space (AS 3, NASID 0). */
    memory_region_init_alias(&s->ram_cacheable, OBJECT(machine),
                             "altix350.ram.cacheable", machine->ram, 0,
                             machine->ram_size);
    memory_region_add_subregion(memory, SGI_SHUB_CACHEABLE_BASE,
                                &s->ram_cacheable);
    /* AS 0, space id 4: local memory whatever the NASID; SAL's copy uses it. */
    memory_region_init_alias(&s->ram_local, OBJECT(machine),
                             "altix350.ram.local", machine->ram, 0,
                             machine->ram_size);
    memory_region_add_subregion(memory, SGI_SHUB_LOCAL_MEMORY_BASE,
                                &s->ram_local);
    if (!g_file_get_contents(filename, &image, &image_len, NULL)) {
        error_setg(&error_fatal, "altix350: failed to read the PROM image");
    }
    /* One flash: the reset window views its top 6 MiB, not the chip's mode. */
    {
        DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);

        sgi_shub_add_prom_flash(&s->shub,
                                dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
                                image, image_len, &error_fatal);
    }
    memory_region_init_ram_ptr(&s->flash_reset, OBJECT(machine),
                               "altix350.flash.reset", ALTIX350_FLASH_SIZE,
                               memory_region_get_ram_ptr(
                                   sgi_shub_prom_flash_array(&s->shub)) +
                               SGI_SHUB_PROM_FLASH_IMAGE);
    memory_region_set_readonly(&s->flash_reset, true);
    memory_region_add_subregion(memory, ALTIX350_FLASH_BASE, &s->flash_reset);

    /* The CPU's architectural reset path enters the shipped PAL. */
    for (unsigned i = 0; i < machine->smp.cpus; i++) {
        Object *obj = object_new(machine->cpu_type);

        /* The generic PAL_A's DSA key block, read by br.ia's image check. */
        object_property_set_uint(obj, "pal-auth-key-addr", 0xffffa1d0,
                                 &error_fatal);
        /* Id 0x0100 is the second front-side bus: slice 2. */
        object_property_set_uint(obj, "sapic-id", i << 8, &error_fatal);
        qdev_realize_and_unref(DEVICE(obj), NULL, &error_fatal);
    }
}

static const char *const altix350_valid_cpu_types[] = {
    "madison-1600-3m-ia64-cpu",
    NULL
};

static void altix350_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Altix 350";
    mc->init = altix350_init;
    mc->default_cpu_type = "madison-1600-3m-ia64-cpu";
    mc->valid_cpu_types = altix350_valid_cpu_types;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 2;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "altix350.ram";
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
    mc->no_serial = true;
    sgi_board_mac_class_add_property(oc, offsetof(Altix350State, mac),
        "The IO9 NIC's station address when -nic gives none (default derived "
        "from emu-id)");
    sgi_board_identity_class_add_properties(oc,
        offsetof(Altix350State, identity), NULL);
}

static const TypeInfo altix350_type = {
    .name = TYPE_ALTIX350_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Altix350State),
    .class_init = altix350_class_init,
};

static void altix350_register_types(void)
{
    type_register_static(&altix350_type);
}

type_init(altix350_register_types)
