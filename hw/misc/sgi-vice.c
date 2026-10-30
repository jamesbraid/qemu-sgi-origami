/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VICE host/local-memory and common descriptor DMA transport.
 * Contract: Origami docs/datasheets/o2-vice-host-dma.md, SHA-256
 * 2f7d2767e0e8bf53e96ee1b60224e3f6dd6569e5e28136af09c9469e4b615615.
 * Scalar execution is in sgi-vice-msp.c; FIFO/image operations remain held.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-vice-msp.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "system/runstate.h"
#include "system/ram-discard-manager.h"

#define TYPE_SGI_VICE "sgi-vice"
OBJECT_DECLARE_SIMPLE_TYPE(SGIViceState, SGI_VICE)

#define CTRL_GO       0x001
#define CTRL_IE       0x002
#define CTRL_STOP     0x004
#define CTRL_RESET    0x008
#define CTRL_POINTER  0x0f0
#define CTRL_BYPASS   0x100
#define CTRL_KNOWN    0x1ff
#define DESC_READ     0x2000
#define DESC_FILL     0x1000
#define DESC_SKIP     0x4000
#define DESC_HALT     0x8000
#define DESC_HELD     0x0f8f
#define STATUS_DONE   1
#define STATUS_ERROR  2
#define STATUS_ACTIVE 4
#define STATUS_READ   8

/* These are physical stores; the host and future cores share one local bus. */
static const struct {
    unsigned base;
    unsigned size;
    unsigned write_unit;
    const char *name;
} vice_memories[] = {
    { 0x2000, 0x1000, 4, "vice-msp-instructions" },
    { 0x4000, 0x0800, 2, "vice-bsp-instructions" },
    { 0x5000, 0x1400, 4, "vice-tables" },
    { 0x8000, 0x0800, 1, "vice-data-a" },
    { 0x8800, 0x0800, 1, "vice-data-b" },
    { 0x9000, 0x0800, 1, "vice-data-c" },
};

typedef struct ViceChannel {
    uint16_t control;
    uint16_t status;
    uint16_t fill;
    uint16_t descriptors[4][8];
    uint16_t latched[8];
    uint32_t system_pointer;
    uint32_t local_pointer;
    uint32_t cursor;
    uint16_t remaining;
    uint8_t descriptor;
    uint8_t length;
    uint8_t filled;
    uint8_t committed;
    uint32_t unit_local;
    uint8_t staging[8];
    bool loaded;
    bool staged;
    bool blocked;
    bool model_blocked;
    bool reset_held;
    int64_t reset_start;
} ViceChannel;

struct SGIViceState {
    SysBusDevice parent_obj;
    MemoryRegion aperture;
    MemoryRegion local_bus;
    MemoryRegion ram[G_N_ELEMENTS(vice_memories)];
    MemoryRegion *dma_memory;
    AddressSpace local_as;
    AddressSpace dma_as;
    qemu_irq irq;
    Clock *device_clock;
    Clock *msp_clock;
    QEMUTimer *timer;
    SGIViceMsp msp;
    bool dma_pending;
    bool msp_servicing;
    uint32_t device_clock_hz;
    uint32_t msp_clock_hz;
    uint8_t part_id;
    bool stable_reference;

    uint16_t config;
    uint16_t interrupt_status;
    uint16_t interrupt_enable;
    uint32_t bank_grants;
    uint32_t translations[64];
    ViceChannel channel[2];
    int64_t counter_epoch;
    int64_t next_ns;
    uint32_t next_fraction;
    uint8_t next_channel;
    bool external_reset;
    bool initialized;
    bool unsupported;
    int64_t external_reset_start;
    int64_t saved_clock;
};

static void vice_reschedule(SGIViceState *s);
static MemTxResult vice_access(SGIViceState *s, hwaddr address,
                               uint64_t *value, unsigned size, bool write);

static MemTxResult vice_refuse(SGIViceState *s, const char *reason,
                               hwaddr address)
{
    qemu_log_mask(LOG_UNIMP, "sgi-vice: unsupported %s at 0x%" HWADDR_PRIx
                  "\n", reason, address);
    s->unsupported = true;
    timer_del(s->timer);
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
    return MEMTX_ERROR;
}

static void vice_irq(SGIViceState *s)
{
    qemu_set_irq(s->irq, !!(s->interrupt_status & s->interrupt_enable));
}

static int vice_memory(hwaddr address)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(vice_memories); i++) {
        if (address >= vice_memories[i].base &&
            address - vice_memories[i].base < vice_memories[i].size) {
            return i;
        }
    }
    return -1;
}

static bool vice_ram_hole(hwaddr address)
{
    return (address >= 0x3000 && address < 0x4000) ||
           (address >= 0x4800 && address < 0x5000) ||
           (address >= 0x6400 && address < 0x7000) ||
           (address >= 0x9800 && address < 0xe000) || address >= 0x10000;
}

static bool vice_channel_active(ViceChannel *c)
{
    return (c->status & STATUS_ACTIVE) && !c->model_blocked;
}

static bool vice_active(SGIViceState *s)
{
    return vice_channel_active(&s->channel[0]) ||
           vice_channel_active(&s->channel[1]);
}

