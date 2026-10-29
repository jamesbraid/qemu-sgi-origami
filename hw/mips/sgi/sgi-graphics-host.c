/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The rule between pipes that needs only the pipes already fitted, kept
 * apart from the catalogue checks in sgi-graphics.c so a unit test can
 * exercise it on a hand-built node map (tests/unit/test-sgi-graphics-host.c).
 *
 * On an Onyx2 a node controls at most one pipe (the Origin and Onyx2
 * Hardware Quick-reference Booklet, HMQ-380-C). On an Onyx 3000 each V12
 * needs a dedicated C-brick (007-4264-002 printed page 32); that each
 * G-brick pipe needs a C-brick of its own is inferred, not stated.
 * Which node controls a slot depends on how many nodes are fitted, so the
 * machine names it when the pipe plugs, not the catalogue.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/sgi/sgi-graphics.h"

bool sgi_graphics_check_host(const char *machine, GPtrArray *fitted,
                             const char *name, const char *host,
                             Error **errp)
{
    for (guint i = 0; host && i < fitted->len; i++) {
        const SGIGraphicsFitted *other = g_ptr_array_index(fitted, i);

        /* A second pipe in the same slot is the slot's to refuse. */
        if (g_str_equal(other->name, name)) {
            continue;
        }
        if (other->host && g_str_equal(other->host, host)) {
            error_setg(errp, "%s: graphics slot %s is controlled by node "
                       "%s, which already controls the pipe in %s, and a "
                       "node controls at most one pipe", machine, name, host,
                       other->name);
            return false;
        }
    }
    return true;
}
