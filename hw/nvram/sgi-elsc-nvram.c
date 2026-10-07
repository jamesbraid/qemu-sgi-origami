/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI module system controller configuration NVRAM: one 2048-byte store
 * answering eight consecutive I2C addresses, each a 256-byte block reached
 * with a one-byte word address. The contract is the Origami project's
 * PCF8584 and ELSC datasheet, configuration NVRAM section.
 *
 * One device rather than eight 256-byte EEPROMs, so that a caller's 2 KiB
 * raw file backs the whole store and one image holds the controller's
 * state. Which part, and how many packages, the real controller used is
 * unknown; a single 24C16-class part fits the addressing.
 *
 * Without a backing file the store starts in the state a master controller
 * leaves after its first power-up (sgi_elsc_nvram_fresh_image()), built from
 * the "config", "module" and "clock" properties. With one, the file's
 * contents win and those properties are not used. Either way nothing is
 * written to the store at reset: it is non-volatile.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/i2c-wire.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/nvram/sgi-elsc-nvram.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(SGIElscNvramState, SGI_ELSC_NVRAM)

struct SGIElscNvramState {
    I2CSlave parent_obj;

    BlockBackend *blk;
    /* What a store with no backing file starts from. */
    uint8_t config;
    uint8_t module;
    uint8_t clock;

    /* The block the current transfer addressed, from its I2C address. */
    uint8_t block;
    /* The eleven-bit address the next data byte reads or writes. */
    uint16_t cur;
    /* A write transfer whose word address has not arrived yet. */
    bool want_word;
    /* A failed backing write, retried by the next store to any byte. */
    bool storage_failed;

    uint8_t mem[SGI_ELSC_NVRAM_BYTES];
};

/*
 * Behaviour at a block's end is unknown on hardware. The counter wraps to
 * the start of the same block, kept from the previous model's eight
 * 256-byte parts. A write of any length is accepted the same way, where a
 * real 24C16 wraps a write within a 16-byte page; the firmware's longest
 * write is 12 bytes from a block's start, so it never sees the difference.
 */
static void sgi_elsc_nvram_advance(SGIElscNvramState *s)
{
    s->cur = (s->cur & ~0xff) | ((s->cur + 1) & 0xff);
}

/*
 * Writes complete at once: the firmware's log writer does not retry, so a
 * busy NAK would cut a record short. A failed backing write is reported
 * and not the guest's error; the byte stays in the store.
 */
static void sgi_elsc_nvram_store(SGIElscNvramState *s, uint8_t data)
{
    if (s->mem[s->cur] == data && !s->storage_failed) {
        return;
    }
    s->mem[s->cur] = data;
    if (!s->blk) {
        return;
    }
    s->storage_failed = blk_pwrite(s->blk, s->cur, 1, &s->mem[s->cur], 0) < 0;
    if (s->storage_failed) {
        error_report("%s: write to the backing store failed",
                     TYPE_SGI_ELSC_NVRAM);
    }
}

static int sgi_elsc_nvram_event(I2CSlave *i2c, enum i2c_event event)
{
    SGIElscNvramState *s = SGI_ELSC_NVRAM(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->want_word = true;
        break;
    case I2C_START_RECV:
        /*
         * A read continues from the counter, in the block this transfer
         * addressed: a word-address write, a repeated START and a read of
         * the same address reads the byte written.
         */
        s->cur = (s->block << 8) | (s->cur & 0xff);
        s->want_word = false;
        break;
    case I2C_FINISH:
        s->want_word = false;
        break;
    case I2C_RESTART:
    case I2C_NACK:
        break;
    default:
        return -1;
    }
    return 0;
}

static int sgi_elsc_nvram_send(I2CSlave *i2c, uint8_t data)
{
    SGIElscNvramState *s = SGI_ELSC_NVRAM(i2c);

    if (s->want_word) {
        s->cur = (s->block << 8) | data;
        s->want_word = false;
        return 0;
    }
    sgi_elsc_nvram_store(s, data);
    sgi_elsc_nvram_advance(s);
    return 0;
}

static uint8_t sgi_elsc_nvram_recv(I2CSlave *i2c)
{
    SGIElscNvramState *s = SGI_ELSC_NVRAM(i2c);
    uint8_t data = s->mem[s->cur];

    sgi_elsc_nvram_advance(s);
    return data;
}

/*
 * Eight addresses from the device's own, the block chosen by which one.
 * A 24C16-class part answers only its own address pattern, so it does not
 * accept a general call.
 */
