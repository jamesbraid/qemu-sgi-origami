/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 console's "env" report for the Chimera node boards.
 *
 * The table layout, names, states and limits follow the reviewed L1 and
 * IP59 datasheets and the console captures they cite. The firmware has one
 * copy of each row format, shared by every brick personality, so each row
 * kind below has one printer and the tables only supply data.
 *
 * Every reading is model state: a fixed, in-limits value, not a
 * measurement, so a guest or a test sees the same value each time. The
 * datasheet lets a model choose its own readings. The 2U and workstation
 * IP59 readings are taken from captured sessions; the IP53 2U readings
 * reuse the IP59 2U ones, and every rack-mounted workstation reading
 * (including the Odyssey fan's RPM) is a model choice. The node's own
 * voltages, fans and temperatures are not simulated.
 *
 * Output the datasheet does not record is an approximation. Each one is
 * stated where it is chosen and logged with LOG_UNIMP when it is printed.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-l1-env.h"

typedef enum {
    ENV_IP53,
    ENV_IP59,
} EnvBoard;

/* Warning and fault limits are these percentages either side of nominal. */
#define ENV_WARN_PERCENT 10
#define ENV_FAULT_PERCENT 20

/* Supply names are right-aligned in this many columns on every board. */
#define ENV_NAME_WIDTH 14

/* The state column of a row on a node that has no power. */
#define ENV_WAIT_POWER "Wait Pwr"

/* Wraps the supply table when power faults do not shut the brick down. */
#define ENV_MONITOR_ONLY \
    "**** MONITOR-ONLY, NO BRICK SHUTDOWN ON POWER FAULTS ****\r\n"

#define ENV_ZONE_MEAN UINT_MAX

typedef struct EnvRail {
    const char *name;
    unsigned nominal_mv;
    unsigned reading_mv;
} EnvRail;

/*
 * A fan's control channel drive bytes: low, medium and high. The low level is
 * what the monitor writes at start-up. @low_ip59 replaces it on IP59 when
 * non-zero. A fan of the workstation has no levels: its channel mapping was
 * not traced.
 */
typedef struct EnvFan {
    const char *name;
    unsigned warning_rpm;
    unsigned reading_rpm[2];
    uint8_t level[3];
    uint8_t low_ip59;
} EnvFan;

/* A sensor's limits in degrees C: advisory, critical, fault and maximum. */
typedef struct EnvLimits {
    unsigned advisory, critical, fault, maximum;
} EnvLimits;

/* The limits the firmware assigns each personality's sensors. */
static const EnvLimits lim_7 = { 31, 48, 55, 60 };
static const EnvLimits lim_q = { 70, 71, 72, 75 };
static const EnvLimits lim_9 = { 70, 72, 74, 76 };
/* The on-node sensor of the workstation: never changed by altitude. */
static const EnvLimits lim_9_node = { 70, 75, 80, 85 };

typedef struct EnvTemp {
    const char *name;
    bool present;
    unsigned reading_c;
    /* A sensor on the node cannot be read while the node has no power. */
    bool on_node;
    const struct EnvLimits *limits;
} EnvTemp;

/* How a temperature row shows its limits. */
typedef enum {
    ENV_TEMP_FULL,          /* advisory, critical, fault */
    ENV_TEMP_AUTOFAN,       /* "[Autofan Control]" and the fault limit */
    ENV_TEMP_CHECKS_OFF,    /* advisory and critical checking is off */
} EnvTempMode;

typedef struct EnvZone {
    const char *name;
    const char *sensors;
    /* A zero target prints N/A for both temperatures. */
    unsigned target_c;
    unsigned current_c;     /* ENV_ZONE_MEAN: the mean of the sensors */
    const char *fans;
    /*
     * Zones of the rack-mounted workstation derive their percentages from
     * the first fan's drive byte (@fan_index) and the zone's minimum byte,
     * inverted for @inverted zones. The workstation zones have no fan
     * mapping, so @fan_index is -1 and both print the fixed @min_percent.
     */
    int fan_index;
    uint8_t min_byte;
    bool inverted;
    unsigned min_percent;
} EnvZone;

/*
 * The VCPU nominal comes from the node's PSC code, not from the sensor
 * record, which is the same on both boards. The model has no PSC register;
 * the codes below are what the captured boards imply: 1.27 V (code 5) on
 * every captured IP59, 1.25 V (code 1 or 3) on every captured IP53.
 */
static unsigned env_psc_code(EnvBoard board)
{
    return board == ENV_IP59 ? 5 : 1;
}

static unsigned env_vcpu_nominal_mv(unsigned psc)
{
    switch (psc) {
    case 0:
        return 1600;
    case 1:
    case 3:
        return 1250;
    case 4:
        return 1275;
    case 5:
        return 1270;
    default:
        return 0;
    }
}

/* A limit in hundredths of a volt, rounded half up. */
static unsigned env_limit_centivolts(unsigned nominal_mv, int percent)
{
    return (nominal_mv * (100 + percent) / 100 + 5) / 10;
}

/* Rails common to both generations; the node's own follow. */
static const EnvRail ws_rails[] = {
    { "1.8V", 1800, 1720 },
    { "12V", 12000, 11938 },
    { "12V #2", 12000, 12125 },
    { "3.3V", 3300, 3285 },
    { "2.5V", 2500, 2509 },
    { "12V IO", 12000, 11875 },
    { "5V AUX", 5000, 4888 },
    { "3.3V AUX", 3300, 3268 },
    { "5V", 5000, 4992 },
    { "XIO 12V BIAS", 12000, 11750 },
    { "XIO 5V", 5000, 4992 },
    { "XIO 2.5V", 2500, 2457 },
    { "XIO 3.3V AUX", 3300, 3285 },
};

/* The 2U board has no XIO rails fitted; they print as not present. */
static const EnvRail u2_rails[] = {
    { "1.8V", 1800, 1777 },
    { "12V", 12000, 12063 },
    { "12V #2", 12000, 12063 },
    { "3.3V", 3300, 3337 },
    { "12V IO", 12000, 12063 },
    { "5V AUX", 5000, 5044 },
    { "3.3V AUX", 3300, 3285 },
    { "PCI 5V AUX", 5000, 5044 },
    { "PCI 3.3V", 3300, 3320 },
    { "PCI 2.5V", 2500, 2496 },
    { "PCI 5V", 5000, 4966 },
    { "XIO 12V BIAS", 0, 0 },
    { "XIO 5V", 0, 0 },
    { "XIO 2.5V", 0, 0 },
    { "XIO 3.3V AUX", 0, 0 },
};

/* The rack-mounted workstation has the 2U rails with its XIO rails fitted. */
static const EnvRail q_rails[] = {
    { "1.8V", 1800, 1777 },
    { "12V", 12000, 12063 },
    { "12V #2", 12000, 12063 },
    { "3.3V", 3300, 3337 },
    { "12V IO", 12000, 12063 },
    { "5V AUX", 5000, 5044 },
    { "3.3V AUX", 3300, 3285 },
    { "PCI 5V AUX", 5000, 5044 },
    { "PCI 3.3V", 3300, 3320 },
    { "PCI 2.5V", 2500, 2496 },
    { "PCI 5V", 5000, 4966 },
    { "XIO 12V BIAS", 12000, 11750 },
    { "XIO 5V", 5000, 4992 },
    { "XIO 2.5V", 2500, 2457 },
    { "XIO 3.3V AUX", 3300, 3285 },
};

/*
 * The node board's rails, shown with the board's name in front. Readings
 * are per generation. The VCPU nominal is filled in from the PSC code.
 */
static const EnvRail node_rails[2][6] = {
    [ENV_IP53] = {
        { "3.3V AUX", 3300, 3285 },
        { "5V AUX", 5000, 5044 },
        { "12V", 12000, 12000 },
        { "VCPU", 0, 1241 },
        { "SRAM", 2500, 2470 },
        { "1.5V", 1500, 1480 },
    },
    [ENV_IP59] = {
        { "3.3V AUX", 3300, 3302 },
        { "5V AUX", 5000, 4888 },
        { "12V", 12000, 11875 },
        { "VCPU", 0, 1269 },
        { "SRAM", 2500, 2457 },
        { "1.5V", 1500, 1495 },
    },
};

/* The IP53 and IP59 workstation fan lists differ by the N0 fans. */
static const EnvFan ws_fans[] = {
    { "NODE 1", 1800, { 2235, 2860 } },
    { "NODE 2", 1800, { 2191, 2860 } },
    { "NODE 3", 1800, { 2250, 2986 } },
    { "PCI 1", 1350, { 1548, 1454 } },
    { "PCI 2", 1350, { 1548, 1454 } },
    { "HD", 1620, { 3668, 3068 } },
    { "ODY 1", 1300, { 1704, 2481 } },
    { "ODY 2", 1300, { 1591, 2376 } },
    { "N0 LEFT", 1800, { 0, 2702 } },
    { "N0 CNTR", 1800, { 0, 2678 } },
    { "N0 RIGHT", 1800, { 0, 2803 } },
};
#define WS_FANS_IP53 8

/*
 * The IP53 2U list is the first five; the three N0 fans are IP59's. The
 * IP59 readings are captured; the IP53 ones are model values, since the
 * generations read differently. The exhaust fans' drive is inverted: low is
 * the fast end, 210 on IP53 and 190 on IP59.
 */
static const EnvFan u2_fans[] = {
    { "EXHST 1", 1980, { 2311, 2636 }, { 210, 0, 0 }, 190 },
    { "EXHST 2", 1980, { 2290, 2616 }, { 210, 0, 0 }, 190 },
    { "PS", 3200, { 3980, 4066 }, { 140, 255, 255 }, 0 },
    { "PCI 1", 1980, { 2440, 2556 }, { 158, 255, 255 }, 0 },
    { "PCI 2", 1980, { 2701, 2812 }, { 158, 255, 255 }, 0 },
    { "N0 LEFT", 1980, { 0, 4054 }, { 14, 31, 31 }, 0 },
    { "N0 CNTR", 1980, { 0, 3797 }, { 14, 31, 31 }, 0 },
    { "N0 RIGHT", 1980, { 0, 4109 }, { 14, 31, 31 }, 0 },
};
#define U2_FANS_IP53 5

/*
 * The rack-mounted workstation replaces the second exhaust fan with an
 * Odyssey fan, which warns at 1679 RPM in the captured module. The IP53 list
 * is the first five; the three N0 fans are IP59's. The current readings,
 * including the Odyssey fan's 1850 RPM, are model choices. The exhaust
 * fan's low level is 210 on both generations: the datasheet's 190 is the
 * IP59 2U row, and 210 is the level that gives the Q zone's 18 percent
 * minimum on IP59 as well. That departure is logged when Q prints.
 */
static const EnvFan q_fans[] = {
    { "EXHST 1", 1980, { 2636, 2636 }, { 210, 0, 0 }, 0 },
    { "PS", 3200, { 4066, 4066 }, { 140, 255, 255 }, 0 },
    { "PCI 1", 1980, { 2556, 2556 }, { 140, 255, 255 }, 0 },
    { "PCI 2", 1980, { 2812, 2812 }, { 140, 255, 255 }, 0 },
    { "ODY", 1679, { 1850, 1850 }, { 140, 255, 255 }, 0 },
    { "N0 LEFT", 1980, { 0, 4054 }, { 10, 31, 31 }, 0 },
    { "N0 CNTR", 1980, { 0, 3797 }, { 10, 31, 31 }, 0 },
    { "N0 RIGHT", 1980, { 0, 4109 }, { 10, 31, 31 }, 0 },
};
#define Q_FANS_IP53 5

/* The captured autofan faults are 76 C, and 85 C on the BEDROCK sensor. */
static const EnvTemp ws_temps[2][7] = {
    [ENV_IP53] = {
        { "INTERFACE 0", true, 36, false, &lim_9 },
        { "INTERFACE 1", true, 31, false, &lim_9 },
        { "INTERFACE 2", true, 31, false, &lim_9 },
        { "INTERFACE 3", true, 38, false, &lim_9 },
        { "ODYSSEY", true, 42, false, &lim_9 },
        { "NODE", true, 50, false, &lim_9 },
        { "BEDROCK", true, 50, true, &lim_9_node },
    },
    [ENV_IP59] = {
        { "INTERFACE 0", true, 40, false, &lim_9 },
        { "INTERFACE 1", true, 34, false, &lim_9 },
        { "INTERFACE 2", true, 34, false, &lim_9 },
        { "INTERFACE 3", true, 41, false, &lim_9 },
        { "ODYSSEY", true, 45, false, &lim_9 },
        { "NODE", true, 50, false, &lim_9 },
        { "BEDROCK", true, 47, true, &lim_9_node },
    },
};

static const EnvTemp u2_temps[] = {
    { "INTERFACE 0", true, 11, false, &lim_7 },
    { "INTERFACE 1", true, 11, false, &lim_7 },
    { "INTERFACE 2", true, 12, false, &lim_7 },
    { "PCI RISER", true, 16, false, &lim_7 },
    { "ODYSSEY", false, 0, false, &lim_7 },
    { "NODE", true, 14, false, &lim_7 },
    { "BEDROCK", true, 13, true, &lim_7 },
};

/* The rack-mounted workstation's captured faults are 75 C on every sensor. */
static const EnvTemp q_temps[2][7] = {
    [ENV_IP53] = {
        { "INTERFACE 0", true, 36, false, &lim_q },
        { "INTERFACE 1", true, 31, false, &lim_q },
        { "INTERFACE 2", true, 31, false, &lim_q },
        { "PCI RISER", true, 38, false, &lim_q },
        { "ODYSSEY", true, 42, false, &lim_q },
        { "NODE", true, 50, false, &lim_q },
        { "BEDROCK", true, 50, true, &lim_q },
    },
    [ENV_IP59] = {
        { "INTERFACE 0", true, 40, false, &lim_q },
        { "INTERFACE 1", true, 34, false, &lim_q },
        { "INTERFACE 2", true, 34, false, &lim_q },
        { "PCI RISER", true, 41, false, &lim_q },
        { "ODYSSEY", true, 45, false, &lim_q },
        { "NODE", true, 50, false, &lim_q },
        { "BEDROCK", true, 47, true, &lim_q },
    },
};

/* The cooling zones follow each workstation board's captured firmware. */
static const EnvZone ws_zones_ip53[] = {
    { "Node", "5,6", 62, ENV_ZONE_MEAN, "0", -1, 0, false, 46 },
    { "PCI", "0,1,2,3", 45, ENV_ZONE_MEAN, "3,4", -1, 0, false, 57 },
    { "ODY", "4", 50, ENV_ZONE_MEAN, "6", -1, 0, false, 64 },
    { "HD", "5", 40, ENV_ZONE_MEAN, "5", -1, 0, false, 38 },
};

/* The unused zones print N/A for sensors, target and average. */
static const EnvZone ws_zones_ip59[] = {
    { "NODE", "5,6", 53, ENV_ZONE_MEAN, "0", -1, 0, false, 46 },
    { "BOOST", "N/A", 0, 0, "8,9,10", -1, 0, false, 50 },
    { "HD", "N/A", 0, 0, "5", -1, 0, false, 38 },
    { "PCI", "0,1,2,3", 45, ENV_ZONE_MEAN, "3,4", -1, 0, false, 57 },
    { "ODY", "4", 50, ENV_ZONE_MEAN, "6", -1, 0, false, 64 },
};

/*
 * The rack-mounted workstation's zones: the datasheet's default table for
 * the 8 and Q personalities, which matches the captured modules. The
 * current percentage follows the zone's first fan's drive byte, so at idle
 * it equals the minimum. The NODE zone's exhaust fan is inverted.
 */
static const EnvZone q_zones[] = {
    { "NODE", "0,1,2,5,6", 47, ENV_ZONE_MEAN, "0", 0, 210, true, 0 },
    { "PS", "0,1,2,5,6", 47, ENV_ZONE_MEAN, "1", 1, 140, false, 0 },
    { "PCI", "3", 45, ENV_ZONE_MEAN, "2,3", 2, 140, false, 0 },
    { "ODY", "4", 48, ENV_ZONE_MEAN, "4", 4, 140, false, 0 },
};

static unsigned env_fahrenheit(unsigned celsius)
{
    return celsius * 9 / 5 + 32;
}

/*
 * "%14s %10s  " opens a supply row, then "%2d%% %6.2f/%6.2f" for each limit
 * pair and "%7.3f" for the reading. The limits are integer arithmetic so
 * that they round half up, as the captures do.
 */
static void env_supply_row(GString *out, const char *name, const char *state,
                           unsigned nominal_mv, unsigned reading_mv)
{
    if (!nominal_mv) {
        g_string_append_printf(out, "%*s <not present>\r\n",
                               ENV_NAME_WIDTH, name);
        return;
    }
    g_string_append_printf(out,
        "%*s %10s  %2d%% %3u.%02u/%3u.%02u  %2d%% %3u.%02u/%3u.%02u  %7.3f\r\n",
        ENV_NAME_WIDTH, name, state,
        ENV_WARN_PERCENT,
        env_limit_centivolts(nominal_mv, -ENV_WARN_PERCENT) / 100,
        env_limit_centivolts(nominal_mv, -ENV_WARN_PERCENT) % 100,
        env_limit_centivolts(nominal_mv, ENV_WARN_PERCENT) / 100,
        env_limit_centivolts(nominal_mv, ENV_WARN_PERCENT) % 100,
        ENV_FAULT_PERCENT,
        env_limit_centivolts(nominal_mv, -ENV_FAULT_PERCENT) / 100,
        env_limit_centivolts(nominal_mv, -ENV_FAULT_PERCENT) % 100,
        env_limit_centivolts(nominal_mv, ENV_FAULT_PERCENT) / 100,
        env_limit_centivolts(nominal_mv, ENV_FAULT_PERCENT) % 100,
        reading_mv / 1000.0);
}

static void env_fan_row(GString *out, unsigned index, const EnvFan *fan,
                        const char *state, unsigned reading_rpm)
{
    g_string_append_printf(out, "FAN %2u %8s %10s  %11u  %11u\r\n", index,
                           fan->name, state, fan->warning_rpm, reading_rpm);
}

/*
 * "%2d %-14s %10s  " opens a temperature row. A sensor under automatic fan
 * control shows "[Autofan Control]" and its maximum limit, one with
 * advisory and critical checking off shows "Disabled" for them and its
 * maximum, and one that cannot be read shows that it is unavailable.
 */
static void env_temp_row(GString *out, unsigned index, const EnvTemp *temp,
                         const char *state, EnvTempMode mode, bool available,
                         unsigned reading_c, const SGIL1EnvSettings *settings)
{
    EnvLimits l = *temp->limits;

    /*
     * Custom limits replace the defaults and high altitude lowers them,
     * except for the workstation's fixed on-node row.
     */
    if (settings->custom_limits && temp->limits != &lim_9_node) {
        l = (EnvLimits) { settings->custom[0], settings->custom[1],
                          settings->custom[2], settings->custom[3] };
    }
    if (settings->altitude_high && temp->limits != &lim_9_node) {
        l.advisory -= 3;
        l.critical -= 4;
        l.fault -= 5;
        l.maximum -= 5;
    }
    if (!temp->present) {
        g_string_append_printf(out, "%2u %-14s <not present>\r\n", index,
                               temp->name);
        return;
    }
    g_string_append_printf(out, "%2u %-14s %10s  ", index, temp->name, state);
    if (!available) {
        g_string_append(out, "Not currently available\r\n");
        return;
    }
    switch (mode) {
    case ENV_TEMP_AUTOFAN:
        g_string_append_printf(out, "  [Autofan Control]   %3uC/%3uF  ",
                               l.maximum, env_fahrenheit(l.maximum));
        break;
    case ENV_TEMP_CHECKS_OFF:
        g_string_append_printf(out, " Disabled   Disabled  %3uC/%3uF  ",
                               l.maximum, env_fahrenheit(l.maximum));
        break;
    default:
        g_string_append_printf(out, "%3uC/%3uF  %3uC/%3uF  %3uC/%3uF  ",
                               l.advisory, env_fahrenheit(l.advisory),
                               l.critical, env_fahrenheit(l.critical),
                               l.fault, env_fahrenheit(l.fault));
        break;
    }
    g_string_append_printf(out, "%3uC/%3uF\r\n", reading_c,
                           env_fahrenheit(reading_c));
}

/* The row layout reproduces the captured zone rows. */
static void env_zone_row(GString *out, const EnvZone *zone, const char *state,
                         unsigned current_c, unsigned percent,
                         unsigned min_percent)
{
    g_autofree char *target = zone->target_c ?
        g_strdup_printf("%2uC/%3uF", zone->target_c,
                        env_fahrenheit(zone->target_c)) :
        g_strdup("N/A");
    g_autofree char *current = zone->target_c ?
        g_strdup_printf("%2uC/%3uF", current_c,
                        env_fahrenheit(current_c)) :
        g_strdup("N/A");

    g_string_append_printf(out,
        "%-10s %8s  %12s  %8s  %8s  %9s  %3u%%/%3u%%\r\n",
        zone->name, state, zone->sensors, target, current, zone->fans,
        percent, min_percent);
}

/*
 * A row's state: Disabled when monitoring is off, else Wait Pwr with no node
 * power, else Enabled. The datasheet gives the order of Fault, Warning,
 * Enabled, Wait Pwr and Disabled but not the monitor state that "env off"
 * leaves a row in, so Disabled over Wait Pwr is a model choice.
 */
static const char *env_state(const SGIL1EnvSettings *settings, bool powered)
{
    if (settings->disabled) {
        return "Disabled";
    }
    return powered ? "Enabled" : ENV_WAIT_POWER;
}

static void env_rails(GString *out, const EnvRail *rails, unsigned count,
                      EnvBoard board, bool powered, const char *state,
                      const SGIL1EnvSettings *settings)
{
    const char *generation = board == ENV_IP59 ? "IP59 " : "IP53 ";
    bool monitor_only = settings->power_mode == 2;

    /* "env power off" clears the state of every supply row. */
    if (settings->power_mode == 1) {
        state = "Disabled";
    }

    if (monitor_only) {
        g_string_append(out, ENV_MONITOR_ONLY);
    }
    g_string_append(out,
        "Description    State       Warning Limits     "
        "Fault Limits       Current\r\n"
        "-------------- ----------  -----------------  "
        "-----------------  -------\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_supply_row(out, rails[i].name, state, rails[i].nominal_mv,
                       powered ? rails[i].reading_mv : 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(node_rails[board]); i++) {
        EnvRail rail = node_rails[board][i];
        g_autofree char *name = NULL;

        if (!strcmp(rail.name, "VCPU")) {
            rail.nominal_mv = env_vcpu_nominal_mv(env_psc_code(board));
        }
        name = g_strdup_printf("%s%s", generation, rail.name);
        env_supply_row(out, name, state, rail.nominal_mv,
                       powered ? rail.reading_mv : 0);
    }
    if (monitor_only) {
        g_string_append(out, ENV_MONITOR_ONLY);
    }
}

static void env_fans(GString *out, const EnvFan *fans, unsigned count,
                     EnvBoard board, bool powered, const char *state,
                     bool fan_auto_off)
{
    g_string_append(out,
        "\r\n"
        "Description     State       Warning RPM  Current RPM\r\n"
        "--------------- ----------  -----------  -----------\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_fan_row(out, i, &fans[i], state,
                    powered ? fans[i].reading_rpm[board] : 0);
    }
    if (fan_auto_off) {
        g_string_append(out, "Automatic fan control is disabled\r\n");
    }
}

