/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * psitech-rad4: bounded functional approximation of a PsiTech RAD4 PCI
 * graphics head (vendor/device 12d1:0104), one primary head on an Origin
 * 200 Bridge PCI slot.
 *
 * This models a compatibility profile for one exact stock software path
 * (PsiTech release 1.3k kernel driver and standalone X server, pixel DMA
 * disabled, the driver's mode zero at 1280x1024), not recovered RAD4
 * hardware fidelity. Every approximated behavior below is named, evidenced
 * and independently reviewed in the owning Origami repository:
 * docs/datasheets/psitech-rad4.md and
 * docs/designs/rad4-one-mode-functional-approximation.md. Read those
 * documents before changing register policy here; a policy change needs
 * renewed independent review there before it lands here.
 *
 * The one-mode fixed timing sequence gating scanout comes from a reviewed,
 * generated fixture (psitech-rad4-mode0-fixture.h), not from vendor source
 * or a copied instruction listing.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

#include "psitech-rad4-mode0-fixture.h"

#define TYPE_PSITECH_RAD4 "psitech-rad4"
OBJECT_DECLARE_SIMPLE_TYPE(PsitechRad4State, PSITECH_RAD4)

#define RAD4_VENDOR_ID          0x12d1
#define RAD4_DEVICE_ID          0x0104

#define RAD4_BAR0_SIZE          128
#define RAD4_BAR2_SIZE          (16 * MiB)
#define RAD4_ROMROUTE_SIZE      (8 * MiB)

#define RAD4_MODE0_WIDTH        1280
#define RAD4_MODE0_HEIGHT       1024
#define RAD4_MODE0_STRIDE       (RAD4_MODE0_WIDTH * 4)
#define RAD4_FB_SIZE            (8 * MiB)

/* BAR0 offsets, PLX9060-compatible. */
#define RAD4_BAR0_REG14         0x14
#define RAD4_BAR0_REG18         0x18
#define RAD4_BAR0_MAILBOX       0x40
#define RAD4_BAR0_INTCSR        0x68
#define RAD4_BAR0_SERIAL        0x6c

#define RAD4_INTCSR_PENDING     (1u << 15)
#define RAD4_INTCSR_ENABLE      (1u << 8)
#define RAD4_INTCSR_WRITABLE    0xffff7fffu

#define RAD4_SERIAL_CLOCK       (1u << 24)
#define RAD4_SERIAL_CS          (1u << 25)
#define RAD4_SERIAL_MOSI        (1u << 26)
#define RAD4_SERIAL_MISO        (1u << 27)
#define RAD4_SERIAL_WRITABLE    0x0700000fu

/* Expansion-ROM route, revision-3-compatible DMA register aliases. */
#define RAD4_ROM_DESC           0x100
#define RAD4_ROM_MODE           0x110
#define RAD4_ROM_COMMAND        0x128

/* BAR2 apertures. */
#define RAD4_BAR2_FB_BASE       0x000000
#define RAD4_BAR2_RGB_LO        0x900000
#define RAD4_BAR2_RGB_HI        0x910000
#define RAD4_BAR2_RGB_DATA      0x920000
#define RAD4_BAR2_RGB_RESERVED  0x930000
#define RAD4_BAR2_HORIZ0        0xa00000
#define RAD4_BAR2_HORIZ1        0xa01000
#define RAD4_BAR2_HORIZ3        0xa03000
#define RAD4_BAR2_VERT          0xb00000
#define RAD4_BAR2_MASK          0xd00000
#define RAD4_BAR2_CTRL          0xf00000

#define RAD4_HORIZ_WORDS        422
#define RAD4_VERT_WORDS         1065

#define RAD4_CTRL_MODE_CMD      0x20u
#define RAD4_CTRL_STORED_BIT    0x02u
#define RAD4_STATUS_FRAME       0x20u

#define RAD4_FRAME_HZ           60

/* RGB561 index ranges this profile stores and treats as supported. */
typedef struct Rad4RgbRange {
    uint16_t first;
    uint16_t last;
} Rad4RgbRange;

static const Rad4RgbRange rad4_rgb_supported_ranges[] = {
    { 0x0001, 0x0009 },
    { 0x0010, 0x0013 },
    { 0x0020, 0x0022 },
    { 0x0030, 0x0039 },
    { 0x0040, 0x004e },
    { 0x0050, 0x0056 },
    { 0x005f, 0x005f },
    { 0x0082, 0x0082 },
    { 0x0a11, 0x0a12 },
    { 0x0a15, 0x0a15 },
    { 0x0e00, 0x0e0f },
    { 0x0f00, 0x0f0f },
    { 0x1000, 0x10ff },
    { 0x1400, 0x14ff },
    { 0x2000, 0x23ff },
    { 0x3000, 0x30ff },
    { 0x3400, 0x34ff },
    { 0x3800, 0x38ff },
    { 0x4000, 0x43ff },
};

/*
 * Indices that gate mode recognition: writing data here after recognition
 * invalidates the active mode instead of taking effect as an ordinary
 * runtime register update.
 */
static bool rad4_rgb_index_is_mode_mutating(uint16_t index)
{
    return index == 0x0020 || index == 0x0021 || index == 0x0022;
}

static bool rad4_rgb_index_supported(uint16_t index)
{
    for (size_t i = 0; i < ARRAY_SIZE(rad4_rgb_supported_ranges); i++) {
        if (index >= rad4_rgb_supported_ranges[i].first &&
            index <= rad4_rgb_supported_ranges[i].last) {
            return true;
        }
    }
    return false;
}

typedef enum Rad4SerialState {
    RAD4_SERIAL_IDLE,
    RAD4_SERIAL_COMMAND,
    RAD4_SERIAL_ACK,
    RAD4_SERIAL_PAYLOAD,
    RAD4_SERIAL_FAILED,
} Rad4SerialState;

/* Start bit (1) followed by command byte 0xac, MSB first, 9 bits total. */
#define RAD4_SERIAL_COMMAND_BITS 9
#define RAD4_SERIAL_PAYLOAD_BITS (40 * 8)
static const uint16_t rad4_serial_expected_command = 0x1ac;

typedef enum Rad4RecState {
    RAD4_REC_UNARMED,
    RAD4_REC_PREFIX_SEEN,
    RAD4_REC_COLLECTING,
    RAD4_REC_RECOGNIZED,
    RAD4_REC_FAILED,
} Rad4RecState;

struct PsitechRad4State {
    PCIDevice pci;

    MemoryRegion bar0;
    MemoryRegion bar2;
    MemoryRegion romroute;

    QemuConsole *con;
    QEMUTimer *frame_timer;

    /* BAR0 state. */
    uint32_t reg14;
    uint32_t reg18;
    uint32_t mailbox;
    uint32_t intcsr;
    uint32_t serial;

    int serial_state; /* Rad4SerialState, as plain int for VMState */
    bool serial_clk_prev;
    bool serial_cs_prev;
    uint32_t serial_bit_count;
    uint16_t serial_command_shift;
    bool serial_miso;

    /* Expansion-ROM route state. */
    uint32_t rom_desc;
    uint32_t rom_mode;
    uint32_t rom_command;

    /* BAR2 framebuffer and mask. */
    uint8_t *framebuffer;
    uint32_t mask;

    /* RGB561 indexed register file. */
    uint8_t rgb_index_low;
    uint8_t rgb_index_high;
    uint8_t *rgb_store;

    /*
     * Timing memories, stored for readback only; recognition is gated by
     * exact sequence matching against the reviewed fixture, not by
     * interpreting these values.
     */
    uint32_t vert_timing[RAD4_VERT_WORDS];
    uint32_t horiz_timing[3][RAD4_HORIZ_WORDS];

    /* BAR2 +0xf00000 control/status. */
    uint32_t bar2_ctrl_bit;
    bool frame_pending;

    /* Mode recognizer. */
    int rec_state; /* Rad4RecState, as plain int for VMState */
    uint32_t rec_index;
};

static uint16_t rad4_rgb_index(PsitechRad4State *s)
{
    return ((uint16_t)s->rgb_index_high << 8) | s->rgb_index_low;
}

static void rad4_update_irq(PsitechRad4State *s)
{
    bool level = (s->intcsr & RAD4_INTCSR_PENDING) &&
                 (s->intcsr & RAD4_INTCSR_ENABLE);

    pci_set_irq(&s->pci, level);
}

static void rad4_recognizer_reset(PsitechRad4State *s)
{
    s->rec_state = RAD4_REC_UNARMED;
    s->rec_index = 0;
}

static void rad4_disable_scanout(PsitechRad4State *s)
{
    if (s->rec_state == RAD4_REC_RECOGNIZED) {
        timer_del(s->frame_timer);
    }
}

static void rad4_frame_timer_cb(void *opaque)
{
    PsitechRad4State *s = opaque;

    if (s->rec_state != RAD4_REC_RECOGNIZED) {
        return;
    }

    s->frame_pending = true;
    s->intcsr |= RAD4_INTCSR_PENDING;
    rad4_update_irq(s);

    timer_mod(s->frame_timer,
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
             NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
}

/*
 * Feed one recognition-relevant (address, value) event through the exact
 * mode-zero grammar. Addresses are BAR2-relative. Returns true if the
 * event was consumed as part of that grammar (whether or not it advanced
 * recognition); the caller still applies the event's own register effect
 * separately, since recognition state and register storage are distinct.
 */
static void rad4_recognizer_feed(PsitechRad4State *s, uint32_t offset,
                                 uint32_t value)
{
    if (offset == RAD4_BAR2_CTRL) {
        if (value == 0) {
            /*
             * Arms/re-arms the sequence from any state, including an
             * already-recognized mode: a fresh mode-set invalidates the
             * active mode until the new sequence completes.
             */
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_PREFIX_SEEN;
            s->rec_index = 0;
            return;
        }
        if (s->rec_state == RAD4_REC_COLLECTING &&
            s->rec_index < RAD4_MODE0_EVENT_COUNT &&
            rad4_mode0_events[s->rec_index].bar2_offset == offset &&
            rad4_mode0_events[s->rec_index].value == value) {
            s->rec_index++;
            if (s->rec_index == RAD4_MODE0_EVENT_COUNT) {
                s->rec_state = RAD4_REC_RECOGNIZED;
                timer_mod(s->frame_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
            }
            return;
        }
        if (s->rec_state == RAD4_REC_COLLECTING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: mode recognition mismatch at "
                         "+0x%06x=0x%08x, failing until reset\n",
                         offset, value);
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_FAILED;
            return;
        }
        /*
         * An unrecognized control write in another state has no bearing
         * on recognition; the register-level effect is handled by the
         * caller.
         */
        return;
    }

    /* Every other recognition-relevant address. */
    switch (s->rec_state) {
    case RAD4_REC_PREFIX_SEEN:
        if (s->rec_index == 0 &&
            rad4_mode0_events[1].bar2_offset == offset &&
            rad4_mode0_events[1].value == value) {
            s->rec_state = RAD4_REC_COLLECTING;
            s->rec_index = 2;
            if (s->rec_index == RAD4_MODE0_EVENT_COUNT) {
                s->rec_state = RAD4_REC_RECOGNIZED;
            }
        } else {
            /*
             * Not the expected next event: quietly returns to unarmed,
             * no failure logged.
             */
            s->rec_state = RAD4_REC_UNARMED;
        }
        break;
    case RAD4_REC_COLLECTING:
        if (s->rec_index < RAD4_MODE0_EVENT_COUNT &&
            rad4_mode0_events[s->rec_index].bar2_offset == offset &&
            rad4_mode0_events[s->rec_index].value == value) {
            s->rec_index++;
            if (s->rec_index == RAD4_MODE0_EVENT_COUNT) {
                s->rec_state = RAD4_REC_RECOGNIZED;
                timer_mod(s->frame_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: mode recognition mismatch at "
                         "+0x%06x=0x%08x, failing until reset\n",
                         offset, value);
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_FAILED;
        }
        break;
    case RAD4_REC_UNARMED:
    case RAD4_REC_RECOGNIZED: {
        bool timing_bank = offset == RAD4_BAR2_VERT ||
                            offset == RAD4_BAR2_HORIZ0 ||
                            offset == RAD4_BAR2_HORIZ1 ||
                            offset == RAD4_BAR2_HORIZ3;
        bool mutating_rgb = offset == RAD4_BAR2_RGB_DATA &&
                             rad4_rgb_index_is_mode_mutating(
                                 rad4_rgb_index(s));

        if (timing_bank || mutating_rgb) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad4: timing/PLL/sync write while %s, "
                         "failing until reset\n",
                         s->rec_state == RAD4_REC_UNARMED ?
                         "unarmed" : "recognized");
            rad4_disable_scanout(s);
            s->rec_state = RAD4_REC_FAILED;
        }
        break;
    }
    case RAD4_REC_FAILED:
        break;
    }
}

static bool rad4_gfx_update(void *opaque)
{
    PsitechRad4State *s = opaque;

    if (s->rec_state != RAD4_REC_RECOGNIZED) {
        return true;
    }
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps rad4_gfx_ops = {
    .gfx_update = rad4_gfx_update,
};

static void rad4_ensure_surface(PsitechRad4State *s)
{
    DisplaySurface *ds = qemu_console_surface(s->con);

    if (ds && surface_data(ds) == s->framebuffer) {
        return;
    }
    ds = qemu_create_displaysurface_from(RAD4_MODE0_WIDTH, RAD4_MODE0_HEIGHT,
                                         PIXMAN_BE_x8r8g8b8,
                                         RAD4_MODE0_STRIDE, s->framebuffer);
    qemu_console_set_surface(s->con, ds);
}

static uint64_t rad4_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_BAR0_REG14:
        return s->reg14;
    case RAD4_BAR0_REG18:
        return s->reg18;
    case RAD4_BAR0_MAILBOX:
        return s->mailbox;
    case RAD4_BAR0_INTCSR:
        return s->intcsr;
    case RAD4_BAR0_SERIAL:
        return s->serial;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR0 read at 0x%02x\n",
                     (unsigned)addr);
        return 0;
    }
}

