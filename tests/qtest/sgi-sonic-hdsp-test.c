/*
 * Register-contract tests for the Sonic Solutions HDSP card on a Bridge test
 * head.
 *
 * The card runs the firmware the user supplies; these tests never use a
 * vendor image. The synthetic image in sgi-sonic-hdsp-image.h stands in for it,
 * and the expected values are restated from the page, section by section,
 * rather than shared with the device.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include <math.h>
#include "libqos/libqos-malloc.h"
#include "libqos/pci.h"
#include "libqos/pci-sgi-bridge.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
#include "sgi-migrate.h"
#include "sgi-sonic-hdsp-image.h"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

#define SLOT            1
#define PIN_INTB_BRIDGE (1u << (SLOT ^ 4))
#define BRIDGE_INT_STATUS (0x40000000ull + 0x100 + 4)

typedef struct Card {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *fn0, *fn1;
    QPCIBar bar;
    uint64_t barsize;
    char *flash;
} Card;

static QGuestAllocator alloc;

/* @pre is command line words placed before the card (a box, an audiodev). */
static Card card_start_with(unsigned flags, const char *pre, const char *extra)
{
    Image img = { flags, SISR_BOOT_ROM };
    Card c = { 0 };

    c.flash = flash_file(&img);
    qpci_sgi_bridge_alloc_init(&alloc, 0);
    c.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                        "-chardev ringbuf,id=con,size=4096 %s"
                        "-device sgi-sonic-hdsp,id=hdsp,bus=pci,addr=%d,"
                        "flash-image=%s,iop-console=con%s",
                        pre, SLOT, c.flash, extra);
    c.bus = qpci_init_sgi_bridge(c.qts, &alloc);
    return c;
}

static Card card_start(unsigned flags, const char *extra)
{
    return card_start_with(flags, "", extra);
}

/* Let the processor run its init, then find the functions and enable BAR0. */
static void card_boot(Card *c)
{
    qtest_clock_step(c->qts, 5 * 1000 * 1000);
    c->fn0 = qpci_device_find(c->bus, QPCI_DEVFN(SLOT, 0));
    c->fn1 = qpci_device_find(c->bus, QPCI_DEVFN(SLOT, 1));
    g_assert_nonnull(c->fn0);
    g_assert_nonnull(c->fn1);
    c->bar = qpci_iomap(c->fn1, 0, &c->barsize);
    qpci_device_enable(c->fn1);
}

static void card_quit(Card *c)
{
    g_free(c->fn0);
    g_free(c->fn1);
    qpci_free_sgi_bridge(c->bus);
    qtest_quit(c->qts);
    alloc_destroy(&alloc);
    unlink(c->flash);
    g_free(c->flash);
}

static uint32_t rd(Card *c, uint64_t off)
{
    return qpci_io_readl(c->fn1, c->bar, off);
}

static void wr(Card *c, uint64_t off, uint32_t val)
{
    qpci_io_writel(c->fn1, c->bar, off, val);
}

static uint8_t rdb(Card *c, uint64_t off)
{
    return qpci_io_readb(c->fn1, c->bar, off);
}

static void wrb(Card *c, uint64_t off, uint8_t val)
{
    qpci_io_writeb(c->fn1, c->bar, off, val);
}

/* The host's offset of a scratch word, in either translate state. */
#define SCR(s)          ((s) & 0xffffff)

/* Send a message word and wait for the image to answer it. */
static uint32_t message(Card *c, uint32_t value)
{
    wr(c, 0x10, value);
    qtest_clock_step(c->qts, 2 * 1000 * 1000);
    return rd(c, 0x18);
}

