/*
 * Origin 200 ELSC configuration NVRAM: the store behind I2C 0x50-0x57 on the
 * Hub's own bus, as the configuration NVRAM section of the project's PCF8584
 * and ELSC datasheet specifies it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define I2C_DATA        0x220000
#define I2C_CTRL        0x220008

#define CTL_PIN  0x80
#define CTL_ESO  0x40
#define CTL_STA  0x04
#define CTL_STO  0x02
#define CTL_ACK  0x01

#define CTL_START   (CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK)
#define CTL_RESTART (CTL_ESO | CTL_STA | CTL_ACK)
#define CTL_NAK     CTL_ESO
#define CTL_STOP    (CTL_PIN | CTL_ESO | CTL_STO)

#define STS_LRB  0x08

#define NVRAM_FIRST     0x50
#define NVRAM_BYTES     2048
#define NVRAM_MARKER    0x700
#define NVRAM_PASSWORD  0x701
#define NVRAM_CONFIG    0x707
#define NVRAM_MODULE    0x708
#define NVRAM_CLOCK     0x70b

#define MACHINE         "-M origin200"

/*
 * The processor whose own Hub window the helpers below use: processor 0 is
 * on node 0, and on a two-module Origin 200 processor 2 is on node 1.
 */
static unsigned int hub_cpu;

static void hub_write(QTestState *qts, uint64_t offset, uint8_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, hub_cpu, HUB_LOCAL + offset,
                                     bytes, sizeof(bytes)));
}

static uint8_t hub_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, hub_cpu, HUB_LOCAL + offset,
                                    bytes, sizeof(bytes)));
    return (uint8_t)ldq_be_p(bytes);
}

static uint8_t nvram_address(uint16_t offset)
{
    return NVRAM_FIRST + (offset >> 8);
}

/* Address @address for writing and send the word address @word. */
static void begin_write(QTestState *qts, uint8_t address, uint8_t word)
{
    hub_write(qts, I2C_CTRL, CTL_PIN | CTL_ESO);
    hub_write(qts, I2C_DATA, address << 1);
    hub_write(qts, I2C_CTRL, CTL_START);
    g_assert_cmphex(hub_read(qts, I2C_CTRL) & STS_LRB, ==, 0);
    hub_write(qts, I2C_DATA, word);
    g_assert_cmphex(hub_read(qts, I2C_CTRL) & STS_LRB, ==, 0);
}

static void send_data(QTestState *qts, uint8_t value)
{
    hub_write(qts, I2C_DATA, value);
    g_assert_cmphex(hub_read(qts, I2C_CTRL) & STS_LRB, ==, 0);
}

static void stop(QTestState *qts)
{
    hub_write(qts, I2C_CTRL, CTL_STOP);
}

/* Write @count bytes from @offset in one transaction, as the firmware does. */
static void nvram_write(QTestState *qts, uint16_t offset,
                        const uint8_t *data, size_t count)
{
    g_assert_cmpuint(offset + count, <=, NVRAM_BYTES);
    begin_write(qts, nvram_address(offset), offset & 0xff);
    for (size_t i = 0; i < count; i++) {
        send_data(qts, data[i]);
    }
    stop(qts);
}

static void nvram_write_byte(QTestState *qts, uint16_t offset, uint8_t value)
{
    nvram_write(qts, offset, &value, 1);
}

/*
 * One byte as the firmware reads it: the word address written, a repeated
 * START to the same address for reading, and the first data-register read
 * discarded because it only starts the transfer.
 */
static uint8_t nvram_read_at(QTestState *qts, uint8_t address, uint8_t word)
{
    uint8_t value;

    begin_write(qts, address, word);
    hub_write(qts, I2C_CTRL, CTL_RESTART);
    hub_write(qts, I2C_DATA, (address << 1) | 1);
    g_assert_cmphex(hub_read(qts, I2C_CTRL) & STS_LRB, ==, 0);
    (void)hub_read(qts, I2C_DATA);
    hub_write(qts, I2C_CTRL, CTL_NAK);
    value = hub_read(qts, I2C_DATA);
    stop(qts);
    return value;
}

static uint8_t nvram_read(QTestState *qts, uint16_t offset)
{
    g_assert_cmpuint(offset, <, NVRAM_BYTES);
    return nvram_read_at(qts, nvram_address(offset), offset & 0xff);
}

