/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 console's "env" report for the Chimera node boards.
 *
 * The table layout, names, states and limits follow the reviewed IP59
 * datasheet and the console captures it cites. Every reading is model
 * state: a fixed, in-limits snapshot taken from one captured session, not
 * a measurement, so a guest or a test sees the same value each time. The
 * node's own voltages, fans and temperatures are not simulated.
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

/*
 * The captures right-align the supply names one column apart: the
 * workstation board in 13 columns, the 2U board in 14.
 */
#define ENV_WS_NAME_WIDTH 13
#define ENV_2U_NAME_WIDTH 14

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
} EnvTemp;

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

/* A limit in tenths of a hundredth of a volt, rounded half up. */
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
        { "BEDROCK", true, 50 },
    },
    [ENV_IP59] = {
        { "INTERFACE 0", true, 40 }, { "INTERFACE 1", true, 34 },
        { "INTERFACE 2", true, 34 }, { "INTERFACE 3", true, 41 },
        { "ODYSSEY", true, 45 }, { "NODE", true, 50 },
        { "BEDROCK", true, 47 },
    },
};

static const EnvTemp u2_temps[] = {
    { "INTERFACE 0", true, 11 }, { "INTERFACE 1", true, 11 },
    { "INTERFACE 2", true, 12 }, { "PCI RISER", true, 16 },
    { "ODYSSEY", false, 0 }, { "NODE", true, 14 },
    { "BEDROCK", true, 13 },
};

/* The cooling zones follow each workstation board's captured firmware. */
static const char *const ws_zones[2] = {
    [ENV_IP53] =
    "Node        Enabled"
    "           5,6  62C/143F  50C/122F          0   46%/ 46%\r\n"
    "PCI         Enabled"
    "       0,1,2,3  45C/113F  34C/ 93F        3,4   57%/ 57%\r\n"
    "ODY         Enabled"
    "             4  50C/122F  42C/107F          6   64%/ 64%\r\n"
    "HD          Enabled"
    "             5  40C/104F  50C/122F          5   66%/ 38%\r\n",
    [ENV_IP59] =
    "NODE        Enabled"
    "           5,6  53C/127F  48C/118F          0   62%/ 46%\r\n"
    "BOOST       Enabled"
    "           N/A       N/A       N/A     8,9,10   64%/ 50%\r\n"
    "HD          Enabled"
    "           N/A       N/A       N/A          5   56%/ 38%\r\n"
    "PCI         Enabled"
    "       0,1,2,3  45C/113F  37C/ 98F        3,4   57%/ 57%\r\n"
    "ODY         Enabled"
    "             4  50C/122F  45C/113F          6   82%/ 64%\r\n",
};

static unsigned env_fahrenheit(unsigned celsius)
{
    return celsius * 9 / 5 + 32;
}

static void env_rail(GString *out, unsigned width, const char *prefix,
                     const EnvRail *rail, bool powered)
{
    g_autofree char *name = g_strdup_printf("%s%s", prefix, rail->name);
    unsigned nominal = rail->nominal_mv;

    if (!nominal) {
        g_string_append_printf(out, "%*s <not present>\r\n", width, name);
        return;
    }
    g_string_append_printf(out,
        "%*s %10s  %d%% %3u.%02u/%3u.%02u  %d%% %3u.%02u/%3u.%02u  %7.3f\r\n",
        width, name, powered ? "Enabled" : ENV_WAIT_POWER,
        ENV_WARN_PERCENT,
        env_limit_centivolts(nominal, -ENV_WARN_PERCENT) / 100,
        env_limit_centivolts(nominal, -ENV_WARN_PERCENT) % 100,
        env_limit_centivolts(nominal, ENV_WARN_PERCENT) / 100,
        env_limit_centivolts(nominal, ENV_WARN_PERCENT) % 100,
        ENV_FAULT_PERCENT,
        env_limit_centivolts(nominal, -ENV_FAULT_PERCENT) / 100,
        env_limit_centivolts(nominal, -ENV_FAULT_PERCENT) % 100,
        env_limit_centivolts(nominal, ENV_FAULT_PERCENT) / 100,
        env_limit_centivolts(nominal, ENV_FAULT_PERCENT) % 100,
        powered ? rail->reading_mv / 1000.0 : 0.0);
}

