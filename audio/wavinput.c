/* SPDX-License-Identifier: BSD-3-Clause */

#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

#include "wavinput.h"

struct WavInput {
    FILE *file;
    char *path;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t remaining;
    uint16_t bytes_per_frame;
    struct audsettings settings;
    bool loop;
    bool failed;
};

static bool wav_input_read_at(WavInput *input, uint64_t offset,
                              void *buffer, size_t size, Error **errp)
{
    if (offset > INT64_MAX) {
        error_setg(errp, "wav: input '%s' has an offset that is too large",
                   input->path);
        return false;
    }
    if (fseeko(input->file, (off_t)offset, SEEK_SET) < 0) {
        error_setg_errno(errp, errno, "wav: seeking input '%s'", input->path);
        return false;
    }
    if (fread(buffer, 1, size, input->file) != size) {
        int err = ferror(input->file) ? errno : EIO;
        error_setg_errno(errp, err, "wav: reading input '%s'", input->path);
        return false;
    }
    return true;
}

static bool wav_input_parse(WavInput *input, Error **errp)
{
    uint8_t header[12];
    uint8_t chunk[8];
    uint8_t format[16];
    struct stat st;
    uint64_t file_size, riff_end, position = sizeof(header);
    uint32_t riff_size;
    bool have_format = false, have_data = false;

    if (fstat(fileno(input->file), &st) < 0) {
        error_setg_errno(errp, errno,
                         "wav: inspecting input '%s'", input->path);
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        error_setg(errp, "wav: input '%s' is not a regular file", input->path);
        return false;
    }
    file_size = st.st_size;
    if (file_size < sizeof(header) ||
        !wav_input_read_at(input, 0, header, sizeof(header), errp)) {
        if (!errp || !*errp) {
            error_setg(errp, "wav: input '%s' has a truncated RIFF header",
                       input->path);
        }
        return false;
    }
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        error_setg(errp, "wav: input '%s' is not a RIFF/WAVE file",
                   input->path);
        return false;
    }

    riff_size = ldl_le_p(header + 4);
    if (riff_size < 4) {
        error_setg(errp, "wav: input '%s' has an invalid RIFF size",
                   input->path);
        return false;
    }
    riff_end = (uint64_t)riff_size + 8;
    if (riff_end > file_size) {
        error_setg(errp, "wav: input '%s' is shorter than its RIFF size",
                   input->path);
        return false;
    }

    while (position < riff_end) {
        uint64_t chunk_span, payload;
        uint32_t chunk_size;

        if (riff_end - position < sizeof(chunk) ||
            !wav_input_read_at(input, position, chunk, sizeof(chunk), errp)) {
            if (!errp || !*errp) {
                error_setg(errp, "wav: input '%s' has a truncated chunk header",
                           input->path);
            }
            return false;
        }
        chunk_size = ldl_le_p(chunk + 4);
        chunk_span = sizeof(chunk) + (uint64_t)chunk_size + (chunk_size & 1);
        if (chunk_span > riff_end - position) {
            error_setg(errp, "wav: input '%s' has a chunk beyond its RIFF size",
                       input->path);
            return false;
        }
        payload = position + sizeof(chunk);

        if (!memcmp(chunk, "fmt ", 4)) {
            uint16_t encoding, channels, block_align, bits;
            uint32_t rate, byte_rate, expected_align;

            if (have_format) {
                error_setg(errp, "wav: input '%s' has multiple format chunks",
                           input->path);
                return false;
            }
            if (chunk_size != sizeof(format) &&
                chunk_size != sizeof(format) + 2) {
                error_setg(errp,
                           "wav: input '%s' has an unsupported format chunk",
                           input->path);
                return false;
            }
            if (!wav_input_read_at(input, payload, format, sizeof(format),
                                   errp)) {
                return false;
            }
            if (chunk_size == sizeof(format) + 2) {
                uint8_t extension_size[2];

                if (!wav_input_read_at(input, payload + sizeof(format),
                                       extension_size, sizeof(extension_size),
                                       errp)) {
                    return false;
                }
                if (lduw_le_p(extension_size) != 0) {
                    error_setg(errp,
                               "wav: input '%s' has an unsupported PCM extension",
                               input->path);
                    return false;
                }
            }
            encoding = lduw_le_p(format);
            channels = lduw_le_p(format + 2);
            rate = ldl_le_p(format + 4);
            byte_rate = ldl_le_p(format + 8);
            block_align = lduw_le_p(format + 12);
            bits = lduw_le_p(format + 14);

            if (encoding != 1) {
                error_setg(errp, "wav: input '%s' is not PCM format 1",
                           input->path);
                return false;
            }
            if (channels != 1 && channels != 2) {
                error_setg(errp, "wav: input '%s' must be mono or stereo",
                           input->path);
                return false;
            }
            if (rate < 8000 || rate > 192000) {
                error_setg(errp, "wav: input '%s' has unsupported rate %u",
                           input->path, rate);
                return false;
            }
            if (bits != 8 && bits != 16) {
                error_setg(errp, "wav: input '%s' has unsupported bit depth %u",
                           input->path, bits);
                return false;
            }
            expected_align = channels * (bits / 8);
            if (block_align != expected_align ||
                byte_rate != rate * expected_align) {
                error_setg(errp, "wav: input '%s' has inconsistent PCM rates",
                           input->path);
                return false;
            }
            input->bytes_per_frame = block_align;
            input->settings = (struct audsettings) {
                .freq = rate,
                .nchannels = channels,
                .fmt = bits == 8 ? AUDIO_FORMAT_U8 : AUDIO_FORMAT_S16,
                .big_endian = false,
            };
            have_format = true;
        } else if (!memcmp(chunk, "data", 4)) {
            if (have_data) {
                error_setg(errp, "wav: input '%s' has multiple data chunks",
                           input->path);
                return false;
            }
            input->data_offset = payload;
            input->data_size = chunk_size;
            have_data = true;
        }
        position += chunk_span;
    }

    if (!have_format || !have_data) {
        error_setg(errp, "wav: input '%s' lacks a format or data chunk",
                   input->path);
        return false;
    }
    if (input->data_size % input->bytes_per_frame) {
        error_setg(errp, "wav: input '%s' data is not frame aligned",
                   input->path);
        return false;
    }

    input->remaining = input->data_size;
    if (input->data_offset > INT64_MAX ||
        fseeko(input->file, (off_t)input->data_offset, SEEK_SET) < 0) {
        error_setg_errno(errp, errno, "wav: seeking input '%s' data",
                         input->path);
        return false;
    }
    return true;
}

