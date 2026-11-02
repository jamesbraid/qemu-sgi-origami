/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Register-contract tests for the sgi-dm8 audio card (an ICE1724 controller,
 * with either the DM8's AKM converters or an AC'97 codec), placed on the
 * Fuel's XBridge head E, device 2. Expected values are spelled from
 * docs/datasheets/dm8-ice1724.md in the Origami repository, not taken from
 * the model's headers.
 *
 * Register access goes through the XBridge, as the processor does. The
 * head presents the little-endian I/O registers with a byte at offset XOR 3
 * and a halfword at offset XOR 2 (words are at their offset); the helpers
 * below apply that rule. DMA goes through the head's page-mapped window to
 * ordinary memory.
 *
 * GPIO data reads and the reset values in test_gpio_mask expect the
 * model's weak pull-up on input pins, which is the model's reading of "an
 * input bit reads the pin", not a datasheet value.
 *
 * What is not asserted, because the datasheet leaves it unknown: the MPU-401
 * UART, the peak meters, the output routing, the effect of power-down and
 * the sample data the converters would play.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "sgi-test.h"
#include "sgi-migrate.h"

#define WIDGET_E        (SPACE_IO + 14 * UINT64_C(0x1000000))
#define WIDGET_F        (SPACE_IO + 15 * UINT64_C(0x1000000))
#define CONFIG(w, d)    ((w) + 0x20000 + (d) * 0x1000)
#define CARD            2
#define CARD_REG        (WIDGET_E + 0x200 + CARD * 8 + 4)
#define PIO             (WIDGET_E + 0x600000)
#define ATE0            (WIDGET_E + 0x10000)
#define HOST_PORT       0xa
#define ATE_VALID       1
#define BEDROCK_MD_CONFIG (SPACE_IO + UINT64_C(0x01000000) + 0x780008)

/* The card's I/O windows in PCI I/O space, in the head's first megabyte. */
#define CCS_BAR         0x100000
#define MT_BAR          0x100080
#define MT              0x80

#define RAM_BUFFER      0x200000
#define DMA_BUFFER      0x40000000

#define GPIO_CCLK       0x02
#define GPIO_CDOUT      0x08
#define GPIO_CS_FRONT   0x20
#define GPIO_CS_SURROUND 0x40
#define GPIO_IDLE       0x72

#define PATH            "/machine/peripheral/dm8"

static char *prom_path;

static const char *machine_args(void)
{
    return "-M fuel,board-id-word=0x4abc,ioc3-subsystem-id=0xc30a,"
           "l1-type-code=0x41,bedrock-revision=0x2 "
           "-cpu R14000 -smp 1 -m 512M -S";
}

static QTestState *start(const char *variant, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    g_autofree char *device = g_strdup_printf(
        "-audiodev none,id=a -device sgi-dm8,id=dm8,bus=head-e.0,addr=2,"
        "audiodev=a%s%s", variant ? ",variant=" : "", variant ? variant : "");

    return qtest_initf("%s -bios %s %s %s", machine_args(), quoted, device,
                       extra);
}

/* ---- accessors ---------------------------------------------------------- */

static uint32_t cfg_l(QTestState *qts, unsigned off)
{
    return qtest_readl(qts, CONFIG(WIDGET_E, CARD) + off);
}

static void cfg_wl(QTestState *qts, unsigned off, uint32_t v)
{
    qtest_writel(qts, CONFIG(WIDGET_E, CARD) + off, v);
}

static uint8_t cfg_b(QTestState *qts, unsigned off)
{
    return qtest_readb(qts, CONFIG(WIDGET_E, CARD) + (off ^ 3));
}

static void cfg_wb(QTestState *qts, unsigned off, uint8_t v)
{
    qtest_writeb(qts, CONFIG(WIDGET_E, CARD) + (off ^ 3), v);
}

static uint8_t rb(QTestState *qts, unsigned off)
{
    return qtest_readb(qts, PIO + (off ^ 3));
}

static uint16_t rw(QTestState *qts, unsigned off)
{
    return qtest_readw(qts, PIO + (off ^ 2));
}

static uint32_t rl(QTestState *qts, unsigned off)
{
    return qtest_readl(qts, PIO + off);
}

static void wb(QTestState *qts, unsigned off, uint8_t v)
{
    qtest_writeb(qts, PIO + (off ^ 3), v);
}

static void ww(QTestState *qts, unsigned off, uint16_t v)
{
    qtest_writew(qts, PIO + (off ^ 2), v);
}

static void wl(QTestState *qts, unsigned off, uint32_t v)
{
    qtest_writel(qts, PIO + off, v);
}

static uint64_t prop(QTestState *qts, const char *name)
{
    QDict *r = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':%s}}", PATH, name);
    QNum *n;
    uint64_t v;

    g_assert_false(qdict_haskey(r, "error"));
    n = qobject_to(QNum, qdict_get(r, "return"));
    g_assert_true(qnum_get_try_uint(n, &v));
    qobject_unref(r);
    return v;
}

/* Enable I/O decoding and bus mastering, and map both windows. */
static void map_card(QTestState *qts)
{
    qtest_writeq(qts, BEDROCK_MD_CONFIG, 0x33);
    cfg_wl(qts, PCI_BAR0, CCS_BAR | 1);
    cfg_wl(qts, PCI_BAR1, MT_BAR | 1);
    cfg_wl(qts, PCI_COMMAND, 5);
    /* An I/O-space device in the head's megabyte 1. */
    qtest_writel(qts, CARD_REG, CCS_BAR >> 20);
}

static void map_dma_page(QTestState *qts)
{
    qtest_writeq(qts, ATE0, RAM_BUFFER | ((uint64_t)HOST_PORT << 8) |
                 ATE_VALID);
}

/* ---- EEPROM and converter helpers --------------------------------------- */

