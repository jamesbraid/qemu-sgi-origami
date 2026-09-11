/*
 * SGI Router ASIC register target and programmed-route lookup.
 *
 * A Router has no processor-visible register window.  The machine-owned SN0
 * fabric reaches this device through sgi_router_access() after walking a
 * vector.  Routed reply construction stays in the fabric because the
 * Router's reply-source encoding is not established.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/1wire/onewire.h"
#include "hw/1wire/ds2505.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/misc/sgi-router.h"
#include "migration/vmstate.h"
#include "system/reset.h"

#define RR_STATUS_REV_ID       0x00000
#define RR_PORT_RESET          0x00008
#define RR_PROTECTION_CONFIG   0x00010
#define RR_GLOBAL_PARMS        0x00018
#define RR_SCRATCH_REG0        0x00020
#define RR_SCRATCH_REG1        0x00028
#define RR_DIAG_PARMS          0x00030
#define RR_NIC_ULAN            0x00038
#define RR_BIST_DATA           0x00050
#define RR_BIST_READY          0x00058
#define RR_BIST_COUNT_TARGET   0x00060
#define RR_BIST_SHIFT_LOAD     0x00068
#define RR_BIST_SHIFT_UNLOAD   0x00070
#define RR_BIST_ENTER_RUN      0x00078

#define RR_PORT_BLOCK_FIRST    1
#define RR_PORT_BLOCK_LAST     6
#define RR_PORT_PARMS          0x0000
#define RR_STATUS_ERROR        0x0008
#define RR_HISTOGRAM           0x0010
#define RR_RESET_MASK          0x0018
#define RR_ERROR_CLEAR         0x0088

#define RR_META_TABLE_BASE     0x70000
#define RR_META_TABLE_COUNT    32
#define RR_LOCAL_TABLE_BASE    0x70100
#define RR_LOCAL_TABLE_COUNT   16
#define RR_TABLE_STRIDE        8

#define RR_STATUS_IDENTITY         UINT64_C(1)
#define RR_STATUS_REVISION         (UINT64_C(1) << 4)
#define RR_STATUS_INPUTS_SHIFT     8
#define RR_STATUS_OUTPUTS_MASK     (UINT64_C(0xf) << 12)
#define RR_STATUS_OUTPUTS_RESET    (UINT64_C(1) << 12)
#define RR_STATUS_LED_MASK         (UINT64_C(0x3f) << 16)
#define RR_STATUS_UNUSED_MASK      (UINT64_C(3) << 22)
#define RR_STATUS_CONTROL_MASK     (RR_STATUS_OUTPUTS_MASK | \
                                    RR_STATUS_LED_MASK | \
                                    RR_STATUS_UNUSED_MASK)
#define RR_STATUS_LOCAL_ERROR_MASK (UINT64_C(0xf) << 24)
#define RR_STATUS_INPUT_PORT_SHIFT 46

#define RR_PROTECTION_MASK     UINT64_C(0x1fffff)
#define RR_PROTECTION_RESET    UINT64_C(0x3f)

#define RR_GLOBAL_READ_ONLY    (UINT64_C(3) << 10)
#define RR_GLOBAL_RESET        UINT64_C(0x3fffffe0a01ffff1)

#define RR_SCRATCH1_MASK       UINT64_C(0xffff)
#define RR_DIAG_MASK           UINT64_C(0xfffff)

#define RR_NIC_PHASE_LOW       (UINT64_C(0x7f) << 20)
#define RR_NIC_PHASE_HIGH      (UINT64_C(0x7f) << 27)
#define RR_NIC_PHASE_MASK      (RR_NIC_PHASE_LOW | RR_NIC_PHASE_HIGH)
#define RR_NIC_SAMPLE_MASK     (UINT64_C(0xff) << 2)
#define RR_NIC_PULSE_MASK      (UINT64_C(0x3ff) << 10)
#define RR_NIC_DONE            (UINT64_C(1) << 1)
#define RR_NIC_RESET           ((UINT64_C(0x31) << 20) | \
                                (UINT64_C(0x31) << 27) | RR_NIC_DONE)
#define RR_NIC_STATE_MASK      (RR_NIC_PHASE_MASK | RR_NIC_DONE | UINT64_C(1))

#define RR_PORT_PARMS_MASK     UINT64_C(0xfffff)
#define RR_PORT_PARMS_RESET    UINT64_C(0x1bf0)
#define RR_PORT_HISTOGRAM_SELECT_SHIFT 18
#define RR_PORT_ERROR_MASK     UINT64_C(0x3ffffffff)
#define RR_PORT_SEQUENCE_ERROR_SHIFT 0
#define RR_PORT_CHECK_ERROR_SHIFT 8
#define RR_PORT_RETRY_SHIFT    16
#define RR_PORT_COUNTER_MASK   UINT64_C(0xff)
#define RR_RESET_MASK_MASK     UINT64_C(0x3f)
#define RR_TABLE_MASK          UINT64_C(0xffffff)

#define RR_DIAG_ABSOLUTE_HISTOGRAM (UINT64_C(1) << 19)
#define RR_HISTOGRAM_UTILISATION   1
#define RR_HISTOGRAM_TICK_NS       20

#define RR_PROTECTION_FORCE_LOCAL  (UINT64_C(1) << 12)
#define RR_PROTECTION_META_VALID   (UINT64_C(1) << 11)
#define RR_PROTECTION_META_SHIFT   6
#define RR_PROTECTION_META_MASK    UINT64_C(0x1f)

#define RR_RESET_ADMIN             (UINT64_C(1) << 0)
#define RR_RESET_PORT_MASK         (UINT64_C(0x3f) << 1)
#define RR_RESET_WARM              (UINT64_C(1) << 7)

#define RR_BIST_TARGET_LOGIC       1
#define RR_BIST_TARGET_ARRAY       2
#define RR_BIST_TARGET_CLOCK       3
#define RR_BIST_LOGIC_INPUT_BITS   191
#define RR_BIST_ARRAY_INPUT_BITS   110
#define RR_BIST_CLOCK_INPUT_BITS   1
#define RR_BIST_LOGIC_UNLOAD_BITS  64
#define RR_BIST_ARRAY_UNLOAD_BITS  110

typedef enum RouterNICPhase {
    ROUTER_NIC_IDLE,
    ROUTER_NIC_DRIVING,
    ROUTER_NIC_SAMPLING,
} RouterNICPhase;

struct SGIRouterState {
    DeviceState parent_obj;

    uint8_t chip_inputs;

    OneWireBus *nic_bus;
    DeviceState *nic;
    QEMUTimer *nic_timer;
    uint64_t nic_clock_hz;
    uint64_t nic_serial;
    char *nic_image;
    bool nic_present;
    uint8_t nic_population;
    uint8_t nic_phase;
    uint8_t nic_candidate;
    uint16_t nic_pulse;
    uint16_t nic_sample;
    int64_t nic_release_ns;
    int64_t nic_sample_ns;

    void (*changed)(void *opaque);
    void *changed_opaque;

    uint64_t status_control;
    uint64_t local_errors;
    uint64_t protection_config;
    uint64_t global_parms;
    uint64_t scratch0;
    uint16_t scratch1;
    uint64_t diag_parms;
    uint64_t nic_ulan;
    uint64_t bist_data;
    uint16_t bist_loaded[4];
    uint16_t bist_unload_position;
    uint8_t bist_count;
    uint8_t bist_target;
    uint8_t bist_run_target;
    uint8_t bist_completed_target;
    bool bist_ready;

    uint64_t port_parms[SGI_ROUTER_PORTS];
    uint64_t port_errors[SGI_ROUTER_PORTS];
    uint64_t histograms[SGI_ROUTER_PORTS];
    int64_t histogram_next_ns;
    uint8_t histogram_phase_ns;
    uint8_t reset_masks[SGI_ROUTER_PORTS];
    bool port_working[SGI_ROUTER_PORTS];
    bool port_eight_bit[SGI_ROUTER_PORTS];

    uint32_t meta_table[RR_META_TABLE_COUNT];
    uint32_t local_table[RR_LOCAL_TABLE_COUNT];
    bool meta_valid[RR_META_TABLE_COUNT];
    bool local_valid[RR_LOCAL_TABLE_COUNT];
};

/* These are reviewed compatibility cases, not a divider transfer formula. */
static bool sgi_router_nic_calibrated(SGIRouterState *s, uint64_t value)
{
    unsigned int first = extract64(value, 20, 7);
    unsigned int second = extract64(value, 27, 7);

    return first == second &&
           ((s->nic_clock_hz == 97500000 && first == 47) ||
            (s->nic_clock_hz == 100000000 && first == 49));
}

