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
 * the real builder, for a qtest-only machine class that sets
 * test_topology and only calls this when its own property turns it on --
 * inert for qom-test and test-hmp's blanket, property-free instantiation
 * of every registered machine type.
 */
void sgi_machine_init(MachineState *machine);

/*
 * Exposed for each product class file (sn0.c's origin200/origin2000,
 * sn1.c's origin300): default_cpus, max_cpus and default_ram_size for
 * mc->default_cpus/max_cpus/default_ram_size, derived from the same
 * topology the class names.
 */
unsigned int sgi_machine_topology_default_cpus(const SGIModelTopology *t);
unsigned int sgi_machine_product_max_cpus(const char *product);
uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *t);

/*
 * Copies the machine's own chassis-eeprom/board-eeprom/spd-eeprom
 * identity-record file lists onto an L1 device before it is realized.
 * Takes the L1 explicitly because it lives on the node (sgi-sn1-node),
 * while the file lists it reads are machine properties -- one identity
 * per machine, one L1 per node.
 */
void sgi_machine_set_l1_identity_records(MachineState *machine,
                                         DeviceState *l1);

#endif /* HW_MIPS_SGI_SN_MACHINE_H */
