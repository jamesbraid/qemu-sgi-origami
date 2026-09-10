/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercise the Origin 200 PROM's deliberate I/O bus-error probe through an
 * executing processor.  qtest MMIO cannot observe a processor exception, so
 * the guest installs a DBE handler, probes from each slice, and records Cause
 * and a completion marker in RAM.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#include <elf.h>

#define ELF_OFFSET             0x1000
#define IMAGE_SIZE             0x20200
#define KSEG_BASE              0xffffffff80000000ULL
#define VECTOR_OFFSET          0x180
#define PRIMARY_OFFSET         0x20000
#define SECONDARY_OFFSET       0x20100
#define LAUNCH_SLAVE           0xffffffffbfc00038ULL

#define CAUSE_A                0x50000
#define CAUSE_B                0x50008
#define MARKER_A               0x50010
#define MARKER_B               0x50018
#define START_A                0x50020
#define NARROW_MARKER          0x50028
#define LAUNCH_B               0x50030
#define REPROBE_B              0x50038
#define MARKER_B2              0x50040
#define MARKER                 0x5a17

#define ORIGIN2000_ROM_SIZE    (1024 * 1024)
#define ORIGIN2000_CPU         4
#define ORIGIN2000_CAUSE       0x50000
#define ORIGIN2000_RESULT      0x50008
#define ORIGIN2000_MARKER      0x50010
#define ORIGIN2000_HUB_LOCAL   0x20001000000ULL
#define ORIGIN2000_XBOW_LINK   10

#define HUB_LOCAL              0x20001000000ULL
#define WIDGET2_PROBE          0x20002400208ULL
#define WIDGETA_PROBE          0x2000a400208ULL
#define BRIDGE_BASE            0x20008000000ULL
#define BRIDGE_REG_DATA        4
#define BRIDGE_CFG0            0x020000
#define BRIDGE_CFG0_STRIDE     0x1000
#define BRIDGE_DEVICE(n)       (BRIDGE_BASE + 0x200 + 8 * (n) + \
                                BRIDGE_REG_DATA)
#define TESTDEV_SLOT           3
#define TESTDEV_CFG            (BRIDGE_BASE + BRIDGE_CFG0 + \
                                TESTDEV_SLOT * BRIDGE_CFG0_STRIDE)
#define PCI_COMMAND            0x04
#define PCI_COMMAND_MEMORY     0x0002
#define PCI_BAR2               0x18
#define TESTDEV_MMIO           0x14000000u
#define DEV_MEMORY_SPACE       (1u << 12)
#define PI_ERR_STATUS0_A       0x430
#define PI_ERR_STATUS0_CLR_A   0x438
#define PI_ERR_STATUS0_B       0x450
#define PI_ERR_STATUS0_CLR_B   0x458
#define PI_STATUS_VALID        (1ULL << 63)
#define PI_STATUS_ADDRESS      (0x480041ULL << 25)
#define PI_STATUS_EXPECTED     (PI_STATUS_VALID | PI_STATUS_ADDRESS)
#define CAUSE_EXCCODE_MASK     0x7c
#define CAUSE_EXCCODE_DBE      (7 << 2)

#define XTALK_BASE             0x20000000000ULL
#define XBOW_REG_DATA          4
#define XBOW_LINK(widget)      (0x100 + ((widget) - 8) * 0x40)
#define XBOW_LINK_STATUS       0x10
#define XBOW_LINK_STATUS_CLR   0x28
#define XBOW_LINK_ILLEGAL_DEST (1u << 17)
#define XBOW_LINK_MULTIPLE_ERR (1u << 18)

static void put_insn(uint8_t *image, unsigned int at, uint32_t insn)
{
    stl_be_p(image + ELF_OFFSET + at, insn);
}

