/* SPDX-License-Identifier: BSD-3-Clause */
/* Timed pin fixtures exercise the shared engine, not PCF register timing. */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/i2c-wire.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "migration/vmstate.h"
#include "../migration/qemu-file.h"
#include "../migration/savevm.h"
#include "io/channel-file.h"

#define TYPE_WIRE_HOST "i2c-wire-test-host"
#define TYPE_WIRE_SLAVE "i2c-wire-test-slave"
/* Carries no migration state, so no wire may be enabled over it. */
#define TYPE_WIRE_MUTE "i2c-wire-test-mute"

static int64_t virtual_time;

int64_t cpus_get_virtual_clock(void)
{
    return virtual_time;
}

void cpus_set_virtual_clock(int64_t value)
{
    virtual_time = value;
}

typedef struct TestSlave {
    I2CSlave parent_obj;
    unsigned starts, writes, reads, stops, restarts, nacks, addresses;
    uint8_t last_address;
    unsigned services[I2C_WIRE_SERVICE_COUNT];
    unsigned reads_at_service[I2C_WIRE_SERVICE_COUNT];
    enum i2c_wire_service hold_service;
    uint8_t value;
} TestSlave;

static int slave_event(I2CSlave *slave, enum i2c_event event)
{
    TestSlave *s = (TestSlave *)slave;

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        s->starts++;
        break;
    case I2C_FINISH:
        s->stops++;
        break;
    case I2C_RESTART:
        s->restarts++;
        break;
    case I2C_NACK:
        s->nacks++;
        break;
    default:
        g_assert_not_reached();
    }
    return 0;
}

static int slave_send(I2CSlave *slave, uint8_t value)
{
    TestSlave *s = (TestSlave *)slave;

    s->writes++;
    s->value = value;
    return 0;
}

static uint8_t slave_recv(I2CSlave *slave)
{
    TestSlave *s = (TestSlave *)slave;

    s->reads++;
    return s->value++;
}

static bool slave_wire_service(I2CSlave *slave,
                               enum i2c_wire_service service)
{
    TestSlave *s = (TestSlave *)slave;

    s->reads_at_service[service] = s->reads;
    s->services[service]++;
    return service == s->hold_service;
}

static void slave_wire_address(I2CSlave *slave, uint8_t address_byte)
{
    TestSlave *s = (TestSlave *)slave;

    s->addresses++;
    s->last_address = address_byte;
}

/* A wired bus refuses a slave that cannot report in when the wire loads. */
static const VMStateDescription vmstate_test_slave = {
    .name = "i2c-wire-test-slave",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, TestSlave),
        VMSTATE_END_OF_LIST()
    },
};

static void slave_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);
    I2CWireEndpointClass *ec = I2C_WIRE_ENDPOINT_CLASS(oc);

    DEVICE_CLASS(oc)->vmsd = &vmstate_test_slave;
    sc->event = slave_event;
    sc->send = slave_send;
    sc->recv = slave_recv;
    ec->wire_service = slave_wire_service;
    ec->wire_address = slave_wire_address;
}

typedef struct Fixture {
    DeviceState *host;
    I2CBus *bus;
    GArray *events;
    I2CSlave *a, *b;
} Fixture;

static void observe(void *opaque, const I2CWireEvent *event)
{
    Fixture *f = opaque;

    g_array_append_val(f->events, *event);
}

static void advance_to(int64_t time)
{
    g_assert_cmpint(qemu_clock_advance_virtual_time(time), ==, time);
}

static void setup_masters(Fixture *f, unsigned master_count)
{
    f->host = qdev_new(TYPE_WIRE_HOST);
    qdev_realize(f->host, NULL, &error_abort);
    f->bus = i2c_init_bus_type(TYPE_I2C_WIRE, f->host, "i2c");
    f->a = i2c_slave_create_simple(f->bus, TYPE_WIRE_SLAVE, 0x20);
    f->b = i2c_slave_create_simple(f->bus, TYPE_WIRE_SLAVE, 0x21);
    ((TestSlave *)f->a)->hold_service = I2C_WIRE_SERVICE_COUNT;
    ((TestSlave *)f->b)->hold_service = I2C_WIRE_SERVICE_COUNT;
    f->events = g_array_new(false, false, sizeof(I2CWireEvent));
    g_assert_cmpint(i2c_bus_enable_wire(f->bus, master_count, observe, f,
                                        &error_abort),
                     ==, 0);
    g_assert_cmpint(i2c_wire_configure(f->bus, 0, 5, 7, false), ==, 0);
    g_assert_cmpint(i2c_wire_configure(f->bus, 1, 9, 3, false), ==, 0);
    if (master_count == 3) {
        g_assert_cmpint(i2c_wire_configure(f->bus, 2, 7, 4, false), ==, 0);
    }
}

static void setup(Fixture *f, gconstpointer unused)
{
    setup_masters(f, 2);
}

static void setup_three(Fixture *f, gconstpointer unused)
{
    setup_masters(f, 3);
}

static void teardown(Fixture *f, gconstpointer unused)
{
    object_unparent(OBJECT(f->host));
    object_unref(OBJECT(f->host));
    g_array_unref(f->events);
}

static void stage_pair(Fixture *f, int64_t at, bool clock, bool reverse)
{
    unsigned first = reverse ? 1 : 0;

    g_assert_cmpint(i2c_wire_stage(f->bus, first, at, false, clock), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, first ^ 1, at, false, clock), ==, 0);
}

static bool write_byte(Fixture *f, uint8_t a, uint8_t b, bool first);
static void stop_bytes(Fixture *f);

static void test_clock_synchronization(Fixture *f, gconstpointer reversed)
{
    int64_t start = virtual_time;

    stage_pair(f, start + 10, false, reversed != NULL);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmpuint(f->events->len, ==, 0);
    advance_to(start + 10);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 3);
    g_assert_true(i2c_wire_scl(f->bus));
    g_assert_false(i2c_wire_sda(f->bus));
    stage_pair(f, start + 20, true, reversed != NULL);
    advance_to(start + 20);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 25);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 29);
    g_assert_true(i2c_wire_scl(f->bus));
    advance_to(start + 31);
    g_assert_true(i2c_wire_scl(f->bus));
    advance_to(start + 32);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 41);
    g_assert_true(i2c_wire_scl(f->bus));
    g_assert_cmpuint(f->events->len, ==, 5);
    for (unsigned i = 0; i < f->events->len; i++) {
        static const int offsets[] = { 10, 20, 29, 32, 41 };
        const I2CWireEvent *event = &g_array_index(f->events, I2CWireEvent, i);

        g_assert_cmpint(event->time, ==, start + offsets[i]);
        g_assert_cmpint(event->scl, ==, !(i & 1));
        g_assert_false(event->sda);
        g_assert_cmphex(event->contenders, ==, 3);
        g_assert_cmphex(event->lost, ==, 0);
    }
}

/*
 * The same synchronised clock as above, but nothing services the wire's
 * timer until the clock is past six of its edges. A host that runs a timer
 * late has not moved the wire's deadlines, so each edge still lands at its
 * own instant, and the edges after it count from there rather than from
 * whenever the timer happened to run.
 */
static void test_late_timer_keeps_instants(Fixture *f, gconstpointer unused)
{
    static const int offsets[] = { 10, 20, 29, 32, 41, 44 };
    int64_t start = virtual_time;

    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
    stage_pair(f, start + 20, true, false);

    cpus_set_virtual_clock(start + 45);
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);

    g_assert_cmpuint(f->events->len, ==, ARRAY_SIZE(offsets));
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        const I2CWireEvent *event = &g_array_index(f->events, I2CWireEvent, i);

        g_assert_cmpint(event->time, ==, start + offsets[i]);
        g_assert_cmpint(event->scl, ==, !(i & 1));
    }
    advance_to(start + 53);
    g_assert_cmpuint(f->events->len, ==, ARRAY_SIZE(offsets) + 1);
    g_assert_cmpint(g_array_index(f->events, I2CWireEvent,
                                  f->events->len - 1).time, ==, start + 53);
}

