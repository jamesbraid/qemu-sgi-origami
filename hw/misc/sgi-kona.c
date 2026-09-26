/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI KONA: the InfiniteReality graphics pipe.
 *
 * An sgi-xtalk-board subclass, built the way hw/mips/sgi/xio-baseio.c
 * builds the BaseIO card: this file owns only the widget itself, attached
 * through the general crosstalk framework at a free Origin 2000 XIO slot.
 * The widget answers the PROM's widget-header probe at `+0x4`, its
 * three-bit subsystem-classification probe at `+0x64`
 * (docs/datasheets/infinitereality-host-interface.md, docs/designs/
 * infinitereality-host-command-approximation.md), and the one-wire
 * identity chain at `+0x98`, reusing hw/1wire's own DS2502 model and the
 * shared MicroLAN slot decode exactly as hw/pci-host/sgi-bridge.c's own
 * NIC register does.
 *
 * Increment 2 adds the register window at `+0x800xxx` (credit, mailbox,
 * cause, reset and EEPROM-fallback registers) and the PROM textport's own
 * command port at `+0xc10000`, framed through hw/misc/sgi-command-port.c
 * with a bulk-transfer layer of this file's own in front of it for the
 * bitmap/glyph blit command (0xa), plus a native 1280x1024 framebuffer
 * published as a display surface MGRAS-style: native storage first,
 * conversion on damage. See docs/designs/
 * infinitereality-host-command-approximation.md for what each of these
 * approximates and why. Every other offset still reads zero and accepts
 * any write, logged as unimplemented.
 *
 * Both one-wire parts this model seeds -- one geometry-board record, one
 * Ktown record -- are synthetic beyond their part field. No one-wire
 * capture of a real InfiniteReality pipe has been reviewed; only the
 * board part numbers (030-1398-001, 030-1067-001) come from the reviewed
 * rack capture. See the commit introducing this file and the
 * approximation ledger for the disclosure.
 *
 * Started from two spike trees, neither of whose logging or bespoke
 * glue survives here: the VPro stub (sgi-origami-vpro-stub-spike,
 * 7aa883ab0f, hw/misc/sgi-odyssey-stub.c) and the InfiniteReality stub
 * spike (sgi-origami-ir-stub-spike, 6e5f474fa3).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/1wire/ds2502.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/misc/sgi-kona.h"
#include "hw/misc/sgi-kona-ports.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "ui/console.h"
#include "trace.h"

/* Widget-header identification: revision:part:manufacturer:1, generic. */
#define WIDGET_ID_REVISION_SHIFT     28
#define WIDGET_ID_PART_SHIFT         12
#define WIDGET_ID_MANUFACTURER_SHIFT 1
#define WIDGET_ID_ALWAYS_ONE         1

/*
 * The DS2502's real family code (Dallas 1-Wire family list), not synthetic.
 * Bridge's own board-identity parts are the larger DS2505 (2048 bytes), but
 * no reviewed capture or datasheet says which part a real InfiniteReality
 * board carries, and the DS2502's 128 bytes hold the shared record with
 * room to spare, so this model keeps the part it already had rather than
 * switching for no evidenced reason.
 */
#define SGI_KONA_ONEWIRE_FAMILY 0x09

/* Two placeholder identities; no reviewed capture assigns real serials. */
#define SGI_KONA_GEOMETRY_SERIAL 0x000000000001ULL
#define SGI_KONA_KTOWN_SERIAL    0x000000000002ULL

/*
 * Two board-identity records, built through hw/pci-host/sgi-bridge.c's own
 * shared fixed-offset record layout (docs/datasheets/bridge.md, "The
 * record layout, and every constraint on building it") rather than a
 * scanned ";Field:value;" text of this model's own invention. Only
 * barcode, part, revision and name are stored fields; the record's group,
 * capability and variety bytes are always 0xff (unprogrammed), and its
 * "Laser" display field is the part's own one-wire ROM number (the
 * "serial" property below), not a stored field at all -- so this model
 * carries none of those as a field of its own. Part carries the reviewed
 * rack capture's real board part number; barcode, revision and name are
 * synthetic placeholders pending a real one-wire capture.
 */
static const SGIBridgeBoardRecord sgi_kona_geometry_record = {
    .barcode = "00000001",
    .part = "030-1398-001",
    .revision = "01",
    .name = "GEOM-BD",
};
static const SGIBridgeBoardRecord sgi_kona_ktown_record = {
    .barcode = "00000002",
    .part = "030-1067-001",
    .revision = "01",
    .name = "KTOWN-BD",
};

