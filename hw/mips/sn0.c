/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) family machines.
 *
 * The bare machine: one node's memory and its boot flash, and nothing else yet.
 * The Hub, the crossbar and the I/O bridge grow into the address space this
 * file lays out.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/loader.h"
#include "hw/mips/bootloader.h"
#include "elf.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/datadir.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/mips/mips.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/numa.h"
#include "system/reset.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "trace/control.h"
#include "cpu.h"
#include "sn0.h"

/*
 * A stock Origin 200 runs its R10000 at 180 MHz and its Hub at 90 MHz. The
 * processor's own counter runs at half the processor clock, which is what the
 * CPU model's counter divisor expresses, so this is the input that counter is
 * derived from.
 */
#define SN0_CPU_REFCLK_HZ (180 * 1000 * 1000)

/* IP29 has four banks, each made from one matched DIMM pair. */
#define IP29_MEM_BANKS 4

/*
 * Where the argument vector handed to a kernel goes. Low memory below the
 * firmware structures and above the exception vectors a kernel installs. The
 * structure work owns the final layout of this region; this is the one address
 * in it that exists so far.
 */
#define SN0_ARGV_ADDR   0x800
#define SN0_ARGV_STRING (SN0_ARGV_ADDR + 0x40)

/*
 * The standard MIPS ECOFF object layout: documented publicly in binutils'
 * COFF/ECOFF support and used by SGI's own pre-ELF toolchain among others,
 * not SGI-proprietary. Every offset below is cross-checked directly against
 * a real sashARCS binary -- see
 * docs/library/irix-sash-direct-load-proposal.md for the byte trace.
 */
#define ECOFF_MAGIC_MIPSEB        0x0163
#define ECOFF_FILEHDR_SIZE        20
#define ECOFF_FILEHDR_MAGIC_OFF   0x00   /* uint16 */
#define ECOFF_FILEHDR_NSCNS_OFF   0x02   /* uint16 */
#define ECOFF_FILEHDR_OPTHDR_OFF  0x10   /* uint16 */
#define ECOFF_AOUTHDR_OMAGIC      0407   /* fixed-address, unrelocated */
#define ECOFF_AOUTHDR_MAGIC_OFF   0x00   /* uint16, within the optional header */
#define ECOFF_AOUTHDR_ENTRY_OFF   0x10   /* uint32 */
#define ECOFF_AOUTHDR_GPVALUE_OFF 0x34   /* uint32, the o32 ABI gp base */
#define ECOFF_SCNHDR_SIZE         40
#define ECOFF_SCNHDR_VADDR_OFF    0x0c   /* uint32, within each section header */
#define ECOFF_SCNHDR_SIZE_OFF     0x10   /* uint32 */
#define ECOFF_SCNHDR_SCNPTR_OFF   0x14   /* uint32 */

/*
 * Scratch layout for a direct-loaded sashARCS's argv/envp/stack: computed
 * from the image's own extent, never a fixed address, since the two IRIX
 * releases this project holds ship differently sized images.
 */
#define SN0_SASH_SCRATCH_GAP  (1 * MiB)
#define SN0_SASH_STACK_SIZE   (64 * KiB)

/* What the firmware configuration block says about this machine. */
#define SN0_CPU_FREQ_HZ   180000000
#define SN0_HUB_FREQ_HZ   90000000
#define SN0_RTC_FREQ_HZ   1250000       /* an 800 ns tick */
#define SN0_MACHINE_TYPE  1             /* 0 is Origin 2000, 1 is Origin 200 */
#define SN0_FPROM_CYC     15
#define SN0_FPROM_WR      4
#define SN0_CONFIG_SIZE   0x50
#define SN0_CONFIG_FPROM_CYC_OFFSET 0x2c
#define SN0_CONFIG_MACHINE_TYPE_OFFSET 0x30
#define SN0_CONFIG_CHECKSUM_OFFSET 0x34
#define SN0_CONFIG_FPROM_WR_OFFSET 0x3c
#define SN0_CPUS_PER_NODE 2
#define SN0_ORIGIN200_MAX_NODES 2
#define SN0_BRIDGE_ONEWIRE_SERIAL_BASE 0x060504030201ULL
#define SN0_UNCACHED_SPACES 4
#define SN0_HUB_PRIVATE_SIZE (128 * KiB)
#define SN0_HUB_PRIVATE_HALF (SN0_HUB_PRIVATE_SIZE / 2)
#define SN0_PROMDATA_PADDR  0x100000
#define SN0_PROMDATA_SIZE   (32 * KiB)
#define SN0_PROM_COPY_PADDR 0x01a00000
#define SN0_PROM_BSR_DEX    (1U << 1)

/*
 * A component's port descriptor: a sixteen-bit node identifier, an eight-bit
 * flag byte, eight bits of padding, and a thirty-two-bit offset, packed into
 * one big-endian eight-byte field. Zero is a live node identifier on this
 * machine, not a safe default — the invalid marker is all ones in the node-
 * identifier bits and nothing else, with the rest of the descriptor zero for
 * a port that names no link, which every port on this machine does: the Hub
 * sits at a fixed widget with no crossbar component modelled.
 */
#define SN0_PORT_NODE_INVALID   0xffffULL
#define SN0_PORT_UNCONNECTED    (SN0_PORT_NODE_INVALID << 48)

#define TYPE_SN0_NODE "sn0-node"

typedef struct SN0BackdoorState {
    uint8_t *data;
    uint64_t bank_offset[SN0_MEM_BANKS];
    uint64_t bank_span[SN0_MEM_BANKS];
    unsigned int populated_banks;
} SN0BackdoorState;

typedef enum SN0FlashState {
    SN0_FLASH_READ,
    SN0_FLASH_UNLOCK1,
    SN0_FLASH_UNLOCK2,
    SN0_FLASH_AUTOSELECT,
    SN0_FLASH_PROGRAM,
    SN0_FLASH_ERASE_UNLOCK1,
    SN0_FLASH_ERASE_UNLOCK2,
    SN0_FLASH_ERASE_SELECT,
} SN0FlashState;

typedef struct SN0PromDataState {
    MIPSCPU *cpu;
    hwaddr node_base;
    MemoryRegion data;
    MemoryRegion filter;
} SN0PromDataState;

typedef struct SN0NodeState {
    DeviceState parent_obj;

    uint32_t nasid;
    Clock *cpuclk;
    MIPSCPU *cpu[SN0_CPUS_PER_NODE];
    MemoryRegion cpu_sysmem[SN0_CPUS_PER_NODE];
    MemoryRegion sysmem_alias[SN0_CPUS_PER_NODE];
    MemoryRegion local_space[SN0_CPUS_PER_NODE][SN0_UNCACHED_SPACES];
    MemoryRegion hub_private[SN0_CPUS_PER_NODE][2];
    SN0PromDataState promdata[SN0_CPUS_PER_NODE];
    SGIHubState hub;

    /*
     * The Hub's own view of memory, for its block transfer engines: the
     * global map underneath and this node's memory over the node-less
     * addresses, the same shape as each processor's local view.
     */
    MemoryRegion hub_memory;
    MemoryRegion hub_memory_system;
    MemoryRegion hub_memory_local;

    MemoryRegion flash;
    MemoryRegion flash_hub_view;
    MemoryRegion flash_rboot_view;
    SN0FlashState flash_state;
    MemoryRegion hub_regs_node_view;
    MemoryRegion arcs_trap;
    MemoryRegion fw_text;
    MemoryRegion backdoor_ecc;
    MemoryRegion backdoor_ecc_absent;
    MemoryRegion backdoor_dir;
    MemoryRegion backdoor_dir_absent;
    SN0BackdoorState backdoor_ecc_state;
    SN0BackdoorState backdoor_dir_state;

    MemoryRegion xtalk;
    MemoryRegion small_window[SN0_XTALK_WIDGETS];
    MemoryRegion mem_slot[SN0_MEM_BANKS][SN0_MEM_BANK_SLOTS];
    MemoryRegion hub_local_mem_slot[SN0_MEM_BANKS][SN0_MEM_BANK_SLOTS];
    uint16_t bank_mb[SN0_MEM_BANKS];
    unsigned int populated_banks;

    uint64_t mem_addr;
    unsigned int mem_count;
} SN0NodeState;

DECLARE_INSTANCE_CHECKER(SN0NodeState, SN0_NODE, TYPE_SN0_NODE)

#define TYPE_SN0_MACHINE MACHINE_TYPE_NAME("sn0")

#define SN0_DIAGNOSTIC_EVENT_COUNT 4096
#define SN0_DIAGNOSTIC_READ_CACHE_COUNT 256

typedef struct SN0DiagnosticEvent {
    uint64_t sequence;
    int64_t virtual_time_ns;
    uint32_t node;
    const char *component;
    const char *operation;
    uint64_t address;
    uint64_t value;
    uint8_t size;
    bool has_cpu_index;
    int cpu_index;
    bool has_pc;
    uint64_t pc;
    bool has_before_after;
    uint64_t before;
    uint64_t after;
} SN0DiagnosticEvent;

typedef struct SN0DiagnosticSinkContext {
    void *machine;
    uint32_t node;
} SN0DiagnosticSinkContext;

typedef struct SN0DiagnosticReadCacheEntry {
    const char *component;
    uint64_t address;
    uint64_t value;
    uint64_t before;
    uint64_t after;
    uint64_t pc;
    uint64_t sequence;
    int cpu_index;
    uint8_t size;
    bool has_cpu_index;
    bool has_pc;
    bool has_before_after;
    bool valid;
} SN0DiagnosticReadCacheEntry;

struct SN0MachineState {
    MachineState parent_obj;

    SN0NodeState nodes[SN0_ORIGIN200_MAX_NODES];
    SGIBridgeState io_bridge[SN0_ORIGIN200_MAX_NODES];

    /* Host diagnostics only; not guest-visible and not migrated. */
    bool diagnostic_recording;
    uint64_t diagnostic_query_sequence;
    SN0DiagnosticEvent *diagnostic_events;
    uint32_t diagnostic_event_head;
    uint32_t diagnostic_event_count;
    uint64_t diagnostic_event_next_sequence;
    uint64_t diagnostic_event_dropped;
    SN0DiagnosticReadCacheEntry *diagnostic_read_cache;
    uint16_t diagnostic_read_cache_next[SN0_ORIGIN200_MAX_NODES];
    DeviceDiagnosticSink diagnostic_sink[SN0_ORIGIN200_MAX_NODES];
    SN0DiagnosticSinkContext diagnostic_sink_context[
        SN0_ORIGIN200_MAX_NODES];

    /*
     * Widget 0, which on this machine is the I/O bridge seen a second time
     * rather than a device of its own. Held here because a MemoryRegion has
     * one parent, so the second view of the bridge's widget space has to be an
     * alias with its own storage.
     */
    MemoryRegion widget0[SN0_ORIGIN200_MAX_NODES];

    /*
     * Whether the I/O bridge is fitted. **On by default**, because a real
     * Origin 200 always has one and every device on this machine hangs off
     * it — both disk adapters, the base I/O controller with its serial ports,
     * its ethernet and its keyboard controller, and the clock on its byte
     * bus. Turning it off leaves a board that never existed.
     *
     * It was off by default once, and the reason is worth keeping because it
     * says what would have to change to flip it back: the bridge was not
     * merely absent from the default machine, it was worse than absent, since
     * OpenBSD used to enumerate further with it fitted and then stop in its
     * debugger. That is no longer the trade — measured rather than assumed,
     * with the byte-bus clock modelled OpenBSD 7.9 GENERIC-IP27 reaches the
     * same prompt either way and attaches twenty devices against eight, a
     * strict superset with no bus error in the run.
     *
     * The option stays because that bring-up aid is still useful for
     * isolating a fault to the fabric or to what sits behind it. **It does not
     * gate widget 0**, which is aliased unconditionally: turning the option off
     * must not stop widget 0 answering, or a bring-up aid becomes a machine
     * OpenBSD panics on.
     */
    bool io_bridge_present;

    /*
     * Whether the junk-bus UART is fitted; see the class notes. Off by
     * default, because a shipped Origin 200 has no such part and because
     * real firmware treats it as the highest-priority console candidate —
     * fitting it unasked would hide the two real candidates behind it.
     */
    bool junk_uart_present;

    /*
     * Whether the firmware's boot-progress indicator is shown; see the class
     * notes. On by default, because it is the only thing this machine prints
     * while the firmware is still deciding which console to talk to.
     */
    bool debug_leds;

    /*
     * Whether this machine presents firmware of its own; see the class notes.
     */
    bool synthesized_firmware;

    /* Physical switch mask returned through each node's ELSC. */
    uint8_t elsc_dip_switches;

    /* Where the firmware's console output goes. */
    Chardev *console;

    /*
     * The PCI devices on the I/O bridge's bus whose resources firmware
     * assigns after reset (task #43): the base I/O controller and the two
     * SCSI adapters. Kept here because reset is not realize — a device's own
     * reset clears the state a realize-time assignment would have set, so
     * the assignment has to run again every time the machine resets, and it
     * needs these to still have somewhere to reach the devices from.
     */
    PCIDevice *pci_ioc3[SN0_ORIGIN200_MAX_NODES];
    PCIDevice *pci_scsi[SN0_ORIGIN200_MAX_NODES][SN0_SCSI_ADAPTERS];

    /*
     * Candidate values: hardware facts this machine asserts without having
     * observed them. They are settable so that two runs can differ in one value
     * and nothing else — a candidate that takes a rebuild to change makes every
     * comparison a comparison of two emulators as well as two values, and a
     * difference then cannot be attributed to either.
     *
     * A value belongs here when it is derived rather than captured. Moving one
     * in is how we say we are not sure; moving one out, to a constant, is how
     * we
     * say an observation settled it.
     */
    uint32_t widget0_revision;

};

typedef struct SN0LaunchData {
    uint64_t call_addr;
    uint64_t call_parm;
    uint64_t stack_addr;
    uint64_t gp_addr;
} SN0LaunchData;

static void sn0_mode_bits_reset(void *opaque);

typedef struct SN0MachineState SN0MachineState;

DECLARE_INSTANCE_CHECKER(SN0MachineState, SN0_MACHINE,
                         TYPE_SN0_MACHINE)

static void sn0_diagnostic_invalidate_reads(SN0MachineState *s,
                                            uint32_t node)
{
    SN0DiagnosticReadCacheEntry *cache;

    if (!s->diagnostic_read_cache) {
        return;
    }
    cache = &s->diagnostic_read_cache[
        node * SN0_DIAGNOSTIC_READ_CACHE_COUNT];
    memset(cache, 0, sizeof(*cache) * SN0_DIAGNOSTIC_READ_CACHE_COUNT);
    s->diagnostic_read_cache_next[node] = 0;
}

static bool sn0_diagnostic_read_repeated(SN0MachineState *s, uint32_t node,
                                         const char *component,
                                         uint64_t address, uint64_t value,
                                         unsigned int size,
                                         bool has_cpu_index, int cpu_index,
                                         bool has_pc, uint64_t pc,
                                         bool has_before_after,
                                         uint64_t before, uint64_t after)
{
    SN0DiagnosticReadCacheEntry *cache;
    SN0DiagnosticReadCacheEntry *entry = NULL;
    uint64_t oldest_sequence = s->diagnostic_event_next_sequence -
                               s->diagnostic_event_count;
    unsigned int i;

    if (!s->diagnostic_read_cache) {
        s->diagnostic_read_cache = g_new0(
            SN0DiagnosticReadCacheEntry,
            SN0_ORIGIN200_MAX_NODES * SN0_DIAGNOSTIC_READ_CACHE_COUNT);
    }
    cache = &s->diagnostic_read_cache[
        node * SN0_DIAGNOSTIC_READ_CACHE_COUNT];
    for (i = 0; i < SN0_DIAGNOSTIC_READ_CACHE_COUNT; i++) {
        if (cache[i].valid && cache[i].address == address &&
            cache[i].size == size &&
            cache[i].has_cpu_index == has_cpu_index &&
            (!has_cpu_index || cache[i].cpu_index == cpu_index) &&
            cache[i].has_pc == has_pc &&
            (!has_pc || cache[i].pc == pc) &&
            !strcmp(cache[i].component, component)) {
            entry = &cache[i];
            break;
        }
    }
    if (!entry) {
        entry = &cache[s->diagnostic_read_cache_next[node]++];
        s->diagnostic_read_cache_next[node] %=
            SN0_DIAGNOSTIC_READ_CACHE_COUNT;
    } else if (entry->value == value &&
               entry->has_before_after == has_before_after &&
               (!has_before_after ||
                (entry->before == before && entry->after == after)) &&
               entry->sequence >= oldest_sequence) {
        return true;
    }

    *entry = (SN0DiagnosticReadCacheEntry) {
        .component = component,
        .address = address,
        .value = value,
        .before = before,
        .after = after,
        .pc = pc,
        .sequence = s->diagnostic_event_next_sequence,
        .cpu_index = cpu_index,
        .size = size,
        .has_cpu_index = has_cpu_index,
        .has_pc = has_pc,
        .has_before_after = has_before_after,
        .valid = true,
    };
    return false;
}

static void sn0_diagnostic_record(void *opaque, const char *component,
                                  const char *operation, uint64_t address,
                                  uint64_t value, unsigned int size,
                                  bool has_before_after, uint64_t before,
                                  uint64_t after)
{
    SN0DiagnosticSinkContext *context = opaque;
    SN0MachineState *s = context->machine;
    SN0DiagnosticEvent *event;
    CPUState *cpu;
    bool has_cpu_index;
    int cpu_index;
    bool has_pc;
    uint64_t pc;

    if (unlikely(!s->diagnostic_recording)) {
        return;
    }
    assert(bql_locked());
    cpu = current_cpu;
    has_cpu_index = cpu != NULL;
    cpu_index = has_cpu_index ? cpu->cpu_index : 0;
    has_pc = cpu != NULL;
    pc = has_pc ? cpu->cc->get_pc(cpu) : 0;
    if (!strcmp(operation, "mmio-read")) {
        if (sn0_diagnostic_read_repeated(s, context->node, component,
                                         address, value, size,
                                         has_cpu_index, cpu_index, has_pc, pc,
                                         has_before_after, before, after)) {
            return;
        }
    } else {
        sn0_diagnostic_invalidate_reads(s, context->node);
    }
    if (!s->diagnostic_events) {
        s->diagnostic_events = g_new0(SN0DiagnosticEvent,
                                      SN0_DIAGNOSTIC_EVENT_COUNT);
    }
    event = &s->diagnostic_events[s->diagnostic_event_head];

    event->sequence = s->diagnostic_event_next_sequence++;
    event->virtual_time_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    event->node = context->node;
    event->component = component;
    event->operation = operation;
    event->address = address;
    event->value = value;
    event->size = size;
    if (has_cpu_index) {
        event->has_cpu_index = true;
        event->cpu_index = cpu_index;
    } else {
        event->has_cpu_index = false;
    }
    if (has_pc) {
        event->has_pc = true;
        event->pc = pc;
    } else {
        event->has_pc = false;
    }
    event->has_before_after = has_before_after;
    event->before = before;
    event->after = after;

    s->diagnostic_event_head =
        (s->diagnostic_event_head + 1) % SN0_DIAGNOSTIC_EVENT_COUNT;
    if (s->diagnostic_event_count < SN0_DIAGNOSTIC_EVENT_COUNT) {
        s->diagnostic_event_count++;
    } else {
        s->diagnostic_event_dropped++;
    }
}

