/*
 * SGI Origin 200 (IP27) physical address map: memory and boot flash appear
 * where the map says, the flash's three views are one region rather than
 * copies that can drift, and pre-firmware contents read as specified.
 * Addresses are what the memory system sees; plain physical addresses
 * reach memory, and the Hub's special space starts one window up.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/* Node zero's window, and the space the Hub answers uncached references in. */
#define NODE0           0x0000000000ULL
#define SPACE_HUB       0x10000000000ULL
#define NODE_STRIDE     0x100000000ULL
#define LBOOT_OFFSET    0x10000000ULL
#define RBOOT_OFFSET    0x30000000ULL

/* The flash at reset, through local LBOOT, and through node-explicit RBOOT. */
#define FLASH_RESET     (NODE0 + 0x1fc00000)
#define FLASH_HUB       (SPACE_HUB + NODE0 + 0x10000000)
#define FLASH_REMOTE    (SPACE_HUB + NODE0 + 0x30000000)
/* The flash base in Hub space -- where the real PROM fetches relocated code. */
#define FLASH_HUB_LOCAL (SPACE_HUB + NODE0 + 0x1fc00000)

/* AMD 29F080 command addresses and the blank first PROM-log sector. */
#define FLASH_UNLOCK1       0x5555
#define FLASH_UNLOCK2       0x2aaa
#define FLASH_LOG_SECTOR    0xe0000
#define FLASH_SECTOR_SIZE   0x10000
#define PROMLOG_MAGIC_OFF   0x10
#define PROMLOG_VERSION_OFF 0x14
#define PROMLOG_SEQUENCE_OFF 0x18
#define PROMLOG_MAGIC       0x504c4f47

static void flash_command(QTestState *qts, uint32_t offset, uint8_t value)
{
    /* Hub presents the flash's byte-wide command port on 64-bit cycles. */
    qtest_writeq(qts, FLASH_HUB + 8 * offset, value);
}

/*
 * The directory/ECC back doors: nothing about coherence is modelled, so
 * the property is the diagnostic's own round trip.
 */
#define BACKDOOR_ECC    (SPACE_HUB + NODE0 + 0x80000000ULL)
#define BACKDOOR_DIR    (SPACE_HUB + NODE0 + 0xc0000000ULL)
#define BACKDOOR_SIZE   0x40000000ULL

/* The configuration block, at flash offset 0x60. */
#define CONFIG_OFFSET   0x60
#define CONFIG_MAGIC_OFF 0x68
#define CONFIG_MAGIC    0x69703237636f6e66ULL    /* "ip27conf" */

#define NODE0_LBOOT (SPACE_HUB + NODE0 + LBOOT_OFFSET)
#define NODE1_LBOOT (SPACE_HUB + NODE_STRIDE + LBOOT_OFFSET)
#define NODE0_RBOOT (SPACE_HUB + NODE0 + RBOOT_OFFSET)
#define NODE1_RBOOT (SPACE_HUB + NODE_STRIDE + RBOOT_OFFSET)

#define SN0_DUAL_CHASSIS_FIRMWARE_ARGS \
    "-M origin200 -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

#define FIELD_MAGIC     0x08
#define FIELD_CPU_FREQ  0x10
#define FIELD_HUB_FREQ  0x18
#define FIELD_RTC_FREQ  0x20
#define FIELD_FPROM_CYC 0x2c
#define FIELD_MACHTYPE  0x30
#define FIELD_CHECKSUM  0x34
#define FIELD_FPROM_WR  0x3c
#define CONFIG_SIZE     0x50

#define ENTRY_POINTS    10
#define ENTRY_STRIDE    8

/* The default synthesized graph's eight plain-megabyte bank counts. */
#define GRAPH_BANK_SIZES    0x453c

/* Every instruction word at an entry point has to be a branch of some kind. */
static bool is_branch(uint32_t insn)
{
    uint32_t op = insn >> 26;
    uint32_t func = insn & 0x3f;

    switch (op) {
    case 0x00:                  /* SPECIAL: jr and jalr are branches */
        return func == 0x08 || func == 0x09;
    case 0x01:                  /* REGIMM: the branch-on-condition forms */
    case 0x02:                  /* j */
    case 0x03:                  /* jal */
    case 0x04:                  /* beq, and b as its idiom */
    case 0x05:                  /* bne */
    case 0x06:                  /* blez */
    case 0x07:                  /* bgtz */
    case 0x14:                  /* beql */
    case 0x15:                  /* bnel */
    case 0x16:                  /* blezl */
    case 0x17:                  /* bgtzl */
        return true;
    default:
        return false;
    }
}

