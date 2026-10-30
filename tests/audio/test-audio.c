/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/config-file.h"
#include "qemu/cutils.h"
#include "qemu/help_option.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/audio.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "trace/control.h"
#include "glib.h"

#include "audio/audio_int.h"
#include "audio/wavinput.h"
#include "audio-test.h"

#include <math.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#ifdef CONFIG_SDL
/*
 * SDL insists on wrapping the main() function with its own implementation on
 * some platforms; it does so via a macro that renames our main function, so
 * <SDL.h> must be #included here even with no SDL code called from this file.
 */
#include <SDL.h>
#endif

#define SAMPLE_RATE     44100
#define CHANNELS        2
#define DURATION_SECS   2
#define FREQUENCY       440.0
#define BUFFER_FRAMES   1024
#define TIMEOUT_SECS    (DURATION_SECS + 1)

/* Command-line options */
static gchar *opt_audiodev;
static gchar *opt_trace;

static GOptionEntry test_options[] = {
    { "audiodev", 'a', 0, G_OPTION_ARG_STRING, &opt_audiodev,
      "Audio device spec (e.g., none or pa,out.buffer-length=50000)", "DEV" },
    { "trace", 'T', 0, G_OPTION_ARG_STRING, &opt_trace,
      "Trace options (e.g., 'pw_*')", "TRACE" },
    { NULL }
};

#define TEST_AUDIODEV_ID "test"

typedef struct TestSineState {
    AudioBackend *be;
    SWVoiceOut *voice;
    int64_t total_frames;
    int64_t frames_written;
} TestSineState;

/* Default audio settings for tests */
static const struct audsettings default_test_settings = {
    .freq = SAMPLE_RATE,
    .nchannels = CHANNELS,
    .fmt = AUDIO_FORMAT_S16,
    .big_endian = false,
};

static void dummy_audio_callback(void *opaque, int avail)
{
}

static AudioBackend *get_test_audio_backend(void)
{
    AudioBackend *be;
    Error *err = NULL;

    if (opt_audiodev) {
        be = audio_be_by_name(TEST_AUDIODEV_ID, &err);
    } else {
        be = audio_get_default_audio_be(&err);
    }

    if (err) {
        g_error("%s", error_get_pretty(err));
        error_free(err);
        exit(1);
    }
    g_assert_nonnull(be);
    return be;
}

/*
 * Helper functions for opening test voices with default settings.
 * These reduce boilerplate in test functions.
 */
static SWVoiceOut *open_test_voice_out(AudioBackend *be, const char *name,
                                       void *opaque, audio_callback_fn cb)
{
    struct audsettings as = default_test_settings;
    SWVoiceOut *voice;

    voice = audio_be_open_out(be, NULL, name, opaque, cb, &as);
    g_assert_nonnull(voice);
    return voice;
}

static SWVoiceIn *open_test_voice_in(AudioBackend *be, const char *name,
                                     void *opaque, audio_callback_fn cb)
{
    struct audsettings as = default_test_settings;

    return audio_be_open_in(be, NULL, name, opaque, cb, &as);
}

/*
 * Generate 440Hz sine wave samples into buffer.
 */
static void generate_sine_samples(int16_t *buffer, int frames,
                                  int64_t start_frame)
{
    for (int i = 0; i < frames; i++) {
        double t = (double)(start_frame + i) / SAMPLE_RATE;
        double sample = sin(2.0 * M_PI * FREQUENCY * t);
        int16_t s = (int16_t)(sample * 32767.0);

        buffer[i * 2] = s;       /* left channel */
        buffer[i * 2 + 1] = s;   /* right channel */
    }
}

static void test_sine_callback(void *opaque, int avail)
{
    TestSineState *s = opaque;
    int16_t buffer[BUFFER_FRAMES * CHANNELS];
    int frames_remaining;
    int frames_to_write;
    size_t bytes_written;

    frames_remaining = s->total_frames - s->frames_written;
    if (frames_remaining <= 0) {
        return;
    }

    frames_to_write = avail / (sizeof(int16_t) * CHANNELS);
    frames_to_write = MIN(frames_to_write, BUFFER_FRAMES);
    frames_to_write = MIN(frames_to_write, frames_remaining);

    generate_sine_samples(buffer, frames_to_write, s->frames_written);

    bytes_written = audio_be_write(s->be, s->voice, buffer,
                                   frames_to_write * sizeof(int16_t) * CHANNELS);
    s->frames_written += bytes_written / (sizeof(int16_t) * CHANNELS);
}


static void test_audio_out_sine_wave(void)
{
    TestSineState state = {0};
    int64_t start_time;
    int64_t elapsed_ms;

    state.be = get_test_audio_backend();
    state.total_frames = SAMPLE_RATE * DURATION_SECS;
    state.frames_written = 0;

    g_test_message("Opening audio output...");
    state.voice = open_test_voice_out(state.be, "test-sine",
                                      &state, test_sine_callback);

    g_test_message("Playing 440Hz sine wave for %d seconds...", DURATION_SECS);
    audio_be_set_active_out(state.be, state.voice, true);

    /*
     * Run the audio subsystem until all frames are written or timeout.
     */
    start_time = g_get_monotonic_time();
    while (state.frames_written < state.total_frames) {
        audio_run(AUDIO_MIXENG_BACKEND(state.be), "test");
        main_loop_wait(true);

        elapsed_ms = (g_get_monotonic_time() - start_time) / 1000;
        if (elapsed_ms > TIMEOUT_SECS * 1000) {
            g_test_message("Timeout waiting for audio to complete");
            break;
        }

        g_usleep(G_USEC_PER_SEC / 100);  /* 10ms */
    }

    g_test_message("Wrote %" PRId64 " frames (%.2f seconds)",
                   state.frames_written,
                   (double)state.frames_written / SAMPLE_RATE);

    g_assert_cmpint(state.frames_written, ==, state.total_frames);

    audio_be_set_active_out(state.be, state.voice, false);
    audio_be_close_out(state.be, state.voice);
}

static void test_audio_prio_list(void)
{
    g_autofree gchar *backends = NULL;
    GString *str = g_string_new(NULL);
    bool has_none = false;

    for (int i = 0; audio_prio_list[i]; i++) {
        if (i > 0) {
            g_string_append_c(str, ' ');
        }
        g_string_append(str, audio_prio_list[i]);

        if (g_strcmp0(audio_prio_list[i], "none") == 0) {
            has_none = true;
        }
    }

    backends = g_string_free(str, FALSE);
    g_test_message("Available backends: %s", backends);

    /* The 'none' backend should always be available */
    g_assert_true(has_none);
}

