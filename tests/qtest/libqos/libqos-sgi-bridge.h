/*
 * libqos boot and shutdown interface for the SGI Bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LIBQOS_SGI_BRIDGE_H
#define LIBQOS_SGI_BRIDGE_H

#include "libqos.h"

QOSState *qtest_sgi_bridge_vboot(const char *cmdline_fmt, va_list ap)
    G_GNUC_PRINTF(1, 0);
QOSState *qtest_sgi_bridge_boot(const char *cmdline_fmt, ...)
    G_GNUC_PRINTF(1, 2);
void qtest_sgi_bridge_shutdown(QOSState *qs);

#endif
