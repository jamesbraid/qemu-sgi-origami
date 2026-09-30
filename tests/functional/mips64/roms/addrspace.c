/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check R10000 extended address boundaries and supervisor SX permission
 * (R10000 User's Manual v2.0).
 * The low ERL alias uses backed memory so a bus error cannot mimic a segment
 * refusal; this cacheless ROM does not test ERL's uncached attribute.
 */

#include "rom.h"
#include "probe-symbols.h"

const char rom_name[] = "addrspace";

/* Bootstrap vector identifiers used by the exception checks below. */
#define VEC_BOOTSTRAP_XTLB    (ROM_VEC_BOOTSTRAP | ROM_VEC_XTLB_REFILL)
#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/* Backed scratch page, also used by xkphys.c. */
#define SCRATCH_PA 0x00400000ull

/* Uncached compatibility-segment alias of the scratch page. */
#define KSEG1_OF(pa) (0xffffffffa0000000ull + (pa))

/* Distinct marker for alias readback. */
#define SENTINEL 0xa5d0fadeb00cfaceull

/* Aligned probes distinguish segment refusals from address errors. */
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
 * Stage Status after resetting the exception record; ERL probes need the
 * handler to clear ERL before returning to the kernel address.
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

/* Load variant used for successful accesses. */
static uint64_t load64(uint64_t addr)
{
    uint64_t v;

    __asm__ __volatile__("ld %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

/*
 * Runs the tested instruction in supervisor mode, writes a marker on
 * success, then traps back to the kernel.
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

/* Supervisor code and data pages in the 32-bit reachable user segment. */
#define SUP_CODE_VA 0x0000000040400000ull
#define SUP_DATA_VA 0x0000000040401000ull
#define SUP_CODE_PA 0x0000000000300000ull
#define SUP_DATA_PA 0x0000000000301000ull

#define MARKER_OFFSET 8
#define MARKER_VALUE  0x5a5aa5a5u

/* Reserved TLB entry for the supervisor mapping. */
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
        (volatile uint32_t *)KSEG1_OF(SUP_DATA_PA + MARKER_OFFSET);

    *m = v;
}

static uint32_t get_marker(void)
{
    /* Volatile preserves reads through the uncached KSEG1 alias. */
    volatile uint32_t *m =
        (volatile uint32_t *)KSEG1_OF(SUP_DATA_PA + MARKER_OFFSET);

    return *m;
}

/*
 * Enter supervisor mode and return through the exception handler; its
 * staged Status value is required because a supervisor return to kernel
 * space would itself address-error.
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
