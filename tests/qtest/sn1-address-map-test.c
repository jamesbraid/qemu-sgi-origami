/*
 * SGI Origin 300 (IP45) address-space routing.
 *
 * These are machine-bus addresses after the R10000-family uncached
 * attribute has been carried above the 40-bit physical address.  The
 * values are repeated here rather than shared with the machine so a bad
 * decoder constant cannot make both sides agree.
 *
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

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define SPACE_HUB UINT64_C(0x10000000000)
#define SPACE_XTALK UINT64_C(0x20000000000)
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
#define BEDROCK_MD_BANK0_512M UINT64_C(0x41)

#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_CPU_ENABLE_B UINT64_C(0x58)
#define PI_NMI_A UINT64_C(0x70)
#define PI_NMI_B UINT64_C(0x78)
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
 * One ECC byte per eight bytes of node memory, so the machine's 512 MiB gives
 * a 64 MiB extent inside the gigabyte the back door reserves for it.
 */
#define ECC_EXTENT UINT64_C(0x4000000)
#define ECC_APERTURE UINT64_C(0x40000000)
#define HUB_DIRECTORY (SPACE_HUB + UINT64_C(0x1c0000000))
#define BOARD_ID_LOCAL (HUB_LOCAL_REGISTERS + UINT64_C(0x4000000))
#define BOARD_ID_REMOTE (HUB_REMOTE_REGISTERS + UINT64_C(0x4000000))
#define BOARD_ID_UNUSED (HUB_LOCAL_REGISTERS + UINT64_C(0x5000000))
#define BOARD_ID_VALUE UINT64_C(0x8000)
#define HUB_LOCAL_HIGH_BOOT (SPACE_HUB + UINT64_C(0x1fc00000))
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define HUB_REMOTE_BOOT (SPACE_HUB + UINT64_C(0x38000000))
#define HUB_REMOTE_FLASH (SPACE_HUB + UINT64_C(0x3a000000))
#define L1_UART UINT64_C(0x80)
#define L1_SCRATCH UINT64_C(0x38)
#define L1_UART_BASE (HUB_REMOTE_REGISTERS + L1_UART)
#define L1_UART_STATUS UINT64_C(0x28)
#define L1_STATUS_READY UINT64_C(0x61)
#define L1_EEPROM_CHASSIS 1
#define L1_EEPROM_BOARD 2
#define L1_EEPROM_SPD 4
#define L1_REPLY_ABSENT 0x98

#define PROM_RESET UINT64_C(0x1fc00000)
#define PROM_SIZE UINT64_C(0x1686a8)
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
 * The directory words for memory line 0x1a00000 and for the last line of
 * 512 MiB, and the aperture the node ends with.
 */
#define DIRECTORY_LINE_1A00000 UINT64_C(0x340000)
#define DIRECTORY_LAST_FITTED UINT64_C(0x3fffff0)
#define DIRECTORY_FIRST_ABSENT UINT64_C(0x4000000)
#define DIRECTORY_PROTECTION_BANK1_HIGH UINT64_C(0xf800408)
#define DIRECTORY_APERTURE UINT64_C(0x40000000)

#define EXEC_STATE_PHYS UINT64_C(0x100000)
#define EXEC_STATE_KSEG1_HI 0xa010
#define EXEC_RESET_OFFSET 0x00
#define EXEC_XKPHYS_STATE_OFFSET 0x08
#define EXEC_FAULT_OFFSET 0x10
#define EXEC_RESET_MARKER UINT64_C(0x1155)
#define EXEC_XKPHYS_MARKER UINT64_C(0x2277)
#define EXEC_FAULT_MARKER UINT64_C(0xdead)
#define EXEC_XKPHYS_MEMORY_HI 0xbe00
#define EXEC_CODE_OFFSET 0x400
#define EXEC_XKPHYS_OFFSET 0x500

