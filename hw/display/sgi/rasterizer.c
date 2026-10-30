/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The software rasterizer the SGI graphics heads share.
 *
 * A draw's vertices are transformed by the draw state's matrices, its
 * primitives clipped to the view volume, mapped through its viewport, or
 * a window matrix in place of projection and viewport, into window
 * coordinates, snapped to a subpixel grid and rasterized with edge
 * functions at pixel centres, clipped by the enabled client clip planes,
 * textured from a family-owned image view while supported, and
 * fogged while fog is enabled. Each fragment then passes the
 * per-fragment operations in OpenGL 1.2.1's order (4.1):
 * pixel ownership against the drawable's rectangles, the scissor test, the
 * alpha test, the stencil test, the depth test, blending, dithering and
 * the logical operation, and is stored under the write masks through the
 * family's target. What it draws follows OpenGL 1.2.1 for
 * aliased points and lines, polygons, bitmaps and
 * RGBA8 pixel rectangles,
 * flat or smooth shaded, in RGBA or colour-index mode, from vertex colours
 * the context has already lit, with a back-facing polygon's back colours under
 * two-sided lighting, and polygons culled by facing. Where OpenGL leaves a
 * choice, the head's SGIRasterRules make it.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/notify.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "hw/display/sgi/rasterizer.h"
#include "hw/display/sgi/x-gc.h"

/*
 * The drawable's window: its origin and the bounds of the pixels it owns,
 * in plane pixels, and the rectangles themselves.
 */
typedef struct SGISoftWindow {
    int ox, oy;
    int x0, y0, x1, y1;
    const SGIClipList *clip;
} SGISoftWindow;

/* One draw's fixed inputs. */
typedef struct SGISoftDraw {
    const SGIRasterRules *rules;
    const SGIRasterTarget *t;
    const SGIGLDrawState *st;
    SGISoftWindow win;
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];
    uint32_t ntargets;
    /* Back-facing polygons take the vertices' back colours. */
    bool back_colors;
    /* Fragments blend with the colour they replace. */
    bool blend;
    /* Fragments combine with it by the logical operation instead. */
    bool logic_op;
    /* In RGBA mode, the packed colour's bits glColorMask lets through. */
    uint32_t colour_mask;
    /* The stored colour is dithered: GL_DITHER and the rules' dither. */
    bool dither;
    const SGIGLTexImage *texture;
    const SGIGLTexLevel *texture_base;
    uint32_t texture_last;
    unsigned *texture_reports;
    uint32_t point_size, line_width;
    bool point_smooth, line_smooth;
    double smooth_point_size, smooth_line_width;
    bool polygon_smooth;
    const uint32_t *polygon_stipple;
    uint32_t rgba_mask;
    bool rgba_read;
    bool *rgba_reported;
    /* Texels already read in this draw, or NULL (sgi_soft_texel_cache). */
    struct SGISoftTexelCache *texels;
    /* Filled triangles sample in fixed point, or NULL (sgi_soft_fixed). */
    const struct SGISoftFixedTex *fixed;
    /* Of bands > 1, the rows this pass draws (sgi_soft_row_in_band()). */
    unsigned band, bands;
} SGISoftDraw;

/* A vertex after transformation, ready to rasterize. */
typedef struct SGISoftVertex {
    int64_t x, y;         /* window position in subpixels */
    double z;             /* normalized depth; [0, 1] inside the view volume */
    double inv_w;         /* 1 / clip w, for perspective-correct colour */
    double depth_w;       /* the rules' depth divisor over clip w */
    double rgba[4];
    double texcoord[4];
    double index;         /* the colour index, in colour-index mode */
    double fog;           /* the fog coordinate, |z| in eye coordinates */
    /* Each client clip plane's value at the vertex, kept where >= 0. */
    double clip[SGI_GL_CLIP_PLANES];
} SGISoftVertex;

/*
 * A fragment's values as rasterization leaves them (OpenGL 1.2.1, 3.1),
 * before the per-fragment operations of chapter 4: its colour, each
 * component in [0, 1], and in colour-index mode its index.
 */
typedef struct SGISoftFrag {
    double rgba[4];
    double texcoord[4];
    double texture_rho;
    /*
     * With fixed set, the texel the fixed-point sampler filtered for the
     * fragment, which the texture environment applies in place of
     * sampling texcoord.
     */
    bool fixed;
    double texel[4];
    double index;
    double fog;           /* the fog coordinate (3.10) */
} SGISoftFrag;

/*
 * Band-parallel drawing. A draw large enough, into a target whose
 * callbacks may run concurrently for different pixels
 * (SGIRasterTarget.concurrent_pixels), is drawn by several threads at
 * once, each pass taking every primitive in order but only its own band
 * of rows: every pixel sees the same fragments in the same order as from
 * one pass, so the planes come out the same, and the call still returns
 * only when all are drawn. The caller draws band 0 itself; the other
 * bands go to worker threads started on first use. Per-primitive work is
 * repeated in each band, so small draws stay on one thread.
 */
#define SGI_SOFT_BAND_ROWS_SHIFT 3      /* rows interleave in runs of 8 */

static unsigned sgi_soft_bands_wanted;
static uint64_t sgi_soft_bands_min_pixels = SGI_RASTER_PARALLEL_MIN_PIXELS;
/*
 * The host's processors, read once (sgi_soft_pool_init()): on Linux the
 * count comes from sysfs, which costs system calls, and every draw and
 * clear chooses its bands. A later change to the host's processors is not
 * seen.
 */
static unsigned sgi_soft_host_processors = 1;

/* A band other than the first repeats the primitive work: say it once. */
static __thread bool sgi_soft_band_quiet;

typedef void SGISoftBandFn(void *opaque, unsigned band, unsigned bands);

static struct {
    QemuMutex run;              /* one band-parallel call at a time */
    QemuMutex lock;
    QemuCond start, done;
    unsigned threads;
    uint64_t generation;
    SGISoftBandFn *fn;
    void *opaque;
    unsigned bands, pending;
} sgi_soft_pool;

/*
 * Count the host's processors and ready the pool, once, on the first draw
 * or clear that chooses its bands. Workers start only when a draw first
 * runs in bands, so a process that forks before then, as -daemonize does,
 * gives its child a usable pool; a process that has drawn in bands must
 * not fork and then draw.
 */
static void sgi_soft_pool_init(void)
{
    static gsize ready;

    if (g_once_init_enter(&ready)) {
        sgi_soft_host_processors = MAX(g_get_num_processors(), 1);
        qemu_mutex_init(&sgi_soft_pool.run);
        qemu_mutex_init(&sgi_soft_pool.lock);
        qemu_cond_init(&sgi_soft_pool.start);
        qemu_cond_init(&sgi_soft_pool.done);
        g_once_init_leave(&ready, 1);
    }
}

static void *sgi_soft_band_worker(void *opaque)
{
    unsigned band = (uintptr_t)opaque;
    uint64_t seen = 0;

    /*
     * A band can log (qemu_log_mask()), which reads the log file under
     * rcu_read_lock(); synchronize_rcu() waits only for registered
     * threads, so an unregistered worker could see the file freed.
     */
    rcu_register_thread();
    qemu_mutex_lock(&sgi_soft_pool.lock);
    for (;;) {
        SGISoftBandFn *fn;
        void *job;
        unsigned bands;

        while (sgi_soft_pool.generation == seen) {
            qemu_cond_wait(&sgi_soft_pool.start, &sgi_soft_pool.lock);
        }
        seen = sgi_soft_pool.generation;
        if (band >= sgi_soft_pool.bands) {
            continue;
        }
        fn = sgi_soft_pool.fn;
        job = sgi_soft_pool.opaque;
        bands = sgi_soft_pool.bands;
        qemu_mutex_unlock(&sgi_soft_pool.lock);
        fn(job, band, bands);
        qemu_mutex_lock(&sgi_soft_pool.lock);
        if (--sgi_soft_pool.pending == 0) {
            qemu_cond_signal(&sgi_soft_pool.done);
        }
    }
    rcu_unregister_thread();
    return NULL;
}

static unsigned sgi_soft_bands(void)
{
    unsigned bands;

    sgi_soft_pool_init();
    bands = qatomic_read(&sgi_soft_bands_wanted);

    if (!bands) {
        bands = MIN(SGI_RASTER_PARALLEL_DEFAULT_BANDS,
                    sgi_soft_host_processors);
    }
    return MIN(bands, SGI_RASTER_PARALLEL_MAX_BANDS);
}

void sgi_raster_set_parallel(unsigned bands, uint64_t min_pixels)
{
    qatomic_set(&sgi_soft_bands_wanted, bands);
    qatomic_set(&sgi_soft_bands_min_pixels, min_pixels);
}

/*
 * fn(opaque, band, bands) for each band, band 0 on this thread, the rest
 * on workers, returning when all have; false, having run none, when
 * another thread is using the workers.
 */
static bool sgi_soft_bands_run(SGISoftBandFn *fn, void *opaque,
                               unsigned bands)
{
    sgi_soft_pool_init();
    if (qemu_mutex_trylock(&sgi_soft_pool.run)) {
        return false;
    }
    qemu_mutex_lock(&sgi_soft_pool.lock);
    while (sgi_soft_pool.threads < bands - 1) {
        QemuThread thread;
        char name[32];

        snprintf(name, sizeof(name), "sgi-raster/%u",
                 sgi_soft_pool.threads + 1);
        qemu_thread_create(&thread, name, sgi_soft_band_worker,
                           (void *)(uintptr_t)(sgi_soft_pool.threads + 1),
                           QEMU_THREAD_DETACHED);
        sgi_soft_pool.threads++;
    }
    sgi_soft_pool.fn = fn;
    sgi_soft_pool.opaque = opaque;
    sgi_soft_pool.bands = bands;
    sgi_soft_pool.pending = bands - 1;
    sgi_soft_pool.generation++;
    qemu_cond_broadcast(&sgi_soft_pool.start);
    qemu_mutex_unlock(&sgi_soft_pool.lock);

    fn(opaque, 0, bands);

    qemu_mutex_lock(&sgi_soft_pool.lock);
    while (sgi_soft_pool.pending) {
        qemu_cond_wait(&sgi_soft_pool.done, &sgi_soft_pool.lock);
    }
    qemu_mutex_unlock(&sgi_soft_pool.lock);
    qemu_mutex_unlock(&sgi_soft_pool.run);
    return true;
}

/* The pixel count from which a draw or clear into @t runs in bands. */
static uint64_t sgi_soft_band_min_pixels(const SGIRasterTarget *t)
{
    return t->band_min_pixels ? t->band_min_pixels :
           qatomic_read(&sgi_soft_bands_min_pixels);
}

static inline bool sgi_soft_row_in_band(const SGISoftDraw *r, int py)
{
    return r->bands <= 1 ||
           ((uint32_t)py >> SGI_SOFT_BAND_ROWS_SHIFT) % r->bands == r->band;
}

static bool sgi_soft_window(const SGIGLDrawState *st, SGISoftWindow *win)
{
    const SGIGLDrawable *d = &st->drawable;

    if (!d->bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: draw with no window bound "
                      "dropped\n");
        return false;
    }
    if (!d->clip.n) {
        /* Other windows cover all of this one: nothing to draw. */
        return false;
    }
    win->ox = d->origin[0];
    win->oy = d->origin[1];
    win->x0 = d->clip.r[0].x0;
    win->y0 = d->clip.r[0].y0;
    win->x1 = d->clip.r[0].x1;
    win->y1 = d->clip.r[0].y1;
    for (uint32_t i = 1; i < MIN(d->clip.n, SGI_CLIP_MAX); i++) {
        win->x0 = MIN(win->x0, d->clip.r[i].x0);
        win->y0 = MIN(win->y0, d->clip.r[i].y0);
        win->x1 = MAX(win->x1, d->clip.r[i].x1);
        win->y1 = MAX(win->y1, d->clip.r[i].y1);
    }
    win->clip = &d->clip;
    return true;
}

bool sgi_raster_rules_supported(const SGIRasterRules *r)
{
    return r->subpixel_bits >= 1 && r->subpixel_bits <= 12 &&
           r->color_bits >= 1 && r->color_bits <= 10 &&
           r->depth_bits >= 1 && r->depth_bits <= 32 &&
           (!r->depth_float ||
            (r->depth_bits >= 10 && r->depth_bits <= 30)) &&
           (!r->depth_eye_float ||
            (!r->depth_float && r->depth_row_given &&
             r->depth_bits >= 12 && r->depth_bits <= 30 &&
             r->depth_scale > 0 && isfinite(r->depth_scale))) &&
           (!r->depth_row_given ||
            (isfinite(r->depth_row[0]) && isfinite(r->depth_row[1]) &&
             isfinite(r->depth_row[2]) && isfinite(r->depth_row[3]))) &&
           (!r->depth_divisor_given ||
            (r->depth_row_given &&
             isfinite(r->depth_divisor[0]) && isfinite(r->depth_divisor[1]) &&
             isfinite(r->depth_divisor[2]) &&
             isfinite(r->depth_divisor[3]))) &&
           r->depth_slack >= 0 && r->depth_slack < 1 &&
           r->stencil_bits <= 16 && r->alpha_bits <= 10 &&
           r->alpha_test_bits <= 16 &&
           r->rounding <= SGI_ROUND_HALF_AWAY &&
           r->tie_rule == SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN &&
           r->arith == SGI_ARITH_DOUBLE &&
           (r->line_rule == SGI_LINE_COVERAGE_DOUBLE ||
            r->line_rule == SGI_LINE_CENTRE_SAMPLED) &&
           r->dither <= SGI_DITHER_BAYER4_TOP_ROWS &&
           r->smooth_rule <= SGI_SMOOTH_TWO_PIXEL_FILTER &&
           (r->dither == SGI_DITHER_NONE ||
            r->dither_anchor == SGI_DITHER_SCREEN);
}

static bool sgi_soft_check_rules(const SGIRasterRules *r)
{
    if (!sgi_raster_rules_supported(r)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: raster rules are not "
                      "implemented; nothing drawn\n");
        return false;
    }
    return true;
}

/* Whether the target can take the colour-index work a draw state asks. */
static bool sgi_soft_check_index(const SGIRasterTarget *t,
                                 uint32_t index_bits)
{
    if (index_bits && !t->store_index) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: colour-index drawing is not "
                      "implemented for these planes; nothing drawn\n");
        return false;
    }
    return true;
}

/* x rounded to an integer by the rules' rounding. */
static inline int64_t sgi_soft_round(const SGIRasterRules *r, double x)
{
    double f, frac;

    if (r->rounding != SGI_ROUND_FLOOR && r->rounding != SGI_ROUND_HALF_UP &&
        r->rounding != SGI_ROUND_HALF_EVEN) {
        /*
         * llround(), without the call: x less its truncation is exact, so
         * a fraction of a half or more rounds away from zero.
         */
        int64_t t = (int64_t)x;

        frac = x - t;
        return t + (frac >= 0.5) - (frac <= -0.5);
    }
    f = floor(x);
    frac = x - f;
    switch (r->rounding) {
    case SGI_ROUND_FLOOR:
        return (int64_t)f;
    case SGI_ROUND_HALF_UP:
        return (int64_t)f + (frac >= 0.5);
    default:
        if (frac == 0.5) {
            return (int64_t)f + ((int64_t)f & 1);
        }
        return (int64_t)f + (frac > 0.5);
    }
}

/* The largest stored depth. */
static uint32_t sgi_soft_depth_max(const SGIRasterRules *r)
{
    return UINT32_MAX >> (32 - r->depth_bits);
}

/*
 * A non-negative single's bits order as unsigned integers do, so the
 * shifted bits of a depth in [0, 1], at most 0x3f800000 >> (30 -
 * depth_bits), keep its order in depth_bits bits.
 */
uint32_t sgi_raster_eye_float_encode(double value, unsigned mantissa_bits)
{
    uint32_t mantissa;
    int exponent, e;

    if (!(value > 0)) {
        return 0;
    }
    if (value >= ldexp(1, 33)) {
        return (1u << (6 + mantissa_bits)) - 1;
    }
    /*
     * value = m 2^exponent with m in [0.5, 1): the biased exponent is
     * exponent - 1 + 31.
     */
    frexp(value, &exponent);
    e = exponent - 1 + 31;
    if (e < 0) {
        return 0;
    }
    mantissa = (uint32_t)floor((ldexp(value, -exponent + 1) - 1.0) *
                               (1u << mantissa_bits));
    return (uint32_t)e << mantissa_bits | mantissa;
}

double sgi_raster_eye_float_decode(uint32_t word, unsigned mantissa_bits)
{
    uint32_t mask = (1u << mantissa_bits) - 1;

    return ldexp(1.0 + (double)(word & mask) / (1u << mantissa_bits),
                 (int)(word >> mantissa_bits & 63) - 31);
}

uint32_t sgi_raster_depth_code(const SGIRasterRules *rules, double z)
{
    float f;
    uint32_t bits;

    if (rules->depth_eye_float) {
        /* Odyssey keeps depth_scale * z; a z below 0 is not a depth. */
        return sgi_raster_eye_float_encode(MAX(z, 0.0) * rules->depth_scale,
                                           rules->depth_bits - 6);
    }
    if (!rules->depth_float) {
        return (uint32_t)sgi_soft_round(rules, z * sgi_soft_depth_max(rules));
    }
    f = MIN(MAX(z, 0.0), 1.0);
    memcpy(&bits, &f, sizeof(bits));
    return bits >> (30 - rules->depth_bits);
}

/*
 * Window depth z once an eye-float polygon offset of steps words has been
 * added to its stored word: the word of z plus steps, clamped to the
 * words 0 to the one for z = 1, as the depth of the middle of that word's
 * cell, which stores as exactly that word again whatever the rounding of
 * the divide by depth_scale. The clamp at the far word is a MODEL CHOICE:
 * the datasheet fixes neither the sum's saturation nor whether the board
 * keeps a word past the far plane's.
 */
static double sgi_soft_offset_word_depth(const SGIRasterRules *rules,
                                         double z, double steps)
{
    unsigned mantissa_bits = rules->depth_bits - 6;
    int64_t far_word = sgi_raster_depth_code(rules, 1.0);
    int64_t word = sgi_raster_depth_code(rules, z) +
                   (int64_t)MAX(MIN(steps, 1 << 25), -(1 << 25));

    word = MAX(MIN(word, far_word), 0);
    if (!word) {
        return 0;
    }
    /* The far word's cell reaches past the far plane; z = 1 stores it. */
    return MIN(sgi_raster_eye_float_decode(word, mantissa_bits) *
               (1.0 + ldexp(1.0, -(int)mantissa_bits - 1)) /
               rules->depth_scale, 1.0);
}

/*
 * A head's window depth at a fragment: the depth row and its divisor each
 * over w interpolate linearly in the window, so their ratio, with the
 * polygon offset, is the row's depth there. A depth a little outside
 * [0, 1], by the rules' slack, is clamped to it. Ordinarily the offset is
 * in window depth and divides with the row; under the eye-linear float it
 * counts steps of the stored word.
 */
static double sgi_soft_row_depth(const SGIRasterRules *rules, double z,
                                 double offset, double depth_w)
{
    double slack = rules->depth_slack;

    z = (z + (rules->depth_eye_float ? 0 : offset)) / depth_w;
    if (z < 0 && z >= -slack) {
        z = 0;
    } else if (z > 1 && z <= 1 + slack) {
        z = 1;
    }
    if (rules->depth_eye_float && offset != 0 && z >= 0 && z <= 1) {
        z = sgi_soft_offset_word_depth(rules, z, offset);
    }
    return z;
}

/*
 * A component clamped to [0, 1] and scaled to the rules' colour bits, a
 * fraction of a step rounded half up, whatever the rules' rounding: the
 * sum is truncated (OpenGL 1.2.1, 2.13.9).
 */
static inline uint32_t sgi_soft_channel(const SGIRasterRules *r, double c)
{
    uint32_t max = (1u << r->color_bits) - 1;

    if (!(c > 0.0)) {
        return 0;
    }
    if (c >= 1.0) {
        return max;
    }
    return (uint32_t)(c * max + 0.5);
}

/* Incoming alpha precision is independent of absent or narrower stored A. */
static uint32_t sgi_soft_alpha_max(const SGIRasterRules *r)
{
    return (1u << MAX(2, MAX(r->color_bits, r->alpha_bits))) - 1;
}

static uint32_t sgi_soft_alpha(const SGIRasterRules *r, double value)
{
    uint32_t max = sgi_soft_alpha_max(r);

    return !(value > 0) ? 0 : value >= 1 ? max : value * max + 0.5;
}

static uint32_t sgi_soft_alpha_test(const SGIRasterRules *r, double value)
{
    uint32_t max = r->alpha_test_bits ?
                   (1u << r->alpha_test_bits) - 1 : sgi_soft_alpha_max(r);

    return !(value > 0) ? 0 : value >= 1 ? max : value * max + 0.5;
}

/* Internal nearest conversion precedes storage truncation, even on clears. */
static uint32_t sgi_soft_stored_alpha(const SGIRasterRules *r, double value)
{
    return sgi_soft_alpha(r, value) * ((1u << r->alpha_bits) - 1) /
           sgi_soft_alpha_max(r);
}

static uint32_t sgi_soft_rgba_mask(const SGIRasterRules *r, uint32_t mask)
{
    return mask & (r->alpha_bits ? SGI_GL_COLOUR_MASK_ALL :
                                  SGI_GL_COLOUR_MASK_ALPHA - 1);
}

/* Reject unsupported capabilities before the operation can write any plane. */
static bool sgi_soft_rgba_capable(const SGIRasterRules *r,
                                 const SGIRasterTarget *t, bool read)
{
    if ((r->alpha_bits && !t->store_rgba) ||
        (t->store_rgba && read && !t->load_rgba) ||
        (!t->store_rgba && t->load_rgba)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: inconsistent RGBA callbacks; "
                      "operation dropped\n");
        return false;
    }
    return true;
}

/* Read every selected destination before any colour/depth/stencil write. */
static bool sgi_soft_rgba_load(const SGIRasterRules *r,
                              const SGIRasterTarget *t,
                              const SGIColourTarget *targets, unsigned count,
                              int x, int y,
                              SGIRasterColour dest[SGI_GL_MAX_TARGETS],
                              bool *reported)
{
    for (unsigned i = 0; i < count; i++) {
        bool valid = t->load_rgba(t->opaque, targets[i], x, y, &dest[i]);

        for (unsigned k = 0; valid && k < 4; k++) {
            unsigned bits = k == 3 ? r->alpha_bits : r->color_bits;

            /* An absent A is ignored, including its returned representation. */
            valid = !bits || dest[i].component[k] < (1u << bits);
        }
        if (!valid) {
            /* Bands of one draw share the flag (sgi_soft_bands_run()). */
            if (!qatomic_xchg(reported, true)) {
                qemu_log_mask(LOG_UNIMP, "sgi-gl: RGBA destination read "
                              "failed; pixel writes dropped\n");
            }
            return false;
        }
        if (!r->alpha_bits) {
            dest[i].component[3] = 0;
        }
    }
    return true;
}

/* A colour, each component clamped and rounded to the rules' bits. */
static inline uint32_t sgi_soft_rgb(const SGIRasterRules *r,
                                    const double c[3])
{
    return (sgi_soft_channel(r, c[0]) << (2 * r->color_bits)) |
           (sgi_soft_channel(r, c[1]) << r->color_bits) |
           sgi_soft_channel(r, c[2]);
}

/* SGI_DITHER_BAYER4_TOP_ROWS's thresholds, in sixteenths of a step. */
static const uint8_t sgi_soft_bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/*
 * A fragment's colour at plane pixel (x, y) as the planes store it:
 * dithered while the draw dithers, otherwise rounded as sgi_soft_rgb()
 * rounds. A clamped component of 0 or 1 is exact either way, and one
 * below 1 stays below max + 1 with the largest threshold added.
 */
static uint32_t sgi_soft_store_rgb(const SGISoftDraw *r, int x, int y,
                                   const double c[3])
{
    const int bits = r->rules->color_bits;
    const uint32_t max = (1u << bits) - 1;
    int64_t row;
    double b;
    uint32_t out = 0;

    if (!r->dither) {
        return sgi_soft_rgb(r->rules, c);
    }
    row = r->t->screen_rows_down ? (int64_t)y - r->t->screen_top :
                                   (int64_t)r->t->screen_top - y;
    b = sgi_soft_bayer4[row & 3][x & 3] / 16.0;
    for (int k = 0; k < 3; k++) {
        uint32_t v;

        if (!(c[k] > 0.0)) {
            v = 0;
        } else if (c[k] >= 1.0) {
            v = max;
        } else {
            v = floor(c[k] * max + b);
        }
        out |= v << ((2 - k) * bits);
    }
    return out;
}

static int64_t sgi_soft_edge(const SGISoftVertex *a, const SGISoftVertex *b,
                             int64_t px, int64_t py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN: a counter-clockwise edge owns the pixel
 * centres on it when it is a left edge (running down) or a top edge
 * (horizontal, running left), with rows counted up from the bottom of the
 * plane, so two triangles sharing an edge never both draw its centres, as
 * OpenGL 1.2.1, 3.5.1 requires.
 */
static bool sgi_soft_owns(const SGISoftVertex *a, const SGISoftVertex *b,
                          int64_t e)
{
    if (e != 0) {
        return e > 0;
    }
    return b->y < a->y || (b->y == a->y && b->x < a->x);
}

/*
 * Narrow [*lo, *hi], pixel steps along a row from a start whose edge value
 * is e, to the steps where sgi_soft_owns() holds for the edge from a to b,
 * whose value moves by step a pixel. The edge function is linear along the
 * row, so the owned steps are one run: e + k * step >= need, where need is
 * 0 for an edge that owns the centres on it and 1 for one that does not.
 */
static void sgi_soft_owned_run(const SGISoftVertex *a, const SGISoftVertex *b,
                               int64_t e, int64_t step, int64_t *lo,
                               int64_t *hi)
{
    int64_t need = sgi_soft_owns(a, b, 0) ? 0 : 1;
    int64_t d = need - e;

    if (step > 0) {
        /* The first k with k * step >= d, rounding the quotient up. */
        *lo = MAX(*lo, d > 0 ? (d + step - 1) / step : -(-d / step));
    } else if (step < 0) {
        /* The last k with k * -step <= -d, rounding the quotient down. */
        *hi = MIN(*hi, -d >= 0 ? -d / -step : -((d - step - 1) / -step));
    } else if (d > 0) {
        *hi = *lo - 1;
    }
}

/*
 * Whether value passes a test comparing it with ref by func, the
 * comparison minus GL_NEVER: the depth test's incoming depth against the
 * stored one (4.1.5), the alpha test's alpha against its reference
 * (4.1.4), the stencil test's masked reference against the masked stored
 * value (4.1.5).
 */
static bool sgi_soft_compare(uint32_t func, uint32_t value, uint32_t ref)
{
    switch (func) {
    case SGI_GL_NEVER:
        return false;
    case SGI_GL_LESS:
        return value < ref;
    case SGI_GL_EQUAL:
        return value == ref;
    case SGI_GL_LEQUAL:
        return value <= ref;
    case SGI_GL_GREATER:
        return value > ref;
    case SGI_GL_NOTEQUAL:
        return value != ref;
    case SGI_GL_GEQUAL:
        return value >= ref;
    default:
        return true;
    }
}

bool sgi_raster_alpha_test(const SGIRasterRules *r, uint32_t func,
                           double alpha, double ref)
{
    return sgi_soft_compare(func, sgi_soft_alpha_test(r, alpha),
                            sgi_soft_alpha_test(r, ref));
}

/* Whether the planes have a stencil buffer, and its largest value. */
static bool sgi_soft_stencil_max(const SGIRasterRules *rules,
                                 const SGIRasterTarget *t, uint32_t *max)
{
    *max = (1u << rules->stencil_bits) - 1;
    return rules->stencil_bits && t->load_stencil && t->store_stencil;
}

/*
 * One of glStencilOp's operations on the stencil value at (x, y), the
 * result written under the stencil write mask (4.1.5, 4.2.2). GL_INCR
 * and GL_DECR clamp to the buffer's range.
 */
static void sgi_soft_stencil_op(const SGIGLDrawState *st,
                                const SGIRasterTarget *t, int x, int y,
                                uint32_t op, uint32_t max, uint32_t ref)
{
    uint32_t old = t->load_stencil(t->opaque, x, y) & max, value;
    uint32_t mask = st->stencil_write_mask & max;

    switch (op) {
    case SGI_GL_KEEP:
        return;
    case SGI_GL_REPLACE:
        value = ref;
        break;
    case SGI_GL_INCR:
        value = old < max ? old + 1 : max;
        break;
    case SGI_GL_DECR:
        value = old ? old - 1 : 0;
        break;
    case SGI_GL_INVERT:
        value = ~old & max;
        break;
    default:
        value = 0;
        break;
    }
    t->store_stencil(t->opaque, x, y, (old & ~mask) | (value & mask));
}

bool sgi_raster_depth_stencil(const SGIRasterRules *rules,
                               const SGIGLDrawState *st,
                               const SGIRasterTarget *t, int x, int y,
                               double z)
{
    uint32_t zq, smax = 0, sref = 0;
    bool stencil;

    if (!(z >= 0 && z <= 1) ||
        (st->depth_test && (!t->load_depth ||
                           (st->depth_mask && !t->store_depth)))) {
        return false;
    }
    /*
     * The reference clamps to the buffer's range; with no stencil buffer
     * the test passes and nothing is written.
     */
    stencil = st->stencil_test && sgi_soft_stencil_max(rules, t, &smax);
    if (stencil) {
        uint32_t vmask = st->stencil_value_mask;

        sref = st->stencil_ref < 0 ? 0 : MIN((uint32_t)st->stencil_ref,
                                                smax);
        if (!sgi_soft_compare(st->stencil_func, sref & vmask,
                              t->load_stencil(t->opaque, x, y) & smax &
                              vmask)) {
            sgi_soft_stencil_op(st, t, x, y, st->stencil_op[0], smax, sref);
            return false;
        }
    }
    zq = sgi_raster_depth_code(rules, z);
    if (st->depth_test) {
        if (!sgi_soft_compare(st->depth_func, zq,
                              t->load_depth(t->opaque, x, y))) {
            if (stencil) {
                sgi_soft_stencil_op(st, t, x, y, st->stencil_op[1], smax,
                                    sref);
            }
            return false;
        }
        if (st->depth_mask) {
            t->store_depth(t->opaque, x, y, zq);
        }
    }
    /* A disabled depth test passes (4.1.5). */
    if (stencil) {
        sgi_soft_stencil_op(st, t, x, y, st->stencil_op[2], smax, sref);
    }
    return true;
}

/*
 * One blend factor (OpenGL 1.2.1, tables 4.1 and 4.2) for component k,
 * red to alpha, from the source s, destination d and constant colour c.
 */
static double sgi_soft_blend_factor(uint32_t factor, int k, const double s[4],
                                    const double d[4], const float c[4])
{
    switch (factor) {
    case 1:
        return 1;
    case SGI_GL_SRC_COLOR:
        return s[k];
    case SGI_GL_ONE_MINUS_SRC_COLOR:
        return 1 - s[k];
    case SGI_GL_DST_COLOR:
        return d[k];
    case SGI_GL_ONE_MINUS_DST_COLOR:
        return 1 - d[k];
    case SGI_GL_SRC_ALPHA:
        return s[3];
    case SGI_GL_ONE_MINUS_SRC_ALPHA:
        return 1 - s[3];
    case SGI_GL_DST_ALPHA:
        return d[3];
    case SGI_GL_ONE_MINUS_DST_ALPHA:
        return 1 - d[3];
    case SGI_GL_SRC_ALPHA_SATURATE:
        return k == 3 ? 1 : MIN(s[3], 1 - d[3]);
    case SGI_GL_CONSTANT_COLOR:
        return c[k];
    case SGI_GL_ONE_MINUS_CONSTANT_COLOR:
        return 1 - c[k];
    case SGI_GL_CONSTANT_ALPHA:
        return c[3];
    case SGI_GL_ONE_MINUS_CONSTANT_ALPHA:
        return 1 - c[3];
    default:
        return 0;
    }
}

static double sgi_soft_blend_component(const SGIGLDrawState *st, unsigned k,
                                       const double src[4], const double dst[4])
{
    double sf = sgi_soft_blend_factor(st->blend_src, k, src, dst,
                                      st->blend_color);
    double df = sgi_soft_blend_factor(st->blend_dst, k, src, dst,
                                      st->blend_color);

    switch (st->blend_equation) {
    case SGI_GL_MIN:
        return MIN(src[k], dst[k]);
    case SGI_GL_MAX:
        return MAX(src[k], dst[k]);
    case SGI_GL_FUNC_SUBTRACT:
        return src[k] * sf - dst[k] * df;
    case SGI_GL_FUNC_REVERSE_SUBTRACT:
        return dst[k] * df - src[k] * sf;
    default:
        return src[k] * sf + dst[k] * df;
    }
}

/*
 * Blend the packed source colour src, with alpha alpha, and the packed
 * destination dst (4.1.6), each component the fixed point of the rules'
 * colour bits taken as a fraction of its largest value. The planes hold
 * no alpha, so the destination's alpha is 1. The result is clamped to
 * [0, 1] by the subsequent colour conversion.
 */
static void sgi_soft_blend_rgb(const SGIRasterRules *rules,
                                const SGIGLDrawState *st, uint32_t src,
                                double alpha, uint32_t dst, double out[3])
{
    const int bits = rules->color_bits;
    const double max = (1u << bits) - 1;
    double s[4], d[4];

    for (int k = 0; k < 3; k++) {
        int shift = (2 - k) * bits;

        s[k] = ((src >> shift) & (uint32_t)max) / max;
        d[k] = ((dst >> shift) & (uint32_t)max) / max;
    }
    s[3] = alpha;
    d[3] = 1;
    for (int k = 0; k < 3; k++) {
        out[k] = sgi_soft_blend_component(st, k, s, d);
    }
}

uint32_t sgi_raster_blend_rgb(const SGIRasterRules *rules,
                              const SGIGLDrawState *st, uint32_t src,
                              double alpha, uint32_t dst)
{
    double out[3];

    alpha = sgi_soft_alpha(rules, alpha) / (double)sgi_soft_alpha_max(rules);
    sgi_soft_blend_rgb(rules, st, src, alpha, dst, out);
    return sgi_soft_rgb(rules, out);
}

static uint32_t sgi_soft_blend(const SGISoftDraw *r, int x, int y,
                               uint32_t src, double alpha, uint32_t dst)
{
    double out[3];

    sgi_soft_blend_rgb(r->rules, r->st, src, alpha, dst, out);
    return sgi_soft_store_rgb(r, x, y, out);
}

/*
 * glLogicOp's operation (4.1.8, table 4.3) on the source s and the
 * destination d, bit by bit.
 */
static uint32_t sgi_soft_logic_op(uint32_t op, uint32_t s, uint32_t d)
{
    return sgi_x_logic_op(op - SGI_GL_CLEAR, s, d);
}

/* RGB retains its existing arithmetic; spatial alpha dithering is omitted. */
static SGIRasterColour sgi_soft_rgba_value(const SGIRasterRules *rules,
                                          uint32_t rgb, double alpha)
{
    SGIRasterColour c;
    uint32_t max = (1u << rules->color_bits) - 1;

    for (unsigned k = 0; k < 3; k++) {
        c.component[k] = (rgb >> ((2 - k) * rules->color_bits)) & max;
    }
    c.component[3] = sgi_soft_stored_alpha(rules, alpha);
    return c;
}

static SGIRasterColour sgi_soft_rgba_result(const SGISoftDraw *r, int x, int y,
                                           const SGISoftFrag *frag,
                                           const SGIRasterColour *dest)
{
    SGIRasterColour out;
    uint32_t max = (1u << r->rules->color_bits) - 1;
    uint32_t alpha_max = sgi_soft_alpha_max(r->rules);
    uint32_t alpha_q = sgi_soft_alpha(r->rules, frag->rgba[3]);
    double alpha = alpha_q / (double)alpha_max;
    uint32_t rgb = r->blend ? sgi_soft_rgb(r->rules, frag->rgba) :
                              sgi_soft_store_rgb(r, x, y, frag->rgba);

    if (!r->blend && !r->logic_op &&
        r->rgba_mask == sgi_soft_rgba_mask(r->rules, SGI_GL_COLOUR_MASK_ALL)) {
        /*
         * sgi_soft_rgba_value()'s stored alpha, sgi_soft_alpha() of
         * alpha_q / alpha_max scaled to the stored bits, without the round
         * trip: alpha_q / alpha_max * alpha_max + 0.5 truncates to alpha_q
         * for every alpha_q up to 1023.
         */
        uint32_t stored = (1u << r->rules->alpha_bits) - 1;

        for (unsigned k = 0; k < 3; k++) {
            out.component[k] = (rgb >> ((2 - k) * r->rules->color_bits)) &
                               max;
        }
        out.component[3] = stored == alpha_max ? alpha_q :
                           alpha_q * stored / alpha_max;
        return out;
    }
    out = sgi_soft_rgba_value(r->rules, rgb, alpha);
    if (r->blend) {
        double src[4], dst[4], blended[4];

        for (unsigned k = 0; k < 3; k++) {
            src[k] = out.component[k] / (double)max;
            dst[k] = dest->component[k] / (double)max;
        }
        src[3] = alpha;
        dst[3] = r->rules->alpha_bits ? dest->component[3] /
                  (double)((1u << r->rules->alpha_bits) - 1) : 1;
        for (unsigned k = 0; k < 4; k++) {
            blended[k] = sgi_soft_blend_component(r->st, k, src, dst);
        }
        out = sgi_soft_rgba_value(r->rules,
                                  sgi_soft_store_rgb(r, x, y, blended),
                                  blended[3]);
    }
    for (unsigned k = 0; k < 4; k++) {
        uint32_t bits = k == 3 ? r->rules->alpha_bits : r->rules->color_bits;

        if (r->logic_op) {
            out.component[k] = sgi_soft_logic_op(r->st->logic_op_mode,
                                  out.component[k], dest->component[k]) &
                               ((1u << bits) - 1);
        }
        if (!(r->rgba_mask & (1u << k))) {
            out.component[k] = dest->component[k];
        }
    }
    return out;
}

/*
 * The bits of a packed colour of the rules' colour bits that a
 * glColorMask channel mask lets through. The planes hold no alpha, so
 * its bit has no effect.
 */
uint32_t sgi_raster_colour_mask(const SGIRasterRules *r, uint32_t mask)
{
    uint32_t channel = (1u << r->color_bits) - 1, out = 0;

    for (int k = 0; k < 3; k++) {
        if (mask & (SGI_GL_COLOUR_MASK_RED << k)) {
            out |= channel << ((2 - k) * r->color_bits);
        }
    }
    return out;
}

/* The packed colour's bits, all of which a full mask writes. */
static uint32_t sgi_soft_colour_bits(const SGIRasterRules *r)
{
    return sgi_raster_colour_mask(r, SGI_GL_COLOUR_MASK_ALL);
}

/*
 * Which of the operations that read the stored colour a draw state
 * enables. The logical operation applies to a colour index while
 * GL_INDEX_LOGIC_OP is enabled, and to an RGBA colour while
 * GL_COLOR_LOGIC_OP is, or while blending is enabled with
 * EXT_blend_logic_op's GL_LOGIC_OP equation; it replaces blending
 * (4.1.8). A colour mask with some channels clear keeps the stored
 * bits of those (4.2.2). Planes that cannot be read draw as if the
 * operations were disabled and the mask were all ones, logged once for
 * the draw. An RGBA draw is dithered while GL_DITHER is enabled under
 * rules that name a dither; a colour index never is.
 */
static bool sgi_soft_setup_ops(SGISoftDraw *r)
{
    const SGIGLDrawState *st = r->st;

    if (st->index_bits) {
        r->blend = false;
        r->logic_op = st->index_logic_op;
    } else {
        r->logic_op = st->color_logic_op ||
                      (st->blend && st->blend_equation == SGI_GL_LOGIC_OP);
        r->blend = st->blend && !r->logic_op;
    }
    r->colour_mask = sgi_raster_colour_mask(r->rules, st->colour_mask);
    r->dither = st->dither && r->rules->dither != SGI_DITHER_NONE;
    if (!st->index_bits) {
        r->rgba_mask = sgi_soft_rgba_mask(r->rules, st->colour_mask);
        r->rgba_read = r->ntargets && r->rgba_mask &&
            (r->blend || r->logic_op ||
            r->rgba_mask != sgi_soft_rgba_mask(r->rules,
                                              SGI_GL_COLOUR_MASK_ALL));
        if (!sgi_soft_rgba_capable(r->rules, r->t, r->rgba_read)) {
            return false;
        }
        if (r->t->store_rgba) {
            return true;
        }
    }
    if ((r->blend || r->logic_op ||
         (!st->index_bits && r->colour_mask &&
          r->colour_mask != sgi_soft_colour_bits(r->rules))) &&
        !r->t->load_colour) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: blending, logical operations and "
                      "partial colour masks need planes that can be read "
                      "back; drawn without them\n");
        r->blend = r->logic_op = false;
        r->colour_mask = sgi_soft_colour_bits(r->rules);
    }
    return true;
}

