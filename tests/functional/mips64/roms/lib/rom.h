/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The interface a bare-metal test ROM is written against.
 *
 * A ROM defines its name and a rom_main(); returning from rom_main() reports a
 * pass. Everything else here is either a way to reach a coprocessor 0
 * register, a way to arm and inspect an exception, or an assertion.
 *
 * Assertion numbers are per ROM and start at one. A failing assertion is the
 * ROM's exit status as well as a printed line, so a runner can tell which
 * check failed without parsing anything.
 */

#ifndef ROM_H
#define ROM_H

#include "exc-offsets.h"

typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef signed int int32_t;

/* Defined by each ROM. */
extern const char rom_name[];
void rom_main(void);

/*
 * Statuses that are not assertion numbers. An assertion number must stay
 * below the first of them, and the process exit status is only eight bits
 * wide, so there is no room to be generous.
 */
#define ROM_STATUS_UNEXPECTED_EXCEPTION 250
#define ROM_STATUS_MAX_ASSERT           199

void rom_write(const char *buf, unsigned long len);
void rom_puts(const char *s);
void rom_puthex(uint64_t v);
void rom_exit(int status) __attribute__((noreturn));
void rom_pass(void) __attribute__((noreturn));
void rom_fail(int id, uint64_t expected, uint64_t actual)
    __attribute__((noreturn));
void rom_report_unexpected_exception(void) __attribute__((noreturn));

/* The workhorse: every expected value in the suite arrives through here. */
static inline void rom_assert_eq(int id, uint64_t expected, uint64_t actual)
{
    if (expected != actual) {
        rom_fail(id, expected, actual);
    }
}

static inline void rom_assert(int id, int condition)
{
    rom_assert_eq(id, 1, condition ? 1 : 0);
}

/* The record the assembly exception handler writes. */
struct rom_exc {
    uint64_t count;
    uint64_t vector;
    uint64_t resume;
    uint64_t cause;
    uint64_t status;
    uint64_t epc;
    uint64_t badvaddr;
    uint64_t context;
    uint64_t xcontext;
    uint64_t entryhi;
    uint64_t errorepc;
    uint64_t refill;
    uint64_t entrylo0;
    uint64_t entrylo1;
    uint64_t pagemask;
    uint64_t status_set;
    uint64_t jump;
};

extern struct rom_exc rom_exc;

_Static_assert(__builtin_offsetof(struct rom_exc, count) == EXC_COUNT,
               "count");
_Static_assert(__builtin_offsetof(struct rom_exc, vector) == EXC_VECTOR,
               "vector");
_Static_assert(__builtin_offsetof(struct rom_exc, resume) == EXC_RESUME,
               "resume");
_Static_assert(__builtin_offsetof(struct rom_exc, cause) == EXC_CAUSE,
               "cause");
_Static_assert(__builtin_offsetof(struct rom_exc, status) == EXC_STATUS,
               "status");
_Static_assert(__builtin_offsetof(struct rom_exc, epc) == EXC_EPC, "epc");
_Static_assert(__builtin_offsetof(struct rom_exc, badvaddr) == EXC_BADVADDR,
               "badvaddr");
_Static_assert(__builtin_offsetof(struct rom_exc, context) == EXC_CONTEXT,
               "context");
_Static_assert(__builtin_offsetof(struct rom_exc, xcontext) == EXC_XCONTEXT,
               "xcontext");
_Static_assert(__builtin_offsetof(struct rom_exc, entryhi) == EXC_ENTRYHI,
               "entryhi");
_Static_assert(__builtin_offsetof(struct rom_exc, errorepc) == EXC_ERROREPC,
               "errorepc");
_Static_assert(__builtin_offsetof(struct rom_exc, refill) == EXC_REFILL,
               "refill");
_Static_assert(__builtin_offsetof(struct rom_exc, entrylo0) == EXC_ENTRYLO0,
               "entrylo0");
_Static_assert(__builtin_offsetof(struct rom_exc, entrylo1) == EXC_ENTRYLO1,
               "entrylo1");
_Static_assert(__builtin_offsetof(struct rom_exc, pagemask) == EXC_PAGEMASK,
               "pagemask");
_Static_assert(__builtin_offsetof(struct rom_exc, status_set) == EXC_STATUS_SET,
               "status_set");
_Static_assert(__builtin_offsetof(struct rom_exc, jump) == EXC_JUMP, "jump");
_Static_assert(sizeof(struct rom_exc) == EXC_SIZE, "record size");

