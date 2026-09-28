/*
 * Test Origin 200 (IP27) memory, boot flash placement and specified
 * pre-firmware contents. Three flash views alias one region; physical
 * addresses reach memory and the Hub special space starts one window up.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-test.h"

/* Node zero's window, and the space the Hub answers uncached references
 * in. */
#define NODE0           0x0000000000ULL
#define LBOOT_OFFSET    0x10000000ULL
#define RBOOT_OFFSET    0x30000000ULL

/* The flash at reset, through local LBOOT, and through node-explicit
 * RBOOT. */
#define FLASH_RESET     (NODE0 + 0x1fc00000)
#define FLASH_HUB       (SPACE_HUB + NODE0 + 0x10000000)
#define FLASH_REMOTE    (SPACE_HUB + NODE0 + 0x30000000)
/* The flash base in Hub space, where the real PROM fetches relocated
 * code. */
#define FLASH_HUB_LOCAL (SPACE_HUB + NODE0 + 0x1fc00000)

/* AMD 29F080 command addresses and the first sector, erased at reset. */
#define FLASH_UNLOCK1       0x5555
#define FLASH_UNLOCK2       0x2aaa
#define FLASH_LOG_SECTOR    0xe0000
#define FLASH_SECTOR_SIZE   0x10000

/*
 * One plain origin200 serves every case that names no memory size, no
 * firmware image, no trace file and no second node, started in main().
 */
static QTestState *qts;

/*
 * Open such a case. Flash contents deliberately survive a machine reset;
 * that is what test_flash_program_and_erase asserts, so the reset alone
 * would not prove a case starts clean. Reading the log sector erased does: a
 * chip left in autoselect or a sector left programmed both answer something
 * other than an erased byte here.
 */
static void address_map_reset(void)
{
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + FLASH_LOG_SECTOR), ==,
                    0xff);
}

static void flash_command(QTestState *qts, uint32_t offset, uint8_t value)
{
    /* Hub presents the flash's byte-wide command port on 64-bit cycles. */
    qtest_writeq(qts, FLASH_HUB + 8 * offset, value);
}

/*
 * The directory/ECC back doors: nothing about coherence is modelled, so the
 * property is the diagnostic's own round trip.
 */
#define BACKDOOR_ECC    (SPACE_HUB + NODE0 + 0x80000000ULL)
#define BACKDOOR_DIR    (SPACE_HUB + NODE0 + 0xc0000000ULL)
#define BACKDOOR_SIZE   0x40000000ULL

/* The configuration block, at flash offset 0x60. */
#define CONFIG_OFFSET   0x60
#define CONFIG_MAGIC_OFF 0x68
#define CONFIG_MAGIC    0x69703237636f6e66ULL    /* "ip27conf" */

#define NODE0_LBOOT (SPACE_HUB + NODE0 + LBOOT_OFFSET)
#define NODE0_RBOOT (SPACE_HUB + NODE0 + RBOOT_OFFSET)

#define SN0_DUAL_CHASSIS_FIRMWARE_ARGS \
    "-M origin200,topology=origin200-dual -m 128M " \
    "-smp 4"

#define FIELD_MAGIC     0x08
#define FIELD_CPU_FREQ  0x10
#define FIELD_HUB_FREQ  0x18
#define FIELD_RTC_FREQ  0x20
#define FIELD_FPROM_CYC 0x2c
#define FIELD_MACHTYPE  0x30
#define FIELD_CHECKSUM  0x34
#define FIELD_FPROM_WR  0x3c
#define CONFIG_SIZE     0x50

/*
 * Nothing fills a node's boot flash but -bios or a pflash drive now, so a
 * test that reads the configuration block back needs one of its own: a
 * config block with the real "ip27conf" magic at the real IP27 offset, the
 * pattern every stock PROM image carries there.
 */
static char *write_config_block_fixture(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(CONFIG_OFFSET + CONFIG_SIZE);
    char *path = NULL;
    int fd;

    stq_be_p(image + CONFIG_OFFSET + FIELD_MAGIC, CONFIG_MAGIC);
    fd = g_file_open_tmp("sn0-address-map-config-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      CONFIG_OFFSET + CONFIG_SIZE, &error));
    g_assert_no_error(error);
    return path;
}

static void test_flash_three_views(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    g_autofree char *args =
        g_strdup_printf("-M origin200 -bios %s", path);
    QTestState *qts = qtest_init(args);
    int i;

    /*
 * The same bytes have to appear at all three addresses. Comparing the whole
 * configuration block rather than one field is what would catch two regions
 * built separately from the same values.
 */
    for (i = 0; i < 0x50; i += 4) {
        uint32_t reset_view = qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET + i);
        uint32_t hub_view = qtest_readl(qts, FLASH_HUB + CONFIG_OFFSET + i);
        uint32_t remote_view =
            qtest_readl(qts, FLASH_REMOTE + CONFIG_OFFSET + i);

        g_assert_cmphex(reset_view, ==, hub_view);
        g_assert_cmphex(reset_view, ==, remote_view);
    }

    g_assert_cmphex(qtest_readq(qts, FLASH_HUB + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);
    g_assert_cmphex(qtest_readq(qts, FLASH_REMOTE + CONFIG_OFFSET +
                               FIELD_MAGIC), ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

/*
 * A supplied -bios image carrying the real config block (the pattern
 * every stock PROM image carries at flash offset 0x60) gets three fields
 * patched to this product's own values (sn0-origin200-model.c: 15 read
 * cycles, machine type 1, 4 write cycles) while the block's total byte sum
 * stays exactly where the supplied image left it. That is the flasher's
 * whole contract, and it runs whether the image is a synthesized template or
 * a real one; sn0_load_flash must keep applying it after -bios loads a real
 * PROM image, not only when building the synthesized default.
 */
static void test_flash_configure_patches_model_fields(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    g_autofree char *args = g_strdup_printf("-M origin200 -bios %s", path);
    QTestState *qts = qtest_init(args);
    /* The fixture is all zero except the eight magic bytes "ip27conf". */
    uint8_t sum = (uint8_t)(0x69 + 0x70 + 0x32 + 0x37 +
                            0x63 + 0x6f + 0x6e + 0x66);
    int i;

    g_assert_cmphex(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                                FIELD_FPROM_CYC), ==, 15);
    g_assert_cmphex(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                                FIELD_MACHTYPE), ==, 1);
    g_assert_cmphex(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                                FIELD_FPROM_WR), ==, 4);

    for (i = 0; i < CONFIG_SIZE; i++) {
        sum -= qtest_readb(qts, FLASH_RESET + CONFIG_OFFSET + i);
    }
    g_assert_cmphex(sum, ==, 0);

    qtest_quit(qts);
}

/*
 * Node zero's own boot windows in a two-node machine, the same shape
 * test_flash_three_views checks for the single-node default. Node one's
 * NASID-qualified boot windows are not checked here: reading them this way
 * returns zero rather than node one's own flash content even though "info
 * mtree -f" shows the window placed correctly, and nothing has chased that
 * down yet. See the firmware-deletion packet's rulings.
 */
