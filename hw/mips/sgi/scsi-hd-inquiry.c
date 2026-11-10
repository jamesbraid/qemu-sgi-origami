/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * scsi-hd-inquiry: a scsi-hd whose standard INQUIRY data is configurable.
 *
 * Some software reads a disk's identity and serial number from the
 * vendor-specific area of the standard INQUIRY data (bytes 36 to 55)
 * rather than from the unit serial number VPD page, and scsi-hd has no
 * way to fill that area. This type is a scsi-hd, with the same VPD pages,
 * block sizes, READ CAPACITY and everything else, that adds:
 *
 * - inquiry-vendor-specific: the content of bytes 36 to 55;
 * - inquiry-peripheral-qualifier and inquiry-peripheral-type: byte 0;
 * - a space-padded product revision level (scsi-hd pads it with NULs).
 *
 * The vendor, product and ver properties it inherits already set bytes 8
 * to 35. They are checked against the fields' widths here, because scsi-hd
 * cuts a longer value short without a word.
 *
 * scsi-disk.c is left alone. scsi-disk.c does not export its state
 * structure or its INQUIRY code, so the type reaches the data through the
 * public request interface: its alloc_req hook gives an INQUIRY the type's
 * own copy of scsi-disk's emulated-command request operations, whose
 * send_command runs the original and then overwrites the fields above in
 * the finished buffer, before the host adapter is told there is data.
 * The settings live outside the instance, in a table keyed by the device,
 * because the instance structure is private to scsi-disk.c as well; the
 * same arrangement as hw/scsi/sgi-scsi-cd.c.
 *
 * This is a configuration feature, not a model of a particular drive. The
 * layout is SPC-3 section 6.4.2, the standard INQUIRY data format table
 * (see docs/system/mips/sgi/hardware/scsi-hd-inquiry.rst).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/qdev.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "qom/object.h"

#define TYPE_SCSI_HD_INQUIRY    "scsi-hd-inquiry"

/* SPC-3 section 6.4.2: the fields of the standard INQUIRY data. */
#define INQ_VENDOR_OFF          8
#define INQ_VENDOR_LEN          8
#define INQ_PRODUCT_OFF         16
#define INQ_PRODUCT_LEN         16
#define INQ_REVISION_OFF        32
#define INQ_REVISION_LEN        4
#define INQ_VENDOR_SPECIFIC_OFF 36
#define INQ_VENDOR_SPECIFIC_LEN 20

#define INQ_QUALIFIER_MAX       0x07
#define INQ_TYPE_MAX            0x1f

typedef struct InquirySettings {
    /* The property's text, as it was given. */
    char *vendor_specific_text;
    /* Bytes 36 to 55; vendor_specific_set says whether to write them. */
    uint8_t vendor_specific[INQ_VENDOR_SPECIFIC_LEN];
    bool vendor_specific_set;
    uint8_t qualifier;
    uint8_t type;
    /* Bytes 32 to 35, space padded, taken at realize. */
    uint8_t revision[INQ_REVISION_LEN];
} InquirySettings;

/*
 * Every scsi-hd-inquiry's settings. Commands to the SGI host adapters run
 * on the main loop thread under the BQL, as do the property setters and
 * instance creation and destruction, so the table needs no lock of its own.
 */
static GHashTable *inquiry_settings;

/* The parent class's hooks, which this class's replace. */
static void (*parent_realize)(SCSIDevice *dev, Error **errp);
static SCSIRequest *(*parent_alloc_req)(SCSIDevice *s, uint32_t tag,
                                        uint32_t lun, uint8_t *buf,
                                        void *hba_private);

/*
 * scsi-disk's request operations for INQUIRY, found on the first request
 * since scsi-disk.c keeps them private, and this class's copy of them.
 */
static const SCSIReqOps *stock_ops;
static SCSIReqOps inquiry_ops;

static void inquiry_settings_free(gpointer data)
{
    InquirySettings *s = data;

    g_free(s->vendor_specific_text);
    g_free(s);
}

static InquirySettings *inquiry_settings_of(const SCSIDevice *dev)
{
    return g_hash_table_lookup(inquiry_settings, dev);
}

/*
 * Parse the text of inquiry-vendor-specific into the 20 bytes. "hex:"
 * followed by an even number of hex digits gives raw bytes, padded with
 * NULs. Anything else is ASCII, left aligned and padded with spaces as SPC
 * pads its ASCII data fields. An empty string leaves the field to
 * scsi-hd's (zeros).
 */
