/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality/KONA stream recorder.
 *
 * With the pipe's "record" property naming a file, the pipe saves what the
 * guest sends it and what it answers, so the stream can be replayed into
 * another model of the same board and the answers compared. The record
 * layouts, and what the file leaves out, are in the "Recording the stream"
 * section of docs/system/mips/sgi/hardware/kona.rst.
 */
#ifndef HW_MISC_SGI_KONA_RECORD_H
#define HW_MISC_SGI_KONA_RECORD_H

#include "qapi/error.h"

#define SGI_KONA_REC_MAGIC       "KONAREC"
#define SGI_KONA_REC_VERSION     1

#define SGI_KONA_REC_CONFIG      'C'
#define SGI_KONA_REC_STORE       'W'
#define SGI_KONA_REC_LOAD        'R'
#define SGI_KONA_REC_DMA_READ    'D'
#define SGI_KONA_REC_DMA_WRITE   'O'
#define SGI_KONA_REC_INTERRUPT   'I'
#define SGI_KONA_REC_MARK        'M'
#define SGI_KONA_REC_END         'E'

/*
 * Where a DMA record's bytes came from or went to. Addresses are as the
 * manual page defines them for each source.
 */
#define SGI_KONA_REC_SRC_HOST    1   /* direct host memory; image-DMA writes */
#define SGI_KONA_REC_SRC_IMAGE   2   /* image-DMA reads, through the map RAM */
#define SGI_KONA_REC_SRC_LIST    3   /* display-list fetch */
#define SGI_KONA_REC_SRC_PIXELS  4   /* pixel-rectangle rows */

/* Interrupt kinds. */
#define SGI_KONA_REC_INT_GENERAL 0
#define SGI_KONA_REC_INT_GE      1

/* Marker kinds. */
#define SGI_KONA_REC_MARK_RESET  1   /* device reset; arg 0 */
#define SGI_KONA_REC_MARK_SWAP   2   /* a swap took effect; arg context */
#define SGI_KONA_REC_MARK_FRAME  3   /* a retrace completion; arg its tag */
#define SGI_KONA_REC_MARK_USER   0x100 /* set by the x-record-mark property */

typedef struct SGIKonaRecorder SGIKonaRecorder;

/* NULL, with errp set, when the file cannot be created. */
SGIKonaRecorder *sgi_kona_record_open(const char *path, uint32_t part,
                                      uint32_t revision, uint32_t rm_count,
                                      uint32_t texture_mb,
                                      const char *product, Error **errp);
/* Write the end record, flush and stop recording. Safe to repeat. */
void sgi_kona_record_finish(SGIKonaRecorder *r);
void sgi_kona_record_free(SGIKonaRecorder *r);

void sgi_kona_record_store(SGIKonaRecorder *r, unsigned size, uint32_t off,
                           uint64_t val);
void sgi_kona_record_load(SGIKonaRecorder *r, unsigned size, uint32_t off,
                          uint64_t val);
void sgi_kona_record_dma_read(SGIKonaRecorder *r, unsigned src,
                              uint64_t addr, const void *buf, uint32_t len);
void sgi_kona_record_dma_write(SGIKonaRecorder *r, unsigned src,
                               uint64_t addr, const void *buf, uint32_t len);
void sgi_kona_record_interrupt(SGIKonaRecorder *r, unsigned kind,
                               uint8_t vector, bool delivered);
void sgi_kona_record_mark(SGIKonaRecorder *r, uint32_t kind, uint32_t arg);

/* The rows of the pixel transfer about to run: its buffer offset and pitch. */
void sgi_kona_record_pixels_begin(SGIKonaRecorder *r, uint64_t offset,
                                  uint32_t pitch);
/* One row of it, read from or written to guest memory. */
void sgi_kona_record_pixels_row(SGIKonaRecorder *r, bool write,
                                const void *buf, uint32_t len);

#endif
