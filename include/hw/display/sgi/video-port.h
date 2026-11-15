/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI video frames: planar 4:2:2, 16-bit samples of 8 or 10 bits, limited
 * range, interlaced frames woven. That is Y4M C422p10, so a file is a copy.
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

/* Memory layouts of a 4:2:2 row, CbYCrY order (see the DM2 page's Payload). */
typedef enum SGIVideoPacking {
    SGI_PACK_8,                 /* Cb Y Cr Y, one byte each */
    SGI_PACK_10,                /* the same, 10 bits each, 5 bytes per pair */
    SGI_PACK_10IN16L,           /* 16-bit big-endian words, sample high */
    SGI_PACK_10IN16R,           /* 16-bit big-endian words, sample low */
    SGI_PACK_COUNT
} SGIVideoPacking;

/* The largest raster a guest-set timing record may ask for. */
#define SGI_VIDEO_MAX_WIDTH 4096
#define SGI_VIDEO_MAX_HEIGHT 2304

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

/* "YUV4MPEG2 ..." and its newline; the caller frees it. */
char *sgi_y4m_header(const SGIVideoFormat *fmt);
/* "FRAME\n" and the planes (8-bit bytes or 16-bit little endian). */
size_t sgi_y4m_frame_put(const SGIVideoFrame *f, uint8_t *dst);

#endif
