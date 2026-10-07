/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI GBE 1.1 register subset from the published ASIC specification.
 * This device stores reviewed control fields but has no timing clock,
 * descriptor fetch, pixel output, shadow-copy event or interrupt route.
 */

#include "qemu/osdep.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_SGI_O2_GBE "sgi-o2-gbe"
OBJECT_DECLARE_SIMPLE_TYPE(SGIO2GBEState, SGI_O2_GBE)

#define GBE_APERTURE_SIZE (16 * MiB)
#define GBE_WID_COUNT 32
#define GBE_CMAP_COUNT 4608
#define GBE_GMAP_COUNT 256
#define GBE_CURSOR_COLORS 3

#define CTRLSTAT       0x000000
#define DEVICE_ID      0x000014
#define VT_0           0x010000
#define VT_1           0x010004
#define VT_4           0x010010
#define VT_5           0x010014
#define OVR_0          0x020000
#define OVR_2          0x020008
#define FRM_0          0x030000
#define FRM_3          0x03000c
#define DID            0x040000
#define DID_SHADOW     0x040004
#define WID_BASE       0x048000
#define CMAP_BASE      0x050000
#define GMAP_BASE      0x060000
#define CRS_0          0x070000
#define CRS_1          0x070004
#define CRS_CM_BASE    0x070008

#define VT_FIELDS      0x00ffffffu
#define VT_FREEZE      0x80000000u
#define TILE_POINTER   0xffffffe1u
#define DID_FIELDS     0x0001ffffu
#define RGB_FIELDS     0xffffff00u

struct SGIO2GBEState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t vt0;
    uint32_t vt1;
    uint32_t vt4;
    uint32_t vt5;
    uint32_t ovr[3];
    uint32_t frm[4];
    uint32_t did[2];
    uint32_t wid[GBE_WID_COUNT];
    uint32_t cmap[GBE_CMAP_COUNT];
    uint32_t gmap[GBE_GMAP_COUNT];
    uint32_t crs[2 + GBE_CURSOR_COLORS];
    uint8_t wid_written[GBE_WID_COUNT];
    uint8_t cmap_written[GBE_CMAP_COUNT];
    uint8_t gmap_written[GBE_GMAP_COUNT];
    uint8_t crs_color_written[GBE_CURSOR_COLORS];
};

static bool gbe_accepts(void *opaque, hwaddr addr, unsigned size,
                        bool is_write, MemTxAttrs attrs)
{
    if (size == 4 && !(addr & 3)) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-o2-gbe: unsupported access %s at 0x%06" HWADDR_PRIx
                  " (%u bytes)\n", is_write ? "write" : "read", addr, size);
    return false;
}

static uint64_t gbe_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIO2GBEState *s = opaque;
    unsigned index;

    switch (addr) {
    case CTRLSTAT:
        return 1;
    case DEVICE_ID:
        return 0x666;
    case VT_0:
        return s->vt0;
    case VT_1:
        return s->vt1;
    case VT_4:
        return s->vt4;
    case VT_5:
        return s->vt5;
    case DID:
    case DID_SHADOW:
        return s->did[(addr - DID) / 4];
    default:
        break;
    }
    if (addr >= OVR_0 && addr <= OVR_2) {
        return s->ovr[(addr - OVR_0) / 4];
    }
    if (addr >= FRM_0 && addr <= FRM_3) {
        return s->frm[(addr - FRM_0) / 4];
    }
    if (addr >= WID_BASE && addr < WID_BASE + 4 * GBE_WID_COUNT) {
        index = (addr - WID_BASE) / 4;
        if (!s->wid_written[index]) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: WID[%u] reset value is "
                          "unspecified\n", index);
        }
        return s->wid[index];
    }
    if (addr >= CMAP_BASE && addr < CMAP_BASE + 4 * GBE_CMAP_COUNT) {
        index = (addr - CMAP_BASE) / 4;
        if (!s->cmap_written[index]) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: CMAP[%u] reset value is "
                          "unspecified\n", index);
        }
        return s->cmap[index];
    }
    if (addr >= GMAP_BASE && addr < GMAP_BASE + 4 * GBE_GMAP_COUNT) {
        index = (addr - GMAP_BASE) / 4;
        if (!s->gmap_written[index]) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: GMAP[%u] reset value is "
                          "unspecified\n", index);
        }
        return s->gmap[index];
    }
    if (addr >= CRS_0 && addr <= CRS_CM_BASE + 4 * (GBE_CURSOR_COLORS - 1)) {
        index = (addr - CRS_0) / 4;
        if (index >= 2 && !s->crs_color_written[index - 2]) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: cursor color %u reset "
                          "value is unspecified\n", index - 1);
        }
        return s->crs[index];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-o2-gbe: unsupported access read "
                  "at 0x%06" HWADDR_PRIx "\n", addr);
    return UINT32_MAX;
}

