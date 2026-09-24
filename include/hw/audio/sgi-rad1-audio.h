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
 * stream counter's hardware term (value >> 18, masked to 14 bits). On real
 * hardware the upper bits advance while the audio DMA transfers, and the
 * driver reconstructs a monotonic sample count from them. The model backs
 * each register with a stored value that starts at zero and, while the main
 * DMA runs (see the streaming path in hw/audio/sgi-rad1-audio.c), advances by
 * (frames << SAMPLE_POS_SHIFT) so the driver's MSC tracks; a static bank
 * would leave the MSC fixed. This bank does not drive the "UST not
 * incrementing" report, which the status-DMA completion handshake does.
 */
#define SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST 0x04
#define SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST  0x2c
#define SGI_RAD1_AUDIO_SAMPLE_POS_WORDS \
    (((SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST - \
       SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST) / 4) + 1)
#define SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT     18
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
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_E    0x58 /* bring-up 0x20000000 */
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
#define SGI_RAD1_AUDIO_SLOT_FRAME_BITS      0x180u /* +0x78: 0x100 and 0x80 */
#define SGI_RAD1_AUDIO_LIMIT_BASE           0x78

/*
 * Continuous status-DMA engine. The one-shot start (0x40001) marks a single
 * slot done and rad_one_shot_status busy-polls it -- enough for the boot
 * liveness poll but nothing more. rad_start_status_dma instead starts a
 * ring engine (0x40003, the ring bit set) that keeps producing completions
 * around the ring on its own; rad_update_subsys walks the ring, and for each
 * completed slot advances the driver's frame counters (subsystem[0x2cc] on
 * +0x78 bit 0x80, subsystem[0x2d0] on bit 0x100), clearing the done bit as it
 * consumes. Once per servicing window it divides elapsed time by the frame
 * total to a per-frame period (subsystem[0x72e8]) and, when that is nonzero,
 * calls rad_update_ksync, which classifies the period against fixed thresholds
 * and reprograms the clock generator -- the software clock lock the audio
 * library waits on before it starts output (rad_start_dma). A single
 * completion never advances the rate, so the model runs a periodic timer that
 * completes the next ring slot each tick.
 *
 * The interval field encoded into the 0x40003 control word ([16:2], value
 * ((subsystem[0x234]*33+1)&0x7fff)<<2) is stripped by CTRL_MASK; the ring bit
 * (0x2) is what distinguishes the continuous start from the one-shot.
 */
#define SGI_RAD1_AUDIO_STATUS_CTRL_MASK     0x40003u /* interval masked out */

/*
 * Timer cadence. rad_update_subsys measures a per-frame period of
 * elapsed_ns / frame_total, where frame_total counts both +0x78 bits per
 * completion (so two per completion here). The model fires one completion
 * every STATUS_PERIOD_NS, giving a measured period of STATUS_PERIOD_NS/2 =
 * ~16.67 ms. That lands inside rad_update_ksync's 16.39-16.95 ms band (the
 * 0xfa24e2..0x1029fa0 thresholds read from the driver), the ~60 Hz genlock
 * field rate the driver classifies to a clock source and locks. The true
 * status-frame rate is not pinned by any source -- the 0x5c interval field's
 * unit is undecoded -- so this cadence is a stated approximation chosen to
 * land in that classification band, not a captured hardware value.
 */
#define SGI_RAD1_AUDIO_STATUS_PERIOD_NS     33333334


/*
 * Main audio-data DMA, output-only on the master channel. Separate from the
 * status ring: rad_start_dma programs a per-channel address/control pair
 * directly, then kicks central control 0x80. The offsets below are the master
 * channel's (rad_dd.o's channel type 4, the one rad_start_dma treats as the
 * DMA master), loaded by rad_create_device from a per-device-type table read
 * out of the shipped object's .data and store-back sequence:
 *
 *   set A: address-low 0x4e8, address-high 0x51c, control 0x4ec
 *   set B: address-low 0x43c, address-high 0x440, control 0x444
 *
 * Both sets are programmed with the same address and control at start (a
 * ping-pong double buffer). The address is a 64-bit PCI address split low/
 * high; the control word carries the transfer length as a negated count of
 * 128-byte units in bits [31:7] (a small tag rides bits [6:3]). All six live
 * in the store-backed 0x400..0x528 DMA bank.
 *
 * The channel's DMA enable is a bit in the register at 0x50 (the same BAR0
 * offset the bring-up writes as PREDISPATCH_C): rad_start_dma sets bit 0
 * before the kick, rad_stop_dma clears it. Central control 0x80 (the same
 * register the bring-up converges and rad_check_adat_rx toggles) is the kick:
 * rad_start_dma writes it with the 0xc0 DMA/ADAT-enable pair cleared. The
 * model treats a 0x80 write that clears 0xc0, made while the enable bit is
 * set and a non-zero length is programmed, as the point the transfer starts.
 * The 0xc0 bits and the control-word tag are otherwise undecoded.
 */
