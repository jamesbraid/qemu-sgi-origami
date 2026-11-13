/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DMediaPro DM2 (XT-DIGVID) XIO video board register-contract tests, on the
 * Octane2 and Octane. Every check cites a section of
 * docs/system/mips/sgi/hardware/xt-digvid.rst; the expectations come from
 * the page, not from the model.
 *
 * The GIF register file answers only after the GIF is configured, which is
 * 1.4 million clock pairs. Tests of that file use the model's host-only
 * x-test-gif-loaded switch; the "fpga-load" test runs the page's whole load
 * sequence instead.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sgi-launch.h"
#include "sgi-migrate.h"

#define XBOW        0x10000000ULL
#define BOARD       (XBOW + (10ULL << 24))
#define RAM         0x20000000ULL
#define PATH        "/machine/module0/dm2"
#define OCTANE2_CPU "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900," \
    "reset-mode=0x1aa683"

/* Window offsets ("Window map"). */
#define XG_ID           0x04
#define XG_STATUS       0x0c
#define XG_CONTROL      0x24
#define XG_DEST_UPPER   0x34
#define XG_DEST_LOWER   0x3c
#define XG_VECTORS      0x5c
#define XG_CONFIG       0x64
#define XG_ERROR        0x6c
#define XG_WATER        0x74
#define XG_MAP_ERR      0x94
#define XG_NIC          0x9c
#define MAP(i)          (0x100000 + 4 * (i))
#define FPGA            0x120000
#define GIF(off)        (0x800000 + (off))

#define CTL_OUT         GIF(0x14)
#define CTL_IN          GIF(0x3c)
#define START_OUT       GIF(0x1c)
#define START_IN        GIF(0x44)
#define RING_RESET_OUT  GIF(0x74)
#define RING_RESET_IN   GIF(0x64)
#define EN_IN           GIF(0x180c)
#define EN_OUT          GIF(0x200c)
#define TX(k)           GIF(0x3804 + 8 * (k))
#define RX(k)           GIF(0x3884 + 8 * (k))
#define DOORBELL        GIF(0x38ec)
#define RX_RELEASE      GIF(0x38f4)

/* Ring flags ("Status ring and interrupt acknowledgement"). */
#define F_VALID         0x1u
#define F_INPUT_OFF     0x4u
#define F_INPUT_CHANGE  0x20u
#define F_ONE           0x2u
#define F_PARITY        0x40u
#define F_FIELD         0x80u
#define F_DONE(k)       (0x100u << (k))
#define F_ABORTED(k)    (0x1000u << (k))
#define CTL_ABORT_BIT   0x10u
/* The first entry of a stream: field interrupt, parity 1, slot done. */
#define F_FIRST_DONE(k) (F_VALID | F_FIELD | F_PARITY | F_DONE(k))
#define F_TX            0x40000u
#define F_RX            0x80000u

/* Where the test puts the board's host pages. */
#define RING0_HOST      (RAM + 0x100000)
#define RING1_HOST      (RAM + 0x104000)
#define SLOT0_HOST      (RAM + 0x108000)
#define SLOT1_HOST      (RAM + 0x10c000)
#define IRQ_SINK        (RAM + 0x110000)

#define MAP_VALID_ENTRY(host)   (0x20000000u | (uint32_t)(((host) >> 14) << 2))

#define FIELD_NS        16683333    /* 1001/60000 s */
/* "Timestamps and buffer completion": a word 0 tick is 12 host ticks. */
#define UST_TICK_NS     960
/* A 525 frame: 858 * 525 samples at 13.5 MHz. */
#define FRAME_525_NS    33366667

static QTestState *qts;
static char *flash_path;
static unsigned ring_idx[2];

static uint32_t rd(uint32_t off)
{
    return qtest_readl(qts, BOARD + off);
}

static void wr(uint32_t off, uint32_t value)
{
    qtest_writel(qts, BOARD + off, value);
}

static void start_with(const char *machine, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    qts = qtest_initf(MEMTX_TEST_DEVICE "-M %s -m 256 %s -bios %s -S "
                      "-display none -vga none "
                      "-device sgi-xt-digvid,slot=widget-10,id=dm2 %s",
                      machine,
                      g_str_equal(machine, "octane2") ? "-cpu " OCTANE2_CPU
                                                      : "-cpu R10000",
                      quoted, extra);
    ring_idx[0] = ring_idx[1] = 0;
    /* The virtual clock the field timers run on stops with the machine. */
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
}

static void start(const char *extra)
{
    start_with("octane2", extra);
}

static void set_gif_loaded(void)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'x-test-gif-loaded','value':true}}", PATH);
}

static void set_prop(const char *name, uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
        "'value':%" PRIu64 "}}", PATH, name, value);
}

static uint32_t ram_l(uint64_t addr)
{
    uint8_t b[4];

    qtest_memread(qts, addr, b, sizeof(b));
    return ldl_be_p(b);
}

static void ram_set_l(uint64_t addr, uint32_t value)
{
    uint8_t b[4];

    stl_be_p(b, value);
    qtest_memwrite(qts, addr, b, sizeof(b));
}

static uint64_t ram_q(uint64_t addr)
{
    uint8_t b[8];

    qtest_memread(qts, addr, b, sizeof(b));
    return ldq_be_p(b);
}

/* What the host's interrupt controller would have been sent. */
static uint64_t irq_seen(void)
{
    return ram_q(IRQ_SINK);
}

static void irq_clear(void)
{
    uint8_t zero[8] = { 0 };

    qtest_memwrite(qts, IRQ_SINK, zero, sizeof(zero));
}

/*
 * Attach as the driver does ("Widget block and XG registers"): the
 * destination, the vectors and the error interrupt enable. Video 0 (the
 * input pipe) sends 0x41, video 1 (the output pipe) 0x42 and the error
 * interrupt 0x43.
 */
static void program_interrupts(uint32_t vector_enables)
{
    wr(XG_DEST_UPPER, 8u << 16);
    wr(XG_DEST_LOWER, IRQ_SINK);
    wr(XG_VECTORS, 0x41 | (0x42u << 9) | (0x43u << 18) | vector_enables);
    wr(XG_CONTROL, 0x4200 | 10 | (8u << 22) | (1u << 17));
}

/* The two status pages and one slot page per pipe, as the driver maps them. */
static void map_pages(void)
{
    wr(0x108000, MAP_VALID_ENTRY(RING0_HOST));
    wr(0x109000, MAP_VALID_ENTRY(RING1_HOST));
    /* Pipe 0 slot 1 start 0x1000, pipe 1 slot 2 start 0x2000. */
    wr(MAP(0x1400), MAP_VALID_ENTRY(SLOT0_HOST));
    wr(MAP(0x800), MAP_VALID_ENTRY(SLOT1_HOST));
}

static uint32_t ring_flags(unsigned pipe, unsigned index)
{
    return ram_l((pipe ? RING1_HOST : RING0_HOST) + index * 0x80 + 4);
}

static uint32_t ring_word0(unsigned pipe, unsigned index)
{
    return ram_l((pipe ? RING1_HOST : RING0_HOST) + index * 0x80);
}

/* The board counter's tick count from word 0 of one entry to the next. */
static uint32_t ust_ticks(uint32_t from, uint32_t to)
{
    return (to - from) & 0xffffff;
}

/* Read the next entry and clear VALID, as the driver's consumer does. */
static uint32_t ring_pop(unsigned pipe)
{
    uint64_t at = (pipe ? RING1_HOST : RING0_HOST) + ring_idx[pipe] * 0x80 + 4;
    uint32_t flags = ram_l(at);

    if (flags & F_VALID) {
        ram_set_l(at, flags & ~F_VALID);
        ring_idx[pipe] = (ring_idx[pipe] + 1) % 16;
    }
    return flags;
}

/* ---- Identity and the window ---- */

/* The one-wire master's slots: the Bridge's layout ("Identity"). */
static bool nic_slot(unsigned pulse, unsigned sample)
{
    uint32_t v;

    wr(XG_NIC, (pulse << 10) | (sample << 2));
    v = rd(XG_NIC);
    g_assert_cmphex(v & 2, ==, 2);
    return v & 1;
}

static void nic_write_byte(uint8_t byte)
{
    for (unsigned i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            nic_slot(6, 13);
        } else {
            nic_slot(80, 30);
        }
    }
}

static uint8_t nic_read_byte(void)
{
    uint8_t byte = 0;

    for (unsigned i = 0; i < 8; i++) {
        byte |= nic_slot(6, 13) << i;
    }
    return byte;
}

static void test_identity(void)
{
    uint32_t id;
    uint8_t rec[64];

    start("");
    /* "Identity" and "Widget block and XG registers": the word, 0x04. */
    id = rd(XG_ID);
    g_assert_cmphex(id, ==, 0x0c102555);
    g_assert_cmphex(id >> 28, ==, 0);
    g_assert_cmphex((id >> 12) & 0xffff, ==, 0xc102);
    g_assert_cmpuint((id >> 1) & 0x7ff, ==, 682);
    g_assert_cmphex(id & 1, ==, 1);
    /* The status word reads zero (bit 5 clear); the configuration 0x09. */
    g_assert_cmphex(rd(XG_STATUS), ==, 0);
    g_assert_cmphex(rd(XG_CONFIG), ==, 0x09);

    /* The one-wire part behind 0x9c: a DS2505-family record, Skip ROM. */
    g_assert_false(nic_slot(520, 65));
    nic_write_byte(0xcc);
    nic_write_byte(0xf0);
    nic_write_byte(0x00);
    nic_write_byte(0x00);
    /* A DS2505-class part echoes no header check: data starts at once. */
    for (unsigned i = 0; i < sizeof(rec); i++) {
        rec[i] = nic_read_byte();
    }
    g_assert_cmpmem(rec + 48, 9, "XT-DIGVID", 9);
    g_assert_cmpmem(rec + 21, 9, "030-1653-", 9);
    g_assert_cmpmem(rec + 35, 3, "002", 3);
    g_assert_cmpmem(rec + 1, 3, "EMU", 3);
    /* The revision text is the model's letter G; nothing validates it. */
    g_assert_cmpmem(rec + 38, 1, "G", 1);
    qtest_quit(qts);
}

