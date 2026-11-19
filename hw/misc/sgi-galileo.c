/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI OCTANE Digital Video option (Galileo 1.5 board), at the level where a
 * guest sees it present, enumerates it, opens it and sets up a path.
 *
 * Registers are reached through an option window of the IMPACT graphics
 * board (sgi-mgras.h); DMA and interrupts go through a GIO-mode Bridge on
 * the option's XIO board. Every fact, its grade and what is not modelled
 * is in docs/system/mips/sgi/hardware/galileo.rst.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_SGI_GALILEO "sgi-galileo"
OBJECT_DECLARE_TYPE(GalState, GalClass,
                    SGI_GALILEO)

/* "Identity": board product ID, revisions and the board's part. */
#define GAL_PRODUCT_ID         0x11
#define GAL_BOARD_REVISION     0x24
#define GAL_TMI_REVISION       2
#define GAL_CSC_REVISION       0
#define GAL_BOARD_PART         "030-0931-003"
#define GAL_BOARD_NAME         "GALILEO15"
#define GAL_BOARD_REVISION_TXT "C"

/* "GIO window": slot 0's product ID word, and where the other probes land. */
#define GIO_ID_OFFSET           0x4
#define GIO_MODE                0x10
#define GIO_CHIP_ID             0x20
#define GIO_CHANNEL1_SETUP      0x1000
#define GIO_SLOT_STRIDE_1       0x400000
#define GIO_SLOT_STRIDE_2       0x500000
#define GIO_SLOT_STRIDE_3       0x600000

/* "Interrupts": GIO lines, which the model wires to Bridge pins alike. */
#define GIO_LINE_DMA            4
#define GIO_LINE_CC1            6

/* "Register blocks". */
enum { BLK_BLEND, BLK_CC1, BLK_CONTROL, BLK_CSC, BLK_COUNT };
static const hwaddr blk_base[BLK_COUNT] = {
    0x072800, 0x073000, 0x073400, 0x073800,
};
static const char *const blk_name[BLK_COUNT] = {
    "blender", "CC1", "control", "CSC"
};
/* Index and data direct registers; the blender's never match (see below). */
static const uint8_t blk_index_reg[BLK_COUNT] = { 0, 1, 0, 0 };
static const uint8_t blk_data_reg[BLK_COUNT] = { 8, 2, 1, 1 };
/* Highest indirect register; the control block also answers 240 to 255. */
static const uint16_t blk_last[BLK_COUNT] = { 38, 80, 39, 5 };
#define CONTROL_CROSSPOINT_FIRST 240
#define CONTROL_CROSSPOINT_LAST  255
#define CONTROL_DIRECT_REVISION  2
#define CONTROL_DIRECT_TMI_INDEX 6
#define CONTROL_DIRECT_TMI_DATA  7
#define CSC_DIRECT_REVISION      3
#define TMI_LAST                 149
#define TMI_REVISION_REG         147
#define TMI_STATUS_REG           129
#define TMI_STATUS_START         0x02
#define BLEND_DIRECT_DATA1       1
#define BLEND_DIRECT_DATA2       2
#define BLEND_DIRECT_ADDR_LOW    3
#define BLEND_DIRECT_ADDR_HIGH   4
#define BLEND_LAST2              3
#define CC1_FIELD_CONTROL        38
#define CC1_FIELD_ENABLES        0xa0
#define CONTROL_I2C_STATUS       37
#define CONTROL_I2C_BUSY         0x80
#define CONTROL_TIMER_STATUS     17
/* No field event is modelled: these bits never read set. */
#define CONTROL_TIMER_PENDING    0x51

struct GalClass {
    SGIXioBoardClass parent_class;

    DeviceUnrealize parent_unrealize;
    ResettablePhases parent_phases;
};

struct GalState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint32_t field_period_ns;

    SGIBridgeState *bridge;
    SGIMgrasState *graphics;
    MemoryRegion gio;
    QEMUTimer *field_timer;

    uint8_t direct[BLK_COUNT][8];
    uint8_t ind[BLK_COUNT][256];
    uint8_t tmi[TMI_LAST + 1];
    uint8_t blend2[BLEND_LAST2 + 1];
    bool tmi_start;
};

static void gal_unimp(const char *what, hwaddr addr, bool write)
{
    qemu_log_mask(LOG_UNIMP, "sgi-galileo: unimplemented %s %s at "
                  "0x%" HWADDR_PRIx "\n", what, write ? "write" : "read",
                  addr);
}

/* ---- Register blocks ---- */

