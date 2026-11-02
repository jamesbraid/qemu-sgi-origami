/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI DMediaPro DM8 PCI audio: the IC Ensemble ICE1724 (Envy24HT) controller
 * with the converter wiring of an M-Audio Revolution 7.1 board, and an
 * AC'97 variant of the same controller.
 *
 * The register contract is docs/datasheets/dm8-ice1724.md in the Origami
 * repository. Facts there are graded established, observed, expectation,
 * inferred, approximated or unknown. Where this file departs from an
 * established fact, or fills an unknown with a safe behaviour, a comment says
 * "approximation" and names the datasheet section; the same list is in
 * docs/system/mips/sgi/hardware/sgi-dm8.rst. An access to a register or a
 * value the datasheet leaves unknown is logged (LOG_UNIMP or
 * LOG_GUEST_ERROR), never silently accepted.
 *
 * Layout:
 *  - BAR0 (32 bytes, I/O) is the controller block "CCS"; BAR1 (128 bytes,
 *    I/O) is the multi-channel block "MT". Both are little-endian register
 *    files and are implemented a byte at a time, so a wider access is
 *    several byte steps in address order.
 *  - Seven DMA engines share one sample rate. Their position and interrupt
 *    behaviour follows the virtual clock, so a qtest can step time and read
 *    the registers; samples are handed to the audio backend best effort.
 *  - The DM8 variant has two write-only AKM converters behind bit-banged
 *    GPIO pins and a configuration EEPROM on the I2C port. The AC'97 variant
 *    has no converter pins and no EEPROM, and a generic AC'97 register file
 *    behind MT04 to MT06.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TYPE_SGI_DM8 "sgi-dm8"
OBJECT_DECLARE_SIMPLE_TYPE(SGIDM8State, SGI_DM8)

#define DM8_VENDOR_ID           0x1412
#define DM8_DEVICE_ID           0x1724
#define DM8_REVISION            1
/* Subsystem ID the IRIX driver compares against: inferred for the DM8. */
#define DM8_SUBSYSTEM_ID        0x3630
/* The datasheet's default when no EEPROM supplies one. */
#define DM8_NO_EEPROM_SUBSYSTEM_ID 0x1724

#define CCS_SIZE                0x20
#define MT_SIZE                 0x80
#define PM_CAP_OFFSET           0x80
#define PCI_SVID_MASK_OFFSET    0x42

/* Controller block registers (BAR0). */
#define CCS_CONTROL             0x00
#define CCS_INT_MASK            0x01
#define CCS_INT_STATUS          0x02
#define CCS_SYS_CONFIG          0x04
#define CCS_AC_CONFIG           0x05
#define CCS_I2S_FEATURES        0x06
#define CCS_SPDIF_CONFIG        0x07
#define CCS_UART_TX_COUNT       0x0a
#define CCS_UART_RX_COUNT       0x0b
#define CCS_UART_DATA           0x0c
#define CCS_UART_COMMAND        0x0d
#define CCS_UART_SETTING        0x0e
#define CCS_I2C_DEVICE          0x10
#define CCS_I2C_BYTE            0x11
#define CCS_I2C_DATA            0x12
#define CCS_I2C_STATUS          0x13
#define CCS_GPIO_DATA_LO        0x14
#define CCS_GPIO_MASK_LO        0x16
#define CCS_GPIO_DIR            0x18
#define CCS_POWER_DOWN          0x1c
#define CCS_GPIO_DATA_HI        0x1e
#define CCS_GPIO_MASK_HI        0x1f

#define CCS_INT_MPU_RX          0x80
#define CCS_INT_MPU_TX          0x20
#define CCS_INT_MT              0x10
#define CCS_INT_STATUS_WRITABLE 0xe0

#define CCS_CONTROL_SOFT_RESET  0x80
#define CCS_AC_I2S              0x80
#define CCS_SYS_CRYSTAL_49      0x40
#define CCS_I2C_BUSY            0x01
#define CCS_I2C_EEPROM          0x80
#define CCS_PD_MT_CLOCK         0x01

/* Multi-channel block registers (BAR1). */
#define MT_STATUS               0x00
#define MT_RATE                 0x01
#define MT_FORMAT               0x02
#define MT_MASK                 0x03
#define MT_AC97_INDEX           0x04
#define MT_AC97_CMD             0x05
#define MT_AC97_DATA            0x06
#define MT_START                0x18
#define MT_CHANNELS             0x19
#define MT_FIFO_ERROR           0x1a
#define MT_PAUSE                0x1b
#define MT_ROUTING              0x2c
#define MT_SPDIF_CTRL           0x3c
#define MT_PEAK_INDEX           0x3e
#define MT_PEAK_DATA            0x3f

#define MT_STATUS_FIFO          0x08

#define MT_AC97_COLD_RESET      0x80
#define MT_AC97_WARM_RESET      0x40
#define MT_AC97_WRITE           0x20
#define MT_AC97_READ            0x10
#define MT_AC97_READY           0x08

#define MT_FORMAT_128X          0x08

/* Pin assignment of the Revolution 7.1 wiring (datasheet, GPIO block). */
#define GPIO_CCLK               BIT(1)
#define GPIO_CDOUT              BIT(3)
#define GPIO_CS_FRONT           BIT(5)
#define GPIO_CS_SURROUND        BIT(6)
#define GPIO_MASK_ALL           0x7fffffU

/* DMA engines are numbered by their bit in MT00, MT18, MT1A and MT1B. */
#define ENGINES                 8
#define ENG_PDMAI               0
#define ENG_RDMA0               1
#define ENG_RDMA1               2
#define ENG_PDMA1               4
#define ENG_PDMA2               5
#define ENG_PDMA3               6
#define ENG_PDMA4               7
#define ENGINE_BITS             0xf7

#define SAMPLE_BYTES            4
/* A chunk is a whole number of frames for 2, 4, 6 and 8 slots. */
#define CHUNK_DWORDS            3840
#define FLOW_NS                 (2 * SCALE_MS)
#define MAX_CATCHUP_SECONDS     1

#define EEPROM_SIZE             256
#define EEPROM_I2C_ADDRESS      0x50
#define AC97_REGS               64
#define AK4381_REGS             5
#define AK4355_REGS             11
#define AK4381_CHIP_ADDRESS     1
#define AK4355_CHIP_ADDRESS     3

typedef enum {
    DM8_VARIANT_DM8,
    DM8_VARIANT_AC97,
} DM8Variant;

typedef struct DM8Engine {
    uint32_t addr;          /* buffer base, bits 31:2 */
    uint32_t count;         /* buffer size in dwords, minus one */
    uint32_t tc;            /* interrupt interval in dwords, minus one */
    uint32_t pos;           /* dwords moved in this pass of the buffer */
    uint32_t tc_pos;        /* dwords moved in this interval */
    uint64_t frames;        /* frames moved since the clock anchor */
    int64_t anchor;         /* virtual time of the anchor, in nanoseconds */
    int64_t elapsed;        /* anchor age, valid only while migrating */
    bool active;            /* the clock is running for this engine */
} DM8Engine;

struct SGIDM8State {
    PCIDevice parent_obj;

    MemoryRegion ccs_mr;
    MemoryRegion mt_mr;
    QEMUTimer *timer;
    AudioBackend *audio_be;
    char *variant_name;
    DM8Variant variant;

    uint8_t ccs[CCS_SIZE];
    uint8_t mt[MT_SIZE];
    uint8_t svid_mask;
    uint8_t pm_state;
    uint8_t eeprom[EEPROM_SIZE];
    bool eeprom_fitted;
    DM8Engine eng[ENGINES];
    uint32_t rate;

