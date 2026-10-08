/*
 * SGI IOC4 revision 0x53, serial-console subset.
 *
 * The four byte-wide 16550A ports and their local interrupt controller are
 * modeled. ATA, serial DMA, keyboard/mouse, GPIO and byte-bus devices are not.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "qapi/error.h"
#include "hw/char/serial.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/dma.h"

#define TYPE_SGI_IOC4 "sgi-ioc4"
#define IOC4_PORTS 4
#define IOC4_BAR_SIZE 0x100000
#define IOC4_UART_BASE 0x380
#define IOC4_CLOCK_BAR 0x87ff8
#define IOC4_CLOCK_NATIVE 0x7f8
#define IOC4_CLOCK_SIZE 8
#define IOC4_DMA_PORT_BASE(n) (0x310 + 0x1c * (n))
#define IOC4_DMA_CONTROL(n) (IOC4_DMA_PORT_BASE(n) + 0x00)
#define IOC4_DMA_TX_PRODUCE(n) (IOC4_DMA_PORT_BASE(n) + 0x04)
#define IOC4_DMA_TX_CONSUME(n) (IOC4_DMA_PORT_BASE(n) + 0x08)
#define IOC4_DMA_RX_PRODUCE(n) (IOC4_DMA_PORT_BASE(n) + 0x0c)
#define IOC4_DMA_RX_CONSUME(n) (IOC4_DMA_PORT_BASE(n) + 0x10)
#define IOC4_DMA_RX_TIMER(n) (IOC4_DMA_PORT_BASE(n) + 0x14)
#define IOC4_DMA_SHADOW(n) (IOC4_DMA_PORT_BASE(n) + 0x18)
#define IOC4_SSCR_THRESHOLD 0x000001ff
#define IOC4_SSCR_DRAIN     0x08000000
#define IOC4_SSCR_DMA_EN    0x10000000
#define IOC4_SSCR_PAUSE     0x20000000
#define IOC4_SSCR_PAUSE_STATE 0x40000000
#define IOC4_SSCR_RESET     0x80000000
#define IOC4_SRCIR_TIMER_ARM 0x80000000
#define IOC4_RING_1K 0x400
#define IOC4_RING_4K 0x1000
#define IOC4_RX_VALID 0x80
#define IOC4_RX_MODEM_VALID 0x40
#define IOC4_TX_VALID 0x40
#define IOC4_TX_EXPLICIT 0x20
#define IOC4_TX_MCR 0x80
#define IOC4_TX_DELAY 0xc0
#define IOC4_DMA_SHADOW_OUTPUTS 0x1f000000
#define IOC4_DMA_SHADOW_LOOP 0x10000000
#define IOC4_DMA_OTHER_SHIFT 2
#define IOC4_DMA_ERROR_VALID 1
#define IOC4_DMA_ERROR_MULTIPLE 0x20
#define IOC4_DMA_ERROR_REQUESTER(n) ((n) << 1)

typedef struct SGIIOC4State SGIIOC4State;
typedef struct SGIIOC4Port SGIIOC4Port;

struct SGIIOC4Port {
    SGIIOC4State *ioc4;
    unsigned index;
    uint32_t control;
    uint32_t tx_produce;
    uint32_t tx_consume;
    uint32_t rx_produce;
    uint32_t rx_consume;
    uint32_t rx_timer_reload;
    uint32_t shadow;
    uint8_t rx_data[4];
    uint8_t rx_status[4];
    uint8_t rx_count;
    uint8_t tx_data[4];
    uint8_t tx_status[4];
    uint8_t tx_slot;
    bool rx_staged;
    bool tx_waiting_rx;
    bool tx_active;
    bool timer_armed;
    bool rx_timer_running;
    bool tx_timer_running;
    bool tx_completion_pending;
    bool halted;
    bool drain_pending;
    int64_t rx_remaining_ns;
    int64_t tx_remaining_ns;
    QEMUTimer *rx_timer;
    QEMUTimer *tx_timer;
    guint tx_watch;
    IOEventHandler *saved_event;
    IOCanReadHandler *saved_can_read;
    IOReadHandler *saved_read;
    BackendChangeHandler *saved_be_change;
    void *saved_opaque;
};

struct SGIIOC4State {
    PCIDevice parent_obj;
    MemoryRegion bar;
    MemoryRegion clock_alias;
    MemoryRegion *clock;
    bool clock_alias_initialized;
    bool clock_alias_mapped;
    SerialState uart[IOC4_PORTS];
    SGIIOC4Port dma[IOC4_PORTS];
    uint64_t dma_base[2];
    uint32_t dma_ring_size[2];
    uint32_t other_pending;
    uint32_t dma_error_low;
    uint32_t dma_error_high;
    unsigned dma_request_depth;
    uint8_t dma_migration_mode;
    bool serial_dma_experimental;
    IRQState uart_irq[IOC4_PORTS];
    uint32_t serial_pending;
    uint32_t serial_enable;
    uint32_t other_enable;
    uint32_t uart_level;
    uint32_t clock_hz;
};

typedef struct SGIIOC4Class {
    PCIDeviceClass parent_class;
    ResettablePhases parent_phases;
} SGIIOC4Class;

static unsigned ioc4_pair(unsigned port)
{
    return port / 2;
}

static uint32_t ioc4_ring_size(SGIIOC4State *s, unsigned port)
{
    return s->dma_ring_size[ioc4_pair(port)];
}

static uint32_t ioc4_ring_mask(SGIIOC4State *s, unsigned port)
{
    return ioc4_ring_size(s, port) - 8;
}

static bool ioc4_port_paused(SGIIOC4Port *p)
{
    return p->control & IOC4_SSCR_PAUSE;
}

static bool ioc4_rx_ring_full(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    uint32_t next = (p->rx_produce + 8) & ioc4_ring_mask(s, p->index);

    return next == p->rx_consume;
}

static bool ioc4_tx_ring_queued(SGIIOC4Port *p)
{
    return p->tx_produce != p->tx_consume || p->tx_active;
}

static bool ioc4_dma_loopback(SGIIOC4Port *p)
{
    return p->shadow & IOC4_DMA_SHADOW_LOOP;
}

static void ioc4_shadow_set_outputs(SGIIOC4Port *p, uint32_t outputs)
{
    bool was_loopback = ioc4_dma_loopback(p);

    p->shadow = (p->shadow & ~IOC4_DMA_SHADOW_OUTPUTS) |
                (outputs & IOC4_DMA_SHADOW_OUTPUTS);
    if (was_loopback && !ioc4_dma_loopback(p)) {
        qemu_chr_fe_accept_input(&p->ioc4->uart[p->index].chr);
    }
}

static void ioc4_update_irq(SGIIOC4State *s)
{
    pci_set_irq(&s->parent_obj,
                !!((s->serial_pending & s->serial_enable) |
                   (s->uart_level & s->serial_enable) |
                   (s->other_pending & s->other_enable)));
}

static void ioc4_uart_irq(void *opaque, int n, int level)
{
    SGIIOC4State *s = opaque;
    uint32_t bit = 0x40u << (8 * n);

    if (level) {
        s->uart_level |= bit;
        s->serial_pending |= bit;
    } else {
        s->uart_level &= ~bit;
    }
    ioc4_update_irq(s);
}

static uint64_t ioc4_tx_ring_address(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    uint32_t ring_size = ioc4_ring_size(s, p->index);
    uint64_t offset = (p->index & 1 ? 2 : 0) * ring_size;

    return s->dma_base[ioc4_pair(p->index)] + offset + p->tx_consume;
}

static uint64_t ioc4_rx_ring_address(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    uint32_t ring_size = ioc4_ring_size(s, p->index);
    uint64_t offset = (p->index & 1 ? 3 : 1) * ring_size;

    return s->dma_base[ioc4_pair(p->index)] + offset + p->rx_produce;
}

static void ioc4_dma_error(SGIIOC4Port *p, uint64_t address, bool tx)
{
    SGIIOC4State *s = p->ioc4;
    unsigned requester = 2 * p->index + !tx;

    p->halted = true;
    if (!(s->dma_error_low & IOC4_DMA_ERROR_VALID)) {
        s->dma_error_low = (address & UINT32_C(0xffffffc0)) |
                           IOC4_DMA_ERROR_REQUESTER(requester) |
                           IOC4_DMA_ERROR_VALID;
        s->dma_error_high = address >> 32;
    } else {
        s->dma_error_low |= IOC4_DMA_ERROR_MULTIPLE;
    }
    s->other_pending |= 1u << (p->index + IOC4_DMA_OTHER_SHIFT);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-ioc4: serial DMA %s failed for port %u at 0x%"
                  PRIx64 "\n", tx ? "read" : "write", p->index, address);
    ioc4_update_irq(s);
}

static void ioc4_update_rx_threshold(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    unsigned threshold = p->control & IOC4_SSCR_THRESHOLD;
    unsigned entries = ((p->rx_produce - p->rx_consume) &
                        ioc4_ring_mask(s, p->index)) / 8;

    if (threshold && entries >= threshold) {
        s->serial_pending |= 1u << (8 * p->index + 2);
    }
    ioc4_update_irq(s);
}

static bool ioc4_rx_publish(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    uint8_t entry[8] = { 0 };
    uint64_t address;
    MemTxResult result;
    unsigned i;

    if (!p->rx_count || p->halted || ioc4_port_paused(p) ||
        !(p->control & IOC4_SSCR_DMA_EN)) {
        return false;
    }
    if (ioc4_rx_ring_full(p)) {
        s->serial_pending |= 1u << (8 * p->index + 1);
        ioc4_update_irq(s);
        return false;
    }
    for (i = 0; i < p->rx_count; i++) {
        entry[i] = p->rx_data[i];
        entry[4 + i] = p->rx_status[i];
    }
    address = ioc4_rx_ring_address(p);
    s->dma_request_depth++;
    result = pci_dma_write(&s->parent_obj, address, entry, sizeof(entry));
    s->dma_request_depth--;
    if (result != MEMTX_OK) {
        ioc4_dma_error(p, address, false);
        return false;
    }
    p->rx_produce = (p->rx_produce + 8) & ioc4_ring_mask(s, p->index);
    p->rx_count = 0;
    p->rx_staged = false;
    p->tx_waiting_rx = false;
    p->drain_pending = false;
    p->control &= ~IOC4_SSCR_DRAIN;
    ioc4_update_rx_threshold(p);
    qemu_chr_fe_accept_input(&s->uart[p->index].chr);
    return true;
}

static void ioc4_rx_stage_byte(SGIIOC4Port *p, uint8_t value,
                               bool loopback)
{
    p->rx_data[p->rx_count] = value;
    p->rx_status[p->rx_count] = IOC4_RX_VALID |
        (loopback ? IOC4_RX_MODEM_VALID : 0);
    p->rx_count++;
    if (p->timer_armed && !p->rx_remaining_ns &&
        (p->rx_timer_reload & 0xfff)) {
        p->rx_remaining_ns = (int64_t)(p->rx_timer_reload & 0xfff) * 62500;
        if (p->rx_remaining_ns) {
            p->rx_timer_running = true;
            timer_mod(p->rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      p->rx_remaining_ns);
        }
    }
    if (p->rx_count == 4) {
        p->rx_staged = !ioc4_rx_publish(p);
    }
}

static gboolean ioc4_tx_watch(void *opaque, GIOCondition condition,
                              void *data);

static void ioc4_tx_run(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    uint32_t mask = ioc4_ring_mask(s, p->index);
    bool was_queued = ioc4_tx_ring_queued(p);

    if (!s->serial_dma_experimental || p->halted || p->tx_waiting_rx ||
        ioc4_port_paused(p) ||
        !(p->control & IOC4_SSCR_DMA_EN) || p->tx_timer_running) {
        return;
    }
    while (p->tx_consume != p->tx_produce) {
        if (!p->tx_active) {
            uint64_t address = ioc4_tx_ring_address(p);
            uint8_t entry[8];
            MemTxResult result;

            s->dma_request_depth++;
            result = pci_dma_read(&s->parent_obj, address, entry,
                                 sizeof(entry));
            s->dma_request_depth--;
            if (result != MEMTX_OK) {
                ioc4_dma_error(p, address, true);
                return;
            }
            memcpy(p->tx_data, entry, 4);
            memcpy(p->tx_status, entry + 4, 4);
            p->tx_slot = 0;
            p->tx_active = true;
        }
        while (p->tx_slot < 4) {
            uint8_t status = p->tx_status[p->tx_slot];
            uint8_t kind = status & 0xc0;

            if (!status) {
                p->tx_slot++;
                continue;
            }
            if (status != 0x40 && status != 0x60 && status != 0x80 &&
                status != 0xa0 && status != 0xc0 && status != 0xe0) {
                p->halted = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-ioc4: unsupported TX status 0x%02x on port %u\n",
                              status, p->index);
                return;
            }
            if (kind == IOC4_TX_VALID) {
                if (ioc4_dma_loopback(p)) {
                    ioc4_rx_stage_byte(p, p->tx_data[p->tx_slot], true);
                    p->tx_waiting_rx = p->rx_staged;
                } else {
                    int sent = qemu_chr_fe_write(&s->uart[p->index].chr,
                                                 &p->tx_data[p->tx_slot], 1);
                    if (sent == 0) {
                        if (!p->tx_watch) {
                            p->tx_watch = qemu_chr_fe_add_watch(
                                &s->uart[p->index].chr, G_IO_OUT | G_IO_HUP,
                                ioc4_tx_watch, p);
                        }
                        return;
                    }
                    if (sent < 0) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "sgi-ioc4: serial backend rejected TX on port %u\n",
                                      p->index);
                        return;
                    }
                }
            } else if (kind == IOC4_TX_MCR) {
                ioc4_shadow_set_outputs(p,
                    (uint32_t)(p->tx_data[p->tx_slot] & 0x1f) << 24);
            } else if (kind == IOC4_TX_DELAY) {
                p->tx_remaining_ns = (int64_t)p->tx_data[p->tx_slot] *
                                     1000000;
                if (p->tx_remaining_ns) {
                    p->tx_timer_running = true;
                    timer_mod(p->tx_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              p->tx_remaining_ns);
                }
            } else {
                p->halted = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-ioc4: unsupported TX status 0x%02x on port %u\n",
                              status, p->index);
                return;
            }
            if (status & IOC4_TX_EXPLICIT) {
                if (kind == IOC4_TX_DELAY && p->tx_remaining_ns) {
                    p->tx_completion_pending = true;
                } else {
                    s->serial_pending |= 1u << (8 * p->index + 7);
                    ioc4_update_irq(s);
                }
            }
            p->tx_slot++;
            if (p->tx_remaining_ns || p->tx_waiting_rx) {
                return;
            }
        }
        p->tx_active = false;
        p->tx_slot = 0;
        p->tx_consume = (p->tx_consume + 8) & mask;
    }
    if (was_queued && !ioc4_tx_ring_queued(p)) {
        s->serial_pending |= 1u << (8 * p->index);
        ioc4_update_irq(s);
    }
}

static gboolean ioc4_tx_watch(void *opaque, GIOCondition condition,
                              void *data)
{
    SGIIOC4Port *p = data;

    p->tx_watch = 0;
    if (condition & G_IO_OUT) {
        ioc4_tx_run(p);
    }
    return G_SOURCE_REMOVE;
}

static void ioc4_rx_timeout(void *opaque)
{
    SGIIOC4Port *p = opaque;

    p->timer_armed = false;
    p->rx_timer_running = false;
    p->rx_remaining_ns = 0;
    if (p->rx_count) {
        p->rx_staged = !ioc4_rx_publish(p);
    }
    p->ioc4->serial_pending |= 1u << (8 * p->index + 3);
    ioc4_update_irq(p->ioc4);
}

static void ioc4_tx_timeout(void *opaque)
{
    SGIIOC4Port *p = opaque;

    p->tx_remaining_ns = 0;
    p->tx_timer_running = false;
    if (p->tx_completion_pending) {
        p->tx_completion_pending = false;
        p->ioc4->serial_pending |= 1u << (8 * p->index + 7);
        ioc4_update_irq(p->ioc4);
    }
    ioc4_tx_run(p);
}

static int ioc4_char_can_read(void *opaque)
{
    SGIIOC4Port *p = opaque;
    SGIIOC4State *s = p->ioc4;

    if (!s->serial_dma_experimental || !(p->control & IOC4_SSCR_DMA_EN)) {
        return p->saved_can_read ? p->saved_can_read(p->saved_opaque) : 0;
    }
    if (ioc4_port_paused(p) || p->halted || p->rx_staged ||
        ioc4_dma_loopback(p)) {
        return 0;
    }
    return 4 - p->rx_count;
}

static void ioc4_char_read(void *opaque, const uint8_t *buf, int size)
{
    SGIIOC4Port *p = opaque;
    SGIIOC4State *s = p->ioc4;
    int i;

    if (!s->serial_dma_experimental || !(p->control & IOC4_SSCR_DMA_EN)) {
        if (p->saved_read) {
            p->saved_read(p->saved_opaque, buf, size);
        }
        return;
    }
    if (s->uart[p->index].wakeup) {
        qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);
    }
    for (i = 0; i < size && p->rx_count < 4; i++) {
        ioc4_rx_stage_byte(p, buf[i], false);
    }
}

static void ioc4_char_event(void *opaque, QEMUChrEvent event)
{
    SGIIOC4Port *p = opaque;

    if (p->saved_event) {
        p->saved_event(p->saved_opaque, event);
    }
}

static int ioc4_char_backend_change(void *opaque);

static void ioc4_install_char_handlers(SGIIOC4Port *p)
{
    CharFrontend *chr = &p->ioc4->uart[p->index].chr;

    p->saved_event = chr->chr_event;
    p->saved_can_read = chr->chr_can_read;
    p->saved_read = chr->chr_read;
    p->saved_be_change = chr->chr_be_change;
    p->saved_opaque = chr->opaque;
    qemu_chr_fe_set_handlers(chr, ioc4_char_can_read, ioc4_char_read,
                             ioc4_char_event, ioc4_char_backend_change,
                             p, NULL, true);
}

static int ioc4_char_backend_change(void *opaque)
{
    SGIIOC4Port *p = opaque;

    if (p->tx_watch) {
        g_source_remove(p->tx_watch);
        p->tx_watch = 0;
    }
    if (p->saved_be_change && p->saved_be_change(p->saved_opaque) < 0) {
        return -1;
    }
    ioc4_install_char_handlers(p);
    ioc4_tx_run(p);
    return 0;
}

static bool ioc4_pair_quiescent(SGIIOC4State *s, unsigned pair)
{
    unsigned i;

    for (i = pair * 2; i < pair * 2 + 2; i++) {
        SGIIOC4Port *p = &s->dma[i];

        if ((p->control & (IOC4_SSCR_DMA_EN | IOC4_SSCR_PAUSE)) ||
            p->tx_produce || p->tx_consume || p->rx_produce || p->rx_consume ||
            p->tx_active || p->rx_count || p->timer_armed ||
            p->drain_pending) {
            return false;
        }
    }
    return true;
}

static bool ioc4_port_quiescent(SGIIOC4Port *p)
{
    return !ioc4_tx_ring_queued(p) && p->rx_produce == p->rx_consume &&
           !p->rx_count && !p->timer_armed && !p->drain_pending;
}

static void ioc4_port_reset(SGIIOC4Port *p)
{
    SGIIOC4State *s = p->ioc4;
    unsigned error_bit = p->index + IOC4_DMA_OTHER_SHIFT;

    if (p->rx_timer) {
        timer_del(p->rx_timer);
    }
    if (p->tx_timer) {
        timer_del(p->tx_timer);
    }
    if (p->tx_watch) {
        g_source_remove(p->tx_watch);
        p->tx_watch = 0;
    }
    p->control = 0;
    p->tx_produce = p->tx_consume = 0;
    p->rx_produce = p->rx_consume = 0;
    p->rx_timer_reload = 0;
    p->shadow &= ~IOC4_DMA_SHADOW_OUTPUTS;
    memset(p->rx_data, 0, sizeof(p->rx_data));
    memset(p->rx_status, 0, sizeof(p->rx_status));
    memset(p->tx_data, 0, sizeof(p->tx_data));
    memset(p->tx_status, 0, sizeof(p->tx_status));
    p->rx_count = p->tx_slot = 0;
    p->rx_staged = p->tx_waiting_rx = p->tx_active = false;
    p->timer_armed = p->rx_timer_running = p->tx_timer_running = false;
    p->tx_completion_pending = false;
    p->halted = p->drain_pending = false;
    p->rx_remaining_ns = p->tx_remaining_ns = 0;
    s->serial_pending &= ~((uint32_t)0xff << (8 * p->index));
    s->other_pending &= ~(1u << error_bit);
    qemu_chr_fe_accept_input(&s->uart[p->index].chr);
    ioc4_update_irq(s);
}

static void ioc4_control_write(SGIIOC4Port *p, uint32_t value)
{
    SGIIOC4State *s = p->ioc4;
    uint32_t supported = IOC4_SSCR_THRESHOLD | IOC4_SSCR_DRAIN |
                         IOC4_SSCR_DMA_EN | IOC4_SSCR_PAUSE |
                         IOC4_SSCR_RESET;
    uint32_t old = p->control;
    bool want_dma = value & IOC4_SSCR_DMA_EN;
    bool want_pause = value & IOC4_SSCR_PAUSE;

    if (value & IOC4_SSCR_RESET) {
        ioc4_port_reset(p);
        return;
    }
    if (value & ~supported) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: unsupported serial control bits 0x%08x on port %u\n",
                      value & ~supported, p->index);
    }
    if (!!(old & IOC4_SSCR_DMA_EN) != want_dma) {
        SerialState *uart = &s->uart[p->index];

        if (want_dma) {
            if (!fifo8_is_empty(&uart->recv_fifo) ||
                !fifo8_is_empty(&uart->xmit_fifo) ||
                (uart->lsr & 1) || (uart->lsr & 0x60) != 0x60) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-ioc4: DMA enable rejected with pending PIO data on port %u\n",
                              p->index);
                return;
            }
        } else if (!ioc4_port_quiescent(p)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: DMA disable rejected with pending work on port %u\n",
                          p->index);
            return;
        }
    }
    if (!(old & IOC4_SSCR_PAUSE) && want_pause) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (p->rx_timer_running) {
            if (timer_pending(p->rx_timer)) {
                p->rx_remaining_ns = MAX(0,
                    timer_expire_time_ns(p->rx_timer) - now);
            }
            timer_del(p->rx_timer);
        }
        if (p->tx_timer_running) {
            if (timer_pending(p->tx_timer)) {
                p->tx_remaining_ns = MAX(0,
                    timer_expire_time_ns(p->tx_timer) - now);
            }
            timer_del(p->tx_timer);
        }
    }
    p->control = value & (IOC4_SSCR_THRESHOLD | IOC4_SSCR_DMA_EN |
                          IOC4_SSCR_PAUSE);
    if (p->drain_pending) {
        p->control |= IOC4_SSCR_DRAIN;
    }
    if (value & IOC4_SSCR_DRAIN) {
        p->drain_pending = true;
        p->control |= IOC4_SSCR_DRAIN;
        if (!p->rx_count && (p->control & IOC4_SSCR_DMA_EN) && !want_pause) {
            p->drain_pending = false;
            p->control &= ~IOC4_SSCR_DRAIN;
        }
    }
    if (old & IOC4_SSCR_PAUSE && !want_pause) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (p->rx_timer_running) {
            timer_mod(p->rx_timer, now + p->rx_remaining_ns);
        }
        if (p->tx_timer_running) {
            timer_mod(p->tx_timer, now + p->tx_remaining_ns);
        }
    }
    if (p->control & IOC4_SSCR_DMA_EN) {
        if (p->drain_pending && !want_pause) {
            if (p->rx_count) {
                ioc4_rx_publish(p);
            } else {
                p->drain_pending = false;
                p->control &= ~IOC4_SSCR_DRAIN;
            }
        }
        if (!want_pause) {
            ioc4_tx_run(p);
            qemu_chr_fe_accept_input(&s->uart[p->index].chr);
        }
    }
    ioc4_update_rx_threshold(p);
}

static uint64_t ioc4_dma_register_read(SGIIOC4State *s, hwaddr addr)
{
    unsigned n;

    if (addr >= 0x300 && addr <= 0x30c) {
        unsigned pair = (addr - 0x300) / 8;
        bool high = (addr & 4) != 0;

        if (high) {
            return s->dma_base[pair] >> 32;
        }
        return (uint32_t)s->dma_base[pair] |
               (s->dma_ring_size[pair] == IOC4_RING_4K);
    }
    if (addr < IOC4_DMA_PORT_BASE(0) ||
        addr > IOC4_DMA_SHADOW(IOC4_PORTS - 1)) {
        return UINT64_MAX;
    }
    n = (addr - IOC4_DMA_PORT_BASE(0)) / 0x1c;
    if (n >= IOC4_PORTS || addr != IOC4_DMA_PORT_BASE(n) +
        ((addr - IOC4_DMA_PORT_BASE(n)) / 4) * 4) {
        return UINT64_MAX;
    }
    switch ((addr - IOC4_DMA_PORT_BASE(n)) / 4) {
    case 0:
        return s->dma[n].control |
               (ioc4_port_paused(&s->dma[n]) ? IOC4_SSCR_PAUSE_STATE : 0);
    case 1: return s->dma[n].tx_produce;
    case 2: return s->dma[n].tx_consume;
    case 3: return s->dma[n].rx_produce;
    case 4: return s->dma[n].rx_consume;
    case 5: return s->dma[n].rx_timer_reload & 0xfff;
    case 6: return s->dma[n].shadow;
    default: return UINT64_MAX;
    }
}

static void ioc4_dma_register_write(SGIIOC4State *s, hwaddr addr,
                                    uint32_t value)
{
    unsigned n;
    unsigned pair;
    SGIIOC4Port *p;
    uint32_t mask;

    if (addr >= 0x300 && addr <= 0x30c) {
        pair = (addr - 0x300) / 8;
        if (!ioc4_pair_quiescent(s, pair)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: DMA base change rejected while pair %u is active\n",
                          pair);
            return;
        }
        if (addr & 4) {
            s->dma_base[pair] = (s->dma_base[pair] & UINT32_MAX) |
                                ((uint64_t)value << 32);
        } else {
            s->dma_ring_size[pair] = value & 1 ? IOC4_RING_4K : IOC4_RING_1K;
            s->dma_base[pair] =
                (s->dma_base[pair] & UINT64_C(0xffffffff00000000)) |
                (value & UINT32_C(0xfffffffe));
        }
        return;
    }
    if (addr < IOC4_DMA_PORT_BASE(0) ||
        addr > IOC4_DMA_SHADOW(IOC4_PORTS - 1)) {
        return;
    }
    n = (addr - IOC4_DMA_PORT_BASE(0)) / 0x1c;
    if (n >= IOC4_PORTS) {
        return;
    }
    p = &s->dma[n];
    mask = ioc4_ring_mask(s, n);
    switch ((addr - IOC4_DMA_PORT_BASE(n)) / 4) {
    case 0: {
        bool enabling = !(p->control & IOC4_SSCR_DMA_EN) &&
                        (value & IOC4_SSCR_DMA_EN);
        uint64_t base = s->dma_base[ioc4_pair(n)];
        unsigned size = ioc4_ring_size(s, n);

        if (s->serial_dma_experimental && enabling &&
            (base & (size * 4 - 1))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: misaligned DMA pair base 0x%" PRIx64
                          " on port %u\n", base, n);
            p->halted = true;
            return;
        }
        ioc4_control_write(p, value);
        break;
    }
    case 1:
        if ((value & 7) || value > mask) {
            p->halted = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: invalid TX producer 0x%08x on port %u\n",
                          value, n);
            break;
        }
        p->tx_produce = value;
        ioc4_tx_run(p);
        break;
    case 2:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: guest write to device-owned TX consumer on port %u\n",
                      n);
        break;
    case 3:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: guest write to device-owned RX producer on port %u\n",
                      n);
        break;
    case 4: {
        uint32_t pointer = value & ~IOC4_SRCIR_TIMER_ARM;

        if ((pointer & 7) || pointer > mask) {
            p->halted = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: invalid RX consumer 0x%08x on port %u\n",
                          value, n);
            break;
        }
        p->rx_consume = pointer;
        timer_del(p->rx_timer);
        p->timer_armed = value & IOC4_SRCIR_TIMER_ARM;
        p->rx_timer_running = false;
        p->rx_remaining_ns = 0;
        if (p->rx_staged || p->drain_pending) {
            ioc4_rx_publish(p);
        }
        if (!p->rx_staged) {
            ioc4_tx_run(p);
        }
        qemu_chr_fe_accept_input(&s->uart[n].chr);
        break;
    }
    case 5:
        if (value & ~0xfff) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc4: unsupported RX timer bits 0x%08x on port %u\n",
                          value & ~0xfff, n);
        }
        p->rx_timer_reload = value & 0xfff;
        break;
    case 6:
        if (value & ~IOC4_DMA_SHADOW_OUTPUTS) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc4: unsupported shadow bits 0x%08x on port %u\n",
                          value & ~IOC4_DMA_SHADOW_OUTPUTS, n);
        }
        ioc4_shadow_set_outputs(p, value);
        break;
    default:
        break;
    }
}

static uint64_t ioc4_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC4State *s = opaque;
    uint64_t dma_value;

    if (addr <= 0x04) {
        return addr == 0 ? s->dma_error_low : s->dma_error_high;
    }
    dma_value = ioc4_dma_register_read(s, addr);
    if (dma_value != UINT64_MAX) {
        return dma_value;
    }
    if (addr == 0x20 && s->serial_dma_experimental) {
        return s->dma_request_depth ? 0 : 0x80;
    }
    switch (addr) {
    case 0x08:
        return s->serial_pending | s->uart_level;
    case 0x0c:
        return s->other_pending;
    case 0x10:
        return s->serial_enable;
    case 0x14:
        return s->other_enable;
    case 0x24:
        return s->clock_hz == 66666667;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: read of unsupported BAR offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void ioc4_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    SGIIOC4State *s = opaque;
    uint64_t dma_value = ioc4_dma_register_read(s, addr);

    if (addr <= 0x04) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to read-only DMA error address at 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    if (dma_value != UINT64_MAX) {
        ioc4_dma_register_write(s, addr, val);
        return;
    }
    if (addr == 0x20 && s->serial_dma_experimental) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to read-only serial status at 0x20\n");
        return;
    }
    switch (addr) {
    case 0x08:
        s->serial_pending &= ~(uint32_t)val;
        for (int i = 0; i < IOC4_PORTS; i++) {
            unsigned threshold = s->dma[i].control & IOC4_SSCR_THRESHOLD;
            unsigned count = ((s->dma[i].rx_produce - s->dma[i].rx_consume) &
                              ioc4_ring_mask(s, i)) / 8;
            uint32_t bit = 1u << (8 * i + 2);

            if (threshold && count >= threshold) {
                s->serial_pending |= bit;
            }
        }
        break;
    case 0x0c:
        s->other_pending &= ~(uint32_t)val;
        break;
    case 0x10:
        s->serial_enable |= (uint32_t)val;
        break;
    case 0x14:
        s->other_enable |= (uint32_t)val;
        break;
    case 0x18:
        s->serial_enable &= ~(uint32_t)val;
        break;
    case 0x1c:
        s->other_enable &= ~(uint32_t)val;
        break;
    case 0x24:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: unsupported misc control write 0x%"
                      PRIx64 "\n", val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to unsupported BAR offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    ioc4_update_irq(s);
}

static const MemoryRegionOps ioc4_ops = {
    .read = ioc4_read,
    .write = ioc4_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4,
               .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool ioc4_uart_accepts(void *opaque, hwaddr addr, unsigned size,
                              bool is_write, MemTxAttrs attrs)
{
    if (size != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: unsupported UART %s width %u at 0x%"
                      HWADDR_PRIx "\n", is_write ? "write" : "read",
                      size, addr);
        return false;
    }
    return true;
}

static uint64_t ioc4_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC4Port *p = opaque;

    return serial_io_ops.read(&p->ioc4->uart[p->index], addr, size);
}

static void ioc4_uart_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    SGIIOC4Port *p = opaque;
    SerialState *uart = &p->ioc4->uart[p->index];

    if (p->ioc4->serial_dma_experimental &&
        (p->control & IOC4_SSCR_DMA_EN) && addr == 0 && !(uart->lcr & 0x80)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: PIO transmit rejected while DMA is enabled on port %u\n",
                      p->index);
        return;
    }
    serial_io_ops.write(uart, addr, val, size);
    if (p->ioc4->serial_dma_experimental && addr == 4) {
        ioc4_shadow_set_outputs(p, (uint32_t)(uart->mcr & 0x1f) << 24);
    }
}

static const MemoryRegionOps ioc4_uart_ops = {
    .read = ioc4_uart_read,
    .write = ioc4_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Preserve wide cycles until accepts can reject them. */
    .valid = { .min_access_size = 1, .max_access_size = 4,
               .unaligned = true, .accepts = ioc4_uart_accepts },
    .impl = { .min_access_size = 1, .max_access_size = 4,
              .unaligned = true },
};