static void env_rails(GString *out, unsigned width, const EnvRail *rails,
                      unsigned count, EnvBoard board, bool powered)
{
    const char *generation = board == ENV_IP59 ? "IP59 " : "IP53 ";

    g_string_append(out,
        "Description    State       Warning Limits     "
        "Fault Limits       Current\r\n"
        "-------------- ----------  -----------------  "
        "-----------------  -------\r\n");
    for (unsigned i = 0; i < count; i++) {
        env_rail(out, width, "", &rails[i], powered);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(node_rails[board]); i++) {
        EnvRail rail = node_rails[board][i];

        if (!strcmp(rail.name, "VCPU")) {
            rail.nominal_mv = env_vcpu_nominal_mv(env_psc_code(board));
        }
        env_rail(out, width, generation, &rail, powered);
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
        g_string_append_printf(out, "FAN %2u %8s %10s %12u %12u\r\n", i,
                               fans[i].name,
                               powered ? "Enabled" : ENV_WAIT_POWER,
                               fans[i].warning_rpm,
                               powered ? fans[i].reading_rpm[board] : 0);
    }
}

static void env_temp_current(GString *out, bool powered, unsigned celsius)
{
    unsigned c = powered ? celsius : 0;

    g_string_append_printf(out, "%2uC/%3uF\r\n", c, env_fahrenheit(c));
}

static void env_ws_temps(GString *out, EnvBoard board, bool powered)
{
    g_string_append(out,
        "\r\n"
        "                             Advisory   Critical   Fault      "
        "Current\r\n"
        "Description       State       Temp       Temp       Temp       "
        "Temp\r\n"
        "----------------- ----------  ---------  ---------  ---------  "
        "---------\r\n");
    for (unsigned i = 0; i < ARRAY_SIZE(ws_temps[board]); i++) {
        const EnvTemp *t = &ws_temps[board][i];
        bool bedrock = i == ARRAY_SIZE(ws_temps[board]) - 1;

        g_string_append_printf(out, "%u %-14s %10s    [Autofan Control]    "
                               "%uC/%uF   ", i, t->name,
                               powered ? "Enabled" : ENV_WAIT_POWER,
                               bedrock ? 85 : 76,
                               env_fahrenheit(bedrock ? 85 : 76));
        env_temp_current(out, powered, t->reading_c);
    }
    g_string_append(out,
        "\r\n"
        "                    Zone Temp     Target    Current   Zone Fan   "
        "Curr/Min\r\n"
        "Zone Name  State     Sensors       Average   Average   Index      "
        "Fan %\r\n"
        "---------  --------  ------------  --------  --------  ---------  "
        "---------\r\n");
    if (powered) {
        g_string_append(out, ws_zones[board]);
    } else {
        g_auto(GStrv) rows = g_strsplit(ws_zones[board], "Enabled ", -1);
        g_autofree char *waiting = g_strjoinv(ENV_WAIT_POWER, rows);

        g_string_append(out, waiting);
    }
}

static void env_u2_temps(GString *out, bool powered)
{
    g_string_append(out,
        "\r\n"
        "                              Advisory   Critical   Fault      "
        "Current\r\n"
        "Description       State       Temp       Temp       Temp       "
        "Temp\r\n"
        "----------------- ----------  ---------  ---------  ---------  "
        "---------\r\n");
    for (unsigned i = 0; i < ARRAY_SIZE(u2_temps); i++) {
        const EnvTemp *t = &u2_temps[i];

        if (!t->present) {
            g_string_append_printf(out, "%2u %-14s <not present>\r\n", i,
                                   t->name);
            continue;
        }
        g_string_append_printf(out, "%2u %-14s %10s   31C/%3uF   "
                               "48C/%3uF   55C/%3uF   ", i, t->name,
                               powered ? "Enabled" : ENV_WAIT_POWER,
                               env_fahrenheit(31), env_fahrenheit(48),
                               env_fahrenheit(55));
        env_temp_current(out, powered, t->reading_c);
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
        env_rails(out, ENV_WS_NAME_WIDTH, ws_rails, ARRAY_SIZE(ws_rails), board,
                  powered);
        env_fans(out, ws_fans,
                 board == ENV_IP59 ? ARRAY_SIZE(ws_fans) : WS_FANS_IP53,
                 board, powered);
        env_ws_temps(out, board, powered);
    } else {
        env_rails(out, ENV_2U_NAME_WIDTH, u2_rails, ARRAY_SIZE(u2_rails), board,
                  powered);
        env_fans(out, u2_fans,
                 board == ENV_IP59 ? ARRAY_SIZE(u2_fans) : U2_FANS_IP53,
                 board, powered);
        env_u2_temps(out, powered);
    }
    return g_string_free(out, FALSE);
}
