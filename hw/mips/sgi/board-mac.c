/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The identity machine properties every SGI machine shares: emu-id, serial,
 * hostid and the read-only hostids, the "mac" property with its parsing, and
 * the per-node and per-port station addresses and serial texts derived from
 * the resolved inputs (see sgi-board-identity.h).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "hw/core/qdev.h"
#include "net/util.h"
#include "hw/core/boards.h"
#include "hw/mips/sgi/sgi-board-mac.h"
#include "hw/mips/sgi/sgi-board-identity.h"

static bool sgi_board_mac_unicast(const uint8_t mac[6])
{
    return memcmp(mac, "\0\0\0\0\0\0", 6) && !(mac[0] & 1);
}

char *sgi_board_default_text_serial(unsigned int node)
{
    char text[SGI_BOARD_TEXT_SERIAL_SIZE];

    sgi_board_text_serial(node, text);
    return g_strdup(text);
}

char *sgi_board_default_system_serial(const SGIBoardId *id)
{
    return g_strdup(id->text);
}

char *sgi_board_default_module_serial(const SGIBoardId *id,
                                      unsigned int module, Error **errp)
{
    char text[SGI_BOARD_MODULE_SERIAL_SIZE];

    if (!id->kind) {
        error_setg(errp, "the machine's identity inputs are not resolved");
        return NULL;
    }
    if (!sgi_board_id_module_text(id, module, text)) {
        error_setg(errp, "module serials number at most %d modules, and "
                   "module %u does not fit", SGI_BOARD_MODULE_COUNT, module);
        return NULL;
    }
    return g_strdup(text);
}

bool sgi_board_identity_resolve(SGIBoardIdentityInputs *in,
                                SGIBoardFamily family, const char *mac,
                                Error **errp)
{
    char message[SGI_BOARD_ERROR_SIZE];
    uint8_t address[6];

    if (mac && net_parse_macaddr(address, mac) < 0) {
        error_setg(errp, "mac must be a nonzero unicast address");
        return false;
    }
    if (!sgi_board_id_resolve(family, SGI_BOARD_KIND, in->emu_id, in->serial,
                              in->hostid, mac ? address : NULL, &in->id,
                              message)) {
        error_setg(errp, "%s", message);
        return false;
    }
    in->resolved = true;
    return true;
}

char *sgi_board_mac(const char *base, const SGIBoardId *id,
                    unsigned int offset, Error **errp)
{
    g_autofree char *fallback = NULL;
    uint8_t mac[6];
    uint64_t value = 0;

    if (!base && !id->kind) {
        error_setg(errp, "the machine's identity inputs are not resolved");
        return NULL;
    }
    if (!base && id->address_from_id) {
        /* An id the user gave on an address family: its address, plus n. */
        sgi_board_mac_from_hostid(id->id, mac);
        fallback = g_strdup_printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
                                   mac[1], mac[2], mac[3], mac[4], mac[5]);
        base = fallback;
    } else if (!base) {
        /*
         * The default leaves the low nibble for the node or port number, so
         * adding it below cannot carry into the emu-id. Past 16 it would.
         */
        if (!sgi_board_default_mac(id->kind, id->emu_id, 0, mac)) {
            g_assert_not_reached();
        }
        if (offset >= SGI_BOARD_SUB_INDEX_COUNT) {
            error_setg(errp, "the default station address numbers at most "
                       "%d nodes or ports, and node %u does not fit; set "
                       "mac=", SGI_BOARD_SUB_INDEX_COUNT, offset);
            return NULL;
        }
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
    /*
     * An address family's id is the address's last four bytes and starts 69,
     * so a node's address must stay in the 08:00:69 block: the sum may not
     * carry out of the last three bytes.
     */
    if (id->family == SGI_BOARD_FAMILY_ADDRESS &&
        (value & 0xffffff) + offset > 0xffffff) {
        error_setg(errp, "node %u of mac=%02x:%02x:%02x:%02x:%02x:%02x would "
                   "leave the 08:00:69 block, which this machine's station "
                   "address must stay in; use mac=08:00:69:xx:xx:xx with "
                   "room for the nodes, or a lower hostid", offset, mac[0],
                   mac[1], mac[2], mac[3], mac[4], mac[5]);
        return NULL;
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
    /* A text family, which has no carry rule: only the address is checked. */
    static const SGIBoardId no_id = {
        .family = SGI_BOARD_FAMILY_TEXT, .kind = SGI_BOARD_KIND,
    };
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
    /* The family rules, which need the resolved id, come at machine start. */
    check = sgi_board_mac(value, &no_id, 0, errp);
    if (!check) {
        g_free(value);
        return;
    }
    g_free(*slot);
    *slot = value;
}

/* What one class's identity properties need from its machine. */
typedef struct SGIBoardIdentityClass {
    size_t inputs_offset;
    SGIBoardHostidsFn hostids;
} SGIBoardIdentityClass;

static SGIBoardIdentityInputs *sgi_board_inputs(Object *obj, void *opaque)
{
    SGIBoardIdentityClass *c = opaque;

    return (SGIBoardIdentityInputs *)((char *)obj + c->inputs_offset);
}

static bool sgi_board_inputs_locked(Object *obj, const char *name,
                                    Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "%s cannot change once the machine is built", name);
        return true;
    }
    return false;
}

static void sgi_board_emu_id_get_visit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint32_t value = sgi_board_inputs(obj, opaque)->emu_id;

    visit_type_uint32(v, name, &value, errp);
}

