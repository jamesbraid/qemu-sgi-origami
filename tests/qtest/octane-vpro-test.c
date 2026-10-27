/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The VPro (Odyssey) board in the Octane graphics slot. An Octane IRIX
 * inventory lists its VPro board under xtalk/11, the widget the SI board
 * also takes, so the processor reaches the board's widget registers through
 * the Crossbow's widget 11 windows and the board's interrupt packets reach
 * HEART through the Crossbow.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define XBOW 0x10000000ULL
#define VPRO_SHORT 0x1b000000ULL
#define VPRO_LONG 0xb000000000ULL
#define HEART 0x0ff00000ULL
#define HEART_PENDING 0x10030
#define VPRO_WIDGET 11
#define VPRO_PATH "/machine/module0/widget-11"
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

/* The widget identification register, in its slot's data half. */
#define ODSY_IDENTITY 0x0004
#define ODSY_MEMORY_CONFIG 0x106c
#define ODSY_CONTROL 0x0024
/* The routed destination and vector table; the flag source has byte 7:0. */
#define ODSY_ROUTE_DEST_UPPER 0x1044
#define ODSY_ROUTE_DEST_LOWER 0x104c
#define ODSY_ROUTE_VECTORS 0x1054
#define ODSY_INTR_CLEAR 0x101c
#define ODSY_INTR_ENABLE_SET 0x1034
#define ODSY_INTR_FIFO_LOW_WATER 0x00000200
/* Revision 0xa, part 0xc013, manufacturer 0x023. */
#define ODSY_IDENTITY_DEFAULT 0xac013047
#define ODSY_CONTROL_WIDGET_MASK 0xf

/* Each Crossbow link's status register, in its slot's data half. */
#define XBOW_LINK_STATUS(w) (XBOW + 0x100 + ((w) - 8) * 0x40 + 0x10 + 4)
#define XBOW_LINK_ALIVE (1u << 31)

/*
 * HEART takes an interrupt packet as an eight-byte write at 0x80 of its
 * widget 8: the vector in bits 7:0, bit 8 to raise it.
 */
#define HEART_WIDGET 8
#define HEART_PACKET 0x80
#define TEST_VECTOR 18
#define ROUTE_DEST_UPPER (HEART_WIDGET << 25)

static char *flash_path;

/*
 * Start an Octane or Octane2 with a VPro of @product in port 11, or none
 * for NULL. The catalogue offers only the V12, and only on the Octane2,
 * whose Crossbow and supply the upgrade guide allows; every other case is
 * a register contract of the board, so it skips the catalogue.
 */
static QTestState *start(bool octane2, const char *product)
{
    g_autofree char *quoted = g_shell_quote(flash_path);
    bool offered = octane2 && product && g_str_equal(product, "v12");
    g_autofree char *graphics = product ?
        g_strdup_printf("-device sgi-odyssey,slot=widget-11,product=%s",
                        product) : g_strdup("-vga none");

    return qtest_initf("-M %s%s -m %uM -cpu %s -bios %s -S %s",
                       octane2 ? "octane2" : "octane",
                       offered || !product ? "" :
                       ",x-uncatalogued-graphics=on",
                       octane2 ? 256 : 128,
                       octane2 ? OCTANE2_CPU : "R10000", quoted, graphics);
}

static const char *qom_type(QTestState *qts, const char *path)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'type'}}",
        path);
    static char type[64];

    if (!qdict_haskey(response, "return")) {
        return NULL;
    }
    g_strlcpy(type, qdict_get_str(response, "return"), sizeof(type));
    return type;
}

/* The board answers as Odyssey at widget 11 through both windows. */
static void test_widget_identity(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), "v12");

    g_assert_cmpstr(qom_type(qts, VPRO_PATH), ==, "sgi-odyssey");
    g_assert_cmphex(qtest_readl(qts, VPRO_SHORT + ODSY_IDENTITY), ==,
                    ODSY_IDENTITY_DEFAULT);
    g_assert_cmphex(qtest_readl(qts, VPRO_LONG + ODSY_IDENTITY), ==,
                    ODSY_IDENTITY_DEFAULT);
    g_assert_cmphex(qtest_readl(qts, VPRO_SHORT + ODSY_CONTROL) &
                    ODSY_CONTROL_WIDGET_MASK, ==, VPRO_WIDGET);
    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_STATUS(VPRO_WIDGET)) &
                    XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
    qtest_quit(qts);
}

typedef struct VProProfileCase {
    const char *name;
    uint32_t revision;
    uint32_t size_code;
    bool octane2;
} VProProfileCase;

static const VProProfileCase profile_cases[] = {
    { "v6", 2, 0, false }, { "v8", 2, 2, false },
    { "v10", 0xa, 0, false }, { "v12", 0xa, 2, false },
    { "v6", 2, 0, true }, { "v8", 2, 2, true },
    { "v10", 0xa, 0, true }, { "v12", 0xa, 2, true },
};

