/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 (IP27) family mechanics and node device. Origin 200 supplies a
 * compiled topology to the generic model consumer. -bios and pflash fill a
 * node's boot flash here.
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
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/xtalk-link.h"
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
 * The PROM's second execution home during the cache-as-RAM phase: a RAM copy
 * of its text, reached by an ASID re-point rather than a PC change. See
 * sn-promdata.c for the mapping.
 */
#define SN0_PROM_COPY_PADDR 0x01a00000

/* MIPS CPU input 0 is IP0. Hardware interrupt inputs begin at IP2. */
#define SN0_MIPS_HARDWARE_IRQ_OFFSET 2

/* The Hub's three CPU interrupt inputs: hardwired, not configuration. */
#define SN0_HUB_CPU_IRQ_BANK0 0
#define SN0_HUB_CPU_IRQ_BANK1 1
#define SN0_HUB_CPU_IRQ_RT    2

/* Every node carries two processor-interface slots. */
#define SN0_HUB_CPU_SLOTS_PER_NODE 2

const SN0FlashImplementation sn0_flash = {
    .sector_size = 0x10000,
    .width = 1,
    .mappings = 1,
    .big_endian = true,
    .manufacturer_id = 0x01,
    .device_id = 0xd5,
    .unlock_address = { 0x5555, 0x2aaa },
};

/* The Hub's flash controller offers this view whenever the board wires it. */
static const SN0ModelRegion sn0_hub_lboot = {
    .offset = 0x10000000ULL,
    .size = 0x10000000ULL,
};

/* The Hub's physical address map: identical on every SN0 node measured. */
const SN0ModelAddressMap sn0_hub_address_map = {
    .node_stride = 0x100000000ULL,
    .memory = {
        .selector = 3,
        .carrier_base = 0x0ULL,
        .unmapped_base = 0x9600000000000000ULL,
    },
    .hub_special = {
        .selector = 0,
        .carrier_base = 0x10000000000ULL,
        .unmapped_base = 0x9000000000000000ULL,
    },
    .xtalk = {
        .selector = 1,
        .carrier_base = 0x20000000000ULL,
        .unmapped_base = 0x9200000000000000ULL,
    },
    .memory_special = {
        .selector = 2,
        .carrier_base = 0x30000000000ULL,
        .unmapped_base = 0x9400000000000000ULL,
    },
    .node_memory = { .offset = 0x0ULL, .size = 0x100000000ULL },
    .hub_registers_local = { .offset = 0x1000000ULL, .size = 0x800000ULL },
    .hub_registers_node = { .offset = 0x1800000ULL, .size = 0x800000ULL },
    .hub_local_memory = { .offset = 0x0ULL, .size = 0x10000000ULL },
    .hub_remote_memory = { .offset = 0x20000000ULL, .size = 0x10000000ULL },
    .backdoor_ecc = { .offset = 0x80000000ULL, .size = 0x40000000ULL },
    .backdoor_directory = { .offset = 0xc0000000ULL, .size = 0x40000000ULL },
    .flash_reset = { .offset = 0x1fc00000ULL, .size = 0x100000ULL },
    .lboot = &sn0_hub_lboot,
    .rboot = { .offset = 0x30000000ULL, .size = 0x10000000ULL },
    .bridge_registers = { .offset = 0x8000000ULL, .size = 0x1000000ULL },
};

/*
 * The Hub's bank decode is the same on every SN0 node; which banks a board
 * wires and which sizes its DIMMs make is the board's. This is the Origin 200
 * (IP29) board: four banks.
 */
static const uint64_t sn0_hub_memory_sizes_mib[] = {
    0x200ULL, 0x100ULL, 0x80ULL, 0x40ULL,
};

const SN0ModelMemory sn0_hub_memory = {
    .board = "IP29",
    .physical_banks = 4,
    .architectural_banks = 8,
    .supported_mib = sn0_hub_memory_sizes_mib,
    .supported_size_count = ARRAY_SIZE(sn0_hub_memory_sizes_mib),
    .first_bank_max_mib = 0x100ULL,
    .bank_stride_mib = 0x200ULL,
    .interleave_slot_stride_mib = 0x80ULL,
    .interleave_slots_per_bank = 4,
};