static void sgi_board_emu_id_set_visit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint32_t value;

    if (sgi_board_inputs_locked(obj, name, errp) ||
        !visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!sgi_board_emu_id_valid(value)) {
        error_setg(errp, "%s must be 0 to %d, not %u", name,
                   SGI_BOARD_EMU_ID_MAX, value);
        return;
    }
    sgi_board_inputs(obj, opaque)->emu_id = value;
}

static void sgi_board_serial_get_visit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SGIBoardIdentityInputs *in = sgi_board_inputs(obj, opaque);
    g_autofree char *text = g_strdup(in->serial ? in->serial : "");

    visit_type_str(v, name, &text, errp);
}

static void sgi_board_serial_set_visit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SGIBoardIdentityInputs *in = sgi_board_inputs(obj, opaque);
    char *value;

    if (sgi_board_inputs_locked(obj, name, errp) ||
        !visit_type_str(v, name, &value, errp)) {
        return;
    }
    if (value[0] && !sgi_board_serial_valid(value)) {
        error_setg(errp, "serial '%s' must be 1 to %d printable characters, "
                   "none of ':' or ';'", value, SGI_BOARD_SERIAL_MAX_LENGTH);
        g_free(value);
        return;
    }
    g_free(in->serial);
    in->serial = value;
}

/* The first id of a built machine, or NULL with @errp set. */
static bool sgi_board_first_hostid(Object *obj, SGIBoardIdentityClass *c,
                                   const char *name, GArray *ids,
                                   Error **errp)
{
    if (!phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "%s is known once the machine is built", name);
        return false;
    }
    if (!sgi_board_inputs(obj, c)->resolved) {
        /*
         * A machine type that never built its identity, such as a test
         * fixture whose init is gated, has no id to report.
         */
        error_setg(errp, "%s is unknown: this machine has not resolved its "
                   "identity inputs", name);
        return false;
    }
    if (!c->hostids(obj, ids, errp)) {
        return false;
    }
    g_assert(ids->len);
    return true;
}

static void sgi_board_hostid_get_visit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SGIBoardIdentityInputs *in = sgi_board_inputs(obj, opaque);
    g_autoptr(GArray) ids = g_array_new(false, false, sizeof(uint32_t));
    g_autofree char *text = NULL;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        if (!sgi_board_first_hostid(obj, opaque, name, ids, errp)) {
            return;
        }
        text = g_strdup_printf("%08x", g_array_index(ids, uint32_t, 0));
    } else {
        text = g_strdup(in->hostid ? in->hostid : "");
    }
    visit_type_str(v, name, &text, errp);
}

static void sgi_board_hostid_set_visit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SGIBoardIdentityInputs *in = sgi_board_inputs(obj, opaque);
    char *value;

    /* The family decides which forms are valid, when the machine resolves. */
    if (sgi_board_inputs_locked(obj, name, errp) ||
        !visit_type_str(v, name, &value, errp)) {
        return;
    }
    g_free(in->hostid);
    in->hostid = value;
}

static void sgi_board_hostids_get_visit(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    g_autoptr(GArray) ids = g_array_new(false, false, sizeof(uint32_t));
    g_autoptr(GString) text = g_string_new("");

    if (!sgi_board_first_hostid(obj, opaque, name, ids, errp)) {
        return;
    }
    for (guint i = 0; i < ids->len; i++) {
        g_string_append_printf(text, "%s%08x", i ? " " : "",
                               g_array_index(ids, uint32_t, i));
    }
    visit_type_str(v, name, &text->str, errp);
}

void sgi_board_identity_class_add_properties(ObjectClass *oc,
                                             size_t inputs_offset,
                                             SGIBoardHostidsFn hostids)
{
    SGIBoardIdentityClass *c = g_new0(SGIBoardIdentityClass, 1);

    c->inputs_offset = inputs_offset;
    c->hostids = hostids;
    object_class_property_add(oc, "emu-id", "uint32",
                              sgi_board_emu_id_get_visit,
                              sgi_board_emu_id_set_visit, NULL, c);
    object_class_property_set_description(oc, "emu-id",
        "Mints the machine's system id in the emulator layout, 0 to 4095 "
        "(default 0, a plainly generic id); serial, hostid and mac win over "
        "it");
    if (!hostids) {
        return;
    }
    object_class_property_add(oc, "serial", "str",
                              sgi_board_serial_get_visit,
                              sgi_board_serial_set_visit, NULL, c);
    object_class_property_set_description(oc, "serial",
        "The machine's system serial as a real unit has it written: 1 to 10 "
        "printable characters, none of ':' or ';', for example M2100887 on "
        "the IP35 machines; sets the system id by taking the low nibble of "
        "each character. The Origin 2000 and Onyx2 take a module serial that "
        "must start with K, for example K0017299, padded on the left with K "
        "to 10 characters");
    object_class_property_add(oc, "hostid", "str",
                              sgi_board_hostid_get_visit,
                              sgi_board_hostid_set_visit, NULL, c);
    object_class_property_set_description(oc, "hostid",
        "The system id as 8 hex digits; on the Octane, Origin 200 and Fuel "
        "also 6 digits, the low 24 bits with the leading 69 implied. Read "
        "back, the id of the first node or module once the machine is built");
    object_class_property_add(oc, "hostids", "str",
                              sgi_board_hostids_get_visit, NULL, NULL, c);
    object_class_property_set_description(oc, "hostids",
        "Every system id the guest can print, as 8 hex digits each, separated "
        "by spaces, in node, module or chassis order (read-only)");
}

void sgi_board_mac_class_add_property(ObjectClass *oc, size_t offset,
                                      const char *description)
{
    object_class_property_add(oc, "mac", "str", sgi_board_mac_get_visit,
                              sgi_board_mac_set_visit, NULL,
                              (void *)(uintptr_t)offset);
    object_class_property_set_description(oc, "mac", description);
}
