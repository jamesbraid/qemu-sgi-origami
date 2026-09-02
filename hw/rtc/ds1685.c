/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DS1685/DS1687 real-time clock: the DS1287 PC clock's fourteen registers,
 * a second software-selected bank carrying the extended half, 242 bytes of
 * battery-backed RAM in three pieces, and the power control a machine uses
 * to switch itself off.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * Reached indirectly, through an address port and a data port one byte each:
 * a parallel host cannot drive the part's multiplexed bus. The bank bit
 * switches only 40h-7Fh, so the four control registers and the low RAM are
 * the same cells whichever bank is selected -- a model that switches the
 * whole file answers every driver wrongly and no boot need notice.
 */

#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/rtc/ds1685.h"
#include "migration/vmstate.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "trace.h"

/* The vendor's tBUC: how long before a transfer the in-progress bit rises. */
#define DS1685_UIP_HOLD_NS      (8 * NANOSECONDS_PER_SECOND / 32768)

/* The rate decoder's reference, which is also the crystal's frequency. */
#define DS1685_TICKS_PER_SECOND 32768

/* Bank-1 registers are stored in one array based at 40h. */
#define EXT(reg)                ((reg) - DS1685_UPPER_FIRST)

static void ds1685_schedule_update(DS1685State *s);
static void ds1685_schedule_periodic(DS1685State *s);

static bool ds1685_bank1(DS1685State *s)
{
    return s->common[DS1685_REG_A] & DS1685_A_DV0;
}

/*
 * A pattern of 01X in DV2, DV1, DV0 turns the oscillator on and lets the
 * countdown chain run. 11X keeps the oscillator on with the chain held in
 * reset, and anything else stops the oscillator; neither of those updates
 * the visible time.
 */
static bool ds1685_counting(DS1685State *s)
{
    return (s->common[DS1685_REG_A] & (DS1685_A_DV2 | DS1685_A_DV1)) ==
           DS1685_A_DV1;
}

/* One decoder feeds both the periodic interrupt and the square-wave pin. */
static bool ds1685_binary(DS1685State *s)
{
    return s->common[DS1685_REG_B] & DS1685_B_DM;
}

static uint8_t ds1685_encode(DS1685State *s, unsigned int value)
{
    return ds1685_binary(s) ? value : to_bcd(value);
}

static unsigned int ds1685_decode(DS1685State *s, uint8_t value)
{
    return ds1685_binary(s) ? value : from_bcd(value);
}

static uint8_t ds1685_encode_hour(DS1685State *s, int hour)
{
    bool afternoon;
    int twelve;

    if (s->common[DS1685_REG_B] & DS1685_B_24_12) {
        return ds1685_encode(s, hour);
    }

    /* One through twelve, with bit 7 marking the afternoon. */
    afternoon = hour >= 12;
    twelve = hour % 12;
    if (twelve == 0) {
        twelve = 12;
    }
    return ds1685_encode(s, twelve) | (afternoon ? 0x80 : 0);
}

static int ds1685_decode_hour(DS1685State *s, uint8_t value)
{
    int hour;

    if (s->common[DS1685_REG_B] & DS1685_B_24_12) {
        return ds1685_decode(s, value);
    }

    hour = ds1685_decode(s, value & 0x7f) % 12;
    return (value & 0x80) ? hour + 12 : hour;
}

/* Six sources, one level-driven line, and IRQF is exactly this predicate. */
static bool ds1685_irq_asserted(DS1685State *s)
{
    uint8_t b = s->common[DS1685_REG_B];
    uint8_t c = s->common[DS1685_REG_C];
    uint8_t x4a = s->ext[EXT(DS1685_EXT_CTRL_4A)];
    uint8_t x4b = s->ext[EXT(DS1685_EXT_CTRL_4B)];

    return ((c & DS1685_C_PF) && (b & DS1685_B_PIE)) ||
           ((c & DS1685_C_AF) && (b & DS1685_B_AIE)) ||
           ((c & DS1685_C_UF) && (b & DS1685_B_UIE)) ||
           ((x4a & DS1685_4A_RF) && (x4b & DS1685_4B_RIE)) ||
           ((x4a & DS1685_4A_WF) && (x4b & DS1685_4B_WIE)) ||
           ((x4a & DS1685_4A_KF) && (x4b & DS1685_4B_KSE));
}