static void test_config_block(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    g_assert_cmphex(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);

    /* The clocks a guest reads its timebase from. */
    g_assert_cmpuint(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_CPU_FREQ), ==, 180000000);
    g_assert_cmpuint(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_HUB_FREQ), ==, 90000000);
    g_assert_cmpuint(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_RTC_FREQ), ==, 1250000);

    /*
     * One means Origin 200; zero would make a guest introduce itself as an
     * Origin 2000. The flash timings are the matching SN00 values rather than
     * the generic release image's Origin 2000 defaults.
     */
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_FPROM_CYC), ==, 15);
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_MACHTYPE), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_FPROM_WR), ==, 4);

    qtest_quit(qts);
}

static uint8_t config_byte_sum(const uint8_t *config)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < CONFIG_SIZE; i++) {
        sum += config[i];
    }
    return sum;
}

/*
 * A generic release image must describe the concrete machine after QEMU
 * installs it, keeping the configuration block's byte sum -- the visible
 * half of SGI's own reflash.
 */
static void test_supplied_prom_gets_origin200_config(const void *data)
{
    uint8_t image[CONFIG_OFFSET + CONFIG_SIZE] = { 0 };
    uint8_t configured[CONFIG_SIZE];
    uint8_t *config = image + CONFIG_OFFSET;
    g_autofree char *path = NULL;
    g_autofree char *quoted_path = NULL;
    g_autofree char *args = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    uint8_t original_sum;
    int fd;

    stq_be_p(config + FIELD_MAGIC, CONFIG_MAGIC);
    stl_be_p(config + FIELD_FPROM_CYC, 8);
    stl_be_p(config + FIELD_MACHTYPE, 0);
    stl_be_p(config + FIELD_CHECKSUM, 0);
    stl_be_p(config + FIELD_FPROM_WR, 1);
    original_sum = config_byte_sum(config);

    fd = g_file_open_tmp("sn0-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    quoted_path = g_shell_quote(path);
    args = g_strdup_printf("-M origin200,synthesized-firmware=off -bios %s",
                           quoted_path);
    qts = qtest_init(args);

    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_FPROM_CYC), ==, 15);
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_MACHTYPE), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_FPROM_WR), ==, 4);
    qtest_memread(qts, FLASH_RESET + CONFIG_OFFSET, configured,
                  sizeof(configured));
    g_assert_cmphex(config_byte_sum(configured), ==, original_sum);

    qtest_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_flash_three_views(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
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

static void test_dual_node_boot_windows(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_FIRMWARE_ARGS);

    g_assert_cmphex(qtest_readq(qts, NODE0_LBOOT + CONFIG_MAGIC_OFF),
                    ==, CONFIG_MAGIC);
    g_assert_cmphex(qtest_readq(qts, NODE1_LBOOT + CONFIG_MAGIC_OFF),
                    ==, CONFIG_MAGIC);
    g_assert_cmphex(qtest_readq(qts, NODE0_RBOOT + CONFIG_MAGIC_OFF),
                    ==, CONFIG_MAGIC);
    g_assert_cmphex(qtest_readq(qts, NODE1_RBOOT + CONFIG_MAGIC_OFF),
                    ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

/*
 * 256 MB views of a 1 MB chip repeat: firmware fetches past the real
 * size, and offset zero alone cannot show the repeat.
 */
static void test_boot_windows_mirror_the_flash(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t direct, mirrored;

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

    qtest_quit(qts);
}

/*
 * The PROM runs its own code through this base, and an I/O region
 * executes one instruction per block: the base must be a direct (romd)
 * view, read off the machine's own flat view rather than assumed.
 */
static void test_flash_hub_local_is_fast(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    const char *hit = strstr(mtree, "000001001fc00000-");
    const char *line_end;
    g_autofree char *line = NULL;

    g_assert_nonnull(hit);
    line_end = strchr(hit, '\n');
    line = g_strndup(hit, line_end ? (size_t)(line_end - hit) : strlen(hit));
    g_assert_nonnull(strstr(line, "romd"));

    /* The fast view still has to read the chip's real bytes at this base. */
    g_assert_cmphex(qtest_readq(qts, FLASH_HUB_LOCAL + CONFIG_OFFSET +
                               FIELD_MAGIC), ==, CONFIG_MAGIC);

    qtest_quit(qts);
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
    qts = qtest_initf("-M origin200,synthesized-firmware=off -m 64M "
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

static void test_entry_points(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    int i;

    /*
     * A guest calls nine of these by address and expects to come back, so
     * whatever is there has to redirect rather than run on into the next one.
     */
    for (i = 0; i < ENTRY_POINTS; i++) {
        uint32_t insn = qtest_readl(qts, FLASH_RESET + i * ENTRY_STRIDE);

        g_assert_true(is_branch(insn));
    }

    qtest_quit(qts);
}

static void test_flash_rejects_bare_writes(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
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
    QTestState *qts = qtest_init("-M origin200");
    uint32_t before = qtest_readl(qts, FLASH_HUB);

    flash_command(qts, 0, 0xf0);
    flash_command(qts, FLASH_UNLOCK1, 0xaa);
    flash_command(qts, FLASH_UNLOCK2, 0x55);
    flash_command(qts, FLASH_UNLOCK1, 0x90);

    /*
     * Manufacturer 0x01 and device 0xd5 occupy logical bytes zero and one.
     * Only those two identity bytes are asserted: what an Am29F080 answers
     * for higher autoselect addresses (sector protection, then unspecified)
     * is not something any source we hold pins, so the bytes beyond the ID
     * follow the flash device model rather than an invented filler.
     */
    g_assert_cmphex(qtest_readb(qts, FLASH_HUB), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, FLASH_HUB + 1), ==, 0xd5);
    g_assert_cmphex(qtest_readb(qts, FLASH_REMOTE), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, FLASH_REMOTE + 1), ==, 0xd5);

    /*
     * Every view of the chip must answer command-mode reads alike, at any
     * width: the reset view is the same byte-wide chip behind the same
     * controller, so a word read must gather the same bytes the Hub
     * windows gather rather than widening one chip lookup across the bus.
     */
    g_assert_cmphex(qtest_readl(qts, FLASH_RESET), ==,
                    qtest_readl(qts, FLASH_HUB));
    g_assert_cmphex(qtest_readl(qts, FLASH_REMOTE), ==,
                    qtest_readl(qts, FLASH_HUB));
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + 1), ==, 0xd5);

    flash_command(qts, 0, 0xf0);
    g_assert_cmphex(qtest_readl(qts, FLASH_HUB), ==, before);

    qtest_quit(qts);
}

