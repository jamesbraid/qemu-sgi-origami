/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI MediaIO RAD1 audio register and device definitions. */
/*
 * The MediaIO daughtercard's audio device: one PCI BAR carrying the register
 * file IRIX's rad_dd.o programs during attach.
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
 * rad_create_subsys and keeps the revision byte's two nibbles as the board
 * mode (high nibble) and a sub-revision (low nibble). Mode 7 selects the
 * MediaIO paths (rad_vl_write_MUX_SEL_MIO and the mode-7 branches of
 * rad_start_subsys and rad_halt), 0xd the RADICAL card's, 0xc a third
 * board's; a revision of 0 leaves the driver on none of them. hinv on a real
 * BaseIO-G prints the two nibbles as "RAD revision 7.0".
 *
 * No source establishes the PCI class code. It stays the standard
 * multimedia-audio class hw/audio/ac97.c and hw/audio/es1370.c already use
 * for a PCI audio function, the closest documented PCI convention, not a
 * verified hardware value.
 */
#define SGI_RAD1_AUDIO_REVISION 0x70
#define SGI_RAD1_AUDIO_CLASS_ID PCI_CLASS_MULTIMEDIA_AUDIO

/*
 * BAR0: the register file, 0x2000 bytes. rad_attach maps exactly one PCI
 * window, pciio_piotrans_addr(conn, 0, space 8, offset 0, size 0x2000), and
 * the Bridge driver reads spaces 8..13 as BARs 0..5; rad_create_subsys stores
 * the result at the per-device struct's 0x228 slot every later register
 * access goes through. The Bridge refuses a direct translation whose request
 * runs past the BAR, so a smaller BAR0 leaves that slot NULL and IRIX panics
 * in rad_start_subsys's first register write. rad_attach's other
 * pciio_piotrans_addr asks for space 7, size 0x20: that is PCI configuration
 * space, not a second BAR. Most of the window past the highest documented
 * offset remains unestablished.
 */
#define SGI_RAD1_AUDIO_BAR0_SIZE 0x2000

/*
 * The BAR0 register offsets, named for rad_start_subsys's bring-up writes.
 * None has an independently confirmed reset value or read behavior: this
 * model stores whatever the guest last wrote and returns it on read, an
 * approximation flagged in hw/audio/sgi-rad1-audio.c rather than a verified
 * hardware behavior.
 */
/*
 * Sample-position bank: a read-only 32-bit register per channel, at BAR0 0x04
 * through 0x2c step 4. rad_start_dma reads one of these as the media stream
 * counter's hardware term. Each channel type reads a pair, A and B, named by
 * a per-type table in the driver. The model backs each register with a stored
 * value that starts at zero and changes only while a channel's DMA engine
 * runs. This bank does not drive the "UST not incrementing" report, which
 * comes from the free-running counter at 0x30.
 *
 * The analog input, channel type 0, reads A = 0x14 and B = 0x18; the analog
 * output, type 1, reads A = 0x2c and B = 0x18.
 * - A carries the frame count in bits [31:18] (the driver reads it >> 18,
 *   masked to 14 bits) and a latch of the 0x30 counter in bits [17:0].
 * - B is shared by the two. Type 0 reads its bits [31:18]; type 1 reads bits
 *   [17:4]. The driver's start routine polls those bits after the enable and
 *   needs them to change within about 500 us.
 *
 * The register format puts a 0x30 latch in bits [17:0] of every register
 * whose field is at shift 18, which for B would overlap type 1's field. The
 * model latches only into A and leaves B's low bits to type 1.
 */
#define SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST 0x04
#define SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST  0x2c
#define SGI_RAD1_AUDIO_SAMPLE_POS_WORDS \
    (((SGI_RAD1_AUDIO_REG_SAMPLE_POS_LAST - \
       SGI_RAD1_AUDIO_REG_SAMPLE_POS_FIRST) / 4) + 1)
#define SGI_RAD1_AUDIO_SAMPLE_POS_SHIFT     18
#define SGI_RAD1_AUDIO_UST_LATCH_MASK       0x3ffffu
#define SGI_RAD1_AUDIO_POS_FIELD_MASK       0x3fffu
#define SGI_RAD1_AUDIO_REG_IN_POS_A         0x14
#define SGI_RAD1_AUDIO_REG_OUT_POS_A        0x2c
#define SGI_RAD1_AUDIO_REG_ANALOG_POS_B     0x18
#define SGI_RAD1_AUDIO_IN_POS_B_SHIFT       18
#define SGI_RAD1_AUDIO_OUT_POS_B_SHIFT      4

