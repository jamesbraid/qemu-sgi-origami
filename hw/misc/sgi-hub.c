/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC: processor interface, memory/directory controller, I/O
 * interface and network interface behind one 8 MB register window per node.
 * Implements the registers guests touch on the way to a console and reports
 * anything else. See docs/specs/sgi-hub.rst.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2502.h"
#include "hw/1wire/ds2505.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "chardev/char.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "exec/cpu-interrupt.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "qemu/host-utils.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "trace.h"

/* Processor interface, at the base of the window. */
#define HUB_PI_REGION_PRESENT   0x000018  /* which memory regions exist */
#define HUB_PI_PROCESSOR_NUMBER 0x000020  /* which processor is reading */
#define HUB_PI_CALIAS_SIZE      0x000028  /* size of the cached alias */
#define HUB_PI_INT_MODIFY       0x000090  /* the only way to set or clear one */
#define HUB_PI_INT_PEND0        0x000098  /* pending interrupts, first bank */
#define HUB_PI_INT_PEND1        0x0000a0  /* pending interrupts, second bank */
#define HUB_PI_INT_MASK0_A      0x0000a8  /* first bank reaching processor A */
#define HUB_PI_INT_MASK1_A      0x0000b0  /* second bank reaching processor A */
#define HUB_PI_INT_MASK0_B      0x0000b8  /* first bank reaching processor B */
#define HUB_PI_INT_MASK1_B      0x0000c0  /* second bank reaching processor B */

/* I/O interface: the Hub's own widget block on the crossbar. */
#define HUB_IO_WIDGET_ID        0x400000
#define HUB_IO_WIDGET_STATUS    0x400008
#define HUB_IO_WIDGET_CONTROL   0x400020

/* Crosstalk ID layout; the always-one low bit is the crosstalk contract's. */
#define IO_WIDGET_ID_REVISION_SHIFT 28
#define IO_WIDGET_ID_REVISION_MAX   0xf
#define IO_WIDGET_ID_PART       (0xc101ULL << 12)
#define IO_WIDGET_ID_ONE        1ULL
#define IO_WIDGET_ID_MFG_SHIFT  1

/*
 * Widget number resets to 1 (the address map's Hub widget) and the crossbar
 * credit to 2. The tag-mode bit reports hardware and is not writable.
 */
#define IO_WIDGET_CONTROL_NUMBER    1ULL
#define IO_WIDGET_CONTROL_CREDIT    (2ULL << 13)
#define IO_WIDGET_CONTROL_RESET     (IO_WIDGET_CONTROL_NUMBER | \
                                     IO_WIDGET_CONTROL_CREDIT)
#define IO_WIDGET_CONTROL_TAG_MODE  (1ULL << 4)
#define IO_WIDGET_CONTROL_WRITABLE  (0x7fffffULL & ~IO_WIDGET_CONTROL_TAG_MODE)

/* The first registers Linux writes; both must accept a write at reset. */
#define HUB_IO_XTALK_TIMEOUT    0x4003b8
#define HUB_IO_XTALK_PRESCALE   0x4003c0

/* Scratch: a 36-bit word of four sub-fields, firmware's private boot state. */
#define HUB_IO_SCRATCH_0        0x400150
#define HUB_IO_SCRATCH_1        0x400158
#define IO_SCRATCH_MASK         0x0000000f00f11fffULL

/*
 * Incoming crosstalk request buffers: only slots 7-14's C register, the ones
 * firmware's diagnostic sweep reads. They read zero; nothing is in flight.
 */
#define HUB_IO_ICRB_C_BASE      0x4004f0  /* slot 7's C register */
#define HUB_IO_ICRB_C_STRIDE    0x20      /* one slot to the next */
#define HUB_IO_ICRB_C_FIRST     7
#define HUB_IO_ICRB_C_LAST      14

/*
 * Block transfer engines (Origami project tree, docs/datasheets/hub-bte.md):
 * they work from reset gated on nothing, addresses keep bits 39:7 only, and
 * a transfer completes inside the control store that launches it.
 */
#define HUB_BTE_BASE            0x410000  /* engine 0; engine 1 at +0x10000 */
#define HUB_BTE_STRIDE          0x10000
#define HUB_BTE_LENGTH          0x00      /* length/status */
#define HUB_BTE_SOURCE          0x08
#define HUB_BTE_DEST            0x10
#define HUB_BTE_CONTROL         0x18      /* a store launches; a load stops */
#define HUB_BTE_NOTIFY          0x20      /* notification address */
#define HUB_BTE_INTERRUPT       0x28      /* interrupt level and node */

#define BTE_LENGTH_BUSY         (1ULL << 20)
#define BTE_LENGTH_ERROR        (1ULL << 16)
#define BTE_LENGTH_LINES        0xffffULL
#define BTE_ADDRESS_MASK        0x000000ffffffff80ULL  /* bits 39:7 */
#define BTE_LINE_SIZE           128
#define BTE_CONTROL_POISON      (1ULL << 8)
#define BTE_CONTROL_NOTIFY      (1ULL << 4)
#define BTE_CONTROL_ZFILL       (1ULL << 0)
#define BTE_CONTROL_MASK        (BTE_CONTROL_POISON | BTE_CONTROL_NOTIFY | \
                                 BTE_CONTROL_ZFILL)
#define BTE_INT_LEVEL_SHIFT     16
#define BTE_INT_LEVEL_MASK      (0x7fULL << BTE_INT_LEVEL_SHIFT)
#define BTE_INT_NODE_MASK       0x1ffULL

/* Each engine's bit of the I/O error-clear register below. */
#define IO_ERROR_CLEAR_BTE(engine)  (1ULL << (17 + (engine)))

/* 512 KiB per call: the diagnostic's transfer is one call each way. */
#define BTE_COPY_CHUNK_LINES    4096

/* Memory and directory interface. */
#define HUB_MD_INDICATOR_0      0x220050  /* real firmware's diagnostic LED, CPU A */
#define HUB_MD_INDICATOR_1      0x220058  /* same, CPU B */
#define HUB_MD_SN00_INDICATOR_0 0x220080  /* SN00 diagnostic indicator, CPU A */
#define HUB_MD_SN00_INDICATOR_1 0x2200a0  /* same, CPU B */

/*
 * Slot identity and microcontroller status. The boot flash's ready signal
 * appears at two positions that are one signal; both must read ready.
 */
#define HUB_MD_SLOT_ID_USTAT    0x220048
#define MD_SLOT_FLASH_READY_LOW  (1ULL << 0)
#define MD_SLOT_XJMP_ID          (1ULL << 2)
#define MD_SLOT_FLASH_READY_HIGH (1ULL << 4)
#define MD_SLOT_MASTER_MODULE    (1ULL << 7)
#define MD_SLOT_ID_MASK          0x7
#define MD_SLOT_FLASH_READY     (MD_SLOT_FLASH_READY_LOW | \
                                 MD_SLOT_FLASH_READY_HIGH)
#define IO_XTALK_TIMEOUT_MASK   0xffULL
#define IO_XTALK_PRESCALE_MASK  0xffffffULL

/*
 * Memory configuration, masked storage; the non-zero reset value is the fact
 * that matters (Origami project tree, docs/datasheets/hub-memory-directory.md).
 */
#define HUB_MD_CONFIG           0x200018

/* One-wire bus master, 34 bits wide: the upper timing phase is bits 33:27. */
#define HUB_MD_ONEWIRE          0x2000a8
#define MD_ONEWIRE_RXDATA       (1ULL << 0)
#define MD_ONEWIRE_DONE         (1ULL << 1)
#define MD_ONEWIRE_SAMPLE_SHIFT 2
#define MD_ONEWIRE_SAMPLE       (0xffULL << MD_ONEWIRE_SAMPLE_SHIFT)
#define MD_ONEWIRE_PULSE_SHIFT  10
#define MD_ONEWIRE_PULSE        (0x3ffULL << MD_ONEWIRE_PULSE_SHIFT)
#define MD_ONEWIRE_PHASE_LO     (0x7fULL << 20)
#define MD_ONEWIRE_PHASE_HI     (0x7fULL << 27)

/* The completion and received-data bits are the device's to report. */
#define MD_ONEWIRE_WRITABLE     (MD_ONEWIRE_SAMPLE | MD_ONEWIRE_PULSE | \
                                 MD_ONEWIRE_PHASE_LO | MD_ONEWIRE_PHASE_HI)

/*
 * Slot duration, chosen not derived: the encodings' real-time meaning is not
 * established. The phase reset value is the memory/directory datasheet's;
 * the one-wire datasheet does not carry it.
 */
#define MD_ONEWIRE_SLOT_NS      1000

#define MD_ONEWIRE_PHASE_INIT   0x31ULL
#define MD_ONEWIRE_RESET        ((MD_ONEWIRE_PHASE_INIT << 20) | \
                                 (MD_ONEWIRE_PHASE_INIT << 27))

/*
 * The responder: a DS2505 holding the PIMM manufacturing record -- the hub
 * one-wire datasheet's settled contract, and the part the headless bring-up
 * reads as the remote NIC. Family code 0Bh, and a 48-bit synthetic per-unit
 * serial the machine wires. The part is left unprogrammed, so every memory
 * byte reads 0xff and every redirection byte says "not redirected" by the
 * part's own definition: what the record bytes should say is an open
 * question, and a blank part states nothing.
 */
#define MD_ONEWIRE_ROM_FAMILY      0x0b
#define MD_MODULE_ROM_FAMILY       0x09
#define MD_ONEWIRE_SERIAL_MASK     MAKE_64BIT_MASK(0, 48)
#define MD_CONFIG_MASK          0x003fffffffffffffULL  /* 54-bit register */
#define MD_CONFIG_RESET         0x001e7fbc3fffffffULL

/* All eight read zero: firmware's memory test aborts on any valid bit. */
#define HUB_MD_ERROR_BASE       0x200050
#define HUB_MD_ERROR_STRIDE     8
/* The last error slot, MD_MISC_ERROR_CLR. */
#define HUB_MD_ERROR_LAST_OFF   (HUB_MD_ERROR_BASE + 7 * HUB_MD_ERROR_STRIDE)

/* Per-DIMM mode initialisation: write-only in both sources, unstored. */
#define HUB_MD_MEM_DIMM_INIT    0x200090
#define HUB_MD_DIR_DIMM_INIT    0x200098

#define HUB_MD_PERF_CNT0        0x210010
#define MD_PERF_CNT_MASK        0x00000000000fffffULL  /* 20-bit counter */

/* The PCF8584 (hw/i2c/pcf8584.c) is mapped over this window in realize. */
#define HUB_MD_I2C_BASE         0x220000  /* PCF8584, S0 then S1 eight on */

#define ELSC_NVRAM_PAGE_COUNT   8
#define ELSC_NVRAM_PAGE_SIZE    256
#define ELSC_NVRAM_RESERVED_PAGE 7

/*
 * The junk-bus UART: an NS16550 at an eight-byte stride, which belongs to
 * the junk bus, not the chip. A shipped Origin 200 has no such part.
 */
#define HUB_MD_JUNKUART_BASE    0x2200c0
#define HUB_MD_JUNKUART_REGSHIFT 3        /* eight-byte stride, junk-bus rule */
#define HUB_MD_JUNKUART_OSC_HZ  12000000  /* the port's reference crystal */
/* The 16550 wants the oscillator already divided by sixteen. */
#define HUB_MD_JUNKUART_CLOCK   (HUB_MD_JUNKUART_OSC_HZ / 16)

/* Present is hardware-sensed and read-only; enable is firmware's to set. */
#define HUB_PI_CPU_PRESENT_A    0x000040
#define HUB_PI_CPU_PRESENT_B    0x000048
/* Error spool addresses, bits 31:3; firmware parks a reset marker in B's. */
#define HUB_PI_ERR_PENDING      0x000400
#define HUB_PI_ERR_MASK_A       0x000408
#define HUB_PI_ERR_MASK_B       0x000410
#define HUB_PI_ERR_SPOOL_A      0x000418
#define HUB_PI_ERR_SPOOL_B      0x000420
#define HUB_PI_ERR_SPOOL_SIZE   0x000428
#define HUB_PI_ERR_STATUS_BASE  0x000430
#define HUB_PI_ERR_STATUS_LAST  0x000468
#define HUB_PI_ERR_STATUS_STRIDE 8
#define PI_ERR_MASK             0x01ffffffU
#define PI_ERR_SPOOL_MASK       0xfffffff8ULL
#define PI_ERR_SPOOL_SIZE_MASK  0xf
#define PI_ERR_STATUS0_VALID    (1ULL << 63)
#define PI_ERR_STATUS0_ADDRESS  (0x480041ULL << 25)

/* Timed-out request buffers: nothing times out here, so both read zero. */
#define HUB_PI_TIMEDOUT_BUF_A   0x000480
#define HUB_PI_TIMEDOUT_BUF_B   0x000488

#define HUB_PI_CPU_ENABLE_A     0x000050
#define HUB_PI_CPU_ENABLE_B     0x000058

/*
 * Real-time counter: 52 bits counting the clock the machine connects, read
 * off the virtual clock so it ticks between two reads a few instructions
 * apart, which Linux's arming depends on. A write moves the origin, not the
 * count.
 */
#define HUB_PI_RT_COUNT         0x030100
#define HUB_PI_RT_COMPARE_A     0x000108
#define HUB_PI_RT_COMPARE_B     0x000110
#define HUB_PI_RT_PENDING_A     0x000120
#define HUB_PI_RT_PENDING_B     0x000128
#define HUB_PI_RT_ENABLE_A      0x000140
#define HUB_PI_RT_ENABLE_B      0x000148
/*
 * The rate a machine supplies when it connects nothing, which every machine
 * in-tree overrides; the datasheet's three supports are for this number.
 */
#define PI_RT_DEFAULT_HZ        1250000ULL
#define PI_RT_MASK              (((uint64_t)1 << 52) - 1)

/* Clock control: bit 16 resets set; the network-clock bits read zero. */
#define HUB_PI_RT_LOCAL_CTRL    0x000160
#define PI_RT_LOCAL_CTRL_MASK   0x100ffULL
#define PI_RT_LOCAL_CTRL_RESET  0x10000ULL

/* I/O interface: the crossbar link. */
#define HUB_IO_LINK_STATUS      0x400128
#define HUB_IO_LLP_LOG          0x400130
#define HUB_IO_ERROR_CLEAR      0x4001f8

/*
 * All read zero, nothing being in flight. A guest polls the deallocation
 * registers for a written pending bit to clear, so those must.
 */
#define HUB_IO_PIO_DEALLOC      0x400388
#define HUB_IO_CRB_DEALLOC      0x400390
#define HUB_IO_IOQ_FIFO_DEPTH   0x400398
#define HUB_IO_CRB_MGMT         0x4003a8
#define HUB_IO_CRB_CONTROL      0x4003b0

/* Link state: 0 failed, 1 down, 2 and 3 up; Linux tests the high bit. */
#define IO_LINK_STATE_SHIFT     12
#define IO_LINK_STATE_UP        (2ULL << IO_LINK_STATE_SHIFT)
#define IO_LINK_STATE_MASK      (3ULL << IO_LINK_STATE_SHIFT)

#define IO_LINK_MAX_BURST       (0x3fcULL << 32)
#define IO_LINK_MAX_RETRY       (0x3ffULL << 16)
#define IO_LINK_NULL_TIMEOUT    0xaULL

/* A control firmware operates (hubii_llp_enable()), so unset at reset. */
#define IO_LINK_ENABLED         (1ULL << 10)

#define IO_LINK_RESET           (IO_LINK_MAX_BURST | IO_LINK_MAX_RETRY | \
                                 IO_LINK_STATE_UP | IO_LINK_NULL_TIMEOUT)

/* The link state and mode stay read-only: nothing may claim the link down. */
#define IO_LINK_WARM_RESET      (1ULL << 8)
#define IO_LINK_WRITABLE        (IO_LINK_MAX_BURST | IO_LINK_MAX_RETRY | \
                                 IO_LINK_WARM_RESET | IO_LINK_ENABLED | \
                                 0x3fULL)

/* Big-window translation table, entry n at 8n. */
#define HUB_IO_BWIN_TABLE       0x400160
#define HUB_IO_BWIN_STRIDE      8

/* Thirteen bits and no valid bit: disable by naming an absent widget. */
#define IO_BWIN_MASK            0x1fff
#define IO_BWIN_SPACE           (1U << 12) /* set selects I/O space */
#define IO_BWIN_WIDGET_SHIFT    8
#define IO_BWIN_WIDGET_MASK     (0xfU << IO_BWIN_WIDGET_SHIFT)
#define IO_BWIN_SLICE_MASK      0x1f       /* crosstalk address bits 33:29 */
#define IO_BWIN_SLICE_SHIFT     29

/*
 * Network interface (Origami project tree,
 * docs/datasheets/hub-network-interface.md).
 */
#define HUB_NI_STATUS           0x600000  /* status, revision and identity */
#define HUB_NI_SCRATCH_0        0x600100
#define HUB_NI_SCRATCH_1        0x600108
#define HUB_NI_VECTOR_PARMS     0x600200
#define HUB_NI_VECTOR           0x600208
#define HUB_NI_VECTOR_DATA      0x600210
#define HUB_NI_VECTOR_STATUS    0x600300
#define HUB_NI_RETURN_VECTOR    0x600308
#define HUB_NI_VECTOR_READ_DATA 0x600310
#define HUB_NI_VECTOR_CLEAR     0x600380
#define HUB_NI_META_TABLE       0x638000
#define HUB_NI_LOCAL_TABLE      0x638100
#define NI_ROUTE_STRIDE         8
#define NI_ROUTE_MASK           0xf

#define NI_VECTOR_PIOID_MASK    (0x3ffULL << 40)
#define NI_VECTOR_WRITEID_MASK  (0xffULL << 32)
#define NI_VECTOR_ADDRESS_MASK  0xffff8ULL
#define NI_VECTOR_TYPE_MASK     0x3ULL

#define NI_VECTOR_VALID         (1ULL << 63)
#define NI_VECTOR_TARGET_SHIFT  51
#define NI_VECTOR_STATUS_ADDR_MASK 0xfffffff8ULL

#define NI_VECTOR_READ          0
#define NI_VECTOR_WRITE         1
#define NI_VECTOR_EXCHANGE      3
#define NI_VECTOR_ADDRESS_ERROR 4

/*
 * Port reset: bit 0 resets this Hub, tested alone because the three writers
 * set different companion bits. The protection register is not consulted;
 * only OpenBSD arms it.
 */
#define HUB_NI_PORT_RESET       0x600008
#define NI_PORT_RESET_LOCAL     (1ULL << 0)  /* resets this Hub */

/*
 * Protection configuration: one bit that permits reset, resetting
 * permissive. Storage the model never consults -- the port-reset path
 * must not require it armed, because Linux never arms it -- stored
 * because OpenBSD arms it before rebooting.
 */
#define HUB_NI_PROTECTION       0x600010
#define NI_PROTECTION_MASK      0x1ULL
#define NI_PROTECTION_RESET     0x1ULL

/*
 * Diagnostic parameters: nineteen bits of link fault injection and
 * tuning -- a port-timeout reset, forced eight-bit mode, port disable,
 * deliberate data errors. The routed fabric consumes the data-error bit;
 * the remaining controls are stored and never acted on. The
 * network-interface datasheet defines no reset value for the
 * port-timeout bit and no nonzero reset for the rest; zero at reset
 * is the model's stated choice, the port-error precedent.
 */
#define HUB_NI_DIAG_PARMS       0x600110
#define NI_DIAG_PARMS_MASK      0x7ffffULL
#define NI_DIAG_SEND_DATA_ERROR (1ULL << 0)

/*
 * PIO protection: region-permission storage this model never consults
 * -- one bit per region, permissive at reset -- and its write-only
 * override, whose only documented semantic is that writing the
 * password "SGIrules" unlocks the protection. The Origami project
 * tree's network-interface datasheet ("Reset and protection" in
 * docs/datasheets/hub-network-interface.md there) carries both rows.
 */
