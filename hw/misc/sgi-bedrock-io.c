/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/dma.h"

#define II_WIDGET_ID       0x00000
#define II_WIDGET_STATUS   0x00008
#define II_WIDGET_CONTROL  0x00020
#define II_LOCAL_PROTECT   0x00100
#define II_OUT_ACCESS      0x00110
#define II_IN_ACCESS       0x00118
#define II_DEVICE_MASK     0x00120
#define II_LINK_STATUS     0x00128
#define II_LINK_LOG        0x00130
#define II_INTERRUPT_DEST  0x00138
#define II_SCRATCH0        0x00150
#define II_ITTE0           0x00160
#define II_REQBUF0         0x00198
#define II_MISC_ERROR_MASK 0x001e8
#define II_CREDIT_TIMEOUT  0x001e0
#define II_TAIL_TIMEOUT    0x001f0
#define II_ERROR_CLEAR     0x001f8
#define II_BTE_CONTROL     0x00200
#define II_REQ_MANAGEMENT  0x003a8
#define II_REQ_CONTROL     0x003b0
#define II_REQ_TIMEOUT     0x003b8
#define II_REQ_PRESCALER   0x003c0
#define II_BTE0            0x10000
#define II_BTE1            0x20000

#define II_WIDGET_ID_VALUE ((UINT64_C(0xc110) << 12) | (UINT64_C(0x36) << 1))
#define II_WIDGET_CONTROL_MASK UINT64_C(0x7fe01f)
#define II_LINK_STATUS_MASK UINT64_C(0x3ff0fff7d3f)
#define II_LINK_RESET UINT64_C(0x3fc0fff6403)
#define II_BTE_BUSY (UINT64_C(1) << 20)
#define II_BTE_ERROR (UINT64_C(1) << 16)
#define II_BTE_LENGTH_MASK UINT64_C(0xffff)
#define II_BTE_ADDR_MASK UINT64_C(0xffffffff80)
#define II_PASSWORD UINT64_C(0x53474972756c6573)

/*
 * Crosstalk bit 47 selects register space over memory.  The address map
 * puts the target node in bits 46:39 and the Bedrock's own registers in
 * small window 1 of the node's I/O space in the checked remote form (bit
 * 23), 8 MB wide, with the second processor interface 0x200000 above the
 * first.
 */
#define II_XIO_REGISTER   (UINT64_C(1) << 47)
#define II_XIO_NODE_SHIFT 39
#define II_XIO_NODE_WIDTH 8
#define II_XIO_HUB_WINDOW UINT64_C(0x1800000)
#define II_XIO_HUB_MASK   UINT64_C(0x7fffff)

static int ii_retained_index(hwaddr offset)
{
    static const hwaddr singleton[] = {
        0x208, 0x210, 0x218, 0x220, 0x228, 0x300,
        0x140, 0x148, 0x388, 0x390, 0x398, 0x3a0, 0x600, 0x608, 0x610,
        0x30000, 0x30008,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(singleton); i++) {
        if (offset == singleton[i]) {
            return i;
        }
    }
    if (offset >= 0x308 && offset < 0x348 && !(offset & 7)) {
        return 17 + (offset - 0x308) / 8;
    }
    if (offset >= 0x400 && offset <= 0x5d8 && !(offset & 7)) {
        return 25 + (offset - 0x400) / 8;
    }
    return -1;
}

static unsigned ii_reqbuf_index(hwaddr offset)
{
    if (offset == II_REQBUF0) {
        return 0;
    }
    if (offset >= II_REQBUF0 + 8 && offset <= II_REQBUF0 + 64) {
        return 1 + (offset - II_REQBUF0 - 8) / 8;
    }
    return 9;
}

static SGIBedrockIOBTE *ii_bte(SGIBedrockIOState *s, hwaddr *offset)
{
    if (*offset >= II_BTE0 && *offset < II_BTE0 + 0x30) {
        *offset -= II_BTE0;
        return &s->bte[0];
    }
    if (*offset >= II_BTE1 && *offset < II_BTE1 + 0x30) {
        *offset -= II_BTE1;
        return &s->bte[1];
    }
    return NULL;
}

/*
 * The engine's interrupt-address register names where its completion
 * interrupt lands: bit 0 selects the processor interface, bits 8:1 the node
 * and bits 22:16 the level.  The hub delivers it by setting that level in the
 * interface's interrupt-pending register through the node's register window,
 * offset 0x90 for interface 0 and 0x200090 for interface 1 -- the same modify
 * register an external widget's interrupt packet reaches.  A modify write
 * carries the level in bits 6:0 and sets it with bit 8.
 */
