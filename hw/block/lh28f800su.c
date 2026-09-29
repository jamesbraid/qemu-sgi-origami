/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Sharp LH28F800SU x16 compatible commands (SMT96107, May 1996).
 * Enhanced commands, x8 mode, and process-restart backing are unsupported.
 * The board owns chip selection, bus lanes, and physical pin wiring.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

#define TYPE_LH28F800SU "lh28f800su"
OBJECT_DECLARE_SIMPLE_TYPE(LH28F800SUState, LH28F800SU)

#define FLASH_SIZE MiB
#define BLOCK_SIZE (64 * KiB)
#define PROGRAM_NS 8000
#define ERASE_NS 700000000
#define RECOVERY_NS 400

#define STATUS_READY 0x80
#define STATUS_SUSPEND 0x40
#define STATUS_ERASE_ERROR 0x20
#define STATUS_PROGRAM_ERROR 0x10
#define STATUS_LOW_VPP 0x08

typedef enum FlashMode {
    FLASH_ARRAY,
    FLASH_ID,
    FLASH_STATUS,
} FlashMode;

typedef enum FlashPending {
    FLASH_IDLE,
    FLASH_PROGRAM,
    FLASH_ERASE,
} FlashPending;

typedef enum FlashOperation {
    FLASH_OP_NONE,
    FLASH_OP_PROGRAM,
    FLASH_OP_ERASE,
    FLASH_OP_RECOVER,
} FlashOperation;

typedef enum FlashPin {
    FLASH_PIN_VPP,
    FLASH_PIN_WP,
    FLASH_PIN_RP,
} FlashPin;

struct LH28F800SUState {
    SysBusDevice parent_obj;
    MemoryRegion mem;
    uint8_t *storage;
    QEMUTimer *timer;
    qemu_irq ready_busy;
    uint16_t initial_lock_mask;
    uint16_t lock_mask;
    uint16_t program_data;
    uint8_t status;
    uint8_t mode;
    uint8_t pending;
    uint8_t operation;
    uint32_t operation_offset;
    int64_t remaining_ns;
    bool suspended;
    bool vpp_good;
    bool wp_high;
    bool rp_high;
};

static void flash_update_outputs(LH28F800SUState *s)
{
    bool ready = (s->operation != FLASH_OP_PROGRAM &&
                  s->operation != FLASH_OP_ERASE) || s->suspended;

    /* ROMD would bypass the chip's x16 access-width check on array reads. */
    memory_region_rom_device_set_romd(&s->mem, false);
    qemu_set_irq(s->ready_busy, ready);
}

static void flash_set_mode(LH28F800SUState *s, FlashMode mode)
{
    s->mode = mode;
    flash_update_outputs(s);
}

static void flash_finish(void *opaque)
{
    LH28F800SUState *s = opaque;
    uint32_t offset = s->operation_offset;

    switch (s->operation) {
    case FLASH_OP_PROGRAM:
        s->storage[offset] = s->program_data >> 8;
        s->storage[offset + 1] = s->program_data;
        memory_region_set_dirty(&s->mem, offset, 2);
        break;
    case FLASH_OP_ERASE:
        memset(s->storage + offset, 0xff, BLOCK_SIZE);
        memory_region_set_dirty(&s->mem, offset, BLOCK_SIZE);
        break;
    case FLASH_OP_RECOVER:
        break;
    default:
        g_assert_not_reached();
    }
    s->operation = FLASH_OP_NONE;
    s->remaining_ns = 0;
    s->suspended = false;
    s->status |= STATUS_READY;
    flash_update_outputs(s);
}

static void flash_start(LH28F800SUState *s, FlashOperation operation,
                        uint32_t offset, int64_t duration_ns)
{
    s->operation = operation;
    s->operation_offset = offset;
    s->remaining_ns = duration_ns;
    s->status &= ~(STATUS_READY | STATUS_SUSPEND);
    flash_set_mode(s, FLASH_STATUS);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + duration_ns);
}

static void flash_failure(LH28F800SUState *s, uint8_t flags)
{
    s->status |= STATUS_READY | flags;
    flash_set_mode(s, FLASH_STATUS);
}

