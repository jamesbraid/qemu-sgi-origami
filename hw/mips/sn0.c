/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) family machines.
 *
 * The family core: the abstract SN0 machine, which derives its chassis count
 * from the SMP topology and builds one node per chassis, and the node device
 * itself -- up to two R10000s with their per-processor views of the shared
 * address map, the node's memory banks, the boot flash with its Hub-special
 * views, the directory and ECC back doors, and the Hub. Products register
 * concrete machine types beside this and parameterize it through
 * SN0MachineClass (sn0-origin200.c); the synthesized firmware lives in
 * sn0-firmware.c and the host diagnostics in sn0-diag.c.
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
 * Not a hardware register: the IP27 PROM's own software convention of keeping
 * a word of boot-status flags in floating-point register 0 during early boot.
 * Neither Linux nor OpenBSD references it -- their kernels run only after the
 * convention has ended.  Bit 1, set while the cache-as-RAM phase is live, is
 * the only bit this model knows; the word's remaining bits are unmapped here
 * and belong to the firmware-structures datasheet.
 */
#define SN0_PROM_BOOT_STATUS_CACHE_AS_RAM    (1U << 1)

#define TYPE_SN0_NODE "sn0-node"

DECLARE_INSTANCE_CHECKER(SN0NodeState, SN0_NODE, TYPE_SN0_NODE)

/* The AMD 29F080's two unlock-cycle command addresses. */
#define SN0_FLASH_CMD_ADDR1   0x5555
#define SN0_FLASH_CMD_ADDR2   0x2aaa

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
     * C stack and local data. A cache-as-RAM flag in FPR 0's boot-status word
     * records that cache-only phase. Restrict the convention to code running
     * from either the boot PROM or the RAM copy to which it transfers before
     * installing the final mapped alias. Later software can then use FPR 0
     * and physical memory at 1 MiB normally.
     */
    return (env->active_fpu.fpr[0].d & SN0_PROM_BOOT_STATUS_CACHE_AS_RAM) &&
           ((pc >= SN0_FLASH_LOCAL &&
             pc < SN0_FLASH_LOCAL + SN0_FLASH_SIZE) ||
            (pc >= SN0_PROM_COPY_PADDR &&
             pc < SN0_PROM_COPY_PADDR + SN0_FLASH_SIZE));
}

/*
 * The cache-only phase keeps only the PROM's stack and locals in this window;
 * the PROM's own code runs from flash, never from here.  So a processor
 * *executing* from inside the window can only be the operating system, which
 * means the private phase is over and the per-access routing is no longer
 * needed -- from here on the window is just the shared DRAM underneath it.
 *
 * Recognise that by translating the program counter through the MMU -- the
 * kernel runs its text through a TLB-mapped segment (IRIX links this window's
 * driver code near 0xc0000000001026a8), not only the unmapped windows -- and
 * comparing the physical address against the window.  It is one-way: the PROM
 * cannot execute from here, so this can only ever turn the overlay off once the
 * window is genuinely running operating-system code, never during the phase.
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
 * Runs once, at a quiescent point with every vCPU drained, the first time the
 * window executes operating-system code.  Disabling the priority-2 overlay
 * exposes the shared-DRAM alias beneath it, so the window becomes plain RAM
 * that backs cached translations at full speed instead of retranslating one
 * instruction per block.  A machine reset re-enables it for the next boot's
 * cache-only phase (sn0_promdata_reset).
 */
