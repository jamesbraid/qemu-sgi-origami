/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) synthesized firmware: what a real IP27
 * PROM leaves behind for a guest, plus the direct -kernel and sashARCS
 * loaders. The flash chip is modelled in sn0.c; this file owns its contents.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "hw/mips/bootloader.h"
#include "elf.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "cpu.h"
#include "sn0.h"

/* The kernel argument vector, below the firmware structures. */
#define SN0_ARGV_ADDR   0x800
#define SN0_ARGV_STRING (SN0_ARGV_ADDR + 0x40)

/* Computed from the image's extent; IRIX releases ship different sizes. */
#define SN0_SASH_SCRATCH_GAP  (1 * MiB)
#define SN0_SASH_STACK_SIZE   (64 * KiB)

/* Where the factory-fresh environment log sits in the flash image. */
#define SN0_PROMLOG_SECTOR    14
#define SN0_PROMLOG_MAGIC     0x504c4f47
#define SN0_PROMLOG_VERSION   1

/* The configuration block's fixed geometry; its values come from the class. */
#define SN0_CONFIG_SIZE   0x50
#define SN0_CONFIG_FPROM_CYC_OFFSET 0x2c
#define SN0_CONFIG_MACHINE_TYPE_OFFSET 0x30
#define SN0_CONFIG_CHECKSUM_OFFSET 0x34
#define SN0_CONFIG_FPROM_WR_OFFSET 0x3c

/* Zero is a live node identifier; a no-link port carries all ones. */
#define SN0_PORT_NODE_INVALID   0xffffULL
#define SN0_PORT_UNCONNECTED    (SN0_PORT_NODE_INVALID << 48)

typedef struct SN0LaunchData {
    uint64_t call_addr;
    uint64_t call_parm;
    uint64_t stack_addr;
    uint64_t gp_addr;
} SN0LaunchData;


/*
 * Kernel headers name the unmapped 64-bit window as both virtual and
 * physical: take the low forty bits; an uncached window keeps its attribute.
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
 * The reset-entry jump must be two instructions -- all the eight bytes
 * between flash entry points allow -- so it is region-relative.
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

    /* Enter in 64-bit kernel mode; only kernel scratch is disturbed. */
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
 * The real ARCS client_start convention, with CP0 Status left at reset:
 * a 32-bit link address needs unmapped kuseg and no TLB entry.
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

static void sn0_build_flash(SN0MachineClass *smc, uint8_t *flash)
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

    /* The field order and widths are the IP27 firmware's. */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, smc->cpu_module.mode_bits);
    stq_be_p(config + 0x08, SN0_CONFIG_MAGIC);
    stq_be_p(config + 0x10, smc->cpu_module.clock_hz);
    stq_be_p(config + 0x18, smc->hub_hz);
    stq_be_p(config + 0x20, smc->rtc_hz);
    stl_be_p(config + 0x28, 1);                    /* error correction on */
    stl_be_p(config + 0x2c, smc->fprom_cyc);       /* flash read timing */
    stl_be_p(config + 0x30, smc->fw_machine_type);
    stl_be_p(config + 0x34, 0);                    /* checksum adjustment */
    stl_be_p(config + 0x38, 0);                    /* flash program count */
    stl_be_p(config + 0x3c, smc->fprom_wr);        /* flash write timing */
    stl_be_p(config + 0x40, 6);                    /* firmware version */
    stl_be_p(config + 0x44, 11);                   /* firmware revision */
    stl_be_p(config + 0x48, 0);                    /* configuration flags */

    /* The factory-fresh environment log; an erased first entry ends it. */
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
 * The flasher's contract: the three product values in, every other field
 * and the old block sum preserved; no IP27 signature, no touch.
 */
