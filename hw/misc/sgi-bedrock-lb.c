/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bedrock local block: bounded register storage and atomic scratch aliases.
 * Offsets and names follow Linux ia64 SN1
 * include/asm-ia64/sn/sn1/hublb.h and hublb_next.h.
*/
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-lb.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/mips/sgi-net-port.h"
#include "trace.h"

#define LB_REVISION        0x0000
#define LB_CPU_PERMISSION  0x4000
#define LB_CPU_OVERRIDE    0x4008
#define LB_IO_PERMISSION   0x4010
#define LB_SOFT_RESET      0x4018
#define LB_REGION_PRESENT  0x4020
#define LB_NODES_ABSENT    0x4028
#define LB_MICROLAN        0x4030
#define LB_ERROR_BITS      0x4040
#define LB_ERROR_CLEAR     0x4048
#define LB_ERROR_HEADER_1  0x4050
#define LB_ERROR_HEADER_2  0x4058
#define LB_ERROR_DATA      0x4060
#define LB_DEBUG_SELECT    0x4100
#define LB_DEBUG_PINS      0x4108
#define LB_RTC_LOCAL       0x4200
#define LB_RTC_FILTER      0x4208
#define LB_SCRATCH_BASE    0x8000
#define LB_SCRATCH_WZ_BASE 0x8040
#define LB_SCRATCH_RS_BASE 0x8058
#define LB_VECTOR_PARAMS   0xc000
#define LB_VECTOR_ROUTE    0xc008
#define LB_VECTOR_DATA     0xc010
#define LB_VECTOR_STATUS   0xc020
#define LB_VECTOR_RETURN   0xc028
#define LB_VECTOR_READ     0xc030
#define LB_VECTOR_STATUS_CLEAR 0xc038

#define LB_PASSWORD 0x53474972756c6573ULL
/*
 * Address, write id and request id occupy the same positions in parameters
 * and status, so the reply echoes them in
 * place. The type width is open (one bit in the union, two in the companion
 * header) and busy (bit 63) never reads
 * set: the operation completes at once, so no launch is ever seen in flight.
*/
#define LB_VECTOR_ADDR         (0x1fffffULL << 3)
#define LB_VECTOR_WRITE_ID     (0xffULL << 32)
#define LB_VECTOR_REQUEST_ID   (0x7ffULL << 40)
/* The fields a reply echoes from the launch, in their shared positions. */
#define LB_VECTOR_ECHO         (LB_VECTOR_ADDR | LB_VECTOR_WRITE_ID | \
                                LB_VECTOR_REQUEST_ID)
#define LB_VECTOR_PARAMS_MASK  (1ULL | LB_VECTOR_ECHO)
/* Parameters bit 0: the stock PROM launches reads with 0, writes with 1. */
#define LB_VECTOR_TYPE_WRITE   1ULL
/* Status type-or-error 2:0: 4 is an address error. Then overrun and valid. */
#define LB_VECTOR_ADDR_ERROR   4ULL
#define LB_VECTOR_OVERRUN      (1ULL << 62)
#define LB_VECTOR_VALID        (1ULL << 63)
#define LB_ERROR_MASK 0x7ffULL
/* The revision word's two writable fields; 3 is an invalid region size. */
#define LB_REVISION_NODE_SHIFT   32
#define LB_REVISION_NODE_WIDTH   8
#define LB_REVISION_REGION_SHIFT 46
#define LB_REVISION_REGION_WIDTH 2
#define LB_REGION_SIZE_INVALID   3
#define LB_NODES_ABSENT_MASK 0x80ff80ff80ff80ffULL
#define LB_RTC_LOCAL_MASK (1ULL | (0x3ffULL << 4) | (0x3ffULL << 16) | \
                           (1ULL << 28) | (1ULL << 32))
#define LB_RTC_FILTER_MASK (0x1fULL | (0xfffULL << 8) | (1ULL << 20) | \
                            (0x3ffULL << 24) | (0x3ffULL << 36) | \
                            (0x3ffULL << 48))
