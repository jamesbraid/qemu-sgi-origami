/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercise the cross-node RUNTIME contract of the four-CPU, two-node Origin 200
 * under TCG.  sn0-firmware-test's test_dual_node_inventory verifies the two-node
 * KLCONFIG *structure* statically; this proves the second node actually
 * EXECUTES.  It asserts a deterministic contract only -- the low-probability
 * init race tracked separately is the soak's job and is NOT reachable here.
 *
 * Cross-node facts (address-map datasheet + architecture contract, confirmed by
 * arch-eng; NOT from vendor firmware):
 *  - launch_slave (0xffffffffbfc00038) is LAUNCH_SLAVE(nasid, slice, entry,
 *    parm, sp=gpr8, gp=gpr9): a0 = NASID, a1 = SLICE WITHIN THAT NODE (< 2),
 *    NOT the global CPU number.  So node-1's CPUs are launched with a0 = 1 and
 *    a1 = 0 (CPU 2) / a1 = 1 (CPU 3).  A bad (nasid, slice) returns ARCS_EINVAL
 *    in v0 and logs LOG_GUEST_ERROR (silent without -d guest_errors).  The
 *    target arrives with k1 = entry, a0 = parm, sp/gp as passed, everything
 *    else RESET -- so Status.KX is clear (matters for a follow-up point's
 *    xkphys loads, not for this one).
 *  - The node-0-absolute uncached window is executable and reaches node-0 RAM;
 *    launch to the kseg1 form 0xffffffffa0010080 (no KX needed) to fetch node-0's
 *    ELF at physical 0x10080.
 *  - Under synthesized firmware + -kernel, a node-1 CPU's NASID-less addresses
 *    (kseg0, NASID-0 xkphys) ALIAS TO NODE 0 by design, so a node-1 CPU writing
 *    kseg0 0x2010 lands in node-0 memory, which the qtest reads at node-0
 *    physical 0x2010.  (Naming node-1 memory needs a NASID-qualified form,
 *    0x9600000100000000+phys; a follow-up point uses that.)
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define ELF_CODE_OFFSET      0x1000
#define ELF_CODE_SIZE        0x100
#define KERNEL_ENTRY         0xffffffff80010000ULL

/* Node-1 CPUs fetch through the node-0-absolute kseg1 aliases of their code. */
#define CPU2_ENTRY_HI        0xa001
#define CPU2_ENTRY_LO        0x0080   /* phys 0x10080 */
#define CPU3_ENTRY_HI        0xa001
#define CPU3_ENTRY_LO        0x00c0   /* phys 0x100c0 */

#define LAUNCH_SLAVE         0xffffffffbfc00038ULL

/* Result slots in node-0 physical memory (what qtest_readq reads). */
#define PRIMARY_RESULT       0x2000
#define CPU2_MARK_ADDR       0x2010
#define CPU3_MARK_ADDR       0x2018

#define PRIMARY_MAGIC        0x5a5aa5a5ULL
#define CPU2_MARK            0x00020002ULL
#define CPU3_MARK            0x00030003ULL

static void put_instruction(uint8_t *image, unsigned int offset,
                            uint32_t instruction)
{
    stl_be_p(image + ELF_CODE_OFFSET + offset, instruction);
}