/* Realize one DS2502 identity part onto the board's own one-wire bus. */
static bool sgi_kona_onewire_attach_part(
    SGIKonaState *s, const char *child_name,
    const SGIBridgeBoardRecord *record, uint64_t serial, Error **errp)
{
    uint8_t rom[SGI_BRIDGE_ONEWIRE_RECORD_SIZE];
    DeviceState *part;

    sgi_bridge_onewire_build_record(rom, record);
    part = qdev_new(TYPE_ONEWIRE_DS2502);
    object_property_add_child(OBJECT(s), child_name, OBJECT(part));
    qdev_prop_set_uint8(part, "family", SGI_KONA_ONEWIRE_FAMILY);
    qdev_prop_set_uint64(part, "serial", serial);
    ds2502_set_image(part, rom, sizeof(rom));
    return qdev_realize_and_unref(part, BUS(s->onewire_bus), errp);
}

/*
 * A guest-controlled width*height could otherwise demand an arbitrarily
 * large payload allocation; clamp to the model's own fixed framebuffer
 * size, which is more than any real glyph/logo-scale blit needs. This
 * bound is this model's own safety choice, not a hardware fact.
 */
#define SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS \
    DIV_ROUND_UP((uint64_t)SGI_KONA_FB_WIDTH * SGI_KONA_FB_HEIGHT, 32)

static void sgi_kona_display_ensure_surface(SGIKonaState *s)
{
    DisplaySurface *ds = qemu_console_surface(s->con);

    if (ds && surface_data(ds) == (uint8_t *)s->framebuffer) {
        return;
    }
    ds = qemu_create_displaysurface_from(SGI_KONA_FB_WIDTH, SGI_KONA_FB_HEIGHT,
                                         PIXMAN_x8r8g8b8,
                                         SGI_KONA_FB_WIDTH * 4,
                                         (uint8_t *)s->framebuffer);
    qemu_console_set_surface(s->con, ds);
}

/*
 * No palette, gamma or cursor stage is established for KONA (unlike
 * hw/misc/sgi-mgras-display.c's CMAP+gamma+cursor resolve): the native
 * framebuffer is already stored in the surface's own pixel format, so
 * "conversion on damage" is this dirty-flag-gated identity update.
 */
static bool sgi_kona_gfx_update(void *opaque)
{
    SGIKonaState *s = opaque;

    if (s->framebuffer_dirty) {
        qemu_console_update_full(s->con);
        s->framebuffer_dirty = false;
    }
    return true;
}

static const GraphicHwOps sgi_kona_display_ops = {
    .gfx_update = sgi_kona_gfx_update,
};

static void sgi_kona_draw_point(SGIKonaState *s, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= SGI_KONA_FB_WIDTH || y >= SGI_KONA_FB_HEIGHT) {
        return;
    }
    s->framebuffer[y * SGI_KONA_FB_WIDTH + x] = s->draw_color;
    s->framebuffer_dirty = true;
}

static void sgi_kona_bitmap_reset(SGIKonaState *s)
{
    s->bitmap_state = SGI_KONA_BITMAP_IDLE;
    s->bitmap_header_have = 0;
    g_free(s->bitmap_payload);
    s->bitmap_payload = NULL;
    s->bitmap_payload_have = 0;
    s->bitmap_payload_expected = 0;
}

/*
 * Unpack the completed payload into the destination rectangle the
 * header names, one bit per pixel, MSB-first, row-major. The header's
 * own field order (x, y, width, height, then two unconfirmed fields)
 * and this bit order are both this model's own approximation; see the
 * approximation ledger.
 */
static void sgi_kona_bitmap_blit(SGIKonaState *s)
{
    int32_t x0 = (int16_t)s->bitmap_header[0];
    int32_t y0 = (int16_t)s->bitmap_header[1];
    uint64_t total = (uint64_t)s->bitmap_width * s->bitmap_height;
    uint64_t bit;

    for (bit = 0; bit < total; bit++) {
        uint32_t word = s->bitmap_payload[bit / 32];
        bool set = (word >> (31 - (bit % 32))) & 1;

        if (set) {
            sgi_kona_draw_point(s, x0 + (int32_t)(bit % s->bitmap_width),
                                y0 + (int32_t)(bit / s->bitmap_width));
        }
    }
}

