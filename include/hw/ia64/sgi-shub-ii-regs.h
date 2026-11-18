/* SPDX-License-Identifier: BSD-3-Clause */
/* The SHub I/O interface registers the model uses; names follow Linux's public
 * shubio.h and shub_mmr.h, and a register with no public name is described by
 * its address. */
#ifndef SHUB_II_CLEAN_H
#define SHUB_II_CLEAN_H

#include <stdint.h>

/* widget_id */
#define SHUB_II_WIDGET_ID                            0x400000

/* widget_status */
#define SHUB_II_WIDGET_STATUS                        0x400008

/* widget_control */
#define SHUB_II_WIDGET_CONTROL                       0x400020

/* local_access_protection */
#define SHUB_II_LOCAL_ACCESS_PROTECTION              0x400100

/* local_access_override */
#define SHUB_II_LOCAL_ACCESS_OVERRIDE                0x400108

/* outbound_widget_access */
#define SHUB_II_OUTBOUND_WIDGET_ACCESS               0x400110

/* inbound_widget_access */
#define SHUB_II_INBOUND_WIDGET_ACCESS                0x400118

/* inbound_device_error_mask */
/* llp_control_status */
#define SHUB_II_LLP_CONTROL_STATUS                   0x400128

/* llp_log */
#define SHUB_II_LLP_LOG                              0x400130

/* interrupt_destination */
#define SHUB_II_INTERRUPT_DESTINATION                0x400138

/* graphics_map */
#define SHUB_II_GRAPHICS_MAP                         0x400140

/* scratch */
#define SHUB_II_SCRATCH                              0x400150

/* itte */
#define SHUB_II_ITTE                                 0x400160
#define SHUB_II_ITTE_COUNT                           7
#define SHUB_II_ITTE_STRIDE                          8
#define SHUB_II_ITTE_IOSP_SHIFT                      12
#define SHUB_II_ITTE_IOSP_MASK                       0x1ull
#define SHUB_II_ITTE_IOSP_INPLACE \
    (SHUB_II_ITTE_IOSP_MASK << SHUB_II_ITTE_IOSP_SHIFT)

/* prb */
#define SHUB_II_PRB                                  0x400198

/* crosstalk_credit_timeout */
#define SHUB_II_CROSSTALK_CREDIT_TIMEOUT             0x4001e0

/* misc_error_mask */
/* crosstalk_timeout_threshold */
#define SHUB_II_CROSSTALK_TIMEOUT_THRESHOLD          0x4001f0

/* error_clear */
#define SHUB_II_ERROR_CLEAR                          0x4001f8

/* bte_common_control */
#define SHUB_II_BTE_COMMON_CONTROL                   0x400200

/* spurious_message */
#define SHUB_II_SPURIOUS_MESSAGE                     0x400208

/* spurious_sideband */
#define SHUB_II_SPURIOUS_SIDEBAND                    0x400210

/* llp_channel_test */
#define SHUB_II_LLP_CHANNEL_TEST                     0x400218

/* error_packet_header_1 */
#define SHUB_II_ERROR_PACKET_HEADER_1                0x400220

/* error_packet_header_2 */
#define SHUB_II_ERROR_PACKET_HEADER_2                0x400228

/* sxb_access_protection */
#define SHUB_II_SXB_ACCESS_PROTECTION                0x400230

/* sxb_access_override */
#define SHUB_II_SXB_ACCESS_OVERRIDE                  0x400238

/* wrapper_interrupt */
#define SHUB_II_WRAPPER_INTERRUPT                    0x400240

/* wrapper_error_log */
#define SHUB_II_WRAPPER_ERROR_LOG                    0x400248

/* wrapper_control */
#define SHUB_II_WRAPPER_CONTROL                      0x400250

/* wrapper_status */
#define SHUB_II_WRAPPER_STATUS                       0x400258

/* wrapper_error_interrupt_mask */
#define SHUB_II_WRAPPER_ERROR_INTERRUPT_MASK         0x400260

/* prb_counter_adjust */
#define SHUB_II_PRB_COUNTER_ADJUST                   0x400300

/* pio_read_entry_a */
#define SHUB_II_PIO_READ_ENTRY_A                     0x400308

/* pio_read_entry_b */
#define SHUB_II_PIO_READ_ENTRY_B                     0x400348

/* pio_deallocate */
#define SHUB_II_PIO_DEALLOCATE                       0x400388

