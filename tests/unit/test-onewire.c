#include "qemu/osdep.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/onewire.h"
#include "hw/1wire/ds2502.h"
#include "hw/1wire/ds2505.h"
#include "hw/misc/sgi-onewire-slot.h"
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

/* CRC-8 running value: exercise the seed argument. IRIX sends
 * {0xF0,0x00,0x00} and requires 0x8D as the next byte.
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
 * Each test attaches its 1-Wire bus to an unattached device:
 * onewire_create_bus() needs a parent, and qbus_new() reserves a parentless
 * bus for the implicit system bus (hw/core/bus.c). An anchor realized with
 * bus=NULL enters /machine/unattached (hw/core/qdev.c:
 * device_set_realized()); create that container first, as in
 * tests/unit/test-qdev.c.
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

/*
 * This slave tests only the ROM layer. The abstract onewire-slave realize
 * derives its identity from family and serial properties; this type needs no
 * realize callback.
 */
#define TYPE_ONEWIRE_TEST_STUB "onewire-test-stub"

static const TypeInfo onewire_test_stub_info = {
    .name = TYPE_ONEWIRE_TEST_STUB,
    .parent = TYPE_ONEWIRE_SLAVE,
    .instance_size = sizeof(OneWireSlave),
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

/*
 * The memory function returns its family byte for every request. An
 * unselected or IDLE part never calls function_byte, so the bus returns the
 * driven byte; two slaves identify which part answered.
 */
#define TYPE_ONEWIRE_TEST_REPLY "onewire-test-reply"

static bool onewire_test_reply_function_byte(OneWireSlave *s, uint8_t in,
                                              uint8_t *out)
{
    *out = s->family;
    return true;
}

static void onewire_test_reply_class_init(ObjectClass *oc, const void *data)
{
    OneWireSlaveClass *k = ONEWIRE_SLAVE_CLASS(oc);

    k->function_byte = onewire_test_reply_function_byte;
}

static const TypeInfo onewire_test_reply_info = {
    .name = TYPE_ONEWIRE_TEST_REPLY,
    .parent = TYPE_ONEWIRE_SLAVE,
    .instance_size = sizeof(OneWireSlave),
    .class_init = onewire_test_reply_class_init,
};

static void onewire_test_add_reply_part(OneWireBus *bus, uint8_t family,
                                         uint64_t serial)
{
    DeviceState *dev = qdev_new(TYPE_ONEWIRE_TEST_REPLY);

    qdev_prop_set_uint8(dev, "family", family);
    qdev_prop_set_uint64(dev, "serial", serial);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
}

static OneWireBus *onewire_test_bus_with_reply_part(uint8_t family,
                                                     uint64_t serial)
{
    OneWireBus *bus = onewire_test_bus();

    onewire_test_add_reply_part(bus, family, serial);
    return bus;
}

/*
 * Add a DS2501 or DS2502, loading an optional EPROM image through
 * ds2502_set_image() before realize, as a machine model must.
 */
static DeviceState *onewire_test_add_ds2502_variant(OneWireBus *bus,
                                                      const char *type_name,
                                                      uint8_t family,
                                                      uint64_t serial,
                                                      const uint8_t *image,
                                                      size_t image_len)
{
    DeviceState *dev = qdev_new(type_name);

    qdev_prop_set_uint8(dev, "family", family);
    qdev_prop_set_uint64(dev, "serial", serial);
    if (image) {
        ds2502_set_image(dev, image, image_len);
    }
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
    return dev;
}

/* A blank (unprogrammed) DS2501, the 512-bit sibling the IOC3 carries. */
static OneWireBus *onewire_test_bus_with_ds2501(uint8_t family,
                                                 uint64_t serial)
{
    OneWireBus *bus = onewire_test_bus();

    onewire_test_add_ds2502_variant(bus, TYPE_ONEWIRE_DS2501, family, serial,
                                     NULL, 0);
    return bus;
}

/*
 * Add a DS2505, loading an optional EPROM image through ds2505_set_image()
 * before realize, as for DS2502.
 */
static DeviceState *onewire_test_add_ds2505(OneWireBus *bus, uint8_t family,
                                             uint64_t serial,
                                             const uint8_t *image,
                                             size_t image_len)
{
    DeviceState *dev = qdev_new(TYPE_ONEWIRE_DS2505);

    qdev_prop_set_uint8(dev, "family", family);
    qdev_prop_set_uint64(dev, "serial", serial);
    if (image) {
        ds2505_set_image(dev, image, image_len);
    }
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
    return dev;
}

/*
 * Fill each of 64 DS2505 pages with its index (0x00..0x3f). Distinct nonblank
 * pages show whether Extended Read Memory returns the physical page rather
 * than following redirection.
 */
static OneWireBus *onewire_test_bus_with_ds2505_paged(uint8_t family,
                                                       uint64_t serial)
{
    OneWireBus *bus = onewire_test_bus();
    uint8_t image[DS2505_DATA_SIZE];

    for (size_t i = 0; i < sizeof(image); i++) {
        image[i] = (i / DS2505_PAGE_SIZE) & 0xff;
    }
    onewire_test_add_ds2505(bus, family, serial, image, sizeof(image));
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
    OneWireBus *bus = onewire_test_bus_with_reply_part(0x0b, 0x010203040506ULL);
    const uint8_t wrong[8] = { 0x0b, 0x99, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };
    uint8_t reply;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0x55);
    for (unsigned i = 0; i < 8; i++) {
        bus_touch_byte(bus, wrong[i]);
    }

    /*
     * Deselected, so a function byte now must float rather than answer.
     * A part freshly reached FUNCTION also has out == 0xff before its
     * first function_byte call, so a bare "does it pull" check right after
     * Match ROM cannot tell selected-but-silent-so-far apart from
     * deselected, and a model that ignored the mismatch and selected
     * anyway would pass such a check too. Priming and reading back a real
     * function byte is the only way to tell them apart.
     */
    bus_touch_byte(bus, 0xaa);
    reply = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(reply, ==, 0xff);
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
 * The DS2502/DS2505 ROM-function flow charts (Figures 9 and 8) send Read ROM,
 * Match ROM, completed Search ROM and Skip ROM directly to ONEWIRE_FUNCTION
 * without another reset or address. Search ROM gets a separate test because
 * its bit-triplet termination is easiest to get wrong.
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
     * Only one part on the bus: it must transmit its own identity bit on
     * the identify slot and the bit's inverse on the complement slot, in
     * that order. A model with the two slots swapped would transmit the
     * identity inverted and still finish the search, since nothing here
     * checked what was actually said until now. Driving the identity bit
     * back on the direction slot keeps the part alive for the whole
     * search; checking it too, rather than discarding it, is what catches
     * a model that dropped the open-drain "master AND no pull" term from
     * the direction slot's result (the part itself never pulls there, so
     * an implementation that instead always reported "no pull" as a bare
     * high would echo every driven 0 back as a 1).
     */
    for (unsigned bitn = 0; bitn < 64; bitn++) {
        bool bitval = (id[bitn / 8] >> (bitn % 8)) & 1;

        g_assert_cmpint(onewire_touch_bit(bus, true), ==, bitval);
        g_assert_cmpint(onewire_touch_bit(bus, true), ==, !bitval);
        g_assert_cmpint(onewire_touch_bit(bus, bitval), ==, bitval);
    }

    bus_touch_byte(bus, 0xaa);                   /* prime the reply */
    reply = bus_touch_byte(bus, 0xff);           /* read it back */

    /*
     * A model that instead dropped a surviving part to ONEWIRE_IDLE after
     * Search ROM would never call function_byte, and this read would float
     * the driven 0xff back unchanged instead of the part's own family byte.
     */
    g_assert_cmpuint(reply, ==, 0x0b);
}

