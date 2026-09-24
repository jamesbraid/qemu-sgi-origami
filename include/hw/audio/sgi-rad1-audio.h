/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI MediaIO RAD1 digital/analog/genlock audio ASIC.
 *
 * The MediaIO daughtercard's audio device: one PCI BAR carrying the
 * register file IRIX's rad_dd.o programs during attach. See
 * docs/datasheets/mediaio-rad1-audio.md.
 */

#ifndef HW_AUDIO_SGI_RAD1_AUDIO_H
#define HW_AUDIO_SGI_RAD1_AUDIO_H

#include "hw/pci/pci_device.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define TYPE_SGI_RAD1_AUDIO "sgi-rad1-audio"
OBJECT_DECLARE_SIMPLE_TYPE(SGIRad1AudioState, SGI_RAD1_AUDIO)

#define SGI_RAD1_AUDIO_VENDOR_ID 0x10a9
#define SGI_RAD1_AUDIO_DEVICE_ID 0x0005

/*
 * PCI revision 0x70. The driver reads the class/revision dword in
 * rad_create_subsys and keeps the revision byte's two nibbles as the
 * board mode (high nibble) and a sub-revision (low nibble). Mode 7 selects
 * the MediaIO paths (rad_vl_write_MUX_SEL_MIO and the mode-7 branches of
 * rad_start_subsys and rad_halt), 0xd the RADICAL card's, 0xc a third
 * board's; a revision of 0 leaves the driver on none of them. hinv on a
 * real BaseIO-G prints the two nibbles as "RAD revision 7.0".
 *
 * No source establishes the PCI class code. It stays the standard
 * multimedia-audio class hw/audio/ac97.c and hw/audio/es1370.c already
 * use for a PCI audio function -- the closest documented PCI convention,
 * not a captured value.
 */
#define SGI_RAD1_AUDIO_REVISION 0x70
#define SGI_RAD1_AUDIO_CLASS_ID PCI_CLASS_MULTIMEDIA_AUDIO

/*
 * BAR0: the register file, 0x2000 bytes. rad_attach maps exactly one PCI
 * window, pciio_piotrans_addr(conn, 0, space 8, offset 0, size 0x2000),
 * and the Bridge driver reads spaces 8..13 as BARs 0..5; rad_create_subsys
 * stores the result at the per-device struct's 0x228 slot every later
 * register access goes through. The Bridge refuses a direct translation
 * whose request runs past the BAR, so a smaller BAR0 leaves that slot
 * NULL and IRIX panics in rad_start_subsys's first register write.
 * rad_attach's other pciio_piotrans_addr asks for space 7, size 0x20:
 * that is PCI configuration space, not a second BAR. Most of the window
 * past the highest documented offset remains unestablished.
 */
#define SGI_RAD1_AUDIO_BAR0_SIZE 0x2000

/*
 * The documented BAR0 register offsets, named for the datasheet's own
 * register-map entries and rad_start_subsys's bring-up writes. None has
 * an independently confirmed reset value or read behavior: this model
 * stores whatever the guest last wrote and returns it on read, an
 * approximation flagged in hw/audio/sgi-rad1-audio.c rather than a
 * captured hardware contract. See docs/datasheets/mediaio-rad1-audio.md's
 * register map for the write sites and evidence behind each offset.
 */
/*
 * Sample-position bank: a read-only 32-bit register per channel, at BAR0
 * 0x04 through 0x2c step 4. rad_start_dma reads one of these as the media
 * stream counter's hardware term. Each channel type reads a pair, A and B,
 * named by a per-type table in the driver; the datasheet's streaming-engine
 * contract gives the field layout. The model backs each register with a
 * stored value that starts at zero and changes only while a channel's DMA
 * engine runs. This bank does not drive the "UST not incrementing" report,
 * which comes from the free-running counter at 0x30.
 *
 * The analog output, channel type 1, reads A = 0x2c and B = 0x18.
 * - A carries the frame count in bits [31:18] (the driver reads it >> 18,
 *   masked to 14 bits) and a latch of the 0x30 counter in bits [17:0].
 * - B is shared with the analog input, type 0. Type 0 reads its bits
 *   [31:18]; type 1 reads bits [17:4]. The driver's start routine polls
 *   those bits after the enable and needs them to change within about
 *   500 us.
 */
