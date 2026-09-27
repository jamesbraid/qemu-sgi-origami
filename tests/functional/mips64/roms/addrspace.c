/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boundaries the R10000 draws around its 64-bit address spaces, and
 * the
 * one instruction-set gate that depends on a Status register mode bit
 * rather
 * than an address: which parts of the user and kernel regions exist
 * under
 * each combination of the extended-addressing bits, and whether
 * supervisor
 * mode may run MIPS III instructions at all.
 *
 * Three rules are checked, each from both sides of its boundary.
 *
 * First, the user region in 64-bit kernel mode. The manual's 64-bit
 * kernel
 * mode user space section says selection "is dependent upon the UX and
 * ERL
 * bits": with UX = 1 and ERL = 0 the whole 16-Tbyte space exists, but
 * "If
 * UX = 0 or ERL = 1, the address space 0x0000 0000 0000 0000 through
 * 0000 0000 7FFF FFFF (2 Gbytes) is selected. Addressing the space
 * ranging
 * from 0000 0000 8000 0000 through 0000 0FFF FFFF FFFF will cause an
 * address
 * error. Moreover, if ERL=1, the selected 2-Gbyte address space becomes
 * unmapped and uncached." An error handler running with error level
 * still
 * set must find the low user segment aliasing physical memory and
 * everything
 * above it refused, whatever UX says.
 *
 * Second, the top of mapped kernel space. The manual bounds xkseg at
 * 0xC000 0FFE FFFF FFFF in three independent places -- its 64-bit
 * kernel mode
 * address space figure, its virtual address space table ("C0000000
 * 00000000
 * TO C0000FFE FFFFFFFF" is xkseg, "C0000FFF 00000000 TO FFFFFFFF
 * 7FFFFFFF"
 * is an address error), and its TLB refill vector table -- so the top
 * 0x1 0000 0000 bytes of the 44-bit region are not a mapped hole but a
 * refusal. That is one 2^32-byte band more than the R4000's rule of
 * stopping
 * 2^31 short, which is what a model derived from the R4000 gets wrong.
 *
 * Third, the supervisor instruction set. The manual's addressing modes
 * section says "In Supervisor mode, the SX bit allows 64-bit addressing
 * and
 * the MIPS III instructions", its operating-mode table shows MIPS III
 * disabled in the supervisor rows with SX clear while every kernel row
 * has
 * it enabled regardless of KX, and its reserved instruction exception
 * cause
 * list includes "an attempt is made to execute 64-bit operations in
 * 32-bit
 * mode when in User or Supervisor modes". A doubleword instruction in
 * supervisor mode with SX clear must be refused as a reserved
 * instruction,
 * and the same instruction with SX set must run.
 *
 * Oracle: the MIPS R10000 User's Manual version 2.0, memory management
 * chapter (operating modes, addressing modes, 64-bit kernel mode
 * spaces,
 * virtual address space table), CPU exceptions chapter (TLB refill
 * vector
 * table, reserved instruction exception cause list).
 *
 * Two halves of the rules above are deliberately not asserted here. The
 * "uncached" half of the ERL rule is invisible to a ROM: this model has
 * no
 * caches, and the attribute an uncached reference presents to the
 * machine is
 * pinned by the fork's own machine-level tests instead. And the last
 * mapped
 * byte below the 2-Gbyte line under ERL is probed at a backed physical
 * page
 * rather than at 0x7FFF FFF8, because an unmapped reference lands on
 * physical memory and this machine answers unclaimed physical addresses
 * with
 * a bus error, which would measure the machine's population rather than
 * the
 * segment rule.
 *
 * This ROM is for the R10000 alone. The fork scopes all three rules to
 * that
 * processor; QEMU's long-established R4000 model keeps its upstream
 * behaviour, and the real R4000's boundaries differ anyway -- its
 * address
 * space is narrower and its xkseg stops 2^31 short of the region top
 * rather
 * than 2^32.
 */

#include "rom.h"
#include "probe-symbols.h"

const char rom_name[] = "addrspace";

/*
 * The bootstrap entry points, from the vector identifier scheme.
 */
#define VEC_BOOTSTRAP_XTLB    (ROM_VEC_BOOTSTRAP | ROM_VEC_XTLB_REFILL)
#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/*
 * Backed by memory on every machine this runs on; xkphys.c uses the
 * same.
 */