#define HUB_NI_IO_PROTECT       0x600400
#define HUB_NI_IO_PROT_OVRRD    0x600408
#define NI_IO_PROTECT_RESET     (~0ULL)
#define NI_PROT_OVRRD_PASSWORD  0x53474972756c6573ULL  /* "SGIrules" */

/*
 * The LLP port registers. The parameters register is sixteen bits of
 * storage -- tuning for a link this machine does not model -- whose
 * reset value carries the null-packet timeout in bits 15:10 and the
 * maximum burst in bits 9:0. The routed fabric latches diagnostic traffic
 * errors in the error register; its clearing alias returns and clears them.
 * The Origami project tree's
 * network-interface datasheet (docs/datasheets/hub-network-interface.md
 * there) carries the field accounts and the choice's justification.
 */
#define HUB_NI_PORT_PARMS       0x608000
#define HUB_NI_PORT_ERROR       0x608008
#define HUB_NI_PORT_ERROR_CLEAR 0x608088
#define NI_PORT_PARMS_MASK      0xffffULL
#define NI_PORT_PARMS_RESET     ((6ULL << 10) | 0x3f0)
#define NI_PORT_ERROR_MASK      0x3fffffffffULL
#define NI_PORT_CHECK_ERROR_SHIFT 8
#define NI_PORT_ERROR_COUNT_MASK 0xffULL

/*
 * Status: M mode, link-up per the product's wiring, the node field programmed
 * by firmware (or the nasid property), region-size resetting set.
 */
#define NI_STATUS_MORE_NODES    (1ULL << 18)
#define NI_STATUS_LINK_UP       (1ULL << 29)
#define NI_STATUS_REGION_SIZE   (1ULL << 17)
#define NI_STATUS_NODE_SHIFT    8
#define NI_STATUS_NODE_MASK     (0x1ffULL << NI_STATUS_NODE_SHIFT)
#define NI_STATUS_REVISION_SHIFT 4
#define NI_STATUS_REVISION_MAX  0xf

#define NI_STATUS_RESET         NI_STATUS_REGION_SIZE
#define NI_STATUS_WRITABLE      (NI_STATUS_MORE_NODES | \
                                 NI_STATUS_REGION_SIZE | \
                                 NI_STATUS_NODE_MASK)

uint32_t sgi_hub_programmed_nasid(const SGIHubState *s)
{
    return (s->ni_status & NI_STATUS_NODE_MASK) >> NI_STATUS_NODE_SHIFT;
}

bool sgi_hub_sends_data_error(const SGIHubState *s)
{
    return s->ni_diag_parms & NI_DIAG_SEND_DATA_ERROR;
}

void sgi_hub_latch_port_error(SGIHubState *s, uint64_t errors)
{
    s->ni_port_error |= errors & NI_PORT_ERROR_MASK;
}

void sgi_hub_record_port_check_error(SGIHubState *s)
{
    uint64_t count = extract64(s->ni_port_error,
                               NI_PORT_CHECK_ERROR_SHIFT, 8);

    if (count != NI_PORT_ERROR_COUNT_MASK) {
        s->ni_port_error = deposit64(s->ni_port_error,
                                     NI_PORT_CHECK_ERROR_SHIFT, 8,
                                     count + 1);
    }
}


/*
 * Interrupt modify: seven bits of number, the top bit selecting the bank, and
 * a set bit. The only way to change a pending bit; the Bridge writes here.
 */
#define PI_INT_MODIFY_NUMBER    0x7f
#define PI_INT_MODIFY_SET       0x100

/*
 * The cached-alias size is a four-bit code, and resets to the largest.
 * The reset value covers only the pre-store window: the real PROM
 * stores the 4 KiB code in its earliest per-processor bring-up, before
 * anything cross-node, and never reads the register back.
 */
#define PI_CALIAS_SIZE_MASK     0xf
#define PI_CALIAS_SIZE_RESET    15

/*
 * One descriptor per decoded doubleword; the ranged families keep their own
 * predicates ahead of the walk. Peek and store are the side-effect-free
 * halves (diagnostic shadow, vector PIO, the 32-bit merge); the read and
 * write hooks carry the guest-visible side effects.
 */
enum {
    HUB_REG_STORE       = 1 << 0, /* a plain write stores val & wmask */
    HUB_REG_DIAG        = 1 << 1, /* the diagnostic shadow reads it */
    HUB_REG_VECTOR      = 1 << 2, /* a remote vector PIO reaches it */
    HUB_REG_TRACE_READ  = 1 << 3, /* in the protocol-read trace set */
    HUB_REG_TRACE_WRITE = 1 << 4, /* in the protocol-write trace set */
    HUB_REG_READ_CLEARS = 1 << 5, /* a full-width read has side effects */
    HUB_REG_WRITE_CLEARS = 1 << 6, /* a write clears each asserted bit */
    HUB_REG_TRACE_COORD = 1 << 7, /* in the coordination trace set */
    HUB_REG_TRACE_PENDING = 1 << 8, /* its reads go to the pending trace */
};

typedef struct SGIHubRegDef {
    hwaddr offset;
    unsigned arg;               /* which of an A/B pair or bank the hooks get */
    unsigned flags;
    size_t field;               /* backing state, when width is nonzero */
    unsigned width;             /* sizeof that state: 1, 4 or 8; 0 = none */
    uint64_t wmask;             /* what a HUB_REG_STORE write keeps */
    uint64_t (*read)(SGIHubState *s, hwaddr addr, unsigned arg);
    uint64_t (*peek)(SGIHubState *s, unsigned arg);
    void (*store)(SGIHubState *s, unsigned arg, uint64_t val);
    void (*write)(SGIHubState *s, hwaddr addr, unsigned arg, uint64_t val,
                  unsigned size);
} SGIHubRegDef;

static const SGIHubRegDef *sgi_hub_reg_lookup(hwaddr addr);

static uint64_t sgi_hub_reg_peek(SGIHubState *s, const SGIHubRegDef *reg)
{
    const void *p = (const char *)s + reg->field;

    if (reg->peek) {
        return reg->peek(s, reg->arg);
    }
    switch (reg->width) {
    case 1:
        return *(const uint8_t *)p;
    case 4:
        return *(const uint32_t *)p;
    default:
        return *(const uint64_t *)p;
    }
}

static bool sgi_hub_reg_peekable(const SGIHubRegDef *reg)
{
    return reg->peek || reg->width;
}

static bool sgi_hub_reg_storable(const SGIHubRegDef *reg)
{
    return reg->store || ((reg->flags & HUB_REG_STORE) && reg->width);
}

static void sgi_hub_reg_store(SGIHubState *s, const SGIHubRegDef *reg,
                              uint64_t val)
{
    void *p = (char *)s + reg->field;

    if (reg->store) {
        reg->store(s, reg->arg, val);
        return;
    }
    val &= reg->wmask;
    switch (reg->width) {
    case 1:
        *(uint8_t *)p = val;
        break;
    case 4:
        *(uint32_t *)p = val;
        break;
    default:
        *(uint64_t *)p = val;
        break;
    }
}

/*
 * Rendezvous-register protocol traces, separate from the generic MMIO trace
 * and tested on the access's aligned register.
 */
static void sgi_hub_trace_protocol_write(SGIHubState *s, hwaddr addr,
                                         uint64_t value, unsigned int size)
{
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(addr & ~(hwaddr)7);
    CPUState *cpu;

    if (!reg || !(reg->flags & HUB_REG_TRACE_WRITE)) {
        return;
    }

    cpu = current_cpu;
    trace_sgi_hub_protocol_write(
        cpu ? cpu->cpu_index : -1, s->cpu_base,
        sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
        addr, value, size, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void sgi_hub_trace_protocol_read(SGIHubState *s, hwaddr addr,
                                        uint64_t value, unsigned int size)
{
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(addr & ~(hwaddr)7);
    CPUState *cpu;

    if (!reg || !(reg->flags & HUB_REG_TRACE_READ)) {
        return;
    }

    cpu = current_cpu;
    trace_sgi_hub_protocol_read(
        cpu ? cpu->cpu_index : -1, s->cpu_base,
        sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
        addr, value, size, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/* Which translation entry an address is, if it is one. */
static bool sgi_hub_bwin_entry(hwaddr addr, unsigned int *n)
{
    hwaddr off = addr - HUB_IO_BWIN_TABLE;

    if (addr < HUB_IO_BWIN_TABLE ||
        off >= SGI_HUB_BWIN_ENTRIES * HUB_IO_BWIN_STRIDE ||
        off % HUB_IO_BWIN_STRIDE) {
        return false;
    }

    *n = off / HUB_IO_BWIN_STRIDE;
    return true;
}

/* Slots 7-14's C register only; the base check keeps the subtraction safe. */
static bool sgi_hub_icrb_c_entry(hwaddr addr)
{
    hwaddr off;
    unsigned int slot;

    if (addr < HUB_IO_ICRB_C_BASE) {
        return false;
    }

    off = addr - HUB_IO_ICRB_C_BASE;
    if (off % HUB_IO_ICRB_C_STRIDE) {
        return false;
    }

    slot = HUB_IO_ICRB_C_FIRST + off / HUB_IO_ICRB_C_STRIDE;
    return slot <= HUB_IO_ICRB_C_LAST;
}

/* Which engine's register an address is: six aligned doublewords per block. */
static bool sgi_hub_bte_entry(hwaddr addr, unsigned int *engine, hwaddr *reg)
{
    hwaddr off;

    if (addr < HUB_BTE_BASE) {
        return false;
    }

    off = addr - HUB_BTE_BASE;
    *engine = off / HUB_BTE_STRIDE;
    *reg = off % HUB_BTE_STRIDE;
    return *engine < SGI_HUB_BTES && *reg <= HUB_BTE_INTERRUPT &&
           (*reg % 8) == 0;
}

/* The eight error-cluster registers, all answering zero. */
static bool sgi_hub_md_error_entry(hwaddr addr)
{
    hwaddr off;

    if (addr < HUB_MD_ERROR_BASE || addr > HUB_MD_ERROR_LAST_OFF) {
        return false;
    }

    off = addr - HUB_MD_ERROR_BASE;
    return (off % HUB_MD_ERROR_STRIDE) == 0;
}

/*
 * Aim a window where its entry says. A window naming an absent widget lands
 * over empty crosstalk space and must not fault: guests disable them so.
 */
static void sgi_hub_bwin_follow(SGIHubState *s, unsigned int n)
{
    uint32_t entry = s->io_bwin[n];
    uint64_t widget = (entry & IO_BWIN_WIDGET_MASK) >> IO_BWIN_WIDGET_SHIFT;
    uint64_t slice = (uint64_t)(entry & IO_BWIN_SLICE_MASK)
                     << IO_BWIN_SLICE_SHIFT;

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->bwin_window[n], false);
    if (s->xtalk_io) {
        memory_region_set_enabled(&s->bwin_io_window[n], false);
    }
    if (entry & IO_BWIN_SPACE) {
        if (!s->xtalk_io || n != 0 || slice) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-hub: unsupported I/O big window %u, widget %"
                          PRIu64 ", slice 0x%" PRIx64 "\n", n, widget, slice);
        } else {
            memory_region_set_alias_offset(&s->bwin_io_window[n],
                                           widget << s->xtalk_widget_shift);
            memory_region_set_enabled(&s->bwin_io_window[n], true);
        }
    } else {
        memory_region_set_alias_offset(&s->bwin_window[n],
                                       (widget << s->xtalk_widget_shift) |
                                       slice);
        memory_region_set_enabled(&s->bwin_window[n], true);
    }
    memory_region_transaction_commit();
}

/* Floor beneath the windows: zero is a choice, no source says what is real. */
static uint64_t sgi_hub_bwin_empty_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: read of %u bytes through big window %u at offset "
                  "0x%" HWADDR_PRIx ", which reaches no widget\n", size,
                  (unsigned)(addr / SGI_HUB_BWIN_SIZE),
                  (hwaddr)(addr % SGI_HUB_BWIN_SIZE));
    return 0;
}

static void sgi_hub_bwin_empty_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " through big window %u at "
                  "offset 0x%" HWADDR_PRIx ", which reaches no widget\n", val,
                  (unsigned)(addr / SGI_HUB_BWIN_SIZE),
                  (hwaddr)(addr % SGI_HUB_BWIN_SIZE));
}