/*
 * Run one or more instructions with a return address armed, so that a fault
 * anywhere inside them continues after the block instead of being reported as
 * unexpected. Registers the block needs are passed as extra operands, exactly
 * as for a plain asm statement. The kernel scratch register the arming code
 * uses is declared clobbered because an exception would have overwritten it
 * anyway.
 */
#define ROM_TRY(insns, ...)                                                   \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[rom_resume])\n\t"                       \
                         insns "\n"                                           \
                         "9:\t.set pop"                                       \
                         :                                                    \
                         : [rom_resume] "r"(&rom_exc.resume), ##__VA_ARGS__   \
                         : "$26", "memory")

static inline void rom_exc_reset(void)
{
    rom_exc.count = 0;
    rom_exc.vector = ~0ULL;
    rom_exc.resume = 0;
    rom_exc.cause = 0;
    rom_exc.refill = 0;
    rom_exc.status_set = 0;
    rom_exc.jump = 0;
}

/* Coprocessor 0. The register numbers are the ones the manual gives. */
#define ROM_CP0_32(name, reg)                                                 \
    static inline uint32_t rom_get_##name(void)                               \
    {                                                                         \
        uint32_t v;                                                           \
        __asm__ __volatile__("mfc0 %0, $" #reg : "=r"(v));                    \
        return v;                                                             \
    }                                                                         \
    static inline void rom_set_##name(uint32_t v)                             \
    {                                                                         \
        __asm__ __volatile__("mtc0 %0, $" #reg : : "r"(v));                   \
    }

#define ROM_CP0_64(name, reg)                                                 \
    static inline uint64_t rom_get_##name(void)                               \
    {                                                                         \
        uint64_t v;                                                           \
        __asm__ __volatile__("dmfc0 %0, $" #reg : "=r"(v));                   \
        return v;                                                             \
    }                                                                         \
    static inline void rom_set_##name(uint64_t v)                             \
    {                                                                         \
        __asm__ __volatile__("dmtc0 %0, $" #reg : : "r"(v));                  \
    }

ROM_CP0_32(index, 0)
ROM_CP0_32(random, 1)
ROM_CP0_64(entrylo0, 2)
ROM_CP0_64(entrylo1, 3)
ROM_CP0_64(context, 4)
ROM_CP0_32(pagemask, 5)
ROM_CP0_32(wired, 6)
ROM_CP0_64(badvaddr, 8)
ROM_CP0_32(count, 9)
ROM_CP0_64(entryhi, 10)
ROM_CP0_32(compare, 11)
ROM_CP0_32(status, 12)
ROM_CP0_32(cause, 13)
ROM_CP0_64(epc, 14)
ROM_CP0_32(prid, 15)
ROM_CP0_32(config, 16)
ROM_CP0_32(lladdr, 17)
ROM_CP0_32(watchlo, 18)
ROM_CP0_32(watchhi, 19)
ROM_CP0_64(xcontext, 20)
ROM_CP0_32(framemask, 21)
ROM_CP0_32(ecc, 26)
ROM_CP0_32(cacherr, 27)
ROM_CP0_32(taglo, 28)
ROM_CP0_32(taghi, 29)
ROM_CP0_64(brdiag, 22)
ROM_CP0_64(errorepc, 30)

/* Status register bits, from the R10000 manual's Status register figure. */
#define ROM_ST_XX   (1u << 31)
#define ROM_ST_CU2  (1u << 30)
#define ROM_ST_CU1  (1u << 29)
#define ROM_ST_CU0  (1u << 28)
#define ROM_ST_RP   (1u << 27)
#define ROM_ST_FR   (1u << 26)
#define ROM_ST_RE   (1u << 25)
#define ROM_ST_BEV  (1u << 22)
#define ROM_ST_TS   (1u << 21)
#define ROM_ST_SR   (1u << 20)
#define ROM_ST_NMI  (1u << 19)
#define ROM_ST_CH   (1u << 18)
#define ROM_ST_CE   (1u << 17)
#define ROM_ST_DE   (1u << 16)
/* The two bits the diagnostic status field figure leaves reserved. */
#define ROM_ST_DS_RESERVED (3u << 23)
#define ROM_ST_KX   (1u << 7)
#define ROM_ST_SX   (1u << 6)
#define ROM_ST_UX   (1u << 5)
#define ROM_ST_KSU_USER       (2u << 3)
#define ROM_ST_KSU_SUPERVISOR (1u << 3)
#define ROM_ST_KSU_KERNEL     (0u << 3)
#define ROM_ST_ERL  (1u << 2)
#define ROM_ST_EXL  (1u << 1)
#define ROM_ST_IE   (1u << 0)

/* Cause register fields. */
#define ROM_CAUSE_BD        (1u << 31)
#define ROM_CAUSE_CE_SHIFT  28
#define ROM_CAUSE_IP7       (1u << 15)
#define ROM_CAUSE_EXC(c)    (((c) >> 2) & 0x1f)

/* Exception codes, from the Cause register's exception code table. */
#define ROM_EXC_INT   0
#define ROM_EXC_MOD   1
#define ROM_EXC_TLBL  2
#define ROM_EXC_TLBS  3
#define ROM_EXC_ADEL  4
#define ROM_EXC_ADES  5
#define ROM_EXC_DBE   7
#define ROM_EXC_SYS   8
#define ROM_EXC_BP    9
#define ROM_EXC_RI    10
#define ROM_EXC_CPU   11
#define ROM_EXC_OV    12
#define ROM_EXC_TR    13
#define ROM_EXC_FPE   15
#define ROM_EXC_WATCH 23

/* TLB entry fields, from the EntryLo and EntryHi register figures. */
#define ROM_ENTRYLO_G       (1ull << 0)
#define ROM_ENTRYLO_V       (1ull << 1)
#define ROM_ENTRYLO_D       (1ull << 2)
#define ROM_ENTRYLO_C_SHIFT 3
#define ROM_ENTRYLO_PFN_SHIFT 6
#define ROM_CACHE_UNCACHED  2
#define ROM_CACHE_NONCOHERENT 3

/*
 * The uncached attribute an entry carries, in the top two bits of the low half.
 * An uncached reference drives it out beside the address. On this machine it
 * names which of four spaces the reference is for. A mapping that means memory
 * has to say so: the value that means memory is three, and zero — which is
 * an entry built without thinking about it would carry — means the Hub's own
 * registers.
 */
#define ROM_ENTRYLO_UC_SHIFT 62
#define ROM_UC_HUB      0
#define ROM_UC_XTALK    1
#define ROM_UC_MEMSPEC  2
#define ROM_UC_MEMORY   3

/*
 * Build the low half of a TLB entry mapping a physical address in a named
 * space. Only an uncached entry carries an attribute; a cached one reaches
 * memory whatever is in those bits.
 */
static inline uint64_t rom_entrylo_in(uint64_t paddr, uint64_t flags, int cache,
                                      int uc)
{
    return ((paddr >> 12) << ROM_ENTRYLO_PFN_SHIFT) |
           ((uint64_t)cache << ROM_ENTRYLO_C_SHIFT) |
           ((uint64_t)uc << ROM_ENTRYLO_UC_SHIFT) | flags;
}

/*
 * The same with the attribute left zero, for a cached mapping, where no
 * attribute is driven and the bits mean nothing. A ROM that runs against more
 * than one processor has to use this: on the R4000 those two bits are the
 * read-inhibit and execute-inhibit bits, so setting them there makes the page
 * unreadable rather than naming a space.
 */
static inline uint64_t rom_entrylo(uint64_t paddr, uint64_t flags, int cache)
{
    return rom_entrylo_in(paddr, flags, cache, 0);
}

/* PageMask values, from the R10000 manual's page size table. */
#define ROM_PAGEMASK_4K   0x00000000u
#define ROM_PAGEMASK_16K  0x00006000u
#define ROM_PAGEMASK_64K  0x0001e000u
#define ROM_PAGEMASK_256K 0x0007e000u
#define ROM_PAGEMASK_1M   0x001fe000u
#define ROM_PAGEMASK_4M   0x007fe000u
#define ROM_PAGEMASK_16M  0x01ffe000u

/*
 * Put every TLB entry out of the way of the addresses the tests use, the way
 * firmware does before it installs any mapping of its own: a distinct virtual
 * page number per entry, both halves invalid. The chosen page numbers sit far
 * above the region the suite maps, so no test can accidentally hit one.
 */
#define ROM_TLB_ENTRIES 64
#define ROM_TLB_PARK_BASE 0x000000003f000000ull

static inline void rom_tlb_park(int entries)
{
    int i;

    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entrylo0(0);
    rom_set_entrylo1(0);
    for (i = 0; i < entries; i++) {
        rom_set_entryhi(ROM_TLB_PARK_BASE + (uint64_t)i * 0x2000);
        rom_set_index(i);
        __asm__ __volatile__("tlbwi" : : : "memory");
    }
    rom_set_entryhi(0);
}

#endif /* ROM_H */
