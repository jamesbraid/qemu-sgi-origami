/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Controlled resolved START events, not a multi-master arbitration model. */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/sgi-i2c-wire.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "../migration/qemu-file.h"
#include "../migration/savevm.h"
#include "io/channel-file.h"

#define TYPE_TEST_HOST "i2c-masters-test-host"
#define TYPE_TEST_SLAVE "i2c-masters-test-slave"
#define TYPE_LEGACY_SLAVE "i2c-masters-legacy-slave"

typedef struct TestSlave {
    I2CSlave parent_obj;
    unsigned starts, restarts, stops, nacks, writes;
    uint8_t value;
    uint8_t match_address;
    bool reject_address, reject_data;
} TestSlave;

typedef struct Fixture {
    DeviceState *host;
    I2CBus *bus;
    TestSlave *a, *b;
} Fixture;

static int slave_event(I2CSlave *dev, enum i2c_event event)
{
    TestSlave *s = (TestSlave *)dev;

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        s->starts++;
        return s->reject_address;
    case I2C_RESTART:
        s->restarts++;
        break;
    case I2C_FINISH:
        s->stops++;
        break;
    case I2C_NACK:
        s->nacks++;
        break;
    default:
        g_assert_not_reached();
    }
    return 0;
}

static int slave_send(I2CSlave *dev, uint8_t data)
{
    TestSlave *s = (TestSlave *)dev;

    s->value = data;
    s->writes++;
    return s->reject_data;
}

static uint8_t slave_recv(I2CSlave *dev)
{
    return ((TestSlave *)dev)->value;
}

static bool slave_match(I2CSlave *dev, uint8_t address, bool broadcast,
                        I2CNodeList *matches)
{
    TestSlave *s = (TestSlave *)dev;
    I2CNode *node;

    if (!broadcast && address != s->match_address) {
        return false;
    }
    node = g_new(I2CNode, 1);
    node->elt = dev;
    QLIST_INSERT_HEAD(matches, node, next);
    return true;
}

static void slave_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    sc->match_and_add = slave_match;
    sc->event = slave_event;
    sc->send = slave_send;
    sc->recv = slave_recv;
}

static void legacy_slave_class_init(ObjectClass *oc, const void *data)
{
    I2C_SLAVE_CLASS(oc)->event = slave_event;
}

static void fixture_setup(Fixture *f, gconstpointer unused)
{
    f->host = qdev_new(TYPE_TEST_HOST);
    qdev_realize(f->host, NULL, &error_abort);
    f->bus = i2c_init_bus_type(TYPE_SGI_I2C_WIRE_BUS, f->host, "i2c");
    if (unused != GINT_TO_POINTER(2)) {
        g_assert_cmpint(i2c_bus_enable_master_ids(f->bus, 4), ==, 0);
    }
    f->a = (TestSlave *)i2c_slave_create_simple(f->bus, TYPE_TEST_SLAVE, 0x20);
    f->b = (TestSlave *)i2c_slave_create_simple(f->bus, TYPE_TEST_SLAVE, 0x21);
    f->a->match_address = 0x20;
    f->b->match_address = 0x21;
}

static void fixture_teardown(Fixture *f, gconstpointer unused)
{
    object_unparent(OBJECT(f->host));
    object_unref(OBJECT(f->host));
}

static void test_configuration(Fixture *f, gconstpointer unused)
{
    I2CPendingMaster pending = { 0 };

    g_assert_cmpint(i2c_bus_enable_master_ids(f->bus, 0), ==, -EINVAL);
    g_assert_cmpint(i2c_start_transfer(f->bus, 0x20, false), ==, 0);
    g_assert_cmpint(i2c_bus_enable_master_ids(f->bus, 4), ==, -EBUSY);
    i2c_end_transfer(f->bus);
    /* A queued legacy request is a control-plane fixture, not arbitration. */
    QSIMPLEQ_INSERT_TAIL(&f->bus->pending_masters, &pending, entry);
    g_assert_cmpint(i2c_bus_enable_master_ids(f->bus, 4), ==, -EBUSY);
    QSIMPLEQ_REMOVE_HEAD(&f->bus->pending_masters, entry);
    g_assert_cmpint(i2c_bus_enable_master_ids(f->bus, 4), ==, 0);
    g_assert_cmpint(i2c_bus_enable_master_ids(f->bus, 4), ==, -EBUSY);
}

