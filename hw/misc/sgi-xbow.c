/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow XIO crossbar.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * Nine ports around one crossbar: eight external links carrying decoded
 * widget identities 8 through 15, and the ninth terminating at logical
 * widget zero, where these registers answer. The switch translates no
 * address and reorders no byte. A packet's destination widget selects the
 * output port and its offset travels unchanged. Widget zero and the eight
 * links are the only destinations a packet may name; any other number is
 * an error reported on the port that received it.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/*
 * Eight external ports decode widgets 8-15; port nine terminates at widget 0.
 * Destination widget selects the output port, with offset and byte order
 * unchanged. Other destinations signal an error on the ingress port.
 */

/* Registers are 32 bits in the low half of an eight-byte slot: offset + 4. */
#define XBOW_REG_DATA               4
#define XBOW_SLOT_STRIDE            8

#define XBOW_IDENTIFICATION         0x0000
#define XBOW_STATUS                 0x0008
#define XBOW_ERR_UPPER              0x0010
#define XBOW_ERR_LOWER              0x0018
#define XBOW_CONTROL                0x0020
#define XBOW_PACKET_TIMEOUT         0x0028
#define XBOW_INT_UPPER              0x0030
#define XBOW_INT_LOWER              0x0038
#define XBOW_ERR_CMD                0x0040
#define XBOW_LLP_CONTROL            0x0048
#define XBOW_STATUS_CLEAR           0x0050
#define XBOW_ARB_RELOAD             0x0058
#define XBOW_PERF_A                 0x0060
#define XBOW_PERF_B                 0x0068
#define XBOW_NIC                    0x0070

#define XBOW_LINK_BASE              0x0100
#define XBOW_LINK_STRIDE            0x0040
#define XBOW_LINK_FLUSH             0x00
#define XBOW_LINK_CONTROL           0x08
#define XBOW_LINK_STATUS            0x10
#define XBOW_LINK_ARB_UPPER         0x18
#define XBOW_LINK_ARB_LOWER         0x20
#define XBOW_LINK_STATUS_CLEAR      0x28
#define XBOW_LINK_RESET             0x30
#define XBOW_LINK_AUX_STATUS        0x38
#define XBOW_LINK_END               (XBOW_LINK_BASE + \
                                     SGI_XBOW_LINKS * XBOW_LINK_STRIDE)

#define XBOW_ID_REVISION_SHIFT      28
#define XBOW_ID_PART_SHIFT          12
#define XBOW_ID_MANUFACTURER_SHIFT  1
#define XBOW_ID_ONE                 1

/* Error command word: which widget was addressed, and which port sent it. */
#define XBOW_ERR_CMD_DIDN_SHIFT     28
#define XBOW_ERR_CMD_SIDN_SHIFT     24

/* Widget 0 status: the link requests, this widget's request, then errors. */
#define XBOW_STATUS_LINK_REQUEST(link) BIT(24 + (link))
#define XBOW_STATUS_LINK_REQUESTS   0xff000000u
#define XBOW_STATUS_REQUEST         BIT(23)
#define XBOW_STATUS_ACCESS_ERROR    BIT(5)
#define XBOW_STATUS_XTALK_ERROR     BIT(2)
#define XBOW_STATUS_MULTIPLE_ERROR  BIT(0)
#define XBOW_STATUS_WIDGET0_ERRORS  (XBOW_STATUS_ACCESS_ERROR | \
                                     XBOW_STATUS_XTALK_ERROR | \
                                     XBOW_STATUS_MULTIPLE_ERROR)
#define XBOW_STATUS_ASSERTING       (XBOW_STATUS_LINK_REQUESTS | \
                                     XBOW_STATUS_REQUEST)

/*
 * Control carries the register-access and Crosstalk error enables at the bits
 * their conditions occupy, and the connection-timeout enable at bit 1. This
 * revision gives that third enable no status location. Its own status
 * register calls bit 1 reserved, so the bit stores and enables nothing
 * until evidence settles where the condition would be reported.
 */
#define XBOW_CONTROL_MASK           0x00000026
#define XBOW_CONTROL_CONDITIONS     (XBOW_STATUS_ACCESS_ERROR | \
                                     XBOW_STATUS_XTALK_ERROR)

#define XBOW_PACKET_TIMEOUT_MASK    0x000fffff
#define XBOW_INT_UPPER_MASK         0xff07ffff
#define XBOW_INT_UPPER_TARGET_HIGH  BIT(19)
#define XBOW_INT_UPPER_TARGET_SHIFT 16
#define XBOW_INT_UPPER_VECTOR_SHIFT 24
#define XBOW_INT_UPPER_ADDRESS      0xffffu
#define XBOW_INTERRUPT_ASSERT       BIT(8)
#define XBOW_LLP_CONTROL_MASK       0x03ffffff
#define XBOW_LLP_CONTROL_RESET      0x03ff1810
#define XBOW_ARB_RELOAD_MASK        0x0000003f
#define XBOW_ARB_RELOAD_RESET       0x00000002
#define XBOW_PERF_SELECT_MASK       0x00f00000
#define XBOW_PERF_RESET             0x00800000
#define XBOW_NIC_MASK               0x000fffff
#define XBOW_NIC_RESET              0x00000002

/* Link status, and which link control enable answers for each condition. */
#define XBOW_LINK_ALIVE             BIT(31)
#define XBOW_LINK_MULTIPLE_ERROR    BIT(18)
#define XBOW_LINK_ILLEGAL_DEST      BIT(17)
#define XBOW_LINK_BANDWIDTH_SOURCE  0x0000ff00u
#define XBOW_LINK_MAX_RETRY         BIT(5)
#define XBOW_LINK_STATUS_ERRORS     0x0007fffb
#define XBOW_LINK_ALIVE_ENABLE      BIT(31)
#define XBOW_LINK_BANDWIDTH_ENABLE  BIT(8)

/*
 * Every other enable sits at the bit its condition occupies: illegal
 * destination at 17, input overallocation at 16, the two counter overflows
 * and the three link-level errors at 7 down to 3, maximum request timeout at
 * 1 and source timeout at 0.
 */
#define XBOW_LINK_DIRECT_ENABLES    0x000300fbu

/* Link control: the force-bad-packet one-shot at 23 reads back zero. */
#define XBOW_LINK_FORCE_BAD         BIT(23)
#define XBOW_LINK_EIGHT_BIT_FORCE   BIT(24)
#define XBOW_LINK_CONTROL_MASK      (BIT(31) | (3u << 28) | (7u << 25) | \
                                     BIT(24) | (0x1fu << 18) | \
                                     BIT(17) | BIT(16) | 0x1ffu)
#define XBOW_LINK_CREDIT_RESET      (2u << 18)

/* Link auxiliary status: attachment, negotiated width, failure mode. */
#define XBOW_AUX_FAILURE_MODE       BIT(6)
#define XBOW_AUX_PRESENT            BIT(5)
#define XBOW_AUX_EIGHT_BIT          BIT(4)
#define XBOW_AUX_ERRORS             0xffffff00u

/* Every widget mask names widgets 8 through 15 and nothing below. */
#define XBOW_WIDGET_MASK            0xff00u
#define XBOW_TIMER_INACTIVE         -1

/* Widget zero and the links are legal destinations; 1 through 7 are not. */
#define XBOW_FIRST_ILLEGAL_WIDGET   1
#define XBOW_ILLEGAL_WIDGETS        (SGI_XBOW_FIRST_WIDGET - \
                                     XBOW_FIRST_ILLEGAL_WIDGET)

/*
 * Two Crossbow core cycles of link reset, at the nominal 100 MHz core the
 * specification derives from the transmit clock. This is the pulse width, not
 * a bound on how long link initialization takes.
 */
#define XBOW_LINK_RESET_NS          20

static unsigned int sgi_xbow_widget(unsigned int link)
{
    return link + SGI_XBOW_FIRST_WIDGET;
}

/* A widget mask as the descriptor spells it, in this device's link order. */
static uint8_t sgi_xbow_links_of(uint16_t widgets)
{
    return widgets >> SGI_XBOW_FIRST_WIDGET;
}

static uint16_t sgi_xbow_widgets_of(uint8_t links)
{
    return (uint16_t)links << SGI_XBOW_FIRST_WIDGET;
}

static bool sgi_xbow_present(const SGIXbowState *s, unsigned int link)
{
    return s->link_present & BIT(link);
}

static bool sgi_xbow_alive(const SGIXbowState *s, unsigned int link)
{
    return s->link_alive & BIT(link);
}