static void env_temps(GString *out, const EnvTemp *temps, unsigned count,
                      EnvTempMode mode, bool powered, const char *state,
                      const SGIL1EnvSettings *settings)
{
    g_string_append(out,
        "\r\n"
        "                              Advisory   Critical   Fault      "
        "Current\r\n"
        "Description       State       Temp       Temp       Temp       "
        "Temp\r\n"
        "----------------- ----------  ---------  ---------  ---------  "
        "---------\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_temp_row(out, i, &temps[i], state, mode,
                     powered || !temps[i].on_node,
                     powered ? temps[i].reading_c : 0, settings);
    }
}

/* A fan's drive byte: an operator's write, else the monitor's low level. */
static unsigned env_fan_drive(const EnvFan *fans, unsigned index,
                              EnvBoard board, const SGIL1EnvSettings *settings)
{
    if (settings->fan_override_mask & (1u << index)) {
        return settings->fan_override[index];
    }
    if (board == ENV_IP59 && fans[index].low_ip59) {
        return fans[index].low_ip59;
    }
    return fans[index].level[0];
}

/* 100 x value / full scale, from the inverted end for an inverted zone. */
static unsigned env_drive_percent(unsigned value, bool inverted)
{
    unsigned v = inverted ? 255 - value : value;

    return (unsigned)(100.0 * v / 255 + 0.5);
}

