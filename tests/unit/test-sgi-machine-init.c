/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Run qemu-sgi-machine-init as a launcher does and check what it accepts,
 * what it refuses and what it leaves behind. All images are synthetic.
 */
#include "qemu/osdep.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "qemu/bswap.h"
#include "hw/rtc/m48t35-ip27.h"

typedef struct ToolRun {
    int status;
    char *out;
    char *err;
} ToolRun;

static const char *tool;
static char *scratch;

static void tool_run_clear(ToolRun *run)
{
    g_free(run->out);
    g_free(run->err);
}

/* Run the tool with @args, after running @setup in the child if given. */
static ToolRun run_toolv(GSpawnChildSetupFunc setup, const char *const *args)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    ToolRun run = { 0 };

    g_ptr_array_add(argv, (gpointer)tool);
    for (; *args; args++) {
        g_ptr_array_add(argv, (gpointer)*args);
    }
    g_ptr_array_add(argv, NULL);

    g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                               G_SPAWN_DEFAULT, setup, NULL, &run.out,
                               &run.err, &run.status, &error));
    g_assert_no_error(error);
    return run;
}

/* Run the tool with the NULL-terminated arguments after argv[0]. */
static ToolRun run_tool(const char *first, ...)
{
    g_autoptr(GPtrArray) args = g_ptr_array_new();
    const char *arg;
    va_list ap;

    va_start(ap, first);
    for (arg = first; arg; arg = va_arg(ap, const char *)) {
        g_ptr_array_add(args, (gpointer)arg);
    }
    va_end(ap);
    g_ptr_array_add(args, NULL);
    return run_toolv(NULL, (const char *const *)args->pdata);
}

static bool run_succeeded(const ToolRun *run)
{
    return g_spawn_check_exit_status(run->status, NULL);
}

/* The tool failed, said @message and printed nothing on stdout. */
static void assert_refused(const ToolRun *run, const char *message)
{
    if (!strstr(run->err, message)) {
        g_printerr("expected '%s' in:\n%s", message, run->err);
    }
    g_assert_false(run_succeeded(run));
    g_assert_nonnull(strstr(run->err, message));
    g_assert_cmpstr(run->out, ==, "");
}

static char *scratch_path(const char *name)
{
    return g_build_filename(scratch, name, NULL);
}

static void test_help(void)
{
    ToolRun run = run_tool("--help", NULL);

    g_assert_true(run_succeeded(&run));
    g_assert_nonnull(strstr(run.out, "--machine OPTIONS"));
    tool_run_clear(&run);
}

/* Every listed selection appears once, and nothing else is printed. */
static void test_machine_help(void)
{
    static const char *const listed[] = {
        "origin200,topology=origin200,nodes=1",
        "origin200,topology=origin200-gigachannel,nodes=1",
        "origin2000,topology=origin2000-deskside,nodes=2",
        "origin300,topology=origin300,nodes=1",
        "fuel,topology=fuel,nodes=1",
        "octane,topology=octane,nodes=1",
    };
    ToolRun run = run_tool("--machine", "help", NULL);
    g_auto(GStrv) lines = NULL;
    size_t i;

    g_assert_true(run_succeeded(&run));
    g_assert_cmpstr(run.err, ==, "");
    lines = g_strsplit(run.out, "\n", -1);
    for (i = 0; i < ARRAY_SIZE(listed); i++) {
        g_autofree char *prefix = g_strdup_printf("%s ", listed[i]);
        unsigned int count = 0;
        char **line;

        for (line = lines; *line; line++) {
            count += g_str_has_prefix(*line, prefix);
        }
        g_assert_cmpuint(count, ==, 1);
    }
    tool_run_clear(&run);
}

