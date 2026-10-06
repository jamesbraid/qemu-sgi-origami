/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Hub's inbound XIO window, as a Bridge's DMA meets it. A memory-class
 * address below 2^40 passes through the Hub unchanged into the node-qualified
 * claim view, so it lands on whichever node holds that number. The
 * interrupt-modify write, the peer identity probe and every other address
 * keep the Hub's own decode, and that decode refuses what it does not
 * recognise.
 *
 * Two kinds of test card do the transfers. One sits behind its slot's Bridge
 * route and exercises the whole path, byte lanes included. The other has its
 * DMA linked straight to the Hub's inbound face, so a test can present any
 * 48-bit XIO address and access size the Bridge would never form.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sn0-user-image.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

/* The Bridge at widget 8 of each node, its registers four bytes into a slot. */
#define BRIDGE_WIDGET       8
#define BRIDGE_BASE         (SPACE_IO + ((uint64_t)BRIDGE_WIDGET << 24))
#define BRIDGE_AT(node)     (BRIDGE_BASE + (uint64_t)(node) * NODE_STRIDE)
#define REG_DATA            4
#define REG(node, offset)   (BRIDGE_AT(node) + (offset) + REG_DATA)
#define BRIDGE_DIRECT_MAP   0x080
#define BRIDGE_DEVICE(n)    (0x200 + 8 * (n))
#define DEV_SWAP_DIRECT     (1u << 19)
#define BRIDGE_CFG0         0x020000
#define CFG0(node, device)  (BRIDGE_AT(node) + BRIDGE_CFG0 + (device) * 0x1000)
#define CARD_MMIO(slot)     (0x10000000u + (slot) * 0x1000000u)

static const uint64_t window_offset[8] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};
#define WINDOW(node, slot)  (BRIDGE_AT(node) + window_offset[slot])

/* The bus-mastering test card's registers (hw/misc/sgi-bridge-dma-test.c). */
#define DMA_REG_ADDRESS     0x00
#define DMA_REG_LENGTH      0x04
#define DMA_REG_SEED        0x08
#define DMA_REG_GO          0x0c
#define DMA_REG_SOURCE      0x14
#define DMA_REG_ADDRESS_HI  0x18
#define DMA_REG_SOURCE_HI   0x1c
#define DMA_REG_RESULT      0x28
#define DMA_GO_FILL         1
#define DMA_GO_COPY         2
#define DMA_MAX             65536

/* The memory API's transaction results, as the card reports them. */
#define RESULT_OK           0
#define RESULT_ERROR        1
#define RESULT_DECODE_ERROR 2

/*
 * The Bridge's 64-bit direct window: widget 0xa in the top nibble, then the
 * 40-bit system address, which carries the node number at bit 32.
 */
#define PCI_DIRECT64(sys)   (0xa000000000000000ULL | (sys))

/* Inbound XIO addresses: the memory view ends at 2^40; bit 47 is I/O. */
#define XIO_MEMORY_END      (1ULL << 40)
#define XIO_IO_VIEW         (1ULL << 47)
#define XIO_NASID(n)        ((uint64_t)(n) << 38)
#define XIO_INT_MODIFY(n)   (XIO_IO_VIEW | XIO_NASID(n) | 0x01800090ULL)
#define XIO_IDENTITY        (XIO_IO_VIEW | 0x01600000ULL)

#define NI_STATUS           0x600000
#define NI_REGION_SIZE      (1ULL << 17)
#define NI_NODE_MASK        (0x1ffULL << 8)
#define NI_NODE(n)          ((uint64_t)(n) << 8)
#define INT_MODIFY_SET      0x100

/* Slots on node 0's Bridge; the base I/O functions use 0 to 2. */
#define SLOT_NATIVE         3   /* plain memory API transfers */
#define SLOT_FACE           5   /* DMA linked to the Hub's inbound face */

#define HUB_FACE(node)      "/machine/node" #node "/hub/sgi-hub.ii.packet[0]"

#define CARDS(bus, node) \
    " -device sgi-bridge-dma-test,bus=" bus ",addr=3" \
    " -device sgi-bridge-dma-test,bus=" bus ",addr=5," \
    "dma-memory=" HUB_FACE(node)

