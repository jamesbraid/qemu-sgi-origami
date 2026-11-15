/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI video frames. See the header for the layout.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "hw/display/sgi/video-port.h"

bool sgi_video_format_valid(const SGIVideoFormat *fmt)
{
    return fmt->width && fmt->height && !(fmt->width & 1) &&
           fmt->width <= SGI_VIDEO_MAX_WIDTH &&
           fmt->height <= SGI_VIDEO_MAX_HEIGHT &&
           fmt->rate_num && fmt->rate_den &&
           (fmt->depth == 8 || fmt->depth == 10) &&
           fmt->fields <= SGI_VIDEO_BFF;
}

SGIVideoFrame *sgi_video_frame_new(const SGIVideoFormat *fmt)
{
    SGIVideoFrame *f = g_new0(SGIVideoFrame, 1);
    size_t luma = (size_t)fmt->width * fmt->height;
    uint16_t black = 16 << (fmt->depth - 8), grey = 128 << (fmt->depth - 8);

    f->fmt = *fmt;
    f->y = g_new(uint16_t, luma * 2);
    f->cb = f->y + luma;
    f->cr = f->cb + luma / 2;
    for (size_t i = 0; i < luma; i++) {
        f->y[i] = black;
    }
    for (size_t i = 0; i < luma / 2; i++) {
        f->cb[i] = f->cr[i] = grey;
    }
    return f;
}

SGIVideoFrame *sgi_video_frame_dup(const SGIVideoFrame *f)
{
    SGIVideoFrame *d = sgi_video_frame_new(&f->fmt);

    memcpy(d->y, f->y, (size_t)f->fmt.width * f->fmt.height * 2 *
                       sizeof(uint16_t));
    return d;
}

void sgi_video_frame_free(SGIVideoFrame *f)
{
    if (f) {
        g_free(f->y);
        g_free(f);
    }
}

size_t sgi_video_frame_bytes(const SGIVideoFormat *fmt)
{
    return (size_t)fmt->width * fmt->height * 2 * (fmt->depth > 8 ? 2 : 1);
}

static uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void sgi_video_row_rgb(const SGIVideoFrame *f, unsigned y, uint32_t *dst)
{
    unsigned shift = f->fmt.depth - 8;
    bool hd = f->fmt.width > 720;
    /* Integer BT.601 and BT.709 limited-range matrices, 8 fractional bits. */
    int rv = hd ? 459 : 409, gu = hd ? 55 : 100, gv = hd ? 136 : 208;
    int bu = hd ? 541 : 516;

    for (unsigned x = 0; x < f->fmt.width; x++) {
        size_t c = (size_t)y * (f->fmt.width / 2) + x / 2;
        int l = 298 * ((f->y[(size_t)y * f->fmt.width + x] >> shift) - 16);
        int u = (f->cb[c] >> shift) - 128;
        int v = (f->cr[c] >> shift) - 128;

        dst[x] = (clamp8((l + rv * v + 128) >> 8) << 16) |
                 (clamp8((l - gu * u - gv * v + 128) >> 8) << 8) |
                 clamp8((l + bu * u + 128) >> 8);
    }
}

static const struct {
    const char *name;
    unsigned depth;
} packings[SGI_PACK_COUNT] = {
    [SGI_PACK_8] = { "8", 8 },
    [SGI_PACK_10] = { "10", 10 },
    [SGI_PACK_10IN16L] = { "10in16l", 10 },
    [SGI_PACK_10IN16R] = { "10in16r", 10 },
};

int sgi_video_packing_parse(const char *name)
{
    for (int i = 0; i < SGI_PACK_COUNT; i++) {
        if (!strcmp(packings[i].name, name)) {
            return i;
        }
    }
    return -1;
}

const char *sgi_video_packing_name(SGIVideoPacking p)
{
    return packings[p].name;
}

unsigned sgi_video_packing_depth(SGIVideoPacking p)
{
    return packings[p].depth;
}

size_t sgi_video_packed_row_bytes(SGIVideoPacking p, unsigned width)
{
    switch (p) {
    case SGI_PACK_8:
        return 2 * (size_t)width;
    case SGI_PACK_10:
        return 5 * (size_t)width / 2;
    default:
        return 4 * (size_t)width;
    }
}