/*
 * Match ROM drops a part on the first mismatched identity bit, before all 64
 * bits arrive. Final selection cannot distinguish delayed rejection; inspect
 * the part phase mid-transaction.
 * test_match_rom_selects_only_on_an_exact_identity covers the final result.
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

/*
 * Search ROM alone exposes multi-part wired-AND arbitration; other ROM
 * commands are master-driven. These parts differ at family bit 0: both are
 * active and drive opposite values, yielding 0/0 in the identify and
 * complement slots rather than a single part's complementary pair. Poll
 * every slave even after one pulls low so migratable parts advance every
 * bit; preserve the open-drain master AND every-slave result. Either error
 * loses the 0/0 result. Steering toward family 0x0b drops the other part;
 * the next 63 bits must show single-part values despite differing CRC bytes.
 * Further 0/0 results would show the dropped part still driving.
 */
static void test_search_rom_arbitrates_two_parts(void)
{
    OneWireBus *bus = onewire_test_bus_with_reply_part(0x0b, 0x010203040506ULL);
    const uint8_t bodyA[7] = { 0x0b, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
    const uint8_t bodyB[7] = { 0x0a, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
    uint8_t idA[8], idB[8];
    bool b_alive = true;
    uint8_t reply;

    onewire_test_add_reply_part(bus, 0x0a, 0x010203040506ULL);

    memcpy(idA, bodyA, sizeof(bodyA));
    idA[7] = onewire_crc8(0, bodyA, sizeof(bodyA));
    memcpy(idB, bodyB, sizeof(bodyB));
    idB[7] = onewire_crc8(0, bodyB, sizeof(bodyB));
    /*
     * Confirms the CRC bytes really do differ too, so the second half of
     * this search exercises silence at a position neither part was
     * explicitly steered on, not just the one forced discrepancy.
     */
    g_assert_cmpuint(idA[7], !=, idB[7]);

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xf0);                   /* Search ROM */

    for (unsigned bitn = 0; bitn < 64; bitn++) {
        bool bitA = (idA[bitn / 8] >> (bitn % 8)) & 1;
        bool bitB = (idB[bitn / 8] >> (bitn % 8)) & 1;

        if (b_alive && bitA != bitB) {
            g_assert_false(onewire_touch_bit(bus, true));   /* identify */
            g_assert_false(onewire_touch_bit(bus, true));   /* complement */
            onewire_touch_bit(bus, bitA);                   /* steer to A */
            b_alive = false;
        } else {
            g_assert_cmpint(onewire_touch_bit(bus, true), ==, bitA);
            g_assert_cmpint(onewire_touch_bit(bus, true), ==, !bitA);
            g_assert_cmpint(onewire_touch_bit(bus, bitA), ==, bitA);
        }
    }
    g_assert_false(b_alive);

    bus_touch_byte(bus, 0xaa);
    reply = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(reply, ==, 0x0b);
}

/*
 * DS2502-family.
 * The IOC3's part is a DS2501, family 0x91, two programmable pages,
 * sharing the DS2502's 128-byte 0000h-007Fh address space and command set.
 */

static void test_ds2502_plain_read_has_no_page_crc(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    uint8_t page[32];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);                 /* Skip ROM */
    bus_touch_byte(bus, 0xf0);                 /* Read Memory */
    bus_touch_byte(bus, 0x00);
    bus_touch_byte(bus, 0x00);

    /* Leading CRC-8 over command and both address bytes. */
    const uint8_t hdr[3] = { 0xf0, 0x00, 0x00 };
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, onewire_crc8(0, hdr, 3));

    for (unsigned i = 0; i < sizeof(page); i++) {
        page[i] = bus_touch_byte(bus, 0xff);
    }
    /*
     * Page zero is not the end of memory either way, so no CRC follows here.
     * The next byte is page one's first byte, unprogrammed. This is the
     * assertion that fails on a model that copies 0xc3's per-page framing
     * onto 0xf0. It deliberately does not read as far as end-of-memory,
     * because where that lies on a DS2501 is still open.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

static void test_ds2502_status_serves_the_real_field(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    const uint8_t hdr[3] = { 0xaa, 0x00, 0x00 };
    uint8_t status[8];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xaa);                 /* Read Status */
    bus_touch_byte(bus, 0x00);                 /* TA1: start at 0000h */
    bus_touch_byte(bus, 0x00);                 /* TA2 */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, onewire_crc8(0, hdr, 3));

    for (unsigned i = 0; i < sizeof(status); i++) {
        status[i] = bus_touch_byte(bus, 0xff);
    }

    /*
     * 0x0001 is page zero's redirection byte, and 0xff is its unprogrammed
     * value meaning "not redirected", the byte OpenBSD's redirection
     * walk stops on, and it must fall out of blank status memory rather
     * than a coded special case. 0x0007 is the one byte that actually
     * distinguishes this field: factory-programmed 00h, so a genuine
     * DS2502-class status readout is never all-0xff even on a blank part.
     * A regression in DS2502_STATUS_FACTORY_ADDR or in
     * ds2502_status_byte()'s equality check would only show up by reading
     * this far. The closing CRC-8 below is checked for the same reason.
     */
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmpuint(status[i], ==, 0xff);
    }
    g_assert_cmpuint(status[7], ==, 0x00);

    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, status, sizeof(status)));
}