#define MACHINE_SINGLE      "-M origin200 -m 128M" CARDS("pci", 0)
#define MACHINE_DUAL \
    "-M origin200,topology=origin200-dual -m 128M -smp 4" CARDS("pci.1", 1)

/* RAM well clear of what the PROM and the base I/O functions use. */
#define TARGET              0x00c00000ULL
#define SOURCE              0x00d00000ULL
#define GUARD               16

/* A number that is no board position on a two-node machine. */
#define FAR_NUMBER          7

static void card_in_slot(QTestState *qts, unsigned int node,
                         unsigned int slot, uint32_t swap_bits)
{
    qtest_writel(qts, CFG0(node, slot) + PCI_BAR0, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(node, slot) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG(node, BRIDGE_DEVICE(slot)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20) | swap_bits);
}

static void cards_ready(QTestState *qts, unsigned int node, uint32_t swap_bits)
{
    qtest_writel(qts, REG(node, BRIDGE_DIRECT_MAP), 0);
    card_in_slot(qts, node, SLOT_NATIVE, swap_bits);
    card_in_slot(qts, node, SLOT_FACE, 0);
}

static void card_swap(QTestState *qts, unsigned int node, uint32_t swap_bits)
{
    card_in_slot(qts, node, SLOT_NATIVE, swap_bits);
}

/* One fill or copy; returns the card's transaction result. */
static uint32_t card_run(QTestState *qts, unsigned int node, unsigned int slot,
                         unsigned int op, uint64_t address, uint64_t source,
                         uint32_t length, uint32_t seed)
{
    uint64_t window = WINDOW(node, slot);

    qtest_writel(qts, window + DMA_REG_ADDRESS, (uint32_t)address);
    qtest_writel(qts, window + DMA_REG_ADDRESS_HI, address >> 32);
    qtest_writel(qts, window + DMA_REG_SOURCE, (uint32_t)source);
    qtest_writel(qts, window + DMA_REG_SOURCE_HI, source >> 32);
    qtest_writel(qts, window + DMA_REG_LENGTH, length);
    qtest_writel(qts, window + DMA_REG_SEED, seed);
    qtest_writel(qts, window + DMA_REG_GO, op);
    return qtest_readl(qts, window + DMA_REG_RESULT);
}

/*
 * Where PCI byte i of a transfer starting at host offset start lands. With
 * the slot's swap bit set the stream keeps its order; clear, the Bridge
 * places host offset k on PCI byte k ^ 3 and keeps word order (bridge.md,
 * "Byte lanes on the DMA path", established for 32-bit data phases).
 */
static uint64_t lane(uint64_t start, unsigned int i, bool swapped)
{
    return swapped ? ((start + i) ^ 3) : start + i;
}

/*
 * Fill [start, start + length) through a slot and check every byte of the
 * span and its guards. The swap rule can move a byte up to three places out
 * of the span, so the guards are what catch a byte that went astray.
 */
static void check_fill(QTestState *qts, uint64_t host_base, unsigned int node,
                       unsigned int slot, uint64_t pci_base, uint64_t start,
                       uint32_t length, bool swapped, uint8_t seed)
{
    uint64_t lo = start - GUARD, span = length + 2 * GUARD;
    g_autofree uint8_t *seen = g_malloc(span);
    g_autofree uint8_t *want = g_malloc(span);

    memset(want, 0xee, span);
    for (unsigned int i = 0; i < length; i++) {
        want[lane(start, i, swapped) - lo] = (uint8_t)(seed + i);
    }
    qtest_memset(qts, host_base + lo, 0xee, span);
    g_assert_cmpuint(card_run(qts, node, slot, DMA_GO_FILL, pci_base + start,
                              0, length, seed), ==, RESULT_OK);
    qtest_memread(qts, host_base + lo, seen, span);
    if (memcmp(seen, want, span)) {
        for (uint64_t i = 0; i < span; i++) {
            if (seen[i] != want[i]) {
                g_test_message("slot %u start 0x%" PRIx64 " length %u: "
                               "byte 0x%" PRIx64 " is 0x%02x, not 0x%02x",
                               slot, start, length, lo + i, seen[i], want[i]);
                break;
            }
        }
        g_assert_not_reached();
    }
}

/*
 * The read direction: copy through the same route. A byte read from host
 * offset (s + i) ^ 3 is written to host offset (d + i) ^ 3 when the swap is
 * clear, so the expectation composes the same lane rule twice.
 */
