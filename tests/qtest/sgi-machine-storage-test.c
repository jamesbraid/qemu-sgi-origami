/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Attach each SGI machine's writable stores as block nodes through the
 * machine property named for the catalogue's storage item, and check that a
 * store persists across QEMU processes, that a wrongly sized or unknown
 * backing is refused, and that the legacy -drive forms still work.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-launch.h"

#define SN0_FLASH_SIZE (1024 * 1024)
#define SN0_FLASH_BASE UINT64_C(0x1fc00000)
#define SN0_FLASH_MARK 0x100
#define SN0_FLASH_PROBE 0x8000

#define O2K_DESKSIDE "origin2000,topology=origin2000-deskside,nodes=2"
#define O2K_DESKSIDE_SIZE "-smp 4 -m 512M"
#define O200_GIGACHANNEL "origin200,topology=origin200-gigachannel"

/* Origin 300's 16 MiB two-position flash and the PROM image -bios takes. */
#define SN1_FLASH_SIZE (16 * 1024 * 1024)
#define SN1_PROM_SIZE 0x1686a8
#define SN1_PROM_RESET UINT64_C(0x1fc00000)
#define SN1_FLASH_MARK 0x2000
#define SN1_MACHINE "-cpu R14000 -smp 2"

/* A module system controller's configuration NVRAM. */
#define MODULE_NVRAM_SIZE 2048

/* The m48t35 Timekeeper's NVRAM and the clock record kept beside it. */
#define NVRAM_SIZE (32 * 1024)
#define NVRAM_CLOCK_SIZE 16

static char *directory;

static char *store_path(const char *name)
{
    return g_strdup_printf("%s/%s.raw", directory, name);
}

/* An erased store of @size bytes with @mark at SN0_FLASH_MARK. */
static char *make_store(const char *name, size_t size, uint8_t mark)
{
    g_autofree char *bytes = g_malloc(size);
    char *path = store_path(name);

    memset(bytes, 0xff, size);
    if (size > SN0_FLASH_MARK) {
        bytes[SN0_FLASH_MARK] = mark;
    }
    g_assert_true(g_file_set_contents(path, bytes, size, NULL));
    return path;
}

static uint8_t file_byte(const char *path, size_t offset)
{
    g_autofree char *bytes = NULL;
    gsize length;

    g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    g_assert_cmpuint(offset, <, length);
    return bytes[offset];
}

/* "-blockdev ..." for a raw file node named @node. */
static char *blockdev(const char *node, const char *path)
{
    return g_strdup_printf("driver=raw,node-name=%s,file.driver=file,"
                           "file.filename=%s", node, path);
}

static uint8_t sn0_flash_read(QTestState *qts, unsigned int cpu,
                              uint32_t offset)
{
    uint8_t value;

    g_assert_true(qtest_memread_cpu(qts, cpu, SN0_FLASH_BASE + offset,
                                    &value, sizeof(value)));
    return value;
}

static void sn0_flash_write(QTestState *qts, unsigned int cpu,
                            uint32_t offset, uint8_t value)
{
    g_assert_true(qtest_memwrite_cpu(qts, cpu, SN0_FLASH_BASE + offset,
                                     &value, sizeof(value)));
}

/* The AMD program sequence the node's Am29F080-class part answers. */
static void sn0_flash_program(QTestState *qts, unsigned int cpu,
                              uint32_t offset, uint8_t value)
{
    sn0_flash_write(qts, cpu, 0x555, 0xaa);
    sn0_flash_write(qts, cpu, 0x2aa, 0x55);
    sn0_flash_write(qts, cpu, 0x555, 0xa0);
    sn0_flash_write(qts, cpu, offset, value);
}

static QTestState *start_origin200_flash(const char *path)
{
    g_autofree char *node = blockdev("flash", path);

    return qtest_initf("-M origin200,node0-flash=flash -blockdev %s", node);
}

/*
 * The node flash reads its file, not a seeded image; a programmed byte
 * reaches the file and the next QEMU process reads it back.
 */
static void test_sn0_node_flash_persists(void)
{
    g_autofree char *path = make_store("o200-node0", SN0_FLASH_SIZE, 0x69);
    QTestState *qts = start_origin200_flash(path);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x69);
    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_PROBE), ==, 0xff);
    sn0_flash_program(qts, 0, SN0_FLASH_PROBE, 0x5a);
    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_PROBE), ==, 0x5a);
    qtest_quit(qts);

    g_assert_cmphex(file_byte(path, SN0_FLASH_PROBE), ==, 0x5a);
    qts = start_origin200_flash(path);
    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_PROBE), ==, 0x5a);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/* Each node's property backs that node's own part. */
