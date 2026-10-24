/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics pipes against the machine catalogue; see the header.
 *
 * Every message here carries the catalogue's own words: a slot's
 * description and products, and a refusal's reason verbatim, so QEMU and
 * a launcher reading query-sgi-machines say the same thing.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"
#include "system/system.h"
#include "hw/mips/sgi/sgi-graphics.h"

/* The device each family's pipes are, as the catalogue names them. */
static const char *const sgi_graphics_devices[] = {
    [SGI_MODEL_GRAPHICS_FAMILY_KIND_MGRAS] = "sgi-mgras",
    [SGI_MODEL_GRAPHICS_FAMILY_KIND_ODYSSEY] = "sgi-odyssey",
    [SGI_MODEL_GRAPHICS_FAMILY_KIND_KONA] = "sgi-kona",
    [SGI_MODEL_GRAPHICS_FAMILY_KIND_PSITECH] = "psitech-rad4",
};

bool sgi_graphics_pipe_family(DeviceState *dev,
                              SGIModelGraphicsFamilyKind *family)
{
    const char *type = object_get_typename(OBJECT(dev));

    for (int i = 0; i < SGI_MODEL_GRAPHICS_FAMILY_KIND__MAX; i++) {
        if (g_str_equal(type, sgi_graphics_devices[i])) {
            *family = i;
            return true;
        }
    }
    return false;
}

static const char *sgi_graphics_vga_name(int type)
{
    static const char *const names[VGA_TYPE_MAX] = {
        [VGA_STD] = "std", [VGA_CIRRUS] = "cirrus", [VGA_VMWARE] = "vmware",
        [VGA_XENFB] = "xenfb", [VGA_QXL] = "qxl", [VGA_TCX] = "tcx",
        [VGA_CG3] = "cg3", [VGA_VIRTIO] = "virtio",
    };

    return type >= 0 && type < VGA_TYPE_MAX && names[type] ? names[type] :
           "another adapter";
}

/*
 * The adapter type system/vl.c selects when -vga is absent and the machine
 * names no default display: Cirrus where the binary has it, else the
 * standard VGA, else none. A machine sees only the resulting
 * vga_interface_type, so -vga set to that same type cannot be told from no
 * -vga at all and is taken as the default.
 */
static int sgi_graphics_vga_absent(void)
{
    if (module_object_class_by_name("cirrus-vga") ||
        module_object_class_by_name("isa-cirrus-vga")) {
        return VGA_CIRRUS;
    }
    if (module_object_class_by_name("VGA") ||
        module_object_class_by_name("isa-vga")) {
        return VGA_STD;
    }
    return VGA_NONE;
}

bool sgi_graphics_vga_none(const char *machine, bool *none, Error **errp)
{
    *none = false;
    if (vga_interface_type == VGA_NONE) {
        *none = true;
        return true;
    }
    /*
     * VGA_DEVICE is -device of a PC display adapter with no -vga: a PCI
     * card the machine does not judge, beside its own default pipe.
     */
    if (vga_interface_type == VGA_DEVICE ||
        vga_interface_type == sgi_graphics_vga_absent()) {
        return true;
    }
    error_setg(errp, "%s: -vga %s is not a display an SGI machine takes; use "
               "-vga none for no graphics, or a graphics -device such as "
               "sgi-mgras", machine, sgi_graphics_vga_name(vga_interface_type));
    return false;
}

const SGIModelGraphicsSlot *sgi_graphics_default_slot(
    const SGIModelGraphics *g)
{
    for (SGIModelGraphicsSlotList *s = g ? g->slots : NULL; s; s = s->next) {
        if (s->value->q_default) {
            return s->value;
        }
    }
    return NULL;
}

static const char *sgi_graphics_placement(const SGIModelGraphicsSlot *slot,
                                          const char *name)
{
    for (SGIModelGraphicsPropertyList *p = slot->placement; p; p = p->next) {
        if (g_str_equal(p->value->name, name)) {
            return p->value->value;
        }
    }
    return NULL;
}

const SGIModelGraphicsSlot *sgi_graphics_pipe_slot(
    const SGIModelGraphics *g, DeviceState *dev,
    SGIModelGraphicsFamilyKind family)
{
    SGIModelGraphicsSlotList *s;

    if (family == SGI_MODEL_GRAPHICS_FAMILY_KIND_PSITECH) {
        PCIDevice *pci = PCI_DEVICE(dev);
        const char *bus = dev->parent_bus ? dev->parent_bus->name : NULL;

        if (pci->devfn < 0 || !bus) {
            return NULL;
        }
        for (s = g ? g->slots : NULL; s; s = s->next) {
            const char *want_bus = sgi_graphics_placement(s->value, "bus");
            const char *want_addr = sgi_graphics_placement(s->value, "addr");
            unsigned int addr;

            if (want_bus && want_addr && g_str_equal(want_bus, bus) &&
                !qemu_strtoui(want_addr, NULL, 0, &addr) &&
                PCI_DEVFN(addr, 0) == pci->devfn) {
                return s->value;
            }
        }
        return NULL;
    }
    {
        g_autofree char *name = object_property_get_str(OBJECT(dev), "slot",
                                                        &error_abort);

        for (s = g && name ? g->slots : NULL; s; s = s->next) {
            const char *want = sgi_graphics_placement(s->value, "slot");

            if (want && g_str_equal(want, name)) {
                return s->value;
            }
        }
    }
    return NULL;
}