/*
 * A participant with deadlines of its own, timed against the wire. It records
 * what it saw at each of them and may hold the clock line from inside one.
 */
typedef struct TimedParticipant {
    Fixture *f;
    QEMUTimer *timer;
    int64_t deadline[3];
    unsigned count, done;
    int64_t instant[3];
    unsigned events_seen[3];
    bool scl_seen[3];
    bool hold_lines;
} TimedParticipant;

static int64_t participant_next(void *opaque)
{
    TimedParticipant *p = opaque;

    return p->done < p->count ? p->deadline[p->done] : INT64_MAX;
}

static void participant_run(void *opaque, int64_t now)
{
    TimedParticipant *p = opaque;
    unsigned i = p->done++;

    g_assert_cmpint(now, ==, p->deadline[i]);
    p->instant[i] = i2c_wire_now(p->f->bus);
    p->events_seen[i] = p->f->events->len;
    p->scl_seen[i] = i2c_wire_scl(p->f->bus);
    if (p->hold_lines && i == 1) {
        g_assert_cmpint(i2c_wire_hold_after(p->f->a, 5, true), ==, 0);
    } else if (p->hold_lines && i == 2) {
        g_assert_cmpint(i2c_wire_hold_after(p->f->a, 1, false), ==, 0);
    }
}

/* Its own timer does nothing but bring the timeline up to date. */
static void participant_timer(void *opaque)
{
    TimedParticipant *p = opaque;

    i2c_wire_advance(p->f->bus);
}

static void participant_attach(TimedParticipant *p, Fixture *f)
{
    p->f = f;
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, participant_timer, p);
    timer_mod(p->timer, p->deadline[0]);
    g_assert_cmpint(i2c_wire_set_timeline(f->bus, 1, participant_next,
                                          participant_run, p), ==, 0);
}

static void participant_detach(TimedParticipant *p)
{
    g_assert_cmpint(i2c_wire_clear_timeline(p->f->bus, 1, participant_run, p),
                    ==, 0);
    timer_free(p->timer);
}

/*
 * One poll, long after the wire's timer and the participant's should have
 * run, settles both in time order. The participant runs at its own instants,
 * ahead of a wire edge due at the same one, and a hold it requests counts
 * from its instant, not from the poll: held at 35 and released at 45, the
 * rising edge the masters would have made at 41 waits for the release.
 */
static void test_participant_timeline(Fixture *f, gconstpointer unused)
{
    static const int offsets[] = { 10, 20, 29, 32, 45, 48 };
    int64_t start = virtual_time;
    TimedParticipant p = {
        .deadline = { start + 29, start + 30, start + 44 },
        .count = 3, .hold_lines = true,
    };

    participant_attach(&p, f);
    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
    stage_pair(f, start + 20, true, false);

    cpus_set_virtual_clock(start + 50);
    i2c_wire_advance(f->bus);

    g_assert_cmpuint(f->events->len, ==, ARRAY_SIZE(offsets));
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        const I2CWireEvent *event = &g_array_index(f->events, I2CWireEvent, i);

        g_assert_cmpint(event->time, ==, start + offsets[i]);
        g_assert_cmpint(event->scl, ==, !(i & 1));
    }
    g_assert_cmpuint(p.done, ==, 3);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmpint(p.instant[i], ==, p.deadline[i]);
    }
    /* At 29 the masters' rising edge is due too, and has not happened yet. */
    g_assert_cmpuint(p.events_seen[0], ==, 2);
    g_assert_false(p.scl_seen[0]);
    g_assert_cmpuint(p.events_seen[1], ==, 3);
    g_assert_true(p.scl_seen[1]);
    g_assert_cmpuint(p.events_seen[2], ==, 4);
    g_assert_false(p.scl_seen[2]);
    /* Outside an advance the timeline's instant is the present. */
    g_assert_cmpint(i2c_wire_now(f->bus), ==, start + 50);
    participant_detach(&p);
}

/*
 * The timers are only a backstop. Once a poll has settled a deadline, the
 * participant's timer still armed for it, and any the host runs afterwards,
 * find nothing left to do: no second run and no further edge.
 */
static void test_late_timer_after_advance(Fixture *f, gconstpointer unused)
{
    int64_t start = virtual_time;
    TimedParticipant p = { .deadline = { start + 30 }, .count = 1 };
    unsigned events;

    participant_attach(&p, f);
    stage_pair(f, start + 10, false, false);

    cpus_set_virtual_clock(start + 35);
    i2c_wire_advance(f->bus);
    g_assert_cmpuint(p.done, ==, 1);
    g_assert_cmpuint(f->events->len, ==, 1);
    g_assert_cmpint(g_array_index(f->events, I2CWireEvent, 0).time, ==,
                    start + 10);
    g_assert_true(timer_pending(p.timer));

    events = f->events->len;
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
    g_assert_false(timer_pending(p.timer));
    g_assert_cmpuint(p.done, ==, 1);
    g_assert_cmpuint(f->events->len, ==, events);
    i2c_wire_advance(f->bus);
    g_assert_cmpuint(p.done, ==, 1);
    g_assert_cmpuint(f->events->len, ==, events);
    participant_detach(&p);
}

/* A timeline attaches once per participant, and only to a wired bus. */
static void test_timeline_attachment(Fixture *f, gconstpointer unused)
{
    TimedParticipant p = { .count = 0 };

    g_assert_cmpint(i2c_wire_set_timeline(f->bus, 2, participant_next,
                                          participant_run, &p), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_set_timeline(f->bus, 0, NULL,
                                          participant_run, &p), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_set_timeline(f->bus, 0, participant_next,
                                          participant_run, &p), ==, 0);
    g_assert_cmpint(i2c_wire_set_timeline(f->bus, 0, participant_next,
                                          participant_run, &p), ==, -EBUSY);
    g_assert_cmpint(i2c_wire_clear_timeline(f->bus, 0, participant_run, f),
                    ==, -ENOENT);
    g_assert_cmpint(i2c_wire_clear_timeline(f->bus, 0, participant_run, &p),
                    ==, 0);
}

static void test_invalid_configuration(Fixture *f, gconstpointer unused)
{
    g_assert_cmpint(i2c_wire_configure(f->bus, 0, 0, 7, false), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_configure(f->bus, 0, 5, -1, false), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_configure(f->bus, 2, 5, 7, false), ==, -EINVAL);
    g_assert_cmpint(i2c_bus_start_resolved(f->bus, 0), ==, -ENOTSUP);
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, virtual_time - 1, false, false),
                    ==, -EINVAL);
    g_assert_false(i2c_bus_busy(f->bus));
}

static void test_arbitration(Fixture *f, gconstpointer mode_ptr)
{
    int64_t start = virtual_time;
    unsigned mode = GPOINTER_TO_UINT(mode_ptr);
    bool keep_clock = mode == 1;
    bool reverse = mode == 2;
    unsigned loser = keep_clock ? 1 : 0;
    unsigned first;
    const I2CWireEvent *event;

    if (keep_clock) {
        g_assert_cmpint(i2c_wire_configure(f->bus, 1, 9, 3, true), ==, 0);
    }
    stage_pair(f, start + 10, false, reverse);
    advance_to(start + 10);
    stage_pair(f, start + 20, true, reverse);
    advance_to(start + 32);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 3);
    first = reverse ? loser ^ 1 : loser;
    g_assert_cmpint(i2c_wire_stage(f->bus, first, start + 33,
                                  first == loser, true), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, first ^ 1, start + 33,
                                  (first ^ 1) == loser, true), ==, 0);
    advance_to(start + 40);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 3);
    advance_to(start + 41);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 3 ^ (1U << loser));
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmphex(event->lost, ==, 1U << loser);
    g_assert_false(event->sda);
    advance_to(start + 44);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 49);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 53);
    g_assert_true(i2c_wire_scl(f->bus));
    if (!keep_clock) {
        static const int offsets[] = { 10, 20, 29, 32, 41, 44, 53 };
        static const bool clocks[] = { true, false, true, false,
                                      true, false, true };

        g_assert_cmpuint(f->events->len, ==, ARRAY_SIZE(offsets));
        for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
            const I2CWireEvent *e =
                &g_array_index(f->events, I2CWireEvent, i);

            g_assert_cmpint(e->time, ==, start + offsets[i]);
            g_assert_cmphex(e->contenders, ==, i < 4 ? 3 : 2);
            g_assert_cmphex(e->lost, ==, i == 4 ? 1 : 0);
            g_assert_cmpint(e->scl, ==, clocks[i]);
            g_assert_false(e->sda);
            g_assert_cmpint(e->error, ==, 0);
        }
    }
}