/*
 * The digital channels' pairs, from the same per-type table. Every one reads
 * A and B at shift 18, so each count is bits [31:18]; the lower bits of each
 * B belong to something else and are left alone:
 * - AES3 input, type 2: A = 0x0c, B = 0x10 (bits [17:4] are the AES3-input
 *   subcode channel's, type 6);
 * - AES3 output, type 3: A = 0x24, B = 0x28 (bit 0 is the subcode buffer
 *   half, held constant, and bits [17:4] a subcode block count);
 * - ADAT input, type 4: A = 0x04, B = 0x08 (bits [17:4] are type 7's);
 * - ADAT output, type 5: A = 0x1c, B = 0x20 (as 0x28 for AES3 output).
 */
#define SGI_RAD1_AUDIO_REG_AES_IN_POS_A     0x0c
#define SGI_RAD1_AUDIO_REG_AES_IN_POS_B     0x10
#define SGI_RAD1_AUDIO_REG_AES_OUT_POS_A    0x24
#define SGI_RAD1_AUDIO_REG_AES_OUT_POS_B    0x28
#define SGI_RAD1_AUDIO_REG_ADAT_IN_POS_A    0x04
#define SGI_RAD1_AUDIO_REG_ADAT_IN_POS_B    0x08
#define SGI_RAD1_AUDIO_REG_ADAT_OUT_POS_A   0x1c
#define SGI_RAD1_AUDIO_REG_ADAT_OUT_POS_B   0x20
#define SGI_RAD1_AUDIO_DIGITAL_POS_B_SHIFT  18
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

/*
 * The digital channels' enables share these bring-up registers: bit 0 of 0x44
 * starts the ADAT input (type 4), of 0x48 the AES3 input (type 2), of 0x50
 * the ADAT output (type 5) and of 0x54 the AES3 output (type 3). Bit 2 of
 * 0x44 and 0x48 starts the input subcode DMA channels, types 7 and 6,
 * independently of bit 0; bits 2 and 3 of 0x54 are AES3 output subcode
 * controls, accepted and shadowed.
 */
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_A    0x44 /* bring-up 0xa0000008 */
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_B    0x48 /* bring-up 0x10 */
/*
 * Mode-gated bring-up write. Bit 0 is the analog input's (type 0) DMA enable;
 * bit 27 is the clock the MIO mux bit-bang pulses, and the driver keeps bit 0
 * in its shadow while it does, so only bit 0 starts or stops the input.
 */
#define SGI_RAD1_AUDIO_REG_SHIFT_LOAD_CLOCK 0x4c
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_C    0x50 /* mode-gated init write */
#define SGI_RAD1_AUDIO_REG_PREDISPATCH_D    0x54 /* mode-gated init write */
/* Bring-up 0x20000000; bit 0 is the analog output's (type 1) DMA enable. */
#define SGI_RAD1_AUDIO_REG_ANALOG_OUT_EN    0x58
#define SGI_RAD1_AUDIO_REG_STATUS_DMA_CTRL  0x5c /* status-DMA start/stop */
#define SGI_RAD1_AUDIO_REG_HALT_TEARDOWN    0x70 /* bring-up 0x1500 */
#define SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_A  0x74 /* bring-up 0x8000010 */
#define SGI_RAD1_AUDIO_REG_BRINGUP_FIXED_B  0x78 /* bring-up 0xfac688 */
#define SGI_RAD1_AUDIO_REG_MUX_CLOCK_SELECT 0x7c
#define SGI_RAD1_AUDIO_REG_SHADOW_MASKED    0x80 /* converges to 0xffe20000 */
#define SGI_RAD1_AUDIO_REG_MODE_GATED_A     0x84 /* mode-gated init write */
#define SGI_RAD1_AUDIO_REG_LATCH_PULSE      0x88 /* mode-gated init write */
#define SGI_RAD1_AUDIO_REG_SHIFT_LOAD_DATA  0x8c /* mode-gated init write */
#define SGI_RAD1_AUDIO_REG_MODE_GATED_B     0x90 /* all three modes write 2 */
#define SGI_RAD1_AUDIO_REG_BRINGUP_FINAL    0xa0 /* bring-up 0x2000001 */
#define SGI_RAD1_AUDIO_REG_CONVERGENCE_A    0xb8
#define SGI_RAD1_AUDIO_REG_CONVERGENCE_B    0xbc