static void sgi_kona_mailbox_enqueue(SGIKonaState *s, const uint32_t *words,
                                     unsigned count)
{
    unsigned i;

    assert(count <= SGI_KONA_MAILBOX_MAX_WORDS);
    for (i = 0; i < count; i++) {
        s->mailbox_queue[i] = words[i];
    }
    s->mailbox_len = count;
    s->mailbox_pos = 0;
    if (count > 0) {
        s->int_cause |= SGI_KONA_INT_CAUSE_MAILBOX;
    }
}

static void sgi_kona_textport_packet(void *opaque, unsigned port_id,
                                     uint32_t selector, const uint32_t *args,
                                     unsigned argc)
{
    SGIKonaState *s = opaque;

    switch (selector) {
    case 0x2:
        /*
         * Set color: approximated as a packed 0x00RRGGBB value, matching
         * docs/designs/rad4-one-mode-functional-approximation.md's own
         * RAD4 precedent.
         */
        s->draw_color = args[0] & 0x00ffffff;
        break;
    case 0x6:
        /* Point: plots (a, b) in the current draw color. */
        sgi_kona_draw_point(s, (int32_t)args[0], (int32_t)args[1]);
        break;
    case 0x8:
        /*
         * Screen size: accepted and traced only; the framebuffer stays
         * fixed at SGI_KONA_FB_WIDTH x SGI_KONA_FB_HEIGHT.
         */
        break;
    case 0x9:
        /* Sync probe. */
        sgi_kona_mailbox_enqueue(s, (uint32_t[]){ 0x1234abcd }, 1);
        break;
    case 0xc:
        /*
         * Graphics info: width/height are established; the rest of the
         * reply is approximated zero.
         */
        sgi_kona_mailbox_enqueue(
            s, (uint32_t[]){ 0, 0, 0, SGI_KONA_FB_WIDTH, SGI_KONA_FB_HEIGHT },
            5);
        break;
    case 0x1:
    case 0x3:
    case 0x4:
    case 0x5:
    case 0x7:
        /*
         * Argument shape is established; no rendering, blanking or
         * cursor role is confirmed for any of these -- see docs/
         * datasheets/infinitereality-display.md. Framed and traced only.
         */
        break;
    default:
        g_assert_not_reached(); /* not in sgi_kona_textport_selectors[] */
    }
    s->textport_expect_sync = true;
}

static void sgi_kona_textport_unsupported(void *opaque, unsigned port_id,
                                          uint32_t word)
{
    SGIKonaState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-kona: unsupported textport selector 0x%" PRIx32 "\n",
                  word);
    s->textport_expect_sync = true;
}

static const SGICommandPortOps sgi_kona_textport_ops = {
    .packet = sgi_kona_textport_packet,
    .unsupported = sgi_kona_textport_unsupported,
};

/*
 * The textport's own bulk-transfer framing, layered in front of the
 * generic command-port framer: strip the wire protocol's leading sync
 * word, then either forward a word to sgi_command_port_write() or, mid
 * bitmap-blit (command 0xa, excluded from the selector table for
 * exactly this reason), accumulate it into the header or payload.
 */