static const MemoryRegionOps sgi_hub_bwin_empty_ops = {
    .read = sgi_hub_bwin_empty_read,
    .write = sgi_hub_bwin_empty_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * The floor beneath the claim aliases: a node-qualified access whose
 * number neither Hub has programmed fails, and is never served by a Hub
 * the number does not name. The error, not a timeout, is the datasheet's
 * conservative shape for an unobserved fault; discovery only needs the
 * probe not to complete normally.
 */
static MemTxResult sgi_hub_claim_error_read(void *opaque, hwaddr addr,
                                            uint64_t *val, unsigned size,
                                            MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-hub: read of %u bytes at offset 0x%" HWADDR_PRIx
                  " in a node-qualified stride no Hub claims\n", size, addr);
    *val = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult sgi_hub_claim_error_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-hub: write of 0x%" PRIx64 " at offset 0x%" HWADDR_PRIx
                  " in a node-qualified stride no Hub claims\n", val, addr);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps sgi_hub_claim_error_ops = {
    .read_with_attrs = sgi_hub_claim_error_read,
    .write_with_attrs = sgi_hub_claim_error_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

/* The claim views this Hub fills: each populated processor's and its own. */
static bool sgi_hub_claim_view_present(const SGIHubState *s, unsigned int view)
{
    return view == SGI_HUB_DEVICE_VIEW || view < s->cpus;
}

/*
 * Aim this Hub's claim aliases by the fabric's map: for every endpoint the
 * map reaches, one alias per view and space sits at the stride of the node
 * number that endpoint holds and resolves into the endpoint's own stride in
 * the canonical map; every other alias is disabled, so its stride falls to
 * the error floor. The fabric keeps own number first: while two Hubs both
 * hold the reset value zero, each keeps zero for itself.
 */
static void sgi_hub_claim_apply(SGIHubState *s)
{
    unsigned int endpoints, view, space, i;
    const int16_t *map;

    if (!s->claim_ready) {
        return;
    }
    map = sn0_fabric_map(s->fabric, s->fabric_endpoint);
    endpoints = sn0_fabric_endpoint_count(s->fabric);

    memory_region_transaction_begin();
    for (view = 0; view < SGI_HUB_CLAIM_VIEWS; view++) {
        if (!sgi_hub_claim_view_present(s, view)) {
            continue;
        }
        for (space = 0; space < SGI_HUB_CLAIM_SPACES; space++) {
            for (i = 0; i < endpoints; i++) {
                MemoryRegion *alias = &s->claim_alias[view][space][i];
                uint32_t number = sn0_fabric_endpoint_number(s->fabric, i);
                bool reachable = number < SN0_FABRIC_NUMBERS &&
                                 map[number] == (int16_t)i;

                if (reachable) {
                    memory_region_set_address(alias,
                        ((hwaddr)space << s->claim_space_shift) |
                        ((hwaddr)number << s->claim_node_shift));
                    memory_region_set_alias_offset(alias,
                        ((hwaddr)space << s->claim_space_shift) |
                        ((hwaddr)sn0_fabric_endpoint_stride(s->fabric, i)
                         << s->claim_node_shift));
                }
                memory_region_set_enabled(alias, reachable);
            }
        }
    }
    memory_region_transaction_commit();
}

/*
 * The status register's link-up bit follows the fabric, the one source of
 * link state; it is read-only to the guest and set again at every reset.
 */
static void sgi_hub_link_refresh(SGIHubState *s)
{
    bool up = s->fabric && sn0_fabric_link_up(s->fabric, s->fabric_endpoint);

    s->ni_status = (s->ni_status & ~NI_STATUS_LINK_UP) |
                   (up ? NI_STATUS_LINK_UP : 0);
}

/* The fabric's map or link state for this endpoint moved: follow it. */
void sgi_hub_fabric_changed(SGIHubState *s)
{
    sgi_hub_link_refresh(s);
    sgi_hub_claim_apply(s);
}

/*
 * The public size table: zero disables the window, one is 4 KiB, and
 * every further code doubles it, up to 64 MiB at fifteen.
 */
static uint64_t sgi_hub_calias_bytes(uint32_t code)
{
    return code ? 4 * KiB << (code - 1) : 0;
}

/*
 * Size the nameless low memory windows to the programmed cached-alias
 * size: within the window a processor's node-field-zero memory access
 * stays on its own node; beyond it the access falls through to the
 * claim layer, which routes stride zero by programmed number. One
 * register serves both processors' views (the PAM idiom again: the
 * machine pre-creates each window at the largest encodable size, and a
 * store, reset or load only resizes and gates it).
 */
static void sgi_hub_calias_update(SGIHubState *s)
{
    uint64_t size = sgi_hub_calias_bytes(s->pi_calias_size);
    unsigned int view;

    memory_region_transaction_begin();
    for (view = 0; view < s->cpus; view++) {
        MemoryRegion *window = s->calias_window[view];

        if (!window) {
            continue;
        }
        if (size) {
            memory_region_set_size(window, size);
        }
        memory_region_set_enabled(window, size != 0);
    }
    memory_region_transaction_commit();
}

/*
 * The programmed number moved, by a store, a reset or a load: tell the
 * fabric, which recomputes every endpoint's map and calls back each Hub
 * whose map changed, this one included.
 */
static void sgi_hub_report_number(SGIHubState *s)
{
    if (s->fabric) {
        sn0_fabric_report_number(s->fabric, s->fabric_endpoint,
                                 sgi_hub_programmed_nasid(s));
    }
}

/* Level outputs, recomputed on every change: raise-on-set becomes a storm. */
static void sgi_hub_update_irq(SGIHubState *s)
{
    unsigned int bank;
    unsigned int cpu;

    for (cpu = 0; cpu < SGI_HUB_PROCESSORS; cpu++) {
        for (bank = 0; bank < SGI_HUB_PENDING_BANKS; bank++) {
            qemu_set_irq(s->bank_irq[bank][cpu],
                         !!(s->pi_int_pend[bank] &
                            s->pi_int_mask[cpu * SGI_HUB_PENDING_BANKS +
                                           bank]));
        }
        qemu_set_irq(s->rt_irq[cpu], s->pi_rt_pending[cpu] &&
                                     s->pi_rt_enable[cpu]);
    }
}

static void sgi_hub_cpu_unhalt(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    cs->halted = 0;
}

static void sgi_hub_cpu_halt(CPUState *cs, run_on_cpu_data data)
{
    cpu_interrupt(cs, CPU_INTERRUPT_HALT);
}

/*
 * A run-loop exit the Hub forces on a processor, whether a bare kick or
 * the one queued work brings: who kicked (-1 for no processor), whom, why.
 */
static void sgi_hub_trace_kick(CPUState *target, const char *reason)
{
    if (trace_event_get_state_backends(TRACE_SGI_HUB_KICK)) {
        trace_sgi_hub_kick(current_cpu ? current_cpu->cpu_index : -1,
                           target->cpu_index, reason);
    }
}

static void sgi_hub_queue_cpu_enable(SGIHubState *s, unsigned int cpu,
                                     bool enabled, bool synchronous)
{
    CPUState *cs = qemu_get_cpu(s->cpu_base + cpu);

    if (!cs) {
        return;
    }
    sgi_hub_trace_kick(cs, enabled ? "cpu-enable" : "cpu-disable");
    if (synchronous) {
        run_on_cpu(cs, enabled ? sgi_hub_cpu_unhalt : sgi_hub_cpu_halt,
                   RUN_ON_CPU_NULL);
        return;
    }
    if (enabled) {
        async_safe_run_on_cpu(cs, sgi_hub_cpu_unhalt, RUN_ON_CPU_NULL);
    } else {
        async_run_on_cpu(cs, sgi_hub_cpu_halt, RUN_ON_CPU_NULL);
    }
}

void sgi_hub_set_cpu_enable(SGIHubState *s, unsigned int cpu, bool enabled,
                            bool synchronous)
{
    s->pi_cpu_enable[cpu] = enabled;
    sgi_hub_queue_cpu_enable(s, cpu, enabled, synchronous);
}

static void sgi_hub_write_cpu_enable(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    bool enabled = val & 1;

    sgi_hub_set_cpu_enable(s, arg, enabled, false);
}

/* Wake the other processor without changing Hub state. */
static void sgi_hub_kick_peer(CPUState *peer, const char *reason)
{
    if (!peer || peer == current_cpu) {
        return;
    }

    sgi_hub_trace_kick(peer, reason);
    qemu_cpu_kick(peer);
}

/*
 * The read assists hand a polling reader's local peer a slice. They exist
 * for single-threaded round-robin execution, where one host slice serves
 * every processor and a bounded guest poll can expire before the peer it
 * waits for has run at all. Under multi-threaded execution that peer holds
 * its own host thread and is already running, so the kick releases nobody
 * and costs the peer a run-loop exit on every poll. Write-side phase
 * changes still kick unconditionally: those are rare, and warm-reset
 * discovery depends on them.
 */
static void sgi_hub_kick_polled_peer(CPUState *peer, const char *reason)
{
    /*
     * Single-threaded round-robin TCG is the only mode that hands slices
     * around, so it is the only one the assist can help. Asking
     * tcg_enabled() first also keeps the accelerator query out of a build
     * configured without TCG, where it has no definition to link against.
     */
    if (!tcg_enabled() || qemu_tcg_mttcg_enabled()) {
        return;
    }

    sgi_hub_kick_peer(peer, reason);
}

/* Slice B's PEND1 credit is about to be set or spent; was it held? */
static void sgi_hub_trace_pend1_credit(SGIHubState *s, const char *what)
{
    if (trace_event_get_state_backends(TRACE_SGI_HUB_PEND1_CREDIT)) {
        CPUState *cpu = current_cpu;

        trace_sgi_hub_pend1_credit(cpu ? cpu->cpu_index : -1, s->cpu_base,
                                   sgi_hub_programmed_nasid(s), what,
                                   s->pi_int_pend1_primary_credit,
                                   cpu ? cpu->cc->get_pc(cpu) : 0);
    }
}

static void sgi_hub_int_modify(SGIHubState *s, uint64_t val)
{
    unsigned int number = val & PI_INT_MODIFY_NUMBER;
    unsigned int bank = number / SGI_HUB_BANK_BITS;
    unsigned int i;
    CPUState *primary = qemu_get_cpu(s->cpu_base);
    uint64_t bit = 1ULL << (number % SGI_HUB_BANK_BITS);

    if (val & PI_INT_MODIFY_SET) {
        s->pi_int_pend[bank] |= bit;
    } else {
        s->pi_int_pend[bank] &= ~bit;
    }
    sgi_hub_update_irq(s);

    /* Credit slice B a PEND1 snapshot before kicking it: the PROM's bit 113. */
    if (bank == 1 && number == 113 && primary && current_cpu == primary) {
        sgi_hub_trace_pend1_credit(s, "set");
        s->pi_int_pend1_primary_credit = true;
    }

    /* The PROM uses pending bits as barrier mailboxes; release the pollers. */
    for (i = 0; i < s->cpus; i++) {
        sgi_hub_kick_peer(qemu_get_cpu(s->cpu_base + i), "int-modify");
    }
}

static uint64_t sgi_hub_int_pending_read(SGIHubState *s, unsigned int bank);

static uint64_t sgi_hub_int_pend1_read(SGIHubState *s)
{
    CPUState *primary = qemu_get_cpu(s->cpu_base);
    CPUState *secondary = qemu_get_cpu(s->cpu_base + 1);
    uint64_t value = s->pi_int_pend[1];

    if (s->cpus >= 2 && primary && secondary && current_cpu == secondary &&
        (value & (1ULL << (113 - SGI_HUB_BANK_BITS)))) {
        if (s->pi_int_pend1_primary_credit) {
            sgi_hub_trace_pend1_credit(s, "consumed");
            s->pi_int_pend1_primary_credit = false;
        } else {
            sgi_hub_kick_polled_peer(primary, "pend1-poll");
        }
        return value;
    }

    return sgi_hub_int_pending_read(s, 1);
}

static void sgi_hub_kick_remote_scratch_node(SGIHubState *s)
{
    unsigned int i;

    if (!current_cpu ||
        (current_cpu->cpu_index >= s->cpu_base &&
         current_cpu->cpu_index < s->cpu_base + s->cpus)) {
        return;
    }

    /* A remote scratch write is a discovery phase change; release the node. */
    for (i = 0; i < s->cpus; i++) {
        sgi_hub_kick_peer(qemu_get_cpu(s->cpu_base + i), "remote-scratch");
    }
}

/*
 * Only an unchanged re-read kicks the peers: a read whose value moved is an
 * answered poll. Keyed on neither the bank's value nor the masks; both name
 * the wrong reads.
 */
static uint64_t sgi_hub_int_pending_read(SGIHubState *s, unsigned int bank)
{
    uint64_t value = s->pi_int_pend[bank];
    unsigned int i;

    if (!current_cpu || current_cpu->cpu_index < s->cpu_base ||
        current_cpu->cpu_index >= s->cpu_base + s->cpus) {
        return value;
    }

    i = current_cpu->cpu_index - s->cpu_base;
    if (s->pi_int_pend_seen_valid[i][bank] &&
        s->pi_int_pend_seen[i][bank] == value) {
        for (i = 0; i < s->cpus; i++) {
            sgi_hub_kick_polled_peer(qemu_get_cpu(s->cpu_base + i),
                                     "pending-reread");
        }
        return value;
    }

    s->pi_int_pend_seen[i][bank] = value;
    s->pi_int_pend_seen_valid[i][bank] = true;
    return value;
}

/* Bits above 39 drop silently, as firmware depends on; bits 6:0 are logged. */
static uint64_t sgi_hub_bte_address(unsigned int engine, const char *what,
                                    uint64_t val)
{
    if (val & (BTE_LINE_SIZE - 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: BTE%u %s address 0x%" PRIx64 " is not "
                      "128-byte aligned; bits 6:0 are not stored\n",
                      engine, what, val);
    }
    return val & BTE_ADDRESS_MASK;
}

/* Run the armed transfer inside the launching store, through the node view. */
static void sgi_hub_bte_launch(SGIHubState *s, unsigned int engine)
{
    uint64_t lines = s->bte_length[engine] & BTE_LENGTH_LINES;
    uint64_t src = s->bte_source[engine];
    uint64_t dst = s->bte_dest[engine];
    uint64_t control = s->bte_control[engine];
    g_autofree uint8_t *buf = NULL;
    uint64_t chunk;
    uint64_t done;

    trace_sgi_hub_bte_launch(engine, control, src, dst, lines);

    if (control & BTE_CONTROL_POISON) {
        /* The poisoned directory state belongs to a directory not modelled. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: BTE%u launched in poison mode; the copy "
                      "runs but no directory state is poisoned\n", engine);
    }
    if (lines == 0) {
        /* Open on hardware; completing a zero-line transfer is a choice. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: BTE%u launched with a zero length\n", engine);
    }

    /*
     * Chunked through one bounce buffer; a failed chunk is retried line by
     * line to latch the first unmoved line, and overlapping ranges go line
     * by line to keep the engine's forward propagation.
     */
    chunk = BTE_COPY_CHUNK_LINES;
    if (!(control & BTE_CONTROL_ZFILL) &&
        src < dst + lines * BTE_LINE_SIZE &&
        dst < src + lines * BTE_LINE_SIZE) {
        chunk = 1;
    }
    if (lines) {
        buf = g_malloc(MIN(chunk, lines) * BTE_LINE_SIZE);
        if (control & BTE_CONTROL_ZFILL) {
            /* Whether zero-fill reads the source is open; not read here. */
            memset(buf, 0, MIN(chunk, lines) * BTE_LINE_SIZE);
        }
    }

    done = 0;
    while (done < lines) {
        uint64_t run = MIN(lines - done, chunk);
        MemTxResult result = MEMTX_OK;

        if (!(control & BTE_CONTROL_ZFILL)) {
            result = address_space_read(&s->bte_as,
                                        src + done * BTE_LINE_SIZE,
                                        MEMTXATTRS_UNSPECIFIED, buf,
                                        run * BTE_LINE_SIZE);
        }
        if (result == MEMTX_OK) {
            result = address_space_write(&s->bte_as,
                                         dst + done * BTE_LINE_SIZE,
                                         MEMTXATTRS_UNSPECIFIED, buf,
                                         run * BTE_LINE_SIZE);
        }
        if (result != MEMTX_OK) {
            if (run > 1) {
                /* Find the first line that does not move. */
                chunk = 1;
                continue;
            }
            /*
             * A failure latches error beside busy and keeps the unmoved
             * count; only the I/O error-clear register absolves it.
             * Neither completion path runs.
             */
            s->bte_length[engine] = BTE_LENGTH_BUSY | BTE_LENGTH_ERROR |
                                    (lines - done);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-hub: BTE%u transfer failed at line %" PRIu64
                          " (src 0x%" PRIx64 " dst 0x%" PRIx64 ")\n",
                          engine, done, src + done * BTE_LINE_SIZE,
                          dst + done * BTE_LINE_SIZE);
            qemu_log_mask(LOG_UNIMP,
                          "sgi-hub: BTE%u error latched; the Hub error "
                          "interrupt it would raise is not modelled\n",
                          engine);
            return;
        }
        done += run;
    }

    /* Busy and the count clear; a previously latched error bit survives. */
    s->bte_length[engine] &= BTE_LENGTH_ERROR;

    if (control & BTE_CONTROL_NOTIFY) {
        /* Exactly eight bytes: the kernel reads only the first doubleword. */
        MemTxResult res;

        address_space_stq_be(&s->bte_as, s->bte_notify[engine],
                             s->bte_length[engine], MEMTXATTRS_UNSPECIFIED,
                             &res);
        if (res != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-hub: BTE%u notification address 0x%" PRIx64
                          " reaches no memory\n",
                          engine, s->bte_notify[engine]);
        }
    } else {
        /*
         * Completion arrives as a device interrupt, via interrupt modify,
         * on the Hub the fabric's map holds for the destination number:
         * this Hub itself when the number is its own. A direct call, not
         * a store through the fabric view, so the launching store never
         * re-enters this Hub's own register region.
         */
        uint64_t level = (s->bte_interrupt[engine] & BTE_INT_LEVEL_MASK) >>
                         BTE_INT_LEVEL_SHIFT;
        uint64_t node = s->bte_interrupt[engine] & BTE_INT_NODE_MASK;
        SGIHubState *target = s->fabric ?
            sn0_fabric_hub_for_number(s->fabric, s->fabric_endpoint, node) :
            NULL;

        if (!target) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-hub: BTE%u completion interrupt aimed at "
                          "node %" PRIu64 ", which no reachable Hub is "
                          "programmed with\n", engine, node);
            return;
        }
        sgi_hub_int_modify(target, PI_INT_MODIFY_SET | level);
    }
}

static uint64_t sgi_hub_bte_read(SGIHubState *s, unsigned int engine,
                                 hwaddr reg)
{
    switch (reg) {
    case HUB_BTE_LENGTH:
        return s->bte_length[engine];
    case HUB_BTE_SOURCE:
        return s->bte_source[engine];
    case HUB_BTE_DEST:
        return s->bte_dest[engine];
    case HUB_BTE_CONTROL:
        /* A load stops a transfer in progress; there is never one here. */
        return s->bte_control[engine];
    case HUB_BTE_NOTIFY:
        return s->bte_notify[engine];
    case HUB_BTE_INTERRUPT:
        return s->bte_interrupt[engine];
    default:
        g_assert_not_reached();
    }
}

static void sgi_hub_bte_write(SGIHubState *s, unsigned int engine,
                              hwaddr reg, uint64_t val)
{
    switch (reg) {
    case HUB_BTE_LENGTH:
        /*
         * A store arms or disarms but never absolves: a set error bit
         * survives. Whether a store can set it is open; not allowed here.
         */
        s->bte_length[engine] = (s->bte_length[engine] & BTE_LENGTH_ERROR) |
                                (val & (BTE_LENGTH_BUSY | BTE_LENGTH_LINES));
        break;
    case HUB_BTE_SOURCE:
        s->bte_source[engine] = sgi_hub_bte_address(engine, "source", val);
        break;
    case HUB_BTE_DEST:
        s->bte_dest[engine] = sgi_hub_bte_address(engine, "destination",
                                                  val);
        break;
    case HUB_BTE_CONTROL:
        s->bte_control[engine] = val & BTE_CONTROL_MASK;
        /* Launches if armed; busy clear does nothing (datasheet proposal). */
        if (s->bte_length[engine] & BTE_LENGTH_BUSY) {
            sgi_hub_bte_launch(s, engine);
        }
        break;
    case HUB_BTE_NOTIFY:
        s->bte_notify[engine] = sgi_hub_bte_address(engine, "notification",
                                                    val);
        break;
    case HUB_BTE_INTERRUPT:
        s->bte_interrupt[engine] = val & (BTE_INT_LEVEL_MASK |
                                          BTE_INT_NODE_MASK);
        break;
    default:
        g_assert_not_reached();
    }
}

/* Where the counter has got to, from the clock rather than from stored state. */
static uint64_t sgi_hub_rt_count(SGIHubState *s)
{
    int64_t delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->pi_rt_origin_ns;

    if (delta < 0) {
        return 0;
    }
    return clock_ns_to_ticks(s->rt_clk, delta) & PI_RT_MASK;
}

/*
 * How long a comparator hold may last. Nothing is held during the wait,
 * so this only decides when a partner that never answers stops being
 * waited for; partner lags measured on a loaded host reach tens of
 * milliseconds.
 */
#define SGI_HUB_RT_HOLD_MS      2000

/* What a held processor waits for. */
typedef enum SGIHubRTHoldKind {
    SGI_HUB_RT_HOLD_MATCH = 1,  /* the partner to read its post back */
    SGI_HUB_RT_HOLD_POST,       /* the partner to write its comparator */
} SGIHubRTHoldKind;

typedef struct SGIHubRTHold {
    SGIHubState *hub;
    unsigned int slice;
    uint64_t id;
} SGIHubRTHold;

/* This Hub's slice for the executing processor, or -1 for any other. */
static int sgi_hub_current_slice(SGIHubState *s)
{
    if (!current_cpu || current_cpu->cpu_index < s->cpu_base ||
        current_cpu->cpu_index >= s->cpu_base + s->cpus) {
        return -1;
    }
    return current_cpu->cpu_index - s->cpu_base;
}

/* Whether a slice can still reach a read: enabled, running, not stopped. */
static bool sgi_hub_slice_can_read(SGIHubState *s, unsigned int slice)
{
    CPUState *cs = qemu_get_cpu(s->cpu_base + slice);

    return cs && s->pi_cpu_enable[slice] && !cs->halted && cpu_can_run(cs);
}

static const char *sgi_hub_rt_hold_kind_name(SGIHubRTHoldKind kind)
{
    return kind == SGI_HUB_RT_HOLD_MATCH ? "match" : "post";
}

/*
 * The event a hold waits for happened on another thread: end the hold
 * here, under the big lock, rather than when the held thread next runs,
 * so the partner can take its own hold at once.
 */
static void sgi_hub_rt_release(SGIHubState *s, unsigned int slice,
                               SGIHubRTHoldKind kind)
{
    CPUState *cs = qemu_get_cpu(s->cpu_base + slice);

    if (s->pi_rt_hold[slice] && s->pi_rt_hold_kind[slice] == kind) {
        if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_HOLD_RELEASE)) {
            trace_sgi_hub_rt_hold_release(
                current_cpu ? current_cpu->cpu_index : -1, s->cpu_base + slice,
                sgi_hub_rt_hold_kind_name(kind), s->pi_rt_hold[slice],
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        s->pi_rt_hold[slice] = 0;
        if (cs) {
            sgi_hub_trace_kick(cs, "rt-hold-release");
            qemu_cpu_kick(cs);
        }
    }
}

static void sgi_hub_rt_hold(CPUState *cs, run_on_cpu_data data)
{
    SGIHubRTHold *hold = data.host_ptr;
    SGIHubState *s = hold->hub;
    unsigned int slice = hold->slice;
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t deadline = start + SGI_HUB_RT_HOLD_MS * SCALE_MS;
    int64_t now;
    const char *outcome;
    bool traced_wait = false;
    int64_t wait_start = 0;
    int64_t wait_end = 0;
    int wait_ms = 0;

    for (;;) {
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (s->pi_rt_hold[slice] != hold->id) {
            outcome = "answered";
        } else if (!sgi_hub_slice_can_read(s, slice ^ 1)) {
            outcome = "partner stopped";
        } else if (cs->stop || cs->unplug || !cpu_work_list_empty(cs) ||
                   cpu_has_work(cs)) {
            outcome = "preempted";
        } else if (now >= deadline) {
            outcome = "timed out";
        } else {
            outcome = NULL;
        }

        /*
         * A wake is traced once the loop has decided what it means. The
         * stamps bracket the wait, which retakes the big lock before it
         * returns; it does not say whether it was signalled or timed out.
         */
        if (traced_wait) {
            trace_sgi_hub_rt_hold_wake(cs->cpu_index, hold->id, wait_ms,
                                       wait_start, wait_end,
                                       outcome ? outcome : "waiting");
        }
        if (outcome) {
            break;
        }

        wait_ms = (deadline - now) / SCALE_MS + 1;
        traced_wait =
            trace_event_get_state_backends(TRACE_SGI_HUB_RT_HOLD_WAKE);
        if (traced_wait) {
            wait_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
        qemu_cond_timedwait_bql(cs->halt_cond, wait_ms);
        if (traced_wait) {
            wait_end = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        }
    }

    if (s->pi_rt_hold[slice] == hold->id) {
        s->pi_rt_hold[slice] = 0;
    }
    /* End at the clock read the loop decided on, so the length is exact. */
    if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_HOLD_END)) {
        trace_sgi_hub_rt_hold_end(cs->cpu_index, outcome,
                                  (now - start) / SCALE_US,
                                  s->cpu_base + (slice ^ 1), start, now,
                                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  hold->id);
    }
    object_unref(OBJECT(s));
    g_free(hold);
}

/*
 * With its counter interrupt off, a processor's comparator is a progress
 * mailbox. The stock PROM meets a node's two processors through the pair:
 * each posts the same value to its own comparator and polls the other's,
 * and the second to arrive finds the match on its first read and
 * withdraws its post shortly after. Real processors run continuously, so
 * the first poller always reads that post while it stands, and reaches it
 * well inside its poll budget. Host threads do not run continuously: a
 * poller descheduled across the post misses it, spends its budget and
 * disables its partner.
 *
 * Under multi-threaded TCG, a processor that has posted a value and reads
 * the other comparator is therefore held in two cases. Reading its own
 * value back there, before the partner has read the post, it is held
 * until the partner does. Re-reading anything else with no write in
 * between, it is held once per partner write until the partner next
 * writes. A hold also ends when the partner can no longer run, when the
 * held processor has a stop, queued work or a deliverable interrupt, or
 * after SGI_HUB_RT_HOLD_MS.
 *
 * The hold is queued work on the reader's own vCPU. It starts at the
 * reader's next block boundary, outside the execution loop, and waits on
 * that vCPU's halt condition with the big lock released, so it holds no
 * RCU read-side section and every kick the vCPU honours reaches it. It
 * never waits on another vCPU's queue, and a Hub holds at most one
 * processor at a time. Register values are unchanged; only when the reader
 * resumes moves.
 */