static void gbe_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned size)
{
    SGIO2GBEState *s = opaque;
    unsigned index;
    uint32_t data = value;

    switch (addr) {
    case VT_0:
        s->vt0 = data & (VT_FREEZE | VT_FIELDS);
        return;
    case VT_1:
        s->vt1 = data & VT_FIELDS;
        return;
    case VT_4:
        s->vt4 = data & VT_FIELDS;
        return;
    case VT_5:
        s->vt5 = data & VT_FIELDS;
        return;
    case DID:
    case DID_SHADOW:
        s->did[(addr - DID) / 4] = data & DID_FIELDS;
        if (data & BIT(16)) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: DID DMA is unsupported\n");
        }
        return;
    case CTRLSTAT:
    case DEVICE_ID:
        qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: identity/control write "
                      "at 0x%06" HWADDR_PRIx " is unsupported\n", addr);
        return;
    default:
        break;
    }
    if (addr >= OVR_0 && addr <= OVR_2) {
        index = (addr - OVR_0) / 4;
        s->ovr[index] = data & (index ? TILE_POINTER : 0x3fff);
        if ((index && (data & 1)) || (!index && (data & BIT(13)))) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: overlay DMA and "
                          "diagnostic reset are unsupported\n");
        }
        return;
    }
    if (addr >= FRM_0 && addr <= FRM_3) {
        index = (addr - FRM_0) / 4;
        s->frm[index] = data & (index >= 2 ? TILE_POINTER :
                                index ? 0xffff0000u : 0xffffu);
        if ((index >= 2 && (data & 1)) || (!index && (data & BIT(15)))) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: normal DMA and "
                          "diagnostic reset are unsupported\n");
        }
        return;
    }
    if (addr >= WID_BASE && addr < WID_BASE + 4 * GBE_WID_COUNT) {
        index = (addr - WID_BASE) / 4;
        s->wid[index] = data & 0x1fff;
        s->wid_written[index] = 1;
        return;
    }
    if (addr >= CMAP_BASE && addr < CMAP_BASE + 4 * GBE_CMAP_COUNT) {
        index = (addr - CMAP_BASE) / 4;
        s->cmap[index] = data & RGB_FIELDS;
        s->cmap_written[index] = 1;
        return;
    }
    if (addr >= GMAP_BASE && addr < GMAP_BASE + 4 * GBE_GMAP_COUNT) {
        index = (addr - GMAP_BASE) / 4;
        s->gmap[index] = data & RGB_FIELDS;
        s->gmap_written[index] = 1;
        return;
    }
    if (addr >= CRS_0 && addr <= CRS_CM_BASE + 4 * (GBE_CURSOR_COLORS - 1)) {
        index = (addr - CRS_0) / 4;
        s->crs[index] = data & (index == 0 ? UINT32_MAX :
                                index == 1 ? 3u : RGB_FIELDS);
        if (index == 1 && (data & 1)) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: cursor scanout is "
                          "unsupported\n");
        }
        if (index >= 2) {
            s->crs_color_written[index - 2] = 1;
        }
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-o2-gbe: unsupported access write "
                  "at 0x%06" HWADDR_PRIx "\n", addr);
}