static void rad4_serial_reset(PsitechRad4State *s)
{
    s->serial_state = RAD4_SERIAL_IDLE;
    s->serial_bit_count = 0;
    s->serial_command_shift = 0;
    s->serial_miso = true;
}

/*
 * Bit-banged synthetic EEPROM: a start bit followed by command 0xac reads
 * back 40 zero bytes, matching the reviewed synthetic configuration. Any
 * other command, or chip select deasserted before the transaction
 * completes, latches an all-ones readback until cold reset.
 */
static void rad4_serial_edge(PsitechRad4State *s, bool cs, bool clk,
                             bool mosi)
{
    bool clk_rising = clk && !s->serial_clk_prev;
    bool clk_falling = !clk && s->serial_clk_prev;

    if (!s->serial_cs_prev && cs) {
        rad4_serial_reset(s);
        s->serial_state = RAD4_SERIAL_COMMAND;
    }
    if (s->serial_cs_prev && !cs) {
        if (s->serial_state != RAD4_SERIAL_IDLE &&
            s->serial_bit_count <
                (s->serial_state == RAD4_SERIAL_PAYLOAD ?
                 RAD4_SERIAL_PAYLOAD_BITS : 0)) {
            /* Premature deassertion mid-transaction. */
            s->serial_state = RAD4_SERIAL_FAILED;
            s->serial_miso = true;
        } else if (s->serial_state == RAD4_SERIAL_COMMAND ||
                  s->serial_state == RAD4_SERIAL_ACK) {
            s->serial_state = RAD4_SERIAL_FAILED;
            s->serial_miso = true;
        } else {
            s->serial_state = RAD4_SERIAL_IDLE;
        }
    }

    if (cs && clk_rising) {
        switch (s->serial_state) {
        case RAD4_SERIAL_COMMAND:
            s->serial_command_shift =
                (uint16_t)((s->serial_command_shift << 1) | (mosi ? 1 : 0));
            s->serial_bit_count++;
            if (s->serial_bit_count == RAD4_SERIAL_COMMAND_BITS) {
                if (s->serial_command_shift == rad4_serial_expected_command) {
                    s->serial_state = RAD4_SERIAL_ACK;
                } else {
                    s->serial_state = RAD4_SERIAL_FAILED;
                    s->serial_miso = true;
                }
                s->serial_bit_count = 0;
            }
            break;
        default:
            break;
        }
    }

    if (cs && clk_falling) {
        switch (s->serial_state) {
        case RAD4_SERIAL_ACK:
            s->serial_miso = false;
            s->serial_state = RAD4_SERIAL_PAYLOAD;
            s->serial_bit_count = 0;
            break;
        case RAD4_SERIAL_PAYLOAD:
            s->serial_miso = false;
            s->serial_bit_count++;
            break;
        default:
            break;
        }
    }

    s->serial_clk_prev = clk;
    s->serial_cs_prev = cs;
}

