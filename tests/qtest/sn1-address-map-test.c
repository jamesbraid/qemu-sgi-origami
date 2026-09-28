/*
 * SGI Origin 300 (IP45) machine-bus address routing, including the R10000
 * uncached attribute above bit 39. Independent constants catch bad decodes.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include <poll.h>
#include <sys/socket.h>
#include "sgi-migrate.h"
#include "sgi-test.h"

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define SPACE_MEMORY_SPECIAL UINT64_C(0x30000000000)
#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))

#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define BEDROCK_PI1 UINT64_C(0x200000)
#define BEDROCK_IO UINT64_C(0x400000)
#define BEDROCK_LB UINT64_C(0x600000)
#define BEDROCK_NI UINT64_C(0x680000)
#define BEDROCK_WINDOW_SIZE UINT64_C(0x800000)
#define BEDROCK_MD_MEMORY_CONFIG UINT64_C(0x780008)
#define BEDROCK_MD_CURRENT_CELL UINT64_C(0x780000)
#define BEDROCK_MD_INSTALLED UINT64_C(0x3300)
#define BEDROCK_MD_INSTALLED_SELECT2 \
    ((UINT64_C(2) << 32) | BEDROCK_MD_INSTALLED)

#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_CPU_PRESENT_A UINT64_C(0x40)
#define PI_CPU_PRESENT_B UINT64_C(0x48)
#define PI_CPU_ENABLE_A UINT64_C(0x50)
#define PI_CPU_ENABLE_B UINT64_C(0x58)
#define PI_CPU_ENABLE_C (BEDROCK_PI1 + UINT64_C(0x50))
#define PI_NMI_A UINT64_C(0x70)
#define PI_NMI_B UINT64_C(0x78)
#define PI_MASK_1_B UINT64_C(0xc0)
#define PI_CACHED_ALIAS UINT64_C(0x28)
#define IO_WIDGET_ID UINT64_C(0x0)
#define IO_SCRATCH0 UINT64_C(0x150)
#define IO_ITTE0 UINT64_C(0x160)
#define LB_SCRATCH2 UINT64_C(0x8010)
#define NI_PORT_STATUS UINT64_C(0x0)

#define ITTE_WIDGET(w) ((uint64_t)(w) << 8)
#define XTALK_WIDGET(w) (SPACE_XTALK + (uint64_t)(w) * UINT64_C(0x1000000))
#define BIG_WINDOW(n) (SPACE_XTALK + UINT64_C(0x20000000) + \
                       (uint64_t)(n) * UINT64_C(0x20000000))

#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define HUB_LOCAL_BOOT (SPACE_HUB + UINT64_C(0x18000000))
#define HUB_LOCAL_FLASH (SPACE_HUB + UINT64_C(0x1a000000))
#define HUB_ECC (SPACE_HUB + UINT64_C(0x140000000))
/*
 * One ECC byte per eight bytes of memory gives each installed 256 MiB bank a
 * 32 MiB extent inside the carrier window reserved for its software bank.
 */
#define ECC_EXTENT UINT64_C(0x2000000)
#define ECC_APERTURE UINT64_C(0x40000000)
#define HUB_DIRECTORY (SPACE_HUB + UINT64_C(0x1c0000000))
#define BOARD_ID_LOCAL (HUB_LOCAL_REGISTERS + UINT64_C(0x4000000))
#define BOARD_ID_REMOTE (HUB_REMOTE_REGISTERS + UINT64_C(0x4000000))
#define BOARD_ID_UNUSED (HUB_LOCAL_REGISTERS + UINT64_C(0x5000000))
#define BOARD_ID_VALUE UINT64_C(0x8001)
#define HUB_LOCAL_HIGH_BOOT (SPACE_HUB + UINT64_C(0x1fc00000))
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define HUB_REMOTE_BOOT (SPACE_HUB + UINT64_C(0x38000000))
#define HUB_REMOTE_FLASH (SPACE_HUB + UINT64_C(0x3a000000))
#define L1_UART UINT64_C(0x80)
#define HUB_LED(slice) (UINT64_C(0xc0) + 8 * (slice))
#define L1_SCRATCH UINT64_C(0x38)
#define L1_UART_BASE (HUB_REMOTE_REGISTERS + L1_UART)
#define L1_UART_LOCAL_BASE (HUB_LOCAL_REGISTERS + L1_UART)
#define L1_UART_IER UINT64_C(0x08)
#define L1_UART_IIR UINT64_C(0x10)
#define L1_UART_STATUS UINT64_C(0x28)
#define L1_STATUS_READY UINT64_C(0x61)
#define PI_UART_PENDING (UINT64_C(1) << 4)
#define L1_EEPROM_CHASSIS 1
#define L1_EEPROM_BOARD 2
#define L1_EEPROM_SPD 4
#define L1_REPLY_ABSENT 0x98

#define PROM_RESET UINT64_C(0x1fc00000)
#define PROM_CONFIG_OFFSET UINT64_C(0x60)
#define FLASH_OBSERVED_OFFSET UINT64_C(0x1e0000)
#define FLASH_INTERIOR_OFFSET UINT64_C(0x400000)
/* Distinct from the -bios payload, so these reads identify their source. */
#define FLASH_LOWER_FIRST 0x51
#define FLASH_LOWER_CONFIG 0x72
#define FLASH_LOWER_LAST 0xb3
#define FLASH_LOWER_WORD UINT16_C(0x5100)
/*
 * Position one's own bytes. An erased seed would read the same as nothing
 * being mapped at all, so a read proving which part answered needs the second
 * position to carry values position zero does not.
 */
#define FLASH_UPPER_FIRST 0xa5
#define FLASH_UPPER_LAST 0x5a
#define FLASH_UPPER_WORD UINT16_C(0xa55a)
#define BOOT_SPACE_SIZE UINT64_C(0x8000000)
/* The AMD unlock words 0x555 and 0x2aa, at the bus offsets 8n. */
#define FLASH_UNLOCK0 UINT64_C(0x2aa8)
#define FLASH_UNLOCK1 UINT64_C(0x1550)
#define FLASH_QUERY UINT64_C(0x2a8)
#define FLASH_QUERY_READ(i) ((uint64_t)(i) * 2)
/*
 * The same two unlock cycles as a part sees them when the address is one byte
 * to one byte rather than four: the word addresses doubled. An execution view
 * decodes that way, so a sequence sent there has to use these to be the
 * native sequence at all.
 */
#define FLASH_NATIVE_UNLOCK0 UINT64_C(0xaaa)
#define FLASH_NATIVE_UNLOCK1 UINT64_C(0x554)
#define FLASH_ERASED_TARGET UINT64_C(0x200000)
#define FLASH_PAYLOAD_TARGET UINT64_C(0x1000)
/*
 * The directory words for memory line 0x1a00000 and for the last line of one
 * installed 256 MiB bank, and the aperture the node ends with.
 */
#define DIRECTORY_LINE_1A00000 UINT64_C(0x340000)
#define DIRECTORY_LAST_FITTED UINT64_C(0x1fffff0)
#define DIRECTORY_FIRST_ABSENT UINT64_C(0x2000000)
#define DIRECTORY_PROTECTION_BANK1_HIGH UINT64_C(0xf800408)
#define DIRECTORY_APERTURE UINT64_C(0x40000000)

#define NMI_READY_MARKER UINT64_C(0x1400)
#define NMI_TAKEN_MARKER UINT64_C(0x4e4d)
#define CONFIG0_READY_MARKER UINT64_C(0xc0f0)
#define CPU_NUMBER_READY_MARKER UINT64_C(0x600d)
#define CONFIG0_RESET_READBACK UINT64_C(0x6c9aa985)
#define CONFIG0_MUTATED_READBACK UINT64_C(0x6c9aa982)
#define POLL_TRIES 10000
#define POLL_INTERVAL_US 1000
/*
 * Guest run-time to allow a mistaken NMI to vector before asserting none did.
 */
#define NMI_SETTLE_TRIES 20
#define NMI_SETTLE_INTERVAL_US 10000

/*
 * The modelled minimum between a frame arriving and its completion, in
 * nanoseconds: the sgi-ioc3-eth device default. Delivery is published only
 * once this much virtual time has passed, so a wait for one has to advance
 * the clock as well as give the main loop a pass.
 */
#define RX_ARRIVAL_LATENCY_NS 200000

#define BEDROCK_WIDGET_PART UINT64_C(0xc110)
#define BEDROCK_WIDGET_MFG UINT64_C(0x036)
#define XBRIDGE_XBOW_PART UINT64_C(0xd000)
#define XBRIDGE_HEAD_PART UINT64_C(0xd002)
#define XBRIDGE_HEAD_MFG UINT64_C(0x024)
#define WIDGET_PART(v) (((v) >> 12) & 0xffff)
#define WIDGET_MFG(v) (((v) >> 1) & 0x7ff)
#define WIDGET_REV(v) ((v) >> 28)

#define IO8_USB_WIDGET 0xf
#define IO8_USB_SLOT 5
#define IO8_USB_CFG (XTALK_WIDGET(IO8_USB_WIDGET) + 0x20000 + \
                     IO8_USB_SLOT * 0x1000)
#define IO8_USB_WINDOW (XTALK_WIDGET(IO8_USB_WIDGET) + 0x900000)
#define IO8_USB_BAR 0x10000000
#define IO8_USB_HCCA_PHYS 0x100000
#define IO8_USB_HCCA_IOVA 0x40000000
#define IO8_USB_HCCA_FRAME 0x80

#define IO8_IOC3_WIDGET 0xf
#define IO8_IOC3_SLOT 4
#define IO8_IOC3_CFG (XTALK_WIDGET(IO8_IOC3_WIDGET) + 0x20000 + \
                     IO8_IOC3_SLOT * 0x1000)
#define IO8_IOC3_WINDOW (XTALK_WIDGET(IO8_IOC3_WIDGET) + 0x800000)
#define IO8_IOC3_BAR 0x10000000
#define IO8_IOC3_SERIAL_A 0x20178
#define IO8_IOC3_SERIAL_B 0x20170
#define IO8_IOC3_SERIAL_SCRATCH 7
#define IO8_IOC3_KBC_CONTROL 0x09c
#define IO8_IOC3_INT_STATUS 0x01c
#define IO8_IOC3_INT_ENABLE_SET 0x020
#define IO8_IOC3_MICROLAN_CONTROL 0x030
#define IO8_IOC3_PIN_CONTROL_SET 0x034
#define IO8_IOC3_ENET_MAC_CONTROL 0x0f0
#define IO8_IOC3_ENET_INT_STATUS 0x0f4
#define IO8_IOC3_ENET_INT_ENABLE 0x0f8
#define IO8_IOC3_ENET_RX_CONTROL 0x0fc
#define IO8_IOC3_ENET_RX_RING_HI 0x100
#define IO8_IOC3_ENET_RX_RING_LO 0x104
#define IO8_IOC3_ENET_RX_CONSUME 0x10c
#define IO8_IOC3_ENET_RX_PRODUCE 0x110
#define IO8_IOC3_ENET_RX_TIMER 0x114
#define IO8_IOC3_PARALLEL_ECR 0x2015a
#define IO8_IOC3_BYTEBUS_DEV0 0x80000
#define IO8_IOC3_RTC_CONTROL (IO8_IOC3_BYTEBUS_DEV0 + 0x7ff8)
#define IO8_IOC3_RTC_SECONDS (IO8_IOC3_BYTEBUS_DEV0 + 0x7ff9)
#define IO8_IOC3_RTC_YEAR (IO8_IOC3_BYTEBUS_DEV0 + 0x7fff)
#define IO8_IOC3_RTC_READ 0x40
#define IO8_IOC3_RTC_WRITE 0x80

#define IOC3_MICROLAN_LEVEL (1U << 0)
#define IOC3_PIN_MICROLAN_ENABLE (1U << 21)
#define IOC3_MICROLAN_RESET_SLOT ((500U << 10) | (65U << 2))
#define IOC3_INT_SERIAL_A (1U << 6)
#define IOC3_ENET_MCR_PROMISC (1U << 1)
#define IOC3_ENET_MCR_RX_DMA_EN (1U << 15)
#define IOC3_ENET_MCR_RX_EN (1U << 16)
#define IOC3_ENET_MCR_RX_OFFSET(halfwords) ((uint32_t)(halfwords) << 3)
#define IOC3_ENET_INT_RX_TIMER (1U << 0)
#define IOC3_ENET_RX_PRODUCE_ARM (1U << 31)
#define IOC3_ENET_RX_HEADER_VALID (1U << 31)
#define IOC3_RX_RING_PHYS 0x200000
#define IOC3_RX_BUFFER_PHYS 0x201000
#define IOC3_RX_RING_IOVA 0x40000000
#define IOC3_RX_BUFFER_IOVA 0x40001000

/*
 * The ENET block's MII management registers (include/hw/net/sgi-ioc3-eth.h).
 */
#define MII_CONTROL (IO8_IOC3_WINDOW + 0x148)
#define MII_DATA_R  (IO8_IOC3_WINDOW + 0x14c)
#define MII_DATA_W  (IO8_IOC3_WINDOW + 0x150)
#define MII_READ    (1u << 10)

/* What the BASEIO PROM's MII helpers hard-code, and Origin 200's default. */
#define PHY_ADDRESS       1
#define ORIGIN200_ADDRESS 31

/*
 * The IO-8's fixed devices are an sgi-baseio (the "io8" preset) on the
 * XBridge's own head F; the XBridge itself is a fitted part of the IP45 node
 * board (sn1-node.c creates it directly, as a QOM child of the node object)
 * rather than a removable board.
 */
#define IOC3_PATH "/machine/node0/baseio/ioc3"
#define MAC "02:00:5d:aa:bb:cc"

#define XBRIDGE_ATE0 0x10000
#define XBRIDGE_DEVICE(n) (0x200 + (n) * 8)
#define XBRIDGE_DEVICE_MEMORY (1U << 12)
/*
 * XBridge device registers 0 to 7: bit 13 is PIO swap, reset clear. Clear
 * places a narrow access by offset exclusive-or with the value preserved; set
 * makes it a plain byte stream, mirrored. An ordinary register (IOC3, the
 * OHCI's own BAR-mapped registers) wants the former, so these devices are
 * left at the reset default rather than setting the bit. The flag this
 * file used to set here was named backwards and set the mirror.
 */
#define XBRIDGE_HOST_PORT 0xa
#define XBRIDGE_ATE_VALID 1
#define XBRIDGE_ATE_NO_SWAP (1U << 5)
#define XBRIDGE_INT_STATUS 0x100
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_INT_RESET 0x110
#define XBRIDGE_ERR_UPPER 0x170
#define XBRIDGE_PAGE_FAULT (1U << 30)
#define XBRIDGE_INT_UPPER 0x30
#define XBRIDGE_INT_LOWER 0x38
#define XBRIDGE_INT_HOST(n) (0x130 + (n) * 8)
#define XBRIDGE_USB_PIN 5
#define XBRIDGE_IOC3_SUPERIO_PIN 0
#define XBRIDGE_IOC3_ETH_PIN 4

#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_0 0x98
#define PI_MASK_0_A 0xa8
#define PI_REMOTE_WINDOW 0x1800000
#define PI_TEST_PENDING_BIT 17
#define MIPS_CAUSE_HARDWARE_IRQ0 (1U << 10)

#define OHCI_HC_CONTROL 0x04
#define OHCI_HC_INTERRUPT_STATUS 0x0c
#define OHCI_HC_INTERRUPT_ENABLE 0x10
#define OHCI_HC_HCCA 0x18
#define OHCI_HC_RH_DESCRIPTOR_A 0x48
#define OHCI_CONTROL_OPERATIONAL 0x80
#define OHCI_CONTROL_SUSPEND 0xc0
#define OHCI_INTERRUPT_SF 0x04
#define OHCI_INTERRUPT_MIE 0x80000000

static char *prom_path;