static const MemoryRegionOps gbe_ops = {
    .read = gbe_read,
    .write = gbe_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
        .accepts = gbe_accepts,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void gbe_reset_enter(Object *obj, ResetType type)
{
    SGIO2GBEState *s = SGI_O2_GBE(obj);

    s->vt0 = VT_FREEZE;
    s->vt1 = s->vt4 = s->vt5 = 0;
    memset(s->ovr, 0, sizeof(s->ovr));
    memset(s->frm, 0, sizeof(s->frm));
    memset(s->did, 0, sizeof(s->did));
    s->crs[0] = s->crs[1] = 0;
    memset(s->wid, 0, sizeof(s->wid));
    memset(s->cmap, 0, sizeof(s->cmap));
    memset(s->gmap, 0, sizeof(s->gmap));
    memset(&s->crs[2], 0, sizeof(s->crs) - 2 * sizeof(s->crs[0]));
    memset(s->wid_written, 0, sizeof(s->wid_written));
    memset(s->cmap_written, 0, sizeof(s->cmap_written));
    memset(s->gmap_written, 0, sizeof(s->gmap_written));
    memset(s->crs_color_written, 0, sizeof(s->crs_color_written));
}

static const VMStateDescription vmstate_gbe = {
    .name = TYPE_SGI_O2_GBE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(vt0, SGIO2GBEState),
        VMSTATE_UINT32(vt1, SGIO2GBEState),
        VMSTATE_UINT32(vt4, SGIO2GBEState),
        VMSTATE_UINT32(vt5, SGIO2GBEState),
        VMSTATE_UINT32_ARRAY(ovr, SGIO2GBEState, 3),
        VMSTATE_UINT32_ARRAY(frm, SGIO2GBEState, 4),
        VMSTATE_UINT32_ARRAY(did, SGIO2GBEState, 2),
        VMSTATE_UINT32_ARRAY(wid, SGIO2GBEState, GBE_WID_COUNT),
        VMSTATE_UINT32_ARRAY(cmap, SGIO2GBEState, GBE_CMAP_COUNT),
        VMSTATE_UINT32_ARRAY(gmap, SGIO2GBEState, GBE_GMAP_COUNT),
        VMSTATE_UINT32_ARRAY(crs, SGIO2GBEState, 2 + GBE_CURSOR_COLORS),
        VMSTATE_UINT8_ARRAY(wid_written, SGIO2GBEState, GBE_WID_COUNT),
        VMSTATE_UINT8_ARRAY(cmap_written, SGIO2GBEState, GBE_CMAP_COUNT),
        VMSTATE_UINT8_ARRAY(gmap_written, SGIO2GBEState, GBE_GMAP_COUNT),
        VMSTATE_UINT8_ARRAY(crs_color_written, SGIO2GBEState,
                            GBE_CURSOR_COLORS),
        VMSTATE_END_OF_LIST()
    },
};

static void gbe_init(Object *obj)
{
    SGIO2GBEState *s = SGI_O2_GBE(obj);

    memory_region_init_io(&s->mmio, obj, &gbe_ops, s, TYPE_SGI_O2_GBE,
                          GBE_APERTURE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void gbe_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_gbe;
    dc->desc = "SGI GBE 1.1 register subset";
    dc->user_creatable = true;
    rc->phases.enter = gbe_reset_enter;
}

static const TypeInfo gbe_info = {
    .name = TYPE_SGI_O2_GBE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIO2GBEState),
    .instance_init = gbe_init,
    .class_init = gbe_class_init,
};

static void gbe_register_types(void)
{
    type_register_static(&gbe_info);
}

type_init(gbe_register_types)
