/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define TWO_NODE_TOPOLOGY \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

#define TWO_NODE_MACHINE \
    "-M origin200 " TWO_NODE_TOPOLOGY \
    " -accel tcg,thread=multi -no-reboot -no-shutdown"
#define TWO_NODE_RECORDING_MACHINE \
    "-M origin200,diagnostic-recording=on " TWO_NODE_TOPOLOGY

#define RECORDING_MACHINE "-M origin200,diagnostic-recording=on"

#define SPACE_XTALK 0x20000000000ULL
#define HUB_LOCAL (SPACE_XTALK + 0x01000000)
#define PI_REGION_PRESENT 0x000018
#define PI_RT_COUNT 0x030100
#define PI_RT_COMPARE_A 0x000108
#define PI_INT_MODIFY 0x000090
#define PI_INT_PEND0 0x000098
#define PI_INT_MASK0_A 0x0000a8
#define INT_MODIFY_SET 0x100
#define NI_STATUS 0x600000
#define NI_STATUS_NODE_SHIFT 8
#define NI_VECTOR_PARMS 0x600200
#define NI_VECTOR 0x600208
#define NI_VECTOR_DATA 0x600210
#define NI_VECTOR_STATUS 0x600300
#define NI_VECTOR_CLEAR 0x600380
#define NI_VECTOR_VALID (1ULL << 63)
#define NI_VECTOR_WRITE 1
#define I2C_DATA 0x220000
#define I2C_CTRL 0x220008
#define CTL_PIN 0x80
#define CTL_ESO 0x40
#define CTL_STA 0x04
#define CTL_STO 0x02
#define CTL_ACK 0x01
#define ELSC_ADDR 0x08
#define RESPONSE_CAPACITY 96

#define NODE_STRIDE 0x100000000ULL
#define HUB_BY_NODE (SPACE_XTALK + 0x01000000)

#define ELF_CODE_OFFSET 0x1000
#define ELF_CODE_SIZE 0x200
#define KERNEL_ENTRY 0xffffffff80010000ULL
#define SECONDARY_ENTRY (KERNEL_ENTRY + 0x100)
#define STORE_PC (KERNEL_ENTRY + 0x118)
#define POLL_PC (KERNEL_ENTRY + 0x118)

typedef struct GuestVisibleState {
    uint64_t region_present;
    uint64_t interrupt_pending;
    uint64_t interrupt_mask;
    uint64_t i2c_status;
    uint8_t response[RESPONSE_CAPACITY];
    unsigned int response_length;
} GuestVisibleState;

static QDict *query_origin_state_limit(QTestState *qts,
                                       unsigned int event_limit)
{
    QDict *response = qtest_qmp(
        qts, "{'execute': 'x-query-origin-state', "
             "'arguments': {'event-limit': %u}}", event_limit);
    QDict *state;

    g_assert(qdict_haskey(response, "return"));
    state = qobject_ref(qdict_get_qdict(response, "return"));
    qobject_unref(response);
    return state;
}

static QDict *query_origin_state(QTestState *qts)
{
    return query_origin_state_limit(qts, 4096);
}

static void assert_running(QTestState *qts)
{
    QDict *response = qtest_qmp(qts, "{'execute': 'query-status'}");
    QDict *status = qdict_get_qdict(response, "return");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "running");
    qobject_unref(response);
}

static QList *flight_events(QDict *state)
{
    return qdict_get_qlist(qdict_get_qdict(state, "flight-recorder"),
                           "events");
}

static QDict *find_event(QList *events, const char *component,
                         const char *operation, uint64_t address,
                         uint64_t value)
{
    const QListEntry *entry;

    for (entry = qlist_first(events); entry; entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(event, "component"), component) &&
            !strcmp(qdict_get_str(event, "operation"), operation) &&
            qdict_get_uint(event, "address") == address &&
            qdict_get_uint(event, "value") == value) {
            return event;
        }
    }
    return NULL;
}

static QDict *hub_for_node(QDict *state, unsigned int node)
{
    const QListEntry *entry;

    for (entry = qlist_first(qdict_get_qlist(state, "hubs")); entry;
         entry = qlist_next(entry)) {
        QDict *hub = qobject_to(QDict, qlist_entry_obj(entry));

        if (qdict_get_int(hub, "node") == node) {
            return hub;
        }
    }
    return NULL;
}

static uint8_t frame_checksum(const uint8_t *buf, unsigned int len)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < len; i++) {
        sum += buf[i];
    }
    return sum;
}

static unsigned int build_request(uint8_t *buf, const char *text)
{
    unsigned int text_len = strlen(text);

    buf[0] = text_len;
    memcpy(&buf[1], text, text_len);
    buf[text_len + 1] = (uint8_t)(-(int)frame_checksum(buf, text_len + 1));
    return text_len + 2;
}

static void elsc_send_request(QTestState *qts, const uint8_t *request,
                              unsigned int request_length)
{
    unsigned int i;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    for (i = 0; i < request_length; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, request[i]);
    }
}

static unsigned int elsc_transact(QTestState *qts, const uint8_t *request,
                                  unsigned int request_length,
                                  uint8_t *response,
                                  unsigned int response_capacity,
                                  bool query_mid_transaction)
{
    unsigned int i;
    unsigned int total;

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);

    for (i = 0; i < request_length; i++) {
        qtest_writeq(qts, HUB_LOCAL + I2C_DATA, request[i]);
    }

    if (query_mid_transaction) {
        QDict *state = query_origin_state(qts);

        qobject_unref(state);
    }

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, HUB_LOCAL + I2C_DATA, (ELSC_ADDR << 1) | 1);

    (void)qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    response[0] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    total = response[0] + 2;
    g_assert_cmpuint(total, <=, response_capacity);
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_ESO);
        }
        response[i] = qtest_readq(qts, HUB_LOCAL + I2C_DATA);
    }

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL,
                 CTL_PIN | CTL_ESO | CTL_STO);
    return total;
}