static void ioc4_reset_hold(Object *obj, ResetType type)
{
    SGIIOC4State *s = (SGIIOC4State *)obj;
    SGIIOC4Class *ic = (SGIIOC4Class *)object_get_class(obj);
    int i;

    if (ic->parent_phases.hold) {
        ic->parent_phases.hold(obj, type);
    }

    s->serial_pending = 0;
    s->serial_enable = 0;
    s->other_pending = 0;
    s->other_enable = 0;
    s->uart_level = 0;
    s->dma_base[0] = s->dma_base[1] = 0;
    s->dma_ring_size[0] = s->dma_ring_size[1] = IOC4_RING_1K;
    s->dma_error_low = s->dma_error_high = 0;
    s->dma_request_depth = 0;
    for (i = 0; i < IOC4_PORTS; i++) {
        ioc4_port_reset(&s->dma[i]);
        s->uart[i].baudbase = s->clock_hz / 16;
    }
    ioc4_update_irq(s);
}

static const VMStateDescription vmstate_ioc4_port = {
    .name = "sgi-ioc4-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, SGIIOC4Port),
        VMSTATE_UINT32(tx_produce, SGIIOC4Port),
        VMSTATE_UINT32(tx_consume, SGIIOC4Port),
        VMSTATE_UINT32(rx_produce, SGIIOC4Port),
        VMSTATE_UINT32(rx_consume, SGIIOC4Port),
        VMSTATE_UINT32(rx_timer_reload, SGIIOC4Port),
        VMSTATE_UINT32(shadow, SGIIOC4Port),
        VMSTATE_UINT8_ARRAY(rx_data, SGIIOC4Port, 4),
        VMSTATE_UINT8_ARRAY(rx_status, SGIIOC4Port, 4),
        VMSTATE_UINT8(rx_count, SGIIOC4Port),
        VMSTATE_UINT8_ARRAY(tx_data, SGIIOC4Port, 4),
        VMSTATE_UINT8_ARRAY(tx_status, SGIIOC4Port, 4),
        VMSTATE_UINT8(tx_slot, SGIIOC4Port),
        VMSTATE_BOOL(rx_staged, SGIIOC4Port),
        VMSTATE_BOOL(tx_waiting_rx, SGIIOC4Port),
        VMSTATE_BOOL(tx_active, SGIIOC4Port),
        VMSTATE_BOOL(timer_armed, SGIIOC4Port),
        VMSTATE_BOOL(rx_timer_running, SGIIOC4Port),
        VMSTATE_BOOL(tx_timer_running, SGIIOC4Port),
        VMSTATE_BOOL(tx_completion_pending, SGIIOC4Port),
        VMSTATE_BOOL(halted, SGIIOC4Port),
        VMSTATE_BOOL(drain_pending, SGIIOC4Port),
        VMSTATE_INT64(rx_remaining_ns, SGIIOC4Port),
        VMSTATE_INT64(tx_remaining_ns, SGIIOC4Port),
        VMSTATE_END_OF_LIST()
    }
};

