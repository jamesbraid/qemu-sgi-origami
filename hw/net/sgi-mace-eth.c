/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* MACE 2.0 internal Ethernet MAC. */

#include "qemu/osdep.h"
#include "hw/net/sgi-mace-eth.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "system/dma.h"
#include "system/replay.h"

#define MACE_ETH_SIZE 0x80000
#define MACE_ETH_REGISTER_MASK 0x1ff
#define MACE_ETH_MAC_CONTROL 0x00
#define MACE_ETH_DMA_CONTROL 0x10
#define MACE_ETH_STATION 0xa0
#define MACE_ETH_INT_STATUS 0x08
#define MACE_ETH_INTERRUPT_REQUEST 0x58
#define MACE_ETH_RX_DELAY 0x18
#define MACE_ETH_TX_INT_ALIAS 0x20
#define MACE_ETH_RX_INT_ALIAS 0x28
#define MACE_ETH_SECONDARY_STATION 0xa8
#define MACE_ETH_MULTICAST_MASK 0xb0
#define MACE_ETH_TX_POINTER 0x30
#define MACE_ETH_TX_POINTER_ALIAS 0x38
#define MACE_ETH_TX_BASE 0xb8
#define MACE_ETH_RX_FIFO_INFO 0x40
#define MACE_ETH_RX_FIFO_INFO_ALIAS1 0x48
#define MACE_ETH_RX_FIFO_INFO_ALIAS2 0x50
#define MACE_ETH_RX_FIFO_PORT 0x100
#define MACE_ETH_RX_FIFO_PORT_END 0x1f8
#define MACE_ETH_RX_ENABLE (1U << 15)
#define MACE_ETH_RX_INT_ENABLE (1U << 9)
#define MACE_ETH_RX_THRESHOLD_INT (1U << 5)
#define MACE_ETH_RX_UNDERFLOW_INT (1U << 6)
#define MACE_ETH_RX_STATUS_VALID (UINT64_C(1) << 63)
#define MACE_ETH_RX_STATUS_BROADCAST (1U << 20)
#define MACE_ETH_RX_STATUS_PHYSICAL (1U << 26)
#define MACE_ETH_TX_ENABLE (1U << 1)
#define MACE_ETH_TX_INT_ENABLE 1U
#define MACE_ETH_TX_DONE_INT (1U << 1)
#define MACE_ETH_TX_EMPTY_INT 1U
#define MACE_ETH_MEMORY_ERROR_INT (1U << 3)
#define MACE_ETH_TX_STATUS_DONE (UINT64_C(1) << 63)
#define MACE_ETH_TX_STATUS_SUCCESS (1U << 23)
#define MACE_ETH_TX_REINIT_STOP  0x01
#define MACE_ETH_TX_REINIT_RESET 0x02
#define MACE_ETH_TX_REINIT_BASE  0x04
#define MACE_ETH_TX_REINIT_READY 0x07
#define MACE_ETH_RX_REINIT_DMA_OFF 0x01
#define MACE_ETH_RX_REINIT_MAC_HELD 0x02
#define MACE_ETH_RX_REINIT_MAC_PULSED 0x04
#define MACE_ETH_MDIO_DATA 0x60
#define MACE_ETH_MDIO_ADDRESS 0x68
#define MACE_ETH_MDIO_READ 0x70
#define MACE_ETH_BACKOFF 0x78
#define MACE_ETH_MDIO_BUSY (1U << 16)
/* The specification leaves MDIO duration open; keep busy visible to polls. */
#define MACE_ETH_MDIO_NS 1000
#define MACE_ETH_RX_TICK_NS 30690
#define MACE_ETH_REVISION (1U << 29)
#define MACE_ETH_MAC_RESET 1U
#define MACE_ETH_MAC_LOOPBACK (1U << 2)
#define MACE_ETH_MAC_SIA (1U << 4)
#define MACE_ETH_RX_GATHER (1U << 11)

static void sgi_mace_eth_schedule_flush(SGIMACEEthState *s)
{
    if (s->flush_bh) {
        replay_bh_schedule_event(s->flush_bh);
    }
}

static void sgi_mace_eth_phy_ready(void *opaque)
{
    sgi_mace_eth_schedule_flush(opaque);
}

static void sgi_mace_eth_mdio_complete(void *opaque)
{
    SGIMACEEthState *s = opaque;
    unsigned phy = (s->mdio_address >> 5) & 31;
    unsigned reg = s->mdio_address & 31;
    bool selected = s->phy_ics1890 && phy == s->phy_address;

    if (s->mdio_pending_read) {
        s->mdio_data = selected ? ics1890_read(s->phy, reg) : 0xffff;
    } else if (selected) {
        ics1890_write(s->phy, reg, s->mdio_pending_value);
    }
    s->mdio_busy = false;
    sgi_mace_eth_schedule_flush(s);
}

static void sgi_mace_eth_mdio_start(SGIMACEEthState *s, bool read)
{
    if (s->mdio_busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-eth: MDIO request while busy\n");
        return;
    }
    s->mdio_pending_read = read;
    s->mdio_pending_value = s->mdio_data;
    s->mdio_busy = true;
    timer_mod(s->mdio_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MACE_ETH_MDIO_NS);
}

