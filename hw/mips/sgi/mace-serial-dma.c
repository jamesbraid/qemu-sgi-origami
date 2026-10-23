/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* MACE serial packet transport; UART state remains in the shared core. */
#include "qemu/osdep.h"
#include "mace-serial-dma.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/dma.h"

#define CHANNEL_RESET 0x400
#define CHANNEL_ENABLE 0x200
#define POINTER_MASK 0xfe0
#define CONTROL_MASK 0x6e0
#define RING_CAPACITY 0xfe0

static unsigned channel_depth(MACESerialChannel *c)
{
    return (c->write - c->read) & POINTER_MASK;
}

static bool channel_enabled(MACESerialChannel *c)
{
    return (c->control & (CHANNEL_RESET | CHANNEL_ENABLE)) == CHANNEL_ENABLE;
}

static bool channel_eligible(MACESerialChannel *c)
{
    MACESerialDMA *s = c->dma;

    return channel_enabled(c) && !c->fault && !(*s->ring_base & 1) &&
           !s->modem_pause[c->port];
}

static bool channel_threshold(MACESerialChannel *c)
{
    unsigned depth = channel_depth(c) / 32;
    unsigned selector = (c->control >> 5) & 7;

    if (!channel_enabled(c) || c->fault) {
        return false;
    }
    switch (selector) {
    case 0:
        return false;
    case 1:
    case 2:
    case 3:
        return c->receive ? depth >= selector * 32 : depth < selector * 32;
    case 4:
        return depth == 0;
    case 5:
        return depth != 0;
    case 6:
        return depth == 127;
    case 7:
        return depth != 127;
    default:
        g_assert_not_reached();
    }
}

static void serial_update_irq(MACESerialDMA *s)
{
    uint32_t status = 0;

    for (unsigned p = 0; p < 2; p++) {
        MACESerialChannel *tx = &s->channel[2 * p];
        MACESerialChannel *rx = &s->channel[2 * p + 1];
        unsigned shift = 20 + p * 6;

        status |= (uint32_t)channel_threshold(tx) << (shift + 1);
        status |= (uint32_t)tx->event << (shift + 2);
        status |= (uint32_t)tx->fault << (shift + 3);
        status |= (uint32_t)channel_threshold(rx) << (shift + 4);
        status |= (uint32_t)rx->event << (shift + 5);
    }
    s->update_irq(s->opaque, status);
}

static void channel_reset(MACESerialChannel *c)
{
    timer_del(c->timer);
    c->control = CHANNEL_RESET;
    c->read = c->write = 0;
    memset(c->data, 0, sizeof(c->data));
    c->count = c->pair = 0;
    c->staged = c->fault = c->event = c->delaying = false;
    c->delay_remaining = c->delay_deadline = 0;
}

uint64_t mace_serial_dma_read(MACESerialDMA *s, unsigned port, unsigned reg)
{
    MACESerialChannel *c = &s->channel[2 * port + (reg >= 4)];

    switch (reg & 3) {
    case 0:
        return c->control;
    case 1:
        return c->read;
    case 2:
        return c->write;
    case 3:
        return channel_depth(c);
    default:
        g_assert_not_reached();
    }
}

void mace_serial_dma_write(MACESerialDMA *s, unsigned port, unsigned reg,
                           uint64_t value)
{
    MACESerialChannel *c = &s->channel[2 * port + (reg >= 4)];
    unsigned pointer = value & POINTER_MASK;
    unsigned advance;

    switch (reg & 3) {
    case 0:
        if (value & CHANNEL_RESET) {
            channel_reset(c);
        } else {
            c->control = value & CONTROL_MASK;
        }
        break;
    case 1:
    case 2:
        if ((reg & 3) == (c->receive ? 2 : 1)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-mace: write to hardware serial DMA pointer\n");
            return;
        }
        advance = (pointer - (c->receive ? c->read : c->write)) & POINTER_MASK;
        if (advance > (c->receive ? channel_depth(c) :
                       RING_CAPACITY - channel_depth(c))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-mace: serial DMA pointer exceeds ring capacity\n");
            return;
        }
        if (c->receive) {
            c->read = pointer;
        } else {
            c->write = pointer;
        }
        break;
    case 3:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: write to serial DMA depth register\n");
        return;
    }
    mace_serial_dma_service(s);
}

bool mace_serial_dma_base_allowed(MACESerialDMA *s, uint32_t value)
{
    if (((value ^ *s->ring_base) & 0xffff8000U) == 0) {
        return true;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!(s->channel[i].control & CHANNEL_RESET)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-mace: ring base change with serial DMA out of reset\n");
            return false;
        }
    }
    return true;
}

void mace_serial_dma_ack(MACESerialDMA *s, uint32_t value)
{
    for (unsigned p = 0; p < 2; p++) {
        if (!(value & (1U << (22 + 6 * p)))) {
            s->channel[2 * p].event = false;
        }
    }
    serial_update_irq(s);
}

