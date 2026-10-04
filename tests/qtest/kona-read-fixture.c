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
    uint64_t dma_io;
    uint64_t dma_pitch;
    uint64_t dma_reads;
    bool dma_result;
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
    uint8_t *layout_stream;
    size_t layout_stream_len;
    uint8_t *layout_prefix;
    uint32_t layout_prefix_len;
    uint64_t layout_old_size;
    uint64_t layout_size;
    uint32_t layout_planes;
    bool layout_match;
    int layout_marker;
    int layout_stream_error;
    size_t layout_eof_bytes;
    int layout_eof_error;
};

static MemTxResult page_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    KonaReadFixture *s = opaque;

    s->dma_reads++;
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

static void roundtrip_state(KonaReadFixture *s)
{
    SGIKonaState *k = s->source;
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(k)->vmsd;
    QIOChannelBuffer *output, *input;
    QEMUFile *file;
    Error *err = NULL;

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

static void roundtrip(KonaReadFixture *s, unsigned request)
{
    SGIKonaState *k = s->source;
    unsigned kind = request & 0xff;
    unsigned flags = request >> 8;
    int current = k->context_current;

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

    roundtrip_state(s);
}

typedef struct LayoutIdentity {
    uint64_t size;
    uint32_t bytes_len;
    uint32_t nplanes;
} LayoutIdentity;

static LayoutIdentity layout_project(SGIBoardMemory *mem, uint64_t old_size)
{
    LayoutIdentity saved = { mem->size, mem->bytes_len, mem->nplanes };

    mem->size = old_size;
    mem->bytes_len = old_size;
    mem->nplanes = 6;
    return saved;
}

static void layout_restore(SGIBoardMemory *mem, LayoutIdentity saved)
{
    mem->size = saved.size;
    mem->bytes_len = saved.bytes_len;
    mem->nplanes = saved.nplanes;
}

/* Bind the projection to the six-plane layout before stencil was appended. */
static uint64_t layout_check(SGIKonaState *k)
{
    const uint64_t pixels = (uint64_t)SGI_KONA_FB_WIDTH * SGI_KONA_FB_HEIGHT;
    const uint64_t lengths[] = {
        4 * pixels, 4 * pixels, pixels, pixels, k->list_memory_size,
        4 * pixels, pixels,
    };
    const SGIPlaneFormat formats[] = {
        SGI_PLANE_RGB24, SGI_PLANE_RGB24, SGI_PLANE_TAG, SGI_PLANE_TAG,
        SGI_PLANE_RAW, SGI_PLANE_Z24, SGI_PLANE_S8,
    };
    const uint32_t indices[] = {
        k->plane_front, k->plane_back, k->plane_front_tag, k->plane_back_tag,
        k->plane_list, k->plane_depth, k->plane_stencil,
    };
    uint64_t base = 0, old_size = 14 * pixels + k->list_memory_size;

    g_assert_cmpuint(k->mem.nplanes, ==, 7);
    g_assert_cmpuint(k->mem.npages, ==, 0);
    g_assert_cmpuint(k->mem.size, ==, old_size + pixels);
    g_assert_cmpuint(k->mem.bytes_len, ==, k->mem.size);
    for (unsigned i = 0; i < ARRAY_SIZE(indices); i++) {
        const SGIPlane *p = sgi_board_memory_plane(&k->mem, indices[i]);
        bool list = i == 4;
        unsigned bpp = i == 0 || i == 1 || i == 5 ? 4 : 1;
        SGIPacking packing = list ? SGI_PACK_BE_RED_LOW :
            i < 2 ? SGI_PACK_LE_RED_HIGH : SGI_PACK_LE_RED_LOW;

        g_assert_cmpuint(indices[i], ==, i);
        g_assert_nonnull(p);
        g_assert_cmpuint(p->base, ==, base);
        g_assert_cmpuint(p->len, ==, lengths[i]);
        g_assert_cmpuint(p->format, ==, formats[i]);
        g_assert_cmpuint(p->packing, ==, packing);
        g_assert_cmpuint(p->width, ==, list ?
                         SGI_KONA_LIST_ROW_BYTES / 4 : SGI_KONA_FB_WIDTH);
        g_assert_cmpuint(p->height, ==, list ?
                         k->list_memory_size / SGI_KONA_LIST_ROW_BYTES :
                         SGI_KONA_FB_HEIGHT);
        g_assert_cmpuint(p->stride, ==, list ?
                         SGI_KONA_LIST_ROW_BYTES : SGI_KONA_FB_WIDTH * bpp);
        g_assert_cmpint(p->bottom_origin, ==, !list);
        if (i == 6) {
            g_assert_cmpuint(base, ==, old_size);
        }
        base += lengths[i];
    }
    g_assert_cmpuint(base, ==, k->mem.size);
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        g_assert_false(k->gl_pipe[i].draw_config_received);
    }
    g_assert_cmpuint(k->gl_data_remaining, ==, 0);
    return old_size;
}

