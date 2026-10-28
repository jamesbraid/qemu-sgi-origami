/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Octane (IP30) functional profile with optional IMPACT or VPro graphics.
 * Fitted populations come from the checked IP30 catalogue. Device models
 * retain their documented functional approximations; catalogue data does
 * not establish additional physical behavior.
 */
#include "qemu/osdep.h"
#include "hw/char/sgi-ioc3-parallel.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "qemu/bswap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-board-record.h"
#include "hw/misc/sgi-heart.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/misc/sgi-odyssey.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/scsi/qlogic-isp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "hw/block/flash.h"
#include "target/mips/cpu.h"
#include "hw/mips/sgi/initial-flash.h"
#include "hw/mips/sgi/sgi-board-mac.h"
#include "hw/mips/sgi/sgi-cpu.h"
#include "hw/mips/sgi/prom-image.h"
#include "hw/mips/sgi/sgi-machines.h"
#include "hw/mips/sgi/sgi-graphics.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/core/hotplug.h"
#include "hw/mips/sgi/sgi-storage.h"

#define OCTANE_SHORT_BASE 0x10000000ULL
#define OCTANE_SHORT_SPAN (16 * MiB)
/*
 * The four Crossbow ports the Octane gives graphics and expansion boards,
 * the catalogue's slots widget-9 to widget-12. Published inventories show
 * IMPACT boards at 10, 11 and 12, and the stock IDE probes 9 to 12.
 */
#define OCTANE_GRAPHICS_WIDGET_FIRST 9
#define OCTANE_GRAPHICS_WIDGETS 4
#define OCTANE_LONG_SPAN (1ULL << 36)
#define OCTANE_LONG_BRIDGE (15ULL << 36)
#define OCTANE_FLASH_SECTOR (64 * KiB)
#define OCTANE_HEART_REGS 0x0ff00000ULL
#define OCTANE_HEART_WIDGET 0x18000000ULL
#define OCTANE_BANK_UNIT (32 * MiB)
#define OCTANE_PHYSICAL_SPAN (1ULL << 40)
#define OCTANE_ATTR0_BASE (1ULL << 40)
#define OCTANE_HIGH_GUARD_BASE 0x20000000ULL
#define OCTANE_HIGH_GUARD_SIZE 0x4000

typedef struct OctaneMachineState {
    MachineState parent;
    MemoryRegion high_guard;
    uint64_t bank_size[HEART_BANKS];
    /* Which bank<N>-size properties the launch set, one bit per bank. */
    uint8_t banks_set;
    const SGIModelTopology *model;
    const SGIModelNode *node;
    char *topology_name;
    /* nodes=, or zero for the topology's largest row. */
    uint32_t nodes_requested;
    /*
     * Graphics pipes (docs/system/mips/sgi/ip30.rst, Graphics): each is a
     * -device in one of the four graphics ports, by its slot name, or the
     * topology's default pipe, created at init and realized by
     * octane_machine_done() once no -device turned out to be a pipe.
     * x-uncatalogued-graphics skips the catalogue checks and the default.
     */
    SGIXbowState *crossbow;
    /* The processor's physical address space, which holds the long windows. */
    MemoryRegion *physical;
    SGIXioLink graphics_links[OCTANE_GRAPHICS_WIDGETS];
    bool graphics_claimed[OCTANE_GRAPHICS_WIDGETS];
    MemoryRegion graphics_long[OCTANE_GRAPHICS_WIDGETS];
    bool uncatalogued_graphics;
    DeviceState *default_pipe;
    unsigned int pipes;
    char *first_pipe;
    Notifier machine_done;
    /* Catalogue storage item name -> the block node or drive it binds. */
    GHashTable *storage;
    bool flash_select;
    bool flash_strap;
    /* mac=, or NULL for SGI_BOARD_DEFAULT_MAC. */
    char *mac;
} OctaneMachineState;

/* Whether @t is a topology this machine can build: one HEART node. */
static bool octane_ip30_topology(const SGIModelTopology *t)
{
    return t->generation == SGI_MODEL_GENERATION_IP30 && t->nodes &&
           !t->nodes->next && t->nodes->value->heart;
}

/*
 * The topology the machine builds: the chassis topology= names, or the
 * product's default, with the node-population row nodes= selects. This is
 * the resolution the SN machines and qemu-sgi-machine-init use.
 */
static const SGIModelTopology *octane_model(const char *product,
                                           const char *name,
                                           unsigned int nodes, Error **errp)
{
    const char *selected = name ? name : sgi_model_default_topology(product);
    const SGIModelTopology *chassis = selected ?
        sgi_model_product_topology(sgi_machines(), product, selected) : NULL;
    const SGIModelTopology *model;

    if (!chassis) {
        g_autoptr(GString) names = g_string_new(NULL);

        for (const SGIModelTopologyList *entry = sgi_machines()->topologies;
             entry; entry = entry->next) {
            if (sgi_model_topology_matches_product(entry->value, product)) {
                g_string_append_printf(names, "%s%s", names->len ? ", " : "",
                                       entry->value->name);
            }
        }
        error_setg(errp, "%s: unknown topology '%s'; known topologies: %s",
                   product, selected ? selected : "(none)", names->str);
        return NULL;
    }
    model = sgi_model_resolve_population(chassis, nodes, errp);
    if (model && !octane_ip30_topology(model)) {
        error_setg(errp, "%s requires its checked IP30 topology", product);
        return NULL;
    }
    return model;
}

/* The product's default topology, which the compiled catalogue carries. */
static const SGIModelTopology *octane_default_model(const char *product)
{
    const SGIModelTopology *model = octane_model(product, NULL, 0, NULL);

    g_assert(model);
    return model;
}

static uint64_t octane_model_ram(const SGIModelNode *node)
{
    uint64_t total = 0;
    uint64List *bank;

    for (bank = node->bank_mib; bank; bank = bank->next) {
        total += bank->value * MiB;
    }
    return total;
}