static void ii_bte_interrupt(SGIBedrockIOState *s, SGIBedrockIOBTE *bte)
{
    unsigned node = extract64(bte->interrupt, 1, 8);
    unsigned level = extract64(bte->interrupt, 16, 7);
    hwaddr offset = (bte->interrupt & 1) ? 0x200090 : 0x90;
    uint8_t buffer[8];

    if (!s->hub_as_initialized) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: BTE completion interrupt "
                      "with no hub registers linked\n");
        return;
    }
    if (node != s->node_number) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: BTE completion interrupt to "
                      "node %u off this node is unsupported\n", node);
        return;
    }
    stq_be_p(buffer, (UINT64_C(1) << 8) | level);
    if (address_space_write(&s->hub_as, offset, MEMTXATTRS_UNSPECIFIED,
                            buffer, sizeof(buffer)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-io: BTE completion interrupt not delivered\n");
    }
}

/*
 * A finished engine reports completion the way its launch selected.  With the
 * notify control bit it writes the final length-status word to the
 * notification line; without it it raises the completion interrupt the
 * interrupt-address register names.
 *
 * The interrupt lands in the node's own register window, which reaches this
 * I/O interface again.  A launch that finishes synchronously -- a
 * bounds-checked failure, or a zero-latency engine -- runs ii_bte_signal from
 * inside the launching MMIO write, and a self-directed completion writes back
 * into this device; either way QEMU's re-entrancy guard would drop the write.
 * Deliver the interrupt from a bottom half that runs once the launching access
 * has returned.  The notification-line write targets ordinary memory and stays
 * synchronous.
 */
