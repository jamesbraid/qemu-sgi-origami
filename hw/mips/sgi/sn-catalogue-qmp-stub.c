/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SGI Scalable Node catalogue query on a MIPS target built without
 * the SN0/SN1 machines.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-sgi-sn.h"

SGIModelCatalogue *qmp_query_sgi_sn_catalogue(Error **errp)
{
    error_setg(errp,
               "the SGI Scalable Node catalogue is not supported on this machine");
    return NULL;
}
