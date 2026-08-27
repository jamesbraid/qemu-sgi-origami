/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC — the registers a guest reads before it has a console.
 *
 * The Hub is four units behind one 8 MB register window: a processor interface,
 * a memory and directory controller, an I/O interface and a network interface.
 * This model implements the registers an operating system touches on its way to
 * switching its console away from the firmware, which is a small and specific
 * set, and reports anything else rather than answering it. The rest arrives
 * with the interrupt path and the crossbar — and, where real firmware turns
 * out to be a guest in its own right rather than only the thing an OS boots
 * past, with whatever it touches that no OS does.
 *
 * Two of these registers decide things a guest can never recover from getting
 * wrong. The processor-number register tells it which of a node's two
 * processors it is, and so which copy of every per-processor register to use.
 * The network-interface status register tells it whether the machine numbers
 * nodes in M mode or N mode, and every node-relative address it forms
 * afterwards depends on the answer. Both must read zero on an unprogrammed
 * Origin 200, which is what their reset values say.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-elsc.h"
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

/*
 * I/O interface: the Hub's own face as a widget on the crossbar. Every device
 * on the crossbar begins its address space with the same identification, status
 * and control registers, and this is where the Hub presents that block —
 * anything walking the fabric reads the identification first and learns it is
 * talking to a Hub.
 */
#define HUB_IO_WIDGET_ID        0x400000
#define HUB_IO_WIDGET_STATUS    0x400008
#define HUB_IO_WIDGET_CONTROL   0x400020

/*
 * The identification register's layout is the common crosstalk one, shared with
 * every other widget: revision, part number, manufacturer, and a lowest bit
 * that
 * always reads one. That last bit is declared by no Hub source — the register
 * table's field list stops above it and the header calls it reserved — and is
 * set here because this is the JTAG device identification format the crossbar
 * contract uses, which the Bridge and the crossbar both state. Following the
 * shared contract is better than inventing a Hub-specific answer, and it is
 * recorded as inherited rather than sourced.
 */
#define IO_WIDGET_ID_REVISION   (1ULL << 28)
#define IO_WIDGET_ID_PART       (0xc101ULL << 12)
#define IO_WIDGET_ID_ONE        1ULL
#define IO_WIDGET_ID_MFG_SHIFT  1

/*
 * The control register. Two of its reset values are load-bearing rather than
 * decorative: this Hub's own widget number resets to 1, which is the
 * register-level counterpart of the address map putting the Hub at widget 1, so
 * a model resetting it to zero contradicts its own decode; and the crossbar
 * credit resets to 2.
 *
 * The tag-mode bit is what the hardware reports rather than what software
 * chooses, so it is not writable. The widget number is, which is storage here:
 * nothing re-decodes if a guest changes it, and no guest does.
 */
#define IO_WIDGET_CONTROL_NUMBER    1ULL
#define IO_WIDGET_CONTROL_CREDIT    (2ULL << 13)
#define IO_WIDGET_CONTROL_RESET     (IO_WIDGET_CONTROL_NUMBER | \
                                     IO_WIDGET_CONTROL_CREDIT)
#define IO_WIDGET_CONTROL_TAG_MODE  (1ULL << 4)
#define IO_WIDGET_CONTROL_WRITABLE  (0x7fffffULL & ~IO_WIDGET_CONTROL_TAG_MODE)

/*
 * The crosstalk timeouts, which are the first two registers mainline Linux
 * writes when it initialises a Hub — the prescale and then the timeout, before
 * it enables anything. Both must accept a write at reset with no unlock step.
 */
#define HUB_IO_XTALK_TIMEOUT    0x4003b8
#define HUB_IO_XTALK_PRESCALE   0x4003c0

/*
 * The I/O interface's two general-purpose scratch registers — named that in
 * every source that names them at all, with no other behaviour attached. Real
 * firmware's own boot measurably reads register 0 234 times against 6 writes,
 * far more than any register with a hardware side effect this Hub carries, and
 * SGI's own machine-readable Hub register table (hwreg_hub.hwreg) and current
 * mainline Linux (hubio.h) agree byte-for-byte on the field layout: four
 * sub-fields packed into one 36-bit word, with everything above bit 35 read as
 * zero. Real firmware uses this as its own private state, most likely a
 * boot-sequencing flag this model has no other way to see — the same shape as
 * the console-ready bits found elsewhere in this Hub, a software convention
 * riding on hardware storage rather than a hardware behaviour of its own. The
 * masked bits are the only thing establishable from any source; what the
 * firmware stores there is not.
 */
#define HUB_IO_SCRATCH_0        0x400150
#define HUB_IO_SCRATCH_1        0x400158
#define IO_SCRATCH_MASK         0x0000000f00f11fffULL

/*
 * The Incoming Crosstalk Request Buffer array: fifteen slots (0 through 0xE),
 * four sub-registers each, tracking one outstanding incoming crosstalk
 * transaction per slot while it is in flight. SGI's own register table admits
 * it never finished documenting any of the sixty — every entry across all
 * fifteen slots and all four sub-registers says only "the definition for this
 * register is not available." Mainline Linux's hubio.h fills the gap with the
 * real field layout: sub-register C alone carries a read count, a priority
 * count and prescalar, a BTE operation flag, a combined push-address/byte-
 * enable field, a supplemental field, and barrier/response/GBR bits — entirely
 * per-transaction metadata, nothing that reports the slot's own state (that is
 * sub-register A's "valid" bit, per the same header).
 *
 * Real firmware's own boot measurably reads only slots 7 through 14's C
 * sub-register, four times each — a diagnostic sweep of the array's upper
 * half, not a wait on any one of them. Implemented only that far, matching
 * what is measured rather than the whole sixty-register array: this model has
 * never generated a real incoming crosstalk transaction for any slot to track,
 * so every field a real slot's C register would hold is honestly zero, the
 * same reasoning this Hub already applies to the widget status register's
 * error and retry counts. Not stored — there is nothing to keep and nothing
 * a guest ever writes here on real hardware.
 */
#define HUB_IO_ICRB_C_BASE      0x4004f0  /* slot 7's C register */
#define HUB_IO_ICRB_C_STRIDE    0x20      /* one slot to the next */
#define HUB_IO_ICRB_C_FIRST     7
#define HUB_IO_ICRB_C_LAST      14

/*
 * The two block transfer engines: cache-coherent memory-to-memory copy
 * engines in the Hub's I/O section, publicly named BTE0 and BTE1
 * (docs/datasheets/hub-bte.md). Software gives an engine a source, a
 * destination and a length in 128-byte cache lines; the engine copies or
 * zero-fills that range and signals completion either as a pending
 * interrupt bit on a processor interface or as a status doubleword written
 * to memory. The one guest consumer on our boot path is real firmware's
 * power-on diagnostic, which launches both engines in interrupt mode and
 * waits thirty seconds for completion interrupts no engine existed to
 * deliver.
 *
 * Three facts here are load-bearing:
 *
 * **The engines work from reset, gated on nothing.** The diagnostic never
 * writes the miscellaneous enable mask before launching, so a model gating
 * launches on an enable bit that resets to zero reproduces exactly the
 * hang this device exists to remove. Whatever those enable bits gate — the
 * datasheet leaves their sense open — nothing the machine boots writes them
 * before first use, so launches are not gated on them and the register
 * stays unimplemented.
 *
 * **Stored addresses keep bits 39:7 and nothing else.** Real firmware
 * stores the cached-memory window's virtual addresses — values with bits
 * above 39 set — and depends on the register dropping them; a model that
 * kept them would aim every transfer at nothing. Bits 6:0 sit below the
 * cache-line size and are not stored either.
 *
 * **A transfer completes inside the store that launches it.** The
 * diagnostic clears its two pending bits only when it observes both set
 * together and does not loop back for the second — a window instant
 * completion avoids entirely, which the datasheet documents as the correct
 * model here. It also means a load of the control/terminate register,
 * which stops a transfer in progress on real hardware, never has one to
 * stop.
 *
 * **The engines see memory as their node's processors do.** The engines
 * sit in the Hub's own register window and move node memory — nothing here
 * crosses the Bridge or any crosstalk widget, so no IOMMU is involved — but
 * the machine gives every processor a view in which an address carrying no
 * node number names its own node, and the diagnostic fills, launches and
 * compares its buffers through that view with node-less addresses. An
 * engine copying through the global map instead would, on the second node,
 * move the first node's memory while the second node's processors compared
 * their own; so the machine hands the Hub the same node-local view and the
 * engines copy through that.
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

/*
 * Memory and directory interface. Both guests we boot read memory size
 * through the synthesized configuration graph rather than this unit's
 * registers directly, which is why only these two exist here: real firmware's
 * own diagnostics are the first guest that writes them.
 */
#define HUB_MD_INDICATOR_0      0x220050  /* real firmware's diagnostic LED, CPU A */
#define HUB_MD_INDICATOR_1      0x220058  /* same, CPU B */
#define HUB_MD_SN00_INDICATOR_0 0x220080  /* SN00 diagnostic indicator, CPU A */
#define HUB_MD_SN00_INDICATOR_1 0x2200a0  /* same, CPU B */

/*
 * Slot identity and microcontroller status: read-only signals sampled off the
 * board, and the register real firmware polls before it touches the boot flash.
 *
 * The boot flash's ready signal appears at two bit positions, and the register
 * table this machine's variant comes from states that the two are the same
 * signal rather than two separate ones. Both are reported ready here, because
 * firmware waiting on either and never seeing it waits forever — the failure is
 * a boot that stops with nothing logged, which is the same shape as a crosstalk
 * link that never comes up.
 *
 * The machine supplies the master/slave sense sampled by each Origin 200
 * processor module and the XIO jumper fitted to every plain Origin 200 module.
 * The remaining fields sample signals this model does not have: a bus-fault
 * line and a processor-speed strap. They read clear. The register table
 * declares no reset value for any field in either variant, which is what a
 * read-only status register sampling external signals looks like, so what the
 * absent signals read is this machine's decision rather than a transcribed one.
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
 * The first of the memory/directory unit's six hardware performance counters
 * (IRIX's own hubmd.h and mainline Linux's agree: a 20-bit event counter,
 * selected by a sibling register this model does not implement). Real
 * firmware's own boot measurably reads this one specifically 144 times
 * against 23 writes — repeated resets and re-samples, the shape of a
 * benchmark or a timed diagnostic loop, not a single setup-then-poll.
 *
 * This model counts no real hardware event: nothing here tracks memory or
 * directory bus activity for a counter to report, so inventing an increment
 * would be fabricating a measurement rather than modelling one. Answered as
 * plain storage instead — a write sets the count a real diagnostic would read
 * back, and the register never advances on its own. Honest for a boot-time
 * self-test that resets and re-reads to check the register behaves, and
 * unable to satisfy anything that depends on the count actually changing
 * between two reads with no write between them; nothing on our boot path is
 * known to do that.
 */
/*
 * The memory configuration register: eight per-bank descriptors, a
 * reply-guarantee count, the directory-format and error-correction-ignore
 * bits, an arbitration bit, an I/O-protection enable, a memory-module select
 * and two timing-field pairs (docs/datasheets/hub-memory-directory.md has
 * the full field-by-field account, dual-sourced against SGI's own composite
 * reset-defaults constant). Modelled as plain masked storage -- nothing on
 * our boot path is known to depend on a write here changing what memory
 * this model presents, so there is no derived behaviour to add, only the
 * value a guest reads back. The reset value is the one fact that matters:
 * it is not zero, and a model that zeroed it would misreport eight empty
 * banks, an enabled error-correction bit the hardware leaves ignored, a
 * cleared directory-format bit that resets set, and a reply-guarantee count
 * dropped from full to zero, all at once.
 */
#define HUB_MD_CONFIG           0x200018

/*
 * The node's one-wire bus master. Firmware programs the timing of one bit
 * slot, starts it with a write, and polls for the completion bit; the level
 * sampled from the wire arrives beside it.
 *
 * **The register is 34 bits wide and that is a correctness requirement, not
 * trivia.** The upper timing phase occupies bits 33:27, so a value the
 * firmware writes genuinely sets bit 32. Truncating to 32 bits does not drop
 * a spare bit — it rewrites that phase field from 44 to 12, a timing
 * parameter wrong by nearly four, with every field below it intact and
 * looking perfectly reasonable. No fault, nothing to notice.
 *
 * **There is no bus behind this yet, and answering as though there were is
 * worse than the stall.** A model that reports completion without a bus hands
 * firmware a fabricated identity part: every bit it clocks out answered by
 * whatever constant we chose, assembled into a serial number that was never
 * on any wire. The stall announces itself; the fabrication does not. So this
 * accepts the timing parameters and starts nothing.
 *
 * **Starting a transaction clears the completion bit**, which is what makes
 * the reset value not matter for correctness. The two documents that describe
 * this register disagree about whether completion resets set or clear, both
 * unsourced — and either way the firmware's own request clears it and the
 * poll spins, so the failure stays loud while the bus does not exist.
 */