#define LB_ERROR_HEADER_1_MASK (0x7fULL | (0x7ffULL << 8) | \
                                (0x7ffULL << 20) | (0xfULL << 32) | \
                                (1ULL << 40) | (1ULL << 44))
#define LB_ERROR_HEADER_2_MASK ((1ULL << 38) - 1)

/*
 * MicroLAN control. Bit 0 is the level sampled off the wire and bit 1 is
 * completion; then the sample offset, the
 * low-pulse duration and two phase dividers, the upper of which reaches bit
 * 33. The register is therefore not a
 * 32-bit one, and the model already takes only aligned eight-byte accesses.
 * This also stops QEMU splitting one
 * guest write into two wire slots. Which transfer sizes the hardware accepts
 * is not established: the single wide
 * access is a choice, not a finding about the silicon.
*/
#define LB_MICROLAN_RXDATA       (1ULL << 0)
#define LB_MICROLAN_DONE         (1ULL << 1)
#define LB_MICROLAN_SAMPLE_SHIFT 2
#define LB_MICROLAN_SAMPLE       (0xffULL << LB_MICROLAN_SAMPLE_SHIFT)
#define LB_MICROLAN_PULSE_SHIFT  10
#define LB_MICROLAN_PULSE        (0x3ffULL << LB_MICROLAN_PULSE_SHIFT)
#define LB_MICROLAN_PHASE_0      (0x7fULL << 20)
#define LB_MICROLAN_PHASE_1      (0x7fULL << 27)

/* The two result bits are the device's to publish; 63:34 are reserved. */
#define LB_MICROLAN_WRITABLE (LB_MICROLAN_SAMPLE | LB_MICROLAN_PULSE | \
                              LB_MICROLAN_PHASE_0 | LB_MICROLAN_PHASE_1)

/*
 * Slot duration, chosen not derived. No public source turns this register's
 * pulse and divider encodings into time
 * or gives a completion latency, so the Hub master's one microsecond is
 * reused rather than a second arbitrary
 * number invented. What the delay has to be is positive, so that a slot in
 * flight is a state a register test can
 * see.
*/
#define LB_MICROLAN_SLOT_NS 1000

static uint64_t lb_identity(SGIBedrockLBState *s)
{
    return ((uint64_t)s->manufacturer << 1) |
           ((uint64_t)s->part_number << 12) |
           ((uint64_t)s->revision << 28) |
           ((uint64_t)s->programmed_node << LB_REVISION_NODE_SHIFT) |
           ((uint64_t)s->programmed_region << LB_REVISION_REGION_SHIFT);
}

uint8_t sgi_bedrock_lb_node_number(SGIBedrockLBState *s)
{
    return s->programmed_node;
}

void sgi_bedrock_lb_add_node_notifier(SGIBedrockLBState *s,
                                      Notifier *notifier)
{
    notifier_list_add(&s->node_notifiers, notifier);
}

static void lb_node_changed(SGIBedrockLBState *s)
{
    notifier_list_notify(&s->node_notifiers, s);
}

/*
 * The stock PROM assigns the node number by storing the whole revision word:
 * zero at hub initialisation, then the
 * word it read with bits 39:32 and 47:46 replaced. It addresses itself
 * afterwards by the number it reads back, so
 * the two fields latch; manufacturer, part and revision stay the device's
 * identity, which the early store of zero
 * must not clear.
*/
static void lb_revision_write(SGIBedrockLBState *s, uint64_t value)
{
    unsigned region = extract64(value, LB_REVISION_REGION_SHIFT,
                                LB_REVISION_REGION_WIDTH);
    uint8_t before = s->programmed_node;

    if (region == LB_REGION_SIZE_INVALID) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-lb: revision write of "
                      "0x%" PRIx64 " names the invalid region size 3; "
                      "ignored\n", value);
        return;
    }
    s->programmed_node = extract64(value, LB_REVISION_NODE_SHIFT,
                                   LB_REVISION_NODE_WIDTH);
    s->programmed_region = region;
    if (s->programmed_node != before) {
        lb_node_changed(s);
    }
}