/*
 * The DMA register bank, BAR0 0x400..0x528: the status-DMA descriptor-ring
 * base/limit and the main-DMA per-channel ping-pong address/control
 * registers. Unlike the sparse bring-up offsets above, this is a dense
 * contiguous bank the driver programs by computed offset, so a plain
 * store-backed array over the whole span models it. A write anywhere in
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
 * Main audio-data DMA for the analog channels. rad_start_dma writes the
 * ring's PCI address and a control word into two identical register sets,
 * then sets bit 0 of the channel's enable register: 0x4c for the input,
 * channel type 0, and 0x58 for the output, type 1. The control word is (tag
 * << 3) | (-count << 7), count in 128-byte units; the tag is the channel
 * type. See the per-type table in the deferred bring-up sequence and its
 * main-DMA streaming contract.
 */
#define SGI_RAD1_AUDIO_REG_IN_A_ADDR_LO     0x4d0
#define SGI_RAD1_AUDIO_REG_IN_A_ADDR_HI     0x510
#define SGI_RAD1_AUDIO_REG_IN_A_CTRL        0x4d4
#define SGI_RAD1_AUDIO_REG_IN_B_ADDR_LO     0x400
#define SGI_RAD1_AUDIO_REG_IN_B_ADDR_HI     0x404
#define SGI_RAD1_AUDIO_REG_IN_B_CTRL        0x408
#define SGI_RAD1_AUDIO_REG_AES_IN_A_ADDR_LO     0x4c8
#define SGI_RAD1_AUDIO_REG_AES_IN_A_ADDR_HI     0x50c
#define SGI_RAD1_AUDIO_REG_AES_IN_A_CTRL        0x4cc
#define SGI_RAD1_AUDIO_REG_AES_OUT_A_ADDR_LO    0x4f0
#define SGI_RAD1_AUDIO_REG_AES_OUT_A_ADDR_HI    0x520
#define SGI_RAD1_AUDIO_REG_AES_OUT_A_CTRL       0x4f4
#define SGI_RAD1_AUDIO_REG_ADAT_IN_A_ADDR_LO    0x4c0
#define SGI_RAD1_AUDIO_REG_ADAT_IN_A_ADDR_HI    0x508
#define SGI_RAD1_AUDIO_REG_ADAT_IN_A_CTRL       0x4c4
#define SGI_RAD1_AUDIO_REG_ADAT_OUT_A_ADDR_LO   0x4e8
#define SGI_RAD1_AUDIO_REG_ADAT_OUT_A_ADDR_HI   0x51c
#define SGI_RAD1_AUDIO_REG_ADAT_OUT_A_CTRL      0x4ec

/*
 * The input subcode DMA channels, types 6 and 7, from the per-type table:
 * AES3-input subcode on bit 2 of 0x48, set A 0x4e0/0x518/0x4e4, and
 * ADAT-input subcode on bit 2 of 0x44, set A 0x4d8/0x514/0x4dc; each shares
 * its enable register with its parent input's bit 0. Their DMA pointers sit
 * at slot +0x20 and +0x18.
 *
 * Each reads a single position register as both A and B: 0x10 for type 6,
 * 0x08 for type 7, A at shift 4 and B at shift 18. That B field, bits
 * [31:18], is also the parent input's B count, so the model credits a subcode
 * channel's frames to its A field, bits [17:4], only and leaves [31:18] to
 * the parent. A subcode channel started alone therefore sees its B stay
 * still. The documented latch in [17:0] applies to fields at shift 18, so no
 * latch is written here.
 *
 * What a subcode record holds is known only in part, and nothing on the host
 * supplies AES3 channel status or ADAT user bits, so both channels write
 * zeros. The driver's own device fields give the frame widths, read live: 64
 * words (256 bytes) for type 6 and 32 words (128 bytes) for type 7. How many
 * audio frames one record covers is unknown, so the rate is a flagged model
 * policy: one record per parent frame, AES3-input subcode at the AES3 input's
 * rate and ADAT-input subcode at 48 kHz. A capture of a running subcode
 * channel's position against its parent's would settle it.
 */
