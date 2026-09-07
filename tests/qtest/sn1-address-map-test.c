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
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include <poll.h>
#include <sys/socket.h>

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define SPACE_HUB UINT64_C(0x10000000000)
#define SPACE_XTALK UINT64_C(0x20000000000)
#define SPACE_MEMORY_SPECIAL UINT64_C(0x30000000000)

#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define BEDROCK_PI1 UINT64_C(0x200000)
#define BEDROCK_IO UINT64_C(0x400000)
#define BEDROCK_LB UINT64_C(0x600000)
#define BEDROCK_NI UINT64_C(0x680000)
#define BEDROCK_WINDOW_SIZE UINT64_C(0x800000)

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
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define HUB_REMOTE_BOOT (SPACE_HUB + UINT64_C(0x38000000))
#define L1_UART UINT64_C(0x80)
#define L1_SCRATCH UINT64_C(0x38)

#define PROM_RESET UINT64_C(0x1fc00000)
#define PROM_SIZE UINT64_C(0x1686a8)
#define PROM_CONFIG_OFFSET UINT64_C(0x60)

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
#define CPU_ENABLE_RACE_DONE UINT64_C(0x1d0e)
#define POLL_TRIES 10000
#define POLL_INTERVAL_US 1000

#define BEDROCK_WIDGET_PART UINT64_C(0xc110)
#define BEDROCK_WIDGET_MFG UINT64_C(0x036)
#define XBRIDGE_XBOW_PART UINT64_C(0xd000)
#define XBRIDGE_HEAD_PART UINT64_C(0xd002)
#define XBRIDGE_HEAD_MFG UINT64_C(0x024)
#define WIDGET_PART(v) (((v) >> 12) & 0xffff)
#define WIDGET_MFG(v) (((v) >> 1) & 0x7ff)
#define WIDGET_REV(v) ((v) >> 28)

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

static char *create_cpu_enable_race_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }

    /*
     * CPU A claims a RAM word before enabling CPU B.  CPU B therefore takes
     * the secondary path when the queued unhalt eventually reaches it.
     */
    stl_be_p(image + 0x400, 0x3c08a010); /* lui t0, 0xa010 */
    stl_be_p(image + 0x404, 0xdd090000); /* ld t1, 0(t0) */
    stl_be_p(image + 0x408, 0x15200019); /* bne t1, zero, 0x470 */
    stl_be_p(image + 0x40c, 0x00000000); /* nop */
    stl_be_p(image + 0x410, 0x34090001); /* ori t1, zero, 1 */
    stl_be_p(image + 0x414, 0xfd090000); /* sd t1, 0(t0) */
    stl_be_p(image + 0x418, 0x0000000f); /* sync */

    /* Enable 64-bit kernel addressing and select the crosstalk window. */
    stl_be_p(image + 0x41c, 0x400a6000); /* mfc0 t2, Status */
    stl_be_p(image + 0x420, 0x354a0080); /* ori t2, t2, KX */
    stl_be_p(image + 0x424, 0x408a6000); /* mtc0 t2, Status */
    stl_be_p(image + 0x428, 0x000000c0); /* ehb */
    stl_be_p(image + 0x42c, 0x3c0aba00); /* lui t2, 0xba00 */
    stl_be_p(image + 0x430, 0x000a503c); /* dsll32 t2, t2, 0 */
    stl_be_p(image + 0x434, 0x3c0b0100); /* lui t3, 0x0100 */
    stl_be_p(image + 0x438, 0x014b5025); /* or t2, t2, t3 */

    /*
     * These stores share one translation block.  CPU B's exclusive unhalt
     * cannot run until CPU A leaves that block, so the disable is guaranteed
     * to happen before the queued unhalt callback.
     */
    stl_be_p(image + 0x43c, 0x340c0001); /* ori t4, zero, 1 */
    stl_be_p(image + 0x440, 0xfd4c0058); /* sd t4, PI_CPU_ENABLE_B(t2) */
    stl_be_p(image + 0x444, 0xfd400058); /* sd zero, PI_CPU_ENABLE_B(t2) */
    stl_be_p(image + 0x448, 0x0000000f); /* sync */
    stl_be_p(image + 0x44c, 0x34101d0e); /* ori s0, zero, done */
    stl_be_p(image + 0x450, 0x658c0001); /* daddiu t4, t4, 1 */
    stl_be_p(image + 0x454, 0x1000fffe); /* beq zero, zero, 0x450 */
    stl_be_p(image + 0x458, 0x00000000); /* nop */

    stl_be_p(image + 0x470, 0x34102b00); /* ori s0, zero, secondary */
    stl_be_p(image + 0x474, 0x65ad0001); /* daddiu t5, t5, 1 */
    stl_be_p(image + 0x478, 0x1000fffe); /* beq zero, zero, 0x474 */
    stl_be_p(image + 0x47c, 0x00000000); /* nop */

    fd = g_file_open_tmp("sn1-cpu-enable-race-prom-XXXXXX.bin", &path,
                         &error);
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

