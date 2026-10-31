/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 "env" console output for the Chimera interface boards, as the
 * console captures show it. The Current column is a measurement, which the
 * model does not make, so each reading is masked as "#"; the names, order,
 * states and limits are exact. The workstation tables are the IP59 and
 * IP53 workstation captures, and the 2U IP59 table is the Onyx 350 module
 * capture. The IP53 2U table has no capture: it is the IP59 2U table with
 * the board's name, VCPU limits and fan list, which is inferred.
 *
 * The firmware has one copy of each row format, so the workstation rows
 * use the same 14-wide supply names and two-wide temperature indexes as
 * the 2U rows. The workstation captures show them one column narrower
 * because the forum pages dropped a leading space.
 */
#ifndef SGI_L1_CHIMERA_ENV_VECTORS_H
#define SGI_L1_CHIMERA_ENV_VECTORS_H

#define ENV_WS_IP59 \
    "Environmental monitoring is enabled and running.\r\n" \
    "\r\n" \
    "Description    State       Warning Limits     Fault Limits    " \
    "   Current\r\n" \
    "-------------- ----------  -----------------  ----------------" \
    "-  -------\r\n" \
    "          1.8V    Enabled  10%   1.62/  1.98  20%   1.44/  2.16" \
    "  #\r\n" \
    "           12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "        12V #2    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "          3.3V    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    "          2.5V    Enabled  10%   2.25/  2.75  20%   2.00/  3.00" \
    "  #\r\n" \
    "        12V IO    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "        5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "      3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    "            5V    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "  XIO 12V BIAS    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "        XIO 5V    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "      XIO 2.5V    Enabled  10%   2.25/  2.75  20%   2.00/  3.00" \
    "  #\r\n" \
    "  XIO 3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    " IP59 3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    "   IP59 5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "      IP59 12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "     IP59 VCPU    Enabled  10%   1.14/  1.40  20%   1.02/  1.52" \
    "  #\r\n" \
    "     IP59 SRAM    Enabled  10%   2.25/  2.75  20%   2.00/  3.00" \
    "  #\r\n" \
    "     IP59 1.5V    Enabled  10%   1.35/  1.65  20%   1.20/  1.80" \
    "  #\r\n" \
    "\r\n" \
    "Description     State       Warning RPM  Current RPM\r\n" \
    "--------------- ----------  -----------  -----------\r\n" \
    "FAN  0   NODE 1    Enabled         1800 #\r\n" \
    "FAN  1   NODE 2    Enabled         1800 #\r\n" \
    "FAN  2   NODE 3    Enabled         1800 #\r\n" \
    "FAN  3    PCI 1    Enabled         1350 #\r\n" \
    "FAN  4    PCI 2    Enabled         1350 #\r\n" \
    "FAN  5       HD    Enabled         1620 #\r\n" \
    "FAN  6    ODY 1    Enabled         1300 #\r\n" \
    "FAN  7    ODY 2    Enabled         1300 #\r\n" \
    "FAN  8  N0 LEFT    Enabled         1800 #\r\n" \
    "FAN  9  N0 CNTR    Enabled         1800 #\r\n" \
    "FAN 10 N0 RIGHT    Enabled         1800 #\r\n" \
    "\r\n" \
    "                              Advisory   Critical   Fault      " \
    "Current\r\n" \
    "Description       State       Temp       Temp       Temp      " \
    " Temp\r\n" \
    "----------------- ----------  ---------  ---------  --------- " \
    " ---------\r\n" \
    " 0 INTERFACE 0       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 1 INTERFACE 1       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 2 INTERFACE 2       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 3 INTERFACE 3       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 4 ODYSSEY           Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 5 NODE              Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 6 BEDROCK           Enabled    [Autofan Control]    85C/185F  " \
    " #\r\n" \
    "\r\n" \
    "                    Zone Temp     Target    Current   Zone Fan" \
    "   Curr/Min\r\n" \
    "Zone Name  State     Sensors       Average   Average   Index  " \
    "    Fan %\r\n" \
    "---------  --------  ------------  --------  --------  -------" \
    "--  ---------\r\n" \
    "NODE        Enabled           5,6  53C/127F  48C/118F         " \
    " 0   62%/ 46%\r\n" \
    "BOOST       Enabled           N/A       N/A       N/A     8,9," \
    "10   64%/ 50%\r\n" \
    "HD          Enabled           N/A       N/A       N/A         " \
    " 5   56%/ 38%\r\n" \
    "PCI         Enabled       0,1,2,3  45C/113F  37C/ 98F        3" \
    ",4   57%/ 57%\r\n" \
    "ODY         Enabled             4  50C/122F  45C/113F         " \
    " 6   82%/ 64%\r\n"

