/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A test-only stand-in for an L2 on the other end of an sgi-l1 "l2" link.
 *
 * It is not a model of the L2. It listens on a Unix socket, reads the
 * segments the controller forwards, and answers one request shape: the
 * partition-record fetch, a request to controller type 1, task 0xa
 * (destination 0x1ffff00a). Every other request gets no answer.
 *
 * Nothing the stub puts in a reply is a claim about what an L2 sends. The
 * reply shape is the one the stock boot firmware's reader accepts in a boot
 * run (an integer result descriptor of 0, then one integer descriptor, read
 * as the brick's partition number); the value in the second descriptor, the
 * filler bytes of a long reply and the segment count of a long reply are the
 * test's own. Those bytes are marked below.
 */
#ifndef SGI_L1_L2_STUB_H
#define SGI_L1_L2_STUB_H

#include <poll.h>
#include <sys/un.h>
#include "qemu/bswap.h"
#include "hw/misc/sgi-l1-l2-link.h"

#define L2_STUB_FETCH_DEST UINT32_C(0x1ffff00a)
#define L2_STUB_TIMEOUT_MS 10000

typedef struct L2Stub {
    char *dir;
    char *path;
    int listener;
    int fd;
} L2Stub;

static inline L2Stub *l2_stub_listen(void)
{
    GError *error = NULL;
    L2Stub *stub = g_new0(L2Stub, 1);
    struct sockaddr_un address = { .sun_family = AF_UNIX };

    stub->dir = g_dir_make_tmp("sgi-l1-l2-XXXXXX", &error);
    g_assert_no_error(error);
    stub->path = g_build_filename(stub->dir, "l2.sock", NULL);
    g_assert_cmpuint(strlen(stub->path), <, sizeof(address.sun_path));
    strcpy(address.sun_path, stub->path);
    stub->fd = -1;
    stub->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(stub->listener, >=, 0);
    g_assert_cmpint(bind(stub->listener, (struct sockaddr *)&address,
                         sizeof(address)), ==, 0);
    g_assert_cmpint(listen(stub->listener, 1), ==, 0);
    return stub;
}

/* The machine connects when it starts: accept it once it is up. */
static inline void l2_stub_accept(L2Stub *stub)
{
    struct pollfd p = { .fd = stub->listener, .events = POLLIN };

    g_assert_cmpint(poll(&p, 1, L2_STUB_TIMEOUT_MS), ==, 1);
    stub->fd = accept(stub->listener, NULL, NULL);
    g_assert_cmpint(stub->fd, >=, 0);
}

static inline void l2_stub_hang_up(L2Stub *stub)
{
    if (stub->fd >= 0) {
        close(stub->fd);
        stub->fd = -1;
    }
}

/* Stop listening, to give a chardev the path or to leave it unconnected. */
static inline void l2_stub_unlisten(L2Stub *stub)
{
    if (stub->listener >= 0) {
        close(stub->listener);
        stub->listener = -1;
    }
    unlink(stub->path);
}

static inline void l2_stub_free(L2Stub *stub)
{
    l2_stub_hang_up(stub);
    l2_stub_unlisten(stub);
    g_assert_cmpint(rmdir(stub->dir), ==, 0);
    g_free(stub->path);
    g_free(stub->dir);
    g_free(stub);
}

static inline bool l2_stub_read_exact(L2Stub *stub, uint8_t *p, size_t n,
                                      int timeout_ms)
{
    while (n) {
        struct pollfd pfd = { .fd = stub->fd, .events = POLLIN };
        ssize_t got;

        if (poll(&pfd, 1, timeout_ms) != 1) {
            return false;
        }
        got = read(stub->fd, p, n);
        g_assert_cmpint(got, >, 0);
        p += got;
        n -= got;
    }
    return true;
}

/* One link message: asserts it is a segment and returns the segment. */
static inline size_t l2_stub_read_segment(L2Stub *stub, uint8_t *segment,
                                          int timeout_ms)
{
    uint8_t head[3];
    size_t length;

    if (!l2_stub_read_exact(stub, head, sizeof(head), timeout_ms)) {
        return 0;
    }
    g_assert_cmpuint(head[0], ==, SGI_L1_L2_KIND_SEGMENT);
    length = lduw_be_p(head + 1);
    g_assert_cmpuint(length, >=, SGI_L1_L2_SEGMENT_MIN);
    g_assert_cmpuint(length, <=, SGI_L1_L2_SEGMENT_MAX);
    memcpy(segment, head + 1, 2);
    g_assert_true(l2_stub_read_exact(stub, segment + 2, length - 2,
                                     L2_STUB_TIMEOUT_MS));
    return length;
}

static inline void l2_stub_write_raw(L2Stub *stub, const uint8_t *p, size_t n)
{
    g_assert_cmpint(write(stub->fd, p, n), ==, n);
}

static inline void l2_stub_write_segment(L2Stub *stub, const uint8_t *segment,
                                         size_t length)
{
    uint8_t message[1 + SGI_L1_L2_SEGMENT_MAX];

    g_assert_cmpuint(length, <=, SGI_L1_L2_SEGMENT_MAX);
    message[0] = SGI_L1_L2_KIND_SEGMENT;
    memcpy(message + 1, segment, length);
    l2_stub_write_raw(stub, message, length + 1);
}

/* The response header: the request's source and destination swapped. */
static inline void l2_stub_response_header(uint8_t *segment, size_t length,
                                           unsigned count_byte,
                                           const uint8_t *request,
                                           unsigned descriptors)
{
    stw_be_p(segment + SGI_L1_L2_SEG_LENGTH, length);
    segment[SGI_L1_L2_SEG_FORMAT] = 2;
    segment[SGI_L1_L2_SEG_SEQ_LOW] = request[SGI_L1_L2_SEG_SEQ_LOW];
    segment[SGI_L1_L2_SEG_COUNT] = count_byte;
    segment[SGI_L1_L2_SEG_CLASS] = SGI_L1_L2_CLASS_RESPONSE;
    memcpy(segment + SGI_L1_L2_SEG_DEST, request + SGI_L1_L2_SEG_SOURCE, 4);
    memcpy(segment + SGI_L1_L2_SEG_SOURCE, request + SGI_L1_L2_SEG_DEST, 4);
    segment[SGI_L1_L2_SEG_NDESC] = descriptors;
    segment[SGI_L1_L2_SEG_SEQ_HIGH] = request[SGI_L1_L2_SEG_SEQ_HIGH];
    memcpy(segment + SGI_L1_L2_SEG_CODE, request + SGI_L1_L2_SEG_CODE, 2);
}

/*
 * The reply the PROM's reader takes as a complete record: one segment, an
 * integer result of 0, then an integer. The integer is test input; whether
 * an L2 sends the partition there is a reading of its partition service,
 * not a capture.
 */
static inline size_t l2_stub_build_fetch_reply(uint8_t *segment,
                                               const uint8_t *request,
                                               uint32_t second_value)
{
    size_t length = SGI_L1_L2_SEG_DESC + 16;

    memset(segment, 0, length);
    l2_stub_response_header(segment, length, SGI_L1_L2_FIRST_SEGMENT | 1,
                            request, 2);
    /* Descriptor 0: integer result 0. Descriptor 1: the value. */
    stl_be_p(segment + SGI_L1_L2_SEG_DESC + 8 + 4, second_value);
    return length;
}

/*
 * Test fixture: @sizes[i] bytes in segment i, a chain of @n segments. The
 * first has the response header and one integer result descriptor of 0; the
 * rest is filler whose byte at offset k is (k + segment index) & 0xff, and
 * every continuation segment is its length, the segment format byte, the
 * flag and sequence bytes copied from the request, the down-count, then
 * filler. None of that is a claim about an L2's reply.
 */
static inline size_t l2_stub_build_chain(uint8_t chain[][SGI_L1_L2_SEGMENT_MAX],
                                         const unsigned *sizes, unsigned n,
                                         const uint8_t *request)
{
    size_t total = 0;

    for (unsigned i = 0; i < n; i++) {
        uint8_t *segment = chain[i];
        unsigned count = n - i;

        g_assert_cmpuint(sizes[i], >=, SGI_L1_L2_SEG_DESC + 8);
        for (unsigned k = 0; k < sizes[i]; k++) {
            segment[k] = (k + i) & 0xff;
        }
        if (!i) {
            memset(segment, 0, SGI_L1_L2_SEG_DESC + 8);
            l2_stub_response_header(segment, sizes[i],
                                    SGI_L1_L2_FIRST_SEGMENT | count,
                                    request, 1);
        } else {
            stw_be_p(segment, sizes[i]);
            segment[SGI_L1_L2_SEG_FORMAT] = 2;
            segment[SGI_L1_L2_SEG_FLAGS] = 0;
            segment[SGI_L1_L2_SEG_SEQ_LOW] = request[SGI_L1_L2_SEG_SEQ_LOW];
            segment[SGI_L1_L2_SEG_COUNT] = count;
        }
        total += sizes[i];
    }
    return total;
}

/*
 * Read one forwarded segment and answer it as the stub does. Returns the
 * request length, or 0 if nothing arrived in @timeout_ms; a request that is
 * not the fetch is read and left unanswered.
 */
static inline size_t l2_stub_serve(L2Stub *stub, uint8_t *request,
                                   uint32_t second_value, int timeout_ms)
{
    uint8_t reply[SGI_L1_L2_SEGMENT_MAX];
    size_t length = l2_stub_read_segment(stub, request, timeout_ms);

    if (length >= SGI_L1_L2_SEG_DESC &&
        request[SGI_L1_L2_SEG_CLASS] == SGI_L1_L2_CLASS_REQUEST &&
        ldl_be_p(request + SGI_L1_L2_SEG_DEST) == L2_STUB_FETCH_DEST) {
        l2_stub_write_segment(stub, reply,
            l2_stub_build_fetch_reply(reply, request, second_value));
    }
    return length;
}

#endif /* SGI_L1_L2_STUB_H */
