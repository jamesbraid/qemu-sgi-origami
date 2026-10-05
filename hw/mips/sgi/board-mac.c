/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The "mac" machine property every SGI machine shares: its default, its
 * parsing and the per-board address it derives.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "hw/core/qdev.h"
#include "net/util.h"
#include "hw/mips/sgi/sgi-board-mac.h"

static bool sgi_board_mac_unicast(const uint8_t mac[6])
{
    return memcmp(mac, "\0\0\0\0\0\0", 6) && !(mac[0] & 1);
}

char *sgi_board_mac(const char *base, unsigned int offset, Error **errp)
{
    uint8_t mac[6];
    uint64_t value = 0;

    if (net_parse_macaddr(mac, base ? base : SGI_BOARD_DEFAULT_MAC) < 0 ||
        !sgi_board_mac_unicast(mac)) {
        error_setg(errp, "mac must be a nonzero unicast address");
        return NULL;
    }
    for (unsigned int i = 0; i < 6; i++) {
        value = value << 8 | mac[i];
    }
    value = (value + offset) & MAKE_64BIT_MASK(0, 48);
    for (unsigned int i = 0; i < 6; i++) {
        mac[i] = value >> (40 - 8 * i);
    }
    if (!sgi_board_mac_unicast(mac)) {
        error_setg(errp, "mac leaves node %u without a unicast address",
                   offset);
        return NULL;
    }
    return g_strdup_printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                           mac[2], mac[3], mac[4], mac[5]);
}

static char **sgi_board_mac_slot(Object *obj, void *opaque)
{
    return (char **)((char *)obj + (uintptr_t)opaque);
}


static void sgi_board_mac_get_visit(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    char *value = *sgi_board_mac_slot(obj, opaque);
    g_autofree char *text = g_strdup(value ? value : "");

    visit_type_str(v, name, &text, errp);
}

static void sgi_board_mac_set_visit(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    char **slot = sgi_board_mac_slot(obj, opaque);
    g_autofree char *check = NULL;
    char *value;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "%s cannot change once the machine is built", name);
        return;
    }
    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    check = sgi_board_mac(value, 0, errp);
    if (!check) {
        g_free(value);
        return;
    }
    g_free(*slot);
    *slot = value;
}

void sgi_board_mac_class_add_property(ObjectClass *oc, size_t offset,
                                      const char *description)
{
    object_class_property_add(oc, "mac", "str", sgi_board_mac_get_visit,
                              sgi_board_mac_set_visit, NULL,
                              (void *)(uintptr_t)offset);
    object_class_property_set_description(oc, "mac", description);
}
