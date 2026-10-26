/*
 * Shared definitions for the MGRAS (SGI Odyssey graphics board) qtests.
 *
 * The qtests that exercise the board are split by area into the
 * sgi-mgras-*-test binaries. Each machine-bringup helper, register address
 * and test pattern that more than one of them uses lives here; anything only
 * one binary uses stays in that binary's source file.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_MGRAS_TEST_H
#define SGI_MGRAS_TEST_H

#define WIDGET_BASE(w)    (SPACE_IO + ((uint64_t)(w) << 24))
#define SI_WIDGET         0xf
#define MGRAS_BASE           WIDGET_BASE(SI_WIDGET)
#define MGRAS_REG(offset)    (MGRAS_BASE + (offset))

/*
 * The register cases share one machine: nothing they read depends on the
 * CPU count or the memory size, so neither is spelled out here. The
 * migration cases start their own machines and keep the fuller machine,
 * because what they check crosses a whole machine's saved state.
 */
#define MACHINE_ARGS "-M origin2000 -device sgi-mgras,slot=io3"
#define MIGRATION_ARGS \
    "-M origin2000 -smp 8 -m 256M -device sgi-mgras,slot=io3"

/*
 * Shared QTestState, started lazily and reused by shared tests. Released
 * before tests that start their own machines.
 */
static QTestState *qts;

typedef struct {
    bool uses_shared;
    void (*fn)(void);
} MgrasQTestFunc;

typedef struct {
    bool uses_shared;
    const void *data;
    void (*fn)(const void *);
} MgrasQTestDataFunc;

static inline void mgras_qtest_prepare(bool uses_shared)
{
    if (uses_shared) {
        if (!qts) {
            qts = qtest_init(MACHINE_ARGS);
        }
    } else if (qts) {
        qtest_quit(qts);
        qts = NULL;
    }
}

static inline void mgras_qtest_run_func(const void *opaque)
{
    const MgrasQTestFunc *test = opaque;
    mgras_qtest_prepare(test->uses_shared);
    test->fn();
}

static inline void mgras_qtest_run_data_func(const void *opaque)
{
    const MgrasQTestDataFunc *test = opaque;
    mgras_qtest_prepare(test->uses_shared);
    test->fn(test->data);
}

static inline void mgras_qtest_add_func(const char *path, void (*fn)(void),
                                        bool uses_shared)
{
    MgrasQTestFunc *test = g_new(MgrasQTestFunc, 1);
    test->uses_shared = uses_shared;
    test->fn = fn;
    qtest_add_data_func_full(path, test, mgras_qtest_run_func, g_free);
}

static inline void mgras_qtest_add_data_func(const char *path,
                                             const void *data,
                                             void (*fn)(const void *),
                                             bool uses_shared)
{
    MgrasQTestDataFunc *test = g_new(MgrasQTestDataFunc, 1);
    test->uses_shared = uses_shared;
    test->data = data;
    test->fn = fn;
    qtest_add_data_func_full(path, test, mgras_qtest_run_data_func, g_free);
}
#define REG_INT_DEST_UPPER     MGRAS_REG(0x000034)
#define REG_INT_DEST_LOWER     MGRAS_REG(0x00003c)
#define REG_STARTUP_STATUS     MGRAS_REG(0x008000)
#define REG_PREFIX             MGRAS_REG(0x011000)
#define REG_PENDING            MGRAS_REG(0x011020)
#define REG_PENDING_ACK        MGRAS_REG(0x011028)
#define REG_IRQ_MASK           MGRAS_REG(0x011040)
#define REG_HQ_STATUS          MGRAS_REG(0x020000)
#define REG_RESET_CONTROL      MGRAS_REG(0x072408)
#define REG_REPP_STATUS_1      MGRAS_REG(0x071c08)
#define REG_XMAP_INDEX         MGRAS_REG(0x071c88)
#define REG_REPP_STATUS_3      MGRAS_REG(0x071d00)
#define REG_REPP_STATUS_4      MGRAS_REG(0x071d08)
#define REG_DAC_LUT_INDEX      MGRAS_REG(0x071810)
#define REG_DAC_LUT_DATA_2     MGRAS_REG(0x071910)
#define REG_DAC_LUT_INDEX_LOW  MGRAS_REG(0x071811)
#define REG_GE_DATA_A           MGRAS_REG(0x058048)
#define REG_GE_CTRL_A           MGRAS_REG(0x05804c)
#define REG_GE_READ_1           MGRAS_REG(0x05822c)
#define REG_GE_READ_2           MGRAS_REG(0x058230)
#define REG_ENTRY               MGRAS_REG(0x130500)
#define REG_CONTEXT_COMMAND     MGRAS_REG(0x120400)
#define REG_VC3_INDEXDATA       MGRAS_REG(0x072038)
#define REG_VC3_RAM             MGRAS_REG(0x072190)
#define REG_CMAP_WRITE_INDEX    MGRAS_REG(0x070c30)
#define REG_CMAP_DATA           MGRAS_REG(0x070d18)
#define REG_XMAP_MAIN_MODE      MGRAS_REG(0x071e00)
#define REG_XMAP_BUF_SELECT     MGRAS_REG(0x071d80)
#define REG_HQ_ENABLE           MGRAS_REG(0x011050)
#define REG_HQ_ENABLE_CLEAR     MGRAS_REG(0x011058)
#define REG_HQ_VECTOR_DEST_UPPER MGRAS_REG(0x011060)
#define REG_HQ_VECTOR_DEST_LOWER MGRAS_REG(0x011068)
#define REG_HQ_VECTORS           MGRAS_REG(0x011070)
#define REG_HQ_FLAGS            MGRAS_REG(0x020010)
#define REG_HQ_FLAGS_CLEAR      MGRAS_REG(0x020018)

#define MGRAS_ENTRY_WORD           0x000004fc
#define MGRAS_FINAL_TOKEN           0x0000f400
#define MGRAS_PENDING_FLAGS         0x00080000
#define MGRAS_PENDING_CONTEXT       0x10000000
#define MGRAS_CONTEXT_WORDS         63
#define REG_QUERY_ANSWER            MGRAS_REG(0x028004)
#define MGRAS_SWAP_TOKEN            0x00003700
#define MGRAS_QUERY_TOKEN           0x0000a20c
#define MGRAS_HQ_FLAG_QUERY         0x00020000
#define MGRAS_FENCE_TOKEN           0x000e0400
#define MGRAS_HQ_FLAG_FENCE         0x00010000

#define MGRAS_COMMAND(reg, value) \
    ((((uint64_t)(0x60001 | ((reg) << 6))) << 34) | (uint32_t)(value))
#define MGRAS_COMMAND_EXEC(reg, value) \
    ((((uint64_t)(0x70001 | ((reg) << 6))) << 34) | (uint32_t)(value))

#define VC3_INDEXDATA(index, data16) \
    (((uint32_t)(index) << 24) | (((uint32_t)(data16) & 0xffff) << 8))
#define REG_BLOCK_START         0x46
#define REG_BLOCK_END           0x47
#define REG_PACKEDCOLOR         0x05b
#define REG_CHAR_H              0x070
#define REG_CHAR_L              0x071
#define REG_BKGRD_RG            0x140
#define REG_BKGRD_BA            0x141
#define REG_PP1FILLMODE         0x161
#define REG_COLORMASKMSBS       0x162
#define REG_RE_POINT            0x040
#define REG_COLORMASKLSBSA      0x163
#define REG_DRBPOINTERS         0x16d

/*
 * FILLMODE (OpenBSD impact(4) driver name) picks a CHAR_H/CHAR_L execute's
 * paint mode; see hw/misc/sgi-mgras-raster.c.
 */
#define REG_FILLMODE             0x110
#define MGRAS_FILLMODE_BYTE_XFER    0x10c00000
#define MGRAS_FILLMODE_RECT_FILL_VALUE 0x00100000

/*
 * A rect fill's colour comes from these registers, not PACKEDCOLOR: CI8 masks
 * FILLCOLOR_R to a CMAP index byte, RGB packs the low byte of each of the
 * three high nibbles; see mgras_draw_fill() in hw/misc/sgi-mgras-raster.c.
 */
#define REG_FILLCOLOR_R          0x176
#define REG_FILLCOLOR_G          0x177
#define REG_FILLCOLOR_B          0x178

/*
 * Byte-transfer mode's own packing register; see
 * hw/misc/sgi-mgras-raster.c's MGRAS_RSS_BYTE_XFER_PACKING comment.
 */
#define REG_BYTE_XFER_PACKING     0x159
#define MGRAS_BYTE_XFER_PACKING_PAIRS 0x0080
#define MGRAS_LOGIC_OP_COPY          0x3
#define MGRAS_LOGIC_OP_XOR           0x6

/*
 * The X server's whole RGB pixel word, as written to PACKEDCOLOR or a
 * pair-packed CHAR_H/CHAR_L, has red in bits 7:0 (0x00BBGGRR); the model
 * stores 0x00RRGGBB. See mgras_rgb_from_pixel_word() in the raster model.
 * Tests write MGRAS_PIXEL_WORD(rgb) so their expected pixel stays rgb.
 */
#define MGRAS_PIXEL_WORD(rgb) \
    ((((rgb) & 0xffu) << 16) | ((rgb) & 0xff00u) | (((rgb) >> 16) & 0xffu))

/* PP1FILLMODE bit 13; see MGRAS_RSS_FILLCOLOR_R's comment in the raster model. */
#define MGRAS_PP1FILLMODE_RGB      0x2000

/* x-test-format values; see SGI_MGRAS_PIXEL_FORMAT_* in sgi-mgras.h. */
#define MGRAS_PIXEL_FORMAT_CI8     0
#define MGRAS_PIXEL_FORMAT_RGB     1

#define MGRAS_STATE_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x18 | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))
#define MGRAS_EXEC_TOKEN(reg, payload_bytes) \
    ((uint32_t)(((0x1c | (((reg) >> 8) & 1)) << 16) | \
                (((reg) & 0xff) << 8) | (payload_bytes)))

#define MGRAS_QOM_PATH "/machine/module0/io3"

static inline uint64_t mgras_qom_get_uint(QTestState *qts, const char *property)
{
    QDict *resp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", MGRAS_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) s = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, s->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

/*
 * Open a register case. The model's reset restores every field this file
 * reads, and asserting the settled bring-up status and a clear sticky
 * error here is what proves it: a case cannot silently inherit the
 * register face the previous case left behind.
 */
static inline void mgras_reset_state(QTestState *qts)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x13);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static inline void mgras_reset(void)
{
    mgras_reset_state(qts);
}

static inline void mgras_assert_width_error(QTestState *qts, uint64_t addr)
{
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error-addr"), ==,
                    addr - MGRAS_BASE);
}

static inline uint32_t mgras_test_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-pixel");
}

/*
 * The `count` pixels along line y from x, each what mgras_test_pixel()
 * reads there, from one x-test-pixel-row command. Free the result.
 */
static inline uint32_t *mgras_test_pixel_row(QTestState *qts, uint32_t x,
                                             uint32_t y, uint32_t count)
{
    uint32_t *row = g_new0(uint32_t, count);
    QDict *reply;
    const char *digits;

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-count', 'value': %u}}", MGRAS_QOM_PATH,
        count);
    reply = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-row'}}", MGRAS_QOM_PATH);
    digits = qdict_get_str(reply, "return");
    g_assert_cmpuint(strlen(digits), ==, count * 8);
    for (uint32_t i = 0; i < count; i++) {
        char word[9] = { 0 };

        memcpy(word, digits + i * 8, 8);
        row[i] = g_ascii_strtoull(word, NULL, 16);
    }
    qobject_unref(reply);
    return row;
}

static inline uint32_t mgras_test_display_pixel(QTestState *qts, uint32_t x,
                                                uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-display-pixel");
}

/* The overlay index at (x, y); 0 is transparent. */
static inline uint32_t mgras_test_overlay(QTestState *qts, uint32_t x,
                                          uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-overlay");
}

static inline uint32_t mgras_test_wid(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-wid");
}

static inline uint32_t mgras_test_format(QTestState *qts, uint32_t x,
                                         uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", MGRAS_QOM_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", MGRAS_QOM_PATH, y);
    return mgras_qom_get_uint(qts, "x-test-format");
}

static inline uint32_t mgras_screendump_pixel(QTestState *qts, uint32_t x,
                                              uint32_t y)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    char *pixels;
    gsize length;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-surface-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    {
        QDict *response = qtest_qmp(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s}}",
            path);
        if (!qdict_haskey(response, "return")) {
            g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
            g_error("screendump failed: %s", text->str);
        }
        qobject_unref(response);
    }
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    pixels = g_strstr_len(contents, length, "\n255\n");
    g_assert_nonnull(pixels);
    pixels += 5;
    g_assert_cmpuint(x, <, 1280);
    g_assert_cmpuint(y, <, 1024);
    g_assert_cmpuint((gsize)(pixels - contents) + 1280 * 1024 * 3, <=,
                     length);
    pixels += (y * 1280 + x) * 3;
    unlink(path);
    return ((uint8_t)pixels[0] << 16) | ((uint8_t)pixels[1] << 8) |
           (uint8_t)pixels[2];
}

static inline void mgras_enter_context(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==, 0);

    /*
     * No closing word exists at 0x120400: the image completes as soon as
     * the 63rd word lands.
     */
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
}

static inline void mgras_write_cmap_position(QTestState *qts, uint16_t selector,
                                             unsigned int position,
                                             uint32_t color)
{
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, selector);
    for (unsigned int i = 0; i < position; i++) {
        qtest_writel(qts, REG_CMAP_DATA, 0);
    }
    qtest_writel(qts, REG_CMAP_DATA, color);
}

