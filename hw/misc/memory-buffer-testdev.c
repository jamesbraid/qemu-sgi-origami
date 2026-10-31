/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Test device for the optional MemoryRegionOps buffer callbacks.
 *
 * MMIO region 0 is a small byte store whose ops may offer read_buffer and
 * write_buffer; region 2 is RAM of the same size, logged for
 * DIRTY_MEMORY_VGA; region 1 holds 64-bit status and control registers:
 *
 *   0x00  scalar callbacks since reset (write: clear both counters)
 *   0x08  buffer callbacks since reset
 *   0x10  last result an internal transaction or buffer callback saw
 *   0x18  length the last buffer callback was offered
 *   0x20  write N: address_space_map() N bytes of region 0 for reading,
 *         then unmap; a bounce buffer reaches region 0 through the core
 *   0x28  write N: map N bytes of region 0 for writing, fill the mapping
 *         with bytes 0xc0 + i, then unmap
 *   0x30  sum of the bytes the last read mapping returned
 *   0x38  write N: address_space_write() N bytes of 0xa0 + i to region 0
 *   0x40  result of the last transaction started at 0x38 or 0x48
 *   0x48  write N: address_space_read() N bytes of region 0
 *   0x50  target address of the buffer-only transfers below
 *   0x58  write N: address_space_write_buffer_only() N bytes of 0xa0 + i
 *         at the target; the result goes to 0x40
 *   0x60  bytes the last buffer-only transfer moved
 *   0x68  write N: address_space_read_buffer_only() N bytes at the target;
 *         the result goes to 0x40
 *   0x70  sum of the bytes the last buffer-only read moved
 *   0x78  read: whether region 2 is dirty since the last read, then clear
 *
 * The mode property selects the buffer callback's behaviour:
 *
 *   0  consume the whole offer
 *   1  no buffer callbacks at all
 *   2  decline every offer
 *   3  consume at most 16 bytes
 *   4  re-enter region 0 through the system address space
 *   5  consume at most 16 bytes and report MEMTX_ERROR
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/event_notifier.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ramblock.h"

#define TYPE_MEMORY_BUFFER_TESTDEV "memory-buffer-testdev"
OBJECT_DECLARE_SIMPLE_TYPE(MemoryBufferTestState, MEMORY_BUFFER_TESTDEV)

#define MEMORY_BUFFER_TESTDEV_BYTES 256

enum {
    MODE_CONSUME,
    MODE_NO_CALLBACK,
    MODE_DECLINE,
    MODE_PARTIAL,
    MODE_REENTER,
    MODE_ERROR,
};

struct MemoryBufferTestState {
    SysBusDevice parent_obj;

    MemoryRegionOps ops;
    MemoryRegion store;
    MemoryRegion status;
    MemoryRegion ram;
    EventNotifier notifier;
    bool notifier_ready;

    uint32_t mode;
    uint64_t size;
    bool accepts;
    bool eventfd;

    uint64_t scalar_calls;
    uint64_t buffer_calls;
    uint64_t result;
    uint64_t offered;
    uint64_t mapped_sum;
    uint64_t transaction;
    uint64_t target;
    uint64_t moved;
    uint64_t moved_sum;
    uint8_t bytes[MEMORY_BUFFER_TESTDEV_BYTES];
};

/* Big-endian, so a scalar access sees the bytes in address order. */
static uint64_t store_read(void *opaque, hwaddr addr, unsigned size)
{
    MemoryBufferTestState *s = opaque;

    s->scalar_calls++;
    return ldn_be_p(s->bytes + addr, size);
}

static void store_write(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size)
{
    MemoryBufferTestState *s = opaque;

    s->scalar_calls++;
    stn_be_p(s->bytes + addr, size, value);
}

/* One of @rbuf (a read) and @wbuf (a write) is set. */
static hwaddr store_buffer(MemoryBufferTestState *s, hwaddr addr,
                           uint8_t *rbuf, const uint8_t *wbuf, hwaddr len,
                           MemTxAttrs attrs, MemTxResult *result)
{
    s->buffer_calls++;
    s->offered = len;
    assert(addr + len <= s->size);

    switch (s->mode) {
    case MODE_DECLINE:
        return 0;
    case MODE_PARTIAL:
        len = MIN(len, 16);
        break;
    case MODE_ERROR:
        len = MIN(len, 16);
        if (rbuf) {
            memset(rbuf, 0, len);
        }
        *result = MEMTX_ERROR;
        s->result = *result;
        return len;
    case MODE_REENTER: {
        uint8_t scratch[16];

        *result = address_space_read(&address_space_memory, s->store.addr,
                                     attrs, scratch, sizeof(scratch));
        s->result = *result;
        if (rbuf) {
            memset(rbuf, 0, len);
        }
        return len;
    }
    default:
        break;
    }

    if (wbuf) {
        memcpy(s->bytes + addr, wbuf, len);
    } else {
        memcpy(rbuf, s->bytes + addr, len);
    }
    return len;
}

