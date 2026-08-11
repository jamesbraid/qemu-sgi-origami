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
#include "hw/core/boards.h"
#include "hw/core/clock.h"
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
#include "hw/misc/sgi-xbow.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/mips/mips.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "cpu.h"
#include "sn0.h"

/*
 * A stock Origin 200 runs its R10000 at 180 MHz and its Hub at 90 MHz. The
 * processor's own counter runs at half the processor clock, which is what the
 * CPU model's counter divisor expresses, so this is the input that counter is
 * derived from.
 */
#define SN0_CPU_REFCLK_HZ (180 * 1000 * 1000)

/*
 * Where the argument vector handed to a kernel goes. Low memory below the
 * firmware structures and above the exception vectors a kernel installs. The
 * structure work owns the final layout of this region; this is the one address
 * in it that exists so far.
 */
#define SN0_ARGV_ADDR   0x800
#define SN0_ARGV_STRING (SN0_ARGV_ADDR + 0x40)

/* What the firmware configuration block says about this machine. */
#define SN0_CPU_FREQ_HZ   180000000
#define SN0_HUB_FREQ_HZ   90000000
#define SN0_RTC_FREQ_HZ   1250000       /* an 800 ns tick */
#define SN0_MACHINE_TYPE  1             /* 0 is Origin 2000, 1 is Origin 200 */

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

struct SN0MachineState {
    MachineState parent_obj;

    MemoryRegion flash;
    MemoryRegion flash_hub_view;
    MemoryRegion hub_regs_node_view;
    MemoryRegion arcs_trap;
    MemoryRegion fw_text;

    /* Crosstalk space, and the small window each widget is reached through. */
    MemoryRegion xtalk;
    MemoryRegion small_window[SN0_XTALK_WIDGETS];

    SGIHubState hub;
    SGIIOC3State ioc3;
    SGIXbowState xbow;
    SGIBridgeState io_bridge;

    /*
     * Whether the I/O bridge is present. It is being built, and until it can
     * carry the base I/O controller on its PCI bus the machine this option
     * turns on is not the machine the contract describes — so the default is
     * off and the default machine is unchanged, region for region. What the
     * option is for is the bridge's own tests, which need the chip reachable
     * and need nothing else about the machine to move.
     */
    bool io_bridge_present;

    /*
     * Whether this machine presents firmware of its own; see the class notes.
     */
    bool synthesized_firmware;

    /* Where the firmware's console output goes. */
    Chardev *console;

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

    /* The memory descriptor list, and where a guest sees it. */
    uint64_t mem_addr;
    unsigned int mem_count;
};
typedef struct SN0MachineState SN0MachineState;

DECLARE_INSTANCE_CHECKER(SN0MachineState, SN0_MACHINE,
                         MACHINE_TYPE_NAME("origin200"))

/*
 * The flash is not programmable from the guest side in this model. Real
 * hardware programs it through a sector-erase protocol that only SGI's own
 * flash utility drives, and nothing on the path to booting an operating system
 * writes here, so a write is reported rather than acted on.
 */
static void sn0_flash_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "sn0: ignoring write of 0x%" PRIx64 " to boot flash offset "
                  "0x%" HWADDR_PRIx "; programming the flash is not modeled\n",
                  val, addr);
}

static uint64_t sn0_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    /*
     * Not reached while the region serves reads from its backing store, which
     * it does for as long as nothing takes it out of that mode. Reported rather
     * than asserted, because a guest must never be able to stop the emulator.
     */
    qemu_log_mask(LOG_UNIMP,
                  "sn0: read of boot flash offset 0x%" HWADDR_PRIx
                  " reached the device path\n", addr);
    return 0;
}