#define EXEC_TLB_SETUP_OFFSET 0x18
#define EXEC_TLB_STATE_OFFSET 0x20
#define EXEC_TLB_EXCEPTION_OFFSET 0x28
#define EXEC_TLB_SETUP_MARKER UINT64_C(0x3366)
#define EXEC_TLB_MARKER UINT64_C(0x4499)
#define EXEC_TLB_REFILL_MARKER UINT64_C(0xe201)
#define EXEC_XTLB_REFILL_MARKER UINT64_C(0xe281)
#define EXEC_CACHE_ERROR_MARKER UINT64_C(0xe301)
#define EXEC_GENERAL_EXCEPTION_MARKER UINT64_C(0xe381)
#define EXEC_TLB_VIRTUAL_HI 0xc000
#define EXEC_TLB_VIRTUAL_OFFSET 0x1fc021a0
#define EXEC_TLB_ENTRYLO_MEMORY_HI 0xc000
/* UC=3, PFN=0x1fc01, C=2, D=1, V=1. */
#define EXEC_TLB_4K_ENTRYHI_LOW 0x1fc02000
#define EXEC_TLB_4K_ENTRYLO0_LOW 0x7f0056
#define EXEC_TLB_4K_PHYSICAL_OFFSET 0x11a0
/* ASID=1, UC=3, 1 MiB PFNs 0x1fc00/0x1fd00, C=2, V=1. */
#define EXEC_TLB_1M_ENTRYHI_LOW 0x1fc00001
#define EXEC_TLB_1M_PAGEMASK 0x1fe000
#define EXEC_TLB_1M_ENTRYLO0_LOW 0x7f0012
#define EXEC_TLB_1M_ENTRYLO1_LOW 0x7f4012
#define EXEC_TLB_1M_PHYSICAL_OFFSET 0x21a0

#define NMI_READY_MARKER UINT64_C(0x1400)
#define NMI_TAKEN_MARKER UINT64_C(0x4e4d)
#define CONFIG0_READY_MARKER UINT64_C(0xc0f0)
#define CONFIG0_RESET_READBACK UINT64_C(0x6c9aa985)
#define CONFIG0_MUTATED_READBACK UINT64_C(0x6c9aa982)
#define POLL_TRIES 10000
#define POLL_INTERVAL_US 1000
/* Guest run-time to allow a mistaken NMI to vector before asserting none did. */
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

#define XBRIDGE_ATE0 0x10000
#define XBRIDGE_DEVICE(n) (0x200 + (n) * 8)
#define XBRIDGE_DEVICE_MEMORY (1U << 12)
/*
 * docs/datasheets/xbridge.md, "device 0 to 7": bit 13 is PIO swap, reset
 * clear. Clear places a narrow access by offset exclusive-or with the
 * value preserved (Tables 109 to 111); set makes it a plain byte stream,
 * mirrored. An ordinary register (IOC3, the OHCI's own BAR-mapped
 * registers) wants the former, so these devices are left at the reset
 * default rather than setting the bit -- the flag this file used to set
 * here for that purpose was named backwards from the datasheet's own bit
 * and set the mirror instead.
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

typedef struct TlbFetchCase {
    uint32_t entryhi_low;
    uint32_t pagemask;
    uint32_t entrylo0_low;
    uint32_t entrylo1_low;
    unsigned int physical_offset;
} TlbFetchCase;

static const TlbFetchCase tlb_fetch_4k = {
    .entryhi_low = EXEC_TLB_4K_ENTRYHI_LOW,
    .entrylo0_low = EXEC_TLB_4K_ENTRYLO0_LOW,
    .physical_offset = EXEC_TLB_4K_PHYSICAL_OFFSET,
};

static const TlbFetchCase tlb_fetch_1m = {
    .entryhi_low = EXEC_TLB_1M_ENTRYHI_LOW,
    .pagemask = EXEC_TLB_1M_PAGEMASK,
    .entrylo0_low = EXEC_TLB_1M_ENTRYLO0_LOW,
    .entrylo1_low = EXEC_TLB_1M_ENTRYLO1_LOW,
    .physical_offset = EXEC_TLB_1M_PHYSICAL_OFFSET,
};

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

static void write_execution_report(uint8_t *image, unsigned int offset,
                                   uint16_t marker, uint16_t state_offset)
{
    stl_be_p(image + offset + 0x00,
             0x3c080000 | EXEC_STATE_KSEG1_HI); /* lui t0, 0xa010 */
    stl_be_p(image + offset + 0x04,
             0x34090000 | marker);               /* ori t1, zero, marker */
    stl_be_p(image + offset + 0x08,
             0xfd090000 | state_offset);         /* sd t1, offset(t0) */
    stl_be_p(image + offset + 0x0c, 0x0000000f); /* sync */
    stl_be_p(image + offset + 0x10, 0x1000ffff); /* beq zero, zero, self */
}