/* The mean of a zone's sensor readings, truncated. */
static unsigned env_zone_mean(const EnvZone *zone, const EnvTemp *temps)
{
    unsigned sum = 0, count = 0;

    for (const char *p = zone->sensors; *p; p++) {
        if (g_ascii_isdigit(*p)) {
            sum += temps[*p - '0'].reading_c;
            count++;
        }
    }
    return count ? sum / count : 0;
}

static void env_zones(GString *out, const EnvZone *zones, unsigned count,
                      const EnvTemp *temps, bool powered, const char *state,
                      const EnvFan *fans, EnvBoard board,
                      const SGIL1EnvSettings *settings)
{
    if (!powered) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: env zone rows with the node "
                      "off are not recorded; their averages use the "
                      "static sensor values\n");
    }
    g_string_append(out,
        "\r\n"
        "                    Zone Temp     Target    Current   Zone Fan   "
        "Curr/Min\r\n"
        "Zone Name  State     Sensors       Average   Average   Index      "
        "Fan %\r\n"
        "---------  --------  ------------  --------  --------  ---------  "
        "---------\r\n");
    if (count && zones[0].fan_index < 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: workstation zones print their "
                      "fixed minimum as the current fan percentage; the "
                      "fan-to-channel mapping is not recorded\n");
    }
    for (unsigned i = 0; i < count; i++) {
        const EnvZone *z = &zones[i];
        unsigned min = z->min_percent;
        unsigned cur = min;

        if (z->fan_index >= 0) {
            min = env_drive_percent(z->min_byte, z->inverted);
            cur = env_drive_percent(env_fan_drive(fans, z->fan_index, board,
                                                  settings), z->inverted);
        }
        env_zone_row(out, z, state,
                     z->current_c == ENV_ZONE_MEAN ?
                     env_zone_mean(z, temps) : z->current_c, cur, min);
    }
}

