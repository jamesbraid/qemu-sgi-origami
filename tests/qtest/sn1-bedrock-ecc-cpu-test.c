/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock ECC stores as issued by the Origin 300 processor. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define PROM_SIZE 0x1686a8
#define SPACE_HUB UINT64_C(0x10000000000)
#define HUB_ECC (SPACE_HUB + UINT64_C(0x140000000))
#define SPACE_XTALK UINT64_C(0x20000000000)
#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
#define BANK0_512M UINT64_C(0x41)

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
    qtest_writeq(s, BEDROCK_NODE0 + MD_MEMORY_CONFIG, BANK0_512M);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-ecc/cpu-halfword-stores",
                   test_cpu_halfword_stores);
    return g_test_run();
}