/*
 * The Origin 2000 node board (IP27) wires eight banks, each a DIMM pair.
 * 16-Mb parts make 64 or 128 MB banks, and 64-Mb parts make 512 MB banks
 * (SGI 007-3439-002, page 35). No part makes a 256 MB bank.
 * The stock IP27 PROM disables one ("256 MB dimm banks not allowed on IP27
 * node boards").
 * The bank-zero limit is the Origin 200's, carried over unestablished for
 * this board; no accepted Origin 2000 total reaches it.
 */
static const uint64_t sn0_ip27_memory_sizes_mib[] = {
    0x200ULL, 0x80ULL, 0x40ULL,
};

const SN0ModelMemory sn0_ip27_memory = {
    .board = "IP27",
    .physical_banks = 8,
    .architectural_banks = 8,
    .supported_mib = sn0_ip27_memory_sizes_mib,
    .supported_size_count = ARRAY_SIZE(sn0_ip27_memory_sizes_mib),
    .first_bank_max_mib = 0x100ULL,
    .bank_stride_mib = 0x200ULL,
    .interleave_slot_stride_mib = 0x80ULL,
    .interleave_slots_per_bank = 4,
};

/* Generic consumers for a node's address map, shared by every SN0 product. */

static uint64_t sn0_model_node_offset(const SN0ModelAddressMap *map,
                                      unsigned int nasid)
{
    uint64_t offset;

    g_assert(!umul64_overflow(map->node_stride, nasid, &offset));
    return offset;
}

uint64_t sn0_model_node_address(const SN0ModelAddressMap *map,
                                const SN0ModelAddressSpace *space,
                                unsigned int nasid, uint64_t offset)
{
    uint64_t address;

    g_assert(!uadd64_overflow(space->carrier_base,
                             sn0_model_node_offset(map, nasid), &address));
    g_assert(!uadd64_overflow(address, offset, &address));
    return address;
}

uint64_t sn0_model_region_address(const SN0ModelAddressMap *map,
                                  const SN0ModelAddressSpace *space,
                                  unsigned int nasid,
                                  const SN0ModelRegion *region,
                                  uint64_t offset)
{
    uint64_t region_offset;

    g_assert(offset < region->size);
    g_assert(!uadd64_overflow(region->offset, offset, &region_offset));
    return sn0_model_node_address(map, space, nasid, region_offset);
}

static bool sn0_model_layout_from(const SN0ModelMemory *memory,
                                  uint64_t remaining_mb, unsigned int bank,
                                  uint16_t *bank_mb)
{
    unsigned int size_index;

    if (remaining_mb == 0) {
        return bank > 0;
    }
    if (bank == memory->physical_banks) {
        return false;
    }

    for (size_index = 0; size_index < memory->supported_size_count;
         size_index++) {
        uint64_t size_mb = memory->supported_mib[size_index];

        if ((bank == 0 && size_mb > memory->first_bank_max_mib) ||
            size_mb > remaining_mb) {
            continue;
        }
        bank_mb[bank] = size_mb;
        if (sn0_model_layout_from(memory, remaining_mb - size_mb, bank + 1,
                                  bank_mb)) {
            return true;
        }
        bank_mb[bank] = 0;
    }
    return false;
}

bool sn0_model_memory_layout(const SN0ModelMemory *memory, uint64_t ram_size,
                             uint16_t *bank_mb,
                             unsigned int *populated_banks)
{
    unsigned int bank;

    memset(bank_mb, 0, sizeof(*bank_mb) * memory->architectural_banks);
    if (ram_size == 0 || ram_size % MiB != 0 ||
        !sn0_model_layout_from(memory, ram_size / MiB, 0, bank_mb)) {
        return false;
    }

    for (bank = 0; bank < memory->physical_banks; bank++) {
        if (bank_mb[bank] == 0) {
            break;
        }
    }
    *populated_banks = bank;
    return true;
}