/*
 * A link carries traffic while alive, and also through a reset pulse the
 * machine's restore policy will end alive: its routes in both directions and
 * the Crossbow's own interrupt packets to it. The pulse is two core cycles on
 * hardware but ends on a QEMU timer, which the main loop can run milliseconds
 * of guest time late under multi-threaded TCG. IRIX resets the SI graphics
 * link, waits a fixed millisecond without reading link status and programs
 * the board; withdrawing the route for longer than that turned the wait into
 * a bus error. Only the status shows the pulse.
 */
static bool sgi_xbow_carries(const SGIXbowState *s, unsigned int link)
{
    return sgi_xbow_alive(s, link) || (s->link_reset_restore_alive & BIT(link));
}

static uint32_t sgi_xbow_identification(const SGIXbowState *s)
{
    return (s->revision << XBOW_ID_REVISION_SHIFT) |
           (s->part << XBOW_ID_PART_SHIFT) |
           (s->manufacturer << XBOW_ID_MANUFACTURER_SHIFT) |
           XBOW_ID_ONE;
}

/*
 * The four port states the specification sets out. A port under
 * initialization shows none of the three bits; one that is up shows alive;
 * one that failed, whether before coming up or after, shows maximum retry.
 */
static uint32_t sgi_xbow_link_state(const SGIXbowState *s, unsigned int link)
{
    if (sgi_xbow_alive(s, link)) {
        return XBOW_LINK_ALIVE;
    }
    if (s->link_initializing & BIT(link)) {
        return 0;
    }
    return XBOW_LINK_MAX_RETRY;
}

static uint32_t sgi_xbow_link_status(const SGIXbowState *s, unsigned int link)
{
    return s->link_status[link] | sgi_xbow_link_state(s, link);
}

/*
 * Attachment is reported whether or not the link ever trained. Failure mode
 * separates a port that never came out of reset from one that came up and
 * then failed, and says nothing while the port is alive or initializing.
 * Width is what was negotiated when the port came up, not what the force
 * control says now: software changes that control and then resets the port
 * into the mode, so reading it live would report a width no link agreed.
 */
static uint32_t sgi_xbow_link_aux(const SGIXbowState *s, unsigned int link)
{
    uint32_t value = s->link_aux_status[link] & XBOW_AUX_ERRORS;

    if (sgi_xbow_present(s, link)) {
        value |= XBOW_AUX_PRESENT;
    }
    if (sgi_xbow_alive(s, link)) {
        if (s->link_eight_bit & BIT(link)) {
            value |= XBOW_AUX_EIGHT_BIT;
        }
    } else if (!(s->link_initializing & BIT(link)) &&
               !(s->link_came_up & BIT(link))) {
        value |= XBOW_AUX_FAILURE_MODE;
    }
    return value;
}

/*
 * A link asks for an interrupt while one of the conditions it has seen is
 * still enabled. Every enable but one sits at the bit its condition occupies;
 * the bandwidth enable answers for the whole source field. The conditions
 * this model never produces are link-level micropacket errors, because it
 * moves whole packets; their enables are wired the same way, so masking
 * behaves alike for all of them.
 */
static bool sgi_xbow_link_requesting(const SGIXbowState *s, unsigned int link)
{
    uint32_t enables = s->link_control[link];
    uint32_t events = s->link_event[link];

    if (events & enables & (XBOW_LINK_ALIVE_ENABLE |
                            XBOW_LINK_DIRECT_ENABLES)) {
        return true;
    }
    return (enables & XBOW_LINK_BANDWIDTH_ENABLE) &&
           (events & XBOW_LINK_BANDWIDTH_SOURCE);
}

/* Widget zero asks while one of its own enabled errors stands. */
static bool sgi_xbow_widget0_requesting(const SGIXbowState *s)
{
    return s->status & s->control & XBOW_CONTROL_CONDITIONS;
}

/*
 * One doubleword write from widget zero to the programmed target, carrying
 * the vector in bits 7:0 and assertion in bit 8. The destination register
 * holds a 48-bit address of the target's own rather than an XIO offset,
 * so the packet goes into that widget's space and not through the routes an
 * XIO address selects.
 */
static bool sgi_xbow_deliver_at(SGIXbowState *s, uint32_t upper,
                                uint32_t lower, bool raise)
{
    unsigned int target = (upper >> XBOW_INT_UPPER_TARGET_SHIFT) & 0xf;
    uint64_t address = ((uint64_t)(upper & XBOW_INT_UPPER_ADDRESS) << 32) |
                       lower;
    uint64_t data = (upper >> XBOW_INT_UPPER_VECTOR_SHIFT) & 0xff;
    unsigned int link;

    if (target < SGI_XBOW_FIRST_WIDGET) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbow: interrupt target %u is not a link widget; "
                      "no packet sent\n", target);
        return false;
    }
    link = target - SGI_XBOW_FIRST_WIDGET;
    if (!s->face[link]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbow: interrupt targets widget %u, which has "
                      "nothing attached; no packet sent\n", target);
        return false;
    }
    address_space_stq_be(s->face_as[link], address,
                         data | (raise ? XBOW_INTERRUPT_ASSERT : 0),
                         MEMTXATTRS_UNSPECIFIED, NULL);
    return true;
}

/*
 * The same packet, not sent when the link it would leave by does not carry
 * traffic (sgi_xbow_carries()). A widget that is unreachable does not
 * receive, and nothing here invents a failure response for the sender.
 */
static bool sgi_xbow_deliver(SGIXbowState *s, bool raise)
{
    unsigned int target = (s->int_upper >> XBOW_INT_UPPER_TARGET_SHIFT) & 0xf;

    if (target >= SGI_XBOW_FIRST_WIDGET &&
        !sgi_xbow_carries(s, target - SGI_XBOW_FIRST_WIDGET)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbow: widget %u is not reachable, so its "
                      "interrupt packet goes nowhere\n", target);
        return false;
    }
    return sgi_xbow_deliver_at(s, s->int_upper, s->int_lower, raise);
}

/*
 * One assertion is outstanding at a time. Later conditions accumulate without
 * a further packet, and the deassertion follows only once every pending
 * request has been acknowledged.
 */
static void sgi_xbow_update_interrupt(SGIXbowState *s)
{
    unsigned int link;
    bool pending;

    if (s->quiesced) {
        return;
    }
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (sgi_xbow_link_requesting(s, link)) {
            s->status |= XBOW_STATUS_LINK_REQUEST(link);
        } else {
            s->status &= ~XBOW_STATUS_LINK_REQUEST(link);
        }
    }
    if (sgi_xbow_widget0_requesting(s)) {
        s->status |= XBOW_STATUS_REQUEST;
    }

    pending = s->status & XBOW_STATUS_ASSERTING;
    if (pending == s->interrupt_asserted) {
        return;
    }
    if (sgi_xbow_deliver(s, pending)) {
        s->interrupt_asserted = pending;
    }
}

/* A route needs an attached widget and cannot loop back to itself. */
static bool sgi_xbow_routed(const SGIXbowState *s, unsigned int port,
                            unsigned int link)
{
    return s->face[link] && port != link;
}

static void sgi_xbow_sync_routes(SGIXbowState *s)
{
    unsigned int port;
    unsigned int link;

    if (s->quiesced) {
        return;
    }
    memory_region_transaction_begin();
    for (port = 0; port < SGI_XBOW_LINKS; port++) {
        memory_region_set_enabled(&s->ingress[port],
                                  sgi_xbow_carries(s, port));
        memory_region_set_enabled(&s->packet_ingress[port],
                                  sgi_xbow_carries(s, port));
        for (link = 0; link < SGI_XBOW_LINKS; link++) {
            if (sgi_xbow_routed(s, port, link)) {
                memory_region_set_enabled(&s->route[port][link],
                                          sgi_xbow_carries(s, link));
                memory_region_set_enabled(&s->packet_route[port][link],
                                          sgi_xbow_carries(s, link));
            }
        }
    }
    memory_region_transaction_commit();
}

/*
 * The first-error latch holds the XIO command word of the packet that
 * failed and suppresses later logging until software writes it back. An
 * illegal-destination packet carries that command word, built from the
 * receiving port and the destination it named, so those errors fill the latch
 * below. A widget zero access error arrives instead as a processor access
 * through a memory region, which carries no command word to put there, and
 * filling the register with anything else would report a packet that never
 * existed, so that one condition alone still leaves the register and both
 * error address halves at zero and prints here on its first occurrence.
 */
static void sgi_xbow_log_uncaptured_packet(void)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-xbow: the failing packet is not recorded; the "
                  "first-error latch is not modeled\n");
}

static void sgi_xbow_record_access_error(SGIXbowState *s)
{
    if (s->status & XBOW_STATUS_ACCESS_ERROR) {
        s->status |= XBOW_STATUS_MULTIPLE_ERROR;
    } else {
        sgi_xbow_log_uncaptured_packet();
    }
    s->status |= XBOW_STATUS_ACCESS_ERROR;
    sgi_xbow_update_interrupt(s);
}

