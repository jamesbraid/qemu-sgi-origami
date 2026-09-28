/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock processor-interface register and interrupt state. */
#ifndef HW_MISC_SGI_BEDROCK_PI_H
#define HW_MISC_SGI_BEDROCK_PI_H

#include "hw/core/sysbus.h"
#include "hw/mips/sgi/pi-gfx.h"
#include "qemu/notify.h"
#include "qemu/timer.h"

#define TYPE_SGI_BEDROCK_PI "sgi-bedrock-pi"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockPIState, SGI_BEDROCK_PI)

#define SGI_BEDROCK_PI_SIZE 0x30108
#define SGI_BEDROCK_PI_PROCESSORS 2
#define SGI_BEDROCK_PI_HW_IRQS 5

typedef struct SGIBedrockPITimerLine {
    struct SGIBedrockPIState *pi;
    uint8_t cpu;
} SGIBedrockPITimerLine;

struct SGIBedrockPIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq hardware_irq[SGI_BEDROCK_PI_PROCESSORS *
                           SGI_BEDROCK_PI_HW_IRQS];
    qemu_irq nmi[SGI_BEDROCK_PI_PROCESSORS];
    qemu_irq cpu_enable_out[SGI_BEDROCK_PI_PROCESSORS];

    uint32_t interface;
    /*
     * The vCPU index in each of this interface's two slots, -1 when the slot
     * is empty. The CPU number register answers with the reading processor's
     * own slot, so the interface has to be told which processor that is
     * rather than deriving it from the population.
    */
    int32_t slot_cpu_index[SGI_BEDROCK_PI_PROCESSORS];
    uint32_t processor_count;
    uint32_t processor_mask;
    uint8_t cpu_enable_initial;
    uint32_t request_region;
    uint64_t cpu_protect;
    uint64_t io_protect;
    uint64_t region_present;
    uint8_t cached_alias;
    /*
     * Told when the cached-alias size changes, by a write or a load: the
     * node's address decode (sn1-node.c) sizes the alias window from it.
    */
    NotifierList cached_alias_notifiers;
    uint8_t rq_timeout_max;
    uint32_t rq_timeout_scale;
    uint8_t cpu_enable[2];
    uint64_t pending[2];
    uint64_t cc_pend[2];
    uint64_t cc_pend_mask;
    uint64_t mask[2][2];
    uint8_t remap;
    uint64_t error_pending;
    uint64_t error_mask[2];
    uint64_t error_stack[2];
    uint8_t error_stack_size;
    uint64_t error_status[2][2];
    uint64_t nack_count[2];
    uint32_t nack_compare;
    uint8_t spool_mask;
    uint8_t err_check_enable;
    uint64_t spurious_header[2];
    uint64_t rt_base;
    int64_t rt_base_ns;
    uint64_t rt_migrate_count;
    uint16_t rt_migrate_phase_ns;
    uint64_t rt_compare[2];
    uint32_t profile_compare;
    uint8_t rt_pending[2];
    uint8_t profile_pending[2];
    uint8_t rt_enable[2];
    uint8_t profile_enable[2];
    QEMUTimer *rt_timer[2];
    QEMUTimer *profile_timer;
    SGIBedrockPITimerLine rt_line[2];
    bool rt_timer_match[2];
    /*
     * Graphics flow control, per slot, and the credit-mode bit. The
     * owning node counts stores into it and routes returns to it.
     */
    SGIPIGfx gfx;
    uint8_t gfx_credit_mode;
    bool lb_error;
    bool ni_error;
    bool xb_error;
    bool uart_irq;
    /*
     * The owning node, when this device is one of sgi-bedrock's children: a
     * plain pointer, as the I/O interface's is. Real-time compare writes and
     * real-time counter reads are reported to the presence join's partner
     * wait (sgi_bedrock_rt_compare_written(), sgi_bedrock_rt_counter_read());
     * see sgi-bedrock.h. NULL for a standalone device.
    */
    struct SGIBedrockState *bedrock;
};

void sgi_bedrock_pi_set_lb_error(SGIBedrockPIState *s, bool level);
void sgi_bedrock_pi_set_ni_error(SGIBedrockPIState *s, bool level);
void sgi_bedrock_pi_set_xb_error(SGIBedrockPIState *s, bool level);
void sgi_bedrock_pi_set_uart_irq(SGIBedrockPIState *s, bool level);
/*
 * Set one bit a Bedrock unit raises directly in a pending bank, which
 * software then clears through the pending-modify register: the memory unit's
 * correctable-error bit, bank 1 bit 61.
*/
void sgi_bedrock_pi_set_pending(SGIBedrockPIState *s, unsigned bank,
                                unsigned bit);
/*
 * The memory unit found an uncorrectable error: raise the error-interrupt
 * pending register's memory-uncorrectable bit, bit 24, the one bit of that
 * register software cannot set through the writable view and clears by
 * writing it back to the pending register.
*/
void sgi_bedrock_pi_set_memory_uncorrectable(SGIBedrockPIState *s);
void sgi_bedrock_pi_recompute(SGIBedrockPIState *s);
/*
 * An I/O-issued write to the interrupt-pending-modify register from a
 * requester in @region. Applied like a processor write when I/O protect
 * permits the region; otherwise logged and dropped, returning false.
*/
bool sgi_bedrock_pi_io_modify(SGIBedrockPIState *s, uint64_t value,
                              unsigned region);

/* Call notifier whenever the cached-alias size register changes. */
void sgi_bedrock_pi_add_cached_alias_notifier(SGIBedrockPIState *s,
                                              Notifier *notifier);

#endif