static void test_independent_holds(Fixture *f, gconstpointer reverse)
{
    int64_t start = virtual_time;
    I2CSlave *first = reverse ? f->b : f->a;
    I2CSlave *second = reverse ? f->a : f->b;

    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
    stage_pair(f, start + 20, true, false);
    advance_to(start + 20);
    g_assert_cmpint(i2c_wire_hold(first, start + 21, true), ==, 0);
    g_assert_cmpint(i2c_wire_hold(second, start + 21, true), ==, 0);
    advance_to(start + 29);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpint(i2c_wire_hold(first, start + 30, false), ==, 0);
    advance_to(start + 35);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpint(i2c_wire_hold(second, start + 40, false), ==, 0);
    advance_to(start + 40);
    g_assert_true(i2c_wire_scl(f->bus));
    /* High clocks count from the actual release, not the original deadline. */
    advance_to(start + 42);
    g_assert_true(i2c_wire_scl(f->bus));
    advance_to(start + 43);
    g_assert_false(i2c_wire_scl(f->bus));
}

static void test_clock_admission_while_low(Fixture *f, gconstpointer unused)
{
    int64_t start = virtual_time;
    const I2CWireEvent *event;

    g_assert_cmpint(i2c_wire_stage(f->bus, 0, start + 10,
                                  false, false), ==, 0);
    advance_to(start + 10);
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, start + 20,
                                  false, true), ==, 0);
    /* Accepted while high, but due after the peer has pulled SCL low. */
    g_assert_cmpint(i2c_wire_stage(f->bus, 1, start + 21,
                                  true, true), ==, 0);
    advance_to(start + 20);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 21);
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, -ENOTSUP);
}

static void test_contender_clock_admission_while_low(Fixture *f,
                                                     gconstpointer unused)
{
    int64_t start = virtual_time;
    const I2CWireEvent *event;

    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 3);
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, start + 20,
                                  false, true), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, 1, start + 21,
                                  false, true), ==, 0);
    advance_to(start + 20);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 21);
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, 0);
    advance_to(start + 25);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 29);
    g_assert_true(i2c_wire_scl(f->bus));
}

static void test_contender_clock_admission_at_deadline(
    Fixture *f, gconstpointer delayed_ptr)
{
    int64_t start = virtual_time;
    unsigned delayed = GPOINTER_TO_UINT(delayed_ptr);
    unsigned first = delayed ^ 1;
    int64_t first_low = first ? 9 : 5;
    const I2CWireEvent *event;

    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
    g_assert_cmpint(i2c_wire_stage(f->bus, first, start + 20,
                                  false, true), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, delayed,
                                  start + 20 + first_low,
                                  false, true), ==, 0);
    advance_to(start + 20);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(start + 20 + first_low);
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, 0);
    advance_to(start + 29);
    g_assert_true(i2c_wire_scl(f->bus));
}

static void test_contender_clock_admission_preserves_error(
    Fixture *f, gconstpointer unused)
{
    int64_t start = virtual_time;
    const I2CWireEvent *event;

    g_assert_cmpint(i2c_wire_stage(f->bus, 1, start + 10,
                                  false, false), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, 2, start + 10,
                                  false, false), ==, 0);
    advance_to(start + 10);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 6);
    g_assert_cmpint(i2c_wire_stage(f->bus, 1, start + 20,
                                  false, true), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, start + 21,
                                  false, true), ==, 0);
    g_assert_cmpint(i2c_wire_stage(f->bus, 2, start + 21,
                                  false, true), ==, 0);
    advance_to(start + 20);
    advance_to(start + 21);
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, -ENOTSUP);
}

static void test_inactive_reset(Fixture *f, gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;

    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(i2c_wire_configure(f->bus, i, 5, 5, false), ==, 0);
    }
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, virtual_time + 10,
                                  false, false), ==, 0);
    advance_to(virtual_time + 10);
    g_assert_true(write_byte(f, 0x40, 0xff, true));
    g_assert_cmpuint(a->starts, ==, 1);
    g_assert_cmpint(i2c_wire_reset_master(f->bus, 1), ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 1);
    g_assert_cmpint(i2c_wire_reset_master(f->bus, 0), ==, -ENOTSUP);
    g_assert_true(write_byte(f, 0xa5, 0xff, false));
    g_assert_cmpuint(a->writes, ==, 1);
    g_assert_cmphex(a->value, ==, 0xa5);
    stop_bytes(f);
    g_assert_cmpuint(a->stops, ==, 1);
}

static void begin_bytes(Fixture *f)
{
    int64_t start = virtual_time;

    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(i2c_wire_configure(f->bus, i, 5, 5, false), ==, 0);
    }
    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
}

static bool write_byte(Fixture *f, uint8_t a, uint8_t b, bool first)
{
    int64_t low = virtual_time + (first ? 10 : 0);
    uint8_t values[] = { a, b };
    bool ack;

    for (unsigned bit = 0; bit < 8; bit++) {
        for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
            if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
                g_assert_cmpint(i2c_wire_stage(f->bus, i,
                       low + (first && !bit ? 0 : 1),
                       (values[i] >> (7 - bit)) & 1, true), ==, 0);
            }
        }
        advance_to(low + 5);
        advance_to(low + 10);
        low += 10;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 1, true, true),
                            ==, 0);
        }
    }
    advance_to(low + 5);
    ack = !i2c_wire_sda(f->bus);
    advance_to(low + 10);
    return ack;
}

static void write_byte_to_ack(Fixture *f, uint8_t a, uint8_t b)
{
    int64_t low = virtual_time;
    uint8_t values[] = { a, b };

    for (unsigned bit = 0; bit < 8; bit++) {
        for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
            if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
                g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 1,
                       (values[i] >> (7 - bit)) & 1, true), ==, 0);
            }
        }
        advance_to(low + 5);
        advance_to(low + 10);
        low += 10;
    }
}

static void finish_ack(Fixture *f)
{
    int64_t low = virtual_time;

    for (unsigned i = 0; i < 2; i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 1,
                                          true, true), ==, 0);
        }
    }
    advance_to(low + 5);
    advance_to(low + 10);
}

static void stop_bytes(Fixture *f)
{
    int64_t low = virtual_time;

    for (unsigned i = 0; i < 2; i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 5, false, false),
                            ==, 0);
        }
    }
    advance_to(low + 5);
    for (unsigned i = 0; i < 2; i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 7, true, false),
                            ==, 0);
        }
    }
    advance_to(low + 7);
}

static void test_endpoint_write(Fixture *f, gconstpointer differing)
{
    TestSlave *a = (TestSlave *)f->a;
    TestSlave *b = (TestSlave *)f->b;

    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, differing ? 0x42 : 0x40, true));
    g_assert_cmpuint(a->starts, ==, 1);
    g_assert_cmpuint(b->starts, ==, 0);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, differing ? 1 : 3);
    g_assert_true(write_byte(f, 0xa5, 0xa5, false));
    g_assert_cmpuint(a->writes, ==, 1);
    g_assert_cmphex(a->value, ==, 0xa5);
    g_assert_cmpuint(b->writes, ==, 0);
    stop_bytes(f);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmpuint(a->stops, ==, 1);
    g_assert_cmpuint(b->stops, ==, 0);
}

