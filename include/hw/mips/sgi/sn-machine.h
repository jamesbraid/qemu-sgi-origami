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
    /*
     * The boot flash's firmware configuration block carries these three
     * fields per product (real IP27 PROM layout): a POD/inventory
     * machine-type code and the flash chip's read/write wait-state
     * counts. Not carried in the format-2 descriptor -- every measured
     * product shares one processor catalogue and one Hub, but each
     * ships its own firmware identity and flash timing.
     */
    uint32_t firmware_machine_type;
    uint32_t flash_read_cycles;
    uint32_t flash_write_cycles;
    /*
     * A fixture's own descriptor, bypassing the name lookup in
     * sgi_model_entries[] entirely. Set only by a qtest machine type that
     * needs the real builder over a topology no product ships (a
     * non-default console node, say); every shipping product leaves this
     * NULL and resolves topology= against the generated table instead.
     */
    const SGIModelTopology *test_topology;
};

/*
 * Exposed for the builder's own fixture (tests/qtest/sn-builder-test.c):
 * validate a topology's shape the way sgi_machine_init does, before any
 * device exists. True and *errp untouched on success.
 */
bool sgi_machine_validate_topology(const SGIModelTopology *topology,
                                   Error **errp);

/*
 * Exposed for the same fixture: the real builder, for a qtest-only
 * machine class that sets test_topology and wants to run it, gated
 * behind its own property the way sn-builder-fault-test.c's validator
 * is gated behind "fault" -- inert for qom-test and test-hmp's blanket,
 * property-free sweep of every registered machine type.
 */
void sgi_machine_init(MachineState *machine);

#endif /* HW_MIPS_SGI_SN_MACHINE_H */