#define SGI_RAD1_AUDIO_SUBCODE_ENABLE_BIT       0x4u
#define SGI_RAD1_AUDIO_REG_AES_SUB_A_ADDR_LO    0x4e0
#define SGI_RAD1_AUDIO_REG_AES_SUB_A_ADDR_HI    0x518
#define SGI_RAD1_AUDIO_REG_AES_SUB_A_CTRL       0x4e4
#define SGI_RAD1_AUDIO_REG_ADAT_SUB_A_ADDR_LO   0x4d8
#define SGI_RAD1_AUDIO_REG_ADAT_SUB_A_ADDR_HI   0x514
#define SGI_RAD1_AUDIO_REG_ADAT_SUB_A_CTRL      0x4dc
#define SGI_RAD1_AUDIO_SUB_POS_A_SHIFT          4
#define SGI_RAD1_AUDIO_AES_SUB_WORDS            64
#define SGI_RAD1_AUDIO_ADAT_SUB_WORDS           32
#define SGI_RAD1_AUDIO_REG_OUT_A_ADDR_LO    0x4f8
#define SGI_RAD1_AUDIO_REG_OUT_A_ADDR_HI    0x524
#define SGI_RAD1_AUDIO_REG_OUT_A_CTRL       0x4fc
#define SGI_RAD1_AUDIO_REG_OUT_B_ADDR_LO    0x40c
#define SGI_RAD1_AUDIO_REG_OUT_B_ADDR_HI    0x410
#define SGI_RAD1_AUDIO_REG_OUT_B_CTRL       0x414
#define SGI_RAD1_AUDIO_ENABLE_BIT           0x1u
#define SGI_RAD1_AUDIO_CTRL_TAG_SHIFT       3
#define SGI_RAD1_AUDIO_CTRL_TAG_MASK        0xfu
#define SGI_RAD1_AUDIO_CTRL_LEN_SHIFT       7
#define SGI_RAD1_AUDIO_CTRL_LEN_MASK        0x1ffffffu
#define SGI_RAD1_AUDIO_DMA_UNIT_BYTES       128
/*
 * Largest ring the model walks. The driver's rings are 4 KiB or 16 KiB; the
 * cap only stops a garbage control word from naming gigabytes.
 */
#define SGI_RAD1_AUDIO_DMA_MAX_BYTES        (1u << 20)

/*
 * Status-DMA control values written to 0x5c, after masking off the interval
 * field with CTRL_MASK (below). rad_start_status_dma starts the continuous
 * ring engine with STATUS_START (the ring bit 0x2 set); rad_one_shot_status
 * starts a single transfer with ONESHOT_START (ring bit clear);
 * rad_stop_status_dma stops with STATUS_STOP. The individual bit meanings
 * beyond start/stop and ring/one-shot are not decoded.
 */
#define SGI_RAD1_AUDIO_STATUS_START         0x40003u
#define SGI_RAD1_AUDIO_STATUS_ONESHOT_START 0x40001u
#define SGI_RAD1_AUDIO_STATUS_STOP          0x1fffcu

/*
 * Descriptor-ring slot geometry, from rad_start_status_dma: slots are 0x80
 * bytes apart, at most 0x80 of them; the engine marks each processed slot
 * done by setting bit 31 of its +0x7c word and the completion bits in its
 * +0x78 word. rad_start_status_dma encodes the slot count in the limit
 * register as LIMIT = 0x78 - count * 0x80.
 */
#define SGI_RAD1_AUDIO_SLOT_STRIDE          0x80
#define SGI_RAD1_AUDIO_SLOT_MAX             0x80
#define SGI_RAD1_AUDIO_SLOT_DONE_OFFSET     0x7c
#define SGI_RAD1_AUDIO_SLOT_FLAGS_OFFSET    0x78
#define SGI_RAD1_AUDIO_SLOT_DONE_BIT        0x80000000u
#define SGI_RAD1_AUDIO_LIMIT_BASE           0x78

/*
 * What a completion writes into its slot, per the streaming-engine behavior
 * and its status-slot flag bits:
 *
 * - bytes +0x44..+0x74 mirror registers 0x04..0x34 (slot byte = register
 *   offset + 0x40), the copy rad_update_dev works from;
 * - the even words of +0x00..+0x38 hold each active channel's current DMA
 *   address, low 32 bits: +0x00 type 4, +0x08 type 2, +0x10 type 0, +0x18
 *   type 7, +0x20 type 6, +0x28 type 5, +0x30 type 3, +0x38 type 1;
 * - +0x78 carries the ADAT-present bit (0x40) and the two bits the driver
 *   counts into a period it classifies against video field rates
 *   (0x80/0x100);
 * - +0x7c is the completion word: bit 31 done, bit 19 "no valid AES3 input",
 *   bits [26:24] an AES3 rate code.
 *
 * By default the model presents no ADAT input, no AES3 input and no video
 * reference: +0x78 is written 0, which leaves 0x40, 0x80 and 0x100 clear, and
 * +0x7c is 0x80080000. Setting 0x80/0x100 on every completion would present a
 * synthetic 60 Hz genlock. The other +0x78 bits are undecoded; zero is the
 * model's choice, not a verified hardware value.
 *
 * What is plugged into the digital inputs is the host's to say, through two
 * properties that describe the cable, not guest-visible behavior of the
 * chip's own:
 * - "aes-input-rate", 0 (nothing connected), 32000, 44100 or 48000: a locked
 *   AES3 source at that rate. Every completion word then carries the
 *   documented lock value, done plus the rate code with bit 19 clear:
 *   0x83000000, 0x82000000 or 0x81000000, held steady so the driver's
 *   500-pass debounce settles. Codes 3, 2 and 1 are the model's choice: the
 *   driver's rate table gives codes 7, 5 and 4 the same rates, and what bit
 *   26, which tells the two sets apart, means in hardware is unknown.
 * - "adat-input", a present ADAT source: every completion sets +0x78 bit
 *   0x40, with no gaps, as the driver's ADAT state machine needs. The
 *   driver's 0x80 writes for that state machine are accepted and shadowed
 *   like any other.
 *
 * Neither property is migrated: like any device configuration, both must be
 * the same on the source and destination of a migration, or the receivers
 * change under the guest as it resumes.
 */