static bool sgi_mace_eth_link_ready(SGIMACEEthState *s)
{
    if ((s->mac_control & (MACE_ETH_MAC_LOOPBACK | MACE_ETH_MAC_SIA)) ||
        !s->phy || !ics1890_data_enabled(s->phy) ||
        ics1890_in_loopback(s->phy) || s->phy->link_down) {
        return false;
    }
    return !(s->phy->regs[0] & 0x1000) || s->phy->autoneg_done;
}

static bool sgi_mace_eth_rx_available(SGIMACEEthState *s)
{
    unsigned offset = ((s->dma_control >> 12) & 7) * 8 + 2;

    if (!s->rx_count) {
        return false;
    }
    /* Closing a gathered buffer consumes an address before the next frame. */
    return !s->rx_gather_open ||
           s->rx_gather_cursor + offset + 1514 + 4 <= 4096 ||
           s->rx_count > 1;
}

static void sgi_mace_eth_rx_retire(SGIMACEEthState *s)
{
    if (s->rx_threshold_retirement &&
        s->rx_count == ((s->dma_control >> 4) & 31)) {
        s->int_status &= ~MACE_ETH_RX_THRESHOLD_INT;
    }
}

static void sgi_mace_eth_update_irq(SGIMACEEthState *s)
{
    unsigned threshold = (s->dma_control >> 4) & 31;
    uint32_t events;
    bool rx_active = (s->dma_control & (MACE_ETH_RX_ENABLE |
                                       MACE_ETH_RX_INT_ENABLE)) ==
                     (MACE_ETH_RX_ENABLE | MACE_ETH_RX_INT_ENABLE);
    bool tx_active;

    if (!(s->mac_control & MACE_ETH_MAC_RESET) &&
        (s->dma_control & (MACE_ETH_TX_ENABLE | MACE_ETH_TX_INT_ENABLE)) ==
        (MACE_ETH_TX_ENABLE | MACE_ETH_TX_INT_ENABLE) &&
        s->tx_read == s->tx_write && sgi_mace_eth_link_ready(s)) {
        s->int_status |= MACE_ETH_TX_EMPTY_INT;
    }
    if (!s->rx_threshold_retirement && rx_active &&
        !s->rx_delay && !s->rx_pending &&
        s->rx_count != threshold) {
        s->int_status |= MACE_ETH_RX_THRESHOLD_INT;
    }
    events = s->int_status;
    tx_active = (s->dma_control & MACE_ETH_TX_INT_ENABLE) &&
                (events & (MACE_ETH_TX_EMPTY_INT | MACE_ETH_TX_DONE_INT));
    qemu_set_irq(s->irq, tx_active ||
                 (rx_active && (events & MACE_ETH_RX_THRESHOLD_INT)) ||
                 (events & (MACE_ETH_MEMORY_ERROR_INT |
                            MACE_ETH_RX_UNDERFLOW_INT | (1U << 2) |
                            (1U << 4) | (1U << 7))));
}

static uint32_t sgi_mace_eth_fifo_pop(SGIMACEEthState *s);

static void sgi_mace_eth_rx_close(SGIMACEEthState *s)
{
    if (s->rx_gather_open) {
        s->rx_gather_open = false;
        sgi_mace_eth_fifo_pop(s);
    }
    s->rx_gather_address = 0;
    s->rx_gather_cursor = 0;
}

static void sgi_mace_eth_rx_expire(void *opaque)
{
    SGIMACEEthState *s = opaque;

    sgi_mace_eth_rx_close(s);
    timer_del(s->rx_timer);
    s->rx_pending = false;
    if ((s->dma_control & MACE_ETH_RX_ENABLE) &&
        s->rx_count != ((s->dma_control >> 4) & 31)) {
        s->int_status |= MACE_ETH_RX_THRESHOLD_INT;
    }
    sgi_mace_eth_update_irq(s);
}

static void sgi_mace_eth_rx_cancel(SGIMACEEthState *s)
{
    timer_del(s->rx_timer);
    s->rx_pending = false;
    s->rx_gather_open = false;
    s->rx_gather_address = 0;
    s->rx_gather_cursor = 0;
}

static uint32_t sgi_mace_eth_fifo_info(SGIMACEEthState *s)
{
    return ((uint32_t)s->rx_write_generation << 20) |
           ((uint32_t)s->rx_write << 16) |
           ((uint32_t)s->rx_read_generation << 12) |
           ((uint32_t)s->rx_read << 8) | s->rx_count;
}

static void sgi_mace_eth_rx_reinit_finish(SGIMACEEthState *s)
{
    if ((s->rx_reinit_seen & (MACE_ETH_RX_REINIT_DMA_OFF |
                              MACE_ETH_RX_REINIT_MAC_PULSED)) !=
        (MACE_ETH_RX_REINIT_DMA_OFF | MACE_ETH_RX_REINIT_MAC_PULSED)) {
        return;
    }
    sgi_mace_eth_rx_cancel(s);
    s->rx_stopped = false;
    s->rx_count = 0;
    s->rx_read = 0;
    s->rx_write = 0;
    s->rx_read_generation = false;
    s->rx_write_generation = false;
    sgi_mace_eth_rx_retire(s);
    sgi_mace_eth_update_irq(s);
    s->rx_reinit_seen = 0;
}

static void sgi_mace_eth_fifo_advance(uint8_t *index, bool *generation)
{
    *index = (*index + 1) & 15;
    if (*index == 0) {
        *generation = !*generation;
    }
}

