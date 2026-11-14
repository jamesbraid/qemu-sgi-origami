/*
 * The ColdFire controller board of an Origin 3000 C-, I- or R-brick L1.
 *
 * A stand-alone board: an MCF5206e, a 1 MiB boot flash, a 2 KiB battery
 * backed clock RAM, external static RAM and a four-channel UART, with the
 * management USB controller, the two-wire bus and the other peripherals
 * of the board left as logged, unimplemented windows. It takes the boot
 * flash contents as -bios and has no node attached.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/datadir.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/m68k/mcf5206e.h"
#include "hw/misc/sgi-l1-identity.h"
#include "hw/nvram/eeprom_at24c.h"
#include "net/util.h"
#include "hw/core/loader.h"
#include "migration/blocker.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"

#define FLASH_SIZE          (1 * MiB)
#define CS_NVRAM            1
#define CS_RAM              2
#define CS_USB0             3
#define CS_QUART            4
#define CS_USB1             5

#define QUART_CLOCK_HZ      12000000
#define QUART_CHANNELS      4
#define QUART_EXT_IRQ       4

#define RAM_SIZE_FULL       (512 * KiB)
#define RAM_SIZE_ALIASED    (128 * KiB)

/*
 * The firmware calls each identity part with one address byte, so the model
 * answers a 256-byte window at each address the firmware's table names. The
 * table's configured length is 512; the part, and what lies above 256, is
 * unknown, and a call to the second page's address is not acknowledged.
 */
#define IDENTITY_EEPROM_SIZE    256
#define IDENTITY_NODE_ADDR7     0x52    /* wire address 0xa4 */
#define IDENTITY_LOGIC_ADDR7    0x54    /* wire address 0xa8 */

#define TYPE_L1_BOARD_MACHINE   MACHINE_TYPE_NAME("sgi-l1-board")
OBJECT_DECLARE_SIMPLE_TYPE(L1BoardMachine, L1_BOARD_MACHINE)

struct L1BoardMachine {
    MachineState parent_obj;

    /* Which brick's identity parts the two-wire bus carries; none if unset. */
    char *brick_type;
    char *node_board_name;
    char *node_board_part;
    char *node_board_revision;
    char *mac;
};

typedef struct L1Board {
    M68kCPU *cpu;
    MCF5206ESIMState *sim;
    MemoryRegion flash;
    Error *migration_blocker;
} L1Board;

static uint64_t flash_read(void *opaque, hwaddr addr, unsigned size)
{
    L1Board *b = opaque;
    uint8_t *p = memory_region_get_ram_ptr(&b->flash);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | p[addr + i];
    }
    return val;
}

static void flash_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "sgi-l1-board flash: write%u at 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 ": the flash command set is not "
                  "modelled, ignored\n", size * 8, addr, val);
}

static const MemoryRegionOps flash_ops = {
    .read = flash_read,
    .write = flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void l1_board_load_flash(L1Board *b, const char *firmware,
                                Error **errp)
{
    g_autofree char *path = NULL;
    uint8_t *p;
    int size;

    memory_region_init_rom_device(&b->flash, NULL, &flash_ops, b,
                                  "sgi-l1-board.flash", FLASH_SIZE, errp);
    if (*errp) {
        return;
    }
    p = memory_region_get_ram_ptr(&b->flash);
    /* Without an image the flash is erased and the core has no vectors. */
    memset(p, 0xff, FLASH_SIZE);
    if (!firmware) {
        return;
    }
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);
    if (!path) {
        error_setg(errp, "could not find the boot flash image '%s'",
                   firmware);
        return;
    }
    size = load_image_size(path, p, FLASH_SIZE);
    if (size < 0) {
        error_setg(errp, "could not load '%s' (at most %d bytes)", path,
                   (int)FLASH_SIZE);
    }
}

static void l1_board_unimp_window(MemoryRegion *window, const char *name,
                                  uint64_t size)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(window, 0,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
}

static void l1_board_reset(void *opaque)
{
    L1Board *b = opaque;
    CPUState *cs = CPU(b->cpu);

    /* The SIM has been reset: CS0 is global, so the vectors come from flash. */
    cpu_reset(cs);
    b->cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    b->cpu->env.pc = ldl_phys(cs->as, 4);
}

#define L1_BOARD_STRING_PROPERTY(field)                                     \
    static char *l1_board_get_##field(Object *obj, Error **errp)            \
    {                                                                       \
        return g_strdup(L1_BOARD_MACHINE(obj)->field);                      \
    }                                                                       \
    static void l1_board_set_##field(Object *obj, const char *value,        \
                                     Error **errp)                          \
    {                                                                       \
        L1BoardMachine *m = L1_BOARD_MACHINE(obj);                          \
                                                                            \
        g_free(m->field);                                                   \
        m->field = g_strdup(value);                                         \
    }

