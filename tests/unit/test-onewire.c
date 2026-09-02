#include "qemu/osdep.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/onewire.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"

/* The identity CRC-8 spans exactly seven bytes: family plus six serial. */
static void test_crc8_rom_identity(void)
{
    /* Family 0x0b, serial 01 02 03 04 05 06. */
    const uint8_t id[7] = { 0x0b, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    uint8_t crc = onewire_crc8(0, id, sizeof(id));

    /* A full identity including its CRC checks to zero. */
    uint8_t full[8];
    memcpy(full, id, sizeof(id));
    full[7] = crc;
    g_assert_cmpuint(onewire_crc8(0, full, sizeof(full)), ==, 0);
}

static void test_crc8_is_not_inverted(void)
{
    const uint8_t zero[1] = { 0x00 };
    g_assert_cmpuint(onewire_crc8(0, zero, 1), ==, 0x00);
}

static void test_crc16_accumulates(void)
{
    const uint8_t a[2] = { 0xf0, 0x00 };
    const uint8_t b[1] = { 0x00 };
    uint16_t split = onewire_crc16(onewire_crc16(0, a, 2), b, 1);
    const uint8_t all[3] = { 0xf0, 0x00, 0x00 };
    uint16_t whole = onewire_crc16(0, all, 3);
    g_assert_cmpuint(split, ==, whole);
    /* This is the UN-inverted value: our crc16 deliberately does not invert,
     * and callers invert when transmitting. The test asserts what the
     * function returns, not what goes on the wire.
     */
    g_assert_cmpuint(whole, ==, 0x3300);
}

/* CRC-8 against externally verified catalogue value (CRC-8/MAXIM-DOW). */
static void test_crc8_catalogue(void)
{
    const uint8_t msg[9] = "123456789";
    g_assert_cmpuint(onewire_crc8(0, msg, sizeof(msg)), ==, 0xa1);
}

/* CRC-16 against externally verified catalogue value (CRC-16/ARC). */
static void test_crc16_catalogue(void)
{
    const uint8_t msg[9] = "123456789";
    g_assert_cmpuint(onewire_crc16(0, msg, sizeof(msg)), ==, 0xbb3d);
}

/* CRC-8 running value: exercise the seed argument. IOC3 datasheet records
 * that IRIX sends {0xF0,0x00,0x00} and requires 0x8D as the next byte.
 */
static void test_crc8_ioc3_requirement(void)
{
    const uint8_t ioc3[3] = { 0xf0, 0x00, 0x00 };
    g_assert_cmpuint(onewire_crc8(0, ioc3, sizeof(ioc3)), ==, 0x8d);
}

/* CRC-8 accumulation: split call gives same result as single call. */
static void test_crc8_accumulates(void)
{
    const uint8_t a[1] = { 0x12 };
    const uint8_t b[1] = { 0x34 };
    uint8_t split = onewire_crc8(onewire_crc8(0, a, 1), b, 1);
    const uint8_t all[2] = { 0x12, 0x34 };
    uint8_t whole = onewire_crc8(0, all, 2);
    g_assert_cmpuint(split, ==, whole);
    g_assert_cmpuint(split, ==, 0xa2);
}

/* Drive a whole byte, LSB first, and return what the bus sampled. */
static uint8_t bus_touch_byte(OneWireBus *bus, uint8_t out)
{
    uint8_t in = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (onewire_touch_bit(bus, (out >> i) & 1)) {
            in |= 1u << i;
        }
    }
    return in;
}

/*
 * The shared ROM layer has no machine yet, so every test builds its own
 * throwaway device to hang a 1-Wire bus off. onewire_create_bus() requires
 * a real parent device -- qbus_new() asserts that a parentless bus can
 * only ever be the implicit main system bus (hw/core/bus.c) -- so this
 * anchors the test bus under a bare, unattached device instead. (The
 * plan's original sketch called onewire_create_bus(NULL, "test") directly;
 * James corrected the plan to this form once the assert was found -- see
 * the task report.)
 *
 * An anchor with no bus of its own realizes with bus=NULL, which parents
 * it under "/machine/unattached" (hw/core/qdev.c: device_set_realized()).
 * That container only exists once something has created it, matching
 * tests/unit/test-qdev.c's test_init_machine().
 */
static void onewire_test_init_machine(void)
{
    Object *machine = object_property_add_new_container(
        object_get_root(), "machine");

    object_property_add_new_container(machine, "unattached");
}

#define TYPE_ONEWIRE_TEST_HOST "onewire-test-host"

static const TypeInfo onewire_test_host_info = {
    .name = TYPE_ONEWIRE_TEST_HOST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DeviceState),
};

static OneWireBus *onewire_test_bus(void)
{
    DeviceState *host = qdev_new(TYPE_ONEWIRE_TEST_HOST);

    qdev_realize_and_unref(host, NULL, &error_fatal);
    return onewire_create_bus(host, "test");
}