/* A caller's store: a 2048-byte raw file, in a directory of its own. */
typedef struct Store {
    char *dir;
    char *path;
} Store;

static void store_create(Store *store, const uint8_t *contents)
{
    uint8_t zero[NVRAM_BYTES] = { };

    store->dir = g_dir_make_tmp("sn0-elsc-nvram-XXXXXX", NULL);
    g_assert_nonnull(store->dir);
    store->path = g_build_filename(store->dir, "module0-nvram.raw", NULL);
    g_assert_true(g_file_set_contents(store->path,
                                      (const char *)(contents ?: zero),
                                      NVRAM_BYTES, NULL));
}

static void store_read(const Store *store, uint8_t *contents)
{
    g_autofree char *data = NULL;
    gsize length;

    g_assert_true(g_file_get_contents(store->path, &data, &length, NULL));
    g_assert_cmpuint(length, ==, NVRAM_BYTES);
    memcpy(contents, data, NVRAM_BYTES);
}

static void store_destroy(Store *store)
{
    unlink(store->path);
    g_assert_cmpint(rmdir(store->dir), ==, 0);
    g_free(store->path);
    g_free(store->dir);
}

static QTestState *start_with_store(const Store *store, const char *extra)
{
    return qtest_initf(MACHINE ",module0-nvram=nv "
                       "-blockdev driver=raw,node-name=nv,file.driver=file,"
                       "file.filename=%s%s", store->path, extra);
}

/*
 * A new store holds what a master controller writes at its first power-up,
 * the Origin 200 build's bytes included: automatic power-up, no module
 * number, and the full-clock midplane's clock-select default.
 */
static void test_new_store_defaults(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    g_assert_cmphex(nvram_read(qts, NVRAM_MARKER), ==, 0x37);
    g_assert_cmphex(nvram_read(qts, NVRAM_PASSWORD + 0), ==, 'n');
    g_assert_cmphex(nvram_read(qts, NVRAM_PASSWORD + 1), ==, 'o');
    g_assert_cmphex(nvram_read(qts, NVRAM_PASSWORD + 2), ==, 'n');
    g_assert_cmphex(nvram_read(qts, NVRAM_PASSWORD + 3), ==, 'e');
    g_assert_cmphex(nvram_read(qts, 0x705), ==, 0);
    g_assert_cmphex(nvram_read(qts, 0x706), ==, 0);
    g_assert_cmphex(nvram_read(qts, NVRAM_CONFIG), ==, 0x02);
    g_assert_cmphex(nvram_read(qts, NVRAM_MODULE), ==, 0);
    g_assert_cmphex(nvram_read(qts, 0x709), ==, 0);
    g_assert_cmphex(nvram_read(qts, 0x70a), ==, 0);
    /* The full-clock midplane default the Origin 200 build always writes. */
    g_assert_cmphex(nvram_read(qts, NVRAM_CLOCK), ==, 0x78);
    for (uint16_t offset = 0x70c; offset < 0x710; offset++) {
        g_assert_cmphex(nvram_read(qts, offset), ==, 0);
    }
    /* The inventory log region starts empty. */
    g_assert_cmphex(nvram_read(qts, 0), ==, 0);
    qtest_quit(qts);
}

/*
 * The block is the I2C address and the byte within it the word address:
 * 0x50 word 0 is offset 0, 0x57 word 0 is 0x700, 0x57 word 8 is 0x708.
 */
static void test_block_addressing(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    nvram_write_byte(qts, 0x000, 0x11);
    nvram_write_byte(qts, 0x1ff, 0x22);
    nvram_write_byte(qts, 0x6ff, 0x33);
    nvram_write_byte(qts, NVRAM_MODULE, 0x44);
    g_assert_cmphex(nvram_read_at(qts, 0x50, 0x00), ==, 0x11);
    g_assert_cmphex(nvram_read_at(qts, 0x51, 0xff), ==, 0x22);
    g_assert_cmphex(nvram_read_at(qts, 0x56, 0xff), ==, 0x33);
    g_assert_cmphex(nvram_read_at(qts, 0x57, 0x00), ==, 0x37);
    g_assert_cmphex(nvram_read_at(qts, 0x57, 0x08), ==, 0x44);
    /* Neighbouring blocks are distinct bytes. */
    g_assert_cmphex(nvram_read_at(qts, 0x51, 0x00), ==, 0);
    g_assert_cmphex(nvram_read_at(qts, 0x50, 0xff), ==, 0);
    qtest_quit(qts);
}