static void test_sn0_node_flash_per_node(void)
{
    g_autofree char *path0 = make_store("o2k-node0", SN0_FLASH_SIZE, 0x10);
    g_autofree char *path1 = make_store("o2k-node1", SN0_FLASH_SIZE, 0x11);
    g_autofree char *node0 = blockdev("n0", path0);
    g_autofree char *node1 = blockdev("n1", path1);
    QTestState *qts = qtest_initf(
        "-M " O2K_DESKSIDE ",node0-flash=n0,node1-flash=n1 "
        O2K_DESKSIDE_SIZE " "
        "-blockdev %s -blockdev %s", node0, node1);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x10);
    g_assert_cmphex(sn0_flash_read(qts, 2, SN0_FLASH_MARK), ==, 0x11);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path0), ==, 0);
    g_assert_cmpint(g_unlink(path1), ==, 0);
}

/*
 * With some node flash bound, -bios fills the rest: node 1 holds the -bios
 * payload while node 0 reads its own bound file.
 */
static void test_sn0_node_flash_partly_bound(void)
{
    g_autofree char *path0 = make_store("o2k-part0", SN0_FLASH_SIZE, 0x10);
    g_autofree char *prom = make_store("o2k-part-prom", 4096, 0x33);
    g_autofree char *node0 = blockdev("n0", path0);
    QTestState *qts = qtest_initf(
        "-M " O2K_DESKSIDE ",node0-flash=n0 " O2K_DESKSIDE_SIZE " "
        "-blockdev %s -bios %s", node0, prom);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x10);
    g_assert_cmphex(sn0_flash_read(qts, 2, SN0_FLASH_MARK), ==, 0x33);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path0), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/*
 * The same binding without -bios is refused: node 1 would boot a flash
 * with no PROM in it.
 */
static void test_sn0_node_flash_partly_bound_needs_bios(void)
{
    g_autofree char *path0 = make_store("o2k-part1", SN0_FLASH_SIZE, 0x10);
    g_autofree char *node0 = blockdev("n0", path0);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", O2K_DESKSIDE ",node0-flash=n0",
        "-smp", "4", "-m", "512M", "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", node0, NULL,
    };

    assert_launch_rejected(args, "origin2000: 1 of 2 boot flashes are bound; "
                           "pass -bios to fill the rest, or bind "
                           "node<N>-flash for every node");
    g_assert_cmpint(g_unlink(path0), ==, 0);
}

/* The legacy pflash drive still backs the node flash. */
static void test_sn0_node_flash_legacy_drive(void)
{
    g_autofree char *path = make_store("o200-legacy", SN0_FLASH_SIZE, 0x42);
    QTestState *qts = qtest_initf("-M origin200 "
                                  "-drive if=pflash,file=%s,format=raw",
                                  path);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x42);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void assert_rejected(const char *machine, const char *extra_option,
                            const char *extra_value, const char *message)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-display", "none", "-S",
        "-qmp", "stdio", extra_option, extra_value, NULL,
    };

    assert_launch_rejected(args, message);
}

static void test_sn0_node_flash_refusals(void)
{
    g_autofree char *small = make_store("o200-small", SN0_FLASH_SIZE / 2, 0);
    g_autofree char *right = make_store("o200-right", SN0_FLASH_SIZE, 0);
    g_autofree char *small_node = blockdev("flash", small);
    g_autofree char *right_node = blockdev("flash", right);
    g_autofree char *legacy = g_strdup_printf("if=pflash,file=%s,format=raw",
                                              right);
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,node0-flash=flash",
        "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", right_node, "-drive", legacy, NULL,
    };
    /* With the only boot flash bound, -bios has nothing to fill. */
    const char *bios[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,node0-flash=flash",
        "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", right_node, "-bios", small, NULL,
    };

    assert_rejected("origin200,node0-flash=flash", "-blockdev", small_node,
                    "origin200: node0-flash is 524288 bytes; it must be "
                    "exactly 1048576");
    assert_rejected("origin200,node0-flash=missing", "-blockdev", right_node,
                    "origin200: node0-flash names 'missing', which is no "
                    "block node or drive");
    assert_rejected("origin200,node1-flash=flash", "-blockdev", right_node,
                    "origin200: topology 'origin200' with 1 node board has "
                    "no node1-flash");
    assert_launch_rejected(both, "node0-flash and -drive if=pflash,unit=0 "
                           "both back node 0's flash");
    assert_launch_rejected(bios, "origin200: -bios has no unbound boot flash "
                           "to fill; the bound boot flash already holds the "
                           "PROM");
    g_assert_cmpint(g_unlink(small), ==, 0);
    g_assert_cmpint(g_unlink(right), ==, 0);
}

/*
 * "-M <machine>,help" lists the storage properties, as it lists io-prom,
 * and Octane's own machine properties too.
 */
