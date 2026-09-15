/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 (IP27) family mechanics and node device.
 * Origin 200 supplies a compiled topology to the generic model consumer.
 * -bios and pflash fill a node's boot flash here.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/host-utils.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-hub.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/mips/mips.h"
#include "system/address-spaces.h"
#include "system/numa.h"
#include "system/reset.h"
#include "migration/vmstate.h"
#include "trace/control.h"
#include "trace.h"
#include "cpu.h"
#include "sn0.h"

#define SN0_HUB_PRIVATE_SIZE (128 * KiB)
#define SN0_HUB_PRIVATE_HALF (SN0_HUB_PRIVATE_SIZE / 2)

/*
 * The PROM's second execution home during the cache-as-RAM phase: a RAM
 * copy of its own text, reached by an ASID re-point rather than a PC
 * change. See sn-promdata.c and the datasheet section it cites.
 */
#define SN0_PROM_COPY_PADDR 0x01a00000

/* MIPS CPU input 0 is IP0. Hardware interrupt inputs begin at IP2. */
#define SN0_MIPS_HARDWARE_IRQ_OFFSET 2

const SN0FlashImplementation sn0_flash = {
    .sector_size = 0x10000,
    .width = 1,
    .mappings = 1,
    .big_endian = true,
    .manufacturer_id = 0x01,
    .device_id = 0xd5,
    .unlock_address = { 0x5555, 0x2aaa },
};

/*
 * The per-CPU cache-as-RAM private window (physical 0x10_0000, keyed on
 * the PROM boot-status flag and the PC being in the PROM) is
 * sn-promdata.c's SNPromDataState and its functions, shared with
 * sgi-sn1-node. See that file and docs/datasheets/firmware-structures.md,
 * "The PROM cache-as-RAM phase and its boot-status bit", for the
 * contract. This file only computes the bounds sn0_node_create_cpus
 * passes it, the peer write counter its trace line reports below, and
 * the array-level migration wrapper.
 */
static uint64_t sn0_promdata_peer_writes(const void *peer)
{
    const SN0NodeState *node = peer;

    return node->backdoor_ecc_state.writes + node->backdoor_dir_state.writes;
}

static bool sn0_promdata_needed(void *opaque)
{
    SN0NodeState *node = opaque;

    return node->promdata[0].present;
}

static int sn0_promdata_post_load(void *opaque, int version_id)
{
    SN0NodeState *node = opaque;
    unsigned int slice;

    /* Reapply how far each processor's cache-only phase had got. */
    for (slice = 0; slice < SN0_MAX_CPUS_PER_NODE; slice++) {
        sn_promdata_apply_load(&node->promdata[slice]);
    }
    return 0;
}

/*
 * vmstate_sn_promdata_slice (sn-promdata.c) is the one-slice subsection,
 * shared with sgi-sn1-node; this array-level wrapper is per node type
 * because it names SN0NodeState's own promdata[] field. Its name stays
 * "sn0-flash/promdata" for wire compatibility with streams saved before
 * the node device carried a shared promdata implementation.
 */
static const VMStateDescription vmstate_sn0_promdata = {
    .name = "sn0-flash/promdata",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sn0_promdata_needed,
    .post_load = sn0_promdata_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(promdata, SN0NodeState, SN0_MAX_CPUS_PER_NODE, 1,
                             vmstate_sn_promdata_slice, SNPromDataState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Named "sn0-flash" for wire compatibility: the pflash device migrates
 * its own state and the back doors migrate with their RAM blocks, so
 * this carries only promdata. Version 4 drops the back-door buffers and
 * refuses an older stream rather than translating one.
 */
static const VMStateDescription vmstate_sn0_flash = {
    .name = "sn0-flash",
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sn0_promdata,
        NULL
    }
};

/*
 * The Hub's flash controller: the decode between the 64-bit junk bus and
 * the byte-wide chip. Reads pack the chip 1:1 and repeat its image across
 * each decoded window; command writes present the byte-wide command
 * port on 64-bit cycles only, chip byte N at bus offset 8N, the mapping
 * the vendor flash programmer states directly.
 */
static uint64_t sn0_boot_mirror_read(void *opaque, hwaddr addr, unsigned size)
{
    SN0NodeState *node = opaque;
    MemoryRegion *chip =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < size; i++) {
        uint64_t byte = 0;

        memory_region_dispatch_read(chip, (addr + i) % node->flash_size,
                                    &byte, MO_8, MEMTXATTRS_UNSPECIFIED);
        value = (value << 8) | (byte & 0xff);
    }
    return value;
}

static void sn0_boot_mirror_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    SN0NodeState *node = opaque;
    MemoryRegion *chip =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);

    /* The Hub exposes the byte-wide command port only on 64-bit writes. */
    if (size != 8) {
        return;
    }
    memory_region_dispatch_write(chip, (addr / 8) % node->flash_size,
                                 val & 0xff, MO_8, MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps sn0_boot_mirror_ops = {
    .read = sn0_boot_mirror_read,
    .write = sn0_boot_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * The floor beneath a back-door window: firmware's presence probe writes
 * a pattern, then zero, and calls a bank absent when neither survives, so
 * accept both writes and echo neither. 0xa5 is an arbitrary choice -- the
 * real data lines float.
 */
#define SN0_BACKDOOR_ABSENT_VALUE 0xa5a5a5a5a5a5a5a5ULL

static void sn0_backdoor_trace_absent(const SN0BackdoorState *s, hwaddr addr,
                                      unsigned size, bool write,
                                      uint64_t value)
{
    CPUState *cpu = current_cpu;

    trace_sn0_backdoor_absent(cpu ? cpu->cpu_index : -1, s->node, s->window,
                              cpu ? cpu->cc->get_pc(cpu) : 0, addr, size,
                              write, value);
}

static uint64_t sn0_backdoor_absent_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    if (trace_event_get_state_backends(TRACE_SN0_BACKDOOR_ABSENT)) {
        /* As the guest sees it: the memory core keeps only @size bytes. */
        sn0_backdoor_trace_absent(opaque, addr, size, false,
                                  SN0_BACKDOOR_ABSENT_VALUE &
                                  MAKE_64BIT_MASK(0, size * 8));
    }
    return SN0_BACKDOOR_ABSENT_VALUE;
}

static void sn0_backdoor_absent_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size)
{
    /* Dropped; firmware's sizing walk runs with no fault guard here. */
    if (trace_event_get_state_backends(TRACE_SN0_BACKDOOR_ABSENT)) {
        sn0_backdoor_trace_absent(opaque, addr, size, true, val);
    }
}

static const MemoryRegionOps sn0_backdoor_absent_ops = {
    .read = sn0_backdoor_absent_read,
    .write = sn0_backdoor_absent_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * Real storage only as wide as the plainer directory format: the low two
 * bytes of each eight-byte entry, the rest reading zero -- plain RAM
 * would answer firmware's width probe "premium format". Writes drop the
 * dead bytes so a read is a plain load.
 */
#define SN0_BACKDOOR_ENTRY_BYTES 8
#define SN0_BACKDOOR_LIVE_BYTES  2

/*
 * The aliases below fold the window, so an offset the memory core hands
 * a handler is already an index into the storage. Naturally aligned, so
 * the access lies inside one entry.
 */
static uint8_t *sn0_backdoor_entry(SN0BackdoorState *s, hwaddr addr,
                                   unsigned size)
{
    g_assert(QEMU_IS_ALIGNED(addr, size));
    g_assert(addr + size <= s->data_size);
    return s->data + addr;
}

/*
 * The bytes of this access that are stored: the tail of the entry it
 * lies in, which is empty when the access ends before the live bytes.
 */
static uint64_t sn0_backdoor_live_mask(hwaddr addr, unsigned size)
{
    unsigned int dead = SN0_BACKDOOR_ENTRY_BYTES - SN0_BACKDOOR_LIVE_BYTES;
    unsigned int offset = addr & (SN0_BACKDOOR_ENTRY_BYTES - 1);
    unsigned int live = offset >= dead ? size : size - MIN(size, dead - offset);

    return live ? MAKE_64BIT_MASK(0, live * 8) : 0;
}

/*
 * The offset and the storage index are now one number: the aliases fold
 * the window, so the window offset the guest used is gone by the time a
 * handler runs. Only a write reaches one.
 */
static void sn0_backdoor_trace_populated(SN0BackdoorState *s, hwaddr addr,
                                         unsigned size, bool write,
                                         uint64_t value)
{
    CPUState *cpu = current_cpu;

    trace_sn0_backdoor_populated(cpu ? cpu->cpu_index : -1, s->node,
                                 s->window, cpu ? cpu->cc->get_pc(cpu) : 0,
                                 addr, size, write, value, addr);
}

/*
 * Only a path that cannot take a host pointer arrives here -- the
 * monitor, the debugger, a qtest. A processor's load is served from the
 * storage by the translation fast path and never enters the memory core.
 */
static uint64_t sn0_backdoor_populated_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    SN0BackdoorState *s = opaque;
    const uint8_t *entry = sn0_backdoor_entry(s, addr, size);
    uint64_t raw;

    switch (size) {
    case 1:
        raw = ldub_p(entry);
        break;
    case 2:
        raw = lduw_be_p(entry);
        break;
    case 4:
        raw = ldl_be_p(entry);
        break;
    case 8:
        raw = ldq_be_p(entry);
        break;
    default:
        g_assert_not_reached();
    }
    if (trace_event_get_state_backends(TRACE_SN0_BACKDOOR_POPULATED)) {
        sn0_backdoor_trace_populated(s, addr, size, false, raw);
    }
    return raw;
}