static void test_dual_node_boot_windows(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    g_autofree char *args =
        g_strdup_printf(SN0_DUAL_CHASSIS_FIRMWARE_ARGS " -bios %s", path);
    QTestState *qts = qtest_init(args);

    g_assert_cmphex(qtest_readq(qts, NODE0_LBOOT + CONFIG_MAGIC_OFF),
                    ==, CONFIG_MAGIC);
    g_assert_cmphex(qtest_readq(qts, NODE0_RBOOT + CONFIG_MAGIC_OFF),
                    ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

/*
 * 256 MB views of a 1 MB chip repeat: firmware fetches past the real size,
 * and offset zero alone cannot show the repeat.
 */
static void test_boot_windows_mirror_the_flash(const void *data)
{
    uint64_t direct, mirrored;

    address_map_reset();

    /* One repeat period past the flash's own size: the first wrap. */
    direct = qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET);
    mirrored = qtest_readq(qts, FLASH_HUB + 0x100000 + CONFIG_OFFSET);
    g_assert_cmphex(mirrored, ==, direct);
    mirrored = qtest_readq(qts, FLASH_REMOTE + 0x100000 + CONFIG_OFFSET);
    g_assert_cmphex(mirrored, ==, direct);

    /* Deep into the window, an offset a captured PROM trace really hit. */
    direct = qtest_readl(qts, FLASH_RESET + 0x2a3c);
    mirrored = qtest_readl(qts, FLASH_HUB + 0xfc02a3c);
    g_assert_cmphex(mirrored, ==, direct);
    mirrored = qtest_readl(qts, FLASH_REMOTE + 0xfc02a3c);
    g_assert_cmphex(mirrored, ==, direct);

    /* The last aligned double word of the 256 MB window still wraps. */
    direct = qtest_readq(qts, FLASH_RESET + (0x10000000 - 8) % 0x100000);
    mirrored = qtest_readq(qts, FLASH_HUB + 0x10000000 - 8);
    g_assert_cmphex(mirrored, ==, direct);
    mirrored = qtest_readq(qts, FLASH_REMOTE + 0x10000000 - 8);
    g_assert_cmphex(mirrored, ==, direct);
}

/*
 * The region-type token ("ram", "rom", "romd", "i/o", ...) that "info mtree
 * -f"'s flat view of the CPU's address space reports for the entry covering
 * address, or NULL if the flat view has nothing mapped there.
 */
static char *flat_view_region_type_at(QTestState *qts, uint64_t address)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    const char *system_view = strstr(mtree, "Root memory region: system");
    const char *system_view_end;
    const char *line;

    g_assert_nonnull(system_view);
    system_view = strchr(system_view, '\n');
    g_assert_nonnull(system_view);
    system_view++;
    system_view_end = strstr(system_view, "\nFlatView #");
    if (!system_view_end) {
        system_view_end = system_view + strlen(system_view);
    }

    for (line = system_view; line < system_view_end; ) {
        const char *line_end = strchr(line, '\n');
        uint64_t start;
        uint64_t end;
        char type[32];

        if (!line_end || line_end > system_view_end) {
            line_end = system_view_end;
        }
        if (sscanf(line, " %" SCNx64 "-%" SCNx64 " (prio %*d, %31[^)])",
                   &start, &end, type) == 3 &&
            address >= start && address <= end) {
            return g_strdup(type);
        }
        line = line_end + (line_end < system_view_end);
    }
    return NULL;
}

/*
 * Every window the processor fetches firmware from must resolve to ram, rom
 * or romd in the flat view. An i/o entry there caches no TCG translation:
 * each instruction recompiles as its own one-instruction block, and the
 * guest runs 100-300x slower while giving no outward sign anything is wrong.
 */
static void assert_flat_view_is_execution_storage(QTestState *qts,
                                                   uint64_t address)
{
    g_autofree char *type = flat_view_region_type_at(qts, address);

    g_assert_nonnull(type);
    if (strcmp(type, "ram") && strcmp(type, "rom") && strcmp(type, "romd")) {
        g_test_message("0x%" PRIx64 " resolves to a %s region, not "
                       "ram/rom/romd", address, type);
        g_assert_not_reached();
    }
}

/*
 * The PROM runs its own code through this base, and an I/O region executes
 * one instruction per block: the base must be a direct (romd) view, read off
 * the machine's own flat view rather than assumed.
 */