static void put_probe(uint8_t *image, unsigned int at, uint32_t cause,
                      uint32_t marker)
{
    static const uint32_t probe[] = {
        0x3c109000, /* replaced: lui s0, cause address */
        0x36100000, /* replaced: ori s0, s0, cause */
        0x3c089200, /* lui t0, 0x9200 */
        0x0008403c, /* dsll32 t0, t0, 0 */
        0x3c090240, /* lui t1, 0x0240 */
        0x35290208, /* ori t1, t1, 0x0208 */
        0x01094025, /* or t0, t0, t1 */
        0xdd0a0000, /* ld t2, 0(t0) */
        0x3c088000, /* lui t0, 0x8000 */
        0x35080000, /* replaced: ori t0, t0, marker */
        0x24095a17, /* addiu t1, zero, MARKER */
        0xfd090000, /* sd t1, 0(t0) */
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(probe); i++) {
        put_insn(image, at + i * 4, probe[i]);
    }
    put_insn(image, at, 0x3c108000 | (cause >> 16));
    put_insn(image, at + 4, 0x36100000 | (cause & 0xffff));
    put_insn(image, at + 32, 0x3c088000 | (marker >> 16));
    put_insn(image, at + 36, 0x35080000 | (marker & 0xffff));
}

static char *write_probe_guest(void)
{
    uint8_t image[ELF_OFFSET + IMAGE_SIZE] = { 0 };
    GError *err = NULL;
    char *path = NULL;
    int fd;

    memcpy(image, ELFMAG, SELFMAG);
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stq_be_p(image + 24, KSEG_BASE + PRIMARY_OFFSET);
    stq_be_p(image + 32, 64);
    stw_be_p(image + 52, 64);
    stw_be_p(image + 54, 56);
    stw_be_p(image + 56, 3);

    stl_be_p(image + 64, PT_LOAD);
    stl_be_p(image + 68, PF_R | PF_X);
    stq_be_p(image + 72, ELF_OFFSET + VECTOR_OFFSET);
    stq_be_p(image + 80, KSEG_BASE + VECTOR_OFFSET);
    stq_be_p(image + 88, KSEG_BASE + VECTOR_OFFSET);
    stq_be_p(image + 96, 0x1c);
    stq_be_p(image + 104, 0x1c);
    stq_be_p(image + 112, 4);

    stl_be_p(image + 120, PT_LOAD);
    stl_be_p(image + 124, PF_R | PF_X);
    stq_be_p(image + 128, ELF_OFFSET + PRIMARY_OFFSET);
    stq_be_p(image + 136, KSEG_BASE + PRIMARY_OFFSET);
    stq_be_p(image + 144, KSEG_BASE + PRIMARY_OFFSET);
    stq_be_p(image + 152, 0x200);
    stq_be_p(image + 160, 0x200);
    stq_be_p(image + 168, 4);

    /* Processor B's private low-memory view uses the next 64 KiB backing. */
    stl_be_p(image + 176, PT_LOAD);
    stl_be_p(image + 180, PF_R | PF_X);
    stq_be_p(image + 184, ELF_OFFSET + VECTOR_OFFSET);
    stq_be_p(image + 192, KSEG_BASE + 0x10000 + VECTOR_OFFSET);
    stq_be_p(image + 200, KSEG_BASE + 0x10000 + VECTOR_OFFSET);
    stq_be_p(image + 208, 0x1c);
    stq_be_p(image + 216, 0x1c);
    stq_be_p(image + 224, 4);

    /* General exception handler: save Cause, skip the probe, and return. */
    put_insn(image, VECTOR_OFFSET + 0x00, 0x403a6800); /* dmfc0 k0, Cause */
    put_insn(image, VECTOR_OFFSET + 0x04, 0xfe1a0000); /* sd k0, 0(s0) */
    put_insn(image, VECTOR_OFFSET + 0x08, 0x403b7000); /* dmfc0 k1, EPC */
    put_insn(image, VECTOR_OFFSET + 0x0c, 0x677b0004); /* daddiu k1, k1, 4 */
    put_insn(image, VECTOR_OFFSET + 0x10, 0x409b7000); /* mtc0 k1, EPC */
    put_insn(image, VECTOR_OFFSET + 0x14, 0x42000018); /* eret */
    put_insn(image, VECTOR_OFFSET + 0x18, 0x00000000); /* nop */

    /* Both 32-bit halves must still complete through the Bridge. */
    put_insn(image, PRIMARY_OFFSET + 0x00, 0x3c108005);
    put_insn(image, PRIMARY_OFFSET + 0x04, 0x36100000);
    put_insn(image, PRIMARY_OFFSET + 0x08, 0x3c089200);
    put_insn(image, PRIMARY_OFFSET + 0x0c, 0x0008403c);
    put_insn(image, PRIMARY_OFFSET + 0x10, 0x3c090240);
    put_insn(image, PRIMARY_OFFSET + 0x14, 0x35290208);
    put_insn(image, PRIMARY_OFFSET + 0x18, 0x01094025);
    put_insn(image, PRIMARY_OFFSET + 0x1c, 0x8d0a0000); /* lw t2, 0(t0) */
    put_insn(image, PRIMARY_OFFSET + 0x20, 0x8d0b0004); /* lw t3, 4(t0) */
    put_insn(image, PRIMARY_OFFSET + 0x24, 0x3c088005);
    put_insn(image, PRIMARY_OFFSET + 0x28, 0x35080028);
    put_insn(image, PRIMARY_OFFSET + 0x2c, 0x24095a17);
    put_insn(image, PRIMARY_OFFSET + 0x30, 0xfd090000);
    put_insn(image, PRIMARY_OFFSET + 0x34, 0x3c088005);
    put_insn(image, PRIMARY_OFFSET + 0x38, 0x35080020);
    put_insn(image, PRIMARY_OFFSET + 0x3c, 0xdd090000);
    put_insn(image, PRIMARY_OFFSET + 0x40, 0x1120fffe);
    put_insn(image, PRIMARY_OFFSET + 0x44, 0x00000000);

    put_probe(image, PRIMARY_OFFSET + 0x48, CAUSE_A, MARKER_A);
    /* Let the test inspect A before it permits processor B to run. */
    put_insn(image, PRIMARY_OFFSET + 0x78, 0x3c088005);
    put_insn(image, PRIMARY_OFFSET + 0x7c, 0x35080030);
    put_insn(image, PRIMARY_OFFSET + 0x80, 0xdd090000);
    put_insn(image, PRIMARY_OFFSET + 0x84, 0x1120fffe);
    put_insn(image, PRIMARY_OFFSET + 0x88, 0x00000000);

    put_insn(image, PRIMARY_OFFSET + 0x8c, 0x24040000); /* a0 = node 0 */
    put_insn(image, PRIMARY_OFFSET + 0x90, 0x24050001); /* a1 = CPU 1 */
    put_insn(image, PRIMARY_OFFSET + 0x94,
             0x3c060000 | ((SECONDARY_OFFSET >> 16) + 0x8000));
    put_insn(image, PRIMARY_OFFSET + 0x98,
             0x34c60000 | (SECONDARY_OFFSET & 0xffff));
    put_insn(image, PRIMARY_OFFSET + 0x9c, 0x3c19bfc0); /* t9 = launch */
    put_insn(image, PRIMARY_OFFSET + 0xa0, 0x37390038);
    put_insn(image, PRIMARY_OFFSET + 0xa4, 0x0320f809); /* jalr ra, t9 */
    put_insn(image, PRIMARY_OFFSET + 0xa8, 0x00000000);
    put_insn(image, PRIMARY_OFFSET + 0xac, 0x1000ffff); /* b . */
    put_insn(image, PRIMARY_OFFSET + 0xb0, 0x00000000);

    put_probe(image, SECONDARY_OFFSET, CAUSE_B, MARKER_B);
    put_insn(image, SECONDARY_OFFSET + 0x30, 0x3c088005);
    put_insn(image, SECONDARY_OFFSET + 0x34, 0x35080038);
    put_insn(image, SECONDARY_OFFSET + 0x38, 0xdd090000);
    put_insn(image, SECONDARY_OFFSET + 0x3c, 0x1120fffe);
    put_insn(image, SECONDARY_OFFSET + 0x40, 0x00000000);
    put_probe(image, SECONDARY_OFFSET + 0x48, CAUSE_B, MARKER_B2);
    put_insn(image, SECONDARY_OFFSET + 0x78, 0x1000ffff); /* b . */
    put_insn(image, SECONDARY_OFFSET + 0x7c, 0x00000000);

    fd = g_file_open_tmp("sn0-pi-error-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==,
                    (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void wait_for_marker(QTestState *qts, uint64_t addr)
{
    unsigned int tries;

    for (tries = 0; tries < 2000; tries++) {
        if (qtest_readq(qts, addr) == MARKER) {
            return;
        }
        g_usleep(1000);
    }
    g_error("guest did not write marker at 0x%" PRIx64
            " (cause B 0x%" PRIx64 ", status B 0x%" PRIx64 ")",
            addr, qtest_readq(qts, CAUSE_B),
            qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B));
}

static void wait_for_pi_status_reset(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        if (qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A) == 0 &&
            qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B) == 0) {
            return;
        }
        g_usleep(1000);
    }
    g_error("timed out waiting for PI error status reset");
}