    /* Converter pins and the two AKM write ports. */
    uint32_t gpio_pins;
    uint8_t frame_bits[2];
    uint16_t frame_word[2];
    uint8_t ak4381[AK4381_REGS];
    uint8_t ak4355[AK4355_REGS];
    uint32_t ak4381_writes;
    uint32_t ak4355_writes;
    uint32_t codec_rejects;

    uint16_t ac97[AC97_REGS];

    /* Host audio. */
    SWVoiceOut *out_analog;
    SWVoiceOut *out_spdif;
    SWVoiceIn *in_adc;
    int analog_channels;
    int analog_rate;
    int spdif_rate;
    int adc_rate;
    uint64_t analog_frames;
    uint32_t analog_last;
    uint64_t spdif_frames;
    uint64_t capture_frames;
    uint64_t dropped_frames;
};

/*
 * Register offsets of each engine: address, count and terminal count. The
 * count and terminal count of the interleaved engine are 19 bits in three
 * bytes; the stereo engines have 16 bits in two.
 */
static const struct {
    uint8_t addr, count, tc;
} dm8_engine_regs[ENGINES] = {
    [ENG_PDMAI] = { 0x10, 0x14, 0x1c },
    [ENG_RDMA0] = { 0x20, 0x24, 0x26 },
    [ENG_RDMA1] = { 0x30, 0x34, 0x36 },
    [ENG_PDMA1] = { 0x70, 0x74, 0x76 },
    [ENG_PDMA2] = { 0x60, 0x64, 0x66 },
    [ENG_PDMA3] = { 0x50, 0x54, 0x56 },
    [ENG_PDMA4] = { 0x40, 0x44, 0x46 },
};

/* Sample rates by MT01 bits 3:0; code 13 is not listed. */
static const uint32_t dm8_rates[16] = {
    48000, 24000, 12000, 9600, 32000, 16000, 8000, 96000,
    44100, 22050, 11025, 88200, 176400, 0, 192000, 64000,
};

/*
 * Approximation (datasheet, "EEPROM layout" and "Unknowns"): the DM8's own
 * EEPROM image was never captured. This image carries only what the
 * datasheet constrains: the subsystem pair byte-swapped as the public
 * Revolution 7.1 table stores it, the configuration bytes the stock driver
 * writes without an EEPROM (49.152 MHz crystal) with four stereo DACs, an I2S
 * converter, the converter pins as outputs, and CCLK, the chip selects and
 * the mute line high. The size and version bytes are the layout's own.
 */
static const uint8_t dm8_eeprom_image[] = {
    0x36, 0x30, 0x14, 0x12,         /* 0..3: subsystem pair */
    0x13, 0x02,                     /* 4: size, 5: version */
    0x43, 0x80, 0x01, 0x01,         /* 6..9: CCS04 to CCS07 */
    0x7a, 0x00, 0x40,               /* 10..12: direction, GPIO 1,3,4,5,6,22 */
    0x85, 0xff, 0xbf,               /* 13..15: write mask, those pins open */
    0x72, 0x00, 0x40,               /* 16..18: data, lines idle, not muted */
};

static void dm8_update_irq(SGIDM8State *s);
static void dm8_sync(SGIDM8State *s);
static void dm8_reclock(SGIDM8State *s);

static bool dm8_is_ac97(SGIDM8State *s)
{
    return s->variant == DM8_VARIANT_AC97;
}

/* ---- interrupts -------------------------------------------------------- */

static void dm8_update_irq(SGIDM8State *s)
{
    bool macro = s->mt[MT_STATUS] & ~s->mt[MT_MASK];

    /*
     * Inferred (datasheet, Interrupts): the macro bit is the OR of the
     * unmasked MT00 bits, and INTA# is asserted while any unmasked CCS02 bit
     * is set.
     */
    if (macro) {
        s->ccs[CCS_INT_STATUS] |= CCS_INT_MT;
    } else {
        s->ccs[CCS_INT_STATUS] &= ~CCS_INT_MT;
    }
    pci_set_irq(PCI_DEVICE(s),
                !!(s->ccs[CCS_INT_STATUS] & ~s->ccs[CCS_INT_MASK] &
                   (CCS_INT_MPU_RX | CCS_INT_MPU_TX | CCS_INT_MT)));
}

/*
 * Inferred (datasheet, MT00 and MT03): a masked source does not latch into
 * MT00.
 */
static void dm8_raise(SGIDM8State *s, unsigned bit)
{
    if (!(s->mt[MT_MASK] & BIT(bit))) {
        s->mt[MT_STATUS] |= BIT(bit);
    }
}

/* ---- DMA engines ------------------------------------------------------- */

static unsigned dm8_frame_dwords(SGIDM8State *s, unsigned idx)
{
    if (idx == ENG_PDMAI) {
        return 8 - 2 * (s->mt[MT_CHANNELS] & 3);
    }
    return 2;
}

static bool dm8_engine_available(SGIDM8State *s, unsigned idx)
{
    unsigned streams = s->mt[MT_CHANNELS] & 3;

    switch (idx) {
    case ENG_PDMA1:
        return streams == 3;
    case ENG_PDMA2:
        return streams >= 2;
    case ENG_PDMA3:
        return streams >= 1;
    default:
        return true;
    }
}

static bool dm8_engine_clocked(SGIDM8State *s, unsigned idx)
{
    return (s->mt[MT_START] & BIT(idx)) && !(s->mt[MT_PAUSE] & BIT(idx)) &&
           dm8_engine_available(s, idx) &&
           !(s->ccs[CCS_POWER_DOWN] & CCS_PD_MT_CLOCK);
}

static uint32_t dm8_count_mask(unsigned idx)
{
    return idx == ENG_PDMAI ? 0x7ffff : 0xffff;
}

static bool dm8_engine_records(unsigned idx)
{
    return idx == ENG_RDMA0 || idx == ENG_RDMA1;
}

static SWVoiceOut *dm8_out_voice(SGIDM8State *s, unsigned idx)
{
    return idx == ENG_PDMAI ? s->out_analog :
           idx == ENG_PDMA4 ? s->out_spdif : NULL;
}

/*
 * Move @dwords through one engine, no more than one chunk, and no further
 * than the end of its buffer per DMA transfer. Returns false when a PCI
 * transfer failed.
 */
