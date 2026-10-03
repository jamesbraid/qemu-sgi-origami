/* SPDX-License-Identifier: BSD-3-Clause */
/* Isolated backing-state and transaction faults for the real KONA device. */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-kona.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/qtest.h"
#include "kona-read-fixture.h"

OBJECT_DECLARE_SIMPLE_TYPE(KonaReadFixture, KONA_READ_FIXTURE)

struct KonaReadFixture {
    SysBusDevice parent_obj;
    SGIKonaState *source;
    SGIKonaState *destination;
    MemoryRegion control;
    MemoryRegion commands;
    MemoryRegion fabric;
    MemoryRegion page;
    MemoryRegion page_view;
    MemoryRegion *mapped_face;
    uint8_t bytes[FIXTURE_PAGE_BYTES];
    bool ready_fault;
    uint64_t payload_bytes;
    uint64_t payload_writes;
    uint64_t ready_writes;
    uint64_t failed_ready;
    uint64_t widths;
    uint64_t sequence;
    uint64_t ready_sequence;
    uint64_t last_payload;
    int save_result;
    int result;
    bool load_error;
};

static MemTxResult page_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    KonaReadFixture *s = opaque;

    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        *value = (*value << 8) | s->bytes[offset + i];
    }
    return MEMTX_OK;
}

static MemTxResult page_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    KonaReadFixture *s = opaque;

    if (!qtest_driver()) {
        return MEMTX_ERROR;
    }
    s->sequence++;
    s->widths |= 1ULL << size;
    if (offset == 0) {
        s->ready_writes++;
        s->ready_sequence = s->sequence;
        if (s->ready_fault) {
            s->failed_ready++;
            return MEMTX_ERROR;
        }
    } else {
        s->payload_bytes += size;
        s->payload_writes++;
        s->last_payload = offset;
    }
    for (unsigned i = 0; i < size; i++) {
        s->bytes[offset + i] = value >> (8 * (size - 1 - i));
    }
    return MEMTX_OK;
}

static const MemoryRegionOps page_ops = {
    .read_with_attrs = page_read,
    .write_with_attrs = page_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void discard_destination(KonaReadFixture *s)
{
    if (s->destination) {
        qdev_unrealize(DEVICE(s->destination));
        object_unparent(OBJECT(s->destination));
        object_unref(OBJECT(s->destination));
        s->destination = NULL;
    }
}

static SGIKonaState *new_kona(KonaReadFixture *s, unsigned module, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_SGI_KONA);

    /* The XIO base class places the child under its module at realize. */
    qdev_prop_set_uint32(dev, "module", module);
    qdev_prop_set_string(dev, "slot", "11");
    qdev_prop_set_uint32(dev, "fabric-hub-widget", 9);
    object_property_set_link(OBJECT(dev), "fabric", OBJECT(&s->fabric), errp);
    if (*errp || !sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        object_unparent(OBJECT(dev));
        object_unref(OBJECT(dev));
        return NULL;
    }
    /* Retain the creator reference across the XIO base class re-parent. */
    return SGI_KONA(dev);
}

static void map_commands(KonaReadFixture *s, SGIKonaState *kona)
{
    if (s->mapped_face) {
        memory_region_del_subregion(&s->commands, s->mapped_face);
    }
    s->mapped_face = SGI_XIO_BOARD(kona)->cpu_face;
    memory_region_add_subregion(&s->commands, 0, s->mapped_face);
}

static void roundtrip(KonaReadFixture *s, unsigned request)
{
    SGIKonaState *k = s->source;
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(k)->vmsd;
    unsigned kind = request & 0xff;
    unsigned flags = request >> 8;
    int current = k->context_current;
    QIOChannelBuffer *output, *input;
    QEMUFile *file;
    Error *err = NULL;

    assert(current >= 0 && current < SGI_KONA_CONTEXTS);
    k->x_read_gc_valid = flags & 1;
    k->x_read_setup_valid = flags & 2;
    switch (kind) {
    case FIXTURE_NO_CURRENT:
        k->context_current = SGI_KONA_CONTEXT_NONE;
        break;
    case FIXTURE_INVALID_CURRENT:
        k->context_current = SGI_KONA_CONTEXTS;
        break;
    case FIXTURE_INVALID_CONTEXT:
        k->contexts[current].valid = false;
        break;
    case FIXTURE_INVALID_STORED_PAGE:
        k->contexts[current].return_page_valid = false;
        break;
    case FIXTURE_INVALID_ACTIVE_PAGE:
        k->return_page_valid = false;
        break;
    case FIXTURE_MISMATCHED_PAGE:
    case FIXTURE_FALSE_MISMATCH:
        k->return_page += FIXTURE_PAGE_BYTES;
        break;
    case FIXTURE_ABSENT_FLAGS:
    case FIXTURE_VALID_STATE:
        break;
    default:
        g_assert_not_reached();
    }

    /* Failed loads never contaminate the next destination. */
    map_commands(s, k);
    discard_destination(s);
    s->destination = new_kona(s, 1, &err);
    if (!s->destination) {
        s->result = -EINVAL;
        s->load_error = true;
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", TYPE_KONA_READ_FIXTURE,
                      error_get_pretty(err));
        error_free(err);
        return;
    }
    output = qio_channel_buffer_new(4096);
    file = qemu_file_new_output(QIO_CHANNEL(output));
    s->save_result = vmstate_save_state(file, vmsd, k, NULL, &err);
    /* A following section marker lets subsection loading peek safely. */
    qemu_put_byte(file, 0);
    qemu_fflush(file);
    input = qio_channel_buffer_new(output->usage);
    memcpy(input->data, output->data, output->usage);
    input->usage = output->usage;
    qemu_fclose(file);
    if (s->save_result == 0) {
        file = qemu_file_new_input(QIO_CHANNEL(input));
        s->result = vmstate_load_state(file, vmsd, s->destination,
                                       vmsd->version_id, &err);
        qemu_fclose(file);
    } else {
        s->result = s->save_result;
    }
    s->load_error = err != NULL;
    if (err) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", TYPE_KONA_READ_FIXTURE,
                      error_get_pretty(err));
        error_free(err);
    }
    object_unref(OBJECT(input));
    object_unref(OBJECT(output));
    if (!s->result) {
        map_commands(s, s->destination);
    } else {
        discard_destination(s);
    }
}

