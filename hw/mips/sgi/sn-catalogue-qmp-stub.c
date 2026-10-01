/* SPDX-License-Identifier: BSD-3-Clause */
/* Catalogue query for MIPS builds without SN0/SN1 machines. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-sgi-sn.h"

SGIModelCatalogue *qmp_query_sgi_sn_catalogue(Error **errp)
{
    error_setg(errp,
               "the SGI Scalable Node catalogue is not supported on this machine");
    return NULL;
}