#define SCRATCH_PA 0x00400000ull

/*
 * The scratch page seen three ways: uncached compatibility segment from
 * kernel mode, and the user segment alias that exists only under ERL.
 */
#define KSEG1_OF(pa) (0xffffffffa0000000ull + (pa))

/*
 * A value nothing else in this ROM writes anywhere.
 */
#define SENTINEL 0xa5d0fadeb00cfaceull

/*
 * The probe addresses. All are doubleword-aligned so that an address
 * error
 * can only mean the segment rule refused the address, never mere
 * misalignment.
 */
#define XKUSEG_FIRST_HIGH 0x0000000080000000ull  /* first byte above 2G */
#define XKUSEG_LAST       0x00000ffffffffff8ull  /* last dword of 16T */
#define XKUSEG_PAST       0x0000100000000000ull  /* first byte past 16T */

#define XKSEG_BASE        0xc000000000000000ull  /* first xkseg dword */
#define XKSEG_LAST        0xc0000ffefffffff8ull  /* last mapped dword */
#define XKSEG_HOLE        0xc0000fff00000000ull  /* first refused dword */
/*
 * Inside the R4000-shaped remainder of the hole.
 */
#define XKSEG_HOLE_HIGH   0xc0000fff7ffffff8ull

/*
 * One load, with the resume address armed; reports via the record. The
 * Status register to reinstall on the way back is staged after the
 * record is
 * reset, because resetting the record clears it; the probes that run
 * with
 * error level set need one for the handler's own return to work.
 */
static void probe_staged(uint64_t addr, uint64_t status_set)
{
    uint64_t dead;

    rom_exc_reset();
    rom_exc.status_set = status_set;
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "lui $26, %%hi(9f)\n\t"
                         "daddiu $26, $26, %%lo(9f)\n\t"
                         "sd $26, 0(%[res])\n\t"
                         "ld %[d], 0(%[a])\n"
                         "9:\t.set pop"
                         : [d] "=&r"(dead)
                         : [res] "r"(&rom_exc.resume), [a] "r"(addr)
                         : "$26", "memory");
    (void)dead;
}

static void probe(uint64_t addr)
{
    probe_staged(addr, 0);
}

/*
 * The same load keeping its value, for the access that must succeed.
 */
