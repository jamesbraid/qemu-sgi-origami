/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Check node-qualified Hub routing from all four processors. Scratch
 * markers expose accesses folded to one node; other configurations fail.
 */

#include "rom.h"

const char rom_name[] = "foldback";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1               /* the attribute that means crosstalk */

#define HUB_LOCAL       0x01000000ull   /* "my own Hub", whoever is asking */
#define HUB_NODE        0x01800000ull   /* "the Hub of the node I name" */
#define NODE(n)         ((uint64_t)(n) << 32)

#define PI_PROCESSOR_NUMBER 0x000020    /* which of its node's two is reading */

/*
 * The board's own master/slave strap, readable locally on either node
 * without needing a node number of its own -- unlike
 * PI_PROCESSOR_NUMBER
 * above, this one exists before anything has been assigned.
 */
#define MD_SLOT_ID_USTAT     0x220048
#define MD_SLOT_MASTER       (1ull << 7)  /* set on the first node only */

/* Assign node 1 in the emulator before testing node-qualified accesses.
 * This setup does not establish physical Hub vector-routing behavior.
 */
#define SN0_NI_STATUS             0x600000
#define SN0_NI_STATUS_MORE_NODES  (1ull << 18)  /* one node knows of another */
#define SN0_NI_STATUS_REGION_SIZE (1ull << 17)
#define SN0_NI_STATUS_NODE_SHIFT  8
#define SN0_NI_VECTOR_PARMS       0x600200
#define SN0_NI_VECTOR             0x600208
#define SN0_NI_VECTOR_DATA        0x600210
#define SN0_PIOTYPE_WRITE         1

/*
 * Separate marker and verdict registers so remote marker writes cannot
 * erase another processor's result.
 */
#define IO_SCRATCH(slice)   (0x400150 + 8 * (slice))
#define NI_SCRATCH(slice)   (0x600100 + 8 * (slice))

#define OWN(off)        XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_LOCAL + (off))
#define AT(node, off) \
    XKPHYS(CCA_UNCACHED, ATTR_XTALK, NODE(node) + HUB_NODE + (off))

/*
 * Markers fit the scratch register's writable mask, and are distinct on
 * sight.
 */
#define MARK_NODE0      0x0AAAull
#define MARK_NODE1      0x0555ull

#define VERDICT_PASS    0x0E1ull        /* both reads arrived where addressed */
/* The first node's read returned the second's. */
#define VERDICT_FOLD    0x0F0ull
/* Neither shape: the test itself is wrong. */
#define VERDICT_OTHER   0x0EEull
#define VERDICT_NONE    0ull

/*
 * Distinct from any assertion number: the machine, not the contract.
 */
#define ROM_STATUS_WRONG_MACHINE 200

/* One processor reports so the result is emitted as a single line. */
#define ASSERT_ALL_CROSSED   1

static void put(uint64_t addr, uint64_t val)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get(uint64_t addr)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    return *(volatile uint64_t *)(unsigned long)addr;
}

void rom_main(void)
{
    unsigned int slice, first_node;
    uint64_t r0, r1, verdict;
    uint64_t seen[2][2];
    unsigned int node, s;

    /* Hub windows require 64-bit unmapped addressing. */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /* Read this processor's slice from its local Hub window. */
    slice = (unsigned int)(get(OWN(PI_PROCESSOR_NUMBER)) & 1);

    /* Assign node 1 before using node-qualified addresses. */
    first_node = (get(OWN(MD_SLOT_ID_USTAT)) & MD_SLOT_MASTER) != 0;
    if (first_node) {
        if (slice == 0) {
            put(OWN(SN0_NI_VECTOR), 0);
            put(OWN(SN0_NI_VECTOR_DATA), SN0_NI_STATUS_REGION_SIZE |
                (1ull << SN0_NI_STATUS_NODE_SHIFT));
            put(OWN(SN0_NI_VECTOR_PARMS), SN0_NI_STATUS | SN0_PIOTYPE_WRITE);
            put(OWN(SN0_NI_STATUS),
                get(OWN(SN0_NI_STATUS)) | SN0_NI_STATUS_MORE_NODES);
        } else {
            while ((get(OWN(SN0_NI_STATUS)) & SN0_NI_STATUS_MORE_NODES) ==
                   0) {
            }
        }
    } else {
        while (((get(OWN(SN0_NI_STATUS)) >> SN0_NI_STATUS_NODE_SHIFT) &
                0xff) == 0) {
        }
    }

    /* Refuse configurations without the second node required by this test. */
    rom_exc_reset();
    ROM_TRY("ld $27, 0(%[a])", [a] "r"((unsigned long)AT(1, IO_SCRATCH(0))));
    if (rom_exc.count != 0) {
        rom_puts("foldback: no second node on this machine -- this test needs\n"
                 "          the two-node four-processor configuration, and\n"
                 "          cannot say anything about cross-node addressing\n"
                 "          without it.\n");
        rom_exit(ROM_STATUS_WRONG_MACHINE);
    }

    /* Write distinct markers and read them through each named-node view. */
    put(AT(0, IO_SCRATCH(slice)), MARK_NODE0);
    put(AT(1, IO_SCRATCH(slice)), MARK_NODE1);
    r0 = get(AT(0, IO_SCRATCH(slice)));
    r1 = get(AT(1, IO_SCRATCH(slice)));

    if (r0 == MARK_NODE0 && r1 == MARK_NODE1) {
        verdict = VERDICT_PASS;
    } else if (r0 == MARK_NODE1 && r1 == MARK_NODE1) {
        /* Both named writes reached the same register on the second node. */
        verdict = VERDICT_FOLD;
    } else {
        verdict = VERDICT_OTHER;
    }

    /* Publish this processor's verdict through its local Hub. */
    put(OWN(NI_SCRATCH(slice)), verdict);

    /* Publish one complete verdict line from the first node's first processor. */
    if (verdict != VERDICT_PASS ||
        slice != 0 ||
        get(OWN(IO_SCRATCH(slice))) != MARK_NODE0) {
        for (;;) {
            /*
             * verdict published; this processor has nothing further to
             * add
             */
        }
    }

    /* Collect all verdicts through the elected processor's node views. */
    for (;;) {
        unsigned int ready = 1;

        for (node = 0; node < 2; node++) {
            for (s = 0; s < 2; s++) {
                seen[node][s] = get(AT(node, NI_SCRATCH(s)));
                if (seen[node][s] == VERDICT_NONE) {
                    ready = 0;
                }
            }
        }
        if (ready) {
            break;
        }
    }

    for (node = 0; node < 2; node++) {
        for (s = 0; s < 2; s++) {
            rom_puts("foldback: node ");
            rom_puthex(node);
            rom_puts(" processor ");
            rom_puthex(s);
            rom_puts(" verdict ");
            rom_puthex(seen[node][s]);
            rom_puts(seen[node][s] == VERDICT_PASS ?
                     " crossed\n" : " DID NOT CROSS\n");
        }
    }

    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[1][0]);
    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[1][1]);
    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[0][0]);
    rom_assert_eq(ASSERT_ALL_CROSSED, VERDICT_PASS, seen[0][1]);
}