static void test_flash_program_and_erase(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    const uint32_t target = FLASH_LOG_SECTOR + 0x100;

    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target), ==, 0xff);

    /*
     * A real Am29F080 takes program and erase time, and the device model
     * keeps that: step the clock over each operation's completion timer
     * before reading data back, the way the chip's own test does.
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
     * Step over the 50 us additional-sector window and then the erase time
     * by explicit amounts: this machine keeps other timers armed, so
     * stepping to the next deadline would advance those instead.
     */
    qtest_clock_step(qts, 100 * 1000 * 1000);
    qtest_clock_step(qts, 100 * 1000 * 1000);

    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + target), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_RESET + FLASH_LOG_SECTOR), ==,
                    0xff);
    g_assert_cmphex(qtest_readb(qts,
                               FLASH_RESET + FLASH_LOG_SECTOR +
                               FLASH_SECTOR_SIZE - 1), ==, 0xff);

    qtest_quit(qts);
}

static void test_flash_has_empty_factory_log(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,synthesized-firmware=off");

    g_assert_cmphex(qtest_readl(qts, FLASH_RESET + FLASH_LOG_SECTOR +
                               PROMLOG_MAGIC_OFF), ==, PROMLOG_MAGIC);
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + FLASH_LOG_SECTOR +
                                PROMLOG_VERSION_OFF), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + FLASH_LOG_SECTOR +
                                PROMLOG_SEQUENCE_OFF), ==, 1);

    /* The second log sector is the erased alternate until compaction. */
    g_assert_cmphex(qtest_readl(qts, FLASH_RESET + FLASH_LOG_SECTOR +
                               FLASH_SECTOR_SIZE + PROMLOG_MAGIC_OFF), ==,
                    0xffffffff);

    qtest_quit(qts);
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

/*
 * A 256 MiB bank is four 64 MiB runs, not one contiguous run. This is the
 * layout both supported guests derive from the graph's per-bank size.
 */
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

    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES), ==, 256);
    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES + 2), ==, 0);

    qtest_quit(qts);
}