static void test_post_ack_service(Fixture *f, gconstpointer reverse)
{
    TestSlave *a = (TestSlave *)f->a;
    TestSlave *b = (TestSlave *)f->b;
    I2CSlave *first = reverse ? f->b : f->a;
    I2CSlave *second = reverse ? f->a : f->b;
    int64_t completed;

    a->hold_service = I2C_WIRE_WRITE;
    b->hold_service = I2C_WIRE_WRITE;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x00, 0x00, true));
    g_assert_cmpuint(a->services[I2C_WIRE_ADDRESS_SEND], ==, 1);
    g_assert_cmpuint(b->services[I2C_WIRE_ADDRESS_SEND], ==, 1);
    g_assert_true(write_byte(f, 0xa5, 0xa5, false));
    completed = virtual_time;
    g_assert_cmpuint(a->writes, ==, 1);
    g_assert_cmpuint(b->writes, ==, 1);
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 1);
    g_assert_cmpuint(b->services[I2C_WIRE_WRITE], ==, 1);
    g_assert_cmpint(i2c_wire_hold(first, completed + 6, false), ==, 0);
    g_assert_cmpint(i2c_wire_hold(second, completed + 7, false), ==, 0);
    advance_to(completed + 5);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(completed + 6);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 1);
    g_assert_cmpuint(b->services[I2C_WIRE_WRITE], ==, 1);
    advance_to(completed + 7);
    g_assert_true(i2c_wire_scl(f->bus));
}

static void restart_bytes(Fixture *f)
{
    int64_t low = virtual_time;

    for (unsigned i = 0; i < 2; i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 5, true, false),
                            ==, 0);
        }
    }
    advance_to(low + 5);
    for (unsigned i = 0; i < 2; i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 7, false, false),
                            ==, 0);
        }
    }
    advance_to(low + 7);
}

static void test_restart_nack(Fixture *f, gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;

    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    g_assert_true(write_byte(f, 0x7e, 0x7e, false));
    restart_bytes(f);
    g_assert_cmpuint(a->restarts, ==, 1);
    g_assert_cmpuint(a->stops, ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_false(write_byte(f, 0x60, 0x60, true));
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_true(QLIST_EMPTY(&f->bus->current_devs));
    stop_bytes(f);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmpuint(a->stops, ==, 0);
    g_assert_cmpuint(a->restarts, ==, 1);
    g_assert_cmpuint(a->writes, ==, 1);
}

static uint8_t read_byte(Fixture *f, bool ack_a, bool ack_b)
{
    int64_t low = virtual_time;
    bool acks[] = { ack_a, ack_b };
    uint8_t value = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        for (unsigned i = 0; i < ARRAY_SIZE(acks); i++) {
            if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
                g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 1,
                                              true, true), ==, 0);
            }
        }
        advance_to(low + 5);
        value = (value << 1) | i2c_wire_sda(f->bus);
        advance_to(low + 10);
        low += 10;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(acks); i++) {
        if (i2c_wire_contenders(f->bus) & (1ULL << i)) {
            g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 1, !acks[i], true),
                            ==, 0);
        }
    }
    advance_to(low + 5);
    advance_to(low + 10);
    return value;
}

static void test_read_ack_arbitration(Fixture *f, gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    unsigned losses = 0;

    a->value = 0xa5;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    g_assert_cmpuint(a->reads, ==, 1);
    g_assert_cmphex(read_byte(f, true, false), ==, 0xa5);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 1);
    g_assert_cmpuint(a->nacks, ==, 0);
    g_assert_cmpuint(a->reads, ==, 2);
    for (unsigned i = 0; i < f->events->len; i++) {
        const I2CWireEvent *e = &g_array_index(f->events, I2CWireEvent, i);

        g_assert_cmphex(e->lost & 1, ==, 0);
        losses += e->lost == 2;
    }
    g_assert_cmpuint(losses, ==, 1);
    g_assert_cmphex(read_byte(f, false, false), ==, 0xa6);
    g_assert_cmpuint(a->nacks, ==, 1);
    g_assert_cmpuint(a->reads, ==, 2);
    stop_bytes(f);
    g_assert_cmpuint(a->stops, ==, 1);
}

/*
 * A master receiver that acknowledges a byte and then tries to STOP meets
 * the slave already sending the next one. A zero first bit keeps SDA low, so
 * no STOP forms and the bus stays busy until the slave lets go.
 */
static void test_release_slave_completes_stop(Fixture *f,
                                              gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    const I2CWireEvent *last;

    a->value = 0x21;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    g_assert_cmphex(read_byte(f, true, true), ==, 0x21);
    stop_bytes(f);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_true(i2c_wire_scl(f->bus));
    g_assert_false(i2c_wire_sda(f->bus));

    g_assert_cmpint(i2c_wire_release_slave(f->a), ==, 0);
    last = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_true(last->scl && last->sda);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 0);
    g_assert_cmpuint(a->reads, ==, 2);
    g_assert_cmpint(i2c_wire_release_slave(f->a), ==, 0);
}

typedef struct ReleasingObserver {
    I2CSlave *slave;
    int64_t from;
    unsigned releases;
    int64_t released_at;
} ReleasingObserver;

static void release_when_stop_blocked(void *opaque, const I2CWireEvent *event)
{
    ReleasingObserver *observer = opaque;

    if (observer->from && event->time >= observer->from &&
        event->scl && !event->sda) {
        observer->from = 0;
        observer->releases++;
        observer->released_at = event->time;
        g_assert_cmpint(i2c_wire_release_slave(observer->slave), ==, 0);
    }
}

/*
 * A controller may switch its interface off from an observer while the wire
 * is publishing. The lines then settle at the same instant once the
 * publication finishes, so the blocked STOP still forms.
 */
static void test_release_slave_from_observer(Fixture *f, gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    ReleasingObserver observer = { .slave = f->a };
    const I2CWireEvent *last;

    g_assert_cmpint(i2c_wire_set_observer(f->bus, 1,
                                          release_when_stop_blocked,
                                          &observer), ==, 0);
    a->value = 0x21;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    g_assert_cmphex(read_byte(f, true, true), ==, 0x21);
    stop_bytes(f);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_false(i2c_wire_sda(f->bus));

    /*
     * A blocked STOP publishes nothing, so let the slave stretch the clock
     * once: SCL rising again is the event the observer acts on.
     */
    g_assert_cmpint(i2c_wire_hold(f->a, virtual_time + 1, true), ==, 0);
    advance_to(virtual_time + 1);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpint(i2c_wire_hold(f->a, virtual_time + 1, false), ==, 0);
    observer.from = virtual_time + 1;
    advance_to(virtual_time + 1);
    g_assert_cmpuint(observer.releases, ==, 1);
    last = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_true(last->scl && last->sda);
    g_assert_cmpint(last->time, ==, observer.released_at);
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 0);
}

static void test_address_read_service_holds_preparation(Fixture *f,
                                                        gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    int64_t completed;

    a->value = 0xa5;
    a->hold_service = I2C_WIRE_ADDRESS_RECV;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    completed = virtual_time;
    g_assert_cmpuint(a->services[I2C_WIRE_ADDRESS_RECV], ==, 1);
    g_assert_cmpuint(a->reads_at_service[I2C_WIRE_ADDRESS_RECV], ==, 0);
    g_assert_cmpuint(a->reads, ==, 0);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_hold);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpint(i2c_wire_hold(f->a, completed + 6, false), ==, 0);
    advance_to(completed + 5);
    g_assert_cmpuint(a->reads, ==, 0);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(completed + 6);
    g_assert_cmpuint(a->reads, ==, 1);
    g_assert_true(i2c_wire_scl(f->bus));
    g_assert_cmphex(i2c_wire_sda(f->bus), ==, 1);
}

