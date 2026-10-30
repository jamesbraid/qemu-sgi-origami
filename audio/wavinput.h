/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AUDIO_WAVINPUT_H
#define AUDIO_WAVINPUT_H

#include <stdbool.h>
#include <stddef.h>

#include "qapi/error.h"
#include "qemu/audio.h"

typedef struct WavInput WavInput;

WavInput *wav_input_open(const char *path, bool loop, Error **errp);
void wav_input_get_settings(WavInput *input, struct audsettings *settings);
size_t wav_input_read(WavInput *input, void *buffer, size_t size);
void wav_input_close(WavInput *input);

#endif