/*
 * The part behind 0x9c is a DS2505 (family 0x0b), so Read Status (AAh) at
 * 0100h answers eight unprogrammed 0xff bytes and then a CRC-16, low byte
 * first, stored complemented. DS2505 datasheet, "READ STATUS [AAH]": the
 * first page's CRC covers the command, both address bytes and the page's
 * data bytes; a later page's covers only its own bytes. For aa 00 01 and
 * eight ff, the generator holds 0xce6f, sent as 0x3190; eight ff alone give
 * 0x7bbe. A DS2502 model answers a CRC-8 echo (0xc2) in the first data
 * position instead, which a driver's CRC-16 check rejects.
 */
static void test_read_status(void)
{
    start("");
    g_assert_false(nic_slot(520, 65));
    nic_write_byte(0xcc);
    nic_write_byte(0xaa);
    nic_write_byte(0x00);
    nic_write_byte(0x01);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(nic_read_byte(), ==, 0xff);
    }
    g_assert_cmphex(nic_read_byte(), ==, 0x90);
    g_assert_cmphex(nic_read_byte(), ==, 0x31);
    /* The next page: its CRC starts cleared, so no header is in it. */
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(nic_read_byte(), ==, 0xff);
    }
    g_assert_cmphex(nic_read_byte(), ==, 0xbe);
    g_assert_cmphex(nic_read_byte(), ==, 0x7b);
    qtest_quit(qts);
}

/*
 * "Window map": every access is a word; halfword and byte accesses are
 * refused. A 64-bit access reaches the device as two words, which the
 * memory API does before the device sees it, so it is not refused.
 */
static void test_widths(void)
{
    uint64_t v;

    start("");
    g_assert_cmpuint(memtx_read(qts, BOARD + XG_ID, &v, 4), ==, 0);
    g_assert_cmpuint(memtx_read(qts, BOARD + XG_ID, &v, 2), !=, 0);
    g_assert_cmpuint(memtx_read(qts, BOARD + XG_ID, &v, 1), !=, 0);
    g_assert_cmpuint(memtx_write(qts, BOARD + XG_CONTROL, 0xffff, 2), !=, 0);
    g_assert_cmpuint(memtx_write(qts, BOARD + XG_CONTROL, 0xff, 1), !=, 0);
    /* A processor's 64-bit load or store keeps its width and faults. */
    g_assert_cmpuint(memtx_load(qts, BOARD, &v, 8), !=, 0);
    g_assert_cmpuint(memtx_store(qts, BOARD + XG_CONTROL, 0xff, 8), !=, 0);
    g_assert_cmphex(rd(XG_CONTROL), ==, 0);
    qtest_quit(qts);
}

/*
 * "Window map": any other offset reads zero, ignores a write and is
 * logged as unimplemented; "Widget block and XG registers": a store of zero
 * to 0x6c is the module's delay and is not logged.
 */
static void test_unimplemented_logged(void)
{
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;
    int fd = g_file_open_tmp("octane-xt-digvid-log-XXXXXX", &log_path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d unimp -D %s", log_path);
    start(extra);
    g_assert_cmphex(rd(0x400), ==, 0);
    wr(0x400, 0xffffffff);
    g_assert_cmphex(rd(0x400), ==, 0);
    wr(XG_ERROR, 0);
    wr(0x00, 1);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "read of the window offset 0x400"));
    g_assert_nonnull(strstr(log, "write of the window offset 0x400"));
    g_assert_nonnull(strstr(log, "write of the widget block offset 0x0 "));
    g_assert_null(strstr(log, "offset 0x6c"));
    g_assert_cmpint(unlink(log_path), ==, 0);
}

/* ---- Widget block and XG registers ---- */

static void test_xg_registers(void)
{
    start("");
    /* Control keeps all 32 bits. */
    wr(XG_CONTROL, 0xdeadbeef);
    g_assert_cmphex(rd(XG_CONTROL), ==, 0xdeadbeef);
    /* The destination pair and the vectors are stored whole. */
    wr(XG_DEST_UPPER, 0x00081234);
    wr(XG_DEST_LOWER, 0x20110000);
    wr(XG_VECTORS, 0x0a5a5a5a);
    g_assert_cmphex(rd(XG_DEST_UPPER), ==, 0x00081234);
    g_assert_cmphex(rd(XG_DEST_LOWER), ==, 0x20110000);
    g_assert_cmphex(rd(XG_VECTORS), ==, 0x0a5a5a5a);
    /* The water marks store bits 27:0. */
    wr(XG_WATER, 0xffffffff);
    g_assert_cmphex(rd(XG_WATER), ==, 0x0fffffff);
    /* Read only: identification, status, configuration, map error address. */
    wr(XG_ID, 0);
    wr(XG_STATUS, 0xffffffff);
    wr(XG_CONFIG, 0);
    wr(XG_MAP_ERR, 0xffffffff);
    g_assert_cmphex(rd(XG_ID), ==, 0x0c102555);
    g_assert_cmphex(rd(XG_STATUS), ==, 0);
    g_assert_cmphex(rd(XG_CONFIG), ==, 0x09);
    g_assert_cmphex(rd(XG_MAP_ERR), ==, 0);
    /* The Crosstalk block's slots the software never touches store. */
    for (unsigned i = 0; i < 6; i++) {
        static const uint32_t slots[] = { 0x14, 0x1c, 0x2c, 0x44, 0x4c, 0x54 };

        g_assert_cmphex(rd(slots[i]), ==, 0);
        wr(slots[i], 0x11111111 * (i + 1));
        g_assert_cmphex(rd(slots[i]), ==, 0x11111111 * (i + 1));
    }
    qtest_quit(qts);
}

/* ---- Map RAM ---- */

static void test_map_ram(void)
{
    start("");
    /* "Map RAM": one word per 16 KiB page at 0x100000 + 4 * page. */
    g_assert_cmphex(rd(MAP(0)), ==, 0);
    g_assert_cmphex(rd(MAP(32767)), ==, 0);
    wr(MAP(0), 0x2000abcd);
    wr(MAP(1), 0x10000001);
    wr(MAP(32767), 0x3ffffffc);
    g_assert_cmphex(rd(MAP(0)), ==, 0x2000abcd);
    g_assert_cmphex(rd(MAP(1)), ==, 0x10000001);
    g_assert_cmphex(rd(MAP(2)), ==, 0);
    g_assert_cmphex(rd(MAP(32767)), ==, 0x3ffffffc);
    /* The last entry's neighbour is the programming register, not an entry. */
    g_assert_cmphex(rd(0x11fffc), ==, 0x3ffffffc);
    g_assert_cmphex(rd(FPGA), ==, 0);
    /* "Model choices": entries reset to zero. */
    qtest_system_reset(qts);
    g_assert_cmphex(rd(MAP(0)), ==, 0);
    g_assert_cmphex(rd(MAP(32767)), ==, 0);
    qtest_quit(qts);
}

/* ---- FPGA programming ---- */

typedef struct Fpga {
    const char *name;
    unsigned kind;
    uint32_t bytes;
    uint32_t select;
    uint32_t port;
    uint32_t done;
    uint32_t init;
} Fpga;

/* "The programming register": the table the module carries. */
static const Fpga gif_fpga = { "GIF", 1, 179233, 0x00, 0x0000, 0x1, 0x2 };
static const Fpga fpgas[] = {
    { "CLINK", 0, 35428, 0x40, 0x3800, 0x4000, 0x8000 },
    { "HDOC", 0, 104066, 0x08, 0x2000, 0x1000, 0x2000 },
    { "OXCC", 2, 318256, 0x20, 0x3000, 0x0100, 0x0200 },
    { "unpacker", 0, 104066, 0x02, 0x1000, 0x0010, 0x0020 },
    { "VIF", 0, 104066, 0x04, 0x1800, 0x0400, 0x0800 },
    { "packer", 0, 104066, 0x01, 0x0800, 0x0004, 0x0008 },
    { "IXCC", 2, 318256, 0x10, 0x2800, 0x0040, 0x0080 },
};

/* "GIF load", steps 1 to 7. */
static void load_gif(uint32_t bytes)
{
    unsigned polls = 0;

    wr(FPGA, 0x300);
    wr(FPGA, 0x100);
    wr(FPGA, 0x300);
    /* Step 2: DONE reads 0. */
    g_assert_cmphex(rd(FPGA) & 1, ==, 0);
    /* Step 3: INIT reads 1 by the first read after the strobe is high. */
    g_assert_cmphex(rd(FPGA) & 2, ==, 2);
    /* Step 4: one clock pair with data 0. */
    wr(FPGA, 0x200);
    wr(FPGA, 0x300);
    /* Step 5: each byte eight times, as the module sends it. */
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t b = (i * 7 + 3) & 0xff;

        for (unsigned bit = 0; bit < 8; bit++) {
            wr(FPGA, b | 0x200);
            wr(FPGA, b | 0x300);
        }
    }
    /* Step 6: INIT is still 1. */
    g_assert_cmphex(rd(FPGA) & 2, ==, 2);
    /* Step 7: ten more pairs with data 0, then DONE reads 1. */
    for (unsigned i = 0; i < 10; i++) {
        wr(FPGA, 0x200);
        wr(FPGA, 0x300);
    }
    while (!(rd(FPGA) & 1)) {
        g_assert_cmpuint(++polls, <, 10);
    }
}