static void test_target_nack(Fixture *f, gconstpointer unused)
{
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 0), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x20, false), ==, 0);
    f->a->reject_data = true;
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x42), ==, 1);
    g_assert_cmpuint(f->a->writes, ==, 1);
    g_assert_true(i2c_bus_busy(f->bus));
    f->b->reject_address = true;
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x21, false), ==, 1);
    g_assert_cmpuint(f->a->restarts, ==, 1);
    g_assert_cmpuint(f->a->stops, ==, 0);
    g_assert_cmpuint(f->b->starts, ==, 1);
    g_assert_cmpuint(f->b->stops, ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x43), ==, -ENXIO);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 0), ==, 0);
}

static void test_general_call(Fixture *f, gconstpointer reverse)
{
    TestSlave *reject = reverse ? f->b : f->a;

    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 0), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0, false), ==, 0);
    reject->reject_data = true;
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x42), ==, 0);
    g_assert_cmpuint(f->a->writes, ==, 1);
    g_assert_cmpuint(f->b->writes, ==, 1);
    f->a->reject_data = true;
    f->b->reject_data = true;
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x43), ==, 1);
    g_assert_cmpuint(f->a->writes, ==, 2);
    g_assert_cmpuint(f->b->writes, ==, 2);
    f->a->reject_data = false;
    f->b->reject_data = false;
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x44), ==, 0);
    g_assert_cmpuint(f->a->writes, ==, 3);
    g_assert_cmpuint(f->b->writes, ==, 3);
    reject->reject_address = true;
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0, false), ==, 0);
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x45), ==, 0);
    g_assert_cmpuint(reject->writes, ==, 3);
    g_assert_cmpuint((reverse ? f->a : f->b)->writes, ==, 4);
    f->a->reject_address = true;
    f->b->reject_address = true;
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0, false), ==, 1);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x46), ==, -ENXIO);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 0), ==, 0);
}

static void test_start_byte(Fixture *f, gconstpointer unused)
{
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 0), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x20, false), ==, 0);
    /* Address zero with R/W one is the unacknowledged START byte. */
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0, true), ==, 1);
    g_assert_cmpuint(f->a->restarts, ==, 1);
    g_assert_cmpuint(f->a->starts, ==, 1);
    g_assert_cmpuint(f->b->starts, ==, 0);
    g_assert_cmpuint(f->a->stops, ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 0), ==, 0);
}

static void test_nack_occupancy(Fixture *f, gconstpointer unused)
{
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 2), ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 2, 0x30, false), ==, 1);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_master_send(f->bus, 2, 0xaa), ==, -ENXIO);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 2), ==, 0);
    g_assert_false(i2c_bus_busy(f->bus));
}

static void test_owner(Fixture *f, gconstpointer unused)
{
    uint8_t byte = 0xa5;

    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 4), ==, -EINVAL);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 1), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 1, 0x20, false), ==, 0);
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 2), ==, -EBUSY);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 2, 0x21, false), ==,
                    -EPERM);
    g_assert_cmpint(i2c_master_send(f->bus, 2, 0x77), ==, -EPERM);
    g_assert_cmpint(i2c_master_recv(f->bus, 2, &byte), ==, -EPERM);
    g_assert_cmphex(byte, ==, 0xa5);
    g_assert_cmpint(i2c_master_nack(f->bus, 2), ==, -EPERM);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 2), ==, -EPERM);
    g_assert_cmpuint(f->a->stops, ==, 0);
    g_assert_cmpuint(f->b->starts, ==, 0);
    g_assert_cmpint(i2c_master_send(f->bus, 1, 0x42), ==, 0);
    g_assert_cmphex(f->a->value, ==, 0x42);
    g_assert_cmpuint(f->a->writes, ==, 1);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 1), ==, 0);
    g_assert_cmpuint(f->a->stops, ==, 1);
}