/*
 * Unlike F0h, C3h ends each 32-byte page with a device CRC-8 over that page.
 * Copying F0h framing would return 0xff for byte 33, as
 * test_ds2502_plain_read_has_no_page_crc expects only for F0h.
 */
static void test_ds2502_read_data_crc_emits_page_crc_every_32_bytes(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    uint8_t page[32];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xc3);       /* Read Data / Generate 8-bit CRC */
    bus_touch_byte(bus, 0x00);
    bus_touch_byte(bus, 0x00);

    const uint8_t hdr[3] = { 0xc3, 0x00, 0x00 };
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, onewire_crc8(0, hdr, 3));

    for (unsigned i = 0; i < sizeof(page); i++) {
        page[i] = bus_touch_byte(bus, 0xff);
    }

    /* Byte 33 is a real CRC-8 over the 32 page bytes just read. */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, page, sizeof(page)));

    /*
     * The 33-byte cycle repeats: this is page one's first data byte, not
     * a second CRC.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

/*
 * C3h's first page can start mid-page, not just page-aligned: the CRC
 * then covers only the bytes actually read in that partial page, from
 * the start address to the page's end, not a fixed 32 bytes. The
 * page-aligned test above only ever exercises
 * `page_end = addr | (DS2502_PAGE_SIZE - 1)` in its trivial case, where
 * the OR happens to be a no-op; this is the case that actually forces
 * that rounding to do something.
 */
static void test_ds2502_read_data_crc_first_page_can_start_mid_page(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    uint8_t partial[16];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xc3);
    bus_touch_byte(bus, 0x10);       /* TA1: 0010h, 16 bytes into page 0 */
    bus_touch_byte(bus, 0x00);       /* TA2 */

    const uint8_t hdr[3] = { 0xc3, 0x10, 0x00 };
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, onewire_crc8(0, hdr, 3));

    for (unsigned i = 0; i < sizeof(partial); i++) {
        partial[i] = bus_touch_byte(bus, 0xff);
    }

    /*
     * Page 0 ends at 001Fh, 16 bytes from the 0010h start, not 32. A
     * model that only ever waited for a full 32-byte count instead of
     * computing the page boundary from the actual address would answer a
     * plain data byte here instead of this CRC.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, partial, sizeof(partial)));

    /*
     * Page one starts fresh, page-aligned, at 0020h with real data,
     * not a second CRC.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

/*
 * Read Memory's one CRC-8 only appears if the read actually runs through
 * the last byte of memory (007Fh), unlike C3h, which never stops.
 */
static void test_ds2502_read_memory_emits_one_crc_at_end_of_memory(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    uint8_t tail[2];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xf0);
    bus_touch_byte(bus, 0x7e);       /* TA1: two bytes from the end */
    bus_touch_byte(bus, 0x00);       /* TA2 */

    const uint8_t hdr[3] = { 0xf0, 0x7e, 0x00 };
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, onewire_crc8(0, hdr, 3));

    tail[0] = bus_touch_byte(bus, 0xff);    /* 007Eh */
    tail[1] = bus_touch_byte(bus, 0xff);    /* last memory byte: 007Fh */

    /*
     * Only now, having read through 007Fh, does the DS2501 answer a
     * closing CRC-8 over the data actually read. A model that added this
     * CRC unconditionally would already fail
     * test_ds2502_plain_read_has_no_page_crc; one that never adds it
     * would fail here instead.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, tail, sizeof(tail)));

    /*
     * Nothing further is defined past the closing CRC; the part floats
     * high rather than restarting or repeating it.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

/*
 * "A start address above 007Fh has its 9 most significant bits forced to
 * zero" (DS2502 datasheet). TA1=TA2=
 * 0xff sends a raw 16-bit start address of 0xffff; masked to 7 bits that
 * lands on 007Fh, the single last byte of memory. A model using the raw,
 * unmasked address would never satisfy the addr==007Fh check that
 * produces the closing CRC below, and so would answer 0xff forever
 * instead. This differs from getting the mask width wrong (which would land
 * somewhere else in 0000h-007Fh instead of at the
 * very end, and also never trip this same check).
 */
