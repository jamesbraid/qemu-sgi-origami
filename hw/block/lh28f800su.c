/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Sharp LH28F800SU x16 commands (SMT96107, May 1996).
 * Page operations, queuing, and x8 mode are unsupported.
 * The board owns chip selection, bus lanes, and physical pin wiring.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "block/block-common.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"
#include "system/runstate.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

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
#define GSR_BUFFER_READY 0x06
#define BSR_OPERATION_ERROR 0x20
#define BSR_LOW_VPP 0x04

typedef enum FlashMode {
    FLASH_ARRAY,
    FLASH_ID,
    FLASH_STATUS,
    FLASH_EXT_STATUS,
} FlashMode;

typedef enum FlashPending {
    FLASH_IDLE,
    FLASH_PROGRAM,
    FLASH_ERASE,
    FLASH_UPLOAD,
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
    qemu_irq array_read;
    uint16_t initial_lock_mask;
    uint16_t lock_mask;
    uint16_t displayed_unlocked_mask;
    uint16_t program_data;
    uint8_t status;
    uint8_t block_status[FLASH_SIZE / BLOCK_SIZE];
    bool gsr_error;
    uint8_t mode;
    uint8_t pending;
    uint8_t operation;
    uint32_t operation_offset;
    int64_t remaining_ns;
    bool suspended;
    bool vpp_good;
    bool wp_high;
    bool rp_high;
    bool initial_rp_high;
    BlockBackend *blk;
    Notifier shutdown;
    bool shutdown_registered;
    bool storage_failed;
};

static void flash_failed_run_shutdown(Notifier *notifier, void *data)
{
    qemu_system_shutdown_request_with_code(*(ShutdownCause *)data,
                                           EXIT_FAILURE);
}

static void flash_preserve_run_failure(void)
{
    static Notifier notifier = { .notify = flash_failed_run_shutdown };
    static bool registered;

    /* A failed run remains failed after the device is removed. */
    if (!registered) {
        qemu_register_shutdown_notifier(&notifier);
        registered = true;
    }
}

static void flash_update_outputs(LH28F800SUState *s);

static void flash_storage_error(LH28F800SUState *s, int ret)
{
    error_report("lh28f800su: persistent flash I/O failed: %s",
                 strerror(-ret));
    s->storage_failed = true;
    flash_update_outputs(s);
    flash_preserve_run_failure();
    timer_del(s->timer);
    /* Keep the failed operation busy until the operator terminates the VM. */
    /* Defer the stop until the current timer callback has returned. */
    blk_error_action(s->blk, BLOCK_ERROR_ACTION_STOP, false, -ret);
}

static bool flash_store(LH28F800SUState *s, uint32_t offset,
                        const uint8_t *bytes, size_t size)
{
    int ret;

    if (!s->blk) {
        return true;
    }
    ret = blk_pwrite(s->blk, offset, size, bytes, 0);
    if (ret >= 0) {
        ret = blk_flush(s->blk);
    }
    if (ret < 0) {
        flash_storage_error(s, ret);
        return false;
    }
    return true;
}

static void flash_shutdown(Notifier *notifier, void *data)
{
    LH28F800SUState *s = container_of(notifier, LH28F800SUState, shutdown);
    int ret;

    /* Interrupted operations never become completed writes at shutdown. */
    timer_del(s->timer);
    ret = blk_flush(s->blk);
    if (ret < 0 && !s->storage_failed) {
        flash_storage_error(s, ret);
    }
    if (s->storage_failed) {
        qemu_system_shutdown_request_with_code(*(ShutdownCause *)data,
                                               EXIT_FAILURE);
    }
}

