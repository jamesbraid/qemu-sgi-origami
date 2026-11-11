/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the Adaptec AIC-1160 Fibre Channel controller: PCI identity
 * and windows, the register banks and chip reset, the sequencer download,
 * the serial SEEPROM, the interrupt line and status block, and SCSI
 * commands to loop disks through the TCB ring and done queue.
 *
 * The register offsets, bank selects, ring setup and layouts come from the
 * driver behaviour described in docs/system/mips/sgi/hardware/
 * adaptec-aic1160.rst. No chip programming manual is public, so a test
 * marked "approximation" pins a model choice, not a measured hardware
 * fact. The controller sits behind the Bridge on the libqos head; its
 * interrupt is observed on the Bridge's pin status register.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"

#define AIC_VENDOR_ID       0x9004
#define AIC_DEVICE_ID       0x1160
#define AIC_WINDOW_SIZE     0x100
#define AIC_QOM_PATH        "/machine/peripheral/fc0"
#define AIC_SLOT            4

#define BAR_REGS            0
#define BAR_MEM             1
#define BAR_SEQ_MEM         3

#define REG_SEQ_CTL         0x00
#define REG_SEQ_ADDR        0x02
#define REG_SEQ_DATA        0x14
#define REG_HOST_CTL        0x22
#define REG_INT_STATUS      0x24
#define REG_INT_STATUS_HI   0x25
#define REG_COMPLETE_CTL    0x29
#define REG_DOORBELL        0x2a
#define REG_BANK            0x2c
#define REG_SCRATCH_PAGE    0x34
#define REG_SEEPROM_DATA    0x68
#define REG_SEEPROM_ADDR    0x6a
#define REG_SEEPROM_CMD     0x6b
#define REG_TCB_CONSUMER    0x6a
#define REG_STATUS_BLOCK    0x70
#define REG_LOOP_STATE      0xa1

#define BANK_0              0x00
#define BANK_1              0x11
#define BANK_4              0x44

#define SEQ_LOAD            0x01
#define SEQ_RUN             0x40

#define HOST_RESET          0x43
#define HOST_IRQ_EN         0x04
#define HOST_PAUSE          0x40
#define HOST_PAUSE_TOP      0x80

#define COMPLETE_ARM        0x20

#define INT_LOOP            0x0008
#define INT_LOW_IGNORED     0x0001
#define INT_COMPLETE        0x1000
#define INT_PCI_ERROR       0x8000

#define LOOP_UP             0x20

/* Scratch RAM offsets of the ring setup, in page 0. */
#define SCR_DONE_BASE       0x08
#define SCR_TCB_BASE        0x1c
#define SCR_PASS            0x45
#define SCR_DONE_NEG        0x48

#define SEEPROM_READ        0x61

/* The Bridge's live PCI pin status; the controller is on pin AIC_SLOT. */
#define BRIDGE_INT_STATUS   0x40000104ULL

#define TCB_SIZE            128
#define RSP_SIZE            256
#define MODE_FLAT           0x08
#define MODE_SG             0x0b
#define MODE_NONE           0x0c
#define DISK_ALPA           0xe4        /* Loop ID 2. */
#define DISK_ID             2
#define DISK_ALPA0          0xef        /* Loop ID 0. */
#define DISK_PORT_WWN       UINT64_C(0x2100000000000002)
#define DISK_NODE_WWN       UINT64_C(0x2000000000000002)

#define DONE_OK             0x00
#define DONE_ERROR_RSP      0x04
#define DONE_FORMAT         0x06
#define DONE_NO_DEVICE      0x08
#define DISK_SIZE           (1024 * 1024)
#define WWN                 UINT64_C(0x2100001122334455)

typedef struct QAic1160 {
    QOSGraphObject obj;
    QPCIDevice dev;
} QAic1160;

typedef struct Aic {
    QPCIDevice *dev;
    QTestState *qts;
    QGuestAllocator *alloc;
    QPCIBar regs;
    QPCIBar seq;
    /* The rings the host built. */
    uint64_t tcbs;
    uint64_t done;
    uint64_t status_block;
    unsigned count;
    unsigned produced;
    /* Ring slot of index 0: the driver restarts its indices after a LIP. */
    unsigned origin;
    unsigned consumed;
    uint8_t expected_pass;
} Aic;

typedef struct Seg {
    uint64_t addr;
    uint32_t len;
} Seg;

typedef struct Done {
    uint16_t handle;
    uint8_t status;
    uint8_t pass;
} Done;

static char *disk_path;

static void *aic_get_driver(void *obj, const char *interface)
{
    QAic1160 *aic = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &aic->dev;
    }
    fprintf(stderr, "%s not present in aic1160\n", interface);
    g_assert_not_reached();
}

static void *aic_create(void *pci_bus, QGuestAllocator *alloc, void *addr)
{
    QAic1160 *aic = g_new0(QAic1160, 1);

    qpci_device_init(&aic->dev, pci_bus, addr);
    aic->obj.get_driver = aic_get_driver;
    return &aic->obj;
}

static void aic_open(Aic *t, void *obj, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;

    memset(t, 0, sizeof(*t));
    t->dev = &aic->dev;
    t->qts = t->dev->bus->qts;
    t->alloc = alloc;
    qpci_device_enable(t->dev);
    t->regs = qpci_iomap(t->dev, BAR_REGS, NULL);
    t->seq = qpci_iomap(t->dev, BAR_SEQ_MEM, NULL);
}

static void aic_close(Aic *t)
{
    qpci_iounmap(t->dev, t->regs);
    qpci_iounmap(t->dev, t->seq);
}

static uint8_t rd8(Aic *t, uint8_t reg)
{
    return qpci_io_readb(t->dev, t->regs, reg);
}

static uint16_t rd16(Aic *t, uint8_t reg)
{
    return qpci_io_readw(t->dev, t->regs, reg);
}

static void wr8(Aic *t, uint8_t reg, uint8_t val)
{
    qpci_io_writeb(t->dev, t->regs, reg, val);
}

static void wr16(Aic *t, uint8_t reg, uint16_t val)
{
    qpci_io_writew(t->dev, t->regs, reg, val);
}

static uint8_t seq_rd8(Aic *t, uint8_t off)
{
    return qpci_io_readb(t->dev, t->seq, off);
}

static void seq_wr8(Aic *t, uint8_t off, uint8_t val)
{
    qpci_io_writeb(t->dev, t->seq, off, val);
}

static void seq_wr16(Aic *t, uint8_t off, uint16_t val)
{
    qpci_io_writew(t->dev, t->seq, off, val);
}

/* An 8-byte address, a halfword at a time as the window takes it. */
static void seq_wr64(Aic *t, uint8_t off, uint64_t val)
{
    for (int i = 0; i < 4; i++) {
        seq_wr16(t, off + 2 * i, val >> (16 * i));
    }
}

static uint64_t aic_prop(Aic *t, const char *name)
{
    QDict *rsp = qtest_qmp(t->qts,
                           "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': %s, 'property': %s } }",
                           AIC_QOM_PATH, name);
    uint64_t val;

    g_assert(qdict_haskey(rsp, "return"));
    val = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return val;
}

static bool aic_running(Aic *t)
{
    QDict *rsp = qtest_qmp(t->qts,
                           "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': %s, 'property': "
                           "'x-sequencer-running' } }", AIC_QOM_PATH);
    bool val;

    g_assert(qdict_haskey(rsp, "return"));
    val = qdict_get_bool(rsp, "return");
    qobject_unref(rsp);
    return val;
}

static void aic_set(Aic *t, const char *name, unsigned val)
{
    QDict *rsp = qtest_qmp(t->qts,
                           "{ 'execute': 'qom-set', 'arguments': {"
                           " 'path': %s, 'property': %s,"
                           " 'value': %u } }", AIC_QOM_PATH, name, val);

    g_assert(!qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
}

static bool bridge_pin(Aic *t)
{
    return (qtest_readl(t->qts, BRIDGE_INT_STATUS) & (1u << AIC_SLOT)) != 0;
}

static void test_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;
    QPCIDevice *dev = &aic->dev;

    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, AIC_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, AIC_DEVICE_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==, 0x0c04);
    /* The revision defaults to 2, the one owners report most. */
    g_assert_cmphex(qpci_config_readb(dev, PCI_REVISION_ID), ==, 2);
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_PIN), ==, 1);
}

static void test_revision_property(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;

    g_assert_cmphex(qpci_config_readb(&aic->dev, PCI_REVISION_ID), ==, 1);
}

/*
 * BAR 0 and BAR 3 are 256-byte I/O windows. BAR 1 is a 4 KiB 64-bit
 * non-prefetchable memory BAR with BAR 2 as its upper dword; BARs 4 and 5 do
 * not answer. The expansion ROM is 64 KiB.
 */
static void test_windows(void *obj, void *data, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;
    QPCIDevice *dev = &aic->dev;
    uint32_t bar[6];

    for (int i = 0; i < 6; i++) {
        qpci_config_writel(dev, PCI_BASE_ADDRESS_0 + 4 * i, 0xffffffff);
        bar[i] = qpci_config_readl(dev, PCI_BASE_ADDRESS_0 + 4 * i);
    }
    g_assert_cmphex(bar[BAR_REGS], ==, 0xffffff01);
    g_assert_cmphex(bar[BAR_MEM], ==, 0xfffff004);
    g_assert_cmphex(bar[BAR_MEM + 1], ==, 0xffffffff);
    g_assert_cmphex(bar[BAR_SEQ_MEM], ==, 0xffffff01);
    g_assert_cmphex(bar[4], ==, 0);
    g_assert_cmphex(bar[5], ==, 0);

    qpci_config_writel(dev, PCI_ROM_ADDRESS, 0xfffffffe);
    g_assert_cmphex(qpci_config_readl(dev, PCI_ROM_ADDRESS), ==, 0xffff0000);
}