static void sn0_configure_flash(SN0MachineClass *smc, uint8_t *flash)
{
    uint8_t *config = flash + SN0_CONFIG_OFFSET;
    uint8_t old_sum;
    uint8_t new_sum;

    if (ldq_be_p(config + 0x08) != SN0_CONFIG_MAGIC) {
        return;
    }

    old_sum = sn0_config_byte_sum(config);
    stl_be_p(config + SN0_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + SN0_CONFIG_FPROM_CYC_OFFSET, smc->fprom_cyc);
    stl_be_p(config + SN0_CONFIG_MACHINE_TYPE_OFFSET, smc->fw_machine_type);
    stl_be_p(config + SN0_CONFIG_FPROM_WR_OFFSET, smc->fprom_wr);
    new_sum = sn0_config_byte_sum(config);
    stl_be_p(config + SN0_CONFIG_CHECKSUM_OFFSET,
             (uint8_t)(old_sum - new_sum));
}

/* A stub's load from the trap region is the call, the value the result. */
enum {
    ARCS_ESUCCESS = 0,
    ARCS_EINVAL = 21,
};

/* ARCS Write(handle, buffer, length, count): all early output. */
static uint64_t sn0_arcs_write(SN0MachineState *s, CPUMIPSState *env)
{
    uint64_t buffer = env->active_tc.gpr[5];
    uint64_t length = env->active_tc.gpr[6];
    uint64_t count_ptr = env->active_tc.gpr[7];
    uint64_t written = 0;
    uint8_t chunk[256];

    /* The 32-bit count field caps the operation and bounds a hostile length. */
    length = MIN(length, UINT32_MAX);
    while (written < length) {
        unsigned int n = MIN(sizeof(chunk), length - written);

        /* A guest virtual address: the processor's own translation applies. */
        if (cpu_memory_rw_debug(env_cpu(env), buffer + written, chunk, n,
                                false) < 0) {
            break;
        }
        if (s->console) {
            qemu_chr_write_all(s->console, chunk, n);
        }
        written += n;
    }

    /* Report the count whether or not anyone was listening. */
    if (count_ptr) {
        uint32_t be = cpu_to_be32(written);

        cpu_memory_rw_debug(env_cpu(env), count_ptr, &be, sizeof(be), true);
    }

    return ARCS_ESUCCESS;
}

/*
 * First free memory must start below page 0x20, where a guest silently
 * decides it is the successor machine; the walk stops at the first
 * firmware-permanent entry.
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

/* ARCS GetMemoryDescriptor(previous): the guest walks a list. */
static uint64_t sn0_arcs_get_memory(SN0MachineState *s, CPUMIPSState *env)
{
    uint64_t previous = env->active_tc.gpr[4];
    unsigned int next = 0;

    if (previous) {
        unsigned int i;

        /* Which one did we hand out last? */
        for (i = 0; i < s->nodes[0]->mem_count; i++) {
            if (previous == s->nodes[0]->mem_addr +
                            i * sizeof(struct sn0_mem_descriptor)) {
                next = i + 1;
                break;
            }
        }
        if (i == s->nodes[0]->mem_count) {
            return 0;
        }
    }

    if (next >= s->nodes[0]->mem_count) {
        return 0;                       /* the end of the list */
    }
    return s->nodes[0]->mem_addr + next * sizeof(struct sn0_mem_descriptor);
}

