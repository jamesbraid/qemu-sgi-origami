/* SPDX-License-Identifier: BSD-3-Clause */
/* The SHub registers and fields the model uses; names follow Linux's public
 * shub_mmr.h, and a register with no public name is described by its address. */
#ifndef SHUB_CLEAN_H
#define SHUB_CLEAN_H

#include <stdint.h>

/* The chip identification word at node offset 0x1_0000_0000 (no public name). */
#define SHUB_MD_REG_0000_0000                                 0x100000000
#define SHUB_MD_REG_0000_0000_REVISION_SHIFT                  28
#define SHUB_MD_REG_0000_0000_PART_NUMBER_SHIFT               12

/* SH_MMRBIST_ERR */
#define SHUB_MMRBIST_ERR                         0x100000080

/* SH_MEM_ERROR_SUMMARY */
/* SH_MEM_ERROR_SUMMARY_ALIAS */
#define SHUB_MEM_ERROR_SUMMARY_ALIAS                           0x1000000c0

/* SH_X_DIMM_CFG */
#define SHUB_X_DIMM_CFG                            0x100010000

/* SH_Y_DIMM_CFG */
#define SHUB_Y_DIMM_CFG                            0x100010008

/* SH_JNR_DIMM_CFG */
#define SHUB_JNR_DIMM_CFG                            0x100010010

/* SH_X_CFG */
#define SHUB_X_CFG                             0x100010020

/* SH_X_DQCT_CFG */
#define SHUB_X_DQCT_CFG                             0x100010028

/* SH_X_REFRESH_CONTROL */
#define SHUB_X_REFRESH_CONTROL                            0x100010030

/* SH_Y_CFG */
#define SHUB_Y_CFG                             0x100010040

/* SH_Y_DQCT_CFG */
#define SHUB_Y_DQCT_CFG                             0x100010048

/* SH_Y_REFRESH_CONTROL */
#define SHUB_Y_REFRESH_CONTROL                            0x100010050

/* SH_MEM_RED_BLACK */
#define SHUB_MEM_RED_BLACK                             0x100010058

/* SH_MISC_MEM_CFG */
#define SHUB_MISC_MEM_CFG                               0x100010060

/* SH_PIO_RQ_CRD_CTL */
#define SHUB_PIO_RQ_CRD_CTL                        0x100010068

/* SH_PI_MD_RP_CRD_CTL */
#define SHUB_PI_MD_RP_CRD_CTL                             0x100010078

/* SH_MMRBIST_BASE */
#define SHUB_MMRBIST_BASE                            0x100020080

/* SH_MMRBIST_CTL */
#define SHUB_MMRBIST_CTL                            0x100020088

/* md_reg_0003_0000 */
#define SHUB_MD_REG_0003_0000                        0x100030000

/* md_reg_0003_0200 */
#define SHUB_MD_REG_0003_0200                        0x100030200
#define SHUB_MD_REG_0003_0200_COUNT                  8
#define SHUB_MD_REG_0003_0200_STRIDE                 16

/* md_dqlp_dir_privilege_vector */
#define SHUB_MD_DQLP_DIR_PRIVILEGE_VECTOR            0x100030300

/* md_reg_0003_0400 */
#define SHUB_MD_REG_0003_0400                        0x100030400

/* md_dir_access_lp */
#define SHUB_MD_DIR_ACCESS_LP                        0x100031000

/* SH_MD_DQLP_MMR_XBIST_H */
#define SHUB_MD_DQLP_MMR_XBIST_H                         0x100038000
#define SHUB_MD_DQLP_MMR_XBIST_H_COUNT                   4
#define SHUB_MD_DQLP_MMR_XBIST_H_STRIDE                  65536

/* SH_MD_DQLP_MMR_XBIST_L */
#define SHUB_MD_DQLP_MMR_XBIST_L                         0x100038010
#define SHUB_MD_DQLP_MMR_XBIST_L_COUNT                   4
#define SHUB_MD_DQLP_MMR_XBIST_L_STRIDE                  65536

/* SH_MD_DQLP_MMR_YBIST_H */
#define SHUB_MD_DQLP_MMR_YBIST_H                         0x100038800
#define SHUB_MD_DQLP_MMR_YBIST_H_COUNT                   4
#define SHUB_MD_DQLP_MMR_YBIST_H_STRIDE                  65536

/* SH_MD_DQLP_MMR_YBIST_L */
#define SHUB_MD_DQLP_MMR_YBIST_L                         0x100038810
#define SHUB_MD_DQLP_MMR_YBIST_L_COUNT                   4
#define SHUB_MD_DQLP_MMR_YBIST_L_STRIDE                  65536

/* SH_MD_DQRP_MMR_DIR_CONFIG */
#define SHUB_MD_DQRP_MMR_DIR_CONFIG                        0x100050000

/* SH_MD_DQRP_MMR_DIR_LOCVEC0 */
#define SHUB_MD_DQRP_MMR_DIR_LOCVEC0                        0x100050200
#define SHUB_MD_DQRP_MMR_DIR_LOCVEC0_COUNT                  8
#define SHUB_MD_DQRP_MMR_DIR_LOCVEC0_STRIDE                 16

