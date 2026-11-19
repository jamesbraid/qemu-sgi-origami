/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * OCTANE Digital Video option register-contract tests, on the octane and
 * octane2 machines. Expectations come from
 * docs/system/mips/sgi/hardware/galileo.rst, except the field
 * period and the line-to-pin wiring, which are the page's approximations.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"

#define XBOW        0x10000000ULL
#define BOARD       (XBOW + (10ULL << 24))
#define GFX         (XBOW + (11ULL << 24))
#define RAM         0x20000000ULL
#define IRQ_SINK    (RAM + 0x110000)

/* "Probe" and "Register blocks". */
#define OPTION_CTRL 0x61000
#define OPTION_ID   0x70008
#define BLEND       0x72800
#define CC1         0x73000
#define CONTROL     0x73400
#define CSC         0x73800
#define DIRECT(blk, n)  ((blk) + (n) * 0x80 + 8)

/* Bridge widget registers, data half of each slot. */
#define W_ID        0x04
#define W_STATUS    0x0c
#define W_DEST_UP   0x34
#define W_DEST_LOW  0x3c
#define B_INT_EN    0x10c
#define B_INT_HOST(pin) (0x134 + 8 * (pin))
#define GIO_SLOT0   0x200004
#define GIO_SLOT1   0x600004

/* An approximation: the NTSC field period. */
#define FIELD_NS    16683333
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "reset-mode=0x1aa683"
#define MACHINE_BRIDGE 0x1f000000ULL

static QTestState *qts;
static bool octane2;
static char *flash_path;

static void start(const char *option)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -m 256 -cpu %s "
                      "-bios %s -S -display none "
                      "-device sgi-mgras,slot=widget-11,product=mxi %s",
                      octane2 ? "octane2" : "octane",
                      octane2 ? OCTANE2_CPU : "R10000", quoted, option);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
}

static void start_video(void)
{
    start("-device sgi-galileo,slot=widget-10,id=galileo");
}

static uint8_t rb(uint32_t off)
{
    uint8_t v;

    g_assert_cmpuint(memtx_readb(qts, GFX + off, &v), ==, 0);
    return v;
}

static void wb(uint32_t off, uint8_t v)
{
    g_assert_cmpuint(memtx_writeb(qts, GFX + off, v), ==, 0);
}

static uint32_t bridge_rd(uint32_t off)
{
    uint32_t v;

    g_assert_cmpuint(memtx_readl(qts, BOARD + off, &v), ==, 0);
    return v;
}

static void bridge_wr(uint32_t off, uint32_t v)
{
    g_assert_cmpuint(memtx_writel(qts, BOARD + off, v), ==, 0);
}

/* An indirect register of a block whose index and data are directs i and d. */
static uint8_t ind_rd(uint32_t blk, unsigned i, unsigned d, uint8_t idx)
{
    wb(DIRECT(blk, i), idx);
    return rb(DIRECT(blk, d));
}

static void ind_wr(uint32_t blk, unsigned i, unsigned d, uint8_t idx,
                   uint8_t v)
{
    wb(DIRECT(blk, i), idx);
    wb(DIRECT(blk, d), v);
}

static void blend_addr(uint16_t addr)
{
    wb(DIRECT(BLEND, 3), addr & 0xff);
    wb(DIRECT(BLEND, 4), addr >> 8);
}

/* "Probe": the driver's store, then the byte that must read 0x11. */
static void test_probe(void)
{
    start_video();
    g_assert_cmpuint(memtx_writel(qts, GFX + OPTION_CTRL, 0x4210), ==, 0);
    g_assert_cmphex(rb(OPTION_ID), ==, 0x11);
    qtest_quit(qts);
}

/* The option is off unless fitted: the probe byte reads all ones. */
static void test_absent(void)
{
    start("");
    g_assert_cmpuint(memtx_writel(qts, GFX + OPTION_CTRL, 0x4210), ==, 0);
    g_assert_cmphex(rb(OPTION_ID), ==, 0xff);
    qtest_quit(qts);
}

/* "Identity": the three revision reads of the driver's reset. */
static void test_revisions(void)
{
    start_video();
    wb(DIRECT(CONTROL, 3), 1);
    g_assert_cmphex(rb(DIRECT(CONTROL, 3)), ==, 1);
    g_assert_cmphex(rb(DIRECT(CONTROL, 2)), ==, 0x24);
    g_assert_cmphex(ind_rd(CONTROL, 6, 7, 147), ==, 2);
    g_assert_cmphex(rb(DIRECT(CSC, 3)), ==, 0);
    /* The revisions are not writable. */
    wb(DIRECT(CONTROL, 2), 9);
    ind_wr(CONTROL, 6, 7, 147, 9);
    wb(DIRECT(CSC, 3), 9);
    g_assert_cmphex(rb(DIRECT(CONTROL, 2)), ==, 0x24);
    g_assert_cmphex(ind_rd(CONTROL, 6, 7, 147), ==, 2);
    g_assert_cmphex(rb(DIRECT(CSC, 3)), ==, 0);
    qtest_quit(qts);
}

