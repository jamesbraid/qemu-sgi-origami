/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SGI SHub 1: the registers the reviewed register notes define or the PROM's
 * reset path needs. Anything else is refused and logged, never answered.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/block/flash.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "hw/core/cpu.h"
#include "hw/ia64/sapic.h"
#include "hw/ia64/sgi-shub.h"
#include "hw/misc/sgi-xio-port.h"
#include "hw/ia64/sgi-shub-regs.h"
#include "hw/ia64/sgi-shub-ii-regs.h"

typedef enum {
    K_STORE,            /* plain 64-bit storage in a slot */
    K_ELECT_SET,        /* write-only side of the election word */
    K_IPI_INT,
    K_RTC_INT_CONFIG,
    K_RTC_INT_ENABLE,
    K_EVENT,
    K_EVENT_ALIAS,
    K_EVENT_OVERFLOW_ALIAS,
    K_SHUB_ID,
    K_PTC,
    K_INT_CMP,
    K_RTC,
    K_RTC_CTL,
    K_PI_CAM_CONTROL,
    K_PIO_WRITE_STATUS,
    K_PIO_WRITE_STATUS_ALIAS,
    K_LED,
    K_CHIP_ID,
    K_WRITE_ONLY,       /* writes accepted and ignored, reads stop */
    K_MBIST_CTL,
    K_CLK_SEL,
    K_BTE_LEN,
    K_BTE_CTL,
    K_SLICE_REC,
    K_SLICE_PRESENT,
    K_PIO_ERR_HOLE,     /* unnamed LB address: a write raises a PIO error */
    K_PI_CREDITS,       /* PI-to-memory credits: reads 8, no writes */
    K_DIR_RESULT,       /* directory-entry read result: reads 0, no writes */
    K_LOCK,
    K_LOCK_STATUS,
    K_LOCK2,
    K_LOCK2_STATUS,
    K_LLP_CSR,          /* II link control: the link reads up */
    K_CRB_MSG,          /* CRB message control; the quiesce store stops */
    K_II_INT,           /* a PIC interrupt packet's target */
} ShubKind;

/* The coherent request buffer's message control resets to 6 (notes). */
#define SHUB_CRB_MESSAGE_CONTROL_RESET 6ULL
/* The PROM's die path stores this to stop coherent traffic, then spins. */
#define SHUB_CRB_QUIESCE 0x1000100000000ULL

#define SHUB_WIDGET_WINDOW (16 * MiB)
#define SHUB_WIDGET_SPACE (4 * GiB)
#define SHUB_BIG_WINDOW (512 * MiB)

/* The directory read's four result registers, picked by address bits 11, 7. */
#define SHUB_LB_HOLE_0608 0x110050608ULL
#define SHUB_PIO_ERR_MULTI 1ULL
#define SHUB_PIO_ERR_WRITE 4ULL
#define SHUB_UART_INT_ENABLE (1ULL << 13)
#define SHUB_PIO_ERR_ADDR_MASK 0x3fffffffffff8ULL
#define SHUB_LB_ERROR_DETAIL_1 0x110050200ULL
#define SHUB_LB_ERROR_DETAIL_2 0x110050280ULL
#define SHUB_PI_ERROR_DETAIL_1 0x120060500ULL
#define SHUB_LB_ERROR_DETAIL_VALID (1ULL << 63)
#define SHUB_LB_ERROR_CMD_WRITE 0xea
#define SHUB_PI_ERROR_ADDR_MASK 0x1fffffff8ULL
#define SHUB_PIO_ALIAS_CLEAR_ALL 0x3fffffffffffdULL
#define SHUB_MD_DIR_RESULT 0x100032000ULL
#define SHUB_MD_DIR_RESULT_HIGH 0x100032800ULL
#define SHUB_MD_DIR_RESULT_STRIDE 0x20000ULL

/* SH_FSB_VGA_REMAP, from the 2.4 header; not yet in the generated list. */
#define SHUB_FSB_VGA_REMAP 0x120010180ULL

/* An LB register the PROM writes once; the page gives it no name. */
#define SHUB_UNNAMED_1007_1080 0x110071080ULL

/* The II block is reached at global offset 0x1800000 plus its register. */
#define II_REG(r) (0x1800000ULL + (r))

typedef struct {
    ShubKind kind;
    uint64_t base;
    unsigned count;
    uint64_t stride;
    int slot;           /* K_STORE: first slot, one per instance */
} ShubRegSpec;

/* XN compare words the PROM zeroes at node set-up: stored cells, reset 0. */
#define SHUB_XN_ZERO_0                               0x150031220
#define SHUB_XN_ZERO_1                               0x150031230
#define SHUB_XN_ZERO_2                               0x1500313c0
#define SHUB_XN_ZERO_3                               0x1500313e0
#define SHUB_XN_ZERO_4                               0x1500315b0
#define SHUB_XN_ZERO_5                               0x1500315c0
#define SHUB_XN_ZERO_6                               0x1500315a0
#define SHUB_XN_ZERO_7                               0x150031360
#define SHUB_XN_ZERO_8                               0x150031370