/* Carry the sub-nanosecond clock phase rather than rounding each period. */
static void vice_schedule(SGIViceState *s, bool first)
{
    uint64_t period = clock_get(s->device_clock);
    uint64_t fraction;

    if (s->unsupported || s->external_reset || !vice_active(s)) {
        return;
    }
    if (first) {
        s->next_ns = s->msp_servicing ? s->msp.next_ns :
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->next_fraction = s->msp_servicing ? s->msp.next_fraction : 0;
    }
    fraction = (uint64_t)s->next_fraction + (uint32_t)period;
    s->next_ns += (period >> 32) + (fraction >> 32);
    s->next_fraction = fraction;
    s->dma_pending = true;
    vice_reschedule(s);
}

static bool vice_reset_ready(SGIViceState *s, int64_t assertion)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - assertion;

    return s->stable_reference && elapsed >= 0 &&
           ((unsigned __int128)elapsed << 32) >=
           (unsigned __int128)16 * clock_get(s->device_clock);
}

static void vice_channel_reset(ViceChannel *c)
{
    c->control = 0x18;
    c->status = 0x10;
    c->system_pointer = 0;
    c->local_pointer = 0;
    c->cursor = 0;
    c->remaining = 0;
    c->descriptor = 0;
    c->length = 0;
    c->filled = 0;
    c->committed = 0;
    c->unit_local = 0;
    c->loaded = false;
    c->staged = false;
    c->blocked = false;
    c->model_blocked = false;
    c->reset_held = true;
    memset(c->latched, 0, sizeof(c->latched));
    memset(c->staging, 0, sizeof(c->staging));
}

static void vice_clear(SGIViceState *s)
{
    timer_del(s->timer);
    s->dma_pending = false;
    sgi_vice_msp_reset(&s->msp);
    s->config = 0;
    s->interrupt_enable = 0;
    s->interrupt_status = 0;
    s->bank_grants = 0;
    s->counter_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->next_channel = 0;
    s->next_fraction = 0;
    s->next_ns = s->counter_epoch;
    s->unsupported = false;
    memset(s->translations, 0, sizeof(s->translations));
    memset(s->channel, 0, sizeof(s->channel));
    for (unsigned i = 0; i < 2; i++) {
        vice_channel_reset(&s->channel[i]);
        s->channel[i].reset_held = false;
        s->channel[i].control = 0x10;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(vice_memories); i++) {
        uint8_t zero[0x1400] = { 0 };

        address_space_write(&s->local_as, vice_memories[i].base,
                            MEMTXATTRS_UNSPECIFIED, zero,
                            vice_memories[i].size);
    }
    vice_irq(s);
}