static bool bridge_pin(Card *c)
{
    return qtest_readl(c->qts, BRIDGE_INT_STATUS) & PIN_INTB_BRIDGE;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * "PCI identity": function 0 is the bridge unit with header type 0x81, and
 * every configuration cycle to the card is retried while EBCR bit 2 is set,
 * which the boot code sets and the resident system clears; the reset state
 * of the bit is the model's strap.
 */
static void test_retry_until_cleared(void)
{
    Card c = card_start(FW_NO_RETRY_CLEAR, "");

    /* Before the image runs, and while it leaves the bit set. */
    g_assert_cmphex(c.bus->config_readl(c.bus, QPCI_DEVFN(SLOT, 0), 0), ==,
                    0xffffffff);
    g_assert_cmphex(c.bus->config_readl(c.bus, QPCI_DEVFN(SLOT, 1), 0), ==,
                    0xffffffff);
    qtest_clock_step(c.qts, 5 * 1000 * 1000);
    g_assert_cmphex(c.bus->config_readl(c.bus, QPCI_DEVFN(SLOT, 0), 0), ==,
                    0xffffffff);
    g_assert_cmphex(c.bus->config_readl(c.bus, QPCI_DEVFN(SLOT, 1), 0), ==,
                    0xffffffff);
    c.bus->config_writew(c.bus, QPCI_DEVFN(SLOT, 1), PCI_COMMAND, 6);
    {
        QDict *r = qtest_qmp(c.qts, "{ 'execute': 'qom-get', 'arguments': "
                             "{ 'path': '/machine/peripheral/hdsp', "
                             "'property': 'config-retries' } }");

        g_assert_cmpint(qdict_get_int(r, "return"), >=, 5);
        qobject_unref(r);
    }
    card_quit(&c);

    c = card_start(0, "");
    g_assert_cmphex(c.bus->config_readl(c.bus, QPCI_DEVFN(SLOT, 1), 0), ==,
                    0xffffffff);
    card_boot(&c);
    g_assert_cmphex(qpci_config_readw(c.fn0, PCI_VENDOR_ID), ==, 0x1263);
    g_assert_cmphex(qpci_config_readw(c.fn0, PCI_DEVICE_ID), ==, 0x060e);
    card_quit(&c);
}

/* "PCI identity": header type, class, identities and the interrupt pin. */
static void test_identity(void)
{
    Card c = card_start(0, "");

    card_boot(&c);

    /* Function 0: PCI-to-PCI bridge, header type 0x81 (multifunction). */
    g_assert_cmphex(qpci_config_readb(c.fn0, PCI_HEADER_TYPE), ==, 0x81);
    g_assert_cmphex(qpci_config_readw(c.fn0, PCI_CLASS_DEVICE), ==, 0x0604);
    g_assert_cmphex(qpci_config_readw(c.fn0, 0x34), ==, 0x1263);
    g_assert_cmphex(qpci_config_readw(c.fn0, 0x36), ==, 0x060e);
    /* The slot scan reads the header type again for each function. */
    g_assert_cmphex(qpci_config_readb(c.fn1, PCI_HEADER_TYPE) & 0x7f, ==, 0);

    /* Function 1: the ATU, as the boot code programmed it. */
    g_assert_cmphex(qpci_config_readw(c.fn1, PCI_VENDOR_ID), ==, 0x1263);
    g_assert_cmphex(qpci_config_readw(c.fn1, PCI_DEVICE_ID), ==, 0x05dc);
    g_assert_cmphex(qpci_config_readw(c.fn1, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    0x1263);
    g_assert_cmphex(qpci_config_readw(c.fn1, PCI_SUBSYSTEM_ID), ==, 0x05dc);
    g_assert_cmphex(qpci_config_readl(c.fn1, PCI_CLASS_REVISION) >> 8, ==,
                    0x050800);
    g_assert_cmphex(qpci_config_readb(c.fn1, PCI_INTERRUPT_PIN), ==, 2);

    /*
     * The driver reads the pin as a halfword at 0x3d, so the byte after it,
     * the grant register at 0x3e, is part of the value and must read zero.
     */
    g_assert_cmphex((qpci_config_readl(c.fn1, 0x3c) >> 8) & 0xffff, ==, 0x0002);
    g_assert_cmphex(qpci_config_readb(c.fn1, 0x3e), ==, 0);

    /* BAR0: memory, 32-bit, prefetchable, 128 MiB from the limit register. */
    g_assert_cmpuint(c.barsize, ==, 128 * MiB);
    g_assert_cmphex(qpci_config_readl(c.fn1, PCI_BASE_ADDRESS_0) & 0xf, ==, 8);
    /* No other BAR. */
    for (unsigned i = 1; i < 6; i++) {
        g_assert_cmphex(qpci_config_readl(c.fn1, PCI_BASE_ADDRESS_0 + 4 * i),
                        ==, 0);
    }
    card_quit(&c);
}

/*
 * "Inbound window": the local address is the PCI offset with the limit's bits
 * cleared, ORed with the translate value. At the boot code's translate the
 * glue logic is at BAR0 + 0x6000000; at the operating system's it is at
 * BAR0 + 0x4000000 and BAR0 + 0x2000000 aliases BAR0.
 */
#define GLUE_RESET_BYTE 0xa0003

static void test_translate_rule(void)
{
    Card c = card_start(0, "");

    card_boot(&c);
    /* Boot-time view: DRAM from the bottom, glue at +0x6000000. */
    wr(&c, 0x1000, 0xcafe0001);
    wr(&c, 0x2001000, 0xcafe0002);          /* local 0xa2001000 */
    g_assert_cmphex(rd(&c, 0x1000), ==, 0xcafe0001);
    g_assert_cmphex(rd(&c, 0x2001000), ==, 0xcafe0002);
    /* Reset byte after a board reset: all four DSPs held in reset. */
    g_assert_cmphex(rdb(&c, 0x6000000 + GLUE_RESET_BYTE), ==, 0x0f);
    /* Past the end of a 64 MiB DRAM nothing answers; reads are all ones. */
    g_assert_cmphex(rd(&c, 0x4000000), ==, 0xffffffff);
    /* The image reports its own view of the glue base. */
    g_assert_cmphex(rd(&c, SCR(S_BANK1)), ==, 0xa6000000);

    /* The image computes the OS translate from the DRAM it found: 64 MiB. */
    g_assert_cmphex(rd(&c, SCR(S_TRANSLATE)), ==, 0xa2000000);
    /*
     * MSG_FLIP moves the translate to it and writes the I2O class. The bottom
     * half of the DRAM is then out of the window: bit 25 is always ORed in.
     */
    g_assert_cmphex(message(&c, MSG_FLIP), ==, MSG_FLIP + 1);
    g_assert_cmphex(qpci_config_readl(c.fn1, PCI_CLASS_REVISION) >> 8, ==,
                    0x0e0001);
    g_assert_cmphex(rdb(&c, 0x4000000 + GLUE_RESET_BYTE), ==, 0x0f);
    /* The second half of the DRAM aliases the first: bit 25 is set already. */
    wr(&c, 0x1000, 0xcafe0003);
    g_assert_cmphex(rd(&c, 0x2001000), ==, 0xcafe0003);
    g_assert_cmphex(rd(&c, 0x1000), ==, 0xcafe0003);
    card_quit(&c);
}

/* "Messaging unit": the handshake, the reply and the interrupt pin. */
static void test_message_handshake(void)
{
    Card c = card_start(0, "");

    card_boot(&c);
    g_assert_false(bridge_pin(&c));
    wr(&c, 0x34, 0xffffffff);               /* mask every source first */
    g_assert_cmphex(message(&c, 0x699), ==, 0x69a);
    g_assert_false(bridge_pin(&c));         /* masked: no interrupt */
    wr(&c, 0x34, 0);
    /* The reply is pending in OISR bit 0 and now reaches INTB. */
    g_assert_cmphex(rd(&c, 0x30) & 1, ==, 1);
    g_assert_true(bridge_pin(&c));
    wr(&c, 0x30, 1);                        /* the host clears the bit */
    g_assert_cmphex(rd(&c, 0x30) & 1, ==, 0);
    g_assert_false(bridge_pin(&c));
    /* The image consumed the inbound message and cleared its status. */
    g_assert_cmphex(rd(&c, 0x24) & 1, ==, 0);
    card_quit(&c);
}

/*
 * "Messaging unit": the queue ports. The inbound port hands out the free
 * frame the image posted and then reads all ones; a request posted through it
 * is read, answered through the outbound queue, and the outbound port returns
 * the reply and then all ones.
 */
static void test_queue_ports(void)
{
    Card c = card_start(0, "");
    uint32_t frame;

    card_boot(&c);
    wr(&c, 0x34, 0);
    frame = rd(&c, 0x40);
    g_assert_cmphex(frame, ==, FRAME);
    g_assert_cmphex(rd(&c, 0x40), ==, 0xffffffff);          /* empty */
    g_assert_cmphex(rd(&c, 0x44), ==, 0xffffffff);          /* empty */
    g_assert_false(bridge_pin(&c));
    /*
     * Rx manual 17.7.13 to 17.7.20: a read of a queue pointer register is the
     * local address (QBAR's upper bits, then the offset); a write keeps bits
     * 19:2 only.
     */
    g_assert_cmphex(rd(&c, SCR(S_IPH_READ)), ==, QBAR + 0x4000);
    g_assert_cmphex(rd(&c, SCR(S_MASK_READ)), ==, QBAR + 0xdbeec);

    /* Write the request frame through the window, then post its address. */
    wr(&c, frame - LOCAL_DRAM, 0x1234abcd);
    wr(&c, 0x40, frame);
    qtest_clock_step(c.qts, 2 * 1000 * 1000);
    g_assert_true(bridge_pin(&c));          /* outbound post queue not empty */
    g_assert_cmphex(rd(&c, 0x44), ==, 0x1234abce);
    g_assert_cmphex(rd(&c, 0x44), ==, 0xffffffff);
    g_assert_false(bridge_pin(&c));         /* empty again */
    card_quit(&c);
}

/*
 * "Glue logic" and "Board glue the boot ROM assumes": the reset byte at byte
 * position 3 of its word, seen by the host at the glue base and by the i960
 * at the bank 1 base; the UART's transmit-ready status and its transmit data
 * reaching the character device; the board-identification byte; and an
 * unclaimed local read.
 */
static void test_glue_and_uart(void)
{
    Card c = card_start(0, "");
    QDict *r;

    card_boot(&c);
    wrb(&c, 0x6000000 + GLUE_RESET_BYTE, 0x05);
    g_assert_cmphex(rdb(&c, 0x6000000 + GLUE_RESET_BYTE), ==, 0x05);
    g_assert_cmphex(message(&c, 5), ==, 6);
    g_assert_cmphex(rd(&c, SCR(S_GLUE)), ==, 0x05);     /* the i960's view */

    g_assert_cmphex(rd(&c, SCR(S_UART_STATUS)), ==, 0x0c);
    g_assert_cmphex(rd(&c, SCR(S_BOARD_ID)) >> 4, ==, 1);
    g_assert_cmphex(rd(&c, SCR(S_UNCLAIMED)), ==, 0xffffffff);
    /* Outside every bank: all ones, no fault, whatever the image reads. */
    g_assert_cmphex(rd(&c, SCR(S_FAULT_AREA)), ==, 0xffffffff);
    /* The boot-mode byte has bit 7 set. */
    g_assert_cmphex(rd(&c, SCR(S_BOOT_MODE)) & 0x80, ==, 0x80);

    r = qtest_qmp(c.qts, "{ 'execute': 'ringbuf-read', 'arguments': "
                  "{ 'device': 'con', 'size': 16, 'format': 'utf8' } }");
    g_assert_cmpstr(qdict_get_str(r, "return"), ==, "HI");
    qobject_unref(r);
    card_quit(&c);
}

/*
 * "DRAM and flash": the image sizes the DRAM by readback; with 32 MiB the
 * 64 MiB probe finds nothing and the translate follows the size. A bad
 * dram-size is refused.
 */
static void test_dram_size(void)
{
    Card c = card_start(0, ",dram-size=32M");

    card_boot(&c);
    g_assert_cmphex(rd(&c, SCR(S_PROBE32)), ==, MAGIC_32);
    g_assert_cmphex(rd(&c, SCR(S_PROBE64)), !=, MAGIC_64);
    g_assert_cmphex(rd(&c, SCR(S_PROBE64)), ==, 0xffffffff);
    g_assert_cmphex(rd(&c, SCR(S_TRANSLATE)), ==, 0xa1000000);
    /* Bank 1 sits half the DRAM and 64 MiB above its base. */
    g_assert_cmphex(rd(&c, SCR(S_BANK1)), ==, 0xa5000000);
    card_quit(&c);
}

/*
 * "DRAM and flash": the AT29C040A programming model. A sector program cycle
 * needs the flash to be writable; the bytes land after the 10 ms write cycle.
 */
static void test_flash_program(void)
{
    Card c = card_start(FW_FLASH_PROGRAM, ",flash-writable=on");

    card_boot(&c);
    qtest_clock_step(c.qts, 20 * 1000 * 1000);      /* the write cycle */
    wr(&c, WAIT_FLAG & 0xffffff, 1);                /* release the image */
    g_assert_cmphex(message(&c, 1), ==, 2);
    g_assert_cmphex(rd(&c, SCR(S_FLASH)), ==, 0x2211);
    card_quit(&c);

    c = card_start(FW_FLASH_PROGRAM, "");
    card_boot(&c);
    qtest_clock_step(c.qts, 20 * 1000 * 1000);
    wr(&c, WAIT_FLAG & 0xffffff, 1);
    g_assert_cmphex(message(&c, 1), ==, 2);
    g_assert_cmphex(rd(&c, SCR(S_FLASH)), ==, 0);       /* dropped */
    card_quit(&c);
}

/*
 * "Secondary PCI bus and the host port": a DSP held in reset does not answer
 * a configuration cycle (the model holds all four after reset); released by
 * the glue's reset byte it answers with the DSP56301's hardwired identity.
 * An IDSEL line private to the chip never answers.
 */
static void test_dsp_function_reset(void)
{
    Card c = card_start(0, "");

    card_boot(&c);
    g_assert_cmphex(message(&c, 1), ==, 2);
    g_assert_cmphex(rd(&c, SCR(S_DSP0_ID)), ==, 0xffffffff);
    g_assert_cmphex(rd(&c, SCR(S_DSP3_ID)), ==, 0xffffffff);
    g_assert_cmphex(rd(&c, SCR(S_PRIVATE_ID)), ==, 0xffffffff);

    wrb(&c, 0x6000000 + GLUE_RESET_BYTE, 0x08);     /* release DSPs 0 to 2 */
    g_assert_cmphex(message(&c, 3), ==, 4);
    g_assert_cmphex(rd(&c, SCR(S_DSP0_ID)), ==, 0x18011057);
    g_assert_cmphex(rd(&c, SCR(S_DSP0_CSID)), ==, 0);
    g_assert_cmphex(rd(&c, SCR(S_DSP3_ID)), ==, 0xffffffff);
    wrb(&c, 0x6000000 + GLUE_RESET_BYTE, 0x00);
    g_assert_cmphex(message(&c, 5), ==, 6);
    g_assert_cmphex(rd(&c, SCR(S_DSP3_ID)), ==, 0x18011057);
    g_assert_cmphex(rd(&c, SCR(S_PRIVATE_ID)), ==, 0xffffffff);
    card_quit(&c);
}

/*
 * "Firmware image": the device refuses to realize without a flash image of
 * exactly 512 KiB. Run on the Malta board, which has a PCI bus without a
 * Bridge, so this also shows the card is not tied to the Octane.
 */
static void realize_fails(const char *props, const char *want)
{
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *cmd = g_strdup_printf("%s -M malta -nic none -vga none "
        "-display none -S -device sgi-sonic-hdsp,bus=pci,addr=0x12%s", qemu,
        props);
    g_autofree char *err = NULL;
    g_auto(GStrv) argv = NULL;
    int status = 0;
    GError *gerr = NULL;

    g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, &gerr));
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &err, &status, &gerr));
    g_assert_cmpint(status, !=, 0);
    g_assert_nonnull(strstr(err, want));
}

static void test_flash_refused(void)
{
    g_autofree char *path = NULL;
    g_autofree char *props = NULL;
    GError *err = NULL;
    int fd = g_file_open_tmp("sonic-hdsp-short-XXXXXX", &path, &err);

    g_assert_no_error(err);
    g_assert_cmpint(write(fd, "short", 5), ==, 5);
    close(fd);

    realize_fails("", "flash-image is required");
    props = g_strdup_printf(",flash-image=%s", path);
    realize_fails(props, "is 5 bytes, not 524288");
    realize_fails(",flash-image=/nonexistent/sonic.bin", "cannot read");
    unlink(path);
}

/*
 * "Hiding the DSPs from the host scan": a host type 1 scan of devices 0 to 31
 * behind function 0. The Bridge does not decode type 1 cycles, so this runs on
 * the Malta board, whose PCI host passes the bus number through to the PCI
 * core. SISR bit n hides S_AD line 16 + n, which is device n of the scan; the
 * DSPs sit on S_AD17 to S_AD20, devices 1 to 4. A DSP held in reset is not
 * found either.
 */