static char *create_prom(uint8_t first, uint8_t config, uint8_t last)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    image[0] = first;
    image[PROM_CONFIG_OFFSET] = config;
    image[PROM_SIZE - 1] = last;

    fd = g_file_open_tmp("sn1-address-map-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *create_cpu_control_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }

    /* Each CPU increments its own t0 while it is runnable. */
    stl_be_p(image + 0x400, 0x65080001); /* daddiu t0, t0, 1 */
    stl_be_p(image + 0x404, 0x1000fffe); /* beq zero, zero, 0x400 */

    fd = g_file_open_tmp("sn1-cpu-control-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *create_cpu_number_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* branch to 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* park fault vectors */
    }
    stl_be_p(image + 0x400, 0x400a6000); /* mfc0 t2, Status */
    stl_be_p(image + 0x404, 0x354a0080); /* enable KX */
    stl_be_p(image + 0x408, 0x408a6000); /* mtc0 t2, Status */
    stl_be_p(image + 0x40c, 0x000000c0); /* ehb */
    stl_be_p(image + 0x410, 0x3c029200); /* upper local window */
    stl_be_p(image + 0x414, 0x3c030100); /* CPU-number offset */
    stl_be_p(image + 0x418, 0x34630020);
    stl_be_p(image + 0x41c, 0x0002103c);
    stl_be_p(image + 0x420, 0x00431025);
    stl_be_p(image + 0x424, 0xdc530000); /* ld s3, 0(v0) */
    stl_be_p(image + 0x428, 0xdc540078); /* ld s4, pending 0 */
    stl_be_p(image + 0x42c, 0xdc560030); /* ld s6, CPU_ENABLE_A(v0) */
    stl_be_p(image + 0x430, 0x34150000 | CPU_NUMBER_READY_MARKER);
    stl_be_p(image + 0x434, 0x1000ffff); /* park */
    stl_be_p(image + 0x438, 0x00000000);

    fd = g_file_open_tmp("sn1-cpu-number-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *create_nmi_control_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }

    /* Both reset and NMI enter through the reset vector. */
    stl_be_p(image + 0x400, 0x40086000); /* mfc0 t0, Status */
    stl_be_p(image + 0x404, 0x3c090008); /* lui t1, Status.NMI >> 16 */
    stl_be_p(image + 0x408, 0x01094824); /* and t1, t0, t1 */
    stl_be_p(image + 0x40c, 0x15200008); /* bne t1, zero, 0x430 */
    stl_be_p(image + 0x410, 0x00000000); /* nop */
    stl_be_p(image + 0x414, 0x34101400); /* ori s0, zero, ready */
    stl_be_p(image + 0x418, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x41c, 0x00000000); /* nop */

    stl_be_p(image + 0x430, 0x34104e4d); /* ori s0, zero, taken */
    stl_be_p(image + 0x434, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x438, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-nmi-control-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *create_config0_reset_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }

    stl_be_p(image + 0x400, 0x40108000); /* mfc0 s0, Config0 */
    stl_be_p(image + 0x404, 0x34080002); /* ori t0, zero, 2 */
    stl_be_p(image + 0x408, 0x40888000); /* mtc0 t0, Config0 */
    stl_be_p(image + 0x40c, 0x000000c0); /* ehb */
    stl_be_p(image + 0x410, 0x40118000); /* mfc0 s1, Config0 */
    stl_be_p(image + 0x414, 0x3412c0f0); /* ori s2, zero, ready */
    stl_be_p(image + 0x418, 0x1000ffff); /* beq zero, zero, self */
    stl_be_p(image + 0x41c, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-config0-reset-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *machine_start_with(const char *path, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(path);

    return qtest_initf(MACHINE_ARGS " -bios %s %s", quoted, extra);
}

static QTestState *machine_start_bare(const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom_path);

    return qtest_initf("-M origin300 -bios %s %s", quoted, extra);
}

static QTestState *machine_start(void)
{
    return machine_start_with(prom_path, "");
}

static void machine_configure_memory(QTestState *qts)
{
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_SELECT2);
}

static void expect_console_text(int fd, const char *expected)
{
    g_autofree char *actual = g_malloc(strlen(expected) + 1);
    size_t length = strlen(expected);
    size_t offset = 0;

    while (offset < length) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        ssize_t count;

        g_assert_cmpint(poll(&pfd, 1, 3000), ==, 1);
        count = read(fd, actual + offset, length - offset);
        g_assert_cmpint(count, >, 0);
        offset += count;
    }
    actual[length] = 0;
    g_assert_cmpstr(actual, ==, expected);
}

static uint64_t cpu_gpr(QTestState *qts, unsigned int cpu, const char *name)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers %u", cpu);
    g_autofree char *field = g_strdup_printf("%s ", name);
    const char *value_text = strstr(registers, field);
    uint64_t value;

    g_assert_nonnull(value_text);
    value_text += strlen(field);
    g_assert_cmpint(sscanf(value_text, "%" SCNx64, &value), ==, 1);
    return value;
}

static uint32_t cpu_cause(QTestState *qts, unsigned int cpu)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers %u", cpu);
    const char *value_text = strstr(registers, "Cause   0x");
    uint32_t value;

    g_assert_nonnull(value_text);
    value_text += strlen("Cause   0x");
    g_assert_cmpint(sscanf(value_text, "%" SCNx32, &value), ==, 1);
    return value;
}

static void wait_for_cpu_gpr(QTestState *qts, unsigned int cpu,
                             const char *name, uint64_t expected)
{
    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        if (cpu_gpr(qts, cpu, name) == expected) {
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(cpu_gpr(qts, cpu, name), ==, expected);
}

static uint64_t wait_for_cpu_gpr_change(QTestState *qts, unsigned int cpu,
                                        const char *name, uint64_t previous)
{
    uint64_t current = previous;

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        current = cpu_gpr(qts, cpu, name);
        if (current != previous) {
            return current;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(current, !=, previous);
    return current;
}

static void stop_vm(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(qts, "STOP");
}

static uint64_t run_vm_until_cpu_gpr_changes(QTestState *qts,
                                              unsigned int cpu,
                                              const char *name,
                                              uint64_t previous)
{
    uint64_t current;

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    current = wait_for_cpu_gpr_change(qts, cpu, name, previous);
    stop_vm(qts);
    return current;
}

static void stop_vm_if_running(QTestState *qts)
{
    g_autoptr(QDict) response =
        qtest_qmp(qts, "{'execute':'query-status'}");
    QDict *status = qdict_get_qdict(response, "return");

    if (qdict_get_bool(status, "running")) {
        stop_vm(qts);
    }
}

static void check_access_has_no_storage(QTestState *qts, uint64_t address)
{
    uint64_t before = qtest_readq(qts, address);

    qtest_writeq(qts, address, ~before);
    g_assert_cmphex(qtest_readq(qts, address), ==, before);
}

static uint32_t xtalk_reg_read(QTestState *qts, unsigned int widget,
                               uint64_t offset)
{
    return qtest_readl(qts, XTALK_WIDGET(widget) + offset + 4);
}

static void xtalk_reg_write(QTestState *qts, unsigned int widget,
                            uint64_t offset, uint32_t value)
{
    qtest_writel(qts, XTALK_WIDGET(widget) + offset + 4, value);
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

static void assert_flat_view_unmapped(QTestState *qts, uint64_t address)
{
    g_autofree char *type = flat_view_region_type_at(qts, address);

    if (type) {
        g_test_message("0x%" PRIx64 " is mapped as %s", address, type);
        g_assert_not_reached();
    }
}

/*
 * Every window the processor fetches firmware from must resolve to ram, rom
 * or romd in the flat view. An i/o entry there caches no TCG translation:
 * each instruction recompiles as its own one-instruction block, and the guest
 * runs 100-300x slower while giving no outward sign anything is wrong.
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

static void assert_flat_view_is_io(QTestState *qts, uint64_t address)
{
    g_autofree char *type = flat_view_region_type_at(qts, address);

    g_assert_cmpstr(type, ==, "i/o");
}

static void test_ram_and_hub_alias(const void *data)
{
    QTestState *qts = machine_start();

    machine_configure_memory(qts);
    assert_flat_view_is_execution_storage(qts, 0x1000);
    assert_flat_view_is_execution_storage(qts, 1 * GiB + 0x1000);
    assert_flat_view_is_io(qts, 256 * MiB);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED);
    assert_flat_view_is_io(qts, 0x1000);
    assert_flat_view_is_execution_storage(qts, 2 * GiB + 0x1000);
    assert_flat_view_is_execution_storage(qts, 3 * GiB + 0x1000);
    machine_configure_memory(qts);
    assert_flat_view_is_execution_storage(qts, 0x1000);
    qtest_system_reset(qts);
    assert_flat_view_is_io(qts, 0x1000);
    machine_configure_memory(qts);
    assert_flat_view_is_execution_storage(qts, 0x1000);
    qtest_writeq(qts, 0x1000, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(qts, SPACE_HUB + 0x1000), ==,
                    UINT64_C(0x0123456789abcdef));

    qtest_writeq(qts, SPACE_HUB + 0x2000,
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(qts, 0x2000), ==,
                    UINT64_C(0xfedcba9876543210));

    /* The 256 MiB boundary starts the local register block, not more RAM. */
    qtest_writeq(qts, 256 * MiB + PI_CPU_NUMBER,
                 UINT64_C(0xdeadbeefcafef00d));
    g_assert_cmphex(qtest_readq(qts, SPACE_HUB + 256 * MiB + PI_CPU_NUMBER),
                    ==, 0);
    /* The first fixed 1 GiB window has a 256 MiB installed bank. */
    qtest_writeq(qts, 512 * MiB, UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(qts, 512 * MiB), !=,
                    UINT64_C(0x599a599a599a599a));
    /*
     * Memory-special reaches node memory through the memory unit's
     * fetch-and-op operations, never as plain RAM.
     */
    assert_flat_view_is_io(qts, SPACE_MEMORY_SPECIAL + 0x1000);

    qtest_quit(qts);
}

static void test_bedrock_register_aliases(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x8badf00d12345678);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_NUMBER), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_NUMBER), ==, 2);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_PRESENT_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_PRESENT_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_PRESENT_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_PRESENT_B), ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_LB + LB_SCRATCH2, scratch);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_NI +
                                NI_PORT_STATUS) & (UINT64_C(1) << 18),
                    ==, 0);

    /* The 8 MiB architectural window does not turn its tail into storage. */
    check_access_has_no_storage(qts,
                                BEDROCK_LOCAL + BEDROCK_WINDOW_SIZE - 8);
    check_access_has_no_storage(qts,
                                BEDROCK_NODE0 + BEDROCK_WINDOW_SIZE - 8);

    qtest_quit(qts);
}

static void test_origin300_cpu_c_initial_enable(const void *data)
{
    QTestState *qts = machine_start();

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + 0x50), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                0x50), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_ENABLE_B), ==, 0);

    /* The initial model policy must not override later register writes. */
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + 0x50, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                0x50), ==, 0);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + 0x50, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                0x50), ==, 1);
    qtest_quit(qts);
}

/*
 * The four-processor population fits every slot, and every one comes out of
 * reset granted. The stock PROM's boot path never sets a CPU-enable register
 * (hw/mips/sgi/sn1-node.c says what it does), so a slot the model leaves
 * clear is a slot the firmware does not count.
 */
static void test_origin300_four_cpu_initial_enable(const void *data)
{
    QTestState *qts = machine_start_bare("-cpu R14000 -smp 4");

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_ENABLE_A), ==, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_ENABLE_B), ==, 1);

    qtest_quit(qts);
}

/*
 * Neither Hub-special register window is a view of Bedrock's own register
 * block: that is the crosstalk form's job alone. A store at the local-block
 * scratch register's offset through either window must not reach the local
 * block.
 */
static void test_hub_windows_omit_bedrock_registers(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x1234abcd5678ef90);

    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_LB + LB_SCRATCH2, scratch);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + BEDROCK_LB + LB_SCRATCH2,
                 ~scratch);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + BEDROCK_LB + LB_SCRATCH2,
                 ~scratch);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);

    qtest_quit(qts);
}

/*
 * The stock PROM stores 0x2000, 0x100, 0x2b and 0x29 to the sixteen
 * doublewords below the L1 UART while it initializes a node's memory. They
 * land in an unidentified block that keeps nothing, not on the processor
 * interface's CPU enable, region-present and cached-alias registers that
 * share those offsets in the crosstalk form.
 */
static void test_hub_unidentified_block_keeps_nothing(const void *data)
{
    QTestState *qts = machine_start();
    uint64_t pi[16];

    for (unsigned int i = 0; i < ARRAY_SIZE(pi); i++) {
        pi[i] = qtest_readq(qts, BEDROCK_LOCAL + 8 * i);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(pi); i++) {
        qtest_writeq(qts, HUB_REMOTE_REGISTERS + 8 * i,
                     i < 8 ? 0x2b : 0x29);
        qtest_writeq(qts, HUB_LOCAL_REGISTERS + 8 * i, 0x2000);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(pi); i++) {
        if (8 * i == PI_CPU_NUMBER) {
            continue; /* answers with the reading slot, not stored state */
        }
        g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + 8 * i), ==, pi[i]);
        g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + 8 * i), ==, 0);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + 8 * i), ==, 0);
    }

    qtest_quit(qts);
}

/*
 * What the Hub-special windows refuse, and that each refusal is said out
 * loud: offsets past the LEDs decode to nothing in either form, an LED
 * answers only an aligned eight-byte access, the unidentified block logs what
 * it ignores, and both blocks honour the current-cell access rule.
 */
static void test_hub_window_refusals(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    const uint64_t md_cell = BEDROCK_LOCAL + BEDROCK_MD_CURRENT_CELL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-hub-window-log-XXXXXX.txt", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", quoted);
    qts = machine_start_with(prom_path, extra);

    /* Past the LEDs: nothing in the local form, a logged refusal remotely. */
    assert_flat_view_unmapped(qts, HUB_LOCAL_REGISTERS + 0xe0);
    assert_flat_view_unmapped(qts, HUB_LOCAL_REGISTERS + 0x1000);
    qtest_readq(qts, HUB_REMOTE_REGISTERS + 0xe0);
    qtest_readq(qts, HUB_REMOTE_REGISTERS + 0x1000);

    /* A narrow LED access is refused and leaves the LED alone. */
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(1), 0x5d);
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL_REGISTERS + HUB_LED(1) + 4),
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(1)), ==,
                    0x5d);

    /* The unidentified block logs what it reads as zero and ignores. */
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + PI_CACHED_ALIAS, 0x2b);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + PI_CACHED_ALIAS),
                    ==, 0);

    /* With the region's current-cell bit clear, both blocks refuse. */
    qtest_writeq(qts, md_cell, 0);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + PI_CACHED_ALIAS, 0x29);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(1), 0x11);
    qtest_writeq(qts, md_cell, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(1)), ==,
                    0x5d);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "remote register window read at 0xe0 is "
                                 "not decoded"));
    g_assert_nonnull(strstr(log, "remote register window read at 0x1000 is "
                                 "not decoded"));
    g_assert_nonnull(strstr(log, "refusing a 4-byte read at offset 0xc"));
    g_assert_nonnull(strstr(log, "ignores a 8-byte write of 0x2b at offset "
                                 "0x28"));
    g_assert_nonnull(strstr(log, "block read of 8 bytes at offset 0x28 "
                                 "returns 0"));
    g_assert_nonnull(strstr(log, "Hub-special register window write at 0x28 "
                                 "denied"));
    g_assert_nonnull(strstr(log, "processor LED write denied"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * PI state a Hub-special window store could disturb: masks and cross-calls.
 */
static void read_pi_interrupt_state(QTestState *qts, uint64_t base,
                                    uint64_t state[9])
{
    for (unsigned int i = 0; i < 9; i++) {
        state[i] = qtest_readq(qts, base + PI_MASK_0_A + 8 * i);
    }
}

/*
 * One LED register per processor slice at 0xc0 + 8 * slice of both windows.
 * Before this decode existed those offsets were PI interrupt mask 1 B and the
 * cross-call-pending set A, set B and clear A registers, so every PROM
 * progress code and IRIX heartbeat rewrote a mask or raised or cleared a
 * cross-call; this test fails on that decode.
 */
static void test_hub_special_leds(const void *data)
{
    QTestState *qts = machine_start_bare("-cpu R14000 -smp 4");
    uint64_t pi0[9], pi1[9], after[9];

    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_1_B, 0x5a5a);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + PI_MASK_1_B, 0xa5a5);
    read_pi_interrupt_state(qts, BEDROCK_LOCAL, pi0);
    read_pi_interrupt_state(qts, BEDROCK_NODE0 + BEDROCK_PI1, pi1);

    for (unsigned int slice = 0; slice < 4; slice++) {
        qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(slice), 0x5d + slice);
    }
    for (unsigned int slice = 0; slice < 4; slice++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(slice)),
                        ==, 0x5d + slice);
        g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS +
                                    HUB_LED(slice)), ==, 0x5d + slice);
        qtest_writeq(qts, HUB_REMOTE_REGISTERS + HUB_LED(slice), 0xf0 + slice);
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(slice)),
                        ==, 0xf0 + slice);
    }

    read_pi_interrupt_state(qts, BEDROCK_LOCAL, after);
    g_assert_cmpmem(after, sizeof(after), pi0, sizeof(pi0));
    read_pi_interrupt_state(qts, BEDROCK_NODE0 + BEDROCK_PI1, after);
    g_assert_cmpmem(after, sizeof(after), pi1, sizeof(pi1));

    /* The LED bank is eight bits wide. */
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(0), 0x1a5);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(0)), ==,
                    0xa5);
    /* Only the aligned eight-byte access every consumer makes answers. */
    qtest_writel(qts, HUB_LOCAL_REGISTERS + HUB_LED(1) + 4, 0x11);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(1)), ==,
                    0xf1);
    g_assert_cmphex(qtest_readl(qts, HUB_LOCAL_REGISTERS + HUB_LED(1) + 4),
                    ==, 0);

    /* No source gives a reset value; the model starts them dark. */
    qtest_system_reset(qts);
    for (unsigned int slice = 0; slice < 4; slice++) {
        g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(slice)),
                        ==, 0);
    }

    qtest_quit(qts);
}

static void test_hub_registers_refuse_denied_region(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x1122334455667788);
    const uint64_t md_cell = BEDROCK_LOCAL + BEDROCK_MD_CURRENT_CELL;

    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_LB + LB_SCRATCH2, scratch);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + L1_UART + L1_SCRATCH, 0x5a);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(2), 0x5d);
    qtest_writeq(qts, md_cell, 0);
    g_assert_cmphex(qtest_readq(qts, md_cell), ==, 0);

    qtest_writeq(qts, HUB_LOCAL_REGISTERS + PI_CPU_ENABLE_B, 1);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + PI_CPU_ENABLE_B, 1);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(2), 0xaa);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + HUB_LED(2), 0xbb);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + L1_UART + L1_SCRATCH, 0xcc);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + L1_UART + L1_SCRATCH, 0xdd);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + PI_CPU_ENABLE_B),
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + HUB_LED(2)), ==,
                    0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(2)), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_LOCAL), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_REMOTE), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, 0);

    /* Restoring access reveals the unchanged UART and register contents. */
    qtest_writeq(qts, md_cell, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, 0x5a);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, 0x5a);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + HUB_LED(2)), ==,
                    0x5d);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_REMOTE), ==, BOARD_ID_VALUE);
    qtest_quit(qts);
}