#define SGI_RAD1_AUDIO_SLOT_MIRROR_OFFSET   0x40
#define SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST    0x04
#define SGI_RAD1_AUDIO_SLOT_MIRROR_LAST     0x34
#define SGI_RAD1_AUDIO_SLOT_MIRROR_WORDS \
    (((SGI_RAD1_AUDIO_SLOT_MIRROR_LAST - SGI_RAD1_AUDIO_SLOT_MIRROR_FIRST) \
      / 4) + 1)
#define SGI_RAD1_AUDIO_SLOT_ADAT_IN_PTR_OFFSET  0x00
#define SGI_RAD1_AUDIO_SLOT_AES_IN_PTR_OFFSET   0x08
#define SGI_RAD1_AUDIO_SLOT_IN_PTR_OFFSET   0x10
#define SGI_RAD1_AUDIO_SLOT_ADAT_OUT_PTR_OFFSET 0x28
#define SGI_RAD1_AUDIO_SLOT_ADAT_SUB_PTR_OFFSET 0x18
#define SGI_RAD1_AUDIO_SLOT_AES_SUB_PTR_OFFSET  0x20
#define SGI_RAD1_AUDIO_SLOT_AES_OUT_PTR_OFFSET  0x30
#define SGI_RAD1_AUDIO_SLOT_OUT_PTR_OFFSET  0x38
#define SGI_RAD1_AUDIO_SLOT_ADAT_PRESENT    0x40u
#define SGI_RAD1_AUDIO_SLOT_AES_RATE_SHIFT  24
#define SGI_RAD1_AUDIO_AES_CODE_48000       1
#define SGI_RAD1_AUDIO_AES_CODE_44100       2
#define SGI_RAD1_AUDIO_AES_CODE_32000       3
#define SGI_RAD1_AUDIO_SLOT_AES_NO_INPUT    0x00080000u
#define SGI_RAD1_AUDIO_SLOT_DONE_WORD \
    (SGI_RAD1_AUDIO_SLOT_DONE_BIT | SGI_RAD1_AUDIO_SLOT_AES_NO_INPUT)
#define SGI_RAD1_AUDIO_SLOT_FLAGS_WORD      0x0u

/*
 * Output subcode windows. The driver writes AES3 and ADAT output subcode into
 * BAR0 with 64-bit stores and never reads it back: ADAT at 0x1000..0x10f7
 * (two buffers of 15 doublewords), AES3 at 0x1100..0x121f (two of 16, then
 * two more doublewords each). The model accepts and shadows them; no signal
 * path consumes them. The Bridge delivers a doubleword as two 4-byte writes
 * to a slot it treats as 32-bit, and as one 8-byte access, byte-swapped or
 * not, to a slot whose device register marks it 64-bit; which IRIX sets up
 * for RAD1 is unknown. The window takes 4-byte accesses only, and QEMU's
 * memory core splits an 8-byte one into two in address order. The shadow is
 * byte-addressed little-endian, so the split stores the same bytes a native
 * 8-byte write would, and the wider access would add no behavior.
 * 0x10f8..0x10ff, between the two, is not documented.
 */
#define SGI_RAD1_AUDIO_SUBCODE_FIRST        0x1000
#define SGI_RAD1_AUDIO_SUBCODE_BYTES        0x220
#define SGI_RAD1_AUDIO_SUBCODE_GAP_FIRST    0x10f8
#define SGI_RAD1_AUDIO_SUBCODE_GAP_LAST     0x10ff