static char *rom_path;

/* A romfile replaces the placeholder: the core sizes the ROM from the file. */
static void test_romfile(void *obj, void *data, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;

    qpci_config_writel(&aic->dev, PCI_ROM_ADDRESS, 0xfffffffe);
    g_assert_cmphex(qpci_config_readl(&aic->dev, PCI_ROM_ADDRESS), ==,
                    0xfffff000);
    unlink(rom_path);
    g_free(rom_path);
}

/* rombar=0 hides the ROM BAR. */
static void test_rombar0(void *obj, void *data, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;

    qpci_config_writel(&aic->dev, PCI_ROM_ADDRESS, 0xfffffffe);
    g_assert_cmphex(qpci_config_readl(&aic->dev, PCI_ROM_ADDRESS), ==, 0);
}

/* The attach-time config writes: cache line, latency, DAC enable. */
static void test_config_space(void *obj, void *data, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;
    QPCIDevice *dev = &aic->dev;

    g_assert_cmphex(qpci_config_readb(dev, 0x40), ==, 0);
    qpci_config_writeb(dev, 0x40, 0x04);
    g_assert_cmphex(qpci_config_readb(dev, 0x40), ==, 0x04);
    qpci_config_writeb(dev, PCI_CACHE_LINE_SIZE, 32);
    qpci_config_writeb(dev, PCI_LATENCY_TIMER, 0xf0);
    g_assert_cmphex(qpci_config_readb(dev, PCI_CACHE_LINE_SIZE), ==, 32);
    g_assert_cmphex(qpci_config_readb(dev, PCI_LATENCY_TIMER), ==, 0xf0);
    /* The driver's zero writes to BARs 1 and 2 leave the type bits. */
    qpci_config_writel(dev, PCI_BASE_ADDRESS_1, 0);
    qpci_config_writel(dev, PCI_BASE_ADDRESS_2, 0);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_1), ==,
                    PCI_BASE_ADDRESS_MEM_TYPE_64);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_2), ==, 0);
}

/*
 * Config collection, the first traffic an attach makes: the link speed the
 * driver accepts, and the TCB SRAM probe pattern echoed by BAR 3.
 */
static void test_config_collection(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    static const uint8_t pattern[] = { 0x66, 0xdd, 0x55, 0xaa };
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_BANK, BANK_4);
    wr8(&t, 0x62, 0x81);
    wr8(&t, REG_BANK, BANK_4);
    /* The driver takes (value & 0xfc) | 0x02 and accepts 0xbe at most. */
    g_assert_cmphex((rd8(&t, 0x63) & 0xfc) | 0x02, ==, 0xbe);
    /* The register is chip state: a reset puts the speed back. */
    wr8(&t, 0x63, 0);
    wr8(&t, REG_BANK, BANK_0);
    wr8(&t, REG_HOST_CTL, HOST_RESET);
    wr8(&t, REG_BANK, BANK_4);
    g_assert_cmphex(rd8(&t, 0x63), ==, 0xbe);

    /* The TCB SRAM probe: an index, then a pattern echoed. */
    wr16(&t, 0x64, 0x01ff);
    g_assert_cmphex(rd16(&t, 0x64), ==, 0x01ff);
    for (unsigned off = 0x80; off <= 0xfc; off += 0x7c) {
        for (int i = 0; i < 4; i++) {
            qpci_io_writeb(t.dev, t.seq, off + i, pattern[i]);
        }
        for (int i = 0; i < 4; i++) {
            g_assert_cmphex(qpci_io_readb(t.dev, t.seq, off + i), ==,
                            pattern[i]);
        }
    }
    aic_close(&t);
}

static void test_reset_values(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    g_assert_cmphex(rd8(&t, REG_SEQ_CTL), ==, 0);
    g_assert_cmphex(rd16(&t, REG_SEQ_ADDR), ==, 0);
    g_assert_cmphex(rd8(&t, REG_HOST_CTL), ==, 0);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    g_assert_cmphex(rd16(&t, REG_DOORBELL), ==, 0);
    g_assert_cmphex(rd8(&t, REG_BANK), ==, BANK_0);
    g_assert_false(aic_running(&t));
    g_assert_false(bridge_pin(&t));
    aic_close(&t);
}

/* The bank select at 0x2c picks which register set the cells belong to. */
static void test_banks(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);

    wr8(&t, REG_BANK, BANK_4);
    g_assert_cmphex(rd8(&t, REG_BANK), ==, BANK_4);
    wr8(&t, REG_COMPLETE_CTL, COMPLETE_ARM);
    g_assert_cmphex(rd8(&t, REG_COMPLETE_CTL), ==, COMPLETE_ARM);

    /* The same offset in another bank is another cell. */
    wr8(&t, REG_BANK, BANK_1);
    g_assert_cmphex(rd8(&t, REG_COMPLETE_CTL), ==, 0);
    wr8(&t, REG_LOOP_STATE, 0x55);
    g_assert_cmphex(rd8(&t, REG_LOOP_STATE), ==, 0x55);
    wr8(&t, REG_BANK, BANK_4);
    g_assert_cmphex(rd8(&t, REG_COMPLETE_CTL), ==, COMPLETE_ARM);
    g_assert_cmphex(rd8(&t, REG_LOOP_STATE), ==, 0);

    /* Bank 4's 0x6a is the TCB consumer; bank 0's is the SEEPROM address. */
    wr16(&t, REG_TCB_CONSUMER, 0x0123);
    g_assert_cmphex(rd16(&t, REG_TCB_CONSUMER), ==, 0x0123);
    wr8(&t, REG_BANK, BANK_0);
    g_assert_cmphex(rd8(&t, REG_SEEPROM_ADDR), ==, 0);

    /* A value that is not a bank select is refused and changes nothing. */
    wr8(&t, REG_BANK, BANK_1);
    wr8(&t, REG_BANK, 0x45);
    g_assert_cmphex(rd8(&t, REG_BANK), ==, BANK_1);
    wr8(&t, REG_BANK, 0x88);
    g_assert_cmphex(rd8(&t, REG_BANK), ==, BANK_1);
    aic_close(&t);
}

/* Register 0x22 bit 0 resets; the bank select is not a reset. */
static void test_chip_reset(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(&t, REG_SEQ_ADDR, 0);
    wr16(&t, REG_SEQ_DATA, 0xbeef);
    wr8(&t, REG_SEQ_CTL, SEQ_RUN);
    seq_wr8(&t, SCR_PASS, 0xff);
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);
    wr8(&t, REG_BANK, BANK_4);
    wr8(&t, REG_COMPLETE_CTL, COMPLETE_ARM);
    aic_set(&t, "x-raise-status", INT_COMPLETE);
    g_assert_true(bridge_pin(&t));
    g_assert_true(aic_running(&t));

    /* Selecting banks and pausing change none of that. */
    wr8(&t, REG_BANK, BANK_1);
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN | HOST_PAUSE);
    g_assert_true(aic_running(&t));
    g_assert_cmphex(rd8(&t, REG_HOST_CTL), ==, HOST_IRQ_EN | HOST_PAUSE);

    wr8(&t, REG_HOST_CTL, HOST_RESET);
    g_assert_cmphex(rd8(&t, REG_SEQ_CTL), ==, 0);
    g_assert_cmphex(rd16(&t, REG_SEQ_ADDR), ==, 0);
    g_assert_cmphex(rd8(&t, REG_HOST_CTL), ==, 0);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    g_assert_cmphex(rd8(&t, REG_BANK), ==, BANK_0);
    g_assert_false(aic_running(&t));
    g_assert_false(bridge_pin(&t));
    wr8(&t, REG_BANK, BANK_4);
    g_assert_cmphex(rd8(&t, REG_COMPLETE_CTL), ==, 0);
    /* The microcode and the scratch variables are gone. */
    wr16(&t, REG_SEQ_ADDR, 0);
    g_assert_cmphex(rd16(&t, REG_SEQ_DATA), ==, 0);
    g_assert_cmphex(seq_rd8(&t, SCR_PASS), ==, 0);

    /* A fresh download works. */
    wr8(&t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(&t, REG_SEQ_ADDR, 0);
    wr16(&t, REG_SEQ_DATA, 0x0102);
    wr16(&t, REG_SEQ_ADDR, 0);
    g_assert_cmphex(rd16(&t, REG_SEQ_DATA), ==, 0x0102);
    aic_close(&t);
}

/* Pause and pause-at-top read back as the driver's acknowledge poll. */
static void test_host_control(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_HOST_CTL, HOST_PAUSE);
    g_assert_cmphex(rd8(&t, REG_HOST_CTL) & HOST_PAUSE, ==, HOST_PAUSE);
    wr8(&t, REG_HOST_CTL, HOST_PAUSE_TOP);
    g_assert_cmphex(rd8(&t, REG_HOST_CTL), ==, HOST_PAUSE_TOP);
    wr8(&t, REG_HOST_CTL, 0);
    g_assert_cmphex(rd8(&t, REG_HOST_CTL), ==, 0);
    aic_close(&t);
}

