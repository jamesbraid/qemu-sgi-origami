/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-controlled synthetic R10000 agent. It models no SGI board. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/mips/r10k-coherence.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "hw/core/cpu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "system/memory.h"

#define TYPE_R10K_TEST_AGENT "r10k-test-agent"
OBJECT_DECLARE_SIMPLE_TYPE(R10KTestAgent, R10K_TEST_AGENT)

struct R10KTestAgent {
    DeviceState parent_obj;
    CPUState *cpu;
    MemoryRegion requests;
    MemoryRegion backing;
    uint64_t base;
    uint64_t id;
    uint64_t address;
    uint64_t state;
    uint64_t offset;
    uint64_t notifications;
    uint64_t size;
    uint8_t data[R10K_COHERENCE_BLOCK_MAX];
    R10KExternalTransaction external;
};

static MemTxResult notify_read(void *opaque, hwaddr addr, uint64_t *value,
                                unsigned int size, MemTxAttrs attrs)
{
    R10KTestAgent *s = opaque;

    s->notifications++;
    *value = 0;
    return MEMTX_OK;
}

static const MemoryRegionOps requests_ops = {
    .read_with_attrs = notify_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void control_get(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    R10KTestAgent *s = R10K_TEST_AGENT(obj);
    R10KTransaction t;
    uint64_t value = 0;

    mips_r10k_request_snapshot(s->cpu, !strncmp(name, "writeback-", 10), &t);
    if (!strcmp(name, "request-id") || !strcmp(name, "writeback-id")) {
        value = t.phase ? t.id : 0;
    } else if (!strcmp(name, "request-kind")) {
        value = t.command;
    } else if (!strcmp(name, "request-phase")) {
        value = t.phase;
    } else if (!strcmp(name, "request-address")) {
        value = t.address;
    } else if (!strcmp(name, "request-size")) {
        value = t.size;
    } else if (!strcmp(name, "request-cca")) {
        value = t.cca;
    } else if (!strcmp(name, "request-cause")) {
        value = t.cause;
    } else if (!strcmp(name, "request-former")) {
        value = t.former;
    } else if (!strcmp(name, "notifications")) {
        value = s->notifications;
    } else if (!strcmp(name, "external-state")) {
        value = s->external.former;
    } else if (!strcmp(name, "external-data-state")) {
        value = s->external.data_state;
    } else if (!strcmp(name, "external-have-data")) {
        value = s->external.have_data;
    } else if (!strcmp(name, "data-word")) {
        value = ldq_be_p(s->data + s->offset);
    } else if (!strcmp(name, "memory-word")) {
        value = ldq_be_p(memory_region_get_ram_ptr(&s->backing) + s->offset);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void control_set(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    R10KTestAgent *s = R10K_TEST_AGENT(obj);
    uint64_t value;
    bool ok = true;
    R10KTransaction t;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!strcmp(name, "offset")) {
        if (value > sizeof(s->data) - 8 || value % 8) {
            error_setg(errp, "offset must be an aligned byte offset below 128");
            return;
        }
        s->offset = value;
    } else if (!strcmp(name, "id")) {
        s->id = value;
    } else if (!strcmp(name, "address")) {
        s->address = value;
    } else if (!strcmp(name, "state")) {
        s->state = value;
    } else if (!strcmp(name, "size")) {
        s->size = value;
    } else if (!strcmp(name, "data-word")) {
        stq_be_p(s->data + s->offset, value);
    } else if (!strcmp(name, "memory-word")) {
        stq_be_p(memory_region_get_ram_ptr(&s->backing) + s->offset, value);
    } else if (!strcmp(name, "command")) {
        switch (value) {
        case 1:
            ok = mips_r10k_request_issue(s->cpu, s->id);
            break;
        case 2:
            ok = mips_r10k_response_data(s->cpu, s->id, s->state,
                                        s->data, s->size);
            break;
        case 3: case 4: case 5:
            ok = mips_r10k_response_complete(s->cpu, s->id, value - 3);
            break;
        case 6: case 7: case 8:
            ok = mips_r10k_external_request(s->cpu, s->id, s->address,
                                            value - 6, &s->external);
            if (ok && s->external.have_data) {
                memcpy(s->data, s->external.data, s->external.size);
            }
            break;
        case 9:
            /* The test recipient copies every byte before acknowledging it. */
            ok = mips_r10k_external_consumed(s->cpu, s->id);
            break;
        case 10:
            ok = mips_r10k_external_complete(s->cpu, s->id);
            break;
        case 11:
            ok = mips_r10k_request_snapshot(s->cpu, true, &t);
            if (ok && t.id == s->id) {
                memcpy(s->data, t.data, t.size);
                ok = mips_r10k_writeback_consumed(s->cpu, s->id);
            } else {
                ok = false;
            }
            break;
        case 12:
            ok = mips_r10k_request_snapshot(s->cpu, true, &t);
            if (ok) {
                memcpy(s->data, t.data, t.size);
            }
            break;
        case 13:
            mips_r10k_global_performed(s->cpu, true);
            break;
        case 14:
            mips_r10k_global_performed(s->cpu, false);
            break;
        case 15:
            memcpy(s->data, memory_region_get_ram_ptr(&s->backing),
                    sizeof(s->data));
            break;
        case 16:
            memcpy(memory_region_get_ram_ptr(&s->backing), s->data,
                    sizeof(s->data));
            break;
        default:
            ok = false;
        }
        if (!ok) {
            error_setg(errp, "synthetic agent operation %" PRIu64 " rejected",
                        value);
        }
    }
}

static void agent_realize(DeviceState *dev, Error **errp)
{
    R10KTestAgent *s = R10K_TEST_AGENT(dev);
    static const char *const readable[] = {
        "request-id", "writeback-id", "request-kind", "request-phase",
        "request-address", "request-size", "request-cca", "request-former",
        "request-cause",
        "notifications", "external-state", "external-data-state",
        "external-have-data",
    };
    static const char *const writable[] = {
        "id", "address", "state", "size", "offset", "command",
    };
    unsigned int i;

    s->cpu = qemu_get_cpu(0);
    if (!s->cpu) {
        error_setg(errp, "synthetic agent requires CPU 0");
        return;
    }
    memory_region_init_io(&s->requests, OBJECT(s), &requests_ops, s,
                          "r10k-test-requests", R10K_SYSTEM_REQUEST_SIZE);
    if (!mips_r10k_attach_agent(s->cpu, &s->requests, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->backing, OBJECT(s), "r10k-test-backing",
                                4096, errp)) {
        mips_r10k_detach_agent(s->cpu, &s->requests);
        return;
    }
    memory_region_add_subregion_overlap(get_system_memory(), s->base,
                                        &s->backing, 1);
    s->address = s->base;
    s->size = 128;
    for (i = 0; i < ARRAY_SIZE(readable); i++) {
        object_property_add(OBJECT(s), readable[i], "uint64", control_get,
                            NULL, NULL, NULL);
    }
    for (i = 0; i < ARRAY_SIZE(writable); i++) {
        object_property_add(OBJECT(s), writable[i], "uint64", NULL,
                            control_set, NULL, NULL);
    }
    object_property_add(OBJECT(s), "data-word", "uint64", control_get,
                        control_set, NULL, NULL);
    object_property_add(OBJECT(s), "memory-word", "uint64", control_get,
                        control_set, NULL, NULL);
}

static void agent_unrealize(DeviceState *dev)
{
    R10KTestAgent *s = R10K_TEST_AGENT(dev);

    memory_region_del_subregion(get_system_memory(), &s->backing);
    mips_r10k_detach_agent(s->cpu, &s->requests);
}

static const Property properties[] = {
    DEFINE_PROP_UINT64("base", R10KTestAgent, base, 0x20010000),
};

static const VMStateDescription agent_vmstate = {
    .name = "r10k-test-agent",
    .unmigratable = true,
};

static void agent_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = agent_realize;
    dc->unrealize = agent_unrealize;
    dc->vmsd = &agent_vmstate;
    dc->hotpluggable = false;
    device_class_set_props(dc, properties);
}

static const TypeInfo agent_type = {
    .name = TYPE_R10K_TEST_AGENT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(R10KTestAgent),
    .class_init = agent_class_init,
};

static void agent_register(void)
{
    type_register_static(&agent_type);
}

type_init(agent_register)