static uint8_t eeprom_byte(QTestState *qts, unsigned index)
{
    wb(qts, 0x11, index);
    wb(qts, 0x10, 0xa0);
    g_assert_cmphex(rb(qts, 0x13) & 1, ==, 0);
    return rb(qts, 0x12);
}

/* The sequence the IRIX driver runs: direction, mask, then data. */
static void load_gpio_from_eeprom(QTestState *qts)
{
    uint32_t dir = eeprom_byte(qts, 10) | eeprom_byte(qts, 11) << 8 |
                   eeprom_byte(qts, 12) << 16;
    uint32_t mask = eeprom_byte(qts, 13) | eeprom_byte(qts, 14) << 8 |
                    eeprom_byte(qts, 15) << 16;
    uint32_t data = eeprom_byte(qts, 16) | eeprom_byte(qts, 17) << 8 |
                    eeprom_byte(qts, 18) << 16;

    ww(qts, 0x18, dir);
    wb(qts, 0x1a, dir >> 16);
    ww(qts, 0x16, mask);
    wb(qts, 0x1f, mask >> 16);
    ww(qts, 0x14, data);
    wb(qts, 0x1e, data >> 16);
}

static void pins(QTestState *qts, uint8_t value)
{
    wb(qts, 0x14, value);
}

/* One 16-bit frame as the drivers send it, most significant bit first. */
static void codec_frame_bits(QTestState *qts, uint8_t select, uint16_t word,
                             unsigned nbits)
{
    uint8_t low = GPIO_IDLE & ~select;

    pins(qts, low);
    for (int i = nbits - 1; i >= 0; i--) {
        uint8_t data = (word >> i & 1) ? GPIO_CDOUT : 0;

        pins(qts, low & ~GPIO_CCLK);
        pins(qts, (low & ~GPIO_CCLK) | data);
        pins(qts, low | data);
    }
    pins(qts, GPIO_IDLE);
}

static void codec_write(QTestState *qts, uint8_t select, unsigned chip,
                        unsigned reg, uint8_t value)
{
    codec_frame_bits(qts, select, (chip << 14 | 0x2000 | reg << 8 | value),
                     16);
}

/* ---- identity and reset ------------------------------------------------- */

static void test_config_identity(void)
{
    QTestState *qts = start(NULL, "");

    g_assert_cmphex(cfg_l(qts, 0x00), ==, 0x17241412);
    /* Revision 1 as the DM8 reports it, class 0401, programming interface 0 */
    g_assert_cmphex(cfg_l(qts, 0x08), ==, 0x04010001);
    g_assert_cmphex(cfg_l(qts, 0x2c), ==, 0x36301412);
    /* Medium DEVSEL, capability list present; no interrupt, no errors. */
    g_assert_cmphex(cfg_l(qts, 0x04) >> 16, ==, 0x0210);
    g_assert_cmphex(cfg_l(qts, 0x04) & 0xffff, ==, 0);
    g_assert_cmphex(cfg_b(qts, 0x0c), ==, 0);
    g_assert_cmphex(cfg_b(qts, 0x0e), ==, 0);
    g_assert_cmphex(cfg_l(qts, 0x10), ==, 1);
    g_assert_cmphex(cfg_l(qts, 0x14), ==, 1);
    g_assert_cmphex(cfg_l(qts, 0x18), ==, 0);
    g_assert_cmphex(cfg_l(qts, 0x1c), ==, 0);
    g_assert_cmphex(cfg_l(qts, 0x30), ==, 0);
    g_assert_cmphex(cfg_b(qts, 0x34), ==, 0x80);
    g_assert_cmphex(cfg_b(qts, 0x3d), ==, 1);
    g_assert_cmphex(cfg_b(qts, 0x3c), ==, 0xff);
    /* Power management capability: version 1.0, D2 only, no PME. */
    g_assert_cmphex(cfg_l(qts, 0x80), ==, 0x04010001);
    g_assert_cmphex(cfg_l(qts, 0x84), ==, 0);
    qtest_quit(qts);
}

static void test_config_writes(void)
{
    QTestState *qts = start(NULL, "");

    /* BAR sizes: 32 and 128 bytes of I/O space. */
    cfg_wl(qts, PCI_BAR0, 0xffffffff);
    cfg_wl(qts, PCI_BAR1, 0xffffffff);
    g_assert_cmphex(cfg_l(qts, 0x10), ==, 0xffffffe1);
    g_assert_cmphex(cfg_l(qts, 0x14), ==, 0xffffff81);
    cfg_wl(qts, PCI_BAR0, 0);
    cfg_wl(qts, PCI_BAR1, 0);

    /* Only I/O decode and bus mastering are writable in the command. */
    cfg_wl(qts, PCI_COMMAND, 0xffff);
    g_assert_cmphex(cfg_l(qts, 0x04) & 0xffff, ==, 0x0005);
    cfg_wl(qts, PCI_COMMAND, 0);

    /* The latency timer keeps bits 7:3. */
    cfg_wb(qts, 0x0d, 0xff);
    g_assert_cmphex(cfg_b(qts, 0x0d), ==, 0xf8);
    cfg_wb(qts, 0x0c, 0xff);
    g_assert_cmphex(cfg_b(qts, 0x0c), ==, 0);
    cfg_wb(qts, 0x3c, 0x05);
    g_assert_cmphex(cfg_b(qts, 0x3c), ==, 0x05);

    /* The subsystem pair is read only until PCI42 bit 7 is set. */
    cfg_wl(qts, 0x2c, 0x12345678);
    g_assert_cmphex(cfg_l(qts, 0x2c), ==, 0x36301412);
    cfg_wb(qts, 0x42, 0x80);
    cfg_wl(qts, 0x2c, 0x12345678);
    g_assert_cmphex(cfg_l(qts, 0x2c), ==, 0x12345678);
    cfg_wb(qts, 0x42, 0);

    /* Nothing else past the header is writable. */
    cfg_wl(qts, 0x88, 0xffffffff);
    g_assert_cmphex(cfg_l(qts, 0x88), ==, 0);
    qtest_quit(qts);
}