/* crb_deallocate */
#define SHUB_II_CRB_DEALLOCATE                       0x400390

/* ioq_fifo_depth */
#define SHUB_II_IOQ_FIFO_DEPTH                       0x400398

/* iiq_arbitration */
#define SHUB_II_IIQ_ARBITRATION                      0x4003a0

/* crb_management */
#define SHUB_II_CRB_MANAGEMENT                       0x4003a8

/* crb_control */
#define SHUB_II_CRB_CONTROL                          0x4003b0

/* crb_timeout */
#define SHUB_II_CRB_TIMEOUT                          0x4003b8

/* crb_timeout_prescaler */
#define SHUB_II_CRB_TIMEOUT_PRESCALER                0x4003c0

/* crb_a */
#define SHUB_II_CRB_A                                0x400400

/* crb_b */
#define SHUB_II_CRB_B                                0x400408

/* crb_c */
#define SHUB_II_CRB_C                                0x400410

/* crb_d */
#define SHUB_II_CRB_D                                0x400418

/* crb_e */
#define SHUB_II_CRB_E                                0x400420

/* spurious_crb_message_low */
#define SHUB_II_SPURIOUS_CRB_MESSAGE_LOW             0x400700

/* spurious_crb_message_mid */
#define SHUB_II_SPURIOUS_CRB_MESSAGE_MID             0x400708

/* spurious_crb_message_high */
#define SHUB_II_SPURIOUS_CRB_MESSAGE_HIGH            0x400710

/* debug_submenu_select */
#define SHUB_II_DEBUG_SUBMENU_SELECT                 0x400718

/* bte_length_status */
#define SHUB_II_BTE_LENGTH_STATUS                    0x410000
#define SHUB_II_BTE_LENGTH_STATUS_COUNT              2
#define SHUB_II_BTE_LENGTH_STATUS_STRIDE             65536
#define SHUB_II_BTE_LENGTH_STATUS_BUSY_SHIFT         20
#define SHUB_II_BTE_LENGTH_STATUS_BUSY_MASK          0x1ull
#define SHUB_II_BTE_LENGTH_STATUS_LENGTH_MASK        0xffffull

/* bte_source */
#define SHUB_II_BTE_SOURCE                           0x410008
#define SHUB_II_BTE_SOURCE_COUNT                     2
#define SHUB_II_BTE_SOURCE_STRIDE                    65536

/* bte_destination */
#define SHUB_II_BTE_DESTINATION                      0x410010
#define SHUB_II_BTE_DESTINATION_COUNT                2
#define SHUB_II_BTE_DESTINATION_STRIDE               65536

/* bte_control */
#define SHUB_II_BTE_CONTROL                          0x410018
#define SHUB_II_BTE_CONTROL_COUNT                    2
#define SHUB_II_BTE_CONTROL_STRIDE                   65536
#define SHUB_II_BTE_CONTROL_NOTIFY_SHIFT             4
#define SHUB_II_BTE_CONTROL_NOTIFY_MASK              0x1ull
#define SHUB_II_BTE_CONTROL_NOTIFY_INPLACE \
    (SHUB_II_BTE_CONTROL_NOTIFY_MASK << SHUB_II_BTE_CONTROL_NOTIFY_SHIFT)
#define SHUB_II_BTE_CONTROL_ZEROFILL_SHIFT           0
#define SHUB_II_BTE_CONTROL_ZEROFILL_MASK            0x1ull
#define SHUB_II_BTE_CONTROL_ZEROFILL_INPLACE \
    (SHUB_II_BTE_CONTROL_ZEROFILL_MASK << SHUB_II_BTE_CONTROL_ZEROFILL_SHIFT)

/* bte_notify */
#define SHUB_II_BTE_NOTIFY                           0x410020
#define SHUB_II_BTE_NOTIFY_COUNT                     2
#define SHUB_II_BTE_NOTIFY_STRIDE                    65536

/* bte_interrupt */
#define SHUB_II_BTE_INTERRUPT                        0x410028
#define SHUB_II_BTE_INTERRUPT_COUNT                  2
#define SHUB_II_BTE_INTERRUPT_STRIDE                 65536

/* perf_control */
#define SHUB_II_PERF_CONTROL                         0x430000

/* perf_profile */
#define SHUB_II_PERF_PROFILE                         0x430008

#endif /* SHUB_II_CLEAN_H */
