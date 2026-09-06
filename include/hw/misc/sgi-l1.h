/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_L1_H
#define HW_MISC_SGI_L1_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"

#define TYPE_SGI_L1 "sgi-l1"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1State, SGI_L1)
#define SGI_L1_SIZE 0x40
#define SGI_L1_FRAME_SIZE 129
#define SGI_L1_QUEUE_SIZE 4096

typedef struct SGIL1EEPROM {
    uint32_t length;
    uint8_t *data;
} SGIL1EEPROM;

struct SGIL1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq uart;
    bool thre_pending;
    CharFrontend chr;
    int32_t revision_major;
    int32_t revision_minor;
    int32_t revision_patch;
    bool console_mode;
    bool return_to_console;
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
    uint8_t lcr;
    uint8_t ier;
    uint8_t mcr;
    uint8_t scratch;
    uint8_t divisor[2];
    uint8_t frame[SGI_L1_FRAME_SIZE];
    uint32_t frame_len;
    bool in_frame;
    bool escape;
    bool overflow;
    uint8_t queue[SGI_L1_QUEUE_SIZE];
    uint32_t queue_head;
    uint32_t queue_len;
    uint8_t display[2][128];
};
#endif
