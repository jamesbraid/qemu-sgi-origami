/* SPDX-License-Identifier: BSD-3-Clause */
/* Eight-port SN1 register target and bounded M-mode forwarding. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-sn1-router.h"
#include "hw/misc/sgi-l1.h"
#include "hw/mips/sgi/net-cable.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "system/reset.h"

#define ROUTER_SIZE 0x600008
#define FORCE_LOCAL (UINT64_C(1) << 12)
#define META_ID_MASK (UINT64_C(15) << 8)
#define PROTECTION_CONFIG_MASK UINT64_C(0x3fff)
#define PARAMETER_MASK ((UINT64_C(1) << 30) - 1)
#define CHANNEL_TEST_MASK UINT64_C(0x004000000fffffff)
#define REMOTE_RESET_MASK UINT64_C(0xff)
#define GLOBAL_PORT_DEF_MASK UINT64_C(0xffff)
#define INACTIVE_BARRIER_CONTEXTS 32
#define INACTIVE_BARRIER_MASK ((UINT64_C(1) << 26) - 1)
#define DEBUG_ADDRESS_MASK ((UINT64_C(1) << 10) - 1)
#define DEBUG_ADDRESS_FIXED (UINT64_C(0xffff) << 10)

struct SGISN1RouterState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    SGINetCableState *cable[SGI_SN1_ROUTER_PORTS];
    DeviceState *uart;
    AddressSpace uart_as;
    bool uart_as_ready;
    bool cables_locked;
    uint8_t revision;
    uint8_t synthetic_diagnostics;
    uint64_t diagnostic[3];
    uint64_t protection;
    uint64_t global[2];
    uint64_t global_port_def;
    uint64_t jbus_control;
    uint64_t scratch[5];
    uint64_t parameter[SGI_SN1_ROUTER_PORTS];
    uint64_t channel_test[SGI_SN1_ROUTER_PORTS];
    uint64_t remote_reset_mask[SGI_SN1_ROUTER_PORTS];
    uint64_t inactive_barrier[INACTIVE_BARRIER_CONTEXTS];
    uint64_t local[SGI_SN1_ROUTER_PORTS][128];
    uint64_t meta[SGI_SN1_ROUTER_PORTS][16];
    uint64_t generation;
    void (*changed)(void *);
    void *changed_opaque;
};

static const hwaddr bank[SGI_SN1_ROUTER_PORTS] = {
    0x10000, 0x10800, 0x20000, 0x20800,
    0x30000, 0x30800, 0x40000, 0x40800,
};

bool sgi_sn1_router_port_neighbor(SGISN1RouterState *s, unsigned port,
                                 Object **peer, unsigned *index)
{
    return port >= 1 && port <= SGI_SN1_ROUTER_PORTS && s->cable[port - 1] &&
        sgi_net_cable_neighbor(s->cable[port - 1], OBJECT(s), port,
                               peer, index);
}

void sgi_sn1_router_set_changed(SGISN1RouterState *s,
                               void (*changed)(void *), void *opaque)
{
    s->changed = changed;
    s->changed_opaque = opaque;
}

static void router_changed(SGISN1RouterState *s)
{
    if (s->generation == sgi_net_port_flood_current()) {
        return;
    }
    s->generation = sgi_net_port_flood_current();
    if (s->changed) {
        s->changed(s->changed_opaque);
    }
    for (unsigned p = 1; p <= SGI_SN1_ROUTER_PORTS; p++) {
        Object *peer;
        unsigned index;

        /* Notify the physically wired peer even when the cable is down. */
        if (s->cable[p - 1] &&
            sgi_net_cable_other_end(s->cable[p - 1], OBJECT(s), p,
                                     &peer, &index)) {
            sgi_net_port_peer_changed(peer, index);
        }
    }
}

static uint64_t router_status(SGISN1RouterState *s, unsigned ingress)
{
    uint64_t value = (UINT64_C(0x3017) << 12) |
        ((uint64_t)s->revision << 28) | ((uint64_t)ingress << 52);

    if (s->synthetic_diagnostics) {
        value |= (UINT64_C(0x24) << 1) | 1;
    }

    for (unsigned p = 1; p <= SGI_SN1_ROUTER_PORTS; p++) {
        Object *peer;
        unsigned index;

        if (sgi_sn1_router_port_neighbor(s, p, &peer, &index)) {
            value |= UINT64_C(1) << (35 + 2 * p);
        } else if (s->synthetic_diagnostics) {
            value |= UINT64_C(1) << (34 + 2 * p);
        }
    }
    return value;
}

