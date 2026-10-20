/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI GBE 1.1 register subset from the published ASIC specification.
 * This device stores reviewed control fields. A default-off diagnostic
 * presents one synthetic normal tile; it is not a fitted O2 display model.
 */

#include "qemu/osdep.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/sgi/scanout.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "system/dma.h"

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
#define CTRLSTAT_RESET 0x03ffffc1u
#define TILE_POINTER   0xffffffe1u
#define DID_FIELDS     0x0001ffffu
#define RGB_FIELDS     0xffffff00u
#define GBE_DMA_BASE   0x40000000u
/* Synthetic timing; neither value identifies a fitted O2 clock. */
#define GBE_TEST_CLOCK_HZ 1000000u
#define GBE_TEST_FRAME_CLOCKS 16667u
#define GBE_TEST_WIDTH 128u
#define GBE_TEST_HEIGHT 128u
#define GBE_TEST_FRM_0 (BIT(14) | 16u)
#define GBE_TEST_WID_0 (5u << 2)

struct SGIO2GBEState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion *ram;
    MemoryRegion dma_window;
    MemoryRegion dma_ram;
    AddressSpace dma_as;
    SGIScanout scanout;
    Error *migration_blocker;
    bool synthetic_single_tile;
    bool frame_ready;
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
    uint8_t tile[64 * KiB];
};

static void gbe_resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    SGIO2GBEState *s = opaque;

    /* Synthetic RGB8 pixels use big-endian 32-bit words. */
    for (unsigned x = 0; x < GBE_TEST_WIDTH; x++) {
        uint32_t pixel = s->frame_ready ?
            ldl_be_p(&s->tile[(y * GBE_TEST_WIDTH + x) * 4]) : 0;
        uint8_t r = pixel >> 16, g = pixel >> 8, b = pixel;

        dst[x] = (s->gmap[r] & 0xff000000u) >> 8 |
                 (s->gmap[g] & 0x00ff0000u) >> 8 |
                 (s->gmap[b] & 0x0000ff00u) >> 8;
    }
}

static bool gbe_dma_in_fit(SGIO2GBEState *s, uint32_t addr, size_t len)
{
    uint64_t offset;

    if (addr < GBE_DMA_BASE) {
        return false;
    }
    offset = (uint64_t)addr - GBE_DMA_BASE;
    return offset <= memory_region_size(s->ram) &&
           len <= memory_region_size(s->ram) - offset;
}

static void gbe_retrace(void *opaque)
{
    SGIO2GBEState *s = opaque;
    uint8_t entry_bytes[2];
    uint32_t list = s->frm[2] & ~0x1fu;
    uint32_t tile_addr;
    uint16_t entry;

    /* Failure leaves this diagnostic surface black; no bus fault is modeled. */
    s->frame_ready = false;
    if (!gbe_dma_in_fit(s, list, sizeof(entry_bytes)) ||
        dma_memory_read(&s->dma_as, list, entry_bytes,
                        sizeof(entry_bytes), MEMTXATTRS_UNSPECIFIED)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-gbe: tile list outside the linked RAM fit\n");
        goto present;
    }
    /* The diagnostic chooses big-endian normal-list entries. */
    entry = lduw_be_p(entry_bytes);
    tile_addr = (uint32_t)entry << 16;
    if (!entry || !gbe_dma_in_fit(s, tile_addr, sizeof(s->tile))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-gbe: tile address 0x%08x outside the linked "
                      "RAM fit or zero\n", tile_addr);
        goto present;
    }
    if (dma_memory_read(&s->dma_as, tile_addr, s->tile,
                        sizeof(s->tile), MEMTXATTRS_UNSPECIFIED)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-gbe: tile DMA failed at 0x%08x\n", tile_addr);
        goto present;
    }
    s->frame_ready = true;

present:
    sgi_scanout_invalidate_all(&s->scanout);
}

static const SGIHeadOps gbe_scanout_ops = {
    .resolve_row = gbe_resolve_row,
    .retrace = gbe_retrace,
};