static void dm8_move(SGIDM8State *s, unsigned idx, uint32_t dwords)
{
    DM8Engine *e = &s->eng[idx];
    PCIDevice *pdev = PCI_DEVICE(s);
    uint8_t buf[CHUNK_DWORDS * SAMPLE_BYTES];
    bool record = dm8_engine_records(idx);
    uint32_t total = dwords, done = 0;
    bool failed = false;
    unsigned frame = dm8_frame_dwords(s, idx);

    assert(dwords <= CHUNK_DWORDS);

    if (record) {
        size_t want = dwords * SAMPLE_BYTES, got = 0;

        if (idx == ENG_RDMA0 && s->audio_be && s->in_adc) {
            got = audio_be_read(s->audio_be, s->in_adc, buf, want);
        }
        /* Whatever the host supplies short of the request is silence. */
        memset(buf + got, 0, want - got);
    }

    while (done < total) {
        uint32_t span = MIN(total - done, e->count + 1 - e->pos);
        dma_addr_t at = (dma_addr_t)(e->addr & ~3U) + 4ULL * e->pos;
        uint32_t events, interval = e->tc + 1;

        if (record) {
            if (pci_dma_write(pdev, at, buf + done * SAMPLE_BYTES,
                              span * SAMPLE_BYTES) != MEMTX_OK) {
                failed = true;
            }
        } else if (pci_dma_read(pdev, at, buf + done * SAMPLE_BYTES,
                                span * SAMPLE_BYTES) != MEMTX_OK) {
            failed = true;
            memset(buf + done * SAMPLE_BYTES, 0, span * SAMPLE_BYTES);
        }
        done += span;
        e->pos += span;
        if (e->pos > e->count) {
            e->pos = 0;
        }
        events = (e->tc_pos + span) / interval;
        e->tc_pos = (e->tc_pos + span) % interval;
        if (events) {
            dm8_raise(s, idx);
        }
    }

    if (failed) {
        /*
         * Approximation (datasheet, "Start, stop, pause, underrun"): the
         * datasheet does not say what a failed bus transfer does. The
         * engine keeps its clock, the data is silence, the PCI status
         * records a master abort and MT1A flags an underrun or overrun.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: DMA engine %u transfer failed\n", idx);
        pci_set_word(pdev->config + PCI_STATUS,
                     pci_get_word(pdev->config + PCI_STATUS) |
                     PCI_STATUS_REC_MASTER_ABORT);
        s->mt[MT_FIFO_ERROR] |= BIT(idx);
        dm8_raise(s, 3);
    }

    if (!record && dm8_out_voice(s, idx) && s->audio_be) {
        size_t bytes = total * SAMPLE_BYTES;
        size_t put = audio_be_write(s->audio_be, dm8_out_voice(s, idx), buf,
                                    bytes);
        unsigned channels = idx == ENG_PDMAI ? frame : 2;

        s->dropped_frames += (bytes - put) / (channels * SAMPLE_BYTES);
    }
    if (idx == ENG_PDMAI) {
        if (!failed) {
            s->analog_last = ldl_le_p(buf + (total - 1) * SAMPLE_BYTES);
        }
        s->analog_frames += total / frame;
    } else if (idx == ENG_PDMA4) {
        s->spdif_frames += total / frame;
    } else if (idx == ENG_RDMA0) {
        s->capture_frames += total / frame;
    }
}

static void dm8_engine_advance(SGIDM8State *s, unsigned idx, int64_t now)
{
    DM8Engine *e = &s->eng[idx];
    unsigned frame = dm8_frame_dwords(s, idx);
    uint64_t due = muldiv64(now - e->anchor, s->rate, NANOSECONDS_PER_SECOND);

    if (due > e->frames + (uint64_t)s->rate * MAX_CATCHUP_SECONDS) {
        /*
         * Approximation: a clock jump of more than a second (a paused
         * guest, a restored snapshot) is not replayed through memory; the
         * engine resumes at the current time.
         */
        qemu_log_mask(LOG_UNIMP, "sgi-dm8: DMA clock jump skipped\n");
        e->frames = due;
        return;
    }
    while (e->frames < due) {
        uint32_t frames = MIN(due - e->frames, CHUNK_DWORDS / frame);

        dm8_move(s, idx, frames * frame);
        e->frames += frames;
    }
}

/* Bring every running engine up to the present. */
static void dm8_sync(SGIDM8State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < ENGINES; i++) {
        if (s->eng[i].active) {
            dm8_engine_advance(s, i, now);
        }
    }
    dm8_update_irq(s);
}

/* Time at which frame @frames of an engine is moved, rounded up. */
static int64_t dm8_frame_time(SGIDM8State *s, DM8Engine *e, uint64_t frames)
{
    uint64_t ns = muldiv64(frames, NANOSECONDS_PER_SECOND, s->rate);

    if (muldiv64(ns, s->rate, NANOSECONDS_PER_SECOND) < frames) {
        ns++;
    }
    return e->anchor + ns;
}

static void dm8_arm_timer(SGIDM8State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = INT64_MAX;

    for (unsigned i = 0; i < ENGINES; i++) {
        DM8Engine *e = &s->eng[i];
        int64_t when;
        uint64_t frames;

        if (!e->active) {
            continue;
        }
        when = now + FLOW_NS;
        if (!(s->mt[MT_MASK] & BIT(i))) {
            frames = DIV_ROUND_UP(e->tc - e->tc_pos + 1,
                                  dm8_frame_dwords(s, i));
            when = MIN(when, MAX(dm8_frame_time(s, e, e->frames + frames),
                                 now));
        }
        next = MIN(next, when);
    }
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static void dm8_audio_callback(void *opaque, int avail)
{
    /* The engines are paced by the virtual clock, not by the host. */
}

/* Open, retune or silence the host voices for the engines' present state. */
static void dm8_voices_update(SGIDM8State *s)
{
    struct audsettings as = {
        .fmt = AUDIO_FORMAT_S32,
        .big_endian = false,
        .freq = s->rate,
    };
    int channels = dm8_frame_dwords(s, ENG_PDMAI);
    bool analog = s->eng[ENG_PDMAI].active;
    bool spdif = s->eng[ENG_PDMA4].active;
    bool adc = s->eng[ENG_RDMA0].active;

    if (!s->audio_be) {
        return;
    }
    if (analog && (!s->out_analog || s->analog_rate != s->rate ||
                   s->analog_channels != channels)) {
        as.nchannels = channels;
        s->out_analog = audio_be_open_out(s->audio_be, s->out_analog,
                                          "sgi-dm8.analog", s,
                                          dm8_audio_callback, &as);
        s->analog_channels = channels;
        s->analog_rate = s->rate;
    }
    if (spdif && (!s->out_spdif || s->spdif_rate != s->rate)) {
        as.nchannels = 2;
        s->out_spdif = audio_be_open_out(s->audio_be, s->out_spdif,
                                         "sgi-dm8.spdif", s,
                                         dm8_audio_callback, &as);
        s->spdif_rate = s->rate;
    }
    if (adc && (!s->in_adc || s->adc_rate != s->rate)) {
        as.nchannels = 2;
        s->in_adc = audio_be_open_in(s->audio_be, s->in_adc, "sgi-dm8.adc", s,
                                     dm8_audio_callback, &as);
        s->adc_rate = s->rate;
    }
    if (s->out_analog) {
        audio_be_set_active_out(s->audio_be, s->out_analog, analog);
    }
    if (s->out_spdif) {
        audio_be_set_active_out(s->audio_be, s->out_spdif, spdif);
    }
    if (s->in_adc) {
        audio_be_set_active_in(s->audio_be, s->in_adc, adc);
    }
}

static void dm8_close_voices(SGIDM8State *s)
{
    if (!s->audio_be) {
        return;
    }
    audio_be_close_out(s->audio_be, s->out_analog);
    audio_be_close_out(s->audio_be, s->out_spdif);
    audio_be_close_in(s->audio_be, s->in_adc);
    s->out_analog = NULL;
    s->out_spdif = NULL;
    s->in_adc = NULL;
}

/*
 * Follow a change of start, pause, availability, rate or clock gating:
 * engines that begin running anchor their clock at the present, and every
 * running engine's anchor restarts when @restart is set. Callers sync
 * before they change the state.
 */
static void dm8_clock_changed(SGIDM8State *s, bool restart)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < ENGINES; i++) {
        DM8Engine *e = &s->eng[i];
        bool clocked = dm8_engine_clocked(s, i);

        if (clocked && (!e->active || restart)) {
            e->anchor = now;
            e->frames = 0;
        }
        e->active = clocked;
    }
    dm8_voices_update(s);
    dm8_arm_timer(s);
}

static void dm8_reclock(SGIDM8State *s)
{
    dm8_clock_changed(s, false);
}

static void dm8_timer(void *opaque)
{
    SGIDM8State *s = opaque;

    dm8_sync(s);
    dm8_arm_timer(s);
}

/* ---- sample rate ------------------------------------------------------- */