static uint64_t cpu_readq(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void cpu_writeq(QTestState *qts, unsigned int cpu, uint64_t addr,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

static uint32_t cpu_xbow_link_reg(QTestState *qts, unsigned int cpu,
                                  unsigned int widget, unsigned int reg)
{
    uint8_t bytes[4];
    uint64_t addr = XTALK_BASE + XBOW_LINK(widget) +
                    reg + XBOW_REG_DATA;

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static uint32_t cpu_xbow_link_status(QTestState *qts, unsigned int cpu,
                                     unsigned int widget)
{
    return cpu_xbow_link_reg(qts, cpu, widget, XBOW_LINK_STATUS);
}

static void assert_origin2000_widget2_report(QTestState *qts,
                                             const uint32_t before[8])
{
    for (unsigned int widget = 8; widget <= 15; widget++) {
        uint32_t status = cpu_xbow_link_status(qts, ORIGIN2000_CPU, widget);
        uint32_t expected = before[widget - 8];

        if (widget == ORIGIN2000_XBOW_LINK) {
            expected |= XBOW_LINK_ILLEGAL_DEST;
        }
        g_assert_cmphex(status, ==, expected);
        g_assert_cmphex(status & XBOW_LINK_MULTIPLE_ERR, ==, 0);
    }
    (void)cpu_xbow_link_reg(qts, ORIGIN2000_CPU, ORIGIN2000_XBOW_LINK,
                            XBOW_LINK_STATUS_CLR);
    for (unsigned int widget = 8; widget <= 15; widget++) {
        g_assert_cmphex(cpu_xbow_link_status(qts, ORIGIN2000_CPU, widget),
                        ==, before[widget - 8]);
    }
}

static GPtrArray *write_origin2000_probe_roms(void)
{
    g_autofree uint8_t *image = g_malloc0(ORIGIN2000_ROM_SIZE);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    uint32_t reset_words[2];
    unsigned int at = 0;

#define ROM_INSN(insn) do {                 \
        stl_be_p(image + at, (insn));        \
        at += sizeof(uint32_t);              \
    } while (0)

    ROM_INSN(0x3c080040); /* lui t0, 0x0040 */
    ROM_INSN(0x35080080); /* ori t0, t0, BEV | KX */
    ROM_INSN(0x40886000); /* mtc0 t0, Status */
    for (unsigned int i = 0; i < 4; i++) {
        ROM_INSN(0x00000000);
    }

    ROM_INSN(0x3c10a005); /* lui s0, uncached mailbox */
    ROM_INSN(0x3c119200); /* lui s1, Hub special XKPHYS */
    ROM_INSN(0x0011883c); /* dsll32 s1, s1, 0 */
    ROM_INSN(0x3c090100); /* lui t1, Hub local base */
    ROM_INSN(0x02298825); /* or s1, s1, t1 */
    ROM_INSN(0xde280020); /* ld t0, PI processor number(s1) */
    ROM_INSN(0x15000032); /* bne t0, zero, 0x100 */
    ROM_INSN(0x00000000); /* nop */

    ROM_INSN(0xfe000000); /* sd zero, cause(s0) */
    ROM_INSN(0x240a5a17); /* addiu t2, zero, sentinel */
    ROM_INSN(0x3c089200); /* lui t0, 0x9200 */
    ROM_INSN(0x0008403c); /* dsll32 t0, t0, 0 */
    ROM_INSN(0x3c090240); /* lui t1, 0x0240 */
    ROM_INSN(0x35290208); /* ori t1, t1, 0x0208 */
    ROM_INSN(0x01094025); /* or t0, t0, t1 */
    ROM_INSN(0xdd0a0000); /* ld t2, 0(t0) */
    ROM_INSN(0xfe0a0008); /* sd t2, result(s0) */
    ROM_INSN(0x24095a17); /* addiu t1, zero, marker */
    ROM_INSN(0xfe090010); /* sd t1, marker(s0) */
    ROM_INSN(0x1000ffff); /* b . */
    ROM_INSN(0x00000000); /* nop */

    at = 0x100;
    ROM_INSN(0x1000ffff); /* CPUs other than CPU 4 park here. */
    ROM_INSN(0x00000000);

    at = 0x380;
    ROM_INSN(0x403a6800); /* dmfc0 k0, Cause */
    ROM_INSN(0xfe1a0000); /* sd k0, cause(s0) */
    ROM_INSN(0x403b7000); /* dmfc0 k1, EPC */
    ROM_INSN(0x677b0004); /* daddiu k1, k1, 4 */
    ROM_INSN(0x409b7000); /* mtc0 k1, EPC */
    ROM_INSN(0x42000018); /* eret */
    ROM_INSN(0x00000000); /* nop */

#undef ROM_INSN

    reset_words[0] = ldl_be_p(image);
    reset_words[1] = ldl_be_p(image + 4);
    for (unsigned int node = 0; node < 4; node++) {
        g_autofree char *pattern = g_strdup_printf(
            "sn0-origin2000-node%u-pi-error-XXXXXX", node);
        GError *err = NULL;
        char *path = NULL;
        int fd;

        stl_be_p(image, node == 2 ? reset_words[0] : 0x1000ffff);
        stl_be_p(image + 4, node == 2 ? reset_words[1] : 0);
        fd = g_file_open_tmp(pattern, &path, &err);
        g_assert_no_error(err);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(write(fd, image, ORIGIN2000_ROM_SIZE), ==,
                        ORIGIN2000_ROM_SIZE);
        g_assert_cmpint(close(fd), ==, 0);
        g_ptr_array_add(paths, path);
    }
    return paths;
}

static void wait_for_origin2000_marker(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        if (cpu_readq(qts, ORIGIN2000_CPU, ORIGIN2000_MARKER) == MARKER) {
            return;
        }
        g_usleep(1000);
    }
    g_error("CPU 4 did not complete the guarded bus-error probe");
}

static void test_origin2000_probe_is_local_to_issuing_hub(const void *data)
{
    g_autoptr(GPtrArray) roms = write_origin2000_probe_roms();
    const uint8_t write_data[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t read_data[4];
    uint32_t links_before[8];
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = qtest_initf(
        "-M origin2000 -smp 8,sockets=4,cores=2 -m 256M "
        "-drive if=pflash,index=0,file=%s,format=raw "
        "-drive if=pflash,index=1,file=%s,format=raw "
        "-drive if=pflash,index=2,file=%s,format=raw "
        "-drive if=pflash,index=3,file=%s,format=raw "
        "-accel tcg,thread=multi -no-reboot -no-shutdown -S",
        (char *)g_ptr_array_index(roms, 0),
        (char *)g_ptr_array_index(roms, 1),
        (char *)g_ptr_array_index(roms, 2),
        (char *)g_ptr_array_index(roms, 3));
    for (unsigned int node = 0; node < roms->len; node++) {
        g_unlink(g_ptr_array_index(roms, node));
    }
    cpu_writeq(qts, ORIGIN2000_CPU, ORIGIN2000_CAUSE, 0);
    cpu_writeq(qts, ORIGIN2000_CPU, ORIGIN2000_RESULT, 0);
    cpu_writeq(qts, ORIGIN2000_CPU, ORIGIN2000_MARKER, 0);
    for (unsigned int widget = 8; widget <= 15; widget++) {
        links_before[widget - 8] =
            cpu_xbow_link_status(qts, ORIGIN2000_CPU, widget);
        g_assert_cmphex(links_before[widget - 8] & XBOW_LINK_ILLEGAL_DEST,
                        ==, 0);
        g_assert_cmphex(links_before[widget - 8] & XBOW_LINK_MULTIPLE_ERR,
                        ==, 0);
    }

    g_assert_true(qtest_memread_cpu(qts, ORIGIN2000_CPU, WIDGET2_PROBE,
                                    read_data, sizeof(read_data)));
    assert_origin2000_widget2_report(qts, links_before);
    g_assert_true(qtest_memwrite_cpu(qts, ORIGIN2000_CPU, WIDGET2_PROBE,
                                     write_data, sizeof(write_data)));
    assert_origin2000_widget2_report(qts, links_before);
    g_assert_true(qtest_memread_cpu(qts, ORIGIN2000_CPU, WIDGET2_PROBE + 1,
                                    read_data, sizeof(read_data)));
    assert_origin2000_widget2_report(qts, links_before);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");

    wait_for_origin2000_marker(qts);
    g_assert_cmphex(cpu_readq(qts, ORIGIN2000_CPU, ORIGIN2000_CAUSE) &
                    CAUSE_EXCCODE_MASK, ==, CAUSE_EXCCODE_DBE);
    g_assert_cmphex(cpu_readq(qts, ORIGIN2000_CPU, ORIGIN2000_RESULT), ==,
                    MARKER);
    g_assert_cmphex(cpu_readq(qts, ORIGIN2000_CPU,
                              ORIGIN2000_HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(cpu_readq(qts, ORIGIN2000_CPU,
                              ORIGIN2000_HUB_LOCAL + PI_ERR_STATUS0_B), ==, 0);
    g_assert_cmphex(cpu_readq(qts, 0,
                              ORIGIN2000_HUB_LOCAL + PI_ERR_STATUS0_A), ==, 0);
    for (unsigned int widget = 8; widget <= 15; widget++) {
        uint32_t status = cpu_xbow_link_status(qts, ORIGIN2000_CPU, widget);

        g_assert_cmphex(status, ==, links_before[widget - 8]);
        g_assert_cmphex(status & XBOW_LINK_ILLEGAL_DEST, ==, 0);
    }

    cpu_writeq(qts, ORIGIN2000_CPU,
               ORIGIN2000_HUB_LOCAL + PI_ERR_STATUS0_CLR_A, 0);
    g_assert_cmphex(cpu_readq(qts, ORIGIN2000_CPU,
                              ORIGIN2000_HUB_LOCAL + PI_ERR_STATUS0_A), ==, 0);
    qtest_quit(qts);
}

static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{'execute': 'migrate', 'arguments': {'uri': %s}}", uri);
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(
            from, "{'execute': 'query-migrate'}");
        const char *status = qdict_get_str(ret, "status");

        if (!strcmp(status, "completed")) {
            qobject_unref(ret);
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(ret);
        g_usleep(1000);
    }
}

static void test_prom_probe_latches_the_issuing_slice(const void *data)
{
    g_autofree char *kernel = write_probe_guest();
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = qtest_initf("-M origin200 -m 64M -smp 2 "
                      "-kernel %s -accel tcg,thread=multi "
                      "-no-reboot -no-shutdown -S", kernel);
    g_unlink(kernel);
    qtest_writeq(qts, START_A, 0);
    qtest_writeq(qts, LAUNCH_B, 0);
    qtest_writeq(qts, REPROBE_B, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");

    wait_for_marker(qts, NARROW_MARKER);
    g_assert_cmphex(qtest_readq(qts, CAUSE_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==, 0);

    qtest_writeq(qts, START_A, 1);
    wait_for_marker(qts, MARKER_A);
    g_assert_cmphex(qtest_readq(qts, CAUSE_A) & CAUSE_EXCCODE_MASK, ==,
                    CAUSE_EXCCODE_DBE);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==, 0);

    qtest_writeq(qts, LAUNCH_B, 1);
    wait_for_marker(qts, MARKER_B);
    g_assert_cmphex(qtest_readq(qts, CAUSE_B) & CAUSE_EXCCODE_MASK, ==,
                    CAUSE_EXCCODE_DBE);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);

    /*
     * What a 32-bit access to a read-to-clear register does on real
     * hardware is unobserved, so the model's conservative choice is
     * pinned here: a half read returns its big-endian half of the
     * doubleword with the latch intact, and only a full-width access —
     * the read, or the observed zero store — clears it. A nonzero store
     * of either width clears nothing.
     */
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_B), ==,
                    PI_STATUS_EXPECTED >> 32);
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_B + 4),
                    ==, (uint32_t)PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);

    qtest_writeq(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_B, 1);
    qtest_writel(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_B, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    qtest_writeq(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_B, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);

    qtest_writeq(qts, REPROBE_B, 1);
    wait_for_marker(qts, MARKER_B2);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_B), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_CLR_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==, 0);

    qtest_quit(qts);
}