static void assert_flat_view_unmapped(QTestState *qts, uint64_t address)
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

        if (!line_end || line_end > system_view_end) {
            line_end = system_view_end;
        }
        if (sscanf(line, " %" SCNx64 "-%" SCNx64, &start, &end) == 2 &&
            address >= start && address <= end) {
            g_autofree char *mapping = g_strndup(line, line_end - line);

            g_test_message("0x%" PRIx64 " is mapped by %s", address,
                           mapping);
            g_assert_not_reached();
        }
        line = line_end + (line_end < system_view_end);
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

    /* The local uncached alias is exactly 256 MiB. */
    assert_flat_view_unmapped(qts, SPACE_HUB + 256 * MiB);
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

    qtest_quit(qts);
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

    expect_console_text(sv[0], "001c01-L1> ");

    qtest_quit(qts);
    g_assert_cmpint(close(sv[0]), ==, 0);
}

static void test_prom_three_read_only_views(const void *data)
{
    QTestState *qts = machine_start();
    static const uint64_t views[] = {
        PROM_RESET, HUB_LOCAL_BOOT, HUB_REMOTE_BOOT,
    };
    static const struct {
        uint64_t offset;
        uint8_t value;
    } probes[] = {
        { 0, 0x31 },
        { PROM_CONFIG_OFFSET, 0x62 },
        { PROM_SIZE - 1, 0x93 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            uint64_t address = views[i] + probes[j].offset;

            g_assert_cmphex(qtest_readb(qts, address), ==, probes[j].value);
            qtest_writeb(qts, address, probes[j].value ^ 0xff);
            g_assert_cmphex(qtest_readb(qts, address), ==, probes[j].value);
        }
    }

    /* Both boot aliases stop at the exact supplied length. */
    assert_flat_view_unmapped(qts, HUB_LOCAL_BOOT + PROM_SIZE);
    assert_flat_view_unmapped(qts, HUB_REMOTE_BOOT + PROM_SIZE);

    /* The reset view ends there too and reveals the RAM underneath. */
    qtest_writeq(qts, PROM_RESET + PROM_SIZE,
                 UINT64_C(0x7766554433221100));
    g_assert_cmphex(qtest_readq(qts, PROM_RESET + PROM_SIZE), ==,
                    UINT64_C(0x7766554433221100));

    qtest_quit(qts);
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
    assert_flat_view_extent(qts, HUB_REMOTE_BOOT, PROM_SIZE);

    qtest_quit(qts);
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
        "/machine/cpu0", "/machine/cpu1",
    };
    static const char * const clock_paths[] = {
        "/machine/cpu0/clk-in", "/machine/cpu1/clk-in",
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
    }

    for (size_t i = 0; i < ARRAY_SIZE(clock_paths); i++) {
        qobject_unref(response);
        response = qtest_qmp(qts,
            "{'execute':'qom-get', 'arguments':{"
            "'path':%s, 'property':'qtest-clock-period'}}", clock_paths[i]);
        g_assert_cmpint(qdict_get_int(response, "return"), ==,
                        UINT64_C(2) << 32);
    }

    qtest_quit(qts);
}