static void test_power_state(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    wb(qts, 0x04, 0x4f);
    g_assert_cmphex(rb(qts, 0x04), ==, 0x4f);

    /* D1 is not supported: the write leaves D0. */
    cfg_wl(qts, 0x84, 1);
    g_assert_cmphex(cfg_l(qts, 0x84) & 3, ==, 0);
    g_assert_cmphex(rb(qts, 0x04), ==, 0x4f);
    cfg_wl(qts, 0x84, 3);
    g_assert_cmphex(cfg_l(qts, 0x84) & 3, ==, 3);
    /* D3hot to D0 resets the chip including configuration space. */
    cfg_wl(qts, 0x84, 0);
    g_assert_cmphex(cfg_l(qts, 0x84) & 3, ==, 0);
    g_assert_cmphex(cfg_l(qts, 0x04) & 0xffff, ==, 0);
    g_assert_cmphex(cfg_l(qts, 0x10), ==, 1);
    g_assert_cmphex(cfg_l(qts, 0x2c), ==, 0x36301412);
    qtest_quit(qts);
}

static void test_reset_values(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    /* Dword views of the byte registers, little-endian. */
    g_assert_cmphex(rl(qts, 0x00), ==, 0x0000fe00);      /* 00 01 02 03 */
    g_assert_cmphex(rl(qts, 0x04), ==, 0x0101000f);      /* 04 05 06 07 */
    g_assert_cmphex(rl(qts, 0x08), ==, 0);
    g_assert_cmphex(rl(qts, 0x0c), ==, 0);
    /* I2C: the EEPROM strap reads back in CCS13 bit 7. */
    g_assert_cmphex(rl(qts, 0x10), ==, 0x80000000);
    /* GPIO data is the pull-up on every pin, mask all ones, direction 0. */
    g_assert_cmphex(rw(qts, 0x16), ==, 0xffff);
    g_assert_cmphex(rb(qts, 0x1f), ==, 0xff);
    g_assert_cmphex(rw(qts, 0x18), ==, 0);
    g_assert_cmphex(rb(qts, 0x1c), ==, 0);
    /* Multi-channel block. */
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x01), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x02), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x03), ==, 0xff);
    g_assert_cmphex(rb(qts, MT + 0x18), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x19), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x1a), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x1b), ==, 0);
    g_assert_cmphex(rl(qts, MT + 0x10), ==, 0);
    g_assert_cmphex(rl(qts, MT + 0x2c), ==, 0);

    /* Write-masked bits: MT01 bits 7:5 read 0, CCS07 bit 6 is read only. */
    wb(qts, MT + 0x01, 0xff);
    g_assert_cmphex(rb(qts, MT + 0x01), ==, 0x1f);
    wb(qts, 0x07, 0xff);
    g_assert_cmphex(rb(qts, 0x07), ==, 0xbf);
    wb(qts, 0x04, 0x43);
    wb(qts, 0x01, 0x00);
    ww(qts, 0x16, 0);

    /* The soft reset keeps CCS04 to CCS07 and restores the rest. */
    wb(qts, 0x00, 0x80);
    g_assert_cmphex(rb(qts, 0x04), ==, 0x43);
    g_assert_cmphex(rb(qts, 0x07), ==, 0xbf);
    g_assert_cmphex(rb(qts, 0x01), ==, 0xfe);
    g_assert_cmphex(rw(qts, 0x16), ==, 0xffff);
    g_assert_cmphex(rb(qts, MT + 0x01), ==, 0);

    /* A system reset restores everything, configuration space too. */
    qtest_system_reset(qts);
    g_assert_cmphex(cfg_l(qts, 0x10), ==, 1);
    g_assert_cmphex(cfg_l(qts, 0x04) & 0xffff, ==, 0);
    map_card(qts);
    g_assert_cmphex(rl(qts, 0x04), ==, 0x0101000f);
    g_assert_cmphex(rb(qts, 0x01), ==, 0xfe);
    qtest_quit(qts);
}

static void test_system_reset(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    map_dma_page(qts);
    wb(qts, 0x04, 0x43);
    wb(qts, MT + 0x01, 0x08);
    wb(qts, MT + 0x19, 3);
    wb(qts, MT + 0x03, 0xfe);
    wb(qts, MT + 0x18, 0x01);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(prop(qts, "analog-frames"), ==, 44);

    qtest_system_reset(qts);
    map_card(qts);
    g_assert_cmphex(rb(qts, 0x04), ==, 0x0f);
    g_assert_cmphex(rb(qts, MT + 0x01), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x03), ==, 0xff);
    g_assert_cmphex(rb(qts, MT + 0x18), ==, 0);
    g_assert_cmphex(cfg_b(qts, 0x3c), ==, 0xff);
    /* A stopped engine moves nothing. */
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(prop(qts, "analog-frames"), ==, 44);
    qtest_quit(qts);
}

/* ---- I2C EEPROM --------------------------------------------------------- */

static void test_eeprom(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    g_assert_cmphex(rb(qts, 0x13) & 0x80, ==, 0x80);
    /* The subsystem pair, stored the way the public tables read it. */
    g_assert_cmphex(eeprom_byte(qts, 0), ==, 0x36);
    g_assert_cmphex(eeprom_byte(qts, 1), ==, 0x30);
    g_assert_cmphex(eeprom_byte(qts, 2), ==, 0x14);
    g_assert_cmphex(eeprom_byte(qts, 3), ==, 0x12);
    /* The configuration bytes give an I2S board with four stereo DACs. */
    g_assert_cmphex(eeprom_byte(qts, 6) & 3, ==, 3);
    g_assert_cmphex(eeprom_byte(qts, 7) & 0x80, ==, 0x80);
    /* Past the image the part is erased. */
    g_assert_cmphex(eeprom_byte(qts, 100), ==, 0xff);

    /* A write cycle sticks. */
    wb(qts, 0x11, 100);
    wb(qts, 0x12, 0x5a);
    wb(qts, 0x10, 0xa1);
    g_assert_cmphex(eeprom_byte(qts, 100), ==, 0x5a);

    /* Bit 7 and bit 0 of CCS13 are read only; bit 1 is storage. */
    wb(qts, 0x13, 0x7f);
    g_assert_cmphex(rb(qts, 0x13), ==, 0x82);

    /* A transfer to a device nothing answers leaves the data alone. */
    wb(qts, 0x12, 0x77);
    wb(qts, 0x10, 0xa4);
    g_assert_cmphex(rb(qts, 0x12), ==, 0x77);
    qtest_quit(qts);
}