/* Runtime errors are observable without logging once for every pixel. */
static void sgi_soft_texture_report(const SGISoftDraw *r, unsigned reason,
                                    const char *message)
{
    /* Bands of one draw share the reports (sgi_soft_bands_run()). */
    if (!(qatomic_fetch_or(r->texture_reports, reason) & reason)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: texture %s\n", message);
    }
}

static bool sgi_soft_texture_format(uint32_t format)
{
    return format == SGI_GL_ALPHA || format == SGI_GL_LUMINANCE ||
           format == SGI_GL_LUMINANCE_ALPHA || format == SGI_GL_INTENSITY ||
           format == SGI_GL_RGB || format == SGI_GL_RGBA ||
           format == SGI_GL_DEPTH_COMPONENT;
}

static bool sgi_soft_texture_wrap(uint32_t wrap)
{
    return wrap == SGI_GL_REPEAT || wrap == SGI_GL_CLAMP ||
           wrap == SGI_GL_CLAMP_TO_EDGE || wrap == SGI_GL_CLAMP_TO_BORDER;
}

/* OpenGL 1.2.1 table 3.16: exact identities remain distinct in a chain. */
static uint32_t sgi_soft_texture_internal_format(uint32_t format)
{
    switch (format) {
    case SGI_GL_DEPTH_COMPONENT16_SGIX:
    case SGI_GL_DEPTH_COMPONENT24_SGIX:
    case SGI_GL_DEPTH_COMPONENT32_SGIX:
        return SGI_GL_DEPTH_COMPONENT;
    case SGI_GL_ALPHA4: case SGI_GL_ALPHA8:
    case SGI_GL_ALPHA12: case SGI_GL_ALPHA16:
        return SGI_GL_ALPHA;
    case SGI_GL_LUMINANCE4: case SGI_GL_LUMINANCE8:
    case SGI_GL_LUMINANCE12: case SGI_GL_LUMINANCE16:
        return SGI_GL_LUMINANCE;
    case SGI_GL_LUMINANCE4_ALPHA4: case SGI_GL_LUMINANCE6_ALPHA2:
    case SGI_GL_LUMINANCE8_ALPHA8: case SGI_GL_LUMINANCE12_ALPHA4:
    case SGI_GL_LUMINANCE12_ALPHA12: case SGI_GL_LUMINANCE16_ALPHA16:
        return SGI_GL_LUMINANCE_ALPHA;
    case SGI_GL_INTENSITY4: case SGI_GL_INTENSITY8:
    case SGI_GL_INTENSITY12: case SGI_GL_INTENSITY16:
        return SGI_GL_INTENSITY;
    case SGI_GL_R3_G3_B2: case SGI_GL_RGB4: case SGI_GL_RGB5:
    case SGI_GL_RGB8: case SGI_GL_RGB10: case SGI_GL_RGB12: case SGI_GL_RGB16:
        return SGI_GL_RGB;
    case SGI_GL_RGBA2: case SGI_GL_RGBA4: case SGI_GL_RGB5_A1:
    case SGI_GL_RGBA8: case SGI_GL_RGB10_A2:
    case SGI_GL_RGBA12: case SGI_GL_RGBA16:
        return SGI_GL_RGBA;
    default:
        return sgi_soft_texture_format(format) ? format : 0;
    }
}

static bool sgi_soft_texture_mipmap(uint32_t filter)
{
    return filter >= SGI_GL_NEAREST_MIPMAP_NEAREST &&
           filter <= SGI_GL_LINEAR_MIPMAP_LINEAR;
}

static bool sgi_soft_texture_sharpen(uint32_t filter)
{
    return filter == SGI_GL_LINEAR_SHARPEN_SGIS ||
           filter == SGI_GL_LINEAR_SHARPEN_ALPHA_SGIS ||
           filter == SGI_GL_LINEAR_SHARPEN_COLOR_SGIS;
}

static bool sgi_soft_texture_detail(uint32_t filter)
{
    return filter == SGI_GL_LINEAR_DETAIL_SGIS ||
           filter == SGI_GL_LINEAR_DETAIL_ALPHA_SGIS ||
           filter == SGI_GL_LINEAR_DETAIL_COLOR_SGIS;
}

static bool sgi_soft_texture_dimensions(uint32_t w, uint32_t h)
{
    return w && h && w <= INT32_MAX && h <= INT32_MAX &&
           is_power_of_2(w) && is_power_of_2(h);
}

static uint8_t sgi_soft_texture_component_mask(uint32_t format)
{
    switch (format) {
    case SGI_GL_ALPHA:
        return 1u << 3;
    case SGI_GL_LUMINANCE: case SGI_GL_INTENSITY:
        return 1u;
    case SGI_GL_LUMINANCE_ALPHA:
        return (1u << 0) | (1u << 3);
    case SGI_GL_RGB:
        return 7;
    case SGI_GL_RGBA:
        return 15;
    default:
        return 0;
    }
}

static bool sgi_soft_texture_component_bits_valid(uint32_t format,
                                                   const uint8_t bits[4])
{
    uint8_t mask = sgi_soft_texture_component_mask(format);

    if (!mask) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        if ((mask & (1u << k)) && (!bits[k] || bits[k] > 16)) {
            return false;
        }
    }
    return true;
}

static const SGIGLTexLevel *sgi_soft_texture_level(const SGIGLTexPyramid *p,
                                                  uint32_t level)
{
    for (uint32_t i = 0; i < p->count; i++) {
        if (p->levels[i].level == level) {
            return &p->levels[i];
        }
    }
    return NULL;
}

static bool sgi_soft_texture_border_supported(const SGIGLTexImage *im,
                                               const SGIGLTexLevel *level)
{
    uint32_t border = level ? level->border : im->border;
    bool callback = im->volume ?
        (level ? (level->read_border_texel_3d ||
                  level->read_border_texel_3d_components) :
                 (im->read_border_texel_3d ||
                  im->read_border_texel_3d_components)) :
        (level ? (level->read_border_texel ||
                  level->read_border_texel_components) :
                 (im->read_border_texel ||
                  im->read_border_texel_components));

    return border <= 1 && (!border || callback);
}

static bool sgi_soft_texture_shadow_supported(const SGIGLTexImage *im,
                                               uint32_t format)
{
    if ((im->shadow_compare &&
         im->shadow_compare != SGI_GL_TEXTURE_LEQUAL_R_SGIX &&
         im->shadow_compare != SGI_GL_TEXTURE_GEQUAL_R_SGIX) ||
        (im->shadow_compare &&
         (!isfinite(im->shadow_ambient) || im->shadow_ambient < 0 ||
          im->shadow_ambient > 1))) {
        return false;
    }
    /* SGIX_depth_texture alone does not define a texture environment. */
    return format != SGI_GL_DEPTH_COMPONENT ||
           (im->shadow_compare && !sgi_soft_texture_sharpen(im->mag_filter) &&
            !sgi_soft_texture_detail(im->mag_filter));
}

static bool sgi_soft_texture_pyramid(SGISoftDraw *r, const SGIGLTexImage *im,
                                     bool report)
{
    const SGIGLTexPyramid *p = im->pyramid;
    const SGIGLTexLevel *base;
    bool mipmap = sgi_soft_texture_mipmap(im->min_filter);
    bool border_finite = true;
    uint32_t last;

    for (unsigned k = 0; k < 4; k++) {
        border_finite &= isfinite(im->border_color[k]);
    }
    if (p->count > SGI_GL_TEX_LEVELS_MAX || (p->count && !p->levels) ||
        !isfinite(p->min_lod) || !isfinite(p->max_lod) ||
        p->min_lod > p->max_lod || !border_finite ||
        !sgi_soft_texture_wrap(im->wrap_s) ||
        (!im->one_dimensional && !sgi_soft_texture_wrap(im->wrap_t)) ||
        (im->volume && !sgi_soft_texture_wrap(im->wrap_r)) ||
        (im->min_filter != SGI_GL_NEAREST && im->min_filter != SGI_GL_LINEAR &&
         !mipmap) ||
        (sgi_soft_texture_detail(im->mag_filter) && im->detail_image &&
         (im->detail_image->read_texel_components ||
          im->detail_image->read_border_texel_components) &&
         !sgi_soft_texture_component_bits_valid(
             im->detail_image->base_format,
             im->detail_image->component_bits)) ||
        (im->mag_filter != SGI_GL_NEAREST && im->mag_filter != SGI_GL_LINEAR &&
         !sgi_soft_texture_sharpen(im->mag_filter) &&
         !sgi_soft_texture_detail(im->mag_filter))) {
        goto unsupported;
    }
    for (uint32_t i = 1; i < p->count; i++) {
        if (p->levels[i - 1].level >= p->levels[i].level) {
            goto unsupported;
        }
    }
    if (mipmap && p->max_level < p->base_level) {
        goto incomplete;
    }
    base = sgi_soft_texture_level(p, p->base_level);
    if (!base) {
        goto incomplete;
    }
    if (!sgi_soft_texture_shadow_supported(im, base->base_format) ||
        !sgi_soft_texture_border_supported(im, base) ||
        sgi_soft_texture_internal_format(base->internal_format) !=
        base->base_format || !sgi_soft_texture_format(base->base_format) ||
        ((base->read_texel_components || base->read_texel_3d_components ||
          base->read_border_texel_components ||
          base->read_border_texel_3d_components) &&
         !sgi_soft_texture_component_bits_valid(base->base_format,
                                                base->component_bits)) ||
        (r->st->texture_env_mode == SGI_GL_DECAL &&
         base->base_format != SGI_GL_RGB && base->base_format != SGI_GL_RGBA)) {
        goto unsupported;
    }
    if (im->one_dimensional && base->height != 1) {
        goto unsupported;
    }
    if (!sgi_soft_texture_dimensions(base->width, base->height) ||
        (im->volume && (!base->depth || base->depth > INT32_MAX ||
                        !is_power_of_2(base->depth)))) {
        goto incomplete;
    }
    last = p->base_level;
    if (mipmap) {
        uint32_t extent = MAX(base->width, base->height);
        uint32_t k;

        if (im->volume) {
            extent = MAX(extent, base->depth);
        }
        k = 31 - clz32(extent);
        last = MIN((uint64_t)p->base_level + k, p->max_level);
    }
    /* Unsupported relevant forms cannot be concealed by missing images. */
    for (uint32_t i = 0; i < p->count; i++) {
        const SGIGLTexLevel *l = &p->levels[i];

        if (l->level < p->base_level || l->level > last) {
            continue;
        }
        if (im->one_dimensional && l->height != 1) {
            goto unsupported;
        }
        if (!sgi_soft_texture_shadow_supported(im, l->base_format)) {
            goto unsupported;
        }
        if (!sgi_soft_texture_border_supported(im, l) ||
            !sgi_soft_texture_format(l->base_format) ||
            sgi_soft_texture_internal_format(l->internal_format) !=
            l->base_format ||
            ((l->read_texel_components || l->read_texel_3d_components ||
              l->read_border_texel_components ||
              l->read_border_texel_3d_components) &&
             !sgi_soft_texture_component_bits_valid(l->base_format,
                                                    l->component_bits))) {
            goto unsupported;
        }
    }
    for (uint64_t id = p->base_level; id <= last; id++) {
        const SGIGLTexLevel *l = sgi_soft_texture_level(p, id);
        uint32_t shift = id - p->base_level;

        if (!l || (im->volume ?
                   !(l->read_texel_3d || l->read_texel_3d_components) :
                   !(l->read_texel || l->read_texel_components)) ||
            (im->volume && l->depth != MAX(1, base->depth >> shift)) ||
            l->width != MAX(1, base->width >> shift) ||
            l->height != MAX(1, base->height >> shift) ||
            l->internal_format != base->internal_format ||
            l->border != base->border) {
            goto incomplete;
        }
    }
    r->texture = im;
    r->texture_base = base;
    r->texture_last = last;
    return true;

unsupported:
    if (report) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: unsupported texture state "
                      "in pyramid, draw dropped\n");
    }
    return false;
incomplete:
    if (report) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: incomplete texture image pyramid, "
                      "texturing disabled for draw\n");
    }
    return true;
}

/* Incomplete images disable texturing; unsupported states drop the draw. */
static bool sgi_soft_texture_setup(SGISoftDraw *r, bool report)
{
    const SGIGLTexImage *im = r->t->texture;
    const SGIGLDrawState *st = r->st;

    /*
     * With several targets enabled the highest dimension wins: 3D, then
     * 2D, then 1D (OpenGL 1.2.1, 3.8.10). The family supplies the winning
     * target's image, so the image is checked against that target only.
     */
    unsigned target = st->texture_3d ? 3 : st->texture_2d ? 2 :
                      st->texture_1d ? 1 : 0;

    if (!target || st->index_bits) {
        return true;
    }
    if (im && ((im->one_dimensional && im->volume) ||
               (target == 1) != im->one_dimensional ||
               (im->one_dimensional && !im->pyramid && im->height != 1))) {
        if (report) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: incompatible 1D texture image, "
                          "draw dropped\n");
        }
        return false;
    }
    /*
     * A volume image under the 2D enable alone stays accepted, as before
     * targets were ranked. With 1D enabled as well the 2D binding wins,
     * and a volume is not a 2D image.
     */
    if (target == 2 && im && im->volume && st->texture_1d) {
        if (report) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: volume texture image for a 2D "
                          "target, draw dropped\n");
        }
        return false;
    }
    if (target == 3 && im && !im->volume) {
        if (report) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: 3D texturing requires a volume "
                          "image, draw dropped\n");
        }
        return false;
    }
    if (st->texture_env_mode != SGI_GL_REPLACE &&
        st->texture_env_mode != SGI_GL_MODULATE &&
        st->texture_env_mode != SGI_GL_DECAL &&
        st->texture_env_mode != SGI_GL_BLEND) {
        if (report) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: unsupported texture state "
                          "environment 0x%x, draw dropped\n",
                          st->texture_env_mode);
        }
        return false;
    }
    if (im && im->max_clamp &&
        !(im->max_clamp_s >= 0 && im->max_clamp_s <= 1 &&
          im->max_clamp_t >= 0 && im->max_clamp_t <= 1 &&
          im->max_clamp_r >= 0 && im->max_clamp_r <= 1)) {
        if (report) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-gl: invalid coordinate clamp, draw dropped\n");
        }
        return false;
    }
    if (im && (!isfinite(im->lod_scale_s) || im->lod_scale_s < 0 ||
               (!im->one_dimensional && (!isfinite(im->lod_scale_t) ||
                                         im->lod_scale_t < 0)) ||
               (im->volume && (!isfinite(im->lod_scale_r) ||
                               im->lod_scale_r < 0)))) {
        if (report) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-gl: invalid derivative scales, draw dropped\n");
        }
        return false;
    }
    if (im && im->pyramid) {
        return sgi_soft_texture_pyramid(r, im, report);
    }
    if (im) {
        bool border_finite = true;

        for (int k = 0; k < 4; k++) {
            border_finite &= isfinite(im->border_color[k]);
        }
        if (!sgi_soft_texture_shadow_supported(im, im->base_format) ||
            !sgi_soft_texture_border_supported(im, NULL) ||
            !border_finite ||
            !sgi_soft_texture_format(im->base_format) ||
            ((im->read_texel_components || im->read_texel_3d_components ||
              im->read_border_texel_components ||
              im->read_border_texel_3d_components) &&
             !sgi_soft_texture_component_bits_valid(im->base_format,
                                                    im->component_bits)) ||
            (sgi_soft_texture_detail(im->mag_filter) && im->detail_image &&
             (im->detail_image->read_texel_components ||
              im->detail_image->read_border_texel_components) &&
             !sgi_soft_texture_component_bits_valid(
                 im->detail_image->base_format,
                 im->detail_image->component_bits)) ||
            !sgi_soft_texture_wrap(im->wrap_s) ||
            (!im->one_dimensional && !sgi_soft_texture_wrap(im->wrap_t)) ||
            (im->volume && !sgi_soft_texture_wrap(im->wrap_r)) ||
            im->min_filter != im->mag_filter ||
            (im->min_filter != SGI_GL_NEAREST &&
             im->min_filter != SGI_GL_LINEAR) ||
            (st->texture_env_mode == SGI_GL_DECAL &&
             im->base_format != SGI_GL_RGB && im->base_format != SGI_GL_RGBA)) {
            if (report) {
                qemu_log_mask(LOG_UNIMP, "sgi-gl: unsupported texture state "
                              "format 0x%x border %u filters 0x%x/0x%x "
                              "wraps 0x%x/0x%x/0x%x environment 0x%x%s, "
                              "draw dropped\n",
                              im->base_format, im->border, im->min_filter,
                              im->mag_filter, im->wrap_s, im->wrap_t,
                              im->wrap_r, st->texture_env_mode,
                              border_finite ? "" : " nonfinite border colour");
            }
            return false;
        }
    }
    if (!im || (im->volume ?
                !(im->read_texel_3d || im->read_texel_3d_components) :
                !(im->read_texel || im->read_texel_components)) ||
        !im->width || !im->height ||
        im->width > INT32_MAX || im->height > INT32_MAX ||
        !is_power_of_2(im->width) || !is_power_of_2(im->height) ||
        (im->volume && (!im->depth || im->depth > INT32_MAX ||
                        !is_power_of_2(im->depth)))) {
        if (report) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: incomplete texture image, "
                          "texturing disabled for draw\n");
        }
        return true;
    }
    r->texture = im;
    return true;
}

/* Validate every surviving vertex before any part of its primitive draws. */
/*
 * Whether a clipped vertex has a clip w and the texture coordinates im
 * uses: s and q always, t unless the image is one-dimensional, and r for
 * a shadow comparison against a depth image.
 */
static bool sgi_soft_texcoord_valid(const SGIGLTexImage *im,
                                    const SGIGLTexLevel *base,
                                    const SGIVolumeVertex *v)
{
    bool valid = isfinite(v->clip[3]) && v->clip[3] > 0 &&
                 v->attr[SGI_GL_V_TEXCOORD + 3] > 0;

    for (int k = 0; k < 4; k++) {
        if (!im->one_dimensional || k == 0 || k == 3 ||
            (k == 2 && im->shadow_compare &&
             (base ? base->base_format : im->base_format) ==
             SGI_GL_DEPTH_COMPONENT)) {
            valid &= isfinite(v->attr[SGI_GL_V_TEXCOORD + k]);
        }
    }
    return valid;
}

static bool sgi_soft_texture_vertex_valid(const SGISoftDraw *r,
                                          const SGIVolumeVertex *v)
{
    bool valid;

    if (!r->texture) {
        return true;
    }
    valid = sgi_soft_texcoord_valid(r->texture, r->texture_base, v);
    if (!valid) {
        sgi_soft_texture_report(r, 4, "invalid clipped coordinates, "
                                "primitive dropped");
    }
    return valid;
}

/* Meaningful base components, including the border, before filtering. */
static void sgi_soft_texture_components(uint32_t format, double v[4])
{
    switch (format) {
    case SGI_GL_ALPHA:
        v[0] = v[1] = v[2] = 1;
        break;
    case SGI_GL_LUMINANCE:
        v[3] = 1;
        /* fall through */
    case SGI_GL_LUMINANCE_ALPHA:
        v[1] = v[2] = v[0];
        break;
    case SGI_GL_INTENSITY:
        v[1] = v[2] = v[3] = v[0];
        break;
    case SGI_GL_RGB:
        v[3] = 1;
        break;
    }
}

static void sgi_soft_texture_shadow(const SGIGLTexImage *im, double depth,
                                     double reference, double out[4])
{
    bool pass = im->shadow_compare == SGI_GL_TEXTURE_LEQUAL_R_SGIX ?
                depth <= reference : depth >= reference;

    out[0] = out[1] = out[2] = pass ? 1 : im->shadow_ambient;
    out[3] = 1;
}

/*
 * GL_REPEAT's wrap of a texel index into [0, n). Image and level sizes are
 * powers of two wherever setup admits them, and there the mask gives the
 * remainder without two 64-bit divisions a tap.
 */
static inline int64_t sgi_soft_texture_repeat(int64_t i, uint32_t n)
{
    return is_power_of_2(n) ? i & (n - 1) : (i % n + n) % n;
}

/*
 * An 8-bit channel normalized, k / 255.0 for each byte k: the same
 * quotients the division gives, computed once instead of four times a tap.
 */
static double sgi_soft_unorm8[256];

static void __attribute__((constructor)) sgi_soft_unorm8_init(void)
{
    for (unsigned k = 0; k < ARRAY_SIZE(sgi_soft_unorm8); k++) {
        sgi_soft_unorm8[k] = k / 255.0;
    }
}

/*
 * The texels one draw has read, by level and interior index, after
 * conversion to components in [0, 1]. A family's image stays unchanged
 * through a synchronous draw (SGIGLTexImage), so a texel read twice in one
 * draw reads the same both times: a bilinear tap reads four, and adjacent
 * fragments mostly the same four. Each thread draws through its own
 * table of about 256 KiB, allocated on its first textured draw and freed
 * when the thread exits through qemu_thread_exit() or its start routine.
 * A thread that never does, such as the main thread or a band worker,
 * keeps its table until the process ends. A draw starts by taking a new
 * serial, which empties the table without clearing it. Only the draw's
 * own image is cached: a detail texture is sampled through a copy of it
 * whose wrap differs. Border texels, depth texels, whose value depends on
 * the comparison reference, and failed reads always go to the family.
 */
#define SGI_SOFT_TEXEL_CACHE_SIZE 4096
#define SGI_SOFT_FIXED_LEVELS 14

/*
 * The fixed-point sampler's texels of one level for one draw, by interior
 * index: each the read tag in the high word and the packed RGBA8 texel,
 * components expanded, in the low (sgi_soft_fixed_texel()).
 */
typedef struct SGISoftFixedStore {
    uint64_t *cell;
    size_t size;
    /*
     * Every texel of the level, for a draw that reads them all first, in
     * the sampler's lanes (sgi_soft_fixed_lanes()).
     */
    uint64_t *plain;
    size_t plain_size;
} SGISoftFixedStore;

typedef struct SGISoftTexelEntry {
    uint64_t serial;
    const SGIGLTexLevel *level;
    uint32_t x, y, z;
    double out[4];
} SGISoftTexelEntry;

typedef struct SGISoftTexelCache {
    uint64_t serial;
    const SGIGLTexImage *image;
    /*
     * Set for a fragment that minifies by more than two texels a pixel
     * without mipmaps: its neighbours read other texels, and filling the
     * cache would only cost.
     */
    bool bypass;
    /*
     * The fixed-point sampler's levels, and the tag a cell read in this
     * draw carries: the draw's serial in 31 bits, never zero, and with
     * SGI_SOFT_FIXED_FAILED set for a texel whose read failed.
     */
    uint32_t tag;
    SGISoftFixedStore fixed[SGI_SOFT_FIXED_LEVELS];
    SGISoftTexelEntry entry[SGI_SOFT_TEXEL_CACHE_SIZE];
} SGISoftTexelCache;

#define SGI_SOFT_FIXED_FAILED 0x80000000u

static __thread SGISoftTexelCache *sgi_soft_texel_cache;
static __thread Notifier sgi_soft_texel_cache_exit;

static void sgi_soft_texel_cache_free(Notifier *n, void *unused)
{
    for (unsigned i = 0; sgi_soft_texel_cache && i < SGI_SOFT_FIXED_LEVELS;
         i++) {
        g_free(sgi_soft_texel_cache->fixed[i].cell);
        g_free(sgi_soft_texel_cache->fixed[i].plain);
    }
    g_free(sgi_soft_texel_cache);
    sgi_soft_texel_cache = NULL;
}

static SGISoftTexelCache *sgi_soft_texel_cache_begin(
    const SGIGLTexImage *image)
{
    SGISoftTexelCache *c = sgi_soft_texel_cache;

    if (!c) {
        c = sgi_soft_texel_cache = g_new0(SGISoftTexelCache, 1);
        sgi_soft_texel_cache_exit.notify = sgi_soft_texel_cache_free;
        qemu_thread_atexit_add(&sgi_soft_texel_cache_exit);
    }
    c->serial++;
    c->image = image;
    c->bypass = false;
    c->tag = c->serial & ~SGI_SOFT_FIXED_FAILED;
    if (!c->tag) {
        /* Tags have come round: cells from 2^31 draws ago would match. */
        for (unsigned i = 0; i < SGI_SOFT_FIXED_LEVELS; i++) {
            if (c->fixed[i].cell) {
                memset(c->fixed[i].cell, 0,
                       c->fixed[i].size * sizeof(*c->fixed[i].cell));
            }
        }
        c->serial++;
        c->tag = c->serial & ~SGI_SOFT_FIXED_FAILED;
    }
    return c;
}

static SGISoftTexelEntry *sgi_soft_texel_slot(SGISoftTexelCache *c,
                                              const SGIGLTexLevel *level,
                                              uint32_t x, uint32_t y,
                                              uint32_t z)
{
    uint32_t hash = x * 0x9e3779b1u ^ y * 0x85ebca77u ^ z * 0xc2b2ae3du ^
                    (uint32_t)((uintptr_t)level >> 4) * 0x27d4eb2fu;

    return &c->entry[(hash ^ hash >> 15) & (SGI_SOFT_TEXEL_CACHE_SIZE - 1)];
}

static bool sgi_soft_texture_tap(const SGISoftDraw *r,
                                  const SGIGLTexLevel *level,
                                  int64_t x, int64_t y, int64_t z,
                                  double reference, double out[4])
{
    const SGIGLTexImage *im = r->texture;
    uint32_t width = level ? level->width : im->width;
    uint32_t height = level ? level->height : im->height;
    uint32_t depth = im->volume ?
                     (level ? level->depth : im->depth) : 1;
    uint32_t format = level ? level->base_format : im->base_format;
    uint32_t rgba;
    uint16_t component[4];
    bool raw_components = false;
    SGISoftTexelEntry *slot = NULL;

    if (im->wrap_s == SGI_GL_REPEAT) {
        x = sgi_soft_texture_repeat(x, width);
    }
    if (!im->one_dimensional && im->wrap_t == SGI_GL_REPEAT) {
        y = sgi_soft_texture_repeat(y, height);
    }
    if (im->volume && im->wrap_r == SGI_GL_REPEAT) {
        z = sgi_soft_texture_repeat(z, depth);
    }
    if (x < 0 || y < 0 || z < 0 || x >= width || y >= height || z >= depth) {
        uint32_t border = level ? level->border : im->border;

        if (!border) {
            for (int k = 0; k < 4; k++) {
                out[k] = MIN(MAX(im->border_color[k], 0), 1);
            }
            if (format == SGI_GL_DEPTH_COMPONENT) {
                sgi_soft_texture_shadow(im, out[0], reference, out);
            } else {
                sgi_soft_texture_components(format, out);
            }
            return true;
        }
        /* Signed indices cover the stored faces, edges and corners. */
        bool read;

        if (x < -1 || y < -1 || x > width || y > height ||
            (im->volume && (z < -1 || z > depth))) {
            goto read_failure;
        }
        if (im->volume) {
            raw_components = level ? !!level->read_border_texel_3d_components :
                                    !!im->read_border_texel_3d_components;
            read = level ?
                (raw_components ?
                 level->read_border_texel_3d_components(level->opaque,
                     level->level, x, y, z, component) :
                 level->read_border_texel_3d &&
                 level->read_border_texel_3d(level->opaque, level->level,
                                             x, y, z, &rgba)) :
                (raw_components ?
                 im->read_border_texel_3d_components(im->opaque, x, y, z,
                                                      component) :
                 im->read_border_texel_3d &&
                 im->read_border_texel_3d(im->opaque, x, y, z, &rgba));
        } else {
            raw_components = level ? !!level->read_border_texel_components :
                                    !!im->read_border_texel_components;
            read = level ?
                (raw_components ?
                 level->read_border_texel_components(level->opaque,
                     level->level, x, y, component) :
                 level->read_border_texel &&
                 level->read_border_texel(level->opaque, level->level,
                                          x, y, &rgba)) :
                (raw_components ?
                 im->read_border_texel_components(im->opaque, x, y,
                                                   component) :
                 im->read_border_texel &&
                 im->read_border_texel(im->opaque, x, y, &rgba));
        }
        if (!read) {
            goto read_failure;
        }
    } else {
        bool read;

        if (r->texels && r->texels->image == im && !r->texels->bypass &&
            format != SGI_GL_DEPTH_COMPONENT) {
            slot = sgi_soft_texel_slot(r->texels, level, x, y, z);
            if (slot->serial == r->texels->serial && slot->level == level &&
                slot->x == x && slot->y == y && slot->z == z) {
                memcpy(out, slot->out, sizeof(slot->out));
                return true;
            }
        }
        if (im->volume) {
            raw_components = level ? !!level->read_texel_3d_components :
                                    !!im->read_texel_3d_components;
            read = level ?
                (raw_components ?
                 level->read_texel_3d_components(level->opaque, level->level,
                                                x, y, z, component) :
                 level->read_texel_3d(level->opaque, level->level, x, y, z,
                                      &rgba)) :
                (raw_components ?
                 im->read_texel_3d_components(im->opaque, x, y, z, component) :
                 im->read_texel_3d(im->opaque, x, y, z, &rgba));
        } else {
            raw_components = level ? !!level->read_texel_components :
                                    !!im->read_texel_components;
            read = level ?
                (raw_components ?
                 level->read_texel_components(level->opaque, level->level,
                                              x, y, component) :
                 level->read_texel(level->opaque, level->level, x, y, &rgba)) :
                (raw_components ?
                 im->read_texel_components(im->opaque, x, y, component) :
                 im->read_texel(im->opaque, x, y, &rgba));
        }

        if (!read) {
            goto read_failure;
        }
    }
    if (format == SGI_GL_DEPTH_COMPONENT) {
        /* Depth arrives only through the packed callback. */
        if (raw_components) {
            goto read_failure;
        }
        sgi_soft_texture_shadow(im, rgba / (double)UINT32_MAX, reference, out);
        return true;
    }
    if (raw_components) {
        const uint8_t *bits = level ? level->component_bits :
                                      im->component_bits;
        uint8_t mask = sgi_soft_texture_component_mask(format);

        if (!sgi_soft_texture_component_bits_valid(format, bits)) {
            goto read_failure;
        }
        for (int k = 0; k < 4; k++) {
            if (mask & (1u << k)) {
                uint32_t maximum = (1u << bits[k]) - 1;

                if (component[k] > maximum) {
                    sgi_soft_texture_report(r, 64,
                        "component outside declared precision, "
                        "fragment dropped");
                    return false;
                }
                out[k] = (double)component[k] / maximum;
            }
        }
    } else {
        for (int k = 0; k < 4; k++) {
            out[k] = sgi_soft_unorm8[(rgba >> (24 - k * 8)) & 255];
        }
    }
    sgi_soft_texture_components(format, out);
    if (slot) {
        *slot = (SGISoftTexelEntry) {
            .serial = r->texels->serial, .level = level,
            .x = x, .y = y, .z = z,
        };
        memcpy(slot->out, out, sizeof(slot->out));
    }
    return true;

read_failure:
    sgi_soft_texture_report(r, 1, "image-read failure, fragment dropped");
    return false;
}

