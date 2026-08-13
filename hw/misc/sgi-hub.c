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
#include "qemu/log.h"
#include "hw/misc/sgi-hub.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "chardev/char.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
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
 * Memory and directory interface. Both guests we boot read memory size
 * through the synthesized configuration graph rather than this unit's
 * registers directly, which is why only these two exist here: real firmware's
 * own diagnostics are the first guest that writes them.
 */
#define HUB_MD_INDICATOR_0      0x220050  /* real firmware's diagnostic LED, CPU A */
#define HUB_MD_INDICATOR_1      0x220058  /* same, CPU B */

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
 * Every other field samples something this board does not have — a second
 * board's master/slave sense, a bus-fault line, a jumper identity and a
 * processor-speed strap — so they read clear, and no test asserts them. The
 * register table declares no reset value for any field in either variant, which
 * is what a read-only status register sampling external signals looks like, so
 * what the absent signals read is this machine's decision rather than a
 * transcribed one.
 */
#define HUB_MD_SLOT_ID_USTAT    0x220048
#define MD_SLOT_FLASH_READY_LOW  (1ULL << 0)
#define MD_SLOT_FLASH_READY_HIGH (1ULL << 4)
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
 * The status register's fields. Only three of them matter to a booting guest,
 * and all three read zero out of reset: the machine is in M mode, this Hub is
 * node zero, and no network link has come up because a single-node machine has
 * none. The region-size bit resets set, selecting one region per node.
 */
#define NI_STATUS_MORE_NODES    (1ULL << 18)
#define NI_STATUS_REGION_SIZE   (1ULL << 17)
#define NI_STATUS_NODE_SHIFT    8
#define NI_STATUS_NODE_MASK     (0x1ffULL << NI_STATUS_NODE_SHIFT)
#define NI_STATUS_REVISION      (2ULL << 4)

#define NI_STATUS_RESET         (NI_STATUS_REGION_SIZE | NI_STATUS_REVISION)
#define NI_STATUS_WRITABLE      (NI_STATUS_MORE_NODES | \
                                 NI_STATUS_REGION_SIZE | \
                                 NI_STATUS_NODE_MASK)

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
 * it
 * has to run on every change to a pending bit or a mask. A model that raised a
 * line when a bit was set, rather than recomputing, would leave it asserted
 * after software cleared the last bit it could see — which a guest experiences
 * as an interrupt storm rather than as a lost interrupt.
 *
 * One processor for now, so the masks consulted are processor A's. A second
 * processor gets its own pair of lines from its own masks over the same pending
 * banks.
 */
static void sgi_hub_update_irq(SGIHubState *s)
{
    unsigned int bank;

    unsigned int cpu;

    for (bank = 0; bank < SGI_HUB_PENDING_BANKS; bank++) {
        qemu_set_irq(s->bank_irq[bank],
                     !!(s->pi_int_pend[bank] & s->pi_int_mask[bank]));
    }
    for (cpu = 0; cpu < SGI_HUB_PROCESSORS; cpu++) {
        qemu_set_irq(s->rt_irq[cpu], s->pi_rt_pending[cpu] &&
                                     s->pi_rt_enable[cpu]);
    }
}

/*
 * Set or clear one pending bit. The number's low six bits are the bit and its
 * seventh selects the bank, which is what makes numbers 64 through 127 the
 * second bank rather than a separate register.
 */