static const MemoryRegionOps sn0_flash_ops = {
    .read = sn0_flash_read,
    .write = sn0_flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};


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
static void sn0_build_kernel_handoff(uint8_t *flash, uint8_t *fw,
                                     hwaddr fw_vaddr, uint64_t entry,
                                     unsigned argc, hwaddr argv)
{
    void *p = fw + SN0_FW_BOOTLOADER;
    hwaddr target = fw_vaddr + SN0_FW_BOOTLOADER;

    stl_be_p(flash, 0x08000000 | ((target >> 2) & 0x03ffffff)); /* j target */
    stl_be_p(flash + 4, 0);                                     /* delay slot */

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

static void sn0_build_flash(uint8_t *flash)
{
    static const uint32_t jr_ra = 0x03e00008;      /* jr $31 */
    static const uint32_t branch_to_self = 0x1000ffff; /* b . */
    uint8_t *config = flash + SN0_CONFIG_OFFSET;
    int i;

    memset(flash, 0, SN0_FLASH_SIZE);

    for (i = 0; i < SN0_FLASH_ENTRIES; i++) {
        uint8_t *entry = flash + i * SN0_FLASH_ENTRY_STRIDE;

        stl_be_p(entry, i == 0 ? branch_to_self : jr_ra);
        stl_be_p(entry + 4, 0);                    /* the delay slot */
    }

    /*
     * The field order, widths and values are the shipped Origin 200 firmware's,
     * so a guest reading this block sees a machine SGI actually built. The
     * timing and mode words are the measured contents of that image rather than
     * anything this model acts on.
     */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, 0x1211a785);           /* processor reset mode */
    stq_be_p(config + 0x08, SN0_CONFIG_MAGIC);
    stq_be_p(config + 0x10, SN0_CPU_FREQ_HZ);
    stq_be_p(config + 0x18, SN0_HUB_FREQ_HZ);
    stq_be_p(config + 0x20, SN0_RTC_FREQ_HZ);
    stl_be_p(config + 0x28, 1);                    /* error correction on */
    stl_be_p(config + 0x2c, 15);                   /* flash read timing */
    stl_be_p(config + 0x30, SN0_MACHINE_TYPE);
    stl_be_p(config + 0x34, 0);                    /* checksum adjustment */
    stl_be_p(config + 0x38, 0);                    /* flash program count */
    stl_be_p(config + 0x3c, 4);                    /* flash write timing */
    stl_be_p(config + 0x40, 6);                    /* firmware version */
    stl_be_p(config + 0x44, 11);                   /* firmware revision */
    stl_be_p(config + 0x48, 0);                    /* configuration flags */
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
        for (i = 0; i < s->mem_count; i++) {
            if (previous == s->mem_addr +
                            i * sizeof(struct sn0_mem_descriptor)) {
                next = i + 1;
                break;
            }
        }
        if (i == s->mem_count) {
            return 0;
        }
    }

    if (next >= s->mem_count) {
        return 0;                       /* the end of the list */
    }
    return s->mem_addr + next * sizeof(struct sn0_mem_descriptor);
}