L1_BOARD_STRING_PROPERTY(brick_type)
L1_BOARD_STRING_PROPERTY(node_board_name)
L1_BOARD_STRING_PROPERTY(node_board_part)
L1_BOARD_STRING_PROPERTY(node_board_revision)
L1_BOARD_STRING_PROPERTY(mac)

static void l1_board_add_identity_eeprom(I2CBus *bus, uint8_t addr7,
                                         GByteArray *chassis,
                                         GByteArray *board)
{
    uint8_t *image = sgi_l1_identity_board_image(chassis, board,
                                                 IDENTITY_EEPROM_SIZE,
                                                 &error_fatal);

    at24c_eeprom_init_rom(bus, addr7, IDENTITY_EEPROM_SIZE, image,
                          IDENTITY_EEPROM_SIZE);
    g_free(image);
    g_byte_array_free(chassis, true);
    g_byte_array_free(board, true);
}

/*
 * The identity parts on bus segment 0, built by the behavioural L1's own
 * builder from the same inputs, so that both models present one identity:
 * a C-brick's node board, and an I-brick's power and logic boards. The
 * processor modules' and memory modules' records sit behind a bus segment
 * select that is not modelled.
 */
static void l1_board_identity(L1BoardMachine *m, I2CBus *bus, Error **errp)
{
    uint8_t mac[6];

    if (!m->brick_type) {
        if (m->node_board_name || m->node_board_part ||
            m->node_board_revision || m->mac) {
            error_setg(errp, "identity properties need a brick-type");
        }
        return;
    }
    if (!strcmp(m->brick_type, "cbrick")) {
        if (m->mac) {
            error_setg(errp, "mac is for the I-brick only");
            return;
        }
        if ((m->node_board_name || m->node_board_part ||
             m->node_board_revision) &&
            (!m->node_board_name || !m->node_board_part ||
             !m->node_board_revision)) {
            error_setg(errp, "a node board identity needs a name, part and "
                       "revision");
            return;
        }
        if (m->node_board_name &&
            (strlen(m->node_board_name) > 0x3f ||
             strlen(m->node_board_part) > 0x3f ||
             strlen(m->node_board_revision) > 0x3f)) {
            error_setg(errp, "a node board identity field is too long");
            return;
        }
        l1_board_add_identity_eeprom(bus, IDENTITY_NODE_ADDR7,
            sgi_l1_identity_chassis_area(),
            sgi_l1_identity_board_area(
                m->node_board_name ? m->node_board_name : "NODE",
                m->node_board_part ? m->node_board_part : "NOTOBSERVED0",
                m->node_board_revision ? m->node_board_revision : "00",
                NULL, 'N', 0, NULL, NULL));
    } else if (!strcmp(m->brick_type, "ibrick")) {
        if (m->node_board_name || m->node_board_part ||
            m->node_board_revision) {
            error_setg(errp, "a node board identity is for the C-brick only");
            return;
        }
        if (!m->mac || net_parse_macaddr(mac, m->mac) < 0 ||
            !memcmp(mac, "\0\0\0\0\0\0", 6) || (mac[0] & 1)) {
            error_setg(errp, "an I-brick needs a nonzero unicast mac");
            return;
        }
        l1_board_add_identity_eeprom(bus, IDENTITY_NODE_ADDR7,
            sgi_l1_identity_chassis_area(),
            sgi_l1_identity_board_area("POWER", "NOTOBSERVED0", "00", NULL,
                                       'S', 0, NULL, NULL));
        l1_board_add_identity_eeprom(bus, IDENTITY_LOGIC_ADDR7,
            sgi_l1_identity_chassis_area(),
            sgi_l1_identity_board_area("LOGIC", "NOTOBSERVED0", "00", mac,
                                       'I', 0, NULL, NULL));
    } else {
        error_setg(errp, "brick-type is cbrick or ibrick");
        return;
    }
}