static void ds1685_update_irq(DS1685State *s)
{
    bool asserted = ds1685_irq_asserted(s);

    trace_ds1685_irq(asserted);
    qemu_set_irq(s->irq, asserted);
}

/*
 * The part will not hold the supply off while an enabled wake-up or
 * kickstart is asking it back on. Applied after anything that can change a
 * flag, an enable, or the power bit itself.
 */
static void ds1685_apply_power_rule(DS1685State *s)
{
    uint8_t x4a = s->ext[EXT(DS1685_EXT_CTRL_4A)];
    uint8_t x4b = s->ext[EXT(DS1685_EXT_CTRL_4B)];

    if (((x4a & DS1685_4A_WF) && (x4b & DS1685_4B_WIE)) ||
        ((x4a & DS1685_4A_KF) && (x4b & DS1685_4B_KSE))) {
        s->ext[EXT(DS1685_EXT_CTRL_4A)] &= ~DS1685_4A_PAB;
    }
}

/*
 * Computed, never stored. Raising SET clears it, and a stopped countdown
 * chain never raises it, so the two cases a driver can force are exact.
 */
static bool ds1685_uip(DS1685State *s)
{
    int64_t now;

    if ((s->common[DS1685_REG_B] & DS1685_B_SET) || !ds1685_counting(s)) {
        return false;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return s->next_update_ns > now &&
           s->next_update_ns - now <= DS1685_UIP_HOLD_NS;
}

/* The visible bytes are a copy of the counters, rewritten once a second. */
static void ds1685_publish_time(DS1685State *s)
{
    struct tm tm;

    qemu_get_timedate(&tm, s->time_offset);

    s->common[DS1685_SECONDS] = ds1685_encode(s, tm.tm_sec) & 0x7f;
    s->common[DS1685_MINUTES] = ds1685_encode(s, tm.tm_min);
    s->common[DS1685_HOURS] = ds1685_encode_hour(s, tm.tm_hour);
    /* One through seven, where the C library counts Sunday as zero. */
    s->common[DS1685_WEEKDAY] = ds1685_encode(s, tm.tm_wday + 1);
    s->common[DS1685_DAY] = ds1685_encode(s, tm.tm_mday);
    s->common[DS1685_MONTH] = ds1685_encode(s, tm.tm_mon + 1);
    s->common[DS1685_YEAR] = ds1685_encode(s, (tm.tm_year + 1900) % 100);
    s->ext[EXT(DS1685_CENTURY)] = ds1685_encode(s, (tm.tm_year + 1900) / 100);
}

/* Releasing the freeze is what hands the edited copy back to the counters. */
static void ds1685_commit_time(DS1685State *s)
{
    struct tm tm = { 0 };
    struct tm now;

    tm.tm_sec = ds1685_decode(s, s->common[DS1685_SECONDS] & 0x7f);
    tm.tm_min = ds1685_decode(s, s->common[DS1685_MINUTES]);
    tm.tm_hour = ds1685_decode_hour(s, s->common[DS1685_HOURS]);
    tm.tm_mday = ds1685_decode(s, s->common[DS1685_DAY]);
    tm.tm_mon = ds1685_decode(s, s->common[DS1685_MONTH]) - 1;
    tm.tm_year = ds1685_decode(s, s->ext[EXT(DS1685_CENTURY)]) * 100 +
                 ds1685_decode(s, s->common[DS1685_YEAR]) - 1900;

    /*
     * Not qemu_timedate_diff(): that subtracts the host clock unconditionally
     * while reads answer from whichever clock -rtc clock= selected.
     * Differencing through qemu_get_timedate() keeps both ends on one clock.
     */
    qemu_get_timedate(&now, 0);
    s->time_offset = mktimegm(&tm) - mktimegm(&now);
}

/* Any alarm byte from C0h up matches every value of its own field. */
static bool ds1685_alarm_field_matches(uint8_t alarm, uint8_t value)
{
    return alarm >= DS1685_ALARM_DONT_CARE || alarm == value;
}

static bool ds1685_alarm_matches(DS1685State *s)
{
    return ds1685_alarm_field_matches(s->common[DS1685_SECONDS_ALARM],
                                      s->common[DS1685_SECONDS]) &&
           ds1685_alarm_field_matches(s->common[DS1685_MINUTES_ALARM],
                                      s->common[DS1685_MINUTES]) &&
           ds1685_alarm_field_matches(s->common[DS1685_HOURS_ALARM],
                                      s->common[DS1685_HOURS]);
}

/*
 * The wake-up alarm is the time alarm with a date on it, and the date has no
 * don't-care encoding -- only the three time bytes do.
 */
static bool ds1685_wakeup_matches(DS1685State *s)
{
    return ds1685_alarm_matches(s) &&
           s->ext[EXT(DS1685_DATE_ALARM)] == s->common[DS1685_DAY];
}

static void ds1685_update_cycle(void *opaque)
{
    DS1685State *s = opaque;

    /*
     * SET inhibits the transfer, so no update cycle happens and nothing an
     * update cycle does happens either -- no flag, no alarm check. The
     * counters run on underneath, which is what makes the freeze safe to
     * hold across a driver's eight reads.
     */
    if (s->common[DS1685_REG_B] & DS1685_B_SET) {
        ds1685_schedule_update(s);
        return;
    }

    ds1685_publish_time(s);
    s->common[DS1685_REG_C] |= DS1685_C_UF;

    if (ds1685_alarm_matches(s)) {
        s->common[DS1685_REG_C] |= DS1685_C_AF;
    }
    if (ds1685_wakeup_matches(s)) {
        s->ext[EXT(DS1685_EXT_CTRL_4A)] |= DS1685_4A_WF;
        ds1685_apply_power_rule(s);
    }

    ds1685_update_irq(s);
    ds1685_schedule_update(s);
}

static void ds1685_schedule_update(DS1685State *s)
{
    if (!ds1685_counting(s)) {
        timer_del(s->update_timer);
        s->next_update_ns = 0;
        return;
    }

    s->next_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        NANOSECONDS_PER_SECOND;
    timer_mod(s->update_timer, s->next_update_ns);
}

/*
 * Sixteen rate codes select thirteen taps: zero is off, and codes 1 and 2
 * duplicate codes 8 and 9. The result is a count of 32.768 kHz cycles.
 */
static int64_t ds1685_periodic_period(DS1685State *s)
{
    int code = s->common[DS1685_REG_A] & DS1685_A_RS;

    if (code == 0) {
        return 0;
    }
    if (code <= 2) {
        code += 7;
    }
    return (int64_t)(1 << (code - 1)) * NANOSECONDS_PER_SECOND /
           DS1685_TICKS_PER_SECOND;
}

static void ds1685_schedule_periodic(DS1685State *s)
{
    s->periodic_period_ns = ds1685_periodic_period(s);

    if (s->periodic_period_ns == 0 || !ds1685_counting(s)) {
        timer_del(s->periodic_timer);
        return;
    }

    timer_mod(s->periodic_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              s->periodic_period_ns);
}

static void ds1685_periodic_tick(void *opaque)
{
    DS1685State *s = opaque;

    /* The flag is set whether or not its enable is, so polling works. */
    s->common[DS1685_REG_C] |= DS1685_C_PF;
    ds1685_update_irq(s);
    ds1685_schedule_periodic(s);
}

/* Bank-1 locations the vendor says read zero and cannot be written. */
static bool ds1685_bank1_reserved(uint8_t reg)
{
    switch (reg) {
    case 0x4c:
    case 0x4d:
    case 0x51:
    case 0x52:
        return true;
    default:
        return reg >= 0x54;
    }
}

static uint8_t ds1685_read_reg(DS1685State *s, uint8_t reg)
{
    if (reg < DS1685_UPPER_FIRST) {
        switch (reg) {
        case DS1685_REG_A:
            return (s->common[reg] & ~DS1685_A_UIP) |
                   (ds1685_uip(s) ? DS1685_A_UIP : 0);

        case DS1685_REG_C: {
            uint8_t value = (s->common[reg] & DS1685_C_FLAGS) |
                            (ds1685_irq_asserted(s) ? DS1685_C_IRQF : 0);

            /*
             * Reading is what clears these three, and only these three: the
             * extended register's flags take a written zero instead. A model
             * that gets the pair the wrong way round loses interrupts in one
             * direction and storms in the other.
             */
            s->common[reg] &= ~DS1685_C_FLAGS;
            ds1685_update_irq(s);
            return value;
        }

        case DS1685_REG_D:
            /* Bits 6-0 always read zero. */
            return s->common[reg] & DS1685_D_VRT;

        default:
            return s->common[reg];
        }
    }

    if (!ds1685_bank1(s)) {
        return s->nvram[EXT(reg)];
    }

    switch (reg) {
    case DS1685_EXT_CTRL_4A:
        /*
         * The increment bit never reads set. An update here is one atomic
         * step, so there is no interval to be inside, and Linux spins on
         * this bit with no timeout in probe and on every time read -- a
         * model that answers otherwise wedges the guest before any console
         * message names the clock.
         */
        return s->ext[EXT(reg)] & ~DS1685_4A_INCR;

    case DS1685_EXTRAM_DATA:
        return s->extram[s->ext[EXT(DS1685_EXTRAM_ADDR)] &
                         (DS1685_EXTRAM_SIZE - 1)];

    case DS1685_SMI_STACK_FIRST:
    case DS1685_SMI_STACK_LAST:
        qemu_log_mask(LOG_UNIMP,
                      "ds1685: read of the SMI recovery stack at 0x%02x, "
                      "which this model does not latch\n", reg);
        return 0;

    default:
        if (ds1685_bank1_reserved(reg)) {
            return 0;
        }
        return s->ext[EXT(reg)];
    }
}

static void ds1685_write_reg(DS1685State *s, uint8_t reg, uint8_t val)
{
    if (reg < DS1685_UPPER_FIRST) {
        switch (reg) {
        case DS1685_SECONDS:
            /* Bit 7 here is the one read-only bit among the time bytes. */
            s->common[reg] = val & 0x7f;
            break;

        case DS1685_REG_A: {
            uint8_t was = s->common[reg];

            /*
             * Both kernels write back a value they just read, so a read-only
             * update bit that took a write would set itself.
             */
            s->common[reg] = val & ~DS1685_A_UIP;
            if ((was ^ s->common[reg]) & DS1685_A_RS) {
                ds1685_schedule_periodic(s);
            }
            if ((was ^ s->common[reg]) & (DS1685_A_DV2 | DS1685_A_DV1)) {
                ds1685_schedule_update(s);
                ds1685_schedule_periodic(s);
            }
            break;
        }

        case DS1685_REG_B: {
            uint8_t was = s->common[reg];

            if ((val & DS1685_B_SET) && !(was & DS1685_B_SET)) {
                /*
                 * Raising the freeze also disables the update-ended
                 * interrupt, which Linux's platform data works around by
                 * declaring the feature unsupported rather than by noticing.
                 */
                val &= ~DS1685_B_UIE;
            }
            if ((val & ~was) & DS1685_B_DSE) {
                qemu_log_mask(LOG_UNIMP,
                              "ds1685: daylight-saving enable set; this model "
                              "does not apply the part's pre-2007 rule\n");
            }

            s->common[reg] = val;

            if (!(val & DS1685_B_SET) && (was & DS1685_B_SET)) {
                ds1685_commit_time(s);
                ds1685_schedule_update(s);
            }
            ds1685_update_irq(s);
            break;
        }

        case DS1685_REG_C:
        case DS1685_REG_D:
            /* Read-only entire, two of the vendor's three stated exceptions. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ds1685: write of 0x%02x to read-only register "
                          "0x%02x\n", val, reg);
            break;

        default:
            /*
             * Everything else takes the write whole, the fixed-zero bits of
             * the time and date registers included: the vendor says such a
             * bit can be written to a 1 that a later update may overwrite,
             * so the model stores it rather than masking it away.
             */
            s->common[reg] = val;
            break;
        }
        return;
    }

    if (!ds1685_bank1(s)) {
        s->nvram[EXT(reg)] = val;
        return;
    }

    if (reg == DS1685_MODEL ||
        (reg >= DS1685_SERIAL_FIRST && reg <= DS1685_SERIAL_CRC)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ds1685: write of 0x%02x to read-only register 0x%02x\n",
                      val, reg);
        return;
    }

    switch (reg) {
    case DS1685_EXT_CTRL_4A:
        /*
         * The battery and increment bits are read-only; everything below
         * them takes the write, the two inert reserved bits included. A
         * plain store is also what gives the flags their clearing rule:
         * they go when a zero is written over them and not when read, and
         * software can write them to a 1 to force an interrupt condition.
         */
        s->ext[EXT(reg)] = (s->ext[EXT(reg)] &
                            (DS1685_4A_VRT2 | DS1685_4A_INCR)) |
                           (val & ~(DS1685_4A_VRT2 | DS1685_4A_INCR));
        ds1685_apply_power_rule(s);
        ds1685_update_irq(s);

        if (s->ext[EXT(reg)] & DS1685_4A_PAB) {
            /*
             * The machine's power-off path. The guest never returns from
             * this write: Linux spins forever after it.
             */
            trace_ds1685_power_off();
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        break;

    case DS1685_EXT_CTRL_4B:
        s->ext[EXT(reg)] = val;
        ds1685_apply_power_rule(s);
        ds1685_update_irq(s);
        break;

    case DS1685_EXTRAM_DATA:
        s->extram[s->ext[EXT(DS1685_EXTRAM_ADDR)] &
                  (DS1685_EXTRAM_SIZE - 1)] = val;
        break;

    case DS1685_SMI_STACK_FIRST:
    case DS1685_SMI_STACK_LAST:
        qemu_log_mask(LOG_UNIMP,
                      "ds1685: write of 0x%02x to the SMI recovery stack at "
                      "0x%02x, which this model does not latch\n", val, reg);
        break;

    default:
        if (ds1685_bank1_reserved(reg)) {
            /*
             * Documented as unwritable rather than unimplemented: a guest
             * writing here is not making a mistake.
             */
            trace_ds1685_reserved_write(reg, val);
            break;
        }
        s->ext[EXT(reg)] = val;
        break;
    }
}

/*
 * Nothing in the sources establishes what a read of the address port
 * returns; it is a write port on the part and no driver reads it. Answering
 * with the latch is at least the truth about this model.
 */
static uint64_t ds1685_index_read(void *opaque, hwaddr addr, unsigned size)
{
    DS1685State *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "ds1685: read of the address port, which is a write port\n");
    return s->index;
}

