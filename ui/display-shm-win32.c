/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The display-shm producer's Windows handle passing: peer checks, copying a
 * section handle into the viewer's process, and ORPHANS recovery. See
 * docs/interop/display-shm.rst.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "ui/display-shm-priv.h"

#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR, 256)
#endif

static bool token_user(HANDLE process, TOKEN_USER *out, DWORD size)
{
    HANDLE token;
    DWORD got;
    bool ok;

    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return false;
    }
    ok = GetTokenInformation(token, TokenUser, out, size, &got);
    CloseHandle(token);
    return ok;
}

static bool same_user(HANDLE process)
{
    union {
        TOKEN_USER u;
        char buf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
    } mine, theirs;

    return token_user(GetCurrentProcess(), &mine.u, sizeof(mine)) &&
           token_user(process, &theirs.u, sizeof(theirs)) &&
           EqualSid(mine.u.User.Sid, theirs.u.User.Sid);
}

/* The process at the other end, opened to copy handles into; same user only. */
bool dshm_win_peer(DShmViewer *v)
{
    SOCKET sock = _get_osfhandle(v->fd);
    DWORD pid = 0, got = 0;

    if (WSAIoctl(sock, SIO_AF_UNIX_GETPEERPID, NULL, 0, &pid, sizeof(pid),
                 &got, NULL, NULL) != 0 || !pid) {
        error_report("display-shm: cannot find the viewer's process: error %d",
                     WSAGetLastError());
        return false;
    }
    v->peer_process = OpenProcess(PROCESS_DUP_HANDLE |
                                  PROCESS_QUERY_LIMITED_INFORMATION,
                                  FALSE, pid);
    if (!v->peer_process) {
        error_report("display-shm: cannot open the viewer, process %lu: "
                     "error %lu", pid, GetLastError());
        return false;
    }
    v->peer_pid = pid;
    if (!same_user(v->peer_process)) {
        CloseHandle(v->peer_process);
        v->peer_process = NULL;
        return false;
    }
    return true;
}

void dshm_win_viewer_free(DShmViewer *v)
{
    if (v->peer_process) {
        CloseHandle(v->peer_process);
        v->peer_process = NULL;
    }
}

static void remote_close(DShmViewer *v, uint64_t value)
{
    DuplicateHandle(v->peer_process, (HANDLE)(uintptr_t)value, NULL, NULL, 0,
                    FALSE, DUPLICATE_CLOSE_SOURCE);
}

/* Copy o's handle into the viewer when its first byte is about to go. */
bool dshm_win_stage(DShmViewer *v, DShmOut *o)
{
    HANDLE dup;

    if (!o->staged) {
        if (!DuplicateHandle(GetCurrentProcess(), o->h, v->peer_process, &dup,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            error_report("display-shm: cannot give the viewer a handle: "
                         "error %lu", GetLastError());
            return false;
        }
        o->staged = (uint64_t)(uintptr_t)dup;
        stq_le_p(o->buf + o->len - 8, o->staged);
    }
    return true;
}

/* A message that never started owes the viewer nothing but its copy. */
void dshm_win_discard(DShmViewer *v, DShmOut *o)
{
    if (o->staged) {
        remote_close(v, o->staged);
        o->staged = 0;
    }
    if (o->h) {
        CloseHandle(o->h);
        o->h = NULL;
    }
}

/* The first byte went: the copy is the viewer's, and ours is done. */
void dshm_win_started(DShmViewer *v, DShmOut *o)
{
    unsigned i = v->handles_started % DSHM_HANDLE_WINDOW;

    v->handles_started++;
    v->started[i].ordinal = v->handles_started;
    v->started[i].value = o->staged;
    o->staged = 0;
    CloseHandle(o->h);
    o->h = NULL;
}

/* Keep what a closed connection left in the viewer for its next HELLO. */
void dshm_win_depart(DShmViewer *v)
{
    DisplayShm *d = v->d;
    int64_t now = dshm_now_ns();
    DShmDeparted *r;
    unsigned keep = 0;

    for (unsigned i = 0; i < d->n_departed; i++) {
        if (now - d->departed[i].at_ns < DSHM_DEPARTED_NS) {
            d->departed[keep++] = d->departed[i];
        }
    }
    d->n_departed = keep;
    if (v->handles_started == v->handles_seen) {
        return;
    }
    if (d->n_departed == DSHM_DEPARTED_MAX) {
        memmove(d->departed, d->departed + 1,
                (DSHM_DEPARTED_MAX - 1) * sizeof(*d->departed));
        d->n_departed--;
    }
    r = &d->departed[d->n_departed++];
    r->viewer_id = v->id;
    r->pid = v->peer_pid;
    r->seen = v->handles_seen;
    r->started = v->handles_started;
    r->at_ns = now;
    for (uint64_t n = v->handles_seen + 1; n <= v->handles_started; n++) {
        r->entry[(n - 1) % DSHM_HANDLE_WINDOW] =
            v->started[(n - 1) % DSHM_HANDLE_WINDOW];
    }
}

/* Answer the viewer's account of its previous connection with ORPHANS. */
bool dshm_win_hello(DShmViewer *v, const DShmHello *h)
{
    DisplayShm *d = v->d;
    g_autofree uint8_t *buf = NULL;
    DShmOrphans m = { .hdr = { .type = DSHM_MSG_ORPHANS } };
    DShmDeparted *r = NULL;
    unsigned at = 0;
    size_t len;

    if (!h->prev_viewer_id) {
        return true;
    }
    for (unsigned i = 0; i < d->n_departed; i++) {
        if (d->departed[i].viewer_id == h->prev_viewer_id &&
            d->departed[i].pid == v->peer_pid &&
            h->prev_session_id == d->session_id) {
            r = &d->departed[i];
            at = i;
        }
    }
    if (r && (h->prev_handles_taken < r->seen ||
              h->prev_handles_taken > r->started)) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "HELLO taken count");
        return false;
    }
    m.count = r ? r->started - h->prev_handles_taken : 0;
    len = sizeof(m) + m.count * sizeof(DShmOrphan);
    buf = g_malloc0(len);
    memcpy(buf, &m, sizeof(m));
    for (unsigned i = 0; i < m.count; i++) {
        uint64_t n = h->prev_handles_taken + 1 + i;
        DShmOrphan o = { .ordinal = n,
                         .value = r->entry[(n - 1) %
                                           DSHM_HANDLE_WINDOW].value };

        memcpy(buf + sizeof(m) + i * sizeof(o), &o, sizeof(o));
    }
    dshm_queue_ext(v, buf, len, false);
    if (r) {
        memmove(r, r + 1, (d->n_departed - at - 1) * sizeof(*r));
        d->n_departed--;
    }
    return true;
}