static void dm8_apply_rate(SGIDM8State *s)
{
    unsigned code = s->mt[MT_RATE] & 0xf;
    uint32_t rate = dm8_rates[code];

    if (s->mt[MT_RATE] & 0x10) {
        /*
         * Approximation (datasheet, MT01): with the S/PDIF-input clock as
         * master the rate comes from an external clock the model has no
         * source for, so the rate code still sets the pace.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: external master clock is not modelled\n");
    }
    if (!rate) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: reserved sample rate code %u\n", code);
        return;
    }
    if (code == 14 &&
        !(s->ccs[CCS_SYS_CONFIG] & CCS_SYS_CRYSTAL_49) &&
        !(s->mt[MT_FORMAT] & MT_FORMAT_128X)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: 192 kHz needs a 49.152 MHz crystal or 128x\n");
    }
    s->rate = rate;
}

/* ---- converters behind the GPIO pins ----------------------------------- */

static uint32_t dm8_gpio_read(SGIDM8State *s)
{
    return s->ccs[CCS_GPIO_DATA_LO] | s->ccs[CCS_GPIO_DATA_LO + 1] << 8 |
           (uint32_t)(s->ccs[CCS_GPIO_DATA_HI] & 0x7f) << 16;
}

static uint32_t dm8_gpio_direction(SGIDM8State *s)
{
    return (s->ccs[CCS_GPIO_DIR] | s->ccs[CCS_GPIO_DIR + 1] << 8 |
            s->ccs[CCS_GPIO_DIR + 2] << 16) & GPIO_MASK_ALL;
}

/*
 * The level on each pin: an output drives its last written value. An input
 * floats to its weak pull-up. Approximation (datasheet, GPIO block): what an
 * input pin reads is "the pin", inferred, and the board's pull-ups and
 * strapping are not documented.
 */
static uint32_t dm8_gpio_pins(SGIDM8State *s)
{
    uint32_t dir = dm8_gpio_direction(s);

    return (dm8_gpio_read(s) & dir) | (GPIO_MASK_ALL & ~dir);
}

static uint8_t *dm8_ak_reg(SGIDM8State *s, unsigned chip, unsigned reg)
{
    if (chip == AK4381_CHIP_ADDRESS) {
        return reg < AK4381_REGS ? &s->ak4381[reg] : NULL;
    }
    return reg < AK4355_REGS ? &s->ak4355[reg] : NULL;
}

static void dm8_codec_latch(SGIDM8State *s, unsigned port)
{
    unsigned chip = port ? AK4355_CHIP_ADDRESS : AK4381_CHIP_ADDRESS;
    unsigned header = s->frame_word[port] >> 8;
    unsigned reg = header & 0x1f;
    uint8_t *slot;

    if (!s->frame_bits[port]) {
        /* A select pulse with no clock is not a frame. */
        return;
    }
    if (s->frame_bits[port] != 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: converter frame of %u bits ignored\n",
                      s->frame_bits[port]);
        s->codec_rejects++;
        return;
    }
    /* The chip address is fixed in each part and the frame is write-only. */
    if ((header >> 6) != chip || !(header & 0x20)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: converter frame 0x%04x is not a write to "
                      "chip %u\n", s->frame_word[port], chip);
        s->codec_rejects++;
        return;
    }
    slot = dm8_ak_reg(s, chip, reg);
    if (!slot) {
        /* The parts say these addresses "must not be written". */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: write to undefined register %u of chip %u\n",
                      reg, chip);
        s->codec_rejects++;
        return;
    }
    *slot = s->frame_word[port] & 0xff;
    if (chip == AK4381_CHIP_ADDRESS) {
        s->ak4381_writes++;
    } else {
        s->ak4355_writes++;
    }
}

/*
 * Follow the converter pins after a GPIO register change. Each part sees the
 * shared CCLK and CDOUT lines and its own select: a frame starts when the
 * select falls, takes a bit on each rising CCLK edge while the select is
 * held across the change, and takes effect when the select rises. Edges
 * that arrive in one register write are taken as the datasheet's timing
 * orders them: a select that rises or falls with CCLK does not see that
 * clock edge. Approximation (datasheet, "Serial register write"): the parts
 * ignore writes while their PDN pin is low or MCLK is absent, and how the
 * board wires PDN is unknown, so every complete frame is accepted and
 * recorded.
 */
static void dm8_codec_pins(SGIDM8State *s)
{
    static const uint32_t selects[2] = { GPIO_CS_FRONT, GPIO_CS_SURROUND };
    uint32_t pins = dm8_gpio_pins(s);
    uint32_t old = s->gpio_pins;
    bool clock_rise = (pins & GPIO_CCLK) && !(old & GPIO_CCLK);

    s->gpio_pins = pins;
    if (dm8_is_ac97(s)) {
        return;
    }

    for (unsigned port = 0; port < 2; port++) {
        bool was = !(old & selects[port]);
        bool now = !(pins & selects[port]);

        if (clock_rise && was && now) {
            if (s->frame_bits[port] < 16) {
                s->frame_word[port] = s->frame_word[port] << 1 |
                                      !!(pins & GPIO_CDOUT);
            }
            if (s->frame_bits[port] < 255) {
                s->frame_bits[port]++;
            }
        }
        if (now && !was) {
            s->frame_bits[port] = 0;
            s->frame_word[port] = 0;
        } else if (was && !now) {
            dm8_codec_latch(s, port);
            s->frame_bits[port] = 0;
        }
    }
}

/* ---- I2C port and EEPROM ----------------------------------------------- */

static void dm8_i2c_start(SGIDM8State *s)
{
    uint8_t cmd = s->ccs[CCS_I2C_DEVICE];
    unsigned device = cmd >> 1;
    bool write = cmd & 1;

    /*
     * Approximation (datasheet, "I2C port and EEPROM"): a transfer finishes
     * at once, where hardware takes about a third of a millisecond. A
     * transfer to an address nothing answers is unknown (NAK handling is not
     * described): it is logged and leaves the data register alone.
     */
    if (device != EEPROM_I2C_ADDRESS || !s->eeprom_fitted) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: I2C %s of device 0x%02x has no answer "
                      "modelled\n", write ? "write" : "read", device);
        return;
    }
    if (write) {
        s->eeprom[s->ccs[CCS_I2C_BYTE]] = s->ccs[CCS_I2C_DATA];
    } else {
        s->ccs[CCS_I2C_DATA] = s->eeprom[s->ccs[CCS_I2C_BYTE]];
    }
}

/* ---- AC'97 port -------------------------------------------------------- */

static bool dm8_ac97_codec_ready(SGIDM8State *s)
{
    return dm8_is_ac97(s) && !(s->ccs[CCS_AC_CONFIG] & CCS_AC_I2S) &&
           !(s->mt[MT_AC97_CMD] & MT_AC97_COLD_RESET);
}

static void dm8_ac97_cycle(SGIDM8State *s, uint8_t cmd)
{
    unsigned index = (s->mt[MT_AC97_INDEX] & 0x7e) >> 1;
    uint16_t *reg = &s->ac97[index];

    if (s->ccs[CCS_AC_CONFIG] & CCS_AC_I2S) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: AC'97 cycle in I2S mode has no meaning\n");
        return;
    }
    if (!dm8_ac97_codec_ready(s) || (cmd & 3)) {
        /*
         * Approximation (datasheet, "AC'97 variant"): only one codec,
         * ready after the cold reset is released, exists. A cycle with no
         * codec to answer does nothing and reads back zero.
         */
        qemu_log_mask(LOG_UNIMP, "sgi-dm8: AC'97 cycle with no codec to "
                      "answer (command 0x%02x)\n", cmd);
        if (cmd & MT_AC97_READ) {
            s->mt[MT_AC97_DATA] = 0;
            s->mt[MT_AC97_DATA + 1] = 0;
        }
        return;
    }
    if (cmd & MT_AC97_WRITE) {
        *reg = s->mt[MT_AC97_DATA] | s->mt[MT_AC97_DATA + 1] << 8;
    } else {
        s->mt[MT_AC97_DATA] = *reg & 0xff;
        s->mt[MT_AC97_DATA + 1] = *reg >> 8;
    }
}