#define ENV_WS_IP53 \
    "Environmental monitoring is enabled and running.\r\n" \
    "\r\n" \
    "Description    State       Warning Limits     Fault Limits    " \
    "   Current\r\n" \
    "-------------- ----------  -----------------  ----------------" \
    "-  -------\r\n" \
    "          1.8V    Enabled  10%   1.62/  1.98  20%   1.44/  2.16" \
    "  #\r\n" \
    "           12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "        12V #2    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "          3.3V    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    "          2.5V    Enabled  10%   2.25/  2.75  20%   2.00/  3.00" \
    "  #\r\n" \
    "        12V IO    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "        5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "      3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    "            5V    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "  XIO 12V BIAS    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "        XIO 5V    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "      XIO 2.5V    Enabled  10%   2.25/  2.75  20%   2.00/  3.00" \
    "  #\r\n" \
    "  XIO 3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    " IP53 3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.96" \
    "  #\r\n" \
    "   IP53 5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.00" \
    "  #\r\n" \
    "      IP53 12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40" \
    "  #\r\n" \
    "     IP53 VCPU    Enabled  10%   1.13/  1.38  20%   1.00/  1.50" \
    "  #\r\n" \
    "     IP53 SRAM    Enabled  10%   2.25/  2.75  20%   2.00/  3.00" \
    "  #\r\n" \
    "     IP53 1.5V    Enabled  10%   1.35/  1.65  20%   1.20/  1.80" \
    "  #\r\n" \
    "\r\n" \
    "Description     State       Warning RPM  Current RPM\r\n" \
    "--------------- ----------  -----------  -----------\r\n" \
    "FAN  0   NODE 1    Enabled         1800 #\r\n" \
    "FAN  1   NODE 2    Enabled         1800 #\r\n" \
    "FAN  2   NODE 3    Enabled         1800 #\r\n" \
    "FAN  3    PCI 1    Enabled         1350 #\r\n" \
    "FAN  4    PCI 2    Enabled         1350 #\r\n" \
    "FAN  5       HD    Enabled         1620 #\r\n" \
    "FAN  6    ODY 1    Enabled         1300 #\r\n" \
    "FAN  7    ODY 2    Enabled         1300 #\r\n" \
    "\r\n" \
    "                              Advisory   Critical   Fault      " \
    "Current\r\n" \
    "Description       State       Temp       Temp       Temp      " \
    " Temp\r\n" \
    "----------------- ----------  ---------  ---------  --------- " \
    " ---------\r\n" \
    " 0 INTERFACE 0       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 1 INTERFACE 1       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 2 INTERFACE 2       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 3 INTERFACE 3       Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 4 ODYSSEY           Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 5 NODE              Enabled    [Autofan Control]    76C/168F  " \
    " #\r\n" \
    " 6 BEDROCK           Enabled    [Autofan Control]    85C/185F  " \
    " #\r\n" \
    "\r\n" \
    "                    Zone Temp     Target    Current   Zone Fan" \
    "   Curr/Min\r\n" \
    "Zone Name  State     Sensors       Average   Average   Index  " \
    "    Fan %\r\n" \
    "---------  --------  ------------  --------  --------  -------" \
    "--  ---------\r\n" \
    "Node        Enabled           5,6  62C/143F  50C/122F         " \
    " 0   46%/ 46%\r\n" \
    "PCI         Enabled       0,1,2,3  45C/113F  34C/ 93F        3" \
    ",4   57%/ 57%\r\n" \
    "ODY         Enabled             4  50C/122F  42C/107F         " \
    " 6   64%/ 64%\r\n" \
    "HD          Enabled             5  40C/104F  50C/122F         " \
    " 5   66%/ 38%\r\n"