MemTxResult sgi_bedrock_lb_hub_access(SGIBedrockLBState *s, hwaddr addr,
                                      uint64_t *value, bool write)
{
    uint8_t buffer[8];
    MemTxResult result;

    if (!s->hub_as_initialized) {
        return MEMTX_DECODE_ERROR;
    }
    if (write) {
        stq_be_p(buffer, *value);
        return address_space_write(&s->hub_as, addr, MEMTXATTRS_UNSPECIFIED,
                                   buffer, sizeof(buffer));
    }
    result = address_space_read(&s->hub_as, addr, MEMTXATTRS_UNSPECIFIED,
                                buffer, sizeof(buffer));
    if (result == MEMTX_OK) {
        *value = ldq_be_p(buffer);
    }
    return result;
}

static void lb_update_irq(SGIBedrockLBState *s)
{
    qemu_set_irq(s->error, s->errors != 0);
}

/* The slot's deadline: publish completion and what the wire sampled. */
static void lb_microlan_slot_done(void *opaque)
{
    SGIBedrockLBState *s = opaque;

    s->microlan |= LB_MICROLAN_DONE |
                   (s->microlan_level ? LB_MICROLAN_RXDATA : 0);
}

static void lb_microlan_write(SGIBedrockLBState *s, uint64_t value)
{
    if (timer_pending(s->microlan_timer)) {
        /*
         * Unpublished: no source says whether hardware restarts the slot,
         * queues the write or drops it, and picking one
         * silently would settle a transaction semantics this model has no
         * evidence for.
        */
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: MicroLAN write of 0x%"
                      PRIx64 " while a slot is pending is not modelled; no "
                      "second slot is started and the write is discarded\n",
                      value);
        return;
    }

    /*
     * Starting a slot clears completion; the deadline below publishes it. The
     * bus answers inside this write, but
     * reporting it here would let a guest read a finished slot it never
     * waited for. Start, completion and presence
     * handling are the model's choices, not Bedrock's contract.
    */
    s->microlan = value & LB_MICROLAN_WRITABLE;
    s->microlan_level = sgi_onewire_slot(
        s->microlan_bus,
        (value & LB_MICROLAN_PULSE) >> LB_MICROLAN_PULSE_SHIFT,
        (value & LB_MICROLAN_SAMPLE) >> LB_MICROLAN_SAMPLE_SHIFT);
    timer_mod(s->microlan_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LB_MICROLAN_SLOT_NS);
}

/*
 * A vector launch. The engine reads or writes a hub register at the end of an
 * explicit route, one port number per
 * hop from the least significant nybble. Route 0 is the device at the other
 * end of this node's cable; the owning
 * Bedrock sends the request there and the peer performs it on its own
 * registers. What hardware answers with no
 * link, or for a nonzero route on a direct pair, is unknown. Such a launch
 * latches no reply, so the status stays
 * not valid and the firmware's own poll times out, and the launch is logged.
 * The return route and source are not
 * established either and read 0.
*/
static void lb_vector_launch(SGIBedrockLBState *s)
{
    uint64_t reply = s->vector_params & LB_VECTOR_ECHO;
    SGINetPacket packet = {
        .op = (s->vector_params & LB_VECTOR_TYPE_WRITE) ?
              SGI_NET_PACKET_WRITE : SGI_NET_PACKET_READ,
        .addr = s->vector_params & LB_VECTOR_ADDR,
        .data = s->vector_data,
        .vector = s->vector_route,
    };
    SGINetPacketResult result = SGI_NET_PACKET_NONE;

    /* Software clears the status before launching; a launch over an unread
     * reply is an overrun, not a reload. */
    if (s->vector_status & LB_VECTOR_VALID) {
        s->vector_status |= LB_VECTOR_OVERRUN;
        return;
    }
    if (s->bedrock) {
        result = sgi_bedrock_vector_send(s->bedrock, &packet);
    }
    trace_sgi_bedrock_lb_vector_launch(s->vector_route, s->vector_params,
                                       result, packet.value);
    switch (result) {
    case SGI_NET_PACKET_NONE:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: vector %s of 0x%" HWADDR_PRIx
                      " on route 0x%" PRIx64 " reached no device; no reply "
                      "is latched\n",
                      packet.op == SGI_NET_PACKET_WRITE ? "write" : "read",
                      packet.addr, s->vector_route);
        return;
    case SGI_NET_PACKET_ERROR:
        s->vector_return = 0;
        s->vector_status = reply | LB_VECTOR_VALID | LB_VECTOR_ADDR_ERROR;
        return;
    case SGI_NET_PACKET_OK:
        if (packet.op == SGI_NET_PACKET_READ) {
            s->vector_read = packet.value;
        }
        s->vector_return = 0;
        /*
         * Bits 2:0 carry the request type on success: the stock PROM's write
         * primitive accepts only 1 there, its read
         * primitive only 0.
        */
        s->vector_status = reply | LB_VECTOR_VALID |
                           (s->vector_params & LB_VECTOR_TYPE_WRITE);
        return;
    }
}