static void test_read_ack_service_holds_preparation(Fixture *f,
                                                    gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    int64_t completed;

    a->value = 0xa5;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    g_assert_cmpuint(a->reads, ==, 1);
    a->hold_service = I2C_WIRE_READ_ACK;
    g_assert_cmphex(read_byte(f, true, true), ==, 0xa5);
    completed = virtual_time;
    g_assert_cmpuint(a->services[I2C_WIRE_READ_ACK], ==, 1);
    g_assert_cmpuint(a->reads_at_service[I2C_WIRE_READ_ACK], ==, 1);
    g_assert_cmpuint(a->reads, ==, 1);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_hold);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpint(i2c_wire_hold(f->a, completed + 6, false), ==, 0);
    advance_to(completed + 6);
    g_assert_cmpuint(a->reads, ==, 2);
    g_assert_true(i2c_wire_scl(f->bus));
}

static void test_unsupported_stop_mix(Fixture *f, gconstpointer unused)
{
    int64_t high;
    const I2CWireEvent *event;

    begin_bytes(f);
    stage_pair(f, virtual_time + 10, true, false);
    advance_to(virtual_time + 15);
    high = virtual_time;
    g_assert_true(i2c_wire_scl(f->bus));
    /* One participant attempts STOP while its peer continues the data bit. */
    g_assert_cmpint(i2c_wire_stage(f->bus, 1, high + 1, true, false), ==, 0);
    advance_to(high + 1);
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, -ENOTSUP);
    g_assert_cmphex(event->lost, ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, high + 2, true, false),
                    ==, -ENOTSUP);
}

static void test_deadline_overflow(Fixture *f, gconstpointer unused)
{
    const I2CWireEvent *event;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }
    cpus_set_virtual_clock(INT64_MAX - 50);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(i2c_wire_configure(f->bus, i, 9, 9, false), ==, 0);
    }
    stage_pair(f, INT64_MAX - 40, false, false);
    advance_to(INT64_MAX - 40);
    stage_pair(f, INT64_MAX - 30, true, false);
    advance_to(INT64_MAX - 3);
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, -ERANGE);
    g_assert_cmphex(event->lost, ==, 0);
    g_assert_true(i2c_bus_busy(f->bus));
}

static void test_deadline_exact_max(Fixture *f, gconstpointer unused)
{
    const I2CWireEvent *event;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }
    cpus_set_virtual_clock(INT64_MAX - 30);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(i2c_wire_configure(f->bus, i, 9, 9, false), ==, 0);
    }
    stage_pair(f, INT64_MAX - 20, false, false);
    advance_to(INT64_MAX - 20);
    stage_pair(f, INT64_MAX - 9, true, false);
    advance_to(INT64_MAX - 9);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(INT64_MAX);
    g_assert_true(i2c_wire_scl(f->bus));
    event = &g_array_index(f->events, I2CWireEvent, f->events->len - 1);
    g_assert_cmpint(event->error, ==, -ERANGE);
}

/*
 * A controller that waits for its host between bytes stops clocking without
 * letting SCL go: the low phase then lasts as long as the host takes.
 */
static void test_pause_holds_clock(Fixture *f, gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    int64_t paused_at;

    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0xff, true));
    g_assert_cmpuint(a->starts, ==, 1);
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 1);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 0), ==, 0);
    paused_at = virtual_time;
    advance_to(paused_at + 1000);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 1);
    /* Resuming starts the next bit's high phase from the resume instant. */
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, virtual_time + 1, true, true),
                    ==, 0);
    advance_to(virtual_time + 1);
    g_assert_true(i2c_wire_scl(f->bus));
    advance_to(virtual_time + 5);
    g_assert_false(i2c_wire_scl(f->bus));
}

static void test_pause_rejects(Fixture *f, gconstpointer unused)
{
    begin_bytes(f);
    /* Not clocking, and no such participant. */
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 0), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 2), ==, -EINVAL);
    g_assert_true(write_byte(f, 0x40, 0xff, true));
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 0), ==, 0);
    /* Already paused: the clock generator is stopped. */
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 0), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_stage(f->bus, 0, virtual_time + 1, true, true),
                    ==, 0);
    /* A staged request has not settled onto the line yet. */
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 0), ==, -EINVAL);
    advance_to(virtual_time + 1);
    g_assert_true(i2c_wire_scl(f->bus));
    /* Pausing on a high phase would drop a line the caller still owns. */
    g_assert_cmpint(i2c_wire_pause_master(f->bus, 0), ==, -EINVAL);
}

/*
 * Address observation is not selection: every endpoint sees the completed
 * byte, and an unmatched address selects nobody while still being seen.
 */
static void test_address_observation(Fixture *f, gconstpointer unused)
{
    TestSlave *a = (TestSlave *)f->a;
    TestSlave *b = (TestSlave *)f->b;

    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    g_assert_cmpuint(a->addresses, ==, 1);
    g_assert_cmpuint(b->addresses, ==, 1);
    g_assert_cmphex(a->last_address, ==, 0x40);
    g_assert_cmphex(b->last_address, ==, 0x40);
    g_assert_cmpuint(a->starts, ==, 1);
    g_assert_cmpuint(b->starts, ==, 0);
    /* A data byte is not an address phase. */
    g_assert_true(write_byte(f, 0xa5, 0xa5, false));
    g_assert_cmpuint(a->addresses, ==, 1);
    g_assert_cmpuint(b->addresses, ==, 1);
    stop_bytes(f);

    /* An address nobody answers still reaches both endpoints. */
    stage_pair(f, virtual_time + 10, false, false);
    advance_to(virtual_time + 10);
    g_assert_false(write_byte(f, 0xe0, 0xe0, true));
    g_assert_cmpuint(a->addresses, ==, 2);
    g_assert_cmpuint(b->addresses, ==, 2);
    g_assert_cmphex(a->last_address, ==, 0xe0);
    g_assert_cmphex(b->last_address, ==, 0xe0);
    g_assert_cmpuint(a->starts, ==, 1);
    g_assert_cmpuint(b->starts, ==, 0);
}

typedef struct MasterObserver {
    unsigned count;
    I2CWireEvent last;
} MasterObserver;

static void master_observe(void *opaque, const I2CWireEvent *event)
{
    MasterObserver *observer = opaque;

    observer->count++;
    observer->last = *event;
}

static void test_master_observers(Fixture *f, gconstpointer unused)
{
    MasterObserver zero = { 0 }, one = { 0 };
    const I2CWireEvent *last;
    unsigned seen;

    g_assert_cmpint(i2c_wire_set_observer(f->bus, 0, master_observe, &zero),
                    ==, 0);
    g_assert_cmpint(i2c_wire_set_observer(f->bus, 0, master_observe, &one),
                    ==, -EBUSY);
    g_assert_cmpint(i2c_wire_set_observer(f->bus, 2, master_observe, &one),
                    ==, -EINVAL);
    g_assert_cmpint(i2c_wire_set_observer(f->bus, 1, NULL, &one), ==, -EINVAL);
    g_assert_cmpint(i2c_wire_set_observer(f->bus, 1, master_observe, &one),
                    ==, 0);

    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0xff, true));
    seen = f->events->len;
    g_assert_cmpuint(seen, >, 0);
    g_assert_cmpuint(zero.count, ==, seen);
    g_assert_cmpuint(one.count, ==, seen);
    last = &g_array_index(f->events, I2CWireEvent, seen - 1);
    g_assert_cmpint(zero.last.time, ==, last->time);
    g_assert_cmpint(zero.last.scl, ==, last->scl);
    g_assert_cmpint(zero.last.sda, ==, last->sda);
    g_assert_cmphex(one.last.contenders, ==, last->contenders);

    g_assert_cmpint(i2c_wire_clear_observer(f->bus, 0, master_observe, &one),
                    ==, -ENOENT);
    g_assert_cmpint(i2c_wire_clear_observer(f->bus, 2, master_observe, &zero),
                    ==, -EINVAL);
    g_assert_cmpint(i2c_wire_clear_observer(f->bus, 0, master_observe, &zero),
                    ==, 0);
    g_assert_true(write_byte(f, 0xa5, 0xa5, false));
    g_assert_cmpuint(zero.count, ==, seen);
    g_assert_cmpuint(one.count, ==, f->events->len);
}

