/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock local block: bounded register storage and atomic scratch aliases. */
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
 * and status, so the reply echoes them in place. The type width is open (one
 * bit in the union, two in the companion header) and busy (bit 63) never reads
 * set: the operation completes at once, so no launch is ever seen in flight.
 */
#define LB_VECTOR_ADDR         (0x1fffffULL << 3)
#define LB_VECTOR_WRITE_ID     (0xffULL << 32)
#define LB_VECTOR_REQUEST_ID   (0x7ffULL << 40)
/* The fields a reply echoes from the launch, in their shared positions. */
#define LB_VECTOR_ECHO         (LB_VECTOR_ADDR | LB_VECTOR_WRITE_ID | \
                                LB_VECTOR_REQUEST_ID)
#define LB_VECTOR_PARAMS_MASK  (1ULL | LB_VECTOR_ECHO)
/* Status type-or-error 2:0: 4 is an address error. Then overrun and valid. */
#define LB_VECTOR_ADDR_ERROR   4ULL
#define LB_VECTOR_OVERRUN      (1ULL << 62)
#define LB_VECTOR_VALID        (1ULL << 63)
#define LB_ERROR_MASK 0x7ffULL
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
 * completion; then the sample offset, the low-pulse duration and two phase
 * dividers, the upper of which reaches bit 33. The register is therefore not
 * a 32-bit one, and the model already takes only aligned eight-byte accesses
 * -- which also stops QEMU splitting one guest write into two wire slots.
 * Which transfer sizes the hardware accepts is not established: the single
 * wide access is a choice, not a finding about the silicon.
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
 * pulse and divider encodings into time or gives a completion latency, so
 * the Hub master's one microsecond is reused rather than a second arbitrary
 * number invented. What the delay has to be is positive, so that a slot in
 * flight is a state a register test can see.
 */
#define LB_MICROLAN_SLOT_NS 1000

static uint64_t lb_identity(SGIBedrockLBState *s)
{
    return ((uint64_t)s->manufacturer << 1) |
           ((uint64_t)s->part_number << 12) |
           ((uint64_t)s->revision << 28) |
           ((uint64_t)s->node_number << 32) |
           ((uint64_t)s->region_size << 46);
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
         * queues the write or drops it, and picking one silently would
         * settle a transaction semantics this model has no evidence for.
         */
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: MicroLAN write of 0x%"
                      PRIx64 " while a slot is pending is not modelled; no "
                      "second slot is started and the write is discarded\n",
                      value);
        return;
    }

    /*
     * Starting a slot clears completion; the deadline below publishes it.
     * The bus answers inside this write, but reporting it here would let a
     * guest read a finished slot it never waited for. Start, completion and
     * presence handling are the model's choices, not Bedrock's contract.
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
 * A vector launch. The engine reads or writes a register along an explicit
 * port-by-port route (datasheet "vector operations"); the firmware drives it
 * during NUMAlink discovery to read each hub before building KLCONFIG, and the
 * block replies to the parameters write at once.
 *
 * This models the routed READ on the local route. A one-chassis node has no
 * NUMAlink peer, so every route resolves to this node's own hub, read from the
 * linked hub register window -- the block aliased into the local register
 * space. Kept out of the model until a boot trace or the header settles them:
 * the self-vs-remote route encoding (not decoded; there is no remote peer to
 * route to), the type field's read/write polarity (discovery reads only, so a
 * launch completes as a read), and write-transaction behavior.
 */
static void lb_vector_launch(SGIBedrockLBState *s)
{
    uint64_t reply = s->vector_params & LB_VECTOR_ECHO;
    hwaddr addr = s->vector_params & LB_VECTOR_ADDR;
    uint8_t buffer[8];

    if (!s->hub_as_initialized) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: vector launch with no hub "
                      "register window linked\n");
        return;
    }
    /*
     * Software clears the status before launching; a launch over an unread
     * reply is an overrun, not a reload.
     */
    if (s->vector_status & LB_VECTOR_VALID) {
        s->vector_status |= LB_VECTOR_OVERRUN;
        return;
    }
    if (address_space_read(&s->hub_as, addr, MEMTXATTRS_UNSPECIFIED,
                           buffer, sizeof(buffer)) != MEMTX_OK) {
        s->vector_status = reply | LB_VECTOR_VALID | LB_VECTOR_ADDR_ERROR;
        return;
    }
    s->vector_read = ldq_be_p(buffer);
    /* The self-route return path is not pinned; report none. */
    s->vector_return = 0;
    s->vector_status = reply | LB_VECTOR_VALID;
}