/*
 * The controller initializes its block with bytes 0-11 in one transaction
 * at 0x57, word address 0.
 */
static void test_twelve_byte_block_write(const void *data)
{
    static const uint8_t block[12] = {
        0x37, 'a', 'b', 'c', 'd', 0x05, 0x06, 0x01, 0x09, 0x0a, 0x0b, 0x0c,
    };
    QTestState *qts = qtest_init(MACHINE);

    nvram_write(qts, NVRAM_MARKER, block, sizeof(block));
    for (unsigned i = 0; i < sizeof(block); i++) {
        g_assert_cmphex(nvram_read(qts, NVRAM_MARKER + i), ==, block[i]);
    }
    /* Byte 12 was not part of the write. */
    g_assert_cmphex(nvram_read(qts, NVRAM_MARKER + 12), ==, 0);
    qtest_quit(qts);
}

/* Non-volatile: a machine reset leaves the store as the guest wrote it. */
static void test_survives_system_reset(const void *data)
{
    QTestState *qts = qtest_init(MACHINE);

    nvram_write_byte(qts, 0x010, 0x5a);
    nvram_write_byte(qts, NVRAM_MODULE, 1);
    qtest_system_reset(qts);
    g_assert_cmphex(nvram_read(qts, 0x010), ==, 0x5a);
    g_assert_cmphex(nvram_read(qts, NVRAM_MODULE), ==, 1);
    g_assert_cmphex(nvram_read(qts, NVRAM_CONFIG), ==, 0x02);
    qtest_quit(qts);
}

/* A caller's image wins over the product's seed, byte for byte. */
static void test_caller_image_wins(const void *data)
{
    uint8_t image[NVRAM_BYTES];
    Store store;
    QTestState *qts;

    for (unsigned i = 0; i < NVRAM_BYTES; i++) {
        image[i] = i * 7 + 3;
    }
    image[NVRAM_MODULE] = 5;
    image[NVRAM_CONFIG] = 0;
    store_create(&store, image);
    qts = start_with_store(&store, "");
    g_assert_cmphex(nvram_read(qts, NVRAM_MODULE), ==, 5);
    g_assert_cmphex(nvram_read(qts, NVRAM_CONFIG), ==, 0);
    g_assert_cmphex(nvram_read(qts, NVRAM_MARKER), ==, image[NVRAM_MARKER]);
    g_assert_cmphex(nvram_read(qts, 0x123), ==, image[0x123]);
    qtest_quit(qts);
    store_destroy(&store);
}

/*
 * Writes reach the caller's file, and a second QEMU process on the same file
 * reads back what the first wrote, the log region and block 7 alike.
 */
static void test_persists_across_processes(const void *data)
{
    static const uint8_t record[] = {
        'I', 'P', '2', '7', ' ', 'n', '0', ' ', '\n', '\r', 0,
    };
    uint8_t contents[NVRAM_BYTES];
    Store store;
    QTestState *qts;

    store_create(&store, NULL);
    qts = start_with_store(&store, "");
    nvram_write(qts, 0, record, sizeof(record));
    nvram_write_byte(qts, NVRAM_MODULE, 1);
    nvram_write_byte(qts, NVRAM_MARKER, 0x37);
    qtest_quit(qts);

    store_read(&store, contents);
    g_assert_cmpmem(contents, sizeof(record), record, sizeof(record));
    g_assert_cmphex(contents[NVRAM_MODULE], ==, 1);
    g_assert_cmphex(contents[NVRAM_MARKER], ==, 0x37);

    qts = start_with_store(&store, "");
    for (unsigned i = 0; i < sizeof(record); i++) {
        g_assert_cmphex(nvram_read(qts, i), ==, record[i]);
    }
    g_assert_cmphex(nvram_read(qts, NVRAM_MODULE), ==, 1);
    /* The zero-filled file was not reseeded: no configuration default. */
    g_assert_cmphex(nvram_read(qts, NVRAM_CONFIG), ==, 0);
    qtest_quit(qts);
    store_destroy(&store);
}