static void l1_board_init(MachineState *machine)
{
    L1Board *b = g_new0(L1Board, 1);
    L1BoardMachine *lm = L1_BOARD_MACHINE(machine);
    DeviceState *sim_dev, *or_dev;
    MemoryRegion *quart_window;
    uint64_t ram_size = machine->ram_size;
    int i;

    if (ram_size != RAM_SIZE_FULL && ram_size != RAM_SIZE_ALIASED) {
        error_report("sgi-l1-board RAM is 512 KiB, or 128 KiB where the "
                     "size probe finds the part aliased; got %" PRIu64
                     " bytes", ram_size);
        exit(1);
    }

    b->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    l1_board_load_flash(b, machine->firmware, &error_fatal);

    sim_dev = qdev_new(TYPE_MCF5206E_SIM);
    object_property_set_link(OBJECT(sim_dev), "m68k-cpu", OBJECT(b->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(sim_dev), "boot-rom", OBJECT(&b->flash),
                             &error_abort);
    /* On-chip UART 1 is the power-bay link; serial ports 0 to 3 are below. */
    if (serial_hd(4)) {
        qdev_prop_set_chr(sim_dev, "uart1-chardev", serial_hd(4));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sim_dev), &error_fatal);
    b->sim = MCF5206E_SIM(sim_dev);
    l1_board_identity(lm, mcf5206e_sim_mbus(b->sim), &error_fatal);

    /* CS1: clock RAM. */
    {
        DeviceState *rtc = qdev_new("sysbus-m48t02");

        sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
        memory_region_add_subregion(mcf5206e_sim_cs_window(b->sim, CS_NVRAM), 0,
                                    sysbus_mmio_get_region(SYS_BUS_DEVICE(rtc),
                                                           0));
    }

    /* CS2: external RAM, repeated through the 512 KiB window if smaller. */
    {
        MemoryRegion *window = mcf5206e_sim_cs_window(b->sim, CS_RAM);

        memory_region_add_subregion(window, 0, machine->ram);
        for (i = 1; i * ram_size < RAM_SIZE_FULL; i++) {
            MemoryRegion *alias = g_new0(MemoryRegion, 1);

            memory_region_init_alias(alias, NULL, "sgi-l1-board.ram.alias",
                                     machine->ram, 0, ram_size);
            memory_region_add_subregion(window, i * ram_size, alias);
        }
    }

    /* CS3 and CS5: management USB controller slots. CS6 and CS7 are unused. */
    l1_board_unimp_window(mcf5206e_sim_cs_window(b->sim, CS_USB0),
                          "usb-controller-slot0", 64 * KiB);
    l1_board_unimp_window(mcf5206e_sim_cs_window(b->sim, CS_USB1),
                          "cs5-device", 64 * KiB);

    /*
     * CS4: the four-channel UART. One interrupt line serves all four
     * channels, which the board presents as external level 4. Serial port
     * 0 is channel 2, the operator console; 1 is channel 3 (the node link)
     * and 2 and 3 are channels 0 and 1 (the peer links).
     */
    quart_window = mcf5206e_sim_cs_window(b->sim, CS_QUART);
    or_dev = qdev_new(TYPE_OR_IRQ);
    object_property_set_int(OBJECT(or_dev), "num-lines", QUART_CHANNELS,
                            &error_abort);
    qdev_realize_and_unref(or_dev, NULL, &error_fatal);
    qdev_connect_gpio_out(or_dev, 0,
                          qdev_get_gpio_in_named(sim_dev, "ext-irq",
                                                 QUART_EXT_IRQ));
    {
        static const int channel_serial[QUART_CHANNELS] = { 2, 3, 0, 1 };

        for (i = 0; i < QUART_CHANNELS; i++) {
            serial_mm_init(quart_window, i * 8, 0,
                           qdev_get_gpio_in(or_dev, i), QUART_CLOCK_HZ / 16,
                           serial_hd(channel_serial[i]), DEVICE_BIG_ENDIAN);
        }
    }

    error_setg(&b->migration_blocker, "sgi-l1-board does not support "
               "snapshots or migration");
    if (migrate_add_blocker(&b->migration_blocker, &error_fatal) < 0) {
        exit(1);
    }

    qemu_register_reset(l1_board_reset, b);
}

static void l1_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI L1 controller board, C-, I- and R-brick (ColdFire)";
    mc->init = l1_board_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m5206");
    mc->default_ram_id = "sgi-l1-board.ram";
    mc->default_ram_size = RAM_SIZE_FULL;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    object_class_property_add_str(oc, "brick-type", l1_board_get_brick_type,
                                  l1_board_set_brick_type);
    object_class_property_set_description(oc, "brick-type",
        "Brick whose identity EEPROMs the two-wire bus carries: cbrick or "
        "ibrick; none if unset");
    object_class_property_add_str(oc, "node-board-name",
                                  l1_board_get_node_board_name,
                                  l1_board_set_node_board_name);
    object_class_property_add_str(oc, "node-board-part",
                                  l1_board_get_node_board_part,
                                  l1_board_set_node_board_part);
    object_class_property_add_str(oc, "node-board-revision",
                                  l1_board_get_node_board_revision,
                                  l1_board_set_node_board_revision);
    object_class_property_set_description(oc, "node-board-name",
        "C-brick node board name, as the behavioural L1 takes it");
    object_class_property_set_description(oc, "node-board-part",
        "C-brick node board part number, as the behavioural L1 takes it");
    object_class_property_set_description(oc, "node-board-revision",
        "C-brick node board revision, as the behavioural L1 takes it");
    object_class_property_add_str(oc, "mac", l1_board_get_mac,
                                  l1_board_set_mac);
    object_class_property_set_description(oc, "mac",
        "I-brick logic board station address");
}

static const TypeInfo l1_board_machine_info = {
    .name          = TYPE_L1_BOARD_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(L1BoardMachine),
    .class_init    = l1_board_class_init,
};

static void l1_board_register_types(void)
{
    type_register_static(&l1_board_machine_info);
}

type_init(l1_board_register_types)