static char *create_xkphys_fetch_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */

    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        write_execution_report(image, offset, EXEC_FAULT_MARKER,
                               EXEC_FAULT_OFFSET);
    }

    /* Report the reset fetch before entering XKPHYS. */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x00,
             0x3c080000 | EXEC_STATE_KSEG1_HI);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x04,
             0x34090000 | EXEC_RESET_MARKER);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x08,
             0xfd090000 | EXEC_RESET_OFFSET);

    /*
     * Enable 64-bit kernel addresses, then use uncached attribute 3 to
     * reach physical 0x1fc00500 in the memory window.
     */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x0c, 0x400a6000); /* mfc0 t2, Status */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x10, 0x354a0080); /* ori t2, t2, KX */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x14, 0x408a6000); /* mtc0 t2, Status */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x18, 0x000000c0); /* ehb */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x1c,
             0x3c0a0000 | EXEC_XKPHYS_MEMORY_HI);         /* lui t2, 0xbe00 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x20,
             0x000a503c);                                /* dsll32 t2, t2, 0 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x24, 0x3c0b1fc0); /* lui t3, 0x1fc0 */
    /* ori t3, t3, 0x500 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x28,
             0x356b0000 | EXEC_XKPHYS_OFFSET);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x2c, 0x014b5025); /* or t2, t2, t3 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x30, 0x01400008); /* jr t2 */

    write_execution_report(image, EXEC_XKPHYS_OFFSET,
                           EXEC_XKPHYS_MARKER,
                           EXEC_XKPHYS_STATE_OFFSET);

    fd = g_file_open_tmp("sn1-xkphys-fetch-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *create_tlb_fetch_prom(uint32_t entryhi_low, uint32_t pagemask,
                                   uint32_t entrylo0_low,
                                   uint32_t entrylo1_low,
                                   unsigned int physical_offset)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */

    write_execution_report(image, 0x200, EXEC_TLB_REFILL_MARKER,
                           EXEC_TLB_EXCEPTION_OFFSET);
    write_execution_report(image, 0x280, EXEC_XTLB_REFILL_MARKER,
                           EXEC_TLB_EXCEPTION_OFFSET);
    write_execution_report(image, 0x300, EXEC_CACHE_ERROR_MARKER,
                           EXEC_TLB_EXCEPTION_OFFSET);
    write_execution_report(image, 0x380, EXEC_GENERAL_EXCEPTION_MARKER,
                           EXEC_TLB_EXCEPTION_OFFSET);

    /* Report setup before entering the mapped 64-bit segment. */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x00,
             0x3c080000 | EXEC_STATE_KSEG1_HI);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x04,
             0x34090000 | EXEC_TLB_SETUP_MARKER);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x08,
             0xfd090000 | EXEC_TLB_SETUP_OFFSET);

    /* Enable 64-bit kernel addresses. */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x0c, 0x400a6000); /* mfc0 t2, Status */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x10, 0x354a0080); /* ori t2, t2, KX */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x14, 0x408a6000); /* mtc0 t2, Status */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x18, 0x000000c0); /* ehb */

    /* Install the requested uncached mapping into the PROM backing. */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x1c,
             0x3c0a0000 | EXEC_TLB_VIRTUAL_HI);           /* lui t2, 0xc000 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x20,
             0x000a503c);                                /* dsll32 t2, t2, 0 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x24,
             0x3c0b0000 | (entryhi_low >> 16));           /* lui t3 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x28,
             0x356b0000 | (entryhi_low & 0xffff));
    stl_be_p(image + EXEC_CODE_OFFSET + 0x2c, 0x014b5025); /* or t2, t2, t3 */
    /* dmtc0 t2, EntryHi */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x30, 0x40aa5000);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x34,
             0x3c0c0000 | (pagemask >> 16));              /* lui t4 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x38,
             0x358c0000 | (pagemask & 0xffff));           /* ori t4 */
    /* mtc0 t4, PageMask */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x3c, 0x408c2800);
    /* Uncached attribute 3 selects the memory window. */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x40,
             0x3c0a0000 | EXEC_TLB_ENTRYLO_MEMORY_HI);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x44,
             0x000a503c);                                /* dsll32 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x48,
             0x3c0b0000 | (entrylo0_low >> 16));          /* lui t3 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x4c,
             0x356b0000 | (entrylo0_low & 0xffff));
    stl_be_p(image + EXEC_CODE_OFFSET + 0x50, 0x014b5025); /* or t2, t2, t3 */
    /* dmtc0 t2, EntryLo0 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x54, 0x40aa1000);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x58,
             0x3c0a0000 | EXEC_TLB_ENTRYLO_MEMORY_HI);    /* lui t2 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x5c,
             0x000a503c);                                /* dsll32 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x60,
             0x3c0b0000 | (entrylo1_low >> 16));          /* lui t3 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x64,
             0x356b0000 | (entrylo1_low & 0xffff));
    stl_be_p(image + EXEC_CODE_OFFSET + 0x68, 0x014b5025); /* or t2, t2, t3 */
    /* dmtc0 t2, EntryLo1 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x6c, 0x40aa1800);
    /* mtc0 zero, Index */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x70, 0x40800000);
    stl_be_p(image + EXEC_CODE_OFFSET + 0x74, 0x42000002); /* tlbwi */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x78, 0x000000c0); /* ehb */

    /* Jump through the mapping to 0xc00000001fc021a0. */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x7c,
             0x3c0a0000 | EXEC_TLB_VIRTUAL_HI);           /* lui t2, 0xc000 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x80,
             0x000a503c);                                /* dsll32 t2, t2, 0 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x84, 0x3c0b1fc0); /* lui t3, 0x1fc0 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x88,
             0x356b0000 | (EXEC_TLB_VIRTUAL_OFFSET & 0xffff));
    stl_be_p(image + EXEC_CODE_OFFSET + 0x8c, 0x014b5025); /* or t2, t2, t3 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x90, 0x01400008); /* jr t2 */
    stl_be_p(image + EXEC_CODE_OFFSET + 0x94, 0x00000000); /* nop */

    write_execution_report(image, physical_offset,
                           EXEC_TLB_MARKER, EXEC_TLB_STATE_OFFSET);

    fd = g_file_open_tmp("sn1-tlb-fetch-prom-XXXXXX.bin", &path, &error);
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