static void write_cross_node_elf(int fd)
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

    /*
     * CPU 0 launches node-1's two slices, then records PRIMARY_MAGIC and waits.
     * launch_slave(nasid=1, slice, entry, parm=0, sp=t0, gp=t1) returns to the
     * caller, so it is called twice with all args reloaded.
     */
    /* launch CPU 2 = node 1, slice 0 */
    put_instruction(image, 0x00, 0x24040001); /* addiu a0, zero, 1  (NASID 1) */
    put_instruction(image, 0x04, 0x24050000); /* addiu a1, zero, 0  (slice 0) */
    put_instruction(image, 0x08, 0x3c060000 | CPU2_ENTRY_HI); /* lui a2 */
    put_instruction(image, 0x0c, 0x34c60000 | CPU2_ENTRY_LO); /* ori a2 */
    put_instruction(image, 0x10, 0x24070000); /* addiu a3, zero, 0 (parm) */
    put_instruction(image, 0x14, 0x3c088000); /* lui t0, 0x8000 */
    put_instruction(image, 0x18, 0x35087ff0); /* ori t0, t0, 0x7ff0 (sp) */
    put_instruction(image, 0x1c, 0x3c098000); /* lui t1, 0x8000 */
    put_instruction(image, 0x20, 0x35291000); /* ori t1, t1, 0x1000 (gp) */
    put_instruction(image, 0x24, 0x3c190000 |
                    ((uint32_t)(LAUNCH_SLAVE >> 16) & 0xffff)); /* lui t9 */
    put_instruction(image, 0x28, 0x37390000 | (LAUNCH_SLAVE & 0xffff)); /* ori t9 */
    put_instruction(image, 0x2c, 0x0320f809); /* jalr ra, t9 */
    put_instruction(image, 0x30, 0x00000000); /* nop */
    /* launch CPU 3 = node 1, slice 1 */
    put_instruction(image, 0x34, 0x24040001); /* addiu a0, zero, 1  (NASID 1) */
    put_instruction(image, 0x38, 0x24050001); /* addiu a1, zero, 1  (slice 1) */
    put_instruction(image, 0x3c, 0x3c060000 | CPU3_ENTRY_HI); /* lui a2 */
    put_instruction(image, 0x40, 0x34c60000 | CPU3_ENTRY_LO); /* ori a2 */
    put_instruction(image, 0x44, 0x24070000); /* addiu a3, zero, 0 */
    put_instruction(image, 0x48, 0x3c088000); /* lui t0, 0x8000 */
    put_instruction(image, 0x4c, 0x35087fe0); /* ori t0, t0, 0x7fe0 (sp) */
    put_instruction(image, 0x50, 0x3c098000); /* lui t1, 0x8000 */
    put_instruction(image, 0x54, 0x35291000); /* ori t1, t1, 0x1000 (gp) */
    put_instruction(image, 0x58, 0x3c190000 |
                    ((uint32_t)(LAUNCH_SLAVE >> 16) & 0xffff)); /* lui t9 */
    put_instruction(image, 0x5c, 0x37390000 | (LAUNCH_SLAVE & 0xffff)); /* ori t9 */
    put_instruction(image, 0x60, 0x0320f809); /* jalr ra, t9 */
    put_instruction(image, 0x64, 0x00000000); /* nop */
    /* CPU 0 records its own magic last. */
    put_instruction(image, 0x68, 0x3c088000); /* lui t0, 0x8000 */
    put_instruction(image, 0x6c, 0x35080000 | PRIMARY_RESULT); /* ori t0 */
    put_instruction(image, 0x70, 0x3c095a5a); /* lui t1, 0x5a5a */
    put_instruction(image, 0x74, 0x3529a5a5); /* ori t1, t1, 0xa5a5 */
    put_instruction(image, 0x78, 0xfd090000); /* sd t1, 0(t0) */
    put_instruction(image, 0x7c, 0x42000020); /* wait */

    /* CPU 2 (node 1, slice 0): write its mark to node-0 result slot, then wait. */
    put_instruction(image, 0x80, 0x3c088000); /* lui t0, 0x8000 */
    put_instruction(image, 0x84, 0x35080000 | CPU2_MARK_ADDR); /* ori t0 */
    put_instruction(image, 0x88, 0x3c090000 |
                    ((uint32_t)(CPU2_MARK >> 16) & 0xffff)); /* lui t1 */
    put_instruction(image, 0x8c, 0x35290000 | (CPU2_MARK & 0xffff)); /* ori t1 */
    put_instruction(image, 0x90, 0xfd090000); /* sd t1, 0(t0) */
    put_instruction(image, 0x94, 0x42000020); /* wait */

    /* CPU 3 (node 1, slice 1): same, to its own slot. */
    put_instruction(image, 0xc0, 0x3c088000); /* lui t0, 0x8000 */
    put_instruction(image, 0xc4, 0x35080000 | CPU3_MARK_ADDR); /* ori t0 */
    put_instruction(image, 0xc8, 0x3c090000 |
                    ((uint32_t)(CPU3_MARK >> 16) & 0xffff)); /* lui t1 */
    put_instruction(image, 0xcc, 0x35290000 | (CPU3_MARK & 0xffff)); /* ori t1 */
    put_instruction(image, 0xd0, 0xfd090000); /* sd t1, 0(t0) */
    put_instruction(image, 0xd4, 0x42000020); /* wait */

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void test_second_node_cpus_execute(const void *data)
{
    g_autofree char *kernel = NULL;
    QTestState *qts;
    int fd;
    unsigned int tries;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-cross-node-XXXXXX", &kernel, NULL);
    g_assert_cmpint(fd, >=, 0);
    write_cross_node_elf(fd);
    close(fd);

    qts = qtest_initf(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1 "
        "-kernel %s -accel tcg,thread=multi -no-reboot -no-shutdown",
        kernel);
    unlink(kernel);

    /* Both node-1 CPUs must reach their marks. */
    for (tries = 0; tries < 2000; tries++) {
        if (qtest_readq(qts, CPU2_MARK_ADDR) == CPU2_MARK &&
            qtest_readq(qts, CPU3_MARK_ADDR) == CPU3_MARK) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmphex(qtest_readq(qts, PRIMARY_RESULT), ==, PRIMARY_MAGIC);
    g_assert_cmphex(qtest_readq(qts, CPU2_MARK_ADDR), ==, CPU2_MARK);
    g_assert_cmphex(qtest_readq(qts, CPU3_MARK_ADDR), ==, CPU3_MARK);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/cross-node/second-node-cpus-execute",
                        NULL, test_second_node_cpus_execute);
    return g_test_run();
}
