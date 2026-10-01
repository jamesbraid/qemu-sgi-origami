/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Odyssey (VPro) graphics board, as an XIO board carrying one crosstalk
 * widget. Crosstalk part 0xc013, manufacturer 0x023.
 *
 * This first increment models what a stock IP35 PROM and the IRIX IP35
 * graphics driver have been observed to need before they will claim the
 * board: the crosstalk configuration block, the board-identity part on its
 * one-wire access register, the memory-configuration register, the software
 * sync register and the interrupt flag, enable and acknowledge trio.
 *
 * Not implemented: the command FIFO at 0x110000, the indirect write port at
 * 0x400000, the read-back window at 0x200000, the display back end and its
 * bit-banged I2C at 0x233xxx, DMA, context switching and every rendering
 * path. Those offsets read zero, discard writes and report themselves once
 * through LOG_UNIMP; every access is traced. Nothing here paints a pixel.
 *
 * No interrupt source is modeled yet, so the flag register stays zero and
 * the interrupt line stays low; the flag, enable and acknowledge semantics
 * are modeled so a later increment only has to raise flags.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/1wire/ds2505.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-odyssey.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

/*
 * The whole aperture a driver maps. IRIX maps 0x401000 bytes and OpenBSD
 * 0x410000; the difference is unreconciled, so the region covers the
 * larger one.
 */
#define ODSY_APERTURE 0x410000

#define ODSY_PART 0xc013
#define ODSY_MANUFACTURER 0x023

/*
 * The crosstalk configuration block: eight 64-bit registers on an 8-byte
 * stride, whose low halves are the 0x04, 0x0c, ... spellings every SGI
 * driver and this tree's other widgets use.
 */
#define ODSY_BLOCK_END 0x60
#define ODSY_IDENTITY 0x00
#define ODSY_STATUS 0x08
#define ODSY_ERROR_ADDR_UPPER 0x10
#define ODSY_ERROR_ADDR_LOWER 0x18
#define ODSY_CONTROL 0x20
#define ODSY_INT_DEST_UPPER 0x30
#define ODSY_INT_DEST_LOWER 0x38
#define ODSY_ERROR_COMMAND 0x40

/* The board registers this increment answers. */
#define ODSY_INTR_FLAGS 0x1014
#define ODSY_INTR_ENABLE 0x1034
#define ODSY_INTR_ACK 0x103c
#define ODSY_MEMORY_CONFIG 0x106c
#define ODSY_SW_SYNC 0x107c
#define ODSY_NIC_PROM 0x10d8
#define ODSY_NIC_KERNEL 0x10dc

/*
 * SGI's MicroLAN control register, the layout the Bridge, the XBridge and
 * the Hub all carry: pulse width in bits 19:10, sample offset in 9:2,
 * completion in bit 1 and the sampled wire level in bit 0.
 */
#define ODSY_NIC_LEVEL (1u << 0)
#define ODSY_NIC_DONE (1u << 1)
#define ODSY_NIC_SAMPLE_SHIFT 2
#define ODSY_NIC_SAMPLE_MASK (0xffu << ODSY_NIC_SAMPLE_SHIFT)
#define ODSY_NIC_PULSE_SHIFT 10
#define ODSY_NIC_PULSE_MASK (0x3ffu << ODSY_NIC_PULSE_SHIFT)

/* Board-identity part family 0x0b, one 32-byte page, 48-bit serial. */
#define ODSY_NIC_FAMILY 0x0b
#define ODSY_NIC_PAGE 32
#define ODSY_NIC_ROM_SIZE 160
#define ODSY_NIC_SERIAL_MASK MAKE_64BIT_MASK(0, 48)
#define ODSY_NIC_RECORD_DEFAULT "Name:ODY128VERSIONB"

#define ODSY_CONTROL_WIDGET_MASK 0xf

/* Memory configuration: size code, bank count and column-address latency. */
#define ODSY_CONFIG_SIZE_SHIFT 30
#define ODSY_CONFIG_TWO_BANKS (1u << 29)
#define ODSY_CONFIG_LATENCY_2 (1u << 28)

static uint32_t odyssey_identity(SGIOdysseyState *s)
{
    return (s->revision << 28) | (ODSY_PART << 12) |
           (ODSY_MANUFACTURER << 1) | 1;
}

/* Reads and writes are reported apart: a write carries a command word. */
#define ODSY_UNIMP_WRITE (1u << 31)