static void wait_for_migration(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                             "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
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
 * The region-type token ("ram", "rom", "romd", "i/o", ...) that "info
 * mtree -f"'s flat view of the CPU's address space reports for the entry
 * covering address, or NULL if the flat view has nothing mapped there.
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
 * Every window the processor fetches firmware from must resolve to ram,
 * rom or romd in the flat view. An i/o entry there caches no TCG
 * translation: each instruction recompiles as its own one-instruction
 * block, and the guest runs 100-300x slower while giving no outward sign
 * anything is wrong.
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

static void test_ram_and_hub_alias(const void *data)
{
    QTestState *qts = machine_start();

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
    /* Node memory ends at the selected 512 MiB population. */
    assert_flat_view_unmapped(qts, 512 * MiB);
    /* Memory-special has no mapping in this skeleton. */
    assert_flat_view_unmapped(qts, SPACE_MEMORY_SPECIAL + 0x1000);

    qtest_quit(qts);
}

static void test_bedrock_register_aliases(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x8badf00d12345678);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_NUMBER), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_NUMBER), ==, 2);

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

static void test_hub_local_reaches_bedrock(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x1234abcd5678ef90);

    qtest_writeq(qts, HUB_LOCAL_REGISTERS + BEDROCK_LB + LB_SCRATCH2,
                 scratch);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);

    qtest_quit(qts);
}

static void test_hub_remote_reaches_bedrock_tail(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x90ef7856cdab3412);

    qtest_writeq(qts, HUB_REMOTE_REGISTERS + BEDROCK_LB + LB_SCRATCH2,
                 scratch);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);

    qtest_quit(qts);
}

