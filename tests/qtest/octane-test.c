/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics-free Octane profile: synthetic RAM and raw flash policy. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "hw/core/clock.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "sgi-launch.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#include <sys/socket.h>
#include <netinet/in.h>

#define HEART 0x0ff00000ULL
#define XBOW 0x10000000ULL
#define HEART_WIDGET 0x18000000ULL
#define BRIDGE 0x1f000000ULL
#define OCTANE_LONG_BRIDGE (15ULL << 36)
#define FLASH (BRIDGE + 0xc00000)
#define MPCONF 0x600ULL
#define FLASH_SELECT_SIZE 0x200000
#define FLASH_WRITE_EN (1u << 31)
#define RAM 0x20000000ULL
#define LOW_RAM_SIZE 0x4000
#define LOW_RAM_HALF 0x2000
#define HIGH_GUARD RAM
#define HIGH_RAM (RAM + 0x4000)
#define ATTR0 (1ULL << 40)
#define UNIT 0x02000000ULL
#define BANK0 0x80030000U
#define OCTANE2_BANK0 0x80070000U
#define WIDE 0x80070000U
#define CFG(slot) (BRIDGE + 0x020000 + (slot) * 0x1000)
#define BRIDGE_NIC (BRIDGE + 0x0b4)
#define IOC3 (BRIDGE + 0x600000)
#define IOC3_MICROLAN (IOC3 + 0x30)
#define IOC3_PIN_CONTROL_SET (IOC3 + 0x34)
#define IOC3_ENET_MII_DATA_R (IOC3 + 0x14c)
#define IOC3_ENET_MII_CONTROL (IOC3 + 0x148)
#define IOC3_ENET_MAC_CONTROL (IOC3 + 0x0f0)
#define IOC3_ENET_INT_STATUS (IOC3 + 0x0f4)
#define IOC3_ENET_INT_ENABLE (IOC3 + 0x0f8)
#define IOC3_ENET_RX_RING_LO (IOC3 + 0x104)
#define IOC3_ENET_RX_CONSUME (IOC3 + 0x10c)
#define IOC3_ENET_RX_PRODUCE (IOC3 + 0x110)
#define IOC3_ENET_RX_TIMER (IOC3 + 0x114)
#define IOC3_ENET_TX_RING_LO (IOC3 + 0x12c)
#define IOC3_ENET_TX_CONSUME (IOC3 + 0x130)
#define IOC3_ENET_TX_PRODUCE (IOC3 + 0x134)
#define IOC3_PAR_BUFFER_LO_A (IOC3 + 0x084)
#define IOC3_PAR_COUNT_A (IOC3 + 0x088)
#define IOC3_PAR_ECP_DATA (IOC3 + 0x20150)
#define IOC3_PAR_ECP_FIFO (IOC3 + 0x20158)
#define IOC3_PAR_ECP_CONFIG_B (IOC3 + 0x20159)
#define IOC3_PAR_ECP_EXTENDED (IOC3 + 0x2015a)
#define RTC_INDEX (IOC3 + 0xa0000)
#define RTC_DATA (IOC3 + 0xc0000)
#define UART_A (IOC3 + 0x20178)
#define UART_B (IOC3 + 0x20170)
#define IOC3_SERIAL_IRQ (1u << 6)
#define RAD_BAR0 (BRIDGE + 0x700000)
#define RAD_PCI_BAR0 0x13000000u
#define RAD_PCI_ID 0x000510a9u
#define RAD_REVISION 0xc0u
#define BRIDGE_REG(offset) (BRIDGE + (offset) + 4)
#define SUPERIO_PIN (1u << 4)
#define RAD_PIN (1u << 3)
#define SUPERIO_VECTOR (1u << 16)
#define SUPERIO_CAUSE 0x800u
#define ENET_PIN (1u << 2)
#define ENET_VECTOR (1u << 17)
#define ENET_INT_RX_TIMER (1u << 0)
#define ENET_INT_RX_THRESHOLD (1u << 1)
#define ENET_INT_TX_COMPLETE (1u << 22)
#define SCSI0_WINDOW (BRIDGE + 0x200000)
#define BRIDGE_SWAP_DIRECT (1u << 19)
/* Bridge reverses the two halfword lanes within each 32-bit PCI word. */
#define ISP_MAILBOX(n) (SCSI0_WINDOW + ((0x70 + 2 * (n)) ^ 2))
#define ISP_HOST_CMD (SCSI0_WINDOW + 0xc2)
#define ISP_MBOX_COMPLETE 0x4000
#define ISP_MBOX_HOST_ERROR 0x4002

static char *flash_path;

static QTestState *octane_start_image_args(const char *image,
                                           const char *extra)
{
    g_autofree char *quoted = g_shell_quote(image);

    return qtest_initf("-M octane -m 128M -cpu R10000 -bios %s -S %s",
                       quoted, extra);
}

static QTestState *octane_start_args(const char *extra)
{
    return octane_start_image_args(flash_path, extra);
}

static QTestState *octane_start(void)
{
    return octane_start_args("");
}

static QTestState *octane_start_serial(int *sock)
{
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *args = g_strdup_printf(
        "-M octane -m 128M -cpu R10000 -bios %s -S", quoted);

    return qtest_init_with_serial(args, sock);
}

/* Test-owned R12000 configuration, not measured Octane2 reset values. */
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

static QTestState *octane2_start_args(const char *extra)
{
    g_autofree char *quoted = g_shell_quote(flash_path);

    return qtest_initf("-M octane2 -m 256M -cpu " OCTANE2_CPU
                       " -bios %s -S %s", quoted, extra);
}

static void octane_heart_identity_check(QTestState *qts, bool octane2)
{
    const uint32_t expected = octane2 ? 0x6c00106d : 0x4c00106d;
    uint64_t wide;
    uint32_t word;

    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, expected);
    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 4, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, expected);
    g_assert_cmphex(word, ==, (uint32_t)wide);
    g_assert_cmphex(wide >> 32, ==, 0);
    g_assert_cmphex(word >> 28, ==, octane2 ? 6 : 4);
    g_assert_cmphex(word & 1, ==, 1);
    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET + 8, &wide), ==,
                     octane2 ? MEMTX_OK : MEMTX_ERROR);
    if (octane2) {
        g_assert_cmphex(wide, ==, 0x60);
    }
}

static uint64_t octane_cpu_clock_period(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/cpu-refclk',"
        "'property':'qtest-clock-period'}}");

    return qdict_get_int(reply, "return");
}

static void test_cpu_clock(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    uint64_t expected = CLOCK_PERIOD_FROM_HZ(octane2 ? 360000000 :
                                              195000000);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();

    g_assert_cmpuint(octane_cpu_clock_period(qts), ==, expected);
    qtest_system_reset(qts);
    g_assert_cmpuint(octane_cpu_clock_period(qts), ==, expected);
    qtest_quit(qts);
}

static void test_octane2_population(void)
{
    QTestState *qts = octane2_start_args("");
    uint32_t value;
    uint64_t missing;

    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, OCTANE2_BANK0);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 4), ==, 0x6c00106d);
    g_assert_cmpuint(qtest_readl_tx(qts, XBOW + 4, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x50000001);
    g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, 0x1b000004, &value), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, CFG(0)), ==, 0x10201077);
    g_assert_cmphex(qtest_readl(qts, CFG(1)), ==, 0x10201077);
    g_assert_cmphex(qtest_readl(qts, CFG(2)), ==, 0x000310a9);
    g_assert_cmphex(qtest_readl(qts, CFG(3)), ==, RAD_PCI_ID);

    qtest_writeq(qts, HIGH_RAM, 0x0123456789abcdefULL);
    qtest_writeq(qts, RAM + 8 * UNIT - 8, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, RAM + 8 * UNIT - 8), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM + 8 * UNIT, &missing), !=,
                     MEMTX_OK);
    qtest_writel(qts, HEART + 0x20, BANK0);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM + 4 * UNIT, &missing), !=,
                     MEMTX_OK);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, XBOW + 4), ==, 0x50000001);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, OCTANE2_BANK0);
    g_assert_cmphex(qtest_readq(qts, RAM + 8 * UNIT - 8), ==,
                    0xfedcba9876543210ULL);
    qtest_quit(qts);
}

static void test_memory_populations(void)
{
    struct OctaneMemoryCase {
        const char *machine;
        const char *options;
        unsigned size[4];
    };
    static const struct OctaneMemoryCase cases[] = {
        { "octane", "", { 128, 0, 0, 0 } },
        { "octane", ",bank0-size=67108864,bank1-size=67108864",
          { 64, 64, 0, 0 } },
        { "octane2", "", { 256, 0, 0, 0 } },
        { "octane2", ",bank0-size=134217728,bank1-size=134217728",
          { 128, 128, 0, 0 } },
        { "octane2", ",bank0-size=67108864,bank1-size=67108864,"
          "bank2-size=67108864,bank3-size=67108864",
          { 64, 64, 64, 64 } },
    };
    g_autofree char *quoted = g_shell_quote(flash_path);

    for (unsigned n = 0; n < G_N_ELEMENTS(cases); n++) {
        const struct OctaneMemoryCase *test = &cases[n];
        const char *cpu = !strcmp(test->machine, "octane2") ?
            OCTANE2_CPU : "R10000";
        unsigned base = 0;
        QTestState *qts = qtest_initf("-M %s%s -m %uM -cpu %s -bios %s -S",
                                     test->machine, test->options,
                                     !strcmp(test->machine, "octane2") ?
                                     256 : 128, cpu, quoted);

        for (unsigned bank = 0; bank < 4; bank++) {
            unsigned size = test->size[bank];
            uint32_t descriptor = size ?
                0x80000000U | ((size / 32 - 1) << 16) | (base / 32) : 0;

            g_assert_cmphex(qtest_readl(qts, HEART + 0x20 + bank * 4), ==,
                            descriptor);
            if (size) {
                uint64_t first = 0x1020304050607080ULL + bank;
                uint64_t last = 0x8070605040302010ULL + bank;
                uint64_t start = base ? RAM + (uint64_t)base * MiB : HIGH_RAM;
                uint64_t end = RAM + (uint64_t)(base + size) * MiB - 8;

                qtest_writeq(qts, start, first);
                qtest_writeq(qts, end, last);
                base += size;
            }
        }
        base = 0;
        for (unsigned bank = 0; bank < 4; bank++) {
            if (test->size[bank]) {
                uint64_t start = base ? RAM + (uint64_t)base * MiB : HIGH_RAM;
                uint64_t end = RAM +
                    (uint64_t)(base + test->size[bank]) * MiB - 8;

                g_assert_cmphex(qtest_readq(qts, start), ==,
                                0x1020304050607080ULL + bank);
                g_assert_cmphex(qtest_readq(qts, end), ==,
                                0x8070605040302010ULL + bank);
                base += test->size[bank];
            }
        }
        g_assert_cmpuint(base, ==, !strcmp(test->machine, "octane2") ?
                         256 : 128);
        {
            uint64_t missing;

            g_assert_cmpuint(qtest_readq_tx(qts, RAM + (uint64_t)base * MiB,
                                            &missing), !=, MEMTX_OK);
        }
        base = 0;
        for (unsigned bank = 0; bank < 4; bank++) {
            uint64_t missing;
            uint64_t start = base ? RAM + (uint64_t)base * MiB : HIGH_RAM;

            qtest_writel(qts, HEART + 0x20 + bank * 4, 0);
            g_assert_cmphex(qtest_readl(qts, HEART + 0x20 + bank * 4), ==, 0);
            if (test->size[bank]) {
                g_assert_cmpuint(qtest_readq_tx(qts, start, &missing), !=,
                                 MEMTX_OK);
            }
            base += test->size[bank];
        }
        qtest_system_reset(qts);
        for (unsigned bank = 0, offset = 0; bank < 4; bank++) {
            unsigned size = test->size[bank];
            uint32_t descriptor = size ?
                0x80000000U | ((size / 32 - 1) << 16) | (offset / 32) : 0;

            g_assert_cmphex(qtest_readl(qts, HEART + 0x20 + bank * 4), ==,
                            descriptor);
            if (size) {
                uint64_t start = offset ?
                    RAM + (uint64_t)offset * MiB : HIGH_RAM;
                uint64_t end = RAM + (uint64_t)(offset + size) * MiB - 8;

                g_assert_cmphex(qtest_readq(qts, start), ==,
                                0x1020304050607080ULL + bank);
                g_assert_cmphex(qtest_readq(qts, end), ==,
                                0x8070605040302010ULL + bank);
            }
            offset += size;
        }
        qtest_quit(qts);
    }
}

static void test_memory_population_rejections(void)
{
    static const struct {
        const char *machine;
        const char *ram;
        const char *error;
    } cases[] = {
        { "octane,bank0-size=67108864,bank2-size=67108864", "128M",
          "unsupported HEART bank population" },
        { "octane,bank0-size=134217728,bank1-size=67108864", "128M",
          "unsupported HEART bank population" },
        { "octane2,bank0-size=134217728,bank1-size=67108864,"
          "bank2-size=67108864", "256M",
          "unsupported HEART bank population" },
        { "octane2,bank0-size=67108864,bank1-size=67108864,"
          "bank2-size=67108864,bank3-size=67108864", "128M",
          "supports one CPU and 256 MiB of RAM" },
    };

    for (unsigned n = 0; n < G_N_ELEMENTS(cases); n++) {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", cases[n].machine,
            "-m", cases[n].ram, "-display", "none", NULL
        };
        g_autofree char *error_output = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                                   NULL, &error_output, &status, NULL));
        g_assert_false(g_spawn_check_exit_status(status, NULL));
        g_assert_nonnull(strstr(error_output, cases[n].error));
    }
}

static bool octane_onewire_slot(QTestState *qts, uint64_t master,
                                unsigned int pulse, unsigned int sample)
{
    uint32_t command = (pulse << 10) | (sample << 2);
    uint32_t result;

    if (master == HEART + 0xb8) {
        qtest_writeq(qts, master, command);
    } else {
        qtest_writel(qts, master, command);
    }
    if (master == HEART + 0xb8 || master == HEART + 0xbc) {
        qtest_clock_step(qts, (pulse + sample) * 1000);
    }
    result = master == HEART + 0xb8 ? qtest_readq(qts, master) :
        qtest_readl(qts, master);
    g_assert_cmphex(result & 2, ==, 2);
    return result & 1;
}

static void octane_onewire_write_byte(QTestState *qts, uint64_t master,
                                      uint8_t value)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        octane_onewire_slot(qts, master, value & (1u << bit) ? 6 : 80,
                            value & (1u << bit) ? 13 : 30);
    }
}

static uint8_t octane_onewire_read_byte(QTestState *qts, uint64_t master)
{
    uint8_t value = 0;

    for (unsigned int bit = 0; bit < 8; bit++) {
        value |= octane_onewire_slot(qts, master, 6, 13) << bit;
    }
    return value;
}


typedef struct OctaneBoardIdentity {
    uint64_t master;
    uint64_t rom;
    const char *barcode;
    const char *part;
    const char *revision;
    const char *name;
} OctaneBoardIdentity;

/* Reviewed virtual identities; the ROM serials and barcodes are synthetic. */
static const OctaneBoardIdentity octane_board_identities[2][4] = {
    {
        { HEART + 0xb8, 0x817a30010000010bULL, "SIM3010001",
          "030-0888-004", "B", "PM10" },
        { BRIDGE_NIC, 0xd87a30010000020bULL, "SIM3010002",
          "030-0887-003", "F", "IP30" },
        { IOC3_MICROLAN, 0xef7a30010000030bULL, "SIM3010003",
          "030-0891-003", "A", "FP1" },
        { IOC3_MICROLAN, 0x6a7a30010000040bULL, "SIM3010004",
          "060-0028-003", "A", "PWR.SPPLY.SR" },
    }, {
        { HEART + 0xb8, 0x657a30020000010bULL, "SIM3020001",
          "030-1590-002", "B", "PM10360MHZ" },
        { BRIDGE_NIC, 0x3c7a30020000020bULL, "SIM3020002",
          "030-1467-001", "B", "IP30" },
        { IOC3_MICROLAN, 0x0b7a30020000030bULL, "SIM3020003",
          "030-0891-003", "H", "FP1" },
        { IOC3_MICROLAN, 0x8e7a30020000040bULL, "SIM3020004",
          "060-0035-002", "A", "PWR.SPPLY.ER" },
    },
};

static uint8_t octane_crc8(const uint8_t *bytes, size_t size)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0x8c : 0);
        }
    }
    return crc;
}

static uint16_t octane_crc16(const uint8_t *bytes, size_t size)
{
    uint16_t crc = 0;

    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xa001 : 0);
        }
    }
    return crc;
}

static void octane_identity_map(QTestState *qts)
{
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, IOC3_PIN_CONTROL_SET, 1u << 21);
}

static unsigned octane_onewire_search(QTestState *qts, uint64_t master,
                                      uint64_t found[3])
{
    unsigned count = 0, last = 0;
    uint64_t previous = 0;

    do {
        uint64_t rom = 0;
        unsigned next = 0;
        uint8_t bytes[8];

        g_assert_false(octane_onewire_slot(qts, master, 520, 65));
        octane_onewire_write_byte(qts, master, 0xf0);
        for (unsigned i = 1; i <= 64; i++) {
            bool bit = octane_onewire_slot(qts, master, 6, 13);
            bool inverse = octane_onewire_slot(qts, master, 6, 13);
            bool direction;

            g_assert_false(bit && inverse);
            if (bit != inverse) {
                direction = bit;
            } else {
                direction = i < last ? (previous >> (i - 1)) & 1 : i == last;
                if (!direction) {
                    next = i;
                }
            }
            rom |= (uint64_t)direction << (i - 1);
            octane_onewire_slot(qts, master, direction ? 6 : 80,
                                direction ? 110 : 30);
        }
        g_assert_cmpuint(count, <, 3);
        found[count++] = rom;
        previous = rom;
        last = next;
        stq_le_p(bytes, rom);
        g_assert_cmphex(octane_crc8(bytes, sizeof(bytes)), ==, 0);
    } while (last);

    return count;
}

static void octane_onewire_address(QTestState *qts, uint64_t master,
                                   uint64_t rom, uint8_t cmd, uint16_t addr)
{
    g_assert_false(octane_onewire_slot(qts, master, 520, 65));
    octane_onewire_write_byte(qts, master, 0x55);
    for (unsigned i = 0; i < 8; i++) {
        octane_onewire_write_byte(qts, master, rom >> (8 * i));
    }
    octane_onewire_write_byte(qts, master, cmd);
    octane_onewire_write_byte(qts, master, addr);
    octane_onewire_write_byte(qts, master, addr >> 8);
}

static void octane_onewire_read(QTestState *qts, uint64_t master,
                                uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        bytes[i] = octane_onewire_read_byte(qts, master);
    }
}

static void octane_identity_check(const OctaneBoardIdentity *want,
                                  const uint8_t page[64])
{
    char part[13];
    char revision[5];
    char name[15];

    g_assert_cmphex(page[0], ==, 1);
    g_assert_cmpmem(page + 1, 10, want->barcode, 10);
    for (unsigned i = 11; i < 21; i++) {
        g_assert_cmphex(page[i], ==, ' ');
    }
    for (unsigned i = 32; i < 35; i++) {
        g_assert_cmphex(page[i], ==, ' ');
    }
    memcpy(part, page + 21, 9);
    memcpy(part + 9, page + 35, 3);
    part[12] = 0;
    memcpy(revision, page + 38, 4);
    revision[4] = 0;
    memcpy(name, page + 48, 14);
    name[14] = 0;
    g_assert_cmpstr(part, ==, want->part);
    g_assert_cmpstr(g_strchomp(revision), ==, want->revision);
    g_assert_cmpstr(g_strchomp(name), ==, want->name);
    for (unsigned i = 42; i < 48; i++) {
        g_assert_cmphex(page[i], ==, 0xff);
    }
    g_assert_cmphex(octane_crc16(page, 32), ==, 0xb001);
    g_assert_cmphex(octane_crc16(page + 32, 32), ==, 0xb001);
}

static void octane_identity_read(QTestState *qts,
                                 const OctaneBoardIdentity *want)
{
    uint8_t page[80];
    uint8_t status[13] = { 0xaa, 0x00, 0x01 };
    uint8_t extended[37];
    uint8_t tail[7] = { 0xf0, 0xfe, 0x07 };

    octane_onewire_address(qts, want->master, want->rom, 0xaa, 0x100);
    for (unsigned i = 0; i < 8; i++) {
        octane_onewire_read(qts, want->master, status + 3, 10);
        for (unsigned j = 3; j < 11; j++) {
            g_assert_cmphex(status[j], ==, 0xff);
        }
        g_assert_cmphex(octane_crc16(status + (i ? 3 : 0),
                                    i ? 10 : 13), ==, 0xb001);
    }

    octane_onewire_address(qts, want->master, want->rom, 0xf0, 0);
    octane_onewire_read(qts, want->master, page, sizeof(page));
    octane_identity_check(want, page);
    for (unsigned i = 64; i < sizeof(page); i++) {
        g_assert_cmphex(page[i], ==, 0xff);
    }

    /* The terminal F0 check is distinct from the two stored page checks. */
    octane_onewire_address(qts, want->master, want->rom, 0xf0, 0x7fe);
    octane_onewire_read(qts, want->master, tail + 3, 4);
    g_assert_cmphex(tail[3], ==, 0xff);
    g_assert_cmphex(tail[4], ==, 0xff);
    g_assert_cmphex(octane_crc16(tail, sizeof(tail)), ==, 0xb001);
    g_assert_cmphex(octane_onewire_read_byte(qts, want->master), ==, 0xff);

    octane_onewire_address(qts, want->master, want->rom, 0xa5, 0);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t header[6] = { 0xa5, 0, 0 };

        octane_onewire_read(qts, want->master, header + 3, 3);
        g_assert_cmphex(header[3], ==, 0xff);
        g_assert_cmphex(octane_crc16(header + (i ? 3 : 0),
                                    i ? 3 : 6), ==, 0xb001);
        octane_onewire_read(qts, want->master, extended, 34);
        g_assert_cmpmem(extended, 32, page + 32 * i, 32);
        g_assert_cmphex(octane_crc16(extended, 34), ==, 0xb001);
    }
}