#define SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST 0x04
#define SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST  0x2c
#define SGI_RAD1_AUDIO_SAMPLE_POS_WORDS \
    (((SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST - \
       SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST) / 4) + 1)
#define SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT     18
#define SGI_RAD1_AUDIO_UST_LATCH_MASK       0x3ffffu
#define SGI_RAD1_AUDIO_POS_FIELD_MASK       0x3fffu
#define SGI_RAD1_AUDIO_REG_OUT_POS_A        0x2c
#define SGI_RAD1_AUDIO_REG_OUT_POS_B        0x18
#define SGI_RAD1_AUDIO_OUT_POS_B_SHIFT      4
/* Word index into sample_pos[] for a sample-position BAR0 offset. */
#define SGI_RAD1_AUDIO_POS_IDX(off) \
    (((off) - SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST) / 4)

/*
 * Free-running UST (unadjusted system time) counter at BAR0 0x30, read-only.
 * rad_set_ust_offset reads it twice a fixed delay apart and warns "RAD UST
 * not incrementing" if it advanced by fewer than three ticks between the two
 * reads. A real board's counter runs off a clock, so the model derives it
 * from the emulated monotonic virtual clock rather than storing a value: one
 * tick per UST_NS_PER_TICK nanoseconds of virtual time (about 1.25 MHz), fast
 * enough that the driver's threshold is met by a wide margin. Board mode 7
 * runs it from reset with no enable write.
 */
#define SGI_RAD1_AUDIO_REG_UST_COUNTER      0x30
#define SGI_RAD1_AUDIO_UST_NS_PER_TICK      800

#define SGI_RAD1_AUDIO_REG_PREDISPATCH_A    0x44 /* bring-up 0xa0000008 */
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_B    0x48 /* bring-up 0x10 */
#define SGI_RAD1_AUDIO_REG_SHIFT_LOAD_CLOCK 0x4c /* mode-gated bring-up write */
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_C    0x50 /* mode-gated bring-up write */
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_D    0x54 /* mode-gated bring-up write */
/* Bring-up 0x20000000; bit 0 is the analog output's (type 1) DMA enable. */
#define SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN    0x58
#define SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL  0x5c /* status-DMA start/stop */
#define SGI_RAD1_AUDIO_REG_HALT_TEARDOWN    0x70 /* bring-up 0x1500 */
#define SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_A  0x74 /* bring-up 0x8000010 */
#define SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_B  0x78 /* bring-up 0xfac688 */
#define SGI_RAD1_AUDIO_REG_MUX_CLOCK_SELECT 0x7c
#define SGI_RAD1_AUDIO_REG_SHADOW_MASKED    0x80 /* converges to 0xffe20000 */
#define SGI_RAD1_AUDIO_REG_MODE_GATED_A     0x84 /* mode-gated bring-up write */
#define SGI_RAD1_AUDIO_REG_LATCH_PULSE      0x88 /* mode-gated bring-up write */
#define SGI_RAD1_AUDIO_REG_SHIFT_LOAD_DATA  0x8c /* mode-gated bring-up write */
#define SGI_RAD1_AUDIO_REG_MODE_GATED_B     0x90 /* all three modes write 2 */
#define SGI_RAD1_AUDIO_REG_BRINGUP_FINAL    0xa0 /* bring-up 0x2000001 */
#define SGI_RAD1_AUDIO_REG_CONVERGENCE_A    0xb8
#define SGI_RAD1_AUDIO_REG_CONVERGENCE_B    0xbc