/* A minimal slave with no memory function: it exercises the ROM layer alone. */
#define TYPE_ONEWIRE_TEST_STUB "onewire-test-stub"

static void onewire_test_stub_realize(DeviceState *dev, Error **errp)
{
    OneWireSlave *s = ONEWIRE_SLAVE(dev);

    onewire_slave_set_identity(s, s->family, s->serial);
}

static void onewire_test_stub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = onewire_test_stub_realize;
}

static const TypeInfo onewire_test_stub_info = {
    .name = TYPE_ONEWIRE_TEST_STUB,
    .parent = TYPE_ONEWIRE_SLAVE,
    .instance_size = sizeof(OneWireSlave),
    .class_init = onewire_test_stub_class_init,
};

static OneWireBus *onewire_test_bus_with_part(uint8_t family, uint64_t serial)
{
    OneWireBus *bus = onewire_test_bus();
    DeviceState *dev = qdev_new(TYPE_ONEWIRE_TEST_STUB);

    qdev_prop_set_uint8(dev, "family", family);
    qdev_prop_set_uint64(dev, "serial", serial);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
    return bus;
}

static void test_empty_bus_reads_ones(void)
{
    OneWireBus *bus = onewire_test_bus();

    /* No part, so no presence and every slot samples high. */
    g_assert_false(onewire_reset(bus));
    for (unsigned i = 0; i < 16; i++) {
        g_assert_true(onewire_touch_bit(bus, true));
    }
}

static void test_read_rom_returns_the_identity(void)
{
    OneWireBus *bus = onewire_test_bus_with_part(0x0b, 0x010203040506ULL);
    uint8_t id[8];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0x33);                  /* Read ROM */
    for (unsigned i = 0; i < 8; i++) {
        id[i] = bus_touch_byte(bus, 0xff);      /* read = drive one */
    }

    g_assert_cmpuint(id[0], ==, 0x0b);
    g_assert_cmpuint(onewire_crc8(0, id, sizeof(id)), ==, 0);
}

static void test_match_rom_selects_only_on_an_exact_identity(void)
{
    OneWireBus *bus = onewire_test_bus_with_part(0x0b, 0x010203040506ULL);
    const uint8_t wrong[8] = { 0x0b, 0x99, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0x55);
    for (unsigned i = 0; i < 8; i++) {
        bus_touch_byte(bus, wrong[i]);
    }
    /* Deselected, so the part drives nothing and the wire floats high. */
    g_assert_true(onewire_touch_bit(bus, true));
}

static void test_a_driven_zero_differs_from_a_driven_one(void)
{
    OneWireBus *bus = onewire_test_bus_with_part(0x0b, 0x010203040506ULL);

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0x33);
    /*
     * The first identity byte is 0x0b = 00001011b, LSB first: 1,1,0,1,...
     * so slot three must sample low. A model with no bus at all would
     * sample high on every slot and pass a completion-only assertion.
     */
    g_assert_true(onewire_touch_bit(bus, true));
    g_assert_true(onewire_touch_bit(bus, true));
    g_assert_false(onewire_touch_bit(bus, true));
}

/*
 * A slave whose memory function always answers a fixed byte, regardless of
 * what it is asked. It exists only so a test can tell whether
 * ONEWIRE_FUNCTION was ever reached at all: an unselected or IDLE part
 * never calls function_byte, so the bus floats back whatever the caller
 * drove instead of this constant.
 */
#define TYPE_ONEWIRE_TEST_REPLY "onewire-test-reply"

static bool onewire_test_reply_function_byte(OneWireSlave *s, uint8_t in,
                                              uint8_t *out)
{
    *out = 0x3c;
    return true;
}

static void onewire_test_reply_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    OneWireSlaveClass *k = ONEWIRE_SLAVE_CLASS(oc);

    dc->realize = onewire_test_stub_realize;
    k->function_byte = onewire_test_reply_function_byte;
}

static const TypeInfo onewire_test_reply_info = {
    .name = TYPE_ONEWIRE_TEST_REPLY,
    .parent = TYPE_ONEWIRE_SLAVE,
    .instance_size = sizeof(OneWireSlave),
    .class_init = onewire_test_reply_class_init,
};

static OneWireBus *onewire_test_bus_with_reply_part(uint8_t family,
                                                     uint64_t serial)
{
    OneWireBus *bus = onewire_test_bus();
    DeviceState *dev = qdev_new(TYPE_ONEWIRE_TEST_REPLY);

    qdev_prop_set_uint8(dev, "family", family);
    qdev_prop_set_uint64(dev, "serial", serial);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
    return bus;
}