/*
 * The board generation: a product name of at least six characters that
 * starts "IP5", then 3 for IP53 or 9 for IP59.
 */
static bool env_board_from_name(const char *name, EnvBoard *board)
{
    if (!name || strlen(name) < 6 || strncmp(name, "IP5", 3)) {
        return false;
    }
    if (name[3] == '3') {
        *board = ENV_IP53;
        return true;
    }
    if (name[3] == '9') {
        *board = ENV_IP59;
        return true;
    }
    return false;
}

bool sgi_l1_chimera_has_autofan(char type)
{
    /* The workstation letters; 8, 0 and A have the command but no report. */
    return strchr("9Q80A", type) != NULL;
}

static const char *const env_failsafe_banner =
    "\r\n"
    "!!!!!!!!!!!!!!!!  WARNING  !!!!!!!!!!!!!!!!!!!!!\r\n"
    "ALL TEMPERATURE MONITORING FAILSAFES DISABLED!!\r\n"
    "(to re-enable, use \"env on\" or \"env temp on\")\r\n"
    "!!!!!!!!!!!!!!!!  WARNING  !!!!!!!!!!!!!!!!!!!!!\r\n";

/*
 * The banner with monitoring off, or the status line with it on. The
 * firmware prints the banners again after the tables. The failsafe banner
 * is printed only inside the monitoring-off branch, so "env temp off" with
 * monitoring on prints only the status line.
 */