static void test_live_state_is_available_without_recording(void)
{
    QTestState *qts = qtest_init(TWO_NODE_MACHINE);
    QDict *first;
    QDict *second;
    QDict *recorder;
    QList *cpus;
    QList *hubs;
    QDict *cpu0;
    QDict *hub0;
    QDict *pcf;
    QDict *elsc;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    assert_running(qts);
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x55);
    qtest_writeq(qts, HUB_LOCAL + NI_STATUS, 5 << NI_STATUS_NODE_SHIFT);

    first = query_origin_state(qts);
    cpus = qdict_get_qlist(first, "cpus");
    hubs = qdict_get_qlist(first, "hubs");
    g_assert_cmpuint(qlist_size(cpus), ==, 4);
    g_assert_cmpuint(qlist_size(hubs), ==, 2);

    cpu0 = qobject_to(QDict, qlist_peek(cpus));
    g_assert_cmpint(qdict_get_int(cpu0, "index"), ==, 0);
    g_assert(qdict_haskey(cpu0, "interrupt-request"));
    g_assert_false(qdict_haskey(cpu0, "pc"));
    g_assert_false(qdict_haskey(cpu0, "halted"));

    hub0 = qobject_to(QDict, qlist_peek(hubs));
    g_assert_cmpint(qdict_get_int(hub0, "node"), ==, 0);
    g_assert_cmpint(qdict_get_int(hub0, "nasid"), ==, 5);
    g_assert(qdict_haskey(hub0, "pending0"));
    g_assert(qdict_haskey(hub0, "mask-a0"));
    g_assert(qdict_haskey(hub0, "rt-compare-a"));

    pcf = qdict_get_qdict(hub0, "pcf8584");
    g_assert(qdict_haskey(pcf, "control"));
    g_assert(qdict_haskey(pcf, "status"));
    g_assert(qdict_haskey(pcf, "transfer"));

    elsc = qdict_get_qdict(hub0, "elsc");
    g_assert(qdict_haskey(elsc, "rx-length"));
    g_assert(qdict_haskey(elsc, "tx-length"));
    g_assert(qdict_haskey(elsc, "arbitration-code"));
    g_assert(qdict_haskey(elsc, "queue-length"));

    recorder = qdict_get_qdict(first, "flight-recorder");
    g_assert_false(qdict_get_bool(recorder, "enabled"));
    g_assert_cmpuint(qlist_size(qdict_get_qlist(recorder, "events")), ==, 0);
    g_assert_cmpint(qdict_get_int(recorder, "next-sequence"), ==, 0);
    g_assert_cmpint(qdict_get_int(recorder, "dropped"), ==, 0);

    second = query_origin_state(qts);
    g_assert_cmpint(qdict_get_int(second, "query-sequence"), >,
                    qdict_get_int(first, "query-sequence"));
    assert_running(qts);

    qobject_unref(second);
    qobject_unref(first);
    qtest_quit(qts);
}

static void test_recording_starts_empty_after_machine_reset(void)
{
    QTestState *qts = qtest_init(TWO_NODE_RECORDING_MACHINE);
    QDict *state = query_origin_state(qts);
    QDict *recorder = qdict_get_qdict(state, "flight-recorder");

    g_assert_cmpuint(qlist_size(qdict_get_qlist(recorder, "events")), ==, 0);
    g_assert_cmpuint(qdict_get_uint(recorder, "next-sequence"), ==, 0);
    g_assert_cmpuint(qdict_get_uint(recorder, "dropped"), ==, 0);

    qobject_unref(state);
    qtest_quit(qts);
}

static void test_flight_recorder_orders_hub_and_i2c_transitions(void)
{
    QTestState *qts = qtest_init(RECORDING_MACHINE);
    uint8_t request[8];
    uint8_t response[RESPONSE_CAPACITY];
    unsigned int request_length;
    QDict *state;
    QDict *recorder;
    QList *events;
    const QListEntry *entry;
    uint64_t previous = 0;
    uint64_t hub_sequence = 0;
    uint64_t elsc_start_sequence = 0;
    uint64_t elsc_send_sequence = 0;
    uint64_t elsc_receive_start_sequence = 0;
    uint64_t elsc_receive_sequence = 0;
    uint64_t elsc_finish_sequence = 0;
    bool first_event = true;
    bool saw_pcf = false;

    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x11);
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x22);
    request_length = build_request(request, "ver");
    elsc_transact(qts, request, request_length, response, sizeof(response),
                  false);

    state = query_origin_state(qts);
    recorder = qdict_get_qdict(state, "flight-recorder");
    g_assert_true(qdict_get_bool(recorder, "enabled"));
    events = qdict_get_qlist(recorder, "events");
    g_assert_cmpuint(qlist_size(events), >, request_length);

    for (entry = qlist_first(events); entry; entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));
        uint64_t sequence = qdict_get_int(event, "sequence");
        const char *component = qdict_get_str(event, "component");
        const char *operation = qdict_get_str(event, "operation");

        if (!first_event) {
            g_assert_cmpuint(sequence, >, previous);
        }
        first_event = false;
        previous = sequence;
        g_assert_cmpint(qdict_get_int(event, "node"), ==, 0);
        g_assert(qdict_haskey(event, "size"));
        g_assert_false(qdict_haskey(event, "cpu-index"));
        g_assert_false(qdict_haskey(event, "pc"));

        if (!strcmp(component, "hub") &&
            qdict_get_int(event, "address") == PI_REGION_PRESENT &&
            qdict_get_int(event, "value") == 0x22) {
            g_assert_cmpint(qdict_get_int(event, "size"), ==, 8);
            g_assert_cmpint(qdict_get_int(event, "before"), ==, 0x11);
            g_assert_cmpint(qdict_get_int(event, "after"), ==, 0x22);
            hub_sequence = sequence;
        } else if (!strcmp(component, "pcf8584")) {
            g_assert_cmpint(qdict_get_int(event, "size"), ==, 8);
            g_assert(qdict_haskey(event, "before"));
            g_assert(qdict_haskey(event, "after"));
            saw_pcf = true;
        } else if (!strcmp(component, "elsc")) {
            g_assert_cmpint(qdict_get_int(event, "size"), ==, 1);
            g_assert(qdict_haskey(event, "before"));
            g_assert(qdict_haskey(event, "after"));
            if (!strcmp(operation, "start-send") && !elsc_finish_sequence &&
                sequence > hub_sequence) {
                elsc_start_sequence = sequence;
                elsc_send_sequence = 0;
                elsc_receive_start_sequence = 0;
                elsc_receive_sequence = 0;
            } else if (!strcmp(operation, "send") && elsc_start_sequence &&
                       !elsc_send_sequence) {
                elsc_send_sequence = sequence;
            } else if (!strcmp(operation, "start-receive") &&
                       elsc_send_sequence) {
                elsc_receive_start_sequence = sequence;
            } else if (!strcmp(operation, "receive") &&
                       elsc_receive_start_sequence &&
                       !elsc_receive_sequence) {
                elsc_receive_sequence = sequence;
            } else if (!strcmp(operation, "finish") &&
                       elsc_receive_sequence) {
                elsc_finish_sequence = sequence;
            }
        }
    }

    g_assert_cmpuint(hub_sequence, >, 0);
    g_assert_true(saw_pcf);
    g_assert_cmpuint(elsc_start_sequence, >, hub_sequence);
    g_assert_cmpuint(elsc_send_sequence, >, elsc_start_sequence);
    g_assert_cmpuint(elsc_receive_start_sequence, >, elsc_send_sequence);
    g_assert_cmpuint(elsc_receive_sequence, >, elsc_receive_start_sequence);
    g_assert_cmpuint(elsc_finish_sequence, >, elsc_receive_sequence);

    qobject_unref(state);
    qtest_quit(qts);
}