static uint64_t lb_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockLBState *s = opaque;

    if (offset >= LB_SCRATCH_BASE && offset < LB_SCRATCH_BASE + 5 * 8) {
        unsigned index = (offset - LB_SCRATCH_BASE) / 8;

        /* An outside reader sees the node's withheld scratch-1 posts. */
        if (index == 1 && s->bedrock) {
            sgi_bedrock_scratch1_read(s->bedrock);
        }
        return s->scratch[index];
    }
    if (offset >= LB_SCRATCH_RS_BASE && offset < LB_SCRATCH_RS_BASE + 2 * 8) {
        unsigned index = 3 + (offset - LB_SCRATCH_RS_BASE) / 8;
        uint64_t old = s->scratch[index];

        s->scratch[index] = 1;
        trace_sgi_bedrock_lb_scratch_twin_read(
            current_cpu ? current_cpu->cpu_index : -1, index, old);
        return old;
    }

    switch (offset) {
    case LB_REVISION:
        return lb_identity(s);
    case LB_CPU_PERMISSION:
        return s->cpu_permission;
    case LB_IO_PERMISSION:
        return s->io_permission;
    case LB_REGION_PRESENT:
        return s->region_present;
    case LB_NODES_ABSENT:
        return s->nodes_absent;
    case LB_MICROLAN:
        /* A read is not a slot: polling for completion drives nothing. */
        return s->microlan;
    case LB_ERROR_BITS:
        return s->errors;
    case LB_ERROR_HEADER_1:
        return s->error_header[0];
    case LB_ERROR_HEADER_2:
        return s->error_header[1];
    case LB_ERROR_DATA:
        return s->error_data;
    case LB_DEBUG_SELECT:
        return s->debug_select;
    case LB_RTC_LOCAL:
        return s->rtc_local;
    case LB_RTC_FILTER:
        return s->rtc_filter;
    case LB_VECTOR_PARAMS:
        return s->vector_params;
    case LB_VECTOR_ROUTE:
        return s->vector_route;
    case LB_VECTOR_DATA:
        return s->vector_data;
    case LB_VECTOR_STATUS:
        return s->vector_status;
    case LB_VECTOR_RETURN:
        return s->vector_return;
    case LB_VECTOR_READ:
        return s->vector_read;
    case LB_VECTOR_STATUS_CLEAR: {
        /*
         * The stock PROM reads this before every launch and never writes it,
         * then fails the operation if status is still
         * valid: the read clears.
        */
        uint64_t status = s->vector_status;

        s->vector_status = 0;
        return status;
    }
    case LB_CPU_OVERRIDE:
    case LB_SOFT_RESET:
    case LB_ERROR_CLEAR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-lb: read from write-only register 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    case LB_DEBUG_PINS:
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void lb_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockLBState *s = opaque;

    if (offset >= LB_SCRATCH_BASE && offset < LB_SCRATCH_BASE + 5 * 8) {
        unsigned index = (offset - LB_SCRATCH_BASE) / 8;

        /*
         * Scratch 1 is the stock PROM's inter-processor rendezvous mailbox;
         * the owning node runs the reset-barrier park there. A standalone
         * device under a register-contract qtest has no owning node
         * (s->bedrock is only set by sgi-bedrock.c's bedrock_realize()) and
         * stores the value directly, as before.
         */
        /* The value as issued: before the park below, and scratch 3's mask. */
        trace_sgi_bedrock_lb_scratch_write(
            current_cpu ? current_cpu->cpu_index : -1, index, value);
        if (index == 1 && s->bedrock) {
            sgi_bedrock_hold_scratch1_write(s->bedrock, value);
            return;
        }
        s->scratch[index] = index < 3 ? value : value & 1;
        /*
         * Clearing scratch 3 releases the node's hub lock, which the stock
         * PROM publishes presence under (sgi_bedrock_scratch3_released).
         * A standalone device has no owning node.
         */
        if (index == 3 && s->scratch[3] == 0 && s->bedrock) {
            sgi_bedrock_scratch3_released(s->bedrock);
        }
        return;
    }
    if (offset >= LB_SCRATCH_WZ_BASE && offset < LB_SCRATCH_WZ_BASE + 3 * 8) {
        unsigned index = (offset - LB_SCRATCH_WZ_BASE) / 8;

        if (s->scratch[index] == 0) {
            s->scratch[index] = value;
        }
        return;
    }

    switch (offset) {
    case LB_CPU_PERMISSION:
        s->cpu_permission = value;
        if (value != UINT64_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: CPU permission "
                          "enforcement requires requester context\n");
        }
        return;
    case LB_CPU_OVERRIDE:
        if (value == LB_PASSWORD) {
            s->cpu_permission |= 1ULL << s->request_region;
        }
        return;
    case LB_IO_PERMISSION:
        s->io_permission = value;
        if (value != UINT64_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: I/O permission "
                          "enforcement requires requester context\n");
        }
        return;
    case LB_SOFT_RESET:
        if (value & 1) {
            device_cold_reset(DEVICE(s));
            qemu_irq_pulse(s->soft_reset);
        }
        return;
    case LB_REGION_PRESENT:
        s->region_present = value;
        return;
    case LB_NODES_ABSENT:
        s->nodes_absent = value & LB_NODES_ABSENT_MASK;
        return;
    case LB_MICROLAN:
        lb_microlan_write(s, value);
        return;
    case LB_ERROR_BITS:
        s->errors |= value & LB_ERROR_MASK;
        lb_update_irq(s);
        return;
    case LB_ERROR_CLEAR:
        s->errors &= ~(value & LB_ERROR_MASK);
        lb_update_irq(s);
        return;
    case LB_ERROR_HEADER_1:
        s->error_header[0] = value & LB_ERROR_HEADER_1_MASK;
        return;
    case LB_ERROR_HEADER_2:
        s->error_header[1] = value & LB_ERROR_HEADER_2_MASK;
        return;
    case LB_ERROR_DATA:
        s->error_data = value;
        return;
    case LB_DEBUG_SELECT:
        s->debug_select = value & 0x3f;
        return;
    case LB_RTC_LOCAL:
        s->rtc_local = value & LB_RTC_LOCAL_MASK;
        return;
    case LB_RTC_FILTER:
        s->rtc_filter = value & LB_RTC_FILTER_MASK;
        return;
    case LB_VECTOR_PARAMS:
        s->vector_params = value & LB_VECTOR_PARAMS_MASK;
        lb_vector_launch(s);
        return;
    case LB_VECTOR_ROUTE:
        s->vector_route = value;
        return;
    case LB_VECTOR_DATA:
        s->vector_data = value;
        return;
    case LB_VECTOR_STATUS_CLEAR:
        /* No consumer writes it; clearing on a write too is the model's. */
        s->vector_status = 0;
        return;
    case LB_REVISION:
        lb_revision_write(s, value);
        return;
    case LB_VECTOR_STATUS:
        /*
         * The stock PROM zeroes status at hub initialisation. What any write
         * does on hardware is unknown; zero clears, and
         * nothing else may fabricate a reply.
        */
        if (value) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: nonzero write 0x%"
                          PRIx64 " to the vector status is ignored\n", value);
            return;
        }
        s->vector_status = 0;
        return;
    case LB_VECTOR_RETURN:
        s->vector_return = value;
        return;
    case LB_VECTOR_READ:
        /* The PROM zeroes it, then seeds a sentinel before each launch. */
        s->vector_read = value;
        return;
    case LB_DEBUG_PINS:
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-lb: unsupported write at 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", offset, value);
        return;
    }
}