static const SGIModelBoardRecord *octane_board_record(OctaneMachineState *s,
                                                     const char *role,
                                                     Error **errp)
{
    for (SGIModelBoardRecordList *item = s->node->heart->board_records;
         item; item = item->next) {
        if (!strcmp(item->value->role, role)) {
            return item->value;
        }
    }
    error_setg(errp, "IP30 catalogue has no %s board identity", role);
    return NULL;
}

static bool octane_attach_record(OctaneMachineState *s, Object *owner,
                                  const char *child, OneWireBus *bus,
                                  const char *role, Error **errp)
{
    const SGIModelBoardRecord *data = octane_board_record(s, role, errp);
    SGIBoardRecord record;

    if (!data) {
        return false;
    }
    record = (SGIBoardRecord) {
        data->barcode, data->part, data->revision, data->name,
    };
    return sgi_board_record_attach(owner, child, bus, &record, data->serial,
                                   role, errp);
}

/* Whether the catalogue lets one populated bank have @size bytes. */
static bool octane_bank_size_accepted(const SGIModelNode *node, uint64_t size)
{
    const uint64List *item;

    for (item = node->accepted_bank_mib; item; item = item->next) {
        if (size == item->value * MiB) {
            return true;
        }
    }
    return false;
}

/*
 * The catalogue's bank rule (docs/system/mips/sgi/ip30.rst, Memory kits and
 * capacity): banks fill from bank 0 without a gap, each populated bank
 * takes an accepted capacity, and on a product that requires it no bank
 * is larger than the one before.
 */
static bool octane_valid_banks(OctaneMachineState *s)
{
    bool empty = false;

    for (unsigned i = 0; i < HEART_BANKS; i++) {
        uint64_t size = s->bank_size[i];

        if (!size) {
            empty = true;
            continue;
        }
        if (empty || !octane_bank_size_accepted(s->node, size) ||
            (i && s->node->banks_descending && size > s->bank_size[i - 1])) {
            return false;
        }
    }
    return s->bank_size[0] != 0;
}

/*
 * Fill the banks for @total bytes the way a buyer would, largest DIMM
 * pairs first in the fewest banks. The accepted capacities are powers of
 * two, so this finds a layout whenever any layout makes @total.
 */
static bool octane_layout_banks(OctaneMachineState *s, uint64_t total)
{
    const uint64List *item;

    for (unsigned i = 0; i < HEART_BANKS; i++) {
        uint64_t best = 0;

        for (item = s->node->accepted_bank_mib; item; item = item->next) {
            uint64_t size = item->value * MiB;

            if (size <= total && size > best) {
                best = size;
            }
        }
        s->bank_size[i] = best;
        total -= best;
    }
    return !total && octane_valid_banks(s);
}

/* "64, 128, 256 or 512": the accepted bank capacities, for a refusal. */
static char *octane_bank_sizes(const SGIModelNode *node)
{
    GString *out = g_string_new(NULL);
    const uint64List *item;

    for (item = node->accepted_bank_mib; item; item = item->next) {
        g_string_append_printf(out, "%s%" PRIu64,
                               item == node->accepted_bank_mib ? "" :
                               item->next ? ", " : " or ", item->value);
    }
    return g_string_free(out, false);
}

/* Settle bank_size[] from bank<N>-size or -m; refusals go to @errp. */
static bool octane_fit_memory(OctaneMachineState *s, const char *product,
                              uint64_t ram_size, Error **errp)
{
    g_autofree char *sizes = octane_bank_sizes(s->node);
    const char *order = s->node->banks_descending ? ", largest first" : "";

    if (!s->banks_set) {
        if (!octane_layout_banks(s, ram_size)) {
            error_setg(errp, "%s cannot make %" PRIu64 " MiB of RAM from "
                       "one to four banks of %s MiB filled from bank 0%s",
                       product, ram_size / MiB, sizes, order);
            return false;
        }
        return true;
    }
    for (unsigned i = 0; i < HEART_BANKS; i++) {
        if (!(s->banks_set & (1 << i))) {
            s->bank_size[i] = 0;
        }
    }
    if (!octane_valid_banks(s)) {
        error_setg(errp, "%s unsupported HEART bank population: banks take "
                   "%s MiB and fill from bank 0 without a gap%s", product,
                   sizes, order);
        return false;
    }
    if (s->bank_size[0] + s->bank_size[1] + s->bank_size[2] +
        s->bank_size[3] != ram_size) {
        error_setg(errp, "%s HEART banks hold %" PRIu64 " MiB, but the "
                   "machine's memory (-m) is %" PRIu64 " MiB", product,
                   (s->bank_size[0] + s->bank_size[1] + s->bank_size[2] +
                    s->bank_size[3]) / MiB, ram_size / MiB);
        return false;
    }
    return true;
}

static MemTxResult octane_high_guard_read(void *opaque, hwaddr addr,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "octane: unsupported %u-byte read at "
                  "physical 0x%" HWADDR_PRIx "\n", size,
                  (hwaddr)(OCTANE_HIGH_GUARD_BASE + addr));
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult octane_high_guard_write(void *opaque, hwaddr addr,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "octane: unsupported %u-byte write at "
                  "physical 0x%" HWADDR_PRIx "\n", size,
                  (hwaddr)(OCTANE_HIGH_GUARD_BASE + addr));
    return MEMTX_ERROR;
}

static const MemoryRegionOps octane_high_guard_ops = {
    .read_with_attrs = octane_high_guard_read,
    .write_with_attrs = octane_high_guard_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static void octane_reset(MachineState *machine, ResetType type)
{
    OctaneMachineState *s = (OctaneMachineState *)machine;
    uint8_t descriptor[4];
    uint32_t base = 0;
    CPUState *cpu;

    qemu_devices_reset(type);
    /* Synthetic machine setup; the PROM may replace these descriptors. */
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        uint64_t capacity = s->bank_size[bank];
        uint32_t value = capacity ?
            0x80000000U | ((capacity / OCTANE_BANK_UNIT - 1) << 16) | base : 0;

        stl_be_p(descriptor, value);
        if (address_space_write(&address_space_memory,
                                OCTANE_HEART_REGS + 0x20 + bank * 4,
                                MEMTXATTRS_UNSPECIFIED, descriptor,
                                sizeof(descriptor)) != MEMTX_OK) {
            error_setg(&error_fatal, "octane cannot map HEART bank %u", bank);
        }
        base += capacity / OCTANE_BANK_UNIT;
    }
    CPU_FOREACH(cpu) {
        cpu_reset(cpu);
    }
}