static bool gal_locate(hwaddr addr, unsigned *blk, unsigned *sel)
{
    for (unsigned b = 0; b < BLK_COUNT; b++) {
        hwaddr off = addr - blk_base[b];

        if (addr >= blk_base[b] && off < SGI_MGRAS_OPTION_BLOCK_SIZE) {
            if ((off & 0x7f) != 8) {
                return false;
            }
            *blk = b;
            *sel = off >> 7;
            return true;
        }
    }
    return false;
}

static bool gal_ind_valid(unsigned blk, unsigned idx)
{
    return idx <= blk_last[blk] ||
           (blk == BLK_CONTROL && idx >= CONTROL_CROSSPOINT_FIRST &&
            idx <= CONTROL_CROSSPOINT_LAST);
}

static void gal_field_update(GalState *s)
{
    if (s->ind[BLK_CC1][CC1_FIELD_CONTROL] & CC1_FIELD_ENABLES) {
        if (!timer_pending(s->field_timer)) {
            timer_mod(s->field_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                      s->field_period_ns);
        }
    } else {
        timer_del(s->field_timer);
    }
}

/* A pulse, not a level; the real line type is unknown. */
static void gal_field_tick(void *opaque)
{
    GalState *s = opaque;

    qemu_irq_pulse(qdev_get_gpio_in(DEVICE(s->bridge), GIO_LINE_CC1));
    timer_mod(s->field_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              s->field_period_ns);
}

static uint8_t gal_ind_read(GalState *s, unsigned blk, unsigned idx)
{
    if (!gal_ind_valid(blk, idx)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: %s indirect %u is "
                      "out of range\n", blk_name[blk], idx);
        return 0;
    }
    if (blk == BLK_CONTROL && idx == CONTROL_I2C_STATUS) {
        return s->ind[blk][idx] & ~CONTROL_I2C_BUSY;
    }
    if (blk == BLK_CONTROL && idx == CONTROL_TIMER_STATUS) {
        return s->ind[blk][idx] & ~CONTROL_TIMER_PENDING;
    }
    return s->ind[blk][idx];
}

static void gal_ind_write(GalState *s, unsigned blk, unsigned idx,
                           uint8_t value)
{
    if (!gal_ind_valid(blk, idx)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: %s indirect %u is "
                      "out of range\n", blk_name[blk], idx);
        return;
    }
    s->ind[blk][idx] = value;
    if (blk == BLK_CC1 && idx == CC1_FIELD_CONTROL) {
        gal_field_update(s);
    }
}

static uint8_t gal_option_read_reg(void *opaque, hwaddr addr)
{
    GalState *s = opaque;
    unsigned blk, sel;

    if (addr == SGI_MGRAS_OPTION_ID) {
        return GAL_PRODUCT_ID;
    }
    if (!gal_locate(addr, &blk, &sel)) {
        gal_unimp("option window register", addr, false);
        return 0;
    }
    if (blk == BLK_BLEND &&
        (sel == BLEND_DIRECT_DATA1 || sel == BLEND_DIRECT_DATA2)) {
        unsigned idx = s->direct[blk][BLEND_DIRECT_ADDR_LOW] |
                       s->direct[blk][BLEND_DIRECT_ADDR_HIGH] << 8;

        if (sel == BLEND_DIRECT_DATA2 && idx <= BLEND_LAST2) {
            return s->blend2[idx];
        }
        return sel == BLEND_DIRECT_DATA1 ? gal_ind_read(s, blk, idx) : 0;
    }
    if (sel == blk_data_reg[blk]) {
        return gal_ind_read(s, blk, s->direct[blk][blk_index_reg[blk]]);
    }
    if (blk == BLK_CONTROL && sel == CONTROL_DIRECT_TMI_DATA) {
        unsigned idx = s->direct[blk][CONTROL_DIRECT_TMI_INDEX];

        if (idx > TMI_LAST) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: TMI indirect %u "
                          "is out of range\n", idx);
            return 0;
        }
        if (idx == TMI_STATUS_REG) {
            /* A start pulse: set on one read, clear on the next. */
            s->tmi_start = !s->tmi_start;
            return s->tmi_start ? s->tmi[idx] | TMI_STATUS_START :
                                  s->tmi[idx] & ~TMI_STATUS_START;
        }
        return s->tmi[idx];
    }
    if (blk == BLK_CONTROL && sel == CONTROL_DIRECT_REVISION) {
        return GAL_BOARD_REVISION;
    }
    return s->direct[blk][sel];
}