static void sn0_promdata_leave_private(CPUState *cs, run_on_cpu_data data)
{
    SN0PromDataState *s = data.host_ptr;

    BQL_LOCK_GUARD();
    /*
     * A machine reset between scheduling this and running it clears the flag
     * and re-enables the overlay for a fresh cache-only phase; if that has
     * happened, leave the overlay in place rather than disabling it back out
     * from under the new boot.
     */
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

    /*
     * A reboot re-enters the PROM's cache-only phase, so restore the overlay
     * that a previous boot disabled once its operating system began executing
     * from the window.
     */
    s->leaving_private = false;
    memory_region_set_enabled(&s->filter, true);
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

/*
 * The ECC and directory back-door storage is a raw buffer behind the
 * populated-bank ops, not a RAM MemoryRegion, so the memory core does not
 * migrate it the way it does guest RAM; the node's descriptor carries it as a
 * sub-section. Both migration ends derive data_size from the same memory
 * configuration at realize and allocate the buffer before any state loads, so
 * only the contents have to cross. A node with no populated banks would leave
 * data NULL and data_size zero, but that machine is unreachable today:
 * sn0_ip29_memory_layout() rejects a zero-size node and construction exits, so
 * every node that boots populates at least one bank.
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

    /*
     * The stream carries how far each processor's cache-only phase had got.
     * Reapply that to the overlay the destination built enabled, so a boot
     * that had already switched the window to plain RAM does not revert to the
     * slow per-instruction path.
     */
    for (slice = 0; slice < SN0_CPUS_PER_NODE; slice++) {
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
        VMSTATE_STRUCT_ARRAY(promdata, SN0NodeState, SN0_CPUS_PER_NODE, 1,
                             vmstate_sn0_promdata_slice, SN0PromDataState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sn0_flash = {
    .name = "sn0-flash",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = sn0_flash_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(flash_state, SN0NodeState),
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
        memory_region_flush_rom_device(&node->flash, offset, 1);
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
            memory_region_flush_rom_device(&node->flash, offset,
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
 * the address folding and the width mask have to happen on every access. A
 * raw buffer is not migrated for free the way a RAM region is, so the node's
 * descriptor carries it as a sub-section (vmstate_sn0_backdoor): a guest write
 * here survives savevm/loadvm and a live migration.
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
    s->data_size = total;
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

void sn0_mode_bits_reset(void *opaque)
{
    CPUMIPSState *env = &MIPS_CPU(opaque)->env;
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(qdev_get_machine());

    env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
                       (smc->processor_mode_bits & SN0_PROCESSOR_MODE_MASK);
}

/*
 * The machine's own reset, after every device's. The order below is the
 * contract the old registration list happened to give: the firmware window
 * write and the processor mode-bits write must land after the Hub's and the
 * CPUs' own resets, and the PCI reassignment after the devices cleared
 * their registers.
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
                                         unsigned int node_index)
{
    if (node_index != 0 && SN0_MACHINE(machine)->synthesized_firmware &&
        machine->kernel_filename) {
        return SN0_NODE_BASE(0);
    }
    return SN0_NODE_BASE(node_index);
}

static void sn0_node_create_cpus(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    unsigned int node_index = node->nasid;
    unsigned int slice, half, space;

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

        memory_region_init(&node->cpu_sysmem[slice], OBJECT(node),
                           sysmem_name, UINT64_MAX);
        memory_region_init_alias(&node->sysmem_alias[slice], OBJECT(node),
                                 alias_name, system, 0, UINT64_MAX);
        memory_region_add_subregion_overlap(&node->cpu_sysmem[slice], 0,
                                            &node->sysmem_alias[slice], -1);

        /*
         * Addresses without a NASID normally name the reading processor's
         * node; the local-memory-base property carries the one exception,
         * which applies to memory space alone.  Keep the global system map
         * underneath so an explicit NASID still reaches either node in
         * both cases.
         */
        for (space = 0; space < SN0_UNCACHED_SPACES; space++) {
            g_autofree char *local_name = g_strdup_printf(
                "sn0.node%u.cpu%u.local-space%u", node_index, slice, space);
            hwaddr alias_node_base = space == 0 ?
                node->local_memory_base : node_base;
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
            uint64_t private_offset = node->ram_start +
                (half ^ slice) * SN0_HUB_PRIVATE_HALF;

            memory_region_init_alias(&node->hub_private[slice][half],
                                     OBJECT(node), private_name, node->ram,
                                     private_offset, SN0_HUB_PRIVATE_HALF);
            memory_region_add_subregion_overlap(
                &node->cpu_sysmem[slice],
                SN0_SPACE_HUB + half * SN0_HUB_PRIVATE_HALF,
                &node->hub_private[slice][half], 1);
        }

        node->cpu[slice] = MIPS_CPU(qdev_new(node->cpu_type));

        /*
         * The cache-only private phase the filter models exists only under
         * the real PROM: it is entered when the PROM sets FPR 0's cache-as-RAM boot-status bit,
         * which synthesized firmware never does before handing a directly
         * loaded kernel control.  In that mode the filter would be pure
         * forwarding -- except that an I/O region cannot back translated
         * code, so kernel text linked into this 32 KiB window would execute
         * one instruction per translation block.  For a directly loaded kernel
         * there is no cache-only phase, so the machine leaves promdata-filter
         * off and the window is the plain RAM it forwards to.  For a real
         * PROM boot the filter is genuinely needed during that phase, so
         * keep it -- but it disables itself once the
         * operating system first executes from the window
         * (sn0_promdata_leave_private), after which the window is the plain
         * shared RAM underneath and backs cached translations at full speed.
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
        cpu_mips_irq_init_cpu(node->cpu[slice]);
        cpu_mips_clock_init(node->cpu[slice]);
    }
}

static void sn0_node_map_memory(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    uint64_t ram_offset = node->ram_start;
    unsigned int node_index = node->nasid;
    unsigned int bank, segment;

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
                                     OBJECT(node), name, node->ram,
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
                    alias_name, node->ram, ram_offset, alias_size);
                memory_region_add_subregion(
                    system, SN0_SPACE_HUB + node_base + segment_addr,
                    &node->hub_local_mem_slot[bank][segment]);
            }
            ram_offset += segment_size;
        }
    }
}

static void sn0_node_map_boot_storage(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    unsigned int node_index = node->nasid;
    g_autofree char *flash_name =
        g_strdup_printf("sn0.node%u.flash", node_index);
    g_autofree char *hub_name =
        g_strdup_printf("sn0.node%u.flash.hub", node_index);
    g_autofree char *rboot_name =
        g_strdup_printf("sn0.node%u.flash.rboot", node_index);
    g_autofree char *hub_fast_name =
        g_strdup_printf("sn0.node%u.flash.hub.fast", node_index);
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

    /*
     * The PROM fetches its relocated code through the flash base in Hub space,
     * not the reset view. An I/O region cannot back translated code, so overlay
     * a direct romd alias of the chip there; the mirror underneath still serves
     * the repeated decode and the byte-wide command port.
     */
    memory_region_init_alias(&node->flash_hub_fast, OBJECT(node),
                             hub_fast_name, &node->flash, 0, SN0_FLASH_SIZE);
    memory_region_add_subregion_overlap(
        system, SN0_SPACE_HUB + node_base + SN0_FLASH_LOCAL,
        &node->flash_hub_fast, 1);

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

static void sn0_node_create_hub(SN0NodeState *node)
{
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
                             SN0_SPACE_MEMORY + node->local_memory_base,
                             SN0_NODE_SIZE);
    memory_region_add_subregion_overlap(&node->hub_memory, SN0_SPACE_MEMORY,
                                        &node->hub_memory_local, 0);
    object_property_set_link(OBJECT(&node->hub), "memory",
                             OBJECT(&node->hub_memory), &error_fatal);

    qdev_prop_set_uint32(DEVICE(&node->hub), "cpus", node->nr_cpus);
    qdev_prop_set_uint32(DEVICE(&node->hub), "cpu-base", node->cpu_base);
    qdev_prop_set_uint32(DEVICE(&node->hub), "xtalk-widget-shift",
                         SN0_XTALK_WIDGET_SHIFT);
    object_property_set_link(OBJECT(&node->hub), "xtalk",
                             OBJECT(&node->xtalk), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&node->hub), &error_fatal);

    cpu = node->cpu[0];
    sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 0,
                       cpu->env.irq[SN0_HUB_BANK0_CAUSE_BIT - CP0Ca_IP]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 1,
                       cpu->env.irq[SN0_HUB_BANK1_CAUSE_BIT - CP0Ca_IP]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 2,
                       cpu->env.irq[SN0_HUB_RT_CAUSE_BIT - CP0Ca_IP]);
    if (node->nr_cpus == SN0_CPUS_PER_NODE) {
        cpu = node->cpu[1];
        sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 4,
                           cpu->env.irq[SN0_HUB_BANK0_CAUSE_BIT - CP0Ca_IP]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 5,
                           cpu->env.irq[SN0_HUB_BANK1_CAUSE_BIT - CP0Ca_IP]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&node->hub), 3,
                           cpu->env.irq[SN0_HUB_RT_CAUSE_BIT - CP0Ca_IP]);
    }
}