static void env_status(GString *out, const SGIL1EnvSettings *settings)
{
    if (settings->disabled) {
        g_string_append(out,
            "\r\n"
            "************************************************\r\n"
            "ATTENTION: Environmental monitoring is disabled!\r\n"
            "************************************************\r\n");
        if (settings->temp_off) {
            g_string_append(out, env_failsafe_banner);
        }
    } else {
        g_string_append(out,
            "Environmental monitoring is enabled and running.\r\n");
    }
}

char *sgi_l1_chimera_env(char type, const char *node_board, bool powered,
                         const SGIL1EnvSettings *settings, bool full)
{
    EnvBoard board;
    GString *out;
    bool autofan = type == '9' || type == 'Q';
    bool fan_auto_off = autofan && settings->fan_auto_off;
    const char *state = env_state(settings, powered);
    EnvTempMode mode;

    if ((type != '7' && type != '9' && type != 'Q') ||
        !env_board_from_name(node_board, &board)) {
        return NULL;
    }
    out = g_string_new(NULL);
    env_status(out, settings);
    if (!full) {
        return g_string_free(out, FALSE);
    }
    /*
     * The order of the row forms: an on-node sensor with no power, then
     * automatic fan control running, then temperature checking off or no
     * running monitor, then the full form. That the flag tested for the
     * autofan form is the fan-control state is inferred.
     */
    if (autofan && !fan_auto_off) {
        mode = ENV_TEMP_AUTOFAN;
    } else if (settings->temp_off || settings->disabled) {
        mode = ENV_TEMP_CHECKS_OFF;
    } else {
        mode = ENV_TEMP_FULL;
    }
    if (settings->disabled) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: rows under \"env off\" are "
                      "printed as Disabled with their values; the monitor "
                      "state they are in is inferred\n");
    }
    if (type == 'Q') {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: rack-mounted workstation "
                      "readings are model values; its exhaust fan's low "
                      "level is 210 on IP59 as well (the 2U row gives "
                      "190); the sub-type 4 zone tables are not "
                      "modelled\n");
    }
    if (type == '7' && board == ENV_IP53) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: IP53 2U fan readings are model "
                      "values, and its supply and temperature readings are "
                      "the IP59 2U capture's\n");
    }
    if (board == ENV_IP59) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: the IP59 NODE FANS temperature "
                      "row is registered by the firmware but not printed "
                      "here\n");
    }
    if (fan_auto_off) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: the zone state with automatic "
                      "fan control off was not traced; it prints as "
                      "usual\n");
    }
    g_string_append(out, "\r\n");
    if (type == '9') {
        unsigned fans = board == ENV_IP59 ? ARRAY_SIZE(ws_fans) : WS_FANS_IP53;

        env_rails(out, ws_rails, ARRAY_SIZE(ws_rails), board, powered, state,
                  settings);
        env_fans(out, ws_fans, fans, board, powered, state, fan_auto_off);
        env_temps(out, ws_temps[board], ARRAY_SIZE(ws_temps[board]), mode,
                  powered, state, settings);
        if (board == ENV_IP59) {
            env_zones(out, ws_zones_ip59, ARRAY_SIZE(ws_zones_ip59),
                      ws_temps[board], powered, state, ws_fans, board,
                      settings);
        } else {
            env_zones(out, ws_zones_ip53, ARRAY_SIZE(ws_zones_ip53),
                      ws_temps[board], powered, state, ws_fans, board,
                      settings);
        }
    } else if (type == 'Q') {
        unsigned fans = board == ENV_IP59 ? ARRAY_SIZE(q_fans) : Q_FANS_IP53;

        env_rails(out, q_rails, ARRAY_SIZE(q_rails), board, powered, state,
                  settings);
        env_fans(out, q_fans, fans, board, powered, state, fan_auto_off);
        env_temps(out, q_temps[board], ARRAY_SIZE(q_temps[board]), mode,
                  powered, state, settings);
        env_zones(out, q_zones, ARRAY_SIZE(q_zones), q_temps[board], powered,
                  state, q_fans, board, settings);
    } else {
        unsigned fans = board == ENV_IP59 ? ARRAY_SIZE(u2_fans) : U2_FANS_IP53;

        env_rails(out, u2_rails, ARRAY_SIZE(u2_rails), board, powered, state,
                  settings);
        env_fans(out, u2_fans, fans, board, powered, state, false);
        env_temps(out, u2_temps, ARRAY_SIZE(u2_temps), mode, powered, state,
                  settings);
    }
    if (settings->disabled) {
        env_status(out, settings);
    }
    return g_string_free(out, FALSE);
}