static const ShubRegSpec shub_regs[] = {
    { K_STORE, SHUB_MD_DQLP_DIR_PRIVILEGE_VECTOR, 1, 0, SHUB_SLOT_PRIV0 },
    { K_STORE, SHUB_MD_DQRP_DIR_PRIVILEGE_VECTOR, 1, 0, SHUB_SLOT_PRIV1 },
    { K_STORE, SHUB_IPI_ACCESS, 1, 0, SHUB_SLOT_IPI_ACCESS },
    { K_STORE, SHUB_CPU_HIST, SHUB_CPU_HIST_COUNT, SHUB_CPU_HIST_STRIDE,
      SHUB_SLOT_HIST },
    { K_STORE, SHUB_FW_WORD_1540, 1, 0, SHUB_SLOT_FW_1540 },
    { K_STORE, SHUB_CPU_BITS, 1, 0, SHUB_SLOT_CPU_BITS },
    { K_STORE, SHUB_FW_DEBUG_WORD, 1, 0, SHUB_SLOT_FW_DEBUG },
    { K_STORE, SHUB_XN_ZERO_0, 1, 0, SHUB_SLOT_XN_ZERO },
    { K_STORE, SHUB_XN_ZERO_1, 1, 0, SHUB_SLOT_XN_ZERO + 1 },
    { K_STORE, SHUB_XN_ZERO_2, 1, 0, SHUB_SLOT_XN_ZERO + 2 },
    { K_STORE, SHUB_XN_ZERO_3, 1, 0, SHUB_SLOT_XN_ZERO + 3 },
    { K_STORE, SHUB_XN_ZERO_4, 1, 0, SHUB_SLOT_XN_ZERO + 4 },
    { K_STORE, SHUB_XN_ZERO_5, 1, 0, SHUB_SLOT_XN_ZERO + 5 },
    { K_STORE, SHUB_XN_ZERO_6, 1, 0, SHUB_SLOT_XN_ZERO + 6 },
    { K_STORE, SHUB_XN_ZERO_7, 1, 0, SHUB_SLOT_XN_ZERO + 7 },
    { K_STORE, SHUB_XN_ZERO_8, 1, 0, SHUB_SLOT_XN_ZERO + 8 },
    { K_STORE, 0x150031270, 1, 0, SHUB_SLOT_XN_ZERO + 9 },
    { K_STORE, 0x150031110, 1, 0, SHUB_SLOT_XN_ZERO + 10 },
    { K_STORE, 0x150031120, 1, 0, SHUB_SLOT_XN_ZERO + 11 },
    { K_STORE, 0x150031130, 1, 0, SHUB_SLOT_XN_ZERO + 12 },
    { K_STORE, 0x150031140, 1, 0, SHUB_SLOT_XN_ZERO + 13 },
    { K_STORE, 0x150031150, 1, 0, SHUB_SLOT_XN_ZERO + 14 },
    { K_STORE, 0x150031160, 1, 0, SHUB_SLOT_XN_ZERO + 15 },
    { K_STORE, 0x150031170, 1, 0, SHUB_SLOT_XN_ZERO + 16 },
    { K_STORE, 0x150031180, 1, 0, SHUB_SLOT_XN_ZERO + 17 },
    { K_STORE, 0x150031190, 1, 0, SHUB_SLOT_XN_ZERO + 18 },
    { K_STORE, 0x1500311a0, 1, 0, SHUB_SLOT_XN_ZERO + 19 },
    { K_STORE, 0x1500311b0, 1, 0, SHUB_SLOT_XN_ZERO + 20 },
    { K_STORE, 0x1500311c0, 1, 0, SHUB_SLOT_XN_ZERO + 21 },
    { K_STORE, 0x1500311d0, 1, 0, SHUB_SLOT_XN_ZERO + 22 },
    { K_STORE, 0x1500311e0, 1, 0, SHUB_SLOT_XN_ZERO + 23 },
    { K_STORE, 0x1500311f0, 1, 0, SHUB_SLOT_XN_ZERO + 24 },
    { K_STORE, 0x150031200, 1, 0, SHUB_SLOT_XN_ZERO + 25 },
    { K_STORE, 0x150031210, 1, 0, SHUB_SLOT_XN_ZERO + 26 },
    { K_STORE, 0x150031240, 1, 0, SHUB_SLOT_XN_ZERO + 27 },
    { K_STORE, 0x150031260, 1, 0, SHUB_SLOT_XN_ZERO + 28 },
    { K_STORE, 0x150031610, 1, 0, SHUB_SLOT_XN_ZERO + 29 },
    { K_STORE, 0x150031620, 1, 0, SHUB_SLOT_XN_ZERO + 30 },
    { K_STORE, 0x150031630, 1, 0, SHUB_SLOT_XN_ZERO + 31 },
    { K_STORE, 0x150031640, 1, 0, SHUB_SLOT_XN_ZERO + 32 },
    { K_STORE, 0x150031650, 1, 0, SHUB_SLOT_XN_ZERO + 33 },
    { K_STORE, 0x150031660, 1, 0, SHUB_SLOT_XN_ZERO + 34 },
    { K_STORE, 0x150031670, 1, 0, SHUB_SLOT_XN_ZERO + 35 },
    { K_STORE, 0x150031300, 1, 0, SHUB_SLOT_XN_ZERO + 36 },
    { K_STORE, 0x150031310, 1, 0, SHUB_SLOT_XN_ZERO + 37 },
    { K_STORE, 0x150031320, 1, 0, SHUB_SLOT_XN_ZERO + 38 },
    { K_STORE, 0x150031330, 1, 0, SHUB_SLOT_XN_ZERO + 39 },
    { K_STORE, 0x150031340, 1, 0, SHUB_SLOT_XN_ZERO + 40 },
    { K_STORE, 0x1500315e0, 1, 0, SHUB_SLOT_XN_ZERO + 41 },
    { K_STORE, 0x150031590, 1, 0, SHUB_SLOT_XN_ZERO + 42 },
    { K_STORE, 0x1500313f0, 1, 0, SHUB_SLOT_XN_ZERO + 43 },
    { K_STORE, 0x1500313d0, 1, 0, SHUB_SLOT_XN_ZERO + 44 },
    { K_STORE, 0x150031570, 1, 0, SHUB_SLOT_XN_ZERO + 45 },
    { K_STORE, 0x150031580, 1, 0, SHUB_SLOT_XN_ZERO + 46 },
    { K_STORE, 0x150031600, 1, 0, SHUB_SLOT_XN_ZERO + 47 },
    { K_STORE, SHUB_PI_REG_2001_0100, 1, 0, SHUB_SLOT_PI_2001_0100 },
    { K_STORE, SHUB_SCRATCH2, 1, 0, SHUB_SLOT_L1_STATE },
    { K_STORE, SHUB_SCRATCH1, 1, 0, SHUB_SLOT_ELECT },
    /* A write here shows up in a read of the election register. */
    { K_ELECT_SET, SHUB_SCRATCH1_ALIAS, 1, 0, SHUB_SLOT_ELECT },
    { K_RTC_CTL, SHUB_RTC_CTL, 1, 0, SHUB_SLOT_RTC_CTL },
    { K_IPI_INT, SHUB_IPI_INT, 1, 0, 0 },
    /* Comparator 0 is the firmware's; the rest follow at the same stride. */
    { K_RTC_INT_CONFIG, SHUB_RTC0_INT_CONFIG, 4, SHUB_RTC_INT_CONFIG_STRIDE,
      0 },
    { K_RTC_INT_ENABLE, SHUB_RTC0_INT_ENABLE, 4, SHUB_RTC_INT_ENABLE_STRIDE,
      0 },
    { K_STORE, SHUB_INT_NODE_ID_CONFIG, 1, 0, SHUB_SLOT_INT_NODE },
    { K_STORE, SHUB_IPI_INT_ENABLE, 1, 0, SHUB_SLOT_IPI_ENABLE },
    { K_STORE, SHUB_LOCAL_INT_CONFIG, SHUB_LOCAL_INT_CONFIG_COUNT,
      SHUB_LOCAL_INT_CONFIG_STRIDE, SHUB_SLOT_LOCAL_CFG },
    { K_STORE, SHUB_LOCAL_INT0_ENABLE, SHUB_LOCAL_INT0_ENABLE_COUNT,
      SHUB_LOCAL_INT0_ENABLE_STRIDE, SHUB_SLOT_LOCAL_EN },
    { K_STORE, SHUB_EVENT_OVERFLOW, 1, 0, SHUB_SLOT_EVENT_OVF },
    { K_EVENT_OVERFLOW_ALIAS, SHUB_EVENT_OVERFLOW_ALIAS, 1, 0, 0 },
    { K_STORE, SHUB_PTC_PARMS, 1, 0, SHUB_SLOT_PTC_PARMS },
    { K_STORE, SHUB_FSB_VGA_REMAP, 1, 0, SHUB_SLOT_FSB_VGA_REMAP },
    { K_STORE, II_REG(SHUB_II_ITTE), SHUB_II_ITTE_COUNT, SHUB_II_ITTE_STRIDE,
      SHUB_SLOT_ITTE },
    { K_PIO_ERR_HOLE, SHUB_LB_HOLE_0608, 1, 0, 0 },
    { K_WRITE_ONLY, 0x120050d80ULL, 1, 0, 0 },
    { K_STORE, 0x150000010ULL, 2, 0x2000, SHUB_SLOT_LLP_RESET_EN },
    { K_PI_CREDITS, 0x120060980ULL, 1, 0, 0 },
    { K_EVENT, SHUB_EVENT_OCCURRED, 1, 0, 0 },
    { K_EVENT_ALIAS, SHUB_EVENT_OCCURRED_ALIAS, 1, 0, 0 },
    { K_SHUB_ID, SHUB_SHUB_ID, 1, 0, 0 },
    { K_PTC, SHUB_PTC, SHUB_PTC_COUNT, SHUB_PTC_STRIDE, 0 },
    { K_INT_CMP, SHUB_INT_CMPA, 4, SHUB_INT_CMP_STRIDE, 0 },
    { K_RTC, SHUB_RTC, 1, 0, 0 },
    { K_PI_CAM_CONTROL, SHUB_PI_CAM_CONTROL, 1, 0, 0 },
    { K_PIO_WRITE_STATUS, SHUB_PIO_WRITE_STATUS, SHUB_PIO_WRITE_STATUS_COUNT,
      SHUB_PIO_WRITE_STATUS_STRIDE, 0 },
    { K_PIO_WRITE_STATUS_ALIAS, SHUB_PIO_WRITE_STATUS_ALIAS,
      SHUB_PIO_WRITE_STATUS_ALIAS_COUNT, SHUB_PIO_WRITE_STATUS_ALIAS_STRIDE,
      0 },
    { K_LED, SHUB_JUNK_BUS_LED, SHUB_JUNK_BUS_LED_COUNT,
      SHUB_JUNK_BUS_LED_STRIDE, 0 },
    { K_CHIP_ID, SHUB_MD_REG_0000_0000, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_JUNK_BUS_TIME, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0003_0400, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0005_0400, 1, 0, 0 },
    { K_STORE, SHUB_PIO_RQ_CRD_CTL, 1, 0, SHUB_SLOT_MD_0068 },
    /* Memory bring-up: devices/shub/memory-init.md in the chips repository. */
    { K_STORE, SHUB_X_DIMM_CFG, 1, 0, SHUB_SLOT_MD_BANK_CFG },
    { K_STORE, SHUB_Y_DIMM_CFG, 1, 0, SHUB_SLOT_MD_BANK_CFG + 1 },
    { K_STORE, SHUB_JNR_DIMM_CFG, 1, 0, SHUB_SLOT_MD_BANK_CFG + 2 },
    { K_STORE, SHUB_Y_CFG, 1, 0, SHUB_SLOT_MD_TIMING_2 },
    { K_STORE, SHUB_Y_DQCT_CFG, 1, 0, SHUB_SLOT_MD_TIMING_3 },
    { K_STORE, SHUB_Y_REFRESH_CONTROL, 1, 0, SHUB_SLOT_MD_REFRESH_1 },
    { K_STORE, SHUB_MD_REG_0003_0000, 1, 0, SHUB_SLOT_MD_0003_0000 },
    { K_STORE, SHUB_MD_DQRP_MMR_DIR_CONFIG, 1, 0, SHUB_SLOT_MD_0005_0000 },
    { K_STORE, SHUB_MMRBIST_ERR, 1, 0, SHUB_SLOT_MBIST_STATUS },
    { K_STORE, SHUB_MMRBIST_BASE, 1, 0, SHUB_SLOT_MBIST_CMD },
    { K_STORE, SHUB_MEM_ERROR_SUMMARY_ALIAS, 1, 0, SHUB_SLOT_MBIST_MASK },
    { K_MBIST_CTL, SHUB_MMRBIST_CTL, 1, 0, SHUB_SLOT_MBIST_CTL },
    { K_WRITE_ONLY, SHUB_X_CFG, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_X_DQCT_CFG, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_X_REFRESH_CONTROL, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MEM_RED_BLACK, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MISC_MEM_CFG, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_PI_MD_RP_CRD_CTL, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0003_0200, SHUB_MD_REG_0003_0200_COUNT,
      SHUB_MD_REG_0003_0200_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DQRP_MMR_DIR_LOCVEC0,
      SHUB_MD_DQRP_MMR_DIR_LOCVEC0_COUNT,
      SHUB_MD_DQRP_MMR_DIR_LOCVEC0_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DQLP_MMR_XBIST_H, SHUB_MD_DQLP_MMR_XBIST_H_COUNT,
      SHUB_MD_DQLP_MMR_XBIST_H_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DQLP_MMR_XBIST_L, SHUB_MD_DQLP_MMR_XBIST_L_COUNT,
      SHUB_MD_DQLP_MMR_XBIST_L_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DQLP_MMR_YBIST_H, SHUB_MD_DQLP_MMR_YBIST_H_COUNT,
      SHUB_MD_DQLP_MMR_YBIST_H_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DQLP_MMR_YBIST_L, SHUB_MD_DQLP_MMR_YBIST_L_COUNT,
      SHUB_MD_DQLP_MMR_YBIST_L_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DIR_ACCESS_LP, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_DIR_ACCESS_RP, 1, 0, 0 },
    { K_CLK_SEL, SHUB_JTAG_CONFIG, 1, 0, 0 },
    /* The rest of the I/O interface: plain cells (PROM I/O interface notes). */
    { K_STORE, II_REG(SHUB_II_WIDGET_ID), 1, 0, SHUB_SLOT_II + 0 },
    { K_STORE, II_REG(SHUB_II_WIDGET_STATUS), 1, 0, SHUB_SLOT_II + 1 },
    { K_STORE, II_REG(SHUB_II_WIDGET_CONTROL), 1, 0, SHUB_SLOT_II + 2 },
    { K_STORE, II_REG(SHUB_II_LOCAL_ACCESS_PROTECTION), 1, 0,
      SHUB_SLOT_II + 3 },
    { K_STORE, II_REG(SHUB_II_LOCAL_ACCESS_OVERRIDE), 1, 0, SHUB_SLOT_II + 4 },
    { K_STORE, II_REG(SHUB_II_OUTBOUND_WIDGET_ACCESS), 1, 0, SHUB_SLOT_II + 5 },
    { K_STORE, II_REG(SHUB_II_INBOUND_WIDGET_ACCESS), 1, 0, SHUB_SLOT_II + 6 },
    { K_STORE, II_REG(SHUB_II_LLP_LOG), 1, 0, SHUB_SLOT_II + 7 },
    { K_STORE, II_REG(SHUB_II_INTERRUPT_DESTINATION), 1, 0, SHUB_SLOT_II + 8 },
    { K_STORE, II_REG(SHUB_II_GRAPHICS_MAP), 2, 8, SHUB_SLOT_II + 9 },
    { K_STORE, II_REG(SHUB_II_SCRATCH), 2, 8, SHUB_SLOT_II + 11 },
    { K_STORE, II_REG(SHUB_II_PRB), 9, 8, SHUB_SLOT_II + 13 },
    { K_STORE, II_REG(SHUB_II_CROSSTALK_CREDIT_TIMEOUT), 1, 0,
      SHUB_SLOT_II + 22 },
    { K_STORE, II_REG(SHUB_II_CROSSTALK_TIMEOUT_THRESHOLD), 1, 0,
      SHUB_SLOT_II + 23 },
    { K_STORE, II_REG(SHUB_II_BTE_COMMON_CONTROL), 1, 0, SHUB_SLOT_II + 24 },
    { K_STORE, II_REG(SHUB_II_SPURIOUS_MESSAGE), 1, 0, SHUB_SLOT_II + 25 },
    { K_STORE, II_REG(SHUB_II_SPURIOUS_SIDEBAND), 1, 0, SHUB_SLOT_II + 26 },
    { K_STORE, II_REG(SHUB_II_LLP_CHANNEL_TEST), 1, 0, SHUB_SLOT_II + 27 },
    { K_STORE, II_REG(SHUB_II_ERROR_PACKET_HEADER_1), 1, 0, SHUB_SLOT_II + 28 },
    { K_STORE, II_REG(SHUB_II_ERROR_PACKET_HEADER_2), 1, 0, SHUB_SLOT_II + 29 },
    { K_STORE, II_REG(SHUB_II_SXB_ACCESS_PROTECTION), 1, 0, SHUB_SLOT_II + 30 },
    { K_STORE, II_REG(SHUB_II_SXB_ACCESS_OVERRIDE), 1, 0, SHUB_SLOT_II + 31 },
    { K_STORE, II_REG(SHUB_II_WRAPPER_INTERRUPT), 1, 0, SHUB_SLOT_II + 32 },
    { K_STORE, II_REG(SHUB_II_WRAPPER_ERROR_LOG), 1, 0, SHUB_SLOT_II + 33 },
    { K_STORE, II_REG(SHUB_II_WRAPPER_CONTROL), 1, 0, SHUB_SLOT_II + 34 },
    { K_STORE, II_REG(SHUB_II_WRAPPER_STATUS), 1, 0, SHUB_SLOT_II + 35 },
    { K_STORE, II_REG(SHUB_II_PRB_COUNTER_ADJUST), 1, 0, SHUB_SLOT_II + 36 },
    { K_STORE, II_REG(SHUB_II_PIO_READ_ENTRY_A), 8, 8, SHUB_SLOT_II + 37 },
    { K_STORE, II_REG(SHUB_II_PIO_READ_ENTRY_B), 8, 8, SHUB_SLOT_II + 45 },
    { K_STORE, II_REG(SHUB_II_PIO_DEALLOCATE), 1, 0, SHUB_SLOT_II + 53 },
    { K_STORE, II_REG(SHUB_II_CRB_DEALLOCATE), 1, 0, SHUB_SLOT_II + 54 },
    { K_STORE, II_REG(SHUB_II_IOQ_FIFO_DEPTH), 1, 0, SHUB_SLOT_II + 55 },
    { K_STORE, II_REG(SHUB_II_IIQ_ARBITRATION), 1, 0, SHUB_SLOT_II + 56 },
    { K_STORE, II_REG(SHUB_II_CRB_MANAGEMENT), 1, 0, SHUB_SLOT_II + 57 },
    { K_STORE, II_REG(SHUB_II_CRB_CONTROL), 1, 0, SHUB_SLOT_II + 58 },
    { K_STORE, II_REG(SHUB_II_CRB_TIMEOUT), 1, 0, SHUB_SLOT_II + 59 },
    { K_STORE, II_REG(SHUB_II_CRB_TIMEOUT_PRESCALER), 1, 0, SHUB_SLOT_II + 60 },
    { K_STORE, II_REG(SHUB_II_CRB_A), 15, 48, SHUB_SLOT_II + 61 },
    { K_STORE, II_REG(SHUB_II_CRB_B), 15, 48, SHUB_SLOT_II + 76 },
    { K_STORE, II_REG(SHUB_II_CRB_C), 15, 48, SHUB_SLOT_II + 91 },
    { K_STORE, II_REG(SHUB_II_CRB_D), 15, 48, SHUB_SLOT_II + 106 },
    { K_STORE, II_REG(SHUB_II_CRB_E), 15, 48, SHUB_SLOT_II + 121 },
    { K_STORE, II_REG(SHUB_II_SPURIOUS_CRB_MESSAGE_LOW), 1, 0,
      SHUB_SLOT_II + 136 },
    { K_STORE, II_REG(SHUB_II_SPURIOUS_CRB_MESSAGE_MID), 1, 0,
      SHUB_SLOT_II + 137 },
    { K_STORE, II_REG(SHUB_II_SPURIOUS_CRB_MESSAGE_HIGH), 1, 0,
      SHUB_SLOT_II + 138 },
    { K_STORE, II_REG(SHUB_II_DEBUG_SUBMENU_SELECT), 1, 0, SHUB_SLOT_II + 139 },
    { K_STORE, II_REG(SHUB_II_PERF_CONTROL), 1, 0, SHUB_SLOT_II + 140 },
    { K_STORE, II_REG(SHUB_II_PERF_PROFILE), 1, 0, SHUB_SLOT_II + 141 },
    { K_STORE, II_REG(SHUB_II_WRAPPER_ERROR_INTERRUPT_MASK), 1, 0,
      SHUB_SLOT_II + 142 },
    { K_LLP_CSR, II_REG(SHUB_II_LLP_CONTROL_STATUS), 1, 0, SHUB_SLOT_LLP_CSR },
    /* Coherent request buffer controls; only the first has a known reset. */
    { K_CRB_MSG, SHUB_CRB_MESSAGE_CONTROL, 1, 0, SHUB_SLOT_CRB_MSG },
    { K_STORE, SHUB_CRB_NACK_LIMIT, 1, 0, SHUB_SLOT_CRB_NACK },
    { K_STORE, SHUB_CRB_TIMEOUT_PRESCALE, 1, 0, SHUB_SLOT_CRB_PRESCALE },
    { K_STORE, SHUB_CRB_TIMEOUT_SKID, 1, 0, SHUB_SLOT_CRB_SKID },
    /* Where the PIC's interrupt packets land, with their destination. */
    { K_II_INT, SHUB_II_INT, SHUB_II_INT_COUNT, SHUB_II_INT_STRIDE, 0 },
    { K_STORE, SHUB_II_INT_CONFIG, SHUB_II_INT_CONFIG_COUNT,
      SHUB_II_INT_CONFIG_STRIDE, SHUB_SLOT_II_INT_CFG },
    { K_STORE, SHUB_II_INT_ENABLE, SHUB_II_INT_ENABLE_COUNT,
      SHUB_II_INT_ENABLE_STRIDE, SHUB_SLOT_II_INT_EN },
    /* Network routing tables and per-port flow words the PROM programs. */
    { K_STORE, 0x1500303a0, 2, 512, SHUB_SLOT_TOPO + 0 },
    { K_STORE, 0x150030400, 2, 512, SHUB_SLOT_TOPO + 2 },
    { K_STORE, 0x150022000, 128, 8, SHUB_SLOT_TOPO + 4 },
    { K_STORE, 0x150023000, 128, 8, SHUB_SLOT_TOPO + 132 },
    { K_STORE, 0x150022400, 16, 8, SHUB_SLOT_TOPO + 260 },
    { K_STORE, 0x150023400, 16, 8, SHUB_SLOT_TOPO + 276 },
    { K_STORE, 0x150022480, 1, 0, SHUB_SLOT_TOPO + 292 },
    { K_STORE, 0x150023480, 1, 0, SHUB_SLOT_TOPO + 293 },
    { K_STORE, 0x150024000, 128, 8, SHUB_SLOT_TOPO + 294 },
    { K_STORE, 0x150024400, 16, 8, SHUB_SLOT_TOPO + 422 },
    { K_STORE, 0x150020000, 128, 8, SHUB_SLOT_TOPO + 438 },
    { K_STORE, 0x150020400, 16, 8, SHUB_SLOT_TOPO + 566 },
    { K_STORE, 0x150021000, 128, 8, SHUB_SLOT_TOPO + 582 },
    { K_STORE, 0x150021400, 16, 8, SHUB_SLOT_TOPO + 710 },
    { K_STORE, 0x150024480, 1, 0, SHUB_SLOT_TOPO + 726 },
    { K_STORE, 0x150020480, 1, 0, SHUB_SLOT_TOPO + 727 },
    { K_STORE, 0x150021480, 1, 0, SHUB_SLOT_TOPO + 728 },
    { K_WRITE_ONLY, II_REG(SHUB_II_ERROR_CLEAR), 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_UNNAMED_1007_1080, 1, 0, 0 },
    /* The two block transfer engines of the I/O interface. */
    { K_BTE_LEN, II_REG(SHUB_II_BTE_LENGTH_STATUS),
      SHUB_II_BTE_LENGTH_STATUS_COUNT, SHUB_II_BTE_LENGTH_STATUS_STRIDE,
      SHUB_SLOT_BTE_LEN },
    { K_STORE, II_REG(SHUB_II_BTE_SOURCE), SHUB_II_BTE_SOURCE_COUNT,
      SHUB_II_BTE_SOURCE_STRIDE, SHUB_SLOT_BTE_SRC },
    { K_STORE, II_REG(SHUB_II_BTE_DESTINATION), SHUB_II_BTE_DESTINATION_COUNT,
      SHUB_II_BTE_DESTINATION_STRIDE, SHUB_SLOT_BTE_DST },
    { K_BTE_CTL, II_REG(SHUB_II_BTE_CONTROL), SHUB_II_BTE_CONTROL_COUNT,
      SHUB_II_BTE_CONTROL_STRIDE, SHUB_SLOT_BTE_CTL },
    { K_STORE, II_REG(SHUB_II_BTE_NOTIFY), SHUB_II_BTE_NOTIFY_COUNT,
      SHUB_II_BTE_NOTIFY_STRIDE, SHUB_SLOT_BTE_NOTIFY },
    { K_STORE, II_REG(SHUB_II_BTE_INTERRUPT), SHUB_II_BTE_INTERRUPT_COUNT,
      SHUB_II_BTE_INTERRUPT_STRIDE, SHUB_SLOT_BTE_INT },
    { K_SLICE_REC, SHUB_JUNK_BUS_LED0, SHUB_JUNK_BUS_LED0_COUNT,
      SHUB_JUNK_BUS_LED0_STRIDE, 0 },
    { K_SLICE_PRESENT, SHUB_FSB_SYMMETRIC_AGENT_STATUS, 1, 0, 0 },
    { K_DIR_RESULT, SHUB_MD_DIR_RESULT, 2, SHUB_MD_DIR_RESULT_STRIDE, 0 },
    { K_DIR_RESULT, SHUB_MD_DIR_RESULT_HIGH, 2, SHUB_MD_DIR_RESULT_STRIDE, 0 },
    { K_LOCK, SHUB_LB_LOCK, 1, 0, 0 },
    { K_LOCK_STATUS, SHUB_LB_LOCK_STATUS, 1, 0, 0 },
    { K_LOCK2, SHUB_LB_LOCK2, 1, 0, 0 },
    { K_LOCK2_STATUS, SHUB_LB_LOCK2_STATUS, 1, 0, 0 },
};