static void test_hub_remote_reaches_processor_interface(const void *data)
{
    QTestState *qts = machine_start();

    qtest_writeq(qts, HUB_REMOTE_REGISTERS + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 1);

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

static void test_xbridge_topology(const void *data)
{
    QTestState *qts = machine_start();
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
    wait_for_migration(from);
    wait_for_migration(to);

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

static void test_hub_special_register_windows(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x5a);

    qtest_writeq(qts, HUB_REMOTE_REGISTERS + L1_UART + L1_SCRATCH, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);

    /* Public consumers establish the L1 decoder only in the remote form. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_SCRATCH), !=, scratch);
    check_access_has_no_storage(qts, HUB_REMOTE_REGISTERS + 0x1000);

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

static void test_prom_four_read_only_views(const void *data)
{
    QTestState *qts = machine_start();
    static const uint64_t views[] = {
        PROM_RESET, HUB_LOCAL_BOOT, HUB_LOCAL_HIGH_BOOT, HUB_REMOTE_BOOT,
    };
    static const struct {
        uint64_t offset;
        uint8_t value;
    } probes[] = {
        { 0, 0x31 },
        { PROM_CONFIG_OFFSET, 0x62 },
        { PROM_SIZE - 1, 0x93 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(views); j++) {
            g_assert_cmphex(qtest_readb(qts, views[j] + probes[i].offset), ==,
                            probes[i].value);
        }

        for (size_t write_view = 0;
             write_view < ARRAY_SIZE(views); write_view++) {
            qtest_writeb(qts, views[write_view] + probes[i].offset,
                         probes[i].value ^ 0xff);
            for (size_t read_view = 0;
                 read_view < ARRAY_SIZE(views); read_view++) {
                g_assert_cmphex(qtest_readb(qts,
                                            views[read_view] +
                                            probes[i].offset), ==,
                                probes[i].value);
            }
        }
    }

    /* All three boot aliases stop at the exact supplied length. */
    assert_flat_view_unmapped(qts, HUB_LOCAL_BOOT + PROM_SIZE);
    assert_flat_view_unmapped(qts, HUB_LOCAL_HIGH_BOOT - 1);
    assert_flat_view_unmapped(qts, HUB_LOCAL_HIGH_BOOT + PROM_SIZE);
    assert_flat_view_unmapped(qts, HUB_REMOTE_BOOT + PROM_SIZE);

    /* The reset view ends there too and reveals the RAM underneath. */
    qtest_writeq(qts, PROM_RESET + PROM_SIZE,
                 UINT64_C(0x7766554433221100));
    g_assert_cmphex(qtest_readq(qts, PROM_RESET + PROM_SIZE), ==,
                    UINT64_C(0x7766554433221100));

    qtest_quit(qts);
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

static void assert_flat_view_extent(QTestState *qts, uint64_t start,
                                    uint64_t size)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *range = g_strdup_printf("%016" PRIx64 "-%016" PRIx64,
                                             start, start + size - 1);
    const char *hit = strstr(mtree, range);
    const char *line_end;
    g_autofree char *line = NULL;

    g_assert_nonnull(hit);
    line_end = strchr(hit, '\n');
    line = g_strndup(hit, line_end ? (size_t)(line_end - hit) : strlen(hit));
    g_assert_nonnull(strstr(line, "sn1.prom"));
}

static void test_prom_flat_view_extents(const void *data)
{
    QTestState *qts = machine_start();

    assert_flat_view_extent(qts, PROM_RESET, PROM_SIZE);
    assert_flat_view_extent(qts, HUB_LOCAL_BOOT, PROM_SIZE);
    assert_flat_view_extent(qts, HUB_LOCAL_HIGH_BOOT, PROM_SIZE);
    assert_flat_view_extent(qts, HUB_REMOTE_BOOT, PROM_SIZE);
    assert_flat_view_extent(qts, HUB_LOCAL_FLASH,
                            SGI_BEDROCK_FLASH_PART_SIZE);
    assert_flat_view_extent(qts, HUB_REMOTE_FLASH,
                            SGI_BEDROCK_FLASH_PART_SIZE);

    qtest_quit(qts);
}

/*
 * The reset vector and the bounded high local view are the windows the
 * processor actually fetches firmware from without a flash part attached.
 * A stock-PROM boot must reach them as direct storage, not as the i/o
 * handlers a flash part overlays those same two addresses with.
 */
static void test_prom_execution_windows_are_not_io(const void *data)
{
    QTestState *qts = machine_start();

    assert_flat_view_is_execution_storage(qts, PROM_RESET);
    assert_flat_view_is_execution_storage(qts, HUB_LOCAL_HIGH_BOOT);

    qtest_quit(qts);
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
 * The controller speaks the same framed protocol through the machine's
 * mapped window as it does on its own bus, so these helpers mirror the
 * device test's, addressed at the window instead.
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
     * Synthetic bytes owned by this test. The controller's presence-detect
     * area rejects a record whose first 64 bytes do not sum to zero, so the
     * fixture satisfies that and says nothing else.
     */
    uint8_t chassis[24], board[40], spd[128] = { 0 };
    uint8_t reply[131];
    QTestState *qts;

    g_assert_no_error(error);
    memset(chassis, 0xc1, sizeof(chassis));
    memset(board, 0xb2, sizeof(board));
    spd[0] = 64;
    spd[63] = 192;
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

    /* Both consumers reach one response: the firmware's form and OpenBSD's. */
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

    /* The response is fixed rather than stored, so reset cannot disturb it. */
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
 * from the fitted memory rather than from the aperture, and nothing answers
 * on either side. A machine that mapped the region but left the controller's
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

    /* The first byte and the last byte of the fitted extent both answer. */
    g_assert_cmphex(qtest_readb(qts, HUB_ECC), ==, 0);
    g_assert_cmphex(qtest_readb(qts, HUB_ECC + ECC_EXTENT - 1), ==, 0);
    /* An aligned halfword is the other width the carrier takes. */
    g_assert_cmphex(qtest_readw(qts, HUB_ECC + ECC_EXTENT - 2), ==, 0);

    /*
     * One byte past the fitted extent is refused while still inside the
     * aperture, which is what separates the two bounds from each other.
     */
    qtest_readb(qts, HUB_ECC + ECC_EXTENT);

    /*
     * Neither the word below the base nor the far side of the aperture is
     * mapped, so the region has not been stretched to fill the space.
     */
    assert_flat_view_unmapped(qts, HUB_ECC - 8);
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
    /* A format-masked word beside an opaque one, both distinctive. */
    const uint64_t entry = UINT64_C(0xc001d00d);
    const uint64_t counter = UINT64_C(0x0123456789abcdef);
    const uint64_t carrier = HUB_DIRECTORY + DIRECTORY_LINE_1A00000;
    const uint64_t last = HUB_DIRECTORY + DIRECTORY_LAST_FITTED;
    const uint64_t absent = HUB_DIRECTORY + DIRECTORY_FIRST_ABSENT;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_BANK0_512M);

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
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_BANK0_512M);
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
 * when it is marked read-only, so the part would accept the sequence and
 * stop answering with array data underneath the running firmware.
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
    wait_for_migration(from);
    /* The source completes when its state is sent, not when it is loaded. */
    wait_for_migration(to);

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