/* "Loading the other seven", steps 2 to 7, at first load. */
static void load_other(const Fpga *f)
{
    uint32_t status = GIF(0x4 + f->port);
    uint32_t data = 0x880004 + f->port;
    unsigned polls = 0;

    wr(GIF(0x0c), f->select | 0x80);
    wr(GIF(0x0c), f->select);
    wr(GIF(0x0c), f->select | 0x80);
    /* Step 3: DONE reads 0 after the pulse; step 4: INIT reads 1. */
    g_assert_cmphex(rd(FPGA) & f->done, ==, 0);
    g_assert_cmphex(rd(FPGA) & f->init, ==, f->init);
    for (uint32_t i = 0; i < f->bytes; i++) {
        if (f->kind == 0) {
            /* Step 5: bit 7 reads 1 on every read of a kind 0 port. */
            g_assert_cmphex(rd(status) & 0x80, ==, 0x80);
            wr(data, i & 0xff);
        } else {
            wr(status, i & 0xff);
        }
        if (i == f->bytes - 2) {
            /* Step 6: INIT is still 1 before the last byte. */
            g_assert_cmphex(rd(FPGA) & f->init, ==, f->init);
            g_assert_cmphex(rd(FPGA) & f->done, ==, 0);
        }
    }
    while (!(rd(FPGA) & f->done)) {
        g_assert_cmpuint(++polls, <, 10);
    }
}

static void assert_all_done(unsigned count)
{
    uint32_t word = rd(FPGA);

    g_assert_cmphex(word & gif_fpga.done, ==, gif_fpga.done);
    for (unsigned i = 0; i < count; i++) {
        g_assert_cmphex(word & fpgas[i].done, ==, fpgas[i].done);
        g_assert_cmphex(word & fpgas[i].init, ==, fpgas[i].init);
    }
}

/*
 * "Load order and reset": the GIF, then CLINK, HDOC, OXCC, unpacker, VIF,
 * packer and IXCC, with the page's polls. The register file and the colour
 * converter port conflict (a converter's configuration byte goes to the
 * address of its first register) is part of this: the register stores
 * nothing while the converter loads and works as a register afterwards.
 */
static void test_fpga_load(void)
{
    start("");
    /* Before any load nothing is done; "Access to the GIF file before DONE". */
    g_assert_cmphex(rd(FPGA), ==, 0);
    wr(GIF(0x04), 0x1234);
    g_assert_cmphex(rd(GIF(0x84)), ==, 0);
    load_gif(gif_fpga.bytes);
    g_assert_cmphex(rd(FPGA) & 3, ==, 3);
    /* The file answers now: the identification, then the register. */
    g_assert_cmphex(rd(GIF(0x84)), ==, 0x47494601);
    g_assert_cmphex(rd(GIF(0x04)), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(fpgas); i++) {
        load_other(&fpgas[i]);
        assert_all_done(i + 1);
    }
    /* A colour converter's first register stores after its load. */
    wr(GIF(0x3004), 0x5a5a);
    g_assert_cmphex(rd(GIF(0x3004)), ==, 0x5a5a);
    qtest_quit(qts);
}

/*
 * "GIF load": a stream shorter than 8 * 179233 clocks leaves DONE clear
 * and INIT held at 1; the strobe's low period starts the count again.
 */
static void test_fpga_short_stream(void)
{
    start("");
    wr(FPGA, 0x300);
    wr(FPGA, 0x100);
    wr(FPGA, 0x300);
    for (unsigned i = 0; i < 5000; i++) {
        wr(FPGA, 0x200);
        wr(FPGA, 0x300);
    }
    g_assert_cmphex(rd(FPGA), ==, 0x2);
    g_assert_cmphex(rd(GIF(0x84)), ==, 0);
    /* The strobe's low period restarts the count and clears DONE. */
    wr(FPGA, 0x100);
    wr(FPGA, 0x300);
    g_assert_cmphex(rd(FPGA), ==, 0x2);
    qtest_quit(qts);
}

/*
 * "Loading the other seven": a reload at any time after the first load,
 * reading nothing from 0x120000, leaves the GIF and the other FPGAs'
 * registers alone.
 */
static void test_fpga_reload(void)
{
    const Fpga *clink = &fpgas[0];

    start("");
    set_gif_loaded();
    wr(GIF(0x800), 0);
    wr(GIF(0x804), 0x1f0008);
    wr(GIF(0x3804), 0x1234);
    wr(GIF(0x0c), clink->select | 0x80);
    wr(GIF(0x0c), clink->select);
    wr(GIF(0x0c), clink->select | 0x80);
    for (uint32_t i = 0; i < clink->bytes; i++) {
        g_assert_cmphex(rd(GIF(0x3804)) & 0x80, ==, 0x80);
        wr(0x883804, i & 0xff);
    }
    g_assert_cmphex(rd(FPGA) & clink->done, ==, clink->done);
    g_assert_cmphex(rd(FPGA) & 1, ==, 1);
    g_assert_cmphex(rd(GIF(0x804)), ==, 0x1f0008);
    qtest_quit(qts);
}

/* ---- GIF register file ---- */

typedef struct MaskRow {
    uint32_t off;
    uint32_t mask;
} MaskRow;

/* "Core and the two pipes" and the block tables: window offsets and masks. */
static const MaskRow masks[] = {
    { 0x800004, 0xffffffff }, { 0x80000c, 0xff },
    { 0x80001c, 0xfffff8 }, { 0x800024, 0xfffff8 },
    { 0x80002c, 0x3ffffff }, { 0x800034, 0xfff },
    { 0x800044, 0xfffff8 }, { 0x80004c, 0xfffff8 },
    { 0x800054, 0x3ffffff }, { 0x80005c, 0xfff },
    { 0x80008c, 0xffffffff },
    { 0x800824, 0x5 }, { 0x80101c, 0xffd },
    { 0x801804, 0x3fff }, { 0x80180c, 0x3 }, { 0x80182c, 0xff },
    { 0x801884, 0x3fffff }, { 0x80188c, 0x3fffff },
    { 0x802004, 0x3ffffff }, { 0x80200c, 0x1 }, { 0x802014, 0x7fffff },
    { 0x80201c, 0xffffff }, { 0x802024, 0xffffff },
    { 0x80202c, 0x3fffff }, { 0x802034, 0x3fffff }, { 0x80203c, 0x3fffff },
    { 0x802044, 0x3fffff }, { 0x80204c, 0x3fffff },
    { 0x802054, 0xffffff }, { 0x80205c, 0x7fffff }, { 0x802064, 0x7fffff },
    { 0x80206c, 0x7fffff }, { 0x802074, 0xffffff }, { 0x80207c, 0xffffff },
    { 0x802084, 0xffffff },
    { 0x803864, 0xffff }, { 0x80386c, 0xff }, { 0x803874, 0x1ff },
    { 0x8038e4, 0x1 },
};

/* Registers the page lists with no mask, which store every bit. */
static const uint32_t plain[] = {
    0x800804, 0x80080c, 0x800814, 0x800844, 0x80084c, 0x800854, 0x801004,
    0x80100c, 0x801044, 0x80104c, 0x801814, 0x801894, 0x8020b4, 0x8020ec,
    0x802814, 0x80285c, 0x803014, 0x80306c, 0x803044,
};

/*
 * "Reset and storage": a register stores only the bits of its mask, reads
 * zero until written, and a read of what was written gives write & mask.
 */
static void test_gif_masks(void)
{
    start("");
    set_gif_loaded();
    for (unsigned i = 0; i < G_N_ELEMENTS(masks); i++) {
        g_assert_cmphex(rd(masks[i].off), ==, 0);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(masks); i++) {
        wr(masks[i].off, 0xffffffff);
        g_assert_cmphex(rd(masks[i].off), ==, masks[i].mask);
        wr(masks[i].off, 0x5a5a5a5a);
        g_assert_cmphex(rd(masks[i].off), ==, 0x5a5a5a5a & masks[i].mask);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(plain); i++) {
        g_assert_cmphex(rd(plain[i]), ==, 0);
        wr(plain[i], 0xa5c3f00f);
        g_assert_cmphex(rd(plain[i]), ==, 0xa5c3f00f);
    }
    /* Transmit halfwords keep 16 bits; the receive words are read only. */
    for (unsigned k = 0; k < 12; k++) {
        wr(TX(k), 0xffffffff);
        g_assert_cmphex(rd(TX(k)), ==, 0xffff);
        wr(RX(k), 0xffffffff);
        g_assert_cmphex(rd(RX(k)), ==, 0);
    }
    /* The identification is read only; an unlisted register is logged and 0. */
    wr(GIF(0x84), 0);
    g_assert_cmphex(rd(GIF(0x84)), ==, 0x47494601);
    wr(GIF(0x94), 0xffffffff);
    g_assert_cmphex(rd(GIF(0x94)), ==, 0);
    /* The lookup table data port accepts writes and keeps nothing. */
    for (unsigned i = 0; i < 16; i++) {
        wr(GIF(0x280c), 0x1000 + i);
        wr(GIF(0x300c), 0x2000 + i);
    }
    g_assert_cmphex(rd(GIF(0x280c)), ==, 0);
    g_assert_cmphex(rd(GIF(0x300c)), ==, 0);
    /* Reset returns every register to zero. */
    qtest_system_reset(qts);
    set_gif_loaded();
    for (unsigned i = 0; i < G_N_ELEMENTS(masks); i++) {
        g_assert_cmphex(rd(masks[i].off), ==, 0);
    }
    qtest_quit(qts);
}