static void test_crosstalk_big_window_routes(const void *data)
{
    QTestState *qts = machine_start();
    uint64_t identity;

    /* Aim big window 0 at Bedrock's widget-1 face. */
    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_IO + IO_ITTE0,
                 ITTE_WIDGET(1));
    identity = qtest_readq(qts, BIG_WINDOW(0) + IO_WIDGET_ID);
    g_assert_cmphex(WIDGET_PART(identity), ==, BEDROCK_WIDGET_PART);
    g_assert_cmphex(WIDGET_MFG(identity), ==, BEDROCK_WIDGET_MFG);

    /* Retargeting the translation entry removes that route. */
    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_IO + IO_ITTE0,
                 ITTE_WIDGET(2));
    g_assert_cmphex(qtest_readq(qts, BIG_WINDOW(0) + IO_WIDGET_ID), !=,
                    identity);

    /* The unimplemented PIO catch-all does not retain writes. */
    check_access_has_no_storage(qts,
                                SPACE_XTALK + UINT64_C(0x10000000));

    qtest_quit(qts);
}

static void test_pi1_a_irq_reaches_cpu1(const void *data)
{
    QTestState *qts = machine_start();

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + PI_PENDING_MODIFY,
                 0x107);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + PI_MASK_0_A,
                 UINT64_C(1) << 7);
    g_assert_cmphex(cpu_cause(qts, 1) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + PI_MASK_0_A, 0);
    g_assert_cmphex(cpu_cause(qts, 1) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    /* A line for absent B must not reach either fitted processor. */
    qtest_writeq(qts, BEDROCK_NODE0 + PI_PENDING_MODIFY, 0x108);
    qtest_writeq(qts, BEDROCK_NODE0 + 0xb8, UINT64_C(1) << 8);
    g_assert_cmphex(cpu_cause(qts, 1) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    qtest_writeq(qts, BEDROCK_NODE0 + 0xb8, 0);
    qtest_quit(qts);
}

static void test_xbridge_topology(const void *data)
{
    QTestState *qts = machine_start();
    g_autoptr(QDict) response = NULL;
    uint32_t identity;

    identity = xtalk_reg_read(qts, 0, 0);
    g_assert_cmphex(WIDGET_REV(identity), ==, 2);
    g_assert_cmphex(WIDGET_PART(identity), ==, XBRIDGE_XBOW_PART);
    g_assert_cmphex(WIDGET_MFG(identity), ==, 0);
    g_assert_cmphex(identity & 1, ==, 1);
    g_assert_cmphex((xtalk_reg_read(qts, 0, 8) >> 6) & 0xf, ==, 0xa);

    identity = xtalk_reg_read(qts, 0xe, 0);
    g_assert_cmphex(WIDGET_REV(identity), ==, 2);
    g_assert_cmphex(WIDGET_PART(identity), ==, XBRIDGE_HEAD_PART);
    g_assert_cmphex(WIDGET_MFG(identity), ==, XBRIDGE_HEAD_MFG);
    g_assert_cmphex(identity & 1, ==, 1);
    identity = xtalk_reg_read(qts, 0xf, 0);
    g_assert_cmphex(WIDGET_REV(identity), ==, 2);
    g_assert_cmphex(WIDGET_PART(identity), ==, XBRIDGE_HEAD_PART);
    g_assert_cmphex(WIDGET_MFG(identity), ==, XBRIDGE_HEAD_MFG);
    g_assert_cmphex(identity & 1, ==, 1);

    /*
     * A widget scan addresses the same chip by the physical crosstalk port
     * Bedrock actually attached it to, not only by the widget-zero local-CPU
     * shortcut above. Both name the same upstream link, so this must read
     * identically to widget 0. A field diagnostic that walks Bedrock's own
     * ports (rather than using the shortcut) reads exactly this address to
     * decide the IO-8 board is present; before this widget was wired, the
     * read reached nothing at all (bus error, not the empty-slot 0xffffffff),
     * and that diagnostic reported no board present without ever exercising
     * the board's own PCI functions.
     */
    identity = xtalk_reg_read(qts, XBRIDGE_HOST_PORT, 0);
    g_assert_cmphex(WIDGET_REV(identity), ==, 2);
    g_assert_cmphex(WIDGET_PART(identity), ==, XBRIDGE_XBOW_PART);
    g_assert_cmphex(WIDGET_MFG(identity), ==, 0);
    g_assert_cmphex(identity & 1, ==, 1);

    /*
     * The XBridge is a fitted part of the IP45 node board itself: sn1-node.c
     * creates it directly, as a QOM child of the node object, the same
     * "xbridge" this test otherwise reads through xtalk_reg_*. "board-part-f"
     * defaults to the origin300-base.yaml catalogue's own recorded part for
     * this board, not the chip's own placeholder.
     */
    response = qtest_qmp(qts,
        "{'execute':'qom-get', 'arguments':{"
        "'path':'/machine/node0/xbridge', 'property':'board-part-f'}}");
    g_assert_cmpstr(qdict_get_try_str(response, "return"), ==,
                    "030-1673-003");

    xtalk_reg_write(qts, 0xe, 0x20, 0x0f00320e);
    g_assert_cmphex(xtalk_reg_read(qts, 0xe, 0x20), ==, 0x0f00321e);
    g_assert_cmphex(xtalk_reg_read(qts, 0xf, 0x20), ==, 0x0f00320f);
    xtalk_reg_write(qts, 0xf, 0x20, 0x0f00320d);
    g_assert_cmphex(xtalk_reg_read(qts, 0xe, 0x20), ==, 0x0f00321e);
    g_assert_cmphex(xtalk_reg_read(qts, 0xf, 0x20), ==, 0x0f00320d);

    /* The two head-E option positions are unpopulated. */
    g_assert_cmphex(qtest_readl(qts, XTALK_WIDGET(0xe) + 0x21000), ==,
                    0xffffffff);
    g_assert_cmphex(qtest_readl(qts, XTALK_WIDGET(0xe) + 0x22000), ==,
                    0xffffffff);

    qtest_quit(qts);
}

static uint32_t io8_usb_config_readl(QTestState *qts, unsigned offset)
{
    return qtest_readl(qts, IO8_USB_CFG + offset);
}

static void io8_usb_config_writel(QTestState *qts, unsigned offset,
                                  uint32_t value)
{
    qtest_writel(qts, IO8_USB_CFG + offset, value);
}

static uint32_t io8_usb_readl(QTestState *qts, unsigned offset)
{
    return qtest_readl(qts, IO8_USB_WINDOW + offset);
}

static void io8_usb_writel(QTestState *qts, unsigned offset, uint32_t value)
{
    qtest_writel(qts, IO8_USB_WINDOW + offset, value);
}

static uint32_t io8_ioc3_config_readl(QTestState *qts, unsigned offset)
{
    return qtest_readl(qts, IO8_IOC3_CFG + offset);
}

static void io8_ioc3_config_writel(QTestState *qts, unsigned offset,
                                   uint32_t value)
{
    qtest_writel(qts, IO8_IOC3_CFG + offset, value);
}

static uint32_t io8_ioc3_readl(QTestState *qts, unsigned offset)
{
    uint8_t bytes[4];

    qtest_memread(qts, IO8_IOC3_WINDOW + offset, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

static void io8_ioc3_writel(QTestState *qts, unsigned offset, uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    qtest_memwrite(qts, IO8_IOC3_WINDOW + offset, bytes, sizeof(bytes));
}

static void io8_ioc3_attach(QTestState *qts)
{
    io8_ioc3_config_writel(qts, 0x10, IO8_IOC3_BAR);
    io8_ioc3_config_writel(qts, 0x04, 0x0006);
    xtalk_reg_write(qts, IO8_IOC3_WIDGET,
                    XBRIDGE_DEVICE(IO8_IOC3_SLOT),
                    XBRIDGE_DEVICE_MEMORY | (IO8_IOC3_BAR >> 20));
}

static uint16_t phy_read(QTestState *qts, unsigned int phy, unsigned int reg)
{
    qtest_writel(qts, MII_CONTROL, MII_READ | (phy << 5) | reg);
    return qtest_readl(qts, MII_DATA_R) & 0xffff;
}

static void phy_write(QTestState *qts, unsigned int phy, unsigned int reg,
                      uint16_t val)
{
    qtest_writel(qts, MII_DATA_W, val);
    qtest_writel(qts, MII_CONTROL, (phy << 5) | reg);
}

static void io8_route_interrupt(QTestState *qts, unsigned int pin,
                                unsigned int pending_bit)
{
    xtalk_reg_write(qts, IO8_IOC3_WIDGET, XBRIDGE_INT_UPPER,
                    XBRIDGE_HOST_PORT << 16);
    xtalk_reg_write(qts, IO8_IOC3_WIDGET, XBRIDGE_INT_LOWER,
                    PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
    xtalk_reg_write(qts, IO8_IOC3_WIDGET, XBRIDGE_INT_HOST(pin),
                    (1U << 17) | pending_bit);
    xtalk_reg_write(qts, IO8_IOC3_WIDGET, XBRIDGE_INT_ENABLE, 1U << pin);
}

static uint16_t io8_usb_hcca_frame(QTestState *qts)
{
    uint8_t bytes[2];

    qtest_memread(qts, IO8_USB_HCCA_PHYS + IO8_USB_HCCA_FRAME,
                  bytes, sizeof(bytes));
    return lduw_le_p(bytes);
}

static void test_io8_usb_attachment(const void *data)
{
    static const unsigned bars[] = { 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24 };
    uint8_t hcca[0x100] = { 0 };
    QTestState *qts = machine_start();
    uint32_t identity;
    unsigned i;

    machine_configure_memory(qts);

    identity = io8_usb_config_readl(qts, 0);
    g_assert_cmphex(identity, ==, 0x580211c1);
    g_assert_cmphex(qtest_readl(qts, XTALK_WIDGET(0xe) + 0x25000), ==,
                    0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IO8_USB_CFG + 0x100), ==, 0xffffffff);
    g_assert_cmphex(io8_usb_config_readl(qts, 8), ==, 0x0c031010);
    g_assert_cmphex(io8_usb_config_readl(qts, 0x2c), ==, 0xffffffff);
    g_assert_cmphex((io8_usb_config_readl(qts, 0x3c) >> 8) & 0xff, ==, 1);

    for (i = 0; i < G_N_ELEMENTS(bars); i++) {
        io8_usb_config_writel(qts, bars[i], UINT32_MAX);
        if (i == 0) {
            g_assert_cmphex(io8_usb_config_readl(qts, bars[i]) &
                            0xfffffff0, !=, 0);
            g_assert_cmphex(io8_usb_config_readl(qts, bars[i]) & 1, ==, 0);
        } else {
            g_assert_cmphex(io8_usb_config_readl(qts, bars[i]), ==, 0);
        }
    }
    io8_usb_config_writel(qts, 0x10, IO8_USB_BAR);
    io8_usb_config_writel(qts, 4, 6);
    xtalk_reg_write(qts, IO8_USB_WIDGET, XBRIDGE_DEVICE(IO8_USB_SLOT),
                    XBRIDGE_DEVICE_MEMORY | (IO8_USB_BAR >> 20));
    g_assert_cmphex(io8_usb_readl(qts, 0), ==, 0x10);
    g_assert_cmphex(io8_usb_readl(qts, OHCI_HC_RH_DESCRIPTOR_A) & 0xff, ==,
                    2);

    qtest_memwrite(qts, IO8_USB_HCCA_PHYS, hcca, sizeof(hcca));
    qtest_writeq(qts, XTALK_WIDGET(IO8_USB_WIDGET) + XBRIDGE_ATE0,
                 IO8_USB_HCCA_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP | XBRIDGE_ATE_VALID);

    xtalk_reg_write(qts, IO8_USB_WIDGET, XBRIDGE_INT_UPPER,
                    XBRIDGE_HOST_PORT << 16);
    xtalk_reg_write(qts, IO8_USB_WIDGET, XBRIDGE_INT_LOWER,
                    PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
    xtalk_reg_write(qts, IO8_USB_WIDGET,
                    XBRIDGE_INT_HOST(XBRIDGE_USB_PIN),
                    (1U << 17) | PI_TEST_PENDING_BIT);
    xtalk_reg_write(qts, IO8_USB_WIDGET, XBRIDGE_INT_ENABLE,
                    1U << XBRIDGE_USB_PIN);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    (1U << XBRIDGE_USB_PIN), ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    io8_usb_writel(qts, OHCI_HC_HCCA, IO8_USB_HCCA_IOVA);
    io8_usb_writel(qts, OHCI_HC_INTERRUPT_ENABLE,
                   OHCI_INTERRUPT_MIE | OHCI_INTERRUPT_SF);
    io8_usb_writel(qts, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    for (i = 0; i < 64 && io8_usb_hcca_frame(qts) == 0; i++) {
        qtest_clock_step_next(qts);
    }
    g_assert_cmphex(io8_usb_hcca_frame(qts), !=, 0);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_INT_STATUS) & 0xff, ==,
                    1U << XBRIDGE_USB_PIN);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    UINT64_C(1) << PI_TEST_PENDING_BIT);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A,
                 UINT64_C(1) << PI_TEST_PENDING_BIT);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    UINT64_C(1) << PI_TEST_PENDING_BIT);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);

    io8_usb_writel(qts, OHCI_HC_INTERRUPT_STATUS, OHCI_INTERRUPT_SF);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    (1U << XBRIDGE_USB_PIN), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    UINT64_C(1) << PI_TEST_PENDING_BIT);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);
    /* The guest acknowledges its selected pending bit after INTA falls. */
    qtest_writeq(qts, BEDROCK_LOCAL + PI_PENDING_MODIFY,
                 PI_TEST_PENDING_BIT);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);
    io8_usb_writel(qts, OHCI_HC_CONTROL, OHCI_CONTROL_SUSPEND);

    qtest_quit(qts);
}

static void test_io8_usb_rejects_invalid_ate_dma(const void *data)
{
    uint8_t sentinel[0x100];
    uint8_t actual[sizeof(sentinel)];
    QTestState *qts = machine_start();

    machine_configure_memory(qts);

    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(qts, IO8_USB_HCCA_PHYS, sentinel, sizeof(sentinel));
    io8_usb_config_writel(qts, 0x10, IO8_USB_BAR);
    io8_usb_config_writel(qts, 4, 6);
    xtalk_reg_write(qts, IO8_USB_WIDGET, XBRIDGE_DEVICE(IO8_USB_SLOT),
                    XBRIDGE_DEVICE_MEMORY | (IO8_USB_BAR >> 20));
    qtest_writeq(qts, XTALK_WIDGET(IO8_USB_WIDGET) + XBRIDGE_ATE0,
                 IO8_USB_HCCA_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP);

    io8_usb_writel(qts, OHCI_HC_HCCA, IO8_USB_HCCA_IOVA);
    io8_usb_writel(qts, OHCI_HC_CONTROL, OHCI_CONTROL_OPERATIONAL);
    qtest_clock_step_next(qts);

    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    XBRIDGE_PAGE_FAULT, ==, XBRIDGE_PAGE_FAULT);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_ERR_UPPER) &
                    XBRIDGE_PAGE_FAULT, ==, XBRIDGE_PAGE_FAULT);
    qtest_memread(qts, IO8_USB_HCCA_PHYS, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), sentinel, sizeof(sentinel));

    xtalk_reg_write(qts, IO8_USB_WIDGET, XBRIDGE_INT_RESET, 2);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    XBRIDGE_PAGE_FAULT, ==, 0);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_USB_WIDGET,
                                  XBRIDGE_ERR_UPPER) &
                    XBRIDGE_PAGE_FAULT, ==, 0);
    qtest_quit(qts);
}

static void test_io8_ioc3_attachment(const void *data)
{
    QTestState *qts = machine_start();

    g_assert_cmphex(io8_ioc3_config_readl(qts, 0x00), ==, 0x000310a9);
    g_assert_cmphex(io8_ioc3_config_readl(qts, 0x08), ==, 0xff000001);
    g_assert_cmphex(io8_ioc3_config_readl(qts, 0x2c), ==, 0xffff10a9);
    g_assert_cmphex((io8_ioc3_config_readl(qts, 0x3c) >> 8) & 0xff, ==, 1);

    g_assert_cmphex(qtest_readl(qts, XTALK_WIDGET(0xe) + 0x24000), ==,
                    0xffffffff);
    g_assert_cmphex(qtest_readl(qts, IO8_IOC3_CFG + 0x100), ==, 0xffffffff);

    io8_ioc3_config_writel(qts, 0x10, UINT32_MAX);
    g_assert_cmphex(io8_ioc3_config_readl(qts, 0x10), ==, 0xfff00000);
    io8_ioc3_attach(qts);

    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_A +
                 IO8_IOC3_SERIAL_SCRATCH, 0x5a);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_B +
                 IO8_IOC3_SERIAL_SCRATCH, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_A +
                                IO8_IOC3_SERIAL_SCRATCH), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_B +
                                IO8_IOC3_SERIAL_SCRATCH), ==, 0xa5);

    io8_ioc3_writel(qts, IO8_IOC3_ENET_INT_ENABLE, 1);
    g_assert_cmphex(io8_ioc3_readl(qts, IO8_IOC3_ENET_INT_ENABLE), ==, 1);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW +
                                IO8_IOC3_PARALLEL_ECR), ==, 1);

    io8_ioc3_writel(qts, IO8_IOC3_KBC_CONTROL, 0xffff);
    g_assert_cmphex(io8_ioc3_readl(qts, IO8_IOC3_KBC_CONTROL), ==, 0);
    io8_ioc3_writel(qts, IO8_IOC3_PIN_CONTROL_SET,
                    IOC3_PIN_MICROLAN_ENABLE);
    io8_ioc3_writel(qts, IO8_IOC3_MICROLAN_CONTROL,
                    IOC3_MICROLAN_RESET_SLOT);
    g_assert_cmphex(io8_ioc3_readl(qts, IO8_IOC3_MICROLAN_CONTROL) &
                    IOC3_MICROLAN_LEVEL, ==, IOC3_MICROLAN_LEVEL);

    qtest_quit(qts);
}