static void sgi_kona_textport_push(SGIKonaState *s, uint32_t word)
{
    if (s->textport_expect_sync) {
        if (word != SGI_KONA_TEXTPORT_SYNC_WORD) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: textport expected a sync word, got "
                          "0x%" PRIx32 "\n", word);
            return;
        }
        s->textport_expect_sync = false;
        return;
    }

    switch (s->bitmap_state) {
    case SGI_KONA_BITMAP_IDLE:
        if (word == SGI_KONA_BITMAP_SELECTOR) {
            s->bitmap_state = SGI_KONA_BITMAP_HEADER;
            s->bitmap_header_have = 0;
            return;
        }
        sgi_command_port_write(&s->textport, word, 4);
        return;
    case SGI_KONA_BITMAP_HEADER:
        s->bitmap_header[s->bitmap_header_have++] = word;
        if (s->bitmap_header_have < SGI_KONA_BITMAP_HEADER_WORDS) {
            return;
        }
        /*
         * h2/h3 = width/height: this model's own approximation, following
         * an x, y, width, height, ?, ? field order; see the ledger.
         */
        s->bitmap_width = s->bitmap_header[2];
        s->bitmap_height = s->bitmap_header[3];
        s->bitmap_payload_expected =
            DIV_ROUND_UP((uint64_t)s->bitmap_width * s->bitmap_height, 32);
        s->bitmap_payload_have = 0;
        if (s->bitmap_payload_expected == 0) {
            sgi_kona_bitmap_reset(s);
            s->textport_expect_sync = true;
            return;
        }
        if (s->bitmap_payload_expected > SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: bitmap blit %" PRIu32 "x%" PRIu32
                          " exceeds this model's payload bound; dropping\n",
                          s->bitmap_width, s->bitmap_height);
            sgi_kona_bitmap_reset(s);
            s->textport_expect_sync = true;
            return;
        }
        s->bitmap_payload = g_new0(uint32_t, s->bitmap_payload_expected);
        s->bitmap_state = SGI_KONA_BITMAP_PAYLOAD;
        return;
    case SGI_KONA_BITMAP_PAYLOAD:
        s->bitmap_payload[s->bitmap_payload_have++] = word;
        if (s->bitmap_payload_have < s->bitmap_payload_expected) {
            return;
        }
        sgi_kona_bitmap_blit(s);
        sgi_kona_bitmap_reset(s);
        s->textport_expect_sync = true;
        return;
    }
    g_assert_not_reached();
}

static uint64_t sgi_kona_read(void *opaque, hwaddr addr,
                              unsigned size)
{
    SGIKonaState *s = opaque;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;
    uint64_t value;

    switch (addr) {
    case SGI_KONA_WIDGET_ID:
        value = ((uint64_t)s->revision << WIDGET_ID_REVISION_SHIFT) |
                ((uint64_t)s->part << WIDGET_ID_PART_SHIFT) |
                ((uint64_t)s->manufacturer << WIDGET_ID_MANUFACTURER_SHIFT) |
                WIDGET_ID_ALWAYS_ONE;
        break;
    case SGI_KONA_WIDGET_CONTROL:
        value = s->widget_control;
        break;
    case SGI_KONA_PROBE:
        value = s->probe_value;
        break;
    case SGI_KONA_ONEWIRE:
        value = s->onewire_reg;
        break;
    case SGI_KONA_LOW_WINDOW_WRITE:
        value = s->low_window_write;
        break;
    case SGI_KONA_CREDIT:
        value = sgi_command_port_credit_read(&s->textport);
        break;
    case SGI_KONA_RESET_ARG:
        value = s->reset_arg;
        break;
    case SGI_KONA_INT_CAUSE:
        value = s->int_cause;
        break;
    case SGI_KONA_MAILBOX:
        value = s->mailbox_pos < s->mailbox_len ?
                s->mailbox_queue[s->mailbox_pos] : 0;
        break;
    case SGI_KONA_RESET_STROBE:
        value = s->reset_strobe;
        break;
    case SGI_KONA_EEPROM_ENABLE:
        value = s->eeprom_enable;
        break;
    case SGI_KONA_EEPROM_ADDR:
        value = s->eeprom_addr;
        break;
    case SGI_KONA_EEPROM_DATA:
        value = (s->eeprom_addr == SGI_KONA_EEPROM_CHECKED_ADDR) ?
                SGI_KONA_EEPROM_CHECKED_VALUE : 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unimplemented read of offset "
                      "0x%" HWADDR_PRIx "\n", addr);
        value = 0;
        break;
    }

    trace_sgi_kona_pio_read(cpu, addr, value, size);
    return value;
}