static void check_copy(QTestState *qts, unsigned int slot, uint64_t dst,
                       uint64_t src, uint32_t length, bool swapped)
{
    uint64_t lo = dst - GUARD, span = length + 2 * GUARD;
    uint64_t src_lo = src - GUARD;
    g_autofree uint8_t *source = g_malloc(length + 2 * GUARD);
    g_autofree uint8_t *seen = g_malloc(span);
    g_autofree uint8_t *want = g_malloc(span);

    for (unsigned int i = 0; i < length + 2 * GUARD; i++) {
        source[i] = (uint8_t)(0x40 + i * 7);
    }
    qtest_memwrite(qts, src_lo, source, length + 2 * GUARD);
    memset(want, 0xee, span);
    for (unsigned int i = 0; i < length; i++) {
        want[lane(dst, i, swapped) - lo] =
            source[lane(src, i, swapped) - src_lo];
    }
    qtest_memset(qts, lo, 0xee, span);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY, PCI_DIRECT64(dst),
                              PCI_DIRECT64(src), length, 0), ==, RESULT_OK);
    qtest_memread(qts, lo, seen, span);
    g_assert_cmpmem(seen, span, want, span);
}

static const uint32_t lengths[] = {
    1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 64, 127, 1500, 4096, 4100, 9000,
};

/*
 * Bridge DMA into Hub-local RAM, both swap settings, both transfer
 * interfaces, every start alignment within a doubleword, across a page
 * boundary for the longer lengths. Each length starts eight bytes before a
 * page end so anything from nine bytes on crosses it.
 */
static void test_bridge_dma_bytes(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_SINGLE);

    cards_ready(qts, 0, 0);
    for (unsigned int swap = 0; swap < 2; swap++) {
        card_swap(qts, 0, swap ? 0 : DEV_SWAP_DIRECT);
        for (unsigned int l = 0; l < G_N_ELEMENTS(lengths); l++) {
            for (unsigned int align = 0; align < 8; align++) {
                uint64_t start = TARGET + 0xff8 + align;

                check_fill(qts, 0, 0, SLOT_NATIVE, PCI_DIRECT64(0), start,
                           lengths[l], swap, 0x11 + l + align);
            }
        }
        for (unsigned int align = 0; align < 8; align++) {
            check_copy(qts, SLOT_NATIVE, TARGET + 0x3ff8 + align,
                       SOURCE + 0x1ff8 + align, 1500, swap);
            check_copy(qts, SLOT_NATIVE, TARGET + 0x3ff8 + align,
                       SOURCE + 0x1ff8 + ((align + 3) & 7), 61, swap);
        }
    }
    qtest_quit(qts);
}

/* The face cards reach RAM with no Bridge between: bytes keep their order. */
static void test_face_dma_bytes(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_SINGLE);

    cards_ready(qts, 0, 0);
    for (unsigned int l = 0; l < G_N_ELEMENTS(lengths); l++) {
        for (unsigned int align = 0; align < 8; align++) {
            check_fill(qts, 0, 0, SLOT_FACE, 0, TARGET + 0xff8 + align,
                       lengths[l], false, 0x23 + l + align);
        }
    }
    qtest_quit(qts);
}

static uint64_t read_be64(QTestState *qts, uint64_t address)
{
    uint8_t bytes[8];

    qtest_memread(qts, address, bytes, sizeof(bytes));
    return ldq_be_p(bytes);
}

/*
 * Every address the Hub decodes itself, through both face cards: the
 * identity probe answers an eight-byte read and nothing else, the
 * interrupt-modify register takes an eight-byte write and nothing else, and
 * memory-class addresses at or above 2^40, a read or write straddling 2^40,
 * and other I/O-view addresses all fail. A number no Hub holds fails in the
 * claim layer, with its own result.
 */