#define GT64120_ISD     0x14000000ull
#define GT_CFGADDR      (GT64120_ISD + 0xcf8)
#define GT_CFGDATA      (GT64120_ISD + 0xcfc)

static bool gt_data_swapped;

/* The GT-64120's configuration ports are little-endian words. */
static uint32_t malta_cfg_read(QTestState *qts, unsigned bus, unsigned dev,
                               unsigned fn, unsigned reg)
{
    uint32_t addr = 0x80000000u | bus << 16 | dev << 11 | fn << 8 | reg;
    uint32_t v;

    qtest_writel(qts, GT_CFGADDR, bswap32(addr));
    v = qtest_readl(qts, GT_CFGDATA);
    return gt_data_swapped ? bswap32(v) : v;
}

static void malta_cfg_write(QTestState *qts, unsigned bus, unsigned dev,
                            unsigned fn, unsigned reg, uint32_t val)
{
    uint32_t addr = 0x80000000u | bus << 16 | dev << 11 | fn << 8 | reg;

    qtest_writel(qts, GT_CFGADDR, bswap32(addr));
    qtest_writel(qts, GT_CFGDATA, gt_data_swapped ? bswap32(val) : val);
}

#define MALTA_SLOT      0x12

/* Which devices of the secondary bus answer, as a mask of device numbers. */
static uint32_t type1_scan(QTestState *qts)
{
    uint32_t found = 0;

    for (unsigned dev = 0; dev < 32; dev++) {
        uint32_t id = malta_cfg_read(qts, 1, dev, 0, 0);

        if ((id & 0xffff) != 0xffff) {
            found |= 1u << dev;
            g_assert_cmphex(id, ==, 0x18011057);
        }
    }
    return found;
}

static uint32_t scan_with(unsigned flags, uint16_t sisr, const char *props)
{
    Image img = { flags, sisr };
    g_autofree char *path = flash_file(&img);
    QTestState *qts = qtest_initf("-M malta -nic none -vga none "
                                  "-device sgi-sonic-hdsp,bus=pci,addr=%x,"
                                  "flash-image=%s%s", MALTA_SLOT, path, props);
    uint32_t found;

    /* Calibrate the byte order against the board's ISA bridge (8086:7110). */
    qtest_writel(qts, GT_CFGADDR, bswap32(0x80000000u | 10 << 11));
    gt_data_swapped = qtest_readl(qts, GT_CFGDATA) != 0x71108086;
    g_assert_cmphex(malta_cfg_read(qts, 0, 10, 0, 0), ==, 0x71108086);

    /* Retry until the image has run; then the bridge unit answers. */
    g_assert_cmphex(malta_cfg_read(qts, 0, MALTA_SLOT, 0, 0), ==, 0xffffffff);
    qtest_clock_step(qts, 5 * 1000 * 1000);
    g_assert_cmphex(malta_cfg_read(qts, 0, MALTA_SLOT, 0, 0), ==, 0x060e1263);
    g_assert_cmphex(malta_cfg_read(qts, 0, MALTA_SLOT, 1, 0), ==, 0x05dc1263);
    /* Header type 0x81 at byte 2 of the dword at 0x0c. */
    g_assert_cmphex((malta_cfg_read(qts, 0, MALTA_SLOT, 0, 0x0c) >> 16) &
                    0xff, ==, 0x81);
    /* Primary 0, secondary 1, subordinate 1. */
    malta_cfg_write(qts, 0, MALTA_SLOT, 0, 0x18, 0x00010100);

    found = type1_scan(qts);
    qtest_quit(qts);
    unlink(path);
    return found;
}

static void test_type1_scan(void)
{
    /* Held in reset after the board reset, the DSPs are never found. */
    g_assert_cmphex(scan_with(0, SISR_BOOT_ROM, ""), ==, 0);
    /* The boot code's SISR leaves S_AD17 to S_AD20 reachable. */
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS, SISR_BOOT_ROM, ""), ==, 0x1e);
    /* SISR with bits 1 to 4 added hides all four DSPs. */
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS, SISR_BOOT_ROM | 0x1e, ""), ==,
                    0);
    /* A blank store hides nothing. */
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS, 0, ""), ==, 0x1e);
    /* Bit n hides device n: bits 1 and 2 hide DSPs 0 and 1. */
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS, 0x6, ""), ==, 0x18);
    /*
     * The image reads the nv word from the EEPROM and loads SISR from it, as
     * the page says the resident system does: the default word hides every
     * DSP, and a blank word (magic only) hides none.
     */
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS | FW_SISR_FROM_EEPROM, 0, ""),
                    ==, 0);
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS | FW_SISR_FROM_EEPROM, 0,
                              ",nv-word=0x98760000"), ==, 0x1e);
    /* A word without the 0x9876 magic gives SISR = 0 as well. */
    g_assert_cmphex(scan_with(FW_RELEASE_DSPS | FW_SISR_FROM_EEPROM, 0,
                              ",nv-word=0x000003df"), ==, 0x1e);
}

/*
 * A DMA descriptor chain that points at itself must not hang the machine:
 * the model stops it after a bounded number of descriptors and carries on.
 */
static void test_dma_loop_bounded(void)
{
    Card c = card_start(FW_DMA_LOOP, "");

    card_boot(&c);
    g_assert_cmphex(message(&c, 1), ==, 2);
    card_quit(&c);
}

/*
 * A self-looping descriptor 16 MiB long is moved a slice at a time: starting
 * it returns promptly, and the card keeps answering messages while the
 * channel stays busy.
 */
static void test_dma_big_loop(void)
{
    Card c = card_start(FW_DMA_BIG, "");

    card_boot(&c);
    g_assert_cmphex(message(&c, MSG_DMA), ==, MSG_DMA + 1);
    qtest_clock_step(c.qts, 10 * 1000 * 1000);
    g_assert_cmphex(message(&c, 1), ==, 2);
    card_quit(&c);
}

/* "I2C": the EEPROM's first four bytes are nv-word, little-endian. */
static void test_eeprom_read(void)
{
    Card c = card_start(FW_SISR_FROM_EEPROM, ",nv-word=0x12345678");

    card_boot(&c);
    g_assert_cmphex(rd(&c, SCR(S_NVWORD)), ==, 0x12345678);
    card_quit(&c);
}

/*
 * Migration: the processor's state, the units and the DRAM travel; the
 * destination goes on answering the messaging unit from where the source was.
 */
static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("sonic-hdsp-migrate-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    Card from = card_start(0, ""), to = { 0 };

    card_boot(&from);
    wr(&from, 0x1000, 0x5ca1ab1e);
    g_assert_cmphex(message(&from, 0x40), ==, 0x41);
    wrb(&from, 0x6000000 + GLUE_RESET_BYTE, 0x03);

    to.flash = g_strdup(from.flash);
    to.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                         "-device sgi-sonic-hdsp,id=hdsp,bus=pci,addr=%d,"
                         "flash-image=%s -incoming %s", SLOT, from.flash, uri);
    qtest_qmp_assert_success(from.qts, "{ 'execute': 'stop' }");
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    to.bus = qpci_init_sgi_bridge(to.qts, &alloc);
    sgi_migrate_resume(to.qts);
    to.fn0 = qpci_device_find(to.bus, QPCI_DEVFN(SLOT, 0));
    to.fn1 = qpci_device_find(to.bus, QPCI_DEVFN(SLOT, 1));
    g_assert_nonnull(to.fn1);
    to.bar = from.bar;                  /* the config space says where */

    g_assert_cmphex(qpci_config_readw(to.fn1, PCI_DEVICE_ID), ==, 0x05dc);
    g_assert_cmphex(qpci_config_readb(to.fn0, PCI_HEADER_TYPE), ==, 0x81);
    g_assert_cmphex(rd(&to, 0x1000), ==, 0x5ca1ab1e);
    g_assert_cmphex(rd(&to, 0x18), ==, 0x41);
    g_assert_cmphex(rdb(&to, 0x6000000 + GLUE_RESET_BYTE), ==, 0x03);
    /* The processor goes on running: it answers a message after the move. */
    g_assert_cmphex(message(&to, 0x50), ==, 0x51);

    g_free(from.fn0);
    g_free(from.fn1);
    qpci_free_sgi_bridge(from.bus);
    qtest_quit(from.qts);
    g_free(to.fn0);
    g_free(to.fn1);
    qpci_free_sgi_bridge(to.bus);
    qtest_quit(to.qts);
    alloc_destroy(&alloc);
    unlink(from.flash);
    g_free(from.flash);
    g_free(to.flash);
    g_rmdir(dir);
}

/*
 * A write into DRAM after the RAM has been sent, while the migration waits
 * before the switchover, must reach the destination: the model's direct DRAM
 * stores have to mark their pages dirty.
 */
