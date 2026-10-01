/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated synthetic board interface; no physical controller transport. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bic-board-interface.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

struct SGIBICBoardInterfaceState {
    DeviceState parent_obj;
    uint64_t identity;
    uint16_t initial_latch;
    bool initial_latch_set;
    bool migration_initial_latch_set;
    uint8_t high;
    uint8_t low;
    bool high_valid;
    bool low_valid;
};

MemTxResult sgi_bic_board_read(SGIBICBoardInterfaceState *s, hwaddr offset,
                              unsigned size, uint64_t *value)
{
    if (offset == 0x34000000 && size == 8) {
        *value = s->identity;
        return MEMTX_OK;
    }
    if (offset == 0x35000000 && size == 2) {
        if (!s->high_valid || !s->low_valid) {
            return MEMTX_ERROR;
        }
        *value = ((uint16_t)s->high << 8) | s->low;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-bic-board-interface: unsupported "
                  "%u-byte read at 0x%" HWADDR_PRIx "\n", size, offset);
    return MEMTX_ERROR;
}

MemTxResult sgi_bic_board_write(SGIBICBoardInterfaceState *s, hwaddr offset,
                               unsigned size, uint64_t value)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-bic-board-interface: unsupported "
                  "%u-byte write at 0x%" HWADDR_PRIx " value 0x%" PRIx64
                  "\n", size, offset, value);
    return MEMTX_ERROR;
}

MemTxResult sgi_bic_board_controller_byte(SGIBICBoardInterfaceState *s,
                                   unsigned position, bool high,
                                   uint8_t value, bool completed)
{
    if (position != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bic-board-interface: absent "
                      "controller position %u\n", position);
        return MEMTX_ERROR;
    }
    if (!completed) {
        return MEMTX_ERROR;
    }
    if (high) {
        s->high = value;
        s->high_valid = true;
    } else {
        s->low = value;
        s->low_valid = true;
    }
    return MEMTX_OK;
}

static void bic_reset_enter(Object *obj, ResetType type)
{
    SGIBICBoardInterfaceState *s = SGI_BIC_BOARD_INTERFACE(obj);

    s->high = s->initial_latch >> 8;
    s->low = s->initial_latch;
    s->high_valid = s->initial_latch_set;
    s->low_valid = s->initial_latch_set;
}

static void bic_realize(DeviceState *dev, Error **errp)
{
    SGIBICBoardInterfaceState *s = SGI_BIC_BOARD_INTERFACE(dev);

    /* Zero cannot silently supply an expansion identity. */
    if ((s->identity & 0xf000) != 0xc000) {
        error_setg(errp, "BIC experiment requires an explicit synthetic "
                   "identity with (identity & 0xf000) == 0xc000");
    }
}

static int bic_pre_save(void *opaque)
{
    SGIBICBoardInterfaceState *s = opaque;

    s->migration_initial_latch_set = s->initial_latch_set;
    return 0;
}

static int bic_post_load(void *opaque, int version_id)
{
    SGIBICBoardInterfaceState *s = opaque;

    return s->migration_initial_latch_set == s->initial_latch_set ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_bic = {
    .name = TYPE_SGI_BIC_BOARD_INTERFACE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = bic_pre_save,
    .post_load = bic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(identity, SGIBICBoardInterfaceState),
        VMSTATE_UINT16_EQUAL(initial_latch, SGIBICBoardInterfaceState),
        VMSTATE_BOOL(migration_initial_latch_set, SGIBICBoardInterfaceState),
        VMSTATE_UINT8(high, SGIBICBoardInterfaceState),
        VMSTATE_UINT8(low, SGIBICBoardInterfaceState),
        VMSTATE_BOOL(high_valid, SGIBICBoardInterfaceState),
        VMSTATE_BOOL(low_valid, SGIBICBoardInterfaceState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property bic_properties[] = {
    DEFINE_PROP_UINT64("identity", SGIBICBoardInterfaceState, identity, 0),
    DEFINE_PROP_UINT16("initial-latch", SGIBICBoardInterfaceState,
                       initial_latch, 0),
    DEFINE_PROP_BOOL("initial-latch-set", SGIBICBoardInterfaceState,
                     initial_latch_set, false),
};

static void bic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "Isolated synthetic BIC board interface";
    dc->realize = bic_realize;
    dc->vmsd = &vmstate_bic;
    dc->user_creatable = false;
    device_class_set_props(dc, bic_properties);
    rc->phases.enter = bic_reset_enter;
}

static const TypeInfo bic_type = {
    .name = TYPE_SGI_BIC_BOARD_INTERFACE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIBICBoardInterfaceState),
    .class_init = bic_class_init,
};

static void bic_register_types(void)
{
    type_register_static(&bic_type);
}

type_init(bic_register_types)