static bool octane_fit_pci(MachineState *machine, SGIBridgeState *bridge,
                           const char *nvram, const char *nvram_clock,
                           Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)machine;
    PCIBus *bus = PCI_HOST_BRIDGE(bridge)->bus;
    const SGIModelIoBoard *board = s->model->io_boards ?
        s->model->io_boards->value : NULL;
    g_autofree char *mac = sgi_board_mac(s->mac, 0, errp);
    PCIDevice *pci;
    Chardev *parallel_chr;

    if (!mac) {
        return false;
    }
    /* The onboard I/O this function builds is the catalogue's ip30 board. */
    if (!board || board->preset != SGI_MODEL_IO_BOARD_PRESET_IP30 ||
        !board->has_ioc3_serial) {
        error_setg(errp, "%s catalogue topology '%s' has no ip30 I/O board "
                   "with an IOC3 serial", MACHINE_GET_CLASS(machine)->name,
                   s->model->name);
        return false;
    }

    for (unsigned slot = 0; slot < 2; slot++) {
        pci = pci_new(PCI_DEVFN(slot, 0), TYPE_QLOGIC_ISP1040);
        object_property_add_child(OBJECT(machine),
                                  slot ? "external-scsi" : "internal-scsi",
                                  OBJECT(pci));
        if (!pci_realize_and_unref(pci, bus, errp)) {
            return false;
        }
        qlogic_isp_handle_legacy_cmdline(DEVICE(pci));
    }

    pci = pci_new(PCI_DEVFN(2, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(machine), "ioc3", OBJECT(pci));
    qdev_prop_set_uint16(DEVICE(pci), "subsystem-id",
                         SGI_IOC3_SUBSYS_IP30_SYSBOARD);
    qdev_prop_set_uint64(DEVICE(pci), "onewire-serial", board->ioc3_serial);
    qdev_prop_set_uint32(DEVICE(pci), "population",
                         SGI_IOC3_POPULATION_SERIAL_A |
                         SGI_IOC3_POPULATION_SERIAL_B |
                         SGI_IOC3_POPULATION_ETHERNET |
                         SGI_IOC3_POPULATION_KEYBOARD_MOUSE |
                         SGI_IOC3_POPULATION_PARALLEL |
                         SGI_IOC3_POPULATION_ONEWIRE_IDENTITY);
    {
        DeviceState *parallel = DEVICE(object_resolve_path_component(
            OBJECT(pci), "parallel"));

        /* Rev F is photographed; Octane2 Rev B is inferred from Rev D. */
        qdev_prop_set_uint16(parallel, "parallel-part",
                             SGI_IOC3_PAR_PART_TL16PIR552);
        qdev_prop_set_uint16(parallel, "fifo-depth", 16);
    }
    /* Public IP30 wiring assigns GPIO2 as the read-only product input. */
    qdev_prop_set_uint32(DEVICE(pci), "gpio-input-mask", 1u << 2);
    qdev_prop_set_uint32(DEVICE(pci), "gpio-input-values",
                         s->node->heart->gpio_classic ? 1u << 2 : 0);
    qdev_prop_set_uint8(DEVICE(pci), "timekeeper-kind",
                       SGI_IOC3_TIMEKEEPER_DS1685);
    if (nvram &&
        !object_property_set_str(OBJECT(pci), "ds1685-drive", nvram, errp)) {
        object_unref(OBJECT(pci));
        return false;
    }
    if (nvram_clock &&
        !object_property_set_str(OBJECT(pci), "ds1685-clock-drive",
                                 nvram_clock, errp)) {
        object_unref(OBJECT(pci));
        return false;
    }
    /*
     * The IOC3 builds its Ethernet-address part from this, and refuses a
     * different -nic mac=.
     */
    qdev_prop_set_string(DEVICE(pci), "board-mac", mac);
    qdev_prop_set_bit(DEVICE(pci), "phy-ics1890", true);
    qdev_prop_set_uint8(DEVICE(pci), "phy-addr", 1);
    if (serial_hd(0)) {
        qdev_prop_set_chr(DEVICE(pci), "chardev-a", serial_hd(0));
    }
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(pci), "chardev-b", serial_hd(1));
    }
    /*
     * Serial lines 2 and 3 take a write-only transcript of ports A and B,
     * after the ports themselves, as Origin 300 places them.
     */
    if (serial_hd(2)) {
        qdev_prop_set_chr(DEVICE(pci), "record-a", serial_hd(2));
    }
    if (serial_hd(3)) {
        qdev_prop_set_chr(DEVICE(pci), "record-b", serial_hd(3));
    }
    /* The populated TL16PIR552 port takes -parallel, as on the SN boards. */
    parallel_chr = sgi_ioc3_parallel_host_backend(0);
    if (parallel_chr) {
        qdev_prop_set_chr(DEVICE(pci), "parallel-chardev", parallel_chr);
    }
    if (!pci_realize_and_unref(pci, bus, errp) ||
        !octane_attach_record(s, OBJECT(pci), "frontplane-record",
                              SGI_IOC3(pci)->onewire_bus, "frontplane",
                              errp) ||
        !octane_attach_record(s, OBJECT(pci), "power-supply-record",
                              SGI_IOC3(pci)->onewire_bus, "power-supply",
                              errp)) {
        return false;
    }
    /* The fitted SuperIO line is Bridge input 4; Ethernet uses input 2. */
    qdev_connect_gpio_out(DEVICE(pci), 0,
                          qdev_get_gpio_in(DEVICE(bridge), 4));

    pci = pci_new(PCI_DEVFN(3, 0), TYPE_SGI_RAD1_OCTANE);
    object_property_add_child(OBJECT(machine), "rad", OBJECT(pci));
    return pci_realize_and_unref(pci, bus, errp);
}

/*
 * The boot flash and DS1685 backings: the catalogue's node0-flash, nvram0
 * and nvram0-clock bindings, or the older sgi-nvram0 drive form, which
 * still works. Both forms of one store at once, and a clock record
 * without its NVRAM, are refused as on the SN machines.
 */
