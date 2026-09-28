/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-ioc3-release-fixture: realize an IOC3 behind a Bridge and release it
 * again while the machine lives on, the way a builder drops a chip it has
 * already brought up when the rest of a board fails to build
 * (hw/mips/sgi/baseio.c, baseio_build_ioc3()).
 *
 * -M none refuses to hot-plug anything onto its main system bus, and the
 * Bridge's PCI bus is cold-plug only, so no QMP command can take an IOC3
 * away once it is realized. This fixture does it from its own realize()
 * instead: build a Bridge, realize an IOC3 fitted with the keyboard and
 * mouse controller on it, unparent the IOC3, then drain the RCU callbacks
 * that finish every bus removal, so any free of the chip's children has
 * happened before the qtest's first command rather than at some later,
 * racy moment.
 *
 * Nothing here is guest-visible; the qtest only needs the machine to still
 * be standing and the chip to be gone.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/rcu.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3_RELEASE_FIXTURE "sgi-ioc3-release-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3ReleaseFixtureState,
                           SGI_IOC3_RELEASE_FIXTURE)

/* The IP29 slot, though any free device number would do. */
#define RELEASE_FIXTURE_IOC3_DEVICE 2

struct SGIIOC3ReleaseFixtureState {
    SysBusDevice parent_obj;

    MemoryRegion sink;
};

static void ioc3_release_fixture_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3ReleaseFixtureState *s = SGI_IOC3_RELEASE_FIXTURE(dev);
    DeviceState *chip;
    PCIDevice *ioc3;

    memory_region_init(&s->sink, OBJECT(s), "sgi-ioc3-release-fixture.sink",
                       64 * KiB);

    chip = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(s), "chip", OBJECT(chip));
    qdev_prop_set_string(chip, "board-part", "030-1244-001");
    object_property_set_link(OBJECT(chip), "fabric", OBJECT(&s->sink),
                             &error_abort);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(chip), errp)) {
        return;
    }

    ioc3 = pci_new(PCI_DEVFN(RELEASE_FIXTURE_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(s), "ioc3", OBJECT(ioc3));
    qdev_prop_set_uint32(DEVICE(ioc3), "population",
                         SGI_IOC3_POPULATION_KEYBOARD_MOUSE);
    if (!pci_realize_and_unref(ioc3, PCI_HOST_BRIDGE(chip)->bus, errp)) {
        return;
    }

    object_unparent(OBJECT(ioc3));
    drain_call_rcu();
}

/* Test-only harness state, not guest-visible. */
static const VMStateDescription vmstate_ioc3_release_fixture = {
    .name = TYPE_SGI_IOC3_RELEASE_FIXTURE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void ioc3_release_fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "IOC3 release test fixture";
    dc->realize = ioc3_release_fixture_realize;
    dc->vmsd = &vmstate_ioc3_release_fixture;
    dc->user_creatable = true;
}

static const TypeInfo ioc3_release_fixture_types[] = {
    {
        .name = TYPE_SGI_IOC3_RELEASE_FIXTURE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIIOC3ReleaseFixtureState),
        .class_init = ioc3_release_fixture_class_init,
    },
};

DEFINE_TYPES(ioc3_release_fixture_types)
