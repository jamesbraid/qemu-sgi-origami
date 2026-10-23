/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_SERIAL_DMA_H
#define HW_MIPS_SGI_MACE_SERIAL_DMA_H

#include "hw/char/serial.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "system/memory.h"

typedef struct MACESerialDMA MACESerialDMA;
typedef struct MACESerialChannel {
    MACESerialDMA *dma;
    unsigned port;
    bool receive;
    uint16_t control;
    uint16_t read;
    uint16_t write;
    uint8_t data[32];
    uint8_t count;
    uint8_t pair;
    bool staged;
    bool fault;
    bool event;
    int64_t delay_remaining;
    int64_t delay_deadline;
    bool delaying;
    QEMUTimer *timer;
} MACESerialChannel;

struct MACESerialDMA {
    MACESerialChannel channel[4];
    SerialState *uart[2];
    AddressSpace *as;
    uint32_t *ring_base;
    void (*update_irq)(void *opaque, uint32_t status);
    void *opaque;
    QEMUBH *bh;
    bool servicing;
    bool loading;
    bool modem_pause[2];
};

void mace_serial_dma_init(MACESerialDMA *s, SerialState *uart,
                          AddressSpace *as, uint32_t *ring_base,
                          void (*update_irq)(void *, uint32_t), void *opaque);
void mace_serial_dma_reset(MACESerialDMA *s);
void mace_serial_dma_cleanup(MACESerialDMA *s);
uint64_t mace_serial_dma_read(MACESerialDMA *s, unsigned port, unsigned reg);
void mace_serial_dma_write(MACESerialDMA *s, unsigned port, unsigned reg,
                           uint64_t value);
bool mace_serial_dma_base_allowed(MACESerialDMA *s, uint32_t value);
void mace_serial_dma_service(MACESerialDMA *s);
void mace_serial_dma_ack(MACESerialDMA *s, uint32_t value);
int mace_serial_dma_pre_save(MACESerialDMA *s);
int mace_serial_dma_post_load(MACESerialDMA *s);
extern const VMStateDescription vmstate_mace_serial_dma;

#endif
