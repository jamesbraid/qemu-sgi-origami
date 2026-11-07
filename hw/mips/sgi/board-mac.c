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
#include "system/block-backend.h"
#include "hw/mips/sgi/sgi-board-mac.h"
#include "hw/mips/sgi/sgi-board-identity.h"

static bool sgi_board_mac_unicast(const uint8_t mac[6])
{
    return memcmp(mac, "\0\0\0\0\0\0", 6) && !(mac[0] & 1);
}

/*
 * The key the default identity hashes, in order of preference: the
 * machine's identity-key property, which a launcher sets to something
 * unique per guest such as the host path of its disk; the canonical path of
 * the first drive with a file, found by the legacy drive table, which is
 * where a -drive lands before the machine is built; or the machine type
 * name. A disk attached later with -device or -blockdev is not visible yet,
 * so it does not count, and a guest started that way shares the machine
 * name's identity unless it sets identity-key. The file= text is hashed as
 * given when it is not a local path.
 */
static char *sgi_board_explicit_key;
static bool sgi_board_identity_done;
static bool sgi_board_identity_seeded;

static char *sgi_board_identity_key(void)
{
    DriveInfo *dinfo = drive_get(IF_SCSI, 0, 0);
    const char *file = dinfo && dinfo->opts ?
                       qemu_opt_get(dinfo->opts, "file") : NULL;

    if (sgi_board_explicit_key) {
        sgi_board_identity_seeded = true;
        return g_strdup(sgi_board_explicit_key);
    }
    if (!file) {
        for (BlockBackend *blk = blk_next(NULL); blk; blk = blk_next(blk)) {
            dinfo = blk_legacy_dinfo(blk);
            /*
             * Only a disk counts. if=none and pflash drives back parts such
             * as the IO9 timekeeper and the firmware flash, whose files are
             * not the guest's identity.
             */
            file = dinfo && dinfo->opts &&
                   (dinfo->type == IF_SCSI || dinfo->type == IF_IDE) ?
                   qemu_opt_get(dinfo->opts, "file") : NULL;
            if (file && file[0]) {
                break;
            }
            file = NULL;
        }
    }
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
        sgi_board_identity_seeded = true;
        return path;
    }
    return g_strdup(current_machine ?
                    MACHINE_GET_CLASS(current_machine)->name : "sgi");
}

/* The default identity, worked out once: the disk and machine do not change. */
static const SGIBoardIdentity *sgi_board_identity(void)
{
    static SGIBoardIdentity identity;

    if (!sgi_board_identity_done) {
        g_autofree char *key = sgi_board_identity_key();

        sgi_board_identity_from_key(key, &identity);
        sgi_board_identity_done = true;
    }
    return &identity;
}

bool sgi_board_identity_is_seeded(void)
{
    sgi_board_identity();
    return sgi_board_identity_seeded;
}

char *sgi_board_default_module_serial(unsigned int module)
{
    char text[SGI_BOARD_MODULE_SERIAL_SIZE];

    sgi_board_module_serial(sgi_board_identity(), module, text);
    return g_strdup(text);
}

static void sgi_board_identity_key_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    g_autofree char *text = g_strdup(sgi_board_explicit_key ?
                                     sgi_board_explicit_key : "");

    visit_type_str(v, name, &text, errp);
}

static void sgi_board_identity_key_set(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    char *value;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    if (sgi_board_identity_done) {
        error_setg(errp, "%s cannot change once the identity is derived",
                   name);
        g_free(value);
        return;
    }
    g_free(sgi_board_explicit_key);
    sgi_board_explicit_key = value[0] ? value : NULL;
    if (!sgi_board_explicit_key) {
        g_free(value);
    }
}

char *sgi_board_default_text_serial(unsigned int node)
{
    char text[SGI_BOARD_TEXT_SERIAL_SIZE];

    sgi_board_text_serial(sgi_board_identity(), node, text);
    return g_strdup(text);
}

char *sgi_board_default_system_serial(void)
{
    char text[SGI_BOARD_SYSTEM_SERIAL_SIZE];

    sgi_board_system_serial(sgi_board_identity(), text);
    return g_strdup(text);
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
    object_class_property_add(oc, "identity-key", "str",
                              sgi_board_identity_key_get,
                              sgi_board_identity_key_set, NULL, NULL);
    object_class_property_set_description(oc, "identity-key",
        "Text the default station address and serial numbers are derived "
        "from, such as the host path of the system disk, so that guests "
        "differ (default the system disk path or machine name)");
}