static QEMUFile *state_file(int fd, bool save)
{
    QIOChannelFile *ioc;
    QEMUFile *file;

    g_assert_cmpint(lseek(fd, 0, SEEK_SET), ==, 0);
    ioc = qio_channel_file_new_fd(dup(fd));
    file = save ? qemu_file_new_output(QIO_CHANNEL(ioc)) :
                  qemu_file_new_input(QIO_CHANNEL(ioc));
    object_unref(OBJECT(ioc));
    return file;
}

/*
 * The bus subclass carries masters, wire and every slave's wire state in one
 * section, so a wire round-trip saves and loads only that.
 */
static void save_wire(int fd, Fixture *f)
{
    QEMUFile *file = state_file(fd, true);

    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
}

static void load_wire(int fd, Fixture *f)
{
    QEMUFile *file = state_file(fd, false);

    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      1, &error_abort), ==, 0);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
}

enum {
    WIRE_MASTER_STATE_SIZE = 4 * sizeof(uint64_t) + 9,
    WIRE_CONTENDERS_OFFSET = I2C_WIRE_MAX_MASTERS * WIRE_MASTER_STATE_SIZE,
    WIRE_OCCUPIED_OFFSET = WIRE_CONTENDERS_OFFSET + sizeof(uint64_t) + 2,
    WIRE_ADDRESS_BYTE_OFFSET = WIRE_OCCUPIED_OFFSET + 2,
    WIRE_BITS_OFFSET = WIRE_ADDRESS_BYTE_OFFSET + 3,
    WIRE_BYTE_OFFSET = WIRE_BITS_OFFSET + 1,
};

static off_t wire_payload_offset(int fd)
{
    static const char subsection[] = "sgi-i2c-wire-bus/wire";
    g_autofree uint8_t *data = NULL;
    off_t size = lseek(fd, 0, SEEK_END);

    g_assert_cmpint(size, >, 0);
    data = g_malloc(size);
    g_assert_cmpint(pread(fd, data, size, 0), ==, size);
    for (off_t i = 2; i + sizeof(subsection) - 1 <= size; i++) {
        if (data[i - 2] == QEMU_VM_SUBSECTION &&
            data[i - 1] == sizeof(subsection) - 1 &&
            !memcmp(data + i, subsection, sizeof(subsection) - 1)) {
            return i + sizeof(subsection) - 1 + sizeof(uint32_t);
        }
    }
    g_assert_not_reached();
}

static void patch_state_byte(int fd, off_t payload, off_t field,
                             uint8_t value)
{
    g_assert_cmpint(pwrite(fd, &value, sizeof(value), payload + field), ==,
                    sizeof(value));
}

static void sample_first_address_bit(Fixture *f)
{
    int64_t low;

    begin_bytes(f);
    low = virtual_time + 10;
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(i2c_wire_stage(f->bus, i, low, false, true), ==, 0);
    }
    advance_to(low + 5);
    advance_to(low + 10);
}

static void test_migration(Fixture *f, gconstpointer held)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-XXXXXX", &path, NULL);
    int64_t start = virtual_time;
    g_autoptr(GArray) expected =
        g_array_new(false, false, sizeof(I2CWireEvent));

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    stage_pair(f, start + 10, false, false);
    advance_to(start + 10);
    stage_pair(f, start + 20, true, false);
    advance_to(start + 20);
    if (held) {
        g_assert_cmpint(i2c_wire_hold(f->a, start + 21, true), ==, 0);
        g_assert_cmpint(i2c_wire_hold(f->b, start + 21, true), ==, 0);
    }
    advance_to(start + 22);
    if (held) {
        g_assert_cmpint(i2c_wire_hold(f->a, start + 30, false), ==, 0);
        g_assert_cmpint(i2c_wire_hold(f->b, start + 40, false), ==, 0);
    }
    advance_to(start + (held ? 35 : 22));
    save_wire(fd, f);
    g_array_set_size(f->events, 0);
    advance_to(start + 45);
    g_array_append_vals(expected, f->events->data, f->events->len);
    bus_cold_reset(BUS(f->bus));
    g_assert_false(i2c_bus_busy(f->bus));
    g_assert_true(i2c_wire_scl(f->bus));
    g_assert_true(i2c_wire_sda(f->bus));
    g_assert_cmphex(i2c_wire_contenders(f->bus), ==, 0);
    /* The full VM restores virtual time separately from the device stream. */
    cpus_set_virtual_clock(start + (held ? 35 : 22));
    load_wire(fd, f);
    g_assert_true(i2c_bus_busy(f->bus));
    g_assert_false(i2c_wire_scl(f->bus));
    g_array_set_size(f->events, 0);
    advance_to(start + 45);
    g_assert_cmpuint(f->events->len, ==, expected->len);
    for (unsigned i = 0; i < expected->len; i++) {
        const I2CWireEvent *a = &g_array_index(expected, I2CWireEvent, i);
        const I2CWireEvent *b = &g_array_index(f->events, I2CWireEvent, i);

        g_assert_cmpint(a->time, ==, b->time);
        g_assert_cmpint(a->scl, ==, b->scl);
        g_assert_cmpint(a->sda, ==, b->sda);
        g_assert_cmphex(a->contenders, ==, b->contenders);
        g_assert_cmphex(a->lost, ==, b->lost);
        g_assert_cmpint(a->error, ==, b->error);
    }
    close(fd);
}

static void test_migration_prepared_read(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-read-XXXXXX", &path, NULL);
    TestSlave *a = (TestSlave *)f->a;
    int64_t saved_time;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    a->value = 0xa5;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    g_assert_cmpuint(a->reads, ==, 1);
    saved_time = virtual_time;
    save_wire(fd, f);

    a->value = 0x5a;
    a->reads = a->nacks = 0;
    bus_cold_reset(BUS(f->bus));
    cpus_set_virtual_clock(saved_time);
    load_wire(fd, f);
    g_assert_cmphex(read_byte(f, false, false), ==, 0xa5);
    g_assert_cmpuint(a->reads, ==, 0);
    g_assert_cmpuint(a->nacks, ==, 1);
    close(fd);
}

static void test_migration_service_hold(Fixture *f, gconstpointer reverse)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-service-XXXXXX", &path, NULL);
    TestSlave *a = (TestSlave *)f->a;
    TestSlave *b = (TestSlave *)f->b;
    I2CSlave *first = reverse ? f->b : f->a;
    I2CSlave *second = reverse ? f->a : f->b;
    int64_t completed, saved_time;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    a->hold_service = b->hold_service = I2C_WIRE_WRITE;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x00, 0x00, true));
    g_assert_true(write_byte(f, 0x5a, 0x5a, false));
    completed = virtual_time;
    g_assert_cmpint(i2c_wire_hold(first, completed + 6, false), ==, 0);
    advance_to(completed + 6);
    g_assert_false(i2c_wire_scl(f->bus));
    g_assert_false(i2c_wire_slave_state(first)->wire_hold);
    g_assert_cmphex(i2c_wire_slave_state(first)->wire_hold_phase, ==, 0);
    g_assert_true(i2c_wire_slave_state(second)->wire_hold);
    g_assert_cmphex(i2c_wire_slave_state(second)->wire_hold_phase, !=, 0);
    g_assert_cmpint(i2c_wire_hold(second, completed + 10, false), ==, 0);
    saved_time = virtual_time;
    save_wire(fd, f);

    a->services[I2C_WIRE_WRITE] = b->services[I2C_WIRE_WRITE] = 0;
    bus_cold_reset(BUS(f->bus));
    cpus_set_virtual_clock(saved_time);
    load_wire(fd, f);
    g_assert_false(i2c_wire_slave_state(first)->wire_hold);
    g_assert_cmphex(i2c_wire_slave_state(first)->wire_hold_phase, ==, 0);
    g_assert_true(i2c_wire_slave_state(second)->wire_hold);
    g_assert_cmphex(i2c_wire_slave_state(second)->wire_hold_phase, !=, 0);
    g_assert_false(i2c_wire_scl(f->bus));
    advance_to(completed + 10);
    g_assert_true(i2c_wire_scl(f->bus));
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 0);
    g_assert_cmpuint(b->services[I2C_WIRE_WRITE], ==, 0);
    close(fd);
}