static void sgi_mace_eth_fifo_push(SGIMACEEthState *s, uint32_t address)
{
    bool resume = s->rx_threshold_retirement && !sgi_mace_eth_rx_available(s);

    if (s->rx_count == 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-eth: RX address FIFO overrun is unsupported\n");
        return;
    }
    s->rx_fifo[s->rx_write] = address & UINT32_C(0xfffff000);
    sgi_mace_eth_fifo_advance(&s->rx_write, &s->rx_write_generation);
    s->rx_count++;
    sgi_mace_eth_rx_retire(s);
    sgi_mace_eth_update_irq(s);
    if (resume && sgi_mace_eth_rx_available(s)) {
        sgi_mace_eth_schedule_flush(s);
    }
}

static uint32_t sgi_mace_eth_fifo_pop(SGIMACEEthState *s)
{
    uint32_t address;

    if (!s->rx_count) {
        return 0;
    }
    address = s->rx_fifo[s->rx_read];
    sgi_mace_eth_fifo_advance(&s->rx_read, &s->rx_read_generation);
    s->rx_count--;
    sgi_mace_eth_rx_retire(s);
    sgi_mace_eth_update_irq(s);
    return address;
}

static bool sgi_mace_eth_accept(SGIMACEEthState *s, const uint8_t *frame,
                                size_t size, uint64_t *match)
{
    unsigned mode = (s->mac_control >> 5) & 3;
    bool broadcast = true;
    bool physical = true;

    if (size < 14) {
        return false;
    }
    for (unsigned i = 0; i < 6; i++) {
        broadcast &= frame[i] == 0xff;
        physical &= frame[i] == ((s->station >> (8 * (5 - i))) & 0xff);
    }
    *match = physical ? MACE_ETH_RX_STATUS_PHYSICAL :
             broadcast ? MACE_ETH_RX_STATUS_BROADCAST : 0;
    if (mode == 3 || physical || (broadcast && mode >= 1)) {
        return true;
    }
    if (frame[0] & 1) {
        if (mode == 2) {
            return true;
        }
        if (mode == 1) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-eth: multicast hash filtering "
                          "is unsupported\n");
        }
    }
    return false;
}

static bool sgi_mace_eth_rx_ready(SGIMACEEthState *s)
{
    return !s->rx_stopped && !(s->mac_control & MACE_ETH_MAC_RESET) &&
           (s->dma_control & MACE_ETH_RX_ENABLE) &&
           sgi_mace_eth_link_ready(s);
}

static bool sgi_mace_eth_can_receive(NetClientState *nc)
{
    SGIMACEEthState *s = qemu_get_nic_opaque(nc);

    return sgi_mace_eth_rx_ready(s) &&
           (!s->rx_threshold_retirement || sgi_mace_eth_rx_available(s));
}

static ssize_t sgi_mace_eth_receive(NetClientState *nc,
                                    const uint8_t *frame, size_t size)
{
    SGIMACEEthState *s = qemu_get_nic_opaque(nc);
    uint8_t fcs[4], status[8], padding[2] = { 0, 0 };
    uint64_t match, vector;
    uint32_t crc, address, sum = 0;
    unsigned offset = ((s->dma_control >> 12) & 7) * 8 + 2;
    unsigned bucket = s->rx_gather_open ? s->rx_gather_cursor : 0;
    unsigned end;
    MemTxResult result;

    if (!sgi_mace_eth_rx_ready(s) ||
        !sgi_mace_eth_accept(s, frame, size, &match)) {
        return size;
    }
    if (!s->rx_count && !s->rx_threshold_retirement) {
        s->int_status |= MACE_ETH_RX_UNDERFLOW_INT;
        sgi_mace_eth_update_irq(s);
        return size;
    }
    if (size < 60 && !(s->dma_control & (1U << 10))) {
        return size;
    }
    if (size > 1514) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-eth: oversized RX frame is unsupported\n");
        return size;
    }
    /* Queued retries bypass can_receive. */
    if (s->rx_threshold_retirement && !sgi_mace_eth_rx_available(s)) {
        return 0;
    }
    if (s->rx_gather_open && bucket + offset + 1514 + sizeof(fcs) > 4096) {
        sgi_mace_eth_rx_close(s);
        bucket = 0;
    }
    end = bucket + offset + size + sizeof(fcs);
    address = s->rx_gather_open ? s->rx_gather_address :
              s->rx_fifo[s->rx_read];
    address += bucket;
    crc = ~net_crc32_le(frame, size);
    stl_le_p(fcs, crc);
    for (unsigned i = 0; i < size + sizeof(fcs); i++) {
        uint8_t byte = i < size ? frame[i] : fcs[i - size];

        sum += byte << ((i & 1) ? 0 : 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    vector = MACE_ETH_RX_STATUS_VALID | match | ((uint64_t)sum << 32) |
             ((uint64_t)s->rx_sequence << 27) | (size + sizeof(fcs));
    stq_be_p(status, vector);
    result = dma_memory_write(&s->dma_as, address + offset, frame, size,
                              MEMTXATTRS_UNSPECIFIED);
    result |= dma_memory_write(&s->dma_as, address + offset + size, fcs,
                               sizeof(fcs), MEMTXATTRS_UNSPECIFIED);
    result |= dma_memory_write(&s->dma_as, address + offset - 2, padding,
                               sizeof(padding), MEMTXATTRS_UNSPECIFIED);
    if (result != MEMTX_OK) {
        goto memory_error;
    }
    result = dma_memory_write(&s->dma_as, address, status, sizeof(status),
                              MEMTXATTRS_UNSPECIFIED);
    if (result != MEMTX_OK) {
        goto memory_error;
    }
    s->rx_sequence = (s->rx_sequence + 1) & 31;
    if (s->rx_delay && (s->dma_control & MACE_ETH_RX_GATHER) &&
        offset + size + sizeof(fcs) <= 256 &&
        ROUND_UP(end, 256) + offset + 1514 + sizeof(fcs) <= 4096 &&
        s->rx_count >= 4) {
        s->rx_gather_open = true;
        s->rx_gather_address = address - bucket;
        s->rx_gather_cursor = ROUND_UP(end, 256);
    } else if (s->rx_gather_open) {
        sgi_mace_eth_rx_close(s);
    } else {
        sgi_mace_eth_fifo_pop(s);
    }
    if (s->rx_delay || s->rx_pending) {
        if (s->rx_count < 4) {
            sgi_mace_eth_rx_expire(s);
        } else if (!s->rx_pending) {
            s->rx_pending = true;
            timer_mod(s->rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      s->rx_delay * MACE_ETH_RX_TICK_NS);
        }
    } else if (s->rx_threshold_retirement) {
        sgi_mace_eth_rx_expire(s);
    }
    return size;

memory_error:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-mace-eth: RX DMA failed at 0x%" PRIx32 "\n",
                  address);
    sgi_mace_eth_rx_cancel(s);
    s->rx_stopped = true;
    s->dma_control &= ~MACE_ETH_RX_ENABLE;
    sgi_mace_eth_update_irq(s);
    return size;
}