static void test_migration_dirty_dram(void)
{
    g_autofree char *dir = g_dir_make_tmp("sonic-hdsp-dirty-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    Card from = card_start(0, ""), to = { 0 };

    card_boot(&from);
    to.flash = g_strdup(from.flash);
    to.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                         "-device sgi-sonic-hdsp,id=hdsp,bus=pci,addr=%d,"
                         "flash-image=%s -incoming %s", SLOT, from.flash, uri);
    qtest_qmp_assert_success(from.qts, "{ 'execute': "
        "'migrate-set-capabilities', 'arguments': { 'capabilities': [ "
        "{ 'capability': 'pause-before-switchover', 'state': true } ] } }");
    qtest_qmp_assert_success(to.qts, "{ 'execute': "
        "'migrate-set-capabilities', 'arguments': { 'capabilities': [ "
        "{ 'capability': 'pause-before-switchover', 'state': true } ] } }");
    qtest_qmp_assert_success(from.qts,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    sgi_wait_migration(from.qts, "pre-switchover");
    /* The RAM went over in the first pass; this store must be re-sent. */
    wr(&from, 0x1000, 0xfeedbeef);
    qtest_qmp_assert_success(from.qts,
        "{ 'execute': 'migrate-continue', 'arguments': "
        "{ 'state': 'pre-switchover' } }");
    sgi_wait_migration(from.qts, "completed");
    sgi_wait_migration(to.qts, "completed");
    to.bus = qpci_init_sgi_bridge(to.qts, &alloc);
    to.fn0 = qpci_device_find(to.bus, QPCI_DEVFN(SLOT, 0));
    to.fn1 = qpci_device_find(to.bus, QPCI_DEVFN(SLOT, 1));
    g_assert_nonnull(to.fn1);
    to.bar = from.bar;
    g_assert_cmphex(rd(&to, 0x1000), ==, 0xfeedbeef);

    g_free(from.fn0);
    g_free(from.fn1);
    qpci_free_sgi_bridge(from.bus);
    qtest_quit(from.qts);
    g_free(to.fn0);
    g_free(to.fn1);
    qpci_free_sgi_bridge(to.bus);
    qtest_quit(to.qts);
    alloc_destroy(&alloc);
    unlink(from.flash);
    g_free(from.flash);
    g_free(to.flash);
    g_rmdir(dir);
}

/* ------------------------------------------------------------------ */
/* The DSPs                                                            */
/* ------------------------------------------------------------------ */

/*
 * The host reaches a DSP through the image's proxy: a list of local bus
 * accesses the i960 runs. A DSP's BAR0 is at 0x10000 * (n + 1) on the
 * secondary bus, which the outbound window shows at 0x88000000 plus that
 * base; the host-port registers are at +0x10 (HCTR), +0x14 (HSTR), +0x18
 * (HCVR) and +0x1c (the data port), as the page lists them.
 */
#define SEC_WINDOW      0x88000000u
#define DSP_BASE(n)     (0x10000u * ((n) + 1))
#define HP_HCTR         0x10
#define HP_HSTR         0x14
#define HP_HCVR         0x18
#define HP_DATA         0x1c
#define HSTR_TRDY       (1u << 0)
#define HSTR_HTRQ       (1u << 1)
#define HSTR_HRRQ       (1u << 2)

#define PX_GO_OFF       (PROXY_GO & 0xffffff)
#define PX_LIST_OFF     (PROXY_LIST & 0xffffff)

typedef struct Proxy {
    uint32_t op[PROXY_MAX], addr[PROXY_MAX], val[PROXY_MAX];
    unsigned n;
} Proxy;

static unsigned px_add(Proxy *p, uint32_t op, uint32_t addr, uint32_t val)
{
    g_assert_cmpuint(p->n, <, PROXY_MAX - 1);
    p->op[p->n] = op;
    p->addr[p->n] = addr;
    p->val[p->n] = val;
    return p->n++;
}

static void px_write(Proxy *p, uint32_t addr, uint32_t val)
{
    px_add(p, PX_WRITE, addr, val);
}

static unsigned px_read(Proxy *p, uint32_t addr)
{
    return px_add(p, PX_READ, addr, 0);
}

/* Secondary configuration cycle: IDSEL for DSP n, the register, the data. */
static void px_cfg_write(Proxy *p, unsigned n, uint32_t reg, uint32_t val)
{
    px_write(p, 0x12a8, (1u << (17 + n)) | reg);
    px_write(p, 0x12b0, val);
}

static void px_hp_write(Proxy *p, unsigned n, uint32_t off, uint32_t val)
{
    px_write(p, SEC_WINDOW + DSP_BASE(n) + off, val);
}

static unsigned px_hp_read(Proxy *p, unsigned n, uint32_t off)
{
    return px_read(p, SEC_WINDOW + DSP_BASE(n) + off);
}

/* Run the list on the i960 and fetch the loaded values. */
static void px_run(Card *c, Proxy *p)
{
    for (unsigned i = 0; i < p->n; i++) {
        wr(c, PX_LIST_OFF + 12 * i, p->op[i]);
        wr(c, PX_LIST_OFF + 12 * i + 4, p->addr[i]);
        wr(c, PX_LIST_OFF + 12 * i + 8, p->val[i]);
    }
    wr(c, PX_LIST_OFF + 12 * p->n, PX_END);
    wr(c, PX_GO_OFF, 1);
    for (unsigned i = 0; i < 200 && rd(c, PX_GO_OFF); i++) {
        qtest_clock_step(c->qts, 100 * 1000);
    }
    g_assert_cmpuint(rd(c, PX_GO_OFF), ==, 0);
    for (unsigned i = 0; i < p->n; i++) {
        if (p->op[i] == PX_READ) {
            p->val[i] = rd(c, PX_LIST_OFF + 12 * i + 8);
        }
    }
    p->n = 0;
}

static uint32_t hp_read(Card *c, unsigned n, uint32_t off)
{
    Proxy *p = g_new0(Proxy, 1);
    unsigned i = px_hp_read(p, n, off);
    uint32_t v;

    px_run(c, p);
    v = p->val[i];
    g_free(p);
    return v;
}

static void hp_write(Card *c, unsigned n, uint32_t off, uint32_t val)
{
    Proxy *p = g_new0(Proxy, 1);

    px_hp_write(p, n, off, val);
    px_run(c, p);
    g_free(p);
}

/*
 * A DSP the way the module brings it up: out of reset (the glue byte), then
 * configuration: base address and the command register that enables it.
 */
static void dsp_release(Card *c, unsigned n)
{
    Proxy *p = g_new0(Proxy, 1);
    uint8_t reset = rdb(c, 0x6000000 + GLUE_RESET_BYTE);

    wrb(c, 0x6000000 + GLUE_RESET_BYTE, reset & ~(1u << n));
    px_cfg_write(p, n, 0x10, DSP_BASE(n));
    px_cfg_write(p, n, 0x04, 0xf8000006);
    px_run(c, p);
    g_free(p);
}

/* Host bootstrap: format word, count, start address, then the program. */
static void dsp_boot(Card *c, unsigned n, uint32_t start,
                     const uint32_t *prog, unsigned len)
{
    Proxy *p = g_new0(Proxy, 1);

    px_hp_write(p, n, HP_HCTR, 0x900);
    px_hp_write(p, n, HP_DATA, len);
    px_hp_write(p, n, HP_DATA, start);
    for (unsigned i = 0; i < len; i++) {
        px_hp_write(p, n, HP_DATA, prog[i]);
    }
    px_run(c, p);
    g_free(p);
}

/*
 * Data words a DSP wrote, in order: a status read then a data read for each
 * of up to @max words, stopping when the receive request clears.
 */
static unsigned dsp_words(Card *c, unsigned n, uint32_t *out, unsigned max)
{
    Proxy *p = g_new0(Proxy, 1);
    unsigned st[32], dat[32], got = 0;

    g_assert_cmpuint(max, <=, 32);
    for (unsigned i = 0; i < max; i++) {
        st[i] = px_hp_read(p, n, HP_HSTR);
        dat[i] = px_hp_read(p, n, HP_DATA);
    }
    px_run(c, p);
    for (unsigned i = 0; i < max; i++) {
        if (!(p->val[st[i]] & HSTR_HRRQ)) {
            break;
        }
        out[got++] = p->val[dat[i]];
    }
    g_free(p);
    return got;
}

/*
 * DSP56300 instruction words, written from the instruction formats in the
 * family manual: MOVEP between a register and X:pp, MOVEP of a long
 * immediate to X:pp or X:qq, JCLR on X:pp, and the fixed words.
 */
#define PP(a)           ((a) - 0xffffc0u)
#define QQ(a)           ((a) - 0xffff80u)
#define MOVEP_RD(d, a)  (0x084000u | ((d) << 8) | PP(a))
#define MOVEP_WR(d, a)  (0x08c000u | ((d) << 8) | PP(a))
#define MOVEP_IPP(a)    (0x08f480u | PP(a))
#define MOVEP_IQQ(a)    (0x07f400u | QQ(a))
#define JCLR_PP(b, a)   (0x0a8080u | (PP(a) << 8) | (b))
#define JMP(a)          (0x0c0000u | (a))
#define JSR(a)          (0x0d0000u | (a))
#define DSP_WAIT        0x000086u
#define DSP_RTI         0x000004u
#define ANDI_MR(m)      (0x0000b8u | ((m) << 8))
#define REG_A1          0x0c
/* MOVEP X:qq,D and D,X:qq: the six-bit qq splits around bit 5. */
#define QQ_BITS(a)      (((((a) - 0xffff80u) & 0x20) << 1) | \
                         (((a) - 0xffff80u) & 0x1f))
#define MOVEQQ_RD(d, a) (0x044080u | ((d) << 8) | QQ_BITS(a))

#define X_DCTR          0xffffc5u
#define X_DSR           0xffffc9u
#define X_DRXR          0xffffcbu
#define X_DTXS          0xffffcdu
#define X_IPRP          0xfffffeu
#define X_CRA0          0xffffb5u
#define X_CRB0          0xffffb6u
#define X_TX00          0xffffbcu
#define X_RX0           0xffffb8u
#define X_CRA1          0xffffa5u
#define X_CRB1          0xffffa6u
#define X_TX10          0xffffacu
#define X_RX1           0xffffa8u
#define X_TSMA0         0xffffb4u
#define X_RSMA0         0xffffb2u
#define X_SSISR1        0xffffa7u
#define X_TSR1          0xffffa9u
#define X_TCSR0         0xffff8fu
#define X_TLR0          0xffff8eu
#define X_TCPR0         0xffff8du
#define X_DSR_CH(ch)    (0xffffefu - 4 * (ch))
#define X_DDR_CH(ch)    (0xffffeeu - 4 * (ch))
#define X_DCO_CH(ch)    (0xffffedu - 4 * (ch))
#define X_DCR_CH(ch)    (0xffffecu - 4 * (ch))

/* DCR: enabled, word transfers (DTM 1), the request source, the modes. */
#define DCR(drs, dam, dds, dss) \
    (0x800000u | (1u << 19) | ((drs) << 11) | ((dam) << 4) | ((dds) << 2) | \
     (dss))
#define DRS_ESSI0_RX    0x0a
#define DRS_ESSI0_TX    0x0b
#define DRS_ESSI1_RX    0x0c
#define DRS_ESSI1_TX    0x0d
#define DRS_TIMER0      0x10
#define DAM_NONE_NONE   0x24            /* both ends fixed */
#define DAM_NONE_INC    0x25            /* destination fixed, source steps */
#define SP_X            0
#define SP_P            2

/* Two words that set a peripheral register to an immediate, appended. */
#define PUT_PP(prog, i, a, v) \
    do { (prog)[(i)++] = MOVEP_IPP(a); (prog)[(i)++] = (v); } while (0)
#define PUT_QQ(prog, i, a, v) \
    do { (prog)[(i)++] = MOVEP_IQQ(a); (prog)[(i)++] = (v); } while (0)

#define DATA_AT         0x120

/*
 * "Secondary PCI bus and the host port", bootstrap: a program the host
 * downloads runs, and answers through the host port: it announces itself,
 * then echoes every word the host sends.
 */