/*
 * A destination number that is neither widget zero nor a link decodes to no
 * port at all, so the error belongs to the port the packet arrived at rather
 * than to anything it was addressed to. A repeat on the same port while its
 * own status bit still stands reports the second there, and the packet goes
 * nowhere either way. A link that is merely down or unpopulated is a
 * different case: its number is legal and nothing here latches for it.
 *
 * The global capture registers are a single instance shared by the whole
 * switch, not one per port, so capture depends on whether any port's error
 * still stands there, not this one's own status bit: two ports can each take
 * their first illegal-destination error before either is acknowledged, and
 * only the one that arrived first may fill the registers. The command word's
 * DIDN is the illegal widget the packet named and its SIDN is the widget
 * attached to the port that sent it; the address registers hold the offset
 * the packet carried within that widget's own space. Splitting that offset
 * across the two halves the way the interrupt destination address's own
 * registers are documented to, bits 47:32 in the upper half and bits 31:0 in
 * the lower, is an inference by analogy: this pair's own layout is not itself
 * directly stated. Every offset this switch can ever see stays within 48
 * bits, so the inference holds exactly throughout that range regardless.
 */
static void sgi_xbow_record_illegal_destination(SGIXbowState *s,
                                                unsigned int port,
                                                unsigned int widget,
                                                uint64_t offset)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbow: widget %u addressed widget %u, which is not a "
                  "destination this switch has\n", sgi_xbow_widget(port),
                  widget);
    if (s->link_status[port] & XBOW_LINK_ILLEGAL_DEST) {
        s->link_status[port] |= XBOW_LINK_MULTIPLE_ERROR;
    }
    if (s->err_cmd == 0) {
        s->err_cmd = (widget << XBOW_ERR_CMD_DIDN_SHIFT) |
                     (sgi_xbow_widget(port) << XBOW_ERR_CMD_SIDN_SHIFT);
        s->err_upper = offset >> 32;
        s->err_lower = (uint32_t)offset;
    }
    s->link_status[port] |= XBOW_LINK_ILLEGAL_DEST;
    s->link_event[port] |= XBOW_LINK_ILLEGAL_DEST;
    sgi_xbow_update_interrupt(s);
}

static bool sgi_xbow_decode_link_register(hwaddr slot, unsigned int *link,
                                          hwaddr *reg)
{
    hwaddr relative;

    if (slot < XBOW_LINK_BASE || slot >= XBOW_LINK_END) {
        return false;
    }
    relative = slot - XBOW_LINK_BASE;
    *link = relative / XBOW_LINK_STRIDE;
    *reg = relative % XBOW_LINK_STRIDE;
    return true;
}

static bool sgi_xbow_accessible(hwaddr slot, bool is_write)
{
    unsigned int link;
    hwaddr reg;

    switch (slot) {
    case XBOW_IDENTIFICATION:
    case XBOW_STATUS:
    case XBOW_ERR_UPPER:
    case XBOW_ERR_LOWER:
    case XBOW_STATUS_CLEAR:
        return !is_write;
    case XBOW_CONTROL:
    case XBOW_PACKET_TIMEOUT:
    case XBOW_INT_UPPER:
    case XBOW_INT_LOWER:
    case XBOW_ERR_CMD:
    case XBOW_LLP_CONTROL:
    case XBOW_ARB_RELOAD:
    case XBOW_PERF_A:
    case XBOW_PERF_B:
    case XBOW_NIC:
        return true;
    default:
        break;
    }

    if (!sgi_xbow_decode_link_register(slot, &link, &reg)) {
        return false;
    }
    switch (reg) {
    case XBOW_LINK_FLUSH:
    case XBOW_LINK_STATUS:
    case XBOW_LINK_STATUS_CLEAR:
    case XBOW_LINK_AUX_STATUS:
        return !is_write;
    case XBOW_LINK_CONTROL:
    case XBOW_LINK_ARB_UPPER:
    case XBOW_LINK_ARB_LOWER:
        return true;
    case XBOW_LINK_RESET:
        /*
         * The specification's error list names a reserved address, a wrong
         * packet type and a write to a read-only or read-and-clear register.
         * It does not settle what a read of this write-only register does,
         * and the data such a read would answer is not specified either, so
         * this follows the sibling crossbar and reports the access rather
         * than inventing a value.
         */
        return is_write;
    default:
        return false;
    }
}

static bool sgi_xbow_decode(hwaddr addr, unsigned int size, hwaddr *slot)
{
    if (size != 4 || (addr % XBOW_SLOT_STRIDE) != XBOW_REG_DATA) {
        return false;
    }
    *slot = addr - XBOW_REG_DATA;
    return true;
}

/*
 * The specification allows a register access only by an XIO doubleword
 * packet, and the established host form of that is the 32-bit access to the
 * data half. Whether this machine's adapter turns an eight-byte processor
 * access into the same packet is not established, so the model refuses that
 * form rather than answering it. Refusing is an emulator bound, not a report
 * that the chip rejected a packet, so it raises no status flag. The forms the
 * specification does call errors, a reserved address, a write to a
 * read-only or read-and-clear register, an access by any other packet type --
 * set the register-access error the way it says they do.
 */
static MemTxResult sgi_xbow_refuse(SGIXbowState *s, hwaddr addr,
                                   unsigned int size, bool is_write)
{
    if (size == 8) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbow: eight-byte %s at 0x%" HWADDR_PRIx " has no "
                      "established XIO packet form; refusing rather "
                      "than answering\n", is_write ? "write" : "read", addr);
        return MEMTX_ERROR;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbow: invalid %u-byte %s at 0x%" HWADDR_PRIx "\n",
                  size, is_write ? "write" : "read", addr);
    sgi_xbow_record_access_error(s);
    return MEMTX_ERROR;
}

static void sgi_xbow_link_reset_expire(void *opaque)
{
    SGIXbowLink *context = opaque;
    SGIXbowState *s = context->xbow;
    uint8_t bit = BIT(context->link);

    if (s->quiesced || s->stop_on_link_reset) {
        return;
    }
    s->link_reset_remaining[context->link] = XBOW_TIMER_INACTIVE;
    s->link_reset_asserted &= ~bit;
    qemu_set_irq(s->link_reset[context->link], 0);
    if (s->link_reset_restore_alive_mask & bit) {
        s->link_initializing &= ~bit;
        if (s->link_reset_restore_alive & bit) {
            s->link_alive |= bit;
            s->link_came_up |= bit;
            s->link_event[context->link] |= XBOW_LINK_ALIVE;
            if (s->link_control[context->link] &
                XBOW_LINK_EIGHT_BIT_FORCE) {
                s->link_eight_bit |= bit;
            }
        }
        s->link_reset_restore_alive &= ~bit;
        sgi_xbow_sync_routes(s);
        sgi_xbow_update_interrupt(s);
    }
}

/*
 * End every link reset whose pulse is already over. The timer that ends a
 * pulse runs from the main loop, and a processor polling link status straight
 * after a reset can outpace it: the IP27 PROM gives a link 100 polls ten
 * microseconds apart to come back, and in stock-PROM boots of the Origin 200
 * GIGAchannel it ran out of them with the pulse's 20 ns long past and its
 * timer still waiting, then faulted on the widget. Any access to this chip's
 * own registers first ends the pulses the virtual clock says are over, so
 * what the guest sees there follows that clock, not host scheduling.
 *
 * Only register accesses catch up. An access routed straight through the
 * switch to a widget does not come here, so if nothing has read or written
 * the registers since the pulse ended and the timer is still late, it still
 * finds that widget's route disabled. The PROM polls link status before it
 * touches the widget, which is what makes this enough for it.
 */
static void sgi_xbow_link_reset_catch_up(SGIXbowState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned int link;

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        QEMUTimer *timer = s->link_reset_timer[link];

        if (timer_pending(timer) && timer_expire_time_ns(timer) <= now) {
            timer_del(timer);
            sgi_xbow_link_reset_expire(&s->link_context[link]);
        }
    }
}

/*
 * The write is a one-shot: link reset is asserted locally and driven at the
 * remote end for two core cycles, and mode arbitration is forced. The
 * force-eight-bit control survives it. Link training completion is unknown:
 * neither pulse end nor peer reset proves it, so the generic port remains in
 * initialization. A machine may restore an attached link's pre-command health
 * at pulse end under its compatibility policy.
 */
