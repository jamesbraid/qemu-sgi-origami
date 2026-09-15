/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) direct kernel boot: the -kernel and
 * sashARCS loaders that patch a node's boot flash to jump straight into a
 * directly loaded image instead of running the PROM in it. The flash chip
 * itself is modelled in sn0.c; this file only owns what -kernel writes on
 * top of whatever -bios or pflash already put there.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "hw/mips/bootloader.h"
#include "elf.h"
#include "system/system.h"
#include "sn0.h"
#include "sn0-ecoff.h"
#include "trace.h"

static hwaddr sn0_firmware_node_base(const SN0ModelNodeTopology *topology,
                                     unsigned int nasid)
{
    const SN0ModelAddressMap *map = &topology->map;

    return sn0_model_node_address(map, &map->memory, nasid, 0);
}

/* The kernel argument vector, below the firmware structures. */
#define SN0_ARGV_ADDR   0x800
#define SN0_ARGV_STRING (SN0_ARGV_ADDR + 0x40)

/* Computed from the image's extent; IRIX releases ship different sizes. */
#define SN0_SASH_SCRATCH_GAP  (1 * MiB)
#define SN0_SASH_STACK_SIZE   (64 * KiB)

/*
 * Kernel headers name the unmapped 64-bit window as both virtual and
 * physical: take the low forty bits; an uncached window keeps its attribute.
 */
static uint64_t sn0_elf_to_phys(void *opaque, uint64_t addr)
{
    const SN0ModelAddressMap *map = opaque;
    const SN0ModelAddressSpace *memory = &map->memory;

    if ((addr >> 62) == 2) {                    /* the unmapped 64-bit window */
        unsigned int cca = (addr >> 59) & 7;
        uint64_t pa = addr & ((1ULL << 40) - 1);

        if (cca == 2 || cca == 7) {     /* uncached, so it has an attribute */
            unsigned int selector = (addr >> 57) & 3;
            const SN0ModelAddressSpace *space =
                selector == map->memory.selector ? &map->memory :
                selector == map->hub_special.selector ? &map->hub_special :
                selector == map->xtalk.selector ? &map->xtalk :
                &map->memory_special;

            return space->carrier_base + pa;
        }
        return memory->carrier_base + pa;
    }

    /* Both compatibility segments reach the bottom of node-local memory. */
    if (addr >= SN0_COMPAT_CACHED && addr < 0xffffffffc0000000ull) {
        return memory->carrier_base + (addr & 0x1fffffff);
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
    const SN0ModelNodeTopology *boot_topology = s->nodes[0]->topology;
    const SN0ModelRegion *flash_region = &boot_topology->map.flash_reset;
    MemoryRegion *system = get_system_memory();
    unsigned int node_count = s->num_nodes;
    hwaddr node = sn0_firmware_node_base(boot_topology, 0);
    uint8_t *flash, *fw;
    hwaddr fw_vaddr;
    unsigned int node_index;

    /*
     * A node with its own pflash drive (set on SN0NodeState before this
     * runs) keeps that drive's content untouched below; this buffer is
     * only what -bios supplies to every node that has none.
     */
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

    /* Trap region, stubs and vector go together; any subset is worse. */
    fw = g_malloc0(SN0_FW_TEXT_SIZE);
    fw_vaddr = (hwaddr)(int32_t)(0xa0000000 + SN0_FW_TEXT_LOCAL);

    /* The direct load path every test and guest we boot uses. */
    if (machine->kernel_filename) {
        uint64_t entry, low, high, gp_value = 0;
        ssize_t elf_size = load_elf(machine->kernel_filename, NULL,
                                    sn0_elf_to_phys,
                                    (void *)&boot_topology->map,
                                    &entry, &low,
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

    /*
     * Not rom_add_blob_fixed(): reset copy-back would erase retained
     * state. A node with its own pflash drive keeps that drive's content.
     */
    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node_state = s->nodes[node_index];
        MemoryRegion *chip;

        if (node_state->flash_drive) {
            continue;
        }
        chip = sysbus_mmio_get_region(SYS_BUS_DEVICE(node_state->flash), 0);
        memcpy(memory_region_get_ram_ptr(chip), flash, flash_region->size);
        memory_region_set_dirty(chip, 0, flash_region->size);
    }
    g_free(flash);

    /* Outside the flash, so any -bios image keeps every byte of it. */
    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node_state = s->nodes[node_index];
        hwaddr node_base = sn0_firmware_node_base(node_state->topology,
                                                  node_state->nasid);
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