static void sn0_backdoor_populated_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    SN0BackdoorState *s = opaque;
    uint8_t *entry = sn0_backdoor_entry(s, addr, size);
    uint64_t stored = val & sn0_backdoor_live_mask(addr, size);

    s->writes++;
    switch (size) {
    case 1:
        stb_p(entry, stored);
        break;
    case 2:
        stw_be_p(entry, stored);
        break;
    case 4:
        stl_be_p(entry, stored);
        break;
    case 8:
        stq_be_p(entry, stored);
        break;
    default:
        g_assert_not_reached();
    }
    memory_region_flush_rom_device(&s->storage, addr, size);
    if (trace_event_get_state_backends(TRACE_SN0_BACKDOOR_POPULATED)) {
        sn0_backdoor_trace_populated(s, addr, size, true, val);
    }
}

static const MemoryRegionOps sn0_backdoor_populated_ops = {
    .read = sn0_backdoor_populated_read,
    .write = sn0_backdoor_populated_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * One window's storage, as a ROM device so a processor's read comes out
 * of it with no dispatch and no lock while a write still traps here.
 * The window repeats each bank's storage across the bank stride, and an
 * alias per repeat leaves that fold to the memory core.
 */
static void sn0_backdoor_configure(SN0BackdoorState *s, SN0NodeState *node,
                                   MemoryRegion *window, hwaddr base,
                                   uint64_t bank_stride, const char *name)
{
    unsigned int bank, repeat, next = 0;
    uint64_t total = 0;

    /* An address-bit fold needs a power of two on both sides. */
    g_assert(is_power_of_2(bank_stride));
    for (bank = 0; bank < node->populated_banks; bank++) {
        uint64_t span = (uint64_t)node->bank_mb[bank] * MiB / 4;

        g_assert(is_power_of_2(span) && span <= bank_stride);
        s->bank_offset[bank] = total;
        s->folds += bank_stride / span;
        total += span;
    }
    memory_region_init_rom_device(&s->storage, OBJECT(node),
                                  &sn0_backdoor_populated_ops, s, name,
                                  total, &error_fatal);
    s->data = memory_region_get_ram_ptr(&s->storage);
    s->data_size = total;

    s->fold = g_new0(MemoryRegion, s->folds);
    for (bank = 0; bank < node->populated_banks; bank++) {
        uint64_t span = (uint64_t)node->bank_mb[bank] * MiB / 4;

        for (repeat = 0; repeat < bank_stride / span; repeat++) {
            g_autofree char *fold_name =
                g_strdup_printf("%s.bank%u.fold%u", name, bank, repeat);

            memory_region_init_alias(&s->fold[next], OBJECT(node), fold_name,
                                     &s->storage, s->bank_offset[bank], span);
            memory_region_add_subregion(window,
                                        base + bank * bank_stride +
                                        repeat * span, &s->fold[next]);
            next++;
        }
    }
}

static uint32_t sn0_board_mode_bits(SN0MachineState *s)
{
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(s);

    return s->composition ? s->composition->hardware->cpu_mode_bits
                          : smc->model->cpu_mode_bits;
}

void sn0_mode_bits_reset(void *opaque)
{
    CPUMIPSState *env = &MIPS_CPU(opaque)->env;
    SN0MachineState *s = SN0_MACHINE(qdev_get_machine());
    uint32_t mode_bits = sn0_board_mode_bits(s);

    env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
                       (mode_bits & SN0_PROCESSOR_MODE_MASK);
}

/* Read a big-endian field of a node's boot flash the way a processor does. */
static uint64_t sn0_flash_field(SN0NodeState *node, hwaddr offset,
                                unsigned int bytes)
{
    MemoryRegion *chip =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < bytes; i++) {
        uint64_t byte = 0;

        memory_region_dispatch_read(chip, (offset + i) % node->flash_size,
                                    &byte, MO_8, MEMTXATTRS_UNSPECIFIED);
        value = (value << 8) | (byte & 0xff);
    }
    return value;
}

/*
 * A node board's boot flash records the configuration that board was
 * provisioned for, while the processor latches its mode bits from the board
 * itself. Firmware compares the two and stops with indicator 0xb4 before it
 * has a console, so an image provisioned for a different board is
 * indistinguishable from a dead machine unless something says so here.
 */
static void sn0_report_node_flash_mode(SN0MachineState *s)
{
    uint32_t board = sn0_board_mode_bits(s);
    unsigned int node;

    for (node = 0; node < s->num_nodes; node++) {
        SN0NodeState *node_state = s->nodes[node];
        uint32_t flash;

        if (!node_state->flash ||
            sn0_flash_field(node_state, SN0_CONFIG_OFFSET + 8, 8) !=
            SN0_CONFIG_MAGIC) {
            continue;
        }
        flash = sn0_flash_field(node_state, SN0_CONFIG_OFFSET + 4, 4);
        if (!((flash ^ board) & SN0_PROCESSOR_MODE_MASK)) {
            continue;
        }
        warn_report("node %u boot flash is provisioned for a different node "
                    "board: it describes processor mode 0x%08x where this "
                    "board latches 0x%08x, differing in 0x%08x. Firmware "
                    "stops with indicator 0xb4 and no console output.",
                    node, flash, board,
                    (flash ^ board) & SN0_PROCESSOR_MODE_MASK);
    }
}

/*
 * After every device's reset: the firmware window and mode-bits writes
 * must follow the Hub's and the CPUs' resets, the PCI reassignment the
 * devices clearing their registers.
 */
static void sn0_machine_reset(MachineState *machine, ResetType type)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    CPUState *cs;
    unsigned int node, slice;

    qemu_devices_reset(type);

    /*
     * Nothing resets a MIPS processor automatically: without this the
     * machine services a reset request with the program counter
     * untouched.
     */
    CPU_FOREACH(cs) {
        cpu_reset(cs);
        sn0_mode_bits_reset(cs);
    }
    for (node = 0; node < s->num_nodes; node++) {
        SN0NodeState *node_state = s->nodes[node];

        for (slice = 0; slice < node_state->nr_cpus; slice++) {
            sgi_hub_set_cpu_enable(&node_state->hub, slice, true, true);
        }
    }
    sn0_report_node_flash_mode(s);
}

/*
 * Arm the sgi_hub_led trace event: the only progress this machine shows
 * while firmware picks a console. One line is enough -- the log backend
 * already forces LOG_TRACE on every run. The emitted text is parsed, so
 * the event stays the single route.
 */
static void sn0_show_indicator_codes(Error **errp)
{
    TraceEvent *ev = trace_event_name("sgi_hub_led");

    if (!ev) {
        error_setg(errp, "the sgi_hub_led trace event is missing, so the "
                         "firmware's boot-progress indicator cannot be "
                         "shown; build with it or pass debug-leds=off");
        return;
    }
    trace_event_set_state_dynamic(ev, true);
}