static void test_audio_out_active_state(void)
{
    AudioBackend *be;
    SWVoiceOut *voice;

    be = get_test_audio_backend();
    voice = open_test_voice_out(be, "test-active", NULL, dummy_audio_callback);

    g_assert_false(audio_be_is_active_out(be, voice));

    audio_be_set_active_out(be, voice, true);
    g_assert_true(audio_be_is_active_out(be, voice));

    audio_be_set_active_out(be, voice, false);
    g_assert_false(audio_be_is_active_out(be, voice));

    audio_be_close_out(be, voice);
}

static void test_audio_out_buffer_size(void)
{
    AudioBackend *be;
    SWVoiceOut *voice;
    int buffer_size;

    be = get_test_audio_backend();
    voice = open_test_voice_out(be, "test-buffer", NULL, dummy_audio_callback);

    buffer_size = audio_be_get_buffer_size_out(be, voice);
    g_test_message("Buffer size: %d bytes", buffer_size);
    g_assert_cmpint(buffer_size, >, 0);

    audio_be_close_out(be, voice);

    g_assert_cmpint(audio_be_get_buffer_size_out(be, NULL), ==, 0);
}

static void test_audio_out_volume(void)
{
    AudioBackend *be;
    SWVoiceOut *voice;
    Volume vol;

    be = get_test_audio_backend();
    voice = open_test_voice_out(be, "test-volume", NULL, dummy_audio_callback);

    vol = (Volume){ .mute = false, .channels = 2, .vol = {255, 255} };
    audio_be_set_volume_out(be, voice, &vol);

    vol = (Volume){ .mute = true, .channels = 2, .vol = {255, 255} };
    audio_be_set_volume_out(be, voice, &vol);

    vol = (Volume){ .mute = false, .channels = 2, .vol = {128, 128} };
    audio_be_set_volume_out(be, voice, &vol);

    audio_be_close_out(be, voice);
}

static void test_audio_in_active_state(void)
{
    AudioBackend *be;
    SWVoiceIn *voice;

    be = get_test_audio_backend();
    voice = open_test_voice_in(be, "test-in-active", NULL, dummy_audio_callback);
    if (!voice) {
        g_test_skip("The backend may not support input");
        return;
    }

    g_assert_false(audio_be_is_active_in(be, voice));

    audio_be_set_active_in(be, voice, true);
    g_assert_true(audio_be_is_active_in(be, voice));

    audio_be_set_active_in(be, voice, false);
    g_assert_false(audio_be_is_active_in(be, voice));

    audio_be_close_in(be, voice);
}

static void test_audio_in_volume(void)
{
    AudioBackend *be;
    SWVoiceIn *voice;
    Volume vol;

    be = get_test_audio_backend();
    voice = open_test_voice_in(be, "test-in-volume", NULL, dummy_audio_callback);
    if (!voice) {
        g_test_skip("The backend may not support input");
        return;
    }

    vol = (Volume){ .mute = false, .channels = 2, .vol = {255, 255} };
    audio_be_set_volume_in(be, voice, &vol);

    vol = (Volume){ .mute = true, .channels = 2, .vol = {255, 255} };
    audio_be_set_volume_in(be, voice, &vol);

    audio_be_close_in(be, voice);
}


/* Capture test state */
#define CAPTURE_BUFFER_FRAMES (SAMPLE_RATE / 10)  /* 100ms of audio */
#define CAPTURE_BUFFER_SIZE   (CAPTURE_BUFFER_FRAMES * CHANNELS * sizeof(int16_t))

typedef struct TestCaptureState {
    bool notify_called;
    bool capture_called;
    bool destroy_called;
    audcnotification_e last_notify;
    int16_t *captured_samples;
    size_t captured_bytes;
    size_t capture_buffer_size;
} TestCaptureState;

static void test_capture_notify(void *opaque, audcnotification_e cmd)
{
    TestCaptureState *s = opaque;
    g_assert_false(s->destroy_called);
    s->notify_called = true;
    s->last_notify = cmd;
}

static void test_capture_capture(void *opaque, const void *buf, int size)
{
    TestCaptureState *s = opaque;
    size_t bytes_to_copy;

    s->capture_called = true;

    if (!s->captured_samples || s->captured_bytes >= s->capture_buffer_size) {
        return;
    }

    bytes_to_copy = MIN(size, s->capture_buffer_size - s->captured_bytes);
    memcpy((uint8_t *)s->captured_samples + s->captured_bytes, buf, bytes_to_copy);
    s->captured_bytes += bytes_to_copy;
}

static void test_capture_destroy(void *opaque)
{
    TestCaptureState *s = opaque;
    g_assert_false(s->destroy_called);
    s->destroy_called = true;
}

/*
 * Compare captured audio with expected sine wave.
 * Returns the number of matching samples (within tolerance).
 */
static int compare_sine_samples(const int16_t *captured, int frames,
                                int64_t start_frame, int tolerance)
{
    int matching = 0;

    for (int i = 0; i < frames; i++) {
        double t = (double)(start_frame + i) / SAMPLE_RATE;
        double sample = sin(2.0 * M_PI * FREQUENCY * t);
        int16_t expected = (int16_t)(sample * 32767.0);

        /* Check left channel */
        if (abs(captured[i * 2] - expected) <= tolerance) {
            matching++;
        }
        /* Check right channel */
        if (abs(captured[i * 2 + 1] - expected) <= tolerance) {
            matching++;
        }
    }

    return matching;
}

