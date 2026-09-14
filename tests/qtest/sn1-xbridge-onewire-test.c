/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bridge-half board-identity NIC (BRIDGE_NIC, offset 0xb0), which the
 * stock IP35 PROM and the loaded BASEIO PROM both read while attaching the
 * IO-8's bridge object -- see the widget-15 amendment in
 * docs/datasheets/xbridge.md, "Stock-PROM IO-8 identity access". Standalone
 * head; the fixture does not represent an Origin 300 (see sn1-xbridge-test.c).
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define BASE 0x10000000ULL
#define HEAD "/machine/peripheral/head"
#define NO_BOARD "/machine/peripheral/no-board-head"

#define BRIDGE_NIC 0xb0

#define NIC_LEVEL (1u << 0)
#define NIC_DONE  (1u << 1)
#define NIC_PULSE(n)  ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)

/* The placeholder identity docs/datasheets/xbridge.md and hw/mips/sn1.c cite. */
#define PLACEHOLDER_PART "NOT-OBSERVED"
#define PLACEHOLDER_FAMILY 0x0b
#define PLACEHOLDER_SERIAL 0x060504030201ULL

static QTestState *head_start(const char *board_part)
{
    g_autofree char *args = board_part ?
        g_strdup_printf("-M none -cpu R10000 -S "
                        "-device sgi-xbridge-head,id=head,board-part=%s",
                        board_part) :
        g_strdup("-M none -cpu R10000 -S "
                "-device sgi-xbridge-head,id=no-board-head");
    QTestState *qts = qtest_init(args);

    qtest_sysbus_mmio_map(qts, board_part ? HEAD : NO_BOARD, 0, BASE);
    return qts;
}

static uint32_t reg_read(QTestState *qts, unsigned off)
{
    return qtest_readl(qts, BASE + off + 4);
}

static void reg_write(QTestState *qts, unsigned off, uint32_t value)
{
    qtest_writel(qts, BASE + off + 4, value);
}

/* One bit slot, mainline's widths: a long reset, a short write-one/read,
 * a middling write-zero. */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    reg_write(qts, BRIDGE_NIC, NIC_PULSE(pulse) | NIC_SAMPLE(sample));

    uint32_t nic = reg_read(qts, BRIDGE_NIC);

    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q)      nic_slot((q), 520, 65)
#define NIC_READ(q)       nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

static uint16_t nic_crc(uint16_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}
#define NIC_CRC_RESIDUE 0xb001

static uint8_t nic_crc8(uint8_t crc, uint64_t rom)
{
    unsigned int byte, bit;

    for (byte = 0; byte < 8; byte++) {
        crc ^= rom >> (byte * 8);
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/* Drive a search and return the identity the part hands back. */
static uint64_t nic_search_for_identity(QTestState *qts)
{
    uint64_t rom = 0;
    unsigned int i;

    nic_write_byte(qts, 0xf0);
    for (i = 0; i < 64; i++) {
        bool bit = NIC_READ(qts);
        bool complement = NIC_READ(qts);

        g_assert_cmpint(complement, ==, !bit);
        if (bit) {
            rom |= 1ULL << i;
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
    return rom;
}

/*
 * The scanning reader: find "name" within the first `len` bytes and copy
 * everything up to the next ';' delimiter, exactly -- unlike the fixed-span
 * positional reader (sn0-bridge-test.c's partnum_from_page(), not reused
 * here), this never collects the delimiter itself or reads past what it
 * was told to scan.
 */
static bool page_field(const uint8_t *page, unsigned int len,
                       const char *name, char *out)
{
    unsigned int pos = 0, n = strlen(name);

    while (pos < len) {
        unsigned int i;

        if (pos + n <= len && memcmp(&page[pos], name, n) == 0) {
            unsigned int j = 0;

            for (i = pos + n; i < len && page[i] != ';'; i++) {
                out[j++] = page[i];
            }
            out[j] = 0;
            return true;
        }
        for (i = pos; i < len && page[i] != ';'; i++) {
            /* skip to the next delimiter */
        }
        if (i >= len) {
            return false;
        }
        pos = i + 1;
    }
    return false;
}

/*
 * The identity the search returns and the part-number page it reads back:
 * the discriminator the datasheet's checklist asks for, exercised on this
 * head's own offset rather than widget 0's crossbar NIC (a separate,
 * unmodelled register the stock PROM never uses for the IO-8).
 */
static void test_placeholder_identity_reads_back(void)
{
    QTestState *qts = head_start(PLACEHOLDER_PART);
    uint64_t rom;
    uint8_t exchange[6];
    uint8_t page[34];
    char part[13];
    unsigned int i;

    g_assert_false(NIC_RESET(qts));         /* pulled low: the part answers */

    rom = nic_search_for_identity(qts);
    g_assert_cmphex(rom & 0xff, ==, PLACEHOLDER_FAMILY);
    g_assert_cmphex((rom >> 8) & 0xffffffffffffULL, ==, PLACEHOLDER_SERIAL);
    g_assert_cmphex(nic_crc8(0, rom), ==, 0);

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0xcc);              /* one part on the bus: skip ROM */

    exchange[0] = 0xa5;                     /* read memory */
    exchange[1] = 0x00;
    exchange[2] = 0x00;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }
    for (i = 3; i < 6; i++) {
        exchange[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, exchange, 6), ==, NIC_CRC_RESIDUE);

    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    g_assert_cmpmem(&page[6], 5, "Part:", 5);
    g_assert_cmpint(page[11 + 12], ==, ';');
    /* Scan the field by name over page 1 alone (30 bytes, short of its own
     * CRC), exactly as sn0-bridge-test.c's page_field() does: never
     * collecting the ';' delimiter, unlike the fixed-span positional
     * reader that field also has to satisfy on a real driver. */
    g_assert_true(page_field(page, 30, "Part:", part));
    g_assert_cmpstr(part, ==, PLACEHOLDER_PART);

    qtest_quit(qts);
}

/*
 * A head a machine gives no board-part keeps BRIDGE_NIC exactly as inert as
 * the reviewed rev-A contract recorded: no board behind that widget (like
 * the empty option slot at widget 14) must not gain a responder.
 */
static void test_no_board_stays_inert(void)
{
    QTestState *qts = head_start(NULL);

    g_assert_cmphex(reg_read(qts, BRIDGE_NIC), ==, 0);
    reg_write(qts, BRIDGE_NIC, NIC_PULSE(520) | NIC_SAMPLE(65));
    /* No onewire_bus for this head: the write is dropped, same as before. */
    g_assert_cmphex(reg_read(qts, BRIDGE_NIC), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn1/xbridge/onewire/placeholder-identity-reads-back",
                   test_placeholder_identity_reads_back);
    qtest_add_func("/sn1/xbridge/onewire/no-board-stays-inert",
                   test_no_board_stays_inert);
    return g_test_run();
}