static void sgi_hub_rt_mailbox_read(SGIHubState *s, unsigned int owner,
                                    uint64_t value)
{
    int reader = sgi_hub_current_slice(s);
    uint64_t posted;
    bool reread;
    SGIHubRTHoldKind kind;
    SGIHubRTHold *hold;

    if (reader < 0 || reader == owner) {
        return;
    }

    s->pi_rt_seen[owner] = true;
    sgi_hub_rt_release(s, owner, SGI_HUB_RT_HOLD_MATCH);
    reread = s->pi_rt_read_at[reader] == s->pi_rt_writes[owner];
    s->pi_rt_read_at[reader] = s->pi_rt_writes[owner];

    posted = s->pi_rt_compare[reader];
    if (!posted || s->pi_rt_enable[0] || s->pi_rt_enable[1] ||
        s->pi_rt_hold[owner] || !tcg_enabled() ||
        !qemu_tcg_mttcg_enabled() || !sgi_hub_slice_can_read(s, owner)) {
        return;
    }
    if (value == posted) {
        if (s->pi_rt_seen[reader]) {
            return;
        }
        kind = SGI_HUB_RT_HOLD_MATCH;
    } else {
        if (!reread || s->pi_rt_held_at[reader] == s->pi_rt_writes[owner]) {
            return;
        }
        s->pi_rt_held_at[reader] = s->pi_rt_writes[owner];
        kind = SGI_HUB_RT_HOLD_POST;
    }

    hold = g_new(SGIHubRTHold, 1);
    hold->hub = SGI_HUB(object_ref(OBJECT(s)));
    hold->slice = reader;
    hold->id = ++s->pi_rt_hold_seq;
    s->pi_rt_hold[reader] = hold->id;
    s->pi_rt_hold_kind[reader] = kind;
    if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_HOLD)) {
        trace_sgi_hub_rt_hold(current_cpu->cpu_index,
                              sgi_hub_rt_hold_kind_name(kind),
                              posted, value, s->cpu_base + owner,
                              qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                              hold->id);
    }
    async_run_on_cpu(current_cpu, sgi_hub_rt_hold, RUN_ON_CPU_HOST_PTR(hold));
}

/* Release every hold, and forget every post and read: a fresh start. */
static void sgi_hub_rt_hold_reset(SGIHubState *s)
{
    unsigned int n;

    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        CPUState *cs = qemu_get_cpu(s->cpu_base + n);

        if (s->pi_rt_hold[n] && cs) {
            sgi_hub_trace_kick(cs, "rt-hold-reset");
            qemu_cpu_kick(cs);
        }
        s->pi_rt_hold[n] = 0;
        s->pi_rt_seen[n] = false;
        s->pi_rt_writes[n] = 0;
        s->pi_rt_read_at[n] = UINT64_MAX;
        s->pi_rt_held_at[n] = UINT64_MAX;
    }
}

static uint64_t sgi_hub_read_rt_compare(SGIHubState *s, hwaddr addr,
                                        unsigned arg)
{
    CPUState *owner = qemu_get_cpu(s->cpu_base + arg);
    uint64_t value = s->pi_rt_compare[arg];

    /* A progress mailbox: let the owner publish before the poll budget ends. */
    sgi_hub_kick_polled_peer(owner, "rt-compare-poll");
    sgi_hub_rt_mailbox_read(s, arg, value);
    return value;
}

/* Move the origin so the counter reads what was written, and keep running. */
static void sgi_hub_rt_set(SGIHubState *s, uint64_t val)
{
    val &= PI_RT_MASK;
    s->pi_rt_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                         clock_ticks_to_ns(s->rt_clk, val);
}

static void sgi_hub_rt_match(SGIHubState *s, unsigned int index)
{
    s->pi_rt_pending[index] = 1;
    if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_EXPIRE)) {
        trace_sgi_hub_rt_expire(
            s->cpu_base, sgi_hub_programmed_nasid(s), index,
            sgi_hub_rt_count(s), s->pi_rt_compare[index],
            s->pi_rt_enable[index],
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    sgi_hub_update_irq(s);
}

/*
 * The comparator fires when the counter reaches the compare value, never
 * on being merely past it: IRIX parks a processor's tick by writing a
 * value behind the counter and depends on the silence. The next match is
 * the modular distance forward -- a whole revolution when the write lands
 * on equality -- and is the same arithmetic as an ordinary short deadline.
 */
static void sgi_hub_rt_arm(SGIHubState *s, unsigned int index)
{
    uint64_t ticks = ((s->pi_rt_compare[index] -
                       sgi_hub_rt_count(s) - 1) &
                      PI_RT_MASK) + 1;

    /* An unpopulated slice has no line wired. */
    if (index >= s->cpus) {
        return;
    }

    timer_mod(s->rt_timer[index], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              clock_ticks_to_ns(s->rt_clk, ticks));
}

/*
 * Completion and the sampled level, once the slot underway has finished.
 * Derived on the spot rather than delivered by a timer, because a timer
 * callback needs the big lock and firmware bit-banging this bus holds it:
 * the callback for a one-microsecond slot arrived roughly four milliseconds
 * late under eight processors, and the guest spent about two hundred more
 * polls of this register waiting for it. The guest-visible contract is the
 * one the one-wire datasheet sets and is unchanged -- starting a slot clears
 * completion, and no read before the deadline reports it finished.
 */
static uint64_t sgi_hub_onewire_value(SGIHubState *s, unsigned arg)
{
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->md_onewire_deadline_ns) {
        return s->md_onewire;
    }
    return s->md_onewire | MD_ONEWIRE_DONE |
           (s->onewire_level ? MD_ONEWIRE_RXDATA : 0);
}

static uint64_t sgi_hub_read_onewire(SGIHubState *s, hwaddr addr,
                                     unsigned arg)
{
    return sgi_hub_onewire_value(s, arg);
}

static void sgi_hub_rt_expire(void *opaque)
{
    SGIHubCounterLine *line = opaque;

    sgi_hub_rt_match(line->hub, line->index);
}

static uint8_t *sgi_hub_route_entry(SGIHubState *s, hwaddr addr)
{
    if ((addr & (NI_ROUTE_STRIDE - 1)) != 0) {
        return NULL;
    }
    if (addr >= HUB_NI_META_TABLE &&
        addr < HUB_NI_META_TABLE + SGI_HUB_META_ROUTES * NI_ROUTE_STRIDE) {
        return &s->ni_meta_route[(addr - HUB_NI_META_TABLE) / NI_ROUTE_STRIDE];
    }
    if (addr >= HUB_NI_LOCAL_TABLE &&
        addr < HUB_NI_LOCAL_TABLE + SGI_HUB_LOCAL_ROUTES * NI_ROUTE_STRIDE) {
        return &s->ni_local_route[(addr - HUB_NI_LOCAL_TABLE) /
                                  NI_ROUTE_STRIDE];
    }
    return NULL;
}

/* Undefined reset entries do not establish a programmed route. */
static void sgi_hub_route_written(SGIHubState *s, hwaddr addr)
{
    if (addr >= HUB_NI_LOCAL_TABLE) {
        s->ni_local_route_written |=
            1U << ((addr - HUB_NI_LOCAL_TABLE) / NI_ROUTE_STRIDE);
    } else {
        s->ni_meta_route_written |=
            1U << ((addr - HUB_NI_META_TABLE) / NI_ROUTE_STRIDE);
    }
    if (s->fabric) {
        sn0_fabric_recompute(s->fabric);
    }
}

bool sgi_hub_first_turn(const SGIHubState *s, uint32_t destination,
                         uint8_t *turn)
{
    unsigned int index;

    if (destination >= SN0_FABRIC_NUMBERS) {
        return false;
    }
    if ((destination >> 4) == (sgi_hub_programmed_nasid(s) >> 4)) {
        index = destination & 0xf;
        if (!(s->ni_local_route_written & (1U << index))) {
            return false;
        }
        *turn = s->ni_local_route[index];
    } else {
        index = destination >> 4;
        if (!(s->ni_meta_route_written & (1U << index))) {
            return false;
        }
        *turn = s->ni_meta_route[index];
    }
    return true;
}

static bool sgi_hub_vector_target_read(SGIHubState *target, hwaddr addr,
                                       uint64_t *value)
{
    uint8_t *route = sgi_hub_route_entry(target, addr);
    const SGIHubRegDef *reg;

    if (route) {
        *value = *route;
        return true;
    }
    reg = sgi_hub_reg_lookup(addr);
    if (reg && (reg->flags & HUB_REG_VECTOR) &&
        (reg->read || sgi_hub_reg_peekable(reg))) {
        *value = reg->read ? reg->read(target, addr, reg->arg) :
                             sgi_hub_reg_peek(target, reg);
        return true;
    }
    return false;
}

static bool sgi_hub_vector_target_write(SGIHubState *target, hwaddr addr,
                                        uint64_t value)
{
    uint8_t *route = sgi_hub_route_entry(target, addr);
    const SGIHubRegDef *reg;

    if (route) {
        *route = value & NI_ROUTE_MASK;
        sgi_hub_route_written(target, addr);
        return true;
    }
    /* The pure store: a remote vector write has no local write-hook effects. */
    reg = sgi_hub_reg_lookup(addr);
    if (reg && (reg->flags & HUB_REG_VECTOR) && sgi_hub_reg_storable(reg)) {
        sgi_hub_reg_store(target, reg, value);
        return true;
    }
    return false;
}

/*
 * The target's half of a vector operation, which the fabric delivers to
 * whichever Hub the route ends at. The request carries bits 19:3 of the
 * register address; a Hub target supplies the NI base. An exchange reads,
 * and writes only over a zero.
 */
bool sgi_hub_vector_access(SGIHubState *target, SN0FabricVectorAccess *access)
{
    hwaddr addr = HUB_NI_STATUS + access->addr;

    switch (access->op) {
    case SN0_FABRIC_VECTOR_READ:
        return sgi_hub_vector_target_read(target, addr, &access->value);
    case SN0_FABRIC_VECTOR_WRITE:
        return sgi_hub_vector_target_write(target, addr, access->data);
    case SN0_FABRIC_VECTOR_EXCHANGE:
        if (!sgi_hub_vector_target_read(target, addr, &access->value)) {
            return false;
        }
        if (access->value == 0) {
            return sgi_hub_vector_target_write(target, addr, access->data);
        }
        return true;
    default:
        /* A type the protocol does not define: answered, and refused. */
        return false;
    }
}

/*
 * Vector PIO: the fabric walks the route from this Hub's port and performs
 * the access on whatever the route ends at. A route nothing answers latches
 * nothing and lets the poll time out -- the datasheet's proposal for an
 * unobserved case, not a silicon fact; a device that answers and refuses
 * latches the address-error response.
 */
static void sgi_hub_vector_execute(SGIHubState *s, uint64_t parms)
{
    hwaddr addr = parms & NI_VECTOR_ADDRESS_MASK;
    uint64_t type = parms & NI_VECTOR_TYPE_MASK;
    uint64_t response_type = type;
    SN0FabricVectorAccess access = {
        .addr = addr,
        .data = s->ni_vector_data,
    };
    SN0FabricVectorResult result = SN0_FABRIC_VECTOR_NONE;

    /* The fabric is serialized by BQL MMIO dispatch alone. */
    assert(bql_locked());

    switch (type) {
    case NI_VECTOR_READ:
        access.op = SN0_FABRIC_VECTOR_READ;
        break;
    case NI_VECTOR_WRITE:
        access.op = SN0_FABRIC_VECTOR_WRITE;
        break;
    case NI_VECTOR_EXCHANGE:
        access.op = SN0_FABRIC_VECTOR_EXCHANGE;
        break;
    default:
        access.op = SN0_FABRIC_VECTOR_INVALID;
        break;
    }
    if (s->fabric) {
        result = sn0_fabric_vector(s->fabric, s->fabric_endpoint, s->ni_vector,
                                   &access);
    }
    if (result == SN0_FABRIC_VECTOR_NONE) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: vector launch (parms 0x%" PRIx64
                      ") with no fabric to answer\n", parms);
        return;
    }

    if (result == SN0_FABRIC_VECTOR_OK) {
        if (access.op != SN0_FABRIC_VECTOR_WRITE) {
            s->ni_vector_read_data = access.value;
        }
    } else {
        response_type = NI_VECTOR_ADDRESS_ERROR;
    }
    s->ni_vector_status = NI_VECTOR_VALID |
        ((uint64_t)access.source << NI_VECTOR_TARGET_SHIFT) |
        (parms & (NI_VECTOR_PIOID_MASK | NI_VECTOR_WRITEID_MASK)) |
        (addr & NI_VECTOR_STATUS_ADDR_MASK) | response_type;
    if (trace_event_get_state_backends(TRACE_SGI_HUB_VECTOR_COMPLETE)) {
        CPUState *cpu = current_cpu;

        trace_sgi_hub_vector_complete(
            cpu ? cpu->cpu_index : -1, sgi_hub_programmed_nasid(s),
            (int)access.source, cpu ? cpu->cc->get_pc(cpu) : 0, parms,
            s->ni_vector_data, access.value, s->ni_vector_status,
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

/*
 * Firmware's progress codes in our own words (Origami project tree,
 * docs/datasheets/hub-memory-directory.md). NULL marks a reserved code.
 */
static const char *const sgi_hub_led_progress[0x61] = {
    [0x00] = "processor reset",
    [0x01] = "initializing the CPU",
    [0x02] = "testing the floating-point unit",
    [0x03] = "running the TLB test",
    [0x04] = "testing the instruction cache",
    [0x05] = "testing the data cache",
    [0x06] = "testing the secondary cache",
    [0x07] = "flushing caches",
    [0x08] = "checking the Hub's local registers",
    [0x09] = "checking the Hub's configuration",
    [0x0a] = "invalidating the instruction cache",
    [0x0b] = "invalidating the data cache",
    [0x0c] = "invalidating the secondary cache",
    [0x0d] = "early main() entry",
    [0x0e] = "preparing to raise the PROM access speed",
    [0x0f] = "PROM access speed raised",
    [0x10] = "initializing the data cache",
    [0x11] = "initializing the instruction cache",
    [0x12] = "initializing coprocessor 0",
    [0x13] = "flushing the TLB",
    [0x14] = "clearing cache tags",
    [0x15] = "UART init failed (one path)",
    [0x16] = "initializing the Hub",
    [0x17] = "UART init failed (another path)",
    [0x18] = "UART init failed, no clock detected",
    [0x19] = "Hub initialization complete",
    [0x1a] = "probing the system controller",
    [0x1b] = "probing the Junk UART",
    [0x1c] = "system-controller probe complete",
    [0x1d] = "initializing the UART",
    [0x1e] = "UART initialization complete",
    [0x1f] = "checking the Hub chip",
    [0x20] = "entering POD diagnostics",
    [0x21] = "running the POD diagnostic loop",
    [0x22] = "at the POD command prompt",
    [0x23] = "POD mode",
    [0x24] = "arbitrating for local resources",
    [0x25] = "initializing the system controller",
    [0x26] = "arbitrating for boot-master status",
    [0x27] = "elected boot master",
    [0x28] = "waiting at a synchronization barrier",
    [0x29] = "checking the primary data cache",
    [0x2a] = "setting up the boot stack",
    [0x2b] = "entering main()",
    [0x2c] = "loading the I/O-board firmware",
    [0x2d] = "checking secondary cache RAM",
    [0x2e] = "checking secondary cache tags",
    [0x2f] = "entering slave-processor mode",
    [0x30] = "jumping into the loaded firmware",
    [0x31] = "handling a non-maskable interrupt",
    [0x32] = "slave processor invalidating instruction/data caches",
    [0x33] = "slave processor invalidating the secondary cache",
    [0x34] = "writing the configuration entry",
    [0x35] = "initializing the real-time clock",
    [0x36] = "real-time clock initialization complete",
    [0x37] = "taking a hardware lock",
    [0x38] = "synchronization barrier cleared",
    [0x39] = "hardware lock acquired",
    [0x3a] = "initializing the flash PROM",
    [0x3b] = "flash PROM initialization complete",
    [0x3c] = "jumping to code running from RAM, uncached",
    [0x3d] = "running from uncached RAM, confirmed",
    [0x3e] = "jumping to code running from RAM, cached",
    [0x3f] = "running from cached RAM, confirmed",
    [0x40] = "switching the stack to RAM",
    [0x41] = "stack running from RAM, confirmed",
    [0x42] = "signalling a slave processor",
    [0x43] = "calling into a slave processor's routine",
    [0x44] = "a slave processor's routine returned",
    [0x45] = "waiting in the processor-launch loop",
    [0x46] = "processor-launch interrupt received",
    [0x47] = "calling the launched routine",
    [0x48] = "processor launch complete",
    [0x49] = "computing the UART base address",
    [0x4a] = "beginning memory/directory unit setup",
    [0x4b] = "configuring the memory/directory unit",
    [0x4c] = "starting I2C bus initialization",
    [0x4d] = "I2C bus initialization complete",
    [0x4e] = "beginning configuration-graph construction",
    [0x4f] = "discovering I/O widgets",
    [0x50] = "writing the Hub's own configuration entry",
    [0x51] = "writing a router's configuration entry",
    [0x52] = "initializing I/O devices",
    [0x53] = "locating the console device",
    [0x54] = "console device located",
    [0x55] = NULL, /* reserved (source marks this value avoid-use) */
    [0x56] = "I/O device initialization complete",
    [0x57] = "the Hub's error-status registers have been saved off",
    [0x58] = "the Hub's error-status registers have been cleared",
    [0x59] = "Hub error-interrupt checking is now enabled",
    [0x5a] = "I/O widget discovery complete",
    [0x5b] = "setting up the non-maskable-interrupt path",
    [0x5c] = "testing interrupts",
    [0x5d] = "early reset of the Hub I/O section",
    [0x5e] = "the processor has exited its reset state",
    [0x5f] = "about to reset the I2C bus",
    [0x60] = "I2C bus reset complete",
};

/* Firmware's failure codes, 0x81-0xb5, from the same source. */
static const char *const sgi_hub_led_failure[0xb6 - 0x81] = {
    [0x00] = "floating-point unit failed self-test", /* 0x81 */
    [0x01] = "restarting after failure", /* 0x82 */
    [0x02] = "instruction cache failed self-test", /* 0x83 */
    [0x03] = "data cache failed self-test", /* 0x84 */
    [0x04] = "secondary cache failed self-test", /* 0x85 */
    [0x05] = "processor disabled by another processor", /* 0x86 */
    [0x06] = "real-time counter is broken", /* 0x87 */
    [0x07] = "ECC error", /* 0x88 */
    [0x08] = "unexpected extended TLB miss", /* 0x89 */
    [0x09] = "unexpected user TLB miss", /* 0x8a */
    [0x0a] = "unexpected kernel TLB miss", /* 0x8b */
    [0x0b] = ("the processor took an exception outside every "
              "specifically-handled category"), /* 0x8c */
    [0x0c] = "unimplemented-instruction exception", /* 0x8d */
    [0x0d] = "the processor signaled a cache parity or ECC fault", /* 0x8e */
    [0x0e] = "the operating system requested this code", /* 0x8f */
    [0x0f] = "Hub interrupt self-test failed", /* 0x90 */
    [0x10] = "Hub local-register self-test failed", /* 0x91 */
    [0x11] = "Hub configuration self-test failed", /* 0x92 */
    [0x12] = "a node reported non-premium unexpectedly", /* 0x93 */
    [0x13] = NULL, /* 0x94: reserved (a POD re-entry via NMI was requested) */
    [0x14] = "Hub UART initialization failed", /* 0x95 */
    [0x15] = "Hub cross-processor interrupt self-test failed", /* 0x96 */
    [0x16] = ("execution fell out of the firmware's main() routine, "
              "which should never return"), /* 0x97 */
    [0x17] = "this node has no memory of its own installed", /* 0x98 */
    [0x18] = "an I2C condition that should be impossible occurred", /* 0x99 */
    [0x19] = "this processor is disabled by an environment variable", /* 0x9a */
    [0x1a] = "a data transfer into memory did not complete correctly", /* 0x9b */
    [0x1b] = "could not set the core-debug register", /* 0x9c */
    [0x1c] = "I/O widget discovery failed", /* 0x9d */
    [0x1d] = "writing the Hub's configuration entry failed", /* 0x9e */
    [0x1e] = "writing a router's configuration entry failed", /* 0x9f */
    [0x1f] = "Hub I/O interface initialization failed", /* 0xa0 */
    [0x20] = "node configuration-graph construction failed", /* 0xa1 */
    [0x21] = "router chip failed diagnostics", /* 0xa2 */
    [0x22] = "a fabric link failed diagnostics", /* 0xa3 */
    [0x23] = "Hub chip failed its built-in self-test", /* 0xa4 */
    [0x24] = "router chip failed its built-in self-test", /* 0xa5 */
    [0x25] = "waiting for a reset to complete", /* 0xa6 */
    [0x26] = "low-level fabric protocol failed after reset", /* 0xa7 */
    [0x27] = "low-level fabric protocol never came up after reset", /* 0xa8 */
    [0x28] = "no good local memory found", /* 0xa9 */
    [0x29] = NULL, /* 0xaa: reserved (source marks this value avoid-use) */
    [0x2a] = "surveying the rest of the interconnect fabric did not succeed", /* 0xab */
    [0x2b] = "node-id calculation failed", /* 0xac */
    [0x2c] = ("computing the paths packets should take between nodes "
              "did not succeed"), /* 0xad */
    [0x2d] = ("sending the computed routing table out to the rest of "
              "the machine did not succeed"), /* 0xae */
    [0x2e] = "node-id distribution failed", /* 0xaf */
    [0x2f] = "boot master assigned no node id", /* 0xb0 */
    [0x30] = "module-id arbitration failed", /* 0xb1 */
    [0x31] = "an SN0 node mixed with an SN00 node", /* 0xb2 */
    [0x32] = "a partition-configuration error", /* 0xb3 */
    [0x33] = "an error copying mode bits", /* 0xb4 */
    [0x34] = "an error calculating the backplane frequency", /* 0xb5 */
};

/* NULL outside the enumeration; undecoded is not a fault. */
static const char *sgi_hub_led_decode(uint8_t code, bool *is_failure)
{
    *is_failure = false;
    if (code <= 0x60) {
        return sgi_hub_led_progress[code];
    }
    if (code >= 0x81 && code <= 0xb5) {
        *is_failure = true;
        return sgi_hub_led_failure[code - 0x81];
    }
    return NULL;
}

/* The per-register hooks the descriptor table names. */

static uint64_t sgi_hub_read_processor_number(SGIHubState *s, hwaddr addr,
                                              unsigned arg)
{
    /* current_cpu is thread-local, so this is sound under MTTCG. */
    if (current_cpu && current_cpu->cpu_index >= s->cpu_base &&
        current_cpu->cpu_index < s->cpu_base + s->cpus) {
        return current_cpu->cpu_index - s->cpu_base;
    }
    return 0;
}

static void sgi_hub_write_processor_number(SGIHubState *s, hwaddr addr,
                                           unsigned arg, uint64_t val,
                                           unsigned size)
{
    /* A logic-analyzer convention in the vendor's own drivers, not an error. */
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to the processor-"
                  "number register 0x%" HWADDR_PRIx " (a logic-analyzer "
                  "convention on real firmware, not modelled)\n",
                  val, addr);
}

static uint64_t sgi_hub_read_cpu_present(SGIHubState *s, hwaddr addr,
                                         unsigned arg)
{
    return s->cpus >= arg + 1;
}

static uint64_t sgi_hub_read_int_modify(SGIHubState *s, hwaddr addr,
                                        unsigned arg)
{
    /* An instruction, not a value; its state reads through the banks. */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-hub: read of the write-only interrupt-modify "
                  "register\n");
    return 0;
}

