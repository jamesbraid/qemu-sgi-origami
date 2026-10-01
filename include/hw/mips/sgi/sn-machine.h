/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The abstract SGI Scalable Node family machine (QOM "sgi-sn-machine"): every
 * product's machine class parents this one, which carries the family's
 * machine properties once (topology, debug-leds, junk-uart,
 * elsc-dip-switches) and the builder that reads an SGIModelTopology from the
 * compiled catalogue (qapi/sgi-sn.json, hw/mips/sgi/sn-catalogue.c) to
 * construct one. Origin 200 is the first product on this base.
 */

#ifndef HW_MIPS_SGI_SN_MACHINE_H
#define HW_MIPS_SGI_SN_MACHINE_H

#include "hw/core/boards.h"
#include "hw/mips/sgi/sn-catalogue.h"

#define TYPE_SGI_SN_MACHINE MACHINE_TYPE_NAME("sgi-sn")

typedef struct SGIMachineState SGIMachineState;
typedef struct SGIMachineClass SGIMachineClass;

DECLARE_OBJ_CHECKERS(SGIMachineState, SGIMachineClass,
                     SGI_MACHINE, TYPE_SGI_SN_MACHINE)

struct SGIMachineClass {
    /* < private > */
    MachineClass parent_class;

    /* < public > */
    /* The product name topology= and the harness key off ("origin200"). */
    const char *product;
    /* The topology name topology= selects when the user names none. */
    const char *default_topology;
    /*
     * The boot flash's firmware configuration block carries these three
     * fields per product (real IP27 PROM layout): a POD/inventory
     * machine-type code and the flash chip's read/write wait-state counts.
     * Not carried in the format-2 descriptor: every measured product shares
     * one processor catalogue and one Hub, but each ships its firmware
     * identity and flash timing.
     */
    uint32_t firmware_machine_type;
    uint32_t flash_read_cycles;
    uint32_t flash_write_cycles;
    /*
     * The SN0 node board's bank wiring and sizes (sn0.h): Origin 200's IP29
     * or Origin 2000's IP27. NULL, as a fixture leaves it, is IP29.
     */
    const struct SN0ModelMemory *node_memory;
    /*
     * A fixture's descriptor, bypassing the name lookup in the compiled
     * catalogue entirely. Set only by a qtest machine type that needs the
     * real builder over a topology no product ships (a non-default console
     * node, say); every shipping product leaves this NULL and resolves
     * topology= against the compiled catalogue instead.
     */
    const SGIModelTopology *test_topology;
};

/*
 * Exposed for the builder's fixture (tests/qtest/sn-builder-test.c): the real
 * builder, for a qtest-only machine class that sets test_topology and only
 * calls this when its property turns it on. It stays inert for qom-test and
 * test-hmp's blanket, property-free instantiation of every registered machine
 * type.
 */
void sgi_machine_init(MachineState *machine);

/*
 * Exposed for each product class file (sn0.c's origin200/origin2000, sn1.c's
 * origin300): default_cpus, max_cpus and default_ram_size for
 * mc->default_cpus/max_cpus/default_ram_size, derived from the same topology
 * the class names.
 */
unsigned int sgi_machine_topology_default_cpus(const SGIModelTopology *t);
unsigned int sgi_machine_product_max_cpus(const char *product);
uint64_t sgi_machine_topology_default_ram(const SGIModelTopology *t);

/*
 * Copies the machine's chassis-eeprom/board-eeprom/spd-eeprom identity-record
 * file lists onto an L1 device before it is realized. Takes the L1 explicitly
 * because it lives on the node (sgi-sn1-node), while the file lists it reads
 * are machine properties. With several Bedrock nodes each list splits evenly,
 * node_index taking its share in order, since each chassis's L1 reads only
 * its chassis's parts; a list that does not divide evenly is refused.
 */
bool sgi_machine_set_l1_identity_records(MachineState *machine,
                                         DeviceState *l1,
                                         unsigned int node_index,
                                         Error **errp);

/*
 * The -serial lines a Bedrock node takes, a block of this many per node in
 * node order: its L1, then its IO-8's IOC3 port A, port B, and the two record
 * lines. Node 0's block is the one-chassis machine's lines 0 to 4.
 */
#define SGI_SN1_SERIAL_LINES_PER_NODE 5

static inline unsigned int sgi_sn1_serial_line(unsigned int node_index,
                                               unsigned int line)
{
    return node_index * SGI_SN1_SERIAL_LINES_PER_NODE + line;
}

#endif /* HW_MIPS_SGI_SN_MACHINE_H */