static void ds1685_index_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    DS1685State *s = opaque;

    /*
     * Seven bits. Bit 7 of a latched address holds the bank, and the bank
     * comes from Register A rather than from what is written here.
     */
    s->index = val & 0x7f;
    trace_ds1685_select(s->index);
}

static uint64_t ds1685_data_read(void *opaque, hwaddr addr, unsigned size)
{
    DS1685State *s = opaque;
    uint8_t value;
    bool bank1 = ds1685_bank1(s);

    /* The latch is untouched: a driver reads one register repeatedly. */
    value = ds1685_read_reg(s, s->index);
    trace_ds1685_read(s->index, bank1, value);
    return value;
}

static void ds1685_data_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    DS1685State *s = opaque;

    trace_ds1685_write(s->index, ds1685_bank1(s), val);
    ds1685_write_reg(s, s->index, val);
}

/*
 * One byte each, and declared so. An undeclared implementation width lets
 * the core chunk a wide access into several calls, which would run the
 * address latch's side effect more than once.
 *
 * Each port answers on any byte lane of its word rather than at one exact
 * address, which is what DS1685_PORT_SIZE is for: the chip select picks a
 * byte-wide port, not the low address bits, and the two guests land on
 * different lanes. OpenBSD hands this chip a non-swizzling accessor and
 * writes offset 0. Linux inverts the low two address bits of every byte
 * access on this machine, and cancels that by pre-permuting the structures
 * it lays over the Timekeeper and the serial part; a single-byte port at a
 * word-aligned base has nothing to permute, so the inversion stands and the
 * access lands at offset 3. A model decoding one exact byte answers one
 * guest and returns zero to the other.
 */