static MemTxResult sn0_arcs_read(void *opaque, hwaddr addr, uint64_t *val,
                                 unsigned size, MemTxAttrs attrs)
{
    SN0MachineState *s = opaque;
    unsigned int op = addr / 8;
    CPUMIPSState *env;

    *val = ARCS_EINVAL;

    /*
     * A debugger examining memory must not make a firmware call happen as a
     * side effect of looking at it.
     */
    if (attrs.debug) {
        *val = 0;
        return MEMTX_OK;
    }

    /* No processor at all is a state a test harness can produce. */
    if (!current_cpu) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sn0: firmware routine %u reached with no processor\n",
                      op);
        return MEMTX_OK;
    }
    env = cpu_env(current_cpu);

    switch (op) {
    case SN0_ARCS_WRITE:
        *val = sn0_arcs_write(s, env);
        break;
    case SN0_ARCS_GET_MEMORY:
        *val = sn0_arcs_get_memory(s, env);
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
 */
static void sn0_build_arcs(uint8_t *fw, hwaddr fw_vaddr, hwaddr trap_vaddr,
                           uint64_t *vector)
{
    int i;

    for (i = 0; i < SN0_ARCS_ENTRIES; i++) {
        uint8_t *stub = fw + SN0_ARCS_STUBS + i * SN0_ARCS_STUB_SIZE;
        uint64_t slot = trap_vaddr + i * 8;

        stl_be_p(stub, 0x3c010000 | ((slot >> 16) & 0xffff)); /* lui $1, hi */
        stl_be_p(stub + 4, 0xdc220000 | (slot & 0xffff));  /* ld $2, lo($1) */
        stl_be_p(stub + 8, 0x03e00008);                       /* jr $31 */
        stl_be_p(stub + 12, 0);                               /* delay slot */

        vector[i] = cpu_to_be64(fw_vaddr + SN0_ARCS_STUBS +
                                i * SN0_ARCS_STUB_SIZE);
    }
}

/* One component record, with the fields every kind of component shares. */
static void sn0_component(uint8_t *graph, unsigned int index, uint8_t type,
                          uint8_t widget)
{
    uint8_t *c = graph + (SN0_GRAPH_COMPONENTS - SN0_CONFIG_GRAPH) +
                 index * SN0_GRAPH_COMP_STRIDE;

    c[0x00] = type;
    c[0x01] = 1;                                /* record version */
    /* Not optional: a component without this is skipped by every consumer. */
    c[0x02] = SN0_COMP_ENABLED;
    c[0x20] = widget;
    stl_be_p(c + 0x1c, index);                  /* virtual identifier */
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
 * One board record, linked to the next. The list is terminated only by a zero
 * next-board offset: a consumer reads the record first and checks afterwards,
 * so there is no escape from a bad link. A board without the local-board kind
 * is skipped entirely, components and all.
 */
static void sn0_board(uint8_t *graph, unsigned int index, uint8_t type,
                      unsigned int next, unsigned int first_component,
                      unsigned int components)
{
    uint8_t *b = graph + (SN0_GRAPH_BOARDS - SN0_CONFIG_GRAPH) +
                 index * SN0_GRAPH_BOARD_SIZE;
    unsigned int i;

    stl_be_p(b + 0x00, next);
    b[0x04] = SN0_BOARD_LOCAL;
    b[0x05] = type;
    b[0x06] = 1;                                /* record version */
    b[0x17] = components;
    /*
     * A board's components are addressed relative to the board's own node
     * identifier rather than the node whose list is being walked, and that
     * field also indexes a table with no bounds check, so it has to be right
     * and in range on every board even here.
     */
    stw_be_p(b + 0x20, 0);
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
static void sn0_build_graph(MachineState *machine, hwaddr node)
{
    uint8_t *graph = g_malloc0(SN0_CONFIG_GRAPH_SIZE);
    uint8_t *console = graph + SN0_GRAPH_CONSOLE_OFF;
    uint8_t *cpu, *hub, *mem;
    uint64_t total_mb = machine->ram_size / MiB;
    uint64_t chip;

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
     * All three base fields are derived from one chip address rather than
     * composed separately, because composing them separately is how the device
     * window came to be present in one of them and missing from the other two.
     * The three are read by different guests for different purposes, so a
     * correction applied to one does not announce itself to the others.
     */
    chip = 0x9200000000000000ULL |
           ((uint64_t)SN0_CONSOLE_WIDGET << 24) | SN0_BRIDGE_DEVICE2;

    /*
     * The serial-port base, which OpenBSD splits into a bus and a register
     * offset. This is the field carrying the generation trap in its low twelve
     * bits.
     */
    stq_be_p(console + 0x00, chip | SN0_IOC3_SERIAL_A);

    /*
     * The configuration base. No consumer for this field is known and nothing
     * has been observed in it, so the chip's own base is a choice rather than
     * a measurement: the least surprising value, given the fields either side
     * of it are chip-relative. Written down as an open question rather than
     * left looking settled.
     */
    stq_be_p(console + 0x08, chip);

    /*
     * The memory base, the one field in this descriptor Linux reads. It takes
     * this as the base of the controller chip and adds the chip's own register
     * offset itself, validating nothing — no null check, no signature, no
     * timeout. So it needs the device window the serial-port base carries, and
     * it needs to be an address the processor can issue rather than a bare
     * physical one. Wrong here, Linux takes a bus error or spins forever on a
     * transmitter-empty bit that never sets, and both look like a silent boot.
     */
    stq_be_p(console + 0x10, chip);
    stw_be_p(console + 0x18, SN0_CONSOLE_BAUD);
    stl_be_p(console + 0x1c, 0);                /* console type */
    stw_be_p(console + 0x20, 0);                /* node identifier */
    console[0x22] = SN0_CONSOLE_WIDGET;
    console[0x23] = 1;                          /* devices behind the bridge */

    /* Board 0: the node board, with a processor, a Hub and a memory bank. */
    sn0_board(graph, 0, SN0_BOARD_NODE,
              SN0_GRAPH_BOARDS + SN0_GRAPH_BOARD_SIZE, 0, 3);
    sn0_component(graph, 0, SN0_COMP_PROCESSOR, 0);
    sn0_component(graph, 1, SN0_COMP_HUB, 0);
    sn0_component(graph, 2, SN0_COMP_MEMORY, 0);

    /*
     * The processor's own fields, which are the only source a guest has for its
     * clock and cache: with none readable its clock stays zero, which breaks
     * its delay loop, and it dies just after its last printed line. The
     * identification field is sixteen bits, so it carries the low half of the
     * processor identification register and not the whole of it.
     */
    cpu = sn0_component_at(graph, 0);
    stw_be_p(cpu + 0x38, 0x0900);               /* identification, low half */
    /*
     * The floating-point unit's implementation and revision register. It holds
     * the same value as the processor's identification for a reason rather than
     * by a slip of the keyboard: the unit is on the same die and carries the
     * same implementation number, nine, with revision zero. This is what the
     * CPU model reports through that register, so a guest comparing the two
     * finds them agreeing, which is the point of writing it over zero.
     */
    stw_be_p(cpu + 0x3a, 0x0900);
    stw_be_p(cpu + 0x3c, 180);                  /* clock in megahertz */
    stw_be_p(cpu + 0x3e, 1);                    /* secondary cache, megabytes */
    stw_be_p(cpu + 0x40, 90);                   /* secondary cache clock */

    hub = sn0_component_at(graph, 1);
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
    mem = sn0_component_at(graph, 2);
    stw_be_p(mem + 0x38, total_mb);             /* total in megabytes */
    stw_be_p(mem + 0x3a, 0);                    /* bank-to-address mapping */
    stw_be_p(mem + 0x3c, total_mb);             /* one populated bank */

    /*
     * Board 1: the midplane, carrying a module serial number and nothing else,
     * which is what a real Origin 200 emits.
     */
    sn0_board(graph, 1, SN0_BOARD_MIDPLANE,
              SN0_GRAPH_BOARDS + 2 * SN0_GRAPH_BOARD_SIZE, 3, 1);
    sn0_component(graph, 3, SN0_COMP_MODULE_SERIAL, 0);

    /*
     * Board 2: the base-I/O board, and the end of the list. Nothing on the path
     * to the console switch reads it; it is here because the graph has to
     * describe the machine that was actually built, and because the moment a
     * console works this is what a guest walks looking for one.
     */
    sn0_board(graph, 2, SN0_BOARD_BASE_IO, 0, 4, 2);
    sn0_component(graph, 4, SN0_COMP_BRIDGE, SN0_CONSOLE_WIDGET);
    sn0_component(graph, 5, SN0_COMP_IO_CONTROLLER, SN0_CONSOLE_WIDGET);

    rom_add_blob_fixed("sn0.graph", graph, SN0_CONFIG_GRAPH_SIZE,
                       node + SN0_CONFIG_GRAPH);
    g_free(graph);
}

/*
 * The structures a guest reads out of low memory before it has a console. Each
 * is written where the boot structures datasheet puts it, and each carries
 * every field rather than only the ones a guest currently reads: a block that
 * is right only where somebody looks is a trap for whoever comes next.
 */
static void sn0_build_structures(MachineState *machine, hwaddr node,
                                 uint64_t kernel_page, uint64_t kernel_pages)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    struct sn0_mem_descriptor mem[6];
    uint8_t param[0x60] = { };
    uint8_t dir[SN0_HW_DIR_ENTRIES * SN0_HW_DIR_ENTRY_SIZE] = { };
    uint8_t gda[SN0_GDA_SIZE] = { };
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
        default:
            break;
        }
    }
    rom_add_blob_fixed("sn0.hwdir", dir, sizeof(dir), node + SN0_HW_DIRECTORY);

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
                 i == 0 ? 0 : SN0_GDA_NODE_ABSENT);
    }
    rom_add_blob_fixed("sn0.gda", gda, sizeof(gda), node + SN0_GDA);

    sn0_build_graph(machine, node);

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
    s->mem_addr = SN0_COMPAT_CACHED + SN0_MEM_LIST;
    s->mem_count = n;
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