/* "slot=io4" or "bus=pci,addr=5": how -device names @slot. */
static char *sgi_graphics_placement_text(const SGIModelGraphicsSlot *slot)
{
    GString *text = g_string_new(NULL);

    for (SGIModelGraphicsPropertyList *p = slot->placement; p; p = p->next) {
        g_string_append_printf(text, "%s%s=%s", text->len ? "," : "",
                               p->value->name, p->value->value);
    }
    return g_string_free(text, false);
}

/* "io2 (slot=io2), ...": each slot with the placement that reaches it. */
static char *sgi_graphics_slot_placements(const SGIModelGraphics *g)
{
    GString *list = g_string_new(NULL);

    for (SGIModelGraphicsSlotList *s = g->slots; s; s = s->next) {
        g_autofree char *placement = sgi_graphics_placement_text(s->value);

        g_string_append_printf(list, "%s%s (%s)", list->len ? ", " : "",
                               s->value->name, placement);
    }
    return g_string_free(list, false);
}

/*
 * A setting's value as -device spells it and the catalogue lists it, or
 * NULL when the device leaves it unset (an empty string, or zero for a
 * number) or has no such setting.
 */
static char *sgi_graphics_get(DeviceState *dev, const char *name)
{
    ObjectProperty *prop = object_property_find(OBJECT(dev), name);
    uint64_t number;
    char *text;

    if (!prop) {
        return NULL;
    }
    if (g_str_equal(prop->type, "str")) {
        text = object_property_get_str(OBJECT(dev), name, &error_abort);
        if (!text || !*text) {
            g_free(text);
            return NULL;
        }
        return text;
    }
    number = object_property_get_uint(OBJECT(dev), name, &error_abort);
    return number ? g_strdup_printf("%" PRIu64, number) : NULL;
}

/*
 * The first of @slot's refusals that matches a pipe of @family and
 * @product with settings @values: its family matches, its product matches
 * if it names one, and every setting it lists has the value @values gives
 * it. @product NULL matches only a refusal that names none.
 */
static const SGIModelGraphicsRefusal *sgi_graphics_refused(
    const SGIModelGraphicsSlot *slot, SGIModelGraphicsFamilyKind family,
    const char *product, GHashTable *values)
{
    for (SGIModelGraphicsRefusalList *r = slot->refusals; r; r = r->next) {
        const SGIModelGraphicsRefusal *refusal = r->value;
        bool match = refusal->family == family;

        if (refusal->product) {
            match &= product && g_str_equal(refusal->product, product);
        }
        for (SGIModelGraphicsPropertyList *p = refusal->settings;
             match && p; p = p->next) {
            const char *value = g_hash_table_lookup(values, p->value->name);

            match = value && g_str_equal(value, p->value->value);
        }
        if (match) {
            return refusal;
        }
    }
    return NULL;
}

static const SGIModelGraphicsFamily *sgi_graphics_offer(
    const SGIModelGraphicsSlot *slot, SGIModelGraphicsFamilyKind family)
{
    for (SGIModelGraphicsFamilyList *f = slot->families; f; f = f->next) {
        if (f->value->family == family) {
            return f->value;
        }
    }
    return NULL;
}

static const SGIModelGraphicsProduct *sgi_graphics_product(
    const SGIModelGraphicsFamily *offer, const char *product)
{
    for (SGIModelGraphicsProductList *p = offer->products; p; p = p->next) {
        if (g_str_equal(p->value->product, product)) {
            return p->value;
        }
    }
    return NULL;
}

/* "si, si-t and esi" for @offer's products, or @setting's values. */
static char *sgi_graphics_choices(const SGIModelGraphicsFamily *offer,
                                  const SGIModelGraphicsSetting *setting)
{
    GString *list = g_string_new(NULL);

    if (offer) {
        for (SGIModelGraphicsProductList *p = offer->products; p;
             p = p->next) {
            g_string_append_printf(list, "%s%s", list->len ?
                                   (p->next ? ", " : " and ") : "",
                                   p->value->product);
        }
    } else {
        for (SGIModelGraphicsValueList *v = setting->values; v; v = v->next) {
            g_string_append_printf(list, "%s%s", list->len ?
                                   (v->next ? ", " : " and ") : "",
                                   v->value->value);
        }
    }
    return g_string_free(list, false);
}