static void sgi_hub_write_int_modify(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    sgi_hub_int_modify(s, val);
}

static uint64_t sgi_hub_read_int_pend(SGIHubState *s, hwaddr addr,
                                      unsigned arg)
{
    if (arg == 1) {
        return sgi_hub_int_pend1_read(s);
    }
    return sgi_hub_int_pending_read(s, arg);
}

static void sgi_hub_write_read_only(SGIHubState *s, hwaddr addr,
                                    unsigned arg, uint64_t val,
                                    unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-hub: write of 0x%" PRIx64 " to the read-only "
                  "register 0x%" HWADDR_PRIx "\n", val, addr);
}

static void sgi_hub_write_int_mask(SGIHubState *s, hwaddr addr,
                                   unsigned arg, uint64_t val,
                                   unsigned size)
{
    s->pi_int_mask[arg] = val;
    sgi_hub_update_irq(s);
}

static void sgi_hub_write_rt_count(SGIHubState *s, hwaddr addr,
                                   unsigned arg, uint64_t val,
                                   unsigned size)
{
    sgi_hub_rt_set(s, val);
    /* Moving the counter moves every deadline standing against it. */
    sgi_hub_rt_arm(s, 0);
    sgi_hub_rt_arm(s, 1);
}

static uint64_t sgi_hub_peek_rt_count(SGIHubState *s, unsigned arg)
{
    return sgi_hub_rt_count(s);
}

static uint64_t sgi_hub_read_rt_count_reg(SGIHubState *s, hwaddr addr,
                                          unsigned arg)
{
    uint64_t value = sgi_hub_rt_count(s);

    if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_COUNT_READ)) {
        CPUState *cpu = current_cpu;

        trace_sgi_hub_rt_count_read(
            cpu ? cpu->cpu_index : -1, s->cpu_base,
            sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
            value, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return value;
}

static void sgi_hub_write_rt_compare(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    CPUState *other;

    s->pi_rt_writes[arg]++;
    if (s->pi_rt_compare[arg] != (val & PI_RT_MASK)) {
        s->pi_rt_seen[arg] = false;
        sgi_hub_rt_release(s, arg, SGI_HUB_RT_HOLD_MATCH);
    }
    if ((arg ^ 1) < s->cpus) {
        sgi_hub_rt_release(s, arg ^ 1, SGI_HUB_RT_HOLD_POST);
    }
    s->pi_rt_compare[arg] = val & PI_RT_MASK;
    sgi_hub_rt_arm(s, arg);

    /*
     * A progress mailbox only while the slice's interrupt is disabled: the
     * PROM arbitrates through it that way, so kick the other processor before
     * its instruction-count timeout expires. A deadline writer has the
     * interrupt enabled and does not kick the peer.
     */
    other = qemu_get_cpu(s->cpu_base + (arg ^ 1));
    if (!s->pi_rt_enable[arg] && (arg ^ 1) < s->cpus) {
        sgi_hub_kick_peer(other, "rt-compare-post");
    }
}

static void sgi_hub_write_rt_pending(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    /* The handler acknowledges by writing zero, which drops the line. */
    s->pi_rt_pending[arg] = val & 1;
    sgi_hub_update_irq(s);
}

static void sgi_hub_write_rt_enable(SGIHubState *s, hwaddr addr,
                                    unsigned arg, uint64_t val,
                                    unsigned size)
{
    s->pi_rt_enable[arg] = val & 1;
    sgi_hub_update_irq(s);
}

static uint64_t sgi_hub_peek_zero(SGIHubState *s, unsigned arg)
{
    return 0;
}

static uint64_t sgi_hub_read_zero(SGIHubState *s, hwaddr addr, unsigned arg)
{
    return 0;
}

static void sgi_hub_write_err_pending(SGIHubState *s, hwaddr addr,
                                      unsigned arg, uint64_t val,
                                      unsigned size)
{
    /* Write-one-to-clear has no effect while no error source can set it. */
}

static uint64_t sgi_hub_read_err_status_clear(SGIHubState *s, hwaddr addr,
                                              unsigned arg)
{
    uint64_t value = s->pi_err_status0[arg];

    s->pi_err_status0[arg] = 0;
    return value;
}

static void sgi_hub_write_err_status_clear(SGIHubState *s, hwaddr addr,
                                           unsigned arg, uint64_t val,
                                           unsigned size)
{
    /*
     * Only a full-width zero store clears, the same conservative choice the
     * 32-bit read path makes for an unobserved access.
     */
    if (size == 8 && val == 0) {
        s->pi_err_status0[arg] = 0;
    }
}

static void sgi_hub_write_timedout(SGIHubState *s, hwaddr addr,
                                   unsigned arg, uint64_t val,
                                   unsigned size)
{
    /* Nothing timed out for a write to clear. */
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to processor-interface "
                  "timed-out-buffer register 0x%" HWADDR_PRIx
                  " (no buffer timed out here for a write to clear)\n",
                  val, addr);
}

static uint64_t sgi_hub_read_widget_id(SGIHubState *s, hwaddr addr,
                                       unsigned arg)
{
    return ((uint64_t)s->widget_revision << IO_WIDGET_ID_REVISION_SHIFT) |
           IO_WIDGET_ID_PART |
           ((uint64_t)s->manufacturer << IO_WIDGET_ID_MFG_SHIFT) |
           IO_WIDGET_ID_ONE;
}

static void sgi_hub_write_widget_id(SGIHubState *s, hwaddr addr,
                                    unsigned arg, uint64_t val,
                                    unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-hub: write of 0x%" PRIx64 " to the read-only widget "
                  "register 0x%" HWADDR_PRIx "\n", val, addr);
}

static uint64_t sgi_hub_read_widget_status(SGIHubState *s, hwaddr addr,
                                           unsigned arg)
{
    /* No errors, retries or outstanding requests here; bit 4 is undefined. */
    return 0;
}

static void sgi_hub_write_widget_status(SGIHubState *s, hwaddr addr,
                                        unsigned arg, uint64_t val,
                                        unsigned size)
{
    /* Firmware clears this at power-on; nothing here ever sets it. */
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to the widget status "
                  "register 0x%" HWADDR_PRIx " (firmware clears this at "
                  "power-on; nothing here ever sets it)\n", val, addr);
}

static void sgi_hub_store_widget_control(SGIHubState *s, unsigned arg,
                                         uint64_t val)
{
    s->io_widget_control = (s->io_widget_control &
                            ~IO_WIDGET_CONTROL_WRITABLE) |
                           (val & IO_WIDGET_CONTROL_WRITABLE);
}

static void sgi_hub_store_link_status(SGIHubState *s, unsigned arg,
                                      uint64_t val)
{
    s->io_link_status = (s->io_link_status & ~IO_LINK_WRITABLE) |
                        (val & IO_LINK_WRITABLE);
}

/*
 * The crossbar's per-link reset, arriving on the crosstalk link.
 *
 * The published Crossbow specification establishes three effects at this
 * end and no others: the link-layer transmit buffer is cleared, link-layer
 * protocol state is reset, and width negotiation restarts. This model
 * represents none of the three, so the pulse changes nothing and says so
 * rather than substituting an effect on state the evidence does not reach.
 *
 * There is no transmit buffer to empty, because every crosstalk access
 * this model makes completes before it returns and nothing is ever queued
 * on the link. There is no link-layer protocol state either -- no running
 * sequence numbers, retry counters or credits -- and the link log's two counts
 * are served as zero from a link that commits none. The one field that
 * would show renegotiation is the link state, and it cannot move: no
 * reviewed source gives the value it reads while negotiation runs, and
 * none gives the predicate that ends negotiation, so both the transient
 * code and the return from it would be invented.
 *
 * Nothing else in the link register moves. The maximum burst and retry
 * counts, the null-packet timeout, the link-enable bit firmware operates
 * for itself and the warm-reset trigger are programmed configuration,
 * which is neither the transmit buffer nor protocol state. The contract's
 * one datum about a link-layer control across this reset says retained:
 * Crossbow's own force-eight-bit control survives the pulse so software
 * can pick a mode before negotiation restarts. Unsetting the link-enable
 * bit would be the most expensive guess available, because a Hub that
 * reports its crosstalk link down loses the machine quietly.
 *
 * The wider I/O interface is not in this at all. Every non-link-layer Hub
 * reset effect is open.
 */
static void sgi_hub_link_reset(void *opaque, int line, int level)
{
    if (!level) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: inbound link reset; nothing this model holds "
                  "changes. No transmit buffer is queued and no link-layer "
                  "protocol state is represented, and the link state cannot "
                  "show renegotiation: no reviewed source gives its value "
                  "while negotiation runs or the predicate that ends it\n");
}

static void sgi_hub_write_error_clear(SGIHubState *s, hwaddr addr,
                                      unsigned arg, uint64_t val,
                                      unsigned size)
{
    unsigned int n;

    /* Each bit clears a latch; only the engines' error bits exist here. */
    for (n = 0; n < SGI_HUB_BTES; n++) {
        if (val & IO_ERROR_CLEAR_BTE(n)) {
            s->bte_length[n] &= ~BTE_LENGTH_ERROR;
        }
    }
    if (val & ~(IO_ERROR_CLEAR_BTE(0) | IO_ERROR_CLEAR_BTE(1))) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the error "
                      "clear register 0x%" HWADDR_PRIx " (only the two "
                      "block-transfer-engine latches exist here)\n",
                      val, addr);
    }
}

static uint64_t sgi_hub_read_ioq(SGIHubState *s, hwaddr addr, unsigned arg)
{
    /* Nothing in flight, queued or valid; the deallocation polls need zero. */
    return 0;
}

static void sgi_hub_write_ioq(SGIHubState *s, hwaddr addr,
                              unsigned arg, uint64_t val, unsigned size)
{
    /* Nothing outstanding: the free completes at once, the bit reads clear. */
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to I/O-queue register "
                  "0x%" HWADDR_PRIx
                  " (no outstanding entry to free; completes at once)\n",
                  val, addr);
}

/*
 * The link log accretes crosstalk LLP errors -- check-bit counts in bits
 * 31:16, sequence-number counts in 15:0 -- and the emulated link commits
 * none, so the counts hold their reset zero. The 6.5.30 kernel's periodic
 * hub-statistics capture reads the log about once a second and feeds the
 * counts to a rate evaluation, so the value is an input to a decision:
 * never serve an error the link did not commit.
 */
static uint64_t sgi_hub_read_llp_log(SGIHubState *s, hwaddr addr,
                                     unsigned arg)
{
    return 0;
}

static void sgi_hub_write_llp_log(SGIHubState *s, hwaddr addr,
                                  unsigned arg, uint64_t val, unsigned size)
{
    /*
     * The documented idiom is read-then-write-zero to close a sampling
     * interval; clearing zero counts is complete at once. What a nonzero
     * write does to the real counters is unverified, so it is discarded
     * rather than invented.
     */
}

static uint64_t sgi_hub_read_slot_id(SGIHubState *s, hwaddr addr,
                                     unsigned arg)
{
    uint64_t value = MD_SLOT_FLASH_READY | MD_SLOT_XJMP_ID |
                     (s->slave_module ? 0 : MD_SLOT_MASTER_MODULE);

    if (s->physical_slot) {
        value = (value & ~MD_SLOT_ID_MASK) |
                ((8 - s->physical_slot) & MD_SLOT_ID_MASK);
    }
    return value;
}

static void sgi_hub_write_dimm_init(SGIHubState *s, hwaddr addr,
                                    unsigned arg, uint64_t val,
                                    unsigned size)
{
    /* Firmware's per-DIMM sweep; write-only, with no functional signature. */
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to the DIMM mode "
                  "initialisation register 0x%" HWADDR_PRIx " (write-"
                  "only; no functional signature on this model)\n",
                  val, addr);
}

static void sgi_hub_write_onewire(SGIHubState *s, hwaddr addr,
                                  unsigned arg, uint64_t val,
                                  unsigned size)
{
    /*
     * Programs the timing and starts a slot. Starting clears completion. The
     * bus answers inside the write, but this master reports the answer only
     * from the slot's own deadline -- see sgi_hub_onewire_value().
     */
    s->md_onewire = val & MD_ONEWIRE_WRITABLE;
    s->onewire_level = sgi_onewire_slot(
        s->onewire_bus, (val & MD_ONEWIRE_PULSE) >> MD_ONEWIRE_PULSE_SHIFT,
        (val & MD_ONEWIRE_SAMPLE) >> MD_ONEWIRE_SAMPLE_SHIFT);
    s->md_onewire_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                MD_ONEWIRE_SLOT_NS;
}

static void sgi_hub_write_indicator(SGIHubState *s, hwaddr addr,
                                    unsigned arg, uint64_t val,
                                    unsigned size)
{
    /* Write-only; every write is traced, decoded or flagged undecoded. */
    uint8_t code = val & 0xff;
    bool is_failure = false;
    const char *meaning = sgi_hub_led_decode(code, &is_failure);

    if (meaning) {
        trace_sgi_hub_led(arg, code, is_failure ? "FAILURE" : "progress",
                          meaning);
    } else {
        trace_sgi_hub_led(arg, code, "undecoded",
                          "not one of real firmware's progress or "
                          "failure codes -- may be a guest OS write "
                          "or the firmware's character-wait flash");
    }
}

static void sgi_hub_store_ni_status(SGIHubState *s, unsigned arg,
                                    uint64_t val)
{
    /* Node and region size are firmware's; link state and identity are not. */
    s->ni_status = (s->ni_status & ~NI_STATUS_WRITABLE) |
                   (val & NI_STATUS_WRITABLE);
    /* The programmed number moved: the fabric re-derives every map. */
    sgi_hub_report_number(s);
}

static void sgi_hub_store_pi_calias_size(SGIHubState *s, unsigned arg,
                                         uint64_t val)
{
    s->pi_calias_size = val & PI_CALIAS_SIZE_MASK;
    /* The size moved: the nameless low windows resize with it. */
    sgi_hub_calias_update(s);
}

static void sgi_hub_write_port_reset(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    /* Bit 0 alone: the three writers set different companion bits. */
    if (val & NI_PORT_RESET_LOCAL) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static void sgi_hub_store_io_prot_ovrrd(SGIHubState *s, unsigned arg,
                                        uint64_t val)
{
    /*
     * The password unlock is a no-op on a model that never enforces
     * the protection; a non-password write's effect is unstated.
     * Neither stores anything.
     */
    if (val != NI_PROT_OVRRD_PASSWORD) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the protection "
                      "override register (not the password; effect "
                      "unstated)\n", val);
    }
}

static void sgi_hub_store_ni_port_error(SGIHubState *s, unsigned arg,
                                        uint64_t val)
{
    s->ni_port_error &= ~(val & NI_PORT_ERROR_MASK);
}

static uint64_t sgi_hub_read_ni_port_error_clear(SGIHubState *s, hwaddr addr,
                                                 unsigned arg)
{
    uint64_t value = s->ni_port_error;

    s->ni_port_error = 0;
    return value;
}

static void sgi_hub_write_ni_scratch(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    trace_sgi_hub_ni_scratch_write(s->cpu_base, s->nasid, arg, val);
    s->ni_scratch[arg] = val;
    sgi_hub_kick_remote_scratch_node(s);
}

static void sgi_hub_write_vector_parms(SGIHubState *s, hwaddr addr,
                                       unsigned arg, uint64_t val,
                                       unsigned size)
{
    sgi_hub_vector_execute(s, val);
}

static void sgi_hub_write_vector_status(SGIHubState *s, hwaddr addr,
                                        unsigned arg, uint64_t val,
                                        unsigned size)
{
    if (val == 0) {
        s->ni_vector_status = 0;
    }
}

static uint64_t sgi_hub_read_vector_clear(SGIHubState *s, hwaddr addr,
                                          unsigned arg)
{
    s->ni_vector_status = 0;
    return 0;
}

static void sgi_hub_write_vector_clear(SGIHubState *s, hwaddr addr,
                                       unsigned arg, uint64_t val,
                                       unsigned size)
{
    s->ni_vector_status = 0;
}