static void ii_bte_signal(SGIBedrockIOState *s, SGIBedrockIOBTE *bte)
{
    uint64_t value;

    if (!(bte->control & (1 << 4))) {
        bte->signal_pending = true;
        qemu_bh_schedule(bte->signal_bh);
        return;
    }
    value = cpu_to_be64(bte->status);
    if (dma_memory_write(s->dma_as, bte->notification, &value, sizeof(value),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-io: BTE notification write failed\n");
    }
}

static void ii_bte_signal_bh(void *opaque)
{
    SGIBedrockIOBTE *bte = opaque;

    bte->signal_pending = false;
    ii_bte_interrupt(bte->io, bte);
}

static void ii_bte_complete(void *opaque)
{
    SGIBedrockIOBTE *bte = opaque;
    SGIBedrockIOState *s = bte->io;
    uint8_t buffer[128] = { 0 };
    MemTxResult result = MEMTX_OK;

    if (!(bte->control & 1)) {
        result = dma_memory_read(s->dma_as, bte->source, buffer, sizeof(buffer),
                                 MEMTXATTRS_UNSPECIFIED);
    }
    if (result == MEMTX_OK) {
        result = dma_memory_write(s->dma_as, bte->destination, buffer,
                                  sizeof(buffer),
                                  MEMTXATTRS_UNSPECIFIED);
    }
    if (result != MEMTX_OK) {
        bte->status = (bte->status & II_BTE_LENGTH_MASK) | II_BTE_ERROR;
    } else {
        uint64_t remaining = bte->status & II_BTE_LENGTH_MASK;

        if (remaining) {
            remaining--;
            bte->source = (bte->source + 128) & II_BTE_ADDR_MASK;
            bte->destination = (bte->destination + 128) & II_BTE_ADDR_MASK;
        }
        bte->status = remaining | (remaining ? II_BTE_BUSY : 0);
        if (remaining) {
            timer_mod_ns(bte->timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
            return;
        }
    }
    bte->active = false;
    ii_bte_signal(s, bte);
}

static uint64_t ii_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockIOState *s = opaque;
    SGIBedrockIOBTE *bte;
    unsigned index = ii_reqbuf_index(offset);
    int retained = ii_retained_index(offset);

    if (retained >= 0) {
        return s->retained[retained];
    }
    if (index < 9) {
        return s->request_buffer[index];
    }
    bte = ii_bte(s, &offset);
    if (bte) {
        switch (offset) {
        case 0: return bte->status;
        case 8: return bte->source;
        case 0x10: return bte->destination;
        case 0x18:
            timer_del(bte->timer);
            bte->active = false;
            bte->status &= ~II_BTE_BUSY;
            return bte->control;
        case 0x20: return bte->notification;
        case 0x28: return bte->interrupt;
        }
    }
    switch (offset) {
    case II_WIDGET_ID: return II_WIDGET_ID_VALUE | (uint64_t)s->revision << 28;
    case II_WIDGET_STATUS:
        /* No request pending, no timeout, retry or error detected. */
        return 0;
    case II_WIDGET_CONTROL: return s->widget_control;
    case II_LOCAL_PROTECT: return s->local_protect;
    case II_OUT_ACCESS: return s->outbound_access;
    case II_IN_ACCESS: return s->inbound_access;
    case II_DEVICE_MASK: return s->device_mask;
    case II_LINK_STATUS: return s->link_status;
    case II_LINK_LOG: return s->link_log;
    case II_INTERRUPT_DEST: return s->interrupt_destination & ~(1 << 28);
    case II_SCRATCH0: return s->scratch[0];
    case II_SCRATCH0 + 8: return s->scratch[1];
    case II_MISC_ERROR_MASK: return s->misc_error_mask;
    case II_CREDIT_TIMEOUT: return s->credit_timeout;
    case II_TAIL_TIMEOUT: return s->tail_timeout;
    case II_BTE_CONTROL: return s->bte_control;
    case II_REQ_MANAGEMENT: return s->request_management;
    case II_REQ_CONTROL: return s->request_control;
    case II_REQ_TIMEOUT: return s->request_timeout;
    case II_REQ_PRESCALER: return s->request_prescaler;
    case II_ERROR_CLEAR: return 0;
    default:
        if (offset >= II_ITTE0 && offset < II_ITTE0 + 7 * 8) {
            return s->itte[(offset - II_ITTE0) / 8];
        }
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void ii_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SGIBedrockIOState *s = opaque;
    SGIBedrockIOBTE *bte;
    unsigned index = ii_reqbuf_index(offset);
    int retained = ii_retained_index(offset);

    /* Both keep all 64 bits: model policy, not an established field map. */
    if (retained >= 0) {
        s->retained[retained] = value;
        return;
    }
    if (index < 9) {
        s->request_buffer[index] = value;
        return;
    }
    bte = ii_bte(s, &offset);
    if (bte) {
        if (bte->active) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bedrock-io: BTE write while active\n");
            return;
        }
        switch (offset) {
        case 0:
            bte->status = value & (II_BTE_LENGTH_MASK | II_BTE_BUSY);
            return;
        case 8:
            bte->source = value & II_BTE_ADDR_MASK;
            return;
        case 0x10:
            bte->destination = value & II_BTE_ADDR_MASK;
            return;
        case 0x18:
            bte->control = value & 0x111;
            if (bte->control & (1 << 8)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-bedrock-io: BTE poison is unsupported\n");
                return;
            }
            if (!(bte->status & II_BTE_BUSY)) {
                return;
            }
            if (!(bte->status & II_BTE_LENGTH_MASK)) {
                bte->status = 0;
                qemu_log_mask(LOG_UNIMP,
                              "sgi-bedrock-io: zero-count BTE start\n");
                return;
            }
            {
                uint64_t bytes = (bte->status & II_BTE_LENGTH_MASK) * 128;
                bool source_overflow = !(bte->control & 1) &&
                    bte->source > (UINT64_C(1) << 40) - bytes;
                bool destination_overflow =
                    bte->destination > (UINT64_C(1) << 40) - bytes;

                if (source_overflow || destination_overflow) {
                    bte->status = (bte->status & II_BTE_LENGTH_MASK) |
                                  II_BTE_ERROR;
                    ii_bte_signal(s, bte);
                    return;
                }
            }
            bte->active = true;
            timer_mod_ns(bte->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
            return;
        case 0x20:
            bte->notification = value & II_BTE_ADDR_MASK;
            return;
        case 0x28:
            bte->interrupt = value & 0x7f01ff;
            return;
        }
    }
    switch (offset) {
    case II_WIDGET_ID:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-io: write to the "
                      "read-only widget identification ignored\n");
        return;
    case II_WIDGET_STATUS:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: widget status write "
                      "ignored, no error state to clear\n");
        return;
    case II_WIDGET_CONTROL:
        s->widget_control = value & II_WIDGET_CONTROL_MASK;
        return;
    case II_LOCAL_PROTECT:
        s->local_protect = value;
        return;
    case II_LOCAL_PROTECT + 8:
        if (value == II_PASSWORD) {
            s->local_protect |= UINT64_C(1) << s->local_region;
        }
        return;
    case II_OUT_ACCESS:
        s->outbound_access = value;
        return;
    case II_IN_ACCESS:
        s->inbound_access = value;
        return;
    case II_DEVICE_MASK:
        s->device_mask |= value;
        return;
    case II_LINK_STATUS:
        s->link_status = value & II_LINK_STATUS_MASK;
        return;
    case II_LINK_LOG:
        if (value == 0) {
            s->link_log = 0;
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-bedrock-io: nonzero link-log write\n");
        }
        return;
    case II_INTERRUPT_DEST:
        s->interrupt_destination = value & UINT64_C(0x101ff7f);
        return;
    case II_SCRATCH0:
        s->scratch[0] = value;
        return;
    case II_SCRATCH0 + 8:
        s->scratch[1] = value;
        return;
    case II_MISC_ERROR_MASK:
        s->misc_error_mask |= value & 0x1111;
        return;
    case II_CREDIT_TIMEOUT:
        s->credit_timeout = value & 0x3ffffff;
        return;
    case II_TAIL_TIMEOUT:
        s->tail_timeout = value & UINT64_C(0x0fffffff03ffffff);
        return;
    case II_ERROR_CLEAR:
        if (value) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: error clear 0x%" PRIx64
                          " ignored, no error state to clear\n", value);
        }
        return;
    case II_BTE_CONTROL:
        s->bte_control = value & 0x10f;
        return;
    case II_REQ_MANAGEMENT:
        s->request_management = value & UINT64_C(0x13ffffffff30f3);
        return;
    case II_REQ_CONTROL:
        s->request_control = value & 0x1ff0f;
        return;
    case II_REQ_TIMEOUT:
        s->request_timeout = value & 0xff;
        return;
    case II_REQ_PRESCALER:
        s->request_prescaler = value & 0xffffff;
        return;
    default:
        if (offset >= II_ITTE0 && offset < II_ITTE0 + 7 * 8) {
            s->itte[(offset - II_ITTE0) / 8] = value & 0x1f1f;
            return;
        }
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: unsupported write at 0x%"
                      HWADDR_PRIx " value 0x%" PRIx64 "\n", offset, value);
    }
}