/* md_dqrp_dir_privilege_vector */
#define SHUB_MD_DQRP_DIR_PRIVILEGE_VECTOR            0x100050300

/* md_reg_0005_0400 */
#define SHUB_MD_REG_0005_0400                        0x100050400

/* md_dir_access_rp */
#define SHUB_MD_DIR_ACCESS_RP                        0x100051000

/* ii_int */
#define SHUB_II_INT                                  0x110000000
#define SHUB_II_INT_COUNT                            2
#define SHUB_II_INT_STRIDE                           256

/* ii_int_config */
#define SHUB_II_INT_CONFIG                           0x110000080
#define SHUB_II_INT_CONFIG_COUNT                     2
#define SHUB_II_INT_CONFIG_STRIDE                    256
#define SHUB_II_INT_CONFIG_PID_SHIFT                 4
#define SHUB_II_INT_CONFIG_PID_MASK                  0xffffull

/* ii_int_enable */
#define SHUB_II_INT_ENABLE                           0x110000200
#define SHUB_II_INT_ENABLE_COUNT                     2
#define SHUB_II_INT_ENABLE_STRIDE                    128

/* int_node_id_config */
#define SHUB_INT_NODE_ID_CONFIG                      0x110000300

/* ipi_int */
#define SHUB_IPI_INT                                 0x110000380
#define SHUB_IPI_INT_SEND_SHIFT                      63
#define SHUB_IPI_INT_SEND_MASK                       0x1ull
#define SHUB_IPI_INT_SEND_INPLACE \
    (SHUB_IPI_INT_SEND_MASK << SHUB_IPI_INT_SEND_SHIFT)
#define SHUB_IPI_INT_IDX_SHIFT                       52
#define SHUB_IPI_INT_IDX_MASK                        0xffull
#define SHUB_IPI_INT_PID_SHIFT                       4
#define SHUB_IPI_INT_PID_MASK                        0xffffull
#define SHUB_IPI_INT_TYPE_SHIFT                      0
#define SHUB_IPI_INT_TYPE_MASK                       0x7ull

/* ipi_int_enable */
#define SHUB_IPI_INT_ENABLE                          0x110000400

/* local_int_config */
#define SHUB_LOCAL_INT_CONFIG                        0x110000480
#define SHUB_LOCAL_INT_CONFIG_COUNT                  6
#define SHUB_LOCAL_INT_CONFIG_STRIDE                 256

/* SH_LOCAL_INT0_ENABLE */
#define SHUB_LOCAL_INT0_ENABLE                        0x110000500
#define SHUB_LOCAL_INT0_ENABLE_COUNT                  6
#define SHUB_LOCAL_INT0_ENABLE_STRIDE                 256

/* SH_RTC0_INT_CONFIG */
#define SHUB_RTC0_INT_CONFIG                         0x110001380

/* SH_RTC0_INT_ENABLE */
#define SHUB_RTC0_INT_ENABLE                         0x110001400

/* rtc_int_config */
#define SHUB_RTC_INT_CONFIG                          0x110001480
#define SHUB_RTC_INT_CONFIG_STRIDE                   256

/* rtc_int_enable */
#define SHUB_RTC_INT_ENABLE                          0x110001500
#define SHUB_RTC_INT_ENABLE_STRIDE                   256

/* event_occurred */
#define SHUB_EVENT_OCCURRED                          0x110010000
#define SHUB_EVENT_OCCURRED_IPI_INT_SHIFT            28
#define SHUB_EVENT_OCCURRED_IPI_INT_MASK             0x1ull
#define SHUB_EVENT_OCCURRED_IPI_INT_INPLACE \
    (SHUB_EVENT_OCCURRED_IPI_INT_MASK << SHUB_EVENT_OCCURRED_IPI_INT_SHIFT)
#define SHUB_EVENT_OCCURRED_UART_INT_SHIFT           20
#define SHUB_EVENT_OCCURRED_UART_INT_MASK            0x1ull
#define SHUB_EVENT_OCCURRED_UART_INT_INPLACE \
    (SHUB_EVENT_OCCURRED_UART_INT_MASK << SHUB_EVENT_OCCURRED_UART_INT_SHIFT)

/* event_occurred_alias */
#define SHUB_EVENT_OCCURRED_ALIAS                    0x110010008

/* event_overflow */
#define SHUB_EVENT_OVERFLOW                          0x110010080

/* SH_EVENT_OVERFLOW_ALIAS */
#define SHUB_EVENT_OVERFLOW_ALIAS                    0x110010088

/* SH_JUNK_BUS_TIME */
#define SHUB_JUNK_BUS_TIME                        0x110020000

/* SH_JUNK_BUS_LED0 */
#define SHUB_JUNK_BUS_LED0                            0x110030000
#define SHUB_JUNK_BUS_LED0_COUNT                      4
#define SHUB_JUNK_BUS_LED0_STRIDE                     128

/* rtc_ctl */
#define SHUB_RTC_CTL                                 0x110060280
#define SHUB_RTC_CTL_PERIOD_SHIFT                    4
#define SHUB_RTC_CTL_PERIOD_MASK                     0x3ffull

