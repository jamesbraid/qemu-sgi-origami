/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * XT-FC-2P two-port Fibre Channel XIO board: placement on the machines
 * that take it, the two controllers behind its Bridge, and their
 * interrupts reaching the Bridge.
 *
 * The controller's own register contract is in adaptec-aic1160-test.c.
 * This test holds the board to what the published hardware-graph paths
 * show: a Bridge with one controller on each of PCI devices 0 and 1 and
 * no other position open. See docs/system/mips/sgi/hardware/
 * adaptec-aic1160.rst.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sn0-user-image.h"
#include "sgi-test.h"
#include "sgi-launch.h"

#define AIC_ID_PAIR         0x11609004u
#define AIC_CLASS_REVISION  0x0c040002u

#define CFG0(base, dev)     ((base) + 0x020000 + (dev) * 0x1000)
#define BRIDGE_INT_STATUS   0x104
#define DEVICE_REG(dev)     (0x200 + 8 * (dev) + 4)

/* The Bridge's device windows: two 2 MB, then six 1 MB. */
static const uint64_t window_offset[] = { 0x200000, 0x400000 };
/* The chip's registers are I/O BAR 0; each device has an I/O window. */
#define PCI_IO(dev)         (0x100u + (dev) * 0x100u)

#define REG_SEQ_ADDR        0x02
#define REG_SEQ_DATA        0x14
#define REG_HOST_CTL        0x22
#define REG_INT_STATUS      0x24

/* The Octane's small windows to its widgets start here. */
#define OCTANE_XBOW         0x10000000ULL

#define OCTANE2_CPU "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900," \
    "reset-mode=0x1aa683"

typedef struct Fit {
    const char *name;
    /* The machine's arguments, and the board's place on it. */
    const char *machine;
    const char *board;
    /* Where the board's Bridge answers, or 0 to check the tree only. */
    uint64_t base;
    bool octane;
    /* The product the board is when none is named. */
    const char *product;
} Fit;

static const Fit fits[] = {
    {
        "origin2000", "-M origin2000 -display none",
        "-device sgi-fc-board,id=fc,slot=io2",
        SPACE_IO + ((uint64_t)12 << 24), false, "xt-fc-2p",
    }, {
        "octane", "-M octane -cpu R10000 -display none",
        "-device sgi-fc-board,id=fc,slot=widget-9",
        OCTANE_XBOW + ((uint64_t)9 << 24), true, "x9-fc-2p",
    }, {
        "octane2", "-M octane2 -cpu " OCTANE2_CPU " -display none",
        "-device sgi-fc-board,id=fc,slot=widget-10",
        OCTANE_XBOW + ((uint64_t)10 << 24), true, "x9-fc-2p",
    }, {
        /* No widget address is asserted: only that the board realizes. */
        "onyx2", "-M onyx2 -display none",
        "-device sgi-fc-board,id=fc,slot=io2", 0, false, "xt-fc-2p",
    },
};

/* The Octane machines want a boot image; an empty one is enough here. */
static QTestState *fit_start_board(const Fit *f, const char *board,
                                   const char *extra)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (f->octane) {
        g_autofree SN0Image *image = g_new0(SN0Image, 1);

        path = sn0_image_write(image, "xt-fc-2p");
        qts = qtest_initf("%s -bios %s -S %s %s", f->machine, path,
                          board, extra);
        unlink(path);
    } else {
        qts = qtest_initf("%s %s %s", f->machine, board, extra);
    }
    return qts;
}

static QTestState *fit_start(const Fit *f, const char *extra)
{
    return fit_start_board(f, f->board, extra);
}

static uint32_t rd32(QTestState *qts, uint64_t addr)
{
    return qtest_readl(qts, addr);
}

/* Narrow accesses aim at the byte lane the Bridge undoes. */
static uint64_t aim(uint64_t offset, unsigned size)
{
    return (offset & ~UINT64_C(3)) | ((offset & 3) ^ (4 - size));
}