/* The word-by-word microcode download and the driver's verification. */
static void test_microcode_load(void *obj, void *data, QGuestAllocator *alloc)
{
    static const uint16_t image[] = {
        0x1234, 0xabcd, 0x0001, 0xffff, 0x8000, 0x7fff, 0x5aa5, 0x0000,
    };
    Aic t;

    aic_open(&t, obj, alloc);

    wr8(&t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(&t, REG_SEQ_ADDR, 0);
    for (int i = 0; i < ARRAY_SIZE(image); i++) {
        wr16(&t, REG_SEQ_DATA, image[i]);
    }
    /* The address pointer advances once per data write. */
    g_assert_cmphex(rd16(&t, REG_SEQ_ADDR), ==, ARRAY_SIZE(image));
    g_assert_cmpuint(aic_prop(&t, "x-words-loaded"), ==, ARRAY_SIZE(image));

    wr16(&t, REG_SEQ_ADDR, 0);
    for (int i = 0; i < ARRAY_SIZE(image); i++) {
        g_assert_cmphex(rd16(&t, REG_SEQ_DATA), ==, image[i]);
    }
    /* ... and once per data read. */
    g_assert_cmphex(rd16(&t, REG_SEQ_ADDR), ==, ARRAY_SIZE(image));
    g_assert_false(aic_running(&t));

    wr8(&t, REG_SEQ_CTL, SEQ_RUN);
    g_assert_cmphex(rd8(&t, REG_SEQ_CTL), ==, SEQ_RUN);
    g_assert_true(aic_running(&t));
    /* Run plus restart at the entry point, as the driver writes it. */
    wr8(&t, REG_SEQ_CTL, SEQ_RUN | 0x02);
    g_assert_cmphex(rd8(&t, REG_SEQ_CTL), ==, SEQ_RUN | 0x02);
    g_assert_true(aic_running(&t));

    wr16(&t, REG_SEQ_ADDR, 3);
    g_assert_cmphex(rd16(&t, REG_SEQ_DATA), ==, image[3]);
    aic_close(&t);
}

/* The RAM is 4096 bytes: the mainline image fits and the word past it not. */
static void test_microcode_capacity(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    const unsigned words = 4096 / 2;
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(&t, REG_SEQ_ADDR, 0);
    for (unsigned i = 0; i < words; i++) {
        wr16(&t, REG_SEQ_DATA, i * 7 + 1);
    }
    wr16(&t, REG_SEQ_DATA, 0xdead);
    wr16(&t, REG_SEQ_ADDR, 0);
    for (unsigned i = 0; i < words; i++) {
        g_assert_cmphex(rd16(&t, REG_SEQ_DATA), ==, (uint16_t)(i * 7 + 1));
    }
    g_assert_cmphex(rd16(&t, REG_SEQ_DATA), ==, 0);
    aic_close(&t);
}

/* The sequencer memory window: scratch pages chosen by register 0x34. */
static void test_sequencer_memory(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_SCRATCH_PAGE, 0);
    seq_wr8(&t, SCR_PASS, 0xff);
    g_assert_cmphex(seq_rd8(&t, SCR_PASS), ==, 0xff);

    wr8(&t, REG_SCRATCH_PAGE, 1);
    g_assert_cmphex(seq_rd8(&t, SCR_PASS), ==, 0);
    seq_wr8(&t, SCR_PASS, 0x11);
    wr8(&t, REG_SCRATCH_PAGE, 0);
    g_assert_cmphex(seq_rd8(&t, SCR_PASS), ==, 0xff);
    wr8(&t, REG_SCRATCH_PAGE, 1);
    g_assert_cmphex(seq_rd8(&t, SCR_PASS), ==, 0x11);

    /* The RAM test: patterns across a page at offsets 0 to 0x7e. */
    wr8(&t, REG_BANK, BANK_4);
    wr8(&t, 0x62, 0x03);        /* Size select; stores. */
    g_assert_cmphex(rd8(&t, 0x62), ==, 0x03);
    wr8(&t, REG_BANK, BANK_0);
    for (int page = 0; page < 4; page++) {
        wr8(&t, REG_SCRATCH_PAGE, page);
        for (unsigned off = 0; off <= 0x7e; off += 2) {
            qpci_io_writew(t.dev, t.seq, off, off % 4 ? 0x55aa : 0xa5a5);
        }
        for (unsigned off = 0; off <= 0x7e; off += 2) {
            g_assert_cmphex(qpci_io_readw(t.dev, t.seq, off), ==,
                            off % 4 ? 0x55aa : 0xa5a5);
        }
    }
    aic_close(&t);
}

/* The line needs the master enable in register 0x22 bit 2. */
static void test_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);

    aic_set(&t, "x-raise-status", INT_LOOP);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, INT_LOOP);
    g_assert_false(bridge_pin(&t));
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);
    g_assert_true(bridge_pin(&t));
    wr8(&t, REG_HOST_CTL, 0);
    g_assert_false(bridge_pin(&t));
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);

    /* The handled bit is written back as a byte write and clears. */
    wr8(&t, REG_INT_STATUS, INT_LOOP);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    g_assert_false(bridge_pin(&t));

    /* A high-byte event raises it too, and clears by byte 0x25. */
    aic_set(&t, "x-raise-status", INT_PCI_ERROR);
    g_assert_cmphex(rd8(&t, REG_INT_STATUS_HI), ==, 0x80);
    g_assert_true(bridge_pin(&t));
    wr8(&t, REG_INT_STATUS_HI, 0x80);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    g_assert_false(bridge_pin(&t));
    aic_close(&t);
}

/* Only bit 3 and the high byte count: other low-byte bits are not sources. */
static void test_interrupt_mask(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);
    aic_set(&t, "x-raise-status", INT_LOW_IGNORED);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, INT_LOW_IGNORED);
    g_assert_false(bridge_pin(&t));

    aic_set(&t, "x-raise-status", INT_LOOP);
    g_assert_true(bridge_pin(&t));
    wr8(&t, REG_INT_STATUS, INT_LOOP);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, INT_LOW_IGNORED);
    g_assert_false(bridge_pin(&t));
    aic_close(&t);
}

/* A loop event: the loop-state register and the attention interrupt. */
static void test_loop_event(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);
    aic_set(&t, "x-loop-event", LOOP_UP);
    g_assert_true(bridge_pin(&t));
    g_assert_cmphex(rd8(&t, REG_INT_STATUS), ==, INT_LOOP);
    wr8(&t, REG_BANK, BANK_1);
    g_assert_cmphex(rd8(&t, REG_LOOP_STATE), ==, LOOP_UP);
    wr8(&t, REG_BANK, BANK_0);
    wr8(&t, REG_INT_STATUS, INT_LOOP);
    g_assert_false(bridge_pin(&t));
    aic_close(&t);
}

/* The status block gets the status bytes when posting is armed. */
static void test_status_block(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t block[8], back[8];
    uint64_t addr = guest_alloc(alloc, 8);
    Aic t;

    aic_open(&t, obj, alloc);
    memset(block, 0xee, sizeof(block));
    qtest_memwrite(t.qts, addr, block, sizeof(block));
    wr8(&t, REG_BANK, BANK_4);
    for (int i = 0; i < 8; i++) {
        wr8(&t, REG_STATUS_BLOCK + i, addr >> (8 * i));
    }

    /* Not armed yet: nothing is written. */
    aic_set(&t, "x-raise-status", INT_LOOP);
    qtest_memread(t.qts, addr, back, sizeof(back));
    g_assert_cmpmem(back, 8, block, 8);
    wr8(&t, REG_INT_STATUS, INT_LOOP);

    wr8(&t, REG_COMPLETE_CTL, COMPLETE_ARM);
    aic_set(&t, "x-raise-status", INT_LOOP | INT_COMPLETE);
    qtest_memread(t.qts, addr, back, sizeof(back));
    g_assert_cmphex(back[0], ==, INT_LOOP);
    g_assert_cmphex(back[1], ==, INT_COMPLETE >> 8);
    g_assert_cmphex(back[2], ==, 0xee);
    aic_close(&t);
}

static uint16_t seeprom_word(Aic *t, uint8_t word, uint8_t cmd)
{
    wr8(t, REG_SEEPROM_ADDR, word);
    wr8(t, REG_SEEPROM_CMD, cmd);
    g_assert_cmphex(rd8(t, REG_SEEPROM_CMD) & 1, ==, 0);
    return rd8(t, REG_SEEPROM_DATA) | (rd8(t, REG_SEEPROM_DATA + 1) << 8);
}

/* The WWN is image words 3 to 6, most significant word first. */
static void test_seeprom_wwn(void *obj, void *data, QGuestAllocator *alloc)
{
    uint64_t wwn = 0;
    Aic t;

    aic_open(&t, obj, alloc);
    for (int i = 0; i < 4; i++) {
        /* Most significant word first. */
        wwn = (wwn << 16) | seeprom_word(&t, 3 + i, SEEPROM_READ);
    }
    g_assert_cmphex(wwn, ==, WWN);
    aic_close(&t);
}