static void sgi_mace_eth_set_link(NetClientState *nc)
{
    SGIMACEEthState *s = qemu_get_nic_opaque(nc);
    bool was_ready = sgi_mace_eth_link_ready(s);

    if (s->phy) {
        ics1890_update_link(s->phy, nc->link_down || !nc->peer);
    }
    if (was_ready && !sgi_mace_eth_link_ready(s) &&
        (s->mac_control & (1U << 7))) {
        s->int_status |= 1U << 2;
        sgi_mace_eth_update_irq(s);
    }
    if (!was_ready && sgi_mace_eth_link_ready(s)) {
        sgi_mace_eth_schedule_flush(s);
    }
}

static NetClientInfo sgi_mace_eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sgi_mace_eth_can_receive,
    .receive = sgi_mace_eth_receive,
    .link_status_changed = sgi_mace_eth_set_link,
};

static bool sgi_mace_eth_tx_one(SGIMACEEthState *s)
{
    uint8_t descriptor[128];
    uint8_t frame[2048];
    uint8_t status[8];
    uint64_t command, address, fault_address, vector;
    unsigned ring_entries = 64U << ((s->dma_control >> 2) & 3);
    unsigned total, start, inline_length, copied, pointer_flags;
    MemTxResult result;

    address = s->tx_base + ((s->tx_read % ring_entries) << 7);
    fault_address = address;
    result = dma_memory_read(&s->dma_as, address, descriptor,
                             sizeof(descriptor), MEMTXATTRS_UNSPECIFIED);
    if (result != MEMTX_OK) {
        goto memory_error;
    }
    command = ldq_be_p(descriptor);
    total = (command & 0xffff) + 1;
    start = (command >> 16) & 0x7f;
    pointer_flags = (command >> 25) & 7;
    if ((command & ~UINT64_C(0x0fffffff)) || total > sizeof(frame) ||
        (start && start < 8 + 8 * (32 - clz32(pointer_flags))) ||
        (start == 0 && !pointer_flags)) {
        goto invalid_descriptor;
    }
    inline_length = start ? MIN(total, sizeof(descriptor) - start) : 0;
    copied = inline_length;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t pointer;
        unsigned length;

        if (!(pointer_flags & (1U << i))) {
            continue;
        }
        pointer = ldq_be_p(descriptor + 8 + 8 * i);
        length = ((pointer >> 32) & 0xffff) + 1;
        if ((pointer & ~UINT64_C(0x0000fffffffffff8)) ||
            length > total - copied) {
            goto invalid_descriptor;
        }
        copied += length;
    }
    if (copied != total) {
        goto invalid_descriptor;
    }
    memcpy(frame, descriptor + start, inline_length);
    copied = inline_length;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t pointer;
        unsigned length;

        if (!(pointer_flags & (1U << i))) {
            continue;
        }
        pointer = ldq_be_p(descriptor + 8 + 8 * i);
        length = ((pointer >> 32) & 0xffff) + 1;
        fault_address = pointer & UINT32_C(0xfffffff8);
        result = dma_memory_read(&s->dma_as, fault_address,
                                 frame + copied, length,
                                 MEMTXATTRS_UNSPECIFIED);
        if (result != MEMTX_OK) {
            goto memory_error;
        }
        copied += length;
    }
    qemu_send_packet(qemu_get_queue(s->nic), frame, total);
    vector = MACE_ETH_TX_STATUS_DONE | MACE_ETH_TX_STATUS_SUCCESS | total;
    s->tx_vector = vector;
    stq_be_p(status, vector);
    fault_address = address;
    result = dma_memory_write(&s->dma_as, address, status, sizeof(status),
                              MEMTXATTRS_UNSPECIFIED);
    if (result != MEMTX_OK) {
        goto memory_error;
    }
    if (command & (1U << 24)) {
        s->int_status |= MACE_ETH_TX_DONE_INT;
    }
    s->tx_read = (s->tx_read + 1) % ring_entries;
    s->tx_reinit_seen = 0;
    return true;