/* ---- the converters' write protocol ------------------------------------- */

static void test_codec_writes(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    load_gpio_from_eeprom(qts);
    g_assert_cmphex(prop(qts, "ak4381-reg3"), ==, 0xff);
    g_assert_cmphex(prop(qts, "ak4355-reg9"), ==, 0xff);
    g_assert_cmphex(prop(qts, "ak4381-reg0"), ==, 0);

    /* Front DAC, chip address 1, select on GPIO 5. */
    codec_write(qts, GPIO_CS_FRONT, 1, 0, 0x0c);
    g_assert_cmphex(prop(qts, "ak4381-reg0"), ==, 0x0c);
    codec_write(qts, GPIO_CS_FRONT, 1, 0, 0x0f);
    codec_write(qts, GPIO_CS_FRONT, 1, 3, 0x80);
    codec_write(qts, GPIO_CS_FRONT, 1, 4, 0x40);
    g_assert_cmphex(prop(qts, "ak4381-reg0"), ==, 0x0f);
    g_assert_cmphex(prop(qts, "ak4381-reg3"), ==, 0x80);
    g_assert_cmphex(prop(qts, "ak4381-reg4"), ==, 0x40);
    g_assert_cmpuint(prop(qts, "ak4381-writes"), ==, 4);

    /* Surround DAC, chip address 3, select on GPIO 6, eleven registers. */
    codec_write(qts, GPIO_CS_SURROUND, 3, 1, 0x01);
    codec_write(qts, GPIO_CS_SURROUND, 3, 2, 0x2e);
    codec_write(qts, GPIO_CS_SURROUND, 3, 10, 0x55);
    g_assert_cmphex(prop(qts, "ak4355-reg1"), ==, 0x01);
    g_assert_cmphex(prop(qts, "ak4355-reg2"), ==, 0x2e);
    g_assert_cmphex(prop(qts, "ak4355-reg10"), ==, 0x55);
    g_assert_cmpuint(prop(qts, "ak4355-writes"), ==, 3);
    /* The two ports do not see each other's frames. */
    g_assert_cmphex(prop(qts, "ak4381-reg1"), ==, 0);
    g_assert_cmpuint(prop(qts, "codec-rejects"), ==, 0);

    /* Frames the parts would not take are counted and dropped. */
    codec_write(qts, GPIO_CS_FRONT, 3, 0, 0x99);         /* wrong chip */
    codec_write(qts, GPIO_CS_SURROUND, 1, 1, 0x99);      /* wrong chip */
    codec_write(qts, GPIO_CS_FRONT, 1, 5, 0x99);         /* no register 5 */
    codec_write(qts, GPIO_CS_SURROUND, 3, 11, 0x99);     /* no register 11 */
    codec_frame_bits(qts, GPIO_CS_FRONT, 0x600f, 15);    /* short frame */
    codec_frame_bits(qts, GPIO_CS_FRONT, 0x600f, 17);    /* long frame */
    codec_frame_bits(qts, GPIO_CS_FRONT, 0x400f, 16);    /* read command */
    g_assert_cmpuint(prop(qts, "codec-rejects"), ==, 7);
    g_assert_cmpuint(prop(qts, "ak4381-writes"), ==, 4);
    g_assert_cmpuint(prop(qts, "ak4355-writes"), ==, 3);
    g_assert_cmphex(prop(qts, "ak4381-reg0"), ==, 0x0f);

    /* Data changes while CCLK stays high do not clock a frame. */
    pins(qts, GPIO_IDLE & ~GPIO_CS_FRONT);
    for (int i = 0; i < 16; i++) {
        pins(qts, (GPIO_IDLE & ~GPIO_CS_FRONT) | GPIO_CDOUT);
        pins(qts, GPIO_IDLE & ~GPIO_CS_FRONT);
    }
    pins(qts, GPIO_IDLE);
    g_assert_cmpuint(prop(qts, "codec-rejects"), ==, 7);
    g_assert_cmpuint(prop(qts, "ak4381-writes"), ==, 4);
    g_assert_cmphex(prop(qts, "ak4381-reg0"), ==, 0x0f);

    /* A reset powers the converters back to their defaults. */
    qtest_system_reset(qts);
    g_assert_cmphex(prop(qts, "ak4381-reg0"), ==, 0);
    g_assert_cmphex(prop(qts, "ak4381-reg3"), ==, 0xff);
    qtest_quit(qts);
}

static void test_gpio_mask(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    /* Masked at reset: a data write changes nothing but the pull-ups. */
    ww(qts, 0x18, 0x00ff);
    ww(qts, 0x14, 0x0000);
    g_assert_cmphex(rw(qts, 0x14), ==, 0xff00);
    ww(qts, 0x16, 0xfff0);              /* open the low nibble */
    ww(qts, 0x14, 0x0005);
    g_assert_cmphex(rw(qts, 0x14), ==, 0xff05);
    /* An unmasked output reads back what was written. */
    ww(qts, 0x14, 0x000a);
    g_assert_cmphex(rw(qts, 0x14), ==, 0xff0a);
    /* The upper block has its own mask and direction. */
    wb(qts, 0x1a, 0x40);
    wb(qts, 0x1f, 0xbf);
    wb(qts, 0x1e, 0x00);
    g_assert_cmphex(rb(qts, 0x1e), ==, 0x3f);
    wb(qts, 0x1e, 0x40);
    g_assert_cmphex(rb(qts, 0x1e), ==, 0x7f);
    g_assert_cmphex(rb(qts, 0x1a), ==, 0x40);
    /*
     * A masked pin that is an output keeps its value: bits 16 and 17 are
     * outputs here, protected by the mask, so a write to them does not stick.
     * (The pull-up on input pins is the model's reading of "reads the pin".)
     */
    wb(qts, 0x1a, 0x43);
    wb(qts, 0x1e, 0x43);
    g_assert_cmphex(rb(qts, 0x1e), ==, 0x7c);
    /* Bit 23 of the direction register is reserved. */
    wb(qts, 0x1a, 0xff);
    g_assert_cmphex(rb(qts, 0x1a), ==, 0x7f);
    qtest_quit(qts);
}