/* Reduce finite coordinates before any float-to-integer conversion. */
static double sgi_soft_texture_axis(double x, uint32_t n, uint32_t wrap,
                                    double upper)
{
    double half = 0.5 / n;

    switch (wrap) {
    case SGI_GL_REPEAT:
        return x - floor(x);
    case SGI_GL_CLAMP_TO_EDGE:
        return MIN(MAX(x, half), 1 - half);
    case SGI_GL_CLAMP_TO_BORDER:
        return MIN(MAX(x, -half), 1 + half);
    default:
        return MIN(MAX(x, 0), upper);
    }
}

/* Sampling always reduces coordinates before any integer conversion. */
static bool sgi_soft_texture_sample(const SGISoftDraw *r,
                                     const SGIGLTexLevel *level,
                                     uint32_t filter, double s, double t,
                                     double rr,
                                     double texel[4])
{
    const SGIGLTexImage *im = r->texture;
    uint32_t width = level ? level->width : im->width;
    uint32_t height = level ? level->height : im->height;
    uint32_t depth = im->volume ?
                     (level ? level->depth : im->depth) : 1;

    double reference = rr;

    memset(texel, 0, 4 * sizeof(*texel));
    s = sgi_soft_texture_axis(s, width, im->wrap_s,
                              im->max_clamp ? im->max_clamp_s : 1);
    t = im->one_dimensional ? 0 :
        sgi_soft_texture_axis(t, height, im->wrap_t,
                              im->max_clamp ? im->max_clamp_t : 1);
    if (im->volume) {
        rr = sgi_soft_texture_axis(rr, depth, im->wrap_r,
                                   im->max_clamp ? im->max_clamp_r : 1);
    }
    if (filter == SGI_GL_NEAREST) {
        int64_t x = floor(width * s), y = floor(height * t);
        int64_t z = im->volume ? floor(depth * rr) : 0;

        if (s == 1 && im->wrap_s != SGI_GL_CLAMP_TO_BORDER) {
            x = width - 1;
        }
        if (t == 1 && im->wrap_t != SGI_GL_CLAMP_TO_BORDER) {
            y = height - 1;
        }
        if (rr == 1 && im->volume &&
            im->wrap_r != SGI_GL_CLAMP_TO_BORDER) {
            z = depth - 1;
        }
        return sgi_soft_texture_tap(r, level, x, y, z, reference, texel);
    }
    double u = width * s - 0.5;
    double v = im->one_dimensional ? 0 : height * t - 0.5;
    int64_t x = floor(u), y = floor(v);
    double w = depth * rr - 0.5;
    int64_t z = im->volume ? floor(w) : 0;
    double a = u - x, b = v - y;
    double c = im->volume ? w - z : 0;

    for (int k = 0; k < (im->volume ? 2 : 1); k++) {
        for (int j = 0; j < (im->one_dimensional ? 1 : 2); j++) {
            for (int i = 0; i < 2; i++) {
                double weight = (i ? a : 1 - a) * (j ? b : 1 - b) *
                                (im->volume ? (k ? c : 1 - c) : 1);
                double tap[4];

                if (!weight) {
                    continue;
                }
                if (!sgi_soft_texture_tap(r, level, x + i, y + j, z + k,
                                          reference, tap)) {
                    return false;
                }
                for (int channel = 0; channel < 4; channel++) {
                    texel[channel] += weight * tap[channel];
                }
            }
        }
    }
    return true;
}

typedef struct SGISoftTexSelection {
    uint32_t first, second, filter;
    double alpha, lambda;
} SGISoftTexSelection;

/* Relative levels avoid rounding a large absolute base in floating point. */
static bool sgi_soft_texture_select(uint32_t min, uint32_t mag,
                                     const SGIGLTexPyramid *p, uint32_t last,
                                     double rho, SGISoftTexSelection *out)
{
    bool enhanced = sgi_soft_texture_sharpen(mag) ||
                    sgi_soft_texture_detail(mag);
    double lambda, c = enhanced ?
        (min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
         min == SGI_GL_LINEAR_MIPMAP_NEAREST ? 0.5 : 0) :
        (mag == SGI_GL_LINEAR &&
         (min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
          min == SGI_GL_NEAREST_MIPMAP_LINEAR) ? 0.5 : 0);
    uint32_t range = last - p->base_level;

    if (!isfinite(rho) || rho < 0) {
        return false;
    }
    lambda = rho ? log2(rho) : p->min_lod;
    if (!isfinite(lambda)) {
        return false;
    }
    lambda = MIN(MAX(lambda, p->min_lod), p->max_lod);
    *out = (SGISoftTexSelection) {
        .first = p->base_level, .second = p->base_level,
        .filter = lambda <= c ? mag : min, .lambda = lambda,
    };
    if (lambda <= c || !sgi_soft_texture_mipmap(min)) {
        return true;
    }
    out->filter = min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
                  min == SGI_GL_NEAREST_MIPMAP_LINEAR ? SGI_GL_NEAREST :
                                                      SGI_GL_LINEAR;
    if (min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
        min == SGI_GL_LINEAR_MIPMAP_NEAREST) {
        double relative;

        if (lambda <= 0.5) {
            relative = 0;
        } else if (lambda > range + 0.5) {
            relative = range;
        } else {
            relative = floor(lambda);
            /* Adding 0.5 can round the next value above a half tie onto it. */
            relative += lambda - relative > 0.5;
        }
        out->first += (uint32_t)MIN(MAX(relative, 0), range);
        out->second = out->first;
    } else {
        double relative = MIN(MAX(lambda, 0), range);
        uint32_t n = floor(relative);

        out->first += n;
        out->second = out->first == last ? last : out->first + 1;
        out->alpha = relative - n;
    }
    return true;
}

/* Projective gradients precede wrapping; D is Q/W, not geometry W. */
static double sgi_soft_texture_lod_scale(double scale)
{
    return scale ? scale : 1.0;
}

static bool sgi_soft_texture_derivatives(const double value[4],
                                          const double dx[4],
                                          const double dy[4],
                                          double ns, double nt,
                                          double nr,
                                          double out[5])
{
    double d = value[2], d2 = d * d;

    if (!(d > 0) || !isfinite(d2) || !d2) {
        return false;
    }
    for (unsigned k = 0; k < 2; k++) {
        out[2 * k] = (dx[k] * d - value[k] * dx[2]) / d2;
        out[2 * k + 1] = (dy[k] * d - value[k] * dy[2]) / d2;
        if (!isfinite(value[k] / d) || !isfinite(out[2 * k]) ||
            !isfinite(out[2 * k + 1])) {
            return false;
        }
    }
    out[4] = MAX(hypot(ns * out[0], nt * out[2]),
                 hypot(ns * out[1], nt * out[3]));
    if (nr) {
        double drdx = (dx[3] * d - value[3] * dx[2]) / d2;
        double drdy = (dy[3] * d - value[3] * dy[2]) / d2;

        if (!isfinite(value[3] / d) || !isfinite(drdx) || !isfinite(drdy)) {
            return false;
        }
        out[4] = MAX(hypot(hypot(ns * out[0], nt * out[2]), nr * drdx),
                     hypot(hypot(ns * out[1], nt * out[3]), nr * drdy));
    }
    return isfinite(out[4]);
}

/*
 * The last level selection a thread's draw made, by the footprint it was
 * made for: sgi_soft_texture_select() depends on nothing else that varies
 * within a draw, and the fragments of a primitive whose Q/W is constant
 * across it all have the same footprint.
 */
typedef struct SGISoftTexSelectionMemo {
    uint64_t serial;
    uint64_t rho;
    SGISoftTexSelection selected;
} SGISoftTexSelectionMemo;

static __thread SGISoftTexSelectionMemo sgi_soft_texture_selected;

static bool sgi_soft_texture_select_memo(const SGISoftDraw *r, double rho,
                                         SGISoftTexSelection *out)
{
    const SGIGLTexImage *im = r->texture;
    SGISoftTexSelectionMemo *m = &sgi_soft_texture_selected;
    bool memo = r->texels && r->texels->image == im;
    uint64_t bits;

    memcpy(&bits, &rho, sizeof(bits));
    if (memo && m->serial == r->texels->serial && m->rho == bits) {
        *out = m->selected;
        return true;
    }
    if (!sgi_soft_texture_select(im->min_filter, im->mag_filter,
                                 im->pyramid, r->texture_last, rho, out)) {
        return false;
    }
    if (memo) {
        *m = (SGISoftTexSelectionMemo) {
            .serial = r->texels->serial, .rho = bits, .selected = *out,
        };
    }
    return true;
}

static void sgi_soft_texture_mix(double value[4], const double other[4],
                                  double alpha)
{
    for (unsigned k = 0; k < 4; k++) {
        value[k] = (1 - alpha) * value[k] + alpha * other[k];
    }
}

static bool sgi_soft_texture_weight(const SGISoftDraw *r, double lambda,
                                     double *weight)
{
    const SGIGLTexImage *im = r->texture;

    *weight = MIN(MAX(-lambda / 4, 0), 1);
    if (im->read_filter_weight &&
        (!im->read_filter_weight(im->opaque, lambda, weight) ||
         !isfinite(*weight))) {
        sgi_soft_texture_report(r, 16, "invalid filter weight, "
                                      "fragment dropped");
        return false;
    }
    return true;
}

static bool sgi_soft_texture_sharpen_sample(const SGISoftDraw *r,
                                             uint32_t filter, double lambda,
                                             double s, double t, double rr,
                                             double texel[4])
{
    const SGIGLTexImage *im = r->texture;
    const SGIGLTexPyramid *p = im->pyramid;
    const SGIGLTexLevel *base = r->texture_base, *coarse;
    uint32_t extent = MAX(base->width, base->height);
    double other[4], weight;

    if (!sgi_soft_texture_sample(r, base, SGI_GL_LINEAR, s, t, rr, texel)) {
        return false;
    }
    if (im->volume) {
        extent = MAX(extent, base->depth);
    }
    coarse = p->base_level < UINT32_MAX ?
        sgi_soft_texture_level(p, p->base_level + 1) : NULL;
    if (extent <= 1 || p->max_level <= p->base_level || !coarse ||
        coarse->width != MAX(1, base->width / 2) ||
        coarse->height != MAX(1, base->height / 2) ||
        (im->volume && coarse->depth != MAX(1, base->depth / 2)) ||
        coarse->internal_format != base->internal_format ||
        coarse->base_format != base->base_format ||
        coarse->border != base->border ||
        !sgi_soft_texture_border_supported(im, coarse) ||
        (im->volume ? !coarse->read_texel_3d : !coarse->read_texel)) {
        return true;
    }
    if (!sgi_soft_texture_weight(r, lambda, &weight) ||
        !sgi_soft_texture_sample(r, coarse, SGI_GL_LINEAR, s, t, rr, other)) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        if ((k == 3 && filter == SGI_GL_LINEAR_SHARPEN_COLOR_SGIS) ||
            (k != 3 && filter == SGI_GL_LINEAR_SHARPEN_ALPHA_SGIS)) {
            continue;
        }
        texel[k] = MIN(MAX(texel[k] + weight * (texel[k] - other[k]), 0), 1);
    }
    return true;
}

static bool sgi_soft_texture_detail_sample(const SGISoftDraw *r,
                                            uint32_t filter, double lambda,
                                            double s, double t, double rr,
                                            double texel[4])
{
    const SGIGLTexImage *im = r->texture;
    const SGIGLTexLevel *base = r->texture_base, *detail = im->detail_image;
    SGIGLTexImage repeat = *im;
    SGISoftDraw draw = *r;
    double other[4], weight, ds, dt;
    int scale = MIN(-(int64_t)im->detail_level, INT_MAX);

    if (!sgi_soft_texture_sample(r, base, SGI_GL_LINEAR, s, t, rr, texel)) {
        return false;
    }
    if (im->one_dimensional || im->volume ||
        im->pyramid->base_level || !detail ||
        detail->level || detail->border ||
        !(detail->read_texel || detail->read_texel_components) ||
        !sgi_soft_texture_dimensions(detail->width, detail->height) ||
        detail->internal_format != base->internal_format ||
        detail->base_format != base->base_format || im->detail_level > 0) {
        return true;
    }
    if (im->detail_mode != SGI_GL_ADD && im->detail_mode != SGI_GL_MODULATE) {
        sgi_soft_texture_report(r, 32, "unsupported detail mode, "
                                      "fragment dropped");
        return false;
    }
    /* Scaling is in primary texels, independent of the detail dimensions. */
    ds = scalbn(s * base->width / detail->width, scale);
    dt = scalbn(t * base->height / detail->height, scale);
    if (!isfinite(ds) || !isfinite(dt)) {
        sgi_soft_texture_report(r, 32, "invalid detail coordinates, "
                                      "fragment dropped");
        return false;
    }
    repeat.wrap_s = repeat.wrap_t = SGI_GL_REPEAT;
    draw.texture = &repeat;
    if (!sgi_soft_texture_weight(r, lambda, &weight) ||
        !sgi_soft_texture_sample(&draw, detail, SGI_GL_LINEAR,
                                  ds, dt, 0, other)) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        double signed_detail = weight * (2 * other[k] - 1);

        if ((k == 3 && filter == SGI_GL_LINEAR_DETAIL_COLOR_SGIS) ||
            (k != 3 && filter == SGI_GL_LINEAR_DETAIL_ALPHA_SGIS)) {
            continue;
        }
        texel[k] = im->detail_mode == SGI_GL_ADD ?
                   texel[k] + signed_detail : texel[k] * (1 + signed_detail);
        texel[k] = MIN(MAX(texel[k], 0), 1);
    }
    return true;
}

/*
 * A filtered texel through the image's post-filter, if it has one, and
 * then by the texture environment env into a fragment's colour rgba,
 * for an image of base format format (OpenGL 1.2.1, 3.8.9). Inlined, so
 * a caller that names a constant format and environment carries no test
 * of them.
 */
static inline QEMU_ALWAYS_INLINE
void sgi_soft_texture_apply(const SGISoftDraw *r, uint32_t format,
                            uint32_t env, double texel[4], double rgba[4])
{
    const SGIGLTexImage *im = r->texture;

    if (im->post_filter) {
        im->post_filter(im->opaque, texel);
    }
    if (format == SGI_GL_DEPTH_COMPONENT) {
        format = SGI_GL_LUMINANCE;
    }
    for (int k = 0; k < 4; k++) {
        bool alpha = k == 3;

        if ((!alpha && format == SGI_GL_ALPHA) ||
            (alpha && (format == SGI_GL_RGB ||
                       format == SGI_GL_LUMINANCE))) {
            continue;
        }
        switch (env) {
        case SGI_GL_REPLACE:
            rgba[k] = texel[k];
            break;
        case SGI_GL_MODULATE:
            rgba[k] *= texel[k];
            break;
        case SGI_GL_DECAL:
            if (!alpha) {
                rgba[k] = format == SGI_GL_RGB ? texel[k] :
                    rgba[k] * (1 - texel[3]) + texel[k] * texel[3];
            }
            break;
        case SGI_GL_BLEND:
            rgba[k] = alpha && format != SGI_GL_INTENSITY ?
                rgba[k] * texel[k] :
                rgba[k] * (1 - texel[k]) +
                MIN(MAX(r->st->texture_env_color[k], 0), 1) * texel[k];
            break;
        }
    }
}

static bool sgi_soft_texture(const SGISoftDraw *r, SGISoftFrag *frag)
{
    const SGIGLTexImage *im = r->texture;
    const SGIGLDrawState *st = r->st;
    const SGIGLTexLevel *base = r->texture_base;
    uint32_t format = base ? base->base_format : im->base_format;
    double texel[4], s, t, rr = 0, q = frag->texcoord[3];

    if (!(q > 0) || !isfinite(q)) {
        sgi_soft_texture_report(r, 2, "invalid projected coordinates, "
                                "fragment dropped");
        return false;
    }
    s = frag->texcoord[0] / q;
    t = im->one_dimensional ? 0 : frag->texcoord[1] / q;
    if (im->volume || (format == SGI_GL_DEPTH_COMPONENT &&
                       im->shadow_compare)) {
        rr = frag->texcoord[2] / q;
    }
    if (!isfinite(s) || !isfinite(t) || !isfinite(rr) ||
        (format == SGI_GL_DEPTH_COMPONENT && (rr < 0 || rr > 1))) {
        sgi_soft_texture_report(r, 2, "invalid projected coordinates, "
                                "fragment dropped");
        return false;
    }
    if (base) {
        SGISoftTexSelection selected;
        const SGIGLTexLevel *level;
        double other[4];

        if (!sgi_soft_texture_select_memo(r, frag->texture_rho, &selected)) {
            sgi_soft_texture_report(r, 8, "invalid footprint, "
                                          "fragment dropped");
            return false;
        }
        level = sgi_soft_texture_level(im->pyramid, selected.first);
        if (r->texels) {
            r->texels->bypass = selected.lambda > 1 &&
                                !sgi_soft_texture_mipmap(im->min_filter);
        }
        if (sgi_soft_texture_sharpen(selected.filter)) {
            if (!sgi_soft_texture_sharpen_sample(r, selected.filter,
                                                  selected.lambda,
                                                  s, t, rr, texel)) {
                return false;
            }
        } else if (sgi_soft_texture_detail(selected.filter)) {
            if (!sgi_soft_texture_detail_sample(r, selected.filter,
                                                 selected.lambda,
                                                 s, t, rr, texel)) {
                return false;
            }
        } else if (!sgi_soft_texture_sample(r, level, selected.filter,
                                             s, t, rr, texel)) {
            return false;
        }
        if (selected.alpha && selected.first != selected.second) {
            level = sgi_soft_texture_level(im->pyramid, selected.second);
            if (!sgi_soft_texture_sample(r, level, selected.filter,
                                          s, t, rr, other)) {
                return false;
            }
            sgi_soft_texture_mix(texel, other, selected.alpha);
        }
    } else if (!sgi_soft_texture_sample(r, NULL, im->min_filter, s, t, rr,
                                        texel)) {
        return false;
    }
    sgi_soft_texture_apply(r, format, st->texture_env_mode, texel,
                           frag->rgba);
    return true;
}

struct SGIRasterTexture {
    SGIRasterTarget target;
    SGISoftDraw draw;
    unsigned reports;
};

static SGIRasterTextureResult sgi_raster_texture_check_draw(
    SGISoftDraw *draw, bool report)
{
    const SGIGLDrawState *state = draw->st;

    if (!state || !sgi_soft_texture_setup(draw, report)) {
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    if (draw->texture) {
        return SGI_RASTER_TEXTURE_READY;
    }
    if (state->index_bits ||
        (!state->texture_1d && !state->texture_2d && !state->texture_3d)) {
        return SGI_RASTER_TEXTURE_DISABLED;
    }
    return SGI_RASTER_TEXTURE_INCOMPLETE;
}

SGIRasterTextureResult sgi_raster_texture_check(
    const SGIGLDrawState *state, const SGIGLTexImage *texture)
{
    SGIRasterTarget target = { .texture = texture };
    unsigned reports = 0;
    SGISoftDraw draw = {
        .t = &target, .st = state, .texture_reports = &reports,
    };

    return sgi_raster_texture_check_draw(&draw, false);
}

SGIRasterTextureResult sgi_raster_texture_prepare_with_allocator(
    const SGIGLDrawState *state, const SGIGLTexImage *texture,
    SGIRasterTexture **out, void *(*allocate)(size_t))
{
    SGIRasterTarget target = { .texture = texture };
    unsigned reports = 0;
    SGISoftDraw draw = {
        .t = &target, .st = state, .texture_reports = &reports,
    };
    SGIRasterTextureResult result;
    SGIRasterTexture *sampler;

    if (!out || !allocate) {
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    *out = NULL;
    result = sgi_raster_texture_check_draw(&draw, true);
    if (result != SGI_RASTER_TEXTURE_READY) {
        return result;
    }
    sampler = allocate(sizeof(*sampler));
    if (!sampler) {
        return SGI_RASTER_TEXTURE_OOM;
    }
    memset(sampler, 0, sizeof(*sampler));
    sampler->target.texture = texture;
    sampler->draw = draw;
    sampler->draw.t = &sampler->target;
    sampler->draw.texture_reports = &sampler->reports;
    *out = sampler;
    return SGI_RASTER_TEXTURE_READY;
}

SGIRasterTextureResult sgi_raster_texture_prepare(
    const SGIGLDrawState *state, const SGIGLTexImage *texture,
    SGIRasterTexture **out)
{
    return sgi_raster_texture_prepare_with_allocator(
        state, texture, out, g_try_malloc0);
}

void sgi_raster_texture_free(SGIRasterTexture *texture)
{
    g_free(texture);
}

void sgi_raster_texture_quiet(SGIRasterTexture *texture)
{
    texture->reports = UINT_MAX;
}

bool sgi_raster_texture_fragment(SGIRasterTexture *texture,
                                 const double coord[4], double color[4])
{
    SGISoftFrag frag = { 0 };

    if (!texture || !coord || !color) {
        return false;
    }
    memcpy(frag.texcoord, coord, sizeof(frag.texcoord));
    memcpy(frag.rgba, color, sizeof(frag.rgba));
    if (!sgi_soft_texture(&texture->draw, &frag)) {
        return false;
    }
    memcpy(color, frag.rgba, sizeof(frag.rgba));
    return true;
}

bool sgi_gl_texture_apply_pixel(const SGIGLDrawState *state,
                                const SGIGLTexImage *texture,
                                const float coord[4], const float color[4],
                                float out[4])
{
    unsigned reports = 0;
    SGIRasterTarget target = { .texture = texture };
    SGISoftDraw r = {
        .t = &target,
        .st = state,
        .texture_reports = &reports,
    };
    SGISoftFrag frag = { 0 };

    if (!state || !texture || !coord || !color || !out ||
        !sgi_soft_texture_setup(&r, true) || !r.texture) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        frag.texcoord[k] = coord[k];
        frag.rgba[k] = color[k];
    }
    if (!sgi_soft_texture(&r, &frag)) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        out[k] = frag.rgba[k];
    }
    return true;
}

/*
 * Fog (3.10): the fog factor f from the fragment's fog coordinate c by
 * the mode, clamped to [0, 1], blends an RGBA fragment's red, green and
 * blue toward the fog colour, f C + (1 - f) Cf, and adds (1 - f) times
 * the fog index to a colour index. A family's fog_factor, where set, gives
 * f from the window depth and fog coordinate instead. GL_LINEAR with start
 * equal to end divides by zero; the model takes f as 1 before the end and
 * 0 from it, the limit of the equation from either side.
 */
static void sgi_soft_fog(const SGISoftDraw *r, SGISoftFrag *frag, double z)
{
    const SGIGLDrawState *st = r->st;
    double c = frag->fog, f;

    if (r->t->fog_factor) {
        /* A family's own curve replaces the mode; the blend is the same. */
        f = r->t->fog_factor(r->t->opaque, z, c);
    } else if (st->fog_mode == SGI_GL_LINEAR) {
        f = st->fog_end == st->fog_start ? (c < st->fog_end ? 1 : 0) :
            (st->fog_end - c) / (st->fog_end - st->fog_start);
    } else if (st->fog_mode == SGI_GL_EXP2) {
        f = exp(-(st->fog_density * c) * (st->fog_density * c));
    } else {
        f = exp(-st->fog_density * c);
    }
    f = f >= 1 ? 1 : f > 0 ? f : 0;
    if (st->index_bits) {
        frag->index += (1 - f) * st->fog_index;
        return;
    }
    for (int k = 0; k < 3; k++) {
        frag->rgba[k] = f * frag->rgba[k] + (1 - f) * st->fog_color[k];
    }
}

/*
 * Whether a point whose client clip plane values are d lies outside an
 * enabled plane (2.11).
 */
static bool sgi_soft_clipped(const SGIGLDrawState *st, const double d[])
{
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        if ((st->clip_planes & (1u << i)) && !(d[i] >= 0)) {
            return true;
        }
    }
    return false;
}

/* A fragment's colour value from an interpolated or flat index. */
static uint32_t sgi_soft_index(const SGISoftDraw *r, double index)
{
    return sgi_gl_index_value(r->st->index_bits, index);
}

/*
 * One fragment at window pixel (px, py) with window depth z: fogged while
 * fog is enabled, dropped outside [0, 1], outside the window's rectangles
 * and, while the scissor test is enabled, outside the scissor box (4.1.2),
 * in RGBA mode alpha tested (4.1.4), stencil tested and depth tested (4.1.5)
 * when those tests are enabled, the stencil buffer updated by the operation
 * its outcome selects, blended in RGBA mode while blending is enabled or
 * combined by the logical operation while it is enabled, then written under
 * the index or colour mask to each colour target the draw buffers select.
 * Its colour is stored as the rules' packed colour in RGBA mode, dithered
 * after blending and before the logical operation while the draw dithers
 * (4.1.7), and as its index in colour-index mode, which glIndexMask masks
 * (4.2.2).
 */
static bool sgi_soft_owned_at(const SGISoftDraw *r, int px, int py)
{
    int x = r->win.ox + px, y = r->win.oy + py;

    return (r->win.clip->n <= 1 || sgi_clip_contains(r->win.clip, x, y)) &&
           (!r->st->scissor_test ||
            (px >= r->st->scissor[0] &&
             (int64_t)px < (int64_t)r->st->scissor[0] + r->st->scissor[2] &&
             py >= r->st->scissor[1] &&
             (int64_t)py < (int64_t)r->st->scissor[1] + r->st->scissor[3]));
}

/*
 * The colour of a fragment that has passed every test written to the
 * draw's colour targets at plane pixel (x, y): blended with or combined
 * by the logical operation with what is there, dithered and masked, from
 * dest in RGBA mode, which the caller has read where the draw reads it.
 */
static inline QEMU_ALWAYS_INLINE
void sgi_soft_fragment_store(const SGISoftDraw *r, int x, int y,
                             const SGISoftFrag *f,
                             const SGIRasterColour *dest)
{
    const SGIRasterTarget *t = r->t;
    uint32_t colour;

    if (!r->st->index_bits && t->store_rgba) {
        for (unsigned i = 0; r->rgba_mask && i < r->ntargets; i++) {
            SGIRasterColour out = sgi_soft_rgba_result(r, x, y, f,
                                                       &dest[i]);

            t->store_rgba(t->opaque, r->targets[i], x, y, &out);
        }
        return;
    }
    /*
     * Dithering follows blending (4.1.7), so a colour to be blended is
     * rounded and the blend's result dithered.
     */
    colour = r->st->index_bits ? sgi_soft_index(r, f->index) :
             r->blend ? sgi_soft_rgb(r->rules, f->rgba) :
                        sgi_soft_store_rgb(r, x, y, f->rgba);
    for (uint32_t i = 0; i < r->ntargets; i++) {
        uint32_t out = colour;

        if (r->logic_op) {
            uint32_t bits = r->st->index_bits ? r->st->index_bits :
                                                3 * r->rules->color_bits;

            out = sgi_soft_logic_op(r->st->logic_op_mode, colour,
                                    t->load_colour(t->opaque, r->targets[i],
                                                   x, y)) &
                  (bits >= 32 ? UINT32_MAX : (1u << bits) - 1);
        }
        if (r->st->index_bits) {
            t->store_index(t->opaque, r->targets[i], x, y, out,
                           r->st->index_mask);
            continue;
        }
        if (!r->colour_mask) {
            continue;
        }
        if (r->blend) {
            out = sgi_soft_blend(r, x, y, colour,
                                 sgi_soft_alpha(r->rules, f->rgba[3]) /
                                 (double)sgi_soft_alpha_max(r->rules),
                                 t->load_colour(t->opaque, r->targets[i],
                                                x, y));
        }
        if (r->colour_mask != sgi_soft_colour_bits(r->rules)) {
            out = (out & r->colour_mask) |
                  (t->load_colour(t->opaque, r->targets[i], x, y) &
                   ~r->colour_mask);
        }
        t->store_colour(t->opaque, r->targets[i], x, y, out);
    }
}

static void sgi_soft_fragment_at(const SGISoftDraw *r, int px, int py,
                                 double z, const SGISoftFrag *frag,
                                 double coverage, bool antialias)
{
    const SGIRasterTarget *t = r->t;
    SGISoftFrag fogged = *frag;
    const SGISoftFrag *f = &fogged;
    int x = r->win.ox + px, y = r->win.oy + py;
    SGIRasterColour dest[SGI_GL_MAX_TARGETS] = { 0 };

    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    if (r->texture) {
        if (frag->fixed) {
            uint32_t format = r->texture_base ? r->texture_base->base_format :
                              r->texture->base_format;

            sgi_soft_texture_apply(r, format, r->st->texture_env_mode,
                                   fogged.texel, fogged.rgba);
        } else if (!sgi_soft_texture(r, &fogged)) {
            return;
        }
    }
    if (r->st->fog) {
        sgi_soft_fog(r, &fogged, z);
    }
    fogged.rgba[3] *= coverage;
    if (antialias && r->st->index_bits) {
        uint32_t bits = MIN(4, r->st->index_bits);
        uint32_t mask = (1u << bits) - 1;
        uint32_t index = sgi_soft_index(r, fogged.index);
        uint32_t value = MIN((uint32_t)floor(coverage * (1u << bits)), mask);

        fogged.index = (index & ~mask) | value;
    }
    if (!sgi_soft_owned_at(r, px, py)) {
        return;
    }
    /*
     * Incoming alpha and reference use at least two bits and at least the
     * stored alpha width, independently of the colour mask (4.1.4, 2.13.9).
     */
    if (r->st->alpha_test && !r->st->index_bits &&
        !sgi_raster_alpha_test(r->rules, r->st->alpha_func,
                               f->rgba[3], r->st->alpha_ref)) {
        return;
    }
    if (!r->st->index_bits && t->store_rgba && r->rgba_read &&
        !sgi_soft_rgba_load(r->rules, t, r->targets, r->ntargets, x, y,
                            dest, r->rgba_reported)) {
        return;
    }
    if (!sgi_raster_depth_stencil(r->rules, r->st, t, x, y, z)) {
        return;
    }
    sgi_soft_fragment_store(r, x, y, f, dest);
}

/*
 * The depth range [n, f] normalized depth is mapped through (2.10.1): the
 * draw state's, or [0, 1] under a window matrix, whose depth is already
 * window depth.
 */
static void sgi_soft_depth_range(const SGIGLDrawState *st, double *n,
                                 double *f)
{
    *n = st->window_matrix_given ? 0 : st->depth_range[0];
    *f = st->window_matrix_given ? 1 : st->depth_range[1];
}

bool sgi_raster_window_depth(const SGIGLDrawState *st, double z,
                              double offset, double *window)
{
    double n, f, zw;

    if (!(z >= 0 && z <= 1)) {
        return false;
    }
    sgi_soft_depth_range(st, &n, &f);
    zw = n + (f - n) * z + offset;
    *window = zw >= 1 ? 1 : zw > 0 ? zw : 0;
    return true;
}

/*
 * The window pixels a primitive's fragments may fall on, inclusive: the
 * bounds of those the window owns, within the viewport unless a window
 * matrix replaces it. False when there are none.
 */
static bool sgi_soft_bounds(const SGISoftDraw *r, int *x0, int *y0, int *x1,
                            int *y1)
{
    const SGIGLDrawState *st = r->st;
    const SGISoftWindow *win = &r->win;
    int vx = st->viewport[0], vy = st->viewport[1];
    int vw = st->viewport[2], vh = st->viewport[3];

    *x0 = win->x0 - win->ox;
    *y0 = win->y0 - win->oy;
    *x1 = win->x1 - win->ox;
    *y1 = win->y1 - win->oy;
    if (st->window_matrix_given) {
        return true;
    }
    if (vw <= 0 || vh <= 0) {
        return false;
    }
    *x0 = MAX(*x0, vx);
    *y0 = MAX(*y0, vy);
    *x1 = MIN(*x1, vx + vw - 1);
    *y1 = MIN(*y1, vy + vh - 1);
    return true;
}

/*
 * Whether a polygon fragment at (px, py) is drawn under the polygon
 * stipple, and in *background whether it takes the target's opaque
 * stipple background (SGIRasterTarget's stipple_opaque).
 */
static bool sgi_soft_stipple_pass(const SGISoftDraw *r, int px, int py,
                                  bool *background)
{
    *background = false;
    if (!r->polygon_stipple ||
        (r->polygon_stipple[(uint32_t)py & 31] &
         (1u << ((uint32_t)px & 31)))) {
        return true;
    }
    *background = r->t->stipple_opaque;
    return *background;
}

static void sgi_soft_stipple_background(const SGISoftDraw *r,
                                        SGISoftFrag *frag)
{
    for (int k = 0; k < 4; k++) {
        frag->rgba[k] = r->t->stipple_background[k];
    }
    frag->index = r->t->stipple_background_index;
}

/*
 * A primitive's fragment at normalized depth z, (zd + 1) / 2 for the
 * normalized device z zd: dropped outside [0, 1], the view volume's near
 * and far planes, which clipping has already cut the primitive to, and
 * otherwise mapped to window depth n + (f - n) z through the depth range
 * [n, f] (OpenGL 1.2.1, 2.10.1). A polygon's offset is then added to the
 * window depth, and the sum clamped to [0, 1] (3.5.5).
 */
static void sgi_soft_fragment_coverage(const SGISoftDraw *r, int px, int py,
                                       double z, double offset,
                                       double depth_w,
                                       const SGISoftFrag *frag,
                                       double coverage, bool antialias)
{
    double zw;

    if (r->rules->depth_row_given) {
        z = sgi_soft_row_depth(r->rules, z, offset, depth_w);
        offset = 0;
    }
    if (!(coverage > 0.0) ||
        !sgi_raster_window_depth(r->st, z, offset, &zw)) {
        return;
    }
    sgi_soft_fragment_at(r, px, py, zw, frag, coverage, antialias);
}

static void sgi_soft_fragment(const SGISoftDraw *r, int px, int py, double z,
                              double offset, double depth_w,
                              const SGISoftFrag *frag)
{
    sgi_soft_fragment_coverage(r, px, py, z, offset, depth_w, frag, 1, false);
}

/*
 * A triangle's polygon offset (3.5.5), o = m factor + r units, where m is
 * the larger magnitude of its window depth's slopes in x and y, which the
 * specification allows in place of their length, and r the rules' one
 * depth step; EXT_polygon_offset's bias adds in window depth. Boundary modes
 * share this slope calculation and select their own offset enable.
 */
static double sgi_raster_depth_slope(const SGIGLDrawState *st,
                                      const double window[3][3])
{
    double x1, y1, z1, x2, y2, z2, det, n, f;

    sgi_soft_depth_range(st, &n, &f);
    x1 = window[1][0] - window[0][0], y1 = window[1][1] - window[0][1];
    x2 = window[2][0] - window[0][0], y2 = window[2][1] - window[0][1];
    z1 = window[1][2] - window[0][2], z2 = window[2][2] - window[0][2];
    det = x1 * y2 - x2 * y1;
    /* Reviewed model choice for an entirely degenerate boundary: m = 0. */
    if (det == 0) {
        return 0;
    }
    return MAX(fabs(z1 * y2 - z2 * y1), fabs(x1 * z2 - x2 * z1)) /
           fabs(det) * fabs(f - n);
}

static void sgi_soft_window_vertices(const SGISoftDraw *r,
                                      const SGISoftVertex v[3],
                                      double window[3][3])
{
    const double sub = 1 << r->rules->subpixel_bits;

    for (unsigned i = 0; i < 3; i++) {
        window[i][0] = v[i].x / sub;
        window[i][1] = v[i].y / sub;
        window[i][2] = v[i].z;
    }
}

