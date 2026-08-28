/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) synthesized firmware.
 *
 * What a real IP27 PROM leaves behind for a guest, built by the machine when
 * it is asked to present firmware of its own: the programmed boot flash and
 * its configuration block, the ARCS call vectors and the routines behind
 * them, the KLCONFIG configuration graph and the other low-memory structures,
 * and the reset-time device state real firmware sets up -- PCI resources and
 * the widget-0 window redirection. The direct -kernel and sashARCS loaders
 * live here too, because loading a guest and handing it control is the
 * firmware's job. The flash chip itself is modelled in sn0.c; this file owns
 * only its contents.
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

/* The configuration block's fixed geometry; its values come from the class. */
#define SN0_CONFIG_SIZE   0x50
#define SN0_CONFIG_FPROM_CYC_OFFSET 0x2c
#define SN0_CONFIG_MACHINE_TYPE_OFFSET 0x30
#define SN0_CONFIG_CHECKSUM_OFFSET 0x34
#define SN0_CONFIG_FPROM_WR_OFFSET 0x3c

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

typedef struct SN0LaunchData {
    uint64_t call_addr;
    uint64_t call_parm;
    uint64_t stack_addr;
    uint64_t gp_addr;
} SN0LaunchData;


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

    /*
     * The field order and widths are the IP27 firmware's. The release image is
     * built with Origin 2000 defaults because a real flasher preserves the
     * running node's configuration; this synthesized image instead carries
     * this product's Origin 200 values directly.
     */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, SN0_PROCESSOR_MODE_BITS);
    stq_be_p(config + 0x08, SN0_CONFIG_MAGIC);
    stq_be_p(config + 0x10, smc->cpu_hz);
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
 * has no preceding physical PROM to preserve, but the concrete machine class
 * supplies the same three values: the machine-type word and the two flash
 * timings.
 *
 * Preserve every other field supplied by the image, including clocks, CPU
 * mode, version, and flash count. Preserve the old block sum too, following
 * the flasher's observable contract. A diagnostic ROM without an IP27 config
 * signature remains byte-for-byte untouched.
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
    /*
     * Two instructions are enough only because the trap page sits in the
     * compatibility segment: lui sign-extends on a 64-bit processor, so this
     * pair addresses 0xffffffff_xxxx_xxxx and nothing else, and the load's
     * displacement is signed. Both hold for the page this machine places at
     * kseg1, and a page moved into one of the machine's 64-bit windows would
     * silently load from the wrong address instead -- so the reach is
     * asserted here rather than left for a guest to discover.
     */
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
                          uint8_t revision, uint8_t physical_id,
                          uint8_t widget)
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
 * then the three board records a single-node Origin 200 emits.
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
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
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
    console[0x23] = SN0_IOC3_DEVICE;            /* controller's PCI device */

    /* Board 0: the processor-node record, with CPUs, Hub, and memory banks. */
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
                      SN0_COMP_PROCESSOR, SN0_COMP_REV_UNKNOWN, i, 0);
    }
    sn0_component(graph, hub_component, 0, SN0_COMP_HUB,
                  SN0_COMP_REV_HUB, node_state->nasid, 0);
    sn0_component(graph, memory_component, 0, SN0_COMP_MEMORY,
                  SN0_COMP_REV_UNKNOWN, 0, 0);

    /*
     * The processor's own fields, which are the only source a guest has for its
     * clock and cache: with none readable its clock stays zero, which breaks
     * its delay loop, and it dies just after its last printed line. The
     * identification field is sixteen bits, so it carries the low half of the
     * processor identification register and not the whole of it.
     */
    for (i = 0; i < cpu_count; i++) {
        cpu = sn0_component_at(graph, i);
        stw_be_p(cpu + 0x38, 0x0926);           /* identification, low half */
        /*
         * The floating-point unit is on the same die but reports revision zero.
         */
        stw_be_p(cpu + 0x3a, 0x0900);
        stw_be_p(cpu + 0x3c, smc->cpu_hz / 1000000); /* clock in MHz */
        stw_be_p(cpu + 0x3e, 1);                /* secondary cache, megabytes */
        stw_be_p(cpu + 0x40, smc->scache_clock_mhz);
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
    stq_be_p(hub + 0x58, smc->hub_hz);          /* Hub clock in hertz */

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
     * Board 1: the midplane record, carrying a module serial number and
     * nothing else, which is what a real Origin 200 emits.
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
    sn0_component(graph, midplane_component, 0, SN0_COMP_MODULE_SERIAL,
                  SN0_COMP_REV_UNKNOWN, 0, 0);

    /*
     * Board 2: the base-I/O record, and the end of the list. Nothing on the
     * path to the console switch reads it; it is here because the graph has to
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
                  SN0_COMP_REV_BRIDGE, SN0_CONSOLE_WIDGET,
                  SN0_CONSOLE_WIDGET);
    sn0_component(graph, base_io_component + 1, 0, SN0_COMP_SCSI,
                  SN0_COMP_REV_SCSI, SN0_SCSI_DEVICE0, SN0_CONSOLE_WIDGET);
    sn0_component(graph, base_io_component + 2, 1, SN0_COMP_SCSI,
                  SN0_COMP_REV_SCSI, SN0_SCSI_DEVICE1, SN0_CONSOLE_WIDGET);
    sn0_component(graph, base_io_component + 3, 0, SN0_COMP_IO_CONTROLLER,
                  SN0_COMP_REV_IOC3, SN0_IOC3_DEVICE, SN0_CONSOLE_WIDGET);

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

void sn0_assign_pci_resources(void *opaque)
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

/*
 * Everything the machine's firmware side contributes at construction: the
 * flash contents, the call vectors and their trap region, a directly loaded
 * kernel or sashARCS, and the low-memory structures that describe both.
 * Runs after every node's memory and boot storage exist.
 */
void sn0_firmware_init(MachineState *machine, unsigned int node_count)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
    MemoryRegion *system = get_system_memory();
    hwaddr node = SN0_NODE_BASE(0);
    uint8_t *flash, *fw;
    hwaddr fw_vaddr;
    uint64_t kernel_page = 0, kernel_pages = 0;
    unsigned int node_index;

    /*
     * A firmware image, if one was supplied, replaces what this file would
     * otherwise synthesize. Both go into the same buffer and reach the flash as
     * one blob, so there is never a question of which of two writers lands
     * last. An image shorter than the flash leaves the synthesized bytes behind
     * it, which is what a partly programmed flash looks like.
     */
    flash = g_malloc0(SN0_FLASH_SIZE);
    sn0_build_flash(smc, flash);

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
    sn0_configure_flash(smc, flash);

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
         * A 32-bit ELF's entry point is the low half of a compatibility
         * address: bit 31 set means the missing upper word is all ones, not
         * all zeros, and load_elf() zero-extends it. Sign-extend so a kernel
         * linked in the 0x8xxxxxxx range is handed off at its canonical
         * address rather than an unmapped low one. Only for a 32-bit ELF: a
         * 64-bit one carries the whole address already, and truncating it
         * would destroy any entry outside the compatibility segment.
         */
        bool elf_is_64 = false;
        union {
            Elf32_Ehdr h32;
            Elf64_Ehdr h64;
        } ehdr;

        if (load_elf_hdr(machine->kernel_filename, &ehdr, &elf_is_64, NULL) &&
            !elf_is_64) {
            entry = (int64_t)(int32_t)entry;
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
}