static bool sgi_router_nic_waveform(unsigned int pulse, unsigned int sample)
{
    return (pulse == 520 && sample == 65) ||
           (pulse == 6 && (sample == 13 || sample == 110)) ||
           (pulse == 80 && sample == 30);
}

static void sgi_router_nic_idle(SGIRouterState *s)
{
    s->nic_phase = ROUTER_NIC_IDLE;
    s->nic_candidate = 0;
    s->nic_pulse = 0;
    s->nic_sample = 0;
    s->nic_release_ns = 0;
    s->nic_sample_ns = 0;
}

static void sgi_router_nic_tick(void *opaque)
{
    SGIRouterState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->nic_phase == ROUTER_NIC_DRIVING && now >= s->nic_release_ns) {
        if (s->nic_pulse == 520) {
            s->nic_candidate = !onewire_reset(s->nic_bus);
        } else {
            bool result = onewire_touch_bit(s->nic_bus, s->nic_pulse == 6);

            /*
             * Retaining a short-read bit through 19us is a reviewed
             * compatibility approximation beyond DS2505's 15us window.
             * The two late write samples see the released line instead.
             */
            s->nic_candidate = s->nic_sample == 13 ? result : 1;
        }
        s->nic_phase = ROUTER_NIC_SAMPLING;
    }
    if (s->nic_phase == ROUTER_NIC_SAMPLING && now >= s->nic_sample_ns) {
        s->nic_ulan |= RR_NIC_DONE | s->nic_candidate;
        sgi_router_nic_idle(s);
    }
    if (s->nic_phase != ROUTER_NIC_IDLE) {
        timer_mod(s->nic_timer, s->nic_phase == ROUTER_NIC_DRIVING ?
                               s->nic_release_ns : s->nic_sample_ns);
    }
}

static bool sgi_router_nic_write(SGIRouterState *s, uint64_t value)
{
    unsigned int pulse = extract64(value, 10, 10);
    unsigned int sample = extract64(value, 2, 8);
    int64_t now;

    /* Reject-and-preserve is the model policy for undocumented overlap. */
    if (s->nic_phase != ROUTER_NIC_IDLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-router: one-wire write while busy\n");
        return false;
    }
    if (!sgi_router_nic_calibrated(s, value) ||
        !sgi_router_nic_waveform(pulse, sample)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-router: unsupported one-wire clock %" PRIu64
                      " phases %" PRIu64 "/%" PRIu64 " pulse %u sample %u\n",
                      s->nic_clock_hz, extract64(value, 20, 7),
                      extract64(value, 27, 7), pulse, sample);
        return false;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < 0 || now > INT64_MAX - (pulse + sample) * 1000) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-router: one-wire clock overflow\n");
        return false;
    }
    s->nic_ulan = value & RR_NIC_PHASE_MASK;
    s->nic_phase = ROUTER_NIC_DRIVING;
    s->nic_candidate = 0;
    s->nic_pulse = pulse;
    s->nic_sample = sample;
    s->nic_release_ns = now + pulse * 1000;
    s->nic_sample_ns = s->nic_release_ns + sample * 1000;
    timer_mod(s->nic_timer, s->nic_release_ns);
    return true;
}

static void sgi_router_changed(SGIRouterState *s)
{
    if (s->changed) {
        s->changed(s->changed_opaque);
    }
}