static void test_flash_hub_local_is_fast(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    g_autofree char *args =
        g_strdup_printf("-M origin200 -bios %s", path);
    QTestState *qts = qtest_init(args);
    g_autofree char *type = flat_view_region_type_at(qts, FLASH_HUB_LOCAL);

    g_assert_cmpstr(type, ==, "romd");

    /* The fast view still has to read the chip's real bytes at this base. */
    g_assert_cmphex(qtest_readq(qts, FLASH_HUB_LOCAL + CONFIG_OFFSET +
                               FIELD_MAGIC), ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

/*
 * The reset vector and the Hub-local fast alias are the two windows the
 * default machine's processor fetches firmware from. Neither may resolve to
 * an i/o region: a real Hub always decodes them as direct storage, and an
 * i/o entry there would run TCG uncached, 100-300x slower, with nothing
 * outwardly wrong to see.
 */
static void test_execution_windows_are_not_io(const void *data)
{
    address_map_reset();

    assert_flat_view_is_execution_storage(qts, FLASH_RESET);
    assert_flat_view_is_execution_storage(qts, FLASH_HUB_LOCAL);
}

#define ORIGIN2000_FLASH_SIZE  (1024 * 1024)
#define ORIGIN2000_FLASH_NODES 4

static char *create_origin2000_flash_image(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc(ORIGIN2000_FLASH_SIZE);
    char *path = NULL;
    int fd;

    memset(image, 0xff, ORIGIN2000_FLASH_SIZE);
    fd = g_file_open_tmp("sn0-origin2000-execution-flash-XXXXXX.bin",
                        &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      ORIGIN2000_FLASH_SIZE, &error));
    g_assert_no_error(error);
    return path;
}

/*
 * Origin 2000 boots from four node-owned pflash images rather than the -bios
 * image Origin 200 takes in the rest of this file. The reset vector and the
 * Hub-local fast alias must resolve to direct storage with a real part
 * behind them there too.
 */
static void test_origin2000_execution_windows_are_not_io(const void *data)
{
    g_autoptr(GString) args = g_string_new("-M origin2000");
    char *paths[ORIGIN2000_FLASH_NODES];
    QTestState *qts;
    unsigned int i;

    for (i = 0; i < ORIGIN2000_FLASH_NODES; i++) {
        paths[i] = create_origin2000_flash_image();
        g_string_append_printf(args,
            " -drive if=pflash,index=%u,file=%s,format=raw", i, paths[i]);
    }

    qts = qtest_init(args->str);
    assert_flat_view_is_execution_storage(qts, FLASH_RESET);
    assert_flat_view_is_execution_storage(qts, FLASH_HUB_LOCAL);
    qtest_quit(qts);

    for (i = 0; i < ORIGIN2000_FLASH_NODES; i++) {
        g_assert_cmpint(g_unlink(paths[i]), ==, 0);
        g_free(paths[i]);
    }
}

#define FLASH_CPU_IMAGE_SIZE 0x1000
#define FLASH_CPU_HANDLER    0x380
#define FLASH_CPU_CODE       0x400
#define FLASH_CPU_STATE      0x00200000

static void flash_cpu_emit(uint8_t *image, unsigned int *at, uint32_t insn)
{
    stl_be_p(image + *at, insn);
    *at += 4;
}

static void test_flash_wide_read_in_command_mode(const void *data)
{
    uint8_t image[FLASH_CPU_IMAGE_SIZE] = { 0 };
    uint8_t state[16];
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    unsigned int at = 0;
    int fd;

    /* Jump clear of the configuration block installed at offset 0x60. */
    flash_cpu_emit(image, &at,
                   0x08000000 | ((0xbfc00000 + FLASH_CPU_CODE) >> 2 &
                                 0x03ffffff));
    flash_cpu_emit(image, &at, 0);

    at = FLASH_CPU_HANDLER;
    flash_cpu_emit(image, &at, 0x3c1aa020); /* lui k0, 0xa020 */
    flash_cpu_emit(image, &at, 0x341bdead); /* ori k1, zero, 0xdead */
    flash_cpu_emit(image, &at, 0xff5b0000); /* sd k1, 0(k0) */
    flash_cpu_emit(image, &at, 0x1000ffff); /* beq zero, zero, self */
    flash_cpu_emit(image, &at, 0);

    at = FLASH_CPU_CODE;
    flash_cpu_emit(image, &at, 0x3c08bfc0); /* lui t0, 0xbfc0 */
    flash_cpu_emit(image, &at, 0x65080068); /* daddiu t0, t0, 0x68 */
    flash_cpu_emit(image, &at, 0xdd090000); /* ld t1, 0(t0) */
    flash_cpu_emit(image, &at, 0x3c1aa020); /* lui k0, 0xa020 */
    flash_cpu_emit(image, &at, 0xff490008); /* sd t1, 8(k0) */
    flash_cpu_emit(image, &at, 0x341b0055); /* ori k1, zero, 0x55 */
    flash_cpu_emit(image, &at, 0xff5b0000); /* sd k1, 0(k0) */
    flash_cpu_emit(image, &at, 0x1000ffff); /* beq zero, zero, self */
    flash_cpu_emit(image, &at, 0);

    stq_be_p(image + CONFIG_OFFSET + FIELD_MAGIC, CONFIG_MAGIC);

    fd = g_file_open_tmp("sn0-flash-wide-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    qts = qtest_initf("-M origin200 -m 64M "
                      "-cpu R10000 -bios %s -accel tcg,thread=single -S",
                      path);
    g_assert_cmpint(unlink(path), ==, 0);

    /* Leave the shared x8 flash in its I/O path before the CPU loads it. */
    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    for (unsigned int i = 0; i < 10000; i++) {
        qtest_memread(qts, FLASH_CPU_STATE, state, sizeof(state));
        if (ldq_be_p(state) != 0) {
            break;
        }
        g_usleep(1000);
    }
    qtest_quit(qts);

    g_assert_cmphex(ldq_be_p(state), ==, 0x55);
    g_assert_cmphex(ldq_be_p(state + 8), ==, CONFIG_MAGIC);
}

static void test_flash_rejects_bare_writes(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    g_autofree char *args =
        g_strdup_printf("-M origin200 -bios %s", path);
    QTestState *qts = qtest_init(args);
    uint64_t before = qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                                FIELD_MAGIC);

    /* Rejected, and without taking the machine down with it. */
    qtest_writeq(qts, FLASH_RESET + CONFIG_OFFSET + FIELD_MAGIC, 0);
    g_assert_cmphex(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, before);

    /* Still answering afterwards. */
    g_assert_cmphex(qtest_readq(qts, FLASH_HUB + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);
    g_assert_cmphex(qtest_readq(qts,
                               FLASH_REMOTE + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

static void test_flash_autoselect(const void *data)
{
    uint32_t before;

    address_map_reset();

    before = qtest_readl(qts, FLASH_HUB);

    flash_command(qts, 0, 0xf0);
    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    flash_command(qts, FLASH_UNLOCK2, 0x55);
    flash_command(qts, FLASH_UNLOCK1, 0x90);

    /*
 * Manufacturer 0x01 and device 0xd5 occupy logical bytes zero and one. Only
 * those two identity bytes are asserted: what an Am29F080 answers for higher
 * autoselect addresses (sector protection, then unspecified) is not
 * something any source we hold pins, so the bytes beyond the ID follow the
 * flash device model rather than an invented filler.
 */
    g_assert_cmphex(qtest_readb(qts, FLASH_HUB), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, FLASH_HUB + 1), ==, 0xd5);
    g_assert_cmphex(qtest_readb(qts, FLASH_REMOTE), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, FLASH_REMOTE + 1), ==, 0xd5);

    /*
 * Every view of the chip must answer command-mode reads alike, at any width:
 * the reset view is the same byte-wide chip behind the same controller, so a
 * word read must gather the same bytes the Hub windows gather rather than
 * widening one chip lookup across the bus.
 */
    g_assert_cmphex(qtest_readl(qts, FLASH_RESET), ==,
                    qtest_readl(qts, FLASH_HUB));
    g_assert_cmphex(qtest_readl(qts, FLASH_REMOTE), ==,
                    qtest_readl(qts, FLASH_HUB));
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + 1), ==, 0xd5);

    flash_command(qts, 0, 0xf0);
    g_assert_cmphex(qtest_readl(qts, FLASH_HUB), ==, before);
}

static void test_flash_program_and_erase(const void *data)
{
    const uint32_t target = FLASH_LOG_SECTOR + 0x100;

    address_map_reset();

    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target), ==, 0xff);

    /*
 * A real Am29F080 takes program and erase time, and the device model keeps
 * that: step the clock over each operation's completion timer before reading
 * data back, the way the chip's own test does.
 */
    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    flash_command(qts, FLASH_UNLOCK2, 0x55);
    flash_command(qts, FLASH_UNLOCK1, 0xa0);
    flash_command(qts, target, 0x5a);
    qtest_clock_step_next(qts);

    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    flash_command(qts, FLASH_UNLOCK2, 0x55);
    flash_command(qts, FLASH_UNLOCK1, 0xa0);
    flash_command(qts, target + 1, 0xf0);
    qtest_clock_step_next(qts);

    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target + 1), ==, 0xf0);
    g_assert_cmphex(qtest_readb(qts, FLASH_HUB + target), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FLASH_REMOTE + target), ==, 0x5a);

    /* Flash is retained state; a machine reset must not reload -bios. */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target + 1), ==, 0xf0);

    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    flash_command(qts, FLASH_UNLOCK2, 0x55);
    flash_command(qts, FLASH_UNLOCK1, 0x80);
    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    flash_command(qts, FLASH_UNLOCK2, 0x55);
    flash_command(qts, FLASH_LOG_SECTOR, 0x30);
    /*
 * Step over the 50 us additional-sector window and then the erase time by
 * explicit amounts: this machine keeps other timers armed, so stepping to
 * the next deadline would advance those instead.
 */
    qtest_clock_step(qts, 100 * 1000 * 1000);
    qtest_clock_step(qts, 100 * 1000 * 1000);

    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + FLASH_LOG_SECTOR), ==,
                    0xff);
    g_assert_cmphex(qtest_readb(qts,
                               FLASH_RESET + FLASH_LOG_SECTOR +
                               FLASH_SECTOR_SIZE - 1), ==, 0xff);
}

static void check_ram(QTestState *qts, uint64_t addr, uint64_t pattern)
{
    qtest_writeq(qts, addr, pattern);
    g_assert_cmphex(qtest_readq(qts, addr), ==, pattern);
}

static void check_not_ram(QTestState *qts, uint64_t addr)
{
    uint64_t before = qtest_readq(qts, addr);

    qtest_writeq(qts, addr, ~before);
    g_assert_cmphex(qtest_readq(qts, addr), ==, before);
}

/*
 * An IP27 bank of at most 128 MiB occupies the first 128 MiB slot. A larger
 * bank is interleaved evenly over all four slots on a 128 MiB stride.
 */
static void test_128m_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 128M");

    check_ram(qts, NODE0 + 0x1000, 0x0123456789abcdefULL);
    check_ram(qts, NODE0 + 128 * MiB - 8, 0x1281281281281281ULL);
    check_not_ram(qts, NODE0 + 128 * MiB);

    qtest_quit(qts);
}

/* One bank's address stride, kept as an independent literal. */
#define MEM_BANK_SIZE   0x20000000ULL
#define MEM_SLOT_SIZE   0x08000000ULL

/* A 256 MiB bank is four 64 MiB runs, not one contiguous run. */
static void test_256m_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 256M");
    unsigned int slot;

    for (slot = 0; slot < 4; slot++) {
        check_ram(qts, NODE0 + slot * MEM_SLOT_SIZE,
                  0x2560000000000000ULL + slot);
        check_ram(qts, NODE0 + slot * MEM_SLOT_SIZE + 64 * MiB - 8,
                  0x256fffffffffff00ULL + slot);
        check_not_ram(qts, NODE0 + slot * MEM_SLOT_SIZE + 64 * MiB);
    }

    qtest_quit(qts);
}