static void test_first_read_retained_and_repeats_suppressed(void)
{
    QTestState *qts = qtest_init(RECORDING_MACHINE);
    uint64_t region = qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT);
    uint64_t ni_status = qtest_readq(qts, HUB_LOCAL + NI_STATUS);
    uint64_t pcf_status = qtest_readq(qts, HUB_LOCAL + I2C_CTRL);
    QDict *state;
    const QListEntry *entry;
    unsigned int initial_region_reads = 0;
    unsigned int changed_region_reads = 0;
    unsigned int ni_status_reads = 0;
    unsigned int initial_pcf_reads = 0;
    unsigned int changed_pcf_reads = 0;

    g_assert_cmphex(region, ==, UINT64_MAX);
    g_assert_cmphex(pcf_status, ==, 0x80);

    (void)qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT);
    (void)qtest_readq(qts, HUB_LOCAL + NI_STATUS);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_CTRL);

    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x55);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT), ==,
                    0x55);
    (void)qtest_readq(qts, HUB_LOCAL + PI_REGION_PRESENT);

    qtest_writeq(qts, HUB_LOCAL + I2C_CTRL, CTL_PIN | CTL_ESO);
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + I2C_CTRL), ==, 0x81);
    (void)qtest_readq(qts, HUB_LOCAL + I2C_CTRL);

    state = query_origin_state(qts);
    for (entry = qlist_first(flight_events(state)); entry;
         entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));
        const char *component = qdict_get_str(event, "component");
        const char *operation = qdict_get_str(event, "operation");
        uint64_t address = qdict_get_uint(event, "address");
        uint64_t value = qdict_get_uint(event, "value");

        if (strcmp(operation, "mmio-read")) {
            continue;
        }
        g_assert_cmpuint(qdict_get_uint(event, "size"), ==, 8);
        if (!strcmp(component, "hub") &&
            address == PI_REGION_PRESENT && value == UINT64_MAX) {
            initial_region_reads++;
        } else if (!strcmp(component, "hub") &&
                   address == PI_REGION_PRESENT && value == 0x55) {
            changed_region_reads++;
        } else if (!strcmp(component, "hub") &&
                   address == NI_STATUS && value == ni_status) {
            ni_status_reads++;
        } else if (!strcmp(component, "pcf8584") &&
                   address == 8 && value == 0x80) {
            initial_pcf_reads++;
            g_assert_cmpuint(qdict_get_uint(event, "before"), ==,
                             qdict_get_uint(event, "after"));
        } else if (!strcmp(component, "pcf8584") &&
                   address == 8 && value == 0x81) {
            changed_pcf_reads++;
        }
    }
    g_assert_cmpuint(initial_region_reads, ==, 1);
    g_assert_cmpuint(changed_region_reads, ==, 1);
    g_assert_cmpuint(ni_status_reads, ==, 1);
    g_assert_cmpuint(initial_pcf_reads, ==, 1);
    g_assert_cmpuint(changed_pcf_reads, ==, 1);

    qobject_unref(state);
    qtest_quit(qts);
}

static void assert_controller_state_equal(QDict *left, QDict *right)
{
    static const char * const pcf_number_fields[] = {
        "data", "control", "status", "address",
    };
    static const char * const pcf_bool_fields[] = {
        "transfer", "receiving", "start-pending", "address-staged",
    };
    static const char * const elsc_number_fields[] = {
        "rx-length", "tx-length", "tx-position", "arbitration-code",
        "queue-length",
    };
    QDict *left_pcf = qdict_get_qdict(left, "pcf8584");
    QDict *right_pcf = qdict_get_qdict(right, "pcf8584");
    QDict *left_elsc = qdict_get_qdict(left, "elsc");
    QDict *right_elsc = qdict_get_qdict(right, "elsc");
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pcf_number_fields); i++) {
        g_assert_cmpint(qdict_get_int(left_pcf, pcf_number_fields[i]), ==,
                        qdict_get_int(right_pcf, pcf_number_fields[i]));
    }
    for (i = 0; i < ARRAY_SIZE(pcf_bool_fields); i++) {
        g_assert_cmpint(qdict_get_bool(left_pcf, pcf_bool_fields[i]), ==,
                        qdict_get_bool(right_pcf, pcf_bool_fields[i]));
    }
    for (i = 0; i < ARRAY_SIZE(elsc_number_fields); i++) {
        g_assert_cmpint(qdict_get_int(left_elsc, elsc_number_fields[i]), ==,
                        qdict_get_int(right_elsc, elsc_number_fields[i]));
    }
    g_assert_cmpint(qdict_get_bool(left_elsc, "arbitration-present"), ==,
                    qdict_get_bool(right_elsc, "arbitration-present"));
}

