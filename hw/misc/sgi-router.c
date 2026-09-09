/*
 * SGI Router ASIC register target and programmed-route lookup.
 *
 * A Router has no processor-visible register window.  The machine-owned SN0
 * fabric reaches this device through sgi_router_access() after walking a
 * vector.  Routed reply construction stays in the fabric because the
 * Router's reply-source encoding is not established.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/misc/sgi-router.h"
#include "migration/vmstate.h"
#include "system/reset.h"

#define RR_STATUS_REV_ID       0x00000
#define RR_PORT_RESET          0x00008
#define RR_PROTECTION_CONFIG   0x00010
#define RR_GLOBAL_PARMS        0x00018
#define RR_SCRATCH_REG0        0x00020
#define RR_SCRATCH_REG1        0x00028
#define RR_DIAG_PARMS          0x00030
#define RR_NIC_ULAN            0x00038
#define RR_BIST_DATA           0x00050
#define RR_BIST_READY          0x00058
#define RR_BIST_COUNT_TARGET   0x00060
#define RR_BIST_SHIFT_LOAD     0x00068
#define RR_BIST_SHIFT_UNLOAD   0x00070
#define RR_BIST_ENTER_RUN      0x00078

#define RR_PORT_BLOCK_FIRST    1
#define RR_PORT_BLOCK_LAST     6
#define RR_PORT_PARMS          0x0000
#define RR_STATUS_ERROR        0x0008
#define RR_HISTOGRAM           0x0010
#define RR_RESET_MASK          0x0018
#define RR_ERROR_CLEAR         0x0088

#define RR_META_TABLE_BASE     0x70000
#define RR_META_TABLE_COUNT    32
#define RR_LOCAL_TABLE_BASE    0x70100
#define RR_LOCAL_TABLE_COUNT   16
#define RR_TABLE_STRIDE        8

#define RR_STATUS_IDENTITY         UINT64_C(1)
#define RR_STATUS_REVISION         (UINT64_C(1) << 4)
#define RR_STATUS_INPUTS_SHIFT     8
#define RR_STATUS_OUTPUTS_MASK     (UINT64_C(0xf) << 12)
#define RR_STATUS_OUTPUTS_RESET    (UINT64_C(1) << 12)
#define RR_STATUS_LED_MASK         (UINT64_C(0x3f) << 16)
#define RR_STATUS_UNUSED_MASK      (UINT64_C(3) << 22)
#define RR_STATUS_CONTROL_MASK     (RR_STATUS_OUTPUTS_MASK | \
                                    RR_STATUS_LED_MASK | \
                                    RR_STATUS_UNUSED_MASK)
#define RR_STATUS_LOCAL_ERROR_MASK (UINT64_C(0xf) << 24)
#define RR_STATUS_INPUT_PORT_SHIFT 46

#define RR_PROTECTION_MASK     UINT64_C(0x1fffff)
#define RR_PROTECTION_RESET    UINT64_C(0x3f)

#define RR_GLOBAL_READ_ONLY    (UINT64_C(3) << 10)
#define RR_GLOBAL_RESET        UINT64_C(0x3fffffe0a01ffff1)

#define RR_SCRATCH1_MASK       UINT64_C(0xffff)
#define RR_DIAG_MASK           UINT64_C(0xfffff)

#define RR_NIC_PHASE_LOW       (UINT64_C(0x7f) << 20)
#define RR_NIC_PHASE_HIGH      (UINT64_C(0x7f) << 27)
#define RR_NIC_PHASE_MASK      (RR_NIC_PHASE_LOW | RR_NIC_PHASE_HIGH)
#define RR_NIC_SAMPLE_MASK     (UINT64_C(0xff) << 2)
#define RR_NIC_PULSE_MASK      (UINT64_C(0x3ff) << 10)
#define RR_NIC_DONE            (UINT64_C(1) << 1)
#define RR_NIC_RESET           ((UINT64_C(0x31) << 20) | \
                                (UINT64_C(0x31) << 27) | RR_NIC_DONE)
#define RR_NIC_STATE_MASK      (RR_NIC_PHASE_MASK | RR_NIC_DONE | UINT64_C(1))

#define RR_PORT_PARMS_MASK     UINT64_C(0xfffff)
#define RR_PORT_PARMS_RESET    UINT64_C(0x1bf0)
#define RR_PORT_ERROR_MASK     UINT64_C(0x3ffffffff)
#define RR_RESET_MASK_MASK     UINT64_C(0x3f)
#define RR_TABLE_MASK          UINT64_C(0xffffff)

#define RR_PROTECTION_FORCE_LOCAL  (UINT64_C(1) << 12)
#define RR_PROTECTION_META_VALID   (UINT64_C(1) << 11)
#define RR_PROTECTION_META_SHIFT   6
#define RR_PROTECTION_META_MASK    UINT64_C(0x1f)

struct SGIRouterState {
    DeviceState parent_obj;

    uint8_t chip_inputs;

    void (*changed)(void *opaque);
    void *changed_opaque;

    uint64_t status_control;
    uint64_t local_errors;
    uint64_t protection_config;
    uint64_t global_parms;
    uint64_t scratch0;
    uint16_t scratch1;
    uint64_t diag_parms;
    uint64_t nic_ulan;
    uint64_t bist_data;

    uint64_t port_parms[SGI_ROUTER_PORTS];
    uint64_t port_errors[SGI_ROUTER_PORTS];
    uint64_t histograms[SGI_ROUTER_PORTS];
    uint8_t reset_masks[SGI_ROUTER_PORTS];
    bool port_working[SGI_ROUTER_PORTS];
    bool port_eight_bit[SGI_ROUTER_PORTS];

    uint32_t meta_table[RR_META_TABLE_COUNT];
    uint32_t local_table[RR_LOCAL_TABLE_COUNT];
    bool meta_valid[RR_META_TABLE_COUNT];
    bool local_valid[RR_LOCAL_TABLE_COUNT];
};

static void sgi_router_changed(SGIRouterState *s)
{
    if (s->changed) {
        s->changed(s->changed_opaque);
    }
}

static uint64_t sgi_router_status(SGIRouterState *s,
                                  unsigned int arrival_port)
{
    uint64_t status = RR_STATUS_IDENTITY | RR_STATUS_REVISION |
                      ((uint64_t)s->chip_inputs << RR_STATUS_INPUTS_SHIFT) |
                      s->status_control | s->local_errors |
                      ((uint64_t)arrival_port << RR_STATUS_INPUT_PORT_SHIFT);
    unsigned int port;

    for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
        if (s->port_working[port - 1]) {
            status |= UINT64_C(1) << (26 + 3 * port);
        }
        if (s->port_eight_bit[port - 1]) {
            status |= UINT64_C(1) << (27 + 3 * port);
        }
    }
    return status;
}

static uint32_t *sgi_router_table_entry(SGIRouterState *s, hwaddr addr,
                                        bool **valid)
{
    unsigned int index;

    if ((addr & (RR_TABLE_STRIDE - 1)) != 0) {
        return NULL;
    }
    if (addr >= RR_META_TABLE_BASE &&
        addr < RR_META_TABLE_BASE + RR_META_TABLE_COUNT * RR_TABLE_STRIDE) {
        index = (addr - RR_META_TABLE_BASE) / RR_TABLE_STRIDE;
        *valid = &s->meta_valid[index];
        return &s->meta_table[index];
    }
    if (addr >= RR_LOCAL_TABLE_BASE &&
        addr < RR_LOCAL_TABLE_BASE + RR_LOCAL_TABLE_COUNT * RR_TABLE_STRIDE) {
        index = (addr - RR_LOCAL_TABLE_BASE) / RR_TABLE_STRIDE;
        *valid = &s->local_valid[index];
        return &s->local_table[index];
    }
    return NULL;
}

static bool sgi_router_port_register(hwaddr addr, unsigned int *port,
                                     hwaddr *offset)
{
    unsigned int block = extract64(addr, 16, 4);

    if (block < RR_PORT_BLOCK_FIRST || block > RR_PORT_BLOCK_LAST) {
        return false;
    }
    *port = block - 1;
    *offset = addr & 0xffff;
    return true;
}

static bool sgi_router_read(SGIRouterState *s, unsigned int arrival_port,
                            hwaddr addr, uint64_t *value)
{
    uint32_t *table;
    bool *table_valid = NULL;
    unsigned int port;
    hwaddr offset;

    table = sgi_router_table_entry(s, addr, &table_valid);
    if (table) {
        *value = *table;
        return true;
    }

    if (sgi_router_port_register(addr, &port, &offset)) {
        switch (offset) {
        case RR_PORT_PARMS:
            *value = s->port_parms[port];
            return true;
        case RR_STATUS_ERROR:
            *value = s->port_errors[port];
            return true;
        case RR_HISTOGRAM:
            *value = s->histograms[port];
            return true;
        case RR_RESET_MASK:
            *value = s->reset_masks[port];
            return true;
        case RR_ERROR_CLEAR:
            *value = s->port_errors[port];
            s->port_errors[port] = 0;
            return true;
        default:
            return false;
        }
    }

    switch (addr) {
    case RR_STATUS_REV_ID:
        *value = sgi_router_status(s, arrival_port);
        return true;
    case RR_PROTECTION_CONFIG:
        *value = s->protection_config;
        return true;
    case RR_GLOBAL_PARMS:
        *value = s->global_parms;
        return true;
    case RR_SCRATCH_REG0:
        *value = s->scratch0;
        return true;
    case RR_SCRATCH_REG1:
        *value = s->scratch1;
        return true;
    case RR_DIAG_PARMS:
        *value = s->diag_parms;
        return true;
    case RR_NIC_ULAN:
        *value = s->nic_ulan;
        return true;
    case RR_BIST_DATA:
        *value = s->bist_data;
        return true;
    case RR_BIST_READY:
        *value = 0;
        return true;
    default:
        return false;
    }
}

static bool sgi_router_write(SGIRouterState *s, hwaddr addr, uint64_t value)
{
    uint32_t *table;
    bool *table_valid = NULL;
    unsigned int port;
    hwaddr offset;

    table = sgi_router_table_entry(s, addr, &table_valid);
    if (table) {
        *table = value & RR_TABLE_MASK;
        *table_valid = true;
        sgi_router_changed(s);
        return true;
    }

    if (sgi_router_port_register(addr, &port, &offset)) {
        switch (offset) {
        case RR_PORT_PARMS:
            s->port_parms[port] = value & RR_PORT_PARMS_MASK;
            return true;
        case RR_STATUS_ERROR:
            s->port_errors[port] &= ~(value & RR_PORT_ERROR_MASK);
            return true;
        case RR_HISTOGRAM:
            qemu_log_mask(LOG_UNIMP,
                          "sgi-router: histogram clearing and progression "
                          "are not implemented\n");
            return false;
        case RR_RESET_MASK:
            s->reset_masks[port] = value & RR_RESET_MASK_MASK;
            return true;
        case RR_ERROR_CLEAR:
            if (value == 0) {
                s->port_errors[port] = 0;
                return true;
            }
            qemu_log_mask(LOG_UNIMP,
                          "sgi-router: nonzero write to the error-clearing "
                          "alias is not defined\n");
            return false;
        default:
            return false;
        }
    }

    switch (addr) {
    case RR_STATUS_REV_ID:
        s->local_errors &= ~(value & RR_STATUS_LOCAL_ERROR_MASK);
        s->status_control = value & RR_STATUS_CONTROL_MASK;
        return true;
    case RR_PORT_RESET:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-router: administration, port, and warm reset "
                      "extent is not implemented\n");
        return false;
    case RR_PROTECTION_CONFIG:
        s->protection_config = value & RR_PROTECTION_MASK;
        sgi_router_changed(s);
        return true;
    case RR_GLOBAL_PARMS:
        s->global_parms = (value & ~RR_GLOBAL_READ_ONLY) |
                          RR_GLOBAL_READ_ONLY;
        return true;
    case RR_SCRATCH_REG0:
        s->scratch0 = value;
        return true;
    case RR_SCRATCH_REG1:
        s->scratch1 = value & RR_SCRATCH1_MASK;
        return true;
    case RR_DIAG_PARMS:
        s->diag_parms = value & RR_DIAG_MASK;
        sgi_router_changed(s);
        return true;
    case RR_NIC_ULAN:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-router: one-wire bit slots are not implemented\n");
        return false;
    case RR_BIST_DATA:
        s->bist_data = value;
        return true;
    case RR_BIST_READY:
        return true;
    case RR_BIST_COUNT_TARGET:
    case RR_BIST_SHIFT_LOAD:
    case RR_BIST_SHIFT_UNLOAD:
    case RR_BIST_ENTER_RUN:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-router: built-in self-test is not implemented\n");
        return false;
    default:
        return false;
    }
}

bool sgi_router_access(SGIRouterState *s, unsigned int arrival_port,
                       SN0FabricVectorAccess *access)
{
    bool accepted;

    if (arrival_port < 1 || arrival_port > SGI_ROUTER_PORTS ||
        (access->addr & 7) != 0 || access->addr >= (UINT64_C(1) << 20)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-router: invalid vector access at 0x%" HWADDR_PRIx
                      " from port %u\n", access->addr, arrival_port);
        return false;
    }

    switch (access->op) {
    case SN0_FABRIC_VECTOR_READ:
        accepted = sgi_router_read(s, arrival_port, access->addr,
                                   &access->value);
        break;
    case SN0_FABRIC_VECTOR_WRITE:
        accepted = sgi_router_write(s, access->addr, access->data);
        break;
    case SN0_FABRIC_VECTOR_EXCHANGE:
        if (access->addr != RR_SCRATCH_REG1) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-router: exchange outside scratch register 1 "
                          "is not established\n");
            return false;
        }
        access->value = s->scratch1;
        if (s->scratch1 == 0) {
            s->scratch1 = access->data & RR_SCRATCH1_MASK;
        }
        return true;
    default:
        return false;
    }

    if (!accepted) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-router: unsupported vector operation at 0x%"
                      HWADDR_PRIx "\n", access->addr);
    }
    return accepted;
}

bool sgi_router_port_eight_bit_forced(SGIRouterState *s, unsigned int port)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    return s->diag_parms & (UINT64_C(1) << (11 + port));
}

bool sgi_router_port_enabled(SGIRouterState *s, unsigned int port)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    return !(s->diag_parms & (UINT64_C(1) << (5 + port)));
}

void sgi_router_set_changed(SGIRouterState *s, void (*changed)(void *),
                            void *opaque)
{
    s->changed = changed;
    s->changed_opaque = opaque;
}

void sgi_router_set_port_link(SGIRouterState *s, unsigned int port,
                              bool working, bool eight_bit)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    s->port_working[port - 1] = working;
    s->port_eight_bit[port - 1] = working && eight_bit;
}

void sgi_router_latch_local_error(SGIRouterState *s, uint64_t errors)
{
    s->local_errors |= errors & RR_STATUS_LOCAL_ERROR_MASK;
}

void sgi_router_latch_port_error(SGIRouterState *s, unsigned int port,
                                 uint64_t errors)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    s->port_errors[port - 1] |= errors & RR_PORT_ERROR_MASK;
}

bool sgi_router_resolve_turn(unsigned int entry_port, uint8_t turn,
                             unsigned int *exit_port)
{
    if (entry_port < 1 || entry_port > SGI_ROUTER_PORTS ||
        turn > 5) {
        return false;
    }
    *exit_port = ((entry_port - 1 + turn) % SGI_ROUTER_PORTS) + 1;
    return true;
}

bool sgi_router_route(SGIRouterState *s, unsigned int entry_port,
                      uint8_t turn, uint16_t destination,
                      unsigned int *exit_port, uint8_t *next_turn)
{
    uint32_t entry;
    bool local;
    unsigned int index;

    if (!sgi_router_resolve_turn(entry_port, turn, exit_port)) {
        return false;
    }

    local = s->protection_config & RR_PROTECTION_FORCE_LOCAL;
    if (!local) {
        uint8_t meta;

        if (!(s->protection_config & RR_PROTECTION_META_VALID)) {
            return false;
        }
        meta = (s->protection_config >> RR_PROTECTION_META_SHIFT) &
               RR_PROTECTION_META_MASK;
        local = ((destination >> 4) & 0x1f) == meta;
    }

    if (local) {
        index = destination & 0xf;
        if (!s->local_valid[index]) {
            return false;
        }
        entry = s->local_table[index];
    } else {
        index = (destination >> 4) & 0x1f;
        if (!s->meta_valid[index]) {
            return false;
        }
        entry = s->meta_table[index];
    }
    *next_turn = extract32(entry, 4 * (*exit_port - 1), 4);
    return true;
}

static void sgi_router_reset_enter(Object *obj, ResetType type)
{
    SGIRouterState *s = SGI_ROUTER(obj);
    unsigned int port;

    s->status_control = RR_STATUS_OUTPUTS_RESET;
    s->local_errors = 0;
    s->protection_config = RR_PROTECTION_RESET;
    s->global_parms = RR_GLOBAL_RESET;
    s->scratch0 = 0;
    s->scratch1 = 0;
    s->diag_parms = 0;
    s->nic_ulan = RR_NIC_RESET;
    s->bist_data = 0;

    for (port = 0; port < SGI_ROUTER_PORTS; port++) {
        s->port_parms[port] = RR_PORT_PARMS_RESET;
        s->port_errors[port] = 0;
        s->histograms[port] = 0;
        s->reset_masks[port] = RR_RESET_MASK_MASK;
        s->port_working[port] = false;
        s->port_eight_bit[port] = false;
    }
    memset(s->meta_table, 0, sizeof(s->meta_table));
    memset(s->local_table, 0, sizeof(s->local_table));
    memset(s->meta_valid, 0, sizeof(s->meta_valid));
    memset(s->local_valid, 0, sizeof(s->local_valid));
}

static void sgi_router_reset_exit(Object *obj, ResetType type)
{
    sgi_router_changed(SGI_ROUTER(obj));
}

static bool sgi_router_array_masked(const uint64_t *values, size_t count,
                                    uint64_t mask)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (values[i] & ~mask) {
            return false;
        }
    }
    return true;
}

static int sgi_router_post_load(void *opaque, int version_id)
{
    SGIRouterState *s = opaque;
    unsigned int i;

    if ((s->status_control & ~RR_STATUS_CONTROL_MASK) ||
        (s->local_errors & ~RR_STATUS_LOCAL_ERROR_MASK) ||
        (s->protection_config & ~RR_PROTECTION_MASK) ||
        (s->global_parms & RR_GLOBAL_READ_ONLY) != RR_GLOBAL_READ_ONLY ||
        (s->diag_parms & ~RR_DIAG_MASK) ||
        (s->nic_ulan & ~RR_NIC_STATE_MASK) ||
        !sgi_router_array_masked(s->port_parms, SGI_ROUTER_PORTS,
                                 RR_PORT_PARMS_MASK) ||
        !sgi_router_array_masked(s->port_errors, SGI_ROUTER_PORTS,
                                 RR_PORT_ERROR_MASK)) {
        return -EINVAL;
    }
    for (i = 0; i < SGI_ROUTER_PORTS; i++) {
        if (s->reset_masks[i] & ~RR_RESET_MASK_MASK ||
            (!s->port_working[i] && s->port_eight_bit[i])) {
            return -EINVAL;
        }
    }
    for (i = 0; i < RR_META_TABLE_COUNT; i++) {
        if (s->meta_table[i] & ~RR_TABLE_MASK) {
            return -EINVAL;
        }
    }
    for (i = 0; i < RR_LOCAL_TABLE_COUNT; i++) {
        if (s->local_table[i] & ~RR_TABLE_MASK) {
            return -EINVAL;
        }
    }
    sgi_router_changed(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_router = {
    .name = "sgi-router",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_router_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(status_control, SGIRouterState),
        VMSTATE_UINT64(local_errors, SGIRouterState),
        VMSTATE_UINT64(protection_config, SGIRouterState),
        VMSTATE_UINT64(global_parms, SGIRouterState),
        VMSTATE_UINT64(scratch0, SGIRouterState),
        VMSTATE_UINT16(scratch1, SGIRouterState),
        VMSTATE_UINT64(diag_parms, SGIRouterState),
        VMSTATE_UINT64(nic_ulan, SGIRouterState),
        VMSTATE_UINT64(bist_data, SGIRouterState),
        VMSTATE_UINT64_ARRAY(port_parms, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(port_errors, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(histograms, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT8_ARRAY(reset_masks, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_BOOL_ARRAY(port_working, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_BOOL_ARRAY(port_eight_bit, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT32_ARRAY(meta_table, SGIRouterState, RR_META_TABLE_COUNT),
        VMSTATE_UINT32_ARRAY(local_table, SGIRouterState,
                             RR_LOCAL_TABLE_COUNT),
        VMSTATE_BOOL_ARRAY(meta_valid, SGIRouterState, RR_META_TABLE_COUNT),
        VMSTATE_BOOL_ARRAY(local_valid, SGIRouterState,
                           RR_LOCAL_TABLE_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_router_realize(DeviceState *dev, Error **errp)
{
    SGIRouterState *s = SGI_ROUTER(dev);

    if (s->chip_inputs > 0xf) {
        error_setg(errp, "chip-inputs must fit in four bits");
        return;
    }
    qemu_register_resettable(OBJECT(dev));
}

static void sgi_router_unrealize(DeviceState *dev)
{
    qemu_unregister_resettable(OBJECT(dev));
}

static const Property sgi_router_properties[] = {
    DEFINE_PROP_UINT8("chip-inputs", SGIRouterState, chip_inputs, 1),
};

static void sgi_router_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI Router CrayLink switch";
    dc->realize = sgi_router_realize;
    dc->unrealize = sgi_router_unrealize;
    dc->vmsd = &vmstate_sgi_router;
    dc->user_creatable = false;
    device_class_set_props(dc, sgi_router_properties);
    rc->phases.enter = sgi_router_reset_enter;
    rc->phases.exit = sgi_router_reset_exit;
}

static const TypeInfo sgi_router_types[] = {
    {
        .name = TYPE_SGI_ROUTER,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIRouterState),
        .class_init = sgi_router_class_init,
    },
};

DEFINE_TYPES(sgi_router_types)