static void test_usage_errors(void)
{
    g_autofree char *dir = scratch_path("usage");
    ToolRun run;

    run = run_tool(dir, NULL);
    assert_refused(&run, "--machine is required");
    tool_run_clear(&run);

    run = run_tool("--machine", "help", dir, NULL);
    assert_refused(&run, "--machine help takes no other arguments");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", "prom", NULL);
    assert_refused(&run, "no directory named");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", "prom", dir, "extra", NULL);
    assert_refused(&run, "unexpected argument 'extra'");
    tool_run_clear(&run);

    run = run_tool("--frobnicate", NULL);
    assert_refused(&run, "--help");
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

static void test_unknown_machine(void)
{
    g_autofree char *dir = scratch_path("unknown");
    ToolRun run;

    run = run_tool("--machine", "vax", "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "unknown machine 'vax'");
    tool_run_clear(&run);

    /* A selection the machine resolves, but not in the catalogue's words. */
    run = run_tool("--machine", "origin200", "--boot-prom", "prom", dir,
                   NULL);
    assert_refused(&run, "'origin200' is not a listed machine");
    g_assert_nonnull(strstr(run.err,
                            "It selects 'origin200,topology=origin200,"
                            "nodes=1'"));
    tool_run_clear(&run);

    run = run_tool("--machine",
                   "origin200,topology=origin200,nodes=1,memory=64",
                   "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "machine option 'memory' is not part of a "
                   "catalogue selection");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=3",
                   "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "topology 'origin200' fits 1 node board, not 3");
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

static void test_inputs_checked(void)
{
    g_autofree char *dir = scratch_path("inputs");
    ToolRun run;

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1", dir,
                   NULL);
    assert_refused(&run, "origin200,topology=origin200,nodes=1 requires "
                   "--boot-prom");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin2000,topology=origin2000-deskside,"
                   "nodes=2", "--boot-prom", "prom", dir, NULL);
    assert_refused(&run, "requires --io-prom");
    tool_run_clear(&run);

    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", "prom", "--io-prom", "io", dir, NULL);
    assert_refused(&run, "origin200,topology=origin200,nodes=1 takes no "
                   "--io-prom");
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

/* Synthetic images: a recognisable pattern, nothing from a real PROM. */
#define PAYLOAD_SIZE 0x40
#define IO_PROM_SIZE 0x3000
#define IP27_CONFIG 0x60
#define IP35_LABEL (8 * 1024 * 1024 + 0x1e0000 + 0x10)
/* The exact payload every Bedrock topology's -bios must be. */
#define IP35_PROM_SIZE 1476264

typedef struct StoreSize {
    const char *name;
    size_t size;
} StoreSize;

static char *boot_prom;
static char *ip35_prom;
static char *io_prom;

static char *make_image(const char *name, size_t size, uint8_t seed)
{
    g_autofree uint8_t *bytes = g_malloc(size);
    char *path = scratch_path(name);
    size_t i;

    for (i = 0; i < size; i++) {
        bytes[i] = seed + i * 7;
    }
    g_assert_true(g_file_set_contents(path, (char *)bytes, size, NULL));
    return path;
}

static uint8_t *read_store(const char *dir, const char *name, gsize *size)
{
    g_autofree char *path = g_strdup_printf("%s/%s.raw", dir, name);
    char *bytes;

    g_assert_true(g_file_get_contents(path, &bytes, size, NULL));
    return (uint8_t *)bytes;
}

static void remove_tree(const char *dir)
{
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    const char *name;

    g_assert_nonnull(d);
    while ((name = g_dir_read_name(d))) {
        g_autofree char *path = g_build_filename(dir, name, NULL);

        g_assert_cmpint(g_unlink(path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* How many entries @dir holds. */
static unsigned int count_entries(const char *dir)
{
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    unsigned int count = 0;

    g_assert_nonnull(d);
    while (g_dir_read_name(d)) {
        count++;
    }
    return count;
}

/*
 * Create @machine's state in a fresh directory and check that it holds
 * exactly @stores, each its catalogue size. Returns the directory.
 */
static char *create_and_check(const char *stem, const char *machine,
                              bool with_io_prom, const StoreSize *stores,
                              size_t count)
{
    char *dir = scratch_path(stem);
    bool ip35 = g_str_has_prefix(machine, "origin300,") ||
                g_str_has_prefix(machine, "fuel,");
    const char *args[] = {
        "--machine", machine, "--boot-prom", ip35 ? ip35_prom : boot_prom,
        dir, NULL, NULL, NULL,
    };
    ToolRun run;
    size_t i;

    if (with_io_prom) {
        args[4] = "--io-prom";
        args[5] = io_prom;
        args[6] = dir;
    }
    run = run_toolv(NULL, args);
    if (!run_succeeded(&run)) {
        g_printerr("%s", run.err);
    }
    g_assert_true(run_succeeded(&run));
    g_assert_cmpstr(run.out, ==, "");
    g_assert_cmpstr(run.err, ==, "");
    tool_run_clear(&run);

    g_assert_cmpuint(count_entries(dir), ==, count);
    for (i = 0; i < count; i++) {
        g_autofree char *path = g_strdup_printf("%s/%s.raw", dir,
                                                stores[i].name);
        GStatBuf st;

        g_assert_cmpint(g_lstat(path, &st), ==, 0);
        g_assert_true(S_ISREG(st.st_mode));
        g_assert_cmpuint(st.st_size, ==, stores[i].size);
    }
    return dir;
}

static void assert_all(const uint8_t *bytes, size_t from, size_t to,
                       uint8_t value)
{
    size_t i;

    for (i = from; i < to; i++) {
        if (bytes[i] != value) {
            g_printerr("byte 0x%zx is 0x%02x, not 0x%02x\n", i, bytes[i],
                       value);
        }
        g_assert_cmphex(bytes[i], ==, value);
    }
}

static void assert_zero_store(const char *dir, const char *name)
{
    gsize size;
    g_autofree uint8_t *bytes = read_store(dir, name, &size);

    assert_all(bytes, 0, size, 0);
}

/*
 * @name holds the fresh IP27 Timekeeper image for the day the tool ran,
 * which lies between @before and the time of this check.
 */
static void assert_fresh_nvram(const char *dir, const char *name,
                               time_t before)
{
    time_t after = time(NULL);
    gsize size;
    g_autofree uint8_t *bytes = read_store(dir, name, &size);
    g_autofree uint8_t *first = g_malloc(M48T35_SIZE);
    g_autofree uint8_t *last = g_malloc(M48T35_SIZE);
    struct tm tm;

    g_assert_cmpuint(size, ==, M48T35_SIZE);
    m48t35_ip27_fresh_image(first, gmtime_r(&before, &tm));
    m48t35_ip27_fresh_image(last, gmtime_r(&after, &tm));
    g_assert_true(!memcmp(bytes, first, size) || !memcmp(bytes, last, size));
}

static void assert_starts_with_payload(const uint8_t *bytes, uint8_t seed)
{
    size_t i;

    for (i = 0; i < PAYLOAD_SIZE; i++) {
        g_assert_cmphex(bytes[i], ==, (uint8_t)(seed + i * 7));
    }
}

/*
 * @name holds a module controller's configuration NVRAM as the controller
 * leaves it after its first power-up: zeroes but for the marker, the
 * password "none", @config and @module.
 */
static void assert_module_nvram(const char *dir, const char *name,
                                uint8_t config, uint8_t module, uint8_t clock)
{
    gsize size;
    g_autofree uint8_t *bytes = read_store(dir, name, &size);

    g_assert_cmpuint(size, ==, 2048);
    assert_all(bytes, 0, 0x700, 0);
    g_assert_cmphex(bytes[0x700], ==, 0x37);
    g_assert_cmpmem(&bytes[0x701], 4, "none", 4);
    assert_all(bytes, 0x705, 0x707, 0);
    g_assert_cmphex(bytes[0x707], ==, config);
    g_assert_cmphex(bytes[0x708], ==, module);
    assert_all(bytes, 0x709, 0x70b, 0);
    g_assert_cmphex(bytes[0x70b], ==, clock);
    assert_all(bytes, 0x70c, size, 0);
}

static void test_origin200(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 1048576 }, { "nvram0", 32768 },
        { "nvram0-clock", 16 }, { "module0-nvram", 2048 },
    };
    time_t before = time(NULL);
    g_autofree char *dir = create_and_check(
        "o200", "origin200,topology=origin200,nodes=1", false, stores,
        ARRAY_SIZE(stores));
    gsize size;
    g_autofree uint8_t *flash = read_store(dir, "node0-flash", &size);

    /*
     * A payload too short to reach the configuration block leaves the
     * template's block, carrying Origin 200's own values.
     */
    assert_starts_with_payload(flash, 0x27);
    g_assert_cmphex(ldq_be_p(flash + IP27_CONFIG + 0x08), ==,
                    0x69703237636f6e66ULL);
    g_assert_cmpuint(ldl_be_p(flash + IP27_CONFIG + 0x2c), ==, 15);
    g_assert_cmpuint(ldl_be_p(flash + IP27_CONFIG + 0x30), ==, 1);
    g_assert_cmpuint(ldl_be_p(flash + IP27_CONFIG + 0x3c), ==, 4);
    assert_fresh_nvram(dir, "nvram0", before);
    assert_zero_store(dir, "nvram0-clock");
    /* The Origin 200 build: automatic power-up, no module number. */
    assert_module_nvram(dir, "module0-nvram", 0x02, 0, 0x78);
    remove_tree(dir);
}

/*
 * A processor= offering's flash carries that module's mode word and clock,
 * here the R12000's: 4 MiB, cache clock divided by 1.5, system clock by 3,
 * tap 9, at 270 MHz. A name the topology does not offer is refused.
 */
static void test_origin200_processor(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 1048576 }, { "nvram0", 32768 },
        { "nvram0-clock", 16 }, { "module0-nvram", 2048 },
    };
    g_autofree char *dir = create_and_check(
        "o200-r12k",
        "origin200,topology=origin200,nodes=1,processor=r12000-270mhz-4mb",
        false, stores, ARRAY_SIZE(stores));
    g_autofree char *refused = scratch_path("o200-refused");
    gsize size;
    g_autofree uint8_t *flash = read_store(dir, "node0-flash", &size);
    ToolRun run;

    g_assert_cmphex(ldl_be_p(flash + IP27_CONFIG + 0x04), ==, 0x1213ab85);
    g_assert_cmpuint(ldq_be_p(flash + IP27_CONFIG + 0x10), ==, 270000000);
    g_assert_cmpuint(ldl_be_p(flash + IP27_CONFIG + 0x30), ==, 1);

    run = run_tool("--machine",
                   "origin200,topology=origin200,nodes=1,"
                   "processor=r12000-300mhz-8mb",
                   "--boot-prom", "prom", refused, NULL);
    assert_refused(&run, "topology 'origin200' offers no processor "
                   "'r12000-300mhz-8mb'");
    tool_run_clear(&run);
    g_assert_false(g_file_test(refused, G_FILE_TEST_EXISTS));
    remove_tree(dir);
}

static void test_origin2000(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 1048576 }, { "node1-flash", 1048576 },
        { "io0-flash", 1048576 }, { "nvram0", 32768 },
        { "nvram0-clock", 16 }, { "module0-nvram", 2048 },
    };
    time_t before = time(NULL);
    g_autofree char *dir = create_and_check(
        "o2k", "origin2000,topology=origin2000-deskside,nodes=2", true,
        stores, ARRAY_SIZE(stores));
    gsize size, size1;
    g_autofree uint8_t *node0 = read_store(dir, "node0-flash", &size);
    g_autofree uint8_t *node1 = read_store(dir, "node1-flash", &size1);
    g_autofree uint8_t *io = read_store(dir, "io0-flash", &size);
    size_t i;

    /* Every node gets the same PROM. */
    g_assert_cmpmem(node0, size, node1, size1);
    /* Origin 2000's catalogue values are the stock image's own. */
    g_assert_cmphex(ldq_be_p(node0 + IP27_CONFIG + 0x08), ==,
                    0x69703237636f6e66ULL);
    g_assert_cmpuint(ldl_be_p(node0 + IP27_CONFIG + 0x2c), ==, 8);
    g_assert_cmpuint(ldl_be_p(node0 + IP27_CONFIG + 0x30), ==, 0);
    g_assert_cmpuint(ldl_be_p(node0 + IP27_CONFIG + 0x3c), ==, 1);
    /* The I/O PROM is kept as supplied, erased after it. */
    for (i = 0; i < IO_PROM_SIZE; i++) {
        g_assert_cmphex(io[i], ==, (uint8_t)(0x06 + i * 7));
    }
    assert_all(io, IO_PROM_SIZE, size, 0xff);
    assert_fresh_nvram(dir, "nvram0", before);
    assert_zero_store(dir, "nvram0-clock");
    /* The Origin 2000 build, and the deskside module's number 1. */
    assert_module_nvram(dir, "module0-nvram", 0, 1, 0);
    remove_tree(dir);
}

