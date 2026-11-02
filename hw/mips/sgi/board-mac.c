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
#include "hw/core/boards.h"
#include "qemu/option.h"
#include "system/blockdev.h"
#include "hw/mips/sgi/sgi-board-mac.h"
#include "hw/mips/sgi/sgi-board-identity.h"

static bool sgi_board_mac_unicast(const uint8_t mac[6])
{
    return memcmp(mac, "\0\0\0\0\0\0", 6) && !(mac[0] & 1);
}

/*
 * The key the default identity hashes: the canonical path of the disk on
 * SCSI bus 0, unit 0, which is where a legacy -drive if=scsi lands before
 * the machine is built, or the machine type name when there is none. A
 * disk attached later with -device is not visible yet, so it does not
 * count. The file= text is hashed as given when it is not a local path.
 */
static char *sgi_board_identity_key(void)
{
    DriveInfo *dinfo = drive_get(IF_SCSI, 0, 0);
    const char *file = dinfo && dinfo->opts ?
                       qemu_opt_get(dinfo->opts, "file") : NULL;

    if (file && file[0]) {
        char *path = g_canonicalize_filename(file, NULL);
#ifndef _WIN32
        char *real = realpath(path, NULL);

        if (real) {
            g_free(path);
            path = g_strdup(real);
            free(real);
        }
#endif
        return path;
    }
    return g_strdup(current_machine ?
                    MACHINE_GET_CLASS(current_machine)->name : "sgi");
}

/* The default identity, worked out once: the disk and machine do not change. */
static const SGIBoardIdentity *sgi_board_identity(void)
{
    static bool done;
    static SGIBoardIdentity identity;

    if (!done) {
        g_autofree char *key = sgi_board_identity_key();

        sgi_board_identity_from_key(key, &identity);
        done = true;
    }
    return &identity;
}

char *sgi_board_mac(const char *base, unsigned int offset, Error **errp)
{
    g_autofree char *fallback = NULL;
    uint8_t mac[6];
    uint64_t value = 0;

    if (!base) {
        memcpy(mac, sgi_board_identity()->mac, sizeof(mac));
        fallback = g_strdup_printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
                                   mac[1], mac[2], mac[3], mac[4], mac[5]);
        base = fallback;
    }
    if (net_parse_macaddr(mac, base) < 0 ||
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