static void replay_pcf_writes(QTestState *qts, QList *events)
{
    const QListEntry *entry;

    for (entry = qlist_first(events); entry; entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));
        uint64_t address;
        uint64_t value;

        if (strcmp(qdict_get_str(event, "component"), "pcf8584") ||
            strcmp(qdict_get_str(event, "operation"), "mmio-write")) {
            continue;
        }
        address = HUB_LOCAL + I2C_DATA + qdict_get_uint(event, "address");
        value = qdict_get_uint(event, "value");
        switch (qdict_get_int(event, "size")) {
        case 1:
            qtest_writeb(qts, address, value);
            break;
        case 2:
            qtest_writew(qts, address, value);
            break;
        case 4:
            qtest_writel(qts, address, value);
            break;
        case 8:
            qtest_writeq(qts, address, value);
            break;
        default:
            g_assert_not_reached();
        }
    }
}

static void test_equal_pcf_writes_remain_replayable(void)
{
    QTestState *source = qtest_init(RECORDING_MACHINE);
    QTestState *replay = qtest_init("-M origin200");
    uint8_t request[8];
    unsigned int request_length = build_request(request, "vv");
    QDict *source_state;
    QDict *replay_state;
    QList *events;
    const QListEntry *entry;
    unsigned int equal_data_writes = 0;

    elsc_send_request(source, request, request_length);
    source_state = query_origin_state(source);
    events = flight_events(source_state);
    for (entry = qlist_first(events); entry; entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(event, "component"), "pcf8584") &&
            !strcmp(qdict_get_str(event, "operation"), "mmio-write") &&
            qdict_get_uint(event, "address") == 0 &&
            qdict_get_uint(event, "value") == 'v') {
            equal_data_writes++;
        }
    }
    g_assert_cmpuint(equal_data_writes, ==, 2);

    replay_pcf_writes(replay, events);
    replay_state = query_origin_state(replay);
    assert_controller_state_equal(hub_for_node(source_state, 0),
                                  hub_for_node(replay_state, 0));

    qobject_unref(replay_state);
    qobject_unref(source_state);
    qtest_quit(replay);
    qtest_quit(source);
}

static int aux_listen(uint16_t *port)
{
    struct sockaddr_in addr = { 0 };
    socklen_t addrlen = sizeof(addr);
    int fd;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    g_assert_cmpint(getsockname(fd, (struct sockaddr *)&addr, &addrlen), ==,
                    0);
    *port = ntohs(addr.sin_port);
    g_assert_cmpint(listen(fd, 1), ==, 0);
    return fd;
}

static int aux_accept(int listener)
{
    fd_set readfds;
    struct timeval timeout = { .tv_sec = 10 };
    int fd;

    FD_ZERO(&readfds);
    FD_SET(listener, &readfds);
    g_assert_cmpint(select(listener + 1, &readfds, NULL, NULL, &timeout), ==,
                    1);
    fd = accept(listener, NULL, 0);
    g_assert_cmpint(fd, >=, 0);
    return fd;
}

static void test_elsc_records_queue_boundaries_without_timer_ticks(void)
{
    uint16_t port;
    int listener = aux_listen(&port);
    g_autofree char *args = g_strdup_printf(
        "-M origin200,diagnostic-recording=on -serial none -serial none "
        "-chardev socket,id=aux,host=127.0.0.1,port=%u,reconnect-ms=10000 "
        "-serial chardev:aux", port);
    QTestState *qts = qtest_init(args);
    int aux = aux_accept(listener);
    uint8_t request[8];
    uint8_t response[RESPONSE_CAPACITY];
    unsigned int request_length;
    QDict *before;
    QDict *queued = NULL;
    QDict *after;
    uint64_t initial_next;
    unsigned int tries;
    QDict *start;
    QDict *end;

    before = query_origin_state(qts);
    initial_next = qdict_get_uint(qdict_get_qdict(before, "flight-recorder"),
                                  "next-sequence");
    qtest_clock_step(qts, 20 * 1000 * 1000);
    after = query_origin_state(qts);
    g_assert_cmpuint(qdict_get_uint(qdict_get_qdict(after, "flight-recorder"),
                                    "next-sequence"), ==, initial_next);
    qobject_unref(after);

    g_assert_cmpint(write(aux, "AB", 2), ==, 2);
    for (tries = 0; tries < 1000; tries++) {
        queued = query_origin_state(qts);
        if (qdict_get_int(qdict_get_qdict(hub_for_node(queued, 0), "elsc"),
                          "queue-length") == 2) {
            break;
        }
        qobject_unref(queued);
        queued = NULL;
        g_usleep(1000);
    }
    g_assert_nonnull(queued);
    g_assert_cmpuint(tries, <, 1000);
    start = find_event(flight_events(queued), "elsc", "arbitration-start",
                       0, 'A');
    g_assert_nonnull(start);
    g_assert_cmpint(qdict_get_int(start, "size"), ==, 1);

    request_length = build_request(request, "get");
    elsc_transact(qts, request, request_length, response, sizeof(response),
                  false);
    after = query_origin_state(qts);
    end = find_event(flight_events(after), "elsc", "arbitration-end", 2, 2);
    g_assert_nonnull(end);
    g_assert_cmpint(qdict_get_int(end, "size"), ==, 1);
    g_assert_cmpint(qdict_get_int(qdict_get_qdict(hub_for_node(after, 0),
                                                  "elsc"),
                                  "queue-length"), ==, 0);

    qobject_unref(after);
    qobject_unref(queued);
    qobject_unref(before);
    close(aux);
    close(listener);
    qtest_quit(qts);
}

static void test_recording_property_is_creation_time_only(void)
{
    QTestState *qts = qtest_init("-M origin200");
    QDict *response;

    response = qtest_qmp_assert_failure_ref(
        qts, "{'execute': 'qom-set', 'arguments': {"
             "'path': '/machine', 'property': 'diagnostic-recording', "
             "'value': true}}");
    qobject_unref(response);
    qtest_quit(qts);
}