static void test_io8_ioc3_timekeeper(const void *data)
{
    QTestState *qts = machine_start();

    io8_ioc3_attach(qts);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_BYTEBUS_DEV0, 0x5a);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL - 1, 0xa5);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL,
                 IO8_IOC3_RTC_READ);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW +
                                IO8_IOC3_BYTEBUS_DEV0), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW +
                                IO8_IOC3_RTC_CONTROL - 1), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW +
                                IO8_IOC3_RTC_CONTROL), ==,
                    IO8_IOC3_RTC_READ);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_io8_ioc3_nvram_drive(const void *data)
{
    const char *drive_options = data;
    g_autofree char *path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-ioc3-nvram-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 0x8000), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(path);
    extra = g_strdup_printf("-drive %s,file=%s,format=raw", drive_options,
                            quoted);

    qts = machine_start_with(prom_path, extra);
    io8_ioc3_attach(qts);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_BYTEBUS_DEV0 + 0x100,
                 0xc3);
    qtest_quit(qts);

    qts = machine_start_with(prom_path, extra);
    io8_ioc3_attach(qts);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW +
                                IO8_IOC3_BYTEBUS_DEV0 + 0x100), ==, 0xc3);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_io8_ioc3_serial_chardevs(const void *data)
{
    g_autofree char *extra = NULL;
    QTestState *qts;
    int a[2];
    int b[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, a), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, b), ==, 0);
    extra = g_strdup_printf(
        "-S -serial null "
        "-chardev socket,id=ioc3-a,fd=%d -serial chardev:ioc3-a "
        "-chardev socket,id=ioc3-b,fd=%d -serial chardev:ioc3-b",
        a[1], b[1]);
    qts = machine_start_with(prom_path, extra);
    g_assert_cmpint(close(a[1]), ==, 0);
    g_assert_cmpint(close(b[1]), ==, 0);
    io8_ioc3_attach(qts);

    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_A, 'A');
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_B, 'B');
    expect_console_text(a[0], "A");
    expect_console_text(b[0], "B");

    qtest_quit(qts);
    g_assert_cmpint(close(a[0]), ==, 0);
    g_assert_cmpint(close(b[0]), ==, 0);
}

static void test_io8_ioc3_superio_interrupt(const void *data)
{
    const unsigned int pending_bit = 18;
    const uint64_t pending = UINT64_C(1) << pending_bit;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int sv[2];
    unsigned int i;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    extra = g_strdup_printf(
        "-S -serial null -chardev socket,id=ioc3-a,fd=%d "
        "-serial chardev:ioc3-a", sv[1]);
    qts = machine_start_with(prom_path, extra);
    g_assert_cmpint(close(sv[1]), ==, 0);

    io8_ioc3_attach(qts);
    io8_route_interrupt(qts, XBRIDGE_IOC3_SUPERIO_PIN, pending_bit);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) & 1, ==, 0);

    io8_ioc3_writel(qts, IO8_IOC3_INT_ENABLE_SET, IOC3_INT_SERIAL_A);
    qtest_writeb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_A + 1, 0x01);
    g_assert_cmpint(write(sv[0], "S", 1), ==, 1);
    for (i = 0; i < POLL_TRIES; i++) {
        if (qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) == pending) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(io8_ioc3_readl(qts, IO8_IOC3_INT_STATUS) &
                    IOC3_INT_SERIAL_A, ==, IOC3_INT_SERIAL_A);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) & 1, ==, 1);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    (1U << XBRIDGE_IOC3_ETH_PIN), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    pending);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, pending);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);
    g_assert_cmphex(qtest_readb(qts, IO8_IOC3_WINDOW + IO8_IOC3_SERIAL_A),
                    ==, 'S');
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) & 1, ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    pending);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_PENDING_MODIFY, pending_bit);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    qtest_quit(qts);
    g_assert_cmpint(close(sv[0]), ==, 0);
}

static void test_io8_ioc3_ethernet_dma_interrupt(const void *data)
{
    static const uint8_t frame[14] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x08, 0x00,
    };
    const unsigned int pending_bit = 19;
    const uint64_t pending = UINT64_C(1) << pending_bit;
    uint8_t packet[4 + sizeof(frame)];
    g_autofree char *extra = NULL;
    QTestState *qts;
    int sv[2];
    unsigned int i;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    extra = g_strdup_printf("-nic socket,fd=%d,id=ioc3-net", sv[1]);
    qts = machine_start_with(prom_path, extra);
    machine_configure_memory(qts);
    g_assert_cmpint(close(sv[1]), ==, 0);

    io8_ioc3_attach(qts);
    qtest_writeq(qts, XTALK_WIDGET(IO8_IOC3_WIDGET) + XBRIDGE_ATE0,
                 IOC3_RX_RING_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP | XBRIDGE_ATE_VALID);
    qtest_writeq(qts, XTALK_WIDGET(IO8_IOC3_WIDGET) + XBRIDGE_ATE0 + 8,
                 IOC3_RX_BUFFER_PHYS |
                 ((uint64_t)XBRIDGE_HOST_PORT << 8) |
                 XBRIDGE_ATE_NO_SWAP | XBRIDGE_ATE_VALID);
    io8_route_interrupt(qts, XBRIDGE_IOC3_ETH_PIN, pending_bit);

    qtest_writel(qts, IOC3_RX_RING_PHYS, 0);
    qtest_writel(qts, IOC3_RX_RING_PHYS + 4, IOC3_RX_BUFFER_IOVA);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_MAC_CONTROL,
                    IOC3_ENET_MCR_PROMISC | IOC3_ENET_MCR_RX_DMA_EN |
                    IOC3_ENET_MCR_RX_EN |
                    IOC3_ENET_MCR_RX_OFFSET(33));
    io8_ioc3_writel(qts, IO8_IOC3_ENET_INT_ENABLE,
                    IOC3_ENET_INT_RX_TIMER);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_RX_RING_HI, 0);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_RX_RING_LO, IOC3_RX_RING_IOVA);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_RX_TIMER, 0);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_RX_CONTROL, 0);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_RX_PRODUCE,
                    8 | IOC3_ENET_RX_PRODUCE_ARM);

    stl_be_p(packet, sizeof(frame));
    memcpy(packet + 4, frame, sizeof(frame));
    g_assert_cmpint(write(sv[0], packet, sizeof(packet)), ==,
                    (int)sizeof(packet));
    for (i = 0; i < POLL_TRIES; i++) {
        if (io8_ioc3_readl(qts, IO8_IOC3_ENET_RX_CONSUME) == 8) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }

    g_assert_cmphex(io8_ioc3_readl(qts, IO8_IOC3_ENET_RX_CONSUME), ==, 8);
    g_assert_cmphex(qtest_readl(qts, IOC3_RX_BUFFER_PHYS) &
                    IOC3_ENET_RX_HEADER_VALID, ==,
                    IOC3_ENET_RX_HEADER_VALID);
    g_assert_cmphex(io8_ioc3_readl(qts, IO8_IOC3_ENET_INT_STATUS) &
                    IOC3_ENET_INT_RX_TIMER, ==, IOC3_ENET_INT_RX_TIMER);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    (1U << XBRIDGE_IOC3_ETH_PIN), ==,
                    1U << XBRIDGE_IOC3_ETH_PIN);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    (1U << XBRIDGE_IOC3_SUPERIO_PIN), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    pending);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, pending);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);
    io8_ioc3_writel(qts, IO8_IOC3_ENET_INT_STATUS,
                    IOC3_ENET_INT_RX_TIMER);
    g_assert_cmphex(xtalk_reg_read(qts, IO8_IOC3_WIDGET,
                                  XBRIDGE_INT_STATUS) &
                    (1U << XBRIDGE_IOC3_ETH_PIN), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==,
                    pending);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_PENDING_MODIFY, pending_bit);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    qtest_quit(qts);
    g_assert_cmpint(close(sv[0]), ==, 0);
}

static void test_io8_ioc3_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-ioc3-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from;
    QTestState *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = machine_start_with(
        prom_path, "-rtc base=2026-08-11T12:30:05,clock=vm");
    to = machine_start_with(
        prom_path,
        "-rtc base=2026-08-11T12:30:05,clock=vm -incoming defer");
    io8_ioc3_attach(from);
    io8_ioc3_attach(to);

    qtest_writeb(from, IO8_IOC3_WINDOW + IO8_IOC3_BYTEBUS_DEV0 + 0x100,
                 0xc3);
    qtest_writeb(from, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL,
                 IO8_IOC3_RTC_WRITE);
    qtest_writeb(from, IO8_IOC3_WINDOW + IO8_IOC3_RTC_SECONDS, 0x45);
    qtest_writeb(from, IO8_IOC3_WINDOW + IO8_IOC3_RTC_YEAR, 0x40);
    qtest_writeb(from, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL, 0);
    qtest_writeb(from, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL,
                 IO8_IOC3_RTC_READ);
    g_assert_cmphex(qtest_readb(from, IO8_IOC3_WINDOW +
                                IO8_IOC3_RTC_SECONDS), ==, 0x45);
    g_assert_cmphex(qtest_readb(from, IO8_IOC3_WINDOW + IO8_IOC3_RTC_YEAR),
                    ==, 0x40);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(qtest_readb(to, IO8_IOC3_WINDOW +
                                IO8_IOC3_BYTEBUS_DEV0 + 0x100), ==, 0xc3);
    g_assert_cmphex(qtest_readb(to, IO8_IOC3_WINDOW + IO8_IOC3_RTC_CONTROL),
                    ==, IO8_IOC3_RTC_READ);
    g_assert_cmphex(qtest_readb(to, IO8_IOC3_WINDOW + IO8_IOC3_RTC_SECONDS),
                    ==, 0x45);
    g_assert_cmphex(qtest_readb(to, IO8_IOC3_WINDOW + IO8_IOC3_RTC_YEAR),
                    ==, 0x40);

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/*
 * IO-8 uses DP83843 at MII address 1, as BASEIO PROM diagnostics require;
 * Origin 200 defaults to DP83840A at 31 (hw/net/dp83840.c, sn0-ioc3-test.c).
 * Origin 300 OpenBSD reports "nsphyter0 at iec0 phy 1: DP83843 10/100 PHY".
 * sn1.c passes the per-device sn1-model.h address through sgi-ioc3 ->
 * sgi-ioc3-eth -> dp83840's phy-addr; phy-dp83843 selects the different part.
 * This test checks identity and register 0x17 (Loopback Register) readback.
 * sgi-ioc3-eth-test.c checks actual loopback via a plain sgi-ioc3 rather than
 * full crosstalk; sn0-ioc3-test.c covers reset and autonegotiation. Here only
 * IO-8 placement and identity are asserted.
 */
static void test_transceiver_identity(const void *data)
{
    QTestState *qts = machine_start();

    io8_ioc3_attach(qts);

    /*
     * PHYIDR1 is the OUI's upper bits, the same on every National
     * Semiconductor PHY this project models, so it stays 0x2000 on both
     * parts. PHYIDR2 is this board's own: 0x5c10 is the DP83843's, not the
     * Origin 200 DP83840A's 0x5c01. The transceiver's own register contract
     * otherwise belongs to sn0-ioc3-test.c.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 2), ==, 0x2000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 3), ==, 0x5c10);

    qtest_quit(qts);
}

/*
 * Read-after-write only: the loopback behaviour this register selects on the
 * DP83843 belongs to tests/qtest/sgi-ioc3-eth-test.c, which reaches the
 * register through a plain sgi-ioc3 device rather than this board's full
 * crosstalk wiring.
 */
static void test_register_0x17_reads_back_a_written_value(const void *data)
{
    QTestState *qts = machine_start();

    io8_ioc3_attach(qts);
    phy_write(qts, PHY_ADDRESS, 0x17, 0x20);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x17), ==, 0x20);

    qtest_quit(qts);
}

static void test_origin200_address_is_silent(const void *data)
{
    QTestState *qts = machine_start();

    io8_ioc3_attach(qts);
    /* The Origin 200's address answers nothing on this board. */
    g_assert_cmphex(phy_read(qts, ORIGIN200_ADDRESS, 2), ==, 0xffff);

    qtest_quit(qts);
}

static void test_transceiver_answers_at_one_address(const void *data)
{
    QTestState *qts = machine_start();
    unsigned int phy, answered = 0;

    io8_ioc3_attach(qts);

    /*
     * Every address: both IRIX and the BASEIO PROM stop at the first that
     * answers, so a model answering everywhere would be found at address zero
     * and no boot notices. The count is the assertion.
     */
    for (phy = 0; phy < 32; phy++) {
        if (phy_read(qts, phy, 2) != 0xffff) {
            g_assert_cmpuint(phy, ==, PHY_ADDRESS);
            answered++;
        }
    }
    g_assert_cmpuint(answered, ==, 1);

    qtest_quit(qts);
}

static void test_hub_special_register_windows(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x5a);
    uint64_t cached_alias = qtest_readq(qts, BEDROCK_LOCAL + PI_CACHED_ALIAS);

    qtest_writeq(qts, HUB_REMOTE_REGISTERS + L1_UART + L1_SCRATCH, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);

    /*
     * sMDK's headless console (PROM console=d) polls line status through this
     * local form; the local window now reaches the same L1 UART the remote
     * window does.
     */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);
    /*
     * The PROM's stores just below the UART's 0x80 edge land in the
     * unidentified block, not the UART and not the PI registers.
     */
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + PI_NMI_A, 0x100);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + PI_NMI_B, 0x100);
    qtest_writeq(qts, HUB_REMOTE_REGISTERS + PI_CACHED_ALIAS, 0x2b);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CACHED_ALIAS), ==,
                    cached_alias);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + PI_CACHED_ALIAS),
                    ==, 0);
    check_access_has_no_storage(qts, HUB_REMOTE_REGISTERS + 0x1000);

    qtest_quit(qts);
}

/*
 * The local window holds its own blocks (see sn1_node_create_bedrock), not
 * the PI register file, so a PI register written through the crosstalk form
 * must not leak through when read back at the same relative offset in the
 * local window, and a store there must not reach the PI.
 */
static void test_hub_local_register_window_omits_pi_registers(
    const void *data)
{
    QTestState *qts = machine_start();
    /* Idle line status: transmitter empty, nothing queued to read yet. */
    const uint64_t idle = UINT64_C(0x60);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, 0x80);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_UART_STATUS), ==, idle);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_UART_STATUS), ==, idle);

    /* PI itself still works: only the crosstalk form ever reaches it. */
    qtest_writeq(qts, BEDROCK_LOCAL + PI_PENDING_MODIFY, 0x107);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) & 0x80,
                    ==, 0x80);

    /* Neither side of the UART reaches the PI register file. */
    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_1_B, 0x3c);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + PI_CPU_ENABLE_B),
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + PI_MASK_1_B, 0x5a);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_MASK_1_B), ==, 0x3c);

    qtest_quit(qts);
}

static void test_l1_operator_console_uses_serial0(const void *data)
{
    g_autofree char *extra = NULL;
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    extra = g_strdup_printf("-S -chardev socket,id=l1-console,fd=%d "
                            "-serial chardev:l1-console", sv[1]);
    qts = machine_start_with(prom_path, extra);
    g_assert_cmpint(close(sv[1]), ==, 0);

    /* The L1 powers the node at start and enters console mode on its own. */
    expect_console_text(sv[0], "INFO: 001c01 powering up the system.\r\n"
                               "001c01-L1> \r\n"
                               "entering console mode  001c01 CPU0, "
                               "<CTRL_T> to escape to L1\r\n");
    g_assert_cmpint(write(sv[0], "\024\r", 2), ==, 2);
    expect_console_text(sv[0], "escaping to L1 system controller\r\n"
                               "001c01-L1> ");

    qtest_quit(qts);
    g_assert_cmpint(close(sv[0]), ==, 0);
}

/*
 * A byte typed at the operator console reaches the node UART's receive queue
 * (confirmed live: LSR Data Ready and IIR 0x04 latch correctly) but, before
 * this fix, the UART's own IRQ output was never connected to any interrupt
 * consumer, so sMDK's smdk> prompt never accepted typed input. Bedrock
 * pending bank 0 bit 4 is the documented UART consumer; this exercises the
 * whole path a keystroke takes, chardev to CPU Cause, the way
 * test_io8_ioc3_superio_interrupt exercises the IOC3 serial path.
 */
static void test_l1_uart_irq_reaches_cpu(const void *data)
{
    g_autofree char *extra = NULL;
    QTestState *qts;
    int sv[2];
    unsigned int i;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    extra = g_strdup_printf("-S -chardev socket,id=l1-console,fd=%d "
                            "-serial chardev:l1-console", sv[1]);
    qts = machine_start_with(prom_path, extra);
    g_assert_cmpint(close(sv[1]), ==, 0);

    /* The L1 powers the node and enters console mode on its own. */
    expect_console_text(sv[0], "INFO: 001c01 powering up the system.\r\n"
                               "001c01-L1> \r\n"
                               "entering console mode  001c01 CPU0, "
                               "<CTRL_T> to escape to L1\r\n");

    qtest_writeq(qts, L1_UART_LOCAL_BASE + L1_UART_IER, 0x01);
    g_assert_cmphex(qtest_readq(qts, L1_UART_LOCAL_BASE + L1_UART_STATUS),
                    ==, 0x60);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                    PI_UART_PENDING, ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    /* A keystroke at the operator console reaches the node's UART queue. */
    g_assert_cmpint(write(sv[0], "x", 1), ==, 1);
    for (i = 0; i < POLL_TRIES; i++) {
        if (qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
            PI_UART_PENDING) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(qtest_readq(qts, L1_UART_LOCAL_BASE + L1_UART_STATUS) &
                    1, ==, 1);
    g_assert_cmphex(qtest_readq(qts, L1_UART_LOCAL_BASE + L1_UART_IIR) &
                    0xf, ==, 0x04);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                    PI_UART_PENDING, ==, PI_UART_PENDING);
    /* One node-wide UART line, broadcast to both processor interfaces. */
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_PENDING_0) & PI_UART_PENDING, ==,
                    PI_UART_PENDING);
    /* Still masked off: the pending bit alone must not reach the CPU. */
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, PI_UART_PENDING);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==,
                    MIPS_CAUSE_HARDWARE_IRQ0);

    /* Draining the whole framed reply clears the pending bit and the line. */
    while (qtest_readq(qts, L1_UART_LOCAL_BASE + L1_UART_STATUS) & 1) {
        qtest_readq(qts, L1_UART_LOCAL_BASE);
    }
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                    PI_UART_PENDING, ==, 0);
    g_assert_cmphex(cpu_cause(qts, 0) & MIPS_CAUSE_HARDWARE_IRQ0, ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_MASK_0_A, 0);
    qtest_quit(qts);
    g_assert_cmpint(close(sv[0]), ==, 0);
}

