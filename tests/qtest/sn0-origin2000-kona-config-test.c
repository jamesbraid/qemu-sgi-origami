/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA (InfiniteReality) configuration qtests: what a pipe configured
 * with "product" presents, against the real captures it is built from.
 *
 * Each configuration's 0x1e9 answer is compared byte for byte with a block
 * this file builds from the capture's own values, at the offsets gfxinit's
 * report reads them from: true-wombat's InfiniteReality3 (one RM with 256
 * MB, DG5-2 base 030-1242-001 with its GVO), whole-tetra's deskside
 * GE14-4 with two RM7 boards and an eight-channel 030-1087-001,
 * fancy-akita's two-GE GE14 board, and an InfiniteReality2E built from
 * true-wombat's GE16-4 and RM9 with 64 MB texture boards. The two words the
 * driver's attach classes the pipe by are checked through the indirect
 * port, and every configuration no source grounds is refused at realize
 * with its reason. The fixture attaches the pipe as
 * sn0-origin2000-kona-bringup-test.c does.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define KONA_WIDGET 11
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset) (KONA_BASE + (offset))

#define REG_INDIRECT_SELECT KONA_REG(0x8003c0)
#define REG_INDIRECT_DATA   KONA_REG(0x8003d0)
#define REG_KICK            KONA_REG(0xc00108)
#define REG_COMMAND         KONA_REG(0xc10000)

/* The selectors the kernel driver's attach routine reads its class from. */
#define SELECT_PRODUCT 0x8800
#define SELECT_REDUCED 0xb00

#define RETURN_PAGE   UINT64_C(0x320000)
#define CONFIG_OFFSET 8
#define CONFIG_SIZE   0x778

/* An origin2000 with one pipe at io4, the slot widget KONA_WIDGET names. */
#define MACHINE \
    "-M origin2000,nodes=1,x-uncatalogued-graphics=on -smp 1 -m 64M "
#define RACK_PIPE \
    MACHINE "-device sgi-kona,id=kona0,slot=io4,fabric-hub-widget=9"
/* A pipe with no Ktown record, which is what makes it a deskside one. */
#define DESKSIDE_PIPE(props) \
    MACHINE "-device '{\"driver\":\"sgi-kona\",\"id\":\"kona0\"," \
    "\"slot\":\"io4\",\"fabric-hub-widget\":9," \
    "\"other-identity-parts\":[]" props "}'"

typedef struct Board {
    const char *name;
    const char *part;
    const char *revision;
} Board;

/* One configuration as the captures describe it. */
typedef struct Expected {
    uint32_t board_set;
    uint32_t engines;
    uint32_t occupancy;
    uint32_t rms;
    uint32_t texture_code;
    uint32_t texture_mb;
    uint32_t channels;
    uint32_t gamma;
    uint32_t option;
    uint32_t board_word;
    uint32_t rm_word;
    uint32_t trailing_word;
    Board rm, tm, dg, dgopt;
} Expected;

/*
 * true-wombat: GE16-4 030-1398-001 rev E, an RM9 030-1402-001 rev E that
 * gfxinfo calls RM10, TM 030-1588-001 with 256 MB, the DG5-2 base
 * 030-1242-001 rev D with its GVO 030-1184-002 rev B, two channels;
 * gfxinfo's brd line "80f61806 3041606/-/-/- f9311002" and "GVO option
 * detected", display=dg5-2-gvo.
 */
static const Expected true_wombat = {
    .board_set = 5, .engines = 4, .occupancy = 0xf, .rms = 1,
    .texture_code = 4, .texture_mb = 256, .channels = 2, .gamma = 0,
    .option = 6, .board_word = 0x80f61806, .rm_word = 0x3041606,
    .trailing_word = 0xf9311002,
    .rm = { "RM9", "030-1402-001", "E" },
    .tm = { "", "030-1588-001", "" },
    .dg = { "DG5-2", "030-1242-001", "D" },
    .dgopt = { "GVO", "030-1184-002", "B" },
};

/*
 * whole-tetra: GE14-4 030-1129-002, two RM7 030-1054-001 and TM
 * 030-1053-001 with 64 MB each, DG 030-1087-001 reporting eight channels
 * and 64K of external gamma; brd "80160a04 3020c06/3020c06/-/- 51bf1002".
 * Names and revisions of the RM and TM from fancy-akita's owner list.
 */
static const Expected whole_tetra = {
    .board_set = 1, .engines = 4, .occupancy = 0xf, .rms = 2,
    .texture_code = 2, .texture_mb = 64, .channels = 8, .gamma = 1,
    .option = 0, .board_word = 0x80160a04, .rm_word = 0x3020c06,
    .trailing_word = 0x51bf1002,
    .rm = { "RM7", "030-1054-001", "G" },
    .tm = { "TM7", "030-1053-001", "F" },
    .dg = { "", "030-1087-001", "" },
};