static double sgi_soft_slope(const SGISoftDraw *r, const SGISoftVertex v[3])
{
    double window[3][3];

    sgi_soft_window_vertices(r, v, window);
    return sgi_raster_depth_slope(r->st, window);
}

/*
 * A primitive's offset from its slope: OpenGL's factor and units terms and
 * EXT_polygon_offset's bias, or under the eye-linear float just the bias,
 * a count of steps of the stored word with no slope term.
 */
static double sgi_soft_offset_value(const SGIRasterRules *rules,
                                    const SGIGLDrawState *st, double slope)
{
    if (rules->depth_eye_float) {
        return st->offset_bias;
    }
    return slope * st->offset_factor +
           st->offset_units / sgi_soft_depth_max(rules) + st->offset_bias;
}

double sgi_raster_polygon_offset(const SGIRasterRules *rules,
                                  const SGIGLDrawState *st,
                                  const double window[3][3])
{
    if (!(st->polygon_offset & SGI_GL_OFFSET_FILL)) {
        return 0;
    }
    return sgi_soft_offset_value(rules, st,
                                 rules->depth_eye_float ? 0 :
                                 sgi_raster_depth_slope(st, window));
}

static double sgi_soft_offset(const SGISoftDraw *r, const SGISoftVertex v[3])
{
    double window[3][3];

    sgi_soft_window_vertices(r, v, window);
    return sgi_raster_polygon_offset(r->rules, r->st, window);
}

/*
 * The six planes of the view volume (OpenGL 1.2.1, 2.11), -w <= x, y,
 * z <= w, as each one's value at a point, which is >= 0 inside it.
 */
#define SGI_VOLUME_PLANES 6

static double sgi_volume_plane(const SGIVolumeVertex *v, int plane)
{
    double c = v->clip[plane / 2];

    return plane & 1 ? v->clip[3] - c : v->clip[3] + c;
}

bool sgi_volume_contains(const SGIVolumeVertex *v)
{
    for (int i = 0; i < SGI_VOLUME_PLANES; i++) {
        if (!(sgi_volume_plane(v, i) >= 0)) {
            return false;
        }
    }
    return true;
}

/*
 * The point at t along from in to out, in clip and eye coordinates and
 * every float attribute (2.13.8); the material index and edge flag are
 * bit patterns and stay in's.
 */
static void sgi_volume_lerp(const SGIVolumeVertex *in,
                            const SGIVolumeVertex *out, double t,
                            SGIVolumeVertex *r)
{
    for (int k = 0; k < 4; k++) {
        r->clip[k] = in->clip[k] + t * (out->clip[k] - in->clip[k]);
        r->eye[k] = in->eye[k] + t * (out->eye[k] - in->eye[k]);
    }
    for (int k = 0; k < SGI_GL_VERTEX_FLOATS; k++) {
        r->attr[k] = k == SGI_GL_V_MATERIAL || k == SGI_GL_V_EDGE ?
                     in->attr[k] :
                     in->attr[k] + t * (out->attr[k] - in->attr[k]);
    }
}

/*
 * Where the edge from in, inside a plane with value din, to out, outside
 * it with value dout, meets the plane. It is always found from the inside
 * end, so that two primitives sharing the edge make the same vertex.
 */
static void sgi_volume_cut(const SGIVolumeVertex *in, double din,
                           const SGIVolumeVertex *out, double dout,
                           SGIVolumeVertex *r)
{
    sgi_volume_lerp(in, out, din / (din - dout), r);
}

/*
 * Sutherland and Hodgman's clipping, one plane at a time: each edge
 * keeps its inside end and adds the point where it crosses the plane.
 * A value that is not a number is outside.
 */
unsigned sgi_volume_clip_triangle(const SGIVolumeVertex in[3],
                                  SGIVolumeVertex out[SGI_VOLUME_TRIANGLE_MAX])
{
    SGIVolumeVertex buf[SGI_VOLUME_TRIANGLE_MAX];
    SGIVolumeVertex *src = out, *dst = buf, *t;
    unsigned n = 3;

    memcpy(out, in, 3 * sizeof(in[0]));
    if (sgi_volume_contains(&in[0]) && sgi_volume_contains(&in[1]) &&
        sgi_volume_contains(&in[2])) {
        return 3;
    }
    /* Six planes swap the buffers six times, ending in out. */
    for (int p = 0; p < SGI_VOLUME_PLANES; p++) {
        unsigned m = 0;

        for (unsigned i = 0; i < n; i++) {
            const SGIVolumeVertex *a = &src[i], *b = &src[(i + 1) % n];
            double da = sgi_volume_plane(a, p), db = sgi_volume_plane(b, p);

            if (da >= 0) {
                dst[m++] = *a;
                if (!(db >= 0)) {
                    sgi_volume_cut(a, da, b, db, &dst[m++]);
                }
            } else if (db >= 0) {
                sgi_volume_cut(b, db, a, da, &dst[m++]);
            }
        }
        n = m;
        t = src, src = dst, dst = t;
        if (n < 3) {
            return 0;
        }
    }
    return n;
}

/*
 * Liang and Barsky's clipping: the segment keeps the part every plane
 * allows. Each end that a plane bounds moves to where the segment meets
 * that plane, found from the other end, inside it, as a triangle's cut
 * is: a fraction sa of the way from b to a, and tb from a to b.
 */
bool sgi_volume_clip_segment(SGIVolumeVertex *a, SGIVolumeVertex *b)
{
    SGIVolumeVertex na = *a, nb = *b;
    double sa = 1, tb = 1;

    for (int p = 0; p < SGI_VOLUME_PLANES; p++) {
        double da = sgi_volume_plane(a, p), db = sgi_volume_plane(b, p);

        if (da >= 0 && db >= 0) {
            continue;
        }
        if (!(da >= 0) && !(db >= 0)) {
            return false;
        }
        if (da >= 0) {
            tb = MIN(tb, da / (da - db));
        } else {
            sa = MIN(sa, db / (db - da));
        }
    }
    if (!(sa + tb >= 1)) {
        return false;
    }
    if (sa < 1) {
        sgi_volume_lerp(b, a, sa, &na);
    }
    if (tb < 1) {
        sgi_volume_lerp(a, b, tb, &nb);
    }
    *a = na;
    *b = nb;
    return true;
}

/*
 * One vertex to clip coordinates (OpenGL 1.2.1, 2.10): object to eye by
 * the modelview and eye to clip by the projection, or by the window
 * matrix as sgi_gl_eye_to_clip() takes it.
 */
void sgi_volume_from_draw_vertex(const SGIGLDrawState *st, const float *v,
                                 SGIVolumeVertex *out)
{
    for (int row = 0; row < 4; row++) {
        out->eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            out->eye[row] += st->modelview[k * 4 + row] *
                             (double)v[SGI_GL_V_POSITION + k];
        }
    }
    sgi_gl_eye_to_clip(st, out->eye, out->clip);
    memcpy(out->attr, v, sizeof(out->attr));
}

/*
 * Map one clipped vertex (2.10): the divide by w and the viewport map,
 * with depth left normalized to [0, 1]; the depth range maps it per
 * fragment. Under a window matrix the position is the matrix's and the
 * depth its window depth. Clipping has put the vertex in the view volume,
 * so its normalized depth is clamped to [0, 1] against rounding. Under the
 * rules' depth row the depth is the row's over w instead, unclamped, as
 * clipping does not bound it. The colour is the front colour; a back-facing
 * triangle takes its back colour later.
 * A vertex with w <= 0 is in the volume only at the clip-space origin,
 * where the divide has no answer, and returns false, as does one mapped
 * beyond the rasterizer's range.
 */
static bool sgi_soft_project(const SGISoftDraw *r, const SGIVolumeVertex *cv,
                             SGISoftVertex *out)
{
    const SGIGLDrawState *st = r->st;
    const double *clip = cv->clip, *eye = cv->eye;
    double win[3], xw, yw, z;

    if (!sgi_soft_texture_vertex_valid(r, cv)) {
        return false;
    }
    if (!(clip[3] > 0)) {
        if (!sgi_soft_band_quiet) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: clipped vertex at w %g has no "
                          "window position, primitive dropped\n", clip[3]);
        }
        return false;
    }
    sgi_gl_clip_to_window(st, clip, win);
    xw = win[0];
    yw = win[1];
    if (!(fabs(xw) < 65536.0 && fabs(yw) < 65536.0)) {
        if (!sgi_soft_band_quiet) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: vertex outside the "
                          "rasterizer's range, primitive dropped\n");
        }
        return false;
    }
    for (int k = 0; k < 4; k++) {
        out->texcoord[k] = cv->attr[SGI_GL_V_TEXCOORD + k];
    }
    out->x = sgi_soft_round(r->rules, xw * (1 << r->rules->subpixel_bits));
    out->y = sgi_soft_round(r->rules, yw * (1 << r->rules->subpixel_bits));
    if (!r->rules->depth_row_given) {
        z = win[2];
        out->z = z >= 1 ? 1 : z > 0 ? z : 0;
    } else {
        const double *d = r->rules->depth_row;

        /* The head's depth over w, which a window line interpolates. */
        out->z = (d[0] * eye[0] + d[1] * eye[1] + d[2] * eye[2] +
                  d[3] * eye[3]) / clip[3];
    }
    out->inv_w = 1.0 / clip[3];
    out->depth_w = out->inv_w;
    if (r->rules->depth_divisor_given) {
        const double *d = r->rules->depth_divisor;

        out->depth_w = (d[0] * eye[0] + d[1] * eye[1] + d[2] * eye[2] +
                        d[3] * eye[3]) / clip[3];
    }
    /*
     * The fog coordinate approximates the eye distance by |z|, which
     * 3.10 allows; it is affine in eye space, so interpolating it as a
     * vertex attribute gives it exactly at each fragment.
     */
    out->fog = fabs(eye[3] != 0 ? eye[2] / eye[3] : eye[2]);
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        const float *p = st->clip_plane[i];

        out->clip[i] = p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] +
                       p[3] * eye[3];
    }
    for (int k = 0; k < 4; k++) {
        out->rgba[k] = MIN(MAX(cv->attr[SGI_GL_V_COLOR + k], 0.0f), 1.0f);
    }
    out->index = cv->attr[SGI_GL_V_INDEX];
    return true;
}

typedef struct SGISoftTexPlane {
    /* S/W, T/W, Q/W and, for volumes, R/W. */
    double value[4], dx[4], dy[4];
    double x, y;
} SGISoftTexPlane;

/*
 * The footprint of a pixel's 2x2 quad, in texels: the largest absolute s or
 * t difference along the quad's two diagonals, pixel 1 to pixel 4 and pixel
 * 2 to pixel 3, as the Odyssey board's texture unit takes its level of
 * detail (docs/datasheets/odyssey.md, "Mipmap level of detail"). The
 * quad's four pixels share the value; its base-2 logarithm is the level of
 * detail, as for a per-pixel footprint. ns and nt scale s and t to texels.
 *
 * MODEL CHOICE: the datasheet leaves two things open and this takes them
 * as follows. Quads align to even pixel columns and rows of the window
 * the rasterizer draws in; whether the board aligns to the window or the
 * screen is unknown. The four s and t values come from the triangle's
 * planes at each of the quad's pixel centres, so a pixel the triangle does
 * not cover contributes its extrapolated value; how the board treats such
 * pixels is unknown. False where a pixel centre has no positive Q/W or a
 * value is not finite: the caller keeps the pixel's own footprint.
 */
static bool sgi_soft_texture_quad_rho(const SGISoftTexPlane *g, int px,
                                      int py, double ns, double nt,
                                      double *rho)
{
    const int x0 = px & ~1, y0 = py & ~1;
    double s[4], t[4];

    for (unsigned i = 0; i < 4; i++) {
        double x = x0 + (i & 1) + 0.5 - g->x;
        double y = y0 + (i >> 1) + 0.5 - g->y;
        double q = g->value[2] + x * g->dx[2] + y * g->dy[2];

        if (!(q > 0)) {
            return false;
        }
        s[i] = ns * (g->value[0] + x * g->dx[0] + y * g->dy[0]) / q;
        t[i] = nt * (g->value[1] + x * g->dx[1] + y * g->dy[1]) / q;
    }
    *rho = MAX(MAX(fabs(s[3] - s[0]), fabs(t[3] - t[0])),
               MAX(fabs(s[2] - s[1]), fabs(t[2] - t[1])));
    return isfinite(*rho);
}

/*
 * Fixed-point texturing for filled triangles.
 *
 * Approximated: no source gives any family's sub-texel, filter-weight or
 * level-of-detail widths, and the double arithmetic of sgi_soft_texture()
 * is no closer to a board's than another choice. Where the state allows,
 * a filled triangle samples instead with:
 *  - each fragment's s and t from the triangle's S/W, T/W and Q/W planes,
 *    divided in double, so a far, many-times repeated floor keeps its
 *    texel positions (a 32-bit fixed-point numerator does not);
 *  - texel positions in 1/256 of a texel: bilinear weights of 8 bits;
 *  - lambda, the level of detail, in 1/256, from the squared footprint
 *    through a 10-bit table of log2, with no square root; within a few
 *    steps of a choice between filters or nearest levels it is computed
 *    exactly instead, rounded up, so the choice is the double path's;
 *  - a blend between two levels by an 8-bit fraction;
 *  - texels as 8-bit components, filtered in integers with no rounding
 *    until the texture environment takes the result in double.
 * Coverage, depth, stencil and the pixels written are those of the
 * double path. Colour differs from it within a bound measured on the
 * comparison corpus; tests/unit/test-sgi-gl.c checks that bound and
 * pins each width.
 *
 * The state it takes: a 2D image or pyramid of base format alpha,
 * luminance, luminance-alpha, intensity, RGB or RGBA, read through the
 * packed reader or through component readers 1, 2, 4 or 8 bits wide,
 * which convert to 8 bits exactly; levels of at most 8192 texels a side
 * and 2^22 texels; NEAREST, LINEAR or a mipmap minification filter with
 * NEAREST or LINEAR magnification; any wrap, a border image only under
 * REPEAT or CLAMP_TO_EDGE, which never reach it; a target that does not
 * draw mixed frames (SGIRasterTarget.mixed_frame); no alpha test, whose
 * outcome would move with the texel's alpha; and no logical operation,
 * which turns a difference in the lowest bit into one in any. Anything
 * else, and a
 * fragment the fixed sampler cannot take (a coordinate beyond 2^30, Q/W
 * not positive, a failed read), goes to sgi_soft_texture(), so it is
 * dropped and reported as before.
 */
#define SGI_SOFT_FIXED_MAX_SIDE 8192u
#define SGI_SOFT_FIXED_MAX_TEXELS (1u << 22)
/* Lambda's bounds, beyond any level count, in 1/256. */
#define SGI_SOFT_FIXED_LAMBDA_MAX (1 << 24)

static SGIRasterTexturePath sgi_soft_texture_path;

void sgi_raster_set_texture_path(SGIRasterTexturePath path)
{
    sgi_soft_texture_path = path;
}

typedef struct SGISoftFixedLevel {
    const SGIGLTexLevel *level;     /* NULL: the image's level-zero fields */
    uint32_t width, height;
    uint32_t width_shift, height_shift;
    bool raw;                       /* read through the component reader */
} SGISoftFixedLevel;

typedef struct SGISoftTexSpan SGISoftTexSpan;
typedef int SGISoftTexSpanFn(const SGISoftDraw *r, const SGISoftTexSpan *sp,
                             int py, int px0, int px1, int64_t e1,
                             int64_t e2);

typedef struct SGISoftFixedTex {
    SGISoftFixedLevel level[SGI_SOFT_FIXED_LEVELS];
    /* The row loop for textured triangles, or NULL for the general one. */
    SGISoftTexSpanFn *span;
    /* Every texel read before drawing, by level (sgi_soft_fixed_prefill()). */
    bool prefilled;
    const uint64_t *plain[SGI_SOFT_FIXED_LEVELS];
    uint32_t range;                 /* the last level, relative to base */
    uint32_t format;
    uint32_t min, mag, wrap_s, wrap_t;
    uint64_t border;                /* the border colour, in lanes */
    /* CLAMP's upper coordinate on each axis (SGIGLTexImage.max_clamp). */
    double upper_s, upper_t;
    /* Whether the footprint chooses the filter or level at all. */
    bool lod;
    /* Whether it chooses between filters or nearest levels. */
    bool choice;
    int32_t min_lod, max_lod;       /* in 1/256 */
    int32_t mag_below;              /* lambda at or below it magnifies */
    double ns, nt;                  /* the footprint's per-axis scales */
    /* Lambda per 2x2 quad (SGIGLTexImage.quad_lod), not per pixel. */
    bool quad;
} SGISoftFixedTex;

/*
 * A packed RRGGBBAA texel's components each in the low byte of a 16-bit
 * lane, red highest: a component times a weight of at most 256 fits its
 * lane, so one product weights all four.
 */
static inline uint64_t sgi_soft_fixed_lanes(uint32_t rgba)
{
    uint64_t t = (uint64_t)(rgba & 0xffff0000u) << 16 | (rgba & 0xffff);

    return (t & 0x0000ff000000ff00ull) << 8 | (t & 0x000000ff000000ffull);
}

/* An 8-bit image component from one of a component reader's width. */
static uint32_t sgi_soft_fixed_component(uint32_t value, uint8_t bits)
{
    return value * (255 / ((1u << bits) - 1));
}

/*
 * A packed RRGGBBAA texel with the components the base format lacks
 * filled in, as sgi_soft_texture_components() fills them.
 */
static uint32_t sgi_soft_fixed_expand(uint32_t format, uint32_t rgba)
{
    uint32_t r = rgba >> 24;

    switch (format) {
    case SGI_GL_ALPHA:
        return 0xffffff00u | (rgba & 0xff);
    case SGI_GL_LUMINANCE:
        return r * 0x01010100u | 0xff;
    case SGI_GL_LUMINANCE_ALPHA:
        return r * 0x01010100u | (rgba & 0xff);
    case SGI_GL_INTENSITY:
        return r * 0x01010101u;
    case SGI_GL_RGB:
        return rgba | 0xff;
    default:
        return rgba;
    }
}

static bool sgi_soft_fixed_widths(uint32_t format, const uint8_t bits[4])
{
    uint8_t mask = sgi_soft_texture_component_mask(format);

    for (unsigned k = 0; k < 4; k++) {
        if ((mask & (1u << k)) && bits[k] != 1 && bits[k] != 2 &&
            bits[k] != 4 && bits[k] != 8) {
            return false;
        }
    }
    return true;
}

static uint32_t sgi_soft_fixed_unit(double c)
{
    return (uint32_t)(MIN(MAX(c, 0.0), 1.0) * 255 + 0.5);
}

/*
 * Whether draw r's filled triangles sample its texture in fixed point,
 * and if so f made ready for them; the tests sgi_soft_texture_setup()
 * has already made are not repeated.
 */
static bool sgi_soft_fixed_setup(const SGISoftDraw *r, SGISoftFixedTex *f)
{
    const SGIGLTexImage *im = r->texture;
    const SGIGLTexLevel *base = r->texture_base;
    const SGIGLTexPyramid *p = im ? im->pyramid : NULL;
    bool mipmap, edge_only;
    double border[4];

    if (sgi_soft_texture_path == SGI_RASTER_TEXTURE_DOUBLE || !im ||
        !r->texels || r->t->mixed_frame || im->volume ||
        im->one_dimensional || r->st->index_bits || r->st->alpha_test ||
        r->logic_op) {
        return false;
    }
    memset(f, 0, sizeof(*f));
    f->format = base ? base->base_format : im->base_format;
    f->min = im->min_filter;
    f->mag = im->mag_filter;
    f->wrap_s = im->wrap_s;
    f->wrap_t = im->wrap_t;
    f->upper_s = im->max_clamp ? im->max_clamp_s : 1;
    f->upper_t = im->max_clamp ? im->max_clamp_t : 1;
    mipmap = sgi_soft_texture_mipmap(f->min);
    if (f->format == SGI_GL_DEPTH_COMPONENT ||
        (f->mag != SGI_GL_NEAREST && f->mag != SGI_GL_LINEAR) ||
        (f->min != SGI_GL_NEAREST && f->min != SGI_GL_LINEAR && !mipmap)) {
        return false;
    }
    edge_only = (f->wrap_s == SGI_GL_REPEAT ||
                 f->wrap_s == SGI_GL_CLAMP_TO_EDGE) &&
                (f->wrap_t == SGI_GL_REPEAT ||
                 f->wrap_t == SGI_GL_CLAMP_TO_EDGE);
    f->range = base ? r->texture_last - p->base_level : 0;
    if (f->range >= SGI_SOFT_FIXED_LEVELS) {
        return false;
    }
    for (uint32_t i = 0; i <= f->range; i++) {
        const SGIGLTexLevel *l = base ?
            sgi_soft_texture_level(p, p->base_level + i) : NULL;
        SGISoftFixedLevel *fl = &f->level[i];
        uint32_t format = l ? l->base_format : im->base_format;
        bool raw = l ? !!l->read_texel_components :
                       !!im->read_texel_components;

        fl->level = l;
        fl->width = l ? l->width : im->width;
        fl->height = l ? l->height : im->height;
        fl->width_shift = ctz32(fl->width);
        fl->height_shift = ctz32(fl->height);
        fl->raw = raw;
        if (!l && i) {
            return false;
        }
        if (fl->width > SGI_SOFT_FIXED_MAX_SIDE ||
            fl->height > SGI_SOFT_FIXED_MAX_SIDE ||
            (uint64_t)fl->width * fl->height > SGI_SOFT_FIXED_MAX_TEXELS ||
            !is_power_of_2(fl->width) || !is_power_of_2(fl->height) ||
            format != f->format ||
            ((l ? l->border : im->border) && !edge_only) ||
            (raw ? !sgi_soft_fixed_widths(format, l ? l->component_bits :
                                                  im->component_bits) :
                   !(l ? !!l->read_texel : !!im->read_texel))) {
            return false;
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        border[k] = MIN(MAX(im->border_color[k], 0), 1);
    }
    sgi_soft_texture_components(f->format, border);
    f->border = sgi_soft_fixed_lanes(sgi_soft_fixed_unit(border[0]) << 24 |
                                     sgi_soft_fixed_unit(border[1]) << 16 |
                                     sgi_soft_fixed_unit(border[2]) << 8 |
                                     sgi_soft_fixed_unit(border[3]));
    /* An image without a pyramid has one filter and no footprint. */
    f->lod = base && (mipmap || f->min != f->mag);
    f->choice = f->lod && !(f->min == SGI_GL_LINEAR_MIPMAP_LINEAR &&
                            f->mag == SGI_GL_LINEAR);
    if (base) {
        /* Rounded up, as an exact lambda is (sgi_soft_fixed_decide()). */
        f->min_lod = ceil(MIN(MAX(p->min_lod * 256,
                                  -SGI_SOFT_FIXED_LAMBDA_MAX),
                              SGI_SOFT_FIXED_LAMBDA_MAX));
        f->max_lod = ceil(MIN(MAX(p->max_lod * 256,
                                  -SGI_SOFT_FIXED_LAMBDA_MAX),
                              SGI_SOFT_FIXED_LAMBDA_MAX));
        f->ns = base->width * sgi_soft_texture_lod_scale(im->lod_scale_s);
        f->nt = base->height * sgi_soft_texture_lod_scale(im->lod_scale_t);
        f->quad = im->quad_lod && !im->volume;
    }
    /* sgi_soft_texture_select()'s c, for these filters. */
    f->mag_below = f->mag == SGI_GL_LINEAR &&
                   (f->min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
                    f->min == SGI_GL_NEAREST_MIPMAP_LINEAR) ? 128 : 0;
    return true;
}

/*
 * Make this thread's texel store ready for draw r's levels: false, and
 * the draw samples in double, where it cannot be allocated.
 */
static bool sgi_soft_fixed_begin(const SGISoftDraw *r)
{
    const SGISoftFixedTex *f = r->fixed;

    for (uint32_t i = 0; i <= f->range; i++) {
        SGISoftFixedStore *store = &r->texels->fixed[i];
        size_t size = (size_t)f->level[i].width * f->level[i].height;

        if (store->size < size) {
            g_free(store->cell);
            store->size = 0;
            store->cell = g_try_new0(uint64_t, size);
            if (!store->cell) {
                return false;
            }
            store->size = size;
        }
    }
    return true;
}

/*
 * Interior texel (x, y) of relative level i of draw r's image as a packed
 * texel, components expanded; false where the read fails or returns a
 * component beyond its width.
 */
static bool sgi_soft_fixed_read(const SGISoftDraw *r,
                                const SGISoftFixedTex *f, uint32_t i,
                                uint32_t x, uint32_t y, uint32_t *texel)
{
    const SGISoftFixedLevel *fl = &f->level[i];
    const SGIGLTexLevel *l = fl->level;
    const SGIGLTexImage *im = r->texture;
    uint32_t rgba = 0;
    bool read;

    if (fl->raw) {
        const uint8_t *bits = l ? l->component_bits : im->component_bits;
        uint8_t mask = sgi_soft_texture_component_mask(f->format);
        uint16_t c[4] = { 0 };

        read = l ? l->read_texel_components(l->opaque, l->level, x, y, c) :
                   im->read_texel_components(im->opaque, x, y, c);
        for (unsigned k = 0; read && k < 4; k++) {
            if (mask & (1u << k)) {
                read = c[k] < (1u << bits[k]);
                rgba |= sgi_soft_fixed_component(c[k], bits[k]) <<
                        (24 - 8 * k);
            }
        }
    } else {
        read = l ? l->read_texel(l->opaque, l->level, x, y, &rgba) :
                   im->read_texel(im->opaque, x, y, &rgba);
    }
    *texel = sgi_soft_fixed_expand(f->format, rgba);
    return read;
}

/*
 * Read interior texel (x, y) of relative level i into its cell, and
 * return the cell, tagged failed where the read failed.
 */
static uint64_t __attribute__((noinline))
sgi_soft_fixed_fill(const SGISoftDraw *r, uint32_t i, uint32_t x,
                    uint32_t y, uint64_t *cell)
{
    uint32_t tag = r->texels->tag, texel;
    bool read = sgi_soft_fixed_read(r, r->fixed, i, x, y, &texel);

    *cell = (uint64_t)(read ? tag : tag | SGI_SOFT_FIXED_FAILED) << 32 | texel;
    return *cell;
}

/*
 * The relative levels a draw's fragments can sample, first to last: the
 * level clamp keeps lambda within [min_lod, max_lod], and a lambda above
 * a half never magnifies, so no level outside reaches the sampler.
 */
static void sgi_soft_fixed_reach(const SGISoftFixedTex *f, uint32_t *first,
                                 uint32_t *last)
{
    *first = 0;
    *last = f->range;
    if (!f->lod) {
        return;
    }
    if (f->min_lod > 128) {
        *first = MIN(f->min_lod >> 8, (int32_t)f->range);
    }
    *last = f->max_lod <= 0 ? 0 :
            MIN((f->max_lod + 255) >> 8, (int32_t)f->range);
    *first = MIN(*first, *last);
}

/*
 * Read every texel of the levels draw r can sample into this thread's
 * plain arrays, which f then shares with the draw's bands: for a large
 * draw that covers about as many pixels as those levels have texels,
 * which then reads each once rather than once in each band, and taps
 * them without a tag. False, and the draw reads texels as it reaches them,
 * where any read fails or the arrays cannot be allocated.
 */
static bool sgi_soft_fixed_prefill(const SGISoftDraw *r, SGISoftFixedTex *f)
{
    uint32_t first, last;

    sgi_soft_fixed_reach(f, &first, &last);
    for (uint32_t i = first; i <= last; i++) {
        SGISoftFixedStore *store = &r->texels->fixed[i];
        const SGISoftFixedLevel *fl = &f->level[i];
        size_t size = (size_t)fl->width * fl->height;

        if (store->plain_size < size) {
            g_free(store->plain);
            store->plain_size = 0;
            store->plain = g_try_new(uint64_t, size);
            if (!store->plain) {
                return false;
            }
            store->plain_size = size;
        }
        for (uint32_t y = 0; y < fl->height; y++) {
            for (uint32_t x = 0; x < fl->width; x++) {
                uint32_t texel;

                if (!sgi_soft_fixed_read(r, f, i, x, y, &texel)) {
                    return false;
                }
                store->plain[((size_t)y << fl->width_shift) + x] =
                    sgi_soft_fixed_lanes(texel);
            }
        }
    }
    for (uint32_t i = first; i <= last; i++) {
        f->plain[i] = r->texels->fixed[i].plain;
    }
    f->prefilled = true;
    return true;
}

/* Interior texel (x, y) of relative level i in lanes, read once a draw. */
static inline QEMU_ALWAYS_INLINE
bool sgi_soft_fixed_texel(const SGISoftDraw *r, uint32_t i, uint32_t x,
                          uint32_t y, uint64_t *lanes)
{
    size_t index = ((size_t)y << r->fixed->level[i].width_shift) + x;
    uint64_t *cell, v;

    if (r->fixed->prefilled) {
        *lanes = r->fixed->plain[i][index];
        return true;
    }
    cell = &r->texels->fixed[i].cell[index];
    v = *cell;
    if (unlikely((uint32_t)(v >> 32) != r->texels->tag)) {
        if ((uint32_t)(v >> 32) == (r->texels->tag | SGI_SOFT_FIXED_FAILED)) {
            return false;
        }
        v = sgi_soft_fixed_fill(r, i, x, y, cell);
        if ((uint32_t)(v >> 32) != r->texels->tag) {
            return false;
        }
    }
    *lanes = sgi_soft_fixed_lanes(v);
    return true;
}

/*
 * The texel at signed index (x, y) of relative level i after wrapping,
 * which leaves an index outside the interior only under CLAMP and
 * CLAMP_TO_BORDER: there the border colour.
 */
static inline QEMU_ALWAYS_INLINE
bool sgi_soft_fixed_tap(const SGISoftDraw *r, uint32_t i, int64_t x,
                        int64_t y, const bool repeat, uint64_t *lanes)
{
    const SGISoftFixedLevel *fl = &r->fixed->level[i];

    if (!repeat && (x < 0 || y < 0 || x >= fl->width || y >= fl->height)) {
        *lanes = r->fixed->border;
        return true;
    }
    return sgi_soft_fixed_texel(r, i, x, y, lanes);
}

/* floor(x) for |x| below 2^62. */
static inline int64_t sgi_soft_fixed_floor(double x)
{
    int64_t i = (int64_t)x;

    return i - (x < i);
}

/*
 * One axis of a fixed-point position u, in 1/256 of a texel of a level n
 * texels wide, wrapped as sgi_soft_texture_axis() wraps the coordinate,
 * with CLAMP's upper bound upper rather than 1: floor() of the clamped
 * coordinate is the clamped floor(), so the bound is exactly the double
 * path's.
 * for LINEAR, the first tap's index in *x0, the second's in *x1 and the
 * second's weight; for NEAREST, the tap's index in *x0.
 */
static inline QEMU_ALWAYS_INLINE
uint32_t sgi_soft_fixed_axis(int64_t u, uint32_t n, uint32_t wrap,
                             double upper, bool linear, int64_t *x0,
                             int64_t *x1)
{
    int64_t whole = (int64_t)n << 8;
    uint32_t a = 0;

    if (wrap == SGI_GL_CLAMP && upper < 1) {
        u = MIN(u, sgi_soft_fixed_floor(upper * whole));
    }

    if (linear) {
        switch (wrap) {
        case SGI_GL_REPEAT:
            u -= 128;
            break;
        case SGI_GL_CLAMP_TO_EDGE:
            u = MIN(MAX(u - 128, 0), whole - 256);
            break;
        case SGI_GL_CLAMP_TO_BORDER:
            u = MIN(MAX(u, -128), whole + 128) - 128;
            break;
        default:
            u = MIN(MAX(u, 0), whole) - 128;
            break;
        }
        a = u & 255;
        *x0 = u >> 8;
        *x1 = *x0 + 1;
        if (wrap == SGI_GL_REPEAT) {
            *x0 &= n - 1;
            *x1 &= n - 1;
        } else if (wrap == SGI_GL_CLAMP_TO_EDGE) {
            *x1 = MIN(*x1, n - 1);
        }
        return a;
    }
    *x0 = u >> 8;
    switch (wrap) {
    case SGI_GL_REPEAT:
        *x0 &= n - 1;
        break;
    case SGI_GL_CLAMP_TO_BORDER:
        *x0 = MIN(MAX(*x0, -1), (int64_t)n);
        break;
    default:
        *x0 = MIN(MAX(*x0, 0), (int64_t)n - 1);
        break;
    }
    return 0;
}

/* Two of a texel's lanes, in the low 16 bits of each 32-bit half. */
#define SGI_SOFT_FIXED_PAIRS 0x0000ffff0000ffffull

/*
 * Relative level i sampled at position (u, v), in 1/256 of its texels,
 * with filter, each component in 1/(255 * 2^16): out[0] holds red over
 * blue and out[1] green over alpha, one to each 32-bit half. Across, one
 * product weights all four of a texel's components in their 16-bit
 * lanes; down, the sums need 24 bits, so two products take two each.
 */
static inline QEMU_ALWAYS_INLINE
bool sgi_soft_fixed_level(const SGISoftDraw *r, uint32_t i, uint32_t filter,
                          int64_t u, int64_t v, const bool repeat,
                          uint64_t out[2])
{
    const SGISoftFixedTex *f = r->fixed;
    const SGISoftFixedLevel *fl = &f->level[i];
    uint32_t ws = repeat ? SGI_GL_REPEAT : f->wrap_s;
    uint32_t wt = repeat ? SGI_GL_REPEAT : f->wrap_t;
    int64_t x0, x1, y0, y1;
    uint64_t c00, c10, c01, c11, top, bot;
    uint32_t a, b;

    if (filter == SGI_GL_NEAREST) {
        sgi_soft_fixed_axis(u, fl->width, ws, f->upper_s, false, &x0, &x1);
        sgi_soft_fixed_axis(v, fl->height, wt, f->upper_t, false, &y0, &y1);
        if (!sgi_soft_fixed_tap(r, i, x0, y0, repeat, &c00)) {
            return false;
        }
        out[0] = ((c00 >> 16) & SGI_SOFT_FIXED_PAIRS) << 16;
        out[1] = (c00 & SGI_SOFT_FIXED_PAIRS) << 16;
        return true;
    }
    a = sgi_soft_fixed_axis(u, fl->width, ws, f->upper_s, true, &x0, &x1);
    b = sgi_soft_fixed_axis(v, fl->height, wt, f->upper_t, true, &y0, &y1);
    if (!sgi_soft_fixed_tap(r, i, x0, y0, repeat, &c00) ||
        !sgi_soft_fixed_tap(r, i, x1, y0, repeat, &c10) ||
        !sgi_soft_fixed_tap(r, i, x0, y1, repeat, &c01) ||
        !sgi_soft_fixed_tap(r, i, x1, y1, repeat, &c11)) {
        return false;
    }
    top = c00 * (256 - a) + c10 * a;
    bot = c01 * (256 - a) + c11 * a;
    out[0] = ((top >> 16) & SGI_SOFT_FIXED_PAIRS) * (256 - b) +
             ((bot >> 16) & SGI_SOFT_FIXED_PAIRS) * b;
    out[1] = (top & SGI_SOFT_FIXED_PAIRS) * (256 - b) +
             (bot & SGI_SOFT_FIXED_PAIRS) * b;
    return true;
}

/* round(128 log2(1 + (i + 0.5) / 1024)): lambda's table, in 1/256. */
static int16_t sgi_soft_fixed_log2[1024];

static void __attribute__((constructor)) sgi_soft_fixed_log2_init(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(sgi_soft_fixed_log2); i++) {
        sgi_soft_fixed_log2[i] = lround(128 * log2(1 + (i + 0.5) / 1024));
    }
}