/*
 * The per-CPU cache-as-RAM private window (physical 0x10_0000, keyed on the
 * PROM boot-status flag and the PC being in the PROM) is sn-promdata.c's
 * SNPromDataState and its functions, shared with sgi-sn1-node, sharing the
 * same PROM cache-as-RAM phase and boot-status-bit contract. This file only
 * computes the bounds sn0_node_create_cpus passes it, the peer write counter
 * its trace line reports below, and the array-level migration wrapper.
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
 * shared with sgi-sn1-node; this array-level wrapper is per node type because
 * it names SN0NodeState's promdata[] field. Its name stays
 * "sn0-flash/promdata" for wire compatibility with streams saved before the
 * node device carried a shared promdata implementation.
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
 * Named "sn0-flash" for wire compatibility: the pflash device migrates its
 * state and the back doors migrate with their RAM blocks, so this carries
 * only promdata. Version 4 drops the back-door buffers and refuses an older
 * stream rather than translating one.
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
 * The Hub's flash controller: the decode between the 64-bit junk bus and the
 * byte-wide chip. Reads pack the chip 1:1 and repeat its image across each
 * decoded window; command writes present the byte-wide command port on 64-bit
 * cycles only, chip byte N at bus offset 8N, the mapping the vendor flash
 * programmer states directly.
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
 * The floor beneath a back-door window: firmware's presence probe writes a
 * pattern, then zero, and calls a bank absent when neither survives, so
 * accept both writes and echo neither. 0xa5 is an arbitrary choice; the real
 * data lines float.
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
 * bytes of each eight-byte entry, the rest reading zero; plain RAM would
 * answer firmware's width probe "premium format". Writes drop the dead bytes
 * so a read is a plain load.
 */
#define SN0_BACKDOOR_ENTRY_BYTES 8
#define SN0_BACKDOOR_LIVE_BYTES  2

/*
 * The aliases below fold the window, so an offset the memory core hands a
 * handler is already an index into the storage. Naturally aligned, so the
 * access lies inside one entry.
 */
static uint8_t *sn0_backdoor_entry(SN0BackdoorState *s, hwaddr addr,
                                   unsigned size)
{
    g_assert(QEMU_IS_ALIGNED(addr, size));
    g_assert(addr + size <= s->data_size);
    return s->data + addr;
}

/*
 * The bytes of this access that are stored: the tail of the entry it lies in,
 * which is empty when the access ends before the live bytes.
 */
static uint64_t sn0_backdoor_live_mask(hwaddr addr, unsigned size)
{
    unsigned int dead = SN0_BACKDOOR_ENTRY_BYTES - SN0_BACKDOOR_LIVE_BYTES;
    unsigned int offset = addr & (SN0_BACKDOOR_ENTRY_BYTES - 1);
    unsigned int live = offset >= dead ? size : size - MIN(size, dead - offset);

    return live ? MAKE_64BIT_MASK(0, live * 8) : 0;
}

/*
 * The offset and the storage index are now one number: the aliases fold the
 * window, so the window offset the guest used is gone by the time a handler
 * runs. Only a write reaches one.
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
 * Only a path that cannot take a host pointer arrives here: the monitor, the
 * debugger, a qtest. A processor's load is served from the storage by the
 * translation fast path and never enters the memory core.
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
 * One window's storage, as a ROM device so a processor's read comes out of it
 * with no dispatch and no lock while a write still traps here. The window
 * repeats each bank's storage across the bank stride, and an alias per repeat
 * leaves that fold to the memory core.
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

static const SN0ModelAddressMap *sn0_node_address_map(void)
{
    return &sn0_hub_address_map;
}

static const SN0ModelMemory *sn0_node_memory_geometry(SN0NodeState *node)
{
    return node->memory ? node->memory : &sn0_hub_memory;
}

static unsigned int sn0_node_cpu_slots(void)
{
    return SN0_HUB_CPU_SLOTS_PER_NODE;
}

static void sn0_node_create_cpus(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelAddressMap *map = sn0_node_address_map();
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
                              SN0_MAX_CPUS_PER_NODE,
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
         * The Hub's claim layer fills this container: the existing nodes'
         * node-qualified strides route by programmed number, above the static
         * placement, below the nameless local windows.
         */
        memory_region_init(&node->claim_view[slice], OBJECT(node),
                           claim_name, UINT64_MAX);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[slice], 0,
                                            &node->claim_view[slice], -1);

        /*
         * NASID-less addresses name the reading processor's node; in the
         * memory space only within the cached-alias bound, beyond which, like
         * every explicit NASID, they fall to the Hub's claim layer
         * underneath.
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
             * Per space, the public alias table (Linux asm/sn/addrs.h and
             * sn0/addrs.h): the crosstalk space self-decodes the IALIAS, the
             * Hub's eight-megabyte register window, always. The memory
             * space's nameless window is the CALIAS: "the first n bytes of
             * memory on the reader's node", based at the cached base,
             * pre-created here at the largest size the register encodes, then
             * resized and controlled by this node's Hub, which gets the
             * region at wiring time. Below the bound an access stays local
             * even while the programmed numbers are in motion; retiring the
             * window outright put mid-renumbering local accesses into claim
             * transients and broke the two-node firmware boot. The
             * Hub-special space is the UALIAS, uncached references to the
             * memory of the reader's node, position-independent by definition
             * and register-independent;
             * the memory-special space and the LBOOT path are likewise local.
             * Those keep the wide reader-local window.
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
    const SN0ModelAddressMap *map = sn0_node_address_map();
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
     * A bank no larger than one interleave slot occupies only its first slot.
     * Larger banks are split evenly over every slot in the bank.
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
             * The Hub-local region mirrors the bottom of local memory; mirror
             * each run separately to keep the slot holes.
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
    const SN0ModelAddressMap *map = sn0_node_address_map();
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
     * Without a backing drive the firmware loader seeds this byte-wide flash
     * after realize. Every view uses the same flash device.
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

/*
 * What the node slot's midplane drives on the Hub's slot-identity pins.
 * The catalogue names it where a midplane differs from the eight-processor
 * module, whose slots 1 to 4 drive 7 to 4 (the Onyx2 deskside's drive 3
 * and 2). Origin 200's chassis numbers no node slot and drives nothing.
 */
