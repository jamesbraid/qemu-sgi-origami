/*
 * Motorola MCF5206e M-Bus (two-wire) controller, master role.
 *
 * Five byte registers drive a timed open-drain two-wire bus: START, the eight
 * data clocks, the ninth clock that carries the acknowledge and STOP are
 * separate events at the divided module clock, so the byte-complete flag and
 * the interrupt come when the ninth clock falls and not when the register
 * is written. The slave role is not modelled; see the board's manual page.
 *
 * Register behaviour is from the MCF5206e User's Manual, revision 1,
 * chapter 13. The manual fixes the SCL frequency only: the equal clock
 * phases, applying a divider change at the next byte, a STOP waiting for the
 * byte in flight, the effect of clearing MEN and a write that sets MEN and
 * MSTA together starting a transfer are this model's choices.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/core/resettable.h"
#include "hw/i2c/i2c-wire.h"
#include "hw/m68k/mcf5206e-mbus.h"
#include "trace.h"

#define MBCR_MEN  0x80
#define MBCR_MIEN 0x40
#define MBCR_MSTA 0x20
#define MBCR_MTX  0x10
#define MBCR_TXAK 0x08
#define MBCR_RSTA 0x04
#define MBCR_STORED (MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_MTX | MBCR_TXAK)

#define MBSR_MCF  0x80
#define MBSR_MAAS 0x40
#define MBSR_MBB  0x20
#define MBSR_MAL  0x10
#define MBSR_SRW  0x04
#define MBSR_MIF  0x02
#define MBSR_RXAK 0x01
#define MBSR_RESET (MBSR_MCF | MBSR_RXAK)

/* The wire participant that is this controller. */
#define MBUS_MASTER 0

typedef enum MBusPhase {
    MBUS_IDLE,
    MBUS_START_SDA,     /* SDA fall requested */
    MBUS_START_HOLD,    /* START seen, clock about to start */
    MBUS_PAUSED,        /* SCL held low, waiting for software */
    MBUS_BITS,          /* eight data clocks */
    MBUS_ACK,           /* the ninth clock */
    MBUS_RESTART_SCL,   /* SCL rising for a repeated START */
    MBUS_STOP_SCL,
    MBUS_STOP_SDA,
} MBusPhase;

struct MCF5206EMBusState {
    SysBusDevice parent_obj;

    qemu_irq irq;
    I2CBus *bus;
    uint32_t clock_hz;

    uint8_t madr, mfdr, mbcr, mbsr, mbdr;

    MBusPhase phase;
    uint8_t bit;            /* clocks completed in this byte */
    uint8_t shift;          /* byte being sent */
    uint8_t received;       /* byte being received */
    bool tx;                /* this controller drives the data bits */
    bool scl, sda;          /* last levels the wire reported */
    bool staged;            /* a byte written before the clock was ready */
    uint8_t staged_byte;
    bool stop_pending;      /* STOP asked for while a byte was on the bus */
    int64_t low_ns, high_ns;
};

/* MCF5206EUM revision 1, table 13-2: a lookup, not a linear divisor. */
static const uint16_t mbus_divider[64] = {
    28, 30, 34, 40, 44, 48, 56, 68, 80, 88, 104, 128, 144, 160, 192, 240,
    288, 320, 384, 480, 576, 640, 768, 960, 1152, 1280, 1536, 1920,
    2304, 2560, 3072, 3840,
    20, 22, 24, 26, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
    1280, 1536, 1792, 2048
};

static void mbus_update_irq(MCF5206EMBusState *s)
{
    qemu_set_irq(s->irq, (s->mbcr & MBCR_MIEN) && (s->mbsr & MBSR_MIF));
}

/*
 * The manual gives the SCL frequency, module clock over the divisor, and no
 * duty cycle: the model splits the period equally, an approximation.
 */
static void mbus_configure_timing(MCF5206EMBusState *s)
{
    uint64_t period = (uint64_t)mbus_divider[s->mfdr & 0x3f] *
                      NANOSECONDS_PER_SECOND;
    uint64_t half = DIV_ROUND_UP(period, 2 * (uint64_t)s->clock_hz);
    int rc;

    s->low_ns = s->high_ns = MAX(half, 1);
    rc = i2c_wire_configure(s->bus, MBUS_MASTER, s->low_ns, s->high_ns,
                            false);
    if (rc) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: the wire refused the clock "
                      "timing (%d)\n", rc);
    }
}

static void mbus_stage(MCF5206EMBusState *s, int64_t at, bool sda, bool clock)
{
    int rc = i2c_wire_stage_at_or_after(s->bus, MBUS_MASTER, at, 1, sda,
                                        clock);

    if (rc) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: the wire refused a line "
                      "transition (%d); the transfer is abandoned\n", rc);
        s->phase = MBUS_IDLE;
        s->staged = false;
        s->stop_pending = false;
    }
}

