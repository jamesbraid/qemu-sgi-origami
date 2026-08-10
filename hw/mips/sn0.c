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
#include "hw/mips/mips.h"
#include "system/address-spaces.h"
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

struct SN0MachineState {
    MachineState parent_obj;

    MemoryRegion flash;
    MemoryRegion flash_hub_view;
    MemoryRegion hub_regs_node_view;
    MemoryRegion arcs_trap;
    MemoryRegion fw_text;

    SGIHubState hub;

    /*
     * Whether this machine presents firmware of its own; see the class notes.
     */
    bool synthesized_firmware;

    /* Where the firmware's console output goes. */
    Chardev *console;
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

static void sn0_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    MemoryRegion *system = get_system_memory();
    hwaddr node = SN0_NODE_BASE(0);
    Clock *cpuclk;
    MIPSCPU *cpu;
    uint8_t *flash, *fw;
    hwaddr fw_vaddr;

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
    sysbus_realize(SYS_BUS_DEVICE(&s->hub), &error_fatal);

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
        uint64_t entry;
        ssize_t size = load_elf(machine->kernel_filename, NULL,
                                sn0_elf_to_phys, NULL, &entry, NULL, NULL,
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
}

static bool sn0_get_synthesized_firmware(Object *obj, Error **errp)
{
    return SN0_MACHINE(obj)->synthesized_firmware;
}

static void sn0_set_synthesized_firmware(Object *obj, bool value, Error **errp)
{
    SN0_MACHINE(obj)->synthesized_firmware = value;
}

static void sn0_instance_init(Object *obj)
{
    SN0_MACHINE(obj)->synthesized_firmware = true;
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