static int ioc4_pre_save(void *opaque)
{
    SGIIOC4State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned i;

    for (i = 0; i < IOC4_PORTS; i++) {
        SGIIOC4Port *p = &s->dma[i];

        if (p->rx_timer_running && timer_pending(p->rx_timer)) {
            p->rx_remaining_ns = MAX(0,
                timer_expire_time_ns(p->rx_timer) - now);
        }
        if (p->tx_timer_running && timer_pending(p->tx_timer)) {
            p->tx_remaining_ns = MAX(0,
                timer_expire_time_ns(p->tx_timer) - now);
        }
    }
    return 0;
}

static int ioc4_post_load(void *opaque, int version_id)
{
    SGIIOC4State *s = opaque;
    int i;

    for (i = 0; i < IOC4_PORTS; i++) {
        SGIIOC4Port *p = &s->dma[i];
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        s->uart[i].baudbase = s->clock_hz / 16;
        serial_io_ops.write(&s->uart[i], 3, s->uart[i].lcr, 1);
        if (!ioc4_port_paused(p)) {
            if (p->rx_timer_running) {
                timer_mod(p->rx_timer, now + p->rx_remaining_ns);
            }
            if (p->tx_timer_running) {
                timer_mod(p->tx_timer, now + p->tx_remaining_ns);
            }
            if (p->rx_staged || p->drain_pending) {
                if (p->rx_count) {
                    ioc4_rx_publish(p);
                } else {
                    p->rx_staged = false;
                    p->drain_pending = false;
                    p->control &= ~IOC4_SSCR_DRAIN;
                }
            }
            ioc4_tx_run(p);
        }
        qemu_chr_fe_accept_input(&s->uart[i].chr);
    }
    ioc4_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_ioc4 = {
    .name = "sgi-ioc4",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = ioc4_pre_save,
    .post_load = ioc4_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(dma_migration_mode, SGIIOC4State),
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC4State),
        VMSTATE_STRUCT_ARRAY(uart, SGIIOC4State, IOC4_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_UINT32(serial_pending, SGIIOC4State),
        VMSTATE_UINT32(serial_enable, SGIIOC4State),
        VMSTATE_UINT32(other_enable, SGIIOC4State),
        VMSTATE_UINT32(uart_level, SGIIOC4State),
        VMSTATE_UINT32(clock_hz, SGIIOC4State),
        VMSTATE_UINT64_ARRAY(dma_base, SGIIOC4State, 2),
        VMSTATE_UINT32_ARRAY(dma_ring_size, SGIIOC4State, 2),
        VMSTATE_UINT32(other_pending, SGIIOC4State),
        VMSTATE_UINT32(dma_error_low, SGIIOC4State),
        VMSTATE_UINT32(dma_error_high, SGIIOC4State),
        VMSTATE_STRUCT_ARRAY(dma, SGIIOC4State, IOC4_PORTS, 1,
                             vmstate_ioc4_port, SGIIOC4Port),
        VMSTATE_END_OF_LIST()
    }
};

