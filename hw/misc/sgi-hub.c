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
#include "hw/1wire/ds2505.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/nvram/eeprom_at24c.h"
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
#include "qemu/host-utils.h"
#include "system/system.h"
#include "system/runstate.h"
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
#define IO_WIDGET_ID_REVISION   (1ULL << 28)
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

#define ELSC_NVRAM_FIRST_ADDR   0x50
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
 * Real-time counter: 52 bits at 1.25 MHz, derived from the virtual clock so
 * it ticks between two reads a few instructions apart, which Linux's arming
 * depends on. A write moves the origin, not the count.
 */
#define HUB_PI_RT_COUNT         0x030100
#define HUB_PI_RT_COMPARE_A     0x000108
#define HUB_PI_RT_COMPARE_B     0x000110
#define HUB_PI_RT_PENDING_A     0x000120
#define HUB_PI_RT_PENDING_B     0x000128
#define HUB_PI_RT_ENABLE_A      0x000140
#define HUB_PI_RT_ENABLE_B      0x000148
#define PI_RT_HZ                1250000ULL
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
 * deliberate data errors -- every one acting on a link this machine
 * does not model, so the bits are stored and never acted on. The
 * network-interface datasheet defines no reset value for the
 * port-timeout bit and no nonzero reset for the rest; zero at reset
 * is the model's stated choice, the port-error precedent.
 */
#define HUB_NI_DIAG_PARMS       0x600110
#define NI_DIAG_PARMS_MASK      0x7ffffULL

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
 * maximum burst in bits 9:0. The error register and its clearing alias
 * read zero as a stated modeling choice, not a hardware reset value:
 * the chip leaves the register undefined at reset, and this machine
 * has no fabric that could latch an error for either to report, so the
 * alias's read-to-clear is a no-op. The Origami project tree's
 * network-interface datasheet (docs/datasheets/hub-network-interface.md
 * there) carries the field accounts and the choice's justification.
 */
#define HUB_NI_PORT_PARMS       0x608000
#define HUB_NI_PORT_ERROR       0x608008
#define HUB_NI_PORT_ERROR_CLEAR 0x608088
#define NI_PORT_PARMS_MASK      0xffffULL
#define NI_PORT_PARMS_RESET     ((6ULL << 10) | 0x3f0)

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
    /* Widget I/O space is not modelled; the window is left reaching nothing. */
    if (entry & IO_BWIN_SPACE) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: big window %u asks for widget %" PRIu64 "'s "
                      "I/O space, which this machine does not model; it will "
                      "reach no widget\n", n, widget);
        memory_region_set_enabled(&s->bwin_window[n], false);
        return;
    }

    memory_region_set_alias_offset(&s->bwin_window[n],
                                   (widget << s->xtalk_widget_shift) | slice);
    memory_region_set_enabled(&s->bwin_window[n], true);
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

/*
 * Aim this Hub's claim aliases by the programmed node numbers: a stride
 * equal to this Hub's number is local, one equal to the directly wired
 * peer's routes there, anything else drops to the error floor. Own
 * number first: while both Hubs still hold the reset value zero, every
 * node's node-field-zero access must stay on its own Hub.
 */
