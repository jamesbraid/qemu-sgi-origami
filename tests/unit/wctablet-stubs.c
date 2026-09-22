/* SPDX-License-Identifier: GPL-2.0-or-later */

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

QemuInputHandlerState *
qemu_input_handler_register(DeviceState *dev, const QemuInputHandler *handler)
{
    wctablet_stub_dev = dev;
    wctablet_stub_handler = handler;
    return NULL;
}

void qemu_input_handler_unregister(QemuInputHandlerState *s)
{
}