static void sn0_diagnostic_reset(void *opaque)
{
    SN0MachineState *s = opaque;

    if (!s->diagnostic_recording) {
        return;
    }
    s->diagnostic_event_head = 0;
    s->diagnostic_event_count = 0;
    s->diagnostic_event_next_sequence = 0;
    s->diagnostic_event_dropped = 0;
    for (unsigned int i = 0; i < SN0_ORIGIN200_MAX_NODES; i++) {
        sn0_diagnostic_invalidate_reads(s, i);
    }
}

SgiOriginState *sn0_query_origin_state(uint32_t event_limit, Error **errp)
{
    BQL_LOCK_GUARD();
    MachineState *machine = MACHINE(qdev_get_machine());
    SN0MachineState *s;
    SgiOriginState *result;
    SgiOriginCpuStateList **cpu_tail;
    SgiOriginHubStateList **hub_tail;
    unsigned int node_count;
    CPUState *cs;
    unsigned int i;

    if (!object_dynamic_cast(OBJECT(machine), TYPE_SN0_MACHINE)) {
        error_setg(errp, "current machine is not an SGI Origin");
        return NULL;
    }
    s = SN0_MACHINE(machine);
    result = g_new0(SgiOriginState, 1);
    result->query_sequence = ++s->diagnostic_query_sequence;

    cpu_tail = &result->cpus;
    CPU_FOREACH(cs) {
        SgiOriginCpuState *cpu = g_new0(SgiOriginCpuState, 1);

        cpu->index = cs->cpu_index;
        cpu->interrupt_request = qatomic_load_acquire(&cs->interrupt_request);
        QAPI_LIST_APPEND(cpu_tail, cpu);
    }

    node_count = machine->numa_state && machine->numa_state->num_nodes ?
                 machine->numa_state->num_nodes : 1;
    hub_tail = &result->hubs;
    for (i = 0; i < node_count; i++) {
        SGIHubState *hub = &s->nodes[i].hub;
        PCF8584State *pcf = &hub->i2c;
        SGIElscState *elsc = hub->elsc;
        SgiOriginHubState *info = g_new0(SgiOriginHubState, 1);

        info->node = i;
        info->nasid = sgi_hub_programmed_nasid(hub);
        info->cpu_a_enabled = hub->pi_cpu_enable[0];
        info->cpu_b_enabled = hub->pi_cpu_enable[1];

        info->pending0 = qatomic_read(&hub->pi_int_pend[0]);
        info->pending1 = qatomic_read(&hub->pi_int_pend[1]);
        info->mask_a0 = hub->pi_int_mask[0];
        info->mask_a1 = hub->pi_int_mask[1];
        info->mask_b0 = hub->pi_int_mask[2];
        info->mask_b1 = hub->pi_int_mask[3];
        info->rt_compare_a = qatomic_read(&hub->pi_rt_compare[0]);
        info->rt_compare_b = qatomic_read(&hub->pi_rt_compare[1]);
        info->rt_pending_a = hub->pi_rt_pending[0];
        info->rt_pending_b = hub->pi_rt_pending[1];
        info->rt_enabled_a = hub->pi_rt_enable[0];
        info->rt_enabled_b = hub->pi_rt_enable[1];

        info->pcf8584 = g_new0(SgiOriginPcf8584State, 1);
        info->pcf8584->data = pcf->data;
        info->pcf8584->control = pcf->control;
        info->pcf8584->status = pcf->status;
        info->pcf8584->transfer = pcf->transfer;
        info->pcf8584->receiving = pcf->recv;
        info->pcf8584->start_pending = pcf->start_pending;
        info->pcf8584->address_staged = pcf->start_address_staged;
        info->pcf8584->address = pcf->cur_addr7;

        info->elsc = g_new0(SgiOriginElscState, 1);
        if (elsc) {
            info->elsc->rx_length = elsc->rxlen;
            info->elsc->tx_length = elsc->txlen;
            info->elsc->tx_position = elsc->txpos;
            info->elsc->arbitration_code = elsc->arb_code;
            info->elsc->arbitration_present = elsc->arb_present;
            info->elsc->queue_length = elsc->rx_queue_len;
        }

        QAPI_LIST_APPEND(hub_tail, info);
    }

    result->flight_recorder = g_new0(SgiOriginFlightRecorder, 1);
    result->flight_recorder->enabled = s->diagnostic_recording;
    result->flight_recorder->next_sequence =
        s->diagnostic_event_next_sequence;
    result->flight_recorder->dropped = s->diagnostic_event_dropped;
    {
        SgiOriginFlightEventList **event_tail =
            &result->flight_recorder->events;
        uint32_t retained = MIN(s->diagnostic_event_count, event_limit);
        uint32_t oldest =
            (s->diagnostic_event_head + SN0_DIAGNOSTIC_EVENT_COUNT -
             retained) % SN0_DIAGNOSTIC_EVENT_COUNT;

        for (i = 0; i < retained; i++) {
            SN0DiagnosticEvent *source = &s->diagnostic_events[
                (oldest + i) % SN0_DIAGNOSTIC_EVENT_COUNT];
            SgiOriginFlightEvent *event =
                g_new0(SgiOriginFlightEvent, 1);

            event->sequence = source->sequence;
            event->virtual_time_ns = source->virtual_time_ns;
            event->node = source->node;
            event->component = g_strdup(source->component);
            event->operation = g_strdup(source->operation);
            event->address = source->address;
            event->value = source->value;
            event->size = source->size;
            event->has_cpu_index = source->has_cpu_index;
            event->cpu_index = source->cpu_index;
            event->has_pc = source->has_pc;
            event->pc = source->pc;
            event->has_before = source->has_before_after;
            event->before = source->before;
            event->has_after = source->has_before_after;
            event->after = source->after;
            QAPI_LIST_APPEND(event_tail, event);
        }
    }

    return result;
}

/* AMD 29F080 command addresses and geometry. */
#define SN0_FLASH_CMD_ADDR1   0x5555
#define SN0_FLASH_CMD_ADDR2   0x2aaa
#define SN0_FLASH_SECTOR_SIZE (64 * KiB)
#define SN0_PROMLOG_SECTOR    14
#define SN0_PROMLOG_MAGIC     0x504c4f47
#define SN0_PROMLOG_VERSION   1

static bool sn0_promdata_is_private(SN0PromDataState *s, MemTxAttrs attrs)
{
    CPUMIPSState *env;
    uint32_t pc;

    if (!current_cpu || current_cpu != CPU(s->cpu)) {
        return false;
    }

    if (attrs.mips_uncached) {
        return false;
    }

    env = &s->cpu->env;
    pc = env->active_tc.PC;
    /* The PROM switches between its physical and 32-bit KSEG aliases. */
    if (pc >= 0x80000000 && pc < 0xc0000000) {
        pc &= 0x1fffffff;
    }

    /*
     * Before memory is initialized, the IP27 PROM marks this 32 KiB range
     * dirty-exclusive in each R10000's primary data cache and uses it for its
     * C stack and local data. FPR 0 is the PROM's boot-status register; DEX
     * records that cache-only phase. Restrict the convention to code running
     * from either the boot PROM or the RAM copy to which it transfers before
     * installing the final mapped alias. Later software can then use FPR 0
     * and physical memory at 1 MiB normally.
     */
    return (env->active_fpu.fpr[0].d & SN0_PROM_BSR_DEX) &&
           ((pc >= SN0_FLASH_LOCAL &&
             pc < SN0_FLASH_LOCAL + SN0_FLASH_SIZE) ||
            (pc >= SN0_PROM_COPY_PADDR &&
             pc < SN0_PROM_COPY_PADDR + SN0_FLASH_SIZE));
}

static MemTxResult sn0_promdata_read(void *opaque, hwaddr addr, uint64_t *val,
                                     unsigned size, MemTxAttrs attrs)
{
    SN0PromDataState *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;

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
}

static uint64_t sn0_flash_load(SN0NodeState *node, hwaddr addr, unsigned size)
{
    const uint8_t *p = memory_region_get_ram_ptr(&node->flash) +
                       (addr & (SN0_FLASH_SIZE - 1));
    uint8_t id[8];
    unsigned int i;

    if (node->flash_state == SN0_FLASH_AUTOSELECT) {
        for (i = 0; i < size; i++) {
            switch ((addr + i) & (SN0_FLASH_SIZE - 1)) {
            case 0:
                id[i] = 0x01; /* AMD */
                break;
            case 1:
                id[i] = 0xd5; /* Am29F080 */
                break;
            default:
                id[i] = 0xff;
                break;
            }
        }
        p = id;
    }

    switch (size) {
    case 1:
        return ldub_p(p);
    case 2:
        return lduw_be_p(p);
    case 4:
        return ldl_be_p(p);
    case 8:
        return ldq_be_p(p);
    default:
        g_assert_not_reached();
    }
}

static void sn0_flash_read_mode(SN0NodeState *node)
{
    node->flash_state = SN0_FLASH_READ;
    memory_region_rom_device_set_romd(&node->flash, true);
}

static void sn0_flash_reset(void *opaque)
{
    sn0_flash_read_mode(opaque);
}

static int sn0_flash_post_load(void *opaque, int version_id)
{
    SN0NodeState *node = opaque;

    memory_region_rom_device_set_romd(
        &node->flash, node->flash_state != SN0_FLASH_AUTOSELECT);
    return 0;
}

static const VMStateDescription vmstate_sn0_flash = {
    .name = "sn0-flash",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sn0_flash_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(flash_state, SN0NodeState),
        VMSTATE_END_OF_LIST()
    }
};

static void sn0_flash_command(SN0NodeState *node, hwaddr addr, uint64_t val,
                              unsigned size)
{
    uint32_t offset;
    uint8_t byte = val;
    uint8_t *p;

    /* The Hub exposes the byte-wide command port only on 64-bit writes. */
    if (size != 8) {
        return;
    }

    offset = (addr / 8) & (SN0_FLASH_SIZE - 1);

    if (byte == 0xf0 && node->flash_state != SN0_FLASH_PROGRAM) {
        sn0_flash_read_mode(node);
        return;
    }

    switch (node->flash_state) {
    case SN0_FLASH_READ:
        node->flash_state = offset == SN0_FLASH_CMD_ADDR1 && byte == 0xaa ?
                            SN0_FLASH_UNLOCK1 : SN0_FLASH_READ;
        break;
    case SN0_FLASH_UNLOCK1:
        node->flash_state = offset == SN0_FLASH_CMD_ADDR2 && byte == 0x55 ?
                            SN0_FLASH_UNLOCK2 : SN0_FLASH_READ;
        break;
    case SN0_FLASH_UNLOCK2:
        if (offset != SN0_FLASH_CMD_ADDR1) {
            node->flash_state = SN0_FLASH_READ;
        } else if (byte == 0x90) {
            node->flash_state = SN0_FLASH_AUTOSELECT;
            memory_region_rom_device_set_romd(&node->flash, false);
        } else if (byte == 0xa0) {
            node->flash_state = SN0_FLASH_PROGRAM;
        } else if (byte == 0x80) {
            node->flash_state = SN0_FLASH_ERASE_UNLOCK1;
        } else {
            node->flash_state = SN0_FLASH_READ;
        }
        break;
    case SN0_FLASH_AUTOSELECT:
        break;
    case SN0_FLASH_PROGRAM:
        p = memory_region_get_ram_ptr(&node->flash);
        p[offset] &= byte;
        memory_region_set_dirty(&node->flash, offset, 1);
        sn0_flash_read_mode(node);
        break;
    case SN0_FLASH_ERASE_UNLOCK1:
        node->flash_state = offset == SN0_FLASH_CMD_ADDR1 && byte == 0xaa ?
                            SN0_FLASH_ERASE_UNLOCK2 : SN0_FLASH_READ;
        break;
    case SN0_FLASH_ERASE_UNLOCK2:
        node->flash_state = offset == SN0_FLASH_CMD_ADDR2 && byte == 0x55 ?
                            SN0_FLASH_ERASE_SELECT : SN0_FLASH_READ;
        break;
    case SN0_FLASH_ERASE_SELECT:
        if (byte == 0x30) {
            offset &= ~(SN0_FLASH_SECTOR_SIZE - 1);
            p = memory_region_get_ram_ptr(&node->flash);
            memset(p + offset, 0xff, SN0_FLASH_SECTOR_SIZE);
            memory_region_set_dirty(&node->flash, offset,
                                    SN0_FLASH_SECTOR_SIZE);
        }
        sn0_flash_read_mode(node);
        break;
    default:
        g_assert_not_reached();
    }
}

static void sn0_flash_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    sn0_flash_command(opaque, addr, val, size);
}

static uint64_t sn0_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    return sn0_flash_load(opaque, addr, size);
}

static const MemoryRegionOps sn0_flash_ops = {
    .read = sn0_flash_read,
    .write = sn0_flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * The Hub special views of the boot flash, LBOOT and RBOOT, are each 256 MB
 * decoding a 1 MB chip. A real chip-select ignores the offset bits above its
 * own size, so the chip's contents repeat every 1 MB across either window.
 * Expressed here as the decode dropping those bits rather than as 256 separate
 * aliases naming the same repeat, because that is what the hardware does and
 * a guest's own code genuinely lands on offsets past the first repeat.
 *
 * The mirror calls the same flash state machine as the reset view. Ordinary
 * reads come from the shared backing store; autoselect reads return the chip
 * identity instead.
 */
static uint64_t sn0_boot_mirror_read(void *opaque, hwaddr addr, unsigned size)
{
    return sn0_flash_load(opaque, addr, size);
}

static void sn0_boot_mirror_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    sn0_flash_command(opaque, addr, val, size);
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
 * The floor beneath a back-door window: what a directory or ECC entry for a
 * bank this configuration does not populate answers. Real firmware's presence
 * probe writes a test pattern, writes zero through the same data lines, and
 * reads back; it calls a bank absent exactly when neither value survives, so
 * this must accept both writes and echo neither.
 *
 * The value returned is a modelling choice, not a fact about the machine --
 * the vendor source says an absent bank's data lines float, so nothing
 * architectural constrains the answer beyond "not zero and not whatever
 * pattern the probe wrote." 0xa5 repeated is arbitrary, chosen only to be
 * visually distinct from either all-zero or an all-ones fill in a trace.
 *
 * impl sizes are declared explicitly (matching valid) so an eight-byte guest
 * access reaches this callback once. Left unset, the maximum defaults to
 * four and the memory core would split it into two four-byte calls -- and
 * because both halves resolve to the same fixed value, nothing about that
 * split would show up as wrong here the way it would on a register with a
 * side effect. Declared anyway, on the general rule rather than because
 * this specific handler would misbehave without it.
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
    /*
     * Accepted and dropped. A bank that is not there has nothing behind
     * this address for the write to change, which is the whole point --
     * and real firmware's own sizing walk runs with no fault guard here,
     * so refusing or faulting the write would break the one routine that
     * has nothing to catch it.
     */
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
 * A populated bank's own share of a back-door window -- real storage, but
 * only as wide as this machine's directory format actually is. SGI's own
 * manual states the Origin 200 carries no premium directory memory, the
 * additional hardware a larger machine has; ours is the plainer format
 * throughout. Real firmware's presence probe tells the two apart by how
 * much of a written pattern survives a read: the richer format's entry
 * keeps all six bytes of its low 48 bits, the plainer format's keeps only
 * its low two. A backing store that remembers every byte it is given, as
 * plain RAM does, keeps all of them regardless -- which is what made every
 * populated bank here answer "premium format" whether or not the machine
 * has any.
 *
 * So this remembers everything a guest writes, to whatever width and
 * alignment it writes it, but a read reveals only the low sixteen bits of
 * each eight-byte-aligned entry; the other six always come back as zero,
 * never as what was stored there, because the hardware has no storage
 * there to come back from. Writing the discarded bytes is accepted rather
 * than refused -- real firmware's own restore step (the probe saves an
 * entry, overwrites it, classifies it, then writes the saved value back)
 * writes all eight, and a plainer-format entry's real two bytes have to
 * survive that restore correctly, which this does; the other six are
 * harmless to store since nothing ever reads them back.
 *
 * Back-door addresses are ordinary memory addresses shifted right by two.
 * A bank therefore has one quarter as much unique back-door storage as RAM,
 * repeated across its fixed 128 MiB share of the window. Firmware uses that
 * aliasing to distinguish bank capacities. Keeping only the unique span also
 * prevents a default 64 MiB guest from allocating 256 MiB of mostly redundant
 * diagnostic backing across these two windows.
 *
 * The compact buffer is used instead of memory_region_init_ram because both
 * the address folding and the width mask have to happen on every access. It
 * has machine lifetime, like the flash and the other back-door pieces above,
 * and does not currently migrate: this content is diagnostic scratch a guest
 * never relies on surviving a snapshot, so the gap is recorded here rather
 * than closed.
 */
static uint8_t *sn0_backdoor_byte(SN0BackdoorState *s, hwaddr addr)
{
    unsigned int bank = addr / SN0_BACKDOOR_BANK_STRIDE;
    uint64_t within_bank = addr % SN0_BACKDOOR_BANK_STRIDE;

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

    /*
     * Byte i of this access sits at addr + i and occupies bits
     * [8*(size-1-i), 8*(size-1-i)+7] of the big-endian value just read.
     * The low two bytes of each eight-byte-aligned entry -- (addr+i) & 7
     * equal to 6 or 7 -- are real; the rest read as zero.
     */
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
                                   const SN0NodeState *node)
{
    uint64_t total = 0;
    unsigned int bank;

    s->populated_banks = node->populated_banks;
    for (bank = 0; bank < node->populated_banks; bank++) {
        s->bank_offset[bank] = total;
        s->bank_span[bank] = (uint64_t)node->bank_mb[bank] * MiB / 4;
        total += s->bank_span[bank];
    }
    s->data = g_malloc0(total);
}

/*
 * Build what a programmed flash would hold at the addresses a guest reaches
 * before any firmware runs: the entry points it calls by address, and the
 * configuration block it reads the machine's identity and clocks out of.
 *
 * The entry points are branches. A guest calls nine of them expecting to come
 * back, so each returns; the tenth is the reset vector, which the processor
 * enters rather than calls, and which stops here because there is no firmware
 * to run yet. The firmware that fills these arrives with the boot structure
 * work.
 */
/*
 * Where an address in a kernel's headers puts its bytes. A kernel is linked
 * into the unmapped 64-bit window and its headers carry that address as both
 * the virtual and the physical one, so taking the physical address at face
 * value would place the image far outside any memory the machine has —
 * silently, and the guest then executes whatever the untouched memory at its
 * entry point holds.
 *
 * The translation is the processor's own: the physical address is the low forty
 * bits, and the cache algorithm above them is not part of it. An uncached
 * window keeps its attribute, so a kernel linked into one lands in that space
 * rather than in memory, for the same reason a processor reference to it would.
 */
static uint64_t sn0_elf_to_phys(void *opaque, uint64_t addr)
{
    if ((addr >> 62) == 2) {                    /* the unmapped 64-bit window */
        unsigned int cca = (addr >> 59) & 7;
        uint64_t pa = addr & ((1ULL << 40) - 1);

        if (cca == 2 || cca == 7) {     /* uncached, so it has an attribute */
            return SN0_SPACE((((addr >> 57) & 3) + 1) & 3) | pa;
        }
        return pa;
    }

    /* Both compatibility segments reach the bottom of node-local memory. */
    if (addr >= SN0_COMPAT_CACHED && addr < 0xffffffffc0000000ull) {
        return addr & 0x1fffffff;
    }

    return addr;
}

/*
 * Hand a loaded kernel control with the convention it expects: the argument
 * count in the first argument register and the vector in the second. A real
 * firmware reaches this point after all its own work; ours has none yet, so the
 * reset entry jumps straight here.
 *
 * The jump at the reset entry has to be two instructions, which is all the
 * eight bytes between entry points allow, so it is a region-relative jump
 * rather than an absolute one. Both addresses are in the same region of the
 * flash, so that reaches.
 */