/* ---- reset ------------------------------------------------------------- */

static void dm8_reset_engines(SGIDM8State *s)
{
    for (unsigned i = 0; i < ENGINES; i++) {
        memset(&s->eng[i], 0, sizeof(s->eng[i]));
    }
}

static void dm8_reset_registers(SGIDM8State *s, bool soft)
{
    uint8_t keep[4];

    /*
     * Approximation (datasheet, "Access conventions"): the soft reset in
     * CCS00 is not described for the ICE1724. The VT1720 sibling datasheet
     * restores every register except configuration space, CCS00 and
     * CCS04 to CCS07, which is done here.
     */
    memcpy(keep, &s->ccs[CCS_SYS_CONFIG], sizeof(keep));
    memset(s->ccs, 0, sizeof(s->ccs));
    memset(s->mt, 0, sizeof(s->mt));
    s->ccs[CCS_INT_MASK] = 0xfe;
    s->ccs[CCS_SYS_CONFIG] = 0x0f;
    s->ccs[CCS_I2S_FEATURES] = 0x01;
    s->ccs[CCS_SPDIF_CONFIG] = 0x01;
    s->ccs[CCS_GPIO_MASK_LO] = 0xff;
    s->ccs[CCS_GPIO_MASK_LO + 1] = 0xff;
    s->ccs[CCS_GPIO_MASK_HI] = 0xff;
    if (s->eeprom_fitted) {
        s->ccs[CCS_I2C_STATUS] = CCS_I2C_EEPROM;
    }
    s->mt[MT_MASK] = 0xff;
    s->rate = 48000;
    dm8_reset_engines(s);
    s->gpio_pins = dm8_gpio_pins(s);
    memset(s->frame_bits, 0, sizeof(s->frame_bits));
    if (soft) {
        memcpy(&s->ccs[CCS_SYS_CONFIG], keep, sizeof(keep));
        s->ccs[CCS_CONTROL] = CCS_CONTROL_SOFT_RESET;
    }
}

static void dm8_reset_codecs(SGIDM8State *s)
{
    memset(s->ak4381, 0, sizeof(s->ak4381));
    memset(s->ak4355, 0, sizeof(s->ak4355));
    /* Both parts reset their attenuation registers to full level. */
    s->ak4381[3] = s->ak4381[4] = 0xff;
    memset(&s->ak4355[4], 0xff, 6);
    memset(s->ac97, 0, sizeof(s->ac97));
}

static void dm8_config_defaults(SGIDM8State *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);

    pdev->config[PCI_LATENCY_TIMER] = 0;
    pdev->config[PCI_INTERRUPT_LINE] = 0xff;
    pci_set_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID, DM8_VENDOR_ID);
    pci_set_word(pdev->config + PCI_SUBSYSTEM_ID,
                 s->eeprom_fitted ? DM8_SUBSYSTEM_ID :
                                    DM8_NO_EEPROM_SUBSYSTEM_ID);
    pdev->config[PCI_SVID_MASK_OFFSET] = 0;
    s->svid_mask = 0;
    s->pm_state = 0;
}

static ResettablePhases dm8_parent_phases;

static void dm8_reset_exit(Object *obj, ResetType type)
{
    SGIDM8State *s = SGI_DM8(obj);

    if (dm8_parent_phases.exit) {
        dm8_parent_phases.exit(obj, type);
    }
    /* After the bus reset, which would leave the interrupt line at zero. */
    dm8_config_defaults(s);
}

static void dm8_reset_hold(Object *obj, ResetType type)
{
    SGIDM8State *s = SGI_DM8(obj);

    if (dm8_parent_phases.hold) {
        dm8_parent_phases.hold(obj, type);
    }
    dm8_reset_registers(s, false);
    dm8_reset_codecs(s);
    dm8_clock_changed(s, true);
    dm8_update_irq(s);
}

/* ---- controller block registers (BAR0) --------------------------------- */

static uint64_t dm8_ccs_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIDM8State *s = opaque;
    uint8_t value = s->ccs[addr];

    switch (addr) {
    case CCS_INT_STATUS:
        dm8_sync(s);
        value = s->ccs[addr];
        break;
    case CCS_UART_TX_COUNT:
    case CCS_UART_RX_COUNT:
        value = 0;
        break;
    case CCS_UART_DATA:
    case CCS_UART_COMMAND:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: MPU-401 UART read at CCS 0x%02x\n",
                      (unsigned)addr);
        break;
    case CCS_GPIO_DATA_LO:
    case CCS_GPIO_DATA_LO + 1:
    case CCS_GPIO_DATA_HI:
        value = dm8_gpio_pins(s) >> ((addr == CCS_GPIO_DATA_HI ? 16 :
                                      (addr - CCS_GPIO_DATA_LO) * 8)) & 0xff;
        break;
    case CCS_GPIO_DIR + 2:
        value &= 0x7f;
        break;
    case 0x03: case 0x08: case 0x09: case 0x0f: case 0x1b: case 0x1d:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: read of unassigned CCS 0x%02x\n",
                      (unsigned)addr);
        value = 0;
        break;
    default:
        break;
    }
    return value;
}

static void dm8_soft_reset(SGIDM8State *s)
{
    dm8_sync(s);
    dm8_reset_registers(s, true);
    dm8_clock_changed(s, true);
    dm8_update_irq(s);
}

static void dm8_ccs_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SGIDM8State *s = opaque;
    uint8_t v = value;

    switch (addr) {
    case CCS_CONTROL:
        s->ccs[addr] = v;
        if (v & CCS_CONTROL_SOFT_RESET) {
            dm8_soft_reset(s);
        }
        break;
    case CCS_INT_MASK:
        s->ccs[addr] = v;
        dm8_update_irq(s);
        break;
    case CCS_INT_STATUS:
        /* Write 1 to clear; the macro bit follows MT00. */
        s->ccs[addr] &= ~(v & CCS_INT_STATUS_WRITABLE);
        dm8_update_irq(s);
        break;
    case CCS_SYS_CONFIG:
    case CCS_I2S_FEATURES:
    case CCS_I2C_BYTE:
    case CCS_I2C_DATA:
        s->ccs[addr] = v;
        break;
    case CCS_GPIO_DIR:
    case CCS_GPIO_DIR + 1:
        s->ccs[addr] = v;
        dm8_codec_pins(s);
        break;
    case CCS_AC_CONFIG:
        s->ccs[addr] = v & 0x82;
        break;
    case CCS_SPDIF_CONFIG:
        /* Bit 6, the transmitter-implemented flag, is read only. */
        s->ccs[addr] = (s->ccs[addr] & 0x40) | (v & 0xbf);
        break;
    case CCS_UART_DATA:
    case CCS_UART_COMMAND:
    case CCS_UART_SETTING:
        s->ccs[addr] = v;
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: MPU-401 UART write at CCS 0x%02x\n",
                      (unsigned)addr);
        break;
    case CCS_UART_TX_COUNT:
    case CCS_UART_RX_COUNT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: write to read-only CCS 0x%02x\n",
                      (unsigned)addr);
        break;
    case CCS_I2C_DEVICE:
        s->ccs[addr] = v;
        dm8_i2c_start(s);
        break;
    case CCS_I2C_STATUS:
        /* Bit 7 is the EEPROM strap and bit 0 is busy: both read only. */
        s->ccs[addr] = (s->ccs[addr] & 0x81) | (v & 0x02);
        break;
    case CCS_GPIO_DATA_LO:
    case CCS_GPIO_DATA_LO + 1:
        /* Only the bits whose write-mask bit is clear take the value. */
        s->ccs[addr] = (s->ccs[addr] & s->ccs[CCS_GPIO_MASK_LO +
                                              (addr - CCS_GPIO_DATA_LO)]) |
                       (v & ~s->ccs[CCS_GPIO_MASK_LO +
                                    (addr - CCS_GPIO_DATA_LO)]);
        dm8_codec_pins(s);
        break;
    case CCS_GPIO_DATA_HI:
        s->ccs[addr] = (s->ccs[addr] & s->ccs[CCS_GPIO_MASK_HI]) |
                       (v & ~s->ccs[CCS_GPIO_MASK_HI] & 0x7f);
        dm8_codec_pins(s);
        break;
    case CCS_GPIO_MASK_LO:
    case CCS_GPIO_MASK_LO + 1:
    case CCS_GPIO_MASK_HI:
        s->ccs[addr] = v;
        break;
    case CCS_GPIO_DIR + 2:
        s->ccs[addr] = v & 0x7f;
        dm8_codec_pins(s);
        break;
    case CCS_POWER_DOWN:
        dm8_sync(s);
        s->ccs[addr] = v & 0xbd;
        dm8_reclock(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: write of 0x%02x to unassigned CCS 0x%02x\n",
                      v, (unsigned)addr);
        break;
    }
}

