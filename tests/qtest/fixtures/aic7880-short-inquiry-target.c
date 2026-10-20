/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* QTest SCSI target that changes to STATUS after a short INQUIRY response. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/scsi/scsi.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "scsi/constants.h"
#include "system/qtest.h"

#define TYPE_AIC7880_SHORT_INQUIRY_TARGET "aic7880-short-inquiry-target"
OBJECT_DECLARE_SIMPLE_TYPE(AIC7880ShortInquiryTarget,
                           AIC7880_SHORT_INQUIRY_TARGET)

typedef struct AIC7880ShortInquiryRequest {
    SCSIRequest req;
    Notifier finished;
    bool delivered;
    uint8_t data[19];
} AIC7880ShortInquiryRequest;

struct AIC7880ShortInquiryTarget {
    SCSIDevice parent_obj;
    SCSIRequest *active;
};

static int32_t aic7880_short_send_command(SCSIRequest *req, uint8_t *cdb)
{
    static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    AIC7880ShortInquiryTarget *s =
        AIC7880_SHORT_INQUIRY_TARGET(req->dev);

    if (s->active || memcmp(cdb, inquiry, sizeof(inquiry))) {
        scsi_req_complete(req, CHECK_CONDITION);
        return 0;
    }
    s->active = scsi_req_ref(req);
    return req->cmd.xfer;
}

static void aic7880_short_read_data(SCSIRequest *req)
{
    AIC7880ShortInquiryRequest *r =
        DO_UPCAST(AIC7880ShortInquiryRequest, req, req);

    if (r->delivered) {
        /* The test controls when the short response changes to STATUS. */
        return;
    }
    r->delivered = true;
    scsi_req_data(req, sizeof(r->data));
}

static uint8_t *aic7880_short_get_buf(SCSIRequest *req)
{
    AIC7880ShortInquiryRequest *r =
        DO_UPCAST(AIC7880ShortInquiryRequest, req, req);

    return r->data;
}

static void aic7880_short_request_finished(Notifier *notifier, void *opaque)
{
    AIC7880ShortInquiryRequest *r =
        container_of(notifier, AIC7880ShortInquiryRequest, finished);
    AIC7880ShortInquiryTarget *s =
        AIC7880_SHORT_INQUIRY_TARGET(r->req.dev);

    if (s->active == &r->req) {
        s->active = NULL;
        scsi_req_unref(&r->req);
    }
}

static void aic7880_short_init_req(SCSIRequest *req)
{
    static const uint8_t data[19] = {
        0, 0, 5, 0x12, 31, 0, 0, 0x10,
        'S', 'H', 'O', 'R', 'T', ' ', ' ', ' ',
        'A', 'I', 'C'
    };
    AIC7880ShortInquiryRequest *r =
        DO_UPCAST(AIC7880ShortInquiryRequest, req, req);

    memcpy(r->data, data, sizeof(data));
    r->finished.notify = aic7880_short_request_finished;
    notifier_list_add(&req->cancel_notifiers, &r->finished);
}

static const SCSIReqOps aic7880_short_reqops = {
    .size = sizeof(AIC7880ShortInquiryRequest),
    .init_req = aic7880_short_init_req,
    .send_command = aic7880_short_send_command,
    .read_data = aic7880_short_read_data,
    .get_buf = aic7880_short_get_buf,
};

static SCSIRequest *aic7880_short_alloc_req(SCSIDevice *dev, uint32_t tag,
                                            uint32_t lun, uint8_t *buf,
                                            void *hba_private)
{
    return scsi_req_alloc(&aic7880_short_reqops, dev, tag, lun, hba_private);
}

static void aic7880_short_realize(SCSIDevice *dev, Error **errp)
{
    if (!qtest_enabled()) {
        error_setg(errp, "AIC-7880 short INQUIRY target requires QTest");
    }
}

static void aic7880_short_unrealize(SCSIDevice *dev)
{
    AIC7880ShortInquiryTarget *s = AIC7880_SHORT_INQUIRY_TARGET(dev);

    if (s->active) {
        scsi_req_unref(s->active);
        s->active = NULL;
    }
}

static void aic7880_short_finish(Object *obj, const char *value,
                                  Error **errp)
{
    AIC7880ShortInquiryTarget *s = AIC7880_SHORT_INQUIRY_TARGET(obj);
    SCSIRequest *req = s->active;
    AIC7880ShortInquiryRequest *r;

    if (!g_str_equal(value, "status") || !req || req->io_canceled) {
        error_setg(errp, "no short INQUIRY request is ready for STATUS");
        return;
    }
    r = DO_UPCAST(AIC7880ShortInquiryRequest, req, req);
    if (!r->delivered) {
        error_setg(errp, "short INQUIRY data has not been offered");
        return;
    }
    s->active = NULL;
    scsi_req_complete(req, CHECK_CONDITION);
    scsi_req_unref(req);
}

static void aic7880_short_instance_init(Object *obj)
{
    object_property_add_str(obj, "x-test-finish", NULL, aic7880_short_finish);
}

static void aic7880_short_class_init(ObjectClass *klass, const void *data)
{
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize = aic7880_short_realize;
    sc->unrealize = aic7880_short_unrealize;
    sc->alloc_req = aic7880_short_alloc_req;
}

static const TypeInfo aic7880_short_info = {
    .name = TYPE_AIC7880_SHORT_INQUIRY_TARGET,
    .parent = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(AIC7880ShortInquiryTarget),
    .instance_init = aic7880_short_instance_init,
    .class_init = aic7880_short_class_init,
};

static void aic7880_short_register_types(void)
{
    type_register_static(&aic7880_short_info);
}

type_init(aic7880_short_register_types)