static hwaddr store_read_buffer(void *opaque, hwaddr addr, void *buffer,
                                hwaddr len, MemTxAttrs attrs,
                                MemTxResult *result)
{
    return store_buffer(opaque, addr, buffer, NULL, len, attrs, result);
}

static hwaddr store_write_buffer(void *opaque, hwaddr addr,
                                 const void *buffer, hwaddr len,
                                 MemTxAttrs attrs, MemTxResult *result)
{
    return store_buffer(opaque, addr, NULL, buffer, len, attrs, result);
}

static bool store_accepts(void *opaque, hwaddr addr, unsigned size,
                          bool is_write, MemTxAttrs attrs)
{
    return true;
}

static void memory_buffer_testdev_map(MemoryBufferTestState *s, hwaddr len,
                                      bool is_write)
{
    hwaddr mapped = len;
    uint8_t *p;

    p = address_space_map(&address_space_memory, s->store.addr, &mapped,
                          is_write, MEMTXATTRS_UNSPECIFIED);
    if (!p) {
        s->result = MEMTX_ERROR;
        return;
    }
    s->result = MEMTX_OK;
    if (is_write) {
        for (hwaddr i = 0; i < mapped; i++) {
            p[i] = 0xc0 + i;
        }
    } else {
        s->mapped_sum = 0;
        for (hwaddr i = 0; i < mapped; i++) {
            s->mapped_sum += p[i];
        }
    }
    address_space_unmap(&address_space_memory, p, mapped, is_write, mapped);
}

/* The core's view of a whole transaction, not what the callback recorded. */
static void memory_buffer_testdev_transfer(MemoryBufferTestState *s,
                                           hwaddr len, bool is_write)
{
    uint8_t bytes[MEMORY_BUFFER_TESTDEV_BYTES];

    for (hwaddr i = 0; i < len; i++) {
        bytes[i] = 0xa0 + i;
    }
    s->transaction = address_space_rw(&address_space_memory, s->store.addr,
                                      MEMTXATTRS_UNSPECIFIED, bytes, len,
                                      is_write);
}

static void memory_buffer_testdev_buffer_only(MemoryBufferTestState *s,
                                              hwaddr len, bool is_write)
{
    uint8_t bytes[MEMORY_BUFFER_TESTDEV_BYTES * 2];
    MemTxResult result;

    len = MIN(len, sizeof(bytes));
    if (is_write) {
        for (hwaddr i = 0; i < len; i++) {
            bytes[i] = 0xa0 + i;
        }
        s->moved = address_space_write_buffer_only(&address_space_memory,
                                                   s->target,
                                                   MEMTXATTRS_UNSPECIFIED,
                                                   bytes, len, &result);
    } else {
        s->moved = address_space_read_buffer_only(&address_space_memory,
                                                  s->target,
                                                  MEMTXATTRS_UNSPECIFIED,
                                                  bytes, len, &result);
        s->moved_sum = 0;
        for (hwaddr i = 0; i < s->moved; i++) {
            s->moved_sum += bytes[i];
        }
    }
    s->transaction = result;
}

static bool memory_buffer_testdev_ram_dirty(MemoryBufferTestState *s)
{
    g_autofree DirtyBitmapSnapshot *snap =
        memory_region_snapshot_and_clear_dirty(&s->ram, 0, s->size,
                                               DIRTY_MEMORY_VGA);

    return memory_region_snapshot_get_dirty(&s->ram, snap, 0, s->size);
}

static uint64_t status_read(void *opaque, hwaddr addr, unsigned size)
{
    MemoryBufferTestState *s = opaque;

    switch (addr) {
    case 0x00:
        return s->scalar_calls;
    case 0x08:
        return s->buffer_calls;
    case 0x10:
        return s->result;
    case 0x18:
        return s->offered;
    case 0x30:
        return s->mapped_sum;
    case 0x40:
        return s->transaction;
    case 0x50:
        return s->target;
    case 0x60:
        return s->moved;
    case 0x70:
        return s->moved_sum;
    case 0x78:
        return memory_buffer_testdev_ram_dirty(s);
    default:
        return 0;
    }
}