static void test_board_identities(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const OctaneBoardIdentity *boards = octane_board_identities[octane2];
    QTestState *qts = octane2 ?
        octane2_start_args("-nic user,mac=08:00:69:12:34:56") :
        octane_start_args("-nic user,mac=08:00:69:12:34:56");
    uint64_t found[3], ethernet = 0;
    uint8_t reply[3][64], broadcast[64];
    OctaneBoardIdentity low = boards[0];

    octane_identity_map(qts);
    for (unsigned bus = 0; bus < 3; bus++) {
        unsigned count = octane_onewire_search(qts, boards[bus].master, found);

        g_assert_cmpuint(count, ==, bus == 2 ? 3 : 1);
        for (unsigned i = 0; i < count; i++) {
            if (bus == 2 && (found[i] & 0xff) == 0x91) {
                ethernet = found[i];
                g_assert_cmphex((ethernet >> 8) & 0xffffffffffffULL, ==,
                                0x5e7000000001ULL);
            } else {
                bool matched = false;

                for (unsigned j = bus; j < (bus == 2 ? 4 : bus + 1); j++) {
                    matched |= found[i] == boards[j].rom;
                }
                g_assert_true(matched);
            }
            for (unsigned j = 0; j < i; j++) {
                g_assert_cmphex(found[j], !=, found[i]);
            }
        }
    }
    g_assert_cmphex(ethernet, !=, 0);
    for (unsigned i = 0; i < 4; i++) {
        uint8_t silence[8];

        octane_identity_read(qts, &boards[i]);
        octane_onewire_address(qts, boards[i].master,
                               boards[i].rom ^ (1ULL << 63), 0xf0, 0);
        octane_onewire_read(qts, boards[i].master, silence, sizeof(silence));
        for (unsigned j = 0; j < sizeof(silence); j++) {
            g_assert_cmphex(silence[j], ==, 0xff);
        }
    }
    /* The low-word firmware path reaches the same HEART responder. */
    low.master = HEART + 0xbc;
    octane_identity_read(qts, &low);

    for (unsigned i = 0; i < 3; i++) {
        octane_onewire_address(qts, IOC3_MICROLAN,
                               i == 2 ? ethernet : boards[2 + i].rom, 0xf0, 0);
        octane_onewire_read(qts, IOC3_MICROLAN, reply[i], sizeof(reply[i]));
    }
    g_assert_cmpint(memcmp(reply[0], reply[1], sizeof(reply[0])), !=, 0);
    g_assert_false(octane_onewire_slot(qts, IOC3_MICROLAN, 520, 65));
    octane_onewire_write_byte(qts, IOC3_MICROLAN, 0xcc);
    octane_onewire_write_byte(qts, IOC3_MICROLAN, 0xf0);
    octane_onewire_write_byte(qts, IOC3_MICROLAN, 0);
    octane_onewire_write_byte(qts, IOC3_MICROLAN, 0);
    octane_onewire_read(qts, IOC3_MICROLAN, broadcast, sizeof(broadcast));
    for (unsigned i = 0; i < sizeof(broadcast); i++) {
        g_assert_cmphex(broadcast[i], ==,
                        reply[0][i] & reply[1][i] & reply[2][i]);
    }
    g_assert_cmpuint(octane_onewire_search(qts, IOC3_MICROLAN, found), ==, 3);
    octane_identity_read(qts, &boards[3]);
    qtest_system_reset(qts);
    octane_identity_map(qts);
    for (unsigned i = 0; i < 4; i++) {
        octane_identity_read(qts, &boards[i]);
    }
    qtest_quit(qts);
}

static void test_board_identity_migration(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    const OctaneBoardIdentity *boards = octane_board_identities[octane2];
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("octane-identity-migration-XXXXXX", &error);
    g_autofree char *socket =
        g_build_filename(directory, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = octane2 ? octane2_start_args("") : octane_start();
    QTestState *to = octane2 ? octane2_start_args("-incoming defer") :
        octane_start_args("-incoming defer");
    uint8_t records[3][64];

    g_assert_no_error(error);
    octane_heart_identity_check(from, octane2);
    octane_identity_map(from);
    for (unsigned i = 0; i < 3; i++) {
        octane_onewire_address(from, boards[i].master, boards[i].rom, 0xf0, 0);
        octane_onewire_read(from, boards[i].master, records[i], 17);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    octane_heart_identity_check(to, octane2);
    for (unsigned i = 0; i < 3; i++) {
        octane_onewire_read(to, boards[i].master, records[i] + 17, 47);
        octane_identity_check(&boards[i], records[i]);
    }
    qtest_system_reset(to);
    octane_heart_identity_check(to, octane2);
    octane_identity_map(to);
    for (unsigned i = 0; i < 4; i++) {
        octane_identity_read(to, &boards[i]);
    }
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_octane_ioc3_ethernet(bool octane2)
{
    static const uint8_t mac[] = { 0x08, 0x00, 0x69, 0x12, 0x34, 0x56 };
    QTestState *qts = octane2 ?
        octane2_start_args("-nic user,mac=08:00:69:12:34:56") :
        octane_start_args("-nic user,mac=08:00:69:12:34:56");
    uint8_t page[11];
    uint64_t found[3], ethernet = 0;

    g_assert_cmphex(qtest_readl(qts, CFG(2)), ==, 0x000310a9);
    g_assert_cmphex(qtest_readl(qts, CFG(2) + 0x2c), ==, 0xc30410a9);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_MII_DATA_R), ==, 0xffff);
    qtest_writel(qts, IOC3_ENET_MII_CONTROL, (1u << 5) | 0x400 | 2);
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_MII_DATA_R), ==, 0x0015);
    qtest_writel(qts, IOC3_ENET_MII_CONTROL, (1u << 5) | 0x400 | 3);
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_MII_DATA_R), ==, 0xf423);

    qtest_writel(qts, IOC3_PIN_CONTROL_SET, 1u << 21);
    g_assert_cmpuint(octane_onewire_search(qts, IOC3_MICROLAN, found), ==, 3);
    for (unsigned i = 0; i < 3; i++) {
        if ((found[i] & 0xff) == 0x91) {
            ethernet = found[i];
        }
    }
    g_assert_cmphex(ethernet, !=, 0);
    octane_onewire_address(qts, IOC3_MICROLAN, ethernet, 0xf0, 0);
    octane_onewire_read_byte(qts, IOC3_MICROLAN); /* Command/address CRC. */
    for (unsigned int i = 0; i < sizeof(page); i++) {
        page[i] = octane_onewire_read_byte(qts, IOC3_MICROLAN);
    }
    g_assert_cmphex(page[0], ==, 0x0a);
    for (unsigned int i = 0; i < sizeof(mac); i++) {
        g_assert_cmphex(page[10 - i], ==, mac[i]);
    }
    qtest_quit(qts);
}

static void test_octane_ethernet(void)
{
    test_octane_ioc3_ethernet(false);
}

static void test_ioc3_parallel(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();

    g_assert_cmphex(qtest_readl(qts, CFG(2) + 0x2c), ==, 0xc30410a9);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, IOC3_PAR_BUFFER_LO_A, 0x11112000);
    g_assert_cmphex(qtest_readl(qts, IOC3_PAR_BUFFER_LO_A), ==, 0x11112000);
    qtest_writel(qts, IOC3_PAR_COUNT_A, 0x5a5a);
    g_assert_cmphex(qtest_readl(qts, IOC3_PAR_COUNT_A) & 0xffff, ==, 0x5a5a);
    qtest_writeb(qts, IOC3_PAR_ECP_DATA, 0x5a);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_DATA), ==, 0x5a);
    qtest_quit(qts);
}

static void test_ioc3_parallel_fifo(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);

    /* TI test mode bypasses the cable handshake. */
    qtest_writeb(qts, IOC3_PAR_ECP_EXTENDED, 0xc4);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_EXTENDED), ==, 0xc5);
    for (unsigned i = 0; i < 16; i++) {
        qtest_writeb(qts, IOC3_PAR_ECP_FIFO, 0x30 + i);
        g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_EXTENDED), ==,
                       i == 15 ? 0xc6 : 0xc4);
    }
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_FIFO), ==, 0x30 + i);
        g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_EXTENDED), ==,
                       i == 15 ? 0xc5 : 0xc4);
    }

    qtest_writeb(qts, IOC3_PAR_ECP_EXTENDED, 0xf4);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_FIFO), ==, 0x1f);
    /* CNFGB's IRQ sample remains outside the modeled wiring contract. */
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_CONFIG_B) & ~0x40, ==, 0x0b);
    qtest_writeb(qts, IOC3_PAR_ECP_FIFO, 0x55);
    qtest_writeb(qts, IOC3_PAR_ECP_CONFIG_B, 0xff);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_FIFO), ==, 0x1f);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_CONFIG_B) & ~0x40, ==, 0x0b);

    qtest_writeb(qts, IOC3_PAR_ECP_EXTENDED, 0x04);
    qtest_writeb(qts, IOC3_PAR_ECP_EXTENDED, 0xc4);
    qtest_writeb(qts, IOC3_PAR_ECP_FIFO, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_EXTENDED), ==, 0xc4);
    qtest_system_reset(qts);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, IOC3_PAR_ECP_EXTENDED, 0xc4);
    g_assert_cmphex(qtest_readb(qts, IOC3_PAR_ECP_EXTENDED), ==, 0xc5);
    qtest_quit(qts);
}

static void test_octane_ioc3_ethernet_polled(bool octane2)
{
    const uint32_t pci_base = 0xa0000000u;
    const uint32_t tx_desc = 0x4000;
    const uint32_t rx_ring = 0x8000;
    const uint32_t rx_buf = 0xc000;
    uint8_t frame[60], landed[sizeof(frame)], sent[sizeof(frame)];
    uint32_t length;
    struct timeval timeout = { .tv_sec = 5 };
    g_autofree char *args = NULL;
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    g_assert_cmpint(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                               sizeof(timeout)), ==, 0);
    args = g_strdup_printf("-nic socket,fd=%d", sv[1]);
    qts = octane2 ? octane2_start_args(args) : octane_start_args(args);
    /* The machine helpers launch paused; socket receive needs a running VM. */
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000006);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);

    memset(frame, 0, sizeof(frame));
    memset(frame, 0xff, 6);
    for (unsigned int i = 6; i < sizeof(frame); i++) {
        frame[i] = i;
    }

    qtest_writeq(qts, RAM + rx_ring, pci_base + rx_buf);
    qtest_writeq(qts, RAM + rx_ring + 8, pci_base + rx_buf + 0x1000);
    qtest_writel(qts, IOC3_ENET_RX_RING_LO, pci_base + rx_ring);
    qtest_writel(qts, IOC3_ENET_RX_PRODUCE, 16);
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_INT_ENABLE), ==, 0);
    qtest_writel(qts, IOC3_ENET_MAC_CONTROL,
                 (1u << 1) | (17u << 3) | (1u << 2) |
                 (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16));

    length = htonl(sizeof(frame));
    g_assert_cmpint(send(sv[0], &length, sizeof(length), 0), ==,
                    sizeof(length));
    g_assert_cmpint(send(sv[0], frame, sizeof(frame), 0), ==, sizeof(frame));

    /* The buffer valid bit is the poll result; no index or IRQ is needed. */
    for (unsigned int i = 0; i < 200; i++) {
        if (qtest_readl(qts, RAM + rx_buf) & (1u << 31)) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(qts, 50000);
    }
    g_assert_cmphex(qtest_readl(qts, RAM + rx_buf) & (1u << 31), ==,
                    1u << 31);
    qtest_memread(qts, RAM + rx_buf + 34, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & ENET_PIN, ==, 0);

    qtest_writel(qts, RAM + tx_desc, sizeof(frame) | (1u << 16));
    qtest_writel(qts, RAM + tx_desc + 4, sizeof(frame));
    qtest_memwrite(qts, RAM + tx_desc + 24, frame, sizeof(frame));
    qtest_writel(qts, IOC3_ENET_TX_RING_LO, pci_base + tx_desc);
    qtest_writel(qts, IOC3_ENET_TX_PRODUCE, 128);
    g_assert_cmpint(recv(sv[0], &length, sizeof(length), MSG_WAITALL), ==,
                    sizeof(length));
    g_assert_cmpuint(ntohl(length), ==, sizeof(frame));
    g_assert_cmpint(recv(sv[0], sent, sizeof(sent), MSG_WAITALL), ==,
                    sizeof(sent));
    g_assert_cmpmem(sent, sizeof(sent), frame, sizeof(frame));
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_TX_CONSUME), ==,
                    128 | (1u << 31));
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & ENET_PIN, ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_octane_ethernet_polled(void)
{
    test_octane_ioc3_ethernet_polled(false);
}

static void test_octane2_ethernet_polled(void)
{
    test_octane_ioc3_ethernet_polled(true);
}

static uint32_t octane_cpu_cause(QTestState *qts);

/* Model integration: Bridge DMA and IRQ routing, not an IP30 board contract. */
static void test_octane_ioc3_ethernet_dma_and_interrupt(bool octane2)
{
    const uint32_t pci_base = 0xa0000000u;
    const uint32_t tx_desc = 0x4000;
    const uint32_t rx_ring = 0x8000;
    const uint32_t rx_buf = 0xc000;
    const uint32_t irq_bits = ENET_INT_RX_TIMER | ENET_INT_TX_COMPLETE;
    uint8_t frame[60];
    uint8_t landed[sizeof(frame)];
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();

    memset(frame, 0, sizeof(frame));
    memset(frame, 0xff, 6);
    for (unsigned int i = 6; i < sizeof(frame); i++) {
        frame[i] = i;
    }

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000006);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);

    qtest_writel(qts, BRIDGE_REG(0x030), 8u << 16);
    qtest_writel(qts, BRIDGE_REG(0x038), 0x80);
    qtest_writel(qts, BRIDGE_REG(0x130 + 8 * 2), 17);
    qtest_writel(qts, BRIDGE_REG(0x118), ENET_PIN);
    qtest_writel(qts, BRIDGE_REG(0x108), ENET_PIN);
    qtest_writeq(qts, HEART + 0x10000, ENET_VECTOR);

    qtest_writeq(qts, RAM + rx_ring, pci_base + rx_buf);
    qtest_writel(qts, IOC3_ENET_RX_RING_LO, pci_base + rx_ring);
    qtest_writel(qts, IOC3_ENET_RX_TIMER, 0);
    qtest_writel(qts, IOC3_ENET_RX_PRODUCE, 8 | (1u << 31));
    qtest_writel(qts, IOC3_ENET_INT_ENABLE, irq_bits);
    qtest_writel(qts, IOC3_ENET_MAC_CONTROL,
                 (1u << 1) | (33u << 3) | (1u << 13) | (1u << 14) |
                 (1u << 15) | (1u << 16) | (1u << 17));

    qtest_writel(qts, RAM + tx_desc, sizeof(frame) | (1u << 12) |
                 (1u << 16));
    qtest_writel(qts, RAM + tx_desc + 4, sizeof(frame));
    qtest_memwrite(qts, RAM + tx_desc + 24, frame, sizeof(frame));
    qtest_writel(qts, IOC3_ENET_TX_RING_LO, pci_base + tx_desc);
    qtest_writel(qts, IOC3_ENET_TX_PRODUCE, 128);

    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_TX_CONSUME), ==,
                    128 | (1u << 31));
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_RX_CONSUME), ==, 8);
    g_assert_cmphex(qtest_readl(qts, RAM + rx_buf) & 0x80000000u, ==,
                    0x80000000u);
    qtest_memread(qts, RAM + rx_buf + 66, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    g_assert_cmphex(qtest_readl(qts, IOC3_ENET_INT_STATUS) & irq_bits, ==,
                    irq_bits);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & ENET_PIN, ==,
                    ENET_PIN);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & ENET_VECTOR, ==,
                    ENET_VECTOR);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);

    qtest_writel(qts, IOC3_ENET_INT_STATUS, irq_bits);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & ENET_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & ENET_VECTOR, ==, 0);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==, 0);
    qtest_quit(qts);
}

static void test_octane_ethernet_dma_and_interrupt(void)
{
    test_octane_ioc3_ethernet_dma_and_interrupt(false);
}

static void test_octane2_ethernet_dma_and_interrupt(void)
{
    test_octane_ioc3_ethernet_dma_and_interrupt(true);
}

static void test_octane2_ethernet(void)
{
    test_octane_ioc3_ethernet(true);
}

static void test_octane2_serial(void)
{
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *args = g_strdup_printf(
        "-M octane2 -m 256M -cpu " OCTANE2_CPU " -bios %s -S", quoted);
    int sock;
    QTestState *qts = qtest_init_with_serial(args, &sock);
    const uint8_t sent = '2';
    uint8_t received;
    struct timeval deadline = { .tv_sec = 5 };

    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, UART_A + 3, 0x03);
    qtest_writeb(qts, UART_A + 1, 0x01);
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);
    g_test_timer_start();
    while (!(qtest_readb(qts, UART_A + 5) & 1)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 5.0);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, UART_A), ==, sent);
    qtest_writeb(qts, UART_A, 'O');
    g_assert_cmpint(read(sock, &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 'O');
    close(sock);
    qtest_quit(qts);
}

static uint32_t octane_cpu_cause(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *field = strstr(registers, "Cause");
    uint32_t cause;

    g_assert_nonnull(field);
    g_assert_cmpint(sscanf(field, "Cause %" SCNx32, &cause), ==, 1);
    return cause;
}

static uint64_t octane_cpu_pc(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *field = strstr(registers, "pc=0x");
    uint64_t pc;

    g_assert_nonnull(field);
    g_assert_cmpint(sscanf(field, "pc=0x%" SCNx64, &pc), ==, 1);
    return pc;
}

static void test_widgets_and_memory(void)
{
    QTestState *qts = octane_start();
    uint32_t value;
    uint64_t missing;

    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, BANK0);
    g_assert_cmpuint(qtest_readl_tx(qts, XBOW + 4, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x40000001);
    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 4, &value), ==,
                     MEMTX_OK);
    g_assert_cmphex(value, ==, 0x4c00106d);
    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET, &missing), ==,
                     MEMTX_OK);
    g_assert_cmphex(missing, ==, (uint64_t)value);
    g_assert_cmpuint(qtest_readl_tx(qts, BRIDGE + 4, &value), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, 0x92000004, &value), !=, MEMTX_OK);

    qtest_writeq(qts, HIGH_RAM, 0x0123456789abcdefULL);
    qtest_writeq(qts, RAM + 4 * UNIT - 8, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, HIGH_RAM), ==, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, RAM + 4 * UNIT - 8), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM + 4 * UNIT, &missing), !=,
                     MEMTX_OK);

    qtest_writel(qts, HEART + 0x20, WIDE);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, WIDE);
    g_assert_cmphex(qtest_readq(qts, HIGH_RAM + 4 * UNIT), ==,
                    0x0123456789abcdefULL);
    qtest_writel(qts, HEART + 0x20, BANK0);
    g_assert_cmpuint(qtest_readq_tx(qts, RAM + 4 * UNIT, &missing), !=,
                     MEMTX_OK);
    qtest_writel(qts, HEART + 0x20, 0);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, 0);
    g_assert_cmpuint(qtest_readq_tx(qts, HIGH_RAM, &missing), !=, MEMTX_OK);
    g_assert_cmpuint(qtest_readq_tx(qts, ATTR0 + HIGH_RAM, &missing), !=,
                     MEMTX_OK);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, XBOW + 4), ==, 0x40000001);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, BANK0);
    g_assert_cmphex(qtest_readl(qts, ATTR0 + HEART + 0x20), ==, BANK0);
    g_assert_cmphex(qtest_readq(qts, HIGH_RAM), ==, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, ATTR0 + HIGH_RAM), ==,
                    0x0123456789abcdefULL);
    qtest_quit(qts);
}

static void test_heart_widget_control(void)
{
    QTestState *qts = octane_start();
    uint32_t word;

    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 0x24, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(qtest_writel_tx(qts, HEART_WIDGET + 0x24, 0x3ca596f0),
                     ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x24), ==, 0x3ca596f0);
    g_assert_cmphex(qtest_readl(qts, ATTR0 + HEART_WIDGET + 0x24), ==,
                    0x3ca596f0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x24), ==, 0);
    qtest_quit(qts);
}

static void test_heart_widget_errors(void)
{
    QTestState *qts = octane_start();
    uint64_t wide;

    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x44), ==, 0);
    qtest_writel(qts, HEART_WIDGET + 0x2c, 0x80);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0x80);
    g_assert_cmphex(qtest_readl(qts, ATTR0 + HEART_WIDGET + 0x2c), ==,
                    0x80);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x2c), ==, 0);
    qtest_writel(qts, HEART_WIDGET + 0x44, 0);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x44), ==, 0);
    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET + 0x60, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(qtest_writel_tx(qts, HEART_WIDGET + 0x44, 0), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writeq_tx(qts, HEART_WIDGET + 0x60, wide), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writeq_tx(qts, HEART_WIDGET + 0x68,
                                    0x3ff62c07), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x68), ==,
                    0x3ff62c07);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x68), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ATTR0 + HEART_WIDGET + 0x68), ==,
                    0x3ff62c07);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x68), ==, 0);
    qtest_quit(qts);
}

static void test_heart_diagnostic_storage(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();
    const uint64_t flow_a = 0xa51923456789abc3ULL;
    const uint64_t flow_b = 0x5a96fedcba76543cULL;
    uint64_t wide, count;
    uint32_t word;

    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x28), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x50), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x58), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30000), ==, 0);

    qtest_writel(qts, HEART_WIDGET + 0x2c, 0x12345678);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x28), ==, 0x12345678);
    qtest_writeq(qts, HEART_WIDGET + 0x28, 0xaabbccddeeadb5a7ULL);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x28), ==, 0xeeadb5a7);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0xeeadb5a7);
    qtest_writeq(qts, HEART_WIDGET + 0x28, 0x112233440019d246ULL);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x28), ==, 0x0019d246);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0x0019d246);
    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 0x28, &word), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writew_tx(qts, HEART_WIDGET + 0x2c, 0), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET + 0x30, &wide), !=,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x28), ==, 0x0019d246);

    qtest_writeq(qts, HEART + 0x50, flow_a);
    qtest_writeq(qts, HEART + 0x58, flow_b);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x50), ==, flow_a);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x58), ==, flow_b);
    qtest_writeq(qts, HEART + 0x50, flow_b ^ 0x123456789abcdef0ULL);
    qtest_writeq(qts, HEART + 0x58, flow_a ^ 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x50), ==,
                    flow_b ^ 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x58), ==,
                    flow_a ^ 0xfedcba9876543210ULL);

    qtest_writeq(qts, HEART + 0xb0, 0xfedcba9876548080ULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb0), ==,
                    0xfedcba9876548080ULL);
    qtest_writeq(qts, HEART + 0xb0, 0x123456789abca55aULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb0), ==,
                    0x123456789abca55aULL);

    qtest_writeq(qts, HEART + 0x10000, UINT64_MAX);
    wide = qtest_readq(qts, HEART + 0x10030);
    g_assert_cmphex(wide, ==, 0);
    count = qtest_readq(qts, HEART + 0x20000);
    qtest_writeq(qts, HEART + 0x30000, 0x1234567800abcde1ULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30000), ==, 0xabcde1);
    qtest_writeq(qts, HEART + 0x30000, 0xfedcba9800abcde1ULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30000), ==, 0xabcde1);
    qtest_writeq(qts, HEART + 0x30000, 0xa5a5a5a501573ecaULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30000), ==, 0x573eca);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, wide);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10038), ==, wide);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x20000), ==, count);

    qtest_writel(qts, HEART + 0x50, 0);
    qtest_writel(qts, HEART + 0xb0, 0);
    qtest_writel(qts, HEART + 0x30000, 0);
    qtest_writeq(qts, HEART + 0x48, 0);
    qtest_writeq(qts, HEART + 0x60, 0);
    qtest_writeq(qts, HEART + 0x30008, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x50), ==,
                    flow_b ^ 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb0), ==,
                    0x123456789abca55aULL);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30000), ==, 0x573eca);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x50), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x58), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEART + 0xb0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x30000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x48), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x60), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30008), ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x28), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x50), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x58), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x30000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    qtest_quit(qts);
}