/* With no wwn property the port still has a valid, non-zero name. */
static void test_seeprom_default_wwn(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    uint64_t wwn = 0;
    Aic t;

    aic_open(&t, obj, alloc);
    for (int i = 0; i < 4; i++) {
        wwn = (wwn << 16) | seeprom_word(&t, 3 + i, SEEPROM_READ);
    }
    g_assert_cmphex(wwn, ==, UINT64_C(0x2100000000000001));
    aic_close(&t);
}

static void test_seeprom_other(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    /* Word 0 has the multi-host speed nibble the driver wants, 0xf. */
    g_assert_cmphex(seeprom_word(&t, 0, SEEPROM_READ), ==, 0x000f);
    /* Word 2, the media sense: voltage sensing, as the driver wants. */
    g_assert_cmphex(seeprom_word(&t, 2, SEEPROM_READ), ==, 0x0100);
    /* An unwritten word reads all ones. */
    g_assert_cmphex(seeprom_word(&t, 20, SEEPROM_READ), ==, 0xffff);
    /* Other opcodes are not modelled: they leave the data cells alone. */
    wr8(&t, REG_SEEPROM_ADDR, 3);
    wr8(&t, REG_SEEPROM_CMD, 0x31);
    g_assert_cmphex(rd8(&t, REG_SEEPROM_CMD) & 1, ==, 0);
    g_assert_cmphex(rd8(&t, REG_SEEPROM_DATA), ==, 0xff);
    aic_close(&t);
}

/*
 * The RAM regions the driver's self test fills and checks: the cells of all
 * eight banks, the global cells, the scatter/gather RAM behind bank 2
 * register 0x60 and the command buffer behind bank 4 register 0x20.
 */
static void test_self_test_rams(void *obj, void *data, QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);

    for (int bank = 0; bank < 8; bank++) {
        wr8(&t, REG_BANK, bank * 0x11);
        for (unsigned off = 0x40; off <= 0x5e; off += 2) {
            wr16(&t, off, 0x55aa);
        }
        for (unsigned off = 0x40; off <= 0x5e; off += 2) {
            g_assert_cmphex(rd16(&t, off), ==, 0x55aa);
        }
    }
    wr8(&t, REG_BANK, BANK_0);

    for (unsigned off = 0x38; off <= 0x3e; off += 2) {
        wr16(&t, off, 0x55aa);
    }
    for (unsigned off = 0x38; off <= 0x3e; off += 2) {
        g_assert_cmphex(rd16(&t, off), ==, 0x55aa);
    }

    /* Scatter/gather RAM: 128 bytes a TCB, rewound, then 0xa5 over it. */
    wr8(&t, REG_BANK, 0x22);
    for (unsigned tcb = 0; tcb < 3; tcb++) {
        wr16(&t, 0x64, tcb);
        wr8(&t, 0x8b, 0);
        for (unsigned i = 0; i < 128; i++) {
            wr8(&t, 0x60, i + tcb);
        }
        wr8(&t, 0x8b, 0);
        for (unsigned i = 0; i < 128; i++) {
            wr8(&t, 0x60, 0xa5);
        }
    }
    for (unsigned tcb = 0; tcb < 3; tcb++) {
        wr16(&t, 0x64, tcb);
        wr8(&t, 0x8b, 0);
        for (unsigned i = 0; i < 128; i++) {
            g_assert_cmphex(rd8(&t, 0x60), ==, 0xa5);
        }
    }
    /* Each TCB's area is its own. */
    wr16(&t, 0x64, 1);
    wr8(&t, 0x8b, 0);
    wr8(&t, 0x60, 0x11);
    wr16(&t, 0x64, 2);
    wr8(&t, 0x8b, 0);
    g_assert_cmphex(rd8(&t, 0x60), ==, 0xa5);
    wr16(&t, 0x64, 1);
    wr8(&t, 0x8b, 0);
    g_assert_cmphex(rd8(&t, 0x60), ==, 0x11);

    /* Command buffer: an address, then a byte stream. */
    wr8(&t, REG_BANK, BANK_4);
    wr8(&t, 0x9e, 0);
    for (unsigned i = 0; i < 128; i += 2) {
        wr16(&t, 0x20, 0x55aa);
    }
    wr8(&t, 0x9e, 0);
    for (unsigned i = 0; i < 128; i += 2) {
        g_assert_cmphex(rd16(&t, 0x20), ==, 0x55aa);
    }
    aic_close(&t);
}

/* A doorbell before the sequencer runs does nothing. */
static void test_doorbell_not_running(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    Aic t;

    aic_open(&t, obj, alloc);
    wr16(&t, REG_DOORBELL, 1);
    g_assert_cmphex(rd16(&t, REG_DOORBELL), ==, 1);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    aic_close(&t);
}

/* ---- SCSI commands through the rings ---- */

static void *disk_setup(GString *cmd_line, void *arg)
{
    uint8_t *image = g_malloc(DISK_SIZE);
    int fd = g_file_open_tmp("qtest-aic1160.XXXXXX", &disk_path, NULL);

    g_assert_cmpint(fd, >=, 0);
    for (unsigned i = 0; i < DISK_SIZE; i++) {
        image[i] = i % 251;
    }
    g_assert_cmpint(write(fd, image, DISK_SIZE), ==, DISK_SIZE);
    close(fd);
    g_free(image);
    g_string_append_printf(cmd_line,
                           " -drive id=d0,if=none,format=raw,file=%s"
                           " -device scsi-hd,bus=fc0.0,drive=d0,"
                           "scsi-id=%d,lun=0,port_wwn=0x%" PRIx64
                           ",wwn=0x%" PRIx64
                           " -drive id=d1,if=none,format=raw,file=null-co://"
                           " -device scsi-hd,bus=fc0.0,drive=d1,scsi-id=0,"
                           "lun=0,port_wwn=0x2100000000000000,"
                           "wwn=0x2000000000000000",
                           disk_path, DISK_ID, DISK_PORT_WWN, DISK_NODE_WWN);
    return arg;
}

static void *wwn_setup(GString *cmd_line, void *arg)
{
    g_string_append_printf(cmd_line, " -global aic1160.wwn=0x%" PRIx64, WWN);
    return arg;
}

static void *romfile_setup(GString *cmd_line, void *arg)
{
    GError *err = NULL;
    int fd = g_file_open_tmp("aic1160-rom-XXXXXX", &rom_path, &err);
    char image[0x1000] = { 0x55, 0xaa };

    g_assert_no_error(err);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    g_string_append_printf(cmd_line, " -global aic1160.romfile=%s", rom_path);
    return arg;
}

static void *rombar0_setup(GString *cmd_line, void *arg)
{
    g_string_append(cmd_line, " -global aic1160.rombar=0");
    return arg;
}

static void *revision_setup(GString *cmd_line, void *arg)
{
    g_string_append(cmd_line, " -global aic1160.revision=1");
    return arg;
}