static void status_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    MemoryBufferTestState *s = opaque;

    switch (addr) {
    case 0x00:
        s->scalar_calls = 0;
        s->buffer_calls = 0;
        break;
    case 0x20:
        memory_buffer_testdev_map(s, MIN(value, s->size), false);
        break;
    case 0x28:
        memory_buffer_testdev_map(s, MIN(value, s->size), true);
        break;
    case 0x38:
    case 0x48:
        memory_buffer_testdev_transfer(s, MIN(value, s->size), addr == 0x38);
        break;
    case 0x50:
        s->target = value;
        break;
    case 0x58:
    case 0x68:
        memory_buffer_testdev_buffer_only(s, value, addr == 0x58);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps status_ops = {
    .read = status_read,
    .write = status_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void memory_buffer_testdev_realize(DeviceState *dev, Error **errp)
{
    MemoryBufferTestState *s = MEMORY_BUFFER_TESTDEV(dev);

    if (!s->size || s->size > MEMORY_BUFFER_TESTDEV_BYTES) {
        error_setg(errp, "size must be 1 to %d bytes",
                   MEMORY_BUFFER_TESTDEV_BYTES);
        return;
    }
    if (s->mode > MODE_ERROR) {
        error_setg(errp, "unknown mode %u", s->mode);
        return;
    }

    s->ops = (MemoryRegionOps) {
        .read = store_read,
        .write = store_write,
        .endianness = DEVICE_BIG_ENDIAN,
        .valid = { .min_access_size = 1, .max_access_size = 8,
                   .unaligned = true },
        .impl = { .min_access_size = 1, .max_access_size = 8,
                  .unaligned = true },
    };
    if (s->mode != MODE_NO_CALLBACK) {
        s->ops.read_buffer = store_read_buffer;
        s->ops.write_buffer = store_write_buffer;
    }
    if (s->accepts) {
        s->ops.valid.accepts = store_accepts;
    }
    memory_region_init_io(&s->store, OBJECT(s), &s->ops, s,
                          "memory-buffer-testdev.store", s->size);
    if (s->eventfd) {
        if (event_notifier_init(&s->notifier, 0) < 0) {
            error_setg(errp, "cannot create the eventfd");
            return;
        }
        s->notifier_ready = true;
        memory_region_add_eventfd(&s->store, 0, 8, true, UINT64_MAX,
                                  &s->notifier);
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->store);

    memory_region_init_io(&s->status, OBJECT(s), &status_ops, s,
                          "memory-buffer-testdev.status", 0x80);
    /* Control writes start their own transactions into region 0. */
    s->status.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->status);

    /* Not migrated, so a second instance needs no unique RAM block name. */
    if (!memory_region_init_ram_flags_nomigrate(&s->ram, OBJECT(s),
                                                "memory-buffer-testdev.ram",
                                                s->size, 0, errp)) {
        return;
    }
    memory_region_set_log(&s->ram, true, DIRTY_MEMORY_VGA);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ram);
}

static void memory_buffer_testdev_unrealize(DeviceState *dev)
{
    MemoryBufferTestState *s = MEMORY_BUFFER_TESTDEV(dev);

    if (s->notifier_ready) {
        memory_region_del_eventfd(&s->store, 0, 8, true, UINT64_MAX,
                                  &s->notifier);
        event_notifier_cleanup(&s->notifier);
    }
}

static const Property memory_buffer_testdev_properties[] = {
    DEFINE_PROP_UINT32("mode", MemoryBufferTestState, mode, MODE_CONSUME),
    DEFINE_PROP_UINT64("size", MemoryBufferTestState, size,
                       MEMORY_BUFFER_TESTDEV_BYTES),
    DEFINE_PROP_BOOL("accepts", MemoryBufferTestState, accepts, false),
    DEFINE_PROP_BOOL("eventfd", MemoryBufferTestState, eventfd, false),
};

static void memory_buffer_testdev_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "memory API buffer callback test device";
    dc->realize = memory_buffer_testdev_realize;
    dc->unrealize = memory_buffer_testdev_unrealize;
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, memory_buffer_testdev_properties);
}

static const TypeInfo memory_buffer_testdev_info = {
    .name = TYPE_MEMORY_BUFFER_TESTDEV,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MemoryBufferTestState),
    .class_init = memory_buffer_testdev_class_init,
};

static void memory_buffer_testdev_register_types(void)
{
    type_register_static(&memory_buffer_testdev_info);
}

type_init(memory_buffer_testdev_register_types)