static void sgi_kona_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    SGIKonaState *s = opaque;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;

    trace_sgi_kona_pio_write(cpu, addr, value, size);

    switch (addr) {
    case SGI_KONA_WIDGET_CONTROL:
        /* Unknown control fields are stored without interpreted effects. */
        s->widget_control = value;
        break;
    case SGI_KONA_ONEWIRE: {
        uint32_t pulse = (value & SGI_KONA_ONEWIRE_PULSE_MASK) >>
                         SGI_KONA_ONEWIRE_PULSE_SHIFT;
        uint32_t sample = (value & SGI_KONA_ONEWIRE_SAMPLE_MASK) >>
                          SGI_KONA_ONEWIRE_SAMPLE_SHIFT;
        bool level = sgi_onewire_slot(s->onewire_bus, pulse, sample);

        /*
         * Matches the Bridge NIC register's own two-step behaviour: the
         * pulse/sample bits the guest wrote persist, and completion is
         * reported synchronously, inside this same write, with DONE
         * always set and LEVEL carrying this slot's sampled result.
         */
        s->onewire_reg = ((uint32_t)value & SGI_KONA_ONEWIRE_WRITABLE) |
                         SGI_KONA_ONEWIRE_DONE |
                         (level ? SGI_KONA_ONEWIRE_LEVEL : 0);
        break;
    }
    case SGI_KONA_LOW_WINDOW_WRITE:
        /* Observed value 0x23 during install; meaning unknown. */
        s->low_window_write = value;
        break;
    case SGI_KONA_RESET_ARG:
        s->reset_arg = value;
        break;
    case SGI_KONA_INT_CAUSE:
        /* Write-1-to-clear. */
        s->int_cause &= ~(uint32_t)value;
        /*
         * Re-arm MAILBOX synchronously if a next reply word is already
         * queued: this model has no separate device-side timing to
         * simulate the real interrupt's re-assertion latency.
         */
        if ((value & SGI_KONA_INT_CAUSE_MAILBOX) &&
            s->mailbox_pos < s->mailbox_len) {
            s->int_cause |= SGI_KONA_INT_CAUSE_MAILBOX;
        }
        break;
    case SGI_KONA_MAILBOX:
        /*
         * Any write value (evidence always shows 0) consumes the current
         * reply word; it does not touch SGI_KONA_INT_CAUSE.
         */
        if (s->mailbox_pos < s->mailbox_len) {
            s->mailbox_pos++;
        }
        break;
    case SGI_KONA_RESET_STROBE:
        s->reset_strobe = value;
        break;
    case SGI_KONA_EEPROM_ENABLE:
        s->eeprom_enable = value;
        break;
    case SGI_KONA_EEPROM_ADDR:
        s->eeprom_addr = value;
        break;
    case SGI_KONA_EEPROM_DATA:
        /* Primes the read side; this model has no separate state to arm. */
        break;
    case SGI_KONA_TEXTPORT_PORT:
        sgi_kona_textport_push(s, value);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unimplemented write of offset "
                      "0x%" HWADDR_PRIx " (value 0x%" PRIx64 ")\n",
                      addr, value);
        break;
    }
}

/*
 * Crosstalk widget registers are word-sized; both bounds declared so the
 * core rejects an access outside them instead of reshaping it, matching
 * hw/pci-host/sgi-bridge.c's own register file.
 */
static const MemoryRegionOps sgi_kona_ops = {
    .read = sgi_kona_read,
    .write = sgi_kona_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static bool sgi_kona_build_widget(SGIXioBoardState *board,
                                  Error **errp)
{
    SGIKonaState *s = SGI_KONA(board);

    if (!s->fabric) {
        error_setg(errp, "sgi-kona requires the fabric its "
                  "packets travel");
        return false;
    }

    memory_region_init_io(&s->regs, OBJECT(s), &sgi_kona_ops, s,
                          "sgi-kona.regs",
                          SGI_KONA_APERTURE_SIZE);
    board->cpu_face = &s->regs;
    board->packet_face = &s->regs;

    /*
     * Claim the spare fabric proxy now this widget's own faces exist, so
     * sgi_machine_done() both brings the spare widget's link alive for
     * this board's outbound packets and registers those faces as the
     * widget's inbound destination. A fixture wiring "fabric" to a plain
     * sink is a no-op here.
     */
    sgi_xtalk_crossbow_link_claim_fabric(s->fabric, board->cpu_face,
                                         board->packet_face);
    return true;
}

/*
 * The one-wire bus is a qdev child bus of this board (DEVICE(s) doubles as
 * DEVICE(board): SGIKonaState has no separate widget device the way
 * xio-baseio.c's Bridge does), so it must not exist yet when build_widget()
 * returns -- the base class's own realize still has to re-parent this
 * board under its module, and that tears down every child bus a device
 * owns as a side effect. finish_realize() runs after that move, so the bus
 * this creates is never subject to it.
 *
 * Increment 2's own console and textport command port join it here too:
 * qemu_graphic_console_create() registers this device's QOM path with the
 * UI layer, and that path is exactly what the re-parent above changes, so
 * creating it any earlier risks registering the console under a path the
 * board no longer has once realize finishes. The textport command port
 * itself has no such constraint (it is a plain struct, not tied to QOM
 * identity), but is initialized alongside for one increment-2 setup site.
 */
static bool sgi_kona_finish_realize(SGIXioBoardState *board, Error **errp)
{
    SGIKonaState *s = SGI_KONA(board);

    s->onewire_bus = onewire_create_bus(DEVICE(s), "onewire");
    if (!sgi_kona_onewire_attach_part(
            s, "onewire-geometry", &sgi_kona_geometry_record,
            SGI_KONA_GEOMETRY_SERIAL, errp)) {
        return false;
    }
    if (!sgi_kona_onewire_attach_part(
            s, "onewire-ktown", &sgi_kona_ktown_record,
            SGI_KONA_KTOWN_SERIAL, errp)) {
        return false;
    }

    sgi_command_port_init(&s->textport, &sgi_kona_textport_ops, s,
                          SGI_KONA_PORT_TEXTPORT, sgi_kona_textport_selectors,
                          sgi_kona_textport_num_selectors);
    s->textport_expect_sync = true;

    s->framebuffer = g_new0(uint32_t, SGI_KONA_FB_WIDTH * SGI_KONA_FB_HEIGHT);
    s->con = qemu_graphic_console_create(DEVICE(s), 0, &sgi_kona_display_ops,
                                         s);
    sgi_kona_display_ensure_surface(s);
    return true;
}

/*
 * Host-only qtest access to native pixel storage. Unlike sgi-mgras.c's
 * own front/display_buffer split, KONA's display surface is created
 * directly over s->framebuffer (see sgi_kona_display_ensure_surface()),
 * so one property reflects both model and scanout state; no separate
 * x-test-display-pixel is needed.
 */
static void sgi_kona_test_get_pixel(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < SGI_KONA_FB_WIDTH &&
        s->test_pixel_y < SGI_KONA_FB_HEIGHT) {
        value = s->framebuffer[s->test_pixel_y * SGI_KONA_FB_WIDTH +
                               s->test_pixel_x];
    }
    visit_type_uint32(v, name, &value, errp);
}