/*
 * An Onyx2 deskside's graphics slot keeps its InfiniteReality pipe's flash:
 * 512 KiB, zero apart from the header's state word, 2 big-endian at
 * 0x7ff1c, which the PROM requires before it accepts the pipe.
 */
static void test_onyx2_kona_flash(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 1048576 }, { "node1-flash", 1048576 },
        { "io0-flash", 1048576 }, { "nvram0", 32768 },
        { "nvram0-clock", 16 }, { "module0-nvram", 2048 },
        { "io4-kona-flash", 524288 },
    };
    g_autofree char *dir = create_and_check(
        "onyx2", "onyx2,topology=onyx2-deskside,nodes=2", true, stores,
        ARRAY_SIZE(stores));
    gsize size;
    g_autofree uint8_t *flash = read_store(dir, "io4-kona-flash", &size);

    g_assert_cmpuint(size, ==, 524288);
    g_assert_cmphex(ldl_be_p(flash + 0x7ff1c), ==, 2);
    assert_all(flash, 0, 0x7ff1c, 0);
    assert_all(flash, 0x7ff20, size, 0);
    remove_tree(dir);
}

static void test_origin200_gigachannel(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 1048576 }, { "io0-flash", 1048576 },
        { "nvram0", 32768 }, { "nvram0-clock", 16 },
        { "module0-nvram", 2048 },
    };
    g_autofree char *dir = create_and_check(
        "o200-gigachannel", "origin200,topology=origin200-gigachannel,nodes=1",
        true, stores, ARRAY_SIZE(stores));

    remove_tree(dir);
}