/* "DMA control word": the latches, the abort and the stored bits. */
static void test_dma_control_word(void)
{
    start("");
    set_gif_loaded();
    /* The diagnostic's stored bits read back; the pipes' masks differ. */
    wr(CTL_OUT, 0x1e7fe0);
    g_assert_cmphex(rd(CTL_OUT), ==, 0x1e7fe0);
    wr(CTL_IN, 0x1e7fe0);
    g_assert_cmphex(rd(CTL_IN), ==, 0x7fe0);
    /* The interrupt enable bit is stored beside the mask. */
    wr(CTL_IN, 0x10000);
    g_assert_cmphex(rd(CTL_IN), ==, 0x10000);
    /* Valid bits are latches: two writes with one bit each leave both armed. */
    wr(CTL_OUT, 0x1);
    wr(CTL_OUT, 0x4);
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0x5);
    /* A valid bit for an armed slot is a no-op. */
    wr(CTL_OUT, 0x1);
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0x5);
    wr(CTL_OUT, 0x0);
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0x5);
    /* The abort bit clears the latches and is not kept ("Abort"). */
    wr(CTL_OUT, 0x4);
    wr(CTL_OUT, 0x10);
    g_assert_cmphex(rd(CTL_OUT), ==, 0);
    wr(CTL_OUT, 0);
    g_assert_cmphex(rd(CTL_OUT), ==, 0);
    qtest_quit(qts);
}

/* ---- DMA, the status ring and interrupts ---- */

/* Arm one slot of the output pipe the way the driver does. */
static void arm_out(unsigned slot, uint32_t start_offset)
{
    wr(RING_RESET_OUT, 0);
    wr(START_OUT, start_offset);
    wr(CTL_OUT, (1u << slot) | 0xea040 | 0x10000);
}

/*
 * "Slot completion", "The data transfer" and "Status ring": an armed slot
 * completes on a field boundary with a DONE entry, VALID last, and the
 * pipe's video interrupt, a write of the vector to the destination.
 */
static void test_dma_output_field(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    arm_out(1, 0x1000);
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0x2);
    /* Nothing before the field boundary. */
    qtest_clock_step(qts, FIELD_NS - 1);
    g_assert_cmphex(ring_flags(0, 0), ==, 0);
    g_assert_cmphex(irq_seen(), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(ring_flags(0, 0), ==, F_FIRST_DONE(1));
    /* Word 0 is the board counter, zero at reset, a field's ticks on. */
    g_assert_cmpuint(ring_word0(0, 0), ==, FIELD_NS / UST_TICK_NS);
    /* Video 1 belongs to the output pipe; the packet is vector | 0x100. */
    g_assert_cmphex(irq_seen(), ==, 0x142);
    /* The latch cleared on completion. */
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0);
    /* No slot armed and no enable: no further entries. */
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_flags(0, 1), ==, 0);
    qtest_quit(qts);
}

/* Arm one slot of the input pipe. */
static void arm_in(unsigned slot, uint32_t start_offset)
{
    wr(RING_RESET_IN, 0);
    wr(START_IN, start_offset);
    wr(CTL_IN, (1u << slot) | 0xa040 | 0x10000);
}

/*
 * The input pipe produces fields only with an input signal ("Genlock and
 * lock reporting", last bullet), and reports on video 0 and the DONE bit of
 * the slot ("Interrupts").
 */
static void test_dma_input_field(void)
{
    start("-global sgi-xt-digvid.x-vbob-input-timings=0x010101010105 "
          "-global sgi-xt-digvid.x-vbob-present=1");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    arm_in(2, 0x2000);
    g_assert_cmphex(rd(CTL_IN) & 0xf, ==, 0x4);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_flags(1, 0), ==, F_FIRST_DONE(2));
    g_assert_cmphex(irq_seen(), ==, 0x141);
    qtest_quit(qts);

    /* Without an input signal the armed slot never completes. */
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    arm_in(2, 0x2000);
    qtest_clock_step(qts, 10 * FIELD_NS);
    g_assert_cmphex(ring_flags(1, 0), ==, 0);
    g_assert_cmphex(rd(CTL_IN) & 0xf, ==, 0x4);
    qtest_quit(qts);
}

/*
 * "Status ring": slots complete in the order they were armed, one per
 * field, entries advance and wrap, and field parity is the low bit of the
 * field interrupts raised.
 */
static void test_dma_order_and_wrap(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    wr(MAP(0x1800), MAP_VALID_ENTRY(SLOT0_HOST));
    wr(MAP(0x1c00), MAP_VALID_ENTRY(SLOT0_HOST));
    wr(RING_RESET_OUT, 0);
    wr(START_OUT, 0x1000);
    wr(CTL_OUT, 0x8 | 0xea040 | 0x10000);
    wr(CTL_OUT, 0x2 | 0xea040 | 0x10000);
    wr(CTL_OUT, 0x4 | 0xea040 | 0x10000);
    wr(EN_OUT, 1);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_pop(0), ==, F_FIRST_DONE(3));
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_FIELD | F_DONE(1));
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_pop(0), ==, F_FIRST_DONE(2));
    /* The video enable keeps the pipe running with nothing armed. */
    for (unsigned i = 0; i < 20; i++) {
        qtest_clock_step(qts, FIELD_NS);
        g_assert_cmphex(ring_pop(0) & ~F_PARITY, ==, F_VALID | F_FIELD);
    }
    /* The 23rd field wrote entry 6: its word 0 is 23 field periods on. */
    g_assert_cmpuint(ust_ticks(ring_word0(0, 5), ring_word0(0, 6)), >=,
                     FIELD_NS / UST_TICK_NS);
    g_assert_cmpuint(ust_ticks(ring_word0(0, 5), ring_word0(0, 6)), <=,
                     FIELD_NS / UST_TICK_NS + 1);
    /* A pipe reset word restarts the parity and leaves the counter alone. */
    wr(GIF(0x7c), 0);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmpuint(ust_ticks(ring_word0(0, (ring_idx[0] + 15) % 16),
                               ring_word0(0, ring_idx[0])), <=,
                     FIELD_NS / UST_TICK_NS + 1);
    /*
     * ... and the parity: 23 fields leave the count odd, which would not
     * carry it, so the flag here is the reset's.
     */
    g_assert_cmphex(ring_flags(0, ring_idx[0]) & F_PARITY, ==, F_PARITY);
    /* Clearing the video enable stops the fields. */
    wr(EN_OUT, 0);
    qtest_clock_step(qts, 4 * FIELD_NS);
    g_assert_cmphex(ring_flags(0, (ring_idx[0] + 1) % 16) & F_VALID, ==, 0);
    qtest_quit(qts);
}

/*
 * "Interrupts": the vector word's enable bit gates the video interrupt, and
 * the entry is written either way.
 */
static void test_interrupt_gating(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    /* Video 1 enabled alone, then video 0 alone. */
    program_interrupts(0);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_flags(0, 0), ==, F_FIRST_DONE(1));
    g_assert_cmphex(irq_seen(), ==, 0);
    g_assert_cmphex(ring_pop(0), ==, F_FIRST_DONE(1));
    wr(XG_VECTORS, 0x41 | (0x42u << 9) | (0x43u << 18) | 0x20000);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    /*
     * The first field interrupt of a stream carries the parity bit; the
     * second does not ("Pipe start, counters and timing").
     */
    g_assert_cmphex(ring_flags(0, 0), ==, F_VALID | F_FIELD | F_DONE(1));
    g_assert_cmphex(irq_seen(), ==, 0x142);
    /* Control bit 16 is stored and does not gate. */
    g_assert_cmphex(rd(CTL_OUT) & 0x10000, ==, 0x10000);
    qtest_quit(qts);
}

/*
 * "Status ring": a head that still has VALID set holds the entry until the
 * host clears it, then writes it and raises the interrupt.
 */
static void test_ring_overrun_holds(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    ram_set_l(RING0_HOST + 4, F_VALID);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_word0(0, 0), ==, 0);
    g_assert_cmphex(irq_seen(), ==, 0);
    qtest_clock_step(qts, 5 * 1000 * 1000);
    g_assert_cmphex(ring_word0(0, 0), ==, 0);
    ram_set_l(RING0_HOST + 4, 0);
    qtest_clock_step(qts, 1000 * 1000);
    g_assert_cmphex(ring_flags(0, 0), ==, F_FIRST_DONE(1));
    /* The entry carries the counter of the field it reports, not its write. */
    g_assert_cmpuint(ring_word0(0, 0), ==, FIELD_NS / UST_TICK_NS);
    g_assert_cmphex(irq_seen(), ==, 0x142);
    qtest_quit(qts);
}

/*
 * "Map RAM" and "Error cause bits": a translation through an invalid entry
 * raises cause bit 13, records the entry's byte offset in 0x94 and, with
 * control bit 17, raises the error interrupt. Write one clears the cause.
 */