static void test_spdif_control(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    /* MT3C is writable while the transmitter is disabled. */
    ww(qts, MT + 0x3c, 0x1234);
    g_assert_cmphex(rw(qts, MT + 0x3c), ==, 0x1234);
    /* Enabled: only the validity flag, bit 15, takes a write. */
    wb(qts, 0x07, 0x81);
    ww(qts, MT + 0x3c, 0xffff);
    g_assert_cmphex(rw(qts, MT + 0x3c), ==, 0x9234);
    ww(qts, MT + 0x3c, 0x0000);
    g_assert_cmphex(rw(qts, MT + 0x3c), ==, 0x1234);
    /* Disabled again: everything writable. */
    wb(qts, 0x07, 0x01);
    ww(qts, MT + 0x3c, 0x0000);
    g_assert_cmphex(rw(qts, MT + 0x3c), ==, 0);
    qtest_quit(qts);
}

/* ---- DMA playback, interrupts, record ----------------------------------- */

static void fill_playback_buffer(QTestState *qts)
{
    uint8_t buf[4096];

    /*
     * The big-endian host writes numeric samples in memory order; the head
     * presents each word to the card as its little-endian value.
     */
    for (unsigned i = 0; i < sizeof(buf) / 4; i++) {
        stl_be_p(buf + i * 4, 0x01000000 + i);
    }
    qtest_memwrite(qts, RAM_BUFFER, buf, sizeof(buf));
}

/* A stereo stream of 960 dwords with an interrupt every 480. */
static void program_playback(QTestState *qts)
{
    wb(qts, MT + 0x19, 3);
    wl(qts, MT + 0x10, DMA_BUFFER);
    ww(qts, MT + 0x14, 959);
    ww(qts, MT + 0x1c, 479);
}

static bool inta(QTestState *qts)
{
    return cfg_l(qts, 0x04) >> 16 & 0x08;
}

static void test_playback_interrupt(void)
{
    QTestState *qts = start(NULL, "");
    uint64_t first_run;

    map_card(qts);
    map_dma_page(qts);
    fill_playback_buffer(qts);
    program_playback(qts);

    /* Registers read back as programmed. */
    g_assert_cmphex(rl(qts, MT + 0x10), ==, DMA_BUFFER);
    g_assert_cmphex(rw(qts, MT + 0x14), ==, 959);

    /* Reset masks everything: a run raises nothing. */
    wb(qts, MT + 0x18, 0x01);
    qtest_clock_step(qts, 5000000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0);
    g_assert_false(inta(qts));
    wb(qts, MT + 0x18, 0x00);

    /* Unmask the interleaved stream and the multi-channel source. */
    first_run = prop(qts, "analog-frames");
    g_assert_cmpuint(first_run, ==, 240);
    program_playback(qts);
    wb(qts, MT + 0x03, 0xfe);
    wb(qts, 0x01, 0xef);
    wb(qts, MT + 0x18, 0x01);

    /* 235 frames have gone by at 4.9 ms: 470 dwords, no interrupt yet. */
    qtest_clock_step(qts, 4900000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0);
    g_assert_false(inta(qts));
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 470);
    g_assert_cmphex(rl(qts, MT + 0x10), ==, DMA_BUFFER + 470 * 4);

    /* At 5 ms the 480th dword has moved. */
    qtest_clock_step(qts, 100000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0x01);
    g_assert_cmphex(rb(qts, 0x02) & 0x10, ==, 0x10);
    g_assert_true(inta(qts));
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 480);
    g_assert_cmphex(rl(qts, MT + 0x10), ==, DMA_BUFFER + 480 * 4);
    g_assert_cmpuint(prop(qts, "analog-frames"), ==, first_run + 240);
    g_assert_cmphex(prop(qts, "analog-last-sample"), ==,
                    0x01000000 + 479);
    g_assert_cmpuint(prop(qts, "dropped-frames"), ==, 0);

    /* Write 1 to clear; the macro bit and the line follow. */
    wb(qts, MT + 0x00, 0x01);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0);
    g_assert_cmphex(rb(qts, 0x02) & 0x10, ==, 0);
    g_assert_false(inta(qts));

    /* The buffer wraps and the interval repeats. */
    qtest_clock_step(qts, 5000000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0x01);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959);
    g_assert_cmphex(rl(qts, MT + 0x10), ==, DMA_BUFFER);
    g_assert_cmphex(prop(qts, "analog-last-sample"), ==,
                    0x01000000 + 959);
    /* A masked source stops reaching the line, and does not latch. */
    wb(qts, MT + 0x00, 0x01);
    wb(qts, MT + 0x03, 0xff);
    qtest_clock_step(qts, 5000000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0);
    g_assert_false(inta(qts));
    /* The mask in CCS01 gates the line, not MT00. */
    wb(qts, MT + 0x03, 0xfe);
    qtest_clock_step(qts, 5000000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 1);
    g_assert_true(inta(qts));
    wb(qts, 0x01, 0xfe);
    g_assert_false(inta(qts));
    wb(qts, 0x01, 0xef);
    g_assert_true(inta(qts));
    qtest_quit(qts);
}