/*
 * Point the real reset vector at the bootloader stub in fw. Every handoff
 * this machine builds shares this: whichever one wrote the stub, this is
 * what makes the CPU reach it out of reset. The jump has to be two
 * instructions, which is all the eight bytes between flash entry points
 * allow, so it is region-relative rather than absolute -- both addresses
 * are in the same region of the flash, so that reaches.
 */
static void sn0_flash_jump_to_bootloader(uint8_t *flash, hwaddr fw_vaddr)
{
    hwaddr target = fw_vaddr + SN0_FW_BOOTLOADER;

    stl_be_p(flash, 0x08000000 | ((target >> 2) & 0x03ffffff)); /* j target */
    stl_be_p(flash + 4, 0);                                     /* delay slot */
}

static void sn0_flash_jump_to_firmware(uint8_t *flash, hwaddr entry,
                                      hwaddr target)
{
    uint8_t *slot = flash + entry;

    stl_be_p(slot, 0x08000000 | ((target >> 2) & 0x03ffffff));
    stl_be_p(slot + 4, 0);
}

static void sn0_build_kernel_handoff(uint8_t *flash, uint8_t *fw,
                                     hwaddr fw_vaddr, uint64_t entry,
                                     unsigned argc, hwaddr argv)
{
    void *p = fw + SN0_FW_BOOTLOADER;

    sn0_flash_jump_to_bootloader(flash, fw_vaddr);

    /*
     * Hand over in 64-bit kernel mode. A kernel is linked into the unmapped
     * 64-bit window, so the jump to its entry point is itself a 64-bit
     * reference and takes an address error unless extended kernel addressing is
     * on, which it is not out of reset. Clearing error level at the same time
     * puts the user segment back under the translation buffer, and clearing the
     * boot-exception-vector bit points exceptions at the addresses a kernel is
     * about to install handlers at rather than into this flash.
     *
     * The register used is one the architecture reserves for kernel scratch, so
     * nothing a kernel expects on entry is disturbed.
     */
    stl_be_p(p, 0x341a0000 | (1 << CP0St_KX));  /* ori k0, zero, KX */
    p = (uint8_t *)p + 4;
    stl_be_p(p, 0x409a6000);                    /* mtc0 k0, CP0_Status */
    p = (uint8_t *)p + 4;

    bl_gen_jump_kernel(&p,
                       false, 0,          /* the kernel sets up its own stack */
                       true, argc,
                       true, argv,
                       false, 0,
                       false, 0,
                       entry);
}

/*
 * Which of the two loaders a -kernel file needs. An OpenBSD or Linux kernel
 * is ELF64; sashARCS is the one other format this machine loads directly,
 * told apart by its own file-format magic in the first two bytes.
 */
static bool sn0_kernel_file_is_ecoff(const char *filename)
{
    uint8_t magic[2];
    FILE *f = fopen(filename, "rb");
    bool is_ecoff;

    if (!f) {
        return false;
    }
    is_ecoff = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
              lduw_be_p(magic) == ECOFF_MAGIC_MIPSEB;
    fclose(f);
    return is_ecoff;
}

/*
 * Parse a genuine SGI sashARCS-format ECOFF image and place its sections in
 * guest memory. sashARCS links as a fixed-address OMAGIC executable with a
 * real entry point -- unlike its ELF64 sibling sash64, which is a
 * relocatable object with none -- so this is a plain "read each section to
 * its linked address, zero bss" load, the same shape load_elf already does
 * for a kernel. See docs/library/irix-sash-direct-load-proposal.md for the
 * mechanism this reproduces and why no CP0 or TLB setup belongs here.
 *
 * Returns the entry point, the o32 ABI gp-register value the image expects,
 * and the physical extent it occupies, for the same free-memory-descriptor
 * bookkeeping the ELF kernel path keeps.
 */
static void sn0_load_ecoff(const char *filename, hwaddr node,
                           uint64_t *entry, uint64_t *gp_value,
                           uint64_t *low, uint64_t *high)
{
    g_autofree uint8_t *buf = NULL;
    int64_t file_size;
    unsigned int nscns, opthdr, i;
    uint32_t scnhdr_base, aout_magic;

    file_size = get_image_size(filename, &error_fatal);
    if (file_size < ECOFF_FILEHDR_SIZE) {
        error_report("origin200: '%s' is too small to be an ECOFF image",
                     filename);
        exit(EXIT_FAILURE);
    }

    buf = g_malloc(file_size);
    if (load_image_size(filename, buf, file_size) != file_size) {
        error_report("origin200: cannot read '%s'", filename);
        exit(EXIT_FAILURE);
    }

    if (lduw_be_p(buf + ECOFF_FILEHDR_MAGIC_OFF) != ECOFF_MAGIC_MIPSEB) {
        error_report("origin200: '%s' is not a big-endian MIPS ECOFF image",
                     filename);
        exit(EXIT_FAILURE);
    }

    nscns = lduw_be_p(buf + ECOFF_FILEHDR_NSCNS_OFF);
    opthdr = lduw_be_p(buf + ECOFF_FILEHDR_OPTHDR_OFF);
    if (opthdr < ECOFF_AOUTHDR_GPVALUE_OFF + 4) {
        error_report("origin200: '%s' has too small an ECOFF optional "
                     "header", filename);
        exit(EXIT_FAILURE);
    }

    aout_magic = lduw_be_p(buf + ECOFF_FILEHDR_SIZE + ECOFF_AOUTHDR_MAGIC_OFF);
    if (aout_magic != ECOFF_AOUTHDR_OMAGIC) {
        error_report("origin200: '%s' is not an OMAGIC ECOFF executable "
                     "(sashARCS loads directly; its relocatable sibling "
                     "sash64 needs a linker, not this loader)", filename);
        exit(EXIT_FAILURE);
    }

    *entry = ldl_be_p(buf + ECOFF_FILEHDR_SIZE + ECOFF_AOUTHDR_ENTRY_OFF);
    *gp_value = ldl_be_p(buf + ECOFF_FILEHDR_SIZE + ECOFF_AOUTHDR_GPVALUE_OFF);
    *low = UINT64_MAX;
    *high = 0;

    scnhdr_base = ECOFF_FILEHDR_SIZE + opthdr;
    for (i = 0; i < nscns; i++) {
        uint8_t *scn = buf + scnhdr_base + i * ECOFF_SCNHDR_SIZE;
        uint32_t vaddr = ldl_be_p(scn + ECOFF_SCNHDR_VADDR_OFF);
        uint32_t size = ldl_be_p(scn + ECOFF_SCNHDR_SIZE_OFF);
        uint32_t scnptr = ldl_be_p(scn + ECOFF_SCNHDR_SCNPTR_OFF);
        g_autofree char *name = g_strdup_printf("sn0.sash.section%u", i);

        if (size == 0) {
            continue;
        }
        if (scnptr != 0) {
            /* Real data -- text, rdata, data. Copy it as the header says. */
            if (scnptr + (uint64_t)size > file_size) {
                error_report("origin200: '%s' section %u runs past the "
                             "end of the file", filename, i);
                exit(EXIT_FAILURE);
            }
            rom_add_blob_fixed(name, buf + scnptr, size, node + vaddr);
        } else {
            /* bss and its kin: zero-filled, nothing in the file to copy. */
            g_autofree uint8_t *zero = g_malloc0(size);

            rom_add_blob_fixed(name, zero, size, node + vaddr);
        }

        *low = MIN(*low, (uint64_t)vaddr);
        *high = MAX(*high, (uint64_t)vaddr + size);
    }

    if (*high == 0) {
        error_report("origin200: '%s' has no loadable sections", filename);
        exit(EXIT_FAILURE);
    }
}

/*
 * Hand a directly-loaded sashARCS the same registers the real ARCS
 * client_start routine hands a standalone program: argc/argv/environ in
 * a0-a2, a fresh stack in sp. Unlike sn0_build_kernel_handoff, this leaves
 * CP0 Status untouched -- the real handoff chain never sets it either, so
 * the CPU keeps whatever addressing mode it has out of reset, which is
 * unmapped kuseg: exactly what a 32-bit link address like sashARCS's needs
 * with no TLB entry at all. See
 * docs/library/irix-sash-direct-load-proposal.md.
 *
 * gp is not part of that handoff convention, but a 32-bit o32 ABI program
 * needs it correct before its first gp-relative load -- which its own
 * startup code makes with nothing else set up yet -- so it is loaded here
 * explicitly, from the value the image's own header carries.
 */
static void sn0_build_sash_handoff(uint8_t *flash, uint8_t *fw,
                                   hwaddr fw_vaddr, uint64_t entry,
                                   uint64_t gp_value, unsigned argc,
                                   hwaddr argv, hwaddr envp, hwaddr sp)
{
    void *p = fw + SN0_FW_BOOTLOADER;

    sn0_flash_jump_to_bootloader(flash, fw_vaddr);

    stl_be_p(p, 0x3c1c0000 | ((gp_value >> 16) & 0xffff)); /* lui gp, hi16 */
    p = (uint8_t *)p + 4;
    stl_be_p(p, 0x379c0000 | (gp_value & 0xffff));         /* ori gp, gp, lo16 */
    p = (uint8_t *)p + 4;

    bl_gen_jump_kernel(&p,
                       true, sp,
                       true, argc,
                       true, argv,
                       true, envp,
                       false, 0,
                       entry);
}

static void sn0_build_flash(uint8_t *flash)
{
    static const uint32_t jr_ra = 0x03e00008;      /* jr $31 */
    static const uint32_t branch_to_self = 0x1000ffff; /* b . */
    uint8_t *config = flash + SN0_CONFIG_OFFSET;
    int i;

    memset(flash, 0xff, SN0_FLASH_SIZE);

    for (i = 0; i < SN0_FLASH_ENTRIES; i++) {
        uint8_t *entry = flash + i * SN0_FLASH_ENTRY_STRIDE;

        stl_be_p(entry, i == 0 ? branch_to_self : jr_ra);
        stl_be_p(entry + 4, 0);                    /* the delay slot */
    }

    /*
     * The field order and widths are the IP27 firmware's. The release image is
     * built with Origin 2000 defaults because a real flasher preserves the
     * running node's configuration; this synthesized image instead carries
     * this product's Origin 200 values directly.
     */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, SN0_PROCESSOR_MODE_BITS);
    stq_be_p(config + 0x08, SN0_CONFIG_MAGIC);
    stq_be_p(config + 0x10, SN0_CPU_FREQ_HZ);
    stq_be_p(config + 0x18, SN0_HUB_FREQ_HZ);
    stq_be_p(config + 0x20, SN0_RTC_FREQ_HZ);
    stl_be_p(config + 0x28, 1);                    /* error correction on */
    stl_be_p(config + 0x2c, SN0_FPROM_CYC);        /* flash read timing */
    stl_be_p(config + 0x30, SN0_MACHINE_TYPE);
    stl_be_p(config + 0x34, 0);                    /* checksum adjustment */
    stl_be_p(config + 0x38, 0);                    /* flash program count */
    stl_be_p(config + 0x3c, SN0_FPROM_WR);         /* flash write timing */
    stl_be_p(config + 0x40, 6);                    /* firmware version */
    stl_be_p(config + 0x44, 11);                   /* firmware revision */
    stl_be_p(config + 0x48, 0);                    /* configuration flags */

    /*
     * A board leaves the factory with an empty, initialized environment log.
     * It is retained state outside the firmware payload; a complete 1 MiB
     * image supplied with -bios can still replace it along with everything
     * else. An erased first entry is the log's end marker.
     */
    config = flash + SN0_PROMLOG_SECTOR * SN0_FLASH_SECTOR_SIZE;
    stl_be_p(config + 0x10, SN0_PROMLOG_MAGIC);
    stl_be_p(config + 0x14, SN0_PROMLOG_VERSION);
    stl_be_p(config + 0x18, 1);                    /* initial sequence */
}

static uint8_t sn0_config_byte_sum(const uint8_t *config)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < SN0_CONFIG_SIZE; i++) {
        sum += config[i];
    }
    return sum;
}

/*
 * Install the product-specific part of an IP27 PROM configuration block.
 *
 * SGI ships a generic release image with Origin 2000 defaults. Its flasher
 * copies the running node's configuration into the new image and adjusts one
 * byte so the replacement block has the same byte sum as the original. QEMU
 * has no preceding physical PROM to preserve, but the concrete machine type
 * supplies the same three values: SN00 identity and its two flash timings.
 *
 * Preserve every other field supplied by the image, including clocks, CPU
 * mode, version, and flash count. Preserve the old block sum too, following
 * the flasher's observable contract. A diagnostic ROM without an IP27 config
 * signature remains byte-for-byte untouched.
 */
static void sn0_configure_flash_for_origin200(uint8_t *flash)
{
    uint8_t *config = flash + SN0_CONFIG_OFFSET;
    uint8_t old_sum;
    uint8_t new_sum;

    if (ldq_be_p(config + 0x08) != SN0_CONFIG_MAGIC) {
        return;
    }

    old_sum = sn0_config_byte_sum(config);
    stl_be_p(config + SN0_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + SN0_CONFIG_FPROM_CYC_OFFSET, SN0_FPROM_CYC);
    stl_be_p(config + SN0_CONFIG_MACHINE_TYPE_OFFSET, SN0_MACHINE_TYPE);
    stl_be_p(config + SN0_CONFIG_FPROM_WR_OFFSET, SN0_FPROM_WR);
    new_sum = sn0_config_byte_sum(config);
    stl_be_p(config + SN0_CONFIG_CHECKSUM_OFFSET,
             (uint8_t)(old_sum - new_sum));
}

/*
 * The firmware routines a guest calls. A stub in the flash performs one load
 * from the trap region and returns; this is what that load does, and its return
 * value is what the guest finds in its return register, because the translator
 * puts a loaded value there through the ordinary path. Reaching into the
 * processor state to leave a result instead would be writing behind the
 * translator's back, which it does not reload for.
 */
enum {
    ARCS_ESUCCESS = 0,
    ARCS_EINVAL = 21,
};

/*
 * ARCS Write(handle, buffer, length, count) — the routine all early output
 * uses.
 */
static uint64_t sn0_arcs_write(SN0MachineState *s, CPUMIPSState *env)
{
    uint64_t buffer = env->active_tc.gpr[5];
    uint64_t length = env->active_tc.gpr[6];
    uint64_t count_ptr = env->active_tc.gpr[7];
    uint32_t written = 0;
    uint8_t chunk[256];

    while (written < length) {
        unsigned int n = MIN(sizeof(chunk), length - written);

        /*
         * The buffer is a guest virtual address, so it goes through the
         * processor's own translation rather than being taken apart here: a
         * pointer into a translated page would otherwise be decoded wrongly and
         * silently.
         */
        if (cpu_memory_rw_debug(env_cpu(env), buffer + written, chunk, n,
                                false) < 0) {
            break;
        }
        if (s->console) {
            qemu_chr_write_all(s->console, chunk, n);
        }
        written += n;
    }

    /*
     * The guest is told how much went out, whether or not anyone was listening.
     */
    if (count_ptr) {
        uint32_t be = cpu_to_be32(written);

        cpu_memory_rw_debug(env_cpu(env), count_ptr, &be, sizeof(be), true);
    }

    return ARCS_ESUCCESS;
}

/*
 * The memory descriptors a guest walks to learn what memory it has. Two things
 * about this list are load-bearing and neither is obvious.
 *
 * The base page of the first free entry is how a guest tells this machine from
 * its successor. At or above page 0x20 it silently decides it is the later
 * machine and takes a different path for the rest of the boot, with no message,
 * so first free memory has to start below that. Ours starts at page 0x19, which
 * is where a real machine of this generation puts it.
 *
 * The walk stops at the first firmware-permanent entry and the guest rewrites
 * that entry to reach the 32 MB mark, so nothing after it is ever read. The
 * shape here follows a real machine's: pages 2 through 0x18 are described by
 * nothing at all, which is what the real list does.
 */
enum {
    SN0_MEM_EXCEPTION_BLOCK = 0,
    SN0_MEM_PARAM_BLOCK = 1,
    SN0_MEM_FREE = 2,
    SN0_MEM_FIRMWARE_TEMP = 5,
    SN0_MEM_FIRMWARE_PERMANENT = 7,
    SN0_MEM_LOADED_PROGRAM = 6,
};

#define SN0_MEM_FIRST_FREE_PAGE  0x19    /* must stay below 0x20 */
#define SN0_MEM_PERMANENT_PAGE   0x1a00
#define SN0_MEM_PERMANENT_PAGES  0x600

struct sn0_mem_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t base_page;
    uint64_t pages;
};

/*
 * ARCS GetMemoryDescriptor(previous) — called first with nothing and then
 * with each descriptor it returned, so it walks a list. The list is built once
 * and lives in the trap region's own storage, because a guest holds the pointer
 * we hand back and reads through it.
 */
static uint64_t sn0_arcs_get_memory(SN0MachineState *s, CPUMIPSState *env)
{
    uint64_t previous = env->active_tc.gpr[4];
    unsigned int next = 0;

    if (previous) {
        unsigned int i;

        /* Which one did we hand out last? */
        for (i = 0; i < s->nodes[0].mem_count; i++) {
            if (previous == s->nodes[0].mem_addr +
                            i * sizeof(struct sn0_mem_descriptor)) {
                next = i + 1;
                break;
            }
        }
        if (i == s->nodes[0].mem_count) {
            return 0;
        }
    }

    if (next >= s->nodes[0].mem_count) {
        return 0;                       /* the end of the list */
    }
    return s->nodes[0].mem_addr + next * sizeof(struct sn0_mem_descriptor);
}

/*
 * Enter a processor that the synthesized firmware kept in its slave loop.
 * The real PROM changes to 64-bit kernel addressing before calling the
 * requested routine. Start at our equivalent trampoline so the target makes
 * that architectural transition itself rather than having QEMU alter CP0
 * state behind its back.
 */
static void sn0_launch_cpu(CPUState *cs, run_on_cpu_data data)
{
    SN0LaunchData *launch = data.host_ptr;
    CPUMIPSState *env = cpu_env(cs);

    cpu_reset(cs);
    sn0_mode_bits_reset(cs);
    env->active_tc.PC = (int32_t)(0xa0000000 + SN0_FW_TEXT_LOCAL +
                                  SN0_FW_SECONDARY_BOOT);
    env->active_tc.gpr[27] = launch->call_addr;
    env->active_tc.gpr[4] = launch->call_parm;
    if (launch->stack_addr) {
        env->active_tc.gpr[29] = launch->stack_addr;
    }
    if (launch->gp_addr) {
        env->active_tc.gpr[28] = launch->gp_addr;
    }
    cs->halted = 0;
    cs->exception_index = -1;
    g_free(launch);
}