/* Address one entry directly: bits 15:8 are the entry, bits 4:0 the map. */
static inline void mgras_write_cmap_entry(QTestState *qts, unsigned int map,
                                          unsigned int entry, uint32_t color)
{
    qtest_writew(qts, REG_CMAP_WRITE_INDEX, (entry << 8) | map);
    qtest_writel(qts, REG_CMAP_DATA, color);
}

/*
 * Give a rectangle a window ID the way the X server does: through the VC3
 * main display-ID table (register 4), a frame table of one SRAM word per
 * scanline pointing at a run list of (column << 5 | WID) entries ended by a
 * column past the screen. Each tagged row is read back from SRAM, the
 * rectangle laid over it, and a fresh run list appended, so earlier tags on
 * the row survive, including ones a migration carried over. An untagged
 * row's frame entry is still its post-reset zero, which points at SRAM word
 * 0 like any other row would: seed a run list there the first time it is
 * read, rather than treating a zero frame entry as a special empty case.
 */
#define MGRAS_DID_FRAME_BASE 0x0500
#define MGRAS_DID_NEXT_ADDR  (MGRAS_DID_FRAME_BASE + 1024)
#define MGRAS_DID_RUNS_BASE  0x2000

static inline uint16_t mgras_vc3_sram_read(QTestState *qts, uint16_t addr)
{
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, addr));
    return qtest_readw(qts, REG_VC3_RAM);
}

static inline void mgras_did_read_row(QTestState *qts, uint32_t y, uint8_t *did)
{
    uint16_t ptr = mgras_vc3_sram_read(qts, MGRAS_DID_FRAME_BASE + y);
    uint16_t run;
    uint8_t current;

    memset(did, 0, 1280);
    if (!ptr) {
        /*
         * A frame entry of zero is not a special case: it points at SRAM
         * word 0 like any other row would. Seed a one-entry run list there
         * (WID 0, then the screen-edge terminator) so the walk below
         * follows an ordinary pointer instead of relying on word 0
         * happening to still hold its post-reset zero.
         */
        qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0));
        qtest_writew(qts, REG_VC3_RAM, 0);
        qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 1));
        qtest_writew(qts, REG_VC3_RAM, 0xffe0);
    }
    run = mgras_vc3_sram_read(qts, ptr);
    current = run & 0x1f;
    for (uint32_t x = 0; x < 1280;) {
        run = mgras_vc3_sram_read(qts, ++ptr);
        g_assert_cmpuint(run >> 5, >, x);
        for (; x < MIN(run >> 5, 1280u); x++) {
            did[x] = current;
        }
        current = run & 0x1f;
    }
}

static inline void mgras_tag_wid(QTestState *qts, uint32_t x0, uint32_t y0,
                                 uint32_t x1, uint32_t y1, uint8_t wid)
{
    uint16_t next = mgras_vc3_sram_read(qts, MGRAS_DID_NEXT_ADDR);

    if (!next) {
        next = MGRAS_DID_RUNS_BASE;
    }
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(0x04, MGRAS_DID_FRAME_BASE));
    for (uint32_t y = y0; y <= y1; y++) {
        uint8_t did[1280];
        uint16_t list = next;

        mgras_did_read_row(qts, y, did);
        memset(&did[x0], wid, x1 - x0 + 1);
        qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, list));
        qtest_writew(qts, REG_VC3_RAM, did[0]);
        next++;
        for (uint32_t x = 1; x < 1280; x++) {
            if (did[x] != did[x - 1]) {
                qtest_writew(qts, REG_VC3_RAM, (x << 5) | did[x]);
                next++;
            }
        }
        qtest_writew(qts, REG_VC3_RAM, 0xffe0);
        next++;
        qtest_writel(qts, REG_VC3_INDEXDATA,
                     VC3_INDEXDATA(0x07, MGRAS_DID_FRAME_BASE + y));
        qtest_writew(qts, REG_VC3_RAM, list);
    }
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(0x07, MGRAS_DID_NEXT_ADDR));
    qtest_writew(qts, REG_VC3_RAM, next);
}

/*
 * PP1SELECT (0x071c08) picks which PP1 the XMAP registers reach. The IRIX
 * 6.5.30 kernel writes ((re << 1) | pp) << 1 there before its per-PP1
 * sequences (the RE-to-PP sync routine, the PP1 update routine), so 0 and 2
 * name the two PP1s of RE 0 and 4 and 6 those of RE 1, and it writes 1 before
 * configuring every PP1 at once (the XMAP initialization routine, the PP RDRAM
 * sync routine) and again after each per-PP1 loop. A two-RE board has four
 * PP1s, each with its own XMAP register file; 0x071f00 and 0x071f80 are the
 * exception, kept by no PP1 (test_pp1_select_leaves_sync_registers_shared).
 */
#define REG_PP1_SELECT          REG_REPP_STATUS_1
#define PP1_SELECT(re, pp)      ((((re) << 1) | (pp)) << 1)

static inline void mgras_select_xmap(QTestState *s, uint8_t select,
                                     uint8_t index)
{
    qtest_writeb(s, REG_PP1_SELECT, select);
    qtest_writeb(s, REG_XMAP_INDEX, index);
}
#define REG_XMAP_OVERLAY_MODE   MGRAS_REG(0x071e80)
#define MGRAS_TWO_RE_ARGS       ",product=ssi"

/* The PP1SELECT value naming PP1 n: RE n / 2, its PP1 n % 2. */
static inline uint8_t mgras_pp1_select_one(unsigned int n)
{
    return PP1_SELECT(n / 2, n % 2);
}

/*
 * Give each of a two-RE board's four PP1s different XMAP contents: under
 * index 0, main mode 0x100 + n, buffer select 0x10 + n, overlay mode
 * 0x20 + n, 0x071d00 0x1000 + n and 0x071d08 0x2000 + n, and then index
 * byte 4 * (n + 1). No broadcast write follows, so the four stay apart.
 */
static inline void mgras_give_pp1s_distinct_xmaps(QTestState *s)
{
    for (unsigned int n = 0; n < 4; n++) {
        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        qtest_writel(s, REG_XMAP_MAIN_MODE, 0x100 + n);
        qtest_writel(s, REG_XMAP_BUF_SELECT, 0x10 + n);
        qtest_writel(s, REG_XMAP_OVERLAY_MODE, 0x20 + n);
        qtest_writel(s, REG_REPP_STATUS_3, 0x1000 + n);
        qtest_writel(s, REG_REPP_STATUS_4, 0x2000 + n);
        qtest_writeb(s, REG_XMAP_INDEX, 4 * (n + 1));
    }
}

/* Read PP1 n's index byte, then its registers under index 0. */
static inline void mgras_assert_pp1_xmap(QTestState *s, uint8_t select,
                                         unsigned int n)
{
    qtest_writeb(s, REG_PP1_SELECT, select);
    g_assert_cmphex(qtest_readb(s, REG_XMAP_INDEX), ==, 4 * (n + 1));
    qtest_writeb(s, REG_XMAP_INDEX, 0);
    g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==, 0x100 + n);
    g_assert_cmphex(qtest_readl(s, REG_XMAP_BUF_SELECT), ==, 0x10 + n);
    g_assert_cmphex(qtest_readl(s, REG_XMAP_OVERLAY_MODE), ==, 0x20 + n);
    g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3), ==, 0x1000 + n);
    g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_4), ==, 0x2000 + n);
    qtest_writeb(s, REG_XMAP_INDEX, 4 * (n + 1));
}

#define HUB_WIDGET              9
#define HUB_REG_OFFSET          0x90
#define HQ_WIDGET               11
#define HQ_REG_OFFSET           0xa0

/*
 * The IRIX 6.5.30 kernel connects its retrace handler to the third of the
 * board's crosstalk interrupts, places that vector in bits 23:16 of the HQ
 * vector word, and enables retrace interrupts by setting bit 0 of VC3
 * register 0x1e. It never sets bit 0 of the 0x011040 enable word (it writes
 * 0x3008a200 there), and the handler acknowledges nothing on the board: the
 * Hub's own pending bit is the only latch, which the kernel clears itself.
 * So the retrace interrupt is gated by the VC3 bit and arrives again at every
 * retrace. Aim the vector at the local Hub's interrupt-modify register and
 * watch the Hub's pending bit.
 */
#define HUB_BASE                UINT64_C(0x20001000000)
#define HUB_PI_INT_MODIFY       0x90
#define HUB_PI_INT_PEND0        0x98
/* The destination words the IRIX 6.5.30 kernel programs for node 0's Hub. */
#define KERNEL_HQ_DEST_UPPER    0x29008000
#define KERNEL_HQ_DEST_LOWER    0x01800090
#define KERNEL_IRQ_MASK         0x3008a200
#define VC3_CONTROL             0x1e
#define VC3_CONTROL_VINTR       0x1
#define VC3_CONTROL_DISPLAY     0x4

/*
 * A retrace period as frame clocks of a clock, and when retrace k falls
 * after the phase starts: exactly k frames, truncated to the nanosecond.
 * The default is the IRIX 6.5.30 1280x1024 60 Hz table (1065 lines of 840
 * VC3 clocks) at half its 107.25 MHz PLL output, about 59.943 Hz: the
 * timing the IRIX kernel's table gives.
 */
typedef struct MgrasFrameTime {
    uint64_t clocks;
    uint64_t hz;
} MgrasFrameTime;

static const MgrasFrameTime mgras_stock_frame = { 1065 * 840, 53625000 };

static inline int64_t mgras_retrace_at(const MgrasFrameTime *t, uint64_t k)
{
    return k * t->clocks * NANOSECONDS_PER_SECOND / t->hz;
}

/* From retrace k - 1 to retrace k. */
static inline int64_t mgras_retrace_gap(const MgrasFrameTime *t, uint64_t k)
{
    return mgras_retrace_at(t, k) - mgras_retrace_at(t, k - 1);
}

static inline uint64_t mgras_hub_pending(QTestState *q)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(q, 0, HUB_BASE + HUB_PI_INT_PEND0, bytes,
                                    sizeof(bytes)));
    return ldq_be_p(bytes);
}

static inline void mgras_hub_clear(QTestState *q, unsigned int vector)
{
    uint8_t bytes[8];

    stq_be_p(bytes, vector);
    g_assert_true(qtest_memwrite_cpu(q, 0, HUB_BASE + HUB_PI_INT_MODIFY,
                                     bytes, sizeof(bytes)));
}

/*
 * Step to one nanosecond before the next retrace, see none, then step onto
 * it and see exactly one.
 */
static inline void mgras_expect_retrace_in(QTestState *q, int64_t ns,
                                           unsigned int vector)
{
    const uint64_t bit = UINT64_C(1) << vector;
    uint64_t before = mgras_qom_get_uint(q, "x-test-retraces");

    mgras_hub_clear(q, vector);
    qtest_clock_step(q, ns - 1);
    g_assert_cmphex(mgras_hub_pending(q) & bit, ==, 0);
    g_assert_cmpuint(mgras_qom_get_uint(q, "x-test-retraces"), ==, before);
    qtest_clock_step(q, 1);
    g_assert_cmphex(mgras_hub_pending(q) & bit, ==, bit);
    g_assert_cmpuint(mgras_qom_get_uint(q, "x-test-retraces"), ==,
                     before + 1);
}

static inline void mgras_retrace_to_hub_on(QTestState *q, unsigned int vector)
{
    qtest_writel(q, REG_HQ_VECTOR_DEST_UPPER, KERNEL_HQ_DEST_UPPER);
    qtest_writel(q, REG_HQ_VECTOR_DEST_LOWER, KERNEL_HQ_DEST_LOWER);
    qtest_writel(q, REG_HQ_VECTORS, vector << 16);
    qtest_writel(q, REG_IRQ_MASK, KERNEL_IRQ_MASK);
    qtest_writel(q, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
}

/* Retraces first .. last of t fall where t puts them, one at a time. */
static inline void mgras_expect_retraces(QTestState *q, const MgrasFrameTime *t,
                                         uint64_t first, uint64_t last,
                                         unsigned int vector)
{
    for (uint64_t k = first; k <= last; k++) {
        mgras_expect_retrace_in(q, mgras_retrace_gap(t, k), vector);
    }
}

static inline void mgras_vc3_sram_load(QTestState *q, uint16_t addr,
                                       const uint16_t *words, size_t n)
{
    qtest_writel(q, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, addr));
    for (size_t i = 0; i < n; i++) {
        qtest_writew(q, REG_VC3_RAM, words[i]);
    }
}

/* One DAC register, as the kernel writes the PLL: number byte, data byte. */
static inline void mgras_dac_reg_write(QTestState *q, uint8_t reg,
                                       uint8_t value)
{
    qtest_writeb(q, REG_DAC_LUT_INDEX_LOW, reg);
    qtest_writeb(q, REG_DAC_LUT_DATA_2, value);
}

/*
 * A 640x480-shaped mode, 525 lines of 400 VC3 clocks, in a frame table at
 * 0x380 and two line sequences. The first mixes one- and two-halfword
 * entries and has an entry whose first halfword alone has bit 15 set,
 * which does not end the line; the second ends on a one-halfword entry.
 * Each sequence closes with its own address.
 */
static const uint16_t mgras_vt_line_a[] = {
    (100 << 8) | 0x05, 0x0003,          /* two halfwords, 100 clocks */
    0x0080 | (100 << 8),                /* one halfword, 100 clocks */
    0x8000 | (100 << 8) | 0x01, 0x0000, /* bit 15 on the first only */
    0x8080 | (100 << 8),                /* ends the line: 400 clocks */
    0x0000,
};
static const uint16_t mgras_vt_line_b[] = {
    0x0080 | (127 << 8), 0x0080 | (127 << 8), 0x0080 | (127 << 8),
    0x8080 | (19 << 8),                 /* 3 x 127 + 19 = 400 clocks */
    0x0010,
};
static const uint16_t mgras_vt_frame[] = {
    0x0000, 25, 0x0010, 500, 0x0000, 0,
};