#define HUB_MD_ONEWIRE          0x2000a8
#define MD_ONEWIRE_RXDATA       (1ULL << 0)
#define MD_ONEWIRE_DONE         (1ULL << 1)
#define MD_ONEWIRE_SAMPLE       (0xffULL << 2)
#define MD_ONEWIRE_PULSE        (0x3ffULL << 10)
#define MD_ONEWIRE_PHASE_LO     (0x7fULL << 20)
#define MD_ONEWIRE_PHASE_HI     (0x7fULL << 27)

/*
 * Everything except the two status bits is the guest's to set; the completion
 * and received-data bits are the device's to report, so a write never sets
 * them.
 */
#define MD_ONEWIRE_WRITABLE     (MD_ONEWIRE_SAMPLE | MD_ONEWIRE_PULSE | \
                                 MD_ONEWIRE_PHASE_LO | MD_ONEWIRE_PHASE_HI)

/*
 * Both timing phases at the value our memory/directory datasheet records for
 * them. **The one-wire datasheet is the authority on this register's fields
 * and does not carry this constant**, while instructing a test to assert the
 * actual value rather than merely a non-zero one — so this is taken from the
 * other page and the discrepancy is reported rather than papered over. A
 * wrong constant here is a wrong value in a register no booting guest reads,
 * and it does not affect the poll either way.
 */
/*
 * How long a bit slot takes before completion is reported. **Chosen, not
 * derived.** The one-wire datasheet states plainly that what the timing values
 * mean in real time, as opposed to as encodings, is not established — so
 * computing a duration from the programmed pulse width and sample point would
 * manufacture a number that looks derived and is not. What the contract
 * actually requires is only that a slot does not complete inside the write, so
 * this is a nominal delay long enough to be a real interval and short enough
 * that a polling firmware is not slowed by it.
 */
#define MD_ONEWIRE_SLOT_NS      1000

#define MD_ONEWIRE_PHASE_INIT   0x31ULL
#define MD_ONEWIRE_RESET        ((MD_ONEWIRE_PHASE_INIT << 20) | \
                                 (MD_ONEWIRE_PHASE_INIT << 27))
#define MD_CONFIG_MASK          0x003fffffffffffffULL  /* 54-bit register */
#define MD_CONFIG_RESET         0x001e7fbc3fffffffULL

/*
 * The four error registers -- directory, protocol, memory, miscellaneous --
 * and their four read-only clear aliases, eight bytes above each, evenly
 * strided across 0x200050 through 0x200088. None declares a reset value in
 * SGI's own table, which this datasheet's own convention reads as a
 * read-only status register sampling external signals -- here, an error
 * this model never raises. Real firmware's mtest_basic reads every alias
 * before running a memory or directory test and aborts with "Unable to
 * clear memory errors" if any valid bit is set, so answering zero
 * unconditionally is not a placeholder, it is what the one guest that reads
 * these needs. Writes (the aliases' own documented use is a read that is
 * inferred, not proven, to clear) are accepted and logged as unimplemented.
 */
#define HUB_MD_ERROR_BASE       0x200050
#define HUB_MD_ERROR_STRIDE     8
#define HUB_MD_ERROR_LAST_OFF   (HUB_MD_ERROR_BASE + 7 * HUB_MD_ERROR_STRIDE) /* MD_MISC_ERROR_CLR */

/*
 * The two per-DIMM mode-initialisation registers. Both dual-sourced
 * (mainline Linux's hubmd.h, SGI's own table) as write-only: a four-bit
 * DIMM_SELECT field at bits 35:32 and a twelve-bit DIMM_MODE field at bits
 * 11:0, no readable bits declared at all. Real firmware writes both
 * sixteen times each, DIMM_SELECT stepping 0x0 through 0xf while DIMM_MODE
 * holds a constant value -- one iteration per DIMM slot. Accepted and
 * unstored: real hardware's DIMM mode setting has no functional signature
 * on a machine whose transfers complete instantly, and nothing declares
 * either field readable for a model to answer regardless.
 */
#define HUB_MD_MEM_DIMM_INIT    0x200090
#define HUB_MD_DIR_DIMM_INIT    0x200098

#define HUB_MD_PERF_CNT0        0x210010
#define MD_PERF_CNT_MASK        0x00000000000fffffULL  /* 20-bit counter */

/*
 * The PCF8584 I2C-bus controller chip real firmware drives heavily during
 * bring-up (docs/datasheets/hub-memory-directory.md has the register-level
 * detail; docs/datasheets/pcf8584-elsc.md has the chip's own behavioral
 * contract. It is a third-party chip with a public interface, not an SGI
 * one). This window used to answer as a fixed, always-idle controller —
 * enough to stop firmware from spinning through its full I2C timeout, but
 * not a real controller. It is now a real PCF8584 model (hw/i2c/pcf8584.c),
 * mapped over this window at higher priority than sgi_hub_ops answers below,
 * so every access here reaches the chip instead of this switch — see
 * sgi_hub_realize(). The two addresses stay defined here only because the
 * chip's own size is derived from where the Hub places it.
 */
#define HUB_MD_I2C_BASE         0x220000  /* PCF8584, S0 then S1 eight on */

#define ELSC_NVRAM_FIRST_ADDR   0x50
#define ELSC_NVRAM_PAGE_COUNT   8
#define ELSC_NVRAM_PAGE_SIZE    256
#define ELSC_NVRAM_RESERVED_PAGE 7

/*
 * The junk-bus UART: a real NS16550-compatible serial interface at the upper
 * half of this same eight-register block, one register per eight-byte stride
 * rather than the chip's own one-byte spacing -- the spacing belongs to the
 * junk bus, not to the NS16550. A shipped Origin 200 has no such part; when
 * origami is asked to fit one, sgi_hub_realize() places it here as a
 * subregion, and its own register handler answers this range instead of the
 * unimplemented default below. Left unfitted, nothing claims this range and
 * every one of these eight offsets reads and writes exactly like any other
 * undefined register in this group.
 */
#define HUB_MD_JUNKUART_BASE    0x2200c0
#define HUB_MD_JUNKUART_REGSHIFT 3        /* eight-byte stride, junk-bus rule */
#define HUB_MD_JUNKUART_OSC_HZ  12000000  /* the port's reference crystal */
/* What QEMU's 16550 wants for its clock argument: the oscillator already
 * divided by sixteen, not the raw crystal rate. Passing the crystal rate
 * here breaks no boot test -- it only changes the line rate the model
 * reports to whatever sits behind the character device, which is invisible
 * against a pipe and garbage against a real serial port. */
#define HUB_MD_JUNKUART_CLOCK   (HUB_MD_JUNKUART_OSC_HZ / 16)

/*
 * Whether each processor socket is populated, and whether that processor is
 * allowed to run. Present is hardware-sensed and read-only: no firmware we
 * hold ever writes it, so the model derives it from the node's configured
 * processor count rather than from anything a guest does. Enable resets clear
 * and real firmware sets it during bring-up, so a guest that reads present
 * correctly still sees a disabled processor until the firmware acts.
 */
#define HUB_PI_CPU_PRESENT_A    0x000040
#define HUB_PI_CPU_PRESENT_B    0x000048
/*
 * Where each processor spools error records. Bits 31:3, so the low three bits
 * of a written value do not survive.
 *
 * **Processor B's is the register the firmware uses to survive its own reset.**
 * It writes a marker there, triggers the port reset, and the restarted machine
 * reads it back to learn it is on the second pass. On a single-processor
 * machine that register belongs to a processor which is not fitted, which is
 * presumably the point.
 */
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

#define HUB_PI_CPU_ENABLE_A     0x000050
#define HUB_PI_CPU_ENABLE_B     0x000058

/*
 * The real-time counter: a free-running 52-bit upcounter at 1.25 MHz, and the
 * one part of this unit both guests need. OpenBSD's whole requirement is that
 * it be readable and advance monotonically at that rate — it drives its clock
 * from the processor's own timer and uses this only as a reference. Linux
 * reads it as a clock source and arms a comparator against it, so for Linux it
 * must also tick *between* two reads a few instructions apart: it reads the
 * counter, adds a delta, writes the comparator, and re-reads to check the
 * deadline has not already passed. A counter that does not move between those
 * two reads reports a deadline safely in the future that never arrives.
 *
 * Derived from the virtual clock rather than stored, so it cannot drift from
 * the timer a comparator will eventually be armed against. A write moves the
 * origin instead of the count, which is what makes it writable without it
 * ever running backwards on its own — Linux zeroes it once at boot.
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

/*
 * The counter's own clock control (docs/datasheets/hub-processor-interface.md
 * has the full field account). Bit 16 selects the Hub's internal clock over
 * the network-wide one and resets set; bits 7:1 hold a maximum count and bit
 * 0 enables the network clock, both writable; bit 15 and bits 14:8 observe
 * the network-wide clock and are read-only. This model has no network-wide
 * clock to observe -- a single-node machine has nothing crosstalk-connected
 * driving one -- so those bits are not part of the writable mask and always
 * read zero, the same reasoning already applied to every other status field
 * nothing on this model ever sets. Plain masked storage for the rest:
 * nothing on our boot path is known to depend on the counter's rate tracking
 * whatever this register is programmed to, and the counter's 1.25 MHz rate
 * is independently corroborated three ways already (the shipped firmware's
 * own configuration block, OpenBSD's registered frequency, Linux's hard-
 * coded tick), all agreeing before this register was ever read back.
 */
#define HUB_PI_RT_LOCAL_CTRL    0x000160
#define PI_RT_LOCAL_CTRL_MASK   0x100ffULL
#define PI_RT_LOCAL_CTRL_RESET  0x10000ULL

/*
 * I/O interface: the link to the crossbar, and what this Hub says about it.
 */
#define HUB_IO_LINK_STATUS      0x400128
#define HUB_IO_ERROR_CLEAR      0x4001f8

/*
 * The link state is two bits with a counter-intuitive encoding: zero means the
 * link failed, one means it never came up, and *both* two and three mean it is
 * working. The convenient consequence, which a guest relies on and so does
 * this model, is that the two working values are the two with the field's high
 * bit set — so testing that one bit answers "is this link up?", which is
 * exactly the test mainline Linux performs.
 *
 * Between two and three nothing we have distinguishes them and nothing reads
 * the low bit, so two is a choice rather than a finding.
 */
#define IO_LINK_STATE_SHIFT     12
#define IO_LINK_STATE_UP        (2ULL << IO_LINK_STATE_SHIFT)
#define IO_LINK_STATE_MASK      (3ULL << IO_LINK_STATE_SHIFT)

#define IO_LINK_MAX_BURST       (0x3fcULL << 32)
#define IO_LINK_MAX_RETRY       (0x3ffULL << 16)
#define IO_LINK_NULL_TIMEOUT    0xaULL

/*
 * Real firmware operates this bit itself — hubii_llp_enable() does an
 * ordinary load-modify-store through this exact register once it has
 * identified a real part on the link — so it is a control the guest drives,
 * not a status the hardware alone reports. Left unset at reset: presetting it
 * would model the firmware's own write for it, silently discarding whatever
 * that write was actually gated on.
 */
#define IO_LINK_ENABLED         (1ULL << 10)

#define IO_LINK_RESET           (IO_LINK_MAX_BURST | IO_LINK_MAX_RETRY | \
                                 IO_LINK_STATE_UP | IO_LINK_NULL_TIMEOUT)

/*
 * What a guest may change. The burst, the retry count and the null-packet
 * timeout are configuration; the warm-reset trigger is a one-shot the model
 * accepts and does nothing with; the link-enabled bit is a control real
 * firmware operates, per the comment above. The link state and the eight-bit
 * mode stay read-only: firmware may enable the link, but nothing may claim
 * the link came up — a guest that could write the state field could tell
 * this machine its own link was down.
 */