static MemTxResult ii_pio_rw(SGIBedrockIOState *s, hwaddr offset,
                             uint64_t *data, unsigned size, bool write)
{
    uint8_t buffer[8];
    unsigned widget;
    hwaddr target;
    MemTxResult result;

    if (offset < 0x10000000) {
        widget = offset >> 24;
        target = offset & 0xffffff;
    } else if (offset >= 0x20000000) {
        unsigned entry = (offset >> 29) - 1;
        uint64_t itte;

        if (entry >= 7) {
            return MEMTX_DECODE_ERROR;
        }
        itte = s->itte[entry];
        widget = extract64(itte, 8, 4);
        target = (extract64(itte, 0, 5) << 29) | (offset & 0x1fffffff);
        if (itte & (1 << 12)) {
            target |= UINT64_C(1) << 47;
        }
    } else {
        return MEMTX_DECODE_ERROR;
    }
    if (!(s->local_protect & (UINT64_C(1) << s->local_region)) ||
        !(s->outbound_access & (UINT64_C(1) << widget))) {
        return MEMTX_ACCESS_ERROR;
    }
    if (widget == 1 && target == 0) {
        uint64_t identity = II_WIDGET_ID_VALUE | (uint64_t)s->revision << 28;

        if (!write) {
            *data = identity;
        }
        return MEMTX_OK;
    }
    if (write) {
        switch (size) {
        case 1:
            buffer[0] = *data;
            break;
        case 2:
            stw_be_p(buffer, *data);
            break;
        case 4:
            stl_be_p(buffer, *data);
            break;
        case 8:
            stq_be_p(buffer, *data);
            break;
        default:
            return MEMTX_DECODE_ERROR;
        }
        if (!s->widget_as_initialized[widget]) {
            return MEMTX_DECODE_ERROR;
        }
        return address_space_write(&s->widget_as[widget], target,
                                   MEMTXATTRS_UNSPECIFIED,
                                   buffer, size);
    }
    if (!s->widget_as_initialized[widget]) {
        return MEMTX_DECODE_ERROR;
    }
    result = address_space_read(&s->widget_as[widget], target,
                                MEMTXATTRS_UNSPECIFIED,
                                buffer, size);
    if (result != MEMTX_OK) {
        return result;
    }
    switch (size) {
    case 1:
        *data = buffer[0];
        break;
    case 2:
        *data = lduw_be_p(buffer);
        break;
    case 4:
        *data = ldl_be_p(buffer);
        break;
    case 8:
        *data = ldq_be_p(buffer);
        break;
    }
    return MEMTX_OK;
}