/* The Resettable callback admits the stable initial boundary only once. */
static void vice_reset_enter(Object *obj, ResetType type)
{
    SGIViceState *s = SGI_VICE(obj);

    vice_clear(s);
    if (!s->initialized && s->stable_reference) {
        s->external_reset = false;
    } else {
        s->external_reset = true;
        s->external_reset_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->initialized = true;
}

static void vice_external_reset(void *opaque, int n, int level)
{
    SGIViceState *s = opaque;

    if (level && !s->external_reset) {
        vice_clear(s);
        s->external_reset = true;
        s->external_reset_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else if (!level && s->external_reset) {
        if (!vice_reset_ready(s, s->external_reset_start)) {
            vice_refuse(s, "short or unstable external reset release", 0);
            return;
        }
        s->external_reset = false;
        s->unsupported = false;
    }
}

static void vice_fault(SGIViceState *s, unsigned channel, unsigned code)
{
    ViceChannel *c = &s->channel[channel];

    c->status = (c->status & 0xf8) | STATUS_ERROR | (code << 8);
    c->blocked = true;
    c->staged = false;
    c->filled = 0;
    s->interrupt_status |= channel ? 1 << 8 : 1 << 1;
    vice_irq(s);
}

/* Called for every actual request, including the destination commit. */
static bool vice_system(SGIViceState *s, unsigned channel, uint64_t address,
                         bool write, hwaddr *physical, unsigned *length)
{
    ViceChannel *c = &s->channel[channel];
    hwaddr span = *length;
    MemoryRegionSection section;
    RamDiscardManager *discard;
    bool supported;

    if (address > UINT32_MAX) {
        vice_fault(s, channel, 7);
        return false;
    }
    if ((s->config & 2) && (c->control & CTRL_BYPASS)) {
        *physical = address;
        span = MIN(span, (uint64_t)UINT32_MAX + 1 - address);
    } else {
        unsigned index, offset;
        uint32_t entry;

        if (address >= 0x00800000 && address <= 0x00bfffff) {
            index = (address >> 16) & 63;
            offset = address & 65535;
            span = MIN(span, 65536 - offset);
        } else if (address >= 0x10800000 && address <= 0x10bfffff) {
            index = (((address >> 19) & 7) << 3) | ((address >> 9) & 7);
            offset = (((address >> 12) & 127) << 9) | (address & 511);
            span = MIN(span, 512 - (address & 511));
        } else {
            vice_fault(s, channel, 7);
            return false;
        }
        entry = s->translations[index];
        if (!(entry & 1) || (write && !(entry & 2))) {
            vice_fault(s, channel, entry & 1 ? 8 : 7);
            return false;
        }
        *physical = (entry & 0xffff0000) | offset;
    }
    c->system_pointer = *physical & ~3ULL;
    /*
     * The resolved section retains alias protection and populated boundaries.
     */
    section = memory_region_find(s->dma_as.root, *physical, span);
    span = MIN(span, int128_get64(section.size));
    supported = section.mr && span &&
                section.offset_within_address_space == *physical &&
                memory_region_is_ram(section.mr) &&
                !memory_region_is_ram_device(section.mr) &&
                !(write && section.readonly);
    if (supported) {
        discard = memory_region_get_ram_discard_manager(section.mr);
        supported = !discard || ram_discard_manager_is_populated(discard,
                                                                &section);
    }
    memory_region_unref(section.mr);
    if (!supported) {
        vice_refuse(s, "system target is not accessible populated RAM",
                    *physical);
        return false;
    }
    *length = MIN(*length, span);
    return true;
}

static bool vice_fetch_descriptor(SGIViceState *s, ViceChannel *c)
{
    uint16_t *d = c->latched;
    uint32_t count;
    int memory;

    memcpy(d, c->descriptors[c->descriptor], sizeof(c->latched));
    c->loaded = true;
    c->status = STATUS_ACTIVE | (0x10 << c->descriptor) | 0x300;
    if ((d[0] & (DESC_SKIP | DESC_HALT)) == (DESC_SKIP | DESC_HALT)) {
        vice_refuse(s, "skip combined with halt", 0x1000);
        return false;
    }
    if (d[0] & DESC_SKIP) {
        c->descriptor = (c->descriptor + 1) & 3;
        c->loaded = false;
        c->status = STATUS_ACTIVE | (0x10 << c->descriptor) | 0x300;
        return false;
    }
    if (d[0] & DESC_HELD) {
        vice_refuse(s, "descriptor mode", 0x1000);
        return false;
    }
    if (d[0] & 0x70) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-vice: ignored descriptor location bits\n");
    }
    count = (uint32_t)d[3] * d[5];
    if (!d[3] || !d[5] || count > UINT16_MAX ||
        (d[5] > 1 && (d[4] & 0x8000)) || (d[6] & 7) ||
        ((d[0] & DESC_FILL) && (!(d[0] & DESC_READ) || (d[3] & 1)))) {
        vice_refuse(s, "descriptor count, stride, alignment or fill", 0x1000);
        return false;
    }
    memory = vice_memory(d[6]);
    if (memory < 0) {
        vice_refuse(s, "local DMA endpoint", d[6]);
        return false;
    }
    if (memory < 3) {
        unsigned unit = memory == 0 ? 8 : vice_memories[memory].write_unit;
        uint32_t system = (uint32_t)d[1] << 16 | d[2];

        if ((d[3] % unit) || (memory == 0 && !(d[0] & DESC_FILL) &&
            ((system % unit) || (d[5] > 1 && (d[4] % unit))))) {
            vice_refuse(s, "instruction or table DMA alignment", d[6]);
            return false;
        }
    }
    c->remaining = count;
    c->cursor = 0;
    c->status |= d[0] & DESC_READ ? STATUS_READ : 0;
    return true;
}

static void vice_stop(ViceChannel *c)
{
    c->status = (c->status & 0xf8) | 0x100;
    c->blocked = true;
}

static void vice_phase(SGIViceState *s, unsigned channel)
{
    ViceChannel *c = &s->channel[channel];
    uint16_t *d = c->latched;
    unsigned row, column, length;
    uint64_t local, system;
    hwaddr physical;
    bool read, fill;
    int memory;
    MemTxResult result;

    if (!vice_channel_active(c) || s->unsupported) {
        return;
    }
    if (!c->length && (c->control & CTRL_STOP)) {
        vice_stop(c);
        return;
    }
    if (!c->loaded && !vice_fetch_descriptor(s, c)) {
        return;
    }
    row = c->cursor / d[3];
    column = c->cursor % d[3];
    system = ((uint32_t)d[1] << 16 | d[2]) + (uint64_t)row * d[4] + column;
    read = d[0] & DESC_READ;
    fill = d[0] & DESC_FILL;
    local = c->length ? c->unit_local : (uint64_t)d[6] + c->cursor;
    memory = vice_memory(local);
    if (memory < 0) {
        vice_refuse(s, "local DMA exceeds fitted storage", local);
        return;
    }
    if (memory == 0 && s->msp.running) {
        vice_refuse(s, "MSP runtime instruction DMA", local);
        return;
    }
    if (!c->length) {
        unsigned unit = memory == 0 ? 8 : memory < 3 ?
                        vice_memories[memory].write_unit : 8;

        c->length = MIN(unit, d[3] - column);
        c->length = MIN(c->length, vice_memories[memory].base +
                         vice_memories[memory].size - local);
        c->unit_local = local;
        c->filled = 0;
        c->committed = 0;
    }
    if (!c->staged) {
        length = c->length - c->filled;
        if (fill) {
            for (unsigned i = 0; i < length; i++) {
                c->staging[i] = c->fill >> (i & 1 ? 0 : 8);
            }
            c->filled = c->length;
        } else if (read) {
            if (!vice_system(s, channel, system + c->filled, false,
                              &physical, &length)) {
                return;
            }
            /* Byte RAM has no typed publication boundary to assemble. */
            if (memory >= 3) {
                c->length = length;
            }
            result = dma_memory_read(&s->dma_as, physical,
                                      c->staging + c->filled, length,
                                      MEMTXATTRS_UNSPECIFIED);
            if (result != MEMTX_OK) {
                vice_refuse(s, "upstream RAM read failure", physical);
                return;
            }
            c->filled += length;
        } else {
            result = address_space_read(&s->local_as, local,
                                         MEMTXATTRS_UNSPECIFIED,
                                         c->staging, length);
            if (result != MEMTX_OK) {
                vice_refuse(s, "local read failure", local);
                return;
            }
            c->local_pointer = local;
            c->filled = c->length;
        }
        c->staged = c->filled == c->length;
        c->status = (c->status & 0xff) | 0x600;
        return;
    }
    if (read) {
        /* Accepted source bytes retain their old content and grants. */
        length = c->length;
        if (memory == 2) {
            stl_be_p(c->staging, ldl_be_p(c->staging) & 0x3fffff);
        }
        result = address_space_write(&s->local_as, local,
                                      MEMTXATTRS_UNSPECIFIED,
                                      c->staging, length);
        c->local_pointer = local;
    } else {
        length = c->length - c->committed;
        if (!vice_system(s, channel, system, true, &physical, &length)) {
            return;
        }
        result = dma_memory_write(&s->dma_as, physical,
                                   c->staging + c->committed, length,
                                   MEMTXATTRS_UNSPECIFIED);
    }
    if (result != MEMTX_OK) {
        vice_refuse(s, "destination transaction failure", local);
        return;
    }
    c->cursor += length;
    c->remaining -= length;
    c->committed += length;
    if (c->committed == c->length) {
        c->staged = false;
        c->length = 0;
        c->filled = 0;
        c->committed = 0;
    }
    if (c->control & CTRL_STOP) {
        if (!c->length) {
            vice_stop(c);
        }
        return;
    }
    if (!c->remaining) {
        if (d[0] & DESC_HALT) {
            c->status = (c->status & 0xf8) | STATUS_DONE | 0x200;
            c->control &= ~CTRL_GO;
            c->loaded = false;
            if (c->control & CTRL_IE) {
                s->interrupt_status |= channel ? 1 << 7 : 1;
                vice_irq(s);
            }
        } else {
            c->descriptor = (c->descriptor + 1) & 3;
            c->loaded = false;
            c->status = STATUS_ACTIVE | (0x10 << c->descriptor) | 0x300;
        }
    }
}

static bool vice_dma_deferred(SGIViceState *s, ViceChannel *c)
{
    uint64_t local;
    int memory;

    if (!c->length && (c->control & CTRL_STOP)) {
        return false;
    }
    if (!c->loaded) {
        const uint16_t *d = c->descriptors[c->descriptor];

        if (d[0] & (DESC_SKIP | DESC_READ)) {
            return false;
        }
        local = d[6];
    } else {
        bool read = c->latched[0] & DESC_READ;
        bool local_phase = read ? c->staged : !c->staged;

        if (!local_phase) {
            return false;
        }
        local = c->length ? c->unit_local :
                (uint64_t)c->latched[6] + c->cursor;
    }
    memory = vice_memory(local);
    return memory >= 3 && sgi_vice_msp_reserved(&s->msp, memory - 3,
                                               s->next_ns,
                                               s->next_fraction);
}

static void vice_tick(void *opaque)
{
    SGIViceState *s = opaque;
    unsigned channel = s->next_channel;

    if (!vice_channel_active(&s->channel[channel])) {
        channel ^= 1;
    }
    s->next_channel = channel ^ 1;
    if (!vice_dma_deferred(s, &s->channel[channel])) {
        vice_phase(s, channel);
    }
    if (s->unsupported) {
        /* A peer reset cannot make an unadmitted descriptor executable. */
        s->channel[channel].model_blocked = true;
    }
    vice_schedule(s, false);
}

static MemTxResult vice_control(SGIViceState *s, unsigned channel,
                                 uint16_t value, uint16_t written)
{
    ViceChannel *c = &s->channel[channel];
    bool active = c->status & STATUS_ACTIVE;

    if (value & CTRL_RESET) {
        if (!c->reset_held) {
            vice_channel_reset(c);
            c->reset_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        return MEMTX_OK;
    }
    if (c->reset_held) {
        if (!vice_reset_ready(s, c->reset_start)) {
            return vice_refuse(s, "short or unstable channel reset release",
                                0x180 + channel * 0x40);
        }
        if (value & CTRL_GO) {
            return vice_refuse(s, "GO combined with reset release", 0x180);
        }
        c->reset_held = false;
        c->control = 0x10;
        s->unsupported = false;
        if (!s->dma_pending) {
            vice_schedule(s, true);
        }
        return MEMTX_OK;
    }
    if (active) {
        if ((value ^ c->control) & ~(CTRL_GO | CTRL_IE | CTRL_STOP) ||
            ((value & CTRL_GO) && (written & CTRL_GO) &&
             !(value & CTRL_STOP)) ||
            ((c->control & CTRL_STOP) && !(value & CTRL_STOP))) {
            return vice_refuse(s, "active DMA control change", 0x180);
        }
        c->control = (c->control & CTRL_GO) | value;
        return MEMTX_OK;
    }
    if ((c->control & CTRL_STOP) && !(value & CTRL_STOP)) {
        return vice_refuse(s, "STOP release without reset", 0x180);
    }
    if (value & CTRL_GO) {
        unsigned pointer = (value & CTRL_POINTER) >> 4;

        if (s->external_reset || !s->stable_reference || c->blocked ||
            !pointer || (pointer & (pointer - 1)) ||
            (value & ~CTRL_KNOWN) || (value & CTRL_STOP)) {
            return vice_refuse(s, "DMA start configuration", 0x180);
        }
        c->descriptor = ctz32(pointer);
        c->status = STATUS_ACTIVE | (0x10 << c->descriptor) | 0x300;
        c->loaded = false;
        c->staged = false;
        c->length = 0;
        c->filled = 0;
        c->committed = 0;
        c->remaining = 0;
        c->cursor = 0;
        c->control = value;
        if (!s->dma_pending) {
            vice_schedule(s, true);
        }
    } else {
        c->control = value;
    }
    return MEMTX_OK;
}

static int vice_deadline_compare(int64_t a, uint32_t af,
                                  int64_t b, uint32_t bf)
{
    if (a != b) {
        return a < b ? -1 : 1;
    }
    return af == bf ? 0 : af < bf ? -1 : 1;
}

static void vice_reschedule(SGIViceState *s)
{
    bool msp = s->msp.pending && !s->msp.model_blocked;
    int64_t ns = s->next_ns;
    uint32_t fraction = s->next_fraction;

    if (s->unsupported || s->external_reset || (!msp && !s->dma_pending)) {
        timer_del(s->timer);
        return;
    }
    if (msp && (!s->dma_pending ||
        vice_deadline_compare(s->msp.next_ns, s->msp.next_fraction,
                              ns, fraction) < 0)) {
        ns = s->msp.next_ns;
        fraction = s->msp.next_fraction;
    }
    timer_mod_ns(s->timer, ns + !!fraction);
}

static void vice_dispatch(void *opaque)
{
    SGIViceState *s = opaque;
    bool msp = s->msp.pending && !s->msp.model_blocked;
    int order = vice_deadline_compare(s->msp.next_ns, s->msp.next_fraction,
                                      s->next_ns, s->next_fraction);
    bool dma_at_same_time = s->dma_pending && msp && !order;

    if (msp && (!s->dma_pending || order <= 0)) {
        s->msp_servicing = true;
        sgi_vice_msp_tick(&s->msp);
        s->msp_servicing = false;
        if (dma_at_same_time && s->dma_pending && !s->unsupported) {
            s->dma_pending = false;
            vice_tick(s);
        }
    } else if (s->dma_pending) {
        s->dma_pending = false;
        vice_tick(s);
    }
    vice_reschedule(s);
}

static bool vice_msp_fetch(void *opaque, uint16_t pc, uint32_t words[2])
{
    SGIViceState *s = opaque;
    uint8_t bytes[8];

    if (address_space_read(&s->local_as, pc, MEMTXATTRS_UNSPECIFIED,
                            bytes, sizeof(bytes)) != MEMTX_OK) {
        return false;
    }
    words[0] = ldl_be_p(bytes);
    words[1] = ldl_be_p(bytes + 4);
    return true;
}

static bool vice_msp_local(void *opaque, uint32_t address, uint32_t *value,
                            unsigned size, bool write)
{
    SGIViceState *s = opaque;
    uint8_t bytes[8];
    MemTxResult result;

    if (size != 1 && size != 2 && size != 4) {
        return false;
    }
    if (write) {
        stn_be_p(bytes, size, *value);
        result = address_space_write(&s->local_as, address,
                                       MEMTXATTRS_UNSPECIFIED, bytes, size);
    } else {
        result = address_space_read(&s->local_as, address,
                                      MEMTXATTRS_UNSPECIFIED, bytes, size);
        *value = ldn_be_p(bytes, size);
    }
    return result == MEMTX_OK;
}

static bool vice_msp_common(void *opaque, unsigned slot, uint32_t *value,
                             bool write)
{
    uint64_t logical = *value;
    MemTxResult result = vice_access(opaque, slot, &logical, 8, write);

    *value = logical;
    return result == MEMTX_OK;
}

static uint32_t vice_msp_grants(void *opaque)
{
    return ((SGIViceState *)opaque)->bank_grants;
}

static bool vice_msp_admitted(void *opaque)
{
    SGIViceState *s = opaque;

    return s->stable_reference && !s->external_reset;
}

static void vice_msp_held(void *opaque, const char *reason, uint32_t pc)
{
    vice_refuse(opaque, reason, pc);
}

static void vice_msp_exception(void *opaque)
{
    SGIViceState *s = opaque;

    s->interrupt_status |= BIT(3);
    vice_irq(s);
}

static const SGIViceMspOps vice_msp_ops = {
    .fetch = vice_msp_fetch,
    .local = vice_msp_local,
    .common = vice_msp_common,
    .grants = vice_msp_grants,
    .admitted = vice_msp_admitted,
    .held = vice_msp_held,
    .exception = vice_msp_exception,
};

static uint64_t vice_slice(uint64_t value, hwaddr address, unsigned size)
{
    return extract64(value, (8 - (address & 7) - size) * 8, size * 8);
}

static bool vice_whole(hwaddr address, unsigned size, unsigned bytes)
{
    return (address & 7) + size == 8 && size >= bytes;
}

static MemTxResult vice_memory_access(SGIViceState *s, hwaddr address,
                                      uint64_t *value, unsigned size,
                                      bool write, int memory)
{
    uint8_t bytes[8];
    unsigned unit = vice_memories[memory].write_unit;
    unsigned end = vice_memories[memory].base + vice_memories[memory].size;

    if (memory == 0 && s->msp.running) {
        return vice_refuse(s, "MSP runtime instruction PIO", address);
    }
    if (address + size > end || (write && (size < unit || size % unit))) {
        return vice_refuse(s, "partial or crossing local memory access",
                            address);
    }
    if (write) {
        stn_be_p(bytes, size, *value);
        if (memory == 2) {
            for (unsigned i = 0; i < size; i += 4) {
                stl_be_p(bytes + i, ldl_be_p(bytes + i) & 0x3fffff);
            }
        }
        return address_space_write(&s->local_as, address,
                                    MEMTXATTRS_UNSPECIFIED, bytes, size);
    }
    address_space_read(&s->local_as, address, MEMTXATTRS_UNSPECIFIED,
                        bytes, size);
    *value = ldn_be_p(bytes, size);
    return MEMTX_OK;
}

static MemTxResult vice_access(SGIViceState *s, hwaddr address,
                               uint64_t *value, unsigned size, bool write)
{
    hwaddr slot = address & ~7ULL;
    unsigned shift;
    uint64_t mask, payload;
    uint64_t old = 0, updated;
    uint32_t *entry32 = NULL;
    uint16_t *entry16 = NULL;
    unsigned channel = 2, reg = 0;
    bool msp_port = false;
    int memory = vice_memory(address);

    if ((address & (size - 1)) || (address & 7) + size > 8) {
        return vice_refuse(s, "unaligned or crossing transaction", address);
    }
    shift = (8 - (address & 7) - size) * 8;
    mask = size == 8 ? UINT64_MAX : MAKE_64BIT_MASK(shift, size * 8);
    payload = *value << shift;
    if (memory >= 0) {
        return vice_memory_access(s, address, value, size, write, memory);
    }
    if (vice_ram_hole(address)) {
        if (write) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-vice: discarded RAM expansion "
                          "write at 0x%" HWADDR_PRIx "\n", address);
        } else {
            *value = 0;
        }
        return MEMTX_OK;
    }
    if (slot >= 0x1000 && slot < 0x1200) {
        unsigned ch = (slot - 0x1000) / 0x100;
        unsigned d = (slot & 0xff) / 0x40;

        entry16 = &s->channel[ch].descriptors[d][(slot & 0x3f) / 8];
        if (write && (!vice_whole(address, size, 2) ||
            (s->channel[ch].loaded && s->channel[ch].descriptor == d))) {
            return vice_refuse(s, "partial or active descriptor write",
                                address);
        }
    } else if (slot >= 0xf000 && slot < 0xf200) {
        entry32 = &s->translations[(slot - 0xf000) / 8];
        if (write && (!vice_whole(address, size, 4) || (payload & 0xfffc))) {
            return vice_refuse(s, "partial or undefined translation write",
                                address);
        }
    } else if (slot >= 0x180 && slot <= 0x1e8 && slot != 0x1b0 &&
               slot != 0x1b8) {
        channel = (slot - 0x180) / 0x40;
        reg = (slot - 0x180) % 0x40;
        switch (reg) {
        case 0:
            old = s->channel[channel].control;
            break;
        case 8:
            old = s->channel[channel].status;
            break;
        case 0x10:
            old = s->channel[channel].fill;
            break;
        case 0x18:
            old = s->channel[channel].system_pointer;
            break;
        case 0x20:
            old = s->channel[channel].local_pointer;
            break;
        case 0x28:
            old = s->channel[channel].remaining;
            break;
        default:
            return vice_refuse(s, "unknown channel register", address);
        }
        if (write && reg != 0 && reg != 0x10) {
            return vice_refuse(s, "read-only channel register write", address);
        }
    } else {
        switch (slot) {
        case 0:
            old = 0;
            break;
        case 8:
            if (write) {
                return vice_refuse(s, "immutable part ID write", address);
            }
            old = s->part_id;
            break;
        case 0x40: case 0x48: case 0x50: case 0x58:
        case 0x60: case 0x68: case 0x70: {
            uint32_t logical;

            if (write && (slot == 0x58 || slot == 0x68 || slot == 0x70)) {
                return vice_refuse(s, "MSP read-only host register", address);
            }
            sgi_vice_msp_host_read(&s->msp, slot, &logical);
            old = logical;
            msp_port = true;
            break;
        }
        case 0x80:
        case 0x1b8:
            if (!write || !vice_whole(address, size, 4)) {
                return vice_refuse(s, "software interrupt transaction",
                                    address);
            }
            s->interrupt_status |= slot == 0x80 ? 1 << 4 : 1 << 2;
            vice_irq(s);
            return MEMTX_OK;
        case 0x100:
            entry32 = &s->bank_grants;
            break;
        case 0x108:
            if (write) {
                return vice_refuse(s, "clock counter write", address);
            }
            old = clock_ns_to_ticks(s->msp_clock,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->counter_epoch);
            old &= UINT32_MAX;
            break;
        case 0x160:
            old = s->interrupt_status;
            break;
        case 0xe000:
            entry16 = &s->config;
            break;
        case 0xe008:
            if (!write) {
                return vice_refuse(s, "interrupt clear read", address);
            }
            s->interrupt_status &= ~(payload & 0x1ff);
            vice_irq(s);
            return MEMTX_OK;
        case 0xe010:
            entry16 = &s->interrupt_enable;
            break;
        default:
            if (!write && slot >= 0x200 && slot < 0x1000) {
                *value = 0;
                return MEMTX_OK;
            }
            return vice_refuse(s, "core-owned or unassigned port", address);
        }
    }
    if (entry16) {
        old = *entry16;
    } else if (entry32) {
        old = *entry32;
    }
    if (!write) {
        *value = vice_slice(old, address, size);
        return MEMTX_OK;
    }
    if (channel < 2 && !(mask & UINT16_MAX)) {
        return MEMTX_OK;
    }
    updated = (old & ~mask) | payload;
    if (msp_port) {
        if (!(mask & UINT32_MAX)) {
            return MEMTX_OK;
        }
        if (!sgi_vice_msp_host_write(&s->msp, slot, updated)) {
            return MEMTX_ERROR;
        }
        if (slot == 0x40 && !s->msp.model_blocked) {
            s->unsupported = false;
        }
        vice_reschedule(s);
    } else if (channel < 2) {
        if (!reg) {
            return vice_control(s, channel, updated, payload);
        }
        s->channel[channel].fill = updated;
    } else if (entry16) {
        if (slot == 0xe000 && ((uint16_t)updated & ~3U)) {
            return vice_refuse(s, "reserved host configuration bits", address);
        }
        *entry16 = updated;
        if (slot == 0xe010) {
            *entry16 &= 0x1ff;
            vice_irq(s);
        }
    } else if (entry32) {
        *entry32 = updated;
    }
    return MEMTX_OK;
}