static const CPUArchIdList *sn0_possible_cpu_arch_ids(MachineState *machine)
{
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(machine)->model;
    const SN0ModelProfile *profile;
    unsigned int max_cpus = machine->smp.max_cpus;
    g_autofree unsigned int *cpus = NULL;
    unsigned int index = 0;
    unsigned int node_count = machine->smp.sockets;
    unsigned int node_index;

    if (machine->possible_cpus) {
        assert(machine->possible_cpus->len == max_cpus);
        return machine->possible_cpus;
    }

    cpus = g_new0(unsigned int, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        cpus[node_index] = node_count == 1 ? machine->smp.max_cpus :
            machine->smp.cores * machine->smp.threads;
    }
    profile = sn0_model_find_profile(model, node_count, cpus);

    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * max_cpus);
    machine->possible_cpus->len = max_cpus;
    if (!profile) {
        /*
         * NUMA parsing asks for CPU properties before sn0_init() can reject
         * an unsupported geometry.  Preserve the physical slot mapping long
         * enough for that validation to report the user-facing error.
         */
        for (index = 0; index < max_cpus; index++) {
            CPUArchId *slot = &machine->possible_cpus->cpus[index];
            unsigned int node = index / model->cpu_slots_per_node;
            unsigned int slice = index % model->cpu_slots_per_node;

            slot->type = machine->cpu_type;
            slot->vcpus_count = 1;
            slot->arch_id = node * model->cpu_slots_per_node + slice;
            slot->props.has_socket_id = true;
            slot->props.socket_id = node;
            slot->props.has_core_id = true;
            slot->props.core_id = slice;
            slot->props.has_thread_id = true;
            slot->props.thread_id = 0;
        }
        return machine->possible_cpus;
    }

    for (node_index = 0; node_index < profile->node_count; node_index++) {
        const SN0ModelNodeInstance *node =
            &profile->nodes[node_index];
        unsigned int slice;

        for (slice = 0; slice < node->cpu_count; slice++, index++) {
            CPUArchId *slot = &machine->possible_cpus->cpus[index];

            g_assert(index < max_cpus);
            slot->type = machine->cpu_type;
            slot->vcpus_count = 1;
            slot->arch_id = node->cpu_first_id + slice;
            slot->props.has_socket_id = true;
            slot->props.socket_id = node_index;
            slot->props.has_core_id = true;
            slot->props.core_id = slice;
            slot->props.has_thread_id = true;
            slot->props.thread_id = 0;
        }
    }
    g_assert(index == max_cpus);

    return machine->possible_cpus;
}

static CpuInstanceProperties
sn0_cpu_index_to_props(MachineState *machine, unsigned int cpu_index)
{
    const CPUArchIdList *possible_cpus =
        sn0_possible_cpu_arch_ids(machine);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t sn0_get_default_cpu_node_id(const MachineState *machine,
                                           int index)
{
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(machine)->model;
    const SN0ModelProfile *profile;
    g_autofree unsigned int *cpus = NULL;
    unsigned int node_count = machine->smp.sockets;
    unsigned int node_index;

    cpus = g_new0(unsigned int, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        cpus[node_index] = node_count == 1 ? machine->smp.cpus :
            machine->smp.cores * machine->smp.threads;
    }
    profile = sn0_model_find_profile(model, node_count, cpus);
    if (!profile) {
        return index / model->cpu_slots_per_node;
    }
    for (node_index = 0; node_index < profile->node_count; node_index++) {
        const SN0ModelNodeInstance *node =
            &profile->nodes[node_index];

        if (index < node->cpu_count) {
            return node_index;
        }
        index -= node->cpu_count;
    }
    g_assert_not_reached();
}

static const SN0ModelAddressMap *sn0_node_address_map(SN0NodeState *node)
{
    return node->physical ? &node->physical->map : &node->topology->map;
}

static const SN0ModelMemory *sn0_node_memory_geometry(SN0NodeState *node)
{
    return node->hardware ? &node->hardware->memory : &node->model->memory;
}

static unsigned int sn0_node_cpu_slots(SN0NodeState *node)
{
    return node->hardware ? node->hardware->cpu_slots_per_node :
                            node->model->cpu_slots_per_node;
}

static void sn0_node_create_cpus(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelAddressMap *map = sn0_node_address_map(node);
    const SN0ModelAddressSpace *spaces[SN0_MAX_ADDRESS_SPACES] = {
        &map->memory,
        &map->hub_special,
        &map->xtalk,
        &map->memory_special,
    };
    const SN0ModelRegion *memory_region = &map->node_memory;
    const SN0ModelAddressSpace *hub_special = &map->hub_special;
    hwaddr node_base = sn0_model_node_address(
        map, &map->memory, node->nasid, 0);
    unsigned int node_index = node->nasid;
    unsigned int slice, half, space_index;

    if (node->promdata_filter) {
        sn_promdata_poll_init(&node->promdata_poll, node->promdata,
                              SN0_MAX_CPUS_PER_NODE, node->promdata_fast,
                              sn0_promdata_peer_writes, node);
    }

    for (slice = 0; slice < node->nr_cpus; slice++) {
        g_autofree char *alias_name =
            g_strdup_printf("sn0.node%u.cpu%u.system", node_index, slice);
        g_autofree char *name = g_strdup_printf("cpu%u", slice);
        g_autofree char *promdata_prefix =
            g_strdup_printf("sn0.node%u.cpu%u", node_index, slice);
        g_autofree char *sysmem_name =
            g_strdup_printf("sn0.node%u.cpu%u.memory", node_index, slice);
        g_autofree char *claim_name =
            g_strdup_printf("sn0.node%u.cpu%u.claim", node_index, slice);

        memory_region_init(&node->cpu_sysmem[slice], OBJECT(node),
                           sysmem_name, UINT64_MAX);
        memory_region_init_alias(&node->sysmem_alias[slice], OBJECT(node),
                                 alias_name, system, 0, UINT64_MAX);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[slice], 0,
                                            &node->sysmem_alias[slice], -2);

        /*
         * The Hub's claim layer fills this container: the existing
         * nodes' node-qualified strides route by programmed number,
         * above the static placement, below the nameless local windows.
         */
        memory_region_init(&node->claim_view[slice], OBJECT(node),
                           claim_name, UINT64_MAX);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[slice], 0,
                                            &node->claim_view[slice], -1);

        /*
         * NASID-less addresses name the reading processor's node -- in
         * the memory space only within the cached-alias bound, beyond
         * which, like every explicit NASID, they fall to the Hub's
         * claim layer underneath.
         */
        for (space_index = 0;
             space_index < SN0_MAX_ADDRESS_SPACES; space_index++) {
            const SN0ModelAddressSpace *space_model = spaces[space_index];
            g_autofree char *local_name = g_strdup_printf(
                "sn0.node%u.cpu%u.local-space%u", node_index, slice,
                space_model->selector);
            hwaddr alias_node_base = node_base;
            hwaddr local_offset = 0;
            hwaddr local_size = memory_region->size;

            /*
             * Per space, the public alias table (Linux asm/sn/addrs.h
             * and sn0/addrs.h): the crosstalk space self-decodes the
             * IALIAS, the Hub's own eight-megabyte register window,
             * always. The memory space's nameless window is the CALIAS
             * -- "the first n bytes of memory on the reader's node",
             * based at the cached base -- pre-created here at the
             * largest size the register encodes, then resized and gated
             * by this node's Hub, which gets the region at wiring time.
             * Below the bound an access stays local even while the
             * programmed numbers are in motion -- retiring the window
             * outright put mid-renumbering local accesses into claim
             * transients and broke the two-node firmware boot. The
             * Hub-special space is the UALIAS, uncached references to
             * the memory of the reader's node, position-independent by
             * definition and register-independent; the memory-special
             * space and the LBOOT path are likewise local. Those keep
             * the wide reader-local window.
             */
            if (space_model == &map->xtalk) {
                const SN0ModelRegion *hub_region =
                    &map->hub_registers_local;

                local_offset = hub_region->offset;
                local_size = hub_region->size;
            } else if (space_model == &map->memory) {
                local_size = SGI_HUB_CALIAS_MAX_SIZE;
            }

            memory_region_init_alias(&node->local_space[slice][space_index],
                                     OBJECT(node), local_name, system,
                                     space_model->carrier_base +
                                     alias_node_base +
                                     local_offset,
                                     local_size);
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                space_model->carrier_base + local_offset,
                &node->local_space[slice][space_index], 0);
        }

        /* The bottom 128 KiB of Hub-local space: bit 16 flips per slice. */
        for (half = 0; half < 2; half++) {
            g_autofree char *private_name = g_strdup_printf(
                "sn0.node%u.cpu%u.hub-private%u", node_index, slice, half);
            uint64_t private_offset = node->ram_start +
                (half ^ slice) * SN0_HUB_PRIVATE_HALF;

            memory_region_init_alias(&node->hub_private[slice][half],
                                     OBJECT(node), private_name, node->ram,
                                     private_offset, SN0_HUB_PRIVATE_HALF);
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                hub_special->carrier_base + half * SN0_HUB_PRIVATE_HALF,
                &node->hub_private[slice][half], 1);
        }

        node->cpu[slice] = MIPS_CPU(qdev_new(node->cpu_type));

        /*
         * The cache-only phase exists only under a real PROM; for a
         * directly loaded kernel the filter would be pure forwarding, and
         * an I/O region cannot back translated code, so leave it off.
         */
        if (node->promdata_filter) {
            sn_promdata_init(&node->promdata[slice], OBJECT(node),
                             node->cpu[slice], &node->promdata_poll,
                             node->promdata_fast, node_base,
                             memory_region->size,
                             map->flash_reset.offset, map->flash_reset.size,
                             SN0_PROM_COPY_PADDR, map->flash_reset.size,
                             &node->cpu_sysmem[slice],
                             map->memory.carrier_base,
                             &node->cpu_uncached[slice],
                             &node->cpu_uncached_alias[slice],
                             promdata_prefix);
        }
        object_property_add_child(OBJECT(node), name,
                                  OBJECT(node->cpu[slice]));
        object_property_set_link(OBJECT(node->cpu[slice]), "memory",
                                 OBJECT(&node->cpu_sysmem[slice]),
                                 &error_abort);
        qdev_connect_clock_in(DEVICE(node->cpu[slice]), "clk-in",
                              node->cpuclk);
        object_property_set_bool(OBJECT(node->cpu[slice]), "big-endian", true,
                                 &error_abort);
        qdev_realize(DEVICE(node->cpu[slice]), NULL, &error_abort);
        cpu_mips_irq_init_cpu(node->cpu[slice]);
        cpu_mips_clock_init(node->cpu[slice]);
    }
}