static void test_machine_help_lists_storage(void)
{
    static const char *const expected[][2] = {
        { "origin200", "node0-flash=" },
        { "origin200", "module0-nvram=" },
        { "origin2000", "module0-nvram=" },
        { "onyx2", "module0-nvram=" },
        { "origin2000", "io0-flash=" },
        { "onyx2", "nvram0-clock=" },
        { "origin300", "node1-flash=" },
        { "fuel", "nvram0=" },
        { "octane", "node0-flash=" },
        { "octane", "nvram0-clock=" },
        { "octane", "x-uncatalogued-graphics=" },
        { "octane", "bank3-size=" },
        { "octane", "flash-strap=" },
        { "octane", "topology=" },
        { "octane", "nodes=" },
        { "octane2", "nvram0=" },
        { "octane2", "bank0-size=" },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(expected); i++) {
        g_autofree char *machine = g_strdup_printf("%s,help", expected[i][0]);
        const char *argv[] = {
            qtest_qemu_binary(NULL), "-M", machine, NULL,
        };
        g_autofree char *out = NULL;
        g_autoptr(GError) error = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                                   G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                                   &out, NULL, &status, &error));
        g_assert_no_error(error);
        g_assert_true(g_spawn_check_exit_status(status, NULL));
        g_assert_nonnull(strstr(out, expected[i][1]));
    }
}

/* Whether type @type has a property named @name. */
static bool type_has_property(QTestState *qts, const char *type,
                              const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute': 'qom-list-properties', 'arguments': {'typename': %s}}",
        type);
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(property, "name"), name)) {
            return true;
        }
    }
    return false;
}

/*
 * Every override an offering lists is a property of its machine or of its
 * processor part's CPU type, so a launcher can take the list instead of
 * keeping its own.
 */
static void test_overrides_name_properties(void)
{
    QTestState *qts = qtest_init("-M none");
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute': 'query-sgi-machines'}");
    QList *offerings = qdict_get_qlist(qdict_get_qdict(reply, "return"),
                                       "offerings");
    const QListEntry *entry, *item;
    unsigned int checked = 0;

    QLIST_FOREACH_ENTRY(offerings, entry) {
        QDict *offering = qobject_to(QDict, qlist_entry_obj(entry));
        g_autofree char *machine = g_strdup_printf(
            "%s-machine", qdict_get_str(offering, "product"));
        g_autofree char *cpu = g_strdup_printf(
            "%s-mips64-cpu", qdict_get_str(offering, "default-cpu-model"));

        QLIST_FOREACH_ENTRY(qdict_get_qlist(offering, "overrides"), item) {
            QDict *override = qobject_to(QDict, qlist_entry_obj(item));
            const char *name = qdict_get_str(override, "property");
            bool on_cpu = g_str_equal(qdict_get_str(override, "target"),
                                      "cpu");

            if (!type_has_property(qts, on_cpu ? cpu : machine, name)) {
                g_error("%s lists %s, which %s lacks",
                        qdict_get_str(offering, "machine-options"), name,
                        on_cpu ? cpu : machine);
            }
            checked++;
        }
    }
    g_assert_cmpuint(checked, >, 0);
    qtest_quit(qts);
}

/* One block node cannot back two stores. */
static void test_shared_node_refused(void)
{
    g_autofree char *path = make_store("shared", SN0_FLASH_SIZE, 0);
    g_autofree char *node = blockdev("flash", path);

    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "origin2000,topology=origin2000-deskside,nodes=2,"
        "node0-flash=flash,node1-flash=flash", "-smp", "4", "-m", "512M",
        "-display", "none", "-S", "-qmp", "stdio", "-blockdev", node, NULL,
    };

    assert_launch_rejected(args, "origin2000: node0-flash and node1-flash "
                           "both name 'flash'");
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/* A zero-filled stand-in for the IP35 PROM -bios requires. */
static char *make_sn1_prom(void)
{
    g_autofree char *bytes = g_malloc0(SN1_PROM_SIZE);
    char *path = store_path("ip35-prom");

    g_assert_true(g_file_set_contents(path, bytes, SN1_PROM_SIZE, NULL));
    return path;
}

/* A 16 MiB flash image with @mark at SN1_FLASH_MARK of position zero. */
static char *make_sn1_flash(const char *name, size_t size, uint8_t mark)
{
    g_autofree char *bytes = g_malloc(size);
    char *path = store_path(name);

    memset(bytes, 0xff, size);
    bytes[SN1_FLASH_MARK] = mark;
    g_assert_true(g_file_set_contents(path, bytes, size, NULL));
    return path;
}

