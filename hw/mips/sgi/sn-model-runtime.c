/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check endpoint support before machine construction or view selection. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/sgi/sn-catalogue.h"

bool sgi_model_check_runtime_endpoints(const SGIModelTopology *t, Error **errp)
{
    const SGIModelModuleList *module;
    const SGIModelCableList *cable;
    bool has_peers = t->io_peers != NULL;

    for (module = t->modules; module; module = module->next) {
        has_peers |= module->value->io_peers != NULL;
    }
    for (cable = t->cables; cable; cable = cable->next) {
        const SGIModelLinkEndList *end;

        for (end = cable->value->ends; end; end = end->next) {
            has_peers |= end->value->kind == SGI_MODEL_LINK_END_KIND_IO_PEER;
        }
    }
    if (has_peers) {
        error_setg(errp, "topology '%s' names I/O-only network endpoints; "
                   "their runtime construction is not supported", t->name);
        return false;
    }
    return true;
}