/* The register map, one row per decoded doubleword, in address order. */
static const SGIHubRegDef sgi_hub_registers[] = {
    { .offset = HUB_PI_REGION_PRESENT,
      .field = offsetof(SGIHubState, pi_region_present), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = ~0ULL },
    { .offset = HUB_PI_PROCESSOR_NUMBER,
      .flags = HUB_REG_TRACE_COORD,
      .read = sgi_hub_read_processor_number,
      .write = sgi_hub_write_processor_number },
    { .offset = HUB_PI_CALIAS_SIZE,
      .field = offsetof(SGIHubState, pi_calias_size), .width = 4,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .store = sgi_hub_store_pi_calias_size },
    { .offset = HUB_PI_CPU_PRESENT_A, .arg = 0,
      .flags = HUB_REG_TRACE_COORD,
      .read = sgi_hub_read_cpu_present },
    { .offset = HUB_PI_CPU_PRESENT_B, .arg = 1,
      .flags = HUB_REG_TRACE_COORD,
      .read = sgi_hub_read_cpu_present },
    { .offset = HUB_PI_CPU_ENABLE_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_cpu_enable[0]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE | HUB_REG_TRACE_COORD,
      .write = sgi_hub_write_cpu_enable },
    { .offset = HUB_PI_CPU_ENABLE_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_cpu_enable[1]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE | HUB_REG_TRACE_COORD,
      .write = sgi_hub_write_cpu_enable },
    { .offset = HUB_PI_INT_MODIFY,
      .flags = HUB_REG_TRACE_WRITE | HUB_REG_TRACE_COORD,
      .read = sgi_hub_read_int_modify, .write = sgi_hub_write_int_modify },
    { .offset = HUB_PI_INT_PEND0, .arg = 0,
      .flags = HUB_REG_TRACE_READ | HUB_REG_TRACE_COORD |
               HUB_REG_TRACE_PENDING,
      .read = sgi_hub_read_int_pend, .write = sgi_hub_write_read_only },
    { .offset = HUB_PI_INT_PEND1, .arg = 1,
      .flags = HUB_REG_TRACE_READ | HUB_REG_TRACE_COORD |
               HUB_REG_TRACE_PENDING,
      .read = sgi_hub_read_int_pend, .write = sgi_hub_write_read_only },
    { .offset = HUB_PI_INT_MASK0_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_int_mask[0]), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_INT_MASK1_A, .arg = 1,
      .field = offsetof(SGIHubState, pi_int_mask[1]), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_INT_MASK0_B, .arg = 2,
      .field = offsetof(SGIHubState, pi_int_mask[2]), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_INT_MASK1_B, .arg = 3,
      .field = offsetof(SGIHubState, pi_int_mask[3]), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_RT_COMPARE_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_rt_compare[0]), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE,
      .read = sgi_hub_read_rt_compare, .write = sgi_hub_write_rt_compare },
    { .offset = HUB_PI_RT_COMPARE_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_rt_compare[1]), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE,
      .read = sgi_hub_read_rt_compare, .write = sgi_hub_write_rt_compare },
    { .offset = HUB_PI_RT_PENDING_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_rt_pending[0]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE,
      .write = sgi_hub_write_rt_pending },
    { .offset = HUB_PI_RT_PENDING_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_rt_pending[1]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE,
      .write = sgi_hub_write_rt_pending },
    { .offset = HUB_PI_RT_ENABLE_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_rt_enable[0]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .write = sgi_hub_write_rt_enable },
    { .offset = HUB_PI_RT_ENABLE_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_rt_enable[1]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .write = sgi_hub_write_rt_enable },
    { .offset = HUB_PI_RT_LOCAL_CTRL,
      .field = offsetof(SGIHubState, pi_rt_local_ctrl), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG,
      .wmask = PI_RT_LOCAL_CTRL_MASK },
    { .offset = HUB_PI_ERR_PENDING,
      .flags = HUB_REG_DIAG,
      .peek = sgi_hub_peek_zero, .write = sgi_hub_write_err_pending },
    { .offset = HUB_PI_ERR_MASK_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_err_mask[0]), .width = 4,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = PI_ERR_MASK },
    { .offset = HUB_PI_ERR_MASK_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_err_mask[1]), .width = 4,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = PI_ERR_MASK },
    { .offset = HUB_PI_ERR_SPOOL_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_err_spool[0]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = PI_ERR_SPOOL_MASK },
    { .offset = HUB_PI_ERR_SPOOL_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_err_spool[1]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = PI_ERR_SPOOL_MASK },
    { .offset = HUB_PI_ERR_SPOOL_SIZE,
      .field = offsetof(SGIHubState, pi_err_spool_size), .width = 1,
      .flags = HUB_REG_STORE | HUB_REG_DIAG,
      .wmask = PI_ERR_SPOOL_SIZE_MASK },
    /*
     * Error status and its read-to-clear aliases; only the first doubleword
     * of each record is modelled, and the aliases clear on full width only.
     */
    { .offset = HUB_PI_ERR_STATUS_BASE, .arg = 0,
      .field = offsetof(SGIHubState, pi_err_status0[0]), .width = 8 },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x08, .arg = 0,
      .field = offsetof(SGIHubState, pi_err_status0[0]), .width = 8,
      .flags = HUB_REG_READ_CLEARS,
      .read = sgi_hub_read_err_status_clear,
      .write = sgi_hub_write_err_status_clear },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x10,
      .read = sgi_hub_read_zero },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x18,
      .read = sgi_hub_read_zero },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x20, .arg = 1,
      .field = offsetof(SGIHubState, pi_err_status0[1]), .width = 8 },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x28, .arg = 1,
      .field = offsetof(SGIHubState, pi_err_status0[1]), .width = 8,
      .flags = HUB_REG_READ_CLEARS,
      .read = sgi_hub_read_err_status_clear,
      .write = sgi_hub_write_err_status_clear },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x30,
      .read = sgi_hub_read_zero },
    { .offset = HUB_PI_ERR_STATUS_BASE + 0x38,
      .read = sgi_hub_read_zero },
    /* No processor-interface request buffer times out on this model. */
    { .offset = HUB_PI_TIMEDOUT_BUF_A,
      .read = sgi_hub_read_zero, .write = sgi_hub_write_timedout },
    { .offset = HUB_PI_TIMEDOUT_BUF_B,
      .read = sgi_hub_read_zero, .write = sgi_hub_write_timedout },
    { .offset = HUB_PI_RT_COUNT,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .read = sgi_hub_read_rt_count_reg, .peek = sgi_hub_peek_rt_count,
      .write = sgi_hub_write_rt_count },
    { .offset = HUB_MD_CONFIG,
      .field = offsetof(SGIHubState, md_config), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .wmask = MD_CONFIG_MASK },
    { .offset = HUB_MD_MEM_DIMM_INIT, .write = sgi_hub_write_dimm_init },
    { .offset = HUB_MD_DIR_DIMM_INIT, .write = sgi_hub_write_dimm_init },
    { .offset = HUB_MD_ONEWIRE,
      .field = offsetof(SGIHubState, md_onewire), .width = 8,
      .flags = HUB_REG_DIAG, .read = sgi_hub_read_onewire,
      .peek = sgi_hub_onewire_value, .write = sgi_hub_write_onewire },
    /* Storage that never counts: nothing here tracks an event to count. */
    { .offset = HUB_MD_PERF_CNT0,
      .field = offsetof(SGIHubState, md_perf_cnt0), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .wmask = MD_PERF_CNT_MASK },
    { .offset = HUB_MD_SLOT_ID_USTAT, .read = sgi_hub_read_slot_id },
    { .offset = HUB_MD_INDICATOR_0, .arg = 0,
      .write = sgi_hub_write_indicator },
    { .offset = HUB_MD_INDICATOR_1, .arg = 1,
      .write = sgi_hub_write_indicator },
    { .offset = HUB_MD_SN00_INDICATOR_0, .arg = 0,
      .write = sgi_hub_write_indicator },
    { .offset = HUB_MD_SN00_INDICATOR_1, .arg = 1,
      .write = sgi_hub_write_indicator },
    { .offset = HUB_IO_WIDGET_ID,
      .read = sgi_hub_read_widget_id, .write = sgi_hub_write_widget_id },
    { .offset = HUB_IO_WIDGET_STATUS,
      .read = sgi_hub_read_widget_status,
      .write = sgi_hub_write_widget_status },
    { .offset = HUB_IO_WIDGET_CONTROL,
      .field = offsetof(SGIHubState, io_widget_control), .width = 8,
      .flags = HUB_REG_DIAG, .store = sgi_hub_store_widget_control },
    { .offset = HUB_IO_LINK_STATUS,
      .field = offsetof(SGIHubState, io_link_status), .width = 8,
      .flags = HUB_REG_DIAG, .store = sgi_hub_store_link_status },
    { .offset = HUB_IO_LLP_LOG,
      .read = sgi_hub_read_llp_log, .write = sgi_hub_write_llp_log },
    { .offset = HUB_IO_SCRATCH_0, .arg = 0,
      .field = offsetof(SGIHubState, io_scratch[0]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .wmask = IO_SCRATCH_MASK },
    { .offset = HUB_IO_SCRATCH_1, .arg = 1,
      .field = offsetof(SGIHubState, io_scratch[1]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_COORD,
      .wmask = IO_SCRATCH_MASK },
    { .offset = HUB_IO_ERROR_CLEAR, .write = sgi_hub_write_error_clear },
    { .offset = HUB_IO_PIO_DEALLOC,
      .read = sgi_hub_read_ioq, .write = sgi_hub_write_ioq },
    { .offset = HUB_IO_CRB_DEALLOC,
      .read = sgi_hub_read_ioq, .write = sgi_hub_write_ioq },
    /* The queue-depth register takes no write; see sgi_hub_write_ioq. */
    { .offset = HUB_IO_IOQ_FIFO_DEPTH, .read = sgi_hub_read_ioq },
    { .offset = HUB_IO_CRB_MGMT,
      .read = sgi_hub_read_ioq, .write = sgi_hub_write_ioq },
    { .offset = HUB_IO_CRB_CONTROL,
      .read = sgi_hub_read_ioq, .write = sgi_hub_write_ioq },
    { .offset = HUB_IO_XTALK_TIMEOUT,
      .field = offsetof(SGIHubState, io_xtalk_timeout), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = IO_XTALK_TIMEOUT_MASK },
    { .offset = HUB_IO_XTALK_PRESCALE,
      .field = offsetof(SGIHubState, io_xtalk_prescale), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG,
      .wmask = IO_XTALK_PRESCALE_MASK },
    { .offset = HUB_NI_STATUS,
      .field = offsetof(SGIHubState, ni_status), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_VECTOR | HUB_REG_TRACE_WRITE,
      .store = sgi_hub_store_ni_status },
    { .offset = HUB_NI_PORT_RESET, .write = sgi_hub_write_port_reset },
    /*
     * Reset permission and link diagnostics: storage nothing consults
     * or acts on. Both answer the remote vector decode -- IRIX
     * 6.5.30's fail-soft burst vector-reads and vector-writes the
     * diagnostic parameters at every boot, so on a two-chassis
     * machine an address-error completion there is a phantom failure
     * in the OS.
     */
    { .offset = HUB_NI_PROTECTION,
      .field = offsetof(SGIHubState, ni_protection), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR,
      .wmask = NI_PROTECTION_MASK },
    { .offset = HUB_NI_SCRATCH_0, .arg = 0,
      .field = offsetof(SGIHubState, ni_scratch[0]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR |
               HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE,
      .wmask = ~0ULL, .write = sgi_hub_write_ni_scratch },
    { .offset = HUB_NI_SCRATCH_1, .arg = 1,
      .field = offsetof(SGIHubState, ni_scratch[1]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR |
               HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE,
      .wmask = ~0ULL, .write = sgi_hub_write_ni_scratch },
    { .offset = HUB_NI_DIAG_PARMS,
      .field = offsetof(SGIHubState, ni_diag_parms), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR,
      .wmask = NI_DIAG_PARMS_MASK },
    { .offset = HUB_NI_VECTOR_PARMS, .write = sgi_hub_write_vector_parms },
    { .offset = HUB_NI_VECTOR,
      .field = offsetof(SGIHubState, ni_vector), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .wmask = ~0ULL },
    { .offset = HUB_NI_VECTOR_DATA,
      .field = offsetof(SGIHubState, ni_vector_data), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .wmask = ~0ULL },
    { .offset = HUB_NI_VECTOR_STATUS,
      .field = offsetof(SGIHubState, ni_vector_status), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_READ,
      .write = sgi_hub_write_vector_status },
    { .offset = HUB_NI_RETURN_VECTOR,
      .field = offsetof(SGIHubState, ni_return_vector), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = ~0ULL },
    { .offset = HUB_NI_VECTOR_READ_DATA,
      .field = offsetof(SGIHubState, ni_vector_read_data), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_TRACE_READ,
      .wmask = ~0ULL },
    /* The clear alias's stored value is the status register itself. */
    { .offset = HUB_NI_VECTOR_CLEAR,
      .field = offsetof(SGIHubState, ni_vector_status), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_READ | HUB_REG_TRACE_WRITE |
               HUB_REG_READ_CLEARS,
      .read = sgi_hub_read_vector_clear,
      .write = sgi_hub_write_vector_clear },
    /*
     * PIO protection is storage, never consulted: enforcing a register
     * firmware might not open produces a dead machine with no
     * diagnostic. Both rows answer the remote vector decode -- the
     * stock PROM's link diagnostic opens its cross-link access by
     * vector-writing the remote Hub's protection register before its link
     * reads, and an address-error completion there counts as a phantom
     * link error. The override is write-only, so its row carries only
     * a store and a read keeps the undecoded register's answer.
     */
    { .offset = HUB_NI_IO_PROTECT,
      .field = offsetof(SGIHubState, ni_io_protect), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR,
      .wmask = ~0ULL },
    { .offset = HUB_NI_IO_PROT_OVRRD,
      .flags = HUB_REG_VECTOR,
      .store = sgi_hub_store_io_prot_ovrrd },
    /*
     * All three LLP port registers answer the remote vector decode as
     * well as plain PIO: the stock PROM's link diagnostic vector-reads
     * the remote Hub's error register, and an address-error completion there
     * counts as a phantom link error on every two-chassis boot. The
     * error register's store applies its write-one-to-clear rule. The alias
     * returns and clears the same traffic-driven state on either a full local
     * read or a remote vector read.
     */
    { .offset = HUB_NI_PORT_PARMS,
      .field = offsetof(SGIHubState, ni_port_parms), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR,
      .wmask = NI_PORT_PARMS_MASK },
    { .offset = HUB_NI_PORT_ERROR,
      .field = offsetof(SGIHubState, ni_port_error), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_VECTOR | HUB_REG_WRITE_CLEARS,
      .store = sgi_hub_store_ni_port_error },
    { .offset = HUB_NI_PORT_ERROR_CLEAR,
      .field = offsetof(SGIHubState, ni_port_error), .width = 8,
      .flags = HUB_REG_DIAG | HUB_REG_VECTOR | HUB_REG_READ_CLEARS,
      .read = sgi_hub_read_ni_port_error_clear },
};

static int sgi_hub_reg_compare(const void *key, const void *element)
{
    const SGIHubRegDef *reg = element;
    hwaddr addr = *(const hwaddr *)key;

    if (addr < reg->offset) {
        return -1;
    }
    return addr > reg->offset;
}

/*
 * The table is in offset order, which sgi_hub_realize() asserts, so a decode
 * does not walk it. Decode runs under the BQL, so a walk here is paid by every
 * processor waiting behind the one doing it.
 */
static const SGIHubRegDef *sgi_hub_reg_lookup(hwaddr addr)
{
    return bsearch(&addr, sgi_hub_registers, ARRAY_SIZE(sgi_hub_registers),
                   sizeof(sgi_hub_registers[0]), sgi_hub_reg_compare);
}

static uint64_t sgi_hub_read_internal(void *opaque, hwaddr addr,
                                      unsigned size)
{
    SGIHubState *s = opaque;
    const SGIHubRegDef *reg;
    uint8_t *route;
    unsigned int n;
    hwaddr bte_reg;

    if (sgi_hub_bwin_entry(addr, &n)) {
        return s->io_bwin[n];
    }
    if (sgi_hub_bte_entry(addr, &n, &bte_reg)) {
        return sgi_hub_bte_read(s, n, bte_reg);
    }
    route = sgi_hub_route_entry(s, addr);
    if (route) {
        return *route;
    }
    if (sgi_hub_icrb_c_entry(addr)) {
        /* No slot has ever tracked a transaction. */
        return 0;
    }
    if (sgi_hub_md_error_entry(addr)) {
        /* No error is ever raised; firmware's memory test requires zero. */
        return 0;
    }

    reg = sgi_hub_reg_lookup(addr);
    if (!reg || (!reg->read && !sgi_hub_reg_peekable(reg))) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: read of unimplemented register 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
    if (reg->read) {
        return reg->read(s, addr, reg->arg);
    }
    return sgi_hub_reg_peek(s, reg);
}


static bool sgi_hub_diagnostic_value(SGIHubState *s, hwaddr addr,
                                     uint64_t write_value, uint64_t *value)
{
    const SGIHubRegDef *reg;
    uint8_t *route;
    unsigned int n;
    hwaddr bte_reg;

    if (sgi_hub_bwin_entry(addr, &n)) {
        *value = s->io_bwin[n];
        return true;
    }
    if (sgi_hub_bte_entry(addr, &n, &bte_reg)) {
        *value = sgi_hub_bte_read(s, n, bte_reg);
        return true;
    }
    route = sgi_hub_route_entry(s, addr);
    if (route) {
        *value = *route;
        return true;
    }
    /* Interrupt modify is an instruction; record the bank it names. */
    if (addr == HUB_PI_INT_MODIFY) {
        *value = s->pi_int_pend[
            (write_value & PI_INT_MODIFY_NUMBER) / SGI_HUB_BANK_BITS];
        return true;
    }

    reg = sgi_hub_reg_lookup(addr);
    if (reg && (reg->flags & HUB_REG_DIAG)) {
        *value = sgi_hub_reg_peek(s, reg);
        return true;
    }
    return false;
}

static bool sgi_hub_diagnostic_read_value(SGIHubState *s, hwaddr addr,
                                          uint64_t *value)
{
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(addr);

    if (reg && (reg->flags & HUB_REG_DIAG) &&
        (reg->flags & HUB_REG_READ_CLEARS)) {
        *value = sgi_hub_reg_peek(s, reg);
        return true;
    }
    return false;
}

/*
 * The registers a node's processors coordinate through, narrow enough to
 * trace a whole boot. The pending banks answer their own event: the
 * processors waiting to be launched poll bank zero for the whole of a
 * boot, tens of millions of times, and the other registers are quiet.
 */
static void sgi_hub_trace_coordination_read(SGIHubState *s, hwaddr addr,
                                            uint64_t value, unsigned int size)
{
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(addr & ~(hwaddr)7);
    bool pending;
    CPUState *cpu;
    uint64_t pc;
    int index;

    if (!reg || !(reg->flags & HUB_REG_TRACE_COORD)) {
        return;
    }
    /* The event names are macro tokens, so each is tested on its own. */
    pending = reg->flags & HUB_REG_TRACE_PENDING;
    if (pending) {
        if (!trace_event_get_state_backends(TRACE_SGI_HUB_PENDING_READ)) {
            return;
        }
    } else if (!trace_event_get_state_backends(
                   TRACE_SGI_HUB_COORDINATION_READ)) {
        return;
    }

    cpu = current_cpu;
    index = cpu ? cpu->cpu_index : -1;
    pc = cpu ? cpu->cc->get_pc(cpu) : 0;
    if (pending) {
        trace_sgi_hub_pending_read(
            index, s->cpu_base, sgi_hub_programmed_nasid(s), pc,
            addr, value, size, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    } else {
        trace_sgi_hub_coordination_read(
            index, s->cpu_base, sgi_hub_programmed_nasid(s), pc,
            addr, value, size, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

/*
 * A write is traced once it has landed, so anything it set off is
 * recorded first.
 */

/*
 * The stored value is the one the diagnostic shadow samples after a
 * write, so interrupt modify reports the bank it named; a high-half write
 * reaches that register shifted up, naming bank 0. The shadow samples
 * nothing for processor number, presence or the pending banks, which no
 * write changes, and those report zero.
 */
static void sgi_hub_trace_coordination_write(SGIHubState *s, hwaddr addr,
                                             uint64_t value,
                                             unsigned int size)
{
    hwaddr aligned = addr & ~(hwaddr)7;
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(aligned);
    uint64_t delivered = value;
    uint64_t stored = 0;
    CPUState *cpu;

    if (!reg || !(reg->flags & HUB_REG_TRACE_COORD)) {
        return;
    }

    if (size == 4 && !(addr & 4)) {
        delivered = value << 32;
    }
    sgi_hub_diagnostic_value(s, aligned, delivered, &stored);
    cpu = current_cpu;
    trace_sgi_hub_coordination_write(
        cpu ? cpu->cpu_index : -1, s->cpu_base,
        sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
        addr, value, size, stored,
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/*
 * A 32-bit access names half of the aligned doubleword (offset 0 is bits
 * 63:32), composed here rather than by impl.max_access_size = 8, whose
 * read-modify-write would fire read-to-clear side effects on a store. What
 * a half read of a read-to-clear register does on hardware is unobserved;
 * the conservative choice is that only a full-width read clears.
 */
static uint64_t sgi_hub_read_half(SGIHubState *s, hwaddr addr, unsigned size)
{
    hwaddr aligned = addr & ~(hwaddr)7;
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(aligned);
    uint64_t value;

    if (reg && (reg->flags & HUB_REG_READ_CLEARS)) {
        value = sgi_hub_reg_peek(s, reg);
    } else {
        value = sgi_hub_read_internal(s, aligned, size);
    }
    return (addr & 4) ? (uint32_t)value : value >> 32;
}

static uint64_t sgi_hub_read_sized(SGIHubState *s, hwaddr addr, unsigned size)
{
    uint64_t value;

    if (size == 4) {
        value = sgi_hub_read_half(s, addr, size);
    } else {
        value = sgi_hub_read_internal(s, addr, size);
    }
    /* Traced above the halving, as the guest issued it. */
    if (trace_event_get_state_backends(TRACE_SGI_HUB_PROTOCOL_READ)) {
        sgi_hub_trace_protocol_read(s, addr, value, size);
    }
    if (trace_event_get_state_backends(TRACE_SGI_HUB_COORDINATION_READ) ||
        trace_event_get_state_backends(TRACE_SGI_HUB_PENDING_READ)) {
        sgi_hub_trace_coordination_read(s, addr, value, size);
    }
    return value;
}

static uint64_t sgi_hub_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIHubState *s = opaque;
    uint64_t before = 0;
    uint64_t value;
    uint64_t after = 0;
    bool has_before_after;

    if (likely(!s->diagnostic)) {
        return sgi_hub_read_sized(s, addr, size);
    }

    /* The shadow is probed at the aligned doubleword it recognizes. */
    has_before_after =
        sgi_hub_diagnostic_read_value(s, addr & ~(hwaddr)7, &before);
    value = sgi_hub_read_sized(s, addr, size);
    if (has_before_after &&
        !sgi_hub_diagnostic_read_value(s, addr & ~(hwaddr)7, &after)) {
        has_before_after = false;
    }
    device_diagnostic_record(s->diagnostic, "hub", "mmio-read", addr,
                             value, size, has_before_after, before, after);
    return value;
}

static void sgi_hub_write_internal(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    SGIHubState *s = opaque;
    const SGIHubRegDef *reg;
    uint8_t *route;
    unsigned int n;
    hwaddr bte_reg;

    if (sgi_hub_bwin_entry(addr, &n)) {
        /* Stored: OpenBSD's window allocator reads an entry straight back. */
        s->io_bwin[n] = val & IO_BWIN_MASK;
        sgi_hub_bwin_follow(s, n);
        return;
    }
    if (sgi_hub_bte_entry(addr, &n, &bte_reg)) {
        sgi_hub_bte_write(s, n, bte_reg, val);
        return;
    }
    route = sgi_hub_route_entry(s, addr);
    if (route) {
        *route = val & NI_ROUTE_MASK;
        sgi_hub_route_written(s, addr);
        return;
    }
    if (sgi_hub_icrb_c_entry(addr)) {
        /* Filled by transactions, not writes; firmware's sweep stores zero. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only "
                      "incoming-request-buffer register 0x%" HWADDR_PRIx
                      " (real firmware writes 0x0 here every boot; not "
                      "modelled)\n",
                      val, addr);
        return;
    }
    if (sgi_hub_md_error_entry(addr)) {
        /* Nothing latched for a write to clear. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the memory/"
                      "directory error-cluster register 0x%" HWADDR_PRIx
                      " (nothing here ever latches an error to clear)\n",
                      val, addr);
        return;
    }

    reg = sgi_hub_reg_lookup(addr);
    if (reg && reg->write) {
        reg->write(s, addr, reg->arg, val, size);
        return;
    }
    if (reg && sgi_hub_reg_storable(reg)) {
        sgi_hub_reg_store(s, reg, val);
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to unimplemented "
                  "register 0x%" HWADDR_PRIx "\n", val, addr);
}

/*
 * A 32-bit write replaces its half; the other half comes from the shadow, or
 * zero for a register that stores nothing or clears asserted bits. The merged
 * value takes the ordinary write path, the guest's size riding along.
 */
static void sgi_hub_write_half(SGIHubState *s, hwaddr addr, uint64_t val,
                               unsigned size)
{
    hwaddr aligned = addr & ~(hwaddr)7;
    const SGIHubRegDef *reg = sgi_hub_reg_lookup(aligned);
    uint64_t merged = 0;

    if (aligned == HUB_PI_INT_MODIFY ||
        (reg && (reg->flags & HUB_REG_WRITE_CLEARS)) ||
        !sgi_hub_diagnostic_value(s, aligned, 0, &merged)) {
        merged = 0;
    }
    if (addr & 4) {
        merged = (merged & ~(uint64_t)UINT32_MAX) | (uint32_t)val;
    } else {
        merged = (merged & UINT32_MAX) | (val << 32);
    }
    sgi_hub_write_internal(s, aligned, merged, size);
}

static void sgi_hub_write_sized(SGIHubState *s, hwaddr addr, uint64_t val,
                                unsigned size)
{
    /* Traced above the halving and before any decode. */
    if (trace_event_get_state_backends(TRACE_SGI_HUB_PROTOCOL_WRITE)) {
        sgi_hub_trace_protocol_write(s, addr, val, size);
    }
    if (size == 4) {
        sgi_hub_write_half(s, addr, val, size);
    } else {
        sgi_hub_write_internal(s, addr, val, size);
    }
    if (trace_event_get_state_backends(TRACE_SGI_HUB_COORDINATION_WRITE)) {
        sgi_hub_trace_coordination_write(s, addr, val, size);
    }
}

static void sgi_hub_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    SGIHubState *s = opaque;
    uint64_t before = 0;
    uint64_t after = 0;
    bool has_before;
    bool has_after;

    if (likely(!s->diagnostic)) {
        sgi_hub_write_sized(s, addr, val, size);
        return;
    }

    /* The shadow samples the aligned doubleword. */
    has_before = sgi_hub_diagnostic_value(s, addr & ~(hwaddr)7, val,
                                          &before);
    sgi_hub_write_sized(s, addr, val, size);
    has_after = sgi_hub_diagnostic_value(s, addr & ~(hwaddr)7, val,
                                         &after);
    device_diagnostic_record(s->diagnostic, "hub", "mmio-write", addr,
                             val, size, has_before && has_after,
                             before, after);
}

void sgi_hub_latch_prom_pi_error(SGIHubState *s)
{
    unsigned int slice;

    if (!current_cpu || current_cpu->cpu_index < s->cpu_base ||
        current_cpu->cpu_index >= s->cpu_base + s->cpus) {
        return;
    }
    slice = current_cpu->cpu_index - s->cpu_base;
    s->pi_err_status0[slice] = PI_ERR_STATUS0_VALID |
                               PI_ERR_STATUS0_ADDRESS;
}

static const MemoryRegionOps sgi_hub_ops = {
    .read = sgi_hub_read,
    .write = sgi_hub_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void sgi_hub_reset_enter(Object *obj, ResetType type)
{
    SGIHubState *s = SGI_HUB(obj);
    unsigned int n;

    sgi_hub_rt_hold_reset(s);
    s->pi_region_present = ~0ULL;
    s->pi_calias_size = PI_CALIAS_SIZE_RESET;
    memset(s->pi_cpu_enable, 0, sizeof(s->pi_cpu_enable));
    /* The error-spool addresses survive reset: firmware's reset marker. */
    s->pi_rt_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(s->pi_rt_compare, 0, sizeof(s->pi_rt_compare));
    memset(s->pi_rt_pending, 0, sizeof(s->pi_rt_pending));
    memset(s->pi_rt_enable, 0, sizeof(s->pi_rt_enable));
    s->pi_rt_local_ctrl = PI_RT_LOCAL_CTRL_RESET;
    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        timer_del(s->rt_timer[n]);
    }
    memset(s->pi_int_pend, 0, sizeof(s->pi_int_pend));
    memset(s->pi_int_pend_seen_valid, 0, sizeof(s->pi_int_pend_seen_valid));
    s->pi_int_pend1_primary_credit = false;
    memset(s->pi_int_mask, 0, sizeof(s->pi_int_mask));
    memset(s->pi_err_mask, 0, sizeof(s->pi_err_mask));
    s->pi_err_spool_size = 0;
    memset(s->pi_err_status0, 0, sizeof(s->pi_err_status0));
    s->ni_status = NI_STATUS_RESET |
                   ((uint64_t)s->ni_revision << NI_STATUS_REVISION_SHIFT) |
                   ((uint64_t)s->nasid << NI_STATUS_NODE_SHIFT);
    sgi_hub_link_refresh(s);
    s->ni_scratch[0] = 0;
    s->ni_scratch[1] = 0;
    s->ni_vector = 0;
    s->ni_vector_data = 0;
    s->ni_vector_status = 0;
    s->ni_return_vector = 0;
    s->ni_vector_read_data = 0;
    s->ni_port_parms = NI_PORT_PARMS_RESET;
    s->ni_port_error = 0;
    s->ni_io_protect = NI_IO_PROTECT_RESET;
    s->ni_protection = NI_PROTECTION_RESET;
    s->ni_diag_parms = 0;
    memset(s->ni_meta_route, 0, sizeof(s->ni_meta_route));
    memset(s->ni_local_route, 0, sizeof(s->ni_local_route));
    s->ni_meta_route_written = 0;
    s->ni_local_route_written = 0;

    /*
     * The link comes up reporting itself up, a ratified departure from the
     * documented reset: Linux abandons its crosstalk probe on a down link.
     */
    s->io_link_status = IO_LINK_RESET;
    s->io_widget_control = IO_WIDGET_CONTROL_RESET;
    s->io_xtalk_timeout = 0;
    s->io_xtalk_prescale = 0;
    s->io_scratch[0] = 0;
    s->io_scratch[1] = 0;

    /* No transfer is ever in flight, so zeroing the registers is the reset. */
    memset(s->bte_length, 0, sizeof(s->bte_length));
    memset(s->bte_source, 0, sizeof(s->bte_source));
    memset(s->bte_dest, 0, sizeof(s->bte_dest));
    memset(s->bte_control, 0, sizeof(s->bte_control));
    memset(s->bte_notify, 0, sizeof(s->bte_notify));
    memset(s->bte_interrupt, 0, sizeof(s->bte_interrupt));
    s->md_perf_cnt0 = 0;
    s->md_config = MD_CONFIG_RESET;
    s->md_onewire = MD_ONEWIRE_RESET;
    /* A slot started before reset must not complete after it. */
    s->md_onewire_deadline_ns = INT64_MAX;
    /* The part's own protocol state is reset by the part, as its child. */
    s->onewire_level = true;

    /*
     * Entries reset to zero with the windows enabled: a guest calls a zero
     * entry free, and zero spells widget 0, OpenBSD's only route there.
     * Zeroed entries with disabled windows would pass every Linux test.
     */
    for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
        s->io_bwin[n] = 0;
        sgi_hub_bwin_follow(s, n);
    }
}

/* Output lines move in the hold phase, after every enter phase. */
static void sgi_hub_reset_hold(Object *obj, ResetType type)
{
    SGIHubState *s = SGI_HUB(obj);
    unsigned int cpu;

    sgi_hub_update_irq(s);
    for (cpu = 0; cpu < s->cpus; cpu++) {
        sgi_hub_set_cpu_enable(s, cpu, false, true);
    }
}

/*
 * The fabric's maps re-derive once every Hub's programmed number is back
 * at its seeded value -- the exit phase, because a map depends on every
 * endpoint's number, and every enter phase has run by now. The nameless
 * low windows re-derive with them, back to the reset size.
 */
static void sgi_hub_reset_exit(Object *obj, ResetType type)
{
    SGIHubState *s = SGI_HUB(obj);

    sgi_hub_report_number(s);
    sgi_hub_calias_update(s);
}

bool sgi_hub_attach_module_identity(SGIHubState *s,
                                    const SGIHubModuleIdentity *identity,
                                    Error **errp)
{
    if (qdev_is_realized(DEVICE(s))) {
        error_setg(errp, "cannot attach Hub module identity after realize");
        return false;
    }
    if (s->has_module_identity) {
        error_setg(errp, "Hub already has a module identity responder");
        return false;
    }
    if (identity->rom[0] != MD_MODULE_ROM_FAMILY ||
        onewire_crc8(0, identity->rom, sizeof(identity->rom)) != 0) {
        error_setg(errp, "Hub module identity has an invalid one-wire ROM");
        return false;
    }

    s->module_identity = *identity;
    s->has_module_identity = true;
    return true;
}

static void sgi_hub_realize(DeviceState *dev, Error **errp)
{
    SGIHubState *s = SGI_HUB(dev);
    uint32_t unknown_population;
    unsigned int n;

    /* Decode binary searches the table, so an entry added out of order would
     * silently stop being found. */
    for (n = 1; n < ARRAY_SIZE(sgi_hub_registers); n++) {
        assert(sgi_hub_registers[n - 1].offset < sgi_hub_registers[n].offset);
    }

    if (!s->xtalk) {
        error_setg(errp, "the Hub needs the crosstalk space its big windows "
                         "translate into");
        return;
    }
    if (s->xtalk_io &&
        (s->xtalk_widget_shift != 34 ||
         memory_region_size(s->xtalk_io) != (1ULL << 38))) {
        error_setg(errp, "Hub I/O target requires a 34-bit widget stride "
                         "and a 38-bit address space");
        return;
    }
    if (s->onewire_serial & ~MD_ONEWIRE_SERIAL_MASK) {
        error_setg(errp, "the Hub one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }
    unknown_population = s->population & ~SGI_HUB_POPULATION_ALL;
    if (unknown_population) {
        error_setg(errp, "Hub population has unknown bits 0x%x",
                   unknown_population);
        return;
    }
    if (s->onewire_kind > SGI_HUB_ONEWIRE_BLANK_DS2505) {
        error_setg(errp, "Hub one-wire kind %u is unsupported",
                   s->onewire_kind);
        return;
    }
    if ((s->population & SGI_HUB_POPULATION_ELSC) &&
        !(s->population & SGI_HUB_POPULATION_I2C_CONTROLLER)) {
        error_setg(errp, "Hub ELSC requires I2C");
        return;
    }
    if ((s->population & SGI_HUB_POPULATION_ELSC) &&
        (!s->i2c_elsc_addr || s->i2c_elsc_addr > 0x7f)) {
        error_setg(errp, "Hub ELSC needs a nonzero seven-bit I2C address");
        return;
    }
    if (s->i2c_nvram_pages &&
        !(s->population & SGI_HUB_POPULATION_I2C_CONTROLLER)) {
        error_setg(errp, "Hub I2C NVRAM requires I2C");
        return;
    }
    if (s->i2c_nvram_pages &&
        (!s->i2c_nvram_first_addr || s->i2c_nvram_first_addr > 0x7f ||
         s->i2c_nvram_pages > 0x80 - s->i2c_nvram_first_addr)) {
        error_setg(errp, "Hub I2C NVRAM needs consecutive seven-bit "
                   "addresses");
        return;
    }
    if (s->i2c_nvram_pages > ELSC_NVRAM_PAGE_COUNT) {
        error_setg(errp, "Hub I2C NVRAM has at most %u pages",
                   ELSC_NVRAM_PAGE_COUNT);
        return;
    }
    if (s->ni_revision > NI_STATUS_REVISION_MAX) {
        error_setg(errp, "the Hub NI revision is a four-bit field, so at "
                         "most %u", NI_STATUS_REVISION_MAX);
        return;
    }
    if (s->widget_revision > IO_WIDGET_ID_REVISION_MAX) {
        error_setg(errp, "the Hub widget revision is a four-bit field, so "
                         "at most %u", IO_WIDGET_ID_REVISION_MAX);
        return;
    }
    if (!s->xtalk_widget_shift) {
        error_setg(errp, "the Hub needs the widget stride of the crosstalk "
                         "space its big windows translate into");
        return;
    }
    if (!s->fabric) {
        error_setg(errp, "the Hub needs the fabric its network interface is "
                         "an endpoint on");
        return;
    }
    if (s->fabric_endpoint >= sn0_fabric_endpoint_count(s->fabric)) {
        error_setg(errp, "the Hub's fabric endpoint %u does not exist",
                   s->fabric_endpoint);
        return;
    }
    if (!s->claim_target || !s->claim_node_shift || !s->claim_space_shift) {
        error_setg(errp, "the Hub needs the canonical map and its node and "
                         "space geometry to claim node-qualified strides");
        return;
    }
    for (n = 0; n < SGI_HUB_CLAIM_VIEWS; n++) {
        if (sgi_hub_claim_view_present(s, n) && !s->claim_view[n]) {
            error_setg(errp, "the Hub needs a claim container for each of "
                             "its %u processors' views and for its own",
                       s->cpus);
            return;
        }
    }

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_hub_ops, s,
                          "sgi-hub.regs", SGI_HUB_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    /*
     * The engines address memory as a processor does, by programmed node
     * number through the claim layer, minus the per-processor pieces:
     * their space is the device view and nothing else.
     */
    address_space_init(&s->bte_as, s->claim_view[SGI_HUB_DEVICE_VIEW],
                       "sgi-hub.bte");

    if (s->junk_uart_present) {
        DeviceState *uart;

        object_initialize_child(OBJECT(dev), "uart", &s->junk_uart,
                                TYPE_SERIAL_MM);
        uart = DEVICE(&s->junk_uart);
        qdev_prop_set_uint8(uart, "regshift", HUB_MD_JUNKUART_REGSHIFT);
        qdev_prop_set_uint32(uart, "baudbase", HUB_MD_JUNKUART_CLOCK);
        qdev_prop_set_chr(uart, "chardev", s->junk_uart_chr);
        /* The vmstate identity serial_mm_init() gave the port. */
        qdev_set_legacy_instance_id(uart, HUB_MD_JUNKUART_BASE, 2);
        qdev_prop_set_uint8(uart, "endianness", DEVICE_BIG_ENDIAN);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->junk_uart), errp)) {
            return;
        }
        /* A subregion of s->regs, so both the machine's mappings reach it. */
        memory_region_add_subregion(
            &s->regs, HUB_MD_JUNKUART_BASE,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->junk_uart), 0));
    }

    if (s->population & SGI_HUB_POPULATION_I2C_CONTROLLER) {
        s->i2c.diagnostic = s->diagnostic;
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c), errp)) {
            return;
        }
        /* Above the Hub registers, so these bytes reach the controller. */
        memory_region_add_subregion_overlap(
            &s->regs, s->i2c_controller_offset,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->i2c), 0), 1);
    } else {
        object_unparent(OBJECT(&s->i2c));
    }

    /*
     * The module system controller (MSC): a real board on the same I2C bus,
     * not something the Hub reaches through a window, so an ordinary
     * addressed slave on the chip's own bus is nearly the whole of the
     * wiring. It also needs to know how many processors' arbitration
     * signals to rotate through (this node's own populated count), and the
     * chardev behind its AUX serial port. The machine resolves that
     * backend and hands it in through the elsc-chardev property, because
     * every tower has its own physical AUX connector and only the machine
     * knows which -serial index this node's connector answers to; a Hub
     * that resolved an index itself would claim the same backend once per
     * node. NULL is a connector with no cable, exactly like real
     * hardware. The firmware redirects its console to this port after a
     * failed power-on diagnostic; the IRIX console (console=d) is IOC3
     * serial A, not this one.
     */
    if (s->population & SGI_HUB_POPULATION_ELSC) {
        I2CSlave *elsc = i2c_slave_new(TYPE_SGI_ELSC, s->i2c_elsc_addr);

        object_property_add_child(OBJECT(s), "elsc", OBJECT(elsc));
        qdev_prop_set_uint32(DEVICE(elsc), "num-cpus", s->cpus);
        qdev_prop_set_uint32(DEVICE(elsc), "cpu-base", s->cpu_base);
        qdev_prop_set_bit(DEVICE(elsc), "arbitration", !s->slave_module);
        qdev_prop_set_uint8(DEVICE(elsc), "dip-switches",
                            s->elsc_dip_switches);
        qdev_prop_set_chr(DEVICE(elsc), "chardev", s->elsc_chr);
        s->elsc = SGI_ELSC(elsc);
        s->elsc->diagnostic = s->diagnostic;
        if (!i2c_slave_realize_and_unref(elsc, s->i2c.bus, errp)) {
            s->elsc = NULL;
            return;
        }
    }

    /*
     * The controller's 2 KiB NVRAM: eight I2C addresses of one 256-byte page
     * each. Page 7 is the reserved page the PROM reads before probing a
     * console; only its validity marker has a required value.
     */
    if (s->i2c_nvram_pages) {
        static const uint8_t reserved_page_defaults[16] = { 0x37 };

        for (n = 0; n < s->i2c_nvram_pages; n++) {
            uint8_t address = s->i2c_nvram_first_addr + n;

            if (n == ELSC_NVRAM_RESERVED_PAGE) {
                at24c_eeprom_init_rom(s->i2c.bus, address,
                                     ELSC_NVRAM_PAGE_SIZE,
                                     reserved_page_defaults,
                                     sizeof(reserved_page_defaults));
            } else {
                at24c_eeprom_init(s->i2c.bus, address,
                                  ELSC_NVRAM_PAGE_SIZE);
            }
        }
    }

    /* Each list is indexed by processor slice, so a consumer names its line. */
    qdev_init_gpio_out_named(dev, s->bank_irq[0], "bank0",
                             SGI_HUB_PROCESSORS);
    qdev_init_gpio_out_named(dev, s->bank_irq[1], "bank1",
                             SGI_HUB_PROCESSORS);
    qdev_init_gpio_out_named(dev, s->rt_irq, "rt", SGI_HUB_PROCESSORS);

    /* One crosstalk link, so one line: what the crossbar drives at us. */
    qdev_init_gpio_in_named(dev, sgi_hub_link_reset, "link-reset", 1);

    /*
     * The MicroLAN wire and the PIMM record part hanging off it, hung here
     * and never again. An EPROM keeps its contents across a reset -- that is
     * what the technology is -- so a chip that rebuilt a part's bytes on
     * every reset would be modelling something the hardware does not do.
     * Those bytes are the part's own migrated state as well, and state with
     * one owner wants one writer. Reaching into a child device's storage
     * from a parent's reset is the wrong shape whichever order the phases
     * run in, and they run in the safe one: hw/core/resettable.c holds
     * children before the object itself, so the ordering was never the
     * hazard.
     *
     * Nothing is composed here in any case. What the record should say is an
     * open question, and a DS2505 left unprogrammed reads 0xff everywhere,
     * which states nothing rather than inventing a serial number the
     * firmware would parse into this machine's own.
     */
    s->onewire_bus = onewire_create_bus(dev, "onewire");
    if (s->onewire_kind == SGI_HUB_ONEWIRE_BLANK_DS2505) {
        DeviceState *part = qdev_new(TYPE_ONEWIRE_DS2505);

        object_property_add_child(OBJECT(dev), "onewire-part", OBJECT(part));
        qdev_prop_set_uint8(part, "family", MD_ONEWIRE_ROM_FAMILY);
        qdev_prop_set_uint64(part, "serial", s->onewire_serial);
        if (!qdev_realize_and_unref(part, BUS(s->onewire_bus), errp)) {
            return;
        }
    }
    if (s->has_module_identity) {
        DeviceState *part = qdev_new(TYPE_ONEWIRE_DS2502);
        uint64_t serial = 0;

        for (n = 0; n < 6; n++) {
            serial |= (uint64_t)s->module_identity.rom[1 + n] << (8 * n);
        }
        object_property_add_child(OBJECT(dev), "module-identity",
                                  OBJECT(part));
        qdev_prop_set_uint8(part, "family", MD_MODULE_ROM_FAMILY);
        qdev_prop_set_uint64(part, "serial", serial);
        ds2502_set_image(part, &s->module_identity.page[0][0],
                         sizeof(s->module_identity.page));
        if (!qdev_realize_and_unref(part, BUS(s->onewire_bus), errp)) {
            return;
        }
    }

    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        s->rt_line[n].hub = s;
        s->rt_line[n].index = n;
        s->rt_timer[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_hub_rt_expire,
                                      &s->rt_line[n]);
    }

    memory_region_init(&s->bwin, OBJECT(dev), "sgi-hub.bigwin",
                       SGI_HUB_BWIN_TOTAL);
    memory_region_init_io(&s->bwin_empty, OBJECT(dev), &sgi_hub_bwin_empty_ops,
                          s, "sgi-hub.bigwin.empty", SGI_HUB_BWIN_TOTAL);
    memory_region_add_subregion_overlap(&s->bwin, 0, &s->bwin_empty, -1);
    for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
        char *name = g_strdup_printf("sgi-hub.bigwin%u", n);

        memory_region_init_alias(&s->bwin_window[n], OBJECT(dev), name,
                                 s->xtalk, 0, SGI_HUB_BWIN_SIZE);
        memory_region_add_subregion(&s->bwin, (hwaddr)n * SGI_HUB_BWIN_SIZE,
                                    &s->bwin_window[n]);
        if (s->xtalk_io) {
            memory_region_init_alias(&s->bwin_io_window[n], OBJECT(dev),
                                     name, s->xtalk_io, 0, SGI_HUB_BWIN_SIZE);
            memory_region_set_enabled(&s->bwin_io_window[n], false);
            memory_region_add_subregion_overlap(&s->bwin,
                                                 (hwaddr)n * SGI_HUB_BWIN_SIZE,
                                                 &s->bwin_io_window[n], 0);
        }
        g_free(name);
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->bwin);

    /*
     * The claim overlays, disabled until the fabric's first map aims them
     * (the PAM idiom: pre-created alternatives, switched, never
     * recreated). One error floor per view and space covers every node
     * number's stride, so an unclaimed stride fails rather than falling
     * through to the static placement; one alias per fabric endpoint
     * moves to wherever that endpoint's number is.
     */
    memory_region_transaction_begin();
    for (n = 0; n < SGI_HUB_CLAIM_VIEWS; n++) {
        unsigned int endpoints = sn0_fabric_endpoint_count(s->fabric);
        const char *view_name = n == SGI_HUB_DEVICE_VIEW ? "engines" : "cpu";
        unsigned int space, i;

        if (!sgi_hub_claim_view_present(s, n)) {
            continue;
        }
        for (space = 0; space < SGI_HUB_CLAIM_SPACES; space++) {
            hwaddr space_base = (hwaddr)space << s->claim_space_shift;
            uint64_t size = 1ULL << s->claim_node_shift;
            char *name;

            name = g_strdup_printf("sgi-hub.claim.%s%u.space%u.error",
                                   view_name, n, space);
            memory_region_init_io(&s->claim_error[n][space], OBJECT(dev),
                                  &sgi_hub_claim_error_ops, s, name,
                                  (uint64_t)SN0_FABRIC_NUMBERS <<
                                  s->claim_node_shift);
            /*
             * An engine's transfer that lands on the floor re-enters this
             * Hub from inside the register write that launched it. The
             * floor holds no state the re-entrancy guard could protect, so
             * the guard is lifted for it alone; the register region keeps it.
             */
            s->claim_error[n][space].disable_reentrancy_guard = true;
            memory_region_add_subregion_overlap(s->claim_view[n], space_base,
                                                &s->claim_error[n][space], 0);
            g_free(name);

            s->claim_alias[n][space] = g_new0(MemoryRegion, endpoints);
            for (i = 0; i < endpoints; i++) {
                MemoryRegion *alias = &s->claim_alias[n][space][i];

                name = g_strdup_printf(
                    "sgi-hub.claim.%s%u.space%u.endpoint%u",
                    view_name, n, space, i);
                memory_region_init_alias(alias, OBJECT(dev), name,
                                         s->claim_target, space_base, size);
                memory_region_set_enabled(alias, false);
                memory_region_add_subregion_overlap(s->claim_view[n],
                                                    space_base, alias, 1);
                g_free(name);
            }
        }
    }
    memory_region_transaction_commit();
    s->claim_ready = true;
    sgi_hub_fabric_changed(s);
}