static void sgi_hub_int_modify(SGIHubState *s, uint64_t val)
{
    unsigned int number = val & PI_INT_MODIFY_NUMBER;
    unsigned int bank = number / SGI_HUB_BANK_BITS;
    uint64_t bit = 1ULL << (number % SGI_HUB_BANK_BITS);

    if (val & PI_INT_MODIFY_SET) {
        s->pi_int_pend[bank] |= bit;
    } else {
        s->pi_int_pend[bank] &= ~bit;
    }
    sgi_hub_update_irq(s);
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

/* Move the origin so the counter reads what was written, and keep running. */
static void sgi_hub_rt_set(SGIHubState *s, uint64_t val)
{
    val &= PI_RT_MASK;
    s->pi_rt_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                         muldiv64(val, NANOSECONDS_PER_SECOND, PI_RT_HZ);
}

/*
 * Arm one comparator against the counter as it stands now. The distance is
 * taken forward and wrapping, so a comparator left behind the counter waits a
 * full revolution rather than firing at once.
 *
 * The distance is one to 2^52 rather than zero to 2^52 - 1, so a comparator
 * that already equals the counter waits a revolution too. No manual states
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
 * itself — so the engineers who had the part appear to have believed an
 * already-reached value does not fire again until wraparound. That is an
 * inference from a design decision rather than a documented statement, which
 * is why it is written here and not asserted in the datasheet, but it agrees
 * with the choice this code had already made for a different reason: of two
 * readings we cannot separate, take the one that cannot produce a zero-length
 * interval, because an interval of no time is a timer that fires forever.
 */
static void sgi_hub_rt_arm(SGIHubState *s, unsigned int index)
{
    uint64_t ticks = ((s->pi_rt_compare[index] - sgi_hub_rt_count(s) - 1) &
                      PI_RT_MASK) + 1;

    timer_mod(s->rt_timer[index], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(ticks, NANOSECONDS_PER_SECOND, PI_RT_HZ));
}

/*
 * A match. Set the pending bit and recompute the line; the enable bit decides
 * whether that reaches the processor.
 *
 * Deliberately not re-armed here. A comparator would match again one full
 * revolution later, which at 1.25 MHz over 52 bits is longer than any run, and
 * re-arming at the match point risks a zero-length interval that would fire
 * forever. The guests that use the comparator rewrite it from every handler.
 */
static void sgi_hub_rt_expire(void *opaque)
{
    SGIHubCounterLine *line = opaque;

    line->hub->pi_rt_pending[line->index] = 1;
    sgi_hub_update_irq(line->hub);
}

static uint64_t sgi_hub_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIHubState *s = opaque;
    unsigned int n;

    if (sgi_hub_bwin_entry(addr, &n)) {
        return s->io_bwin[n];
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

    switch (addr) {
    case HUB_PI_RT_COUNT:
        return sgi_hub_rt_count(s);
    case HUB_PI_RT_COMPARE_A: return s->pi_rt_compare[0];
    case HUB_PI_RT_COMPARE_B: return s->pi_rt_compare[1];
    case HUB_PI_RT_PENDING_A: return s->pi_rt_pending[0];
    case HUB_PI_RT_PENDING_B: return s->pi_rt_pending[1];
    case HUB_PI_RT_ENABLE_A:  return s->pi_rt_enable[0];
    case HUB_PI_RT_ENABLE_B:  return s->pi_rt_enable[1];
    case HUB_PI_RT_LOCAL_CTRL: return s->pi_rt_local_ctrl;
    case HUB_PI_CPU_PRESENT_A:
        return s->cpus >= 1;
    case HUB_PI_CPU_PRESENT_B:
        return s->cpus >= 2;
    case HUB_PI_CPU_ENABLE_A:
        return s->pi_cpu_enable[0];
    case HUB_PI_CPU_ENABLE_B:
        return s->pi_cpu_enable[1];
    case HUB_PI_REGION_PRESENT:
        return s->pi_region_present;
    case HUB_PI_PROCESSOR_NUMBER:
        /*
         * Which processor of the pair is reading. One processor per node for
         * now, so always the first; when a second exists this has to answer
         * differently depending on which one asked, which the register window
         * alone cannot tell.
         */
        return 0;
    case HUB_PI_CALIAS_SIZE:
        return s->pi_calias_size;
    case HUB_PI_INT_PEND0:
        return s->pi_int_pend[0];
    case HUB_PI_INT_PEND1:
        return s->pi_int_pend[1];
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
    case HUB_MD_SLOT_ID_USTAT:
        return MD_SLOT_FLASH_READY;
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

static void sgi_hub_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    SGIHubState *s = opaque;
    unsigned int n;

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
        sgi_hub_rt_arm(s, 0);
        sgi_hub_rt_arm(s, 1);
        break;
    case HUB_PI_RT_COMPARE_A:
    case HUB_PI_RT_COMPARE_B: {
        unsigned int i = (addr == HUB_PI_RT_COMPARE_B);

        s->pi_rt_compare[i] = val & PI_RT_MASK;
        sgi_hub_rt_arm(s, i);
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
    case HUB_PI_CPU_ENABLE_A:
        s->pi_cpu_enable[0] = val & 1;
        break;
    case HUB_PI_CPU_ENABLE_B:
        s->pi_cpu_enable[1] = val & 1;
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
        break;
    case HUB_PI_INT_MASK1_B:
        s->pi_int_mask[3] = val;
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
         * A write programs the timing and starts a bit slot. Nothing drives
         * the wire, so the slot never finishes: the completion bit goes clear
         * and stays clear, and firmware polling for it waits forever. That is
         * the intended behaviour until this bus has a master and a part, and
         * it is deliberately louder than the alternative.
         */
        s->md_onewire = val & MD_ONEWIRE_WRITABLE;
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
         * latch, including the widget status register's unrecoverable-state
         * bit. This model has no error latches to clear -- the widget status
         * read above always answers zero because nothing here ever raises an
         * error in the first place -- so there is nothing this write could
         * change. Real firmware writes here as part of its normal error-
         * handling path (measured this session, three writes per boot), not
         * as a guest asking for something we should be doing and are not.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-hub: write of 0x%" PRIx64 " to the error clear "
                      "register 0x%" HWADDR_PRIx " (nothing here ever "
                      "latches an error to clear)\n", val, addr);
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
            unsigned int cpu = (addr == HUB_MD_INDICATOR_1);
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
    memset(s->pi_cpu_enable, 0, sizeof(s->pi_cpu_enable));
    s->pi_rt_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(s->pi_rt_compare, 0, sizeof(s->pi_rt_compare));
    memset(s->pi_rt_pending, 0, sizeof(s->pi_rt_pending));
    memset(s->pi_rt_enable, 0, sizeof(s->pi_rt_enable));
    s->pi_rt_local_ctrl = PI_RT_LOCAL_CTRL_RESET;
    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        timer_del(s->rt_timer[n]);
    }
    memset(s->pi_int_pend, 0, sizeof(s->pi_int_pend));
    memset(s->pi_int_mask, 0, sizeof(s->pi_int_mask));
    s->ni_status = NI_STATUS_RESET |
                   ((uint64_t)s->nasid << NI_STATUS_NODE_SHIFT);

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

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_hub_ops, s,
                          "sgi-hub.regs", SGI_HUB_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);

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
     * The entry-level system controller: a real board on the same I2C bus,
     * not something the Hub reaches through a window, so an ordinary
     * addressed slave on the chip's own bus is nearly the whole of the
     * wiring. It also needs to know how many processors' arbitration
     * signals to rotate through (this node's own populated count), and the
     * chardev behind its AUX serial port -- the second -serial device, so
     * it does not contend with the IOC3's own first port or the
     * synthesized-firmware console shortcut, both of which claim
     * serial_hd(0).
     */
    {
        I2CSlave *elsc = i2c_slave_new(TYPE_SGI_ELSC, SGI_ELSC_ADDR);

        qdev_prop_set_uint32(DEVICE(elsc), "num-cpus", s->cpus);
        qdev_prop_set_chr(DEVICE(elsc), "chardev", serial_hd(1));
        if (!i2c_slave_realize_and_unref(elsc, s->i2c.bus, errp)) {
            return;
        }
    }

    for (n = 0; n < SGI_HUB_PENDING_BANKS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->bank_irq[n]);
    }
    for (n = 0; n < SGI_HUB_PROCESSORS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->rt_irq[n]);
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
}

static int sgi_hub_post_load(void *opaque, int version_id);

static const VMStateDescription vmstate_sgi_hub = {
    .name = "sgi-hub",
    .version_id = 11,
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
        VMSTATE_UINT64_ARRAY(pi_int_mask, SGIHubState, 4),
        VMSTATE_UINT64(ni_status, SGIHubState),
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
        VMSTATE_UINT64_V(pi_rt_local_ctrl, SGIHubState, 10),
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

    for (n = 0; n < SGI_HUB_BWIN_ENTRIES; n++) {
        sgi_hub_bwin_follow(s, n);
    }
    sgi_hub_update_irq(s);
    return 0;
}

static const Property sgi_hub_properties[] = {
    DEFINE_PROP_UINT32("nasid", SGIHubState, nasid, 0),
    DEFINE_PROP_UINT32("cpus", SGIHubState, cpus, 1),
    DEFINE_PROP_UINT32("manufacturer", SGIHubState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("xtalk-widget-shift", SGIHubState,
                       xtalk_widget_shift, 0),
    DEFINE_PROP_LINK("xtalk", SGIHubState, xtalk, TYPE_MEMORY_REGION,
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
