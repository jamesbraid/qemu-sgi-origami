/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI D-brick: the Origin 3000 and Onyx 3000 Fibre Channel JBOD enclosure.
 *
 * Twelve bays of dual-ported Fibre Channel disks on two loop resiliency
 * circuit modules, cabled to a host adapter in an I-brick or P-brick. The
 * brick has no L1 and no XIO or NUMAlink connection; its only controller
 * is the operator panel's enclosure-services microcontroller (SGI Origin
 * 3000 Series Owner's Guide, 007-4240-002, chapter 8; Onyx 3000 Series
 * Graphics System Owner's Guide, 007-4264-002, printed page 128).
 *
 * Behind a QLogic ISP2200's private loop nothing of the enclosure but its
 * disks is visible to the host, so this device builds nothing: it names
 * which scsi-hd disk sits in which bay and refuses a population the
 * guides rule out. Each disk keeps its own loop ID and names on the
 * adapter's bus. The contract, its sources and its model choices are in
 * docs/system/mips/sgi/hardware/dbrick.rst.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/scsi.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_SGI_DBRICK "sgi-dbrick"
OBJECT_DECLARE_SIMPLE_TYPE(SGIDBrickState, SGI_DBRICK)

/* Four columns left to right by three rows top to bottom. */
#define DBRICK_COLUMNS  4
#define DBRICK_ROWS     3
#define DBRICK_BAYS     (DBRICK_COLUMNS * DBRICK_ROWS)
#define DBRICK_BAY(column, row) (((row) - 1) * DBRICK_COLUMNS + (column) - 1)

/* The adapter whose loop the bays are on; its type is private to it. */
#define DBRICK_LOOP_ADAPTER "qlogic-isp2200"

struct SGIDBrickState {
    DeviceState parent_obj;

    SCSIDevice *bay[DBRICK_BAYS];
};

static char *dbrick_bay_name(unsigned int bay)
{
    return g_strdup_printf("%u/%u", bay % DBRICK_COLUMNS + 1,
                           bay / DBRICK_COLUMNS + 1);
}

/* The bay's disk is the only device at its loop ID: one disk, one port. */
static bool dbrick_disk_owns_its_port(SCSIDevice *disk, const char *bay,
                                      Error **errp)
{
    BusState *bus = qdev_get_parent_bus(DEVICE(disk));
    BusChild *kid;

    WITH_RCU_READ_LOCK_GUARD() {
        QTAILQ_FOREACH_RCU(kid, &bus->children, sibling) {
            SCSIDevice *other = SCSI_DEVICE(kid->child);

            if (other != disk && other->id == disk->id) {
                error_setg(errp, "sgi-dbrick bay %s: loop ID %d also holds "
                           "%s at LUN %d; a bay's disk is the only device "
                           "on its loop port", bay, disk->id,
                           object_get_typename(OBJECT(other)), other->lun);
                return false;
            }
        }
    }
    return true;
}

static void sgi_dbrick_realize(DeviceState *dev, Error **errp)
{
    SGIDBrickState *s = SGI_DBRICK(dev);
    BusState *loop = NULL;
    unsigned int loop_bay = 0;
    unsigned int i, j;

    /*
     * The panel firmware needs both bottom-corner disks "to serve as
     * conduits for information from the D-brick to the ESI/ops panel
     * module" (007-4240-002 printed pages 159 and 166), so a fitted brick
     * always has them.
     */
    if (!s->bay[DBRICK_BAY(1, 3)] || !s->bay[DBRICK_BAY(4, 3)]) {
        error_setg(errp, "sgi-dbrick needs disks in bays 1/3 and 4/3, "
                   "which the enclosure's operator panel requires");
        return;
    }
    for (i = 0; i < DBRICK_BAYS; i++) {
        SCSIDevice *disk = s->bay[i];
        g_autofree char *bay = dbrick_bay_name(i);
        BusState *bus;

        if (!disk) {
            continue;
        }
        if (!object_dynamic_cast(OBJECT(disk), "scsi-hd")) {
            error_setg(errp, "sgi-dbrick bay %s holds a %s; a bay takes a "
                       "Fibre Channel disk, a scsi-hd", bay,
                       object_get_typename(OBJECT(disk)));
            return;
        }
        if (!qdev_is_realized(DEVICE(disk))) {
            error_setg(errp, "sgi-dbrick bay %s: the disk is not plugged in "
                       "yet; create it before the enclosure", bay);
            return;
        }
        for (j = 0; j < i; j++) {
            if (s->bay[j] == disk) {
                g_autofree char *first = dbrick_bay_name(j);

                error_setg(errp, "sgi-dbrick bays %s and %s hold the same "
                           "disk", first, bay);
                return;
            }
        }
        bus = qdev_get_parent_bus(DEVICE(disk));
        if (!object_dynamic_cast(OBJECT(bus->parent), DBRICK_LOOP_ADAPTER)) {
            error_setg(errp, "sgi-dbrick bay %s: the disk is on %s, not on "
                       "a QLogic ISP2200 loop", bay, bus->name);
            return;
        }
        /*
         * A supported adapter is single-port, so it reaches one loop
         * resiliency module of the brick (007-4311-002 printed page 51).
         */
        if (loop && bus != loop) {
            g_autofree char *first = dbrick_bay_name(loop_bay);

            error_setg(errp, "sgi-dbrick bays %s and %s are on different "
                       "loops, %s and %s; this model attaches one loop",
                       first, bay, loop->name, bus->name);
            return;
        }
        loop = bus;
        loop_bay = i;
        if (disk->lun != 0) {
            error_setg(errp, "sgi-dbrick bay %s: the disk is LUN %d; a bay's "
                       "disk is LUN 0 of its own loop port", bay, disk->lun);
            return;
        }
        if (!dbrick_disk_owns_its_port(disk, bay, errp)) {
            return;
        }
    }
}

#define DEFINE_DBRICK_BAY(column, row) \
    DEFINE_PROP_LINK("bay" #column "-" #row, SGIDBrickState, \
                     bay[DBRICK_BAY(column, row)], TYPE_SCSI_DEVICE, \
                     SCSIDevice *)

static const Property sgi_dbrick_properties[] = {
    DEFINE_DBRICK_BAY(1, 1), DEFINE_DBRICK_BAY(2, 1),
    DEFINE_DBRICK_BAY(3, 1), DEFINE_DBRICK_BAY(4, 1),
    DEFINE_DBRICK_BAY(1, 2), DEFINE_DBRICK_BAY(2, 2),
    DEFINE_DBRICK_BAY(3, 2), DEFINE_DBRICK_BAY(4, 2),
    DEFINE_DBRICK_BAY(1, 3), DEFINE_DBRICK_BAY(2, 3),
    DEFINE_DBRICK_BAY(3, 3), DEFINE_DBRICK_BAY(4, 3),
};

static void sgi_dbrick_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "SGI D-brick Fibre Channel disk enclosure";
    dc->realize = sgi_dbrick_realize;
    /* Bays are fixed when the enclosure is built; hot swap is not modelled. */
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    device_class_set_props(dc, sgi_dbrick_properties);
}

static const TypeInfo sgi_dbrick_info = {
    .name = TYPE_SGI_DBRICK,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIDBrickState),
    .class_init = sgi_dbrick_class_init,
};

static void sgi_dbrick_register_types(void)
{
    type_register_static(&sgi_dbrick_info);
}

type_init(sgi_dbrick_register_types)
