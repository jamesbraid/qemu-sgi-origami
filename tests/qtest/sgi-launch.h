/*
 * Check refused SGI qtest launches from multiple tests.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_LAUNCH_H
#define SGI_LAUNCH_H

#include "qemu/osdep.h"
#include "libqtest.h"

/* @args must include "-qmp stdio"; require failure and @message on stderr.
 * Send quit over QMP if QEMU accepts the launch. */
static inline void assert_launch_rejected(const char *const args[],
                                          const char *message)
{
    const char commands[] = "{\"execute\":\"qmp_capabilities\"}\n"
                            "{\"execute\":\"quit\"}\n";
    char error_output[1024] = { 0 };
    struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
    GPid pid;
    int input_fd, error_fd, status;
    ssize_t length;

    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)args, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                    NULL, NULL, &pid, &input_fd, NULL, &error_fd, NULL));
    /* Quit an incorrectly accepted launch instead of leaving it running. */
    sigemptyset(&ignore.sa_mask);
    g_assert_cmpint(sigaction(SIGPIPE, &ignore, &previous), ==, 0);
    length = qemu_write_full(input_fd, commands, sizeof(commands) - 1);
    /*
 * qemu_write_full() returns the count actually written, not -1, even when
 * one of its write() calls fails. A rejected launch that exits and
 * closes its end of the pipe after accepting only part of the buffer (or
 * none of it) shows up here as a short count with errno left at EPIPE, not
 * as a -1 return. Any such count is fine: this write only delivers the QMP
 * commands, it does not prove the rejection. That proof is the exit status
 * and expected stderr text checked below, which still catch a launch QEMU
 * wrongly accepted.
 */
    g_assert_true(length == sizeof(commands) - 1 ||
                  (length >= 0 && length < sizeof(commands) - 1 &&
                   errno == EPIPE));
    g_assert_cmpint(sigaction(SIGPIPE, &previous, NULL), ==, 0);
    close(input_fd);
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    length = read(error_fd, error_output, sizeof(error_output) - 1);
    g_assert_cmpint(length, >=, 0);
    close(error_fd);
    g_spawn_close_pid(pid);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_nonnull(strstr(error_output, message));
}

#endif /* SGI_LAUNCH_H */