static void odyssey_unimplemented(SGIOdysseyState *s, hwaddr addr,
                                  unsigned size, bool write)
{
    uint32_t token = (uint32_t)addr | (write ? ODSY_UNIMP_WRITE : 0);
    unsigned int i;

    for (i = 0; i < s->reported_count; i++) {
        if (s->reported[i] == token) {
            return;
        }
    }
    if (s->reported_count == SGI_ODYSSEY_UNIMP_REPORTS) {
        return;
    }
    s->reported[s->reported_count++] = token;
    qemu_log_mask(LOG_UNIMP, "sgi-odyssey: %u-byte %s at 0x%" HWADDR_PRIx
                  " is not modeled%s\n", size, write ? "write" : "read", addr,
                  s->reported_count == SGI_ODYSSEY_UNIMP_REPORTS ?
                  "; further offsets stay silent" : "");
}

/*
 * The modeled register an access names, or -1. A configuration-block
 * register answers both its 64-bit address and the 32-bit spelling of its
 * low half; every other register has only been seen as a 32-bit access.
 * Every other width, alignment and offset is unmodeled rather than an
 * error: what the board does with them is not established.
 */
static int64_t odyssey_reg(hwaddr addr, unsigned size)
{
    if (addr < ODSY_BLOCK_END) {
        if (size == 8 && (addr & 7) == 0) {
            return addr;
        }
        if (size == 4 && (addr & 7) == 4) {
            return addr & ~7;
        }
        return -1;
    }
    if (size != 4) {
        return -1;
    }
    switch (addr) {
    case ODSY_INTR_FLAGS:
    case ODSY_INTR_ENABLE:
    case ODSY_INTR_ACK:
    case ODSY_MEMORY_CONFIG:
    case ODSY_SW_SYNC:
    case ODSY_NIC_PROM:
    case ODSY_NIC_KERNEL:
        return addr;
    default:
        return -1;
    }
}

static void odyssey_update_irq(SGIOdysseyState *s)
{
    bool level = (s->intr_flags & s->intr_enable) != 0;

    if (level != s->irq_asserted) {
        s->irq_asserted = level;
        qemu_set_irq(s->irq, level);
    }
}

/* One bit slot, reported finished inside the write the guest polls after. */
static void odyssey_nic_slot(SGIOdysseyState *s, uint32_t value)
{
    uint32_t pulse = (value & ODSY_NIC_PULSE_MASK) >> ODSY_NIC_PULSE_SHIFT;
    uint32_t sample = (value & ODSY_NIC_SAMPLE_MASK) >> ODSY_NIC_SAMPLE_SHIFT;
    bool level = sgi_onewire_slot(s->nic_bus, pulse, sample);

    s->nic = (value & ~(ODSY_NIC_LEVEL | ODSY_NIC_DONE)) | ODSY_NIC_DONE |
             (level ? ODSY_NIC_LEVEL : 0);
}

static uint64_t odyssey_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIOdysseyState *s = opaque;
    uint64_t value = 0;

    switch (odyssey_reg(addr, size)) {
    case ODSY_IDENTITY:
        value = odyssey_identity(s);
        break;
    case ODSY_CONTROL:
        value = s->widget_control;
        break;
    case ODSY_INT_DEST_UPPER:
        value = s->int_dest_upper;
        break;
    case ODSY_INT_DEST_LOWER:
        value = s->int_dest_lower;
        break;
    case ODSY_ERROR_COMMAND:
        value = s->error_command;
        break;
    case ODSY_STATUS:
    case ODSY_ERROR_ADDR_UPPER:
    case ODSY_ERROR_ADDR_LOWER:
        /* No crosstalk error is ever posted, so the error class is clear. */
        break;
    case ODSY_INTR_FLAGS:
        value = s->intr_flags;
        break;
    case ODSY_INTR_ENABLE:
        value = s->intr_enable;
        break;
    case ODSY_MEMORY_CONFIG:
        value = s->memory_config;
        break;
    case ODSY_SW_SYNC:
        value = s->sw_sync;
        break;
    case ODSY_NIC_PROM:
    case ODSY_NIC_KERNEL:
        value = s->nic;
        break;
    default:
        odyssey_unimplemented(s, addr, size, false);
        break;
    }
    trace_sgi_odyssey_mmio(addr, size, value, false);
    return value;
}