/* What every driver does at attach, then the rings the driver set up. */
static void aic_start(Aic *t, unsigned count)
{
    uint8_t ff[4096];

    g_assert_cmpuint(count * 4, <=, sizeof(ff));
    /* QEMU has the disk open; the name is no longer needed. */
    if (disk_path) {
        unlink(disk_path);
        g_free(disk_path);
        disk_path = NULL;
    }
    t->count = count;
    t->tcbs = guest_alloc(t->alloc, count * TCB_SIZE);
    t->done = guest_alloc(t->alloc, count * 4);
    t->status_block = guest_alloc(t->alloc, 8);
    memset(ff, 0xff, sizeof(ff));
    qtest_memwrite(t->qts, t->done, ff, count * 4);
    memset(ff, 0, 8);
    qtest_memwrite(t->qts, t->status_block, ff, 8);

    wr8(t, REG_HOST_CTL, HOST_PAUSE);
    wr8(t, REG_HOST_CTL, HOST_RESET);
    wr8(t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(t, REG_SEQ_ADDR, 0);
    for (int i = 0; i < 16; i++) {
        wr16(t, REG_SEQ_DATA, 0x1000 + i);
    }
    wr8(t, REG_SEQ_CTL, SEQ_RUN);

    wr8(t, REG_SCRATCH_PAGE, 0);
    seq_wr64(t, SCR_TCB_BASE, t->tcbs);
    seq_wr64(t, SCR_DONE_BASE, t->done);
    seq_wr16(t, SCR_DONE_NEG, (uint16_t)-(int)count);
    seq_wr8(t, SCR_PASS, 0xff);
    g_assert_cmphex(seq_rd8(t, SCR_PASS), ==, 0xff);

    wr8(t, REG_BANK, BANK_4);
    for (int i = 0; i < 8; i++) {
        wr8(t, REG_STATUS_BLOCK + i, t->status_block >> (8 * i));
    }
    wr16(t, REG_TCB_CONSUMER, 0);
    wr8(t, REG_COMPLETE_CTL, COMPLETE_ARM);
    wr8(t, REG_BANK, BANK_0);
    wr8(t, REG_HOST_CTL, HOST_IRQ_EN);
}

/* Build a SCSI TCB: flat for one segment, a list for @nsegs > 0. */
static void aic_build(Aic *t, uint8_t *tcb, uint16_t handle, uint8_t alpa,
                      unsigned lun, const uint8_t *cdb, uint32_t dl,
                      const Seg *segs, unsigned nsegs, uint64_t rsp)
{
    memset(tcb, 0, TCB_SIZE);
    stw_le_p(tcb, handle);
    stw_le_p(tcb + 0x02, 0xffff);
    tcb[0x04] = alpa;
    tcb[0x07] = 0x01;               /* R_CTL: FCP command */
    tcb[0x0f] = 0x08;               /* TYPE: FCP */
    tcb[0x21] = cdb[0] == 0x2a ? 0x02 : 0x00;
    tcb[0x24] = lun >> 8;
    tcb[0x25] = lun;
    memcpy(tcb + 0x30, cdb, 16);
    stl_be_p(tcb + 0x40, dl);
    if (nsegs > 1) {
        uint8_t entries[4 * 16] = { 0 };
        uint64_t list;

        g_assert_cmpuint(nsegs, <=, 4);
        for (unsigned i = 0; i < nsegs; i++) {
            stq_le_p(entries + 16 * i, segs[i].addr);
            stl_le_p(entries + 16 * i + 8, segs[i].len |
                     (i == nsegs - 1 ? 1u << 24 : 0));
        }
        list = guest_alloc(t->alloc, sizeof(entries));
        qtest_memwrite(t->qts, list, entries, sizeof(entries));
        tcb[0x23] = MODE_SG;
        stq_le_p(tcb + 0x4c, list);
        /* A list has no count; its last entry's length has a top byte of 1. */
        stl_le_p(tcb + 0x54, 0);
    } else {
        tcb[0x23] = nsegs ? MODE_FLAT : MODE_NONE;
        if (nsegs) {
            stq_le_p(tcb + 0x4c, segs[0].addr);
            stl_le_p(tcb + 0x54, segs[0].len);
        }
    }
    stq_le_p(tcb + 0x58, rsp);
    stl_le_p(tcb + 0x60, dl);
    stw_le_p(tcb + 0x68, handle);
}

static void aic_put(Aic *t, const uint8_t *tcb)
{
    qtest_memwrite(t->qts, t->tcbs + ((t->origin + t->produced) % t->count) *
                           TCB_SIZE, tcb, TCB_SIZE);
    t->produced = (t->produced + 1) % t->count;
}

static void aic_post(Aic *t, uint16_t handle, uint8_t alpa, unsigned lun,
                     const uint8_t *cdb, uint32_t dl, const Seg *segs,
                     unsigned nsegs, uint64_t rsp)
{
    uint8_t tcb[TCB_SIZE];

    aic_build(t, tcb, handle, alpa, lun, cdb, dl, segs, nsegs, rsp);
    aic_put(t, tcb);
    wr16(t, REG_DOORBELL, t->produced);
}

/* Wait for the next done-queue entry: its pass byte becomes the lap. */
static Done aic_wait_done(Aic *t, bool ack)
{
    uint8_t entry[4];
    Done d;

    for (int i = 0; i < 10000; i++) {
        qtest_memread(t->qts, t->done + t->consumed * 4, entry,
                      sizeof(entry));
        if (entry[3] == t->expected_pass) {
            break;
        }
        g_usleep(1000);
    }
    d.handle = lduw_le_p(entry);
    d.status = entry[2];
    d.pass = entry[3];
    g_assert_cmphex(d.pass, ==, t->expected_pass);
    if (++t->consumed == t->count) {
        t->consumed = 0;
        t->expected_pass++;
    }
    if (ack) {
        wr8(t, REG_INT_STATUS_HI, INT_COMPLETE >> 8);
    }
    return d;
}

static const uint8_t cdb_tur[16] = { 0 };

/*
 * The first command a disk takes after power-on reports a unit attention,
 * and the first command here is INQUIRY-exempt only for INQUIRY. Take it
 * on a TEST UNIT READY, which also checks the sense data comes back.
 */
static void aic_clear_unit_attention(Aic *t)
{
    uint64_t rsp = guest_alloc(t->alloc, RSP_SIZE);
    uint8_t r[RSP_SIZE];
    Done d;

    aic_post(t, 0xff00, DISK_ALPA, 0, cdb_tur, 0, NULL, 0, rsp);
    d = aic_wait_done(t, true);
    g_assert_cmphex(d.handle, ==, 0xff00);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);
    qtest_memread(t->qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0a], ==, 0x02);
    g_assert_cmphex(r[0x0b], ==, 0x02);
    g_assert_cmphex(r[0x18 + 2] & 0x0f, ==, 0x06);
}

static void cdb_rw(uint8_t *cdb, uint8_t op, uint32_t lba, uint16_t blocks)
{
    memset(cdb, 0, 16);
    cdb[0] = op;
    stl_be_p(cdb + 2, lba);
    stw_be_p(cdb + 7, blocks);
}

static void test_scsi_inquiry(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t cdb[16] = { 0x12, 0, 0, 0, 36 };
    uint8_t out[36], r[RSP_SIZE];
    uint64_t buf, rsp;
    Seg seg;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    buf = guest_alloc(alloc, 64);
    rsp = guest_alloc(alloc, RSP_SIZE);
    seg = (Seg) { buf, 36 };
    aic_post(&t, 0x0101, DISK_ALPA, 0, cdb, 36, &seg, 1, rsp);
    d = aic_wait_done(&t, false);
    g_assert_cmphex(d.handle, ==, 0x0101);
    g_assert_cmphex(d.status, ==, 0);
    g_assert_cmphex(d.pass, ==, 0);
    qtest_memread(t.qts, buf, out, sizeof(out));
    g_assert_cmpmem(out + 8, 4, "QEMU", 4);
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0b], ==, 0);
    g_assert_cmphex(r[0x0a], ==, 0);
    g_assert_cmphex(ldl_be_p(r + 0x0c), ==, 0);

    /* The completion raised the line and reached the status block. */
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, INT_COMPLETE);
    g_assert_true(bridge_pin(&t));
    qtest_memread(t.qts, t.status_block, r, 2);
    g_assert_cmphex(r[1], ==, INT_COMPLETE >> 8);
    wr8(&t, REG_INT_STATUS_HI, INT_COMPLETE >> 8);
    g_assert_false(bridge_pin(&t));
    aic_close(&t);
}

/* A READ into one buffer, then a READ into a scatter list. */
static void test_scsi_read(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t cdb[16], *out = g_malloc(4096), r[RSP_SIZE];
    uint64_t buf, rsp, a, b;
    Seg segs[2];
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    rsp = guest_alloc(alloc, RSP_SIZE);

    buf = guest_alloc(alloc, 4096);
    cdb_rw(cdb, 0x28, 2, 8);
    segs[0] = (Seg) { buf, 4096 };
    aic_post(&t, 0x0201, DISK_ALPA, 0, cdb, 4096, segs, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0201);
    qtest_memread(t.qts, buf, out, 4096);
    for (unsigned i = 0; i < 4096; i++) {
        g_assert_cmphex(out[i], ==, (1024 + i) % 251);
    }
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0b], ==, 0);
    g_assert_cmphex(ldl_be_p(r + 0x0c), ==, 0);

    /* Three blocks in two non-contiguous pieces. */
    a = guest_alloc(alloc, 1024);
    guest_alloc(alloc, 4096);
    b = guest_alloc(alloc, 4096);
    cdb_rw(cdb, 0x28, 0, 3);
    segs[0] = (Seg) { a, 1000 };
    segs[1] = (Seg) { b, 536 };
    aic_post(&t, 0x0202, DISK_ALPA, 0, cdb, 1536, segs, 2, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0202);
    qtest_memread(t.qts, a, out, 1000);
    qtest_memread(t.qts, b, out + 1000, 536);
    for (unsigned i = 0; i < 1536; i++) {
        g_assert_cmphex(out[i], ==, i % 251);
    }
    g_free(out);
    aic_close(&t);
}

static void test_scsi_write(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t cdb[16], pattern[1024], out[1024], r[RSP_SIZE];
    uint64_t wbuf, rbuf, rsp;
    Seg seg;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    rsp = guest_alloc(alloc, RSP_SIZE);
    wbuf = guest_alloc(alloc, 1024);
    rbuf = guest_alloc(alloc, 1024);
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = 0x80 ^ i;
    }
    qtest_memwrite(t.qts, wbuf, pattern, sizeof(pattern));

    cdb_rw(cdb, 0x2a, 100, 2);
    seg = (Seg) { wbuf, 1024 };
    aic_post(&t, 0x0301, DISK_ALPA, 0, cdb, 1024, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0301);
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0b], ==, 0);

    cdb_rw(cdb, 0x28, 100, 2);
    seg = (Seg) { rbuf, 1024 };
    aic_post(&t, 0x0302, DISK_ALPA, 0, cdb, 1024, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0302);
    qtest_memread(t.qts, rbuf, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), pattern, sizeof(pattern));
    aic_close(&t);
}

