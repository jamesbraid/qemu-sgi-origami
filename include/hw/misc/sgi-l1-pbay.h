/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1's end of the power-bay serial link: the initialisation sequence,
 * the polling of the bay's sensors and the status the operator's pbay
 * command reports.
 */
#ifndef HW_MISC_SGI_L1_PBAY_H
#define HW_MISC_SGI_L1_PBAY_H

#include "chardev/char-fe.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"

/* The link's statuses, in the order the L1 numbers them. */
enum {
    SGI_L1_PBAY_NOT_INITIALISED,
    SGI_L1_PBAY_INITIALISING,
    SGI_L1_PBAY_READY,
    SGI_L1_PBAY_BUSY,
    SGI_L1_PBAY_FLASHING,
    SGI_L1_PBAY_COMM_FAILURE,
};

#define SGI_L1_PBAY_FRAME_MAX   300
#define SGI_L1_PBAY_SENSORS     14

typedef void (*SGIL1PbayLog)(void *opaque, const char *text);

typedef struct SGIL1Pbay {
    /* Configuration. */
    CharFrontend chr;
    uint16_t l1_address;
    uint32_t poll_ms;

    /* The owning controller's log. */
    SGIL1PbayLog log;
    void *opaque;

    /* None of this is migrated: a configured link blocks migration. */
    Error *blocker;
    QEMUBH *start_bh;
    QEMUTimer *timeout;
    QEMUTimer *poll_timer;
    uint8_t status;
    uint8_t step;
    uint8_t tag;
    uint8_t attempts;
    uint16_t bay;
    uint8_t target;
    uint8_t capability[3];
    uint16_t record_id;
    uint32_t records;
    uint8_t sensor;
    uint8_t mask[SGI_L1_PBAY_SENSORS + 1];
    uint8_t last_mask[SGI_L1_PBAY_SENSORS + 1];
    /* The supplies present at the last pass, bit n-1 for supply n. */
    uint8_t present;

    /* The transaction in flight, kept for a retry. */
    uint8_t request[32];
    unsigned request_len;
    uint8_t request_cmd;
    uint8_t request_netfn;

    /* The receiver. */
    uint8_t rx[SGI_L1_PBAY_FRAME_MAX];
    unsigned rx_len;
    bool rx_in_frame;
    bool rx_escape;
    bool rx_drop;
} SGIL1Pbay;

/* Add the read-only status properties to @obj. */
void sgi_l1_pbay_init(Object *obj, SGIL1Pbay *p);

/*
 * Attach the link if a backend is configured. @letter is the brick's type
 * character: the L1's UART for this link is closed on some bricks.
 */
bool sgi_l1_pbay_realize(SGIL1Pbay *p, Object *owner, uint32_t letter,
                         SGIL1PbayLog log, void *opaque, Error **errp);
void sgi_l1_pbay_finalize(SGIL1Pbay *p);

/* Restart the link as the L1 does after its own reset. */
void sgi_l1_pbay_reset(SGIL1Pbay *p);

bool sgi_l1_pbay_active(const SGIL1Pbay *p);
const char *sgi_l1_pbay_status_name(const SGIL1Pbay *p);

#endif /* HW_MISC_SGI_L1_PBAY_H */