static bool octane_storage_backing(OctaneMachineState *s, const char *product,
                                   const char **flash, const char **nvram,
                                   const char **nvram_clock, Error **errp)
{
    BlockBackend *legacy_nvram = blk_by_name("sgi-nvram0");
    MachineState *machine = MACHINE(s);
    const char *node;

    if (!sgi_storage_check(s->storage, product, product, s->model, errp)) {
        return false;
    }
    node = sgi_storage_node(s->storage, "node0-flash");
    if (!sgi_storage_check_boot_prom(product, machine->firmware, !!node, 1,
                                     false, errp)) {
        return false;
    }
    *flash = node;

    *nvram = sgi_storage_node(s->storage, "nvram0");
    *nvram_clock = sgi_storage_node(s->storage, "nvram0-clock");
    if (legacy_nvram && (*nvram || *nvram_clock)) {
        error_setg(errp, "%s: %s and -drive id=sgi-nvram0 both back "
                   "the same Timekeeper NVRAM", product,
                   *nvram ? "nvram0" : "nvram0-clock");
        return false;
    }
    if (*nvram_clock && !*nvram) {
        error_setg(errp, "%s: nvram0-clock needs nvram0", product);
        return false;
    }
    if (legacy_nvram) {
        /* The DS1685 finds the legacy "sgi-nvram0-clock" itself. */
        *nvram = blk_name(legacy_nvram);
    }
    return true;
}