static void test_hub_records_equal_and_timer_writes(void)
{
    QTestState *qts = qtest_init(RECORDING_MACHINE);
    QDict *state;
    QList *events;
    const QListEntry *entry;
    unsigned int compare_writes = 0;
    bool saw_equal_before_after = false;
    bool saw_rt_count = false;

    qtest_writel(qts, HUB_LOCAL + PI_RT_COMPARE_A, 0x12345678);
    qtest_writel(qts, HUB_LOCAL + PI_RT_COMPARE_A, 0x12345678);
    qtest_writeq(qts, HUB_LOCAL + PI_RT_COUNT, 0x3344);
    state = query_origin_state(qts);
    events = flight_events(state);

    for (entry = qlist_first(events); entry; entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (strcmp(qdict_get_str(event, "component"), "hub") ||
            strcmp(qdict_get_str(event, "operation"), "mmio-write")) {
            continue;
        }
        if (qdict_get_uint(event, "address") == PI_RT_COMPARE_A &&
            qdict_get_uint(event, "value") == 0x12345678) {
            compare_writes++;
            g_assert_cmpint(qdict_get_int(event, "size"), ==, 4);
            g_assert(qdict_haskey(event, "before"));
            g_assert(qdict_haskey(event, "after"));
            if (qdict_get_uint(event, "before") ==
                qdict_get_uint(event, "after")) {
                saw_equal_before_after = true;
            }
        } else if (qdict_get_uint(event, "address") == PI_RT_COUNT &&
                   qdict_get_uint(event, "value") == 0x3344) {
            g_assert_cmpint(qdict_get_int(event, "size"), ==, 8);
            saw_rt_count = true;
        }
    }

    g_assert_cmpuint(compare_writes, ==, 2);
    g_assert_true(saw_equal_before_after);
    g_assert_true(saw_rt_count);
    qobject_unref(state);
    qtest_quit(qts);
}

static void test_vector_launch_and_clear_are_replayable(void)
{
    QTestState *qts = qtest_init(TWO_NODE_RECORDING_MACHINE);
    uint64_t node0 = HUB_BY_NODE;
    uint64_t parms = NI_VECTOR_WRITE;
    QDict *state;
    QList *events;
    const QListEntry *entry;
    QDict *launch;
    unsigned int clear_reads = 0;
    unsigned int changed_clear_reads = 0;
    unsigned int unchanged_clear_reads = 0;

    qtest_writeq(qts, node0 + NI_VECTOR, 0);
    qtest_writeq(qts, node0 + NI_VECTOR_DATA, 7 << NI_STATUS_NODE_SHIFT);
    qtest_writeq(qts, node0 + NI_VECTOR_PARMS, parms);
    g_assert(qtest_readq(qts, node0 + NI_VECTOR_STATUS) & NI_VECTOR_VALID);
    (void)qtest_readq(qts, node0 + NI_VECTOR_CLEAR);
    (void)qtest_readq(qts, node0 + NI_VECTOR_CLEAR);
    (void)qtest_readq(qts, node0 + NI_VECTOR_CLEAR);

    state = query_origin_state(qts);
    g_assert_cmpint(qdict_get_int(hub_for_node(state, 1), "nasid"), ==, 7);
    events = flight_events(state);
    launch = find_event(events, "hub", "mmio-write", NI_VECTOR_PARMS,
                        parms);
    g_assert_nonnull(launch);
    g_assert_cmpint(qdict_get_int(launch, "size"), ==, 8);
    g_assert_false(qdict_haskey(launch, "before"));
    g_assert_false(qdict_haskey(launch, "after"));

    for (entry = qlist_first(events); entry; entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(event, "component"), "hub") &&
            !strcmp(qdict_get_str(event, "operation"), "mmio-read") &&
            qdict_get_uint(event, "address") == NI_VECTOR_CLEAR) {
            clear_reads++;
            g_assert_cmpint(qdict_get_int(event, "size"), ==, 8);
            if (qdict_get_uint(event, "before") ==
                qdict_get_uint(event, "after")) {
                unchanged_clear_reads++;
            } else {
                changed_clear_reads++;
            }
        }
    }
    g_assert_cmpuint(clear_reads, ==, 2);
    g_assert_cmpuint(changed_clear_reads, ==, 1);
    g_assert_cmpuint(unchanged_clear_reads, ==, 1);

    qobject_unref(state);
    qtest_quit(qts);
}

static void test_event_limit_returns_newest_ordered_subset(void)
{
    QTestState *qts = qtest_init(RECORDING_MACHINE);
    QDict *state;
    QDict *recorder;
    QList *events;
    const QListEntry *first;
    const QListEntry *second;
    uint64_t next;

    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x10);
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x20);
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x30);
    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x40);

    state = query_origin_state_limit(qts, 2);
    recorder = qdict_get_qdict(state, "flight-recorder");
    events = qdict_get_qlist(recorder, "events");
    next = qdict_get_uint(recorder, "next-sequence");
    g_assert_cmpuint(qlist_size(events), ==, 2);
    first = qlist_first(events);
    second = qlist_next(first);
    g_assert_cmpuint(qdict_get_uint(qobject_to(QDict,
                                              qlist_entry_obj(first)),
                                    "sequence"), ==, next - 2);
    g_assert_cmpuint(qdict_get_uint(qobject_to(QDict,
                                              qlist_entry_obj(second)),
                                    "sequence"), ==, next - 1);
    g_assert_cmpuint(qdict_get_uint(qobject_to(QDict,
                                              qlist_entry_obj(first)),
                                    "value"), ==, 0x30);
    g_assert_cmpuint(qdict_get_uint(qobject_to(QDict,
                                              qlist_entry_obj(second)),
                                    "value"), ==, 0x40);

    qobject_unref(state);
    qtest_quit(qts);
}