static void odyssey_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SGIOdysseyState *s = opaque;

    trace_sgi_odyssey_mmio(addr, size, value, true);

    switch (odyssey_reg(addr, size)) {
    case ODSY_CONTROL:
        s->widget_control = value;
        break;
    case ODSY_INT_DEST_UPPER:
        s->int_dest_upper = value;
        break;
    case ODSY_INT_DEST_LOWER:
        s->int_dest_lower = value;
        break;
    case ODSY_ERROR_COMMAND:
        /* Written to acknowledge; the data itself is discarded. */
        s->error_command = 0;
        break;
    case ODSY_INTR_ENABLE:
        s->intr_enable = value;
        odyssey_update_irq(s);
        break;
    case ODSY_INTR_ACK:
        s->intr_flags &= ~(uint32_t)value;
        odyssey_update_irq(s);
        break;
    case ODSY_SW_SYNC:
        s->sw_sync = value;
        break;
    case ODSY_NIC_PROM:
    case ODSY_NIC_KERNEL:
        odyssey_nic_slot(s, value);
        break;
    case ODSY_IDENTITY:
    case ODSY_STATUS:
    case ODSY_ERROR_ADDR_UPPER:
    case ODSY_ERROR_ADDR_LOWER:
    case ODSY_INTR_FLAGS:
    case ODSY_MEMORY_CONFIG:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-odyssey: write of 0x%" PRIx64
                      " to the read-only register at 0x%" HWADDR_PRIx "\n",
                      value, addr);
        break;
    default:
        odyssey_unimplemented(s, addr, size, true);
        break;
    }
}

static const MemoryRegionOps odyssey_ops = {
    .read = odyssey_read,
    .write = odyssey_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    /* Keep 64-bit register and FIFO words whole instead of splitting them. */
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * The board-identity record. Its field spelling is the one this project
 * captured off real SGI firmware (docs/library/boot-logs.md), and the
 * prologue -- a format byte, four spaces and a semicolon, so a scanning
 * reader meets ";Name:" and a positional reader finds the value at byte 11
 * -- is the convention hw/pci-host/sgi-bridge.c already writes.
 *
 * One field by default, because the stock IP35 PROM was observed reading
 * exactly one 32-byte page and stopping; a longer record is truncated
 * where the master stops clocking. The default value is the public owner
 * record of the board the Odyssey contract describes.
 */
static void odyssey_build_nic_rom(SGIOdysseyState *s,
                                  uint8_t rom[ODSY_NIC_ROM_SIZE])
{
    const char *fields = s->nic_record ?: ODSY_NIC_RECORD_DEFAULT;
    g_autofree char *record = g_strdup_printf("%c    ;%s;", 0x01, fields);
    size_t length = MIN(strlen(record), ODSY_NIC_ROM_SIZE);

    memset(rom, 0xff, ODSY_NIC_ROM_SIZE);
    memcpy(rom, record, length);
    if (length < ODSY_NIC_PAGE) {
        memset(&rom[length], ' ', ODSY_NIC_PAGE - length);
    }
}

static bool odyssey_attach_nic(SGIOdysseyState *s, Error **errp)
{
    uint8_t rom[ODSY_NIC_ROM_SIZE];
    DeviceState *part;

    if (s->nic_serial & ~ODSY_NIC_SERIAL_MASK) {
        error_setg(errp, "the sgi-odyssey one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", s->nic_serial);
        return false;
    }
    s->nic_bus = onewire_create_bus(DEVICE(s), "nic");
    odyssey_build_nic_rom(s, rom);
    part = qdev_new(TYPE_ONEWIRE_DS2505);
    object_property_add_child(OBJECT(s), "nic-part", OBJECT(part));
    qdev_prop_set_uint8(part, "family", ODSY_NIC_FAMILY);
    qdev_prop_set_uint64(part, "serial", s->nic_serial);
    ds2505_set_image(part, rom, sizeof(rom));
    return qdev_realize_and_unref(part, BUS(s->nic_bus), errp);
}

static bool odyssey_compose_memory_config(SGIOdysseyState *s, Error **errp)
{
    uint32_t code;

    switch (s->memory_size) {
    case 32:
        code = 0;
        break;
    case 64:
        code = 1;
        break;
    case 128:
        code = 2;
        break;
    default:
        error_setg(errp, "sgi-odyssey memory-size must be 32, 64 or 128 "
                   "megabytes, not %u", s->memory_size);
        return false;
    }
    if (s->memory_banks != 2 && s->memory_banks != 4) {
        error_setg(errp, "sgi-odyssey memory-banks must be 2 or 4, not %u",
                   s->memory_banks);
        return false;
    }
    if (s->cas_latency != 2 && s->cas_latency != 3) {
        error_setg(errp, "sgi-odyssey cas-latency must be 2 or 3, not %u",
                   s->cas_latency);
        return false;
    }
    /*
     * The outstanding-operation count and the data-FIFO level, which the
     * driver waits to reach zero, stay zero: neither is modeled.
     */
    s->memory_config = code << ODSY_CONFIG_SIZE_SHIFT |
                       (s->memory_banks == 2 ? ODSY_CONFIG_TWO_BANKS : 0) |
                       (s->cas_latency == 2 ? ODSY_CONFIG_LATENCY_2 : 0);
    return true;
}

static void odyssey_reset_registers(SGIOdysseyState *s)
{
    /*
     * The driver reads its own widget number out of the low nibble before
     * anything has written the register. The rest of the reset value is
     * not established, so it is zero.
     */
    s->widget_control = SGI_XIO_BOARD(s)->slot & ODSY_CONTROL_WIDGET_MASK;
    s->int_dest_upper = 0;
    s->int_dest_lower = 0;
    s->error_command = 0;
    s->intr_flags = 0;
    s->intr_enable = 0;
    s->sw_sync = 0;
    s->nic = 0;
    s->reported_count = 0;
}

static bool odyssey_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIOdysseyState *s = SGI_ODYSSEY(board);

    if (s->revision > 0xf) {
        error_setg(errp, "sgi-odyssey revision must fit in four bits, not %u",
                   s->revision);
        return false;
    }
    if (!odyssey_compose_memory_config(s, errp)) {
        return false;
    }
    odyssey_reset_registers(s);
    board->cpu_face = &s->regs;
    board->packet_face = &s->regs;
    return true;
}