static uint64_t load64(uint64_t addr)
{
    uint64_t v;

    __asm__ __volatile__("ld %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

/*
 * The supervisor routine, copied into the page this ROM maps. It runs
 * the
 * doubleword instruction under test, then leaves a marker and traps
 * back
 * into the kernel; reaching either of those means the instruction was
 * allowed. Only the two kernel scratch registers are used, exactly as
 * in
 * mips4user.c, so the kernel can hand it a pointer and a value with no
 * calling convention.
 */
__asm__(".pushsection .text.rom_sup_probe, \"ax\"\n\t"
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl rom_sup_probe\n"
        "rom_sup_probe:\n\t"
        /* MIPS III instruction under test. */
        "daddu $27, $27, $0\n\t"
        "sw $27, 8($26)\n\t"            /* got here: leave the marker behind */
        "syscall\n\t"                   /* hand control back to the kernel */
        "nop\n\t"
        ".globl rom_sup_probe_end\n"
        "rom_sup_probe_end:\n\t"
        ".set pop\n\t"
        ".popsection");

/*
 * The supervisor page pair, in the part of the user segment a 32-bit
 * supervisor can reach. The physical pages are spare: the harness
 * itself
 * uses only the first two megabytes.
 */
#define SUP_CODE_VA 0x0000000040400000ull
#define SUP_DATA_VA 0x0000000040401000ull
#define SUP_CODE_PA 0x0000000000300000ull
#define SUP_DATA_PA 0x0000000000301000ull

#define MARKER_OFFSET 8
#define MARKER_VALUE  0x5a5aa5a5u

/*
 * The entry the supervisor mapping lives in, held down.
 */
#define SUP_TLB_INDEX 0

static uint32_t kstatus;

static void map_sup_page(void)
{
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(SUP_CODE_VA);
    rom_set_entrylo0(rom_entrylo(SUP_CODE_PA,
                                 ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_entrylo1(rom_entrylo(SUP_DATA_PA,
                                 ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G,
                                 ROM_CACHE_NONCOHERENT));
    rom_set_index(SUP_TLB_INDEX);
    __asm__ __volatile__("tlbwi" : : : "memory");
    rom_set_wired(SUP_TLB_INDEX + 1);
    rom_set_entryhi(0);
}

static void copy_sup_probe(void)
{
    /* Volatile preserves writes through the uncached KSEG1 alias. */
    volatile unsigned char *dst =
        (volatile unsigned char *)KSEG1_OF(SUP_CODE_PA); /* KSEG1 uncached */
    unsigned long len = (unsigned long)(rom_sup_probe_end - rom_sup_probe);
    unsigned long i;

    for (i = 0; i < len; i++) {
        dst[i] = rom_sup_probe[i];
    }
}

static void set_marker(uint32_t v)
{
    /* Volatile preserves writes through the uncached KSEG1 alias. */
    volatile uint32_t *m =
        /*
         * Volatile preserves accesses through the uncached KSEG1 alias.
         */
        (volatile uint32_t *)KSEG1_OF(SUP_DATA_PA + MARKER_OFFSET);

    *m = v;
}

static uint32_t get_marker(void)
{
    /* Volatile preserves reads through the uncached KSEG1 alias. */
    volatile uint32_t *m =
        /*
         * Volatile preserves accesses through the uncached KSEG1 alias.
         */
        (volatile uint32_t *)KSEG1_OF(SUP_DATA_PA + MARKER_OFFSET);

    return *m;
}

/*
 * Drop into the supervisor routine and come back, exactly as
 * mips4user.c
 * drops into user mode: the return address is armed by the block
 * itself, and
 * the Status register to reinstall is staged separately because
 * resuming at
 * a kernel address from supervisor mode would take an address error
 * instead.
 */
static void run_sup_probe(uint32_t sstatus)
{
    set_marker(0);
    rom_exc_reset();
    rom_exc.status_set = kstatus | ROM_ST_EXL;

    ROM_TRY("move $26, %[data]\n\t"
            "move $27, %[mark]\n\t"
            "dmtc0 %[entry], $14\n\t"
            "mtc0 %[sst], $12\n\t"
            "eret",
            [data] "r"(SUP_DATA_VA), [mark] "r"((uint64_t)MARKER_VALUE),
            [entry] "r"(SUP_CODE_VA), [sst] "r"(sstatus));
}

void rom_main(void)
{
    uint64_t v;

    /*
     * Extended kernel addressing on for the xkseg half; every assertion
     * says
     * which of the other extended-addressing bits it sets. Every TLB
     * entry
     * is parked first so that a mapped access can only appear as a
     * refill,
     * never as a stale hit.
     */
    kstatus = rom_get_status() | ROM_ST_KX;
    rom_set_status(kstatus);
    rom_tlb_park(ROM_TLB_ENTRIES);

    /*
     * The user region with UX = 1 and ERL = 0: the whole 16-Tbyte space
     * exists, so both ends of it miss in the TLB and arrive at the
     * extended
     * refill entry point as refills, not refusals.
     */
    rom_set_status(kstatus | ROM_ST_UX);
    probe(XKUSEG_FIRST_HIGH);
    rom_assert_eq(1, 1, rom_exc.count);
    rom_assert_eq(2, VEC_BOOTSTRAP_XTLB, rom_exc.vector);
    rom_assert_eq(3, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(4, XKUSEG_FIRST_HIGH, rom_exc.badvaddr);
    probe(XKUSEG_LAST);
    rom_assert_eq(5, 1, rom_exc.count);
    rom_assert_eq(6, VEC_BOOTSTRAP_XTLB, rom_exc.vector);

    /*
     * Past the 44 bits the processor has, refused whatever the mode.
     */
    probe(XKUSEG_PAST);
    rom_assert_eq(7, 1, rom_exc.count);
    rom_assert_eq(8, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(9, XKUSEG_PAST, rom_exc.badvaddr);

    /*
     * With UX clear the same space above 2 Gbytes is a refusal.
     */
    rom_set_status(kstatus);
    probe(XKUSEG_FIRST_HIGH);
    rom_assert_eq(10, 1, rom_exc.count);
    rom_assert_eq(11, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(12, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);

    /*
     * The same two probes with error level set and UX still on: the
     * manual's
     * "UX = 0 or ERL = 1" makes ERL alone shrink the space, so both
     * must now
     * be address errors even though UX would have allowed them. The
     * Status
     * register to reinstall is staged before each probe because the
     * handler
     * must drop error level on the way back for its own return to work.
     */
    rom_set_status(kstatus | ROM_ST_ERL | ROM_ST_UX);
    probe_staged(XKUSEG_FIRST_HIGH, kstatus | ROM_ST_EXL);
    rom_assert_eq(13, 1, rom_exc.count);
    rom_assert_eq(14, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(15, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(16, XKUSEG_FIRST_HIGH, rom_exc.badvaddr);

    rom_set_status(kstatus | ROM_ST_ERL | ROM_ST_UX);
    probe_staged(XKUSEG_LAST, kstatus | ROM_ST_EXL);
    rom_assert_eq(17, 1, rom_exc.count);
    rom_assert_eq(18, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * The half of the rule error level leaves standing: below 2 Gbytes
     * the
     * user segment becomes an unmapped alias of physical memory. A
     * sentinel
     * planted through the compatibility segment must come back through
     * the
     * user-segment address of the same physical page, with no exception
     * and
     * in particular no refill -- a translation here would mean the
     * segment
     * was still mapped.
     */
    *(volatile uint64_t *)KSEG1_OF(SCRATCH_PA) = SENTINEL;
    rom_set_status(kstatus | ROM_ST_ERL | ROM_ST_UX);
    rom_exc_reset();
    v = load64(SCRATCH_PA);
    rom_set_status(kstatus);
    rom_assert_eq(19, SENTINEL, v);
    rom_assert_eq(20, 0, rom_exc.count);

    /*
     * The top of mapped kernel space. Both ends of xkseg proper are
     * refills;
     * the first address of the carved-out top band and a point deep
     * inside
     * it are refusals. Without extended kernel addressing the region's
     * very
     * base is refused too.
     */
    probe(XKSEG_BASE);
    rom_assert_eq(21, 1, rom_exc.count);
    rom_assert_eq(22, VEC_BOOTSTRAP_XTLB, rom_exc.vector);
    rom_assert_eq(23, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    probe(XKSEG_LAST);
    rom_assert_eq(24, 1, rom_exc.count);
    rom_assert_eq(25, VEC_BOOTSTRAP_XTLB, rom_exc.vector);
    rom_assert_eq(26, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));

    probe(XKSEG_HOLE);
    rom_assert_eq(27, 1, rom_exc.count);
    rom_assert_eq(28, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(29, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(30, XKSEG_HOLE, rom_exc.badvaddr);
    probe(XKSEG_HOLE_HIGH);
    rom_assert_eq(31, 1, rom_exc.count);
    rom_assert_eq(32, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));

    rom_set_status(kstatus & ~ROM_ST_KX);
    probe(XKSEG_BASE);
    rom_set_status(kstatus);
    rom_assert_eq(33, 1, rom_exc.count);
    rom_assert_eq(34, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * The supervisor instruction-set gate. Everything from here runs
     * the
     * probe from a page this ROM maps into supervisor space.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    map_sup_page();
    copy_sup_probe();

    /*
     * In kernel mode the doubleword instruction runs whatever SX says.
     */
    rom_exc_reset();
    ROM_TRY("daddu $27, $27, $0");
    rom_assert_eq(35, 0, rom_exc.count);

    /*
     * Supervisor mode with SX clear: the instruction is refused as a
     * reserved instruction at its own address, and the routine never
     * reaches
     * its marker.
     */
    run_sup_probe(ROM_ST_BEV | ROM_ST_KSU_SUPERVISOR | ROM_ST_EXL);
    rom_assert_eq(36, 1, rom_exc.count);
    rom_assert_eq(37, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(38, SUP_CODE_VA, rom_exc.epc);
    rom_assert_eq(39, 0, get_marker());

    /*
     * Supervisor mode with SX set: the instruction runs, the marker is
     * written, and the routine gets back to the kernel through its own
     * system call.
     */
    run_sup_probe(ROM_ST_BEV | ROM_ST_KSU_SUPERVISOR | ROM_ST_EXL |
                  ROM_ST_SX);
    rom_assert_eq(40, 1, rom_exc.count);
    rom_assert_eq(41, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(42, MARKER_VALUE, get_marker());
}