static void rad4_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_BAR0_REG14:
        s->reg14 = val;
        break;
    case RAD4_BAR0_REG18:
        s->reg18 = val;
        break;
    case RAD4_BAR0_MAILBOX:
        s->mailbox = val;
        break;
    case RAD4_BAR0_INTCSR:
        s->intcsr = (s->intcsr & RAD4_INTCSR_PENDING) |
                    (val & RAD4_INTCSR_WRITABLE & ~RAD4_INTCSR_PENDING);
        rad4_update_irq(s);
        break;
    case RAD4_BAR0_SERIAL: {
        uint32_t written = (val & RAD4_SERIAL_WRITABLE);
        bool cs = (written & RAD4_SERIAL_CS) != 0;
        bool clk = (written & RAD4_SERIAL_CLOCK) != 0;
        bool mosi = (written & RAD4_SERIAL_MOSI) != 0;

        rad4_serial_edge(s, cs, clk, mosi);
        s->serial = written | (s->serial_miso ? RAD4_SERIAL_MISO : 0);
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR0 write at 0x%02x\n",
                     (unsigned)addr);
        break;
    }
}

static const MemoryRegionOps rad4_bar0_ops = {
    .read = rad4_bar0_read,
    .write = rad4_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t rad4_romroute_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_ROM_DESC:
        return s->rom_desc;
    case RAD4_ROM_MODE:
        return s->rom_mode;
    case RAD4_ROM_COMMAND:
        return s->rom_command;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported ROM-route read at 0x%03x\n",
                     (unsigned)addr);
        return 0;
    }
}