/*
 * An IP27 node board has no 256 MB bank: its DIMMs give 64 or 128 MB banks,
 * or 512 MB ones, and the stock PROM disables a 256 MB bank on this board.
 * So 256 MiB on the one-node Origin 2000 is two 128 MiB banks, each wholly
 * in its bank's first slot, where the Origin 200 rule above would build one
 * bank split over four 64 MiB runs.
 */
static void test_origin2000_256m_memory(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin2000,topology=origin2000-one-node -smp 2 -m 256M");
    unsigned int bank;

    for (bank = 0; bank < 2; bank++) {
        uint64_t base = NODE0 + bank * MEM_BANK_SIZE;

        check_ram(qts, base, 0x1280000000000000ULL + bank);
        check_ram(qts, base + 128 * MiB - 8, 0x128fffffffffff00ULL + bank);
        check_not_ram(qts, base + 128 * MiB);
    }
    check_not_ram(qts, NODE0 + 2 * MEM_BANK_SIZE);

    qtest_quit(qts);
}

/*
 * The explicit 512 MiB configuration is two 256 MiB banks, with the boot
 * flash's own addresses left out of the RAM map.
 */
static void test_512m_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 512M");
    unsigned int bank, slot;

    for (bank = 0; bank < 2; bank++) {
        for (slot = 0; slot < 4; slot++) {
            check_ram(qts, NODE0 + bank * MEM_BANK_SIZE +
                      slot * MEM_SLOT_SIZE,
                      0x5120000000000000ULL + bank * 4 + slot);
        }
    }

    check_not_ram(qts, NODE0 + 0x1f800000);
    check_not_ram(qts, NODE0 + 0x1fc00000);
    check_not_ram(qts, NODE0 + 0x1fd00000);

    qtest_quit(qts);
}

/*
 * A 256 MiB view of local node memory that must preserve the sparse bank
 * layout rather than turn the whole window into RAM.
 */
static void test_hub_local_memory_alias(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t prom_copy = 0x01a0a520;

    qtest_writeq(qts, prom_copy, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, SPACE_HUB + prom_copy), ==,
                    0x0123456789abcdefULL);
    qtest_writeq(qts, SPACE_HUB + prom_copy, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, prom_copy), ==,
                    0xfedcba9876543210ULL);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 256M");
    check_ram(qts, SPACE_HUB + 0x1000, 0x1000000000000001ULL);
    check_not_ram(qts, SPACE_HUB + 64 * MiB);
    check_ram(qts, SPACE_HUB + 128 * MiB + 0x1000,
              0x1000000000000002ULL);
    check_not_ram(qts, SPACE_HUB + 192 * MiB);
    qtest_quit(qts);
}

/*
 * 96 MiB is not one of origin200's accepted per-node populations (512, 256,
 * 128, 64): sgi_machine_validate_memory_population() now catches it before
 * node construction ever reaches the IP29 bank-fitting check this test used
 * to name, so the accepted-population message is what a real user sees.
 */
static void test_invalid_memory_size(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "96M",
        "-display", "none", "-bios", path, NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output,
                           "topology 'origin200' does not accept 96 MiB "
                           "per node"));
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * One bank's stride within a back-door window: an even eight-way split of
 * the 1 GB window, restated independently from the compiled product model.
 */
#define BACKDOOR_BANK_STRIDE   0x8000000ULL  /* 128 MiB */

/*
 * Firmware's presence probe: write the 0x55 pattern, write zero eight bytes
 * on (only to disturb the shared data lines), then read back. Premium if the
 * low 48 bits survived, standard if only the low 16, else absent.
 */
#define BACKDOOR_PATTERN       0x5555555555555555ULL
#define BACKDOOR_PATTERN_LOW16 0x5555ULL
#define BACKDOOR_PATTERN_LOW48 0x0000555555555555ULL

/*
 * Only the format's sixteen bits survive; checked at base, middle and top,
 * folded with the bank index so banks cannot alias.
 */
static void check_backdoor_entry(QTestState *qts, uint64_t base,
                                 unsigned int bank)
{
    uint64_t bank_base = base + (uint64_t)bank * BACKDOOR_BANK_STRIDE;
    static const uint64_t offsets[] = { 0x0, 0x1234560,
                                        BACKDOOR_BANK_STRIDE - 8 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint64_t addr = bank_base + offsets[i];
        uint64_t written = (0xdaedbaefdaedba00ULL ^ offsets[i]) + bank;
        uint64_t readback;

        qtest_writeq(qts, addr, written);
        readback = qtest_readq(qts, addr);

        g_assert_cmphex(readback & 0xffffULL, ==, written & 0xffffULL);
        g_assert_cmphex(readback & ~0xffffULL, ==, 0);
    }
}

/* Assert the classification, not the value the model chose. */
static void check_backdoor_standard_format(QTestState *qts, uint64_t addr)
{
    uint64_t readback;

    qtest_writeq(qts, addr, BACKDOOR_PATTERN);
    qtest_writeq(qts, addr + 8, 0);
    readback = qtest_readq(qts, addr);

    g_assert_cmphex(readback & 0xffffULL, ==, BACKDOOR_PATTERN_LOW16);
    g_assert_cmphex(readback & 0xffffffffffffULL, !=, BACKDOOR_PATTERN_LOW48);
}

/* Absent classifies absent; pinning the read value would freeze a choice. */
static void check_backdoor_absent(QTestState *qts, uint64_t addr)
{
    uint64_t readback;

    qtest_writeq(qts, addr, BACKDOOR_PATTERN);
    qtest_writeq(qts, addr + 8, 0);
    readback = qtest_readq(qts, addr);

    g_assert_cmphex(readback & 0xffffULL, !=, BACKDOOR_PATTERN_LOW16);
}

/*
 * Narrower accesses see the same two bytes. The window takes only naturally
 * aligned accesses, so one lies inside a single entry and either overlaps
 * its live tail or reads zero.
 */
static void check_backdoor_entry_widths(QTestState *qts, uint64_t addr)
{
    qtest_writeq(qts, addr, 0);

    qtest_writeb(qts, addr + 0, 0x11);
    qtest_writeb(qts, addr + 5, 0x22);
    qtest_writeb(qts, addr + 6, 0x33);
    qtest_writeb(qts, addr + 7, 0x44);
    g_assert_cmphex(qtest_readq(qts, addr), ==, 0x3344);
    g_assert_cmphex(qtest_readb(qts, addr + 0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, addr + 5), ==, 0);
    g_assert_cmphex(qtest_readb(qts, addr + 6), ==, 0x33);
    g_assert_cmphex(qtest_readb(qts, addr + 7), ==, 0x44);

    qtest_writew(qts, addr + 0, 0x5566);
    qtest_writew(qts, addr + 4, 0x7788);
    qtest_writew(qts, addr + 6, 0x99aa);
    g_assert_cmphex(qtest_readq(qts, addr), ==, 0x99aa);
    g_assert_cmphex(qtest_readw(qts, addr + 0), ==, 0);
    g_assert_cmphex(qtest_readw(qts, addr + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, addr + 6), ==, 0x99aa);

    qtest_writel(qts, addr + 0, 0xbbccddee);
    qtest_writel(qts, addr + 4, 0x01020304);
    g_assert_cmphex(qtest_readq(qts, addr), ==, 0x0304);
    g_assert_cmphex(qtest_readl(qts, addr + 0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, addr + 4), ==, 0x0304);
}