static const MemoryRegionOps ds1685_index_ops = {
    .read = ds1685_index_read,
    .write = ds1685_index_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static const MemoryRegionOps ds1685_data_ops = {
    .read = ds1685_data_read,
    .write = ds1685_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void ds1685_reset_hold(Object *obj, ResetType type)
{
    DS1685State *s = DS1685(obj);

    /*
     * A reset is VCC arriving at a part whose cell never went away, so it
     * applies the values the vendor documents for a power-on and nothing
     * else. The register file, all three RAM regions, the bank bit and the
     * running time survive it, as the Timekeeper's do.
     */
    s->common[DS1685_REG_A] |= DS1685_A_DV1;
    s->ext[EXT(DS1685_EXT_CTRL_4B)] |= DS1685_4B_E32K;
    s->ext[EXT(DS1685_EXT_CTRL_4A)] &= ~DS1685_4A_PAB;

    /*
     * The two remaining documented values need no clearing: the
     * update-in-progress and increment bits are computed rather than stored.
     * The interrupt output is derived rather than latched, so this
     * recomputes it instead of forcing it low -- a flag that survives with
     * its enable brings the line straight back, which is what a level output
     * does.
     */
    ds1685_schedule_update(s);
    ds1685_schedule_periodic(s);
    ds1685_update_irq(s);
}

static void ds1685_realize(DeviceState *dev, Error **errp)
{
    DS1685State *s = DS1685(dev);

    /*
     * The vendor declares almost every power-up value undefined. These are
     * chosen once, here, from the set no driver can tell from working
     * hardware. They are this model's answer on a first read and not a fact
     * about the part, which is exactly why they are here and not in reset:
     * the four values reset applies are the vendor's, and these are ours.
     *
     * The serial number and its check byte stay zero. No public source gives
     * a value for either, and no driver on our paths validates them.
     */
    s->common[DS1685_REG_D] = DS1685_D_VRT;
    s->ext[EXT(DS1685_MODEL)] = DS1685_MODEL_NUMBER;
    s->ext[EXT(DS1685_EXT_CTRL_4A)] = DS1685_4A_VRT2;

    s->update_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ds1685_update_cycle, s);
    s->periodic_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     ds1685_periodic_tick, s);

    /* Binary-coded decimal, because the format bit starts clear. */
    ds1685_publish_time(s);

    memory_region_init_io(&s->index_port, OBJECT(dev), &ds1685_index_ops, s,
                          "ds1685.index", DS1685_PORT_SIZE);
    memory_region_init_io(&s->data_port, OBJECT(dev), &ds1685_data_ops, s,
                          "ds1685.data", DS1685_PORT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->index_port);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->data_port);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void ds1685_unrealize(DeviceState *dev)
{
    DS1685State *s = DS1685(dev);

    timer_free(s->update_timer);
    timer_free(s->periodic_timer);
    s->update_timer = NULL;
    s->periodic_timer = NULL;
}