static void sgi_xbow_start_link_reset(SGIXbowState *s, unsigned int link)
{
    uint8_t bit = BIT(link);

    if (s->link_reset_restore_alive_mask & bit) {
        if (s->link_alive & bit) {
            s->link_reset_restore_alive |= bit;
        } else {
            s->link_reset_restore_alive &= ~bit;
        }
    }
    s->link_alive &= ~bit;
    s->link_eight_bit &= ~bit;
    /*
     * A port with nothing on it goes to initialization failure and stays
     * there, which the specification states outright, so its reset needs no
     * completion predicate. Only an attached port waits for the one this
     * model does not have. Nothing here acknowledges a sticky error or a
     * pending request: the clearing aliases are the only acknowledgment this
     * chip has.
     */
    if (sgi_xbow_present(s, link)) {
        s->link_initializing |= bit;
    } else {
        s->link_initializing &= ~bit;
        s->link_came_up &= ~bit;
    }
    sgi_xbow_sync_routes(s);
    sgi_xbow_update_interrupt(s);

    s->link_reset_asserted |= bit;
    qemu_set_irq(s->link_reset[link], 1);
    timer_mod(s->link_reset_timer[link],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + XBOW_LINK_RESET_NS);
    if (sgi_xbow_present(s, link) &&
        !(s->link_reset_restore_alive_mask & bit)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbow: widget %u link reset; no reviewed "
                      "completion predicate brings an attached link out of "
                      "initialization, so it stays there\n",
                      sgi_xbow_widget(link));
    }
}

static MemTxResult sgi_xbow_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned int size, MemTxAttrs attrs)
{
    SGIXbowState *s = opaque;
    unsigned int link;
    hwaddr slot;
    hwaddr reg;
    uint32_t value;

    if (s->quiesced) {
        return MEMTX_ERROR;
    }
    sgi_xbow_link_reset_catch_up(s);
    *data = 0;
    if (!sgi_xbow_decode(addr, size, &slot) ||
        !sgi_xbow_accessible(slot, false)) {
        return sgi_xbow_refuse(s, addr, size, false);
    }
    switch (slot) {
    case XBOW_IDENTIFICATION:
        value = sgi_xbow_identification(s);
        break;
    case XBOW_STATUS:
        value = s->status;
        break;
    case XBOW_ERR_UPPER:
        value = s->err_upper;
        break;
    case XBOW_ERR_LOWER:
        value = s->err_lower;
        break;
    case XBOW_CONTROL:
        value = s->control;
        break;
    case XBOW_PACKET_TIMEOUT:
        value = s->packet_timeout;
        break;
    case XBOW_INT_UPPER:
        value = s->int_upper;
        break;
    case XBOW_INT_LOWER:
        value = s->int_lower;
        break;
    case XBOW_ERR_CMD:
        value = s->err_cmd;
        break;
    case XBOW_LLP_CONTROL:
        value = s->llp_control;
        break;
    case XBOW_STATUS_CLEAR:
        /*
         * The alias returns the status and acknowledges widget zero's own
         * errors and its request. The eight link requests are each
         * acknowledged at their own link's alias, not here.
         */
        value = s->status;
        s->status &= ~(XBOW_STATUS_WIDGET0_ERRORS | XBOW_STATUS_REQUEST);
        sgi_xbow_update_interrupt(s);
        break;
    case XBOW_ARB_RELOAD:
        value = s->arb_reload;
        break;
    case XBOW_PERF_A:
        value = s->perf_a;
        break;
    case XBOW_PERF_B:
        value = s->perf_b;
        break;
    case XBOW_NIC:
        value = s->nic;
        break;
    default:
        g_assert(sgi_xbow_decode_link_register(slot, &link, &reg));
        switch (reg) {
        case XBOW_LINK_FLUSH:
            /* Nothing is queued, so a flush finishes before it is read. */
            value = 0;
            break;
        case XBOW_LINK_CONTROL:
            value = s->link_control[link];
            break;
        case XBOW_LINK_STATUS:
            value = sgi_xbow_link_status(s, link);
            break;
        case XBOW_LINK_ARB_UPPER:
            value = s->link_arb_upper[link];
            break;
        case XBOW_LINK_ARB_LOWER:
            value = s->link_arb_lower[link];
            break;
        case XBOW_LINK_STATUS_CLEAR:
            /*
             * Reading here returns what the plain register returned and
             * negates the error bits, the auxiliary error fields and this
             * link's request. Attachment and the state the port is in are not
             * errors, and reappear on the next read.
             */
            value = sgi_xbow_link_status(s, link);
            /*
             * The sticky error and retry-counter fields are the link-level
             * micropacket errors this model never produces, so they are zero
             * already and the clearing is only the contract's shape.
             */
            s->link_status[link] &= ~XBOW_LINK_STATUS_ERRORS;
            s->link_aux_status[link] &= ~XBOW_AUX_ERRORS;

            /* This is the acknowledgment the interrupt machine waits on. */
            s->link_event[link] = 0;
            sgi_xbow_update_interrupt(s);
            break;
        case XBOW_LINK_AUX_STATUS:
            value = sgi_xbow_link_aux(s, link);
            break;
        default:
            g_assert_not_reached();
        }
        break;
    }
    *data = value;
    return MEMTX_OK;
}