static void sn0_machine_ready(Notifier *notifier, void *data)
{
    qemu_register_reset(sn0_firmware_reset, SN0_MACHINE(qdev_get_machine()));
}

static Notifier sn0_machine_ready_notifier = {
    .notify = sn0_machine_ready,
};

static void sn0_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    MemoryRegion *system = get_system_memory();
    hwaddr node = SN0_NODE_BASE(0);
    Clock *cpuclk;
    MIPSCPU *cpu;
    uint8_t *flash, *fw;
    hwaddr fw_vaddr;
    uint64_t kernel_page = 0, kernel_pages = 0;
    unsigned int w;

    if (machine->ram_size > SN0_NODE_SIZE) {
        error_report("origin200: a node holds at most %" PRIu64 " bytes of "
                     "memory", (uint64_t)SN0_NODE_SIZE);
        exit(EXIT_FAILURE);
    }

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, SN0_CPU_REFCLK_HZ);
    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, true);
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    /*
     * Node memory sits at the bottom of the node's window. A cached reference,
     * an uncached reference carrying the attribute that means memory, and both
     * compatibility segments all arrive here, because that attribute is the one
     * placed at window zero.
     */
    memory_region_add_subregion(system, node, machine->ram);

    /*
     * The boot flash, at the node offset the reset vector reaches through the
     * uncached compatibility segment. It overlaps the first memory bank, which
     * covers the same offset, and wins: that is the only arrangement under
     * which the processor's first fetch finds firmware rather than memory.
     *
     * These regions belong to the machine and live as long as it does, so they
     * have no owner. An owner exists to name a region for migration and has to
     * be a device; a machine is not one, and a machine-lifetime region needs
     * neither the naming nor the lifetime tracking.
     */
    memory_region_init_rom_device(&s->flash, NULL, &sn0_flash_ops,
                                  NULL, "sn0.node0.flash", SN0_FLASH_SIZE,
                                  &error_fatal);
    memory_region_add_subregion_overlap(system, node + SN0_FLASH_LOCAL,
                                        &s->flash, 1);

    /*
     * The same bytes through the Hub's special space, which is how a guest
     * reads the machine type without going through the compatibility segment.
     * One region and an alias, so the two views cannot drift apart.
     */
    memory_region_init_alias(&s->flash_hub_view, NULL,
                             "sn0.node0.flash.hub", &s->flash, 0,
                             SN0_FLASH_SIZE);
    memory_region_add_subregion(system,
                                SN0_SPACE_HUB + node + SN0_FLASH_HUB,
                                &s->flash_hub_view);

    /*
     * A firmware image, if one was supplied, replaces what this file would
     * otherwise synthesize. Both go into the same buffer and reach the flash as
     * one blob, so there is never a question of which of two writers lands
     * last. An image shorter than the flash leaves the synthesized bytes behind
     * it, which is what a partly programmed flash looks like.
     */
    /*
     * Crosstalk space, which every widget lives in and which the node's
     * crosstalk windows are views onto. A widget's registers go in here once,
     * at that widget's own base, and the windows a processor reaches them
     * through are aliases of it — so a device has one address, and the small
     * window and the big window that both reach it cannot come to disagree
     * about what it is.
     */
    memory_region_init(&s->xtalk, NULL, "sn0.xtalk", SN0_XTALK_SIZE);

    /*
     * The crossbar, answering at widget 0 with this machine's identity rather
     * than a crossbar's own. It is reachable only through a big window: widget
     * 0's small window is the one a Hub defect makes unreliable, so it is left
     * unmapped and the guests reach widget 0 through the last big window
     * instead, which is what they expect to find already arranged.
     *
     * What arranges it is the translation table's reset value rather than our
     * firmware's write, which puts back the value already there. The firmware
     * call is kept for being what a real firmware does.
     */
    object_initialize_child(OBJECT(machine), "xbow", &s->xbow, TYPE_SGI_XBOW);
    qdev_prop_set_uint64(DEVICE(&s->xbow), "span", SN0_XTALK_WIDGET_SIZE);
    qdev_prop_set_uint32(DEVICE(&s->xbow), "part", SN0_WIDGET0_PART);
    qdev_prop_set_uint32(DEVICE(&s->xbow), "manufacturer",
                         SN0_WIDGET0_MANUFACTURER);
    qdev_prop_set_uint32(DEVICE(&s->xbow), "revision", s->widget0_revision);
    sysbus_realize(SYS_BUS_DEVICE(&s->xbow), &error_fatal);
    memory_region_add_subregion(&s->xtalk, SN0_XTALK_BASE(SN0_XBOW_WIDGET),
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->xbow), 0));

    /*
     * The Hub's register window, at both halves of the crossbar's widget 1
     * small window. The lower half is how a guest reaches the Hub of whichever
     * node it is running on without first learning its node number: the address
     * carries node zero whoever forms it. The upper half names a node
     * explicitly. On one node the two resolve to the same Hub, and both have to
     * decode, because a guest uses one to read its processor number and the
     * other to clear interrupt state in the same boot.
     */
    object_initialize_child(OBJECT(machine), "hub", &s->hub, TYPE_SGI_HUB);
    qdev_prop_set_uint32(DEVICE(&s->hub), "nasid", 0);
    qdev_prop_set_uint32(DEVICE(&s->hub), "xtalk-widget-shift",
                         SN0_XTALK_WIDGET_SHIFT);
    object_property_set_link(OBJECT(&s->hub), "xtalk", OBJECT(&s->xtalk),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&s->hub), &error_fatal);

    /*
     * The Hub's two pending banks reach the processor on its first two hardware
     * interrupt lines. The processor's own interrupt array is indexed by the
     * position within the Cause register's pending field, so the index is the
     * Cause bit less that field's offset — derived rather than written, because
     * the sources all name the Cause bit and a hand-written index is a silent
     * two-line error with nothing to check it against.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->hub), 0,
                       cpu->env.irq[SN0_HUB_BANK0_CAUSE_BIT - CP0Ca_IP]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->hub), 1,
                       cpu->env.irq[SN0_HUB_BANK1_CAUSE_BIT - CP0Ca_IP]);

    /*
     * The base-I/O controller, at the crosstalk address the configuration graph
     * advertises. What is mapped is the chip's whole window, whose base is the
     * console widget plus the I/O bridge's window for the third device on its
     * bus. The serial ports sit inside that window at their own offsets, so
     * port A lands where the graph's console descriptor points and the two
     * agree by construction rather than by arithmetic done twice.
     *
     * The bridge itself is not modelled, so this reaches the chip without the
     * chip's real parent in the path. That is an interim the machine contract
     * records as an interim: the address a guest issues is right, the reason it
     * decodes is not, and it is revisited when the bridge lands.
     */
    /*
     * Announced rather than left to a comment. A temporary arrangement that
     * nothing says out loud is one somebody later reads as the design, and this
     * one is deliberately wrong about *why* the address decodes while being
     * right about the address itself. It says so on every run, and the line
     * disappears when the bridge makes it true.
     */
    warn_report("origin200: the base I/O controller is mapped directly at its "
                "crosstalk address; the I/O bridge that really decodes it is "
                "not modelled yet (an approved interim)");

    object_initialize_child(OBJECT(machine), "ioc3", &s->ioc3, TYPE_SGI_IOC3);
    qdev_prop_set_chr(DEVICE(&s->ioc3), "chardev-a", serial_hd(0));
    sysbus_realize(SYS_BUS_DEVICE(&s->ioc3), &error_fatal);
    memory_region_add_subregion(&s->xtalk,
                                SN0_XTALK_BASE(SN0_CONSOLE_WIDGET) +
                                SN0_BRIDGE_DEVICE2,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->ioc3), 0));

    /*
     * The I/O bridge, when this machine is asked for one. It is the chip that
     * really decodes the address the controller above answers at, and it is
     * being built; what it can do so far is answer its own register block, so
     * turning it on adds that block at the bottom of the console widget's
     * crosstalk space and changes nothing else.
     *
     * The controller keeps its direct mapping while both are present, which is
     * why the bridge's widget goes underneath it rather than over it. That
     * arrangement is temporary in a specific way worth naming: the controller
     * is at the address of the bridge's own window for the third device on its
     * PCI bus, so when the bridge can carry a PCI bus the controller moves onto
     * it and the overlap goes away rather than being resolved.
     */
    if (s->io_bridge_present) {
        object_initialize_child(OBJECT(machine), "bridge", &s->io_bridge,
                                TYPE_SGI_BRIDGE);
        /*
         * The silicon revision this machine reports for its I/O bridge. It is
         * a candidate value the machine already states once, for the identity
         * widget 0 answers with, and that identity is this chip's — so the
         * chip's own identification register is derived from the same setting
         * rather than carrying a second copy. Two registers reporting different
         * revisions for a chip the machine names once would be asserting
         * something nobody established, and would do it silently.
         */
        qdev_prop_set_uint32(DEVICE(&s->io_bridge), "revision",
                             s->widget0_revision);
        /*
         * The fabric its interrupt packets travel, and how this machine encodes
         * a crosstalk destination in it. A pin rise becomes a write to the
         * Hub's interrupt-modify register on the named node, and it goes
         * through the machine's own address space rather than by calling into
         * the Hub — the packet is a real transfer across the bus and modelling
         * it as a function call would invent a wire and step over the fabric.
         *
         * The chip knows the packet's format; this knows where a node's Hub
         * lives, which is why the encoding arrives from here.
         */
        object_property_set_link(OBJECT(&s->io_bridge), "fabric",
                                 OBJECT(system), &error_fatal);
        qdev_prop_set_uint64(DEVICE(&s->io_bridge), "fabric-base",
                             SN0_SPACE_XTALK);
        qdev_prop_set_uint32(DEVICE(&s->io_bridge), "fabric-node-shift",
                             SN0_NODE_SHIFT);
        sysbus_realize(SYS_BUS_DEVICE(&s->io_bridge), &error_fatal);
        memory_region_add_subregion_overlap(
            &s->xtalk, SN0_XTALK_BASE(SN0_CONSOLE_WIDGET),
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io_bridge), 0), -1);
    }

    /*
     * The crossbar's eight device ports, each reached through its own small
     * window onto the first 16 MB of that widget's crosstalk space. All eight
     * decode because all eight exist on the crossbar; the seven with nothing
     * attached reach an empty part of crosstalk space and fault, which is what
     * a port with no widget on it does. That is deliberately not the same
     * answer a disabled big window gives — a disabled window is a normal state
     * the guests program on purpose, and an empty port is a device we have not
     * modelled yet, so the second should be loud and the first should not.
     */
    for (w = SN0_XTALK_PORT_FIRST; w <= SN0_XTALK_PORT_LAST; w++) {
        char *name = g_strdup_printf("sn0.node0.xtalk.small%u", w);

        memory_region_init_alias(&s->small_window[w], NULL, name, &s->xtalk,
                                 SN0_XTALK_BASE(w), SN0_SMALL_WINDOW_SIZE);
        memory_region_add_subregion(system,
                                    SN0_SPACE_XTALK + node +
                                    SN0_SMALL_WINDOW(w),
                                    &s->small_window[w]);
        g_free(name);
    }

    /*
     * The seven big windows, filling the crosstalk window above the small ones.
     * Where each of them points is the Hub's to say, not the machine's: they
     * follow the Hub's translation table, and this only decides where the set
     * of them lands in the node's address space.
     */
    memory_region_add_subregion(system,
                                SN0_SPACE_XTALK + node + SN0_BIG_WINDOW_BASE,
                                &s->hub.bwin);

    memory_region_add_subregion(system,
                                SN0_SPACE_XTALK + node + SN0_HUB_LOCAL_WINDOW,
                                &s->hub.regs);
    memory_region_init_alias(&s->hub_regs_node_view, NULL,
                             "sn0.node0.hub.explicit", &s->hub.regs, 0,
                             SGI_HUB_WINDOW_SIZE);
    memory_region_add_subregion(system,
                                SN0_SPACE_XTALK + node + SN0_HUB_NODE_WINDOW,
                                &s->hub_regs_node_view);

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

        s->console = serial_hd(0);

        memory_region_init_io(&s->arcs_trap, NULL, &sn0_arcs_ops, s,
                              "sn0.arcs.trap", SN0_ARCS_TRAP_SIZE);
        memory_region_add_subregion(system, node + SN0_ARCS_TRAP_LOCAL,
                                    &s->arcs_trap);

        sn0_build_arcs(fw, fw_vaddr,
                       (hwaddr)(int32_t)(0xa0000000 + SN0_ARCS_TRAP_LOCAL),
                       vector);
        rom_add_blob_fixed("sn0.arcs.vector", vector, sizeof(vector),
                           node + SN0_ARCS_VECTOR);
    }

    /*
     * A kernel, loaded straight from an ELF image, which is the boot path every
     * test and every guest we care about uses. Booting from a disk is the
     * firmware's job and arrives with the disk.
     */
    if (machine->kernel_filename) {
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
        sn0_build_structures(machine, node, kernel_page, kernel_pages);
    }

    rom_add_blob_fixed("sn0.flash", flash, SN0_FLASH_SIZE,
                       node + SN0_FLASH_LOCAL);
    g_free(flash);

    /*
     * The firmware's own instructions, in a region of the machine's rather than
     * in the flash, so that whatever image the flash holds keeps every byte of
     * it. Read-only, like the flash, because nothing should be writing here.
     */
    memory_region_init_rom(&s->fw_text, NULL, "sn0.firmware.text",
                           SN0_FW_TEXT_SIZE, &error_fatal);
    memory_region_add_subregion(system, node + SN0_FW_TEXT_LOCAL, &s->fw_text);
    rom_add_blob_fixed("sn0.firmware.text", fw, SN0_FW_TEXT_SIZE,
                       node + SN0_FW_TEXT_LOCAL);
    g_free(fw);

    if (s->synthesized_firmware) {
        qemu_add_machine_init_done_notifier(&sn0_machine_ready_notifier);
    }
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