invalid_descriptor:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-mace-eth: invalid TX descriptor at 0x%" PRIx64 "\n",
                  address);
    s->int_status |= 1U << 4;
    s->tx_stopped = true;
    s->dma_control &= ~MACE_ETH_TX_ENABLE;
    return false;

memory_error:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-mace-eth: TX DMA failed at 0x%" PRIx64 "\n",
                  fault_address);
    s->int_status |= MACE_ETH_MEMORY_ERROR_INT;
    s->tx_stopped = true;
    s->dma_control &= ~MACE_ETH_TX_ENABLE;
    return false;
}

static void sgi_mace_eth_tx_run(SGIMACEEthState *s)
{
    if (s->tx_stopped || (s->mac_control & MACE_ETH_MAC_RESET) ||
        !(s->dma_control & MACE_ETH_TX_ENABLE) ||
        !sgi_mace_eth_link_ready(s)) {
        return;
    }
    while (s->tx_read != s->tx_write) {
        if (!sgi_mace_eth_tx_one(s)) {
            return;
        }
    }
    if ((s->dma_control & MACE_ETH_TX_INT_ENABLE) &&
        s->tx_read == s->tx_write) {
        s->int_status |= MACE_ETH_TX_EMPTY_INT;
    }
}

/* A queued receive must run after the MMIO write that enabled it returns. */
static void sgi_mace_eth_flush(void *opaque)
{
    SGIMACEEthState *s = opaque;

    sgi_mace_eth_tx_run(s);
    sgi_mace_eth_update_irq(s);
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static uint64_t sgi_mace_eth_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIMACEEthState *s = opaque;
    unsigned reg = offset & MACE_ETH_REGISTER_MASK;

    if (size == 4) {
        switch (offset) {
        case MACE_ETH_MAC_CONTROL + 4:
        case MACE_ETH_DMA_CONTROL + 4:
        case MACE_ETH_MDIO_DATA + 4:
            return sgi_mace_eth_read(opaque, offset - 4, 8) & UINT32_MAX;
        }
    }
    if (size == 2 && offset == MACE_ETH_TX_POINTER + 4) {
        return s->tx_read;
    }
    if (size == 2 && offset == MACE_ETH_TX_POINTER + 6) {
        return s->tx_write;
    }
    if (size == 1 && offset == MACE_ETH_RX_FIFO_INFO + 6) {
        return (sgi_mace_eth_fifo_info(s) >> 8) & 0xff;
    }
    if (size == 8) {
        if (reg >= MACE_ETH_RX_FIFO_PORT &&
            reg <= MACE_ETH_RX_FIFO_PORT_END && !(reg & 7)) {
            if (s->dma_control & MACE_ETH_RX_ENABLE) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace-eth: RX FIFO read while DMA active\n");
                return 0;
            }
            return sgi_mace_eth_fifo_pop(s);
        }
        switch (reg) {
        case MACE_ETH_MAC_CONTROL:
            return s->mac_control;
        case MACE_ETH_DMA_CONTROL:
            return s->dma_control;
        case MACE_ETH_INT_STATUS:
            return s->int_status | (sgi_mace_eth_fifo_info(s) & 0x1f00) |
                   ((uint64_t)s->tx_read << 16) |
                   ((uint64_t)s->rx_sequence << 25);
        case MACE_ETH_RX_FIFO_INFO:
        case MACE_ETH_RX_FIFO_INFO_ALIAS1:
        case MACE_ETH_RX_FIFO_INFO_ALIAS2:
            return sgi_mace_eth_fifo_info(s);
        case MACE_ETH_TX_POINTER:
        case MACE_ETH_TX_POINTER_ALIAS:
            return ((uint64_t)s->tx_read << 16) | s->tx_write;
        case MACE_ETH_TX_BASE:
            return s->tx_base;
        case MACE_ETH_INTERRUPT_REQUEST:
            return s->tx_vector;
        case MACE_ETH_STATION:
            return s->station;
        case MACE_ETH_SECONDARY_STATION:
            return s->secondary_station;
        case MACE_ETH_MULTICAST_MASK:
            return s->multicast_mask;
        case MACE_ETH_RX_DELAY:
            return s->rx_delay;
        case MACE_ETH_MDIO_DATA:
            return s->mdio_data | (s->mdio_busy ? MACE_ETH_MDIO_BUSY : 0);
        case MACE_ETH_MDIO_ADDRESS:
            return s->mdio_address;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mace-eth: unsupported read at 0x%"
                  HWADDR_PRIx " size %u\n", offset, size);
    return 0;
}

