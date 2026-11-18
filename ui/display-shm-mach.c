/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * macos.iosurface: a Mach port cannot ride the Unix socket, so a viewer
 * registers a receive port at our bootstrap service and slots go there.
 * See docs/interop/display-shm.rst.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "crypto/random.h"
#include "ui/display-shm-priv.h"
#include <mach/mach.h>
#include <servers/bootstrap.h>

typedef struct DShmMach {
    DisplayShm *d;
    mach_port_t service;
    char name[DSHM_MACH_SERVICE_BYTES];
} DShmMach;

typedef struct DShmMachSlotQ {
    uint32_t region;
    mach_port_t port;
} DShmMachSlotQ;

typedef struct DShmMachReg {
    DisplayShm *d;
    uint64_t token;
    mach_port_t port;
} DShmMachReg;

typedef struct {
    mach_msg_base_t base;
    mach_msg_port_descriptor_t port;
    DShmMachRegister body;
    mach_msg_trailer_t trailer;
} RegisterIn;

typedef struct {
    mach_msg_base_t base;
    mach_msg_port_descriptor_t port;
    DShmMachSlot body;
} SlotOut;

/* ---- Ports ------------------------------------------------------------ */

/* One more send right on the same port, for whoever takes it next. */
uint32_t dshm_mach_port_dup(int64_t port)
{
    if (port <= 0 ||
        mach_port_mod_refs(mach_task_self(), (mach_port_t)port,
                           MACH_PORT_RIGHT_SEND, 1) != KERN_SUCCESS) {
        return 0;
    }
    return (mach_port_t)port;
}

void dshm_mach_port_drop(uint32_t port)
{
    if (port) {
        mach_port_deallocate(mach_task_self(), port);
    }
}

/* ---- The service ------------------------------------------------------ */

static void mach_register_bh(void *opaque)
{
    DShmMachReg *r = opaque;
    DisplayShm *d = r->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmViewer *v = d->viewers[i];

        if (v->mach_token == r->token && !v->mach_port && !v->closing) {
            v->mach_port = r->port;
            r->port = MACH_PORT_NULL;
            dshm_mach_pump(v);
            break;
        }
    }
    dshm_mach_port_drop(r->port);
    g_free(r);
}