/* "Register blocks": direct storage and each block's indirect pair. */
static void test_indirect(void)
{
    start_video();
    wb(DIRECT(CC1, 0), 0x44);
    g_assert_cmphex(rb(DIRECT(CC1, 0)), ==, 0x44);

    ind_wr(CC1, 1, 2, 80, 0x5a);
    ind_wr(CC1, 1, 2, 3, 0xa5);
    g_assert_cmphex(ind_rd(CC1, 1, 2, 80), ==, 0x5a);
    g_assert_cmphex(ind_rd(CC1, 1, 2, 3), ==, 0xa5);
    /* Past the last register nothing is stored. */
    ind_wr(CC1, 1, 2, 81, 0x77);
    g_assert_cmphex(ind_rd(CC1, 1, 2, 81), ==, 0);

    /* Blender: a 16-bit address in direct 3 (low), 4 (high); data at 1 or 2. */
    blend_addr(38);
    wb(DIRECT(BLEND, 1), 0x11);
    g_assert_cmphex(rb(DIRECT(BLEND, 1)), ==, 0x11);
    blend_addr(0x100 | 38);
    g_assert_cmphex(rb(DIRECT(BLEND, 1)), ==, 0);
    blend_addr(3);
    wb(DIRECT(BLEND, 2), 0x22);
    g_assert_cmphex(rb(DIRECT(BLEND, 2)), ==, 0x22);
    /* The two files are separate, and the second ends at 3. */
    g_assert_cmphex(rb(DIRECT(BLEND, 1)), ==, 0);
    blend_addr(4);
    wb(DIRECT(BLEND, 2), 0x33);
    g_assert_cmphex(rb(DIRECT(BLEND, 2)), ==, 0);

    ind_wr(CSC, 0, 1, 5, 0x33);
    g_assert_cmphex(ind_rd(CSC, 0, 1, 5), ==, 0x33);

    /* Control: direct 0 and 1, 0 to 39 and 240 to 255; the TMI has its own. */
    ind_wr(CONTROL, 0, 1, 39, 0x12);
    ind_wr(CONTROL, 0, 1, 240, 0x34);
    ind_wr(CONTROL, 0, 1, 100, 0x56);
    g_assert_cmphex(ind_rd(CONTROL, 0, 1, 39), ==, 0x12);
    g_assert_cmphex(ind_rd(CONTROL, 0, 1, 240), ==, 0x34);
    g_assert_cmphex(ind_rd(CONTROL, 0, 1, 100), ==, 0);
    ind_wr(CONTROL, 6, 7, 0x80, 0x9c);
    g_assert_cmphex(ind_rd(CONTROL, 6, 7, 0x80), ==, 0x9c);
    g_assert_cmphex(ind_rd(CONTROL, 0, 1, 0x80), ==, 0);
    qtest_quit(qts);
}

/* The registers that do not read back what was written. */
static void test_overrides(void)
{
    start_video();
    /* I2C busy (control 37 bit 7) reads clear, the other bits as written. */
    ind_wr(CONTROL, 0, 1, 37, 0xc1);
    g_assert_cmphex(ind_rd(CONTROL, 0, 1, 37), ==, 0x41);
    /* Timer status (control 17 bits 0, 4, 6) reads clear while idle. */
    ind_wr(CONTROL, 0, 1, 17, 0xff);
    g_assert_cmphex(ind_rd(CONTROL, 0, 1, 17), ==, 0xae);
    qtest_quit(qts);
}

/* "GIO window": the Bridge's identity, mode and the slot probes. */
static void test_gio_bridge(void)
{
    uint32_t v;

    start_video();
    g_assert_cmphex(bridge_rd(W_ID), ==, (1u << 28) | (0xc002 << 12) |
                                         (0x36 << 1) | 1);
    g_assert_cmphex(bridge_rd(W_STATUS) & 0x20, ==, 0);
    /* The machine's own Bridge keeps its PCI strap. */
    g_assert_cmpuint(memtx_readl(qts, MACHINE_BRIDGE + W_STATUS, &v), ==, 0);
    g_assert_cmphex(v & 0x20, ==, 0x20);
    g_assert_cmpuint(memtx_readl(qts, BOARD + GIO_SLOT0, &v), ==, 0);
    g_assert_cmphex(v & 0x7f, ==, 0x11);
    g_assert_cmpuint(memtx_readl(qts, BOARD + GIO_SLOT1, &v), ==, 0);
    g_assert_cmphex(v & 0x7f, ==, 0);
    qtest_quit(qts);
}

