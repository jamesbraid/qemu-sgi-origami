/*
 * Host-time waits for the SGI qtests.
 *
 * Under the qtest accelerator, virtual time moves only when the test steps
 * it, so device timing never needs a host wait. Two kinds of work still
 * run on the host's schedule: whatever QEMU's main loop or another host
 * thread does (reading a netdev or chardev socket, block-layer I/O, an RCU
 * callback, a migration), and guest code that a TCG vCPU thread runs.
 * Neither finishes in a fixed host time. At a host load average near 150,
 * waits of one to ten seconds for this kind of work failed while the work
 * was still progressing.
 *
 * So a wait here only bounds a hang. It polls for its condition, logs what
 * it is still waiting for every SGI_WAIT_LOG_S seconds, and fails, naming
 * the wait, after SGI_WAIT_S. The bound matches the SGI migration waits in
 * sgi-migrate.h and upstream's MIGRATION_STATUS_WAIT_TIMEOUT. Where meson's
 * budget for a suite is shorter, meson kills a hang first, and the periodic
 * line still leaves the stuck wait in the test log.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_WAIT_H
#define SGI_WAIT_H

#include "qemu/osdep.h"
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#define SGI_WAIT_S      120
#define SGI_WAIT_LOG_S  15

typedef struct SgiWait {
    char what[96];
    int64_t start;
    int64_t next_log;
} SgiWait;

/* Start a wait; @fmt names what it waits for in the log. */
static inline void G_GNUC_PRINTF(2, 3)
sgi_wait_start(SgiWait *w, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    g_vsnprintf(w->what, sizeof(w->what), fmt, ap);
    va_end(ap);
    w->start = g_get_monotonic_time();
    w->next_log = w->start + SGI_WAIT_LOG_S * G_TIME_SPAN_SECOND;
}

/*
 * Call once per poll while the condition is still false. Returns false,
 * having logged that it gave up, once the wait has run SGI_WAIT_S; for a
 * caller that reports its own state before failing. Otherwise logs every
 * SGI_WAIT_LOG_S, sleeps @us microseconds of host time and returns true.
 */
static inline bool sgi_wait_continue(SgiWait *w, unsigned int us)
{
    int64_t now = g_get_monotonic_time();
    int64_t waited_s = (now - w->start) / G_TIME_SPAN_SECOND;

    if (waited_s >= SGI_WAIT_S) {
        g_test_message("%s: gave up after %d s", w->what, SGI_WAIT_S);
        return false;
    }
    if (now >= w->next_log) {
        g_test_message("%s: still waiting after %" PRId64 " s", w->what,
                       waited_s);
        w->next_log = now + SGI_WAIT_LOG_S * G_TIME_SPAN_SECOND;
    }
    g_usleep(us);
    return true;
}

/* As sgi_wait_continue(), but fails the test itself once the bound passes. */
static inline void sgi_wait_pause(SgiWait *w, unsigned int us)
{
    if (!sgi_wait_continue(w, us)) {
        g_error("%s: gave up after %d s", w->what, SGI_WAIT_S);
    }
}

/*
 * Wait until QEMU has read everything the test wrote to @fd, its end of a
 * stream socketpair whose other end a netdev or chardev socket backend
 * reads. On Linux, TIOCOUTQ on a Unix stream socket counts the bytes the
 * peer has not consumed yet. Either backend passes what it reads to the
 * device within the same main-loop callback, and QEMU handles the test's
 * next qtest command only after that callback returns. So once this
 * returns, a frame or byte the device drops has been dropped, rather than
 * still sitting in the socket, and a window asserting that nothing arrived
 * no longer depends on how soon the host scheduled QEMU.
 *
 * This needs a backend that reads: one holding data the device could not
 * take stops reading, and this wait then fails. Hosts without TIOCOUTQ on
 * sockets fall back to the fixed pause these windows used.
 */
static inline void sgi_wait_socket_read(int fd)
{
#ifdef TIOCOUTQ
    SgiWait w;
    int unread;

    sgi_wait_start(&w, "QEMU reading socket %d", fd);
    while (ioctl(fd, TIOCOUTQ, &unread) == 0) {
        if (unread == 0) {
            return;
        }
        sgi_wait_pause(&w, 1000);
    }
#endif
    g_usleep(100 * 1000);
}

#endif /* SGI_WAIT_H */
