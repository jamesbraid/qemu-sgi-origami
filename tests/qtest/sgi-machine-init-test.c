/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A machine started on state qemu-sgi-machine-init created must read the
 * same boot flash as one started with no files on the same PROM image.
 * Each case creates the state from a synthetic PROM, starts the machine
 * once with every store attached through -blockdev and its storage
 * property and once unbacked with the image through -bios, and compares
 * the flash through the window the processor boots from.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/*
 * The IP27 and IP35 boot windows, uncached. Octane's Bridge flash
 * window, Bridge base 0x1f000000 plus 0xc00000, is at the same address.
 */
#define BOOT_WINDOW UINT64_C(0x1fc00000)

#define IP27_FLASH_SIZE (1024 * 1024)
/* A stock-shaped IP27 configuration block, so the product values apply. */
#define IP27_CONFIG 0x60
#define IP27_CONFIG_MAGIC 0x69703237636f6e66ULL
#define IP27_PROM_SIZE 0x20000

/* Origin 300's PROM position holds exactly the image its -bios takes. */
#define IP35_PROM_SIZE 0x1686a8
#define SN1_MACHINE "-cpu R14000 -smp 2"
#define FUEL_MACHINE "-cpu R14000 -smp 1 -m 512M"

/* Octane's whole 1 MiB flash; a PROM image shorter than that. */
#define IP30_FLASH_SIZE (1024 * 1024)
#define IP30_PROM_SIZE 0x9a3c4

#define O2K_DESKSIDE "origin2000,topology=origin2000-deskside,nodes=2"

static char *scratch;

/* A synthetic image: a pattern, with an IP27 configuration block if asked. */
static char *make_prom(const char *name, size_t size, bool ip27_config)
{
    g_autofree uint8_t *bytes = g_malloc(size);
    char *path = g_build_filename(scratch, name, NULL);
    size_t i;

    for (i = 0; i < size; i++) {
        bytes[i] = 0x5a ^ (i * 13) ^ (i >> 8);
    }
    if (ip27_config) {
        stq_be_p(bytes + IP27_CONFIG + 0x08, IP27_CONFIG_MAGIC);
        stl_be_p(bytes + IP27_CONFIG + 0x2c, 8);
        stl_be_p(bytes + IP27_CONFIG + 0x30, 0);
        stl_be_p(bytes + IP27_CONFIG + 0x3c, 1);
    }
    g_assert_true(g_file_set_contents(path, (char *)bytes, size, NULL));
    return path;
}

/* Run the tool for @machine; returns the new state directory. */
static char *create_state(const char *stem, const char *machine,
                          const char *boot_prom, const char *io_prom)
{
    const char *tool = g_getenv("QTEST_QEMU_SGI_MACHINE_INIT");
    char *dir = g_build_filename(scratch, stem, NULL);
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    g_autofree char *err = NULL;
    int status;

    g_assert_nonnull(tool);
    g_ptr_array_add(argv, (gpointer)tool);
    g_ptr_array_add(argv, (gpointer)"--machine");
    g_ptr_array_add(argv, (gpointer)machine);
    g_ptr_array_add(argv, (gpointer)"--boot-prom");
    g_ptr_array_add(argv, (gpointer)boot_prom);
    if (io_prom) {
        g_ptr_array_add(argv, (gpointer)"--io-prom");
        g_ptr_array_add(argv, (gpointer)io_prom);
    }
    g_ptr_array_add(argv, dir);
    g_ptr_array_add(argv, NULL);
    g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                               G_SPAWN_DEFAULT, NULL, NULL, NULL, &err,
                               &status, &error));
    g_assert_no_error(error);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        g_printerr("%s", err);
    }
    g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return dir;
}

/*
 * "-blockdev ... -M <machine>,<name>=<name>,..." attaching every store in
 * @dir, each as a block node named for its storage item.
 */
