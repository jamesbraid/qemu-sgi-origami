/*
 * libqos bring-up on the SGI Bridge head
 *
 * The same QOSState a test gets from a PC, over one Bridge on the "none"
 * machine. A caller passes only its own devices: the head's chip, the memory
 * a card transfers into and the machine they sit on are prepended here, so a
 * card test names one command line whichever host it runs on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