static void test_audio_capture(void)
{
    AudioBackend *be;
    CaptureVoiceOut *cap;
    SWVoiceOut *voice;
    TestCaptureState state = {0};
    TestSineState sine_state = {0};
    struct audsettings as = default_test_settings;
    struct audio_capture_ops ops = {
        .notify = test_capture_notify,
        .capture = test_capture_capture,
        .destroy = test_capture_destroy,
    };
    int64_t start_time;
    int64_t elapsed_ms;
    int captured_frames;
    int matching_samples;
    int total_samples;
    double match_ratio;

    be = get_test_audio_backend();

    state.captured_samples = g_malloc0(CAPTURE_BUFFER_SIZE);
    state.captured_bytes = 0;
    state.capture_buffer_size = CAPTURE_BUFFER_SIZE;

    cap = audio_be_add_capture(be, &as, &ops, &state);
    g_assert_nonnull(cap);

    sine_state.be = be;
    sine_state.total_frames = CAPTURE_BUFFER_FRAMES;
    sine_state.frames_written = 0;

    voice = open_test_voice_out(be, "test-capture-sine",
                                &sine_state, test_sine_callback);
    sine_state.voice = voice;

    audio_be_set_active_out(be, voice, true);

    start_time = g_get_monotonic_time();
    while (sine_state.frames_written < sine_state.total_frames ||
           state.captured_bytes < CAPTURE_BUFFER_SIZE) {
        audio_run(AUDIO_MIXENG_BACKEND(be), "test-capture");
        main_loop_wait(true);

        elapsed_ms = (g_get_monotonic_time() - start_time) / 1000;
        if (elapsed_ms > 1000) {  /* 1 second timeout */
            break;
        }

        g_usleep(G_USEC_PER_SEC / 1000);  /* 1ms */
    }

    g_test_message("Wrote %" PRId64 " frames, captured %zu bytes",
                   sine_state.frames_written, state.captured_bytes);

    g_assert_true(state.capture_called);
    g_assert_cmpuint(state.captured_bytes, >, 0);

    /* Compare captured data with expected sine wave */
    captured_frames = state.captured_bytes / (CHANNELS * sizeof(int16_t));
    if (captured_frames > 0) {
        /*
         * Allow some tolerance due to mixing/conversion.
         * The tolerance accounts for potential rounding differences.
         */
        matching_samples = compare_sine_samples(state.captured_samples,
                                                captured_frames, 0, 100);
        total_samples = captured_frames * CHANNELS;
        match_ratio = (double)matching_samples / total_samples;

        g_test_message("Captured %d frames, %d/%d samples match (%.1f%%)",
                       captured_frames, matching_samples, total_samples,
                       match_ratio * 100.0);

        /*
         * Expect at least 90% of samples to match within tolerance.
         * Some variation is expected due to mixing engine processing.
         */
        g_assert_cmpfloat(match_ratio, >=, 0.9);
    }

    audio_be_set_active_out(be, voice, false);
    audio_be_close_out(be, voice);

    audio_be_del_capture(be, cap, &state);
    g_assert_true(state.destroy_called);

    g_free(state.captured_samples);
}

static void test_audio_null_handling(void)
{
    AudioBackend *be = get_test_audio_backend();
    uint8_t buffer[64];

    /* audio_be_is_active_out/in(NULL) should return false */
    g_assert_false(audio_be_is_active_out(be, NULL));
    g_assert_false(audio_be_is_active_in(be, NULL));

    /* audio_be_get_buffer_size_out(NULL) should return 0 */
    g_assert_cmpint(audio_be_get_buffer_size_out(be, NULL), ==, 0);

    /* audio_be_write/read(NULL, ...) should return 0 */
    g_assert_cmpuint(audio_be_write(be, NULL, buffer, sizeof(buffer)), ==, 0);
    g_assert_cmpuint(audio_be_read(be, NULL, buffer, sizeof(buffer)), ==, 0);

    /* These should not crash */
    audio_be_set_active_out(be, NULL, true);
    audio_be_set_active_out(be, NULL, false);
    audio_be_set_active_in(be, NULL, true);
    audio_be_set_active_in(be, NULL, false);
}

static void test_audio_multiple_voices(void)
{
    AudioBackend *be;
    SWVoiceOut *out1, *out2;
    SWVoiceIn *in1;

    be = get_test_audio_backend();
    out1 = open_test_voice_out(be, "test-multi-out1", NULL, dummy_audio_callback);
    out2 = open_test_voice_out(be, "test-multi-out2", NULL, dummy_audio_callback);
    in1 = open_test_voice_in(be, "test-multi-in1", NULL, dummy_audio_callback);

    audio_be_set_active_out(be, out1, true);
    audio_be_set_active_out(be, out2, true);
    audio_be_set_active_in(be, in1, true);

    g_assert_true(audio_be_is_active_out(be, out1));
    g_assert_true(audio_be_is_active_out(be, out2));
    if (in1) {
        g_assert_true(audio_be_is_active_in(be, in1));
    }

    audio_be_set_active_out(be, out1, false);
    audio_be_set_active_out(be, out2, false);
    audio_be_set_active_in(be, in1, false);

    audio_be_close_in(be, in1);
    audio_be_close_out(be, out2);
    audio_be_close_out(be, out1);
}

static const struct audsettings invalid_test_settings = {
    .nchannels = 0,
    .freq = SAMPLE_RATE,
    .fmt = AUDIO_FORMAT_S16,
    .big_endian = false,
};

static void test_audio_invalid_settings(void)
{
    AudioBackend *be = get_test_audio_backend();
    void *voice;

    voice = audio_be_open_out(be, NULL, "invalid", NULL,
                              dummy_audio_callback, &invalid_test_settings);
    g_assert_null(voice);
    voice = audio_be_open_in(be, NULL, "invalid", NULL,
                             dummy_audio_callback, &invalid_test_settings);
    g_assert_null(voice);
}