/* Migration carries the store and the controller's place in it. */
static void test_migration(const void *data)
{
    g_autofree char *directory = g_dir_make_tmp("sn0-elsc-nvram-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(directory, "migration.sock",
                                               NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = qtest_init(MACHINE);
    QTestState *to = qtest_init(MACHINE " -incoming defer");

    nvram_write_byte(from, 0x321, 0xa5);
    nvram_write_byte(from, NVRAM_MODULE, 3);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_migrate_resume(to);
    g_assert_cmphex(nvram_read(to, 0x321), ==, 0xa5);
    g_assert_cmphex(nvram_read(to, NVRAM_MODULE), ==, 3);
    g_assert_cmphex(nvram_read(to, NVRAM_CONFIG), ==, 0x02);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/*
 * Each module of a two-module Origin 200 has a store of its own, and the
 * slave module's is reachable: node 1's processors address it through their
 * own Hub's I2C controller, as node 0's address the master's.
 */
static void test_two_module_stores(const void *data)
{
    Store first, second;
    uint8_t image[NVRAM_BYTES] = { };
    QTestState *qts;

    image[NVRAM_MODULE] = 9;
    store_create(&first, NULL);
    store_create(&second, image);
    qts = qtest_initf("-M origin200,topology=origin200-dual,nodes=2,"
                      "module0-nvram=a,module1-nvram=b -smp 4 -m 128M "
                      "-blockdev driver=raw,node-name=a,file.driver=file,"
                      "file.filename=%s "
                      "-blockdev driver=raw,node-name=b,file.driver=file,"
                      "file.filename=%s", first.path, second.path);
    hub_cpu = 0;
    nvram_write_byte(qts, NVRAM_MODULE, 4);
    hub_cpu = 2;
    g_assert_cmphex(nvram_read(qts, NVRAM_MODULE), ==, 9);
    nvram_write_byte(qts, 0x6f0, 0xa5);
    hub_cpu = 0;
    qtest_quit(qts);

    store_read(&first, image);
    g_assert_cmphex(image[NVRAM_MODULE], ==, 4);
    g_assert_cmphex(image[0x6f0], ==, 0);
    store_read(&second, image);
    g_assert_cmphex(image[NVRAM_MODULE], ==, 9);
    g_assert_cmphex(image[0x6f0], ==, 0xa5);
    store_destroy(&first);
    store_destroy(&second);
}

/*
 * A new slave-module store is seeded like the master's, an approximation:
 * the slave controller does not initialize its store, so its contents are
 * unknown, and which store the slave Hub reaches is unknown too.
 */
static void test_slave_module_store_seed(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,topology=origin200-dual,"
                                 "nodes=2 -smp 4 -m 128M");

    hub_cpu = 2;
    g_assert_cmphex(nvram_read(qts, NVRAM_MARKER), ==, 0x37);
    g_assert_cmphex(nvram_read(qts, NVRAM_CONFIG), ==, 0x02);
    g_assert_cmphex(nvram_read(qts, NVRAM_CLOCK), ==, 0x78);
    hub_cpu = 0;
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/elsc-nvram/new-store-defaults", NULL,
                        test_new_store_defaults);
    qtest_add_data_func("/sn0/elsc-nvram/block-addressing", NULL,
                        test_block_addressing);
    qtest_add_data_func("/sn0/elsc-nvram/twelve-byte-block-write", NULL,
                        test_twelve_byte_block_write);
    qtest_add_data_func("/sn0/elsc-nvram/survives-system-reset", NULL,
                        test_survives_system_reset);
    qtest_add_data_func("/sn0/elsc-nvram/caller-image-wins", NULL,
                        test_caller_image_wins);
    qtest_add_data_func("/sn0/elsc-nvram/persists-across-processes", NULL,
                        test_persists_across_processes);
    qtest_add_data_func("/sn0/elsc-nvram/migration", NULL, test_migration);
    qtest_add_data_func("/sn0/elsc-nvram/two-module-stores", NULL,
                        test_two_module_stores);
    qtest_add_data_func("/sn0/elsc-nvram/slave-module-store-seed", NULL,
                        test_slave_module_store_seed);
    return g_test_run();
}