static const MemoryRegionOps lb_ops = {
    .read = lb_read,
    .write = lb_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void lb_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(obj);

    s->cpu_permission = UINT64_MAX;
    s->io_permission = UINT64_MAX;
    /*
     * The node number's hardware reset value is unknown; the realized
     * properties stand in for it, and the firmware
     * rewrites both fields.
    */
    s->programmed_node = s->node_number;
    s->programmed_region = s->region_size;
    s->region_present = 0;
    s->nodes_absent = 0;
    /*
     * Zero is the model's value: no public source states this register's
     * power-on contents, so nothing here is a
     * hardware reset state. A slot armed before the reset must not complete
     * after it, and the endpoint's own protocol
     * phase is reset by the endpoint, as a child of this bus.
    */
    s->microlan = 0;
    timer_del(s->microlan_timer);
    s->microlan_level = true;
    s->error_header[0] = 0;
    s->error_header[1] = 0;
    s->error_data = 0;
    s->debug_select = 0;
    /*
     * Zero is the model's value; neither register has a published power-on
     * value. The local block's maximum-count field feeds the PROM's own
     * real-time conversion, so a wrong reset value here does not
     * misbehave loudly; it just makes every duration the firmware derives
     * from the counter come out short.
    */
    s->rtc_local = 0;
    s->rtc_filter = 0;
    s->scratch[0] = 0;
    s->scratch[3] = 0;
    s->scratch[4] = 0;
    s->vector_params = 0;
    s->vector_route = 0;
    s->vector_data = 0;
    s->vector_status = 0;
    s->vector_return = 0;
    s->vector_read = 0;
}