static void test_ds2502_start_address_above_top_is_masked(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    const uint8_t masked_hdr[3] = { 0xf0, 0x7f, 0x00 };
    const uint8_t sent_hdr[3] = { 0xf0, 0xff, 0xff };
    uint8_t echo;
    uint8_t last_byte;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xf0);

    /*
     * Every bit of TA1/TA2 here is a driven write-one, so this is exactly
     * the case that exposes a part wrongly pulling the line low during a
     * master-write window (OneWireSlaveClass::function_byte's "*out must
     * stay 0xff on a window the master drives" rule): a stuck-low device
     * would sample back something other than 0xff on either of these two
     * touches.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);    /* TA1 */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);    /* TA2 */

    /*
     * The echoed header CRC-8 covers the address AS MASKED. Both halves
     * are asserted, because only the pair is the datasheet's "deliberate
     * checksum mismatch": the echo matches a check over 007Fh, and it
     * does NOT match one over the FFFFh the master sent, which is how the
     * master learns it asked out of range rather than being handed 007Fh
     * as though it were the address it named.
     */
    echo = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(echo, ==, onewire_crc8(0, masked_hdr, 3));
    g_assert_cmpuint(echo, !=, onewire_crc8(0, sent_hdr, 3));

    last_byte = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(last_byte, ==, 0xff);      /* blank EPROM at 007Fh */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, &last_byte, 1));
}

/*
 * The masking above belongs to the memory commands only. The DS2502 family's
 * eight-byte status field has no documented mask of its own (the datasheet
 * states the 007Fh forcing for memory, and nothing for status), so AAh keeps
 * the address as sent, unlike the DS2505, whose status field the datasheet
 * does mask, and which has its own test for that. The difference only shows
 * above 007Fh, so this asks for 0087h: it masks onto 0007h, the one status
 * address whose byte is factory-programmed 00h and the one that closes the
 * field with a CRC-8. A part that masked the status address would therefore
 * answer a different stream: an echo over 0007h, then 00h, then
 * a closing CRC. The real part reads unprogrammed EPROM at 0087h and
 * never reaches the end of the field at all. Both halves are asserted for the
 * same reason the memory-address test asserts both: only the pair
 * distinguishes a part that masks from one that does not.
 */
static void test_ds2502_status_start_address_is_not_masked(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    const uint8_t sent_hdr[3] = { 0xaa, 0x87, 0x00 };
    const uint8_t masked_hdr[3] = { 0xaa, 0x07, 0x00 };
    uint8_t echo;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xaa);       /* Read Status */
    bus_touch_byte(bus, 0x87);       /* TA1: 0087h, above the memory top */
    bus_touch_byte(bus, 0x00);       /* TA2 */

    echo = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(echo, ==, onewire_crc8(0, sent_hdr, 3));
    g_assert_cmpuint(echo, !=, onewire_crc8(0, masked_hdr, 3));

    /*
     * 0087h is not 0007h, so this is unprogrammed status EPROM, not the
     * factory 00h; and nothing closes the field, so the byte after it is
     * more of the same rather than a CRC-8 over what came before.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

/*
 * Both parts decode the full 0000h-007Fh space; they differ only in how
 * many pages are programmable. A DS2501 (two pages) must read 0xff at
 * byte 64 even when handed a full 128-byte image, while a DS2502 (four
 * pages) must read the real programmed byte there. Getting either part a
 * shortened address space, or failing to bound the programmable area by
 * page count, would pass one of these two assertions and fail the other.
 */
static void test_ds2502_pages_bound_the_programmable_area(void)
{
    uint8_t image[DS2502_DATA_SIZE];
    OneWireBus *bus2501, *bus2502;

    memset(image, 0x5a, sizeof(image));

    bus2501 = onewire_test_bus();
    onewire_test_add_ds2502_variant(bus2501, TYPE_ONEWIRE_DS2501,
                                     0x91, 0x5e7000000001ULL,
                                     image, sizeof(image));

    bus2502 = onewire_test_bus();
    onewire_test_add_ds2502_variant(bus2502, TYPE_ONEWIRE_DS2502,
                                     0x09, 0x0102030405ULL,
                                     image, sizeof(image));

    /*
     * Skip ROM, Read Memory from 0x0040 (start of page 2), consume the
     * header CRC, and read the first data byte.
     */
    g_assert_true(onewire_reset(bus2501));
    bus_touch_byte(bus2501, 0xcc);
    bus_touch_byte(bus2501, 0xf0);
    bus_touch_byte(bus2501, 0x40);
    bus_touch_byte(bus2501, 0x00);
    bus_touch_byte(bus2501, 0xff);              /* header CRC, discarded */
    g_assert_cmpuint(bus_touch_byte(bus2501, 0xff), ==, 0xff);

    g_assert_true(onewire_reset(bus2502));
    bus_touch_byte(bus2502, 0xcc);
    bus_touch_byte(bus2502, 0xf0);
    bus_touch_byte(bus2502, 0x40);
    bus_touch_byte(bus2502, 0x00);
    bus_touch_byte(bus2502, 0xff);
    g_assert_cmpuint(bus_touch_byte(bus2502, 0xff), ==, 0x5a);
}

/*
 * These parts are modelled add-only: Write Memory (0Fh) and Write Status
 * (55h) must be refused rather than misread as a Read Memory or Read
 * Status. If either were instead accepted and treated as though it needed
 * TA1/TA2, the next two bytes here would be consumed as a bogus address
 * instead of a fresh command, and the header CRC checked below would not
 * come back matching {0xf0,0x00,0x00}.
 */
static void test_ds2502_write_commands_are_refused_not_misread(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    const uint8_t hdr[3] = { 0xf0, 0x00, 0x00 };
    const uint8_t write_cmds[2] = { 0x0f, 0x55 };

    for (unsigned i = 0; i < 2; i++) {
        g_assert_true(onewire_reset(bus));
        bus_touch_byte(bus, 0xcc);
        bus_touch_byte(bus, write_cmds[i]);

        bus_touch_byte(bus, 0xf0);
        bus_touch_byte(bus, 0x00);
        bus_touch_byte(bus, 0x00);
        g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                          onewire_crc8(0, hdr, 3));
    }
}