static hwaddr channel_address(MACESerialChannel *c)
{
    return (*c->dma->ring_base & 0xffff8000U) + 0x4000 +
           0x2000 * c->port + (c->receive ? 0x1000 : 0) +
           (c->receive ? c->write : c->read);
}

static void channel_fault(MACESerialChannel *c)
{
    c->fault = true;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-mace: port %u %s DMA memory transaction failed\n",
                  c->port + 1, c->receive ? "RX" : "TX");
}

static void serial_notify(void *opaque)
{
    MACESerialDMA *s = opaque;

    qemu_bh_schedule(s->bh);
}

static void delay_update(MACESerialChannel *c)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!c->delaying) {
        return;
    }
    if (c->delay_deadline) {
        c->delay_remaining = MAX(c->delay_deadline - now, 0);
    }
    if (!channel_eligible(c) || !c->delay_remaining) {
        timer_del(c->timer);
        c->delay_deadline = 0;
    } else if (!c->delay_deadline) {
        c->delay_deadline = now + c->delay_remaining;
        timer_mod(c->timer, c->delay_deadline);
    }
}

static void tx_service(MACESerialChannel *c)
{
    MACESerialDMA *s = c->dma;
    SerialState *uart = s->uart[c->port];

    delay_update(c);
    /* A callback cannot monopolize the main loop on a ring of skips. */
    for (unsigned work = 0; work < 128 && channel_eligible(c); work++) {
        unsigned index;
        uint8_t command, byte;

        if (!c->staged) {
            if (!channel_depth(c)) {
                return;
            }
            if (dma_memory_read(s->as, channel_address(c), c->data, 32,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                channel_fault(c);
                return;
            }
            c->staged = true;
            c->pair = 0;
        }
        index = (c->pair / 4) * 8 + c->pair % 4;
        command = c->data[index];
        byte = c->data[index + 4];
        switch (command >> 6) {
        case 0:
            break;
        case 1:
            if (!serial_dma_write(uart, byte)) {
                return;
            }
            break;
        case 2:
            serial_dma_modem_control(uart, byte);
            break;
        case 3:
            if (!c->delaying) {
                c->delaying = true;
                c->delay_remaining = byte * SCALE_MS;
                delay_update(c);
            }
            if (c->delay_remaining) {
                return;
            }
            c->delaying = false;
            break;
        }
        if (command & 0x20) {
            c->event = true;
        }
        c->pair++;
        if (c->pair == 16) {
            c->read = (c->read + 32) & POINTER_MASK;
            c->staged = false;
            c->pair = 0;
        }
        /* An accepted MCR command may itself change modem interrupt state. */
        if (serial_modem_irq_pending(uart)) {
            s->modem_pause[c->port] = true;
            delay_update(c);
            return;
        }
    }
    if (channel_eligible(c) && channel_depth(c)) {
        qemu_bh_schedule(s->bh);
    }
}

static bool rx_publish(MACESerialChannel *c)
{
    if (!c->count || channel_depth(c) == RING_CAPACITY) {
        return false;
    }
    if (dma_memory_write(c->dma->as, channel_address(c), c->data, 32,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        channel_fault(c);
        return false;
    }
    c->write = (c->write + 32) & POINTER_MASK;
    c->count = 0;
    c->staged = false;
    return true;
}

static void rx_append(MACESerialChannel *c, uint8_t byte, uint8_t status)
{
    unsigned index;

    if (!c->count) {
        for (unsigned i = 0; i < 4; i++) {
            memset(c->data + i * 8, 0, 4);
            memset(c->data + i * 8 + 4, 0xff, 4);
        }
        c->staged = true;
    }
    index = c->count / 4 * 8 + c->count % 4;
    c->data[index] = 0x80 | ((status >> 1) & 0xf);
    c->data[index + 4] = byte;
    c->count++;
}

static void rx_service(MACESerialChannel *c)
{
    SerialState *uart = c->dma->uart[c->port];
    bool overrun;

    if (!channel_eligible(c)) {
        return;
    }
    overrun = serial_dma_line_status(uart) & 2;
    if (c->count && channel_depth(c) != RING_CAPACITY) {
        if (!rx_publish(c)) {
            return;
        }
    }
    if (channel_depth(c) != RING_CAPACITY) {
        for (unsigned work = 0; work < 16 && c->count < 16; work++) {
            uint8_t byte, status;

            if (serial_modem_irq_pending(uart)) {
                c->dma->modem_pause[c->port] = true;
                break;
            }
            if (!serial_dma_read(uart, &byte, &status)) {
                break;
            }
            overrun |= !!(status & 2);
            rx_append(c, byte, status);
            if (overrun) {
                break;
            }
        }
        if (c->count && !c->dma->modem_pause[c->port]) {
            rx_publish(c);
        }
    }
    if (overrun) {
        c->fault = true;
        c->event = true;
    }
    if (channel_eligible(c) && channel_depth(c) != RING_CAPACITY &&
        (serial_dma_line_status(uart) & 1)) {
        qemu_bh_schedule(c->dma->bh);
    }
}

void mace_serial_dma_service(MACESerialDMA *s)
{
    if (s->servicing) {
        return;
    }
    s->servicing = true;
    for (unsigned p = 0; p < 2; p++) {
        s->modem_pause[p] = serial_modem_irq_pending(s->uart[p]);
        tx_service(&s->channel[2 * p]);
        rx_service(&s->channel[2 * p + 1]);
    }
    serial_update_irq(s);
    s->servicing = false;
}

static void serial_service_bh(void *opaque)
{
    mace_serial_dma_service(opaque);
}

static void delay_expired(void *opaque)
{
    MACESerialChannel *c = opaque;

    mace_serial_dma_service(c->dma);
}

void mace_serial_dma_init(MACESerialDMA *s, SerialState *uart,
                          AddressSpace *as, uint32_t *ring_base,
                          void (*update_irq)(void *, uint32_t), void *opaque)
{
    s->as = as;
    s->ring_base = ring_base;
    s->update_irq = update_irq;
    s->opaque = opaque;
    s->bh = qemu_bh_new(serial_service_bh, s);
    for (unsigned p = 0; p < 2; p++) {
        s->uart[p] = &uart[p];
        serial_set_service_notify(s->uart[p], serial_notify, s);
    }
    for (unsigned i = 0; i < 4; i++) {
        MACESerialChannel *c = &s->channel[i];

        c->dma = s;
        c->port = i / 2;
        c->receive = i & 1;
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, delay_expired, c);
        channel_reset(c);
    }
}

void mace_serial_dma_reset(MACESerialDMA *s)
{
    qemu_bh_cancel(s->bh);
    for (unsigned i = 0; i < 4; i++) {
        channel_reset(&s->channel[i]);
    }
    s->modem_pause[0] = s->modem_pause[1] = false;
    serial_update_irq(s);
}

void mace_serial_dma_cleanup(MACESerialDMA *s)
{
    for (unsigned p = 0; p < 2; p++) {
        serial_set_service_notify(s->uart[p], NULL, NULL);
    }
    qemu_bh_delete(s->bh);
    for (unsigned i = 0; i < 4; i++) {
        timer_free(s->channel[i].timer);
    }
}

int mace_serial_dma_pre_save(MACESerialDMA *s)
{
    for (unsigned i = 0; i < 4; i++) {
        delay_update(&s->channel[i]);
    }
    return 0;
}

int mace_serial_dma_post_load(MACESerialDMA *s)
{
    for (unsigned i = 0; i < 4; i++) {
        MACESerialChannel *c = &s->channel[i];

        if ((c->read & ~POINTER_MASK) || (c->write & ~POINTER_MASK) ||
            (c->control & ~CONTROL_MASK) || c->count > 16 || c->pair >= 16 ||
            c->delay_remaining < 0 || c->delay_remaining > 255 * SCALE_MS ||
            (c->receive && (c->delaying || c->pair ||
                            (c->staged && !c->count))) ||
            (!c->receive && c->count) ||
            (!c->staged && (c->count || c->pair || c->delaying)) ||
            (c->delaying && c->data[c->pair / 4 * 8 + c->pair % 4] >> 6 != 3)) {
            return -EINVAL;
        }
        timer_del(c->timer);
        c->delay_deadline = 0;
    }
    /* Parent post-load runs after the embedded UART state has loaded. */
    for (unsigned p = 0; p < 2; p++) {
        s->modem_pause[p] = serial_modem_irq_pending(s->uart[p]);
    }
    serial_update_irq(s);
    qemu_bh_schedule(s->bh);
    return 0;
}

static const VMStateDescription vmstate_mace_serial_channel = {
    .name = "sgi-mace/serial-channel",
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(control, MACESerialChannel),
        VMSTATE_UINT16(read, MACESerialChannel),
        VMSTATE_UINT16(write, MACESerialChannel),
        VMSTATE_UINT8_ARRAY(data, MACESerialChannel, 32),
        VMSTATE_UINT8(count, MACESerialChannel),
        VMSTATE_UINT8(pair, MACESerialChannel),
        VMSTATE_BOOL(staged, MACESerialChannel),
        VMSTATE_BOOL(fault, MACESerialChannel),
        VMSTATE_BOOL(event, MACESerialChannel),
        VMSTATE_INT64(delay_remaining, MACESerialChannel),
        VMSTATE_BOOL(delaying, MACESerialChannel),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_mace_serial_dma = {
    .name = "sgi-mace/serial-dma",
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channel, MACESerialDMA, 4, 0,
                             vmstate_mace_serial_channel, MACESerialChannel),
        VMSTATE_END_OF_LIST()
    },
};
