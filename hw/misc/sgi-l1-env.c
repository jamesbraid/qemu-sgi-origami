/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 console's "env" report for the Chimera node boards.
 *
 * The table layout, names, states and limits follow the reviewed L1 and
 * IP59 datasheets and the console captures they cite. The firmware has one
 * copy of each row format, shared by every brick personality, so each row
 * kind below has one printer and the tables only supply data. Every
 * reading is model state: a fixed, in-limits snapshot taken from one
 * captured session, not a measurement, so a guest or a test sees the same
 * value each time. The node's own voltages, fans and temperatures are not
 * simulated.
 */
#include "qemu/osdep.h"
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

typedef struct EnvRail {
    const char *name;
    unsigned nominal_mv;
    unsigned reading_mv;
} EnvRail;

typedef struct EnvFan {
    const char *name;
    unsigned warning_rpm;
    unsigned reading_rpm[2];
} EnvFan;

typedef struct EnvTemp {
    const char *name;
    bool present;
    unsigned reading_c;
    /* A sensor on the node cannot be read while the node has no power. */
    bool on_node;
} EnvTemp;

typedef struct EnvZone {
    const char *name;
    const char *sensors;
    /* A zero target prints N/A for both temperatures. */
    unsigned target_c;
    unsigned current_c;
    const char *fans;
    unsigned fan_percent;
    unsigned fan_min_percent;
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
 * The IP53 2U list is the first five; the three N0 fans are IP59's. No
 * IP53 2U console was captured, so the IP53 readings reuse the IP59 ones.
 */
static const EnvFan u2_fans[] = {
    { "EXHST 1", 1980, { 2636, 2636 } },
    { "EXHST 2", 1980, { 2616, 2616 } },
    { "PS", 3200, { 4066, 4066 } },
    { "PCI 1", 1980, { 2556, 2556 } },
    { "PCI 2", 1980, { 2812, 2812 } },
    { "N0 LEFT", 1980, { 0, 4054 } },
    { "N0 CNTR", 1980, { 0, 3797 } },
    { "N0 RIGHT", 1980, { 0, 4109 } },
};
#define U2_FANS_IP53 5

static const EnvTemp ws_temps[2][7] = {
    [ENV_IP53] = {
        { "INTERFACE 0", true, 36 }, { "INTERFACE 1", true, 31 },
        { "INTERFACE 2", true, 31 }, { "INTERFACE 3", true, 38 },
        { "ODYSSEY", true, 42 }, { "NODE", true, 50 },
        { "BEDROCK", true, 50, true },
    },
    [ENV_IP59] = {
        { "INTERFACE 0", true, 40 }, { "INTERFACE 1", true, 34 },
        { "INTERFACE 2", true, 34 }, { "INTERFACE 3", true, 41 },
        { "ODYSSEY", true, 45 }, { "NODE", true, 50 },
        { "BEDROCK", true, 47, true },
    },
};

static const EnvTemp u2_temps[] = {
    { "INTERFACE 0", true, 11 }, { "INTERFACE 1", true, 11 },
    { "INTERFACE 2", true, 12 }, { "PCI RISER", true, 16 },
    { "ODYSSEY", false, 0 }, { "NODE", true, 14 },
    { "BEDROCK", true, 13, true },
};

/* The cooling zones follow each workstation board's captured firmware. */
static const EnvZone ws_zones_ip53[] = {
    { "Node", "5,6", 62, 50, "0", 46, 46 },
    { "PCI", "0,1,2,3", 45, 34, "3,4", 57, 57 },
    { "ODY", "4", 50, 42, "6", 64, 64 },
    { "HD", "5", 40, 50, "5", 66, 38 },
};

static const EnvZone ws_zones_ip59[] = {
    { "NODE", "5,6", 53, 48, "0", 62, 46 },
    { "BOOST", "N/A", 0, 0, "8,9,10", 64, 50 },
    { "HD", "N/A", 0, 0, "5", 56, 38 },
    { "PCI", "0,1,2,3", 45, 37, "3,4", 57, 57 },
    { "ODY", "4", 50, 45, "6", 82, 64 },
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
 * control shows only its fault limit, and one that cannot be read shows
 * that it is unavailable.
 */
static void env_temp_row(GString *out, unsigned index, const EnvTemp *temp,
                         const char *state, bool autofan, bool available,
                         unsigned fault_c, unsigned reading_c)
{
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
    if (autofan) {
        g_string_append_printf(out, "  [Autofan Control]   %3uC/%3uF  ",
                               fault_c, env_fahrenheit(fault_c));
    } else {
        g_string_append_printf(out, "%3uC/%3uF  %3uC/%3uF  %3uC/%3uF  ",
                               31, env_fahrenheit(31),
                               48, env_fahrenheit(48),
                               fault_c, env_fahrenheit(fault_c));
    }
    g_string_append_printf(out, "%3uC/%3uF\r\n", reading_c,
                           env_fahrenheit(reading_c));
}

/* The row layout reproduces the captured zone rows. */
static void env_zone_row(GString *out, const EnvZone *zone, const char *state)
{
    g_autofree char *target = zone->target_c ?
        g_strdup_printf("%2uC/%3uF", zone->target_c,
                        env_fahrenheit(zone->target_c)) :
        g_strdup("N/A");
    g_autofree char *current = zone->target_c ?
        g_strdup_printf("%2uC/%3uF", zone->current_c,
                        env_fahrenheit(zone->current_c)) :
        g_strdup("N/A");

    g_string_append_printf(out,
        "%-10s  %-8s%13s  %8s  %8s  %9s  %3u%%/%3u%%\r\n",
        zone->name, state, zone->sensors, target, current, zone->fans,
        zone->fan_percent, zone->fan_min_percent);
}

static void env_rails(GString *out, const EnvRail *rails, unsigned count,
                      EnvBoard board, bool powered)
{
    const char *generation = board == ENV_IP59 ? "IP59 " : "IP53 ";
    const char *state = powered ? "Enabled" : ENV_WAIT_POWER;

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
}

static void env_fans(GString *out, const EnvFan *fans, unsigned count,
                     EnvBoard board, bool powered)
{
    g_string_append(out,
        "\r\n"
        "Description     State       Warning RPM  Current RPM\r\n"
        "--------------- ----------  -----------  -----------\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_fan_row(out, i, &fans[i], powered ? "Enabled" : ENV_WAIT_POWER,
                    powered ? fans[i].reading_rpm[board] : 0);
    }
}

static void env_temps(GString *out, const EnvTemp *temps, unsigned count,
                      bool autofan, bool powered)
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
        bool bedrock = !strcmp(temps[i].name, "BEDROCK");
        /* The captured autofan faults are 76 C, and 85 C on the BEDROCK. */
        unsigned fault_c = autofan ? (bedrock ? 85 : 76) : 55;

        env_temp_row(out, i, &temps[i], powered ? "Enabled" : ENV_WAIT_POWER,
                     autofan, powered || !temps[i].on_node, fault_c,
                     powered ? temps[i].reading_c : 0);
    }
}