static uint8_t sn0_node_hub_slot_bits(const SGIModelNode *model_node)
{
    if (model_node->has_hub_slot_bits) {
        return model_node->hub_slot_bits;
    }
    return model_node->slot ? 8 - model_node->slot
                            : SGI_HUB_SLOT_BITS_UNDRIVEN;
}

static void sn0_node_create_hub(SN0NodeState *node)
{
    const SGIModelAsicIdentity *identity = node->model_node->identity;
    const SN0ModelAddressMap *map = sn0_node_address_map();
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
     * The Hub's engines address memory as this node's processors do, minus
     * the per-processor pieces; a device has no slice to flip and no nameless
     * window. The container is the machine's; the Hub fills it with its claim
     * layer, which routes every node-qualified address by programmed number,
     * and an engine sees nothing else.
     */
    memory_region_init(&node->device_view, OBJECT(node), device_view_name,
                       UINT64_MAX);
    object_property_set_link(OBJECT(&node->hub), "claim-device-view",
                             OBJECT(&node->device_view), &error_fatal);

    qdev_prop_set_uint32(DEVICE(&node->hub), "cpus", node->nr_cpus);
    qdev_prop_set_uint32(DEVICE(&node->hub), "cpu-base", node->cpu_base);
    qdev_prop_set_uint8(DEVICE(&node->hub), "slot-bits",
                        sn0_node_hub_slot_bits(node->model_node));
    qdev_prop_set_bit(DEVICE(&node->hub), "slave-module", node->slave_role);
    qdev_prop_set_uint8(DEVICE(&node->hub), "ni-revision",
                        identity->ni_revision);
    qdev_prop_set_uint8(DEVICE(&node->hub), "widget-revision",
                        identity->widget_revision);
    /*
     * Each PIMM record part gets its synthetic serial: per-unit, as the Hub's
     * one-wire bus requires, never a real machine's.
     */
    qdev_prop_set_uint64(DEVICE(&node->hub), "onewire-serial",
                         identity->serial);
    qdev_prop_set_uint32(
        DEVICE(&node->hub), "population",
        (identity->i2c_controller ?
         SGI_HUB_POPULATION_I2C_CONTROLLER : 0) |
        (identity->system_controller ? SGI_HUB_POPULATION_ELSC : 0));
    /*
     * The ELSC's address answers only where this Hub's I2C bus carries one
     * (identity->system_controller); the NVRAM behind it answers wherever the
     * Hub carries an I2C controller at all (identity->i2c_controller);
     * Origin 2000's Hub has one without an embedded ELSC. Both are 0 (unread:
     * sgi-hub.c guards each with its population bit) where the fact does not
     * apply.
     */
    qdev_prop_set_uint32(DEVICE(&node->hub), "i2c-nvram-pages",
                         identity->i2c_controller ?
                         SN0_HUB_I2C_NVRAM_PAGES : 0);
    qdev_prop_set_uint64(DEVICE(&node->hub), "i2c-controller-offset",
                         SN0_HUB_MODULE_ADAPTER_OFFSET);
    qdev_prop_set_uint8(DEVICE(&node->hub), "i2c-elsc-addr",
                        identity->system_controller ?
                        SN0_HUB_I2C_ELSC_ADDR : 0);
    qdev_prop_set_uint8(DEVICE(&node->hub), "i2c-nvram-first-addr",
                        identity->i2c_controller ?
                        SN0_HUB_I2C_NVRAM_FIRST_ADDR : 0);
    qdev_prop_set_uint8(DEVICE(&node->hub), "onewire-kind",
                        SGI_HUB_ONEWIRE_BLANK_DS2505);
    qdev_prop_set_uint32(DEVICE(&node->hub), "xtalk-widget-shift",
                         SN0_XTALK_WIDGET_SHIFT);
    object_property_set_link(OBJECT(&node->hub), "xtalk",
                             OBJECT(&node->xtalk), &error_fatal);
    qdev_prop_set_uint64(DEVICE(&node->hub), "xtalk-carrier-base",
                         map->xtalk.carrier_base);

    /*
     * The claim layer's wiring: the per-view containers the Hub fills, the
     * canonical map its aliases resolve into, and the geometry; the Hub gets
     * offsets to alias by, never the node's regions.
     */
    for (slice = 0; slice < node->nr_cpus; slice++) {
        g_autofree char *view_prop = g_strdup_printf("claim-view%u", slice);
        object_property_set_link(OBJECT(&node->hub), view_prop,
                                 OBJECT(&node->claim_view[slice]),
                                 &error_fatal);

        /*
         * The one nameless window the cached-alias size bounds: the memory
         * space's, per view. The other spaces' windows are
         * position-independent aliases the register never moves.
         */
        g_autofree char *window_prop =
            g_strdup_printf("calias-window%u", slice);

        object_property_set_link(
            OBJECT(&node->hub), window_prop,
            OBJECT(&node->local_space[slice][0]), &error_fatal);

        /*
         * This processor's physical view, for the Origin 2000 bus-error
         * aperture the Hub installs when it finds itself Crossbow-attached.
         */
        g_autofree char *sysmem_prop = g_strdup_printf("cpu-sysmem%u", slice);

        object_property_set_link(OBJECT(&node->hub), sysmem_prop,
                                 OBJECT(&node->cpu_sysmem[slice]),
                                 &error_fatal);
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
            cpu->env.irq[SN0_HUB_CPU_IRQ_BANK0 +
                         SN0_MIPS_HARDWARE_IRQ_OFFSET]);
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "bank1", slice,
            cpu->env.irq[SN0_HUB_CPU_IRQ_BANK1 +
                         SN0_MIPS_HARDWARE_IRQ_OFFSET]);
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "rt", slice,
            cpu->env.irq[SN0_HUB_CPU_IRQ_RT +
                         SN0_MIPS_HARDWARE_IRQ_OFFSET]);
    }
}