static void sn0_node_map_memory(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelAddressMap *map = sn0_node_address_map(node);
    const SN0ModelMemory *geometry = sn0_node_memory_geometry(node);
    const SN0ModelRegion *local_memory = &map->hub_local_memory;
    const SN0ModelRegion *remote_memory = &map->hub_remote_memory;
    hwaddr node_base = sn0_model_node_address(
        map, &map->memory, node->nasid, 0);
    hwaddr hub_base = sn0_model_node_address(
        map, &map->hub_special, node->nasid, 0);
    uint64_t ram_offset = node->ram_start;
    unsigned int node_index = node->nasid;
    unsigned int bank, segment;

    g_assert(geometry->architectural_banks <= SN0_MAX_MEMORY_BANKS);
    g_assert(geometry->physical_banks <= geometry->architectural_banks);
    g_assert(geometry->bank_stride_mib > 0);
    g_assert(geometry->interleave_slot_stride_mib > 0);
    g_assert(geometry->interleave_slots_per_bank > 0);
    g_assert(geometry->interleave_slots_per_bank <= SN0_MAX_MEMORY_SLOTS);
    g_assert((uint64_t)geometry->interleave_slot_stride_mib *
             geometry->interleave_slots_per_bank <=
             geometry->bank_stride_mib);
    /*
     * A bank no larger than one interleave slot occupies only its first
     * slot. Larger banks are split evenly over every slot in the bank.
     */
    for (bank = 0; bank < node->populated_banks; bank++) {
        uint64_t bank_size = (uint64_t)node->bank_mb[bank] * MiB;
        uint64_t bank_stride = (uint64_t)geometry->bank_stride_mib * MiB;
        uint64_t slot_stride =
            (uint64_t)geometry->interleave_slot_stride_mib * MiB;
        unsigned int segments = bank_size <= slot_stride ?
            1 : geometry->interleave_slots_per_bank;
        uint64_t segment_size = bank_size / segments;

        g_assert(bank_size <= bank_stride);
        g_assert(segments > 0 && bank_size % segments == 0);
        for (segment = 0; segment < segments; segment++) {
            hwaddr segment_addr = (hwaddr)bank * bank_stride +
                                  (hwaddr)segment * slot_stride;
            g_autofree char *name = g_strdup_printf(
                "sn0.node%u.ram.bank%u.slot%u", node_index, bank, segment);

            memory_region_init_alias(&node->mem_slot[bank][segment],
                                     OBJECT(node), name, node->ram,
                                     ram_offset, segment_size);
            memory_region_add_subregion(system, node_base + segment_addr,
                                        &node->mem_slot[bank][segment]);

            /*
             * The Hub-local region mirrors the bottom of local memory;
             * mirror each run separately to keep the slot holes.
             */
            if (segment_addr < local_memory->size) {
                uint64_t alias_size = MIN(segment_size,
                                          local_memory->size - segment_addr);
                g_autofree char *local_name = g_strdup_printf(
                    "sn0.node%u.hub-local-ram.bank%u.slot%u",
                    node_index, bank, segment);

                memory_region_init_alias(
                    &node->hub_local_mem_slot[bank][segment], OBJECT(node),
                    local_name, node->ram, ram_offset, alias_size);
                memory_region_add_subregion(
                    system, hub_base + local_memory->offset + segment_addr,
                    &node->hub_local_mem_slot[bank][segment]);
            }
            if (segment_addr < remote_memory->size) {
                uint64_t alias_size = MIN(segment_size,
                                          remote_memory->size - segment_addr);
                g_autofree char *remote_name = g_strdup_printf(
                    "sn0.node%u.hub-remote-ram.bank%u.slot%u",
                    node_index, bank, segment);

                memory_region_init_alias(
                    &node->hub_remote_mem_slot[bank][segment], OBJECT(node),
                    remote_name, node->ram, ram_offset, alias_size);
                memory_region_add_subregion(
                    system, hub_base + remote_memory->offset + segment_addr,
                    &node->hub_remote_mem_slot[bank][segment]);
            }
            ram_offset += segment_size;
        }
    }
}