static void env_zones(GString *out, const EnvZone *zones, unsigned count,
                      bool powered)
{
    g_string_append(out,
        "\r\n"
        "                    Zone Temp     Target    Current   Zone Fan   "
        "Curr/Min\r\n"
        "Zone Name  State     Sensors       Average   Average   Index      "
        "Fan %\r\n"
        "---------  --------  ------------  --------  --------  ---------  "
        "---------\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_zone_row(out, &zones[i], powered ? "Enabled" : ENV_WAIT_POWER);
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

char *sgi_l1_chimera_env(char type, const char *node_board, bool powered)
{
    EnvBoard board;
    GString *out;

    if ((type != '7' && type != '9') ||
        !env_board_from_name(node_board, &board)) {
        return NULL;
    }
    out = g_string_new("Environmental monitoring is enabled and running.\r\n"
                       "\r\n");
    if (type == '9') {
        env_rails(out, ws_rails, ARRAY_SIZE(ws_rails), board, powered);
        env_fans(out, ws_fans,
                 board == ENV_IP59 ? ARRAY_SIZE(ws_fans) : WS_FANS_IP53,
                 board, powered);
        env_temps(out, ws_temps[board], ARRAY_SIZE(ws_temps[board]), true,
                  powered);
        if (board == ENV_IP59) {
            env_zones(out, ws_zones_ip59, ARRAY_SIZE(ws_zones_ip59), powered);
        } else {
            env_zones(out, ws_zones_ip53, ARRAY_SIZE(ws_zones_ip53), powered);
        }
    } else {
        env_rails(out, u2_rails, ARRAY_SIZE(u2_rails), board, powered);
        env_fans(out, u2_fans,
                 board == ENV_IP59 ? ARRAY_SIZE(u2_fans) : U2_FANS_IP53,
                 board, powered);
        env_temps(out, u2_temps, ARRAY_SIZE(u2_temps), false, powered);
    }
    return g_string_free(out, FALSE);
}
