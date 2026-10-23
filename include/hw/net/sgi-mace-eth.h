/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_NET_SGI_MACE_ETH_H
#define HW_NET_SGI_MACE_ETH_H

#include "hw/core/sysbus.h"
#include "hw/net/ics1890.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "net/net.h"

#define TYPE_SGI_MACE_ETH "sgi-mace-eth"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMACEEthState, SGI_MACE_ETH)

struct SGIMACEEthState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint64_t mac_control;
    uint64_t dma_control;
    uint64_t station;
    uint64_t secondary_station;
    uint64_t multicast_mask;
    uint8_t rx_delay;
    uint64_t tx_base;
    uint32_t int_status;
    uint16_t tx_read;
    uint16_t tx_write;
    uint8_t tx_reinit_seen;
    bool tx_stopped;
    uint8_t rx_reinit_seen;
    uint32_t rx_fifo[16];
    uint8_t rx_read;
    uint8_t rx_write;
    uint8_t rx_count;
    uint8_t rx_sequence;
    bool rx_read_generation;
    bool rx_write_generation;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    NICConf conf;
    NICState *nic;
    ICS1890State *phy;
    QEMUTimer *mdio_timer;
    QEMUTimer *rx_timer;
    bool rx_pending;
    bool rx_gather_open;
    uint32_t rx_gather_address;
    uint16_t rx_gather_cursor;
    QEMUBH *flush_bh;
    uint16_t mdio_data;
    uint16_t mdio_address;
    uint16_t mdio_pending_value;
    uint8_t phy_address;
    bool phy_ics1890;
    bool mdio_busy;
    bool mdio_pending_read;
};

#endif