static void check_ip35_flash(const char *dir)
{
    gsize size;
    g_autofree uint8_t *flash = read_store(dir, "node0-flash", &size);

    assert_starts_with_payload(flash, 0x27);
    g_assert_cmphex(flash[IP35_PROM_SIZE - 1], ==,
                    (uint8_t)(0x27 + (IP35_PROM_SIZE - 1) * 7));
    assert_all(flash, IP35_PROM_SIZE, IP35_PROM_SIZE + 0x1000, 0xff);
    /* The blank-media label in position one, and nothing else there. */
    g_assert_cmphex(ldl_be_p(flash + IP35_LABEL), ==, 0x504c4f49);
    assert_all(flash, 8 * 1024 * 1024, IP35_LABEL - 0x10, 0xff);
}

static void test_origin300(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 16777216 }, { "nvram0", 32768 },
        { "nvram0-clock", 16 },
    };
    g_autofree char *dir = create_and_check(
        "o300", "origin300,topology=origin300,nodes=1", false, stores,
        ARRAY_SIZE(stores));

    check_ip35_flash(dir);
    assert_zero_store(dir, "nvram0");
    remove_tree(dir);
}

/* An IP35 PROM the machine would refuse as -bios is refused here too. */
static void test_ip35_prom_size(void)
{
    g_autofree char *dir = scratch_path("o300-short");
    ToolRun run = run_tool("--machine", "origin300,topology=origin300,nodes=1",
                           "--boot-prom", boot_prom, dir, NULL);
    g_autofree char *message = g_strdup_printf(
        "boot PROM '%s' decodes to %d bytes; origin300,topology=origin300,"
        "nodes=1 needs exactly %d", boot_prom, PAYLOAD_SIZE, IP35_PROM_SIZE);

    assert_refused(&run, message);
    tool_run_clear(&run);
    g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

static void test_fuel(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 16777216 }, { "nvram0", 32768 },
        { "nvram0-clock", 16 },
    };
    g_autofree char *dir = create_and_check(
        "fuel", "fuel,topology=fuel,nodes=1", false, stores,
        ARRAY_SIZE(stores));

    check_ip35_flash(dir);
    remove_tree(dir);
}