#define MBIST_GO (1ULL << 40)
#define MBIST_FAIL (1ULL << 41)
#define MBIST_OP(v) (((v) >> 32) & 0x7f)

/* Part number the PROM's method table needs; a wrong one calls NULL. */
#define SHUB_MD_REG_0000_0000_PART 0x4363ULL

#define SHUB_ID_WRITABLE \
    ((SHUB_SHUB_ID_NASID_MASK << SHUB_SHUB_ID_NASID_SHIFT) | \
     (SHUB_SHUB_ID_SIZE_CODE_MASK << SHUB_SHUB_ID_SIZE_CODE_SHIFT) | \
     (SHUB_SHUB_ID_NODES_PER_BIT_MASK << SHUB_SHUB_ID_NODES_PER_BIT_SHIFT))

#define SHUB_RTC_CTL_ENABLE 1ULL
#define SHUB_RTC_CTL_RESET 0x100000000ULL

#define SHUB_RTC_PERIOD_FIELD(ctl) \
    (((ctl) >> SHUB_RTC_CTL_PERIOD_SHIFT) & SHUB_RTC_CTL_PERIOD_MASK)

typedef struct ShubDecoded {
    const ShubRegSpec *spec;
    unsigned index;
} ShubDecoded;

/* Offset to register and index, built once; the first matching spec wins. */
static GHashTable *shub_decode_table;

static void shub_decode_table_build(void)
{
    shub_decode_table = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              g_free, g_free);
    for (unsigned i = 0; i < ARRAY_SIZE(shub_regs); i++) {
        const ShubRegSpec *spec = &shub_regs[i];

        for (unsigned n = 0; n < spec->count; n++) {
            int64_t k = spec->base + n * spec->stride;
            int64_t *key;
            ShubDecoded *d;

            if (g_hash_table_contains(shub_decode_table, &k)) {
                continue;
            }
            key = g_new(int64_t, 1);
            d = g_new(ShubDecoded, 1);
            *key = k;
            *d = (ShubDecoded){ spec, n };
            g_hash_table_insert(shub_decode_table, key, d);
        }
    }
}

static const ShubRegSpec *shub_decode(hwaddr offset, unsigned *index)
{
    int64_t key;
    const ShubDecoded *d;

    /* The local window reaches the II block 0x800000 lower than the global. */
    if (offset >= 0x1400000 && offset < 0x1440000) {
        offset += 0x800000;
    }
    key = offset;
    d = g_hash_table_lookup(shub_decode_table, &key);
    if (!d) {
        return NULL;
    }
    *index = d->index;
    return d->spec;
}

/*
 * An access the model does not implement: log it and fail the transaction.
 * @r is MEMTX_DECODE_ERROR when no register is there, else MEMTX_ERROR.
 */
static MemTxResult shub_refuse(hwaddr offset, unsigned size, bool write,
                               uint64_t value, const char *why, MemTxResult r)
{
    qemu_log_mask(LOG_UNIMP, "sgi-shub: unsupported %s at 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 ": %s\n",
                  write ? "write" : "read", offset, size, value, why);
    return r;
}

static MemTxResult shub_unmodelled(hwaddr offset, unsigned size, bool write,
                                   uint64_t value, const char *why)
{
    return shub_refuse(offset, size, write, value, why, MEMTX_ERROR);
}

static unsigned shub_rtc_period_ns(SGIShubState *s)
{
    return 10 * (SHUB_RTC_PERIOD_FIELD(s->slot[SHUB_SLOT_RTC_CTL].value) + 1);
}