/*
 * Register 0x34, which the driver reads only through slot +0x74, carries
 * per-input sync-present flags whose polarity is unknown. It is held constant
 * whatever the genlock property says; zero is a placeholder until the
 * polarity is settled, so a model with a reference connected shows the field
 * events below but not a sync-present flag.
 */
#define SGI_RAD1_AUDIO_SYNC_STATUS          0x0u

/*
 * Video reference, for video clock and genlock. The driver counts status
 * slots carrying +0x78 bit 0x80 or 0x100, divides elapsed time by the count,
 * and classifies the period as a 59.94 Hz or 50 Hz video field rate. So a
 * model presents a locked reference by emitting one of those bits once per
 * video field, and nothing for none.
 *
 * What is cabled to the genlock daughtercard's "Video Sync" inputs is the
 * host's to say, through the "genlock" property: "none" (the default), "ntsc"
 * (59.94 Hz fields, 16.683 ms) or "pal" (50 Hz fields, 20 ms). It describes
 * the cable, like the digital receiver properties, and like them is not
 * migrated: both ends of a migration must set it alike.
 *
 * Fields are counted from reset on the virtual clock. Each completion carries
 * one bit for each field that began since the last completion, at most two;
 * the completions are 250 us apart and fields at least 16 ms, so two happen
 * only after a gap in the status engine. Which bit belongs to which field is
 * not established: the register description reads them, as plausible, as one
 * bit per field parity. The model alternates them, 0x80 on even fields and
 * 0x100 on odd, a stated choice.
 */
#define SGI_RAD1_AUDIO_SLOT_FIELD_EVEN      0x80u
#define SGI_RAD1_AUDIO_SLOT_FIELD_ODD       0x100u
/* Fields per ns as a ratio: 60/1.001 per s for NTSC, 50 per s for PAL. */
#define SGI_RAD1_AUDIO_NTSC_FIELDS          60u
#define SGI_RAD1_AUDIO_NTSC_NS              1001000000u
#define SGI_RAD1_AUDIO_PAL_FIELDS           50u
#define SGI_RAD1_AUDIO_PAL_NS               1000000000u

typedef enum SGIRad1AudioGenlock {
    SGI_RAD1_AUDIO_GENLOCK_NONE,
    SGI_RAD1_AUDIO_GENLOCK_NTSC,
    SGI_RAD1_AUDIO_GENLOCK_PAL,
} SGIRad1AudioGenlock;

/*
 * The MIO mode-7 mux serial word. The driver shifts 16 bits out most
 * significant first: each bit as 0x8c bit 0, clocked by 0x4c bit 27 going
 * from clear to set, then latches the word with 0x88 = 2 followed by 0x88 =
 * 3. What the word selects on the daughtercard is only partly known, so the
 * model decodes it for tracing and for the read-only "mux-word" property and
 * does nothing else with it.
 */
#define SGI_RAD1_AUDIO_MUX_CLOCK            0x08000000u
#define SGI_RAD1_AUDIO_MUX_DATA             0x1u
#define SGI_RAD1_AUDIO_MUX_LATCH_ARM        2u
#define SGI_RAD1_AUDIO_MUX_LATCH_FIRE       3u
#define SGI_RAD1_AUDIO_MUX_BITS             16

/*
 * Continuous status-DMA engine. The one-shot start (0x40001) fills a single
 * slot and rad_one_shot_status busy-polls it. rad_start_status_dma instead
 * starts a ring engine (0x40003, the ring bit set) that keeps completing
 * slots around the ring on its own; rad_update_subsys walks the ring,
 * clearing each done bit as it consumes, and copies the last completed slot
 * for rad_update_dev, which takes each channel's DMA pointer and position
 * registers from it. The model runs a timer that fills the next ring slot at
 * each completion.
 *
 * CTRL_MASK strips the interval field ([16:2]) before the start words are
 * compared; the ring bit (0x2) distinguishes the continuous start from the
 * one-shot.
 */
#define SGI_RAD1_AUDIO_STATUS_CTRL_MASK     0x40003u /* interval masked out */