static MemTxResult vice_read(void *opaque, hwaddr address, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    *value = 0;
    return vice_access(opaque, address, value, size, false);
}

static MemTxResult vice_write(void *opaque, hwaddr address, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    return vice_access(opaque, address, &value, size, true);
}

static const MemoryRegionOps vice_ops = {
    .read_with_attrs = vice_read,
    .write_with_attrs = vice_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static const VMStateDescription vice_channel_vmstate = {
    .name = TYPE_SGI_VICE "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(control, ViceChannel),
        VMSTATE_UINT16(status, ViceChannel),
        VMSTATE_UINT16(fill, ViceChannel),
        VMSTATE_UINT16_2DARRAY(descriptors, ViceChannel, 4, 8),
        VMSTATE_UINT16_ARRAY(latched, ViceChannel, 8),
        VMSTATE_UINT32(system_pointer, ViceChannel),
        VMSTATE_UINT32(local_pointer, ViceChannel),
        VMSTATE_UINT32(cursor, ViceChannel),
        VMSTATE_UINT16(remaining, ViceChannel),
        VMSTATE_UINT8(descriptor, ViceChannel),
        VMSTATE_UINT8(length, ViceChannel),
        VMSTATE_UINT8(filled, ViceChannel),
        VMSTATE_UINT8(committed, ViceChannel),
        VMSTATE_UINT32(unit_local, ViceChannel),
        VMSTATE_BUFFER(staging, ViceChannel),
        VMSTATE_BOOL(loaded, ViceChannel),
        VMSTATE_BOOL(staged, ViceChannel),
        VMSTATE_BOOL(blocked, ViceChannel),
        VMSTATE_BOOL(model_blocked, ViceChannel),
        VMSTATE_BOOL(reset_held, ViceChannel),
        VMSTATE_INT64(reset_start, ViceChannel),
        VMSTATE_END_OF_LIST()
    },
};