/* A short transfer completes underrun; a failure is an error response. */
static void test_scsi_residual_and_sense(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    uint8_t cdb[16], r[RSP_SIZE];
    uint64_t buf, rsp;
    Seg seg;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    rsp = guest_alloc(alloc, RSP_SIZE);
    buf = guest_alloc(alloc, 2048);

    /* One block asked for with room for two: 512 bytes under. */
    cdb_rw(cdb, 0x28, 0, 1);
    seg = (Seg) { buf, 1024 };
    aic_post(&t, 0x0401, DISK_ALPA, 0, cdb, 1024, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);
    /* GOOD status, residual under 512, and the residual-info block. */
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0a], ==, 0x08);
    g_assert_cmphex(r[0x0b], ==, 0);
    g_assert_cmphex(ldl_be_p(r + 0x0c), ==, 512);
    /* One buffer: a zero pointer, the bytes left, nothing else. */
    g_assert_cmphex(ldq_le_p(r + 0x80), ==, 0);
    g_assert_cmphex(r[0x88] | (r[0x89] << 8) | (r[0x8a] << 16), ==, 512);
    g_assert_cmphex(r[0x8b], ==, 0);
    g_assert_cmphex(r[0x8c], ==, 0);

    /* A read past the end of the disk: CHECK CONDITION with sense. */
    cdb_rw(cdb, 0x28, DISK_SIZE / 512, 1);
    seg = (Seg) { buf, 512 };
    aic_post(&t, 0x0402, DISK_ALPA, 0, cdb, 512, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0402);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0a], ==, 0x0a);     /* Sense valid, residual under. */
    g_assert_cmphex(r[0x0b], ==, 0x02);
    g_assert_cmphex(ldl_be_p(r + 0x0c), ==, 512);
    g_assert_cmphex(ldl_be_p(r + 0x10), >=, 14);
    g_assert_cmphex(r[0x18] & 0x7f, ==, 0x70);
    g_assert_cmphex(r[0x18 + 2] & 0x0f, ==, 0x05);
    aic_close(&t);
}

/*
 * A short transfer into a list. The block's pointer is the list's address
 * plus 0x80 for each page of eight descriptors passed, plus 0x80; its index
 * nibble the next descriptor in that page; its residual what the stopped
 * descriptor has left. The driver adds the later descriptors itself.
 */
static void test_scsi_residual_list(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint8_t cdb[16], r[RSP_SIZE];
    uint64_t a, b, c, rsp;
    Seg segs[3];
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    rsp = guest_alloc(alloc, RSP_SIZE);
    a = guest_alloc(alloc, 1024);
    b = guest_alloc(alloc, 1024);
    c = guest_alloc(alloc, 1024);

    /* One 512-byte block for 2560 bytes of room: 512, 1024 and 1024. */
    cdb_rw(cdb, 0x28, 0, 1);
    segs[0] = (Seg) { a, 512 };
    segs[1] = (Seg) { b, 1024 };
    segs[2] = (Seg) { c, 1024 };
    aic_post(&t, 0x0801, DISK_ALPA, 0, cdb, 2560, segs, 3, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0a], ==, 0x08);
    g_assert_cmphex(ldl_be_p(r + 0x0c), ==, 2048);
    /* The transfer ended at the edge of descriptor 0: 1024 left in 1. */
    g_assert_cmphex(r[0x88] | (r[0x89] << 8) | (r[0x8a] << 16), ==, 1024);
    g_assert_cmphex(r[0x8b], ==, 0);
    /* The next is descriptor 2: the list address plus 0x80, index 2. */
    g_assert_cmphex(r[0x8c] >> 4, ==, 2);
    g_assert_cmphex(ldq_le_p(r + 0x80) & 0xff, ==, 0x80);
    aic_close(&t);
}

/* An absent port completes with the no-target status. */
static void test_scsi_no_target(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t r[RSP_SIZE];
    uint64_t rsp;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    rsp = guest_alloc(alloc, RSP_SIZE);

    aic_post(&t, 0x0501, 0xe8, 0, cdb_tur, 0, NULL, 0, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0501);
    g_assert_cmphex(d.status, ==, DONE_NO_DEVICE);

    /* A missing LUN on a present port is the SCSI layer's CHECK CONDITION. */
    aic_post(&t, 0x0502, DISK_ALPA, 3, cdb_tur, 0, NULL, 0, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0502);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);
    qtest_memread(t.qts, rsp, r, sizeof(r));
    g_assert_cmphex(r[0x0b], ==, 0x02);
    g_assert_cmphex(r[0x18 + 2] & 0x0f, ==, 0x05);
    aic_close(&t);
}

/* The pass byte is the lap number around a four-entry ring. */
static void test_done_queue_laps(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    uint64_t rsp;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 4);
    aic_clear_unit_attention(&t);
    rsp = guest_alloc(alloc, RSP_SIZE);
    for (unsigned i = 0; i < 10; i++) {
        aic_post(&t, 0x0600 + i, DISK_ALPA, 0, cdb_tur, 0, NULL, 0, rsp);
        d = aic_wait_done(&t, true);
        g_assert_cmphex(d.handle, ==, 0x0600 + i);
        g_assert_cmphex(d.pass, ==, (i + 1) / 4);
    }
    /* The chip's consumer position wrapped with the ring. */
    wr8(&t, REG_BANK, BANK_4);
    g_assert_cmphex(rd16(&t, REG_TCB_CONSUMER), ==, 11 % 4);
    aic_close(&t);
}

/* Several TCBs posted with one doorbell all complete. */
static void test_batch(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t cdb[16], tcb[TCB_SIZE], out[512];
    uint64_t buf[3], rsp[3];
    unsigned seen = 0;
    Seg seg;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    for (int i = 0; i < 3; i++) {
        buf[i] = guest_alloc(alloc, 512);
        rsp[i] = guest_alloc(alloc, RSP_SIZE);
        cdb_rw(cdb, 0x28, i, 1);
        seg = (Seg) { buf[i], 512 };
        aic_build(&t, tcb, 0x0700 + i, DISK_ALPA, 0, cdb, 512, &seg, 1,
                  rsp[i]);
        aic_put(&t, tcb);
    }
    wr16(&t, REG_DOORBELL, t.produced);
    for (int i = 0; i < 3; i++) {
        Done d = aic_wait_done(&t, true);

        g_assert_cmphex(d.handle & 0xff00, ==, 0x0700);
        seen |= 1u << (d.handle & 0xff);
    }
    g_assert_cmphex(seen, ==, 7);
    for (int i = 0; i < 3; i++) {
        qtest_memread(t.qts, buf[i], out, sizeof(out));
        for (unsigned j = 0; j < sizeof(out); j++) {
            g_assert_cmphex(out[j], ==, (i * 512 + j) % 251);
        }
    }
    aic_close(&t);
}


/* ---- Link services and loop initialisation ---- */

static void build_els(Aic *t, uint8_t *tcb, uint16_t handle, uint8_t alpa,
                      uint8_t code, uint64_t payload, uint64_t rsp)
{
    uint8_t body[4] = { code };

    qtest_memwrite(t->qts, payload, body, sizeof(body));
    memset(tcb, 0, TCB_SIZE);
    stw_le_p(tcb, handle);
    stw_le_p(tcb + 0x02, 0xffff);
    tcb[0x04] = alpa;
    tcb[0x07] = 0x22;               /* R_CTL: ELS request */
    tcb[0x0f] = 0x01;               /* TYPE: extended link service */
    tcb[0x21] = 0x0a;               /* Send link command, receive reply */
    tcb[0x23] = MODE_FLAT;
    stq_le_p(tcb + 0x4c, payload);
    stl_le_p(tcb + 0x54, sizeof(body));
    stq_le_p(tcb + 0x58, rsp);
    stw_le_p(tcb + 0x68, handle);
}

static Done els(Aic *t, uint16_t handle, uint8_t alpa, uint8_t code,
                uint8_t *accept)
{
    uint64_t payload = guest_alloc(t->alloc, 64);
    uint64_t rsp = guest_alloc(t->alloc, RSP_SIZE);
    uint8_t tcb[TCB_SIZE];
    Done d;

    build_els(t, tcb, handle, alpa, code, payload, rsp);
    aic_put(t, tcb);
    wr16(t, REG_DOORBELL, t->produced);
    d = aic_wait_done(t, true);
    qtest_memread(t->qts, rsp, accept, 128);
    return d;
}

/* PLOGI, PRLI and ADISC each get an accept with the port's names. */
static void test_els_accepts(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t a[128];
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);

    d = els(&t, 0x0801, DISK_ALPA, 0x03, a);
    g_assert_cmphex(d.handle, ==, 0x0801);
    g_assert_cmphex(d.status, ==, DONE_OK);
    g_assert_cmphex(a[0], ==, 0x02);        /* LS_ACC */
    g_assert_cmphex(ldq_be_p(a + 20), ==, DISK_PORT_WWN);
    g_assert_cmphex(ldq_be_p(a + 28), ==, DISK_NODE_WWN);
    g_assert_cmphex(a[68] & 0x80, ==, 0x80);

    d = els(&t, 0x0802, DISK_ALPA, 0x20, a);
    g_assert_cmphex(d.status, ==, DONE_OK);
    g_assert_cmphex(a[0], ==, 0x02);
    g_assert_cmphex(a[4], ==, 0x08);        /* FCP service parameter page */
    g_assert_cmphex(ldl_be_p(a + 16) & 0x10, ==, 0x10);

    d = els(&t, 0x0803, DISK_ALPA, 0x52, a);
    g_assert_cmphex(d.status, ==, DONE_OK);
    g_assert_cmphex(a[0], ==, 0x02);
    g_assert_cmphex(a[7], ==, DISK_ALPA);
    g_assert_cmphex(ldq_be_p(a + 8), ==, DISK_PORT_WWN);
    g_assert_cmphex(ldq_be_p(a + 16), ==, DISK_NODE_WWN);
    g_assert_cmphex(a[27], ==, DISK_ALPA);

    /* An unknown command code is rejected, still a completed exchange. */
    d = els(&t, 0x0804, DISK_ALPA, 0x7f, a);
    g_assert_cmphex(d.status, ==, DONE_OK);
    g_assert_cmphex(a[0], ==, 0x01);        /* LS_RJT */

    /* A login to an absent port is undelivered. */
    d = els(&t, 0x0805, 0xe8, 0x03, a);
    g_assert_cmphex(d.status, ==, DONE_NO_DEVICE);
    aic_close(&t);
}

