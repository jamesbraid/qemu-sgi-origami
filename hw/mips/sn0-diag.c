/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) host diagnostics.
 *
 * The flight recorder behind the diagnostic-recording machine option and the
 * x-query-origin-state QMP command. Host-side observation only: nothing here
 * is guest-visible, nothing migrates, and a machine built without recording
 * behaves identically.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/reset.h"
#include "sn0.h"

#define SN0_DIAGNOSTIC_EVENT_COUNT 4096
#define SN0_DIAGNOSTIC_READ_CACHE_COUNT 256

struct SN0DiagnosticEvent {
    uint64_t sequence;
    int64_t virtual_time_ns;
    uint32_t node;
    const char *component;
    const char *operation;
    uint64_t address;
    uint64_t value;
    uint8_t size;
    bool has_cpu_index;
    int cpu_index;
    bool has_pc;
    uint64_t pc;
    bool has_before_after;
    uint64_t before;
    uint64_t after;
};

struct SN0DiagnosticReadCacheEntry {
    const char *component;
    uint64_t address;
    uint64_t value;
    uint64_t before;
    uint64_t after;
    uint64_t pc;
    uint64_t sequence;
    int cpu_index;
    uint8_t size;
    bool has_cpu_index;
    bool has_pc;
    bool has_before_after;
    bool valid;
};

static void sn0_diagnostic_invalidate_reads(SN0MachineState *s,
                                            uint32_t node)
{
    SN0DiagnosticReadCacheEntry *cache;

    if (!s->diagnostic_read_cache) {
        return;
    }
    cache = &s->diagnostic_read_cache[
        node * SN0_DIAGNOSTIC_READ_CACHE_COUNT];
    memset(cache, 0, sizeof(*cache) * SN0_DIAGNOSTIC_READ_CACHE_COUNT);
    s->diagnostic_read_cache_next[node] = 0;
}

static bool sn0_diagnostic_read_repeated(SN0MachineState *s, uint32_t node,
                                         const char *component,
                                         uint64_t address, uint64_t value,
                                         unsigned int size,
                                         bool has_cpu_index, int cpu_index,
                                         bool has_pc, uint64_t pc,
                                         bool has_before_after,
                                         uint64_t before, uint64_t after)
{
    SN0DiagnosticReadCacheEntry *cache;
    SN0DiagnosticReadCacheEntry *entry = NULL;
    uint64_t oldest_sequence = s->diagnostic_event_next_sequence -
                               s->diagnostic_event_count;
    unsigned int i;

    if (!s->diagnostic_read_cache) {
        s->diagnostic_read_cache = g_new0(
            SN0DiagnosticReadCacheEntry,
            SN0_ORIGIN200_MAX_NODES * SN0_DIAGNOSTIC_READ_CACHE_COUNT);
    }
    cache = &s->diagnostic_read_cache[
        node * SN0_DIAGNOSTIC_READ_CACHE_COUNT];
    for (i = 0; i < SN0_DIAGNOSTIC_READ_CACHE_COUNT; i++) {
        if (cache[i].valid && cache[i].address == address &&
            cache[i].size == size &&
            cache[i].has_cpu_index == has_cpu_index &&
            (!has_cpu_index || cache[i].cpu_index == cpu_index) &&
            cache[i].has_pc == has_pc &&
            (!has_pc || cache[i].pc == pc) &&
            !strcmp(cache[i].component, component)) {
            entry = &cache[i];
            break;
        }
    }
    if (!entry) {
        entry = &cache[s->diagnostic_read_cache_next[node]++];
        s->diagnostic_read_cache_next[node] %=
            SN0_DIAGNOSTIC_READ_CACHE_COUNT;
    } else if (entry->value == value &&
               entry->has_before_after == has_before_after &&
               (!has_before_after ||
                (entry->before == before && entry->after == after)) &&
               entry->sequence >= oldest_sequence) {
        return true;
    }

    *entry = (SN0DiagnosticReadCacheEntry) {
        .component = component,
        .address = address,
        .value = value,
        .before = before,
        .after = after,
        .pc = pc,
        .sequence = s->diagnostic_event_next_sequence,
        .cpu_index = cpu_index,
        .size = size,
        .has_cpu_index = has_cpu_index,
        .has_pc = has_pc,
        .has_before_after = has_before_after,
        .valid = true,
    };
    return false;
}