/* The fan table of a type, or NULL. Only 7 and Q have drive levels. */
static const EnvFan *env_fan_table(char type, EnvBoard board, unsigned *count)
{
    if (type == '9') {
        *count = board == ENV_IP59 ? ARRAY_SIZE(ws_fans) : WS_FANS_IP53;
        return ws_fans;
    } else if (type == 'Q') {
        *count = board == ENV_IP59 ? ARRAY_SIZE(q_fans) : Q_FANS_IP53;
        return q_fans;
    } else if (type == '7') {
        *count = board == ENV_IP59 ? ARRAY_SIZE(u2_fans) : U2_FANS_IP53;
        return u2_fans;
    }
    return NULL;
}

char *sgi_l1_chimera_fan(char type, const char *node_board, bool powered,
                         const SGIL1EnvSettings *settings, bool speed)
{
    EnvBoard board;
    const EnvFan *fans;
    unsigned count;
    GString *out;

    if (!env_board_from_name(node_board, &board)) {
        return NULL;
    }
    fans = env_fan_table(type, board, &count);
    if (!fans) {
        return NULL;
    }
    if (speed && type == '9') {
        /* The workstation's fan-to-channel mapping was not traced. */
        return NULL;
    }
    out = g_string_new(NULL);
    if (!speed) {
        g_string_append_printf(out, "fan(s) are %s.\r\n",
                               settings->fans_off ? "off" : "on");
    }
    for (unsigned i = 0; i < count; i++) {
        if (speed) {
            g_string_append_printf(out, "fan %d %-8s speed: %d\r\n", i,
                                   fans[i].name,
                                   env_fan_drive(fans, i, board, settings));
        } else {
            g_string_append_printf(out, "fan %d %-8s rpm %d\r\n", i,
                                   fans[i].name,
                                   powered ? fans[i].reading_rpm[board] : 0);
        }
    }
    return g_string_free(out, FALSE);
}

