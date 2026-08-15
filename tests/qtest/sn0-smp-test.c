/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercise the synthesized IP27 PROM's launch_slave vector with code that is
 * made at test time.  qtest normally runs with the qtest accelerator, so it
 * cannot prove a secondary CPU executes; this test selects TCG explicitly.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define ELF_CODE_OFFSET      0x1000
#define ELF_CODE_SIZE        0x100
#define KERNEL_ENTRY         0xffffffff80010000ULL
#define SECONDARY_ENTRY      (KERNEL_ENTRY + 0x80)
#define RESULT_PHYS          0x2000
#define RESULT_KSEG          (0xffffffff80000000ULL | RESULT_PHYS)
#define RESULT_MAGIC         0x1eedbeefULL
#define PRIMARY_MAGIC        0x5a5aa5a5ULL
#define PRIMARY_RESULT       (RESULT_PHYS + 40)
#define LAUNCH_SLAVE         0xffffffffbfc00038ULL
#define CALL_PARM            0x13579bdfULL
#define STACK_VALUE          0x10203040ULL
#define GP_VALUE             0x2468ace0ULL

static void put_instruction(uint8_t *image, unsigned int offset,
                            uint32_t instruction)
{
    stl_be_p(image + ELF_CODE_OFFSET + offset, instruction);
}

static void write_launch_test_elf(int fd)
{
    uint8_t image[ELF_CODE_OFFSET + ELF_CODE_SIZE] = { };
    ssize_t written;

    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 2;             /* ELFCLASS64 */
    image[5] = 2;             /* ELFDATA2MSB */
    image[6] = 1;             /* EV_CURRENT */
    stw_be_p(image + 0x10, 2); /* ET_EXEC */
    stw_be_p(image + 0x12, 8); /* EM_MIPS */
    stl_be_p(image + 0x14, 1); /* EV_CURRENT */
    stq_be_p(image + 0x18, KERNEL_ENTRY);
    stq_be_p(image + 0x20, 0x40);
    stw_be_p(image + 0x34, 0x40);
    stw_be_p(image + 0x36, 0x38);
    stw_be_p(image + 0x38, 1);

    stl_be_p(image + 0x40, 1); /* PT_LOAD */
    stl_be_p(image + 0x44, 5); /* PF_R | PF_X */
    stq_be_p(image + 0x48, ELF_CODE_OFFSET);
    stq_be_p(image + 0x50, KERNEL_ENTRY);
    stq_be_p(image + 0x58, KERNEL_ENTRY);
    stq_be_p(image + 0x60, ELF_CODE_SIZE);
    stq_be_p(image + 0x68, ELF_CODE_SIZE);
    stq_be_p(image + 0x70, ELF_CODE_OFFSET);

    /* CPU 0 calls launch_slave(0, 1, secondary, parm, sp, gp). */
    put_instruction(image, 0x00, 0x24040000); /* addiu a0, zero, 0 */
    put_instruction(image, 0x04, 0x24050001); /* addiu a1, zero, 1 */
    put_instruction(image, 0x08, 0x3c060000 |
                    ((uint32_t)(SECONDARY_ENTRY >> 16) & 0xffff));
    put_instruction(image, 0x0c, 0x34c60000 | (SECONDARY_ENTRY & 0xffff));
    put_instruction(image, 0x10, 0x3c071357); /* lui a3, 0x1357 */
    put_instruction(image, 0x14, 0x34e79bdf); /* ori a3, a3, 0x9bdf */
    put_instruction(image, 0x18, 0x3c081020); /* lui t0, 0x1020 */
    put_instruction(image, 0x1c, 0x35083040); /* ori t0, t0, 0x3040 */
    put_instruction(image, 0x20, 0x3c092468); /* lui t1, 0x2468 */
    put_instruction(image, 0x24, 0x3529ace0); /* ori t1, t1, 0xace0 */
    put_instruction(image, 0x28, 0x3c190000 |
                    ((uint32_t)(LAUNCH_SLAVE >> 16) & 0xffff));
    put_instruction(image, 0x2c, 0x37390000 | (LAUNCH_SLAVE & 0xffff));
    put_instruction(image, 0x30, 0x0320f809); /* jalr ra, t9 */
    put_instruction(image, 0x34, 0x00000000); /* nop */
    put_instruction(image, 0x38, 0x3c088000); /* lui t0, 0x8000 */
    put_instruction(image, 0x3c, 0x35082028); /* ori t0, t0, 0x2028 */
    put_instruction(image, 0x40, 0x3c095a5a); /* lui t1, 0x5a5a */
    put_instruction(image, 0x44, 0x3529a5a5); /* ori t1, t1, 0xa5a5 */
    put_instruction(image, 0x48, 0xfd090000); /* sd t1, 0(t0) */
    put_instruction(image, 0x4c, 0x42000020); /* wait */

    /* CPU 1 records the real entry arguments and waits. */
    put_instruction(image, 0x80, 0x3c080000 |
                    ((uint32_t)(RESULT_KSEG >> 16) & 0xffff));
    put_instruction(image, 0x84, 0x35080000 | (RESULT_KSEG & 0xffff));
    put_instruction(image, 0x88, 0xfd040000); /* sd a0, 0(t0) */
    put_instruction(image, 0x8c, 0xfd070008); /* sd a3, 8(t0) */
    put_instruction(image, 0x90, 0xfd1c0010); /* sd gp, 16(t0) */
    put_instruction(image, 0x94, 0xfd1d0018); /* sd sp, 24(t0) */
    put_instruction(image, 0x98, 0x3c091eed); /* lui t1, 0x1eed */
    put_instruction(image, 0x9c, 0x3529beef); /* ori t1, t1, 0xbeef */
    put_instruction(image, 0xa0, 0xfd090020); /* sd t1, 32(t0) */
    put_instruction(image, 0xa4, 0x42000020); /* wait */

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void test_secondary_reaches_firmware_launch_entry(const void *data)
{
    g_autofree char *kernel = NULL;
    QTestState *qts;
    int fd;
    unsigned int tries;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-launch-XXXXXX", &kernel, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_launch_test_elf(fd);
    close(fd);

    qts = qtest_initf("-M origin200 -m 64M -smp 2 -kernel %s "
                      "-accel tcg,thread=multi -no-reboot -no-shutdown",
                      kernel);
    unlink(kernel);

    for (tries = 0; tries < 1000; tries++) {
        if (qtest_readq(qts, PRIMARY_RESULT) == PRIMARY_MAGIC) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(tries, <, 1000);

    for (tries = 0; tries < 1000; tries++) {
        if (qtest_readq(qts, RESULT_PHYS + 32) == RESULT_MAGIC) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmpuint(tries, <, 1000);
    g_assert_cmphex(qtest_readq(qts, RESULT_PHYS), ==, CALL_PARM);
    g_assert_cmphex(qtest_readq(qts, RESULT_PHYS + 8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, RESULT_PHYS + 16), ==, GP_VALUE);
    g_assert_cmphex(qtest_readq(qts, RESULT_PHYS + 24), ==, STACK_VALUE);
    g_assert_cmphex(qtest_readq(qts, RESULT_PHYS + 32), ==, RESULT_MAGIC);
    g_assert_cmphex(qtest_readq(qts, PRIMARY_RESULT), ==, PRIMARY_MAGIC);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/smp/secondary-reaches-firmware-launch-entry",
                        NULL, test_secondary_reaches_firmware_launch_entry);
    return g_test_run();
}
