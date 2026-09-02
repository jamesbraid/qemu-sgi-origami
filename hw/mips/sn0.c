/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) family machines: the abstract machine
 * and the node device. Products parameterize them through SN0MachineClass
 * (sn0-origin200.c); firmware in sn0-firmware.c, diagnostics in sn0-diag.c.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "system/system.h"
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
#include "cpu.h"
#include "sn0.h"

#define SN0_HUB_PRIVATE_SIZE (128 * KiB)
#define SN0_HUB_PRIVATE_HALF (SN0_HUB_PRIVATE_SIZE / 2)
#define SN0_PROMDATA_PADDR  0x100000
#define SN0_PROMDATA_SIZE   (32 * KiB)
#define SN0_PROM_COPY_PADDR 0x01a00000
/*
 * Not a hardware register: the IP27 PROM's own software convention of
 * boot-status flags in FPR 0. Bit 1 marks the cache-as-RAM phase.
 */
#define SN0_PROM_BOOT_STATUS_CACHE_AS_RAM    (1U << 1)

#define TYPE_SN0_NODE "sn0-node"

DECLARE_INSTANCE_CHECKER(SN0NodeState, SN0_NODE, TYPE_SN0_NODE)

static const SN0ModelAddressSpace *sn0_space(const SN0MachineModel *model,
                                             SN0ModelAddressSpaceKind kind)
{
    const SN0ModelAddressSpace *space = sn0_model_find_space(model, kind);

    g_assert(space);
    return space;
}

static hwaddr sn0_space_node_base(const SN0MachineModel *model,
                                  SN0ModelAddressSpaceKind kind,
                                  unsigned int nasid)
{
    const SN0ModelAddressSpace *space = sn0_space(model, kind);

    return sn0_model_node_address(model, space, nasid, 0);
}

static const SN0ModelRegion *sn0_region(const SN0MachineModel *model,
                                        SN0ModelRegionRole role)
{
    const SN0ModelRegion *region = sn0_model_find_region(model, role);

    g_assert(region);
    return region;
}

/* The AMD 29F080's two unlock-cycle command addresses. */
#define SN0_FLASH_CMD_ADDR1   0x5555
#define SN0_FLASH_CMD_ADDR2   0x2aaa

/*
 * The phase's two execution homes, from the full 64-bit PC. The PC is
 * translated through the MMU rather than pattern-matched, because the
 * phase does not stay on the unmapped aliases: besides the compatibility
 * segments and XKPHYS, the PROM's guarded PI error probe runs from a
 * TLB-mapped alias of the flash, and under ERL the low region is an
 * unmapped alias of physical memory. A node-qualified physical address
 * reduces to its node-local offset. Comparing physically also admits
 * the cached alias of the PROM's RAM copy; harmless, because the phase
 * bit already over-covers the residency and post-exit PROM code touches
 * no window address on the normal path.
 */
static bool sn0_promdata_pc_in_prom(SN0PromDataState *s)
{
    const SN0MachineModel *model =
        SN0_MACHINE_GET_CLASS(qdev_get_machine())->model;
    const SN0ModelRegion *memory_region =
        sn0_region(model, SN0_MODEL_REGION_NODE_MEMORY);
    const SN0ModelRegion *flash =
        sn0_region(model, SN0_MODEL_REGION_PFLASH_RESET);
    TranslateForDebugResult r;
    uint64_t phys;

    if (!cpu_translate_for_debug(CPU(s->cpu), s->cpu->env.active_tc.PC, &r)) {
        return false;
    }
    /*
     * An uncached alias names the same node-local text: drop the
     * uncached-attribute window above the address, then the node.
     */
    phys = r.physaddr & ((1ULL << MIPS_UNCACHED_WINDOW_SHIFT) - 1);
    if (phys - s->node_base < memory_region->size) {
        phys -= s->node_base;
    }

    return (phys >= flash->offset && phys < flash->offset + flash->size) ||
           (phys >= SN0_PROM_COPY_PADDR &&
            phys < SN0_PROM_COPY_PADDR + flash->size);
}

/*
 * The window holds this CPU's cache-resident PROM stack for the whole
 * cache-as-RAM phase, entered when boot-status bit 1 is set with the PC
 * in one of the phase's execution homes -- the boot flash, or the RAM
 * copy of the PROM text it jumps to mid-phase. Both conditions are
 * needed to recognise the phase: the bit alone over-covers it, staying
 * set well after the stack has left the window, and a single-home PC
 * test flips the classification at the mid-phase jump. Once recognised,
 * the phase holds for this CPU until reset or OS handoff: on real
 * hardware the window is resident cache lines, and neither clearing the
 * bookkeeping bit nor moving the PC is a cache operation, so no single
 * later state change may reroute an access. Handoff is both recognition
 * inputs withdrawn at once -- the bit clear and the PC outside both
 * homes -- a state first reached with the OS running, because the PROM
 * clears the bit at its second uncached switch and then executes from
 * its RAM copy through handoff. The exit's cache invalidate discards
 * the private lines without writing them back, so serving RAM from then
 * on is faithful; like the OS-execution release below, this one is
 * one-way until reset. Uncached accesses always forward -- the phase's
 * exit copies the persistent structures out to real RAM through exactly
 * that path. Contract: the cache-as-RAM phase and exit sections of
 * docs/datasheets/firmware-structures.md (in the origami project tree,
 * not this repository).
 */
static void sn0_promdata_leave_private(CPUState *cs, run_on_cpu_data data);