static const char *port_path(unsigned port)
{
    return port ? "/machine/module0/fc/port1" : "/machine/module0/fc/port0";
}

static void raise_status(QTestState *qts, unsigned port, uint16_t bits)
{
    QDict *rsp = qtest_qmp(qts,
                           "{ 'execute': 'qom-set', 'arguments': {"
                           " 'path': %s, 'property': 'x-raise-status',"
                           " 'value': %u } }", port_path(port), bits);

    g_assert(!qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
}

static char *board_product(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts,
                           "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': '/machine/module0/fc',"
                           " 'property': 'product' } }");
    char *product;

    g_assert(qdict_haskey(rsp, "return"));
    product = g_strdup(qdict_get_str(rsp, "return"));
    qobject_unref(rsp);
    return product;
}

/* The product follows the machine unless one is named. */
static void test_default_product(gconstpointer data)
{
    const Fit *f = data;
    QTestState *qts = fit_start(f, "");
    g_autofree char *product = board_product(qts);

    g_assert_cmpstr(product, ==, f->product);
    qtest_quit(qts);
}

/* A named product replaces the machine's default; both have two ports. */
static void test_named_products(gconstpointer data)
{
    const Fit *f = data;
    QTestState *qts = fit_start_board(f, "-device sgi-fc-board,id=fc,"
                                         "slot=widget-10,product=xt-fc-2p",
                                      "");
    g_autofree char *product = board_product(qts);

    g_assert_cmpstr(product, ==, "xt-fc-2p");
    g_assert_cmphex(rd32(qts, CFG0(f->base, 0)), ==, AIC_ID_PAIR);
    g_assert_cmphex(rd32(qts, CFG0(f->base, 1)), ==, AIC_ID_PAIR);
    g_assert_cmphex(rd32(qts, CFG0(f->base, 2)), ==, 0xffffffff);
    qtest_quit(qts);
}

/* Both controllers exist in the board's tree, whichever machine it is on. */
static void test_ports_exist(gconstpointer data)
{
    const Fit *f = data;
    /* A disk placed on a port's loop bus, named after the board's id. */
    QTestState *qts = fit_start(f, "-drive id=d0,if=none,format=raw,"
                                   "file=null-co:// -device scsi-hd,"
                                   "bus=fc-p1,drive=d0,scsi-id=3");

    for (unsigned port = 0; port < 2; port++) {
        QDict *rsp = qtest_qmp(qts,
                               "{ 'execute': 'qom-get', 'arguments': {"
                               " 'path': %s, 'property': "
                               "'x-sequencer-running' } }", port_path(port));

        g_assert(qdict_haskey(rsp, "return"));
        qobject_unref(rsp);
    }
    qtest_quit(qts);
}

/* Devices 0 and 1 are controllers; no other position answers. */
static void test_identity(gconstpointer data)
{
    const Fit *f = data;
    QTestState *qts;

    qts = fit_start(f, "");
    for (unsigned dev = 0; dev < 2; dev++) {
        g_assert_cmphex(rd32(qts, CFG0(f->base, dev)), ==, AIC_ID_PAIR);
        g_assert_cmphex(rd32(qts, CFG0(f->base, dev) + 8), ==,
                        AIC_CLASS_REVISION);
        /* A BAR walk that stops at a zero BAR must get past BAR 1 and 2. */
        for (unsigned bar = 0; bar < 4; bar++) {
            qtest_writel(qts, CFG0(f->base, dev) + 0x10 + 4 * bar,
                         0xffffffff);
        }
        g_assert_cmphex(rd32(qts, CFG0(f->base, dev) + 0x10), ==, 0xffffff01);
        g_assert_cmphex(rd32(qts, CFG0(f->base, dev) + 0x14), ==, 0xfffff004);
        g_assert_cmphex(rd32(qts, CFG0(f->base, dev) + 0x18), ==, 0xffffffff);
        g_assert_cmphex(rd32(qts, CFG0(f->base, dev) + 0x1c), ==, 0xffffff01);
    }
    g_assert_cmphex(rd32(qts, CFG0(f->base, 2)), ==, 0xffffffff);
    qtest_quit(qts);
}