static void test_heart_divider_onewire_time(void)
{
    QTestState *qts = octane_start();
    const uint64_t command = (500u << 10) | (65u << 2);

    qtest_writeq(qts, HEART + 0xb8, command);
    qtest_writeq(qts, HEART + 0xb0, 0x123456789abc7f7fULL);
    qtest_clock_step(qts, 564000);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb8) & 2, ==, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readq(qts, HEART + 0xb8) & 2, ==, 2);
    qtest_quit(qts);
}

static void test_low_ram(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();
    const uint64_t offsets[] = { 0, 0x1234, 0x1fff };
    uint64_t wide;
    uint32_t word;
    uint16_t half;
    uint8_t byte;

    for (uint64_t addr = 0; addr <= 0x3ff8; addr += 0x400) {
        g_assert_cmpuint(qtest_readb_tx(qts, addr, &byte), ==, MEMTX_OK);
        g_assert_cmphex(byte, ==, 0);
        g_assert_cmpuint(qtest_readw_tx(qts, addr, &half), ==, MEMTX_OK);
        g_assert_cmphex(half, ==, 0);
        g_assert_cmpuint(qtest_readl_tx(qts, addr, &word), ==, MEMTX_OK);
        g_assert_cmphex(word, ==, 0);
        g_assert_cmpuint(qtest_readq_tx(qts, addr, &wide), ==, MEMTX_OK);
        g_assert_cmphex(wide, ==, 0);
    }
    g_assert_cmpuint(qtest_readw_tx(qts, MPCONF + 0x14, &half), ==,
                     MEMTX_OK);
    g_assert_cmphex(half, ==, 0);
    g_assert_cmpuint(qtest_readb_tx(qts, MPCONF + 0x14, &byte), ==,
                     MEMTX_OK);
    g_assert_cmphex(byte, ==, 0);
    g_assert_cmpuint(qtest_readl_tx(qts, MPCONF + 0x14, &word), ==,
                     MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(qtest_readq_tx(qts, MPCONF + 0x10, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(qtest_readq_tx(qts, 0x1ff8, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    g_assert_cmpuint(qtest_readl_tx(qts, 0x1ffc, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0);
    g_assert_cmpuint(qtest_readb_tx(qts, 0x1fff, &byte), ==, MEMTX_OK);
    g_assert_cmphex(byte, ==, 0);

    g_assert_cmpuint(qtest_writeq_tx(qts, 0, 0x0123456789abcdefULL), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, 0), ==, 0x01);
    g_assert_cmphex(qtest_readw(qts, 2), ==, 0x4567);
    g_assert_cmphex(qtest_readl(qts, 4), ==, 0x89abcdef);
    g_assert_cmpuint(qtest_writel_tx(qts, 0x400, 0xa1b2c3d4), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readw(qts, 0x400), ==, 0xa1b2);
    g_assert_cmphex(qtest_readb(qts, 0x403), ==, 0xd4);
    g_assert_cmpuint(qtest_writew_tx(qts, MPCONF + 0x14, 0x5a6b), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, MPCONF + 0x10), ==,
                    0x000000005a6b0000ULL);
    g_assert_cmphex(qtest_readw(qts, ATTR0 + MPCONF + 0x14), ==, 0x5a6b);
    g_assert_cmpuint(qtest_writeq_tx(qts, 0x1ff8,
                                     0x1020304050607080ULL), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_writeb_tx(qts, 0x1fff, 0x9a), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, 0x1ff8), ==, 0x102030405060709aULL);

    g_assert_cmpuint(qtest_readq_tx(qts, LOW_RAM_HALF, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint64_t lower = offsets[i];
        uint64_t upper = LOW_RAM_HALF + lower;
        uint8_t lower_value = 0x31 + i;
        uint8_t upper_value = 0xa1 + i;

        g_assert_cmpuint(qtest_writeb_tx(qts, lower, lower_value), ==,
                         MEMTX_OK);
        g_assert_cmpuint(qtest_writeb_tx(qts, upper, upper_value), ==,
                         MEMTX_OK);
        g_assert_cmpuint(qtest_readb_tx(qts, lower, &byte), ==, MEMTX_OK);
        g_assert_cmphex(byte, ==, lower_value);
        g_assert_cmpuint(qtest_readb_tx(qts, upper, &byte), ==, MEMTX_OK);
        g_assert_cmphex(byte, ==, upper_value);
    }
    g_assert_cmpuint(qtest_writeq_tx(qts, LOW_RAM_SIZE - 8,
                                     0x1020304050607080ULL), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq_tx(qts, LOW_RAM_SIZE - 8, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0x1020304050607080ULL);
    g_assert_cmpuint(qtest_readb_tx(qts, 0x3fff, &byte), ==, MEMTX_OK);
    g_assert_cmphex(byte, ==, 0x80);
    g_assert_cmpuint(qtest_readb_tx(qts, 0x4000, &byte), !=, MEMTX_OK);
    g_assert_cmpuint(qtest_writeb_tx(qts, 0x4000, 0xff), !=, MEMTX_OK);
    g_assert_cmpuint(qtest_readb_tx(qts, ATTR0 + LOW_RAM_HALF, &byte), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readb_tx(qts, HIGH_GUARD, &byte), !=, MEMTX_OK);
    g_assert_cmpuint(qtest_writeb_tx(qts, HIGH_GUARD, 0xff), !=, MEMTX_OK);
    g_assert_cmpuint(qtest_readb_tx(qts, HIGH_GUARD + 0x3fff, &byte), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writeb_tx(qts, HIGH_GUARD + 0x3fff, 0xff), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readw_tx(qts, RAM + 0x614, &half), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writeq_tx(qts, HIGH_RAM,
                                     0x1122334455667788ULL), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq_tx(qts, HIGH_RAM, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readw(qts, MPCONF + 0x14), ==, 0x5a6b);

    for (uint64_t addr = 0; addr < LOW_RAM_SIZE; addr += 8) {
        g_assert_cmpuint(qtest_writeq_tx(qts, addr, UINT64_MAX), ==,
                         MEMTX_OK);
    }
    qtest_system_reset(qts);
    for (uint64_t addr = 0; addr < LOW_RAM_SIZE; addr += 8) {
        g_assert_cmpuint(qtest_readq_tx(qts, addr, &wide), ==, MEMTX_OK);
        g_assert_cmphex(wide, ==, 0);
    }
    g_assert_cmphex(qtest_readb(qts, 0x3fff), ==, 0);
    qtest_quit(qts);
}

static void test_heart_identity(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();
    uint8_t byte;
    uint16_t half;
    uint32_t word;
    uint64_t wide;

    octane_heart_identity_check(qts, octane2);
    for (unsigned offset = 0; offset < 8; offset++) {
        uint64_t addr = HEART_WIDGET + offset;

        g_assert_cmpuint(qtest_readb_tx(qts, addr, &byte), ==, MEMTX_ERROR);
        g_assert_cmpuint(qtest_readw_tx(qts, addr, &half), ==, MEMTX_ERROR);
        if (offset != 4) {
            g_assert_cmpuint(qtest_readl_tx(qts, addr, &word), ==,
                             MEMTX_ERROR);
        }
        if (offset != 0) {
            g_assert_cmpuint(qtest_readq_tx(qts, addr, &wide), ==,
                             MEMTX_ERROR);
        }
        g_assert_cmpuint(qtest_writeb_tx(qts, addr, 0), ==, MEMTX_ERROR);
        g_assert_cmpuint(qtest_writew_tx(qts, addr, 0), ==, MEMTX_ERROR);
        g_assert_cmpuint(qtest_writel_tx(qts, addr, 0), ==, MEMTX_ERROR);
        g_assert_cmpuint(qtest_writeq_tx(qts, addr, 0), ==, MEMTX_ERROR);
    }
    octane_heart_identity_check(qts, octane2);
    qtest_system_reset(qts);
    octane_heart_identity_check(qts, octane2);
    qtest_quit(qts);
}

static void test_octane2_heart_widget_status(void)
{
    QTestState *qts = octane2_start_args("");
    uint64_t wide;
    uint32_t word;

    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET + 0x08, &wide), ==,
                     MEMTX_OK);
    g_assert_cmphex(wide, ==, 0x60);
    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 0x08, &word), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, HEART_WIDGET + 0x0c, &word), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writeq_tx(qts, HEART_WIDGET + 0x08, 0), !=,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_writel_tx(qts, HEART_WIDGET + 0x0c, 0), !=,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, HEART_WIDGET + 0x08), ==, 0x60);
    qtest_quit(qts);

    qts = octane_start();
    g_assert_cmpuint(qtest_readq_tx(qts, HEART_WIDGET + 0x08, &wide), !=,
                     MEMTX_OK);
    qtest_quit(qts);
}

static void test_flash_and_pci(void)
{
    QTestState *qts = octane_start();
    uint32_t value;
    uint64_t last;
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_assert_nonnull(strstr(mtree, "sgi-bridge.flash-read"));

    g_assert_cmpuint(qtest_readl_tx(qts, FLASH, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x12345678);
    g_assert_cmphex(qtest_readb(qts, FLASH), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, FLASH + 1), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, FLASH + 2), ==, 0x56);
    g_assert_cmphex(qtest_readb(qts, FLASH + 3), ==, 0x78);
    g_assert_cmphex(qtest_readb(qts, FLASH + 4), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH + 0x1fffff), ==, 0xff);
    g_assert_cmphex(qtest_readw(qts, FLASH + 1), ==, 0x3456);
    g_assert_cmphex(qtest_readl(qts, FLASH + 1), ==, 0x345678ff);
    g_assert_cmphex(qtest_readq(qts, FLASH + 1), ==,
                    0x345678ffffffffffULL);
    /* QEMU splits this unaligned read across the two flash selects. */
    g_assert_cmpuint(qtest_readq_tx(qts, FLASH + FLASH_SELECT_SIZE - 7,
                                    &last), ==, MEMTX_OK);
    g_assert_cmphex(last, ==, UINT64_MAX);
    /* Bridge drops writes while flash write enable is clear. */
    g_assert_cmpuint(qtest_writel_tx(qts, FLASH, 0), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readl_tx(qts, FLASH, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0x12345678);
    g_assert_cmphex(qtest_readb(qts, FLASH), ==, 0x12);
    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    qtest_writeb(qts, FLASH, 0);
    qtest_writew(qts, FLASH, 0);
    g_assert_cmpuint(qtest_writel_tx(qts, FLASH, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x12345678);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x12345678);
    g_clear_pointer(&mtree, g_free);
    mtree = qtest_hmp(qts, "info mtree -f");
    g_assert_nonnull(strstr(mtree, "sgi-bridge.flash-read"));
    g_assert_cmphex(qtest_readl(qts, CFG(0)), ==, 0x10201077);
    g_assert_cmphex(qtest_readl(qts, CFG(1)), ==, 0x10201077);
    g_assert_cmphex(qtest_readl(qts, CFG(2)), ==, 0x000310a9);
    g_assert_cmpuint(qtest_readl_tx(qts, CFG(3), &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, RAD_PCI_ID);
    g_assert_cmpuint(qtest_readl_tx(qts, OCTANE_LONG_BRIDGE + 4, &value), ==,
                     MEMTX_OK);
    /* An empty one-wire bus has no device to pull a reset slot low. */
    qtest_writel(qts, BRIDGE_NIC, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, BRIDGE_NIC) & 3, ==, 2);
    qtest_quit(qts);
}

static void test_alternate_flash(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();
    uint16_t word;
    uint32_t primary;

    g_assert_cmpuint(qtest_readw_tx(qts, FLASH + FLASH_SELECT_SIZE, &word),
                     ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xffff);
    g_assert_cmpuint(qtest_readl_tx(qts, FLASH, &primary), ==, MEMTX_OK);
    g_assert_cmphex(primary, ==, 0x12345678);

    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    g_assert_cmpuint(qtest_writew_tx(qts, FLASH + FLASH_SELECT_SIZE, 0x1234),
                     ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readw_tx(qts, FLASH + FLASH_SELECT_SIZE, &word),
                     ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0xffff);
    g_assert_cmpuint(qtest_readl_tx(qts, FLASH, &primary), ==, MEMTX_OK);
    g_assert_cmphex(primary, ==, 0x12345678);
    qtest_quit(qts);
}

static QTestState *octane_start_sharp(const char *path, const char *marker,
                                      unsigned select, unsigned strap)
{
    g_autofree char *image = g_shell_quote(path);
    g_autofree char *failure = g_shell_quote(marker);

    QTestState *qts = qtest_initf(
        "-M octane,flash-profile=sharp-lh28f800su,"
        "flash-select=%u,flash-strap=%u,flash-drive=flash0,"
        "flash-marker=%s -m 128M -cpu R10000 "
        "-drive if=none,id=flash0,file=%s,format=raw",
        select, strap, failure, image);

    /* The explicit RP rise has a 400 ns recovery interval. */
    qtest_clock_step(qts, 401);
    return qts;
}

static void test_sharp_flash(gconstpointer data)
{
    unsigned select = GPOINTER_TO_UINT(data) >> 1;
    unsigned strap = GPOINTER_TO_UINT(data) & 1;
    bool lower = select == strap;
    uint64_t base = FLASH + (lower ? 0 : FLASH_SELECT_SIZE);
    uint64_t absent = FLASH + (lower ? FLASH_SELECT_SIZE : 0);
    g_autofree char *dir = g_dir_make_tmp("octane-sharp-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;
    uint16_t word;
    uint64_t wide;
    uint8_t stored[2];

    memset(image, 0xff, 1 * MiB);
    image[0] = 0x12;
    image[1] = 0x34;
    image[64 * KiB] = 0xab;
    image[64 * KiB + 1] = 0xcd;
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = octane_start_sharp(path, marker, select, strap);
    g_assert_cmpuint(qtest_readw_tx(qts, absent, &word), ==,
                     MEMTX_DECODE_ERROR);
    g_assert_cmpuint(qtest_readw_tx(qts, base, &word), ==, MEMTX_OK);
    g_assert_cmphex(word, ==, 0x1234);
    g_assert_cmphex(qtest_readb(qts, base), ==, 0x12);
    g_assert_cmphex(qtest_readl(qts, base), ==, 0x1234ffff);
    g_assert_cmphex(qtest_readq(qts, base), ==,
                    UINT64_C(0x1234ffffffffffff));
    g_assert_cmpuint(qtest_readq_tx(qts, FLASH + FLASH_SELECT_SIZE - 4,
                                    &wide), ==, MEMTX_DECODE_ERROR);
    g_assert_cmphex(qtest_readw(qts, base + 1 * MiB), ==, 0x1234);
    g_assert_cmpuint(qtest_writew_tx(qts, base, 0x90), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readw(qts, base), ==, 0x1234);
    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    g_assert_cmpuint(qtest_writel_tx(qts, base, 0x90), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_writew_tx(qts, base + 1, 0x90), ==, MEMTX_ERROR);
    qtest_writew(qts, base + 1 * MiB, 0x90);
    g_assert_cmphex(qtest_readw(qts, base), ==, 0x00b0);
    g_assert_cmphex(qtest_readw(qts, base + 2), ==, 0x66a8);
    g_assert_cmphex(qtest_readb(qts, base), ==, 0);
    g_assert_cmphex(qtest_readb(qts, base + 1), ==, 0xb0);
    g_assert_cmphex(qtest_readl(qts, base), ==, 0x00b066a8);
    g_assert_cmpuint(qtest_readq_tx(qts, base, &wide), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readw(qts, base + 1 * MiB), ==, 0x00b0);
    qtest_writew(qts, base, 0x70);
    g_assert_cmphex(qtest_readw(qts, base), ==, 0x0080);
    g_assert_cmphex(qtest_readb(qts, base + 1), ==, 0x80);
    g_assert_cmphex(qtest_readl(qts, base), ==, 0x00800080);
    g_assert_cmphex(qtest_readq(qts, base + 1 * MiB), ==,
                    UINT64_C(0x0080008000800080));
    qtest_writew(qts, base + 1 * MiB, 0x71);
    g_assert_cmphex(qtest_readw(qts, base + 2), ==, 0x0080);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0x0086);
    g_assert_cmphex(qtest_readl(qts, base + 2), ==, 0x00800086);

    qtest_writew(qts, base, 0xff);
    qtest_writew(qts, base + 1 * MiB + 0x20, 0x40);
    qtest_writew(qts, base + 1 * MiB + 0x20, 0x1234);
    g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0);
    qtest_clock_step(qts, 8000);
    g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0x80);
    qtest_writew(qts, base, 0xff);
    g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, base + 1 * MiB + 0x20), ==, 0x1234);
    qtest_writew(qts, base, 0x90);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readw(qts, base), ==, 0x00b0);
    g_assert_cmpuint(qtest_writew_tx(qts, base, 0xff), ==, MEMTX_ERROR);
    qtest_quit(qts);

    qts = octane_start_sharp(path, marker, select, strap);
    g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0x1234);
    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    qtest_writew(qts, base + 64 * KiB, 0x20);
    qtest_writew(qts, base + 64 * KiB, 0xd0);
    qtest_clock_step(qts, 700000000);
    g_assert_cmphex(qtest_readw(qts, base), ==, 0x80);
    qtest_writew(qts, base, 0xff);
    g_assert_cmphex(qtest_readw(qts, base + 64 * KiB), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, base), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0x1234);
    qtest_quit(qts);
    qts = octane_start_sharp(path, marker, select, strap);
    g_assert_cmphex(qtest_readw(qts, base + 64 * KiB), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, base + 0x20), ==, 0x1234);
    qtest_quit(qts);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    {
        FILE *file = fopen(path, "rb");

        g_assert_nonnull(file);
        g_assert_cmpint(fseek(file, 0x20, SEEK_SET), ==, 0);
        g_assert_cmpuint(fread(stored, 1, sizeof(stored), file), ==,
                         sizeof(stored));
        fclose(file);
    }
    g_assert_cmphex(stored[0], ==, 0x12);
    g_assert_cmphex(stored[1], ==, 0x34);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void sharp_assert_read_view(QTestState *qts, bool enabled)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *expected = g_strdup_printf(
        ", %s): sgi-bridge.flash-read", enabled ? "romd" : "i/o");

    g_assert_nonnull(strstr(mtree, expected));
}