/* ipi_access */
#define SHUB_IPI_ACCESS                              0x110060480

/* SH_JTAG_CONFIG */
#define SHUB_JTAG_CONFIG                              0x110060500

/* shub_id */
#define SHUB_SHUB_ID                                 0x110060580
#define SHUB_SHUB_ID_NODES_PER_BIT_SHIFT             48
#define SHUB_SHUB_ID_NODES_PER_BIT_MASK              0x1full
#define SHUB_SHUB_ID_SIZE_CODE_SHIFT                 44
#define SHUB_SHUB_ID_SIZE_CODE_MASK                  0x3ull
#define SHUB_SHUB_ID_NASID_SHIFT                     32
#define SHUB_SHUB_ID_NASID_MASK                      0x7ffull
#define SHUB_SHUB_ID_REVISION_SHIFT                  28
#define SHUB_SHUB_ID_REVISION_MASK                   0xfull

/* ptc */
#define SHUB_PTC                                     0x1101a0000
#define SHUB_PTC_COUNT                               2
#define SHUB_PTC_STRIDE                              128

/* ptc_parms */
#define SHUB_PTC_PARMS                               0x1101a0100

/* SH_INT_CMPA */
#define SHUB_INT_CMPA                                0x1101b0000

/* int_cmp */
#define SHUB_INT_CMP                                 0x1101b0080
#define SHUB_INT_CMP_STRIDE                          128
#define SHUB_INT_CMP_COMPARE_MASK                    0x7fffffffffffffull

/* rtc */
#define SHUB_RTC                                     0x1101c0000

/* SH_SCRATCH1 */
#define SHUB_SCRATCH1                                0x1101d0080

/* SH_SCRATCH1_ALIAS */
#define SHUB_SCRATCH1_ALIAS                            0x1101d0088

/* SH_SCRATCH2 */
#define SHUB_SCRATCH2                                0x1101d0100

/* lb_lock2 */
#define SHUB_LB_LOCK2                                0x1101d0180

/* lb_lock2_status */
#define SHUB_LB_LOCK2_STATUS                         0x1101d0188

/* lb_lock */
#define SHUB_LB_LOCK                                 0x1101d0200

/* lb_lock_status */
#define SHUB_LB_LOCK_STATUS                          0x1101d0208

/* crb_message_control */
#define SHUB_CRB_MESSAGE_CONTROL                     0x120000000

/* crb_nack_limit */
#define SHUB_CRB_NACK_LIMIT                          0x120000080

/* crb_timeout_prescale */
#define SHUB_CRB_TIMEOUT_PRESCALE                    0x120000100

/* crb_timeout_skid */
#define SHUB_CRB_TIMEOUT_SKID                        0x120000180

/* pi_reg_2001_0100 */
#define SHUB_PI_REG_2001_0100                        0x120010100

/* SH_FSB_SYMMETRIC_AGENT_STATUS */
#define SHUB_FSB_SYMMETRIC_AGENT_STATUS                        0x120020080

/* pi_cam_control */
#define SHUB_PI_CAM_CONTROL                          0x120050300

/* SH_PI_CAM_ADDRESS_READ_DATA */
/* SH_PI_CAM_LPRA_READ_DATA */
/* SH_PI_CAM_STATE_READ_DATA */
/* pio_write_status */
#define SHUB_PIO_WRITE_STATUS                        0x120070200
#define SHUB_PIO_WRITE_STATUS_COUNT                  2
#define SHUB_PIO_WRITE_STATUS_STRIDE                 128

/* pio_write_status_alias */
#define SHUB_PIO_WRITE_STATUS_ALIAS                  0x120070208
#define SHUB_PIO_WRITE_STATUS_ALIAS_COUNT            2
#define SHUB_PIO_WRITE_STATUS_ALIAS_STRIDE           128

/* cpu_hist */
#define SHUB_CPU_HIST                                0x150031500
#define SHUB_CPU_HIST_COUNT                          4
#define SHUB_CPU_HIST_STRIDE                         16

/* fw_word_1540 */
#define SHUB_FW_WORD_1540                            0x150031540

/* cpu_bits */
#define SHUB_CPU_BITS                                0x150031550

/* fw_debug_word */
#define SHUB_FW_DEBUG_WORD                           0x150031560

/* junk_bus_led */
#define SHUB_JUNK_BUS_LED                            0x7fed00000
#define SHUB_JUNK_BUS_LED_COUNT                      4
#define SHUB_JUNK_BUS_LED_STRIDE                     65536

#define SHUB_PIO_WRITE_STATUS_ALIAS_CLEAR            0x7u
#define SHUB_PIO_WRITE_STATUS_IDLE                   0x3f00000000000000ull
#define SHUB_PI_CAM_WAR_VALUE                        0x8000000000000010ull
#define SHUB_RTC_CONFIG_MASK                         0xff3ffffffefffffull
#define SHUB_RTC_ENABLE_MASK                         0x1u
#define SHUB_RTC_MASK                                0x7fffffffffffffull

#endif /* SHUB_CLEAN_H */