static void gal_option_write_reg(void *opaque, hwaddr addr, uint8_t value)
{
    GalState *s = opaque;
    unsigned blk, sel;

    if (addr == SGI_MGRAS_OPTION_ID) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: write to the "
                      "identification byte\n");
        return;
    }
    if (!gal_locate(addr, &blk, &sel)) {
        gal_unimp("option window register", addr, true);
        return;
    }
    if (blk == BLK_BLEND &&
        (sel == BLEND_DIRECT_DATA1 || sel == BLEND_DIRECT_DATA2)) {
        unsigned idx = s->direct[blk][BLEND_DIRECT_ADDR_LOW] |
                       s->direct[blk][BLEND_DIRECT_ADDR_HIGH] << 8;

        if (sel == BLEND_DIRECT_DATA2 && idx <= BLEND_LAST2) {
            s->blend2[idx] = value;
        } else if (sel == BLEND_DIRECT_DATA1) {
            gal_ind_write(s, blk, idx, value);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: blender file 2 "
                          "register %u is out of range\n", idx);
        }
        return;
    }
    if (sel == blk_data_reg[blk]) {
        gal_ind_write(s, blk, s->direct[blk][blk_index_reg[blk]], value);
        return;
    }
    if (blk == BLK_CONTROL && sel == CONTROL_DIRECT_TMI_DATA) {
        unsigned idx = s->direct[blk][CONTROL_DIRECT_TMI_INDEX];

        if (idx > TMI_LAST) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: TMI indirect %u "
                          "is out of range\n", idx);
        } else if (idx != TMI_REVISION_REG) {
            s->tmi[idx] = value;
        }
        return;
    }
    if ((blk == BLK_CSC && sel == CSC_DIRECT_REVISION) ||
        (blk == BLK_CONTROL && sel == CONTROL_DIRECT_REVISION)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: write to a revision "
                      "register\n");
        return;
    }
    s->direct[blk][sel] = value;
}

static uint8_t gal_option_read(void *opaque, hwaddr addr)
{
    uint8_t value = gal_option_read_reg(opaque, addr);

    trace_sgi_galileo_option(addr, value, false);
    return value;
}

static void gal_option_write(void *opaque, hwaddr addr, uint8_t value)
{
    trace_sgi_galileo_option(addr, value, true);
    gal_option_write_reg(opaque, addr, value);
}

static const SGIMgrasOptionWindowOps gal_option_window_ops = {
    .read = gal_option_read,
    .write = gal_option_write,
};

/* ---- The GIO window ---- */

static uint64_t gal_gio_read_reg(hwaddr addr, unsigned size)
{
    /* The 64-bit identification register; its low word is the slot probe. */
    if ((addr == GIO_ID_OFFSET && size == 4) || (!addr && size == 8)) {
        return GAL_PRODUCT_ID;
    }
    if (!addr && size == 4) {
        return 0;
    }
    if (addr == GIO_CHIP_ID && size == 8) {
        return 0;
    }
    /* Slots 1 to 3 hold no board: the probe reads zero. */
    if ((addr == GIO_SLOT_STRIDE_1 + GIO_ID_OFFSET ||
         addr == GIO_SLOT_STRIDE_2 + GIO_ID_OFFSET ||
         addr == GIO_SLOT_STRIDE_3 + GIO_ID_OFFSET) && size == 4) {
        return 0;
    }
    gal_unimp("GIO window", addr, false);
    return 0;
}

static uint64_t gal_gio_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t value = gal_gio_read_reg(addr, size);

    trace_sgi_galileo_gio(addr, value, size, false);
    return value;
}

static void gal_gio_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    trace_sgi_galileo_gio(addr, value, size, true);
    /* The driver's presence path stores the system mode and channel setup. */
    if (size == 8 && (addr == GIO_MODE || addr == GIO_CHANNEL1_SETUP)) {
        return;
    }
    gal_unimp("GIO window", addr, true);
}