static void test_octane(void)
{
    static const StoreSize stores[] = {
        { "node0-flash", 1048576 }, { "nvram0", 512 },
        { "nvram0-clock", 24 },
    };
    g_autofree char *dir = create_and_check(
        "octane", "octane,topology=octane,nodes=1", false, stores,
        ARRAY_SIZE(stores));
    gsize size;
    g_autofree uint8_t *flash = read_store(dir, "node0-flash", &size);

    assert_starts_with_payload(flash, 0x27);
    assert_all(flash, PAYLOAD_SIZE, size, 0xff);
    assert_zero_store(dir, "nvram0");
    remove_tree(dir);
}

/*
 * A version-6 single-segment container holding the PAYLOAD_SIZE-byte
 * pattern make_image() writes for @seed, stored rather than compressed.
 * The header fields the decoder checks are filled; the others are
 * arbitrary.
 */
static char *make_container(const char *name, uint8_t seed)
{
    const size_t header = 4096;
    g_autofree uint8_t *bytes = g_malloc0(header + PAYLOAD_SIZE);
    char *path = scratch_path(name);
    uint64_t sum = 0;
    size_t i;

    for (i = 0; i < PAYLOAD_SIZE; i++) {
        bytes[header + i] = seed + i * 7;
        sum += bytes[header + i];
    }
    stl_be_p(bytes + 0x3c, 156);
    stq_be_p(bytes + 0x40, 0x4a464b535743534dULL);
    stq_be_p(bytes + 0x48, 6);
    stq_be_p(bytes + 0x50, header + PAYLOAD_SIZE);
    stl_be_p(bytes + 0x7c, 1);
    memcpy(bytes + 0x80, "synthetic-image", 16);
    stq_be_p(bytes + 0x90, 0x10);
    stq_be_p(bytes + 0x98, header);
    stq_be_p(bytes + 0xb0, PAYLOAD_SIZE);
    stq_be_p(bytes + 0xc0, sum);
    stq_be_p(bytes + 0xd0, PAYLOAD_SIZE);
    g_assert_true(g_file_set_contents(path, (char *)bytes,
                                      header + PAYLOAD_SIZE, NULL));
    return path;
}