static void sn0_diagnostic_record(void *opaque, const char *component,
                                  const char *operation, uint64_t address,
                                  uint64_t value, unsigned int size,
                                  bool has_before_after, uint64_t before,
                                  uint64_t after)
{
    SN0DiagnosticSinkContext *context = opaque;
    SN0MachineState *s = context->machine;
    SN0DiagnosticEvent *event;
    CPUState *cpu;
    bool has_cpu_index;
    int cpu_index;
    bool has_pc;
    uint64_t pc;

    if (unlikely(!s->diagnostic_recording)) {
        return;
    }
    assert(bql_locked());
    cpu = current_cpu;
    has_cpu_index = cpu != NULL;
    cpu_index = has_cpu_index ? cpu->cpu_index : 0;
    has_pc = cpu != NULL;
    pc = has_pc ? cpu->cc->get_pc(cpu) : 0;
    if (!strcmp(operation, "mmio-read")) {
        if (sn0_diagnostic_read_repeated(s, context->node, component,
                                         address, value, size,
                                         has_cpu_index, cpu_index, has_pc, pc,
                                         has_before_after, before, after)) {
            return;
        }
    } else {
        sn0_diagnostic_invalidate_reads(s, context->node);
    }
    if (!s->diagnostic_events) {
        s->diagnostic_events = g_new0(SN0DiagnosticEvent,
                                      SN0_DIAGNOSTIC_EVENT_COUNT);
    }
    event = &s->diagnostic_events[s->diagnostic_event_head];

    event->sequence = s->diagnostic_event_next_sequence++;
    event->virtual_time_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    event->node = context->node;
    event->component = component;
    event->operation = operation;
    event->address = address;
    event->value = value;
    event->size = size;
    if (has_cpu_index) {
        event->has_cpu_index = true;
        event->cpu_index = cpu_index;
    } else {
        event->has_cpu_index = false;
    }
    if (has_pc) {
        event->has_pc = true;
        event->pc = pc;
    } else {
        event->has_pc = false;
    }
    event->has_before_after = has_before_after;
    event->before = before;
    event->after = after;

    s->diagnostic_event_head =
        (s->diagnostic_event_head + 1) % SN0_DIAGNOSTIC_EVENT_COUNT;
    if (s->diagnostic_event_count < SN0_DIAGNOSTIC_EVENT_COUNT) {
        s->diagnostic_event_count++;
    } else {
        s->diagnostic_event_dropped++;
    }
}

static void sn0_diagnostic_reset(void *opaque)
{
    SN0MachineState *s = opaque;

    if (!s->diagnostic_recording) {
        return;
    }
    s->diagnostic_event_head = 0;
    s->diagnostic_event_count = 0;
    s->diagnostic_event_next_sequence = 0;
    s->diagnostic_event_dropped = 0;
    for (unsigned int i = 0; i < SN0_ORIGIN200_MAX_NODES; i++) {
        sn0_diagnostic_invalidate_reads(s, i);
    }
}

