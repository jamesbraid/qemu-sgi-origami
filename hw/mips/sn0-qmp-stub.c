/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Origin query on a MIPS target built without the SN0 machine.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc-mips.h"

SgiOriginState *qmp_x_query_origin_state(bool has_event_limit,
                                         uint32_t event_limit,
                                         Error **errp)
{
    error_setg(errp, "SGI Origin state is not supported on this machine");
    return NULL;
}
