/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TESTS_AUDIO_AUDIO_TEST_H
#define TESTS_AUDIO_AUDIO_TEST_H

#include <stdint.h>

/* A negative time restores the standalone tests' real clock. */
void audio_test_set_virtual_clock(int64_t time);

#endif