/* The counter runs only while the control word's enable bit is set. */
static bool shub_rtc_running(SGIShubState *s)
{
    return s->slot[SHUB_SLOT_RTC_CTL].value & SHUB_RTC_CTL_ENABLE;
}

/* The counter at the current rate. */
static uint64_t shub_rtc_count(SGIShubState *s)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->rtc_base_ns;

    if (!shub_rtc_running(s)) {
        return s->rtc_base_count & SHUB_RTC_MASK;
    }
    return (s->rtc_base_count + ns / shub_rtc_period_ns(s)) & SHUB_RTC_MASK;
}

/* A processor's slice: its id's low bit picks the pair, its eid the member. */
static unsigned shub_slice_of(CPUState *cs)
{
    uint16_t lid = ia64_cpu_sapic_id(cs);

    return ((lid >> 8) & 1) << 1 | (lid & 1);
}

/* The slice that issued the access in progress; a test access is slice 0. */
static unsigned shub_issuing_slice(void)
{
    return current_cpu ? shub_slice_of(current_cpu) : 0;
}

/* The processor a pid names; with one processor every pid names it (approx). */
static CPUState *shub_cpu_for_pid(uint16_t pid)
{
    CPUState *cs, *only = NULL;
    unsigned count = 0;

    CPU_FOREACH(cs) {
        if (ia64_cpu_sapic_id(cs) == pid) {
            return cs;
        }
        only = cs;
        count++;
    }
    return count == 1 ? only : NULL;
}

/* Raise the message's vector on the processor its pid names. */
static MemTxResult shub_deliver(hwaddr offset, uint64_t msg)
{
    unsigned type = (msg >> SHUB_IPI_INT_TYPE_SHIFT) & SHUB_IPI_INT_TYPE_MASK;
    unsigned vector = (msg >> SHUB_IPI_INT_IDX_SHIFT) & SHUB_IPI_INT_IDX_MASK;
    CPUState *cs = shub_cpu_for_pid((msg >> SHUB_IPI_INT_PID_SHIFT) &
                                    SHUB_IPI_INT_PID_MASK);

    if (type != 0) {
        return shub_unmodelled(offset, 8, true, msg,
                               "only the interrupt delivery type is modelled");
    }
    if (vector != 0 && vector != 2 && vector < 16) {
        return shub_unmodelled(offset, 8, true, msg, "reserved vector");
    }
    if (!cs) {
        return shub_unmodelled(offset, 8, true, msg,
                               "no processor with that id");
    }
    ia64_sapic_set_irq(cs, vector);
    return MEMTX_OK;
}

/*
 * Schedule comparator n for the tick the counter reaches its value; one
 * already reached fires at once only when it is (re)programmed.
 */
static void shub_comparator_arm(SGIShubState *s, unsigned n, bool if_reached)
{
    uint64_t period = shub_rtc_period_ns(s);
    uint64_t count, delta;
    int64_t ticks;

    timer_del(s->comparator[n]);
    if (!s->rtc_int_enable[n] || !shub_rtc_running(s)) {
        return;
    }
    count = shub_rtc_count(s);
    if (!if_reached && s->int_cmp[n] <= count) {
        return;
    }
    delta = s->int_cmp[n] > count ? s->int_cmp[n] - count : 0;
    /* A compare that far ahead, like the driver's all-ones, never fires. */
    if (delta > (uint64_t)INT64_MAX / 4 / period) {
        return;
    }
    ticks = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->rtc_base_ns) / period;
    timer_mod(s->comparator[n],
              s->rtc_base_ns + (ticks + (int64_t)delta) * (int64_t)period);
}

/* The counter moved or the state was loaded: only matches still ahead. */
static void shub_comparator_rearm_all(SGIShubState *s)
{
    for (unsigned n = 0; n < 4; n++) {
        shub_comparator_arm(s, n, false);
    }
}

static void shub_comparator_fire(void *opaque)
{
    struct SGIShubTimerArg *arg = opaque;
    SGIShubState *s = arg->shub;
    unsigned n = arg->index;

    /* Comparator n sets event bit 23 + n; 23 is the firmware's. */
    s->event |= 1ULL << (23 + n);
    shub_deliver(SHUB_RTC0_INT_CONFIG + n * SHUB_RTC_INT_CONFIG_STRIDE,
                 s->rtc_int_config[n]);
}

/* Latch event bit 20; only a latching from clear sends a message. */
static void shub_uart_latch(SGIShubState *s)
{
    bool latched = s->event & SHUB_EVENT_OCCURRED_UART_INT_INPLACE;

    s->event |= SHUB_EVENT_OCCURRED_UART_INT_INPLACE;
    if (!latched && (s->slot[SHUB_SLOT_LOCAL_EN + 5].value &
                     SHUB_UART_INT_ENABLE)) {
        shub_deliver(SHUB_LOCAL_INT_CONFIG + 5 * SHUB_LOCAL_INT_CONFIG_STRIDE,
                     s->slot[SHUB_SLOT_LOCAL_CFG + 5].value);
    }
}

/* A rising edge of the line latches event bit 20. */
static void shub_uart_irq(void *opaque, int n, int level)
{
    SGIShubState *s = opaque;
    bool was = s->uart_line;

    s->uart_line = level;
    if (level && !was) {
        shub_uart_latch(s);
    }
}

static void shub_global_remap(SGIShubState *s)
{
    uint64_t nasid = (s->shub_id >> SHUB_SHUB_ID_NASID_SHIFT) &
                     SHUB_SHUB_ID_NASID_MASK;

    memory_region_set_address(&s->global_mmr, SGI_SHUB_GLOBAL_MMR_BASE +
                              (nasid << SGI_SHUB_NASID_SHIFT));
}

/* Error, mask and alias words the PROM writes at set-up: stored, reset 0. */
static const hwaddr shub_blind_cells[] = {
    /* Link-buffer words the PROM sets by read-modify-write. */
    0x110040000, 0x110040100, 0x110040180, 0x110040300,
    /* The memory set-up's presence, local, privilege and present-SHub words. */
    0x100030100, 0x100030110, 0x100030120, 0x100030130, 0x100030210,
    0x100030220, 0x100030230, 0x100030240, 0x100030250, 0x100030260,
    0x100030270, 0x100030310, 0x100030320, 0x100030330, 0x100030340,
    0x100030350, 0x100030360, 0x100030370, 0x100050100, 0x100050110,
    0x100050120, 0x100050130, 0x100050210, 0x100050220, 0x100050230,
    0x100050240, 0x100050250, 0x100050260, 0x100050270, 0x100050310,
    0x100050320, 0x100050330, 0x100050340, 0x100050350, 0x100050360,
    0x100050370, 0x110060600, 0x110060680, 0x110060700, 0x110060780,
    /* The CPU and I/O permission words the PROM writes after the credits. */
    0x110060000, 0x110060100, 0x110060180, 0x110060380, 0x110060400,
    0x1000000c0, 0x1000000d0, 0x1000000d8, 0x100033000, 0x100033020,
    0x100033040, 0x100033800, 0x100033820, 0x100033840, 0x10004a000,
    0x100053000, 0x100053020, 0x100053040, 0x100053800, 0x100053820,
    0x100053840, 0x10006a000, 0x110010008, 0x110010088, 0x110050200,
    0x110050280, 0x110050300, 0x110050380, 0x110050400, 0x110050480,
    0x110050508, 0x110050588, 0x110050600, 0x110071000, 0x110071100,
    0x110071180, 0x110071200, 0x110071280, 0x110071300, 0x110071380,
    0x110071400, 0x110071880, 0x110071900, 0x110071980, 0x120050080,
    0x120050500, 0x120050a80, 0x120060080, 0x120060480, 0x120060500,
    0x120060608, 0x120060688, 0x120060788, 0x120060580,
    0x110030200, 0x150000000, 0x150002000, 0x150000048,
    0x150002048,
    /* Processor and profile interrupt pairs the NASID change walks. */
    0x110000c80, 0x110000d00, 0x110000d80, 0x110000e00, 0x110001080,
    0x110001100, 0x110001180, 0x110001200, 0x110000a80, 0x110000b00,
    0x110000b80, 0x110000c00, 0x110000e80, 0x110000f00, 0x110000f80,
    0x110001000, 0x110001280, 0x110001300,
    /* Network port link set-up: reset, mode, config, test control, flow. */
    0x150000008, 0x150002008, 0x150000018, 0x150002018, 0x150000020,
    0x150002020, 0x150000028, 0x150002028, 0x1500303c0, 0x1500303d0,
    0x1500303f0, 0x1500305c0, 0x1500305d0, 0x1500305f0, 0x150040008,
    0x150040028, 0x150040040, 0x150040060, 0x150040070, 0x150040080,
    0x150040090,
    0x1500400a0, 0x1500400b0, 0x1500400c0, 0x1500400d0, 0x1500400e0,
    0x1500400f0, 0x150040100, 0x150040110, 0x150040120, 0x150040130,
    0x150040140, 0x150040150, 0x150040208, 0x150040228, 0x150040240,
    0x150040260, 0x150040308, 0x150040328, 0x150040340, 0x150040360,
    0x150040408, 0x150040428, 0x150040440, 0x150040460, 0x150040508,
    0x150040518, 0x150040528, 0x150040538, 0x150040540, 0x150040550,
    0x150040560, 0x150040570, 0x150040580, 0x150040590, 0x150040608,
    0x150040618, 0x150040628, 0x150040638, 0x150040640, 0x150040650,
    0x150040660, 0x150040670, 0x150040680, 0x150040690,
};
QEMU_BUILD_BUG_ON(ARRAY_SIZE(shub_blind_cells) != SGI_SHUB_BLIND_CELLS);

/* Offset to blind cell index plus one, built once beside the decode table. */
static GHashTable *shub_blind_table;

static int shub_blind_find(hwaddr offset)
{
    int64_t key = offset;

    return GPOINTER_TO_INT(g_hash_table_lookup(shub_blind_table, &key)) - 1;
}

static void shub_blind_table_build(void)
{
    shub_blind_table = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, NULL);
    for (unsigned i = 0; i < ARRAY_SIZE(shub_blind_cells); i++) {
        int64_t *key = g_new(int64_t, 1);

        *key = shub_blind_cells[i];
        g_hash_table_insert(shub_blind_table, key, GINT_TO_POINTER(i + 1));
    }
}

/* Reset values from the 2.4 header: 0x1f counts, all-ones access words. */
static void shub_blind_reset(SGIShubState *s)
{
    memset(s->blind_cell, 0, sizeof(s->blind_cell));
    s->blind_cell[shub_blind_find(0x110040100)] = 0x1f;
    s->blind_cell[shub_blind_find(0x110040180)] = 0x1f;
    s->blind_cell[shub_blind_find(0x110060000)] = UINT64_MAX;
    s->blind_cell[shub_blind_find(0x110060180)] = UINT64_MAX;
    s->blind_cell[shub_blind_find(0x110060400)] = UINT64_MAX;
    /* The processor interface's error masks start with every valid bit set. */
    s->blind_cell[shub_blind_find(0x120050080)] = 0x7ffe00000ULL;
    s->blind_cell[shub_blind_find(0x120050500)] = 0x1fffffULL;
    s->blind_cell[shub_blind_find(0x120050a80)] = 0x7ffffffffULL;
    s->blind_cell[shub_blind_find(0x150000020)] = 0x7fc6ffd00;
    s->blind_cell[shub_blind_find(0x150002020)] = 0x7fc6ffd00;
}

static MemTxResult shub_blind(SGIShubState *s, hwaddr offset, unsigned size,
                              bool write, uint64_t value, uint64_t *data,
                              int cell)
{
    if (size != 8) {
        return shub_unmodelled(offset, size, write, value,
                               "register is 64 bits wide");
    }
    if (write) {
        s->blind_cell[cell] = value;
    } else {
        *data = s->blind_cell[cell];
    }
    return MEMTX_OK;
}