static void test_reset_clears_flight_recorder(void)
{
    QTestState *qts = qtest_init(RECORDING_MACHINE);
    QDict *before;
    QDict *after;
    QDict *recorder;

    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x55);
    before = query_origin_state(qts);
    g_assert_cmpuint(qlist_size(flight_events(before)), >, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    after = query_origin_state(qts);
    recorder = qdict_get_qdict(after, "flight-recorder");
    g_assert_cmpuint(qlist_size(qdict_get_qlist(recorder, "events")), ==, 0);
    g_assert_cmpuint(qdict_get_uint(recorder, "next-sequence"), ==, 0);
    g_assert_cmpuint(qdict_get_uint(recorder, "dropped"), ==, 0);

    qobject_unref(after);
    qobject_unref(before);
    qtest_quit(qts);
}

static void test_flight_recorder_wraps_at_4096_events(void)
{
    QTestState *qts = qtest_init(TWO_NODE_RECORDING_MACHINE);
    QDict *baseline = query_origin_state(qts);
    QDict *baseline_recorder = qdict_get_qdict(baseline,
                                               "flight-recorder");
    QDict *state;
    QDict *recorder;
    QList *events;
    const QListEntry *last;
    QDict *oldest;
    QDict *newest;
    uint64_t baseline_next = qdict_get_int(baseline_recorder,
                                           "next-sequence");
    uint64_t baseline_dropped = qdict_get_int(baseline_recorder, "dropped");
    uint64_t baseline_count = qlist_size(
        qdict_get_qlist(baseline_recorder, "events"));
    uint64_t expected_next = baseline_next + 4100;
    uint64_t expected_dropped = baseline_dropped + baseline_count + 4100 -
                                4096;
    unsigned int node_events[2] = { 0 };
    unsigned int i;

    for (i = 0; i < 4100; i++) {
        qtest_writeq(qts, HUB_BY_NODE + (i & 1) * NODE_STRIDE +
                          PI_REGION_PRESENT, i);
    }

    state = query_origin_state(qts);
    recorder = qdict_get_qdict(state, "flight-recorder");
    events = qdict_get_qlist(recorder, "events");
    oldest = qobject_to(QDict, qlist_peek(events));
    last = qlist_first(events);
    while (qlist_next(last)) {
        last = qlist_next(last);
    }
    newest = qobject_to(QDict, qlist_entry_obj(last));
    g_assert_cmpuint(qlist_size(events), ==, 4096);
    g_assert_cmpint(qdict_get_int(recorder, "dropped"), ==,
                    expected_dropped);
    g_assert_cmpint(qdict_get_int(recorder, "next-sequence"), ==,
                    expected_next);
    g_assert_cmpint(qdict_get_int(oldest, "sequence"), ==,
                    expected_next - 4096);
    g_assert_cmpint(qdict_get_int(newest, "sequence"), ==,
                    expected_next - 1);
    for (last = qlist_first(events); last; last = qlist_next(last)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(last));
        unsigned int node = qdict_get_int(event, "node");

        g_assert_cmpuint(node, <, ARRAY_SIZE(node_events));
        node_events[node]++;
    }
    g_assert_cmpuint(node_events[0], ==, 2048);
    g_assert_cmpuint(node_events[1], ==, 2048);

    qobject_unref(baseline);
    qobject_unref(state);
    qtest_quit(qts);
}

static void test_read_is_retained_after_cross_node_wrap(void)
{
    QTestState *qts = qtest_init(TWO_NODE_RECORDING_MACHINE);
    QDict *state;
    QDict *recorder;
    const QListEntry *entry;
    unsigned int retained_reads = 0;
    unsigned int i;

    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_REGION_PRESENT), ==,
                    UINT64_MAX);
    for (i = 0; i < 4096; i++) {
        qtest_writeq(qts, HUB_BY_NODE + NODE_STRIDE + PI_REGION_PRESENT, i);
    }
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NODE + PI_REGION_PRESENT), ==,
                    UINT64_MAX);

    state = query_origin_state(qts);
    recorder = qdict_get_qdict(state, "flight-recorder");
    g_assert_cmpuint(qlist_size(flight_events(state)), ==, 4096);
    g_assert_cmpuint(qdict_get_uint(recorder, "next-sequence"), ==, 4098);
    g_assert_cmpuint(qdict_get_uint(recorder, "dropped"), ==, 2);
    for (entry = qlist_first(flight_events(state)); entry;
         entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(event, "component"), "hub") &&
            !strcmp(qdict_get_str(event, "operation"), "mmio-read") &&
            qdict_get_uint(event, "node") == 0 &&
            qdict_get_uint(event, "address") == PI_REGION_PRESENT &&
            qdict_get_uint(event, "value") == UINT64_MAX) {
            retained_reads++;
            g_assert_cmpuint(qdict_get_uint(event, "sequence"), ==, 4097);
            g_assert_false(qdict_haskey(event, "cpu-index"));
            g_assert_false(qdict_haskey(event, "pc"));
        }
    }
    g_assert_cmpuint(retained_reads, ==, 1);

    qobject_unref(state);
    qtest_quit(qts);
}

static GuestVisibleState run_guest_transaction(bool recording,
                                                bool query_mid_transaction)
{
    QTestState *qts = qtest_init(recording ? RECORDING_MACHINE :
                                             "-M origin200");
    GuestVisibleState result = { 0 };
    uint8_t request[8];
    unsigned int request_length = build_request(request, "ver");

    qtest_writeq(qts, HUB_LOCAL + PI_REGION_PRESENT, 0x1234);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MASK0_A, 1ULL << 12);
    qtest_writeq(qts, HUB_LOCAL + PI_INT_MODIFY,
                 INT_MODIFY_SET | 12);
    result.response_length = elsc_transact(
        qts, request, request_length, result.response,
        sizeof(result.response), query_mid_transaction);
    result.region_present = qtest_readq(qts,
                                        HUB_LOCAL + PI_REGION_PRESENT);
    result.interrupt_pending = qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0);
    result.interrupt_mask = qtest_readq(qts, HUB_LOCAL + PI_INT_MASK0_A);
    result.i2c_status = qtest_readq(qts, HUB_LOCAL + I2C_CTRL);

    qtest_quit(qts);
    return result;
}