static void test_dsp_runs_program(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t out[8];
    const uint32_t prog[] = {
        /* 0x100 */ MOVEP_IPP(X_DTXS), 0xc0ffee,
        /* 0x102 */ JCLR_PP(2, X_DSR), 0x102,
        /* 0x104 */ MOVEP_RD(REG_A1, X_DRXR),
        /* 0x105 */ MOVEP_WR(REG_A1, X_DTXS),
        /* 0x106 */ JMP(0x102),
    };

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, ARRAY_SIZE(prog));
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0xc0ffee);

    hp_write(&c, 0, HP_DATA, 0x123456);
    hp_write(&c, 0, HP_DATA, 0xabcdef);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 4), ==, 2);
    g_assert_cmphex(out[0], ==, 0x123456);
    g_assert_cmphex(out[1], ==, 0xabcdef);
    /* The transmit side is empty again: TRDY and HTRQ set, HRRQ clear. */
    g_assert_cmphex(hp_read(&c, 0, HP_HSTR), ==, HSTR_TRDY | HSTR_HTRQ);
    card_quit(&c);
}

/*
 * The bootstrap stops early when the host sets HF0 with the FIFO empty, and
 * the program starts at the address it was given, not at a block's end.
 */
static void test_dsp_boot_hf0(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t out[4];
    Proxy *p = g_new0(Proxy, 1);
    const uint32_t word[] = { MOVEP_IPP(X_DTXS), 0x5a5a5a, DSP_WAIT };

    card_boot(&c);
    dsp_release(&c, 1);
    /* Announce 4 words, send 3, and the DSP is still loading. */
    px_hp_write(p, 1, HP_HCTR, 0x900);
    px_hp_write(p, 1, HP_DATA, 4);
    px_hp_write(p, 1, HP_DATA, 0x200);
    for (unsigned i = 0; i < ARRAY_SIZE(word); i++) {
        px_hp_write(p, 1, HP_DATA, word[i]);
    }
    px_run(&c, p);
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmpuint(dsp_words(&c, 1, out, 2), ==, 0);
    /* HF0 (HCTR bit 3) ends the load; the program at 0x200 runs. */
    hp_write(&c, 1, HP_HCTR, 0x900 | (1u << 3));
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmpuint(dsp_words(&c, 1, out, 2), ==, 1);
    g_assert_cmphex(out[0], ==, 0x5a5a5a);
    g_free(p);
    card_quit(&c);
}

/*
 * A start-up program for the host command tests: entry at 0x40 sets IPRP to
 * @iprp (1 enables the host command interrupt at level 0) and DCTR[HCIE],
 * lowers the mask when @unmask, and waits at 0xf8. The vectors for HV 0x7a
 * (0xf4) and 0x7e (0xfc) call handlers that announce a word and return.
 */
static unsigned host_command_program(uint32_t *prog, bool unmask,
                                     uint32_t iprp)
{
    unsigned i = 0;

    memset(prog, 0, 0xcc * sizeof(*prog));
    prog[i++] = MOVEP_IPP(X_IPRP);
    prog[i++] = iprp;
    prog[i++] = MOVEP_IPP(X_DCTR);
    prog[i++] = 0x100001;
    if (unmask) {
        prog[i++] = ANDI_MR(0xfc);
    }
    prog[i++] = JMP(0xf8);
    g_assert_cmpuint(i, <, 0xf4 - 0x40);
    prog[0xf4 - 0x40] = JSR(0x100);
    prog[0xf8 - 0x40] = DSP_WAIT;
    prog[0xf9 - 0x40] = JMP(0xf8);
    prog[0xfc - 0x40] = JSR(0x108);
    prog[0x100 - 0x40] = MOVEP_IPP(X_DTXS);
    prog[0x101 - 0x40] = 0x00beef;
    prog[0x102 - 0x40] = DSP_RTI;
    prog[0x108 - 0x40] = MOVEP_IPP(X_DTXS);
    prog[0x109 - 0x40] = 0x00dead;
    prog[0x10a - 0x40] = DSP_RTI;
    return 0x10b - 0x40;
}

/*
 * HCVR: a host command takes the vector in HV (twice its value) once the
 * core's mask and the enables allow it, and the interrupt logic clears HC.
 */
static void test_dsp_host_command(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0xcc], out[4];
    unsigned len = host_command_program(prog, true, 1);

    card_boot(&c);
    dsp_release(&c, 3);
    dsp_boot(&c, 3, 0x40, prog, len);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 3, out, 4), ==, 0);

    hp_write(&c, 3, HP_HCVR, 0xf5);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 3, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0xbeef);
    g_assert_cmphex(hp_read(&c, 3, HP_HCVR) & 1, ==, 0);

    /* HNMI with HC and the vector at 0xfc, as the module's clear does. */
    hp_write(&c, 3, HP_HCVR, 0x80fd);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 3, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0xdead);
    g_assert_cmphex(hp_read(&c, 3, HP_HCVR) & 1, ==, 0);
    card_quit(&c);
}

/*
 * A masked core does not take a maskable host command, and HC stays set for
 * the host to see; a non-maskable one is taken regardless.
 */
static void test_dsp_host_command_masked(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0xcc], out[4];
    unsigned len = host_command_program(prog, false, 1);

    card_boot(&c);
    dsp_release(&c, 2);
    dsp_boot(&c, 2, 0x40, prog, len);
    qtest_clock_step(c.qts, 1000 * 1000);
    hp_write(&c, 2, HP_HCVR, 0xf5);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 2, out, 4), ==, 0);
    g_assert_cmphex(hp_read(&c, 2, HP_HCVR) & 1, ==, 1);

    hp_write(&c, 2, HP_HCVR, 0x80fd);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 2, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0xdead);
    card_quit(&c);
}

/*
 * HI32 flags: the host flags HF2 to HF0 the host sets in HCTR read in the
 * DSP's status register with HACT, and the flags and interrupt bit the DSP
 * sets in DCTR read in the host's HSTR.
 */
static void test_dsp_hi32_flags(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t out[2];
    const uint32_t prog[] = {
        /* 0x100 */ MOVEP_IPP(X_DCTR), 0x100078,   /* HM = PCI, HINT, HF5-3 */
        /* 0x102 */ JCLR_PP(2, X_DSR), 0x102,
        /* 0x104 */ MOVEP_RD(REG_A1, X_DSR),
        /* 0x105 */ MOVEP_WR(REG_A1, X_DTXS),
        /* 0x106 */ DSP_WAIT,
    };

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, ARRAY_SIZE(prog));
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmphex(hp_read(&c, 0, HP_HSTR), ==,
                    HSTR_TRDY | HSTR_HTRQ | 0x78);
    hp_write(&c, 0, HP_HCTR, 0x900 | 0x28);         /* HF2 and HF0 */
    hp_write(&c, 0, HP_DATA, 0);
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 2), ==, 1);
    /* HACT, HF2 and HF0, SRRQ (the word is still queued) and STRQ. */
    g_assert_cmphex(out[0], ==, 0x800000 | 0x28 | 0x4 | 0x2);
    card_quit(&c);
}

/*
 * ESSI between cores: with x-essi1-chain set, DSP 0's ESSI1 transmitter feeds
 * DSP 1's receiver. DMA channel 0 on each end moves program-memory words
 * out of one and into the host port of the other (24-bit words, one frame
 * sync per word), so four words cross the link in order.
 */
static void test_dsp_essi_link(void)
{
    Card c = card_start(FW_PROXY, ",x-essi1-chain=on");
    uint32_t tx[0x24] = { 0 }, rx[0x24] = { 0 }, out[8];
    unsigned i;

    i = 0;
    PUT_QQ(tx, i, X_CRA1, 0x180000);
    PUT_PP(tx, i, X_DSR_CH(0), DATA_AT);
    PUT_PP(tx, i, X_DDR_CH(0), X_TX10);
    PUT_PP(tx, i, X_DCO_CH(0), 3);
    PUT_PP(tx, i, X_DCR_CH(0), DCR(DRS_ESSI1_TX, DAM_NONE_INC, SP_X, SP_P));
    PUT_QQ(tx, i, X_CRB1, 0x010000);             /* TE0 */
    tx[i++] = DSP_WAIT;
    tx[DATA_AT - 0x100 + 0] = 0x111111;
    tx[DATA_AT - 0x100 + 1] = 0x222222;
    tx[DATA_AT - 0x100 + 2] = 0x333333;
    tx[DATA_AT - 0x100 + 3] = 0x444444;
    i = 0;
    PUT_QQ(rx, i, X_CRA1, 0x180000);
    PUT_PP(rx, i, X_DSR_CH(0), X_RX1);
    PUT_PP(rx, i, X_DDR_CH(0), X_DTXS);
    PUT_PP(rx, i, X_DCO_CH(0), 3);
    PUT_PP(rx, i, X_DCR_CH(0), DCR(DRS_ESSI1_RX, DAM_NONE_NONE, SP_X, SP_X));
    PUT_QQ(rx, i, X_CRB1, 0x020000);             /* RE */
    rx[i++] = DSP_WAIT;

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_release(&c, 1);
    dsp_boot(&c, 1, 0x100, rx, 8 + DATA_AT - 0x100);
    dsp_boot(&c, 0, 0x100, tx, DATA_AT - 0x100 + 4);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 1, out, 8), ==, 4);
    g_assert_cmphex(out[0], ==, 0x111111);
    g_assert_cmphex(out[1], ==, 0x222222);
    g_assert_cmphex(out[2], ==, 0x333333);
    g_assert_cmphex(out[3], ==, 0x444444);
    card_quit(&c);
}

/* Without the chain nothing drives DSP 1's receiver: no word arrives. */
static void test_dsp_essi_unlinked(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t rx[0x20] = { 0 }, out[4];
    unsigned i = 0;

    PUT_QQ(rx, i, X_CRA1, 0x180000);
    PUT_PP(rx, i, X_DSR_CH(0), X_RX1);
    PUT_PP(rx, i, X_DDR_CH(0), X_DTXS);
    PUT_PP(rx, i, X_DCO_CH(0), 0);
    PUT_PP(rx, i, X_DCR_CH(0), DCR(DRS_ESSI1_RX, DAM_NONE_NONE, SP_X, SP_X));
    PUT_QQ(rx, i, X_CRB1, 0x020000);
    rx[i++] = DSP_WAIT;

    card_boot(&c);
    dsp_release(&c, 1);
    dsp_boot(&c, 1, 0x100, rx, i);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 1, out, 4), ==, 0);
    card_quit(&c);
}

/*
 * DMA from a timer: timer 0 counts the core clock divided by two, compares
 * every 1000 counts (20 us at the default clock) and each compare triggers a
 * DMA word to the host port, so the words arrive one per period, in order.
 */