static MemTxResult sgi_xbow_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size, MemTxAttrs attrs)
{
    SGIXbowState *s = opaque;
    unsigned int link;
    hwaddr slot;
    hwaddr reg;

    if (s->quiesced) {
        return MEMTX_ERROR;
    }
    sgi_xbow_link_reset_catch_up(s);
    if (!sgi_xbow_decode(addr, size, &slot)) {
        return sgi_xbow_refuse(s, addr, size, true);
    }
    if (!sgi_xbow_accessible(slot, true)) {
        MemTxResult result = sgi_xbow_refuse(s, addr, size, true);

        if (s->complete_posted_error_address_writes &&
            (slot == XBOW_ERR_UPPER || slot == XBOW_ERR_LOWER)) {
            return MEMTX_OK;
        }
        return result;
    }
    switch (slot) {
    case XBOW_CONTROL:
        /*
         * Enabling a notification after its condition has already latched
         * raises the interrupt here. Whether the chip replays a latched
         * condition on a late enable is not settled by the inspected
         * specification; this is the reading that keeps one rule for the
         * enable rather than two.
         */
        s->control = value & XBOW_CONTROL_MASK;
        sgi_xbow_update_interrupt(s);
        return MEMTX_OK;
    case XBOW_PACKET_TIMEOUT:
        s->packet_timeout = value & XBOW_PACKET_TIMEOUT_MASK;
        return MEMTX_OK;
    case XBOW_INT_UPPER:
        s->int_upper = (value & XBOW_INT_UPPER_MASK) |
                       XBOW_INT_UPPER_TARGET_HIGH;
        return MEMTX_OK;
    case XBOW_INT_LOWER:
        s->int_lower = value;
        return MEMTX_OK;
    case XBOW_ERR_CMD:
        /* The write rearms error logging and discards its data. */
        s->err_cmd = 0;
        s->err_upper = 0;
        s->err_lower = 0;
        return MEMTX_OK;
    case XBOW_LLP_CONTROL:
        s->llp_control = value & XBOW_LLP_CONTROL_MASK;
        return MEMTX_OK;
    case XBOW_ARB_RELOAD:
        s->arb_reload = value & XBOW_ARB_RELOAD_MASK;
        return MEMTX_OK;
    case XBOW_PERF_A:
    case XBOW_PERF_B:
        /* The selector is kept; nothing counts, so the count stays zero. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbow: performance counting is not modeled\n");
        if (slot == XBOW_PERF_A) {
            s->perf_a = value & XBOW_PERF_SELECT_MASK;
        } else {
            s->perf_b = value & XBOW_PERF_SELECT_MASK;
        }
        return MEMTX_OK;
    case XBOW_NIC:
        s->nic = value & XBOW_NIC_MASK;
        return MEMTX_OK;
    default:
        break;
    }

    g_assert(sgi_xbow_decode_link_register(slot, &link, &reg));
    switch (reg) {
    case XBOW_LINK_CONTROL:
        if (value & XBOW_LINK_FORCE_BAD) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-xbow: the forced bad micropacket is not "
                          "modeled; the one-shot still reads back zero\n");
        }
        s->link_control[link] = value & XBOW_LINK_CONTROL_MASK &
                                ~XBOW_LINK_FORCE_BAD;
        sgi_xbow_update_interrupt(s);
        return MEMTX_OK;
    case XBOW_LINK_ARB_UPPER:
    case XBOW_LINK_ARB_LOWER:
        /*
         * Each register holds a guaranteed-bandwidth count and a remainder
         * weight for four source links. They are stored and nothing
         * arbitrates on them.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbow: link arbitration is not modeled\n");
        if (reg == XBOW_LINK_ARB_UPPER) {
            s->link_arb_upper[link] = value;
        } else {
            s->link_arb_lower[link] = value;
        }
        return MEMTX_OK;
    case XBOW_LINK_RESET:
        /*
         * A MemoryRegion write carries no posted-versus-response-requested
         * attribute, so this is the ordinary write the specification's
         * workaround names. The erratum where a response-requesting write
         * produces no response needs an adapter that keeps that bit; nothing
         * here fabricates a successful response.
         */
        if (s->stop_on_link_reset) {
            g_autofree char *path = object_get_canonical_path(OBJECT(s));

            assert(bql_locked());
            error_report("%s: unsupported link reset: widget %u, "
                         "write address 0x%" HWADDR_PRIx ", width %u",
                         path, sgi_xbow_widget(link), addr, size);
            qemu_system_vmstop_request_prepare();
            qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
            return MEMTX_ERROR;
        }
        sgi_xbow_start_link_reset(s, link);
        return MEMTX_OK;
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps sgi_xbow_ops = {
    .read_with_attrs = sgi_xbow_read,
    .write_with_attrs = sgi_xbow_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static unsigned int sgi_xbow_illegal_widget(const SGIXbowState *s, hwaddr addr)
{
    return XBOW_FIRST_ILLEGAL_WIDGET + addr / s->span;
}

/* The offset the packet carried within its (illegal) widget's own space. */
static uint64_t sgi_xbow_illegal_offset(const SGIXbowState *s, hwaddr addr)
{
    return addr % s->span;
}

/*
 * One region per port covers the whole illegal range, so the port a packet
 * entered by is what selects the status register that records it. Nothing is
 * delivered and no value is invented for a read.
 */
static MemTxResult sgi_xbow_illegal_read(void *opaque, hwaddr addr,
                                         uint64_t *data, unsigned int size,
                                         MemTxAttrs attrs)
{
    SGIXbowLink *context = opaque;
    SGIXbowState *s = context->xbow;

    *data = 0;
    sgi_xbow_record_illegal_destination(s, context->link,
                                        sgi_xbow_illegal_widget(s, addr),
                                        sgi_xbow_illegal_offset(s, addr));
    return MEMTX_ERROR;
}

static MemTxResult sgi_xbow_illegal_write(void *opaque, hwaddr addr,
                                          uint64_t value, unsigned int size,
                                          MemTxAttrs attrs)
{
    SGIXbowLink *context = opaque;
    SGIXbowState *s = context->xbow;

    sgi_xbow_record_illegal_destination(s, context->link,
                                        sgi_xbow_illegal_widget(s, addr),
                                        sgi_xbow_illegal_offset(s, addr));
    return MEMTX_ERROR;
}

/*
 * One report per host access, at every width and alignment. Two fields below
 * decide it: the implementation width, which covers anything a host can issue
 * in one go, and the implementation's unaligned flag, without which an access
 * crossing a natural boundary is broken into pieces. Narrowing either splits
 * one packet into two reports. Where an access ends does not matter, because
 * QEMU hands a region an access that starts inside it whole rather than
 * clipping it at the region's end: one running off the top of the range is
 * reported once, against the widget it began in, and the part beyond is
 * swallowed as the hole here swallowed it before.
 */
static const MemoryRegionOps sgi_xbow_illegal_ops = {
    .read_with_attrs = sgi_xbow_illegal_read,
    .write_with_attrs = sgi_xbow_illegal_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

/*
 * Build one directed route: what a packet entering on `port` and addressed to
 * `link` reaches, in both the processor and packet XIO views. The
 * destination `link`'s own face must already be set. sgi_xbow_realize()
 * builds every route this way as the Crossbow comes up, and
 * sgi_xbow_attach_endpoint_live() builds the routes into a widget attached
 * afterwards the same way, so the two paths cannot drift.
 */
static void sgi_xbow_install_route(SGIXbowState *s, unsigned int port,
                                   unsigned int link)
{
    g_autofree char *route = g_strdup_printf("sgi-xbow.widget%u.to-widget%u",
                                             sgi_xbow_widget(port),
                                             sgi_xbow_widget(link));
    g_autofree char *packet = g_strdup_printf("%s.packet", route);

    memory_region_init_alias(&s->route[port][link], OBJECT(s), route,
                             s->face[link], 0,
                             MIN(s->span, memory_region_size(s->face[link])));
    memory_region_add_subregion(&s->ingress[port],
                                (hwaddr)sgi_xbow_widget(link) * s->span,
                                &s->route[port][link]);
    memory_region_init_alias(&s->packet_route[port][link], OBJECT(s), packet,
                             s->packet_face[link], 0,
                             MIN(SGI_XBOW_PACKET_SIZE,
                                 memory_region_size(s->packet_face[link])));
    memory_region_add_subregion(&s->packet_ingress[port],
                                (hwaddr)sgi_xbow_widget(link) *
                                SGI_XBOW_PACKET_SIZE,
                                &s->packet_route[port][link]);
}

MemoryRegion *sgi_xbow_ingress(SGIXbowState *s, unsigned int widget)
{
    g_assert(widget >= SGI_XBOW_FIRST_WIDGET && widget < SGI_XBOW_WIDGETS);
    return &s->ingress[widget - SGI_XBOW_FIRST_WIDGET];
}

MemoryRegion *sgi_xbow_packet_ingress(SGIXbowState *s, unsigned int widget)
{
    g_assert(widget >= SGI_XBOW_FIRST_WIDGET && widget < SGI_XBOW_WIDGETS);
    return &s->packet_ingress[widget - SGI_XBOW_FIRST_WIDGET];
}

bool sgi_xbow_attach_endpoint(SGIXbowState *s, unsigned int widget,
                              MemoryRegion *cpu_face, MemoryRegion *packet_face,
                              Error **errp)
{
    unsigned int link;

    if (s->quiesced) {
        error_setg(errp, "crossbar is quiesced");
        return false;
    }
    if (DEVICE(s)->realized) {
        error_setg(errp, "crossbar endpoints must be attached "
                   "before realization");
        return false;
    }
    if (widget < SGI_XBOW_FIRST_WIDGET || widget >= SGI_XBOW_WIDGETS) {
        error_setg(errp, "crossbar widget %u is outside its link range",
                   widget);
        return false;
    }
    if (!cpu_face || !packet_face || !memory_region_size(cpu_face) ||
        !memory_region_size(packet_face)) {
        error_setg(errp, "crossbar endpoints require two initialized faces");
        return false;
    }
    link = widget - SGI_XBOW_FIRST_WIDGET;
    if (s->face[link] || s->packet_face[link]) {
        error_setg(errp, "crossbar widget %u already has a face attached",
                   widget);
        return false;
    }
    s->face[link] = cpu_face;
    s->packet_face[link] = packet_face;
    return true;
}

bool sgi_xbow_attach_widget(SGIXbowState *s, unsigned int widget,
                            MemoryRegion *face, Error **errp)
{
    return sgi_xbow_attach_endpoint(s, widget, face, face, errp);
}

uint16_t sgi_xbow_endpoint_widgets(const SGIXbowState *s)
{
    uint8_t links = 0;
    unsigned int link;

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (s->face[link]) {
            links |= BIT(link);
        }
    }
    return sgi_xbow_widgets_of(links);
}

bool sgi_xbow_mark_widget_alive(SGIXbowState *s, unsigned int widget,
                                bool restore_after_reset, Error **errp)
{
    unsigned int link;

    if (s->quiesced) {
        error_setg(errp, "crossbar is quiesced");
        return false;
    }
    if (!DEVICE(s)->realized) {
        error_setg(errp, "the crossbar takes its operational links from "
                         "startup-links-alive until it is realized");
        return false;
    }
    if (widget < SGI_XBOW_FIRST_WIDGET || widget >= SGI_XBOW_WIDGETS) {
        error_setg(errp, "crossbar widget %u is outside its link range",
                  widget);
        return false;
    }
    link = widget - SGI_XBOW_FIRST_WIDGET;
    if (!sgi_xbow_present(s, link)) {
        error_setg(errp, "crossbar widget %u is not present", widget);
        return false;
    }
    if (sgi_xbow_alive(s, link)) {
        error_setg(errp, "crossbar widget %u is already alive", widget);
        return false;
    }

    /*
     * qdev_machine_creation_done() (hw/core/machine.c) fires the
     * machine_init_done notifiers this call runs from and then
     * unconditionally runs the machine's own cold reset in the same breath,
     * before the guest's first instruction. That reset's
     * sgi_xbow_reset_enter() reseeds link_alive from link_startup, so a
     * widget only marked alive in link_alive would go back to not-alive
     * before anything ever observes it alive. Marking it in link_startup as
     * well makes every future reset, this first one and any later
     * guest-initiated one, reproduce the same alive state, the same as a
     * widget that came up before startup-links-alive was ever read.
     */
    s->link_startup |= BIT(link);
    /*
     * The same holds for a guest-initiated link reset: a link configured
     * alive at startup comes back from one only under the machine's
     * "link-reset-restore-alive" policy, so a widget brought up here follows
     * whichever policy the caller gives it.
     */
    if (restore_after_reset) {
        s->link_reset_restore_alive_mask |= BIT(link);
    }
    s->link_alive |= BIT(link);
    s->link_came_up |= BIT(link);
    s->link_event[link] |= XBOW_LINK_ALIVE;
    if (s->link_control[link] & XBOW_LINK_EIGHT_BIT_FORCE) {
        s->link_eight_bit |= BIT(link);
    } else {
        s->link_eight_bit &= ~BIT(link);
    }
    s->link_initializing &= ~BIT(link);
    sgi_xbow_sync_routes(s);
    sgi_xbow_update_interrupt(s);
    return true;
}

bool sgi_xbow_attach_endpoint_live(SGIXbowState *s, unsigned int widget,
                                   MemoryRegion *cpu_face,
                                   MemoryRegion *packet_face, Error **errp)
{
    g_autofree char *name = NULL;
    unsigned int link;
    unsigned int port;

    if (s->quiesced) {
        error_setg(errp, "crossbar is quiesced");
        return false;
    }
    if (!DEVICE(s)->realized) {
        error_setg(errp, "a live crossbar attachment needs the crossbar "
                   "realized; attach before realize otherwise");
        return false;
    }
    if (widget < SGI_XBOW_FIRST_WIDGET || widget >= SGI_XBOW_WIDGETS) {
        error_setg(errp, "crossbar widget %u is outside its link range",
                   widget);
        return false;
    }
    if (!cpu_face || !packet_face || !memory_region_size(cpu_face) ||
        !memory_region_size(packet_face)) {
        error_setg(errp, "crossbar endpoints require two initialized faces");
        return false;
    }
    link = widget - SGI_XBOW_FIRST_WIDGET;
    if (s->face[link] || s->packet_face[link]) {
        error_setg(errp, "crossbar widget %u already has a face attached",
                   widget);
        return false;
    }

    /*
     * Register the widget as a destination and build the routes into it every
     * other port needs to reach it, exactly as sgi_xbow_realize() does for a
     * widget attached before realize. The packet address space matches the
     * one realize builds too, so widget zero's own interrupt can target this
     * widget the same way. The caller brings the widget's own link alive next
     * (sgi_xbow_mark_widget_alive()), whose sgi_xbow_sync_routes() settles
     * these routes to the enabled state a live destination has.
     *
     * Attaching a widget is what makes its port report one present (auxiliary
     * status bit 5), independently of whether the link has trained yet, so
     * the port stops reading as an empty slot here.
     */
    s->link_present |= BIT(link);
    s->face[link] = cpu_face;
    s->packet_face[link] = packet_face;
    name = g_strdup_printf("sgi-xbow.widget%u.packets", widget);
    s->face_as[link] = g_new0(AddressSpace, 1);
    address_space_init(s->face_as[link], s->packet_face[link], name);

    memory_region_transaction_begin();
    for (port = 0; port < SGI_XBOW_LINKS; port++) {
        if (sgi_xbow_routed(s, port, link)) {
            sgi_xbow_install_route(s, port, link);
        }
    }
    memory_region_transaction_commit();
    return true;
}

static void sgi_xbow_get_links_alive(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    SGIXbowState *s = SGI_XBOW(obj);
    uint16_t value = sgi_xbow_widgets_of(s->link_alive);

    visit_type_uint16(v, name, &value, errp);
}

/*
 * The operational mask is what changes while the machine runs. A widget that
 * is not attached cannot be operational, and a link that comes up or goes
 * down here takes the same route and interrupt consequences it takes when the
 * registers move it.
 */
static void sgi_xbow_set_links_alive(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    SGIXbowState *s = SGI_XBOW(obj);
    uint16_t widgets;
    uint8_t value;
    uint8_t changed;
    unsigned int link;

    if (s->quiesced) {
        error_setg(errp, "crossbar is quiesced");
        return;
    }
    if (!visit_type_uint16(v, name, &widgets, errp)) {
        return;
    }
    if (widgets & ~XBOW_WIDGET_MASK) {
        error_setg(errp, "a crossbar link mask names widgets 8 through 15, "
                         "so only bits 15:8 may be set");
        return;
    }
    value = sgi_xbow_links_of(widgets);
    if (!DEVICE(obj)->realized) {
        error_setg(errp, "the crossbar takes its operational links from "
                         "startup-links-alive until it is realized");
        return;
    }
    if (value & ~s->link_present) {
        error_setg(errp, "an operational crossbar link must be populated");
        return;
    }

    changed = value ^ s->link_alive;
    s->link_alive = value;
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (!(changed & BIT(link))) {
            continue;
        }
        s->link_initializing &= ~BIT(link);
        if (sgi_xbow_alive(s, link)) {
            s->link_came_up |= BIT(link);
            s->link_event[link] |= XBOW_LINK_ALIVE;
            if (s->link_control[link] & XBOW_LINK_EIGHT_BIT_FORCE) {
                s->link_eight_bit |= BIT(link);
            } else {
                s->link_eight_bit &= ~BIT(link);
            }
        } else {
            s->link_event[link] |= XBOW_LINK_MAX_RETRY;
        }
    }
    sgi_xbow_sync_routes(s);
    sgi_xbow_update_interrupt(s);
}

/*
 * Reset installs the configured startup health. That is emulator policy: it
 * is not a claim about how long a real link takes to train or whether it
 * succeeds.
 *
 * An outstanding assertion is dropped here and no deassertion packet follows.
 * Both deassertions the contract describes follow a software action that
 * takes away the last standing request, acknowledging it, or masking the
 * source that raised it, and a reset is neither. Deferring one would not
 * help either. Nothing may leave this device during the enter phase, so the
 * packet would have to go out in the hold phase, and by then every object in
 * the reset group has run its enter phase: it would land in a target that has
 * cleared its interrupt state, leaving a widget coming out of reset holding a
 * vector no hardware could have put there.
 *
 * That argument holds because the target resets alongside this device. A
 * reset of the crossbar alone would leave a peer with a latched interrupt
 * nothing retracts, since a later condition sends a fresh assertion rather
 * than a deassertion. No machine does that today.
 */
static void sgi_xbow_reset_enter(Object *obj, ResetType type)
{
    SGIXbowState *s = SGI_XBOW(obj);
    unsigned int link;

    if (s->quiesced) {
        return;
    }
    s->status = 0;
    s->err_upper = 0;
    s->err_lower = 0;
    s->control = 0;
    s->packet_timeout = XBOW_PACKET_TIMEOUT_MASK;
    s->int_upper = XBOW_INT_UPPER_TARGET_HIGH;
    s->int_lower = 0;
    s->err_cmd = 0;
    s->llp_control = XBOW_LLP_CONTROL_RESET;
    s->arb_reload = XBOW_ARB_RELOAD_RESET;
    s->perf_a = XBOW_PERF_RESET;
    s->perf_b = XBOW_PERF_RESET;
    s->nic = XBOW_NIC_RESET;
    s->interrupt_asserted = false;
    s->link_reset_asserted = 0;
    s->link_reset_restore_alive = 0;
    s->link_initializing = 0;
    s->link_alive = s->link_startup;
    s->link_came_up = s->link_startup;
    s->link_eight_bit = 0;

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        timer_del(s->link_reset_timer[link]);
        s->link_reset_remaining[link] = XBOW_TIMER_INACTIVE;
        s->link_control[link] = XBOW_LINK_CREDIT_RESET;
        s->link_arb_upper[link] = 0;
        s->link_arb_lower[link] = 0;
        s->link_status[link] = 0;
        s->link_aux_status[link] = 0;
        s->link_event[link] = 0;
    }
}

static void sgi_xbow_reset_hold(Object *obj, ResetType type)
{
    SGIXbowState *s = SGI_XBOW(obj);
    unsigned int link;

    if (s->quiesced) {
        return;
    }
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        qemu_set_irq(s->link_reset[link], 0);
    }
}