static MemTxResult shub_read(void *opaque, hwaddr offset, uint64_t *data,
                             unsigned size, MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    const ShubRegSpec *spec;
    CPUState *cs;
    unsigned n;

    *data = 0;
    spec = shub_decode(offset, &n);
    if (!spec && shub_blind_find(offset) >= 0) {
        return shub_blind(s, offset, size, false, 0, data,
                          shub_blind_find(offset));
    }
    if (!spec) {
        return shub_refuse(offset, size, false, 0, "register not modelled",
                           MEMTX_DECODE_ERROR);
    }
    if (spec->kind == K_LED) {
        return shub_unmodelled(offset, size, false, 0,
                               "register is not readable");
    }
    if (size != 8) {
        return shub_unmodelled(offset, size, false, 0,
                               "register is 64 bits wide");
    }

    switch (spec->kind) {
    case K_STORE:
    case K_CRB_MSG:
    case K_RTC_CTL:
    case K_MBIST_CTL:
    case K_BTE_LEN:
    case K_BTE_CTL: {
        SGIShubSlot *slot = &s->slot[spec->slot + n];

        if (!slot->valid) {
            return shub_unmodelled(offset, size, false, 0,
                                   "reset value unknown and not yet written");
        }
        *data = slot->value;
        return MEMTX_OK;
    }
    case K_RTC_INT_CONFIG:
        *data = s->rtc_int_config[n];
        return MEMTX_OK;
    case K_RTC_INT_ENABLE:
        *data = s->rtc_int_enable[n];
        return MEMTX_OK;
    case K_INT_CMP:
        *data = s->int_cmp[n];
        return MEMTX_OK;
    case K_SHUB_ID:
        *data = s->shub_id;
        return MEMTX_OK;
    case K_LLP_CSR:
        /* Bit 13 (llp_stat bit 1): the PROM reads an I/O brick attached. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: link status bit 13 is an approximation\n");
        *data = s->slot[SHUB_SLOT_LLP_CSR].value | (1ULL << 13);
        return MEMTX_OK;
    case K_CLK_SEL:
        /* Bits 1:0 pick the bus clock; 0 is the simplest supported one. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: the bus clock select is an approximation\n");
        *data = 0;
        return MEMTX_OK;
    case K_RTC:
        *data = shub_rtc_count(s);
        return MEMTX_OK;
    case K_PIO_WRITE_STATUS:
        /* PIO writes complete synchronously, so the count is always idle. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: PIO write status is always idle\n");
        *data = SHUB_PIO_WRITE_STATUS_IDLE |
                s->slot[SHUB_SLOT_PIO_ERR + n].value;
        return MEMTX_OK;
    case K_CHIP_ID:
        *data = (SHUB_MD_REG_0000_0000_PART <<
                 SHUB_MD_REG_0000_0000_PART_NUMBER_SHIFT) |
                (((s->shub_id >> SHUB_SHUB_ID_REVISION_SHIFT) &
                  SHUB_SHUB_ID_REVISION_MASK) <<
                 SHUB_MD_REG_0000_0000_REVISION_SHIFT);
        return MEMTX_OK;
    case K_SLICE_REC:
        /* Any value satisfies the PROM; it only becomes a progress code. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: the slice record is an approximation\n");
        *data = 0;
        return MEMTX_OK;
    case K_PI_CREDITS:
        /* Eight credits: the PROM then skips its block-transfer top-up. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: the credit count is an approximation\n");
        *data = 8;
        return MEMTX_OK;
    case K_DIR_RESULT:
        /* Op 80 would load a directory entry here; the PROM never tests it. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: the directory result reads 0\n");
        *data = 0;
        return MEMTX_OK;
    case K_SLICE_PRESENT:
        /* One bit per front-side bus that has a processor: slice / 2. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: slice presence follows the CPU ids\n");
        *data = 0;
        CPU_FOREACH(cs) {
            *data |= 1ULL << (shub_slice_of(cs) / 2);
        }
        return MEMTX_OK;
    case K_LOCK_STATUS:
    case K_LOCK2_STATUS: {
        /* A read takes the lock: it returns the bit and sets it (inferred). */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-shub: a lock read takes the lock (approximation)\n");
        uint64_t *lock = &s->slot[SHUB_SLOT_LOCK +
                                  (spec->kind == K_LOCK2_STATUS)].value;

        *data = *lock;
        *lock = 1;
        return MEMTX_OK;
    }
    case K_EVENT:
        *data = s->event;
        return MEMTX_OK;
    default:
        return shub_unmodelled(offset, size, false, 0,
                               "register is not readable");
    }
}

/* Translate a block-transfer address in this node's memory. */
static MemTxResult shub_bte_phys(SGIShubState *s, hwaddr offset, uint64_t addr,
                                 uint64_t *phys)
{
    uint64_t nasid = (s->shub_id >> SHUB_SHUB_ID_NASID_SHIFT) &
                     SHUB_SHUB_ID_NASID_MASK;

    if ((addr >> SGI_SHUB_NASID_SHIFT) != nasid || (addr & (3ULL << 36))) {
        return shub_unmodelled(offset, 8, true, 0,
                               "copy address outside this node's memory");
    }
    *phys = SGI_SHUB_CACHEABLE_BASE | (addr & ((1ULL << 36) - 1));
    return MEMTX_OK;
}

/*
 * A plain copy or zero fill runs to completion inside the control write, in
 * 4 KiB chunks under the BQL; the length field bounds it to under 8 MiB.
 */
static MemTxResult shub_bte_run(SGIShubState *s, hwaddr offset, unsigned n,
                                bool fill)
{
    uint64_t len = (s->slot[SHUB_SLOT_BTE_LEN + n].value &
                    SHUB_II_BTE_LENGTH_STATUS_LENGTH_MASK) << 7;
    uint8_t buf[4096] = { 0 };
    uint64_t src = 0, dst;
    MemTxResult r;

    if (!s->slot[SHUB_SLOT_BTE_DST + n].valid ||
        (!fill && !s->slot[SHUB_SLOT_BTE_SRC + n].valid)) {
        return shub_unmodelled(offset, 8, true, 0,
                               "transfer before its addresses are written");
    }
    r = shub_bte_phys(s, offset, s->slot[SHUB_SLOT_BTE_DST + n].value, &dst);
    if (r == MEMTX_OK && !fill) {
        r = shub_bte_phys(s, offset, s->slot[SHUB_SLOT_BTE_SRC + n].value,
                          &src);
    }
    if (r != MEMTX_OK) {
        return r;
    }
    for (uint64_t done = 0; done < len; done += sizeof(buf)) {
        uint64_t chunk = MIN(len - done, sizeof(buf));

        if ((!fill && address_space_read(&s->dma_as, src + done,
                                         MEMTXATTRS_UNSPECIFIED, buf, chunk)) ||
            address_space_write(&s->dma_as, dst + done,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk)) {
            return shub_unmodelled(offset, 8, true, 0,
                                   "transfer reached no memory");
        }
    }
    return MEMTX_OK;
}

/*
 * With the notify attribute the engine ends by writing the final status word
 * to the notification address: length 0, no busy, no error, so 0.
 */
static MemTxResult shub_bte_notify(SGIShubState *s, hwaddr offset, unsigned n)
{
    uint64_t phys, status = 0;
    MemTxResult r;

    if (!s->slot[SHUB_SLOT_BTE_NOTIFY + n].valid) {
        return shub_unmodelled(offset, 8, true, 0,
                               "notify before its address is written");
    }
    r = shub_bte_phys(s, offset, s->slot[SHUB_SLOT_BTE_NOTIFY + n].value,
                      &phys);
    if (r != MEMTX_OK) {
        return r;
    }
    if (address_space_write(&s->dma_as, phys, MEMTXATTRS_UNSPECIFIED, &status,
                            sizeof(status))) {
        return shub_unmodelled(offset, 8, true, 0,
                               "notification reached no memory");
    }
    return MEMTX_OK;
}

static MemTxResult shub_bte_copy(SGIShubState *s, hwaddr offset, unsigned n)
{
    MemTxResult r = shub_bte_run(s, offset, n, false);

    if (r == MEMTX_OK) {
        r = shub_bte_notify(s, offset, n);
    }
    if (r != MEMTX_OK) {
        return r;
    }
    s->slot[SHUB_SLOT_BTE_CTL + n].value = SHUB_II_BTE_CONTROL_NOTIFY_INPLACE;
    s->slot[SHUB_SLOT_BTE_CTL + n].valid = true;
    s->slot[SHUB_SLOT_BTE_LEN + n].value = 0;
    return MEMTX_OK;
}

