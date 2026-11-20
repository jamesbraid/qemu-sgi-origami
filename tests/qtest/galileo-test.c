/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * OCTANE Digital Video option register-contract tests, on the octane and
 * octane2 machines. Expectations come from
 * docs/system/mips/sgi/hardware/galileo.rst, except the field
 * period and the line-to-pin wiring, which are the page's approximations.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"
#include "sgi-migrate.h"

#define XBOW        0x10000000ULL
#define BOARD       (XBOW + (10ULL << 24))
#define GFX         (XBOW + (11ULL << 24))
#define RAM         0x20000000ULL
#define IRQ_SINK    (RAM + 0x110000)

/* "Probe" and "Register blocks". */
#define OPTION_CTRL 0x61000
#define OPTION_ID   0x70008
#define CMAP_RA     0x71708
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

static QTestState *launch(const char *option)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    return qtest_initf(MEMTX_TEST_DEVICE "-M %s -m 256 -cpu %s "
                       "-bios %s -S -display none "
                       "-device sgi-mgras,slot=widget-11,product=mxi %s",
                       octane2 ? "octane2" : "octane",
                       octane2 ? OCTANE2_CPU : "R10000", quoted, option);
}

static void start(const char *option)
{
    qts = launch(option);
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

/*
 * "Probe": the driver's store, then the byte that must read 0x11. The
 * graphics board's option bit reads clear.
 */
static void test_probe(void)
{
    start_video();
    g_assert_cmpuint(memtx_writel(qts, GFX + OPTION_CTRL, 0x4210), ==, 0);
    g_assert_cmphex(rb(OPTION_ID), ==, 0x11);
    g_assert_cmphex(rb(CMAP_RA) & 0x80, ==, 0);
    qtest_quit(qts);
}

/* Not fitted: the probe byte reads all ones and the option bit reads set. */
static void test_absent(void)
{
    start("");
    g_assert_cmpuint(memtx_writel(qts, GFX + OPTION_CTRL, 0x4210), ==, 0);
    g_assert_cmphex(rb(OPTION_ID), ==, 0xff);
    g_assert_cmphex(rb(CMAP_RA) & 0x80, ==, 0x80);
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

/*
 * Fields keep their phase: re-enabling between two fields waits for the
 * next field of the original grid, which still holds 600 fields later.
 */
static void test_field_phase(void)
{
    uint64_t word;

    start_video();
    bridge_wr(W_DEST_UP, 8u << 16);
    bridge_wr(W_DEST_LOW, IRQ_SINK);
    bridge_wr(B_INT_HOST(6), 0x46);
    bridge_wr(B_INT_EN, 1u << 6);
    ind_wr(CC1, 1, 2, 38, 0xa2);
    qtest_clock_step(qts, 3 * FIELD_NS + FIELD_NS / 2);
    g_assert_true(sink_hit(&word));
    ind_wr(CC1, 1, 2, 38, 0x02);
    qtest_clock_step(qts, 2 * FIELD_NS);
    g_assert_false(sink_hit(&word));

    ind_wr(CC1, 1, 2, 38, 0xa2);
    qtest_clock_step(qts, FIELD_NS / 2 - 1000);
    g_assert_false(sink_hit(&word));
    qtest_clock_step(qts, 2000);
    g_assert_true(sink_hit(&word));

    qtest_clock_step(qts, 600LL * FIELD_NS - 2000);
    g_assert_true(sink_hit(&word));
    qtest_clock_step(qts, 500);
    g_assert_false(sink_hit(&word));
    qtest_clock_step(qts, 1000);
    g_assert_true(sink_hit(&word));
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


/* "DMA channels": the Bridge direct map sends GIO 0x80000000 to RAM. */
#define B_DIRECT    0x84
#define DIRECT_RAM  ((8u << 20) | (1u << 17))
#define LIST        (RAM + 0x120000)
#define DATA        (RAM + 0x200000)
#define GIO_RAM     0x80000000ULL
#define CH1         0x1000
#define CH_SETUP    0x000
#define CH_INT_ENABLE  0x010
#define CH_HORIZONTAL   0x020
#define CH_VERTICAL   0x030
#define CH_STRIDE   0x040
#define CH_TRIGGER     0x200
#define CH_START_OFFSET 0x210
#define CH_PAGE_LIST 0x220
#define CH_STATUS   0x300
#define LINE_BYTES  64
#define LINES       16
#define START       16000

static uint64_t gio_rd(uint32_t off)
{
    uint64_t v;

    g_assert_cmpuint(memtx_readq(qts, BOARD + GIO_SLOT0 - 4 + off, &v), ==, 0);
    return v;
}

static void gio_wr(uint32_t off, uint64_t v)
{
    g_assert_cmpuint(memtx_writeq(qts, BOARD + GIO_SLOT0 - 4 + off, v), ==, 0);
}

static uint64_t sink_get(const char *prop)
{
    QDict *r = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                         "{'path':'/machine/peripheral/rec',"
                         "'property':%s}}", prop);
    uint64_t v = qdict_get_int(r, "return");

    qobject_unref(r);
    return v;
}

/*
 * Two pages of picture, 16 by 16 pixels of B G R packed in three bytes, the
 * first row white, the second row's first pixel red and the rest clear,
 * starting START bytes into the first page; and a page list.
 */
static void dma_prepare(const char *irq)
{
    uint8_t pic[LINES * LINE_BYTES / 4 * 3] = { 0 };
    uint64_t entry[2];

    memset(pic, 0xff, LINE_BYTES / 4 * 3);
    pic[LINE_BYTES / 4 * 3 + 2] = 0xff;
    qtest_memwrite(qts, DATA + START, pic, 16384 - START);
    qtest_memwrite(qts, DATA + 16384, pic + (16384 - START),
                   sizeof(pic) - (16384 - START));
    entry[0] = cpu_to_be64((GIO_RAM + 0x200000) >> 14);
    entry[1] = cpu_to_be64((GIO_RAM + 0x204000) >> 14);
    qtest_memwrite(qts, LIST, entry, sizeof(entry));
    bridge_wr(B_DIRECT, DIRECT_RAM);
    if (irq) {
        bridge_wr(W_DEST_UP, 8u << 16);
        bridge_wr(W_DEST_LOW, IRQ_SINK);
        bridge_wr(B_INT_HOST(4), 0x44);
        bridge_wr(B_INT_EN, 1u << 4);
    }
    gio_wr(CH1 + CH_SETUP, 0x4c);
    gio_wr(CH1 + CH_STRIDE, LINE_BYTES);
    gio_wr(CH1 + CH_HORIZONTAL, 0x00100010);
    gio_wr(CH1 + CH_VERTICAL, ((uint64_t)8 << 48) | (0x11ULL << 32) |
                            (8 << 16) | 0x11);
    gio_wr(CH1 + CH_STATUS, 0x2f0b);
    gio_wr(CH1 + CH_INT_ENABLE, 0x2f1b);
}

/* The status as the guest reads it; the end of DMA never rides a vertical. */
static uint64_t st(void)
{
    uint64_t v = gio_rd(CH1 + CH_STATUS);

    g_assert_cmphex(v & 9, !=, 9);
    return v;
}

#define EVENTS(v)   ((v) & 0xffff)
#define COUNT(v)    ((v) >> 33 & 0x7fff)
#define PARITY(v)   ((v) >> 32 & 1)

static void dma_request(void)
{
    gio_wr(CH1 + CH_START_OFFSET, START);
    gio_wr(CH1 + CH_PAGE_LIST, GIO_RAM + 0x120000);
}

/* A request's events, the field counter and parity, and its frame. */
static void test_dma_transfer(void)
{
    g_autofree char *dir = g_dir_make_tmp("galileo-dma-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "out.y4m", NULL);
    g_autofree char *opt = g_strdup_printf(
        "-device sgi-video-file-sink,id=rec,path=%s "
        "-device sgi-galileo,slot=widget-10,id=galileo,"
        "out-sink=rec", path);
    g_autofree gchar *data = NULL;
    gsize len = 0;
    const char *head = "YUV4MPEG2 W16 H16 F30000:1001 Ib A0:0 C422\n";
    unsigned hdr = strlen(head);
    uint64_t v, count;

    start(opt);
    dma_prepare(NULL);
    qtest_clock_step(qts, FIELD_NS);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    count = COUNT(v);
    g_assert_cmpuint(PARITY(v), ==, count & 1);
    gio_wr(CH1 + CH_STATUS, EVENTS(v));
    dma_request();

    /* A vertical marked as the start, a plain one, then the end alone. */
    qtest_clock_step(qts, FIELD_NS);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x23);
    g_assert_cmpuint(COUNT(v), ==, count + 1);
    g_assert_cmpuint(PARITY(v), ==, (count + 1) & 1);
    gio_wr(CH1 + CH_STATUS, EVENTS(v));
    qtest_clock_step(qts, FIELD_NS);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    g_assert_cmpuint(COUNT(v), ==, count + 2);
    gio_wr(CH1 + CH_STATUS, EVENTS(v));
    g_assert_cmpuint(sink_get("frames"), ==, 0);
    qtest_clock_step(qts, FIELD_NS / 2 + 1000);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x8);
    g_assert_cmpuint(COUNT(v), ==, count + 2);
    g_assert_cmpuint(v >> 16 & 0x7ff, ==, 8);
    g_assert_cmpuint(sink_get("frames"), ==, 1);
    gio_wr(CH1 + CH_STATUS, EVENTS(v));
    qtest_clock_step(qts, FIELD_NS / 2);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    g_assert_cmpuint(COUNT(v), ==, count + 3);
    for (int i = 0; i < 2000 &&
         sink_get("written") < hdr + 6 + 16 * 16 * 2; i++) {
        g_usleep(5000);
    }
    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    g_assert_cmpuint(len, ==, hdr + 6 + 16 * 16 * 2);
    g_assert_cmpmem(data, hdr, head, hdr);
    /* White is 235, clear is 16, in the first and the last row. */
    g_assert_cmphex((uint8_t)data[hdr + 6], ==, 235);
    g_assert_cmphex((uint8_t)data[hdr + 6 + 15 * 16], ==, 16);
    /* A red pixel in B G R order is 82. */
    g_assert_cmphex((uint8_t)data[hdr + 6 + 16], ==, 82);
    qtest_quit(qts);
    g_unlink(path);
    g_rmdir(dir);
}

/* A field with the bit enabled raises GIO line 4 once; writing back clears. */
static void test_dma_interrupt(void)
{
    uint64_t word;

    start_video();
    dma_prepare("irq");
    qtest_clock_step(qts, FIELD_NS - 3000);
    g_assert_false(sink_hit(&word));
    qtest_clock_step(qts, 4000);
    g_assert_true(sink_hit(&word));
    g_assert_cmphex(word, ==, 0x144);

    /* Held high, the line makes no second edge. */
    qtest_clock_step(qts, FIELD_NS);
    g_assert_false(sink_hit(&word));
    g_assert_cmphex(EVENTS(st()), ==, 0x1);
    gio_wr(CH1 + CH_STATUS, 0x1);
    g_assert_cmphex(EVENTS(st()), ==, 0);

    /* Cleared, the next field is a new edge; masked, it raises nothing. */
    qtest_clock_step(qts, FIELD_NS);
    g_assert_true(sink_hit(&word));
    gio_wr(CH1 + CH_STATUS, 0x1);
    gio_wr(CH1 + CH_INT_ENABLE, 0x2f1a);
    qtest_clock_step(qts, 3 * FIELD_NS);
    g_assert_false(sink_hit(&word));
    qtest_quit(qts);
}

/* An armed trigger marks the start with the trigger event. */
static void test_dma_trigger(void)
{
    start_video();
    dma_prepare(NULL);
    gio_wr(CH1 + CH_TRIGGER, 1);
    dma_request();
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(EVENTS(st()), ==, 0x33);
    qtest_quit(qts);
}

/* A transfer to an address the Bridge cannot reach raises an error bit. */
static void test_dma_error(void)
{
    start_video();
    dma_prepare(NULL);
    gio_wr(CH1 + CH_START_OFFSET, 0);
    gio_wr(CH1 + CH_PAGE_LIST, 0x40000000);
    for (unsigned i = 0; i < 2; i++) {
        qtest_clock_step(qts, FIELD_NS);
        gio_wr(CH1 + CH_STATUS, EVENTS(st()));
    }
    qtest_clock_step(qts, FIELD_NS / 2 + 1000);
    g_assert_cmphex(EVENTS(st()) & 0xff00, ==, 0x100);
    qtest_quit(qts);
}

/* The trigger abort drops a request that has not completed. */
static void test_dma_abort(void)
{
    start_video();
    dma_prepare(NULL);
    dma_request();
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(EVENTS(st()), ==, 0x23);
    gio_wr(CH1 + CH_TRIGGER, 2);
    gio_wr(CH1 + CH_TRIGGER, 0);
    gio_wr(CH1 + CH_STATUS, 0x23);
    qtest_clock_step(qts, 4 * FIELD_NS);
    g_assert_cmphex(EVENTS(st()) & 0x28, ==, 0);
    qtest_quit(qts);
}

/* A vertical held for the guest waits for the next field of the grid. */
static void test_dma_grid(void)
{
    uint64_t v, count;

    start_video();
    dma_prepare(NULL);
    qtest_clock_step(qts, FIELD_NS);
    gio_wr(CH1 + CH_STATUS, EVENTS(st()));
    dma_request();
    qtest_clock_step(qts, 2 * FIELD_NS);
    v = st();
    count = COUNT(v);
    gio_wr(CH1 + CH_STATUS, EVENTS(v));
    qtest_clock_step(qts, FIELD_NS / 2 + 1000);
    g_assert_cmphex(EVENTS(st()), ==, 0x8);

    /* The end is not cleared at the next field, so no vertical comes. */
    qtest_clock_step(qts, FIELD_NS / 2);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x8);
    g_assert_cmpuint(COUNT(v), ==, count);
    gio_wr(CH1 + CH_STATUS, 0x8);
    qtest_clock_step(qts, FIELD_NS / 4);
    g_assert_cmphex(EVENTS(st()), ==, 0);
    qtest_clock_step(qts, FIELD_NS);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    g_assert_cmpuint(COUNT(v), ==, count + 1);
    qtest_quit(qts);
}