static void test_sharp_flash_view_transitions(void)
{
    g_autofree char *dir = g_dir_make_tmp("sharp-view-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;

    memset(image, 0xff, 1 * MiB);
    image[64 * KiB] = 0xab;
    image[64 * KiB + 1] = 0xcd;
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = octane_start_sharp(path, marker, 0, 0);
    sharp_assert_read_view(qts, true);
    g_assert_cmpuint(qtest_writew_tx(qts, FLASH, 0x90), ==, MEMTX_ERROR);
    sharp_assert_read_view(qts, true);
    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    qtest_writew(qts, FLASH, 0x90);
    sharp_assert_read_view(qts, false);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x00b066a8);
    qtest_writew(qts, FLASH, 0x70);
    sharp_assert_read_view(qts, false);
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0x80);
    qtest_writew(qts, FLASH, 0x71);
    sharp_assert_read_view(qts, false);
    g_assert_cmphex(qtest_readl(qts, FLASH + 2), ==, 0x00800086);
    qtest_writew(qts, FLASH, 0xff);
    sharp_assert_read_view(qts, true);
    qtest_writew(qts, FLASH + 0x20, 0x40);
    qtest_writew(qts, FLASH + 0x20, 0x1234);
    sharp_assert_read_view(qts, false);
    qtest_clock_step(qts, 8000);
    sharp_assert_read_view(qts, false);
    qtest_writew(qts, FLASH, 0xff);
    sharp_assert_read_view(qts, true);
    g_assert_cmphex(qtest_readw(qts, FLASH + 0x20), ==, 0x1234);
    qtest_writew(qts, FLASH + 64 * KiB, 0x20);
    qtest_writew(qts, FLASH + 64 * KiB, 0xd0);
    sharp_assert_read_view(qts, false);
    qtest_writew(qts, FLASH, 0xb0);
    qtest_writew(qts, FLASH, 0xff);
    sharp_assert_read_view(qts, true);
    g_assert_cmphex(qtest_readw(qts, FLASH + 64 * KiB), ==, 0xabcd);
    qtest_writew(qts, FLASH, 0xd0);
    sharp_assert_read_view(qts, false);
    qtest_clock_step(qts, 700000000);
    qtest_writew(qts, FLASH, 0xff);
    sharp_assert_read_view(qts, true);
    g_assert_cmphex(qtest_readw(qts, FLASH + 64 * KiB), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, FLASH + 0x20), ==, 0x1234);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_unrealize_access(void)
{
    g_autofree char *dir = g_dir_make_tmp("sharp-unrealize-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;
    MemTxResult setup, data;
    uint16_t value;

    memset(image, 0xff, 1 * MiB);
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = octane_start_sharp(path, marker, 0, 0);
    sharp_assert_read_view(qts, true);
    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/sharp-flash',"
        "'property':'realized','value':false}}");
    sharp_assert_read_view(qts, false);
    setup = qtest_writew_tx(qts, FLASH + 0x20, 0x40);
    data = qtest_writew_tx(qts, FLASH + 0x20, 0x1234);
    g_assert_cmpuint(setup, ==, MEMTX_ERROR);
    g_assert_cmpuint(data, ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readw_tx(qts, FLASH, &value), ==, MEMTX_ERROR);
    qtest_set_irq_in(qts, "/machine/sharp-flash", "pins", 2, 0);
    qtest_set_irq_in(qts, "/machine/sharp-flash", "pins", 2, 1);
    qtest_clock_step(qts, 700000000);
    sharp_assert_read_view(qts, false);
    g_assert_cmpuint(qtest_writew_tx(qts, FLASH, 0xff), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readw_tx(qts, FLASH, &value), ==, MEMTX_ERROR);
    qtest_quit(qts);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_initial_rp(void)
{
    g_autofree char *dir = g_dir_make_tmp("sharp-initial-rp-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;
    uint16_t value;

    memset(image, 0xab, 1 * MiB);
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = qtest_initf(
        "-M octane,flash-profile=sharp-lh28f800su,flash-select=0,"
        "flash-strap=0,flash-drive=flash0,flash-marker=%s "
        "-m 128M -cpu R10000 -drive if=none,id=flash0,file=%s,format=raw",
        marker, path);
    /* Board power-up does not synthesize an RP transition at time zero. */
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0xabab);
    sharp_assert_read_view(qts, true);
    qtest_set_irq_in(qts, "/machine/sharp-flash", "pins", 2, 0);
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0xffff);
    sharp_assert_read_view(qts, false);
    qtest_set_irq_in(qts, "/machine/sharp-flash", "pins", 2, 1);
    qtest_clock_step(qts, 399);
    sharp_assert_read_view(qts, false);
    g_assert_cmpuint(qtest_readw_tx(qts, FLASH, &value), ==, MEMTX_ERROR);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0xabab);
    sharp_assert_read_view(qts, true);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_execute(gconstpointer data)
{
    bool icount = GPOINTER_TO_UINT(data);
    static const uint32_t code[] = {
        0x400a6000, 0x354a0080, 0x408a6000, /* Status.KX = 1 */
        0, 0, 0, 0,
        0x3c089600, 0x0008403c,             /* t0 = 0x9600000000000000 */
        0x3c092000, 0x35294000, 0x01094025, /* t0 |= RAM + 0x4000 */
        0x340a1357, 0xad0a0000,             /* RAM[0x4000] = 0x1357 */
        0x65090008, 0x01200008, 0,          /* Loop in RAM during programming */
    };
    g_autofree char *dir = g_dir_make_tmp("sharp-execute-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;

    memset(image, 0xff, 1 * MiB);
    for (unsigned i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + 4 * i, code[i]);
    }
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = qtest_initf(
        "-M %s,flash-profile=sharp-lh28f800su,flash-select=0,"
        "flash-strap=0,flash-drive=flash0,flash-marker=%s "
        "-m %uM -cpu %s -drive if=none,id=flash0,file=%s,format=raw "
        "-accel tcg,thread=single -S %s", icount ? "octane2" : "octane",
        marker, icount ? 256 : 128,
        icount ? "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900,"
                 "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152,"
                 "r12000-scache-block-words=32" : "R10000", path,
        icount ? "-icount shift=3,align=off,sleep=off -rtc clock=host" : "");
    qtest_writel(qts, RAM + 0x4008, 0x1000ffff);
    qtest_writel(qts, RAM + 0x400c, 0);
    for (unsigned pass = 0; pass < 2; pass++) {
        uint32_t result = 0;
        int64_t deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

        qtest_writel(qts, RAM + 0x4000, 0);
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        do {
            result = qtest_readl(qts, RAM + 0x4000);
            if (result) {
                break;
            }
            g_usleep(1000);
        } while (g_get_monotonic_time() < deadline);
        qtest_qmp_assert_success(qts, "{'execute':'stop'}");
        g_assert_cmphex(result, ==, pass ? 0x0357 : 0x1357);
        if (!pass) {
            qtest_writel(qts, BRIDGE_REG(0x020),
                         qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
            qtest_writew(qts, FLASH, 0x90);
            g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x00b066a8);
            qtest_writew(qts, FLASH, 0xff);
            g_assert_cmphex(qtest_readl(qts, FLASH), ==, code[0]);
            qtest_writew(qts, FLASH + 50, 0x40);
            qtest_writew(qts, FLASH + 50, 0x0357);
            qtest_qmp_assert_success(qts, "{'execute':'cont'}");
            deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
            while (qtest_readw(qts, FLASH) != 0x80) {
                g_assert_cmpint(g_get_monotonic_time(), <, deadline);
                g_usleep(1000);
            }
            qtest_qmp_assert_success(qts, "{'execute':'stop'}");
            g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x00800080);
            qtest_writew(qts, FLASH, 0xff);
            g_assert_cmphex(qtest_readl(qts, FLASH + 48), ==, 0x340a0357);
            qtest_system_reset(qts);
        }
    }
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void sharp_cpu_wait_result(QTestState *qts, unsigned sequence,
                                  uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    uint32_t result, observed;

    qtest_writel(qts, RAM + 0x5000, 0);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    do {
        observed = qtest_readl(qts, RAM + 0x5004);
        result = qtest_readl(qts, RAM + 0x5000);
        if (observed == sequence && result == expected) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(result, ==, expected);
    g_assert_cmpuint(observed, ==, sequence);
}

static void sharp_cpu_refetch(QTestState *qts, bool repeat, unsigned sequence,
                              uint32_t expected)
{
    qtest_writel(qts, RAM + 0x5020, repeat ? 0xbfd10000 : 0xbfc10000);
    /* Jump to the RAM return code. */
    qtest_writel(qts, RAM + 0x4010, 0x08001010);
    sharp_cpu_wait_result(qts, sequence, expected);
}

static unsigned sharp_assert_cached_execution(QTestState *qts)
{
    g_autofree char *jit = qtest_hmp(qts, "info jit");
    const char *size_line = strstr(jit, "TB avg target size");
    const char *hash_line = strstr(jit, "TB hash buckets");
    const char *flush_line = strstr(jit, "TB flush count");
    size_t average, maximum, used, total;
    unsigned flushes;

    g_assert_nonnull(size_line);
    g_assert_nonnull(hash_line);
    g_assert_nonnull(flush_line);
    g_assert_cmpint(sscanf(size_line, "TB avg target size %zu max=%zu",
                          &average, &maximum), ==, 2);
    g_assert_cmpint(sscanf(hash_line, "TB hash buckets %zu/%zu",
                          &used, &total), ==, 2);
    g_assert_cmpint(sscanf(flush_line, "TB flush count %u", &flushes), ==, 1);
    g_test_message("array fetch: max TB %zu bytes, buckets %zu/%zu, flushes %u",
                   maximum, used, total, flushes);
    /* The RAM loop, return code and reset jump are at most 16 bytes. */
    g_assert_cmpuint(maximum, >, 16);
    g_assert_cmpuint(used, >, 0);
    return flushes;
}

static void sharp_cpu_complete_flash(QTestState *qts, unsigned timeout)
{
    int64_t started = g_get_monotonic_time();
    int64_t deadline = started + timeout * G_TIME_SPAN_SECOND;
    uint64_t count = qtest_readq(qts, HEART + 0x20000);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    while (qtest_readw(qts, FLASH) != 0x80) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(qts, "info registers");

            g_test_message("flash wait timed out: HEART count %" PRIu64
                           " -> %" PRIu64, count,
                           qtest_readq(qts, HEART + 0x20000));
            g_test_message("%s", registers);
            g_assert_not_reached();
        }
        g_usleep(1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_test_message("flash wait: HEART advanced %" PRIu64
                   " counts in %" PRIi64 " us",
                   qtest_readq(qts, HEART + 0x20000) - count,
                   g_get_monotonic_time() - started);
    qtest_writew(qts, FLASH, 0xff);
}

static void test_sharp_flash_cached_execute(gconstpointer data)
{
    bool icount = GPOINTER_TO_UINT(data);
    static const uint32_t code[] = {
        0x400a6000, 0x354a0080, 0x408a6000, /* Status.KX = 1 */
        0, 0, 0, 0,
        0x3c089600, 0x0008403c,
        0x3c092000, 0x35295000, 0x01094025, /* t0 = RAM + 0x5000, XKPHYS */
        0x340a1357,                         /* t2 = result */
        0x3c0c1000, 0x358cfffd, 0xad0cf010, /* Restore the RAM loop branch. */
        0x8d0d0004, 0x25ad0001,             /* t5 = next result sequence */
        0x6509f008, 0x01200008, 0,          /* Return to the RAM loop. */
    };
    g_autofree char *dir = g_dir_make_tmp("sharp-cached-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;
    unsigned flushes;

    memset(image, 0xff, 1 * MiB);
    stl_be_p(image, 0x3c0bbfc1);
    stl_be_p(image + 4, 0x01600008);
    stl_be_p(image + 8, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + 64 * KiB + 4 * i, code[i]);
        stl_be_p(image + 0x380 + 4 * i, i == 12 ? 0x340abeef : code[i]);
    }
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = qtest_initf(
        "-M %s,flash-profile=sharp-lh28f800su,flash-select=0,"
        "flash-strap=0,flash-drive=flash0,flash-marker=%s "
        "-m %uM -cpu %s -drive if=none,id=flash0,file=%s,format=raw "
        "-accel tcg,thread=single -S %s", icount ? "octane2" : "octane",
        marker, icount ? 256 : 128,
        icount ? "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900,"
                 "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152,"
                 "r12000-scache-block-words=32" : "R10000", path,
        icount ? "-icount shift=3,align=off,sleep=off -rtc clock=host" : "");
    /* Result data has no translated code on its page. */
    qtest_writel(qts, RAM + 0x5004, 0);
    qtest_writel(qts, RAM + 0x4008, 0xad0a0000);
    qtest_writel(qts, RAM + 0x400c, 0xad0d0004);
    qtest_writel(qts, RAM + 0x4010, 0x1000fffd);
    qtest_writel(qts, RAM + 0x4014, 0);
    qtest_writel(qts, RAM + 0x4040, 0x8d0b0020);
    qtest_writel(qts, RAM + 0x4044, 0x01600008);
    qtest_writel(qts, RAM + 0x4048, 0);
    sharp_cpu_wait_result(qts, 1, 0x1357);
    sharp_cpu_refetch(qts, true, 2, 0x1357);
    flushes = sharp_assert_cached_execution(qts);

    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    qtest_writew(qts, FLASH + 64 * KiB + 50, 0x40);
    qtest_writew(qts, FLASH + 64 * KiB + 50, 0x0357);
    sharp_cpu_complete_flash(qts, 5);
    g_assert_cmphex(qtest_readl(qts, FLASH + 64 * KiB + 48), ==, 0x340a0357);
    sharp_cpu_refetch(qts, false, 3, 0x0357);
    sharp_cpu_refetch(qts, true, 4, 0x0357);
    g_assert_cmpuint(sharp_assert_cached_execution(qts), ==, flushes);

    qtest_writew(qts, FLASH + 1 * MiB + 64 * KiB, 0x20);
    qtest_writew(qts, FLASH + 1 * MiB + 64 * KiB, 0xd0);
    sharp_cpu_complete_flash(qts, 30);
    g_assert_cmphex(qtest_readl(qts, FLASH + 64 * KiB), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x3c0bbfc1);
    /* Erased instructions trap to the unchanged block-zero handler. */
    sharp_cpu_refetch(qts, false, 5, 0xbeef);
    sharp_cpu_refetch(qts, true, 6, 0xbeef);
    g_assert_cmpuint(sharp_assert_cached_execution(qts), ==, flushes);
    qtest_quit(qts);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_migration_rejected(void)
{
    g_autofree char *dir = g_dir_make_tmp("octane-sharp-migrate-XXXXXX",
                                         NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc0(1 * MiB);
    QTestState *qts;
    int64_t deadline;

    memset(image, 0xff, 1 * MiB);
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = octane_start_sharp(path, marker, 0, 0);
    qtest_qmp_assert_success(qts,
        "{'execute':'migrate','arguments':{'uri':'exec:cat >/dev/null'}}");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    for (;;) {
        QDict *result = qtest_qmp_assert_success_ref(qts,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(result, "status");
        bool failed = !strcmp(status, "failed");

        if (failed) {
            g_assert_cmpstr(qdict_get_str(result, "error-desc"), ==,
                            "pre-save failed: lh28f800su");
        }
        qobject_unref(result);
        if (failed) {
            break;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void sharp_flash_restore_attempt(void)
{
    g_autofree char *dir = g_dir_make_tmp("sharp-restore-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *state = g_build_filename(dir, "state", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    g_autofree char *uri = g_strdup_printf("exec:cat >%s", state);
    QTestState *from;
    QTestState *to;
    g_autofree char *stored = NULL;
    gsize size;

    memset(image, 0xab, 1 * MiB);
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    from = qtest_init("-M none -cpu R10000 "
                      "-device lh28f800su-test-device,id=fixture");
    qtest_clock_step(from, 401);
    qtest_writew(from, 0x50000000, 0x20);
    qtest_writew(from, 0x50000000, 0xd0);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_initf(
        "-M none -cpu R10000 -device lh28f800su-test-device,id=fixture -incoming defer "
        "-drive if=none,id=flash0,file=%s,format=raw "
        "-global lh28f800su.drive=flash0 "
        "-global lh28f800su.failure-marker=%s", path, marker);
    g_free(g_steal_pointer(&uri));
    uri = g_strdup_printf("exec:cat %s", state);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    g_assert_true(g_file_get_contents(path, &stored, &size, NULL));
    g_assert_cmpuint(size, ==, 1 * MiB);
    g_assert_cmpmem(stored, size, image, 1 * MiB);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_unlink(state), ==, 0);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_restore_rejected(void)
{
    if (g_test_subprocess()) {
        sharp_flash_restore_attempt();
        return;
    }
    g_test_trap_subprocess(NULL, 30 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*restore with persistent backing "
                             "is unsupported*");
}

static void test_sharp_flash_storage_rejections(void)
{
    g_autofree char *dir = g_dir_make_tmp("octane-sharp-storage-XXXXXX",
                                         NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc0(1 * MiB);
    g_autofree char *machine = g_strdup_printf(
        "octane,flash-profile=sharp-lh28f800su,flash-select=0,"
        "flash-strap=0,flash-drive=flash0,flash-marker=%s", marker);
    g_autofree char *drive = g_strdup_printf(
        "if=none,id=flash0,file=%s,format=raw", path);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-m", "128M", "-cpu",
        "R10000", "-drive", drive, "-display", "none", "-qmp", "stdio",
        NULL
    };
    QTestState *first;

    memset(image, 0xff, 1 * MiB);
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));
    assert_launch_rejected(args, "invalid marker");
    g_assert_cmpint(g_unlink(marker), ==, 0);
    g_assert_cmpint(truncate(path, 1 * MiB - 512), ==, 0);
    assert_launch_rejected(args, "exactly 1 MiB");
    g_assert_cmpint(truncate(path, 1 * MiB), ==, 0);

    first = octane_start_sharp(path, marker, 0, 0);
    assert_launch_rejected(args, "Failed to get");
    qtest_quit(first);
    {
        const char *options[] = {
            "cache=unsafe", "file.cache.no-flush=on", "file.locking=off",
            NULL
        };
        unsigned i;

        for (i = 0; options[i]; i++) {
            g_autofree char *unsafe = g_strdup_printf("%s,%s", drive,
                                                       options[i]);
            const char *bad[] = {
                qtest_qemu_binary(NULL), "-M", machine, "-m", "128M",
                "-cpu", "R10000", "-drive", unsafe, "-display", "none",
                "-qmp", "stdio", NULL
            };

            assert_launch_rejected(bad, "persistent flash requires");
        }
    }
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_raw_subregion(gconstpointer data)
{
    bool offset = GPOINTER_TO_UINT(data);
    g_autofree char *dir = g_dir_make_tmp("sharp-subregion-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(2 * MiB);
    g_autofree char *stored = NULL;
    g_autofree char *machine = g_strdup_printf(
        "octane,flash-profile=sharp-lh28f800su,flash-select=0,"
        "flash-strap=0,flash-drive=flash0,flash-marker=%s", marker);
    g_autofree char *drive = g_strdup_printf(
        "if=none,id=flash0,file=%s,format=raw,%s=1048576",
        path, offset ? "offset" : "size");
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-m", "128M", "-cpu",
        "R10000", "-drive", drive, "-display", "none", "-qmp", "stdio",
        NULL
    };
    gsize size;

    memset(image, 0xab, 2 * MiB);
    g_assert_true(g_file_set_contents(path, image, 2 * MiB, NULL));
    assert_launch_rejected(args, "persistent flash");
    g_assert_true(g_file_get_contents(path, &stored, &size, NULL));
    g_assert_cmpmem(stored, size, image, 2 * MiB);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_backend_error(gconstpointer data)
{
    const char *event = data;
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *marker = NULL;
    g_autofree char *config = NULL;
    g_autofree char *image = NULL;

    if ((!strncmp(event, "dirsync_", 8) ||
         !strcmp(event, "marker_remove")) &&
        !g_getenv("QTEST_SHARP_STORAGE_HELPER")) {
        g_test_skip("storage syscall injection requires "
                    "the Linux preload helper");
        return;
    }
    if (g_test_subprocess()) {
        g_autofree char *drive = g_strdup_printf(
            "blkdebug:%s:%s", g_getenv("SHARP_DEBUG_CONFIG"),
            g_getenv("SHARP_WORKING_IMAGE"));
        g_autofree char *quoted_drive = g_shell_quote(drive);
        g_autofree char *quoted_marker =
            g_shell_quote(g_getenv("SHARP_FAILURE_MARKER"));
        g_autofree char *args = g_strdup_printf(
            "-M octane,flash-profile=sharp-lh28f800su,flash-select=0,"
            "flash-strap=0,flash-drive=flash0,flash-marker=%s "
            "-m 128M -cpu R10000 "
            "-drive if=none,id=flash0,file=%s,format=raw%s",
            quoted_marker, quoted_drive,
            !strcmp(event, "partial_erase") ? ",file.max-transfer=512" : "");

        QTestState *qts;
        bool erase = !strcmp(event, "partial_erase");
        bool final_flush = !strcmp(event, "final_flush");
        bool marker_create = !strcmp(event, "marker_create");
        bool marker_remove = !strcmp(event, "marker_remove");
        bool marker_lost = !strcmp(event, "dirsync_lost_marker");
        uint16_t old = erase ? 0 : 0xffff;
        QDict *status;

        if (!strncmp(event, "dirsync_", 8) || marker_remove) {
            const char *previous = g_getenv("LD_PRELOAD");
            g_autofree char *preload = g_strdup_printf("%s%s%s",
                previous ?: "", previous ? ":" : "",
                g_getenv("QTEST_SHARP_STORAGE_HELPER"));
            g_autofree char *helper = g_shell_quote(preload);
            g_autofree char *binary = g_shell_quote(qtest_qemu_binary(NULL));
            g_autofree char *directory = g_path_get_dirname(
                g_getenv("SHARP_FAILURE_MARKER"));
            g_autofree char *quoted_dir = g_shell_quote(directory);
            g_autofree char *command = g_strdup_printf(
                "env LD_PRELOAD=%s SHARP_SYNC_DIRECTORY=%s "
                "SHARP_FAIL_DIR_SYNC=%u SHARP_FAIL_UNLINK=%u "
                "SHARP_FAIL_MARKER_RESTORE=%u %s",
                helper, quoted_dir,
                marker_remove ? 0 :
                !strcmp(event, "dirsync_create") ? 1 : 2,
                marker_remove, marker_lost, binary);

            g_setenv("SHARP_FAULT_QEMU", command, true);
            qts = qtest_init_ext("SHARP_FAULT_QEMU", args, NULL, true);
            g_unsetenv("SHARP_FAULT_QEMU");
        } else {
            qts = qtest_init(args);
        }

        qtest_clock_step(qts, 401);
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        qtest_writel(qts, BRIDGE_REG(0x020),
                     qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
        if (marker_create) {
            g_assert_cmpint(g_mkdir(g_getenv("SHARP_FAILURE_MARKER"), 0700),
                            ==, 0);
        }
        if (final_flush) {
            qtest_qmp_assert_success(qts,
                "{'execute':'x-qemu-io','arguments':{'device':'flash0',"
                "'command':'write -P 0xff 0 512'}}");
        } else {
            qtest_writew(qts, FLASH + 0x20, erase ? 0x20 : 0x40);
            qtest_writew(qts, FLASH + 0x20, erase ? 0xd0 : 0x1234);
            qtest_clock_step(qts, erase ? 700000000 : 8000);
            status = qtest_qmp_assert_success_ref(qts,
                "{'execute':'query-status'}");
            g_assert_cmpstr(qdict_get_str(status, "status"), ==, "io-error");
            g_assert_false(qdict_get_bool(status, "running"));
            qobject_unref(status);
            g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0);
            qtest_writew(qts, FLASH, 0xff);
            g_assert_cmphex(qtest_readw(qts, FLASH + 0x20), ==, old);
            sharp_assert_read_view(qts, false);
            g_assert_cmpuint(qtest_writew_tx(qts, FLASH, 0x40), ==,
                             MEMTX_ERROR);
        }
        if (!strcmp(event, "failed_unrealize")) {
            qtest_qmp_assert_success(qts,
                "{'execute':'qom-set','arguments':{'path':'/machine/sharp-flash',"
                "'property':'realized','value':false}}");
            sharp_assert_read_view(qts, false);
        }
        qtest_set_expected_status(qts, EXIT_FAILURE);
        qtest_qmp_assert_success(qts, "{'execute':'quit'}");
        qtest_wait_qemu(qts);
        qtest_quit(qts);
        return;
    }

    dir = g_dir_make_tmp("octane-sharp-error-XXXXXX", NULL);
    path = g_build_filename(dir, "working.raw", NULL);
    marker = g_build_filename(dir, "working.invalid", NULL);
    config = g_build_filename(dir, "blkdebug.conf", NULL);
    image = g_malloc(1 * MiB);
    memset(image, !strcmp(event, "partial_erase") ? 0 : 0xff, 1 * MiB);
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    {
        g_autofree char *rules = g_strdup_printf(
            "[inject-error]\nevent = \"%s\"\n"
            "errno = \"5\"\nonce = \"on\"\n",
            (!strcmp(event, "partial_erase") ||
             !strcmp(event, "failed_unrealize")) ? "write_aio" :
            !strcmp(event, "final_flush") ? "flush_to_disk" :
            (!strncmp(event, "marker_", 7) ||
             !strncmp(event, "dirsync_", 8)) ? "none" : event);

        if (!strcmp(event, "partial_erase")) {
            char *partial = g_strconcat(rules, "sector = \"1\"\n", NULL);

            g_free(g_steal_pointer(&rules));
            rules = partial;
        }
        g_assert_true(g_file_set_contents(config,
            (!strncmp(event, "marker_", 7) ||
             !strncmp(event, "dirsync_", 8)) ? "" : rules, -1, NULL));
    }
    g_setenv("SHARP_WORKING_IMAGE", path, true);
    g_setenv("SHARP_FAILURE_MARKER", marker, true);
    g_setenv("SHARP_DEBUG_CONFIG", config, true);
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr("*working image may be incomplete*");
    if (!strcmp(event, "dirsync_create")) {
        g_test_trap_assert_stderr("*directory sync 1 failed*"
                                 "directory sync 2 returned 0*");
    } else if (!strcmp(event, "marker_remove")) {
        g_test_trap_assert_stderr("*marker unlink failed*");
    } else if (!strcmp(event, "dirsync_remove")) {
        g_test_trap_assert_stderr("*directory sync 1 returned 0*"
                                 "directory sync 2 failed*"
                                 "directory sync 3 returned 0*");
    }
    if (!strcmp(event, "dirsync_lost_marker")) {
        g_autofree char *contents = NULL;
        gsize size;
        GDir *files;
        const char *name;
        unsigned count = 0;

        g_test_trap_assert_stderr("*directory sync 2 failed*"
                                 "marker restoration open failed*"
                                 "exclude it from normal reopen*");
        g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
        g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
        g_assert_cmphex((uint8_t)contents[0x20], ==, 0x12);
        g_assert_cmphex((uint8_t)contents[0x21], ==, 0x34);
        files = g_dir_open(dir, 0, NULL);
        g_assert_nonnull(files);
        while ((name = g_dir_read_name(files))) {
            g_assert_true(!strcmp(name, "working.raw") ||
                          !strcmp(name, "blkdebug.conf"));
            count++;
        }
        g_dir_close(files);
        g_assert_cmpuint(count, ==, 2);
    } else {
        g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
    }
    if (!strcmp(event, "final_flush") || !strcmp(event, "failed_unrealize")) {
        g_autofree char *machine = g_strdup_printf(
            "octane,flash-profile=sharp-lh28f800su,flash-select=0,"
            "flash-strap=0,flash-drive=flash0,flash-marker=%s", marker);
        g_autofree char *drive = g_strdup_printf(
            "if=none,id=flash0,file=%s,format=raw", path);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", machine, "-m", "128M", "-cpu",
            "R10000", "-drive", drive, "-display", "none", "-qmp", "stdio",
            NULL
        };

        assert_launch_rejected(args, "invalid marker");
    }
    if (!strcmp(event, "partial_erase")) {
        g_autofree char *contents = NULL;
        gsize size;

        g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
        g_assert_cmphex((uint8_t)contents[0x20], ==, 0xff);
        g_assert_cmphex((uint8_t)contents[512], ==, 0);
        g_assert_cmphex((uint8_t)contents[64 * KiB], ==, 0);
    }
    if (!strcmp(event, "marker_create") ||
        !strcmp(event, "dirsync_create")) {
        g_autofree char *contents = NULL;
        gsize size;

        g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
        g_assert_cmpmem(contents, size, image, 1 * MiB);
        if (!strcmp(event, "marker_create")) {
            g_assert_cmpint(g_rmdir(marker), ==, 0);
        } else {
            g_assert_cmpint(g_unlink(marker), ==, 0);
        }
    } else if (strcmp(event, "dirsync_lost_marker")) {
        if (!strcmp(event, "marker_remove") ||
            !strcmp(event, "dirsync_remove") ||
            !strcmp(event, "flush_to_disk")) {
            g_autofree char *contents = NULL;
            gsize size;

            g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
            g_assert_cmphex((uint8_t)contents[0x20], ==, 0x12);
            g_assert_cmphex((uint8_t)contents[0x21], ==, 0x34);
        }
        g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));
        g_assert_cmpint(g_unlink(marker), ==, 0);
    }
    g_assert_cmpint(g_unlink(config), ==, 0);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_sharp_flash_shutdown_pending(gconstpointer data)
{
    unsigned pending = GPOINTER_TO_UINT(data);
    bool suspend = pending == 1;
    bool program = pending == 2;
    g_autofree char *dir = g_dir_make_tmp("octane-sharp-pending-XXXXXX",
                                         NULL);
    g_autofree char *path = g_build_filename(dir, "working.raw", NULL);
    g_autofree char *marker = g_build_filename(dir, "working.invalid", NULL);
    g_autofree char *image = g_malloc(1 * MiB);
    QTestState *qts;

    memset(image, 0xff, 1 * MiB);
    image[0x20] = 0x56;
    image[0x21] = 0x78;
    image[64 * KiB] = 0xab;
    image[64 * KiB + 1] = 0xcd;
    g_assert_true(g_file_set_contents(path, image, 1 * MiB, NULL));
    qts = octane_start_sharp(path, marker, 0, 0);
    qtest_writel(qts, BRIDGE_REG(0x020),
                 qtest_readl(qts, BRIDGE_REG(0x020)) | FLASH_WRITE_EN);
    qtest_writew(qts, FLASH + 0x20, program ? 0x40 : 0x20);
    qtest_writew(qts, FLASH + 0x20, program ? 0x1230 : 0xd0);
    g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0);
    if (suspend) {
        qtest_writew(qts, FLASH, 0xb0);
        g_assert_cmphex(qtest_readw(qts, FLASH), ==, 0xc0);
    }
    if (pending == 3) {
        qtest_qmp_assert_success(qts,
            "{'execute':'qom-set','arguments':{'path':'/machine/sharp-flash',"
            "'property':'realized','value':false}}");
        qtest_clock_step(qts, 700000000);
        sharp_assert_read_view(qts, false);
    }
    qtest_quit(qts);
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
    qts = octane_start_sharp(path, marker, 0, 0);
    g_assert_cmphex(qtest_readw(qts, FLASH + 0x20), ==, 0x5678);
    g_assert_cmphex(qtest_readw(qts, FLASH + 64 * KiB), ==, 0xabcd);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_rad_pci_identity(gconstpointer data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    QTestState *qts = octane2 ? octane2_start_args("") : octane_start();

    g_assert_cmphex(qtest_readl(qts, CFG(3)), ==, RAD_PCI_ID);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8), ==, RAD_REVISION);
    qtest_writel(qts, CFG(3) + 8, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8), ==, RAD_REVISION);
    qtest_writel(qts, CFG(3) + 8, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8), ==, RAD_REVISION);

    qtest_writel(qts, CFG(3) + 0x10, RAD_PCI_BAR0);
    qtest_writel(qts, CFG(3) + 4, 6);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 0x10), ==, RAD_PCI_BAR0);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 4) & 0xffff, ==, 6);
    /* Machine reset also reaches PCI bus reset and clears writable config. */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 4) & 0xffff, ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(3)), ==, RAD_PCI_ID);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8), ==, RAD_REVISION);
    qtest_writel(qts, CFG(3) + 8, 0x04010070);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8), ==, RAD_REVISION);
    qtest_quit(qts);
}