/*
 * DAC r = 3, v = 13, cntl = 0x75, the IRIX 6.5.30 640x480 60 Hz setting:
 * 33 MHz x 61 / (5 x 8 x 2) = 25.1625 MHz, so the VC3 runs at 12.58125
 * MHz and the frame takes 210,000 of its clocks, about 59.911 Hz.
 */
static const MgrasFrameTime mgras_vga_frame = { 525 * 400, 12581250 };

static inline void mgras_load_vga_timing(QTestState *q)
{
    mgras_vc3_sram_load(q, 0x0000, mgras_vt_line_a,
                        ARRAY_SIZE(mgras_vt_line_a));
    mgras_vc3_sram_load(q, 0x0010, mgras_vt_line_b,
                        ARRAY_SIZE(mgras_vt_line_b));
    mgras_vc3_sram_load(q, 0x0380, mgras_vt_frame,
                        ARRAY_SIZE(mgras_vt_frame));
    qtest_writel(q, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x0380));
    mgras_dac_reg_write(q, 0x0c, 3);
    mgras_dac_reg_write(q, 0x0f, 13);
    mgras_dac_reg_write(q, 0x09, 0x75);
    qtest_writel(q, REG_RESET_CONTROL, 0xe8000000);
}
#define REG_GE_REVISION        MGRAS_REG(0x028004)

/*
 * The OpenGL client's own words on the main port, as the IRIX 6.5.30
 * IP27GR532 libGLcore.so frames them: glViewport 0x3310,
 * glClearColor 0xba10, glClear's colour token 0x1500,
 * glShadeModel 0x4704, glColor3f 0x2000020c, glBegin and
 * glEnd for GL_TRIANGLES 0x1a00 and 0x2400, glVertex2f 0x40000008 and
 * glFlush 0x3600.
 */
#define GL_VIEWPORT_HDR       0x00003310
#define GL_CLEAR_COLOR_HDR    0x0000ba10
#define GL_CLEAR_COLOR_BUFFER 0x00001500
#define GL_SHADE_MODEL_HDR    0x00004704
#define GL_COLOR3F_HDR        0x2000020c
#define GL_BEGIN_TRIANGLES    0x00001a00
#define GL_END_TRIANGLES      0x00002400
#define GL_VERTEX2F_HDR       0x40000008
#define GL_FLUSH              0x00003600
#define GL_FLAT               0x1d00

/*
 * The 63-word context image the IRIX kernel wrote for the probe's GL
 * context, a 128x128 window whose top-left pixel is at screen (256, 256)
 * (a captured run, seq 3959112). Word 2 is the window's
 * lower-left corner as (y << 16) | x with y counted up from the bottom of
 * the screen, word 3 is 0x11, and words 11 and 12 are the window's column
 * and row ranges in the 0x147/0x148 layout. The other words are carried
 * over unchanged.
 */
static const uint32_t gl_context_image[MGRAS_CONTEXT_WORDS] = {
    0x39e9, 0x2c000000, 0x2800100, 0x11, 0, 0x124f8, 0xfb50414, 0, 0,
    0xffa3, 0, 0x100017f, 0x28002ff, 0x90240, 0x2800100, 0x90240, 1, 1,
    0x1000100, 0x17f017f, 0x800080, 0x11400000, 8, 0, 0xcac, 0xc004504, 0,
    0xff, 0xff, 0x10, 0, 0, 0, 0, 0, 0, 0x4ff, 0x3ff, 0xffffff, 0xffffff,
};

/*
 * Switch to the GL context whose image word 0 is slot and word 1 status,
 * and whose window has its lower-left pixel at column x and at row y
 * counted from the bottom of the screen.
 */
static inline void mgras_enter_gl_image(QTestState *qts, uint32_t slot,
                                        uint32_t status, uint32_t x, uint32_t y,
                                        uint32_t w, uint32_t h, uint32_t clip)
{
    uint32_t image[MGRAS_CONTEXT_WORDS];

    memcpy(image, gl_context_image, sizeof(image));
    image[0] = slot;
    image[1] = status;
    image[2] = image[14] = (y << 16) | x;
    image[3] = clip;
    image[11] = (x << 16) | (x + w - 1);
    image[12] = (y << 16) | (y + h - 1);

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, image[i]);
    }
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                    MGRAS_PENDING_CONTEXT);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
}

static inline void mgras_enter_gl_slot(QTestState *qts, uint32_t slot,
                                       uint32_t x, uint32_t y, uint32_t w,
                                       uint32_t h, uint32_t clip)
{
    mgras_enter_gl_image(qts, slot, gl_context_image[1], x, y, w, h, clip);
}

static inline void mgras_enter_gl_window(QTestState *qts, uint32_t x,
                                         uint32_t y, uint32_t w, uint32_t h,
                                         uint32_t clip)
{
    mgras_enter_gl_slot(qts, gl_context_image[0], x, y, w, h, clip);
}

static inline void gl_word(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, REG_CONTEXT_COMMAND, word);
}

static inline void gl_float(QTestState *qts, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    gl_word(qts, bits);
}

static inline void gl_viewport(QTestState *qts, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    gl_word(qts, GL_VIEWPORT_HDR);
    gl_word(qts, x);
    gl_word(qts, y);
    gl_word(qts, w);
    gl_word(qts, h);
}

static inline void gl_clear_color(QTestState *qts, float r, float g, float b,
                                  float a)
{
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, r);
    gl_float(qts, g);
    gl_float(qts, b);
    gl_float(qts, a);
}

static inline void gl_color3f(QTestState *qts, float r, float g, float b)
{
    gl_word(qts, GL_COLOR3F_HDR);
    gl_float(qts, r);
    gl_float(qts, g);
    gl_float(qts, b);
}

static inline void gl_vertex2f(QTestState *qts, float x, float y)
{
    gl_word(qts, GL_VERTEX2F_HDR);
    gl_float(qts, x);
    gl_float(qts, y);
}

/* One screendump, 0xRRGGBB per pixel. */
static inline uint32_t *mgras_screendump(QTestState *qts)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    uint32_t *rgb = g_new(uint32_t, 1280 * 1024);
    const uint8_t *pixels;
    gsize length;
    int fd;

    fd = g_file_open_tmp("sgi-mgras-gl-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    unlink(path);
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    g_assert_cmpuint(length, ==, strlen("P6\n1280 1024\n255\n") +
                     1280 * 1024 * 3);
    pixels = (const uint8_t *)contents + strlen("P6\n1280 1024\n255\n");
    for (int i = 0; i < 1280 * 1024; i++) {
        rgb[i] = (pixels[i * 3] << 16) | (pixels[i * 3 + 1] << 8) |
                 pixels[i * 3 + 2];
    }
    return rgb;
}

/*
 * Expected yellow spans for the triangle test cases in a 128x128 window.
 * Every other window pixel is the clear color.
 */
typedef struct GlSpan {
    uint8_t row;
    uint8_t left;
    uint8_t right;
} GlSpan;

static const GlSpan gl_triangle_spans[] = {
    {18, 55, 56}, {19, 55, 57}, {20, 55, 57}, {21, 54, 58}, {22, 54, 59},
    {23, 53, 59}, {24, 53, 60}, {25, 52, 60}, {26, 52, 61}, {27, 52, 62},
    {28, 51, 62}, {29, 51, 63}, {30, 50, 64}, {31, 50, 64}, {32, 49, 65},
    {33, 49, 66}, {34, 49, 66}, {35, 48, 67}, {36, 48, 68}, {37, 47, 68},
    {38, 47, 69}, {39, 47, 69}, {40, 46, 70}, {41, 46, 71}, {42, 45, 71},
    {43, 45, 72}, {44, 44, 73}, {45, 44, 73}, {46, 44, 74}, {47, 43, 75},
    {48, 43, 75}, {49, 42, 76}, {50, 42, 77}, {51, 41, 77}, {52, 41, 78},
    {53, 41, 78}, {54, 40, 79}, {55, 40, 80}, {56, 39, 80}, {57, 39, 81},
    {58, 39, 82}, {59, 38, 82}, {60, 38, 83}, {61, 37, 84}, {62, 37, 84},
    {63, 36, 85}, {64, 36, 86}, {65, 36, 86}, {66, 35, 87}, {67, 35, 88},
    {68, 34, 88}, {69, 34, 89}, {70, 33, 89}, {71, 33, 90}, {72, 33, 91},
    {73, 32, 91}, {74, 32, 92}, {75, 31, 93}, {76, 31, 93}, {77, 31, 94},
    {78, 30, 95}, {79, 30, 95}, {80, 29, 96}, {81, 29, 97}, {82, 28, 97},
    {83, 28, 98}, {84, 28, 98}, {85, 27, 99}, {86, 27, 100}, {87, 26, 100},
    {88, 26, 101}, {89, 25, 102}, {90, 25, 102}, {91, 25, 103}, {92, 24, 104},
    {93, 24, 104}, {94, 23, 105}, {95, 23, 106}, {96, 23, 106}, {97, 22, 107},
    {98, 22, 107}, {99, 21, 108}, {100, 21, 109}, {101, 20, 109},
    {102, 20, 110}, {103, 20, 111}, {104, 19, 105}, {105, 19, 93},
    {106, 18, 81}, {107, 18, 69}, {108, 17, 57}, {109, 17, 45}, {110, 17, 33},
    {111, 16, 21},
};

static inline void gl_check_spans(const uint32_t *rgb, const GlSpan *spans,
                                  size_t count, uint32_t ink, uint32_t paper)
{
    size_t next = 0;
    unsigned int drawn = 0;

    for (int row = 0; row < 128; row++) {
        const GlSpan *span = next < count && spans[next].row == row ?
                             &spans[next++] : NULL;

        for (int col = 0; col < 128; col++) {
            bool in = span && col >= span->left && col <= span->right;
            uint32_t got = rgb[(256 + row) * 1280 + 256 + col];

            if (got != (in ? ink : paper)) {
                g_error("window pixel (%d, %d) is 0x%06x, expected 0x%06x",
                        col, row, got, in ? ink : paper);
            }
            drawn += in;
        }
    }
    g_assert_cmpuint(next, ==, count);
    g_assert_cmpuint(drawn, >, 0);
}

/* Window pixel (x, y), y counted up from the bottom of the probe window. */
/* `count` window pixels along line y from x, as gl_window_pixel() reads. */
static inline uint32_t *gl_window_row(QTestState *qts, int x, int y,
                                      uint32_t count)
{
    return mgras_test_pixel_row(qts, 256 + x, 383 - y, count);
}

/*
 * Window pixels (0, 0), (1, 0), (0, 1) and (1, 1) as got[0..3], each what
 * gl_window_pixel() reads there, from two row reads.
 */
static inline void gl_window_quad(QTestState *qts, uint32_t got[4])
{
    for (int y = 0; y < 2; y++) {
        g_autofree uint32_t *row = gl_window_row(qts, 0, y, 2);

        got[2 * y] = row[0];
        got[2 * y + 1] = row[1];
    }
}

static inline uint32_t gl_window_pixel(QTestState *qts, int x, int y)
{
    return mgras_test_pixel(qts, 256 + x, 383 - y);
}

/*
 * The GL client's matrix, depth and buffer words as the recorded probe
 * streams of a captured run carry them (glMatrixMode
 * 0x2a04, glLoadIdentity 0x2c00, glLoadMatrixf 0x2b40 with 16 floats in
 * memory order, glMultMatrixf 0x2d40, glFrustum 0x3418 and glOrtho 0x3518
 * with the six arguments as floats, glEnable(GL_DEPTH_TEST) 0x6604 with 1,
 * glDepthFunc 0x4304 with func - 0x200, glClearDepth 0xbc04 with the
 * 24-bit value, glClear's depth token 0x4a00, glDepthMask 0x3c04, the
 * draw-buffer triple 0x490c and the kernel's bank token 0x9808).
 */
#define GL_MATRIX_MODE_HDR    0x00002a04
#define GL_LOAD_MATRIX_HDR    0x00002b40
#define GL_LOAD_IDENTITY      0x00002c00
#define GL_ORTHO_HDR          0x00003518
#define GL_DEPTH_TEST_HDR     0x00006604
#define GL_DEPTH_FUNC_HDR     0x00004304
#define GL_DEPTH_MASK_HDR     0x00003c04
#define GL_CLEAR_DEPTH_HDR    0x0000bc04
#define GL_CLEAR_DEPTH_BUFFER 0x00004a00
#define GL_DRAW_BUFFER_HDR    0x0000490c
#define GL_VERTEX3F_HDR       0x2000000c
#define GL_MODELVIEW          0x1700
#define GL_PROJECTION         0x1701
#define GL_TEXTURE            0x1702
#define GL_DEPTH_LESS         1
#define GL_DEPTH_NEVER        0
#define GL_DEPTH_ALWAYS       7

static inline void gl_vertex3f(QTestState *qts, float x, float y, float z)
{
    gl_word(qts, GL_VERTEX3F_HDR);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
}

static inline void gl_six(QTestState *qts, uint32_t header, float l, float r,
                          float b, float t, float n, float f)
{
    gl_word(qts, header);
    gl_float(qts, l);
    gl_float(qts, r);
    gl_float(qts, b);
    gl_float(qts, t);
    gl_float(qts, n);
    gl_float(qts, f);
}

static inline void gl_one(QTestState *qts, uint32_t header, uint32_t value)
{
    gl_word(qts, header);
    gl_word(qts, value);
}

static inline void gl_draw_buffer(QTestState *qts, uint32_t buffers)
{
    gl_word(qts, GL_DRAW_BUFFER_HDR);
    gl_word(qts, buffers);
    gl_word(qts, 1);
    gl_word(qts, 0);
}