static uint64_t sn0_firmware_launch(SN0MachineState *s, CPUMIPSState *env)
{
    unsigned int nasid = env->active_tc.gpr[4];
    unsigned int slice = env->active_tc.gpr[5];
    SN0NodeState *node = NULL;
    SN0LaunchData *launch;
    CPUState *target;
    unsigned int i;

    for (i = 0; i < SN0_ORIGIN200_MAX_NODES; i++) {
        if (s->nodes[i].cpu[0] && s->nodes[i].nasid == nasid) {
            node = &s->nodes[i];
            break;
        }
    }
    if (!node || slice >= SN0_CPUS_PER_NODE || !node->cpu[slice]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn0: cannot launch processor %u on NASID %u\n",
                      slice, nasid);
        return ARCS_EINVAL;
    }

    target = CPU(node->cpu[slice]);
    if (target == current_cpu) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn0: processor tried to launch itself\n");
        return ARCS_EINVAL;
    }

    launch = g_new(SN0LaunchData, 1);
    launch->call_addr = env->active_tc.gpr[6];
    launch->call_parm = env->active_tc.gpr[7];
    launch->stack_addr = env->active_tc.gpr[8];
    launch->gp_addr = env->active_tc.gpr[9];

    /*
     * async_safe_run_on_cpu(), not async_run_on_cpu(): sn0_launch_cpu()
     * calls cpu_reset(), which bulk-rewrites the target's architectural
     * CPU state. Queueing that under async_safe_run_on_cpu() runs it
     * inside start_exclusive()'s quiescent section, which upstream
     * relies on to keep a live peer's BQL-free read of that state (see
     * mips_vp_active()/mips_vpe_active() in target/mips/cpu.c) from
     * overlapping the reset. R10000 carries none of the CP0 Config bits
     * those readers require, so no peer on this machine reaches them
     * today -- but the target CPU state cpu_reset() rewrites is not
     * scoped to what happens to be reachable here, and async_run_on_cpu()
     * would leave the same class of race live in this dispatch that
     * "hw/misc/mips_cpc: quiesce peers for the VP-run reset" closed in
     * the CPC's.
     */
    async_safe_run_on_cpu(target, sn0_launch_cpu,
                          RUN_ON_CPU_HOST_PTR(launch));
    return ARCS_ESUCCESS;
}

static MemTxResult sn0_arcs_read(void *opaque, hwaddr addr, uint64_t *val,
                                 unsigned size, MemTxAttrs attrs)
{
    SN0MachineState *s = opaque;
    unsigned int op = addr / 8;

    *val = ARCS_EINVAL;

    /*
     * A debugger examining memory must not make a firmware call happen as a
     * side effect of looking at it.
     */
    if (attrs.debug) {
        *val = 0;
        return MEMTX_OK;
    }

    /*
     * Only the two routines below this point read the calling processor's
     * registers, so only they need a processor to exist — "no processor at
     * all" is a state a test harness can produce, and every other routine
     * here answers the same constant regardless of who is asking, so there
     * is no reason to fail those too.
     */
    switch (op) {
    case SN0_ARCS_WRITE:
    case SN0_ARCS_GET_MEMORY:
    case SN0_FIRMWARE_LAUNCH_OP:
        if (!current_cpu) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sn0: firmware routine %u reached with no "
                          "processor\n", op);
            break;
        }
        if (op == SN0_ARCS_WRITE) {
            *val = sn0_arcs_write(s, cpu_env(current_cpu));
        } else if (op == SN0_ARCS_GET_MEMORY) {
            *val = sn0_arcs_get_memory(s, cpu_env(current_cpu));
        } else {
            *val = sn0_firmware_launch(s, cpu_env(current_cpu));
        }
        break;
    case SN0_ARCS_GET_CHILD:
        /*
         * Null, which a guest handles: it falls back to the hardware
         * directory's configuration-graph signature to identify the machine,
         * and that path needs no component tree at all.
         */
        *val = 0;
        break;
    case SN0_ARCS_GET_SYSTEM_ID:
        /*
         * Null too. A guest reads the vendor and product strings out of this
         * only if identification has already failed, and on our path it does
         * not.
         */
        *val = 0;
        break;
    case SN0_ARCS_GET_ENV:
        /*
         * Null for every variable. A guest checks each result before using it,
         * and the one that matters — the console rate — falls back to 9600.
         */
        *val = 0;
        break;
    case SN0_ARCS_CAPABILITY_OP:
        /*
         * Level three: the firmware can load a relocatable object. Not on
         * the banner path — the guests we boot never call the private
         * vector at all — but a caller that does must not be told a lower
         * level and silently take a different, unsupported path.
         */
        *val = SN0_ARCS_CAPABILITY_LEVEL;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sn0: firmware routine %u is not implemented\n", op);
        break;
    }

    return MEMTX_OK;
}

static MemTxResult sn0_arcs_write_mmio(void *opaque, hwaddr addr, uint64_t val,
                                       unsigned size, MemTxAttrs attrs)
{
    if (!attrs.debug) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn0: write of 0x%" PRIx64 " to the firmware trap region "
                      "at 0x%" HWADDR_PRIx "; the routines are called by "
                      "reading\n", val, addr);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps sn0_arcs_ops = {
    .read_with_attrs = sn0_arcs_read,
    .write_with_attrs = sn0_arcs_write_mmio,
    .endianness = DEVICE_BIG_ENDIAN,
    /*
     * Eight bytes exactly, both as what a guest may ask for and as what this
     * implements. Leaving the implementation width unstated would let the
     * memory system split a doubleword load into two word accesses and call the
     * routine twice — which is invisible to the caller, because the second
     * call overwrites the first's result with the same value, and shows up only
     * as every line of a guest's early output appearing twice.
     */
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

/*
 * One stub per vector entry, and the vector that points at them. A stub forms
 * the address of its own slot in the trap region, loads from it, and returns;
 * the assembler temporary carries the address because it is reserved and cannot
 * be live across a call, and it is clear of the registers holding the arguments
 * the routine is about to read.
 *
 * `stub_index` is where in the shared stub area this vector's stubs start,
 * and `op_base` is the trap-page slot — and so the dispatcher's op number —
 * the first entry lands on. The firmware call vector and the private vector
 * share one stub area and one trap page, each continuing where the other
 * leaves off, so the two never collide and the dispatcher tells them apart
 * by op number alone.
 */
static void sn0_build_trap_stub(uint8_t *stub, uint64_t slot)
{
    stl_be_p(stub, 0x3c010000 | ((slot >> 16) & 0xffff)); /* lui $1, hi */
    stl_be_p(stub + 4, 0xdc220000 | (slot & 0xffff));  /* ld $2, lo($1) */
    stl_be_p(stub + 8, 0x03e00008);                       /* jr $31 */
    stl_be_p(stub + 12, 0);                               /* delay slot */
}

static void sn0_build_arcs(uint8_t *fw, hwaddr fw_vaddr, hwaddr trap_vaddr,
                           uint64_t *vector, unsigned int entries,
                           unsigned int stub_index, unsigned int op_base)
{
    unsigned int i;

    for (i = 0; i < entries; i++) {
        uint8_t *stub = fw + SN0_ARCS_STUBS +
                        (stub_index + i) * SN0_ARCS_STUB_SIZE;
        uint64_t slot = trap_vaddr + (op_base + i) * 8;

        sn0_build_trap_stub(stub, slot);

        vector[i] = cpu_to_be64(fw_vaddr + SN0_ARCS_STUBS +
                                (stub_index + i) * SN0_ARCS_STUB_SIZE);
    }
}

/*
 * One component record, with the fields every kind of component shares.
 *
 * `index` is the component's position in the flat graph array, which is
 * where its record physically lives and has to be unique and sequential
 * across every type. `virtual_id` is a different thing that today happens to
 * take the same values by coincidence: IRIX requires a processor
 * component's virtual identifier to be its processor number specifically,
 * checked for range and uniqueness on every boot, and no consumer has been
 * found that reads it for any other type — but numbering every component by
 * its ordinal among components of its own type is one rule rather than a
 * type-by-type list of exceptions, and it is what writing the flat index
 * was standing in for as long as each type had exactly one member appearing
 * first. Add a second processor, or a second component of any type, and the
 * two numbers stop agreeing.
 *
 * `physical_id` is a third identifier again, and it is a parameter rather than
 * something a caller may add afterwards because leaving it out is exactly the
 * defect this signature exists to prevent. Real firmware writes it for every
 * component type and each type means something different by it: a processor's
 * is its processor index, a Hub's is its node identifier, a memory bank's is
 * its bank index, the I/O bridge's is its widget number, and a disk or I/O
 * controller's is its PCI device number. It shares a record with `widget`,
 * which for most types is a different number for a different purpose, so a
 * model that writes only one of the two is wrong in a way that reads as
 * plausible: the guest that skips a component and the guest that addresses the
 * wrong widget both look like ordinary machines. The vendor's own firmware
 * reads this field and not `widget` to learn which widget the I/O bridge is,
 * and a zero there sends it to widget zero.
 */
static void sn0_component(uint8_t *graph, unsigned int index,
                          unsigned int virtual_id, uint8_t type,
                          uint8_t physical_id, uint8_t widget)
{
    uint8_t *c = graph + (SN0_GRAPH_COMPONENTS - SN0_CONFIG_GRAPH) +
                 index * SN0_GRAPH_COMP_STRIDE;

    c[0x00] = type;
    c[0x01] = 1;                                /* record version */
    /* Not optional: a component without this is skipped by every consumer. */
    c[0x02] = SN0_COMP_ENABLED;
    c[SN0_COMP_PHYSICAL_ID] = physical_id;
    c[0x20] = widget;
    stl_be_p(c + 0x1c, virtual_id);
    /*
     * Padding in the reference declaration, but OpenBSD declares a bitfield
     * across these two bytes holding an Origin 3000 controller port, a PCI bus
     * number, a multifunction flag and a PCI function number. Leaving them to
     * whatever the buffer held would have an OpenBSD guest read invented PCI
     * routing out of a machine that has none, so they are zeroed on purpose
     * rather than by luck.
     */
    stw_be_p(c + 0x24, 0);
    /*
     * The back-pointer to a matching firmware component is read for every
     * component a guest visits, so it is zero rather than left as whatever the
     * buffer held.
     */
    stq_be_p(c + 0x28, 0);
}

static uint8_t *sn0_component_at(uint8_t *graph, unsigned int index)
{
    return graph + (SN0_GRAPH_COMPONENTS - SN0_CONFIG_GRAPH) +
           index * SN0_GRAPH_COMP_STRIDE;
}

/*
 * Find a deterministic IP29 DIMM population for QEMU's scalar -m value.
 * Physical banks support 64, 128, 256, or 512 MiB and bank zero must be
 * present. Prefer larger banks and fewer partially populated pairs.
 *
 * A 512 MiB bank zero would fill all four of its slots, including the fixed
 * boot-flash and synthesized-firmware addresses at the top of the stride.
 * Until that fixed decode can be represented independently, keep bank zero
 * at 256 MiB or less and reject totals that cannot be expressed in four banks
 * under that constraint. This is a model limitation, not an IP29 DIMM rule.
 */
static bool sn0_ip29_layout_from(uint64_t remaining_mb, unsigned int bank,
                                 uint16_t bank_mb[SN0_MEM_BANKS])
{
    static const uint16_t supported_mb[] = { 512, 256, 128, 64 };
    unsigned int i;

    if (remaining_mb == 0) {
        return bank > 0;
    }
    if (bank == IP29_MEM_BANKS) {
        return false;
    }

    for (i = 0; i < G_N_ELEMENTS(supported_mb); i++) {
        uint16_t size_mb = supported_mb[i];

        if ((bank == 0 && size_mb > 256) || size_mb > remaining_mb) {
            continue;
        }
        bank_mb[bank] = size_mb;
        if (sn0_ip29_layout_from(remaining_mb - size_mb, bank + 1,
                                 bank_mb)) {
            return true;
        }
        bank_mb[bank] = 0;
    }
    return false;
}

static bool sn0_ip29_memory_layout(uint64_t ram_size,
                                   uint16_t bank_mb[SN0_MEM_BANKS],
                                   unsigned int *populated_banks)
{
    unsigned int i;

    memset(bank_mb, 0, sizeof(uint16_t) * SN0_MEM_BANKS);
    if (ram_size == 0 || ram_size % MiB != 0 ||
        !sn0_ip29_layout_from(ram_size / MiB, 0, bank_mb)) {
        return false;
    }

    i = 0;
    while (i < SN0_MEM_BANKS && bank_mb[i] != 0) {
        i++;
    }
    *populated_banks = i;
    return true;
}

/*
 * One board record, linked to the next. The list is terminated only by a zero
 * next-board offset: a consumer reads the record first and checks afterwards,
 * so there is no escape from a bad link. A board without the local-board kind
 * is skipped entirely, components and all.
 *
 * The declared count and the number of components actually populated in the
 * array are separate parameters rather than one, because real firmware makes
 * them disagree on this machine's midplane: it populates one component and
 * declares a count of zero, on purpose, so that a consumer walking the array
 * to the declared count never visits it. `components` is how many entries
 * this call populates; `declared_count` is what the count field says. Every
 * board but the midplane wants them equal, which is what the callers do.
 */
static void sn0_board(uint8_t *graph, unsigned int index, uint8_t type,
                      uint8_t version, unsigned int next,
                      unsigned int first_component, unsigned int components,
                      unsigned int declared_count, unsigned int nasid)
{
    uint8_t *b = graph + (SN0_GRAPH_BOARDS - SN0_CONFIG_GRAPH) +
                 index * SN0_GRAPH_BOARD_SIZE;
    unsigned int i;

    stl_be_p(b + 0x00, next);
    b[0x04] = SN0_BOARD_LOCAL;
    b[0x05] = type;
    b[0x06] = version;
    b[0x17] = declared_count;
    /*
     * A board's components are addressed relative to the board's own node
     * identifier rather than the node whose list is being walked, and that
     * field also indexes a table with no bounds check, so it has to be right
     * and in range on every board even here.
     */
    stw_be_p(b + 0x20, nasid);
    for (i = 0; i < components; i++) {
        stl_be_p(b + 0x24 + i * 4,
                 SN0_GRAPH_COMPONENTS + (first_component + i) *
                 SN0_GRAPH_COMP_STRIDE);
    }
}

/*
 * The configuration graph: a header carrying the console descriptor inline,
 * then the three boards a single-node Origin 200 has.
 *
 * The board list follows what a real machine of this generation emits rather
 * than what would be tidiest. Its midplane board carries no crossbar component
 * at all — just a module serial number — which is why a guest falling back
 * to assuming the Hub sits at a fixed widget is the normal path on this machine
 * and not a degraded one.
 */
static void sn0_build_graph(MachineState *machine, unsigned int node_index)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0NodeState *node_state = &s->nodes[node_index];
    hwaddr node = SN0_NODE_BASE(node_state->nasid);
    uint8_t *graph = g_malloc0(SN0_CONFIG_GRAPH_SIZE);
    uint8_t *console = graph + SN0_GRAPH_CONSOLE_OFF;
    uint8_t *cpu, *hub, *mem;
    uint64_t total_mb = 0;
    uint64_t widget;
    unsigned int base_io_component;
    unsigned int cpu_count = node_state->cpu[1] ? 2 : 1;
    unsigned int hub_component = cpu_count;
    unsigned int i;
    unsigned int memory_component = hub_component + 1;
    unsigned int midplane_component = memory_component + 1;

    for (i = 0; i < node_state->populated_banks; i++) {
        total_mb += node_state->bank_mb[i];
    }

    stq_be_p(graph + 0x00, SN0_GRAPH_MAGIC);
    /* The structure version, which real firmware writes as zero. */
    stl_be_p(graph + 0x08, 0);
    stl_be_p(graph + 0x0c, 0x48);               /* allocation arenas */
    /*
     * Redundant with the descriptor embedded below and it has to agree with it:
     * a guest reaches the descriptor by adding this to the header's own
     * address, so anything else points the console into the middle of the
     * header.
     */
    stl_be_p(graph + 0x10, SN0_GRAPH_CONSOLE_OFF);
    stl_be_p(graph + 0x14, SN0_GRAPH_BOARDS);

    /*
     * The console descriptor. The port offset is the trap: a guest picks which
     * generation of I/O controller this is from the low twelve bits of the
     * serial-port base, and which register a write lands on from the low
     * twenty. Those are not the same field, and a value that satisfies the
     * first while wrong for the second gives a guest that identifies its
     * console correctly and then drives an address that answers nothing.
     *
     * All three base fields share one root rather than being composed
     * separately, because composing them separately is how the device window
     * came to be present in one of them and missing from the other two. The
     * three are read by different guests for different purposes, so a
     * correction applied to one does not announce itself to the others.
     *
     * **The root is the widget, not the device window, and that is the whole
     * of this arrangement.** Two of the three fields do live inside the
     * controller's own window on the bus; the configuration base does not. It
     * is a *sibling* of that window in the bridge's widget space, at a lower
     * offset. Rooted at the device window, the correct configuration address
     * is not merely unwritten — it is unreachable, and every value the code
     * could express is wrong. Rooted at the widget, each field names its own
     * offset and the shared terms still have one source, so a correction to
     * the widget number or the space base reaches all three.
     */
    widget = 0x9200000000000000ULL | SN0_PCI_WIDGET_BASE;

    /*
     * Every term is a compile-time constant, so the composition is checked
     * where a wrong one cannot be built rather than only where it can be
     * caught. That is worth more here than a test would be: the three fields
     * are read by different guests and one of them by no guest we boot, so a
     * test can only fail for whichever field somebody thought to cover.
     *
     * The serial-port base is the term with a capture behind it, and the whole
     * width is compared. Its low twelve bits alone are the controller's
     * generation and its low twenty the register offset, so a check on either
     * accepts values wrong in the part it does not look at — which is
     * how two wrong values reached this field before.
     */
    QEMU_BUILD_BUG_ON((0x9200000000000000ULL | SN0_PCI_WIDGET_BASE |
                       SN0_CONSOLE_PORT_OFFSET) != 0x9200000008620178ULL);

    /*
     * The serial-port base, which OpenBSD splits into a bus and a register
     * offset. This is the field carrying the generation trap in its low twelve
     * bits.
     */
    stq_be_p(console + 0x00, widget | SN0_CONSOLE_PORT_OFFSET);

    /*
     * The configuration base: the controller's type 0 PCI configuration
     * window, which the vendor kernel's own controller-reset path follows as a
     * pointer into configuration space. Not the chip's memory base, which is
     * what this field held until the two were told apart — the same value as
     * the field below it, and wrong by the whole device window.
     *
     * Still derived rather than captured: nothing has been observed in this
     * field on real hardware. What makes the derivation checkable without one
     * is that the geometry is public — mainline Linux gives the bridge's type
     * 0 base and its per-device stride in arch/mips/include/asm/pci/bridge.h,
     * with a macro composing them exactly as the bridge's header here does.
     */
    stq_be_p(console + 0x08, widget | SGI_BRIDGE_CFG0_DEVICE(SN0_IOC3_DEVICE));

    /*
     * The memory base, the one field in this descriptor Linux reads. It takes
     * this as the base of the controller chip and adds the chip's own register
     * offset itself, validating nothing — no null check, no signature, no
     * timeout. So it needs the device window the serial-port base carries, and
     * it needs to be an address the processor can issue rather than a bare
     * physical one. Wrong here, Linux takes a bus error or spins forever on a
     * transmitter-empty bit that never sets, and both look like a silent boot.
     */
    stq_be_p(console + 0x10, widget | SN0_BRIDGE_DEVICE2);
    stw_be_p(console + 0x18, SN0_CONSOLE_BAUD);
    stl_be_p(console + 0x1c, 0);                /* console type */
    stw_be_p(console + 0x20, 0);                /* node identifier */
    console[0x22] = SN0_CONSOLE_WIDGET;
    console[0x23] = 1;                          /* devices behind the bridge */

    /* Board 0: the node board, with its processors, Hub and memory banks. */
    sn0_board(graph, 0, SN0_BOARD_NODE, SN0_BOARD_VERSION_NODE,
              SN0_GRAPH_BOARDS + SN0_GRAPH_BOARD_SIZE, 0, cpu_count + 2,
              cpu_count + 2, node_state->nasid);
    /*
     * These physical identifiers name different things: a processor's slice,
     * the Hub's node, and the memory component's first bank. Only the latter
     * two remain zero when slice B is populated.
     */
    for (i = 0; i < cpu_count; i++) {
        sn0_component(graph, i, node_index * SN0_CPUS_PER_NODE + i,
                      SN0_COMP_PROCESSOR, i, 0);
    }
    sn0_component(graph, hub_component, 0, SN0_COMP_HUB,
                  node_state->nasid, 0);
    sn0_component(graph, memory_component, 0, SN0_COMP_MEMORY, 0, 0);

    /*
     * The processor's own fields, which are the only source a guest has for its
     * clock and cache: with none readable its clock stays zero, which breaks
     * its delay loop, and it dies just after its last printed line. The
     * identification field is sixteen bits, so it carries the low half of the
     * processor identification register and not the whole of it.
     */
    for (i = 0; i < cpu_count; i++) {
        cpu = sn0_component_at(graph, i);
        stw_be_p(cpu + 0x38, 0x0900);           /* identification, low half */
        /*
         * The floating-point unit is on the same die and carries the same
         * implementation and revision as its processor.
         */
        stw_be_p(cpu + 0x3a, 0x0900);
        stw_be_p(cpu + 0x3c, 180);              /* clock in megahertz */
        stw_be_p(cpu + 0x3e, 1);                /* secondary cache, megabytes */
        stw_be_p(cpu + 0x40, 90);               /* secondary cache clock */
    }

    hub = sn0_component_at(graph, hub_component);
    /*
     * The port descriptor at 0x3c: this Hub names no crossbar link, so the
     * node-identifier bits must read as the invalid marker rather than the
     * zero the buffer starts with. A consumer that treats zero as a real
     * node follows it into a board lookup at this node's own hardware
     * directory and fails to recognise it as a board.
     */
    stq_be_p(hub + 0x3c, SN0_PORT_UNCONNECTED);
    stq_be_p(hub + 0x58, SN0_HUB_FREQ_HZ);      /* Hub clock in hertz */

    /*
     * The memory bank, in the eight-bank layout the addressing mode this
     * machine reports calls for. The mode is not just a count: the other
     * mode's record is eight bytes shorter, so it moves the directory
     * attribute too, and a guest reading four bank sizes out of a record
     * holding eight takes the fifth bank's size for the attribute while
     * separately computing every node-relative address with the wrong shift.
     * One guest cannot even adapt, its bank count being fixed when the kernel
     * is built.
     *
     * The guest reads the mode from the Hub's network-interface status
     * register, which makes this layout conditional on that register rather
     * than on anything visible here. The firmware test asserts the two
     * together so neither can be changed alone.
     */
    mem = sn0_component_at(graph, memory_component);
    stw_be_p(mem + 0x38, total_mb);             /* total in megabytes */
    stw_be_p(mem + 0x3a, 0);                    /* bank-to-address mapping */
    for (i = 0; i < SN0_MEM_BANKS; i++) {
        stw_be_p(mem + 0x3c + i * 2, node_state->bank_mb[i]);
    }

    /*
     * Board 1: the midplane, carrying a module serial number and nothing else,
     * which is what a real Origin 200 emits.
     *
     * Real firmware populates that one component and declares a count of
     * zero — deliberately, not by omission. OpenBSD's enumeration loop runs
     * to the declared count rather than to the end of the array, so on real
     * hardware the serial-number component is present and never visited.
     * There is a mechanism behind the number, not only a captured value to
     * follow: real firmware adds the component and then decrements the
     * count by one on purpose, and its own consumer for that component reads
     * it back at the index *equal to* the (decremented) count — exactly the
     * slot the decrement excludes from an ordinary walk. A count of one
     * would put the component one slot away from where that consumer looks.
     */
    sn0_board(graph, 1, SN0_BOARD_MIDPLANE, SN0_BOARD_VERSION,
              SN0_GRAPH_BOARDS + 2 * SN0_GRAPH_BOARD_SIZE,
              midplane_component, 1, 0, node_state->nasid);
    sn0_component(graph, midplane_component, 0, SN0_COMP_MODULE_SERIAL, 0, 0);

    /*
     * Board 2: the base-I/O board, and the end of the list. Nothing on the path
     * to the console switch reads it; it is here because the graph has to
     * describe the machine that was actually built, and because the moment a
     * console works this is what a guest walks looking for one.
     *
     * Four components in this order — the I/O bridge, two disk controllers,
     * then the I/O controller — matching a real Origin 200's own captured
     * graph and hardware inventory (docs/component-taxonomy.md's captured
     * `hinv` output, docs/library/boot-logs.md). Every component on this board
     * hangs off the same widget and each carries a different physical
     * identifier, which is what makes the two fields easy to confuse and worth
     * stating together here.
     *
     * The I/O bridge's physical identifier is its widget number. The vendor's
     * own firmware reads that field, not the widget field beside it, to learn
     * which widget its bridge is, and then addresses the bridge's error
     * registers through whatever it finds — so a zero here sends the firmware
     * to widget zero and it faults there.
     *
     * Each disk and I/O controller's physical identifier is its PCI device
     * number, and its virtual identifier is its controller number. A guest's
     * boot-device walk compares both against what the Bridge's own PCI probe
     * finds. These are the same SN0_SCSI_DEVICE0/1 and SN0_IOC3_DEVICE
     * constants that place those devices on the bus above, not values asserted
     * separately that merely happen to agree with them.
     */
    base_io_component = midplane_component + 1;
    sn0_board(graph, 2, SN0_BOARD_BASE_IO, SN0_BOARD_VERSION, 0,
              base_io_component, 4, 4, node_state->nasid);
    sn0_component(graph, base_io_component, 0, SN0_COMP_BRIDGE,
                  SN0_CONSOLE_WIDGET, SN0_CONSOLE_WIDGET);
    sn0_component(graph, base_io_component + 1, 0, SN0_COMP_SCSI,
                  SN0_SCSI_DEVICE0, SN0_CONSOLE_WIDGET);
    sn0_component(graph, base_io_component + 2, 1, SN0_COMP_SCSI,
                  SN0_SCSI_DEVICE1, SN0_CONSOLE_WIDGET);
    sn0_component(graph, base_io_component + 3, 0, SN0_COMP_IO_CONTROLLER,
                  SN0_IOC3_DEVICE, SN0_CONSOLE_WIDGET);

    {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.graph", node_index);

        rom_add_blob_fixed(name, graph, SN0_CONFIG_GRAPH_SIZE,
                       node + SN0_CONFIG_GRAPH);
    }
    g_free(graph);
}

