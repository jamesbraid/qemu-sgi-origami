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

/* IP29 has four banks, each made from one matched DIMM pair. */
#define IP29_MEM_BANKS 4

#define SN0_BRIDGE_ONEWIRE_SERIAL_BASE 0x060504030201ULL
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

    env->CP0_Config0 = (env->CP0_Config0 & ~(uint32_t)SN0_PROCESSOR_MODE_MASK) |
                       (SN0_PROCESSOR_MODE_BITS & SN0_PROCESSOR_MODE_MASK);
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
    sn0_diagnostic_register_reset(s);
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
    clock_set_hz(node->cpuclk, SN0_MACHINE_GET_CLASS(machine)->cpu_hz);
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
         * the real PROM: it is entered when the PROM sets FPR 0's cache-as-RAM boot-status bit,
         * which synthesized firmware never does before handing a directly
         * loaded kernel control.  In that mode the filter would be pure
         * forwarding -- except that an I/O region cannot back translated
         * code, so kernel text linked into this 32 KiB window would execute
         * one instruction per translation block.  For a directly loaded kernel
         * there is no cache-only phase, so map the window as the plain RAM it
         * forwards to.  For a real PROM boot the filter is genuinely needed
         * during that phase, so keep it -- but it disables itself once the
         * operating system first executes from the window
         * (sn0_promdata_leave_private), after which the window is the plain
         * shared RAM underneath and backs cached translations at full speed.
         */
        if (!(SN0_MACHINE(machine)->synthesized_firmware &&
              machine->kernel_filename)) {
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

    if (!SN0_MACHINE_GET_CLASS(machine)->node_memory_layout(
            ram_size, node->bank_mb, &node->populated_banks)) {
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

    sn0_diagnostic_attach(machine, node_index);

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

#define SN0_PROM_PI_PROBE_WIDGET 2
#define SN0_PROM_PI_PROBE_OFFSET 0x400208
#define SN0_PROM_PI_PROBE_WINDOW_OFFSET 0x208

/* Preserve the CPU's byte order across the nested Bridge dispatch. */
#define SN0_PROM_PI_PROBE_MEMOP(size) (size_memop(size) | MO_BE)

static MemTxResult sn0_prom_pi_probe_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;

    if (addr == 0 && size == 8) {
        *data = 0;
        sgi_hub_latch_prom_pi_error(&node->hub);
        return MEMTX_ERROR;
    }
    return memory_region_dispatch_read(
        &node->prom_pi_probe_bridge->device_window_mem[1],
        SN0_PROM_PI_PROBE_WINDOW_OFFSET + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static MemTxResult sn0_prom_pi_probe_write(void *opaque, hwaddr addr,
                                           uint64_t data, unsigned size,
                                           MemTxAttrs attrs)
{
    SN0NodeState *node = opaque;
    return memory_region_dispatch_write(
        &node->prom_pi_probe_bridge->device_window_mem[1],
        SN0_PROM_PI_PROBE_WINDOW_OFFSET + addr, data,
        SN0_PROM_PI_PROBE_MEMOP(size), attrs);
}

static const MemoryRegionOps sn0_prom_pi_probe_ops = {
    .read_with_attrs = sn0_prom_pi_probe_read,
    .write_with_attrs = sn0_prom_pi_probe_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

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
     * Each base I/O board's controller carries its own laser-programmed
     * one-wire serial, the same as its bridge does. Consecutive only by
     * choice: node 0 keeps the value this machine reported before it grew a
     * second chassis, and node 1 gets the next one so the firmware -- and a
     * guest's one-wire bus -- cannot merge two physical nodes that fall back
     * to their base I/O identities. Without a distinct value per node the
     * two controllers answer alike, and a guest that registers both fails
     * the second as a duplicate.
     */
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         SGI_IOC3_ONEWIRE_SERIAL + node_index);

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
        MemoryRegion *bridge_face =
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io_bridge[node_index]), 0);
        unsigned int widget;

        memory_region_add_subregion_overlap(
            &s->nodes[node_index].xtalk, SN0_XTALK_BASE(SN0_CONSOLE_WIDGET),
            bridge_face, -1);

        /*
         * The bridge answers at every other non-zero widget too, because the
         * machine has no crossbar and the Hub's link reaches it whatever widget
         * number an address carries. Alias its full widget face onto each of the
         * net-new widgets — 2 through 7 and 9 through 15 — leaving widget 0 (its
         * own alias, mapped unconditionally in sn0_init), widget 1 (the Hub) and
         * widget 8 (mapped just above) alone. This is what lets the firmware's
         * device pointer stamped with widget 2 reach the bridge's device-1 PIO
         * window instead of faulting, matching the ratified no-crossbar
         * contract. Gated with the bridge because the device windows these
         * expose are only real when the devices behind the bridge are.
         */
        for (widget = SN0_WIDGET0 + 2; widget < SN0_XTALK_WIDGETS; widget++) {
            g_autofree char *name = NULL;

            if (widget == SN0_CONSOLE_WIDGET) {
                continue;
            }

            name = g_strdup_printf("sn0.node%u.xtalk.widget%u",
                                   node_index, widget);
            memory_region_init_alias(&s->widget_alias[node_index][widget],
                                     OBJECT(s), name, bridge_face, 0,
                                     SGI_BRIDGE_WIDGET_SIZE);
            memory_region_add_subregion(&s->nodes[node_index].xtalk,
                                        SN0_XTALK_BASE(widget),
                                        &s->widget_alias[node_index][widget]);
        }

        memory_region_init_io(&s->nodes[node_index].prom_pi_probe,
                              OBJECT(&s->nodes[node_index]),
                              &sn0_prom_pi_probe_ops,
                              &s->nodes[node_index],
                              "sn0.prom-pi-probe", 8);
        s->nodes[node_index].prom_pi_probe_bridge =
            &s->io_bridge[node_index];
        memory_region_add_subregion_overlap(
            &s->nodes[node_index].xtalk,
            SN0_XTALK_BASE(SN0_PROM_PI_PROBE_WIDGET) +
            SN0_PROM_PI_PROBE_OFFSET,
            &s->nodes[node_index].prom_pi_probe, 1);

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
    SN0MachineClass *smc = SN0_MACHINE_GET_CLASS(machine);
    MemoryRegion *system = get_system_memory();
    uint64_t ram_offset = 0;
    unsigned int node_count, node_index;

    node_count = machine->numa_state && machine->numa_state->num_nodes ?
                 machine->numa_state->num_nodes : 1;
    if (node_count > smc->max_nodes) {
        error_report("%s: at most %u NUMA nodes are supported",
                     MACHINE_GET_CLASS(machine)->name, smc->max_nodes);
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
     * The silicon revision of the I/O bridge installed in this configuration.
     * Real Origin 200 systems report more than one revision, so the command
     * line can select a different captured configuration.
     *
     * It reaches widget 0 as well as widget 8 without being written twice,
     * because those are one chip and this is the one place the machine says
     * which chip revision is installed.
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
     * The other non-zero widgets answer too, on the argument this once used to
     * refuse them: a crossbar-less machine reaches the bridge at every widget
     * number, and the firmware relies on it — the IRIX miniroot dereferences a
     * device pointer stamped with widget 2. Those aliases go in beside the
     * widget-8 mapping in sn0_create_base_io, gated on the bridge being fitted;
     * widget 0 here is unconditional for the reason above, and widget 1 stays
     * the Hub.
     */
    memory_region_init_alias(&s->widget0[0], OBJECT(s), "sn0.xtalk.widget0",
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
            &s->widget0[1], OBJECT(s), "sn0.node1.xtalk.widget0",
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

    sn0_firmware_init(machine, node_count);

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

    sn0_diagnostic_finalize(s);
}

static void sn0_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

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

static void origin200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    SN0MachineClass *smc = SN0_MACHINE_CLASS(oc);

    mc->desc = "SGI Origin 200 (IP27)";
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "sn0.node0.ram";

    /*
     * One chassis or two coupled over CrayLink, two processor slots each. A
     * stock Origin 200 runs its R10000 at 180 MHz and its Hub and secondary
     * cache at 90 MHz.
     */
    smc->max_nodes = SN0_ORIGIN200_MAX_NODES;
    mc->max_cpus = SN0_CPUS_PER_NODE * smc->max_nodes;
    smc->cpu_hz = 180000000;
    smc->hub_hz = 90000000;
    smc->rtc_hz = 1250000;          /* an 800 ns tick */
    smc->scache_clock_mhz = 90;
    smc->fw_machine_type = 1;       /* 0 is Origin 2000, 1 is Origin 200 */
    smc->fprom_cyc = 15;
    smc->fprom_wr = 4;
    smc->node_memory_layout = sn0_ip29_memory_layout;
}

static const TypeInfo sn0_machine_types[] = {
    {
        .name              = MACHINE_TYPE_NAME("origin200"),
        .parent            = TYPE_SN0_MACHINE,
        .class_init        = origin200_class_init,
    },
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
        .parent            = TYPE_DEVICE,
        .instance_size     = sizeof(SN0NodeState),
        .instance_init     = sn0_node_instance_init,
        .class_init        = sn0_node_class_init,
    },
};

DEFINE_TYPES(sn0_machine_types)