/* Each marketed profile keeps its reviewed generation and capacity class. */
static void test_profile_identity_and_routing(gconstpointer data)
{
    const VProProfileCase *profile = data;
    QTestState *qts = start(profile->octane2, profile->name);
    uint32_t identity = qtest_readl(qts, VPRO_SHORT + ODSY_IDENTITY);
    uint32_t memory = qtest_readl(qts, VPRO_SHORT + ODSY_MEMORY_CONFIG);
    uint64_t bit = UINT64_C(1) << TEST_VECTOR;

    g_assert_cmpstr(qom_type(qts, VPRO_PATH), ==, "sgi-odyssey");
    g_assert_cmphex(identity >> 28, ==, profile->revision);
    g_assert_cmphex(identity & 0x0fffffff,
                    ==, ODSY_IDENTITY_DEFAULT & 0x0fffffff);
    g_assert_cmphex(qtest_readl(qts, VPRO_LONG + ODSY_IDENTITY), ==,
                    identity);
    g_assert_cmphex((memory >> 30) & 3, ==, profile->size_code);
    g_assert_cmphex(qtest_readl(qts, VPRO_SHORT + ODSY_CONTROL) &
                    ODSY_CONTROL_WIDGET_MASK, ==, VPRO_WIDGET);
    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_STATUS(VPRO_WIDGET)) &
                    XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);

    qtest_writel(qts, VPRO_SHORT + ODSY_ROUTE_DEST_UPPER, ROUTE_DEST_UPPER);
    qtest_writel(qts, VPRO_SHORT + ODSY_ROUTE_DEST_LOWER, HEART_PACKET);
    qtest_writel(qts, VPRO_SHORT + ODSY_ROUTE_VECTORS, TEST_VECTOR);
    qtest_writel(qts, VPRO_SHORT + ODSY_INTR_ENABLE_SET,
                 ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, HEART + HEART_PENDING) & bit, ==, bit);
    qtest_writel(qts, VPRO_SHORT + ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, HEART + HEART_PENDING) & bit, ==, 0);
    qtest_quit(qts);
}

/* Without a graphics board the slot's link stays down. */
static void test_absent(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), NULL);

    g_assert_null(qom_type(qts, VPRO_PATH));
    g_assert_cmphex(qtest_readl(qts, XBOW_LINK_STATUS(VPRO_WIDGET)) &
                    XBOW_LINK_ALIVE, ==, 0);
    qtest_quit(qts);
}

/*
 * A raised and a lowered board interrupt each travel as a packet through
 * the Crossbow to HEART, setting and clearing the vector's pending bit.
 */
static void test_interrupt_reaches_heart(gconstpointer data)
{
    QTestState *qts = start(GPOINTER_TO_INT(data), "v12");
    uint64_t bit = UINT64_C(1) << TEST_VECTOR;

    qtest_writel(qts, VPRO_SHORT + ODSY_ROUTE_DEST_UPPER, ROUTE_DEST_UPPER);
    qtest_writel(qts, VPRO_SHORT + ODSY_ROUTE_DEST_LOWER, HEART_PACKET);
    qtest_writel(qts, VPRO_SHORT + ODSY_ROUTE_VECTORS, TEST_VECTOR);
    g_assert_cmphex(qtest_readq(qts, HEART + HEART_PENDING) & bit, ==, 0);

    /* The FIFO is empty, so its low-water flag is already pending. */
    qtest_writel(qts, VPRO_SHORT + ODSY_INTR_ENABLE_SET,
                 ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, HEART + HEART_PENDING) & bit, ==, bit);

    qtest_writel(qts, VPRO_SHORT + ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, HEART + HEART_PENDING) & bit, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    const struct {
        const char *name;
        GTestDataFunc test;
    } tests[] = {
        { "widget-identity", test_widget_identity },
        { "absent", test_absent },
        { "interrupt-reaches-heart", test_interrupt_reaches_heart },
    };
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-vpro-prom-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    for (unsigned i = 0; i < G_N_ELEMENTS(profile_cases); i++) {
        const VProProfileCase *profile = &profile_cases[i];
        const char *machine = profile->octane2 ? "octane2" : "octane";
        g_autofree char *path = g_strdup_printf(
            "/%s/vpro/%s-profile", machine, profile->name);

        qtest_add_data_func(path, (gpointer)profile,
                            test_profile_identity_and_routing);
    }
    for (unsigned profile = 0; profile < 2; profile++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tests); i++) {
            g_autofree char *path = g_strdup_printf("/%s/vpro/%s",
                profile ? "octane2" : "octane", tests[i].name);

            qtest_add_data_func(path, GINT_TO_POINTER(profile), tests[i].test);
        }
    }
    result = g_test_run();
    g_assert_cmpint(unlink(flash_path), ==, 0);
    g_free(flash_path);
    return result;
}