/*
 * The structures a guest reads out of low memory before it has a console. Each
 * is written where the boot structures datasheet puts it, and each carries
 * every field rather than only the ones a guest currently reads: a block that
 * is right only where somebody looks is a trap for whoever comes next.
 */
static void sn0_build_structures(MachineState *machine,
                                 unsigned int node_index,
                                 uint64_t kernel_page, uint64_t kernel_pages)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0NodeState *node_state = &s->nodes[node_index];
    hwaddr node = SN0_NODE_BASE(node_state->nasid);
    struct sn0_mem_descriptor mem[6];
    uint8_t param[0x60] = { };
    uint8_t dir[SN0_HW_DIR_ENTRIES * SN0_HW_DIR_ENTRY_SIZE] = { };
    uint8_t gda[SN0_GDA_SIZE] = { };
    unsigned int node_count =
        machine->numa_state && machine->numa_state->num_nodes ?
        machine->numa_state->num_nodes : 1;
    unsigned int n = 0;
    int i;

    /*
     * The parameter block. Two things about the signature are fatal to get
     * wrong: it is a numeric constant rather than the four characters it looks
     * like, and it has to sit in the second four bytes of a 64-bit field. A
     * guest tests the 32-bit form first by reading the first four bytes, so a
     * constant written there makes it conclude the firmware is 32-bit and read
     * every pointer at half its real offset.
     */
    stq_be_p(param + 0x00, SN0_PARAM_SIGNATURE);
    stq_be_p(param + 0x08, 0x60);                   /* length of this block */
    stw_be_p(param + 0x10, 64);                     /* version */
    stw_be_p(param + 0x12, 0);                      /* revision */
    stq_be_p(param + 0x38, 0x118);                  /* call vector length */
    stq_be_p(param + 0x40, SN0_COMPAT_CACHED + SN0_ARCS_VECTOR);
    stq_be_p(param + 0x48, 0x88);                   /* private vector length */
    stq_be_p(param + 0x50, SN0_COMPAT_CACHED + SN0_ARCS_VECTOR + 0x400);
    rom_add_blob_fixed("sn0.param", param, sizeof(param),
                       node + SN0_PARAM_BLOCK);

    /*
     * The hardware directory. Its index assignments are permanent, and on the
     * path a guest takes when it finds no component tree the
     * configuration-graph entry's signature is the whole of the machine
     * identification — so that entry being right is what stops the guest
     * halting with an unrecognised system.
     */
    for (i = 0; i < SN0_HW_DIR_ENTRIES; i++) {
        uint8_t *e = dir + i * SN0_HW_DIR_ENTRY_SIZE;

        switch (i) {
        case SN0_HW_DIR_LAUNCH:
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x08, SN0_LAUNCH_BLOCKS);
            stq_be_p(e + 0x18, SN0_LAUNCH_DIR_SIZE);
            stq_be_p(e + 0x20, SN0_LAUNCH_ENTRIES);
            stq_be_p(e + 0x28, SN0_LAUNCH_STRIDE);
            break;
        case SN0_HW_DIR_CONFIG_GRAPH:
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x08, SN0_CONFIG_GRAPH);
            stq_be_p(e + 0x18, SN0_CONFIG_GRAPH_SIZE);
            stq_be_p(e + 0x20, 1);
            break;
        case SN0_HW_DIR_GDA:
            /* The odd one: reached through the pointer, not the offset. */
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x10, SN0_COMPAT_CACHED + SN0_GDA);
            stq_be_p(e + 0x18, SN0_GDA_SIZE);
            stq_be_p(e + 0x20, 1);
            break;
        case SN0_HW_DIR_NMI:
            /*
             * OpenBSD reads this entry's offset during machine setup and
             * stores its own handler address and complement straight through
             * it, before the console exists and with nothing that could
             * report a wrong destination. A zeroed entry sends that write to
             * node offset zero — the exception-handler area — invisibly,
             * because the kernel's own exception vectors are copied over the
             * same bytes later in setup and paper over the stray write. That
             * is an ordering accident, not a property worth relying on.
             */
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x08, SN0_NMI_BLOCKS);
            stq_be_p(e + 0x18, SN0_NMI_DIR_SIZE);
            stq_be_p(e + 0x20, SN0_NMI_ENTRIES);
            stq_be_p(e + 0x28, SN0_NMI_STRIDE);
            break;
        case SN0_HW_DIR_FREEMEM:
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x08, SN0_FIRST_FREE);
            stq_be_p(e + 0x18, SN0_HWDIR_FREEMEM_SIZE);
            stq_be_p(e + 0x20, 1);
            break;
        case SN0_HW_DIR_DEBUG_STACKS:
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x08, SN0_HWDIR_DEBUG_STACKS_BASE);
            stq_be_p(e + 0x18, SN0_HWDIR_DEBUG_STACKS_SIZE);
            stq_be_p(e + 0x20, SN0_HWDIR_DEBUG_STACKS_N);
            stq_be_p(e + 0x28, SN0_HWDIR_DEBUG_STACKS_STRIDE);
            break;
        case SN0_HW_DIR_PI_ERROR:
            stq_be_p(e + 0x00, SN0_HW_DIR_SIGNATURE);
            stq_be_p(e + 0x08, SN0_HWDIR_PI_ERROR_BASE);
            stq_be_p(e + 0x18, SN0_HWDIR_PI_ERROR_SIZE);
            stq_be_p(e + 0x20, 1);
            break;
        default:
            break;
        }
    }
    rom_add_blob_fixed("sn0.hwdir", dir, sizeof(dir), node + SN0_HW_DIRECTORY);

    /*
     * The launch blocks. Not on the banner path — they matter when a second
     * processor starts — but the directory advertises them from the start,
     * so they are written from the start: both blocks carry the launch
     * signature and are otherwise zeroed, since there is no second processor
     * to launch yet.
     */
    {
        uint8_t launch[SN0_LAUNCH_ENTRIES][SN0_LAUNCH_STRIDE] = { };
        int j;

        for (j = 0; j < SN0_LAUNCH_ENTRIES; j++) {
            stq_be_p(launch[j] + 0x00, SN0_LAUNCH_SIGNATURE);
        }
        rom_add_blob_fixed("sn0.launch", launch, sizeof(launch),
                           node + SN0_LAUNCH_BLOCKS);
    }

    /*
     * The global data area. The node table is the only enumeration of nodes
     * either guest has: one walks it from entry zero and stops at the first
     * negative entry with no fallback, so entry zero has to be this node and
     * entry one has to be negative already.
     *
     * The entries are signed sixteen-bit identifiers. A guest reads each one
     * with a signed halfword load and skips it when it is negative, so absent
     * has to be all ones across both bytes: a single 0xff byte pairs with its
     * neighbour and reads as a positive 255, which nothing rejects, and the
     * guest then computes addresses on a node numbered 255 and faults on the
     * first one it forms.
     */
    stl_be_p(gda + 0x00, SN0_GDA_SIGNATURE);
    stw_be_p(gda + 0x04, SN0_GDA_VERSION);
    stw_be_p(gda + 0x06, 0);                        /* master node */
    for (i = 0; i < SN0_GDA_NODES; i++) {
        stw_be_p(gda + SN0_GDA_NODE_TABLE + i * 2,
                 i < node_count ? i : SN0_GDA_NODE_ABSENT);
    }
    rom_add_blob_fixed("sn0.gda", gda, sizeof(gda), node + SN0_GDA);

    sn0_build_graph(machine, node_index);

    /* The memory descriptors, shaped as a real machine of this era reports. */
    mem[n].type = SN0_MEM_EXCEPTION_BLOCK;
    mem[n].base_page = 0;
    mem[n++].pages = 1;

    mem[n].type = SN0_MEM_PARAM_BLOCK;
    mem[n].base_page = 1;
    mem[n++].pages = 1;

    /*
     * The load-bearing one: below page 0x20, or the guest silently becomes
     * another machine.
     */
    mem[n].type = SN0_MEM_FREE;
    mem[n].base_page = SN0_MEM_FIRST_FREE_PAGE;
    mem[n++].pages = (kernel_pages ? kernel_page : SN0_MEM_PERMANENT_PAGE) -
                     SN0_MEM_FIRST_FREE_PAGE;

    if (kernel_pages) {
        mem[n].type = SN0_MEM_LOADED_PROGRAM;
        mem[n].base_page = kernel_page;
        mem[n++].pages = kernel_pages;

        mem[n].type = SN0_MEM_FREE;
        mem[n].base_page = kernel_page + kernel_pages;
        mem[n++].pages = SN0_MEM_PERMANENT_PAGE - (kernel_page + kernel_pages);
    }

    /* The walk stops here, and the guest rewrites this entry's count. */
    mem[n].type = SN0_MEM_FIRMWARE_PERMANENT;
    mem[n].base_page = SN0_MEM_PERMANENT_PAGE;
    mem[n++].pages = SN0_MEM_PERMANENT_PAGES;

    for (i = 0; i < (int)n; i++) {
        uint8_t buf[sizeof(struct sn0_mem_descriptor)] = { };

        stl_be_p(buf + 0x00, mem[i].type);
        stq_be_p(buf + 0x08, mem[i].base_page);
        stq_be_p(buf + 0x10, mem[i].pages);
        rom_add_blob_fixed("sn0.memdesc", buf, sizeof(buf),
                           node + SN0_MEM_LIST + i * sizeof(buf));
    }
    node_state->mem_addr = SN0_COMPAT_CACHED + SN0_MEM_LIST;
    node_state->mem_count = n;
}

/*
 * The one piece of firmware state that is a register rather than a structure in
 * memory. Small window 0 is unreliable through a defect on the Hub's
 * processor-interface side, so a real firmware redirects widget 0 onto the last
 * big window and points all software at that; both guests carry the workaround
 * with a comment saying so, and each expects to find the redirection already
 * done. Ours does it here.
 *
 * **It changes nothing, and that is worth knowing rather than discovering.**
 * The entry resets to widget zero, first slice, memory space, which is field
 * for field the value the redirection writes — so all seven windows already
 * point at widget zero before any firmware runs, and removing this call would
 * not move the boot by a line. Nor is that an artifact of our model: it follows
 * from the reset value and the field layout, so a real firmware's write is the
 * same belt-and-braces on any machine whose I/O bridge sits behind widget zero.
 * Ours is kept for being what a real firmware does, and it stops being a no-op
 * the moment the entry, the widget or the reset value differs.
 *
 * The consequence for anyone reading a green boot: reaching widget zero proves
 * the translation mechanism and the reset value, and proves nothing whatever
 * about this write.
 *
 * **What does not make it dispensable is a guest doing it instead, and that
 * reason circulates.** OpenBSD programs translation entries — but the *other*
 * ones. Its large-window allocator skips this entry by a hard-coded index and
 * then reads through it as though it were small window zero, so the guest
 * depends on this entry being right and never writes it. A firmware that
 * omitted the redirection would not be rescued by the guest.
 *
 * The distinction matters because the two reasons have different reach. The
 * coincidence above is a property of a machine whose I/O bridge sits behind
 * widget zero; on a machine where the redirect named anything else, this write
 * would be doing real work, and an implementer carrying the guest-does-it
 * version would drop it there too.
 *
 * It has to run at reset rather than at machine creation, because the Hub
 * resets its translation table and would undo it — the same reason the firmware
 * structures in memory are loaded by the ROM loader at reset rather than
 * written once. And it has to run *after* the Hub's own reset, which is what
 * registering it from the machine-init-done notifier buys: reset handlers run
 * in registration order, and the bus everything hangs off is registered when
 * board init has finished, so a handler registered during board init would run
 * first and be overwritten.
 */
static void sn0_firmware_reset(void *opaque)
{
    address_space_stq(&address_space_memory,
                      SN0_SPACE_XTALK + SN0_NODE_BASE(0) +
                      SN0_HUB_LOCAL_WINDOW + SN0_HUB_IO_BWIN_ENTRY(
                          SN0_FIRMWARE_WIDGET0_WINDOW),
                      SN0_FIRMWARE_WIDGET0_ENTRY,
                      MEMTXATTRS_UNSPECIFIED, NULL);
}

/*
 * The mode bits this board drives into its processor, which the R10000 latches
 * into the low twenty-five bits of its configuration register while reset is
 * asserted. Real firmware's first act is to check them: it reads the word out
 * of the flash configuration block, compares it against what the register
 * actually holds, and reports "an error copying mode bits" if they differ.
 *
 * **Driven from the same constant the flash block advertises, because two
 * copies of one board fact is the defect this check exists to catch.** A board
 * whose processor latched something other than what its own configuration
 * block claims is exactly the fault condition; a model with two constants can
 * produce it by editing one of them.
 *
 * It runs at reset rather than at machine creation because the processor's own
 * reset restores the register from the CPU model's defaults, which describe a
 * generic R10000 and cannot know this board's word. Registered from the
 * machine-init-done notifier for the same ordering reason as the handler
 * above: reset handlers run in registration order and this one has to follow
 * the processor's.
 */