static void sgi_mace_eth_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    SGIMACEEthState *s = opaque;
    unsigned reg = offset & MACE_ETH_REGISTER_MASK;

    if (size == 4) {
        if (offset >= MACE_ETH_RX_FIFO_PORT + 4 &&
            offset <= MACE_ETH_RX_FIFO_PORT_END + 4 &&
            !((offset - 4) & 7)) {
            sgi_mace_eth_fifo_push(s, value);
            return;
        }
        switch (offset) {
        case MACE_ETH_MAC_CONTROL + 4:
        case MACE_ETH_INT_STATUS + 4:
        case MACE_ETH_DMA_CONTROL + 4:
        case MACE_ETH_RX_DELAY + 4:
        case MACE_ETH_TX_INT_ALIAS + 4:
        case MACE_ETH_MDIO_DATA + 4:
        case MACE_ETH_MDIO_ADDRESS + 4:
        case MACE_ETH_MDIO_READ + 4:
        case MACE_ETH_BACKOFF + 4:
            sgi_mace_eth_write(opaque, offset - 4, value, 8);
            return;
        }
    }
    if (size == 2 && offset == MACE_ETH_TX_POINTER + 6) {
        sgi_mace_eth_write(opaque, MACE_ETH_TX_POINTER, value, 8);
        return;
    }
    if (size == 8) {
        if (reg >= MACE_ETH_RX_FIFO_PORT &&
            reg <= MACE_ETH_RX_FIFO_PORT_END && !(reg & 7)) {
            sgi_mace_eth_fifo_push(s, value);
            return;
        }
        switch (reg) {
        case MACE_ETH_MAC_CONTROL:
            if (value & MACE_ETH_MAC_RESET) {
                s->tx_reinit_seen |= MACE_ETH_TX_REINIT_RESET;
                s->rx_reinit_seen |= MACE_ETH_RX_REINIT_MAC_HELD;
            } else if (s->rx_reinit_seen & MACE_ETH_RX_REINIT_MAC_HELD) {
                s->rx_reinit_seen &= ~MACE_ETH_RX_REINIT_MAC_HELD;
                s->rx_reinit_seen |= MACE_ETH_RX_REINIT_MAC_PULSED;
            }
            s->mac_control = MACE_ETH_REVISION | (value & 0x1fffffff);
            if (!(value & MACE_ETH_MAC_RESET)) {
                sgi_mace_eth_rx_reinit_finish(s);
            }
            if (value & (MACE_ETH_MAC_LOOPBACK | MACE_ETH_MAC_SIA)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-eth: MAC loopback/SIA is unsupported\n");
            }
            sgi_mace_eth_tx_run(s);
            sgi_mace_eth_update_irq(s);
            sgi_mace_eth_schedule_flush(s);
            return;
        case MACE_ETH_DMA_CONTROL: {
            bool rx_enable_requested = value & MACE_ETH_RX_ENABLE;

            if (s->rx_stopped && rx_enable_requested) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace-eth: RX restart requires full "
                              "reinitialization\n");
                value &= ~MACE_ETH_RX_ENABLE;
            }
            if ((s->dma_control & (MACE_ETH_TX_ENABLE | MACE_ETH_RX_ENABLE)) &&
                ((value ^ s->dma_control) & 0x0c)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace-eth: live TX ring size change\n");
                value = (value & ~UINT64_C(0x0c)) | (s->dma_control & 0x0c);
            }
            if (!(value & MACE_ETH_TX_ENABLE)) {
                s->tx_reinit_seen |= MACE_ETH_TX_REINIT_STOP;
            }
            if (!(s->dma_control & MACE_ETH_TX_ENABLE) &&
                (value & MACE_ETH_TX_ENABLE)) {
                if (s->tx_reinit_seen == MACE_ETH_TX_REINIT_READY &&
                    !(s->mac_control & MACE_ETH_MAC_RESET)) {
                    s->tx_stopped = false;
                    s->tx_read = 0;
                    s->tx_write = 0;
                }
                s->tx_reinit_seen = 0;
            }
            s->dma_control = value & 0xffff;
            sgi_mace_eth_rx_retire(s);
            if (!rx_enable_requested) {
                sgi_mace_eth_rx_close(s);
                sgi_mace_eth_rx_cancel(s);
                s->rx_reinit_seen |= MACE_ETH_RX_REINIT_DMA_OFF;
                sgi_mace_eth_rx_reinit_finish(s);
            } else {
                s->rx_reinit_seen = 0;
            }
            sgi_mace_eth_tx_run(s);
            sgi_mace_eth_update_irq(s);
            sgi_mace_eth_schedule_flush(s);
            return;
        }
        case MACE_ETH_INT_STATUS:
            s->int_status &= ~(value & 0xff);
            sgi_mace_eth_update_irq(s);
            return;
        case MACE_ETH_INTERRUPT_REQUEST:
            if (offset & 7) {
                break;
            }
            if (value >> 32) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-eth: interrupt request upper bits "
                              "are unsupported\n");
                return;
            }
            if (s->irq_request) {
                s->irq_request(s->irq_opaque, value);
            } else {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-eth: interrupt request has no handler\n");
            }
            return;
        case MACE_ETH_TX_POINTER:
        case MACE_ETH_TX_POINTER_ALIAS:
            if ((value & 0x1ff) >=
                (64U << ((s->dma_control >> 2) & 3))) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-eth: TX pointer outside active ring\n");
                return;
            }
            s->tx_write = value & 0x1ff;
            sgi_mace_eth_tx_run(s);
            sgi_mace_eth_update_irq(s);
            return;
        case MACE_ETH_TX_BASE:
            s->tx_base = value & UINT32_C(0xffffe000);
            if (!(s->dma_control & MACE_ETH_TX_ENABLE) &&
                (s->tx_reinit_seen & (MACE_ETH_TX_REINIT_STOP |
                                      MACE_ETH_TX_REINIT_RESET)) ==
                (MACE_ETH_TX_REINIT_STOP | MACE_ETH_TX_REINIT_RESET)) {
                s->tx_reinit_seen |= MACE_ETH_TX_REINIT_BASE;
            }
            return;
        case MACE_ETH_STATION:
            s->station = value & UINT64_C(0xffffffffffff);
            return;
        case MACE_ETH_SECONDARY_STATION:
            s->secondary_station = value & UINT64_C(0xffffffffffff);
            if (s->secondary_station) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-eth: secondary station filtering "
                              "is unsupported\n");
            }
            return;
        case MACE_ETH_MULTICAST_MASK:
            s->multicast_mask = value;
            if (s->multicast_mask) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace-eth: multicast hash filtering "
                              "is unsupported\n");
            }
            return;
        case MACE_ETH_RX_DELAY:
            s->rx_delay = value & 0x3f;
            return;
        case MACE_ETH_TX_INT_ALIAS:
            s->dma_control = (s->dma_control & ~MACE_ETH_TX_INT_ENABLE) |
                             (value & MACE_ETH_TX_INT_ENABLE);
            sgi_mace_eth_tx_run(s);
            sgi_mace_eth_update_irq(s);
            return;
        case MACE_ETH_RX_INT_ALIAS:
            s->dma_control = (s->dma_control & ~UINT64_C(0x3f0)) |
                             (value & UINT64_C(0x3f0));
            sgi_mace_eth_rx_retire(s);
            sgi_mace_eth_update_irq(s);
            return;
        case MACE_ETH_MDIO_DATA:
            s->mdio_data = value & 0xffff;
            sgi_mace_eth_mdio_start(s, false);
            return;
        case MACE_ETH_MDIO_ADDRESS:
            s->mdio_address = value & 0x3ff;
            return;
        case MACE_ETH_MDIO_READ:
            sgi_mace_eth_mdio_start(s, true);
            return;
        case MACE_ETH_BACKOFF:
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-eth: collision backoff seed 0x%" PRIx64
                          " has no collision model\n", value & 0x7ff);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mace-eth: unsupported write at 0x%"
                  HWADDR_PRIx " size %u value 0x%" PRIx64 "\n",
                  offset, size, value);
}

