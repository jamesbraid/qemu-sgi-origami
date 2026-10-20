/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Optional synthetic DS2502 on the MACE 2.0 identity line. */

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

#define MACE_ID_FLASH_ENABLE 1U
#define MACE_ID_RELEASE (1U << 2)
#define MACE_ID_SAMPLE (1U << 3)
#define MACE_ID_RED_LED (1U << 4)
#define MACE_ID_GREEN_LED (1U << 5)
#define MACE_ID_WRITABLE (MACE_ID_FLASH_ENABLE | MACE_ID_RELEASE | \
                          MACE_ID_RED_LED | \
                          MACE_ID_GREEN_LED)
#define MACE_ID_RESET_MIN_NS (480 * SCALE_US)
#define MACE_ID_RESET_MAX_NS (960 * SCALE_US)
#define MACE_ID_PRESENCE_DELAY_NS (30 * SCALE_US)
#define MACE_ID_PRESENCE_END_NS (150 * SCALE_US)
/* The synthetic responder accepts wider lows than the physical DS2502. */
#define MACE_ID_DIAG_ONE_MAX_NS (24 * SCALE_US)
#define MACE_ID_SLOT_NS (60 * SCALE_US)
#define MACE_ID_WRITE_ZERO_NS (60 * SCALE_US)
#define MACE_ID_SLOT_MAX_NS (120 * SCALE_US)

struct SGIMACEIDState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    OneWireBus *bus;
    DeviceState *part;
    qemu_irq flash_write_enable_irq;
    Error *migration_blocker;
    char *test_serial;
    char *eprom_file;
    char *status_hex;
    int64_t low_start_ns;
    int64_t high_start_ns;
    int64_t presence_start_ns;
    int64_t presence_end_ns;
    int64_t sample_start_ns;
    int64_t sample_end_ns;
    bool released;
    bool flash_write_enabled;
    bool no_responder;
    bool misc_word_pio;
    uint32_t led_control;
    bool high_seen;
    bool reset_ready;
    bool session;
    bool slot_pull;
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

static void mace_id_clear_windows(SGIMACEIDState *s)
{
    s->presence_start_ns = 0;
    s->presence_end_ns = 0;
    s->sample_start_ns = 0;
    s->sample_end_ns = 0;
    s->slot_pull = false;
}

static bool mace_id_presence(SGIMACEIDState *s, int64_t now)
{
    return s->session && now >= s->presence_start_ns &&
           now < s->presence_end_ns;
}

static bool mace_id_sample_pull(SGIMACEIDState *s, int64_t now)
{
    return s->slot_pull && now >= s->sample_start_ns &&
           now < s->sample_end_ns;
}

static uint64_t mace_id_value(SGIMACEIDState *s, int64_t now)
{
    bool level = s->part && s->released && !mace_id_presence(s, now) &&
                 !mace_id_sample_pull(s, now);

    return s->led_control |
           (s->flash_write_enabled ? MACE_ID_FLASH_ENABLE : 0) |
           (s->released ? MACE_ID_RELEASE : 0) |
           (level ? MACE_ID_SAMPLE : 0);
}

static void mace_id_trace_sample(SGIMACEIDState *s, int64_t now,
                                 hwaddr offset, unsigned size, uint64_t value)
{
    if (trace_event_get_state_backends(TRACE_MACE_ID_SAMPLE)) {
        trace_mace_id_sample(now, offset, size,
                             !!(value & MACE_ID_SAMPLE),
                             mace_id_presence(s, now));
    }
}