/*
 * The DMA register bank, BAR0 0x400..0x528: the status-DMA descriptor-ring
 * base/limit and the main-DMA per-channel ping-pong address/control
 * registers. Unlike the sparse bring-up offsets above, this is a dense
 * contiguous bank the driver programs by computed offset, so a plain
 * store-backed array over the whole span models it -- a write anywhere in
 * range is a known register access, not the unimplemented-log default.
 * rad_start_status_dma programs the status-ring base low/high into
 * STATUS_BASE_LO/HI and a wrap/limit into STATUS_LIMIT, with a second
 * identical set at STATUS_BASE2_*; rad_start_dma writes the main-DMA
 * per-channel address/length registers elsewhere in the bank.
 */
#define SGI_RAD1_AUDIO_REG_DMA_BANK_FIRST   0x400
#define SGI_RAD1_AUDIO_REG_DMA_BANK_LAST    0x528
#define SGI_RAD1_AUDIO_DMA_BANK_WORDS \
    (((SGI_RAD1_AUDIO_REG_DMA_BANK_LAST - SGI_RAD1_AUDIO_REG_DMA_BANK_FIRST) \
      / 4) + 1)

#define SGI_RAD1_AUDIO_REG_STATUS_BASE2_LO  0x4b4
#define SGI_RAD1_AUDIO_REG_STATUS_BASE2_HI  0x4b8
#define SGI_RAD1_AUDIO_REG_STATUS_LIMIT2    0x4bc
#define SGI_RAD1_AUDIO_REG_STATUS_BASE_LO   0x500
#define SGI_RAD1_AUDIO_REG_STATUS_LIMIT     0x504
#define SGI_RAD1_AUDIO_REG_STATUS_BASE_HI   0x528

/*
 * Main audio-data DMA for the analog output, channel type 1. rad_start_dma
 * writes the ring's PCI address and a control word into two identical
 * register sets, then sets bit 0 of the enable register at 0x58. The
 * control word is (tag << 3) | (-count << 7), count in 128-byte units; the
 * tag is the channel type. See the per-type table in the datasheet's
 * deferred bring-up section and its main-DMA streaming contract.
 */
#define SGI_RAD1_AUDIO_REG_OUT_A_ADDR_LO    0x4f8
#define SGI_RAD1_AUDIO_REG_OUT_A_ADDR_HI    0x524
#define SGI_RAD1_AUDIO_REG_OUT_A_CTRL       0x4fc
#define SGI_RAD1_AUDIO_REG_OUT_B_ADDR_LO    0x40c
#define SGI_RAD1_AUDIO_REG_OUT_B_ADDR_HI    0x410
#define SGI_RAD1_AUDIO_REG_OUT_B_CTRL       0x414
#define SGI_RAD1_AUDIO_OUT_ENABLE_BIT       0x1u
#define SGI_RAD1_AUDIO_OUT_TAG              1
#define SGI_RAD1_AUDIO_CTRL_TAG_SHIFT       3
#define SGI_RAD1_AUDIO_CTRL_TAG_MASK        0xfu
#define SGI_RAD1_AUDIO_CTRL_LEN_SHIFT       7
#define SGI_RAD1_AUDIO_CTRL_LEN_MASK        0x1ffffffu
#define SGI_RAD1_AUDIO_DMA_UNIT_BYTES       128
/*
 * Largest ring the model walks. The driver's rings are 4 KiB or 16 KiB;
 * the cap only stops a garbage control word from naming gigabytes.
 */
#define SGI_RAD1_AUDIO_DMA_MAX_BYTES        (1u << 20)

/*
 * Status-DMA control values written to 0x5c, after masking off the interval
 * field with CTRL_MASK (below). rad_start_status_dma starts the continuous ring
 * engine with STATUS_START (the ring bit 0x2 set); rad_one_shot_status starts a
 * single transfer with ONESHOT_START (ring bit clear); rad_stop_status_dma
 * stops with STATUS_STOP. The individual bit meanings beyond start/stop and
 * ring/one-shot are not decoded (see docs/datasheets/mediaio-rad1-audio.md).
 */