static MemTxResult shub_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    const ShubRegSpec *spec;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned n;

    spec = shub_decode(offset, &n);
    if (!spec && shub_blind_find(offset) >= 0) {
        return shub_blind(s, offset, size, true, value, NULL,
                          shub_blind_find(offset));
    }
    if (!spec) {
        return shub_refuse(offset, size, true, value, "register not modelled",
                           MEMTX_DECODE_ERROR);
    }
    if (size != (spec->kind == K_LED ? 2 : 8)) {
        return shub_unmodelled(offset, size, true, value,
                               "wrong access width");
    }

    switch (spec->kind) {
    case K_LLP_CSR:
        s->slot[SHUB_SLOT_LLP_CSR].value = value;
        return MEMTX_OK;
    case K_CRB_MSG:
        /* The quiesce store means PAL reported a fault: report it. */
        if (value == SHUB_CRB_QUIESCE) {
            return shub_unmodelled(offset, size, true, value,
                                   "firmware fatal-error exit: PAL reported a fault");
        }
        s->slot[SHUB_SLOT_CRB_MSG].value = value;
        return MEMTX_OK;
    case K_II_INT: {
        CPUState *cs;

        /* Data is 0x100 | vector: the send bit and the vector. Inferred. */
        if (!(value & 0x100) || (value & ~0x1ffULL)) {
            return shub_unmodelled(offset, size, true, value,
                                   "interrupt packet layout not known");
        }
        cs = shub_cpu_for_pid((s->slot[SHUB_SLOT_II_INT_CFG + n].value >>
                               SHUB_II_INT_CONFIG_PID_SHIFT) &
                              SHUB_II_INT_CONFIG_PID_MASK);
        if (!cs || ((value & 0xff) != 0 && (value & 0xff) != 2 &&
                    (value & 0xff) < 16)) {
            return shub_unmodelled(offset, size, true, value,
                                   "no processor or reserved vector");
        }
        ia64_sapic_set_irq(cs, value & 0xff);
        return MEMTX_OK;
    }
    case K_ELECT_SET:
        /* The alias takes a value only while SH_SCRATCH1 reads 0. */
        if (s->slot[spec->slot + n].value) {
            return MEMTX_OK;
        }
        /* fall through */
    case K_STORE:
        s->slot[spec->slot + n].value = value;
        s->slot[spec->slot + n].valid = true;
        return MEMTX_OK;
    case K_MBIST_CTL: {
        uint64_t op = MBIST_OP(value);

        /* Only the operations the PROM issues; the rest have no contract. */
        if ((value & MBIST_GO) && op != 0 && op != 1 && op != 2 && op != 7 &&
            op != 16 && op != 48 && op != 64 && op != 80) {
            return shub_unmodelled(offset, size, true, value,
                                   "engine operation not in the PROM's list");
        }
        /*
         * Every run completes at once and passes: go and fail read clear.
         * No DRAM is modelled, so a compare cannot fail.
         */
        s->slot[spec->slot].value = value & ~(MBIST_GO | MBIST_FAIL);
        s->slot[spec->slot].valid = true;
        return MEMTX_OK;
    }
    case K_BTE_LEN:
        /* A write loads the length and sets busy; a zero length stays idle. */
        s->slot[spec->slot + n].value =
            value & SHUB_II_BTE_LENGTH_STATUS_LENGTH_MASK;
        if (s->slot[spec->slot + n].value) {
            s->slot[spec->slot + n].value |=
                SHUB_II_BTE_LENGTH_STATUS_BUSY_MASK <<
                SHUB_II_BTE_LENGTH_STATUS_BUSY_SHIFT;
        }
        s->slot[spec->slot + n].valid = true;
        return MEMTX_OK;
    case K_BTE_CTL: {
        const uint64_t zerofill = SHUB_II_BTE_CONTROL_ZEROFILL_INPLACE;
        const uint64_t notify = SHUB_II_BTE_CONTROL_NOTIFY_INPLACE;

        if (value == notify) {
            return shub_bte_copy(s, offset, n);
        }
        if ((value & ~(zerofill | notify)) || !(value & zerofill)) {
            return shub_unmodelled(offset, size, true, value,
                                   "block transfer mode not modelled");
        }
        /* The PROM may fill before it has set a destination: store only. */
        if (s->slot[SHUB_SLOT_BTE_DST + n].valid) {
            MemTxResult r = shub_bte_run(s, offset, n, true);

            if (r == MEMTX_OK && (value & notify)) {
                r = shub_bte_notify(s, offset, n);
            }
            if (r != MEMTX_OK) {
                return r;
            }
        }
        s->slot[spec->slot + n].value = value;
        s->slot[spec->slot + n].valid = true;
        /* The transfer finishes at once: a fill has written its zeros. */
        s->slot[SHUB_SLOT_BTE_LEN + n].value = 0;
        return MEMTX_OK;
    }
    case K_RTC_CTL: {
        SGIShubSlot *ctl = &s->slot[SHUB_SLOT_RTC_CTL];

        /* Keep the count continuous across a period or enable change. */
        s->rtc_base_count = shub_rtc_count(s);
        ctl->value = value;
        ctl->valid = true;
        s->rtc_base_ns = now;
        shub_comparator_rearm_all(s);
        return MEMTX_OK;
    }
    case K_RTC:
        if (value & ~SHUB_RTC_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits above the 55-bit counter");
        }
        s->rtc_base_count = value;
        s->rtc_base_ns = now;
        shub_comparator_rearm_all(s);
        return MEMTX_OK;
    case K_SHUB_ID:
        /* The PROM rewrites only the NASID, size code and nodes-per-bit. */
        if ((value ^ s->shub_id) & ~SHUB_ID_WRITABLE) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the NASID, size and node fields");
        }
        s->shub_id = value;
        shub_global_remap(s);
        return MEMTX_OK;
    case K_RTC_INT_CONFIG:
        if (value & ~SHUB_RTC_CONFIG_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the defined fields");
        }
        s->rtc_int_config[n] = value;
        return MEMTX_OK;
    case K_RTC_INT_ENABLE:
        if (value & ~(uint64_t)SHUB_RTC_ENABLE_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the enable field");
        }
        s->rtc_int_enable[n] = value;
        shub_comparator_arm(s, n, true);
        return MEMTX_OK;
    case K_INT_CMP:
        if (value & ~SHUB_INT_CMP_COMPARE_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the compare field");
        }
        s->int_cmp[n] = value;
        shub_comparator_arm(s, n, true);
        return MEMTX_OK;
    case K_PI_CAM_CONTROL:
        /* Only the value Linux writes is known; its effect is not modelled. */
        if (value != SHUB_PI_CAM_WAR_VALUE) {
            return shub_unmodelled(offset, size, true, value,
                                   "value not known");
        }
        qemu_log_mask(LOG_UNIMP, "sgi-shub: pi_cam_control write ignored: "
                      "CRB CAM not modelled\n");
        return MEMTX_OK;
    case K_PIO_WRITE_STATUS_ALIAS:
        /* Write-one-to-clear of the error bits and the address (inferred). */
        if (value != SHUB_PIO_WRITE_STATUS_ALIAS_CLEAR &&
            value != SHUB_PIO_ALIAS_CLEAR_ALL) {
            return shub_unmodelled(offset, size, true, value,
                                   "value not known");
        }
        s->slot[SHUB_SLOT_PIO_ERR + n].value &= ~value;
        return MEMTX_OK;
    case K_PIO_ERR_HOLE: {
        uint64_t *err = &s->slot[SHUB_SLOT_PIO_ERR +
                                 (shub_issuing_slice() >> 1)].value;

        /* No register here: the PROM's self-test expects the write to fail. */
        qemu_log_mask(LOG_UNIMP, "sgi-shub: store to the unnamed register at "
                      "0x%" HWADDR_PRIx " raises a PIO write error\n", offset);
        if (*err & SHUB_PIO_ERR_WRITE) {
            *err |= SHUB_PIO_ERR_MULTI;
            return MEMTX_OK;
        }
        /* The first error latches its address and command (a write, 0xea). */
        *err = SHUB_PIO_ERR_WRITE | (offset & SHUB_PIO_ERR_ADDR_MASK);
        s->blind_cell[shub_blind_find(SHUB_LB_ERROR_DETAIL_1)] =
            SHUB_LB_ERROR_DETAIL_VALID | SHUB_LB_ERROR_CMD_WRITE;
        s->blind_cell[shub_blind_find(SHUB_LB_ERROR_DETAIL_2)] = offset >> 3;
        s->blind_cell[shub_blind_find(SHUB_PI_ERROR_DETAIL_1)] =
            offset & SHUB_PI_ERROR_ADDR_MASK;
        return MEMTX_OK;
    }
    case K_LED:
        s->led[n] = value;
        return MEMTX_OK;
    case K_WRITE_ONLY:
        /* The PROM never reads these; the effect of the write is unknown. */
        qemu_log_mask(LOG_UNIMP, "sgi-shub: write at 0x%" HWADDR_PRIx
                      " ignored: effect unknown\n", offset);
        return MEMTX_OK;
    case K_LOCK:
    case K_LOCK2:
        /* Only the release is known: a write of 0 frees the lock. */
        if (value) {
            return shub_unmodelled(offset, size, true, value,
                                   "value not known");
        }
        s->slot[SHUB_SLOT_LOCK + (spec->kind == K_LOCK2)].value = 0;
        return MEMTX_OK;
    case K_IPI_INT:
        /* A message without the send bit has no documented effect. */
        if (!(value & SHUB_IPI_INT_SEND_INPLACE)) {
            return shub_unmodelled(offset, size, true, value,
                                   "send bit clear");
        }
        s->event |= SHUB_EVENT_OCCURRED_IPI_INT_INPLACE;
        return shub_deliver(offset, value);
    case K_PTC:
        return shub_unmodelled(offset, size, true, value,
                               "purge translation cache effect unknown");
    case K_EVENT:
        /* The PROM sets event bits by writing them; a one ORs in. */
        s->event |= value;
        return MEMTX_OK;
    case K_EVENT_ALIAS:
        /* A one clears a pending bit, bit 20 included while the line is up. */
        s->event &= ~value;
        return MEMTX_OK;
    case K_EVENT_OVERFLOW_ALIAS:
        /* The PROM writes 8 here beside the event alias; no source sets it. */
        s->slot[SHUB_SLOT_EVENT_OVF].value &= ~value;
        return MEMTX_OK;
    default:
        return shub_unmodelled(offset, size, true, value,
                               "register is not writable");
    }
}

static const MemoryRegionOps shub_ops = {
    .read_with_attrs = shub_read,
    .write_with_attrs = shub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void shub_reset_hold(Object *obj, ResetType type)
{
    SGIShubState *s = SGI_SHUB(obj);

    s->shub_id = s->shub_id_reset;
    shub_global_remap(s);
    s->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rtc_base_count = 0;
    s->uart_line = false;
    memset(s->slot, 0, sizeof(s->slot));
    shub_blind_reset(s);
    /* The board's node identity word, if it gave one. */
    s->slot[SHUB_SLOT_L1_STATE].value = s->scratch2_reset;
    s->slot[SHUB_SLOT_L1_STATE].valid = s->scratch2_reset != 0;
    /* The header's initial value: internal source, counter disabled. */
    s->slot[SHUB_SLOT_RTC_CTL].value = SHUB_RTC_CTL_RESET;
    s->slot[SHUB_SLOT_RTC_CTL].valid = true;
    /*
     * Registers the PROM reads before it writes and accepts any value from:
     * their real reset value is unknown, and zero is an approximation.
     */
    s->slot[SHUB_SLOT_CRB_MSG].value = SHUB_CRB_MESSAGE_CONTROL_RESET;
    for (int i = SHUB_SLOT_HIST; i < SHUB_SLOT_COUNT; i++) {
        s->slot[i].valid = true;
    }
    /* A transfer needs its addresses written first, so the guards can refuse. */
    for (unsigned n = 0; n < 2; n++) {
        s->slot[SHUB_SLOT_BTE_SRC + n].valid = false;
        s->slot[SHUB_SLOT_BTE_DST + n].valid = false;
        s->slot[SHUB_SLOT_BTE_NOTIFY + n].valid = false;
    }
    /* The PROM reads these first: the 2.4 header's reset values (inferred). */
    s->slot[SHUB_SLOT_CRB_NACK].valid = true;
    s->slot[SHUB_SLOT_CRB_PRESCALE].valid = true;
    s->slot[SHUB_SLOT_CRB_SKID].valid = true;
    s->slot[SHUB_SLOT_CRB_SKID].value = 7;
    s->slot[SHUB_SLOT_PTC_PARMS].value = 0x7ffffffffULL;
    /* The five over-ride tables start with the header's values. */
    s->slot[SHUB_SLOT_TOPO + 292].value = 0x8000000000000000ULL;  /* NI0 */
    s->slot[SHUB_SLOT_TOPO + 293].value = 0x8000000000000000ULL;  /* NI1 */
    s->slot[SHUB_SLOT_TOPO + 726].value = 0x8000000000002000ULL;  /* MD */
    s->slot[SHUB_SLOT_TOPO + 727].value = 0x8000000000000000ULL;  /* IILB */
    s->slot[SHUB_SLOT_TOPO + 728].value = 0x8000000000002000ULL;  /* PI */
    s->slot[SHUB_SLOT_LLP_RESET_EN].value = 1;
    s->slot[SHUB_SLOT_LLP_RESET_EN + 1].value = 1;
    /* The kernel header's reset value for these families is zero. */
    memset(s->rtc_int_config, 0, sizeof(s->rtc_int_config));
    memset(s->rtc_int_enable, 0, sizeof(s->rtc_int_enable));
    s->event = 0;
    shub_comparator_rearm_all(s);
    memset(s->int_cmp, 0, sizeof(s->int_cmp));
    memset(s->led, 0, sizeof(s->led));
}

static void shub_realize(DeviceState *dev, Error **errp)
{
    SGIShubState *s = SGI_SHUB(dev);

    if (!s->dma_mr) {
        error_setg(errp, "sgi-shub: the dma-memory link is not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sgi-shub.dma");
    if (s->shub_id_reset & ~(SHUB_ID_WRITABLE |
        (SHUB_SHUB_ID_REVISION_MASK << SHUB_SHUB_ID_REVISION_SHIFT))) {
        error_setg(errp, "shub-id sets bits the model cannot qualify");
        return;
    }

    /* Mapping these in init would make the unrealized device own itself. */
    for (unsigned link = 0; link < 16; link++) {
        if (link == 1) {
            continue;
        }
        address_space_init(&s->widget_as[link], &s->widget_window[link],
                           "sgi-shub.widget");
        memory_region_add_subregion_overlap(&s->global_mmr,
                                            (uint64_t)link << 24,
                                            &s->widget_view[link], 1);
    }
    for (unsigned n = 1; n < 8; n++) {
        memory_region_add_subregion_overlap(&s->global_mmr,
                                            (uint64_t)n << 29,
                                            &s->big_window[n], 1);
    }
}

static const MemoryRegionOps shub_xio_ops;
static const MemoryRegionOps shub_widget_ops;
static const MemoryRegionOps shub_big_ops;

static void shub_init(Object *obj)
{
    SGIShubState *s = SGI_SHUB(obj);

    memory_region_init_io(&s->local_mmr, obj, &shub_ops, s,
                          "sgi-shub.local-mmr", SGI_SHUB_MMR_SIZE);
    memory_region_init_io(&s->global_mmr, obj, &shub_ops, s,
                          "sgi-shub.global-mmr", SGI_SHUB_MMR_SIZE);
    memory_region_init_io(&s->xio_packet, obj, &shub_xio_ops, s,
                          "sgi-shub.xio-packet", 1ULL << 48);
    for (unsigned link = 0; link < 16; link++) {
        if (link == 1) {
            continue;
        }
        memory_region_init(&s->widget_window[link], obj,
                           "sgi-shub.widget-window", SHUB_WIDGET_SPACE);
        s->widget_arg[link] = (struct SGIShubWidgetArg){ s, link };
        memory_region_init_io(&s->widget_view[link], obj, &shub_widget_ops,
                              &s->widget_arg[link], "sgi-shub.widget-view",
                              SHUB_WIDGET_WINDOW);
        /* A widget access re-enters memory through widget_as. */
        s->widget_view[link].disable_reentrancy_guard = true;
    }
    for (unsigned n = 1; n < 8; n++) {
        s->big_arg[n] = (struct SGIShubWidgetArg){ s, n };
        memory_region_init_io(&s->big_window[n], obj, &shub_big_ops,
                              &s->big_arg[n], "sgi-shub.big-window",
                              SHUB_BIG_WINDOW);
        s->big_window[n].disable_reentrancy_guard = true;
    }
    for (unsigned n = 0; n < 4; n++) {
        s->comparator_arg[n] = (struct SGIShubTimerArg){ s, n };
        s->comparator[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        shub_comparator_fire,
                                        &s->comparator_arg[n]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), shub_uart_irq, "uart-irq", 1);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->local_mmr);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->global_mmr);
}