/* The level this controller drives for the clock about to run. */
static bool mbus_bit_level(MCF5206EMBusState *s)
{
    if (s->bit == 8) {
        /* Clock nine: a transmitter releases SDA, a receiver drives ACK. */
        return s->tx || (s->mbcr & MBCR_TXAK);
    }
    if (!s->tx) {
        return true;
    }
    return s->shift & (0x80 >> s->bit);
}

static void mbus_begin_byte(MCF5206EMBusState *s, uint8_t byte)
{
    mbus_configure_timing(s);
    s->shift = byte;
    s->received = 0;
    s->bit = 0;
    s->tx = s->mbcr & MBCR_MTX;
    s->phase = MBUS_BITS;
    s->mbsr &= ~MBSR_MCF;
    mbus_stage(s, i2c_wire_now(s->bus) + 1, mbus_bit_level(s), true);
}

static void mbus_lose(MCF5206EMBusState *s)
{
    /* No STOP follows a lost arbitration. */
    trace_mcf5206e_mbus_lost();
    s->mbsr |= MBSR_MAL | MBSR_MIF;
    s->mbcr &= ~MBCR_MSTA;
    s->phase = MBUS_IDLE;
    s->staged = false;
    s->stop_pending = false;
    i2c_wire_reset_master(s->bus, MBUS_MASTER);
    mbus_update_irq(s);
}

static void mbus_send_stop(MCF5206EMBusState *s)
{
    s->stop_pending = false;
    trace_mcf5206e_mbus_stop();
    s->phase = MBUS_STOP_SCL;
    mbus_stage(s, i2c_wire_now(s->bus) + 1, false, true);
}

/* The ninth clock fell: the byte and its acknowledge are complete. */
static void mbus_byte_done(MCF5206EMBusState *s)
{
    if (!s->tx) {
        s->mbdr = s->received;
    }
    trace_mcf5206e_mbus_byte(s->tx, s->tx ? s->shift : s->received,
                             s->tx && (s->mbsr & MBSR_RXAK));
    s->mbsr |= MBSR_MCF | MBSR_MIF;
    s->phase = MBUS_PAUSED;
    /* Between bytes the chip holds SCL low until software writes or reads. */
    if (i2c_wire_pause_master(s->bus, MBUS_MASTER)) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: the wire refused the "
                      "between-byte clock hold\n");
    }
    mbus_update_irq(s);
    if (s->stop_pending) {
        mbus_send_stop(s);
    }
}

static void mbus_event(void *opaque, const I2CWireEvent *e)
{
    MCF5206EMBusState *s = opaque;
    bool start = s->scl && e->scl && s->sda && !e->sda;
    bool stop = s->scl && e->scl && !s->sda && e->sda;
    bool rise = !s->scl && e->scl;
    bool fall = s->scl && !e->scl;

    s->scl = e->scl;
    s->sda = e->sda;
    if (e->error) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: the wire stopped with %d; "
                      "no further transfers are represented\n", e->error);
        s->phase = MBUS_IDLE;
        s->staged = false;
        s->stop_pending = false;
        return;
    }
    if (start) {
        s->mbsr |= MBSR_MBB;
    }
    if (stop) {
        s->mbsr &= ~MBSR_MBB;
    }
    if (e->lost & (1ULL << MBUS_MASTER)) {
        mbus_lose(s);
        return;
    }

    switch (s->phase) {
    case MBUS_START_SDA:
        if (!start) {
            break;
        }
        if (!(e->contenders & (1ULL << MBUS_MASTER))) {
            /* Another START got in between: the bus was not ours. */
            mbus_lose(s);
            break;
        }
        s->phase = MBUS_START_HOLD;
        /* Keep SDA low while the clock starts, so no STOP is formed. */
        mbus_stage(s, e->time + s->high_ns, false, true);
        break;
    case MBUS_START_HOLD:
        if (!fall) {
            break;
        }
        s->phase = MBUS_PAUSED;
        if (i2c_wire_pause_master(s->bus, MBUS_MASTER)) {
            qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: the wire refused the "
                          "clock hold\n");
        }
        if (s->stop_pending) {
            mbus_send_stop(s);
        } else if (s->staged) {
            s->staged = false;
            mbus_begin_byte(s, s->staged_byte);
        }
        break;
    case MBUS_BITS:
        if (rise && !s->tx) {
            s->received = (s->received << 1) | e->sda;
        }
        if (fall) {
            s->bit++;
            if (s->bit == 8) {
                s->phase = MBUS_ACK;
            }
            mbus_stage(s, e->time + 1, mbus_bit_level(s), true);
        }
        break;
    case MBUS_ACK:
        if (rise && s->tx) {
            /* A low level on the ninth clock is an acknowledge. */
            s->mbsr = (s->mbsr & ~MBSR_RXAK) | (e->sda ? MBSR_RXAK : 0);
        }
        if (fall) {
            mbus_byte_done(s);
        }
        break;
    case MBUS_RESTART_SCL:
        if (rise) {
            s->phase = MBUS_START_SDA;
            mbus_stage(s, e->time + s->high_ns, false, false);
        }
        break;
    case MBUS_STOP_SCL:
        if (rise) {
            s->phase = MBUS_STOP_SDA;
            mbus_stage(s, e->time + s->high_ns, true, false);
        }
        break;
    case MBUS_STOP_SDA:
        if (stop) {
            s->phase = MBUS_IDLE;
        }
        break;
    default:
        break;
    }
}

