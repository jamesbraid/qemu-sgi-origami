/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MMIO test device for command-port framing and owner callbacks. */
/*
 * Its register layout and selectors are synthetic, not a board address map or
 * command grammar.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-command-port.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_SGI_COMMAND_PORT_TEST "sgi-command-port-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGICommandPortTestState, SGI_COMMAND_PORT_TEST)

#define REG_PUSH               0x00
#define REG_CREDIT             0x08
#define REG_KICK               0x10
#define REG_PACKET_COUNT       0x18
#define REG_LAST_SELECTOR      0x20
#define REG_LAST_ARGC          0x28
#define REG_LAST_ARGS          0x30
#define REG_UNSUPPORTED_COUNT  (REG_LAST_ARGS + SGI_COMMAND_PORT_MAX_ARGS * 8)
#define REG_LAST_UNSUPPORTED   (REG_UNSUPPORTED_COUNT + 8)
#define SGI_COMMAND_PORT_TEST_SIZE (REG_LAST_UNSUPPORTED + 8)

/* Synthetic: exercises zero-arg, small and largest-known-real-size framing. */
static const SGICommandPortSelector test_selectors[] = {
    { 0x1, 0 },
    { 0x2, 3 },
    { 0x3, 11 },
};

struct SGICommandPortTestState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    SGICommandPort port;

    uint32_t credit_value;
    uint32_t packet_count;
    uint32_t last_selector;
    uint32_t last_argc;
    uint32_t last_args[SGI_COMMAND_PORT_MAX_ARGS];
    uint32_t unsupported_count;
    uint32_t last_unsupported;
};

static void test_packet(void *opaque, unsigned port_id, uint32_t selector,
                        const uint32_t *args, unsigned argc)
{
    SGICommandPortTestState *s = opaque;

    s->packet_count++;
    s->last_selector = selector;
    s->last_argc = argc;
    memset(s->last_args, 0, sizeof(s->last_args));
    memcpy(s->last_args, args, argc * sizeof(uint32_t));
}

static void test_unsupported(void *opaque, unsigned port_id, uint32_t word)
{
    SGICommandPortTestState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "sgi-command-port-test: unsupported "
                  "selector 0x%" PRIx32 " on port %u\n", word, port_id);
    s->unsupported_count++;
    s->last_unsupported = word;
}

static uint32_t test_credit(void *opaque, unsigned port_id)
{
    SGICommandPortTestState *s = opaque;

    return s->credit_value;
}

static const SGICommandPortOps test_command_port_ops = {
    .packet = test_packet,
    .unsupported = test_unsupported,
    .credit = test_credit,
};

static uint64_t test_read(void *opaque, hwaddr offset, unsigned size)
{
    SGICommandPortTestState *s = opaque;

    if (offset >= REG_LAST_ARGS &&
        offset < REG_LAST_ARGS + SGI_COMMAND_PORT_MAX_ARGS * 8) {
        return s->last_args[(offset - REG_LAST_ARGS) / 8];
    }
    switch (offset) {
    case REG_PUSH:
        return 0;
    case REG_CREDIT:
        return sgi_command_port_credit_read(&s->port);
    case REG_KICK:
        return sgi_command_port_kick_read(&s->port);
    case REG_PACKET_COUNT:
        return s->packet_count;
    case REG_LAST_SELECTOR:
        return s->last_selector;
    case REG_LAST_ARGC:
        return s->last_argc;
    case REG_UNSUPPORTED_COUNT:
        return s->unsupported_count;
    case REG_LAST_UNSUPPORTED:
        return s->last_unsupported;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-command-port-test: unimplemented read "
                      "at 0x%" HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void test_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    SGICommandPortTestState *s = opaque;

    if (offset >= REG_LAST_ARGS &&
        offset < REG_LAST_ARGS + SGI_COMMAND_PORT_MAX_ARGS * 8) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-command-port-test: write to "
                      "read-only register 0x%" HWADDR_PRIx "\n", offset);
        return;
    }
    switch (offset) {
    case REG_PUSH:
        sgi_command_port_write(&s->port, value, size);
        break;
    case REG_CREDIT:
        s->credit_value = value;
        break;
    case REG_KICK:
        sgi_command_port_kick_write(&s->port, value);
        break;
    case REG_PACKET_COUNT:
    case REG_LAST_SELECTOR:
    case REG_LAST_ARGC:
    case REG_UNSUPPORTED_COUNT:
    case REG_LAST_UNSUPPORTED:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-command-port-test: write to "
                      "read-only register 0x%" HWADDR_PRIx "\n", offset);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-command-port-test: unimplemented write "
                      "at 0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                      offset, value);
        break;
    }
}

static const MemoryRegionOps test_mmio_ops = {
    .read = test_read,
    .write = test_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void test_reset_enter(Object *obj, ResetType type)
{
    SGICommandPortTestState *s = SGI_COMMAND_PORT_TEST(obj);

    sgi_command_port_reset(&s->port);
    s->credit_value = 0;
    s->packet_count = 0;
    s->last_selector = 0;
    s->last_argc = 0;
    memset(s->last_args, 0, sizeof(s->last_args));
    s->unsupported_count = 0;
    s->last_unsupported = 0;
}

static const VMStateDescription vmstate_command_port = {
    .name = "sgi-command-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(kick, SGICommandPort),
        VMSTATE_BOOL(framing, SGICommandPort),
        VMSTATE_UINT32(selector, SGICommandPort),
        VMSTATE_UINT32(argc_expected, SGICommandPort),
        VMSTATE_UINT32(argc_have, SGICommandPort),
        VMSTATE_UINT32_ARRAY(args, SGICommandPort, SGI_COMMAND_PORT_MAX_ARGS),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_test = {
    .name = TYPE_SGI_COMMAND_PORT_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(port, SGICommandPortTestState, 1,
                       vmstate_command_port, SGICommandPort),
        VMSTATE_UINT32(credit_value, SGICommandPortTestState),
        VMSTATE_UINT32(packet_count, SGICommandPortTestState),
        VMSTATE_UINT32(last_selector, SGICommandPortTestState),
        VMSTATE_UINT32(last_argc, SGICommandPortTestState),
        VMSTATE_UINT32_ARRAY(last_args, SGICommandPortTestState,
                             SGI_COMMAND_PORT_MAX_ARGS),
        VMSTATE_UINT32(unsupported_count, SGICommandPortTestState),
        VMSTATE_UINT32(last_unsupported, SGICommandPortTestState),
        VMSTATE_END_OF_LIST()
    },
};

static void test_init(Object *obj)
{
    SGICommandPortTestState *s = SGI_COMMAND_PORT_TEST(obj);

    sgi_command_port_init(&s->port, &test_command_port_ops, s, 0,
                          test_selectors, ARRAY_SIZE(test_selectors));
    memory_region_init_io(&s->mmio, obj, &test_mmio_ops, s,
                          TYPE_SGI_COMMAND_PORT_TEST,
                          SGI_COMMAND_PORT_TEST_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "sgi-command-port test fixture";
    dc->vmsd = &vmstate_test;
    dc->user_creatable = true;
    rc->phases.enter = test_reset_enter;
}

static const TypeInfo test_info = {
    .name = TYPE_SGI_COMMAND_PORT_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGICommandPortTestState),
    .instance_init = test_init,
    .class_init = test_class_init,
};

static void test_register_types(void)
{
    type_register_static(&test_info);
}

type_init(test_register_types)