/*
 * The Octane boot flash holds a container's decoded payload, as the
 * machine's -bios does, not the container file.
 */
static void test_octane_container(void)
{
    g_autofree char *prom = make_container("ip30-container", 0x31);
    g_autofree char *dir = scratch_path("octane-container");
    ToolRun run = run_tool("--machine", "octane,topology=octane,nodes=1",
                           "--boot-prom", prom, dir, NULL);
    gsize size;
    g_autofree uint8_t *flash = NULL;

    g_assert_true(run_succeeded(&run));
    tool_run_clear(&run);
    flash = read_store(dir, "node0-flash", &size);
    g_assert_cmpuint(size, ==, 1048576);
    assert_starts_with_payload(flash, 0x31);
    assert_all(flash, PAYLOAD_SIZE, size, 0xff);
    remove_tree(dir);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/*
 * The fresh image the Timekeeper datasheet gives for two dates: a month
 * whose leap-cycle record is 1 and one whose record is 2.
 */
static void test_fresh_ip27_nvram(void)
{
    g_autofree uint8_t *cell = g_malloc(M48T35_SIZE);
    struct tm september_2026 = { .tm_year = 126, .tm_mon = 8, .tm_mday = 1 };
    struct tm april_2024 = { .tm_year = 124, .tm_mon = 3, .tm_mday = 1 };
    size_t i;

    m48t35_ip27_fresh_image(cell, &september_2026);
    g_assert_cmphex(cell[0x3883], ==, 1);
    g_assert_cmphex(cell[0x4bc], ==, 0x6d);
    for (i = 0; i < M48T35_SIZE; i++) {
        if (i != 0x3883 && i != 0x4bc) {
            g_assert_cmphex(cell[i], ==, 0);
        }
    }
    m48t35_ip27_fresh_image(cell, &april_2024);
    g_assert_cmphex(cell[0x3883], ==, 2);
    g_assert_cmphex(cell[0x4bc], ==, 0xad);
}

/* "DIR/" creates DIR, not something inside it. */
static void test_trailing_slash(void)
{
    g_autofree char *dir = scratch_path("slash");
    g_autofree char *arg = g_strdup_printf("%s/", dir);
    ToolRun run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                           "--boot-prom", boot_prom, arg, NULL);

    g_assert_true(run_succeeded(&run));
    tool_run_clear(&run);
    g_assert_cmpuint(count_entries(dir), ==, 4);
    remove_tree(dir);
}