/*
 * The base class re-parents the board under its module, and re-parenting a
 * device destroys every bus hanging off it, so the board-identity bus has
 * to be built after the base realize rather than inside build_widget().
 */
static void odyssey_realize(DeviceState *dev, Error **errp)
{
    SGIOdysseyClass *oc = SGI_ODYSSEY_GET_CLASS(dev);
    Error *local_err = NULL;

    oc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    odyssey_attach_nic(SGI_ODYSSEY(dev), errp);
}

static void odyssey_reset_hold(Object *obj, ResetType type)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);

    odyssey_reset_registers(s);
    odyssey_update_irq(s);
}

static void odyssey_instance_init(Object *obj)
{
    SGIOdysseyState *s = SGI_ODYSSEY(obj);

    memory_region_init_io(&s->regs, obj, &odyssey_ops, s, "sgi-odyssey.regs",
                          ODSY_APERTURE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

/*
 * Crosstalk part 0xc013 and manufacturer 0x023 are the board's, from the
 * part number the IP35 driver registers for. Revision nibble 1 is in the
 * set that driver accepts, not an observed silicon stepping. The memory
 * profile and the one-wire record default to the public owner record of
 * the 128 MB board; the serial is synthetic and says so.
 */
static const Property odyssey_properties[] = {
    DEFINE_PROP_UINT32("revision", SGIOdysseyState, revision, 1),
    DEFINE_PROP_UINT32("memory-size", SGIOdysseyState, memory_size, 128),
    DEFINE_PROP_UINT32("memory-banks", SGIOdysseyState, memory_banks, 4),
    DEFINE_PROP_UINT32("cas-latency", SGIOdysseyState, cas_latency, 3),
    DEFINE_PROP_UINT64("nic-serial", SGIOdysseyState, nic_serial,
                       0x5d0000000001ULL),
    DEFINE_PROP_STRING("nic-record", SGIOdysseyState, nic_record),
};

static int odyssey_post_load(void *opaque, int version_id)
{
    SGIOdysseyState *s = opaque;

    s->irq_asserted = false;
    odyssey_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_odyssey = {
    .name = TYPE_SGI_ODYSSEY,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = odyssey_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(widget_control, SGIOdysseyState),
        VMSTATE_UINT32(int_dest_upper, SGIOdysseyState),
        VMSTATE_UINT32(int_dest_lower, SGIOdysseyState),
        VMSTATE_UINT32(error_command, SGIOdysseyState),
        VMSTATE_UINT32(intr_flags, SGIOdysseyState),
        VMSTATE_UINT32(intr_enable, SGIOdysseyState),
        VMSTATE_UINT32(sw_sync, SGIOdysseyState),
        VMSTATE_UINT32(nic, SGIOdysseyState),
        VMSTATE_END_OF_LIST()
    },
};

static void odyssey_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);
    SGIOdysseyClass *odc = SGI_ODYSSEY_CLASS(oc);

    dc->desc = "SGI Odyssey (VPro) graphics board";
    dc->vmsd = &vmstate_sgi_odyssey;
    dc->user_creatable = true;
    device_class_set_props(dc, odyssey_properties);
    device_class_set_parent_realize(dc, odyssey_realize, &odc->parent_realize);
    rc->phases.hold = odyssey_reset_hold;
    bc->build_widget = odyssey_build_widget;
}

static const TypeInfo odyssey_types[] = {
    {
        .name = TYPE_SGI_ODYSSEY,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(SGIOdysseyState),
        .class_size = sizeof(SGIOdysseyClass),
        .instance_init = odyssey_instance_init,
        .class_init = odyssey_class_init,
    },
};

DEFINE_TYPES(odyssey_types)