static void test_prom_second_stride_view(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-second-stride-log-XXXXXX.txt", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf("-d unimp -D %s", quoted);
    qts = machine_start_with(prom_path, extra);

    assert_erased_second_position(qts);

    /* Writes change neither the erased second position nor position zero. */
    qtest_writeb(qts, HUB_LOCAL_FLASH, 0x00);
    qtest_writeq(qts, HUB_LOCAL_FLASH + FLASH_OBSERVED_OFFSET, 0);
    qtest_writeq(qts, HUB_LOCAL_FLASH +
                 SGI_BEDROCK_FLASH_PART_SIZE - 8, 0);
    qtest_writeb(qts, HUB_REMOTE_FLASH, 0x00);
    qtest_writeq(qts, HUB_REMOTE_FLASH + FLASH_INTERIOR_OFFSET, 0);
    qtest_writeb(qts, HUB_REMOTE_FLASH +
                 SGI_BEDROCK_FLASH_PART_SIZE - 1, 0);
    assert_erased_second_position(qts);
    g_assert_cmphex(qtest_readb(qts, PROM_RESET), ==, 0x31);

    qtest_system_reset(qts);
    assert_erased_second_position(qts);
    g_assert_cmphex(qtest_readb(qts, PROM_RESET), ==, 0x31);

    /* Stopping the machine flushes the log this run has been writing. */
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "flash offset 0x0\n"));
    g_assert_nonnull(strstr(log, "flash offset 0x1e0000\n"));
    g_assert_nonnull(strstr(log, "flash offset 0x7ffff8\n"));
    g_assert_nonnull(strstr(log, "flash offset 0x400000\n"));
    g_assert_nonnull(strstr(log, "flash offset 0x7fffff\n"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
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
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *short_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", short_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *long_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", long_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(empty_args, "must be exactly 1476264 bytes");
    assert_qemu_rejects(short_args, "must be exactly 1476264 bytes");
    assert_qemu_rejects(long_args, "must be exactly 1476264 bytes");

    g_assert_cmpint(g_unlink(empty), ==, 0);
    g_assert_cmpint(g_unlink(short_path), ==, 0);
    g_assert_cmpint(g_unlink(long_path), ==, 0);
}

static void test_prom_is_required(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(args, "requires -bios");
}

static void test_cpu_count_is_exact(const void *data)
{
    const char *one_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "1", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *three_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "3", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(one_args, "Invalid SMP CPUs 1");
    assert_qemu_rejects(three_args, "Invalid SMP CPUs 3");
}

static void test_ram_size_is_exact(const void *data)
{
    const char *small_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-m", "256M", "-bios", prom_path,
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *large_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-m", "1G", "-bios", prom_path,
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(small_args, "requires 512 MiB of RAM");
    assert_qemu_rejects(large_args, "requires 512 MiB of RAM");
}

static void test_cpu_type_is_r14000(const void *data)
{
    const char *r4000_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R4000",
        "-smp", "2", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *r10000_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R10000",
        "-smp", "2", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(r4000_args, "Invalid CPU model: R4000");
    assert_qemu_rejects(r10000_args, "Invalid CPU model: R10000");
}

static void test_machine_defaults(const void *data)
{
    static const char * const cpu_paths[] = {
        "/machine/node0/cpu0", "/machine/node0/cpu1",
    };
    static const char * const clock_paths[] = {
        "/machine/node0/cpu0/clk-in", "/machine/node0/cpu1/clk-in",
        "/machine/cpu-refclk",
    };
    QTestState *qts = machine_start_bare("-S");
    g_autoptr(QDict) response = NULL;
    QList *cpus;

    response = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    cpus = qdict_get_qlist(response, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, 2);

    for (size_t i = 0; i < ARRAY_SIZE(cpu_paths); i++) {
        qobject_unref(response);
        response = qtest_qmp(qts,
            "{'execute':'qom-get', 'arguments':{"
            "'path':%s, 'property':'type'}}", cpu_paths[i]);
        g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                        "R14000-mips64-cpu");
        qobject_unref(response);
        response = qtest_qmp(qts,
            "{'execute':'qom-get', 'arguments':{"
            "'path':%s, 'property':'start-powered-off'}}", cpu_paths[i]);
        g_assert_true(qdict_get_bool(response, "return"));
    }

    for (size_t i = 0; i < ARRAY_SIZE(clock_paths); i++) {
        qobject_unref(response);
        response = qtest_qmp(qts,
            "{'execute':'qom-get', 'arguments':{"
            "'path':%s, 'property':'qtest-clock-period'}}", clock_paths[i]);
        g_assert_cmpint(qdict_get_int(response, "return"), ==,
                        UINT64_C(2) << 32);
    }

    qobject_unref(response);
    response = qtest_qmp(qts,
        "{'execute':'qom-get', 'arguments':{"
        "'path':'/machine', 'property':'cpu-release-skew-ns'}}");
    g_assert_cmpint(qdict_get_int(response, "return"), ==, 0);

    qtest_quit(qts);
}