static void mace_id_write_control(SGIMACEIDState *s, uint64_t value,
                                  hwaddr offset, unsigned size)
{
    bool release = value & MACE_ID_RELEASE;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t low_ns;

    if (value & ~MACE_ID_WRITABLE) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace-id: unsupported identity control bits "
                      "0x%" PRIx64 "\n", value & ~MACE_ID_WRITABLE);
    }
    s->led_control = value & (MACE_ID_RED_LED | MACE_ID_GREEN_LED);
    s->flash_write_enabled = value & MACE_ID_FLASH_ENABLE;
    qemu_set_irq(s->flash_write_enable_irq, s->flash_write_enabled);
    if (release == s->released) {
        return;
    }
    trace_mace_id_drive(now, offset, size, release,
                        release ? (s->high_seen ? now - s->low_start_ns : -1) :
                        now - s->high_start_ns);
    s->released = release;
    if (!release) {
        s->reset_ready = s->high_seen &&
                         now - s->high_start_ns >= MACE_ID_RESET_MIN_NS;
        s->low_start_ns = now;
        return;
    }

    s->high_start_ns = now;
    if (!s->high_seen) {
        s->high_seen = true;
        return;
    }
    low_ns = now - s->low_start_ns;
    trace_mace_id_reset(now, low_ns, s->reset_ready,
                        low_ns >= MACE_ID_RESET_MIN_NS &&
                        low_ns < MACE_ID_RESET_MAX_NS && s->reset_ready);
    if (low_ns >= MACE_ID_RESET_MIN_NS) {
        mace_id_clear_windows(s);
        s->session = false;
        onewire_reset(s->bus);
        if (low_ns < MACE_ID_RESET_MAX_NS && s->reset_ready) {
            s->session = true;
            s->presence_start_ns = now + MACE_ID_PRESENCE_DELAY_NS;
            s->presence_end_ns = now + MACE_ID_PRESENCE_END_NS;
            trace_mace_id_presence_schedule(now, s->presence_start_ns,
                                            s->presence_end_ns);
        }
        return;
    }
    if (!s->session || now < s->presence_end_ns) {
        return;
    }
    if (low_ns >= MACE_ID_WRITE_ZERO_NS &&
        low_ns < MACE_ID_SLOT_MAX_NS) {
        onewire_touch_bit(s->bus, false);
    } else if (low_ns >= SCALE_US &&
               low_ns < MACE_ID_DIAG_ONE_MAX_NS) {
        s->slot_pull = !onewire_touch_bit(s->bus, true);
        s->sample_start_ns = now;
        s->sample_end_ns = s->low_start_ns + MACE_ID_SLOT_NS;
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace-id: unsupported low pulse %" PRId64 " ns\n",
                      low_ns);
    }
}

static MemTxResult mace_id_read(void *opaque, hwaddr offset,
                                 uint64_t *value, unsigned size,
                                 MemTxAttrs attrs)
{
    SGIMACEIDState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    *value = 0;
    if (size == 8 && offset == 0) {
        *value = mace_id_value(s, now);
        mace_id_trace_sample(s, now, offset, size, *value);
        return MEMTX_OK;
    }
    if (size == 4 && s->misc_word_pio) {
        if (offset == 0) {
            mace_id_trace_sample(s, now, offset, size, *value);
            return MEMTX_OK;
        }
        if (offset == 4) {
            *value = mace_id_value(s, now) & UINT32_MAX;
            mace_id_trace_sample(s, now, offset, size, *value);
            return MEMTX_OK;
        }
    }
    return MEMTX_DECODE_ERROR;
}

static MemTxResult mace_id_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIMACEIDState *s = opaque;

    if (size == 8 && offset == 0) {
        mace_id_write_control(s, value, offset, size);
        return MEMTX_OK;
    }
    if (size == 4 && s->misc_word_pio) {
        if (offset == 0) {
            if (value) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-id: unsupported nonzero Misc high "
                              "word 0x%" PRIx64 "\n", value);
            }
            return MEMTX_OK;
        }
        if (offset == 4) {
            mace_id_write_control(s, value, offset, size);
            return MEMTX_OK;
        }
    }
    return MEMTX_DECODE_ERROR;
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

    mace_id_clear_windows(s);
    s->released = false;
    s->flash_write_enabled = false;
    qemu_set_irq(s->flash_write_enable_irq, 0);
    s->led_control = MACE_ID_GREEN_LED;
    s->high_seen = false;
    s->reset_ready = false;
    s->session = false;
    s->low_start_ns = 0;
    s->high_start_ns = 0;
    onewire_reset(s->bus);
}