static const MemoryRegionOps sgi_mace_eth_ops = {
    .read = sgi_mace_eth_read,
    .write = sgi_mace_eth_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void sgi_mace_eth_reset_hold(Object *obj, ResetType type)
{
    SGIMACEEthState *s = SGI_MACE_ETH(obj);

    s->mac_control = MACE_ETH_REVISION | MACE_ETH_MAC_RESET;
    s->dma_control = 0;
    s->rx_delay = 0;
    sgi_mace_eth_rx_cancel(s);
    s->tx_base = 0;
    s->tx_read = 0;
    s->tx_write = 0;
    s->tx_reinit_seen = 0;
    s->tx_stopped = false;
    s->rx_stopped = false;
    s->rx_reinit_seen = 0;
    s->int_status = 0;
    s->tx_vector = 0;
    s->rx_read = 0;
    s->rx_write = 0;
    s->rx_count = 0;
    s->rx_sequence = 0;
    s->rx_read_generation = false;
    s->rx_write_generation = false;
    qemu_set_irq(s->irq, 0);
    if (s->irq_reset) {
        s->irq_reset(s->irq_opaque);
    }
    if (s->flush_bh) {
        qemu_bh_cancel(s->flush_bh);
    }
    s->mdio_data = 0;
    s->mdio_address = 0;
    s->mdio_pending_value = 0;
    s->mdio_busy = false;
    s->mdio_pending_read = false;
    timer_del(s->mdio_timer);
}

static int sgi_mace_eth_post_load(void *opaque, int version_id)
{
    SGIMACEEthState *s = opaque;

    if (!s->defer_post_load) {
        sgi_mace_eth_restore_post_load(s);
    }
    return 0;
}

void sgi_mace_eth_restore_post_load(SGIMACEEthState *s)
{
    sgi_mace_eth_rx_retire(s);
    sgi_mace_eth_update_irq(s);
    sgi_mace_eth_schedule_flush(s);
}

static const VMStateDescription vmstate_sgi_mace_eth = {
    .name = TYPE_SGI_MACE_ETH,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_mace_eth_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(rx_threshold_retirement, SGIMACEEthState),
        VMSTATE_UINT64(mac_control, SGIMACEEthState),
        VMSTATE_UINT64(dma_control, SGIMACEEthState),
        VMSTATE_UINT64(station, SGIMACEEthState),
        VMSTATE_UINT64(secondary_station, SGIMACEEthState),
        VMSTATE_UINT64(multicast_mask, SGIMACEEthState),
        VMSTATE_UINT8(rx_delay, SGIMACEEthState),
        VMSTATE_BOOL(rx_pending, SGIMACEEthState),
        VMSTATE_BOOL(rx_gather_open, SGIMACEEthState),
        VMSTATE_UINT32(rx_gather_address, SGIMACEEthState),
        VMSTATE_UINT16(rx_gather_cursor, SGIMACEEthState),
        VMSTATE_TIMER_PTR(rx_timer, SGIMACEEthState),
        VMSTATE_UINT64(tx_base, SGIMACEEthState),
        VMSTATE_UINT32(int_status, SGIMACEEthState),
        VMSTATE_UINT64(tx_vector, SGIMACEEthState),
        VMSTATE_UINT16(tx_read, SGIMACEEthState),
        VMSTATE_UINT16(tx_write, SGIMACEEthState),
        VMSTATE_UINT8(tx_reinit_seen, SGIMACEEthState),
        VMSTATE_BOOL(tx_stopped, SGIMACEEthState),
        VMSTATE_BOOL(rx_stopped, SGIMACEEthState),
        VMSTATE_UINT8(rx_reinit_seen, SGIMACEEthState),
        VMSTATE_UINT32_ARRAY(rx_fifo, SGIMACEEthState, 16),
        VMSTATE_UINT8(rx_read, SGIMACEEthState),
        VMSTATE_UINT8(rx_write, SGIMACEEthState),
        VMSTATE_UINT8(rx_count, SGIMACEEthState),
        VMSTATE_UINT8(rx_sequence, SGIMACEEthState),
        VMSTATE_BOOL(rx_read_generation, SGIMACEEthState),
        VMSTATE_BOOL(rx_write_generation, SGIMACEEthState),
        VMSTATE_UINT16(mdio_data, SGIMACEEthState),
        VMSTATE_UINT16(mdio_address, SGIMACEEthState),
        VMSTATE_UINT16(mdio_pending_value, SGIMACEEthState),
        VMSTATE_BOOL(mdio_busy, SGIMACEEthState),
        VMSTATE_BOOL(mdio_pending_read, SGIMACEEthState),
        VMSTATE_TIMER_PTR(mdio_timer, SGIMACEEthState),
        VMSTATE_END_OF_LIST()
    }
};