static void rad4_romroute_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    PsitechRad4State *s = opaque;

    switch (addr) {
    case RAD4_ROM_DESC:
        s->rom_desc = val;
        break;
    case RAD4_ROM_MODE:
        s->rom_mode = val;
        break;
    case RAD4_ROM_COMMAND:
        s->rom_command = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported ROM-route write at 0x%03x\n",
                     (unsigned)addr);
        break;
    }
}

static const MemoryRegionOps rad4_romroute_ops = {
    .read = rad4_romroute_read,
    .write = rad4_romroute_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rad4_apply_mask_write(PsitechRad4State *s, uint32_t val)
{
    s->mask = val;
}

static uint64_t rad4_bar2_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4State *s = opaque;
    uint32_t offset = addr;

    if (offset < RAD4_FB_SIZE) {
        return ldl_be_p(s->framebuffer + offset);
    }
    if (offset >= RAD4_BAR2_VERT &&
        offset < RAD4_BAR2_VERT + 4 * RAD4_VERT_WORDS) {
        return s->vert_timing[(offset - RAD4_BAR2_VERT) / 4];
    }
    if (offset >= RAD4_BAR2_HORIZ0 &&
        offset < RAD4_BAR2_HORIZ0 + 4 * RAD4_HORIZ_WORDS) {
        return s->horiz_timing[0][(offset - RAD4_BAR2_HORIZ0) / 4];
    }
    if (offset >= RAD4_BAR2_HORIZ1 &&
        offset < RAD4_BAR2_HORIZ1 + 4 * RAD4_HORIZ_WORDS) {
        return s->horiz_timing[1][(offset - RAD4_BAR2_HORIZ1) / 4];
    }
    if (offset >= RAD4_BAR2_HORIZ3 &&
        offset < RAD4_BAR2_HORIZ3 + 4 * RAD4_HORIZ_WORDS) {
        return s->horiz_timing[2][(offset - RAD4_BAR2_HORIZ3) / 4];
    }

    switch (offset) {
    case RAD4_BAR2_RGB_LO:
        return s->rgb_index_low;
    case RAD4_BAR2_RGB_HI:
        return s->rgb_index_high;
    case RAD4_BAR2_RGB_DATA:
        return s->rgb_store[rad4_rgb_index(s)];
    case RAD4_BAR2_RGB_RESERVED:
        return 0;
    case RAD4_BAR2_MASK:
        return s->mask;
    case RAD4_BAR2_CTRL: {
        uint32_t val = s->bar2_ctrl_bit;

        if (s->frame_pending) {
            val |= RAD4_STATUS_FRAME;
        }
        return val;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR2 read at +0x%06x\n",
                     offset);
        return 0;
    }
}