static void flash_update_outputs(LH28F800SUState *s)
{
    bool ready = (s->operation != FLASH_OP_PROGRAM &&
                  s->operation != FLASH_OP_ERASE) || s->suspended;

    /* ROMD would bypass the chip's x16 access-width check on array reads. */
    memory_region_rom_device_set_romd(&s->mem, false);
    qemu_set_irq(s->ready_busy, ready);
    qemu_set_irq(s->array_read, s->mode == FLASH_ARRAY && s->rp_high &&
                 s->operation != FLASH_OP_RECOVER && !s->storage_failed);
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
        if (s->blk) {
            uint8_t bytes[] = { s->program_data >> 8, s->program_data };

            if (!flash_store(s, offset, bytes, sizeof(bytes))) {
                return;
            }
        }
        s->storage[offset] = s->program_data >> 8;
        s->storage[offset + 1] = s->program_data;
        memory_region_set_dirty(&s->mem, offset, 2);
        break;
    case FLASH_OP_ERASE:
        if (s->blk) {
            g_autofree uint8_t *bytes = g_malloc(BLOCK_SIZE);

            memset(bytes, 0xff, BLOCK_SIZE);
            if (!flash_store(s, offset, bytes, BLOCK_SIZE)) {
                return;
            }
        }
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

static void flash_failure(LH28F800SUState *s, uint8_t flags, uint32_t block)
{
    s->status |= STATUS_READY | flags;
    s->gsr_error = true;
    s->block_status[block] |= BSR_OPERATION_ERROR;
    if (flags & STATUS_LOW_VPP) {
        s->block_status[block] |= BSR_LOW_VPP;
    }
    flash_set_mode(s, FLASH_STATUS);
}

static void flash_pin(void *opaque, int n, int level)
{
    LH28F800SUState *s = opaque;

    /* Board links can retain this object after device unrealize. */
    if (!s->timer) {
        return;
    }
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
                           STATUS_PROGRAM_ERROR : STATUS_ERASE_ERROR),
                          s->operation_offset / BLOCK_SIZE);
        }
        s->vpp_good = level;
        break;
    case FLASH_PIN_WP:
        s->wp_high = level;
        break;
    case FLASH_PIN_RP:
        if (s->rp_high == !!level) {
            flash_update_outputs(s);
            break;
        }
        s->rp_high = level;
        timer_del(s->timer);
        s->pending = FLASH_IDLE;
        s->operation = FLASH_OP_NONE;
        s->remaining_ns = 0;
        s->suspended = false;
        s->status = STATUS_READY;
        s->gsr_error = false;
        memset(s->block_status, 0, sizeof(s->block_status));
        s->displayed_unlocked_mask = 0;
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

    if (!s->timer || size != 2 || (offset & 1)) {
        return MEMTX_ERROR;
    }
    if (!s->rp_high) {
        return MEMTX_DECODE_ERROR;
    }
    if (s->operation == FLASH_OP_RECOVER) {
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
    case FLASH_EXT_STATUS: {
        uint32_t block = offset / BLOCK_SIZE;

        switch (offset % BLOCK_SIZE) {
        case 2:
            *value = s->block_status[block] |
                     ((s->displayed_unlocked_mask & (1u << block)) ?
                      0x40 : 0) |
                     (((s->operation == FLASH_OP_PROGRAM ||
                        s->operation == FLASH_OP_ERASE) &&
                       s->operation_offset / BLOCK_SIZE == block &&
                       !s->suspended) ? 0 : STATUS_READY);
            break;
        case 4:
            *value = GSR_BUFFER_READY |
                     (s->status & (STATUS_READY | STATUS_SUSPEND)) |
                     (s->gsr_error ? BSR_OPERATION_ERROR : 0);
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                          "lh28f800su: reserved extended status address "
                          "0x%" HWADDR_PRIx "\n", offset);
            return MEMTX_ERROR;
        }
        break;
    }
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

    if (!s->timer || size != 2 || (offset & 1)) {
        return MEMTX_ERROR;
    }
    if (s->storage_failed && cmd != 0xff && cmd != 0x90 &&
        cmd != 0x70 && cmd != 0x71) {
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
    if (s->operation != FLASH_OP_NONE && !s->suspended &&
        cmd != 0x70 && cmd != 0x71 && !s->storage_failed) {
        qemu_log_mask(LOG_UNIMP, "lh28f800su: command while busy\n");
        return MEMTX_ERROR;
    }
    if (s->pending == FLASH_PROGRAM) {
        s->pending = FLASH_IDLE;
        old = ((uint16_t)s->storage[offset] << 8) | s->storage[offset + 1];
        if (!s->vpp_good) {
            flash_failure(s, STATUS_LOW_VPP | STATUS_PROGRAM_ERROR, block);
        } else if ((!s->wp_high && (s->lock_mask & (1u << block))) ||
                   (value & ~old)) {
            flash_failure(s, STATUS_PROGRAM_ERROR, block);
        } else {
            s->program_data = value;
            flash_start(s, FLASH_OP_PROGRAM, offset, PROGRAM_NS);
        }
        return MEMTX_OK;
    }
    if (s->pending == FLASH_ERASE) {
        s->pending = FLASH_IDLE;
        if (cmd != 0xd0) {
            flash_failure(s, STATUS_ERASE_ERROR | STATUS_PROGRAM_ERROR,
                          block);
        } else if (!s->vpp_good) {
            flash_failure(s, STATUS_LOW_VPP | STATUS_ERASE_ERROR, block);
        } else if (!s->wp_high && (s->lock_mask & (1u << block))) {
            flash_failure(s, STATUS_ERASE_ERROR, block);
        } else {
            flash_start(s, FLASH_OP_ERASE, block * BLOCK_SIZE, ERASE_NS);
        }
        return MEMTX_OK;
    }
    if (s->pending == FLASH_UPLOAD) {
        s->pending = FLASH_IDLE;
        if (cmd != 0xd0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "lh28f800su: invalid status upload confirmation "
                          "0x%02x\n", cmd);
            return MEMTX_ERROR;
        }
        s->displayed_unlocked_mask = ~s->lock_mask;
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
    case 0x71:
        flash_set_mode(s, FLASH_EXT_STATUS);
        break;
    case 0x50:
        s->status &= ~(STATUS_ERASE_ERROR | STATUS_PROGRAM_ERROR |
                       STATUS_LOW_VPP);
        s->gsr_error = false;
        memset(s->block_status, 0, sizeof(s->block_status));
        flash_set_mode(s, FLASH_STATUS);
        break;
    case 0x97:
        if (s->operation != FLASH_OP_NONE) {
            return MEMTX_ERROR;
        }
        s->pending = FLASH_UPLOAD;
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

    if (s->blk) {
        error_report("lh28f800su: migration with persistent backing "
                     "is unsupported");
        return -EINVAL;
    }

    if (s->operation != FLASH_OP_NONE && !s->suspended) {
        s->remaining_ns = MAX((int64_t)1,
                              (int64_t)timer_expire_time_ns(s->timer) -
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return 0;
}

static int flash_pre_load(void *opaque)
{
    LH28F800SUState *s = opaque;

    if (s->blk) {
        error_report("lh28f800su: restore with persistent backing "
                     "is unsupported");
        return -EINVAL;
    }
    return 0;
}

static int flash_post_load(void *opaque, int version_id)
{
    LH28F800SUState *s = opaque;

    if (s->mode > FLASH_EXT_STATUS || s->pending > FLASH_UPLOAD ||
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
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = flash_pre_save,
    .pre_load = flash_pre_load,
    .post_load = flash_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(lock_mask, LH28F800SUState),
        VMSTATE_UINT16(displayed_unlocked_mask, LH28F800SUState),
        VMSTATE_UINT16(program_data, LH28F800SUState),
        VMSTATE_UINT8(status, LH28F800SUState),
        VMSTATE_UINT8_ARRAY(block_status, LH28F800SUState,
                            FLASH_SIZE / BLOCK_SIZE),
        VMSTATE_BOOL(gsr_error, LH28F800SUState),
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
    int64_t size;
    int ret;

    if (s->blk) {
        if (blk_get_flags(s->blk) & BDRV_O_NO_FLUSH) {
            error_setg(errp, "persistent flash requires backend flushing");
            return;
        }
        ret = blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                           BLK_PERM_CONSISTENT_READ, errp);
        if (ret < 0) {
            return;
        }
        size = blk_getlength(s->blk);
        if (size != FLASH_SIZE) {
            error_setg(errp, "persistent flash must be exactly 1 MiB");
            return;
        }
    }

    if (!memory_region_init_rom_device(&s->mem, OBJECT(dev), &flash_ops, s,
                                       TYPE_LH28F800SU, FLASH_SIZE, errp)) {
        return;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, flash_finish, s);
    s->storage = memory_region_get_ram_ptr(&s->mem);
    memset(s->storage, 0xff, FLASH_SIZE);
    if (s->blk) {
        ret = blk_pread(s->blk, 0, FLASH_SIZE, s->storage, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "cannot read persistent flash image");
            timer_free(s->timer);
            s->timer = NULL;
            object_unparent(OBJECT(&s->mem));
            return;
        }
    }
    memory_region_set_dirty(&s->mem, 0, FLASH_SIZE);
    s->lock_mask = s->initial_lock_mask;
    s->rp_high = s->initial_rp_high;
    s->status = STATUS_READY;
    s->mode = FLASH_ARRAY;
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mem);
    flash_update_outputs(s);
    if (s->blk) {
        s->shutdown.notify = flash_shutdown;
        blk_set_on_error(s->blk, BLOCKDEV_ON_ERROR_STOP,
                         BLOCKDEV_ON_ERROR_STOP);
        blk_iostatus_enable(s->blk);
        qemu_register_shutdown_notifier(&s->shutdown);
        s->shutdown_registered = true;
    }
}