static void test_playback_pause_and_rate(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    map_dma_page(qts);
    fill_playback_buffer(qts);
    program_playback(qts);

    /* 44.1 kHz: 220 frames in 5 ms, 440 dwords of the 960. */
    wb(qts, MT + 0x01, 0x08);
    wb(qts, MT + 0x18, 0x01);
    qtest_clock_step(qts, 5000000);
    g_assert_cmpuint(prop(qts, "analog-frames"), ==, 220);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 440);

    /* Pause freezes the position; resume continues from it. */
    wb(qts, MT + 0x1b, 0x01);
    qtest_clock_step(qts, 5000000);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 440);
    wb(qts, MT + 0x1b, 0x00);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 440 - 88);

    /* Stop: the position stays. */
    wb(qts, MT + 0x18, 0x00);
    qtest_clock_step(qts, 5000000);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 440 - 88);

    /* The clock gate in CCS1C stops the engine too. */
    wb(qts, MT + 0x18, 0x01);
    wb(qts, 0x1c, 0x01);
    qtest_clock_step(qts, 5000000);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 440 - 88);
    wb(qts, 0x1c, 0x00);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 440 - 88 - 88);
    qtest_quit(qts);
}

static void test_eight_channel_frames(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    map_dma_page(qts);
    fill_playback_buffer(qts);
    /* MT19 = 0: eight slots per frame, a 960-dword buffer holds 120. */
    wl(qts, MT + 0x10, DMA_BUFFER);
    ww(qts, MT + 0x14, 959);
    wb(qts, MT + 0x18, 0x01);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(prop(qts, "analog-frames"), ==, 48);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 48 * 8);
    /* Six slots. */
    wb(qts, MT + 0x18, 0);
    wb(qts, MT + 0x19, 1);
    wl(qts, MT + 0x10, DMA_BUFFER);
    wb(qts, MT + 0x18, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(rw(qts, MT + 0x14), ==, 959 - 48 * 6);
    qtest_quit(qts);
}

static void test_playback_fault(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    /* No page entry: the transfer fails. */
    program_playback(qts);
    wb(qts, MT + 0x18, 0x01);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(rb(qts, MT + 0x1a), ==, 0x01);
    /* A master abort in the PCI status, and the engine keeps going. */
    g_assert_cmphex(cfg_l(qts, 0x04) >> 16 & 0x2000, ==, 0x2000);
    g_assert_cmpuint(rw(qts, MT + 0x14), <, 959);
    /* MT03 masks the FIFO-error source at reset: nothing in MT00. */
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0);

    /* Write 1 to clear both. */
    wb(qts, MT + 0x1a, 0x01);
    g_assert_cmphex(rb(qts, MT + 0x1a), ==, 0);
    cfg_wl(qts, PCI_COMMAND, 0x20000000 | 5);
    g_assert_cmphex(cfg_l(qts, 0x04) >> 16 & 0x2000, ==, 0);

    /* Unmasked, the error reaches MT00 bit 3 and the line. */
    wb(qts, MT + 0x03, 0xf7);
    wb(qts, 0x01, 0xef);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0x08);
    g_assert_true(inta(qts));
    qtest_quit(qts);
}

static void test_record(void)
{
    QTestState *qts = start(NULL, "");
    uint8_t buf[512], expect[512];

    map_card(qts);
    map_dma_page(qts);
    memset(buf, 0xaa, sizeof(buf));
    qtest_memwrite(qts, RAM_BUFFER, buf, sizeof(buf));

    wl(qts, MT + 0x20, DMA_BUFFER);
    ww(qts, MT + 0x24, 63);
    ww(qts, MT + 0x26, 31);
    wb(qts, MT + 0x03, 0xfd);
    wb(qts, MT + 0x18, 0x02);
    qtest_clock_step(qts, 500000);
    /* 24 frames: 48 dwords, past the 32-dword interval, short of the end. */
    g_assert_cmphex(rb(qts, MT + 0x00), ==, 0x02);
    g_assert_cmpuint(rw(qts, MT + 0x24), ==, 63 - 48);
    g_assert_cmpuint(prop(qts, "capture-frames"), ==, 24);
    memset(expect, 0xaa, sizeof(expect));
    memset(expect, 0, 48 * 4);
    qtest_memread(qts, RAM_BUFFER, buf, sizeof(buf));
    g_assert_cmpmem(buf, sizeof(buf), expect, sizeof(expect));

    /* The second record engine runs independently. */
    wl(qts, MT + 0x30, DMA_BUFFER + 0x400);
    ww(qts, MT + 0x34, 63);
    wb(qts, MT + 0x18, 0x06);
    qtest_clock_step(qts, 250000);
    g_assert_cmpuint(rw(qts, MT + 0x34), ==, 63 - 24);
    qtest_quit(qts);
}

