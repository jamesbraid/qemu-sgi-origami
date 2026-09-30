/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Stubs for audio test - provides missing functions for standalone audio test
 */

#include "qemu/osdep.h"
#include "qemu/dbus.h"
#include "ui/qemu-spice-module.h"
#include "ui/dbus-module.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "system/cpu-timers.h"
#include "qemu/main-loop.h"
#include "audio-test.h"

int using_spice;
int using_dbus_display;

struct QemuSpiceOps qemu_spice;

GQuark dbus_display_error_quark(void)
{
    return g_quark_from_static_string("dbus-display-error-quark");
}

#ifdef WIN32
/* from ui/dbus.h */
bool
dbus_win32_import_socket(GDBusMethodInvocation *invocation,
                         GVariant *arg_listener, int *socket);

bool
dbus_win32_import_socket(GDBusMethodInvocation *invocation,
                         GVariant *arg_listener, int *socket)
{
    return true;
}
#endif

void replay_audio_out(size_t *played)
{
}

void replay_audio_in_start(size_t *nsamples)
{
}

void replay_audio_in_sample_lr(uint64_t *left, uint64_t *right)
{
}

void replay_audio_in_finish(void)
{
}

static int dummy_vmse;

VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb,
                                                     void *opaque)
{
    return (VMChangeStateEntry *)&dummy_vmse;
}

void qemu_del_vm_change_state_handler(VMChangeStateEntry *e)
{
}

bool runstate_is_running(void)
{
    return true;
}

/* A negative value keeps the standalone tests on the existing real clock. */
static int64_t audio_test_virtual_clock = -1;

void audio_test_set_virtual_clock(int64_t time)
{
    audio_test_virtual_clock = time;
}

int64_t cpus_get_virtual_clock(void)
{
    return audio_test_virtual_clock < 0 ? cpu_get_clock() :
        audio_test_virtual_clock;
}

void cpus_set_virtual_clock(int64_t new_time)
{
    if (audio_test_virtual_clock >= 0) {
        audio_test_virtual_clock = new_time;
    }
}