static bool sgi_graphics_value_offered(const SGIModelGraphicsSetting *setting,
                                       const char *value)
{
    for (SGIModelGraphicsValueList *v = setting->values; v; v = v->next) {
        if (g_str_equal(v->value->value, value)) {
            return true;
        }
    }
    return false;
}

/* The settings -device gave @dev for @family, by name. */
static GHashTable *sgi_graphics_requested(DeviceState *dev,
                                          const SGIModelGraphicsSlot *slot,
                                          SGIModelGraphicsFamilyKind family)
{
    GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);

    /*
     * Every setting a refusal or offer of @family at this slot names; other
     * families' settings need not exist on @dev.
     */
    for (SGIModelGraphicsRefusalList *r = slot->refusals; r; r = r->next) {
        if (r->value->family != family) {
            continue;
        }
        for (SGIModelGraphicsPropertyList *p = r->value->settings; p;
             p = p->next) {
            char *value = sgi_graphics_get(dev, p->value->name);

            if (value) {
                g_hash_table_replace(values, g_strdup(p->value->name), value);
            }
        }
    }
    for (SGIModelGraphicsFamilyList *f = slot->families; f; f = f->next) {
        if (f->value->family != family) {
            continue;
        }
        for (SGIModelGraphicsProductList *p = f->value->products; p;
             p = p->next) {
            for (SGIModelGraphicsSettingList *s = p->value->settings; s;
                 s = s->next) {
                char *value = sgi_graphics_get(dev, s->value->name);

                if (value) {
                    g_hash_table_replace(values, g_strdup(s->value->name),
                                         value);
                }
            }
        }
    }
    return values;
}