/* The overlays and the engines' space go with the device. */
static void sgi_hub_unrealize(DeviceState *dev)
{
    SGIHubState *s = SGI_HUB(dev);
    unsigned int endpoints = sn0_fabric_endpoint_count(s->fabric);
    unsigned int view, space, i;

    sgi_hub_rt_hold_reset(s);
    s->claim_ready = false;
    address_space_destroy(&s->bte_as);
    for (view = 0; view < SGI_HUB_CLAIM_VIEWS; view++) {
        if (!sgi_hub_claim_view_present(s, view)) {
            continue;
        }
        for (space = 0; space < SGI_HUB_CLAIM_SPACES; space++) {
            for (i = 0; i < endpoints; i++) {
                MemoryRegion *alias = &s->claim_alias[view][space][i];

                memory_region_del_subregion(s->claim_view[view], alias);
                object_unparent(OBJECT(alias));
            }
            g_free(s->claim_alias[view][space]);
            s->claim_alias[view][space] = NULL;
            memory_region_del_subregion(s->claim_view[view],
                                        &s->claim_error[view][space]);
            object_unparent(OBJECT(&s->claim_error[view][space]));
        }
    }
}

static int sgi_hub_post_load(void *opaque, int version_id);

static const VMStateDescription vmstate_sgi_hub = {
    .name = "sgi-hub",
    .version_id = 26,
    .minimum_version_id = 1,
    .post_load = sgi_hub_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(pi_region_present, SGIHubState),
        VMSTATE_UINT8_ARRAY(pi_cpu_enable, SGIHubState, 2),
        VMSTATE_INT64(pi_rt_origin_ns, SGIHubState),
        VMSTATE_UINT64_ARRAY(pi_rt_compare, SGIHubState, SGI_HUB_PROCESSORS),
        VMSTATE_UINT8_ARRAY(pi_rt_pending, SGIHubState, SGI_HUB_PROCESSORS),
        VMSTATE_UINT8_ARRAY(pi_rt_enable, SGIHubState, SGI_HUB_PROCESSORS),
        VMSTATE_TIMER_PTR_ARRAY(rt_timer, SGIHubState, SGI_HUB_PROCESSORS),
        VMSTATE_UINT32(pi_calias_size, SGIHubState),
        VMSTATE_UINT64_ARRAY(pi_int_pend, SGIHubState, 2),
        VMSTATE_BOOL_V(pi_int_pend1_primary_credit, SGIHubState, 17),
        VMSTATE_UINT64_ARRAY(pi_int_mask, SGIHubState, 4),
        VMSTATE_UINT64(ni_status, SGIHubState),
        VMSTATE_UINT32_V(ni_meta_route_written, SGIHubState, 24),
        VMSTATE_UINT16_V(ni_local_route_written, SGIHubState, 24),
        VMSTATE_UINT64_ARRAY_V(ni_scratch, SGIHubState, 2, 14),
        VMSTATE_UINT64_V(ni_vector, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_vector_data, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_vector_status, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_return_vector, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_vector_read_data, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_port_parms, SGIHubState, 20),
        VMSTATE_UINT64_V(ni_port_error, SGIHubState, 25),
        VMSTATE_UINT64_V(ni_io_protect, SGIHubState, 21),
        VMSTATE_UINT64_V(ni_protection, SGIHubState, 22),
        VMSTATE_UINT64_V(ni_diag_parms, SGIHubState, 22),
        VMSTATE_UINT8_ARRAY_V(ni_meta_route, SGIHubState,
                             SGI_HUB_META_ROUTES, 16),
        VMSTATE_UINT8_ARRAY_V(ni_local_route, SGIHubState,
                             SGI_HUB_LOCAL_ROUTES, 16),
        VMSTATE_UINT64_V(io_link_status, SGIHubState, 3),
        VMSTATE_UINT64_V(io_widget_control, SGIHubState, 4),
        VMSTATE_UINT64_V(io_xtalk_timeout, SGIHubState, 4),
        VMSTATE_UINT64_V(io_xtalk_prescale, SGIHubState, 4),
        /* Version-gated so a stream from before the table still loads. */
        VMSTATE_UINT32_ARRAY_V(io_bwin, SGIHubState, SGI_HUB_BWIN_ENTRIES, 2),
        VMSTATE_UINT64_ARRAY_V(io_scratch, SGIHubState, 2, 8),
        VMSTATE_UINT64_V(md_perf_cnt0, SGIHubState, 8),
        VMSTATE_UINT64_V(md_config, SGIHubState, 9),
        VMSTATE_UINT64_V(md_onewire, SGIHubState, 11),
        VMSTATE_INT64_V(md_onewire_deadline_ns, SGIHubState, 26),
        VMSTATE_UINT64_ARRAY_V(pi_err_spool, SGIHubState, 2, 13),
        VMSTATE_UINT32_ARRAY_V(pi_err_mask, SGIHubState,
                              SGI_HUB_PROCESSORS, 18),
        VMSTATE_UINT8_V(pi_err_spool_size, SGIHubState, 18),
        VMSTATE_UINT64_ARRAY_V(pi_err_status0, SGIHubState,
                              SGI_HUB_PROCESSORS, 19),
        VMSTATE_UINT64_V(pi_rt_local_ctrl, SGIHubState, 10),
        /* The registers are the whole of an engine's state. */
        VMSTATE_UINT64_ARRAY_V(bte_length, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_source, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_dest, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_control, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_notify, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_interrupt, SGIHubState, SGI_HUB_BTES, 17),
        /*
         * The level the wire sampled in the slot now running, which is
         * this master's own state: it is what the timer reports when the
         * slot's deadline arrives. The responder's protocol state used to
         * sit beside it and now belongs to the part on the bus, travelling
         * in the part's own stream. The unused spans keep a stream from
         * before that move loading, in the field order it was written --
         * phase, bit, command and match ahead of the level; address bytes,
         * staged answer, cursor and end pair behind it. A half-finished
         * conversation in such a stream is discarded rather than
         * translated.
         *
         * Discarding is not what a bus reset does. A reset resets the master
         * too; a load does not. A guest resumed from a stream written before
         * the move keeps talking to a part that is now idle, and its next
         * read slots are sampled as a command byte of all ones -- measured,
         * one "unknown ROM command 0xff" on resume, absent from every boot
         * that did not migrate. It is benign: the guest recovers at its next
         * reset pulse. Streams written since the move carry the part's own
         * section and preserve the conversation intact.
         */
        VMSTATE_UNUSED_V(23, 11),
        VMSTATE_BOOL_V(onewire_level, SGIHubState, 23),
        VMSTATE_UNUSED_V(23, 51),
        VMSTATE_END_OF_LIST()
    }
};