static void shub_finalize(Object *obj)
{
    SGIShubState *s = SGI_SHUB(obj);

    for (unsigned n = 0; n < 4; n++) {
        timer_free(s->comparator[n]);
    }
    for (unsigned link = 0; link < 16; link++) {
        if (s->widget_as[link].root) {
            address_space_destroy(&s->widget_as[link]);
        }
    }
    if (s->prom_flash_as.root) {
        address_space_destroy(&s->prom_flash_as);
    }
    if (s->dma_as.root) {
        address_space_destroy(&s->dma_as);
    }
}

static const Property shub_properties[] = {
    DEFINE_PROP_UINT64("shub-id", SGIShubState, shub_id_reset,
                       SGI_SHUB_ID_DEFAULT),
    DEFINE_PROP_UINT64("scratch2-reset", SGIShubState, scratch2_reset, 0),
    DEFINE_PROP_LINK("dma-memory", SGIShubState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static int shub_post_load(void *opaque, int version_id)
{
    shub_global_remap(opaque);
    shub_comparator_rearm_all(opaque);
    return 0;
}

static const VMStateDescription vmstate_shub_slot = {
    .name = "sgi-shub/slot",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(value, SGIShubSlot),
        VMSTATE_BOOL(valid, SGIShubSlot),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription shub_vmstate = {
    .name = "sgi-shub",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = shub_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(shub_id, SGIShubState),
        VMSTATE_INT64(rtc_base_ns, SGIShubState),
        VMSTATE_UINT64(rtc_base_count, SGIShubState),
        VMSTATE_STRUCT_ARRAY(slot, SGIShubState, SHUB_SLOT_COUNT, 1,
                             vmstate_shub_slot, SGIShubSlot),
        VMSTATE_UINT64(event, SGIShubState),
        VMSTATE_BOOL(uart_line, SGIShubState),
        VMSTATE_UINT64_ARRAY(rtc_int_config, SGIShubState, 4),
        VMSTATE_UINT64_ARRAY(rtc_int_enable, SGIShubState, 4),
        VMSTATE_UINT64_ARRAY(int_cmp, SGIShubState, 4),
        VMSTATE_UINT16_ARRAY(led, SGIShubState, 4),
        VMSTATE_UINT64_ARRAY(blind_cell, SGIShubState, SGI_SHUB_BLIND_CELLS),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * A widget's packet to the SHub: 48 bits, bit 47 set for a register write at
 * (node << 36) + offset. Only the PIC's interrupt targets are modelled.
 */
/* The inbound access word has one bit for widget 0 and one for 8 to 15. */
static bool shub_inbound_widget_ok(uint64_t access, unsigned widget)
{
    if (widget == 0) {
        return access & 1;
    }
    return widget >= 8 && (access >> widget) & 1;
}

/* A PIC master's request to node RAM; a refused widget's view is unknown. */
static MemTxResult shub_inbound(SGIShubState *s, hwaddr addr, uint8_t *buf,
                                unsigned size, bool write, MemTxAttrs attrs)
{
    uint64_t nasid = (s->shub_id >> SHUB_SHUB_ID_NASID_SHIFT) &
                     SHUB_SHUB_ID_NASID_MASK;
    SGIXioSource source;
    uint64_t access = s->slot[SHUB_SLOT_II + 6].value;
    hwaddr dest = SGI_SHUB_CACHEABLE_BASE | (addr & ((1ULL << 36) - 1));
    MemTxResult res;

    if (((addr >> 36) & 0x7ff) != nasid) {
        return shub_unmodelled(addr, size, write, 0,
                               "packet for another node");
    }
    if (sgi_xio_source_from_attrs(attrs, &source) &&
        !shub_inbound_widget_ok(access, source.widget)) {
        return shub_unmodelled(addr, size, write, 0,
                               "inbound widget access is clear for the requester");
    }
    res = write ? address_space_write(&s->dma_as, dest,
                                      MEMTXATTRS_UNSPECIFIED, buf, size) :
                  address_space_read(&s->dma_as, dest,
                                     MEMTXATTRS_UNSPECIFIED, buf, size);
    if (res) {
        return shub_unmodelled(addr, size, write, 0,
                               "no memory at the address");
    }
    return MEMTX_OK;
}

static MemTxResult shub_xio_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    uint64_t nasid = (s->shub_id >> SHUB_SHUB_ID_NASID_SHIFT) &
                     SHUB_SHUB_ID_NASID_MASK;
    hwaddr offset = addr & ((1ULL << 36) - 1);

    if (!(addr & (1ULL << 47))) {
        uint8_t buf[8];

        stn_be_p(buf, size, value);
        return shub_inbound(s, addr, buf, size, true, attrs);
    }
    if (size != 8) {
        return shub_unmodelled(addr, size, true, value,
                               "packet is 64 bits wide");
    }
    if (((addr >> 36) & 0x7ff) != nasid) {
        return shub_unmodelled(addr, size, true, value,
                               "packet for another node");
    }
    if (offset != SHUB_II_INT && offset != SHUB_II_INT + SHUB_II_INT_STRIDE) {
        return shub_unmodelled(addr, size, true, value,
                               "packet is not an interrupt");
    }
    return shub_write(s, offset, value, size, attrs);
}

/*
 * Widget registers mirror a narrow access at o ^ (8 - N). PCI config and memory
 * windows are byte addressed: o ^ (4 - N), and 8 bytes move as two LE dwords.
 */
static bool shub_widget_pci_space(hwaddr addr)
{
    hwaddr head_offset = addr & 0x7fffff;

    if (addr >= 0x40000000) {
        return true;
    }
    return head_offset >= 0x20000 && head_offset < 0x29000 ? true :
           (head_offset >= 0x200000 && addr >= 0x200000);
}

static hwaddr shub_widget_lane(hwaddr addr, unsigned size)
{
    if (size <= 4 && shub_widget_pci_space(addr)) {
        return addr ^ (4 - size);
    }
    return addr ^ (8 - size);
}

static MemTxResult shub_widget_read_at(SGIShubState *s, unsigned link,
                                       hwaddr addr, uint64_t *data,
                                       unsigned size, MemTxAttrs attrs)
{
    AddressSpace *as = &s->widget_as[link];
    hwaddr mirror = shub_widget_lane(addr, size);
    MemTxResult res = MEMTX_OK;

    *data = 0;
    if (size == 8 && shub_widget_pci_space(addr) && !(addr & 7)) {
        uint64_t lo = 0, hi = 0;

        res = shub_widget_read_at(s, link, addr, &lo, 4, attrs);
        if (res == MEMTX_OK) {
            res = shub_widget_read_at(s, link, addr + 4, &hi, 4, attrs);
        }
        *data = lo | hi << 32;
        return res;
    }
    if (!is_power_of_2(size) || (addr & (size - 1))) {
        return shub_unmodelled(addr, size, false, 0,
                               "unaligned widget access");
    }
    switch (size) {
    case 1:
        *data = address_space_ldub(as, mirror, attrs, &res);
        break;
    case 2:
        *data = address_space_lduw_be(as, mirror, attrs, &res);
        break;
    case 4:
        *data = address_space_ldl_be(as, mirror, attrs, &res);
        break;
    default:
        *data = address_space_ldq_be(as, addr, attrs, &res);
        break;
    }
    return res == MEMTX_OK ? MEMTX_OK :
        shub_unmodelled(addr, size, false, 0, "no widget answers here");
}

static MemTxResult shub_widget_write_at(SGIShubState *s, unsigned link,
                                        hwaddr addr, uint64_t value,
                                        unsigned size, MemTxAttrs attrs)
{
    AddressSpace *as = &s->widget_as[link];
    hwaddr mirror = shub_widget_lane(addr, size);
    MemTxResult res = MEMTX_OK;

    if (size == 8 && shub_widget_pci_space(addr) && !(addr & 7)) {
        res = shub_widget_write_at(s, link, addr, (uint32_t)value, 4, attrs);
        if (res == MEMTX_OK) {
            res = shub_widget_write_at(s, link, addr + 4, value >> 32, 4,
                                       attrs);
        }
        return res;
    }
    if (!is_power_of_2(size) || (addr & (size - 1))) {
        return shub_unmodelled(addr, size, true, value,
                               "unaligned widget access");
    }
    switch (size) {
    case 1:
        address_space_stb(as, mirror, value, attrs, &res);
        break;
    case 2:
        address_space_stw_be(as, mirror, value, attrs, &res);
        break;
    case 4:
        address_space_stl_be(as, mirror, value, attrs, &res);
        break;
    default:
        address_space_stq_be(as, addr, value, attrs, &res);
        break;
    }
    return res == MEMTX_OK ? MEMTX_OK :
        shub_unmodelled(addr, size, true, value, "no widget answers here");
}

static MemTxResult shub_widget_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    struct SGIShubWidgetArg *arg = opaque;

    return shub_widget_read_at(arg->shub, arg->link, addr, data, size, attrs);
}

static MemTxResult shub_widget_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    struct SGIShubWidgetArg *arg = opaque;

    return shub_widget_write_at(arg->shub, arg->link, addr, value, size,
                                attrs);
}

/* Big window n uses ITTE n: widget 11:8, I/O space 12, address 4:0. */
static MemTxResult shub_big_target(SGIShubState *s, unsigned n, hwaddr addr,
                                   bool write, unsigned size, unsigned *widget,
                                   hwaddr *xtalk)
{
    uint64_t itte = s->slot[SHUB_SLOT_ITTE + n - 1].value;

    *widget = (itte >> 8) & 0xf;
    *xtalk = ((itte & 0x1f) << 29) | addr;
    if (itte & SHUB_II_ITTE_IOSP_INPLACE) {
        return shub_unmodelled(addr, size, write, 0,
                               "big window to I/O space not modelled");
    }
    if (*widget == 1 || !s->widget_as[*widget].root) {
        return shub_unmodelled(addr, size, write, 0,
                               "big window reaches no widget");
    }
    return MEMTX_OK;
}

static MemTxResult shub_big_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    struct SGIShubWidgetArg *arg = opaque;
    unsigned widget;
    hwaddr xtalk;

    *data = 0;
    if (shub_big_target(arg->shub, arg->link, addr, false, size, &widget,
                        &xtalk) != MEMTX_OK) {
        return MEMTX_ERROR;
    }
    return shub_widget_read_at(arg->shub, widget, xtalk, data, size, attrs);
}