static void sn0_node_map_xtalk_windows(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelAddressMap *map = sn0_node_address_map();
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

void sn0_map_cpu_big_windows(Object *owner, SN0NodeState **nodes,
                             unsigned int num_nodes, MemoryRegion **route_out)
{
    unsigned int source, slice, target;
    MemoryRegion *big_window_route = g_new0(
        MemoryRegion, num_nodes * SN0_MAX_CPUS_PER_NODE * (num_nodes + 1));

    *route_out = big_window_route;
    for (source = 0; source < num_nodes; source++) {
        SN0NodeState *source_node = nodes[source];
        const SN0ModelAddressMap *map = sn0_node_address_map();

        for (slice = 0; slice < source_node->nr_cpus; slice++) {
            unsigned int base =
                (source * SN0_MAX_CPUS_PER_NODE + slice) *
                (num_nodes + 1);
            MemoryRegion *nameless = &big_window_route[base];
            g_autofree char *nameless_name = g_strdup_printf(
                "sn0.node%u.cpu%u.xtalk.nameless-bigwin",
                source_node->nasid, slice);

            memory_region_init_alias(nameless, owner, nameless_name,
                                     &nodes[0]->hub.bwin, 0,
                                     memory_region_size(
                                         &nodes[0]->hub.bwin));
            memory_region_add_subregion(
                &source_node->cpu_sysmem[slice],
                map->xtalk.carrier_base + SN0_BIG_WINDOW_BASE,
                nameless);

            for (target = 0; target < num_nodes; target++) {
                SN0NodeState *target_node = nodes[target];
                MemoryRegion *route =
                    &big_window_route[base + 1 + target];
                g_autofree char *name = g_strdup_printf(
                    "sn0.node%u.cpu%u.xtalk.node%u.bigwin",
                    source_node->nasid, slice, target_node->nasid);

                memory_region_init_alias(route, owner, name,
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
 * The PROM's fixed target: widget 2 (a synthetic address, not a populated
 * widget) at the Bridge's device-window offset for its error-status decode.
 * Origin 2000's Crossbow-attached BaseIO card does not use this probe at all;
 * only IP29's onboard I/O, wired through the direct link, does.
 */
#define SN0_PROM_PI_PROBE_WIDGET 2
#define SN0_PROM_PI_PROBE_OFFSET 0x400208ULL

/*
 * IP29's onboard Bridge is reachable at crosstalk widget 8 on its first
 * discovery and decodes every other widget position the same way (a directly
 * linked Hub's Bridge answers its whole face at every widget the Hub decodes
 * to it) except widget 1; fixed by the chip and the System Board's wiring,
 * the same on every Origin 200 built, so this is a device constant rather
 * than per-topology data; the compiled catalogue no longer carries it.
 */
#define SN0_IP29_IO_DISCOVERY_WIDGET   0
#define SN0_IP29_IO_DECODED_WIDGET_MASK 0xfffdU

/*
 * IP29's onboard base I/O: the System Board's Bridge, cabled straight to this
 * node's Hub through the direct link (no Crossbow), and an sgi-baseio behind
 * it with the "ip29" preset. Unlike the Origin 2000 BaseIO card
 * (sgi-xio-baseio), this Bridge is not wrapped in any board class: it is a
 * fitted part of the System Board itself, not a removable card, so it answers
 * the crosstalk port interface directly (hw/pci-host/sgi-bridge.c) rather
 * than through a board's re-parenting and module/slot registry. This is the
 * physical difference between an Origin 200's integrated IP29 System Board
 * and an Origin 2000 BaseIO card.
 *
 * Called once node has realized (its Hub's claim view and crosstalk container
 * exist by then), passing every fitted-part value and backend id the family
 * builder (sn-machine.c) already resolved from the descriptor and the command
 * line; this file owns only the Hub address-map facts (fabric-base,
 * fabric-node-shift, the PROM probe's fixed widget and offset) and the
 * direct-link wiring itself.
 *
 * A GIGAchannel cabinet takes the Hub's link instead and cables this Bridge
 * to one of the cabinet Crossbow's ports. Then crossbow_fabric is that
 * Crossbow link's proxy, the Bridge addresses its packets by widget as every
 * Crossbow-attached Bridge does, no direct link is wired, and the caller
 * attaches the Bridge it gets back through bridge_out to the Crossbow.
 */
bool sn0_node_build_baseio(SN0NodeState *node, Object *owner,
                           SGIXtalkDirectLink *link,
                           MemoryRegion *crossbow_fabric,
                           uint64_t bridge_serial,
                           const uint64_t *ioc3_serial, const char *pci_bus_name,
                           const char *nvram_drive_id,
                           const char *chardev_a_id,
                           const char *chardev_b_id,
                           const char *record_a_id,
                           const char *record_b_id,
                           const char *parallel_chardev_id,
                           unsigned int widget, DeviceState **bridge_out,
                           Error **errp)
{
    const SN0ModelAddressMap *map = sn0_node_address_map();
    const SGIBaseioPresetIdentity *identity = sgi_baseio_preset_lookup("ip29");
    DeviceState *bridge, *baseio;
    PCIBus *bus;

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(node), "bridge", OBJECT(bridge));
    sgi_baseio_preset_set_bridge_identity(bridge, identity);
    qdev_prop_set_uint64(bridge, "onewire-serial", bridge_serial);
    if (pci_bus_name) {
        qdev_prop_set_string(bridge, "pci-bus-name", pci_bus_name);
    }
    if (crossbow_fabric) {
        object_property_set_link(OBJECT(bridge), "fabric",
                                 OBJECT(crossbow_fabric), &error_abort);
        qdev_prop_set_uint32(bridge, "fabric-widget-shift",
                             SGI_BRIDGE_PACKET_ADDRESS_BITS);
    } else {
        /*
         * Interrupt packets travel a processor's claim view, which resolves a
         * node-qualified address by programmed number, not by the board's
         * position. View zero speaks for the node; a node always has a
         * processor.
         */
        object_property_set_link(OBJECT(bridge), "fabric",
                                 OBJECT(&node->claim_view[0]), &error_abort);
        qdev_prop_set_uint64(bridge, "fabric-base", map->xtalk.carrier_base);
        qdev_prop_set_uint32(bridge, "fabric-node-shift",
                             ctz64(map->node_stride));
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    bus = PCI_HOST_BRIDGE(bridge)->bus;

    baseio = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(OBJECT(node), "baseio", OBJECT(baseio));
    qdev_prop_set_string(baseio, "preset", "ip29");
    object_property_set_link(OBJECT(baseio), "pci-bus", OBJECT(bus),
                             &error_abort);
    if (ioc3_serial) {
        qdev_prop_set_uint64(baseio, "onewire-serial", *ioc3_serial);
    }
    if (nvram_drive_id) {
        qdev_prop_set_string(baseio, "nvram-drive", nvram_drive_id);
    }
    if (chardev_a_id) {
        qdev_prop_set_string(baseio, "chardev-a", chardev_a_id);
    }
    if (chardev_b_id) {
        qdev_prop_set_string(baseio, "chardev-b", chardev_b_id);
    }
    if (record_a_id) {
        qdev_prop_set_string(baseio, "record-a", record_a_id);
    }
    if (record_b_id) {
        qdev_prop_set_string(baseio, "record-b", record_b_id);
    }
    if (parallel_chardev_id) {
        qdev_prop_set_string(baseio, "parallel-chardev", parallel_chardev_id);
    }
    if (!qdev_realize(DEVICE(baseio), NULL, errp)) {
        return false;
    }

    *bridge_out = bridge;
    if (crossbow_fabric) {
        return true;
    }
    return sgi_xtalk_direct_link_connect(
        link, owner, node, OBJECT(bridge), SN0_IP29_IO_DISCOVERY_WIDGET,
        widget, SN0_IP29_IO_DECODED_WIDGET_MASK, SN0_PROM_PI_PROBE_WIDGET,
        SN0_PROM_PI_PROBE_OFFSET, errp);
}

static void sn0_node_realize(DeviceState *dev, Error **errp)
{
    SN0NodeState *node = SN0_NODE(dev);
    unsigned int bank;

    g_assert(node->model_node);
    if (node->nr_cpus < 1 || node->nr_cpus > sn0_node_cpu_slots()) {
        error_setg(errp, "a node carries one to %u processors, not %u",
                   sn0_node_cpu_slots(), node->nr_cpus);
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

    sn_promdata_poll_free(&node->promdata_poll);
}

static const Property sn0_node_properties[] = {
    DEFINE_PROP_UINT32("nasid", SN0NodeState, nasid, 0),
    DEFINE_PROP_UINT32("cpu-base", SN0NodeState, cpu_base, 0),
    DEFINE_PROP_UINT32("nr-cpus", SN0NodeState, nr_cpus, 1),
    DEFINE_PROP_STRING("cpu-type", SN0NodeState, cpu_type),
    DEFINE_PROP_BOOL("slave-role", SN0NodeState, slave_role, false),
    DEFINE_PROP_LINK("ram", SN0NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("ram-start", SN0NodeState, ram_start, 0),
    DEFINE_PROP_BOOL("promdata-filter", SN0NodeState, promdata_filter, true),
    DEFINE_PROP_BOOL("promdata-fast", SN0NodeState, promdata_fast, true),
    DEFINE_PROP_ARRAY("banks-mb", SN0NodeState, nr_banks_mb, banks_mb,
                      qdev_prop_uint16, uint16_t),
};

/*
 * A sysbus device, per the tree's device rule; the main system bus has no
 * get_dev_path, so the vmstate keeps its per-name instance ids. Reset stays
 * on the explicit handlers and their long-standing order for now.
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

static const TypeInfo sn0_machine_types[] = {
    {
        .name              = TYPE_SGI_SN0_NODE,
        .parent            = TYPE_SYS_BUS_DEVICE,
        .instance_size     = sizeof(SN0NodeState),
        .instance_init     = sn0_node_instance_init,
        .class_init        = sn0_node_class_init,
    },
};

DEFINE_TYPES(sn0_machine_types)