static void test_dsp_timer_dma(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0x24] = { 0 }, out[8];
    unsigned i = 0;

    PUT_QQ(prog, i, X_TLR0, 0);
    PUT_QQ(prog, i, X_TCPR0, 999);
    PUT_PP(prog, i, X_DSR_CH(0), DATA_AT);
    PUT_PP(prog, i, X_DDR_CH(0), X_DTXS);
    PUT_PP(prog, i, X_DCO_CH(0), 3);
    PUT_PP(prog, i, X_DCR_CH(0),
            DCR(DRS_TIMER0, DAM_NONE_INC, SP_X, SP_P));
    PUT_QQ(prog, i, X_TCSR0, 0x201);             /* TE, TRM */
    prog[i++] = DSP_WAIT;
    prog[DATA_AT - 0x100 + 0] = 0xa1a1a1;
    prog[DATA_AT - 0x100 + 1] = 0xb2b2b2;
    prog[DATA_AT - 0x100 + 2] = 0xc3c3c3;
    prog[DATA_AT - 0x100 + 3] = 0xd4d4d4;

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, DATA_AT - 0x100 + 4);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 8), ==, 4);
    g_assert_cmphex(out[0], ==, 0xa1a1a1);
    g_assert_cmphex(out[1], ==, 0xb2b2b2);
    g_assert_cmphex(out[2], ==, 0xc3c3c3);
    g_assert_cmphex(out[3], ==, 0xd4d4d4);
    card_quit(&c);
}

/*
 * The interrupt-out line: timer 0 in GPIO mode with DIR set drives its pin
 * from the data output bit (TCSR bit 13), which sets DSP n's bit in the
 * glue's interrupt-request byte; clearing the bit, or resetting the DSP,
 * clears it again.
 */
static void test_dsp_interrupt_out(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0x20] = { 0 };
    uint32_t pin_on[] = { MOVEP_IQQ(X_TCSR0), 0x002800, DSP_WAIT };
    unsigned i = 0;

    PUT_QQ(prog, i, X_TCSR0, 0x002800);             /* DIR and DO */
    prog[i++] = JCLR_PP(2, X_DSR);
    prog[i++] = 0x102;
    PUT_QQ(prog, i, X_TCSR0, 0x000800);             /* DIR only */
    prog[i++] = DSP_WAIT;

    card_boot(&c);
    dsp_release(&c, 2);
    dsp_release(&c, 1);
    g_assert_cmphex(rdb(&c, 0x6000000 + 0x28003), ==, 0);
    dsp_boot(&c, 2, 0x100, prog, i);
    dsp_boot(&c, 1, 0x100, pin_on, ARRAY_SIZE(pin_on));
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmphex(rdb(&c, 0x6000000 + 0x28003), ==, 0x06);
    hp_write(&c, 2, HP_DATA, 1);                    /* DSP 2 drops its pin */
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmphex(rdb(&c, 0x6000000 + 0x28003), ==, 0x02);
    /* Holding DSP 1 in reset drops its pin too. */
    wrb(&c, 0x6000000 + GLUE_RESET_BYTE, 0x02);
    g_assert_cmphex(rdb(&c, 0x6000000 + 0x28003), ==, 0x00);
    card_quit(&c);
}

/* The box device, and what DSP 2 gets back from a command word. */
#define BOX_CMD(addr, cmd) ((addr) | ((cmd) << 11))

/*
 * DSP 2 sends a 16-bit command word on ESSI0 the way the page describes
 * (control A 0x100000, control B 0x032130) and a DMA channel moves what the
 * receiver gets into the host port. Returns the number of words back.
 */
static unsigned box_exchange(Card *c, uint32_t cmd, uint32_t *reply)
{
    uint32_t prog[0x20] = { 0 };
    unsigned i = 0;

    PUT_QQ(prog, i, X_CRA0, 0x100000);
    PUT_QQ(prog, i, X_CRB0, 0x032130);
    PUT_PP(prog, i, X_DSR_CH(1), X_RX0);
    PUT_PP(prog, i, X_DDR_CH(1), X_DTXS);
    PUT_PP(prog, i, X_DCO_CH(1), 0);
    PUT_PP(prog, i, X_DCR_CH(1),
            DCR(DRS_ESSI0_RX, DAM_NONE_NONE, SP_X, SP_X));
    PUT_QQ(prog, i, X_TX00, cmd << 8);
    prog[i++] = DSP_WAIT;

    card_boot(c);
    dsp_release(c, 2);
    dsp_boot(c, 2, 0x100, prog, i);
    qtest_clock_step(c->qts, 1000 * 1000);
    return dsp_words(c, 2, reply, 4);
}

/*
 * "What the virtual box answers": command 1 at the box's address returns
 * 288, command 2 returns x-cmd2-reply, command 7 returns zero; another
 * address gets no reply, and without a box nothing answers at all. The
 * values are the model's own choices (the page rates them approximations),
 * so these checks pin the model's behaviour, not hardware.
 */
static void test_box_commands(void)
{
    static const struct {
        const char *name;
        uint32_t cmd;
        const char *extra;
        unsigned count;
        uint32_t reply;
    } cases[] = {
        { "id", BOX_CMD(1, 1), "", 1, 288u << 8 },
        { "version", BOX_CMD(1, 2), "", 1, 0x0100u << 8 },
        { "board", BOX_CMD(1, 7), "", 1, 0 },
        { "other-address", BOX_CMD(2, 1), "", 0, 0 },
        { "address-prop", BOX_CMD(5, 1), "address=5,", 1, 288u << 8 },
        { "reply-prop", BOX_CMD(1, 2), "x-cmd2-reply=0x2a5,", 1, 0x2a5u << 8 },
    };

    for (unsigned k = 0; k < ARRAY_SIZE(cases); k++) {
        g_autofree char *pre = g_strdup_printf(
            "-device sonic-ada8824,id=box,%saudio-channels=2 ",
            cases[k].extra);
        Card c = card_start_with(FW_PROXY, pre, ",box=box");
        uint32_t reply[4] = { 0 };

        g_test_message("box case %s", cases[k].name);
        g_assert_cmpuint(box_exchange(&c, cases[k].cmd, reply), ==,
                         cases[k].count);
        if (cases[k].count) {
            g_assert_cmphex(reply[0], ==, cases[k].reply);
        }
        card_quit(&c);
    }
}

static void test_box_absent(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t reply[4];

    g_assert_cmpuint(box_exchange(&c, BOX_CMD(1, 1), reply), ==, 0);
    card_quit(&c);
}

static uint64_t box_prop(Card *c, const char *name)
{
    QDict *r = qtest_qmp(c->qts, "{ 'execute': 'qom-get', 'arguments': "
                         "{ 'path': '/machine/peripheral/box', "
                         "'property': %s } }", name);
    uint64_t v;

    g_assert_true(qdict_haskey(r, "return"));
    v = qdict_get_int(r, "return");
    qobject_unref(r);
    return v;
}

/*
 * "Audio output": the 24-bit words DSP 2's ESSI0 transmits (DMA channel 0 from
 * program memory, two time slots a frame) reach the audiodev as 24 significant
 * bits of 32-bit samples, one channel per word in transmit order. A wav
 * backend is the capture.
 */
static void test_audio_samples(void)
{
    static const uint32_t words[] = {
        0x7fffff, 0x800000, 0x000001, 0xffffff, 0x123456, 0x654321,
    };
    g_autofree char *dir = g_dir_make_tmp("sonic-hdsp-audio-XXXXXX", NULL);
    g_autofree char *wav = g_strdup_printf("%s/out.wav", dir);
    g_autofree char *pre = g_strdup_printf(
        "-audiodev wav,id=snd0,path=%s,out.frequency=48000,out.channels=2,"
        "out.format=s16 -device sonic-ada8824,id=box,audiodev=snd0 ", wav);
    Card c = card_start_with(FW_PROXY, pre, ",box=box");
    uint32_t prog[0x30] = { 0 };
    g_autofree char *data = NULL;
    gsize len;
    unsigned i = 0;

    PUT_QQ(prog, i, X_CRA0, 0x181000);           /* 24 bit, two slots */
    PUT_PP(prog, i, X_DSR_CH(0), DATA_AT);
    PUT_PP(prog, i, X_DDR_CH(0), X_TX00);
    PUT_PP(prog, i, X_DCO_CH(0), ARRAY_SIZE(words) - 1);
    PUT_PP(prog, i, X_DCR_CH(0),
            DCR(DRS_ESSI0_TX, DAM_NONE_INC, SP_X, SP_P));
    PUT_QQ(prog, i, X_CRB0, 0x012000);           /* TE0, network */
    prog[i++] = DSP_WAIT;
    memcpy(&prog[DATA_AT - 0x100], words, sizeof(words));

    card_boot(&c);
    dsp_release(&c, 2);
    dsp_boot(&c, 2, 0x100, prog, DATA_AT - 0x100 + ARRAY_SIZE(words));
    qtest_clock_step(c.qts, 20 * 1000 * 1000);
    g_test_message("frames-out %" PRIu64, box_prop(&c, "frames-out"));
    g_assert_cmpuint(box_prop(&c, "frames-out"), >=, 3);
    g_assert_cmpuint(box_prop(&c, "frames-dropped"), ==, 0);
    card_quit(&c);

    /*
     * A wav file holds 16 bits at most, so the mixer narrows the card's
     * 32-bit samples: each word's top 16 bits are what lands in the file, as
     * little-endian s16 frames of two channels after the 44-byte header.
     */
    g_assert_true(g_file_get_contents(wav, &data, &len, NULL));
    g_assert_cmpuint(len, >=, 44 + 4 * 3);
    for (unsigned k = 0; k < ARRAY_SIZE(words); k++) {
        int16_t got = lduw_le_p(data + 44 + 2 * k);

        g_assert_cmphex((uint16_t)got, ==, words[k] >> 8);
    }
    unlink(wav);
    g_rmdir(dir);
}