/*
 * DS2505-family.
 * The Bridge's board-identity part and the Hub's node-record part are both
 * DS2505-class. Unlike the DS2502 family, this class echoes no header CRC
 * after TA2, content starts one window earlier, and its command set
 * differs: F0h's own CRC-16 covers the header too, AAh serves paged
 * status with a CRC-16 per page, and A5h replaces C3h entirely.
 */

static void test_ds2505_plain_read_has_no_page_crc(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    uint8_t bytes[40];

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);                 /* Skip ROM */
    bus_touch_byte(bus, 0xf0);                 /* Read Memory */
    bus_touch_byte(bus, 0x00);
    bus_touch_byte(bus, 0x00);

    /*
     * Unlike the DS2502 family, this class echoes no header CRC: the very
     * next window is already the first data byte.
     */
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = bus_touch_byte(bus, 0xff);
    }

    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmpuint(bytes[i], ==, 0x00);          /* page 0 */
    }
    /*
     * Byte 33 (index 32) is page one's first byte, a real data byte, not
     * a CRC. A model that copied A5h's or AAh's per-32-byte framing onto
     * F0h would answer something else here.
     */
    for (unsigned i = 32; i < sizeof(bytes); i++) {
        g_assert_cmpuint(bytes[i], ==, 0x01);
    }
}

/*
 * Read Memory's CRC-16, when it appears, covers the header AND the data
 * read, unlike the DS2502-family F0h CRC-8, which covers data only.
 * Start two bytes from the end of the 2048-byte field so both the header
 * inclusion and the field's true top (07FFh) are exercised together.
 */
static void test_ds2505_read_memory_emits_one_crc16_at_end_of_field(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t hdr[3] = { 0xf0, 0xfe, 0x07 };     /* TA: 07FEh */
    uint8_t tail[2];
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xf0);
    bus_touch_byte(bus, 0xfe);
    bus_touch_byte(bus, 0x07);

    tail[0] = bus_touch_byte(bus, 0xff);    /* 07FEh: page 63 */
    tail[1] = bus_touch_byte(bus, 0xff);    /* 07FFh: the last byte */
    g_assert_cmpuint(tail[0], ==, 0x3f);
    g_assert_cmpuint(tail[1], ==, 0x3f);

    crc = onewire_crc16(onewire_crc16(0, hdr, 3), tail, sizeof(tail));
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;

    /*
     * The transmitted value is inverted and covers the header. A model
     * that instead sent the raw (un-inverted) CRC-16, or one that covered
     * only the data bytes the way the DS2502-family CRC-8 does, would both
     * fail this check.
     */
    g_assert_cmpuint(got, ==, (uint16_t)~crc);

    /* Nothing further is defined; the part floats high, not repeats. */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

static void test_ds2505_status_page_carries_a_real_crc16(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t hdr[3] = { 0xaa, 0x00, 0x00 };
    uint8_t status[8];
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xaa);                 /* Read Status */
    bus_touch_byte(bus, 0x00);
    bus_touch_byte(bus, 0x00);

    for (unsigned i = 0; i < sizeof(status); i++) {
        status[i] = bus_touch_byte(bus, 0xff);
    }
    /*
     * This model has no Write Status: every status byte, implemented
     * region or not, is unprogrammed 0xff.
     */
    for (unsigned i = 0; i < sizeof(status); i++) {
        g_assert_cmpuint(status[i], ==, 0xff);
    }

    crc = onewire_crc16(onewire_crc16(0, hdr, 3), status, sizeof(status));
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    /*
     * A model that answered a flat, CRC-less status field as on the
     * DS2502 family's tiny, single-CRC status field would fail this check,
     * even though the FFh data bytes above already
     * pass either way.
     */
    g_assert_cmpuint(got, ==, (uint16_t)~crc);
}

/*
 * The first status page's CRC-16 covers the header; every later page's
 * covers only that page's own bytes, with the generator cleared in
 * between. A model that instead carried the running CRC across pages, or
 * kept including the header on every page, would answer something else
 * for the second page's CRC.
 */
static void test_ds2505_status_second_page_crc_is_cleared(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    uint8_t page2[8];
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xaa);
    bus_touch_byte(bus, 0x00);
    bus_touch_byte(bus, 0x00);

    for (unsigned i = 0; i < 8; i++) {
        bus_touch_byte(bus, 0xff);              /* page one's data */
    }
    bus_touch_byte(bus, 0xff);                  /* page one's CRC, low */
    bus_touch_byte(bus, 0xff);                  /* page one's CRC, high */

    for (unsigned i = 0; i < sizeof(page2); i++) {
        page2[i] = bus_touch_byte(bus, 0xff);
    }
    g_assert_cmpuint(page2[7], ==, 0xff);      /* still blank: 0008h-000Fh */

    crc = onewire_crc16(0, page2, sizeof(page2));
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    g_assert_cmpuint(got, ==, (uint16_t)~crc);
}

/*
 * A5h returns the addressed page redirection byte, then that same physical
 * page regardless of redirection. Page 3 contains 0x03; following the blank
 * 0xff redirection byte would instead return page 0's 0x00 marker, since its
 * one's-complement page number is zero.
 */
static void
test_ds2505_extended_read_surfaces_redirection_without_following(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t hdr[3] = { 0xa5, 0x60, 0x00 };   /* TA: 0060h, page 3 */
    uint8_t redirect;
    uint8_t page[DS2505_PAGE_SIZE];
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xa5);                 /* Extended Read Memory */
    bus_touch_byte(bus, 0x60);
    bus_touch_byte(bus, 0x00);

    redirect = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(redirect, ==, 0xff);      /* not redirected */

    crc = onewire_crc16(onewire_crc16(0, hdr, 3), &redirect, 1);
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    g_assert_cmpuint(got, ==, (uint16_t)~crc);

    for (unsigned i = 0; i < sizeof(page); i++) {
        page[i] = bus_touch_byte(bus, 0xff);
    }
    for (unsigned i = 0; i < sizeof(page); i++) {
        g_assert_cmpuint(page[i], ==, 0x03);
    }

    crc = onewire_crc16(0, page, sizeof(page));
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    g_assert_cmpuint(got, ==, (uint16_t)~crc);
}