/* Samples of a row in memory order: Cb Y Cr Y for each pair of pixels. */
void sgi_video_unpack_row(SGIVideoPacking p, const uint8_t *src,
                          SGIVideoFrame *f, unsigned y)
{
    size_t luma = (size_t)y * f->fmt.width;
    size_t chroma = (size_t)y * (f->fmt.width / 2);

    for (unsigned i = 0; i < f->fmt.width / 2; i++) {
        uint16_t s[4];

        switch (p) {
        case SGI_PACK_8:
            for (int k = 0; k < 4; k++) {
                s[k] = src[4 * i + k];
            }
            break;
        case SGI_PACK_10: {
            uint64_t b = 0;

            for (int k = 0; k < 5; k++) {
                b = (b << 8) | src[5 * i + k];
            }
            for (int k = 0; k < 4; k++) {
                s[k] = (b >> (30 - 10 * k)) & 0x3ff;
            }
            break;
        }
        case SGI_PACK_10IN16L:
            for (int k = 0; k < 4; k++) {
                s[k] = lduw_be_p(src + 8 * i + 2 * k) >> 6;
            }
            break;
        default:
            for (int k = 0; k < 4; k++) {
                s[k] = lduw_be_p(src + 8 * i + 2 * k) & 0x3ff;
            }
            break;
        }
        f->cb[chroma + i] = s[0];
        f->y[luma + 2 * i] = s[1];
        f->cr[chroma + i] = s[2];
        f->y[luma + 2 * i + 1] = s[3];
    }
}

static uint16_t scale_depth(uint16_t v, unsigned from, unsigned to)
{
    return to >= from ? v << (to - from) : v >> (from - to);
}

void sgi_video_pack_row(SGIVideoPacking p, const SGIVideoFrame *f,
                        unsigned y, uint8_t *dst)
{
    size_t luma = (size_t)y * f->fmt.width;
    size_t chroma = (size_t)y * (f->fmt.width / 2);
    unsigned to = packings[p].depth;

    for (unsigned i = 0; i < f->fmt.width / 2; i++) {
        uint16_t s[4] = {
            f->cb[chroma + i], f->y[luma + 2 * i],
            f->cr[chroma + i], f->y[luma + 2 * i + 1],
        };

        for (int k = 0; k < 4; k++) {
            s[k] = scale_depth(s[k], f->fmt.depth, to);
        }
        switch (p) {
        case SGI_PACK_8:
            for (int k = 0; k < 4; k++) {
                dst[4 * i + k] = s[k];
            }
            break;
        case SGI_PACK_10: {
            uint64_t b = ((uint64_t)s[0] << 30) | ((uint64_t)s[1] << 20) |
                         (s[2] << 10) | s[3];

            for (int k = 0; k < 5; k++) {
                dst[5 * i + k] = b >> (32 - 8 * k);
            }
            break;
        }
        case SGI_PACK_10IN16L:
            /* The six pad bits repeat the sample's top bits. */
            for (int k = 0; k < 4; k++) {
                stw_be_p(dst + 8 * i + 2 * k, (s[k] << 6) | (s[k] >> 4));
            }
            break;
        default:
            for (int k = 0; k < 4; k++) {
                stw_be_p(dst + 8 * i + 2 * k, s[k]);
            }
            break;
        }
    }
}

char *sgi_y4m_header(const SGIVideoFormat *fmt)
{
    static const char order[] = { 'p', 't', 'b' };

    return g_strdup_printf("YUV4MPEG2 W%u H%u F%u:%u I%c A0:0 C422%s\n",
                           fmt->width, fmt->height, fmt->rate_num,
                           fmt->rate_den, order[fmt->fields],
                           fmt->depth > 8 ? "p10" : "");
}

bool sgi_y4m_parse_header(const char *line, SGIVideoFormat *fmt, Error **errp)
{
    g_auto(GStrv) tok = g_strsplit(line, " ", -1);
    SGIVideoFormat f = { .fields = SGI_VIDEO_PROGRESSIVE, .depth = 0 };
    bool chroma_ok = false;

    if (!tok[0] || strcmp(tok[0], "YUV4MPEG2")) {
        error_setg(errp, "not a Y4M header");
        return false;
    }
    for (char **t = tok + 1; *t; t++) {
        char *p = g_strchomp(*t);

        switch (p[0]) {
        case 'W':
            f.width = g_ascii_strtoull(p + 1, NULL, 10);
            break;
        case 'H':
            f.height = g_ascii_strtoull(p + 1, NULL, 10);
            break;
        case 'F':
            if (sscanf(p + 1, "%u:%u", &f.rate_num, &f.rate_den) != 2) {
                f.rate_num = 0;
            }
            break;
        case 'I':
            f.fields = p[1] == 't' ? SGI_VIDEO_TFF :
                       p[1] == 'b' ? SGI_VIDEO_BFF : SGI_VIDEO_PROGRESSIVE;
            break;
        case 'C':
            chroma_ok = !strcmp(p + 1, "422") || !strcmp(p + 1, "422p10");
            f.depth = strcmp(p + 1, "422p10") ? 8 : 10;
            break;
        }
    }
    if (!chroma_ok) {
        error_setg(errp, "only 4:2:2 Y4M (C422, C422p10) is supported");
        return false;
    }
    if (!sgi_video_format_valid(&f)) {
        error_setg(errp, "Y4M header has an unusable raster or rate");
        return false;
    }
    *fmt = f;
    return true;
}

