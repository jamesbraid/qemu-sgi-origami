/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Host test requests use a CPU's physical address space through QOM. */
#include "qemu/osdep.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "system/qtest.h"

#define TYPE_MEMORY_TRANSACTION_TEST "memory-transaction-test"
OBJECT_DECLARE_SIMPLE_TYPE(MemoryTransactionTest, MEMORY_TRANSACTION_TEST)

struct MemoryTransactionTest {
    DeviceState parent_obj;
    char *result;
};

/* CPU -1 selects first_cpu; nonnegative indices are resolved per request. */
static void transaction_request(Object *obj, const char *request, Error **errp)
{
    MemoryTransactionTest *s = MEMORY_TRANSACTION_TEST(obj);
    g_auto(GStrv) args = g_strsplit(request, " ", -1);
    uint8_t bytes[8];
    uint64_t address;
    unsigned size;
    int index;
    CPUState *cpu;
    bool write;
    MemTxResult result;
    GString *reply;

    if (!qtest_driver() || !DEVICE(obj)->realized) {
        error_setg(errp, "Memory transactions require a realized "
                   "qtest fixture");
        return;
    }
    if (g_strv_length(args) != 4 ||
        (strcmp(args[0], "read") && strcmp(args[0], "write")) ||
        qemu_strtoi(args[1], NULL, 0, &index) || index < -1 ||
        qemu_strtou64(args[2], NULL, 0, &address)) {
        error_setg(errp, "Expected read CPU ADDRESS SIZE or "
                   "write CPU ADDRESS HEXBYTES");
        return;
    }
    cpu = index == -1 ? first_cpu : qemu_get_cpu(index);
    if (!cpu || !cpu->as) {
        error_setg(errp, "CPU %d has no address space", index);
        return;
    }
    write = !strcmp(args[0], "write");
    if (write) {
        size = strlen(args[3]) / 2;
        if (strlen(args[3]) != size * 2) {
            error_setg(errp, "Write data must contain complete hex bytes");
            return;
        }
    } else if (qemu_strtoui(args[3], NULL, 0, &size)) {
        error_setg(errp, "Invalid read size");
        return;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        error_setg(errp, "Transaction size must be 1, 2, 4 or 8 bytes");
        return;
    }
    if (address > UINT64_MAX - (size - 1)) {
        error_setg(errp, "Transaction address range overflows");
        return;
    }
    memset(bytes, 0xff, sizeof(bytes));
    if (write) {
        for (unsigned i = 0; i < size; i++) {
            int high = g_ascii_xdigit_value(args[3][2 * i]);
            int low = g_ascii_xdigit_value(args[3][2 * i + 1]);

            if (high < 0 || low < 0) {
                error_setg(errp, "Invalid hex write data");
                return;
            }
            bytes[i] = high << 4 | low;
        }
        result = address_space_write(cpu->as, address, MEMTXATTRS_UNSPECIFIED,
                                     bytes, size);
    } else {
        result = address_space_read(cpu->as, address, MEMTXATTRS_UNSPECIFIED,
                                    bytes, size);
    }
    reply = g_string_new(NULL);
    g_string_append_printf(reply, "0x%08x ", result);
    for (unsigned i = 0; i < size; i++) {
        g_string_append_printf(reply, "%02x", bytes[i]);
    }
    g_free(s->result);
    s->result = g_string_free(reply, false);
}

static char *transaction_result(Object *obj, Error **errp)
{
    MemoryTransactionTest *s = MEMORY_TRANSACTION_TEST(obj);

    if (!s->result) {
        error_setg(errp, "No memory transaction has completed");
        return NULL;
    }
    return g_strdup(s->result);
}

static void transaction_realize(DeviceState *dev, Error **errp)
{
    if (!qtest_driver()) {
        error_setg(errp, "Memory transactions require a qtest server");
    }
}

static void transaction_unrealize(DeviceState *dev)
{
    g_clear_pointer(&MEMORY_TRANSACTION_TEST(dev)->result, g_free);
}

static void transaction_finalize(Object *obj)
{
    g_free(MEMORY_TRANSACTION_TEST(obj)->result);
}

/*
 * The last completed response survives guest reset and rejected requests.
 * Unrealize clears it. Migration transfers an empty test-device section;
 * each process keeps its own response, independent of guest VMState.
 */
static const VMStateDescription transaction_vmstate = {
    .name = TYPE_MEMORY_TRANSACTION_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void transaction_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = transaction_realize;
    dc->unrealize = transaction_unrealize;
    dc->vmsd = &transaction_vmstate;
    object_class_property_add_str(oc, "request", NULL, transaction_request);
    object_class_property_add_str(oc, "result", transaction_result, NULL);
}

static const TypeInfo transaction_type = {
    .name = TYPE_MEMORY_TRANSACTION_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MemoryTransactionTest),
    .instance_finalize = transaction_finalize,
    .class_init = transaction_class_init,
};

static void transaction_register_types(void)
{
    type_register_static(&transaction_type);
}

type_init(transaction_register_types)