static bool inquiry_parse_vendor_specific(const char *text,
                                          InquirySettings *s, Error **errp)
{
    uint8_t bytes[INQ_VENDOR_SPECIFIC_LEN];
    size_t len = strlen(text);

    if (g_str_has_prefix(text, "hex:")) {
        const char *digits = text + 4;
        size_t n = strlen(digits);

        if (n % 2 || n / 2 > sizeof(bytes)) {
            error_setg(errp, "inquiry-vendor-specific hex: takes an even "
                       "number of digits, at most %zu bytes",
                       sizeof(bytes));
            return false;
        }
        memset(bytes, 0, sizeof(bytes));
        for (size_t i = 0; i < n / 2; i++) {
            if (!g_ascii_isxdigit(digits[2 * i]) ||
                !g_ascii_isxdigit(digits[2 * i + 1])) {
                error_setg(errp, "inquiry-vendor-specific hex: \"%s\" is "
                           "not hexadecimal", digits);
                return false;
            }
            bytes[i] = g_ascii_xdigit_value(digits[2 * i]) << 4 |
                       g_ascii_xdigit_value(digits[2 * i + 1]);
        }
        s->vendor_specific_set = true;
    } else {
        if (len > sizeof(bytes)) {
            error_setg(errp, "inquiry-vendor-specific is %zu bytes, but "
                       "INQUIRY bytes 36 to 55 hold %zu", len,
                       sizeof(bytes));
            return false;
        }
        for (size_t i = 0; i < len; i++) {
            if (text[i] < 0x20 || text[i] > 0x7e) {
                error_setg(errp, "inquiry-vendor-specific text takes "
                           "printable ASCII (0x20 to 0x7e); use hex: for "
                           "other bytes");
                return false;
            }
        }
        memset(bytes, ' ', sizeof(bytes));
        memcpy(bytes, text, len);
        s->vendor_specific_set = len != 0;
    }
    memcpy(s->vendor_specific, bytes, sizeof(bytes));
    return true;
}

static bool inquiry_allow_set(Object *obj, const char *name, Error **errp)
{
    if (DEVICE(obj)->realized) {
        error_setg(errp, "Attempt to set property '%s' on active device",
                   name);
        return false;
    }
    return true;
}

static void inquiry_get_vendor_specific(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    InquirySettings *s = opaque;
    char *text = s->vendor_specific_text ? s->vendor_specific_text
                                         : (char *)"";

    visit_type_str(v, name, &text, errp);
}

static void inquiry_set_vendor_specific(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    InquirySettings *s = opaque;
    g_autofree char *text = NULL;

    if (!inquiry_allow_set(obj, name, errp) ||
        !visit_type_str(v, name, &text, errp)) {
        return;
    }
    if (!inquiry_parse_vendor_specific(text, s, errp)) {
        return;
    }
    g_free(s->vendor_specific_text);
    s->vendor_specific_text = g_steal_pointer(&text);
}

static void inquiry_get_u8(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    uint8_t *field = opaque;

    visit_type_uint8(v, name, field, errp);
}

static void inquiry_set_u8(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp, uint8_t max)
{
    uint8_t *field = opaque;
    uint8_t value;

    if (!inquiry_allow_set(obj, name, errp) ||
        !visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > max) {
        error_setg(errp, "%s is 0x%02x, but the field's largest value is "
                   "0x%02x", name, value, max);
        return;
    }
    *field = value;
}

static void inquiry_set_qualifier(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    inquiry_set_u8(obj, v, name, opaque, errp, INQ_QUALIFIER_MAX);
}

static void inquiry_set_type(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    inquiry_set_u8(obj, v, name, opaque, errp, INQ_TYPE_MAX);
}

static void inquiry_instance_init(Object *obj)
{
    InquirySettings *s = g_new0(InquirySettings, 1);
    ObjectProperty *prop;

    g_hash_table_insert(inquiry_settings, obj, s);

    prop = object_property_add(obj, "inquiry-vendor-specific", "str",
                               inquiry_get_vendor_specific,
                               inquiry_set_vendor_specific, NULL, s);
    object_property_set_description(obj, prop->name,
        "INQUIRY bytes 36 to 55: up to 20 ASCII characters, space padded, "
        "or hex: and up to 20 bytes as hex digits, NUL padded");
    prop = object_property_add(obj, "inquiry-peripheral-qualifier", "uint8",
                               inquiry_get_u8, inquiry_set_qualifier, NULL,
                               &s->qualifier);
    object_property_set_description(obj, prop->name,
        "INQUIRY byte 0 bits 7:5, the peripheral qualifier (default 0)");
    prop = object_property_add(obj, "inquiry-peripheral-type", "uint8",
                               inquiry_get_u8, inquiry_set_type, NULL,
                               &s->type);
    object_property_set_description(obj, prop->name,
        "INQUIRY byte 0 bits 4:0, the peripheral device type (default 0, "
        "direct access block device)");
}

