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
#define PB_SUPPLIES             6
#define PB_PORTS                8
/* FRU slot 0 is the bay, 1 to 6 the supplies, 7 the seventh device. */
#define PB_FRU_SLOTS            8

typedef void (*SGIL1PbayLog)(void *opaque, const char *text);
/* The output of an operator command, once it has finished. */
typedef void (*SGIL1PbayDone)(void *opaque, const char *text, bool ok);

/* What sgi_l1_pbay_command() did. */
enum {
    SGI_L1_PBAY_REFUSED = -2,       /* the brick does not take the command */
    SGI_L1_PBAY_BAD_ARGS = -1,
    SGI_L1_PBAY_DONE = 0,           /* the done function has been called */
    SGI_L1_PBAY_PENDING = 1,        /* the done function will be called */
};

typedef struct SGIL1Pbay {
    /* Configuration. */
    CharFrontend chr;
    uint16_t l1_address;
    uint32_t poll_ms;
    /* The L1's kernel tick in microseconds: 10000 for its 100 Hz tick. */
    uint32_t tick_us;
    /* L1 setting 2, the bay number: it sets the start delay. */
    uint32_t bay_number;

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
    uint8_t wait;
    uint8_t transmissions;
    uint8_t reads;
    uint16_t bay;
    uint32_t letter;
    uint8_t target;
    uint8_t capability[3];
    uint16_t record_id;
    uint32_t records;
    /* Polling passes completed, for tests. */
    uint32_t passes;
    uint8_t sensor;
    /* From the records: the sensor of each supply and port (0 unmapped). */
    uint8_t supply_sensor[PB_SUPPLIES + 1];
    uint8_t port_sensor[PB_PORTS + 1];
    /* The FRU device byte of each slot (0 none). */
    uint8_t fru_device[PB_FRU_SLOTS];
    /* Index n is supply or port n; the last pass's reading, and the poll's. */
    uint8_t supply[PB_SUPPLIES + 1];
    uint8_t port[PB_PORTS + 1];
    uint8_t supply_last[PB_SUPPLIES + 1];
    uint8_t port_last[PB_PORTS + 1];
    /* The supplies present at the last pass, bit n-1 for supply n. */
    uint8_t present;

    /*
     * L1 setting 85, the environment-monitoring mask (bit 0 presence and AC
     * fail, 1 supply fail, 2 predictive fail, 3 DC ports). It lives in the
     * L1's settings, so a restart of the link keeps it; zero at power-on.
     */
    uint8_t env_mask;
    /* The product-area serial of slot 0, once `pbay serial` has read it. */
    char *serial;

    /* The job in progress: an operator command or a polling pass. */
    struct {
        uint8_t kind;
        uint16_t want;      /* sensor positions or FRU slots to read */
        uint16_t tried, got;
        unsigned pos;
        uint8_t value[PB_SUPPLIES + PB_PORTS];
        char serial[24];
        GString *out;
        SGIL1PbayDone done;
        void *opaque;
    } job;

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

/*
 * Run the operator's pbay command (@argv[0] is "pbay"). A command that
 * needs the bay finishes later, which @can_wait allows; its text is then
 * handed to @done. Otherwise @done is called before this returns. The
 * return value is one of the SGI_L1_PBAY_ results.
 */
int sgi_l1_pbay_command(SGIL1Pbay *p, int argc, char **argv, bool can_wait,
                        SGIL1PbayDone done, void *opaque);

#endif /* HW_MISC_SGI_L1_PBAY_H */