static void flash_cleanup(LH28F800SUState *s)
{
    if (s->shutdown_registered) {
        notifier_remove(&s->shutdown);
        s->shutdown_registered = false;
    }
    if (s->timer) {
        timer_del(s->timer);
        timer_free(s->timer);
        s->timer = NULL;
    }
}

static void flash_unrealize(DeviceState *dev)
{
    LH28F800SUState *s = LH28F800SU(dev);

    qemu_set_irq(s->array_read, 0);
    qdev_connect_gpio_out_named(dev, "array-read", 0, NULL);
    flash_cleanup(s);
}

static void flash_finalize(Object *obj)
{
    flash_cleanup(LH28F800SU(obj));
}

static const Property flash_properties[] = {
    DEFINE_PROP_BOOL("initial-rp-high", LH28F800SUState,
                     initial_rp_high, false),
    DEFINE_PROP_UINT16("initial-lock-mask", LH28F800SUState,
                       initial_lock_mask, 0xffff),
    DEFINE_PROP_DRIVE("drive", LH28F800SUState, blk),
};

static void flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flash_realize;
    dc->unrealize = flash_unrealize;
    dc->vmsd = &vmstate_flash;
    dc->user_creatable = false;
    device_class_set_props(dc, flash_properties);
}

static void flash_instance_init(Object *obj)
{
    LH28F800SUState *s = LH28F800SU(obj);

    qdev_init_gpio_in_named(DEVICE(obj), flash_pin, "pins", 3);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ready_busy, "ready-busy", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->array_read, "array-read", 1);
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
