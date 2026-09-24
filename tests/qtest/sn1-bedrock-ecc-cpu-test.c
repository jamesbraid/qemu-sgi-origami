/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock ECC stores as issued by the Origin 300 processor. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define HUB_ECC (SPACE_HUB + UINT64_C(0x140000000))
#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
/*
 * Origin 300's installed memory occupies locational banks 2 and 3, not
 * bank 0: map software bank 0 onto them so the guest's low-memory
 * coordination state (its completion flag included) lands in backed RAM
 * instead of an I/O hole.
 */
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

static void emit(uint8_t *image, unsigned *offset, uint32_t instruction)
{
    stl_be_p(image + *offset, instruction);
    *offset += 4;
}

static void emit_load64(uint8_t *image, unsigned *offset, unsigned reg,
                        uint64_t value)
{
    emit(image, offset, 0x34000000 | reg << 16 | (value >> 48));
    emit(image, offset, reg << 16 | reg << 11 | 16 << 6 | 0x38);
    emit(image, offset, 0x34000000 | reg << 21 | reg << 16 |
         ((value >> 32) & 0xffff));
    emit(image, offset, reg << 16 | reg << 11 | 16 << 6 | 0x38);
    emit(image, offset, 0x34000000 | reg << 21 | reg << 16 |
         ((value >> 16) & 0xffff));
    emit(image, offset, reg << 16 | reg << 11 | 16 << 6 | 0x38);
    emit(image, offset, 0x34000000 | reg << 21 | reg << 16 |
         (value & 0xffff));
}

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    unsigned offset = 0x380;
    int fd;

    stl_be_p(image, 0x100000ff); /* beq zero, zero, 0x400 */

    emit_load64(image, &offset, 27, UINT64_C(0x9000000000000110));
    emit(image, &offset, 0x401a6800); /* mfc0 k0, Cause */
    emit(image, &offset, 0xaf7a0000); /* sw k0, 0(k1) */
    emit(image, &offset, 0x401a7000); /* mfc0 k0, EPC */
    emit(image, &offset, 0x275a0004); /* addiu k0, k0, 4 */
    emit(image, &offset, 0x409a7000); /* mtc0 k0, EPC */
    emit(image, &offset, 0x42000018); /* eret */
    emit(image, &offset, 0);          /* nop */

    offset = 0x400;
    emit(image, &offset, 0x400c6000); /* mfc0 t4, Status */
    emit(image, &offset, 0x358c0080); /* ori t4, t4, Status.KX */
    emit(image, &offset, 0x240dfffb); /* addiu t5, zero, ~Status.ERL */
    emit(image, &offset, 0x018d6024); /* and t4, t4, t5 */
    emit(image, &offset, 0x408c6000); /* mtc0 t4, Status */
    emit(image, &offset, 0x000000c0); /* ehb */

    emit_load64(image, &offset, 8, UINT64_C(0x9000000140000000));
    emit_load64(image, &offset, 1, UINT64_C(0x1111222233334444));
    emit(image, &offset, 0xa5010006); /* sh at, 6(t0) */
    emit_load64(image, &offset, 1, UINT64_C(0x5555666677778888));
    emit(image, &offset, 0xa5010004); /* sh at, 4(t0) */
    emit_load64(image, &offset, 1, UINT64_C(0x9999aaaabbbbcccc));
    emit(image, &offset, 0xa5010002); /* sh at, 2(t0) */

    emit_load64(image, &offset, 8, UINT64_C(0x9000000000000100));
    emit_load64(image, &offset, 1, UINT64_C(0x5555666677770000));
    emit(image, &offset, 0xa5010000); /* sh at, 0(t0) */
    emit_load64(image, &offset, 8, UINT64_C(1));
    emit(image, &offset, 0xa5010000); /* unaligned sh at, 0(t0) */
    emit_load64(image, &offset, 8, UINT64_C(0x9000000000000100));
    emit(image, &offset, 0x340b0001); /* ori t3, zero, 1 */
    emit(image, &offset, 0xfd0b0008); /* sd t3, 8(t0) */
    emit(image, &offset, 0x1000ffff); /* beq zero, zero, self */
    emit(image, &offset, 0);          /* nop */

    fd = g_file_open_tmp("sn1-bedrock-ecc-cpu-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_cpu_halfword_stores(void)
{
    g_autofree char *prom = create_prom();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=single -no-shutdown -S", quoted);
    qtest_writeq(s, BEDROCK_NODE0 + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned tries = 0; tries < 200 && !qtest_readq(s, 0x108); tries++) {
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readq(s, 0x108), ==, 1);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0), ==, 0);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 2), ==, 0xaaaa);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 4), ==, 0x5555);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 6), ==, 0x4444);
    g_assert_cmphex(qtest_readw(s, 0x100), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x110) & 0x7c, ==, 5 << 2);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The stock PROM's walking-address memory test reads a positioned check word
 * back from the very top of the fitted bank. Repeat all four residues at the
 * last fitted 64-byte line of installed memory. Its residue 0x30 (ECC
 * carrier offset 0) selects source bits 31:16, the slice the first-line
 * cases above never exercise.
 *
 * Origin 300's installed memory occupies locational banks 2 and 3, 256 MiB
 * each (see MD_INSTALLED_SELECT2 above), not one 512 MiB bank at slot 0.
 * The last fitted line sits in the second (higher) of those two banks:
 * software bank 1, which the 2-selector routes to locational bank 3, at
 * device offset 0x9fffff8 -- that bank's own last 64-byte line, 0x1fffff8,
 * past its dense base of 0x2000000 (bank 2's own 0x2000000 ECC bytes).
 */
static char *create_prom_top_line(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    unsigned offset = 0x380;
    int fd;

    stl_be_p(image, 0x100000ff); /* beq zero, zero, 0x400 */

    emit_load64(image, &offset, 27, UINT64_C(0x9000000000000110));
    emit(image, &offset, 0x401a6800); /* mfc0 k0, Cause */
    emit(image, &offset, 0xaf7a0000); /* sw k0, 0(k1) */
    emit(image, &offset, 0x401a7000); /* mfc0 k0, EPC */
    emit(image, &offset, 0x275a0004); /* addiu k0, k0, 4 */
    emit(image, &offset, 0x409a7000); /* mtc0 k0, EPC */
    emit(image, &offset, 0x42000018); /* eret */
    emit(image, &offset, 0);          /* nop */

    offset = 0x400;
    emit(image, &offset, 0x400c6000); /* mfc0 t4, Status */
    emit(image, &offset, 0x358c0080); /* ori t4, t4, Status.KX */
    emit(image, &offset, 0x240dfffb); /* addiu t5, zero, ~Status.ERL */
    emit(image, &offset, 0x018d6024); /* and t4, t4, t5 */
    emit(image, &offset, 0x408c6000); /* mtc0 t4, Status */
    emit(image, &offset, 0x000000c0); /* ehb */

    /* t0 = xkphys view of the last fitted ECC carrier word (device 0x9fffff8). */
    emit_load64(image, &offset, 8, UINT64_C(0x9000000149fffff8));
    emit_load64(image, &offset, 1, UINT64_C(0x1111222233334444));
    emit(image, &offset, 0xa5010006); /* sh at, 6(t0)  residue 0x00 -> 0x4444 */
    emit_load64(image, &offset, 1, UINT64_C(0x5555666677778888));
    emit(image, &offset, 0xa5010004); /* sh at, 4(t0)  residue 0x10 -> 0x5555 */
    emit_load64(image, &offset, 1, UINT64_C(0x9999aaaabbbbcccc));
    emit(image, &offset, 0xa5010002); /* sh at, 2(t0)  residue 0x20 -> 0xaaaa */
    emit_load64(image, &offset, 1, UINT64_C(0xddddeeee13572468));
    emit(image, &offset, 0xa5010000); /* sh at, 0(t0)  residue 0x30 -> 0x1357 */

    emit_load64(image, &offset, 8, UINT64_C(0x9000000000000100));
    emit(image, &offset, 0x340b0001); /* ori t3, zero, 1 */
    emit(image, &offset, 0xfd0b0008); /* sd t3, 8(t0) */
    emit(image, &offset, 0x1000ffff); /* beq zero, zero, self */
    emit(image, &offset, 0);          /* nop */

    fd = g_file_open_tmp("sn1-bedrock-ecc-cpu-top-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_cpu_halfword_stores_top_line(void)
{
    g_autofree char *prom = create_prom_top_line();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=single -no-shutdown -S", quoted);
    qtest_writeq(s, BEDROCK_NODE0 + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned tries = 0; tries < 200 && !qtest_readq(s, 0x108); tries++) {
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readq(s, 0x108), ==, 1);

    /* Each residue keeps its own positioned check word at the top fitted line. */
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffffe), ==, 0x4444);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffffc), ==, 0x5555);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffffa), ==, 0xaaaa);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffff8), ==, 0x1357);
    /*
     * Big-endian byte view of the residue 0x30 word: the halfword carrier
     * keeps the even doubleword's byte (0x9fffffe, at byte carrier
     * 0x9fffffe ^ 7) in its low half.
     */
    g_assert_cmphex(qtest_readb(s, HUB_ECC + 0x9fffff8), ==, 0x13);
    g_assert_cmphex(qtest_readb(s, HUB_ECC + 0x9fffff9), ==, 0x57);
    /* The adjacent lower carrier word was never written. */
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffff0), ==, 0);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * A doubleword whose check byte names no single bit, read by the processor
 * two ways. The uncached reference (xkphys attribute 3, cache algorithm 2)
 * fails as a bus error: the R10000 family posts one when a doubleword
 * response carries the uncorrectable-error indication, and the Origin 300
 * memory diagnostic's SECDED verification reads each uncorrectable
 * syndrome that way under a bus-error handler. The cached reference (cache
 * algorithm 5) is a block read, which the processor answers with a Cache
 * Error exception the model does not raise; it completes with the word as
 * stored. Both latch the error record and raise the memory-uncorrectable
 * error-interrupt pending bit on both processor interfaces.
 *
 * The handler at the general exception vector records Cause where k1
 * points, which the program moves between the two probes, then skips the
 * faulting load. Each probe's destination register starts as a sentinel
 * that only a completed load replaces.
 */