static uint64_t synthetic_serial(unsigned instance)
{
    return UINT64_C(0x51454d55) << 16 | (uint64_t)'G' << 8 | instance;
}

static void put_text(uint8_t *field, size_t width, const char *text)
{
    size_t len = strlen(text);

    g_assert_cmpuint(len, <=, width);
    memset(field, ' ', width);
    memcpy(field + width - len, text, len);
}

/* gfxinit's board-report layout, the 0x4c-byte record at `rec`. */
static void put_record(uint8_t *rec, const Board *b, unsigned instance)
{
    uint64_t serial = synthetic_serial(instance);
    g_autofree char *barcode = g_strdup_printf("EMUG%02u", instance);

    for (int i = 0; i < 6; i++) {
        rec[4 + i] = serial >> (40 - 8 * i);
    }
    rec[0x0a] = 0x0b;
    rec[0x0b] = 1;
    rec[0x0c] = 1;
    put_text(rec + 0x0d, 10, barcode);
    put_text(rec + 0x17, 19, b->part);
    memset(rec + 0x2c, ' ', 6);
    put_text(rec + 0x32, 4, b->revision);
    memset(rec + 0x36, 0xff, 6);
    put_text(rec + 0x3c, 14, b->name);
}

static void put_words(uint8_t *at, unsigned count, uint32_t word)
{
    for (unsigned i = 0; i < count; i++) {
        stl_be_p(at + 4 * i, word);
    }
}

static void expected_block(const Expected *e, uint8_t *b)
{
    /* The pool descriptors, two sized by the configuration. */
    static const uint32_t pools[4][7] = {
        { 0, 0x4000, 1, 0, 1, 0x400, 0x100 },
        { 0, 0x4000, 1, 1, 1, 0x400, 0x3c00 },
        { 10, 0x4000, 4, 0, 1, 0x400, 0x400 },
        { 0, 0x4000, 1, 0, 1, 0x400, 0x10000 },
    };

    memset(b, 0, CONFIG_SIZE);
    stl_be_p(b + 0x00, e->board_set);
    stl_be_p(b + 0x04, e->engines);
    stl_be_p(b + 0x08, e->rms);
    stl_be_p(b + 0x0c, 1);
    stl_be_p(b + 0x14, e->channels);
    stl_be_p(b + 0x18, e->option);
    stl_be_p(b + 0x20, e->gamma);
    /* The display generator's own channel count, which setmon checks. */
    stl_be_p(b + 0x44, e->channels);
    stl_be_p(b + 0x54, e->engines);
    stl_be_p(b + 0x58, e->rms);
    stl_be_p(b + 0x5c, e->occupancy);
    stl_be_p(b + 0x84, 1280);
    stl_be_p(b + 0x88, 1024);
    for (unsigned c = 0; c < 4; c++) {
        for (unsigned w = 0; w < 7; w++) {
            stl_be_p(b + 0x90 + 0x1c * c + 4 * w, pools[c][w]);
        }
    }
    stl_be_p(b + 0x90 + 0x1c * 2 + 8, e->engines);
    stl_be_p(b + 0x90 + 0x1c * 3 + 0x18, e->texture_mb * 1024);

    /* gfxinfo -v's brd:, ge:, rm<n>: and dg: lines. */
    stl_be_p(b + 0x100, e->board_word);
    stl_be_p(b + 0x108, 0x14832057);
    stl_be_p(b + 0x10c, 0x24731057);
    stl_be_p(b + 0x110, 0x14231057);
    for (unsigned i = 0; i < e->rms; i++) {
        uint8_t *rm = b + 0x114 + 0x8c * i;

        stl_be_p(b + 0x24 + 8 * i, 1);
        stl_be_p(b + 0x28 + 8 * i, e->texture_code);
        stl_be_p(rm, e->rm_word);
        stl_be_p(rm + 4, 0x15032057);
        stl_be_p(rm + 8, 0x15431057);
        put_words(rm + 0x0c, 4, 0x24631057);
        put_words(rm + 0x1c, 8, 0x24d31057);
        put_words(rm + 0x3c, 20, 0x54938057);
        put_record(b + 0x3e8 + 0x4c * i, &e->rm, 2 + i);
        put_record(b + 0x564 + 0x4c * i, &e->tm, 6 + i);
    }
    stl_be_p(b + 0x344, e->trailing_word);
    stl_be_p(b + 0x348, 0x05532057);
    put_words(b + 0x34c, 4, 0x15838057);
    put_words(b + 0x35c, e->channels, 0x15631057);
    put_record(b + 0x6e0, &e->dg, 10);
    if (e->dgopt.part) {
        put_record(b + 0x72c, &e->dgopt, 11);
    }
}