static void test_invalid_map_entry(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    /* Only the error interrupt is enabled, so the packet seen is its. */
    program_interrupts(0);
    /* The slot's page is invalidated the driver's way. */
    wr(MAP(0x1400), 0x10000000);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(rd(XG_ERROR), ==, 1u << 13);
    g_assert_cmphex(rd(XG_MAP_ERR), ==, 0x1400 * 4);
    g_assert_cmphex(irq_seen(), ==, 0x143);
    /* The slot is posted aborted, not done, which ends the driver's drain. */
    g_assert_cmphex(ring_flags(0, 0), ==, F_VALID | F_FIELD | F_PARITY |
                    F_ABORTED(1));
    wr(XG_ERROR, 0);
    g_assert_cmphex(rd(XG_ERROR), ==, 1u << 13);
    wr(XG_ERROR, 1u << 13);
    g_assert_cmphex(rd(XG_ERROR), ==, 0);
    /* An entry with bit 29 clear is invalid; bit 17 clear, no interrupt. */
    irq_clear();
    wr(XG_CONTROL, 0x4200 | 10 | (8u << 22));
    wr(MAP(0x1400), 0x00000004);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(rd(XG_ERROR), ==, 1u << 13);
    g_assert_cmphex(irq_seen(), ==, 0);
    qtest_quit(qts);
}

/* ---- VBOB control channel ---- */

/* "Transmit": twelve halfword stores, then the doorbell. */
static void vbob_send(const uint8_t msg[24])
{
    for (unsigned k = 0; k < 12; k++) {
        wr(TX(k), (msg[2 * k] << 8) | msg[2 * k + 1]);
    }
    wr(DOORBELL, 1);
}

static void vbob_frame(uint8_t frame[24])
{
    for (unsigned k = 0; k < 12; k++) {
        uint32_t w = rd(RX(k));

        g_assert_cmphex(w >> 16, ==, 0);
        frame[2 * k] = w >> 8;
        frame[2 * k + 1] = w;
    }
}

static void vbob_message(unsigned pipe, uint8_t reg, bool write,
                         const uint8_t *body, unsigned n)
{
    uint8_t m[24] = { 0 };

    m[0] = n;
    m[1] = pipe;
    m[2] = reg | (write ? 0x80 : 0);
    if (body) {
        memcpy(m + 3, body, MIN(n - 2, 21));
    }
    vbob_send(m);
}

static void vbob_release(void)
{
    wr(RX_RELEASE, 1);
}

static void vbob_get_sync(uint8_t frame[24])
{
    vbob_message(0, 0x26, false, NULL, 2);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(frame);
    vbob_release();
}

static void vbob_setup(const char *extra)
{
    start(extra);
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
}

/* "Transmit": a set message is acknowledged in the ring of its pipe. */
static void test_vbob_ack(void)
{
    const uint8_t tv[] = { 1, 2 };
    const uint8_t route[] = { 3, 7 };

    vbob_setup("");
    vbob_message(0, 0x20, true, tv, 4);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, 0);
    g_assert_cmphex(irq_seen(), ==, 0x142);
    /* The pipe byte picks the ring: pipe 1 acks in the input ring. */
    irq_clear();
    vbob_message(1, 0x24, true, route, 4);
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, 0);
    g_assert_cmphex(irq_seen(), ==, 0x141);
    /* "Frame format": the driver refuses a pipe above 1; no ack comes. */
    vbob_message(2, 0x20, true, tv, 4);
    g_assert_cmphex(ring_pop(0), ==, 0);
    g_assert_cmphex(ring_pop(1), ==, 0);
    qtest_quit(qts);
}

/*
 * "Messages the virtual endpoint accepts": the routing get returns the
 * stored code in byte 4, the info get a frame with byte 0 set, and a frame
 * is held in the receive buffer until the host stores 1 to 0x8038f4.
 */
static void test_vbob_replies(void)
{
    const uint8_t route[] = { 3, 7 };
    const uint8_t dest[] = { 3 };
    uint8_t f[24];

    vbob_setup("");
    /* "Ports and routing": destination port in byte 3, source in byte 4. */
    vbob_message(0, 0x25, true, route, 4);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    vbob_message(0, 0x25, false, dest, 3);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[0], !=, 0);
    g_assert_cmpuint(f[2], ==, 0x25);
    g_assert_cmpuint(f[4], ==, 7);
    vbob_release();
    /* The info get's byte 1 is its index; 1 acks and replies in ring 1. */
    vbob_message(1, 0x40, false, NULL, 2);
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_RX);
    g_assert_cmphex(ring_pop(0), ==, 0);
    vbob_frame(f);
    g_assert_cmpuint(f[0], ==, 1);
    g_assert_cmpuint(f[1], ==, 1);
    g_assert_cmpuint(f[2], ==, 0x40);
    g_assert_cmpuint(f[3], ==, 0);
    qtest_quit(qts);
}

/*
 * "Receive": a second frame waits for the release. Two gets in a row give
 * the second reply only after the first is released.
 */
static void test_vbob_receive_release(void)
{
    uint8_t f[24];

    vbob_setup("");
    vbob_message(0, 0x26, false, NULL, 2);
    vbob_message(0, 0x25, false, NULL, 3);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    /* The second reply is not deposited yet. */
    g_assert_cmphex(ring_pop(0), ==, 0);
    vbob_frame(f);
    /* A sync-state reply is framed as 0xa6 with byte 3 bit 7 clear. */
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3] & 0x80, ==, 0);
    vbob_release();
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0x25);
    qtest_quit(qts);
}

/*
 * "Genlock and lock reporting": internal genlock is locked, with the
 * detected timing equal to the output timing; an external source locks when
 * a compatible reference is present (Table 5-3), and a change is reported
 * with a 0xa6 frame, the input signal with a 0xa7 frame and status flags.
 * The high nibble of message 0x21 names the HD side's source and the low
 * nibble the SD side's; a zero nibble leaves the other side alone.
 */
static void test_vbob_genlock(void)
{
    const uint8_t jack_a[] = { 0x90 };
    const uint8_t analog[] = { 0x0b };
    const uint8_t reset[] = { 'R', 'E', 'S', 'E', 'T' };
    uint8_t f[24];

    /* HD output 1080 59.94i (wire 7); jack A carries 720 59.94p (wire 5). */
    vbob_setup("-global sgi-xt-digvid.x-vbob-output-timing=7 "
               "-global sgi-xt-digvid.x-vbob-present=1 "
               "-global sgi-xt-digvid.x-vbob-input-timings=0x010101010105");
    vbob_get_sync(f);
    /* Both lock bits agree; each detector reports its own timing. */
    g_assert_cmpuint(f[3] & 3, ==, 3);
    g_assert_cmpuint(f[4], ==, 7);
    g_assert_cmpuint(f[5], ==, 0);

    /* 720 59.94p genlocks a 1080 59.94i output: selecting it stays locked. */
    vbob_message(0, 0x21, true, jack_a, 3);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, 0);
    vbob_get_sync(f);
    g_assert_cmpuint(f[3] & 3, ==, 3);
    g_assert_cmpuint(f[4], ==, 5);

    /* The reference goes away: HD unlocked, and an input change. */
    set_prop("x-vbob-present", 0);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3], ==, 0x80);
    g_assert_cmpuint(f[4], ==, 1);
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_INPUT_CHANGE | F_INPUT_OFF);
    /* The 0xa7 frame waits behind the held one. */
    g_assert_cmphex(ring_pop(1), ==, 0);
    vbob_release();
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa7);
    g_assert_cmpuint(f[3], ==, 0x80);
    /* The HD and SD input slots are bytes 4 and 6. */
    g_assert_cmpuint(f[4], ==, 1);
    g_assert_cmpuint(f[6], ==, 1);
    vbob_release();
    vbob_get_sync(f);
    g_assert_cmpuint(f[3] & 3, ==, 0);
    g_assert_cmpuint(f[4], ==, 1);

    /* It returns: HD locked (bit 1, timing in byte 4), input change alone. */
    set_prop("x-vbob-present", 1);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3], ==, 0x82);
    g_assert_cmpuint(f[4], ==, 5);
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_INPUT_CHANGE);
    vbob_release();
    g_assert_cmphex(ring_pop(1), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[4], ==, 5);
    vbob_release();

    /* The input-signal reply is framed 0xa7 with bit 7 clear: six slots. */
    vbob_message(0, 0x27, false, (const uint8_t[]){ 0 }, 2);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa7);
    g_assert_cmpuint(f[3] & 0x80, ==, 0);
    g_assert_cmpuint(f[4], ==, 5);
    for (unsigned i = 5; i < 10; i++) {
        g_assert_cmpuint(f[i], ==, 1);
    }
    vbob_release();

    /*
     * The SD side takes the analog reference (slot 5), which is absent: it
     * is unlocked (bit 0 clear), the HD side keeps its own state.
     */
    vbob_message(0, 0x21, true, analog, 3);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3], ==, 0x80);
    g_assert_cmpuint(f[5], ==, 1);
    vbob_release();

    /* RESET returns both sides to internal genlock: the SD side locks. */
    vbob_message(0, 0x47, true, reset, 7);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3], ==, 0x81);
    vbob_release();
    qtest_quit(qts);
}

/*
 * "Timing record": the host's message 0x22 sets the output timing, so the
 * internal detected timing and the field period follow the record: 720 60p
 * is 1650 samples by 750 lines at 74.25 MHz, 16.667 ms a frame.
 */
static void test_vbob_timing_record(void)
{
    const uint8_t record[] = {
        5, 0x06, 0x72, 0x05, 0x00, 0x02, 0xee, 0x02, 0xee, 0x02, 0xee,
        0, 0, 25, 0, 5,
    };
    uint8_t f[24];

    vbob_setup("");
    vbob_message(0, 0x22, true, record, 18);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    vbob_get_sync(f);
    g_assert_cmpuint(f[3] & 3, ==, 3);
    g_assert_cmpuint(f[4], ==, 6);
    wr(START_OUT, 0x1000);
    wr(CTL_OUT, 0x2 | 0xea040 | 0x10000);
    qtest_clock_step(qts, 16666665);
    g_assert_cmphex(ring_pop(0), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_FIELD | F_PARITY |
                    F_DONE(1));
    qtest_quit(qts);
}