/* "GIO window": the driver's VGI1 presence path. */
static void test_vgi1(void)
{
    uint64_t v;

    start_video();
    g_assert_cmpuint(memtx_readq(qts, BOARD + GIO_SLOT0 - 4, &v), ==, 0);
    g_assert_cmphex(v & 0x7f, ==, 0x11);
    /* The slot probe's word at +4 is the same register. */
    g_assert_cmphex(v, ==, 0x11);
    g_assert_cmpuint(memtx_readq(qts, BOARD + GIO_SLOT0 - 4 + 0x20, &v), ==,
                     0);
    g_assert_cmpuint(memtx_writeq(qts, BOARD + GIO_SLOT0 - 4 + 0x10, 0x101),
                     ==, 0);
    g_assert_cmpuint(memtx_writeq(qts, BOARD + GIO_SLOT0 - 4 + 0x1000, 0),
                     ==, 0);
    qtest_quit(qts);
}

/* TMI indirect 129 bit 1 pulses, as a texture-transfer start waits for. */
static void test_tmi_status(void)
{
    start_video();
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(ind_rd(CONTROL, 6, 7, 129) & 2, ==, 2);
        g_assert_cmphex(ind_rd(CONTROL, 6, 7, 129) & 2, ==, 0);
    }
    qtest_quit(qts);
}

static bool sink_hit(uint64_t *word)
{
    uint8_t buf[8];
    static const uint8_t zero[8];

    qtest_memread(qts, IRQ_SINK, buf, sizeof(buf));
    qtest_memwrite(qts, IRQ_SINK, zero, sizeof(zero));
    *word = ldq_be_p(buf);
    return *word != 0;
}

/* "Interrupts": CC1 fields on GIO line 6 while CC1 indirect 38 enables them. */
static void test_field_interrupt(void)
{
    uint64_t word;

    start_video();
    bridge_wr(W_DEST_UP, 8u << 16);
    bridge_wr(W_DEST_LOW, IRQ_SINK);
    bridge_wr(B_INT_HOST(6), 0x46);

    /* Attach's last write: bits 7 and 5 set. Pulses before enable are lost. */
    ind_wr(CC1, 1, 2, 38, 0xa2);
    qtest_clock_step(qts, 2 * FIELD_NS + 1000);
    g_assert_false(sink_hit(&word));
    bridge_wr(B_INT_EN, 1u << 6);
    qtest_clock_step(qts, FIELD_NS - 3000);
    g_assert_false(sink_hit(&word));
    for (unsigned i = 0; i < 3; i++) {
        qtest_clock_step(qts, i ? FIELD_NS : 4000);
        g_assert_true(sink_hit(&word));
        g_assert_cmphex(word, ==, 0x146);
    }

    /* Either enable alone keeps it running. */
    ind_wr(CC1, 1, 2, 38, 0x20);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_true(sink_hit(&word));

    /* Both clear stops it. */
    ind_wr(CC1, 1, 2, 38, 0x02);
    qtest_clock_step(qts, 3 * FIELD_NS);
    g_assert_false(sink_hit(&word));
    qtest_quit(qts);
}

/* A system reset clears the registers and stops the field interrupts. */
static void test_reset(void)
{
    uint64_t word;

    start_video();
    ind_wr(CC1, 1, 2, 3, 0x66);
    ind_wr(CC1, 1, 2, 38, 0xa2);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(ind_rd(CC1, 1, 2, 3), ==, 0);
    g_assert_cmphex(ind_rd(CC1, 1, 2, 38), ==, 0);
    qtest_clock_step(qts, 3 * FIELD_NS);
    g_assert_false(sink_hit(&word));
    g_assert_cmphex(ind_rd(CONTROL, 6, 7, 147), ==, 2);
    qtest_quit(qts);
}

typedef struct Case {
    void (*fn)(void);
    bool octane2;
} Case;

static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    { "probe", test_probe },
    { "absent", test_absent },
    { "revisions", test_revisions },
    { "indirect", test_indirect },
    { "overrides", test_overrides },
    { "vgi1", test_vgi1 },
    { "tmi-status", test_tmi_status },
    { "gio-bridge", test_gio_bridge },
    { "field-interrupt", test_field_interrupt },
    { "reset", test_reset },
};

static Case cases[2][G_N_ELEMENTS(tests)];

static void run_test(gconstpointer data)
{
    const Case *c = data;

    octane2 = c->octane2;
    c->fn();
}

int main(int argc, char **argv)
{
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    for (unsigned p = 0; p < 2; p++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
            cases[p][i].fn = tests[i].fn;
            cases[p][i].octane2 = p;
        }
    }
    fd = g_file_open_tmp("galileo-prom-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    for (unsigned p = 0; p < 2; p++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
            g_autofree char *path = g_strdup_printf(
                "/%s/galileo/%s", p ? "octane2" : "octane",
                tests[i].name);

            qtest_add_data_func(path, &cases[p][i], run_test);
        }
    }
    result = g_test_run();
    g_assert_cmpint(unlink(flash_path), ==, 0);
    g_free(flash_path);
    return result;
}