static void test_narrow_probe_access_matches_bridge_alias(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 "
                                 "-device pci-testdev,addr=3,membar=1M,"
                                 "membar-backed=on");

    qtest_writel(qts, TESTDEV_CFG + PCI_BAR2, TESTDEV_MMIO);
    qtest_writel(qts, TESTDEV_CFG + PCI_BAR2 + 4, 0);
    qtest_writel(qts, TESTDEV_CFG + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(1),
                 DEV_MEMORY_SPACE | (TESTDEV_MMIO >> 20));

    qtest_writew(qts, WIDGETA_PROBE, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, WIDGETA_PROBE), ==, 0x5aa5);
    g_assert_cmphex(qtest_readw(qts, WIDGET2_PROBE), ==,
                    qtest_readw(qts, WIDGETA_PROBE));

    qtest_writel(qts, WIDGET2_PROBE + 4, 0x1234abcd);
    g_assert_cmphex(qtest_readl(qts, WIDGET2_PROBE + 4), ==, 0x1234abcd);
    g_assert_cmphex(qtest_readl(qts, WIDGETA_PROBE + 4), ==, 0x1234abcd);
    qtest_quit(qts);
}

static void test_pi_status_survives_migration(const void *data)
{
    g_autofree char *kernel = write_probe_guest();
    g_autofree char *tmpdir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    tmpdir = g_dir_make_tmp("sn0-pi-error-migration-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(tmpdir);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);

    from = qtest_initf("-M origin200 -m 64M -smp 2 "
                       "-kernel %s -accel tcg,thread=multi "
                       "-no-reboot -no-shutdown -S", kernel);
    qtest_writeq(from, START_A, 0);
    qtest_writeq(from, LAUNCH_B, 0);
    qtest_writeq(from, REPROBE_B, 0);
    qtest_qmp_assert_success(from, "{'execute': 'cont'}");
    wait_for_marker(from, NARROW_MARKER);
    qtest_writeq(from, START_A, 1);
    wait_for_marker(from, MARKER_A);
    qtest_writeq(from, LAUNCH_B, 1);
    wait_for_marker(from, MARKER_B);
    qtest_qmp_assert_success(from, "{'execute': 'stop'}");
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);

    dest_args = g_strdup_printf(
        "-M origin200 -m 64M -smp 2 -kernel %s "
        "-accel tcg,thread=multi -no-reboot -no-shutdown -incoming %s",
        kernel, uri);
    to = qtest_init(dest_args);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);

    qtest_quit(to);
    qtest_quit(from);
    g_unlink(kernel);
}