/* The window's pixels, top row first, as the expected PPM stores them. */
static inline void gl_check_window_sha256(const uint32_t *rgb,
                                          const char *expected)
{
    g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);

    for (int row = 0; row < 128; row++) {
        for (int col = 0; col < 128; col++) {
            uint32_t p = rgb[(256 + row) * 1280 + 256 + col];
            uint8_t bytes[3] = { p >> 16, p >> 8, p };

            g_checksum_update(sum, bytes, sizeof(bytes));
        }
    }
    g_assert_cmpstr(g_checksum_get_string(sum), ==, expected);
}

/* Screen pixel of window pixel (x, y), y counted up from the bottom. */
static inline uint32_t gl_dump_pixel(const uint32_t *rgb, int x, int y)
{
    return rgb[(383 - y) * 1280 + 256 + x];
}

/* The probe's window, display on, and the state every scene case sets. */
static inline void gl_scene_start(QTestState *qts, uint32_t shade)
{
    mgras_reset_state(qts);
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_SHADE_MODEL_HDR, shade);
}

static inline uint32_t mgras_test_slot_pixel(QTestState *s, uint32_t slot,
                                             uint32_t x, uint32_t y)
{
    mgras_test_pixel(s, x, y);
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-slot', 'value': %u}}", MGRAS_QOM_PATH, slot);
    return mgras_qom_get_uint(s, "x-test-slot-pixel");
}

/* A GL window on a fresh board, as gl_scene_start() makes one. */
static inline void gl_deep_scene(QTestState *s)
{
    mgras_enter_gl_window(s, 256, 640, 128, 128, 0x11);
    qtest_writel(s, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(s, 0, 0, 128, 128);
    gl_one(s, GL_SHADE_MODEL_HDR, GL_FLAT);
}

/*
 * Context image word 0 names the context's GL state: 0x3bcd and 0x5420
 * for the two probe clients of a captured run, which
 * never re-send their state after a switch. The tests give both contexts
 * the same window, so a model that keyed state by the window, or kept one
 * state for the board, fails them.
 */
#define GL_SLOT_A             0x3bcd
#define GL_SLOT_B             0x5420
#define GL_SLOT_DEFAULT       0xb27
#define GL_CONTEXT_OPENER     0x00000600

static inline void gl_enter_slot(QTestState *qts, uint32_t slot)
{
    mgras_enter_gl_slot(qts, slot, 256, 640, 128, 128, 0x11);
}

/* glOrtho(0, 128, 0, 128, -1, 1): vertices in window coordinates. */
static inline void gl_window_projection(QTestState *qts)
{
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_six(qts, GL_ORTHO_HDR, 0, 128, 0, 128, -1, 1);
}

/*
 * The matrix stack and transform commands, named by the client's own
 * display-list decoder: glPushMatrix 0x2f00, glPopMatrix 0x2e00,
 * glRotatef 0x3010 (angle in degrees, x, y, z), glScalef 0x310c and
 * glTranslatef 0x320c.
 */
#define GL_PUSH_MATRIX        0x00002f00
#define GL_POP_MATRIX         0x00002e00
#define GL_ROTATE_HDR         0x00003010
#define GL_SCALE_HDR          0x0000310c
#define GL_TRANSLATE_HDR      0x0000320c

static inline void gl_three(QTestState *qts, uint32_t header, float x, float y,
                            float z)
{
    gl_word(qts, header);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
}

static inline void gl_rotate(QTestState *qts, float angle, float x, float y,
                             float z)
{
    gl_word(qts, GL_ROTATE_HDR);
    gl_float(qts, angle);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
}

/* A window-unit viewport, glOrtho(0, 128, 0, 128, -1, 1), modelview mode. */
static inline void gl_transform_start(QTestState *qts)
{
    mgras_reset_state(qts);
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
}

/*
 * A small white triangle covering only the pixel whose lower-left corner
 * the modelview maps object (x, y) to, after clearing the window black.
 */
static inline void gl_mark(QTestState *qts, float x, float y)
{
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, x + 0.25f, y + 0.25f);
    gl_vertex2f(qts, x + 0.875f, y + 0.25f);
    gl_vertex2f(qts, x + 0.25f, y + 0.875f);
    gl_word(qts, GL_END_TRIANGLES);
}

#define GL_DITHER_HDR         0x00006704
#define GL_BEGIN_LINES        0x00001700
#define GL_BEGIN_LINE_STRIP   0x00001800
#define GL_BEGIN_LINE_LOOP    0x00001900
#define GL_BEGIN_TRI_STRIP    0x00001b00
#define GL_BEGIN_QUADS        0x00001d00
#define GL_BEGIN_POLYGON      0x00001f00
#define GL_END_OF(begin)      ((begin) + 0x0a00)

static inline uint32_t gl_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline uint32_t gl_attribute_query(QTestState *qts, uint32_t index)
{
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    gl_word(qts, 0x0000a104);
    gl_word(qts, index);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    return qtest_readl(qts, REG_QUERY_ANSWER);
}

/*
 * The state-query routine's 0xa20c query: an index, then a1 and a2, answered at
 * 0x028004 once flag bit 17 is set. The indices are the ones the
 * hq3_revB_HQ4.u symbol table gives, which the library loads for a board
 * reporting GE revision 2 with one GE: GL_MODELVIEW_MATRIX 0x3a + i and
 * GL_PROJECTION_MATRIX 0x4a + i with a1 0, read as floats into the
 * result's element i; GL_MATRIX_MODE 0x29, read as answer + 0x1701;
 * GL_VIEWPORT 0xc1d to 0xc20 and GL_SHADE_MODEL 0xc19 as integers;
 * GL_COLOR_CLEAR_VALUE 0xc0a to 0xc0d as floats.
 */
#define GL_Q_MODELVIEW        0x3a

