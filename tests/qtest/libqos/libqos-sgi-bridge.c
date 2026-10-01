/*
 * libqos QOSState for one SGI Bridge on -M none. Caller devices follow the
 * Bridge and transfer memory, so card tests use one command line on any host.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "libqos-sgi-bridge.h"
#include "pci-sgi-bridge.h"

static void sgi_bridge_alloc_init(QGuestAllocator *alloc, QTestState *qts,
                                  QAllocOpts flags)
{
    qpci_sgi_bridge_alloc_init(alloc, flags);
}

static QOSOps qos_ops = {
    .alloc_init = sgi_bridge_alloc_init,
    .qpci_new = qpci_init_sgi_bridge,
    .qpci_free = qpci_free_sgi_bridge,
    .shutdown = qtest_sgi_bridge_shutdown,
};

QOSState *qtest_sgi_bridge_vboot(const char *cmdline_fmt, va_list ap)
{
    g_autofree char *devices = g_strdup_vprintf(cmdline_fmt, ap);

    return qtest_boot(&qos_ops, "-M none " QPCI_SGI_BRIDGE_HEAD "%s", devices);
}

QOSState *qtest_sgi_bridge_boot(const char *cmdline_fmt, ...)
{
    QOSState *qs;
    va_list ap;

    va_start(ap, cmdline_fmt);
    qs = qtest_sgi_bridge_vboot(cmdline_fmt, ap);
    va_end(ap);

    return qs;
}

void qtest_sgi_bridge_shutdown(QOSState *qs)
{
    return qtest_common_shutdown(qs);
}