static SGINetPacketResult refuse(SGINetPacket *packet,
                                 SGINetPacketFailure failure)
{
    packet->failure = failure;
    qemu_log_mask(LOG_UNIMP, "sgi-sn1-router: refusing operation %u at "
                  "0x%" HWADDR_PRIx " (failure %u)\n",
                  packet->op, packet->addr, failure);
    return SGI_NET_PACKET_ERROR;
}

SGINetPacketResult sgi_sn1_router_access(SGISN1RouterState *s,
    unsigned ingress, SGINetPacket *packet)
{
    hwaddr address = packet->addr;
    uint64_t *storage = NULL;
    uint64_t *table = NULL;
    uint64_t mask = UINT64_MAX;
    bool write = packet->op == SGI_NET_PACKET_WRITE;
    bool notify = true;

    if (packet->op != SGI_NET_PACKET_READ && !write) {
        return refuse(packet, SGI_NET_PACKET_FAILURE_COMMAND);
    }
    if (ingress > SGI_SN1_ROUTER_PORTS || (address & 7)) {
        return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
    }
    if (!address || address == 0x600000) {
        if (write) {
            if (!address && ingress && s->synthetic_diagnostics) {
                return SGI_NET_PACKET_OK;
            }
            return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
        }
        packet->value = router_status(s, ingress);
        return SGI_NET_PACKET_OK;
    }
    if (s->synthetic_diagnostics && address >= 0x30 && address <= 0x48) {
        if (address == 0x30) {
            if (write && packet->data) {
                return refuse(packet, SGI_NET_PACKET_FAILURE_COMMAND);
            }
            packet->value = 0;
            return SGI_NET_PACKET_OK;
        }
        unsigned index = (address - 0x38) / 8;
        mask = index == 0 ? DEBUG_ADDRESS_MASK :
            (UINT64_C(1) << (32 + index)) - 1;

        if (write) {
            s->diagnostic[index] = packet->data & mask;
        } else {
            packet->value = s->diagnostic[index] |
                (index == 0 ? DEBUG_ADDRESS_FIXED : 0);
        }
        return SGI_NET_PACKET_OK;
    }
    if (address == 0x18 && s->synthetic_diagnostics) {
        if (write) {
            s->global_port_def = packet->data & GLOBAL_PORT_DEF_MASK;
        } else {
            packet->value = s->global_port_def;
        }
        return SGI_NET_PACKET_OK;
    }
    if (address == 0x8 && write && s->synthetic_diagnostics && !packet->data) {
        return SGI_NET_PACKET_OK;
    }
    if (address == 0x8 || address == 0x18 || address == 0x30) {
        if (write) {
            return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
        }
        packet->value = 0;
        return SGI_NET_PACKET_OK;
    }
    if (address == 0x518 && !write) {
        packet->value = s->scratch[3];
        s->scratch[3] = 1;
        return SGI_NET_PACKET_OK;
    }
    if (address >= 0x240 && address <= 0x278 && s->uart_as_ready) {
        uint64_t value = cpu_to_be64(packet->data & 0xff);
        MemTxResult result = address_space_rw(&s->uart_as, address - 0x240,
            MEMTXATTRS_UNSPECIFIED, &value, sizeof(value), write);

        if (result != MEMTX_OK) {
            return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
        }
        packet->value = be64_to_cpu(value) & 0xff;
        return SGI_NET_PACKET_OK;
    }
    switch (address) {
    case 0x10:
        storage = &s->protection;
        mask = s->synthetic_diagnostics ? PROTECTION_CONFIG_MASK : UINT64_MAX;
        break;
    case 0x20:
    case 0x28:
        storage = &s->global[(address - 0x20) / 8];
        break;
    case 0x50:
        if (!write) {
            /* Ordinary-router position inputs are a reviewed approximation. */
            packet->value = s->jbus_control | (UINT64_C(3) << 20);
            return SGI_NET_PACKET_OK;
        }
        storage = &s->jbus_control;
        break;
    case 0x100 ... 0x120:
        storage = &s->scratch[(address - 0x100) / 8];
        mask = address >= 0x118 ? 1 : UINT64_MAX;
        break;
    case 0x8000 ... 0x80f8:
        if (write && !s->synthetic_diagnostics &&
            (packet->data & ~INACTIVE_BARRIER_MASK)) {
            return refuse(packet, SGI_NET_PACKET_FAILURE_COMMAND);
        }
        storage = &s->inactive_barrier[(address - 0x8000) / 8];
        notify = false;
        break;
    default:
        for (unsigned p = 0; p < SGI_SN1_ROUTER_PORTS; p++) {
            if (address == bank[p]) {
                storage = &s->parameter[p];
                mask = PARAMETER_MASK;
                notify = false;
                break;
            }
            if (s->synthetic_diagnostics && address == bank[p] + 0x10) {
                storage = &s->channel_test[p];
                mask = CHANNEL_TEST_MASK;
                notify = false;
                break;
            }
            if (s->synthetic_diagnostics && address == bank[p] + 0x18) {
                storage = &s->remote_reset_mask[p];
                mask = REMOTE_RESET_MASK;
                notify = false;
                break;
            }
            if (!write && (address == bank[p] + 8 ||
                           address == bank[p] + 0x88)) {
                packet->value = 0;
                return SGI_NET_PACKET_OK;
            }
            if (address >= bank[p] + 0x100 &&
                address < bank[p] + 0x180) {
                table = &s->meta[p][(address - bank[p] - 0x100) / 8];
            } else if (address >= bank[p] + 0x200 &&
                       address < bank[p] + 0x600) {
                table = &s->local[p][(address - bank[p] - 0x200) / 8];
            }
        }
    }
    if (!storage && !table) {
        return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
    }
    if (write) {
        if (table) {
            *table = packet->data;
        } else {
            *storage = packet->data & mask;
        }
        if (notify) {
            sgi_net_port_flood_start();
            router_changed(s);
        }
    } else {
        packet->value = table ? *table : *storage;
    }
    return SGI_NET_PACKET_OK;
}