static bool sn0_promdata_is_private(SN0PromDataState *s, MemTxAttrs attrs)
{
    if (!current_cpu || current_cpu != CPU(s->cpu)) {
        return false;
    }

    if (attrs.mips_uncached) {
        return false;
    }

    if (s->leaving_private) {
        return false;
    }

    if (s->phase_private) {
        /*
         * Test the bit first: the PC translation runs only on the
         * phase's way out, not on every private access.
         */
        if (!(s->cpu->env.active_fpu.fpr[0].d &
              SN0_PROM_BOOT_STATUS_CACHE_AS_RAM) &&
            !sn0_promdata_pc_in_prom(s)) {
            s->phase_private = false;
            s->leaving_private = true;
            async_safe_run_on_cpu(CPU(s->cpu), sn0_promdata_leave_private,
                                  RUN_ON_CPU_HOST_PTR(s));
        }
        return s->phase_private;
    }

    if ((s->cpu->env.active_fpu.fpr[0].d &
         SN0_PROM_BOOT_STATUS_CACHE_AS_RAM) &&
        sn0_promdata_pc_in_prom(s)) {
        s->phase_private = true;
    }

    return s->phase_private;
}

/*
 * Only the operating system can execute from inside the window -- the
 * PROM never does -- so execution here ends the private phase for good.
 * The PC is translated through the MMU because kernels run mapped.
 */
static bool sn0_promdata_executing_here(SN0PromDataState *s)
{
    TranslateForDebugResult r;

    if (!cpu_translate_for_debug(CPU(s->cpu), s->cpu->env.active_tc.PC, &r)) {
        return false;
    }

    return r.physaddr >= SN0_PROMDATA_PADDR &&
           r.physaddr < SN0_PROMDATA_PADDR + SN0_PROMDATA_SIZE;
}

/*
 * Runs at a quiescent point. Disabling the overlay exposes the DRAM
 * alias beneath, which backs cached translations at full speed.
 */
static void sn0_promdata_leave_private(CPUState *cs, run_on_cpu_data data)
{
    SN0PromDataState *s = data.host_ptr;

    BQL_LOCK_GUARD();
    /* A reset since scheduling re-armed the overlay; leave it in place. */
    if (s->leaving_private) {
        memory_region_set_enabled(&s->filter, false);
    }
}

static MemTxResult sn0_promdata_read(void *opaque, hwaddr addr, uint64_t *val,
                                     unsigned size, MemTxAttrs attrs)
{
    SN0PromDataState *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;

    if (!s->leaving_private && current_cpu == CPU(s->cpu) &&
        sn0_promdata_executing_here(s)) {
        s->leaving_private = true;
        async_safe_run_on_cpu(CPU(s->cpu), sn0_promdata_leave_private,
                              RUN_ON_CPU_HOST_PTR(s));
    }

    if (sn0_promdata_is_private(s, attrs)) {
        *val = ldn_be_p(memory_region_get_ram_ptr(&s->data) + addr, size);
        return MEMTX_OK;
    }

    result = address_space_read(&address_space_memory,
                                s->node_base + SN0_PROMDATA_PADDR + addr,
                                attrs, bytes, size);
    *val = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult sn0_promdata_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size, MemTxAttrs attrs)
{
    SN0PromDataState *s = opaque;
    uint8_t bytes[8];

    if (sn0_promdata_is_private(s, attrs)) {
        stn_be_p(memory_region_get_ram_ptr(&s->data) + addr, size, val);
        memory_region_set_dirty(&s->data, addr, size);
        return MEMTX_OK;
    }

    stn_be_p(bytes, size, val);
    return address_space_write(&address_space_memory,
                               s->node_base + SN0_PROMDATA_PADDR + addr,
                               attrs, bytes, size);
}