static void lb_reset_hold(Object *obj, ResetType type)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(obj);

    lb_update_irq(s);
    /* The enter phase restored the realized number; decode follows it. */
    lb_node_changed(s);
}

static int lb_post_load(void *opaque, int version_id)
{
    lb_update_irq(opaque);
    lb_node_changed(opaque);
    return 0;
}

static const VMStateDescription vmstate_lb = {
    .name = TYPE_SGI_BEDROCK_LB,
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = lb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(cpu_permission, SGIBedrockLBState),
        VMSTATE_UINT64(io_permission, SGIBedrockLBState),
        VMSTATE_UINT64(region_present, SGIBedrockLBState),
        VMSTATE_UINT64(nodes_absent, SGIBedrockLBState),
        /* The slot in flight: its register, its sample and its deadline. */
        VMSTATE_UINT64(microlan, SGIBedrockLBState),
        VMSTATE_BOOL(microlan_level, SGIBedrockLBState),
        VMSTATE_TIMER_PTR(microlan_timer, SGIBedrockLBState),
        VMSTATE_UINT64(errors, SGIBedrockLBState),
        VMSTATE_UINT64_ARRAY(error_header, SGIBedrockLBState, 2),
        VMSTATE_UINT64(error_data, SGIBedrockLBState),
        VMSTATE_UINT64(debug_select, SGIBedrockLBState),
        VMSTATE_UINT64(rtc_local, SGIBedrockLBState),
        VMSTATE_UINT64(rtc_filter, SGIBedrockLBState),
        VMSTATE_UINT64_ARRAY(scratch, SGIBedrockLBState, 5),
        VMSTATE_UINT64(vector_params, SGIBedrockLBState),
        VMSTATE_UINT64(vector_route, SGIBedrockLBState),
        VMSTATE_UINT64(vector_data, SGIBedrockLBState),
        VMSTATE_UINT64(vector_status, SGIBedrockLBState),
        VMSTATE_UINT64(vector_return, SGIBedrockLBState),
        VMSTATE_UINT64(vector_read, SGIBedrockLBState),
        VMSTATE_UINT8(programmed_node, SGIBedrockLBState),
        VMSTATE_UINT8(programmed_region, SGIBedrockLBState),
        VMSTATE_END_OF_LIST()
    },
};

