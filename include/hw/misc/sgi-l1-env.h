/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 console's "env" report for the Chimera interface boards.
 */
#ifndef HW_MISC_SGI_L1_ENV_H
#define HW_MISC_SGI_L1_ENV_H

#include "qemu/typedefs.h"

/**
 * sgi_l1_chimera_env: the text the "env" command prints
 * @type: the brick type character: '7' for the 2U interface board, '9' for
 *        the workstation interface board, 'Q' for the rack-mounted
 *        workstation
 * @node_board: the node board's product name, such as "IP59_4CPU"
 * @powered: whether the node has power; an unpowered node reports every
 *           row as waiting for power
 *
 * The node board's name chooses between the IP53 and the IP59 tables.
 *
 * Returns: a newly allocated string with CR LF line ends, or NULL when
 * @type or @node_board names no modelled table.
 */
char *sgi_l1_chimera_env(char type, const char *node_board, bool powered);

#endif