static const MemoryRegionOps sn0_promdata_ops = {
    .read_with_attrs = sn0_promdata_read,
    .write_with_attrs = sn0_promdata_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static void sn0_promdata_reset(void *opaque)
{
    SN0PromDataState *s = opaque;

    memset(memory_region_get_ram_ptr(&s->data), 0, SN0_PROMDATA_SIZE);
    memory_region_set_dirty(&s->data, 0, SN0_PROMDATA_SIZE);

    /* A reboot re-enters the cache-only phase; restore the overlay. */
    s->leaving_private = false;
    s->phase_private = false;
    memory_region_set_enabled(&s->filter, true);
}

/*
 * A raw buffer, not a RAM region, so the memory core does not migrate it;
 * both ends derive data_size from the same configuration at realize.
 */
static const VMStateDescription vmstate_sn0_backdoor = {
    .name = "sn0-backdoor",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(data, SN0BackdoorState, 0, NULL, data_size),
        VMSTATE_END_OF_LIST()
    }
};

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
        SN0PromDataState *s = &node->promdata[slice];

        if (s->present) {
            memory_region_set_enabled(&s->filter, !s->leaving_private);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sn0_promdata_slice = {
    .name = "sn0-flash/promdata-slice",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(leaving_private, SN0PromDataState),
        VMSTATE_BOOL(phase_private, SN0PromDataState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sn0_promdata = {
    .name = "sn0-flash/promdata",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sn0_promdata_needed,
    .post_load = sn0_promdata_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(promdata, SN0NodeState, SN0_MAX_CPUS_PER_NODE, 1,
                             vmstate_sn0_promdata_slice, SN0PromDataState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Named "sn0-flash" for wire compatibility: the pflash device migrates
 * its own state, so this carries only the back doors and promdata. The
 * unused field keeps old streams loading; their legacy flash state is
 * discarded rather than translated.
 */
static const VMStateDescription vmstate_sn0_flash = {
    .name = "sn0-flash",
    .version_id = 3,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UNUSED_V(1, 4),
        VMSTATE_STRUCT(backdoor_ecc_state, SN0NodeState, 2,
                       vmstate_sn0_backdoor, SN0BackdoorState),
        VMSTATE_STRUCT(backdoor_dir_state, SN0NodeState, 2,
                       vmstate_sn0_backdoor, SN0BackdoorState),
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

static uint64_t sn0_backdoor_absent_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    return SN0_BACKDOOR_ABSENT_VALUE;
}

static void sn0_backdoor_absent_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size)
{
    /* Dropped; firmware's sizing walk runs with no fault guard here. */
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
 * Real storage only as wide as the plainer directory format: a read
 * reveals the low sixteen bits of each eight-byte entry, the rest read
 * zero -- plain RAM would answer firmware's width probe "premium format".
 * Addresses are memory addresses shifted right by two, so only the
 * unique quarter span is stored, repeated across the window.
 */
static uint8_t *sn0_backdoor_byte(SN0BackdoorState *s, hwaddr addr)
{
    unsigned int bank = addr / s->bank_stride;
    uint64_t within_bank = addr % s->bank_stride;

    g_assert(bank < s->populated_banks);
    return s->data + s->bank_offset[bank] +
           within_bank % s->bank_span[bank];
}

static uint64_t sn0_backdoor_populated_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    SN0BackdoorState *s = opaque;
    uint64_t raw = 0;
    unsigned int i;

    for (i = 0; i < size; i++) {
        raw = (raw << 8) | *sn0_backdoor_byte(s, addr + i);
    }

    /* Only the low two bytes of each eight-byte-aligned entry are real. */
    for (i = 0; i < size; i++) {
        if (((addr + i) & 7) < 6) {
            raw &= ~(0xffULL << (8 * (size - 1 - i)));
        }
    }
    return raw;
}

static void sn0_backdoor_populated_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    SN0BackdoorState *s = opaque;
    unsigned int i;

    for (i = 0; i < size; i++) {
        unsigned int shift = 8 * (size - 1 - i);

        *sn0_backdoor_byte(s, addr + i) = val >> shift;
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

static void sn0_backdoor_configure(SN0BackdoorState *s,
                                   const SN0NodeState *node,
                                   uint64_t bank_stride)
{
    uint64_t total = 0;
    unsigned int bank;

    s->populated_banks = node->populated_banks;
    s->bank_stride = bank_stride;
    for (bank = 0; bank < node->populated_banks; bank++) {
        s->bank_offset[bank] = total;
        s->bank_span[bank] = (uint64_t)node->bank_mb[bank] * MiB / 4;
        total += s->bank_span[bank];
    }
    g_assert(total <= UINT32_MAX);
    s->data = g_malloc0(total);
    s->data_size = total;
}

/*
 * Nothing resets a MIPS processor automatically: without the explicit
 * reset the machine services a reset request with the program counter
 * untouched. The mode bits are driven from the same constant the flash
 * configuration block advertises -- real firmware compares the two and
 * reports a mismatch.
 */
static void sn0_cpu_reset(void *opaque)
{
    CPUState *cs = CPU(opaque);
    MachineState *machine = MACHINE(qdev_get_machine());
    SN0MachineState *s = SN0_MACHINE(machine);

    cpu_reset(cs);
    if (s->synthesized_firmware && machine->kernel_filename &&
        cs->cpu_index != 0) {
        /* Direct boot enters only the primary; launch_slave starts the rest. */
        cs->halted = 1;
    }
}

void sn0_mode_bits_reset(void *opaque)
{
    CPUMIPSState *env = &MIPS_CPU(opaque)->env;
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(qdev_get_machine());

    env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
                       (smc->model->cpu_mode_bits & SN0_PROCESSOR_MODE_MASK);
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

    qemu_devices_reset(type);

    if (s->synthesized_firmware) {
        sn0_firmware_reset(s);
    }
    CPU_FOREACH(cs) {
        sn0_cpu_reset(cs);
        sn0_mode_bits_reset(cs);
    }
    if (s->synthesized_firmware) {
        sn0_assign_pci_resources(s);
    }
    sn0_diagnostic_machine_reset(s);
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
    g_assert(profile);

    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * max_cpus);
    machine->possible_cpus->len = max_cpus;
    for (node_index = 0; node_index < profile->node_count; node_index++) {
        const SN0ModelNodeInstance *node =
            sn0_model_profile_node(profile, node_index);
        unsigned int slice;

        for (slice = 0; slice < node->cpu_count; slice++, index++) {
            CPUArchId *slot = &machine->possible_cpus->cpus[index];

            g_assert(index < max_cpus);
            slot->type = machine->cpu_type;
            slot->vcpus_count = 1;
            slot->arch_id = sn0_model_node_cpu_id(node, slice);
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
    g_assert(profile);
    for (node_index = 0; node_index < profile->node_count; node_index++) {
        const SN0ModelNodeInstance *node =
            sn0_model_profile_node(profile, node_index);

        if (index < node->cpu_count) {
            return node_index;
        }
        index -= node->cpu_count;
    }
    g_assert_not_reached();
}

/*
 * Which node a NASID-less address names in memory space: normally the
 * node's own, but after synthesized firmware hands a kernel control,
 * remote processors' unqualified addresses name node zero (Linux
 * disables PI_CALIAS_SIZE first). Shared with the Hub's engines.
 */
static hwaddr sn0_node_local_memory_base(MachineState *machine,
                                         unsigned int nasid)
{
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(machine)->model;

    if (nasid != 0 && SN0_MACHINE(machine)->synthesized_firmware &&
        machine->kernel_filename) {
        return sn0_space_node_base(model, SN0_MODEL_SPACE_MEMORY, 0);
    }
    return sn0_space_node_base(model, SN0_MODEL_SPACE_MEMORY, nasid);
}

static void sn0_node_create_cpus(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelRegion *memory_region =
        sn0_region(node->model, SN0_MODEL_REGION_NODE_MEMORY);
    const SN0ModelAddressSpace *hub_special =
        sn0_space(node->model, SN0_MODEL_SPACE_HUB_SPECIAL);
    hwaddr node_base = sn0_space_node_base(
        node->model, SN0_MODEL_SPACE_MEMORY, node->nasid);
    unsigned int node_index = node->nasid;
    unsigned int slice, half, space_index;

    g_assert(node->model->address_space_count <= SN0_MAX_ADDRESS_SPACES);
    for (slice = 0; slice < node->nr_cpus; slice++) {
        g_autofree char *alias_name =
            g_strdup_printf("sn0.node%u.cpu%u.system", node_index, slice);
        g_autofree char *name = g_strdup_printf("cpu%u", slice);
        g_autofree char *promdata_name =
            g_strdup_printf("sn0.node%u.cpu%u.promdata", node_index, slice);
        g_autofree char *promdata_filter_name = g_strdup_printf(
            "sn0.node%u.cpu%u.promdata-filter", node_index, slice);
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
         * NASID-less addresses name the reading processor's node;
         * explicit NASIDs fall to the Hub's claim layer underneath.
         */
        for (space_index = 0;
             space_index < node->model->address_space_count; space_index++) {
            const SN0ModelAddressSpace *space_model =
                &node->model->address_spaces[space_index];
            g_autofree char *local_name = g_strdup_printf(
                "sn0.node%u.cpu%u.local-space%u", node_index, slice,
                space_model->selector);
            hwaddr alias_node_base =
                space_model->kind == SN0_MODEL_SPACE_MEMORY ?
                node->local_memory_base : node_base;
            hwaddr local_offset = 0;
            hwaddr local_size = memory_region->size;

            /*
             * Self-decode only the Hub's own eight-megabyte window: the
             * nameless half names the reader's node, and every explicit
             * node-qualified crosstalk reference -- node zero included --
             * is the claim layer's to route by programmed number. Other
             * spaces keep the wide alias.
             */
            if (space_model->kind == SN0_MODEL_SPACE_XTALK) {
                const SN0ModelRegion *hub_region = sn0_region(
                    node->model, SN0_MODEL_REGION_HUB_REGISTERS_LOCAL);

                local_offset = hub_region->offset;
                local_size = hub_region->size;
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
            node->promdata[slice].cpu = node->cpu[slice];
            node->promdata[slice].node_base = node_base;
            node->promdata[slice].present = true;
            memory_region_init_ram(&node->promdata[slice].data, OBJECT(node),
                                   promdata_name, SN0_PROMDATA_SIZE,
                                   &error_fatal);
            memory_region_init_io(&node->promdata[slice].filter, OBJECT(node),
                                  &sn0_promdata_ops, &node->promdata[slice],
                                  promdata_filter_name, SN0_PROMDATA_SIZE);
            /*
             * Firmware traps copy through guest pointers; the shared
             * owner would trip the reentrancy guard.
             */
            node->promdata[slice].filter.disable_reentrancy_guard = true;
            qemu_register_reset(sn0_promdata_reset, &node->promdata[slice]);

            /* Intercept the PROM's cached carrier window before node RAM. */
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                sn0_space(node->model, SN0_MODEL_SPACE_MEMORY)->carrier_base +
                SN0_PROMDATA_PADDR,
                &node->promdata[slice].filter, 2);
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
    const SN0ModelMemory *geometry = &node->model->memory;
    const SN0ModelRegion *local_memory =
        sn0_region(node->model, SN0_MODEL_REGION_HUB_LOCAL_MEMORY);
    const SN0ModelRegion *remote_memory =
        sn0_region(node->model, SN0_MODEL_REGION_HUB_REMOTE_MEMORY);
    hwaddr node_base = sn0_space_node_base(
        node->model, SN0_MODEL_SPACE_MEMORY, node->nasid);
    hwaddr hub_base = sn0_space_node_base(
        node->model, SN0_MODEL_SPACE_HUB_SPECIAL, node->nasid);
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
    MemoryRegion *system = get_system_memory();
    const SN0ModelRegion *flash_hub =
        sn0_region(node->model, SN0_MODEL_REGION_PFLASH_LBOOT);
    const SN0ModelRegion *flash_reset =
        sn0_model_region_alias(node->model, flash_hub);
    const SN0ModelRegion *flash_rboot =
        sn0_region(node->model, SN0_MODEL_REGION_PFLASH_RBOOT);
    const SN0ModelRegion *rboot_storage =
        sn0_model_region_alias(node->model, flash_rboot);
    const SN0ModelRegion *ecc =
        sn0_region(node->model, SN0_MODEL_REGION_BACKDOOR_ECC);
    const SN0ModelRegion *directory =
        sn0_region(node->model, SN0_MODEL_REGION_BACKDOOR_DIRECTORY);
    hwaddr node_base = sn0_space_node_base(
        node->model, SN0_MODEL_SPACE_MEMORY, node->nasid);
    hwaddr hub_base = sn0_space_node_base(
        node->model, SN0_MODEL_SPACE_HUB_SPECIAL, node->nasid);
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

    g_assert(flash_reset &&
             flash_reset->space == SN0_MODEL_SPACE_MEMORY);
    g_assert(rboot_storage == flash_reset);
    g_assert(flash_reset->size > 0 &&
             flash_reset->size % SN0_FLASH_SECTOR_SIZE == 0);
    g_assert(flash_reset->size / SN0_FLASH_SECTOR_SIZE <= UINT32_MAX);
    g_assert(node->model->memory.architectural_banks > 0);
    g_assert(ecc->size % node->model->memory.architectural_banks == 0);
    g_assert(directory->size % node->model->memory.architectural_banks == 0);

    /*
     * An Am29F080, byte-wide, with no backing drive -- the firmware builder
     * seeds the image after realize. Never mapped directly; every view goes
     * through the Hub.
     */
    DeviceState *flash_dev = qdev_new(TYPE_PFLASH_CFI02);
    qdev_prop_set_uint32(flash_dev, "num-blocks",
                         flash_reset->size / SN0_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint32(flash_dev, "sector-length", SN0_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(flash_dev, "width", 1);
    qdev_prop_set_uint8(flash_dev, "mappings", 1);
    qdev_prop_set_uint8(flash_dev, "big-endian", 1);
    qdev_prop_set_uint16(flash_dev, "id0", 0x01);       /* AMD */
    qdev_prop_set_uint16(flash_dev, "id1", 0xd5);       /* Am29F080 */
    qdev_prop_set_uint16(flash_dev, "unlock-addr0", SN0_FLASH_CMD_ADDR1);
    qdev_prop_set_uint16(flash_dev, "unlock-addr1", SN0_FLASH_CMD_ADDR2);
    qdev_prop_set_string(flash_dev, "name", flash_name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(flash_dev), &error_fatal);
    node->flash = PFLASH_CFI02(flash_dev);
    node->flash_size = flash_reset->size;

    MemoryRegion *chip =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(flash_dev), 0);

    /*
     * The LBOOT window: the controller decode, with a romd alias of the
     * chip overlaid where the PROM fetches its relocated code.
     */
    memory_region_init_io(&node->flash_hub_view, OBJECT(node),
                          &sn0_boot_mirror_ops, node, hub_name,
                          flash_hub->size);
    memory_region_add_subregion(system, hub_base + flash_hub->offset,
                                &node->flash_hub_view);
    memory_region_init_alias(&node->flash_hub_fast, OBJECT(node),
                             hub_fast_name, chip, 0, flash_reset->size);
    memory_region_add_subregion_overlap(
        system, hub_base + flash_reset->offset,
        &node->flash_hub_fast, 1);

    /*
     * The reset-vector view aliases the megabyte of Hub space at the
     * flash base. Writes through it land on the chip's byte decode, not
     * the controller's strided command port -- an alias cannot route the
     * two differently, and no source establishes the reset view's write
     * decode. It overlaps RAM and wins.
     */
    memory_region_init_alias(&node->flash_reset_view, OBJECT(node),
                             reset_name, system,
                             hub_base + flash_reset->offset,
                             flash_reset->size);
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
    memory_region_init_io(&node->backdoor_ecc_absent, OBJECT(node),
                          &sn0_backdoor_absent_ops, NULL, ecc_absent_name,
                          ecc->size);
    memory_region_add_subregion_overlap(
        system, hub_base + ecc->offset,
        &node->backdoor_ecc_absent, -1);
    if (node->populated_banks > 0) {
        uint64_t bank_stride = ecc->size /
            node->model->memory.architectural_banks;
        uint64_t size = (uint64_t)node->populated_banks * bank_stride;

        sn0_backdoor_configure(&node->backdoor_ecc_state, node,
                               bank_stride);
        memory_region_init_io(&node->backdoor_ecc, OBJECT(node),
                              &sn0_backdoor_populated_ops,
                              &node->backdoor_ecc_state, ecc_name, size);
        memory_region_add_subregion(
            system, hub_base + ecc->offset,
            &node->backdoor_ecc);
    }

    memory_region_init_io(&node->backdoor_dir_absent, OBJECT(node),
                          &sn0_backdoor_absent_ops, NULL, dir_absent_name,
                          directory->size);
    memory_region_add_subregion_overlap(
        system, hub_base + directory->offset,
        &node->backdoor_dir_absent, -1);
    if (node->populated_banks > 0) {
        uint64_t bank_stride = directory->size /
            node->model->memory.architectural_banks;
        uint64_t size = (uint64_t)node->populated_banks * bank_stride;

        sn0_backdoor_configure(&node->backdoor_dir_state, node,
                               bank_stride);
        memory_region_init_io(&node->backdoor_dir, OBJECT(node),
                              &sn0_backdoor_populated_ops,
                              &node->backdoor_dir_state, dir_name, size);
        memory_region_add_subregion(
            system, hub_base + directory->offset,
            &node->backdoor_dir);
    }
}

static void sn0_node_create_hub(SN0NodeState *node)
{
    const SN0ModelRegion *memory_region =
        sn0_region(node->model, SN0_MODEL_REGION_NODE_MEMORY);
    unsigned int node_index = node->nasid;
    g_autofree char *xtalk_name =
        g_strdup_printf("sn0.node%u.xtalk", node_index);
    g_autofree char *memory_name =
        g_strdup_printf("sn0.node%u.hub.memory", node_index);
    g_autofree char *memory_system_name =
        g_strdup_printf("sn0.node%u.hub.memory.system", node_index);
    g_autofree char *memory_local_name =
        g_strdup_printf("sn0.node%u.hub.memory.local", node_index);
    MemoryRegion *system = get_system_memory();
    MIPSCPU *cpu;
    unsigned int slice;

    memory_region_init(&node->xtalk, OBJECT(node), xtalk_name,
                       SN0_XTALK_SIZE);

    /*
     * The Hub's engines address memory as this node's processors do,
     * minus the per-processor pieces -- a device has no slice to flip.
     * The Hub cannot build this itself: its programmed node number reads
     * zero under real firmware.
     */
    memory_region_init(&node->hub_memory, OBJECT(node), memory_name,
                       UINT64_MAX);
    memory_region_init_alias(&node->hub_memory_system, OBJECT(node),
                             memory_system_name, system, 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&node->hub_memory, 0,
                                        &node->hub_memory_system, -1);
    memory_region_init_alias(&node->hub_memory_local, OBJECT(node),
                             memory_local_name, system,
                             node->local_memory_base,
                             memory_region->size);
    memory_region_add_subregion_overlap(
        &node->hub_memory,
        sn0_space(node->model, SN0_MODEL_SPACE_MEMORY)->carrier_base,
                                        &node->hub_memory_local, 0);
    object_property_set_link(OBJECT(&node->hub), "memory",
                             OBJECT(&node->hub_memory), &error_fatal);

    qdev_prop_set_uint32(DEVICE(&node->hub), "cpus", node->nr_cpus);
    qdev_prop_set_uint32(DEVICE(&node->hub), "cpu-base", node->cpu_base);
    qdev_prop_set_uint8(DEVICE(&node->hub), "ni-revision",
                        node->model->hub.ni_revision);
    /*
     * Each PIMM record part gets its own synthetic serial: per-unit as the
     * hub one-wire datasheet's contract requires, never a real machine's.
     */
    qdev_prop_set_uint64(DEVICE(&node->hub), "onewire-serial",
                         node->model->hub.serial_base +
                         node->model->hub.serial_stride * node->nasid);
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
    }
    object_property_set_link(OBJECT(&node->hub), "claim-target",
                             OBJECT(system), &error_fatal);
    qdev_prop_set_uint32(DEVICE(&node->hub), "claim-node-shift",
                         ctz64(node->model->node_stride));
    qdev_prop_set_uint32(DEVICE(&node->hub), "claim-space-shift",
                         MIPS_UNCACHED_WINDOW_SHIFT);
    qdev_prop_set_uint32(DEVICE(&node->hub), "claim-own-node", node->nasid);
    sysbus_realize(SYS_BUS_DEVICE(&node->hub), &error_fatal);

    for (slice = 0; slice < node->nr_cpus; slice++) {
        cpu = node->cpu[slice];
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "bank0", slice,
            cpu->env.irq[SN0_HUB_BANK0_CAUSE_BIT - CP0Ca_IP]);
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "bank1", slice,
            cpu->env.irq[SN0_HUB_BANK1_CAUSE_BIT - CP0Ca_IP]);
        qdev_connect_gpio_out_named(
            DEVICE(&node->hub), "rt", slice,
            cpu->env.irq[SN0_HUB_RT_CAUSE_BIT - CP0Ca_IP]);
    }
}

static void sn0_node_map_xtalk_windows(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    const SN0ModelAddressSpace *xtalk =
        sn0_space(node->model, SN0_MODEL_SPACE_XTALK);
    const SN0ModelRegion *hub_explicit =
        sn0_region(node->model, SN0_MODEL_REGION_HUB_REGISTERS_NODE);
    const SN0ModelRegion *hub_local =
        sn0_model_region_alias(node->model, hub_explicit);
    unsigned int node_index = node->nasid;
    unsigned int widget;

    g_assert(hub_local && hub_local->space == SN0_MODEL_SPACE_XTALK);

    for (widget = 0; widget <= SN0_XTALK_PORT_LAST; widget++) {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.xtalk.small%u",
                            node_index, widget);

        memory_region_init_alias(&node->small_window[widget], OBJECT(node),
                                 name, &node->xtalk,
                                 SN0_XTALK_BASE(widget),
                                 SN0_SMALL_WINDOW_SIZE);
        memory_region_add_subregion(
            system, sn0_model_node_address(node->model, xtalk, node->nasid,
                                           SN0_SMALL_WINDOW(widget)),
            &node->small_window[widget]);
    }

    memory_region_add_subregion(
        system, sn0_model_node_address(node->model, xtalk, node->nasid,
                                       SN0_BIG_WINDOW_BASE),
        &node->hub.bwin);
    memory_region_add_subregion(
        system, sn0_model_region_address(node->model, xtalk, node->nasid,
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
        system, sn0_model_region_address(node->model, xtalk, node->nasid,
                                         hub_explicit, 0),
        &node->hub_regs_node_view);
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
    unsigned int max_nodes = sn0_model_max_nodes(model);
    unsigned int node_count, node_index, cpu_index = 0;

    /* A product class must supply its static description and QOM adapters. */
    g_assert(model && smc->populate_io);
    g_assert(max_nodes > 0);
    g_assert(model->cpu_slots_per_node <= SN0_MAX_CPUS_PER_NODE);
    g_assert(model->address_space_count <= SN0_MAX_ADDRESS_SPACES);
    g_assert(model->memory.architectural_banks <= SN0_MAX_MEMORY_BANKS);
    profile_cpus = g_new0(unsigned int, max_nodes);

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

    s->num_nodes = node_count;
    s->nodes = g_new0(SN0NodeState *, node_count);
    s->io_bridge = g_new0(SGIBridgeState *, node_count);
    s->widget0 = g_new0(MemoryRegion, node_count);
    s->widget_alias = g_malloc0(sizeof(*s->widget_alias) * node_count);
    s->pci_ioc3 = g_new0(PCIDevice *, node_count);
    s->pci_scsi = g_malloc0(sizeof(*s->pci_scsi) * node_count);

    for (node_index = 0; node_index < node_count; node_index++) {
        g_autofree char *name = g_strdup_printf("node%u", node_index);
        Object *node = object_new(TYPE_SN0_NODE);

        object_property_add_child(OBJECT(machine), name, node);
        object_unref(node);
        s->nodes[node_index] = SN0_NODE(node);
    }

    s->cpu_refclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(s->cpu_refclk, model->cpu_clock_hz);

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        const SN0ModelNodeInstance *node_model =
            sn0_model_profile_node(profile, node_index);
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

        g_assert(model->memory.architectural_banks <=
                 SN0_MAX_MEMORY_BANKS);
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
        node->model = model;
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", cpu_base);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", cpu_count);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_uint64(DEVICE(node), "local-memory-base",
                             sn0_node_local_memory_base(machine,
                                                        node_model->nasid));
        qdev_prop_set_bit(DEVICE(node), "promdata-filter",
                          !(s->synthesized_firmware &&
                            machine->kernel_filename));
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks_mb);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);

        /*
         * The Hub facts the machine rather than the node owns: its
         * programmed node number (real firmware programs it later, so a
         * real-PROM boot starts at zero), which chassis is the slave, the
         * chassis DIP switches, the CrayLink peer, the system controller's
         * AUX backend, and the bring-up UART. Every module has its own
         * physical AUX connector, so each node's controller gets its own
         * -serial index, node-major with a fixed stride: node 0's AUX at 2,
         * node 1's at 6. Resolved here because assigning one character
         * backend to two frontends aborts the machine. The bring-up UART is
         * a node-0 diagnostic device, not part of any shipped chassis;
         * keeping it on the console node avoids the same collision.
         */
        qdev_prop_set_uint32(DEVICE(&node->hub), "nasid",
                             s->synthesized_firmware ? node_model->nasid : 0);
        qdev_prop_set_bit(DEVICE(&node->hub), "slave-module",
                          node_index != 0);
        qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                            s->elsc_dip_switches);
        aux_chr = serial_hd(SN0_NODE_SERIAL(node_index, SN0_ELSC_CHARDEV));
        if (aux_chr) {
            object_property_set_link(OBJECT(&node->hub), "elsc-chardev",
                                     OBJECT(aux_chr), &error_fatal);
        }
        if (profile->interconnect == SN0_MODEL_INTERCONNECT_DIRECT_PEER) {
            qdev_prop_set_bit(DEVICE(&node->hub), "network-link-up", true);
            object_property_set_link(OBJECT(&node->hub), "network-peer",
                                     OBJECT(&s->nodes[1 - node_index]->hub),
                                     &error_fatal);
            qdev_prop_set_uint32(DEVICE(&node->hub), "claim-peer-node",
                sn0_model_profile_node(profile, 1 - node_index)->nasid);
        }
        qdev_prop_set_bit(DEVICE(&node->hub), "junk-uart",
                          node_index == 0 && s->junk_uart_present);
        if (node_index == 0 && s->junk_uart_present &&
            serial_hd(SN0_NODE_SERIAL(0, SN0_JUNK_UART_CHARDEV))) {
            object_property_set_link(
                OBJECT(&node->hub), "junk-uart-chardev",
                OBJECT(serial_hd(SN0_NODE_SERIAL(0, SN0_JUNK_UART_CHARDEV))),
                &error_fatal);
        }
        sn0_diagnostic_attach(s, node_index);

        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);

        for (slice = 0; slice < cpu_count; slice++) {
            machine->possible_cpus->cpus[cpu_index++].cpu =
                CPU(node->cpu[slice]);
        }
        ram_offset += ram_size;
    }
    g_assert(cpu_index == machine->smp.cpus);
    g_assert(ram_offset == machine->ram_size);

    for (node_index = 0; node_index < node_count; node_index++) {
        smc->populate_io(s, node_index);
    }

    sn0_firmware_init(machine);
}