/*
 * A read that spans entries (the monitor, debugger, or a device copy)
 * sees each entry's two live bytes and zero between them, whether the memory
 * core splits it into aligned accesses or copies the storage.
 */
static void check_backdoor_straddling_reads(QTestState *qts, uint64_t addr)
{
    static const uint8_t expect[16] = {
        0, 0, 0, 0, 0, 0, 0x12, 0x34,
        0, 0, 0, 0, 0, 0, 0x56, 0x78,
    };
    uint8_t got[16];
    unsigned int offset, length;

    qtest_writeq(qts, addr, 0xffffffffffff1234ULL);
    qtest_writeq(qts, addr + 8, 0xffffffffffff5678ULL);

    for (offset = 0; offset < sizeof(expect); offset++) {
        for (length = 1; offset + length <= sizeof(expect); length++) {
            memset(got, 0xee, sizeof(got));
            qtest_memread(qts, addr + offset, got, length);
            g_assert_cmpmem(got, length, expect + offset, length);
        }
    }
}

/* The fold repeats storage for a narrow access as it does for a wide one. */
static void check_backdoor_narrow_fold(QTestState *qts, uint64_t base,
                                       uint64_t span)
{
    uint64_t entry = base + 0x2000;

    qtest_writeb(qts, entry + 7, 0x5a);
    g_assert_cmphex(qtest_readb(qts, entry + span + 7), ==, 0x5a);
    qtest_writew(qts, entry + span + 6, 0x1234);
    g_assert_cmphex(qtest_readw(qts, entry + 6), ==, 0x1234);
}

static void test_backdoor_entry_widths(const void *data)
{
    address_map_reset();

    check_backdoor_entry_widths(qts, BACKDOOR_DIR);
    check_backdoor_entry_widths(qts, BACKDOOR_ECC);
    check_backdoor_entry_widths(qts, BACKDOOR_DIR + 0x3456780);
    check_backdoor_straddling_reads(qts, BACKDOOR_DIR + 0x4000);
    check_backdoor_straddling_reads(qts, BACKDOOR_ECC + 16 * MiB - 8);
    check_backdoor_narrow_fold(qts, BACKDOOR_DIR, 16 * MiB);
    check_backdoor_narrow_fold(qts, BACKDOOR_ECC, 16 * MiB);
}

/*
 * A processor's back-door read is served from the window's storage by the
 * translation fast path, so it never reaches the memory core's access
 * dispatch; a write still does, because that is where the format mask is
 * applied. Watch the core's own region trace: a window that merely answered
 * correctly would pass every other test here.
 */
static void test_backdoor_reads_bypass_dispatch(const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *trace = NULL;
    g_auto(GStrv) lines = NULL;
    QTestState *qts;
    bool traced_write = false;
    unsigned int i;
    int fd;

    fd = g_file_open_tmp("sn0-backdoor-dispatch-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200 -trace enable=memory_region_ops_*,file=%s",
                      path);
    qtest_writeq(qts, BACKDOOR_DIR, 0x4321);
    g_assert_cmphex(qtest_readq(qts, BACKDOOR_DIR) & 0xffff, ==, 0x4321);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);

    lines = g_strsplit(trace, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        if (!strstr(lines[i], "name 'sn0.node0.backdoor.dir'")) {
            continue;
        }
        g_assert_false(g_str_has_prefix(lines[i], "memory_region_ops_read"));
        if (g_str_has_prefix(lines[i], "memory_region_ops_write")) {
            traced_write = true;
        }
    }
    g_assert_true(traced_write);
}

/*
 * The back door is the memory address shifted right by two; firmware relies
 * on the quarter-size aliasing to size the bank.
 */
static void check_backdoor_bank_span(QTestState *qts, uint64_t base,
                                     unsigned int bank, uint64_t span)
{
    uint64_t bank_base = base + (uint64_t)bank * BACKDOOR_BANK_STRIDE;
    uint64_t first = bank_base + 0x1000;
    uint64_t distinct = first + span / 2;
    uint64_t alias = first + span;

    qtest_writeq(qts, first, 0x0000000000001111ULL);
    qtest_writeq(qts, distinct, 0x0000000000002222ULL);
    g_assert_cmphex(qtest_readq(qts, first) & 0xffffULL, ==, 0x1111);

    qtest_writeq(qts, alias, 0x0000000000003333ULL);
    g_assert_cmphex(qtest_readq(qts, first) & 0xffffULL, ==, 0x3333);
}

/*
 * A bank whose size exactly fills its architectural slot (512 MiB: its span,
 * size/4, equals BACKDOOR_BANK_STRIDE's 128 MiB) gets exactly one fold from
 * sn0_backdoor_configure() and no smaller repeat within it, so there is no
 * alias boundary inside the slot for check_backdoor_ bank_span to find. This
 * instead proves the two entries stay distinct storage within that one fold.
 */
static void check_backdoor_bank_distinct(QTestState *qts, uint64_t base,
                                         unsigned int bank, uint64_t offset)
{
    uint64_t bank_base = base + (uint64_t)bank * BACKDOOR_BANK_STRIDE;
    uint64_t first = bank_base + 0x1000;

    qtest_writeq(qts, first, 0x0000000000004444ULL);
    qtest_writeq(qts, first + offset, 0x0000000000005555ULL);
    g_assert_cmphex(qtest_readq(qts, first) & 0xffffULL, ==, 0x4444);
}