static char *create_flash_image(void);
static void remove_flash_image(const char *flash);
static QTestState *machine_start_with_flash(const char *flash,
                                             const char *extra);

static void test_prom_cpu_module_config(const void *data)
{
    static const uint64_t views[] = {
        PROM_RESET, HUB_LOCAL_BOOT, HUB_LOCAL_HIGH_BOOT, HUB_REMOTE_BOOT,
    };
    static const struct {
        uint64_t offset;
        uint32_t value;
    } words[] = {
        { 0x64, UINT32_C(0x549aa985) },
        { 0x74, UINT32_C(500000000) },
        { 0x90, 1 },
        { 0x94, UINT32_C(0x9c) },
        { 0x98, 1 },
    };
    g_autofree char *flash = create_flash_image();
    QTestState *qts;

    for (unsigned int attached = 0; attached < 2; attached++) {
        qts = attached ? machine_start_with_flash(flash, "") : machine_start();
        for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
            for (size_t j = 0; j < ARRAY_SIZE(views); j++) {
                g_assert_cmphex(qtest_readl(qts, views[j] + words[i].offset),
                                ==, words[i].value);
            }
        }
        qtest_quit(qts);
    }
    remove_flash_image(flash);
}

static void assert_erased_second_position(QTestState *qts)
{
    static const uint64_t views[] = { HUB_LOCAL_FLASH, HUB_REMOTE_FLASH };

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        g_assert_cmphex(qtest_readb(qts, views[i]), ==, 0xff);
        g_assert_cmphex(qtest_readw(qts, views[i]), ==, 0xffff);
        g_assert_cmphex(qtest_readl(qts, views[i]), ==, 0xffffffff);
        g_assert_cmphex(qtest_readq(qts, views[i]), ==, UINT64_MAX);
        g_assert_cmphex(qtest_readq(qts,
                                    views[i] + FLASH_OBSERVED_OFFSET), ==,
                        UINT64_MAX);
        g_assert_cmphex(qtest_readq(qts,
                                    views[i] + FLASH_INTERIOR_OFFSET), ==,
                        UINT64_MAX);
        g_assert_cmphex(qtest_readq(qts, views[i] +
                                    SGI_BEDROCK_FLASH_PART_SIZE - 8), ==,
                        UINT64_MAX);
        g_assert_cmphex(qtest_readb(qts, views[i] +
                                    SGI_BEDROCK_FLASH_PART_SIZE - 1), ==,
                        0xff);
        assert_flat_view_unmapped(qts, views[i] +
                                  SGI_BEDROCK_FLASH_PART_SIZE);
    }
}

/*
 * The controller speaks the same framed protocol through the machine's mapped
 * window as it does on its own bus, so these helpers mirror the device
 * test's, addressed at the window instead.
 */
static uint16_t l1_fcs(uint16_t value, uint8_t byte)
{
    value ^= byte;
    for (unsigned bit = 0; bit < 8; bit++) {
        value = (value & 1) ? (value >> 1) ^ 0x8408 : value >> 1;
    }
    return value;
}

static void l1_put(QTestState *qts, uint8_t byte)
{
    if (byte == 0x7e || byte == 0x7d) {
        qtest_writeq(qts, L1_UART_BASE, 0x7d);
        byte ^= 0x20;
    }
    qtest_writeq(qts, L1_UART_BASE, byte);
}

static void l1_send(QTestState *qts, const uint8_t *frame, size_t length)
{
    uint16_t crc = 0xffff;

    qtest_writeq(qts, L1_UART_BASE, 0x7e);
    for (size_t i = 0; i < length; i++) {
        crc = l1_fcs(crc, frame[i]);
        l1_put(qts, frame[i]);
    }
    crc = ~crc;
    l1_put(qts, crc);
    l1_put(qts, crc >> 8);
    qtest_writeq(qts, L1_UART_BASE, 0x7e);
}

static size_t l1_receive(QTestState *qts, uint8_t *reply)
{
    size_t length = 0;
    bool escape = false;
    uint16_t crc = 0xffff;

    g_assert_cmphex(qtest_readq(qts, L1_UART_BASE + L1_UART_STATUS), ==,
                    L1_STATUS_READY);
    g_assert_cmphex(qtest_readq(qts, L1_UART_BASE), ==, 0x7e);
    for (;;) {
        uint8_t byte;

        g_assert_cmphex(qtest_readq(qts, L1_UART_BASE + L1_UART_STATUS), ==,
                        L1_STATUS_READY);
        byte = qtest_readq(qts, L1_UART_BASE);
        if (byte == 0x7e) {
            break;
        }
        if (byte == 0x7d && !escape) {
            escape = true;
            continue;
        }
        if (escape) {
            byte ^= 0x20;
            escape = false;
        }
        g_assert_cmpuint(length, <, 131);
        reply[length++] = byte;
        crc = l1_fcs(crc, byte);
    }
    g_assert_false(escape);
    g_assert_cmpuint(length, >=, 3);
    g_assert_cmphex(crc, ==, 0xf0b8);
    return length - 2;
}

static size_t l1_eeprom_read(QTestState *qts, unsigned component,
                             unsigned area, unsigned offset, unsigned length,
                             uint8_t *reply)
{
    uint8_t request[28] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 6, 4 };
    const unsigned values[] = { component, area, offset, length };

    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        stl_be_p(request + 9 + 5 * i, values[i]);
    }
    l1_send(qts, request, sizeof(request));
    return l1_receive(qts, reply);
}

/*
 * base-parameterized mirrors of l1_put/l1_send/l1_receive/l1_eeprom_read, so
 * a caller can drive the same framed protocol through either register-window
 * form's L1 UART slot and compare the results.
 */
static void l1_put_at(QTestState *qts, uint64_t base, uint8_t byte)
{
    if (byte == 0x7e || byte == 0x7d) {
        qtest_writeq(qts, base, 0x7d);
        byte ^= 0x20;
    }
    qtest_writeq(qts, base, byte);
}

static void l1_send_at(QTestState *qts, uint64_t base, const uint8_t *frame,
                       size_t length)
{
    uint16_t crc = 0xffff;

    qtest_writeq(qts, base, 0x7e);
    for (size_t i = 0; i < length; i++) {
        crc = l1_fcs(crc, frame[i]);
        l1_put_at(qts, base, frame[i]);
    }
    crc = ~crc;
    l1_put_at(qts, base, crc);
    l1_put_at(qts, base, crc >> 8);
    qtest_writeq(qts, base, 0x7e);
}

static size_t l1_receive_at(QTestState *qts, uint64_t base, uint8_t *reply)
{
    size_t length = 0;
    bool escape = false;
    uint16_t crc = 0xffff;

    g_assert_cmphex(qtest_readq(qts, base + L1_UART_STATUS), ==,
                    L1_STATUS_READY);
    g_assert_cmphex(qtest_readq(qts, base), ==, 0x7e);
    for (;;) {
        uint8_t byte;

        g_assert_cmphex(qtest_readq(qts, base + L1_UART_STATUS), ==,
                        L1_STATUS_READY);
        byte = qtest_readq(qts, base);
        if (byte == 0x7e) {
            break;
        }
        if (byte == 0x7d && !escape) {
            escape = true;
            continue;
        }
        if (escape) {
            byte ^= 0x20;
            escape = false;
        }
        g_assert_cmpuint(length, <, 131);
        reply[length++] = byte;
        crc = l1_fcs(crc, byte);
    }
    g_assert_false(escape);
    g_assert_cmpuint(length, >=, 3);
    g_assert_cmphex(crc, ==, 0xf0b8);
    return length - 2;
}

static size_t l1_eeprom_read_at(QTestState *qts, uint64_t base,
                                unsigned component, unsigned area,
                                unsigned offset, unsigned length,
                                uint8_t *reply)
{
    uint8_t request[28] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 6, 4 };
    const unsigned values[] = { component, area, offset, length };

    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        stl_be_p(request + 9 + 5 * i, values[i]);
    }
    l1_send_at(qts, base, request, sizeof(request));
    return l1_receive_at(qts, base, reply);
}

/*
 * The qtest protocol itself has no per-CPU physical access: every qtest_read
 * call and every human-monitor-command "xp" dispatch goes through the first
 * CPU's own address space regardless of a requested cpu-index in this build.
 * A per-CPU register-window defect (like CPU C's own overlay shadowing the
 * machine-wide UART alias) is therefore invisible to any such check; only a
 * ROM a CPU actually executes proves what that CPU's own view resolves to.
 */
#define L1_LOCAL_FORM_READY_MARKER UINT64_C(0x1a57)

static char *create_l1_local_form_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* branch to 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* park fault vectors */
    }
    stl_be_p(image + 0x400, 0x400a6000); /* mfc0 t2, Status */
    stl_be_p(image + 0x404, 0x354a0080); /* enable KX */
    stl_be_p(image + 0x408, 0x408a6000); /* mtc0 t2, Status */
    stl_be_p(image + 0x40c, 0x000000c0); /* ehb */
    /*
     * Every processor computes the same uncached local register-window L1
     * UART line-status address (0x9000_0000_1000_0080 + L1_UART_STATUS) and
     * loads it independently; cpu_gpr() then reads each one's own copy, the
     * way a real console driver running on any populated slot would see its
     * own load's result.
     */
    stl_be_p(image + 0x410, 0x3c029000); /* lui v0, 0x9000 */
    stl_be_p(image + 0x414, 0x3c031000); /* lui v1, local window */
    stl_be_p(image + 0x418, 0x346300a8); /* ori v1, v1, UART + LSR */
    stl_be_p(image + 0x41c, 0x0002103c); /* dsll32 v0, v0, 0 */
    stl_be_p(image + 0x420, 0x00431025); /* or v0, v0, v1 */
    stl_be_p(image + 0x424, 0xdc530000); /* ld s3, 0(v0) */
    stl_be_p(image + 0x428, 0x34150000 | L1_LOCAL_FORM_READY_MARKER);
    stl_be_p(image + 0x42c, 0x1000ffff); /* park */
    stl_be_p(image + 0x430, 0x00000000);

    fd = g_file_open_tmp("sn1-l1-local-form-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_hub_local_l1_uart_reaches_cpu_c(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    path = create_l1_local_form_prom();
    qts = machine_start_with(path, "-accel tcg,thread=multi -S");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr(qts, 0, "s5", L1_LOCAL_FORM_READY_MARKER);
    wait_for_cpu_gpr(qts, 1, "s5", L1_LOCAL_FORM_READY_MARKER);
    stop_vm(qts);

    /*
     * CPU C (logical index 1, physical slot C) sits on the second processor
     * interface and has its own crosstalk-form PI1 overlay; the Hub-special
     * window has no per-slot view, so C reaches the same machine-wide UART as
     * CPU A.
     */
    g_assert_cmphex(cpu_gpr(qts, 0, "s3") & 0xff, ==, 0x60);
    g_assert_cmphex(cpu_gpr(qts, 1, "s3") & 0xff, ==, 0x60);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_hub_local_reaches_l1_uart(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0xa5);
    uint64_t cached_alias;
    uint8_t reply_local[131];
    uint8_t reply_remote[131];
    size_t length_local;
    size_t length_remote;

    /* Idle line status, read before any protocol byte crosses either form. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_UART_STATUS), ==, 0x60);

    /* The same framed request gets the same reply through either form. */
    length_local = l1_eeprom_read_at(qts, HUB_LOCAL_REGISTERS + L1_UART,
                                     L1_EEPROM_BOARD, 0, 0, 16, reply_local);
    length_remote = l1_eeprom_read_at(qts, HUB_REMOTE_REGISTERS + L1_UART,
                                      L1_EEPROM_BOARD, 0, 0, 16, reply_remote);
    g_assert_cmpuint(length_local, ==, length_remote);
    g_assert_cmpmem(reply_local, length_local, reply_remote, length_remote);

    /*
     * Scratch written through the local form reads back through the remote.
     */
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + L1_UART + L1_SCRATCH, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);

    /*
     * The blocks on either side of the UART do not disturb the scratch just
     * proven above: a store at its 0x78 edge lands in the unidentified block,
     * and one at its 0xc0 edge in slice 0's LED.
     */
    cached_alias = qtest_readq(qts, BEDROCK_LOCAL + PI_CACHED_ALIAS);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + PI_NMI_B, 0x100);
    qtest_writeq(qts, HUB_LOCAL_REGISTERS + HUB_LED(0), 0x5a);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + HUB_LED(0)), ==,
                    0x5a);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CACHED_ALIAS), ==,
                    cached_alias);

    qtest_quit(qts);
}

static char *write_record(const char *directory, const char *name,
                          const uint8_t *bytes, size_t length)
{
    g_autoptr(GError) error = NULL;
    char *path = g_build_filename(directory, name, NULL);

    g_assert_true(g_file_set_contents(path, (const char *)bytes, length,
                                      &error));
    g_assert_no_error(error);
    return path;
}