static void ioc4_exit(PCIDevice *dev)
{
    SGIIOC4State *s = (SGIIOC4State *)dev;
    int i;

    for (i = 0; i < IOC4_PORTS; i++) {
        SGIIOC4Port *p = &s->dma[i];

        if (p->tx_watch) {
            g_source_remove(p->tx_watch);
            p->tx_watch = 0;
        }
        if (p->rx_timer) {
            timer_free(p->rx_timer);
            p->rx_timer = NULL;
        }
        if (p->tx_timer) {
            timer_free(p->tx_timer);
            p->tx_timer = NULL;
        }
        memory_region_del_subregion(&s->bar, &s->uart[i].io);
        qdev_unrealize(DEVICE(&s->uart[i]));
    }
    if (s->clock_alias_mapped) {
        memory_region_del_subregion(&s->bar, &s->clock_alias);
        s->clock_alias_mapped = false;
    }
}

static void ioc4_realize(PCIDevice *dev, Error **errp)
{
    SGIIOC4State *s = (SGIIOC4State *)dev;
    int i;

    if (s->clock_hz != 33333333 && s->clock_hz != 66666667) {
        error_setg(errp, "sgi-ioc4 clock-hz must be 33333333 or 66666667");
        return;
    }
    if (s->clock && memory_region_size(s->clock) < 0x800) {
        error_setg(errp, "sgi-ioc4 clock region must be at least 0x800 bytes");
        return;
    }

    s->dma_migration_mode = s->serial_dma_experimental;
    /* Local PCI IRQ output needs a pin; IO9 board wiring is not modeled. */
    dev->config[PCI_INTERRUPT_PIN] = 1;

    memory_region_init_io(&s->bar, OBJECT(s), &ioc4_ops, s, "sgi-ioc4",
                          IOC4_BAR_SIZE);
    if (s->clock && !s->clock_alias_initialized) {
        memory_region_init_alias(&s->clock_alias, OBJECT(s),
                                 "sgi-ioc4-clock", s->clock,
                                 IOC4_CLOCK_NATIVE, IOC4_CLOCK_SIZE);
        s->clock_alias_initialized = true;
    }
    if (s->clock) {
        memory_region_add_subregion(&s->bar, IOC4_CLOCK_BAR,
                                    &s->clock_alias);
        s->clock_alias_mapped = true;
    }
    for (i = 0; i < IOC4_PORTS; i++) {
        SerialState *uart = &s->uart[i];

        qdev_prop_set_uint32(DEVICE(uart), "baudbase", s->clock_hz / 16);
        if (!qdev_realize(DEVICE(uart), NULL, errp)) {
            while (i--) {
                memory_region_del_subregion(&s->bar, &s->uart[i].io);
                qdev_unrealize(DEVICE(&s->uart[i]));
            }
            if (s->clock_alias_mapped) {
                memory_region_del_subregion(&s->bar, &s->clock_alias);
                s->clock_alias_mapped = false;
            }
            return;
        }
        uart->irq = &s->uart_irq[i];
        memory_region_init_io(&uart->io, OBJECT(s), &ioc4_uart_ops,
                              &s->dma[i],
                              "sgi-ioc4-uart", 8);
        memory_region_add_subregion(&s->bar, IOC4_UART_BASE + 8 * i,
                                    &uart->io);
    }
    for (i = 0; i < IOC4_PORTS; i++) {
        SGIIOC4Port *p = &s->dma[i];

        p->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ioc4_rx_timeout, p);
        p->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ioc4_tx_timeout, p);
        if (s->serial_dma_experimental) {
            ioc4_install_char_handlers(p);
        }
    }
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
}