static void attach(QTestState *qts, const Fit *f, unsigned dev)
{
    uint64_t reg = f->base + DEVICE_REG(dev);

    /* I/O BAR 0 at a port in the device's window, with I/O decode on. */
    qtest_writel(qts, CFG0(f->base, dev) + 0x10, PCI_IO(dev));
    qtest_writel(qts, CFG0(f->base, dev) + PCI_COMMAND, 0x1);
    /* An I/O window: memory-space and window base bits clear. */
    qtest_writel(qts, reg, qtest_readl(qts, reg) &
                           ~(uint32_t)(DEV_MEMORY_SPACE | 0xfff));
}

/* Where a controller register is, aimed at its byte lane. */
static uint64_t io_reg(const Fit *f, unsigned dev, unsigned reg,
                       unsigned size)
{
    return f->base + window_offset[dev] + aim(PCI_IO(dev) + reg, size);
}

static uint16_t reg_rd16(QTestState *qts, const Fit *f, unsigned dev,
                         unsigned reg)
{
    return qtest_readw(qts, io_reg(f, dev, reg, 2));
}

static void reg_wr16(QTestState *qts, const Fit *f, unsigned dev,
                     unsigned reg, uint16_t val)
{
    qtest_writew(qts, io_reg(f, dev, reg, 2), val);
}

/*
 * An Octane's large window reaches the board's Bridge as its small window
 * does: widget << 36 is the processor's 64 GiB view of the widget.
 */
static void test_large_window(gconstpointer data)
{
    const Fit *f = data;
    uint64_t large = ((f->base - OCTANE_XBOW) >> 24) << 36;
    QTestState *qts = fit_start(f, "");

    for (unsigned int dev = 0; dev < 2; dev++) {
        g_assert_cmphex(rd32(qts, large + 0x020000 + dev * 0x1000), ==,
                        AIC_ID_PAIR);
    }
    g_assert_cmphex(rd32(qts, large + 0x020000 + 2 * 0x1000), ==, 0xffffffff);
    qtest_quit(qts);
}

/* Each controller's registers are its own, reached through the Bridge. */
static void test_registers(gconstpointer data)
{
    const Fit *f = data;
    QTestState *qts;

    qts = fit_start(f, "");
    attach(qts, f, 0);
    attach(qts, f, 1);

    reg_wr16(qts, f, 0, REG_SEQ_ADDR, 0x0123);
    reg_wr16(qts, f, 1, REG_SEQ_ADDR, 0x0456);
    g_assert_cmphex(reg_rd16(qts, f, 0, REG_SEQ_ADDR), ==, 0x0123);
    g_assert_cmphex(reg_rd16(qts, f, 1, REG_SEQ_ADDR), ==, 0x0456);

    reg_wr16(qts, f, 1, REG_SEQ_ADDR, 0);
    reg_wr16(qts, f, 1, REG_SEQ_DATA, 0xc0de);
    reg_wr16(qts, f, 1, REG_SEQ_ADDR, 0);
    g_assert_cmphex(reg_rd16(qts, f, 1, REG_SEQ_DATA), ==, 0xc0de);
    qtest_quit(qts);
}

/*
 * The IP30 kernel probes the flash window of every PCI-mode Bridge: it sets
 * Control bit 31, stores 0x9090 at the first select, reads halfwords 0 and
 * 2, then tries the second select, with no fault handler. The reads must
 * complete on the board, whose Bridge has no part fitted. The store has no
 * part to land in and its result is not checked: the guest does not see it.
 */