bool sgi_sn1_router_route(SGISN1RouterState *s, unsigned ingress,
    uint8_t turn, uint8_t destination, unsigned *output, uint8_t *next_turn)
{
    if (ingress < 1 || ingress > SGI_SN1_ROUTER_PORTS || turn > 7 ||
        destination > 127 ||
        ((s->protection & META_ID_MASK) && !(s->protection & FORCE_LOCAL))) {
        return false;
    }
    *output = 1 + ((ingress - 1 + turn) % SGI_SN1_ROUTER_PORTS);
    *next_turn = s->local[*output - 1][destination] & 7;
    return true;
}

static SGINetPacketResult router_receive(Object *port, unsigned ingress,
                                         SGINetPacket *packet, Error **errp)
{
    SGISN1RouterState *s = SGI_SN1_ROUTER(port);
    Object *peer;
    unsigned index;
    unsigned output;
    SGINetPacketResult result;
    uint64_t reply_vector;

    if (ingress < 1 || ingress > SGI_SN1_ROUTER_PORTS) {
        return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
    }
    if (!packet->vector) {
        return sgi_sn1_router_access(s, ingress, packet);
    }
    output = packet->vector & 15;
    if (output < 1 || output > SGI_SN1_ROUTER_PORTS ||
        packet->hops >= SGI_NET_PORT_MAX_HOPS ||
        !sgi_sn1_router_port_neighbor(s, output, &peer, &index) ||
        !sgi_net_packet_append_reply(packet, ingress)) {
        return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
    }
    packet->vector >>= 4;
    packet->hops++;
    reply_vector = packet->reply_vector;
    result = sgi_net_port_receive(peer, index, packet, errp);
    if (result == SGI_NET_PACKET_NONE) {
        packet->reply_vector = reply_vector;
        return refuse(packet, SGI_NET_PACKET_FAILURE_ADDRESS);
    }
    return result;
}