/*
 * Gap 1 of the extraction (DS2502/DS2505 ROM-function flow chart, Figure 9
 * / Figure 8): all four ROM-command success paths -- Read ROM, Match ROM, a
 * completed Search ROM, and Skip ROM -- converge on ONEWIRE_FUNCTION with
 * no second reset or addressing command. This is the one of the four that
 * a naive phase machine is most likely to get wrong, since Search ROM's
 * bit-triplet loop is the most complicated of the four to terminate
 * correctly, so it gets its own test rather than relying on inference from
 * the other three.
 */
static void test_search_rom_completes_and_reaches_function(void)
{
    OneWireBus *bus = onewire_test_bus_with_reply_part(0x0b, 0x010203040506ULL);
    const uint8_t body[7] = { 0x0b, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
    uint8_t id[8];
    uint8_t reply;

    memcpy(id, body, sizeof(body));
    id[7] = onewire_crc8(0, body, sizeof(body));

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xf0);                  /* Search ROM */

    /*
     * Only one part on the bus: drive its own identity bit at every
     * triplet's direction slot, so it survives the whole search.
     */
    for (unsigned bitn = 0; bitn < 64; bitn++) {
        bool bitval = (id[bitn / 8] >> (bitn % 8)) & 1;

        onewire_touch_bit(bus, true);            /* identify slot */
        onewire_touch_bit(bus, true);            /* complement slot */
        onewire_touch_bit(bus, bitval);          /* direction slot */
    }

    bus_touch_byte(bus, 0xaa);                   /* prime the constant reply */
    reply = bus_touch_byte(bus, 0xff);           /* read it back */

    /*
     * A model that instead dropped a surviving part to ONEWIRE_IDLE after
     * Search ROM would never call function_byte, and this read would float
     * the driven 0xff back unchanged instead of the part's constant 0x3c.
     */
    g_assert_cmpuint(reply, ==, 0x3c);
}

/*
 * Gap 2 of the extraction: Match ROM drops a part at the first identity bit
 * that fails to compare, rather than clocking all 64 bits and deciding at
 * the end. The two designs are indistinguishable from outside once the
 * transaction finishes -- test_match_rom_selects_only_on_an_exact_identity
 * already covers that -- so this checks the part's own phase mid
 * transaction, which is where the two designs actually differ.
 */
static void test_match_rom_drops_at_first_mismatch(void)
{
    OneWireBus *bus = onewire_test_bus();
    DeviceState *dev = qdev_new(TYPE_ONEWIRE_TEST_STUB);
    OneWireSlave *s = ONEWIRE_SLAVE(dev);

    qdev_prop_set_uint8(dev, "family", 0x0b);
    qdev_prop_set_uint64(dev, "serial", 0x010203040506ULL);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0x55);                   /* Match ROM */
    /*
     * Family byte 0x0b is 00001011b, LSB first: bit 0 is 1. Driving 0
     * mismatches on the very first identity bit.
     */
    onewire_touch_bit(bus, false);

    /*
     * A model that instead clocked all 64 bits before deciding would still
     * be sitting in ONEWIRE_ROM_MATCH here, one bit into the comparison.
     */
    g_assert_cmpint(s->phase, ==, ONEWIRE_IDLE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    module_call_init(MODULE_INIT_QOM);
    type_register_static(&onewire_test_host_info);
    type_register_static(&onewire_test_stub_info);
    type_register_static(&onewire_test_reply_info);
    onewire_test_init_machine();

    g_test_add_func("/onewire/crc8/rom-identity", test_crc8_rom_identity);
    g_test_add_func("/onewire/crc8/not-inverted", test_crc8_is_not_inverted);
    g_test_add_func("/onewire/crc8/catalogue", test_crc8_catalogue);
    g_test_add_func("/onewire/crc8/ioc3-requirement",
                     test_crc8_ioc3_requirement);
    g_test_add_func("/onewire/crc8/accumulates", test_crc8_accumulates);
    g_test_add_func("/onewire/crc16/accumulates", test_crc16_accumulates);
    g_test_add_func("/onewire/crc16/catalogue", test_crc16_catalogue);

    g_test_add_func("/onewire/bus/empty-bus-reads-ones",
                     test_empty_bus_reads_ones);
    g_test_add_func("/onewire/bus/read-rom-returns-identity",
                     test_read_rom_returns_the_identity);
    g_test_add_func("/onewire/bus/match-rom-selects-on-exact-identity",
                     test_match_rom_selects_only_on_an_exact_identity);
    g_test_add_func("/onewire/bus/driven-zero-differs-from-driven-one",
                     test_a_driven_zero_differs_from_a_driven_one);
    g_test_add_func("/onewire/bus/search-rom-reaches-function",
                     test_search_rom_completes_and_reaches_function);
    g_test_add_func("/onewire/bus/match-rom-drops-at-first-mismatch",
                     test_match_rom_drops_at_first_mismatch);

    return g_test_run();
}
