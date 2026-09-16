/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_XTALK_H
#define HW_MIPS_SN0_ORIGIN2000_XTALK_H

#include "qapi/qapi-types-common.h"
#include "qom/object.h"
#include "hw/mips/sgi/sn-model.h"
#include "sn0.h"

#define TYPE_SN0_ORIGIN2000_XTALK "sn0-origin2000-xtalk"
OBJECT_DECLARE_SIMPLE_TYPE(SN0Origin2000XtalkState, SN0_ORIGIN2000_XTALK)

/*
 * Build the switches and the per-node outgoing proxies over a format-2
 * topology's crossbar records (crossbars[], xtalk_attachments[],
 * io_boards[]). nodes[] is the machine's node array, node-index aligned
 * with t->nodes[]: format 2 carries no separate physical-slot indirection,
 * so the two arrays already agree index for index. owner parents the
 * returned object; every node named in t must exist in nodes[], unrealized.
 */
SN0Origin2000XtalkState *sn0_origin2000_xtalk_prepare(
    Object *owner, SN0NodeState **nodes, unsigned int node_count,
    const SGIModelTopology *t, Error **errp);
/*
 * attach() and destroy() take only the handle prepare() returned: it keeps
 * the nodes[] and topology pointers prepare() was given, since QOM's own
 * teardown calls this object's unparent hook with no extra argument to
 * pass. Call attach() only once nodes[] is fully realized.
 */
bool sn0_origin2000_xtalk_attach(SN0Origin2000XtalkState *s, Error **errp);
void sn0_origin2000_xtalk_destroy(SN0Origin2000XtalkState *s);
bool sn0_origin2000_xtalk_test_proxies_disabled(SN0Origin2000XtalkState *s);

/*
 * Fault injection for the fixture machines. Defined only when the assembly
 * is built with CONFIG_SN0_XTALK_ASSEMBLY_TEST, so anything outside those
 * fixtures that calls one of these fails to link rather than reaching into
 * a shipped machine.
 */

/* Force the board to refuse realization, reaching attach's unwind. */
void sn0_origin2000_xtalk_set_test_board_part(SN0Origin2000XtalkState *s,
                                              const char *part);

/* Force one Crossbow to refuse realization; -1 for none. */
void sn0_origin2000_xtalk_set_test_crossbow_failure(SN0Origin2000XtalkState *s,
                                                    int index);

/*
 * Force one base I/O PCI function to be refused a slot, leaving the board
 * part-built the way a function that fails inside its own realize does;
 * -1 for none.
 */
void sn0_origin2000_xtalk_set_test_function_failure(SN0Origin2000XtalkState *s,
                                                    int index);

#endif