/* A migration of a card with a running DSP: the program goes on after it. */
static void test_migration_dsp(void)
{
    g_autofree char *dir = g_dir_make_tmp("sonic-hdsp-dspmig-XXXXXX", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    Card from = card_start(FW_PROXY, ""), to = { 0 };
    uint32_t out[4];
    const uint32_t prog[] = {
        /* 0x100 */ MOVEP_IPP(X_DTXS), 0xc0ffee,
        /* 0x102 */ JCLR_PP(2, X_DSR), 0x102,
        /* 0x104 */ MOVEP_RD(REG_A1, X_DRXR),
        /* 0x105 */ MOVEP_WR(REG_A1, X_DTXS),
        /* 0x106 */ JMP(0x102),
    };

    card_boot(&from);
    dsp_release(&from, 0);
    dsp_boot(&from, 0, 0x100, prog, ARRAY_SIZE(prog));
    qtest_clock_step(from.qts, 1000 * 1000);

    to.flash = g_strdup(from.flash);
    to.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                         "-device sgi-sonic-hdsp,id=hdsp,bus=pci,addr=%d,"
                         "flash-image=%s -incoming %s", SLOT, from.flash, uri);
    qtest_qmp_assert_success(from.qts, "{ 'execute': 'stop' }");
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    to.bus = qpci_init_sgi_bridge(to.qts, &alloc);
    sgi_migrate_resume(to.qts);
    to.fn0 = qpci_device_find(to.bus, QPCI_DEVFN(SLOT, 0));
    to.fn1 = qpci_device_find(to.bus, QPCI_DEVFN(SLOT, 1));
    g_assert_nonnull(to.fn1);
    to.bar = from.bar;

    /* The announcement waits in the host port; the program still echoes. */
    g_assert_cmpuint(dsp_words(&to, 0, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0xc0ffee);
    hp_write(&to, 0, HP_DATA, 0x424242);
    qtest_clock_step(to.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&to, 0, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0x424242);

    g_free(from.fn0);
    g_free(from.fn1);
    qpci_free_sgi_bridge(from.bus);
    qtest_quit(from.qts);
    g_free(to.fn0);
    g_free(to.fn1);
    qpci_free_sgi_bridge(to.bus);
    qtest_quit(to.qts);
    alloc_destroy(&alloc);
    unlink(from.flash);
    g_free(from.flash);
    g_free(to.flash);
    g_rmdir(dir);
}

/*
 * Slot structure: network mode gives DC + 1 slots a frame, the transmit
 * and receive slot masks pick the active ones, the card's sample-rate sets
 * the frame rate and audio-channels the words in an audio frame. DSP 2
 * transmits zeros out of program memory and receives silence from the box
 * through DMA; the box counts the frames it assembles and the DSP reports
 * how far its receive DMA has advanced. Both are compared with the
 * frame syncs that passed between two reports.
 */
static void test_slot_counts(void)
{
    static const struct {
        const char *name;
        unsigned dc, rate, channels;
        uint32_t tsma, rsma;
        unsigned tx_slots, rx_slots;        /* active per frame sync */
    } cases[] = {
        { "two-slots", 1, 48000, 2, 0xffff, 0xffff, 2, 2 },
        { "tx-mask", 3, 48000, 2, 0x5, 0xffff, 2, 4 },
        { "rx-mask", 3, 48000, 2, 0xf, 0x2, 4, 1 },
        { "rate", 1, 96000, 2, 0xffff, 0xffff, 2, 2 },
        { "channels", 3, 48000, 4, 0xffff, 0xffff, 4, 4 },
    };

    for (unsigned k = 0; k < ARRAY_SIZE(cases); k++) {
        g_autofree char *pre = g_strdup_printf(
            "-device sonic-ada8824,id=box,audio-channels=%u ",
            cases[k].channels);
        g_autofree char *extra = g_strdup_printf(",box=box,sample-rate=%u",
                                                 cases[k].rate);
        Card c = card_start_with(FW_PROXY, pre, extra);
        uint32_t prog[0x40] = { 0 }, out[2];
        unsigned i = 0;
        uint64_t f1, f2;
        int64_t t1, t2;
        double ticks, exp_frames, exp_rx;
        uint32_t d1, d2;

        g_test_message("slot case %s", cases[k].name);
        PUT_QQ(prog, i, X_CRA0, 0x180000 | (cases[k].dc << 12));
        PUT_QQ(prog, i, X_TSMA0, cases[k].tsma);
        PUT_QQ(prog, i, X_RSMA0, cases[k].rsma);
        PUT_PP(prog, i, X_DSR_CH(0), 0x300);
        PUT_PP(prog, i, X_DDR_CH(0), X_TX00);
        PUT_PP(prog, i, X_DCO_CH(0), 0x4ff);
        PUT_PP(prog, i, X_DCR_CH(0),
               DCR(DRS_ESSI0_TX, DAM_NONE_INC, SP_X, SP_P));
        PUT_PP(prog, i, X_DSR_CH(1), X_RX0);
        PUT_PP(prog, i, X_DDR_CH(1), 0x800);
        PUT_PP(prog, i, X_DCO_CH(1), 0x7ff);
        PUT_PP(prog, i, X_DCR_CH(1),
               DCR(DRS_ESSI0_RX, 0x2c, SP_P, SP_X));
        PUT_QQ(prog, i, X_CRB0, 0x032000);          /* TE0, RE, network */
        /* Report the receive DMA's destination address for each host word. */
        prog[i++] = JCLR_PP(2, X_DSR);
        prog[i] = 0x100 + i - 1;
        i++;
        prog[i++] = MOVEP_RD(REG_A1, X_DRXR);
        prog[i++] = MOVEP_RD(REG_A1, X_DDR_CH(1));
        prog[i++] = MOVEP_WR(REG_A1, X_DTXS);
        prog[i] = JMP(0x100 + i - 5);
        i++;

        card_boot(&c);
        dsp_release(&c, 2);
        dsp_boot(&c, 2, 0x100, prog, i);
        qtest_clock_step(c.qts, 1000 * 1000);
        hp_write(&c, 2, HP_DATA, 0);
        qtest_clock_step(c.qts, 300 * 1000);
        g_assert_cmpuint(dsp_words(&c, 2, out, 1), ==, 1);
        d1 = out[0];
        f1 = box_prop(&c, "frames-out");
        t1 = qtest_clock_step(c.qts, 1);
        qtest_clock_step(c.qts, 4 * 1000 * 1000);
        hp_write(&c, 2, HP_DATA, 0);
        qtest_clock_step(c.qts, 300 * 1000);
        g_assert_cmpuint(dsp_words(&c, 2, out, 1), ==, 1);
        d2 = out[0];
        f2 = box_prop(&c, "frames-out");
        t2 = qtest_clock_step(c.qts, 1);

        ticks = (double)(t2 - t1) * cases[k].rate / 1e9;
        exp_frames = ticks * cases[k].tx_slots / cases[k].channels;
        exp_rx = ticks * cases[k].rx_slots;
        g_test_message("ticks %.0f frames %" PRIu64 " (want %.0f) rx %u "
                       "(want %.0f)", ticks, f2 - f1, exp_frames, d2 - d1,
                       exp_rx);
        g_assert_cmpfloat(fabs((double)(f2 - f1) - exp_frames), <=,
                          0.15 * exp_frames + 3);
        g_assert_cmpfloat(fabs((double)(d2 - d1) - exp_rx), <=,
                          0.15 * exp_rx + 3);
        card_quit(&c);
    }
}

/*
 * SSISR: an enabled transmitter with nothing written sets TUE and TDE; a
 * write to the data register, or to TSR, after reading SSISR clears TUE.
 */
static void ssisr_tue(bool use_tsr)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0x20] = { 0 }, out[4];
    unsigned i = 0;

    PUT_QQ(prog, i, X_CRA1, 0x180000);
    PUT_QQ(prog, i, X_CRB1, 0x010000);              /* TE0 */
    prog[i++] = JCLR_PP(2, X_DSR);
    prog[i++] = 0x104;
    prog[i++] = MOVEP_RD(REG_A1, X_DRXR);
    prog[i++] = MOVEQQ_RD(REG_A1, X_SSISR1);
    prog[i++] = MOVEP_WR(REG_A1, X_DTXS);
    if (use_tsr) {
        PUT_QQ(prog, i, X_TSR1, 0);
    } else {
        PUT_QQ(prog, i, X_TX10, 0x123456);
    }
    prog[i++] = MOVEQQ_RD(REG_A1, X_SSISR1);
    prog[i++] = MOVEP_WR(REG_A1, X_DTXS);
    prog[i++] = DSP_WAIT;

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, i);
    qtest_clock_step(c.qts, 1000 * 1000);
    hp_write(&c, 0, HP_DATA, 0);
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 4), ==, 2);
    g_assert_cmphex(out[0] & 0xf0, ==, 0x50);       /* TDE and TUE */
    g_assert_cmphex(out[1] & 0xf0, ==, 0x00);
    card_quit(&c);
}

static void test_ssisr_tue(void)
{
    ssisr_tue(false);
}

static void test_ssisr_tue_tsr(void)
{
    ssisr_tue(true);
}

/*
 * SSISR: a receiver whose word is not read before the next arrives sets
 * ROE with RDF; reading SSISR then the data register clears both.
 */
static void test_ssisr_roe(void)
{
    Card c = card_start(FW_PROXY, ",x-essi1-chain=on");
    uint32_t tx[0x30] = { 0 }, rx[0x30] = { 0 }, out[4];
    unsigned i = 0, j = 0;

    PUT_QQ(tx, i, X_CRA1, 0x180000);
    PUT_PP(tx, i, X_DSR_CH(0), 0x120);
    PUT_PP(tx, i, X_DDR_CH(0), X_TX10);
    PUT_PP(tx, i, X_DCO_CH(0), 0);
    PUT_PP(tx, i, X_DCR_CH(0),
           DCR(DRS_ESSI1_TX, DAM_NONE_INC, SP_X, SP_P));
    PUT_QQ(tx, i, X_CRB1, 0x010000);
    tx[i++] = DSP_WAIT;

    PUT_QQ(rx, j, X_CRA1, 0x180000);
    PUT_QQ(rx, j, X_CRB1, 0x020000);                /* RE, no DMA */
    rx[j++] = JCLR_PP(2, X_DSR);
    rx[j++] = 0x104;
    rx[j++] = MOVEP_RD(REG_A1, X_DRXR);
    rx[j++] = MOVEQQ_RD(REG_A1, X_SSISR1);
    rx[j++] = MOVEP_WR(REG_A1, X_DTXS);
    rx[j++] = MOVEQQ_RD(REG_A1, X_RX1);
    rx[j++] = MOVEQQ_RD(REG_A1, X_SSISR1);
    rx[j++] = MOVEP_WR(REG_A1, X_DTXS);
    rx[j++] = DSP_WAIT;

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_release(&c, 1);
    dsp_boot(&c, 1, 0x100, rx, j);
    dsp_boot(&c, 0, 0x100, tx, 0x24);
    qtest_clock_step(c.qts, 1000 * 1000);
    hp_write(&c, 1, HP_DATA, 0);
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmpuint(dsp_words(&c, 1, out, 4), ==, 2);
    g_assert_cmphex(out[0] & 0xf0, ==, 0xa0);       /* RDF and ROE */
    g_assert_cmphex(out[1] & 0xb0, ==, 0x00);
    card_quit(&c);
}