#define UCE_CACHED_WORD UINT64_C(0x200)
#define UCE_UNCACHED_WORD UINT64_C(0x210)
#define UCE_CACHED_CAUSE UINT64_C(0x300)
#define UCE_CACHED_VALUE UINT64_C(0x308)
#define UCE_UNCACHED_CAUSE UINT64_C(0x310)
#define UCE_UNCACHED_VALUE UINT64_C(0x318)
#define UCE_DONE UINT64_C(0x320)
#define UCE_CACHED_SENTINEL UINT64_C(0x1111111111111111)
#define UCE_UNCACHED_SENTINEL UINT64_C(0x2222222222222222)
#define XKPHYS_CACHED UINT64_C(0xa800000000000000)
#define XKPHYS_UNCACHED UINT64_C(0x9600000000000000)
#define HUB_LOCAL_MD (SPACE_HUB + UINT64_C(0x10000000) + UINT64_C(0x780000))
#define MD_MEMORY_ERROR (HUB_LOCAL_MD + 0x60010)
#define MEMORY_ERROR_READ_UCE_VALID (UINT64_C(1) << 60)
#define MEMORY_ERROR_SYNDROME_MASK (UINT64_C(0xff) << 40)
#define MEMORY_ERROR_SYNDROME(_s) ((uint64_t)(_s) << 40)
#define PI_ERROR_PENDING (BEDROCK_NODE0 + 0x400)
#define PI1_OFFSET UINT64_C(0x200000)
#define MEMORY_UNCORRECTABLE (UINT64_C(1) << 24)
#define CAUSE_EXCCODE_MASK 0x7c
#define CAUSE_EXCCODE_DBE (7 << 2)

