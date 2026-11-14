/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * C interface to MAME's chd_file; see qemu-chd-shim.h.
 */

#include "qemu-chd-shim.h"

#include "chd.h"
#include "hashing.h"
#include "ioprocs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

struct ChdShimFile {
    chd_file chd;
};

namespace {

/* MAME error value for "a C++ exception escaped"; there is no better one. */
constexpr int ERR_EXCEPTION = (int)chd_file::error::INVALID_STATE;

template <typename F>
int guard(const F &fn) noexcept
{
    try {
        return fn();
    } catch (...) {
        return ERR_EXCEPTION;
    }
}

int to_err(std::error_condition e)
{
    if (!e) {
        return 0;
    }
    if (e.category() == chd_category()) {
        return e.value();
    }
    return CHD_SHIM_ERR_IO;  /* a failed read or write from our callbacks */
}

/*
 * Hands every byte MAME moves to the caller's callbacks. The callbacks run in
 * the thread that called the entry point: the compressor's worker threads
 * only work on plain files (create_from_raw, flatten), never on this class.
 */
class CallbackIO : public util::random_read_write {
public:
    CallbackIO(const ChdShimIO &ops, void *opaque)
        : m_ops(ops), m_opaque(opaque) {}

    ~CallbackIO() override
    {
        if (m_ops.release) {
            m_ops.release(m_opaque);
        }
    }

    std::error_condition seek(std::int64_t offset, int whence) noexcept override
    {
        std::uint64_t base = 0;
        switch (whence) {
        case SEEK_SET:
            break;
        case SEEK_CUR:
            base = m_pos;
            break;
        case SEEK_END:
            if (m_ops.length(m_opaque, &base)) {
                return std::errc::io_error;
            }
            break;
        default:
            return std::errc::invalid_argument;
        }
        m_pos = base + offset;
        return {};
    }

    std::error_condition tell(std::uint64_t &result) noexcept override
    {
        result = m_pos;
        return {};
    }

    std::error_condition length(std::uint64_t &result) noexcept override
    {
        return m_ops.length(m_opaque, &result) ? std::errc::io_error
                                               : std::error_condition();
    }

    std::error_condition read_some(void *buffer, std::size_t length,
                                   std::size_t &actual) noexcept override
    {
        auto err = read_some_at(m_pos, buffer, length, actual);
        m_pos += actual;
        return err;
    }

    std::error_condition read_some_at(std::uint64_t offset, void *buffer,
                                      std::size_t length,
                                      std::size_t &actual) noexcept override
    {
        actual = 0;
        return m_ops.read_at(m_opaque, offset, buffer, length, &actual)
                   ? std::errc::io_error : std::error_condition();
    }

    std::error_condition write_some(void const *buffer, std::size_t length,
                                    std::size_t &actual) noexcept override
    {
        auto err = write_some_at(m_pos, buffer, length, actual);
        m_pos += actual;
        return err;
    }

    std::error_condition write_some_at(std::uint64_t offset, void const *buffer,
                                       std::size_t length,
                                       std::size_t &actual) noexcept override
    {
        actual = 0;
        if (!m_ops.write_at) {
            return std::errc::read_only_file_system;
        }
        if (m_ops.write_at(m_opaque, offset, buffer, length)) {
            return std::errc::io_error;
        }
        actual = length;
        return {};
    }

    std::error_condition finalize() noexcept override { return {}; }
    std::error_condition flush() noexcept override { return {}; }

private:
    ChdShimIO m_ops;
    void *m_opaque;
    std::uint64_t m_pos = 0;
};

/* Feeds chd_file_compressor from a raw file. */
class RawSource : public chd_file_compressor {
public:
    explicit RawSource(std::FILE *f) : m_file(f) {}

protected:
    uint32_t read_data(void *dest, uint64_t offset, uint32_t length) override
    {
        size_t n = 0;
        if (!fseeko(m_file, (off_t)offset, SEEK_SET)) {
            n = std::fread(dest, 1, length, m_file);
        }
        std::memset((uint8_t *)dest + n, 0, length - n);
        return length;
    }

private:
    std::FILE *m_file;
};

/* Run a created compressor to the end. cancel may be null. */
int run_compressor(chd_file_compressor &comp, uint64_t logical,
                   ChdShimProgress progress, int (*cancel)(void *),
                   void *opaque)
{
    comp.compress_begin();
    for (;;) {
        if (cancel && cancel(opaque)) {
            return (int)chd_file::error::OPERATION_PENDING;
        }
        double done = 0, ratio = 0;
        auto st = comp.compress_continue(done, ratio);
        if (progress) {
            progress(opaque, (uint64_t)(done * logical), logical);
        }
        if (st == chd_file::error::WALKING_PARENT ||
            st == chd_file::error::COMPRESSING) {
            continue;
        }
        return to_err(st);
    }
}

}  /* namespace */