/*
 * Between pages, 24 slots carry the next page's redirection byte and CRC-16.
 * Clear the CRC generator: this CRC covers only that byte, unlike the first
 * page, whose CRC includes the header.
 */
static void test_ds2505_extended_read_inter_page_sequence(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    uint8_t redirect;
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xa5);
    bus_touch_byte(bus, 0x00);
    bus_touch_byte(bus, 0x00);

    bus_touch_byte(bus, 0xff);                  /* page 0's redirection */
    bus_touch_byte(bus, 0xff);                  /* its CRC, low */
    bus_touch_byte(bus, 0xff);                  /* its CRC, high */
    for (unsigned i = 0; i < DS2505_PAGE_SIZE; i++) {
        bus_touch_byte(bus, 0xff);              /* page 0's data */
    }
    bus_touch_byte(bus, 0xff);                  /* page 0's data CRC, low */
    bus_touch_byte(bus, 0xff);                  /* page 0's data CRC, high */

    redirect = bus_touch_byte(bus, 0xff);       /* page 1's redirection */
    g_assert_cmpuint(redirect, ==, 0xff);

    crc = onewire_crc16(0, &redirect, 1);
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    g_assert_cmpuint(got, ==, (uint16_t)~crc);

    /* Page one's own data follows, not a repeat of the redirection pair. */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0x01);
}

/*
 * A5h's first page can start mid-page too, exactly as C3h's does on the
 * DS2502 family: the redirection byte and its CRC still cover the page
 * containing the start address, and the page-data CRC covers only the
 * bytes actually read, from the start address to that page's own end.
 */
static void test_ds2505_extended_read_first_page_can_start_mid_page(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t hdr[3] = { 0xa5, 0x10, 0x00 };   /* TA: 0010h, page 0 */
    uint8_t redirect;
    uint8_t partial[16];
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xa5);
    bus_touch_byte(bus, 0x10);
    bus_touch_byte(bus, 0x00);

    redirect = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(redirect, ==, 0xff);

    crc = onewire_crc16(onewire_crc16(0, hdr, 3), &redirect, 1);
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    g_assert_cmpuint(got, ==, (uint16_t)~crc);

    for (unsigned i = 0; i < sizeof(partial); i++) {
        partial[i] = bus_touch_byte(bus, 0xff);
    }
    for (unsigned i = 0; i < sizeof(partial); i++) {
        g_assert_cmpuint(partial[i], ==, 0x00);   /* still page 0 */
    }

    /*
     * Page 0 ends at 001Fh, 16 bytes from the 0010h start, not 32. A
     * model that always waited for a full 32-byte count instead of
     * computing the page boundary from the actual address would answer a
     * real data byte here instead of this CRC.
     */
    crc = onewire_crc16(0, partial, sizeof(partial));
    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    g_assert_cmpuint(got, ==, (uint16_t)~crc);

    /* Page one starts fresh at 0020h: its redirection byte, not more CRC. */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

/*
 * Past a short image, reads return 0xff through the 2048-byte field without
 * wrapping to page zero. The older Bridge responder does wrap out-of-range
 * offsets. At address 500, modulo indexing a four-byte image would return
 * its first byte instead.
 */
static void test_ds2505_out_of_range_read_pads_not_wraps(void)
{
    const uint8_t short_image[4] = { 0x11, 0x22, 0x33, 0x44 };
    OneWireBus *bus = onewire_test_bus();
    uint8_t byte;

    onewire_test_add_ds2505(bus, 0x0b, 0x0102030405ULL,
                             short_image, sizeof(short_image));

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xf0);
    bus_touch_byte(bus, 0xf4);      /* TA1: 01F4h = 500, far past 4 bytes */
    bus_touch_byte(bus, 0x01);      /* TA2 */

    byte = bus_touch_byte(bus, 0xff);
    /*
     * A model that indexed the image with addr % image_len instead of a
     * direct, full-width array read would answer short_image[500 % 4] =
     * short_image[0] = 0x11 here instead.
     */
    g_assert_cmpuint(byte, ==, 0xff);
}

/*
 * "A start address above the implemented top has its high address bits
 * forced to zero" (DS2505 datasheet).
 * TA1=TA2=0xff sends a raw 16-bit start address of 0xffff; masked to the
 * field's 11-bit width that lands on 07FFh, the field's true last byte.
 * page 63's own marker, not some other page's, confirming the mask width
 * as well as its presence.
 */
static void test_ds2505_start_address_masked_to_top(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t masked_hdr[3] = { 0xf0, 0xff, 0x07 };
    const uint8_t sent_hdr[3] = { 0xf0, 0xff, 0xff };
    uint8_t last_byte;
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xf0);

    /*
     * Every bit here is a driven write-one: exactly the case that exposes
     * a part wrongly pulling the line low during a master-write window.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);    /* TA1 */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);    /* TA2 */

    last_byte = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(last_byte, ==, 0x3f);      /* page 63's own marker */

    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    /*
     * The header CRC covers the address AS MASKED (0xff, 0x07), and both
     * halves are asserted: it matches a check over 07FFh and does not
     * match one over the FFFFh the master sent. That gap is the
     * datasheet's own deliberate mismatch, the same behaviour as the
     * DS2502 family's start-address masking, and it is what stops an
     * out-of-range read looking to the master like a successful one.
     */
    crc = onewire_crc16(onewire_crc16(0, masked_hdr, 3), &last_byte, 1);
    g_assert_cmpuint(got, ==, (uint16_t)~crc);
    crc = onewire_crc16(onewire_crc16(0, sent_hdr, 3), &last_byte, 1);
    g_assert_cmpuint(got, !=, (uint16_t)~crc);
}