static MemTxResult ii_inbound_register(SGIBedrockIOState *s, hwaddr address,
                                       void *buffer, size_t size, bool write)
{
    unsigned node = extract64(address, II_XIO_NODE_SHIFT, II_XIO_NODE_WIDTH);
    hwaddr window = extract64(address, 0, II_XIO_NODE_SHIFT);

    if (node != s->node_number) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-io: crosstalk register "
                      "access to node %u arrives nowhere\n", node);
        return MEMTX_DECODE_ERROR;
    }
    if ((window & ~II_XIO_HUB_MASK) != II_XIO_HUB_WINDOW) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: unsupported crosstalk "
                      "register access at 0x%" HWADDR_PRIx "\n", window);
        return MEMTX_DECODE_ERROR;
    }
    if (!s->hub_as_initialized) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-io: hub-registers not linked\n");
        return MEMTX_DECODE_ERROR;
    }
    if (write) {
        return address_space_write(&s->hub_as, window & II_XIO_HUB_MASK,
                                   MEMTXATTRS_UNSPECIFIED, buffer, size);
    }
    return address_space_read(&s->hub_as, window & II_XIO_HUB_MASK,
                              MEMTXATTRS_UNSPECIFIED, buffer, size);
}

/* The shared window must check every possible source before forwarding. */
static bool ii_inbound_permitted(SGIBedrockIOState *s, unsigned widget,
                                 unsigned device)
{
    if (widget > 15 || device > 7 ||
        !(s->inbound_access & (UINT64_C(1) << widget))) {
        return false;
    }
    if (widget >= 8 &&
        !(s->device_mask & (UINT64_C(1) << ((widget - 8) * 8 + device)))) {
        return false;
    }
    return true;
}

MemTxResult sgi_bedrock_io_inbound_rw(SGIBedrockIOState *s, unsigned widget,
                                      unsigned device, hwaddr address,
                                      void *buffer, size_t size, bool write)
{
    if (!ii_inbound_permitted(s, widget, device)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-io: inbound %s from "
                      "widget %u device %u at 0x%" HWADDR_PRIx " refused "
                      "(inbound-access=0x%" PRIx64 " device-mask=0x%" PRIx64
                      ")\n", write ? "write" : "read", widget, device,
                      address, s->inbound_access, s->device_mask);
        return MEMTX_ACCESS_ERROR;
    }
    if (address & II_XIO_REGISTER) {
        return ii_inbound_register(s, address, buffer, size, write);
    }
    if (write) {
        return address_space_write(s->dma_as, address, MEMTXATTRS_UNSPECIFIED,
                                   buffer, size);
    }
    return address_space_read(s->dma_as, address, MEMTXATTRS_UNSPECIFIED,
                              buffer, size);
}

void sgi_bedrock_io_disable_device(SGIBedrockIOState *s, unsigned widget,
                                    unsigned device)
{
    if (widget >= 8 && widget <= 15 && device < 8) {
        s->device_mask &= ~(UINT64_C(1) << ((widget - 8) * 8 + device));
    }
}

/* With no source ID on the shared window, a denied candidate must block it. */
static bool ii_inbound_pick_widget(SGIBedrockIOState *s, unsigned *widget)
{
    bool found = false;

    for (unsigned candidate = 0; candidate < 16; candidate++) {
        if (!(s->inbound_widget_mask & (1U << candidate))) {
            continue;
        }
        if (!ii_inbound_permitted(s, candidate, s->inbound_device)) {
            return false;
        }
        if (!found) {
            *widget = candidate;
            found = true;
        }
    }
    return found;
}

static MemTxResult ii_inbound_read(void *opaque, hwaddr offset, uint64_t *value,
                                   unsigned size, MemTxAttrs attrs)
{
    SGIBedrockIOState *s = opaque;
    uint8_t buffer[8] = { 0 };
    MemTxResult result;
    unsigned widget;

    if (!ii_inbound_pick_widget(s, &widget)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-io: inbound read at 0x%"
                      HWADDR_PRIx " refused for widget mask 0x%x "
                      "(inbound-access=0x%" PRIx64 " device-mask=0x%" PRIx64
                      ")\n", offset, s->inbound_widget_mask,
                      s->inbound_access, s->device_mask);
        return MEMTX_ACCESS_ERROR;
    }
    result = sgi_bedrock_io_inbound_rw(s, widget, s->inbound_device, offset,
                                       buffer, size, false);
    if (result != MEMTX_OK) {
        return result;
    }
    switch (size) {
    case 1:
        *value = buffer[0];
        break;
    case 2:
        *value = lduw_be_p(buffer);
        break;
    case 4:
        *value = ldl_be_p(buffer);
        break;
    case 8:
        *value = ldq_be_p(buffer);
        break;
    }
    return MEMTX_OK;
}