static inline uint32_t gl_query(QTestState *qts, uint32_t index, uint32_t a1)
{
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    gl_word(qts, MGRAS_QUERY_TOKEN);
    gl_word(qts, index);
    gl_word(qts, a1);
    gl_word(qts, 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    return qtest_readl(qts, REG_QUERY_ANSWER);
}

/*
 * glRasterPos2f and 3f are 0x40003808 and 0x2000380c with their floats. The
 * raster-position queries in the hq3_revB_HQ4.u column: x, y, z and w at
 * 0x359, 0x35a, 0x35b and 0x350, the valid bit at 0x35c, the colour at 0x351
 * to 0x354, the texture coordinates at 0x355 to 0x358 and the distance at
 * 0x34b. The client adds the double -49151.5 to the x and y words before
 * returning them, and ANDs the valid word with a mask from the microcode's
 * symbol table whose value is not recorded.
 */
#define GL_RASTER_POS2F_HDR   0x40003808
#define GL_RASTER_POS3F_HDR   0x2000380c
#define GL_Q_RASTER_X         0x359
#define GL_Q_RASTER_Y         0x35a
#define GL_Q_RASTER_Z         0x35b
#define GL_Q_RASTER_VALID     0x35c
#define GL_Q_RASTER_COLOR     0x351
#define GL_Q_RASTER_TEXCOORD  0x355
#define GL_Q_RASTER_DISTANCE  0x34b
#define GL_RASTER_BIAS        49151.5

static inline void gl_raster_pos2f(QTestState *qts, float x, float y)
{
    gl_word(qts, GL_RASTER_POS2F_HDR);
    gl_float(qts, x);
    gl_float(qts, y);
}
#define GL_TEX_COORD3I_HDR    0x3800080c
#define GL_TEX_COORD2F_HDR    0x40000808
#define GL_TEX_COORD4F_HDR    0x00000810

static inline void gl_migrate(void (*setup)(QTestState *),
                              void (*check)(QTestState *));

static inline void gl_texcoord4f(QTestState *s, float a, float b, float c,
                                 float d)
{
    gl_word(s, GL_TEX_COORD4F_HDR);
    gl_float(s, a);
    gl_float(s, b);
    gl_float(s, c);
    gl_float(s, d);
}

static inline void gl_texcoord_check(QTestState *s, bool raster,
                                     const float v[4])
{
    for (unsigned i = 0; i < 4; i++) {
        uint32_t value = raster ? gl_query(s, GL_Q_RASTER_TEXCOORD + i, 1) :
                                gl_attribute_query(s, 11 + i);

        g_assert_cmphex(value, ==, gl_bits(v[i]));
    }
}

static inline void gl_texcoord_context_setup(QTestState *s)
{
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_three(s, GL_TRANSLATE_HDR, 3, -2, 4);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_texcoord4f(s, 0.25f, 0.5f, -1, 2);
    gl_raster_pos2f(s, 16, 16);
    gl_word(s, GL_BEGIN_LINES);
    gl_vertex2f(s, 8.5f, 8.5f);
    /* Current state changes independently of the saved raster snapshot. */
    gl_word(s, GL_TEX_COORD2F_HDR);
    gl_float(s, -0.25f);
    gl_float(s, 0.75f);

    gl_enter_slot(s, GL_SLOT_B);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(s, GL_TEX_COORD3I_HDR);
    gl_word(s, -3);
    gl_word(s, 5);
    gl_word(s, 7);
    gl_raster_pos2f(s, 16, 16);
}

static inline void gl_texcoord_context_check(QTestState *s)
{
    static const float acurrent[4] = { -0.25f, 0.75f, 0, 1 };
    static const float araster[4] = { 6.25f, -3.5f, 7, 2 };
    static const float b[4] = { -3, 5, 7, 1 };
    static const float initial[4] = { 0, 0, 0, 1 };

    gl_enter_slot(s, GL_SLOT_A);
    gl_texcoord_check(s, false, acurrent);
    gl_texcoord_check(s, true, araster);
    gl_vertex2f(s, 24.5f, 8.5f);
    gl_word(s, GL_END_OF(GL_BEGIN_LINES));
    gl_texcoord_check(s, false, acurrent);
    gl_texcoord_check(s, true, araster);
    gl_enter_slot(s, GL_SLOT_B);
    gl_texcoord_check(s, false, b);
    gl_texcoord_check(s, true, b);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_texcoord_check(s, false, initial);
    gl_texcoord_check(s, true, initial);
    gl_enter_slot(s, GL_SLOT_B);
    gl_texcoord_check(s, false, b);
    gl_texcoord_check(s, true, b);
}

/*
 * glBitmap, as the IRIX 6.5.30 IP27GR532 client's bitmap routine forms
 * it for a small image: 0x9420, a flags word, the width and height, xorig,
 * yorig, xmove and ymove as floats and the image's length in words; then
 * an inline data block of the image's bytes in memory order, its offset
 * in the first word in bits 23:22 when the image is not word aligned, and
 * a zero word when the number of words sent is odd. The flags word is
 * ((8 - ceil(width / 8) % 8) % 8) << 14 | 0xf, 0x1800f for gr_osview's
 * 16-pixel images. The raster-position update routine sends 0xd608 with xmove
 * and ymove for an empty image.
 */
#define GL_BITMAP_HDR         0x00009420
#define GL_INLINE_BLOCK       0x80000000

static inline void gl_bitmap_packet(QTestState *qts, uint32_t width,
                                    uint32_t height, float xorig, float yorig,
                                    float xmove, float ymove)
{
    uint32_t row = (width + 7) / 8;

    gl_word(qts, GL_BITMAP_HDR);
    gl_word(qts, ((8 - row % 8) % 8) << 14 | 0xf);
    gl_word(qts, width);
    gl_word(qts, height);
    gl_float(qts, xorig);
    gl_float(qts, yorig);
    gl_float(qts, xmove);
    gl_float(qts, ymove);
    gl_word(qts, (row * height + 3) / 4);
}

/* The image's block, word aligned, and the zero word an odd count adds. */
static inline void gl_bitmap_image(QTestState *qts, const uint32_t *words,
                                   unsigned n)
{
    gl_word(qts, GL_INLINE_BLOCK | n * 4);
    for (unsigned i = 0; i < n; i++) {
        gl_word(qts, words[i]);
    }
    if (n & 1) {
        gl_word(qts, 0);
    }
}

/*
 * The colour-index client's words (the IRIX 6.5.30 IP27GR532
 * libGLcore.so): the colour-index initializer sends 0x0a04 with the visual's
 * index bits, 8 in gr_osview's context setup, and the RGB initializer 0x0900.
 * glIndexs is 0x19000202 with the short in bits 31:16, glIndexi 0x18000204,
 * glIndexf 0x00000204, glIndexMask 0x3d04 and glClearIndex 0xbd04 with the
 * index as a float.
 */
#define GL_INIT_CI_HDR        0x00000a04
#define GL_INIT_RGB           0x00000900
#define GL_INDEXS_HDR         0x19000202
#define GL_INDEX_MASK_HDR     0x00003d04
#define GL_CLEAR_INDEX_HDR    0x0000bd04
#define GL_BEGIN_POLYGON      0x00001f00
#define GL_END_POLYGON        0x00002900
#define GL_Q_RASTER_INDEX     0x351

static inline void gl_indexs(QTestState *qts, int16_t index)
{
    gl_word(qts, GL_INDEXS_HDR);
    gl_word(qts, (uint32_t)(uint16_t)index << 16);
}

/* The rectangle from window (x0, y0) to (x1, y1) under glOrtho. */
static inline void gl_window_rect(QTestState *qts, float x0, float y0, float x1,
                                  float y1)
{
    gl_word(qts, GL_BEGIN_POLYGON);
    gl_vertex2f(qts, x0, y0);
    gl_vertex2f(qts, x1, y0);
    gl_vertex2f(qts, x1, y1);
    gl_vertex2f(qts, x0, y1);
    gl_word(qts, GL_END_POLYGON);
}

/*
 * A colour-index context on display ID 14 whose map, 29 under the reset
 * mode, gives index 5 red, 0x13 blue and 0x15 green, drawing into its
 * back buffer, which the reset bank words put in bank 1.
 */
static inline void gl_colour_index_back_buffer(QTestState *qts)
{
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_one(qts, GL_INIT_CI_HDR, 8);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    mgras_write_cmap_entry(qts, 29, 5, 0xff000000);
    mgras_write_cmap_entry(qts, 29, 0x13, 0x0000ff00);
    mgras_write_cmap_entry(qts, 29, 0x15, 0x00ff0000);
    gl_draw_buffer(qts, 2);
}

/*
 * Pixel image transfers as the IRIX 6.5.30 IP27GR532 client sends them
 * (hw/misc/sgi-mgras-gl.c, mgras_gl_pixels_start()): GE parameter loads,
 * header 0x8000 or 0xcd00 with the byte length 0x20 + 4 * (a + b) and a
 * seven-word head before two lists; the program at 0x13b, the unpack
 * routine's word in its parameters, the zoom at 0x191 and the window
 * rectangle at 0xe34; then 0x8d20 and eight words, the first the image's
 * words a row; then the image, inline one block a row, or by a DMA
 * transfer kicked with 0xa7. The values are the ones a captured run took from
 * ipaste, imgview and mandel.
 */
#define GL_GE_CRAM            0x80
#define GL_GE_ERAM            0xcd
#define GL_PIXELS_HDR         0x00008d20
#define GL_GE_PROGRAM         0x13b
#define GL_GE_UNPACK          0xd91
#define GL_GE_ZOOM            0x191
#define GL_GE_BOX             0xe34

enum {
    GL_PIX_4UB, GL_PIX_3UB, GL_PIX_INDEX, GL_PIX_SCALED, GL_PIX_4UB_AS_3,
};

static inline void gl_ge_load(QTestState *qts, uint32_t op, uint32_t addr,
                              const uint32_t *words, uint32_t n)
{
    uint32_t len = 0x20 + 4 * n;

    if (len < 0x100) {
        gl_word(qts, op << 8 | len);
    } else {
        gl_word(qts, op << 8);
        gl_word(qts, GL_INLINE_BLOCK | len);
    }
    gl_word(qts, op == GL_GE_CRAM ? n / 5 : n / 4);
    gl_word(qts, op == GL_GE_CRAM ? n % 5 : n % 4);
    gl_word(qts, 4 * n);
    gl_word(qts, addr);
    gl_word(qts, 0);
    gl_word(qts, 2);
    gl_word(qts, n);
    for (uint32_t i = 0; i < n; i++) {
        gl_word(qts, words[i]);
    }
    gl_word(qts, 0);
}

/* The client's loads for a draw of the form kind into window x0..x1. */
static inline void gl_pixels_program(QTestState *qts, int kind, uint32_t bpr,
                                     uint32_t x0, uint32_t y0, uint32_t x1,
                                     uint32_t y1, float zoom)
{
    static const uint32_t programs[][10] = {
        [GL_PIX_4UB] = { 0x1f44, 0, 0x3e65, 0xd91, 0x156a, 0x1b4,
                         0x352d, 0xdf3 },
        [GL_PIX_3UB] = { 0x1f44, 0, 0x1655, 0xd91, 0x3e65, 0x1d6,
                         0x156a, 0x1b4, 0x352d, 0xdf3 },
        [GL_PIX_INDEX] = { 0x1a3e, 0, 0x352d, 0x1d6 },
        [GL_PIX_SCALED] = { 0x1f44, 0, 0x1234, 0x200, 0x3e65, 0xd91,
                            0x156a, 0x1b4, 0x352d, 0xdf3 },
        /* The four-byte program with the three-byte unpack word. */
        [GL_PIX_4UB_AS_3] = { 0x1f44, 0, 0x3e65, 0xd91, 0x156a, 0x1b4,
                              0x352d, 0xdf3 },
    };
    static const uint32_t length[] = { 8, 10, 4, 10, 8 };
    uint32_t unpack[6] = {
        bpr, kind == GL_PIX_3UB || kind == GL_PIX_4UB_AS_3 ? 0x40f2 : 0x4064,
        0x464, 0xb, 0xc, 0x3b808081,
    };
    uint32_t zooms[2];
    uint32_t box[6] = {
        x0, y0, x1, y1, (y1 - y0 + 1) << 16 | (x1 - x0 + 1), 1,
    };
    float z[2] = { 0, 1 / zoom };

    memcpy(zooms, z, sizeof(zooms));
    gl_ge_load(qts, GL_GE_CRAM, GL_GE_PROGRAM, programs[kind], length[kind]);
    if (kind != GL_PIX_INDEX) {
        gl_ge_load(qts, GL_GE_ERAM, GL_GE_UNPACK, unpack, 6);
    }
    gl_ge_load(qts, GL_GE_CRAM, GL_GE_ZOOM, zooms, 2);
    gl_ge_load(qts, GL_GE_ERAM, GL_GE_BOX, box, 6);
}

/* 0x8d20's last word: 0x18 before inline rows, 0x99 before a DMA. */
#define GL_PIXELS_INLINE      0x18
#define GL_PIXELS_BY_DMA      0x99

/*
 * 0x8d20 with a row's words in two parts, first and second, as imgview's
 * client sends them on the SSI; on the SI the second is 0.
 */
static inline void gl_pixels_draw_parts(QTestState *qts, uint32_t first,
                                        uint32_t second, uint32_t flags)
{
    static const uint32_t rest[5] = { 0, 2, 0x2a, 1, 0x49d0 };

    gl_word(qts, GL_PIXELS_HDR);
    gl_word(qts, first);
    gl_word(qts, second);
    for (int i = 0; i < 5; i++) {
        gl_word(qts, rest[i]);
    }
    gl_word(qts, flags);
}

static inline void gl_pixels_draw_as(QTestState *qts, uint32_t row_words,
                                     uint32_t flags)
{
    gl_pixels_draw_parts(qts, row_words, 0, flags);
}

static inline void gl_pixels_draw(QTestState *qts, uint32_t row_words)
{
    gl_pixels_draw_as(qts, row_words, GL_PIXELS_INLINE);
}

/* One row inline, its first byte at byte offset of the first word. */
static inline void gl_pixels_row(QTestState *qts, uint32_t offset,
                                 const uint32_t *words, uint32_t n)
{
    gl_word(qts, GL_INLINE_BLOCK | offset << 22 | n * 4);
    for (uint32_t i = 0; i < n; i++) {
        gl_word(qts, words[i]);
    }
    if (n & 1) {
        gl_word(qts, 0);
    }
}

static inline void gl_pixels_fence(QTestState *qts)
{
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/*
 * The DMA form, as ipaste sends it: a node-local page list beginning at
 * page 7, an image at page 7 of rows pitch bytes apart, and a kick of slot
 * 3 with bit 4 clear.
 */
#define GL_PIX_DMA_LIST       0x2000u
#define GL_PIX_DMA_SRC        0x7000u
#define GL_PIX_DMA_WIDGET     9u

/*
 * The transfer's page list names consecutive pages of node nasid, and the
 * image is written there; on a node other than 0 the same offset on node 0
 * holds 0xee bytes, which is what a transfer that dropped the node bits of
 * a page address would read.
 */
static inline void gl_pixels_dma_on(QTestState *qts, unsigned int nasid,
                                    const uint8_t *image, uint32_t len,
                                    uint32_t pitch, uint32_t rows, uint32_t bpr,
                                    uint32_t kick)
{
    static const uint8_t header[24] = { 0 };
    uint64_t node = (uint64_t)nasid << 20;
    uint32_t pages = DIV_ROUND_UP(len, 4096);
    g_autofree uint8_t *list = g_malloc0(pages * 8);

    g_assert_cmpuint(pages, >, 0);
    for (uint32_t i = 0; i < pages; i++) {
        stq_be_p(list + 8 * i, node | (0x07 + i));
    }
    qtest_memwrite(qts, GL_PIX_DMA_LIST, header, sizeof(header));
    qtest_memwrite(qts, GL_PIX_DMA_LIST + 0x18, list, pages * 8);
    if (nasid) {
        g_autofree uint8_t *decoy = g_malloc(len);

        memset(decoy, 0xee, len);
        qtest_memwrite(qts, GL_PIX_DMA_SRC, decoy, len);
    }
    qtest_memwrite(qts, ((uint64_t)nasid << 32) | GL_PIX_DMA_SRC, image, len);
    /* DMA-map slot 3: registers 0x26/0x27, 0x33 and 0x4e. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482608);
    qtest_writeq(qts, MGRAS_REG(0x130400), GL_PIX_DMA_LIST + 0x18);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00483304 << 32) | pages);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484e04 << 32) | (GL_PIX_DMA_WIDGET << 16));
    gl_word(qts, 0x00080004);
    gl_word(qts, 0x80000000);
    gl_word(qts, 0x00080404);
    gl_word(qts, pitch);
    gl_word(qts, 0x00080504);
    gl_word(qts, 0);
    gl_word(qts, 0x00080604);
    gl_word(qts, 0);
    gl_word(qts, 0x00080704);
    gl_word(qts, rows);
    gl_word(qts, 0x00080804);
    gl_word(qts, bpr);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    gl_word(qts, 0x00080b04);
    gl_word(qts, kick);
}

static inline void gl_pixels_dma(QTestState *qts, const uint8_t *image,
                                 uint32_t len, uint32_t pitch, uint32_t rows,
                                 uint32_t bpr, uint32_t kick)
{
    gl_pixels_dma_on(qts, 0, image, len, pitch, rows, bpr, kick);
}
static inline void gl_color4f(QTestState *qts, float r, float g, float b,
                              float a);
#define TEX_RECORDS           0x00007c08
#define TEX_ON                0x00000b00
#define TEX_OFF               0x00000c00
#define TEX_T_OFFSET          0x00005f04
#define TEX_LOAD_OPEN         0x00009b00
#define TEX_LOAD_CLOSE        0x00009c00
#define TEX_TABLE             0x0000ec10
#define TEX_SAVE_RSS          0x0000d214
#define TEX_RESTORE_RSS       0x0000d300
#define TEX_PAIRS             0x00007f08
#define TEX_TEXMODE1          0x111
#define TEX_TEXMODE2          0x180
#define TEX_TXSIZE            0x181
#define TEX_TXLOD             0x183
#define TEX_TXCLAMPSIZE       0x18c
#define TEX_TXADDR            0x190
#define TEX_TXMIPMAP          0x191
#define TEX_TL_MODE           0x1a1
#define TEX_TL_SPEC           0x1a2
/* The traced values: TEXMODE1 with texturing on, and the RGBA4 load. */
#define TEX_TEXMODE1_ON       0x40859
#define TEX_TL_MODE_RGBA4     0x34006
#define TEX_TL_MODE_RGBA8     0x34026
#define TEX_TEXMODE2_RGBA4    0x10000
#define TEX_TEXMODE2_RGBA8    0x10004
/* A 4-by-2 level 0, and TXSIZE's two copies of its log2 size. */
#define TEX_SPEC_4X2          0x120
#define TEX_TXSIZE_4X2        0x1212

static inline QTestState *tex_start(const char *args)
{
    QTestState *t = qtest_init(args);

    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    return t;
}

/*
 * Raster register records, n (register, value) pairs, as the client's
 * register-write routine.
 */
static inline void tex_records(QTestState *t, const uint32_t *pairs, uint32_t n)
{
    uint32_t words = (n * 3 + 1) & ~1u;

    gl_word(t, TEX_RECORDS);
    gl_word(t, n);
    gl_word(t, words);
    gl_word(t, GL_INLINE_BLOCK | words * 4);
    for (uint32_t i = 0; i < n; i++) {
        gl_word(t, pairs[2 * i]);
        gl_word(t, pairs[2 * i + 1]);
        gl_word(t, 0x18b);
    }
    if (n * 3 & 1) {
        gl_word(t, pairs[0]);
    }
}

static inline void tex_record(QTestState *t, uint32_t reg, uint32_t value)
{
    tex_records(t, (const uint32_t[]){ reg, value }, 1);
}

/*
 * The unpack routine word the client names an image of comps components
 * with, and the scale word for components of csize bytes.
 */
static inline uint32_t tex_unpack_word(uint32_t comps)
{
    static const uint32_t words[] = { 0, 0x420c, 0x417e, 0x40f2, 0x4064 };

    return words[comps];
}

static inline uint32_t tex_unpack_scale(uint32_t csize)
{
    return csize == 2 ? 0x37800080 : 0x3b808081;
}

/*
 * The client's load of one w-by-h image of comps components and bpt bytes
 * a texel through the GE pixel program kind (GL_PIX_4UB or GL_PIX_3UB),
 * with the unpack block that names that shape. TL_MODE goes out only
 * when send_mode is set, as the client skips it on a reload; box is the
 * destination rectangle and size word.
 */
static inline void tex_load_program(QTestState *t, int kind, bool send_mode,
                                    uint32_t mode, uint32_t spec,
                                    const uint8_t *texels, uint32_t w,
                                    uint32_t h, uint32_t comps, uint32_t bpt,
                                    const uint32_t box[6], bool bracket,
                                    unsigned int nasid, uint32_t src_bytes)
{
    static const uint32_t draw[8] = { 0, 0, 0, 1, 0, 1, 0x511a, 0x98 };
    uint32_t bytes = w * h * bpt;
    uint32_t unpack[6] = {
        DIV_ROUND_UP(bytes, 4), tex_unpack_word(comps), 0x464, 0xb, 0xc,
        tex_unpack_scale(bpt / comps),
    };

    if (send_mode) {
        tex_record(t, TEX_TL_MODE, mode);
    }
    tex_record(t, TEX_TL_SPEC, spec);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    if (bracket) {
        gl_word(t, TEX_LOAD_OPEN);
    }
    gl_pixels_program(t, kind, DIV_ROUND_UP(bytes, 4), 0, 0, w - 1, h - 1, 1);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_UNPACK, unpack, 6);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, 6);
    gl_word(t, TEX_PAIRS);
    gl_word(t, 1);
    gl_word(t, 2);
    gl_word(t, GL_INLINE_BLOCK | 8);
    gl_word(t, 0x159);
    gl_word(t, 0xc00080);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, DIV_ROUND_UP(bytes, 4));
    for (int i = 1; i < 8; i++) {
        gl_word(t, draw[i]);
    }
    if (src_bytes) {
        /* Pitch 0 rereads the one source row for every row of the level. */
        gl_pixels_dma_on(t, nasid, texels, src_bytes, 0, h, w * bpt, 0xa7);
    } else {
        gl_pixels_dma_on(t, nasid, texels, bytes, bytes, 1, bytes, 0xa7);
    }
    if (bracket) {
        gl_word(t, TEX_LOAD_CLOSE);
    }
    gl_word(t, TEX_RESTORE_RSS);
    tex_record(t, TEX_TXSIZE, 0);
}