#define ENV_2U_IP59 \
    "Environmental monitoring is enabled and running.\r\n" \
    "\r\n" \
    "Description    State       Warning Limits     Fault Limits    " \
    "   Current\r\n" \
    "-------------- ----------  -----------------  ----------------" \
    "-  -------\r\n" \
    "          1.8V    Enabled  10%   1.62/  1.98  20%   1.44/  2.1" \
    "6  #\r\n" \
    "           12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "        12V #2    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "          3.3V    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "        12V IO    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "        5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "      3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "    PCI 5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "      PCI 3.3V    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "      PCI 2.5V    Enabled  10%   2.25/  2.75  20%   2.00/  3.0" \
    "0  #\r\n" \
    "        PCI 5V    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "  XIO 12V BIAS <not present>\r\n" \
    "        XIO 5V <not present>\r\n" \
    "      XIO 2.5V <not present>\r\n" \
    "  XIO 3.3V AUX <not present>\r\n" \
    " IP59 3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "   IP59 5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "      IP59 12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "     IP59 VCPU    Enabled  10%   1.14/  1.40  20%   1.02/  1.5" \
    "2  #\r\n" \
    "     IP59 SRAM    Enabled  10%   2.25/  2.75  20%   2.00/  3.0" \
    "0  #\r\n" \
    "     IP59 1.5V    Enabled  10%   1.35/  1.65  20%   1.20/  1.8" \
    "0  #\r\n" \
    "\r\n" \
    "Description     State       Warning RPM  Current RPM\r\n" \
    "--------------- ----------  -----------  -----------\r\n" \
    "FAN  0  EXHST 1    Enabled         1980 #\r\n" \
    "FAN  1  EXHST 2    Enabled         1980 #\r\n" \
    "FAN  2       PS    Enabled         3200 #\r\n" \
    "FAN  3    PCI 1    Enabled         1980 #\r\n" \
    "FAN  4    PCI 2    Enabled         1980 #\r\n" \
    "FAN  5  N0 LEFT    Enabled         1980 #\r\n" \
    "FAN  6  N0 CNTR    Enabled         1980 #\r\n" \
    "FAN  7 N0 RIGHT    Enabled         1980 #\r\n" \
    "\r\n" \
    "                              Advisory   Critical   Fault     " \
    " Current\r\n" \
    "Description       State       Temp       Temp       Temp      " \
    " Temp\r\n" \
    "----------------- ----------  ---------  ---------  --------- " \
    " ---------\r\n" \
    " 0 INTERFACE 0       Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 1 INTERFACE 1       Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 2 INTERFACE 2       Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 3 PCI RISER         Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 4 ODYSSEY        <not present>\r\n" \
    " 5 NODE              Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 6 BEDROCK           Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n"

#define ENV_2U_IP53 \
    "Environmental monitoring is enabled and running.\r\n" \
    "\r\n" \
    "Description    State       Warning Limits     Fault Limits    " \
    "   Current\r\n" \
    "-------------- ----------  -----------------  ----------------" \
    "-  -------\r\n" \
    "          1.8V    Enabled  10%   1.62/  1.98  20%   1.44/  2.1" \
    "6  #\r\n" \
    "           12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "        12V #2    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "          3.3V    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "        12V IO    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "        5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "      3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "    PCI 5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "      PCI 3.3V    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "      PCI 2.5V    Enabled  10%   2.25/  2.75  20%   2.00/  3.0" \
    "0  #\r\n" \
    "        PCI 5V    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "  XIO 12V BIAS <not present>\r\n" \
    "        XIO 5V <not present>\r\n" \
    "      XIO 2.5V <not present>\r\n" \
    "  XIO 3.3V AUX <not present>\r\n" \
    " IP53 3.3V AUX    Enabled  10%   2.97/  3.63  20%   2.64/  3.9" \
    "6  #\r\n" \
    "   IP53 5V AUX    Enabled  10%   4.50/  5.50  20%   4.00/  6.0" \
    "0  #\r\n" \
    "      IP53 12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.4" \
    "0  #\r\n" \
    "     IP53 VCPU    Enabled  10%   1.13/  1.38  20%   1.00/  1.5" \
    "0  #\r\n" \
    "     IP53 SRAM    Enabled  10%   2.25/  2.75  20%   2.00/  3.0" \
    "0  #\r\n" \
    "     IP53 1.5V    Enabled  10%   1.35/  1.65  20%   1.20/  1.8" \
    "0  #\r\n" \
    "\r\n" \
    "Description     State       Warning RPM  Current RPM\r\n" \
    "--------------- ----------  -----------  -----------\r\n" \
    "FAN  0  EXHST 1    Enabled         1980 #\r\n" \
    "FAN  1  EXHST 2    Enabled         1980 #\r\n" \
    "FAN  2       PS    Enabled         3200 #\r\n" \
    "FAN  3    PCI 1    Enabled         1980 #\r\n" \
    "FAN  4    PCI 2    Enabled         1980 #\r\n" \
    "\r\n" \
    "                              Advisory   Critical   Fault     " \
    " Current\r\n" \
    "Description       State       Temp       Temp       Temp      " \
    " Temp\r\n" \
    "----------------- ----------  ---------  ---------  --------- " \
    " ---------\r\n" \
    " 0 INTERFACE 0       Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 1 INTERFACE 1       Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 2 INTERFACE 2       Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 3 PCI RISER         Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 4 ODYSSEY        <not present>\r\n" \
    " 5 NODE              Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n" \
    " 6 BEDROCK           Enabled   31C/ 87F   48C/118F   55C/131F " \
    "  #\r\n"

#endif