static bool sgi_elsc_nvram_match(I2CSlave *candidate, uint8_t address,
                                 bool broadcast, I2CNodeList *current_devs)
{
    SGIElscNvramState *s = SGI_ELSC_NVRAM(candidate);
    I2CNode *node;

    if (broadcast || address < candidate->address ||
        address >= candidate->address + SGI_ELSC_NVRAM_BLOCKS) {
        return false;
    }
    s->block = address - candidate->address;
    node = g_new(I2CNode, 1);
    node->elt = candidate;
    QLIST_INSERT_HEAD(current_devs, node, next);
    return true;
}

static void sgi_elsc_nvram_realize(DeviceState *dev, Error **errp)
{
    SGIElscNvramState *s = SGI_ELSC_NVRAM(dev);
    uint8_t address = I2C_SLAVE(dev)->address;
    int64_t length;

    if (!address || address > 0x80 - SGI_ELSC_NVRAM_BLOCKS) {
        error_setg(errp, "%s: eight consecutive seven-bit addresses cannot "
                   "start at 0x%x", TYPE_SGI_ELSC_NVRAM, address);
        return;
    }
    if (!s->blk) {
        sgi_elsc_nvram_fresh_image(s->mem, s->config, s->module, s->clock);
        return;
    }
    length = blk_getlength(s->blk);
    if (length < 0) {
        error_setg_errno(errp, -length, "%s: cannot read the size of the "
                         "backing store", TYPE_SGI_ELSC_NVRAM);
        return;
    }
    if (length != SGI_ELSC_NVRAM_BYTES) {
        error_setg(errp, "%s: the backing store is %" PRId64 " bytes, not "
                   "the required %u", TYPE_SGI_ELSC_NVRAM, length,
                   SGI_ELSC_NVRAM_BYTES);
        return;
    }
    if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0) {
        return;
    }
    if (blk_pread(s->blk, 0, SGI_ELSC_NVRAM_BYTES, s->mem, 0) < 0) {
        error_setg(errp, "%s: cannot read the backing store",
                   TYPE_SGI_ELSC_NVRAM);
    }
}

/* Only the transfer state resets; the store keeps its contents. */
static void sgi_elsc_nvram_reset_hold(Object *obj, ResetType type)
{
    SGIElscNvramState *s = SGI_ELSC_NVRAM(obj);

    s->block = 0;
    s->cur = 0;
    s->want_word = false;
}

static int sgi_elsc_nvram_post_load(void *opaque, int version_id)
{
    SGIElscNvramState *s = opaque;

    if (s->block >= SGI_ELSC_NVRAM_BLOCKS || s->cur >= SGI_ELSC_NVRAM_BYTES) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_elsc_nvram = {
    .name = TYPE_SGI_ELSC_NVRAM,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_elsc_nvram_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, SGIElscNvramState),
        VMSTATE_UINT8(block, SGIElscNvramState),
        VMSTATE_UINT16(cur, SGIElscNvramState),
        VMSTATE_BOOL(want_word, SGIElscNvramState),
        VMSTATE_BOOL(storage_failed, SGIElscNvramState),
        VMSTATE_UINT8_ARRAY(mem, SGIElscNvramState, SGI_ELSC_NVRAM_BYTES),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sgi_elsc_nvram_properties[] = {
    DEFINE_PROP_DRIVE("drive", SGIElscNvramState, blk),
    DEFINE_PROP_UINT8("config", SGIElscNvramState, config, 0),
    DEFINE_PROP_UINT8("module", SGIElscNvramState, module, 0),
    DEFINE_PROP_UINT8("clock", SGIElscNvramState, clock, 0),
};

static void sgi_elsc_nvram_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    dc->desc = "SGI module system controller configuration NVRAM";
    dc->realize = sgi_elsc_nvram_realize;
    dc->vmsd = &vmstate_sgi_elsc_nvram;
    device_class_set_props(dc, sgi_elsc_nvram_properties);
    /* A Hub or module controller builds its own store. */
    dc->user_creatable = false;
    rc->phases.hold = sgi_elsc_nvram_reset_hold;
    sc->event = sgi_elsc_nvram_event;
    sc->send = sgi_elsc_nvram_send;
    sc->recv = sgi_elsc_nvram_recv;
    sc->match_and_add = sgi_elsc_nvram_match;
}

static const TypeInfo sgi_elsc_nvram_types[] = {
    {
        .name = TYPE_SGI_ELSC_NVRAM,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(SGIElscNvramState),
        .class_init = sgi_elsc_nvram_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_I2C_WIRE_ENDPOINT },
            { }
        },
    },
};

DEFINE_TYPES(sgi_elsc_nvram_types)