/*
 * Reset the processor.
 *
 * **Without this the machine services a reset request without resetting the
 * processor at all**: the devices reset, the machine's own handlers run, and
 * the program counter is untouched, so execution simply carries on. Every
 * other MIPS machine in the tree registers this explicitly and nothing does it
 * automatically.
 *
 * That omission is what made the firmware's deliberate two-pass reset look
 * like a single pass -- the request was serviced and the processor never
 * restarted, so the second pass never happened and the marker it leaves
 * behind was never read.
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

static void sn0_mode_bits_reset(void *opaque)
{
    CPUMIPSState *env = &MIPS_CPU(opaque)->env;

    env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
                       (SN0_PROCESSOR_MODE_BITS & SN0_PROCESSOR_MODE_MASK);
}

/*
 * The PCI resources real ARCS PROM assigns at POST (task #43): a BAR for
 * each attached device, and the I/O bridge's own per-device window pre-aimed
 * at the base I/O controller's, so a guest can reach its console before it
 * has run any code of its own that could aim a window itself.
 *
 * This has to run at reset for the same reason sn0_firmware_reset above
 * does, and it is the same bug in a new place if it does not: a PCI
 * device's config space and the bridge's own device-window registers both
 * reset along with everything else, so an assignment made once at board
 * init is exactly the state their own reset handlers clear. Registering
 * this from the machine-init-done notifier, the same as that function, puts
 * it after the devices whose reset would otherwise undo it.
 *
 * Every device's window is pre-aimed, not only the controller's; see sn0.h
 * for why an earlier version of this function left the two adapters with
 * resources assigned but no window aimed, and for the guest that premise
 * did not hold for.
 *
 * Every device gets PCI_COMMAND_MASTER along with its space-decode bits.
 * This is neither pure fidelity nor a functional requirement proven on this
 * machine — both claims have been made and retracted once already, and the
 * honest position sits between them.
 *
 * The mechanism is real: PCI_COMMAND_MASTER gates the qlogic-isp1040's
 * bus_master_as through the generic PCI core's bus_master_enable_region, so
 * a device this firmware left without it genuinely could not DMA. There is
 * no SeaBIOS-equivalent anywhere on this machine — nothing else assigns PCI
 * resources before a guest runs — so firmware setting it is the only place
 * mastering could come from for a guest that does not set it itself, the
 * same standard way real ARCS PROM sets it at POST alongside the space-
 * decode bits.
 *
 * But every guest this fork actually runs sets it itself, so the bit is
 * currently redundant rather than load-bearing. OpenBSD's qlw driver maps
 * its BARs through pci_mapreg_map() (sys/dev/pci/pci_map.c), whose internal
 * pci_mapreg_assign() unconditionally ORs PCI_COMMAND_MASTER_ENABLE into
 * the command register as part of mapping any BAR, before the driver's own
 * attach code ever touches PCI_COMMAND_STATUS_REG directly. Linux's ISP1040
 * driver does the equivalent explicitly: pci_set_master(pdev) in probe,
 * immediately after pci_enable_device() (drivers/scsi/qla1280.c). Neither
 * guest is depending on this machine to have done it first.
 *
 * And no DMA transfer has actually been observed succeeding on this Origin
 * 200 configuration regardless: the ISP1040's mailbox does not come up past
 * reset here yet (task #44), so this fix is unexercised by anything this
 * fork currently boots. Keep it because it is what real firmware does and
 * because a guest that does not self-enable mastering — none we run today,
 * but not a closed set — would need it; do not read its presence as
 * evidence the machine has been proven to DMA correctly, and do not read
 * its absence-until-now as evidence it did not matter.
 */
static void sn0_assign_node_pci_resources(SN0MachineState *s,
                                          unsigned int node_index)
{
    unsigned int slot;

    pci_default_write_config(s->pci_ioc3[node_index], PCI_BASE_ADDRESS_0,
                             SN0_PCI_IOC3_MEM, 4);
    pci_default_write_config(s->pci_ioc3[node_index], PCI_COMMAND,
                             PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
    sgi_bridge_assign_window(&s->io_bridge[node_index], SN0_IOC3_DEVICE,
                             SN0_PCI_IOC3_MEM,
                             true);

    for (slot = 0; slot < SN0_SCSI_ADAPTERS; slot++) {
        hwaddr io_base = slot == 0 ? SN0_PCI_SCSI0_IO : SN0_PCI_SCSI1_IO;
        hwaddr mem_base = slot == 0 ? SN0_PCI_SCSI0_MEM : SN0_PCI_SCSI1_MEM;

        pci_default_write_config(s->pci_scsi[node_index][slot],
                                 PCI_BASE_ADDRESS_0,
                                 io_base, 4);
        pci_default_write_config(s->pci_scsi[node_index][slot],
                                 PCI_BASE_ADDRESS_1,
                                 mem_base, 4);
        pci_default_write_config(s->pci_scsi[node_index][slot], PCI_COMMAND,
                                 PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                                 PCI_COMMAND_MASTER, 2);
        sgi_bridge_assign_window(&s->io_bridge[node_index],
                                 SN0_SCSI_DEVICE0 + slot,
                                 mem_base, true);
    }
}

static void sn0_assign_pci_resources(void *opaque)
{
    SN0MachineState *s = opaque;
    MachineState *machine = MACHINE(s);
    unsigned int node_count;
    unsigned int node_index;

    if (!s->io_bridge_present) {
        return;
    }

    node_count = machine->numa_state && machine->numa_state->num_nodes ?
                 machine->numa_state->num_nodes : 1;
    for (node_index = 0; node_index < node_count; node_index++) {
        sn0_assign_node_pci_resources(s, node_index);
    }
}

static void sn0_machine_ready(Notifier *notifier, void *data)
{
    SN0MachineState *s = SN0_MACHINE(qdev_get_machine());
    CPUState *cs;

    if (s->synthesized_firmware) {
        qemu_register_reset(sn0_firmware_reset, s);
    }
    CPU_FOREACH(cs) {
        qemu_register_reset(sn0_cpu_reset, cs);
        qemu_register_reset(sn0_mode_bits_reset, cs);
    }
    if (s->synthesized_firmware) {
        qemu_register_reset(sn0_assign_pci_resources, s);
    }
    qemu_register_reset(sn0_diagnostic_reset, s);
}

static Notifier sn0_machine_ready_notifier = {
    .notify = sn0_machine_ready,
};

/*
 * Make the firmware's boot-progress indicator visible without the operator
 * having to know it exists.
 *
 * The Hub emits every write to its indicator register as the trace event
 * sgi_hub_led, decoded against real firmware's own progress and failure
 * codes. A trace event is off by default by construction, so that signal --
 * the only progress this machine reports while the firmware is still
 * deciding which console to talk to -- reached only somebody who already
 * knew to ask for it by name. This turns it on for them.
 *
 * ONE LINE IS ENOUGH, AND IT IS WORTH SAYING WHY, because the log backend
 * makes it look like two are needed: trace_sgi_hub_led() emits only if the
 * event's dynamic state is set AND qemu_loglevel_mask(LOG_TRACE). The second
 * is already true on every run. qemu_init() calls
 * qemu_set_log_filename_flags() unconditionally, not only when -d was given,
 * and qemu_set_log_internal() force-sets LOG_TRACE whenever the log trace
 * backend is compiled in -- which also makes qemu_loglevel non-zero, so the
 * destination opens, defaulting to stderr and honouring -D. Raising the log
 * level here as well changes nothing; it was tried, and a bare run emits the
 * codes with it removed.
 *
 * Going through the trace event rather than printing directly keeps the
 * emitted line byte-identical, which matters because it is parsed:
 * tools/boot-gate ratchets on the set of codes a known-good boot reaches and
 * matches this text with a regular expression. Three runner tools also name
 * the event on their own command lines to arm it, which is harmless now that
 * it is armed here, and is kept so they still work against binaries built
 * before this. Printing the same information by a second route would have
 * given that parser a format to diverge from for no gain.
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
    unsigned int max_cpus = machine->smp.max_cpus;
    unsigned int index;

    if (machine->possible_cpus) {
        assert(machine->possible_cpus->len == max_cpus);
        return machine->possible_cpus;
    }

    machine->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                       sizeof(CPUArchId) * max_cpus);
    machine->possible_cpus->len = max_cpus;
    for (index = 0; index < max_cpus; index++) {
        CPUArchId *slot = &machine->possible_cpus->cpus[index];
        unsigned int node = index / SN0_CPUS_PER_NODE;
        unsigned int slice = index % SN0_CPUS_PER_NODE;

        slot->type = machine->cpu_type;
        slot->vcpus_count = 1;
        slot->arch_id = (node << 1) | slice;
        slot->props.has_socket_id = true;
        slot->props.socket_id = node;
        slot->props.has_core_id = true;
        slot->props.core_id = slice;
        slot->props.has_thread_id = true;
        slot->props.thread_id = 0;
    }

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
    return index / SN0_CPUS_PER_NODE;
}

/*
 * Which node's memory an address without a NASID names on this node, in
 * memory space. Normally the node's own. The exception is the cached
 * low-address alias after synthesized firmware has handed a directly loaded
 * kernel control: Linux disables PI_CALIAS_SIZE before launching remote
 * processors, so their unqualified kernel-text addresses name node zero. A
 * real PROM keeps the reset-local view and owns that transition itself.
 * Shared by the processors' local views and the Hub's, so the block
 * transfer engines always resolve a node-less address onto the same memory
 * as the processors that programmed them.
 */
static hwaddr sn0_node_local_memory_base(MachineState *machine,
                                         SN0NodeState *node,
                                         unsigned int node_index)
{
    if (node_index != 0 && SN0_MACHINE(machine)->synthesized_firmware &&
        machine->kernel_filename) {
        return SN0_NODE_BASE(0);
    }
    return SN0_NODE_BASE(node->nasid);
}

static void sn0_node_create_cpus(MachineState *machine, SN0NodeState *node,
                                 unsigned int node_index,
                                 unsigned int cpu_base,
                                 unsigned int cpu_count,
                                 uint64_t ram_offset)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    unsigned int slice, half, space;

    node->cpuclk = clock_new(OBJECT(node), "cpu-refclk");
    clock_set_hz(node->cpuclk, SN0_CPU_REFCLK_HZ);
    for (slice = 0; slice < cpu_count; slice++) {
        unsigned int cpu_index = cpu_base + slice;
        g_autofree char *alias_name =
            g_strdup_printf("sn0.node%u.cpu%u.system", node_index, slice);
        g_autofree char *name = g_strdup_printf("cpu%u", slice);
        g_autofree char *promdata_name =
            g_strdup_printf("sn0.node%u.cpu%u.promdata", node_index, slice);
        g_autofree char *promdata_filter_name = g_strdup_printf(
            "sn0.node%u.cpu%u.promdata-filter", node_index, slice);
        g_autofree char *sysmem_name =
            g_strdup_printf("sn0.node%u.cpu%u.memory", node_index, slice);

        memory_region_init(&node->cpu_sysmem[slice], OBJECT(node),
                           sysmem_name, UINT64_MAX);
        memory_region_init_alias(&node->sysmem_alias[slice], OBJECT(node),
                                 alias_name, system, 0, UINT64_MAX);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[slice], 0,
                                            &node->sysmem_alias[slice], -1);

        /*
         * Addresses without a NASID normally name the reading processor's
         * node; sn0_node_local_memory_base() carries the one exception,
         * which applies to memory space alone.  Keep the global system map
         * underneath so an explicit NASID still reaches either node in
         * both cases.
         */
        for (space = 0; space < SN0_UNCACHED_SPACES; space++) {
            g_autofree char *local_name = g_strdup_printf(
                "sn0.node%u.cpu%u.local-space%u", node_index, slice, space);
            hwaddr alias_node_base = space == 0 ?
                sn0_node_local_memory_base(machine, node, node_index) :
                node_base;
            hwaddr local_offset = 0;
            hwaddr local_size = SN0_NODE_SIZE;

            /*
             * In crosstalk space, self-decode only the Hub's own window.
             *
             * An address carrying no node name and an address naming node
             * zero are the same bits, because node zero's offset is zero.
             * Redirecting a whole node's worth of every space therefore
             * captured not only the nameless addresses it was meant for but
             * every explicit reference to node zero as well -- so a
             * processor elsewhere aiming at node zero's Hub reached its own,
             * and the interrupt it sent arrived where nobody was waiting for
             * it. Measured: from the second node, no cross-node interrupt
             * store ever crossed.
             *
             * The hardware separates the two by decoding a small window --
             * the Hub's eight megabytes -- as "the reading node's own Hub"
             * from anywhere, and putting the explicit form outside it.
             * Narrow to that window and the distinction returns: nameless
             * Hub addresses still find the reader's own node, and every
             * other crosstalk address, node zero's included, falls through
             * to the global map, where the per-node explicit view at
             * SN0_HUB_NODE_WINDOW already answers.
             *
             * Scoped to crosstalk space, which is where the Hub lives and
             * where the fault was measured. The other spaces keep the wide
             * alias; whether they need the same treatment is unmeasured, and
             * guessing at it would put an unobserved claim in the map.
             *
             * The big windows sit outside this window, so a nameless
             * reference to them no longer resolves to the reader's own node.
             * That follows from the hardware's own window being the Hub's
             * alone, and the ROM suite and firmware boot are what say the
             * guests do not rely on the wider behaviour.
             */
            if (SN0_SPACE(space) == SN0_SPACE_XTALK) {
                local_offset = SN0_HUB_LOCAL_WINDOW;
                local_size = SGI_HUB_WINDOW_SIZE;
            }

            memory_region_init_alias(&node->local_space[slice][space],
                                     OBJECT(node), local_name, system,
                                     SN0_SPACE(space) + alias_node_base +
                                     local_offset,
                                     local_size);
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                SN0_SPACE(space) + local_offset,
                &node->local_space[slice][space], 0);
        }

        /*
         * The bottom 128 KiB of Hub-local space is the remaining exception:
         * the reading slice flips address bit 16, giving the two processors
         * separate scratch halves.
         */
        for (half = 0; half < 2; half++) {
            g_autofree char *private_name = g_strdup_printf(
                "sn0.node%u.cpu%u.hub-private%u", node_index, slice, half);
            uint64_t private_offset = ram_offset +
                (half ^ slice) * SN0_HUB_PRIVATE_HALF;

            memory_region_init_alias(&node->hub_private[slice][half],
                                     OBJECT(node), private_name, machine->ram,
                                     private_offset, SN0_HUB_PRIVATE_HALF);
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                SN0_SPACE_HUB + half * SN0_HUB_PRIVATE_HALF,
                &node->hub_private[slice][half], 1);
        }

        node->cpu[slice] = MIPS_CPU(qdev_new(machine->cpu_type));

        /*
         * The cache-only private phase the filter models exists only under
         * the real PROM: it is entered when the PROM marks FPR 0's DEX bit,
         * which synthesized firmware never does before handing a directly
         * loaded kernel control.  In that mode the filter would be pure
         * forwarding -- except that an I/O region cannot back translated
         * code, so kernel text linked into this 32 KiB window would execute
         * one instruction per translation block.  Map the window as the
         * plain RAM it forwards to instead, and keep the filter for real
         * PROM boots, whose private phase genuinely needs it.
         */
        if (!(SN0_MACHINE(machine)->synthesized_firmware &&
              machine->kernel_filename)) {
            node->promdata[slice].cpu = node->cpu[slice];
            node->promdata[slice].node_base = node_base;
            memory_region_init_ram(&node->promdata[slice].data, OBJECT(node),
                                   promdata_name, SN0_PROMDATA_SIZE,
                                   &error_fatal);
            memory_region_init_io(&node->promdata[slice].filter, OBJECT(node),
                                  &sn0_promdata_ops, &node->promdata[slice],
                                  promdata_filter_name, SN0_PROMDATA_SIZE);
            /*
             * Firmware traps use cpu_memory_rw_debug() to copy results
             * through guest pointers.  The trap and this filter share the
             * node as their owner, so the device-wide guard would otherwise
             * reject that intentional nested access before the filter can
             * forward it to RAM.
             */
            node->promdata[slice].filter.disable_reentrancy_guard = true;
            qemu_register_reset(sn0_promdata_reset, &node->promdata[slice]);

            /*
             * The PROM addresses this cache-only data through its 0xa800...
             * XKPHYS alias. Its cache algorithm translates to ordinary
             * physical memory, so intercept that carrier window before
             * shared node RAM. When the private phase ends, the filter
             * forwards accesses to the same address in the global memory
             * space.
             */
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                SN0_SPACE_MEMORY + SN0_PROMDATA_PADDR,
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
        machine->possible_cpus->cpus[cpu_index].cpu = CPU(node->cpu[slice]);
        cpu_mips_irq_init_cpu(node->cpu[slice]);
        cpu_mips_clock_init(node->cpu[slice]);
    }
}

static uint64_t sn0_node_map_memory(MachineState *machine,
                                    SN0NodeState *node,
                                    unsigned int node_index,
                                    uint64_t ram_offset,
                                    uint64_t ram_size)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    unsigned int bank, segment;

    if (!sn0_ip29_memory_layout(ram_size, node->bank_mb,
                                &node->populated_banks)) {
        error_report("origin200: node %u's %" PRIu64 " MiB cannot be "
                     "populated from IP29 memory banks without overlapping "
                     "firmware regions", node_index, ram_size / MiB);
        exit(EXIT_FAILURE);
    }

    /*
     * Each bank occupies a 512 MiB stride. Banks of 128 MiB or less use its
     * first slot; larger banks interleave one quarter of their bytes into each
     * of four slots on a 128 MiB stride. Both supported guests derive this
     * placement from the plain-megabyte bank sizes in KLCONFIG.
     */
    for (bank = 0; bank < node->populated_banks; bank++) {
        uint64_t bank_size = (uint64_t)node->bank_mb[bank] * MiB;
        unsigned int segments = bank_size <= SN0_MEM_SLOT_SIZE ?
                                1 : SN0_MEM_BANK_SLOTS;
        uint64_t segment_size = bank_size / segments;

        for (segment = 0; segment < segments; segment++) {
            hwaddr segment_addr = (hwaddr)bank * SN0_MEM_BANK_SIZE +
                                  (hwaddr)segment * SN0_MEM_SLOT_SIZE;
            g_autofree char *name = g_strdup_printf(
                "sn0.node%u.ram.bank%u.slot%u", node_index, bank, segment);

            memory_region_init_alias(&node->mem_slot[bank][segment],
                                     OBJECT(node), name, machine->ram,
                                     ram_offset, segment_size);
            memory_region_add_subregion(system, node_base + segment_addr,
                                        &node->mem_slot[bank][segment]);

            /*
             * Hub-special offset zero mirrors the first 256 MiB of the
             * reading processor's local node memory. Mirror each populated
             * run separately so the alias preserves the bank's slot holes.
             * Per-CPU address-space overlays above flip the low 128 KiB for
             * slice B. This shared mapping remains the fallback for qtest,
             * DMA, and every address beyond that private range.
             */
            if (segment_addr < SN0_HUB_LOCAL_MEM_SIZE) {
                uint64_t alias_size = MIN(segment_size,
                                          SN0_HUB_LOCAL_MEM_SIZE -
                                              segment_addr);
                g_autofree char *alias_name = g_strdup_printf(
                    "sn0.node%u.hub-local-ram.bank%u.slot%u",
                    node_index, bank, segment);

                memory_region_init_alias(
                    &node->hub_local_mem_slot[bank][segment], OBJECT(node),
                    alias_name, machine->ram, ram_offset, alias_size);
                memory_region_add_subregion(
                    system, SN0_SPACE_HUB + node_base + segment_addr,
                    &node->hub_local_mem_slot[bank][segment]);
            }
            ram_offset += segment_size;
        }
    }

    return ram_offset;
}