static void flash_pin(void *opaque, int n, int level)
{
    LH28F800SUState *s = opaque;

    switch (n) {
    case FLASH_PIN_VPP:
        if (!level && s->vpp_good &&
            (s->operation == FLASH_OP_PROGRAM ||
             s->operation == FLASH_OP_ERASE)) {
            FlashOperation interrupted = s->operation;

            /* Atomic supply-loss abort is a model policy, not a chip claim. */
            timer_del(s->timer);
            s->operation = FLASH_OP_NONE;
            s->remaining_ns = 0;
            s->suspended = false;
            flash_failure(s, STATUS_LOW_VPP |
                          (interrupted == FLASH_OP_PROGRAM ?
                           STATUS_PROGRAM_ERROR : STATUS_ERASE_ERROR));
        }
        s->vpp_good = level;
        break;
    case FLASH_PIN_WP:
        s->wp_high = level;
        break;
    case FLASH_PIN_RP:
        if (s->rp_high == !!level) {
            break;
        }
        s->rp_high = level;
        timer_del(s->timer);
        s->pending = FLASH_IDLE;
        s->operation = FLASH_OP_NONE;
        s->remaining_ns = 0;
        s->suspended = false;
        s->status = STATUS_READY;
        s->mode = FLASH_ARRAY;
        if (level) {
            s->operation = FLASH_OP_RECOVER;
            s->remaining_ns = RECOVERY_NS;
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      RECOVERY_NS);
        }
        flash_update_outputs(s);
        break;
    default:
        g_assert_not_reached();
    }
}