static void test_decoded_ranges(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_SINGLE);
    const uint64_t pend = HUB_LOCAL + PI_INT_PEND0;
    const uint64_t scratch = TARGET + 0x100;
    const uint64_t refused[] = {
        XIO_MEMORY_END, XIO_MEMORY_END + 0x1000, XIO_MEMORY_END - 4,
        XIO_IO_VIEW | 0x1000, XIO_IO_VIEW | 0x01800098ULL,
        XIO_IDENTITY + 8, (1ULL << 46) | 0x1000,
    };
    const unsigned int slot = SLOT_FACE, level = 50;
    uint64_t status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);

    cards_ready(qts, 0, 0);

    /* The identity probe: whole doubleword reads only. */
    qtest_memset(qts, scratch, 0xee, 16);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY, scratch,
                              XIO_IDENTITY, 8, 0), ==, RESULT_OK);
    g_assert_cmphex(read_be64(qts, scratch), ==, status);
    qtest_memset(qts, scratch, 0xee, 16);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY, scratch,
                              XIO_IDENTITY, 4, 0), ==, RESULT_ERROR);
    g_assert_cmphex(read_be64(qts, scratch), ==, 0xeeeeeeeeeeeeeeeeULL);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_FILL, XIO_IDENTITY,
                              0, 8, 0), ==, RESULT_ERROR);

    /* Interrupt-modify: whole doubleword writes only. */
    g_assert_cmphex(qtest_readq(qts, pend) & (1ULL << level), ==, 0);
    qtest_writeq(qts, scratch, INT_MODIFY_SET | level);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY,
                              XIO_INT_MODIFY(0) + 4, scratch + 4, 4, 0),
                     ==, RESULT_ERROR);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY,
                              XIO_INT_MODIFY(0), scratch, 4, 0),
                     ==, RESULT_ERROR);
    g_assert_cmphex(qtest_readq(qts, pend) & (1ULL << level), ==, 0);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY,
                              XIO_INT_MODIFY(0), scratch, 8, 0),
                     ==, RESULT_OK);
    g_assert_cmphex(qtest_readq(qts, pend) & (1ULL << level), ==,
                    1ULL << level);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY, scratch + 8,
                              XIO_INT_MODIFY(0), 8, 0), ==, RESULT_ERROR);

    /* Everything else the Hub does not decode fails whole. */
    for (unsigned int i = 0; i < G_N_ELEMENTS(refused); i++) {
        g_test_message("slot %u refuses 0x%" PRIx64, slot, refused[i]);
        g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_FILL, refused[i],
                                  0, 8, 0x5a), ==, RESULT_ERROR);
        qtest_memset(qts, scratch, 0xee, 16);
        g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY, scratch,
                                  refused[i], 8, 0), ==, RESULT_ERROR);
        g_assert_cmphex(read_be64(qts, scratch), ==,
                        0xeeeeeeeeeeeeeeeeULL);
    }

    /* A node number nobody holds fails in the claim layer. */
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_FILL,
                              5 * NODE_STRIDE + TARGET, 0, 8, 0x5a), ==,
                     RESULT_DECODE_ERROR);
    g_assert_cmpuint(card_run(qts, 0, slot, DMA_GO_COPY, scratch,
                              5 * NODE_STRIDE + TARGET, 8, 0), ==,
                     RESULT_DECODE_ERROR);
    qtest_quit(qts);
}

/*
 * Count the inbound packet callbacks in a trace: those below 2^40 (plain
 * memory traffic) and those at the interrupt-modify address.
 */
static void count_packet_callbacks(const char *path, unsigned int *memory,
                                   unsigned int *interrupts)
{
    g_autofree char *trace = NULL;
    g_auto(GStrv) lines = NULL;

    *memory = *interrupts = 0;
    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    lines = g_strsplit(trace, "\n", -1);
    for (unsigned int i = 0; lines[i]; i++) {
        const char *event = strstr(lines[i], "sgi_hub_ii_packet_access ");
        uint64_t addr;
        unsigned int size, write;

        if (!event || sscanf(event, "sgi_hub_ii_packet_access addr=0x%"
                             SCNx64 " size=%u write=%u",
                             &addr, &size, &write) != 3) {
            continue;
        }
        if (addr < XIO_MEMORY_END) {
            (*memory)++;
        } else if ((addr & ~XIO_NASID(0x1ff)) == XIO_INT_MODIFY(0)) {
            (*interrupts)++;
        }
    }
}

/*
 * Plain memory traffic takes no inbound packet callback on any route: both
 * swap settings, both interfaces, both directions, and straight from the
 * face. The interrupt-modify write is the trace's positive control: it is
 * still the Hub's callback, so a trace that recorded nothing at all fails.
 */