static void sn0_node_map_boot_storage(SN0NodeState *node)
{
    const SN0ModelAddressMap *map = sn0_node_address_map(node);
    const SN0ModelRegion *lboot = map->lboot;
    const SN0FlashImplementation *implementation = &sn0_flash;
    MemoryRegion *system = get_system_memory();
    const SN0ModelRegion *flash_reset = &map->flash_reset;
    const SN0ModelRegion *flash_rboot = &map->rboot;
    const SN0ModelRegion *ecc = &map->backdoor_ecc;
    const SN0ModelRegion *directory = &map->backdoor_directory;
    const SN0ModelMemory *geometry = sn0_node_memory_geometry(node);
    hwaddr node_base = sn0_model_node_address(
        map, &map->memory, node->nasid, 0);
    hwaddr hub_base = sn0_model_node_address(
        map, &map->hub_special, node->nasid, 0);
    unsigned int node_index = node->nasid;
    g_autofree char *flash_name =
        g_strdup_printf("sn0.node%u.flash", node_index);
    g_autofree char *hub_name =
        g_strdup_printf("sn0.node%u.flash.hub", node_index);
    g_autofree char *rboot_name =
        g_strdup_printf("sn0.node%u.flash.rboot", node_index);
    g_autofree char *hub_fast_name =
        g_strdup_printf("sn0.node%u.flash.hub.fast", node_index);
    g_autofree char *reset_name =
        g_strdup_printf("sn0.node%u.flash.reset", node_index);
    g_autofree char *ecc_absent_name =
        g_strdup_printf("sn0.node%u.backdoor.ecc.absent", node_index);
    g_autofree char *ecc_name =
        g_strdup_printf("sn0.node%u.backdoor.ecc", node_index);
    g_autofree char *dir_absent_name =
        g_strdup_printf("sn0.node%u.backdoor.dir.absent", node_index);
    g_autofree char *dir_name =
        g_strdup_printf("sn0.node%u.backdoor.dir", node_index);

    /*
     * Without a backing drive the firmware loader seeds this byte-wide
     * flash after realize. Every view uses the same flash device.
     */
    DeviceState *flash_dev = qdev_new(TYPE_PFLASH_CFI02);
    if (node->flash_drive) {
        qdev_prop_set_drive(flash_dev, "drive", node->flash_drive);
    }
    qdev_prop_set_uint32(flash_dev, "num-blocks",
                         flash_reset->size / implementation->sector_size);
    qdev_prop_set_uint32(flash_dev, "sector-length",
                         implementation->sector_size);
    qdev_prop_set_uint8(flash_dev, "width", implementation->width);
    qdev_prop_set_uint8(flash_dev, "mappings", implementation->mappings);
    qdev_prop_set_uint8(flash_dev, "big-endian",
                        implementation->big_endian);
    qdev_prop_set_uint16(flash_dev, "id0", implementation->manufacturer_id);
    qdev_prop_set_uint16(flash_dev, "id1", implementation->device_id);
    qdev_prop_set_uint16(flash_dev, "unlock-addr0",
                         implementation->unlock_address[0]);
    qdev_prop_set_uint16(flash_dev, "unlock-addr1",
                         implementation->unlock_address[1]);
    qdev_prop_set_string(flash_dev, "name", flash_name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(flash_dev), &error_fatal);
    node->flash = PFLASH_CFI02(flash_dev);
    node->flash_size = flash_reset->size;

    MemoryRegion *chip =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(flash_dev), 0);

    if (lboot) {
        /*
         * The LBOOT window: the controller decode, with a romd alias of the
         * chip overlaid where the PROM fetches its relocated code.
         */
        memory_region_init_io(&node->flash_hub_view, OBJECT(node),
                              &sn0_boot_mirror_ops, node, hub_name,
                              lboot->size);
        memory_region_add_subregion(system, hub_base + lboot->offset,
                                    &node->flash_hub_view);
        memory_region_init_alias(&node->flash_hub_fast, OBJECT(node),
                                 hub_fast_name, chip, 0, flash_reset->size);
        memory_region_add_subregion_overlap(
            system, hub_base + flash_reset->offset,
            &node->flash_hub_fast, 1);
    }

    /*
     * The reset-vector view aliases the flash chip directly. Writes through
     * it land on the chip's byte decode, not the controller's strided command
     * port. It overlaps RAM and wins.
     */
    memory_region_init_alias(&node->flash_reset_view, OBJECT(node),
                             reset_name, chip, 0, flash_reset->size);
    memory_region_add_subregion_overlap(system,
                                        node_base + flash_reset->offset,
                                        &node->flash_reset_view, 1);

    /* RBOOT is a maintenance path, never a fetch path: no romd overlay. */
    memory_region_init_io(&node->flash_rboot_view, OBJECT(node),
                          &sn0_boot_mirror_ops, node, rboot_name,
                          flash_rboot->size);
    memory_region_add_subregion(system, hub_base + flash_rboot->offset,
                                &node->flash_rboot_view);

    /* Absent floors cover the whole windows; populated storage overlays. */
    node->backdoor_ecc_state.node = node_index;
    node->backdoor_ecc_state.window = "ecc";
    memory_region_init_io(&node->backdoor_ecc_absent, OBJECT(node),
                          &sn0_backdoor_absent_ops, &node->backdoor_ecc_state,
                          ecc_absent_name, ecc->size);
    memory_region_add_subregion_overlap(
        system, hub_base + ecc->offset,
        &node->backdoor_ecc_absent, -1);
    if (node->populated_banks > 0) {
        sn0_backdoor_configure(&node->backdoor_ecc_state, node, system,
                               hub_base + ecc->offset,
                               ecc->size / geometry->architectural_banks,
                               ecc_name);
    }

    node->backdoor_dir_state.node = node_index;
    node->backdoor_dir_state.window = "dir";
    memory_region_init_io(&node->backdoor_dir_absent, OBJECT(node),
                          &sn0_backdoor_absent_ops, &node->backdoor_dir_state,
                          dir_absent_name, directory->size);
    memory_region_add_subregion_overlap(
        system, hub_base + directory->offset,
        &node->backdoor_dir_absent, -1);
    if (node->populated_banks > 0) {
        sn0_backdoor_configure(&node->backdoor_dir_state, node, system,
                               hub_base + directory->offset,
                               directory->size / geometry->architectural_banks,
                               dir_name);
    }
}

static void sn0_node_create_hub(SN0NodeState *node)
{
    const SN0ModelComponent *hub = node->physical ? node->physical->hub :
                                                  node->topology->hub;
    const SN0ModelAddressMap *map = sn0_node_address_map(node);
    unsigned int node_index = node->nasid;
    g_autofree char *xtalk_name =
        g_strdup_printf("sn0.node%u.xtalk", node_index);
    g_autofree char *device_view_name =
        g_strdup_printf("sn0.node%u.hub.device-view", node_index);
    MemoryRegion *system = get_system_memory();
    MIPSCPU *cpu;
    unsigned int slice;

    memory_region_init(&node->xtalk, OBJECT(node), xtalk_name,
                       SN0_XTALK_SIZE);

    /*
     * The Hub's engines address memory as this node's processors do,
     * minus the per-processor pieces -- a device has no slice to flip and
     * no nameless window. The container is the machine's; the Hub fills
     * it with its claim layer, which routes every node-qualified address
     * by programmed number, and an engine sees nothing else.
     */
    memory_region_init(&node->device_view, OBJECT(node), device_view_name,
                       UINT64_MAX);
    object_property_set_link(OBJECT(&node->hub), "claim-device-view",
                             OBJECT(&node->device_view), &error_fatal);

    qdev_prop_set_uint32(DEVICE(&node->hub), "cpus", node->nr_cpus);
    qdev_prop_set_uint32(DEVICE(&node->hub), "cpu-base", node->cpu_base);
    if (node->physical) {
        qdev_prop_set_uint32(DEVICE(&node->hub), "physical-slot",
                            node->physical->slot);
    }
    if (node->instance) {
        switch (node->instance->module_role) {
        case SN0_MODULE_MASTER:
            qdev_prop_set_bit(DEVICE(&node->hub), "slave-module", false);
            break;
        case SN0_MODULE_SLAVE:
            qdev_prop_set_bit(DEVICE(&node->hub), "slave-module", true);
            break;
        default:
            g_assert_not_reached();
        }
    }
    qdev_prop_set_uint8(DEVICE(&node->hub), "ni-revision",
                        node->physical ? hub->ni_revision : hub->revision);
    if (node->physical) {
        qdev_prop_set_uint8(DEVICE(&node->hub), "widget-revision",
                            hub->widget_revision);
    }
    /*
     * Each PIMM record part gets its own synthetic serial: per-unit as the
     * hub one-wire datasheet's contract requires, never a real machine's.
     */
    qdev_prop_set_uint64(DEVICE(&node->hub), "onewire-serial",
                         node->physical ? hub->serial :
                                          node->instance->hub_serial);
    qdev_prop_set_uint32(
        DEVICE(&node->hub), "population",
        (hub->i2c_controller ?
         SGI_HUB_POPULATION_I2C_CONTROLLER : 0) |
        (hub->system_controller ? SGI_HUB_POPULATION_ELSC : 0));
    qdev_prop_set_uint32(DEVICE(&node->hub), "i2c-nvram-pages",
                         hub->i2c_nvram_pages);
    qdev_prop_set_uint64(DEVICE(&node->hub), "i2c-controller-offset",
                         hub->i2c_controller_offset);
    qdev_prop_set_uint8(DEVICE(&node->hub), "i2c-elsc-addr",
                        hub->i2c_elsc_addr);
    qdev_prop_set_uint8(DEVICE(&node->hub), "i2c-nvram-first-addr",
                        hub->i2c_nvram_first_addr);
    qdev_prop_set_uint8(DEVICE(&node->hub), "onewire-kind",
                        SGI_HUB_ONEWIRE_BLANK_DS2505);
    qdev_prop_set_uint32(DEVICE(&node->hub), "xtalk-widget-shift",
                         SN0_XTALK_WIDGET_SHIFT);
    object_property_set_link(OBJECT(&node->hub), "xtalk",
                             OBJECT(&node->xtalk), &error_fatal);

    /*
     * The claim layer's wiring: the per-view containers the Hub fills,
     * the canonical map its aliases resolve into, and the geometry --
     * the Hub gets offsets to alias by, never the node's own regions.
     */
    for (slice = 0; slice < node->nr_cpus; slice++) {
        g_autofree char *view_prop = g_strdup_printf("claim-view%u", slice);
        object_property_set_link(OBJECT(&node->hub), view_prop,
                                 OBJECT(&node->claim_view[slice]),
                                 &error_fatal);

        /*
         * The one nameless window the cached-alias size bounds: the
         * memory space's, per view. The other spaces' windows are
         * position-independent aliases the register never moves.
         */
        g_autofree char *window_prop =
            g_strdup_printf("calias-window%u", slice);

        object_property_set_link(
            OBJECT(&node->hub), window_prop,
            OBJECT(&node->local_space[slice][0]), &error_fatal);
    }
    object_property_set_link(OBJECT(&node->hub), "claim-target",
                             OBJECT(system), &error_fatal);
    qdev_prop_set_uint32(DEVICE(&node->hub), "claim-node-shift",
                         ctz64(map->node_stride));
    qdev_prop_set_uint32(DEVICE(&node->hub), "claim-space-shift",
                         MIPS_UNCACHED_WINDOW_SHIFT);
    qdev_connect_clock_in(DEVICE(&node->hub), "rt-clk", node->rtclk);
    sysbus_realize(SYS_BUS_DEVICE(&node->hub), &error_fatal);

    for (slice = 0; slice < node->nr_cpus; slice++) {
        cpu = node->cpu[slice];
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "bank0", slice,
            cpu->env.irq[hub->cpu_irq_bank0 +
                         SN0_MIPS_HARDWARE_IRQ_OFFSET]);
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "bank1", slice,
            cpu->env.irq[hub->cpu_irq_bank1 +
                         SN0_MIPS_HARDWARE_IRQ_OFFSET]);
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "rt", slice,
            cpu->env.irq[hub->cpu_irq_rt +
                         SN0_MIPS_HARDWARE_IRQ_OFFSET]);
    }
}