static uint64_t sgi_router_status(SGIRouterState *s,
                                  unsigned int arrival_port)
{
    uint64_t status = RR_STATUS_IDENTITY | RR_STATUS_REVISION |
                      ((uint64_t)s->chip_inputs << RR_STATUS_INPUTS_SHIFT) |
                      s->status_control | s->local_errors |
                      ((uint64_t)arrival_port << RR_STATUS_INPUT_PORT_SHIFT);
    unsigned int port;

    for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
        if (s->port_working[port - 1]) {
            status |= UINT64_C(1) << (26 + 3 * port);
        }
        if (s->port_eight_bit[port - 1]) {
            status |= UINT64_C(1) << (27 + 3 * port);
        }
    }
    return status;
}

static uint32_t *sgi_router_table_entry(SGIRouterState *s, hwaddr addr,
                                        bool **valid)
{
    unsigned int index;

    if ((addr & (RR_TABLE_STRIDE - 1)) != 0) {
        return NULL;
    }
    if (addr >= RR_META_TABLE_BASE &&
        addr < RR_META_TABLE_BASE + RR_META_TABLE_COUNT * RR_TABLE_STRIDE) {
        index = (addr - RR_META_TABLE_BASE) / RR_TABLE_STRIDE;
        *valid = &s->meta_valid[index];
        return &s->meta_table[index];
    }
    if (addr >= RR_LOCAL_TABLE_BASE &&
        addr < RR_LOCAL_TABLE_BASE + RR_LOCAL_TABLE_COUNT * RR_TABLE_STRIDE) {
        index = (addr - RR_LOCAL_TABLE_BASE) / RR_TABLE_STRIDE;
        *valid = &s->local_valid[index];
        return &s->local_table[index];
    }
    return NULL;
}

static bool sgi_router_port_register(hwaddr addr, unsigned int *port,
                                     hwaddr *offset)
{
    unsigned int block = extract64(addr, 16, 4);

    if (block < RR_PORT_BLOCK_FIRST || block > RR_PORT_BLOCK_LAST) {
        return false;
    }
    *port = block - 1;
    *offset = addr & 0xffff;
    return true;
}

static bool sgi_router_histogram_progresses(SGIRouterState *s,
                                            unsigned int port)
{
    return !(s->diag_parms & RR_DIAG_ABSOLUTE_HISTOGRAM) &&
           extract64(s->port_parms[port],
                     RR_PORT_HISTOGRAM_SELECT_SHIFT, 2) ==
           RR_HISTOGRAM_UTILISATION;
}

static void sgi_router_histogram_unpack(uint64_t value, uint32_t buckets[4])
{
    unsigned int i;

    for (i = 0; i < 4; i++) {
        buckets[i] = extract64(value, 16 * i, 16);
    }
}

static uint64_t sgi_router_histogram_pack(const uint32_t buckets[4])
{
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < 4; i++) {
        value = deposit64(value, 16 * i, 16, buckets[i]);
    }
    return value;
}

static void sgi_router_histogram_increment(SGIRouterState *s,
                                           unsigned int port,
                                           unsigned int bucket_mask)
{
    uint32_t buckets[4];
    bool rescale = false;
    unsigned int i;

    sgi_router_histogram_unpack(s->histograms[port], buckets);
    for (i = 0; i < 4; i++) {
        if (bucket_mask & (1U << i)) {
            buckets[i]++;
            rescale |= buckets[i] > UINT16_MAX;
        }
    }
    if (rescale) {
        for (i = 0; i < 4; i++) {
            buckets[i] >>= 1;
        }
    }
    s->histograms[port] = sgi_router_histogram_pack(buckets);
}

static void sgi_router_histogram_add_ticks(SGIRouterState *s,
                                           unsigned int port,
                                           uint64_t ticks)
{
    uint32_t buckets[4];
    uint64_t until_rescale;

    sgi_router_histogram_unpack(s->histograms[port], buckets);
    until_rescale = UINT64_C(0x10000) - buckets[3];
    if (ticks < until_rescale) {
        buckets[3] += ticks;
    } else {
        uint64_t rescale_count;

        ticks -= until_rescale;
        rescale_count = 1 + ticks / UINT64_C(0x8000);
        for (unsigned int i = 0; i < 3; i++) {
            if (rescale_count >= 16) {
                buckets[i] = 0;
            } else {
                buckets[i] >>= rescale_count;
            }
        }
        buckets[3] = UINT64_C(0x8000) + ticks % UINT64_C(0x8000);
    }
    s->histograms[port] = sgi_router_histogram_pack(buckets);
}

static void sgi_router_histogram_sync(SGIRouterState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks;
    unsigned int port;

    if (!s->histogram_next_ns || now < s->histogram_next_ns) {
        return;
    }
    ticks = (now - s->histogram_next_ns) / RR_HISTOGRAM_TICK_NS + 1;
    for (port = 0; port < SGI_ROUTER_PORTS; port++) {
        if (sgi_router_histogram_progresses(s, port)) {
            sgi_router_histogram_add_ticks(s, port, ticks);
        }
    }
    if (ticks <= (INT64_MAX - s->histogram_next_ns) /
                 RR_HISTOGRAM_TICK_NS) {
        s->histogram_next_ns += ticks * RR_HISTOGRAM_TICK_NS;
    } else {
        s->histogram_next_ns = INT64_MAX;
    }
}