WavInput *wav_input_open(const char *path, bool loop, Error **errp)
{
    WavInput *input = g_new0(WavInput, 1);
    struct stat path_st, fd_st;
    int flags = O_RDONLY;
    int fd;

    input->path = g_strdup(path);
    input->loop = loop;

    if (stat(path, &path_st) < 0) {
        error_setg_errno(errp, errno, "wav: inspecting input '%s'", path);
        goto fail;
    }
    if (!S_ISREG(path_st.st_mode)) {
        error_setg(errp, "wav: input '%s' is not a regular file", path);
        goto fail;
    }

#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(path, flags);
    if (fd < 0) {
        error_setg_errno(errp, errno, "wav: opening input '%s'", path);
        goto fail;
    }
    if (fstat(fd, &fd_st) < 0) {
        int err = errno;

        close(fd);
        error_setg_errno(errp, err, "wav: inspecting input '%s'", path);
        goto fail;
    }
    if (!S_ISREG(fd_st.st_mode) || path_st.st_dev != fd_st.st_dev ||
        path_st.st_ino != fd_st.st_ino) {
        close(fd);
        error_setg(errp, "wav: input '%s' changed while opening", path);
        goto fail;
    }
    input->file = fdopen(fd, "rb");
    if (!input->file) {
        int err = errno;

        close(fd);
        error_setg_errno(errp, err, "wav: opening input '%s'", path);
        goto fail;
    }
    if (!wav_input_parse(input, errp)) {
        goto fail;
    }
    return input;

fail:
    if (input->file) {
        fclose(input->file);
    }
    g_free(input->path);
    g_free(input);
    return NULL;
}

void wav_input_get_settings(WavInput *input, struct audsettings *settings)
{
    *settings = input->settings;
}

size_t wav_input_read(WavInput *input, void *buffer, size_t size)
{
    size_t done = 0;

    if (input->failed) {
        return 0;
    }
    size -= size % input->bytes_per_frame;
    while (done < size) {
        size_t requested;
        size_t got;

        if (!input->remaining) {
            if (!input->loop || !input->data_size) {
                break;
            }
            if (fseeko(input->file, input->data_offset, SEEK_SET) < 0) {
                error_report("wav: rewinding input '%s': %s", input->path,
                             strerror(errno));
                input->failed = true;
                break;
            }
            input->remaining = input->data_size;
        }

        requested = (size_t)MIN(input->remaining, (uint64_t)(size - done));
        got = fread((uint8_t *)buffer + done, 1, requested, input->file);
        input->remaining -= got;
        if (got != requested) {
            size_t complete = got - got % input->bytes_per_frame;

            error_report("wav: reading input '%s': %s", input->path,
                         ferror(input->file) ? strerror(errno ? errno : EIO) :
                         "short read");
            input->failed = true;
            done += complete;
            break;
        }
        done += got;
    }
    return done;
}

void wav_input_close(WavInput *input)
{
    if (input->file && fclose(input->file) < 0) {
        error_report("wav: closing input '%s': %s", input->path,
                     strerror(errno));
    }
    g_free(input->path);
    g_free(input);
}