/*
 * The explicit 512 MiB configuration is two 256 MiB banks. That keeps the
 * flash and synthesized ARCS regions in bank zero's final gap instead of
 * advertising their addresses as RAM.
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

    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES), ==, 256);
    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES + 2), ==, 256);
    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES + 4), ==, 0);
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

static void test_invalid_memory_size(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "96M",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output,
                           "cannot be populated from IP29 memory banks"));
}

/*
 * One bank's stride within a back-door window: an even eight-way split of
 * the 1 GB window, restated independently from the compiled product model.
 */
#define BACKDOOR_BANK_STRIDE   0x8000000ULL  /* 128 MiB */

/*
 * Firmware's presence probe: write the 0x55 pattern, write zero eight
 * bytes on (only to disturb the shared data lines), read back -- premium
 * if the low 48 bits survived, standard if only the low 16, else absent.
 */
#define BACKDOOR_PATTERN       0x5555555555555555ULL
#define BACKDOOR_PATTERN_LOW16 0x5555ULL
#define BACKDOOR_PATTERN_LOW48 0x0000555555555555ULL

/*
 * Only the format's sixteen bits survive; checked at base, middle and
 * top, folded with the bank index so banks cannot alias.
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
 * The back door is the memory address shifted right by two; firmware
 * relies on the quarter-size aliasing to size the bank.
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

    qts = qtest_init("-M origin200 -m 768M");
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
    QTestState *qts = qtest_init("-M origin200");

    check_backdoor_entry(qts, BACKDOOR_DIR, 0);
    check_backdoor_standard_format(qts, BACKDOOR_DIR);
    check_backdoor_absent(qts, BACKDOOR_DIR + BACKDOOR_BANK_STRIDE);
    check_backdoor_absent(qts, BACKDOOR_DIR + BACKDOOR_SIZE - 8);

    qtest_quit(qts);
}

static void test_ecc_backdoor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    check_backdoor_entry(qts, BACKDOOR_ECC, 0);
    check_backdoor_standard_format(qts, BACKDOOR_ECC);
    check_backdoor_absent(qts, BACKDOOR_ECC + BACKDOOR_BANK_STRIDE);
    check_backdoor_absent(qts, BACKDOOR_ECC + BACKDOOR_SIZE - 8);

    qtest_quit(qts);
}

/*
 * Bank 1 must round-trip and bank 0 must survive it: a model aliasing
 * the banks together shows neither.
 */
static void check_backdoor_multi_bank(QTestState *qts, uint64_t base)
{
    const uint64_t isolation_pattern = 0x00000000000012f0ULL;

    check_backdoor_entry(qts, base, 0);
    check_backdoor_standard_format(qts, base);
    check_backdoor_entry(qts, base, 1);
    check_backdoor_standard_format(qts, base + BACKDOOR_BANK_STRIDE);

    /* Bank 0's own entry, written last so nothing after this touches it,
     * must survive bank 1 being written -- which a model aliasing every
     * populated bank onto the same bytes would not show. */
    qtest_writeq(qts, base, isolation_pattern);
    check_backdoor_entry(qts, base, 1);
    g_assert_cmphex(qtest_readq(qts, base) & 0xffffULL, ==,
                    isolation_pattern & 0xffffULL);

    check_backdoor_absent(qts, base + 2 * BACKDOOR_BANK_STRIDE);
}

static void test_backdoor_tracks_populated_banks(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 768M");

    check_backdoor_multi_bank(qts, BACKDOOR_DIR);
    check_backdoor_multi_bank(qts, BACKDOOR_ECC);

    qtest_quit(qts);
}

/*
 * The promdata window must hold the same bytes for every processor; a
 * NASID-derived per-processor filter disagrees on every node but zero and
 * a loaded kernel reads back as nops. A sentinel for whoever restores
 * such a filter; reads go through each processor's own address space,
 * which the always-correct global space cannot substitute for.
 */

#define NUMA_NODE1_BASE     0x100000000ULL
#define PROMDATA_PROBE      0x107fd0        /* inside the 32 KiB window */
#define OUTSIDE_PROBE       0x110000        /* clear of it, as a control */
#define KERNEL_WORD         0xc0de0001u
#define OUTSIDE_WORD        0xc0de0002u
#define NODE1_WORD          0xbad1bad1u