static void test_migration_deferred_read(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-deferred-XXXXXX", &path, NULL);
    TestSlave *a = (TestSlave *)f->a;
    int64_t completed, saved_time;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    a->value = 0xa5;
    a->hold_service = I2C_WIRE_ADDRESS_RECV;
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x41, 0x41, true));
    completed = saved_time = virtual_time;
    g_assert_cmpuint(a->reads, ==, 0);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_hold);
    save_wire(fd, f);

    a->reads = 0;
    a->services[I2C_WIRE_ADDRESS_RECV] = 0;
    bus_cold_reset(BUS(f->bus));
    cpus_set_virtual_clock(saved_time);
    load_wire(fd, f);
    g_assert_cmpuint(a->reads, ==, 0);
    g_assert_cmpuint(a->services[I2C_WIRE_ADDRESS_RECV], ==, 0);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_hold);
    g_assert_cmpint(i2c_wire_hold(f->a, completed + 6, false), ==, 0);
    advance_to(completed + 6);
    g_assert_cmpuint(a->reads, ==, 1);
    g_assert_cmpuint(a->services[I2C_WIRE_ADDRESS_RECV], ==, 0);
    g_assert_true(i2c_wire_scl(f->bus));
    close(fd);
}

static void test_migration_pending_service(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-pending-XXXXXX", &path, NULL);
    TestSlave *a = (TestSlave *)f->a;
    int64_t saved_time;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    write_byte_to_ack(f, 0xa5, 0xa5);
    g_assert_cmpuint(a->writes, ==, 1);
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 0);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_service_pending);
    saved_time = virtual_time;
    save_wire(fd, f);

    a->services[I2C_WIRE_WRITE] = 0;
    bus_cold_reset(BUS(f->bus));
    cpus_set_virtual_clock(saved_time);
    load_wire(fd, f);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_service_pending);
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 0);
    finish_ack(f);
    g_assert_false(i2c_wire_slave_state(f->a)->wire_service_pending);
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 1);
    advance_to(virtual_time + 10);
    g_assert_cmpuint(a->services[I2C_WIRE_WRITE], ==, 1);
    close(fd);
}

static void test_migration_rejects_bad_phase(Fixture *f,
                                             gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-phase-XXXXXX", &path, NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    write_byte_to_ack(f, 0xa5, 0xa5);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_service_pending);
    i2c_wire_slave_state(f->a)->wire_service_phase++;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_line_mismatch(Fixture *f,
                                                 gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-lines-XXXXXX", &path, NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    i2c_wire_slave_state(f->a)->wire_hold = true;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_idle_address(Fixture *f,
                                                gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-idle-address-XXXXXX", &path, NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    I2C_WIRE(f->bus)->address_valid = true;
    I2C_WIRE(f->bus)->requested_address = 0x20;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                       1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_read_target_ack(Fixture *f,
                                                   gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-read-ack-XXXXXX", &path, NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    write_byte_to_ack(f, 0xa5, 0xa5);
    g_assert_true(i2c_wire_slave_state(f->a)->wire_service_pending);
    I2C_WIRE(f->bus)->master_recv = true;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                       1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_stale_hold_phase(Fixture *f,
                                                    gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-hold-phase-XXXXXX", &path, NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    write_byte_to_ack(f, 0xa5, 0xa5);
    i2c_wire_slave_state(f->a)->wire_service_pending = false;
    i2c_wire_slave_state(f->a)->wire_service_phase = 0;
    i2c_wire_slave_state(f->a)->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
    i2c_wire_slave_state(f->a)->wire_hold = true;
    i2c_wire_slave_state(f->a)->wire_hold_phase = 1;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_stale_service_phase(Fixture *f,
                                                       gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-service-phase-XXXXXX", &path,
                             NULL);
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    i2c_wire_slave_state(f->a)->wire_service_phase = 1;
    i2c_wire_slave_state(f->a)->wire_service_kind = I2C_WIRE_WRITE;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_bad_byte_position(Fixture *f,
                                                     gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-byte-XXXXXX", &path, NULL);
    QEMUFile *file;
    off_t payload;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    sample_first_address_bit(f);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    payload = wire_payload_offset(fd);
    patch_state_byte(fd, payload, WIRE_BYTE_OFFSET, 2);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                       1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_idle_bit_state(Fixture *f,
                                                  gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    uint64_t no_contenders = 0;
    int fd = g_file_open_tmp("test-i2c-wire-idle-bits-XXXXXX", &path, NULL);
    QEMUFile *file;
    off_t payload;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    sample_first_address_bit(f);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    payload = wire_payload_offset(fd);
    g_assert_cmpint(pwrite(fd, &no_contenders, sizeof(no_contenders),
                           payload + WIRE_CONTENDERS_OFFSET), ==,
                    sizeof(no_contenders));
    patch_state_byte(fd, payload, WIRE_OCCUPIED_OFFSET, false);
    patch_state_byte(fd, payload, WIRE_ADDRESS_BYTE_OFFSET, false);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                       1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

static void test_migration_rejects_mid_byte_service_hold(
    Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    int fd = g_file_open_tmp("test-i2c-wire-mid-byte-hold-XXXXXX", &path,
                             NULL);
    QEMUFile *file;
    int64_t low;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    begin_bytes(f);
    g_assert_true(write_byte(f, 0x40, 0x40, true));
    low = virtual_time;
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpint(i2c_wire_stage(f->bus, i, low + 1, true, true), ==,
                        0);
    }
    advance_to(low + 5);
    advance_to(low + 10);
    i2c_wire_slave_state(f->a)->wire_hold = true;
    i2c_wire_slave_state(f->a)->wire_hold_phase = 1;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    bus_cold_reset(BUS(f->bus));
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, f->bus,
                                      1, &local_err), ==, -EINVAL);
    error_free(local_err);
    qemu_fclose(file);
    close(fd);
}

/*
 * The attach-time refusal cannot see a slave that joined before the wire, so
 * enabling one over a slave that carries no migration state fails too, and
 * fails before it has changed anything about the bus.
 */
static void test_enable_refuses_a_mute_slave(void)
{
    DeviceState *host = qdev_new(TYPE_WIRE_HOST);
    Error *err = NULL;
    I2CBus *bus;

    qdev_realize(host, NULL, &error_abort);
    bus = i2c_init_bus_type(TYPE_I2C_WIRE, host, "i2c");
    i2c_slave_create_simple(bus, TYPE_WIRE_MUTE, 0x20);

    g_assert_cmpint(i2c_bus_enable_wire(bus, 2, NULL, NULL, &err), !=, 0);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), TYPE_WIRE_MUTE));
    error_free(err);

    /*
     * Refused before it changed anything. A bus left holding participants it
     * cannot use would abort the first legacy transfer anybody attempted.
     */
    g_assert_null(I2C_WIRE(bus)->wire);
    g_assert_cmpuint(I2C_WIRE(bus)->master_count, ==, 0);

    object_unparent(OBJECT(host));
    object_unref(OBJECT(host));
}