bool sgi_l1_chimera_fan_set(char type, const char *node_board,
                            SGIL1EnvSettings *settings, int fan, int level,
                            unsigned value)
{
    EnvBoard board;
    const EnvFan *fans;
    unsigned count;

    if (type == '9' || !env_board_from_name(node_board, &board)) {
        return false;
    }
    fans = env_fan_table(type, board, &count);
    if (!fans || (fan >= 0 && (unsigned)fan >= count) || value > 255) {
        return false;
    }
    /* The N0 fans' channels take 0 to 31; the others take 0 to 255. */
    if (level < 0 && fan >= 0 && !strncmp(fans[fan].name, "N0", 2) &&
        value > 31) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        if (fan >= 0 && (unsigned)fan != i) {
            continue;
        }
        if (level == 0) {
            settings->fan_override_mask &= ~(1u << i);
            continue;
        }
        settings->fan_override_mask |= 1u << i;
        settings->fan_override[i] = level > 0 ? fans[i].level[level] : value;
    }
    return true;
}

/*
 * The "power" table's per-supply facts: whether it has an enable bit on an
 * I/O expander (otherwise it prints NC) and a margin channel (otherwise N/A).
 * The workstation and the 2U lists differ, and Q uses the 2U list.
 */
static void env_power_facts(bool ws, const char *name, bool *enable,
                            bool *margin)
{
    static const char *const u2_enable[] = {
        "1.8V", "12V", "12V #2", "PCI 2.5V", "PCI 5V", "XIO 2.5V",
        "VCPU", "SRAM", "1.5V",
    };
    static const char *const u2_margin[] = {
        "1.8V", "3.3V", "PCI 2.5V", "PCI 5V", "XIO 2.5V", "VCPU", "SRAM",
        "1.5V",
    };
    static const char *const ws_enable[] = {
        "1.8V", "12V", "2.5V", "XIO 2.5V", "VCPU", "SRAM", "1.5V",
    };
    static const char *const ws_margin[] = {
        "1.8V", "3.3V", "2.5V", "XIO 2.5V", "VCPU", "SRAM", "1.5V",
    };
    const char *const *en = ws ? ws_enable : u2_enable;
    unsigned en_n = ws ? ARRAY_SIZE(ws_enable) : ARRAY_SIZE(u2_enable);
    const char *const *mg = ws ? ws_margin : u2_margin;
    unsigned mg_n = ws ? ARRAY_SIZE(ws_margin) : ARRAY_SIZE(u2_margin);

    /*
     * The node board's rows carry the board name in front. Only its VCPU,
     * SRAM and 1.5V have an enable bit and a margin; its 3.3V AUX, 5V AUX
     * and 12V are NC with no margin, unlike the module's own 12V.
     */
    if (!strncmp(name, "IP5", 3)) {
        name += 5;
        *enable = *margin = !strcmp(name, "VCPU") || !strcmp(name, "SRAM") ||
                            !strcmp(name, "1.5V");
        return;
    }
    *enable = *margin = false;
    for (unsigned i = 0; i < en_n; i++) {
        *enable |= !strcmp(name, en[i]);
    }
    for (unsigned i = 0; i < mg_n; i++) {
        *margin |= !strcmp(name, mg[i]);
    }
}