static void sn0_node_map_xtalk_windows(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelAddressMap *map = sn0_node_address_map(node);
    const SN0ModelAddressSpace *xtalk = &map->xtalk;
    const SN0ModelRegion *hub_explicit = &map->hub_registers_node;
    const SN0ModelRegion *hub_local = &map->hub_registers_local;
    unsigned int node_index = node->nasid;
    unsigned int widget;

    g_assert(memory_region_size(&node->hub.regs) == hub_local->size);

    for (widget = 0; widget <= SN0_XTALK_PORT_LAST; widget++) {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.xtalk.small%u",
                            node_index, widget);

        memory_region_init_alias(&node->small_window[widget], OBJECT(node),
                                 name, &node->xtalk,
                                 SN0_XTALK_BASE(widget),
                                 SN0_SMALL_WINDOW_SIZE);
        memory_region_add_subregion(
            system, sn0_model_node_address(map, xtalk, node->nasid,
                                           SN0_SMALL_WINDOW(widget)),
            &node->small_window[widget]);
    }

    memory_region_add_subregion(
        system, sn0_model_node_address(map, xtalk, node->nasid,
                                       SN0_BIG_WINDOW_BASE),
        &node->hub.bwin);
    memory_region_add_subregion(
        system, sn0_model_region_address(map, xtalk, node->nasid,
                                         hub_local, 0),
        &node->hub.regs);

    {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.hub.explicit", node_index);

        memory_region_init_alias(&node->hub_regs_node_view, OBJECT(node),
                                 name, &node->hub.regs, 0,
                                 hub_explicit->size);
    }
    memory_region_add_subregion(
        system, sn0_model_region_address(map, xtalk, node->nasid,
                                         hub_explicit, 0),
        &node->hub_regs_node_view);
}

void sn0_map_cpu_big_windows(SN0MachineState *s)
{
    unsigned int source, slice, target;

    s->big_window_route = g_new0(
        MemoryRegion, s->num_nodes * SN0_MAX_CPUS_PER_NODE *
                      (s->num_nodes + 1));
    for (source = 0; source < s->num_nodes; source++) {
        SN0NodeState *source_node = s->nodes[source];
        const SN0ModelAddressMap *map =
            sn0_node_address_map(source_node);

        for (slice = 0; slice < source_node->nr_cpus; slice++) {
            unsigned int base =
                (source * SN0_MAX_CPUS_PER_NODE + slice) *
                (s->num_nodes + 1);
            MemoryRegion *nameless = &s->big_window_route[base];
            g_autofree char *nameless_name = g_strdup_printf(
                "sn0.node%u.cpu%u.xtalk.nameless-bigwin",
                source_node->nasid, slice);

            memory_region_init_alias(nameless, OBJECT(s), nameless_name,
                                     &s->nodes[0]->hub.bwin, 0,
                                     memory_region_size(
                                         &s->nodes[0]->hub.bwin));
            memory_region_add_subregion(
                &source_node->cpu_sysmem[slice],
                map->xtalk.carrier_base + SN0_BIG_WINDOW_BASE,
                nameless);

            for (target = 0; target < s->num_nodes; target++) {
                SN0NodeState *target_node = s->nodes[target];
                MemoryRegion *route =
                    &s->big_window_route[base + 1 + target];
                g_autofree char *name = g_strdup_printf(
                    "sn0.node%u.cpu%u.xtalk.node%u.bigwin",
                    source_node->nasid, slice, target_node->nasid);

                memory_region_init_alias(route, OBJECT(s), name,
                                         &target_node->hub.bwin, 0,
                                         memory_region_size(
                                             &target_node->hub.bwin));
                memory_region_add_subregion(
                    &source_node->cpu_sysmem[slice],
                    sn0_model_node_address(map, &map->xtalk,
                                           target_node->nasid,
                                           SN0_BIG_WINDOW_BASE),
                    route);
            }
        }
    }
}

/*
 * -bios fills every node's boot flash unless a pflash drive already backs
 * it (set on the node before it realized); a short -bios image leaves the
 * rest erased, matching an unprogrammed chip. sn0_init() calls this after
 * every node has realized, so each node's flash device already exists.
 */
static void sn0_load_flash(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    const SN0ModelRegion *flash_region =
        &s->nodes[0]->topology->map.flash_reset;
    uint8_t *flash;
    unsigned int node_index;

    flash = g_malloc(flash_region->size);
    memset(flash, 0xff, flash_region->size);
    if (machine->firmware) {
        char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        ssize_t size = path ? load_image_size(path, flash,
                                              flash_region->size) : -1;

        if (size < 0) {
            error_report("%s: cannot read the firmware image '%s'",
                         MACHINE_GET_CLASS(machine)->name,
                         machine->firmware);
            exit(EXIT_FAILURE);
        }
        g_free(path);
    }

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        MemoryRegion *chip;

        if (node->flash_drive) {
            continue;
        }
        chip = sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
        memcpy(memory_region_get_ram_ptr(chip), flash, flash_region->size);
        memory_region_set_dirty(chip, 0, flash_region->size);
    }
    g_free(flash);
}