/* The windows' aim is re-derived from the restored entries. */
static int sgi_hub_post_load(void *opaque, int version_id)
{
    SGIHubState *s = opaque;
    unsigned int n;

    sgi_hub_rt_hold_reset(s);
    if (s->ni_port_error & ~NI_PORT_ERROR_MASK) {
        return -EINVAL;
    }

    if (version_id < 17) {
        s->pi_int_pend1_primary_credit = false;
    }

    if (version_id < 16) {
        memset(s->ni_meta_route, 0, sizeof(s->ni_meta_route));
        memset(s->ni_local_route, 0, sizeof(s->ni_local_route));
    }

    if (version_id < 24) {
        s->ni_meta_route_written = 0;
        s->ni_local_route_written = 0;
    }

    for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
        sgi_hub_bwin_follow(s, n);
    }
    /*
     * The fabric's maps re-derive from the just-restored programmed
     * number. A map also depends on every other endpoint's number, which
     * may not have loaded yet -- the recompute in whichever Hub's section
     * loads last leaves every map aimed by restored state.
     */
    sgi_hub_report_number(s);
    /* The nameless low windows re-derive from the restored size code. */
    sgi_hub_calias_update(s);
    /*
     * Re-driving outputs from post_load is sound here only because each
     * drives an idempotent level input on the CPU, whose cause bits migrate
     * with it; an edge-detecting consumer would not converge.
     */
    sgi_hub_update_irq(s);
    return 0;
}

static const Property sgi_hub_properties[] = {
    DEFINE_PROP_UINT32("nasid", SGIHubState, nasid, 0),
    DEFINE_PROP_UINT32("physical-slot", SGIHubState, physical_slot, 0),
    DEFINE_PROP_BOOL("slave-module", SGIHubState, slave_module, false),
    DEFINE_PROP_LINK("fabric", SGIHubState, fabric, TYPE_SN0_FABRIC,
                     SN0FabricState *),
    DEFINE_PROP_UINT32("fabric-endpoint", SGIHubState, fabric_endpoint, 0),
    DEFINE_PROP_UINT32("cpus", SGIHubState, cpus, 1),
    DEFINE_PROP_UINT8("ni-revision", SGIHubState, ni_revision, 3),
    DEFINE_PROP_UINT8("widget-revision", SGIHubState, widget_revision, 1),
    DEFINE_PROP_UINT8("elsc-dip-switches", SGIHubState,
                      elsc_dip_switches, 0),
    DEFINE_PROP_UINT32("cpu-base", SGIHubState, cpu_base, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIHubState, onewire_serial,
                       0x0c0b0a090807ULL),
    DEFINE_PROP_UINT32("population", SGIHubState, population,
                       SGI_HUB_POPULATION_ALL),
    DEFINE_PROP_UINT64("i2c-controller-offset", SGIHubState,
                       i2c_controller_offset, HUB_MD_I2C_BASE),
    DEFINE_PROP_UINT8("i2c-elsc-addr", SGIHubState, i2c_elsc_addr, 0),
    DEFINE_PROP_UINT8("i2c-nvram-first-addr", SGIHubState,
                      i2c_nvram_first_addr, 0),
    DEFINE_PROP_UINT32("i2c-nvram-pages", SGIHubState, i2c_nvram_pages,
                       ELSC_NVRAM_PAGE_COUNT),
    DEFINE_PROP_UINT8("onewire-kind", SGIHubState, onewire_kind,
                      SGI_HUB_ONEWIRE_BLANK_DS2505),
    DEFINE_PROP_UINT32("manufacturer", SGIHubState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("xtalk-widget-shift", SGIHubState,
                       xtalk_widget_shift, 0),
    DEFINE_PROP_LINK("xtalk-io", SGIHubState, xtalk_io, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("xtalk", SGIHubState, xtalk, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("claim-view0", SGIHubState, claim_view[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("claim-view1", SGIHubState, claim_view[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("claim-device-view", SGIHubState,
                     claim_view[SGI_HUB_DEVICE_VIEW], TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("claim-target", SGIHubState, claim_target,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("calias-window0", SGIHubState, calias_window[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("calias-window1", SGIHubState, calias_window[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("claim-node-shift", SGIHubState, claim_node_shift, 0),
    DEFINE_PROP_UINT32("claim-space-shift", SGIHubState, claim_space_shift, 0),
    DEFINE_PROP_BOOL("junk-uart", SGIHubState, junk_uart_present, false),
    DEFINE_PROP_LINK("elsc-chardev", SGIHubState, elsc_chr,
                     TYPE_CHARDEV, Chardev *),
    DEFINE_PROP_LINK("junk-uart-chardev", SGIHubState, junk_uart_chr,
                     TYPE_CHARDEV, Chardev *),
};

static void sgi_hub_init(Object *obj)
{
    SGIHubState *s = SGI_HUB(obj);

    object_initialize_child(obj, "i2c", &s->i2c, TYPE_PCF8584);
    s->rt_clk = qdev_init_clock_in(DEVICE(obj), "rt-clk", NULL, NULL, 0);
    clock_set_hz(s->rt_clk, PI_RT_DEFAULT_HZ);
}

static void sgi_hub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI SN0 Hub ASIC";
    dc->realize = sgi_hub_realize;
    dc->unrealize = sgi_hub_unrealize;
    /* The machine supplies the crosstalk space, the fabric and the views. */
    dc->user_creatable = false;
    dc->vmsd = &vmstate_sgi_hub;
    device_class_set_props(dc, sgi_hub_properties);
    rc->phases.enter = sgi_hub_reset_enter;
    rc->phases.hold = sgi_hub_reset_hold;
    rc->phases.exit = sgi_hub_reset_exit;
}

static const TypeInfo sgi_hub_types[] = {
    {
        .name = TYPE_SGI_HUB,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIHubState),
        .instance_init = sgi_hub_init,
        .class_init = sgi_hub_class_init,
    },
};

DEFINE_TYPES(sgi_hub_types)