static void sgi_router_reset_state(SGIRouterState *s, bool reset_links)
{
    unsigned int port;

    s->status_control = RR_STATUS_OUTPUTS_RESET;
    s->local_errors = 0;
    s->protection_config = RR_PROTECTION_RESET;
    s->global_parms = RR_GLOBAL_RESET;
    s->scratch0 = 0;
    s->scratch1 = 0;
    s->diag_parms = 0;
    s->histogram_next_ns = 0;
    s->histogram_phase_ns = 0;
    timer_del(s->nic_timer);
    sgi_router_nic_idle(s);
    s->nic_ulan = RR_NIC_RESET;
    s->bist_data = 0;
    memset(s->bist_loaded, 0, sizeof(s->bist_loaded));
    s->bist_unload_position = 0;
    s->bist_count = 0;
    s->bist_target = 0;
    s->bist_run_target = 0;
    s->bist_completed_target = 0;
    s->bist_ready = false;

    for (port = 0; port < SGI_ROUTER_PORTS; port++) {
        s->port_parms[port] = RR_PORT_PARMS_RESET;
        s->port_errors[port] = 0;
        s->histograms[port] = 0;
        s->reset_masks[port] = RR_RESET_MASK_MASK;
        if (reset_links) {
            s->port_working[port] = false;
            s->port_eight_bit[port] = false;
        }
    }
    memset(s->meta_table, 0, sizeof(s->meta_table));
    memset(s->local_table, 0, sizeof(s->local_table));
    memset(s->meta_valid, 0, sizeof(s->meta_valid));
    memset(s->local_valid, 0, sizeof(s->local_valid));
}

static void sgi_router_reset_complete(SGIRouterState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->histogram_next_ns = now <= INT64_MAX - RR_HISTOGRAM_TICK_NS ?
                           now + RR_HISTOGRAM_TICK_NS : INT64_MAX;
    s->histogram_phase_ns = RR_HISTOGRAM_TICK_NS;
    s->bist_ready = true;
    sgi_router_changed(s);
}

static unsigned int sgi_router_bist_input_bits(unsigned int target)
{
    switch (target) {
    case RR_BIST_TARGET_LOGIC:
        return RR_BIST_LOGIC_INPUT_BITS;
    case RR_BIST_TARGET_ARRAY:
        return RR_BIST_ARRAY_INPUT_BITS;
    case RR_BIST_TARGET_CLOCK:
        return RR_BIST_CLOCK_INPUT_BITS;
    default:
        return 0;
    }
}

static unsigned int sgi_router_bist_result_bits(unsigned int target)
{
    switch (target) {
    case RR_BIST_TARGET_LOGIC:
        return RR_BIST_LOGIC_UNLOAD_BITS;
    case RR_BIST_TARGET_ARRAY:
        return RR_BIST_ARRAY_UNLOAD_BITS;
    default:
        return 0;
    }
}

static bool sgi_router_bist_select(SGIRouterState *s, uint64_t value)
{
    unsigned int target = extract64(value, 6, 2);

    if (!s->bist_ready || !sgi_router_bist_input_bits(target)) {
        return false;
    }
    if (s->bist_target != target) {
        s->bist_unload_position = 0;
    }
    s->bist_target = target;
    s->bist_count = extract64(value, 0, 6);
    if (s->bist_count == 0) {
        s->bist_count = 64;
    }
    return true;
}

static bool sgi_router_bist_shift_load(SGIRouterState *s)
{
    unsigned int input_bits;

    if (!s->bist_ready) {
        return false;
    }
    input_bits = sgi_router_bist_input_bits(s->bist_target);
    if (!input_bits || s->bist_loaded[s->bist_target] + s->bist_count >
                       input_bits) {
        return false;
    }
    s->bist_loaded[s->bist_target] += s->bist_count;
    if (s->bist_target != RR_BIST_TARGET_CLOCK) {
        s->bist_run_target = s->bist_target;
    }
    return true;
}

static bool sgi_router_bist_shift_unload(SGIRouterState *s)
{
    unsigned int result_bits;

    if (!s->bist_ready || s->bist_target != s->bist_completed_target) {
        s->bist_data = 0;
        return true;
    }
    result_bits = sgi_router_bist_result_bits(s->bist_target);
    if (!result_bits || s->bist_unload_position + s->bist_count >
                        result_bits) {
        s->bist_data = 0;
        return true;
    }
    /*
     * The deterministic opaque result is zero.  Raw result bit 62 presents
     * the completed status as value 2 after firmware reverses and assembles
     * the shifted chunks.  Keep the bit at its chunk-relative position when
     * the result is unloaded in pieces smaller than a full word.
     */
    s->bist_data = 0;
    if (s->bist_unload_position <= 62 &&
        s->bist_unload_position + s->bist_count > 62) {
        s->bist_data = UINT64_C(1) << (62 - s->bist_unload_position);
    }
    s->bist_unload_position += s->bist_count;
    return true;
}

static void sgi_router_bist_run(SGIRouterState *s)
{
    unsigned int target = s->bist_run_target;
    bool complete = s->bist_loaded[RR_BIST_TARGET_CLOCK] ==
                    RR_BIST_CLOCK_INPUT_BITS &&
                    ((target == RR_BIST_TARGET_LOGIC &&
                      s->bist_loaded[target] == RR_BIST_LOGIC_INPUT_BITS) ||
                     (target == RR_BIST_TARGET_ARRAY &&
                      s->bist_loaded[target] == RR_BIST_ARRAY_INPUT_BITS));

    sgi_router_reset_state(s, true);
    if (complete) {
        s->bist_completed_target = target;
    }
    sgi_router_reset_complete(s);
}

static bool sgi_router_read(SGIRouterState *s, unsigned int arrival_port,
                            hwaddr addr, uint64_t *value)
{
    uint32_t *table;
    bool *table_valid = NULL;
    unsigned int port;
    hwaddr offset;

    table = sgi_router_table_entry(s, addr, &table_valid);
    if (table) {
        *value = *table;
        return true;
    }

    if (sgi_router_port_register(addr, &port, &offset)) {
        switch (offset) {
        case RR_PORT_PARMS:
            *value = s->port_parms[port];
            return true;
        case RR_STATUS_ERROR:
            *value = s->port_errors[port];
            return true;
        case RR_HISTOGRAM:
            sgi_router_histogram_sync(s);
            *value = s->histograms[port];
            return true;
        case RR_RESET_MASK:
            *value = s->reset_masks[port];
            return true;
        case RR_ERROR_CLEAR:
            *value = s->port_errors[port];
            s->port_errors[port] = 0;
            return true;
        default:
            return false;
        }
    }

    switch (addr) {
    case RR_STATUS_REV_ID:
        *value = sgi_router_status(s, arrival_port);
        return true;
    case RR_PROTECTION_CONFIG:
        *value = s->protection_config;
        return true;
    case RR_GLOBAL_PARMS:
        *value = s->global_parms;
        return true;
    case RR_SCRATCH_REG0:
        *value = s->scratch0;
        return true;
    case RR_SCRATCH_REG1:
        *value = s->scratch1;
        return true;
    case RR_DIAG_PARMS:
        *value = s->diag_parms;
        return true;
    case RR_NIC_ULAN:
        *value = s->nic_ulan;
        return true;
    case RR_BIST_DATA:
        *value = s->bist_data;
        return true;
    case RR_BIST_READY:
        *value = s->bist_ready;
        return true;
    default:
        return false;
    }
}