static void test_cpu_b_enable_controls_bus_eligibility(const void *data)
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

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    progress = run_vm_until_cpu_gpr_changes(qts, 1, "t0", 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 1);
    progress = run_vm_until_cpu_gpr_changes(qts, 1, "t0", progress);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", progress);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", 0);

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

static void test_cpu_b_executes_across_enable_falling_edge(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t cpu_b_progress;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_control_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    cpu_b_progress = wait_for_cpu_gpr_change(qts, 1, "t0", 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    wait_for_cpu_gpr_change(qts, 1, "t0", cpu_b_progress);
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
     * moves: the store is inert. This is the same wiring that formerly carried
     * the NMI to the processor, so it still exercises that path, now silent.
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

static void test_prom_executes_from_reset_and_xkphys(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t xkphys = 0;
    uint64_t fault = 0;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_xkphys_fetch_prom();
    qts = machine_start_with(path, "-accel tcg -no-shutdown -S");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < 10000; tries++) {
        xkphys = qtest_readq(qts, EXEC_STATE_PHYS +
                             EXEC_XKPHYS_STATE_OFFSET);
        fault = qtest_readq(qts, EXEC_STATE_PHYS + EXEC_FAULT_OFFSET);
        if (xkphys || fault) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmphex(qtest_readq(qts, EXEC_STATE_PHYS + EXEC_RESET_OFFSET), ==,
                    EXEC_RESET_MARKER);
    if (fault) {
        g_autofree char *registers = qtest_hmp(qts, "info registers 0");

        g_test_message("CPU state after XKPHYS fetch fault");
        g_test_message("%s", registers);
    }
    g_assert_cmphex(fault, ==, 0);
    g_assert_cmphex(xkphys, ==, EXEC_XKPHYS_MARKER);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_prom_executes_from_tlb_mapped_xkseg(const void *data)
{
    const TlbFetchCase *test = data;
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t reached = 0;
    uint64_t exception = 0;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_tlb_fetch_prom(test->entryhi_low, test->pagemask,
                                 test->entrylo0_low, test->entrylo1_low,
                                 test->physical_offset);
    qts = machine_start_with(path, "-accel tcg -no-shutdown -S");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < 10000; tries++) {
        reached = qtest_readq(qts, EXEC_STATE_PHYS + EXEC_TLB_STATE_OFFSET);
        exception = qtest_readq(qts, EXEC_STATE_PHYS +
                                EXEC_TLB_EXCEPTION_OFFSET);
        if (reached || exception) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmphex(qtest_readq(qts, EXEC_STATE_PHYS + EXEC_TLB_SETUP_OFFSET),
                    ==, EXEC_TLB_SETUP_MARKER);
    if (exception) {
        g_autofree char *registers = qtest_hmp(qts, "info registers 0");

        g_test_message("CPU state after xkseg TLB fetch exception");
        g_test_message("%s", registers);
    }
    g_assert_cmphex(exception, ==, 0);
    g_assert_cmphex(reached, ==, EXEC_TLB_MARKER);

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
    wait_for_migration(from);
    /* The source completes when its state is sent, not when it is loaded. */
    wait_for_migration(to);

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

static void test_cpu_b_enabled_migration(const void *data)
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

    qtest_writeq(from, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    run_vm_until_cpu_gpr_changes(from, 1, "t0", 0);
    g_assert_cmphex(cpu_gpr(from, 1, "t0"), !=, 0);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    wait_for_migration(to);
    stop_vm_if_running(to);

    stopped = cpu_gpr(to, 1, "t0");
    g_assert_cmphex(stopped, !=, 0);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 1);
    qtest_writeq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 0);
    stopped = run_vm_until_cpu_gpr_changes(to, 1, "t0", stopped);

    qtest_writeq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    g_assert_cmphex(qtest_readq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B), ==, 1);
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
 * processors shared one system container with no window at all. It
 * forwards instead of serving from its own buffer whenever no processor
 * is in the PROM's cache-only phase -- true throughout a qtest run,
 * which never executes guest code here, so the phase is never entered.
 * A forwarded access still traces, and that trace is what tells a
 * broken post-load apart from a working one for each processor's own
 * window: if migration left either filter disabled or removed, that
 * processor's access would fall straight through to the flash it
 * overlays instead of tracing a forward, silently.
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
    wait_for_migration(from);
    wait_for_migration(to);

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
    qtest_add_data_func("sn1/address-map/hub-local-reaches-bedrock", NULL,
                        test_hub_local_reaches_bedrock);
    qtest_add_data_func("sn1/address-map/hub-remote-reaches-bedrock-tail", NULL,
                        test_hub_remote_reaches_bedrock_tail);
    qtest_add_data_func("sn1/address-map/hub-remote-reaches-processor-interface",
                        NULL, test_hub_remote_reaches_processor_interface);
    qtest_add_data_func("sn1/address-map/crosstalk-big-window", NULL,
                        test_crosstalk_big_window_routes);
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
    qtest_add_data_func("sn1/address-map/hub-special-register-windows", NULL,
                        test_hub_special_register_windows);
    qtest_add_data_func("sn1/address-map/l1-operator-console", NULL,
                        test_l1_operator_console_uses_serial0);
    qtest_add_data_func("sn1/address-map/prom-four-read-only-views", NULL,
                        test_prom_four_read_only_views);
    qtest_add_data_func("sn1/address-map/prom-cpu-module-config", NULL,
                        test_prom_cpu_module_config);
    qtest_add_data_func("sn1/address-map/prom-flat-view-extents", NULL,
                        test_prom_flat_view_extents);
    qtest_add_data_func("sn1/address-map/prom-execution-windows-not-io", NULL,
                        test_prom_execution_windows_are_not_io);
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
    qtest_add_data_func("sn1/address-map/prom-second-stride-view", NULL,
                        test_prom_second_stride_view);
    qtest_add_data_func("sn1/address-map/prom-size-is-exact", NULL,
                        test_prom_size_is_exact);
    qtest_add_data_func("sn1/address-map/boot-flash-image-contract", NULL,
                        test_boot_flash_image_contract);
    qtest_add_data_func("sn1/address-map/boot-flash-nonraw-image", NULL,
                        test_boot_flash_nonraw_image);
    qtest_add_data_func("sn1/address-map/prom-is-required", NULL,
                        test_prom_is_required);
    qtest_add_data_func("sn1/address-map/cpu-count-is-exact", NULL,
                        test_cpu_count_is_exact);
    qtest_add_data_func("sn1/address-map/ram-size-is-exact", NULL,
                        test_ram_size_is_exact);
    qtest_add_data_func("sn1/address-map/cpu-type-is-r14000", NULL,
                        test_cpu_type_is_r14000);
    qtest_add_data_func("sn1/address-map/machine-defaults", NULL,
                        test_machine_defaults);
    qtest_add_data_func("sn1/address-map/cpu-b-enable-controls-bus", NULL,
                        test_cpu_b_enable_controls_bus_eligibility);
    qtest_add_data_func("sn1/address-map/config0-cold-reset", NULL,
                        test_config0_cold_reset);
    qtest_add_data_func("sn1/address-map/cpu-b-executes-across-enable-fall",
                        NULL, test_cpu_b_executes_across_enable_falling_edge);
    qtest_add_data_func("sn1/address-map/nmi-write-a-is-inert", &nmi_a,
                        test_nmi_register_write_is_inert);
    qtest_add_data_func("sn1/address-map/nmi-write-b-is-inert", &nmi_b,
                        test_nmi_register_write_is_inert);
    qtest_add_data_func("sn1/address-map/prom-executes-reset-and-xkphys", NULL,
                        test_prom_executes_from_reset_and_xkphys);
    qtest_add_data_func("sn1/address-map/prom-executes-tlb-mapped-xkseg",
                        &tlb_fetch_4k,
                        test_prom_executes_from_tlb_mapped_xkseg);
    qtest_add_data_func("sn1/address-map/prom-executes-1m-tlb-mapped-xkseg",
                        &tlb_fetch_1m,
                        test_prom_executes_from_tlb_mapped_xkseg);
    qtest_add_data_func("sn1/address-map/prom-migration", NULL,
                        test_prom_migration);
    qtest_add_data_func("sn1/address-map/cpu-b-enabled-migration", NULL,
                        test_cpu_b_enabled_migration);
    qtest_add_data_func("sn1/address-map/promdata-ram-copy-home", NULL,
                        test_promdata_ram_copy_is_execution_home);
    qtest_add_data_func("sn1/address-map/promdata-filter-survives-migration",
                        NULL, test_promdata_filter_survives_migration);
    qtest_add_data_func("sn1/address-map/system-reset", NULL,
                        test_system_reset_clears_bedrock_state);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
