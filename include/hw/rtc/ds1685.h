/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DS1685/DS1687 real-time clock: the industry-standard PC clock's fourteen
 * registers plus a second, software-selected bank carrying everything the
 * older part did not have.
 */

#ifndef HW_RTC_DS1685_H
#define HW_RTC_DS1685_H

#include "hw/core/sysbus.h"
#include "system/runstate.h"
#include "qemu/notify.h"
#include "qom/object.h"

/*
 * The die is a DS1685; the DS1687 is that die sealed with its crystal and
 * cell, and the only programming difference is one inert bit. QEMU names
 * clocks by the die, as its neighbours in this directory do.
 */
#define TYPE_DS1685 "ds1685"
OBJECT_DECLARE_SIMPLE_TYPE(DS1685State, DS1685)

/*
 * Each port is one byte wide but answers on every lane of a word. The chip
 * select is what picks the port; the low address bits are not part of the
 * decode, and the path a guest arrives over moves them.
 */
#define DS1685_PORT_SIZE        4

/*
 * 00h-3Fh is the same storage in both banks; only 40h-7Fh changes meaning
 * with the bank bit. A model that switches the whole file answers every
 * driver wrongly at the four control registers.
 */
#define DS1685_COMMON_SIZE      0x40
#define DS1685_UPPER_SIZE       0x40
#define DS1685_EXTRAM_SIZE      0x80
#define DS1685_PAYLOAD_SIZE     0x140
#define DS1685_IMAGE_SIZE       0x200
#define DS1685_CLOCK_REF_BYTES  24
#define DS1685_CLOCK_REF_MAGIC  "DS168RF1"

/* Common to both banks. */
#define DS1685_SECONDS          0x00
#define DS1685_SECONDS_ALARM    0x01
#define DS1685_MINUTES          0x02
#define DS1685_MINUTES_ALARM    0x03
#define DS1685_HOURS            0x04
#define DS1685_HOURS_ALARM      0x05
#define DS1685_WEEKDAY          0x06
#define DS1685_DAY              0x07
#define DS1685_MONTH            0x08
#define DS1685_YEAR             0x09
#define DS1685_REG_A            0x0a
#define DS1685_REG_B            0x0b
#define DS1685_REG_C            0x0c
#define DS1685_REG_D            0x0d
#define DS1685_RAM_LOW_FIRST    0x0e

/* The bank-1 half. Bank 0 has sixty-four more bytes of user RAM here. */
#define DS1685_UPPER_FIRST      0x40
#define DS1685_MODEL            0x40
#define DS1685_SERIAL_FIRST     0x41
#define DS1685_SERIAL_LAST      0x46
#define DS1685_SERIAL_CRC       0x47
#define DS1685_CENTURY          0x48
#define DS1685_DATE_ALARM       0x49
#define DS1685_EXT_CTRL_4A      0x4a
#define DS1685_EXT_CTRL_4B      0x4b
#define DS1685_SMI_STACK_FIRST  0x4e
#define DS1685_SMI_STACK_LAST   0x4f
#define DS1685_EXTRAM_ADDR      0x50
#define DS1685_EXTRAM_DATA      0x53

/* Register A: update status, the oscillator pair, the bank bit, the rate. */
#define DS1685_A_UIP            0x80    /* read-only */
#define DS1685_A_DV2            0x40
#define DS1685_A_DV1            0x20
#define DS1685_A_DV0            0x10    /* bank select */
#define DS1685_A_RS             0x0f

/* Register B: the freeze, three enables, and the format bits. */
#define DS1685_B_SET            0x80
#define DS1685_B_PIE            0x40
#define DS1685_B_AIE            0x20
#define DS1685_B_UIE            0x10
#define DS1685_B_SQWE           0x08
#define DS1685_B_DM             0x04    /* 1 binary, 0 BCD */
#define DS1685_B_24_12          0x02    /* 1 twenty-four hour */
#define DS1685_B_DSE            0x01

/* Register C: read-only, and reading it clears the three flags. */
#define DS1685_C_IRQF           0x80    /* derived, never stored */
#define DS1685_C_PF             0x40
#define DS1685_C_AF             0x20
#define DS1685_C_UF             0x10
#define DS1685_C_FLAGS          (DS1685_C_PF | DS1685_C_AF | DS1685_C_UF)

/* Register D: read-only. */
#define DS1685_D_VRT            0x80

/* Extended control 4A: the flags here clear only by writing a zero. */
#define DS1685_4A_VRT2          0x80    /* read-only */
#define DS1685_4A_INCR          0x40    /* read-only */
#define DS1685_4A_RESERVED      0x30    /* readable, writable, inert */
#define DS1685_4A_PAB           0x08
#define DS1685_4A_RF            0x04
#define DS1685_4A_WF            0x02
#define DS1685_4A_KF            0x01
#define DS1685_4A_FLAGS         (DS1685_4A_RF | DS1685_4A_WF | DS1685_4A_KF)

/* Extended control 4B: the three extended enables and the pin controls. */
#define DS1685_4B_ABE           0x80
#define DS1685_4B_E32K          0x40
#define DS1685_4B_CS            0x20
#define DS1685_4B_RCE           0x10
#define DS1685_4B_PRS           0x08
#define DS1685_4B_RIE           0x04
#define DS1685_4B_WIE           0x02
#define DS1685_4B_KSE           0x01

/* Read at bank-1 40h. Linux prints "Unknown" for anything else. */
#define DS1685_MODEL_NUMBER     0x71

/* Any alarm byte at or above this matches every value of its field. */
#define DS1685_ALARM_DONT_CARE  0xc0

struct DS1685State {
    SysBusDevice parent_obj;

    /*
     * Two byte-wide ports, not one window: a parallel host cannot drive the
     * part's multiplexed bus, so it writes a register number to the first
     * and reads or writes the register through the second.
     */
    MemoryRegion index_port;
    MemoryRegion data_port;

    /* Open-drain and active-low on the part; asserted here means driven. */
    qemu_irq irq;

    /* The latched register number. Seven bits are meaningful. */
    uint8_t index;

    uint8_t common[DS1685_COMMON_SIZE];   /* 00h-3Fh, both banks */
    uint8_t nvram[DS1685_UPPER_SIZE];     /* 40h-7Fh with the bank bit clear */
    uint8_t ext[DS1685_UPPER_SIZE];       /* 40h-7Fh with the bank bit set */
    uint8_t extram[DS1685_EXTRAM_SIZE];   /* reached through 50h and 53h */

    /* Guest-set time as an offset from the host clock, in seconds. */
    int64_t time_offset;
    int64_t held_seconds;
    bool counter_held;

    BlockBackend *blk;
    BlockBackend *clock_blk;
    VMChangeStateEntry *vmstate_change;
    QEMUBH *save_bh;
    Notifier incoming_complete;
    bool incoming;
    bool load_pending;
    bool storage_failed;

    /*
     * The once-a-second transfer into the visible bytes, and the deadline it
     * is next due at, which is also what the update-in-progress bit is
     * computed from rather than stored.
     */
    QEMUTimer *update_timer;
    int64_t next_update_ns;

    QEMUTimer *periodic_timer;
    int64_t periodic_period_ns;
};

#endif /* HW_RTC_DS1685_H */