static const MemoryRegionOps dm8_ccs_ops = {
    .read = dm8_ccs_read,
    .write = dm8_ccs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* ---- multi-channel block registers (BAR1) ------------------------------ */

/* Decode an engine register byte; returns false for any other offset. */
static bool dm8_engine_decode(hwaddr addr, unsigned *idx, unsigned *kind,
                              unsigned *byte)
{
    for (unsigned i = 0; i < ENGINES; i++) {
        unsigned wide = i == ENG_PDMAI ? 3 : 2;

        if (!dm8_engine_regs[i].addr) {
            continue;
        }
        if (addr >= dm8_engine_regs[i].addr &&
            addr < dm8_engine_regs[i].addr + 4U) {
            *idx = i; *kind = 0; *byte = addr - dm8_engine_regs[i].addr;
            return true;
        }
        if (addr >= dm8_engine_regs[i].count &&
            addr < dm8_engine_regs[i].count + wide) {
            *idx = i; *kind = 1; *byte = addr - dm8_engine_regs[i].count;
            return true;
        }
        if (addr >= dm8_engine_regs[i].tc &&
            addr < dm8_engine_regs[i].tc + wide) {
            *idx = i; *kind = 2; *byte = addr - dm8_engine_regs[i].tc;
            return true;
        }
    }
    return false;
}

static uint64_t dm8_mt_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIDM8State *s = opaque;
    unsigned idx, kind, byte;
    uint8_t value;

    dm8_sync(s);
    if (dm8_engine_decode(addr, &idx, &kind, &byte)) {
        DM8Engine *e = &s->eng[idx];
        uint32_t word;

        switch (kind) {
        case 0:
            word = (e->addr & ~3U) + 4U * e->pos;
            return word >> (8 * byte) & 0xff;
        case 1:
            word = e->count - e->pos;
            return word >> (8 * byte) & 0xff;
        default:
            /* Approximation: terminal counts are write only; read as 0. */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-dm8: read of write-only terminal count 0x%02x\n",
                          (unsigned)addr);
            return 0;
        }
    }

    value = s->mt[addr];
    switch (addr) {
    case MT_STATUS:
    case MT_RATE:
    case MT_FORMAT:
    case MT_MASK:
    case MT_START:
    case MT_CHANNELS:
    case MT_FIFO_ERROR:
    case MT_PAUSE:
    case MT_PEAK_INDEX:
    case MT_ROUTING ... MT_ROUTING + 3:
    case MT_SPDIF_CTRL:
    case MT_SPDIF_CTRL + 1:
        break;
    case MT_AC97_INDEX:
    case MT_AC97_DATA:
    case MT_AC97_DATA + 1:
        if (!dm8_is_ac97(s) || (s->ccs[CCS_AC_CONFIG] & CCS_AC_I2S)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-dm8: read of AC'97 register 0x%02x with no "
                          "AC-link codec\n", (unsigned)addr);
        }
        break;
    case MT_AC97_CMD:
        value &= ~(MT_AC97_READY | 0x34);
        if (dm8_ac97_codec_ready(s)) {
            value |= MT_AC97_READY;
        }
        break;
    case MT_PEAK_DATA:
        /* Approximation: the peak meters are not modelled and read 0. */
        qemu_log_mask(LOG_UNIMP, "sgi-dm8: peak meters are not modelled\n");
        value = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-dm8: read of unassigned MT 0x%02x\n",
                      (unsigned)addr);
        value = 0;
        break;
    }
    return value;
}

static void dm8_engine_write(SGIDM8State *s, unsigned idx, unsigned kind,
                             unsigned byte, uint8_t v)
{
    DM8Engine *e = &s->eng[idx];
    uint32_t *reg = kind == 0 ? &e->addr : kind == 1 ? &e->count : &e->tc;
    uint32_t mask = kind == 0 ? 0xffffffffU : dm8_count_mask(idx);

    /*
     * Approximation (datasheet, "Address, count and terminal-count
     * registers"): the register pair is "current/base" and the effect of a
     * write to a running engine is not stated, so a write reloads the
     * engine's position and interval from the new values.
     */
    *reg = (*reg & ~(0xffU << (8 * byte))) | (uint32_t)v << (8 * byte);
    *reg &= mask;
    if (kind == 0) {
        *reg &= ~3U;
    }
    if (kind == 2) {
        e->tc_pos = 0;
    } else {
        e->pos = 0;
    }
}