static void test_backdoor_tracks_bank_size(const void *data)
{
    QTestState *qts;

    qts = qtest_init("-M origin200");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 16 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 16 * MiB);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 128M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 32 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 32 * MiB);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 256M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 64 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 64 * MiB);
    qtest_quit(qts);

    /*
 * 512 MiB is origin200's largest accepted population, and IP29's first-bank
 * cap (256 MiB) forces it into two banks (256 + 256): bank 0's span is
 * unaffected by a populated bank 1 alongside it.
 */
    qts = qtest_init("-M origin200 -m 512M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 64 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 64 * MiB);
    qtest_quit(qts);

    /*
 * No product's accepted list reaches a bank 1 larger than bank 0
 * (origin200's 512 MiB maximum splits into two equal 256 MiB banks, as
 * above): sn-builder-bank-test ships 256 MiB in bank 0 and 512 MiB in bank
 * 1, the largest single bank IP29 supports, whose span (128 MiB) exactly
 * fills its architectural slot with one fold and no smaller repeat,
 * check_backdoor_bank_distinct is what proves that fold is real, distinct
 * storage rather than aliased.
 */
    qts = qtest_init(
        "-M sn-builder-bank-test,activate=on -cpu R10000 -m 768M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 64 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 64 * MiB);
    check_backdoor_bank_distinct(qts, BACKDOOR_DIR, 1, 64 * MiB);
    check_backdoor_bank_distinct(qts, BACKDOOR_ECC, 1, 64 * MiB);
    qtest_quit(qts);
}

/*
 * The default machine populates one 64 MiB bank from a matched DIMM pair.
 * Bank 0 answers in standard directory format; bank 1 and the top of the
 * window remain absent.
 */
static void test_directory_backdoor(const void *data)
{
    address_map_reset();

    check_backdoor_entry(qts, BACKDOOR_DIR, 0);
    check_backdoor_standard_format(qts, BACKDOOR_DIR);
    check_backdoor_absent(qts, BACKDOOR_DIR + BACKDOOR_BANK_STRIDE);
    check_backdoor_absent(qts, BACKDOOR_DIR + BACKDOOR_SIZE - 8);
}

static void test_ecc_backdoor(const void *data)
{
    address_map_reset();

    check_backdoor_entry(qts, BACKDOOR_ECC, 0);
    check_backdoor_standard_format(qts, BACKDOOR_ECC);
    check_backdoor_absent(qts, BACKDOOR_ECC + BACKDOOR_BANK_STRIDE);
    check_backdoor_absent(qts, BACKDOOR_ECC + BACKDOOR_SIZE - 8);
}

/*
 * Bank 1 must round-trip and bank 0 must survive it: a model aliasing the
 * banks together shows neither.
 */
static void check_backdoor_multi_bank(QTestState *qts, uint64_t base)
{
    const uint64_t isolation_pattern = 0x00000000000012f0ULL;

    check_backdoor_entry(qts, base, 0);
    check_backdoor_standard_format(qts, base);
    check_backdoor_entry(qts, base, 1);
    check_backdoor_standard_format(qts, base + BACKDOOR_BANK_STRIDE);

    /*
 * must survive bank 1 being written, which a model aliasing every
 */
    qtest_writeq(qts, base, isolation_pattern);
    check_backdoor_entry(qts, base, 1);
    g_assert_cmphex(qtest_readq(qts, base) & 0xffffULL, ==,
                    isolation_pattern & 0xffffULL);

    check_backdoor_absent(qts, base + 2 * BACKDOOR_BANK_STRIDE);
}

static void test_backdoor_tracks_populated_banks(const void *data)
{
    /*
 * 512 MiB is the accepted population that reaches two banks; see
 * test_backdoor_tracks_bank_size.
 */
    QTestState *qts = qtest_init("-M origin200 -m 512M");

    check_backdoor_multi_bank(qts, BACKDOOR_DIR);
    check_backdoor_multi_bank(qts, BACKDOOR_ECC);

    qtest_quit(qts);
}

/*
 * A populated write traces the storage index it reached, which is also the
 * offset the handler was given: the window's fold is memory-region aliases
 * now, so the window offset the guest used is gone before the handler runs.
 * The write below goes one 16 MiB span up from the entry it lands on, and
 * the read of that entry proves the fold, while tracing nothing because it
 * comes out of the storage directly. The absent floor still traces both
 * ways, its read value left unpinned.
 */
static void test_backdoor_accesses_trace(const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-backdoor-trace-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = qtest_initf("-M origin200 -trace enable=sn0_backdoor_*,file=%s",
                      path);
    qtest_writeq(qts, BACKDOOR_DIR + 16 * MiB + 0x1008, 0x1234);
    g_assert_cmphex(qtest_readq(qts, BACKDOOR_DIR + 0x1008), ==, 0x1234);
    qtest_writeq(qts, BACKDOOR_ECC + BACKDOOR_BANK_STRIDE, BACKDOOR_PATTERN);
    qtest_readq(qts, BACKDOOR_ECC + BACKDOOR_BANK_STRIDE);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_nonnull(strstr(trace,
        "sn0_backdoor_populated cpu -1 node 0 window dir "
        "pc 0x0000000000000000 offset 0x00001008 size 8 write 1 "
        "value 0x0000000000001234 index 0x00001008\n"));
    g_assert_null(strstr(trace, "sn0_backdoor_populated cpu -1 node 0 "
                               "window dir pc 0x0000000000000000 "
                               "offset 0x00001008 size 8 write 0"));
    g_assert_nonnull(strstr(trace,
        "sn0_backdoor_absent cpu -1 node 0 window ecc "
        "pc 0x0000000000000000 offset 0x08000000 size 8 write 1 "
        "value 0x5555555555555555\n"));
    g_assert_nonnull(strstr(trace,
        "sn0_backdoor_absent cpu -1 node 0 window ecc "
        "pc 0x0000000000000000 offset 0x08000000 size 8 write 0 value "));
}

/*
 * The rest of this file is the disposition of tests/qtest/sn0-model-test.c,
 * deleted in the same commit as this addition: the facts below are not
 * duplicated by any other qtest, so they move here rather than disappear
 * with the file. Every case that read the synthesized KLCONFIG graph or
 * booted a -kernel image went with the file instead. That machinery no
 * longer exists to have a fact left to port. See that commit's body for the
 * full disposition, including what is already covered elsewhere.
 */

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

typedef struct MachineRejectionCase {
    const char *subtest;
    const char *const *args;
    const char *message;
    bool needs_config_fixture;
    const char *skip_if_missing_device;
} MachineRejectionCase;

static const char *const reject_io_bridge_property_args[] = {
    "-M", "origin200,io-bridge=off", "-display", "none", NULL
};

static const char *const reject_widget0_revision_property_args[] = {
    "-M", "origin200,widget0-revision=3", "-display", "none", NULL
};

static const char *const reject_non_r10000_cpu_args[] = {
    "-M", "origin200", "-cpu", "R4000", "-display", "none", NULL
};

static const char *const reject_numa_on_dual_topology_args[] = {
    "-M", "origin200,topology=origin200-dual", "-m", "128M",
    "-object", "memory-backend-ram,id=ram0,size=64M",
    "-object", "memory-backend-ram,id=ram1,size=64M",
    "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0",
    "-numa", "node,nodeid=1,cpus=2-3,memdev=ram1",
    "-smp", "4", "-display", "none", NULL
};

static const char *const reject_invalid_node_memory_args[] = {
    "-M", "origin200,topology=origin200-dual", "-m", "36M", "-smp", "4",
    "-display", "none", NULL
};

static const char *const
reject_dual_population_on_one_node_topology_args[] = {
    "-M", "origin200", "-smp", "4", "-display", "none", NULL
};

static const char *const reject_three_cores_one_socket_args[] = {
    "-M", "origin200", "-smp", "3,sockets=1,cores=3,threads=1",
    "-display", "none", NULL
};

static const char *const reject_three_single_core_sockets_args[] = {
    "-M", "origin200", "-smp", "3,sockets=3,cores=1,threads=1",
    "-display", "none", NULL
};

static const char *const reject_uneven_no_numa_split_args[] = {
    "-M", "origin200,topology=origin200-dual", "-m", "129M", "-smp", "4",
    "-display", "none", NULL
};

static const char *const reject_device_beyond_bridge_slots_args[] = {
    "-M", "origin200", "-device", "sgi-bridge-dma-test,addr=8",
    "-display", "none", NULL
};

static const char *const reject_one_node_numa_args[] = {
    "-M", "origin200", "-m", "64M",
    "-object", "memory-backend-ram,id=ram0,size=64M",
    "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0",
    "-smp", "2", "-display", "none", NULL
};

static const MachineRejectionCase machine_rejection_cases[] = {
    {
        .subtest = "reject-io-bridge-property",
        .args = reject_io_bridge_property_args,
        .message = "Property 'origin200-machine.io-bridge' not found",
    },
    {
        .subtest = "reject-widget0-revision-property",
        .args = reject_widget0_revision_property_args,
        .message =
            "Property 'origin200-machine.widget0-revision' not found",
    },
    {
        .subtest = "reject-non-r10000-cpu",
        .args = reject_non_r10000_cpu_args,
        .message = "Invalid CPU model: R4000",
    },
    {
        .subtest = "reject-numa-on-dual-topology",
        .args = reject_numa_on_dual_topology_args,
        .message = "origin200: -numa is refused; topology= selects the "
                   "wiring, -smp and -m select the population within it",
    },
    /*
 * 18 MiB per node is not one of origin200-dual's accepted populations
 * either; see test_invalid_memory_size for why the message names the
 * accepted-population check rather than the IP29 bank-fitting one.
 */
    {
        .subtest = "reject-invalid-node-memory",
        .args = reject_invalid_node_memory_args,
        .message = "topology 'origin200-dual' does not accept 18 MiB "
                   "per node",
        .needs_config_fixture = true,
    },
    /*
 * topology= selects the wiring explicitly now; naming the dual topology's
 * four-processor population alone does not switch the default one-node
 * topology to the two-node one that accepts it. The two-chassis topology
 * must be named.
 */
    {
        .subtest = "reject-dual-population-on-one-node-topology",
        .args = reject_dual_population_on_one_node_topology_args,
        .message = "origin200: topology 'origin200' accepts 1 or 2 "
                   "CPUs, not 4",
    },
    {
        .subtest = "reject-three-cores-one-socket",
        .args = reject_three_cores_one_socket_args,
        .message = "origin200: every processor is its own single-core "
                   "socket; pass -smp N alone, not cores= or threads=",
    },
    /*
 * Three single-core sockets is the shape the new rule asks for, but three is
 * still not one of origin200's accepted totals: naming the shape explicitly
 * and correctly does not excuse an unsupported population.
 */
    {
        .subtest = "reject-three-single-core-sockets",
        .args = reject_three_single_core_sockets_args,
        .message = "origin200: topology 'origin200' accepts 1 or 2 "
                   "CPUs, not 3",
    },
    {
        .subtest = "reject-uneven-no-numa-split",
        .args = reject_uneven_no_numa_split_args,
        .message = "origin200: topology 'origin200-dual' splits -m "
                   "evenly across 2 nodes",
    },
    /*
 * The Bridge wires eight slots to eight interrupt pins; nothing past slot 7
 * exists on the chip. A device placed there must be refused when it is added
 * If accepted, it would sit unreachable until its first interrupt asserted a
 * pin the bus never wired and aborted the emulator.
 */
    {
        .subtest = "reject-device-beyond-bridge-slots",
        .args = reject_device_beyond_bridge_slots_args,
        .message = "not available for sgi-bridge-dma-test, reserved",
        .needs_config_fixture = true,
        .skip_if_missing_device = "sgi-bridge-dma-test",
    },
    /*
 * -numa is refused outright now, even the one-node form that used to pick a
 * memory backend explicitly: topology= names the wiring and -smp/-m name the
 * population, so a NUMA description is never needed.
 */
    {
        .subtest = "reject-one-node-numa",
        .args = reject_one_node_numa_args,
        .message = "origin200: -numa is refused; topology= selects the "
                   "wiring, -smp and -m select the population within it",
    },
};

static void test_machine_configuration_is_rejected(const void *data)
{
    const MachineRejectionCase *tc = data;
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *fixture_path = NULL;
    size_t i;

    if (tc->skip_if_missing_device &&
        !qtest_has_device(tc->skip_if_missing_device)) {
        g_autofree char *reason = g_strdup_printf(
            "%s is not in this build", tc->skip_if_missing_device);
        g_test_skip(reason);
        return;
    }

    g_ptr_array_add(args, g_strdup(qtest_qemu_binary(NULL)));
    for (i = 0; tc->args[i]; i++) {
        g_ptr_array_add(args, g_strdup(tc->args[i]));
    }
    if (tc->needs_config_fixture) {
        fixture_path = write_config_block_fixture();
        g_ptr_array_add(args, g_strdup("-bios"));
        g_ptr_array_add(args, g_strdup(fixture_path));
    }
    g_ptr_array_add(args, NULL);

    assert_qemu_rejects((const char *const *)args->pdata, tc->message);
    if (fixture_path) {
        g_assert_cmpint(unlink(fixture_path), ==, 0);
    }
}

/*
 * A compat-props entry pins a QOM default across a migration boundary
 * forever; origin200 has never shipped, so it must never accrue one for a
 * property whose value the model itself now decides.
 */
static void test_origin200_has_no_ioc3_compat_global(const void *data)
{
    QTestState *qts = qtest_init("-machine none");
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'query-machines',"
             " 'arguments': { 'compat-props': true } }");
    QList *machines = qdict_get_qlist(response, "return");
    const QListEntry *machine_entry;
    bool found_machine = false;
    bool found_property = false;

    for (machine_entry = qlist_first(machines); machine_entry;
         machine_entry = qlist_next(machine_entry)) {
        QDict *machine = qobject_to(
            QDict, qlist_entry_obj(machine_entry));
        QList *properties;
        const QListEntry *property_entry;

        if (!g_str_equal(qdict_get_str(machine, "name"), "origin200")) {
            continue;
        }
        found_machine = true;
        properties = qdict_get_qlist(machine, "compat-props");
        for (property_entry = qlist_first(properties); property_entry;
             property_entry = qlist_next(property_entry)) {
            QDict *property = qobject_to(
                QDict, qlist_entry_obj(property_entry));

            if (g_str_equal(qdict_get_str(property, "qom-type"),
                            "sgi-ioc3") &&
                g_str_equal(qdict_get_str(property, "property"),
                            "subsystem-id")) {
                g_assert_false(found_property);
                found_property = true;
            }
        }
    }
    g_assert_true(found_machine);
    g_assert_false(found_property);
    qobject_unref(response);
    qtest_quit(qts);
}

