/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI KONA: the InfiniteReality graphics pipe, increment 1, identity only.
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
 * NIC register does. Every other offset reads zero and accepts any write,
 * logged as unimplemented. No FIFO, mailbox or interrupt exists yet.
 *
 * Both one-wire parts this model seeds -- one geometry-board record, one
 * Ktown record -- are synthetic beyond their `Part:` field. No one-wire
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
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2502.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/misc/sgi-kona.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

/* Widget-header identification: revision:part:manufacturer:1, generic. */
#define WIDGET_ID_REVISION_SHIFT     28
#define WIDGET_ID_PART_SHIFT         12
#define WIDGET_ID_MANUFACTURER_SHIFT 1
#define WIDGET_ID_ALWAYS_ONE         1

/* The DS2502's real family code (Dallas 1-Wire family list), not synthetic. */
#define SGI_KONA_ONEWIRE_FAMILY 0x09

/* Two placeholder identities; no reviewed capture assigns real serials. */
#define SGI_KONA_GEOMETRY_SERIAL 0x000000000001ULL
#define SGI_KONA_KTOWN_SERIAL    0x000000000002ULL

#define SGI_KONA_ONEWIRE_PAGE_SIZE 32
#define SGI_KONA_ONEWIRE_PAGES     4
#define SGI_KONA_ONEWIRE_ROM_SIZE \
    (SGI_KONA_ONEWIRE_PAGE_SIZE * SGI_KONA_ONEWIRE_PAGES)
#define SGI_KONA_ONEWIRE_CONTENT_PER_PAGE \
    (SGI_KONA_ONEWIRE_PAGE_SIZE - 2)

/*
 * Eight synthetic fields each, ";Field:value;" scanned rather than
 * positional (unlike sgi-bridge.c's own single-field record, nothing here
 * needs a canonical fixed offset). Part: carries the reviewed rack
 * capture's real board part number; every other field -- Name, Serial,
 * Revision, Group, Capability, Variety, Laser -- is a synthetic
 * placeholder pending a real one-wire capture.
 */
static const char sgi_kona_geometry_record[] =
    ";Revision:01;Capability:0000;Variety:GEOM;Laser:NONE;"
    "Part:030-1398-001;Name:GEOM-BD;Serial:00000001;Group:GFX;";
static const char sgi_kona_ktown_record[] =
    ";Revision:01;Capability:0000;Variety:KTWN;Laser:NONE;"
    "Part:030-1067-001;Name:KTOWN-BD;Serial:00000002;Group:GFX;";

/*
 * Pack a "Field:value;" record into a DS2502's four pages, two bytes short
 * per page for a CRC-16 -- content convention, the way sgi-bridge.c's own
 * one-wire board-identity record reserves its last two bytes per page, not
 * device behaviour and not something this model's own reader checks.
 */
static void sgi_kona_onewire_build_rom(
    uint8_t rom[SGI_KONA_ONEWIRE_ROM_SIZE], const char *record)
{
    size_t len = strlen(record);
    unsigned int page;

    assert(len <= SGI_KONA_ONEWIRE_CONTENT_PER_PAGE *
                  SGI_KONA_ONEWIRE_PAGES);
    memset(rom, 0, SGI_KONA_ONEWIRE_ROM_SIZE);

    for (page = 0; page < SGI_KONA_ONEWIRE_PAGES; page++) {
        uint8_t *at = &rom[page * SGI_KONA_ONEWIRE_PAGE_SIZE];
        size_t offset = page * SGI_KONA_ONEWIRE_CONTENT_PER_PAGE;
        uint16_t crc;

        if (len > offset) {
            size_t chunk = MIN(len - offset,
                (size_t)SGI_KONA_ONEWIRE_CONTENT_PER_PAGE);
            memcpy(at, record + offset, chunk);
        }
        crc = ~onewire_crc16(0, at, SGI_KONA_ONEWIRE_CONTENT_PER_PAGE);
        at[SGI_KONA_ONEWIRE_CONTENT_PER_PAGE] = crc & 0xff;
        at[SGI_KONA_ONEWIRE_CONTENT_PER_PAGE + 1] = crc >> 8;
    }
}

/* Realize one DS2502 identity part onto the board's own one-wire bus. */
static bool sgi_kona_onewire_attach_part(
    SGIKonaState *s, const char *child_name, const char *record,
    uint64_t serial, Error **errp)
{
    uint8_t rom[SGI_KONA_ONEWIRE_ROM_SIZE];
    DeviceState *part;

    sgi_kona_onewire_build_rom(rom, record);
    part = qdev_new(TYPE_ONEWIRE_DS2502);
    object_property_add_child(OBJECT(s), child_name, OBJECT(part));
    qdev_prop_set_uint8(part, "family", SGI_KONA_ONEWIRE_FAMILY);
    qdev_prop_set_uint64(part, "serial", serial);
    ds2502_set_image(part, rom, sizeof(rom));
    return qdev_realize_and_unref(part, BUS(s->onewire_bus), errp);
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

    s->onewire_bus = onewire_create_bus(DEVICE(s), "onewire");
    if (!sgi_kona_onewire_attach_part(
            s, "onewire-geometry", sgi_kona_geometry_record,
            SGI_KONA_GEOMETRY_SERIAL, errp)) {
        return false;
    }
    if (!sgi_kona_onewire_attach_part(
            s, "onewire-ktown", sgi_kona_ktown_record,
            SGI_KONA_KTOWN_SERIAL, errp)) {
        return false;
    }

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
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_kona_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "SGI KONA: InfiniteReality graphics pipe (identity only)";
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
}

static const TypeInfo sgi_kona_type = {
    .name = TYPE_SGI_KONA,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIKonaState),
    .class_init = sgi_kona_class_init,
};

static void sgi_kona_register_types(void)
{
    type_register_static(&sgi_kona_type);
}

type_init(sgi_kona_register_types)