/* The load through the four-unsigned-byte program. */
static inline void tex_load_fmt(QTestState *t, bool send_mode, uint32_t mode,
                                uint32_t spec, const uint8_t *texels,
                                uint32_t w, uint32_t h, uint32_t comps,
                                uint32_t bpt, const uint32_t box[6],
                                bool bracket, unsigned int nasid,
                                uint32_t src_bytes)
{
    tex_load_program(t, GL_PIX_4UB, send_mode, mode, spec, texels, w, h,
                     comps, bpt, box, bracket, nasid, src_bytes);
}

/* The same load for a level of four unsigned bytes a texel. */
static inline void tex_load_full(QTestState *t, bool send_mode, uint32_t mode,
                                 uint32_t spec, const uint8_t *texels,
                                 uint32_t w, uint32_t h, const uint32_t box[6],
                                 bool bracket, unsigned int nasid,
                                 uint32_t src_bytes)
{
    tex_load_fmt(t, send_mode, mode, spec, texels, w, h, 4, 4, box, bracket,
                 nasid, src_bytes);
}

static inline void tex_load_as(QTestState *t, bool send_mode, uint32_t mode,
                               uint32_t spec, const uint8_t *texels, uint32_t w,
                               uint32_t h, const uint32_t box[6], bool bracket)
{
    tex_load_full(t, send_mode, mode, spec, texels, w, h, box, bracket, 0, 0);
}

static inline void tex_load(QTestState *t, uint32_t mode, const uint8_t *texels)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };

    tex_load_as(t, true, mode, TEX_SPEC_4X2, texels, 4, 2, box, true);
}

/* The draw setup for a 4-by-2 level 0, then texturing on. */
static inline void tex_bind(QTestState *t, uint32_t texmode1, uint32_t texmode2)
{
    tex_record(t, TEX_TEXMODE2, texmode2);
    gl_word(t, TEX_ON);
    tex_record(t, TEX_TEXMODE1, texmode1);
    gl_one(t, TEX_T_OFFSET, 0);
    tex_records(t, (const uint32_t[]){ 0x184, 0, 0x185, 0, TEX_TXADDR, 0 },
                3);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, 0);
    tex_records(t, (const uint32_t[]){ TEX_TXSIZE, TEX_TXSIZE_4X2,
                                       TEX_TXLOD, 1,
                                       TEX_TXCLAMPSIZE, 0x8002 }, 3);
}

/*
 * A white quad over window pixels 16 to 47 across and 16 to 31 up,
 * texture coordinates 0 to 1, so each texel of a 4-by-2 level covers 8
 * by 8 pixels, then the client's close of the draw.
 */
static inline void tex_quad_rect(QTestState *t, const float c[4],
                                 const float v[4][4])
{
    gl_color4f(t, c[0], c[1], c[2], c[3]);
    gl_word(t, GL_BEGIN_QUADS);
    for (int i = 0; i < 4; i++) {
        gl_word(t, GL_TEX_COORD2F_HDR);
        gl_float(t, v[i][2]);
        gl_float(t, v[i][3]);
        gl_vertex2f(t, v[i][0], v[i][1]);
    }
    gl_word(t, GL_END_OF(GL_BEGIN_QUADS));
    gl_word(t, 0x0000e910);
    for (int i = 0; i < 4; i++) {
        gl_word(t, i == 0);
    }
    gl_one(t, TEX_T_OFFSET, 0);
    gl_word(t, TEX_OFF);
}

static inline void tex_quad(QTestState *t)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float v[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 1, 0 }, { 48, 32, 1, 1 }, { 16, 32, 0, 1 },
    };

    tex_quad_rect(t, white, v);
}

/* The centre pixel of texel (x, y) under tex_quad(). */
static inline uint32_t tex_texel_pixel(QTestState *t, int x, int y)
{
    return gl_window_pixel(t, 20 + 8 * x, 20 + 8 * y);
}

/* Texel i of a 4-by-2 level: R, G, B distinct in the high four bits. */
static inline void tex_texels(uint8_t texels[32])
{
    for (int i = 0; i < 8; i++) {
        texels[4 * i] = i << 4 | 0x3;
        texels[4 * i + 1] = (7 - i) << 5;
        texels[4 * i + 2] = 0x80 + i;
        texels[4 * i + 3] = 0xff;
    }
}

/* Public OpenBSD FIFO packets, independent of the high-port helpers. */
static inline void mgras_public_write(QTestState *s, unsigned reg,
                                      uint32_t value, bool execute)
{
    uint64_t packet = 0x0018000400000000ULL | ((uint64_t)reg << 40) | value;

    if (execute) {
        packet |= 0x0004000000000000ULL;
    }
    qtest_writeq(s, MGRAS_REG(0x020400), packet);
}

static inline void mgras_public_setup(QTestState *s, bool bitmap)
{
    mgras_public_write(s, 0x161, 0x0c006300, false);
    mgras_public_write(s, 0x110, bitmap ? 0x00400018 : 0, false);
    mgras_public_write(s, 0x163, 0xffffff, false);
    mgras_public_write(s, 0x164, 0xffffff, false);
    mgras_public_write(s, 0x162, 0, false);
    mgras_public_write(s, 0x5b, 0x00332211, false);
    mgras_public_write(s, 0x140, 0x00b00a00, false);
    mgras_public_write(s, 0x141, 0x00000d00, false);
    /* A distinct unrelated legacy fill colour catches source confusion. */
    mgras_public_write(s, 0x176, 0x990, false);
}

static inline void mgras_public_rect(QTestState *s, unsigned x, unsigned y,
                                     unsigned width, unsigned height,
                                     bool execute)
{
    mgras_public_write(s, 0x46, (x << 16) | y, false);
    mgras_public_write(s, 0x47, ((x + width - 1) << 16) | (y + height - 1),
                       false);
    mgras_public_write(s, 0x45, 0x18, execute);
}

/*
 * The context image's clip words, as the IRIX 6.5.30 kernel's
 * clip validation routine in mgras.o writes them: word
 * 3 is (mode << 4) | enables, and enable bit i selects rectangle i, whose
 * columns and rows are words 11 and 12, 9 and 10, 7 and 8 or 5 and 6,
 * each first << 16 | last counted from the bottom-left of the screen. A
 * window with one to four visible rectangles gets mode 1; five or more,
 * mode 1 and no enables; the kernel's null clip, mode 2 with rectangles 0
 * and 1 the whole screen and the origin at row 1024.
 */
static inline void mgras_enter_gl_clip(QTestState *qts, uint32_t origin,
                                       uint32_t clip,
                                       const uint32_t rects[4][2])
{
    static const uint8_t words[4][2] = {
        { 11, 12 }, { 9, 10 }, { 7, 8 }, { 5, 6 },
    };
    uint32_t image[MGRAS_CONTEXT_WORDS];

    memcpy(image, gl_context_image, sizeof(image));
    image[2] = image[14] = origin;
    image[3] = clip;
    for (int i = 0; i < 4; i++) {
        image[words[i][0]] = rects[i][0];
        image[words[i][1]] = rects[i][1];
    }
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, image[i]);
    }
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
}

#define GL_CLIP_SPAN(first, last) (((first) << 16) | (last))

/*
 * The X server's clip-ID fill: DRBPOINTERS 0x90240, COLORMASKMSBS 0xff,
 * COLORMASKLSBSA/B 0, PP1FILLMODE 0x142600, the ID in FILLCOLOR_R, over
 * screen columns x0-x1 and rows y0-y1 counted from the top.
 */
static inline void gl_clip_id_fill(QTestState *qts, uint32_t id, uint32_t x0,
                                   uint32_t y0, uint32_t x1,
                                   uint32_t y1)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_DRBPOINTERS, 0x00090240));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKMSBS, 0xff));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, 0x142600));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, id));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (x0 << 16) | y0));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (x1 << 16) | y1));
}

/* A whole-screen window, mode 1 rectangle 0, with image word 4 as given. */
static inline void gl_enter_clip_id_window(QTestState *qts, uint32_t word4)
{
    uint32_t image[MGRAS_CONTEXT_WORDS];

    memcpy(image, gl_context_image, sizeof(image));
    image[2] = image[14] = 0;
    image[3] = 0x11;
    image[4] = word4;
    image[11] = GL_CLIP_SPAN(0, 1279);
    image[12] = GL_CLIP_SPAN(0, 1023);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, image[i]);
    }
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
}

/*
 * Migrate a guest that setup has driven to a fresh destination, and hand
 * the destination to check.
 */
static inline void gl_migrate(void (*setup)(QTestState *),
                              void (*check)(QTestState *))
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-state-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "migrate.bin", NULL);
    uri = g_strdup_printf("file:%s", path);
    from = qtest_init(MIGRATION_ARGS);

    setup(from);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    sgi_wait_migration_status(to, "completed", 30);
    check(to);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    g_remove(path);
    g_rmdir(tmpfs);
}

/*
 * glDepthRange is 0x4808 and near and far as floats, which the client does not
 * clamp; GL_DEPTH_RANGE is queried at
 * 0xc21 and 0xc22 and read as floats. OpenGL 1.2.1 (2.10.1) starts the range at
 * [0, 1] and clamps each value to [0, 1].
 */
#define GL_DEPTH_RANGE_HDR    0x00004808
#define GL_Q_DEPTH_RANGE      0xc21

static inline void gl_depth_range(QTestState *qts, float n, float f)
{
    gl_word(qts, GL_DEPTH_RANGE_HDR);
    gl_float(qts, n);
    gl_float(qts, f);
}

static inline uint32_t gl_stack_depth(uint32_t answer, uint32_t base)
{
    return ((answer - base) >> 4) + 1;
}

/*
 * The raster-state queries: 0x2ca holds
 * the depth function in bits 2:0, read as GL_NEVER + (answer & 7), and
 * the write mask, true when bits 27:4 are all set; 0x288 holds dither in
 * bit 0, stencil and alpha test in bits 4 and 6 and the logic op in bits
 * 29:26, read as GL_CLEAR + op. glEnable and glDisable of GL_DITHER send
 * 0x6704 with 3 and 0; glDepthFunc 0x4304 the function less GL_NEVER;
 * glDepthMask 0x3c04 the flag. Stencil, alpha and logic op are not
 * tracked and answer OpenGL's initial state; so do 0x286, 0x2c6, 0x2c8
 * and 0x2cc, packed as the context setup's 0x7c08 block gives them.
 */
#define GL_DITHER_HDR         0x00006704
#define GL_Q_FILL_ENABLES     0x286
#define GL_Q_PIXEL_ENABLES    0x288
#define GL_Q_STENCIL          0x2c6
#define GL_Q_STENCIL_MASKS    0x2c8
#define GL_Q_ALPHA            0x2cc

/*
 * Lighting as the client sends it:
 * glLightfv one header per parameter with the light's number and floats,
 * glMaterialfv 0x1118 with face, parameter and four floats,
 * glLightModelfv(GL_LIGHT_MODEL_AMBIENT) 0x570c with three floats,
 * GL_LIGHT_MODEL_TWO_SIDE 0x5904 with an integer, glColorMaterial on the
 * front face 0xbf04 with the parameter, glEnable(GL_COLOR_MATERIAL)
 * 0xc200. glGetLightfv sends 0xef08 with the light's number and the
 * parameter, glGetMaterialfv 0xee08 with the face and the parameter, and
 * both then query 0x3ba to 0x3bd, reading four floats.
 */
#define GL_LIGHT_DIFFUSE_HDR  0x00001314
#define GL_MATERIAL_HDR       0x00001118
#define GL_LIGHT_MODEL_AMBIENT_HDR 0x0000570c
#define GL_LIGHT_MODEL_TWO_SIDE_HDR 0x00005904
#define GL_COLOR_MATERIAL_ON  0x0000c200
#define GL_GET_LIGHT_HDR      0x0000ef08
#define GL_Q_READBACK         0x3ba
#define GL_FRONT_FACE         0x404
#define GL_BACK_FACE          0x405
#define GL_DIFFUSE            0x1201

static inline void gl_light4(QTestState *qts, uint32_t header, uint32_t light,
                             float x, float y, float z, float w)
{
    gl_word(qts, header);
    gl_word(qts, light);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
    gl_float(qts, w);
}

static inline void gl_material(QTestState *qts, uint32_t face, uint32_t pname,
                               float r, float g, float b, float a)
{
    gl_word(qts, GL_MATERIAL_HDR);
    gl_word(qts, face);
    gl_word(qts, pname);
    gl_float(qts, r);
    gl_float(qts, g);
    gl_float(qts, b);
    gl_float(qts, a);
}