static void test_spdif_and_stereo_engines(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    map_dma_page(qts);
    fill_playback_buffer(qts);
    wl(qts, MT + 0x40, DMA_BUFFER);
    ww(qts, MT + 0x44, 255);
    wb(qts, MT + 0x18, 0x80);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(prop(qts, "spdif-frames"), ==, 48);
    g_assert_cmpuint(rw(qts, MT + 0x44), ==, 255 - 96);
    g_assert_cmphex(rl(qts, MT + 0x40), ==, DMA_BUFFER + 96 * 4);

    /* PDMA3 needs MT19 above 0; with 0 it does not run. */
    wl(qts, MT + 0x50, DMA_BUFFER);
    ww(qts, MT + 0x54, 255);
    wb(qts, MT + 0x18, 0xc0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(rw(qts, MT + 0x54), ==, 255);
    wb(qts, MT + 0x19, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(rw(qts, MT + 0x54), ==, 255 - 96);
    qtest_quit(qts);
}

/* ---- unknown and unexpected accesses ------------------------------------ */

static void test_unknown_registers_logged(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-dm8-XXXXXX", &error);
    g_autofree char *log = NULL;
    g_autofree char *text = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;

    g_assert_no_error(error);
    log = g_build_filename(dir, "qemu.log", NULL);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", log);
    qts = start(NULL, extra);
    map_card(qts);

    /* Unassigned offsets read 0, ignore writes, and are logged. */
    g_assert_cmphex(rb(qts, MT + 0x28), ==, 0);
    wb(qts, MT + 0x28, 0xff);
    g_assert_cmphex(rb(qts, MT + 0x28), ==, 0);
    g_assert_cmphex(rb(qts, 0x08), ==, 0);
    wb(qts, 0x1d, 0xff);
    /* Write-only terminal counts, the peak meters and MPU-401 UART. */
    g_assert_cmphex(rw(qts, MT + 0x1c), ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x3f), ==, 0);
    wb(qts, 0x0c, 0x55);
    /* Rates and formats the datasheet does not define. */
    wb(qts, MT + 0x01, 0x0d);
    wb(qts, MT + 0x02, 0x01);
    /* Writes to read-only registers. */
    wb(qts, 0x0a, 0x01);
    wb(qts, MT + 0x3f, 0x01);
    /* Configuration registers the datasheet does not describe. */
    g_assert_cmphex(cfg_l(qts, 0x18), ==, 0);
    g_assert_cmphex(cfg_l(qts, 0x60), ==, 0);
    cfg_wl(qts, 0x40, 0xffffffff);
    /* Routing is stored but not modelled. */
    wl(qts, MT + 0x2c, 0x00000002);
    g_assert_cmphex(rl(qts, MT + 0x2c), ==, 2);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "read of unassigned MT 0x28"));
    g_assert_nonnull(strstr(text, "write of 0xff to unassigned MT 0x28"));
    g_assert_nonnull(strstr(text, "read of unassigned CCS 0x08"));
    g_assert_nonnull(strstr(text, "write of 0xff to unassigned CCS 0x1d"));
    g_assert_nonnull(strstr(text, "read of write-only terminal count 0x1c"));
    g_assert_nonnull(strstr(text, "peak meters are not modelled"));
    g_assert_nonnull(strstr(text, "MPU-401 UART write at CCS 0x0c"));
    g_assert_nonnull(strstr(text, "reserved sample rate code 13"));
    g_assert_nonnull(strstr(text, "reserved data format 1"));
    g_assert_nonnull(strstr(text, "write to read-only CCS 0x0a"));
    g_assert_nonnull(strstr(text, "write to read-only peak data"));
    g_assert_nonnull(strstr(text, "output routing is not modelled"));
    g_assert_nonnull(strstr(text, "config read of unknown register(s) at "
                                  "0x18"));
    g_assert_nonnull(strstr(text, "config read of unknown register(s) at "
                                  "0x60"));
    g_assert_nonnull(strstr(text, "config write of unknown register(s) at "
                                  "0x40"));
    g_assert_cmpint(g_unlink(log), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_bad_variant(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "fuel,board-id-word=0x4abc,ioc3-subsystem-id=0xc30a,"
        "l1-type-code=0x41,bedrock-revision=0x2",
        "-cpu", "R14000", "-smp", "1", "-m", "512M", "-bios", prom_path,
        "-S", "-audiodev", "none,id=a", "-device",
        "sgi-dm8,bus=head-e.0,addr=2,audiodev=a,variant=bogus",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, "variant must be dm8 or ac97"));
}

/* ---- placement ---------------------------------------------------------- */

static void test_placement(void)
{
    QTestState *qts = start(NULL, "");
    g_autoptr(QDict) r = NULL;

    /* On head E, device 2: the first free slot, and only that one. */
    g_assert_cmphex(qtest_readl(qts, CONFIG(WIDGET_E, 2)), ==, 0x17241412);
    g_assert_cmphex(qtest_readl(qts, CONFIG(WIDGET_E, 1)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, CONFIG(WIDGET_E, 3)), ==, 0xffffffff);
    /* Head F keeps the board's own functions and gains nothing. */
    g_assert_cmphex(qtest_readl(qts, CONFIG(WIDGET_F, 1)), ==, 0x12161077);
    g_assert_cmphex(qtest_readl(qts, CONFIG(WIDGET_F, 2)), ==, 0xffffffff);

    r = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                  "{'path':%s,'property':'type'}}", PATH);
    g_assert_false(qdict_haskey(r, "error"));
    qtest_quit(qts);
}

static void test_placement_default_empty(void)
{
    g_autofree char *quoted = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf("%s -bios %s", machine_args(), quoted);

    g_assert_cmphex(qtest_readl(qts, CONFIG(WIDGET_E, CARD)), ==,
                    0xffffffff);
    qtest_quit(qts);
}

/* ---- the AC'97 variant -------------------------------------------------- */