static void sgi_kona_instance_init(Object *obj)
{
    SGIKonaState *s = SGI_KONA(obj);

    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-pixel", "uint32",
                        sgi_kona_test_get_pixel, NULL, NULL, NULL);
}

/*
 * Increment 2's own host-only buffers (framebuffer, an in-flight bitmap
 * payload) and console have no unrealize hook to release them from:
 * sgi-xtalk-board fixes dc->unrealize to sgi_xio_board_unrealize(), which
 * only removes a board from the pending-attach list (see hw/mips/sgi/
 * xio-board.c), and SGIXioBoardClass declares no finish_unrealize
 * counterpart to finish_realize(). instance_finalize runs unconditionally
 * on object destruction regardless of that realize/unrealize contract, so
 * it is the right place instead -- self-contained here, with no change to
 * the shared xio-board base class. Every field is guarded for the unset
 * case (a board that never realized, or whose realize failed before
 * reaching finish_realize()).
 */
static void sgi_kona_instance_finalize(Object *obj)
{
    SGIKonaState *s = SGI_KONA(obj);

    if (s->con) {
        qemu_graphic_console_close(s->con);
    }
    g_free(s->framebuffer);
    g_free(s->bitmap_payload);
}

/*
 * The plain stored fields a guest write can change: widget_control through
 * SGI_KONA_WIDGET_CONTROL, onewire_reg through one bit slot's worth of
 * SGI_KONA_ONEWIRE transaction state. manufacturer/part/revision/
 * probe_value are qdev properties fixed at construction, not guest state,
 * so reset leaves them alone -- matching how sgi-bridge.c's own
 * sgi_bridge_reset_core() recomputes its identification register from
 * properties rather than clearing them.
 */
static void sgi_kona_reset_hold(Object *obj, ResetType type)
{
    SGIKonaState *s = SGI_KONA(obj);

    s->widget_control = 0;
    s->onewire_reg = 0;
}

static const Property sgi_kona_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIKonaState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("manufacturer", SGIKonaState, manufacturer,
                       0x02aa),
    DEFINE_PROP_UINT32("part", SGIKonaState, part, 0xc102),
    DEFINE_PROP_UINT32("revision", SGIKonaState, revision, 0),
    /*
     * Selects the PROM's part-based classification path (bits 2:0 == 0);
     * see the header comment on SGI_KONA_PROBE.
     */
    DEFINE_PROP_UINT32("probe-value", SGIKonaState, probe_value, 0),
};