static void sn0_node_map_boot_storage(SN0NodeState *node,
                                      unsigned int node_index)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    g_autofree char *flash_name =
        g_strdup_printf("sn0.node%u.flash", node_index);
    g_autofree char *hub_name =
        g_strdup_printf("sn0.node%u.flash.hub", node_index);
    g_autofree char *rboot_name =
        g_strdup_printf("sn0.node%u.flash.rboot", node_index);
    g_autofree char *ecc_absent_name =
        g_strdup_printf("sn0.node%u.backdoor.ecc.absent", node_index);
    g_autofree char *ecc_name =
        g_strdup_printf("sn0.node%u.backdoor.ecc", node_index);
    g_autofree char *dir_absent_name =
        g_strdup_printf("sn0.node%u.backdoor.dir.absent", node_index);
    g_autofree char *dir_name =
        g_strdup_printf("sn0.node%u.backdoor.dir", node_index);

    /*
     * The boot flash overlaps RAM at the reset-vector offset and wins. Its
     * Hub-special LBOOT and RBOOT mappings are views of the same physical
     * chip, so guest writes retained by the flash are visible through all
     * three addresses.
     */
    memory_region_init_rom_device(&node->flash, OBJECT(node), &sn0_flash_ops,
                                  node, flash_name, SN0_FLASH_SIZE,
                                  &error_fatal);
    memory_region_add_subregion_overlap(system, node_base + SN0_FLASH_LOCAL,
                                        &node->flash, 1);
    qemu_register_reset(sn0_flash_reset, node);

    memory_region_init_io(&node->flash_hub_view, OBJECT(node),
                          &sn0_boot_mirror_ops, node, hub_name,
                          SN0_LBOOT_SIZE);
    memory_region_add_subregion(system,
                                SN0_SPACE_HUB + node_base + SN0_FLASH_HUB,
                                &node->flash_hub_view);

    memory_region_init_io(&node->flash_rboot_view, OBJECT(node),
                          &sn0_boot_mirror_ops, node, rboot_name,
                          SN0_RBOOT_SIZE);
    memory_region_add_subregion(system,
                                SN0_SPACE_HUB + node_base + SN0_FLASH_RBOOT,
                                &node->flash_rboot_view);

    /*
     * Fixed absent floors cover the complete ECC and directory backdoor
     * windows. Storage for the populated banks overlays those floors.
     */
    memory_region_init_io(&node->backdoor_ecc_absent, OBJECT(node),
                          &sn0_backdoor_absent_ops, NULL, ecc_absent_name,
                          SN0_BACKDOOR_ECC_SIZE);
    memory_region_add_subregion_overlap(
        system, SN0_SPACE_HUB + node_base + SN0_BACKDOOR_ECC_HUB,
        &node->backdoor_ecc_absent, -1);
    if (node->populated_banks > 0) {
        uint64_t size = (uint64_t)node->populated_banks *
                        SN0_BACKDOOR_BANK_STRIDE;

        sn0_backdoor_configure(&node->backdoor_ecc_state, node);
        memory_region_init_io(&node->backdoor_ecc, OBJECT(node),
                              &sn0_backdoor_populated_ops,
                              &node->backdoor_ecc_state, ecc_name, size);
        memory_region_add_subregion(
            system, SN0_SPACE_HUB + node_base + SN0_BACKDOOR_ECC_HUB,
            &node->backdoor_ecc);
    }

    memory_region_init_io(&node->backdoor_dir_absent, OBJECT(node),
                          &sn0_backdoor_absent_ops, NULL, dir_absent_name,
                          SN0_BACKDOOR_DIR_SIZE);
    memory_region_add_subregion_overlap(
        system, SN0_SPACE_HUB + node_base + SN0_BACKDOOR_DIR_HUB,
        &node->backdoor_dir_absent, -1);
    if (node->populated_banks > 0) {
        uint64_t size = (uint64_t)node->populated_banks *
                        SN0_BACKDOOR_BANK_STRIDE;

        sn0_backdoor_configure(&node->backdoor_dir_state, node);
        memory_region_init_io(&node->backdoor_dir, OBJECT(node),
                              &sn0_backdoor_populated_ops,
                              &node->backdoor_dir_state, dir_name, size);
        memory_region_add_subregion(
            system, SN0_SPACE_HUB + node_base + SN0_BACKDOOR_DIR_HUB,
            &node->backdoor_dir);
    }
}

static void sn0_node_create_hub(SN0MachineState *machine,
                                SN0NodeState *node,
                                unsigned int node_index,
                                unsigned int cpu_base,
                                unsigned int cpu_count,
                                SGIHubState *network_peer)
{
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

    memory_region_init(&node->xtalk, OBJECT(node), xtalk_name,
                       SN0_XTALK_SIZE);

    /*
     * The Hub's block transfer engines address memory the way this node's
     * processors do: an address without a NASID names this node, and an
     * explicit NASID still reaches either node through the global map
     * underneath. Built here, once per node, in the same shape as each
     * processor's memory-space local view -- the Hub cannot build it for
     * itself, because under real firmware its programmed node number reads
     * zero on every node. The per-processor pieces of that view -- the
     * slice-flipped Hub-private halves and the cache-only firmware data
     * filter -- are left out on purpose: they belong to a reading
     * processor, and a device has no slice to flip and no cache to be
     * private in.
     */
    memory_region_init(&node->hub_memory, OBJECT(node), memory_name,
                       UINT64_MAX);
    memory_region_init_alias(&node->hub_memory_system, OBJECT(node),
                             memory_system_name, system, 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&node->hub_memory, 0,
                                        &node->hub_memory_system, -1);
    memory_region_init_alias(&node->hub_memory_local, OBJECT(node),
                             memory_local_name, system,
                             SN0_SPACE_MEMORY +
                             sn0_node_local_memory_base(MACHINE(machine),
                                                        node, node_index),
                             SN0_NODE_SIZE);
    memory_region_add_subregion_overlap(&node->hub_memory, SN0_SPACE_MEMORY,
                                        &node->hub_memory_local, 0);
    object_property_set_link(OBJECT(&node->hub), "memory",
                             OBJECT(&node->hub_memory), &error_fatal);

    machine->diagnostic_sink_context[node_index].machine = machine;
    machine->diagnostic_sink_context[node_index].node = node_index;
    machine->diagnostic_sink[node_index].record = sn0_diagnostic_record;
    machine->diagnostic_sink[node_index].opaque =
        &machine->diagnostic_sink_context[node_index];
    if (machine->diagnostic_recording) {
        node->hub.diagnostic = &machine->diagnostic_sink[node_index];
    }

    qdev_prop_set_uint32(DEVICE(&node->hub), "nasid",
                         machine->synthesized_firmware ? node->nasid : 0);
    qdev_prop_set_bit(DEVICE(&node->hub), "slave-module", node_index != 0);
    qdev_prop_set_uint32(DEVICE(&node->hub), "cpus", cpu_count);
    qdev_prop_set_uint32(DEVICE(&node->hub), "cpu-base", cpu_base);
    qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                        machine->elsc_dip_switches);
    qdev_prop_set_uint32(DEVICE(&node->hub), "xtalk-widget-shift",
                         SN0_XTALK_WIDGET_SHIFT);
    if (network_peer) {
        qdev_prop_set_bit(DEVICE(&node->hub), "network-link-up", true);
        object_property_set_link(OBJECT(&node->hub), "network-peer",
                                 OBJECT(network_peer), &error_fatal);
    }
    object_property_set_link(OBJECT(&node->hub), "xtalk",
                             OBJECT(&node->xtalk), &error_fatal);

    /*
     * The bring-up UART is a node-0 diagnostic device, not part of either
     * shipped chassis. Keeping it on the console node also avoids assigning
     * one character backend to two frontends.
     */
    qdev_prop_set_bit(DEVICE(&node->hub), "junk-uart",
                      node_index == 0 && machine->junk_uart_present);
    if (node_index == 0 && machine->junk_uart_present &&
        serial_hd(SN0_JUNK_UART_CHARDEV)) {
        object_property_set_link(OBJECT(&node->hub), "junk-uart-chardev",
                                 OBJECT(serial_hd(SN0_JUNK_UART_CHARDEV)),
                                 &error_fatal);
    }
    sysbus_realize(SYS_BUS_DEVICE(&node->hub), &error_fatal);

    cpu = node->cpu[0];
    sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 0,
                       cpu->env.irq[SN0_HUB_BANK0_CAUSE_BIT - CP0Ca_IP]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 1,
                       cpu->env.irq[SN0_HUB_BANK1_CAUSE_BIT - CP0Ca_IP]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 2,
                       cpu->env.irq[SN0_HUB_RT_CAUSE_BIT - CP0Ca_IP]);
    if (cpu_count == SN0_CPUS_PER_NODE) {
        cpu = node->cpu[1];
        sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 4,
                           cpu->env.irq[SN0_HUB_BANK0_CAUSE_BIT - CP0Ca_IP]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 5,
                           cpu->env.irq[SN0_HUB_BANK1_CAUSE_BIT - CP0Ca_IP]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 3,
                           cpu->env.irq[SN0_HUB_RT_CAUSE_BIT - CP0Ca_IP]);
    }
}

static void sn0_node_map_xtalk_windows(SN0NodeState *node,
                                       unsigned int node_index)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    unsigned int widget;

    for (widget = SN0_WIDGET0;
         widget <= SN0_XTALK_PORT_LAST; widget++) {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.xtalk.small%u",
                            node_index, widget);

        memory_region_init_alias(&node->small_window[widget], OBJECT(node),
                                 name, &node->xtalk,
                                 SN0_XTALK_BASE(widget),
                                 SN0_SMALL_WINDOW_SIZE);
        memory_region_add_subregion(
            system, SN0_SPACE_XTALK + node_base + SN0_SMALL_WINDOW(widget),
            &node->small_window[widget]);
    }

    memory_region_add_subregion(
        system, SN0_SPACE_XTALK + node_base + SN0_BIG_WINDOW_BASE,
        &node->hub.bwin);
    memory_region_add_subregion(
        system, SN0_SPACE_XTALK + node_base + SN0_HUB_LOCAL_WINDOW,
        &node->hub.regs);

    {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.hub.explicit", node_index);

        memory_region_init_alias(&node->hub_regs_node_view, OBJECT(node),
                                 name, &node->hub.regs, 0,
                                 SGI_HUB_WINDOW_SIZE);
    }
    memory_region_add_subregion(
        system, SN0_SPACE_XTALK + node_base + SN0_HUB_NODE_WINDOW,
        &node->hub_regs_node_view);
}

static void sn0_create_base_io(SN0MachineState *s, unsigned int node_index)
{
    PCIDevice *ioc3;
    MACAddr mac = { };
    NICInfo *nd;
    unsigned int slot;

    ioc3 = pci_new(PCI_DEVFN(SN0_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
    if (node_index == 0) {
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-a",
                          serial_hd(SN0_IOC3_SERIAL_A_CHARDEV));
        qdev_prop_set_chr(DEVICE(ioc3), "chardev-b",
                          serial_hd(SN0_IOC3_SERIAL_B_CHARDEV));
    }

    /*
     * Each controller claims its host network backend itself, so the board
     * only adjusts the automatic address before that claim: QEMU supplies the
     * unique suffix, while an on-board IOC3 needs SGI's OUI because the PROM
     * reads it from the one-wire part before it starts the network loader. An
     * explicit mac= is a replacement part's programmed identity and stays
     * intact. An unbacked controller still has that physical identity.
     */
    nd = qemu_find_nic_info(TYPE_SGI_IOC3_ETH, true, NULL);
    if (nd && !nd->macaddr_explicit) {
        nd->macaddr.a[0] = 0x08;
        nd->macaddr.a[1] = 0x00;
        nd->macaddr.a[2] = 0x69;
    } else if (!nd) {
        qemu_macaddr_default_if_unset(&mac);
        mac.a[0] = 0x08;
        mac.a[1] = 0x00;
        mac.a[2] = 0x69;
        qdev_prop_set_macaddr(DEVICE(ioc3), "mac", mac.a);
    }

    /*
     * The timekeeper's battery, optional and per node: each base I/O board
     * carries its own, so the id is derived from node_index rather than
     * fixed, the same way a two-node machine's two controllers are never
     * confused for each other anywhere else in this file. Unnamed, nothing
     * changes -- the timekeeper stays exactly as ephemeral as it always was.
     */
    {
        g_autofree char *nvram_id = g_strdup_printf("sgi-nvram%u", node_index);
        BlockBackend *nvram_blk = blk_by_name(nvram_id);

        if (nvram_blk) {
            qdev_prop_set_drive(DEVICE(ioc3), "nvram-drive", nvram_blk);
        }
    }

    pci_realize_and_unref(ioc3,
                          PCI_HOST_BRIDGE(&s->io_bridge[node_index])->bus,
                          &error_fatal);
    s->pci_ioc3[node_index] = ioc3;

    /*
     * The controller's second interrupt does not reach the Bridge through
     * PCI's own INTx routing: sgi_bridge_map_irq() has no rule for it other
     * than the slot-equals-pin default, which would land it on pin 2
     * alongside the controller's own ethernet interrupt, reached through the
     * chip's ordinary PCI interrupt pin, rather than the pin the board
     * actually wires the second one to. Connected directly to the Bridge's
     * matching input instead, wired regardless of whether the bridge is the
     * machine's default path to the controller, the same way the bridge's
     * fabric link is always set: the wire exists on the real board whether or
     * not a guest currently reaches the controller through the bridge's own
     * window.
     */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(&s->io_bridge[node_index]),
                                           SN0_IOC3_SUPERIO_PIN));

    if (s->io_bridge_present) {
        memory_region_add_subregion_overlap(
            &s->nodes[node_index].xtalk, SN0_XTALK_BASE(SN0_CONSOLE_WIDGET),
            sysbus_mmio_get_region(
                SYS_BUS_DEVICE(&s->io_bridge[node_index]), 0), -1);

        /*
         * The two SCSI adapters the base I/O board carries, at the first two
         * PCI devices. They exist only when the bridge is asked for, because
         * a guest can only reach them through the bridge's own per-device
         * decode — unlike the controller above, nothing here gives them a
         * direct fallback mapping, so without the bridge there is nowhere a
         * guest could reach them at.
         *
         * They are created with no drive attached. A machine that invented
         * backing storage would be describing a disk nobody asked for, and the
         * adapters answer their register contract with an empty bus — a disk
         * arrives on the command line, behind the first adapter, when a guest
         * needs to boot from one.
         *
         * sn0_assign_pci_resources assigns each resource at reset rather than
         * here — see that function for why realize is the wrong point to do
         * it.
         */
        for (slot = 0; slot < SN0_SCSI_ADAPTERS; slot++) {
            s->pci_scsi[node_index][slot] = pci_create_simple(
                PCI_HOST_BRIDGE(&s->io_bridge[node_index])->bus,
                PCI_DEVFN(SN0_SCSI_DEVICE0 + slot, 0), "qlogic-isp1040");
        }
    } else {
        if (node_index == 0) {
            warn_report(
                "origin200: the base I/O controller is mapped directly "
                "at its crosstalk address; without synthesized firmware "
                "to assign it PCI resources the way real ARCS PROM does, "
                "the I/O bridge cannot yet carry it to a booting guest "
                "(an approved interim, see task #43)");
        }
        memory_region_add_subregion(
            &s->nodes[node_index].xtalk,
            SN0_XTALK_BASE(SN0_CONSOLE_WIDGET) + SN0_BRIDGE_DEVICE2,
            &SGI_IOC3(ioc3)->bar);
    }
}