/* The four readback words after glGetLightfv or glGetMaterialfv. */
static inline void gl_readback(QTestState *qts, uint32_t header, uint32_t a,
                               uint32_t pname, const float expect[4])
{
    gl_word(qts, header);
    gl_word(qts, a);
    gl_word(qts, pname);
    for (int k = 0; k < 4; k++) {
        g_assert_cmphex(gl_query(qts, GL_Q_READBACK + k, 1), ==,
                        gl_bits(expect[k]));
    }
}

/*
 * The lighting enables and normals as the client sends them:
 * glEnable and glDisable(GL_LIGHTING) 0x0d00 and 0x0e00 alone,
 * glEnable(GL_LIGHTi) 0x7104 and i, glDisable 0xa004 and i,
 * glEnable/glDisable(GL_NORMALIZE) 0xa304 with 1 or 0, glNormal3f 0x070c,
 * glNormal3d 0x12000718 with three doubles high word first, glNormal3i
 * 0x1c00070c, glNormal3s 0x1d000706 with x and y in one word and z in the
 * high half of a second, glNormal3b 0x1c800703 with x, y and z from bit
 * 31 down, and glLightModel(GL_LIGHT_MODEL_TWO_SIDE) 0x5904.
 */
#define GL_LIGHTING_ON        0x00000d00
#define GL_LIGHT_ON_HDR       0x00007104
#define GL_NORMALIZE_HDR      0x0000a304
#define GL_NORMAL3F_HDR       0x0000070c

/*
 * IRIS GL's colour and normal rule as the client sends it for
 * glIglooInterfaceSGIX: 0x0000dd00 then 0x00010200 turn it on in
 * LMC_COLOR mode, 0x0000df00 and 0x0000de00 turn it off.
 */
#define GL_IRIS_LMC_COLOR     0x0000dd00
#define GL_IRIS_LMC_OTHER     0x0000de00
#define GL_IRIS_RULE_OFF      0x0000df00
#define GL_IRIS_RULE_ON       0x00010200

/*
 * A flat, lit scene in the probe window: light 0 as OpenGL starts it,
 * white along +z, enabled with lighting, and the vertex colour green, which
 * a lit vertex does not take. Unchanged, a front material ambient 0.2
 * under light model ambient 0.2 gives 0.04, 10 in eight bits.
 */
static inline void gl_lit_start(QTestState *qts)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_LIGHT_ON_HDR, 0);
    gl_word(qts, GL_LIGHTING_ON);
    gl_color3f(qts, 0, 1, 0);
}

/*
 * A triangle over window pixel (64, 50), counter-clockwise, or clockwise
 * for back, and that pixel afterwards.
 */
static inline uint32_t gl_lit_triangle(QTestState *qts, bool back)
{
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -0.9f, -0.9f, 0);
    gl_vertex3f(qts, back ? 0 : 0.9f, back ? 0.9f : -0.9f, 0);
    gl_vertex3f(qts, back ? 0.9f : 0, back ? -0.9f : 0.9f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    return gl_window_pixel(qts, 64, 50);
}

/*
 * Polygon culling as the client sends it (the cull-face routine, the
 * front-face routine, and the enable and disable routines for
 * GL_CULL_FACE): 0x7404 and the face, 0x5604 and the direction,
 * 0x7200 and 0x7300 alone.
 */
#define GL_CULL_ON            0x00007200
#define GL_CULL_FACE_HDR      0x00007404
#define GL_FRONT_FACE_HDR     0x00005604
#define GL_FRONT_AND_BACK     0x408
#define GL_CW                 0x900

/*
 * The per-fragment operations as the client sends them. Each scene draws
 * over the probe window's 128x128 viewport; gl_fill_window() covers all
 * of it in one colour at normalized-device depth z.
 */
static inline void gl_fill_window_at(QTestState *qts, float r, float g, float b,
                                     float z)
{
    gl_color3f(qts, r, g, b);
    gl_word(qts, GL_BEGIN_QUADS);
    gl_vertex3f(qts, -1, -1, z);
    gl_vertex3f(qts, 1, -1, z);
    gl_vertex3f(qts, 1, 1, z);
    gl_vertex3f(qts, -1, 1, z);
    gl_word(qts, GL_END_OF(GL_BEGIN_QUADS));
}

static inline void gl_fill_window(QTestState *qts, float r, float g, float b)
{
    gl_fill_window_at(qts, r, g, b, 0);
}

/*
 * glScissor (the scissor routine): 0x3910 and x, width - 1, y,
 * height - 1; glEnable and glDisable of GL_SCISSOR_TEST: 0x6e04 and 1 or
 * 0. GL_SCISSOR_BOX is queried at 0xc24 to 0xc27 as x, y and the last
 * column and row, and GL_SCISSOR_TEST at 0xc28.
 */
#define GL_SCISSOR_HDR        0x00003910
#define GL_SCISSOR_TEST_HDR   0x00006e04
#define GL_Q_SCISSOR_BOX      0xc24
#define GL_Q_SCISSOR_TEST     0xc28

static inline void gl_scissor(QTestState *qts, int32_t x, int32_t y, int32_t w,
                              int32_t h)
{
    gl_word(qts, GL_SCISSOR_HDR);
    gl_word(qts, x);
    gl_word(qts, w - 1);
    gl_word(qts, y);
    gl_word(qts, h - 1);
}

/*
 * glAlphaFunc (the alpha-function routine): 0x3e08, the function less
 * GL_NEVER and the reference as a 12-bit fraction; glEnable and glDisable
 * of GL_ALPHA_TEST: 0x6404 and 1 or 0. GL_ALPHA_TEST is bit 6 of query
 * 0x288, and GL_ALPHA_TEST_FUNC and _REF bits 2:0 and 14:3 of 0x2cc.
 */
#define GL_ALPHA_FUNC_HDR     0x00003e08
#define GL_ALPHA_TEST_HDR     0x00006404
#define GL_COLOR4F_HDR        0x00000210
#define GL_GREATER            4

static inline void gl_color4f(QTestState *qts, float r, float g, float b,
                              float a)
{
    gl_word(qts, GL_COLOR4F_HDR);
    gl_float(qts, r);
    gl_float(qts, g);
    gl_float(qts, b);
    gl_float(qts, a);
}

static inline void gl_alpha_func(QTestState *qts, uint32_t func, uint32_t ref12)
{
    gl_word(qts, GL_ALPHA_FUNC_HDR);
    gl_word(qts, func);
    gl_word(qts, ref12);
}

/* A window of colour (r, g, b, a) and one pixel of it afterwards. */
static inline uint32_t gl_alpha_window(QTestState *qts, float a)
{
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color4f(qts, 0, 1, 0, a);
    gl_word(qts, GL_BEGIN_QUADS);
    gl_vertex2f(qts, -1, -1);
    gl_vertex2f(qts, 1, -1);
    gl_vertex2f(qts, 1, 1);
    gl_vertex2f(qts, -1, 1);
    gl_word(qts, GL_END_OF(GL_BEGIN_QUADS));
    return gl_window_pixel(qts, 64, 64);
}

/*
 * The stencil test as the client sends it: glStencilFunc 0x410c with the
 * function less GL_NEVER, the reference and the mask; glStencilOp 0x420c
 * with GL_KEEP 0, GL_ZERO 1, GL_REPLACE 2, GL_INCR 3, GL_DECR 4 and
 * GL_INVERT 5; glStencilMask 0x3a04; glClearStencil 0xbe04; glClear's
 * stencil buffer 0x4c00; GL_STENCIL_TEST 0x6f04 and 1 or 0. Query 0x288
 * bit 4 is the enable, 0x2c6 holds the function in bits 2:0, the
 * operations in 5:3, 8:6 and 11:9 and the reference in 19:12, 0x2c8 the
 * value and write masks in bits 7:0 and 15:8, and 0xc14 the clear value.
 */
#define GL_STENCIL_FUNC_HDR   0x0000410c
#define GL_STENCIL_OP_HDR     0x0000420c
#define GL_STENCIL_MASK_HDR   0x00003a04
#define GL_CLEAR_STENCIL_HDR  0x0000be04
#define GL_CLEAR_STENCIL_BUFFER 0x00004c00
#define GL_STENCIL_TEST_HDR   0x00006f04
#define GL_CLEAR_DEPTH_BUFFER 0x00004a00
#define GL_STENCIL_EQUAL      2
#define GL_STENCIL_ALWAYS     7
#define GL_OP_KEEP            0
#define GL_OP_REPLACE         2
#define GL_OP_INCR            3

static inline void gl_stencil(QTestState *qts, uint32_t func, uint32_t ref,
                              uint32_t mask, uint32_t fail, uint32_t zfail,
                              uint32_t zpass)
{
    gl_word(qts, GL_STENCIL_FUNC_HDR);
    gl_word(qts, func);
    gl_word(qts, ref);
    gl_word(qts, mask);
    gl_word(qts, GL_STENCIL_OP_HDR);
    gl_word(qts, fail);
    gl_word(qts, zfail);
    gl_word(qts, zpass);
}

/*
 * Blending as the client sends it: glBlendFunc 0x3f08 and the two
 * factors' enumerants, glBlendEquationEXT 0x6004 and the equation's,
 * glBlendColorEXT 0xd910 and four floats, GL_BLEND 0x6504 and 1 or 0.
 * GL_BLEND is query 0xc4a and GL_BLEND_SRC, _DST and _EQUATION 0xc4b to
 * 0xc4d.
 */
#define GL_BLEND_FUNC_HDR     0x00003f08
#define GL_BLEND_EQUATION_HDR 0x00006004
#define GL_BLEND_COLOR_HDR    0x0000d910
#define GL_BLEND_HDR          0x00006504
#define GL_SRC_ALPHA          0x302
#define GL_FUNC_ADD           0x8006

static inline void gl_blend(QTestState *qts, uint32_t src, uint32_t dst,
                            uint32_t equation)
{
    gl_word(qts, GL_BLEND_FUNC_HDR);
    gl_word(qts, src);
    gl_word(qts, dst);
    gl_one(qts, GL_BLEND_EQUATION_HDR, equation);
}

/*
 * The logical operation as the client sends it: glLogicOp 0x4004 and the
 * operation less GL_CLEAR, 0x6b04 and 1 or 0 to enable it; query 0x288
 * holds the operation in bits 29:26.
 */
#define GL_LOGIC_OP_HDR       0x00004004
#define GL_LOGIC_OP_ENABLE_HDR 0x00006b04
#define GL_LOGIC_XOR          6

/*
 * glColorMask as the client sends it: 0x3b04 and red in bit 0, green 1,
 * blue 2, alpha 3, the bits the write-mask query 0x32b answers in an RGBA
 * context.
 */
#define GL_COLOR_MASK_HDR     0x00003b04

/*
 * Fog as the client sends it: GL_FOG_COLOR 0xb410 and four floats,
 * GL_FOG_START 0xb504, GL_FOG_END 0xb604 and GL_FOG_DENSITY 0xb704 and a
 * float, GL_FOG_INDEX 0xb804 and GL_FOG_MODE 0xb904 and an integer,
 * GL_FOG 0x6804 and 1 or 0. Query 0x286 holds the enable in bit 17, 0xbce
 * the mode, 0xbcf to 0xbd1 density, start and end, 0xbd2 to 0xbd5 the
 * colour and 0xbd6 the index.
 */
#define GL_FOG_COLOR_HDR      0x0000b410
#define GL_FOG_START_HDR      0x0000b504
#define GL_FOG_END_HDR        0x0000b604
#define GL_FOG_MODE_HDR       0x0000b904
#define GL_FOG_HDR            0x00006804
#define GL_LINEAR             0x2601

static inline void gl_fog_linear(QTestState *qts, float start, float end)
{
    gl_word(qts, GL_FOG_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);
    gl_float(qts, 0);
    gl_float(qts, 1);
    gl_one(qts, GL_FOG_MODE_HDR, GL_LINEAR);
    gl_word(qts, GL_FOG_START_HDR);
    gl_float(qts, start);
    gl_word(qts, GL_FOG_END_HDR);
    gl_float(qts, end);
}

/*
 * Polygon offset as the client sends it: 0xe70c, the factor, the bias
 * and 1.0 for glPolygonOffsetEXT, or the factor, the units and 1.1 for
 * glPolygonOffset; 0xe804 and the fill, line and point enables in bits 0
 * to 2. GL_POLYGON_OFFSET_FACTOR, _BIAS_EXT and _UNITS are queries 0xca2
 * to 0xca4.
 */
#define GL_POLYGON_OFFSET_HDR 0x0000e70c
#define GL_OFFSET_ENABLES_HDR 0x0000e804

static inline void gl_polygon_offset(QTestState *qts, float factor, float c,
                                     bool ext)
{
    gl_word(qts, GL_POLYGON_OFFSET_HDR);
    gl_float(qts, factor);
    gl_float(qts, c);
    gl_float(qts, ext ? 1.0f : 1.1f);
}

/*
 * Client clip planes as the client sends them: glClipPlane 0xa514, the
 * plane's enumerant and four floats in object coordinates; 0xa604 and
 * 0xa704 and the enumerant to enable and disable one. Query 0xb87 + i's
 * sign bit is GL_CLIP_PLANEi's enable, and 0xb8d + 4i + k coefficient k.
 */
#define GL_CLIP_PLANE_HDR     0x0000a514
#define GL_CLIP_PLANE_ON_HDR  0x0000a604
#define GL_CLIP_PLANE_OFF_HDR 0x0000a704
#define GL_CLIP_PLANE0        0x3000