static void mace_id_realize(DeviceState *dev, Error **errp)
{
    SGIMACEIDState *s = SGI_MACE_ID(dev);
    g_autofree char *contents = NULL;
    g_autoptr(GError) io_error = NULL;
    uint8_t serial_bytes[6];
    uint8_t status[DS2502_STATUS_SIZE];
    uint64_t serial = 0;
    size_t length;
    DeviceState *part;
    struct stat image_stat;

    if (s->no_responder) {
        if (s->test_serial || s->eprom_file || s->status_hex) {
            error_setg(errp, "sgi-mace-id no-responder rejects synthetic "
                             "identity inputs");
        }
        return;
    }
    if (!mace_id_hex(s->test_serial, serial_bytes, sizeof(serial_bytes)) ||
        !mace_id_hex(s->status_hex, status, sizeof(status)) ||
        !s->eprom_file) {
        error_setg(errp, "sgi-mace-id needs 12 hexadecimal test-serial "
                         "digits, 14 hexadecimal status digits and a "
                         "128-byte eprom-file");
        return;
    }
    if (g_stat(s->eprom_file, &image_stat) < 0) {
        error_setg_errno(errp, errno,
                         "sgi-mace-id cannot stat eprom-file '%s'",
                         s->eprom_file);
        return;
    }
    if (!S_ISREG(image_stat.st_mode) ||
        image_stat.st_size != DS2502_DATA_SIZE) {
        error_setg(errp, "sgi-mace-id eprom-file needs exactly 128 bytes "
                         "in a regular file");
        return;
    }
    if (!g_file_get_contents(s->eprom_file, &contents, &length, &io_error)) {
        error_setg(errp, "sgi-mace-id cannot read eprom-file: %s",
                   io_error->message);
        return;
    }
    if (length != DS2502_DATA_SIZE) {
        error_setg(errp, "sgi-mace-id eprom-file needs exactly 128 bytes");
        return;
    }
    for (unsigned i = 0; i < sizeof(serial_bytes); i++) {
        serial |= (uint64_t)serial_bytes[i] << (8 * i);
    }
    part = qdev_new(TYPE_ONEWIRE_DS2502);
    object_property_add_child(OBJECT(dev), "ds2502", OBJECT(part));
    qdev_prop_set_uint8(part, "family", 0x09);
    qdev_prop_set_uint64(part, "serial", serial);
    qdev_prop_set_bit(part, "strict-readonly-diagnostic", true);
    ds2502_set_image(part, (const uint8_t *)contents, length);
    if (!ds2502_set_status(part, status, sizeof(status), errp)) {
        object_unparent(OBJECT(part));
        object_unref(OBJECT(part));
        return;
    }
    if (!qdev_realize_and_unref(part, BUS(s->bus), errp)) {
        object_unparent(OBJECT(part));
        return;
    }
    s->part = part;
    error_setg(&s->migration_blocker,
               "O2 identity diagnostic does not support migration or snapshots");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        migrate_del_blocker(&s->migration_blocker);
        qdev_unrealize(part);
        object_unparent(OBJECT(part));
        s->part = NULL;
    }
}

static void mace_id_unrealize(DeviceState *dev)
{
    SGIMACEIDState *s = SGI_MACE_ID(dev);

    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
    if (s->part) {
        qdev_unrealize(s->part);
        object_unparent(OBJECT(s->part));
        s->part = NULL;
    }
    mace_id_clear_windows(s);
}

static void mace_id_init(Object *obj)
{
    SGIMACEIDState *s = SGI_MACE_ID(obj);

    memory_region_init_io(&s->mmio, obj, &mace_id_ops, s,
                          "sgi-mace-id", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->flash_write_enable_irq,
                             "flash-write-enable", 1);
    s->bus = onewire_create_bus(DEVICE(obj), "onewire");
}

static int mace_id_post_load(void *opaque, int version_id)
{
    SGIMACEIDState *s = opaque;

    qemu_set_irq(s->flash_write_enable_irq, s->flash_write_enabled);
    return 0;
}

static const VMStateDescription vmstate_mace_id = {
    .name = TYPE_SGI_MACE_ID,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mace_id_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(flash_write_enabled, SGIMACEIDState),
        VMSTATE_BOOL(released, SGIMACEIDState),
        VMSTATE_UINT32(led_control, SGIMACEIDState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mace_id_properties[] = {
    DEFINE_PROP_BOOL("misc-word-pio", SGIMACEIDState, misc_word_pio, false),
    DEFINE_PROP_BOOL("no-responder", SGIMACEIDState, no_responder, false),
    DEFINE_PROP_STRING("test-serial", SGIMACEIDState, test_serial),
    DEFINE_PROP_STRING("eprom-file", SGIMACEIDState, eprom_file),
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
