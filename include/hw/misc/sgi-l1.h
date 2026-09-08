/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_L1_H
#define HW_MISC_SGI_L1_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"

#define TYPE_SGI_L1 "sgi-l1"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1State, SGI_L1)
#define TYPE_SGI_L1_UART "sgi-l1-uart"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1UARTState, SGI_L1_UART)
#define SGI_L1_SIZE 0x40
/* 256 payload bytes plus the two check bytes: the firmware's receive cap. */
#define SGI_L1_FRAME_SIZE 258
#define SGI_L1_QUEUE_SIZE 4096
#define SGI_L1_LOG_ENTRIES 128
#define SGI_L1_LOG_TEXT 48

typedef struct SGIL1EEPROM {
    uint32_t length;
    uint8_t *data;
} SGIL1EEPROM;

/* The controller: what the L1 keeps on its own side of the link. */
struct SGIL1State {
    DeviceState parent_obj;
    SGIL1UARTState *uart;
    CharFrontend chr;
    qemu_irq node_power_out;
    qemu_irq node_reset_out;
    qemu_irq soft_reset_out;
    qemu_irq nmi_out;
    QEMUBH *power_up_bh;
    int32_t revision_major;
    int32_t revision_minor;
    int32_t revision_patch;
    bool auto_power;
    bool node_power;
    bool power_up_pending;
    bool console_mode;
    bool escaped;
    bool warned_off;
    bool promreq_debug;
    GString *capture;
    uint8_t command[128];
    uint32_t command_len;
    char **chassis_files;
    char **board_files;
    char **spd_files;
    uint32_t chassis_count;
    uint32_t board_count;
    uint32_t spd_count;
    SGIL1EEPROM records[48];
    uint32_t rack;
    uint32_t bay;
    uint32_t pending_rack;
    uint32_t pending_bay;
    uint32_t node_position;
    uint8_t port_field;
    uint8_t event_subchannel;
    bool events_enabled;
    int64_t clock_offset;
    int64_t log_time[SGI_L1_LOG_ENTRIES];
    uint8_t log_text[SGI_L1_LOG_ENTRIES][SGI_L1_LOG_TEXT];
    uint32_t log_head;
    uint32_t log_len;
    uint8_t frame[SGI_L1_FRAME_SIZE];
    uint32_t frame_len;
    bool in_frame;
    bool escape;
    bool overflow;
    uint8_t display[2][128];
};

/* The node-side junk-bus UART window through which the Bedrock reaches it. */
struct SGIL1UARTState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    SGIL1State *controller;
    bool thre_pending;
    uint8_t lcr;
    uint8_t ier;
    uint8_t mcr;
    uint8_t scratch;
    uint8_t divisor[2];
    uint8_t queue[SGI_L1_QUEUE_SIZE];
    uint32_t queue_head;
    uint32_t queue_len;
};

#endif