static void test_l1_identity_records_come_from_the_command_line(
        const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-identity-XXXXXX", &error);
    g_autofree char *chassis_path = NULL;
    g_autofree char *board_path = NULL;
    g_autofree char *spd_path = NULL;
    g_autofree char *extra = NULL;
    /*
     * Synthetic bytes owned by this test. The raw chassis and board areas
     * each sum to zero; SPD byte 63 equals the sum of bytes 0 to 62.
     */
    uint8_t chassis[24], board[40], spd[128] = { 0 };
    uint8_t reply[131];
    QTestState *qts;

    g_assert_no_error(error);
    memset(chassis, 0xc1, sizeof(chassis));
    memset(board, 0xb2, sizeof(board));
    chassis[sizeof(chassis) - 1] = 0xa9;
    board[sizeof(board) - 1] = 0xe2;
    spd[0] = 64;
    spd[63] = 64;
    memset(spd + 64, 0x5d, 64);
    chassis_path = write_record(directory, "chassis.bin", chassis,
                                sizeof(chassis));
    board_path = write_record(directory, "board.bin", board, sizeof(board));
    spd_path = write_record(directory, "spd.bin", spd, sizeof(spd));

    extra = g_strdup_printf("-M origin300,chassis-eeprom.0=%s,"
                            "board-eeprom.0=%s,spd-eeprom.0=%s "
                            "-cpu R14000 -smp 2 -bios %s -display none",
                            chassis_path, board_path, spd_path, prom_path);
    qts = qtest_init(extra);

    /* Each named file answers as component zero of its own area. */
    g_assert_cmpuint(l1_eeprom_read(qts, 0, L1_EEPROM_CHASSIS, 0,
                                    sizeof(chassis), reply), ==,
                     12 + sizeof(chassis));
    g_assert_cmpmem(reply + 12, sizeof(chassis), chassis, sizeof(chassis));
    g_assert_cmpuint(l1_eeprom_read(qts, 0, L1_EEPROM_BOARD, 0, sizeof(board),
                                    reply), ==, 12 + sizeof(board));
    g_assert_cmpmem(reply + 12, sizeof(board), board, sizeof(board));
    g_assert_cmpuint(l1_eeprom_read(qts, 0, L1_EEPROM_SPD, 64, 64, reply),
                     ==, 76);
    g_assert_cmpmem(reply + 12, 64, spd + 64, 64);

    /* Nothing was invented for a second module. */
    g_assert_cmpuint(l1_eeprom_read(qts, 1, L1_EEPROM_SPD, 0, 1, reply), ==,
                     6);
    g_assert_cmphex(reply[4], ==, L1_REPLY_ABSENT);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(chassis_path), ==, 0);
    g_assert_cmpint(g_unlink(board_path), ==, 0);
    g_assert_cmpint(g_unlink(spd_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_l1_has_no_identity_records_by_default(const void *data)
{
    QTestState *qts = machine_start();
    uint8_t reply[131];

    /* No file, no record: the machine supplies nothing of its own. */
    for (unsigned area = 1; area <= 4; area++) {
        g_assert_cmpuint(l1_eeprom_read(qts, 0, area, 0, 1, reply), ==, 6);
        g_assert_cmphex(reply[4], ==, L1_REPLY_ABSENT);
    }

    qtest_quit(qts);
}

/* The 16-byte CIA and 80-byte BIA use the documented field boundaries. */
static const uint8_t cia[16] = {
    0x00, 0x02, 0x17, 0xc2, 0x51, 0x41, 0xc2, 0x5a,
    0x5a, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c,
};

static const uint8_t bia[80] = {
    0x00, 0x0a, 0x00, 0x00, 0x01, 0x00, 0xc9, 0x54,
    0x45, 0x53, 0x54, 0x4c, 0x41, 0x42, 0x30, 0x31,
    0xc3, 0x49, 0x4f, 0x38, 0xc6, 0x51, 0x53, 0x54,
    0x30, 0x30, 0x31, 0xcc, 0x30, 0x30, 0x30, 0x5f,
    0x54, 0x45, 0x53, 0x54, 0x5f, 0x42, 0x52, 0x44,
    0x00, 0xc2, 0x51, 0x41, 0x00, 0x00, 0xc2, 0x30,
    0x31, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0xcc, 0x30, 0x32, 0x30, 0x30, 0x35, 0x44, 0x41,
    0x41, 0x42, 0x42, 0x43, 0x43, 0xc1, 0x00, 0xd1,
};

static uint8_t sum_bytes(const uint8_t *bytes, size_t length)
{
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum;
}

/* The 12-character address field starts after byte 64's ASCII tag. */
static bool board_matches_mac(const uint8_t *board, const char *mac)
{
    char parsed[18];

    if (board[64] != 0xcc || board[77] != 0xc1 || board[78] != 0 ||
        sum_bytes(board, 80) != 0) {
        return false;
    }
    for (unsigned i = 0; i < 12; i++) {
        if (!g_ascii_isxdigit(board[65 + i])) {
            return false;
        }
    }
    g_snprintf(parsed, sizeof(parsed), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
               board[65], board[66], board[67], board[68],
               board[69], board[70], board[71], board[72],
               board[73], board[74], board[75], board[76]);
    return g_ascii_strcasecmp(parsed, mac) == 0;
}

/*
 * A second module's chassis and board records, read back through the same
 * framed protocol l1_eeprom_read() already drives for module zero.
 */
static void test_configured_mac_in_board_record(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-io8-mac-XXXXXX", &error);
    g_autofree char *blank_prom_path = NULL;
    g_autofree char *chassis_path = NULL;
    g_autofree char *board_path = NULL;
    g_autofree char *extra = NULL;
    g_autofree uint8_t *prom = g_malloc0(PROM_SIZE);
    uint8_t record[sizeof(bia)], reply[131], board[sizeof(bia)];
    g_autoptr(QDict) result = NULL;
    QTestState *qts;
    const char *actual_mac;

    g_assert_no_error(error);
    memcpy(record, bia, sizeof(record));
    blank_prom_path = write_record(directory, "prom.bin", prom, PROM_SIZE);
    chassis_path = write_record(directory, "chassis.bin", cia, sizeof(cia));
    board_path = write_record(directory, "board.bin", record,
                              sizeof(record));
    extra = g_strdup_printf(
        "-M origin300,chassis-eeprom.0=,chassis-eeprom.1=%s,"
        "board-eeprom.0=,board-eeprom.1=%s "
        "-cpu R14000 -smp 2 -bios %s -display none -S "
        "-nic user,mac=" MAC,
        chassis_path, board_path, blank_prom_path);
    qts = qtest_init(extra);

    /*
     * This property reports -nic's configured MAC, not a BIA-derived value.
     */
    result = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                           "{'path':'" IOC3_PATH "','property':'mac'}}");
    g_assert_true(qdict_haskey(result, "return"));
    actual_mac = qdict_get_str(result, "return");
    g_assert_cmpstr(actual_mac, ==, MAC);

    g_assert_cmpuint(l1_eeprom_read(qts, 1, L1_EEPROM_CHASSIS, 0, sizeof(cia),
                                    reply), ==, 12 + sizeof(cia));
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmphex(reply[5], ==, 2);
    g_assert_cmphex(reply[10], ==, 0);
    g_assert_cmphex(reply[11], ==, 0x80 | sizeof(cia));
    g_assert_cmpmem(reply + 12, sizeof(cia), cia, sizeof(cia));
    g_assert_cmphex(reply[12 + 9], ==, 0xc1);
    g_assert_cmphex(sum_bytes(reply + 12, sizeof(cia)), ==, 0);

    g_assert_cmpuint(l1_eeprom_read(qts, 1, L1_EEPROM_BOARD, 0, sizeof(bia),
                                    reply), ==, 12 + sizeof(bia));
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmphex(reply[5], ==, 2);
    g_assert_cmphex(reply[10], ==, 0);
    g_assert_cmphex(reply[11], ==, 0x80 | sizeof(bia));
    g_assert_cmpmem(reply + 12, sizeof(bia), record, sizeof(record));
    memcpy(board, reply + 12, sizeof(board));
    g_assert_cmphex(board[49], ==, 0x04);
    g_assert_cmphex(board[54], ==, 0x04);
    g_assert_cmphex(board[59], ==, 0x04);
    g_assert_cmphex(board[64], ==, 0xcc);
    g_assert_cmphex(board[77], ==, 0xc1);
    g_assert_cmphex(board[78], ==, 0);
    g_assert_cmphex(sum_bytes(board, sizeof(board)), ==, 0);
    g_assert_true(board_matches_mac(board, actual_mac));

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(blank_prom_path), ==, 0);
    g_assert_cmpint(g_unlink(chassis_path), ==, 0);
    g_assert_cmpint(g_unlink(board_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static char *create_flash_image(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc(SGI_BEDROCK_FLASH_SIZE);
    char *path = NULL;
    int fd;

    /*
     * The part holds the supplied payload followed by erased bytes, which is
     * the shape the harness seeds. Two payload bytes are left erased so the
     * test can program inside the range the reset view covers.
     */
    memset(image, 0xff, SGI_BEDROCK_FLASH_SIZE);
    memset(image, 0, PROM_SIZE);
    image[0] = FLASH_LOWER_FIRST;
    image[PROM_CONFIG_OFFSET] = FLASH_LOWER_CONFIG;
    image[PROM_SIZE - 1] = FLASH_LOWER_LAST;
    image[FLASH_PAYLOAD_TARGET] = 0xff;
    image[FLASH_PAYLOAD_TARGET + 1] = 0xff;
    image[SGI_BEDROCK_FLASH_PART_SIZE] = FLASH_UPPER_FIRST;
    image[SGI_BEDROCK_FLASH_PART_SIZE + 1] = FLASH_UPPER_LAST;
    image[SGI_BEDROCK_FLASH_SIZE - 1] = FLASH_UPPER_LAST;

    fd = g_file_open_tmp("sn1-boot-flash-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, SGI_BEDROCK_FLASH_SIZE), ==,
                    (ssize_t)SGI_BEDROCK_FLASH_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void remove_flash_image(const char *flash)
{
    g_assert_cmpint(g_unlink(flash), ==, 0);
}

static QTestState *machine_start_with_flash(const char *flash,
                                            const char *extra)
{
    g_autofree char *prom_quoted = g_shell_quote(prom_path);
    g_autofree char *flash_quoted = g_shell_quote(flash);

    return qtest_initf(MACHINE_ARGS " -bios %s "
                       "-drive if=pflash,file=%s,format=raw %s",
                       prom_quoted, flash_quoted, extra);
}

/* One 16-bit word per bus cycle, at the bus offset four times its byte. */
static void flash_program(QTestState *qts, uint64_t selection,
                          uint64_t offset, uint16_t word)
{
    qtest_writeq(qts, selection + FLASH_UNLOCK0, 0xaa);
    qtest_writeq(qts, selection + FLASH_UNLOCK1, 0x55);
    qtest_writeq(qts, selection + FLASH_UNLOCK0, 0xa0);
    qtest_writeq(qts, selection + offset * 4, word);
}

static void flash_reset_to_array(QTestState *qts, uint64_t selection)
{
    qtest_writeq(qts, selection + FLASH_UNLOCK0, 0xf0);
}

static void flash_enter_query(QTestState *qts, uint64_t selection)
{
    qtest_writeq(qts, selection + FLASH_QUERY, 0x98);
}

/*
 * Every view onto position zero. The second selection is deliberately absent:
 * it is position one, a different part with its own bytes, and the old
 * contract that mirrored position zero there is superseded.
 */
static void assert_flash_everywhere(QTestState *qts, uint64_t offset,
                                    uint16_t word)
{
    static const uint64_t views[] = {
        HUB_LOCAL_BOOT, PROM_RESET, HUB_LOCAL_HIGH_BOOT, HUB_REMOTE_BOOT,
    };

    static const uint64_t second[] = { HUB_LOCAL_FLASH, HUB_REMOTE_FLASH };

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        g_assert_cmphex(qtest_readw(qts, views[i] + offset), ==, word);
    }
    /* Neither second-selection window answers with position zero's word. */
    for (size_t i = 0; i < ARRAY_SIZE(second); i++) {
        g_assert_cmphex(qtest_readw(qts, second[i] + offset), !=, word);
    }
}

/*
 * Position one answers both boot windows at their second PROM address. The
 * remote window is asserted alongside the local one because a machine that
 * mapped the second selection into the local space alone would pass every
 * local check.
 */
static void assert_position_one(QTestState *qts, uint64_t offset,
                                uint16_t word)
{
    g_assert_cmphex(qtest_readw(qts, HUB_LOCAL_FLASH + offset), ==, word);
    g_assert_cmphex(qtest_readw(qts, HUB_REMOTE_FLASH + offset), ==, word);
}

static void test_board_identification(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-board-id-log-XXXXXX.txt", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", quoted);
    qts = machine_start_with(prom_path, extra);

    /*
     * Both consumers reach one response: the firmware's form and OpenBSD's.
     */
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_REMOTE), ==, BOARD_ID_VALUE);

    /* A write cannot change what it answers. */
    qtest_writeq(qts, BOARD_ID_LOCAL, UINT64_C(0xdeadbeef));
    qtest_writeq(qts, BOARD_ID_REMOTE, UINT64_C(0xfeedface));
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_REMOTE), ==, BOARD_ID_VALUE);

    /*
     * The low 32-bit lane is where the product nibble would appear if a
     * narrow access were served by splitting the wide one, so reading it is
     * what tells a refusal apart from a chunked read.
     */
    g_assert_cmphex(qtest_readl(qts, BOARD_ID_LOCAL + 4), !=, BOARD_ID_VALUE);

    /* Neither the word beside it nor the next block answers. */
    assert_flat_view_unmapped(qts, BOARD_ID_LOCAL + 8);
    assert_flat_view_unmapped(qts, BOARD_ID_UNUSED);

    /*
     * The response is fixed rather than stored, so reset cannot disturb it.
     */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(qts, BOARD_ID_REMOTE), ==, BOARD_ID_VALUE);

    qtest_quit(qts);

    /* Every refusal said so, with the address and the width it refused. */
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "refusing a 8-byte write"));
    g_assert_nonnull(strstr(log, "refusing 4 bytes at offset 0x4"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The ECC back door answers where the contract puts it, over an extent taken
 * from installed memory rather than from the aperture, and nothing answers on
 * either side. A machine that mapped the region but left the controller's
 * memory link unset would give a zero extent and refuse its own first byte,
 * so the accepted access at offset zero is what proves the link.
 */
static void test_ecc_backdoor_window(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-ecc-log-XXXXXX.txt", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", quoted);
    qts = machine_start_with(prom_path, extra);
    machine_configure_memory(qts);

    /*
     * The first byte and the last byte of the installed extent both answer.
     */
    g_assert_cmphex(qtest_readb(qts, HUB_ECC), ==, 0);
    g_assert_cmphex(qtest_readb(qts, HUB_ECC + ECC_EXTENT - 1), ==, 0);
    /* An aligned halfword is the other width the carrier takes. */
    g_assert_cmphex(qtest_readw(qts, HUB_ECC + ECC_EXTENT - 2), ==, 0);

    /*
     * One byte past the installed extent is refused while still inside the
     * aperture, which is what separates the two bounds from each other.
     */
    qtest_readb(qts, HUB_ECC + ECC_EXTENT);

    /*
     * The far side of the aperture is not mapped, so the region has not been
     * stretched to fill the space. The word below the base belongs to the
     * scrub carrier at four eighths of the node, not to this region.
     */
    assert_flat_view_is_io(qts, HUB_ECC - 8);
    assert_flat_view_unmapped(qts, HUB_ECC + ECC_APERTURE);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    /* The one past the extent names which bound it broke. */
    g_assert_nonnull(strstr(log, "outside fitted memory"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_directory_backdoor_window(const void *data)
{
    QTestState *qts = machine_start();
    /*
     * A format-masked word beside an opaque one, both distinctive. The entry
     * is a standard-format codeword (check field 0x0f at bits 21:16), so the
     * check field the unit regenerates on a write leaves it as is.
     */
    const uint64_t entry = UINT64_C(0xc00fd00d);
    const uint64_t counter = UINT64_C(0x0123456789abcdef);
    const uint64_t carrier = HUB_DIRECTORY + DIRECTORY_LINE_1A00000;
    const uint64_t last = HUB_DIRECTORY + DIRECTORY_LAST_FITTED;
    const uint64_t absent = HUB_DIRECTORY + DIRECTORY_FIRST_ABSENT;

    machine_configure_memory(qts);

    /*
     * The window answers at the carrier the contract names, which needs the
     * machine's memory linked to the controller for the device to realize.
     */
    g_assert_cmphex(qtest_readq(qts, carrier), ==, 0);
    qtest_writeq(qts, carrier, entry);
    qtest_writeq(qts, carrier + 8, counter);

    /* The two words of one carrier are independent storage. */
    g_assert_cmphex(qtest_readq(qts, carrier), ==, entry);
    g_assert_cmphex(qtest_readq(qts, carrier + 8), ==, counter);

    /* Neither leaks into the carriers on either side. */
    g_assert_cmphex(qtest_readq(qts, carrier - 0x10), ==, 0);
    g_assert_cmphex(qtest_readq(qts, carrier - 0x10 + 8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, carrier + 0x10), ==, 0);
    g_assert_cmphex(qtest_readq(qts, carrier + 0x10 + 8), ==, 0);

    /* The memory this machine fits reaches exactly this far. */
    qtest_writeq(qts, last, entry);
    qtest_writeq(qts, last + 8, counter);
    g_assert_cmphex(qtest_readq(qts, last), ==, entry);
    g_assert_cmphex(qtest_readq(qts, last + 8), ==, counter);

    /* One carrier further is past the fitted memory and is refused. */
    qtest_writeq(qts, absent, UINT64_C(0x13579bdf2468ace0));
    qtest_writeq(qts, absent + 8, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(qts, absent), ==, 0);
    g_assert_cmphex(qtest_readq(qts, absent + 8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, last), ==, entry);
    g_assert_cmphex(qtest_readq(qts, last + 8), ==, counter);

    /* The aperture is the last part of the node's address space. */
    assert_flat_view_unmapped(qts, HUB_DIRECTORY + DIRECTORY_APERTURE);

    qtest_system_reset(qts);
    machine_configure_memory(qts);
    g_assert_cmphex(qtest_readq(qts, carrier), ==, 0);
    g_assert_cmphex(qtest_readq(qts, carrier + 8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, last), ==, 0);
    g_assert_cmphex(qtest_readq(qts, last + 8), ==, 0);

    qtest_quit(qts);
}

static void test_directory_protection(const void *data)
{
    const uint64_t pattern = UINT64_C(0x599a599a599a599a);
    QTestState *qts = machine_start_with(prom_path, "-m 512M");

    qtest_writeq(qts, HUB_DIRECTORY + DIRECTORY_PROTECTION_BANK1_HIGH,
                 pattern);
    g_assert_cmphex(qtest_readq(qts,
                               HUB_DIRECTORY +
                               DIRECTORY_PROTECTION_BANK1_HIGH),
                    !=, pattern);

    qtest_writeq(qts, HUB_DIRECTORY + DIRECTORY_FIRST_ABSENT, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts,
                               HUB_DIRECTORY + DIRECTORY_FIRST_ABSENT),
                    ==, 0);
    qtest_quit(qts);
}

static void test_boot_flash_backs_every_prom_view(const void *data)
{
    g_autofree char *flash = create_flash_image();
    g_autofree char *ramblocks = NULL;
    QTestState *qts = machine_start_with_flash(flash, "");

    ramblocks = qtest_hmp(qts, "info ramblock");
    g_assert_nonnull(strstr(ramblocks, "sn1.prom.reset"));
    g_assert_nonnull(strstr(ramblocks, "sn1.prom.local-high"));
    assert_flat_view_is_execution_storage(qts, PROM_RESET);
    assert_flat_view_is_execution_storage(qts, HUB_LOCAL_HIGH_BOOT);

    /* Every execution view reads position zero, not the supplied payload. */
    assert_flash_everywhere(qts, 0, FLASH_LOWER_WORD);
    g_assert_cmphex(qtest_readb(qts, HUB_LOCAL_BOOT + PROM_SIZE - 1), ==,
                    FLASH_LOWER_LAST);

    /* Both boot windows reach position one at their second PROM address. */
    assert_position_one(qts, 0, FLASH_UPPER_WORD);
    g_assert_cmphex(qtest_readb(qts, HUB_LOCAL_FLASH +
                                SGI_BEDROCK_FLASH_PART_SIZE - 1), ==,
                    FLASH_UPPER_LAST);
    g_assert_cmphex(qtest_readb(qts, HUB_REMOTE_FLASH +
                                SGI_BEDROCK_FLASH_PART_SIZE - 1), ==,
                    FLASH_UPPER_LAST);

    /* Position zero's payload ends in erased bytes inside its own part. */
    g_assert_cmphex(qtest_readb(qts, HUB_LOCAL_BOOT + PROM_SIZE), ==, 0xff);

    /*
     * The alternate selection is a read window inside the boot space rather
     * than a region of its own, so the space is what ends, not the window.
     */
    assert_flat_view_unmapped(qts, HUB_LOCAL_BOOT + BOOT_SPACE_SIZE);
    assert_flat_view_unmapped(qts, HUB_REMOTE_BOOT + BOOT_SPACE_SIZE);

    /*
     * Commands are addressed from a position's own base, and one part answers
     * both boot spaces, so a program through the local space is visible
     * through the remote one. The other position keeps its own bytes.
     */
    flash_program(qts, HUB_LOCAL_BOOT, FLASH_ERASED_TARGET, 0x1234);
    g_assert_cmphex(qtest_readw(qts, HUB_LOCAL_BOOT + FLASH_ERASED_TARGET),
                    ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, HUB_REMOTE_BOOT + FLASH_ERASED_TARGET),
                    ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, HUB_LOCAL_FLASH + FLASH_ERASED_TARGET),
                    ==, 0xffff);

    /*
     * A command sent to a second-selection window addresses position one from
     * that window's own base, so the store compression puts it at the same
     * offset in the other part rather than 8 MiB into a single one. That is
     * what separates this machine from the superseded mirror. Either window
     * drives the part, and neither disturbs position zero.
     */
    flash_program(qts, HUB_LOCAL_FLASH, FLASH_ERASED_TARGET, 0x5678);
    assert_position_one(qts, FLASH_ERASED_TARGET, 0x5678);
    flash_program(qts, HUB_REMOTE_FLASH, FLASH_ERASED_TARGET + 2, 0xbeef);
    assert_position_one(qts, FLASH_ERASED_TARGET + 2, 0xbeef);
    g_assert_cmphex(qtest_readw(qts, HUB_LOCAL_BOOT + FLASH_ERASED_TARGET),
                    ==, 0x1234);

    /* What a monitor writes into the first sector is what a reset fetches. */
    flash_program(qts, HUB_LOCAL_BOOT, FLASH_PAYLOAD_TARGET, 0x9abc);
    assert_flash_everywhere(qts, FLASH_PAYLOAD_TARGET, 0x9abc);

    /* A reset returns both parts to array reads without disturbing bytes. */
    qtest_system_reset(qts);
    assert_flash_everywhere(qts, FLASH_PAYLOAD_TARGET, 0x9abc);
    g_assert_cmphex(qtest_readw(qts, HUB_LOCAL_BOOT + FLASH_ERASED_TARGET), ==,
                    0x1234);
    assert_position_one(qts, FLASH_ERASED_TARGET, 0x5678);

    qtest_quit(qts);

    /* Each position's backing file keeps its own changes across a restart. */
    qts = machine_start_with_flash(flash, "");
    assert_flash_everywhere(qts, FLASH_PAYLOAD_TARGET, 0x9abc);
    g_assert_cmphex(qtest_readw(qts, HUB_LOCAL_BOOT + FLASH_ERASED_TARGET),
                    ==, 0x1234);
    assert_position_one(qts, FLASH_ERASED_TARGET, 0x5678);
    assert_position_one(qts, FLASH_ERASED_TARGET + 2, 0xbeef);
    qtest_quit(qts);
    remove_flash_image(flash);
}

/*
 * A complete native command sequence aimed at an address the processor
 * executes from is refused, says so, and leaves the stored bytes and the
 * part's own mode alone. This is the case a writable alias of the part's
 * region passes by accident: an alias forwards a write to its target even
 * when it is marked read-only, so the part would accept the sequence and stop
 * answering with array data underneath the running firmware.
 */
static void test_execution_views_refuse_commands(const void *data)
{
    static const uint64_t views[] = { PROM_RESET, HUB_LOCAL_HIGH_BOOT };
    g_autofree char *flash = create_flash_image();
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-exec-view-log-XXXXXX.txt", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d guest_errors -D %s", quoted);
    qts = machine_start_with_flash(flash, extra);

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        qtest_writew(qts, views[i] + FLASH_NATIVE_UNLOCK0, 0xaa);
        qtest_writew(qts, views[i] + FLASH_NATIVE_UNLOCK1, 0x55);
        qtest_writew(qts, views[i] + FLASH_NATIVE_UNLOCK0, 0xa0);
        qtest_writew(qts, views[i] + FLASH_PAYLOAD_TARGET, 0x1234);

        /* The word it aimed at is still erased. */
        g_assert_cmphex(qtest_readw(qts, views[i] + FLASH_PAYLOAD_TARGET), ==,
                        0xffff);
        /*
         * And the part still returns its first stored word rather than a
         * status or an identifier, so it never left read-array mode. That is
         * the half of the check that a dropped write alone would not prove.
         */
        g_assert_cmphex(qtest_readw(qts, views[i]), ==, FLASH_LOWER_WORD);
    }

    /* Every view still reads the part. */
    assert_flash_everywhere(qts, 0, FLASH_LOWER_WORD);

    /* And the space the part is really driven from still programs it. */
    flash_program(qts, HUB_LOCAL_BOOT, FLASH_PAYLOAD_TARGET, 0x1234);
    assert_flash_everywhere(qts, FLASH_PAYLOAD_TARGET, 0x1234);
    qtest_quit(qts);

    /* Each refusal named the view it came from. */
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "sn1.prom.reset execution view: refusing"));
    g_assert_nonnull(strstr(log,
                            "sn1.prom.local-high execution view: refusing"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    remove_flash_image(flash);
}

static void test_boot_flash_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-boot-flash-migration-XXXXXX", &error);
    g_autofree char *source_flash = create_flash_image();
    g_autofree char *destination_flash = create_flash_image();
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from;
    QTestState *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = machine_start_with_flash(source_flash, "");
    to = machine_start_with_flash(destination_flash, "-incoming defer");

    flash_program(from, HUB_LOCAL_BOOT, FLASH_PAYLOAD_TARGET, 0x9abc);
    flash_program(from, HUB_LOCAL_FLASH, FLASH_ERASED_TARGET, 0x5678);
    g_assert_cmphex(qtest_readw(to, PROM_RESET + FLASH_PAYLOAD_TARGET), ==,
                    0xffff);

    /* Migrate distinct in-flight command states for both positions. */
    qtest_writeq(from, HUB_LOCAL_BOOT + FLASH_UNLOCK0, 0xaa);
    qtest_writeq(from, HUB_LOCAL_BOOT + FLASH_UNLOCK1, 0x55);
    qtest_writeq(from, HUB_LOCAL_BOOT + FLASH_UNLOCK0, 0xa0);
    flash_enter_query(from, HUB_REMOTE_FLASH);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);

    qtest_writeq(to, HUB_REMOTE_BOOT + FLASH_ERASED_TARGET * 4, 0x1234);
    g_assert_cmphex(qtest_readw(to, HUB_LOCAL_BOOT + FLASH_ERASED_TARGET), ==,
                    0x1234);
    g_assert_cmphex(qtest_readw(to,
                                HUB_LOCAL_FLASH + FLASH_QUERY_READ(0x10)), ==,
                    'Q');
    flash_reset_to_array(to, HUB_LOCAL_FLASH);
    assert_flash_everywhere(to, FLASH_PAYLOAD_TARGET, 0x9abc);
    assert_position_one(to, FLASH_ERASED_TARGET, 0x5678);

    qtest_system_reset(to);
    assert_flash_everywhere(to, FLASH_PAYLOAD_TARGET, 0x9abc);
    g_assert_cmphex(qtest_readw(to, HUB_REMOTE_BOOT + FLASH_ERASED_TARGET), ==,
                    0x1234);
    assert_position_one(to, FLASH_ERASED_TARGET, 0x5678);

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    remove_flash_image(source_flash);
    remove_flash_image(destination_flash);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