static int ds1685_post_load(void *opaque, int version_id)
{
    DS1685State *s = opaque;

    /* Derived rather than carried, on both sides of the stream. */
    ds1685_update_irq(s);
    return 0;
}

/*
 * Battery-backed state has to cross a migration for the same reason it has
 * to cross a reset: the guest was never told about either.
 */
static const VMStateDescription vmstate_ds1685 = {
    .name = "ds1685",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ds1685_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(index, DS1685State),
        VMSTATE_UINT8_ARRAY(common, DS1685State, DS1685_COMMON_SIZE),
        VMSTATE_UINT8_ARRAY(nvram, DS1685State, DS1685_UPPER_SIZE),
        VMSTATE_UINT8_ARRAY(ext, DS1685State, DS1685_UPPER_SIZE),
        VMSTATE_UINT8_ARRAY(extram, DS1685State, DS1685_EXTRAM_SIZE),
        VMSTATE_INT64(time_offset, DS1685State),
        VMSTATE_INT64(next_update_ns, DS1685State),
        VMSTATE_INT64(periodic_period_ns, DS1685State),
        VMSTATE_TIMER_PTR(update_timer, DS1685State),
        VMSTATE_TIMER_PTR(periodic_timer, DS1685State),
        VMSTATE_END_OF_LIST()
    }
};

static void ds1685_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "DS1685/DS1687 real-time clock";
    dc->realize = ds1685_realize;
    dc->unrealize = ds1685_unrealize;
    dc->vmsd = &vmstate_ds1685;
    rc->phases.hold = ds1685_reset_hold;
}

static const TypeInfo ds1685_types[] = {
    {
        .name = TYPE_DS1685,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DS1685State),
        .class_init = ds1685_class_init,
    },
};

DEFINE_TYPES(ds1685_types)