static int vice_pre_save(void *opaque)
{
    SGIViceState *s = opaque;

    s->saved_clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return 0;
}

static int vice_post_load(void *opaque, int version_id)
{
    SGIViceState *s = opaque;
    int64_t delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->saved_clock;

    /* qtest and snapshot restore need not share the source clock's epoch. */
    s->counter_epoch += delta;
    s->external_reset_start += delta;
    s->next_ns += delta;
    if (!sgi_vice_msp_rebase(&s->msp, delta)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < 2; i++) {
        ViceChannel *c = &s->channel[i];

        if (c->descriptor > 3 || c->length > 8 || c->filled > c->length ||
            c->committed > c->length || c->cursor > UINT16_MAX ||
            (c->staged && (!c->length || c->filled != c->length))) {
            return -EINVAL;
        }
        c->reset_start += delta;
    }
    if (timer_pending(s->timer)) {
        timer_mod_ns(s->timer, timer_expire_time_ns(s->timer) + delta);
    }
    vice_reschedule(s);
    vice_irq(s);
    return 0;
}

static const VMStateDescription vice_vmstate = {
    .name = TYPE_SGI_VICE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = vice_pre_save,
    .post_load = vice_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(part_id, SGIViceState),
        VMSTATE_UINT32_EQUAL(device_clock_hz, SGIViceState),
        VMSTATE_UINT32_EQUAL(msp_clock_hz, SGIViceState),
        VMSTATE_INT64(saved_clock, SGIViceState),
        VMSTATE_UINT16(config, SGIViceState),
        VMSTATE_UINT16(interrupt_status, SGIViceState),
        VMSTATE_UINT16(interrupt_enable, SGIViceState),
        VMSTATE_UINT32(bank_grants, SGIViceState),
        VMSTATE_UINT32_ARRAY(translations, SGIViceState, 64),
        VMSTATE_STRUCT_ARRAY(channel, SGIViceState, 2, 0,
                             vice_channel_vmstate, ViceChannel),
        VMSTATE_INT64(counter_epoch, SGIViceState),
        VMSTATE_INT64(next_ns, SGIViceState),
        VMSTATE_UINT32(next_fraction, SGIViceState),
        VMSTATE_UINT8(next_channel, SGIViceState),
        VMSTATE_BOOL(external_reset, SGIViceState),
        VMSTATE_BOOL(initialized, SGIViceState),
        VMSTATE_BOOL(unsupported, SGIViceState),
        VMSTATE_INT64(external_reset_start, SGIViceState),
        VMSTATE_TIMER_PTR(timer, SGIViceState),
        VMSTATE_BOOL(dma_pending, SGIViceState),
        VMSTATE_STRUCT(msp, SGIViceState, 0, vmstate_sgi_vice_msp, SGIViceMsp),
        VMSTATE_CLOCK(device_clock, SGIViceState),
        VMSTATE_CLOCK(msp_clock, SGIViceState),
        VMSTATE_END_OF_LIST()
    },
};