static void sn0_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
    const SN0MachineModel *model = smc->model;
    const SN0ModelProfile *profile;
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    unsigned int numa_nodes = machine->numa_state ?
                              machine->numa_state->num_nodes : 0;
    uint64_t ram_offset = 0;
    g_autofree unsigned int *profile_cpus = NULL;
    g_autofree BlockBackend **flash_drives = NULL;
    unsigned int flash_count = 0;
    unsigned int max_nodes = sn0_model_max_nodes(model);
    unsigned int node_count, node_index, cpu_index = 0;
    size_t link_index;

    /* A product class must supply its static description. */
    g_assert(model);
    g_assert(max_nodes > 0);
    g_assert(model->cpu_slots_per_node <= SN0_MAX_CPUS_PER_NODE);
    g_assert(model->memory.architectural_banks <= SN0_MAX_MEMORY_BANKS);
    profile_cpus = g_new0(unsigned int, max_nodes);

    if (machine->kernel_filename) {
        error_report("%s: stock-PROM boot only; boot a kernel through the "
                     "PROM's own TFTP netboot or a disk image, not -kernel",
                     mc->name);
        exit(EXIT_FAILURE);
    }

    /*
     * One SMP socket is one chassis; -numa still owns the memory split
     * and must agree with the socket count.
     */
    if (numa_nodes > max_nodes) {
        error_report("%s: at most %u NUMA nodes are supported",
                     mc->name, max_nodes);
        exit(EXIT_FAILURE);
    }
    node_count = numa_nodes ? numa_nodes : machine->smp.sockets;
    if (node_count > max_nodes) {
        error_report("%s: at most %u chassis are supported, and each -smp "
                     "socket is one chassis", mc->name, max_nodes);
        exit(EXIT_FAILURE);
    }

    if (node_count == 1) {
        if (machine->smp.sockets != 1 ||
            machine->smp.cores > model->cpu_slots_per_node ||
            machine->smp.threads != 1) {
            error_report("%s: one chassis requires one socket with at most "
                         "%u cores and one thread per core", mc->name,
                         model->cpu_slots_per_node);
            exit(EXIT_FAILURE);
        }
        profile_cpus[0] = machine->smp.cpus;
    } else if (machine->smp.cpus !=
                   node_count * model->cpu_slots_per_node ||
               machine->smp.sockets != node_count ||
               machine->smp.cores != model->cpu_slots_per_node ||
               machine->smp.threads != 1) {
        error_report("%s: %u chassis require %u CPUs as %u sockets with "
                     "%u cores and one thread each", mc->name, node_count,
                     node_count * model->cpu_slots_per_node, node_count,
                     model->cpu_slots_per_node);
        exit(EXIT_FAILURE);
    } else {
        for (node_index = 0; node_index < node_count; node_index++) {
            profile_cpus[node_index] = model->cpu_slots_per_node;
        }
    }

    profile = sn0_model_find_profile(model, node_count, profile_cpus);
    if (!profile) {
        error_report("%s: the requested CPU and chassis population is not "
                     "described by the machine model", mc->name);
        exit(EXIT_FAILURE);
    }
    /* Whole mebibytes per chassis: the bank fit is counted in them. */
    if (node_count > 1 && !numa_nodes &&
        machine->ram_size % ((uint64_t)node_count * MiB)) {
        error_report("%s: %u chassis split -m evenly into whole MiB; use "
                     "-numa for an uneven fit", mc->name, node_count);
        exit(EXIT_FAILURE);
    }

    sn0_possible_cpu_arch_ids(machine);
    if (numa_nodes) {
        for (node_index = 0; node_index < machine->smp.cpus; node_index++) {
            CPUArchId *cpu_slot = &machine->possible_cpus->cpus[node_index];
            unsigned int expected =
                sn0_get_default_cpu_node_id(machine, node_index);

            if (!cpu_slot->props.has_node_id ||
                cpu_slot->props.node_id != expected) {
                error_report("%s: CPU %u must be assigned to NUMA "
                             "node %u", mc->name, node_index, expected);
                exit(EXIT_FAILURE);
            }
        }
    }

    /* Fatal: silently missing codes look like firmware that wrote none. */
    if (s->debug_leds) {
        sn0_show_indicator_codes(&error_fatal);
    }

    /*
     * -bios fills every node's flash; a pflash drive at IF_PFLASH unit N
     * overrides node N's, as Origin 2000 already takes them. Without
     * either there is no firmware and stock PROM boot cannot start, so
     * this is fatal -- except under a register test, which deliberately
     * builds the machine with none of the vendor's bits.
     */
    flash_drives = g_new0(BlockBackend *, node_count);
    for (node_index = 0; node_index < node_count; node_index++) {
        DriveInfo *drive = drive_get_by_index(IF_PFLASH, node_index);

        if (drive) {
            flash_drives[node_index] = blk_by_legacy_dinfo(drive);
            flash_count++;
        }
    }
    if (!machine->firmware && !flash_count && !qtest_enabled()) {
        error_report("%s: supports stock-PROM boot only: pass -bios or a "
                     "pflash drive per node", mc->name);
        exit(EXIT_FAILURE);
    }

    s->num_nodes = node_count;
    s->nodes = g_new0(SN0NodeState *, node_count);
    s->io_bridge = g_new0(SGIBridgeState *, node_count);
    s->widget0 = g_new0(MemoryRegion, node_count);
    s->widget_alias = g_malloc0(sizeof(*s->widget_alias) * node_count);
    s->pci_devices = g_new0(PCIDevice **, node_count);

    for (node_index = 0; node_index < node_count; node_index++) {
        g_autofree char *name = g_strdup_printf("node%u", node_index);
        Object *node = object_new(TYPE_SGI_SN0_NODE);

        object_property_add_child(OBJECT(machine), name, node);
        object_unref(node);
        s->nodes[node_index] = SN0_NODE(node);
    }

    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, model->cpu_clock_hz);
    s->rt_refclk = clock_new(OBJECT(machine), "rt-refclk");
    clock_set_hz(s->rt_refclk, model->rtc_clock_hz);

    /*
     * The fabric that joins the Hubs, built from the profile before any
     * Hub realizes: every node's Hub is an endpoint at its board stride,
     * and the profile's peer link, the one cable a direct-wired pair has,
     * is the direct adapter's single link. The Hubs learn nothing of it
     * beyond their own endpoint.
     */
    s->fabric = SN0_FABRIC(qdev_new(TYPE_SN0_FABRIC_DIRECT));
    object_property_add_child(OBJECT(machine), "fabric", OBJECT(s->fabric));
    for (node_index = 0; node_index < node_count; node_index++) {
        const SN0ModelNodeInstance *node_model =
            &profile->nodes[node_index];
        unsigned int endpoint;

        endpoint = sn0_fabric_add_endpoint(s->fabric,
                                           &s->nodes[node_index]->hub,
                                           node_model->nasid);
        g_assert(endpoint == node_index);
    }
    for (link_index = 0; link_index < profile->link_count; link_index++) {
        const SN0ModelFabricLink *link = &profile->links[link_index];

        sn0_fabric_add_link(s->fabric, link->name, link->endpoint[0],
                            link->endpoint[1]);
    }
    qdev_realize_and_unref(DEVICE(s->fabric), NULL, &error_fatal);

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        const SN0ModelNodeInstance *node_model =
            &profile->nodes[node_index];
        uint64_t ram_size = numa_nodes ?
            machine->numa_state->nodes[node_index].node_mem :
            machine->ram_size / node_count;
        unsigned int cpu_base = node_model->cpu_first_id;
        unsigned int cpu_count = node_model->cpu_count;
        uint16_t bank_mb[SN0_MAX_MEMORY_BANKS];
        unsigned int populated_banks;
        QList *banks_mb;
        Chardev *aux_chr;
        unsigned int slice, bank;

        if (!sn0_model_memory_layout(model, ram_size, bank_mb,
                                     &populated_banks)) {
            error_report("%s: node %u's %" PRIu64 " MiB cannot be "
                         "populated from IP29 memory banks without "
                         "overlapping firmware regions",
                         mc->name, node_index, ram_size / MiB);
            exit(EXIT_FAILURE);
        }
        banks_mb = qlist_new();
        for (bank = 0; bank < populated_banks; bank++) {
            qlist_append_int(banks_mb, bank_mb[bank]);
        }

        qdev_prop_set_uint32(DEVICE(node), "nasid", node_model->nasid);
        node->flash_drive = flash_drives[node_index];
        node->model = model;
        node->instance = node_model;
        node->topology = node_model->topology;
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", cpu_base);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", cpu_count);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_bit(DEVICE(node), "promdata-filter", true);
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks_mb);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);
        qdev_connect_clock_in(DEVICE(node), "rt-refclk", s->rt_refclk);

        /*
         * The runtime Hub state the machine owns: its programmed node number
         * (real firmware programs it later, so a real-PROM boot starts at
         * zero), the chassis DIP switches, the fabric endpoint, the system
         * controller's external AUX backend, and the bring-up UART. Only the
         * master module drives the external AUX port. Keep the node-major
         * serial numbering, but leave a slave module's nominal AUX backend
         * unattached. The bring-up UART is a diagnostic device, not part of
         * any shipped chassis; keeping it on the model's console node avoids
         * character-backend collisions.
         */
        qdev_prop_set_uint32(DEVICE(&node->hub), "nasid",
                             s->synthesized_firmware ? node_model->nasid : 0);
        qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                            s->elsc_dip_switches);
        if (node_model->module_role == SN0_MODULE_MASTER) {
            aux_chr = serial_hd(SN0_NODE_SERIAL(node_index,
                                                SN0_ELSC_CHARDEV));
            if (aux_chr) {
                object_property_set_link(OBJECT(&node->hub), "elsc-chardev",
                                         OBJECT(aux_chr), &error_fatal);
            }
        }
        object_property_set_link(OBJECT(&node->hub), "fabric",
                                 OBJECT(s->fabric), &error_fatal);
        qdev_prop_set_uint32(DEVICE(&node->hub), "fabric-endpoint",
                             node_index);
        sn0_model_configure_junk_uart(s, node_index);

        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);

        for (slice = 0; slice < cpu_count; slice++) {
            machine->possible_cpus->cpus[cpu_index++].cpu =
                CPU(node->cpu[slice]);
        }
        ram_offset += ram_size;
    }
    g_assert(cpu_index == machine->smp.cpus);
    g_assert(ram_offset == machine->ram_size);

    sn0_map_cpu_big_windows(s);

    /* The two-module Origin 200 has four CPU turns and one slave turn. */
    if (node_count == 2) {
        SGIElscState *master = NULL;
        SGIElscState *slave = NULL;

        for (node_index = 0; node_index < node_count; node_index++) {
            if (profile->nodes[node_index].module_role == SN0_MODULE_MASTER) {
                master = s->nodes[node_index]->hub.elsc;
            } else if (profile->nodes[node_index].module_role ==
                       SN0_MODULE_SLAVE) {
                slave = s->nodes[node_index]->hub.elsc;
            }
        }
        g_assert(master);
        g_assert(slave);
        sgi_elsc_share_arbitration(master, slave, 5);
    }

    /* Every Hub has its endpoint and its aliases: the maps can be aimed. */
    sn0_fabric_build(s->fabric);

    for (node_index = 0; node_index < node_count; node_index++) {
        sn0_model_populate_node(s, node_index);
    }

    sn0_load_flash(machine);
}