static bool sgi_router_write(SGIRouterState *s, hwaddr addr, uint64_t value)
{
    uint32_t *table;
    bool *table_valid = NULL;
    unsigned int port;
    hwaddr offset;

    table = sgi_router_table_entry(s, addr, &table_valid);
    if (table) {
        *table = value & RR_TABLE_MASK;
        *table_valid = true;
        sgi_router_changed(s);
        return true;
    }

    if (sgi_router_port_register(addr, &port, &offset)) {
        switch (offset) {
        case RR_PORT_PARMS:
        {
            unsigned int old_mode = extract64(
                s->port_parms[port], RR_PORT_HISTOGRAM_SELECT_SHIFT, 2);
            unsigned int mode = extract64(value,
                                           RR_PORT_HISTOGRAM_SELECT_SHIFT, 2);

            sgi_router_histogram_sync(s);
            s->port_parms[port] = value & RR_PORT_PARMS_MASK;
            if (mode != RR_HISTOGRAM_UTILISATION &&
                old_mode != mode) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-router: histogram selector %u on port %u "
                              "is not implemented\n", mode, port + 1);
            }
            return true;
        }
        case RR_STATUS_ERROR:
            s->port_errors[port] &= ~(value & RR_PORT_ERROR_MASK);
            return true;
        case RR_HISTOGRAM:
            sgi_router_histogram_sync(s);
            if (value == 0) {
                s->histograms[port] = 0;
            } else {
                s->histograms[port] &= ~value;
            }
            return true;
        case RR_RESET_MASK:
            s->reset_masks[port] = value & RR_RESET_MASK_MASK;
            return true;
        case RR_ERROR_CLEAR:
            if (value == 0) {
                s->port_errors[port] = 0;
                return true;
            }
            qemu_log_mask(LOG_UNIMP,
                          "sgi-router: nonzero write to the error-clearing "
                          "alias is not defined\n");
            return false;
        default:
            return false;
        }
    }

    switch (addr) {
    case RR_STATUS_REV_ID:
        s->local_errors &= ~(value & RR_STATUS_LOCAL_ERROR_MASK);
        s->status_control = value & RR_STATUS_CONTROL_MASK;
        return true;
    case RR_PORT_RESET:
        if (value & RR_RESET_WARM) {
            sgi_router_reset_state(s, true);
            sgi_router_reset_complete(s);
        } else if (value & RR_RESET_ADMIN) {
            sgi_router_reset_state(s, false);
            sgi_router_reset_complete(s);
        } else if (value & RR_RESET_PORT_MASK) {
            /* There is no queued transport state in the bounded model. */
            sgi_router_changed(s);
        }
        return true;
    case RR_PROTECTION_CONFIG:
        s->protection_config = value & RR_PROTECTION_MASK;
        sgi_router_changed(s);
        return true;
    case RR_GLOBAL_PARMS:
        s->global_parms = (value & ~RR_GLOBAL_READ_ONLY) |
                          RR_GLOBAL_READ_ONLY;
        return true;
    case RR_SCRATCH_REG0:
        s->scratch0 = value;
        return true;
    case RR_SCRATCH_REG1:
        s->scratch1 = value & RR_SCRATCH1_MASK;
        return true;
    case RR_DIAG_PARMS:
    {
        bool absolute = value & RR_DIAG_ABSOLUTE_HISTOGRAM;

        sgi_router_histogram_sync(s);
        if (absolute && !(s->diag_parms & RR_DIAG_ABSOLUTE_HISTOGRAM)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-router: absolute histogram mode is not "
                          "implemented\n");
        }
        s->diag_parms = value & RR_DIAG_MASK;
        sgi_router_changed(s);
        return true;
    }
    case RR_NIC_ULAN:
        return sgi_router_nic_write(s, value);
    case RR_BIST_DATA:
        s->bist_data = value;
        return true;
    case RR_BIST_READY:
        return false;
    case RR_BIST_COUNT_TARGET:
        return sgi_router_bist_select(s, value);
    case RR_BIST_SHIFT_LOAD:
        return sgi_router_bist_shift_load(s);
    case RR_BIST_SHIFT_UNLOAD:
        return sgi_router_bist_shift_unload(s);
    case RR_BIST_ENTER_RUN:
        sgi_router_bist_run(s);
        return true;
    default:
        return false;
    }
}

bool sgi_router_access(SGIRouterState *s, unsigned int arrival_port,
                       SN0FabricVectorAccess *access)
{
    bool accepted;

    if (arrival_port < 1 || arrival_port > SGI_ROUTER_PORTS ||
        (access->addr & 7) != 0 || access->addr >= (UINT64_C(1) << 20)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-router: invalid vector access at 0x%" HWADDR_PRIx
                      " from port %u\n", access->addr, arrival_port);
        return false;
    }

    switch (access->op) {
    case SN0_FABRIC_VECTOR_READ:
        accepted = sgi_router_read(s, arrival_port, access->addr,
                                   &access->value);
        break;
    case SN0_FABRIC_VECTOR_WRITE:
        accepted = sgi_router_write(s, access->addr, access->data);
        break;
    case SN0_FABRIC_VECTOR_EXCHANGE:
        if (access->addr != RR_SCRATCH_REG1) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-router: exchange outside scratch register 1 "
                          "is not established\n");
            return false;
        }
        access->value = s->scratch1;
        if (s->scratch1 == 0) {
            s->scratch1 = access->data & RR_SCRATCH1_MASK;
        }
        return true;
    default:
        return false;
    }

    if (!accepted) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-router: unsupported vector operation at 0x%"
                      HWADDR_PRIx "\n", access->addr);
    }
    return accepted;
}