static void test_memory_takes_no_callback(const void *data)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-hub-inbound-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "packet.trace", NULL);
    QTestState *qts = qtest_initf(MACHINE_SINGLE
                                  " -trace enable=sgi_hub_ii_packet_access,"
                                  "file=%s", path);
    unsigned int memory, interrupts;

    cards_ready(qts, 0, 0);
    for (unsigned int swap = 0; swap < 2; swap++) {
        card_swap(qts, 0, swap ? 0 : DEV_SWAP_DIRECT);
        g_assert_cmpuint(card_run(qts, 0, SLOT_NATIVE, DMA_GO_FILL,
                                  PCI_DIRECT64(TARGET + 3), 0, 4099, 1),
                         ==, RESULT_OK);
        g_assert_cmpuint(card_run(qts, 0, SLOT_NATIVE, DMA_GO_COPY,
                                  PCI_DIRECT64(TARGET + 0x2001),
                                  PCI_DIRECT64(SOURCE + 6), 1500, 0),
                         ==, RESULT_OK);
    }
    g_assert_cmpuint(card_run(qts, 0, SLOT_FACE, DMA_GO_FILL, TARGET + 5,
                              0, 4099, 1), ==, RESULT_OK);
    g_assert_cmpuint(card_run(qts, 0, SLOT_FACE, DMA_GO_COPY,
                              TARGET + 0x2001, SOURCE + 6, 1500, 0),
                     ==, RESULT_OK);
    qtest_writeq(qts, SOURCE, INT_MODIFY_SET | 52);
    g_assert_cmpuint(card_run(qts, 0, SLOT_FACE, DMA_GO_COPY,
                              XIO_INT_MODIFY(0), SOURCE, 8, 0), ==, RESULT_OK);
    qtest_quit(qts);

    count_packet_callbacks(path, &memory, &interrupts);
    g_test_message("memory callbacks %u, interrupt-modify callbacks %u",
                   memory, interrupts);
    g_assert_cmpuint(memory, ==, 0);
    g_assert_cmpuint(interrupts, ==, 1);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void renumber(QTestState *qts, unsigned int from, unsigned int to)
{
    qtest_writeq(qts, (uint64_t)from * NODE_STRIDE + HUB_LOCAL + NI_STATUS,
                 NI_REGION_SIZE | NI_NODE(to));
    g_assert_cmphex(qtest_readq(qts, (uint64_t)to * NODE_STRIDE +
                                HUB_LOCAL + NI_STATUS) & NI_NODE_MASK,
                    ==, NI_NODE(to));
}

/* No byte of [base, base + span) moved off the sentinel. */
static void assert_untouched(QTestState *qts, uint64_t base, size_t span)
{
    g_autofree uint8_t *seen = g_malloc(span);

    qtest_memread(qts, base, seen, span);
    for (size_t i = 0; i < span; i++) {
        g_assert_cmphex(seen[i], ==, 0xee);
    }
}

/*
 * On the dual, node 1's Bridge DMA lands on the node its address names by
 * programmed number: number 0 is node 0's RAM, node 1's own number is its
 * own, and after node 1 renumbers only the new number reaches it. Each
 * landing leaves the other node's copy of the span untouched.
 */