static void inquiry_instance_finalize(Object *obj)
{
    g_hash_table_remove(inquiry_settings, obj);
}

/* Reject a value scsi-hd would silently cut to the field's width. */
static bool inquiry_check_width(DeviceState *dev, const char *prop,
                                size_t width, Error **errp)
{
    g_autofree char *value = object_property_get_str(OBJECT(dev), prop, errp);

    if (!value) {
        return false;
    }
    if (strlen(value) > width) {
        error_setg(errp, "%s \"%s\" is %zu bytes, but its INQUIRY field "
                   "holds %zu", prop, value, strlen(value), width);
        return false;
    }
    return true;
}

static void inquiry_realize(SCSIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    InquirySettings *s = inquiry_settings_of(dev);
    g_autofree char *ver = NULL;

    if (!inquiry_check_width(DEVICE(dev), "vendor", INQ_VENDOR_LEN, errp) ||
        !inquiry_check_width(DEVICE(dev), "product", INQ_PRODUCT_LEN, errp) ||
        !inquiry_check_width(DEVICE(dev), "ver", INQ_REVISION_LEN, errp)) {
        return;
    }
    parent_realize(dev, errp);
    if (*errp) {
        return;
    }
    /* scsi-hd supplies the default revision during its realize. */
    ver = object_property_get_str(OBJECT(dev), "ver", &error_abort);
    memset(s->revision, ' ', sizeof(s->revision));
    memcpy(s->revision, ver, MIN(strlen(ver), sizeof(s->revision)));
}

/*
 * Run scsi-disk's INQUIRY, then overwrite the configured fields. Only a
 * standard INQUIRY that succeeded and returned data is touched, and only
 * the bytes the transfer includes.
 */
static int32_t inquiry_send_command(SCSIRequest *req, uint8_t *buf)
{
    int32_t len = stock_ops->send_command(req, buf);
    InquirySettings *s = inquiry_settings_of(req->dev);
    uint8_t *data;

    if (len <= 0 || (buf[1] & 1) || buf[2] != 0 || !s) {
        return len;
    }
    data = scsi_req_get_buf(req);
    data[0] = s->qualifier << 5 | s->type;
    for (int i = 0; i < INQ_REVISION_LEN && INQ_REVISION_OFF + i < len; i++) {
        data[INQ_REVISION_OFF + i] = s->revision[i];
    }
    if (s->vendor_specific_set) {
        for (int i = 0; i < INQ_VENDOR_SPECIFIC_LEN &&
                        INQ_VENDOR_SPECIFIC_OFF + i < len; i++) {
            data[INQ_VENDOR_SPECIFIC_OFF + i] = s->vendor_specific[i];
        }
    }
    return len;
}

static SCSIRequest *inquiry_alloc_req(SCSIDevice *d, uint32_t tag,
                                      uint32_t lun, uint8_t *buf,
                                      void *hba_private)
{
    SCSIRequest *req = parent_alloc_req(d, tag, lun, buf, hba_private);

    if (buf[0] == INQUIRY) {
        if (!stock_ops) {
            stock_ops = req->ops;
            inquiry_ops = *stock_ops;
            inquiry_ops.send_command = inquiry_send_command;
        }
        /* One operations table serves INQUIRY, or the copy is wrong. */
        assert(req->ops == stock_ops);
        req->ops = &inquiry_ops;
    }
    return req;
}

static void inquiry_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    inquiry_settings = g_hash_table_new_full(NULL, NULL, NULL,
                                            inquiry_settings_free);
    parent_realize = sc->realize;
    sc->realize = inquiry_realize;
    parent_alloc_req = sc->alloc_req;
    sc->alloc_req = inquiry_alloc_req;
    dc->desc = "virtual SCSI disk with configurable INQUIRY data";
}

static const TypeInfo scsi_hd_inquiry_info = {
    .name              = TYPE_SCSI_HD_INQUIRY,
    .parent            = "scsi-hd",
    .class_init        = inquiry_class_init,
    .instance_init     = inquiry_instance_init,
    .instance_finalize = inquiry_instance_finalize,
};

static void scsi_hd_inquiry_register_types(void)
{
    type_register_static(&scsi_hd_inquiry_info);
}

type_init(scsi_hd_inquiry_register_types)