/*
 * Lambda in 1/256, log2 of the footprint rho, from rho squared, finite
 * and not negative: half its binary exponent plus the table's value for
 * the top ten bits of its significand. Zero and subnormals give the
 * least lambda, which every level clamp raises.
 */
static inline int32_t sgi_soft_fixed_lambda(double rho2)
{
    uint64_t bits;
    int exponent;

    memcpy(&bits, &rho2, sizeof(bits));
    exponent = (bits >> 52) & 0x7ff;
    if (!exponent) {
        return -SGI_SOFT_FIXED_LAMBDA_MAX;
    }
    return (exponent - 1023) * 128 + sgi_soft_fixed_log2[(bits >> 42) & 1023];
}

/*
 * Lambda for footprint squared rho2 from its table value lambda, or,
 * within a few steps of a lambda where the filter or nearest level
 * changes, computed exactly and rounded up. A lambda of ceil(256 l) is at
 * or below 256 c exactly when l is at or below c, and rounds to the same
 * nearest level, so those choices are sgi_soft_texture_select()'s; only
 * the blend between levels takes the table's precision.
 */
static inline int32_t sgi_soft_fixed_decide(const SGISoftFixedTex *f,
                                            double rho2, int32_t lambda)
{
    int32_t clamped = MIN(MAX(lambda, f->min_lod), f->max_lod);
    bool at_edge = abs(clamped - f->mag_below) <= 3 ||
                   ((f->min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
                     f->min == SGI_GL_LINEAR_MIPMAP_NEAREST) &&
                    abs(((clamped - 128) & 255) - 128) >= 125);

    if (!f->choice || !at_edge) {
        return lambda;
    }
    return rho2 > 0 ? MIN(MAX(ceil(128 * log2(rho2)),
                              -SGI_SOFT_FIXED_LAMBDA_MAX),
                          SGI_SOFT_FIXED_LAMBDA_MAX) :
                      -SGI_SOFT_FIXED_LAMBDA_MAX;
}

/*
 * A fragment's s, t and lambda from the triangle's planes g at pixel
 * column px, whose row terms are row; false where the fixed sampler
 * cannot take it. rho2_flat, where Q/W is the same across the triangle,
 * is its footprint squared there, the same at every fragment.
 */
static inline QEMU_ALWAYS_INLINE
bool sgi_soft_fixed_coords(const SGISoftFixedTex *f,
                           const SGISoftTexPlane *g, const double row[4],
                           int px, int py, const int32_t *flat_lambda,
                           double *s, double *t, int32_t *lambda)
{
    double x = px + 0.5 - g->x;
    double q = g->value[2] + x * g->dx[2] + row[2];
    double iq, a, b, c, e, rho2;

    if (!(q > 0)) {
        return false;
    }
    iq = 1 / q;
    *s = (g->value[0] + x * g->dx[0] + row[0]) * iq;
    *t = (g->value[1] + x * g->dx[1] + row[1]) * iq;
    if (!(fabs(*s) < 0x1p30 && fabs(*t) < 0x1p30)) {
        return false;
    }
    if (!f->lod || flat_lambda) {
        *lambda = flat_lambda ? *flat_lambda : 0;
        return true;
    }
    if (f->quad && sgi_soft_texture_quad_rho(g, px, py, f->ns, f->nt, &e)) {
        rho2 = e * e;
        if (!(rho2 <= DBL_MAX)) {
            return false;
        }
        *lambda = sgi_soft_fixed_decide(f, rho2, sgi_soft_fixed_lambda(rho2));
        return true;
    }
    /* d(S/Q)/dx is (dS/dx - s dQ/dx) / Q, and so on. */
    a = f->ns * (g->dx[0] - *s * g->dx[2]) * iq;
    b = f->nt * (g->dx[1] - *t * g->dx[2]) * iq;
    c = f->ns * (g->dy[0] - *s * g->dy[2]) * iq;
    e = f->nt * (g->dy[1] - *t * g->dy[2]) * iq;
    rho2 = MAX(a * a + b * b, c * c + e * e);
    if (!(rho2 <= DBL_MAX)) {
        return false;
    }
    *lambda = sgi_soft_fixed_decide(f, rho2, sgi_soft_fixed_lambda(rho2));
    return true;
}

/*
 * The fixed-point sampler's choice of kernel, a constant in the
 * specialised row loops: NEAREST or LINEAR with no footprint, trilinear
 * (LINEAR_MIPMAP_LINEAR minification with LINEAR magnification), or any
 * filter the image has, chosen per fragment.
 */
typedef enum SGISoftFixedKernel {
    SGI_SOFT_FIXED_ANY,
    SGI_SOFT_FIXED_NEAREST,
    SGI_SOFT_FIXED_LINEAR,
    SGI_SOFT_FIXED_TRILINEAR,
} SGISoftFixedKernel;

/*
 * The filtered texel at (s, t) for lambda, each component in [0, 1], as
 * sgi_soft_texture_select() chooses filter, levels and their blend, with
 * lambda and the blend fraction in 1/256. kernel, and repeat (both axes
 * GL_REPEAT), are constants where the caller can name them.
 */
static inline QEMU_ALWAYS_INLINE
bool sgi_soft_fixed_sample(const SGISoftDraw *r, double s, double t,
                           int32_t lambda, const SGISoftFixedKernel kernel,
                           const bool repeat, double texel[4])
{
    const SGISoftFixedTex *f = r->fixed;
    const double unit = 1.0 / (255.0 * (1 << 24));
    uint32_t first = 0, second = 0, alpha = 0;
    uint32_t filter = kernel == SGI_SOFT_FIXED_NEAREST ? SGI_GL_NEAREST :
                      kernel == SGI_SOFT_FIXED_ANY ? f->min : SGI_GL_LINEAR;
    const SGISoftFixedLevel *fl;
    uint64_t out[2], other[2];
    int64_t u, v;

    if (kernel == SGI_SOFT_FIXED_TRILINEAR ||
        (kernel == SGI_SOFT_FIXED_ANY && f->lod)) {
        lambda = MIN(MAX(lambda, f->min_lod), f->max_lod);
    }
    if (kernel == SGI_SOFT_FIXED_TRILINEAR) {
        /* At or below 0, magnification is the same LINEAR on level 0. */
        uint32_t rel = MIN(MAX(lambda, 0), (int32_t)(f->range << 8));

        first = rel >> 8;
        alpha = rel & 255;
        second = MIN(first + 1, f->range);
    } else if (kernel == SGI_SOFT_FIXED_ANY && f->lod) {
        if (lambda <= f->mag_below) {
            filter = f->mag;
        } else if (sgi_soft_texture_mipmap(f->min)) {
            filter = f->min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
                     f->min == SGI_GL_NEAREST_MIPMAP_LINEAR ?
                     SGI_GL_NEAREST : SGI_GL_LINEAR;
            if (f->min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
                f->min == SGI_GL_LINEAR_MIPMAP_NEAREST) {
                /* The nearest level: lambda rounded, half down. */
                first = lambda <= 128 ? 0 :
                        lambda > (int32_t)(f->range << 8) + 128 ? f->range :
                        (uint32_t)(lambda + 127) >> 8;
            } else {
                uint32_t rel = MIN(MAX(lambda, 0),
                                   (int32_t)(f->range << 8));

                first = rel >> 8;
                alpha = rel & 255;
                second = MIN(first + 1, f->range);
            }
        }
    }
    fl = &f->level[first];
    u = sgi_soft_fixed_floor(s * (fl->width * 256.0));
    v = sgi_soft_fixed_floor(t * (fl->height * 256.0));
    if (!sgi_soft_fixed_level(r, first, filter, u, v, repeat, out)) {
        return false;
    }
    if (alpha) {
        /*
         * The next level's position: a level half as wide scales s by an
         * exact half, so its floor is this one's halved, rounded down.
         */
        u >>= fl->width_shift - f->level[second].width_shift;
        v >>= fl->height_shift - f->level[second].height_shift;
        if (!sgi_soft_fixed_level(r, second, filter, u, v, repeat, other)) {
            return false;
        }
        out[0] = out[0] * (256 - alpha) + other[0] * alpha;
        out[1] = out[1] * (256 - alpha) + other[1] * alpha;
    } else {
        out[0] <<= 8;
        out[1] <<= 8;
    }
    texel[0] = (uint32_t)(out[0] >> 32) * unit;
    texel[1] = (uint32_t)(out[1] >> 32) * unit;
    texel[2] = (uint32_t)out[0] * unit;
    texel[3] = (uint32_t)out[1] * unit;
    return true;
}

/*
 * One triangle's fixed inputs to a textured row loop
 * (sgi_soft_texture_row()).
 */
struct SGISoftTexSpan {
    const SGISoftVertex *v;         /* three, wound as the rows step */
    const SGISoftTexPlane *g;
    const double *row;              /* the texture planes' row terms */
    const int32_t *flat_lambda;     /* lambda where Q/W is flat, or NULL */
    const SGISoftFrag *flat;
    double offset;
    int64_t area, step1, step2;
    bool smooth;
    /*
     * Smooth colour components that are 0 at every vertex, or 1 at
     * every vertex: interpolation gives exactly that value, the terms
     * summing to the weights' own sum, so they are not interpolated.
     */
    uint8_t constant;
};

/*
 * Pixels px0 to px1 of row py of a textured triangle, whose edge values
 * for v[2]-v[0] and v[0]-v[1] at px0 are e1 and e2, under state
 * sgi_soft_fixed_span_select() admits: what the general loop and
 * sgi_soft_fragment_at() leave for each fragment, with the fixed-point
 * sampler, and the same planes; only the order of target calls differs,
 * the depth read coming before the texture's. It stops at the first
 * fragment the fixed sampler cannot take and returns its column, which
 * the caller draws through the general loop, or px1 + 1 when the row is
 * done. The constant arguments select a specialisation each: the
 * sampler's kernel and wrap, the environment and base format where
 * nonzero, and with known set, a depth-tested draw without blending or
 * fog, so the per-pixel loop carries no test of them.
 */
static inline QEMU_ALWAYS_INLINE
int sgi_soft_texture_row(const SGISoftDraw *r, const SGISoftTexSpan *sp,
                         int py, int px0, int px1, int64_t e1, int64_t e2,
                         const SGISoftFixedKernel kernel, const bool repeat,
                         const uint32_t env_known,
                         const uint32_t format_known, const bool known)
{
    const SGIRasterRules *rules = r->rules;
    const SGIGLDrawState *st = r->st;
    const SGIRasterTarget *t = r->t;
    const SGISoftVertex *v = sp->v;
    const int y = r->win.oy + py;
    bool clipped = r->win.clip->n > 1;
    const bool depth_test = known || st->depth_test;
    const bool fog = !known && st->fog;
    const bool rgba_read = t->store_rgba && r->rgba_read;
    const uint32_t env = env_known ? env_known : st->texture_env_mode;
    const uint32_t format = format_known ? format_known : r->fixed->format;
    /* REPLACE leaves none of the colour of these formats' fragments. */
    const bool keeps = env != SGI_GL_REPLACE ||
                       (format != SGI_GL_RGBA && format != SGI_GL_INTENSITY &&
                        format != SGI_GL_LUMINANCE_ALPHA);
    int end = px1 + 1;
    SGISoftFrag frag = *sp->flat;
    SGIRasterColour dest[SGI_GL_MAX_TARGETS] = { 0 };

    for (uint32_t i = 0; clipped && i < MIN(r->win.clip->n, SGI_CLIP_MAX);
         i++) {
        /* One of the window's rectangles holds the whole row. */
        const SGIClipRect *c = &r->win.clip->r[i];

        clipped = !(y >= c->y0 && y <= c->y1 &&
                    (int64_t)r->win.ox + px0 >= c->x0 &&
                    (int64_t)r->win.ox + px1 <= c->x1);
    }
    if (st->scissor_test && !clipped) {
        /* sgi_soft_owned_at()'s scissor test, decided for the whole row. */
        int64_t sx0 = st->scissor[0];
        int64_t sx1 = (int64_t)st->scissor[0] + st->scissor[2] - 1;

        if (!(py >= st->scissor[1] &&
              (int64_t)py < (int64_t)st->scissor[1] + st->scissor[3])) {
            return end;
        }
        if (sx0 > px0) {
            if (sx0 > px1) {
                return end;
            }
            e1 += (sx0 - px0) * sp->step1;
            e2 += (sx0 - px0) * sp->step2;
            px0 = sx0;
        }
        px1 = MIN(px1, sx1);
    }
    for (int px = px0; px <= px1; px++, e1 += sp->step1, e2 += sp->step2) {
        const int x = r->win.ox + px;
        double b1 = (double)e1 / sp->area;
        double b2 = (double)e2 / sp->area;
        double z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
        double w0 = (1.0 - b1 - b2) * v[0].inv_w;
        double w1 = b1 * v[1].inv_w;
        double w2 = b2 * v[2].inv_w;
        double sum = w0 + w1 + w2;
        double off = sp->offset, zw, s, tc;
        int32_t lambda;
        uint32_t zq = 0;

        if (rules->depth_row_given) {
            double depth_w = (1.0 - b1 - b2) * v[0].depth_w +
                             b1 * v[1].depth_w + b2 * v[2].depth_w;

            z = sgi_soft_row_depth(rules, z, off, depth_w);
            off = 0;
        }
        if (!sgi_raster_window_depth(st, z, off, &zw) ||
            !(zw >= 0.0 && zw <= 1.0)) {
            continue;
        }
        if (clipped && !sgi_soft_owned_at(r, px, py)) {
            continue;
        }
        if (depth_test) {
            zq = sgi_raster_depth_code(rules, zw);
            if (!sgi_soft_compare(st->depth_func, zq,
                                  t->load_depth(t->opaque, x, y))) {
                continue;
            }
        }
        bool coords = sgi_soft_fixed_coords(r->fixed, sp->g, sp->row, px,
                                            py, sp->flat_lambda, &s, &tc,
                                            &lambda);

        if (!coords ||
            !sgi_soft_fixed_sample(r, s, tc, lambda, kernel, repeat,
                                   frag.texel)) {
            return px;
        }
        if (sp->smooth && keeps) {
            for (int k = 0; k < 4; k++) {
                frag.rgba[k] = sp->constant & (1u << k) ? v[0].rgba[k] :
                               (w0 * v[0].rgba[k] + w1 * v[1].rgba[k] +
                                w2 * v[2].rgba[k]) / sum;
            }
        } else {
            memcpy(frag.rgba, sp->flat->rgba, sizeof(frag.rgba));
        }
        sgi_soft_texture_apply(r, format, env, frag.texel, frag.rgba);
        if (fog) {
            frag.fog = (w0 * v[0].fog + w1 * v[1].fog + w2 * v[2].fog) / sum;
            sgi_soft_fog(r, &frag, zw);
        }
        if (rgba_read &&
            !sgi_soft_rgba_load(rules, t, r->targets, r->ntargets, x, y,
                                dest, r->rgba_reported)) {
            continue;
        }
        if (depth_test && st->depth_mask) {
            t->store_depth(t->opaque, x, y, zq);
        }
        sgi_soft_fragment_store(r, x, y, &frag, dest);
    }
    return end;
}

#define SGI_SOFT_TEXTURE_SPAN(name, kernel, repeat, env, format, known) \
static int name(const SGISoftDraw *r, const SGISoftTexSpan *sp, int py, \
                int px0, int px1, int64_t e1, int64_t e2) \
{ \
    return sgi_soft_texture_row(r, sp, py, px0, px1, e1, e2, kernel, \
                                repeat, env, format, known); \
}

/* Any state the row loops take, tested per pixel. */
SGI_SOFT_TEXTURE_SPAN(sgi_soft_texture_span_any, SGI_SOFT_FIXED_ANY, false,
                      0, 0, false)

/*
 * The states drawing commonly textures in, one loop each: a repeated RGB
 * or RGBA image, nearest, bilinear or trilinear, replacing or modulating
 * the fragment's colour, depth tested, without blending or fog.
 */
#define SGI_SOFT_TEXTURE_SPANS(kernel, k) \
SGI_SOFT_TEXTURE_SPAN(sgi_soft_texture_span_##k##_replace_rgba, kernel, \
                      true, SGI_GL_REPLACE, SGI_GL_RGBA, true) \
SGI_SOFT_TEXTURE_SPAN(sgi_soft_texture_span_##k##_replace_rgb, kernel, \
                      true, SGI_GL_REPLACE, SGI_GL_RGB, true) \
SGI_SOFT_TEXTURE_SPAN(sgi_soft_texture_span_##k##_modulate_rgba, kernel, \
                      true, SGI_GL_MODULATE, SGI_GL_RGBA, true) \
SGI_SOFT_TEXTURE_SPAN(sgi_soft_texture_span_##k##_modulate_rgb, kernel, \
                      true, SGI_GL_MODULATE, SGI_GL_RGB, true)

SGI_SOFT_TEXTURE_SPANS(SGI_SOFT_FIXED_NEAREST, nearest)
SGI_SOFT_TEXTURE_SPANS(SGI_SOFT_FIXED_LINEAR, linear)
SGI_SOFT_TEXTURE_SPANS(SGI_SOFT_FIXED_TRILINEAR, trilinear)

/* By kernel less one, then replace or modulate, then RGBA or RGB. */
static SGISoftTexSpanFn *const sgi_soft_texture_spans[3][2][2] = {
#define SGI_SOFT_TEXTURE_SPAN_ROW(k) { \
        { sgi_soft_texture_span_##k##_replace_rgba, \
          sgi_soft_texture_span_##k##_replace_rgb }, \
        { sgi_soft_texture_span_##k##_modulate_rgba, \
          sgi_soft_texture_span_##k##_modulate_rgb }, \
    }
    SGI_SOFT_TEXTURE_SPAN_ROW(nearest),
    SGI_SOFT_TEXTURE_SPAN_ROW(linear),
    SGI_SOFT_TEXTURE_SPAN_ROW(trilinear),
#undef SGI_SOFT_TEXTURE_SPAN_ROW
};

/*
 * The row loop for draw r's textured triangles, or NULL for the general
 * loop: under SGI_RASTER_TEXTURE_SPANS, for state whose per-fragment
 * work is ownership, the depth test, the texture, fog, blending and the
 * store, whatever the colour mask, dither and targets. A loop of its own
 * for the common states named above; the others take the loop that tests
 * the state per pixel.
 */
static SGISoftTexSpanFn *sgi_soft_fixed_span_select(const SGISoftDraw *r)
{
    const SGIGLDrawState *st = r->st;
    const SGIRasterTarget *t = r->t;
    const SGISoftFixedTex *f = r->fixed;
    uint32_t smax;
    int kernel, env, format;

    if (sgi_soft_texture_path != SGI_RASTER_TEXTURE_SPANS ||
        r->polygon_stipple || st->clip_planes ||
        (st->stencil_test && sgi_soft_stencil_max(r->rules, t, &smax)) ||
        (st->depth_test &&
         (!t->load_depth || (st->depth_mask && !t->store_depth)))) {
        return NULL;
    }
    kernel = !f->lod && f->min == SGI_GL_NEAREST ? 0 :
             !f->lod && f->min == SGI_GL_LINEAR ? 1 :
             f->min == SGI_GL_LINEAR_MIPMAP_LINEAR &&
             f->mag == SGI_GL_LINEAR ? 2 : -1;
    env = st->texture_env_mode == SGI_GL_REPLACE ? 0 :
          st->texture_env_mode == SGI_GL_MODULATE ? 1 : -1;
    format = f->format == SGI_GL_RGBA ? 0 : f->format == SGI_GL_RGB ? 1 : -1;
    if (kernel < 0 || env < 0 || format < 0 ||
        f->wrap_s != SGI_GL_REPEAT || f->wrap_t != SGI_GL_REPEAT ||
        !st->depth_test || r->blend || st->fog) {
        return sgi_soft_texture_span_any;
    }
    return sgi_soft_texture_spans[kernel][env][format];
}

static void sgi_soft_texture_values(const SGISoftVertex *v, bool volume,
                                     bool one_dimensional, double value[4])
{
    value[0] = v->texcoord[0] * v->inv_w;
    value[1] = one_dimensional ? 0 : v->texcoord[1] * v->inv_w;
    value[2] = v->texcoord[3] * v->inv_w;
    value[3] = volume ? v->texcoord[2] * v->inv_w : 0;
}

static bool sgi_soft_texture_plane(const SGISoftVertex v[3], unsigned sub,
                                   bool volume, bool one_dimensional,
                                   SGISoftTexPlane *g)
{
    double scale = 1 << sub;
    double x1 = (v[1].x - v[0].x) / scale;
    double y1 = (v[1].y - v[0].y) / scale;
    double x2 = (v[2].x - v[0].x) / scale;
    double y2 = (v[2].y - v[0].y) / scale;
    double area = x1 * y2 - x2 * y1;
    double values[3][4];

    *g = (SGISoftTexPlane) { .x = v[0].x / scale, .y = v[0].y / scale };
    if (!area) {
        return true;
    }
    if (!isfinite(area)) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        sgi_soft_texture_values(&v[i], volume, one_dimensional, values[i]);
    }
    for (unsigned k = 0; k < 4; k++) {
        double a = values[1][k] - values[0][k];
        double b = values[2][k] - values[0][k];

        g->value[k] = values[0][k];
        g->dx[k] = (a * y2 - b * y1) / area;
        g->dy[k] = (b * x1 - a * x2) / area;
        if (!isfinite(g->value[k]) || !isfinite(g->dx[k]) ||
            !isfinite(g->dy[k])) {
            return false;
        }
    }
    return true;
}

static bool sgi_soft_texture_line_gradient(const SGISoftVertex *a,
                                           const SGISoftVertex *b, unsigned sub,
                                           bool volume, bool one_dimensional,
                                           double values[4], double delta[4],
                                           double *length)
{
    double end[4];

    *length = hypot((double)(b->x - a->x), (double)(b->y - a->y)) /
              (1 << sub);
    if (!isfinite(*length)) {
        return false;
    }
    sgi_soft_texture_values(a, volume, one_dimensional, values);
    sgi_soft_texture_values(b, volume, one_dimensional, end);
    for (unsigned k = 0; k < 4; k++) {
        delta[k] = end[k] - values[k];
        if (!isfinite(values[k]) || !isfinite(delta[k])) {
            return false;
        }
    }
    return true;
}

static bool sgi_soft_texture_coefficients(const SGISoftDraw *r,
                                           const SGISoftVertex v[3])
{
    SGISoftTexPlane g;

    if (!r->texture_base || sgi_soft_texture_plane(v, r->rules->subpixel_bits,
                                                r->texture->volume,
                                                r->texture->one_dimensional,
                                                &g)) {
        return true;
    }
    sgi_soft_texture_report(r, 16, "invalid primitive coefficients, "
                                  "primitive dropped");
    return false;
}

/*
 * Whether a triangle's fragments can take sgi_soft_simple_row() instead of
 * the full per-fragment path: state that leaves each fragment's work at
 * interpolation, the depth range, pixel ownership, the depth test and an
 * unconditional store. Everything it excludes either reads a fragment
 * value the simple row does not interpolate (texture coordinates, fog
 * distance, clip-plane values, an index) or does per-pixel work it does
 * not do (stipple, alpha and stencil tests, destination reads).
 */
static bool sgi_soft_simple_fragments(const SGISoftDraw *r)
{
    const SGIGLDrawState *st = r->st;
    const SGIRasterTarget *t = r->t;

    if (r->texture || st->fog || st->clip_planes || r->polygon_stipple ||
        st->index_bits || r->blend || r->logic_op || st->alpha_test ||
        st->stencil_test) {
        return false;
    }
    /* sgi_raster_depth_stencil() drops every fragment of such a draw. */
    if (st->depth_test &&
        (!t->load_depth || (st->depth_mask && !t->store_depth))) {
        return false;
    }
    if (t->store_rgba) {
        return !r->rgba_read;
    }
    return !r->colour_mask ||
           r->colour_mask == sgi_soft_colour_bits(r->rules);
}

/*
 * One row of a triangle under sgi_soft_simple_fragments(): pixels px0 to
 * px1 of row py, whose edge values for v[2]-v[0] and v[0]-v[1] at px0 are
 * e1 and e2. Each fragment's values and each target call are those
 * sgi_soft_fragment() makes for it, in the same order; the work that
 * cannot vary along the row, or with the state excluded, is done once.
 * The constant flags select a specialisation each, so the per-pixel loop
 * carries no test of them.
 */
static inline QEMU_ALWAYS_INLINE
void sgi_soft_simple_row(const SGISoftDraw *r, const SGISoftVertex v[3],
                         int py, int px0, int px1, int64_t e1, int64_t e2,
                         int64_t step1, int64_t step2, int64_t area,
                         double offset, const SGISoftFrag *flat,
                         const bool smooth, const bool rgba,
                         const bool depth_row)
{
    const SGIRasterRules *rules = r->rules;
    const SGIGLDrawState *st = r->st;
    const SGIRasterTarget *t = r->t;
    const int y = r->win.oy + py;
    /* A flat colour is the same at every pixel unless dithered. */
    const bool fixed = !smooth && !r->dither;
    const bool store = rgba ? r->rgba_mask && r->ntargets : r->colour_mask;
    const bool clipped = r->win.clip->n > 1;
    SGIRasterColour fixed_rgba = { 0 };
    uint32_t fixed_colour = 0;

    if (st->scissor_test && !clipped) {
        /* sgi_soft_owned_at()'s scissor test, decided for the whole row. */
        int64_t sx0 = st->scissor[0];
        int64_t sx1 = (int64_t)st->scissor[0] + st->scissor[2] - 1;

        if (!(py >= st->scissor[1] &&
              (int64_t)py < (int64_t)st->scissor[1] + st->scissor[3])) {
            return;
        }
        if (sx0 > px0) {
            if (sx0 > px1) {
                return;
            }
            e1 += (sx0 - px0) * step1;
            e2 += (sx0 - px0) * step2;
            px0 = sx0;
        }
        px1 = MIN(px1, sx1);
    }
    if (fixed && store) {
        if (rgba) {
            double alpha = sgi_soft_alpha(rules, flat->rgba[3]) /
                           (double)sgi_soft_alpha_max(rules);

            fixed_rgba = sgi_soft_rgba_value(rules,
                                             sgi_soft_rgb(rules, flat->rgba),
                                             alpha);
        } else {
            fixed_colour = sgi_soft_rgb(rules, flat->rgba);
        }
    }
    for (int px = px0; px <= px1; px++, e1 += step1, e2 += step2) {
        const int x = r->win.ox + px;
        double b1 = (double)e1 / area;
        double b2 = (double)e2 / area;
        double z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
        double off = offset, zw;
        double colour[4];

        if (depth_row) {
            double depth_w = (1.0 - b1 - b2) * v[0].depth_w +
                             b1 * v[1].depth_w + b2 * v[2].depth_w;

            z = sgi_soft_row_depth(rules, z, off, depth_w);
            off = 0;
        }
        if (!sgi_raster_window_depth(st, z, off, &zw) ||
            !(zw >= 0.0 && zw <= 1.0)) {
            continue;
        }
        if (clipped && !sgi_soft_owned_at(r, px, py)) {
            continue;
        }
        if (st->depth_test || store) {
            uint32_t zq = sgi_raster_depth_code(rules, zw);

            if (st->depth_test) {
                if (!sgi_soft_compare(st->depth_func, zq,
                                      t->load_depth(t->opaque, x, y))) {
                    continue;
                }
                if (st->depth_mask) {
                    t->store_depth(t->opaque, x, y, zq);
                }
            }
        }
        if (!store) {
            continue;
        }
        if (smooth) {
            double w0 = (1.0 - b1 - b2) * v[0].inv_w;
            double w1 = b1 * v[1].inv_w;
            double w2 = b2 * v[2].inv_w;
            double sum = w0 + w1 + w2;

            for (int k = 0; k < 4; k++) {
                colour[k] = (w0 * v[0].rgba[k] + w1 * v[1].rgba[k] +
                             w2 * v[2].rgba[k]) / sum;
            }
        } else {
            memcpy(colour, flat->rgba, sizeof(colour));
        }
        if (rgba) {
            SGIRasterColour out = fixed_rgba;

            if (!fixed) {
                double alpha = sgi_soft_alpha(rules, colour[3]) /
                               (double)sgi_soft_alpha_max(rules);

                out = sgi_soft_rgba_value(rules,
                                          sgi_soft_store_rgb(r, x, y, colour),
                                          alpha);
            }
            for (unsigned i = 0; i < r->ntargets; i++) {
                t->store_rgba(t->opaque, r->targets[i], x, y, &out);
            }
        } else {
            uint32_t out = fixed ? fixed_colour :
                                   sgi_soft_store_rgb(r, x, y, colour);

            for (unsigned i = 0; i < r->ntargets; i++) {
                t->store_colour(t->opaque, r->targets[i], x, y, out);
            }
        }
    }
}

/*
 * Out of line: its eight specialisations inlined into the triangle
 * routine made that four times larger and slowed its generic loop, which
 * textured triangles take, by 2-3%. It is called once a row.
 */
static void __attribute__((noinline))
sgi_soft_simple_span(const SGISoftDraw *r, const SGISoftVertex v[3], int py,
                     int px0, int px1, int64_t e1, int64_t e2, int64_t step1,
                     int64_t step2, int64_t area, double offset,
                     const SGISoftFrag *flat, bool smooth)
{
    bool rgba = r->t->store_rgba;

#define SGI_SOFT_SIMPLE(sm, rg, dr) \
    sgi_soft_simple_row(r, v, py, px0, px1, e1, e2, step1, step2, area, \
                        offset, flat, sm, rg, dr)
    if (r->rules->depth_row_given) {
        if (rgba) {
            smooth ? SGI_SOFT_SIMPLE(true, true, true) :
                     SGI_SOFT_SIMPLE(false, true, true);
        } else {
            smooth ? SGI_SOFT_SIMPLE(true, false, true) :
                     SGI_SOFT_SIMPLE(false, false, true);
        }
    } else if (rgba) {
        smooth ? SGI_SOFT_SIMPLE(true, true, false) :
                 SGI_SOFT_SIMPLE(false, true, false);
    } else {
        smooth ? SGI_SOFT_SIMPLE(true, false, false) :
                 SGI_SOFT_SIMPLE(false, false, false);
    }
#undef SGI_SOFT_SIMPLE
}

/*
 * Map and rasterize one triangle of a clipped polygon, given its three
 * vertices in the view volume and, for flat shading, the provoking
 * vertex of the triangle it was clipped from (OpenGL 1.2.1, 2.13.7 and
 * table 2.9).
 *
 * Facing (2.13.1) is the sign of the triangle's area in window
 * coordinates on the subpixel grid: a positive area is front facing under
 * glFrontFace(GL_CCW) and a negative one under GL_CW. Under a window
 * matrix whose rows run down the display, window y runs opposite to the
 * client's, which mirrors the winding, so the area's sign is taken
 * negated: a triangle counter-clockwise as the client drew it is
 * counter-clockwise here too, for culling and two-sided colour alike.
 * With GL_CULL_FACE enabled a triangle whose facing glCullFace names is
 * not rasterized (3.5.1). When the draw carries back colours a
 * back-facing triangle takes them. A polygon or quad is drawn as
 * triangles, each facing by its own area, which for a convex polygon is
 * the polygon's facing; so is each triangle of a clipped one. The tie
 * rule still names edges in window rows as they run, so under rows down
 * the display SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN's owned edges are the
 * bottom and left ones on the display.
 *
 * The fragments are also bounded by the viewport rectangle, when there
 * is one, which clipping has already made them lie in; a normalized depth
 * outside [0, 1] could now come only from rounding, and is still dropped.
 *
 * Depth is interpolated linearly in window coordinates and colour with
 * the perspective correction of equation 3.4 (OpenGL 1.2.1, 3.5.1).
 */