static void emit_probe(uint8_t *image, unsigned *offset, uint64_t cause_slot,
                       uint64_t word, uint64_t sentinel, uint64_t value_slot)
{
    emit_load64(image, offset, 27, XKPHYS_UNCACHED | cause_slot);
    emit_load64(image, offset, 8, word);
    emit_load64(image, offset, 9, sentinel);
    emit(image, offset, 0xdd090000); /* ld t1, 0(t0) */
    emit_load64(image, offset, 10, XKPHYS_UNCACHED | value_slot);
    emit(image, offset, 0xfd490000); /* sd t1, 0(t2) */
}

static char *create_prom_uncorrectable_reads(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    unsigned offset = 0x380;
    int fd;

    stl_be_p(image, 0x100000ff); /* beq zero, zero, 0x400 */

    emit(image, &offset, 0x401a6800); /* mfc0 k0, Cause */
    emit(image, &offset, 0xaf7a0000); /* sw k0, 0(k1) */
    emit(image, &offset, 0x401a7000); /* mfc0 k0, EPC */
    emit(image, &offset, 0x275a0004); /* addiu k0, k0, 4 */
    emit(image, &offset, 0x409a7000); /* mtc0 k0, EPC */
    emit(image, &offset, 0x42000018); /* eret */
    emit(image, &offset, 0);          /* nop */

    offset = 0x400;
    emit(image, &offset, 0x400c6000); /* mfc0 t4, Status */
    emit(image, &offset, 0x358c0080); /* ori t4, t4, Status.KX */
    emit(image, &offset, 0x240dfffb); /* addiu t5, zero, ~Status.ERL */
    emit(image, &offset, 0x018d6024); /* and t4, t4, t5 */
    emit(image, &offset, 0x408c6000); /* mtc0 t4, Status */
    emit(image, &offset, 0x000000c0); /* ehb */

    emit_probe(image, &offset, UCE_CACHED_CAUSE,
               XKPHYS_CACHED | UCE_CACHED_WORD, UCE_CACHED_SENTINEL,
               UCE_CACHED_VALUE);
    emit_probe(image, &offset, UCE_UNCACHED_CAUSE,
               XKPHYS_UNCACHED | UCE_UNCACHED_WORD, UCE_UNCACHED_SENTINEL,
               UCE_UNCACHED_VALUE);

    emit_load64(image, &offset, 8, XKPHYS_UNCACHED | UCE_DONE);
    emit(image, &offset, 0x340b0001); /* ori t3, zero, 1 */
    emit(image, &offset, 0xfd0b0000); /* sd t3, 0(t0) */
    emit(image, &offset, 0x1000ffff); /* beq zero, zero, self */
    emit(image, &offset, 0);          /* nop */

    fd = g_file_open_tmp("sn1-bedrock-ecc-cpu-uce-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_cpu_uncorrectable_reads(void)
{
    const uint64_t cached_data = UINT64_C(0x0f0f0f0f0f0f0f0f);
    const uint64_t uncached_data = UINT64_C(0xfedcba9876543210);
    const uint64_t cached_carrier = HUB_ECC + ((UCE_CACHED_WORD / 8) ^ 7);
    const uint64_t uncached_carrier = HUB_ECC + ((UCE_UNCACHED_WORD / 8) ^ 7);
    g_autofree char *prom = create_prom_uncorrectable_reads();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *s;
    uint8_t cached_byte;
    uint8_t uncached_byte;
    uint64_t record;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=single -no-shutdown -S", quoted);
    qtest_writeq(s, BEDROCK_NODE0 + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    qtest_writeq(s, UCE_CACHED_WORD, cached_data);
    qtest_writeq(s, UCE_UNCACHED_WORD, uncached_data);
    /*
     * Syndrome 0x03, check bits 0 and 1: the first uncorrectable syndrome
     * the memory diagnostic sweeps, forced over the code's own byte, which
     * the carrier reads back for a word it has not overridden.
     */
    cached_byte = qtest_readb(s, cached_carrier) ^ 0x03;
    uncached_byte = qtest_readb(s, uncached_carrier) ^ 0x03;
    qtest_writeb(s, cached_carrier, cached_byte);
    qtest_writeb(s, uncached_carrier, uncached_byte);
    g_assert_cmphex(qtest_readq(s, MD_MEMORY_ERROR), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI_ERROR_PENDING) & MEMORY_UNCORRECTABLE,
                    ==, 0);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned tries = 0; tries < 200 && !qtest_readq(s, UCE_DONE);
         tries++) {
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readq(s, UCE_DONE), ==, 1);

    /* The cached load completed with the word as stored, no exception. */
    g_assert_cmphex(qtest_readl(s, UCE_CACHED_CAUSE) & CAUSE_EXCCODE_MASK,
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, UCE_CACHED_VALUE), ==, cached_data);
    /* The uncached load took a bus error and left its register alone. */
    g_assert_cmphex(qtest_readl(s, UCE_UNCACHED_CAUSE) & CAUSE_EXCCODE_MASK,
                    ==, CAUSE_EXCCODE_DBE);
    g_assert_cmphex(qtest_readq(s, UCE_UNCACHED_VALUE), ==,
                    UCE_UNCACHED_SENTINEL);
    /* Neither read scrubbed its word: the carriers still hold the bytes. */
    g_assert_cmphex(qtest_readb(s, cached_carrier), ==, cached_byte);
    g_assert_cmphex(qtest_readb(s, uncached_carrier), ==, uncached_byte);
    /*
     * Both reads latched: the record names the first, with syndrome 0x03,
     * and the memory-uncorrectable bit is pending on both interfaces.
     */
    record = qtest_readq(s, MD_MEMORY_ERROR);
    g_assert_cmphex(record & (MEMORY_ERROR_READ_UCE_VALID |
                              MEMORY_ERROR_SYNDROME_MASK), ==,
                    MEMORY_ERROR_READ_UCE_VALID | MEMORY_ERROR_SYNDROME(0x03));
    g_assert_cmphex(qtest_readq(s, PI_ERROR_PENDING) & MEMORY_UNCORRECTABLE,
                    ==, MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, PI_ERROR_PENDING + PI1_OFFSET) &
                    MEMORY_UNCORRECTABLE, ==, MEMORY_UNCORRECTABLE);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-ecc/cpu-halfword-stores",
                   test_cpu_halfword_stores);
    qtest_add_func("sn1/bedrock-ecc/cpu-halfword-stores-top-line",
                   test_cpu_halfword_stores_top_line);
    qtest_add_func("sn1/bedrock-ecc/cpu-uncorrectable-reads",
                   test_cpu_uncorrectable_reads);
    return g_test_run();
}