static void vice_realize(DeviceState *dev, Error **errp)
{
    SGIViceState *s = SGI_VICE(dev);

    if (s->part_id != 0xe1 || !s->device_clock_hz || !s->msp_clock_hz) {
        error_setg(errp, "VICE requires original part-id=225 and explicit "
                   "nonzero device-clock-hz and msp-clock-hz");
        return;
    }
    clock_set_hz(s->device_clock, s->device_clock_hz);
    clock_set_hz(s->msp_clock, s->msp_clock_hz);
    memory_region_init(&s->local_bus, OBJECT(s), "vice-local-bus", 0x10000);
    for (unsigned i = 0; i < G_N_ELEMENTS(vice_memories); i++) {
        if (!memory_region_init_ram(&s->ram[i], OBJECT(s),
                                    vice_memories[i].name,
                                    vice_memories[i].size, errp)) {
            return;
        }
        memory_region_add_subregion(&s->local_bus, vice_memories[i].base,
                                    &s->ram[i]);
    }
    address_space_init(&s->local_as, &s->local_bus, "vice-local");
    address_space_init(&s->dma_as,
                        s->dma_memory ? s->dma_memory : get_system_memory(),
                        "vice-system");
}

static void vice_unrealize(DeviceState *dev)
{
    SGIViceState *s = SGI_VICE(dev);

    timer_del(s->timer);
    address_space_destroy(&s->local_as);
    address_space_destroy(&s->dma_as);
}