/* Build the machine; every refusal is returned through @errp. */
static bool octane_realize(MachineState *machine, bool octane2, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)machine;
    DeviceState *heart = qdev_new(TYPE_SGI_HEART);
    DeviceState *xbow = qdev_new(TYPE_SGI_XBOW);
    DeviceState *bridge = qdev_new(TYPE_SGI_BRIDGE);
    SGIXbowState *crossbow = SGI_XBOW(xbow);
    MemoryRegion *system = get_system_memory();
    MemoryRegion *physical = g_new0(MemoryRegion, 1);
    MemoryRegion *attr0 = g_new0(MemoryRegion, 1);
    MemoryRegion *firmware = NULL;
    DeviceState *flash_chip = NULL;
    MemoryRegion *proxy = g_new0(MemoryRegion, 1);
    MemoryRegion *proxy_route = g_new0(MemoryRegion, 1);
    MemoryRegion *short_view = g_new0(MemoryRegion, 1);
    MemoryRegion *heart_local = g_new0(MemoryRegion, 1);
    MemoryRegion *long_view = g_new0(MemoryRegion, 1);
    MIPSCPU *cpu;
    Clock *cpuclk;
    const SGIModelProcessor *processor;
    const SGIModelBoardRecord *system_board;
    const char *product = octane2 ? "octane2" : "octane";
    const char *flash_node, *nvram, *nvram_clock;
    uint64_t flash_size;
    uint16_t links = (1 << 8) | (1 << 15);

    if (machine->kernel_filename) {
        error_setg(errp, "%s: stock-PROM boot only; boot a kernel through "
                   "the PROM's own TFTP netboot or a disk image, not -kernel",
                   product);
        return false;
    }
    s->model = octane_model(product, s->topology_name, s->nodes_requested,
                            errp);
    if (!s->model) {
        return false;
    }
    s->node = s->model->nodes->value;
    processor = s->node->cpu_slots->value->assembly;

    if (machine->smp.cpus != 1) {
        error_setg(errp, "%s supports one CPU", product);
        return false;
    }
    if (!octane_fit_memory(s, product, machine->ram_size, errp)) {
        return false;
    }
    if (!octane_storage_backing(s, product, &flash_node, &nvram, &nvram_clock,
                                errp)) {
        return false;
    }
    flash_chip = qdev_new(TYPE_PFLASH_CFI01);
    object_property_add_child(OBJECT(machine), "boot-flash",
                              OBJECT(flash_chip));
    /* Compatibility geometry and identifiers; the fitted part is unknown. */
    qdev_prop_set_string(flash_chip, "name", "octane.boot-flash");
    flash_size = s->node->flash->size;
    qdev_prop_set_uint32(flash_chip, "num-blocks",
                         flash_size / OCTANE_FLASH_SECTOR);
    qdev_prop_set_uint64(flash_chip, "sector-length", OCTANE_FLASH_SECTOR);
    qdev_prop_set_uint8(flash_chip, "width", 2);
    qdev_prop_set_uint8(flash_chip, "device-width", 2);
    qdev_prop_set_bit(flash_chip, "big-endian", true);
    qdev_prop_set_uint16(flash_chip, "id0", 0x00b0);
    qdev_prop_set_uint16(flash_chip, "id1", 0x66a8);
    /*
     * Bound flash migrates as the SN machines' does: the CFI01 part
     * carries its command state, its array migrates as RAM, and the
     * destination writes that array back to the shared backing once it
     * runs.
     */
    if (flash_node &&
        !object_property_set_str(OBJECT(flash_chip), "drive", flash_node,
                                 errp)) {
        return false;
    }
    sgi_bridge_connect_cfi_flash(SGI_BRIDGE(bridge), PFLASH_CFI01(flash_chip));
    if (!sysbus_realize(SYS_BUS_DEVICE(flash_chip), errp)) {
        return false;
    }
    firmware = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash_chip), 0);
    if (machine->firmware) {
        g_autofree char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                               machine->firmware);
        g_autoptr(SGIPromImage) image = NULL;
        const uint8_t *payload;
        size_t length;

        if (!path) {
            error_setg(errp, "%s: cannot find PROM image '%s'", product,
                       machine->firmware);
            return false;
        }
        image = sgi_prom_image_load(path, flash_size, errp);
        if (!image) {
            return false;
        }
        payload = g_bytes_get_data(image->payload, &length);
        sgi_ip30_initial_flash(memory_region_get_ram_ptr(firmware),
                               flash_size, payload, length);
        memory_region_flush_rom_device(firmware, 0, flash_size);
    }

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, processor->clock_hz);
    {
        DeviceState *dev = qdev_new(machine->cpu_type);

        qdev_connect_clock_in(dev, "clk-in", cpuclk);
        /*
         * IRIX's IP30 kernel arms its scheduler clock a few register
         * instructions after reading or writing Count; the Count-sample
         * window keeps a host delay between the two from costing a full
         * Count wrap (docs/system/mips/sgi/ip30.rst).
         */
        if (!object_property_set_bool(OBJECT(dev), "count-sample-window",
                                      true, errp)) {
            return false;
        }
        /*
         * The catalogue's processor part: PRId, inferred from kernel
         * inventory records, and on the R12000 the FPU identification and
         * the approximated reset word with the cache it selects
         * (docs/system/mips/sgi/ip30.rst).
         */
        if (!object_property_set_bool(OBJECT(dev), "big-endian", true, errp) ||
            !sgi_cpu_apply_processor(dev, processor, errp) ||
            !qdev_realize(dev, NULL, errp)) {
            return false;
        }
        cpu = MIPS_CPU(dev);
    }
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    s->physical = physical;
    memory_region_init(physical, OBJECT(machine), "octane.physical",
                       OCTANE_PHYSICAL_SPAN);
    memory_region_add_subregion(system, 0, physical);
    memory_region_init_alias(attr0, OBJECT(machine), "octane.attr0",
                             physical, 0, OCTANE_PHYSICAL_SPAN);
    memory_region_add_subregion(system, OCTANE_ATTR0_BASE, attr0);

    object_property_add_child(OBJECT(machine), "heart", OBJECT(heart));
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        g_autofree char *name = g_strdup_printf("bank%u-size", bank);

        qdev_prop_set_uint64(heart, name, s->bank_size[bank]);
    }
    qdev_prop_set_uint8(heart, "widget-revision",
                         s->node->heart->widget_revision);
    qdev_prop_set_uint64(heart, "widget-id-64",
                        s->node->heart->widget_id_64);
    if (s->node->heart->widget_status_f_measured) {
        qdev_prop_set_bit(heart, "widget-status-f-measured", true);
    }
    if (!object_property_set_link(OBJECT(heart), "cpu-memory",
                                  OBJECT(physical), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(heart), errp) ||
        !octane_attach_record(s, OBJECT(heart), "processor-record",
                              SGI_HEART(heart)->onewire_bus, "processor",
                              errp)) {
        return false;
    }
    memory_region_init_io(&s->high_guard, OBJECT(machine),
                          &octane_high_guard_ops, s, "octane.high-guard",
                          OCTANE_HIGH_GUARD_SIZE);
    /* Override every bank view while leaving the rest of SDRAM mapped. */
    memory_region_add_subregion_overlap(physical, OCTANE_HIGH_GUARD_BASE,
                                        &s->high_guard, HEART_BANKS + 1);
    /*
     * Physical 0 to 16 KiB is the first 16 KiB of SDRAM, the memory HEART
     * places at 0x20000000, and the processor reaches those bytes only
     * here: the guard above faults the high address. HEART builds the window
     * from the bank views, so it follows the bank descriptors and is RAM
     * for TCG. Reset leaves it alone and it adds no migration state
     * (docs/datasheets/ip30-machine.md).
     */
    memory_region_add_subregion(
        physical, 0,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(heart), HEART_MMIO_SDRAM_BASE));
    memory_region_add_subregion(
        physical, OCTANE_HEART_REGS,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(heart), 0));
    /* HEART levels 0 through 4 reach CPU IP2 through IP6. */
    qdev_connect_gpio_out_named(heart, "processor", 0, cpu->env.irq[2]);
    qdev_connect_gpio_out_named(heart, "processor", 1, cpu->env.irq[3]);
    qdev_connect_gpio_out_named(heart, "processor", 2, cpu->env.irq[4]);
    qdev_connect_gpio_out_named(heart, "processor", 3, cpu->env.irq[5]);
    qdev_connect_gpio_out_named(heart, "processor", 4, cpu->env.irq[6]);

    memory_region_init(proxy, OBJECT(machine), "octane.bridge-fabric",
                       SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    object_property_add_child(OBJECT(machine), "bridge", OBJECT(bridge));
    system_board = octane_board_record(s, "system-board", errp);
    if (!system_board) {
        return false;
    }
    qdev_prop_set_string(bridge, "board-barcode", system_board->barcode);
    qdev_prop_set_string(bridge, "board-part", system_board->part);
    qdev_prop_set_string(bridge, "board-revision", system_board->revision);
    qdev_prop_set_string(bridge, "board-name", system_board->name);
    qdev_prop_set_uint64(bridge, "onewire-serial", system_board->serial);
    if (!object_property_set_link(OBJECT(bridge), "fabric", OBJECT(proxy),
                                  errp) ||
        !object_property_set_link(OBJECT(bridge),
                                  s->flash_select ? "flash-select1" : "flash",
                                  OBJECT(firmware), errp)) {
        return false;
    }
    qdev_prop_set_uint32(bridge, "revision",
                         s->node->heart->bridge_widget_revision);
    qdev_prop_set_bit(bridge, "flash-select", s->flash_strap);
    /* The Octane2's empty second select read 0xffff, not a bus error. */
    qdev_prop_set_bit(bridge, "flash-empty-reads-high", true);
    qdev_prop_set_bit(bridge, "flash-reject-invalid-writes", true);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    object_unref(OBJECT(flash_chip));

    object_property_add_child(OBJECT(machine), "crossbow", OBJECT(xbow));
    s->crossbow = crossbow;
    qdev_prop_set_uint32(xbow, "revision",
                          s->model->crossbars->value->widget_revision);
    qdev_prop_set_uint64(xbow, "span", OCTANE_SHORT_SPAN);
    qdev_prop_set_uint16(xbow, "links-present", links);
    qdev_prop_set_uint16(xbow, "startup-links-alive", links);
    qdev_prop_set_uint16(xbow, "link-reset-restore-alive", links);
    if (!sgi_xbow_attach_endpoint(crossbow, 8,
            sgi_xio_port_cpu_face(OBJECT(heart), 0),
            sgi_xio_port_packet_face(OBJECT(heart), 0), errp) ||
        !sgi_xbow_attach_endpoint(crossbow, 15,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0),
            sgi_bridge_packet_face(SGI_BRIDGE(bridge)), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(xbow), errp)) {
        return false;
    }


    memory_region_init_alias(proxy_route, OBJECT(machine),
                             "octane.bridge-fabric-route",
                             sgi_xbow_packet_ingress(crossbow, 15), 0,
                             SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
    memory_region_add_subregion(proxy, 0, proxy_route);
    memory_region_init_alias(short_view, OBJECT(machine), "octane.short",
                             sgi_xbow_ingress(crossbow, 8), 0,
                             SGI_XBOW_WIDGETS * OCTANE_SHORT_SPAN);
    memory_region_add_subregion(physical, OCTANE_SHORT_BASE, short_view);
    /* The CPU addresses local HEART without a Crossbow loopback. */
    memory_region_init_alias(heart_local, OBJECT(machine),
                             "octane.heart-local",
                             sgi_xio_port_cpu_face(OBJECT(heart), 0), 0,
                             0x1000);
    memory_region_add_subregion_overlap(physical, OCTANE_HEART_WIDGET,
                                        heart_local, 1);
    memory_region_init_alias(long_view, OBJECT(machine), "octane.bridge-long",
                             sgi_xbow_packet_ingress(crossbow, 8),
                             (15ULL << 48) | (1ULL << 47),
                             SGI_BRIDGE_WIDGET_SIZE);
    memory_region_add_subregion(physical, OCTANE_LONG_BRIDGE, long_view);

    return octane_fit_pci(machine, SGI_BRIDGE(bridge), nvram, nvram_clock,
                          errp);
}