static char *create_sized_flash_image(off_t size)
{
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-sized-flash-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_boot_flash_image_contract(const void *data)
{
    g_autofree char *short_path =
        create_sized_flash_image(SGI_BEDROCK_FLASH_PART_SIZE);
    g_autofree char *long_path =
        create_sized_flash_image(SGI_BEDROCK_FLASH_SIZE + 1);
    g_autofree char *first =
        create_sized_flash_image(SGI_BEDROCK_FLASH_SIZE);
    g_autofree char *second =
        create_sized_flash_image(SGI_BEDROCK_FLASH_SIZE);
    g_autofree char *short_drive =
        g_strdup_printf("if=pflash,file=%s,format=raw", short_path);
    g_autofree char *long_drive =
        g_strdup_printf("if=pflash,file=%s,format=raw", long_path);
    g_autofree char *first_drive =
        g_strdup_printf("if=pflash,file=%s,format=raw", first);
    g_autofree char *second_drive =
        g_strdup_printf("if=pflash,file=%s,format=raw", second);
    g_autofree char *index1_drive =
        g_strdup_printf("if=pflash,index=1,file=%s,format=raw", first);
    const char *short_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", prom_path, "-display", "none", "-S",
        "-drive", short_drive, NULL,
    };
    const char *long_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", prom_path, "-display", "none", "-S",
        "-drive", long_drive, NULL,
    };
    const char *two_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", prom_path, "-display", "none", "-S",
        "-drive", first_drive, "-drive", second_drive, NULL,
    };
    const char *index1_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", prom_path, "-display", "none", "-S",
        "-drive", index1_drive, NULL,
    };

    g_test_message("rejecting a short pflash image");
    assert_qemu_rejects(short_args, "pflash image must be exactly 16 MiB");
    g_test_message("rejecting a long pflash image");
    assert_qemu_rejects(long_args, "pflash image must be exactly 16 MiB");
    g_test_message("rejecting two pflash images");
    assert_qemu_rejects(two_args, "accepts one 16 MiB pflash image");
    g_test_message("rejecting an index-one-only pflash image");
    assert_qemu_rejects(index1_args, "accepts one 16 MiB pflash image");

    g_assert_cmpint(g_unlink(short_path), ==, 0);
    g_assert_cmpint(g_unlink(long_path), ==, 0);
    g_assert_cmpint(g_unlink(first), ==, 0);
    g_assert_cmpint(g_unlink(second), ==, 0);
}

static void test_boot_flash_nonraw_image(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *path = NULL;
    g_autofree char *drive = NULL;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", prom_path, "-display", "none", "-S",
        "-drive", NULL, NULL,
    };

    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create non-raw pflash");
        return;
    }

    directory = g_dir_make_tmp("sn1-nonraw-flash-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(directory, "flash.qcow2", NULL);
    g_assert_true(mkimg(path, "qcow2", SGI_BEDROCK_FLASH_SIZE / MiB));
    drive = g_strdup_printf("if=pflash,file=%s,format=qcow2", path);
    args[13] = drive;

    assert_qemu_rejects(args, "pflash image must use raw format");

    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_prom_size_is_exact(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *empty = NULL;
    g_autofree char *short_path = NULL;
    g_autofree char *long_path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-empty-prom-XXXXXX.bin", &empty, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    fd = g_file_open_tmp("sn1-short-prom-XXXXXX.bin", &short_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, PROM_SIZE - 1), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    fd = g_file_open_tmp("sn1-long-prom-XXXXXX.bin", &long_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, PROM_SIZE + 1), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    const char *empty_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", empty, "-display", "none",
        NULL,
    };
    const char *short_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", short_path, "-display", "none",
        NULL,
    };
    const char *long_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", long_path, "-display", "none",
        NULL,
    };

    assert_qemu_rejects(empty_args, "must be exactly 1476264 bytes");
    assert_qemu_rejects(short_args, "must be exactly 1476264 bytes");
    assert_qemu_rejects(long_args, "must be exactly 1476264 bytes");

    g_assert_cmpint(g_unlink(empty), ==, 0);
    g_assert_cmpint(g_unlink(short_path), ==, 0);
    g_assert_cmpint(g_unlink(long_path), ==, 0);
}

static void test_cpu_c_enable_controls_bus_eligibility(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t progress;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_control_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 1);
    progress = run_vm_until_cpu_gpr_changes(qts, 1, "t0", 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 1);
    progress = run_vm_until_cpu_gpr_changes(qts, 1, "t0", progress);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 0);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", progress);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", 0);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_cpu_number_read_is_per_cpu(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    path = create_cpu_number_prom();
    qts = machine_start_with(path, "-accel tcg,thread=multi -no-shutdown -S");
    qtest_writeq(qts, BEDROCK_NODE0 + PI_PENDING_MODIFY, 0x107);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_PI1 + PI_PENDING_MODIFY,
                 0x108);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0), ==,
                    UINT64_C(1) << 7);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_PENDING_0), ==, UINT64_C(1) << 8);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr(qts, 0, "s5", CPU_NUMBER_READY_MARKER);
    wait_for_cpu_gpr(qts, 1, "s5", CPU_NUMBER_READY_MARKER);
    stop_vm(qts);
    g_assert_cmphex(cpu_gpr(qts, 0, "s3"), ==, 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "s3"), ==, 2);
    g_assert_cmphex(cpu_gpr(qts, 0, "s4"), ==, UINT64_C(1) << 7);
    g_assert_cmphex(cpu_gpr(qts, 1, "s4"), ==, UINT64_C(1) << 8);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_cpu_enable_read_is_per_cpu(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_number_prom();
    qts = machine_start_with(path, "-accel tcg,thread=multi -no-shutdown -S");
    /*
     * Both fitted slots come out of reset granted; clear A's so the two
     * processors' reads of "their own" register can be told apart.
     */
    qtest_writeq(qts, BEDROCK_NODE0 + PI_CPU_ENABLE_A, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_CPU_ENABLE_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_CPU_ENABLE_C), ==, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr(qts, 0, "s5", CPU_NUMBER_READY_MARKER);
    wait_for_cpu_gpr(qts, 1, "s5", CPU_NUMBER_READY_MARKER);
    stop_vm(qts);

    /* Register routing is checked here, not enforcement of bus grants. */
    g_assert_cmphex(cpu_gpr(qts, 0, "s6"), ==, 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "s6"), ==, 1);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_config0_cold_reset(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_config0_reset_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        wait_for_cpu_gpr(qts, cpu, "s2", CONFIG0_READY_MARKER);
    }
    stop_vm(qts);
    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        g_assert_cmphex(cpu_gpr(qts, cpu, "s0"), ==,
                        CONFIG0_RESET_READBACK);
        g_assert_cmphex(cpu_gpr(qts, cpu, "s1"), ==,
                        CONFIG0_MUTATED_READBACK);
    }

    qtest_system_reset(qts);
    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        g_assert_cmphex(cpu_gpr(qts, cpu, "s0"), ==, 0);
        g_assert_cmphex(cpu_gpr(qts, cpu, "s1"), ==, 0);
        g_assert_cmphex(cpu_gpr(qts, cpu, "s2"), ==, 0);
    }

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        wait_for_cpu_gpr(qts, cpu, "s2", CONFIG0_READY_MARKER);
    }
    stop_vm(qts);
    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        g_assert_cmphex(cpu_gpr(qts, cpu, "s0"), ==,
                        CONFIG0_RESET_READBACK);
        g_assert_cmphex(cpu_gpr(qts, cpu, "s1"), ==,
                        CONFIG0_MUTATED_READBACK);
    }

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_cpu_c_executes_across_enable_falling_edge(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t cpu_c_progress;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_control_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    cpu_c_progress = wait_for_cpu_gpr_change(qts, 1, "t0", 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 0);
    wait_for_cpu_gpr_change(qts, 1, "t0", cpu_c_progress);
    stop_vm(qts);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_nmi_register_write_is_inert(const void *data)
{
    uint64_t nmi_register = *(const uint64_t *)data;
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_nmi_control_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    /*
     * Both processors run from reset to the ready marker. The stock PROM
     * stores 0x100 to this register during per-node bring-up; that store must
     * not deliver an NMI. A MIPS NMI vectors through the reset vector, which
     * this control PROM records as the taken marker, so a delivered NMI would
     * move the addressed processor off its ready marker. Neither processor
     * moves: the store is inert. This is the same wiring that formerly
     * carried the NMI to the processor, so it still exercises that path, now
     * silent.
     */
    wait_for_cpu_gpr(qts, 0, "s0", NMI_READY_MARKER);
    wait_for_cpu_gpr(qts, 1, "s0", NMI_READY_MARKER);

    qtest_writeq(qts, BEDROCK_LOCAL + nmi_register, 0x100);
    for (unsigned int tries = 0; tries < NMI_SETTLE_TRIES; tries++) {
        g_assert_cmphex(cpu_gpr(qts, 0, "s0"), ==, NMI_READY_MARKER);
        g_assert_cmphex(cpu_gpr(qts, 1, "s0"), ==, NMI_READY_MARKER);
        g_usleep(NMI_SETTLE_INTERVAL_US);
    }

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_pi1_a_nmi_reaches_cpu_c(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_nmi_control_prom();
    qts = machine_start_with(path,
                             "-accel tcg,thread=multi -no-shutdown -S");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr(qts, 0, "s0", NMI_READY_MARKER);
    wait_for_cpu_gpr(qts, 1, "s0", NMI_READY_MARKER);

    qtest_set_irq_in(qts, "/machine/node0/bedrock", "pi-nmi", 2, 1);
    wait_for_cpu_gpr(qts, 1, "s0", NMI_TAKEN_MARKER);
    g_assert_cmphex(cpu_gpr(qts, 0, "s0"), ==, NMI_READY_MARKER);
    qtest_set_irq_in(qts, "/machine/node0/bedrock", "pi-nmi", 2, 0);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_prom_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-prom-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *destination_prom = create_prom(0xa1, 0xb2, 0xc3);
    QTestState *from;
    QTestState *to;
    static const uint64_t views[] = {
        PROM_RESET, HUB_LOCAL_BOOT, HUB_LOCAL_HIGH_BOOT, HUB_REMOTE_BOOT,
    };
    static const struct {
        uint64_t offset;
        uint8_t source;
        uint8_t destination;
    } probes[] = {
        { 0, 0x31, 0xa1 },
        { PROM_CONFIG_OFFSET, 0x62, 0xb2 },
        { PROM_SIZE - 1, 0x93, 0xc3 },
    };

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = machine_start();
    to = machine_start_with(destination_prom, "-incoming defer");

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            g_assert_cmphex(qtest_readb(to, views[i] + probes[j].offset), ==,
                            probes[j].destination);
        }
    }
    assert_erased_second_position(to);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            g_assert_cmphex(qtest_readb(to, views[i] + probes[j].offset), ==,
                            probes[j].source);
        }
    }

    assert_erased_second_position(to);

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_unlink(destination_prom), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_cpu_c_enable_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    QTestState *from;
    QTestState *to;
    uint64_t stopped;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    directory = g_dir_make_tmp("sn1-cpu-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    path = create_cpu_control_prom();
    from = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");
    to = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S -incoming defer");

    qtest_writeq(from, BEDROCK_LOCAL + PI_CPU_ENABLE_C, 0);
    run_vm_until_cpu_gpr_changes(from, 1, "t0", 0);
    g_assert_cmphex(cpu_gpr(from, 1, "t0"), !=, 0);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    stop_vm_if_running(to);

    stopped = cpu_gpr(to, 1, "t0");
    g_assert_cmphex(stopped, !=, 0);
    /* Incoming register state overrides the destination's initial grant. */
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 0);
    stopped = run_vm_until_cpu_gpr_changes(to, 1, "t0", stopped);

    qtest_writeq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_C, 1);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_C), ==, 1);
    run_vm_until_cpu_gpr_changes(to, 1, "t0", stopped);

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/*
 * The per-CPU cache-as-RAM private window (sn-promdata.c) is new for the
 * Origin 300 node as of the node-types step: before it, both Bedrock
 * processors shared one system container with no window at all. It forwards
 * instead of serving from its own buffer whenever no processor is in the
 * PROM's cache-only phase. This holds throughout a qtest run: no guest
 * code executes here, so the phase is never entered. A forwarded access
 * still traces, and that trace is what tells a broken post-load apart from a
 * working one for each processor's own window: if migration left either
 * filter disabled or removed, that processor's access would fall straight
 * through to the flash it overlays instead of tracing a forward, silently.
 */