static void test_readdress(Fixture *f, gconstpointer unused)
{
    uint8_t byte;

    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 0), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x20, false), ==, 0);
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x35), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x20, true), ==, 0);
    g_assert_cmpint(i2c_master_recv(f->bus, 0, &byte), ==, 0);
    g_assert_cmphex(byte, ==, 0x35);
    g_assert_cmpint(i2c_master_nack(f->bus, 0), ==, 0);
    g_assert_cmpuint(f->a->nacks, ==, 1);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x21, false), ==, 0);
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x67), ==, 0);
    g_assert_cmphex(f->a->value, ==, 0x35);
    g_assert_cmphex(f->b->value, ==, 0x67);
    g_assert_cmpuint(f->a->restarts, ==, 2);
    g_assert_cmpuint(f->a->stops, ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x30, true), ==, 1);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpuint(f->b->restarts, ==, 1);
    g_assert_cmpuint(f->b->stops, ==, 0);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 0), ==, 0);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmpuint(f->b->stops, ==, 0);
}

static void test_unaudited_slave(Fixture *f, gconstpointer unused)
{
    TestSlave *legacy = (TestSlave *)i2c_slave_create_simple(
        f->bus, TYPE_LEGACY_SLAVE, 0x22);

    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 0), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x20, false), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 0, 0x22, false), ==,
                    -ENOTSUP);
    g_assert_cmpuint(legacy->starts, ==, 0);
    g_assert_cmpuint(f->a->restarts, ==, 0);
    g_assert_cmpint(i2c_master_send(f->bus, 0, 0x42), ==, 0);
    g_assert_cmphex(f->a->value, ==, 0x42);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 0), ==, 0);
}

static void test_legacy_unchanged(Fixture *f, gconstpointer unused)
{
    g_assert_cmpint(i2c_start_transfer(f->bus, 0x20, false), ==, 0);
    g_assert_cmpint(i2c_send(f->bus, 0x35), ==, 0);
    /* Preserve the legacy API's retained match, even with another address. */
    g_assert_cmpint(i2c_start_transfer(f->bus, 0x21, true), ==, 0);
    g_assert_cmphex(i2c_recv(f->bus), ==, 0x35);
    i2c_nack(f->bus);
    i2c_end_transfer(f->bus);
    g_assert_cmpuint(f->a->starts, ==, 2);
    g_assert_cmpuint(f->a->stops, ==, 1);
    g_assert_cmpuint(f->a->restarts, ==, 0);
    g_assert_cmpuint(f->b->starts, ==, 0);
    g_assert_false(i2c_bus_busy(f->bus));
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

static void test_migration_general_call(Fixture *f, gconstpointer unused)
{
    TestSlave *c = (TestSlave *)i2c_slave_create_simple(
        f->bus, TYPE_TEST_SLAVE, 0x22);
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-masters-XXXXXX", &path, NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    c->match_address = 0x22;
    c->reject_address = true;
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 3), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 3, 0, false), ==, 0);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 3), ==, 0);
    c->reject_address = false;
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 1), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 1, 0x22, false), ==, 0);
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                       1, &error_abort), ==, 0);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_true(f->bus->broadcast);
    g_assert_cmpint(i2c_master_send(f->bus, 3, 0x42), ==, 0);
    g_assert_cmpuint(f->a->writes, ==, 1);
    g_assert_cmpuint(f->b->writes, ==, 1);
    g_assert_cmpuint(c->writes, ==, 0);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 3), ==, 0);
    close(fd);
}