/* An ELS with no payload is a frame-format error. */
static void test_els_format(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t tcb[TCB_SIZE];
    uint64_t payload, rsp;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    payload = guest_alloc(alloc, 64);
    rsp = guest_alloc(alloc, RSP_SIZE);
    build_els(&t, tcb, 0x0901, DISK_ALPA, 0x03, payload, rsp);
    stl_le_p(tcb + 0x54, 0);
    aic_put(&t, tcb);
    wr16(&t, REG_DOORBELL, t.produced);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_FORMAT);
    aic_close(&t);
}

/*
 * The driver's loop initialisation (hwutil sh_loadnrun_lip): every block's
 * TCB pointer aimed at TCB N, the LIP TCB copied into TCB RAM slot N, the
 * valid AL_PA list into scratch page 1, the loop image streamed in and
 * started, the done queue restarted, and the sequencer unpaused. @words is
 * the image's length: the loop image is 822 words and four of padding.
 */
static void aic_lip_load(Aic *t, unsigned n, uint64_t map_buf, unsigned words)
{
    static const int banks[] = { 0, 2, 1, 5, 4, 3 };
    uint8_t lip[128] = { 0 };

    wr8(t, REG_HOST_CTL, HOST_IRQ_EN | HOST_PAUSE);
    for (int i = 0; i < ARRAY_SIZE(banks); i++) {
        wr8(t, REG_BANK, banks[i] * 0x11);
        wr16(t, 0x64, n);
    }
    wr8(t, REG_BANK, BANK_0);

    stl_le_p(lip, 132);
    lip[0x07] = 1;
    lip[0x0c] = 0xef;
    lip[0x0f] = 0x22;
    lip[0x16] = 0x38;
    lip[0x17] = 0x01;
    stq_le_p(lip + 0x38, map_buf);
    for (unsigned i = 0; i < sizeof(lip); i++) {
        seq_wr8(t, 0x80 + i, lip[i]);
    }
    wr8(t, REG_SCRATCH_PAGE, 1);
    for (unsigned i = 0; i < 128; i++) {
        seq_wr8(t, i, i);
    }
    wr8(t, REG_SCRATCH_PAGE, 0);

    wr8(t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(t, REG_SEQ_ADDR, 0);
    for (unsigned i = 0; i < words; i++) {
        wr16(t, REG_SEQ_DATA, 0x3000 + i);
    }
    wr16(t, REG_SEQ_ADDR, 0);
    for (unsigned i = 0; i < words; i++) {
        g_assert_cmphex(rd16(t, REG_SEQ_DATA), ==, 0x3000 + i);
    }
    wr8(t, REG_SEQ_CTL, SEQ_RUN);
}

/* Restart the done queue, host and chip, as the driver does around a LIP. */
static void aic_done_restart(Aic *t)
{
    uint8_t ff[64];

    memset(ff, 0xff, sizeof(ff));
    qtest_memwrite(t->qts, t->done, ff, t->count * 4);
    seq_wr8(t, SCR_PASS, 0xff);
    t->consumed = 0;
    t->expected_pass = 0;
}

static void aic_lip_start(Aic *t)
{
    aic_done_restart(t);
    wr8(t, REG_SEQ_CTL, SEQ_RUN | 0x02);
    wr8(t, REG_HOST_CTL, HOST_IRQ_EN);
}

/*
 * Loop initialisation: the map DMA'd to the buffer at TCB 0x38 in the
 * count-list form without the host, one done entry for TCB N with the host
 * AL_PA as its status, and the completion interrupt.
 */
static void test_lip_map(void *obj, void *data, QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 256);
    uint8_t map[132];
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    memset(map, 0xee, sizeof(map));
    qtest_memwrite(t.qts, buf, map, sizeof(map));
    aic_lip_load(&t, 4, buf, 826);
    g_assert_false(bridge_pin(&t));
    aic_lip_start(&t);
    d = aic_wait_done(&t, false);
    g_assert_cmphex(d.handle, ==, 4);
    /* The host's AL_PA: loop ID 125, the driver's default. */
    g_assert_cmphex(d.status, ==, 0x01);
    g_assert_cmphex(d.pass, ==, 0);
    qtest_memread(t.qts, buf, map, sizeof(map));
    g_assert_cmphex(map[0], ==, 2);
    g_assert_cmphex(map[1], ==, DISK_ALPA0);
    g_assert_cmphex(map[2], ==, DISK_ALPA);
    g_assert_cmphex(map[3], ==, 0);
    /* The interrupt is not optional: the driver does not poll for a LIP. */
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, INT_COMPLETE);
    g_assert_true(bridge_pin(&t));
    wr8(&t, REG_INT_STATUS_HI, INT_COMPLETE >> 8);
    aic_close(&t);
}

/* Only the loop image starts loop initialisation. */
static void test_lip_needs_loop_image(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 256);
    uint8_t entry[4];
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_lip_load(&t, 4, buf, 1960);
    aic_lip_start(&t);
    g_usleep(20000);
    qtest_memread(t.qts, t.done, entry, sizeof(entry));
    g_assert_cmphex(entry[3], ==, 0xff);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    g_assert_false(bridge_pin(&t));
    aic_close(&t);
}

/* A header that is no SCSI command, login or loop initialisation. */
static void test_unrecognised_tcb(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    uint8_t tcb[TCB_SIZE] = { 0 };
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    stw_le_p(tcb, 0x0c01);
    stw_le_p(tcb + 0x68, 0x0c01);
    aic_put(&t, tcb);
    wr16(&t, REG_DOORBELL, t.produced);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0c01);
    g_assert_cmphex(d.status, ==, DONE_FORMAT);
    aic_close(&t);
}

/* Each completion status code the model produces. */
static void test_status_codes(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t cdb[16];
    uint64_t buf, rsp;
    Seg seg;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    aic_clear_unit_attention(&t);
    buf = guest_alloc(alloc, 1024);
    rsp = guest_alloc(alloc, RSP_SIZE);

    cdb_rw(cdb, 0x28, 0, 1);
    seg = (Seg) { buf, 512 };
    aic_post(&t, 0x0b01, DISK_ALPA, 0, cdb, 512, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_OK);

    seg = (Seg) { buf, 1024 };
    aic_post(&t, 0x0b02, DISK_ALPA, 0, cdb, 1024, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);

    /* More read than the FCP data length allows: overrun. */
    seg = (Seg) { buf, 1024 };
    cdb_rw(cdb, 0x28, 0, 2);
    aic_post(&t, 0x0b03, DISK_ALPA, 0, cdb, 512, &seg, 1, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_ERROR_RSP);

    aic_post(&t, 0x0b04, 0xe8, 0, cdb_tur, 0, NULL, 0, rsp);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.status, ==, DONE_NO_DEVICE);
    aic_close(&t);
}

/* ---- Receive buffers and loop bring-up ---- */

/* A receive-buffer TCB: control 1, three buffer addresses, a handle. */
static void build_receive(uint8_t *tcb, uint16_t handle, uint64_t buf)
{
    memset(tcb, 0, TCB_SIZE);
    tcb[0x21] = 0x01;
    stq_le_p(tcb + 0x24, buf);
    stq_le_p(tcb + 0x44, buf + 0x40);
    stq_le_p(tcb + 0x58, buf + 0x80);
    stw_le_p(tcb + 0x68, handle);
}

/* Receive buffers are held, not completed, and do not block commands. */
static void test_receive_buffers_held(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    uint8_t tcb[TCB_SIZE], entry[4];
    uint64_t buf = guest_alloc(alloc, 512), rsp;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 8);
    for (uint16_t h = 1; h <= 3; h++) {
        build_receive(tcb, h, buf);
        aic_put(&t, tcb);
    }
    wr16(&t, REG_DOORBELL, t.produced);
    g_usleep(20000);
    qtest_memread(t.qts, t.done, entry, sizeof(entry));
    g_assert_cmphex(entry[3], ==, 0xff);
    g_assert_cmphex(rd16(&t, REG_INT_STATUS), ==, 0);
    g_assert_false(bridge_pin(&t));

    /* The completion that follows echoes the handle at 0x68, not 0x00. */
    rsp = guest_alloc(alloc, RSP_SIZE);
    aic_build(&t, tcb, 0x0d01, DISK_ALPA, 0, cdb_tur, 0, NULL, 0, rsp);
    stw_le_p(tcb, 0);
    aic_put(&t, tcb);
    wr16(&t, REG_DOORBELL, t.produced);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 0x0d01);
    aic_close(&t);
}

/*
 * The LIP request: a write to config byte 0x4b raises LIP ready, status
 * byte 1 bit 0, with the line.
 */