static MemTxResult shub_big_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    struct SGIShubWidgetArg *arg = opaque;
    unsigned widget;
    hwaddr xtalk;

    if (shub_big_target(arg->shub, arg->link, addr, true, size, &widget,
                        &xtalk) != MEMTX_OK) {
        return MEMTX_ERROR;
    }
    return shub_widget_write_at(arg->shub, widget, xtalk, value, size, attrs);
}

static const MemoryRegionOps shub_big_ops = {
    .read_with_attrs = shub_big_read,
    .write_with_attrs = shub_big_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static const MemoryRegionOps shub_widget_ops = {
    .read_with_attrs = shub_widget_read,
    .write_with_attrs = shub_widget_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static MemTxResult shub_xio_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    uint8_t buf[8] = { 0 };
    MemTxResult res;

    *data = 0;
    if (addr & (1ULL << 47)) {
        return shub_unmodelled(addr, size, false, 0,
                               "widget register reads not modelled");
    }
    res = shub_inbound(opaque, addr, buf, size, false, attrs);
    *data = ldn_be_p(buf, size);
    return res;
}

static const MemoryRegionOps shub_xio_ops = {
    .read_with_attrs = shub_xio_read,
    .write_with_attrs = shub_xio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

MemoryRegion *sgi_shub_xio_packet_face(SGIShubState *s)
{
    return &s->xio_packet;
}

MemoryRegion *sgi_shub_widget_window(SGIShubState *s, unsigned link)
{
    return link >= 16 || link == 1 ? NULL : &s->widget_window[link];
}

static const char *shub_xio_describe(Object *port, unsigned int index)
{
    return "the SHub's I/O interface";
}

static MemoryRegion *shub_xio_packet_face_get(Object *port, unsigned int index)
{
    return sgi_shub_xio_packet_face(SGI_SHUB(port));
}

static MemTxResult shub_xio_packet_rw(Object *port, unsigned int index,
                                      hwaddr address, void *buffer,
                                      size_t size, bool write,
                                      const SGIXioSource *source,
                                      SGIXioPacketAttrs packet_attrs)
{
    SGIShubState *s = SGI_SHUB(port);
    MemTxAttrs attrs = sgi_xio_source_attrs(source->widget);
    uint64_t value = 0;

    if (!(address & (1ULL << 47))) {
        return shub_inbound(s, address, buffer, size, write, attrs);
    }
    if (!write) {
        return shub_xio_read(s, address, &value, size, attrs);
    }
    if (size != 8) {
        return shub_unmodelled(address, size, true, 0,
                               "packet is 64 bits wide");
    }
    memcpy(&value, buffer, 8);
    return shub_xio_write(s, address, be64_to_cpu(value), size, attrs);
}

static void shub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(oc);

    shub_decode_table_build();
    shub_blind_table_build();
    dc->desc = "SGI SHub 1 node controller (local and global MMR windows)";
    dc->realize = shub_realize;
    dc->vmsd = &shub_vmstate;
    device_class_set_props(dc, shub_properties);
    rc->phases.hold = shub_reset_hold;
    pc->describe = shub_xio_describe;
    pc->packet_face = shub_xio_packet_face_get;
    pc->packet_rw = shub_xio_packet_rw;
}

static const TypeInfo shub_type = {
    .name = TYPE_SGI_SHUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIShubState),
    .instance_init = shub_init,
    .instance_finalize = shub_finalize,
    .class_init = shub_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XIO_PORT },
        { }
    },
};

bool sgi_shub_map_junk_bus(SGIShubState *s, unsigned device, MemoryRegion *mr,
                           Error **errp)
{
    hwaddr offset = SHUB_JUNK_BUS_LED + device * SGI_SHUB_JUNK_BUS_DEVICE_SIZE;

    if (device < SGI_SHUB_JUNK_BUS_FIRST_FREE ||
        device >= SGI_SHUB_JUNK_BUS_DEVICES) {
        error_setg(errp, "junk bus slot %u is not free to map", device);
        return false;
    }
    if (s->junk_bus_mapped[device]) {
        error_setg(errp, "junk bus slot %u is already mapped", device);
        return false;
    }
    if (memory_region_size(mr) > SGI_SHUB_JUNK_BUS_DEVICE_SIZE) {
        error_setg(errp, "a junk bus device decodes at most 64 KiB");
        return false;
    }
    memory_region_add_subregion_overlap(&s->local_mmr, offset, mr, 1);
    memory_region_init_alias(&s->junk_bus_alias[device], OBJECT(s),
                             "sgi-shub.junk-bus", mr, 0,
                             memory_region_size(mr));
    memory_region_add_subregion_overlap(&s->global_mmr, offset,
                                        &s->junk_bus_alias[device], 1);
    s->junk_bus_mapped[device] = true;
    return true;
}

/* The flash takes 32-bit accesses at most: a wide read goes as 16-bit beats. */
static MemTxResult shub_prom_flash_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    uint64_t value = 0;

    *data = 0;
    if ((size != 2 && size != 8) || (addr & (size - 1))) {
        return shub_unmodelled(SGI_SHUB_PROM_FLASH_OFFSET + addr, size, false,
                               0,
                               "PROM flash reads are aligned 16 or 64 bits");
    }
    for (unsigned i = 0; i < size / 2; i++) {
        MemTxResult res;
        uint16_t beat = address_space_lduw_le(&s->prom_flash_as, addr + 2 * i,
                                              attrs, &res);

        if (res != MEMTX_OK) {
            return shub_unmodelled(SGI_SHUB_PROM_FLASH_OFFSET + addr, size,
                                   false, 0, "the flash did not answer");
        }
        value |= (uint64_t)beat << (16 * i);
    }
    *data = value;
    return MEMTX_OK;
}

static MemTxResult shub_prom_flash_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    MemTxResult res;

    if (size != 2 || (addr & 1)) {
        return shub_unmodelled(SGI_SHUB_PROM_FLASH_OFFSET + addr, size, true,
                               value, "PROM flash writes are aligned 16 bits");
    }
    address_space_stw_le(&s->prom_flash_as, addr, value, attrs, &res);
    if (res != MEMTX_OK) {
        return shub_unmodelled(SGI_SHUB_PROM_FLASH_OFFSET + addr, size, true,
                               value, "the flash did not answer");
    }
    return MEMTX_OK;
}

/* The alias answers reads like the flash; what a write does is unknown. */
static MemTxResult shub_prom_flash_alias_write(void *opaque, hwaddr addr,
                                               uint64_t value, unsigned size,
                                               MemTxAttrs attrs)
{
    return shub_unmodelled(SGI_SHUB_PROM_FLASH_ALIAS + addr, size, true, value,
                           "write to the flash alias, behaviour unknown");
}

static const MemoryRegionOps shub_prom_flash_alias_ops = {
    .read_with_attrs = shub_prom_flash_read,
    .write_with_attrs = shub_prom_flash_alias_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static const MemoryRegionOps shub_prom_flash_ops = {
    .read_with_attrs = shub_prom_flash_read,
    .write_with_attrs = shub_prom_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

MemoryRegion *sgi_shub_prom_flash_array(SGIShubState *s)
{
    assert(s->prom_flash);
    return sysbus_mmio_get_region(SYS_BUS_DEVICE(s->prom_flash), 0);
}

bool sgi_shub_add_prom_flash(SGIShubState *s, BlockBackend *blk,
                             const void *image, size_t image_len,
                             Error **errp)
{
    DeviceState *flash;
    MemoryRegion *array;

    if (s->prom_flash) {
        error_setg(errp, "the PROM flash is already added");
        return false;
    }
    if (image_len > SGI_SHUB_PROM_FLASH_SIZE - SGI_SHUB_PROM_FLASH_IMAGE) {
        error_setg(errp, "the firmware image does not fit the PROM flash");
        return false;
    }
    flash = qdev_new(TYPE_PFLASH_CFI02);
    if (blk && !qdev_prop_set_drive_err(flash, "drive", blk, errp)) {
        object_unref(OBJECT(flash));
        return false;
    }
    qdev_prop_set_uint32(flash, "num-blocks", SGI_SHUB_PROM_FLASH_SIZE /
                                              SGI_SHUB_PROM_FLASH_SECTOR);
    qdev_prop_set_uint32(flash, "sector-length", SGI_SHUB_PROM_FLASH_SECTOR);
    qdev_prop_set_uint8(flash, "width", 2);
    /* AMD-compatible unlock cycles, as 16-bit word addresses. */
    qdev_prop_set_uint16(flash, "unlock-addr0", 0x555);
    qdev_prop_set_uint16(flash, "unlock-addr1", 0x2aa);
    /* Inferred AMD ID words: the PROM keeps but never compares them. */
    qdev_prop_set_uint16(flash, "id0", 0x0001);
    qdev_prop_set_uint16(flash, "id1", 0x227e);
    qdev_prop_set_string(flash, "name", "sgi-shub.prom-flash");
    object_property_add_child(OBJECT(s), "prom-flash", OBJECT(flash));
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), errp)) {
        return false;
    }
    array = sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 0);
    if (!blk) {
        uint8_t *ram = memory_region_get_ram_ptr(array);

        memset(ram, 0xff, SGI_SHUB_PROM_FLASH_SIZE);
        if (image_len) {
            memcpy(ram + SGI_SHUB_PROM_FLASH_IMAGE, image, image_len);
        }
        memory_region_flush_rom_device(array, 0, SGI_SHUB_PROM_FLASH_SIZE);
    }
    s->prom_flash = flash;
    address_space_init(&s->prom_flash_as, array, "sgi-shub.prom-flash");
    memory_region_init_io(&s->prom_flash_view, OBJECT(s), &shub_prom_flash_ops,
                          s, "sgi-shub.prom-flash-view",
                          SGI_SHUB_PROM_FLASH_SIZE);
    s->prom_flash_view.disable_reentrancy_guard = true;
    memory_region_add_subregion_overlap(&s->local_mmr,
                                        SGI_SHUB_PROM_FLASH_OFFSET,
                                        &s->prom_flash_view, 1);
    memory_region_init_alias(&s->prom_flash_alias, OBJECT(s),
                             "sgi-shub.prom-flash-alias", &s->prom_flash_view,
                             0, SGI_SHUB_PROM_FLASH_SIZE);
    memory_region_add_subregion_overlap(&s->global_mmr,
                                        SGI_SHUB_PROM_FLASH_OFFSET,
                                        &s->prom_flash_alias, 1);
    /* The firmware table pointer a 2.6.5 kernel reads sits at its top. */
    memory_region_init_io(&s->prom_flash_fit_alias, OBJECT(s),
                          &shub_prom_flash_alias_ops, s,
                          "sgi-shub.prom-flash-fit-alias",
                          SGI_SHUB_PROM_FLASH_SIZE);
    s->prom_flash_fit_alias.disable_reentrancy_guard = true;
    memory_region_add_subregion_overlap(&s->local_mmr,
                                        SGI_SHUB_PROM_FLASH_ALIAS,
                                        &s->prom_flash_fit_alias, 1);
    memory_region_init_alias(&s->prom_flash_fit_alias_global, OBJECT(s),
                             "sgi-shub.prom-flash-fit-alias-global",
                             &s->prom_flash_fit_alias, 0,
                             SGI_SHUB_PROM_FLASH_SIZE);
    memory_region_add_subregion_overlap(&s->global_mmr,
                                        SGI_SHUB_PROM_FLASH_ALIAS,
                                        &s->prom_flash_fit_alias_global, 1);
    return true;
}

static void shub_register_types(void)
{
    type_register_static(&shub_type);
}

type_init(shub_register_types)