static void test_ac97_variant(void)
{
    QTestState *qts = start("ac97", "");

    /* No EEPROM: the datasheet's default subsystem pair and strap. */
    g_assert_cmphex(cfg_l(qts, 0x00), ==, 0x17241412);
    g_assert_cmphex(cfg_l(qts, 0x2c), ==, 0x17241412);
    map_card(qts);
    g_assert_cmphex(rb(qts, 0x13) & 0x80, ==, 0);
    wb(qts, 0x12, 0x77);
    wb(qts, 0x11, 0);
    wb(qts, 0x10, 0xa0);
    g_assert_cmphex(rb(qts, 0x12), ==, 0x77);
    /* AC-link mode at reset, codec ready once PRST# is released. */
    g_assert_cmphex(rb(qts, 0x05) & 0x80, ==, 0);
    g_assert_cmphex(rb(qts, MT + 0x05) & 0x08, ==, 0x08);

    /* Write cycle then read cycle at index 0x18. */
    wb(qts, MT + 0x04, 0x18);
    ww(qts, MT + 0x06, 0x1234);
    wb(qts, MT + 0x05, 0x20);
    g_assert_cmphex(rb(qts, MT + 0x05) & 0x30, ==, 0);
    ww(qts, MT + 0x06, 0);
    wb(qts, MT + 0x05, 0x10);
    g_assert_cmphex(rb(qts, MT + 0x05) & 0x30, ==, 0);
    g_assert_cmphex(rw(qts, MT + 0x06), ==, 0x1234);
    wb(qts, MT + 0x04, 0x1a);
    wb(qts, MT + 0x05, 0x10);
    g_assert_cmphex(rw(qts, MT + 0x06), ==, 0);
    wb(qts, MT + 0x04, 0x18);
    wb(qts, MT + 0x05, 0x10);
    g_assert_cmphex(rw(qts, MT + 0x06), ==, 0x1234);

    /* The cold-reset bit holds the codec not ready; cycles then do nothing. */
    wb(qts, MT + 0x05, 0x80);
    g_assert_cmphex(rb(qts, MT + 0x05) & 0x88, ==, 0x80);
    wb(qts, MT + 0x05, 0x90);
    g_assert_cmphex(rw(qts, MT + 0x06), ==, 0);
    wb(qts, MT + 0x05, 0x00);

    /* There are no converter pins: GPIO frames are not decoded. */
    ww(qts, 0x18, 0x7a);
    ww(qts, 0x16, 0);
    ww(qts, 0x14, 0x72);
    codec_write(qts, GPIO_CS_FRONT, 1, 0, 0x0f);
    g_assert_cmpuint(prop(qts, "ak4381-writes"), ==, 0);

    /* Stereo playback is the same engine. */
    map_dma_page(qts);
    fill_playback_buffer(qts);
    program_playback(qts);
    wb(qts, MT + 0x18, 0x01);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(prop(qts, "analog-frames"), ==, 48);
    qtest_quit(qts);
}

static void test_dm8_has_no_ac97_codec(void)
{
    QTestState *qts = start(NULL, "");

    map_card(qts);
    /* The DM8 is an I2S board: no codec answers on the AC-link. */
    g_assert_cmphex(rb(qts, MT + 0x05) & 0x08, ==, 0);
    wb(qts, MT + 0x04, 0x18);
    ww(qts, MT + 0x06, 0x1234);
    wb(qts, MT + 0x05, 0x20);
    wb(qts, MT + 0x05, 0x10);
    g_assert_cmphex(rw(qts, MT + 0x06), ==, 0);
    qtest_quit(qts);
}

/* ---- migration ---------------------------------------------------------- */

static void test_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-dm8-mig-XXXXXX", &error);
    g_autofree char *sock = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    sock = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", sock);
    from = start(NULL, "");
    to = start(NULL, "-incoming defer");

    map_card(from);
    map_dma_page(from);
    fill_playback_buffer(from);
    program_playback(from);
    wb(from, MT + 0x03, 0xfe);
    wb(from, 0x01, 0xef);
    load_gpio_from_eeprom(from);
    codec_write(from, GPIO_CS_FRONT, 1, 4, 0x33);
    wb(from, MT + 0x18, 0x01);
    qtest_clock_step(from, 2000000);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");

    g_assert_cmpuint(rw(to, MT + 0x14), ==, rw(from, MT + 0x14));
    g_assert_cmphex(rl(to, MT + 0x10), ==, rl(from, MT + 0x10));
    g_assert_cmphex(prop(to, "ak4381-reg4"), ==, 0x33);
    g_assert_cmpuint(prop(to, "analog-frames"), ==,
                     prop(from, "analog-frames"));
    g_assert_cmphex(rb(to, MT + 0x18), ==, 1);

    /* The restored engine keeps its pace, interval and interrupt. */
    qtest_clock_step(from, 3000000);
    qtest_clock_step(to, 3000000);
    g_assert_cmpuint(rw(to, MT + 0x14), ==, rw(from, MT + 0x14));
    g_assert_cmpuint(prop(to, "analog-frames"), ==,
                     prop(from, "analog-frames"));
    g_assert_cmphex(rb(to, MT + 0x00), ==, rb(from, MT + 0x00));
    g_assert_cmphex(rb(to, MT + 0x00), ==, 0x01);
    g_assert_true(inta(to));
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    int fd, status;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("sgi-dm8-prom-XXXXXX.bin", &prom_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);

    qtest_add_func("dm8/config-identity", test_config_identity);
    qtest_add_func("dm8/config-writes", test_config_writes);
    qtest_add_func("dm8/power-state", test_power_state);
    qtest_add_func("dm8/reset-values", test_reset_values);
    qtest_add_func("dm8/system-reset", test_system_reset);
    qtest_add_func("dm8/eeprom", test_eeprom);
    qtest_add_func("dm8/codec-writes", test_codec_writes);
    qtest_add_func("dm8/gpio-mask", test_gpio_mask);
    qtest_add_func("dm8/spdif-control", test_spdif_control);
    qtest_add_func("dm8/playback-interrupt", test_playback_interrupt);
    qtest_add_func("dm8/playback-pause-and-rate",
                   test_playback_pause_and_rate);
    qtest_add_func("dm8/eight-channel-frames", test_eight_channel_frames);
    qtest_add_func("dm8/playback-fault", test_playback_fault);
    qtest_add_func("dm8/record", test_record);
    qtest_add_func("dm8/spdif-and-stereo-engines",
                   test_spdif_and_stereo_engines);
    qtest_add_func("dm8/unknown-registers-logged",
                   test_unknown_registers_logged);
    qtest_add_func("dm8/bad-variant", test_bad_variant);
    qtest_add_func("dm8/placement", test_placement);
    qtest_add_func("dm8/placement-default-empty",
                   test_placement_default_empty);
    qtest_add_func("dm8/ac97-variant", test_ac97_variant);
    qtest_add_func("dm8/dm8-has-no-ac97-codec", test_dm8_has_no_ac97_codec);
    qtest_add_func("dm8/migration", test_migration);
    status = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    g_free(prom_path);
    return status;
}