static void sgi_xbow_reset_exit(Object *obj, ResetType type)
{
    sgi_xbow_sync_routes(SGI_XBOW(obj));
}

static void sgi_xbow_realize(DeviceState *dev, Error **errp)
{
    SGIXbowState *s = SGI_XBOW(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    unsigned int port;
    unsigned int link;

    if (s->quiesced) {
        error_setg(errp, "crossbar is quiesced");
        return;
    }
    if (!s->span || s->span > SGI_XBOW_PACKET_SIZE) {
        error_setg(errp, "the crossbar needs the size of one XIO "
                   "widget space, no larger than 48 address bits");
        return;
    }
    if ((s->links_present_prop | s->startup_links_alive_prop |
         s->link_reset_restore_alive_prop) &
        ~XBOW_WIDGET_MASK) {
        error_setg(errp, "a crossbar link mask names widgets 8 through 15, "
                         "so only bits 15:8 may be set");
        return;
    }
    if (s->startup_links_alive_prop & ~s->links_present_prop) {
        error_setg(errp, "a crossbar link that starts operational must also "
                         "be populated");
        return;
    }
    if (s->link_reset_restore_alive_prop & ~s->links_present_prop) {
        error_setg(errp, "a crossbar link with reset recovery must also be "
                         "populated");
        return;
    }
    if (s->revision > 0xf || s->part > 0xffff || s->manufacturer > 0x7ff) {
        error_setg(errp,
                   "crossbar identity fields exceed their register widths");
        return;
    }
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (s->packet_face[link] && !s->face[link]) {
            error_setg(errp, "crossbar packet endpoint requires a CPU face");
            return;
        }
        if (s->face[link] && !memory_region_size(s->face[link])) {
            error_setg(errp, "crossbar endpoint is not initialized");
            return;
        }
        if (!s->packet_face[link]) {
            s->packet_face[link] = s->face[link];
        }
    }
    s->link_present = sgi_xbow_links_of(s->links_present_prop);
    s->link_startup = sgi_xbow_links_of(s->startup_links_alive_prop);
    s->link_reset_restore_alive_mask =
        sgi_xbow_links_of(s->link_reset_restore_alive_prop);

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_xbow_ops, s,
                          "sgi-xbow.regs", s->span);

    for (port = 0; port < SGI_XBOW_LINKS; port++) {
        g_autofree char *view =
            g_strdup_printf("sgi-xbow.widget%u.registers",
                            sgi_xbow_widget(port));
        g_autofree char *missing =
            g_strdup_printf("sgi-xbow.widget%u.illegal-destinations",
                            sgi_xbow_widget(port));

        /* The processor view is exactly sixteen of the machine's spans. */
        memory_region_set_size(&s->ingress[port], SGI_XBOW_WIDGETS * s->span);
        memory_region_init_alias(&s->regs_view[port], OBJECT(dev), view,
                                 &s->regs, 0, s->span);
        memory_region_add_subregion(&s->ingress[port], 0, &s->regs_view[port]);
        memory_region_init_io(&s->illegal[port], OBJECT(dev),
                              &sgi_xbow_illegal_ops, &s->link_context[port],
                              missing, XBOW_ILLEGAL_WIDGETS * s->span);
        memory_region_add_subregion(&s->ingress[port],
                                    (hwaddr)XBOW_FIRST_ILLEGAL_WIDGET *
                                    s->span, &s->illegal[port]);
        sysbus_init_mmio(sbd, &s->ingress[port]);
        {
            g_autofree char *regs = g_strdup_printf("%s.packet", view);

            memory_region_init_alias(&s->packet_regs_view[port], OBJECT(dev),
                                     regs, &s->regs, 0, s->span);
            memory_region_add_subregion(&s->packet_ingress[port], 0,
                                        &s->packet_regs_view[port]);
        }

        for (link = 0; link < SGI_XBOW_LINKS; link++) {
            if (!sgi_xbow_routed(s, port, link)) {
                continue;
            }
            sgi_xbow_install_route(s, port, link);
        }
    }

    for (port = 0; port < SGI_XBOW_LINKS; port++) {
        sysbus_init_mmio(sbd, &s->packet_ingress[port]);
    }

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        g_autofree char *name = NULL;

        if (!s->face[link]) {
            continue;
        }
        name = g_strdup_printf("sgi-xbow.widget%u.packets",
                               sgi_xbow_widget(link));
        s->face_as[link] = g_new0(AddressSpace, 1);
        address_space_init(s->face_as[link], s->packet_face[link], name);
    }
    s->regions_initialized = true;
}