static void test_flash_probe(gconstpointer data)
{
    const Fit *f = data;
    QTestState *qts = fit_start(f, MEMTX_TEST_DEVICE);
    uint32_t control = qtest_readl(qts, f->base + 0x20 + 4);

    for (uint64_t select = 0xc00000; select <= 0xe00000; select += 0x200000) {
        uint64_t at = f->base + select;
        uint16_t half;

        qtest_writel(qts, f->base + 0x20 + 4, control | 0x80000000u);
        memtx_writew(qts, at, 0x9090);
        g_assert_cmpuint(memtx_readw(qts, at, &half), ==, MEMTX_OK);
        g_assert_cmphex(half, ==, 0xffff);
        g_assert_cmpuint(memtx_readw(qts, at + 2, &half), ==, MEMTX_OK);
        g_assert_cmphex(half, ==, 0xffff);
        qtest_writel(qts, f->base + 0x20 + 4, control);
    }
    qtest_quit(qts);
}

/* A controller's interrupt reaches the Bridge on the pin of its device. */
static void test_interrupt_pins(gconstpointer data)
{
    const Fit *f = data;
    QTestState *qts;

    qts = fit_start(f, "");
    attach(qts, f, 0);
    attach(qts, f, 1);
    /* The line needs the master interrupt enable, host control bit 2. */
    for (unsigned dev = 0; dev < 2; dev++) {
        qtest_writeb(qts, io_reg(f, dev, REG_HOST_CTL, 1), 0x04);
    }
    g_assert_cmphex(rd32(qts, f->base + BRIDGE_INT_STATUS) & 3, ==, 0);

    raise_status(qts, 0, 0x1000);
    g_assert_cmphex(rd32(qts, f->base + BRIDGE_INT_STATUS) & 3, ==, 1);
    raise_status(qts, 1, 0x1000);
    g_assert_cmphex(rd32(qts, f->base + BRIDGE_INT_STATUS) & 3, ==, 3);

    /* The driver clears what it handled with a byte write-back. */
    qtest_writeb(qts, io_reg(f, 0, REG_INT_STATUS + 1, 1), 0x10);
    g_assert_cmphex(rd32(qts, f->base + BRIDGE_INT_STATUS) & 3, ==, 2);
    qtest_quit(qts);
}

static void test_unknown_product(void)
{
    const char *const args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-display", "none",
        "-accel", "qtest", "-qmp", "stdio",
        "-device", "sgi-fc-board,slot=io2,product=x9-fc-2popt-b", NULL,
    };

    assert_launch_rejected(args, "unknown Fibre Channel board product");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sgi-fc-board/unknown-product", test_unknown_product);
    qtest_add_data_func("sgi-fc-board/octane2/named-products", &fits[2],
                        test_named_products);

    for (int i = 0; i < ARRAY_SIZE(fits); i++) {
        const Fit *f = &fits[i];
        g_autofree char *path = NULL;

        path = g_strdup_printf("sgi-fc-board/%s/default-product", f->name);
        qtest_add_data_func(path, f, test_default_product);
        path = g_strdup_printf("sgi-fc-board/%s/ports-exist", f->name);
        qtest_add_data_func(path, f, test_ports_exist);
        /* No widget address is asserted for a machine without one. */
        if (!f->base) {
            continue;
        }
        path = g_strdup_printf("sgi-fc-board/%s/identity", f->name);
        qtest_add_data_func(path, f, test_identity);
        if (f->octane) {
            path = g_strdup_printf("sgi-fc-board/%s/large-window", f->name);
            qtest_add_data_func(path, f, test_large_window);
        }
        path = g_strdup_printf("sgi-fc-board/%s/registers", f->name);
        qtest_add_data_func(path, f, test_registers);
        path = g_strdup_printf("sgi-fc-board/%s/interrupt-pins", f->name);
        qtest_add_data_func(path, f, test_interrupt_pins);
        path = g_strdup_printf("sgi-fc-board/%s/flash-probe", f->name);
        qtest_add_data_func(path, f, test_flash_probe);
    }
    return g_test_run();
}