static void test_migration(Fixture *f, gconstpointer unmatched)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-masters-XXXXXX", &path, NULL);
    QEMUFile *file;
    uint8_t byte;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 3), ==, 0);
    f->a->value = 0x56;
    /* Dynamic matching, as on PCF8584: base addresses need not be unique. */
    f->a->parent_obj.address = 0;
    f->b->parent_obj.address = 0;
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 3,
                    unmatched ? 0x30 : 0x20, true), ==, unmatched ? 1 : 0);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 3), ==, 0);
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 1), ==, 0);
    g_assert_cmpint(i2c_master_start_transfer(f->bus, 1, 0x21, false), ==, 0);
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                       1, &error_abort), ==, 0);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 1), ==, -EPERM);
    g_assert_cmpuint(SGI_I2C_WIRE_BUS(f->bus)->requested_address, ==,
                     unmatched ? 0x30 : 0x20);
    g_assert_true(SGI_I2C_WIRE_BUS(f->bus)->master_recv);
    g_assert_cmpint(i2c_master_send(f->bus, 3, 0x11), ==, -EINVAL);
    g_assert_cmpint(i2c_master_recv(f->bus, 3, &byte), ==,
                    unmatched ? -ENXIO : 0);
    if (!unmatched) {
        g_assert_cmphex(byte, ==, 0x56);
    }
    g_assert_cmpint(i2c_master_end_transfer(f->bus, 3), ==, 0);
    close(fd);
}

int main(int argc, char **argv)
{
    const TypeInfo host = { .name = TYPE_TEST_HOST, .parent = TYPE_DEVICE };
    const TypeInfo slave = {
        .name = TYPE_TEST_SLAVE, .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(TestSlave), .class_init = slave_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_I2C_WIRE_ENDPOINT },
            { }
        },
    };
    const TypeInfo legacy = {
        .name = TYPE_LEGACY_SLAVE, .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(TestSlave),
        .class_init = legacy_slave_class_init,
    };
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    type_register_static(&host);
    type_register_static(&slave);
    type_register_static(&legacy);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    g_test_add("/i2c-masters/configuration", Fixture, GINT_TO_POINTER(2),
               fixture_setup, test_configuration, fixture_teardown);
    g_test_add("/i2c-masters/target-nack", Fixture, NULL,
               fixture_setup, test_target_nack, fixture_teardown);
    g_test_add("/i2c-masters/general-call", Fixture, NULL,
               fixture_setup, test_general_call, fixture_teardown);
    g_test_add("/i2c-masters/general-call-reversed", Fixture,
               GINT_TO_POINTER(1),
               fixture_setup, test_general_call, fixture_teardown);
    g_test_add("/i2c-masters/start-byte", Fixture, NULL,
               fixture_setup, test_start_byte, fixture_teardown);
    g_test_add("/i2c-masters/nack-occupancy", Fixture, NULL,
               fixture_setup, test_nack_occupancy, fixture_teardown);
    g_test_add("/i2c-masters/owner", Fixture, NULL,
               fixture_setup, test_owner, fixture_teardown);
    g_test_add("/i2c-masters/readdress", Fixture, NULL,
               fixture_setup, test_readdress, fixture_teardown);
    g_test_add("/i2c-masters/unaudited-slave", Fixture, NULL,
               fixture_setup, test_unaudited_slave, fixture_teardown);
    g_test_add("/i2c-masters/legacy-unchanged", Fixture, GINT_TO_POINTER(2),
               fixture_setup, test_legacy_unchanged, fixture_teardown);
    g_test_add("/i2c-masters/migration-general-call", Fixture, NULL,
               fixture_setup, test_migration_general_call, fixture_teardown);
    g_test_add("/i2c-masters/migration-selected", Fixture, NULL,
               fixture_setup, test_migration, fixture_teardown);
    g_test_add("/i2c-masters/migration-unmatched", Fixture, GINT_TO_POINTER(1),
               fixture_setup, test_migration, fixture_teardown);
    return g_test_run();
}