#define SGI_RAD1_AUDIO_REG_MAIN_A_ADDR_LO   0x4e8
#define SGI_RAD1_AUDIO_REG_MAIN_A_ADDR_HI   0x51c
#define SGI_RAD1_AUDIO_REG_MAIN_A_CTRL      0x4ec
#define SGI_RAD1_AUDIO_REG_MAIN_B_ADDR_LO   0x43c
#define SGI_RAD1_AUDIO_REG_MAIN_B_ADDR_HI   0x440
#define SGI_RAD1_AUDIO_REG_MAIN_B_CTRL      0x444
#define SGI_RAD1_AUDIO_MAIN_ENABLE_BIT      0x1u   /* master channel */
#define SGI_RAD1_AUDIO_DMA_ENABLE_MASK      0xc0u  /* 0x80 write clears these */
#define SGI_RAD1_AUDIO_CTRL_LEN_SHIFT       7
#define SGI_RAD1_AUDIO_CTRL_LEN_MASK        0x1ffffffu
#define SGI_RAD1_AUDIO_DMA_UNIT_BYTES       128
/* Cap a single transfer so a garbage length cannot loop unboundedly. */
#define SGI_RAD1_AUDIO_DMA_MAX_BYTES        (1u << 20)
/* The master channel's two sample-position registers (index A, index B). */
#define SGI_RAD1_AUDIO_REG_MAIN_POS_A       0x04
#define SGI_RAD1_AUDIO_REG_MAIN_POS_B       0x08

/*
 * Output voice format. The driver leaves the sample format and rate to the
 * clock-generator and interface-parameter registers, whose bits this model
 * does not decode, so the streamed voice is a fixed big-endian 16-bit stereo
 * PCM voice -- a stated approximation, not a captured contract. The frame
 * count a transfer length implies (and the sample-position advance) follows
 * from this frame size; changing the assumed format changes that count.
 */
#define SGI_RAD1_AUDIO_OUT_FREQ             48000
#define SGI_RAD1_AUDIO_OUT_CHANNELS         2
#define SGI_RAD1_AUDIO_OUT_FRAME_BYTES      4

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
    uint32_t predispatch_e;    /* 0x58 */
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

    /*
     * Sample-position bank 0x04..0x2c: stored per register, advanced by the
     * main DMA. streaming tracks whether the master channel's output voice is
     * active, so the enable-clear stop deactivates it once.
     */
    uint32_t sample_pos[SGI_RAD1_AUDIO_SAMPLE_POS_WORDS];
    bool streaming;

    /*
     * Continuous status-DMA engine. status_timer fires one completion per
     * STATUS_PERIOD_NS while status_running; status_ring_cursor is the next
     * slot to complete (advancing sequentially with wrap so the driver's
     * consumer, which walks consecutive done slots, keeps finding the next
     * one done), and status_ring_slots is the ring size recovered from the
     * limit register at the continuous start.
     */
    QEMUTimer *status_timer;
    uint32_t status_ring_cursor;
    uint32_t status_ring_slots;
    bool status_running;

    /* Virtual-clock time captured at reset; the 0x30 UST counter's origin. */
    int64_t ust_base_ns;

    /*
     * Host audio backend and the output voice the main DMA pushes to. The
     * backend comes from the audiodev property (or the default); the voice is
     * opened on the first stream and is runtime-only, reopened after migration
     * rather than migrated.
     */
    AudioBackend *audio_be;
    SWVoiceOut *dac_voice;
};

#endif