static void sgi_hub_claim_update(SGIHubState *s)
{
    unsigned int view, space, n;
    unsigned int strides = s->network_peer ? SGI_HUB_CLAIM_STRIDES : 1;

    memory_region_transaction_begin();
    for (view = 0; view < s->cpus; view++) {
        for (space = 0; space < SGI_HUB_CLAIM_SPACES; space++) {
            for (n = 0; n < strides; n++) {
                uint32_t stride = n ? s->claim_peer_node : s->claim_own_node;
                MemoryRegion *alias = &s->claim_alias[view][space][n];

                if (stride == sgi_hub_programmed_nasid(s)) {
                    memory_region_set_alias_offset(alias,
                        ((hwaddr)space << s->claim_space_shift) |
                        ((hwaddr)s->claim_own_node << s->claim_node_shift));
                    memory_region_set_enabled(alias, true);
                } else if (s->network_peer && stride ==
                           sgi_hub_programmed_nasid(s->network_peer)) {
                    memory_region_set_alias_offset(alias,
                        ((hwaddr)space << s->claim_space_shift) |
                        ((hwaddr)s->claim_peer_node << s->claim_node_shift));
                    memory_region_set_enabled(alias, true);
                } else {
                    memory_region_set_enabled(alias, false);
                }
            }
        }
    }
    memory_region_transaction_commit();
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
 * One programmed number decides claims in both Hubs' views, so a store,
 * reset or load re-derives the pair; each call touches only the regions
 * that Hub owns, and the transaction makes the switch atomic.
 */
static void sgi_hub_claim_update_pair(SGIHubState *s)
{
    memory_region_transaction_begin();
    sgi_hub_claim_update(s);
    if (s->network_peer) {
        sgi_hub_claim_update(s->network_peer);
    }
    memory_region_transaction_commit();
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

/*
 * Clearing the bit removes the processor: the firmware's halt routine writes
 * zero as its last act and must not return. Requested, not forced, because
 * this runs inside a memory write.
 */
static void sgi_hub_write_cpu_enable(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    CPUState *cs = qemu_get_cpu(s->cpu_base + arg);

    s->pi_cpu_enable[arg] = val & 1;

    if (!(val & 1) && cs) {
        cpu_interrupt(cs, CPU_INTERRUPT_HALT);
    }
}

/*
 * Wake the peer and end this processor's slice; no successor is chosen.
 * Firmware polls a peer through Hub registers with an instruction-count
 * timeout one slice can exhaust before the peer runs. Changes no Hub state.
 */
static void sgi_hub_yield_to_peer(CPUState *peer)
{
    if (!peer || peer == current_cpu) {
        return;
    }

    qemu_cpu_kick(peer);

    if (current_cpu) {
        cpu_exit(current_cpu);
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

    /* Credit slice B a PEND1 snapshot before it yields: the PROM's bit 113. */
    if (bank == 1 && number == 113 && primary && current_cpu == primary) {
        s->pi_int_pend1_primary_credit = true;
    }

    /* The PROM uses pending bits as barrier mailboxes; release the pollers. */
    for (i = 0; i < s->cpus; i++) {
        sgi_hub_yield_to_peer(qemu_get_cpu(s->cpu_base + i));
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
            s->pi_int_pend1_primary_credit = false;
        } else {
            sgi_hub_yield_to_peer(primary);
        }
        return value;
    }

    return sgi_hub_int_pending_read(s, 1);
}

static void sgi_hub_kick_remote_scratch_peer(SGIHubState *s)
{
    unsigned int i;

    if (!current_cpu ||
        (current_cpu->cpu_index >= s->cpu_base &&
         current_cpu->cpu_index < s->cpu_base + s->cpus)) {
        return;
    }

    /* A remote scratch write is a discovery phase change; release the node. */
    for (i = 0; i < s->cpus; i++) {
        sgi_hub_yield_to_peer(qemu_get_cpu(s->cpu_base + i));
    }
}

/*
 * Only an unchanged re-read hands off: a read whose value moved is an answered
 * poll and the reader keeps its slice. Keyed on neither the bank's value nor
 * the masks; both name the wrong reads.
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
            sgi_hub_yield_to_peer(qemu_get_cpu(s->cpu_base + i));
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
        /* Completion arrives as a device interrupt, via interrupt modify. */
        uint64_t level = (s->bte_interrupt[engine] & BTE_INT_LEVEL_MASK) >>
                         BTE_INT_LEVEL_SHIFT;
        uint64_t node = s->bte_interrupt[engine] & BTE_INT_NODE_MASK;
        SGIHubState *target;

        if (node == sgi_hub_programmed_nasid(s)) {
            target = s;
        } else if (s->network_peer &&
                   node == sgi_hub_programmed_nasid(s->network_peer)) {
            target = s->network_peer;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-hub: BTE%u completion interrupt aimed at "
                          "node %" PRIu64 ", which this machine does not "
                          "have\n", engine, node);
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
    return muldiv64(delta, PI_RT_HZ, NANOSECONDS_PER_SECOND) & PI_RT_MASK;
}

static uint64_t sgi_hub_read_rt_compare(SGIHubState *s, hwaddr addr,
                                        unsigned arg)
{
    CPUState *owner = qemu_get_cpu(s->cpu_base + arg);

    /* A progress mailbox: let the owner publish before the poll budget ends. */
    sgi_hub_yield_to_peer(owner);
    return s->pi_rt_compare[arg];
}

/* Move the origin so the counter reads what was written, and keep running. */
static void sgi_hub_rt_set(SGIHubState *s, uint64_t val)
{
    val &= PI_RT_MASK;
    s->pi_rt_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                         muldiv64(val, NANOSECONDS_PER_SECOND, PI_RT_HZ);
}

/*
 * A comparator the counter has already passed matches at once. Whether
 * silicon does or waits a revolution is unobserved; this failure is visible.
 */
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
 * Only a guest-programmed deadline counts as already passed: moving the
 * counter under a reset-zero comparator is a change of origin, not a
 * deadline. The split at half a revolution is the modular space's limit.
 */
static void sgi_hub_rt_arm(SGIHubState *s, unsigned int index, bool programmed)
{
    uint64_t ticks = ((s->pi_rt_compare[index] -
                       sgi_hub_rt_count(s) - 1) &
                      PI_RT_MASK) + 1;

    /* An unpopulated slice has no line wired. */
    if (index >= s->cpus) {
        return;
    }

    if (programmed && ticks > (PI_RT_MASK >> 1)) {
        /* Delete, or the wrap deadline pends a second time. */
        timer_del(s->rt_timer[index]);
        sgi_hub_rt_match(s, index);
        return;
    }

    timer_mod(s->rt_timer[index], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(ticks, NANOSECONDS_PER_SECOND, PI_RT_HZ));
}

/* A slot finished; report completion and what the wire sampled. */
static void sgi_hub_onewire_slot_done(void *opaque)
{
    SGIHubState *s = opaque;

    s->md_onewire |= MD_ONEWIRE_DONE |
                     (s->onewire_level ? MD_ONEWIRE_RXDATA : 0);
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
    if (reg && (reg->flags & HUB_REG_VECTOR) && sgi_hub_reg_peekable(reg)) {
        *value = sgi_hub_reg_peek(target, reg);
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
 * Vector PIO: a null route names the directly wired peer chassis; any other
 * route needs a router and fails with the address-error response.
 */
static void sgi_hub_vector_execute(SGIHubState *s, uint64_t parms)
{
    /* The request carries bits 19:3; a Hub target supplies the NI base. */
    hwaddr addr = parms & NI_VECTOR_ADDRESS_MASK;
    hwaddr target_addr = HUB_NI_STATUS + addr;
    uint64_t type = parms & NI_VECTOR_TYPE_MASK;
    uint64_t response_type = type;
    uint64_t source;
    uint64_t value = 0;
    bool success = false;

    /* network_peer is serialized by BQL MMIO dispatch alone. */
    assert(bql_locked());

    /*
     * No peer: latch nothing and let the poll time out -- the datasheet's
     * proposal for an unobserved case, not a silicon fact.
     */
    if (!s->network_peer) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: vector launch (parms 0x%" PRIx64
                      ") with no fabric to answer\n", parms);
        return;
    }

    if (s->ni_vector == 0) {
        switch (type) {
        case NI_VECTOR_READ:
            success = sgi_hub_vector_target_read(s->network_peer, target_addr,
                                                 &value);
            if (success) {
                s->ni_vector_read_data = value;
            }
            break;
        case NI_VECTOR_WRITE:
            success = sgi_hub_vector_target_write(s->network_peer, target_addr,
                                                  s->ni_vector_data);
            break;
        case NI_VECTOR_EXCHANGE:
            success = sgi_hub_vector_target_read(s->network_peer, target_addr,
                                                 &value);
            if (success) {
                s->ni_vector_read_data = value;
                if (value == 0) {
                    success = sgi_hub_vector_target_write(
                        s->network_peer, target_addr, s->ni_vector_data);
                }
            }
            break;
        default:
            break;
        }
    }

    source = (s->network_peer->ni_status & NI_STATUS_NODE_MASK) >>
        NI_STATUS_NODE_SHIFT;

    if (!success) {
        response_type = NI_VECTOR_ADDRESS_ERROR;
    }
    s->ni_vector_status = NI_VECTOR_VALID |
        (source << NI_VECTOR_TARGET_SHIFT) |
        (parms & (NI_VECTOR_PIOID_MASK | NI_VECTOR_WRITEID_MASK)) |
        (addr & NI_VECTOR_STATUS_ADDR_MASK) | response_type;
    if (trace_event_get_state_backends(TRACE_SGI_HUB_VECTOR_COMPLETE)) {
        CPUState *cpu = current_cpu;

        trace_sgi_hub_vector_complete(
            cpu ? cpu->cpu_index : -1, sgi_hub_programmed_nasid(s),
            (int)sgi_hub_programmed_nasid(s->network_peer),
            cpu ? cpu->cc->get_pc(cpu) : 0, parms, s->ni_vector_data, value,
            s->ni_vector_status, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
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
    sgi_hub_rt_arm(s, 0, false);
    sgi_hub_rt_arm(s, 1, false);
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
    CPUState *peer;

    s->pi_rt_compare[arg] = val & PI_RT_MASK;
    sgi_hub_rt_arm(s, arg, true);

    /*
     * A progress mailbox only while the slice's interrupt is disabled: the
     * PROM arbitrates through it that way, and ending the writer's slice
     * keeps the peer's instruction-count timeout from expiring unrun. A
     * deadline writer has the interrupt enabled and must keep its slice,
     * or its own read-back finds every deadline already passed.
     */
    peer = qemu_get_cpu(s->cpu_base + (arg ^ 1));
    if (!s->pi_rt_enable[arg] && (arg ^ 1) < s->cpus) {
        sgi_hub_yield_to_peer(peer);
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
    return IO_WIDGET_ID_REVISION | IO_WIDGET_ID_PART |
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
    return MD_SLOT_FLASH_READY | MD_SLOT_XJMP_ID |
           (s->slave_module ? 0 : MD_SLOT_MASTER_MODULE);
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
     * Programs the timing and starts a slot; starting clears completion.
     * The bus answers inside the write, but this master reports the answer
     * only once the slot's own deadline arrives -- see the timer below.
     */
    s->md_onewire = val & MD_ONEWIRE_WRITABLE;
    s->onewire_level = sgi_onewire_slot(
        s->onewire_bus, (val & MD_ONEWIRE_PULSE) >> MD_ONEWIRE_PULSE_SHIFT,
        (val & MD_ONEWIRE_SAMPLE) >> MD_ONEWIRE_SAMPLE_SHIFT);
    timer_mod(s->md_onewire_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MD_ONEWIRE_SLOT_NS);
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
    /* The programmed number moved: the claim decode switches with it. */
    sgi_hub_claim_update_pair(s);
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
    /* Stores nothing: no fabric here ever latches a count to clear. */
}

static void sgi_hub_write_ni_port_error(SGIHubState *s, hwaddr addr,
                                        unsigned arg, uint64_t val,
                                        unsigned size)
{
    /*
     * Accepted and logged: no fabric here ever latches an error for a
     * write to clear, and the register reads zero either way.
     */
    qemu_log_mask(LOG_UNIMP,
                  "sgi-hub: write of 0x%" PRIx64 " to the port error "
                  "register 0x%" HWADDR_PRIx
                  " (no fabric here ever latches an error to clear)\n",
                  val, addr);
}

static void sgi_hub_write_ni_scratch(SGIHubState *s, hwaddr addr,
                                     unsigned arg, uint64_t val,
                                     unsigned size)
{
    s->ni_scratch[arg] = val;
    sgi_hub_kick_remote_scratch_peer(s);
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
      .read = sgi_hub_read_processor_number,
      .write = sgi_hub_write_processor_number },
    { .offset = HUB_PI_CALIAS_SIZE,
      .field = offsetof(SGIHubState, pi_calias_size), .width = 4,
      .flags = HUB_REG_DIAG, .store = sgi_hub_store_pi_calias_size },
    { .offset = HUB_PI_CPU_PRESENT_A, .arg = 0,
      .read = sgi_hub_read_cpu_present },
    { .offset = HUB_PI_CPU_PRESENT_B, .arg = 1,
      .read = sgi_hub_read_cpu_present },
    { .offset = HUB_PI_CPU_ENABLE_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_cpu_enable[0]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .write = sgi_hub_write_cpu_enable },
    { .offset = HUB_PI_CPU_ENABLE_B, .arg = 1,
      .field = offsetof(SGIHubState, pi_cpu_enable[1]), .width = 1,
      .flags = HUB_REG_DIAG | HUB_REG_TRACE_WRITE,
      .write = sgi_hub_write_cpu_enable },
    { .offset = HUB_PI_INT_MODIFY,
      .flags = HUB_REG_TRACE_WRITE,
      .read = sgi_hub_read_int_modify, .write = sgi_hub_write_int_modify },
    { .offset = HUB_PI_INT_PEND0, .arg = 0,
      .flags = HUB_REG_TRACE_READ,
      .read = sgi_hub_read_int_pend, .write = sgi_hub_write_read_only },
    { .offset = HUB_PI_INT_PEND1, .arg = 1,
      .flags = HUB_REG_TRACE_READ,
      .read = sgi_hub_read_int_pend, .write = sgi_hub_write_read_only },
    { .offset = HUB_PI_INT_MASK0_A, .arg = 0,
      .field = offsetof(SGIHubState, pi_int_mask[0]), .width = 8,
      .flags = HUB_REG_DIAG, .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_INT_MASK1_A, .arg = 1,
      .field = offsetof(SGIHubState, pi_int_mask[1]), .width = 8,
      .flags = HUB_REG_DIAG, .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_INT_MASK0_B, .arg = 2,
      .field = offsetof(SGIHubState, pi_int_mask[2]), .width = 8,
      .flags = HUB_REG_DIAG, .write = sgi_hub_write_int_mask },
    { .offset = HUB_PI_INT_MASK1_B, .arg = 3,
      .field = offsetof(SGIHubState, pi_int_mask[3]), .width = 8,
      .flags = HUB_REG_DIAG, .write = sgi_hub_write_int_mask },
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
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = MD_CONFIG_MASK },
    { .offset = HUB_MD_MEM_DIMM_INIT, .write = sgi_hub_write_dimm_init },
    { .offset = HUB_MD_DIR_DIMM_INIT, .write = sgi_hub_write_dimm_init },
    { .offset = HUB_MD_ONEWIRE,
      .field = offsetof(SGIHubState, md_onewire), .width = 8,
      .flags = HUB_REG_DIAG, .write = sgi_hub_write_onewire },
    /* Storage that never counts: nothing here tracks an event to count. */
    { .offset = HUB_MD_PERF_CNT0,
      .field = offsetof(SGIHubState, md_perf_cnt0), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = MD_PERF_CNT_MASK },
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
    { .offset = HUB_IO_SCRATCH_0, .arg = 0,
      .field = offsetof(SGIHubState, io_scratch[0]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = IO_SCRATCH_MASK },
    { .offset = HUB_IO_SCRATCH_1, .arg = 1,
      .field = offsetof(SGIHubState, io_scratch[1]), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG, .wmask = IO_SCRATCH_MASK },
    { .offset = HUB_IO_LLP_LOG,
      .read = sgi_hub_read_llp_log, .write = sgi_hub_write_llp_log },
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
     * vector-writing the peer's protection register before its link
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
     * the peer's error register, and an address-error completion there
     * counts as a phantom link error on every two-chassis boot. The
     * error alias carries no HUB_REG_READ_CLEARS: on an always-zero
     * register the clear is a no-op, so the read has no side effect to
     * guard. The error register's store keeps a vector write clean --
     * the register is writable, with nothing latched for the write to
     * clear -- while the alias, which no source documents as writable,
     * stores nothing and so rejects one.
     */
    { .offset = HUB_NI_PORT_PARMS,
      .field = offsetof(SGIHubState, ni_port_parms), .width = 8,
      .flags = HUB_REG_STORE | HUB_REG_DIAG | HUB_REG_VECTOR,
      .wmask = NI_PORT_PARMS_MASK },
    { .offset = HUB_NI_PORT_ERROR,
      .flags = HUB_REG_DIAG | HUB_REG_VECTOR,
      .peek = sgi_hub_peek_zero,
      .store = sgi_hub_store_ni_port_error,
      .write = sgi_hub_write_ni_port_error },
    { .offset = HUB_NI_PORT_ERROR_CLEAR,
      .flags = HUB_REG_DIAG | HUB_REG_VECTOR,
      .peek = sgi_hub_peek_zero },
};

static const SGIHubRegDef *sgi_hub_reg_lookup(hwaddr addr)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(sgi_hub_registers); i++) {
        if (sgi_hub_registers[i].offset == addr) {
            return &sgi_hub_registers[i];
        }
    }
    return NULL;
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
 * zero for a register that stores nothing (interrupt modify included). The
 * merged value takes the ordinary write path, the guest's size riding along.
 */
static void sgi_hub_write_half(SGIHubState *s, hwaddr addr, uint64_t val,
                               unsigned size)
{
    hwaddr aligned = addr & ~(hwaddr)7;
    uint64_t merged = 0;

    if (aligned == HUB_PI_INT_MODIFY ||
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
        return;
    }
    sgi_hub_write_internal(s, addr, val, size);
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

    s->pi_region_present = ~0ULL;
    s->pi_calias_size = PI_CALIAS_SIZE_RESET;
    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        s->pi_cpu_enable[n] = n < s->cpus;
    }
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
                   (s->network_link_up ? NI_STATUS_LINK_UP : 0) |
                   ((uint64_t)s->nasid << NI_STATUS_NODE_SHIFT);
    s->ni_scratch[0] = 0;
    s->ni_scratch[1] = 0;
    s->ni_vector = 0;
    s->ni_vector_data = 0;
    s->ni_vector_status = 0;
    s->ni_return_vector = 0;
    s->ni_vector_read_data = 0;
    s->ni_port_parms = NI_PORT_PARMS_RESET;
    s->ni_io_protect = NI_IO_PROTECT_RESET;
    s->ni_protection = NI_PROTECTION_RESET;
    s->ni_diag_parms = 0;
    memset(s->ni_meta_route, 0, sizeof(s->ni_meta_route));
    memset(s->ni_local_route, 0, sizeof(s->ni_local_route));

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
    /* A slot armed before reset must not complete after it. */
    timer_del(s->md_onewire_timer);
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
    sgi_hub_update_irq(SGI_HUB(obj));
}

/*
 * The claim overlays re-derive once every Hub's programmed number is
 * back at its seeded value -- the exit phase, because a Hub's claims
 * depend on the peer's number too, and every enter phase has run by now.
 * The nameless low windows re-derive with them, back to the reset size.
 */
static void sgi_hub_reset_exit(Object *obj, ResetType type)
{
    SGIHubState *s = SGI_HUB(obj);

    sgi_hub_claim_update_pair(s);
    sgi_hub_calias_update(s);
}

static void sgi_hub_realize(DeviceState *dev, Error **errp)
{
    SGIHubState *s = SGI_HUB(dev);
    unsigned int n;

    if (!s->xtalk) {
        error_setg(errp, "the Hub needs the crosstalk space its big windows "
                         "translate into");
        return;
    }
    if (s->onewire_serial & ~MD_ONEWIRE_SERIAL_MASK) {
        error_setg(errp, "the Hub one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }
    if (s->ni_revision > NI_STATUS_REVISION_MAX) {
        error_setg(errp, "the Hub NI revision is a four-bit field, so at "
                         "most %u", NI_STATUS_REVISION_MAX);
        return;
    }
    if (!s->xtalk_widget_shift) {
        error_setg(errp, "the Hub needs the widget stride of the crosstalk "
                         "space its big windows translate into");
        return;
    }
    if (!s->memory) {
        error_setg(errp, "the Hub needs the node-local memory view its "
                         "block transfer engines copy through");
        return;
    }
    if (!s->claim_target || !s->claim_node_shift || !s->claim_space_shift) {
        error_setg(errp, "the Hub needs the canonical map and its node and "
                         "space geometry to claim node-qualified strides");
        return;
    }
    for (n = 0; n < s->cpus; n++) {
        if (!s->claim_view[n]) {
            error_setg(errp, "the Hub needs a claim container for each of "
                             "its %u processors' views", s->cpus);
            return;
        }
    }

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_hub_ops, s,
                          "sgi-hub.regs", SGI_HUB_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    address_space_init(&s->bte_as, s->memory, "sgi-hub.bte");

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

    s->i2c.diagnostic = s->diagnostic;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c), errp)) {
        return;
    }
    /* Above sgi_hub_ops, so the chip's sixteen bytes reach the controller. */
    memory_region_add_subregion_overlap(
        &s->regs, HUB_MD_I2C_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->i2c), 0), 1);

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
    {
        I2CSlave *elsc = i2c_slave_new(TYPE_SGI_ELSC, SGI_ELSC_ADDR);

        object_property_add_child(OBJECT(s), "elsc", OBJECT(elsc));
        qdev_prop_set_uint32(DEVICE(elsc), "num-cpus", s->cpus);
        qdev_prop_set_uint32(DEVICE(elsc), "cpu-base", s->cpu_base);
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
    {
        static const uint8_t reserved_page_defaults[16] = { 0x37 };

        for (n = 0; n < ELSC_NVRAM_PAGE_COUNT; n++) {
            uint8_t address = ELSC_NVRAM_FIRST_ADDR + n;

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

    s->md_onewire_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       sgi_hub_onewire_slot_done, s);

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
    {
        DeviceState *part = qdev_new(TYPE_ONEWIRE_DS2505);

        object_property_add_child(OBJECT(dev), "onewire-part", OBJECT(part));
        qdev_prop_set_uint8(part, "family", MD_ONEWIRE_ROM_FAMILY);
        qdev_prop_set_uint64(part, "serial", s->onewire_serial);
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
        g_free(name);
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->bwin);

    /*
     * The claim overlays, disabled until the first reset aims them (the
     * PAM idiom: pre-created alternatives, switched, never recreated).
     * The error floor persists beneath each alias so an unclaimed stride
     * fails rather than falling through to the static placement.
     */
    memory_region_transaction_begin();
    for (n = 0; n < s->cpus; n++) {
        unsigned int space, stride_index;
        unsigned int strides = s->network_peer ? SGI_HUB_CLAIM_STRIDES : 1;

        for (space = 0; space < SGI_HUB_CLAIM_SPACES; space++) {
            for (stride_index = 0; stride_index < strides; stride_index++) {
                uint32_t stride = stride_index ? s->claim_peer_node
                                               : s->claim_own_node;
                hwaddr base = ((hwaddr)space << s->claim_space_shift) |
                              ((hwaddr)stride << s->claim_node_shift);
                uint64_t size = 1ULL << s->claim_node_shift;
                char *name;

                name = g_strdup_printf(
                    "sgi-hub.claim.cpu%u.space%u.node%u.error",
                    n, space, stride);
                memory_region_init_io(
                    &s->claim_error[n][space][stride_index], OBJECT(dev),
                    &sgi_hub_claim_error_ops, s, name, size);
                memory_region_add_subregion_overlap(
                    s->claim_view[n], base,
                    &s->claim_error[n][space][stride_index], 0);
                g_free(name);

                name = g_strdup_printf("sgi-hub.claim.cpu%u.space%u.node%u",
                                       n, space, stride);
                memory_region_init_alias(
                    &s->claim_alias[n][space][stride_index], OBJECT(dev),
                    name, s->claim_target, base, size);
                memory_region_set_enabled(
                    &s->claim_alias[n][space][stride_index], false);
                memory_region_add_subregion_overlap(
                    s->claim_view[n], base,
                    &s->claim_alias[n][space][stride_index], 1);
                g_free(name);
            }
        }
    }
    memory_region_transaction_commit();
}

static int sgi_hub_post_load(void *opaque, int version_id);

static const VMStateDescription vmstate_sgi_hub = {
    .name = "sgi-hub",
    .version_id = 23,
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
        VMSTATE_UINT64_ARRAY_V(ni_scratch, SGIHubState, 2, 14),
        VMSTATE_UINT64_V(ni_vector, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_vector_data, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_vector_status, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_return_vector, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_vector_read_data, SGIHubState, 15),
        VMSTATE_UINT64_V(ni_port_parms, SGIHubState, 20),
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
        VMSTATE_TIMER_PTR_V(md_onewire_timer, SGIHubState, 12),
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

    if (version_id < 17) {
        s->pi_int_pend1_primary_credit = false;
    }

    if (version_id < 16) {
        memset(s->ni_meta_route, 0, sizeof(s->ni_meta_route));
        memset(s->ni_local_route, 0, sizeof(s->ni_local_route));
    }

    for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
        sgi_hub_bwin_follow(s, n);
    }
    /*
     * The claim overlays re-derive from the just-restored programmed
     * number. A Hub's claims also depend on the peer's number, which may
     * not have loaded yet -- the pair update in whichever Hub's section
     * loads second leaves both aimed by restored state.
     */
    sgi_hub_claim_update_pair(s);
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
    DEFINE_PROP_BOOL("slave-module", SGIHubState, slave_module, false),
    DEFINE_PROP_BOOL("network-link-up", SGIHubState, network_link_up, false),
    DEFINE_PROP_LINK("network-peer", SGIHubState, network_peer, TYPE_SGI_HUB,
                     SGIHubState *),
    DEFINE_PROP_UINT32("cpus", SGIHubState, cpus, 1),
    DEFINE_PROP_UINT8("ni-revision", SGIHubState, ni_revision, 3),
    DEFINE_PROP_UINT8("elsc-dip-switches", SGIHubState,
                      elsc_dip_switches, 0),
    DEFINE_PROP_UINT32("cpu-base", SGIHubState, cpu_base, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIHubState, onewire_serial,
                       0x0c0b0a090807ULL),
    DEFINE_PROP_UINT32("manufacturer", SGIHubState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("xtalk-widget-shift", SGIHubState,
                       xtalk_widget_shift, 0),
    DEFINE_PROP_LINK("xtalk", SGIHubState, xtalk, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("memory", SGIHubState, memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("claim-view0", SGIHubState, claim_view[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("claim-view1", SGIHubState, claim_view[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("claim-target", SGIHubState, claim_target,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("calias-window0", SGIHubState, calias_window[0],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("calias-window1", SGIHubState, calias_window[1],
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("claim-node-shift", SGIHubState, claim_node_shift, 0),
    DEFINE_PROP_UINT32("claim-space-shift", SGIHubState, claim_space_shift, 0),
    DEFINE_PROP_UINT32("claim-own-node", SGIHubState, claim_own_node, 0),
    DEFINE_PROP_UINT32("claim-peer-node", SGIHubState, claim_peer_node, 0),
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
}

static void sgi_hub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI SN0 Hub ASIC";
    dc->realize = sgi_hub_realize;
    /* The machine supplies the memory, crosstalk and peer links. */
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