static MemTxResult ii_inbound_write(void *opaque, hwaddr offset, uint64_t value,
                                    unsigned size, MemTxAttrs attrs)
{
    SGIBedrockIOState *s = opaque;
    uint8_t buffer[8];
    unsigned widget;

    switch (size) {
    case 1:
        buffer[0] = value;
        break;
    case 2:
        stw_be_p(buffer, value);
        break;
    case 4:
        stl_be_p(buffer, value);
        break;
    case 8:
        stq_be_p(buffer, value);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    if (!ii_inbound_pick_widget(s, &widget)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-io: inbound write at 0x%"
                      HWADDR_PRIx " refused for widget mask 0x%x "
                      "(inbound-access=0x%" PRIx64 " device-mask=0x%" PRIx64
                      ")\n", offset, s->inbound_widget_mask,
                      s->inbound_access, s->device_mask);
        return MEMTX_ACCESS_ERROR;
    }
    return sgi_bedrock_io_inbound_rw(s, widget, s->inbound_device, offset,
                                     buffer, size, true);
}

static MemTxResult ii_pio_read(void *opaque, hwaddr offset, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    *value = 0;
    return ii_pio_rw(opaque, offset, value, size, false);
}

static MemTxResult ii_pio_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    return ii_pio_rw(opaque, offset, &value, size, true);
}