static MemTxResult flash_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    LH28F800SUState *s = opaque;

    if (size != 2 || (offset & 1)) {
        return MEMTX_ERROR;
    }
    if (!s->rp_high || s->operation == FLASH_OP_RECOVER) {
        return MEMTX_ERROR;
    }
    switch (s->mode) {
    case FLASH_ARRAY:
        *value = ((uint16_t)s->storage[offset] << 8) | s->storage[offset + 1];
        break;
    case FLASH_ID:
        if (offset == 0) {
            *value = 0x00b0;
        } else if (offset == 2) {
            *value = 0x66a8;
        } else {
            qemu_log_mask(LOG_UNIMP, "lh28f800su: unsupported identifier "
                          "word at 0x%" HWADDR_PRIx "\n", offset);
            return MEMTX_ERROR;
        }
        break;
    case FLASH_STATUS:
        /* The chip does not drive DQ8-DQ15 for status reads. */
        *value = s->status;
        break;
    default:
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult flash_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    LH28F800SUState *s = opaque;
    uint8_t cmd = value;
    uint16_t old;
    uint32_t block = offset / BLOCK_SIZE;

    if (size != 2 || (offset & 1)) {
        return MEMTX_ERROR;
    }
    if (!s->rp_high || s->operation == FLASH_OP_RECOVER) {
        return MEMTX_ERROR;
    }
    if (s->operation == FLASH_OP_ERASE && !s->suspended && cmd == 0xb0) {
        s->remaining_ns = MAX((int64_t)1,
                              (int64_t)timer_expire_time_ns(s->timer) -
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        timer_del(s->timer);
        s->suspended = true;
        s->status |= STATUS_READY | STATUS_SUSPEND;
        flash_set_mode(s, FLASH_STATUS);
        return MEMTX_OK;
    }
    if (s->suspended && cmd == 0xd0) {
        s->suspended = false;
        s->status &= ~(STATUS_READY | STATUS_SUSPEND);
        flash_set_mode(s, FLASH_STATUS);
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->remaining_ns);
        return MEMTX_OK;
    }
    if (s->operation != FLASH_OP_NONE && !s->suspended && cmd != 0x70) {
        qemu_log_mask(LOG_UNIMP, "lh28f800su: command while busy\n");
        return MEMTX_ERROR;
    }
    if (s->pending == FLASH_PROGRAM) {
        s->pending = FLASH_IDLE;
        old = ((uint16_t)s->storage[offset] << 8) | s->storage[offset + 1];
        if (!s->vpp_good) {
            flash_failure(s, STATUS_LOW_VPP | STATUS_PROGRAM_ERROR);
        } else if ((!s->wp_high && (s->lock_mask & (1u << block))) ||
                   (value & ~old)) {
            flash_failure(s, STATUS_PROGRAM_ERROR);
        } else {
            s->program_data = value;
            flash_start(s, FLASH_OP_PROGRAM, offset, PROGRAM_NS);
        }
        return MEMTX_OK;
    }
    if (s->pending == FLASH_ERASE) {
        s->pending = FLASH_IDLE;
        if (cmd != 0xd0) {
            flash_failure(s, STATUS_ERASE_ERROR | STATUS_PROGRAM_ERROR);
        } else if (!s->vpp_good) {
            flash_failure(s, STATUS_LOW_VPP | STATUS_ERASE_ERROR);
        } else if (!s->wp_high && (s->lock_mask & (1u << block))) {
            flash_failure(s, STATUS_ERASE_ERROR);
        } else {
            flash_start(s, FLASH_OP_ERASE, block * BLOCK_SIZE, ERASE_NS);
        }
        return MEMTX_OK;
    }

    switch (cmd) {
    case 0xff:
        flash_set_mode(s, FLASH_ARRAY);
        break;
    case 0x90:
        flash_set_mode(s, FLASH_ID);
        break;
    case 0x70:
        flash_set_mode(s, FLASH_STATUS);
        break;
    case 0x50:
        s->status &= ~(STATUS_ERASE_ERROR | STATUS_PROGRAM_ERROR |
                       STATUS_LOW_VPP);
        flash_set_mode(s, FLASH_STATUS);
        break;
    case 0x10:
    case 0x40:
        if (s->operation != FLASH_OP_NONE) {
            return MEMTX_ERROR;
        }
        s->pending = FLASH_PROGRAM;
        flash_set_mode(s, FLASH_STATUS);
        break;
    case 0x20:
        if (s->operation != FLASH_OP_NONE) {
            return MEMTX_ERROR;
        }
        s->pending = FLASH_ERASE;
        flash_set_mode(s, FLASH_STATUS);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "lh28f800su: unsupported command 0x%02x "
                      "at 0x%" HWADDR_PRIx "\n", cmd, offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps flash_ops = {
    .read_with_attrs = flash_read,
    .write_with_attrs = flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static int flash_pre_save(void *opaque)
{
    LH28F800SUState *s = opaque;

    if (s->operation != FLASH_OP_NONE && !s->suspended) {
        s->remaining_ns = MAX((int64_t)1,
                              (int64_t)timer_expire_time_ns(s->timer) -
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return 0;
}

static int flash_post_load(void *opaque, int version_id)
{
    LH28F800SUState *s = opaque;

    if (s->mode > FLASH_STATUS || s->pending > FLASH_ERASE ||
        s->operation > FLASH_OP_RECOVER || s->operation_offset >= FLASH_SIZE ||
        (s->operation == FLASH_OP_ERASE &&
         s->operation_offset % BLOCK_SIZE) ||
        (s->suspended && s->operation != FLASH_OP_ERASE) ||
        (s->operation != FLASH_OP_NONE && s->remaining_ns <= 0)) {
        return -EINVAL;
    }
    if (s->operation != FLASH_OP_NONE && !s->suspended) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->remaining_ns);
    } else {
        timer_del(s->timer);
    }
    flash_update_outputs(s);
    return 0;
}

static const VMStateDescription vmstate_flash = {
    .name = TYPE_LH28F800SU,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = flash_pre_save,
    .post_load = flash_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(lock_mask, LH28F800SUState),
        VMSTATE_UINT16(program_data, LH28F800SUState),
        VMSTATE_UINT8(status, LH28F800SUState),
        VMSTATE_UINT8(mode, LH28F800SUState),
        VMSTATE_UINT8(pending, LH28F800SUState),
        VMSTATE_UINT8(operation, LH28F800SUState),
        VMSTATE_UINT32(operation_offset, LH28F800SUState),
        VMSTATE_INT64(remaining_ns, LH28F800SUState),
        VMSTATE_BOOL(suspended, LH28F800SUState),
        VMSTATE_BOOL(vpp_good, LH28F800SUState),
        VMSTATE_BOOL(wp_high, LH28F800SUState),
        VMSTATE_BOOL(rp_high, LH28F800SUState),
        VMSTATE_END_OF_LIST()
    },
};

static void flash_realize(DeviceState *dev, Error **errp)
{
    LH28F800SUState *s = LH28F800SU(dev);

    if (!memory_region_init_rom_device(&s->mem, OBJECT(dev), &flash_ops, s,
                                       TYPE_LH28F800SU, FLASH_SIZE, errp)) {
        return;
    }
    s->storage = memory_region_get_ram_ptr(&s->mem);
    memset(s->storage, 0xff, FLASH_SIZE);
    memory_region_set_dirty(&s->mem, 0, FLASH_SIZE);
    s->lock_mask = s->initial_lock_mask;
    s->status = STATUS_READY;
    s->mode = FLASH_ARRAY;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, flash_finish, s);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mem);
    flash_update_outputs(s);
}

static void flash_finalize(Object *obj)
{
    LH28F800SUState *s = LH28F800SU(obj);

    timer_free(s->timer);
}

static const Property flash_properties[] = {
    DEFINE_PROP_UINT16("initial-lock-mask", LH28F800SUState,
                       initial_lock_mask, 0xffff),
};

static void flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flash_realize;
    dc->vmsd = &vmstate_flash;
    dc->user_creatable = false;
    device_class_set_props(dc, flash_properties);
}

static void flash_instance_init(Object *obj)
{
    LH28F800SUState *s = LH28F800SU(obj);

    qdev_init_gpio_in_named(DEVICE(obj), flash_pin, "pins", 3);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ready_busy, "ready-busy", 1);
}

static const TypeInfo flash_type = {
    .name = TYPE_LH28F800SU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LH28F800SUState),
    .instance_init = flash_instance_init,
    .instance_finalize = flash_finalize,
    .class_init = flash_class_init,
};

static void flash_register_types(void)
{
    type_register_static(&flash_type);
}

type_init(flash_register_types)