static void rad4_bar2_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PsitechRad4State *s = opaque;
    uint32_t offset = addr;
    uint32_t v = val;

    if (offset < RAD4_FB_SIZE) {
        uint32_t old = ldl_be_p(s->framebuffer + offset);

        stl_be_p(s->framebuffer + offset, (old & ~s->mask) | (v & s->mask));
        return;
    }
    if (offset >= RAD4_BAR2_VERT &&
        offset < RAD4_BAR2_VERT + 4 * RAD4_VERT_WORDS) {
        s->vert_timing[(offset - RAD4_BAR2_VERT) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }
    if (offset >= RAD4_BAR2_HORIZ0 &&
        offset < RAD4_BAR2_HORIZ0 + 4 * RAD4_HORIZ_WORDS) {
        s->horiz_timing[0][(offset - RAD4_BAR2_HORIZ0) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }
    if (offset >= RAD4_BAR2_HORIZ1 &&
        offset < RAD4_BAR2_HORIZ1 + 4 * RAD4_HORIZ_WORDS) {
        s->horiz_timing[1][(offset - RAD4_BAR2_HORIZ1) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }
    if (offset >= RAD4_BAR2_HORIZ3 &&
        offset < RAD4_BAR2_HORIZ3 + 4 * RAD4_HORIZ_WORDS) {
        s->horiz_timing[2][(offset - RAD4_BAR2_HORIZ3) / 4] = v;
        rad4_recognizer_feed(s, offset, v);
        return;
    }

    switch (offset) {
    case RAD4_BAR2_RGB_LO:
        s->rgb_index_low = v;
        rad4_recognizer_feed(s, offset, v);
        break;
    case RAD4_BAR2_RGB_HI:
        s->rgb_index_high = v;
        rad4_recognizer_feed(s, offset, v);
        break;
    case RAD4_BAR2_RGB_DATA: {
        uint16_t index = rad4_rgb_index(s);

        if (!rad4_rgb_index_supported(index)) {
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad4: RGB561 write to reserved index "
                         "0x%04x\n", index);
        }
        s->rgb_store[index] = v;
        rad4_recognizer_feed(s, offset, v);
        /* Documented auto-increment on data-port access. */
        index++;
        s->rgb_index_low = index & 0xff;
        s->rgb_index_high = (index >> 8) & 0xff;
        break;
    }
    case RAD4_BAR2_RGB_RESERVED:
        break;
    case RAD4_BAR2_MASK:
        rad4_apply_mask_write(s, v);
        break;
    case RAD4_BAR2_CTRL:
        if (s->frame_pending && (v & RAD4_CTRL_STORED_BIT) == 0) {
            s->frame_pending = false;
            s->intcsr &= ~RAD4_INTCSR_PENDING;
            s->bar2_ctrl_bit = v & RAD4_CTRL_STORED_BIT;
            rad4_update_irq(s);
            break;
        }
        s->bar2_ctrl_bit = v & RAD4_CTRL_STORED_BIT;
        rad4_recognizer_feed(s, offset, v);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad4: unsupported BAR2 write at +0x%06x\n",
                     offset);
        break;
    }
}

