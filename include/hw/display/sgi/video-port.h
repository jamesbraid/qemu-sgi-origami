/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI video frames: the one frame layout a board model, a console and a
 * file share, the conversions between it and guest memory, and the Y4M
 * and WAV headers.
 *
 * A frame is 4:2:2 planar, one 16-bit host-endian sample per entry, whose
 * value is the board's bit depth (8 or 10 bits): the layout of Y4M
 * C422p10, so writing a file is a copy. Samples are limited-range YCbCr.
 * An interlaced frame is held woven, as the guest stores it.
 */
#ifndef HW_DISPLAY_SGI_VIDEO_PORT_H
#define HW_DISPLAY_SGI_VIDEO_PORT_H

typedef enum SGIVideoFields {
    SGI_VIDEO_PROGRESSIVE,
    SGI_VIDEO_TFF,              /* the first line is the earlier field's */
    SGI_VIDEO_BFF,
} SGIVideoFields;

typedef struct SGIVideoFormat {
    uint32_t width, height;     /* luma samples and lines; width is even */
    uint32_t rate_num, rate_den; /* frames per second */
    SGIVideoFields fields;
    unsigned depth;             /* 8 or 10 */
} SGIVideoFormat;

typedef struct SGIVideoFrame {
    SGIVideoFormat fmt;
    uint16_t *y, *cb, *cr;      /* chroma planes are width / 2 by height */
} SGIVideoFrame;

/* Memory layouts of one 4:2:2 row, the CbYCrY orders of the HD I/O manual. */
typedef enum SGIVideoPacking {
    SGI_PACK_8,                 /* Cb Y Cr Y, one byte each */
    SGI_PACK_10,                /* the same, 10 bits each, 5 bytes per pair */
    SGI_PACK_10IN16L,           /* 16-bit big-endian words, sample high */
    SGI_PACK_10IN16R,           /* 16-bit big-endian words, sample low */
    SGI_PACK_COUNT
} SGIVideoPacking;

bool sgi_video_format_valid(const SGIVideoFormat *fmt);
SGIVideoFrame *sgi_video_frame_new(const SGIVideoFormat *fmt);
SGIVideoFrame *sgi_video_frame_dup(const SGIVideoFrame *f);
void sgi_video_frame_free(SGIVideoFrame *f);
/* Bytes of a frame in a Y4M file, without the "FRAME" line. */
size_t sgi_video_frame_bytes(const SGIVideoFormat *fmt);

/* Row y as 0x00RRGGBB: BT.601 up to 720 wide, else BT.709, full range. */
void sgi_video_row_rgb(const SGIVideoFrame *f, unsigned y, uint32_t *dst);

/* The packing named by a property value ("8", "10", ...), or -1. */
int sgi_video_packing_parse(const char *name);
const char *sgi_video_packing_name(SGIVideoPacking p);
unsigned sgi_video_packing_depth(SGIVideoPacking p);
size_t sgi_video_packed_row_bytes(SGIVideoPacking p, unsigned width);
/* f must have the packing's depth. */
void sgi_video_unpack_row(SGIVideoPacking p, const uint8_t *src,
                          SGIVideoFrame *f, unsigned y);
/* A frame of another depth is shifted to the packing's. */
void sgi_video_pack_row(SGIVideoPacking p, const SGIVideoFrame *f,
                        unsigned y, uint8_t *dst);

/* "YUV4MPEG2 ..." and its newline; the caller frees it. */
char *sgi_y4m_header(const SGIVideoFormat *fmt);
bool sgi_y4m_parse_header(const char *line, SGIVideoFormat *fmt, Error **errp);
/* "FRAME\n" and the planes (8-bit bytes or 16-bit little endian). */
size_t sgi_y4m_frame_put(const SGIVideoFrame *f, uint8_t *dst);

#define SGI_WAV_HEADER_BYTES 44
/* A PCM header for 16-bit samples; data_bytes is patched when known. */
void sgi_wav_header(uint8_t *dst, unsigned rate, unsigned channels,
                    uint32_t data_bytes);

#endif