static void layout_save(KonaReadFixture *s, bool old)
{
    SGIKonaState *k = s->source;
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(k)->vmsd;
    LayoutIdentity saved = { 0 };
    QIOChannelBuffer *output;
    QEMUFile *file;
    Error *err = NULL;

    map_commands(s, k);
    discard_destination(s);
    s->layout_old_size = layout_check(k);
    k->mem.bytes[0] = 0x31;
    k->mem.bytes[s->layout_old_size - 1] = 0x97;
    k->mem.bytes[k->mem.size - 1] = 0xc5;
    g_free(s->layout_stream);
    g_free(s->layout_prefix);
    s->layout_prefix_len = old ? s->layout_old_size : k->mem.bytes_len;
    s->layout_prefix = g_memdup2(k->mem.bytes, s->layout_prefix_len);
    output = qio_channel_buffer_new(4096);
    file = qemu_file_new_output(QIO_CHANNEL(output));
    if (old) {
        saved = layout_project(&k->mem, s->layout_old_size);
    }
    s->save_result = vmstate_save_state(file, vmsd, k, NULL, &err);
    if (old) {
        layout_restore(&k->mem, saved);
    }
    qemu_put_byte(file, 0);
    qemu_fflush(file);
    if (!s->save_result) {
        s->save_result = qemu_file_get_error(file);
    }
    /* Closing a QIOChannelBuffer frees its data, so retain bytes first. */
    s->layout_stream_len = output->usage;
    s->layout_stream = g_memdup2(output->data, output->usage);
    qemu_fclose(file);
    object_unref(OBJECT(output));
    s->load_error = err != NULL;
    if (err) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", TYPE_KONA_READ_FIXTURE,
                      error_get_pretty(err));
        error_free(err);
    }
}

static void layout_load(KonaReadFixture *s, bool old)
{
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(s->source)->vmsd;
    LayoutIdentity saved = { 0 };
    QIOChannelBuffer *input;
    QEMUFile *file;
    Error *err = NULL;
    uint8_t extra;

    s->layout_match = false;
    s->layout_marker = -1;
    s->layout_stream_error = s->layout_eof_error = 0;
    s->layout_eof_bytes = 1;
    map_commands(s, s->source);
    discard_destination(s);
    s->destination = new_kona(s, 1, &err);
    if (!s->destination) {
        s->result = -EINVAL;
        goto done;
    }
    g_assert_nonnull(s->layout_stream);
    g_assert_cmpint(s->save_result, ==, 0);
    layout_check(s->destination);
    input = qio_channel_buffer_new(s->layout_stream_len);
    memcpy(input->data, s->layout_stream, s->layout_stream_len);
    input->usage = s->layout_stream_len;
    file = qemu_file_new_input(QIO_CHANNEL(input));
    if (old) {
        saved = layout_project(&s->destination->mem, s->layout_old_size);
    }
    s->result = vmstate_load_state(file, vmsd, s->destination,
                                   vmsd->version_id, &err);
    s->layout_size = s->destination->mem.size;
    s->layout_planes = s->destination->mem.nplanes;
    if (old) {
        layout_restore(&s->destination->mem, saved);
    }
    if (!s->result) {
        s->layout_match = !memcmp(s->layout_prefix, s->destination->mem.bytes,
                                  s->layout_prefix_len);
        s->layout_stream_error = qemu_file_get_error(file);
        s->layout_marker = qemu_get_byte(file);
        if (!s->layout_stream_error) {
            s->layout_stream_error = qemu_file_get_error(file);
        }
        /* The EOF error belongs to this deliberate probe, not the load. */
        s->layout_eof_bytes = qemu_get_buffer(file, &extra, 1);
        s->layout_eof_error = qemu_file_get_error(file);
    }
    qemu_fclose(file);
    object_unref(OBJECT(input));
 done:
    s->load_error = err != NULL;
    if (err) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s\n", TYPE_KONA_READ_FIXTURE,
                      error_get_pretty(err));
        error_free(err);
    }
    /* Projected and partially loaded children never become command targets. */
    discard_destination(s);
}

