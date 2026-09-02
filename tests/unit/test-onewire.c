#include "qemu/osdep.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/onewire.h"
#include "hw/1wire/ds2502.h"
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

/*
 * A minimal slave with no memory function: it exercises the ROM layer
 * alone. It needs no realize of its own -- the abstract onewire-slave's
 * own realize computes the identity from the family/serial properties
 * automatically, which is exactly the behaviour that makes this type
 * trivial to add.
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
 * A slave whose memory function always answers its own family byte,
 * regardless of what it is asked. It exists so a test can tell both
 * whether ONEWIRE_FUNCTION was ever reached at all -- an unselected or
 * IDLE part never calls function_byte, so the bus floats back whatever the
 * caller drove instead of a real reply -- and, with two of these on one
 * bus, which specific part answered.
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
 * Adds a DS2501 or DS2502 to a bus, optionally loading an EPROM image
 * before realize -- ds2502_set_image() must run before the device is
 * realized, matching how a real machine model provisions one of these
 * parts.
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
     * deselected -- and a model that ignored the mismatch and selected
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
     * Only one part on the bus: it must transmit its own identity bit on
     * the identify slot and the bit's inverse on the complement slot, in
     * that order -- a model with the two slots swapped would transmit the
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

/*
 * The bus is a bus because more than one part can be on it, and Search ROM
 * is the one ROM command where that actually shows: every other command is
 * entirely master-driven, so a single-part test cannot exercise the
 * wired-AND arbitration at all. Two parts here differ only in the low bit
 * of the family byte, so the very first identity bit is a genuine
 * discrepancy -- both parts are still alive and drive opposite values, so
 * the wired AND of "pull low" from either one produces 0 on both the
 * identify and complement slots, unlike a single live part which always
 * shows the two slots as complements of each other. A model that
 * short-circuited the per-slave OR (stopping at the first slave that
 * pulled, rather than always polling every slave so a migratable part
 * never falls behind bit-for-bit) or dropped the open-drain "master AND
 * every slave" term entirely would not reproduce this 0/0 signature.
 *
 * After that one discrepancy this test always steers toward the part
 * whose family byte is 0x0b, so the OTHER part must go silent from there
 * on -- covered by the remaining 63 bits landing on ordinary, single-part
 * values instead of more (0,0) discrepancies, even though the two parts'
 * identities keep differing in their CRC bytes. If the dropped part failed
 * to go silent and kept driving, those later bits would show spurious
 * discrepancies instead.
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
 * DS2502-family (docs/library/dallas-1wire-part-specs.md section C.1).
 * The IOC3's part is a DS2501 -- family 0x91, two programmable pages,
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
     * value meaning "not redirected" -- the byte OpenBSD's redirection
     * walk stops on, and it must fall out of blank status memory rather
     * than a coded special case. 0x0007 is the one byte that actually
     * distinguishes this field: factory-programmed 00h, so a genuine
     * DS2502-class status readout is never all-0xff even on a blank part.
     * A regression in DS2502_STATUS_FACTORY_ADDR or in
     * ds2502_status_byte()'s equality check would only show up by reading
     * this far -- the same reason the closing CRC-8 below is checked too.
     */
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmpuint(status[i], ==, 0xff);
    }
    g_assert_cmpuint(status[7], ==, 0x00);

    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, status, sizeof(status)));
}