/*
 * "Timestamps and buffer completion": the register at 0x80008c counts from the
 * value last stored, one tick per 960 ns, and a store of 0 is what zeroes
 * word 0 of the next entry's sample.
 */
static void test_ust_counter(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    qtest_clock_step(qts, 5 * UST_TICK_NS);
    wr(GIF(0x8c), 0);
    g_assert_cmpuint(rd(GIF(0x8c)), ==, 0);
    qtest_clock_step(qts, 1000 * UST_TICK_NS);
    g_assert_cmpuint(rd(GIF(0x8c)), ==, 1000);
    /* A field entry samples it: zeroed 1000 ticks before this field. */
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmpuint(ring_word0(0, 0), ==, 1000 + FIELD_NS / UST_TICK_NS);
    /* Word 0 keeps 24 bits; the register keeps the rest. */
    wr(GIF(0x8c), 0x1fffff0);
    qtest_clock_step(qts, 100 * UST_TICK_NS);
    g_assert_cmpuint(rd(GIF(0x8c)), ==, 0x1fffff0 + 100);
    qtest_quit(qts);
}

/*
 * "The data transfer" and "Timestamps and buffer completion": an interleaved
 * 525 frame is one buffer, one slot, so a slot completes on every second
 * field interrupt, each field posts an entry with its parity, and word 0
 * advances by the frame period (33,366,667 ns) between completions, to
 * within a counter tick.
 */
static void test_dma_525_frames(void)
{
    enum { FRAMES = 6 };
    uint32_t done_w0[FRAMES], field_w0[2 * FRAMES];

    start("-global sgi-xt-digvid.x-vbob-output-timing=3");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    arm_out(1, 0x1000);
    for (unsigned f = 0; f < FRAMES; f++) {
        for (unsigned half = 0; half < 2; half++) {
            unsigned n = 2 * f + half;
            uint32_t w0, flags;

            qtest_clock_step(qts, FIELD_NS);
            w0 = ring_word0(0, ring_idx[0]);
            flags = ring_pop(0);
            field_w0[n] = w0;
            /* The first field of a stream carries parity 1; they alternate. */
            if (half == 0) {
                g_assert_cmphex(flags, ==, F_VALID | F_FIELD | F_PARITY);
            } else {
                /* One slot per frame: done on the second field only. */
                g_assert_cmphex(flags, ==,
                                F_VALID | F_FIELD | F_ONE | F_DONE(1));
                done_w0[f] = w0;
                /* The next buffer, armed as the driver does. */
                wr(CTL_OUT, 0x2 | 0xea040 | 0x10000);
            }
        }
    }
    for (unsigned f = 1; f < FRAMES; f++) {
        uint64_t ns = (uint64_t)ust_ticks(done_w0[f - 1], done_w0[f]) *
                      UST_TICK_NS;

        g_assert_cmpuint(ns, >, FRAME_525_NS - UST_TICK_NS);
        g_assert_cmpuint(ns, <, FRAME_525_NS + UST_TICK_NS);
    }
    for (unsigned n = 1; n < 2 * FRAMES; n++) {
        uint64_t ns = (uint64_t)ust_ticks(field_w0[n - 1], field_w0[n]) *
                      UST_TICK_NS;

        g_assert_cmpuint(ns, >, FIELD_NS - UST_TICK_NS);
        g_assert_cmpuint(ns, <, FIELD_NS + UST_TICK_NS);
    }
    /* Armed once more than completed: one slot left, nothing stacked. */
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0x2);
    qtest_quit(qts);
}

/*
 * "Abort": a control write with bit 4 posts slot-aborted for each armed
 * incomplete slot of the pipe in arming order, one entry each.
 */
static void test_dma_abort(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    wr(RING_RESET_OUT, 0);
    wr(START_OUT, 0x1000);
    wr(CTL_OUT, 0x4 | 0xea040 | 0x10000);
    wr(CTL_OUT, 0x2 | 0xea040 | 0x10000);
    wr(CTL_OUT, 0xea040 | CTL_ABORT_BIT);
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_ABORTED(2));
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_ABORTED(1));
    g_assert_cmphex(ring_pop(0), ==, 0);
    /* Nothing is left to complete. */
    qtest_clock_step(qts, 4 * FIELD_NS);
    g_assert_cmphex(ring_pop(0), ==, 0);
    qtest_quit(qts);
}

/*
 * "Attach and reset": a widget reset, delivered as the Crossbow's link
 * reset, returns the XG block to zero and invalidates the map RAM; the GIF
 * stays configured with its registers.
 */
static void test_widget_reset(void)
{
    uint32_t link_reset = XBOW + 0x100 + (10 - 8) * 0x40 + 0x30 + 4;

    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    wr(GIF(0x2c), 0x1234);
    /* Raise a map RAM error, so the cause and its address are held. */
    wr(MAP(0x1400), 0x10000000);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(rd(XG_ERROR), ==, 1u << 13);
    g_assert_cmphex(rd(XG_MAP_ERR), ==, 0x1400 * 4);
    g_assert_cmpuint(memtx_writel(qts, link_reset, 0), ==, 0);
    qtest_clock_step(qts, 20);
    g_assert_cmphex(rd(XG_ERROR), ==, 0);
    g_assert_cmphex(rd(XG_MAP_ERR), ==, 0);
    g_assert_cmphex(rd(XG_CONTROL), ==, 0);
    g_assert_cmphex(rd(XG_DEST_LOWER), ==, 0);
    g_assert_cmphex(rd(XG_VECTORS), ==, 0);
    g_assert_cmphex(rd(MAP(0x1400)), ==, 0);
    g_assert_cmphex(rd(XG_ID), ==, 0x0c102555);
    g_assert_cmphex(rd(FPGA) & 1, ==, 1);
    g_assert_cmphex(rd(GIF(0x2c)), ==, 0x1234);
    qtest_quit(qts);
}

/*
 * "Error cause bits": changing the endpoint before the driver has mapped
 * the status pages posts nothing and raises no map RAM error.
 */