static void lb_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(dev);

    if (s->manufacturer > 0x7ff || s->part_number > 0xffff ||
        s->revision > 0xf || s->node_number > 0xff ||
        s->region_size > 2 || s->request_region > 63) {
        error_setg(errp, "Bedrock local-block identity or region property "
                   "is outside its documented field");
        return;
    }

    s->microlan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     lb_microlan_slot_done, s);
    /*
     * The MicroLAN wire, with nothing on it. Which identity part IP45 carries
     * is not established: its family, its
     * commands and its contents are all open. Adding one here would
     * publish an invented endpoint. The bus exists
     * so a cleared endpoint can be added later without the controller's
     * contract moving; until then a slot reports the
     * bare wire and a reset finds no presence.
    */
    s->microlan_bus = onewire_create_bus(dev, "microlan");

    if (s->hub_registers) {
        address_space_init(&s->hub_as, s->hub_registers,
                           "sgi-bedrock-lb.hub-registers");
        s->hub_as_initialized = true;
    }
}

static void lb_unrealize(DeviceState *dev)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(dev);

    if (s->microlan_timer) {
        timer_del(s->microlan_timer);
    }
    if (s->hub_as_initialized) {
        address_space_destroy(&s->hub_as);
        s->hub_as_initialized = false;
    }
}

static void lb_finalize(Object *obj)
{
    lb_unrealize(DEVICE(obj));
}

static const Property lb_properties[] = {
    DEFINE_PROP_UINT32("manufacturer", SGIBedrockLBState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("part-number", SGIBedrockLBState, part_number, 0x3012),
    DEFINE_PROP_UINT32("revision", SGIBedrockLBState, revision, 2),
    DEFINE_PROP_UINT32("node-number", SGIBedrockLBState, node_number, 0),
    DEFINE_PROP_UINT32("region-size", SGIBedrockLBState, region_size, 0),
    DEFINE_PROP_UINT32("request-region", SGIBedrockLBState, request_region, 0),
    DEFINE_PROP_LINK("hub-registers", SGIBedrockLBState, hub_registers,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void lb_init(Object *obj)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(obj);

    notifier_list_init(&s->node_notifiers);
    memory_region_init_io(&s->mmio, obj, &lb_ops, s, TYPE_SGI_BEDROCK_LB,
                          SGI_BEDROCK_LB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->soft_reset, "soft-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->error, "error", 1);
}

static void lb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock local block";
    dc->realize = lb_realize;
    dc->unrealize = lb_unrealize;
    dc->vmsd = &vmstate_lb;
    dc->user_creatable = true;
    device_class_set_props(dc, lb_properties);
    rc->phases.enter = lb_reset_enter;
    rc->phases.hold = lb_reset_hold;
}

static const TypeInfo lb_info = {
    .name = TYPE_SGI_BEDROCK_LB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockLBState),
    .instance_init = lb_init,
    .instance_finalize = lb_finalize,
    .class_init = lb_class_init,
};

static void lb_register_types(void)
{
    type_register_static(&lb_info);
}

type_init(lb_register_types)