/*
 * Timer: TOF sets on overflow alongside TCF at the compare, and each is
 * cleared by writing one to it; the compare flag as a DMA request clears
 * when served, so words arrive one compare apart.
 */
static void test_timer_overflow(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0x20] = { 0 }, out[4];
    unsigned i = 0;

    PUT_QQ(prog, i, X_TLR0, 0xfffff0);
    PUT_QQ(prog, i, X_TCPR0, 0xffffff);
    PUT_QQ(prog, i, X_TCSR0, 0x000001);
    prog[i++] = JCLR_PP(2, X_DSR);
    prog[i++] = 0x106;
    prog[i++] = MOVEP_RD(REG_A1, X_DRXR);
    prog[i++] = MOVEQQ_RD(REG_A1, X_TCSR0);
    prog[i++] = MOVEP_WR(REG_A1, X_DTXS);
    PUT_QQ(prog, i, X_TCSR0, 0x100001);             /* clear TOF only */
    prog[i++] = MOVEQQ_RD(REG_A1, X_TCSR0);
    prog[i++] = MOVEP_WR(REG_A1, X_DTXS);
    prog[i++] = DSP_WAIT;

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, i);
    qtest_clock_step(c.qts, 1000 * 1000);
    hp_write(&c, 0, HP_DATA, 0);
    qtest_clock_step(c.qts, 500 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 4), ==, 2);
    g_assert_cmphex(out[0], ==, 0x300001);
    g_assert_cmphex(out[1], ==, 0x200001);
    card_quit(&c);
}

static void test_timer_dma_period(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0x30] = { 0 }, out[8];
    unsigned i = 0;

    PUT_QQ(prog, i, X_TLR0, 0);
    PUT_QQ(prog, i, X_TCPR0, 49999);                /* 1 ms at 100 MHz */
    PUT_PP(prog, i, X_DSR_CH(0), DATA_AT);
    PUT_PP(prog, i, X_DDR_CH(0), X_DTXS);
    PUT_PP(prog, i, X_DCO_CH(0), 3);
    PUT_PP(prog, i, X_DCR_CH(0),
           DCR(DRS_TIMER0, DAM_NONE_INC, SP_X, SP_P));
    PUT_QQ(prog, i, X_TCSR0, 0x201);
    prog[i++] = DSP_WAIT;
    prog[DATA_AT - 0x100 + 0] = 1;
    prog[DATA_AT - 0x100 + 1] = 2;
    prog[DATA_AT - 0x100 + 2] = 3;
    prog[DATA_AT - 0x100 + 3] = 4;

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, DATA_AT - 0x100 + 4);
    qtest_clock_step(c.qts, 1600 * 1000);
    /* A compare a millisecond: not all four are out after 1.6 ms. */
    g_assert_cmpuint(dsp_words(&c, 0, out, 8), <, 4);
    qtest_clock_step(c.qts, 6 * 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 8), >=, 1);
    card_quit(&c);
}

/* IPRP's host port field of zero disables the host command interrupt. */
static void test_dsp_host_command_priority_off(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t prog[0xcc], out[4];
    unsigned len = host_command_program(prog, true, 0);

    card_boot(&c);
    dsp_release(&c, 1);
    dsp_boot(&c, 1, 0x40, prog, len);
    qtest_clock_step(c.qts, 1000 * 1000);
    hp_write(&c, 1, HP_HCVR, 0xf5);
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 1, out, 4), ==, 0);
    g_assert_cmphex(hp_read(&c, 1, HP_HCVR) & 1, ==, 1);
    card_quit(&c);
}

/*
 * A count of zero starts the program at the address without loading: the
 * next word the host writes is for the program, so it stays queued.
 */
static void test_dsp_boot_zero_count(void)
{
    Card c = card_start(FW_PROXY, "");
    Proxy *p = g_new0(Proxy, 1);

    card_boot(&c);
    dsp_release(&c, 0);
    px_hp_write(p, 0, HP_HCTR, 0x900);
    px_hp_write(p, 0, HP_DATA, 0);
    px_hp_write(p, 0, HP_DATA, 0x100);
    px_run(&c, p);
    hp_write(&c, 0, HP_DATA, 0x123456);
    g_assert_cmphex(hp_read(&c, 0, HP_HSTR) & HSTR_TRDY, ==, 0);
    g_free(p);
    card_quit(&c);
}

/*
 * A system reset puts every DSP back in reset, whatever it was running,
 * and the card comes back: the DSP can be released and loaded again.
 */
static void test_system_reset_dsp(void)
{
    Card c = card_start(FW_PROXY, "");
    uint32_t out[4];
    const uint32_t prog[] = {
        /* 0x100 */ MOVEP_IPP(X_DTXS), 0xc0ffee,
        /* 0x102 */ JCLR_PP(2, X_DSR), 0x102,
        /* 0x104 */ MOVEP_RD(REG_A1, X_DRXR),
        /* 0x105 */ MOVEP_WR(REG_A1, X_DTXS),
        /* 0x106 */ JMP(0x102),
    };

    card_boot(&c);
    dsp_release(&c, 0);
    dsp_boot(&c, 0, 0x100, prog, ARRAY_SIZE(prog));
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 4), ==, 1);

    qtest_system_reset(c.qts);
    g_free(c.fn0);
    g_free(c.fn1);
    card_boot(&c);
    g_assert_cmphex(rdb(&c, 0x6000000 + GLUE_RESET_BYTE), ==, 0x0f);
    g_assert_cmphex(message(&c, 1), ==, 2);
    g_assert_cmphex(rd(&c, SCR(S_DSP0_ID)), ==, 0xffffffff);
    dsp_release(&c, 0);
    g_assert_cmphex(message(&c, 3), ==, 4);
    g_assert_cmphex(rd(&c, SCR(S_DSP0_ID)), ==, 0x18011057);
    dsp_boot(&c, 0, 0x100, prog, ARRAY_SIZE(prog));
    qtest_clock_step(c.qts, 1000 * 1000);
    g_assert_cmpuint(dsp_words(&c, 0, out, 4), ==, 1);
    g_assert_cmphex(out[0], ==, 0xc0ffee);
    card_quit(&c);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-sonic-hdsp/retry-until-cleared",
                   test_retry_until_cleared);
    qtest_add_func("/sgi-sonic-hdsp/identity", test_identity);
    qtest_add_func("/sgi-sonic-hdsp/translate-rule", test_translate_rule);
    qtest_add_func("/sgi-sonic-hdsp/message-handshake",
                   test_message_handshake);
    qtest_add_func("/sgi-sonic-hdsp/queue-ports", test_queue_ports);
    qtest_add_func("/sgi-sonic-hdsp/glue-and-uart", test_glue_and_uart);
    qtest_add_func("/sgi-sonic-hdsp/dram-size", test_dram_size);
    qtest_add_func("/sgi-sonic-hdsp/flash-program", test_flash_program);
    qtest_add_func("/sgi-sonic-hdsp/dsp-function-reset",
                   test_dsp_function_reset);
    qtest_add_func("/sgi-sonic-hdsp/flash-refused", test_flash_refused);
    qtest_add_func("/sgi-sonic-hdsp/type1-scan", test_type1_scan);
    qtest_add_func("/sgi-sonic-hdsp/eeprom-read", test_eeprom_read);
    qtest_add_func("/sgi-sonic-hdsp/dma-loop-bounded", test_dma_loop_bounded);
    qtest_add_func("/sgi-sonic-hdsp/dma-big-loop", test_dma_big_loop);
    qtest_add_func("/sgi-sonic-hdsp/migration", test_migration);
    qtest_add_func("/sgi-sonic-hdsp/migration-dirty-dram",
                   test_migration_dirty_dram);
    qtest_add_func("/sgi-sonic-hdsp/dsp/runs-program", test_dsp_runs_program);
    qtest_add_func("/sgi-sonic-hdsp/dsp/boot-hf0", test_dsp_boot_hf0);
    qtest_add_func("/sgi-sonic-hdsp/dsp/host-command", test_dsp_host_command);
    qtest_add_func("/sgi-sonic-hdsp/dsp/host-command-masked",
                   test_dsp_host_command_masked);
    qtest_add_func("/sgi-sonic-hdsp/dsp/hi32-flags", test_dsp_hi32_flags);
    qtest_add_func("/sgi-sonic-hdsp/dsp/essi-link", test_dsp_essi_link);
    qtest_add_func("/sgi-sonic-hdsp/dsp/essi-unlinked", test_dsp_essi_unlinked);
    qtest_add_func("/sgi-sonic-hdsp/dsp/timer-dma", test_dsp_timer_dma);
    qtest_add_func("/sgi-sonic-hdsp/dsp/interrupt-out",
                   test_dsp_interrupt_out);
    qtest_add_func("/sgi-sonic-hdsp/box/commands", test_box_commands);
    qtest_add_func("/sgi-sonic-hdsp/box/absent", test_box_absent);
    qtest_add_func("/sgi-sonic-hdsp/box/audio-samples", test_audio_samples);
    qtest_add_func("/sgi-sonic-hdsp/migration-dsp", test_migration_dsp);
    qtest_add_func("/sgi-sonic-hdsp/dsp/slot-counts", test_slot_counts);
    qtest_add_func("/sgi-sonic-hdsp/dsp/ssisr-tue", test_ssisr_tue);
    qtest_add_func("/sgi-sonic-hdsp/dsp/ssisr-tue-tsr", test_ssisr_tue_tsr);
    qtest_add_func("/sgi-sonic-hdsp/dsp/ssisr-roe", test_ssisr_roe);
    qtest_add_func("/sgi-sonic-hdsp/dsp/timer-overflow", test_timer_overflow);
    qtest_add_func("/sgi-sonic-hdsp/dsp/timer-dma-period",
                   test_timer_dma_period);
    qtest_add_func("/sgi-sonic-hdsp/dsp/host-command-priority-off",
                   test_dsp_host_command_priority_off);
    qtest_add_func("/sgi-sonic-hdsp/dsp/boot-zero-count",
                   test_dsp_boot_zero_count);
    qtest_add_func("/sgi-sonic-hdsp/system-reset-dsp", test_system_reset_dsp);

    return g_test_run();
}