static void test_pi_status_reset_clears_both_slices(const void *data)
{
    g_autofree char *kernel = write_probe_guest();
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    qts = qtest_initf("-M origin200 -m 64M -smp 2 "
                      "-kernel %s -accel tcg,thread=multi "
                      "-S", kernel);
    qtest_writeq(qts, START_A, 0);
    qtest_writeq(qts, LAUNCH_B, 0);
    qtest_writeq(qts, REPROBE_B, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    wait_for_marker(qts, NARROW_MARKER);
    qtest_writeq(qts, START_A, 1);
    wait_for_marker(qts, MARKER_A);
    qtest_writeq(qts, LAUNCH_B, 1);
    wait_for_marker(qts, MARKER_B);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_A), ==,
                    PI_STATUS_EXPECTED);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_ERR_STATUS0_B), ==,
                    PI_STATUS_EXPECTED);

    qtest_writeq(qts, START_A, 0);
    qtest_writeq(qts, LAUNCH_B, 0);
    qtest_system_reset_nowait(qts);
    wait_for_pi_status_reset(qts);

    qtest_quit(qts);
    g_unlink(kernel);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/pi-error/prom-probe-latches-issuing-slice", NULL,
                        test_prom_probe_latches_the_issuing_slice);
    qtest_add_data_func("sn0/pi-error/status-survives-migration", NULL,
                        test_pi_status_survives_migration);
    qtest_add_data_func("sn0/pi-error/narrow-access-matches-bridge-alias", NULL,
                        test_narrow_probe_access_matches_bridge_alias);
    qtest_add_data_func("sn0/pi-error/reset-clears-both-slices", NULL,
                        test_pi_status_reset_clears_both_slices);
    qtest_add_data_func("sn0/pi-error/origin2000-local-issuing-hub", NULL,
                        test_origin2000_probe_is_local_to_issuing_hub);
    return g_test_run();
}
