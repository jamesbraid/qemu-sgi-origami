/*
 * Motorola MCF5206e system integration module and on-chip peripherals.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_M68K_MCF5206E_H
#define HW_M68K_MCF5206E_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/memory.h"
#include "chardev/char-fe.h"
#include "target/m68k/cpu.h"

#define TYPE_MCF5206E_SIM "mcf5206e-sim"
OBJECT_DECLARE_SIMPLE_TYPE(MCF5206ESIMState, MCF5206E_SIM)

#define MCF5206E_NUM_CS     8
#define MCF5206E_SRAM_SIZE  0x2000
#define MCF5206E_MBAR_SIZE  0x400

/* The ICR, IMR and IPR source numbers. */
#define MCF5206E_SRC_SWT    8
#define MCF5206E_SRC_TIMER1 9
#define MCF5206E_SRC_TIMER2 10
#define MCF5206E_SRC_MBUS   11
#define MCF5206E_SRC_UART1  12
#define MCF5206E_SRC_UART2  13
#define MCF5206E_SRC_DMA0   14
#define MCF5206E_SRC_DMA1   15

typedef struct MCF5206ETimer {
    MCF5206ESIMState *sim;
    int src;
    uint16_t tmr;
    uint16_t trr;
    uint16_t tcr;
    uint8_t ter;
    /* The count when the timer was last started or cleared, and when. */
    uint16_t tcn_base;
    int64_t start_ns;
    int64_t next_ns;
    bool running;
    QEMUTimer *timer;
} MCF5206ETimer;

struct MCF5206ESIMState {
    SysBusDevice parent_obj;

    /* Properties. */
    M68kCPU *cpu;
    MemoryRegion *boot_rom;
    uint32_t clock_hz;
    CharFrontend uart_chr[2];

    MemoryRegion mbar_io;
    MemoryRegion sram;
    MemoryRegion cs_window[MCF5206E_NUM_CS];
    MemoryRegion *rom_alias[16];
    /* Global CS0: answers every address until CSMR0 is first written. */
    bool global_cs0;
    MemoryRegion global_io;
    MemoryRegion global_alias[3];
    MemoryRegion default_mem;

    bool mbar_valid;
    bool rambar_valid;

    uint8_t simr;
    uint8_t marb;
    uint8_t icr[16];
    uint16_t imr;
    uint16_t par;
    uint16_t dmcr;
    uint8_t rsr;
    uint8_t sypcr;
    bool sypcr_written;
    uint8_t swivr;
    uint8_t swsr_seq;
    bool wdt_reset_cause;
    QEMUTimer *wdt;

    uint16_t csar[MCF5206E_NUM_CS];
    uint32_t csmr[MCF5206E_NUM_CS];
    uint16_t cscr[MCF5206E_NUM_CS];
    bool csmr_written[MCF5206E_NUM_CS];

    /* Interrupt sources currently asserted, by IPR bit. */
    uint16_t src_state;
    uint8_t ext_in;

    MCF5206ETimer timer[2];
    DeviceState *uart[2];
    uint8_t uivr[2];
    uint8_t uop[2];
};

/** The window a board adds the devices of chip select @n to. */
MemoryRegion *mcf5206e_sim_cs_window(MCF5206ESIMState *s, int n);

#endif