static void mbus_request_start(MCF5206EMBusState *s)
{
    if (s->mbsr & MBSR_MBB) {
        /* A START on a busy bus loses arbitration. */
        mbus_lose(s);
        return;
    }
    trace_mcf5206e_mbus_start(false);
    mbus_configure_timing(s);
    s->staged = false;
    s->stop_pending = false;
    s->phase = MBUS_START_SDA;
    mbus_stage(s, i2c_wire_now(s->bus) + 1, false, false);
}

static void mbus_request_stop(MCF5206EMBusState *s)
{
    switch (s->phase) {
    case MBUS_IDLE:
    case MBUS_STOP_SCL:
    case MBUS_STOP_SDA:
        return;
    case MBUS_PAUSED:
        mbus_send_stop(s);
        return;
    default:
        /* The manual names no byte boundary; wait for the byte in flight. */
        s->stop_pending = true;
        return;
    }
}

static void mbus_request_restart(MCF5206EMBusState *s)
{
    if (s->phase != MBUS_PAUSED) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: a repeated START part way "
                      "through a byte is not modelled\n");
        return;
    }
    trace_mcf5206e_mbus_start(true);
    s->phase = MBUS_RESTART_SCL;
    mbus_stage(s, i2c_wire_now(s->bus) + 1, true, true);
}

static void mbus_software_reset(MCF5206EMBusState *s)
{
    /*
     * MEN clear holds the module in reset with its registers readable. What
     * MADR, MFDR and MBDR read afterwards is not stated; the model keeps them.
     */
    i2c_wire_reset_master(s->bus, MBUS_MASTER);
    s->phase = MBUS_IDLE;
    s->staged = false;
    s->stop_pending = false;
    s->mbsr = MBSR_RESET;
    s->scl = i2c_wire_scl(s->bus);
    s->sda = i2c_wire_sda(s->bus);
}

static void mbus_write_mbcr(MCF5206EMBusState *s, uint8_t value)
{
    /* Until MEN is set no other MBCR bit has an effect, so none is an edge. */
    uint8_t old = (s->mbcr & MBCR_MEN) ? s->mbcr : 0;
    uint8_t new = value & MBCR_STORED;

    s->mbsr &= ~MBSR_MAAS;
    s->mbcr = new;
    if (!(new & MBCR_MEN)) {
        mbus_software_reset(s);
    } else if (!(old & MBCR_MSTA) && (new & MBCR_MSTA)) {
        mbus_request_start(s);
    } else if ((old & MBCR_MSTA) && !(new & MBCR_MSTA)) {
        mbus_request_stop(s);
    } else if (value & MBCR_RSTA) {
        if (new & MBCR_MSTA) {
            mbus_request_restart(s);
        } else {
            /* A repeated START asked for in slave mode loses arbitration. */
            mbus_lose(s);
        }
    }
    mbus_update_irq(s);
}

static void mbus_write_mbdr(MCF5206EMBusState *s, uint8_t value)
{
    s->mbdr = value;
    if ((s->mbcr & (MBCR_MEN | MBCR_MSTA | MBCR_MTX)) !=
        (MBCR_MEN | MBCR_MSTA | MBCR_MTX)) {
        qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e-mbus: MBDR written outside "
                      "an enabled master transmit, no byte is sent\n");
        return;
    }
    switch (s->phase) {
    case MBUS_START_SDA:
    case MBUS_START_HOLD:
    case MBUS_RESTART_SCL:
        /* The calling byte follows the START it was written ahead of. */
        s->staged = true;
        s->staged_byte = value;
        s->mbsr &= ~MBSR_MCF;
        break;
    case MBUS_PAUSED:
        mbus_begin_byte(s, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e-mbus: MBDR written while a "
                      "byte is on the bus, ignored\n");
        break;
    }
}