/*
 * QEMU's standard CPU-topology interface must describe the IP29 sockets, not
 * merely accept a scalar -smp value: every R10000 is its own single-core
 * socket, so a two-processor Origin 200 chassis is two sockets on the same
 * node, never one socket of two cores.
 */
static void test_one_node_cpu_topology(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    QTestState *qts = qtest_initf(
        "-M origin200 -smp 2 -bios %s", path);
    QDict *response;
    QList *cpus;
    QObject *entry;
    unsigned int seen = 0;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert_true(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");

    while ((entry = qlist_pop(cpus))) {
        QDict *cpu = qobject_to(QDict, entry);
        QDict *props = qdict_get_qdict(cpu, "props");
        int64_t index = qdict_get_int(cpu, "cpu-index");

        g_assert_cmpint(index, >=, 0);
        g_assert_cmpint(index, <, 2);
        g_assert_cmpint(qdict_get_int(props, "socket-id"), ==, index);
        g_assert_cmpint(qdict_get_int(props, "core-id"), ==, 0);
        g_assert_cmpint(qdict_get_int(props, "thread-id"), ==, 0);
        g_assert_cmpint(qdict_get_int(props, "node-id"), ==, 0);
        seen |= 1U << index;
        qobject_unref(entry);
    }
    g_assert_cmphex(seen, ==, 0x3);

    qobject_unref(response);
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_dual_chassis_cpu_topology(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    QTestState *qts = qtest_initf(
        "%s -bios %s", SN0_DUAL_CHASSIS_FIRMWARE_ARGS, path);
    QDict *response;
    QList *cpus;
    QObject *entry;
    unsigned int seen = 0;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert_true(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    while ((entry = qlist_pop(cpus))) {
        QDict *cpu = qobject_to(QDict, entry);
        QDict *props = qdict_get_qdict(cpu, "props");
        int64_t index = qdict_get_int(cpu, "cpu-index");

        g_assert_cmpint(index, >=, 0);
        g_assert_cmpint(index, <, 4);
        g_assert_cmpint(qdict_get_int(props, "socket-id"), ==, index);
        g_assert_cmpint(qdict_get_int(props, "core-id"), ==, 0);
        g_assert_cmpint(qdict_get_int(props, "node-id"), ==, index / 2);
        seen |= 1U << index;
        qobject_unref(entry);
    }
    g_assert_cmphex(seen, ==, 0xf);
    qobject_unref(response);

    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void assert_pci_root_bus_name(QTestState *qts, const char *bridge,
                                     const char *expected)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     bridge);
    QList *children;
    const QListEntry *entry;
    const char *name = NULL;

    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *type = qdict_get_try_str(child, "type");

        if (type && g_str_equal(type, "child<PCI>")) {
            g_assert_null(name);
            name = qdict_get_str(child, "name");
        }
    }

    g_assert_nonnull(name);
    g_assert_cmpstr(name, ==, expected);
    qobject_unref(response);
}

/*
 * Each physical chassis's node owns a Bridge directly, as a fitted part of
 * its own System Board, not a removable board (sn0.c,
 * sn0_node_build_baseio()), and therefore a separate PCI root bus. Node 0
 * keeps its original bus name, part of existing device migration paths; node
 * 1 gets a stable identity of its own.
 */
static void test_bridge_root_bus_names(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    QTestState *qts = qtest_initf(
        "%s -global sgi-bridge.pci-bus-name=hostile -bios %s",
        SN0_DUAL_CHASSIS_FIRMWARE_ARGS, path);

    assert_pci_root_bus_name(qts, "/machine/node0/bridge", "pci");
    assert_pci_root_bus_name(qts, "/machine/node1/bridge", "pci.1");

    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A drive named onto the second adapter. Both adapters on a base I/O board
 * are ordinary SCSI buses, numbered in the order the machine creates them,
 * so -drive if=scsi,bus=1 names the second one. A machine that offers its
 * command-line drives to the first adapter alone leaves that drive attached
 * to nothing.
 */
static void test_a_drive_on_the_second_adapter(const void *data)
{
    g_autofree char *path = write_config_block_fixture();
    QTestState *qts = qtest_initf(
        "-M origin200 -bios %s "
        "-drive if=scsi,bus=1,file=null-co://,format=raw", path);
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-block' }");
    QList *blocks;
    const QListEntry *entry;
    bool found = false;

    g_assert_true(qdict_haskey(response, "return"));
    blocks = qdict_get_qlist(response, "return");
    for (entry = qlist_first(blocks); entry; entry = qlist_next(entry)) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));
        QDict *inserted = qdict_haskey(block, "inserted") ?
                          qdict_get_qdict(block, "inserted") : NULL;
        const char *file = inserted ? qdict_get_try_str(inserted, "file")
                                    : NULL;

        if (file && g_str_equal(file, "null-co://")) {
            g_assert_false(found);
            found = true;
            g_assert_nonnull(qdict_get_try_str(block, "qdev"));
        }
    }

    g_assert_true(found);
    qobject_unref(response);
    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A default drive belongs on the first SCSI adapter. It used to be accepted
 * and attached to nothing: the drive existed, no device claimed it, and a
 * guest booting from "the first disk" found an empty bus.
 */