static const Property ioc4_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", SGIIOC4State, clock_hz, 33333333),
    DEFINE_PROP_BOOL("serial-dma-experimental", SGIIOC4State,
                     serial_dma_experimental, false),
    DEFINE_PROP_LINK("clock", SGIIOC4State, clock, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_CHR("chardev1", SGIIOC4State, uart[0].chr),
    DEFINE_PROP_CHR("chardev2", SGIIOC4State, uart[1].chr),
    DEFINE_PROP_CHR("chardev3", SGIIOC4State, uart[2].chr),
    DEFINE_PROP_CHR("chardev4", SGIIOC4State, uart[3].chr),
};

static void ioc4_instance_init(Object *obj)
{
    SGIIOC4State *s = (SGIIOC4State *)obj;
    int i;

    s->clock_hz = 33333333;
    s->dma_ring_size[0] = IOC4_RING_1K;
    s->dma_ring_size[1] = IOC4_RING_1K;
    for (i = 0; i < IOC4_PORTS; i++) {
        qemu_init_irq_child(obj, "uart-irq[*]", &s->uart_irq[i],
                            ioc4_uart_irq, s, i);
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_SERIAL);
        s->dma[i].ioc4 = s;
        s->dma[i].index = i;
    }
}

static bool ioc4_byte_lane_self_ordered(Object *obj, Error **errp)
{
    return true;
}

static void ioc4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SGIIOC4Class *ic = (SGIIOC4Class *)klass;

    object_class_property_add_bool(klass, SGI_PIC_BYTE_LANE_SELF_ORDERED,
                                   ioc4_byte_lane_self_ordered, NULL);
    pc->realize = ioc4_realize;
    pc->exit = ioc4_exit;
    pc->vendor_id = 0x10a9;
    pc->device_id = 0x100a;
    pc->revision = 0x53;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->vmsd = &vmstate_ioc4;
    device_class_set_props(dc, ioc4_properties);
    resettable_class_set_parent_phases(rc, NULL, ioc4_reset_hold, NULL,
                                       &ic->parent_phases);
}

static const TypeInfo ioc4_info = {
    .name = TYPE_SGI_IOC4,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SGIIOC4State),
    .class_size = sizeof(SGIIOC4Class),
    .instance_init = ioc4_instance_init,
    .class_init = ioc4_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ioc4_register_types(void)
{
    type_register_static(&ioc4_info);
}
type_init(ioc4_register_types)