static inline void gl_clip_plane(QTestState *qts, uint32_t plane, float a,
                                 float b, float c, float d)
{
    gl_word(qts, GL_CLIP_PLANE_HDR);
    gl_word(qts, GL_CLIP_PLANE0 + plane);
    gl_float(qts, a);
    gl_float(qts, b);
    gl_float(qts, c);
    gl_float(qts, d);
}

/*
 * Point, line and polygon state as the client sends it: glLineWidth
 * 0x4504 and glPointSize 0x4d04 and a float; glLineStipple 0x4608, the
 * repeat less 1 and the pattern bit-reversed; glPolygonMode 0xa808, face
 * and mode; GL_LINE_SMOOTH 0x6904, GL_LINE_STIPPLE 0x6a04,
 * GL_POINT_SMOOTH 0x6c04 and GL_POLYGON_STIPPLE 0x6d04 and 1 or 0. The
 * queries: 0xc3d and 0xc3e the size and width, 0xc3f and 0xc40 the cull
 * face and front face, 0xc17 and 0xc18 the polygon modes, 0xc30 the line
 * smoothing, 0xc46 and 0xc47 the stipple as it was sent, and bits 0 and 5
 * of 0x286 the polygon and line stipple enables.
 */
#define GL_LINE_WIDTH_HDR     0x00004504
#define GL_POINT_SIZE_HDR     0x00004d04
#define GL_LINE_STIPPLE_HDR   0x00004608
#define GL_POLYGON_MODE_HDR   0x0000a808
#define GL_LINE_SMOOTH_HDR    0x00006904
#define GL_LINE_STIPPLE_ON_HDR 0x00006a04
#define GL_POLYGON_STIPPLE_HDR 0x00006d04
#define GL_Q_POINT_SIZE       0xc3d
#define GL_Q_LINE_SMOOTH      0xc30
#define GL_LINE_MODE          0x1b01

static inline void gl_width_clear(QTestState *qts)
{
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
}

static inline void gl_polygon_mode(QTestState *s, uint32_t face, uint32_t mode)
{
    gl_word(s, GL_POLYGON_MODE_HDR);
    gl_word(s, face);
    gl_word(s, mode);
}

static inline void gl_native_stipple(QTestState *s, unsigned repeat,
                                     uint16_t packet_pattern)
{
    gl_word(s, GL_LINE_STIPPLE_HDR);
    gl_word(s, repeat - 1);
    gl_word(s, packet_pattern);
    gl_one(s, GL_LINE_STIPPLE_ON_HDR, 1);
}

static inline void gl_native_stipple_bind(QTestState *s, uint32_t clip,
                                          bool outside)
{
    uint32_t rects[4][2];

    for (unsigned i = 0; i < 4; i++) {
        rects[i][0] = outside ? GL_CLIP_SPAN(1400, 1500) :
                               GL_CLIP_SPAN(256, 383);
        rects[i][1] = outside ? GL_CLIP_SPAN(1100, 1200) :
                               GL_CLIP_SPAN(640, 767);
        if (clip == 0x20) {
            rects[i][0] = GL_CLIP_SPAN(0, 1279);
            rects[i][1] = GL_CLIP_SPAN(0, 1023);
        }
    }
    mgras_enter_gl_clip(s, (640 << 16) | 256, clip, rects);
}

static inline void gl_native_stipple_hidden_setup(QTestState *s, uint32_t clip,
                                                  bool outside,
                                                  uint16_t pattern)
{
    mgras_enter_gl_window(s, 256, 640, 128, 128, 0x11);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_color3f(s, 1, 1, 1);
    gl_draw_buffer(s, 3);
    gl_one(s, GL_DEPTH_TEST_HDR, 1);
    gl_one(s, GL_DEPTH_FUNC_HDR, GL_DEPTH_ALWAYS);
    gl_stencil(s, GL_STENCIL_ALWAYS, 7, 0xff, GL_OP_KEEP, GL_OP_KEEP,
                GL_OP_REPLACE);
    gl_one(s, GL_STENCIL_TEST_HDR, 1);
    gl_native_stipple(s, 1, pattern);
    gl_native_stipple_bind(s, clip, outside);
    gl_word(s, GL_BEGIN_LINE_STRIP);
    gl_vertex2f(s, 8.5f, 8.5f);
    gl_vertex2f(s, 13.5f, 8.5f);
    for (unsigned x = 8; x < 13; x++) {
        g_assert_cmphex(gl_window_pixel(s, x, 8), ==, 0);
        g_assert_cmphex(mgras_qom_get_uint(s, "x-test-back-pixel"), ==, 0);
        g_assert_cmphex(mgras_qom_get_uint(s, "x-test-depth-stencil"), ==, 0);
    }
}

static inline void gl_native_stipple_hidden_check(QTestState *s)
{
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 5);
    gl_native_stipple_bind(s, 0x11, false);
    gl_vertex2f(s, 17.5f, 8.5f);
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 9);
    gl_word(s, GL_END_OF(GL_BEGIN_LINE_STRIP));
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 0);
    for (unsigned x = 8; x < 18; x++) {
        g_assert_cmphex(gl_window_pixel(s, x, 8), ==,
                        x == 13 ? 0xffffff : 0);
        g_assert_cmphex(mgras_qom_get_uint(s, "x-test-back-pixel"), ==,
                        x == 13 ? 0xffffff : 0);
        g_assert_cmphex(mgras_qom_get_uint(s, "x-test-depth-stencil"), ==,
                        x == 13 ? 0x07800000 : 0);
    }
}

static inline void gl_native_stipple_slot(QTestState *s, uint32_t slot)
{
    gl_enter_slot(s, slot);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_color3f(s, 1, 1, 1);
}

static inline void gl_native_stipple_loop_setup(QTestState *s)
{
    gl_native_stipple_slot(s, GL_SLOT_A);
    gl_native_stipple(s, 1, 0x0040); /* API bit nine selects the closure. */
    gl_word(s, GL_BEGIN_LINE_LOOP);
    gl_vertex2f(s, 8.5f, 8.5f);
    gl_vertex2f(s, 13.5f, 8.5f);
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 5);
}

static inline void gl_native_stipple_loop_check(QTestState *s)
{
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 5);
    gl_vertex2f(s, 17.5f, 8.5f);
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 9);
    g_assert_cmphex(gl_window_pixel(s, 17, 8), ==, 0);
    gl_word(s, GL_END_OF(GL_BEGIN_LINE_LOOP));
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-line-phase"), ==, 0);
    for (unsigned x = 8; x <= 18; x++) {
        g_assert_cmphex(gl_window_pixel(s, x, 8), ==,
                        x == 17 ? 0xffffff : 0);
    }
}

/* Collinear vertices keep an analytical rectangle while exceeding four. */
static inline void gl_polygon_open(QTestState *s, float dx)
{
    static const float xy[6][2] = {
        { 8.5f, 8.5f }, { 16.5f, 8.5f }, { 24.5f, 8.5f },
        { 24.5f, 24.5f }, { 16.5f, 24.5f }, { 8.5f, 24.5f },
    };

    gl_word(s, 0x00000f00);
    gl_word(s, GL_BEGIN_POLYGON);
    for (unsigned i = 0; i < ARRAY_SIZE(xy); i++) {
        gl_vertex2f(s, xy[i][0] + dx, xy[i][1]);
    }
}

static inline void gl_polygon_check_owned(QTestState *s, unsigned contexts,
                                          unsigned vertices)
{
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-polygon-contexts"), ==,
                     contexts);
    g_assert_cmpuint(mgras_qom_get_uint(s, "x-test-polygon-vertices"), ==,
                     vertices);
}

static inline void gl_polygon_slot(QTestState *s, uint32_t slot)
{
    gl_enter_slot(s, slot);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_polygon_mode(s, GL_FRONT_AND_BACK, GL_LINE_MODE);
}

static inline void gl_polygon_context_setup(QTestState *s)
{
    gl_polygon_slot(s, GL_SLOT_A);
    gl_width_clear(s);
    gl_color3f(s, 1, 0, 0);
    gl_polygon_open(s, 0);
    gl_polygon_slot(s, GL_SLOT_B);
    gl_color3f(s, 0, 1, 0);
    gl_polygon_open(s, 32);
    gl_polygon_check_owned(s, 2, 12);
    g_assert_cmphex(gl_window_pixel(s, 16, 8), ==, 0);
    g_assert_cmphex(gl_window_pixel(s, 48, 8), ==, 0);
}

static inline void gl_polygon_context_check(QTestState *s)
{
    gl_polygon_check_owned(s, 2, 12);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_END_OF(GL_BEGIN_POLYGON));
    gl_polygon_check_owned(s, 1, 6);
    g_assert_cmphex(gl_window_pixel(s, 16, 8), ==, 0xff0000);
    g_assert_cmphex(gl_window_pixel(s, 48, 8), ==, 0);
    gl_enter_slot(s, GL_SLOT_B);
    gl_word(s, GL_END_OF(GL_BEGIN_POLYGON));
    gl_polygon_check_owned(s, 0, 0);
    g_assert_cmphex(gl_window_pixel(s, 16, 8), ==, 0xff0000);
    g_assert_cmphex(gl_window_pixel(s, 48, 8), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(s, 16, 16), ==, 0);
    g_assert_cmphex(gl_window_pixel(s, 48, 16), ==, 0);
}

/*
 * GL_TEXTURE_MATRIX is queried at 0x6a + i, column-major like the other
 * matrices, and GL_TEXTURE_STACK_DEPTH at 0x923, returned as
 * ((answer - 0xb45) >> 4) + 1 with 0xb45 a hq3_revB_HQ4.u symbol-table
 * word. glMatrixMode(GL_TEXTURE) sends 0x2a04 with
 * 0x1702.
 */
#define GL_Q_TEXTURE          0x06a
#define GL_Q_TEXTURE_DEPTH    0x923

/*
 * The kernel's context eviction (sgi-mgras-gl.c, "Context eviction") as
 * a captured run captured it from i3dm: context 0x9d19's
 * 0x1854 ERAM words read out by a DMA read kicked 0xbd from DMA-map slot
 * 2, then that host image written back into 0x5420 in chunks of 0x384
 * words kicked 0xa5. Here node 0's page list maps pages 7 to 14.
 */
#define GL_EVICT_LIST         0x2000u
#define GL_EVICT_FROM         0x9d19
#define GL_EVICT_TO           0x5420
#define GL_EVICT_WORDS        0x1854
#define GL_EVICT_CHUNK        0x384
#define GL_ERAM_READ_HDR      0x0000fc08
#define GL_ERAM_COUNT_HDR     0x0000fd04
#define GL_ERAM_WRITE_HDR     0x0000fb08
#define GL_ERAM_END           0x0000fa00
#define GL_ERAM_READ_KICK     0xbd
#define GL_ERAM_WRITE_KICK    0xa5

/*
 * The page list's entries name node nasid's frames (entry bits 27:20),
 * from frame first.
 */
static inline void gl_eram_map_from(QTestState *qts, unsigned int nasid,
                                    uint32_t first)
{
    static const uint8_t header[24] = { 0 };
    uint8_t list[8 * 8] = { 0 };

    for (int i = 0; i < 8; i++) {
        stq_be_p(list + 8 * i, ((uint64_t)nasid << 20) | (first + i));
    }
    qtest_memwrite(qts, GL_EVICT_LIST, header, sizeof(header));
    qtest_memwrite(qts, GL_EVICT_LIST + 0x18, list, sizeof(list));
    /* DMA-map slot 2: registers 0x24/0x25, 0x32 and 0x4d. */
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482408);
    qtest_writeq(qts, MGRAS_REG(0x130400), GL_EVICT_LIST + 0x18);
    qtest_writeq(qts, MGRAS_REG(0x130400), ((uint64_t)0x00483204 << 32) | 8);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484d04 << 32) | (GL_PIX_DMA_WIDGET << 16));
}

static inline void gl_eram_map_node(QTestState *qts, unsigned int nasid)
{
    gl_eram_map_from(qts, nasid, 7);
}

static inline void gl_eram_map(QTestState *qts)
{
    gl_eram_map_node(qts, 0);
}

/* One row of bpr bytes at offset in slot 2's pages, kicked with kick. */
static inline void gl_eram_kick(QTestState *qts, uint32_t offset, uint32_t bpr,
                                uint32_t kick)
{
    gl_word(qts, 0x00080004);
    gl_word(qts, 0x80000000);
    gl_word(qts, 0x00080404);
    gl_word(qts, 0x20000);
    gl_word(qts, 0x00080604);
    gl_word(qts, offset & ~0xfffu);
    gl_word(qts, 0x00080704);
    gl_word(qts, 1);
    gl_word(qts, 0x00080504);
    gl_word(qts, offset & 0xfffu);
    gl_word(qts, 0x00080804);
    gl_word(qts, bpr);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    gl_word(qts, 0x00080b04);
    gl_word(qts, kick);
}

/*
 * The ERAM read routine's form: the kick, then 0xfc08 with address and count.
 */
static inline void gl_eram_read(QTestState *qts, uint32_t addr)
{
    gl_eram_kick(qts, 0, GL_EVICT_WORDS * 4, GL_ERAM_READ_KICK);
    gl_word(qts, GL_ERAM_READ_HDR);
    gl_word(qts, addr);
    gl_word(qts, GL_EVICT_WORDS);
}

/*
 * Context a, red and unscissored, and context b, green and scissored to
 * window columns and rows 0 to 7, both in one window, as i3dm's top view
 * and tool panel were.
 */
static inline void gl_eviction_contexts(QTestState *qts)
{
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_enter_slot(qts, GL_EVICT_FROM);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 1, 0, 0, 1);

    gl_enter_slot(qts, GL_EVICT_TO);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_scissor(qts, 0, 0, 8, 8);
    gl_one(qts, GL_SCISSOR_TEST_HDR, 1);
}

#endif /* SGI_MGRAS_TEST_H */
