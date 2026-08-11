/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC — the registers a guest reads before it has a console.
 *
 * The Hub is four units behind one 8 MB register window: a processor interface,
 * a memory and directory controller, an I/O interface and a network interface.
 * This model implements the registers an operating system touches on its way to
 * switching its console away from the firmware, which is a small and specific
 * set, and reports anything else rather than answering it. The rest arrives
 * with the interrupt path and the crossbar.
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
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

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
#define IO_XTALK_TIMEOUT_MASK   0xffULL
#define IO_XTALK_PRESCALE_MASK  0xffffffULL

/*
 * I/O interface: the link to the crossbar, and what this Hub says about it.
 */
#define HUB_IO_LINK_STATUS      0x400128

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

#define IO_LINK_RESET           (IO_LINK_MAX_BURST | IO_LINK_MAX_RETRY | \
                                 IO_LINK_STATE_UP | IO_LINK_NULL_TIMEOUT)

/*
 * What a guest may change. The burst, the retry count and the null-packet
 * timeout are configuration; the warm-reset trigger is a one-shot the model
 * accepts and does nothing with. Everything the hardware reports about itself —
 * the link state, the eight-bit mode and the link-enabled bit — is not
 * writable,
 * because a guest that could write the link state could tell this machine its
 * own link was down.
 */
#define IO_LINK_WARM_RESET      (1ULL << 8)
#define IO_LINK_WRITABLE        (IO_LINK_MAX_BURST | IO_LINK_MAX_RETRY | \
                                 IO_LINK_WARM_RESET | 0x3fULL)

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

    for (bank = 0; bank < SGI_HUB_PENDING_BANKS; bank++) {
        qemu_set_irq(s->bank_irq[bank],
                     !!(s->pi_int_pend[bank] & s->pi_int_mask[bank]));
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

static uint64_t sgi_hub_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIHubState *s = opaque;
    unsigned int n;

    if (sgi_hub_bwin_entry(addr, &n)) {
        return s->io_bwin[n];
    }

    switch (addr) {
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
    case HUB_IO_LINK_STATUS:
        return s->io_link_status;
    case HUB_NI_STATUS:
        return s->ni_status;
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

    switch (addr) {
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
    case HUB_IO_WIDGET_ID:
    case HUB_IO_WIDGET_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only widget "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    case HUB_IO_LINK_STATUS:
        s->io_link_status = (s->io_link_status & ~IO_LINK_WRITABLE) |
                            (val & IO_LINK_WRITABLE);
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
    case HUB_PI_PROCESSOR_NUMBER:
    case HUB_PI_INT_PEND0:
    case HUB_PI_INT_PEND1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-hub: write of 0x%" PRIx64 " to the read-only "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
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

    /*
     * Every entry resets to zero, which names widget 0 at its first slice in
     * memory space. That is not a disabled window — the table has nothing to
     * disable with — so all seven come out of reset pointing at whatever
     * widget 0 is, and nothing on this machine aims them anywhere else. Which
     * is why the firmware's redirection of widget 0 onto the last window writes
     * a value that entry already holds: reaching widget 0 through a big window
     * here is the reset state doing it, not the firmware.
     *
     * **The zero is load-bearing and not merely the documented reset.** A guest
     * looking for a spare entry decides by reading the widget field and calling
     * zero free, so an entry resetting to anything else would be read as one
     * already in use. A Hub that reset these to a plausible non-zero value
     * would
     * hand a guest fewer large windows than the machine has, with the shortage
     * surfacing much later in whichever driver failed to get a mapping.
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

    for (n = 0; n < SGI_HUB_PENDING_BANKS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->bank_irq[n]);
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
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = sgi_hub_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(pi_region_present, SGIHubState),
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
    DEFINE_PROP_UINT32("manufacturer", SGIHubState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("xtalk-widget-shift", SGIHubState,
                       xtalk_widget_shift, 0),
    DEFINE_PROP_LINK("xtalk", SGIHubState, xtalk, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

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
        .class_init = sgi_hub_class_init,
    },
};

DEFINE_TYPES(sgi_hub_types)
