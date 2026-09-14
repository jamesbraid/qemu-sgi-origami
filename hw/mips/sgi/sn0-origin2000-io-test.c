/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit test fixture: observe transactions through a real Hub claim view. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "sn0-origin2000-io.h"

#define TYPE_SN0_HUB_PACKET_TEST "sgi-hub-packet-test"
OBJECT_DECLARE_SIMPLE_TYPE(SN0HubPacketTest, SN0_HUB_PACKET_TEST)

struct SN0HubPacketTest {
    SysBusDevice parent_obj;
    SN0NodeState *node;
    SN0Origin2000HubIO io;
    MemoryRegion control;
    AddressSpace *packets;
    AddressSpace *claims;
    MemoryRegion recording;
    uint64_t forwarded;
    uint64_t observed_attrs;
    uint64_t base;
    uint64_t address;
    uint64_t data;
    uint64_t size;
    MemTxResult result;
};

/* Count forwarding without replacing the real Hub's claim semantics. */
static MemTxResult recording_read(void *opaque, hwaddr addr, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    SN0HubPacketTest *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;
    s->forwarded++;
    s->observed_attrs = attrs.requester_id | ((uint64_t)attrs.secure << 16);
    result = address_space_read(s->claims, addr, attrs,
                                bytes + 8 - size, size);
    *value = ldq_be_p(bytes);
    return result;
}

static MemTxResult recording_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    SN0HubPacketTest *s = opaque;
    uint8_t bytes[8];
    s->forwarded++;
    s->observed_attrs = attrs.requester_id | ((uint64_t)attrs.secure << 16);
    stq_be_p(bytes, value);
    return address_space_write(s->claims, addr, attrs, bytes + 8 - size, size);
}

static const MemoryRegionOps recording_ops = {
    .read_with_attrs = recording_read,
    .write_with_attrs = recording_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static uint64_t control_read(void *opaque, hwaddr addr, unsigned size)
{
    SN0HubPacketTest *s = opaque;
    switch (addr) {
    case 0:
        return s->address;
    case 8:
        return s->data;
    case 16:
        return s->size;
    case 32:
        return s->result;
    case 40:
        return s->forwarded;
    case 48:
        return s->observed_attrs;
    default:
        return 0;
    }
}

static void control_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    SN0HubPacketTest *s = opaque;
    uint8_t bytes[16] = { 0 };
    uint8_t *buffer;
    MemTxAttrs attrs = { .requester_id = 0x1234, .secure = true };
    switch (addr) {
    case 0:
        s->address = value;
        break;
    case 8:
        s->data = value;
        break;
    case 16:
        s->size = value;
        break;
    case 24:
        if (s->size != 1 && s->size != 2 && s->size != 4 && s->size != 8 &&
            s->size != 16) {
            s->result = MEMTX_ERROR;
            break;
        }
        buffer = bytes + (s->size < 8 ? 8 - s->size : 0);
        if (value) {
            stq_be_p(bytes, s->data);
        }
        s->result = address_space_rw(s->packets, s->address,
                                     attrs, buffer, s->size, value != 0);
        if (!value) {
            s->data = ldq_be_p(bytes);
        }
        break;
    case 40:
        s->forwarded = 0;
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

static void packet_test_realize(DeviceState *dev, Error **errp)
{
    SN0HubPacketTest *s = SN0_HUB_PACKET_TEST(dev);
    if (!s->node || !DEVICE(s->node)->realized) {
        error_setg(errp, "a realized Origin 2000 node is required");
        return;
    }
    if (!sn0_origin2000_hub_io_init(&s->io, OBJECT(s), s->node, errp)) {
        return;
    }
    s->claims = g_new0(AddressSpace, 1);
    address_space_init(s->claims, &s->node->device_view, "sn0.test.claims");
    memory_region_init_io(&s->recording, OBJECT(s), &recording_ops, s,
                          "sn0.test.forwarding", UINT64_MAX);
    /* The control access forwards through this recording region. */
    s->recording.disable_reentrancy_guard = true;
    address_space_destroy_free(s->io.target);
    s->io.target = g_new0(AddressSpace, 1);
    address_space_init(s->io.target, &s->recording, "sn0.test.target");
    s->packets = g_new0(AddressSpace, 1);
    address_space_init(s->packets, &s->io.face, "sn0.test.packets");
    memory_region_init_io(&s->control, OBJECT(s), &control_ops, s,
                          "sn0.test.packet.control", 64);
    /* The test control deliberately invokes its own packet guard. */
    s->control.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->control);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, s->base);
}

static void packet_test_unrealize(DeviceState *dev)
{
    SN0HubPacketTest *s = SN0_HUB_PACKET_TEST(dev);
    address_space_destroy_free(g_steal_pointer(&s->packets));
    sn0_origin2000_hub_io_destroy(&s->io);
    address_space_destroy_free(g_steal_pointer(&s->claims));
}

static const Property packet_test_properties[] = {
    DEFINE_PROP_LINK("node", SN0HubPacketTest, node, TYPE_SGI_SN0_NODE,
                     SN0NodeState *),
    DEFINE_PROP_UINT64("addr", SN0HubPacketTest, base, 0x40000000000ULL),
};

static void packet_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->user_creatable = true;
    dc->realize = packet_test_realize;
    dc->unrealize = packet_test_unrealize;
    dc->desc = "Origin 2000 Hub packet test fixture";
    device_class_set_props(dc, packet_test_properties);
}

static const TypeInfo packet_test_info = {
    .name = TYPE_SN0_HUB_PACKET_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SN0HubPacketTest),
    .class_init = packet_test_class_init,
};

static void packet_test_register_types(void)
{
    type_register_static(&packet_test_info);
}
type_init(packet_test_register_types)
