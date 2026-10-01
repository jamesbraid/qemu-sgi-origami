/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Stubs for linking chardev/wctablet.c into test-char without pulling in
 * ui/input.c, whose QMP command handlers drag in monitor and replay
 * infrastructure a chardev unit test has no use for.
 */

#include "qemu/osdep.h"
#include "ui/input.h"
#include "wctablet-stubs.h"

DeviceState *wctablet_stub_dev;
const QemuInputHandler *wctablet_stub_handler;
bool wctablet_stub_registered;

/* an opaque handle for wctablet.c to hold and hand back; never dereferenced */
static int wctablet_stub_handle;

QemuInputHandlerState *
qemu_input_handler_register(DeviceState *dev, const QemuInputHandler *handler)
{
    wctablet_stub_dev = dev;
    wctablet_stub_handler = handler;
    wctablet_stub_registered = true;
    return (QemuInputHandlerState *)&wctablet_stub_handle;
}

void qemu_input_handler_activate(QemuInputHandlerState *s)
{
}

void qemu_input_handler_unregister(QemuInputHandlerState *s)
{
    wctablet_stub_registered = false;
}