static void sgi_soft_clipped_triangle(const SGISoftDraw *r,
                                      const SGIVolumeVertex *va,
                                      const SGIVolumeVertex *vb,
                                      const SGIVolumeVertex *vc,
                                      const float *provoking)
{
    const SGIGLDrawState *st = r->st;
    const SGIVolumeVertex *verts[3] = { va, vb, vc };
    const float *flat = &provoking[SGI_GL_V_COLOR];
    const float *flat_index = &provoking[SGI_GL_V_INDEX];
    /*
     * Outside the loop: a fragment declared per pixel without an
     * initializer is zeroed per pixel under -ftrivial-auto-var-init.
     */
    SGISoftFrag flat_frag, frag;
    SGISoftVertex v[3];
    SGISoftTexPlane gradient;
    /* The footprint's per-axis scales, and its value where Q/W is flat. */
    double tex_ns = 0, tex_nt = 0, tex_nr = 0, flat_rho = NAN;
    bool flat_q = false, flat_ok = false;
    /* Fragments sample in fixed point, and their lambda where it is flat. */
    bool fixed = false;
    int32_t fixed_flat_lambda = 0;
    const int32_t *fixed_flat = NULL;
    /* The row loop for textured fragments, and its inputs. */
    SGISoftTexSpanFn *span = NULL;
    SGISoftTexSpan sp;
    int64_t min_x, max_x, min_y, max_y, area, step0, step1, step2;
    double offset;
    bool ccw, front;
    int px0, px1, py0, py1;
    bool smooth = st->shade_model != SGI_GL_FLAT;
    bool simple = sgi_soft_simple_fragments(r);
    double tex_row[4];
    const int sub = r->rules->subpixel_bits;

    for (int i = 0; i < 3; i++) {
        if (!sgi_soft_project(r, verts[i], &v[i])) {
            return;
        }
    }

    area = sgi_soft_edge(&v[0], &v[1], v[2].x, v[2].y);
    if (area == 0) {
        return;
    }
    if (r->texture_base &&
        !sgi_soft_texture_plane(v, sub, r->texture->volume,
                                r->texture->one_dimensional, &gradient)) {
        sgi_soft_texture_report(r, 16, "invalid primitive coefficients, "
                                      "primitive dropped");
        return;
    }
    /* An image with no pyramid has no planes to fail: it samples in double. */
    fixed = r->fixed &&
            (r->texture_base ||
             sgi_soft_texture_plane(v, sub, false, false, &gradient));
    if (fixed && r->fixed->lod && !r->fixed->quad && gradient.dx[2] == 0 &&
        gradient.dy[2] == 0) {
        /*
         * Q/W flat, as for the double footprint below: every fragment's
         * lambda is this one, which sgi_soft_fixed_coords() would compute
         * for each of them, since s dQ/dx and t dQ/dx are then zero.
         */
        double q = gradient.value[2], iq, a, b, c, e, rho2;

        if (q > 0) {
            iq = 1 / q;
            a = r->fixed->ns * gradient.dx[0] * iq;
            b = r->fixed->nt * gradient.dx[1] * iq;
            c = r->fixed->ns * gradient.dy[0] * iq;
            e = r->fixed->nt * gradient.dy[1] * iq;
            rho2 = MAX(a * a + b * b, c * c + e * e);
            if (rho2 <= DBL_MAX) {
                fixed_flat_lambda = sgi_soft_fixed_decide(r->fixed, rho2,
                    sgi_soft_fixed_lambda(rho2));
                fixed_flat = &fixed_flat_lambda;
            }
        }
    }
    if (r->texture_base) {
        tex_ns = r->texture_base->width *
                 sgi_soft_texture_lod_scale(r->texture->lod_scale_s);
        tex_nt = r->texture->one_dimensional ? 0 :
                 r->texture_base->height *
                 sgi_soft_texture_lod_scale(r->texture->lod_scale_t);
        tex_nr = r->texture->volume ? r->texture_base->depth *
                 sgi_soft_texture_lod_scale(r->texture->lod_scale_r) : 0;
        /*
         * With Q/W the same at every vertex, each fragment's Q/W is the
         * plane's own value, since its x and y terms are products with
         * zero, and every S/W, T/W and R/W enters the derivatives only
         * through such a product, which is zero for a finite value: their
         * signs aside, the derivatives are those at any finite point, and
         * the footprint, made of their magnitudes, is one number. Only
         * each fragment's finiteness test remains its own.
         */
        flat_q = gradient.dx[2] == 0 && gradient.dy[2] == 0;
        if (flat_q) {
            double value[4] = { 0, 0, gradient.value[2], 0 }, out[5];

            flat_ok = sgi_soft_texture_derivatives(value, gradient.dx,
                                                   gradient.dy, tex_ns,
                                                   tex_nt, tex_nr, out);
            flat_rho = out[4];
        }
    }
    ccw = st->window_matrix_given && st->window_rows_down ? area < 0 :
                                                            area > 0;
    front = st->front_face == SGI_GL_CW ? !ccw : ccw;
    if (st->cull_face &&
        (st->cull_face_mode == SGI_GL_FRONT_AND_BACK ||
         st->cull_face_mode == (front ? SGI_GL_FRONT : SGI_GL_BACK))) {
        return;
    }
    if (!front && r->back_colors) {
        flat = &provoking[SGI_GL_V_BACK_COLOR];
        flat_index = &provoking[SGI_GL_V_BACK_INDEX];
        for (int i = 0; i < 3; i++) {
            const float *back = &verts[i]->attr[SGI_GL_V_BACK_COLOR];

            for (int k = 0; k < 4; k++) {
                v[i].rgba[k] = MIN(MAX(back[k], 0.0f), 1.0f);
            }
            v[i].index = verts[i]->attr[SGI_GL_V_BACK_INDEX];
        }
    }
    for (int k = 0; k < 4; k++) {
        flat_frag.rgba[k] = flat[k];
    }
    flat_frag.index = *flat_index;
    flat_frag.fixed = false;
    offset = sgi_soft_offset(r, v);
    if (area < 0) {
        /* Rasterize either winding the same way from here. */
        SGISoftVertex t = v[1];

        v[1] = v[2];
        v[2] = t;
        area = -area;
    }

    min_x = MIN(v[0].x, MIN(v[1].x, v[2].x)) >> sub;
    max_x = MAX(v[0].x, MAX(v[1].x, v[2].x)) >> sub;
    min_y = MIN(v[0].y, MIN(v[1].y, v[2].y)) >> sub;
    max_y = MAX(v[0].y, MAX(v[1].y, v[2].y)) >> sub;

    /* Window pixels, bounded by the viewport and the owned rectangle. */
    if (!sgi_soft_bounds(r, &px0, &py0, &px1, &py1)) {
        return;
    }
    px0 = MAX((int)min_x, px0);
    px1 = MIN((int)max_x, px1);
    py0 = MAX((int)min_y, py0);
    py1 = MIN((int)max_y, py1);

    /*
     * Each edge function is linear in x, so across a row it steps by an
     * exact integer: the same values sgi_soft_edge() gives at each centre.
     * A centre is drawn where all three edges own it, which on a row is
     * the overlap of each edge's run of owned centres: solving for that
     * run's ends visits only the pixels a test of every centre between
     * px0 and px1 would keep.
     */
    step0 = -(v[2].y - v[1].y) * (1 << sub);
    step1 = -(v[0].y - v[2].y) * (1 << sub);
    step2 = -(v[1].y - v[0].y) * (1 << sub);
    if (fixed && r->fixed->span) {
        span = r->fixed->span;
        sp = (SGISoftTexSpan) {
            .v = v, .g = &gradient, .row = tex_row,
            .flat_lambda = fixed_flat, .flat = &flat_frag,
            .offset = offset, .area = area, .step1 = step1, .step2 = step2,
            .smooth = smooth,
        };
        for (unsigned k = 0; k < 4; k++) {
            double c = v[0].rgba[k];

            if ((c == 0 || c == 1) && v[1].rgba[k] == c &&
                v[2].rgba[k] == c) {
                sp.constant |= 1u << k;
            }
        }
    }
    for (int py = py0; py <= py1; py++) {
        int64_t cy, cx, e0, e1, e2, lo, hi;

        if (!sgi_soft_row_in_band(r, py)) {
            continue;
        }
        cy = ((int64_t)py * (1 << sub)) + (1 << (sub - 1));
        cx = ((int64_t)px0 * (1 << sub)) + (1 << (sub - 1));
        e0 = sgi_soft_edge(&v[1], &v[2], cx, cy);
        e1 = sgi_soft_edge(&v[2], &v[0], cx, cy);
        e2 = sgi_soft_edge(&v[0], &v[1], cx, cy);
        lo = 0;
        hi = (int64_t)px1 - px0;
        sgi_soft_owned_run(&v[1], &v[2], e0, step0, &lo, &hi);
        sgi_soft_owned_run(&v[2], &v[0], e1, step1, &lo, &hi);
        sgi_soft_owned_run(&v[0], &v[1], e2, step2, &lo, &hi);
        if (lo > hi) {
            continue;
        }
        e1 += lo * step1;
        e2 += lo * step2;
        if (simple) {
            sgi_soft_simple_span(r, v, py, px0 + lo, px0 + hi, e1, e2, step1,
                                 step2, area, offset, &flat_frag, smooth);
            continue;
        }
        if (r->texture_base || fixed) {
            for (unsigned k = 0; k < 4; k++) {
                tex_row[k] = (py + 0.5 - gradient.y) * gradient.dy[k];
            }
        }
        for (int px = px0 + lo; px <= px0 + hi;
             px++, e1 += step1, e2 += step2) {
            double b1, b2, z, w0, w1, w2, sum, depth_w;
            bool background;

            if (span) {
                /*
                 * The row loop draws up to a fragment the fixed sampler
                 * cannot take, which this loop then draws in double.
                 */
                int stop = span(r, &sp, py, px, px0 + hi, e1, e2);

                e1 += (int64_t)(stop - px) * step1;
                e2 += (int64_t)(stop - px) * step2;
                px = stop;
                if (px > px0 + hi) {
                    break;
                }
            }
            /* Window phase precedes plane translation and fragment work. */
            if (!sgi_soft_stipple_pass(r, px, py, &background)) {
                continue;
            }
            frag = flat_frag;
            b1 = (double)e1 / area;
            b2 = (double)e2 / area;
            z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
            w0 = (1.0 - b1 - b2) * v[0].inv_w;
            w1 = b1 * v[1].inv_w;
            w2 = b2 * v[2].inv_w;
            sum = w0 + w1 + w2;
            depth_w = (1.0 - b1 - b2) * v[0].depth_w + b1 * v[1].depth_w +
                      b2 * v[2].depth_w;
            if (fixed && !span) {
                double s, t;
                int32_t lambda;

                frag.fixed = sgi_soft_fixed_coords(r->fixed, &gradient,
                                                   tex_row, px, py,
                                                   fixed_flat,
                                                   &s, &t, &lambda) &&
                             sgi_soft_fixed_sample(r, s, t, lambda,
                                                   SGI_SOFT_FIXED_ANY, false,
                                                   frag.texel);
            }
            /* Only texturing reads the coordinates, only fog the distance. */
            if (r->texture && !frag.fixed) {
                for (int k = 0; k < 4; k++) {
                    frag.texcoord[k] = (w0 * v[0].texcoord[k] +
                                        w1 * v[1].texcoord[k] +
                                        w2 * v[2].texcoord[k]) / sum;
                }
            }
            if (st->fog) {
                frag.fog = (w0 * v[0].fog + w1 * v[1].fog +
                            w2 * v[2].fog) / sum;
            }
            if (st->clip_planes) {
                double d[SGI_GL_CLIP_PLANES];

                for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
                    d[i] = (w0 * v[0].clip[i] + w1 * v[1].clip[i] +
                            w2 * v[2].clip[i]) / sum;
                }
                if (sgi_soft_clipped(st, d)) {
                    continue;
                }
            }
            if (smooth) {
                for (int k = 0; k < 4; k++) {
                    frag.rgba[k] = (w0 * v[0].rgba[k] + w1 * v[1].rgba[k] +
                                    w2 * v[2].rgba[k]) / sum;
                }
                /*
                 * An index is taken as a difference from the first
                 * vertex's, so a constant one stays exact before its
                 * integer part is taken.
                 */
                frag.index = v[0].index +
                             (w1 * (v[1].index - v[0].index) +
                              w2 * (v[2].index - v[0].index)) / sum;
            }
            if (r->texture_base && !frag.fixed) {
                double value[4], out[5];

                for (unsigned k = 0; k < 4; k++) {
                    value[k] = gradient.value[k] +
                               (px + 0.5 - gradient.x) * gradient.dx[k] +
                               tex_row[k];
                }
                if (flat_q) {
                    double d = value[2];

                    frag.texture_rho = flat_ok &&
                        isfinite(value[0] / d) && isfinite(value[1] / d) &&
                        (!tex_nr || isfinite(value[3] / d)) ? flat_rho : NAN;
                } else {
                    frag.texture_rho = sgi_soft_texture_derivatives(value,
                        gradient.dx, gradient.dy, tex_ns, tex_nt, tex_nr,
                        out) ? out[4] : NAN;
                }
                if (r->texture->quad_lod && !r->texture->volume &&
                    isfinite(frag.texture_rho)) {
                    double rho;

                    if (sgi_soft_texture_quad_rho(&gradient, px, py, tex_ns,
                                                  tex_nt, &rho)) {
                        frag.texture_rho = rho;
                    }
                }
            }
            if (background) {
                sgi_soft_stipple_background(r, &frag);
            }
            sgi_soft_fragment(r, px, py, z, offset, depth_w, &frag);
        }
    }
}

/*
 * Transform, clip and rasterize one triangle. It is clipped to the view
 * volume in clip coordinates, before the divide by w (OpenGL 1.2.1,
 * 2.11), and what is left, a convex polygon, is drawn as a fan of
 * triangles from its first vertex, each flat shaded from the triangle's
 * provoking vertex.
 *
 * Approximated: the clipping is OpenGL's, in double arithmetic, not a
 * head's own clipper, whose arithmetic and any guard band are not known.
 * Pixels next to the edges clipping makes, and the attributes at them,
 * may differ from a head's; how a head draws a primitive crossing the
 * near plane would settle it.
 *
 * Under a window matrix there is no clip space of the projection's own,
 * so the clipping is in the window matrix's homogeneous output, the clip
 * space composed with the viewport map, which clipping commutes with.
 * The near and far planes are OpenGL's, window depth 0 and 1, and keep
 * w positive as OpenGL's do. The side planes, which the viewport would
 * place and the window matrix does not say, become a guard band of
 * SGI_GL_WINDOW_GUARD pixels about the drawable's origin, and the
 * window's own pixels and the scissor box bound what is drawn. That
 * matches OpenGL wherever the viewport the client meant covers the
 * window; where a head clips x and y is not known.
 */
static bool sgi_soft_clip_triangle(const SGISoftDraw *r, const float *va,
                                   const float *vb, const float *vc,
                                   SGIVolumeVertex *poly, unsigned *n)
{
    const float *verts[3] = { va, vb, vc };
    SGIVolumeVertex in[3];

    for (int i = 0; i < 3; i++) {
        sgi_volume_from_draw_vertex(r->st, verts[i], &in[i]);
    }
    *n = sgi_volume_clip_triangle(in, poly);
    for (unsigned i = 0; i < *n; i++) {
        if (!sgi_soft_texture_vertex_valid(r, &poly[i])) {
            return false;
        }
    }
    if (r->texture_base) {
        for (unsigned i = 1; i + 1 < *n; i++) {
            const SGIVolumeVertex *vertices[3] = {
                &poly[0], &poly[i], &poly[i + 1],
            };
            SGISoftVertex projected[3];

            for (unsigned j = 0; j < 3; j++) {
                if (!sgi_soft_project(r, vertices[j], &projected[j])) {
                    return false;
                }
            }
            if (!sgi_soft_texture_coefficients(r, projected)) {
                return false;
            }
        }
    }
    return true;
}

/* A quad or polygon validates all its clipped pieces before any are drawn. */
static bool sgi_soft_texture_triangle_valid(const SGISoftDraw *r,
                                           const float *a, const float *b,
                                           const float *c)
{
    SGIVolumeVertex poly[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n;

    return sgi_soft_clip_triangle(r, a, b, c, poly, &n);
}

static void sgi_soft_triangle(const SGISoftDraw *r, const float *va,
                              const float *vb, const float *vc,
                              const float *provoking)
{
    SGIVolumeVertex poly[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n;
    int x0, y0, x1, y1;

    if (!sgi_soft_bounds(r, &x0, &y0, &x1, &y1) ||
        !sgi_soft_clip_triangle(r, va, vb, vc, poly, &n)) {
        return;
    }
    for (unsigned i = 1; i + 1 < n; i++) {
        sgi_soft_clipped_triangle(r, &poly[0], &poly[i], &poly[i + 1],
                                  provoking);
    }
}

unsigned sgi_raster_triangle_pieces(const SGIRasterRules *rules,
                                    const SGIGLDrawState *st,
                                    const float *va, const float *vb,
                                    const float *vc,
                                    SGIRasterTriangle
                                    out[SGI_VOLUME_TRIANGLE_MAX - 2])
{
    SGISoftDraw r = { .rules = rules, .st = st };
    SGIVolumeVertex poly[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n, count = 0;

    if (!sgi_soft_clip_triangle(&r, va, vb, vc, poly, &n)) {
        return 0;
    }
    for (unsigned i = 1; i + 1 < n; i++) {
        const SGIVolumeVertex *verts[3] = {
            &poly[0], &poly[i], &poly[i + 1],
        };
        SGISoftVertex v[3];
        SGIRasterTriangle *t = &out[count];
        int64_t area;
        bool ok = true;

        for (int j = 0; j < 3; j++) {
            ok &= sgi_soft_project(&r, verts[j], &v[j]);
        }
        area = ok ? sgi_soft_edge(&v[0], &v[1], v[2].x, v[2].y) : 0;
        if (!area) {
            continue;
        }
        if (area < 0) {
            /* sgi_soft_clipped_triangle() scans this winding. */
            SGISoftVertex swap = v[1];

            v[1] = v[2];
            v[2] = swap;
        }
        for (int j = 0; j < 3; j++) {
            t->x[j] = v[j].x;
            t->y[j] = v[j].y;
            t->z[j] = v[j].z;
            t->inv_w[j] = v[j].inv_w;
            memcpy(t->rgba[j], v[j].rgba, sizeof(t->rgba[j]));
            memcpy(t->texcoord[j], v[j].texcoord, sizeof(t->texcoord[j]));
        }
        count++;
    }
    return count;
}

bool sgi_raster_triangle_texture_valid(const SGIGLDrawState *st,
                                       const SGIGLTexImage *image,
                                       const float *va, const float *vb,
                                       const float *vc)
{
    const float *verts[3] = { va, vb, vc };
    SGIVolumeVertex in[3], poly[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n;

    for (int i = 0; i < 3; i++) {
        sgi_volume_from_draw_vertex(st, verts[i], &in[i]);
    }
    n = sgi_volume_clip_triangle(in, poly);
    for (unsigned i = 0; i < n; i++) {
        if (!sgi_soft_texcoord_valid(image, NULL, &poly[i])) {
            return false;
        }
    }
    return true;
}

/* Sign of v + e1 * epsilon + e2 * epsilon squared as epsilon -> 0+. */
static int sgi_soft_limit_sign(int64_t v, int64_t e1, int64_t e2)
{
    int64_t first = v ? v : e1 ? e1 : e2;

    return (first > 0) - (first < 0);
}

/* Whether |v + e1*epsilon + e2*epsilon squared| is strictly below bound. */
static bool sgi_soft_limit_abs(int64_t v, int64_t e1, int64_t e2,
                               int64_t bound)
{
    int sign = sgi_soft_limit_sign(v, e1, e2);

    return sgi_soft_limit_sign(sign * v - bound,
                               sign * e1, sign * e2) < 0;
}

/* The endpoint minus (epsilon, epsilon squared) lies in the open diamond. */
static bool sgi_soft_diamond_contains(int64_t x, int64_t y, int64_t half)
{
    int sx = sgi_soft_limit_sign(x, -1, 0);
    int sy = sgi_soft_limit_sign(y, 0, -1);

    return sgi_soft_limit_sign(sx * x + sy * y - half, -sx, -sy) < 0;
}

/*
 * OpenGL 1.2.1, 3.4.1 subtracts (epsilon, epsilon squared) from both
 * endpoints. Lexicographic coefficients implement that limiting perturbation
 * on the snapped grid, including even-width shifts on diamond boundaries.
 * A segment produces a fragment if it enters the open diamond and its final
 * endpoint is outside. The minimum L1 distance occurs at an endpoint or a
 * centre-row/column crossing.
 *
 * Projected coordinates have magnitude at most 2^28 (65536 pixels, at most
 * 12 subpixel bits). The scanner tests base pixels within two pixels of the
 * shifted segment's minor span. Subtracting the identical width shift leaves
 * relative coordinates below 2^30, even for INT32_MAX width. Segment deltas
 * are at most 2^29, so the difference of two products fits signed 64 bits.
 */
static bool sgi_soft_line_diamond_covers(const SGISoftVertex *a,
                                         const SGISoftVertex *b, int64_t px,
                                         int64_t py, int sub)
{
    const int64_t half = 1 << (sub - 1);
    int64_t cx = px * (1 << sub) + half;
    int64_t cy = py * (1 << sub) + half;
    int64_t dx = b->x - a->x, dy = b->y - a->y;
    int64_t ex = cx - a->x, ey = cy - a->y;

    if (sgi_soft_diamond_contains(b->x - cx, b->y - cy, half)) {
        return false;
    }
    if (sgi_soft_diamond_contains(-ex, -ey, half)) {
        return true;
    }
    /* The perturbed column crossing has parameter (ex + epsilon) / dx. */
    if (dx && sgi_soft_limit_sign(ex, 1, 0) * (dx > 0 ? 1 : -1) >= 0 &&
        sgi_soft_limit_sign(dx - ex, -1, 0) * (dx > 0 ? 1 : -1) >= 0 &&
        sgi_soft_limit_abs(dy * ex - ey * dx, dy, -dx,
                           half * llabs(dx))) {
        return true;
    }
    return dy &&
           sgi_soft_limit_sign(ey, 0, 1) * (dy > 0 ? 1 : -1) >= 0 &&
           sgi_soft_limit_sign(dy - ey, 0, -1) * (dy > 0 ? 1 : -1) >= 0 &&
           sgi_soft_limit_abs(dx * ey - ex * dy, -dy, dx,
                              half * llabs(dy));
}

/* floor(n / d) for d > 0. */
static inline int64_t sgi_soft_floor_div(int64_t n, int64_t d)
{
    return n / d - (n % d < 0);
}

/*
 * SGI_LINE_CENTRE_SAMPLED: one pixel per major-axis column (row) whose
 * centre lies between the ends, the start included and the end not, the
 * minor-axis pixel the one holding the segment at that centre, an exact
 * crossing on a pixel edge going to the lower pixel, ceil(v) - 1. On the
 * snapped grid, with the minor coordinate at the centre v = n + t / den
 * exactly: the minor pixel is ceil(v / 2^sub) - 1. Coordinate differences
 * are at most 2^29 (sgi_soft_line_diamond_covers()), so t fits; the minor
 * base, shifted by the width, stays below 2^43.
 */
static bool sgi_soft_line_centre_covers(const SGISoftVertex *a,
                                        const SGISoftVertex *b, int64_t px,
                                        int64_t py, int sub)
{
    const int64_t one = 1 << sub, half = one >> 1;
    bool x_major = llabs(b->x - a->x) >= llabs(b->y - a->y);
    int64_t la = x_major ? a->x : a->y, lb = x_major ? b->x : b->y;
    int64_t na = x_major ? a->y : a->x, nb = x_major ? b->y : b->x;
    int64_t c = (x_major ? px : py) * one + half;
    int64_t den = lb - la, t, v, minor;

    if (den > 0 ? c < la || c >= lb : c > la || c <= lb) {
        return false;
    }
    t = (nb - na) * (c - la);
    if (den < 0) {
        t = -t;
        den = -den;
    }
    /* v = na + floor(t / den) plus a fraction that is zero or not. */
    v = na + sgi_soft_floor_div(t, den);
    minor = t % den ? sgi_soft_floor_div(v, one) :
                      -sgi_soft_floor_div(-v, one) - 1;
    return (x_major ? py : px) == minor;
}

static bool sgi_soft_line_covers(const SGIRasterRules *rules,
                                 const SGISoftVertex *a,
                                 const SGISoftVertex *b, int64_t px,
                                 int64_t py, int sub)
{
    return rules->line_rule == SGI_LINE_CENTRE_SAMPLED ?
           sgi_soft_line_centre_covers(a, b, px, py, sub) :
           sgi_soft_line_diamond_covers(a, b, px, py, sub);
}

/*
 * Transform, clip, map and rasterize one aliased segment (OpenGL 1.2.1,
 * 3.4.1) with the provoking vertex's colour when flat shaded. The segment
 * is clipped to the view volume in clip coordinates (2.11), approximated
 * as a triangle is. A fragment's depth and smooth colour come from t, the
 * centre's projection onto the clipped segment (equations 3.5 to 3.7),
 * the colour corrected for perspective. The fragments are also bounded by
 * the viewport, when there is one. Width follows the minor-axis shift and
 * replication of 3.4.3. Polygon boundaries count stipple in submitted
 * direction; activated standalone lines count across streamed segments.
 * A smoothed segment takes the coverage of the rules' smooth_rule
 * (3.4.2), stippled as one-pixel pieces along its length.
 */
typedef struct SGISoftPoint2 {
    double x, y;
} SGISoftPoint2;

static unsigned sgi_soft_clip_edge(const SGISoftPoint2 *in, unsigned n,
                                   SGISoftPoint2 *out, unsigned axis,
                                   double bound, bool greater)
{
    unsigned count = 0;

    for (unsigned i = 0; i < n; i++) {
        SGISoftPoint2 a = in[i], b = in[(i + 1) % n];
        double av = axis ? a.y : a.x, bv = axis ? b.y : b.x;
        bool ai = greater ? av >= bound : av <= bound;
        bool bi = greater ? bv >= bound : bv <= bound;

        if (ai != bi) {
            double t = (bound - av) / (bv - av);
            SGISoftPoint2 p = { a.x + t * (b.x - a.x),
                                a.y + t * (b.y - a.y) };

            if (axis) {
                p.y = bound;
            } else {
                p.x = bound;
            }
            out[count++] = p;
        }
        if (bi) {
            out[count++] = b;
        }
    }
    return count;
}

static double sgi_soft_polygon_square_area(const SGISoftPoint2 *polygon,
                                          unsigned n, int x, int y)
{
    SGISoftPoint2 a[12], b[12];
    double area = 0;

    memcpy(a, polygon, n * sizeof(*a));
    n = sgi_soft_clip_edge(a, n, b, 0, x, true);
    n = sgi_soft_clip_edge(b, n, a, 0, x + 1, false);
    n = sgi_soft_clip_edge(a, n, b, 1, y, true);
    n = sgi_soft_clip_edge(b, n, a, 1, y + 1, false);
    for (unsigned i = 0; i < n; i++) {
        const SGISoftPoint2 *p = &a[i], *q = &a[(i + 1) % n];

        area += p->x * q->y - p->y * q->x;
    }
    return MIN(1.0, fabs(area) * 0.5);
}

static double sgi_soft_line_coverage(double ax, double ay, double bx,
                                     double by, double width, int x, int y)
{
    double dx = bx - ax, dy = by - ay, length = hypot(dx, dy);
    double nx, ny;
    SGISoftPoint2 rectangle[4];

    if (!(length > 0.0) || !(width > 0.0)) {
        return 0;
    }
    nx = -dy * width / (2 * length);
    ny = dx * width / (2 * length);
    rectangle[0] = (SGISoftPoint2) { ax + nx, ay + ny };
    rectangle[1] = (SGISoftPoint2) { bx + nx, by + ny };
    rectangle[2] = (SGISoftPoint2) { bx - nx, by - ny };
    rectangle[3] = (SGISoftPoint2) { ax - nx, ay - ny };
    return sgi_soft_polygon_square_area(rectangle, ARRAY_SIZE(rectangle), x, y);
}

static double sgi_soft_circle_integral(double x, double radius)
{
    double y = sqrt(MAX(0.0, radius * radius - x * x));

    return 0.5 * (x * y + radius * radius * asin(x / radius));
}

static double sgi_soft_circle_coverage(double cx, double cy, double radius,
                                       int x, int y)
{
    double cuts[8], area = 0;
    unsigned n = 0;
    double lo = MAX(x - cx, -radius), hi = MIN(x + 1 - cx, radius);

    if (!(lo < hi) || !(radius > 0.0)) {
        return 0;
    }
    cuts[n++] = lo;
    cuts[n++] = hi;
    for (int edge = 0; edge < 2; edge++) {
        double yedge = (edge ? y + 1 : y) - cy;

        if (fabs(yedge) < radius) {
            double root = sqrt(radius * radius - yedge * yedge);

            if (-root > lo && -root < hi) {
                cuts[n++] = -root;
            }
            if (root > lo && root < hi) {
                cuts[n++] = root;
            }
        }
    }
    for (unsigned i = 1; i < n; i++) {
        double value = cuts[i];
        unsigned j = i;

        while (j && cuts[j - 1] > value) {
            cuts[j] = cuts[j - 1];
            j--;
        }
        cuts[j] = value;
    }
    for (unsigned i = 0; i + 1 < n; i++) {
        double a = cuts[i], b = cuts[i + 1], mid = (a + b) * 0.5;
        double circle = sqrt(MAX(0.0, radius * radius - mid * mid));
        double ymin = y - cy, ymax = y + 1 - cy;
        double top = MIN(ymax, circle), bottom = MAX(ymin, -circle);
        double arc = sgi_soft_circle_integral(b, radius) -
                     sgi_soft_circle_integral(a, radius);

        if (top <= bottom) {
            continue;
        }
        area += (top == circle ? arc : ymax * (b - a)) -
                (bottom == -circle ? -arc : ymin * (b - a));
    }
    return MIN(1.0, MAX(0.0, area));
}

/*
 * SGI_SMOOTH_TWO_PIXEL_FILTER's weight in one axis for a fragment whose
 * centre is d from a footprint of the given size: a box of that size
 * filtered by a one-pixel tent, 1 - |d| at size 1.
 */
static double sgi_soft_filter_weight(double size, double d)
{
    return MIN(1.0, MAX(0.0, (size + 1) * 0.5 - fabs(d)));
}

/*
 * SGI_SMOOTH_TWO_PIXEL_FILTER's point weight: the product of the weights
 * in x and y, at size 1 bilinear over the 2x2 pixels whose centres
 * surround the point.
 */
static double sgi_soft_point_filter(double cx, double cy, double size,
                                    int x, int y)
{
    return sgi_soft_filter_weight(size, cx - (x + 0.5)) *
           sgi_soft_filter_weight(size, cy - (y + 0.5));
}

/*
 * SGI_SMOOTH_TWO_PIXEL_FILTER's line weight: the segment's part of the
 * fragment's major-axis column, times the weight in the minor axis of the
 * fragment's centre at d from the line at the middle of that part, scaled
 * for slope and clamped.
 */
static double sgi_soft_line_filter(double ax, double ay, double bx,
                                   double by, double width, int x, int y)
{
    bool x_major = fabs(bx - ax) >= fabs(by - ay);
    double l0 = x_major ? ax : ay, l1 = x_major ? bx : by;
    double n0 = x_major ? ay : ax, n1 = x_major ? by : bx;
    int m = x_major ? x : y, n = x_major ? y : x;
    double lo, hi, slope, minor, d;

    if (!(l1 != l0)) {
        return 0;
    }
    if (l1 < l0) {
        double t = l0;

        l0 = l1;
        l1 = t;
        t = n0;
        n0 = n1;
        n1 = t;
    }
    lo = MAX(l0, m);
    hi = MIN(l1, m + 1.0);
    if (!(hi > lo)) {
        return 0;
    }
    slope = (n1 - n0) / (l1 - l0);
    minor = n0 + slope * ((lo + hi) * 0.5 - l0);
    d = minor - (n + 0.5);
    return MIN(1.0, sgi_soft_filter_weight(width, d) *
                    sqrt(1 + slope * slope)) * (hi - lo);
}

double sgi_raster_point_coverage(const SGIRasterRules *r, double cx,
                                 double cy, double size, int x, int y)
{
    if (r->smooth_rule == SGI_SMOOTH_TWO_PIXEL_FILTER) {
        return sgi_soft_point_filter(cx, cy, size, x, y);
    }
    return sgi_soft_circle_coverage(cx, cy, size * 0.5, x, y);
}

double sgi_raster_line_coverage(const SGIRasterRules *r, double ax,
                                double ay, double bx, double by,
                                double width, int x, int y)
{
    if (r->smooth_rule == SGI_SMOOTH_TWO_PIXEL_FILTER) {
        return sgi_soft_line_filter(ax, ay, bx, by, width, x, y);
    }
    return sgi_soft_line_coverage(ax, ay, bx, by, width, x, y);
}

/*
 * How far from a smoothed point's centre, or a smoothed line, in pixels,
 * a fragment can have coverage.
 */
static double sgi_soft_smooth_reach(const SGIRasterRules *r, double size)
{
    return r->smooth_rule == SGI_SMOOTH_TWO_PIXEL_FILTER ? (size + 1) * 0.5 :
                                                           size * 0.5;
}

typedef struct SGISoftLineStipple {
    uint32_t counter;
    uint32_t repeat, pattern;
    bool enabled, consumed;
} SGISoftLineStipple;

static void sgi_soft_line_vertex_at(const SGISoftVertex *a,
                                    const SGISoftVertex *b, double t,
                                    SGISoftVertex *out)
{
    double wa = (1 - t) * a->inv_w, wb = t * b->inv_w;
    double sum = wa + wb;

    out->x = llround(a->x + t * (b->x - a->x));
    out->y = llround(a->y + t * (b->y - a->y));
    out->z = a->z + t * (b->z - a->z);
    out->inv_w = (1 - t) * a->inv_w + t * b->inv_w;
    out->depth_w = (1 - t) * a->depth_w + t * b->depth_w;
    for (int k = 0; k < 4; k++) {
        out->rgba[k] = (wa * a->rgba[k] + wb * b->rgba[k]) / sum;
        out->texcoord[k] = (wa * a->texcoord[k] + wb * b->texcoord[k]) / sum;
    }
    out->index = (wa * a->index + wb * b->index) / sum;
    out->fog = (wa * a->fog + wb * b->fog) / sum;
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        out->clip[i] = (wa * a->clip[i] + wb * b->clip[i]) / sum;
    }
}

static void sgi_soft_line_projected(const SGISoftDraw *r,
                                    const SGISoftVertex *pa,
                                    const SGISoftVertex *pb,
                                    const float *provoking, double offset,
                                    SGISoftLineStipple *stipple,
                                    bool client_clip)
{
    const SGIGLDrawState *st = r->st;
    bool smooth = st->shade_model != SGI_GL_FLAT;
    const int sub = r->rules->subpixel_bits;
    bool x_major;
    SGISoftVertex a = *pa, b = *pb, shifted_a, shifted_b;
    bool visible;
    int step;
    double flat[4], len2;
    double texture_value[4], texture_delta[4], texture_length;
    int major0, major1, bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    visible = sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1);
    if ((!visible && !stipple) || (a.x == b.x && a.y == b.y)) {
        return;
    }
    if (r->line_smooth && stipple && !stipple->enabled) {
        double scale = 1 << sub;
        double length = hypot((b.x - a.x) / scale, (b.y - a.y) / scale);
        uint32_t period = 16 * stipple->repeat;

        /*
         * Unstippled, the segment is one rectangle (3.4.2): its fragments
         * are not split between one-pixel pieces, each blended on its own.
         * The count still moves on by the pieces a stipple would take.
         */
        stipple->counter = (stipple->counter +
                            (uint32_t)fmod(ceil(length), period)) % period;
        stipple->consumed = true;
        if (!visible) {
            return;
        }
        stipple = NULL;
    }
    if (r->line_smooth && stipple) {
        double scale = 1 << sub;
        double length = hypot((b.x - a.x) / scale, (b.y - a.y) / scale);
        double segments = ceil(length);

        for (double i = 0; i < segments; i++) {
            unsigned bit = (stipple->counter / stipple->repeat) % 16;
            bool keep = !stipple->enabled ||
                        (stipple->pattern & (1u << bit));

            stipple->counter = (stipple->counter + 1) %
                               (16 * stipple->repeat);
            stipple->consumed = true;
            if (keep) {
                SGISoftVertex start, end;
                double t0 = i / length, t1 = MIN((i + 1) / length, 1.0);

                sgi_soft_line_vertex_at(&a, &b, t0, &start);
                sgi_soft_line_vertex_at(&a, &b, t1, &end);
                sgi_soft_line_projected(r, &start, &end, provoking, offset,
                                        NULL, client_clip);
            }
        }
        return;
    }
    if (r->texture_base && !sgi_soft_texture_line_gradient(&a, &b, sub,
            r->texture->volume, r->texture->one_dimensional,
            texture_value, texture_delta, &texture_length)) {
        sgi_soft_texture_report(r, 16, "invalid primitive coefficients, "
                                      "primitive dropped");
        return;
    }
    for (int k = 0; k < 4; k++) {
        flat[k] = provoking[SGI_GL_V_COLOR + k];
    }
    len2 = (double)(b.x - a.x) * (b.x - a.x) +
           (double)(b.y - a.y) * (b.y - a.y);
    x_major = llabs(b.x - a.x) >= llabs(b.y - a.y);
    shifted_a = a;
    shifted_b = b;
    /* Width <= INT32_MAX and sub <= 12 give a shift below 2^42. */
    if (x_major) {
        shifted_a.y -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
        shifted_b.y -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
    } else {
        shifted_a.x -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
        shifted_b.x -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
    }

    /*
     * Walk the major axis a pixel at a time, and in each step try the
     * minor-axis pixels within one of those the segment spans there.
     */
    major0 = (x_major ? MIN(a.x, b.x) : MIN(a.y, b.y)) >>
             sub;
    major1 = (x_major ? MAX(a.x, b.x) : MAX(a.y, b.y)) >>
             sub;
    major0--;
    major1++;
    if (r->line_smooth) {
        double pad = sgi_soft_smooth_reach(r->rules, r->smooth_line_width);
        double l0 = x_major ? a.x : a.y;
        double l1 = x_major ? b.x : b.y;

        major0 = (int)floor(MIN(l0, l1) / (1 << sub) - pad) - 1;
        major1 = (int)ceil(MAX(l0, l1) / (1 << sub) + pad) + 1;
    }
    step = 1;
    if (stipple) {
        /* Count coverage before ownership/scissor, in submitted direction. */
        if (x_major ? b.x < a.x : b.y < a.y) {
            int tmp = major0;

            major0 = major1;
            major1 = tmp;
            step = -1;
        }
    } else {
        major0 = MAX(major0, x_major ? bx0 : by0);
        major1 = MIN(major1, x_major ? bx1 : by1);
    }
    for (int m = major0; step > 0 ? m <= major1 : m >= major1; m += step) {
        const SGISoftVertex *lo = x_major ?
            (a.x <= b.x ? &shifted_a : &shifted_b) :
            (a.y <= b.y ? &shifted_a : &shifted_b);
        const SGISoftVertex *hi = lo == &shifted_a ? &shifted_b : &shifted_a;
        double l0 = x_major ? lo->x : lo->y, l1 = x_major ? hi->x : hi->y;
        double n0 = x_major ? lo->y : lo->x, n1 = x_major ? hi->y : hi->x;
        double e0 = MAX((double)m * (1 << sub), l0);
        double e1 = MIN((double)(m + 1) * (1 << sub), l1);
        double f0, f1;
        int64_t minor0, minor1;

        if (l1 > l0) {
            e0 = MIN(e0, l1);
            e1 = MAX(e1, l0);
            f0 = n0 + (n1 - n0) * (e0 - l0) / (l1 - l0);
            f1 = n0 + (n1 - n0) * (e1 - l0) / (l1 - l0);
        } else {
            f0 = n0;
            f1 = n1;
        }
        minor0 = (int64_t)floor(MIN(f0, f1) / (1 << sub)) - 1;
        minor1 = (int64_t)floor(MAX(f0, f1) / (1 << sub)) + 1;
        if (r->line_smooth) {
            double pad = sgi_soft_smooth_reach(r->rules,
                                               r->smooth_line_width);
            double q0 = x_major ? a.y : a.x;
            double q1 = x_major ? b.y : b.x;

            minor0 = (int64_t)floor(MIN(q0, q1) / (1 << sub) - pad) - 1;
            minor1 = (int64_t)ceil(MAX(q0, q1) / (1 << sub) + pad) + 1;
        }
        for (int64_t n = minor0; n <= minor1; n++) {
            int64_t base_x = x_major ? m : n, base_y = x_major ? n : m;
            int64_t replica0 = MAX(n, x_major ? by0 : bx0);
            int64_t replica1 = r->line_smooth ? replica0 :
                MIN(n + r->line_width - 1, x_major ? by1 : bx1);

            bool keep = true;

            double coverage = r->line_smooth ?
                sgi_raster_line_coverage(r->rules, (double)a.x / (1 << sub),
                    (double)a.y / (1 << sub), (double)b.x / (1 << sub),
                    (double)b.y / (1 << sub), r->smooth_line_width,
                    base_x, base_y) :
                sgi_soft_line_covers(r->rules, &shifted_a, &shifted_b,
                                     base_x, base_y, sub) ? 1 : 0;

            if (!(coverage > 0)) {
                continue;
            }
            if (stipple) {
                unsigned bit = (stipple->counter / stipple->repeat) % 16;

                keep = !stipple->enabled ||
                       (stipple->pattern & (1u << bit));
                stipple->counter = (stipple->counter + 1) %
                                   (16 * stipple->repeat);
                stipple->consumed = true;
            }
            if (!keep || !visible || replica0 > replica1 ||
                m < (x_major ? bx0 : by0) || m > (x_major ? bx1 : by1)) {
                continue;
            }
            /* Visit only drawable-visible replicas, never width entries. */
            for (int64_t replica = replica0; replica <= replica1; replica++) {
                int px = x_major ? m : replica, py = x_major ? replica : m;
                double cx = (px + 0.5) * (1 << sub);
                double cy = (py + 0.5) * (1 << sub);
                double t, wa, wb;
                SGISoftFrag frag;

                frag.fixed = false;
                t = ((cx - a.x) * (b.x - a.x) +
                     (cy - a.y) * (b.y - a.y)) / len2;
                /* Approximation: retain endpoint values, not extrapolation. */
                t = MIN(MAX(t, 0.0), 1.0);
                wa = (1 - t) * a.inv_w;
                wb = t * b.inv_w;
                frag.index = smooth ? a.index + wb * (b.index - a.index) /
                                      (wa + wb) :
                             provoking[SGI_GL_V_INDEX];
                for (int k = 0; k < 4; k++) {
                    frag.texcoord[k] = (wa * a.texcoord[k] +
                                        wb * b.texcoord[k]) / (wa + wb);
                }
                frag.fog = (wa * a.fog + wb * b.fog) / (wa + wb);
                if (st->clip_planes && client_clip) {
                    double d[SGI_GL_CLIP_PLANES];

                    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
                        d[i] = (wa * a.clip[i] + wb * b.clip[i]) / (wa + wb);
                    }
                    if (sgi_soft_clipped(st, d)) {
                        continue;
                    }
                }
                for (int k = 0; k < 4; k++) {
                    frag.rgba[k] = smooth ?
                    (wa * a.rgba[k] + wb * b.rgba[k]) / (wa + wb) :
                                   flat[k];
                }
                if (r->texture_base) {
                    double value[4], out[5], zero[4] = { 0 };

                    for (unsigned k = 0; k < 4; k++) {
                        value[k] = texture_value[k] + t * texture_delta[k];
                    }
                    frag.texture_rho = sgi_soft_texture_derivatives(value,
                        texture_delta, zero, r->texture_base->width *
                        sgi_soft_texture_lod_scale(r->texture->lod_scale_s),
                        r->texture->one_dimensional ? 0 :
                        r->texture_base->height *
                        sgi_soft_texture_lod_scale(r->texture->lod_scale_t),
                        r->texture->volume ? r->texture_base->depth *
                        sgi_soft_texture_lod_scale(r->texture->lod_scale_r) : 0,
                        out) ?
                        out[4] / texture_length : NAN;
                }
                sgi_soft_fragment_coverage(r, px, py,
                    a.z + t * (b.z - a.z), offset,
                    (1 - t) * a.depth_w + t * b.depth_w, &frag, coverage,
                    r->line_smooth);
            }
        }
    }
}

/* Keep standalone client-plane rejection after geometric counting. */
static void sgi_soft_line(const SGISoftDraw *r, const float *va,
                          const float *vb, const float *provoking,
                          SGISoftLineStipple *stipple)
{
    SGIVolumeVertex ca, cb;
    SGISoftVertex a, b;
    int bx0, by0, bx1, by1;

    if (!stipple && !sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1)) {
        return;
    }
    sgi_volume_from_draw_vertex(r->st, va, &ca);
    sgi_volume_from_draw_vertex(r->st, vb, &cb);
    if (sgi_volume_clip_segment(&ca, &cb) &&
        sgi_soft_project(r, &ca, &a) && sgi_soft_project(r, &cb, &b)) {
        sgi_soft_line_projected(r, &a, &b, provoking, 0, stipple, true);
    }
}