/* The reset view answers from the bound flash, with no -bios. */
static void test_sn1_node_flash_bound(void)
{
    g_autofree char *path = make_sn1_flash("o300-node0", SN1_FLASH_SIZE,
                                           0x35);
    g_autofree char *node = blockdev("flash", path);
    QTestState *qts = qtest_initf("-M origin300,node0-flash=flash "
                                  SN1_MACHINE " -blockdev %s", node);

    g_assert_cmphex(qtest_readb(qts, SN1_PROM_RESET + SN1_FLASH_MARK), ==,
                    0x35);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/* A protocol node backs the flash as well as a raw one, as on the Hub. */
static void test_sn1_node_flash_protocol_node(void)
{
    g_autofree char *path = make_sn1_flash("o300-file", SN1_FLASH_SIZE,
                                           0x36);
    QTestState *qts = qtest_initf("-M origin300,node0-flash=flash "
                                  SN1_MACHINE " -blockdev "
                                  "driver=file,node-name=flash,filename=%s",
                                  path);

    g_assert_cmphex(qtest_readb(qts, SN1_PROM_RESET + SN1_FLASH_MARK), ==,
                    0x36);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_sn1_node_flash_legacy_drive(void)
{
    g_autofree char *path = make_sn1_flash("o300-legacy", SN1_FLASH_SIZE,
                                           0x53);
    QTestState *qts = qtest_initf("-M origin300 " SN1_MACHINE
                                  " -drive if=pflash,file=%s,format=raw",
                                  path);

    g_assert_cmphex(qtest_readb(qts, SN1_PROM_RESET + SN1_FLASH_MARK), ==,
                    0x53);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_sn1_node_flash_refusals(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *small = make_sn1_flash("o300-small", SN1_FLASH_SIZE / 2,
                                            0);
    g_autofree char *right = make_sn1_flash("o300-right", SN1_FLASH_SIZE, 0);
    g_autofree char *small_node = blockdev("flash", small);
    g_autofree char *right_node = blockdev("flash", right);
    g_autofree char *legacy = g_strdup_printf("if=pflash,file=%s,format=raw",
                                              right);
    const char *wrong_size[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", small_node, NULL,
    };
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", right_node, "-drive", legacy,
        NULL,
    };
    g_autofree char *stray = g_strdup_printf("if=pflash,unit=1,file=%s,"
                                             "format=raw", right);
    const char *extra[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", right_node, "-drive", stray,
        NULL,
    };
    /* With the one boot flash bound, -bios has nothing to fill. */
    const char *bios[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-bios", prom, "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", right_node, NULL,
    };

    assert_launch_rejected(wrong_size, "origin300: node0-flash is 8388608 "
                           "bytes; it must be exactly 16777216");
    assert_launch_rejected(both, "origin300: node0-flash and -drive "
                           "if=pflash,unit=0 both back node 0's flash");
    assert_launch_rejected(extra, "origin300 accepts one 16 MiB pflash "
                           "image per node");
    assert_launch_rejected(bios, "origin300: -bios has no unbound boot flash "
                           "to fill; the bound boot flash already holds the "
                           "PROM");
    {
        const char *nothing[] = {
            qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
            "-smp", "2", "-display", "none", "-S", "-qmp", "stdio", NULL,
        };

        assert_launch_rejected(nothing, "origin300: no boot PROM: pass -bios, "
                               "or bind node<N>-flash for every node");
    }
    g_assert_cmpint(g_unlink(small), ==, 0);
    g_assert_cmpint(g_unlink(right), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/*
 * How many block backends with node or drive id @node are attached to a
 * device whose canonical path ends in @suffix.
 */
static unsigned int attached_count(QTestState *qts, const char *node,
                                   const char *suffix)
{
    QDict *response = qtest_qmp(qts, "{'execute': 'query-block'}");
    QList *blocks = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    unsigned int count = 0;

    QLIST_FOREACH_ENTRY(blocks, entry) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));
        QDict *inserted = qdict_get_qdict(block, "inserted");

        if (qdict_haskey(block, "qdev") && inserted &&
            g_str_has_suffix(qdict_get_str(block, "qdev"), suffix) &&
            (g_str_equal(qdict_get_str(block, "device"), node) ||
             g_str_equal(qdict_get_str(inserted, "node-name"), node))) {
            count++;
        }
    }
    qobject_unref(response);
    return count;
}

/*
 * The canonical path of the device a block backend whose node is @node, or
 * whose drive id is @node, is attached to; NULL when none is.
 */
static char *attached_device(QTestState *qts, const char *node)
{
    QDict *response = qtest_qmp(qts, "{'execute': 'query-block'}");
    QList *blocks = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    char *path = NULL;

    QLIST_FOREACH_ENTRY(blocks, entry) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));
        QDict *inserted = qdict_get_qdict(block, "inserted");

        if (!qdict_haskey(block, "qdev") || !inserted) {
            continue;
        }
        if (g_str_equal(qdict_get_str(block, "device"), node) ||
            g_str_equal(qdict_get_str(inserted, "node-name"), node)) {
            path = g_strdup(qdict_get_str(block, "qdev"));
            break;
        }
    }
    qobject_unref(response);
    return path;
}

static void assert_attached_to(QTestState *qts, const char *node,
                               const char *suffix)
{
    g_autofree char *path = attached_device(qts, node);

    g_assert_nonnull(path);
    g_assert_true(g_str_has_suffix(path, suffix));
}