/*
 * Read Status masks its start address like Read Memory. Blank status bytes
 * are 0xff at every address, so only the closing CRC distinguishes a masked
 * read from a refused read whose bus floats high. Two older hand-written
 * responders instead refuse status reads above a fixed offset.
 */
static void test_ds2505_status_start_address_masked_to_top(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t masked_hdr[3] = { 0xaa, 0xff, 0x07 };
    const uint8_t sent_hdr[3] = { 0xaa, 0xff, 0xff };
    uint8_t last_byte;
    uint16_t crc, got;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);
    bus_touch_byte(bus, 0xaa);

    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);    /* TA1 */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);    /* TA2 */

    last_byte = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(last_byte, ==, 0xff);      /* status: always blank */

    got = bus_touch_byte(bus, 0xff);
    got |= (uint16_t)bus_touch_byte(bus, 0xff) << 8;
    /*
     * As with the plain-read case, the header CRC covers the address as
     * masked and not as sent, and both halves are asserted. Here that is
     * the only thing separating "masked, then answered" from "answered at
     * FFFFh", since every status byte reads blank either way.
     */
    crc = onewire_crc16(onewire_crc16(0, masked_hdr, 3), &last_byte, 1);
    g_assert_cmpuint(got, ==, (uint16_t)~crc);
    crc = onewire_crc16(onewire_crc16(0, sent_hdr, 3), &last_byte, 1);
    g_assert_cmpuint(got, !=, (uint16_t)~crc);
}

/*
 * Add-only: Write Memory, Speed Write Memory, Write Status and Speed
 * Write Status are all refused rather than misread as a read. 0xC3 (the
 * DS2502-family CRC-read command) does not exist on this class either,
 * and is refused the same way. If any were accepted as needing TA1/TA2, the
 * following bytes here would be consumed as a
 * bogus address instead of a fresh command, and the Read Memory issued
 * right after would not come back with page three's real marker.
 */
static void test_ds2505_write_commands_are_refused_not_misread(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2505_paged(0x0b,
                                                          0x0102030405ULL);
    const uint8_t refused_cmds[5] = { 0x0f, 0xf3, 0x55, 0xf5, 0xc3 };

    for (unsigned i = 0; i < ARRAY_SIZE(refused_cmds); i++) {
        g_assert_true(onewire_reset(bus));
        bus_touch_byte(bus, 0xcc);
        bus_touch_byte(bus, refused_cmds[i]);

        bus_touch_byte(bus, 0xf0);      /* a fresh Read Memory */
        bus_touch_byte(bus, 0x60);      /* TA1: 0060h, page 3 */
        bus_touch_byte(bus, 0x00);      /* TA2 */
        g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0x03);
    }
}

/*
 * SGI's MicroLAN slot decode (include/hw/misc/sgi-onewire-slot.h). One write
 * of a control register, decoded by pulse width into a reset, a read, or a
 * write-zero. Each of the following pins one of those three outcomes against
 * the others, because a decode that collapsed any two together would be a
 * real defect a wrong-but-plausible implementation could still pass.
 */

/*
 * A long pulse resets the bus and reports presence. onewire_reset() returns
 * true when a part answers, and a presence pulse pulls the wire LOW, so the
 * sampled level sgi_onewire_slot() returns is the negation: false here. An
 * implementation that forgot to invert, or that treated every pulse as a
 * reset, would return true instead.
 */
static void test_sgi_slot_reset_reports_presence_with_a_part(void)
{
    OneWireBus *bus = onewire_test_bus_with_part(0x0b, 0x010203040506ULL);

    g_assert_false(sgi_onewire_slot(bus, 500, 0));
}

/*
 * The same long pulse on an empty bus: nothing answers, so the wire stays
 * released and the sampled level is high. Together with the test above,
 * this is what tells presence-detected apart from presence-absent rather
 * than a decode that returns a fixed value regardless of the bus.
 */
static void test_sgi_slot_reset_reports_absence_on_empty_bus(void)
{
    OneWireBus *bus = onewire_test_bus();

    g_assert_true(sgi_onewire_slot(bus, 500, 0));
}

/*
 * Short pulses release the wire and sample the slave. Skip ROM selects a
 * responder whose function_byte returns its family byte. The one-window lag
 * (include/hw/1wire/onewire.h) requires a priming command; eight following
 * read slots must return 0x0b. Treating a short pulse as write-zero would
 * hold the line low and return 0x00 instead.
 */
static void test_sgi_slot_short_pulse_reads(void)
{
    OneWireBus *bus = onewire_test_bus_with_reply_part(0x0b, 0x010203040506ULL);
    uint8_t reply = 0;

    g_assert_true(onewire_reset(bus));
    bus_touch_byte(bus, 0xcc);                  /* Skip ROM */
    bus_touch_byte(bus, 0xaa);                  /* prime the reply */

    for (unsigned i = 0; i < 8; i++) {
        if (sgi_onewire_slot(bus, 1, 0)) {
            reply |= 1u << i;
        }
    }
    g_assert_cmpuint(reply, ==, 0x0b);
}

/*
 * A mid-range pulse writes zero. Family 0x0b is 00001011b, LSB first, so
 * zero mismatches its first Match ROM bit and immediately enters ONEWIRE_IDLE
 * (also tested through onewire_touch_bit). A read or write-one would leave
 * the part in ONEWIRE_ROM_MATCH; checking its phase proves it received zero.
 */