uint8_t mcf5206e_mbus_read(MCF5206EMBusState *s, hwaddr offset,
                           bool side_effects)
{
    if (side_effects) {
        i2c_wire_advance(s->bus);
    }
    switch (offset) {
    case MCF5206E_MBUS_MADR:
        return s->madr;
    case MCF5206E_MBUS_MFDR:
        return s->mfdr;
    case MCF5206E_MBUS_MBCR:
        return s->mbcr;
    case MCF5206E_MBUS_MBSR:
        return s->mbsr;
    case MCF5206E_MBUS_MBDR: {
        uint8_t value = s->mbdr;

        /* A read in receive mode starts the next byte. */
        if (side_effects &&
            (s->mbcr & (MBCR_MEN | MBCR_MSTA | MBCR_MTX)) ==
            (MBCR_MEN | MBCR_MSTA) && s->phase == MBUS_PAUSED) {
            mbus_begin_byte(s, 0);
        }
        return value;
    }
    default:
        g_assert_not_reached();
    }
}

void mcf5206e_mbus_write(MCF5206EMBusState *s, hwaddr offset, uint8_t value)
{
    i2c_wire_advance(s->bus);
    switch (offset) {
    case MCF5206E_MBUS_MADR:
        s->madr = value & 0xfe;
        if (s->madr) {
            qemu_log_mask(LOG_UNIMP, "mcf5206e-mbus: own slave address "
                          "0x%02x stored, the slave role is not modelled\n",
                          s->madr >> 1);
        }
        break;
    case MCF5206E_MBUS_MFDR:
        /* A change applies from the next byte. */
        s->mfdr = value & 0x3f;
        break;
    case MCF5206E_MBUS_MBCR:
        mbus_write_mbcr(s, value);
        break;
    case MCF5206E_MBUS_MBSR:
        /* Only MAL and MIF clear, each by a written zero. */
        if (!(value & MBSR_MAL)) {
            s->mbsr &= ~MBSR_MAL;
        }
        if (!(value & MBSR_MIF)) {
            s->mbsr &= ~MBSR_MIF;
        }
        mbus_update_irq(s);
        break;
    case MCF5206E_MBUS_MBDR:
        mbus_write_mbdr(s, value);
        break;
    default:
        g_assert_not_reached();
    }
}

I2CBus *mcf5206e_mbus_get_bus(MCF5206EMBusState *s)
{
    return s->bus;
}

static void mbus_reset_hold(Object *obj, ResetType type)
{
    MCF5206EMBusState *s = MCF5206E_MBUS(obj);

    s->madr = s->mfdr = s->mbcr = s->mbdr = 0;
    mbus_software_reset(s);
    s->bit = s->shift = s->received = 0;
    s->tx = false;
    mbus_update_irq(s);
}

/* The clock phases the last START or byte used, for the register tests. */
static void mbus_get_phase_ns(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    int64_t ns = *(int64_t *)opaque;

    visit_type_int(v, name, &ns, errp);
}

static void mbus_realize(DeviceState *dev, Error **errp)
{
    MCF5206EMBusState *s = MCF5206E_MBUS(dev);

    if (!s->clock_hz) {
        error_setg(errp, "mcf5206e-mbus clock-hz must be non-zero");
        return;
    }
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    object_property_add(OBJECT(dev), "scl-low-ns", "int", mbus_get_phase_ns,
                        NULL, NULL, &s->low_ns);
    object_property_add(OBJECT(dev), "scl-high-ns", "int", mbus_get_phase_ns,
                        NULL, NULL, &s->high_ns);
    s->bus = i2c_init_bus_type(TYPE_I2C_WIRE, dev, "mbus");
    if (i2c_bus_enable_wire(s->bus, 1, NULL, NULL, errp)) {
        return;
    }
    if (i2c_wire_set_observer(s->bus, MBUS_MASTER, mbus_event, s)) {
        error_setg(errp, "mcf5206e-mbus could not attach to its wire");
        return;
    }
    s->scl = i2c_wire_scl(s->bus);
    s->sda = i2c_wire_sda(s->bus);
}

static void mbus_unrealize(DeviceState *dev)
{
    MCF5206EMBusState *s = MCF5206E_MBUS(dev);

    if (s->bus) {
        i2c_wire_clear_observer(s->bus, MBUS_MASTER, mbus_event, s);
    }
}

static const Property mbus_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", MCF5206EMBusState, clock_hz, 0),
};

static void mbus_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "MCF5206e M-Bus controller";
    dc->realize = mbus_realize;
    dc->unrealize = mbus_unrealize;
    device_class_set_props(dc, mbus_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    rc->phases.hold = mbus_reset_hold;
    /* The state is not migrated: the board refuses migration. */
    dc->user_creatable = false;
}

static const TypeInfo mbus_info = {
    .name          = TYPE_MCF5206E_MBUS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MCF5206EMBusState),
    .class_init    = mbus_class_init,
};

static void mbus_register_types(void)
{
    type_register_static(&mbus_info);
}

type_init(mbus_register_types)