static void sn0_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    MemoryRegion *system = get_system_memory();
    hwaddr node = SN0_NODE_BASE(0);
    uint8_t *flash, *fw;
    hwaddr fw_vaddr;
    uint64_t kernel_page = 0, kernel_pages = 0;
    uint64_t ram_offset = 0;
    unsigned int node_count, node_index;

    node_count = machine->numa_state && machine->numa_state->num_nodes ?
                 machine->numa_state->num_nodes : 1;
    if (node_count > SN0_ORIGIN200_MAX_NODES) {
        error_report("origin200: at most two NUMA nodes are supported");
        exit(EXIT_FAILURE);
    }

    if (node_count == 1) {
        if (machine->smp.sockets != 1 ||
            machine->smp.cores > SN0_CPUS_PER_NODE ||
            machine->smp.threads != 1) {
            error_report("origin200: one chassis requires one socket with at "
                         "most two cores and one thread per core");
            exit(EXIT_FAILURE);
        }
    } else if (machine->smp.cpus != 4 || machine->smp.sockets != 2 ||
               machine->smp.cores != 2 || machine->smp.threads != 1) {
        error_report("origin200: two chassis require four CPUs as two "
                     "sockets with two cores and one thread each");
        exit(EXIT_FAILURE);
    }

    sn0_possible_cpu_arch_ids(machine);
    if (machine->numa_state && machine->numa_state->num_nodes) {
        for (node_index = 0; node_index < machine->smp.cpus; node_index++) {
            CPUArchId *cpu_slot = &machine->possible_cpus->cpus[node_index];
            unsigned int expected = node_index / SN0_CPUS_PER_NODE;

            if (!cpu_slot->props.has_node_id ||
                cpu_slot->props.node_id != expected) {
                error_report("origin200: CPU %u must be assigned to NUMA "
                             "node %u", node_index, expected);
                exit(EXIT_FAILURE);
            }
        }
    }

    /*
     * Fatal rather than a warning if this cannot be armed: a machine that
     * silently fails to show the codes is indistinguishable from firmware
     * that never wrote one, and that ambiguity is the whole defect being
     * fixed here.
     */
    if (s->debug_leds) {
        sn0_show_indicator_codes(&error_fatal);
    }

    if (node_count == 2) {
        object_initialize_child(OBJECT(machine), "node1", &s->nodes[1],
                                TYPE_SN0_NODE);
        s->nodes[1].nasid = 1;
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        uint64_t ram_size = node_count == 1 ? machine->ram_size :
                            machine->numa_state->nodes[node_index].node_mem;
        unsigned int cpu_base = node_index * SN0_CPUS_PER_NODE;
        unsigned int cpu_count = node_count == 1 ? machine->smp.cpus :
                                 SN0_CPUS_PER_NODE;
        SGIHubState *network_peer = node_count == 2 ?
            &s->nodes[1 - node_index].hub : NULL;

        sn0_node_create_cpus(machine, &s->nodes[node_index], node_index,
                             cpu_base, cpu_count, ram_offset);
        ram_offset = sn0_node_map_memory(machine, &s->nodes[node_index],
                                         node_index, ram_offset, ram_size);
        sn0_node_map_boot_storage(&s->nodes[node_index], node_index);
        sn0_node_create_hub(s, &s->nodes[node_index], node_index, cpu_base,
                            cpu_count, network_peer);
    }
    g_assert(ram_offset == machine->ram_size);

    /*
     * A firmware image, if one was supplied, replaces what this file would
     * otherwise synthesize. Both go into the same buffer and reach the flash as
     * one blob, so there is never a question of which of two writers lands
     * last. An image shorter than the flash leaves the synthesized bytes behind
     * it, which is what a partly programmed flash looks like.
     */
    /*
     * The I/O bridge. It carries a real PCI bus, and the base I/O controller
     * below is a PCI device that cannot exist without one to realize onto —
     * so the chip is always built, whether or not this machine is asked for
     * an I/O bridge. What "asked for" gates is whether the chip's own
     * register window is mapped into crosstalk space at all.
     *
     * That neutrality is about guest reachability specifically, not about
     * the machine as a whole: unmapped, the chip is exactly as unreachable
     * as it always was, because nothing reaches a MemoryRegion nobody
     * subregioned into anything the guest can address. The topology is not
     * neutral the same way. The bridge and the controller now always exist
     * in QOM and always take part in reset, whether or not a guest can see
     * them — a qtest, `info qtree`, or anything else that introspects the
     * machine sees the always-realized bus even with the option off, which
     * it did not before this change.
     *
     * That has one consequence worth stating plainly because migration is a
     * core workflow: a snapshot taken on a build from before this change
     * cannot be loaded on a build from after it, in either direction. Both
     * devices' own VMStateDescriptions gate this correctly — the bridge and
     * the controller take part in a machine snapshot only once they exist as
     * QOM devices, so their state was simply absent from any snapshot taken
     * before this commit, and the controller's own version bumps since
     * (task #47) set minimum_version_id equal to version_id, which QEMU
     * treats as an unconditional reject rather than a best-effort load. This
     * is not a smaller or larger snapshot; it is an incompatible one.
     *
     * The base I/O controller used to be mapped directly at its crosstalk
     * address, and that direct mapping shadowed the bridge's own window for
     * the third device on its bus — a guest programming that window to reach
     * a different device (the first SCSI adapter's own window landed there)
     * had its store swallowed by the controller's overlay instead. Putting
     * the controller on the bus as a real PCI device retires that overlay
     * when the bridge is asked for: the window belongs to whichever device
     * is actually there, discovered the same way a guest discovers any other
     * PCI device, rather than being reserved for one of them in advance.
     *
     * The controller's crosstalk-side address does not move either way: the
     * bridge aims each slot's window by slot number, not by the PCI address
     * a guest assigns the device's BAR, so the third slot's window sits at
     * the same offset the configuration graph's console descriptor already
     * names. With the bridge unasked-for, the controller's BAR is instead
     * subregioned into that same address directly, bypassing the bridge's
     * own per-device decode entirely — the same placement a system-bus
     * device's region would get, and the reason this is not yet a
     * self-contained fix: task #35's own boot test found that no guest here
     * assigns PCI resources for itself before it needs its console, so
     * without this direct placement a guest reached through the bridge
     * cannot find the controller at all until firmware does that assignment
     * (task #43). This whole branch — the direct subregion, and the warning
     * naming it an interim — is deleted once firmware can.
     */
    object_initialize_child(OBJECT(machine), "bridge", &s->io_bridge[0],
                            TYPE_SGI_BRIDGE);
    /*
     * The silicon revision of the I/O bridge fitted to this machine. It is a
     * candidate value rather than a fact about the machine type — two real
     * Origin 200s report different revisions — so the command line can set it
     * and two runs can differ in it and in nothing else.
     *
     * It reaches widget 0 as well as widget 8 without being written twice,
     * because those are one chip and this is the one place the machine says
     * which chip it fitted.
     */
    qdev_prop_set_uint32(DEVICE(&s->io_bridge[0]), "revision",
                         s->widget0_revision);
    /*
     * Every physical BaseIO board has its own laser-programmed one-wire
     * serial. These stable model values are consecutive only by choice; the
     * first preserves the identity this machine exposed before it supported a
     * second chassis, while the second prevents firmware from merging two
     * physical nodes that fall back to their BaseIO identities.
     */
    qdev_prop_set_uint64(DEVICE(&s->io_bridge[0]), "onewire-serial",
                         SN0_BRIDGE_ONEWIRE_SERIAL_BASE);
    /*
     * The fabric its interrupt packets travel, and how this machine encodes
     * a crosstalk destination in it. A pin rise becomes a write to the
     * Hub's interrupt-modify register on the named node, and it goes
     * through the machine's own address space rather than by calling into
     * the Hub — the packet is a real transfer across the bus and modelling
     * it as a function call would invent a wire and step over the fabric.
     *
     * The chip knows the packet's format; this knows where a node's Hub
     * lives, which is why the encoding arrives from here. Set even when the
     * bridge is not asked for, because realize requires it and nothing here
     * ever drives a packet through an unmapped chip's fabric link anyway.
     */
    object_property_set_link(OBJECT(&s->io_bridge[0]), "fabric",
                             OBJECT(system), &error_fatal);
    qdev_prop_set_uint64(DEVICE(&s->io_bridge[0]), "fabric-base",
                         SN0_SPACE_XTALK);
    qdev_prop_set_uint32(DEVICE(&s->io_bridge[0]), "fabric-node-shift",
                         SN0_NODE_SHIFT);
    sysbus_realize(SYS_BUS_DEVICE(&s->io_bridge[0]), &error_fatal);

    /*
     * Widget 0, which is this same bridge and not a second device.
     *
     * A real Origin 200 has no crossbar. The Hub has one crosstalk link with
     * the bridge on the far end of it, and with no switch in between there is
     * nothing to route by widget number — so every widget address the Hub
     * emits arrives at the same chip. SGI's own firmware says so where it
     * probes that link: a directly connected bridge can be given whichever
     * widget number the firmware chooses, and it chooses 8, avoiding 0 because
     * something about error handling there is a problem. Widget 8 is therefore
     * an agreement between two firmware routines rather than a position.
     *
     * So this is an alias and not a device. Every guest reads widget 0's
     * identification register first and decides what kind of machine it is on
     * from the answer, and a separate responder holding a copy of the bridge's
     * identity would be two objects asserting one chip's — which cannot be
     * kept in step by inspection, only by remembering to. An alias cannot
     * disagree: there is nothing there to hold a second opinion.
     *
     * Unconditional, unlike the mapping at widget 8 below. The bridge chip is
     * always built and the machine option decides only whether its own widget
     * answers, so widget 0 keeps answering with the option off. That matters:
     * OpenBSD panics outright if widget 0's identification register does not
     * read, so gating this would turn a development option into a machine no
     * guest can boot.
     *
     * Widgets 9 through 15 are deliberately not aliased here. Real hardware
     * would answer at those addresses too, by the same argument, but nothing
     * reaches for them and making an absent widget answer would remove the
     * only signal that tells one from a present one.
     */
    memory_region_init_alias(&s->widget0[0], NULL, "sn0.xtalk.widget0",
                             sysbus_mmio_get_region(
                                 SYS_BUS_DEVICE(&s->io_bridge[0]), 0),
                             0, SGI_BRIDGE_WIDGET_SIZE);
    memory_region_add_subregion(&s->nodes[0].xtalk,
                                SN0_XTALK_BASE(SN0_WIDGET0),
                                &s->widget0[0]);

    if (node_count == 2) {
        object_initialize_child(OBJECT(machine), "bridge1", &s->io_bridge[1],
                                TYPE_SGI_BRIDGE);
        qdev_prop_set_uint32(DEVICE(&s->io_bridge[1]), "revision",
                             s->widget0_revision);
        qdev_prop_set_uint64(DEVICE(&s->io_bridge[1]), "onewire-serial",
                             SN0_BRIDGE_ONEWIRE_SERIAL_BASE + 1);
        qdev_prop_set_string(DEVICE(&s->io_bridge[1]), "pci-bus-name",
                             "pci.1");
        object_property_set_link(OBJECT(&s->io_bridge[1]), "fabric",
                                 OBJECT(system), &error_fatal);
        qdev_prop_set_uint64(DEVICE(&s->io_bridge[1]), "fabric-base",
                             SN0_SPACE_XTALK);
        qdev_prop_set_uint32(DEVICE(&s->io_bridge[1]), "fabric-node-shift",
                             SN0_NODE_SHIFT);
        sysbus_realize(SYS_BUS_DEVICE(&s->io_bridge[1]), &error_fatal);

        memory_region_init_alias(
            &s->widget0[1], NULL, "sn0.node1.xtalk.widget0",
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io_bridge[1]), 0),
            0, SGI_BRIDGE_WIDGET_SIZE);
        memory_region_add_subregion(&s->nodes[1].xtalk,
                                    SN0_XTALK_BASE(SN0_WIDGET0),
                                    &s->widget0[1]);
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        sn0_create_base_io(s, node_index);
    }

    for (node_index = 0; node_index < node_count; node_index++) {
        sn0_node_map_xtalk_windows(&s->nodes[node_index], node_index);
    }

    flash = g_malloc0(SN0_FLASH_SIZE);
    sn0_build_flash(flash);

    if (machine->firmware) {
        char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        ssize_t size = path ? load_image_size(path, flash, SN0_FLASH_SIZE) : -1;

        if (size < 0) {
            error_report("origin200: cannot read the firmware image '%s'",
                         machine->firmware);
            exit(EXIT_FAILURE);
        }
        g_free(path);
    }
    sn0_configure_flash_for_origin200(flash);

    /*
     * The firmware this machine presents, unless it was told to stand aside for
     * a real image. The trap region, the stubs the vector entries point at, and
     * the vector itself go together: any subset of them is a half-firmware that
     * is worse than either state.
     */
    fw = g_malloc0(SN0_FW_TEXT_SIZE);
    fw_vaddr = (hwaddr)(int32_t)(0xa0000000 + SN0_FW_TEXT_LOCAL);

    if (s->synthesized_firmware) {
        uint64_t vector[SN0_ARCS_ENTRIES];
        uint64_t private_vector[SN0_ARCS_PRIVATE_ENTRIES];
        hwaddr trap_vaddr = (hwaddr)(int32_t)(0xa0000000 + SN0_ARCS_TRAP_LOCAL);

        s->console = serial_hd(SN0_IOC3_SERIAL_A_CHARDEV);

        sn0_build_arcs(fw, fw_vaddr, trap_vaddr, vector, SN0_ARCS_ENTRIES,
                       0, 0);

        /*
         * The SGI-private call vector. IRIX and the SGI standalone shell
         * need it; the guests we boot never look at it. Every entry not
         * separately dispatched falls to the shared trap handler's default
         * case, which logs an unimplemented call rather than jumping to
         * garbage — the same treatment the main vector's own unimplemented
         * entries get.
         */
        sn0_build_arcs(fw, fw_vaddr, trap_vaddr, private_vector,
                       SN0_ARCS_PRIVATE_ENTRIES, SN0_ARCS_ENTRIES,
                       SN0_ARCS_PRIVATE_BASE_OP);
        for (node_index = 0; node_index < node_count; node_index++) {
            SN0NodeState *node_state = &s->nodes[node_index];
            hwaddr node_base = SN0_NODE_BASE(node_state->nasid);
            g_autofree char *trap_name =
                g_strdup_printf("sn0.node%u.arcs.trap", node_index);
            g_autofree char *vector_name =
                g_strdup_printf("sn0.node%u.arcs.vector", node_index);
            g_autofree char *private_name =
                g_strdup_printf("sn0.node%u.arcs.private", node_index);

            memory_region_init_io(&node_state->arcs_trap, OBJECT(node_state),
                                  &sn0_arcs_ops, s, trap_name,
                                  SN0_ARCS_TRAP_SIZE);
            memory_region_add_subregion(
                system, node_base + SN0_ARCS_TRAP_LOCAL,
                &node_state->arcs_trap);
            rom_add_blob_fixed(vector_name, vector, sizeof(vector),
                               node_base + SN0_ARCS_VECTOR);
            rom_add_blob_fixed(private_name, private_vector,
                               sizeof(private_vector),
                               node_base + SN0_ARCS_PRIVATE_VECTOR);
        }

        if (machine->kernel_filename) {
            uint8_t *secondary = fw + SN0_FW_SECONDARY_BOOT;

            sn0_build_trap_stub(fw + SN0_FW_LAUNCH_STUB,
                               trap_vaddr + SN0_FIRMWARE_LAUNCH_OP * 8);
            /*
             * Remote slaves can execute the kernel's 64-bit FPU loads before
             * its per-node exception vectors are live. Match the execution
             * mode supplied by PROM for that transition; Linux replaces the
             * status word in setup_c0_status_sec immediately afterwards.
             */
            stl_be_p(secondary, 0x3c1a3400); /* lui k0, CU0|CU1|FR */
            stl_be_p(secondary + 4,
                     0x375a0000 | (1 << CP0St_KX)); /* ori k0, k0, KX */
            stl_be_p(secondary + 8, 0x409a6000); /* mtc0 k0, CP0_Status */
            stl_be_p(secondary + 12, 0x03600008); /* jr k1 */
            stl_be_p(secondary + 16, 0);
            sn0_flash_jump_to_firmware(
                flash, SN0_FLASH_LAUNCHSLAVE,
                fw_vaddr + SN0_FW_LAUNCH_STUB);
        }
    }

    /*
     * A kernel, loaded straight from an ELF image, which is the boot path every
     * test and every guest we care about uses. Booting from a disk is the
     * firmware's job and arrives with the disk.
     */
    if (machine->kernel_filename && sn0_kernel_file_is_ecoff(machine->kernel_filename)) {
        uint64_t entry, gp_value, low, high;
        hwaddr scratch, argv0_str, argv_array, envp_array, sp;
        size_t name_len = strlen(machine->kernel_filename) + 1;
        uint32_t argv0_ptr, zero32 = 0;

        sn0_load_ecoff(machine->kernel_filename, node,
                       &entry, &gp_value, &low, &high);

        /*
         * argv/envp/stack for the handoff, in sash's own node-local address
         * space (added to node only where memory is actually populated
         * below), placed a fixed gap above wherever this image's own
         * sections ended -- never a hard-coded address, since the two IRIX
         * releases this project holds ship differently sized sashARCS
         * images.
         */
        scratch = ROUND_UP(high, SN0_SASH_SCRATCH_GAP) + SN0_SASH_SCRATCH_GAP;
        argv0_str = scratch;
        argv_array = argv0_str + ROUND_UP(name_len, sizeof(uint32_t));
        envp_array = argv_array + 2 * sizeof(uint32_t);
        sp = ROUND_UP(envp_array + sizeof(uint32_t), 16) + SN0_SASH_STACK_SIZE;

        argv0_ptr = cpu_to_be32(argv0_str);
        rom_add_blob_fixed("sn0.sash.argv0", machine->kernel_filename,
                           name_len, node + argv0_str);
        rom_add_blob_fixed("sn0.sash.argv", &argv0_ptr, sizeof(argv0_ptr),
                           node + argv_array);
        rom_add_blob_fixed("sn0.sash.argv.nul", &zero32, sizeof(zero32),
                           node + argv_array + sizeof(uint32_t));
        rom_add_blob_fixed("sn0.sash.envp.nul", &zero32, sizeof(zero32),
                           node + envp_array);

        sn0_build_sash_handoff(flash, fw, fw_vaddr, entry, gp_value, 1,
                               argv_array, envp_array, sp);

        kernel_page = low / 0x1000;
        kernel_pages = (high - low + 0xfff) / 0x1000;
    } else if (machine->kernel_filename) {
        uint64_t entry, low, high;
        ssize_t size = load_elf(machine->kernel_filename, NULL,
                                sn0_elf_to_phys, NULL, &entry, &low, &high,
                                NULL, ELFDATA2MSB, EM_MIPS, 1, 0);

        if (size < 0) {
            error_report("origin200: cannot load the kernel '%s'",
                         machine->kernel_filename);
            exit(EXIT_FAILURE);
        }

        /*
         * One argument, the kernel's own name, which is what a firmware that
         * loaded it by name would pass. Nothing on the path to a console reads
         * it, but the count and the vector are both read, so both are real.
         */
        rom_add_blob_fixed("sn0.argv.string", machine->kernel_filename,
                           strlen(machine->kernel_filename) + 1,
                           node + SN0_ARGV_STRING);
        {
            uint64_t argv0 = cpu_to_be64(SN0_COMPAT_CACHED + SN0_ARGV_STRING);

            rom_add_blob_fixed("sn0.argv", &argv0, sizeof(argv0),
                               node + SN0_ARGV_ADDR);
        }

        sn0_build_kernel_handoff(flash, fw, fw_vaddr, entry, 1,
                                 SN0_COMPAT_CACHED + SN0_ARGV_ADDR);

        kernel_page = low / 0x1000;
        kernel_pages = (high - low + 0xfff) / 0x1000;
    }

    /*
     * The structures go in last, because the memory descriptors have to say
     * where the kernel landed and how much of memory it took.
     */
    if (s->synthesized_firmware) {
        for (node_index = 0; node_index < node_count; node_index++) {
            sn0_build_structures(machine, node_index,
                                 node_index == 0 ? kernel_page : 0,
                                 node_index == 0 ? kernel_pages : 0);
        }
    }

    /*
     * Initialize the chip once. rom_add_blob_fixed() would register the
     * contents as a ROM and copy them back on every machine reset, erasing
     * environment changes the PROM deliberately retains across that reset.
     */
    for (node_index = 0; node_index < node_count; node_index++) {
        memcpy(memory_region_get_ram_ptr(&s->nodes[node_index].flash), flash,
               SN0_FLASH_SIZE);
        memory_region_set_dirty(&s->nodes[node_index].flash, 0,
                                SN0_FLASH_SIZE);
    }
    g_free(flash);

    /*
     * The firmware's own instructions, in a region of the machine's rather than
     * in the flash, so that whatever image the flash holds keeps every byte of
     * it. Read-only, like the flash, because nothing should be writing here.
     */
    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node_state = &s->nodes[node_index];
        hwaddr node_base = SN0_NODE_BASE(node_state->nasid);
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.firmware.text", node_index);

        memory_region_init_rom(&node_state->fw_text, OBJECT(node_state),
                               name, SN0_FW_TEXT_SIZE, &error_fatal);
        memory_region_add_subregion(system, node_base + SN0_FW_TEXT_LOCAL,
                                    &node_state->fw_text);
        rom_add_blob_fixed(name, fw, SN0_FW_TEXT_SIZE,
                           node_base + SN0_FW_TEXT_LOCAL);
    }
    g_free(fw);

    for (node_index = 0; node_index < node_count; node_index++) {
        qdev_realize(DEVICE(&s->nodes[node_index]), NULL, &error_fatal);
    }

    qemu_add_machine_init_done_notifier(&sn0_machine_ready_notifier);
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

static void sn0_node_instance_init(Object *obj)
{
    SN0NodeState *node = SN0_NODE(obj);

    object_initialize_child(obj, "hub", &node->hub, TYPE_SGI_HUB);
}

static void sn0_node_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_sn0_flash;
}

static void sn0_instance_init(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);

    object_initialize_child(obj, "node0", &s->nodes[0], TYPE_SN0_NODE);
    s->nodes[0].nasid = 0;
    s->synthesized_firmware = true;
    s->io_bridge_present = true;
    s->debug_leds = true;
    s->widget0_revision = SN0_WIDGET0_REVISION;
}

static void sn0_instance_finalize(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);

    g_free(s->diagnostic_events);
    g_free(s->diagnostic_read_cache);
}

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Origin 200 (IP27)";
    mc->init = sn0_init;
    mc->max_cpus = SN0_CPUS_PER_NODE * SN0_ORIGIN200_MAX_NODES;
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "sn0.node0.ram";
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

    /*
     * The first of the candidate values. A captured boot of this configuration
     * announces revision 3 and that is the default, but another Origin 200's
     * firmware announces 4 — a machine we do not model and whose sources we do
     * not hold, so 3 is this machine's answer rather than the family's.
     * Settable
     * so that the difference can be run rather than argued about, without the
     * two runs also differing in the emulator.
     */
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

static const TypeInfo origin200_type = {
    .name = MACHINE_TYPE_NAME("origin200"),
    .parent = TYPE_SN0_MACHINE,
    .class_init = origin200_class_init,
};

static const TypeInfo sn0_machine_type = {
    .name = TYPE_SN0_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_init = sn0_instance_init,
    .instance_finalize = sn0_instance_finalize,
    .instance_size = sizeof(SN0MachineState),
    .abstract = true,
};

static const TypeInfo sn0_node_type = {
    .name = TYPE_SN0_NODE,
    .parent = TYPE_DEVICE,
    .instance_init = sn0_node_instance_init,
    .instance_size = sizeof(SN0NodeState),
    .class_init = sn0_node_class_init,
};

static void sn0_machine_types(void)
{
    type_register_static(&sn0_node_type);
    type_register_static(&sn0_machine_type);
    type_register_static(&origin200_type);
}

type_init(sn0_machine_types)