static const MemoryRegionOps ii_ops = {
    .read = ii_read, .write = ii_write, .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static const MemoryRegionOps ii_pio_ops = {
    .read_with_attrs = ii_pio_read,
    .write_with_attrs = ii_pio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static const MemoryRegionOps ii_inbound_ops = {
    .read_with_attrs = ii_inbound_read,
    .write_with_attrs = ii_inbound_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * A composing parent may hand over its own DMA address space instead of
 * linking a region.  Only before realize: engines may be mid-transfer after.
 */
void sgi_bedrock_io_set_dma_as(SGIBedrockIOState *s, AddressSpace *as)
{
    assert(!DEVICE(s)->realized);
    s->dma_as = as;
}

static void ii_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockIOState *s = SGI_BEDROCK_IO(obj);

    s->widget_control = 0xa;
    s->local_protect = UINT64_MAX;
    s->outbound_access = UINT64_MAX;
    s->inbound_access = UINT64_MAX;
    s->device_mask = s->device_mask_reset;
    s->link_status = II_LINK_RESET;
    s->link_log = 0;
    s->interrupt_destination = 0;
    s->credit_timeout = 0;
    s->tail_timeout = 0;
    s->bte_control = 0;
    s->request_management = 0;
    s->request_control = 0;
    s->request_timeout = 0;
    s->request_prescaler = 0;
    s->misc_error_mask = 0;
    memset(s->scratch, 0, sizeof(s->scratch));
    memset(s->itte, 0, sizeof(s->itte));
    memset(s->request_buffer, 0, sizeof(s->request_buffer));
    memset(s->retained, 0, sizeof(s->retained));
    for (unsigned i = 0; i < 2; i++) {
        timer_del(s->bte[i].timer);
        qemu_bh_cancel(s->bte[i].signal_bh);
        s->bte[i].status = 0;
        s->bte[i].source = 0;
        s->bte[i].destination = 0;
        s->bte[i].control = 0;
        s->bte[i].notification = 0;
        s->bte[i].interrupt = 0;
        s->bte[i].active = false;
        s->bte[i].signal_pending = false;
    }
}

static const VMStateDescription vmstate_ii_bte = {
    .name = "sgi-bedrock-io/bte", .version_id = 2, .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(status, SGIBedrockIOBTE),
        VMSTATE_UINT64(source, SGIBedrockIOBTE),
        VMSTATE_UINT64(destination, SGIBedrockIOBTE),
        VMSTATE_UINT64(control, SGIBedrockIOBTE),
        VMSTATE_UINT64(notification, SGIBedrockIOBTE),
        VMSTATE_UINT64(interrupt, SGIBedrockIOBTE),
        VMSTATE_BOOL(active, SGIBedrockIOBTE),
        VMSTATE_BOOL(signal_pending, SGIBedrockIOBTE),
        VMSTATE_TIMER_PTR(timer, SGIBedrockIOBTE), VMSTATE_END_OF_LIST()
    },
};

static int ii_post_load(void *opaque, int version_id)
{
    SGIBedrockIOState *s = opaque;

    /* A completion that had not yet delivered its interrupt resumes here. */
    for (unsigned i = 0; i < 2; i++) {
        if (s->bte[i].signal_pending) {
            qemu_bh_schedule(s->bte[i].signal_bh);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_ii = {
    .name = TYPE_SGI_BEDROCK_IO, .version_id = 2, .minimum_version_id = 2,
    .post_load = ii_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(widget_control, SGIBedrockIOState),
        VMSTATE_UINT64(local_protect, SGIBedrockIOState),
        VMSTATE_UINT64(outbound_access, SGIBedrockIOState),
        VMSTATE_UINT64(inbound_access, SGIBedrockIOState),
        VMSTATE_UINT64(device_mask, SGIBedrockIOState),
        VMSTATE_UINT64(link_status, SGIBedrockIOState),
        VMSTATE_UINT64(link_log, SGIBedrockIOState),
        VMSTATE_UINT64(interrupt_destination, SGIBedrockIOState),
        VMSTATE_UINT64(credit_timeout, SGIBedrockIOState),
        VMSTATE_UINT64(tail_timeout, SGIBedrockIOState),
        VMSTATE_UINT64(bte_control, SGIBedrockIOState),
        VMSTATE_UINT64(request_management, SGIBedrockIOState),
        VMSTATE_UINT64(request_control, SGIBedrockIOState),
        VMSTATE_UINT64(request_timeout, SGIBedrockIOState),
        VMSTATE_UINT64(request_prescaler, SGIBedrockIOState),
        VMSTATE_UINT64(misc_error_mask, SGIBedrockIOState),
        VMSTATE_UINT64_ARRAY(scratch, SGIBedrockIOState, 2),
        VMSTATE_UINT64_ARRAY(itte, SGIBedrockIOState, 7),
        VMSTATE_UINT64_ARRAY(request_buffer, SGIBedrockIOState, 9),
        VMSTATE_UINT64_ARRAY(retained, SGIBedrockIOState, 96),
        VMSTATE_STRUCT_ARRAY(bte, SGIBedrockIOState, 2, 1, vmstate_ii_bte,
                             SGIBedrockIOBTE),
        VMSTATE_END_OF_LIST()
    },
};

static void ii_init(Object *obj)
{
    SGIBedrockIOState *s = SGI_BEDROCK_IO(obj);

    memory_region_init_io(&s->regs, obj, &ii_ops, s, TYPE_SGI_BEDROCK_IO,
                          SGI_BEDROCK_IO_REG_SIZE);
    memory_region_init_io(&s->pio, obj, &ii_pio_ops, s,
                          TYPE_SGI_BEDROCK_IO "-pio", SGI_BEDROCK_IO_PIO_SIZE);
    /*
     * A widget reached through this window may answer synchronously, with
     * DMA or an interrupt packet that returns through our own inbound
     * region.  The re-entrancy guard is per device, so an outer PIO
     * callback would block that second dispatch.  Inbound stays guarded.
     */
    s->pio.disable_reentrancy_guard = true;
    memory_region_init_io(&s->inbound, obj, &ii_inbound_ops, s,
                          TYPE_SGI_BEDROCK_IO "-inbound",
                          SGI_BEDROCK_IO_INBOUND_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->inbound);
    for (unsigned i = 0; i < 2; i++) {
        s->bte[i].io = s;
        s->bte[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ii_bte_complete,
                                      &s->bte[i]);
        s->bte[i].signal_bh = qemu_bh_new_guarded(ii_bte_signal_bh, &s->bte[i],
                                      &DEVICE(obj)->mem_reentrancy_guard);
    }
}

static void ii_finalize(Object *obj)
{
    SGIBedrockIOState *s = SGI_BEDROCK_IO(obj);

    timer_free(s->bte[0].timer);
    timer_free(s->bte[1].timer);
    qemu_bh_delete(s->bte[0].signal_bh);
    qemu_bh_delete(s->bte[1].signal_bh);
    if (s->linked_dma_as_initialized) {
        address_space_destroy(&s->linked_dma_as);
    }
    if (s->hub_as_initialized) {
        address_space_destroy(&s->hub_as);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->widget_as); i++) {
        if (s->widget_as_initialized[i]) {
            address_space_destroy(&s->widget_as[i]);
        }
    }
}

static const Property ii_properties[] = {
    DEFINE_PROP_UINT8("local-region", SGIBedrockIOState, local_region, 0),
    DEFINE_PROP_UINT8("node-number", SGIBedrockIOState, node_number, 0),
    DEFINE_PROP_UINT8("revision", SGIBedrockIOState, revision, 1),
    /*
     * Default to the two widget numbers an XBridge presents to its host
     * port (0xe and 0xf: hw/pci-host/sgi-xbridge.c CHIP_HEAD_E/F, matching
     * every populated SN1 xbridge_widgets list to date).  Nothing in a
     * Bedrock+XBridge machine is ever wired to widget 8.
     */
    DEFINE_PROP_UINT16("inbound-widget-mask", SGIBedrockIOState,
                       inbound_widget_mask, (1U << 0xe) | (1U << 0xf)),
    DEFINE_PROP_UINT8("inbound-device", SGIBedrockIOState, inbound_device, 0),
    DEFINE_PROP_UINT64("device-mask-reset", SGIBedrockIOState,
                       device_mask_reset, UINT64_MAX),
    DEFINE_PROP_LINK("dma-memory", SGIBedrockIOState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("hub-registers", SGIBedrockIOState, hub_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-0", SGIBedrockIOState, widget_mr[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-1", SGIBedrockIOState, widget_mr[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-2", SGIBedrockIOState, widget_mr[2],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-3", SGIBedrockIOState, widget_mr[3],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-4", SGIBedrockIOState, widget_mr[4],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-5", SGIBedrockIOState, widget_mr[5],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-6", SGIBedrockIOState, widget_mr[6],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-7", SGIBedrockIOState, widget_mr[7],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-8", SGIBedrockIOState, widget_mr[8],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-9", SGIBedrockIOState, widget_mr[9],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-10", SGIBedrockIOState, widget_mr[10],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-11", SGIBedrockIOState, widget_mr[11],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-12", SGIBedrockIOState, widget_mr[12],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-13", SGIBedrockIOState, widget_mr[13],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-14", SGIBedrockIOState, widget_mr[14],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("widget-15", SGIBedrockIOState, widget_mr[15],
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void ii_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockIOState *s = SGI_BEDROCK_IO(dev);

    if (s->local_region >= 64) {
        error_setg(errp, "local-region must be below 64");
        return;
    }
    if (s->node_number >= 128) {
        error_setg(errp, "node-number must be below 128");
        return;
    }
    if (s->revision >= 16) {
        error_setg(errp, "revision must fit four bits");
        return;
    }
    if (s->inbound_device >= 8) {
        error_setg(errp, "inbound-device must be below 8");
        return;
    }
    if (s->dma_mr) {
        address_space_init(&s->linked_dma_as, s->dma_mr,
                           "sgi-bedrock-io.dma");
        s->linked_dma_as_initialized = true;
        s->dma_as = &s->linked_dma_as;
    } else if (!s->dma_as) {
        error_setg(errp, TYPE_SGI_BEDROCK_IO " 'dma-memory' link not set");
        return;
    }
    if (s->hub_mr) {
        address_space_init(&s->hub_as, s->hub_mr,
                           "sgi-bedrock-io.hub-registers");
        s->hub_as_initialized = true;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->widget_mr); i++) {
        if (s->widget_mr[i]) {
            g_autofree char *name =
                g_strdup_printf("sgi-bedrock-io.widget%u", i);

            address_space_init(&s->widget_as[i], s->widget_mr[i], name);
            s->widget_as_initialized[i] = true;
        }
    }
}

/*
 * ---- The crosstalk port interface ----
 *
 * Nothing in this model addresses the Bedrock as a widget from the
 * crosstalk side -- the host is the originator of widget-relative
 * traffic, never its destination here -- so cpu_face() has nothing to
 * answer with. packet_face() is the shared inbound window
 * (sgi_bedrock_io_inbound_rw()'s own decode): what an XBridge's
 * interrupt and register-modify packets, addressed by crosstalk offset
 * rather than by widget position, reach to land on this node.
 */

static const char *ii_xtalk_port_describe(Object *port, unsigned int index)
{
    return "the Bedrock's I/O interface";
}

static MemoryRegion *ii_xtalk_port_cpu_face(Object *port, unsigned int index)
{
    return NULL;
}

static MemoryRegion *ii_xtalk_port_packet_face(Object *port,
                                               unsigned int index)
{
    return &SGI_BEDROCK_IO(port)->inbound;
}

static void ii_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SGIXtalkPortClass *pc = SGI_XTALK_PORT_CLASS(klass);

    dc->desc = "SGI Bedrock I/O interface";
    dc->vmsd = &vmstate_ii;
    dc->user_creatable = true;
    dc->realize = ii_realize;
    rc->phases.enter = ii_reset_enter;
    device_class_set_props(dc, ii_properties);
    pc->describe = ii_xtalk_port_describe;
    pc->cpu_face = ii_xtalk_port_cpu_face;
    pc->packet_face = ii_xtalk_port_packet_face;
}

static const TypeInfo ii_info = {
    .name = TYPE_SGI_BEDROCK_IO, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockIOState), .instance_init = ii_init,
    .instance_finalize = ii_finalize, .class_init = ii_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XTALK_PORT },
        { }
    },
};

static void ii_register_types(void)
{
    type_register_static(&ii_info);
}
type_init(ii_register_types)