bool sgi_raster_line_span(const SGIRasterRules *rules,
                          const SGIGLDrawState *st, const float *va,
                          const float *vb, uint32_t width,
                          SGIRasterLineSpan *out)
{
    SGISoftDraw r = { .rules = rules, .st = st };
    SGIVolumeVertex ca, cb;
    SGISoftVertex a, b, sa, sb;
    const int sub = rules->subpixel_bits;
    bool x_major;
    int64_t major0, major1, shift;

    *out = (SGIRasterLineSpan) { .exact = true, .lo = 1, .hi = 0 };
    sgi_volume_from_draw_vertex(st, va, &ca);
    sgi_volume_from_draw_vertex(st, vb, &cb);
    if (!sgi_volume_clip_segment(&ca, &cb)) {
        return true;
    }
    if (!sgi_soft_project(&r, &ca, &a) || !sgi_soft_project(&r, &cb, &b)) {
        return false;
    }
    out->ax = a.x;
    out->ay = a.y;
    out->bx = b.x;
    out->by = b.y;
    out->a_inv_w = a.inv_w;
    out->b_inv_w = b.inv_w;
    memcpy(out->a_rgba, a.rgba, sizeof(out->a_rgba));
    memcpy(out->b_rgba, b.rgba, sizeof(out->b_rgba));
    if (a.x == b.x && a.y == b.y) {
        return true;
    }
    /* The same shift, major range and minor candidates as the scan. */
    x_major = llabs(b.x - a.x) >= llabs(b.y - a.y);
    out->x_major = x_major;
    shift = (int64_t)(MAX(width, 1) - 1) * (1 << (sub - 1));
    sa = a;
    sb = b;
    if (x_major) {
        sa.y -= shift;
        sb.y -= shift;
    } else {
        sa.x -= shift;
        sb.x -= shift;
    }
    major0 = ((x_major ? MIN(a.x, b.x) : MIN(a.y, b.y)) >> sub) - 1;
    major1 = ((x_major ? MAX(a.x, b.x) : MAX(a.y, b.y)) >> sub) + 1;
    for (int64_t m = major0; m <= major1; m++) {
        const SGISoftVertex *lo = x_major ? (a.x <= b.x ? &sa : &sb) :
                                            (a.y <= b.y ? &sa : &sb);
        const SGISoftVertex *hi = lo == &sa ? &sb : &sa;
        double l0 = x_major ? lo->x : lo->y, l1 = x_major ? hi->x : hi->y;
        double n0 = x_major ? lo->y : lo->x, n1 = x_major ? hi->y : hi->x;
        double e0 = MAX((double)m * (1 << sub), l0);
        double e1 = MIN((double)(m + 1) * (1 << sub), l1);
        double f0, f1;
        unsigned count = 0;

        if (l1 > l0) {
            e0 = MIN(e0, l1);
            e1 = MAX(e1, l0);
            f0 = n0 + (n1 - n0) * (e0 - l0) / (l1 - l0);
            f1 = n0 + (n1 - n0) * (e1 - l0) / (l1 - l0);
        } else {
            f0 = n0;
            f1 = n1;
        }
        for (int64_t n = (int64_t)floor(MIN(f0, f1) / (1 << sub)) - 1;
             n <= (int64_t)floor(MAX(f0, f1) / (1 << sub)) + 1; n++) {
            count += sgi_soft_line_covers(rules, &sa, &sb, x_major ? m : n,
                                          x_major ? n : m, sub);
        }
        if (!count) {
            continue;
        }
        if (!out->covered) {
            out->covered = true;
            out->lo = m;
        } else if (m != out->hi + 1) {
            out->exact = false;
        }
        out->exact &= count == 1;
        out->hi = m;
    }
    return true;
}

/*
 * Aliased square-point centring follows OpenGL 1.2.1, 3.3, on the shared
 * snapped subpixel grid rather than ideal unsnapped coordinates. A point
 * outside the view volume is dropped (2.11), and so is one outside the
 * viewport, when there is one, which only a point on the volume's edge
 * can be. A smoothed point takes the coverage of the rules' smooth_rule
 * about its snapped centre (3.3.2).
 */
static void sgi_soft_point_projected(const SGISoftDraw *r,
                                     const SGISoftVertex *pv,
                                     const float *provoking, double offset)
{
    const int sub = r->rules->subpixel_bits;
    SGISoftVertex v = *pv;
    SGISoftFrag frag;
    int bx0, by0, bx1, by1;
    int64_t px, py, x0, y0, x1, y1;

    if (!sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1)) {
        return;
    }
    /* Even dimensions centre on a pixel corner; odd ones on a centre. */
    px = (v.x + (r->point_size % 2 ? 0 : 1 << (sub - 1))) >> sub;
    py = (v.y + (r->point_size % 2 ? 0 : 1 << (sub - 1))) >> sub;
    x0 = MAX(px - r->point_size / 2, bx0);
    y0 = MAX(py - r->point_size / 2, by0);
    x1 = MIN(px - r->point_size / 2 + r->point_size - 1, bx1);
    y1 = MIN(py - r->point_size / 2 + r->point_size - 1, by1);
    memcpy(frag.rgba, v.rgba, sizeof(frag.rgba));
    frag.index = v.index;
    frag.fixed = false;
    if (provoking && r->st->shade_model == SGI_GL_FLAT) {
        frag.index = provoking[SGI_GL_V_INDEX];
        for (unsigned k = 0; k < 4; k++) {
            frag.rgba[k] = provoking[SGI_GL_V_COLOR + k];
        }
    }
    frag.fog = v.fog;
    frag.texture_rho = 1;
    memcpy(frag.texcoord, v.texcoord, sizeof(frag.texcoord));
    if (r->point_smooth) {
        double scale = 1 << sub;
        double cx = v.x / scale, cy = v.y / scale;
        double radius = sgi_soft_smooth_reach(r->rules,
                                              r->smooth_point_size);

        x0 = MAX((int64_t)floor(cx - radius), bx0);
        y0 = MAX((int64_t)floor(cy - radius), by0);
        x1 = MIN((int64_t)ceil(cx + radius) - 1, bx1);
        y1 = MIN((int64_t)ceil(cy + radius) - 1, by1);
        for (int64_t y = y0; y <= y1; y++) {
            for (int64_t x = x0; x <= x1; x++) {
                double coverage = sgi_raster_point_coverage(r->rules, cx, cy,
                                                    r->smooth_point_size,
                                                    x, y);

                sgi_soft_fragment_coverage(r, x, y, v.z, offset, v.depth_w,
                                           &frag, coverage, true);
            }
        }
        return;
    }
    for (int64_t y = y0; y <= y1; y++) {
        for (int64_t x = x0; x <= x1; x++) {
            sgi_soft_fragment(r, x, y, v.z, offset, v.depth_w, &frag);
        }
    }
}

static void sgi_soft_point(const SGISoftDraw *r, const float *vp)
{
    SGIVolumeVertex cv;
    SGISoftVertex v;

    sgi_volume_from_draw_vertex(r->st, vp, &cv);
    if (sgi_volume_contains(&cv) && sgi_soft_project(r, &cv, &v) &&
        !sgi_soft_clipped(r->st, v.clip)) {
        sgi_soft_point_projected(r, &v, NULL, 0);
    }
}

/* Internal representation bound, not an SGI width range. Half ties round up. */
static bool sgi_soft_dimension(float requested, uint32_t *rounded)
{
    double n = requested == 0 ? 1 : floor((double)requested + 0.5);

    if (!isfinite(requested) || requested < 0 || n > INT32_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-gl: unsupported aliased dimension %g\n", requested);
        return false;
    }
    *rounded = MAX(n, 1);
    return true;
}


static bool sgi_soft_boundary_edge(const SGIVolumeVertex *v)
{
    uint32_t edge;

    memcpy(&edge, &v->attr[SGI_GL_V_EDGE], sizeof(edge));
    return edge != 0;
}

static void sgi_soft_boundary_set_edge(SGIVolumeVertex *v, bool enabled)
{
    uint32_t edge = enabled;

    memcpy(&v->attr[SGI_GL_V_EDGE], &edge, sizeof(edge));
}

static double sgi_soft_boundary_plane(const SGISoftDraw *r,
                                      const SGIVolumeVertex *v, unsigned p)
{
    if (p < SGI_VOLUME_PLANES) {
        return sgi_volume_plane(v, p);
    } else {
        const float *plane = r->st->clip_plane[p - SGI_VOLUME_PLANES];

        return plane[0] * v->eye[0] + plane[1] * v->eye[1] +
               plane[2] * v->eye[2] + plane[3] * v->eye[3];
    }
}

/*
 * Carry outgoing flags through complete polygon clipping (2.11). Original
 * retained edges keep their flags; the newly connecting cut edges are true.
 * Exact-plane endpoints avoid duplicate intersections without merging distinct
 * submitted records, whose coincident positions still matter in POINT mode.
 */
static bool sgi_soft_boundary_clip(const SGISoftDraw *r,
                                   SGIVolumeVertex **poly,
                                   SGIVolumeVertex *other, size_t *count,
                                   size_t capacity)
{
    SGIVolumeVertex *src = *poly, *dst = other, *tmp;
    size_t n = *count;

    for (unsigned p = 0; p < SGI_VOLUME_PLANES + SGI_GL_CLIP_PLANES; p++) {
        size_t m = 0;

        if (p >= SGI_VOLUME_PLANES &&
            !(r->st->clip_planes & (1u << (p - SGI_VOLUME_PLANES)))) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            const SGIVolumeVertex *a = &src[i], *b = &src[(i + 1) % n];
            double da = sgi_soft_boundary_plane(r, a, p);
            double db = sgi_soft_boundary_plane(r, b, p);

            if (da >= 0) {
                if (m == capacity) {
                    return false;
                }
                dst[m++] = *a;
                if (!(db >= 0)) {
                    if (da == 0) {
                        sgi_soft_boundary_set_edge(&dst[m - 1], true);
                    } else {
                        if (m == capacity) {
                            return false;
                        }
                        sgi_volume_cut(a, da, b, db, &dst[m]);
                        sgi_soft_boundary_set_edge(&dst[m++], true);
                    }
                }
            } else if (db > 0) {
                if (m == capacity) {
                    return false;
                }
                sgi_volume_cut(b, db, a, da, &dst[m]);
                sgi_soft_boundary_set_edge(&dst[m++],
                                            sgi_soft_boundary_edge(a));
            }
        }
        n = m;
        tmp = src, src = dst, dst = tmp;
        if (!n) {
            break;
        }
    }
    *poly = src;
    *count = n;
    return true;
}

static const float *sgi_soft_polygon_vertex(const SGIGLDraw *d,
                                            const uint32_t *indices, size_t i)
{
    size_t index = indices ? indices[i] : i;

    return &d->vertices[index * SGI_GL_VERTEX_FLOATS];
}

typedef struct SGISoftAAPoint {
    double x, y;
} SGISoftAAPoint;

typedef enum SGISoftAAEdge {
    SGI_AA_LEFT,
    SGI_AA_RIGHT,
    SGI_AA_BOTTOM,
    SGI_AA_TOP,
} SGISoftAAEdge;

static bool sgi_soft_aa_inside(const SGISoftAAPoint *p, SGISoftAAEdge edge,
                               double value)
{
    switch (edge) {
    case SGI_AA_LEFT:   return p->x >= value;
    case SGI_AA_RIGHT:  return p->x <= value;
    case SGI_AA_BOTTOM: return p->y >= value;
    case SGI_AA_TOP:    return p->y <= value;
    }
    g_assert_not_reached();
}

static SGISoftAAPoint sgi_soft_aa_intersection(const SGISoftAAPoint *a,
                                               const SGISoftAAPoint *b,
                                               SGISoftAAEdge edge,
                                               double value)
{
    double t = edge <= SGI_AA_RIGHT ? (value - a->x) / (b->x - a->x) :
                                     (value - a->y) / (b->y - a->y);

    return (SGISoftAAPoint) {
        .x = a->x + t * (b->x - a->x),
        .y = a->y + t * (b->y - a->y),
    };
}

/* Clip a convex window-space polygon to one side of a pixel square. */
static bool sgi_soft_aa_clip(const SGISoftAAPoint *src, size_t n,
                             SGISoftAAPoint *dst, size_t capacity,
                             SGISoftAAEdge edge, double value, size_t *out_n)
{
    size_t m = 0;
    const SGISoftAAPoint *a;

    if (!n) {
        *out_n = 0;
        return true;
    }
    a = &src[n - 1];
    for (size_t i = 0; i < n; i++) {
        const SGISoftAAPoint *b = &src[i];
        bool ai = sgi_soft_aa_inside(a, edge, value);
        bool bi = sgi_soft_aa_inside(b, edge, value);

        if (ai != bi) {
            if (m == capacity) {
                return false;
            }
            dst[m++] = sgi_soft_aa_intersection(a, b, edge, value);
        }
        if (bi) {
            if (m == capacity) {
                return false;
            }
            dst[m++] = *b;
        }
        a = b;
    }
    *out_n = m;
    return true;
}

static double sgi_soft_aa_edge(const SGISoftVertex *a,
                               const SGISoftVertex *b,
                               double x, double y)
{
    return (b->x - a->x) * (y - a->y) -
           (b->y - a->y) * (x - a->x);
}

/* A convex polygon contains the square when it contains all four corners. */
static bool sgi_soft_aa_contains_pixel(const SGISoftVertex *v, size_t n,
                                       int px, int py, double scale,
                                       bool ccw)
{
    for (size_t i = 0; i < n; i++) {
        const SGISoftVertex *a = &v[i];
        const SGISoftVertex *b = &v[(i + 1) % n];
        double x = px * scale, y = py * scale;

        if (ccw) {
            if (sgi_soft_aa_edge(a, b, x, y) < 0 ||
                sgi_soft_aa_edge(a, b, x + scale, y) < 0 ||
                sgi_soft_aa_edge(a, b, x, y + scale) < 0 ||
                sgi_soft_aa_edge(a, b, x + scale, y + scale) < 0) {
                return false;
            }
        } else if (sgi_soft_aa_edge(a, b, x, y) > 0 ||
                   sgi_soft_aa_edge(a, b, x + scale, y) > 0 ||
                   sgi_soft_aa_edge(a, b, x, y + scale) > 0 ||
                   sgi_soft_aa_edge(a, b, x + scale, y + scale) > 0) {
            return false;
        }
    }
    return true;
}

/*
 * Rasterize one clipped convex polygon once. Pixel-square clipping computes
 * geometric coverage; centroid sampling is exact for affine datums and the
 * documented approximation for perspective-correct varyings.
 */
static void sgi_soft_polygon_coverage(const SGISoftDraw *r,
                                      const SGISoftVertex *v, size_t n,
                                      const float flat_color[4],
                                      double flat_index,
                                      double offset)
{
    SGISoftAAPoint *a = NULL, *b = NULL;
    SGISoftAAPoint *tmp;
    SGISoftFrag flat = { 0 };
    int px0, px1, py0, py1;
    int sub = r->rules->subpixel_bits;
    double scale = 1 << sub;
    int64_t min_x, max_x, min_y, max_y;
    double polygon_area = 0;
    size_t capacity;

    if (n < 3 || n > SIZE_MAX - 8 || !sgi_soft_bounds(r, &px0, &py0,
                                                      &px1, &py1)) {
        return;
    }
    capacity = n + 8;
    a = g_try_new(SGISoftAAPoint, capacity);
    b = g_try_new(SGISoftAAPoint, capacity);
    if (!a || !b) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: polygon coverage workspace "
                      "allocation failed; primitive dropped\n");
        goto out;
    }
    min_x = max_x = v[0].x;
    min_y = max_y = v[0].y;
    for (size_t i = 0; i < n; i++) {
        const SGISoftVertex *next = &v[(i + 1) % n];

        min_x = MIN(min_x, v[i].x);
        max_x = MAX(max_x, v[i].x);
        min_y = MIN(min_y, v[i].y);
        max_y = MAX(max_y, v[i].y);
        polygon_area += v[i].x * next->y - next->x * v[i].y;
    }
    if (!(fabs(polygon_area) > 0)) {
        goto out;
    }
    px0 = MAX(px0, (int)(min_x >> sub));
    px1 = MIN(px1, (int)(max_x >> sub));
    py0 = MAX(py0, (int)(min_y >> sub));
    py1 = MIN(py1, (int)(max_y >> sub));
    for (unsigned k = 0; k < 4; k++) {
        flat.rgba[k] = flat_color[k];
    }
    flat.index = flat_index;

    for (int py = py0; py <= py1; py++) {
        for (int px = px0; px <= px1; px++) {
            double coverage;
            double x, y, z = 0, bary[3] = { 0 }, w[3];
            size_t fan_i = 0;
            SGISoftVertex fan[3];
            SGISoftFrag frag = flat;
            bool found = false;
            bool background;

            if (!sgi_soft_stipple_pass(r, px, py, &background)) {
                continue;
            }
            if (sgi_soft_aa_contains_pixel(v, n, px, py, scale,
                                           polygon_area > 0)) {
                coverage = 1.0;
                x = px + 0.5;
                y = py + 0.5;
            } else {
                const SGISoftAAPoint *src = a;
                SGISoftAAPoint *dst = b;
                size_t count = n;
                double twice_area = 0, cx = 0, cy = 0;

                /* Restore the polygon before clipping this pixel. */
                for (size_t i = 0; i < n; i++) {
                    a[i] = (SGISoftAAPoint) {
                        v[i].x / scale, v[i].y / scale,
                    };
                }
                for (unsigned e = SGI_AA_LEFT; e <= SGI_AA_TOP; e++) {
                    double bound = e == SGI_AA_LEFT ? px :
                                   e == SGI_AA_RIGHT ? px + 1 :
                                   e == SGI_AA_BOTTOM ? py : py + 1;

                    if (!sgi_soft_aa_clip(src, count, dst, capacity, e,
                                          bound, &count)) {
                        qemu_log_mask(LOG_UNIMP, "sgi-gl: polygon coverage "
                                      "clip exceeded its bound\n");
                        goto out;
                    }
                    tmp = (SGISoftAAPoint *)src;
                    src = dst;
                    dst = tmp;
                    if (count < 3) {
                        break;
                    }
                }
                if (count < 3) {
                    continue;
                }
                for (size_t i = 0; i < count; i++) {
                    const SGISoftAAPoint *p = &src[i];
                    const SGISoftAAPoint *q = &src[(i + 1) % count];
                    double pxp = p->x - px, pyp = p->y - py;
                    double qxp = q->x - px, qyp = q->y - py;
                    double cross = pxp * qyp - qxp * pyp;

                    twice_area += cross;
                    cx += (pxp + qxp) * cross;
                    cy += (pyp + qyp) * cross;
                }
                if (!(fabs(twice_area) > 0)) {
                    continue;
                }
                coverage = MIN(1.0, fabs(twice_area) * 0.5);
                x = px + cx / (3 * twice_area);
                y = py + cy / (3 * twice_area);
            }

            /* Find the fan triangle containing the overlap centroid. */
            for (size_t i = 1; i + 1 < n; i++) {
                double den = sgi_soft_aa_edge(&v[0], &v[i],
                                               v[i + 1].x, v[i + 1].y);

                if (den == 0) {
                    continue;
                }
                bary[0] = sgi_soft_aa_edge(&v[i], &v[i + 1], x * scale,
                                            y * scale) / den;
                bary[1] = sgi_soft_aa_edge(&v[i + 1], &v[0], x * scale,
                                            y * scale) / den;
                bary[2] = 1 - bary[0] - bary[1];
                if (bary[0] >= -0.000000001 && bary[1] >= -0.000000001 &&
                    bary[2] >= -0.000000001) {
                    found = true;
                    fan_i = i;
                    break;
                }
            }
            if (!found) {
                continue;
            }
            {
                size_t i = fan_i;

                w[0] = bary[0] * v[0].inv_w;
                w[1] = bary[1] * v[i].inv_w;
                w[2] = bary[2] * v[i + 1].inv_w;
                double sum = w[0] + w[1] + w[2];
                double depth_w = bary[0] * v[0].depth_w +
                                 bary[1] * v[i].depth_w +
                                 bary[2] * v[i + 1].depth_w;

                if (!(sum > 0) || !isfinite(sum)) {
                    continue;
                }
                fan[0] = v[0];
                fan[1] = v[i];
                fan[2] = v[i + 1];

                z = bary[0] * v[0].z + bary[1] * v[i].z +
                    bary[2] * v[i + 1].z;
                for (int k = 0; k < 4; k++) {
                    frag.texcoord[k] = (w[0] * v[0].texcoord[k] +
                        w[1] * v[i].texcoord[k] +
                        w[2] * v[i + 1].texcoord[k]) / sum;
                    if (r->st->shade_model != SGI_GL_FLAT) {
                        frag.rgba[k] = (w[0] * v[0].rgba[k] +
                            w[1] * v[i].rgba[k] +
                            w[2] * v[i + 1].rgba[k]) / sum;
                    }
                }
                frag.fog = (w[0] * v[0].fog + w[1] * v[i].fog +
                            w[2] * v[i + 1].fog) / sum;
                if (r->st->shade_model != SGI_GL_FLAT) {
                    frag.index = v[0].index +
                        (w[1] * (v[i].index - v[0].index) +
                         w[2] * (v[i + 1].index - v[0].index)) / sum;
                }
                if (r->st->clip_planes) {
                    double d[SGI_GL_CLIP_PLANES];

                    for (int k = 0; k < SGI_GL_CLIP_PLANES; k++) {
                        d[k] = (w[0] * fan[0].clip[k] +
                                w[1] * fan[1].clip[k] +
                                w[2] * fan[2].clip[k]) / sum;
                    }
                    if (sgi_soft_clipped(r->st, d)) {
                        continue;
                    }
                }
                if (r->texture_base) {
                    SGISoftTexPlane gradient;
                    double value[4], out[5];

                    if (!sgi_soft_texture_plane(fan, sub,
                                                r->texture->volume,
                                                r->texture->one_dimensional,
                                                &gradient)) {
                        continue;
                    }
                    for (unsigned k = 0; k < 4; k++) {
                        value[k] = gradient.value[k] +
                            (px + 0.5 - gradient.x) * gradient.dx[k] +
                            (py + 0.5 - gradient.y) * gradient.dy[k];
                    }
                    frag.texture_rho =
                        sgi_soft_texture_derivatives(value, gradient.dx,
                            gradient.dy,
                            r->texture_base->width *
                            sgi_soft_texture_lod_scale(
                                r->texture->lod_scale_s),
                            r->texture->one_dimensional ? 0 :
                            r->texture_base->height *
                            sgi_soft_texture_lod_scale(
                                r->texture->lod_scale_t),
                            r->texture->volume ? r->texture_base->depth *
                            sgi_soft_texture_lod_scale(
                                r->texture->lod_scale_r) : 0, out) ?
                        out[4] : NAN;
                }
                if (background) {
                    sgi_soft_stipple_background(r, &frag);
                }
                sgi_soft_fragment_coverage(r, px, py, z, offset, depth_w,
                                           &frag, coverage, true);
            }
        }
    }
out:
    g_free(a);
    g_free(b);
}

static void sgi_soft_polygon_smooth(const SGISoftDraw *r,
                                    const SGIVolumeVertex *poly, size_t n,
                                    const float *provoking, bool front,
                                    double slope)
{
    SGISoftVertex *v = g_try_new(SGISoftVertex, n);
    double offset = 0;
    const float *flat_color = &provoking[SGI_GL_V_COLOR];
    const float *flat_index = &provoking[SGI_GL_V_INDEX];

    if (!v) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: polygon smoothing workspace "
                      "allocation failed; primitive dropped\n");
        return;
    }
    if (!front && r->back_colors) {
        flat_color = &provoking[SGI_GL_V_BACK_COLOR];
        flat_index = &provoking[SGI_GL_V_BACK_INDEX];
    }
    for (size_t i = 0; i < n; i++) {
        if (!sgi_soft_project(r, &poly[i], &v[i])) {
            goto out;
        }
        if (!front && r->back_colors) {
            const float *back = &poly[i].attr[SGI_GL_V_BACK_COLOR];

            for (unsigned k = 0; k < 4; k++) {
                v[i].rgba[k] = MIN(MAX(back[k], 0.0f), 1.0f);
            }
            v[i].index = poly[i].attr[SGI_GL_V_BACK_INDEX];
        }
    }
    if (r->texture_base) {
        for (size_t i = 1; i + 1 < n; i++) {
            SGISoftVertex fan[3] = { v[0], v[i], v[i + 1] };

            if (!sgi_soft_texture_coefficients(r, fan)) {
                goto out;
            }
        }
    }
    if (r->st->polygon_offset & SGI_GL_OFFSET_FILL) {
        offset = sgi_soft_offset_value(r->rules, r->st, slope);
    }
    sgi_soft_polygon_coverage(r, v, n, flat_color,
                              *flat_index, offset);
out:
    g_free(v);
}

/* Replay the original fan, including its validation and fragment ordering. */
static void sgi_soft_polygon_fill(const SGISoftDraw *r, const SGIGLDraw *d,
                                  const uint32_t *indices, size_t n,
                                  const float *provoking)
{
    const float *v0 = sgi_soft_polygon_vertex(d, indices, 0);
    bool prevalidate = r->texture &&
                       !(d->mode == SGI_GL_POLYGON && d->fill_by_triangle);

    if (prevalidate) {
        for (size_t i = 1; i + 1 < n; i++) {
            if (!sgi_soft_texture_triangle_valid(r, v0,
                    sgi_soft_polygon_vertex(d, indices, i),
                    sgi_soft_polygon_vertex(d, indices, i + 1))) {
                return;
            }
        }
    }
    for (size_t i = 1; i + 1 < n; i++) {
        sgi_soft_triangle(r, v0, sgi_soft_polygon_vertex(d, indices, i),
                          sgi_soft_polygon_vertex(d, indices, i + 1),
                          provoking);
    }
}

/* Workspace bounds are internal representations, not hardware maxima. */
static SGIGLDrawResult sgi_soft_polygon(const SGISoftDraw *r,
                                       const SGIGLDraw *d,
                                       const uint32_t *indices, size_t count,
                                       const float *provoking, bool force_edges)
{
    SGIVolumeVertex local[2][4 + SGI_VOLUME_PLANES + SGI_GL_CLIP_PLANES];
    SGIVolumeVertex *allocated = NULL, *poly, *other;
    SGISoftDraw positions = *r, coverage = *r;
    SGISoftVertex fan[3], first, previous, current;
    SGISoftLineStipple stipple = {
        .enabled = d->line_stipple_enabled,
        .repeat = MAX(1, MIN(d->line_stipple_repeat, 256)),
        .pattern = d->line_stipple_pattern,
    };
    float flat[SGI_GL_VERTEX_FLOATS];
    size_t capacity, n = count;
    double area = 0, slope = 0, offset = 0;
    bool front;
    uint32_t mode, offset_enable;

    if (count < 3) {
        return SGI_GL_DRAW_OK;
    }
    if (count > SIZE_MAX - SGI_VOLUME_PLANES - SGI_GL_CLIP_PLANES) {
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    }
    capacity = count + SGI_VOLUME_PLANES + SGI_GL_CLIP_PLANES;
    if (capacity > SIZE_MAX / (2 * sizeof(*poly))) {
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    }
    if (count <= 4) {
        poly = local[0];
        other = local[1];
    } else {
        allocated = g_try_realloc_n(NULL, capacity, 2 * sizeof(*poly));
        if (!allocated) {
            return SGI_GL_DRAW_OUT_OF_MEMORY;
        }
        poly = allocated;
        other = allocated + capacity;
    }
    /* Facing must not prevalidate texture Q and erase a valid FILL prefix. */
    positions.texture = NULL;
    for (size_t i = 0; i < count; i++) {
        sgi_volume_from_draw_vertex(r->st,
                                    sgi_soft_polygon_vertex(d, indices, i),
                                    &poly[i]);
        for (unsigned k = 0; k < 4; k++) {
            if (!isfinite(poly[i].clip[k]) || !isfinite(poly[i].eye[k])) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-gl: invalid polygon position dropped\n");
                goto out;
            }
        }
        if (force_edges) {
            sgi_soft_boundary_set_edge(&poly[i], true);
        }
    }
    if (!sgi_soft_boundary_clip(r, &poly, other, &n, capacity)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-gl: polygon exceeds convex clipping bound\n");
        goto out;
    }
    if (!n || !sgi_soft_project(&positions, &poly[0], &first)) {
        goto out;
    }
    previous = first;
    fan[0] = first;
    for (size_t i = 1; i < n; i++) {
        if (!sgi_soft_project(&positions, &poly[i], &current)) {
            goto out;
        }
        if (i >= 2) {
            area += sgi_soft_edge(&first, &previous, current.x, current.y);
            fan[1] = previous;
            fan[2] = current;
            slope = MAX(slope, sgi_soft_slope(r, fan));
        }
        previous = current;
    }
    if (r->st->window_matrix_given && r->st->window_rows_down) {
        area = -area;
    }
    if (r->st->front_face == SGI_GL_CW) {
        area = -area;
    }
    front = area > 0; /* Zero remains back-facing through both reversals. */
    if (r->st->cull_face &&
        (r->st->cull_face_mode == SGI_GL_FRONT_AND_BACK ||
         r->st->cull_face_mode == (front ? SGI_GL_FRONT : SGI_GL_BACK))) {
        goto out;
    }
    mode = d->polygon_modes[front ? 0 : 1];
    if (mode == SGI_GL_FILL) {
        if (r->polygon_smooth) {
            sgi_soft_polygon_smooth(r, poly, n, provoking, front, slope);
            goto out;
        }
        sgi_soft_polygon_fill(r, d, indices, count, provoking);
        goto out;
    }
    if (mode != SGI_GL_LINE && mode != SGI_GL_POINT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: invalid polygon mode 0x%x ignored\n", mode);
        goto out;
    }
    if ((mode == SGI_GL_LINE &&
         !sgi_soft_dimension(d->line_width, &coverage.line_width)) ||
        (mode == SGI_GL_POINT &&
         !sgi_soft_dimension(d->point_size, &coverage.point_size))) {
        goto out;
    }
    memcpy(flat, provoking, sizeof(flat));
    if (!front && r->back_colors) {
        memcpy(&flat[SGI_GL_V_COLOR], &provoking[SGI_GL_V_BACK_COLOR],
               4 * sizeof(float));
        for (size_t i = 0; i < n; i++) {
            memcpy(&poly[i].attr[SGI_GL_V_COLOR],
                   &poly[i].attr[SGI_GL_V_BACK_COLOR], 4 * sizeof(float));
        }
    }
    /* Validate the entire boundary before any LINE/POINT fragment or sample. */
    for (size_t i = 0; i < n; i++) {
        if (!sgi_soft_project(r, &poly[i], &current)) {
            goto out;
        }
        if (r->texture_base && mode == SGI_GL_LINE) {
            SGISoftVertex next;
            double values[4], delta[4], length;

            if (!sgi_soft_project(r, &poly[(i + 1) % n], &next) ||
                !sgi_soft_texture_line_gradient(&current, &next,
                    r->rules->subpixel_bits, r->texture->volume,
                    r->texture->one_dimensional, values, delta, &length)) {
                sgi_soft_texture_report(r, 16,
                    "invalid primitive coefficients, primitive dropped");
                goto out;
            }
        }
    }
    offset_enable = mode == SGI_GL_LINE ? SGI_GL_OFFSET_LINE :
                                          SGI_GL_OFFSET_POINT;
    if (r->st->polygon_offset & offset_enable) {
        offset = sgi_soft_offset_value(r->rules, r->st, slope);
    }
    for (size_t i = 0; i < n; i++) {
        SGISoftVertex next;

        if (!sgi_soft_boundary_edge(&poly[i])) {
            continue;
        }
        sgi_soft_project(r, &poly[i], &current);
        if (mode == SGI_GL_POINT) {
            sgi_soft_point_projected(&coverage, &current, flat, offset);
        } else {
            sgi_soft_project(r, &poly[(i + 1) % n], &next);
            sgi_soft_line_projected(&coverage, &current, &next, flat,
                                     offset, &stipple, false);
        }
    }