#define IO_LINK_WARM_RESET      (1ULL << 8)
#define IO_LINK_WRITABLE        (IO_LINK_MAX_BURST | IO_LINK_MAX_RETRY | \
                                 IO_LINK_WARM_RESET | IO_LINK_ENABLED | \
                                 0x3fULL)

/*
 * I/O interface: the big-window translation table, entry n at 8n from its base.
 * One entry aims one 512 MB window of the node's crosstalk window at a slice of
 * one widget's crosstalk space.
 */
#define HUB_IO_BWIN_TABLE       0x400160
#define HUB_IO_BWIN_STRIDE      8

/*
 * An entry is thirteen bits with no valid bit anywhere in it. A window is
 * turned off by naming a widget that is not there, which is what both guests do
 * rather than clearing an enable, so every widget number has to be accepted and
 * a window aimed at an empty port simply fails to reach anything.
 *
 * Bits 7:5 are inside the register's declared width and are named by nothing,
 * so they are stored rather than dropped: no source says they read back zero.
 */
#define IO_BWIN_MASK            0x1fff
#define IO_BWIN_SPACE           (1U << 12) /* set selects I/O space */
#define IO_BWIN_WIDGET_SHIFT    8
#define IO_BWIN_WIDGET_MASK     (0xfU << IO_BWIN_WIDGET_SHIFT)
#define IO_BWIN_SLICE_MASK      0x1f       /* crosstalk address bits 33:29 */
#define IO_BWIN_SLICE_SHIFT     29

/* Network interface. */
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
 * The port reset register. Three separate resets share it and only one of them
 * is this machine's: bit 7 sends a warm reset out over the inter-node link, bit
 * 1 resets that link, and **bit 0 resets this entire Hub** — which on a
 * single-node machine is the machine.
 *
 * **Three consumers write it and they disagree about the companion bits**,
 * which is why the local-reset bit is tested on its own. Linux's reboot path
 * writes bits 7 and 0 together. OpenBSD writes bits 1 and 0. Real firmware's
 * early self-reset writes 0x81 — bits 7 and 0 — just before reporting
 * that it is waiting for a reset to complete, measured on both PROM images
 * this project boots rather than inferred from either.
 *
 * **The protection register at 0x60_0010 is deliberately not consulted.**
 * OpenBSD arms it before resetting and Linux does not, so requiring it would
 * reset for one guest and silently ignore the other.
 */
#define HUB_NI_PORT_RESET       0x600008
#define NI_PORT_RESET_LOCAL     (1ULL << 0)  /* resets this Hub */

/*
 * The status register's fields. The machine stays in M mode, and link-up
 * reports whether the product wires a peer to its network port. The node field
 * resets to zero on real hardware and is programmed by firmware; synthesized
 * firmware may supply that programmed value through the nasid property. The
 * region-size bit resets set, selecting one region per node.
 */
#define NI_STATUS_MORE_NODES    (1ULL << 18)
#define NI_STATUS_LINK_UP       (1ULL << 29)
#define NI_STATUS_REGION_SIZE   (1ULL << 17)
#define NI_STATUS_NODE_SHIFT    8
#define NI_STATUS_NODE_MASK     (0x1ffULL << NI_STATUS_NODE_SHIFT)
#define NI_STATUS_REVISION      (3ULL << 4)

#define NI_STATUS_RESET         (NI_STATUS_REGION_SIZE | NI_STATUS_REVISION)
#define NI_STATUS_WRITABLE      (NI_STATUS_MORE_NODES | \
                                 NI_STATUS_REGION_SIZE | \
                                 NI_STATUS_NODE_MASK)

/*
 * These registers carry the boot rendezvous between processors and Hubs.
 * Keep their trace separate from the generic MMIO trace so a diagnostic run
 * does not have to record every device access in the machine. cpu_base stays
 * unique while real firmware changes the live NASID from its zero reset value.
 */
static bool sgi_hub_protocol_write_register(hwaddr addr)
{
    switch (addr) {
    case HUB_PI_RT_COUNT:
    case HUB_PI_RT_PENDING_A:
    case HUB_PI_RT_PENDING_B:
    case HUB_PI_RT_ENABLE_A:
    case HUB_PI_RT_ENABLE_B:
    case HUB_PI_CPU_ENABLE_A:
    case HUB_PI_CPU_ENABLE_B:
    case HUB_PI_INT_MODIFY:
    case HUB_PI_RT_COMPARE_A:
    case HUB_PI_RT_COMPARE_B:
    case HUB_NI_STATUS:
    case HUB_NI_SCRATCH_0:
    case HUB_NI_SCRATCH_1:
    case HUB_NI_VECTOR:
    case HUB_NI_VECTOR_DATA:
    case HUB_NI_VECTOR_CLEAR:
        return true;
    default:
        return false;
    }
}

uint32_t sgi_hub_programmed_nasid(const SGIHubState *s)
{
    return (s->ni_status & NI_STATUS_NODE_MASK) >> NI_STATUS_NODE_SHIFT;
}