#define PROMDATA_WINDOW_PHYS UINT64_C(0x100000)
#define PROMDATA_RAM_HOME UINT64_C(0x01a00000)
#define PROMDATA_RAM_HOME_READY UINT64_C(0xcafe)

static char *create_promdata_ram_home_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    stl_be_p(image + 0x400, 0x40086000); /* mfc0 t0, Status */
    stl_be_p(image + 0x404, 0x3c092000); /* lui t1, Status.CU1 >> 16 */
    stl_be_p(image + 0x408, 0x01094025); /* or t0, t0, t1 */
    stl_be_p(image + 0x40c, 0x40886000); /* mtc0 t0, Status */
    stl_be_p(image + 0x410, 0x000000c0); /* ehb */
    stl_be_p(image + 0x414, 0x34080002); /* ori t0, zero, private phase */
    stl_be_p(image + 0x418, 0x44a80000); /* dmtc1 t0, f0 */
    stl_be_p(image + 0x41c, 0x3c098010); /* lui t1, cached window */
    stl_be_p(image + 0x420, 0x340a1111); /* ori t2, zero, first value */
    stl_be_p(image + 0x424, 0xad2a0000); /* sw t2, 0(t1) */
    stl_be_p(image + 0x428, 0x3c0881a0); /* lui t0, cached RAM home */
    stl_be_p(image + 0x42c, 0x01000008); /* jr t0 */

    fd = g_file_open_tmp("sn1-promdata-ram-home-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_promdata_ram_copy_is_execution_home(const void *data)
{
    g_autofree char *path = NULL;
    uint8_t copy[0x20] = { 0 };
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    stl_be_p(copy + 0x00, 0x44a00000); /* dmtc1 zero, f0 */
    stl_be_p(copy + 0x04, 0x3c098010); /* lui t1, cached window */
    stl_be_p(copy + 0x08, 0x340a2222); /* ori t2, zero, second value */
    stl_be_p(copy + 0x0c, 0xad2a0000); /* sw t2, 0(t1) */
    stl_be_p(copy + 0x10, 0x3410cafe); /* ori s0, zero, ready */
    stl_be_p(copy + 0x14, 0x1000ffff); /* beq zero, zero, self */

    path = create_promdata_ram_home_prom();
    qts = machine_start_with(path, "-accel tcg,thread=multi -S");
    machine_configure_memory(qts);
    qtest_memwrite(qts, PROMDATA_RAM_HOME, copy, sizeof(copy));
    g_assert_cmphex(qtest_readl(qts, PROMDATA_WINDOW_PHYS), ==, 0);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr(qts, 0, "s0", PROMDATA_RAM_HOME_READY);
    stop_vm(qts);
    /* The cached phase must not spill either store into shared DRAM. */
    g_assert_cmphex(qtest_readl(qts, PROMDATA_WINDOW_PHYS), ==, 0);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void promdata_cpu_probe(QTestState *qts, unsigned int cpu)
{
    g_autofree char *command =
        g_strdup_printf("xp/1wx 0x%" PRIx64, PROMDATA_WINDOW_PHYS);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command',"
        "  'arguments': { 'command-line': %s, 'cpu-index': %u } }",
        command, cpu);

    g_assert(qdict_haskey(response, "return"));
}

static bool promdata_forward_traced(const char *path)
{
    g_autofree char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    return strstr(contents, "sn_promdata_forward") != NULL;
}

static void test_promdata_filter_survives_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *from_trace = NULL;
    g_autofree char *to_trace = NULL;
    g_autofree char *from_extra = NULL;
    g_autofree char *to_extra = NULL;
    QTestState *from, *to;

    directory = g_dir_make_tmp("sn1-promdata-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from_trace = g_build_filename(directory, "from.trace", NULL);
    to_trace = g_build_filename(directory, "to.trace", NULL);
    from_extra = g_strdup_printf("-trace enable=sn_promdata_forward,file=%s",
                                 from_trace);
    to_extra = g_strdup_printf(
        "-trace enable=sn_promdata_forward,file=%s -incoming defer",
        to_trace);

    from = machine_start_with(prom_path, from_extra);
    to = machine_start_with(prom_path, to_extra);

    promdata_cpu_probe(from, 0);
    promdata_cpu_probe(from, 1);
    g_assert_true(promdata_forward_traced(from_trace));

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);

    promdata_cpu_probe(to, 0);
    promdata_cpu_probe(to, 1);
    g_assert_true(promdata_forward_traced(to_trace));

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_unlink(from_trace);
    g_unlink(to_trace);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/*
 * The Bedrock node's object graph hangs off sgi-sn1-node (sn1-node.c) rather
 * than a standalone machine state, so this proves migration still completes
 * and the moved devices still answer afterward, the same concern
 * sn0-migration-test.c's own docstring names, applied here to the node-types
 * and Origin 300 steps' re-parenting instead of a derived register value.
 */
static char *write_prom_image(const char *directory)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *prom = g_malloc0(PROM_SIZE);
    char *path = g_build_filename(directory, "prom.bin", NULL);

    g_assert_true(g_file_set_contents(path, (const char *)prom, PROM_SIZE,
                                      &error));
    g_assert_no_error(error);
    return path;
}

static void test_board_id_and_l1_uart_survive_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-migration-XXXXXX",
                                                &error);
    g_autofree char *prom_path = write_prom_image(directory);
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *cmdline = NULL;
    g_autofree char *to_cmdline = NULL;
    QTestState *from, *to;
    uint64_t l1_status_before;

    g_assert_no_error(error);
    tmpfs = g_dir_make_tmp("sn1-migration-sock-XXXXXX", &error);
    g_assert_no_error(error);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    cmdline = g_strdup_printf(
        "-M origin300 -cpu R14000 -smp 2 -bios %s -display none -S",
        prom_path);
    from = qtest_init(cmdline);
    g_assert_cmphex(qtest_readq(from, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    l1_status_before = qtest_readq(from, L1_UART_BASE + L1_UART_STATUS);
    /* Slot C's processor LED is node state and travels too. */
    qtest_writeq(from, HUB_LOCAL_REGISTERS + HUB_LED(2), 0x5d);

    to_cmdline = g_strdup_printf("%s -incoming %s", cmdline, uri);
    to = qtest_init(to_cmdline);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readq(to, BOARD_ID_LOCAL), ==, BOARD_ID_VALUE);
    g_assert_cmphex(qtest_readq(to, L1_UART_BASE + L1_UART_STATUS), ==,
                    l1_status_before);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL_REGISTERS + HUB_LED(2)), ==,
                    0x5d);

    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_system_reset_clears_bedrock_state(const void *data)
{
    QTestState *qts = machine_start();
    uint64_t address = BEDROCK_LOCAL + BEDROCK_IO + IO_SCRATCH0;

    qtest_writeq(qts, address, UINT64_C(0x0ead0040));
    g_assert_cmphex(qtest_readq(qts, address), ==, UINT64_C(0x0ead0040));
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, address), ==, 0);

    qtest_quit(qts);
}

/*
 * IO-8's IOC3 slot returns its PCI identity through a doubleword read and
 * accepts parity-error response and latency-timer writes through the
 * configuration window.
 */
static void test_io8bt_configuration_reads_and_writes(const void *data)
{
    QTestState *qts = machine_start();
    uint64_t header;

    header = qtest_readq(qts, IO8_IOC3_CFG);
    g_assert_cmphex(header >> 32, ==, qtest_readl(qts, IO8_IOC3_CFG));
    g_assert_cmphex(header >> 32, ==, 0x000310a9);
    g_assert_cmphex((uint32_t)header, ==,
                    qtest_readl(qts, IO8_IOC3_CFG + PCI_COMMAND));
    /* SERR enable, parity-error response, bus master, memory space. */
    qtest_writel(qts, IO8_IOC3_CFG + PCI_COMMAND, 0x146);
    g_assert_cmphex(qtest_readl(qts, IO8_IOC3_CFG + PCI_COMMAND) & 0xffff,
                    ==, 0x146);
    /*
     * The latency timer, the high byte of the word at 0x0c. Cache line size
     * below it takes the zero written with it, and header type and built-in
     * self test above it stay zero.
     */
    qtest_writel(qts, IO8_IOC3_CFG + 0x0c, 0xff00);
    g_assert_cmphex(qtest_readl(qts, IO8_IOC3_CFG + 0x0c), ==, 0xff00);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const uint64_t nmi_a = PI_NMI_A;
    static const uint64_t nmi_b = PI_NMI_B;
    int result;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom(0x31, 0x62, 0x93);

    qtest_add_data_func("sn1/address-map/ram-and-hub-alias", NULL,
                        test_ram_and_hub_alias);
    qtest_add_data_func("sn1/address-map/bedrock-register-aliases", NULL,
                        test_bedrock_register_aliases);
    qtest_add_data_func("sn1/address-map/origin300-cpu-c-initial-enable", NULL,
                        test_origin300_cpu_c_initial_enable);
    qtest_add_data_func("sn1/address-map/origin300-four-cpu-initial-enable",
                        NULL, test_origin300_four_cpu_initial_enable);
    qtest_add_data_func("sn1/address-map/hub-windows-omit-bedrock-registers",
                        NULL, test_hub_windows_omit_bedrock_registers);
    qtest_add_data_func("sn1/address-map/hub-unidentified-block-keeps-nothing",
                        NULL, test_hub_unidentified_block_keeps_nothing);
    qtest_add_data_func("sn1/address-map/hub-special-leds", NULL,
                        test_hub_special_leds);
    qtest_add_data_func("sn1/address-map/hub-window-refusals", NULL,
                        test_hub_window_refusals);
    qtest_add_data_func("sn1/address-map/hub-registers-refuse-denied-region",
                        NULL, test_hub_registers_refuse_denied_region);
    qtest_add_data_func("sn1/address-map/crosstalk-big-window", NULL,
                        test_crosstalk_big_window_routes);
    qtest_add_data_func("sn1/address-map/pi1-a-irq-reaches-cpu1", NULL,
                        test_pi1_a_irq_reaches_cpu1);
    qtest_add_data_func("sn1/address-map/xbridge-topology", NULL,
                        test_xbridge_topology);
    qtest_add_data_func("sn1/address-map/io8-usb-attachment", NULL,
                        test_io8_usb_attachment);
    qtest_add_data_func("sn1/address-map/io8-usb-invalid-ate", NULL,
                        test_io8_usb_rejects_invalid_ate_dma);
    qtest_add_data_func("origin300/ioc3/attachment", NULL,
                        test_io8_ioc3_attachment);
    qtest_add_data_func("origin300/ioc3/timekeeper-compatibility", NULL,
                        test_io8_ioc3_timekeeper);
    qtest_add_data_func("origin300/ioc3/nvram-drive-persistence/named",
                        "if=none,id=sgi-nvram0", test_io8_ioc3_nvram_drive);
    qtest_add_data_func("origin300/ioc3/nvram-drive-persistence/legacy-mtd",
                        "if=mtd", test_io8_ioc3_nvram_drive);
    qtest_add_data_func("origin300/ioc3/serial-chardev-indices", NULL,
                        test_io8_ioc3_serial_chardevs);
    qtest_add_data_func("origin300/ioc3/superio-interrupt", NULL,
                        test_io8_ioc3_superio_interrupt);
    qtest_add_data_func("origin300/ioc3/ethernet-dma-interrupt", NULL,
                        test_io8_ioc3_ethernet_dma_interrupt);
    qtest_add_data_func("origin300/ioc3/identical-build-migration", NULL,
                        test_io8_ioc3_migration);
    qtest_add_data_func("origin300/io8-ioc3/transceiver-identity", NULL,
                        test_transceiver_identity);
    qtest_add_data_func("origin300/io8-ioc3/origin200-address-is-silent",
                        NULL, test_origin200_address_is_silent);
    qtest_add_data_func(
        "origin300/io8-ioc3/transceiver-answers-at-one-address", NULL,
        test_transceiver_answers_at_one_address);
    qtest_add_data_func("origin300/io8-ioc3/register-0x17-reads-back", NULL,
                        test_register_0x17_reads_back_a_written_value);
    qtest_add_data_func("origin300/io8-ioc3/io8bt-configuration", NULL,
                        test_io8bt_configuration_reads_and_writes);
    qtest_add_data_func("origin300/io8/mac/record-matches-configured-mac",
                        NULL, test_configured_mac_in_board_record);
    qtest_add_data_func("sn1/address-map/hub-special-register-windows", NULL,
                        test_hub_special_register_windows);
    qtest_add_data_func(
        "sn1/address-map/hub-local-register-window-omits-pi-registers",
        NULL, test_hub_local_register_window_omits_pi_registers);
    qtest_add_data_func("sn1/address-map/hub-local-reaches-l1-uart", NULL,
                        test_hub_local_reaches_l1_uart);
    qtest_add_data_func("sn1/address-map/hub-local-l1-uart-reaches-cpu-c",
                        NULL, test_hub_local_l1_uart_reaches_cpu_c);
    qtest_add_data_func("sn1/address-map/l1-operator-console", NULL,
                        test_l1_operator_console_uses_serial0);
    qtest_add_data_func("sn1/address-map/l1-uart-irq-reaches-cpu", NULL,
                        test_l1_uart_irq_reaches_cpu);
    qtest_add_data_func("sn1/address-map/prom-cpu-module-config", NULL,
                        test_prom_cpu_module_config);
    qtest_add_data_func("sn1/address-map/l1-identity-records", NULL,
                        test_l1_identity_records_come_from_the_command_line);
    qtest_add_data_func("sn1/address-map/l1-identity-records-absent", NULL,
                        test_l1_has_no_identity_records_by_default);
    qtest_add_data_func("sn1/address-map/board-identification", NULL,
                        test_board_identification);
    qtest_add_data_func("sn1/address-map/ecc-backdoor-window", NULL,
                        test_ecc_backdoor_window);
    qtest_add_data_func("sn1/address-map/directory-backdoor-window", NULL,
                        test_directory_backdoor_window);
    qtest_add_data_func("sn1/address-map/directory-protection", NULL,
                        test_directory_protection);
    qtest_add_data_func("sn1/address-map/boot-flash-backs-prom-views", NULL,
                        test_boot_flash_backs_every_prom_view);
    qtest_add_data_func("sn1/address-map/execution-views-refuse-commands",
                        NULL, test_execution_views_refuse_commands);
    qtest_add_data_func("sn1/address-map/boot-flash-migration", NULL,
                        test_boot_flash_migration);
    qtest_add_data_func("sn1/address-map/prom-size-is-exact", NULL,
                        test_prom_size_is_exact);
    qtest_add_data_func("sn1/address-map/boot-flash-image-contract", NULL,
                        test_boot_flash_image_contract);
    qtest_add_data_func("sn1/address-map/boot-flash-nonraw-image", NULL,
                        test_boot_flash_nonraw_image);
    qtest_add_data_func("sn1/address-map/cpu-number-read-is-per-cpu", NULL,
                        test_cpu_number_read_is_per_cpu);
    qtest_add_data_func("sn1/address-map/cpu-enable-read-is-per-cpu", NULL,
                        test_cpu_enable_read_is_per_cpu);
    qtest_add_data_func("sn1/address-map/cpu-c-enable-controls-bus", NULL,
                        test_cpu_c_enable_controls_bus_eligibility);
    qtest_add_data_func("sn1/address-map/config0-cold-reset", NULL,
                        test_config0_cold_reset);
    qtest_add_data_func("sn1/address-map/cpu-c-executes-across-enable-fall",
                        NULL, test_cpu_c_executes_across_enable_falling_edge);
    qtest_add_data_func("sn1/address-map/nmi-write-a-is-inert", &nmi_a,
                        test_nmi_register_write_is_inert);
    qtest_add_data_func("sn1/address-map/nmi-write-b-is-inert", &nmi_b,
                        test_nmi_register_write_is_inert);
    qtest_add_data_func("sn1/address-map/pi1-a-nmi-reaches-cpu-c", NULL,
                        test_pi1_a_nmi_reaches_cpu_c);
    qtest_add_data_func("sn1/address-map/prom-migration", NULL,
                        test_prom_migration);
    qtest_add_data_func("sn1/address-map/cpu-c-enable-migration", NULL,
                        test_cpu_c_enable_migration);
    qtest_add_data_func("sn1/address-map/promdata-ram-copy-home", NULL,
                        test_promdata_ram_copy_is_execution_home);
    qtest_add_data_func("sn1/address-map/promdata-filter-survives-migration",
                        NULL, test_promdata_filter_survives_migration);
    qtest_add_data_func("sn1/address-map/system-reset", NULL,
                        test_system_reset_clears_bedrock_state);
    qtest_add_data_func("sn1/migration/board-id-and-l1-uart-survive", NULL,
                        test_board_id_and_l1_uart_survive_migration);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