bool sgi_router_port_eight_bit_forced(SGIRouterState *s, unsigned int port)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    return s->diag_parms & (UINT64_C(1) << (11 + port));
}

bool sgi_router_port_enabled(SGIRouterState *s, unsigned int port)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    return !(s->diag_parms & (UINT64_C(1) << (5 + port)));
}

bool sgi_router_port_sends_data_error(SGIRouterState *s, unsigned int port)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    return s->diag_parms & (UINT64_C(1) << (port - 1));
}

void sgi_router_record_port_error(SGIRouterState *s, unsigned int port,
                                  SGIRouterPortError error)
{
    unsigned int shift;
    uint64_t count;

    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    switch (error) {
    case SGI_ROUTER_PORT_SEQUENCE_ERROR:
        shift = RR_PORT_SEQUENCE_ERROR_SHIFT;
        break;
    case SGI_ROUTER_PORT_CHECK_ERROR:
        shift = RR_PORT_CHECK_ERROR_SHIFT;
        break;
    case SGI_ROUTER_PORT_RETRY:
        shift = RR_PORT_RETRY_SHIFT;
        break;
    default:
        g_assert_not_reached();
    }

    count = extract64(s->port_errors[port - 1], shift, 8);
    if (count != RR_PORT_COUNTER_MASK) {
        s->port_errors[port - 1] =
            deposit64(s->port_errors[port - 1], shift, 8, count + 1);
    }
}

void sgi_router_set_changed(SGIRouterState *s, void (*changed)(void *),
                            void *opaque)
{
    s->changed = changed;
    s->changed_opaque = opaque;
}

void sgi_router_set_port_link(SGIRouterState *s, unsigned int port,
                              bool working, bool eight_bit)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    s->port_working[port - 1] = working;
    s->port_eight_bit[port - 1] = working && eight_bit;
}

void sgi_router_latch_local_error(SGIRouterState *s, uint64_t errors)
{
    s->local_errors |= errors & RR_STATUS_LOCAL_ERROR_MASK;
}

void sgi_router_latch_port_error(SGIRouterState *s, unsigned int port,
                                 uint64_t errors)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    s->port_errors[port - 1] |= errors & RR_PORT_ERROR_MASK;
}

void sgi_router_histogram_receive(SGIRouterState *s, unsigned int port,
                                  bool accepted, bool queue_empty,
                                  bool output_uncontended)
{
    unsigned int bucket_mask = 1U << 1;

    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    sgi_router_histogram_sync(s);
    if (!accepted || !sgi_router_histogram_progresses(s, port - 1)) {
        return;
    }
    if (queue_empty && output_uncontended) {
        bucket_mask |= 1U << 0;
    }
    sgi_router_histogram_increment(s, port - 1, bucket_mask);
}

void sgi_router_histogram_send(SGIRouterState *s, unsigned int port,
                               bool accepted)
{
    g_assert(port >= 1 && port <= SGI_ROUTER_PORTS);
    sgi_router_histogram_sync(s);
    if (accepted && sgi_router_histogram_progresses(s, port - 1)) {
        sgi_router_histogram_increment(s, port - 1, 1U << 2);
    }
}

bool sgi_router_resolve_turn(unsigned int entry_port, uint8_t turn,
                             unsigned int *exit_port)
{
    if (entry_port < 1 || entry_port > SGI_ROUTER_PORTS ||
        turn > 5) {
        return false;
    }
    *exit_port = ((entry_port - 1 + turn) % SGI_ROUTER_PORTS) + 1;
    return true;
}

bool sgi_router_route(SGIRouterState *s, unsigned int entry_port,
                      uint8_t turn, uint16_t destination,
                      unsigned int *exit_port, uint8_t *next_turn)
{
    uint32_t entry;
    bool local;
    unsigned int index;

    if (!sgi_router_resolve_turn(entry_port, turn, exit_port)) {
        return false;
    }

    local = s->protection_config & RR_PROTECTION_FORCE_LOCAL;
    if (!local) {
        uint8_t meta;

        if (!(s->protection_config & RR_PROTECTION_META_VALID)) {
            return false;
        }
        meta = (s->protection_config >> RR_PROTECTION_META_SHIFT) &
               RR_PROTECTION_META_MASK;
        local = ((destination >> 4) & 0x1f) == meta;
    }

    if (local) {
        index = destination & 0xf;
        if (!s->local_valid[index]) {
            return false;
        }
        entry = s->local_table[index];
    } else {
        index = (destination >> 4) & 0x1f;
        if (!s->meta_valid[index]) {
            return false;
        }
        entry = s->meta_table[index];
    }
    *next_turn = extract32(entry, 4 * (*exit_port - 1), 4);
    return true;
}

static void sgi_router_reset_enter(Object *obj, ResetType type)
{
    SGIRouterState *s = SGI_ROUTER(obj);

    sgi_router_reset_state(s, true);
}

static void sgi_router_reset_exit(Object *obj, ResetType type)
{
    SGIRouterState *s = SGI_ROUTER(obj);

    sgi_router_reset_complete(s);
}

static bool sgi_router_array_masked(const uint64_t *values, size_t count,
                                    uint64_t mask)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (values[i] & ~mask) {
            return false;
        }
    }
    return true;
}

static int sgi_router_pre_load(void *opaque)
{
    SGIRouterState *s = opaque;

    timer_del(s->nic_timer);
    s->histogram_next_ns = 0;
    s->histogram_phase_ns = 0;
    return 0;
}

