/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "hw/1wire/ds2502.h"
#include "hw/1wire/onewire.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi/mace-id.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace/control.h"
#include "trace.h"

QEMU_BUILD_BUG_ON(sizeof(SGIMACEIDMigrationState) != 8);

struct SGIMACEIDState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    OneWireBus *bus;
    DeviceState *part;
    qemu_irq flash_write_enable_irq;
    Error *migration_blocker;
    char *test_serial;
    char *eprom_file;
    char *eprom_hex;
    char *status_hex;
    bool password_clear_input;
    bool flash_write_enabled;
    bool released;
    uint32_t led_control;
    bool no_responder;
    bool misc_word_pio;
    void *rust_state;
};

static bool mace_id_hex(const char *text, uint8_t *out, size_t len)
{
    if (!text || strlen(text) != len * 2) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = g_ascii_xdigit_value(text[i * 2]);
        int lo = g_ascii_xdigit_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (hi << 4) | lo;
    }
    return true;
}

void sgi_mace_id_trace_drive(int64_t now, uint64_t offset, unsigned size,
                             bool release, int64_t duration)
{
    trace_mace_id_drive(now, offset, size, release, duration);
}

void sgi_mace_id_trace_reset(int64_t now, int64_t low, bool ready,
                             bool accepted)
{
    trace_mace_id_reset(now, low, ready, accepted);
}

void sgi_mace_id_trace_presence(int64_t now, int64_t start, int64_t end)
{
    trace_mace_id_presence_schedule(now, start, end);
}

void sgi_mace_id_trace_sample(int64_t now, uint64_t offset, unsigned size,
                              bool sample, bool presence)
{
    if (trace_event_get_state_backends(TRACE_MACE_ID_SAMPLE)) {
        trace_mace_id_sample(now, offset, size, sample, presence);
    }
}

void sgi_mace_id_log_pulse(int64_t low)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace-id: unsupported low pulse %" PRId64 " ns\n", low);
}

void sgi_mace_id_log_control(uint64_t bits)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace-id: unsupported identity control bits 0x%" PRIx64
                  "\n", bits);
}

void sgi_mace_id_log_misc(uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace-id: unsupported nonzero Misc high word 0x%" PRIx64
                  "\n",
                  value);
}

static MemTxResult mace_id_read(void *opaque, hwaddr offset, uint64_t *value,
                                unsigned size, MemTxAttrs attrs)
{
    SGIMACEIDState *s = opaque;
    int result = sgi_mace_id_read(s->rust_state,
                                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  offset, size, value);
    return result ? MEMTX_DECODE_ERROR : MEMTX_OK;
}

static MemTxResult mace_id_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    SGIMACEIDState *s = opaque;
    int result = sgi_mace_id_write(s->rust_state,
                                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                   offset, size, value);
    if (result == SGI_MACE_ID_WRITE_NOOP) {
        return MEMTX_OK;
    }
    if (result == SGI_MACE_ID_WRITE_DECODE_ERROR) {
        return MEMTX_DECODE_ERROR;
    }
    qemu_set_irq(s->flash_write_enable_irq, result);
    return MEMTX_OK;
}

static const MemoryRegionOps mace_id_ops = {
    .read_with_attrs = mace_id_read,
    .write_with_attrs = mace_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void mace_id_reset_hold(Object *obj, ResetType type)
{
    SGIMACEIDState *s = SGI_MACE_ID(obj);
    qemu_set_irq(s->flash_write_enable_irq,
                 sgi_mace_id_reset(s->rust_state));
}

static void mace_id_password_clear_input(void *opaque, int n, int level)
{
    SGIMACEIDState *s = opaque;
    s->password_clear_input = !!level;
    if (s->rust_state) {
        sgi_mace_id_set_input(s->rust_state, s->password_clear_input);
    }
}

static bool mace_id_create_responder(SGIMACEIDState *s, Error **errp)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) io_error = NULL;
    uint8_t image[DS2502_DATA_SIZE];
    uint8_t serial_bytes[6];
    uint8_t status[DS2502_STATUS_SIZE];
    uint64_t serial = 0;
    DeviceState *part;
    GStatBuf image_stat;

    if (s->no_responder) {
        if (s->test_serial || s->eprom_file || s->eprom_hex || s->status_hex) {
            error_setg(errp,
                       "sgi-mace-id no-responder rejects synthetic identity "
                       "inputs");
            return false;
        }
        return true;
    }
    if (!mace_id_hex(s->test_serial, serial_bytes, sizeof(serial_bytes)) ||
        !mace_id_hex(s->status_hex, status, sizeof(status)) ||
        (!!s->eprom_file == !!s->eprom_hex)) {
        error_setg(errp,
                   "sgi-mace-id needs 12 hexadecimal test-serial digits, 14 "
                   "hexadecimal status digits and one 128-byte image, as "
                   "eprom-file or as 256 hexadecimal eprom-hex digits");
        return false;
    }
    if (s->eprom_hex) {
        if (!mace_id_hex(s->eprom_hex, image, sizeof(image))) {
            error_setg(errp, "sgi-mace-id eprom-hex needs exactly 256 "
                             "hexadecimal digits");
            return false;
        }
    } else {
        gsize length;
        if (g_stat(s->eprom_file, &image_stat) < 0) {
            error_setg_errno(errp, errno,
                             "sgi-mace-id cannot stat eprom-file '%s'",
                             s->eprom_file);
            return false;
        }
        if (!S_ISREG(image_stat.st_mode) ||
            image_stat.st_size != DS2502_DATA_SIZE) {
            error_setg(errp, "sgi-mace-id eprom-file needs exactly 128 "
                             "bytes in a regular file");
            return false;
        }
        if (!g_file_get_contents(s->eprom_file, &contents, &length,
                                 &io_error)) {
            error_setg(errp, "sgi-mace-id cannot read eprom-file: %s",
                       io_error->message);
            return false;
        }
        if (length != DS2502_DATA_SIZE) {
            error_setg(errp, "sgi-mace-id eprom-file needs exactly 128 bytes");
            return false;
        }
        memcpy(image, contents, sizeof(image));
    }
    for (unsigned i = 0; i < sizeof(serial_bytes); i++) {
        serial |= (uint64_t)serial_bytes[i] << (8 * i);
    }
    part = qdev_new(TYPE_ONEWIRE_DS2502);
    object_property_add_child(OBJECT(s), "ds2502", OBJECT(part));
    qdev_prop_set_uint8(part, "family", 0x09);
    qdev_prop_set_uint64(part, "serial", serial);
    qdev_prop_set_bit(part, "strict-readonly-diagnostic", true);
    ds2502_set_image(part, image, sizeof(image));
    if (!ds2502_set_status(part, status, sizeof(status), errp)) {
        object_unparent(OBJECT(part));
        object_unref(OBJECT(part));
        return false;
    }
    if (!qdev_realize_and_unref(part, BUS(s->bus), errp)) {
        object_unparent(OBJECT(part));
        return false;
    }
    s->part = part;
    error_setg(&s->migration_blocker,
               "O2 identity diagnostic does not support migration or "
               "snapshots");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        migrate_del_blocker(&s->migration_blocker);
        qdev_unrealize(part);
        object_unparent(OBJECT(part));
        s->part = NULL;
        return false;
    }
    return true;
}