/*
 * Completion interval. rad_start_status_dma writes ((subsystem[0x234] * 33 +
 * 1) & 0x7fff) << 2 into bits [16:2], 8251 on IRIX 6.5.30. The available
 * register description suggests the field counts 33 MHz PCI clocks, about 250
 * us; both the unit and that it is an interval at all are inferred, so the
 * period is an approximation. It matters: the kernel mixer refills the output
 * ring only about 0xcc words ahead of the pointer it last saw in a slot, and
 * at 250 us the pointer moves about 22 words per completion.
 *
 * Completions run on an absolute schedule, deadline k at start + k * interval
 * clocks, so a late callback does not push later completions back. A callback
 * that runs late by less than a ring of slots leaves past-due deadlines
 * behind it, so the timer fires again at once and the missed completions
 * arrive back to back, all carrying the same pointer and 0x30 value; the
 * driver's walk takes them like any others. A callback more than a ring of
 * slots behind restarts the schedule from now instead of filling a whole lap.
 *
 * Two model policies are unspecified. A start word with a zero interval names
 * no period, so it stops the engine rather than running it. An interval below
 * INTERVAL_MIN, 10 us, is raised to it: a host-protection limit, since an
 * interval of 1 would ask for 33 million completions a second. IRIX writes
 * 8251, far above the floor.
 */
#define SGI_RAD1_AUDIO_STATUS_INTERVAL_SHIFT 2
#define SGI_RAD1_AUDIO_STATUS_INTERVAL_MASK  0x7fffu
#define SGI_RAD1_AUDIO_STATUS_INTERVAL_MIN   330u
#define SGI_RAD1_AUDIO_STATUS_CLOCK_HZ       33000000u


/*
 * The audio channels' streaming engine, shared by all six and both
 * directions; the numbers below are the analog pair's. From the enable edge
 * to its clear it walks the ring [start, start + count * 128) a frame at a
 * time and wraps to the start, with no per-chunk re-arm, paced by a
 * virtual-clock timer at the sample rate. The output reads each frame from
 * the ring and pushes it to the host voice; the input takes a frame from the
 * host voice and writes it into the ring. Each step adds the frames moved to
 * the channel's two position registers.
 *
 * Ring format, from the analog output sample format: a frame is two 32-bit
 * words, one channel each, holding a 24-bit signed sample in bits [23:0]. The
 * words cross the Bridge's byte-lane reversal like the status descriptors, so
 * the device sees them little-endian. The host voice is 16-bit: output takes
 * bits [23:8], input fills bits [23:8], sign-extends through [31:24] and
 * leaves [7:0] zero.
 *
 * The per-word conversions live in sgi-rad1-audio-pcm.h.
 *
 * Approximations, each flagged in the register description or stated here:
 * - 44.1 kHz, the software rate of type 1's clock generator in the live IRIX
 *   run. Which 0xa0/0xa4 bits select the hardware rate is unknown. Type 0's
 *   own clock-generator rate has not been read at all; the input uses 44.1
 *   kHz because types 0 and 1 share master clock +0x498 and the output's rate
 *   is the one measured.
 * - The first word of a frame is the left channel.
 * - The position registers count frames, not words.
 * - The input uses the output's frame layout. The documented layout covers
 *   output-ring content only; nothing yet shows input words.
 * - A host input voice that delivers fewer frames than the sample clock owes,
 *   or none at all, is padded with silence, so the ring and positions advance
 *   at the sample rate whatever the host does. A host input that runs slow
 *   therefore leaves silence gaps in the recording rather than stretching it.
 *
 * TICK_FRAMES frames per timer step, about 181 us, keeps the first change of
 * B inside the driver's 500 us poll window.
 */
#define SGI_RAD1_AUDIO_FREQ                 44100
#define SGI_RAD1_AUDIO_WORD_BYTES           4
#define SGI_RAD1_AUDIO_TICK_FRAMES          8
/* Ring bytes moved per DMA access; a whole number of frames of any type. */
#define SGI_RAD1_AUDIO_CHUNK_BYTES          1024

/*
 * The digital channels, flagged defaults for AES3 and ADAT, where the words
 * per frame are set outside the driver:
 * - AES3 frames are two words, left then right, like the analog ones.
 * - ADAT frames are eight words, one channel each, 24 bits in bits [23:0].
 *
 * Rates, approximations:
 * - The AES3 and ADAT outputs follow master clock +0x498 by default, as the
 *   analog pair does, so they use the analog output's measured 44.1 kHz.
 *   Their own clock generators' rates have not been read.
 * - The ADAT input follows the ADAT clock, whose rate is 48 kHz from creation
 *   and never written in the driver.
 * - The AES3 input follows the AES3 clock, which takes the rate of the locked
 *   source: "aes-input-rate", or the clock's creation default of 48 kHz when
 *   nothing is connected.
 *
 * Each digital interface binds its own optional audiodev, "aes-audiodev" and
 * "adat-audiodev"; the analog pair keeps "audiodev". An unbound digital
 * interface has no host voices: its outputs are dropped and its inputs record
 * silence. QEMU's mixer carries at most two channels a voice, so each digital
 * interface gets a stereo voice: AES3's two channels, or ADAT channels 1 and
 * 2. ADAT channels 3 to 8 are dropped on output and record silence on input.
 */