static int sgi_router_pre_save(void *opaque)
{
    SGIRouterState *s = opaque;
    int64_t now;
    int64_t phase;

    sgi_router_histogram_sync(s);
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    phase = s->histogram_next_ns - now;
    if (phase < 1 || phase > RR_HISTOGRAM_TICK_NS) {
        return -EINVAL;
    }
    s->histogram_phase_ns = phase;
    return 0;
}

static bool sgi_router_nic_state_valid(SGIRouterState *s)
{
    if (s->nic_phase == ROUTER_NIC_IDLE) {
        return s->nic_candidate == 0 && s->nic_pulse == 0 &&
               s->nic_sample == 0 && s->nic_release_ns == 0 &&
               s->nic_sample_ns == 0 && (s->nic_ulan & RR_NIC_DONE) &&
               ((s->nic_ulan & RR_NIC_PHASE_MASK) ==
                (RR_NIC_RESET & RR_NIC_PHASE_MASK) ||
                sgi_router_nic_calibrated(s, s->nic_ulan));
    }
    if (s->nic_phase > ROUTER_NIC_SAMPLING ||
        !sgi_router_nic_calibrated(s, s->nic_ulan) ||
        !sgi_router_nic_waveform(s->nic_pulse, s->nic_sample) ||
        (s->nic_ulan & 3) || s->nic_candidate > 1 ||
        (s->nic_phase == ROUTER_NIC_DRIVING && s->nic_candidate) ||
        s->nic_release_ns < s->nic_pulse * 1000 ||
        s->nic_release_ns > INT64_MAX - s->nic_sample * 1000 ||
        s->nic_sample_ns != s->nic_release_ns + s->nic_sample * 1000) {
        return false;
    }
    if (s->nic_phase == ROUTER_NIC_SAMPLING) {
        if (s->nic_pulse == 520) {
            return s->nic_candidate == !s->nic_present;
        }
        if (s->nic_sample != 13 || !s->nic_present) {
            return s->nic_candidate == 1;
        }
    }
    return true;
}

static bool sgi_router_bist_state_valid(SGIRouterState *s)
{
    unsigned int result_bits =
        sgi_router_bist_result_bits(s->bist_completed_target);

    return s->bist_target <= RR_BIST_TARGET_CLOCK &&
           s->bist_run_target <= RR_BIST_TARGET_ARRAY &&
           s->bist_completed_target <= RR_BIST_TARGET_ARRAY &&
           s->bist_count <= 64 &&
           s->bist_loaded[0] == 0 &&
           s->bist_loaded[RR_BIST_TARGET_LOGIC] <=
               RR_BIST_LOGIC_INPUT_BITS &&
           s->bist_loaded[RR_BIST_TARGET_ARRAY] <=
               RR_BIST_ARRAY_INPUT_BITS &&
           s->bist_loaded[RR_BIST_TARGET_CLOCK] <=
               RR_BIST_CLOCK_INPUT_BITS &&
           (!s->bist_completed_target ||
            (s->bist_ready && result_bits &&
             s->bist_unload_position <= result_bits));
}