SgiOriginState *sn0_query_origin_state(uint32_t event_limit, Error **errp)
{
    BQL_LOCK_GUARD();
    MachineState *machine = MACHINE(qdev_get_machine());
    SN0MachineState *s;
    SgiOriginState *result;
    SgiOriginCpuStateList **cpu_tail;
    SgiOriginHubStateList **hub_tail;
    unsigned int node_count;
    CPUState *cs;
    unsigned int i;

    if (!object_dynamic_cast(OBJECT(machine), TYPE_SN0_MACHINE)) {
        error_setg(errp, "current machine is not an SGI Origin");
        return NULL;
    }
    s = SN0_MACHINE(machine);
    result = g_new0(SgiOriginState, 1);
    result->query_sequence = ++s->diagnostic_query_sequence;

    cpu_tail = &result->cpus;
    CPU_FOREACH(cs) {
        SgiOriginCpuState *cpu = g_new0(SgiOriginCpuState, 1);

        cpu->index = cs->cpu_index;
        cpu->interrupt_request = qatomic_load_acquire(&cs->interrupt_request);
        QAPI_LIST_APPEND(cpu_tail, cpu);
    }

    node_count = machine->numa_state && machine->numa_state->num_nodes ?
                 machine->numa_state->num_nodes : 1;
    hub_tail = &result->hubs;
    for (i = 0; i < node_count; i++) {
        SGIHubState *hub = &s->nodes[i].hub;
        PCF8584State *pcf = &hub->i2c;
        SGIElscState *elsc = hub->elsc;
        SgiOriginHubState *info = g_new0(SgiOriginHubState, 1);

        info->node = i;
        info->nasid = sgi_hub_programmed_nasid(hub);
        info->cpu_a_enabled = hub->pi_cpu_enable[0];
        info->cpu_b_enabled = hub->pi_cpu_enable[1];

        info->pending0 = qatomic_read(&hub->pi_int_pend[0]);
        info->pending1 = qatomic_read(&hub->pi_int_pend[1]);
        info->mask_a0 = hub->pi_int_mask[0];
        info->mask_a1 = hub->pi_int_mask[1];
        info->mask_b0 = hub->pi_int_mask[2];
        info->mask_b1 = hub->pi_int_mask[3];
        info->rt_compare_a = qatomic_read(&hub->pi_rt_compare[0]);
        info->rt_compare_b = qatomic_read(&hub->pi_rt_compare[1]);
        info->rt_pending_a = hub->pi_rt_pending[0];
        info->rt_pending_b = hub->pi_rt_pending[1];
        info->rt_enabled_a = hub->pi_rt_enable[0];
        info->rt_enabled_b = hub->pi_rt_enable[1];

        info->pcf8584 = g_new0(SgiOriginPcf8584State, 1);
        info->pcf8584->data = pcf->data;
        info->pcf8584->control = pcf->control;
        info->pcf8584->status = pcf->status;
        info->pcf8584->transfer = pcf->transfer;
        info->pcf8584->receiving = pcf->recv;
        info->pcf8584->start_pending = pcf->start_pending;
        info->pcf8584->address_staged = pcf->start_address_staged;
        info->pcf8584->address = pcf->cur_addr7;

        info->elsc = g_new0(SgiOriginElscState, 1);
        if (elsc) {
            info->elsc->rx_length = elsc->rxlen;
            info->elsc->tx_length = elsc->txlen;
            info->elsc->tx_position = elsc->txpos;
            info->elsc->arbitration_code = elsc->arb_code;
            info->elsc->arbitration_present = elsc->arb_present;
            info->elsc->queue_length = elsc->rx_queue_len;
        }

        QAPI_LIST_APPEND(hub_tail, info);
    }

    result->flight_recorder = g_new0(SgiOriginFlightRecorder, 1);
    result->flight_recorder->enabled = s->diagnostic_recording;
    result->flight_recorder->next_sequence =
        s->diagnostic_event_next_sequence;
    result->flight_recorder->dropped = s->diagnostic_event_dropped;
    {
        SgiOriginFlightEventList **event_tail =
            &result->flight_recorder->events;
        uint32_t retained = MIN(s->diagnostic_event_count, event_limit);
        uint32_t oldest =
            (s->diagnostic_event_head + SN0_DIAGNOSTIC_EVENT_COUNT -
             retained) % SN0_DIAGNOSTIC_EVENT_COUNT;

        for (i = 0; i < retained; i++) {
            SN0DiagnosticEvent *source = &s->diagnostic_events[
                (oldest + i) % SN0_DIAGNOSTIC_EVENT_COUNT];
            SgiOriginFlightEvent *event =
                g_new0(SgiOriginFlightEvent, 1);

            event->sequence = source->sequence;
            event->virtual_time_ns = source->virtual_time_ns;
            event->node = source->node;
            event->component = g_strdup(source->component);
            event->operation = g_strdup(source->operation);
            event->address = source->address;
            event->value = source->value;
            event->size = source->size;
            event->has_cpu_index = source->has_cpu_index;
            event->cpu_index = source->cpu_index;
            event->has_pc = source->has_pc;
            event->pc = source->pc;
            event->has_before = source->has_before_after;
            event->before = source->before;
            event->has_after = source->has_before_after;
            event->after = source->after;
            QAPI_LIST_APPEND(event_tail, event);
        }
    }

    return result;
}

/*
 * Wire one node's Hub into the recorder. The sink and its context are set up
 * whether or not recording is on -- the state query works either way -- but
 * the Hub emits through its diagnostic pointer only when the machine was asked
 * to record.
 */
void sn0_diagnostic_attach(SN0MachineState *s, unsigned int node_index)
{
    s->diagnostic_sink_context[node_index].machine = s;
    s->diagnostic_sink_context[node_index].node = node_index;
    s->diagnostic_sink[node_index].record = sn0_diagnostic_record;
    s->diagnostic_sink[node_index].opaque =
        &s->diagnostic_sink_context[node_index];
    if (s->diagnostic_recording) {
        s->nodes[node_index].hub.diagnostic = &s->diagnostic_sink[node_index];
    }
}

void sn0_diagnostic_register_reset(SN0MachineState *s)
{
    qemu_register_reset(sn0_diagnostic_reset, s);
}

void sn0_diagnostic_finalize(SN0MachineState *s)
{
    g_free(s->diagnostic_events);
    g_free(s->diagnostic_read_cache);
}
