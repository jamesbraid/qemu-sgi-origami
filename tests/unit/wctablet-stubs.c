/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Stubs for linking chardev/wctablet.c into test-char without pulling in
 * ui/input.c, whose QMP command handlers drag in monitor and replay
 * infrastructure a chardev unit test has no use for.
 */

#include "qemu/osdep.h"
#include "ui/input.h"

QemuInputHandlerState *
qemu_input_handler_register(DeviceState *dev, const QemuInputHandler *handler)
{
    return NULL;
}

void qemu_input_handler_unregister(QemuInputHandlerState *s)
{
}