static uint64_t control_read(void *opaque, hwaddr offset, unsigned size)
{
    KonaReadFixture *s = opaque;
    SGIKonaState *k = s->destination ? s->destination : s->source;

    switch (offset) {
    case FIXTURE_RESULT: return (int64_t)s->result;
    case FIXTURE_ERROR: return s->load_error;
    case FIXTURE_SAVE_RESULT: return (int64_t)s->save_result;
    case FIXTURE_PAYLOAD_BYTES: return s->payload_bytes;
    case FIXTURE_PAYLOAD_WRITES: return s->payload_writes;
    case FIXTURE_READY_WRITES: return s->ready_writes;
    case FIXTURE_FAILED_READY: return s->failed_ready;
    case FIXTURE_WIDTHS: return s->widths;
    case FIXTURE_READY_SEQUENCE: return s->ready_sequence;
    case FIXTURE_LAST_PAYLOAD: return s->last_payload;
    case FIXTURE_FLAGS:
        return k->x_read_gc_valid | (k->x_read_setup_valid << 1);
    default: return 0;
    }
}

static void control_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    KonaReadFixture *s = opaque;
    SGIKonaState *k = s->source;

    if (!qtest_driver()) {
        return;
    }
    switch (offset) {
    case FIXTURE_PREPARE:
        memset(s->bytes, 0xa5, sizeof(s->bytes));
        memset(s->bytes, 0, 4);
        s->payload_bytes = s->payload_writes = 0;
        s->ready_writes = s->failed_ready = 0;
        s->widths = s->sequence = s->ready_sequence = 0;
        s->last_payload = 0;
        break;
    case FIXTURE_MUTATE_PIXEL:
        sgi_plane_store(&k->mem,
                       sgi_board_memory_plane(&k->mem, k->plane_front),
                       459, 322, value == FIXTURE_PIXEL_LARGE_INDEX ?
                                 0x100 : 0x7e);
        sgi_plane_store(&k->mem,
                       sgi_board_memory_plane(&k->mem, k->plane_front_tag),
                       459, 322, value == FIXTURE_PIXEL_UNKNOWN_TAG ? 99 : 2);
        break;
    case FIXTURE_READY_FAULT:
        s->ready_fault = value;
        break;
    case FIXTURE_MIGRATE:
        roundtrip(s, value);
        break;
    }
}

static const MemoryRegionOps control_ops = {
    .read = control_read,
    .write = control_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void fixture_realize(DeviceState *dev, Error **errp)
{
    KonaReadFixture *s = KONA_READ_FIXTURE(dev);
    Error *err = NULL;

    if (!qtest_driver()) {
        error_setg(errp, "%s requires qtest", TYPE_KONA_READ_FIXTURE);
        return;
    }
    memory_region_init(&s->fabric, OBJECT(s), "kona-test.fabric", 1ULL << 52);
    memory_region_init_io(&s->page, OBJECT(s), &page_ops, s,
                          "kona-test.page", FIXTURE_PAGE_BYTES);
    memory_region_add_subregion(&s->fabric, (9ULL << 48) | FIXTURE_PAGE,
                                 &s->page);
    memory_region_init(&s->commands, OBJECT(s), "kona-test.commands",
                       SGI_KONA_APERTURE_SIZE);
    memory_region_init_io(&s->control, OBJECT(s), &control_ops, s,
                          "kona-test.control", 128);
    s->control.disable_reentrancy_guard = true;
    s->source = new_kona(s, 0, &err);
    if (!s->source) {
        error_propagate(errp, err);
        return;
    }
    map_commands(s, s->source);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->control);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->commands);
    memory_region_init_alias(&s->page_view, OBJECT(s), "kona-test.page-view",
                             &s->page, 0, FIXTURE_PAGE_BYTES);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->page_view);
}

static void fixture_unrealize(DeviceState *dev)
{
    KonaReadFixture *s = KONA_READ_FIXTURE(dev);

    memory_region_del_subregion(&s->commands, s->mapped_face);
    s->mapped_face = NULL;
    discard_destination(s);
    qdev_unrealize(DEVICE(s->source));
    object_unparent(OBJECT(s->source));
    object_unref(OBJECT(s->source));
    s->source = NULL;
}

static void fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fixture_realize;
    dc->unrealize = fixture_unrealize;
    dc->desc = "KONA indexed-read defensive qtest fixture";
    dc->user_creatable = true;
}

static const TypeInfo fixture_type = {
    .name = TYPE_KONA_READ_FIXTURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KonaReadFixture),
    .class_init = fixture_class_init,
};

static void fixture_register_types(void)
{
    type_register_static(&fixture_type);
}
type_init(fixture_register_types)