static void mace_id_realize(DeviceState *dev, Error **errp)
{
    SGIMACEIDState *s = SGI_MACE_ID(dev);
    if (!mace_id_create_responder(s, errp)) {
        return;
    }
    s->rust_state = sgi_mace_id_new(s->bus, s->part != NULL,
                                    s->misc_word_pio,
                                    s->password_clear_input);
}

static void mace_id_unrealize(DeviceState *dev)
{
    SGIMACEIDState *s = SGI_MACE_ID(dev);
    if (s->rust_state) {
        sgi_mace_id_free(s->rust_state);
        s->rust_state = NULL;
    }
    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
    if (s->part) {
        qdev_unrealize(s->part);
        object_unparent(OBJECT(s->part));
        s->part = NULL;
    }
}

static void mace_id_init(Object *obj)
{
    SGIMACEIDState *s = SGI_MACE_ID(obj);
    memory_region_init_io(&s->mmio, obj, &mace_id_ops, s, "sgi-mace-id", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->flash_write_enable_irq,
                             "flash-write-enable", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_id_password_clear_input,
                            "password-clear-input", 1);
    s->bus = onewire_create_bus(DEVICE(obj), "onewire");
}

static int mace_id_pre_save(void *opaque)
{
    SGIMACEIDState *s = opaque;
    SGIMACEIDMigrationState state;
    sgi_mace_id_export(s->rust_state, &state);
    s->password_clear_input = state.password_clear_input;
    s->flash_write_enabled = state.flash_write_enabled;
    s->released = state.released;
    s->led_control = state.led_control;
    return 0;
}

static int mace_id_post_load(void *opaque, int version_id)
{
    SGIMACEIDState *s = opaque;
    SGIMACEIDMigrationState state = {
        .password_clear_input = s->password_clear_input,
        .flash_write_enabled = s->flash_write_enabled,
        .released = s->released,
        .led_control = s->led_control,
    };
    qemu_set_irq(s->flash_write_enable_irq,
                 sgi_mace_id_import(s->rust_state, &state));
    return 0;
}

static const VMStateDescription vmstate_mace_id = {
    .name = TYPE_SGI_MACE_ID,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = mace_id_pre_save,
    .post_load = mace_id_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(password_clear_input, SGIMACEIDState),
        VMSTATE_BOOL(flash_write_enabled, SGIMACEIDState),
        VMSTATE_BOOL(released, SGIMACEIDState),
        VMSTATE_UINT32(led_control, SGIMACEIDState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mace_id_properties[] = {
    DEFINE_PROP_BOOL("password-clear-input", SGIMACEIDState,
                     password_clear_input, false),
    DEFINE_PROP_BOOL("misc-word-pio", SGIMACEIDState, misc_word_pio, false),
    DEFINE_PROP_BOOL("no-responder", SGIMACEIDState, no_responder, false),
    DEFINE_PROP_STRING("test-serial", SGIMACEIDState, test_serial),
    DEFINE_PROP_STRING("eprom-file", SGIMACEIDState, eprom_file),
    DEFINE_PROP_STRING("eprom-hex", SGIMACEIDState, eprom_hex),
    DEFINE_PROP_STRING("status", SGIMACEIDState, status_hex),
};

static void mace_id_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    dc->desc = "Synthetic DS2502 on the O2 MACE identity line";
    dc->realize = mace_id_realize;
    dc->unrealize = mace_id_unrealize;
    dc->vmsd = &vmstate_mace_id;
    dc->user_creatable = true;
    device_class_set_props(dc, mace_id_properties);
    rc->phases.hold = mace_id_reset_hold;
}

static const TypeInfo mace_id_info = {
    .name = TYPE_SGI_MACE_ID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIMACEIDState),
    .instance_init = mace_id_init,
    .class_init = mace_id_class_init,
};

static void mace_id_register_types(void)
{
    type_register_static(&mace_id_info);
}

type_init(mace_id_register_types)