static void vice_init(Object *obj)
{
    SGIViceState *s = SGI_VICE(obj);

    s->device_clock = qdev_init_clock_in(DEVICE(s), "device-clock", NULL, s, 0);
    s->msp_clock = qdev_init_clock_in(DEVICE(s), "msp-clock", NULL, s, 0);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vice_dispatch, s);
    sgi_vice_msp_init(&s->msp, &vice_msp_ops, s, s->device_clock);
    memory_region_init_io(&s->aperture, obj, &vice_ops, s,
                          "vice-host", 0x100000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->aperture);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_in_named(DEVICE(s), vice_external_reset,
                            "external-reset", 1);
}

static void vice_finalize(Object *obj)
{
    timer_free(SGI_VICE(obj)->timer);
}

static const Property vice_properties[] = {
    DEFINE_PROP_UINT8("part-id", SGIViceState, part_id, 0),
    DEFINE_PROP_UINT32("device-clock-hz", SGIViceState, device_clock_hz, 0),
    DEFINE_PROP_UINT32("msp-clock-hz", SGIViceState, msp_clock_hz, 0),
    DEFINE_PROP_BOOL("stable-reference", SGIViceState, stable_reference, false),
    DEFINE_PROP_LINK("dma-memory", SGIViceState, dma_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void vice_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->user_creatable = true;
    dc->realize = vice_realize;
    dc->unrealize = vice_unrealize;
    dc->vmsd = &vice_vmstate;
    rc->phases.enter = vice_reset_enter;
    device_class_set_props(dc, vice_properties);
}

static const TypeInfo vice_info = {
    .name = TYPE_SGI_VICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIViceState),
    .instance_init = vice_init,
    .instance_finalize = vice_finalize,
    .class_init = vice_class_init,
};

static void vice_register_types(void)
{
    type_register_static(&vice_info);
}

type_init(vice_register_types)