#define SGI_RAD1_AUDIO_STATUS_START         0x40003u
#define SGI_RAD1_AUDIO_STATUS_ONESHOT_START 0x40001u
#define SGI_RAD1_AUDIO_STATUS_STOP          0x1fffcu

/*
 * Descriptor-ring slot geometry, from rad_start_status_dma: slots are
 * 0x80 bytes apart, at most 0x80 of them; the engine marks each processed
 * slot done by setting bit 31 of its +0x7c word and the completion bits in
 * its +0x78 word. rad_start_status_dma encodes the slot count in the limit
 * register as LIMIT = 0x78 - count * 0x80.
 */
#define SGI_RAD1_AUDIO_SLOT_STRIDE          0x80
#define SGI_RAD1_AUDIO_SLOT_MAX             0x80
#define SGI_RAD1_AUDIO_SLOT_DONE_OFFSET     0x7c
#define SGI_RAD1_AUDIO_SLOT_FLAGS_OFFSET    0x78
#define SGI_RAD1_AUDIO_SLOT_DONE_BIT        0x80000000u
#define SGI_RAD1_AUDIO_LIMIT_BASE           0x78

/*
 * What a completion writes into its slot, per the datasheet's
 * streaming-engine contract and its status-slot flag bits:
 *
 * - bytes +0x44..+0x74 mirror registers 0x04..0x34 (slot byte = register
 *   offset + 0x40), the copy rad_update_dev works from;
 * - the even words of +0x00..+0x38 hold each active channel's current DMA
 *   address, low 32 bits; type 1's is +0x38;
 * - +0x78 carries the ADAT-present bit (0x40) and the two bits the driver
 *   counts into a period it classifies against video field rates
 *   (0x80/0x100);
 * - +0x7c is the completion word: bit 31 done, bit 19 "no valid AES3
 *   input", bits [26:24] an AES3 rate code.
 *
 * The model presents no ADAT input, no AES3 input and no video reference:
 * +0x78 is written 0, which leaves 0x40, 0x80 and 0x100 clear, and +0x7c is
 * 0x80080000. Setting 0x80/0x100 on every completion would present a
 * synthetic 60 Hz genlock. The other +0x78 bits are undecoded; zero is the
 * model's choice, not a captured value.
 */
#define SGI_RAD1_AUDIO_SLOT_MIRROR_OFFSET   0x40
#define SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST    0x04
#define SGI_RAD1_AUDIO_SLOT_MIRROR_LAST     0x34
#define SGI_RAD1_AUDIO_SLOT_MIRROR_WORDS \
    (((SGI_RAD1_AUDIO_SLOT_MIRROR_LAST - SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST) \
      / 4) + 1)
#define SGI_RAD1_AUDIO_SLOT_OUT_PTR_OFFSET  0x38
#define SGI_RAD1_AUDIO_SLOT_AES_NO_INPUT    0x00080000u
#define SGI_RAD1_AUDIO_SLOT_DONE_WORD \
    (SGI_RAD1_AUDIO_SLOT_DONE_BIT | SGI_RAD1_AUDIO_SLOT_AES_NO_INPUT)
#define SGI_RAD1_AUDIO_SLOT_FLAGS_WORD      0x0u

/*
 * Register 0x34, which the driver reads only through slot +0x74, carries
 * per-input sync-present flags whose polarity is unknown. With no video
 * reference modeled it is held constant; zero is a placeholder until the
 * polarity is settled.
 */
#define SGI_RAD1_AUDIO_SYNC_STATUS          0x0u