static void sgi_hub_trace_protocol_write(SGIHubState *s, hwaddr addr,
                                         uint64_t value, unsigned int size)
{
    CPUState *cpu;

    if (!sgi_hub_protocol_write_register(addr)) {
        return;
    }

    cpu = current_cpu;
    trace_sgi_hub_protocol_write(
        cpu ? cpu->cpu_index : -1, s->cpu_base,
        sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
        addr, value, size, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/*
 * The interrupt-modify register: seven bits naming an interrupt number and one
 * bit saying what to do with it. Numbers 0 through 63 are the first pending
 * bank and 64 through 127 the second, so the number's top bit selects the bank.
 * There is no other way to set or clear a bit — the pending banks themselves
 * are
 * read-only — and this is the register a device interrupt arrives at, as a
 * crosstalk write from the I/O bridge rather than as a processor store.
 */
#define PI_INT_MODIFY_NUMBER    0x7f
#define PI_INT_MODIFY_SET       0x100

/* The cached-alias size is a four-bit code, and resets to the largest. */
#define PI_CALIAS_SIZE_MASK     0xf
#define PI_CALIAS_SIZE_RESET    15

/*
 * Which translation entry an address is, if it is one. Entries are eight bytes
 * apart and only the aligned address is the register, so a narrower access part
 * way into one is not treated as the entry.
 */
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

/*
 * Whether addr is one of the measured slots' C register — slots 7 through 14
 * only, matching real firmware's own measured access pattern rather than the
 * full fifteen-slot array. The addr < base check comes first so the
 * subtraction below never wraps before the range is known good.
 */
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

/*
 * Which engine's register an address is, if it is one. The six registers
 * sit at the base of each engine's own 64 KB block and only the aligned
 * 64-bit addresses are registers; the rest of each block stays with the
 * unimplemented default. The addr < base check comes first so the
 * subtraction never wraps before the range is known good.
 */
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

/*
 * Whether addr is one of the eight memory/directory error-cluster registers
 * -- the four error registers and their four clear aliases, evenly strided
 * eight bytes apart from HUB_MD_ERROR_BASE. All eight answer identically
 * (always zero), so one range check serves all of them.
 */
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
 * Point a window where its entry says. The entry names a widget, one of that
 * widget's two address spaces and one 512 MB slice of it, and the window is an
 * alias, so following the entry is moving the alias. A window whose entry names
 * a widget that is not there, or the space this machine does not model, ends up
 * over an empty part of crosstalk space — which is how the hardware disables
 * one, since the table has no valid bit to clear.
 *
 * Empty is not the same as absent. An access through a disabled window must be
 * logged and must not fault: the guests disable windows deliberately, so a
 * disabled window is the normal case rather than an error, and a gap here would
 * turn each one into a bus error. What supplies that is a floor beneath the
 * seven windows, low enough in priority that a window reaching a real widget is
 * unaffected and only the part of a window resolving to nothing shows it.
 *
 * The floor covers the big windows and nothing else. A crossbar port with no
 * widget on it is a different question with a different right answer — that is
 * a device we have not modelled rather than a window somebody turned off — and
 * it keeps faulting.
 */
static void sgi_hub_bwin_follow(SGIHubState *s, unsigned int n)
{
    uint32_t entry = s->io_bwin[n];
    uint64_t widget = (entry & IO_BWIN_WIDGET_MASK) >> IO_BWIN_WIDGET_SHIFT;
    uint64_t slice = (uint64_t)(entry & IO_BWIN_SLICE_MASK)
                     << IO_BWIN_SLICE_SHIFT;
    /*
     * The space-select bit chooses between the widget's memory space, which
     * this machine models, and its I/O space, which it does not. A window
     * asking for the I/O space is reported and left reaching nothing rather
     * than aimed at the memory space anyway: a window on the wrong space
     * returns plausible values that are not the registers the guest asked for,
     * and nothing about a returned value says which space produced it.
     *
     * Which value the redirection uses was open and is now settled at memory,
     * from IRIX programming this entry for this purpose in code compiled for
     * this machine family. This log line is what the other case would say if a
     * guest ever asked for it.
     */
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

/*
 * The floor beneath the big windows: what a window that reaches no widget
 * answers. Reported with the window number, because the offset alone says
 * nothing about which of the seven was disabled.
 *
 * **Answering zero is our choice and not the machine's behaviour**, and no
 * source gives the real one. A crossbar port with nothing on it almost
 * certainly produces an error response rather than zeros; what the datasheets
 * require is only that the access not fault the machine, because disabling a
 * window by naming an absent widget is a thing the guests do deliberately.
 * Zero satisfies that and is a guess about the rest. A guest that ever keys off
 * the difference will have been reading our answer for a long time by then,
 * which is why this says so here rather than leaving it to be inferred.
 */
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
};

/*
 * Recompute what this Hub is asserting. A processor sees a bank's line raised
 * when any bit of that bank is set in the pending register *and* in that
 * processor's mask for it: pending is node-wide state, masking is per
 * processor.
 *
 * The lines are level-triggered, so this is the whole of the output logic and
 * it has to run on every change to a pending bit or a mask. A model that raised
 * a line when a bit was set, rather than recomputing, would leave it asserted
 * after software cleared the last bit it could see — which a guest experiences
 * as an interrupt storm rather than as a lost interrupt.
 *
 * Each processor gets its own pair of lines from its own masks over the same
 * pending banks.
 */
static void sgi_hub_update_irq(SGIHubState *s)
{
    unsigned int bank;
    unsigned int cpu;

    for (cpu = 0; cpu < SGI_HUB_PROCESSORS; cpu++) {
        for (bank = 0; bank < SGI_HUB_PENDING_BANKS; bank++) {
            qemu_set_irq(s->bank_irq[cpu][bank],
                         !!(qatomic_read(&s->pi_int_pend[bank]) &
                            s->pi_int_mask[cpu * SGI_HUB_PENDING_BANKS +
                                           bank]));
        }
        qemu_set_irq(s->rt_irq[cpu], s->pi_rt_pending[cpu] &&
                                     s->pi_rt_enable[cpu]);
    }
}

/*
 * Set or clear one pending bit. The number's low six bits are the bit and its
 * seventh selects the bank, which is what makes numbers 64 through 127 the
 * second bank rather than a separate register.
 */
/*
 * Enable or disable one processor.
 *
 * **Clearing the bit removes the processor, and that is a behaviour rather
 * than a stored value.** The firmware's halt routine writes zero here as its
 * last act; on real hardware the processor is gone before the next instruction
 * retires, so the routine never returns. Storing the bit and running on is
 * what let it return, walk into normal initialisation, and make a two-pass
 * reset look like a one-pass boot.
 *
 * Requesting the halt rather than forcing it is deliberate: this runs inside a
 * memory write, and the same idiom is what MIPS's own power controller uses to
 * stop a processor from a register write.
 */
static void sgi_hub_set_cpu_enable(SGIHubState *s, unsigned int index,
                                   uint64_t val)
{
    CPUState *cs = qemu_get_cpu(s->cpu_base + index);

    s->pi_cpu_enable[index] = val & 1;

    if (!(val & 1) && cs) {
        cpu_interrupt(cs, CPU_INTERRUPT_HALT);
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
        qatomic_or(&s->pi_int_pend[bank], bit);
    } else {
        qatomic_and(&s->pi_int_pend[bank], ~bit);
    }
    sgi_hub_update_irq(s);

    /*
     * Slice B polls PEND1 as a level mailbox. Record that slice A has changed
     * the bank so B can inspect one snapshot before yielding back to A. The
     * credit deliberately does not name a bit: unrelated level state can
     * remain pending while the firmware waits for a different bit to change.
     * Multiple writes coalesce because the pending register itself retains
     * the complete level state.
     */
    /*
     * The observed stock-PROM exchange is PEND1 bit 113. Other PEND1 writes
     * remain on the generic observation path below.
     */
    if (bank == 1 && number == 113 && primary && current_cpu == primary) {
        s->pi_int_pend1_primary_credit = true;
    }

    /*
     * The PROM uses otherwise-unmasked pending bits as node-wide barrier and
     * lock mailboxes. A peer polling one of those bits must observe a write
     * before the writer exhausts the firmware's instruction-count timeout.
     * Wake every other local processor just as the progress mailboxes do;
     * this changes no interrupt state and only makes the already-atomic write
     * visible on the scheduling boundary where real hardware exposes it.
     */
    for (i = 0; i < s->cpus; i++) {
        CPUState *peer = qemu_get_cpu(s->cpu_base + i);

        if (peer && peer != current_cpu) {
            qemu_cpu_handoff(peer);
        }
    }
}

static uint64_t sgi_hub_int_pending_read(SGIHubState *s, unsigned int bank);

static uint64_t sgi_hub_int_pend1_read(SGIHubState *s)
{
    CPUState *primary = qemu_get_cpu(s->cpu_base);
    CPUState *secondary = qemu_get_cpu(s->cpu_base + 1);
    uint64_t value = qatomic_read(&s->pi_int_pend[1]);

    if (s->cpus >= 2 && primary && secondary && current_cpu == secondary &&
        (value & (1ULL << (113 - SGI_HUB_BANK_BITS)))) {
        if (s->pi_int_pend1_primary_credit) {
            s->pi_int_pend1_primary_credit = false;
        } else {
            qemu_cpu_handoff(primary);
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

    /*
     * The stock PROM writes directly into the peer Hub's scratch registers
     * during discovery. On real processors the target node is running at the
     * same time and can consume that phase change immediately. Under RR TCG,
     * the writer can otherwise exhaust the firmware's timeout before any CPU
     * on the target node runs. End the writer's slice without changing Hub or
     * interrupt state, matching the scheduling assists used by the local
     * progress mailboxes above.
     */
    for (i = 0; i < s->cpus; i++) {
        CPUState *peer = qemu_get_cpu(s->cpu_base + i);

        if (peer) {
            qemu_cpu_handoff(peer);
        }
    }
}

/*
 * The stock PROM's slice-pair rendezvous polls a pending bank for a bit its
 * local peer publishes through PI_INT_MODIFY, in both directions: one slice
 * waits for the set, the other for the acknowledging clear.  On real
 * processors the peer runs concurrently and answers inside the poll's bounded
 * budget; under RR TCG the poller can exhaust that budget in one slice while
 * the peer is runnable and never scheduled.  Give the reader's local peer the
 * next slice.  The ordered handoff resumes the displaced successor and bounds
 * nesting, so rotation fairness holds across repeated rounds.
 *
 * Only an unchanged re-read hands off.  A read whose value moved since the
 * same processor's previous read is an answered one -- the producer's own
 * verify read, or a poll observing the edge it waited for -- and the reader
 * must keep its slice to act on it; ending its slice there hands the
 * exchange's window to an unrelated processor.  Deliberately keyed on
 * neither the bank's value nor the mask registers: the observed protocol
 * polls a bank that other pending bits keep nonzero, and the polled-for
 * bit's mask ownership names the acknowledging slice, not the producer, so
 * either key picks the wrong reads to assist.
 */
static uint64_t sgi_hub_int_pending_read(SGIHubState *s, unsigned int bank)
{
    uint64_t value = qatomic_read(&s->pi_int_pend[bank]);
    unsigned int i;

    if (!current_cpu || current_cpu->cpu_index < s->cpu_base ||
        current_cpu->cpu_index >= s->cpu_base + s->cpus) {
        return value;
    }

    i = current_cpu->cpu_index - s->cpu_base;
    if (s->pi_int_pend_seen_valid[i][bank] &&
        s->pi_int_pend_seen[i][bank] == value) {
        for (i = 0; i < s->cpus; i++) {
            CPUState *peer = qemu_get_cpu(s->cpu_base + i);

            if (peer && peer != current_cpu) {
                qemu_cpu_handoff(peer);
            }
        }
        return value;
    }

    s->pi_int_pend_seen[i][bank] = value;
    s->pi_int_pend_seen_valid[i][bank] = true;
    return value;
}

/*
 * Truncate a stored transfer address to the bits the register keeps.
 * Everything above bit 39 is dropped silently — real firmware stores
 * cached-window addresses and depends on exactly that. Bits 6:0 are also
 * not stored, but a value carrying them is misaligned programming, which
 * no observed driver produces and the datasheet asks to be logged.
 */
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

/*
 * Run one engine's armed transfer to completion. Called from the
 * control-register store, which is the launch trigger in both observed
 * programs; MMIO dispatch holds the BQL, which is what serialises the
 * peer-Hub interrupt delivery below just as it does the vector PIO's.
 *
 * The copy is line-at-a-time through the node-local memory view the
 * machine handed this Hub — the engines move node memory and nothing here
 * crosses the Bridge, so no IOMMU is involved, and a node-less address
 * names this node's memory exactly as it does for the node's processors.
 * On our cache-less model the directory protocol's coherent traffic
 * reduces to these plain reads and writes.
 */
static void sgi_hub_bte_launch(SGIHubState *s, unsigned int engine)
{
    uint64_t lines = s->bte_length[engine] & BTE_LENGTH_LINES;
    uint64_t src = s->bte_source[engine];
    uint64_t dst = s->bte_dest[engine];
    uint64_t control = s->bte_control[engine];
    uint64_t done;

    trace_sgi_hub_bte_launch(engine, control, src, dst, lines);

    if (control & BTE_CONTROL_POISON) {
        /*
         * The launch itself is in scope; the poisoned directory state the
         * mode leaves behind belongs to the memory directory, which this
         * model does not have. No guest we boot reaches page migration.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: BTE%u launched in poison mode; the copy "
                      "runs but no directory state is poisoned\n", engine);
    }
    if (lines == 0) {
        /*
         * Never programmed by any observed driver — the kernel returns
         * success without touching the engine — so what the hardware does
         * is open. Falling through treats it as a transfer of zero lines:
         * nothing moves, and the completion below is this model's choice
         * on that open question, not an observed behavior.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: BTE%u launched with a zero length\n", engine);
    }

    for (done = 0; done < lines; done++) {
        uint8_t line[BTE_LINE_SIZE];
        MemTxResult result = MEMTX_OK;

        if (control & BTE_CONTROL_ZFILL) {
            /*
             * Whether the engine reads its source in zero-fill mode is
             * open — every observed launch points it at a page software
             * zeroed itself, so filling with zeros, copying the zeroed
             * page and replicating its first line are indistinguishable.
             * Not reading the source is the least-committal of the three.
             */
            memset(line, 0, sizeof(line));
        } else {
            result = address_space_read(&s->bte_as,
                                        src + done * BTE_LINE_SIZE,
                                        MEMTXATTRS_UNSPECIFIED, line,
                                        sizeof(line));
        }
        if (result == MEMTX_OK) {
            result = address_space_write(&s->bte_as,
                                         dst + done * BTE_LINE_SIZE,
                                         MEMTXATTRS_UNSPECIFIED, line,
                                         sizeof(line));
        }
        if (result != MEMTX_OK) {
            /*
             * A failed transfer latches the error bit and wedges the
             * engine: busy stays set beside error — the datasheet's one
             * direct observation of an errored engine reads the two
             * together — and the length field keeps the lines that never
             * moved. A later store to length/status clears busy and never
             * error; the engine's bit of the I/O error-clear register
             * clears error. On real hardware the error surfaces through
             * the Hub's own error machinery, not through a completion
             * signal — a hardware notification write on error is not
             * established, so neither completion path runs — and this
             * model has no Hub error interrupt to raise in its place.
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
    }

    /*
     * The count drained to zero and busy cleared; only a previously
     * latched error bit could remain, and on success the register — and
     * so the notification value below — reads zero.
     */
    s->bte_length[engine] &= BTE_LENGTH_ERROR;

    if (control & BTE_CONTROL_NOTIFY) {
        /*
         * The final length/status value as one 64-bit doubleword in guest
         * byte order at the notification address. Exactly eight bytes:
         * whether hardware writes more of the line is unobserved, and the
         * kernel reads only the first doubleword.
         */
        uint8_t status[8];

        stq_be_p(status, s->bte_length[engine]);
        if (address_space_write(&s->bte_as, s->bte_notify[engine],
                                MEMTXATTRS_UNSPECIFIED, status,
                                sizeof(status)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-hub: BTE%u notification address 0x%" PRIx64
                          " reaches no memory\n",
                          engine, s->bte_notify[engine]);
        }
    } else {
        /*
         * Interrupt mode: the completion arrives exactly as a device
         * interrupt does, so it goes through the interrupt-modify
         * machinery rather than a second path. The level comes from the
         * register — the diagnostic's 63 and 62 are its choice, not the
         * hardware's. The node field names a processor interface; this
         * Hub and its direct network peer are the only two this machine
         * can have.
         */
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
        /*
         * A load stops the transfer in progress. Transfers here complete
         * inside the store that launches them, so there is never one for
         * this load to stop; what the load returns is unobserved — every
         * driver discards it — and the datasheet's proposal, the written
         * control bits, is what a register that stores its bits answers.
         */
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
         * Busy and the line count take the written value; a set error bit
         * survives the store. SGI's own driver stores zero to a wedged
         * engine's length/status, states the write turns off busy and
         * will not clear the error bits, and asserts afterwards that
         * error is still set — so a store can arm or disarm the engine
         * but never absolve it. Whether a store can *set* the error bit
         * is open (nothing observed writes a one there); this model does
         * not let it, so the bit only ever reports a failure the engine
         * actually took — a choice, not a finding. Arming is not
         * launching: the diagnostic arms busy and length before the
         * source register holds anything, and only the control store
         * below starts the engine.
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
        /*
         * The store launches the armed transfer — last in both observed
         * programming sequences, and the diagnostic's order proves the
         * length/status store cannot be the trigger. Every observed
         * launch has busy armed; what a control store does when busy is
         * clear is open, and the datasheet's proposal — nothing — is
         * implemented here.
         */
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

static uint64_t sgi_hub_rt_compare_read(SGIHubState *s, unsigned int i)
{
    CPUState *owner = qemu_get_cpu(s->cpu_base + i);

    /*
     * Firmware polls the peer's progress through these registers with an
     * instruction-count timeout. Give the owner a chance to publish its next
     * value before an MTTCG reader can consume that entire loop in one host
     * time slice.
     */
    if (owner && owner != current_cpu) {
        qemu_cpu_handoff(owner);
    }
    return qatomic_read(&s->pi_rt_compare[i]);
}

/* Move the origin so the counter reads what was written, and keep running. */
static void sgi_hub_rt_set(SGIHubState *s, uint64_t val)
{
    val &= PI_RT_MASK;
    s->pi_rt_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                         muldiv64(val, NANOSECONDS_PER_SECOND, PI_RT_HZ);
}

/*
 * Arm one comparator against the counter as it stands now. The distance is
 * taken forward and wrapping; a comparator the counter has already passed is
 * a match and is delivered at once, which the arm below detects rather than
 * arming a deadline a revolution away.
 *
 * The distance is one to 2^52 rather than zero to 2^52 - 1, so a comparator
 * that already equals the counter counts as passed as well. No manual states
 * which way the hardware goes — a continuous equality comparator would call
 * that a match, a counter that has to count into the value would not — and
 * neither guest can tell us, because Linux clears the pending bit straight
 * after zeroing the counter and OpenBSD never arms a comparator at all.
 *
 * The one piece of evidence points at counting in. IRIX's own timer setup
 * computes the distance to the requested deadline and, when that distance is
 * zero or negative, does not write the comparator at all: it runs the timeout
 * handler in software instead. Routing around the hardware costs something,
 * and nobody pays it for a comparator that would have fired immediately by
 * itself — which reads as a belief that an already-reached value does not
 * fire until wraparound.
 *
 * That inference no longer decides it, because the same public sources cut
 * both ways. Linux writes this comparator behind the counter routinely and
 * carries a read-back guard for exactly that case, so its authors expected a
 * behind-write to be possible and detectable; and it advertises a maximum
 * interval of the whole counter space, which only means anything under wrap.
 * The firmware writes the comparator constantly and races the same window
 * every boot without ever being observed to lose. What the silicon does with
 * a value the counter has passed remains unobserved.
 *
 * So the choice is made on failure mode rather than on evidence. Wrapping
 * strands the interrupt for a full revolution — 114 years here — which is
 * silent and permanent for any writer without a read-back guard. Matching at
 * once costs a guarded writer one early tick, and an unguarded one a livelock
 * if its handler outruns its own interval, which is loud and diagnosable in
 * seconds. Of two behaviours we cannot separate, take the one that fails
 * where somebody will see it.
 */
static void sgi_hub_rt_match(SGIHubState *s, unsigned int index)
{
    s->pi_rt_pending[index] = 1;
    if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_EXPIRE)) {
        trace_sgi_hub_rt_expire(
            s->cpu_base, sgi_hub_programmed_nasid(s), index,
            sgi_hub_rt_count(s), qatomic_read(&s->pi_rt_compare[index]),
            s->pi_rt_enable[index],
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    sgi_hub_update_irq(s);
}

/*
 * Arm the comparator, or match it now if the counter has already passed it.
 *
 * @programmed says the guest wrote this deadline. Only then is an already
 * reached value a match: moving the counter under a comparator is a change of
 * origin rather than a deadline, and at reset both comparators read zero, so
 * treating that as a match would pend two interrupts the guest never asked
 * for on the first counter write of every boot.
 *
 * The space is modular, so a value one tick behind and one a full revolution
 * ahead are the same number and no scheme can separate them. Splitting at half
 * a revolution puts every deadline a guest actually programs — milliseconds —
 * on the future side, and accepts that an arm asking for the longest possible
 * interval matches immediately instead.
 */
static void sgi_hub_rt_arm(SGIHubState *s, unsigned int index, bool programmed)
{
    uint64_t ticks = ((qatomic_read(&s->pi_rt_compare[index]) -
                       sgi_hub_rt_count(s) - 1) &
                      PI_RT_MASK) + 1;

    if (programmed && ticks > (PI_RT_MASK >> 1)) {
        /*
         * Delete rather than leave armed: the wrap deadline this comparator
         * would otherwise still carry would deliver a second pend later for a
         * match already made.
         */
        timer_del(s->rt_timer[index]);
        sgi_hub_rt_match(s, index);
        return;
    }

    timer_mod(s->rt_timer[index], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(ticks, NANOSECONDS_PER_SECOND, PI_RT_HZ));
}

/*
 * Deliberately not re-armed at a match. A comparator would match again one
 * full revolution later, which at 1.25 MHz over 52 bits is longer than any
 * run. The guests that use it rewrite it from every handler.
 */
/*
 * A bit slot finished.
 *
 * **Nothing is attached to this bus, and that is a state the protocol defines
 * rather than an absence of behaviour.** The line is open-drain with a pull-up:
 * idle is high, and a part signals by pulling it low. So the master drives its
 * pulse, releases, samples a line nothing is holding down, and reads one —
 * every slot, and completion signals normally, because the master's own timing
 * ran whether or not anybody answered.
 *
 * **This is a deliberate stand-in and must not be read as a model of the
 * hardware.** SGI's own firmware manual says every board type carries an
 * identity part, including the node card, so a real Origin 200 almost
 * certainly has one on this line. What is not established is *which* part —
 * and modelling no part is the only statement available that is not a
 * fabrication. Attaching an invented responder, or one borrowed from another
 * board, would turn a visible absence into a plausible identity that nothing
 * downstream could tell was invented.
 */
static void sgi_hub_onewire_slot_done(void *opaque)
{
    SGIHubState *s = opaque;

    s->md_onewire |= MD_ONEWIRE_DONE | MD_ONEWIRE_RXDATA;
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

    if (route) {
        *value = *route;
        return true;
    }
    switch (addr) {
    case HUB_NI_STATUS:
        *value = target->ni_status;
        return true;
    case HUB_NI_SCRATCH_0:
        *value = target->ni_scratch[0];
        return true;
    case HUB_NI_SCRATCH_1:
        *value = target->ni_scratch[1];
        return true;
    default:
        return false;
    }
}

static bool sgi_hub_vector_target_write(SGIHubState *target, hwaddr addr,
                                        uint64_t value)
{
    uint8_t *route = sgi_hub_route_entry(target, addr);

    if (route) {
        *route = value & NI_ROUTE_MASK;
        return true;
    }
    switch (addr) {
    case HUB_NI_STATUS:
        target->ni_status = (target->ni_status & ~NI_STATUS_WRITABLE) |
                            (value & NI_STATUS_WRITABLE);
        return true;
    case HUB_NI_SCRATCH_0:
        target->ni_scratch[0] = value;
        return true;
    case HUB_NI_SCRATCH_1:
        target->ni_scratch[1] = value;
        return true;
    default:
        return false;
    }
}

/*
 * Launch the vector PIO described by NI_VECTOR_PARMS.
 *
 * An Origin 200's network port is wired directly to the other chassis, so a
 * null route names that peer. Non-null routes require a router and fail with
 * the hardware's address-error response; this keeps the direct topology from
 * silently pretending it can route an Origin 2000 network.
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

    /*
     * network_peer is a raw pointer to the other chassis's Hub, read and
     * dereferenced below with no lock of its own; it is serialized only by
     * BQL MMIO dispatch, so this function must not run off the BQL. Assert
     * before the first dereference, not after.
     */
    assert(bql_locked());

    if (s->network_peer && s->ni_vector == 0) {
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

    source = s->network_peer ?
        (s->network_peer->ni_status & NI_STATUS_NODE_MASK) >>
        NI_STATUS_NODE_SHIFT : 0;

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
            s->network_peer ?
                (int)sgi_hub_programmed_nasid(s->network_peer) : -1,
            cpu ? cpu->cc->get_pc(cpu) : 0, parms, s->ni_vector_data, value,
            s->ni_vector_status, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static uint64_t sgi_hub_read_internal(void *opaque, hwaddr addr,
                                      unsigned size)
{
    SGIHubState *s = opaque;
    uint64_t value;
    uint8_t *route;
    unsigned int n;
    hwaddr reg;

    if (sgi_hub_bwin_entry(addr, &n)) {
        return s->io_bwin[n];
    }
    if (sgi_hub_bte_entry(addr, &n, &reg)) {
        return sgi_hub_bte_read(s, n, reg);
    }
    route = sgi_hub_route_entry(s, addr);
    if (route) {
        return *route;
    }
    if (sgi_hub_icrb_c_entry(addr)) {
        /*
         * No slot has ever tracked a real incoming crosstalk transaction, so
         * every field a real slot's C register would hold is honestly zero.
         */
        return 0;
    }
    if (sgi_hub_md_error_entry(addr)) {
        /*
         * This model never raises a memory, directory, protocol or
         * miscellaneous error, so every field any of these eight registers
         * would hold is honestly zero -- which is also what mtest_basic
         * requires before it will run at all.
         */
        return 0;
    }
    if (addr >= HUB_PI_ERR_STATUS_BASE && addr <= HUB_PI_ERR_STATUS_LAST &&
        (addr - HUB_PI_ERR_STATUS_BASE) % HUB_PI_ERR_STATUS_STRIDE == 0) {
        /* No reviewed event supplies a nonzero report on this model. */
        return 0;
    }

    switch (addr) {
    case HUB_PI_RT_COUNT:
        value = sgi_hub_rt_count(s);
        goto trace_rt_count_read;
    case HUB_PI_RT_COMPARE_A:
        value = sgi_hub_rt_compare_read(s, 0);
        goto trace_protocol_read;
    case HUB_PI_RT_COMPARE_B:
        value = sgi_hub_rt_compare_read(s, 1);
        goto trace_protocol_read;
    case HUB_PI_RT_PENDING_A:
        value = s->pi_rt_pending[0];
        goto trace_protocol_read;
    case HUB_PI_RT_PENDING_B:
        value = s->pi_rt_pending[1];
        goto trace_protocol_read;
    case HUB_PI_RT_ENABLE_A:  return s->pi_rt_enable[0];
    case HUB_PI_RT_ENABLE_B:  return s->pi_rt_enable[1];
    case HUB_PI_RT_LOCAL_CTRL: return s->pi_rt_local_ctrl;
    case HUB_PI_CPU_PRESENT_A:
        return s->cpus >= 1;
    case HUB_PI_CPU_PRESENT_B:
        return s->cpus >= 2;
    case HUB_PI_ERR_SPOOL_A:
        return s->pi_err_spool[0];
    case HUB_PI_ERR_SPOOL_B:
        return s->pi_err_spool[1];
    case HUB_PI_ERR_PENDING:
        return 0;
    case HUB_PI_ERR_MASK_A:
        return s->pi_err_mask[0];
    case HUB_PI_ERR_MASK_B:
        return s->pi_err_mask[1];
    case HUB_PI_ERR_SPOOL_SIZE:
        return s->pi_err_spool_size;
    case HUB_PI_CPU_ENABLE_A:
        return s->pi_cpu_enable[0];
    case HUB_PI_CPU_ENABLE_B:
        return s->pi_cpu_enable[1];
    case HUB_PI_REGION_PRESENT:
        return s->pi_region_present;
    case HUB_PI_PROCESSOR_NUMBER:
        /*
         * Sound under MTTCG: MMIO is dispatched on the reading vCPU's own
         * thread and current_cpu is thread-local, so each reader gets its
         * own answer and can never be misattributed to another vCPU's read
         * racing it. The !current_cpu case covers a main-loop or qtest
         * reader, which has no processor of its own to name.
         */
        if (current_cpu && current_cpu->cpu_index >= s->cpu_base &&
            current_cpu->cpu_index < s->cpu_base + s->cpus) {
            return current_cpu->cpu_index - s->cpu_base;
        }
        return 0;
    case HUB_PI_CALIAS_SIZE:
        return s->pi_calias_size;
    case HUB_PI_INT_PEND0:
        value = sgi_hub_int_pending_read(s, 0);
        goto trace_protocol_read;
    case HUB_PI_INT_PEND1:
        value = sgi_hub_int_pend1_read(s);
        goto trace_protocol_read;
    case HUB_PI_INT_MASK0_A:
        return s->pi_int_mask[0];
    case HUB_PI_INT_MASK1_A:
        return s->pi_int_mask[1];
    case HUB_PI_INT_MASK0_B:
        return s->pi_int_mask[2];
    case HUB_PI_INT_MASK1_B:
        return s->pi_int_mask[3];
    case HUB_IO_WIDGET_ID:
        return IO_WIDGET_ID_REVISION | IO_WIDGET_ID_PART |
               ((uint64_t)s->manufacturer << IO_WIDGET_ID_MFG_SHIFT) |
               IO_WIDGET_ID_ONE;
    case HUB_IO_WIDGET_STATUS:
        /*
         * Nothing this model does sets an error, a retry count or an
         * outstanding-request count: transfers complete instantly and the link
         * never fails. Bit 4 reads zero because SGI's own table records it as
         * undefined and assumed reserved, which is preserved rather than
         * resolved.
         */
        return 0;
    case HUB_IO_WIDGET_CONTROL:
        return s->io_widget_control;
    case HUB_IO_XTALK_TIMEOUT:
        return s->io_xtalk_timeout;
    case HUB_IO_XTALK_PRESCALE:
        return s->io_xtalk_prescale;
    case HUB_IO_SCRATCH_0:
        return s->io_scratch[0];
    case HUB_IO_SCRATCH_1:
        return s->io_scratch[1];
    case HUB_IO_LINK_STATUS:
        return s->io_link_status;
    case HUB_NI_STATUS:
        return s->ni_status;
    case HUB_NI_SCRATCH_0:
        value = s->ni_scratch[0];
        goto trace_protocol_read;
    case HUB_NI_SCRATCH_1:
        value = s->ni_scratch[1];
        goto trace_protocol_read;
    case HUB_NI_VECTOR:
        return s->ni_vector;
    case HUB_NI_VECTOR_DATA:
        return s->ni_vector_data;
    case HUB_NI_VECTOR_STATUS:
        value = s->ni_vector_status;
        goto trace_protocol_read;
    case HUB_NI_RETURN_VECTOR:
        return s->ni_return_vector;
    case HUB_NI_VECTOR_READ_DATA:
        value = s->ni_vector_read_data;
        goto trace_protocol_read;
    case HUB_NI_VECTOR_CLEAR:
        s->ni_vector_status = 0;
        value = 0;
        goto trace_protocol_read;
    case HUB_MD_SLOT_ID_USTAT:
        return MD_SLOT_FLASH_READY | MD_SLOT_XJMP_ID |
               (s->slave_module ? 0 : MD_SLOT_MASTER_MODULE);
    case HUB_MD_CONFIG:
        return s->md_config;
    case HUB_MD_ONEWIRE:
        return s->md_onewire;
    case HUB_MD_PERF_CNT0:
        return s->md_perf_cnt0;
    case HUB_PI_INT_MODIFY:
        /*
         * Write-only: the register is an instruction rather than a value, and
         * the state it modifies is read through the pending banks.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: read of the write-only interrupt-modify "
                      "register\n");
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: read of unimplemented register 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }

trace_rt_count_read:
    if (trace_event_get_state_backends(TRACE_SGI_HUB_RT_COUNT_READ)) {
        CPUState *cpu = current_cpu;

        trace_sgi_hub_rt_count_read(
            cpu ? cpu->cpu_index : -1, s->cpu_base,
            sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
            value, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return value;

trace_protocol_read:
    if (trace_event_get_state_backends(TRACE_SGI_HUB_PROTOCOL_READ)) {
        CPUState *cpu = current_cpu;

        trace_sgi_hub_protocol_read(
            cpu ? cpu->cpu_index : -1, s->cpu_base,
            sgi_hub_programmed_nasid(s), cpu ? cpu->cc->get_pc(cpu) : 0,
            addr, value, size, qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return value;
}

/*
 * Real firmware's diagnostic progress codes, one per boot stage, from the
 * PROM's own progress-LED enumeration (stand/arcs/IP27prom/prom_leds.h,
 * IRIX 6.5.7) -- transcribed as facts in our own words, not the vendor's
 * identifiers or comments, per docs/datasheets/hub-memory-directory.md.
 * A NULL entry is a code the source itself marks reserved or unused.
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

/*
 * Real firmware's diagnosed-failure codes, from the same source. Codes
 * outside 0x00-0x60 and 0x81-0xb5 are not part of this enumeration at all --
 * see sgi_hub_led_decode().
 */
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
    [0x0b] = "the processor took an exception outside every specifically-handled category", /* 0x8c */
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
    [0x16] = "execution fell out of the firmware's main() routine, which should never return", /* 0x97 */
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
    [0x2c] = "computing the paths packets should take between nodes did not succeed", /* 0xad */
    [0x2d] = "sending the computed routing table out to the rest of the machine did not succeed", /* 0xae */
    [0x2e] = "node-id distribution failed", /* 0xaf */
    [0x2f] = "boot master assigned no node id", /* 0xb0 */
    [0x30] = "module-id arbitration failed", /* 0xb1 */
    [0x31] = "an SN0 node mixed with an SN00 node", /* 0xb2 */
    [0x32] = "a partition-configuration error", /* 0xb3 */
    [0x33] = "an error copying mode bits", /* 0xb4 */
    [0x34] = "an error calculating the backplane frequency", /* 0xb5 */
};

/*
 * Decode a value written to an indicator register into the diagnostic
 * meaning real firmware gives it, when it has one. is_failure distinguishes
 * a diagnosed failure from ordinary boot progress; the return value is NULL
 * for anything the enumeration does not cover, which includes both the
 * reserved slots the source itself sets aside and the unrelated pattern the
 * firmware flashes while it waits for a character (0x80 XORed against a
 * per-device bit pattern, documented in
 * docs/datasheets/hub-memory-directory.md) -- so an undecoded value is not
 * automatically a fault, only an unenumerated one.
 */
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

static bool sgi_hub_diagnostic_value(SGIHubState *s, hwaddr addr,
                                     uint64_t write_value, uint64_t *value)
{
    uint8_t *route;
    unsigned int n;
    hwaddr reg;

    if (sgi_hub_bwin_entry(addr, &n)) {
        *value = s->io_bwin[n];
        return true;
    }
    if (sgi_hub_bte_entry(addr, &n, &reg)) {
        *value = sgi_hub_bte_read(s, n, reg);
        return true;
    }
    route = sgi_hub_route_entry(s, addr);
    if (route) {
        *value = *route;
        return true;
    }

    switch (addr) {
    case HUB_PI_RT_COUNT:
        *value = sgi_hub_rt_count(s);
        return true;
    case HUB_PI_RT_COMPARE_A:
        *value = qatomic_read(&s->pi_rt_compare[0]);
        return true;
    case HUB_PI_RT_COMPARE_B:
        *value = qatomic_read(&s->pi_rt_compare[1]);
        return true;
    case HUB_PI_RT_PENDING_A:
        *value = s->pi_rt_pending[0];
        return true;
    case HUB_PI_RT_PENDING_B:
        *value = s->pi_rt_pending[1];
        return true;
    case HUB_PI_RT_ENABLE_A:
        *value = s->pi_rt_enable[0];
        return true;
    case HUB_PI_RT_ENABLE_B:
        *value = s->pi_rt_enable[1];
        return true;
    case HUB_PI_RT_LOCAL_CTRL:
        *value = s->pi_rt_local_ctrl;
        return true;
    case HUB_PI_ERR_SPOOL_A:
        *value = s->pi_err_spool[0];
        return true;
    case HUB_PI_ERR_SPOOL_B:
        *value = s->pi_err_spool[1];
        return true;
    case HUB_PI_ERR_PENDING:
        *value = 0;
        return true;
    case HUB_PI_ERR_MASK_A:
        *value = s->pi_err_mask[0];
        return true;
    case HUB_PI_ERR_MASK_B:
        *value = s->pi_err_mask[1];
        return true;
    case HUB_PI_ERR_SPOOL_SIZE:
        *value = s->pi_err_spool_size;
        return true;
    case HUB_PI_CPU_ENABLE_A:
        *value = s->pi_cpu_enable[0];
        return true;
    case HUB_PI_CPU_ENABLE_B:
        *value = s->pi_cpu_enable[1];
        return true;
    case HUB_PI_INT_MODIFY:
        *value = qatomic_read(&s->pi_int_pend[
            (write_value & PI_INT_MODIFY_NUMBER) / SGI_HUB_BANK_BITS]);
        return true;
    case HUB_PI_REGION_PRESENT:
        *value = s->pi_region_present;
        return true;
    case HUB_PI_CALIAS_SIZE:
        *value = s->pi_calias_size;
        return true;
    case HUB_PI_INT_MASK0_A:
        *value = s->pi_int_mask[0];
        return true;
    case HUB_PI_INT_MASK1_A:
        *value = s->pi_int_mask[1];
        return true;
    case HUB_PI_INT_MASK0_B:
        *value = s->pi_int_mask[2];
        return true;
    case HUB_PI_INT_MASK1_B:
        *value = s->pi_int_mask[3];
        return true;
    case HUB_IO_WIDGET_CONTROL:
        *value = s->io_widget_control;
        return true;
    case HUB_IO_XTALK_TIMEOUT:
        *value = s->io_xtalk_timeout;
        return true;
    case HUB_IO_XTALK_PRESCALE:
        *value = s->io_xtalk_prescale;
        return true;
    case HUB_IO_SCRATCH_0:
        *value = s->io_scratch[0];
        return true;
    case HUB_IO_SCRATCH_1:
        *value = s->io_scratch[1];
        return true;
    case HUB_IO_LINK_STATUS:
        *value = s->io_link_status;
        return true;
    case HUB_MD_CONFIG:
        *value = s->md_config;
        return true;
    case HUB_MD_ONEWIRE:
        *value = s->md_onewire;
        return true;
    case HUB_MD_PERF_CNT0:
        *value = s->md_perf_cnt0;
        return true;
    case HUB_NI_STATUS:
        *value = s->ni_status;
        return true;
    case HUB_NI_SCRATCH_0:
        *value = s->ni_scratch[0];
        return true;
    case HUB_NI_SCRATCH_1:
        *value = s->ni_scratch[1];
        return true;
    case HUB_NI_VECTOR:
        *value = s->ni_vector;
        return true;
    case HUB_NI_VECTOR_DATA:
        *value = s->ni_vector_data;
        return true;
    case HUB_NI_VECTOR_STATUS:
    case HUB_NI_VECTOR_CLEAR:
        *value = s->ni_vector_status;
        return true;
    case HUB_NI_RETURN_VECTOR:
        *value = s->ni_return_vector;
        return true;
    case HUB_NI_VECTOR_READ_DATA:
        *value = s->ni_vector_read_data;
        return true;
    default:
        return false;
    }
}

static bool sgi_hub_diagnostic_read_value(SGIHubState *s, hwaddr addr,
                                          uint64_t *value)
{
    if (addr == HUB_NI_VECTOR_CLEAR) {
        *value = s->ni_vector_status;
        return true;
    }
    return false;
}

static uint64_t sgi_hub_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIHubState *s = opaque;
    uint64_t before = 0;
    uint64_t value;
    uint64_t after = 0;
    bool has_before_after;

    if (likely(!s->diagnostic)) {
        return sgi_hub_read_internal(opaque, addr, size);
    }

    has_before_after = sgi_hub_diagnostic_read_value(s, addr, &before);
    value = sgi_hub_read_internal(opaque, addr, size);
    if (has_before_after &&
        !sgi_hub_diagnostic_read_value(s, addr, &after)) {
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
    uint8_t *route;
    unsigned int n;
    hwaddr reg;

    if (trace_event_get_state_backends(TRACE_SGI_HUB_PROTOCOL_WRITE)) {
        sgi_hub_trace_protocol_write(s, addr, val, size);
    }

    if (sgi_hub_bwin_entry(addr, &n)) {
        /*
         * The read-back this implies is a guest requirement rather than a
         * convention: OpenBSD's large-window allocator reads an entry straight
         * back after writing it, so a model that acted on a write without
         * storing it would fail a guest and not only a test.
         */
        s->io_bwin[n] = val & IO_BWIN_MASK;
        sgi_hub_bwin_follow(s, n);
        return;
    }
    if (sgi_hub_bte_entry(addr, &n, &reg)) {
        sgi_hub_bte_write(s, n, reg, val);
        return;
    }
    route = sgi_hub_route_entry(s, addr);
    if (route) {
        *route = val & NI_ROUTE_MASK;
        return;
    }
    if (sgi_hub_icrb_c_entry(addr)) {
        /*
         * Real hardware fills this from a crosstalk transaction in flight,
         * not from a guest write, so there is nothing to keep even when one
         * arrives. Real firmware does write here, though -- measured across
         * both machine configurations, PROM writes 0x0 to every one of these
         * eight registers early in every boot, well before either console
         * fault fires. That is some generic range clear sweeping past a
         * register it has no reason to know is special, not a guest asking
         * this register to do anything; a real slot's C register would
         * ignore the write the same way. Unimplemented, not misbehaviour.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only "
                      "incoming-request-buffer register 0x%" HWADDR_PRIx
                      " (real firmware writes 0x0 here every boot; not "
                      "modelled)\n",
                      val, addr);
        return;
    }
    if (sgi_hub_md_error_entry(addr)) {
        /*
         * The plain registers are documented read-only; the aliases are
         * inferred, not proven, to clear on read rather than on write. This
         * model never sets an error bit in the first place, so there is
         * nothing for either kind of write to change -- accepted and logged
         * as unimplemented, not as guest misbehaviour.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the memory/"
                      "directory error-cluster register 0x%" HWADDR_PRIx
                      " (nothing here ever latches an error to clear)\n",
                      val, addr);
        return;
    }
    if (addr == HUB_MD_MEM_DIMM_INIT || addr == HUB_MD_DIR_DIMM_INIT) {
        /*
         * Write-only in both dual sources, and real hardware's DIMM mode
         * setting has no functional signature on a machine whose transfers
         * complete instantly. Real firmware writes here as its own per-DIMM
         * programming sweep, not as a guest asking for unbuilt behaviour.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the DIMM mode "
                      "initialisation register 0x%" HWADDR_PRIx " (write-"
                      "only; no functional signature on this model)\n",
                      val, addr);
        return;
    }

    switch (addr) {
    case HUB_PI_RT_COUNT:
        sgi_hub_rt_set(s, val);
        /* Moving the counter moves every deadline standing against it. */
        sgi_hub_rt_arm(s, 0, false);
        sgi_hub_rt_arm(s, 1, false);
        break;
    case HUB_PI_RT_COMPARE_A:
    case HUB_PI_RT_COMPARE_B: {
        unsigned int i = (addr == HUB_PI_RT_COMPARE_B);
        CPUState *peer;

        qatomic_set(&s->pi_rt_compare[i], val & PI_RT_MASK);
        sgi_hub_rt_arm(s, i, true);

        /*
         * The boot PROM uses these registers as progress mailboxes while the
         * two local processors arbitrate. Its peer timeout is a tight MMIO
         * read loop, short enough for one MTTCG host thread to exhaust before
         * the other vCPU receives a host time slice. A real register write is
         * immediately visible to the peer; kick that vCPU so host scheduling
         * cannot turn the firmware's instruction-count timeout into a false
         * dead-processor report. This changes no register or interrupt state.
         *
         * Only while the comparator is a mailbox, which the slice's own
         * interrupt enable distinguishes: the arbitration polls with the
         * interrupt disabled, while a guest that has enabled it is writing a
         * deadline from its own tick path and re-arms on every tick. Under
         * round-robin, a handoff here ends the writer's slice between its
         * compare write and its read-back of the count, so the wall-clock
         * time the peer's slice consumes lands inside that window and every
         * deadline the guest asks for has already passed when it checks —
         * the Linux SMP boot livelocked at its clocksource switch on
         * exactly that loop. The gate assumes a deadline writer enables the
         * interrupt first, as Linux does; a guest that polled a deadline
         * with the interrupt disabled would still donate its slice here and
         * could meet the same livelock.
         */
        peer = qemu_get_cpu(s->cpu_base + (i ^ 1));
        if (!s->pi_rt_enable[i] && peer && (i ^ 1) < s->cpus) {
            qemu_cpu_handoff(peer);
        }
        break;
    }
    case HUB_PI_RT_PENDING_A:
    case HUB_PI_RT_PENDING_B:
        /* The handler acknowledges by writing zero, which drops the line. */
        s->pi_rt_pending[addr == HUB_PI_RT_PENDING_B] = val & 1;
        sgi_hub_update_irq(s);
        break;
    case HUB_PI_RT_ENABLE_A:
    case HUB_PI_RT_ENABLE_B:
        s->pi_rt_enable[addr == HUB_PI_RT_ENABLE_B] = val & 1;
        sgi_hub_update_irq(s);
        break;
    case HUB_PI_RT_LOCAL_CTRL:
        s->pi_rt_local_ctrl = val & PI_RT_LOCAL_CTRL_MASK;
        break;
    case HUB_PI_ERR_SPOOL_A:
        s->pi_err_spool[0] = val & PI_ERR_SPOOL_MASK;
        break;
    case HUB_PI_ERR_SPOOL_B:
        s->pi_err_spool[1] = val & PI_ERR_SPOOL_MASK;
        break;
    case HUB_PI_ERR_PENDING:
        /* Write-one-to-clear has no effect while no error source can set it. */
        break;
    case HUB_PI_ERR_MASK_A:
        s->pi_err_mask[0] = val & PI_ERR_MASK;
        break;
    case HUB_PI_ERR_MASK_B:
        s->pi_err_mask[1] = val & PI_ERR_MASK;
        break;
    case HUB_PI_ERR_SPOOL_SIZE:
        s->pi_err_spool_size = val & PI_ERR_SPOOL_SIZE_MASK;
        break;
    case HUB_PI_CPU_ENABLE_A:
        sgi_hub_set_cpu_enable(s, 0, val);
        break;
    case HUB_PI_CPU_ENABLE_B:
        sgi_hub_set_cpu_enable(s, 1, val);
        break;
    case HUB_PI_INT_MODIFY:
        sgi_hub_int_modify(s, val);
        break;
    case HUB_PI_REGION_PRESENT:
        s->pi_region_present = val;
        break;
    case HUB_PI_CALIAS_SIZE:
        s->pi_calias_size = val & PI_CALIAS_SIZE_MASK;
        break;
    case HUB_PI_INT_MASK0_A:
        s->pi_int_mask[0] = val;
        sgi_hub_update_irq(s);
        break;
    case HUB_PI_INT_MASK1_A:
        s->pi_int_mask[1] = val;
        sgi_hub_update_irq(s);
        break;
    case HUB_PI_INT_MASK0_B:
        s->pi_int_mask[2] = val;
        sgi_hub_update_irq(s);
        break;
    case HUB_PI_INT_MASK1_B:
        s->pi_int_mask[3] = val;
        sgi_hub_update_irq(s);
        break;
    case HUB_IO_WIDGET_CONTROL:
        s->io_widget_control = (s->io_widget_control &
                                ~IO_WIDGET_CONTROL_WRITABLE) |
                               (val & IO_WIDGET_CONTROL_WRITABLE);
        break;
    case HUB_IO_XTALK_TIMEOUT:
        s->io_xtalk_timeout = val & IO_XTALK_TIMEOUT_MASK;
        break;
    case HUB_IO_XTALK_PRESCALE:
        s->io_xtalk_prescale = val & IO_XTALK_PRESCALE_MASK;
        break;
    case HUB_IO_SCRATCH_0:
        s->io_scratch[0] = val & IO_SCRATCH_MASK;
        break;
    case HUB_IO_SCRATCH_1:
        s->io_scratch[1] = val & IO_SCRATCH_MASK;
        break;
    case HUB_MD_CONFIG:
        s->md_config = val & MD_CONFIG_MASK;
        break;
    case HUB_MD_ONEWIRE:
        /*
         * A write programs the timing and starts a bit slot.
         *
         * The two status bits are the device's to report, so the write cannot
         * set them: starting a transaction clears completion, and it reads
         * clear until the slot finishes. That is the property the contract
         * rests on, and it holds whichever of the disputed reset values is
         * right.
         */
        s->md_onewire = val & MD_ONEWIRE_WRITABLE;
        timer_mod(s->md_onewire_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MD_ONEWIRE_SLOT_NS);
        break;
    case HUB_MD_PERF_CNT0:
        /*
         * A real self-test resets the counter before a measurement pass;
         * accepted and stored so the read that follows sees what was written,
         * with no counting behaviour attached (see the definition above).
         */
        s->md_perf_cnt0 = val & MD_PERF_CNT_MASK;
        break;
    case HUB_IO_WIDGET_ID:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only widget "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    case HUB_IO_WIDGET_STATUS:
        /*
         * Split from the identification register above, which a guest has no
         * business writing at all. This one is status: the error flag, the
         * retry count and the outstanding-request count are exactly the state
         * firmware clears before it starts using the link, and the shipped
         * PROM writes zero here once during power-on, on every boot. Refused
         * because this model never sets any of those bits, so there is nothing
         * for a clear to do and the register reads zero either way — but a
         * clear at power-on is the register being used as designed, so it logs
         * as unimplemented rather than as guest misbehaviour.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the widget status "
                      "register 0x%" HWADDR_PRIx " (firmware clears this at "
                      "power-on; nothing here ever sets it)\n", val, addr);
        break;
    case HUB_IO_LINK_STATUS:
        s->io_link_status = (s->io_link_status & ~IO_LINK_WRITABLE) |
                            (val & IO_LINK_WRITABLE);
        break;
    case HUB_IO_ERROR_CLEAR:
        /*
         * Documented effect: writing a bit clears the corresponding error
         * latch. The two block-transfer-engine bits now have a latch to
         * clear -- a failed transfer sets the engine's error bit in its
         * length/status register, no store to that register clears it, and
         * this is the register the kernel writes to absolve an engine (once
         * at every boot before first use, and during error recovery).
         * Exactly which visible bits a clear touches is open in the
         * datasheet; the one latched error this model has is the one it
         * clears. Every other latch this register names still does not
         * exist here -- the widget status register always answers zero
         * because nothing ever raises those errors -- so the rest of the
         * write is logged rather than acted on. Real firmware writes here
         * as part of its normal error-handling path (measured, three
         * writes per boot), not as a guest asking for unbuilt behaviour.
         */
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
        break;
    case HUB_NI_STATUS:
        /*
         * Firmware assigns node numbers and chooses the region size through
         * this register, so those fields are writable; the link state and the
         * chip's identity are what the hardware reports and are not.
         */
        s->ni_status = (s->ni_status & ~NI_STATUS_WRITABLE) |
                       (val & NI_STATUS_WRITABLE);
        break;
    case HUB_NI_SCRATCH_0:
        s->ni_scratch[0] = val;
        sgi_hub_kick_remote_scratch_peer(s);
        break;
    case HUB_NI_SCRATCH_1:
        s->ni_scratch[1] = val;
        sgi_hub_kick_remote_scratch_peer(s);
        break;
    case HUB_NI_VECTOR:
        s->ni_vector = val;
        break;
    case HUB_NI_VECTOR_DATA:
        s->ni_vector_data = val;
        break;
    case HUB_NI_VECTOR_STATUS:
        if (val == 0) {
            s->ni_vector_status = 0;
        }
        break;
    case HUB_NI_RETURN_VECTOR:
        s->ni_return_vector = val;
        break;
    case HUB_NI_VECTOR_READ_DATA:
        s->ni_vector_read_data = val;
        break;
    case HUB_NI_VECTOR_CLEAR:
        s->ni_vector_status = 0;
        break;
    case HUB_NI_VECTOR_PARMS:
        sgi_hub_vector_execute(s, val);
        break;
    case HUB_NI_PORT_RESET:
        /*
         * The local-reset bit is tested on its own because the three things
         * that write this register set different companions alongside it, and
         * a model keyed to any one of their values would ignore the others.
         *
         * The other bits are accepted and dropped: they reset the inter-node
         * link, which this machine does not have, and send a warm reset to a
         * neighbour it does not have either.
         *
         * **Nothing was reset here before, and a guest could not tell.** The
         * write landed in the local window, matched no register, and was
         * logged as unimplemented — so firmware that resets itself and halts
         * carried on into the code that runs *after* the reset, on a machine
         * that had not been reset. It reaches the branch the firmware intends
         * to reach, in the one state the firmware guarantees it is never in
         * there.
         */
        if (val & NI_PORT_RESET_LOCAL) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        break;
    case HUB_PI_PROCESSOR_NUMBER:
        /*
         * Refused, and correctly so: real firmware's own headers document
         * writing this register as a debug convention aimed at something
         * watching the bus, not at the chip. The standalone firmware's Hub
         * header offers a macro that stores a fixed pattern here annotated
         * for a logic analyzer, and the firmware's own low-level header
         * pairs a store here with a comment pointing the reader at
         * register-transfer-level simulation logs to find it — this is a
         * pre-silicon capture mechanism, not a register the guest expects
         * to hold a value. IRIX's own PCI bridge and Hub I/O drivers use
         * the same register the same way, storing marker and diagnostic
         * values with nothing ever read back, because the register reports
         * the processor number and cannot report anything else. A real
         * guest doing this is following a documented vendor convention,
         * not misbehaving, so this logs as unimplemented rather than as a
         * guest error.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the processor-"
                      "number register 0x%" HWADDR_PRIx " (a logic-analyzer "
                      "convention on real firmware, not modelled)\n",
                      val, addr);
        break;
    case HUB_PI_INT_PEND0:
    case HUB_PI_INT_PEND1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    case HUB_MD_INDICATOR_0:
    case HUB_MD_INDICATOR_1:
    case HUB_MD_SN00_INDICATOR_0:
    case HUB_MD_SN00_INDICATOR_1:
        {
            /*
             * Write-only: the register carries no state for a guest to read
             * back, on real hardware or here, so nothing is stored. Every
             * write is traced unconditionally -- decoded when the value is
             * one of real firmware's own progress or failure codes,
             * flagged undecoded otherwise, never silent either way. A run
             * with the trace event armed and no output means no write
             * reached this register at all, not that one went unlogged.
             */
            unsigned int cpu = (addr == HUB_MD_INDICATOR_1 ||
                                addr == HUB_MD_SN00_INDICATOR_1);
            uint8_t code = val & 0xff;
            bool is_failure = false;
            const char *meaning = sgi_hub_led_decode(code, &is_failure);

            if (meaning) {
                trace_sgi_hub_led(cpu, code, is_failure ? "FAILURE" : "progress",
                                   meaning);
            } else {
                trace_sgi_hub_led(cpu, code, "undecoded",
                                   "not one of real firmware's progress or "
                                   "failure codes -- may be a guest OS write "
                                   "or the firmware's character-wait flash");
            }
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to unimplemented "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
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
        sgi_hub_write_internal(opaque, addr, val, size);
        return;
    }

    has_before = sgi_hub_diagnostic_value(s, addr, val, &before);
    sgi_hub_write_internal(opaque, addr, val, size);
    has_after = sgi_hub_diagnostic_value(s, addr, val, &after);
    device_diagnostic_record(s->diagnostic, "hub", "mmio-write", addr,
                             val, size, has_before && has_after,
                             before, after);
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

static void sgi_hub_reset_hold(Object *obj, ResetType type)
{
    SGIHubState *s = SGI_HUB(obj);
    unsigned int n;

    s->pi_region_present = ~0ULL;
    s->pi_calias_size = PI_CALIAS_SIZE_RESET;
    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        s->pi_cpu_enable[n] = n < s->cpus;
    }
    /*
     * The error-spool addresses are deliberately not cleared here. The
     * firmware writes a marker into processor B's before triggering a reset,
     * expressly so the restarted machine can read it back and know it is on
     * the second pass -- so a reset that clears it would make the firmware's
     * own design impossible, and our machine would restart forever. They start
     * at zero because the device is zeroed at construction, which is the
     * power-on value the datasheet records.
     */
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
    s->ni_status = NI_STATUS_RESET |
                   (s->network_link_up ? NI_STATUS_LINK_UP : 0) |
                   ((uint64_t)s->nasid << NI_STATUS_NODE_SHIFT);
    s->ni_scratch[0] = 0;
    s->ni_scratch[1] = 0;
    s->ni_vector = 0;
    s->ni_vector_data = 0;
    s->ni_vector_status = 0;
    s->ni_return_vector = 0;
    s->ni_vector_read_data = 0;
    memset(s->ni_meta_route, 0, sizeof(s->ni_meta_route));
    memset(s->ni_local_route, 0, sizeof(s->ni_local_route));

    /*
     * The crosstalk link comes up reporting itself up, and this is one of the
     * few places the model knowingly departs from a documented reset value.
     *
     * The register table resets the link state to zero and its own note says
     * zero means the link failed. Implementing that faithfully produces a
     * machine mainline Linux cannot use at all: it probes crosstalk by reading
     * this register and testing one bit, and if the bit is clear it returns
     * immediately — before it reads widget 0, before it identifies anything.
     * No retry, no timeout, no message. The result is no crossbar, so no
     * bridge, no PCI, no I/O controller, no console and no disk, with nothing
     * in the log saying why.
     *
     * The justification for departing is that the reset value describes a Hub
     * whose link has not been trained yet, and this machine's link is
     * permanently connected to something that is always present — there is no
     * interval during which "not yet up" is true of it. That departure is a
     * machine-contract decision and has been ratified as one; it is not this
     * file's to make, only to implement.
     *
     * OpenBSD never reads this register, so no boot of the guest we run can
     * show whether it is right. The test is the only thing watching it.
     */
    s->io_link_status = IO_LINK_RESET;
    s->io_widget_control = IO_WIDGET_CONTROL_RESET;
    s->io_xtalk_timeout = 0;
    s->io_xtalk_prescale = 0;
    s->io_scratch[0] = 0;
    s->io_scratch[1] = 0;

    /*
     * All twelve engine registers reset to zero. No source describes what
     * a chip reset does to an in-flight transfer, and this model never has
     * one in flight -- transfers complete inside the store that launches
     * them -- so zeroing the registers is the whole of the reset.
     */
    memset(s->bte_length, 0, sizeof(s->bte_length));
    memset(s->bte_source, 0, sizeof(s->bte_source));
    memset(s->bte_dest, 0, sizeof(s->bte_dest));
    memset(s->bte_control, 0, sizeof(s->bte_control));
    memset(s->bte_notify, 0, sizeof(s->bte_notify));
    memset(s->bte_interrupt, 0, sizeof(s->bte_interrupt));
    s->md_perf_cnt0 = 0;
    s->md_config = MD_CONFIG_RESET;
    s->md_onewire = MD_ONEWIRE_RESET;

    /*
     * Every entry resets to zero, which names widget 0 at its first slice in
     * memory space. That is not a disabled window — the table has nothing to
     * disable with — so all seven come out of reset pointing at whatever
     * widget 0 is, and nothing on this machine aims them anywhere else. Which
     * is why the firmware's redirection of widget 0 onto the last window writes
     * a value that entry already holds: reaching widget 0 through a big window
     * here is the reset state doing it, not the firmware.
     *
     * **The zero is load-bearing and not merely the documented reset**, for two
     * independent reasons. Satisfying one of them without the other is possible
     * and would look like a tidy-up, which is why both are written here.
     *
     * **First, a guest looking for a spare entry decides by reading the widget
     * field and calling zero free**, so an entry resetting to anything else
     * would be read as one already in use. A Hub that reset these to a
     * plausible non-zero value would hand a guest fewer large windows than the
     * machine has, with the shortage surfacing much later in whichever driver
     * failed to get a mapping.
     *
     * **Second, a zeroed entry spells widget 0, memory space, offset 0 — so the
     * window it steers reaches the crossbar, and that is one guest's only route
     * there.** OpenBSD redirects every widget-0 access onto the last big window
     * and never programs the entry itself, so the crossbar is reachable for it
     * only because these reset to zero *and the windows are enabled as a
     * result*. The real machine lands in the same place by the same accident:
     * its firmware zeroes all seven entries in a generic loop rather than
     * writing widget 0 anywhere on purpose.
     *
     * **A change that zeroed the entries but left the windows disabled would
     * satisfy the first reason and break the second, and nothing would say so.**
     * Linux reaches the crossbar through its small window and never through
     * this one, so every Linux test and every Linux boot would still pass. The
     * guard is the qtest asserting that a window naming widget 0 answers.
     */
    for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
        s->io_bwin[n] = 0;
        sgi_hub_bwin_follow(s, n);
    }

    sgi_hub_update_irq(s);
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

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_hub_ops, s,
                          "sgi-hub.regs", SGI_HUB_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    address_space_init(&s->bte_as, s->memory, "sgi-hub.bte");

    if (s->junk_uart_present) {
        /*
         * Placed as a subregion of s->regs itself, not mapped separately
         * beside it: this window is mapped twice by the machine, once at an
         * address resolving to the reading processor's own node and once
         * naming a node explicitly, and only a subregion of the Hub's own
         * register region answers both the way every other register here
         * already does. No interrupt line -- nothing on this machine drives
         * or reads one, and the port is polled, matching every other device
         * on this bus that has no interrupt to offer.
         */
        serial_mm_init(&s->regs, HUB_MD_JUNKUART_BASE,
                       HUB_MD_JUNKUART_REGSHIFT, NULL, HUB_MD_JUNKUART_CLOCK,
                       s->junk_uart_chr, DEVICE_BIG_ENDIAN);
    }

    s->i2c.diagnostic = s->diagnostic;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c), errp)) {
        return;
    }
    /*
     * Higher priority than sgi_hub_ops's own leaf region, so every access in
     * the chip's sixteen bytes reaches the real controller instead of the
     * switch statement below -- the switch has no case left for this range.
     */
    memory_region_add_subregion_overlap(
        &s->regs, HUB_MD_I2C_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->i2c), 0), 1);

    /*
     * The module system controller (MSC): a real board on the same I2C bus,
     * not something the Hub reaches through a window, so an ordinary
     * addressed slave on the chip's own bus is nearly the whole of the
     * wiring. It also needs to know how many processors' arbitration
     * signals to rotate through (this node's own populated count), and the
     * chardev behind its AUX serial port -- serial_hd(SN0_ELSC_CHARDEV), so it
     * does not contend with the two IOC3 rear ports on serial_hd(0) and (1) or
     * the synthesized-firmware console shortcut, which shares the IOC3's own
     * serial_hd(0). The firmware redirects its console to this port after a
     * failed power-on diagnostic; the IRIX console (console=d) is IOC3 serial A
     * on serial_hd(0), not this one. A rig that means to capture that
     * diagnostic stream has to drive this index.
     */
    {
        I2CSlave *elsc = i2c_slave_new(TYPE_SGI_ELSC, SGI_ELSC_ADDR);

        qdev_prop_set_uint32(DEVICE(elsc), "num-cpus", s->cpus);
        qdev_prop_set_uint8(DEVICE(elsc), "dip-switches",
                            s->elsc_dip_switches);
        qdev_prop_set_chr(DEVICE(elsc), "chardev",
                          serial_hd(SN0_ELSC_CHARDEV));
        s->elsc = SGI_ELSC(elsc);
        s->elsc->diagnostic = s->diagnostic;
        if (!i2c_slave_realize_and_unref(elsc, s->i2c.bus, errp)) {
            s->elsc = NULL;
            return;
        }
    }

    /*
     * The system controller's 2 KiB NVRAM answers eight consecutive I2C
     * addresses, with each address selecting one 256-byte page. Whether the
     * hardware is one part consuming all eight addresses or several smaller
     * parts is not established, and is invisible on the wire, so use the
     * existing one-page EEPROM model for each address.
     *
     * Page 7 is the controller-reserved page that the PROM reads before it
     * probes a console. Only its validity marker has a required value; zero
     * is a coherent, deliberately non-authoritative default for its password,
     * switches, configuration, module, self-test, and identity fields.
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

    for (n = 0; n < SGI_HUB_PENDING_BANKS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->bank_irq[0][n]);
    }
    s->md_onewire_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       sgi_hub_onewire_slot_done, s);

    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->rt_irq[n]);
        s->rt_line[n].hub = s;
        s->rt_line[n].index = n;
        s->rt_timer[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_hub_rt_expire,
                                      &s->rt_line[n]);
    }
    for (n = 0; n < SGI_HUB_PENDING_BANKS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->bank_irq[1][n]);
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
}

static int sgi_hub_post_load(void *opaque, int version_id);

static const VMStateDescription vmstate_sgi_hub = {
    .name = "sgi-hub",
    .version_id = 18,
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
        VMSTATE_UINT8_ARRAY_V(ni_meta_route, SGIHubState,
                             SGI_HUB_META_ROUTES, 16),
        VMSTATE_UINT8_ARRAY_V(ni_local_route, SGIHubState,
                             SGI_HUB_LOCAL_ROUTES, 16),
        VMSTATE_UINT64_V(io_link_status, SGIHubState, 3),
        VMSTATE_UINT64_V(io_widget_control, SGIHubState, 4),
        VMSTATE_UINT64_V(io_xtalk_timeout, SGIHubState, 4),
        VMSTATE_UINT64_V(io_xtalk_prescale, SGIHubState, 4),
        /*
         * Version-gated rather than unconditional, so that a stream written
         * before this table existed still loads: its windows come up where
         * reset put them, which is where a machine that had no table had them.
         */
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
        VMSTATE_UINT64_V(pi_rt_local_ctrl, SGIHubState, 10),
        /*
         * The block transfer engines. The registers are the whole of an
         * engine's state -- a transfer completes inside the store that
         * launches it, so there is never an in-flight transfer to carry,
         * and nothing is derived for post_load to re-derive.
         */
        VMSTATE_UINT64_ARRAY_V(bte_length, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_source, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_dest, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_control, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_notify, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_UINT64_ARRAY_V(bte_interrupt, SGIHubState, SGI_HUB_BTES, 17),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The windows are aliases whose offsets are not part of the migration stream,
 * so they have to be pointed where the restored entries say once those have
 * arrived.
 */
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
    DEFINE_PROP_UINT8("elsc-dip-switches", SGIHubState,
                      elsc_dip_switches, 0),
    DEFINE_PROP_UINT32("cpu-base", SGIHubState, cpu_base, 0),
    DEFINE_PROP_UINT32("manufacturer", SGIHubState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("xtalk-widget-shift", SGIHubState,
                       xtalk_widget_shift, 0),
    DEFINE_PROP_LINK("xtalk", SGIHubState, xtalk, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("memory", SGIHubState, memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("junk-uart", SGIHubState, junk_uart_present, false),
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
    dc->vmsd = &vmstate_sgi_hub;
    device_class_set_props(dc, sgi_hub_properties);
    rc->phases.hold = sgi_hub_reset_hold;
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