static int sgi_router_post_load(void *opaque, int version_id)
{
    SGIRouterState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned int i;

    if (s->histogram_phase_ns < 1 ||
        s->histogram_phase_ns > RR_HISTOGRAM_TICK_NS ||
        now > INT64_MAX - s->histogram_phase_ns ||
        !sgi_router_nic_state_valid(s) ||
        !sgi_router_bist_state_valid(s) ||
        (s->status_control & ~RR_STATUS_CONTROL_MASK) ||
        (s->local_errors & ~RR_STATUS_LOCAL_ERROR_MASK) ||
        (s->protection_config & ~RR_PROTECTION_MASK) ||
        (s->global_parms & RR_GLOBAL_READ_ONLY) != RR_GLOBAL_READ_ONLY ||
        (s->diag_parms & ~RR_DIAG_MASK) ||
        (s->nic_ulan & ~RR_NIC_STATE_MASK) ||
        !sgi_router_array_masked(s->port_parms, SGI_ROUTER_PORTS,
                                 RR_PORT_PARMS_MASK) ||
        !sgi_router_array_masked(s->port_errors, SGI_ROUTER_PORTS,
                                 RR_PORT_ERROR_MASK)) {
        return -EINVAL;
    }
    for (i = 0; i < SGI_ROUTER_PORTS; i++) {
        if (s->reset_masks[i] & ~RR_RESET_MASK_MASK ||
            (!s->port_working[i] && s->port_eight_bit[i])) {
            return -EINVAL;
        }
    }
    for (i = 0; i < RR_META_TABLE_COUNT; i++) {
        if (s->meta_table[i] & ~RR_TABLE_MASK) {
            return -EINVAL;
        }
    }
    for (i = 0; i < RR_LOCAL_TABLE_COUNT; i++) {
        if (s->local_table[i] & ~RR_TABLE_MASK) {
            return -EINVAL;
        }
    }
    s->histogram_next_ns = now + s->histogram_phase_ns;
    if (s->nic_phase != ROUTER_NIC_IDLE) {
        timer_mod(s->nic_timer, s->nic_phase == ROUTER_NIC_DRIVING ?
                               s->nic_release_ns : s->nic_sample_ns);
    }
    sgi_router_changed(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_router = {
    .name = "sgi-router",
    .version_id = 4,
    .minimum_version_id = 4,
    .pre_save = sgi_router_pre_save,
    .pre_load = sgi_router_pre_load,
    .post_load = sgi_router_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(status_control, SGIRouterState),
        VMSTATE_UINT64(local_errors, SGIRouterState),
        VMSTATE_UINT64(protection_config, SGIRouterState),
        VMSTATE_UINT64(global_parms, SGIRouterState),
        VMSTATE_UINT64(scratch0, SGIRouterState),
        VMSTATE_UINT16(scratch1, SGIRouterState),
        VMSTATE_UINT64(diag_parms, SGIRouterState),
        VMSTATE_UINT64(nic_ulan, SGIRouterState),
        VMSTATE_UINT64(bist_data, SGIRouterState),
        VMSTATE_UINT16_ARRAY(bist_loaded, SGIRouterState, 4),
        VMSTATE_UINT16(bist_unload_position, SGIRouterState),
        VMSTATE_UINT8(bist_count, SGIRouterState),
        VMSTATE_UINT8(bist_target, SGIRouterState),
        VMSTATE_UINT8(bist_run_target, SGIRouterState),
        VMSTATE_UINT8(bist_completed_target, SGIRouterState),
        VMSTATE_BOOL(bist_ready, SGIRouterState),
        VMSTATE_UINT64_ARRAY(port_parms, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(port_errors, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT64_ARRAY(histograms, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT8(histogram_phase_ns, SGIRouterState),
        VMSTATE_UINT8_ARRAY(reset_masks, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_BOOL_ARRAY(port_working, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_BOOL_ARRAY(port_eight_bit, SGIRouterState, SGI_ROUTER_PORTS),
        VMSTATE_UINT32_ARRAY(meta_table, SGIRouterState, RR_META_TABLE_COUNT),
        VMSTATE_UINT32_ARRAY(local_table, SGIRouterState,
                             RR_LOCAL_TABLE_COUNT),
        VMSTATE_BOOL_ARRAY(meta_valid, SGIRouterState, RR_META_TABLE_COUNT),
        VMSTATE_BOOL_ARRAY(local_valid, SGIRouterState,
                           RR_LOCAL_TABLE_COUNT),
        VMSTATE_UINT64_EQUAL(nic_clock_hz, SGIRouterState),
        VMSTATE_UINT64_EQUAL(nic_serial, SGIRouterState),
        VMSTATE_UINT8_EQUAL(nic_population, SGIRouterState),
        VMSTATE_UINT8(nic_phase, SGIRouterState),
        VMSTATE_UINT8(nic_candidate, SGIRouterState),
        VMSTATE_UINT16(nic_pulse, SGIRouterState),
        VMSTATE_UINT16(nic_sample, SGIRouterState),
        VMSTATE_INT64(nic_release_ns, SGIRouterState),
        VMSTATE_INT64(nic_sample_ns, SGIRouterState),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_router_realize(DeviceState *dev, Error **errp)
{
    SGIRouterState *s = SGI_ROUTER(dev);

    if (s->chip_inputs > 0xf) {
        error_setg(errp, "chip-inputs must fit in four bits");
        return;
    }
    if (s->nic_serial >> 48) {
        error_setg(errp, "nic-serial must fit in 48 bits");
        return;
    }
    if (s->nic_image && !s->nic_present) {
        error_setg(errp, "nic-image requires nic-present");
        return;
    }
    if (s->nic_present) {
        g_autofree char *image = NULL;
        gsize length = 0;

        if (s->nic_image) {
            g_autoptr(GError) error = NULL;
            GStatBuf image_stat;

            if (g_stat(s->nic_image, &image_stat) < 0) {
                error_setg_errno(errp, errno, "cannot stat NIC image '%s'",
                                 s->nic_image);
                return;
            }
            if (image_stat.st_size > DS2505_DATA_SIZE) {
                error_setg(errp, "NIC image '%s' exceeds 2048 bytes",
                           s->nic_image);
                return;
            }
            if (!g_file_get_contents(s->nic_image, &image, &length, &error)) {
                error_setg(errp, "cannot read NIC image '%s': %s",
                           s->nic_image, error->message);
                return;
            }
            if (length > DS2505_DATA_SIZE) {
                error_setg(errp, "NIC image '%s' exceeds 2048 bytes",
                           s->nic_image);
                return;
            }
        }
        s->nic = qdev_new(TYPE_ONEWIRE_DS2505);
        qdev_prop_set_uint8(s->nic, "family", 0x0b);
        qdev_prop_set_uint64(s->nic, "serial", s->nic_serial);
        if (image) {
            ds2505_set_image(s->nic, (uint8_t *)image, length);
        }
        object_property_add_child(OBJECT(dev), "nic", OBJECT(s->nic));
        if (!qdev_realize_and_unref(s->nic, BUS(s->nic_bus), errp)) {
            return;
        }
    }
    s->nic_population = s->nic_present;
    qemu_register_resettable(OBJECT(dev));
}

static void sgi_router_unrealize(DeviceState *dev)
{
    SGIRouterState *s = SGI_ROUTER(dev);

    timer_del(s->nic_timer);
    if (s->nic) {
        qdev_unrealize(s->nic);
    }
    qemu_unregister_resettable(OBJECT(dev));
}

static void sgi_router_init(Object *obj)
{
    SGIRouterState *s = SGI_ROUTER(obj);

    s->nic_bus = onewire_create_bus(DEVICE(obj), "onewire");
    s->nic_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_router_nic_tick, s);
}

static void sgi_router_finalize(Object *obj)
{
    timer_free(SGI_ROUTER(obj)->nic_timer);
}

static const Property sgi_router_properties[] = {
    DEFINE_PROP_UINT8("chip-inputs", SGIRouterState, chip_inputs, 1),
    DEFINE_PROP_UINT64("nic-clock-hz", SGIRouterState, nic_clock_hz, 100000000),
    DEFINE_PROP_UINT64("nic-serial", SGIRouterState, nic_serial, 0),
    DEFINE_PROP_BOOL("nic-present", SGIRouterState, nic_present, true),
    DEFINE_PROP_STRING("nic-image", SGIRouterState, nic_image),
};

static void sgi_router_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI Router CrayLink switch";
    dc->realize = sgi_router_realize;
    dc->unrealize = sgi_router_unrealize;
    dc->vmsd = &vmstate_sgi_router;
    dc->user_creatable = false;
    device_class_set_props(dc, sgi_router_properties);
    rc->phases.enter = sgi_router_reset_enter;
    rc->phases.exit = sgi_router_reset_exit;
}

static const TypeInfo sgi_router_types[] = {
    {
        .name = TYPE_SGI_ROUTER,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIRouterState),
        .class_init = sgi_router_class_init,
        .instance_init = sgi_router_init,
        .instance_finalize = sgi_router_finalize,
    },
};

DEFINE_TYPES(sgi_router_types)