#define SGI_RAD1_AUDIO_ANALOG_WORDS         2
#define SGI_RAD1_AUDIO_AES_WORDS            2
#define SGI_RAD1_AUDIO_ADAT_WORDS           8
#define SGI_RAD1_AUDIO_ADAT_IN_FREQ         48000
#define SGI_RAD1_AUDIO_AES_DEFAULT_FREQ     48000
#define SGI_RAD1_AUDIO_HOST_CHANNELS        2

/*
 * The modeled DMA channels, indices into SGIRad1AudioState's chan[]; each
 * index is the driver's channel type.
 */
enum {
    SGI_RAD1_AUDIO_CH_IN,        /* type 0, analog input */
    SGI_RAD1_AUDIO_CH_OUT,       /* type 1, analog output */
    SGI_RAD1_AUDIO_CH_AES_IN,    /* type 2, AES3 input */
    SGI_RAD1_AUDIO_CH_AES_OUT,   /* type 3, AES3 output */
    SGI_RAD1_AUDIO_CH_ADAT_IN,   /* type 4, ADAT input */
    SGI_RAD1_AUDIO_CH_ADAT_OUT,  /* type 5, ADAT output */
    SGI_RAD1_AUDIO_CH_AES_SUB,   /* type 6, AES3-input subcode */
    SGI_RAD1_AUDIO_CH_ADAT_SUB,  /* type 7, ADAT-input subcode */
    SGI_RAD1_AUDIO_NUM_CH,
};

/*
 * One channel's engine. base and len are the ring latched from set A at the
 * enable edge; offset is the next frame's byte offset in it, so base + offset
 * is the DMA pointer the status slot reports, which holds its last value
 * after a stop. frames counts frames moved since start_ns, the virtual time
 * of the enable edge, and paces timer.
 */
typedef struct SGIRad1AudioChannel {
    SGIRad1AudioState *dev;
    /*
     * The channel's host voice, output or input, not migrated. An output
     * voice opens at realize, an input voice the first time the input runs;
     * each then stays open and is only toggled active while its channel runs,
     * as closing and reopening an output would make the wav backend truncate
     * its file on each stream. A backend with no input, such as wav, leaves
     * in_voice NULL, and the input then records silence; in_open_tried keeps
     * that failed open from repeating on every enable.
     */
    SWVoiceOut *out_voice;
    SWVoiceIn *in_voice;
    bool in_open_tried;
    QEMUTimer *timer;
    bool running;
    uint64_t base;
    uint32_t len;
    uint32_t offset;
    int64_t start_ns;
    uint64_t frames;
} SGIRad1AudioChannel;

/*
 * A named field per documented offset rather than an array over the whole
 * 0x44..0xbc span: the span has gaps no source documents (for example
 * 0x60..0x6c), and those must still log as unimplemented instead of silently
 * taking a store the way a documented offset does. The dense 0x400..0x528 DMA
 * bank, by contrast, is a store-backed array.
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

    /* Output subcode shadow, 0x1000..0x121f, and its BAR0 subregion. */
    MemoryRegion subcode_mr;
    uint8_t subcode[SGI_RAD1_AUDIO_SUBCODE_BYTES];

    /* Sample-position bank 0x04..0x2c, advanced by the channel engines. */
    uint32_t sample_pos[SGI_RAD1_AUDIO_SAMPLE_POS_WORDS];

    SGIRad1AudioChannel chan[SGI_RAD1_AUDIO_NUM_CH];

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
     * Host audio backends. audio_be, the "audiodev" property, serves the
     * analog pair and falls back to the default backend; aes_be and adat_be,
     * "aes-audiodev" and "adat-audiodev", serve the digital interfaces only
     * when set.
     */
    AudioBackend *audio_be;
    AudioBackend *aes_be;
    AudioBackend *adat_be;

    /* The host's description of the digital input cables; see above. */
    uint32_t aes_input_rate;
    bool adat_input;

    /*
     * The video reference cabled to the genlock inputs; see above.
     * genlock_fields counts the fields already reported in a completion.
     */
    char *genlock;
    SGIRad1AudioGenlock genlock_mode;
    uint64_t genlock_fields;

    /* The mux serial word: bits shifted in so far, and the last latched. */
    uint32_t mux_shift;
    uint32_t mux_word;
};

#endif