static void test_rad_pci(void)
{
    QTestState *qts = octane_start();
    uint32_t value;

    g_assert_cmphex(qtest_readl(qts, CFG(3)), ==, RAD_PCI_ID);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8) & 0xff, ==, RAD_REVISION);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);

    /* BAR sizing is a model property, not a physical-card measurement. */
    qtest_writel(qts, CFG(3) + 0x10, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 0x10), ==, 0xffffe000);
    qtest_writel(qts, CFG(3) + 0x10, RAD_PCI_BAR0);
    qtest_writel(qts, CFG(3) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 3 + 4, 0x1130);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 0x10), ==, RAD_PCI_BAR0);
    /* A failed RAD read propagates even with Bridge error reporting masked. */
    g_assert_cmpuint(qtest_readl_tx(qts, RAD_BAR0, &value), ==, MEMTX_ERROR);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & (1u << 15), ==, 0);
    g_assert_cmpuint(qtest_readl_tx(qts, RAD_BAR0 + 0x30, &value), ==,
                     MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(2)), ==, 0x000310a9);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x12345678);

    qtest_quit(qts);
}

static void octane_rad_map(QTestState *qts)
{
    qtest_writel(qts, CFG(3) + 0x10, RAD_PCI_BAR0);
    qtest_writel(qts, CFG(3) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 3 + 4, 0x1130);
}


static const struct {
    uint16_t offset;
    uint32_t value;
} octane_rad_startup_words[] = {
    { 0x44, 0xa0000008 }, { 0x48, 0x10 }, { 0x4c, 0x03000000 },
    { 0x50, 0x60000000 }, { 0x54, 0x4000000c }, { 0x70, 0x1440 },
    { 0x74, 0x08000010 }, { 0x78, 0x00fac688 }, { 0x7c, 0xd6d6d6d6 },
    { 0x80, 0xffe20000 }, { 0x88, 3 }, { 0x8c, 2 }, { 0x90, 2 },
    { 0xa0, 0x02000001 }, { 0xa4, 0x12345678 }, { 0xa8, 1 },
    { 0xac, 2 }, { 0xb0, 3 }, { 0xb4, 4 }, { 0xb8, 0x9fffffff },
    { 0xbc, 0x9fffffff }, { 0x4c0, 0xa000c000 }, { 0x4c8, 0xa000c800 },
    { 0x4d0, 0xa000d000 }, { 0x4d8, 0xa000d800 }, { 0x4e0, 0xa000e000 },
    { 0x4e8, 0xa000e800 }, { 0x4f0, 0xa000f000 },
};

static bool octane_rad_subcode_word(unsigned offset)
{
    return offset >= 0x1000 && offset <= 0x1218 &&
           offset != 0x1078 && offset != 0x10f8;
}

static uint64_t octane_rad_subcode_pattern(unsigned offset)
{
    return UINT64_C(0x0123456789abcdef) ^ ((uint64_t)offset << 32) ^ offset;
}

static void octane_rad_startup_program(QTestState *qts)
{
    for (unsigned i = 0; i < ARRAY_SIZE(octane_rad_startup_words); i++) {
        qtest_writel(qts, RAD_BAR0 + octane_rad_startup_words[i].offset,
                     octane_rad_startup_words[i].value);
    }
    for (unsigned offset = 0x1000; offset <= 0x1218; offset += 8) {
        if (octane_rad_subcode_word(offset)) {
            qtest_writeq(qts, RAD_BAR0 + offset,
                         octane_rad_subcode_pattern(offset));
        }
    }
}

static void octane_rad_startup_check(QTestState *qts, bool reset)
{
    for (unsigned i = 0; i < ARRAY_SIZE(octane_rad_startup_words); i++) {
        g_assert_cmphex(qtest_readl(qts,
                        RAD_BAR0 + octane_rad_startup_words[i].offset), ==,
                        reset ? 0 : octane_rad_startup_words[i].value);
    }
    for (unsigned offset = 0x1000; offset <= 0x1218; offset += 8) {
        if (octane_rad_subcode_word(offset)) {
            g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + offset), ==,
                            reset ? 0 : octane_rad_subcode_pattern(offset));
        }
    }
}

static void test_rad_startup_storage(void)
{
    QTestState *qts = octane_start();
    static const struct { unsigned offset; uint32_t value; } sequence[] = {
        { 0x80, 0xffffffff }, { 0x80, 0xffe3cffe },
        { 0x80, 0xffe3c0fe }, { 0x80, 0xffe24070 },
        { 0x80, 0xffe20000 }, { 0x70, 0x1500 }, { 0x70, 0x1540 },
        { 0x70, 0x1440 }, { 0x70, 0x1540 }, { 0x88, 3 },
        { 0x88, 2 }, { 0x88, 0 }, { 0x7c, 0xc0c0c0c0 },
        { 0x70, 0x480 }, { 0x80, 0xfffffffe },
        { 0x80, 0xffffcbfc }, { 0x80, 0xffff8bfc },
        { 0xa8, 0x13579bdf }, { 0xac, 0x2468ace0 },
        { 0xb0, 0x89abcdef }, { 0xb4, 0xfedcba98 },
    };

    octane_rad_map(qts);
    octane_rad_startup_check(qts, true);
    for (unsigned i = 0; i < ARRAY_SIZE(sequence); i++) {
        qtest_writel(qts, RAD_BAR0 + sequence[i].offset, sequence[i].value);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + sequence[i].offset), ==,
                        sequence[i].value);
    }
    octane_rad_startup_program(qts);
    octane_rad_startup_check(qts, false);
    qtest_writeq(qts, RAD_BAR0 + 0x1060, 0);
    qtest_writeq(qts, RAD_BAR0 + 0x10e0, 0);
    for (unsigned offset = 0x1000; offset <= 0x1218; offset += 8) {
        if (octane_rad_subcode_word(offset)) {
            g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + offset), ==,
                            offset == 0x1060 || offset == 0x10e0 ? 0 :
                            octane_rad_subcode_pattern(offset));
        }
    }
    qtest_system_reset(qts);
    octane_rad_map(qts);
    octane_rad_startup_check(qts, true);
    qtest_quit(qts);
}

static void test_rad_subcode_width_swap(void)
{
    QTestState *qts = octane_start();
    const uint64_t value = UINT64_C(0x0123456789abcdef);
    const uint64_t sentinel = UINT64_C(0xfedcba9876543210);

    octane_rad_map(qts);
    for (unsigned mode = 0; mode < 4; mode++) {
        uint32_t config = 0x1130 | ((mode & 1) ? 1u << 22 : 0) |
                          ((mode & 2) ? 1u << 13 : 0);
        uint32_t low = mode == 3 ? value >> 32 : value;
        uint32_t high = mode == 3 ? value : value >> 32;

        qtest_writel(qts, BRIDGE_REG(0x218), config);
        for (unsigned offset = 0x1000; offset <= 0x1218; offset += 8) {
            uint64_t pattern = octane_rad_subcode_pattern(offset);

            if (!octane_rad_subcode_word(offset)) {
                continue;
            }
            qtest_writeq(qts, RAD_BAR0 + offset, pattern);
            g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + offset), ==, pattern);
            g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + offset), ==,
                            mode == 3 ? pattern >> 32 : (uint32_t)pattern);
            g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + offset + 4), ==,
                            mode == 3 ? (uint32_t)pattern : pattern >> 32);
        }
        qtest_writeq(qts, RAD_BAR0 + 0x1100, sentinel);
        g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + 0x1100), ==, sentinel);
        qtest_writeq(qts, RAD_BAR0 + 0x1110, sentinel);
        qtest_writeq(qts, RAD_BAR0 + 0x1108, value);
        g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + 0x1108), ==, value);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1108), ==, low);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x110c), ==, high);
        /* Independent halves must preserve their neighbor at the BAR. */
        qtest_writel(qts, RAD_BAR0 + 0x1108, 0x13579bdf);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x110c), ==, high);
        qtest_writel(qts, RAD_BAR0 + 0x110c, 0x2468ace0);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1108), ==, 0x13579bdf);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x110c), ==, 0x2468ace0);
        for (unsigned dma = 0; dma < 4; dma++) {
            qtest_writel(qts, RAD_BAR0 + 0x1108, 0x13579bdf);
            qtest_writel(qts, BRIDGE_REG(0x218), config | (dma << 19));
            g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1108), ==,
                            0x13579bdf);
            qtest_writeq(qts, RAD_BAR0 + 0x1108, value);
            g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + 0x1108), ==, value);
            g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1108), ==, low);
            g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x110c), ==, high);
        }
        g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + 0x1100), ==, sentinel);
        g_assert_cmphex(qtest_readq(qts, RAD_BAR0 + 0x1110), ==, sentinel);
    }
    qtest_quit(qts);
}

static void octane_rad_status_program(QTestState *qts, uint64_t base,
                                      uint32_t limit)
{
    qtest_writel(qts, RAD_BAR0 + 0x500, base);
    qtest_writel(qts, RAD_BAR0 + 0x528, base >> 32);
    qtest_writel(qts, RAD_BAR0 + 0x504, limit);
    qtest_writel(qts, RAD_BAR0 + 0x4b4, base);
    qtest_writel(qts, RAD_BAR0 + 0x4b8, base >> 32);
    qtest_writel(qts, RAD_BAR0 + 0x4bc, limit);
}

static void test_rad_status_direct(void)
{
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x8000;

    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_writel(qts, BRIDGE_REG(0x030), 8u << 16);
    qtest_writel(qts, BRIDGE_REG(0x038), 0x80);
    qtest_writel(qts, BRIDGE_REG(0x130 + 8 * 3), 16);
    qtest_writel(qts, BRIDGE_REG(0x118), RAD_PIN);
    qtest_writel(qts, BRIDGE_REG(0x108), RAD_PIN);
    qtest_writeq(qts, HEART + 0x10000, UINT64_MAX);
    qtest_memset(qts, buffer - 4, 0xa5, 0x88);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, 0x01020304ULL * 960);
    octane_rad_status_program(qts, 0xa0008000, 0xfffffffc);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0xa5a5a5a5);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x70), ==, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x78), ==, 0);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0x80080000);
    g_assert_cmphex(qtest_readl(qts, buffer + 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, buffer - 4), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x80), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_false(qtest_get_irq(qts, i));
    }
    qtest_quit(qts);
}

static void test_rad_status_primary_only(gconstpointer data)
{
    bool secondary = GPOINTER_TO_INT(data);
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x8000;
    const uint64_t other = RAM + 0x9000;

    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_memset(qts, buffer, 0xa5, 0x80);
    qtest_memset(qts, other, 0xa5, 0x80);
    qtest_writel(qts, RAD_BAR0 + 0x500, 0xa0008000);
    qtest_writel(qts, RAD_BAR0 + 0x528, 0);
    qtest_writel(qts, RAD_BAR0 + 0x504, 0xfffffffc);
    if (secondary) {
        qtest_writel(qts, RAD_BAR0 + 0x4b4, 0xa0009000);
        qtest_writel(qts, RAD_BAR0 + 0x4b8, 0x12345678);
        qtest_writel(qts, RAD_BAR0 + 0x4bc, 0xffffc078);
    }
    g_assert_cmpuint(qtest_writel_tx(qts, RAD_BAR0 + 0x5c, 0x40001), ==,
                     MEMTX_OK);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0x80080000);
    g_assert_cmphex(qtest_readl(qts, other + 0x7c), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x4b4), ==,
                    secondary ? 0xa0009000 : 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x4b8), ==,
                    secondary ? 0x12345678 : 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x4bc), ==,
                    secondary ? 0xffffc078 : 0);

    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x1fffc);
    qtest_writel(qts, buffer + 0x7c, 0xa5a5a5a5);
    qtest_writel(qts, RAD_BAR0 + 0x504, 0xffffc078);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, 0x1fffc);
    qtest_writel(qts, RAD_BAR0 + 0x504, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x500, 0);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, 0x1fffc);
    qtest_quit(qts);
}

static void octane_rad_output_program(QTestState *qts, uint64_t base)
{
    qtest_writel(qts, RAD_BAR0 + 0x4f8, base);
    qtest_writel(qts, RAD_BAR0 + 0x524, base >> 32);
    qtest_writel(qts, RAD_BAR0 + 0x4fc, 0xfffff008);
    qtest_writel(qts, RAD_BAR0 + 0x40c, base);
    qtest_writel(qts, RAD_BAR0 + 0x410, base >> 32);
    qtest_writel(qts, RAD_BAR0 + 0x414, 0xfffff008);
}

static void test_rad_status_routes(void)
{
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x8000;
    const uint64_t page = RAM + 0x9000;

    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, 0x01020304ULL * 960);
    for (unsigned swap = 0; swap < 4; swap++) {
        qtest_writel(qts, BRIDGE_REG(0x218), 0x1130 | (swap << 19));
        qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
        octane_rad_status_program(qts, 0xa0008000, 0xfffffffc);
        qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
        g_assert_cmphex(qtest_readl(qts, buffer + 0x70), ==,
                        swap & 1 ? 0x04030201 : 0x01020304);
        qtest_writeq(qts, BRIDGE + 0x10000, buffer | 0x801);
        octane_rad_status_program(qts, 0x40000000, 0xfffffffc);
        qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
        g_assert_cmphex(qtest_readl(qts, buffer + 0x70), ==,
                        swap & 2 ? 0x04030201 : 0x01020304);
    }
    qtest_writel(qts, BRIDGE_REG(0x218), 0x1130);
    octane_rad_status_program(qts, 0x8000000020008000ULL, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0x80080000);

    /* The two virtual pages deliberately name nonadjacent RAM pages. */
    qtest_writeq(qts, BRIDGE + 0x10000, buffer | 0x801);
    qtest_writeq(qts, BRIDGE + 0x10008, (RAM + 0xb000) | 0x801);
    octane_rad_status_program(qts, 0x40000fc0, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, RAM + 0xb03c), ==, 0x80080000);
    qtest_memset(qts, page, 0xa5, 0x80);
    qtest_writel(qts, BRIDGE_REG(0x080), 9u << 20);
    octane_rad_status_program(qts, 0xa0009000, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, page + 0x7c), ==, 0xa5a5a5a5);
    qtest_writeq(qts, BRIDGE + 0x10000, 0);
    octane_rad_status_program(qts, 0x40000000, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, page + 0x7c), ==, 0xa5a5a5a5);

    /* Mirrors succeed before the final page's invalid translation fails. */
    qtest_memset(qts, RAM + 0x8f88, 0xa5, 0x78);
    qtest_writeq(qts, BRIDGE + 0x10000, buffer | 0x801);
    qtest_writeq(qts, BRIDGE + 0x10008, 0);
    octane_rad_status_program(qts, 0x40000f88, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x8ff8), ==, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x8ffc), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x9004), ==, 0xa5a5a5a5);
    qtest_quit(qts);
}

static void test_rad_output_clock(void)
{
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x8000;
    uint32_t count;

    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_writel(qts, BRIDGE_REG(0x030), 8u << 16);
    qtest_writel(qts, BRIDGE_REG(0x038), 0x80);
    qtest_writel(qts, BRIDGE_REG(0x130 + 8 * 3), 16);
    qtest_writel(qts, BRIDGE_REG(0x118), RAD_PIN);
    qtest_writel(qts, BRIDGE_REG(0x108), RAD_PIN);
    qtest_writeq(qts, HEART + 0x10000, UINT64_MAX);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    octane_rad_output_program(qts, 0xa000a000);
    qtest_writel(qts, RAD_BAR0 + 0x80, 1);
    qtest_writel(qts, RAD_BAR0 + 0x4c, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_clock_step(qts, 22675);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==,
                    (1u << 18) | ((1022676 / 960) & 0x3ffff));
    qtest_clock_step(qts, 24977324);
    count = 1102;
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, count);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x18), ==, count << 4);
    octane_rad_status_program(qts, 0xa0008000, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x38), ==,
                    0xa000a000 + (count * 8) % 4096);
    for (unsigned i = 0; i < 8; i++) {
        if (i != 7) {
            g_assert_cmphex(qtest_readl(qts, buffer + 8 * i), ==, 0);
        }
    }
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000000);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, count);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_clock_step(qts, 22676);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, count + 1);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x38), ==, 0xa000a008);
    qtest_clock_step(qts, 400000000);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==,
                     (count + 1 + 17640) & 0x3fff);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_false(qtest_get_irq(qts, i));
    }
    qtest_system_reset(qts);
    octane_rad_map(qts);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x4f8), ==, 0);
    qtest_quit(qts);
}

static void test_rad_status_continuous(void)
{
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x10000;
    const uint32_t ctrl = 0x40003 | (330 << 2);

    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_memset(qts, buffer, 0xa5, 128 * 0x80);
    for (unsigned i = 0; i < 3; i++) {
        octane_rad_status_program(qts, 0xa0010000, 0xffffc078);
        qtest_writel(qts, RAD_BAR0 + 0x4b4 + 4 * i, 1);
        qtest_writel(qts, RAD_BAR0 + 0x5c, ctrl);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, 0);
        qtest_clock_step(qts, 10000);
        g_assert_cmphex(qtest_readl(qts, buffer + 0xfc), ==, 0xa5a5a5a5);
    }
    octane_rad_status_program(qts, 0xa0010000, 0xffffc078);
    qtest_writel(qts, RAD_BAR0 + 0x5c, ctrl);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, ctrl);
    qtest_clock_step(qts, 9999);
    g_assert_cmphex(qtest_readl(qts, buffer + 0xfc), ==, 0xa5a5a5a5);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, buffer + 0xfc), ==, 0x80080000);
    g_assert_cmphex(qtest_readl(qts, buffer + 0x7c), ==, 0xa5a5a5a5);
    qtest_writel(qts, RAD_BAR0 + 0x500, 0xa0020000);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x500), ==, 0xa0010000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, ctrl);
    for (unsigned i = 2; i <= 260; i++) {
        unsigned cursor = i % 128;
        qtest_writel(qts, buffer + cursor * 0x80 + 0x7c, 0);
        qtest_clock_step(qts, 10000);
        g_assert_cmphex(qtest_readl(qts, buffer + cursor * 0x80 + 0x7c), ==,
                        0x80080000);
    }
    octane_rad_output_program(qts, 0xa0020000);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x1fffc);
    qtest_writel(qts, buffer + 5 * 0x80 + 0x7c, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, buffer + 5 * 0x80 + 0x7c), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, 44);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000000);
    qtest_writel(qts, RAD_BAR0 + 0x5c, ctrl);
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readl(qts, buffer + 0xfc), ==, 0x80080000);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, 44);
    qtest_system_reset(qts);
    octane_rad_map(qts);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x500), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, 0);
    qtest_quit(qts);
}

static void test_rad_status_overrun(void)
{
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x10000;
    unsigned completed = 0;

    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    octane_rad_status_program(qts, 0xa0010000, 0xffffc078);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40003 | (330 << 2));
    /* A stopped VM lets the clock pass a ring without running callbacks. */
    qtest_clock_step(qts, 2000000);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_clock_step(qts, 1);
    for (unsigned i = 0; i < 128; i++) {
        completed += qtest_readl(qts, buffer + i * 0x80 + 0x7c) == 0x80080000;
    }
    g_assert_cmpuint(completed, ==, 1);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x1fffc);
    qtest_memset(qts, buffer, 0, 128 * 0x80);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40007);
    qtest_clock_step(qts, 29);
    g_assert_cmphex(qtest_readl(qts, buffer + 0xfc), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, buffer + 0xfc), ==, 0x80080000);
    qtest_quit(qts);
}

