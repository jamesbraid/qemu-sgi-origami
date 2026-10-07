/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-sn1-router.h"
#include "hw/core/resettable.h"
#include "io/channel-file.h"
#include "migration/qemu-file.h"
#include "migration/vmstate.h"
#include "monitor/hmp.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "system/address-spaces.h"

/* This test calls the register target directly and attaches no MMIO or UART. */
void memory_region_init_io(MemoryRegion *mr, Object *owner,
                           const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size)
{
}

uint64_t memory_region_size(const MemoryRegion *mr)
{
    g_assert_not_reached();
}

MemoryRegion *get_system_memory(void)
{
    g_assert_not_reached();
}

void memory_region_add_subregion(MemoryRegion *mr, hwaddr offset,
                                 MemoryRegion *subregion)
{
    g_assert_not_reached();
}

void memory_region_add_subregion_overlap(MemoryRegion *mr, hwaddr offset,
                                         MemoryRegion *subregion, int priority)
{
    g_assert_not_reached();
}

void memory_region_del_subregion(MemoryRegion *mr, MemoryRegion *subregion)
{
    g_assert_not_reached();
}

int monitor_hmp_printf(MonitorHMP *mon, const char *fmt, ...)
{
    g_assert_not_reached();
}

const char *qdev_fw_name(DeviceState *dev)
{
    g_assert_not_reached();
}

void address_space_init(AddressSpace *as, MemoryRegion *root,
                        const char *name)
{
    g_assert_not_reached();
}

void address_space_destroy(AddressSpace *as)
{
    g_assert_not_reached();
}

MemTxResult address_space_rw(const AddressSpace *as, hwaddr addr,
                             MemTxAttrs attrs, void *buf, hwaddr len,
                             bool is_write)
{
    g_assert_not_reached();
}

void memory_region_transaction_begin(void)
{
}

void memory_region_transaction_commit(void)
{
}

static DeviceState *new_router(bool synthetic)
{
    DeviceState *dev = qdev_new(TYPE_SGI_SN1_ROUTER);

    if (synthetic) {
        sgi_sn1_router_enable_synthetic_diagnostics(SGI_SN1_ROUTER(dev));
    }
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_abort);
    device_cold_reset(dev);
    return dev;
}

static void test_init_machine(void)
{
    Object *machine = object_property_add_new_container(object_get_root(),
                                                         "machine");

    object_property_add_new_container(machine, "unattached");
}

static QEMUFile *state_file(int fd, bool output)
{
    int copy = dup(fd);
    QIOChannel *ioc;
    QEMUFile *file;

    g_assert_cmpint(copy, >=, 0);
    g_assert_cmpint(lseek(copy, 0, SEEK_SET), ==, 0);
    ioc = QIO_CHANNEL(qio_channel_file_new_fd(copy));
    file = output ? qemu_file_new_output(ioc) : qemu_file_new_input(ioc);
    object_unref(OBJECT(ioc));
    return file;
}

static void scratch(SGISN1RouterState *router, bool write, uint64_t *value)
{
    SGINetPacket packet = {
        .op = write ? SGI_NET_PACKET_WRITE : SGI_NET_PACKET_READ,
        .addr = 0x100,
        .data = *value,
    };

    g_assert_cmpint(sgi_sn1_router_access(router, 1, &packet), ==,
                    SGI_NET_PACKET_OK);
    if (!write) {
        *value = packet.value;
    }
}

static void test_profile_mismatch(gconstpointer data)
{
    bool source_synthetic = GPOINTER_TO_INT(data);
    DeviceState *source = new_router(source_synthetic);
    DeviceState *target = new_router(!source_synthetic);
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(source)->vmsd;
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("sn1-router-profile-XXXXXX", &path, NULL);
    QEMUFile *file;
    Error *err = NULL;
    uint64_t marker = UINT64_C(0x123456789abcdef0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    scratch(SGI_SN1_ROUTER(target), true, &marker);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, vmsd, source, NULL,
                                      &error_abort), ==, 0);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, vmsd, target,
                                      vmsd->version_id, &err), !=, 0);
    g_assert_nonnull(err);
    error_free(err);
    qemu_fclose(file);
    marker = 0;
    scratch(SGI_SN1_ROUTER(target), false, &marker);
    g_assert_cmphex(marker, ==, UINT64_C(0x123456789abcdef0));
    close(fd);
    object_unref(OBJECT(source));
    object_unref(OBJECT(target));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    module_call_init(MODULE_INIT_QOM);
    test_init_machine();
    g_test_add_data_func("/sgi-sn1-router/migrate/synthetic-to-ordinary",
                         GINT_TO_POINTER(1), test_profile_mismatch);
    g_test_add_data_func("/sgi-sn1-router/migrate/ordinary-to-synthetic",
                         GINT_TO_POINTER(0), test_profile_mismatch);
    return g_test_run();
}
