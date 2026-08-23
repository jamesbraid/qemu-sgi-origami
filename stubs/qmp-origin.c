/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"

SgiOriginState *qmp_x_query_origin_state(bool has_event_limit,
                                         uint32_t event_limit,
                                         Error **errp)
{
    error_setg(errp, "SGI Origin state is not supported on this target");
    return NULL;
}