static void dm8_mt_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    SGIDM8State *s = opaque;
    unsigned idx, kind, byte;
    uint8_t v = value;

    dm8_sync(s);
    if (dm8_engine_decode(addr, &idx, &kind, &byte)) {
        dm8_engine_write(s, idx, kind, byte, v);
        dm8_arm_timer(s);
        return;
    }

    switch (addr) {
    case MT_STATUS:
        s->mt[MT_STATUS] &= ~v;
        break;
    case MT_RATE:
        s->mt[addr] = v & 0x1f;
        dm8_apply_rate(s);
        dm8_clock_changed(s, true);
        break;
    case MT_FORMAT:
        s->mt[addr] = v & 0x0f;
        if (v & 3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-dm8: reserved data format %u\n", v & 3);
        }
        if (v & 4) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-dm8: MT02 bit 2 has no known meaning\n");
        }
        break;
    case MT_MASK:
        s->mt[addr] = v;
        dm8_arm_timer(s);
        break;
    case MT_AC97_INDEX:
        s->mt[addr] = v & 0x7f;
        break;
    case MT_AC97_DATA:
    case MT_AC97_DATA + 1:
        s->mt[addr] = v;
        break;
    case MT_AC97_CMD:
        s->mt[addr] = (v & 0xc3);
        /*
         * Bit 7 drives PRST# to the converters in I2S mode too (inferred
         * from the sibling datasheet and both public drivers); no converter
         * reacts to it in this model.
         */
        if (v & (MT_AC97_WRITE | MT_AC97_READ)) {
            /*
             * Approximation: a cycle completes at once, where hardware
             * reads the in-progress bit as 1 until the AC-link frame ends.
             */
            dm8_ac97_cycle(s, v);
        }
        break;
    case MT_START:
        if (v & ~ENGINE_BITS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-dm8: MT18 bit 3 is reserved\n");
        }
        s->mt[addr] = v & ENGINE_BITS;
        for (unsigned i = 0; i < ENGINES; i++) {
            if ((v & BIT(i)) && i != 3 && !dm8_engine_available(s, i)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-dm8: engine %u started while MT19 gives "
                              "its slots to the interleaved stream\n", i);
            }
        }
        dm8_reclock(s);
        break;
    case MT_CHANNELS:
        s->mt[addr] = v & 3;
        dm8_clock_changed(s, true);
        break;
    case MT_FIFO_ERROR:
        s->mt[addr] &= ~(v & ENGINE_BITS);
        break;
    case MT_PAUSE:
        s->mt[addr] = v & ENGINE_BITS;
        dm8_reclock(s);
        break;
    case MT_ROUTING ... MT_ROUTING + 3:
        s->mt[addr] = v;
        if (v) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-dm8: output routing is not modelled\n");
        }
        break;
    case MT_SPDIF_CTRL:
    case MT_SPDIF_CTRL + 1:
        /*
         * The datasheet says every field except the validity flag (bit 15)
         * must be written only while the transmitter is disabled.
         * Approximation: the model enforces that rule, keeping the flag
         * writable and refusing the other bits with a log line.
         */
        if (s->ccs[CCS_SPDIF_CONFIG] & 0x80) {
            if (addr == MT_SPDIF_CTRL) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-dm8: MT3C fields written while the S/PDIF "
                              "transmitter is enabled\n");
                break;
            }
            if ((v ^ s->mt[addr]) & 0x7f) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-dm8: MT3C fields written while the S/PDIF "
                              "transmitter is enabled\n");
            }
            v = (s->mt[addr] & 0x7f) | (v & 0x80);
        }
        s->mt[addr] = v;
        break;
    case MT_PEAK_INDEX:
        s->mt[addr] = v & 0x1f;
        break;
    case MT_PEAK_DATA:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-dm8: write to read-only peak data\n");
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: write of 0x%02x to unassigned MT 0x%02x\n",
                      v, (unsigned)addr);
        break;
    }
    dm8_update_irq(s);
}

static const MemoryRegionOps dm8_mt_ops = {
    .read = dm8_mt_read,
    .write = dm8_mt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* ---- PCI configuration space ------------------------------------------- */

/*
 * The datasheet lists registers it does not describe: BARs 2 and 3, the
 * subsystem mask at its other offset (0x40, 0x41), 0x43, and the hardware
 * configuration control at 0x60. Accesses to them are logged.
 */
static bool dm8_config_unknown(uint32_t addr, int len)
{
    static const struct { uint32_t first, last; } unknown[] = {
        { 0x18, 0x1f }, { 0x40, 0x41 }, { 0x43, 0x43 }, { 0x60, 0x63 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(unknown); i++) {
        if (addr <= unknown[i].last && addr + len - 1 >= unknown[i].first) {
            return true;
        }
    }
    return false;
}

static uint32_t dm8_config_read(PCIDevice *pdev, uint32_t addr, int len)
{
    if (dm8_config_unknown(addr, len)) {
        qemu_log_mask(LOG_UNIMP, "sgi-dm8: PCI config read of unknown "
                      "register(s) at 0x%02x, %d bytes\n", addr, len);
    }
    return pci_default_read_config(pdev, addr, len);
}

static void dm8_config_write(PCIDevice *pdev, uint32_t addr, uint32_t val,
                             int len)
{
    SGIDM8State *s = SGI_DM8(pdev);
    uint16_t ids[2];
    unsigned old_state = s->pm_state;

    if (dm8_config_unknown(addr, len)) {
        qemu_log_mask(LOG_UNIMP, "sgi-dm8: PCI config write of unknown "
                      "register(s) at 0x%02x, %d bytes\n", addr, len);
    }

    ids[0] = pci_get_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID);
    ids[1] = pci_get_word(pdev->config + PCI_SUBSYSTEM_ID);
    pci_default_write_config(pdev, addr, val, len);

    /* The subsystem IDs are writable only while PCI42 bit 7 is set. */
    s->svid_mask = pdev->config[PCI_SVID_MASK_OFFSET];
    if (!(s->svid_mask & 0x80)) {
        pci_set_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID, ids[0]);
        pci_set_word(pdev->config + PCI_SUBSYSTEM_ID, ids[1]);
    }

    /*
     * Power management state, bits 1:0 of PMCSR. The PCI core rejects D1,
     * which the capability does not advertise, and keeps both BARs closed
     * outside D0.
     */
    s->pm_state = pdev->config[PM_CAP_OFFSET + PCI_PM_CTRL] & 3;
    if (old_state == 3 && s->pm_state == 0) {
        /* D3hot to D0 resets the chip including configuration space. */
        dm8_sync(s);
        pci_device_reset(pdev);
        dm8_config_defaults(s);
    } else if (!old_state && s->pm_state) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-dm8: D2 and D3hot power-down of the blocks in "
                      "CCS1C is not modelled beyond the clock gate\n");
    }
}

/* ---- device ------------------------------------------------------------ */

static void dm8_realize(PCIDevice *pdev, Error **errp)
{
    SGIDM8State *s = SGI_DM8(pdev);
    Error *local_err = NULL;
    uint8_t *wmask = pdev->wmask;
    int cap;

    if (!s->variant_name || !strcmp(s->variant_name, "dm8")) {
        s->variant = DM8_VARIANT_DM8;
    } else if (!strcmp(s->variant_name, "ac97")) {
        s->variant = DM8_VARIANT_AC97;
    } else {
        error_setg(errp, "sgi-dm8: variant must be dm8 or ac97, not %s",
                   s->variant_name);
        return;
    }
    /*
     * The DM8 carries a configuration EEPROM (a device answers at 0xa0, its
     * image is an approximation). The AC'97 variant's subsystem ID and
     * EEPROM are unknown; it is modelled as a board with no EEPROM, which
     * gives the datasheet's default subsystem pair.
     */
    s->eeprom_fitted = s->variant == DM8_VARIANT_DM8;
    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    if (s->eeprom_fitted) {
        memcpy(s->eeprom, dm8_eeprom_image, sizeof(dm8_eeprom_image));
    }

    if (!audio_be_check(&s->audio_be, &local_err)) {
        warn_report_err(local_err);
        s->audio_be = NULL;
    }

    /* Registers are read only past the header, except what is allowed. */
    memset(wmask + PCI_CONFIG_HEADER_SIZE, 0, 0x100 - PCI_CONFIG_HEADER_SIZE);
    wmask[PCI_CACHE_LINE_SIZE] = 0;
    wmask[PCI_LATENCY_TIMER] = 0xf8;
    wmask[PCI_INTERRUPT_LINE] = 0xff;
    pci_set_word(wmask + PCI_COMMAND, PCI_COMMAND_IO | PCI_COMMAND_MASTER);
    pci_set_word(pdev->w1cmask + PCI_STATUS,
                 PCI_STATUS_PARITY | PCI_STATUS_REC_MASTER_ABORT |
                 PCI_STATUS_REC_TARGET_ABORT | PCI_STATUS_SIG_SYSTEM_ERROR);
    wmask[PCI_SVID_MASK_OFFSET] = 0x80;
    /* Writable only while the mask bit is set; see dm8_config_write(). */
    memset(wmask + PCI_SUBSYSTEM_VENDOR_ID, 0xff, 4);

    pci_config_set_prog_interface(pdev->config, 0);
    pdev->config[PCI_INTERRUPT_PIN] = 1;

    cap = pci_pm_init(pdev, PM_CAP_OFFSET, errp);
    if (cap < 0) {
        return;
    }
    /* Version 1.0, D2 supported, D1 not, no PME. */
    pci_set_word(pdev->config + cap + PCI_PM_PMC, 0x0401);
    pci_set_word(pdev->wmask + cap + PCI_PM_CTRL, 3);
    /* Medium DEVSEL timing; the capability list bit is set above. */
    pci_set_word(pdev->config + PCI_STATUS,
                 pci_get_word(pdev->config + PCI_STATUS) |
                 PCI_STATUS_DEVSEL_MEDIUM);

    memory_region_init_io(&s->ccs_mr, OBJECT(s), &dm8_ccs_ops, s,
                          "sgi-dm8.ccs", CCS_SIZE);
    memory_region_init_io(&s->mt_mr, OBJECT(s), &dm8_mt_ops, s,
                          "sgi-dm8.mt", MT_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->ccs_mr);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->mt_mr);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm8_timer, s);
    dm8_config_defaults(s);
}

