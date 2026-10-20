/*
 * Hotplug handler stubs
 *
 * Copyright (c) Red Hat
 *
 * Authors:
 *  Philippe Mathieu-Daudé,
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "qapi/error.h"

const HotplugHandler *qdev_get_hotplug_handler(DeviceState *dev)
{
    return NULL;
}

bool qdev_hotunplug_allowed(DeviceState *dev, Error **errp)
{
    if (dev->unplug_blockers) {
        error_propagate(errp, error_copy(dev->unplug_blockers->data));
        return false;
    }
    return true;
}

void hotplug_handler_pre_plug(const HotplugHandler *plug_handler,
                              DeviceState *plugged_dev,
                              Error **errp)
{
    g_assert_not_reached();
}

void hotplug_handler_plug(const HotplugHandler *plug_handler,
                          DeviceState *plugged_dev,
                          Error **errp)
{
    g_assert_not_reached();
}