/*
 * Continuous status-DMA engine. The one-shot start (0x40001) fills a single
 * slot and rad_one_shot_status busy-polls it. rad_start_status_dma instead
 * starts a ring engine (0x40003, the ring bit set) that keeps completing
 * slots around the ring on its own; rad_update_subsys walks the ring,
 * clearing each done bit as it consumes, and copies the last completed slot
 * for rad_update_dev, which takes each channel's DMA pointer and position
 * registers from it. The model runs a timer that fills the next ring slot
 * at each completion.
 *
 * CTRL_MASK strips the interval field ([16:2]) before the start words are
 * compared; the ring bit (0x2) distinguishes the continuous start from the
 * one-shot.
 */
#define SGI_RAD1_AUDIO_STATUS_CTRL_MASK     0x40003u /* interval masked out */

/*
 * Completion interval. rad_start_status_dma writes
 * ((subsystem[0x234] * 33 + 1) & 0x7fff) << 2 into bits [16:2], 8251 on IRIX
 * 6.5.30. The datasheet infers the field counts 33 MHz PCI clocks, about
 * 250 us; both the unit and that it is an interval at all are inferred, so
 * the period is an approximation. It matters: the kernel mixer refills the
 * output ring only about 0xcc words ahead of the pointer it last saw in a
 * slot, and at 250 us the pointer moves about 22 words per completion.
 *
 * Completions run on an absolute schedule, deadline k at
 * start + k * interval clocks, so a late callback does not push later
 * completions back. A callback that runs late by less than a ring of slots
 * leaves past-due deadlines behind it, so the timer fires again at once
 * and the missed completions arrive back to back, all carrying the same
 * pointer and 0x30 value; the driver's walk takes them like any others. A
 * callback more than a ring of slots behind restarts the schedule from now
 * instead of filling a whole lap.
 *
 * Two model policies the datasheet does not cover. A start word with a
 * zero interval names no period, so it stops the engine rather than
 * running it. An interval below INTERVAL_MIN, 10 us, is raised to it: a
 * host-protection limit, since an interval of 1 would ask for 33 million
 * completions a second. IRIX writes 8251, far above the floor.
 */
#define SGI_RAD1_AUDIO_STATUS_INTERVAL_SHIFT 2
#define SGI_RAD1_AUDIO_STATUS_INTERVAL_MASK  0x7fffu
#define SGI_RAD1_AUDIO_STATUS_INTERVAL_MIN   330u
#define SGI_RAD1_AUDIO_STATUS_CLOCK_HZ       33000000u


/*
 * The analog output's streaming engine. From the enable edge to its clear
 * it walks the ring [start, start + count * 128) a frame at a time and
 * wraps to the start, with no per-chunk re-arm, paced by a virtual-clock
 * timer at the sample rate. Each step adds the frames moved to both
 * position registers and pushes the samples to the audio backend.
 *
 * Ring format, from the datasheet's "Analog output sample format": a frame
 * is two 32-bit words, one channel each, holding a 24-bit signed sample in
 * bits [23:0]. The words cross the Bridge's byte-lane reversal like the
 * status descriptors, so the device sees them little-endian. The host
 * voice is 16-bit, taking bits [23:8].
 *
 * Approximations, each flagged in the datasheet:
 * - 44.1 kHz, the software rate of type 1's clock generator in the live
 *   IRIX run. Which 0xa0/0xa4 bits select the hardware rate is unknown.
 * - The first word of a frame is the left channel.
 * - Both position registers count frames, not words.
 *
 * OUT_TICK_FRAMES frames per timer step, about 181 us, keeps the first
 * change of 0x18 bits [17:4] inside the driver's 500 us poll window.
 */
#define SGI_RAD1_AUDIO_OUT_FREQ             44100
#define SGI_RAD1_AUDIO_OUT_CHANNELS         2
#define SGI_RAD1_AUDIO_OUT_WORD_BYTES       4
#define SGI_RAD1_AUDIO_OUT_FRAME_BYTES \
    (SGI_RAD1_AUDIO_OUT_CHANNELS * SGI_RAD1_AUDIO_OUT_WORD_BYTES)