out:
    g_free(allocated);
    return SGI_GL_DRAW_OK;
}

/* Dispatch each independently assembled polygon, preserving provoking rules. */
static SGIGLDrawResult sgi_soft_polygon_modes(const SGISoftDraw *r,
                                             const SGIGLDraw *d)
{
    static const uint32_t fill_modes[2] = { SGI_GL_FILL, SGI_GL_FILL };
    uint32_t indices[4];
    uint32_t n = d->count, stride;
    SGIGLDrawResult result;

    if (!d->polygon_modes) {
        SGIGLDraw fill = *d;

        fill.polygon_modes = fill_modes;
        return sgi_soft_polygon_modes(r, &fill);
    }

    if (d->mode == SGI_GL_POLYGON) {
        return sgi_soft_polygon(r, d, NULL, n, d->vertices, false);
    }
    stride = d->mode == SGI_GL_TRIANGLES ? 3 :
             d->mode == SGI_GL_QUADS ? 4 :
             d->mode == SGI_GL_QUAD_STRIP ? 2 : 1;
    for (uint32_t i = 0; i < n; i += stride) {
        unsigned count = d->mode == SGI_GL_QUADS ||
                         d->mode == SGI_GL_QUAD_STRIP ? 4 : 3;
        uint32_t provoking = i + count - 1;
        bool force = stride < 3;

        if (n - i < count) {
            break;
        }
        for (unsigned k = 0; k < count; k++) {
            indices[k] = i + k;
        }
        if (d->mode == SGI_GL_TRIANGLE_STRIP && (i & 1)) {
            indices[0] = i + 1;
            indices[1] = i;
        } else if (d->mode == SGI_GL_TRIANGLE_FAN) {
            indices[0] = 0;
        } else if (d->mode == SGI_GL_QUAD_STRIP) {
            indices[2] = i + 3;
            indices[3] = i + 2;
        }
        result = sgi_soft_polygon(r, d, indices, count,
                   &d->vertices[(size_t)provoking * SGI_GL_VERTEX_FLOATS],
                   force);
        if (result != SGI_GL_DRAW_OK) {
            return result;
        }
    }
    return SGI_GL_DRAW_OK;
}

/*
 * The pieces of a primitive (OpenGL 1.2.1, 2.6.1), each with its flat
 * colour from the vertex table 2.9 names: a point's own, the second
 * vertex of a segment, the last vertex of a triangle, strip or fan
 * triangle, quad or quad-strip quad, and the first vertex of a polygon.
 * Every triangle of a strip keeps the strip's orientation (2.6.1), so an
 * odd one takes its first two vertices swapped, which changes only its
 * facing.
 *
 * A quadrilateral a, b, c, d in order around its edge is drawn as the
 * triangles a b c and a c d. OpenGL 1.2.1 (3.5.1) allows any split of a
 * polygon with more than three vertices that gives a convex combination
 * of the vertices' values, and says nothing of non-convex or non-planar
 * ones; which diagonal a head takes is not known. Approximated: every
 * polygon is split into a fan from its first vertex. The discriminating
 * test is gl-quad-splits-from-its-first-vertex.
 */

/* The filled primitives of a draw, from SGI_GL_TRIANGLES on, in order. */
static void sgi_soft_draw_triangles(const SGISoftDraw *r, const SGIGLDraw *d)
{
    uint32_t n = d->count;

#define V(i) (&d->vertices[(size_t)(i) * SGI_GL_VERTEX_FLOATS])
    switch (d->mode) {
    case SGI_GL_TRIANGLES:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            sgi_soft_triangle(r, V(i), V(i + 1), V(i + 2), V(i + 2));
        }
        break;
    case SGI_GL_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) {
                sgi_soft_triangle(r, V(i + 1), V(i), V(i + 2), V(i + 2));
            } else {
                sgi_soft_triangle(r, V(i), V(i + 1), V(i + 2), V(i + 2));
            }
        }
        break;
    case SGI_GL_TRIANGLE_FAN:
    case SGI_GL_POLYGON:
        if (r->texture && d->mode == SGI_GL_POLYGON && !d->fill_by_triangle) {
            for (uint32_t i = 0; i + 2 < n; i++) {
                if (!sgi_soft_texture_triangle_valid(r, V(0), V(i + 1),
                                                      V(i + 2))) {
                    return;
                }
            }
        }
        for (uint32_t i = 0; i + 2 < n; i++) {
            sgi_soft_triangle(r, V(0), V(i + 1), V(i + 2),
                              d->mode == SGI_GL_POLYGON ? V(0) : V(i + 2));
        }
        break;
    case SGI_GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            if (r->texture &&
                (!sgi_soft_texture_triangle_valid(r, V(i), V(i + 1),
                                                   V(i + 2)) ||
                 !sgi_soft_texture_triangle_valid(r, V(i), V(i + 2),
                                                   V(i + 3)))) {
                continue;
            }
            sgi_soft_triangle(r, V(i), V(i + 1), V(i + 2), V(i + 3));
            sgi_soft_triangle(r, V(i), V(i + 2), V(i + 3), V(i + 3));
        }
        break;
    case SGI_GL_QUAD_STRIP:
        /* Vertices 2i - 1, 2i, 2i + 2 and 2i + 1 in order (2.6.1). */
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            if (r->texture &&
                (!sgi_soft_texture_triangle_valid(r, V(i), V(i + 1),
                                                   V(i + 3)) ||
                 !sgi_soft_texture_triangle_valid(r, V(i), V(i + 3),
                                                   V(i + 2)))) {
                continue;
            }
            sgi_soft_triangle(r, V(i), V(i + 1), V(i + 3), V(i + 3));
            sgi_soft_triangle(r, V(i), V(i + 3), V(i + 2), V(i + 3));
        }
        break;
    default:
        g_assert_not_reached();
    }
#undef V
}

/*
 * About how many pixels a draw's filled primitives cover: the area of
 * each one with its vertices moved into the pixels the window owns,
 * summed until it reaches limit. A vertex with no window position counts
 * as reaching it. A bounding box would take a long thin triangle across
 * the window for a large one.
 */
static uint64_t sgi_soft_draw_pixels(const SGISoftDraw *r, const SGIGLDraw *d,
                                     uint64_t limit)
{
    uint32_t n = d->count, step, size;
    g_autofree double *xy = NULL;
    uint64_t sum = 0;
    int x0, y0, x1, y1;

    if (!sgi_soft_bounds(r, &x0, &y0, &x1, &y1) || n < 3) {
        return 0;
    }
    xy = g_new(double, 2 * (size_t)n);
    for (uint32_t i = 0; i < n; i++) {
        SGIVolumeVertex cv;
        double win[3];

        sgi_volume_from_draw_vertex(r->st,
            &d->vertices[(size_t)i * SGI_GL_VERTEX_FLOATS], &cv);
        if (!(cv.clip[3] > 0)) {
            return limit;
        }
        sgi_gl_clip_to_window(r->st, cv.clip, win);
        xy[2 * i] = win[0];
        xy[2 * i + 1] = win[1];
    }
    /* Each primitive's own vertices; a fan's first is left out. */
    step = d->mode == SGI_GL_TRIANGLES ? 3 : d->mode == SGI_GL_QUADS ? 4 :
           d->mode == SGI_GL_QUAD_STRIP ? 2 : 1;
    size = d->mode == SGI_GL_QUADS || d->mode == SGI_GL_QUAD_STRIP ? 4 : 3;
    for (uint32_t i = 0; i + size <= n && sum < limit; i += step) {
        double px[4], py[4], twice = 0;

        for (uint32_t k = 0; k < size; k++) {
            px[k] = MIN(MAX(xy[2 * (i + k)], x0), x1 + 1);
            py[k] = MIN(MAX(xy[2 * (i + k) + 1], y0), y1 + 1);
        }
        for (uint32_t k = 0; k < size; k++) {
            uint32_t next = (k + 1) % size;

            twice += px[k] * py[next] - px[next] * py[k];
        }
        sum += (uint64_t)(fabs(twice) / 2);
    }
    return sum;
}

/*
 * Whether every vertex of a draw has the texture coordinates its image
 * uses: a draw with one that has not drops that primitive unread, so it
 * reads its texels as it reaches them rather than all of them first.
 */
static bool sgi_soft_fixed_coordinates_valid(const SGISoftDraw *r,
                                             const SGIGLDraw *d)
{
    for (uint32_t i = 0; i < d->count; i++) {
        SGIVolumeVertex v;

        sgi_volume_from_draw_vertex(r->st,
            &d->vertices[(size_t)i * SGI_GL_VERTEX_FLOATS], &v);
        if (!sgi_soft_texcoord_valid(r->texture, r->texture_base, &v)) {
            return false;
        }
    }
    return true;
}

typedef struct SGISoftBandDraw {
    const SGISoftDraw *r;
    const SGIGLDraw *d;
} SGISoftBandDraw;

static void sgi_soft_band_draw(void *opaque, unsigned band, unsigned bands)
{
    SGISoftBandDraw *job = opaque;
    SGISoftDraw r = *job->r;

    r.band = band;
    r.bands = bands;
    if (r.texture) {
        r.texels = sgi_soft_texel_cache_begin(r.texture);
    }
    if (r.fixed && !r.fixed->prefilled && !sgi_soft_fixed_begin(&r)) {
        r.fixed = NULL;
    }
    sgi_soft_band_quiet = band > 0;
    sgi_soft_draw_triangles(&r, job->d);
    sgi_soft_band_quiet = false;
}

/*
 * Draw the filled primitives in bands when the target allows it and they
 * cover enough pixels to pay for repeating each primitive's setup in
 * every band; false, having drawn nothing, otherwise.
 */
static bool sgi_soft_draw_parallel(const SGISoftDraw *r, const SGIGLDraw *d)
{
    unsigned bands = sgi_soft_bands();
    uint64_t min_pixels = sgi_soft_band_min_pixels(r->t);
    SGISoftBandDraw job = { .r = r, .d = d };

    if (!r->t->concurrent_pixels || bands < 2 ||
        sgi_soft_draw_pixels(r, d, min_pixels) < min_pixels) {
        return false;
    }
    return sgi_soft_bands_run(sgi_soft_band_draw, &job, bands);
}

SGIGLDrawResult sgi_soft_draw(const SGIRasterRules *rules,
                             const SGIRasterTarget *t, const SGIGLDraw *d)
{
    SGISoftDraw r = {
        .rules = rules, .t = t, .st = d->state,
        .back_colors = d->back_colors,
        .point_smooth = rules->smooth_primitives && d->point_smooth,
        .line_smooth = rules->smooth_primitives && d->line_smooth,
        .smooth_point_size = d->point_size,
        .smooth_line_width = d->line_width,
        .polygon_smooth = rules->smooth_primitives && d->polygon_smooth,
        .polygon_stipple = d->polygon_stipple_enabled ?
                           d->polygon_stipple : NULL,
    };
    SGISoftLineStipple line = {
        .counter = d->line_phase && !d->line_phase_reset ? *d->line_phase : 0,
        .enabled = d->line_stipple_enabled,
        .repeat = MAX(1, MIN(d->line_stipple_repeat, 256)),
        .pattern = d->line_stipple_pattern,
    };
    SGISoftLineStipple *stipple = d->line_phase || d->line_stipple_enabled ?
                                &line : NULL;
    uint32_t n = d->count;
    unsigned texture_reports = 0;
    bool rgba_reported = false;
    SGISoftFixedTex fixed;

    line.counter %= 16 * line.repeat;
    r.texture_reports = &texture_reports;
    r.rgba_reported = &rgba_reported;

#define V(i) (&d->vertices[(size_t)(i) * SGI_GL_VERTEX_FLOATS])
    r.ntargets = sgi_gl_draw_targets(d->state, r.targets);
    if (!sgi_soft_check_rules(rules) ||
        !sgi_soft_check_index(t, d->state->index_bits)) {
        return SGI_GL_DRAW_OK;
    }
    if (!sgi_soft_window(d->state, &r.win)) {
        if (!stipple || d->mode < SGI_GL_LINES ||
            d->mode > SGI_GL_LINE_STRIP || !d->state->drawable.bound ||
            d->state->drawable.clip.n) {
            return SGI_GL_DRAW_OK;
        }
        /* Empty ownership still counts standalone geometric coverage. */
        r.win.x1 = r.win.y1 = -1;
    }
    if (!sgi_soft_texture_setup(&r, true)) {
        return SGI_GL_DRAW_OK;
    }
    if (r.texture) {
        r.texels = sgi_soft_texel_cache_begin(r.texture);
    }
    if (!sgi_soft_setup_ops(&r)) {
        return SGI_GL_DRAW_OK;
    }
    if (d->mode >= SGI_GL_TRIANGLES && d->mode <= SGI_GL_POLYGON &&
        sgi_soft_fixed_setup(&r, &fixed)) {
        uint64_t texels = 0;
        uint32_t first, last;

        r.fixed = &fixed;
        fixed.span = sgi_soft_fixed_span_select(&r);
        sgi_soft_fixed_reach(&fixed, &first, &last);
        for (uint32_t i = first; i <= last; i++) {
            texels += (uint64_t)fixed.level[i].width * fixed.level[i].height;
        }
        /* A draw as large as the smallest that bands share, at least. */
        texels = MAX(texels, SGI_RASTER_PARALLEL_MIN_PIXELS);
        if (!(sgi_soft_draw_pixels(&r, d, texels) >= texels &&
              sgi_soft_fixed_coordinates_valid(&r, d) &&
              sgi_soft_fixed_prefill(&r, &fixed)) &&
            !sgi_soft_fixed_begin(&r)) {
            r.fixed = NULL;
        }
    }
    if (!rules->smooth_primitives) {
        const char *smooth = NULL;

        if (d->mode == SGI_GL_POINTS && d->point_smooth) {
            smooth = "point";
        } else if (d->mode >= SGI_GL_LINES &&
                   d->mode <= SGI_GL_LINE_STRIP && d->line_smooth) {
            smooth = "line";
        } else if (d->mode >= SGI_GL_TRIANGLES &&
                   d->mode <= SGI_GL_POLYGON && d->polygon_smooth) {
            smooth = "polygon";
        }
        if (smooth) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: %s smoothing requested but "
                          "unsupported by raster rules; drawn aliased\n",
                          smooth);
        }
    }
    if (r.polygon_smooth && d->mode >= SGI_GL_TRIANGLES &&
        d->mode <= SGI_GL_POLYGON) {
        return sgi_soft_polygon_modes(&r, d);
    }
    if (d->polygon_modes && d->mode >= SGI_GL_TRIANGLES &&
        d->mode <= SGI_GL_POLYGON &&
        (d->polygon_modes[0] != SGI_GL_FILL ||
         d->polygon_modes[1] != SGI_GL_FILL)) {
        return sgi_soft_polygon_modes(&r, d);
    }
    switch (d->mode) {
    case SGI_GL_POINTS:
        if (!sgi_soft_dimension(d->point_size, &r.point_size)) {
            return SGI_GL_DRAW_OK;
        }
        for (uint32_t i = 0; i < n; i++) {
            sgi_soft_point(&r, V(i));
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_LINES:
        if (!sgi_soft_dimension(d->line_width, &r.line_width)) {
            return SGI_GL_DRAW_OK;
        }
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            if (!d->line_phase) {
                line.counter = 0;
            }
            sgi_soft_line(&r, V(i), V(i + 1), V(i + 1), stipple);
        }
        if (d->line_phase && line.consumed) {
            *d->line_phase = line.counter;
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_LINE_STRIP:
    case SGI_GL_LINE_LOOP:
        if (!sgi_soft_dimension(d->line_width, &r.line_width)) {
            return SGI_GL_DRAW_OK;
        }
        for (uint32_t i = 0; i + 1 < n; i++) {
            sgi_soft_line(&r, V(i), V(i + 1), V(i + 1), stipple);
        }
        if (d->mode == SGI_GL_LINE_LOOP && n >= 2) {
            sgi_soft_line(&r, V(n - 1), V(0), V(0), stipple);
        }
        if (d->line_phase && line.consumed) {
            *d->line_phase = line.counter;
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_TRIANGLES:
    case SGI_GL_TRIANGLE_STRIP:
    case SGI_GL_TRIANGLE_FAN:
    case SGI_GL_POLYGON:
    case SGI_GL_QUADS:
    case SGI_GL_QUAD_STRIP:
        if (!sgi_soft_draw_parallel(&r, d)) {
            sgi_soft_draw_triangles(&r, d);
        }
        return SGI_GL_DRAW_OK;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: draw of mode %u ignored\n",
                      d->mode);
        return SGI_GL_DRAW_OK;
    }
#undef V
}

/*
 * A run of a bitmap's image (OpenGL 1.2.1, 3.7): a fragment for each set
 * bit, at the image's corner plus the bit's column and row, bounded by
 * the window the draw state's drawable owns and not by the viewport.
 */
static void sgi_soft_bitmap(const SGIRasterRules *rules,
                            const SGIRasterTarget *t, const SGIRasterOp *op)
{
    const SGIRasterBitmap *b = &op->bitmap;
    SGISoftDraw r = { .rules = rules, .t = t, .st = b->state };
    uint32_t row_bytes = DIV_ROUND_UP(b->width, 8);
    bool rgba_reported = false;
    unsigned texture_reports = 0;
    SGISoftFrag frag = {
        .rgba = { b->rgba[0], b->rgba[1], b->rgba[2], b->rgba[3] },
        .texcoord = { b->texcoord[0], b->texcoord[1],
                      b->texcoord[2], b->texcoord[3] },
        .index = b->index,
        .fog = b->distance,
    };

    r.rgba_reported = &rgba_reported;
    r.texture_reports = &texture_reports;
    if (!sgi_soft_texture_setup(&r, true) ||
        !sgi_soft_window(b->state, &r.win) ||
        !sgi_soft_check_index(t, b->state->index_bits)) {
        return;
    }
    if (r.texture) {
        r.texels = sgi_soft_texel_cache_begin(r.texture);
    }
    r.ntargets = MIN(op->ntargets, SGI_GL_MAX_TARGETS);
    memcpy(r.targets, op->targets, sizeof(r.targets));
    if (!sgi_soft_setup_ops(&r)) {
        return;
    }
    for (uint32_t i = 0; i < b->count && row_bytes; i++) {
        uint32_t byte = b->first + i;
        int64_t row = byte / row_bytes, col0 = (byte % row_bytes) * 8;

        if (row >= b->height) {
            break;
        }
        for (int bit = 0; bit < 8 && col0 + bit < b->width; bit++) {
            int64_t px = b->x + col0 + bit, py = b->y + row;

            if (!(b->bytes[i] & (0x80 >> bit)) ||
                px < r.win.x0 - r.win.ox || px > r.win.x1 - r.win.ox ||
                py < r.win.y0 - r.win.oy || py > r.win.y1 - r.win.oy) {
                continue;
            }
            sgi_soft_fragment_at(&r, px, py, b->z, &frag, 1, false);
        }
    }
}

static void sgi_soft_pixels(const SGIRasterRules *rules,
                            const SGIRasterTarget *t, const SGIRasterOp *op)
{
    const SGIRasterPixels *p = &op->pixels;
    bool image = p->rgba || p->rgba_float || p->rgba_double || p->depth ||
                 p->stencil || p->index;
    SGISoftDraw r = { .rules = rules, .t = t, .st = p->state };
    SGISoftFrag frag = {
        .fog = p->distance,
        .texcoord = { p->texcoord[0], p->texcoord[1],
                      p->texcoord[2], p->texcoord[3] },
    };
    bool rgba_reported = false;
    unsigned texture_reports = 0;
    uint32_t stencil_max = 0, stencil_mask = 0;
    double ex = p->x + (double)p->zoom[0] * p->width;
    double ey = p->y + (double)p->zoom[1] * p->height;
    double bx0, by0, bx1, by1;
    int64_t x0, y0, x1, y1;

    r.rgba_reported = &rgba_reported;
    r.texture_reports = &texture_reports;
    if ((!p->stencil && !sgi_soft_texture_setup(&r, true)) ||
        (!p->stencil && !sgi_soft_check_index(t, p->state->index_bits)) ||
        !p->width || !p->height || !image ||
        !isfinite(p->x) || !isfinite(p->y) ||
        !isfinite(p->zoom[0]) || !isfinite(p->zoom[1]) ||
        !p->zoom[0] || !p->zoom[1] || !sgi_soft_window(p->state, &r.win)) {
        return;
    }
    if (r.texture) {
        r.texels = sgi_soft_texel_cache_begin(r.texture);
    }
    r.ntargets = MIN(op->ntargets, SGI_GL_MAX_TARGETS);
    memcpy(r.targets, op->targets, sizeof(r.targets));
    if (p->stencil) {
        if (!sgi_soft_stencil_max(rules, t, &stencil_max)) {
            return;
        }
        stencil_mask = p->state->stencil_write_mask & stencil_max;
        if (!stencil_mask) {
            return;
        }
    } else if (!sgi_soft_setup_ops(&r)) {
        return;
    }
    /* Include lower/left centres and exclude upper/right centres (3.6.4). */
    bx0 = MAX(ceil(MIN(p->x, ex) - 0.5), (int64_t)r.win.x0 - r.win.ox);
    by0 = MAX(ceil(MIN(p->y, ey) - 0.5), (int64_t)r.win.y0 - r.win.oy);
    bx1 = MIN(ceil(MAX(p->x, ex) - 0.5) - 1,
               (int64_t)r.win.x1 - r.win.ox);
    by1 = MIN(ceil(MAX(p->y, ey) - 0.5) - 1,
               (int64_t)r.win.y1 - r.win.oy);
    if (bx0 > bx1 || by0 > by1) {
        return;
    }
    /* Visible bounds keep conversion and work independent of zoom size. */
    x0 = bx0;
    y0 = by0;
    x1 = bx1;
    y1 = by1;
    for (int64_t y = y0; y <= y1; y++) {
        double qy = (y + 0.5 - p->y) / p->zoom[1];
        double sy = p->zoom[1] > 0 ? floor(qy) : ceil(qy) - 1;

        if (!(sy >= 0 && sy < p->height)) {
            continue;
        }
        for (int64_t x = x0; x <= x1; x++) {
            double qx = (x + 0.5 - p->x) / p->zoom[0];
            double sx = p->zoom[0] > 0 ? floor(qx) : ceil(qx) - 1;
            uint64_t at;
            uint32_t rgba;

            if (!(sx >= 0 && sx < p->width)) {
                continue;
            }
            at = (uint64_t)sy * p->width + (uint32_t)sx;
            if (p->stencil) {
                int px = r.win.ox + x, py = r.win.oy + y;

                if (sgi_soft_owned_at(&r, x, y)) {
                    uint32_t old = t->load_stencil(t->opaque, px, py);

                    t->store_stencil(t->opaque, px, py,
                        ((old & ~stencil_mask) |
                         (p->stencil[at] & stencil_mask)) & stencil_max);
                }
                continue;
            }
            frag.index = p->index ? (double)p->index[at] : p->colour_index;
            rgba = p->rgba ? p->rgba[at] : 0;
            for (int k = 0; k < 4; k++) {
                double component = p->depth ? p->colour[k] :
                                   p->rgba_double ? p->rgba_double[at][k] :
                                   p->rgba_float ? p->rgba_float[at][k] :
                                   ((rgba >> (24 - 8 * k)) & 0xff) / 255.0;

                frag.rgba[k] = CLAMP(component * p->transfer.scale[k] +
                                     p->transfer.bias[k], 0.0, 1.0);
            }
            double z = p->depth ?
                CLAMP(p->depth[at] * p->depth_scale + p->depth_bias, 0.0, 1.0) :
                p->z;

            sgi_soft_fragment_at(&r, x, y, z, &frag, 1, false);
        }
    }
}

/*
 * Whether one rectangle of a non-empty clip list holds all of row y from
 * x0 to x1, so that each pixel's own test would pass.
 */
static bool sgi_soft_clip_covers_row(const SGIClipList *clip, int x0, int x1,
                                     int y)
{
    for (uint32_t i = 0; i < MIN(clip->n, SGI_CLIP_MAX); i++) {
        const SGIClipRect *r = &clip->r[i];

        if (y >= r->y0 && y <= r->y1 && x0 >= r->x0 && x1 <= r->x1) {
            return true;
        }
    }
    return false;
}

/* A clear's decided values, which every row of it stores. */
typedef struct SGISoftClear {
    const SGIRasterRules *rules;
    const SGIRasterTarget *t;
    const SGIRasterOp *op;
    bool colour, depth, stencil, rgba, rgba_read;
    uint32_t rgb, smax, mask, rgba_mask, ntargets, depth_value;
    SGIRasterColour clear;
    bool *rgba_reported;
} SGISoftClear;

/* The rows of a clear that are band's of bands (sgi_soft_row_in_band()). */
static void sgi_soft_clear_rows(void *opaque, unsigned band, unsigned bands)
{
    const SGISoftClear *c = opaque;
    const SGIRasterRules *rules = c->rules;
    const SGIRasterTarget *t = c->t;
    const SGIRasterOp *op = c->op;
    const SGISoftDraw rows = { .band = band, .bands = bands };
    /* Only a read fills it, and only a read's targets are read back. */
    SGIRasterColour dest[SGI_GL_MAX_TARGETS] = { 0 };

    for (int y = op->y0; y <= op->y1; y++) {
        bool whole_row;

        if (!sgi_soft_row_in_band(&rows, y)) {
            continue;
        }
        whole_row = op->clip.n <= 1 ||
                    sgi_soft_clip_covers_row(&op->clip, op->x0, op->x1, y);
        for (int x = op->x0; x <= op->x1; x++) {
            if (!whole_row && !sgi_clip_contains(&op->clip, x, y)) {
                continue;
            }

            if (c->rgba && c->rgba_read) {
                memset(dest, 0, sizeof(dest));
                if (!sgi_soft_rgba_load(rules, t, op->targets, c->ntargets,
                                        x, y, dest, c->rgba_reported)) {
                    continue;
                }
            }
            if (c->depth) {
                t->store_depth(t->opaque, x, y, c->depth_value);
            }
            if (c->stencil) {
                uint32_t smask = op->stencil_mask & c->smax;

                t->store_stencil(t->opaque, x, y,
                                 (t->load_stencil(t->opaque, x, y) &
                                  ~smask & c->smax) |
                                 (op->clear_stencil & smask));
            }
            for (uint32_t i = 0; c->colour && i < c->ntargets; i++) {
                if (op->index_bits) {
                    t->store_index(t->opaque, op->targets[i], x, y,
                                   op->clear_index, op->index_mask);
                } else if (c->rgba) {
                    SGIRasterColour out = c->clear;

                    for (unsigned k = 0; k < 4; k++) {
                        if (!(c->rgba_mask & (1u << k))) {
                            out.component[k] = dest[i].component[k];
                        }
                    }
                    t->store_rgba(t->opaque, op->targets[i], x, y, &out);
                } else if (c->mask == sgi_soft_colour_bits(rules)) {
                    t->store_colour(t->opaque, op->targets[i], x, y, c->rgb);
                } else {
                    t->store_colour(t->opaque, op->targets[i], x, y,
                                    (c->rgb & c->mask) |
                                    (t->load_colour(t->opaque, op->targets[i],
                                                    x, y) & ~c->mask));
                }
            }
        }
    }
}

/*
 * CLEAR writes the clear colour under the colour mask, or in colour-index
 * mode the clear index under the index mask, to the op's targets, the
 * clear depth to the depth plane and the clear stencil value under its
 * mask to the stencil buffer, where the planes have one, at every pixel of
 * its rectangle its clip list keeps. BITMAP draws its run of a bitmap,
 * and PIXELS draws a zoomed RGBA rectangle.
 * The other kinds are not executed here yet.
 */
void sgi_soft_raster(const SGIRasterRules *rules, const SGIRasterTarget *t,
                     const SGIRasterOp *op)
{
    bool colour = op->clear_buffers & SGI_GL_COLOR_BUFFER_BIT;
    bool depth = op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT;
    bool stencil = op->clear_buffers & SGI_GL_STENCIL_BUFFER_BIT;
    uint32_t rgb = 0, smax = 0, mask = sgi_soft_colour_bits(rules);
    uint32_t rgba_mask = sgi_soft_rgba_mask(rules, op->clear_colour_mask);
    bool rgba = !op->index_bits && t->store_rgba;
    bool rgba_read = colour && op->ntargets && rgba_mask &&
                     rgba_mask != sgi_soft_rgba_mask(rules,
                                                    SGI_GL_COLOUR_MASK_ALL);
    uint32_t ntargets = MIN(op->ntargets, SGI_GL_MAX_TARGETS);
    bool rgba_reported = false;
    SGIRasterColour clear = { 0 };
    uint32_t depth_value;
    SGISoftClear rows;
    unsigned bands;

    if (!sgi_soft_check_rules(rules)) {
        return;
    }
    /* After the check: the depth maximum shifts by 32 - depth_bits. */
    depth_value = op->clear_depth24 & sgi_soft_depth_max(rules);
    if (op->kind == SGI_RASTER_PIXELS) {
        sgi_soft_pixels(rules, t, op);
        return;
    }
    if (op->kind == SGI_RASTER_BITMAP) {
        sgi_soft_bitmap(rules, t, op);
        return;
    }
    if (op->kind != SGI_RASTER_CLEAR) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: raster op of kind %u is not "
                      "executed\n", op->kind);
        return;
    }
    if (!op->clip.n && op->clip.empty) {
        return;
    }
    if (stencil && !sgi_soft_stencil_max(rules, t, &smax)) {
        stencil = false;
    }
    if (colour && op->index_bits && !sgi_soft_check_index(t, op->index_bits)) {
        colour = false;
    }
    if (!op->index_bits && !sgi_soft_rgba_capable(rules, t, rgba_read)) {
        return;
    }
    if (colour && !op->index_bits) {
        double c[3] = { op->clear_rgba[0], op->clear_rgba[1],
                        op->clear_rgba[2] };

        mask = sgi_raster_colour_mask(rules, op->clear_colour_mask);
        if (!rgba && mask != sgi_soft_colour_bits(rules) &&
            mask && !t->load_colour) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: a partial colour mask needs "
                          "planes that can be read back; cleared without "
                          "it\n");
            mask = sgi_soft_colour_bits(rules);
        }
        colour = rgba ? rgba_mask != 0 : mask != 0;
        rgb = sgi_soft_rgb(rules, c);
        /* Clears remain undithered: no dither state is submitted. */
        clear = sgi_soft_rgba_value(rules, rgb, op->clear_rgba[3]);
    }
    rows = (SGISoftClear) {
        .rules = rules, .t = t, .op = op,
        .colour = colour, .depth = depth, .stencil = stencil,
        .rgba = rgba, .rgba_read = rgba_read,
        .rgb = rgb, .smax = smax, .mask = mask, .rgba_mask = rgba_mask,
        .ntargets = ntargets, .depth_value = depth_value, .clear = clear,
        .rgba_reported = &rgba_reported,
    };
    bands = sgi_soft_bands();
    if (!t->concurrent_pixels || bands < 2 || op->y1 < op->y0 ||
        op->x1 < op->x0 ||
        (uint64_t)(op->x1 - op->x0 + 1) * (op->y1 - op->y0 + 1) <
        sgi_soft_band_min_pixels(t) ||
        !sgi_soft_bands_run(sgi_soft_clear_rows, &rows, bands)) {
        sgi_soft_clear_rows(&rows, 0, 1);
    }
}

#ifdef SGI_GL_TEXTURE_TEST
/* Numeric observability exists only in the unit-test compilation. */
#include "tests/unit/sgi-gl-texture-test.h"

bool sgi_gl_texture_test_derivatives(const double value[3],
    const double dx[3], const double dy[3], uint32_t ns, uint32_t nt,
    double out[5])
{
    double v[4] = { value[0], value[1], value[2], 0 };
    double x[4] = { dx[0], dx[1], dx[2], 0 };
    double y[4] = { dy[0], dy[1], dy[2], 0 };

    return sgi_soft_texture_derivatives(v, x, y, ns, nt, 0, out);
}

bool sgi_gl_texture_test_derivatives_scaled(const double value[3],
    const double dx[3], const double dy[3], uint32_t ns, uint32_t nt,
    double scale_s, double scale_t, double out[5])
{
    double v[4] = { value[0], value[1], value[2], 0 };
    double x[4] = { dx[0], dx[1], dx[2], 0 };
    double y[4] = { dy[0], dy[1], dy[2], 0 };

    return sgi_soft_texture_derivatives(v, x, y,
        ns * sgi_soft_texture_lod_scale(scale_s),
        nt * sgi_soft_texture_lod_scale(scale_t), 0, out);
}

bool sgi_gl_texture_test_select(uint32_t min, uint32_t mag,
    const SGIGLTexPyramid *p, uint32_t last, double rho, uint32_t out[3],
    double numeric[2])
{
    SGISoftTexSelection selected;

    if (!sgi_soft_texture_select(min, mag, p, last, rho, &selected)) {
        return false;
    }
    out[0] = selected.first;
    out[1] = selected.second;
    out[2] = selected.filter;
    numeric[0] = selected.lambda;
    numeric[1] = selected.alpha;
    return true;
}

bool sgi_gl_texture_test_plane(const int64_t xy[3][2], unsigned sub,
    const double coords[3][4], const double iw[3], double value[3],
    double dx[3], double dy[3])
{
    SGISoftVertex v[3] = { 0 };
    SGISoftTexPlane g;

    for (unsigned i = 0; i < 3; i++) {
        v[i].x = xy[i][0];
        v[i].y = xy[i][1];
        v[i].inv_w = iw[i];
        memcpy(v[i].texcoord, coords[i], sizeof(v[i].texcoord));
    }
    if (!sgi_soft_texture_plane(v, sub, false, false, &g)) {
        return false;
    }
    memcpy(value, g.value, 3 * sizeof(*value));
    memcpy(dx, g.dx, 3 * sizeof(*dx));
    memcpy(dy, g.dy, 3 * sizeof(*dy));
    return true;
}

bool sgi_gl_texture_test_sample(const SGIGLTexImage *im,
    const SGIGLTexLevel *level, uint32_t filter, double s, double t,
    double out[4])
{
    unsigned reports = 0;
    SGISoftDraw r = { .texture = im, .texture_reports = &reports };

    return sgi_soft_texture_sample(&r, level, filter, s, t, 0, out);
}

void sgi_gl_texture_test_mix(double value[4], const double other[4],
    double alpha)
{
    sgi_soft_texture_mix(value, other, alpha);
}
#endif