static void assert_guest_state_equal(const GuestVisibleState *left,
                                     const GuestVisibleState *right)
{
    g_assert_cmphex(left->region_present, ==, right->region_present);
    g_assert_cmphex(left->interrupt_pending, ==, right->interrupt_pending);
    g_assert_cmphex(left->interrupt_mask, ==, right->interrupt_mask);
    g_assert_cmphex(left->i2c_status, ==, right->i2c_status);
    g_assert_cmpuint(left->response_length, ==, right->response_length);
    g_assert_cmpmem(left->response, left->response_length,
                    right->response, right->response_length);
}

static void test_recording_and_query_are_behavior_neutral(void)
{
    GuestVisibleState off = run_guest_transaction(false, false);
    GuestVisibleState on = run_guest_transaction(true, false);
    GuestVisibleState queried = run_guest_transaction(true, true);

    assert_guest_state_equal(&off, &on);
    assert_guest_state_equal(&off, &queried);
}

static void put_instruction(uint8_t *image, unsigned int offset,
                            uint32_t instruction)
{
    stl_be_p(image + ELF_CODE_OFFSET + offset, instruction);
}

static void put_launch_call(uint8_t *image, unsigned int offset,
                            unsigned int nasid, unsigned int slice,
                            unsigned int cpu_index)
{
    put_instruction(image, offset + 0x00,
                    0x24040000 | nasid);     /* addiu a0, zero, nasid */
    put_instruction(image, offset + 0x04,
                    0x24050000 | slice);     /* addiu a1, zero, slice */
    put_instruction(image, offset + 0x08,
                    0x02003025);             /* or a2, s0, zero */
    put_instruction(image, offset + 0x0c,
                    0x24070000 | cpu_index); /* addiu a3, zero, index */
    put_instruction(image, offset + 0x10,
                    0x24080000);             /* addiu t0, zero, 0 */
    put_instruction(image, offset + 0x14,
                    0x24090000);             /* addiu t1, zero, 0 */
    put_instruction(image, offset + 0x18,
                    0x0320f809);             /* jalr ra, t9 */
    put_instruction(image, offset + 0x1c, 0); /* nop */
}

