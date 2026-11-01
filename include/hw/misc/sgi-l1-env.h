/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 console's "env" report for the Chimera interface boards.
 */
#ifndef HW_MISC_SGI_L1_ENV_H
#define HW_MISC_SGI_L1_ENV_H

#include "qemu/typedefs.h"

/*
 * The operator's env and fan settings. The all-zero value is the default:
 * monitoring on, power control on, temperature checking on, automatic fan
 * control on, normal altitude.
 */
typedef struct SGIL1EnvSettings {
    bool disabled;          /* "env off" */
    bool temp_off;          /* "env temp off" */
    uint8_t power_mode;     /* "env power": 0 on, 1 off, 2 monitor */
    bool altitude_high;     /* "env altitude high" */
    /* The settings below are RAM-only and are lost at a controller start. */
    bool fan_auto_off;      /* "fan auto off" */
    bool faults_only;       /* "env messages faultsonly" */
    bool fans_off;          /* "fan off" */
    bool custom_limits;     /* "env test <five values>" */
    uint8_t custom[4];      /* advisory, critical, fault, maximum (C) */
    uint8_t fan_override_mask;  /* fans whose drive byte was written */
    uint8_t fan_override[8];
} SGIL1EnvSettings;

/**
 * sgi_l1_chimera_env: the text the "env" command prints
 * @type: the brick type character: '7' for the 2U interface board, '9' for
 *        the workstation interface board, 'Q' for the rack-mounted
 *        workstation
 * @node_board: the node board's product name, such as "IP59_4CPU"
 * @powered: whether the node has power; an unpowered node reports every
 *           row as waiting for power
 * @settings: the operator's env and fan settings
 * @full: print the tables; false prints the banner or status line only
 *
 * The node board's name chooses between the IP53 and the IP59 tables.
 *
 * Returns: a newly allocated string with CR LF line ends, or NULL when
 * @type or @node_board names no modelled table.
 */
char *sgi_l1_chimera_env(char type, const char *node_board, bool powered,
                         const SGIL1EnvSettings *settings, bool full);

/**
 * sgi_l1_chimera_fan: the lines "fan" or "fan speed" prints
 * @speed: print the drive bytes ("fan speed"), not the RPM ("fan")
 *
 * Returns: a newly allocated string, or NULL as sgi_l1_chimera_env(), or
 * for "fan speed" on the workstation, whose fan channels are not mapped.
 */
char *sgi_l1_chimera_fan(char type, const char *node_board, bool powered,
                         const SGIL1EnvSettings *settings, bool speed);

/**
 * sgi_l1_chimera_fan_set: write fan drive bytes
 * @fan: the fan index, or -1 for all
 * @level: 0 low (the monitor's start-up level), 1 medium, 2 high, -1 @value
 *
 * Returns: false when the fan, board or value is not accepted.
 */
bool sgi_l1_chimera_fan_set(char type, const char *node_board,
                            SGIL1EnvSettings *settings, int fan, int level,
                            unsigned value);

/**
 * sgi_l1_chimera_power: the table "power" prints
 *
 * Returns: a newly allocated string, or NULL as sgi_l1_chimera_env().
 */
char *sgi_l1_chimera_power(char type, const char *node_board, bool powered);

/** sgi_l1_chimera_has_autofan: whether the brick type has "fan auto" */
bool sgi_l1_chimera_has_autofan(char type);

#endif
