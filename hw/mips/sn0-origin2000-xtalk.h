/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_XTALK_H
#define HW_MIPS_SN0_ORIGIN2000_XTALK_H

#include "qapi/qapi-types-common.h"
#include "sn0-model.h"

typedef struct SN0MachineState SN0MachineState;

bool sn0_origin2000_xtalk_validate(const SN0ModelComposition *model,
                                  Error **errp);
bool sn0_origin2000_xtalk_prepare(SN0MachineState *machine, Error **errp);
bool sn0_origin2000_xtalk_attach(SN0MachineState *machine, Error **errp);
void sn0_origin2000_xtalk_destroy(SN0MachineState *machine);
bool sn0_origin2000_xtalk_test_proxies_disabled(SN0MachineState *machine);

/*
 * Fault injection for the fixture machines. Defined only when the assembly
 * is built with CONFIG_SN0_XTALK_ASSEMBLY_TEST, so anything outside those
 * fixtures that calls one of these fails to link rather than reaching into
 * a shipped machine.
 */

/* Force the board to refuse realization, reaching attach's unwind. */
void sn0_origin2000_xtalk_set_test_board_part(SN0MachineState *machine,
                                              const char *part);

/* Force one Crossbow to refuse realization; -1 for none. */
void sn0_origin2000_xtalk_set_test_crossbow_failure(SN0MachineState *machine,
                                                    int index);

#endif