static void env_power_row(GString *out, bool ws, const char *name,
                          unsigned nominal_mv, unsigned reading_mv,
                          bool powered)
{
    bool enable, margin;

    if (!nominal_mv) {
        g_string_append_printf(out, "%14.14s     <not present>\r\n", name);
        return;
    }
    env_power_facts(ws, name, &enable, &margin);
    g_string_append_printf(out, "%14.14s  %5.5s %8.3fV  ", name,
                           !enable ? "NC" : powered ? "on" : "off",
                           powered ? reading_mv / 1000.0 : 0.0);
    if (margin) {
        g_string_append_printf(out, "%7.7s %5d\r\n", "default", 0);
    } else {
        g_string_append(out, "    N/A\r\n");
    }
}

char *sgi_l1_chimera_power(char type, const char *node_board, bool powered)
{
    EnvBoard board;
    const EnvRail *rails;
    unsigned count;
    bool ws = type == '9';
    GString *out;

    if ((type != '7' && type != '9' && type != 'Q') ||
        !env_board_from_name(node_board, &board)) {
        return NULL;
    }
    rails = ws ? ws_rails : type == 'Q' ? q_rails : u2_rails;
    count = ws ? ARRAY_SIZE(ws_rails) :
            type == 'Q' ? ARRAY_SIZE(q_rails) : ARRAY_SIZE(u2_rails);
    qemu_log_mask(LOG_UNIMP, "sgi-l1: power: the rule under the header and "
                  "the margin value (0) are model choices\n");
    out = g_string_new(
        "Supply          State Voltage    Margin  Value\r\n"
        "--------------  ----- ---------  -------  -----\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_power_row(out, ws, rails[i].name, rails[i].nominal_mv,
                      rails[i].reading_mv, powered);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(node_rails[board]); i++) {
        EnvRail rail = node_rails[board][i];
        g_autofree char *name = NULL;

        if (!strcmp(rail.name, "VCPU")) {
            rail.nominal_mv = env_vcpu_nominal_mv(env_psc_code(board));
        }
        name = g_strdup_printf("%s%s", board == ENV_IP59 ? "IP59 " : "IP53 ",
                               rail.name);
        env_power_row(out, ws, name, rail.nominal_mv, rail.reading_mv,
                      powered);
    }
    return g_string_free(out, FALSE);
}