static bool router_forwards(Object *port, unsigned index)
{
    return index >= 1 && index <= SGI_SN1_ROUTER_PORTS;
}

static void router_peer_changed(Object *port, unsigned index)
{
    router_changed(SGI_SN1_ROUTER(port));
}

static MemTxResult router_read(void *opaque, hwaddr offset, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    SGINetPacket packet = { .op = SGI_NET_PACKET_READ, .addr = offset };
    SGINetPacketResult result = sgi_sn1_router_access(opaque, 0, &packet);

    *value = packet.value;
    return result == SGI_NET_PACKET_OK ? MEMTX_OK : MEMTX_DECODE_ERROR;
}

static MemTxResult router_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    SGINetPacket packet = {
        .op = SGI_NET_PACKET_WRITE, .addr = offset, .data = value,
    };

    return sgi_sn1_router_access(opaque, 0, &packet) == SGI_NET_PACKET_OK ?
        MEMTX_OK : MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps router_ops = {
    .read_with_attrs = router_read,
    .write_with_attrs = router_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void router_reset_enter(Object *obj, ResetType type)
{
    SGISN1RouterState *s = SGI_SN1_ROUTER(obj);

    /* Cold-zero state is a reviewed functional approximation. */
    s->protection = 0;
    s->jbus_control = 0;
    memset(s->global, 0, sizeof(s->global));
    s->global_port_def = 0;
    memset(s->scratch, 0, sizeof(s->scratch));
    memset(s->parameter, 0, sizeof(s->parameter));
    memset(s->channel_test, 0, sizeof(s->channel_test));
    memset(s->remote_reset_mask, 0, sizeof(s->remote_reset_mask));
    memset(s->inactive_barrier, 0, sizeof(s->inactive_barrier));
    memset(s->diagnostic, 0, sizeof(s->diagnostic));
    memset(s->local, 0, sizeof(s->local));
    memset(s->meta, 0, sizeof(s->meta));
}

static void router_reset_exit(Object *obj, ResetType type)
{
    sgi_net_port_flood_start();
    router_changed(SGI_SN1_ROUTER(obj));
}

static int router_post_load(void *opaque, int version_id)
{
    SGISN1RouterState *s = opaque;

    if ((s->global_port_def & ~GLOBAL_PORT_DEF_MASK) ||
        (!s->synthetic_diagnostics && s->global_port_def) ||
        s->scratch[3] > 1 || s->scratch[4] > 1) {
        return -EINVAL;
    }
    for (unsigned p = 0; p < SGI_SN1_ROUTER_PORTS; p++) {
        if (s->parameter[p] & ~PARAMETER_MASK) {
            return -EINVAL;
        }
        if (s->channel_test[p] & ~CHANNEL_TEST_MASK ||
            s->remote_reset_mask[p] & ~REMOTE_RESET_MASK) {
            return -EINVAL;
        }
    }
    for (unsigned context = 0; context < INACTIVE_BARRIER_CONTEXTS; context++) {
        if (!s->synthetic_diagnostics &&
            (s->inactive_barrier[context] & ~INACTIVE_BARRIER_MASK)) {
            return -EINVAL;
        }
    }
    sgi_net_port_flood_start();
    router_changed(s);
    return 0;
}

static const VMStateDescription vmstate_router = {
    .name = TYPE_SGI_SN1_ROUTER,
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = router_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(revision, SGISN1RouterState),
        VMSTATE_UINT8_EQUAL(synthetic_diagnostics, SGISN1RouterState),
        VMSTATE_UINT64_ARRAY(diagnostic, SGISN1RouterState, 3),
        VMSTATE_UINT64(protection, SGISN1RouterState),
        VMSTATE_UINT64_ARRAY(global, SGISN1RouterState, 2),
        VMSTATE_UINT64(global_port_def, SGISN1RouterState),
        VMSTATE_UINT64(jbus_control, SGISN1RouterState),
        VMSTATE_UINT64_ARRAY(scratch, SGISN1RouterState, 5),
        VMSTATE_UINT64_ARRAY(parameter, SGISN1RouterState,
                             SGI_SN1_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(channel_test, SGISN1RouterState,
                             SGI_SN1_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(remote_reset_mask, SGISN1RouterState,
                             SGI_SN1_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(inactive_barrier, SGISN1RouterState,
                             INACTIVE_BARRIER_CONTEXTS),
        VMSTATE_UINT64_2DARRAY(local, SGISN1RouterState,
                             SGI_SN1_ROUTER_PORTS, 128),
        VMSTATE_UINT64_2DARRAY(meta, SGISN1RouterState,
                             SGI_SN1_ROUTER_PORTS, 16),
        VMSTATE_END_OF_LIST()
    },
};

static void router_realize(DeviceState *dev, Error **errp)
{
    SGISN1RouterState *s = SGI_SN1_ROUTER(dev);

    if (s->revision != 1) {
        error_setg(errp, "SN1 router supports only inferred chip revision one");
        return;
    }
    if (s->uart) {
        if (!s->uart->realized) {
            error_setg(errp, "SN1 router L1 UART must be realized first");
            return;
        }
        address_space_init(&s->uart_as,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->uart), 0),
            "sn1-router.uart");
        s->uart_as_ready = true;
    }
    qemu_register_resettable(OBJECT(dev));
}

static void router_unrealize(DeviceState *dev)
{
    SGISN1RouterState *s = SGI_SN1_ROUTER(dev);

    qemu_unregister_resettable(OBJECT(dev));
    if (s->uart_as_ready) {
        address_space_destroy(&s->uart_as);
        s->uart_as_ready = false;
    }
}

void sgi_sn1_router_lock_cables(SGISN1RouterState *s)
{
    s->cables_locked = true;
}

void sgi_sn1_router_enable_synthetic_diagnostics(SGISN1RouterState *s)
{
    g_assert(!DEVICE(s)->realized);
    s->synthetic_diagnostics = 1;
}

static void router_check_cable(const Object *obj, const char *name,
                                Object *value, Error **errp)
{
    SGISN1RouterState *s = SGI_SN1_ROUTER((Object *)obj);
    unsigned port;

    if (sscanf(name, "port%u-cable", &port) != 1 ||
        port < 1 || port > SGI_SN1_ROUTER_PORTS) {
        error_setg(errp, "invalid router cable property");
        return;
    }
    if (s->cables_locked && value != OBJECT(s->cable[port - 1])) {
        error_setg(errp, "machine network attachments are fixed");
    }
}

static void router_init(Object *obj)
{
    SGISN1RouterState *s = SGI_SN1_ROUTER(obj);

    memory_region_init_io(&s->mmio, obj, &router_ops, s,
                          TYPE_SGI_SN1_ROUTER, ROUTER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    for (unsigned p = 1; p <= SGI_SN1_ROUTER_PORTS; p++) {
        g_autofree char *name = g_strdup_printf("port%u-cable", p);

        object_property_add_link(obj, name, TYPE_SGI_NET_CABLE,
            (Object **)&s->cable[p - 1], router_check_cable, 0);
    }
    object_property_add_link(obj, "l1-uart", TYPE_SGI_L1_UART,
        (Object **)&s->uart, qdev_prop_allow_set_link_before_realize,
        OBJ_PROP_LINK_STRONG);
}

static const Property router_properties[] = {
    DEFINE_PROP_UINT8("chip-revision", SGISN1RouterState, revision, 1),
};

static void router_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGINetPortClass *pc = SGI_NET_PORT_CLASS(oc);

    dc->desc = "Eight-port SN1 NUMAlink router (bounded functional model)";
    dc->realize = router_realize;
    dc->unrealize = router_unrealize;
    dc->vmsd = &vmstate_router;
    dc->user_creatable = true;
    device_class_set_props(dc, router_properties);
    rc->phases.enter = router_reset_enter;
    rc->phases.exit = router_reset_exit;
    pc->forwards = router_forwards;
    pc->peer_changed = router_peer_changed;
    pc->receive = router_receive;
}

static const TypeInfo router_info = {
    .name = TYPE_SGI_SN1_ROUTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGISN1RouterState),
    .instance_init = router_init,
    .class_init = router_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_NET_PORT }, { }
    },
};

static void router_register(void)
{
    type_register_static(&router_info);
}

type_init(router_register)