extern "C" {

const char *chd_shim_strerror(int err)
{
    static const char *const names[] = {
        "no error", "no interface", "file is not open", "file is already open",
        "invalid file", "invalid data", "requires a parent",
        "file is not writeable", "codec error", "invalid parent",
        "hunk out of range", "decompression error", "compression error",
        "can't verify", "metadata not found", "invalid metadata size",
        "unsupported CHD version", "verify incomplete", "invalid metadata",
        "invalid state", "operation pending", "unsupported format",
        "unknown compression", "walking parent", "compressing",
    };
    if (err == CHD_SHIM_ERR_IO) {
        return "I/O error";
    }
    if (err >= 0 && (size_t)err < sizeof(names) / sizeof(names[0])) {
        return names[err];
    }
    return "unknown CHD error";
}

int chd_shim_open(ChdShimFile **out, const ChdShimIO *io, void *opaque,
                  bool writable, ChdShimFile *parent)
{
    *out = nullptr;
    return guard([&] {
        auto f = std::make_unique<ChdShimFile>();
        util::random_read_write::ptr file =
            std::make_unique<CallbackIO>(*io, opaque);
        int err = to_err(f->chd.open(std::move(file), writable,
                                     parent ? &parent->chd : nullptr));
        if (!err) {
            *out = f.release();
        }
        return err;
    });
}

void chd_shim_close(ChdShimFile *f)
{
    guard([&] {
        delete f;
        return 0;
    });
}

void chd_shim_get_info(ChdShimFile *f, ChdShimInfo *info)
{
    guard([&] {
        chd_file &c = f->chd;
        std::memset(info, 0, sizeof(*info));
        info->version = c.version();
        info->hunk_bytes = c.hunk_bytes();
        info->hunk_count = c.hunk_count();
        info->unit_bytes = c.unit_bytes();
        info->logical_bytes = c.logical_bytes();
        for (int i = 0; i < 4; i++) {
            info->codecs[i] = c.compression(i);
        }
        info->compressed = c.compressed();
        info->has_parent = c.parent_sha1() != util::sha1_t::null;
        info->is_hd = !c.check_is_hd();
        std::memcpy(info->sha1, c.sha1().m_raw, 20);
        std::memcpy(info->raw_sha1, c.raw_sha1().m_raw, 20);
        std::memcpy(info->parent_sha1, c.parent_sha1().m_raw, 20);
        return 0;
    });
}

int chd_shim_hd_geometry(ChdShimFile *f, uint32_t *cylinders, uint32_t *heads,
                         uint32_t *sectors, uint32_t *bytes_per_sector)
{
    return guard([&] {
        std::string text;
        int err = to_err(f->chd.read_metadata(HARD_DISK_METADATA_TAG, 0, text));
        if (err) {
            return err;
        }
        /* "CYLS:%d,HEADS:%d,SECS:%d,BPS:%d" */
        unsigned c, h, s, b;
        if (std::sscanf(text.c_str(), "CYLS:%u,HEADS:%u,SECS:%u,BPS:%u",
                        &c, &h, &s, &b) != 4) {
            return (int)chd_file::error::INVALID_METADATA;
        }
        *cylinders = c;
        *heads = h;
        *sectors = s;
        *bytes_per_sector = b;
        return 0;
    });
}

int chd_shim_read_hunk(ChdShimFile *f, uint32_t hunk, void *buf)
{
    return guard([&] { return to_err(f->chd.read_hunk(hunk, buf)); });
}

int chd_shim_read_bytes(ChdShimFile *f, uint64_t offset, void *buf,
                        uint32_t len)
{
    return guard([&] { return to_err(f->chd.read_bytes(offset, buf, len)); });
}

int chd_shim_create_from_raw(const char *out, const char *raw,
                             const uint32_t codecs[4], uint32_t hunk_bytes)
{
    return guard([&] {
        std::FILE *f = std::fopen(raw, "rb");
        if (!f) {
            return CHD_SHIM_ERR_IO;
        }
        std::fseek(f, 0, SEEK_END);
        uint64_t bytes = (uint64_t)std::ftell(f);
        /* chdman's geometry: 16 heads, 63 sectors, cylinders rounded up. */
        const uint32_t heads = 16, secs = 63, bps = 512;
        uint32_t cyls = (uint32_t)((bytes / bps + heads * secs - 1) /
                                   (heads * secs));
        chd_codec_type types[4];
        for (int i = 0; i < 4; i++) {
            types[i] = codecs[i];
        }

        RawSource comp(f);
        int err = to_err(comp.create(out, bytes, hunk_bytes, bps, types));
        if (!err) {
            char geo[64];
            std::snprintf(geo, sizeof(geo), "CYLS:%u,HEADS:%u,SECS:%u,BPS:%u",
                          cyls, heads, secs, bps);
            err = to_err(comp.write_metadata(HARD_DISK_METADATA_TAG, 0,
                                             std::string(geo)));
        }
        if (!err) {
            err = run_compressor(comp, bytes, nullptr, nullptr, nullptr);
        }
        std::fclose(f);
        return err;
    });
}

}  /* extern "C" */