static uint64_t control_read(void *opaque, hwaddr offset, unsigned size)
{
    KonaReadFixture *s = opaque;
    SGIKonaState *k = s->destination ? s->destination : s->source;

    switch (offset) {
    case FIXTURE_DMA_RESULT: return s->dma_result;
    case FIXTURE_DMA_READS: return s->dma_reads;
    case FIXTURE_WRITE_REFUSED: return k->x_write_refused;
    case FIXTURE_RESULT: return (int64_t)s->result;
    case FIXTURE_ERROR: return s->load_error;
    case FIXTURE_SAVE_RESULT: return (int64_t)s->save_result;
    case FIXTURE_LAYOUT_MATCH: return s->layout_match;
    case FIXTURE_LAYOUT_MARKER: return (int64_t)s->layout_marker;
    case FIXTURE_LAYOUT_EOF_BYTES: return s->layout_eof_bytes;
    case FIXTURE_LAYOUT_EOF_ERROR: return (int64_t)s->layout_eof_error;
    case FIXTURE_LAYOUT_STREAM_ERROR: return (int64_t)s->layout_stream_error;
    case FIXTURE_LAYOUT_SIZE: return s->layout_size;
    case FIXTURE_LAYOUT_PLANES: return s->layout_planes;
    case FIXTURE_LAYOUT_OLD_SIZE: return s->layout_old_size;
    case FIXTURE_LAYOUT_SOURCE_SIZE: return s->source->mem.size;
    case FIXTURE_LAYOUT_SOURCE_PLANES: return s->source->mem.nplanes;
    case FIXTURE_LAYOUT_SOURCE_BYTES: return s->source->mem.bytes_len;
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
    case FIXTURE_DMA_IO:
        s->dma_io = value;
        break;
    case FIXTURE_DMA_PITCH:
        s->dma_pitch = value;
        break;
    case FIXTURE_DMA_RUN:
        s->dma_reads = 0;
        s->dma_result = sgi_kona_x_image_dma(k, s->dma_io, value >> 32,
                                             value, s->dma_pitch);
        break;
    case FIXTURE_WRITE_MIGRATE:
        switch (value) {
        case 0:
            break;
        case 1:
            k->x_pixel_bpp = 16;
            break;
        case 2:
            k->x_pixel_stride++;
            break;
        case 3:
            k->x_pixel_words++;
            break;
        case 4:
            k->x_pixel_remaining = k->x_pixel_words + 1;
            break;
        case 5:
            sgi_plane_store(&k->x_write_mem,
                sgi_board_memory_plane(&k->x_write_mem, 1), 70, 80, 0);
            break;
        case 6:
            sgi_plane_store(&k->x_write_mem,
                sgi_board_memory_plane(&k->x_write_mem, 1), 74, 80, 1);
            break;
        case 7:
            sgi_plane_store(&k->x_write_mem,
                sgi_board_memory_plane(&k->x_write_mem, 0), 70, 80, 0x100);
            break;
        default:
            g_assert_not_reached();
        }
        roundtrip_state(s);
        break;
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
    case FIXTURE_LAYOUT_SAVE:
        layout_save(s, value != 0);
        break;
    case FIXTURE_LAYOUT_LOAD:
        layout_load(s, value != 0);
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
                          "kona-test.control", 320);
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
    g_clear_pointer(&s->layout_stream, g_free);
    g_clear_pointer(&s->layout_prefix, g_free);
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
