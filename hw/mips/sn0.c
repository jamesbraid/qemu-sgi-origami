/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) family machines.
 *
 * The bare machine: one node's memory and its boot flash, and nothing else
 * yet. The Hub, the crossbar and the I/O bridge grow into the address space
 * this file lays out.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "qemu/datadir.h"
#include "hw/mips/mips.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include "sn0.h"

/*
 * A stock Origin 200 runs its R10000 at 180 MHz and its hub at 90 MHz. The
 * processor's own counter runs at half the processor clock, which is what the
 * CPU model's counter divisor expresses, so this is the input that counter is
 * derived from.
 */
#define SN0_CPU_REFCLK_HZ (180 * 1000 * 1000)

/* What the firmware configuration block says about this machine. */
#define SN0_CPU_FREQ_HZ   180000000
#define SN0_HUB_FREQ_HZ   90000000
#define SN0_RTC_FREQ_HZ   1250000       /* an 800 ns tick */
#define SN0_MACHINE_TYPE  1             /* 0 is Origin 2000, 1 is Origin 200 */

struct SN0MachineState {
    MachineState parent_obj;

    MemoryRegion flash;
    MemoryRegion flash_hub_view;
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

static void sn0_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    MemoryRegion *system = get_system_memory();
    hwaddr node = SN0_NODE_BASE(0);
    Clock *cpuclk;
    MIPSCPU *cpu;
    uint8_t *flash;

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

    rom_add_blob_fixed("sn0.flash", flash, SN0_FLASH_SIZE,
                       node + SN0_FLASH_LOCAL);
    g_free(flash);
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
}

static const TypeInfo origin200_type = {
    .name = MACHINE_TYPE_NAME("origin200"),
    .parent = TYPE_MACHINE,
    .class_init = origin200_class_init,
    .instance_size = sizeof(SN0MachineState),
};

static void sn0_machine_types(void)
{
    type_register_static(&origin200_type);
}

type_init(sn0_machine_types)