/* The io0-flash binding reaches the BaseIO card's and cabinet's part. */
static void test_io_flash_bound(void)
{
    g_autofree char *path = make_store("io0", SN0_FLASH_SIZE, 0x06);
    g_autofree char *node = blockdev("io0", path);
    QTestState *qts = qtest_initf("-M " O2K_DESKSIDE ",io0-flash=io0 "
                                  O2K_DESKSIDE_SIZE " -blockdev %s", node);

    assert_attached_to(qts, "io0", "/flash");
    qtest_quit(qts);

    qts = qtest_initf("-M " O200_GIGACHANNEL ",io0-flash=io0 -blockdev %s",
                      node);
    assert_attached_to(qts, "io0", "io7/flash");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_io_flash_legacy_drive(void)
{
    g_autofree char *path = make_store("io0-legacy", SN0_FLASH_SIZE, 0x06);
    QTestState *qts = qtest_initf(
        "-M " O2K_DESKSIDE " " O2K_DESKSIDE_SIZE " -drive if=none,"
        "id=sgi-baseio-flash0,file=%s,format=raw", path);

    assert_attached_to(qts, "sgi-baseio-flash0", "/flash");
    qtest_quit(qts);

    qts = qtest_initf("-M " O200_GIGACHANNEL " -drive if=none,"
                      "id=sgi-gigachannel-flash0,file=%s,format=raw", path);
    assert_attached_to(qts, "sgi-gigachannel-flash0", "io7/flash");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_io_flash_refusals(void)
{
    g_autofree char *small = make_store("io0-small", SN0_FLASH_SIZE / 2, 0);
    g_autofree char *right = make_store("io0-right", SN0_FLASH_SIZE, 0);
    g_autofree char *small_node = blockdev("io0", small);
    g_autofree char *right_node = blockdev("io0", right);
    /* Outside qtest the machine wants a PROM before it builds the I/O. */
    g_autofree char *prom = make_store("io0-prom", 4096, 0);
    g_autofree char *legacy = g_strdup_printf(
        "if=none,id=sgi-gigachannel-flash0,file=%s,format=raw", small);
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M", O200_GIGACHANNEL ",io0-flash=io0",
        "-bios", prom, "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", right_node, "-drive", legacy, NULL,
    };

    assert_rejected(O200_GIGACHANNEL ",io0-flash=io0", "-blockdev",
                    small_node, "origin200: io0-flash is 524288 bytes; it "
                    "must be exactly 1048576");
    assert_rejected("origin200,io0-flash=io0", "-blockdev", right_node,
                    "origin200: topology 'origin200' with 1 node board has "
                    "no io0-flash");
    assert_launch_rejected(both, "origin200: io0-flash and -drive "
                           "id=sgi-gigachannel-flash0 both back the same "
                           "BASEIO PROM flash");
    g_assert_cmpint(g_unlink(small), ==, 0);
    g_assert_cmpint(g_unlink(right), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

typedef struct NvramStores {
    char *nvram;
    char *clock;
    char *nvram_node;
    char *clock_node;
} NvramStores;

static void nvram_stores_init(NvramStores *stores, const char *stem)
{
    g_autofree char *nvram = g_strdup_printf("%s-nvram", stem);
    g_autofree char *clock = g_strdup_printf("%s-clock", stem);

    stores->nvram = make_store(nvram, NVRAM_SIZE, 0);
    stores->clock = make_store(clock, NVRAM_CLOCK_SIZE, 0);
    stores->nvram_node = blockdev("nv", stores->nvram);
    stores->clock_node = blockdev("nvclock", stores->clock);
}

static void nvram_stores_cleanup(NvramStores *stores)
{
    g_assert_cmpint(g_unlink(stores->nvram), ==, 0);
    g_assert_cmpint(g_unlink(stores->clock), ==, 0);
    g_free(stores->nvram);
    g_free(stores->clock);
    g_free(stores->nvram_node);
    g_free(stores->clock_node);
}

/* Both stores reach the board's Timekeeper. */
static void assert_nvram_attached(QTestState *qts)
{
    g_assert_cmpuint(attached_count(qts, "nv", "/rtc"), ==, 1);
    g_assert_cmpuint(attached_count(qts, "nvclock", "/rtc"), ==, 1);
}

/*
 * nvram0 and nvram0-clock reach the Timekeeper on Origin 200's onboard
 * I/O, an Origin 2000 BaseIO card and an Origin 300 IO-8.
 */
static void test_nvram_bound(void)
{
    g_autofree char *prom = make_sn1_prom();
    NvramStores stores;
    QTestState *qts;

    nvram_stores_init(&stores, "bound");
    qts = qtest_initf("-M origin200,nvram0=nv,nvram0-clock=nvclock "
                      "-blockdev %s -blockdev %s", stores.nvram_node,
                      stores.clock_node);
    assert_nvram_attached(qts);
    qtest_quit(qts);

    qts = qtest_initf("-M " O2K_DESKSIDE ",nvram0=nv,nvram0-clock=nvclock "
                      O2K_DESKSIDE_SIZE " -blockdev %s -blockdev %s",
                      stores.nvram_node, stores.clock_node);
    assert_nvram_attached(qts);
    qtest_quit(qts);

    qts = qtest_initf("-M origin300,nvram0=nv,nvram0-clock=nvclock "
                      SN1_MACHINE " -bios %s -blockdev %s -blockdev %s",
                      prom, stores.nvram_node, stores.clock_node);
    assert_nvram_attached(qts);
    qtest_quit(qts);
    nvram_stores_cleanup(&stores);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/* The NVRAM can go without its clock record, as before. */
static void test_nvram_without_clock(void)
{
    NvramStores stores;
    QTestState *qts;

    nvram_stores_init(&stores, "no-clock");
    qts = qtest_initf("-M origin200,nvram0=nv -blockdev %s",
                      stores.nvram_node);
    g_assert_cmpuint(attached_count(qts, "nv", "/rtc"), ==, 1);
    qtest_quit(qts);
    nvram_stores_cleanup(&stores);
}

/* A drive id, with its "-clock" partner, still reaches the Timekeeper. */
static void test_nvram_legacy_drive(void)
{
    NvramStores stores;
    QTestState *qts;

    nvram_stores_init(&stores, "legacy");
    qts = qtest_initf("-M origin200 "
                      "-drive if=none,id=sgi-nvram0,file=%s,format=raw "
                      "-drive if=none,id=sgi-nvram0-clock,file=%s,format=raw",
                      stores.nvram, stores.clock);
    g_assert_cmpuint(attached_count(qts, "sgi-nvram0", "/rtc"), ==, 1);
    g_assert_cmpuint(attached_count(qts, "sgi-nvram0-clock", "/rtc"), ==, 1);
    qtest_quit(qts);
    nvram_stores_cleanup(&stores);
}

static void test_nvram_refusals(void)
{
    g_autofree char *small = make_store("nvram-small", NVRAM_SIZE / 2, 0);
    g_autofree char *empty = store_path("nvram-empty-clock");
    g_autofree char *small_node = blockdev("nv", small);
    g_autofree char *empty_node = blockdev("nvclock", empty);
    g_autofree char *prom = make_store("nvram-prom", 4096, 0);
    g_autofree char *legacy = NULL;
    NvramStores stores;

    nvram_stores_init(&stores, "refused");
    legacy = g_strdup_printf("if=none,id=sgi-nvram0,file=%s,format=raw",
                             small);
    g_assert_true(g_file_set_contents(empty, "", 0, NULL));
    assert_rejected("origin200,nvram0=nv", "-blockdev", small_node,
                    "origin200: nvram0 is 16384 bytes; it must be exactly "
                    "32768");
    assert_rejected("origin200,nvram0-clock=nvclock", "-blockdev",
                    empty_node, "origin200: nvram0-clock is 0 bytes; it "
                    "must be at least 16");
    assert_rejected("origin200,nvram1=nv", "-blockdev", stores.nvram_node,
                    "origin200: topology 'origin200' with 1 node board has "
                    "no nvram1");
    {
        const char *clock_only[] = {
            qtest_qemu_binary(NULL), "-M", "origin200,nvram0-clock=nvclock",
            "-bios", prom, "-display", "none", "-S", "-qmp", "stdio",
            "-blockdev", stores.clock_node, NULL,
        };
        const char *both[] = {
            qtest_qemu_binary(NULL), "-M", "origin200,nvram0=nv", "-bios",
            prom, "-display", "none", "-S", "-qmp", "stdio",
            "-blockdev", stores.nvram_node, "-drive", legacy, NULL,
        };

        assert_launch_rejected(clock_only,
                               "origin200: nvram0-clock needs nvram0");
        assert_launch_rejected(both, "origin200: nvram0 and -drive "
                               "id=sgi-nvram0 both back the same Timekeeper "
                               "NVRAM");
    }
    nvram_stores_cleanup(&stores);
    g_assert_cmpint(g_unlink(small), ==, 0);
    g_assert_cmpint(g_unlink(empty), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/*
 * The block layer rounds a file's length up to a 512-byte sector, so the
 * size check must read the real one: a 256-byte file is not a 512-byte
 * NVRAM, nor an eight-byte file a 24-byte clock record.
 */
static void test_nvram_short_of_a_sector_refused(void)
{
    g_autofree char *half = make_store("octane-nvram-half", 256, 0);
    g_autofree char *whole = make_store("octane-nvram-whole", 512, 0);
    g_autofree char *clock = make_store("octane-clock-short", 8, 0);
    g_autofree char *prom = make_store("octane-nvram-prom", 4096, 0);
    g_autofree char *half_node = blockdev("nv", half);
    g_autofree char *whole_node = blockdev("nv", whole);
    g_autofree char *clock_node = blockdev("nvclock", clock);
    QTestState *qts;

    assert_rejected("octane,nvram0=nv", "-blockdev", half_node,
                    "octane: nvram0 is 256 bytes; it must be exactly 512");
    assert_rejected("octane,nvram0-clock=nvclock", "-blockdev", clock_node,
                    "octane: nvram0-clock is 8 bytes; it must be at least "
                    "24");

    /* The sector-sized file the check is meant to accept still starts. */
    qts = qtest_initf("-M octane,nvram0=nv -bios %s -blockdev %s", prom,
                      whole_node);
    g_assert_cmpuint(attached_count(qts, "nv", ""), ==, 1);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(half), ==, 0);
    g_assert_cmpint(g_unlink(whole), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/*
 * A module controller's configuration NVRAM binds through module<N>-nvram
 * and reaches the controller's store: the Hub's embedded ELSC on Origin 200,
 * the shared module controller on Origin 2000.
 */
static void test_module_nvram_bound(void)
{
    g_autofree char *path = make_store("module-nvram", MODULE_NVRAM_SIZE, 0);
    g_autofree char *node = blockdev("mnv", path);
    g_autofree char *device = NULL;
    QTestState *qts;

    qts = qtest_initf("-M origin200,module0-nvram=mnv -blockdev %s", node);
    device = attached_device(qts, "mnv");
    g_assert_cmpstr(device, ==, "/machine/node0/hub/nvram");
    qtest_quit(qts);
    g_free(device);

    qts = qtest_initf("-M " O2K_DESKSIDE ",module0-nvram=mnv "
                      O2K_DESKSIDE_SIZE " -blockdev %s", node);
    device = attached_device(qts, "mnv");
    g_assert_cmpstr(device, ==, "/machine/module-management/nvram");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_module_nvram_refusals(void)
{
    g_autofree char *small = make_store("module-nvram-small",
                                        MODULE_NVRAM_SIZE / 2, 0);
    g_autofree char *small_node = blockdev("mnv", small);

    assert_rejected("origin200,module0-nvram=mnv", "-blockdev", small_node,
                    "origin200: module0-nvram is 1024 bytes; it must be "
                    "exactly 2048");
    assert_rejected("origin200,module1-nvram=mnv", "-blockdev", small_node,
                    "origin200: topology 'origin200' with 1 node board has "
                    "no module1-nvram");
    g_assert_cmpint(g_unlink(small), ==, 0);
}

/* The Tezro deskside trial retains the legacy DS1742 IO9 descriptor. */
#define TEZRO_DS1742 \
    "tezro,topology=tezro-deskside-700mhz-4mib-serial-trial," \
    "ip53-experimental=on"
#define TEZRO_DS1742_ARGS \
    "-cpu R16000,r16000-experimental=on," \
    "r16000-reset-mode=0x009bad85,r16000-scache-bytes=0x400000 " \
    "-smp 4 -m 4G"
#define DS1742_SIZE 2048
#define DS1742_CLOCK_SIZE 16

static void test_ds1742_bound(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *data = make_store("ds1742-data", DS1742_SIZE, 0);
    g_autofree char *clock = make_store("ds1742-clock", DS1742_CLOCK_SIZE, 0);
    g_autofree char *data_node = blockdev("rtc", data);
    g_autofree char *clock_node = blockdev("rtc-clock", clock);
    QTestState *qts = qtest_initf(
        "-M " TEZRO_DS1742 ",ds1742-io9-data=rtc,"
        "ds1742-io9-clock=rtc-clock " TEZRO_DS1742_ARGS " -bios %s "
        "-display none -S -blockdev %s -blockdev %s",
        prom, data_node, clock_node);

    assert_attached_to(qts, "rtc", "/ds1742-io9");
    assert_attached_to(qts, "rtc-clock", "/ds1742-io9");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(data), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/* Onyx 350 selects a DS1744-compatible IO9 timekeeper through nvram0. */
#define ONYX350 "onyx350,topology=onyx350-compat"
#define DS1744_SIZE 32768

static void test_ds1744_bound(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *data = make_store("ds1744-data", DS1744_SIZE, 0);
    g_autofree char *clock = make_store("ds1744-clock", DS1742_CLOCK_SIZE, 0);
    g_autofree char *data_node = blockdev("rtc", data);
    g_autofree char *clock_node = blockdev("rtc-clock", clock);
    QTestState *qts = qtest_initf(
        "-M " ONYX350 ",nvram0=rtc,nvram0-clock=rtc-clock "
        "-cpu R16000 -smp 4 -m 8G -bios %s -display none -S "
        "-blockdev %s -blockdev %s", prom, data_node, clock_node);

    assert_attached_to(qts, "rtc", "/ds1744-io9");
    assert_attached_to(qts, "rtc-clock", "/ds1744-io9");
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(data), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_ds1742_refusals(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *data = make_store("ds1742-data", DS1742_SIZE, 0);
    g_autofree char *clock = make_store("ds1742-clock", DS1742_CLOCK_SIZE, 0);
    g_autofree char *data_node = blockdev("rtc", data);
    g_autofree char *legacy = g_strdup_printf(
        "if=none,id=sgi-ds1742-data-io9,file=%s,format=raw", clock);
    const char *half[] = {
        qtest_qemu_binary(NULL), "-M",
        TEZRO_DS1742 ",ds1742-io9-data=rtc",
        "-cpu", "R16000,r16000-experimental=on,"
        "r16000-reset-mode=0x009bad85,r16000-scache-bytes=0x400000",
        "-smp", "4", "-m", "4G", "-bios", prom,
        "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", data_node, NULL,
    };
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M",
        TEZRO_DS1742 ",ds1742-io9-data=rtc",
        "-cpu", "R16000,r16000-experimental=on,"
        "r16000-reset-mode=0x009bad85,r16000-scache-bytes=0x400000",
        "-smp", "4", "-m", "4G", "-bios", prom,
        "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", data_node, "-drive", legacy, NULL,
    };
    const char *neither[] = {
        qtest_qemu_binary(NULL), "-M", TEZRO_DS1742,
        "-cpu", "R16000,r16000-experimental=on,"
        "r16000-reset-mode=0x009bad85,r16000-scache-bytes=0x400000",
        "-smp", "4", "-m", "4G", "-bios", prom,
        "-display", "none", "-S", "-qmp", "stdio", NULL,
    };

    assert_launch_rejected(half, "tezro: IO9 board 'io9' needs both "
                           "ds1742-io9-data and ds1742-io9-clock");
    assert_launch_rejected(both, "tezro: ds1742-io9-data and -drive "
                           "id=sgi-ds1742-data-io9 both back IO9 board "
                           "'io9''s DS1742");
    assert_launch_rejected(neither, "tezro: IO9 board 'io9' needs its "
                           "DS1742 stores ds1742-io9-data and "
                           "ds1742-io9-clock");
    g_assert_cmpint(g_unlink(data), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/* An Origin 300 PCI expansion module's configuration storage. */
#define EXPANSION "origin300,topology=origin300-pci-expansion-" \
    "configuration-storage-experiment"

static void test_expansion_flash_bound(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *store = make_store("expansion", SN1_FLASH_SIZE, 0);
    g_autofree char *node = blockdev("config", store);
    QTestState *qts = qtest_initf(
        "-M " EXPANSION ",expansion-enclosure-flash=config "
        SN1_MACHINE " -bios %s -display none -S -blockdev %s", prom, node);

    g_assert_cmpuint(attached_count(qts, "config", ""), ==, 1);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(store), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_expansion_flash_refusals(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *store = make_store("expansion", SN1_FLASH_SIZE, 0);
    g_autofree char *node = blockdev("config", store);
    g_autofree char *legacy = g_strdup_printf(
        "if=none,id=cfg,file=%s,format=raw", store);
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M",
        EXPANSION ",expansion-enclosure-flash=config,"
        "expansion-storage.0=expansion-enclosure=cfg",
        "-cpu", "R14000", "-smp", "2", "-bios", prom, "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", node, "-drive", legacy, NULL,
    };

    assert_launch_rejected(both, "expansion-enclosure-flash and "
                           "expansion-storage both back the expansion "
                           "configuration storage");
    g_assert_cmpint(g_unlink(store), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("sgi-machine-storage-XXXXXX", &error);
    g_assert_no_error(error);

    qtest_add_func("/sgi-machine-storage/sn0-node-flash/persists",
                   test_sn0_node_flash_persists);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/per-node",
                   test_sn0_node_flash_per_node);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/partly-bound",
                   test_sn0_node_flash_partly_bound);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/partly-bound-no-bios",
                   test_sn0_node_flash_partly_bound_needs_bios);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/legacy-drive",
                   test_sn0_node_flash_legacy_drive);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/refusals",
                   test_sn0_node_flash_refusals);
    qtest_add_func("/sgi-machine-storage/machine-help",
                   test_machine_help_lists_storage);
    qtest_add_func("/sgi-machine-storage/overrides-name-properties",
                   test_overrides_name_properties);
    qtest_add_func("/sgi-machine-storage/shared-node-refused",
                   test_shared_node_refused);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/bound",
                   test_sn1_node_flash_bound);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/protocol-node",
                   test_sn1_node_flash_protocol_node);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/legacy-drive",
                   test_sn1_node_flash_legacy_drive);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/refusals",
                   test_sn1_node_flash_refusals);
    qtest_add_func("/sgi-machine-storage/io-flash/bound",
                   test_io_flash_bound);
    qtest_add_func("/sgi-machine-storage/io-flash/legacy-drive",
                   test_io_flash_legacy_drive);
    qtest_add_func("/sgi-machine-storage/io-flash/refusals",
                   test_io_flash_refusals);
    qtest_add_func("/sgi-machine-storage/nvram/bound", test_nvram_bound);
    qtest_add_func("/sgi-machine-storage/module-nvram/bound",
                   test_module_nvram_bound);
    qtest_add_func("/sgi-machine-storage/module-nvram/refusals",
                   test_module_nvram_refusals);
    qtest_add_func("/sgi-machine-storage/nvram/without-clock",
                   test_nvram_without_clock);
    qtest_add_func("/sgi-machine-storage/nvram/legacy-drive",
                   test_nvram_legacy_drive);
    qtest_add_func("/sgi-machine-storage/nvram/refusals",
                   test_nvram_refusals);
    qtest_add_func("/sgi-machine-storage/nvram/short-of-a-sector-refused",
                   test_nvram_short_of_a_sector_refused);
    qtest_add_func("/sgi-machine-storage/ds1742/bound", test_ds1742_bound);
    qtest_add_func("/sgi-machine-storage/ds1744/bound", test_ds1744_bound);
    qtest_add_func("/sgi-machine-storage/ds1742/refusals",
                   test_ds1742_refusals);
    qtest_add_func("/sgi-machine-storage/expansion-flash/bound",
                   test_expansion_flash_bound);
    qtest_add_func("/sgi-machine-storage/expansion-flash/refusals",
                   test_expansion_flash_refusals);
    ret = g_test_run();
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(directory);
    return ret;
}