static void test_audio_cleanup_wav(void)
{
    AudioBackend *be;
    SWVoiceOut *voice;
    g_autofree char *path = NULL;
    g_autofree char *spec = NULL;
    g_autofree char *contents = NULL;
    gsize length;
    int fd;
    int16_t samples[128] = { 1, -1 };
    size_t written;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    path = g_test_build_filename(G_TEST_BUILT, "..", "..",
                                "audio-cleanup-XXXXXX", NULL);
    fd = g_mkstemp(path);
    g_test_message("WAV file: %s", path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    spec = g_strdup_printf("wav,id=cleanup,path=%s", path);
    audio_parse_option(spec);
    audio_init_audiodevs();
    be = audio_be_by_name("cleanup", &error_abort);
    voice = open_test_voice_out(be, "cleanup", NULL, dummy_audio_callback);
    audio_be_set_active_out(be, voice, true);

    g_usleep(10000);
    written = AUDIO_MIXENG_BACKEND_GET_CLASS(voice->s)->write(
        voice->hw, samples, sizeof(samples));
    g_assert_cmpuint(written, >, 0);
    g_assert_cmpint(fflush(NULL), ==, 0);
    audio_cleanup();
    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
    g_assert_cmpuint(length, ==, 44 + written);
    g_assert_cmpmem(contents + 44, written, samples, written);
    g_assert_cmpuint(ldl_le_p(contents + 4), ==, length - 8);
    g_assert_cmpuint(ldl_le_p(contents + 40), ==, length - 44);

    audio_cleanup();
    audio_be_close_out(be, voice);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_audio_cleanup_voices(void)
{
    AudioBackend *be;
    SWVoiceOut *out;
    SWVoiceIn *in;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_parse_option("none,id=cleanup");
    audio_init_audiodevs();
    be = audio_be_by_name("cleanup", &error_abort);
    out = open_test_voice_out(be, "cleanup", NULL, dummy_audio_callback);
    in = open_test_voice_in(be, "cleanup", NULL, dummy_audio_callback);
    g_assert_nonnull(in);
    audio_be_set_active_out(be, out, true);
    audio_be_set_active_in(be, in, true);

    object_ref(be);
    audio_cleanup();
    g_assert_false(out->hw->enabled);
    g_assert_false(in->hw->enabled);
    audio_cleanup();
    audio_be_close_out(be, out);
    audio_be_close_in(be, in);
    g_assert_true(QLIST_EMPTY(&AUDIO_MIXENG_BACKEND(be)->hw_head_out));
    g_assert_true(QLIST_EMPTY(&AUDIO_MIXENG_BACKEND(be)->hw_head_in));
    object_unref(be);
}

static void test_audio_cleanup_capture(void)
{
    AudioBackend *be;
    SWVoiceOut *out;
    CaptureVoiceOut *cap;
    TestCaptureState state = {0};
    struct audio_capture_ops ops = {
        .notify = test_capture_notify,
        .capture = test_capture_capture,
        .destroy = test_capture_destroy,
    };

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_parse_option("none,id=cleanup");
    audio_init_audiodevs();
    be = audio_be_by_name("cleanup", &error_abort);
    object_ref(be);
    out = open_test_voice_out(be, "cleanup", NULL, dummy_audio_callback);
    cap = audio_be_add_capture(be, &default_test_settings, &ops, &state);
    g_assert_nonnull(cap);
    audio_be_set_active_out(be, out, true);
    g_assert_true(state.notify_called);

    audio_cleanup();
    g_assert_false(out->hw->enabled);
    g_assert_false(state.destroy_called);
    audio_cleanup();
    audio_be_close_out(be, out);
    g_assert_false(state.destroy_called);
    g_assert_cmpint(state.last_notify, ==, AUD_CNOTIFY_DISABLE);
    audio_be_del_capture(be, cap, &state);
    g_assert_true(state.destroy_called);
    object_unref(be);
}

static unsigned int cleanup_volume_out_calls;
static unsigned int cleanup_volume_in_calls;

static void cleanup_volume_out(HWVoiceOut *hw, Volume *vol)
{
    cleanup_volume_out_calls++;
}

static void cleanup_volume_in(HWVoiceIn *hw, Volume *vol)
{
    cleanup_volume_in_calls++;
}

static void test_audio_cleanup_volume(gconstpointer data)
{
    AudioBackend *be;
    AudioMixengBackendClass *k;
    SWVoiceOut *out;
    SWVoiceIn *in;
    Volume vol = { .channels = 2, .vol = {128, 128} };

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_parse_option("none,id=cleanup");
    audio_init_audiodevs();
    be = audio_be_by_name("cleanup", &error_abort);
    out = open_test_voice_out(be, "cleanup", NULL, dummy_audio_callback);
    in = open_test_voice_in(be, "cleanup", NULL, dummy_audio_callback);
    g_assert_nonnull(in);
    k = AUDIO_MIXENG_BACKEND_GET_CLASS(be);
    /* The subprocess isolates these hooks from other tests. */
    k->volume_out = cleanup_volume_out;
    k->volume_in = cleanup_volume_in;
    audio_be_set_volume_out(be, out, &vol);
    audio_be_set_volume_in(be, in, &vol);
    g_assert_cmpuint(cleanup_volume_out_calls, ==, 1);
    g_assert_cmpuint(cleanup_volume_in_calls, ==, 1);

    audio_cleanup();
    if (GPOINTER_TO_INT(data)) {
        audio_be_set_volume_out(be, out, &vol);
    } else {
        audio_be_set_volume_in(be, in, &vol);
    }
    g_assert_cmpuint(cleanup_volume_out_calls, ==, 1);
    g_assert_cmpuint(cleanup_volume_in_calls, ==, 1);
    audio_be_close_out(be, out);
    audio_be_close_in(be, in);
}

static void test_audio_out_wav_drain(void)
{
    AudioBackend *be;
    SWVoiceOut *out;
    g_autofree char *path = NULL;
    g_autofree char *spec = NULL;
    g_autofree char *contents = NULL;
    gsize length;
    uint8_t samples[512];
    int fd;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_test_set_virtual_clock(0);
    path = g_test_build_filename(G_TEST_BUILT, "..", "..",
                                "audio-tail-drain-XXXXXX", NULL);
    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    spec = g_strdup_printf("wav,id=drain,path=%s", path);
    audio_parse_option(spec);
    audio_init_audiodevs();
    be = audio_be_by_name("drain", &error_abort);
    out = open_test_voice_out(be, "drain", NULL, dummy_audio_callback);
    audio_be_set_active_out(be, out, true);
    for (int i = 0; i < sizeof(samples) / 2; i++) {
        stw_le_p(samples + 2 * i, i - 128);
    }
    g_assert_cmpuint(audio_be_write(be, out, samples, sizeof(samples)), ==,
                     sizeof(samples));
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-stage");
    g_assert_true(out->empty);
    g_assert_cmpuint(out->hw->pending_emul, ==, sizeof(samples));
    audio_test_set_virtual_clock(SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-prefix");
    g_assert_cmpuint(out->hw->pending_emul, >, 0);
    g_assert_cmpuint(out->hw->pending_emul, <, sizeof(samples));
    audio_be_set_active_out(be, out, false);

    audio_test_set_virtual_clock(10 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-drain");
    audio_test_set_virtual_clock(11 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-disable");
    g_assert_false(out->hw->enabled);
    audio_cleanup();
    g_assert_cmpuint(out->hw->pending_emul, ==, 0);
    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
    g_assert_cmpuint(length, ==, 44 + sizeof(samples));
    g_assert_cmpmem(contents + 44, sizeof(samples), samples, sizeof(samples));
    g_assert_cmpuint(ldl_le_p(contents + 4), ==, length - 8);
    g_assert_cmpuint(ldl_le_p(contents + 40), ==, sizeof(samples));
    audio_be_close_out(be, out);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void write_test_wav(const char *path, uint16_t encoding,
                           uint16_t channels, uint32_t rate, uint16_t bits,
                           const uint8_t *data, size_t data_size,
                           uint32_t declared_data_size, bool junk_chunk,
                           bool pcm_extension)
{
    uint8_t riff[12] = { 0 };
    uint8_t chunk[8] = { 0 };
    uint8_t fmt[18] = { 0 };
    size_t format_size = pcm_extension ? 18 : 16;
    uint32_t block_align = channels * bits / 8;
    uint32_t riff_size = 4 + (junk_chunk ? 12 : 0) + 8 + format_size +
                         8 + declared_data_size + (declared_data_size & 1);
    FILE *file;

    memcpy(riff, "RIFF", 4);
    stl_le_p(riff + 4, riff_size);
    memcpy(riff + 8, "WAVE", 4);
    stw_le_p(fmt, encoding);
    stw_le_p(fmt + 2, channels);
    stl_le_p(fmt + 4, rate);
    stl_le_p(fmt + 8, rate * block_align);
    stw_le_p(fmt + 12, block_align);
    stw_le_p(fmt + 14, bits);

    file = g_fopen(path, "wb");
    g_assert_nonnull(file);
    g_assert_cmpuint(fwrite(riff, sizeof(riff), 1, file), ==, 1);
    if (junk_chunk) {
        memcpy(chunk, "JUNK", 4);
        stl_le_p(chunk + 4, 3);
        g_assert_cmpuint(fwrite(chunk, sizeof(chunk), 1, file), ==, 1);
        g_assert_cmpint(fwrite("xyz\0", 4, 1, file), ==, 1);
    }
    memcpy(chunk, "fmt ", 4);
    stl_le_p(chunk + 4, format_size);
    g_assert_cmpuint(fwrite(chunk, sizeof(chunk), 1, file), ==, 1);
    g_assert_cmpuint(fwrite(fmt, format_size, 1, file), ==, 1);
    memcpy(chunk, "data", 4);
    stl_le_p(chunk + 4, declared_data_size);
    g_assert_cmpuint(fwrite(chunk, sizeof(chunk), 1, file), ==, 1);
    if (data_size) {
        g_assert_cmpuint(fwrite(data, data_size, 1, file), ==, 1);
    }
    if (data_size == declared_data_size && (data_size & 1)) {
        g_assert_cmpint(fputc(0, file), ==, 0);
    }
    g_assert_cmpint(fclose(file), ==, 0);
}

static void write_test_wav_s16(const char *path, const int16_t *samples,
                               size_t frames, uint32_t rate, uint16_t channels,
                               bool junk_chunk, bool pcm_extension)
{
    size_t sample_count = frames * channels;
    g_autofree uint8_t *data = g_malloc(sample_count * sizeof(*samples));

    for (size_t i = 0; i < sample_count; i++) {
        stw_le_p(data + i * sizeof(*samples), samples[i]);
    }
    write_test_wav(path, 1, channels, rate, 16, data,
                   sample_count * sizeof(*samples),
                   sample_count * sizeof(*samples), junk_chunk, pcm_extension);
}

static void add_test_wav_backend(const char *id, const char *input_path,
                                 bool loop, unsigned int output_voices,
                                 const char *output_path)
{
    g_autofree char *spec = g_strdup_printf(
        "wav,id=%s,input-path=%s,input-loop=%s,out.voices=%u,in.voices=1%s%s",
        id, input_path, loop ? "on" : "off", output_voices,
        output_path ? ",path=" : "", output_path ?: "");

    audio_parse_option(spec);
}

static void add_test_wav_no_mix_backend(const char *id, const char *input_path,
                                        unsigned int output_voices)
{
    g_autofree char *spec = g_strdup_printf(
        "wav,id=%s,input-path=%s,out.voices=%u,in.voices=1,"
        "in.mixing-engine=off,in.fixed-settings=off",
        id, input_path, output_voices);

    audio_parse_option(spec);
}

static void write_test_bad_wav_chunk(const char *path)
{
    uint8_t bytes[20] = { 0 };
    FILE *file;

    memcpy(bytes, "RIFF", 4);
    stl_le_p(bytes + 4, 12);
    memcpy(bytes + 8, "WAVEfmt ", 8);
    stl_le_p(bytes + 16, UINT32_MAX);
    file = g_fopen(path, "wb");
    g_assert_nonnull(file);
    g_assert_cmpuint(fwrite(bytes, sizeof(bytes), 1, file), ==, 1);
    g_assert_cmpint(fclose(file), ==, 0);
}

static void write_test_bad_wav_format_size(const char *path)
{
    uint8_t bytes[38] = { 0 };
    FILE *file;

    memcpy(bytes, "RIFF", 4);
    stl_le_p(bytes + 4, sizeof(bytes) - 8);
    memcpy(bytes + 8, "WAVEfmt ", 8);
    stl_le_p(bytes + 16, 17);
    stw_le_p(bytes + 20, 1);
    stw_le_p(bytes + 22, 1);
    stl_le_p(bytes + 24, SAMPLE_RATE);
    stl_le_p(bytes + 28, SAMPLE_RATE * sizeof(int16_t));
    stw_le_p(bytes + 32, sizeof(int16_t));
    stw_le_p(bytes + 34, 16);
    file = g_fopen(path, "wb");
    g_assert_nonnull(file);
    g_assert_cmpuint(fwrite(bytes, sizeof(bytes), 1, file), ==, 1);
    g_assert_cmpint(fclose(file), ==, 0);
}

static void test_audio_in_wav(void)
{
    static const int16_t short_samples[] = {
        0x1234, -0x1234, 0x2345, -0x2345,
        0x3456, -0x3456, 0x4567, -0x4567,
    };
    static const int16_t loop_samples[] = {
        0x0102, -0x0102, 0x0304, -0x0304,
    };
    static const uint8_t u8_samples[] = { 0xff, 0x00, 0x00, 0xff };
    static const uint8_t invalid_data[8] = { 0 };
    static const struct {
        const char *id;
        uint16_t encoding;
        uint16_t channels;
        uint32_t rate;
        uint16_t bits;
        size_t actual_size;
        uint32_t declared_size;
    } invalid[] = {
        { "bad-encoding", 3, 1, 44100, 16, 2, 2 },
        { "bad-channels", 1, 3, 44100, 16, 6, 6 },
        { "bad-rate", 1, 1, 7999, 16, 2, 2 },
        { "bad-high-rate", 1, 1, 192001, 16, 2, 2 },
        { "bad-zero-rate", 1, 1, 0, 16, 2, 2 },
        { "bad-bits", 1, 2, 44100, 32, 8, 8 },
        { "bad-alignment", 1, 2, 44100, 16, 3, 3 },
        { "bad-truncation", 1, 2, 44100, 16, 4, 8 },
    };
    g_autofree char *directory = NULL;
    g_autofree char *input_path = NULL;
    g_autofree char *loop_path = NULL;
    g_autofree char *resample_path = NULL;
    g_autofree char *u8_path = NULL;
    g_autofree char *alias_path = NULL;
    g_autofree char *bad_chunk_path = NULL;
    g_autofree char *bad_format_path = NULL;
    g_autofree char *missing_path = NULL;
    g_autofree char *original_contents = NULL;
    g_autofree char *after_contents = NULL;
    g_autofree int16_t *resample_samples = NULL;
    uint8_t actual[1024 * CHANNELS * sizeof(int16_t)];
    uint8_t short_actual[sizeof(short_samples)];
    AudioBackend *be, *alias_be;
    SWVoiceIn *voice, *alias_voice;
    SWVoiceOut *output_voice;
    size_t bytes, original_size, after_size;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_test_set_virtual_clock(0);
    directory = g_dir_make_tmp("qemu-wav-input-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_assert_cmpint(g_chdir(directory), ==, 0);
    input_path = g_build_filename(directory, "input.wav", NULL);
    loop_path = g_build_filename(directory, "loop.wav", NULL);
    resample_path = g_build_filename(directory, "resample.wav", NULL);
    u8_path = g_build_filename(directory, "u8.wav", NULL);
    alias_path = g_build_filename(directory, "input-alias.wav", NULL);
    bad_chunk_path = g_build_filename(directory, "bad-chunk.wav", NULL);
    bad_format_path = g_build_filename(directory, "bad-format.wav", NULL);
    missing_path = g_build_filename(directory, "missing.wav", NULL);

    write_test_wav_s16(input_path, short_samples, 4, SAMPLE_RATE, CHANNELS,
                       true, false);
    write_test_wav_s16(loop_path, loop_samples, 2, SAMPLE_RATE, CHANNELS,
                       false, false);
    resample_samples = g_new(int16_t, 32);
    for (size_t i = 0; i < 32; i++) {
        resample_samples[i] = 12000;
    }
    write_test_wav_s16(resample_path, resample_samples, 32, SAMPLE_RATE / 2,
                       1, false, true);
    write_test_wav(u8_path, 1, 2, SAMPLE_RATE, 8, u8_samples,
                   sizeof(u8_samples), sizeof(u8_samples), false, false);
#ifndef _WIN32
    g_assert_cmpint(link(input_path, alias_path), ==, 0);
#else
    g_free(alias_path);
    alias_path = g_strdup(input_path);
#endif
    write_test_bad_wav_chunk(bad_chunk_path);
    write_test_bad_wav_format_size(bad_format_path);

    add_test_wav_backend("wav-input", input_path, false, 0, NULL);
    add_test_wav_backend("wav-loop", loop_path, true, 1, NULL);
    add_test_wav_backend("wav-resample", resample_path, true, 1, NULL);
    add_test_wav_backend("wav-u8", u8_path, false, 1, NULL);
    add_test_wav_backend("wav-alias", input_path, false, 1, alias_path);
    add_test_wav_backend("wav-bad-chunk", bad_chunk_path, false, 1, NULL);
    add_test_wav_backend("wav-bad-format", bad_format_path, false, 1, NULL);
    add_test_wav_backend("wav-missing", missing_path, false, 1, NULL);
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        g_autofree char *path = g_strdup_printf("%s/%s.wav", directory,
                                                 invalid[i].id);
        write_test_wav(path, invalid[i].encoding, invalid[i].channels,
                       invalid[i].rate, invalid[i].bits, invalid_data,
                       invalid[i].actual_size, invalid[i].declared_size,
                       false, false);
        add_test_wav_backend(invalid[i].id, path, false, 1, NULL);
    }
    audio_init_audiodevs();

    be = audio_be_by_name("wav-input", &error_abort);
    voice = audio_be_open_in(be, NULL, "wav-input", NULL,
                             dummy_audio_callback, &default_test_settings);
    g_assert_nonnull(voice);
    audio_be_set_active_in(be, voice, true);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-input-frozen");
    g_assert_cmpuint(audio_be_read(be, voice, short_actual,
                                   sizeof(short_actual)), ==, 0);
    audio_test_set_virtual_clock(10 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-input");
    bytes = audio_be_read(be, voice, short_actual, sizeof(short_actual));
    g_assert_cmpuint(bytes, ==, sizeof(short_samples));
    for (size_t i = 0; i < ARRAY_SIZE(short_samples); i++) {
        g_assert_cmpint((int16_t)lduw_le_p(short_actual + i * 2), ==,
                        short_samples[i]);
    }
    g_assert_cmpuint(audio_be_read(be, voice, short_actual,
                                   sizeof(short_actual)), ==, 0);
    audio_be_set_active_in(be, voice, false);
    audio_be_close_in(be, voice);

    be = audio_be_by_name("wav-loop", &error_abort);
    audio_test_set_virtual_clock(0);
    voice = audio_be_open_in(be, NULL, "wav-loop", NULL,
                             dummy_audio_callback, &default_test_settings);
    g_assert_nonnull(voice);
    audio_be_set_active_in(be, voice, true);
    audio_test_set_virtual_clock(100 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-loop");
    bytes = audio_be_read(be, voice, actual, 6 * CHANNELS * sizeof(int16_t));
    g_assert_cmpuint(bytes, ==, 6 * CHANNELS * sizeof(int16_t));
    for (size_t i = 0; i < 6; i++) {
        size_t source = (i % 2) * CHANNELS;
        g_assert_cmpint((int16_t)lduw_le_p(actual + i * 4), ==,
                        loop_samples[source]);
        g_assert_cmpint((int16_t)lduw_le_p(actual + i * 4 + 2), ==,
                        loop_samples[source + 1]);
    }
    audio_be_set_active_in(be, voice, false);
    audio_be_close_in(be, voice);

    be = audio_be_by_name("wav-resample", &error_abort);
    audio_test_set_virtual_clock(0);
    voice = audio_be_open_in(be, NULL, "wav-resample", NULL,
                             dummy_audio_callback, &default_test_settings);
    g_assert_nonnull(voice);
    audio_be_set_active_in(be, voice, true);
    audio_test_set_virtual_clock(100 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-resample");
    bytes = audio_be_read(be, voice, actual, 16 * CHANNELS * sizeof(int16_t));
    g_assert_cmpuint(bytes, ==, 16 * CHANNELS * sizeof(int16_t));
    for (size_t i = 0; i < bytes / sizeof(int16_t); i++) {
        g_assert_cmpint(ABS((int16_t)lduw_le_p(actual + i * 2) - 12000), <=, 8);
    }
    audio_be_set_active_in(be, voice, false);
    audio_be_close_in(be, voice);

    be = audio_be_by_name("wav-u8", &error_abort);
    audio_test_set_virtual_clock(0);
    voice = audio_be_open_in(be, NULL, "wav-u8", NULL,
                             dummy_audio_callback, &default_test_settings);
    g_assert_nonnull(voice);
    audio_be_set_active_in(be, voice, true);
    audio_test_set_virtual_clock(50 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-u8");
    bytes = audio_be_read(be, voice, actual, 2 * CHANNELS * sizeof(int16_t));
    g_assert_cmpuint(bytes, ==, 2 * CHANNELS * sizeof(int16_t));
    g_assert_cmpint((int16_t)lduw_le_p(actual), >, 30000);
    g_assert_cmpint((int16_t)lduw_le_p(actual + 2), <, -30000);
    audio_be_set_active_in(be, voice, false);
    audio_be_close_in(be, voice);

    alias_be = audio_be_by_name("wav-alias", &error_abort);
    g_assert_true(g_file_get_contents(input_path, &original_contents,
                                      &original_size, NULL));
    output_voice = audio_be_open_out(alias_be, NULL, "wav-alias", NULL,
                                     dummy_audio_callback,
                                     &default_test_settings);
    g_assert_null(output_voice);
    g_assert_true(g_file_get_contents(input_path, &after_contents,
                                      &after_size, NULL));
    g_assert_cmpuint(after_size, ==, original_size);
    g_assert_cmpmem(after_contents, after_size,
                    original_contents, original_size);
    alias_voice = audio_be_open_in(alias_be, NULL, "wav-alias", NULL,
                                   dummy_audio_callback,
                                   &default_test_settings);
    g_assert_null(alias_voice);

    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        SWVoiceIn *bad_voice;
        AudioBackend *bad_be = audio_be_by_name(invalid[i].id, &error_abort);
        bad_voice = audio_be_open_in(bad_be, NULL, invalid[i].id, NULL,
                                     dummy_audio_callback,
                                     &default_test_settings);
        g_assert_null(bad_voice);
    }
    g_assert_null(audio_be_open_in(audio_be_by_name("wav-bad-chunk",
                                                   &error_abort),
                                   NULL, "wav-bad-chunk", NULL,
                                   dummy_audio_callback,
                                   &default_test_settings));
    g_assert_null(audio_be_open_in(audio_be_by_name("wav-bad-format",
                                                   &error_abort),
                                   NULL, "wav-bad-format", NULL,
                                   dummy_audio_callback,
                                   &default_test_settings));
    g_assert_null(audio_be_open_in(audio_be_by_name("wav-missing",
                                                   &error_abort),
                                   NULL, "wav-missing", NULL,
                                   dummy_audio_callback,
                                   &default_test_settings));
    g_assert_false(g_file_test("qemu.wav", G_FILE_TEST_EXISTS));
    audio_cleanup();
}

static void test_audio_in_wav_clock(void)
{
    enum { INPUT_FRAMES = 2048, INPUT_BUFFER_FRAMES = 1024 };
    g_autofree char *directory = NULL;
    g_autofree char *path = NULL;
    g_autofree int16_t *samples = NULL;
    uint8_t actual[INPUT_BUFFER_FRAMES * CHANNELS * sizeof(int16_t)];
    AudioBackend *be;
    SWVoiceIn *voice;
    size_t bytes;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_test_set_virtual_clock(0);
    directory = g_dir_make_tmp("qemu-wav-clock-XXXXXX", NULL);
    g_assert_nonnull(directory);
    path = g_build_filename(directory, "clock.wav", NULL);
    samples = g_new(int16_t, INPUT_FRAMES * CHANNELS);
    for (size_t frame = 0; frame < INPUT_FRAMES; frame++) {
        samples[frame * CHANNELS] = 1000 + frame;
        samples[frame * CHANNELS + 1] = -(1000 + frame);
    }
    write_test_wav_s16(path, samples, INPUT_FRAMES, SAMPLE_RATE, CHANNELS,
                       false, false);
    add_test_wav_backend("wav-clock", path, false, 0, NULL);
    audio_init_audiodevs();

    be = audio_be_by_name("wav-clock", &error_abort);
    voice = audio_be_open_in(be, NULL, "wav-clock", NULL,
                             dummy_audio_callback, &default_test_settings);
    g_assert_nonnull(voice);
    audio_be_set_active_in(be, voice, true);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-frozen-clock");
    g_assert_cmpuint(audio_be_read(be, voice, actual, sizeof(actual)), ==, 0);

    audio_test_set_virtual_clock(10 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-ten-ms");
    bytes = audio_be_read(be, voice, actual, sizeof(actual));
    g_assert_cmpuint(bytes, ==, 441 * CHANNELS * sizeof(int16_t));
    g_assert_cmpint((int16_t)lduw_le_p(actual), ==, 1000);
    g_assert_cmpint((int16_t)lduw_le_p(actual + (441 - 1) * 4), ==,
                    1000 + 441 - 1);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-no-new-time");
    g_assert_cmpuint(audio_be_read(be, voice, actual, sizeof(actual)), ==, 0);

    audio_test_set_virtual_clock(15 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-five-ms");
    bytes = audio_be_read(be, voice, actual, sizeof(actual));
    g_assert_cmpuint(bytes, ==, 220 * CHANNELS * sizeof(int16_t));
    g_assert_cmpint((int16_t)lduw_le_p(actual), ==, 1000 + 441);
    g_assert_cmpint((int16_t)lduw_le_p(actual + (220 - 1) * 4), ==,
                    1000 + 441 + 220 - 1);

    audio_be_set_active_in(be, voice, false);
    audio_test_set_virtual_clock(1000 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-paused");
    audio_be_set_active_in(be, voice, true);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-resume-without-time");
    g_assert_cmpuint(audio_be_read(be, voice, actual, sizeof(actual)), ==, 0);

    audio_test_set_virtual_clock(1010 * SCALE_MS);
    audio_run(AUDIO_MIXENG_BACKEND(be), "test-wav-resume-ten-ms");
    bytes = audio_be_read(be, voice, actual, sizeof(actual));
    g_assert_cmpuint(bytes, ==, 441 * CHANNELS * sizeof(int16_t));
    g_assert_cmpint((int16_t)lduw_le_p(actual), ==, 1000 + 441 + 220);
    g_assert_cmpint((int16_t)lduw_le_p(actual + 2), ==,
                    -(1000 + 441 + 220));
    audio_be_set_active_in(be, voice, false);
    audio_be_close_in(be, voice);
    audio_cleanup();
    g_assert_false(g_file_test("qemu.wav", G_FILE_TEST_EXISTS));
}

static void test_audio_in_wav_no_mix(void)
{
    static const uint8_t u8_samples[] = { 0xff, 0x00, 0x00, 0xff };
    g_autofree char *directory = NULL;
    g_autofree char *exact_path = NULL;
    g_autofree char *rate_path = NULL;
    g_autofree char *channels_path = NULL;
    g_autofree char *format_path = NULL;
    g_autofree int16_t *samples = NULL;
    g_autofree int16_t *mono_samples = NULL;
    uint8_t actual[1024 * CHANNELS * sizeof(int16_t)];
    AudioBackend *be;
    SWVoiceIn *voice;
    size_t bytes;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed();
        return;
    }

    audio_test_set_virtual_clock(0);
    directory = g_dir_make_tmp("qemu-wav-nomix-XXXXXX", NULL);
    g_assert_nonnull(directory);
    exact_path = g_build_filename(directory, "exact.wav", NULL);
    rate_path = g_build_filename(directory, "rate.wav", NULL);
    channels_path = g_build_filename(directory, "channels.wav", NULL);
    format_path = g_build_filename(directory, "format.wav", NULL);
    samples = g_new(int16_t, 2048 * CHANNELS);
    mono_samples = g_new(int16_t, 32);
    for (size_t frame = 0; frame < 2048; frame++) {
        samples[frame * CHANNELS] = 1000 + frame;
        samples[frame * CHANNELS + 1] = -(1000 + frame);
    }
    for (size_t frame = 0; frame < 32; frame++) {
        mono_samples[frame] = 1000 + frame;
    }
    write_test_wav_s16(exact_path, samples, 2048, SAMPLE_RATE, CHANNELS,
                       false, false);
    write_test_wav_s16(rate_path, samples, 32, SAMPLE_RATE / 2, CHANNELS,
                       false, false);
    write_test_wav_s16(channels_path, mono_samples, 32, SAMPLE_RATE, 1,
                       false, false);
    write_test_wav(format_path, 1, 2, SAMPLE_RATE, 8, u8_samples,
                   sizeof(u8_samples), sizeof(u8_samples), false, false);

    add_test_wav_no_mix_backend("wav-nomix-exact", exact_path, 0);
    add_test_wav_no_mix_backend("wav-nomix-rate", rate_path, 1);
    add_test_wav_no_mix_backend("wav-nomix-channels", channels_path, 1);
    add_test_wav_no_mix_backend("wav-nomix-format", format_path, 1);
    audio_init_audiodevs();

    be = audio_be_by_name("wav-nomix-exact", &error_abort);
    voice = audio_be_open_in(be, NULL, "wav-nomix-exact", NULL,
                             dummy_audio_callback, &default_test_settings);
    g_assert_nonnull(voice);
    audio_be_set_active_in(be, voice, true);
    audio_test_set_virtual_clock(100 * SCALE_MS);
    bytes = audio_be_read(be, voice, actual, sizeof(actual));
    g_assert_cmpuint(bytes, ==, sizeof(actual));
    g_assert_cmpint((int16_t)lduw_le_p(actual), ==, 1000);
    g_assert_cmpint((int16_t)lduw_le_p(actual + (1024 - 1) * 4), ==,
                    1000 + 1024 - 1);
    audio_be_set_active_in(be, voice, false);
    audio_be_close_in(be, voice);

    for (const char *id[] = { "wav-nomix-rate", "wav-nomix-channels",
                              "wav-nomix-format" }, **name = id;
         name < id + ARRAY_SIZE(id); name++) {
        be = audio_be_by_name(*name, &error_abort);
        voice = audio_be_open_in(be, NULL, *name, NULL,
                                 dummy_audio_callback,
                                 &default_test_settings);
        g_assert_null(voice);
    }
    audio_cleanup();
}

#ifndef _WIN32
static void test_audio_in_wav_fifo(void)
{
    g_autofree char *directory = NULL;
    g_autofree char *path = NULL;
    Error *err = NULL;
    WavInput *input;

    directory = g_dir_make_tmp("qemu-wav-fifo-XXXXXX", NULL);
    g_assert_nonnull(directory);
    path = g_build_filename(directory, "input.fifo", NULL);
    g_assert_cmpint(mkfifo(path, 0600), ==, 0);
    input = wav_input_open(path, false, &err);
    g_assert_null(input);
    g_assert_nonnull(err);
    error_free(err);
}
#endif

int main(int argc, char **argv)
{
    GOptionContext *context;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = NULL;
    int ret;

    context = g_option_context_new("- QEMU audio test");
    g_option_context_add_main_entries(context, test_options, NULL);
    g_option_context_set_ignore_unknown_options(context, true);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        return 1;
    }
    g_option_context_free(context);

    g_test_init(&argc, &argv, NULL);

    module_call_init(MODULE_INIT_TRACE);
    qemu_add_opts(&qemu_trace_opts);
    if (opt_trace) {
        trace_opt_parse(opt_trace);
        qemu_set_log(LOG_TRACE, &error_fatal);
    }
    trace_init_backends();
    trace_init_file();

    dir = g_test_build_filename(G_TEST_BUILT, "..", "..", NULL);
    g_setenv("QEMU_MODULE_DIR", dir, true);
    qemu_init_exec_dir(argv[0]);
    module_call_init(MODULE_INIT_QOM);
    module_init_info(qemu_modinfo);

    qemu_init_main_loop(&error_abort);
    if (opt_audiodev) {
        g_autofree gchar *spec = is_help_option(opt_audiodev) ?
            opt_audiodev : g_strdup_printf("%s,id=%s", opt_audiodev, TEST_AUDIODEV_ID);
        audio_parse_option(spec);
    }
    audio_create_default_audiodevs();
    audio_init_audiodevs();

    g_test_add_func("/audio/prio-list", test_audio_prio_list);

    g_test_add_func("/audio/out/active-state", test_audio_out_active_state);
    g_test_add_func("/audio/out/sine-wave", test_audio_out_sine_wave);
    g_test_add_func("/audio/out/buffer-size", test_audio_out_buffer_size);
    g_test_add_func("/audio/out/volume", test_audio_out_volume);
    g_test_add_func("/audio/out/capture", test_audio_capture);

    g_test_add_func("/audio/in/active-state", test_audio_in_active_state);
    g_test_add_func("/audio/in/volume", test_audio_in_volume);

    g_test_add_func("/audio/null-handling", test_audio_null_handling);
    g_test_add_func("/audio/multiple-voices", test_audio_multiple_voices);
    g_test_add_func("/audio/invalid-settings", test_audio_invalid_settings);

    g_test_add_func("/audio/cleanup/wav", test_audio_cleanup_wav);
    g_test_add_func("/audio/cleanup/voices", test_audio_cleanup_voices);
    g_test_add_func("/audio/cleanup/capture", test_audio_cleanup_capture);

    g_test_add_data_func("/audio/cleanup/volume-out", GINT_TO_POINTER(1),
                         test_audio_cleanup_volume);
    g_test_add_data_func("/audio/cleanup/volume-in", GINT_TO_POINTER(0),
                         test_audio_cleanup_volume);

    g_test_add_func("/audio/out/wav-drain", test_audio_out_wav_drain);
    g_test_add_func("/audio/in/wav", test_audio_in_wav);
    g_test_add_func("/audio/in/wav-clock", test_audio_in_wav_clock);
    g_test_add_func("/audio/in/wav-no-mix", test_audio_in_wav_no_mix);
#ifndef _WIN32
    g_test_add_func("/audio/in/wav-fifo", test_audio_in_wav_fifo);
#endif

    ret = g_test_run();

    audio_cleanup();

    return ret;
}
