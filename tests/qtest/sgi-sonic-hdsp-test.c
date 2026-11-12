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

static Card card_start(unsigned flags, const char *extra)
{
    Image img = { flags, SISR_BOOT_ROM };
    Card c = { 0 };

    c.flash = flash_file(&img);
    qpci_sgi_bridge_alloc_init(&alloc, 0);
    c.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
                        "-chardev ringbuf,id=con,size=4096 "
                        "-device sgi-sonic-hdsp,id=hdsp,bus=pci,addr=%d,"
                        "flash-image=%s,iop-console=con%s",
                        SLOT, c.flash, extra);
    c.bus = qpci_init_sgi_bridge(c.qts, &alloc);
    return c;
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

    return g_test_run();
}