static void sn0_instance_init(Object *obj)
{
    SN0MachineState *s = SN0_MACHINE(obj);

    s->synthesized_firmware = true;
    s->widget0_revision = SN0_WIDGET0_REVISION;
}

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Origin 200 (IP27)";
    mc->init = sn0_init;
    mc->max_cpus = 1;
    mc->default_ram_size = SN0_MEM_BANK_SIZE;
    mc->default_ram_id = "sn0.node0.ram";
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R10000");

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
        "It is being built and so far answers only its own register block, "
        "which is what its tests read; the base I/O controller still reaches "
        "the processor directly rather than through it. Off by default, so "
        "that the machine this option changes is the one under test and not "
        "the one the contract describes.");

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
        "The silicon revision crosstalk widget 0 reports. Both guests treat a "
        "revision below four as having a byte-swapping defect and avoid that "
        "widget's PCI I/O space, so this changes behaviour and not only a "
        "printed line.");
}

static const TypeInfo origin200_type = {
    .name = MACHINE_TYPE_NAME("origin200"),
    .parent = TYPE_MACHINE,
    .class_init = origin200_class_init,
    .instance_init = sn0_instance_init,
    .instance_size = sizeof(SN0MachineState),
};

static void sn0_machine_types(void)
{
    type_register_static(&origin200_type);
}

type_init(sn0_machine_types)