static void write_attribution_test_elf(int fd, unsigned int cpu_count,
                                       bool poll)
{
    uint8_t image[ELF_CODE_OFFSET + ELF_CODE_SIZE] = { };
    ssize_t written;

    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 2;
    image[5] = 2;
    image[6] = 1;
    stw_be_p(image + 0x10, 2);
    stw_be_p(image + 0x12, 8);
    stl_be_p(image + 0x14, 1);
    stq_be_p(image + 0x18, KERNEL_ENTRY);
    stq_be_p(image + 0x20, 0x40);
    stw_be_p(image + 0x34, 0x40);
    stw_be_p(image + 0x36, 0x38);
    stw_be_p(image + 0x38, 1);

    stl_be_p(image + 0x40, 1);
    stl_be_p(image + 0x44, 5);
    stq_be_p(image + 0x48, ELF_CODE_OFFSET);
    stq_be_p(image + 0x50, KERNEL_ENTRY);
    stq_be_p(image + 0x58, KERNEL_ENTRY);
    stq_be_p(image + 0x60, ELF_CODE_SIZE);
    stq_be_p(image + 0x68, ELF_CODE_SIZE);
    stq_be_p(image + 0x70, ELF_CODE_OFFSET);

    put_instruction(image, 0x00, 0x3c19bfc0); /* lui t9, 0xbfc0 */
    put_instruction(image, 0x04, 0x37390038); /* ori t9, t9, 0x38 */
    put_instruction(image, 0x08, 0x3c108001); /* lui s0, 0x8001 */
    put_instruction(image, 0x0c, 0x36100100); /* ori s0, s0, 0x100 */
    put_launch_call(image, 0x10, 0, 1, 1);
    if (cpu_count > 2) {
        put_launch_call(image, 0x30, 1, 0, 2);
        put_launch_call(image, 0x50, 1, 1, 3);
    }
    put_instruction(image, 0x70, 0x24040000); /* addiu a0, zero, 0 */
    put_instruction(image, 0x74, 0x02000008); /* jr s0 */
    put_instruction(image, 0x78, 0);          /* nop */

    /* Every CPU continuously accesses this register through its local Hub. */
    put_instruction(image, 0x100, 0x3c089200); /* lui t0, 0x9200 */
    put_instruction(image, 0x104, 0x0008403c); /* dsll32 t0, t0, 0 */
    put_instruction(image, 0x108, 0x3c090100); /* lui t1, 0x100 */
    put_instruction(image, 0x10c, 0x35290018); /* ori t1, t1, 0x18 */
    put_instruction(image, 0x110, 0x01094025); /* or t0, t0, t1 */
    if (poll) {
        put_instruction(image, 0x114, 0);          /* nop */
        put_instruction(image, 0x118, 0xdd090000); /* ld t1, 0(t0) */
    } else {
        put_instruction(image, 0x114,
                        0x64890100); /* daddiu t1, a0, 0x100 */
        put_instruction(image, 0x118, 0xfd090000); /* sd t1, 0(t0) */
    }
    put_instruction(image, 0x11c, 0x1000fffe); /* b 0x118 */
    put_instruction(image, 0x120, 0);          /* nop */

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void test_mttcg_events_have_issuing_cpus_and_pcs(void)
{
    g_autofree char *kernel = NULL;
    QTestState *qts;
    QDict *state = NULL;
    bool saw_cpu[4] = { };
    uint64_t observed_next = 0;
    unsigned int tries;
    unsigned int i;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-diagnostic-attribution-XXXXXX", &kernel,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    write_attribution_test_elf(fd, 4, false);
    close(fd);

    qts = qtest_initf("-M origin200,diagnostic-recording=on %s "
                      "-kernel %s -accel tcg,thread=multi "
                      "-no-reboot -no-shutdown",
                      TWO_NODE_TOPOLOGY, kernel);
    g_assert_cmpint(unlink(kernel), ==, 0);

    for (tries = 0; tries < 2000; tries++) {
        const QListEntry *entry;

        qobject_unref(state);
        state = query_origin_state_limit(qts, 512);
        observed_next = qdict_get_uint(
            qdict_get_qdict(state, "flight-recorder"), "next-sequence");
        for (entry = qlist_first(flight_events(state)); entry;
             entry = qlist_next(entry)) {
            QDict *event = qobject_to(QDict, qlist_entry_obj(entry));
            uint64_t value;
            unsigned int cpu_index;

            if (strcmp(qdict_get_str(event, "component"), "hub") ||
                strcmp(qdict_get_str(event, "operation"), "mmio-write") ||
                qdict_get_uint(event, "address") != PI_REGION_PRESENT) {
                continue;
            }
            value = qdict_get_uint(event, "value");
            if (value < 0x100 || value >= 0x104) {
                continue;
            }
            cpu_index = value - 0x100;
            g_assert_cmpint(qdict_get_int(event, "cpu-index"), ==,
                            cpu_index);
            g_assert_cmpuint(qdict_get_uint(event, "pc"), ==, STORE_PC);
            g_assert_cmpuint(qdict_get_uint(event, "node"), ==,
                             cpu_index / 2);
            g_assert_cmpuint(qdict_get_uint(event, "size"), ==, 8);
            saw_cpu[cpu_index] = true;
        }
        for (i = 0; i < ARRAY_SIZE(saw_cpu); i++) {
            if (!saw_cpu[i]) {
                break;
            }
        }
        if (i == ARRAY_SIZE(saw_cpu)) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(tries, <, 2000);

    for (tries = 0; tries < 1000; tries++) {
        QDict *later = query_origin_state_limit(qts, 0);
        uint64_t next = qdict_get_uint(
            qdict_get_qdict(later, "flight-recorder"), "next-sequence");

        qobject_unref(later);
        if (next > observed_next) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(tries, <, 1000);

    qobject_unref(state);
    qtest_quit(qts);
}

static void test_mttcg_read_dedup_preserves_issuing_cpu(void)
{
    g_autofree char *kernel = NULL;
    QTestState *qts;
    QDict *state = NULL;
    bool saw_cpu[2] = { };
    unsigned int tries;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-diagnostic-read-attribution-XXXXXX", &kernel,
                         NULL);
    g_assert_cmpint(fd, >=, 0);
    write_attribution_test_elf(fd, 2, true);
    close(fd);

    qts = qtest_initf("-M origin200,diagnostic-recording=on "
                      "-m 64M -smp 2,sockets=1,cores=2,threads=1 "
                      "-kernel %s -accel tcg,thread=multi "
                      "-no-reboot -no-shutdown", kernel);
    g_assert_cmpint(unlink(kernel), ==, 0);

    for (tries = 0; tries < 2000; tries++) {
        const QListEntry *entry;

        qobject_unref(state);
        state = query_origin_state_limit(qts, 16);
        for (entry = qlist_first(flight_events(state)); entry;
             entry = qlist_next(entry)) {
            QDict *event = qobject_to(QDict, qlist_entry_obj(entry));
            unsigned int cpu_index;

            if (strcmp(qdict_get_str(event, "component"), "hub") ||
                strcmp(qdict_get_str(event, "operation"), "mmio-read") ||
                qdict_get_uint(event, "address") != PI_REGION_PRESENT ||
                qdict_get_uint(event, "value") != UINT64_MAX) {
                continue;
            }
            cpu_index = qdict_get_uint(event, "cpu-index");
            g_assert_cmpuint(cpu_index, <, ARRAY_SIZE(saw_cpu));
            g_assert_cmpuint(qdict_get_uint(event, "pc"), ==, POLL_PC);
            g_assert_cmpuint(qdict_get_uint(event, "node"), ==, 0);
            g_assert_cmpuint(qdict_get_uint(event, "size"), ==, 8);
            saw_cpu[cpu_index] = true;
        }
        if (saw_cpu[0] && saw_cpu[1]) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(tries, <, 2000);

    qobject_unref(state);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/debug/live-state",
                   test_live_state_is_available_without_recording);
    qtest_add_func("/sn0/debug/flight-recorder/empty-after-machine-reset",
                   test_recording_starts_empty_after_machine_reset);
    qtest_add_func("/sn0/debug/flight-recorder/transitions",
                   test_flight_recorder_orders_hub_and_i2c_transitions);
    qtest_add_func("/sn0/debug/flight-recorder/read-deduplication",
                   test_first_read_retained_and_repeats_suppressed);
    qtest_add_func("/sn0/debug/flight-recorder/equal-pcf-writes-replay",
                   test_equal_pcf_writes_remain_replayable);
    qtest_add_func("/sn0/debug/flight-recorder/elsc-semantic-boundaries",
                   test_elsc_records_queue_boundaries_without_timer_ticks);
    qtest_add_func("/sn0/debug/flight-recorder/creation-time-property",
                   test_recording_property_is_creation_time_only);
    qtest_add_func("/sn0/debug/flight-recorder/hub-equal-and-timer-writes",
                   test_hub_records_equal_and_timer_writes);
    qtest_add_func("/sn0/debug/flight-recorder/vector-launch-and-clear",
                   test_vector_launch_and_clear_are_replayable);
    qtest_add_func("/sn0/debug/flight-recorder/partial-limit",
                   test_event_limit_returns_newest_ordered_subset);
    qtest_add_func("/sn0/debug/flight-recorder/reset",
                   test_reset_clears_flight_recorder);
    qtest_add_func("/sn0/debug/flight-recorder/wrap",
                   test_flight_recorder_wraps_at_4096_events);
    qtest_add_func("/sn0/debug/flight-recorder/read-after-cross-node-wrap",
                   test_read_is_retained_after_cross_node_wrap);
    qtest_add_func("/sn0/debug/behavior-neutral",
                   test_recording_and_query_are_behavior_neutral);
    qtest_add_func("/sn0/debug/mttcg-attribution",
                   test_mttcg_events_have_issuing_cpus_and_pcs);
    qtest_add_func("/sn0/debug/mttcg-read-dedup-attribution",
                   test_mttcg_read_dedup_preserves_issuing_cpu);
    return g_test_run();
}