static void test_rad_startup_unsupported(void)
{
    g_autofree char *path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *args = NULL;
    g_autofree char *log = NULL;
    int fd = g_file_open_tmp("octane-rad-startup-XXXXXX", &path, NULL);
    QTestState *qts;
    const uint64_t buffer = RAM + 0x8000;
    const unsigned enables[] = { 0x44, 0x48, 0x4c, 0x50, 0x54 };
    const unsigned holes[] = { 0x1078, 0x107c, 0x10f8, 0x10fc, 0x1220 };
    const unsigned descriptors[] = {
        0x4c4, 0x4cc, 0x4d4, 0x4dc, 0x4e4, 0x4ec, 0x4f4,
        0x508, 0x50c, 0x510, 0x514, 0x518, 0x51c, 0x520,
    };
    uint8_t memory[0x100];

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(path);
    args = g_strdup_printf("-d unimp -D %s", quoted);
    qts = octane_start_args(args);
    octane_rad_map(qts);
    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    qtest_memset(qts, buffer, 0xa5, sizeof(memory));
    octane_rad_startup_program(qts);
    for (unsigned i = 0; i < ARRAY_SIZE(enables); i++) {
        uint32_t before = qtest_readl(qts, RAD_BAR0 + enables[i]);

        qtest_writel(qts, RAD_BAR0 + enables[i], before | 1);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + enables[i]), ==, before);
        if (enables[i] == 0x44 || enables[i] == 0x48) {
            qtest_writel(qts, RAD_BAR0 + enables[i], before | 4);
            g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + enables[i]), ==,
                            before);
        }
    }
    /* Narrow CPU addresses are corrected back to the aligned BAR word. */
    qtest_writeb(qts, RAD_BAR0 + 0x83, 0x12);
    qtest_writew(qts, RAD_BAR0 + 0x82, 0x1234);
    qtest_readb(qts, RAD_BAR0 + 0x83);
    qtest_readw(qts, RAD_BAR0 + 0x82);
    qtest_writeb(qts, RAD_BAR0 + 0x1103, 0x12);
    qtest_writew(qts, RAD_BAR0 + 0x1102, 0x1234);
    qtest_readb(qts, RAD_BAR0 + 0x1103);
    qtest_readw(qts, RAD_BAR0 + 0x1102);
    for (unsigned i = 0; i < ARRAY_SIZE(holes); i++) {
        qtest_writel(qts, RAD_BAR0 + holes[i], 0x12345678);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + holes[i]), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(descriptors); i++) {
        qtest_writel(qts, RAD_BAR0 + descriptors[i], 0x12345678);
        g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + descriptors[i]), ==, 0);
    }
    qtest_writel(qts, BRIDGE_REG(0x218), 0x1130 | (1u << 22));
    qtest_writeq(qts, RAD_BAR0 + 0x80, UINT64_C(0x123456789abcdef0));
    qtest_readq(qts, RAD_BAR0 + 0x80);
    qtest_writeq(qts, RAD_BAR0 + 0x1078, UINT64_MAX);
    qtest_readq(qts, RAD_BAR0 + 0x1078);
    qtest_writeq(qts, RAD_BAR0 + 0x10f8, UINT64_MAX);
    qtest_readq(qts, RAD_BAR0 + 0x10f8);
    qtest_writeq(qts, RAD_BAR0 + 0x1220, UINT64_MAX);
    qtest_readq(qts, RAD_BAR0 + 0x1220);
    /* This unaligned CPU word is rejected before reaching RAD. */
    qtest_writel_tx(qts, RAD_BAR0 + 0x81, 0x12345678);
    qtest_writel(qts, BRIDGE_REG(0x218), 0x1130);
    octane_rad_startup_check(qts, false);
    /* An unaligned CPU doubleword becomes accepted aligned BAR halves. */
    qtest_writeq(qts, RAD_BAR0 + 0x1104, UINT64_MAX);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1104), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1108), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x1100), ==,
                    (uint32_t)octane_rad_subcode_pattern(0x1100));
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x110c), ==,
                    octane_rad_subcode_pattern(0x1108) >> 32);
    /* Split cycles commit separately, even when either half is rejected. */
    qtest_writeq(qts, RAD_BAR0 + 0x90, UINT64_C(0x1234567889abcdef));
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x90), ==, 0x89abcdef);
    qtest_writeq(qts, RAD_BAR0 + 0x48, UINT64_C(0x0300000200000011));
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x48), ==, 0x10);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x4c), ==, 0x03000002);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x18), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0);
    qtest_memread(qts, buffer, memory, sizeof(memory));
    for (unsigned i = 0; i < sizeof(memory); i++) {
        g_assert_cmphex(memory[i], ==, 0xa5);
    }
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_false(qtest_get_irq(qts, i));
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "unsupported 1-byte BAR0 write at 0x80"));
    g_assert_nonnull(strstr(log, "unsupported 2-byte BAR0 write at 0x80"));
    g_assert_nonnull(strstr(log, "unsupported 1-byte BAR0 read at 0x80"));
    g_assert_nonnull(strstr(log, "unsupported 2-byte BAR0 read at 0x80"));
    g_assert_nonnull(strstr(log, "unsupported 1-byte BAR0 write at 0x1100"));
    g_assert_nonnull(strstr(log, "unsupported 2-byte BAR0 write at 0x1100"));
    g_assert_nonnull(strstr(log, "unsupported 8-byte BAR0 write at 0x80"));
    g_assert_nonnull(strstr(log, "unsupported 8-byte BAR0 read at 0x80"));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 write at 0x94"));
    for (unsigned i = 0; i < ARRAY_SIZE(enables); i++) {
        g_autofree char *needle = g_strdup_printf(
            "unsupported 4-byte BAR0 write at 0x%x", enables[i]);
        g_assert_nonnull(strstr(log, needle));
    }
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_rad_startup_running(void)
{
    QTestState *qts = octane_start();
    const uint64_t buffer = RAM + 0x10000;
    const uint32_t ctrl = 0x40003 | (330 << 2);
    uint32_t position, ust;

    octane_rad_map(qts);
    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_memset(qts, buffer, 0xa5, 128 * 0x80);
    octane_rad_output_program(qts, 0xa0020000);
    octane_rad_status_program(qts, 0xa0010000, 0xffffc078);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_writel(qts, RAD_BAR0 + 0x5c, ctrl);
    qtest_clock_step(qts, 25000);
    position = qtest_readl(qts, RAD_BAR0 + 0x2c);
    ust = qtest_readl(qts, RAD_BAR0 + 0x30);
    octane_rad_startup_program(qts);
    qtest_writel(qts, RAD_BAR0 + 0x70, 0x1540);
    qtest_writel(qts, RAD_BAR0 + 0x7c, 0);
    qtest_writel(qts, RAD_BAR0 + 0x88, 0);
    qtest_writel(qts, RAD_BAR0 + 0x4c, 1);
    qtest_writel(qts, RAD_BAR0 + 0x44, 4);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, position);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, ust);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x58), ==, 0x20000001);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, ctrl);
    qtest_clock_step(qts, 4999);
    g_assert_cmphex(qtest_readl(qts, buffer + 3 * 0x80 + 0x7c), ==, 0xa5a5a5a5);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, buffer + 3 * 0x80 + 0x7c), ==, 0x80080000);
    g_assert_cmphex(qtest_readl(qts, buffer + 3 * 0x80 + 0x38), ==, 0xa0020008);
    g_assert_cmphex(qtest_readl(qts, buffer + 3 * 0x80 + 0x70), ==, 31);
    qtest_clock_step(qts, 15351);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, 2);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000000);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x1fffc);
    position = qtest_readl(qts, RAD_BAR0 + 0x2c);
    qtest_memset(qts, buffer, 0xa5, 128 * 0x80);
    octane_rad_startup_program(qts);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, position);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(qtest_readl(qts, buffer + i * 0x80 + 0x7c), ==,
                        0xa5a5a5a5);
    }
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_false(qtest_get_irq(qts, i));
    }
    qtest_system_reset(qts);
    octane_rad_map(qts);
    octane_rad_startup_check(qts, true);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x58), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);
    qtest_quit(qts);
}

static void test_rad_transfer_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("octane-rad-transfer-XXXXXX",
                                               &error);
    g_autofree char *socket = g_build_filename(directory, "mig.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = octane_start();
    QTestState *to = octane_start_args("-incoming defer");
    const uint64_t buffer = RAM + 0x10000;
    const uint32_t ctrl = 0x40003 | (330 << 2);

    g_assert_no_error(error);
    octane_rad_map(from);
    qtest_writel(from, CFG(3) + 4, 6);
    qtest_writel(from, BRIDGE_REG(0x080), 8u << 20);
    qtest_writel(from, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(from, RAD_BAR0 + 0x40, 0x0a);
    octane_rad_status_program(from, 0xa0010000, 0xffffc078);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    octane_rad_output_program(from, 0xa0020000);
    qtest_writel(from, RAD_BAR0 + 0x58, 0x20000001);
    qtest_writel(from, RAD_BAR0 + 0x5c, ctrl);
    qtest_clock_step(from, 25000);
    octane_rad_startup_program(from);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    octane_rad_startup_check(to, false);
    g_assert_cmpuint(qtest_readl(to, RAD_BAR0 + 0x2c) >> 18, ==, 1);
    g_assert_cmpuint(qtest_readl(to, RAD_BAR0 + 0x30), ==, 26);
    qtest_clock_step(to, 4999);
    g_assert_cmphex(qtest_readl(to, buffer + 3 * 0x80 + 0x7c), ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readl(to, buffer + 3 * 0x80 + 0x7c), ==, 0x80080000);
    g_assert_cmphex(qtest_readl(to, buffer + 3 * 0x80 + 0x38), ==, 0xa0020008);
    qtest_clock_step(to, 15351);
    g_assert_cmpuint(qtest_readl(to, RAD_BAR0 + 0x2c) >> 18, ==, 1);
    qtest_clock_step(to, 1);
    g_assert_cmpuint(qtest_readl(to, RAD_BAR0 + 0x2c) >> 18, ==, 2);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void octane_rad_output_samples(uint64_t elapsed_ns,
                                      bool read_before_disable)
{
    g_autofree char *path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *wav = NULL;
    QTestState *qts;
    gsize length;
    const uint32_t frames = elapsed_ns * 44100 / 1000000000;
    const uint32_t pointer = 0xa000a000 + (frames * 8) % 4096;
    uint32_t position_a, position_b;
    int fd = g_file_open_tmp("octane-rad-output-XXXXXX.wav", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(path);
    extra = g_strdup_printf("-audiodev wav,id=radwav,path=%s,"
                             "out.frequency=44100,out.channels=2,out.format=s16 "
                             "-global sgi-rad1-octane.audiodev=radwav", quoted);
    qts = octane_start_args(extra);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    for (unsigned i = 0; i < 512; i++) {
        uint32_t left = 0x1000 + i;
        uint32_t right = -(0x2000 + i);

        qtest_writel(qts, RAM + 0xa000 + i * 8, (left << 8) | 0x5a);
        qtest_writel(qts, RAM + 0xa004 + i * 8, (right << 8) | 0xa5);
    }
    octane_rad_output_program(qts, 0xa000a000);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    octane_rad_startup_program(qts);
    /* Startup mute, gain and board storage must leave samples intact. */
    qtest_writel(qts, RAD_BAR0 + 0x70, 0x1540);
    qtest_writel(qts, RAD_BAR0 + 0x7c, 0);
    qtest_writel(qts, RAD_BAR0 + 0x88, 0);
    qtest_clock_step(qts, elapsed_ns);
    if (read_before_disable) {
        g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, frames);
    }
    /* The disable write must transfer frames due since the last timer. */
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000000);
    position_a = qtest_readl(qts, RAD_BAR0 + 0x2c);
    position_b = qtest_readl(qts, RAD_BAR0 + 0x18);
    g_assert_cmpuint(position_a >> 18, ==, frames);
    g_assert_cmphex(position_b, ==, frames << 4);
    octane_rad_status_program(qts, 0xa0008000, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x8038), ==, pointer);

    /* Give accepted host samples time to drain without advancing DMA. */
    qtest_clock_step(qts, 20000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, position_a);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x18), ==, position_b);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x8038), ==, pointer);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &wav, &length, NULL));
    g_assert_cmpuint(length, ==, 44 + frames * 4);
    g_assert_cmpmem(wav, 4, "RIFF", 4);
    g_assert_cmpmem(wav + 8, 8, "WAVEfmt ", 8);
    g_assert_cmpmem(wav + 36, 4, "data", 4);
    g_assert_cmpuint(ldl_le_p(wav + 4), ==, length - 8);
    g_assert_cmpuint(ldl_le_p(wav + 40), ==, frames * 4);
    g_assert_cmpuint(lduw_le_p(wav + 20), ==, 1);
    g_assert_cmpuint(lduw_le_p(wav + 22), ==, 2);
    g_assert_cmpuint(ldl_le_p(wav + 24), ==, 44100);
    g_assert_cmpuint(lduw_le_p(wav + 34), ==, 16);
    for (unsigned i = 0; i < frames; i++) {
        int16_t left = lduw_le_p(wav + 44 + i * 4);
        int16_t right = lduw_le_p(wav + 46 + i * 4);

        g_assert_cmpint(left, ==, 0x1000 + i % 512);
        g_assert_cmpint(right, ==, -(int)(0x2000 + i % 512));
    }
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_rad_output_samples(void)
{
    octane_rad_output_samples(50000000, false);
}

static void test_rad_output_tail(gconstpointer data)
{
    /* 44 frames are due at 1 ms, between eight-frame timer callbacks. */
    octane_rad_output_samples(1000000, GPOINTER_TO_INT(data));
}

static void test_rad_transfer_unsupported(void)
{
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("octane-rad-transfer-XXXXXX", &log_path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", quoted);
    qts = octane_start_args(extra);
    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x58), ==, 0);
    octane_rad_output_program(qts, 0xa000a000);
    qtest_writel(qts, RAD_BAR0 + 0x40c, 0xa000b000);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x58), ==, 0);
    octane_rad_output_program(qts, 0xa000a000);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_writel(qts, RAD_BAR0 + 0x4f8, 0xa000b000);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x55);
    qtest_writeb(qts, RAD_BAR0 + 0x58, 0);
    qtest_writew(qts, RAD_BAR0 + 0x58, 0);
    qtest_writel(qts, BRIDGE_REG(0x218), 0x1130 | (1u << 22));
    qtest_writeq(qts, RAD_BAR0 + 0x58, 0);
    qtest_writel(qts, BRIDGE_REG(0x218), 0x1130);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x58), ==, 0x20000001);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x4f8), ==, 0xa000a000);
    octane_rad_status_program(qts, 0xa0008000, 0xfffff078);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40003 | (330 << 2));
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x5c), ==, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x2c) >> 18, ==, 44);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 write at 0x58"));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 write at 0x4f8"));
    g_assert_nonnull(strstr(log, "unsupported 1-byte BAR0 write"));
    g_assert_nonnull(strstr(log, "unsupported 2-byte BAR0 write"));
    g_assert_nonnull(strstr(log, "unsupported 8-byte BAR0 write"));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 write at 0x5c"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_rad_transfer_failures(void)
{
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    uint32_t position;
    int fd = g_file_open_tmp("octane-rad-dma-error-XXXXXX", &log_path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d guest_errors -D %s", quoted);
    qts = octane_start_args(extra);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    octane_rad_map(qts);
    qtest_writel(qts, CFG(3) + 4, 6);
    qtest_writel(qts, BRIDGE_REG(0x080), 9u << 20);
    octane_rad_output_program(qts, 0xa000a000);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, 0);

    /* Successful chunks before a later invalid page retain their progress. */
    qtest_writeq(qts, BRIDGE + 0x10000, (RAM + 0xa000) | 0x801);
    qtest_writeq(qts, BRIDGE + 0x10008, 0);
    octane_rad_output_program(qts, 0x40000c00);
    qtest_writel(qts, RAD_BAR0 + 0x58, 0x20000001);
    qtest_clock_step(qts, 5000000);
    position = qtest_readl(qts, RAD_BAR0 + 0x2c);
    g_assert_cmpuint(position >> 18, ==, 128);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x18), ==, 128 << 4);
    qtest_writeq(qts, BRIDGE + 0x10008, (RAM + 0xc000) | 0x801);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x2c), ==, position);
    octane_rad_status_program(qts, 0xa0008000, 0xfffffffc);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40001);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x8038), ==, 0x40001000);

    qtest_memset(qts, RAM + 0x8000, 0xa5, 0x4000);
    qtest_writeq(qts, BRIDGE + 0x10000, 0);
    octane_rad_status_program(qts, 0x40000000, 0xffffc078);
    qtest_writel(qts, RAD_BAR0 + 0x5c, 0x40003 | (330 << 2));
    qtest_clock_step(qts, 10000);
    qtest_writeq(qts, BRIDGE + 0x10000, (RAM + 0x8000) | 0x801);
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x80fc), ==, 0xa5a5a5a5);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "analog output DMA at 0xa000a000 failed"));
    g_assert_nonnull(strstr(log, "analog output DMA at 0x40001000 failed"));
    g_assert_nonnull(strstr(log, "status-DMA slot write"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_rad_ust_clock(void)
{
    QTestState *qts = octane_start();
    uint32_t before;

    octane_rad_map(qts);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0);
    qtest_clock_step(qts, 9600);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0);

    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_clock_step(qts, 9600);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, 9600);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 10);
    qtest_clock_step(qts, 959);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 10);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 11);

    qtest_writel(qts, RAD_BAR0 + 0x40, 0x02);
    qtest_clock_step(qts, 9600);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 11);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, 9600);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 11);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    before = qtest_readl(qts, RAD_BAR0 + 0x30);
    qtest_clock_step(qts, 10000);
    g_assert_cmpuint(qtest_readl(qts, RAD_BAR0 + 0x30) - before, >=, 3);
    qtest_system_reset(qts);
    octane_rad_map(qts);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0);
    qtest_quit(qts);
}

static void test_rad_ust_word_and_wrap(void)
{
    QTestState *qts = octane_start();
    const uint64_t device_reg = BRIDGE + 0x200 + 8 * 3 + 4;

    octane_rad_map(qts);
    g_assert_cmphex(qtest_readl(qts, device_reg) & (1u << 13), ==, 0);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, UINT64_C(0x01020304) * 960);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0x01020304);
    qtest_clock_step(qts, (UINT64_C(0xffffffff) - 0x01020304) * 960);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0xffffffff);
    qtest_clock_step(qts, 960);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, 0);
    qtest_quit(qts);
}

static void test_rad_ust_unsupported(void)
{
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *args = NULL;
    g_autofree char *log = NULL;
    int fd = g_file_open_tmp("octane-rad-ust-XXXXXX", &log_path, NULL);
    QTestState *qts;
    const uint64_t device_reg = BRIDGE + 0x200 + 8 * 3 + 4;
    uint8_t byte;
    uint16_t half;
    uint32_t count;
    uint64_t wide;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(log_path);
    args = g_strdup_printf("-d unimp -D %s", quoted);
    qts = octane_start_args(args);
    octane_rad_map(qts);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, 960);
    count = qtest_readl(qts, RAD_BAR0 + 0x30);
    g_assert_cmphex(count, ==, 1);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0e);
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x55);
    qtest_writel(qts, RAD_BAR0 + 0x30, 0x12345678);
    /*
     * Bridge posts writes but returns failed reads to the requester. The
     * RAD log and later enable check prove rejection without state changes.
     * Narrow CPU addresses correct back to the same BAR0 offset.
     */
    g_assert_cmpuint(qtest_writeb_tx(qts, RAD_BAR0 + 0x43, 0x0a), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readb_tx(qts, RAD_BAR0 + 0x43, &byte), ==,
                     MEMTX_ERROR);
    g_assert_cmpuint(qtest_writew_tx(qts, RAD_BAR0 + 0x42, 0x0a), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readw_tx(qts, RAD_BAR0 + 0x42, &half), ==,
                     MEMTX_ERROR);
    qtest_readl(qts, RAD_BAR0 + 0x40);
    /* A 64-bit Bridge cycle reaches RAD whole when Device(3) says 64-bit. */
    qtest_writel(qts, device_reg, qtest_readl(qts, device_reg) | (1u << 22));
    g_assert_cmpuint(qtest_writeq_tx(qts, RAD_BAR0 + 0x40,
                                     UINT64_C(0x1122334455667788)), ==,
                     MEMTX_OK);
    g_assert_cmpuint(qtest_readq_tx(qts, RAD_BAR0 + 0x40, &wide), ==,
                     MEMTX_ERROR);
    qtest_writel(qts, device_reg, qtest_readl(qts, device_reg) & ~(1u << 22));
    qtest_writel(qts, RAD_BAR0 + 0x40, 0x0a);
    qtest_clock_step(qts, 9600);
    g_assert_cmphex(qtest_readl(qts, RAD_BAR0 + 0x30), ==, count + 10);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 write at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 write at 0x30"));
    g_assert_nonnull(strstr(log, "unsupported 1-byte BAR0 write at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 1-byte BAR0 read at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 2-byte BAR0 write at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 2-byte BAR0 read at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 4-byte BAR0 read at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 8-byte BAR0 write at 0x40"));
    g_assert_nonnull(strstr(log, "unsupported 8-byte BAR0 read at 0x40"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_rad_ust_migration(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        g_autoptr(GError) error = NULL;
        g_autofree char *directory = g_dir_make_tmp("octane-rad-mig-XXXXXX",
                                                     &error);
        g_autofree char *socket = NULL;
        g_autofree char *uri = NULL;
        QTestState *from, *to;

        g_assert_no_error(error);
        socket = g_build_filename(directory, "migration.sock", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        from = octane_start();
        to = octane_start_args("-incoming defer");
        octane_rad_map(from);
        qtest_writel(from, RAD_BAR0 + 0x40, 0x0e);
        if (mode != 1) {
            qtest_writel(from, RAD_BAR0 + 0x40, 0x0a);
            qtest_clock_step(from, 1234);
        }
        if (mode == 2) {
            qtest_writel(from, RAD_BAR0 + 0x40, 0x02);
        }
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        g_assert_cmphex(qtest_readl(to, RAD_BAR0 + 0x30), ==,
                        mode == 1 ? 0 : 1);
        qtest_clock_step(to, mode == 0 ? 686 : 960);
        g_assert_cmphex(qtest_readl(to, RAD_BAR0 + 0x30), ==,
                        mode == 0 ? 2 : mode == 1 ? 0 : 1);
        if (mode == 1) {
            qtest_writel(to, RAD_BAR0 + 0x40, 0x0a);
            qtest_clock_step(to, 960);
            g_assert_cmphex(qtest_readl(to, RAD_BAR0 + 0x30), ==, 1);
        } else if (mode == 2) {
            qtest_writel(to, RAD_BAR0 + 0x40, 0x0e);
            qtest_writel(to, RAD_BAR0 + 0x40, 0x0a);
            qtest_clock_step(to, 686);
            g_assert_cmphex(qtest_readl(to, RAD_BAR0 + 0x30), ==, 2);
        }
        qtest_quit(to);
        qtest_quit(from);
        if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
            g_assert_cmpint(g_unlink(socket), ==, 0);
        }
        g_assert_cmpint(g_rmdir(directory), ==, 0);
    }
}

static void test_firmware_rejections(void)
{
    const char *absent[] = {
        qtest_qemu_binary(NULL), "-M", "octane", "-m", "128M",
        "-display", "none", NULL
    };
    const char *missing[] = {
        qtest_qemu_binary(NULL), "-M", "octane", "-m", "128M",
        "-bios", "/nonexistent/octane-prom", "-display", "none", NULL
    };
    g_autofree char *oversized = NULL;
    g_autofree char *error_output = NULL;
    int status;
    int fd = g_file_open_tmp("octane-oversized-XXXXXX", &oversized, NULL);
    const char *too_large[] = {
        qtest_qemu_binary(NULL), "-M", "octane", "-m", "128M",
        "-bios", oversized, "-display", "none", NULL
    };

    g_assert_true(g_spawn_sync(NULL, (char **)absent, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "requires -bios"));
    g_clear_pointer(&error_output, g_free);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 2 * 1024 * 1024 + 1), ==, 0);
    close(fd);
    g_assert_true(g_spawn_sync(NULL, (char **)missing, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "cannot read firmware image"));
    g_clear_pointer(&error_output, g_free);
    g_assert_true(g_spawn_sync(NULL, (char **)too_large, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "exceeds 2 MiB"));
    g_assert_cmpint(unlink(oversized), ==, 0);
}