bool sgi_graphics_check_pipe(const char *machine, const SGIModelTopology *t,
                             const SGIModelGraphics *g, uint32_t nodes,
                             DeviceState *dev,
                             SGIModelGraphicsFamilyKind family,
                             unsigned int pipes, const char *first,
                             Error **errp)
{
    const char *device = sgi_graphics_devices[family];
    const SGIModelGraphicsSlot *slot;
    const SGIModelGraphicsFamily *offer;
    const SGIModelGraphicsProduct *product;
    const SGIModelGraphicsRefusal *refusal;
    g_autoptr(GHashTable) values = NULL;
    g_autofree char *requested = NULL;
    g_autofree char *slots = NULL;
    const char *name;

    if (!g) {
        error_setg(errp, "%s: topology '%s' has no graphics slots, so it "
                   "takes no %s", machine, t->name, device);
        return false;
    }
    slots = sgi_graphics_slot_placements(g);
    slot = sgi_graphics_pipe_slot(g, dev, family);
    if (!slot) {
        if (family == SGI_MODEL_GRAPHICS_FAMILY_KIND_PSITECH) {
            PCIDevice *pci = PCI_DEVICE(dev);

            if (pci->devfn < 0) {
                error_setg(errp, "%s: %s needs bus= and addr= naming one of "
                           "the graphics slots of topology '%s': %s", machine,
                           device, t->name, slots);
            } else {
                error_setg(errp, "%s: topology '%s' has no graphics slot at "
                           "bus=%s,addr=%d; its graphics slots are %s",
                           machine, t->name,
                           dev->parent_bus ? dev->parent_bus->name : "",
                           PCI_SLOT(pci->devfn), slots);
            }
            return false;
        }
        name = object_property_get_str(OBJECT(dev), "slot", &error_abort);
        if (!name || !*name) {
            error_setg(errp, "%s: %s needs slot= naming one of the graphics "
                       "slots of topology '%s': %s", machine, device, t->name,
                       slots);
        } else {
            error_setg(errp, "%s: topology '%s' has no graphics slot %s; its "
                       "graphics slots are %s", machine, t->name, name,
                       slots);
        }
        g_free((char *)name);
        return false;
    }
    name = slot->name;

    if (slot->requires && slot->requires->has_min_nodes &&
        nodes < slot->requires->min_nodes) {
        error_setg(errp, "%s: graphics slot %s of topology '%s' exists from "
                   "nodes=%" PRIu32 ", and this machine fits %" PRIu32,
                   machine, name, t->name, slot->requires->min_nodes, nodes);
        return false;
    }
    if (pipes >= g->max_pipes) {
        error_setg(errp, "%s: topology '%s' builds at most %" PRIu32
                   " graphics pipe%s, and %s already holds one", machine,
                   t->name, g->max_pipes, g->max_pipes == 1 ? "" : "s",
                   first);
        return false;
    }

    values = sgi_graphics_requested(dev, slot, family);
    requested = sgi_graphics_get(dev, "product");
    offer = sgi_graphics_offer(slot, family);
    if (!offer) {
        GString *taken = g_string_new(NULL);

        for (SGIModelGraphicsFamilyList *f = slot->families; f; f = f->next) {
            g_string_append_printf(taken, "%s%s", taken->len ?
                                   (f->next ? ", " : " and ") : "",
                                   f->value->device);
        }
        refusal = sgi_graphics_refused(slot, family, requested, values);
        error_setg(errp, "%s: graphics slot %s of topology '%s' takes %s, "
                   "not %s%s%s", machine, name, t->name, taken->str, device,
                   refusal ? ": " : "", refusal ? refusal->reason : "");
        g_string_free(taken, true);
        return false;
    }

    /*
     * No product: the slot's default when it is this family's, else the
     * first product the slot offers the family.
     */
    if (!requested) {
        const SGIModelGraphicsPipe *def = slot->q_default;

        requested = g_strdup(def && def->family == family ? def->product :
                             offer->products->value->product);
        object_property_set_str(OBJECT(dev), "product", requested,
                                &error_abort);
    }
    product = sgi_graphics_product(offer, requested);
    if (!product) {
        g_autofree char *choices = sgi_graphics_choices(offer, NULL);

        refusal = sgi_graphics_refused(slot, family, requested, values);
        error_setg(errp, "%s: graphics slot %s of topology '%s' does not "
                   "offer %s product=%s; it offers %s%s%s", machine, name,
                   t->name, device, requested, choices, refusal ? ": " : "",
                   refusal ? refusal->reason : "");
        return false;
    }

    /* A setting the device has that the product leaves no choice in here. */
    {
        GHashTableIter iter;
        gpointer key, value;

        g_hash_table_iter_init(&iter, values);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            bool listed = false;

            for (SGIModelGraphicsSettingList *s = product->settings; s;
                 s = s->next) {
                listed |= g_str_equal(s->value->name, key);
            }
            if (listed) {
                continue;
            }
            refusal = sgi_graphics_refused(slot, family, requested, values);
            error_setg(errp, "%s: graphics slot %s of topology '%s' offers "
                       "%s product=%s no %s choice, so %s=%s does not fit%s%s",
                       machine, name, t->name, device, requested,
                       (char *)key, (char *)key, (char *)value,
                       refusal ? ": " : "", refusal ? refusal->reason : "");
            return false;
        }
    }

    for (SGIModelGraphicsSettingList *s = product->settings; s; s = s->next) {
        const SGIModelGraphicsSetting *setting = s->value;
        const char *value = g_hash_table_lookup(values, setting->name);

        if (!value) {
            const char *fill = setting->q_default;
            const SGIModelGraphicsPipe *def = slot->q_default;

            /* The default pipe's own values, where this is its product. */
            if (def && def->family == family &&
                g_str_equal(def->product, requested)) {
                for (SGIModelGraphicsPropertyList *p = def->settings; p;
                     p = p->next) {
                    if (g_str_equal(p->value->name, setting->name)) {
                        fill = p->value->value;
                    }
                }
            }
            if (!object_property_parse(OBJECT(dev), setting->name, fill,
                                       errp)) {
                return false;
            }
            g_hash_table_replace(values, g_strdup(setting->name),
                                 g_strdup(fill));
            continue;
        }
        if (!sgi_graphics_value_offered(setting, value)) {
            g_autofree char *choices = sgi_graphics_choices(NULL, setting);

            refusal = sgi_graphics_refused(slot, family, requested, values);
            error_setg(errp, "%s: graphics slot %s of topology '%s' does not "
                       "offer %s product=%s with %s=%s; it offers %s%s%s",
                       machine, name, t->name, device, requested,
                       setting->name, value, choices, refusal ? ": " : "",
                       refusal ? refusal->reason : "");
            return false;
        }
    }

    refusal = sgi_graphics_refused(slot, family, requested, values);
    if (refusal) {
        error_setg(errp, "%s: graphics slot %s of topology '%s' does not "
                   "take this %s: %s", machine, name, t->name, device,
                   refusal->reason);
        return false;
    }
    return true;
}

DeviceState *sgi_graphics_new_default(const SGIModelGraphicsSlot *slot)
{
    const SGIModelGraphicsPipe *def = slot->q_default;
    DeviceState *dev = qdev_new(sgi_graphics_devices[def->family]);

    for (SGIModelGraphicsPropertyList *p = slot->placement; p; p = p->next) {
        object_property_parse(OBJECT(dev), p->value->name, p->value->value,
                              &error_abort);
    }
    object_property_set_str(OBJECT(dev), "product", def->product,
                            &error_abort);
    for (SGIModelGraphicsPropertyList *p = def->settings; p; p = p->next) {
        object_property_parse(OBJECT(dev), p->value->name, p->value->value,
                              &error_abort);
    }
    return dev;
}