void sgi_xbow_quiesce(SGIXbowState *s)
{
    unsigned int link;

    assert(bql_locked());
    if (s->quiesced) {
        return;
    }
    s->quiesced = true;
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        timer_del(s->link_reset_timer[link]);
        s->link_reset_remaining[link] = XBOW_TIMER_INACTIVE;
    }
    if (s->regions_initialized) {
        memory_region_transaction_begin();
        memory_region_set_enabled(&s->regs, false);
        for (link = 0; link < SGI_XBOW_LINKS; link++) {
            memory_region_set_enabled(&s->ingress[link], false);
            memory_region_set_enabled(&s->packet_ingress[link], false);
        }
        memory_region_transaction_commit();
    }
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (s->link_reset_asserted & BIT(link)) {
            qemu_set_irq(s->link_reset[link], 0);
        }
    }
    s->link_reset_asserted = 0;
    s->link_reset_restore_alive = 0;
}

static void sgi_xbow_unrealize(DeviceState *dev)
{
    SGIXbowState *s = SGI_XBOW(dev);
    unsigned int link;

    sgi_xbow_quiesce(s);
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (s->face_as[link]) {
            address_space_destroy_free(s->face_as[link]);
            s->face_as[link] = NULL;
        }
    }
}

/*
 * Migration carries the live mask, not the startup one. An active reset also
 * carries its remaining pulse duration and the health it may restore. The
 * routes and outputs derived from that state are rebuilt here.
 */
static int sgi_xbow_pre_save(void *opaque)
{
    SGIXbowState *s = opaque;
    unsigned int link;

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        int64_t deadline = timer_expire_time_ns(s->link_reset_timer[link]);

        if (timer_pending(s->link_reset_timer[link])) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            if (deadline < 0) {
                return -EINVAL;
            }
            s->link_reset_remaining[link] = MAX(deadline - now, 0);
        } else {
            s->link_reset_remaining[link] = XBOW_TIMER_INACTIVE;
        }
    }
    return 0;
}

static int sgi_xbow_pre_load(void *opaque)
{
    SGIXbowState *s = opaque;
    unsigned int link;

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        timer_del(s->link_reset_timer[link]);
        s->link_reset_remaining[link] = XBOW_TIMER_INACTIVE;
    }
    return s->quiesced ? -EINVAL : 0;
}