/* Registrations arrive on this thread; the main loop does the rest. */
static void *mach_service_thread(void *opaque)
{
    DShmMach *m = opaque;
    RegisterIn in;

    for (;;) {
        mach_msg_return_t kr;
        DShmMachReg *r;

        memset(&in, 0, sizeof(in));
        kr = mach_msg(&in.base.header, MACH_RCV_MSG, 0, sizeof(in),
                      m->service, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr == MACH_RCV_TOO_LARGE) {
            continue;           /* destroyed with its rights: not ours */
        }
        if (kr != MACH_MSG_SUCCESS) {
            break;              /* the service port is gone */
        }
        if (in.base.header.msgh_id != DSHM_MACH_REGISTER ||
            !(in.base.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
            in.base.header.msgh_size != offsetof(RegisterIn, trailer) ||
            in.base.body.msgh_descriptor_count != 1 ||
            in.port.type != MACH_MSG_PORT_DESCRIPTOR ||
            in.port.disposition != MACH_MSG_TYPE_PORT_SEND ||
            in.port.name == MACH_PORT_NULL) {
            mach_msg_destroy(&in.base.header);
            continue;
        }
        r = g_new0(DShmMachReg, 1);
        r->d = m->d;
        r->token = in.body.token;
        r->port = in.port.name;
        aio_bh_schedule_oneshot(qemu_get_aio_context(), mach_register_bh, r);
    }
    return NULL;
}

/* Check the instance's service in on first use; its name goes in the accept. */
bool dshm_mach_start(DisplayShm *d, const char **service)
{
    DShmMach *m = d->mach;
    static unsigned serial;
    kern_return_t kr;
    QemuThread th;

    if (m) {
        *service = m->name;
        return true;
    }
    m = g_new0(DShmMach, 1);
    m->d = d;
    snprintf(m->name, sizeof(m->name), "org.qemu.display-shm.%u.%u.%u",
             (unsigned)geteuid(), (unsigned)getpid(), serial++);
    kr = bootstrap_check_in(bootstrap_port, m->name, &m->service);
    if (kr != BOOTSTRAP_SUCCESS) {
        warn_report_once("display-shm: cannot check in the Mach service %s: "
                         "%s; IOSurface slots are off", m->name,
                         mach_error_string(kr));
        g_free(m);
        return false;
    }
    qemu_thread_create(&th, "dshm-mach", mach_service_thread, m,
                       QEMU_THREAD_DETACHED);
    d->mach = m;
    *service = m->name;
    return true;
}

uint64_t dshm_mach_token(DShmViewer *v)
{
    Error *err = NULL;

    while (!v->mach_token) {
        if (qcrypto_random_bytes(&v->mach_token, sizeof(v->mach_token),
                                 &err) < 0) {
            error_report_err(err);
            err = NULL;
            v->mach_token = (uint64_t)g_random_int() << 32 | g_random_int();
        }
    }
    return v->mach_token;
}

/* ---- Slots to a viewer ------------------------------------------------ */

static mach_msg_return_t send_slot(DShmViewer *v, const DShmMachSlotQ *q)
{
    SlotOut m = { };

    m.base.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
                              MACH_MSGH_BITS_COMPLEX;
    m.base.header.msgh_size = sizeof(m);
    m.base.header.msgh_remote_port = v->mach_port;
    m.base.header.msgh_id = DSHM_MACH_SLOT;
    m.base.body.msgh_descriptor_count = 1;
    m.port.name = q->port;
    m.port.disposition = MACH_MSG_TYPE_COPY_SEND;
    m.port.type = MACH_MSG_PORT_DESCRIPTOR;
    m.body.region = q->region;
    /* Never block the main loop on a viewer that is not reading. */
    return mach_msg(&m.base.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                    sizeof(m), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/* Send what the viewer can take now; the rest waits for its registration. */
void dshm_mach_pump(DShmViewer *v)
{
    DShmMachSlotQ *q;

    while (v->mach_port && !v->closing && (q = g_queue_pop_head(&v->mach_q))) {
        mach_msg_return_t kr = send_slot(v, q);

        dshm_mach_port_drop(q->port);
        g_free(q);
        if (kr == MACH_SEND_TIMED_OUT) {
            dshm_viewer_doom(v, DSHM_ERR_LIMIT, "Mach queue full");
        }
        /* any other failure is a viewer that has gone: the socket says so */
    }
}

/* The REGION is on its way: queue the slot's port, which we own. */
void dshm_mach_queue_slot(DShmViewer *v, uint32_t region, uint32_t port)
{
    DShmMachSlotQ *q = g_new(DShmMachSlotQ, 1);

    *q = (DShmMachSlotQ) { .region = region, .port = port };
    g_queue_push_tail(&v->mach_q, q);
    dshm_mach_pump(v);
}

void dshm_mach_forget(DShmViewer *v, uint32_t region)
{
    for (GList *l = v->mach_q.head, *next; l; l = next) {
        DShmMachSlotQ *q = l->data;

        next = l->next;
        if (q->region == region) {
            g_queue_delete_link(&v->mach_q, l);
            dshm_mach_port_drop(q->port);
            g_free(q);
        }
    }
}

void dshm_mach_viewer_free(DShmViewer *v)
{
    DShmMachSlotQ *q;

    while ((q = g_queue_pop_head(&v->mach_q))) {
        dshm_mach_port_drop(q->port);
        g_free(q);
    }
    dshm_mach_port_drop(v->mach_port);
    v->mach_port = MACH_PORT_NULL;
}
