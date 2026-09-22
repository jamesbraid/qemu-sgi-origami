/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TESTS_UNIT_WCTABLET_STUBS_H
#define TESTS_UNIT_WCTABLET_STUBS_H

#include "ui/input.h"

/*
 * chardev/wctablet.c registers its DeviceState and QemuInputHandler
 * with qemu_input_handler_register(), which this stub does not wire
 * into the real ui/input.c dispatch queue -- a standalone chardev
 * test does not link that. It records what was registered here
 * instead, so the test can drive handler->event()/handler->sync()
 * directly, the way ui/input.c would.
 */
extern DeviceState *wctablet_stub_dev;
extern const QemuInputHandler *wctablet_stub_handler;

#endif