#define SGI_RAD1_AUDIO_OUT_SAMPLE_SHIFT     8
#define SGI_RAD1_AUDIO_OUT_TICK_FRAMES      8

/*
 * A named field per documented offset rather than an array over the whole
 * 0x44..0xbc span: the span has gaps no source documents (for example
 * 0x60..0x6c), and those must still log as unimplemented instead of
 * silently taking a store the way a documented offset does. The dense
 * 0x400..0x528 DMA bank, by contrast, is a store-backed array.
 */
struct SGIRad1AudioState {
    PCIDevice parent_obj;

    MemoryRegion bar0;

    uint32_t predispatch_a;    /* 0x44 */
    uint32_t predispatch_b;    /* 0x48 */
    uint32_t shift_load_clock; /* 0x4c */
    uint32_t predispatch_c;    /* 0x50 */
    uint32_t predispatch_d;    /* 0x54 */
    uint32_t analog_out_en;    /* 0x58 */
    uint32_t status_dma_ctrl;  /* 0x5c */
    uint32_t halt_teardown;    /* 0x70 */
    uint32_t bringup_fixed_a;  /* 0x74 */
    uint32_t bringup_fixed_b;  /* 0x78 */
    uint32_t mux_clock_select; /* 0x7c */
    uint32_t shadow_masked;    /* 0x80 */
    uint32_t mode_gated_a;     /* 0x84 */
    uint32_t latch_pulse;      /* 0x88 */
    uint32_t shift_load_data;  /* 0x8c */
    uint32_t mode_gated_b;     /* 0x90 */
    uint32_t bringup_final;    /* 0xa0 */
    uint32_t convergence_a;    /* 0xb8 */
    uint32_t convergence_b;    /* 0xbc */

    uint32_t dma_bank[SGI_RAD1_AUDIO_DMA_BANK_WORDS]; /* 0x400..0x528 */

    /* Sample-position bank 0x04..0x2c, advanced by the output engine. */
    uint32_t sample_pos[SGI_RAD1_AUDIO_SAMPLE_POS_WORDS];

    /*
     * Analog-output engine. out_base and out_len are the ring latched from
     * set A at the enable edge; out_offset is the next frame's byte offset
     * in it, so out_base + out_offset is the DMA pointer the status slot
     * reports, which holds its last value after a stop. out_frames counts
     * frames moved since out_start_ns, the virtual time of the enable edge,
     * and paces out_timer.
     */
    QEMUTimer *out_timer;
    bool out_running;
    uint64_t out_base;
    uint32_t out_len;
    uint32_t out_offset;
    int64_t out_start_ns;
    uint64_t out_frames;

    /*
     * Continuous status-DMA engine. status_timer fires one completion per
     * interval while status_running; status_ring_cursor is the next slot to
     * complete (advancing sequentially with wrap so the driver's consumer,
     * which walks consecutive done slots, keeps finding the next one done),
     * and status_ring_slots is the ring size recovered from the limit
     * register at the continuous start. status_interval is the start word's
     * interval field; completion k is due status_start_ns plus k intervals,
     * status_done counting those already made.
     */
    QEMUTimer *status_timer;
    uint32_t status_ring_cursor;
    uint32_t status_ring_slots;
    bool status_running;
    uint32_t status_interval;
    int64_t status_start_ns;
    uint64_t status_done;

    /* Virtual-clock time captured at reset; the 0x30 UST counter's origin. */
    int64_t ust_base_ns;

    /*
     * Host audio backend and the output voice the engine pushes to. The
     * backend comes from the audiodev property (or the default). The voice
     * is opened once at realize and only toggled active while the engine
     * runs: closing and reopening it would make the wav backend truncate its
     * file on each stream.
     */
    AudioBackend *audio_be;
    SWVoiceOut *dac_voice;
};

#endif