static int sgi_xbow_post_load(void *opaque, int version_id)
{
    SGIXbowState *s = opaque;
    unsigned int link;

    if (s->quiesced || (s->link_alive & ~s->link_present) ||
        (s->link_reset_restore_alive & ~s->link_reset_restore_alive_mask)) {
        return -EINVAL;
    }
    /*
     * Replaying the level re-drives the edge at the far end, and the
     * endpoints act on the rising edge. Nothing is connected to these lines
     * yet, but once a machine wires them, a migration taken inside the
     * two-cycle pulse will reset that endpoint a second time. Whoever
     * connects them owns the choice: replay the level as here, or restore it
     * without re-driving an edge the destination already had.
     */
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (s->link_reset_remaining[link] < XBOW_TIMER_INACTIVE) {
            return -EINVAL;
        }
        if (!!(s->link_reset_asserted & BIT(link)) !=
            (s->link_reset_remaining[link] != XBOW_TIMER_INACTIVE)) {
            return -EINVAL;
        }
        if (s->stop_on_link_reset &&
            ((s->link_reset_asserted & BIT(link)) ||
             s->link_reset_remaining[link] != XBOW_TIMER_INACTIVE)) {
            return -EINVAL;
        }
    }
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        qemu_set_irq(s->link_reset[link],
                     !!(s->link_reset_asserted & BIT(link)));
    }
    sgi_xbow_sync_routes(s);
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        if (s->link_reset_remaining[link] != XBOW_TIMER_INACTIVE) {
            timer_mod_ns(s->link_reset_timer[link],
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         s->link_reset_remaining[link]);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_xbow = {
    .name = "sgi-xbow",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = sgi_xbow_pre_save,
    .pre_load = sgi_xbow_pre_load,
    .post_load = sgi_xbow_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(status, SGIXbowState),
        VMSTATE_UINT32(err_upper, SGIXbowState),
        VMSTATE_UINT32(err_lower, SGIXbowState),
        VMSTATE_UINT32(control, SGIXbowState),
        VMSTATE_UINT32(packet_timeout, SGIXbowState),
        VMSTATE_UINT32(int_upper, SGIXbowState),
        VMSTATE_UINT32(int_lower, SGIXbowState),
        VMSTATE_UINT32(err_cmd, SGIXbowState),
        VMSTATE_UINT32(llp_control, SGIXbowState),
        VMSTATE_UINT32(arb_reload, SGIXbowState),
        VMSTATE_UINT32(perf_a, SGIXbowState),
        VMSTATE_UINT32(perf_b, SGIXbowState),
        VMSTATE_UINT32(nic, SGIXbowState),
        VMSTATE_UINT32_ARRAY(link_control, SGIXbowState, SGI_XBOW_LINKS),
        VMSTATE_UINT32_ARRAY(link_status, SGIXbowState, SGI_XBOW_LINKS),
        VMSTATE_UINT32_ARRAY(link_arb_upper, SGIXbowState, SGI_XBOW_LINKS),
        VMSTATE_UINT32_ARRAY(link_arb_lower, SGIXbowState, SGI_XBOW_LINKS),
        VMSTATE_UINT32_ARRAY(link_aux_status, SGIXbowState, SGI_XBOW_LINKS),
        VMSTATE_UINT32_ARRAY(link_event, SGIXbowState, SGI_XBOW_LINKS),
        VMSTATE_UINT8(link_alive, SGIXbowState),
        VMSTATE_UINT8(link_initializing, SGIXbowState),
        VMSTATE_UINT8(link_came_up, SGIXbowState),
        VMSTATE_UINT8(link_eight_bit, SGIXbowState),
        VMSTATE_UINT8(link_reset_asserted, SGIXbowState),
        VMSTATE_UINT8(link_reset_restore_alive, SGIXbowState),
        VMSTATE_BOOL(interrupt_asserted, SGIXbowState),
        VMSTATE_INT64_ARRAY(link_reset_remaining, SGIXbowState,
                            SGI_XBOW_LINKS),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * A real Crossbow reports part 0 and manufacturer 0: zero is the identity.
 * Link masks name widgets, so bit 8 is the widget at decoded identity 8,
 * matching the field the machine's generated descriptor carries.
 */
static const Property sgi_xbow_properties[] = {
    DEFINE_PROP_UINT64("span", SGIXbowState, span, 0),
    DEFINE_PROP_BOOL("stop-on-link-reset", SGIXbowState,
                     stop_on_link_reset, false),
    DEFINE_PROP_BOOL("complete-posted-error-address-writes", SGIXbowState,
                     complete_posted_error_address_writes, false),
    DEFINE_PROP_UINT32("revision", SGIXbowState, revision, 1),
    DEFINE_PROP_UINT32("part", SGIXbowState, part, 0),
    DEFINE_PROP_UINT32("manufacturer", SGIXbowState, manufacturer, 0),
    DEFINE_PROP_UINT16("links-present", SGIXbowState, links_present_prop, 0),
    DEFINE_PROP_UINT16("startup-links-alive", SGIXbowState,
                       startup_links_alive_prop, 0),
    DEFINE_PROP_UINT16("link-reset-restore-alive", SGIXbowState,
                       link_reset_restore_alive_prop, 0),
};

static void sgi_xbow_instance_init(Object *obj)
{
    SGIXbowState *s = SGI_XBOW(obj);
    unsigned int link;

    qdev_init_gpio_out_named(DEVICE(obj), s->link_reset, "link-reset",
                             SGI_XBOW_LINKS);
    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        g_autofree char *name =
            g_strdup_printf("target-%x", sgi_xbow_widget(link));
        g_autofree char *ingress =
            g_strdup_printf("sgi-xbow.widget%u.ingress",
                            sgi_xbow_widget(link));
        g_autofree char *packet = g_strdup_printf("%s.packet", ingress);

        /*
         * Both ingress containers exist from here, so a link can hand them
         * to the widget on this port before the Crossbow realizes; realize
         * fills them with the register view and the routes, and sizes the
         * processor container to the machine's span, which it does not know
         * yet.
         */
        memory_region_init(&s->ingress[link], obj, ingress,
                           SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);
        memory_region_init(&s->packet_ingress[link], obj, packet,
                           SGI_XBOW_WIDGETS * SGI_XBOW_PACKET_SIZE);

        s->link_context[link].xbow = s;
        s->link_context[link].link = link;
        s->link_reset_remaining[link] = XBOW_TIMER_INACTIVE;
        s->link_reset_timer[link] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                 sgi_xbow_link_reset_expire,
                                                 &s->link_context[link]);
        object_property_add_link(obj, name, TYPE_MEMORY_REGION,
                                 (Object **)&s->face[link],
                                 qdev_prop_allow_set_link_before_realize, 0);
    }
    object_property_add(obj, "links-alive", "uint16",
                        sgi_xbow_get_links_alive, sgi_xbow_set_links_alive,
                        NULL, NULL);
}

static void sgi_xbow_instance_finalize(Object *obj)
{
    SGIXbowState *s = SGI_XBOW(obj);
    unsigned int link;

    for (link = 0; link < SGI_XBOW_LINKS; link++) {
        timer_free(s->link_reset_timer[link]);
    }
}

/*
 * ---- The XIO port interface ----
 *
 * Each of the eight external links is one port, addressed by index the way
 * every other public accessor here addresses it: a widget number, 8 through
 * 15, not a link index. The ports are switched: their faces carry
 * the destination widget. Both faces exist from instance
 * init.
 */

static const char *sgi_xbow_xio_port_describe(Object *port,
                                              unsigned int index)
{
    return "a Crossbow port";
}

static bool sgi_xbow_xio_port_valid(unsigned int index)
{
    return index >= SGI_XBOW_FIRST_WIDGET && index < SGI_XBOW_WIDGETS;
}

static MemoryRegion *sgi_xbow_xio_port_cpu_face(Object *port,
                                                unsigned int index)
{
    return sgi_xbow_xio_port_valid(index) ?
        sgi_xbow_ingress(SGI_XBOW(port), index) : NULL;
}

static MemoryRegion *sgi_xbow_xio_port_packet_face(Object *port,
                                                   unsigned int index)
{
    return sgi_xbow_xio_port_valid(index) ?
        sgi_xbow_packet_ingress(SGI_XBOW(port), index) : NULL;
}

static bool sgi_xbow_xio_port_switched(Object *port, unsigned int index)
{
    return true;
}

/*
 * The widget on this port becomes the destination the routes into it
 * reach: before realize, as every widget present at startup does, or
 * live, for a board a machine fits once this Crossbow is up.
 */
static bool sgi_xbow_xio_port_connect(Object *port, unsigned int index,
                                      const SGIXioPeer *peer, Error **errp)
{
    SGIXbowState *s = SGI_XBOW(port);

    if (peer->switched) {
        error_setg(errp, "a Crossbow port links to a widget, not to "
                   "another crossbar");
        return false;
    }
    if (!DEVICE(s)->realized) {
        return sgi_xbow_attach_endpoint(s, index, peer->cpu_face,
                                        peer->packet_face, errp);
    }
    return sgi_xbow_attach_endpoint_live(s, index, peer->cpu_face,
                                         peer->packet_face, errp);
}

/* A port's per-link reset (the link reset register) reaches its widget. */
static void sgi_xbow_xio_port_reset_output(Object *port, unsigned int index,
                                           qemu_irq target)
{
    if (index < SGI_XBOW_FIRST_WIDGET || index >= SGI_XBOW_WIDGETS) {
        return;
    }
    qdev_connect_gpio_out_named(DEVICE(port), "link-reset",
                                index - SGI_XBOW_FIRST_WIDGET, target);
}

static void sgi_xbow_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(oc);

    dc->desc = "SGI Crossbow XIO crossbar";
    dc->realize = sgi_xbow_realize;
    dc->unrealize = sgi_xbow_unrealize;
    dc->vmsd = &vmstate_sgi_xbow;
    device_class_set_props(dc, sgi_xbow_properties);
    /* The qtest builds its own switch, like the sibling crossbar test. */
    dc->user_creatable = true;
    rc->phases.enter = sgi_xbow_reset_enter;
    rc->phases.hold = sgi_xbow_reset_hold;
    rc->phases.exit = sgi_xbow_reset_exit;
    pc->describe = sgi_xbow_xio_port_describe;
    pc->cpu_face = sgi_xbow_xio_port_cpu_face;
    pc->packet_face = sgi_xbow_xio_port_packet_face;
    pc->switched = sgi_xbow_xio_port_switched;
    pc->connect = sgi_xbow_xio_port_connect;
    pc->reset_output = sgi_xbow_xio_port_reset_output;
}

static const TypeInfo sgi_xbow_types[] = {
    {
        .name = TYPE_SGI_XBOW,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIXbowState),
        .instance_init = sgi_xbow_instance_init,
        .instance_finalize = sgi_xbow_instance_finalize,
        .class_init = sgi_xbow_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_XIO_PORT },
            { }
        },
    },
};

DEFINE_TYPES(sgi_xbow_types)