static void test_default_drive_reaches_the_first_scsi_adapter(
    const void *data)
{
    g_autofree char *bios = write_config_block_fixture();
    g_autofree char *img = NULL;
    GError *err = NULL;
    int fd = g_file_open_tmp("sn0-hda-XXXXXX", &img, &err);
    QTestState *qts;
    QDict *response;
    QList *blocks;
    const QListEntry *entry;
    bool attached = false;

    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(fd, 1 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    qts = qtest_initf("-M origin200 -bios %s "
                      "-drive file=%s,format=raw,index=0", bios, img);

    response = qtest_qmp(qts, "{ 'execute': 'query-block' }");
    g_assert_true(qdict_haskey(response, "return"));
    blocks = qdict_get_qlist(response, "return");
    for (entry = qlist_first(blocks); entry; entry = qlist_next(entry)) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));
        const char *qdev = qdict_get_try_str(block, "qdev");

        if (qdev && qdev[0] != '\0') {
            attached = true;
        }
    }
    g_assert_true(attached);
    qobject_unref(response);

    qtest_quit(qts);
    g_unlink(img);
    g_assert_cmpint(unlink(bios), ==, 0);
}

int main(int argc, char **argv)
{
    int ret;
    size_t i;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init("-M origin200");

    qtest_add_data_func("sn0/address-map/flash-three-views", NULL,
                        test_flash_three_views);
    qtest_add_data_func("sn0/address-map/flash-configure-patches-model-fields",
                        NULL, test_flash_configure_patches_model_fields);
    qtest_add_data_func("sn0/address-map/dual-node-boot-windows", NULL,
                        test_dual_node_boot_windows);
    qtest_add_data_func("sn0/address-map/boot-windows-mirror-flash", NULL,
                        test_boot_windows_mirror_the_flash);
    qtest_add_data_func("sn0/address-map/flash-hub-local-is-fast", NULL,
                        test_flash_hub_local_is_fast);
    qtest_add_data_func("sn0/address-map/execution-windows-not-io", NULL,
                        test_execution_windows_are_not_io);
    qtest_add_data_func("sn0/address-map/origin2000-execution-windows-not-io",
                        NULL, test_origin2000_execution_windows_are_not_io);
    qtest_add_data_func("sn0/address-map/flash-wide-read-command-mode", NULL,
                        test_flash_wide_read_in_command_mode);
    qtest_add_data_func("sn0/address-map/flash-rejects-bare-writes", NULL,
                        test_flash_rejects_bare_writes);
    qtest_add_data_func("sn0/address-map/flash-autoselect", NULL,
                        test_flash_autoselect);
    qtest_add_data_func("sn0/address-map/flash-program-and-erase", NULL,
                        test_flash_program_and_erase);
    qtest_add_data_func("sn0/address-map/memory-128m", NULL,
                        test_128m_memory);
    qtest_add_data_func("sn0/address-map/memory-256m", NULL,
                        test_256m_memory);
    qtest_add_data_func("sn0/address-map/memory-512m", NULL,
                        test_512m_memory);
    qtest_add_data_func("sn0/address-map/origin2000-memory-256m", NULL,
                        test_origin2000_256m_memory);
    qtest_add_data_func("sn0/address-map/hub-local-memory-alias", NULL,
                        test_hub_local_memory_alias);
    qtest_add_data_func("sn0/address-map/invalid-memory-size", NULL,
                        test_invalid_memory_size);
    qtest_add_data_func("sn0/address-map/directory-backdoor", NULL,
                        test_directory_backdoor);
    qtest_add_data_func("sn0/address-map/ecc-backdoor", NULL,
                        test_ecc_backdoor);
    qtest_add_data_func("sn0/address-map/backdoor-tracks-populated-banks",
                        NULL, test_backdoor_tracks_populated_banks);
    qtest_add_data_func("sn0/address-map/backdoor-tracks-bank-size", NULL,
                        test_backdoor_tracks_bank_size);
    qtest_add_data_func("sn0/address-map/backdoor-entry-widths", NULL,
                        test_backdoor_entry_widths);
    qtest_add_data_func("sn0/address-map/backdoor-accesses-trace", NULL,
                        test_backdoor_accesses_trace);
    qtest_add_data_func("sn0/address-map/backdoor-reads-bypass-dispatch",
                        NULL, test_backdoor_reads_bypass_dispatch);
    for (i = 0; i < G_N_ELEMENTS(machine_rejection_cases); i++) {
        const MachineRejectionCase *tc = &machine_rejection_cases[i];

        /*
 * Not freed: qtest_add_data_func() keeps the path for the life of the suite,
 * which is the life of this process.
 */
        qtest_add_data_func(
            g_strdup_printf("sn0/address-map/%s", tc->subtest),
            tc, test_machine_configuration_is_rejected);
    }
    qtest_add_data_func("sn0/address-map/no-origin200-ioc3-compat-global",
                        NULL, test_origin200_has_no_ioc3_compat_global);
    qtest_add_data_func("sn0/address-map/one-node-cpu-topology", NULL,
                        test_one_node_cpu_topology);
    qtest_add_data_func("sn0/address-map/dual-chassis-cpu-topology", NULL,
                        test_dual_chassis_cpu_topology);
    qtest_add_data_func("sn0/address-map/bridge-root-bus-names", NULL,
                        test_bridge_root_bus_names);
    qtest_add_data_func("sn0/address-map/drive-on-the-second-adapter", NULL,
                        test_a_drive_on_the_second_adapter);
    qtest_add_data_func("sn0/address-map/default-drive-reaches-first-adapter",
                        NULL,
                        test_default_drive_reaches_the_first_scsi_adapter);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