/* XKPHYS, coherent exclusive: how a kernel names physical memory here. */
#define XKPHYS_CACHED       0xa800000000000000ULL

#define ELF_EHDR_SIZE       64
#define ELF_PHDR_SIZE       56
#define ELF_PHNUM           2
#define ELF_PAYLOAD_OFF     (ELF_EHDR_SIZE + ELF_PHNUM * ELF_PHDR_SIZE)
#define ELF_IMAGE_SIZE      (ELF_PAYLOAD_OFF + 8)

static void elf_put_phdr(uint8_t *ph, uint64_t offset, uint64_t paddr,
                         uint64_t size)
{
    stl_be_p(ph + 0, 1);            /* PT_LOAD */
    stl_be_p(ph + 4, 5);            /* read, execute */
    stq_be_p(ph + 8, offset);
    stq_be_p(ph + 16, paddr);       /* p_vaddr */
    stq_be_p(ph + 24, paddr);       /* p_paddr, which the loader translates */
    stq_be_p(ph + 32, size);        /* p_filesz */
    stq_be_p(ph + 40, size);        /* p_memsz */
    stq_be_p(ph + 48, 4);
}

/*
 * The smallest thing the machine's kernel loader accepts: a 64-bit
 * big-endian MIPS executable with two loadable words, one inside the
 * promdata window and one clear of it. Nothing executes it -- no processor
 * runs at all under the qtest accelerator -- so its entry only has to be a
 * legal address.
 */
static size_t build_kernel_elf(uint8_t *image)
{
    memset(image, 0, ELF_IMAGE_SIZE);

    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 2;                   /* ELFCLASS64 */
    image[5] = 2;                   /* ELFDATA2MSB */
    image[6] = 1;                   /* EV_CURRENT */
    stw_be_p(image + 16, 2);        /* ET_EXEC */
    stw_be_p(image + 18, 8);        /* EM_MIPS */
    stl_be_p(image + 20, 1);        /* e_version */
    stq_be_p(image + 24, XKPHYS_CACHED | PROMDATA_PROBE);   /* e_entry */
    stq_be_p(image + 32, ELF_EHDR_SIZE);                    /* e_phoff */
    stw_be_p(image + 52, ELF_EHDR_SIZE);                    /* e_ehsize */
    stw_be_p(image + 54, ELF_PHDR_SIZE);                    /* e_phentsize */
    stw_be_p(image + 56, ELF_PHNUM);                        /* e_phnum */

    elf_put_phdr(image + ELF_EHDR_SIZE, ELF_PAYLOAD_OFF,
                 XKPHYS_CACHED | PROMDATA_PROBE, 4);
    elf_put_phdr(image + ELF_EHDR_SIZE + ELF_PHDR_SIZE, ELF_PAYLOAD_OFF + 4,
                 XKPHYS_CACHED | OUTSIDE_PROBE, 4);

    stl_be_p(image + ELF_PAYLOAD_OFF, KERNEL_WORD);
    stl_be_p(image + ELF_PAYLOAD_OFF + 4, OUTSIDE_WORD);
    return ELF_IMAGE_SIZE;
}

/*
 * One physical word through a processor's own address space. The
 * processor must be named in the same request: each human-monitor-command
 * gets its own monitor, so an "xp" after a separate "cpu" command reads
 * processor zero and agrees with itself forever -- silently, which is why
 * this does not use qtest_hmp().
 */
static uint32_t cpu_view_word(QTestState *qts, unsigned int cpu, uint64_t phys)
{
    g_autofree char *command = g_strdup_printf("xp/1wx 0x%" PRIx64, phys);
    g_autoptr(QDict) response = NULL;
    const char *dumped;
    const char *value;
    unsigned int word;

    response = qtest_qmp(qts,
                         "{ 'execute': 'human-monitor-command',"
                         "  'arguments': { 'command-line': %s,"
                         "                 'cpu-index': %u } }",
                         command, cpu);
    g_assert(qdict_haskey(response, "return"));
    dumped = qdict_get_str(response, "return");
    value = strchr(dumped, ':');
    g_assert_nonnull(value);
    g_assert_cmpint(sscanf(value + 1, " 0x%x", &word), ==, 1);
    return word;
}