static void sn0_node_map_xtalk_windows(SN0NodeState *node)
{
    MemoryRegion *system = get_system_memory();
    hwaddr node_base = SN0_NODE_BASE(node->nasid);
    unsigned int node_index = node->nasid;
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

static void sn0_init(MachineState *machine)
{
    SN0MachineState *s = SN0_MACHINE(machine);
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    unsigned int numa_nodes = machine->numa_state ?
                              machine->numa_state->num_nodes : 0;
    uint64_t ram_offset = 0;
    unsigned int node_count, node_index;

    /* A product class must say how memory fits and what I/O it carries. */
    g_assert(smc->node_memory_layout && smc->populate_io);

    /*
     * One SMP socket is one chassis. -numa still owns the memory split and
     * the processor assignment, and a -numa node count must agree with the
     * socket count, which the topology check below enforces; with no -numa
     * given, the sockets alone select the chassis count and memory splits
     * evenly.
     */
    if (numa_nodes > smc->max_nodes) {
        error_report("%s: at most %u NUMA nodes are supported",
                     mc->name, smc->max_nodes);
        exit(EXIT_FAILURE);
    }
    node_count = numa_nodes ? numa_nodes : machine->smp.sockets;
    if (node_count > smc->max_nodes) {
        error_report("%s: at most %u chassis are supported, and each -smp "
                     "socket is one chassis", mc->name, smc->max_nodes);
        exit(EXIT_FAILURE);
    }

    if (node_count == 1) {
        if (machine->smp.sockets != 1 ||
            machine->smp.cores > SN0_CPUS_PER_NODE ||
            machine->smp.threads != 1) {
            error_report("%s: one chassis requires one socket with at "
                         "most two cores and one thread per core", mc->name);
            exit(EXIT_FAILURE);
        }
    } else if (machine->smp.cpus != node_count * SN0_CPUS_PER_NODE ||
               machine->smp.sockets != node_count ||
               machine->smp.cores != SN0_CPUS_PER_NODE ||
               machine->smp.threads != 1) {
        error_report("%s: %u chassis require %u CPUs as %u sockets with "
                     "%u cores and one thread each", mc->name, node_count,
                     node_count * SN0_CPUS_PER_NODE, node_count,
                     SN0_CPUS_PER_NODE);
        exit(EXIT_FAILURE);
    }

    /*
     * Whole mebibytes per chassis, because the bank fit below is counted
     * in them: a byte-even but mebibyte-odd total would otherwise reach
     * the fit with a truncated size and fail with a misleading message.
     */
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
            unsigned int expected = node_index / SN0_CPUS_PER_NODE;

            if (!cpu_slot->props.has_node_id ||
                cpu_slot->props.node_id != expected) {
                error_report("%s: CPU %u must be assigned to NUMA "
                             "node %u", mc->name, node_index, expected);
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
    clock_set_hz(s->cpu_refclk, smc->cpu_hz);

    for (node_index = 0; node_index < node_count; node_index++) {
        SN0NodeState *node = s->nodes[node_index];
        uint64_t ram_size = numa_nodes ?
            machine->numa_state->nodes[node_index].node_mem :
            machine->ram_size / node_count;
        unsigned int cpu_base = node_index * SN0_CPUS_PER_NODE;
        unsigned int cpu_count = node_count == 1 ? machine->smp.cpus :
                                 SN0_CPUS_PER_NODE;
        uint16_t bank_mb[SN0_MEM_BANKS];
        unsigned int populated_banks;
        QList *banks_mb;
        unsigned int slice, bank;

        if (!smc->node_memory_layout(ram_size, bank_mb, &populated_banks)) {
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

        qdev_prop_set_uint32(DEVICE(node), "nasid", node_index);
        qdev_prop_set_uint32(DEVICE(node), "cpu-base", cpu_base);
        qdev_prop_set_uint32(DEVICE(node), "nr-cpus", cpu_count);
        qdev_prop_set_string(DEVICE(node), "cpu-type", machine->cpu_type);
        object_property_set_link(OBJECT(node), "ram", OBJECT(machine->ram),
                                 &error_abort);
        qdev_prop_set_uint64(DEVICE(node), "ram-start", ram_offset);
        qdev_prop_set_uint64(DEVICE(node), "local-memory-base",
                             sn0_node_local_memory_base(machine, node_index));
        qdev_prop_set_bit(DEVICE(node), "promdata-filter",
                          !(s->synthesized_firmware &&
                            machine->kernel_filename));
        qdev_prop_set_array(DEVICE(node), "banks-mb", banks_mb);
        qdev_connect_clock_in(DEVICE(node), "cpu-refclk", s->cpu_refclk);

        /*
         * The Hub facts the machine rather than the node owns: its
         * programmed node number (real firmware programs it later, so a
         * real-PROM boot starts at zero), which chassis is the slave, the
         * chassis DIP switches, the CrayLink peer, and the bring-up UART.
         * The UART is a node-0 diagnostic device, not part of either
         * shipped chassis; keeping it on the console node also avoids
         * assigning one character backend to two frontends.
         */
        qdev_prop_set_uint32(DEVICE(&node->hub), "nasid",
                             s->synthesized_firmware ? node_index : 0);
        qdev_prop_set_bit(DEVICE(&node->hub), "slave-module",
                          node_index != 0);
        qdev_prop_set_uint8(DEVICE(&node->hub), "elsc-dip-switches",
                            s->elsc_dip_switches);
        if (node_count == 2) {
            qdev_prop_set_bit(DEVICE(&node->hub), "network-link-up", true);
            object_property_set_link(OBJECT(&node->hub), "network-peer",
                                     OBJECT(&s->nodes[1 - node_index]->hub),
                                     &error_fatal);
        }
        qdev_prop_set_bit(DEVICE(&node->hub), "junk-uart",
                          node_index == 0 && s->junk_uart_present);
        if (node_index == 0 && s->junk_uart_present &&
            serial_hd(SN0_JUNK_UART_CHARDEV)) {
            object_property_set_link(OBJECT(&node->hub), "junk-uart-chardev",
                                     OBJECT(serial_hd(SN0_JUNK_UART_CHARDEV)),
                                     &error_fatal);
        }
        sn0_diagnostic_attach(s, node_index);

        sysbus_realize(SYS_BUS_DEVICE(node), &error_fatal);

        for (slice = 0; slice < cpu_count; slice++) {
            machine->possible_cpus->cpus[cpu_base + slice].cpu =
                CPU(node->cpu[slice]);
        }
        ram_offset += ram_size;
    }
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

    if (node->nr_cpus < 1 || node->nr_cpus > SN0_CPUS_PER_NODE) {
        error_setg(errp, "a node carries one or two processors, not %u",
                   node->nr_cpus);
        return;
    }
    if (node->nr_banks_mb < 1 || node->nr_banks_mb > SN0_MEM_BANKS) {
        error_setg(errp, "a node fills one to %u memory banks, not %u",
                   SN0_MEM_BANKS, node->nr_banks_mb);
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
 * A sysbus device, following the family precedent for a machine's
 * chip-level unit (PowerNV's chip is one) and the tree's own device rule.
 * Sysbus parentage does not move the node's migration sections: the main
 * system bus implements no get_dev_path, so the vmstate below keeps its
 * per-name instance ids either way — proven by loading a stream saved by
 * the bus-less build. The node's resettable state — the flash command
 * machine and the promdata overlay — keeps its explicit reset handlers and
 * their long-standing order for now; sysbus parentage is what allows a
 * later move onto ResettablePhases without re-parenting.
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

    s->synthesized_firmware = true;
    s->io_bridge_present = true;
    s->debug_leds = true;
    s->widget0_revision = SN0_WIDGET0_REVISION;
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
     * The widget0 and widget_alias storage stays: those arrays hold
     * MemoryRegions this machine owns as QOM children, and children are
     * released after instance_finalize runs, so freeing their storage here
     * would leave the release walking freed memory. Like every board's
     * regions they live as long as the process.
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

    /*
     * The documented baseline reports revision 4. Keep the property settable
     * so another captured configuration can be measured without changing the
     * Bridge device's general default or introducing a machine profile.
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
