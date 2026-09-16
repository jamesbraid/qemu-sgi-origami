/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The abstract SGI Scalable Node family machine (QOM "sgi-sn-machine"):
 * every product's machine class parents this one, which carries the
 * family's machine properties once (topology, debug-leds, junk-uart,
 * elsc-dip-switches) and the builder that reads a format-2
 * SGIModelTopology (sn-model.h) to construct one. Origin 200 is the
 * first product on this base; see docs/plans/sn-machine-model.md,
 * "QEMU", for what else joins later.
 */

#ifndef HW_MIPS_SGI_SN_MACHINE_H
#define HW_MIPS_SGI_SN_MACHINE_H

#include "hw/core/boards.h"
#include "hw/mips/sgi/sn-model.h"

#define TYPE_SGI_SN_MACHINE MACHINE_TYPE_NAME("sgi-sn")

typedef struct SGIMachineState SGIMachineState;
typedef struct SGIMachineClass SGIMachineClass;

DECLARE_OBJ_CHECKERS(SGIMachineState, SGIMachineClass,
                     SGI_MACHINE, TYPE_SGI_SN_MACHINE)

struct SGIMachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/
    /* The product name topology= and the harness key off ("origin200"). */
    const char *product;
    /* The topology name topology= selects when the user names none. */
    const char *default_topology;
};

/*
 * Exposed for the builder's own fixture (tests/qtest/sn-builder-test.c):
 * validate a topology's shape the way sgi_machine_init does, before any
 * device exists. True and *errp untouched on success.
 */
bool sgi_machine_validate_topology(const SGIModelTopology *topology,
                                   Error **errp);

#endif /* HW_MIPS_SGI_SN_MACHINE_H */