int main(int argc, char **argv)
{
    const TypeInfo host = { .name = TYPE_WIRE_HOST, .parent = TYPE_DEVICE };
    const TypeInfo slave = {
        .name = TYPE_WIRE_SLAVE, .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(TestSlave), .class_init = slave_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_I2C_WIRE_ENDPOINT },
            { }
        },
    };
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    module_call_init(MODULE_INIT_QOM);
    const TypeInfo mute = {
        .name = TYPE_WIRE_MUTE, .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(I2CSlave),
    };

    type_register_static(&host);
    type_register_static(&slave);
    type_register_static(&mute);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    g_test_add_func("/i2c-wire/enable-refuses-mute-slave",
                    test_enable_refuses_a_mute_slave);
    g_test_add("/i2c-wire/configuration", Fixture, NULL,
               setup, test_invalid_configuration, teardown);
    g_test_add("/i2c-wire/clocks", Fixture, NULL,
               setup, test_clock_synchronization, teardown);
    g_test_add("/i2c-wire/clocks-reversed", Fixture, GINT_TO_POINTER(1),
               setup, test_clock_synchronization, teardown);
    g_test_add("/i2c-wire/timeline/late-timer", Fixture, NULL,
               setup, test_late_timer_keeps_instants, teardown);
    g_test_add("/i2c-wire/timeline/participant", Fixture, NULL,
               setup, test_participant_timeline, teardown);
    g_test_add("/i2c-wire/timeline/late-timer-after-advance", Fixture, NULL,
               setup, test_late_timer_after_advance, teardown);
    g_test_add("/i2c-wire/timeline/attachment", Fixture, NULL,
               setup, test_timeline_attachment, teardown);
    g_test_add("/i2c-wire/arbitration", Fixture, NULL,
               setup, test_arbitration, teardown);
    g_test_add("/i2c-wire/arbitration-reversed", Fixture,
               GUINT_TO_POINTER(2), setup, test_arbitration, teardown);
    g_test_add("/i2c-wire/loser-clocks", Fixture, GINT_TO_POINTER(1),
               setup, test_arbitration, teardown);
    g_test_add("/i2c-wire/holds", Fixture, NULL,
               setup, test_independent_holds, teardown);
    g_test_add("/i2c-wire/holds-reversed", Fixture, GINT_TO_POINTER(1),
               setup, test_independent_holds, teardown);
    g_test_add("/i2c-wire/clock-admission-low", Fixture, NULL,
               setup, test_clock_admission_while_low, teardown);
    g_test_add("/i2c-wire/contender-clock-admission-low", Fixture, NULL,
               setup, test_contender_clock_admission_while_low, teardown);
    g_test_add("/i2c-wire/contender-clock-admission-deadline", Fixture,
               GUINT_TO_POINTER(1), setup,
               test_contender_clock_admission_at_deadline, teardown);
    g_test_add("/i2c-wire/contender-clock-admission-deadline-reversed",
               Fixture, GUINT_TO_POINTER(0), setup,
               test_contender_clock_admission_at_deadline, teardown);
    g_test_add("/i2c-wire/contender-clock-admission-preserves-error",
               Fixture, NULL, setup_three,
               test_contender_clock_admission_preserves_error, teardown);
    g_test_add("/i2c-wire/inactive-reset", Fixture, NULL,
               setup, test_inactive_reset, teardown);
    g_test_add("/i2c-wire/migration-low", Fixture, NULL,
               setup, test_migration, teardown);
    g_test_add("/i2c-wire/migration-hold", Fixture, GINT_TO_POINTER(1),
               setup, test_migration, teardown);
    g_test_add("/i2c-wire/migration-prepared-read", Fixture, NULL,
               setup, test_migration_prepared_read, teardown);
    g_test_add("/i2c-wire/migration-service-hold", Fixture, NULL,
               setup, test_migration_service_hold, teardown);
    g_test_add("/i2c-wire/migration-service-hold-reversed", Fixture,
               GINT_TO_POINTER(1), setup, test_migration_service_hold,
               teardown);
    g_test_add("/i2c-wire/migration-deferred-read", Fixture, NULL,
               setup, test_migration_deferred_read, teardown);
    g_test_add("/i2c-wire/migration-pending-service", Fixture, NULL,
               setup, test_migration_pending_service, teardown);
    g_test_add("/i2c-wire/migration-rejects-bad-phase", Fixture, NULL,
               setup, test_migration_rejects_bad_phase, teardown);
    g_test_add("/i2c-wire/migration-rejects-line-mismatch", Fixture, NULL,
               setup, test_migration_rejects_line_mismatch, teardown);
    g_test_add("/i2c-wire/migration-rejects-idle-address", Fixture, NULL,
               setup, test_migration_rejects_idle_address, teardown);
    g_test_add("/i2c-wire/migration-rejects-read-target-ack", Fixture, NULL,
               setup, test_migration_rejects_read_target_ack, teardown);
    g_test_add("/i2c-wire/migration-rejects-stale-hold-phase", Fixture, NULL,
               setup, test_migration_rejects_stale_hold_phase, teardown);
    g_test_add("/i2c-wire/migration-rejects-stale-service-phase", Fixture,
               NULL, setup, test_migration_rejects_stale_service_phase,
               teardown);
    g_test_add("/i2c-wire/migration-rejects-bad-byte-position", Fixture,
               NULL, setup, test_migration_rejects_bad_byte_position,
               teardown);
    g_test_add("/i2c-wire/migration-rejects-idle-bit-state", Fixture, NULL,
               setup, test_migration_rejects_idle_bit_state, teardown);
    g_test_add("/i2c-wire/migration-rejects-mid-byte-service-hold", Fixture,
               NULL, setup, test_migration_rejects_mid_byte_service_hold,
               teardown);
    g_test_add("/i2c-wire/identical-write", Fixture, NULL,
               setup, test_endpoint_write, teardown);
    g_test_add("/i2c-wire/arbitrated-write", Fixture, GINT_TO_POINTER(1),
               setup, test_endpoint_write, teardown);
    g_test_add("/i2c-wire/post-ack-service", Fixture, NULL,
               setup, test_post_ack_service, teardown);
    g_test_add("/i2c-wire/post-ack-service-reversed", Fixture,
               GINT_TO_POINTER(1), setup, test_post_ack_service, teardown);
    g_test_add("/i2c-wire/pause-holds-clock", Fixture, NULL,
               setup, test_pause_holds_clock, teardown);
    g_test_add("/i2c-wire/pause-rejects", Fixture, NULL,
               setup, test_pause_rejects, teardown);
    g_test_add("/i2c-wire/address-observation", Fixture, NULL,
               setup, test_address_observation, teardown);
    g_test_add("/i2c-wire/master-observers", Fixture, NULL,
               setup, test_master_observers, teardown);
    g_test_add("/i2c-wire/restart-nack", Fixture, NULL,
               setup, test_restart_nack, teardown);
    g_test_add("/i2c-wire/read-ack-arbitration", Fixture, NULL,
               setup, test_read_ack_arbitration, teardown);
    g_test_add("/i2c-wire/release-slave-completes-stop", Fixture, NULL,
               setup, test_release_slave_completes_stop, teardown);
    g_test_add("/i2c-wire/release-slave-from-observer", Fixture, NULL,
               setup, test_release_slave_from_observer, teardown);
    g_test_add("/i2c-wire/address-read-service-holds-preparation", Fixture,
               NULL, setup, test_address_read_service_holds_preparation,
               teardown);
    g_test_add("/i2c-wire/read-ack-service-holds-preparation", Fixture, NULL,
               setup, test_read_ack_service_holds_preparation, teardown);
    g_test_add("/i2c-wire/unsupported-stop-mix", Fixture, NULL,
               setup, test_unsupported_stop_mix, teardown);
    g_test_add("/i2c-wire/deadline-overflow", Fixture, NULL,
               setup, test_deadline_overflow, teardown);
    g_test_add("/i2c-wire/deadline-exact-max", Fixture, NULL,
               setup, test_deadline_exact_max, teardown);
    return g_test_run();
}