static bool sn0_get_synthesized_firmware(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->synthesized_firmware;
}

static void sn0_set_synthesized_firmware(Object *obj, bool value, Error **errp)
{
    SN0_MACHINE(obj)->synthesized_firmware = value;
}

static bool sn0_get_io_bridge(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->io_bridge_present;
}

static void sn0_set_io_bridge(Object *obj, bool value, Error **errp)
{
    SN0_MACHINE(obj)->io_bridge_present = value;
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

static bool sn0_get_diagnostic_recording(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->diagnostic_recording;
}

static void sn0_set_diagnostic_recording(Object *obj, bool value, Error **errp)
{
    if (phase_check(PHASE_MACHINE_CREATED)) {
        error_setg(errp, "diagnostic-recording cannot change after machine "
                         "creation");
        return;
    }
    SN0_MACHINE(obj)->diagnostic_recording = value;
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

static void sn0_get_widget0_revision(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    uint32_t value = SN0_MACHINE(obj)->widget0_revision;

    visit_type_uint32(v, name, &value, errp);
}

static void sn0_set_widget0_revision(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > SN0_WIDGET0_REVISION_MAX) {
        error_setg(errp, "the widget 0 revision is a four-bit field, so at "
                         "most %u", SN0_WIDGET0_REVISION_MAX);
        return;
    }
    SN0_MACHINE(obj)->widget0_revision = value;
}

static void sn0_node_realize(DeviceState *dev, Error **errp)
{
    SN0NodeState *node = SN0_NODE(dev);
    unsigned int bank;

    g_assert(node->model);
    if (node->nr_cpus < 1 ||
        node->nr_cpus > node->model->cpu_slots_per_node) {
        error_setg(errp, "a node carries one to %u processors, not %u",
                   node->model->cpu_slots_per_node, node->nr_cpus);
        return;
    }
    if (node->nr_banks_mb < 1 ||
        node->nr_banks_mb > node->model->memory.physical_banks) {
        error_setg(errp, "a node fills one to %u memory banks, not %u",
                   node->model->memory.physical_banks, node->nr_banks_mb);
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
}

static const Property sn0_node_properties[] = {
    DEFINE_PROP_UINT32("nasid", SN0NodeState, nasid, 0),
    DEFINE_PROP_UINT32("cpu-base", SN0NodeState, cpu_base, 0),
    DEFINE_PROP_UINT32("nr-cpus", SN0NodeState, nr_cpus, 1),
    DEFINE_PROP_STRING("cpu-type", SN0NodeState, cpu_type),
    DEFINE_PROP_LINK("ram", SN0NodeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("ram-start", SN0NodeState, ram_start, 0),
    DEFINE_PROP_UINT64("local-memory-base", SN0NodeState,
                       local_memory_base, 0),
    DEFINE_PROP_BOOL("promdata-filter", SN0NodeState, promdata_filter, true),
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
    dc->vmsd = &vmstate_sn0_flash;
    device_class_set_props(dc, sn0_node_properties);
    /* The machine wires a node's RAM, clock and Hub; -device cannot. */
    dc->user_creatable = false;
}

static void sn0_instance_init(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);
    const SN0MachineModel *model = SN0_MACHINE_GET_CLASS(obj)->model;

    g_assert(model);
    s->synthesized_firmware = true;
    s->io_bridge_present = true;
    s->debug_leds = true;
    s->widget0_revision = model->base_io.default_revision;
}

static void sn0_instance_finalize(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);

    sn0_diagnostic_finalize(s);
    g_free(s->nodes);
    g_free(s->io_bridge);
    g_free(s->pci_ioc3);
    g_free(s->pci_scsi);
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
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R10000");
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

    object_class_property_add_bool(oc, "io-bridge",
                                   sn0_get_io_bridge, sn0_set_io_bridge);
    object_class_property_set_description(oc, "io-bridge",
        "Fit the I/O bridge, the chip between the crossbar and the PCI bus. "
        "On by default, because a real Origin 200 always has one and every "
        "device on this machine hangs off it: turning it off leaves a board "
        "with no PCI bus, which is a bring-up aid rather than a machine that "
        "ever existed. Kept as an option because that aid is still useful "
        "for isolating a fault to the fabric or to what sits behind it.");

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

    object_class_property_add_bool(oc, "diagnostic-recording",
                                   sn0_get_diagnostic_recording,
                                   sn0_set_diagnostic_recording);
    object_class_property_set_description(oc, "diagnostic-recording",
        "Record recent Hub and system-controller transitions for "
        "x-query-origin-state. Off by default; the state snapshot remains "
        "available when recording is disabled.");

    object_class_property_add(oc, "elsc-dip-switches", "uint8",
                              sn0_get_elsc_dip_switches,
                              sn0_set_elsc_dip_switches, NULL, NULL);
    object_class_property_set_description(oc, "elsc-dip-switches",
        "Set the eight physical ELSC DIP switches as asserted bits. The "
        "controller reports their active-low wire value to firmware.");

    /* Settable so another captured configuration can be measured. */
    object_class_property_add(oc, "widget0-revision", "uint32",
                              sn0_get_widget0_revision,
                              sn0_set_widget0_revision, NULL, NULL);
    object_class_property_set_description(oc, "widget0-revision",
        "The silicon revision of the I/O bridge, which is what crosstalk "
        "widget 0 and widget 8 both report because they are one chip. Both "
        "guests treat a revision below four as having a byte-swapping defect "
        "and avoid that widget's PCI I/O space, so this changes behaviour and "
        "not only a printed line.");
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
        .name              = TYPE_SN0_NODE,
        .parent            = TYPE_SYS_BUS_DEVICE,
        .instance_size     = sizeof(SN0NodeState),
        .instance_init     = sn0_node_instance_init,
        .class_init        = sn0_node_class_init,
    },
};

DEFINE_TYPES(sn0_machine_types)
