/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded Crosstalk packet addresses into one physical Hub's claim view. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "sn0-origin2000-io.h"

#define PACKET_IO (1ULL << 47)
#define PACKET_NASID_SHIFT 38
#define PACKET_MODIFY 0x01800090ULL
#define PACKET_IDENTITY 0x01600000ULL
#define HUB_NETWORK_IDENTITY 0x600000
#define SUPPORTED_NODE_STRIDE (1ULL << 32)
#define SUPPORTED_NASIDS 256

static bool packet_address(SN0Origin2000HubIO *io, hwaddr addr,
                           unsigned size, bool write, hwaddr *target)
{
    unsigned nasid = (addr >> PACKET_NASID_SHIFT) & 0xff;
    uint64_t interrupt = PACKET_IO | ((uint64_t)nasid << PACKET_NASID_SHIFT) |
                         PACKET_MODIFY;

    /* Validate the dispatched access before any downstream side effect. */
    if (size && addr < io->memory_size && size <= io->memory_size - addr) {
        *target = addr;
        return true;
    }
    if (write && size == 8 && addr == interrupt) {
        *target = io->xtalk_base + (uint64_t)nasid * io->node_stride +
                  io->modify_offset;
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sn0: unsupported Hub packet %s at 0x%" HWADDR_PRIx
                  " size %u\n", write ? "write" : "read", addr, size);
    return false;
}

static MemTxResult packet_read(void *opaque, hwaddr addr, uint64_t *data,
                               unsigned size, MemTxAttrs attrs)
{
    SN0Origin2000HubIO *io = opaque;
    uint8_t bytes[8] = { 0 };
    hwaddr target;
    MemTxResult result;
    AddressSpace *space = &io->target;

    *data = 0;
    if (size == 8 && addr == (PACKET_IO | PACKET_IDENTITY)) {
        /* The selected physical receiver, even when reset NASIDs coincide. */
        space = &io->local_registers;
        target = 0;
    } else if (!packet_address(io, addr, size, false, &target)) {
        return MEMTX_ERROR;
    }
    result = address_space_read(space, target, attrs,
                                bytes + 8 - size, size);
    *data = ldq_be_p(bytes);
    return result;
}

static MemTxResult packet_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size, MemTxAttrs attrs)
{
    SN0Origin2000HubIO *io = opaque;
    uint8_t bytes[8];
    hwaddr target;

    if (!packet_address(io, addr, size, true, &target)) {
        return MEMTX_ERROR;
    }
    stq_be_p(bytes, data);
    return address_space_write(&io->target, target, attrs,
                               bytes + 8 - size, size);
}

static const MemoryRegionOps packet_ops = {
    .read_with_attrs = packet_read,
    .write_with_attrs = packet_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

bool sn0_origin2000_hub_io_init(SN0Origin2000HubIO *io, SN0NodeState *node,
                               Error **errp)
{
    const SN0ModelAddressMap *map;

    if (!node || !DEVICE(node)->realized || !node->physical) {
        error_setg(errp,
                   "Hub packet adapter requires a realized physical node");
        return false;
    }
    map = &node->physical->map;
    if (map->memory.carrier_base != 0 ||
        map->node_stride != SUPPORTED_NODE_STRIDE ||
        map->xtalk.carrier_base != (2ULL << 40) ||
        map->hub_registers_node.offset != 0x01800000 ||
        map->hub_registers_node.size < 0x98) {
        error_setg(errp, "unsupported Hub packet address geometry");
        return false;
    }
    io->memory_size = SUPPORTED_NASIDS * map->node_stride;
    io->xtalk_base = map->xtalk.carrier_base;
    io->node_stride = map->node_stride;
    io->modify_offset = map->hub_registers_node.offset + 0x90;
    memory_region_init_io(&io->face, OBJECT(node), &packet_ops, io,
                          "sn0.hub.packet", 1ULL << 48);
    address_space_init(&io->target, &node->device_view,
                       "sn0.hub.packet.target");
    /* Keep the sole local register independent of its system-bus placement. */
    memory_region_init_alias(&io->local_register_view, OBJECT(node),
                             "sn0.hub.packet.local-register-view",
                             &node->hub.regs, HUB_NETWORK_IDENTITY, 8);
    address_space_init(&io->local_registers, &io->local_register_view,
                       "sn0.hub.packet.local-registers");
    return true;
}

void sn0_origin2000_hub_io_destroy(SN0Origin2000HubIO *io)
{
    address_space_destroy(&io->local_registers);
    object_unparent(OBJECT(&io->local_register_view));
    address_space_destroy(&io->target);
    object_unparent(OBJECT(&io->face));
}