static void gbe_synthetic_mode(SGIO2GBEState *s, bool running)
{
    /* VT_1 is retained as a register; this diagnostic uses fixed timing. */
    SGIScanoutMode mode = {
        .width = GBE_TEST_WIDTH,
        .height = GBE_TEST_HEIGHT,
        .clock_hz = running ? GBE_TEST_CLOCK_HZ : 0,
        .frame_clocks = GBE_TEST_FRAME_CLOCKS,
    };

    sgi_scanout_set_mode(&s->scanout, &mode);
}

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
        return CTRLSTAT_RESET;
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
        if (!(data & VT_FREEZE)) {
            if (!s->synthetic_single_tile ||
                s->frm[0] != GBE_TEST_FRM_0 ||
                s->frm[1] != GBE_TEST_HEIGHT << 16 ||
                !(s->frm[2] & 1) ||
                !gbe_dma_in_fit(s, s->frm[2] & ~0x1fu, 2) ||
                s->wid[0] != GBE_TEST_WID_0) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-o2-gbe: VT_0 clock is unsupported for "
                              "this configuration\n");
                return;
            }
            if (s->vt0 & VT_FREEZE) {
                s->frame_ready = false;
                gbe_synthetic_mode(s, true);
            }
        } else if (s->synthetic_single_tile && !(s->vt0 & VT_FREEZE)) {
            s->frame_ready = false;
            gbe_synthetic_mode(s, false);
        }
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
        if (data & DID_FIELDS) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: %s is unsupported\n",
                          addr == DID ? "DID control" : "DID_SHADOW copy");
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
        if (index) {
            if (data & TILE_POINTER) {
                qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: %s is unsupported\n",
                              index == 1 ? "OVR_1 scanout control" :
                                           "OVR_2 shadow copy");
            }
            return;
        }
        if (data & BIT(13)) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: OVR_0 diagnostic "
                          "reset is unsupported\n");
            return;
        }
        s->ovr[0] = data & 0x1fff;
        return;
    }
    if (addr >= FRM_0 && addr <= FRM_3) {
        index = (addr - FRM_0) / 4;
        if (index >= 2) {
            if (index == 2 && s->synthetic_single_tile &&
                (s->vt0 & VT_FREEZE) &&
                (data & 1) &&
                gbe_dma_in_fit(s, data & ~0x1fu, 2)) {
                s->frm[2] = data & TILE_POINTER;
                return;
            }
            if (data & TILE_POINTER || s->synthetic_single_tile) {
                qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: %s is unsupported\n",
                              index == 2 ? "FRM_2 scanout control" :
                                           "FRM_3 shadow copy");
            }
            return;
        }
        if (!index && (data & BIT(15))) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: FRM_0 diagnostic reset "
                          "is unsupported\n");
            return;
        }
        if (s->synthetic_single_tile && !(s->vt0 & VT_FREEZE) &&
            (data & (index ? 0xffff0000u : 0x7fffu)) != s->frm[index]) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2-gbe: active frame geometry change is "
                          "unsupported\n");
            return;
        }
        s->frm[index] = data & (index ? 0xffff0000u : 0x7fffu);
        return;
    }
    if (addr >= WID_BASE && addr < WID_BASE + 4 * GBE_WID_COUNT) {
        index = (addr - WID_BASE) / 4;
        if (s->synthetic_single_tile && index == 0 &&
            (data & 0x1fff) != GBE_TEST_WID_0) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2-gbe: synthetic WID[0] mode is fixed\n");
            return;
        }
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
        if (index == 1 && (data & 3)) {
            qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: cursor scanout and "
                          "crosshair are unsupported\n");
            return;
        }
        s->crs[index] = data & (index == 0 ? UINT32_MAX :
                                index == 1 ? 3u : RGB_FIELDS);
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
    /* Preserve invalid size and alignment until gbe_accepts rejects them. */
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
        .unaligned = true,
        .accepts = gbe_accepts,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
        .unaligned = true,
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
    if (s->synthetic_single_tile) {
        s->frame_ready = false;
        s->wid[0] = GBE_TEST_WID_0;
        s->wid_written[0] = 1;
        memset(s->cmap_written, 1, sizeof(s->cmap_written));
        for (unsigned i = 0; i < GBE_GMAP_COUNT; i++) {
            s->gmap[i] = i << 24 | i << 16 | i << 8;
        }
        memset(s->gmap_written, 1, sizeof(s->gmap_written));
        gbe_synthetic_mode(s, false);
    }
}

static void gbe_realize(DeviceState *dev, Error **errp)
{
    SGIO2GBEState *s = SGI_O2_GBE(dev);

    if (!s->synthetic_single_tile) {
        return;
    }
    if (!s->ram ||
        !((memory_region_is_ram(s->ram) &&
          memory_region_size(s->ram) == 64 * MiB) ||
          (!memory_region_is_ram(s->ram) &&
           memory_region_size(s->ram) == GiB))) {
        error_setg(errp, "sgi-o2-gbe synthetic-single-tile requires linked "
                         "64 MiB RAM or a 1 GiB decoded RAM view");
        return;
    }
    error_setg(&s->migration_blocker,
               "SGI GBE synthetic single-tile scanout does not support "
               "migration");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        error_free(s->migration_blocker);
        s->migration_blocker = NULL;
        return;
    }
    memory_region_init(&s->dma_window, OBJECT(dev), "sgi-o2-gbe.dma-window",
                       UINT64_MAX);
    memory_region_init_alias(&s->dma_ram, OBJECT(dev), "sgi-o2-gbe.dma-ram",
                             s->ram, 0, memory_region_size(s->ram));
    memory_region_add_subregion(&s->dma_window, GBE_DMA_BASE, &s->dma_ram);
    address_space_init(&s->dma_as, &s->dma_window, "sgi-o2-gbe.dma");
    sgi_scanout_init(&s->scanout, dev, &gbe_scanout_ops, s);
    gbe_synthetic_mode(s, false);
}

static void gbe_unrealize(DeviceState *dev)
{
    SGIO2GBEState *s = SGI_O2_GBE(dev);

    if (!s->synthetic_single_tile) {
        return;
    }
    migrate_del_blocker(&s->migration_blocker);
    sgi_scanout_cleanup(&s->scanout);
    address_space_destroy(&s->dma_as);
    memory_region_del_subregion(&s->dma_window, &s->dma_ram);
    object_unparent(OBJECT(&s->dma_ram));
    object_unparent(OBJECT(&s->dma_window));
}

static const Property gbe_properties[] = {
    DEFINE_PROP_BOOL("synthetic-single-tile", SGIO2GBEState,
                     synthetic_single_tile, false),
    DEFINE_PROP_LINK("ram", SGIO2GBEState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

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
    object_property_add_uint64_ptr(obj, "x-test-frame-count",
                                   &s->scanout.frames, OBJ_PROP_FLAG_READ);
}

static void gbe_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_gbe;
    dc->desc = "SGI GBE 1.1 register subset";
    dc->user_creatable = true;
    dc->realize = gbe_realize;
    dc->unrealize = gbe_unrealize;
    device_class_set_props(dc, gbe_properties);
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