static const MemoryRegionOps rad4_bar2_ops = {
    .read = rad4_bar2_read,
    .write = rad4_bar2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rad4_reset_hold(Object *obj, ResetType type)
{
    PsitechRad4State *s = PSITECH_RAD4(obj);

    timer_del(s->frame_timer);

    s->reg14 = 0;
    s->reg18 = 0;
    s->mailbox = 0;
    s->intcsr = 0;
    s->serial = 0;
    rad4_serial_reset(s);
    s->serial_clk_prev = false;
    s->serial_cs_prev = false;

    s->rom_desc = 0;
    s->rom_mode = 0;
    s->rom_command = 0;

    memset(s->framebuffer, 0, RAD4_FB_SIZE);
    s->mask = 0xffffffffu;

    s->rgb_index_low = 0;
    s->rgb_index_high = 0;
    memset(s->rgb_store, 0, 0x10000);

    memset(s->vert_timing, 0, sizeof(s->vert_timing));
    memset(s->horiz_timing, 0, sizeof(s->horiz_timing));

    s->bar2_ctrl_bit = 0;
    s->frame_pending = false;

    rad4_recognizer_reset(s);

    rad4_update_irq(s);
}

static void rad4_realize(PCIDevice *dev, Error **errp)
{
    PsitechRad4State *s = PSITECH_RAD4(dev);
    Object *obj = OBJECT(dev);

    s->framebuffer = g_malloc0(RAD4_FB_SIZE);
    s->rgb_store = g_malloc0(0x10000);

    s->con = qemu_graphic_console_create(DEVICE(dev), 0, &rad4_gfx_ops, s);

    memory_region_init_io(&s->bar0, obj, &rad4_bar0_ops, s,
                          "psitech-rad4-bar0", RAD4_BAR0_SIZE);
    memory_region_init_io(&s->bar2, obj, &rad4_bar2_ops, s,
                          "psitech-rad4-bar2", RAD4_BAR2_SIZE);
    memory_region_init_io(&s->romroute, obj, &rad4_romroute_ops, s,
                          "psitech-rad4-romroute", RAD4_ROMROUTE_SIZE);

    pci_set_byte(&s->pci.config[PCI_REVISION_ID], 0x03);
    pci_config_set_class(s->pci.config, PCI_CLASS_DISPLAY_OTHER);
    pci_set_word(&s->pci.config[PCI_SUBSYSTEM_VENDOR_ID], 0);
    pci_set_word(&s->pci.config[PCI_SUBSYSTEM_ID], 0);
    s->pci.config[PCI_INTERRUPT_PIN] = 0x01;

    pci_register_bar(&s->pci, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_register_bar(&s->pci, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar2);
    pci_register_bar(&s->pci, PCI_ROM_SLOT, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->romroute);

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rad4_frame_timer_cb, s);

    rad4_ensure_surface(s);
}

static void rad4_exit(PCIDevice *dev)
{
    PsitechRad4State *s = PSITECH_RAD4(dev);

    timer_free(s->frame_timer);
    qemu_graphic_console_close(s->con);
    g_free(s->framebuffer);
    g_free(s->rgb_store);
}

static int rad4_post_load(void *opaque, int version_id)
{
    PsitechRad4State *s = opaque;

    rad4_ensure_surface(s);
    rad4_update_irq(s);
    if (s->rec_state == RAD4_REC_RECOGNIZED && s->frame_timer &&
        !timer_pending(s->frame_timer)) {
        timer_mod(s->frame_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 NANOSECONDS_PER_SECOND / RAD4_FRAME_HZ);
    }
    return 0;
}

static const VMStateDescription vmstate_psitech_rad4 = {
    .name = TYPE_PSITECH_RAD4,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rad4_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(pci, PsitechRad4State),
        VMSTATE_UINT32(reg14, PsitechRad4State),
        VMSTATE_UINT32(reg18, PsitechRad4State),
        VMSTATE_UINT32(mailbox, PsitechRad4State),
        VMSTATE_UINT32(intcsr, PsitechRad4State),
        VMSTATE_UINT32(serial, PsitechRad4State),
        VMSTATE_INT32(serial_state, PsitechRad4State),
        VMSTATE_BOOL(serial_clk_prev, PsitechRad4State),
        VMSTATE_BOOL(serial_cs_prev, PsitechRad4State),
        VMSTATE_UINT32(serial_bit_count, PsitechRad4State),
        VMSTATE_UINT16(serial_command_shift, PsitechRad4State),
        VMSTATE_BOOL(serial_miso, PsitechRad4State),
        VMSTATE_UINT32(rom_desc, PsitechRad4State),
        VMSTATE_UINT32(rom_mode, PsitechRad4State),
        VMSTATE_UINT32(rom_command, PsitechRad4State),
        VMSTATE_BUFFER_POINTER_UNSAFE(framebuffer, PsitechRad4State, 1,
                                      RAD4_FB_SIZE),
        VMSTATE_UINT32(mask, PsitechRad4State),
        VMSTATE_UINT8(rgb_index_low, PsitechRad4State),
        VMSTATE_UINT8(rgb_index_high, PsitechRad4State),
        VMSTATE_BUFFER_POINTER_UNSAFE(rgb_store, PsitechRad4State, 1,
                                      0x10000),
        VMSTATE_UINT32_ARRAY(vert_timing, PsitechRad4State, RAD4_VERT_WORDS),
        VMSTATE_UINT32_2DARRAY(horiz_timing, PsitechRad4State, 3,
                               RAD4_HORIZ_WORDS),
        VMSTATE_UINT32(bar2_ctrl_bit, PsitechRad4State),
        VMSTATE_BOOL(frame_pending, PsitechRad4State),
        VMSTATE_INT32(rec_state, PsitechRad4State),
        VMSTATE_UINT32(rec_index, PsitechRad4State),
        VMSTATE_END_OF_LIST()
    },
};

static void rad4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->vendor_id = RAD4_VENDOR_ID;
    k->device_id = RAD4_DEVICE_ID;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    k->realize = rad4_realize;
    k->exit = rad4_exit;

    rc->phases.hold = rad4_reset_hold;

    dc->desc = "PsiTech RAD4 (one-mode functional approximation)";
    dc->vmsd = &vmstate_psitech_rad4;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo psitech_rad4_type_info = {
    .name          = TYPE_PSITECH_RAD4,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PsitechRad4State),
    .class_init    = rad4_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void psitech_rad4_register_types(void)
{
    type_register_static(&psitech_rad4_type_info);
}

type_init(psitech_rad4_register_types)