static const MemoryRegionOps gal_gio_ops = {
    .read = gal_gio_read,
    .write = gal_gio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* ---- Lifecycle ---- */

static bool gal_build_widget(SGIXioBoardState *board, Error **errp)
{
    GalState *s = SGI_GALILEO(board);
    Object *obj = OBJECT(s);
    DeviceState *bridge;
    bool ambiguous = false;
    Object *gfx;
    uint64_t serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('O', board->slot);
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];

    if (!s->fabric) {
        error_setg(errp, TYPE_SGI_GALILEO " needs the fabric its "
                   "interrupts travel");
        return false;
    }
    if (!s->graphics) {
        /* Without a "graphics" link, take the machine's only MGRAS. */
        gfx = object_resolve_path_type("", TYPE_SGI_MGRAS, &ambiguous);
        if (!gfx || ambiguous) {
            error_setg(errp, TYPE_SGI_GALILEO
                       " needs a graphics link or exactly one sgi-mgras that "
                       "is already realized");
            return false;
        }
        s->graphics = SGI_MGRAS(gfx);
    }

    memory_region_init_io(&s->gio, obj, &gal_gio_ops, s,
                          TYPE_SGI_GALILEO ".gio",
                          SGI_BRIDGE_GIO_WINDOW_SIZE);
    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(obj, "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    sgi_synthetic_barcode(barcode, 'O', serial);
    qdev_prop_set_string(bridge, "board-part", GAL_BOARD_PART);
    qdev_prop_set_string(bridge, "board-barcode", barcode);
    qdev_prop_set_string(bridge, "board-revision", GAL_BOARD_REVISION_TXT);
    qdev_prop_set_string(bridge, "board-name", GAL_BOARD_NAME);
    qdev_prop_set_uint64(bridge, "onewire-serial", serial);
    qdev_prop_set_bit(bridge, "gio-mode", true);
    sgi_xio_board_name_pci_bus(board, bridge, NULL);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    object_property_set_link(OBJECT(bridge), "gio-window", OBJECT(&s->gio),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    if (!sgi_mgras_attach_option_window(s->graphics, &gal_option_window_ops,
                                        s)) {
        error_setg(errp, "the graphics board's option window already has "
                   "a board");
        return false;
    }
    s->field_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gal_field_tick, s);

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    board->link_reset = qdev_get_gpio_in_named(bridge, "link-reset", 0);
    board->link_chip = OBJECT(bridge);
    return true;
}

static void gal_unrealize(DeviceState *dev)
{
    GalClass *dc = SGI_GALILEO_GET_CLASS(dev);
    GalState *s = SGI_GALILEO(dev);

    dc->parent_unrealize(dev);
    if (s->graphics) {
        sgi_mgras_detach_option_window(s->graphics, s);
    }
    timer_free(s->field_timer);
    s->field_timer = NULL;
}

/* A reset returns the registers to the model's zero state and stops fields. */
static void gal_reset_hold(Object *obj, ResetType type)
{
    GalClass *dc = SGI_GALILEO_GET_CLASS(obj);
    GalState *s = SGI_GALILEO(obj);

    if (dc->parent_phases.hold) {
        dc->parent_phases.hold(obj, type);
    }
    memset(s->direct, 0, sizeof(s->direct));
    memset(s->ind, 0, sizeof(s->ind));
    memset(s->tmi, 0, sizeof(s->tmi));
    memset(s->blend2, 0, sizeof(s->blend2));
    s->tmi_start = false;
    s->tmi[TMI_REVISION_REG] = GAL_TMI_REVISION;
    s->direct[BLK_CSC][CSC_DIRECT_REVISION] = GAL_CSC_REVISION;
    if (s->field_timer) {
        timer_del(s->field_timer);
    }
}

static int gal_post_load(void *opaque, int version_id)
{
    gal_field_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_gal = {
    .name = TYPE_SGI_GALILEO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = gal_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_2DARRAY(direct, GalState, BLK_COUNT, 8),
        VMSTATE_UINT8_2DARRAY(ind, GalState, BLK_COUNT, 256),
        VMSTATE_UINT8_ARRAY(tmi, GalState, TMI_LAST + 1),
        VMSTATE_UINT8_ARRAY(blend2, GalState, BLEND_LAST2 + 1),
        VMSTATE_BOOL(tmi_start, GalState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property gal_properties[] = {
    DEFINE_PROP_LINK("fabric", GalState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("graphics", GalState, graphics, TYPE_SGI_MGRAS,
                     SGIMgrasState *),
    /* The CC1 field interrupt period: an NTSC field by default. */
    DEFINE_PROP_UINT32("field-period-ns", GalState, field_period_ns,
                       16683333),
};

static void gal_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    GalClass *dvc = SGI_GALILEO_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    bc->build_widget = gal_build_widget;
    device_class_set_parent_unrealize(dc, gal_unrealize,
                                      &dvc->parent_unrealize);
    resettable_class_set_parent_phases(rc, NULL, gal_reset_hold, NULL,
                                       &dvc->parent_phases);
    dc->vmsd = &vmstate_gal;
    dc->desc = "SGI OCTANE Digital Video option (Galileo 1.5)";
    /* Fitted to an empty XIO slot it names (slot=widget-10). */
    dc->user_creatable = true;
    device_class_set_props(dc, gal_properties);
}

static const TypeInfo gal_types[] = {
    {
        .name = TYPE_SGI_GALILEO,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(GalState),
        .class_size = sizeof(GalClass),
        .class_init = gal_class_init,
    },
};

DEFINE_TYPES(gal_types)