static bool sn0_get_synthesized_firmware(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->synthesized_firmware;
}

static void sn0_set_synthesized_firmware(Object *obj, bool value, Error **errp)
{
    SN0MachineState *s = SN0_MACHINE(obj);

    if (value && s->composition) {
        error_setg(errp, "Origin 2000 requires supplied firmware");
        return;
    }
    s->synthesized_firmware = value;
}

static bool sn0_get_junk_uart(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->junk_uart_present;
}

static void sn0_set_junk_uart(Object *obj, bool value, Error **errp)
{
    SN0_MACHINE(obj)->junk_uart_present = value;
}

static bool sn0_get_debug_leds(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->debug_leds;
}

static void sn0_set_debug_leds(Object *obj, bool value, Error **errp)
{
    SN0_MACHINE(obj)->debug_leds = value;
}

static void sn0_get_elsc_dip_switches(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint8_t value = SN0_MACHINE(obj)->elsc_dip_switches;

    visit_type_uint8(v, name, &value, errp);
}

static void sn0_set_elsc_dip_switches(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        SN0_MACHINE(obj)->elsc_dip_switches = value;
    }
}

static void sn0_node_realize(DeviceState *dev, Error **errp)
{
    SN0NodeState *node = SN0_NODE(dev);
    unsigned int bank;

    g_assert(node->model || node->hardware);
    if (node->nr_cpus < 1 ||
        node->nr_cpus > sn0_node_cpu_slots(node)) {
        error_setg(errp, "a node carries one to %u processors, not %u",
                   sn0_node_cpu_slots(node), node->nr_cpus);
        return;
    }
    if (node->nr_banks_mb < 1 ||
        node->nr_banks_mb > sn0_node_memory_geometry(node)->physical_banks) {
        error_setg(errp, "a node fills one to %u memory banks, not %u",
                   sn0_node_memory_geometry(node)->physical_banks,
                   node->nr_banks_mb);
        return;
    }
    if (!node->ram || !node->cpu_type) {
        error_setg(errp, "the machine must set the node's ram and cpu-type");
        return;
    }

    node->populated_banks = node->nr_banks_mb;
    for (bank = 0; bank < node->nr_banks_mb; bank++) {
        node->bank_mb[bank] = node->banks_mb[bank];
    }

    sn0_node_create_cpus(node);
    sn0_node_map_memory(node);
    sn0_node_map_boot_storage(node);
    sn0_node_create_hub(node);
    sn0_node_map_xtalk_windows(node);
}

static void sn0_node_instance_init(Object *obj)
{
    SN0NodeState *node = SN0_NODE(obj);

    object_initialize_child(obj, "hub", &node->hub, TYPE_SGI_HUB);
    node->cpuclk = qdev_init_clock_in(DEVICE(obj), "cpu-refclk",
                                      NULL, NULL, 0);
    node->rtclk = qdev_init_clock_in(DEVICE(obj), "rt-refclk",
                                     NULL, NULL, 0);
}

static void sn0_node_unrealize(DeviceState *dev)
{
    SN0NodeState *node = SN0_NODE(dev);

    if (node->origin2000_xtalk_as_ready) {
        address_space_destroy(&node->origin2000_xtalk_as);
        node->origin2000_xtalk_as_ready = false;
    }
    sn_promdata_poll_free(&node->promdata_poll);
}

static const Property sn0_node_properties[] = {
    DEFINE_PROP_UINT32("nasid", SN0NodeState, nasid, 0),
    DEFINE_PROP_UINT32("cpu-base", SN0NodeState, cpu_base, 0),
    DEFINE_PROP_UINT32("nr-cpus", SN0NodeState, nr_cpus, 1),
    DEFINE_PROP_STRING("cpu-type", SN0NodeState, cpu_type),
    DEFINE_PROP_LINK("ram", SN0NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("ram-start", SN0NodeState, ram_start, 0),
    DEFINE_PROP_BOOL("promdata-filter", SN0NodeState, promdata_filter, true),
    DEFINE_PROP_BOOL("promdata-fast", SN0NodeState, promdata_fast, true),
    DEFINE_PROP_ARRAY("banks-mb", SN0NodeState, nr_banks_mb, banks_mb,
                      qdev_prop_uint16, uint16_t),
};

/*
 * A sysbus device, per the tree's device rule; the main system bus has
 * no get_dev_path, so the vmstate keeps its per-name instance ids. Reset
 * stays on the explicit handlers and their long-standing order for now.
 */
static void sn0_node_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sn0_node_realize;
    dc->unrealize = sn0_node_unrealize;
    dc->vmsd = &vmstate_sn0_flash;
    device_class_set_props(dc, sn0_node_properties);
    /* The machine wires a node's RAM, clock and Hub; -device cannot. */
    dc->user_creatable = false;
}

static void sn0_instance_init(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);

    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(obj);

    g_assert(smc->model || smc->default_composition);
    s->composition = smc->default_composition;
    s->synthesized_firmware = smc->model != NULL;
    s->debug_leds = true;
    s->junk_uart_present = smc->model && smc->model->junk_uart_default_enabled;
}

static void sn0_instance_finalize(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);
    unsigned int node_index;

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        if (s->pci_devices) {
            g_free(s->pci_devices[node_index]);
        }
    }
    g_free(s->nodes);
    g_free(s->io_bridge);
    g_free(s->pci_devices);
    /*
     * The widget arrays hold MemoryRegions owned as QOM children,
     * released after instance_finalize; freeing them here would leave
     * the release walking freed memory.
     */
}

static void sn0_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->reset = sn0_machine_reset;

    mc->init = sn0_init;
    mc->possible_cpu_arch_ids = sn0_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = sn0_cpu_index_to_props;
    mc->get_default_cpu_node_id = sn0_get_default_cpu_node_id;

    object_class_property_add_bool(oc, "synthesized-firmware",
                                   sn0_get_synthesized_firmware,
                                   sn0_set_synthesized_firmware);
    object_class_property_set_description(oc, "synthesized-firmware",
        "Present firmware of the machine's own: the structures in low memory, "
        "the call vector and the routines behind it. Turn it off to run a real "
        "firmware image with nothing of ours in its way.");

    object_class_property_add_bool(oc, "junk-uart",
                                   sn0_get_junk_uart, sn0_set_junk_uart);
    object_class_property_set_description(oc, "junk-uart",
        "Fit the junk-bus UART, a real NS16550-compatible serial interface "
        "in the Hub's own register space that a shipped Origin 200 does not "
        "have -- SGI fits one only for manufacturing and bring-up. Off by "
        "default: real firmware treats this port as its highest-priority "
        "console candidate and stops probing at the first one that answers, "
        "so fitting it unasked would hide the real candidates behind "
        "it. On, it takes the fourth character device (serial_hd index 3); "
        "the first three stay the base I/O controller's two ports and the "
        "system controller's AUX port.");

    object_class_property_add_bool(oc, "debug-leds",
                                   sn0_get_debug_leds, sn0_set_debug_leds);
    object_class_property_set_description(oc, "debug-leds",
        "Show the firmware's boot-progress indicator, decoded. Real firmware "
        "writes a one-byte code to the Hub before each stage of power-on "
        "self-test, which on a real Origin 200 drives the front-panel LEDs "
        "and is the only progress this machine reports while the firmware is "
        "still deciding which console to talk to -- so it is on by default, "
        "and turning it off buys a quieter stderr and nothing else. The "
        "codes go to the log destination, meaning stderr unless -D names a "
        "file.");

    object_class_property_add(oc, "elsc-dip-switches", "uint8",
                              sn0_get_elsc_dip_switches,
                              sn0_set_elsc_dip_switches, NULL, NULL);
    object_class_property_set_description(oc, "elsc-dip-switches",
        "Set the eight physical ELSC DIP switches as asserted bits. The "
        "controller reports their active-low wire value to firmware.");

}

static const TypeInfo sn0_machine_types[] = {
    {
        .name              = TYPE_SN0_MACHINE,
        .parent            = TYPE_MACHINE,
        .abstract          = true,
        .instance_size     = sizeof(SN0MachineState),
        .instance_init     = sn0_instance_init,
        .instance_finalize = sn0_instance_finalize,
        .class_size        = sizeof(SN0MachineClass),
        .class_init        = sn0_machine_class_init,
    },
    {
        .name              = TYPE_SGI_SN0_NODE,
        .parent            = TYPE_SYS_BUS_DEVICE,
        .instance_size     = sizeof(SN0NodeState),
        .instance_init     = sn0_node_instance_init,
        .class_init        = sn0_node_class_init,
    },
};

DEFINE_TYPES(sn0_machine_types)