static uint64_t lb_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockLBState *s = opaque;

    if (offset >= LB_SCRATCH_BASE && offset < LB_SCRATCH_BASE + 5 * 8) {
        return s->scratch[(offset - LB_SCRATCH_BASE) / 8];
    }
    if (offset >= LB_SCRATCH_RS_BASE && offset < LB_SCRATCH_RS_BASE + 2 * 8) {
        unsigned index = 3 + (offset - LB_SCRATCH_RS_BASE) / 8;
        uint64_t old = s->scratch[index];

        s->scratch[index] = 1;
        /*
         * Scratch 3's twin is the node's hub lock; the owning node arbitrates
         * the second multi-threaded-TCG rendezvous there
         * (sgi_bedrock_scratch3_acquire, docs/known-issues.md). The value is
         * already produced; the hold only decides when the reader runs next.
         * A standalone device under a register-contract qtest has no owning
         * node (s->bedrock is only set by bedrock_realize()) and never holds.
         */
        trace_sgi_bedrock_lb_scratch_twin_read(
            current_cpu ? current_cpu->cpu_index : -1, index, old);
        if (index == 3 && s->bedrock) {
            sgi_bedrock_scratch3_acquire(s->bedrock, old);
        }
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
    case LB_VECTOR_STATUS_CLEAR:
        /* Whether a read or a write clears is open; here a write clears. */
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: read of the vector "
                      "status-clear alias; read-and-clear is not modelled\n");
        return 0;
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
         * Scratch 1 is the stock PROM's inter-processor rendezvous mailbox
         * (docs/known-issues.md, docs/datasheets/bedrock-local-block.md);
         * the owning node arbitrates a multi-threaded-TCG hold there. A
         * standalone device under a register-contract qtest has no owning
         * node (s->bedrock is only set by sgi-bedrock.c's bedrock_realize())
         * and stores the value directly, as before.
         */
        trace_sgi_bedrock_lb_scratch_write(
            current_cpu ? current_cpu->cpu_index : -1, index, value);
        if (index == 1 && s->bedrock) {
            sgi_bedrock_hold_scratch1_write(s->bedrock, value);
            return;
        }
        s->scratch[index] = index < 3 ? value : value & 1;
        /*
         * Clearing scratch 3 releases the node's hub lock; wake any peer this
         * node's own processor parked on it (sgi_bedrock_scratch3_released,
         * docs/known-issues.md). A standalone device has no owning node.
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
        /*
         * The model's chosen semantics while the header is silent on read
         * versus write: a write clears the return status before a launch.
         */
        s->vector_status = 0;
        return;
    case LB_REVISION:
    case LB_VECTOR_STATUS:
    case LB_VECTOR_RETURN:
    case LB_VECTOR_READ:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-lb: write to read-only register 0x%"
                      HWADDR_PRIx "\n", offset);
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
    s->region_present = 0;
    s->nodes_absent = 0;
    /*
     * Zero is the model's value: no public source states this register's
     * power-on contents, so nothing here is a hardware reset state. A slot
     * armed before the reset must not complete after it, and the endpoint's
     * own protocol phase is reset by the endpoint, as a child of this bus.
     */
    s->microlan = 0;
    timer_del(s->microlan_timer);
    s->microlan_level = true;
    s->error_header[0] = 0;
    s->error_header[1] = 0;
    s->error_data = 0;
    s->debug_select = 0;
    /*
     * Zero is the model's value here too: no source we may read states
     * either register's power-on contents. The local block's maximum-count
     * field feeds the PROM's own real-time conversion, so a wrong reset
     * value here does not misbehave loudly -- it just makes every duration
     * the firmware derives from the counter come out short. See the local
     * block datasheet for what is and is not established about these two
     * registers.
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
    lb_update_irq(SGI_BEDROCK_LB(obj));
}

static int lb_post_load(void *opaque, int version_id)
{
    lb_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_lb = {
    .name = TYPE_SGI_BEDROCK_LB,
    .version_id = 3,
    .minimum_version_id = 3,
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
     * The MicroLAN wire, with nothing on it. Which identity part IP45
     * carries is not established -- its family, its commands and its
     * contents are all open -- so hanging one here would publish an
     * invented endpoint. The bus exists so a cleared endpoint can be added
     * later without the controller's contract moving; until then a slot
     * reports the bare wire and a reset finds no presence.
     */
    s->microlan_bus = onewire_create_bus(dev, "microlan");

    if (s->hub_registers) {
        address_space_init(&s->hub_as, s->hub_registers,
                           "sgi-bedrock-lb.hub-registers");
        s->hub_as_initialized = true;
    }
}

static void lb_finalize(Object *obj)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(obj);

    if (s->hub_as_initialized) {
        address_space_destroy(&s->hub_as);
    }
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

    memory_region_init_io(&s->mmio, obj, &lb_ops, s, TYPE_SGI_BEDROCK_LB,
                          SGI_BEDROCK_LB_SIZE);
    /*
     * A vector read dispatches into the hub register window, which aliases
     * this block, so a self-route read lands back on these registers while
     * the launching write is still in flight. The per-device re-entrancy
     * guard would block that nested access. The read is bounded -- one
     * register, no further dispatch -- so opting out cannot recurse.
     */
    s->mmio.disable_reentrancy_guard = true;
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