static const VMStateDescription vmstate_sgi_kona = {
    .name = TYPE_SGI_KONA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(widget_control, SGIKonaState),
        VMSTATE_UINT32(onewire_reg, SGIKonaState),

        /* Increment 2: register-window plain storage and the mailbox queue. */
        VMSTATE_UINT32(low_window_write, SGIKonaState),
        VMSTATE_UINT32(reset_arg, SGIKonaState),
        VMSTATE_UINT32(int_cause, SGIKonaState),
        VMSTATE_UINT32_ARRAY(mailbox_queue, SGIKonaState,
                             SGI_KONA_MAILBOX_MAX_WORDS),
        VMSTATE_UINT32(mailbox_len, SGIKonaState),
        VMSTATE_UINT32(mailbox_pos, SGIKonaState),
        VMSTATE_UINT32(reset_strobe, SGIKonaState),
        VMSTATE_UINT32(eeprom_enable, SGIKonaState),
        VMSTATE_UINT32(eeprom_addr, SGIKonaState),

        /*
         * Increment 2: the textport command port's migrated fields, per
         * include/hw/misc/sgi-command-port.h's own doc comment ("the
         * owner's VMStateDescription lists these"). ops/opaque/port_id/
         * selectors/num_selectors are not listed: sgi_kona_finish_realize()
         * sets them fresh on every realize, including the one an
         * identical-build migration replays before this state loads.
         */
        VMSTATE_UINT32(textport.kick, SGIKonaState),
        VMSTATE_BOOL(textport.framing, SGIKonaState),
        VMSTATE_UINT32(textport.selector, SGIKonaState),
        VMSTATE_UINT32(textport.argc_expected, SGIKonaState),
        VMSTATE_UINT32(textport.argc_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(textport.args, SGIKonaState,
                             SGI_COMMAND_PORT_MAX_ARGS),

        /* Increment 2: the textport's own sync/bitmap framing state. */
        VMSTATE_BOOL(textport_expect_sync, SGIKonaState),
        VMSTATE_UINT32(draw_color, SGIKonaState),
        VMSTATE_UINT32(bitmap_state, SGIKonaState),
        VMSTATE_UINT32_ARRAY(bitmap_header, SGIKonaState,
                             SGI_KONA_BITMAP_HEADER_WORDS),
        VMSTATE_UINT32(bitmap_header_have, SGIKonaState),
        VMSTATE_UINT32(bitmap_width, SGIKonaState),
        VMSTATE_UINT32(bitmap_height, SGIKonaState),
        VMSTATE_UINT32(bitmap_payload_have, SGIKonaState),
        VMSTATE_UINT32(bitmap_payload_expected, SGIKonaState),
        /*
         * A migration mid-blit must carry the in-flight payload itself,
         * not just its counters: matches hw/misc/sgi-mgras.c's own
         * ge[]/front/back precedent of migrating bulk transfer buffers,
         * not only their lengths. bitmap_payload_expected (already listed
         * above) sizes the allocation on load; outside a transfer it is
         * 0 and this reads/writes as an empty array regardless of the
         * pointer's NULL/non-NULL state.
         */
        VMSTATE_VARRAY_UINT32_ALLOC(bitmap_payload, SGIKonaState,
                                   bitmap_payload_expected, 0,
                                   vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_kona_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI KONA: InfiniteReality graphics pipe";
    dc->vmsd = &vmstate_sgi_kona;
    /*
     * Ordinarily fitted by the machine at a free XIO slot, but also
     * `-device`-creatable: a late board's own "fabric" can resolve
     * against the spare fabric proxy a Crossbow machine builder holds
     * back for it, the same outbound-only attach path xio-baseio.c's own
     * class_init documents.
     */
    dc->user_creatable = true;
    device_class_set_props(dc, sgi_kona_properties);
    bc->build_widget = sgi_kona_build_widget;
    bc->finish_realize = sgi_kona_finish_realize;
    rc->phases.hold = sgi_kona_reset_hold;
}

static const TypeInfo sgi_kona_type = {
    .name = TYPE_SGI_KONA,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIKonaState),
    .instance_init = sgi_kona_instance_init,
    .instance_finalize = sgi_kona_instance_finalize,
    .class_init = sgi_kona_class_init,
};

static void sgi_kona_register_types(void)
{
    type_register_static(&sgi_kona_type);
}

type_init(sgi_kona_register_types)