static void test_pci_bar_separation(void)
{
    QTestState *qts = octane_start();
    const uint64_t scsi0 = BRIDGE + 0x200000;
    const uint64_t scsi1 = BRIDGE + 0x400000;

    qtest_writel(qts, CFG(0) + 0x10, 0xffffffff);
    qtest_writel(qts, CFG(1) + 0x10, 0xffffffff);
    qtest_writel(qts, CFG(0) + 0x14, 0xffffffff);
    qtest_writel(qts, CFG(1) + 0x14, 0xffffffff);
    qtest_writel(qts, CFG(2) + 0x10, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, CFG(0) + 0x10), ==, 0xffffff01);
    g_assert_cmphex(qtest_readl(qts, CFG(1) + 0x10), ==, 0xffffff01);
    g_assert_cmphex(qtest_readl(qts, CFG(0) + 0x14), ==, 0xffffff00);
    g_assert_cmphex(qtest_readl(qts, CFG(1) + 0x14), ==, 0xffffff00);
    g_assert_cmphex(qtest_readl(qts, CFG(2) + 0x10), ==, 0xfff00000);

    qtest_writel(qts, CFG(0) + 0x10, 0x00001001);
    qtest_writel(qts, CFG(1) + 0x10, 0x00002001);
    qtest_writel(qts, CFG(0) + 0x14, 0x10000000);
    qtest_writel(qts, CFG(1) + 0x14, 0x11000000);
    qtest_writel(qts, CFG(2) + 0x10, 0x12000000);
    for (unsigned slot = 0; slot < 3; slot++) {
        qtest_writel(qts, CFG(slot) + 0x04, 0x00000003);
        qtest_writel(qts, BRIDGE + 0x200 + 8 * slot + 4,
                     0x1100 + 0x10 * slot);
    }
    g_assert_cmphex(qtest_readl(qts, CFG(0) + 0x10), ==, 0x00001001);
    g_assert_cmphex(qtest_readl(qts, CFG(1) + 0x10), ==, 0x00002001);
    g_assert_cmphex(qtest_readl(qts, CFG(0) + 0x14), ==, 0x10000000);
    g_assert_cmphex(qtest_readl(qts, CFG(1) + 0x14), ==, 0x11000000);
    g_assert_cmphex(qtest_readl(qts, CFG(2) + 0x10), ==, 0x12000000);

    qtest_writew(qts, scsi0 + 0x0c, 1);
    g_assert_cmphex(qtest_readw(qts, scsi0 + 0x0c), ==, 1);
    g_assert_cmphex(qtest_readw(qts, scsi1 + 0x0c), ==, 0);
    qtest_writew(qts, scsi1 + 0x0c, 1);
    g_assert_cmphex(qtest_readw(qts, scsi1 + 0x0c), ==, 1);
    qtest_writeb(qts, UART_A + 7, 0x5a);
    g_assert_cmphex(qtest_readb(qts, UART_A + 7), ==, 0x5a);
    g_assert_cmphex(qtest_readw(qts, scsi0 + 0x0c), ==, 1);
    g_assert_cmphex(qtest_readw(qts, scsi1 + 0x0c), ==, 1);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x12345678);
    qtest_quit(qts);
}

static uint16_t octane_isp_ram_transfer(QTestState *qts, uint16_t command,
                                        uint32_t pci_address)
{
    uint16_t result;

    qtest_writew(qts, ISP_MAILBOX(4), 4);
    qtest_writew(qts, ISP_MAILBOX(6), 0);
    qtest_writew(qts, ISP_MAILBOX(7), 0);
    qtest_writew(qts, ISP_MAILBOX(0), command);
    qtest_writew(qts, ISP_MAILBOX(1), 0x2000);
    qtest_writew(qts, ISP_MAILBOX(2), pci_address >> 16);
    qtest_writew(qts, ISP_MAILBOX(3), pci_address);
    qtest_writew(qts, ISP_HOST_CMD, 0x5000);
    result = qtest_readw(qts, ISP_MAILBOX(0));
    qtest_writew(qts, ISP_HOST_CMD, 0x7000);
    qtest_writew(qts, SCSI0_WINDOW + 0x0e, 0);
    return result;
}

static void test_scsi_dma_to_heart_ram(void)
{
    QTestState *qts = octane_start();
    const uint8_t pattern[8] = { 0x10, 0x21, 0x32, 0x43,
                                 0x54, 0x65, 0x76, 0x87 };
    const uint8_t sentinel[8] = { 0xbb, 0xbb, 0xbb, 0xbb,
                                  0xbb, 0xbb, 0xbb, 0xbb };
    uint8_t seen[sizeof(pattern)];

    qtest_writel(qts, CFG(0) + 0x14, 0x10000000);
    qtest_writel(qts, CFG(0) + 0x04, 0x00000006);
    qtest_writel(qts, BRIDGE + 0x200 + 4, 0x1100);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    qtest_memwrite(qts, RAM + 0x5000, pattern, sizeof(pattern));
    memset(seen, 0, sizeof(seen));
    qtest_memwrite(qts, RAM + 0x6000, seen, sizeof(seen));
    qtest_memwrite(qts, RAM + 0x7000, sentinel, sizeof(sentinel));

    g_assert_cmphex(octane_isp_ram_transfer(qts, 1, 0xa0005000), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(octane_isp_ram_transfer(qts, 3, 0xa0006000), ==,
                    ISP_MBOX_COMPLETE);
    qtest_memread(qts, RAM + 0x6000, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), pattern, sizeof(pattern));

    qtest_writel(qts, BRIDGE_REG(0x080), 9u << 20);
    g_assert_cmphex(octane_isp_ram_transfer(qts, 3, 0xa0007000), ==,
                    ISP_MBOX_HOST_ERROR);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);
    g_assert_cmphex(octane_isp_ram_transfer(qts, 1, 0xa8005000), ==,
                    ISP_MBOX_HOST_ERROR);
    qtest_memread(qts, RAM + 0x7000, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), sentinel, sizeof(sentinel));
    qtest_quit(qts);
}

static uint16_t octane_isp_init_queue(QTestState *qts, uint16_t command,
                                      uint32_t pci_address)
{
    uint16_t result;

    qtest_writew(qts, ISP_MAILBOX(1), 4);
    qtest_writew(qts, ISP_MAILBOX(2), pci_address >> 16);
    qtest_writew(qts, ISP_MAILBOX(3), pci_address);
    qtest_writew(qts, ISP_MAILBOX(6), 0);
    qtest_writew(qts, ISP_MAILBOX(7), 0);
    qtest_writew(qts, ISP_MAILBOX(0), command);
    qtest_writew(qts, ISP_HOST_CMD, 0x5000);
    result = qtest_readw(qts, ISP_MAILBOX(0));
    qtest_writew(qts, ISP_HOST_CMD, 0x7000);
    qtest_writew(qts, SCSI0_WINDOW + 0x0e, 0);
    return result;
}

static void test_octane2_scsi_target1_read(void)
{
    g_autofree char *disk_path = g_strdup("octane-scsi-XXXXXX");
    g_autofree char *quoted_path;
    g_autofree char *args;
    QTestState *qts;
    const uint32_t request = 0xa0008000;
    const uint32_t response = 0xa0009000;
    const uint32_t buffer = 0xa000a000;
    const uint64_t before = RAM + 0x9e00;
    const uint64_t after = RAM + 0xa200;
    uint8_t entry[64] = { 0 };
    uint8_t status[64];
    uint8_t data[512];
    uint8_t sector[sizeof(data)];
    uint8_t untouched[sizeof(data)];
    int fd;

    for (size_t i = 0; i < sizeof(sector); i++) {
        sector[i] = i * 73 + 19;
    }
    fd = g_mkstemp(disk_path);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, sector, sizeof(sector)), ==, sizeof(sector));
    g_assert_cmpint(ftruncate(fd, 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted_path = g_shell_quote(disk_path);
    args = g_strdup_printf(
        "-drive id=disk0,if=none,file=%s,format=raw "
        "-device scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0",
        quoted_path);
    qts = octane2_start_args(args);

    qtest_writel(qts, CFG(0) + 0x14, 0x10000000);
    qtest_writel(qts, CFG(0) + 0x04, 0x00000006);
    /* Keep the PCI descriptors in their little-endian byte order. */
    qtest_writel(qts, BRIDGE + 0x200 + 4,
                 0x1100 | BRIDGE_SWAP_DIRECT);
    qtest_writel(qts, BRIDGE_REG(0x080), 8u << 20);

    memset(data, 0xa5, sizeof(data));
    memset(untouched, 0x5a, sizeof(untouched));
    qtest_memwrite(qts, RAM + 0xa000, data, sizeof(data));
    qtest_memwrite(qts, before, untouched, sizeof(untouched));
    qtest_memwrite(qts, after, untouched, sizeof(untouched));

    g_assert_cmphex(octane_isp_init_queue(qts, 0x0010, request), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(octane_isp_init_queue(qts, 0x0011, response), ==,
                    ISP_MBOX_COMPLETE);

    entry[0] = 0x01; /* Command entry, with no continuation. */
    entry[1] = 1;
    stl_le_p(entry + 4, 0x12345678);
    stw_le_p(entry + 8, 1 << 8); /* Bus 0, target 1, LUN 0. */
    stw_le_p(entry + 10, 6);
    qtest_memwrite(qts, RAM + 0x8000, entry, sizeof(entry));
    qtest_writew(qts, ISP_MAILBOX(4), 1);
    g_assert_cmphex(qtest_readw(qts, ISP_MAILBOX(5)), ==, 1);
    qtest_writew(qts, ISP_HOST_CMD, 0x7000);

    memset(entry, 0, sizeof(entry));
    entry[0] = 0x01;
    entry[1] = 1;
    stl_le_p(entry + 4, 0x07654321);
    stw_le_p(entry + 8, 1 << 8);
    stw_le_p(entry + 10, 10);
    stw_le_p(entry + 18, 1);
    entry[20] = 0x28; /* READ(10), LBA 0, one 512-byte block. */
    entry[28] = 1;
    stl_le_p(entry + 32, buffer);
    stl_le_p(entry + 36, sizeof(data));
    qtest_memwrite(qts, RAM + 0x8040, entry, sizeof(entry));
    qtest_writew(qts, ISP_MAILBOX(4), 2);

    /* File-backed READ(10) can complete after the request doorbell. */
    for (unsigned i = 0; i < 1000 &&
         qtest_readw(qts, ISP_MAILBOX(5)) != 2; i++) {
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readw(qts, ISP_MAILBOX(5)), ==, 2);
    qtest_memread(qts, RAM + 0x9040, status, sizeof(status));
    g_assert_cmphex(status[0], ==, 0x03);
    g_assert_cmphex(ldl_le_p(status + 4), ==, 0x07654321);
    g_assert_cmphex(lduw_le_p(status + 8), ==, 0); /* SCSI GOOD. */
    g_assert_cmphex(lduw_le_p(status + 10), ==, 0); /* IOCB complete. */
    qtest_memread(qts, RAM + 0xa000, data, sizeof(data));
    g_assert_cmpmem(data, sizeof(data), sector, sizeof(sector));
    qtest_memread(qts, before, data, sizeof(data));
    g_assert_cmpmem(data, sizeof(data), untouched, sizeof(untouched));
    qtest_memread(qts, after, data, sizeof(data));
    g_assert_cmpmem(data, sizeof(data), untouched, sizeof(untouched));
    qtest_quit(qts);
    g_assert_cmpint(unlink(disk_path), ==, 0);
}

static void test_uncached_attribute_zero(void)
{
    static const uint32_t code[] = {
        0x400a6000, 0x354a0080, 0x408a6000, /* Status.KX = 1 */
        0, 0, 0, 0,
        0x3c089000, 0x0008403c,             /* t0 = 0x9000000000000000 */
        0x3c091f60, 0x35290028, 0x01094025, /* t0 |= IOC3 + 0x28 */
        0x8d0b0000,                         /* lw t3, 0(t0) */
        0x3c0c9600, 0x000c603c,             /* attr 3, ordinary RAM */
        0x3c0d2000, 0x35ad4000, 0x018d6025,
        0xad8b0000,                         /* RAM[0x4000] = t3 */
        0x3c089000, 0x0008403c,
        0x3c091800, 0x35290004, 0x01094025,
        0x8d0b0000,                         /* lw t3, HEART widget ID */
        0xad8b0004,                         /* RAM[0x4004] = t3 */
        0x1000ffff, 0,
    };
    static const uint32_t handler[] = {
        0x3c0c9600, 0x000c603c, 0x3c0d2000, 0x35ad4000,
        0x018d6025, 0x340bdead, 0xad8b0000, 0xad8b0004,
        0x1000ffff, 0,
    };
    uint8_t image[0x400];
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    uint32_t result;
    uint32_t missing;
    int fd;

    memset(image, 0xff, sizeof(image));
    for (unsigned i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + 4 * i, code[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(handler); i++) {
        stl_be_p(image + 0x380 + 4 * i, handler[i]);
    }
    fd = g_file_open_tmp("octane-xkphys-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    qts = qtest_initf("-M octane -m 128M -cpu R10000 -bios %s "
                      "-accel tcg,thread=single -S", path);
    g_assert_cmpint(unlink(path), ==, 0);

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x28), ==, 0x00400000);
    qtest_writel(qts, RAM + 0x4000, 0xdeadbeef);
    qtest_writel(qts, RAM + 0x4004, 0xdeadbeef);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    for (unsigned i = 0; i < 10000; i++) {
        result = qtest_readl(qts, RAM + 0x4000);
        if (result != 0xdeadbeef) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(result, ==, 0x00400000);
    for (unsigned i = 0; i < 10000; i++) {
        result = qtest_readl(qts, RAM + 0x4004);
        if (result != 0xdeadbeef) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(result & 0x0fffffff, ==, 0x0c00106d);
    g_assert_cmphex(qtest_readl(qts, ATTR0 + IOC3 + 0x28), ==,
                    qtest_readl(qts, IOC3 + 0x28));
    g_assert_cmphex(qtest_readl(qts, ATTR0 + FLASH), ==,
                    qtest_readl(qts, FLASH));
    qtest_writel(qts, ATTR0 + RAM + 0x5000, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x5000), ==, 0x12345678);
    g_assert_cmpuint(qtest_readl_tx(qts, ATTR0 + 0x30000000, &missing), !=,
                     MEMTX_OK);
    qtest_quit(qts);
}

static void test_rtc_bytebus(void)
{
    QTestState *qts = octane_start();

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);

    qtest_writeb(qts, RTC_INDEX, 0x0d);
    g_assert_cmphex(qtest_readb(qts, RTC_DATA) & 0x80, ==, 0x80);
    qtest_writeb(qts, RTC_INDEX, 0x0e);
    qtest_writeb(qts, RTC_DATA, 0x5a);
    g_assert_cmphex(qtest_readb(qts, RTC_DATA), ==, 0x5a);
    qtest_writeb(qts, RTC_INDEX, 0x0f);
    qtest_writeb(qts, RTC_DATA, 0xa5);
    qtest_writeb(qts, RTC_INDEX, 0x0e);
    g_assert_cmphex(qtest_readb(qts, RTC_DATA), ==, 0x5a);
    qtest_writeb(qts, ATTR0 + RTC_INDEX, 0x0e);
    g_assert_cmphex(qtest_readb(qts, ATTR0 + RTC_DATA), ==, 0x5a);
    qtest_system_reset(qts);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, RTC_INDEX, 0x0e);
    g_assert_cmphex(qtest_readb(qts, RTC_DATA), ==, 0x5a);
    qtest_quit(qts);
}

static void test_heart_interrupt_levels(void)
{
    QTestState *qts = octane_start();
    const uint64_t level0 = 1;
    const uint64_t level1 = 1ULL << 16;
    const uint64_t level2 = 1ULL << 32;

    qtest_irq_intercept_out_named(qts, "/machine/heart", "processor");
    qtest_writeq(qts, HEART + 0x10020, level0 | level1 | level2);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==,
                    level0 | level1 | level2);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_false(qtest_get_irq(qts, 2));
    qtest_writeq(qts, HEART + 0x10000, level0 | level1 | level2);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_true(qtest_get_irq(qts, 2));
    qtest_writeq(qts, HEART + 0x10028, level1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_true(qtest_get_irq(qts, 2));
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==,
                    level0 | level2);
    qtest_writeq(qts, HEART + 0x10028, level0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 2));
    qtest_writeq(qts, HEART + 0x10000, 0);
    g_assert_false(qtest_get_irq(qts, 2));
    qtest_quit(qts);
}

static void test_serial_a_loopback(void)
{
    QTestState *qts = octane_start();
    const uint64_t uart = BRIDGE + 0x600000 + 0x20178;

    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, uart + 4, 0x10);
    qtest_writeb(qts, uart, 0x5a);
    g_assert_cmphex(qtest_readb(qts, uart), ==, 0x5a);
    qtest_quit(qts);
}

static void test_serial_a_chardev(void)
{
    int sock;
    QTestState *qts = octane_start_serial(&sock);
    const uint8_t sent = 'I';
    uint8_t received;
    struct timeval deadline = { .tv_sec = 5 };

    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, UART_A + 3, 0x03);
    qtest_writeb(qts, UART_A + 1, 0x01);
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);

    g_test_timer_start();
    while (!(qtest_readb(qts, UART_A + 5) & 1)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 5.0);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);
    qtest_writel(qts, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x20) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);

    g_assert_cmphex(qtest_readb(qts, UART_A), ==, sent);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==, 0);

    qtest_writeb(qts, UART_A, 'O');
    g_assert_cmpint(read(sock, &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 'O');
    close(sock);
    qtest_quit(qts);
}

static void test_serial_b(const void *data)
{
    bool octane2 = GPOINTER_TO_INT(data);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t addrlen = sizeof(addr);
    struct timeval deadline = { .tv_sec = 5 };
    g_autofree char *quoted = g_shell_quote(flash_path);
    g_autofree char *args = NULL;
    QTestState *qts;
    uint8_t received;
    int listener, sock;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_assert_cmpint(listener, >=, 0);
    g_assert_cmpint(bind(listener, (struct sockaddr *)&addr, sizeof(addr)),
                    ==, 0);
    g_assert_cmpint(getsockname(listener, (struct sockaddr *)&addr,
                                &addrlen), ==, 0);
    g_assert_cmpint(listen(listener, 1), ==, 0);
    args = g_strdup_printf(
        "-M %s -m %s -cpu %s -bios %s -S -serial none "
        "-chardev socket,id=portb,host=127.0.0.1,port=%u "
        "-serial chardev:portb",
        octane2 ? "octane2" : "octane", octane2 ? "256M" : "128M",
        octane2 ? OCTANE2_CPU : "R10000", quoted, ntohs(addr.sin_port));
    qts = qtest_init(args);

    g_assert_cmpint(setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    sock = accept(listener, NULL, NULL);
    g_assert_cmpint(sock, >=, 0);
    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writeb(qts, UART_B + 4, 0x10);
    qtest_writeb(qts, UART_B, 0x5a);
    g_assert_cmphex(qtest_readb(qts, UART_B), ==, 0x5a);
    qtest_writeb(qts, UART_B + 4, 0);
    qtest_writeb(qts, UART_B + 3, 3);
    qtest_writeb(qts, UART_B, 'B');
    g_assert_cmpint(read(sock, &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 'B');
    close(sock);
    close(listener);
    qtest_quit(qts);
}

static void test_serial_a_interrupt_route(void)
{
    int sock;
    QTestState *qts = octane_start_serial(&sock);
    const uint8_t sent = 'R';
    struct timeval deadline = { .tv_sec = 5 };

    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                               &deadline, sizeof(deadline)), ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, BRIDGE_REG(0x030), 8u << 16);
    qtest_writel(qts, BRIDGE_REG(0x038), 0x80);
    qtest_writel(qts, BRIDGE_REG(0x130 + 8 * 4), 16);
    qtest_writel(qts, BRIDGE_REG(0x118), SUPERIO_PIN);
    qtest_writeb(qts, UART_A + 3, 0x03);
    qtest_writeb(qts, UART_A + 1, 0x01);
    g_assert_cmpint(write(sock, &sent, 1), ==, 1);

    g_test_timer_start();
    while (!(qtest_readb(qts, UART_A + 5) & 1)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 5.0);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==, 0);
    qtest_writel(qts, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==,
                    SUPERIO_PIN);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    qtest_writel(qts, IOC3 + 0x24, IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==, 0);
    qtest_writel(qts, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    qtest_writel(qts, BRIDGE_REG(0x108), SUPERIO_PIN);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & SUPERIO_VECTOR, ==,
                    SUPERIO_VECTOR);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==, 0);
    qtest_writeq(qts, HEART + 0x10000, SUPERIO_VECTOR);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);
    qtest_writeq(qts, HEART + 0x10000, 0);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030) & SUPERIO_VECTOR, ==,
                    SUPERIO_VECTOR);
    qtest_writeq(qts, HEART + 0x10000, SUPERIO_VECTOR);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);
    qtest_writeq(qts, HEART + 0x10020, 1u << 3);

    g_assert_cmphex(qtest_readb(qts, UART_A), ==, sent);
    g_assert_cmphex(qtest_readl(qts, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 1u << 3);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==, 0);
    close(sock);
    qtest_quit(qts);
}