static void assert_existing_refused(const char *path)
{
    ToolRun run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                           "--boot-prom", boot_prom, path, NULL);
    g_autofree char *message = g_strdup_printf("'%s' already exists", path);

    assert_refused(&run, message);
    tool_run_clear(&run);
}

/* Whatever is already at DIR stays as it was, and nothing joins it. */
static void test_existing_refused(void)
{
    g_autofree char *parent = scratch_path("existing");
    g_autofree char *dir = g_build_filename(parent, "dir", NULL);
    g_autofree char *file = g_build_filename(parent, "file", NULL);
    g_autofree char *contents = NULL;
    unsigned int entries = 2;
#ifndef _WIN32
    /* Symbolic links need privileges a Windows test cannot assume. */
    g_autofree char *link = g_build_filename(parent, "link", NULL);
    g_autofree char *dangling = g_build_filename(parent, "dangling", NULL);
#endif

    g_assert_cmpint(g_mkdir(parent, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(file, "keep", -1, NULL));
#ifndef _WIN32
    g_assert_cmpint(symlink(dir, link), ==, 0);
    g_assert_cmpint(symlink("nowhere", dangling), ==, 0);
    entries += 2;
#endif

    assert_existing_refused(dir);
    assert_existing_refused(file);
#ifndef _WIN32
    assert_existing_refused(link);
    assert_existing_refused(dangling);
#endif

    g_assert_cmpuint(count_entries(dir), ==, 0);
    g_assert_true(g_file_get_contents(file, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "keep");
    g_assert_cmpuint(count_entries(parent), ==, entries);
#ifndef _WIN32
    g_assert_cmpint(g_unlink(dangling), ==, 0);
    g_assert_cmpint(g_unlink(link), ==, 0);
#endif
    g_assert_cmpint(g_unlink(file), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_assert_cmpint(g_rmdir(parent), ==, 0);
}

#ifndef _WIN32
/*
 * Cap the files the tool may write well under a flash part, so the first
 * store fails with EFBIG part-way through the run. Windows has no
 * per-process file-size limit, so the case runs on POSIX hosts only.
 */
static void limit_file_size(gpointer opaque)
{
    struct rlimit limit = { .rlim_cur = 65536, .rlim_max = 65536 };

    signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &limit);
}

/* A failed write leaves neither DIR nor its private directory behind. */
static void test_failure_cleanup(void)
{
    g_autofree char *parent = scratch_path("failure");
    g_autofree char *dir = g_build_filename(parent, "machine", NULL);
    const char *args[] = {
        "--machine", "origin200,topology=origin200,nodes=1",
        "--boot-prom", boot_prom, dir, NULL,
    };
    ToolRun run;

    g_assert_cmpint(g_mkdir(parent, 0700), ==, 0);
    run = run_toolv(limit_file_size, args);
    assert_refused(&run, "cannot write 'node0-flash.raw'");
    tool_run_clear(&run);
    g_assert_cmpuint(count_entries(parent), ==, 0);
    g_assert_cmpint(g_rmdir(parent), ==, 0);
}
#endif

/* An image that does not decode stops the run before any file. */
static void test_bad_image_cleanup(void)
{
    g_autofree char *parent = scratch_path("bad-image");
    g_autofree char *dir = g_build_filename(parent, "machine", NULL);
    g_autofree char *bad = make_image("bad-prom", 4096, 0);
    ToolRun run;

    g_assert_cmpint(g_mkdir(parent, 0700), ==, 0);
    {
        g_autofree uint8_t *bytes = g_malloc0(4096);

        stq_be_p(bytes + 0x40, 0x4a464b535743534dULL);
        g_assert_true(g_file_set_contents(bad, (char *)bytes, 4096, NULL));
    }
    run = run_tool("--machine", "origin200,topology=origin200,nodes=1",
                   "--boot-prom", bad, dir, NULL);
    g_assert_false(run_succeeded(&run));
    tool_run_clear(&run);
    g_assert_cmpuint(count_entries(parent), ==, 0);

    g_assert_cmpint(g_rmdir(parent), ==, 0);
    g_assert_cmpint(g_unlink(bad), ==, 0);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);
    tool = g_getenv("QEMU_SGI_MACHINE_INIT");
    g_assert_nonnull(tool);
    scratch = g_dir_make_tmp("sgi-machine-init-XXXXXX", &error);
    g_assert_no_error(error);

    g_test_add_func("/sgi-machine-init/help", test_help);
    g_test_add_func("/sgi-machine-init/machine-help", test_machine_help);
    g_test_add_func("/sgi-machine-init/usage-errors", test_usage_errors);
    g_test_add_func("/sgi-machine-init/unknown-machine",
                    test_unknown_machine);
    g_test_add_func("/sgi-machine-init/inputs-checked", test_inputs_checked);
    g_test_add_func("/sgi-machine-init/origin200", test_origin200);
    g_test_add_func("/sgi-machine-init/origin200-processor",
                    test_origin200_processor);
    g_test_add_func("/sgi-machine-init/origin2000", test_origin2000);
    g_test_add_func("/sgi-machine-init/onyx2-kona-flash",
                    test_onyx2_kona_flash);
    g_test_add_func("/sgi-machine-init/origin200-gigachannel",
                    test_origin200_gigachannel);
    g_test_add_func("/sgi-machine-init/origin300", test_origin300);
    g_test_add_func("/sgi-machine-init/ip35-prom-size", test_ip35_prom_size);
    g_test_add_func("/sgi-machine-init/fuel", test_fuel);
    g_test_add_func("/sgi-machine-init/octane", test_octane);
    g_test_add_func("/sgi-machine-init/octane-container",
                    test_octane_container);
    g_test_add_func("/sgi-machine-init/fresh-ip27-nvram",
                    test_fresh_ip27_nvram);
    g_test_add_func("/sgi-machine-init/trailing-slash", test_trailing_slash);
    g_test_add_func("/sgi-machine-init/existing-refused",
                    test_existing_refused);
#ifndef _WIN32
    g_test_add_func("/sgi-machine-init/failure-cleanup",
                    test_failure_cleanup);
#endif
    g_test_add_func("/sgi-machine-init/bad-image-cleanup",
                    test_bad_image_cleanup);
    boot_prom = make_image("boot-prom", PAYLOAD_SIZE, 0x27);
    ip35_prom = make_image("ip35-prom", IP35_PROM_SIZE, 0x27);
    io_prom = make_image("io-prom", IO_PROM_SIZE, 0x06);
    ret = g_test_run();
    g_assert_cmpint(g_unlink(boot_prom), ==, 0);
    g_assert_cmpint(g_unlink(io_prom), ==, 0);
    g_assert_cmpint(g_unlink(ip35_prom), ==, 0);
    g_free(boot_prom);
    g_free(ip35_prom);
    g_free(io_prom);

    g_assert_cmpint(g_rmdir(scratch), ==, 0);
    g_free(scratch);
    return ret;
}