/* Start at the trampoline so the target makes the transition itself. */
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

    for (i = 0; i < s->num_nodes; i++) {
        if (s->nodes[i]->cpu[0] && s->nodes[i]->nasid == nasid) {
            node = s->nodes[i];
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
     * async_safe_run_on_cpu(): the quiescent section keeps a peer's
     * BQL-free read of CPU state from overlapping cpu_reset().
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

    /* A debugger's read must not make a firmware call happen. */
    if (attrs.debug) {
        *val = 0;
        return MEMTX_OK;
    }

    /* Only these three read the caller's registers; the rest are constant. */
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
        /* Null; a guest falls back to the hardware directory's signature. */
        *val = 0;
        break;
    case SN0_ARCS_GET_SYSTEM_ID:
        /* Null; read only after identification has already failed. */
        *val = 0;
        break;
    case SN0_ARCS_GET_ENV:
        /* Null for every variable; the console rate falls back to 9600. */
        *val = 0;
        break;
    case SN0_ARCS_CAPABILITY_OP:
        /* A lower level sends a caller down an unsupported path. */
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
    /* Eight bytes, valid and impl: a split load would call a routine twice. */
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

/*
 * One stub per vector entry: form the slot address, load, return. The two
 * vectors share one stub area and one trap page, told apart by op number.
 */
static void sn0_build_trap_stub(uint8_t *stub, uint64_t slot)
{
    /* Two instructions reach only sign-extended addresses; assert the reach. */
    assert(slot == (uint64_t)(int64_t)(int32_t)slot);
    assert((slot & 0xffff) < 0x8000);

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
 * `index` is the flat array slot, `virtual_id` the ordinal within the
 * type, `physical_id` the per-type physical position -- the field vendor
 * firmware reads, not `widget`, to find the I/O bridge.
 */
static void sn0_component(uint8_t *graph, unsigned int index,
                          unsigned int virtual_id, uint8_t type,
                          uint8_t revision, uint8_t physical_id,
                          uint8_t widget, unsigned int nasid)
{
    uint8_t *c = graph + (SN0_GRAPH_COMPONENTS - SN0_CONFIG_GRAPH) +
                 index * SN0_GRAPH_COMP_STRIDE;

    c[0x00] = type;
    c[0x01] = 1;                                /* record version */
    /* Not optional: a component without this is skipped by every consumer. */
    c[0x02] = SN0_COMP_ENABLED;
    c[0x03] = revision;
    c[SN0_COMP_PHYSICAL_ID] = physical_id;
    c[0x20] = widget;
    stw_be_p(c + 0x22, nasid);
    stl_be_p(c + 0x1c, virtual_id);
    /* Padding upstream, but an OpenBSD PCI-routing bitfield: zeroed. */
    stw_be_p(c + 0x24, 0);
    /* The firmware back-pointer is read for every component; zero it. */
    stq_be_p(c + 0x28, 0);
}

static uint8_t *sn0_component_at(uint8_t *graph, unsigned int index)
{
    return graph + (SN0_GRAPH_COMPONENTS - SN0_CONFIG_GRAPH) +
           index * SN0_GRAPH_COMP_STRIDE;
}

/*
 * `components` is how many entries are populated, `declared_count` what
 * the count field says; real firmware makes them disagree on the midplane.
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
    /* The board's own node, and an unchecked table index: keep it right. */
    stw_be_p(b + 0x20, nasid);
    for (i = 0; i < components; i++) {
        stl_be_p(b + 0x24 + i * 4,
                 SN0_GRAPH_COMPONENTS + (first_component + i) *
                 SN0_GRAPH_COMP_STRIDE);
    }
}

/* The console descriptor inline in the header, then three board records. */
static void sn0_build_graph(MachineState *machine, unsigned int node_index)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
    SN0NodeState *node_state = s->nodes[node_index];
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
    /* Must agree with the descriptor embedded below. */
    stl_be_p(graph + 0x10, SN0_GRAPH_CONSOLE_OFF);
    stl_be_p(graph + 0x14, SN0_GRAPH_BOARDS);

    /*
     * All three base fields are rooted at the widget, not the device
     * window: the configuration base is a sibling of that window.
     */
    widget = 0x9200000000000000ULL | SN0_PCI_WIDGET_BASE;

    /* Full width: a partial check accepts values wrong where it looks away. */
    QEMU_BUILD_BUG_ON((0x9200000000000000ULL | SN0_PCI_WIDGET_BASE |
                       SN0_CONSOLE_PORT_OFFSET) != 0x9200000008620178ULL);

    /* The serial-port base; the generation is in its low twelve bits. */
    stq_be_p(console + 0x00, widget | SN0_CONSOLE_PORT_OFFSET);

    /* The type 0 window the vendor kernel follows; derived, not captured. */
    stq_be_p(console + 0x08, widget | SGI_BRIDGE_CFG0_DEVICE(SN0_IOC3_DEVICE));

    /*
     * The one field Linux reads: it adds the register offset itself and
     * validates nothing, so it must be processor-issuable.
     */
    stq_be_p(console + 0x10, widget | SN0_BRIDGE_DEVICE2);
    stw_be_p(console + 0x18, SN0_CONSOLE_BAUD);
    stl_be_p(console + 0x1c, 0);                /* console type */
    stw_be_p(console + 0x20, node_state->nasid); /* node identifier */
    console[0x22] = SN0_CONSOLE_WIDGET;
    console[0x23] = SN0_IOC3_DEVICE;            /* controller's PCI device */

    /* Board 0: the processor-node record, with CPUs, Hub, and memory banks. */
    sn0_board(graph, 0, SN0_BOARD_NODE, SN0_BOARD_VERSION_NODE,
              SN0_GRAPH_BOARDS + SN0_GRAPH_BOARD_SIZE, 0, cpu_count + 2,
              cpu_count + 2, node_state->nasid);
    /* Physical identifiers here: a slice, the Hub's node, a first bank. */
    for (i = 0; i < cpu_count; i++) {
        sn0_component(graph, i, node_index * SN0_CPUS_PER_NODE + i,
                      SN0_COMP_PROCESSOR, SN0_COMP_REV_UNKNOWN, i, 0,
                      node_state->nasid);
    }
    sn0_component(graph, hub_component, 0, SN0_COMP_HUB,
                  SN0_COMP_REV_HUB, node_state->nasid, 0,
                  node_state->nasid);
    sn0_component(graph, memory_component, 0, SN0_COMP_MEMORY,
                  SN0_COMP_REV_UNKNOWN, 0, 0, node_state->nasid);

    /* The only source for the guest's clock; zero breaks its delay loop. */
    for (i = 0; i < cpu_count; i++) {
        cpu = sn0_component_at(graph, i);
        stw_be_p(cpu + 0x38, 0x0926);           /* identification, low half */
        /* The on-die floating-point unit reports revision zero. */
        stw_be_p(cpu + 0x3a, 0x0900);
        stw_be_p(cpu + 0x3c, smc->cpu_module.clock_hz / 1000000); /* clock in MHz */
        stw_be_p(cpu + 0x3e, 1);                /* secondary cache, megabytes */
        stw_be_p(cpu + 0x40, smc->cpu_module.scache_clock_mhz);
    }

    hub = sn0_component_at(graph, hub_component);
    /* No crossbar link: the node bits must read invalid, not zero. */
    stq_be_p(hub + 0x3c, SN0_PORT_UNCONNECTED);
    stq_be_p(hub + 0x58, smc->hub_hz);          /* Hub clock in hertz */

    /*
     * The eight-bank layout the reported addressing mode calls for; the
     * firmware test asserts mode and layout together.
     */
    mem = sn0_component_at(graph, memory_component);
    stw_be_p(mem + 0x38, total_mb);             /* total in megabytes */
    stw_be_p(mem + 0x3a, 0);                    /* bank-to-address mapping */
    for (i = 0; i < SN0_MEM_BANKS; i++) {
        stw_be_p(mem + 0x3c + i * 2, node_state->bank_mb[i]);
    }

    /*
     * Board 1: the midplane. Real firmware populates one component and
     * declares a count of zero; its consumer reads the excluded slot.
     */
    sn0_board(graph, 1, SN0_BOARD_MIDPLANE, SN0_BOARD_VERSION,
              SN0_GRAPH_BOARDS + 2 * SN0_GRAPH_BOARD_SIZE,
              midplane_component, 1, 0, node_state->nasid);
    sn0_component(graph, midplane_component, 0, SN0_COMP_MODULE_SERIAL,
                  SN0_COMP_REV_UNKNOWN, 0, 0, node_state->nasid);

    /*
     * Board 2: bridge, two disk controllers, then the I/O controller, as
     * captured. The bridge's physical identifier is its widget number; a
     * zero sends the vendor firmware's error path to widget 0.
     */
    base_io_component = midplane_component + 1;
    sn0_board(graph, 2, SN0_BOARD_BASE_IO, SN0_BOARD_VERSION, 0,
              base_io_component, 4, 4, node_state->nasid);
    sn0_component(graph, base_io_component, 0, SN0_COMP_BRIDGE,
                  SN0_COMP_REV_BRIDGE, SN0_CONSOLE_WIDGET,
                  SN0_CONSOLE_WIDGET, node_state->nasid);
    sn0_component(graph, base_io_component + 1, 0, SN0_COMP_SCSI,
                  SN0_COMP_REV_SCSI, SN0_SCSI_DEVICE0, SN0_CONSOLE_WIDGET,
                  node_state->nasid);
    sn0_component(graph, base_io_component + 2, 1, SN0_COMP_SCSI,
                  SN0_COMP_REV_SCSI, SN0_SCSI_DEVICE1, SN0_CONSOLE_WIDGET,
                  node_state->nasid);
    sn0_component(graph, base_io_component + 3, 0, SN0_COMP_IO_CONTROLLER,
                  SN0_COMP_REV_IOC3, SN0_IOC3_DEVICE, SN0_CONSOLE_WIDGET,
                  node_state->nasid);

    {
        g_autofree char *name =
            g_strdup_printf("sn0.node%u.graph", node_index);

        rom_add_blob_fixed(name, graph, SN0_CONFIG_GRAPH_SIZE,
                       node + SN0_CONFIG_GRAPH);
    }
    g_free(graph);
}

/* Each block carries every field, not only the ones a guest reads today. */
static void sn0_build_structures(MachineState *machine,
                                 unsigned int node_index,
                                 uint64_t kernel_page, uint64_t kernel_pages)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0NodeState *node_state = s->nodes[node_index];
    hwaddr node = SN0_NODE_BASE(node_state->nasid);
    struct sn0_mem_descriptor mem[6];
    uint8_t param[0x60] = { };
    uint8_t dir[SN0_HW_DIR_ENTRIES * SN0_HW_DIR_ENTRY_SIZE] = { };
    uint8_t gda[SN0_GDA_SIZE] = { };
    unsigned int node_count = s->num_nodes;
    unsigned int n = 0;
    int i;

    /*
     * The signature sits in the second four bytes: a guest tests the
     * 32-bit form first, then reads every pointer at half its offset.
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

    /* The graph entry's signature is the whole machine identification. */
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
             * OpenBSD stores its handler address through this entry; a
             * zeroed one sends that write somewhere invisible.
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

    /* Advertised from the start, so written from the start. */
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
     * The only node enumeration a guest has; absent is all ones across
     * both bytes, or a stray 0xff reads as a positive node number.
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

    /* Below page 0x20, or the guest silently becomes another machine. */
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
 * Real firmware redirects widget 0 onto the last big window; guests never
 * write the entry themselves. The write matches the reset value, so a
 * green boot proves nothing about it. Run after the Hub's own reset.
 */
void sn0_firmware_reset(void *opaque)
{
    address_space_stq(&address_space_memory,
                      SN0_SPACE_XTALK + SN0_NODE_BASE(0) +
                      SN0_HUB_LOCAL_WINDOW + SN0_HUB_IO_BWIN_ENTRY(
                          SN0_FIRMWARE_WIDGET0_WINDOW),
                      SN0_FIRMWARE_WIDGET0_ENTRY,
                      MEMTXATTRS_UNSPECIFIED, NULL);
}

/*
 * What real ARCS PROM assigns at POST, so a guest reaches its console
 * before running code of its own. At reset, registered after the devices
 * whose own reset would undo it; MASTER follows real firmware.
 */
static void sn0_assign_node_pci_resources(SN0MachineState *s,
                                          unsigned int node_index)
{
    unsigned int slot;

    pci_default_write_config(s->pci_ioc3[node_index], PCI_BASE_ADDRESS_0,
                             SN0_PCI_IOC3_MEM, 4);
    pci_default_write_config(s->pci_ioc3[node_index], PCI_COMMAND,
                             PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
    sgi_bridge_assign_window(s->io_bridge[node_index], SN0_IOC3_DEVICE,
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
        sgi_bridge_assign_window(s->io_bridge[node_index],
                                 SN0_SCSI_DEVICE0 + slot,
                                 mem_base, true);
    }
}

void sn0_assign_pci_resources(void *opaque)
{
    SN0MachineState *s = opaque;
    unsigned int node_index;

    if (!s->io_bridge_present) {
        return;
    }

    for (node_index = 0; node_index < s->num_nodes; node_index++) {
        sn0_assign_node_pci_resources(s, node_index);
    }
}

/* A fixed gap above the image's own end; releases ship different sizes. */
static void sn0_hand_off_sash(MachineState *machine, uint8_t *flash,
                              uint8_t *fw, hwaddr fw_vaddr, hwaddr node,
                              uint64_t entry, uint64_t gp_value,
                              uint64_t high)
{
    hwaddr scratch, argv0_str, argv_array, envp_array, sp;
    size_t name_len = strlen(machine->kernel_filename) + 1;
    uint32_t argv0_ptr, zero32 = 0;

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
}

/* One argument, the kernel's own name; the count and vector are read. */
static void sn0_hand_off_elf_kernel(MachineState *machine, uint8_t *flash,
                                    uint8_t *fw, hwaddr fw_vaddr,
                                    hwaddr node, uint64_t entry)
{
    uint64_t argv0 = cpu_to_be64(SN0_COMPAT_CACHED + SN0_ARGV_STRING);

    rom_add_blob_fixed("sn0.argv.string", machine->kernel_filename,
                       strlen(machine->kernel_filename) + 1,
                       node + SN0_ARGV_STRING);
    rom_add_blob_fixed("sn0.argv", &argv0, sizeof(argv0),
                       node + SN0_ARGV_ADDR);

    sn0_build_kernel_handoff(flash, fw, fw_vaddr, entry, 1,
                             SN0_COMPAT_CACHED + SN0_ARGV_ADDR);
}

void sn0_firmware_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
    MemoryRegion *system = get_system_memory();
    unsigned int node_count = s->num_nodes;
    hwaddr node = SN0_NODE_BASE(0);
    uint8_t *flash, *fw;
    hwaddr fw_vaddr;
    uint64_t kernel_page = 0, kernel_pages = 0;
    uint64_t kernel_low = 0, kernel_high = 0;
    unsigned int node_index;

    /* A -bios image replaces this buffer; a short one leaves bytes behind. */
    flash = g_malloc0(SN0_FLASH_SIZE);
    sn0_build_flash(smc, flash);

    if (machine->firmware) {
        char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        ssize_t size = path ? load_image_size(path, flash, SN0_FLASH_SIZE) : -1;

        if (size < 0) {
            error_report("%s: cannot read the firmware image '%s'",
                         MACHINE_GET_CLASS(machine)->name,
                         machine->firmware);
            exit(EXIT_FAILURE);
        }
        g_free(path);
    }
    sn0_configure_flash(smc, flash);

    /* Trap region, stubs and vector go together; any subset is worse. */
    fw = g_malloc0(SN0_FW_TEXT_SIZE);
    fw_vaddr = (hwaddr)(int32_t)(0xa0000000 + SN0_FW_TEXT_LOCAL);

    if (s->synthesized_firmware) {
        uint64_t vector[SN0_ARCS_ENTRIES];
        uint64_t private_vector[SN0_ARCS_PRIVATE_ENTRIES];
        hwaddr trap_vaddr = (hwaddr)(int32_t)(0xa0000000 + SN0_ARCS_TRAP_LOCAL);

        s->console = serial_hd(SN0_IOC3_SERIAL_A_CHARDEV);

        sn0_build_arcs(fw, fw_vaddr, trap_vaddr, vector, SN0_ARCS_ENTRIES,
                       0, 0);

        /* The SGI-private vector; unimplemented entries hit the default. */
        sn0_build_arcs(fw, fw_vaddr, trap_vaddr, private_vector,
                       SN0_ARCS_PRIVATE_ENTRIES, SN0_ARCS_ENTRIES,
                       SN0_ARCS_PRIVATE_BASE_OP);
        for (node_index = 0; node_index < node_count; node_index++) {
            SN0NodeState *node_state = s->nodes[node_index];
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
             * Slaves can run the kernel's 64-bit FPU loads before its
             * vectors are live; match PROM's execution mode.
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

    /* The direct load path every test and guest we boot uses. */
    if (machine->kernel_filename) {
        uint64_t entry, low, high, gp_value = 0;
        ssize_t elf_size = load_elf(machine->kernel_filename, NULL,
                                    sn0_elf_to_phys, NULL, &entry, &low,
                                    &high, NULL, ELFDATA2MSB, EM_MIPS, 1, 0);
        ssize_t ecoff_size = ECOFF_LOAD_FAILED;

        if (elf_size < 0) {
            /* Not an ELF: sashARCS ships as an ECOFF OMAGIC executable. */
            ecoff_size = load_ecoff(machine->kernel_filename, &entry,
                                    &gp_value, &low, &high, node);
        }
        if (elf_size < 0 && ecoff_size < 0) {
            error_report("%s: cannot load the kernel '%s': %s; as ECOFF: %s",
                         MACHINE_GET_CLASS(machine)->name,
                         machine->kernel_filename,
                         load_elf_strerror(elf_size),
                         load_ecoff_strerror(ecoff_size));
            exit(1);
        }
        kernel_low = low;
        kernel_high = high;

        if (ecoff_size >= 0) {
            sn0_hand_off_sash(machine, flash, fw, fw_vaddr, node,
                              entry, gp_value, high);
        } else {
            bool elf_is_64 = false;

            /*
             * IRIX links as 32-bit ELF; sign-extend its kseg entry, keyed
             * on the file's class rather than the entry's shape.
             */
            if (load_elf_hdr(machine->kernel_filename, NULL, &elf_is_64,
                             NULL) && !elf_is_64 && (entry & 0x80000000)) {
                entry = (uint64_t)(int64_t)(int32_t)entry;
            }
            sn0_hand_off_elf_kernel(machine, flash, fw, fw_vaddr, node,
                                    entry);
        }
    }

    if (kernel_high) {
        kernel_page = kernel_low / 0x1000;
        kernel_pages = DIV_ROUND_UP(kernel_high - kernel_low, 0x1000);
    }

    /* Last: the memory descriptors must say where the kernel landed. */
    if (s->synthesized_firmware) {
        for (node_index = 0; node_index < node_count; node_index++) {
            sn0_build_structures(machine, node_index,
                                 node_index == 0 ? kernel_page : 0,
                                 node_index == 0 ? kernel_pages : 0);
        }
    }

    /* Not rom_add_blob_fixed(): reset copy-back would erase retained state. */
    for (node_index = 0; node_index < node_count; node_index++) {
        MemoryRegion *chip = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(s->nodes[node_index]->flash), 0);

        memcpy(memory_region_get_ram_ptr(chip), flash, SN0_FLASH_SIZE);
        memory_region_set_dirty(chip, 0, SN0_FLASH_SIZE);
    }
    g_free(flash);

    /* Outside the flash, so any -bios image keeps every byte of it. */
    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node_state = s->nodes[node_index];
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
}