void sgi_mace_eth_set_irq_handlers(SGIMACEEthState *s, void *opaque,
                                   SGIMACEEthIRQRequest request,
                                   SGIMACEEthResetIRQ reset)
{
    s->irq_opaque = opaque;
    s->irq_request = request;
    s->irq_reset = reset;
}

static void sgi_mace_eth_init(Object *obj)
{
    SGIMACEEthState *s = SGI_MACE_ETH(obj);

    memory_region_init_io(&s->mmio, obj, &sgi_mace_eth_ops, s,
                          "sgi-mace.eth", MACE_ETH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                               sgi_mace_eth_rx_expire, s);
    s->mdio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                 sgi_mace_eth_mdio_complete, s);
}

static void sgi_mace_eth_finalize(Object *obj)
{
    SGIMACEEthState *s = SGI_MACE_ETH(obj);

    timer_free(s->mdio_timer);
    timer_free(s->rx_timer);
}

static void sgi_mace_eth_realize(DeviceState *dev, Error **errp)
{
    SGIMACEEthState *s = SGI_MACE_ETH(dev);

    if (!s->dma_mr) {
        error_setg(errp, "sgi-mace-eth requires dma-memory");
        return;
    }
    if (s->phy_ics1890 && s->phy_address > 31) {
        error_setg(errp, "sgi-mace-eth diagnostic ICS1890 requires "
                         "an explicit phy-address from 0 to 31");
        return;
    }
    if (!s->phy_ics1890 && s->phy_address != UINT8_MAX) {
        error_setg(errp, "sgi-mace-eth phy-address requires phy-ics1890=on");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sgi-mace-eth-dma");
    if (s->phy_ics1890) {
        s->phy = ICS1890(qdev_new(TYPE_ICS1890));
        object_property_add_child(OBJECT(dev), "phy", OBJECT(s->phy));
        qdev_prop_set_uint8(DEVICE(s->phy), "addr", s->phy_address);
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->phy), errp)) {
            object_unparent(OBJECT(s->phy));
            s->phy = NULL;
            address_space_destroy(&s->dma_as);
            return;
        }
        ics1890_update_link(s->phy, true);
    }
    s->nic = qemu_new_nic(&sgi_mace_eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    s->flush_bh = qemu_bh_new_guarded(sgi_mace_eth_flush, s,
                                      &dev->mem_reentrancy_guard);
    if (s->phy) {
        ics1890_set_link_ready_notify(s->phy, sgi_mace_eth_phy_ready, s);
    }
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    sgi_mace_eth_set_link(qemu_get_queue(s->nic));
}

static void sgi_mace_eth_unrealize(DeviceState *dev)
{
    SGIMACEEthState *s = SGI_MACE_ETH(dev);

    if (s->phy) {
        ics1890_set_link_ready_notify(s->phy, NULL, NULL);
    }
    qemu_bh_delete(s->flush_bh);
    s->flush_bh = NULL;
    qemu_del_nic(s->nic);
    address_space_destroy(&s->dma_as);
    if (s->phy_ics1890) {
        object_unparent(OBJECT(s->phy));
        s->phy = NULL;
    }
}

static const Property sgi_mace_eth_properties[] = {
    DEFINE_NIC_PROPERTIES(SGIMACEEthState, conf),
    DEFINE_PROP_LINK("dma-memory", SGIMACEEthState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_BOOL("phy-ics1890", SGIMACEEthState, phy_ics1890, false),
    DEFINE_PROP_UINT8("phy-address", SGIMACEEthState, phy_address, UINT8_MAX),
};

static void sgi_mace_eth_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "MACE 2.0 Ethernet MAC";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->realize = sgi_mace_eth_realize;
    dc->unrealize = sgi_mace_eth_unrealize;
    device_class_set_props(dc, sgi_mace_eth_properties);
    dc->vmsd = &vmstate_sgi_mace_eth;
    rc->phases.hold = sgi_mace_eth_reset_hold;
}

static const TypeInfo sgi_mace_eth_info = {
    .name = TYPE_SGI_MACE_ETH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIMACEEthState),
    .instance_init = sgi_mace_eth_init,
    .instance_finalize = sgi_mace_eth_finalize,
    .class_init = sgi_mace_eth_class_init,
};

static void sgi_mace_eth_register_types(void)
{
    type_register_static(&sgi_mace_eth_info);
}

type_init(sgi_mace_eth_register_types)