/*
 * The behaviour this task exists to get right: unlike F0h, C3h closes
 * every 32-byte page with a device-generated CRC-8 over that page's data.
 * A model that copied F0h's framing onto C3h -- the exact bug flagged in
 * the datasheet extraction's "Contradictions" section -- would answer
 * 0xff for byte 33 here instead, the same wrong answer
 * test_ds2502_plain_read_has_no_page_crc's F0h case correctly gives.
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
 * the start address to the page's end -- not a fixed 32 bytes. The
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
     * Page 0 ends at 001Fh, 16 bytes from the 0010h start -- not 32. A
     * model that only ever waited for a full 32-byte count instead of
     * computing the page boundary from the actual address would answer a
     * plain data byte here instead of this CRC.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, partial, sizeof(partial)));

    /*
     * Page one starts fresh, page-aligned, at 0020h -- real data again,
     * not a second CRC.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, 0xff);
}

/*
 * Read Memory's one CRC-8 only appears if the read actually runs through
 * the last byte of memory (007Fh) -- unlike C3h, which never stops.
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
    tail[1] = bus_touch_byte(bus, 0xff);    /* 007Fh: the last byte of memory */

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
 * zero" (docs/library/dallas-1wire-part-specs.md section C.1). TA1=TA2=
 * 0xff sends a raw 16-bit start address of 0xffff; masked to 7 bits that
 * lands on 007Fh, the single last byte of memory. A model using the raw,
 * unmasked address would never satisfy the addr==007Fh check that
 * produces the closing CRC below, and so would answer 0xff forever
 * instead -- a different, and wrong, failure than getting the mask width
 * wrong (which would land somewhere else in 0000h-007Fh instead of at the
 * very end, and also never trip this same check).
 */
static void test_ds2502_start_address_above_top_is_masked(void)
{
    OneWireBus *bus = onewire_test_bus_with_ds2501(0x91, 0x5e7000000001ULL);
    const uint8_t hdr[3] = { 0xf0, 0xff, 0xff };
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
     * The echoed header CRC-8 covers the raw bytes actually sent, not the
     * masked address -- the datasheet's own "deliberate checksum
     * mismatch" note is about exactly this gap.
     */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==, onewire_crc8(0, hdr, 3));

    last_byte = bus_touch_byte(bus, 0xff);
    g_assert_cmpuint(last_byte, ==, 0xff);      /* blank EPROM at 007Fh */
    g_assert_cmpuint(bus_touch_byte(bus, 0xff), ==,
                      onewire_crc8(0, &last_byte, 1));
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
 * A short pulse is a read: it releases the wire and samples whatever a part
 * drives back. Skip ROM selects the reply part, whose function_byte always
 * answers its own family byte; per the one-window lag (include/hw/1wire/
 * onewire.h), that reply appears starting the byte AFTER the one that
 * primes it, so a command byte is written first. Reading the primed reply
 * back eight short-pulse slots at a time must reproduce the family byte. An
 * implementation that instead read sgi_onewire_slot(bus, pulse,
 * sample)'s short-pulse branch as a write-zero -- driving the wire low
 * instead of releasing it -- would read back 0x00 regardless of what the
 * part sends, not the real 0x0b.
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
 * A mid-range pulse holds the wire down: a write-zero. Family byte 0x0b is
 * 00001011b, LSB first, so its first identity bit is 1 -- driving a zero
 * there mismatches on the very first bit of Match ROM, which drops the part
 * to ONEWIRE_IDLE immediately (test_match_rom_drops_at_first_mismatch
 * exercises the same mechanism directly against onewire_touch_bit). This
 * observes that the part actually received a zero, not merely that
 * sgi_onewire_slot() returned some value: an implementation that instead
 * decoded a mid-range pulse as a read or a write-one would drive/echo a 1
 * on that slot, the part would keep matching, and the phase would still be
 * ONEWIRE_ROM_MATCH here rather than IDLE.
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
    g_test_add_func("/onewire/ds2502/pages-bound-the-programmable-area",
                     test_ds2502_pages_bound_the_programmable_area);
    g_test_add_func("/onewire/ds2502/write-commands-are-refused",
                     test_ds2502_write_commands_are_refused_not_misread);

    g_test_add_func("/onewire/sgi-slot/reset-reports-presence-with-a-part",
                     test_sgi_slot_reset_reports_presence_with_a_part);
    g_test_add_func("/onewire/sgi-slot/reset-reports-absence-on-empty-bus",
                     test_sgi_slot_reset_reports_absence_on_empty_bus);
    g_test_add_func("/onewire/sgi-slot/short-pulse-reads",
                     test_sgi_slot_short_pulse_reads);
    g_test_add_func("/onewire/sgi-slot/mid-pulse-writes-zero",
                     test_sgi_slot_mid_pulse_writes_zero);

    return g_test_run();
}