size_t sgi_y4m_frame_put(const SGIVideoFrame *f, uint8_t *dst)
{
    size_t luma = (size_t)f->fmt.width * f->fmt.height;
    const uint16_t *plane[3] = { f->y, f->cb, f->cr };
    size_t count[3] = { luma, luma / 2, luma / 2 };
    uint8_t *p = dst;

    memcpy(p, "FRAME\n", 6);
    p += 6;
    for (int k = 0; k < 3; k++) {
        for (size_t i = 0; i < count[k]; i++) {
            if (f->fmt.depth > 8) {
                stw_le_p(p, plane[k][i]);
                p += 2;
            } else {
                *p++ = plane[k][i];
            }
        }
    }
    return p - dst;
}

bool sgi_y4m_parse_frame_line(const char *line, Error **errp)
{
    if (strcmp(line, "FRAME")) {
        error_setg(errp, "Y4M frame marker is not a plain FRAME line");
        return false;
    }
    return true;
}

void sgi_y4m_frame_get(SGIVideoFrame *f, const uint8_t *src)
{
    size_t luma = (size_t)f->fmt.width * f->fmt.height;
    uint16_t *plane[3] = { f->y, f->cb, f->cr };
    size_t count[3] = { luma, luma / 2, luma / 2 };

    for (int k = 0; k < 3; k++) {
        for (size_t i = 0; i < count[k]; i++) {
            if (f->fmt.depth > 8) {
                plane[k][i] = lduw_le_p(src) & 0x3ff;
                src += 2;
            } else {
                plane[k][i] = *src++;
            }
        }
    }
}

static unsigned tc_nominal(const SGIVideoFormat *fmt)
{
    return (fmt->rate_num + fmt->rate_den - 1) / fmt->rate_den;
}

bool sgi_tc_drop_capable(const SGIVideoFormat *fmt)
{
    return fmt->rate_den == 1001 &&
           (fmt->rate_num == 30000 || fmt->rate_num == 60000);
}

int64_t sgi_tc_parse(const char *text, const SGIVideoFormat *fmt, bool *drop)
{
    unsigned hh, mm, ss, ff, fps = tc_nominal(fmt);
    char sep;

    if (sscanf(text, "%2u:%2u:%2u%c%2u", &hh, &mm, &ss, &sep, &ff) != 5 ||
        (sep != ':' && sep != ';') || mm > 59 || ss > 59 || ff >= fps ||
        hh > 23 || (sep == ';' && !sgi_tc_drop_capable(fmt))) {
        return -1;
    }
    *drop = sep == ';';
    if (*drop) {
        unsigned skip = fps / 15, minutes = 60 * hh + mm;

        /* A dropped number is not a valid label. */
        if (ss == 0 && ff < skip && mm % 10) {
            return -1;
        }
        return ((int64_t)hh * 3600 + mm * 60 + ss) * fps + ff -
               (int64_t)skip * (minutes - minutes / 10);
    }
    return ((int64_t)hh * 3600 + mm * 60 + ss) * fps + ff;
}

void sgi_tc_format(uint64_t n, const SGIVideoFormat *fmt, bool drop,
                   char out[12])
{
    unsigned fps = tc_nominal(fmt);

    if (drop) {
        uint64_t per10 = 600ULL * fps - 9 * (fps / 15);
        uint64_t per_min = 60ULL * fps - fps / 15;
        uint64_t d = n / per10, m = n % per10;

        n += 9 * (fps / 15) * d;
        if (m >= fps / 15) {
            n += (fps / 15) * ((m - fps / 15) / per_min);
        }
    }
    /* Fields are bounded to two digits so the label always fits. */
    snprintf(out, 12, "%02u:%02u:%02u%c%02u",
             (unsigned)(n / fps / 3600 % 24) % 100,
             (unsigned)(n / fps / 60 % 60) % 100,
             (unsigned)(n / fps % 60) % 100, drop ? ';' : ':',
             (unsigned)(n % fps) % 100);
}