static void test_promdata_window_agrees_across_nodes(const void *data)
{
    uint8_t image[ELF_IMAGE_SIZE];
    g_autofree char *path = NULL;
    g_autofree char *quoted_path = NULL;
    g_autofree char *args = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    size_t size;
    int fd;

    size = build_kernel_elf(image);
    fd = g_file_open_tmp("sn0-numa-kernel-XXXXXX.elf", &path, &error);
    g_assert_no_error(error);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image, size,
                                      &error));
    g_assert_no_error(error);

    /*
     * Two nodes, and synthesized firmware left on, because the low-memory
     * alias this rests on exists only for a directly loaded kernel under
     * firmware of the machine's own. The planted word gives node one's
     * memory something of its own at the same offset.
     */
    quoted_path = g_shell_quote(path);
    args = g_strdup_printf(
        "-M origin200 -m 256M -smp 4,sockets=2,cores=2,threads=1 "
        "-object memory-backend-ram,id=ram0,size=128M "
        "-object memory-backend-ram,id=ram1,size=128M "
        "-numa node,nodeid=0,memdev=ram0 -numa node,nodeid=1,memdev=ram1 "
        "-kernel %s", quoted_path);
    qts = qtest_init(args);
    g_assert_cmpint(unlink(path), ==, 0);

    /*
     * Give node one's memory a word of its own at the same offset, so a
     * failure names what it read rather than only that it read the wrong
     * thing. This goes through the global address space, which has no
     * per-processor filter in it and reaches node one's memory directly.
     */
    qtest_writel(qts, NUMA_NODE1_BASE + PROMDATA_PROBE, NODE1_WORD);
    g_assert_cmphex(qtest_readl(qts, NUMA_NODE1_BASE + PROMDATA_PROBE), ==,
                    NODE1_WORD);
    /* And the kernel landed, so agreement below is not two empty reads. */
    g_assert_cmphex(qtest_readl(qts, PROMDATA_PROBE), ==, KERNEL_WORD);

    g_assert_cmphex(cpu_view_word(qts, 0, PROMDATA_PROBE), ==, KERNEL_WORD);
    g_assert_cmphex(cpu_view_word(qts, 2, PROMDATA_PROBE), ==, KERNEL_WORD);

    /* Clear of the window, both processors already agree by construction. */
    g_assert_cmphex(cpu_view_word(qts, 0, OUTSIDE_PROBE), ==, OUTSIDE_WORD);
    g_assert_cmphex(cpu_view_word(qts, 2, OUTSIDE_PROBE), ==, OUTSIDE_WORD);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/address-map/config-block", NULL,
                        test_config_block);
    qtest_add_data_func("sn0/address-map/supplied-prom-origin200-config",
                        NULL, test_supplied_prom_gets_origin200_config);
    qtest_add_data_func("sn0/address-map/flash-three-views", NULL,
                        test_flash_three_views);
    qtest_add_data_func("sn0/address-map/dual-node-boot-windows", NULL,
                        test_dual_node_boot_windows);
    qtest_add_data_func("sn0/address-map/boot-windows-mirror-flash", NULL,
                        test_boot_windows_mirror_the_flash);
    qtest_add_data_func("sn0/address-map/flash-hub-local-is-fast", NULL,
                        test_flash_hub_local_is_fast);
    qtest_add_data_func("sn0/address-map/flash-wide-read-command-mode", NULL,
                        test_flash_wide_read_in_command_mode);
    qtest_add_data_func("sn0/address-map/entry-points", NULL,
                        test_entry_points);
    qtest_add_data_func("sn0/address-map/flash-rejects-bare-writes", NULL,
                        test_flash_rejects_bare_writes);
    qtest_add_data_func("sn0/address-map/flash-autoselect", NULL,
                        test_flash_autoselect);
    qtest_add_data_func("sn0/address-map/flash-program-and-erase", NULL,
                        test_flash_program_and_erase);
    qtest_add_data_func("sn0/address-map/flash-empty-factory-log", NULL,
                        test_flash_has_empty_factory_log);
    qtest_add_data_func("sn0/address-map/memory-128m", NULL,
                        test_128m_memory);
    qtest_add_data_func("sn0/address-map/memory-256m", NULL,
                        test_256m_memory);
    qtest_add_data_func("sn0/address-map/memory-512m", NULL,
                        test_512m_memory);
    qtest_add_data_func("sn0/address-map/hub-local-memory-alias", NULL,
                        test_hub_local_memory_alias);
    qtest_add_data_func("sn0/address-map/promdata-window-agrees-across-nodes",
                        NULL, test_promdata_window_agrees_across_nodes);
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

    return g_test_run();
}