static char *attach_state(const char *machine, const char *dir)
{
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    g_autoptr(GString) blockdevs = g_string_new("");
    g_autoptr(GString) options = g_string_new(machine);
    const char *file;

    g_assert_nonnull(d);
    while ((file = g_dir_read_name(d))) {
        g_autofree char *name = g_strndup(file, strlen(file) - 4);

        g_assert_true(g_str_has_suffix(file, ".raw"));
        g_string_append_printf(blockdevs,
                               " -blockdev driver=raw,node-name=%s,"
                               "file.driver=file,file.filename=%s/%s",
                               name, dir, file);
        g_string_append_printf(options, ",%s=%s", name, name);
    }
    return g_strdup_printf("-M %s%s", options->str, blockdevs->str);
}

static void remove_state(char *dir)
{
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    const char *file;

    while ((file = g_dir_read_name(d))) {
        g_autofree char *path = g_build_filename(dir, file, NULL);

        g_assert_cmpint(g_unlink(path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_free(dir);
}

static uint8_t *read_window(QTestState *qts, unsigned int cpu, size_t size)
{
    uint8_t *bytes = g_malloc(size);

    g_assert_true(qtest_memread_cpu(qts, cpu, BOOT_WINDOW, bytes, size));
    return bytes;
}

/* Each node's window, through the first processor on that node. */
static uint8_t **read_nodes(QTestState *qts, const unsigned int *cpus,
                            unsigned int nodes, size_t size)
{
    uint8_t **windows = g_new0(uint8_t *, nodes);
    unsigned int i;

    for (i = 0; i < nodes; i++) {
        windows[i] = read_window(qts, cpus[i], size);
    }
    return windows;
}

/*
 * The IP35 configuration block at offset 0x60: the product's machine type
 * at +0x30, and the block's byte sum the same as the image's, which
 * check_sum_adj keeps so the PROM's checksum holds.
 */
static void assert_ip35_config(const uint8_t *flash, const char *prom,
                               uint32_t machine_type)
{
    g_autofree char *image = NULL;
    uint8_t flash_sum = 0, image_sum = 0;
    gsize size;
    unsigned int i;

    g_assert_true(g_file_get_contents(prom, &image, &size, NULL));
    for (i = 0x60; i < 0xb0; i++) {
        flash_sum += flash[i];
        image_sum += (uint8_t)image[i];
    }
    g_assert_cmpuint(ldl_be_p(flash + 0x90), ==, machine_type);
    g_assert_cmphex(flash_sum, ==, image_sum);
}

static void free_nodes(uint8_t **windows, unsigned int nodes)
{
    unsigned int i;

    for (i = 0; i < nodes; i++) {
        g_free(windows[i]);
    }
    g_free(windows);
}

static void assert_nodes_equal(uint8_t **created, uint8_t **unbacked,
                               unsigned int nodes, size_t size)
{
    unsigned int i;

    for (i = 0; i < nodes; i++) {
        g_assert_cmpmem(created[i], size, unbacked[i], size);
    }
}

/*
 * The module controller's configuration NVRAM the tool created: zeroes but
 * for the block a master controller writes at its first power-up, with the
 * product build's configuration byte and module number. A machine with no
 * file starts its store from the same bytes, which the device's own qtests
 * check through the I2C bus.
 */
static void assert_module_nvram(const char *dir, uint8_t config,
                                uint8_t module, uint8_t clock)
{
    g_autofree char *path = g_build_filename(dir, "module0-nvram.raw", NULL);
    g_autofree char *bytes = NULL;
    uint8_t expected[2048] = { };
    gsize size;

    expected[0x700] = 0x37;
    memcpy(&expected[0x701], "none", 4);
    expected[0x707] = config;
    expected[0x708] = module;
    expected[0x70b] = clock;
    g_assert_true(g_file_get_contents(path, &bytes, &size, NULL));
    g_assert_cmpmem(bytes, size, expected, sizeof(expected));
}

static void test_origin200(void)
{
    static const unsigned int cpus[] = { 0 };
    const char *machine = "origin200,topology=origin200,nodes=1";
    g_autofree char *prom = make_prom("o200-prom", IP27_PROM_SIZE, true);
    char *dir = create_state("o200", machine, prom, NULL);
    g_autofree char *attached = attach_state(machine, dir);
    QTestState *qts;
    uint8_t **created, **unbacked;

    qts = qtest_init(attached);
    created = read_nodes(qts, cpus, 1, IP27_FLASH_SIZE);
    qtest_quit(qts);
    qts = qtest_initf("-M %s -bios %s", machine, prom);
    unbacked = read_nodes(qts, cpus, 1, IP27_FLASH_SIZE);
    qtest_quit(qts);

    assert_nodes_equal(created, unbacked, 1, IP27_FLASH_SIZE);
    /* Origin 200's own values replaced the image's 8 and 1. */
    g_assert_cmpuint(ldl_be_p(created[0] + IP27_CONFIG + 0x2c), ==, 15);
    g_assert_cmpuint(ldl_be_p(created[0] + IP27_CONFIG + 0x30), ==, 1);
    g_assert_cmpuint(ldl_be_p(created[0] + IP27_CONFIG + 0x3c), ==, 4);
    /* Origin 200: automatic power-up, no module number, full clock. */
    assert_module_nvram(dir, 0x02, 0, 0x78);
    free_nodes(created, 1);
    free_nodes(unbacked, 1);
    remove_state(dir);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_origin2000(void)
{
    /* Two processors per node: node 1's first is processor 2. */
    static const unsigned int cpus[] = { 0, 2 };
    g_autofree char *prom = make_prom("o2k-prom", IP27_PROM_SIZE, true);
    g_autofree char *io_prom = make_prom("o2k-io-prom", 0x3000, false);
    char *dir = create_state("o2k", O2K_DESKSIDE, prom, io_prom);
    g_autofree char *attached = attach_state(O2K_DESKSIDE, dir);
    QTestState *qts;
    uint8_t **created, **unbacked;

    qts = qtest_initf("%s -smp 4 -m 512M", attached);
    created = read_nodes(qts, cpus, 2, IP27_FLASH_SIZE);
    qtest_quit(qts);
    qts = qtest_initf("-M " O2K_DESKSIDE ",io-prom=%s -smp 4 -m 512M "
                      "-bios %s", io_prom, prom);
    unbacked = read_nodes(qts, cpus, 2, IP27_FLASH_SIZE);
    qtest_quit(qts);

    assert_nodes_equal(created, unbacked, 2, IP27_FLASH_SIZE);
    /* The Origin 2000 build, and the deskside module's number 1. */
    assert_module_nvram(dir, 0, 1, 0);
    free_nodes(created, 2);
    free_nodes(unbacked, 2);
    remove_state(dir);
    g_assert_cmpint(g_unlink(prom), ==, 0);
    g_assert_cmpint(g_unlink(io_prom), ==, 0);
}

/*
 * A created Origin 300 starts from its flash alone, as a launcher starts
 * it, with no -bios; the unbacked machine needs -bios.
 */
static void test_origin300(void)
{
    static const unsigned int cpus[] = { 0 };
    const char *machine = "origin300,topology=origin300,nodes=1";
    g_autofree char *prom = make_prom("o300-prom", IP35_PROM_SIZE, false);
    char *dir = create_state("o300", machine, prom, NULL);
    g_autofree char *attached = attach_state(machine, dir);
    QTestState *qts;
    uint8_t **created, **unbacked;

    qts = qtest_initf("%s " SN1_MACHINE, attached);
    created = read_nodes(qts, cpus, 1, IP35_PROM_SIZE);
    qtest_quit(qts);
    qts = qtest_initf("-M %s " SN1_MACHINE " -bios %s", machine, prom);
    unbacked = read_nodes(qts, cpus, 1, IP35_PROM_SIZE);
    qtest_quit(qts);

    assert_nodes_equal(created, unbacked, 1, IP35_PROM_SIZE);
    /* Origin 300's configuration block names machine type 1, SN10. */
    assert_ip35_config(created[0], prom, 1);
    free_nodes(created, 1);
    free_nodes(unbacked, 1);
    remove_state(dir);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/* Fuel is the second Bedrock product; its PROM position is Origin 300's. */
static void test_fuel(void)
{
    static const unsigned int cpus[] = { 0 };
    const char *machine = "fuel,topology=fuel,nodes=1";
    g_autofree char *prom = make_prom("fuel-prom", IP35_PROM_SIZE, false);
    char *dir = create_state("fuel", machine, prom, NULL);
    g_autofree char *attached = attach_state(machine, dir);
    QTestState *qts;
    uint8_t **created, **unbacked;

    qts = qtest_initf("%s " FUEL_MACHINE, attached);
    created = read_nodes(qts, cpus, 1, IP35_PROM_SIZE);
    qtest_quit(qts);
    qts = qtest_initf("-M %s " FUEL_MACHINE " -bios %s", machine, prom);
    unbacked = read_nodes(qts, cpus, 1, IP35_PROM_SIZE);
    qtest_quit(qts);

    assert_nodes_equal(created, unbacked, 1, IP35_PROM_SIZE);
    /* Fuel's names machine type 2, SN11, which its PROM table expects. */
    assert_ip35_config(created[0], prom, 2);
    free_nodes(created, 1);
    free_nodes(unbacked, 1);
    remove_state(dir);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/*
 * Octane reads its whole flash through the Bridge window, so the erased
 * bytes after a short image are compared too.
 */
static void test_octane(gconstpointer data)
{
    static const unsigned int cpus[] = { 0 };
    bool octane2 = GPOINTER_TO_INT(data);
    const char *machine = octane2 ? "octane2,topology=octane2,nodes=1" :
                                    "octane,topology=octane,nodes=1";
    const char *cpu = octane2 ? "-cpu R12000 -m 256M" : "-cpu R10000 -m 128M";
    g_autofree char *prom = make_prom("ip30-prom", IP30_PROM_SIZE, false);
    char *dir = create_state("ip30", machine, prom, NULL);
    g_autofree char *attached = attach_state(machine, dir);
    QTestState *qts;
    uint8_t **created, **unbacked;

    qts = qtest_initf("%s %s", attached, cpu);
    created = read_nodes(qts, cpus, 1, IP30_FLASH_SIZE);
    qtest_quit(qts);
    qts = qtest_initf("-M %s %s -bios %s", machine, cpu, prom);
    unbacked = read_nodes(qts, cpus, 1, IP30_FLASH_SIZE);
    qtest_quit(qts);

    assert_nodes_equal(created, unbacked, 1, IP30_FLASH_SIZE);
    /* The image's first byte, then the erased tail. */
    g_assert_cmphex(created[0][0], ==, 0x5a);
    g_assert_cmphex(created[0][IP30_PROM_SIZE], ==, 0xff);
    free_nodes(created, 1);
    free_nodes(unbacked, 1);
    remove_state(dir);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);
    scratch = g_dir_make_tmp("sgi-machine-init-XXXXXX", &error);
    g_assert_no_error(error);

    qtest_add_func("/sgi-machine-init/origin200", test_origin200);
    qtest_add_func("/sgi-machine-init/origin2000", test_origin2000);
    qtest_add_func("/sgi-machine-init/origin300", test_origin300);
    qtest_add_func("/sgi-machine-init/fuel", test_fuel);
    qtest_add_data_func("/sgi-machine-init/octane", GINT_TO_POINTER(0),
                        test_octane);
    qtest_add_data_func("/sgi-machine-init/octane2", GINT_TO_POINTER(1),
                        test_octane);
    ret = g_test_run();

    g_assert_cmpint(g_rmdir(scratch), ==, 0);
    g_free(scratch);
    return ret;
}