static uint32_t guest_word(QTestState *s, uint64_t addr)
{
    uint8_t bytes[4];

    qtest_memread(s, addr, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

/* The X server context's 0x1f2, then 0x1e9 0; returns the posted block. */
static void read_config(QTestState *s, uint8_t *block)
{
    uint8_t zero[4] = { 0 };

    qtest_memwrite(s, RETURN_PAGE, zero, sizeof(zero));
    qtest_writel(s, REG_KICK, 0x30000);
    qtest_writel(s, REG_COMMAND, 0x1f2);
    qtest_writel(s, REG_KICK, 0x10000);
    qtest_writel(s, REG_COMMAND,
                 0x10000000 | (uint32_t)(RETURN_PAGE >> 12));
    qtest_writel(s, REG_COMMAND, 0x9000 | (uint32_t)(RETURN_PAGE & 0xfff));
    qtest_writel(s, REG_COMMAND, 0x4000);
    qtest_writel(s, REG_KICK, 0);
    qtest_writel(s, REG_COMMAND, 0x1e9);
    qtest_writel(s, REG_COMMAND, 0);
    g_assert_cmphex(guest_word(s, RETURN_PAGE), ==, 1);
    qtest_memread(s, RETURN_PAGE + CONFIG_OFFSET, block, CONFIG_SIZE);
}

static void assert_config(const char *args, const Expected *e)
{
    QTestState *s = qtest_init(args);
    g_autofree uint8_t *block = g_malloc(CONFIG_SIZE);
    g_autofree uint8_t *expected = g_malloc(CONFIG_SIZE);

    read_config(s, block);
    expected_block(e, expected);
    for (unsigned i = 0; i < CONFIG_SIZE; i += 4) {
        if (ldl_be_p(block + i) != ldl_be_p(expected + i)) {
            g_test_message("first difference at block offset 0x%x", i);
            break;
        }
    }
    g_assert_cmpmem(block, CONFIG_SIZE, expected, CONFIG_SIZE);
    qtest_quit(s);
}

static uint32_t class_word(QTestState *s, uint32_t selector)
{
    qtest_writel(s, REG_INDIRECT_SELECT, selector);
    return qtest_readl(s, REG_INDIRECT_DATA);
}

static void test_ir3_true_wombat(void)
{
    assert_config(RACK_PIPE ",product=ir3,display=dg5-2-gvo", &true_wombat);
}

/* Four RMs, each with true-wombat's boards. */
static void test_ir3_four_rms_gvo(void)
{
    Expected e = true_wombat;

    e.rms = 4;
    assert_config(RACK_PIPE ",product=ir3,rm-count=4,display=dg5-2-gvo", &e);
}

/*
 * A rack pipe that names no display board has a plain DG5-2,
 * fancy-akita's 030-1055-001, and no option record: no capture shows a
 * plain DG5-2 in a rack.
 */
static void test_ir3_plain_dg5_2(void)
{
    Expected e = true_wombat;

    e.option = 0;
    e.dg = (Board){ "DG5-2", "030-1055-001", "" };
    e.dgopt = (Board){ 0 };
    assert_config(RACK_PIPE ",product=ir3", &e);
}

static void test_ir2_whole_tetra(void)
{
    assert_config(DESKSIDE_PIPE(",\"product\":\"ir2\",\"rm-count\":2"),
                  &whole_tetra);
}

/*
 * fancy-akita: "2 GEs (of 2), occmask = 0x03", two RM7 boards, two
 * channels; its owner list names the GE14-2 030-1051-002 and the DG5-2
 * 030-1055-001. Its board words are whole-tetra's.
 */
static void test_ir2_fancy_akita(void)
{
    Expected e = whole_tetra;

    e.engines = 2;
    e.occupancy = 0x3;
    e.channels = 2;
    e.gamma = 0;
    e.dg = (Board){ "DG5-2", "030-1055-001", "" };
    assert_config(DESKSIDE_PIPE(",\"product\":\"ir2\",\"rm-count\":2,"
                                "\"geometry\":\"ge14-2\","
                                "\"display\":\"dg5-2\""),
                  &e);
}

/*
 * InfiniteReality2E: true-wombat's GE16-4 and RM9 with 64 MB TM7 boards,
 * board set 4, which gfxinfo names RM9 by the RM record's name.
 */
static void test_ir2e(void)
{
    Expected e = true_wombat;

    e.board_set = 4;
    e.rms = 2;
    e.texture_code = 2;
    e.texture_mb = 64;
    e.tm = whole_tetra.tm;
    e.option = 0;
    e.dg = (Board){ "DG5-2", "030-1055-001", "" };
    e.dgopt = (Board){ 0 };
    assert_config(RACK_PIPE ",product=ir2e,rm-count=2", &e);
}

/*
 * The attach routine's class: low bits 1 after selector 0x8800 make an
 * InfiniteReality3, 0 leaves the GE16- name test to tell InfiniteReality2E
 * from InfiniteReality2; selector 0xb00's bit 10 would make a Reality.
 */
static void test_class_words(void)
{
    static const struct {
        const char *args;
        uint32_t product;
    } cases[] = {
        { RACK_PIPE ",product=ir3", 1 },
        { RACK_PIPE ",product=ir2e", 0 },
        { DESKSIDE_PIPE(",\"product\":\"ir2\""), 0 },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *s = qtest_init(cases[i].args);

        g_assert_cmphex(class_word(s, SELECT_REDUCED), ==, 0);
        g_assert_cmphex(class_word(s, SELECT_PRODUCT), ==, cases[i].product);
        qtest_quit(s);
    }
}

/* The unconfigured pipe answers neither word: its read stays logged. */
static void test_unconfigured_class_words_logged(void)
{
    g_autofree char *log = g_strdup_printf("%s/sn0-kona-config-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *found = NULL;
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    s = qtest_initf("%s -d unimp -D %s", RACK_PIPE, log);
    g_assert_cmphex(class_word(s, SELECT_PRODUCT), ==, 0);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpint(g_unlink(log), ==, 0);
    g_assert_nonnull(strstr(found, "unimplemented read of offset "
                                   "0x8003d0"));
}

/* Each refused configuration, and the reason realize gives. */
static const struct {
    const char *path;
    const char *args;
    const char *reason;
} refusals[] = {
    { "ir", RACK_PIPE ",product=ir",
      "*IRIX 6.5 never names an Onyx2 pipe InfiniteReality*" },
    { "ir4", RACK_PIPE ",product=ir4",
      "*no Onyx2 InfiniteReality4 has been captured*" },
    { "reality", RACK_PIPE ",product=reality",
      "*no capture gives the Reality pipe's board parts or chip words*" },
    { "unknown-product", RACK_PIPE ",product=ir5",
      "*unknown product 'ir5'*" },
    { "configuration-without-product", RACK_PIPE ",rm-count=2",
      "*set product to configure the pipe*" },
    { "ir2e-geometry", RACK_PIPE ",product=ir2e,geometry=ge14-4",
      "*product ir2e has no geometry board 'ge14-4'; it takes ge16-4*" },
    { "rm-type", RACK_PIPE ",product=ir2,rm-type=rm9",
      "*product ir2 has rm7 raster managers, not 'rm9'*" },
    { "rm-count-3", RACK_PIPE ",product=ir3,rm-count=3",
      "*rm-count 3: a pipe has 1, 2 or 4 raster managers*" },
    { "deskside-four-rms",
      DESKSIDE_PIPE(",\"product\":\"ir2\",\"rm-count\":4"),
      "*at most two raster managers*" },
    { "texture-16", RACK_PIPE ",product=ir2,texture-mb=16",
      "*16 MB of texture per raster manager has no known texture board*" },
    { "channels", DESKSIDE_PIPE(",\"product\":\"ir2\",\"channels\":2"),
      "*channels 2: display generator 030-1087-001 reports 8*" },
    { "display", RACK_PIPE ",product=ir3,display=dg5-2-dplex",
      "*display 'dg5-2-dplex' is not a display board the model presents*" },
    { "display-part", RACK_PIPE ",product=ir3,display=030-1055-001",
      "*display '030-1055-001' is not a display board the model presents*" },
};

static void test_refused(const void *data)
{
    size_t i = (uintptr_t)data;

    if (g_test_subprocess()) {
        qtest_quit(qtest_init(refusals[i].args));
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(refusals[i].reason);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/origin2000/kona-config/ir3-true-wombat",
                   test_ir3_true_wombat);
    qtest_add_func("sn0/origin2000/kona-config/ir3-four-rms-gvo",
                   test_ir3_four_rms_gvo);
    qtest_add_func("sn0/origin2000/kona-config/ir3-plain-dg5-2",
                   test_ir3_plain_dg5_2);
    qtest_add_func("sn0/origin2000/kona-config/ir2-whole-tetra",
                   test_ir2_whole_tetra);
    qtest_add_func("sn0/origin2000/kona-config/ir2-fancy-akita",
                   test_ir2_fancy_akita);
    qtest_add_func("sn0/origin2000/kona-config/ir2e", test_ir2e);
    qtest_add_func("sn0/origin2000/kona-config/class-words",
                   test_class_words);
    qtest_add_func("sn0/origin2000/kona-config/unconfigured-class-words",
                   test_unconfigured_class_words_logged);
    for (size_t i = 0; i < G_N_ELEMENTS(refusals); i++) {
        g_autofree char *path =
            g_strdup_printf("sn0/origin2000/kona-config/refused/%s",
                            refusals[i].path);

        qtest_add_data_func(path, (void *)(uintptr_t)i, test_refused);
    }
    return g_test_run();
}