static void test_early_property_change(void)
{
    uint8_t f[24];

    start("");
    set_gif_loaded();
    program_interrupts(0x20100);
    set_prop("x-vbob-input-timings", 0x010101010105);
    set_prop("x-vbob-present", 1);
    g_assert_cmphex(rd(XG_ERROR), ==, 0);
    g_assert_cmphex(irq_seen(), ==, 0);
    /*
     * The frame the change would have sent had no ring to report it in, so
     * it did not take the receive buffer: a reply after mapping arrives.
     */
    map_pages();
    vbob_get_sync(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    qtest_quit(qts);
}

/* Release every frame the endpoint has sent and pop the rings they used. */
static void vbob_drain(void)
{
    for (unsigned i = 0; i < 8; i++) {
        bool rx = false;

        for (unsigned p = 0; p < 2; p++) {
            uint32_t flags;

            while ((flags = ring_pop(p))) {
                rx |= flags & F_RX;
            }
        }
        if (!rx) {
            return;
        }
        vbob_release();
    }
}

typedef struct GenlockCase {
    const char *name;
    const char *args;
    /* Message 0x22 body (16 bytes) first, or NULL for none. */
    const uint8_t *record;
    uint8_t source;
    unsigned lock_bits;
    uint8_t hd_code;
    uint8_t sd_code;
} GenlockCase;

/* "Message 0x22" bodies for the two standards' rows. */
static const uint8_t record_525[16] = {
    6, 0x03, 0x5a, 0x02, 0xd0, 0x02, 0x0d, 0x00, 0xf0, 0x01, 0x0a, 0x01, 0x06,
    16, 17, 1,
};

/*
 * "Genlock and lock reporting" and "Replies and status": each reference
 * slot, an incompatible reference (Table 5-3), and a positive SD lock. The
 * sync-state reply carries the lock bits of the latest record's side and
 * each detector's own timing in bytes 4 (HD) and 5 (SD).
 */
static void test_vbob_genlock_references(void)
{
    static const GenlockCase cases[] = {
        /* HD output 1080 59.94i (7); jack A holds 50i, which is no pair. */
        { "hd-jack-a-incompatible",
          "-global sgi-xt-digvid.x-vbob-output-timing=7 "
          "-global sgi-xt-digvid.x-vbob-present=1 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010101010110",
          NULL, 0x90, 0, 1, 0 },
        /* HD jack B (slot 1) holds 720 59.94p, a pair of 59.94i. */
        { "hd-jack-b",
          "-global sgi-xt-digvid.x-vbob-output-timing=7 "
          "-global sgi-xt-digvid.x-vbob-present=2 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010101010501",
          NULL, 0xa0, 3, 5, 0 },
        /* HD analog (slot 4) holds 1080 59.94i itself. */
        { "hd-analog",
          "-global sgi-xt-digvid.x-vbob-output-timing=7 "
          "-global sgi-xt-digvid.x-vbob-present=16 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010701010101",
          NULL, 0xb0, 3, 7, 0 },
        /* A reference timing that is present but whose slot flag is clear. */
        { "hd-jack-a-absent",
          "-global sgi-xt-digvid.x-vbob-output-timing=7 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010101010105",
          NULL, 0x90, 0, 1, 0 },
        /* SD output 525, from its record; slots 2, 3 and 5 are its jacks. */
        { "sd-jack-a",
          "-global sgi-xt-digvid.x-vbob-present=4 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010101030101",
          record_525, 0x09, 3, 0, 3 },
        { "sd-jack-b",
          "-global sgi-xt-digvid.x-vbob-present=8 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010103010101",
          record_525, 0x0a, 3, 0, 3 },
        { "sd-analog",
          "-global sgi-xt-digvid.x-vbob-present=32 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x030101010101",
          record_525, 0x0b, 3, 0, 3 },
        /* A 625 reference does not lock a 525 output. */
        { "sd-incompatible",
          "-global sgi-xt-digvid.x-vbob-present=4 "
          "-global sgi-xt-digvid.x-vbob-input-timings=0x010101040101",
          record_525, 0x09, 0, 0, 1 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        const GenlockCase *c = &cases[i];
        const uint8_t src[] = { c->source };
        uint8_t f[24];

        g_test_message("%s", c->name);
        vbob_setup(c->args);
        if (c->record) {
            vbob_message(0, 0x22, true, c->record, 18);
            vbob_drain();
        }
        vbob_message(0, 0x21, true, src, 3);
        vbob_drain();
        vbob_get_sync(f);
        g_assert_cmpuint(f[3] & 3, ==, c->lock_bits);
        g_assert_cmpuint(f[4], ==, c->hd_code);
        g_assert_cmpuint(f[5], ==, c->sd_code);
        qtest_quit(qts);
    }
}

/*
 * "Messages the virtual endpoint accepts", message 0x21: a zero nibble leaves
 * the other side's source as it was.
 */
static void test_vbob_nibbles(void)
{
    const uint8_t hd_a[] = { 0x90 };
    const uint8_t sd_analog[] = { 0x0b };
    const uint8_t hd_internal[] = { 0x80 };
    uint8_t f[24];

    vbob_setup("-global sgi-xt-digvid.x-vbob-output-timing=7 "
               "-global sgi-xt-digvid.x-vbob-present=1 "
               "-global sgi-xt-digvid.x-vbob-input-timings=0x010101010105");
    vbob_message(0, 0x21, true, hd_a, 3);
    vbob_drain();
    vbob_get_sync(f);
    g_assert_cmpuint(f[4], ==, 5);
    /* The SD message names the low nibble only: HD stays on jack A. */
    vbob_message(0, 0x21, true, sd_analog, 3);
    vbob_drain();
    vbob_get_sync(f);
    g_assert_cmpuint(f[3] & 3, ==, 3);
    g_assert_cmpuint(f[4], ==, 5);
    /* An HD internal message does not disturb the SD side's analog source. */
    vbob_message(0, 0x21, true, hd_internal, 3);
    vbob_drain();
    vbob_get_sync(f);
    g_assert_cmpuint(f[4], ==, 7);
    qtest_quit(qts);
}

typedef struct RecordCase {
    const char *name;
    uint8_t body[16];
    /* Which sync-reply byte holds the code, the code, and the field ns. */
    unsigned byte;
    uint8_t code;
    int64_t field_ns;
} RecordCase;

/*
 * "Timing record": a record is matched to a wire code and sets the field
 * period from its arithmetic: samples times lines over the luma clock,
 * halved unless progressive.
 */
static void test_vbob_timing_rows(void)
{
    static const RecordCase cases[] = {
        /* 1080 25 PsF has the bytes of 1080 50i, and reports as 50i (16). */
        { "1080-25psf", { 5, 0x0a, 0x50, 0x07, 0x80, 0x04, 0x65, 0x02, 0x1c,
                          0x02, 0x34, 0x02, 0x1c, 20, 20, 5 },
          4, 16, 20000000 },
        /* SD 525 at 13.5 MHz: 450450 pixels, 16.683 ms a field. */
        { "525", { 6, 0x03, 0x5a, 0x02, 0xd0, 0x02, 0x0d, 0x00, 0xf0,
                   0x01, 0x0a, 0x01, 0x06, 16, 17, 1 },
          5, 3, 16683333 },
        /* 59.94 family, progressive: 1080 29.97p, 74.25 MHz / 1.001. */
        { "1080-29.97p", { 5, 0x08, 0x98, 0x07, 0x80, 0x04, 0x65, 0x04, 0x38,
                           0x04, 0x65, 0x00, 0x00, 41, 0, 4 },
          4, 9, 33366666 },
        /* No row has 26 for byte 16: no code, but the arithmetic stands. */
        { "unmatched", { 5, 0x06, 0x72, 0x05, 0x00, 0x02, 0xee, 0x02, 0xd0,
                         0x02, 0xee, 0x00, 0x00, 26, 0, 5 },
          4, 0, 16666666 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        const RecordCase *c = &cases[i];
        uint8_t f[24];

        g_test_message("%s", c->name);
        vbob_setup("");
        vbob_message(0, 0x22, true, c->body, 18);
        g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
        vbob_get_sync(f);
        g_assert_cmpuint(f[c->byte], ==, c->code);
        wr(START_OUT, 0x1000);
        wr(CTL_OUT, 0x2 | 0xea040 | 0x10000);
        qtest_clock_step(qts, c->field_ns - 1);
        g_assert_cmphex(ring_pop(0), ==, 0);
        qtest_clock_step(qts, 1);
        if (c->body[11] | c->body[12]) {
            /* Interlaced: a buffer is a frame, done on the second field. */
            g_assert_cmphex(ring_pop(0), ==, F_VALID | F_FIELD | F_PARITY);
            qtest_clock_step(qts, c->field_ns);
            g_assert_cmphex(ring_pop(0), ==,
                            F_VALID | F_FIELD | F_ONE | F_DONE(1));
        } else {
            g_assert_cmphex(ring_pop(0), ==, F_FIRST_DONE(1));
        }
        qtest_quit(qts);
    }
}

/*
 * "Ports and routing": two crosspoints per call; the input (0x24) and the
 * output (0x25) tables are separate, and a port of 16 or more is logged.
 */
static void test_vbob_routing(void)
{
    const uint8_t in5[] = { 5, 1 };
    const uint8_t in6[] = { 6, 2 };
    const uint8_t out1[] = { 1, 5 };
    const uint8_t out2[] = { 2, 6 };
    const uint8_t out5[] = { 5, 9 };
    const uint8_t bad[] = { 17, 3 };
    uint8_t f[24];
    uint8_t dest;
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;
    int fd = g_file_open_tmp("octane-xt-digvid-route-XXXXXX", &log_path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d guest_errors -D %s", log_path);
    vbob_setup(extra);
    vbob_message(0, 0x24, true, in5, 4);
    vbob_message(0, 0x24, true, in6, 4);
    vbob_message(0, 0x25, true, out1, 4);
    vbob_message(0, 0x25, true, out2, 4);
    vbob_message(0, 0x25, true, out5, 4);
    vbob_message(0, 0x25, true, bad, 4);
    vbob_drain();
    for (unsigned i = 0; i < 5; i++) {
        static const struct { uint8_t dest, source; } want[] = {
            { 1, 5 }, { 2, 6 }, { 5, 9 }, { 6, 0 }, { 3, 0 },
        };

        dest = want[i].dest;
        vbob_message(0, 0x25, false, &dest, 3);
        g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
        g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
        vbob_frame(f);
        g_assert_cmpuint(f[4], ==, want[i].source);
        vbob_release();
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "route destination port 17"));
    g_assert_cmpint(unlink(log_path), ==, 0);
}

/*
 * Migration carries the endpoint: each side's record and source, the lock
 * state already reported, the held receive frame and the one queued behind
 * it.
 */
static void test_migration_vbob(void)
{
    const char *args = "-global sgi-xt-digvid.x-vbob-output-timing=6 "
                       "-global sgi-xt-digvid.x-vbob-present=1 "
                       "-global sgi-xt-digvid.x-vbob-input-timings="
                       "0x010101010110";
    const uint8_t hd_record[] = {
        5, 0x06, 0x72, 0x05, 0x00, 0x02, 0xee, 0x02, 0xee, 0x02, 0xee,
        0, 0, 25, 0, 5,
    };
    const uint8_t hd_a[] = { 0x90 };
    const uint8_t tv[] = { 1, 2 };
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest = NULL;
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *cmd = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t f[24];

    dir = g_dir_make_tmp("octane-xt-digvid-vmigrate-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", dir);
    cmd = g_strdup_printf(MEMTX_TEST_DEVICE "-M octane2 -m 256 -cpu "
                          OCTANE2_CPU " -bios %s -S -display none -vga none "
                          "-device sgi-xt-digvid,slot=widget-10,id=dm2 %s",
                          quoted, args);
    vbob_setup(args);
    /* The SD record, then the HD record, so the latest is HD (720 60p). */
    vbob_message(0, 0x22, true, record_525, 18);
    vbob_drain();
    vbob_message(0, 0x22, true, hd_record, 18);
    vbob_drain();
    /* HD takes jack A, whose 50i reference is no pair: an 0xa6 frame, held. */
    vbob_message(0, 0x21, true, hd_a, 3);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    /* A reply queues behind it. */
    vbob_message(0, 0x26, false, NULL, 2);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, 0);
    from = qts;
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    dest = g_strdup_printf("%s -incoming %s", cmd, uri);
    to = qtest_init(dest);
    sgi_migrate_resume(to);
    qts = to;

    vbob_frame(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3], ==, 0x80);
    vbob_release();
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    vbob_frame(f);
    /* HD record is the latest: HD unlocked (jack A, 50i), SD internal 525. */
    g_assert_cmpuint(f[2], ==, 0xa6);
    g_assert_cmpuint(f[3] & 0x83, ==, 0);
    g_assert_cmpuint(f[4], ==, 1);
    g_assert_cmpuint(f[5], ==, 3);
    vbob_release();
    /* The lock state was reported before: a write sends no new frame. */
    vbob_message(0, 0x20, true, tv, 4);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, 0);
    qtest_quit(to);
    g_assert_cmpint(unlink(g_strdup_printf("%s/migrate.bin", dir)), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

/*
 * "Receive": a frame the host cannot be told of must not strand the buffer.
 * Frames queued behind a held one are dropped while the ring page is
 * unmapped, and a get after it is mapped again gets its reply.
 */
static void test_vbob_rx_unmapped_ring(void)
{
    uint8_t f[24];

    vbob_setup("");
    /* The first reply takes the buffer; two more queue behind it. */
    vbob_message(0, 0x26, false, NULL, 2);
    vbob_message(0, 0x26, false, NULL, 2);
    vbob_message(0, 0x26, false, NULL, 2);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_RX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    g_assert_cmphex(ring_pop(0), ==, F_VALID | F_TX);
    wr(0x108000, 0);
    vbob_release();
    g_assert_cmphex(ring_pop(0), ==, 0);
    wr(0x108000, MAP_VALID_ENTRY(RING0_HOST));
    vbob_get_sync(f);
    g_assert_cmpuint(f[2], ==, 0xa6);
    /* The dropped frames are gone, not waiting behind the release. */
    g_assert_cmphex(ring_pop(0), ==, 0);
    qtest_quit(qts);
}

/* ---- Reset, migration and placement ---- */

/* "Reset": every register, the map RAM and the pipes return to zero. */
static void test_reset(void)
{
    start("");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    arm_out(1, 0x1000);
    qtest_clock_step(qts, FIELD_NS);
    g_assert_cmphex(ring_flags(0, 0), ==, F_FIRST_DONE(1));
    arm_out(2, 0x1000);
    qtest_system_reset(qts);
    g_assert_cmphex(rd(XG_CONTROL), ==, 0);
    g_assert_cmphex(rd(XG_VECTORS), ==, 0);
    g_assert_cmphex(rd(XG_DEST_LOWER), ==, 0);
    g_assert_cmphex(rd(MAP(0x1400)), ==, 0);
    g_assert_cmphex(rd(FPGA), ==, 0);
    g_assert_cmphex(rd(XG_ID), ==, 0x0c102555);
    set_gif_loaded();
    g_assert_cmphex(rd(CTL_OUT), ==, 0);
    /* A pipe armed before the reset runs no more fields. */
    qtest_clock_step(qts, 4 * FIELD_NS);
    g_assert_cmphex(ring_flags(0, 1), ==, 0);
    qtest_quit(qts);
}

/*
 * Migration carries the registers, the map RAM, the pipes and their
 * running field timer: the destination completes the slot the source armed.
 */
static void test_migration(void)
{
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest = NULL;
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *args = NULL;
    QTestState *from, *to;
    GError *err = NULL;
    int64_t armed_at, expires, dest_now;

    dir = g_dir_make_tmp("octane-xt-digvid-migrate-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", dir);
    args = g_strdup_printf(MEMTX_TEST_DEVICE "-M octane2 -m 256 -cpu "
                           OCTANE2_CPU " -bios %s -S -display none -vga none "
                           "-device sgi-xt-digvid,slot=widget-10,id=dm2 "
                           "-global sgi-xt-digvid.x-vbob-output-timing=6",
                           quoted);
    start_with("octane2", "-global sgi-xt-digvid.x-vbob-output-timing=6");
    set_gif_loaded();
    map_pages();
    program_interrupts(0x20100);
    armed_at = qtest_clock_step(qts, 1);
    arm_out(1, 0x1000);
    wr(GIF(0x2c), 0x3ffffff);
    wr(TX(0), 0xabcd);
    qtest_clock_step(qts, 5000000);
    /* 720 60p fields: 1e9 / 60 = 16666666 ns, from the arm. */
    expires = armed_at + 16666666;
    from = qts;
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    dest = g_strdup_printf("%s -incoming %s", args, uri);
    to = qtest_init(dest);
    sgi_migrate_resume(to);
    qts = to;
    g_assert_cmphex(rd(XG_ID), ==, 0x0c102555);
    g_assert_cmphex(rd(XG_VECTORS), ==, 0x41 | (0x42u << 9) | (0x43u << 18) |
                    0x20100);
    g_assert_cmphex(rd(MAP(0x1400)), ==, MAP_VALID_ENTRY(SLOT0_HOST));
    g_assert_cmphex(rd(GIF(0x2c)), ==, 0x3ffffff);
    g_assert_cmphex(rd(TX(0)), ==, 0xabcd);
    g_assert_cmphex(rd(CTL_OUT) & 0xf, ==, 0x2);
    /*
     * The field timer keeps its absolute expiry; qtest's clock restarts on
     * the destination, so measure from where it is.
     */
    dest_now = qtest_clock_step(qts, 1);
    g_assert_cmpint(dest_now, <, expires);
    g_assert_cmphex(ring_flags(0, 0), ==, 0);
    qtest_clock_step(qts, expires - dest_now - 1);
    g_assert_cmphex(ring_flags(0, 0), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(ring_flags(0, 0), ==, F_FIRST_DONE(1));
    g_assert_cmphex(irq_seen(), ==, 0x142);
    qtest_quit(to);
    g_assert_cmpint(unlink(g_strdup_printf("%s/migrate.bin", dir)), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

/*
 * "Placement on the Crossbow": attach sets bit 5 + 8 * (widget & 3) of the
 * Crossbow's register at offset 0x11c, and 0x124 for widgets 12 to 15; the
 * Crossbow keeps the write.
 */
static void test_crossbow_arbitration(void)
{
    uint32_t before;

    start("");
    before = qtest_readl(qts, XBOW + 0x11c);
    qtest_writel(qts, XBOW + 0x11c, before | (1u << (5 + 8 * (10 & 3))));
    g_assert_cmphex(qtest_readl(qts, XBOW + 0x11c), ==,
                    before | (1u << 21));
    before = qtest_readl(qts, XBOW + 0x124);
    qtest_writel(qts, XBOW + 0x124, before | (1u << (5 + 8 * (12 & 3))));
    g_assert_cmphex(qtest_readl(qts, XBOW + 0x124), ==,
                    before | (1u << 5));
    qtest_quit(qts);
}

/*
 * "Placement": an Octane takes the board in an option port; the PCI
 * module's port and a second board in one port refuse it.
 */
static void test_placement(void)
{
    const char *octane[] = {
        qtest_qemu_binary(NULL), "-M", "octane", "-bios", flash_path,
        "-display", "none", "-qmp", "stdio", "-device",
        "sgi-xt-digvid,slot=widget-13", NULL,
    };
    const char *twice[] = {
        qtest_qemu_binary(NULL), "-M", "octane2", "-bios", flash_path,
        "-display", "none", "-qmp", "stdio", "-device",
        "sgi-xt-digvid,slot=widget-10", "-device",
        "sgi-xt-digvid,slot=widget-10", NULL,
    };

    start_with("octane", "");
    g_assert_cmphex(rd(XG_ID), ==, 0x0c102555);
    qtest_quit(qts);
    assert_launch_rejected(octane, "slot widget-13 is the PCI module's port");
    assert_launch_rejected(twice, "already holds a board");
}

int main(int argc, char **argv)
{
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, result;
    struct {
        const char *name;
        void (*test)(void);
    } tests[] = {
        { "identity", test_identity },
        { "read-status", test_read_status },
        { "widths", test_widths },
        { "unimplemented-logged", test_unimplemented_logged },
        { "xg-registers", test_xg_registers },
        { "map-ram", test_map_ram },
        { "fpga-load", test_fpga_load },
        { "fpga-short-stream", test_fpga_short_stream },
        { "fpga-reload", test_fpga_reload },
        { "gif-masks", test_gif_masks },
        { "dma-control-word", test_dma_control_word },
        { "dma-output-field", test_dma_output_field },
        { "dma-input-field", test_dma_input_field },
        { "dma-order-and-wrap", test_dma_order_and_wrap },
        { "interrupt-gating", test_interrupt_gating },
        { "ring-overrun-holds", test_ring_overrun_holds },
        { "invalid-map-entry", test_invalid_map_entry },
        { "vbob-ack", test_vbob_ack },
        { "vbob-replies", test_vbob_replies },
        { "vbob-receive-release", test_vbob_receive_release },
        { "vbob-genlock", test_vbob_genlock },
        { "early-property-change", test_early_property_change },
        { "vbob-timing-record", test_vbob_timing_record },
        { "dma-abort", test_dma_abort },
        { "ust-counter", test_ust_counter },
        { "dma-525-frames", test_dma_525_frames },
        { "widget-reset", test_widget_reset },
        { "vbob-genlock-references", test_vbob_genlock_references },
        { "vbob-nibbles", test_vbob_nibbles },
        { "vbob-timing-rows", test_vbob_timing_rows },
        { "vbob-routing", test_vbob_routing },
        { "vbob-rx-unmapped-ring", test_vbob_rx_unmapped_ring },
        { "migration-vbob", test_migration_vbob },
        { "reset", test_reset },
        { "migration", test_migration },
        { "crossbow-arbitration", test_crossbow_arbitration },
        { "placement", test_placement },
    };

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-xt-digvid-prom-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
        g_autofree char *path = g_strdup_printf("/octane2/xt-digvid/%s",
                                                tests[i].name);

        qtest_add_func(path, tests[i].test);
    }
    result = g_test_run();
    g_assert_cmpint(unlink(flash_path), ==, 0);
    g_free(flash_path);
    return result;
}