static void test_reset_state(void)
{
    QTestState *qts = octane_start();

    qtest_writel(qts, HEART + 0x20, WIDE);
    qtest_writel(qts, HEART_WIDGET + 0x24, 0x3ca596f0);
    qtest_writeq(qts, HEART + 0x10000, SUPERIO_VECTOR);
    qtest_writeq(qts, HEART + 0x10020, SUPERIO_VECTOR);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(qts, CFG(3) + 0x10, RAD_PCI_BAR0);
    qtest_writel(qts, CFG(3) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 3 + 4, 0x1130);
    qtest_writeb(qts, UART_A + 7, 0x5a);
    g_assert_cmphex(qtest_readb(qts, UART_A + 7), ==, 0x5a);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, HEART + 0x20), ==, BANK0);
    g_assert_cmphex(qtest_readl(qts, HEART_WIDGET + 0x24), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ATTR0 + HEART + 0x20), ==, BANK0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HEART + 0x10030), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x108)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(2) + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG(3)), ==, RAD_PCI_ID);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 8) & 0xff, ==, RAD_REVISION);
    g_assert_cmphex(qtest_readl(qts, CFG(3) + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    qtest_writel(qts, CFG(2) + 0x10, 0x10000000);
    qtest_writel(qts, CFG(2) + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    g_assert_cmphex(qtest_readb(qts, UART_A + 7), ==, 0);
    g_assert_cmphex(qtest_readb(qts, ATTR0 + UART_A + 7), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FLASH), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, ATTR0 + FLASH), ==, 0x12345678);
    g_assert_cmphex(octane_cpu_pc(qts), ==, 0xffffffffbfc00000ULL);
    g_assert_cmphex(octane_cpu_cause(qts) & SUPERIO_CAUSE, ==, 0);
    qtest_quit(qts);
}

static void test_memory_population_migration(void)
{
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *quoted = g_shell_quote(flash_path);
    GError *error = NULL;
    QTestState *from, *to;
    const char *machine =
        "octane,bank0-size=67108864,bank1-size=67108864";

    directory = g_dir_make_tmp("octane-banks-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = qtest_initf("-M %s -m 128M -cpu R10000 -bios %s -S",
                       machine, quoted);
    to = qtest_initf("-M %s -m 128M -cpu R10000 -bios %s -S "
                     "-incoming defer", machine, quoted);

    qtest_writeq(from, HIGH_RAM, 0x1111222233334444ULL);
    qtest_writeq(from, RAM + 64 * MiB - 8, 0x5555666677778888ULL);
    qtest_writeq(from, RAM + 64 * MiB, 0x9999aaaabbbbccccULL);
    qtest_writeq(from, RAM + 128 * MiB - 8, 0xddddeeeeffff0000ULL);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(qtest_readl(to, HEART + 0x20), ==, 0x80010000U);
    g_assert_cmphex(qtest_readl(to, HEART + 0x24), ==, 0x80010002U);
    g_assert_cmphex(qtest_readq(to, HIGH_RAM), ==,
                    0x1111222233334444ULL);
    g_assert_cmphex(qtest_readq(to, RAM + 64 * MiB - 8), ==,
                    0x5555666677778888ULL);
    g_assert_cmphex(qtest_readq(to, RAM + 64 * MiB), ==,
                    0x9999aaaabbbbccccULL);
    g_assert_cmphex(qtest_readq(to, RAM + 128 * MiB - 8), ==,
                    0xddddeeeeffff0000ULL);
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_migration_state(void)
{
    uint8_t low_pattern[LOW_RAM_SIZE];
    uint8_t low_seen[LOW_RAM_SIZE];
    uint8_t low_zero[LOW_RAM_SIZE] = { 0 };
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *other_flash = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int flash_fd;

    directory = g_dir_make_tmp("octane-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    other_flash = g_build_filename(directory, "other-flash", NULL);
    flash_fd = g_open(other_flash, O_WRONLY | O_CREAT | O_EXCL, 0600);
    g_assert_cmpint(flash_fd, >=, 0);
    g_assert_cmpint(write(flash_fd, "\xaa\xbb\xcc\xdd", 4), ==, 4);
    close(flash_fd);
    from = octane_start();
    to = octane_start_image_args(other_flash, "-incoming defer");
    g_assert_cmphex(qtest_readl(to, FLASH), ==, 0xaabbccdd);

    for (unsigned i = 0; i < LOW_RAM_SIZE; i++) {
        low_pattern[i] = (i * 37 + (i >> 8) * 13) % 255 + 1;
    }
    qtest_memwrite(from, 0, low_pattern, sizeof(low_pattern));
    g_assert_cmpuint(qtest_writew_tx(from, MPCONF + 0x14, 0xa5b6),
                     ==, MEMTX_OK);
    low_pattern[MPCONF + 0x14] = 0xa5;
    low_pattern[MPCONF + 0x15] = 0xb6;
    qtest_memread(from, 0, low_seen, sizeof(low_seen));
    g_assert_cmpmem(low_seen, sizeof(low_seen), low_pattern,
                    sizeof(low_pattern));

    qtest_writel(from, HEART + 0x20, WIDE);
    qtest_writel(from, HEART_WIDGET + 0x24, 0x3ca596f0);
    qtest_writel(from, HEART_WIDGET + 0x2c, 0x12345678);
    qtest_writeq(from, HEART_WIDGET + 0x28, 0x991b2837a5c37e19ULL);
    qtest_writeq(from, HEART_WIDGET + 0x68, 0x3ff62c07);
    qtest_writeq(from, HEART + 0x50, 0xe524351324f105a7ULL);
    qtest_writeq(from, HEART + 0x58, 0x1adbc29db3c7e85aULL);
    qtest_writeq(from, HEART + 0xb0, 0x71938f76a5c37f7fULL);
    qtest_writeq(from, HEART + 0x30000, 0xfedcba9800246aceULL);
    qtest_writeq(from, HEART + 0x10008, 0x1234567887654321ULL);
    qtest_writeq(from, HIGH_RAM, 0x0123456789abcdefULL);
    qtest_writeq(from, HIGH_RAM + 8, 0xfedcba9876543210ULL);
    qtest_writel(from, CFG(2) + 0x10, 0x10000000);
    qtest_writel(from, CFG(2) + 0x04, 0x00000002);
    qtest_writel(from, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_writel(from, CFG(3) + 0x10, RAD_PCI_BAR0);
    qtest_writel(from, CFG(3) + 0x04, 0x00000002);
    qtest_writel(from, BRIDGE + 0x200 + 8 * 3 + 4, 0x1130);
    qtest_writeb(from, UART_A + 7, 0x5a);
    qtest_writel(from, BRIDGE_REG(0x030), 8u << 16);
    qtest_writel(from, BRIDGE_REG(0x038), 0x80);
    qtest_writel(from, BRIDGE_REG(0x130 + 8 * 4), 16);
    qtest_writel(from, BRIDGE_REG(0x118), SUPERIO_PIN);
    qtest_writel(from, IOC3 + 0x20, IOC3_SERIAL_IRQ);
    qtest_writel(from, BRIDGE_REG(0x108), SUPERIO_PIN);
    qtest_writeq(from, HEART + 0x10000, SUPERIO_VECTOR);
    qtest_writeb(from, UART_A + 3, 0x03);
    qtest_writeb(from, UART_A + 1, 0x01);
    qtest_writeb(from, UART_A + 4, 0x10);
    qtest_writeb(from, UART_A, 0xa5);
    g_assert_cmphex(qtest_readb(from, UART_A + 5) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(from, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);
    g_assert_cmphex(octane_cpu_cause(from) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);

    qtest_memread(to, 0, low_seen, sizeof(low_seen));
    g_assert_cmpmem(low_seen, sizeof(low_seen), low_pattern,
                    sizeof(low_pattern));
    g_assert_cmphex(qtest_readw(to, MPCONF + 0x14), ==, 0xa5b6);

    g_assert_cmphex(qtest_readl(to, HEART + 0x20), ==, WIDE);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x24), ==, 0x3ca596f0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x28), ==, 0xa5c37e19);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x2c), ==, 0xa5c37e19);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x44), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x68), ==, 0x3ff62c07);
    g_assert_cmphex(qtest_readq(to, HEART + 0x50), ==,
                    0xe524351324f105a7ULL);
    g_assert_cmphex(qtest_readq(to, HEART + 0x58), ==,
                    0x1adbc29db3c7e85aULL);
    g_assert_cmphex(qtest_readq(to, HEART + 0xb0), ==,
                    0x71938f76a5c37f7fULL);
    g_assert_cmphex(qtest_readq(to, HEART + 0x30000), ==, 0x246ace);
    g_assert_cmphex(qtest_readq(to, HEART + 0x10008), ==,
                    0x1234567887654321ULL);
    g_assert_cmphex(qtest_readq(to, HIGH_RAM), ==, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(to, ATTR0 + HIGH_RAM), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(to, HIGH_RAM + 4 * UNIT + 8), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(to, HEART + 0x10000), ==, SUPERIO_VECTOR);
    g_assert_cmphex(qtest_readq(to, HEART + 0x10030) & SUPERIO_VECTOR, ==,
                    SUPERIO_VECTOR);
    g_assert_cmphex(octane_cpu_cause(to) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);
    qtest_writeq(to, HEART + 0x10000, 0);
    g_assert_cmphex(octane_cpu_cause(to) & SUPERIO_CAUSE, ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART + 0x10030) & SUPERIO_VECTOR, ==,
                    SUPERIO_VECTOR);
    qtest_writeq(to, HEART + 0x10000, SUPERIO_VECTOR);
    g_assert_cmphex(octane_cpu_cause(to) & SUPERIO_CAUSE, ==,
                    SUPERIO_CAUSE);
    g_assert_cmphex(qtest_readl(to, CFG(2) + 0x10), ==, 0x10000000);
    g_assert_cmphex(qtest_readl(to, CFG(3)), ==, RAD_PCI_ID);
    g_assert_cmphex(qtest_readl(to, CFG(3) + 8) & 0xff, ==, RAD_REVISION);
    g_assert_cmphex(qtest_readl(to, CFG(3) + 0x10), ==, RAD_PCI_BAR0);
    g_assert_cmphex(qtest_readl(to, BRIDGE_REG(0x100)) & RAD_PIN, ==, 0);
    g_assert_cmphex(qtest_readb(to, UART_A + 7), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, ATTR0 + UART_A + 7), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, UART_A + 5) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(to, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==,
                    IOC3_SERIAL_IRQ);
    g_assert_cmphex(qtest_readl(to, BRIDGE_REG(0x100)) & SUPERIO_PIN, ==,
                    SUPERIO_PIN);
    g_assert_cmphex(qtest_readb(to, UART_A), ==, 0xa5);
    g_assert_cmphex(qtest_readl(to, IOC3 + 0x1c) & IOC3_SERIAL_IRQ, ==, 0);
    g_assert_cmphex(octane_cpu_cause(to) & SUPERIO_CAUSE, ==, 0);
    g_assert_cmphex(qtest_readl(to, FLASH), ==, 0x12345678);

    qtest_system_reset(to);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x24), ==, 0);
    g_assert_cmphex(qtest_readl(to, HEART_WIDGET + 0x2c), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x28), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART_WIDGET + 0x68), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART + 0x50), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART + 0x58), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART + 0xb0), ==, 0);
    g_assert_cmphex(qtest_readq(to, HEART + 0x30000), ==, 0);
    qtest_memread(to, 0, low_seen, sizeof(low_seen));
    g_assert_cmpmem(low_seen, sizeof(low_seen), low_zero, sizeof(low_zero));

    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_unlink(other_flash), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_octane2_low_ram_migration(void)
{
    uint8_t pattern[LOW_RAM_SIZE];
    uint8_t seen[LOW_RAM_SIZE];
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    uint8_t byte;

    directory = g_dir_make_tmp("octane2-low-ram-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = octane2_start_args("");
    to = octane2_start_args("-incoming defer");

    for (unsigned i = 0; i < LOW_RAM_SIZE; i++) {
        pattern[i] = (i * 37 + (i >> 8) * 13) % 255 + 1;
    }
    qtest_memwrite(from, 0, pattern, sizeof(pattern));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);

    qtest_memread(to, 0, seen, sizeof(seen));
    g_assert_cmpmem(seen, sizeof(seen), pattern, sizeof(pattern));
    g_assert_cmpuint(qtest_readb_tx(to, LOW_RAM_SIZE - 1, &byte), ==,
                     MEMTX_OK);
    g_assert_cmphex(byte, ==, pattern[LOW_RAM_SIZE - 1]);
    qtest_system_reset(to);
    qtest_memread(to, 0, seen, sizeof(seen));
    for (unsigned i = 0; i < sizeof(seen); i++) {
        g_assert_cmphex(seen[i], ==, 0);
    }
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    int fd;
    const uint8_t bytes[] = { 0x12, 0x34, 0x56, 0x78 };

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("octane-flash-XXXXXX", &flash_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, bytes, sizeof(bytes)), ==, sizeof(bytes));
    close(fd);
    qtest_add_func("/octane/widgets-and-memory", test_widgets_and_memory);
    qtest_add_data_func("/octane/cpu-clock", GINT_TO_POINTER(0),
                        test_cpu_clock);
    qtest_add_data_func("/octane2/cpu-clock", GINT_TO_POINTER(1),
                        test_cpu_clock);
    qtest_add_func("/octane2/population", test_octane2_population);
    qtest_add_data_func("/octane/board-identities", GINT_TO_POINTER(0),
                        test_board_identities);
    qtest_add_data_func("/octane2/board-identities", GINT_TO_POINTER(1),
                        test_board_identities);
    qtest_add_data_func("/octane/board-identity-migration", GINT_TO_POINTER(0),
                        test_board_identity_migration);
    qtest_add_data_func("/octane2/board-identity-migration", GINT_TO_POINTER(1),
                        test_board_identity_migration);
    qtest_add_func("/octane/memory-populations", test_memory_populations);
    qtest_add_func("/octane/memory-population-rejections",
                   test_memory_population_rejections);
    qtest_add_func("/octane2/ioc3-ethernet", test_octane2_ethernet);
    qtest_add_func("/octane/ioc3-ethernet", test_octane_ethernet);
    qtest_add_data_func("/octane/ioc3-parallel", GINT_TO_POINTER(0),
                        test_ioc3_parallel);
    qtest_add_data_func("/octane2/ioc3-parallel", GINT_TO_POINTER(1),
                        test_ioc3_parallel);
    qtest_add_data_func("/octane/ioc3-parallel-fifo", GINT_TO_POINTER(0),
                        test_ioc3_parallel_fifo);
    qtest_add_data_func("/octane2/ioc3-parallel-fifo", GINT_TO_POINTER(1),
                        test_ioc3_parallel_fifo);
    qtest_add_func("/octane/ioc3-ethernet-polled",
                   test_octane_ethernet_polled);
    qtest_add_func("/octane2/ioc3-ethernet-polled",
                   test_octane2_ethernet_polled);
    qtest_add_func("/octane/ioc3-ethernet-dma-and-interrupt",
                   test_octane_ethernet_dma_and_interrupt);
    qtest_add_func("/octane2/ioc3-ethernet-dma-and-interrupt",
                   test_octane2_ethernet_dma_and_interrupt);
    qtest_add_func("/octane2/serial", test_octane2_serial);
    qtest_add_func("/octane/heart-widget-control", test_heart_widget_control);
    qtest_add_func("/octane/heart-widget-errors", test_heart_widget_errors);
    qtest_add_data_func("/octane/heart-diagnostic-storage", GINT_TO_POINTER(0),
                        test_heart_diagnostic_storage);
    qtest_add_data_func("/octane2/heart-diagnostic-storage", GINT_TO_POINTER(1),
                        test_heart_diagnostic_storage);
    qtest_add_func("/octane/heart-divider-onewire-time",
                   test_heart_divider_onewire_time);
    qtest_add_data_func("/octane/low-ram", GINT_TO_POINTER(0), test_low_ram);
    qtest_add_data_func("/octane2/low-ram", GINT_TO_POINTER(1), test_low_ram);
    qtest_add_data_func("/octane/heart-identity", GINT_TO_POINTER(0),
                        test_heart_identity);
    qtest_add_data_func("/octane2/heart-identity", GINT_TO_POINTER(1),
                        test_heart_identity);
    qtest_add_func("/octane2/heart-widget-status",
                   test_octane2_heart_widget_status);
    qtest_add_func("/octane/flash-and-pci", test_flash_and_pci);
    qtest_add_data_func("/octane/alternate-flash", GINT_TO_POINTER(0),
                        test_alternate_flash);
    qtest_add_data_func("/octane2/alternate-flash", GINT_TO_POINTER(1),
                        test_alternate_flash);
    qtest_add_data_func("/octane/sharp-select0-strap0", GUINT_TO_POINTER(0),
                        test_sharp_flash);
    qtest_add_data_func("/octane/sharp-select0-strap1", GUINT_TO_POINTER(1),
                        test_sharp_flash);
    qtest_add_data_func("/octane/sharp-select1-strap0", GUINT_TO_POINTER(2),
                        test_sharp_flash);
    qtest_add_data_func("/octane/sharp-select1-strap1", GUINT_TO_POINTER(3),
                        test_sharp_flash);
    qtest_add_func("/octane/sharp-view-transitions",
                   test_sharp_flash_view_transitions);
    qtest_add_func("/octane/sharp-unrealize-access",
                   test_sharp_flash_unrealize_access);
    qtest_add_func("/octane/sharp-initial-rp", test_sharp_flash_initial_rp);
    qtest_add_data_func("/octane/sharp-execute", GUINT_TO_POINTER(0),
                        test_sharp_flash_execute);
    qtest_add_data_func("/octane2/sharp-execute-icount", GUINT_TO_POINTER(1),
                        test_sharp_flash_execute);
    qtest_add_data_func("/octane/sharp-execute-cached", GUINT_TO_POINTER(0),
                        test_sharp_flash_cached_execute);
    qtest_add_data_func("/octane2/sharp-execute-repeat-cached",
                        GUINT_TO_POINTER(1), test_sharp_flash_cached_execute);
    qtest_add_func("/octane/sharp-persistent-migration-rejected",
                   test_sharp_flash_migration_rejected);
    qtest_add_func("/octane/sharp-persistent-restore-rejected",
                   test_sharp_flash_restore_rejected);
    qtest_add_func("/octane/sharp-storage-rejections",
                   test_sharp_flash_storage_rejections);
    qtest_add_data_func("/octane/sharp-raw-size-rejected", GUINT_TO_POINTER(0),
                        test_sharp_flash_raw_subregion);
    qtest_add_data_func("/octane/sharp-raw-offset-rejected",
                        GUINT_TO_POINTER(1),
                        test_sharp_flash_raw_subregion);
    qtest_add_data_func("/octane/sharp-failed-unrealize", "failed_unrealize",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-backend-write-error", "write_aio",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-backend-flush-error", "flush_to_disk",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-directory-create-sync-error",
                        "dirsync_create", test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-directory-remove-sync-error",
                        "dirsync_remove", test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-marker-restoration-lost",
                        "dirsync_lost_marker", test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-marker-create-error", "marker_create",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-marker-remove-error", "marker_remove",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-backend-partial-erase", "partial_erase",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-final-flush-error", "final_flush",
                        test_sharp_flash_backend_error);
    qtest_add_data_func("/octane/sharp-unrealize-busy", GUINT_TO_POINTER(3),
                        test_sharp_flash_shutdown_pending);
    qtest_add_data_func("/octane/sharp-shutdown-program", GUINT_TO_POINTER(2),
                        test_sharp_flash_shutdown_pending);
    qtest_add_data_func("/octane/sharp-shutdown-busy", GUINT_TO_POINTER(0),
                        test_sharp_flash_shutdown_pending);
    qtest_add_data_func("/octane/sharp-shutdown-suspended", GUINT_TO_POINTER(1),
                        test_sharp_flash_shutdown_pending);
    qtest_add_func("/octane/rad-pci", test_rad_pci);
    qtest_add_data_func("/octane/rad-pci-identity", GINT_TO_POINTER(0),
                        test_rad_pci_identity);
    qtest_add_data_func("/octane2/rad-pci-identity", GINT_TO_POINTER(1),
                        test_rad_pci_identity);
    qtest_add_func("/octane/rad-startup-storage", test_rad_startup_storage);
    qtest_add_func("/octane/rad-startup-unsupported",
                   test_rad_startup_unsupported);
    qtest_add_func("/octane/rad-startup-running", test_rad_startup_running);
    qtest_add_func("/octane/rad-subcode-width-swap",
                   test_rad_subcode_width_swap);
    qtest_add_func("/octane/rad-status-direct", test_rad_status_direct);
    qtest_add_data_func("/octane/rad-status-primary-only", GINT_TO_POINTER(0),
                        test_rad_status_primary_only);
    qtest_add_data_func("/octane/rad-status-unrelated-secondary",
                        GINT_TO_POINTER(1), test_rad_status_primary_only);
    qtest_add_func("/octane/rad-status-routes", test_rad_status_routes);
    qtest_add_func("/octane/rad-output-clock", test_rad_output_clock);
    qtest_add_func("/octane/rad-status-continuous", test_rad_status_continuous);
    qtest_add_func("/octane/rad-status-overrun", test_rad_status_overrun);
    qtest_add_func("/octane/rad-transfer-migration",
                   test_rad_transfer_migration);
    qtest_add_func("/octane/rad-output-samples", test_rad_output_samples);
    qtest_add_data_func("/octane/rad-output-tail", GINT_TO_POINTER(0),
                        test_rad_output_tail);
    qtest_add_data_func("/octane/rad-output-tail-read-position",
                        GINT_TO_POINTER(1), test_rad_output_tail);
    qtest_add_func("/octane/rad-transfer-failures", test_rad_transfer_failures);
    qtest_add_func("/octane/rad-transfer-unsupported",
                   test_rad_transfer_unsupported);
    qtest_add_func("/octane/rad-ust-clock", test_rad_ust_clock);
    qtest_add_func("/octane/rad-ust-word-and-wrap",
                   test_rad_ust_word_and_wrap);
    qtest_add_func("/octane/rad-ust-unsupported",
                   test_rad_ust_unsupported);
    qtest_add_func("/octane/rad-ust-migration",
                   test_rad_ust_migration);
    qtest_add_func("/octane/firmware-rejections", test_firmware_rejections);
    qtest_add_func("/octane/pci-bar-separation", test_pci_bar_separation);
    qtest_add_func("/octane/scsi-dma-to-heart-ram",
                   test_scsi_dma_to_heart_ram);
    qtest_add_func("/octane2/scsi-target1-read",
                   test_octane2_scsi_target1_read);
    qtest_add_func("/octane/uncached-attribute-zero",
                   test_uncached_attribute_zero);
    qtest_add_func("/octane/rtc-bytebus", test_rtc_bytebus);
    qtest_add_func("/octane/serial-a-loopback", test_serial_a_loopback);
    qtest_add_func("/octane/serial-a-chardev", test_serial_a_chardev);
    qtest_add_data_func("/octane/serial-b", GINT_TO_POINTER(0), test_serial_b);
    qtest_add_data_func("/octane2/serial-b", GINT_TO_POINTER(1), test_serial_b);
    qtest_add_func("/octane/serial-a-interrupt-route",
                   test_serial_a_interrupt_route);
    qtest_add_func("/octane/heart-interrupt-levels",
                   test_heart_interrupt_levels);
    qtest_add_func("/octane/reset-state", test_reset_state);
    qtest_add_func("/octane/memory-population-migration",
                   test_memory_population_migration);
    qtest_add_func("/octane/migration-state", test_migration_state);
    qtest_add_func("/octane2/low-ram-migration",
                   test_octane2_low_ram_migration);
    int result = g_test_run();
    unlink(flash_path);
    g_free(flash_path);
    return result;
}