/*
 * The machine's side of a graphics board fitted by slot name
 * (TYPE_SGI_XIO_SLOTS, xio-board.h): widget-9 to widget-12, the Crossbow
 * ports the Octane gives graphics boards. The board takes its fabric from
 * the link to the port and attaches to the realized Crossbow live; its
 * long window, the 64 GiB of physical space the processor reaches the
 * widget through by way of HEART's port, maps once a board is there.
 */
static SGIXioLink *octane_claim_xio_slot(Object *obj, Object *board,
                                         const char *name,
                                         SGIXioSlotPlace *place,
                                         Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    unsigned int widget, index;
    g_autofree char *link_name = NULL;
    g_autofree char *long_name = NULL;

    if (!g_str_has_prefix(name, "widget-") ||
        qemu_strtoui(name + strlen("widget-"), NULL, 10, &widget) ||
        widget < OCTANE_GRAPHICS_WIDGET_FIRST ||
        widget >= OCTANE_GRAPHICS_WIDGET_FIRST + OCTANE_GRAPHICS_WIDGETS) {
        error_setg(errp, "topology '%s' has no graphics slot %s; its slots "
                   "are widget-9, widget-10, widget-11 and widget-12",
                   s->model->name, name);
        return NULL;
    }
    if (place->module != SGI_XIO_BOARD_MODULE_UNSET && place->module) {
        error_setg(errp, "graphics slot %s is in module 0, not module %u",
                   name, place->module);
        return NULL;
    }
    index = widget - OCTANE_GRAPHICS_WIDGET_FIRST;
    if (s->graphics_claimed[index]) {
        error_setg(errp, "graphics slot %s already holds a board", name);
        return NULL;
    }
    s->graphics_claimed[index] = true;
    link_name = g_strdup_printf("%s-link", name);
    sgi_xio_link_init(&s->graphics_links[index], obj, link_name,
                      OBJECT(s->crossbow), widget, board, 0);
    long_name = g_strdup_printf("octane.%s-long", name);
    memory_region_init_alias(&s->graphics_long[index], obj, long_name,
                             sgi_xbow_packet_ingress(s->crossbow, 8),
                             (uint64_t)widget << 48, OCTANE_LONG_SPAN);
    memory_region_add_subregion(s->physical, widget * OCTANE_LONG_SPAN,
                                &s->graphics_long[index]);
    place->module = 0;
    place->widget = widget;
    place->position = index;
    place->name = g_strdup(name);
    place->label = g_strdup(name);
    /*
     * The one board keeps its own marked synthetic identity, "EMUG00",
     * like the other IP30 identities; offsetting it by port would make the
     * PROM name the same board differently in each port.
     */
    place->own_identity = true;
    return &s->graphics_links[index];
}

/* A board given a fabric outright is a fixture's, not one of the ports. */
static bool octane_claim_xio_fabric(Object *obj, Object *board,
                                    MemoryRegion *fabric, uint32_t module,
                                    uint32_t widget, char **name,
                                    SGIXioLink **link, Error **errp)
{
    *link = NULL;
    return true;
}

static bool octane_get_uncatalogued_graphics(Object *obj, Error **errp)
{
    return ((OctaneMachineState *)obj)->uncatalogued_graphics;
}

static void octane_set_uncatalogued_graphics(Object *obj, bool value,
                                             Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "x-uncatalogued-graphics cannot change once the "
                   "machine is built");
        return;
    }
    ((OctaneMachineState *)obj)->uncatalogued_graphics = value;
}

static const HotplugHandler *octane_get_hotplug_handler(MachineState *machine,
                                                        DeviceState *dev)
{
    SGIModelGraphicsFamilyKind family;

    return sgi_graphics_pipe_family(dev, &family) ?
           HOTPLUG_HANDLER(machine) : NULL;
}

/*
 * Every pipe, a -device or the default, before it realizes: checked
 * against the catalogue with every unset setting filled from the slot's
 * defaults, then given what the IP30 board fixes for an IMPACT board: the
 * serial-number record the PROM names the board from (its default barcode
 * and ROM serial are the marked synthetic "EMUG00", like the other IP30
 * identities), and 32-bit DMA page-list entries, which IRIX 6.5.30's X
 * server and kernel and the Linux IMPACTSR driver all write.
 */