static void test_cpu_b_follows_enable(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t stopped;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_control_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    run_vm_until_cpu_gpr_changes(qts, 0, "t0", 0);
    stopped = cpu_gpr(qts, 1, "t0");
    g_assert_cmphex(stopped, ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    run_vm_until_cpu_gpr_changes(qts, 0, "t0", cpu_gpr(qts, 0, "t0"));
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, stopped);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", stopped);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), !=, stopped);

    stopped = cpu_gpr(qts, 1, "t0");
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", stopped);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), !=, stopped);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr_change(qts, 1, "t0", cpu_gpr(qts, 1, "t0"));
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    wait_for_cpu_gpr_change(qts, 0, "t0", cpu_gpr(qts, 0, "t0"));
    stop_vm(qts);
    stopped = cpu_gpr(qts, 1, "t0");
    run_vm_until_cpu_gpr_changes(qts, 0, "t0", cpu_gpr(qts, 0, "t0"));
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, stopped);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_system_reset(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);
    run_vm_until_cpu_gpr_changes(qts, 1, "t0", 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), !=, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);
    run_vm_until_cpu_gpr_changes(qts, 0, "t0", 0);
    g_assert_cmphex(cpu_gpr(qts, 0, "t0"), !=, 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_cpu_b_disable_overtakes_queued_unhalt(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t cpu_a_progress;
    uint64_t cpu_b_stopped;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_enable_race_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_cpu_gpr(qts, 0, "s0", CPU_ENABLE_RACE_DONE);

    /*
     * Stopping drains CPU B's queued work.  Run CPU A through another
     * observed transition afterward; CPU B must make no further progress.
     */
    stop_vm(qts);
    cpu_b_stopped = cpu_gpr(qts, 1, "t5");
    cpu_a_progress = cpu_gpr(qts, 0, "t4");
    run_vm_until_cpu_gpr_changes(qts, 0, "t4", cpu_a_progress);
    g_assert_cmphex(cpu_gpr(qts, 1, "t5"), ==, cpu_b_stopped);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_nmi_routes_to_cpu(const void *data)
{
    uint64_t nmi_register = *(const uint64_t *)data;
    unsigned int target_cpu = nmi_register == PI_NMI_A ? 0 : 1;
    unsigned int other_cpu = target_cpu ^ 1;
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

    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        wait_for_cpu_gpr(qts, cpu, "s0", NMI_READY_MARKER);
    }

    qtest_writeq(qts, BEDROCK_LOCAL + nmi_register, 0);
    wait_for_cpu_gpr(qts, target_cpu, "s0", NMI_TAKEN_MARKER);
    g_assert_cmphex(cpu_gpr(qts, other_cpu, "s0"), ==, NMI_READY_MARKER);

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
        PROM_RESET, HUB_LOCAL_BOOT, HUB_REMOTE_BOOT,
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

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            g_assert_cmphex(qtest_readb(to, views[i] + probes[j].offset), ==,
                            probes[j].source);
        }
    }

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
    qtest_writeq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    run_vm_until_cpu_gpr_changes(to, 0, "t0", cpu_gpr(to, 0, "t0"));
    g_assert_cmphex(cpu_gpr(to, 1, "t0"), ==, stopped);

    qtest_writeq(to, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    run_vm_until_cpu_gpr_changes(to, 1, "t0", stopped);
    g_assert_cmphex(cpu_gpr(to, 1, "t0"), !=, stopped);

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_unlink(path), ==, 0);
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
    qtest_add_data_func("sn1/address-map/crosstalk-big-window", NULL,
                        test_crosstalk_big_window_routes);
    qtest_add_data_func("sn1/address-map/xbridge-topology", NULL,
                        test_xbridge_topology);
    qtest_add_data_func("sn1/address-map/hub-special-register-windows", NULL,
                        test_hub_special_register_windows);
    qtest_add_data_func("sn1/address-map/l1-operator-console", NULL,
                        test_l1_operator_console_uses_serial0);
    qtest_add_data_func("sn1/address-map/prom-three-read-only-views", NULL,
                        test_prom_three_read_only_views);
    qtest_add_data_func("sn1/address-map/prom-flat-view-extents", NULL,
                        test_prom_flat_view_extents);
    qtest_add_data_func("sn1/address-map/prom-size-is-exact", NULL,
                        test_prom_size_is_exact);
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
    qtest_add_data_func("sn1/address-map/cpu-b-follows-enable", NULL,
                        test_cpu_b_follows_enable);
    qtest_add_data_func("sn1/address-map/cpu-b-disable-overtakes-unhalt", NULL,
                        test_cpu_b_disable_overtakes_queued_unhalt);
    qtest_add_data_func("sn1/address-map/nmi-routes-to-cpu-a", &nmi_a,
                        test_nmi_routes_to_cpu);
    qtest_add_data_func("sn1/address-map/nmi-routes-to-cpu-b", &nmi_b,
                        test_nmi_routes_to_cpu);
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
    qtest_add_data_func("sn1/address-map/system-reset", NULL,
                        test_system_reset_clears_bedrock_state);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