static void test_node_qualified_routing(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    const uint32_t sizes[] = { 3, 8, 61, 1500, 4100 };
    const uint64_t far = FAR_NUMBER * NODE_STRIDE;

    sn0_program_nasid(qts, 1);
    cards_ready(qts, 1, 0);
    for (unsigned int swap = 0; swap < 2; swap++) {
        card_swap(qts, 1, swap ? 0 : DEV_SWAP_DIRECT);
        for (unsigned int l = 0; l < G_N_ELEMENTS(sizes); l++) {
            uint64_t start = TARGET + 0xffb;
            uint64_t lo = start - GUARD, span = sizes[l] + 2 * GUARD;

            /* Number 0 from node 1: node 0's RAM. */
            qtest_memset(qts, NODE_STRIDE + lo, 0xee, span);
            check_fill(qts, 0, 1, SLOT_NATIVE, PCI_DIRECT64(0), start,
                       sizes[l], swap, 0x31 + l);
            assert_untouched(qts, NODE_STRIDE + lo, span);

            /* Number 1 from node 1: its own. */
            qtest_memset(qts, lo, 0xee, span);
            check_fill(qts, NODE_STRIDE, 1, SLOT_NATIVE,
                       PCI_DIRECT64(NODE_STRIDE), start, sizes[l], swap,
                       0x41 + l);
            assert_untouched(qts, lo, span);
        }
    }

    renumber(qts, 1, FAR_NUMBER);
    card_swap(qts, FAR_NUMBER, DEV_SWAP_DIRECT);
    check_fill(qts, far, FAR_NUMBER, SLOT_NATIVE, PCI_DIRECT64(far),
               TARGET + 0x2003, 1500, false, 0x61);
    /* The old number is nobody's now. */
    g_assert_cmpuint(card_run(qts, FAR_NUMBER, SLOT_NATIVE, DMA_GO_FILL,
                              PCI_DIRECT64(NODE_STRIDE + TARGET), 0, 64,
                              0), ==, RESULT_DECODE_ERROR);
    qtest_quit(qts);
}

/* What reset and migration must leave working: RAM, an interrupt, a refusal. */
static void check_window_works(QTestState *qts, uint8_t seed,
                               unsigned int level)
{
    const uint64_t pend = HUB_LOCAL + PI_INT_PEND0;

    check_fill(qts, 0, 0, SLOT_NATIVE, PCI_DIRECT64(0), TARGET + 0x5,
               1500, false, seed);
    check_fill(qts, 0, 0, SLOT_NATIVE, PCI_DIRECT64(0), TARGET + 0x1003,
               4100, false, seed + 1);
    check_fill(qts, 0, 0, SLOT_FACE, 0, TARGET + 0x3001, 61, false, seed + 2);
    g_assert_cmphex(qtest_readq(qts, pend) & (1ULL << level), ==, 0);
    qtest_writeq(qts, SOURCE, INT_MODIFY_SET | level);
    g_assert_cmpuint(card_run(qts, 0, SLOT_FACE, DMA_GO_COPY,
                              XIO_INT_MODIFY(0), SOURCE, 8, 0), ==, RESULT_OK);
    g_assert_cmphex(qtest_readq(qts, pend) & (1ULL << level), ==,
                    1ULL << level);
    g_assert_cmpuint(card_run(qts, 0, SLOT_FACE, DMA_GO_FILL, XIO_MEMORY_END,
                              0, 8, 0), ==, RESULT_ERROR);
}

static void test_reset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_SINGLE);

    cards_ready(qts, 0, DEV_SWAP_DIRECT);
    check_window_works(qts, 0x10, 53);
    qtest_system_reset(qts);
    cards_ready(qts, 0, DEV_SWAP_DIRECT);
    check_window_works(qts, 0x20, 53);
    qtest_quit(qts);
}

static void test_migration(const void *data)
{
    g_autofree char *tmpdir = g_dir_make_tmp("sn0-hub-inbound-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);
    g_autofree char *dest = g_strdup_printf(MACHINE_SINGLE " -incoming %s",
                                            uri);
    QTestState *from, *to;

    from = qtest_init(MACHINE_SINGLE " -S");
    cards_ready(from, 0, DEV_SWAP_DIRECT);
    check_window_works(from, 0x30, 54);
    to = qtest_init(dest);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    /* The cards' configuration came across; nothing is set up again. */
    check_window_works(to, 0x40, 55);
    qtest_quit(to);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/hub-inbound/bridge-dma-bytes",
                        NULL, test_bridge_dma_bytes);
    qtest_add_data_func("/sn0/hub-inbound/face-dma-bytes",
                        NULL, test_face_dma_bytes);
    qtest_add_data_func("/sn0/hub-inbound/decoded-ranges",
                        NULL, test_decoded_ranges);
    qtest_add_data_func("/sn0/hub-inbound/memory-takes-no-callback",
                        NULL, test_memory_takes_no_callback);
    qtest_add_data_func("/sn0/hub-inbound/node-qualified-routing",
                        NULL, test_node_qualified_routing);
    qtest_add_data_func("/sn0/hub-inbound/reset", NULL, test_reset);
    qtest_add_data_func("/sn0/hub-inbound/migration", NULL, test_migration);

    return g_test_run();
}