static void test_lip_request(void *obj, void *data, QGuestAllocator *alloc)
{
    QAic1160 *aic = obj;
    Aic t;

    aic_open(&t, obj, alloc);
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);
    g_assert_false(bridge_pin(&t));
    qpci_config_writeb(&aic->dev, 0x4b, 0x01);
    g_assert_cmphex(rd8(&t, REG_INT_STATUS_HI), ==, 0x01);
    g_assert_true(bridge_pin(&t));
    wr8(&t, REG_INT_STATUS_HI, 0x01);
    g_assert_false(bridge_pin(&t));
    /* The close is not: the driver writes it when a LIP is over. */
    qpci_config_writeb(&aic->dev, 0x4b, 0x00);
    g_assert_cmphex(rd8(&t, REG_INT_STATUS_HI), ==, 0);
    g_assert_false(bridge_pin(&t));
    qpci_config_writeb(&aic->dev, 0x4b, 0x01);
    g_assert_cmphex(rd8(&t, REG_INT_STATUS_HI), ==, 0x01);
    g_assert_true(bridge_pin(&t));
    aic_close(&t);
}

/*
 * The whole bring-up: receive buffers, the LIP request, the LIP, the
 * mainline reloaded with the ring pointer put back, then a login and an
 * INQUIRY for each target found.
 */
static void test_loop_bringup(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t tcb[TCB_SIZE], map[132], a[128], inq[36];
    uint8_t cdb[16] = { 0x12, 0, 0, 0, 36 };
    uint64_t buf = guest_alloc(alloc, 256), ibuf, rsp;
    QAic1160 *aic = obj;
    Seg seg;
    Done d;
    Aic t;

    aic_open(&t, obj, alloc);
    aic_start(&t, 16);

    /* The receive buffers the driver leaves with the chip. */
    for (uint16_t h = 1; h <= 3; h++) {
        build_receive(tcb, h, buf);
        aic_put(&t, tcb);
    }
    wr16(&t, REG_DOORBELL, t.produced);

    qpci_config_writeb(&aic->dev, 0x4b, 0x01);
    g_assert_cmphex(rd8(&t, REG_INT_STATUS_HI) & 0x01, ==, 0x01);
    wr8(&t, REG_INT_STATUS_HI, 0x01);
    qpci_config_writeb(&aic->dev, 0x4b, 0x00);

    aic_lip_load(&t, 4, buf, 826);
    aic_lip_start(&t);
    d = aic_wait_done(&t, true);
    g_assert_cmphex(d.handle, ==, 4);
    g_assert_cmphex(d.status, ==, 0x01);
    qtest_memread(t.qts, buf, map, sizeof(map));
    g_assert_cmphex(map[0], ==, 2);

    /*
     * The mainline comes back: the ring pointer is written back as the next
     * free entry, the done queue restarts again, the mainline image starts.
     */
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN | HOST_PAUSE);
    aic_done_restart(&t);
    t.origin = (t.origin + t.produced) % t.count;
    t.produced = 0;
    seq_wr64(&t, SCR_TCB_BASE, t.tcbs + t.origin * TCB_SIZE);
    wr8(&t, REG_BANK, BANK_4);
    wr16(&t, REG_TCB_CONSUMER, 0);
    wr8(&t, REG_BANK, BANK_0);
    wr16(&t, REG_DOORBELL, 0);
    wr8(&t, REG_SEQ_CTL, SEQ_LOAD);
    wr16(&t, REG_SEQ_ADDR, 0);
    for (unsigned i = 0; i < 1960; i++) {
        wr16(&t, REG_SEQ_DATA, i);
    }
    wr8(&t, REG_SEQ_CTL, SEQ_RUN | 0x02);
    wr8(&t, REG_HOST_CTL, HOST_IRQ_EN);

    /* Each present AL_PA: PLOGI, PRLI, then a LUN 0 INQUIRY. */
    ibuf = guest_alloc(alloc, 64);
    rsp = guest_alloc(alloc, RSP_SIZE);
    for (unsigned i = 0; i < map[0]; i++) {
        uint8_t alpa = map[1 + i];

        d = els(&t, 0x0f01 + 4 * i, alpa, 0x03, a);
        g_assert_cmphex(d.status, ==, DONE_OK);
        g_assert_cmphex(d.pass, ==, 0);
        g_assert_cmphex(a[0], ==, 0x02);
        d = els(&t, 0x0f02 + 4 * i, alpa, 0x20, a);
        g_assert_cmphex(d.status, ==, DONE_OK);
        g_assert_cmphex(a[0], ==, 0x02);
        g_assert_cmphex(a[0x13], ==, 0x12);
        seg = (Seg) { ibuf, 36 };
        aic_post(&t, 0x0f03 + 4 * i, alpa, 0, cdb, 36, &seg, 1, rsp);
        d = aic_wait_done(&t, true);
        g_assert_cmphex(d.status, ==, DONE_OK);
        qtest_memread(t.qts, ibuf, inq, sizeof(inq));
        /* A disk: the driver needs peripheral qualifier and type clear. */
        g_assert_cmphex(inq[0] & 0xc0, ==, 0);
    }
    aic_close(&t);
}

static void aic_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,id=fc0",
    };
    QOSGraphTestOptions rev1_opts = { .before = revision_setup };
    QOSGraphTestOptions romfile_opts = { .before = romfile_setup };
    QOSGraphTestOptions rombar0_opts = { .before = rombar0_setup };
    QOSGraphTestOptions wwn_opts = { .before = wwn_setup };
    QOSGraphTestOptions disk_opts = { .before = disk_setup };

    add_qpci_address(&opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(4, 0) });

    qos_node_create_driver("aic1160", aic_create);
    qos_node_consumes("aic1160", "sgi-pci-bus", &opts);
    qos_node_produces("aic1160", "pci-device");

    qos_add_test("identity", "aic1160", test_identity, NULL);
    qos_add_test("revision-property", "aic1160", test_revision_property,
                 &rev1_opts);
    qos_add_test("windows", "aic1160", test_windows, NULL);
    qos_add_test("romfile", "aic1160", test_romfile, &romfile_opts);
    qos_add_test("rombar0", "aic1160", test_rombar0, &rombar0_opts);
    qos_add_test("config-space", "aic1160", test_config_space, NULL);
    qos_add_test("config-collection", "aic1160", test_config_collection,
                 NULL);
    qos_add_test("reset-values", "aic1160", test_reset_values, NULL);
    qos_add_test("banks", "aic1160", test_banks, NULL);
    qos_add_test("chip-reset", "aic1160", test_chip_reset, NULL);
    qos_add_test("host-control", "aic1160", test_host_control, NULL);
    qos_add_test("microcode-load", "aic1160", test_microcode_load, NULL);
    qos_add_test("microcode-capacity", "aic1160", test_microcode_capacity,
                 NULL);
    qos_add_test("sequencer-memory", "aic1160", test_sequencer_memory, NULL);
    qos_add_test("interrupt", "aic1160", test_interrupt, NULL);
    qos_add_test("interrupt-mask", "aic1160", test_interrupt_mask, NULL);
    qos_add_test("loop-event", "aic1160", test_loop_event, NULL);
    qos_add_test("status-block", "aic1160", test_status_block, NULL);
    qos_add_test("seeprom-wwn", "aic1160", test_seeprom_wwn, &wwn_opts);
    qos_add_test("seeprom-default-wwn", "aic1160", test_seeprom_default_wwn,
                 NULL);
    qos_add_test("self-test-rams", "aic1160", test_self_test_rams, NULL);
    qos_add_test("seeprom-other", "aic1160", test_seeprom_other, NULL);
    qos_add_test("doorbell-not-running", "aic1160", test_doorbell_not_running,
                 NULL);
    qos_add_test("scsi-inquiry", "aic1160", test_scsi_inquiry, &disk_opts);
    qos_add_test("scsi-read", "aic1160", test_scsi_read, &disk_opts);
    qos_add_test("scsi-write", "aic1160", test_scsi_write, &disk_opts);
    qos_add_test("scsi-residual-and-sense", "aic1160",
                 test_scsi_residual_and_sense, &disk_opts);
    qos_add_test("scsi-residual-list", "aic1160", test_scsi_residual_list,
                 &disk_opts);
    qos_add_test("scsi-no-target", "aic1160", test_scsi_no_target,
                 &disk_opts);
    qos_add_test("done-queue-laps", "aic1160", test_done_queue_laps,
                 &disk_opts);
    qos_add_test("batch", "aic1160", test_batch, &disk_opts);
    qos_add_test("receive-buffers-held", "aic1160", test_receive_buffers_held,
                 &disk_opts);
    qos_add_test("lip-request", "aic1160", test_lip_request, NULL);
    qos_add_test("loop-bringup", "aic1160", test_loop_bringup, &disk_opts);
    qos_add_test("els-accepts", "aic1160", test_els_accepts, &disk_opts);
    qos_add_test("els-format", "aic1160", test_els_format, &disk_opts);
    qos_add_test("lip-map", "aic1160", test_lip_map, &disk_opts);
    qos_add_test("lip-needs-loop-image", "aic1160", test_lip_needs_loop_image,
                 &disk_opts);
    qos_add_test("unrecognised-tcb", "aic1160", test_unrecognised_tcb,
                 &disk_opts);
    qos_add_test("status-codes", "aic1160", test_status_codes, &disk_opts);
}

libqos_init(aic_register_nodes);