static void test_sgi_slot_mid_pulse_writes_zero(void)
{
    OneWireBus *bus = onewire_test_bus();
    DeviceState *dev = qdev_new(TYPE_ONEWIRE_TEST_STUB);
    OneWireSlave *s = ONEWIRE_SLAVE(dev);

    qdev_prop_set_uint8(dev, "family", 0x0b);
    qdev_prop_set_uint64(dev, "serial", 0x010203040506ULL);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);

    g_assert_false(sgi_onewire_slot(bus, 500, 0));    /* reset: presence */
    bus_touch_byte(bus, 0x55);                        /* Match ROM */
    sgi_onewire_slot(bus, 100, 0);                    /* write-zero */

    g_assert_cmpint(s->phase, ==, ONEWIRE_IDLE);
}

/*
 * Reset starts at pulse width 400; 399 is write-zero. On an empty bus,
 * reset samples high and write-zero drives low. A pulse > 400 comparison
 * would misclassify 400 and make both results false.
 */
static void test_sgi_slot_reset_threshold_is_closed_at_400(void)
{
    OneWireBus *bus = onewire_test_bus();

    g_assert_true(sgi_onewire_slot(bus, 400, 0));     /* reset: no presence */
    g_assert_false(sgi_onewire_slot(bus, 399, 0));    /* write-zero: low */
}

/*
 * The write-zero/read boundary is closed on the write-zero side: pulse
 * width exactly 40 must already hold the wire down, and 39 must
 * release it. Checked on an empty bus, where a write-zero always
 * samples low (drive false) and a read always samples high with nobody
 * pulling. A `pulse <= 40` off-by-one, instead of `pulse < 40`, would
 * misclassify exactly 40 as a read and return true here too.
 */
static void test_sgi_slot_zero_threshold_is_closed_at_40(void)
{
    OneWireBus *bus = onewire_test_bus();

    g_assert_false(sgi_onewire_slot(bus, 40, 0));     /* write-zero: low */
    g_assert_true(sgi_onewire_slot(bus, 39, 0));      /* released high read */
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
    g_test_add_func("/onewire/bus/search-rom-arbitrates-two-parts",
                     test_search_rom_arbitrates_two_parts);

    g_test_add_func("/onewire/ds2502/plain-read-has-no-page-crc",
                     test_ds2502_plain_read_has_no_page_crc);
    g_test_add_func("/onewire/ds2502/status-serves-the-real-field",
                     test_ds2502_status_serves_the_real_field);
    g_test_add_func("/onewire/ds2502/read-data-crc-emits-page-crc",
                     test_ds2502_read_data_crc_emits_page_crc_every_32_bytes);
    g_test_add_func("/onewire/ds2502/read-data-crc-first-page-mid-page",
                     test_ds2502_read_data_crc_first_page_can_start_mid_page);
    g_test_add_func("/onewire/ds2502/read-memory-emits-one-crc-at-end",
                     test_ds2502_read_memory_emits_one_crc_at_end_of_memory);
    g_test_add_func("/onewire/ds2502/start-address-above-top-is-masked",
                     test_ds2502_start_address_above_top_is_masked);
    g_test_add_func("/onewire/ds2502/status-start-address-is-not-masked",
                     test_ds2502_status_start_address_is_not_masked);
    g_test_add_func("/onewire/ds2502/pages-bound-the-programmable-area",
                     test_ds2502_pages_bound_the_programmable_area);
    g_test_add_func("/onewire/ds2502/write-commands-are-refused",
                     test_ds2502_write_commands_are_refused_not_misread);

    g_test_add_func("/onewire/ds2505/plain-read-has-no-page-crc",
                     test_ds2505_plain_read_has_no_page_crc);
    g_test_add_func("/onewire/ds2505/read-memory-emits-one-crc16-at-end",
                     test_ds2505_read_memory_emits_one_crc16_at_end_of_field);
    g_test_add_func("/onewire/ds2505/status-page-carries-a-real-crc16",
                     test_ds2505_status_page_carries_a_real_crc16);
    g_test_add_func("/onewire/ds2505/status-second-page-crc-is-cleared",
                     test_ds2505_status_second_page_crc_is_cleared);
    g_test_add_func(
        "/onewire/ds2505/extended-read-surfaces-redirection-without-following",
        test_ds2505_extended_read_surfaces_redirection_without_following);
    g_test_add_func("/onewire/ds2505/extended-read-inter-page-sequence",
                     test_ds2505_extended_read_inter_page_sequence);
    g_test_add_func("/onewire/ds2505/extended-read-first-page-mid-page",
                     test_ds2505_extended_read_first_page_can_start_mid_page);
    g_test_add_func("/onewire/ds2505/out-of-range-read-pads-not-wraps",
                     test_ds2505_out_of_range_read_pads_not_wraps);
    g_test_add_func("/onewire/ds2505/start-address-masked-to-top",
                     test_ds2505_start_address_masked_to_top);
    g_test_add_func("/onewire/ds2505/status-start-address-masked-to-top",
                     test_ds2505_status_start_address_masked_to_top);
    g_test_add_func("/onewire/ds2505/write-commands-are-refused",
                     test_ds2505_write_commands_are_refused_not_misread);

    g_test_add_func("/onewire/sgi-slot/reset-reports-presence-with-a-part",
                     test_sgi_slot_reset_reports_presence_with_a_part);
    g_test_add_func("/onewire/sgi-slot/reset-reports-absence-on-empty-bus",
                     test_sgi_slot_reset_reports_absence_on_empty_bus);
    g_test_add_func("/onewire/sgi-slot/short-pulse-reads",
                     test_sgi_slot_short_pulse_reads);
    g_test_add_func("/onewire/sgi-slot/mid-pulse-writes-zero",
                     test_sgi_slot_mid_pulse_writes_zero);
    g_test_add_func("/onewire/sgi-slot/reset-threshold-is-closed-at-400",
                     test_sgi_slot_reset_threshold_is_closed_at_400);
    g_test_add_func("/onewire/sgi-slot/zero-threshold-is-closed-at-40",
                     test_sgi_slot_zero_threshold_is_closed_at_40);

    return g_test_run();
}
