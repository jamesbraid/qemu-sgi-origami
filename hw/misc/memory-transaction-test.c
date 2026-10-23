/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic requests use CPU or global memory address spaces through QOM. */
#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "system/qtest.h"
#include "system/ramblock.h"

#define TYPE_MEMORY_TRANSACTION_TEST "memory-transaction-test"
OBJECT_DECLARE_SIMPLE_TYPE(MemoryTransactionTest, MEMORY_TRANSACTION_TEST)

struct MemoryTransactionTest {
    DeviceState parent_obj;
    char *result;
    MemoryRegion *migrate_ram;
    bool ram_registered;
    MemoryRegion test_memory;
    MemoryRegion fault_memory_io;
    uint8_t bytes[65536];
    bool fault_memory;
    uint64_t fault_memory_base;
    uint64_t fault_start;
    uint64_t fault_end;
    uint32_t fault_mode;
};

static MemTxResult fault_memory_read(void *opaque, hwaddr address,
                                      uint64_t *value, unsigned size,
                                      MemTxAttrs attrs)
{
    MemoryTransactionTest *s = opaque;

    if ((s->fault_mode & 1) && address <= s->fault_end &&
        address + size - 1 >= s->fault_start) {
        *value = 0;
        return MEMTX_ERROR;
    }
    *value = ldn_le_p(s->bytes + address, size);
    return MEMTX_OK;
}

static MemTxResult fault_memory_write(void *opaque, hwaddr address,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    MemoryTransactionTest *s = opaque;

    if ((s->fault_mode & 2) && address <= s->fault_end &&
        address + size - 1 >= s->fault_start) {
        return MEMTX_ERROR;
    }
    stn_le_p(s->bytes + address, size, value);
    return MEMTX_OK;
}

static const MemoryRegionOps fault_memory_ops = {
    .read_with_attrs = fault_memory_read,
    .write_with_attrs = fault_memory_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * -2 uses global memory as a synthetic DMA observer, without an IOMMU.
 * -1 selects first_cpu; nonnegative indices select that CPU.
 */
static void transaction_request(Object *obj, const char *request, Error **errp)
{
    MemoryTransactionTest *s = MEMORY_TRANSACTION_TEST(obj);
    g_auto(GStrv) args = g_strsplit(request, " ", -1);
    uint8_t bytes[8];
    uint64_t address;
    unsigned size;
    int index;
    CPUState *cpu;
    bool dma;
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
        qemu_strtoi(args[1], NULL, 0, &index) || index < -2 ||
        qemu_strtou64(args[2], NULL, 0, &address)) {
        error_setg(errp, "Expected read ADDRESS_SPACE ADDRESS SIZE or "
                   "write ADDRESS_SPACE ADDRESS HEXBYTES");
        return;
    }
    dma = index == -2;
    cpu = dma ? NULL : index == -1 ? first_cpu : qemu_get_cpu(index);
    if (!dma && (!cpu || !cpu->as)) {
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
        result = dma ?
            dma_memory_write(&address_space_memory, address, bytes, size,
                             MEMTXATTRS_UNSPECIFIED) :
            address_space_write(cpu->as, address, MEMTXATTRS_UNSPECIFIED,
                                bytes, size);
    } else {
        result = dma ?
            dma_memory_read(&address_space_memory, address, bytes, size,
                            MEMTXATTRS_UNSPECIFIED) :
            address_space_read(cpu->as, address, MEMTXATTRS_UNSPECIFIED,
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
    MemoryTransactionTest *s = MEMORY_TRANSACTION_TEST(dev);

    if (!qtest_driver()) {
        error_setg(errp, "Memory transactions require a qtest server");
        return;
    }
    if (s->fault_memory &&
        s->fault_memory_base > UINT64_MAX - (UINT64_C(1) << 30)) {
        error_setg(errp, "Fault memory address range overflows");
        return;
    }
    if (s->migrate_ram) {
        if (!memory_region_is_ram(s->migrate_ram) ||
            memory_region_is_rom(s->migrate_ram)) {
            error_setg(errp, "migrate-ram requires writable test RAM");
            return;
        }
        if (qemu_ram_get_idstr(s->migrate_ram->ram_block)[0]) {
            error_setg(errp, "migrate-ram requires unregistered test RAM");
            return;
        }
        vmstate_register_ram(s->migrate_ram, dev);
        s->ram_registered = true;
    }
    if (s->fault_memory) {
        memory_region_init(&s->test_memory, OBJECT(s), "test-memory",
                           1ULL << 30);
        memory_region_init_io(&s->fault_memory_io, OBJECT(s), &fault_memory_ops,
                              s, "fault-memory-io", sizeof(s->bytes));
        memory_region_add_subregion(&s->test_memory, 0, &s->fault_memory_io);
        memory_region_add_subregion(get_system_memory(), s->fault_memory_base,
                                    &s->test_memory);
    }
}

static void transaction_unrealize(DeviceState *dev)
{
    MemoryTransactionTest *s = MEMORY_TRANSACTION_TEST(dev);

    if (s->ram_registered) {
        vmstate_unregister_ram(s->migrate_ram, dev);
        s->ram_registered = false;
    }
    if (s->fault_memory) {
        memory_region_del_subregion(get_system_memory(), &s->test_memory);
    }
    g_clear_pointer(&s->result, g_free);
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
static bool fault_memory_needed(void *opaque)
{
    return MEMORY_TRANSACTION_TEST(opaque)->fault_memory;
}

static const VMStateDescription fault_memory_vmstate = {
    .name = TYPE_MEMORY_TRANSACTION_TEST "/fault-memory",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = fault_memory_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(bytes, MemoryTransactionTest, 65536),
        VMSTATE_UINT64(fault_start, MemoryTransactionTest),
        VMSTATE_UINT64(fault_end, MemoryTransactionTest),
        VMSTATE_UINT32(fault_mode, MemoryTransactionTest),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription transaction_vmstate = {
    .name = TYPE_MEMORY_TRANSACTION_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const[]) {
        &fault_memory_vmstate,
        NULL
    },
};

static const Property transaction_properties[] = {
    DEFINE_PROP_LINK("migrate-ram", MemoryTransactionTest, migrate_ram,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_BOOL("fault-memory", MemoryTransactionTest,
                     fault_memory, false),
    DEFINE_PROP_UINT64("fault-memory-base", MemoryTransactionTest,
                       fault_memory_base, UINT64_C(0x100000000)),
};

static void transaction_init(Object *obj)
{
    MemoryTransactionTest *s = MEMORY_TRANSACTION_TEST(obj);

    object_property_add_uint64_ptr(obj, "fault-start", &s->fault_start,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "fault-end", &s->fault_end,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "fault-mode", &s->fault_mode,
                                   OBJ_PROP_FLAG_READWRITE);
}

static void transaction_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = transaction_realize;
    dc->unrealize = transaction_unrealize;
    dc->vmsd = &transaction_vmstate;
    device_class_set_props(dc, transaction_properties);
    object_class_property_add_str(oc, "request", NULL, transaction_request);
    object_class_property_add_str(oc, "result", transaction_result, NULL);
}

static const TypeInfo transaction_type = {
    .name = TYPE_MEMORY_TRANSACTION_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MemoryTransactionTest),
    .instance_init = transaction_init,
    .instance_finalize = transaction_finalize,
    .class_init = transaction_class_init,
};

static void transaction_register_types(void)
{
    type_register_static(&transaction_type);
}

type_init(transaction_register_types)