static void octane_pre_plug(const HotplugHandler *hotplug_dev,
                            DeviceState *dev, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)hotplug_dev;
    SGIModelGraphicsFamilyKind family;

    if (!sgi_graphics_pipe_family(dev, &family)) {
        return;
    }
    if (!s->uncatalogued_graphics &&
        !sgi_graphics_check_pipe(MACHINE_GET_CLASS(s)->name, s->model,
                                 s->model->graphics,
                                 QAPI_LIST_LENGTH(s->model->nodes), dev,
                                 family, s->pipes, s->first_pipe, errp)) {
        return;
    }
    if (family == SGI_MODEL_GRAPHICS_FAMILY_KIND_MGRAS) {
        qdev_prop_set_bit(dev, "board-record", true);
        qdev_prop_set_uint8(dev, "page-list-entry-bytes", 4);
    }
}

static void octane_plug(const HotplugHandler *hotplug_dev, DeviceState *dev,
                        Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)hotplug_dev;
    const SGIModelGraphicsSlot *slot;
    SGIModelGraphicsFamilyKind family;

    if (!sgi_graphics_pipe_family(dev, &family)) {
        return;
    }
    s->pipes++;
    if (!s->first_pipe) {
        slot = sgi_graphics_pipe_slot(s->model->graphics, dev, family);
        s->first_pipe = g_strdup(slot ? slot->name : dev->id ? dev->id :
                                 object_get_typename(OBJECT(dev)));
    }
}

/*
 * Fit the default pipe when no -device was one and -vga none did not ask
 * for none, and bring every claimed graphics port's link alive, with the
 * pre-reset health restore the HEART and Bridge links get: the stock PROM
 * resets each link it finds attached and expects it back alive. It runs
 * before the machine's cold reset.
 */
static void octane_machine_done(Notifier *notifier, void *data)
{
    OctaneMachineState *s = container_of(notifier, OctaneMachineState,
                                         machine_done);

    if (s->default_pipe) {
        DeviceState *dev = s->default_pipe;

        s->default_pipe = NULL;
        if (s->pipes) {
            object_unref(OBJECT(dev));
        } else {
            sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        }
    }
    for (unsigned int i = 0; i < OCTANE_GRAPHICS_WIDGETS; i++) {
        if (sgi_xio_link_complete(&s->graphics_links[i])) {
            sgi_xbow_mark_widget_alive(s->crossbow,
                                       OCTANE_GRAPHICS_WIDGET_FIRST + i,
                                       true, &error_fatal);
        }
    }
}

/*
 * Check -vga and create the topology's default pipe, which
 * octane_machine_done() realizes once it knows no -device was a pipe. It
 * is created now, while the machine initializes, so it is not taken for a
 * hot-plugged device when it realizes.
 */
static void octane_init_graphics(OctaneMachineState *s, Error **errp)
{
    const SGIModelGraphicsSlot *slot;
    bool none;

    if (!s->uncatalogued_graphics) {
        if (!sgi_graphics_vga_none(MACHINE_GET_CLASS(s)->name, &none, errp)) {
            return;
        }
        slot = none ? NULL : sgi_graphics_default_slot(s->model->graphics);
        if (slot) {
            s->default_pipe = sgi_graphics_new_default(slot);
        }
    }
    s->machine_done.notify = octane_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void octane_init(MachineState *machine)
{
    octane_realize(machine, false, &error_fatal);
    octane_init_graphics((OctaneMachineState *)machine, &error_fatal);
}

static void octane2_init(MachineState *machine)
{
    octane_realize(machine, true, &error_fatal);
    octane_init_graphics((OctaneMachineState *)machine, &error_fatal);
}

static char *octane_get_topology(Object *obj, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    return g_strdup(s->topology_name ? s->topology_name : s->model->name);
}

static void octane_set_topology(Object *obj, const char *name, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "topology cannot change once the machine is built");
        return;
    }
    g_free(s->topology_name);
    s->topology_name = g_strdup(name);
}

static void octane_get_nodes(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    uint32_t value = s->nodes_requested ? s->nodes_requested :
                     QAPI_LIST_LENGTH(s->model->nodes);

    visit_type_uint32(v, name, &value, errp);
}

static void octane_set_nodes(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    uint32_t value;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "%s cannot change once the machine is built", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!value) {
        error_setg(errp, "nodes must be at least 1");
        return;
    }
    s->nodes_requested = value;
}

static void octane_instance_init_common(Object *obj, const char *product)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    uint64List *default_bank;

    s->model = octane_default_model(product);
    s->node = s->model->nodes->value;
    /* The catalogue's wiring, which a launcher may override. */
    s->flash_select = s->node->flash->has_select && s->node->flash->select;
    s->flash_strap = s->node->flash->has_strap && s->node->flash->strap;
    default_bank = s->node->bank_mib;
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        g_assert(default_bank);
        s->bank_size[bank] = default_bank->value * MiB;
        default_bank = default_bank->next;
    }
}

static void octane_instance_init(Object *obj)
{
    octane_instance_init_common(obj, "octane");
}

static void octane2_instance_init(Object *obj)
{
    octane_instance_init_common(obj, "octane2");
}

static void octane_instance_finalize(Object *obj)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    g_free(s->topology_name);
    g_free(s->first_pipe);
    if (s->default_pipe) {
        object_unref(OBJECT(s->default_pipe));
    }
    g_free(s->mac);
    if (s->storage) {
        g_hash_table_destroy(s->storage);
    }
}

static bool octane_get_flash_select(Object *obj, Error **errp)
{
    return ((OctaneMachineState *)obj)->flash_select;
}

static void octane_set_flash_select(Object *obj, bool value, Error **errp)
{
    ((OctaneMachineState *)obj)->flash_select = value;
}

static bool octane_get_flash_strap(Object *obj, Error **errp)
{
    return ((OctaneMachineState *)obj)->flash_strap;
}

static void octane_set_flash_strap(Object *obj, bool value, Error **errp)
{
    ((OctaneMachineState *)obj)->flash_strap = value;
}

/* bank<N>-size: @opaque is the bank. */
static void octane_get_bank_size(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;

    visit_type_uint64(v, name, &s->bank_size[(uintptr_t)opaque], errp);
}

static void octane_set_bank_size(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    OctaneMachineState *s = (OctaneMachineState *)obj;
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        s->bank_size[(uintptr_t)opaque] = value;
        s->banks_set |= 1 << (uintptr_t)opaque;
    }
}