static void dm8_exit(PCIDevice *pdev)
{
    SGIDM8State *s = SGI_DM8(pdev);

    timer_free(s->timer);
    s->timer = NULL;
    dm8_close_voices(s);
}

static int dm8_pre_save(void *opaque)
{
    SGIDM8State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < ENGINES; i++) {
        s->eng[i].elapsed = s->eng[i].active ? now - s->eng[i].anchor : 0;
    }
    return 0;
}

static int dm8_post_load(void *opaque, int version_id)
{
    SGIDM8State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (unsigned i = 0; i < ENGINES; i++) {
        s->eng[i].anchor = now - s->eng[i].elapsed;
    }
    s->gpio_pins = dm8_gpio_pins(s);
    dm8_voices_update(s);
    dm8_arm_timer(s);
    dm8_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_dm8_engine = {
    .name = "sgi-dm8/engine",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(addr, DM8Engine),
        VMSTATE_UINT32(count, DM8Engine),
        VMSTATE_UINT32(tc, DM8Engine),
        VMSTATE_UINT32(pos, DM8Engine),
        VMSTATE_UINT32(tc_pos, DM8Engine),
        VMSTATE_UINT64(frames, DM8Engine),
        VMSTATE_INT64(elapsed, DM8Engine),
        VMSTATE_BOOL(active, DM8Engine),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_dm8 = {
    .name = "sgi-dm8",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = dm8_pre_save,
    .post_load = dm8_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIDM8State),
        VMSTATE_UINT8_ARRAY(ccs, SGIDM8State, CCS_SIZE),
        VMSTATE_UINT8_ARRAY(mt, SGIDM8State, MT_SIZE),
        VMSTATE_UINT8(svid_mask, SGIDM8State),
        VMSTATE_UINT8(pm_state, SGIDM8State),
        VMSTATE_UINT8_ARRAY(eeprom, SGIDM8State, EEPROM_SIZE),
        VMSTATE_STRUCT_ARRAY(eng, SGIDM8State, ENGINES, 1,
                             vmstate_dm8_engine, DM8Engine),
        VMSTATE_UINT32(rate, SGIDM8State),
        VMSTATE_UINT8_ARRAY(frame_bits, SGIDM8State, 2),
        VMSTATE_UINT16_ARRAY(frame_word, SGIDM8State, 2),
        VMSTATE_UINT8_ARRAY(ak4381, SGIDM8State, AK4381_REGS),
        VMSTATE_UINT8_ARRAY(ak4355, SGIDM8State, AK4355_REGS),
        VMSTATE_UINT32(ak4381_writes, SGIDM8State),
        VMSTATE_UINT32(ak4355_writes, SGIDM8State),
        VMSTATE_UINT32(codec_rejects, SGIDM8State),
        VMSTATE_UINT16_ARRAY(ac97, SGIDM8State, AC97_REGS),
        VMSTATE_UINT64(analog_frames, SGIDM8State),
        VMSTATE_UINT32(analog_last, SGIDM8State),
        VMSTATE_UINT64(spdif_frames, SGIDM8State),
        VMSTATE_UINT64(capture_frames, SGIDM8State),
        VMSTATE_UINT64(dropped_frames, SGIDM8State),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * A counter read brings the engines up to date first, so a reader sees the
 * frames moved by the present time.
 */
static void dm8_get_counter(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGIDM8State *s = SGI_DM8(obj);
    uint64_t value;

    if (DEVICE(s)->realized) {
        dm8_sync(s);
    }
    value = *(uint64_t *)opaque;
    visit_type_uint64(v, name, &value, errp);
}

static void dm8_get_last_sample(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    SGIDM8State *s = SGI_DM8(obj);
    uint32_t value;

    if (DEVICE(s)->realized) {
        dm8_sync(s);
    }
    value = s->analog_last;
    visit_type_uint32(v, name, &value, errp);
}

static void dm8_instance_init(Object *obj)
{
    SGIDM8State *s = SGI_DM8(obj);

    /*
     * Read-only views of what the converters were last told and of how many
     * frames reached the host, for tests and for comparing against a guest
     * driver's intent.
     */
    for (unsigned i = 0; i < AK4381_REGS; i++) {
        g_autofree char *name = g_strdup_printf("ak4381-reg%u", i);

        object_property_add_uint8_ptr(obj, name, &s->ak4381[i],
                                      OBJ_PROP_FLAG_READ);
    }
    for (unsigned i = 0; i < AK4355_REGS; i++) {
        g_autofree char *name = g_strdup_printf("ak4355-reg%u", i);

        object_property_add_uint8_ptr(obj, name, &s->ak4355[i],
                                      OBJ_PROP_FLAG_READ);
    }
    object_property_add_uint32_ptr(obj, "ak4381-writes", &s->ak4381_writes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ak4355-writes", &s->ak4355_writes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "codec-rejects", &s->codec_rejects,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "analog-frames", "uint64", dm8_get_counter, NULL,
                        NULL, &s->analog_frames);
    object_property_add(obj, "analog-last-sample", "uint32",
                        dm8_get_last_sample, NULL, NULL, NULL);
    object_property_add(obj, "spdif-frames", "uint64", dm8_get_counter, NULL,
                        NULL, &s->spdif_frames);
    object_property_add(obj, "capture-frames", "uint64", dm8_get_counter, NULL,
                        NULL, &s->capture_frames);
    object_property_add(obj, "dropped-frames", "uint64", dm8_get_counter, NULL,
                        NULL, &s->dropped_frames);
}

static const Property dm8_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SGIDM8State, audio_be),
    DEFINE_PROP_STRING("variant", SGIDM8State, variant_name),
};

static void dm8_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI DMediaPro DM8 audio (ICE1724 / Envy24HT)";
    dc->vmsd = &vmstate_sgi_dm8;
    device_class_set_props(dc, dm8_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    pc->realize = dm8_realize;
    pc->exit = dm8_exit;
    pc->config_read = dm8_config_read;
    pc->config_write = dm8_config_write;
    pc->vendor_id = DM8_VENDOR_ID;
    pc->device_id = DM8_DEVICE_ID;
    pc->revision = DM8_REVISION;
    pc->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    resettable_class_set_parent_phases(rc, NULL, dm8_reset_hold,
                                       dm8_reset_exit, &dm8_parent_phases);
}

static const TypeInfo dm8_info = {
    .name = TYPE_SGI_DM8,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SGIDM8State),
    .instance_init = dm8_instance_init,
    .class_init = dm8_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void dm8_register_types(void)
{
    type_register_static(&dm8_info);
}

type_init(dm8_register_types)