/* Masking the vertical's enable bit does not let the end ride on it. */
static void test_dma_masked_end(void)
{
    start_video();
    dma_prepare(NULL);
    qtest_clock_step(qts, FIELD_NS);
    gio_wr(CH1 + CH_STATUS, EVENTS(st()));
    dma_request();
    qtest_clock_step(qts, FIELD_NS);
    gio_wr(CH1 + CH_STATUS, EVENTS(st()));
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(EVENTS(st()), ==, 0x1);
    gio_wr(CH1 + CH_INT_ENABLE, 0x2f1a);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(EVENTS(st()), ==, 0x1);
    gio_wr(CH1 + CH_STATUS, 0x1);
    qtest_clock_step(qts, FIELD_NS / 8);
    g_assert_cmphex(EVENTS(st()), ==, 0x8);
    qtest_quit(qts);
}

/* A migration between the second vertical and its end keeps both. */
static void test_dma_migrate(void)
{
    g_autofree char *dir = g_dir_make_tmp("galileo-migrate-XXXXXX", NULL);
    g_autofree char *sock = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    QTestState *from, *to;
    uint64_t v, count;

    start_video();
    from = qts;
    dma_prepare(NULL);
    qtest_clock_step(qts, FIELD_NS);
    gio_wr(CH1 + CH_STATUS, EVENTS(st()));
    dma_request();
    qtest_clock_step(qts, FIELD_NS);
    gio_wr(CH1 + CH_STATUS, EVENTS(st()));
    qtest_clock_step(qts, FIELD_NS);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    count = COUNT(v);

    to = launch("-device sgi-galileo,slot=widget-10,id=galileo "
                "-incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_migrate_resume(to);
    qts = to;

    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    g_assert_cmpuint(COUNT(v), ==, count);
    gio_wr(CH1 + CH_STATUS, 0x1);
    /* The end timer and the field timer both resume. */
    qtest_clock_step(qts, FIELD_NS / 2 + 1000);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x8);
    g_assert_cmpuint(COUNT(v), ==, count);
    gio_wr(CH1 + CH_STATUS, 0x8);
    qtest_clock_step(qts, FIELD_NS / 2);
    v = st();
    g_assert_cmphex(EVENTS(v), ==, 0x1);
    g_assert_cmpuint(COUNT(v), ==, count + 1);
    qtest_quit(from);
    qtest_quit(to);
    unlink(sock);
    g_assert_cmpint(rmdir(dir), ==, 0);
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
    { "field-phase", test_field_phase },
    { "reset", test_reset },
    { "dma-transfer", test_dma_transfer },
    { "dma-interrupt", test_dma_interrupt },
    { "dma-trigger", test_dma_trigger },
    { "dma-error", test_dma_error },
    { "dma-abort", test_dma_abort },
    { "dma-grid", test_dma_grid },
    { "dma-masked-end", test_dma_masked_end },
    { "dma-migrate", test_dma_migrate },
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