/*
 * Every machine property is a class property, so "-M octane,help" lists
 * them; values that come from the catalogue are set per instance.
 */
static void octane_class_add_properties(ObjectClass *oc, const char *product)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

    object_class_property_add_bool(oc, "x-uncatalogued-graphics",
                                   octane_get_uncatalogued_graphics,
                                   octane_set_uncatalogued_graphics);
    object_class_property_set_description(oc, "x-uncatalogued-graphics",
        "Skip the graphics catalogue: fit no default pipe, accept any -vga "
        "and check no graphics -device against the topology. For "
        "register-contract tests that use the machine to carry a board.");
    for (uintptr_t bank = 0; bank < HEART_BANKS; bank++) {
        g_autofree char *name = g_strdup_printf("bank%u-size",
                                                (unsigned)bank);

        object_class_property_add(oc, name, "uint64", octane_get_bank_size,
                                  octane_set_bank_size, NULL, (void *)bank);
        object_class_property_set_description(oc, name,
            "Capacity of this HEART bank in bytes; setting any bank "
            "leaves the unset ones empty (default: -m's fewest banks)");
    }
    for (const SGIModelOfferingList *entry = sgi_machines()->offerings;
         entry; entry = entry->next) {
        if (!g_str_equal(entry->value->product, product)) {
            continue;
        }
        for (const SGIModelStorageItemList *item = entry->value->storage;
             item; item = item->next) {
            if (g_hash_table_add(seen, item->value->name)) {
                sgi_storage_class_add_property(
                    oc, offsetof(OctaneMachineState, storage),
                    item->value->name);
            }
        }
    }
}

/* The most processors any of @product's offerings fits. */
static unsigned int octane_max_cpus(const char *product)
{
    unsigned int max_cpus = 0;

    for (const SGIModelOfferingList *entry = sgi_machines()->offerings;
         entry; entry = entry->next) {
        if (g_str_equal(entry->value->product, product)) {
            max_cpus = MAX(max_cpus, entry->value->smp);
        }
    }
    return max_cpus;
}

/*
 * Octane and Octane2 differ only in catalogue data, so one class init
 * serves both, keyed by the product.
 */
static void octane_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SGIXioSlotsClass *xc = SGI_XIO_SLOTS_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);
    const char *product = data;
    const SGIModelTopology *t = octane_default_model(product);
    const SGIModelProcessor *processor =
        t->nodes->value->cpu_slots->value->assembly;
    const char **cpu_types = g_new0(const char *, 2);

    object_class_property_add_str(oc, "topology", octane_get_topology,
                                  octane_set_topology);
    object_class_property_set_description(oc, "topology",
        "Catalogue topology to build (query-sgi-machines lists them)");
    object_class_property_add(oc, "nodes", "uint32", octane_get_nodes,
                              octane_set_nodes, NULL, NULL);
    object_class_property_set_description(oc, "nodes",
        "Node boards to fit: 1, the one HEART node board");

    object_class_property_add_bool(oc, "flash-select",
                                   octane_get_flash_select,
                                   octane_set_flash_select);
    object_class_property_set_description(oc, "flash-select",
        "Wire the boot flash to Bridge flash select 1 (default from "
        "query-sgi-machines; the board's wiring is unknown)");
    object_class_property_add_bool(oc, "flash-strap", octane_get_flash_strap,
                                   octane_set_flash_strap);
    object_class_property_set_description(oc, "flash-strap",
        "Strap the PROM to boot from Bridge flash select 1 (default from "
        "query-sgi-machines; the board's strap is unknown)");
    octane_class_add_properties(oc, product);
    sgi_board_mac_class_add_property(oc, offsetof(OctaneMachineState, mac),
        "Onboard Ethernet address, held in the IOC3 bus's Ethernet-address "
        "part (default " SGI_BOARD_DEFAULT_MAC ", a placeholder)");

    /* The catalogue's processor part is the one CPU type the board takes. */
    cpu_types[0] = g_strconcat(processor->kind, MIPS_CPU_TYPE_SUFFIX, NULL);
    mc->desc = t->description;
    mc->init = g_str_equal(product, "octane2") ? octane2_init : octane_init;
    mc->reset = octane_reset;
    mc->valid_cpu_types = cpu_types;
    mc->default_cpu_type = cpu_types[0];
    mc->default_cpus = 1;
    mc->max_cpus = octane_max_cpus(product);
    mc->default_ram_size = octane_model_ram(t->nodes->value);
    /* -nic and an unqualified -drive reach the onboard IOC3 and ISP. */
    mc->default_nic = "sgi-ioc3-eth";
    mc->block_default_type = IF_SCSI;
    /*
     * QEMU's default parallel backend is a text console nothing reads; the
     * IOC3's port gets a null backend from sgi_ioc3_parallel_host_backend().
     */
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    /* A graphics pipe is a -device in one of the four graphics ports. */
    mc->get_hotplug_handler = octane_get_hotplug_handler;
    hc->pre_plug = octane_pre_plug;
    hc->plug = octane_plug;
    xc->claim = octane_claim_xio_slot;
    xc->claim_fabric = octane_claim_xio_fabric;
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_MGRAS);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_SGI_ODYSSEY);
}

static const TypeInfo octane_machine_types[] = {
    {
        .name = MACHINE_TYPE_NAME("octane"),
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(OctaneMachineState),
        .instance_init = octane_instance_init,
        .instance_finalize = octane_instance_finalize,
        .class_init = octane_class_init,
        .class_data = "octane",
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_XIO_SLOTS },
            { TYPE_HOTPLUG_HANDLER },
            { }
        },
    },
    {
        .name = MACHINE_TYPE_NAME("octane2"),
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(OctaneMachineState),
        .instance_init = octane2_instance_init,
        .instance_finalize = octane_instance_finalize,
        .class_init = octane_class_init,
        .class_data = "octane2",
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_XIO_SLOTS },
            { TYPE_HOTPLUG_HANDLER },
            { }
        },
    },
};

DEFINE_TYPES(octane_machine_types)
