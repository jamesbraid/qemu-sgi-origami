/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI DMediaPro DM2 (XT-DIGVID) XIO video board: the board's XIO side, with
 * a virtual Video Breakout Box (VBOB) behind its control channel.
 *
 * The board is one widget on a Crossbow option port. The model has the
 * widget and XG registers, the map RAM, the FPGA programming register and
 * the load handshake of all eight FPGAs, the GIF register file, DMA control
 * with one slot completed per field, the status ring and its interrupts, and
 * the VBOB message channel with genlock reporting. It runs no FPGA image and
 * moves no video payload.
 *
 * Every behaviour, and every value the hardware does not state, is in
 * docs/system/mips/sgi/hardware/xt-digvid.rst. The page rates each fact and
 * collects the model's choices in its "Model choices" table; comments here
 * cite its section names and do not repeat the ratings.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-widget.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SGI_XT_DIGVID "sgi-xt-digvid"
OBJECT_DECLARE_TYPE(DM2State, DM2Class, SGI_XT_DIGVID)

/* "Identity" and "Window map". */
#define DM2_WINDOW_SIZE         (16 * MiB)
#define DM2_PART                0xc102
#define DM2_MANUFACTURER        682
#define DM2_ID                  ((DM2_PART << 12) | (DM2_MANUFACTURER << 1) | 1)
#define DM2_BOARD_PART          "030-1653-002"
#define DM2_BOARD_NAME          "XT-DIGVID"
/* The revision text is free; nothing validates it. */
#define DM2_BOARD_REVISION      "G"

/* "Widget block and XG registers": window offsets of the second words. */
#define XG_ID                   0x04
#define XG_STATUS               0x0c
#define XG_ERR_ADDR_UPPER       0x14
#define XG_ERR_ADDR_LOWER       0x1c
#define XG_CONTROL              0x24
#define XG_REQ_TIMEOUT          0x2c
#define XG_INT_DEST_UPPER       0x34
#define XG_INT_DEST_LOWER       0x3c
#define XG_ERR_CMD              0x44
#define XG_LLP_CONFIG           0x4c
#define XG_FLUSH                0x54
#define XG_VECTORS              0x5c
#define XG_CONFIG               0x64
#define XG_ERROR_CAUSE          0x6c
#define XG_WATER_MARKS          0x74
#define XG_MAP_ERR_ADDR         0x94
#define XG_NIC                  0x9c
#define XG_SIZE                 0x100

#define XG_CONTROL_ERR_ENABLE   BIT(17)
#define XG_VECTORS_VIDEO0_EN    BIT(8)
#define XG_VECTORS_VIDEO1_EN    BIT(17)
#define XG_CONFIG_VALUE         0x09
#define XG_WATER_MASK           0x0fffffff
#define XG_MAP_ERR_MASK         0xfffff

/* "Error cause bits". */
#define XG_ERR_MAP_INVALID      BIT(13)

/* The one-wire register uses the Bridge's master layout. */
#define NIC_LEVEL               BIT(0)
#define NIC_DONE                BIT(1)
#define NIC_SAMPLE_SHIFT        2
#define NIC_PULSE_SHIFT         10
#define NIC_WRITABLE            0x000ffffcu

/* "Map RAM". */
#define MAP_BASE                0x100000
#define MAP_ENTRIES             32768
#define MAP_SIZE                (MAP_ENTRIES * 4)
#define MAP_PAGE_SHIFT          14
#define MAP_VALID               BIT(29)
#define MAP_INVALID             BIT(28)

/* "FPGA programming". */
#define FPGA_REG                0x120000
#define FPGA_CLOCK              BIT(8)
#define FPGA_STROBE             BIT(9)
#define FPGA_COUNT              8
#define FPGA_GIF                0
#define GIF_LOAD_CLOCKS         (8u * 179233u)
#define FPGA_PORT_STRIDE        0x800
#define FPGA_SELECT_STROBE      BIT(7)
#define FPGA_READY              BIT(7)

/* "GIF register file". */
#define GIF_BASE                0x800000
#define GIF_SIZE                0x4000
#define PORT_BASE               0x880000
#define GIF_SLOTS               (GIF_SIZE / 8)
#define GIF_ID_VALUE            0x47494601
#define GIF_IDX(addr)           (((addr) - GIF_BASE) >> 3)

/* "DMA", "DMA control word" and "Status ring". */
#define PIPE_OUT                0
#define PIPE_IN                 1
#define PIPE_COUNT              2
#define SLOTS                   4
#define CTL_VALID_MASK          0xf
#define CTL_ABORT               BIT(4)
#define CTL_IE                  BIT(16)
#define CTL_STORE_OUT           0x1e7fe0
#define CTL_STORE_IN            0x7fe0
#define EN_VIDEO                BIT(0)
#define SLOT_WINDOW             (16 * MiB)
#define SLOT_WINDOW_OUT         0x4000000u
#define START_MASK              0xfffff8

#define RING_ENTRIES            16
#define RING_ENTRY_SIZE         0x80
#define RING_BOARD_ADDR_OUT     0x8000000u
#define RING_BOARD_ADDR_IN      0x9000000u
#define RING_FLAG_VALID         BIT(0)
#define RING_FLAG_INPUT_OFF     BIT(2)
#define RING_FLAG_INPUT_CHANGE  BIT(5)
#define RING_FLAG_PARITY        BIT(6)
#define RING_FLAG_FIELD         BIT(7)
#define RING_FLAG_SLOT_DONE(k)  BIT(8 + (k))
#define RING_FLAG_SLOT_ABORTED(k) BIT(12 + (k))
#define RING_FLAG_LINK_TX       BIT(18)
#define RING_FLAG_LINK_RX       BIT(19)
#define STREAM_COUNTER_MASK     0xffffff
/* How often a held status entry looks at the host's VALID bit again. */
#define RING_POLL_NS            (1 * SCALE_MS)

/* "VBOB control channel". */
#define VBOB_WORDS              12
#define VBOB_MSG_BYTES          24
#define VBOB_RXQ                8
#define VBOB_INPUT_SLOTS        6
#define SIDES                   2
#define SIDE_HD                 0
#define SIDE_SD                 1
#define VBOB_PORTS              16
#define TIMING_NONE             1
#define TIMING_525              3
#define TIMING_625              4
#define TIMING_FIRST            3
#define TIMING_LAST             17

/* Message registers. */
#define MSG_TV_STANDARD         0x20
#define MSG_GENLOCK_SOURCE      0x21
#define MSG_TIMING              0x22
#define MSG_ROUTE_IN            0x24
#define MSG_ROUTE_OUT           0x25
#define MSG_SYNC_ENABLE         0x26
#define MSG_INPUT_ENABLE        0x27
#define MSG_MISC                0x28
#define MSG_INFO                0x40
#define MSG_RESET               0x47
#define MSG_NOTIFY_SYNC         0xa6
#define MSG_NOTIFY_INPUT        0xa7

typedef enum {
    GR_PLAIN,
    GR_ID,
    GR_SELECT,
    GR_CTL_OUT,
    GR_CTL_IN,
    GR_RING_OUT,
    GR_RING_IN,
    GR_RESET_OUT,
    GR_RESET_IN,
    GR_ENABLE_OUT,
    GR_ENABLE_IN,
    GR_LUT_DATA,
    GR_TX,
    GR_RX,
    GR_DOORBELL,
    GR_RX_RELEASE,
} GifKind;

typedef struct GifReg {
    uint32_t addr;
    uint32_t mask;
    GifKind kind;
} GifReg;

#define FULL 0xffffffffu

/*
 * The registers the page lists for the GIF file, with the diagnostic's
 * storage masks where it gives one ("Reset and storage"). Anything not here
 * is an unimplemented offset.
 */
static const GifReg gif_regs[] = {
    /* "Core and the two pipes" */
    { 0x800004, FULL, GR_PLAIN },
    { 0x80000c, 0xff, GR_SELECT },
    { 0x800014, 0, GR_CTL_OUT },
    { 0x80001c, START_MASK, GR_PLAIN },
    { 0x800024, START_MASK, GR_PLAIN },
    { 0x80002c, 0x3ffffff, GR_PLAIN },
    { 0x800034, 0xfff, GR_PLAIN },
    { 0x80003c, 0, GR_CTL_IN },
    { 0x800044, START_MASK, GR_PLAIN },
    { 0x80004c, START_MASK, GR_PLAIN },
    { 0x800054, 0x3ffffff, GR_PLAIN },
    { 0x80005c, 0xfff, GR_PLAIN },
    { 0x800064, 0, GR_RING_IN },
    { 0x80006c, 0, GR_RESET_IN },
    { 0x800074, 0, GR_RING_OUT },
    { 0x80007c, 0, GR_RESET_OUT },
    { 0x800084, 0, GR_ID },
    { 0x80008c, FULL, GR_PLAIN },
    /* Packer and unpacker constants */
    { 0x800804, FULL, GR_PLAIN },
    { 0x80080c, FULL, GR_PLAIN },
    { 0x800814, FULL, GR_PLAIN },
    { 0x800824, 0x5, GR_PLAIN },
    { 0x800844, FULL, GR_PLAIN },
    { 0x80084c, FULL, GR_PLAIN },
    { 0x800854, FULL, GR_PLAIN },
    { 0x801004, FULL, GR_PLAIN },
    { 0x80100c, FULL, GR_PLAIN },
    { 0x80101c, 0xffd, GR_PLAIN },
    { 0x801044, FULL, GR_PLAIN },
    { 0x80104c, FULL, GR_PLAIN },
    /* VIF */
    { 0x801804, 0x3fff, GR_PLAIN },
    { 0x80180c, 0x3, GR_ENABLE_IN },
    { 0x801814, FULL, GR_PLAIN },
    { 0x80182c, 0xff, GR_PLAIN },
    { 0x801884, 0x3fffff, GR_PLAIN },
    { 0x80188c, 0x3fffff, GR_PLAIN },
    { 0x801894, FULL, GR_PLAIN },
    /* HDOC */
    { 0x802004, 0x3ffffff, GR_PLAIN },
    { 0x80200c, 0x1, GR_ENABLE_OUT },
    { 0x802014, 0x7fffff, GR_PLAIN },
    { 0x80201c, 0xffffff, GR_PLAIN },
    { 0x802024, 0xffffff, GR_PLAIN },
    { 0x80202c, 0x3fffff, GR_PLAIN },
    { 0x802034, 0x3fffff, GR_PLAIN },
    { 0x80203c, 0x3fffff, GR_PLAIN },
    { 0x802044, 0x3fffff, GR_PLAIN },
    { 0x80204c, 0x3fffff, GR_PLAIN },
    { 0x802054, 0xffffff, GR_PLAIN },
    { 0x80205c, 0x7fffff, GR_PLAIN },
    { 0x802064, 0x7fffff, GR_PLAIN },
    { 0x80206c, 0x7fffff, GR_PLAIN },
    { 0x802074, 0xffffff, GR_PLAIN },
    { 0x80207c, 0xffffff, GR_PLAIN },
    { 0x802084, 0xffffff, GR_PLAIN },
    { 0x8020b4, FULL, GR_PLAIN },
    { 0x8020bc, FULL, GR_PLAIN },
    { 0x8020c4, FULL, GR_PLAIN },
    { 0x8020cc, FULL, GR_PLAIN },
    { 0x8020d4, FULL, GR_PLAIN },
    { 0x8020dc, FULL, GR_PLAIN },
    { 0x8020e4, FULL, GR_PLAIN },
    { 0x8020ec, FULL, GR_PLAIN },
    /* Input colour converter, the module's "programmable" layout */
    { 0x802804, FULL, GR_PLAIN },
    { 0x80280c, 0, GR_LUT_DATA },
    { 0x802814, FULL, GR_PLAIN },
    { 0x80281c, FULL, GR_PLAIN },
    { 0x802824, FULL, GR_PLAIN },
    { 0x80282c, FULL, GR_PLAIN },
    { 0x802834, FULL, GR_PLAIN },
    { 0x80283c, FULL, GR_PLAIN },
    { 0x802844, FULL, GR_PLAIN },
    { 0x80284c, FULL, GR_PLAIN },
    { 0x802854, FULL, GR_PLAIN },
    { 0x80285c, FULL, GR_PLAIN },
    /* Output colour converter */
    { 0x803004, FULL, GR_PLAIN },
    { 0x80300c, 0, GR_LUT_DATA },
    { 0x803014, FULL, GR_PLAIN },
    { 0x80301c, FULL, GR_PLAIN },
    { 0x803024, FULL, GR_PLAIN },
    { 0x80302c, FULL, GR_PLAIN },
    { 0x803034, FULL, GR_PLAIN },
    { 0x80303c, FULL, GR_PLAIN },
    { 0x803044, FULL, GR_PLAIN },
    { 0x80304c, FULL, GR_PLAIN },
    { 0x803054, FULL, GR_PLAIN },
    { 0x80305c, FULL, GR_PLAIN },
    { 0x803064, FULL, GR_PLAIN },
    { 0x80306c, FULL, GR_PLAIN },
    /* CLINK: twelve transmit halfwords, then the link and receive words */
    { 0x803804, 0xffff, GR_TX },
    { 0x80380c, 0xffff, GR_TX },
    { 0x803814, 0xffff, GR_TX },
    { 0x80381c, 0xffff, GR_TX },
    { 0x803824, 0xffff, GR_TX },
    { 0x80382c, 0xffff, GR_TX },
    { 0x803834, 0xffff, GR_TX },
    { 0x80383c, 0xffff, GR_TX },
    { 0x803844, 0xffff, GR_TX },
    { 0x80384c, 0xffff, GR_TX },
    { 0x803854, 0xffff, GR_TX },
    { 0x80385c, 0xffff, GR_TX },
    { 0x803864, 0xffff, GR_PLAIN },
    { 0x80386c, 0xff, GR_PLAIN },
    { 0x803874, 0x1ff, GR_PLAIN },
    { 0x803884, 0, GR_RX },
    { 0x80388c, 0, GR_RX },
    { 0x803894, 0, GR_RX },
    { 0x80389c, 0, GR_RX },
    { 0x8038a4, 0, GR_RX },
    { 0x8038ac, 0, GR_RX },
    { 0x8038b4, 0, GR_RX },
    { 0x8038bc, 0, GR_RX },
    { 0x8038c4, 0, GR_RX },
    { 0x8038cc, 0, GR_RX },
    { 0x8038d4, 0, GR_RX },
    { 0x8038dc, 0, GR_RX },
    { 0x8038e4, 0x1, GR_PLAIN },
    { 0x8038ec, 0, GR_DOORBELL },
    { 0x8038f4, 0, GR_RX_RELEASE },
};

#define CLINK_TX_FIRST          0x803804
#define CLINK_RX_FIRST          0x803884

/*
 * The table of the FPGA load path, indexed by the block the FPGA owns in
 * the GIF register file ("The programming register").
 */
typedef struct FpgaInfo {
    const char *name;
    uint8_t kind;
    uint32_t bytes;
    uint8_t select;
    uint8_t done_bit;
} FpgaInfo;

static const FpgaInfo fpga_info[FPGA_COUNT] = {
    { "GIF", 1, 179233, 0x00, 0 },
    { "packer", 0, 104066, 0x01, 2 },
    { "unpacker", 0, 104066, 0x02, 4 },
    { "VIF", 0, 104066, 0x04, 10 },
    { "HDOC", 0, 104066, 0x08, 12 },
    { "input colour converter", 2, 318256, 0x10, 6 },
    { "output colour converter", 2, 318256, 0x20, 8 },
    { "CLINK", 0, 35428, 0x40, 14 },
};

typedef struct DM2Fpga {
    bool done;
    /* INIT reads one when it is not asserted. */
    bool init;
    /* Between a reset pulse and DONE: bytes are being counted. */
    bool loading;
    uint32_t count;
} DM2Fpga;

typedef struct DM2Pipe {
    /* The stored control bits; the latches, abort and enable are below. */
    uint32_t ctl;
    bool ie;
    uint8_t armed;
    /* The slots in the order they were armed. */
    uint8_t queue[SLOTS];
    uint8_t queued;
    uint32_t stream_counter;
    uint32_t field_ints;
    uint8_t ring_head;
    bool pending;
    uint32_t pending_flags;
    QEMUTimer *field_timer;
    QEMUTimer *poll_timer;
} DM2Pipe;

typedef struct DM2Frame {
    uint8_t bytes[VBOB_MSG_BYTES];
} DM2Frame;

typedef struct DM2Vbob {
    /* The properties of the virtual endpoint's world. */
    uint8_t output_override;
    uint8_t input_timings[VBOB_INPUT_SLOTS];
    uint8_t present;
    /* The state the host sets over the control channel. */
    uint8_t tv_standard;
    /* Genlock source of each side, HD then SD. */
    uint8_t source[SIDES];
    /* The latest timing record of each side, and the side of the latest. */
    uint8_t record[SIDES][VBOB_MSG_BYTES];
    bool have_record[SIDES];
    uint8_t active_side;
    /* Crosspoints: source port by destination port, input then output. */
    uint8_t routing[2][VBOB_PORTS];
    uint8_t sync_enable;
    uint8_t input_enable;
    uint8_t misc;
    /* What was last reported to the host. */
    bool reported_locked[SIDES];
    uint8_t reported_input[2];
    bool reported_present;
    /* The receive buffer and frames waiting for it. */
    bool rx_busy;
    DM2Frame rxq[VBOB_RXQ];
    uint8_t rxq_head;
    uint8_t rxq_count;
} DM2Vbob;

struct DM2State {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    AddressSpace fabric_as;
    MemoryRegion regs;

    uint32_t control;
    uint32_t int_dest_upper;
    uint32_t int_dest_lower;
    uint32_t vectors;
    uint32_t water_marks;
    uint32_t misc[6];
    uint32_t error_cause;
    uint32_t map_err_addr;
    uint32_t nic_control;

    uint32_t map[MAP_ENTRIES];
    uint32_t fpga_ctl;
    DM2Fpga fpga[FPGA_COUNT];
    uint32_t gif[GIF_SLOTS];
    DM2Pipe pipe[PIPE_COUNT];
    DM2Vbob vbob;

    /* The one-wire record's barcode, built per slot. */
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];
};

struct DM2Class {
    SGIXioBoardClass parent_class;
    DeviceUnrealize parent_unrealize;
    ResettablePhases parent_phases;
};

static uint8_t gif_lookup[GIF_SLOTS];

static const GifReg *gif_find(uint32_t addr)
{
    unsigned idx = gif_lookup[GIF_IDX(addr)];

    /* The first word of a slot shares its index and is not a register. */
    if (!idx || gif_regs[idx - 1].addr != addr) {
        return NULL;
    }
    return &gif_regs[idx - 1];
}

static void gif_lookup_init(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(gif_regs); i++) {
        gif_lookup[GIF_IDX(gif_regs[i].addr)] = i + 1;
    }
}

/* ---- Interrupts and DMA through the fabric ---- */

static unsigned dm2_host_widget(DM2State *s)
{
    return sgi_xio_int_dest(s->int_dest_upper, s->int_dest_lower).widget;
}

static bool dm2_packet(DM2State *s, hwaddr host, hwaddr *packet)
{
    return sgi_xio_compose(TYPE_SGI_XT_DIGVID, dm2_host_widget(s), host,
                           packet);
}

/* "Interrupts": a write of the vector to the destination address. */
static void dm2_send_irq(DM2State *s, uint8_t vector)
{
    SGIXioIntDest dest = sgi_xio_int_dest(s->int_dest_upper,
                                          s->int_dest_lower);
    hwaddr packet;

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": interrupt "
                      "with no destination programmed; dropped\n");
        return;
    }
    if (!sgi_xio_compose(TYPE_SGI_XT_DIGVID, dest.widget, dest.address,
                         &packet)) {
        return;
    }
    sgi_xio_send_interrupt(&s->fabric_as, packet, vector, true);
}

static void dm2_video_irq(DM2State *s, unsigned pipe)
{
    /* Video 0 belongs to the input pipe and video 1 to the output pipe. */
    if (pipe == PIPE_IN) {
        if (s->vectors & XG_VECTORS_VIDEO0_EN) {
            dm2_send_irq(s, extract32(s->vectors, 0, 8));
        }
    } else if (s->vectors & XG_VECTORS_VIDEO1_EN) {
        dm2_send_irq(s, extract32(s->vectors, 9, 8));
    }
}

/* "Error cause bits": the model raises bit 13 alone. */
static void dm2_raise_error(DM2State *s, uint32_t cause, uint32_t map_offset)
{
    s->error_cause |= cause;
    if (cause & XG_ERR_MAP_INVALID) {
        s->map_err_addr = map_offset & XG_MAP_ERR_MASK;
    }
    if (s->control & XG_CONTROL_ERR_ENABLE) {
        dm2_send_irq(s, extract32(s->vectors, 18, 8));
    }
}

static bool dm2_map_valid(DM2State *s, uint32_t board_addr)
{
    uint32_t index = board_addr >> MAP_PAGE_SHIFT;
    uint32_t entry = index < MAP_ENTRIES ? s->map[index] : 0;

    return (entry & MAP_VALID) && !(entry & MAP_INVALID);
}

/*
 * "Map RAM": the valid test is bit 29 set and bit 28 clear. A page through
 * an invalid entry raises error cause bit 13 and records the entry's byte
 * offset.
 */
static bool dm2_translate(DM2State *s, uint32_t board_addr, hwaddr *host)
{
    uint32_t index = board_addr >> MAP_PAGE_SHIFT;
    uint32_t entry = index < MAP_ENTRIES ? s->map[index] : 0;

    if (!(entry & MAP_VALID) || (entry & MAP_INVALID)) {
        dm2_raise_error(s, XG_ERR_MAP_INVALID, index * 4);
        return false;
    }
    *host = ((hwaddr)extract32(entry, 2, 26) << MAP_PAGE_SHIFT) |
            (board_addr & ((1u << MAP_PAGE_SHIFT) - 1));
    return true;
}

/* ---- The status ring ---- */

static uint32_t dm2_ring_base(unsigned pipe)
{
    return pipe == PIPE_IN ? RING_BOARD_ADDR_IN : RING_BOARD_ADDR_OUT;
}

/*
 * Write the pipe's pending entry, if the entry at the head is free. A head
 * that still has VALID set holds the entry until the host clears it
 * ("Status ring and interrupt acknowledgement").
 */
/*
 * Whether an entry for the pipe can be written now: the GIF is configured
 * and the ring page at the head has a valid map RAM entry.
 */
static bool dm2_ring_ready(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];

    return s->fpga[FPGA_GIF].done &&
           dm2_map_valid(s, dm2_ring_base(p) + pp->ring_head * RING_ENTRY_SIZE);
}

static void dm2_rx_next(DM2State *s);

/*
 * Drop the pipe's pending entry. If it carried link receive done, the host
 * will never be told of the frame and so never release the buffer: free it
 * and go on with the frames behind it.
 */
static void dm2_ring_drop(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];
    bool had_rx = pp->pending_flags & RING_FLAG_LINK_RX;

    pp->pending = false;
    pp->pending_flags = 0;
    if (had_rx) {
        s->vbob.rx_busy = false;
        dm2_rx_next(s);
    }
}

static void dm2_ring_flush(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];
    uint32_t board_addr = dm2_ring_base(p) + pp->ring_head * RING_ENTRY_SIZE;
    uint8_t zeros[RING_ENTRY_SIZE - 8] = { 0 };
    hwaddr host, packet;
    MemTxResult res;
    uint32_t cur;

    if (!pp->pending) {
        return;
    }
    /*
     * Before the GIF is configured, or while the ring page is unmapped,
     * there is nowhere to write: drop the entry without an error.
     */
    if (!s->fpga[FPGA_GIF].done || !dm2_map_valid(s, board_addr) ||
        !dm2_translate(s, board_addr, &host) ||
        !dm2_packet(s, host, &packet)) {
        dm2_ring_drop(s, p);
        return;
    }
    cur = address_space_ldl_be(&s->fabric_as, packet + 4,
                               MEMTXATTRS_UNSPECIFIED, &res);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": status ring "
                      "read at host 0x%" HWADDR_PRIx " failed\n", host);
        dm2_ring_drop(s, p);
        return;
    }
    if (cur & RING_FLAG_VALID) {
        timer_mod(pp->poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RING_POLL_NS);
        return;
    }
    /* Word 0 before the flag word, so a consumer sees whole entries. */
    res = address_space_write(&s->fabric_as, packet + 8,
                             MEMTXATTRS_UNSPECIFIED, zeros, sizeof(zeros));
    if (res == MEMTX_OK) {
        address_space_stl_be(&s->fabric_as, packet,
                             pp->stream_counter & STREAM_COUNTER_MASK,
                             MEMTXATTRS_UNSPECIFIED, &res);
    }
    if (res == MEMTX_OK) {
        address_space_stl_be(&s->fabric_as, packet + 4,
                             pp->pending_flags | RING_FLAG_VALID,
                             MEMTXATTRS_UNSPECIFIED, &res);
    }
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": status ring "
                      "write at host 0x%" HWADDR_PRIx " failed; entry "
                      "dropped\n", host);
        dm2_ring_drop(s, p);
        return;
    }
    pp->ring_head = (pp->ring_head + 1) % RING_ENTRIES;
    pp->pending = false;
    pp->pending_flags = 0;
    dm2_video_irq(s, p);
}

static void dm2_ring_post(DM2State *s, unsigned p, uint32_t flags)
{
    DM2Pipe *pp = &s->pipe[p];

    pp->pending_flags |= flags;
    pp->pending = true;
    dm2_ring_flush(s, p);
}

/* ---- Per-pipe field timing ---- */

typedef struct Rate {
    uint32_t num;
    uint32_t den;
} Rate;

/*
 * The field (segment for PsF, frame for progressive) rate of a timing the
 * endpoint was given. Anything else, including none given, runs at the
 * page's default of 1001/60000 s.
 */
static Rate dm2_field_rate(uint8_t code)
{
    switch (code) {
    case 4:  /* 625 */
    case 16: /* 1080 50i */
        return (Rate){ 50, 1 };
    case 6:  /* 720 60p */
    case 8:  /* 1080 60i */
        return (Rate){ 60, 1 };
    case 9:  /* 1080 29.97p */
        return (Rate){ 30000, 1001 };
    case 10: /* 1080 30p */
        return (Rate){ 30, 1 };
    case 11: /* 1080 23.98p */
        return (Rate){ 24000, 1001 };
    case 12: /* 1080 24p */
        return (Rate){ 24, 1 };
    case 13: /* 1080 23.98 PsF */
        return (Rate){ 48000, 1001 };
    case 14: /* 1080 24 PsF */
        return (Rate){ 48, 1 };
    case 15: /* 1080 25p */
        return (Rate){ 25, 1 };
    default:
        return (Rate){ 60000, 1001 };
    }
}

/*
 * "Timing record": the rows of the module's timing table, as the record
 * carries them, and the wire code each reaches. 1080 25 PsF has the bytes
 * of 1080 50i and no code of its own.
 */
typedef struct TimingRow {
    uint8_t code;
    uint16_t samples;
    uint16_t lines;
    uint8_t first_active;
    uint8_t clock;
    bool progressive;
} TimingRow;

static const TimingRow timing_rows[] = {
    { 3, 858, 525, 16, 1, false },
    { 4, 864, 625, 22, 1, false },
    { 8, 2200, 1125, 20, 5, false },
    { 7, 2200, 1125, 20, 4, false },
    { 16, 2640, 1125, 20, 5, false },
    { 10, 2200, 1125, 41, 5, true },
    { 9, 2200, 1125, 41, 4, true },
    { 15, 2640, 1125, 41, 5, true },
    { 12, 2750, 1125, 41, 5, true },
    { 11, 2750, 1125, 41, 4, true },
    { 14, 2750, 1125, 20, 5, false },
    { 13, 2750, 1125, 20, 4, false },
    { 17, 2200, 1125, 40, 4, false },
    { 6, 1650, 750, 25, 5, true },
    { 5, 1650, 750, 25, 4, true },
};

static unsigned dm2_code_side(uint8_t code)
{
    return code == TIMING_525 || code == TIMING_625 ? SIDE_SD : SIDE_HD;
}

/* The wire code of a record, or 0 when no row has its counts. */
static uint8_t dm2_record_code(const uint8_t *m)
{
    uint16_t samples = (m[4] << 8) | m[5];
    uint16_t lines = (m[8] << 8) | m[9];
    bool progressive = !(m[14] | m[15]);

    for (unsigned i = 0; i < ARRAY_SIZE(timing_rows); i++) {
        const TimingRow *r = &timing_rows[i];

        if (r->samples == samples && r->lines == lines &&
            r->first_active == m[16] && r->clock == m[18] &&
            r->progressive == progressive) {
            return r->code;
        }
    }
    return 0;
}

/*
 * "Timing record": line period is the samples per line over the luma
 * clock, the frame period is the lines times that, and a field is the
 * frame for a progressive record, else half. 0 for a record whose clock
 * code names no luma clock.
 */
static int64_t dm2_record_field_ns(const uint8_t *m)
{
    uint64_t samples = (m[4] << 8) | m[5];
    uint64_t lines = (m[8] << 8) | m[9];
    bool progressive = !(m[14] | m[15]);
    uint64_t pixels = samples * lines;
    int64_t frame;

    if (!pixels) {
        return 0;
    }
    /* Nanoseconds a frame takes at each luma clock, in kHz-scaled terms. */
    if (m[3] == 6) {
        frame = muldiv64(pixels, 1000000, 13500);
    } else if (m[18] == 5) {
        frame = muldiv64(pixels, 1000000, 74250);
    } else if (m[18] == 4) {
        /* 74.25 MHz / 1.001 */
        frame = muldiv64(pixels * 1001, 1000, 74250);
    } else {
        return 0;
    }
    return progressive ? frame : frame / 2;
}

/*
 * The field period: the property override, else the latest timing
 * record's, else 1001/60000 s.
 */
static int64_t dm2_field_period_ns(DM2State *s)
{
    DM2Vbob *v = &s->vbob;
    Rate r;

    if (!v->output_override && v->have_record[v->active_side]) {
        int64_t ns = dm2_record_field_ns(v->record[v->active_side]);

        if (ns) {
            return ns;
        }
    }
    r = dm2_field_rate(v->output_override);
    return muldiv64(NANOSECONDS_PER_SECOND, r.den, r.num);
}

/*
 * The input pipe has a signal when the HD or the SD input slot (bytes 4
 * and 6 of the input-signal reply, slots 0 and 2) is present.
 */
#define INPUT_SLOT_HD   0
#define INPUT_SLOT_SD   2

static bool dm2_input_present(DM2State *s)
{
    return s->vbob.present & (BIT(INPUT_SLOT_HD) | BIT(INPUT_SLOT_SD));
}

static uint32_t dm2_enable_reg(DM2State *s, unsigned p)
{
    return s->gif[GIF_IDX(p == PIPE_IN ? 0x80180c : 0x80200c)];
}

/*
 * A pipe runs its fields while its video enable is set or a slot is armed,
 * and the input pipe only with an input signal ("Interrupts" and "The data
 * transfer").
 */
static bool dm2_pipe_running(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];

    if (!s->fpga[FPGA_GIF].done) {
        return false;
    }
    if (!(pp->armed || (dm2_enable_reg(s, p) & EN_VIDEO))) {
        return false;
    }
    return p == PIPE_OUT || dm2_input_present(s);
}

static void dm2_pipe_update(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];

    if (!pp->field_timer) {
        return;
    }
    if (dm2_pipe_running(s, p)) {
        if (!timer_pending(pp->field_timer)) {
            timer_mod(pp->field_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      dm2_field_period_ns(s));
        }
    } else {
        timer_del(pp->field_timer);
    }
}

static bool dm2_slot_start_page(DM2State *s, unsigned p, unsigned slot)
{
    uint32_t start = s->gif[GIF_IDX(p == PIPE_IN ? 0x800044 : 0x80001c)];
    uint32_t window = p == PIPE_IN ? 0 : SLOT_WINDOW_OUT;
    hwaddr host;

    /* Validate the page of the first start address; no payload moves. */
    return dm2_translate(s, window + slot * SLOT_WINDOW + (start & START_MASK),
                         &host);
}

static void dm2_field_tick(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];
    uint32_t flags = RING_FLAG_FIELD;

    pp->stream_counter = (pp->stream_counter + 1) & STREAM_COUNTER_MASK;
    /* "Pipe start, counters and timing": 1 on a stream's first field. */
    if (!(pp->field_ints & 1)) {
        flags |= RING_FLAG_PARITY;
    }
    pp->field_ints++;
    if (pp->queued) {
        unsigned slot = pp->queue[0];

        memmove(pp->queue, pp->queue + 1, --pp->queued);
        pp->armed &= ~BIT(slot);
        /* An invalid page raises the error and aborts the slot. */
        flags |= dm2_slot_start_page(s, p, slot) ?
                 RING_FLAG_SLOT_DONE(slot) : RING_FLAG_SLOT_ABORTED(slot);
    }
    dm2_ring_post(s, p, flags);
    dm2_pipe_update(s, p);
}

static void dm2_field_tick_out(void *opaque)
{
    dm2_field_tick(opaque, PIPE_OUT);
}

static void dm2_field_tick_in(void *opaque)
{
    dm2_field_tick(opaque, PIPE_IN);
}

static void dm2_ring_poll_out(void *opaque)
{
    dm2_ring_flush(opaque, PIPE_OUT);
}

static void dm2_ring_poll_in(void *opaque)
{
    dm2_ring_flush(opaque, PIPE_IN);
}

/* "DMA control word". */
static uint32_t dm2_ctl_read(DM2State *s, unsigned p)
{
    DM2Pipe *pp = &s->pipe[p];

    return pp->ctl | pp->armed | (pp->ie ? CTL_IE : 0);
}

static void dm2_ctl_write(DM2State *s, unsigned p, uint32_t value)
{
    DM2Pipe *pp = &s->pipe[p];

    pp->ctl = value & (p == PIPE_IN ? CTL_STORE_IN : CTL_STORE_OUT);
    pp->ie = value & CTL_IE;
    if (value & CTL_ABORT) {
        /*
         * "Abort": slot-aborted for each armed incomplete slot in arming
         * order, one entry each; the latches clear and the bit is not kept.
         */
        uint8_t queue[SLOTS];
        unsigned n = pp->queued;

        memcpy(queue, pp->queue, sizeof(queue));
        pp->armed = 0;
        pp->queued = 0;
        for (unsigned i = 0; i < n; i++) {
            dm2_ring_post(s, p, RING_FLAG_SLOT_ABORTED(queue[i]));
        }
    } else {
        for (unsigned k = 0; k < SLOTS; k++) {
            if ((value & BIT(k)) && !(pp->armed & BIT(k))) {
                pp->armed |= BIT(k);
                pp->queue[pp->queued++] = k;
            }
        }
    }
    dm2_pipe_update(s, p);
}

/* ---- The VBOB endpoint ---- */

/*
 * "Genlock and lock reporting": the supported genlock input timings and the
 * output timings that lock to each (the owner's guide, Table 5-3), by wire
 * code. The table does not name 1080 29.97p or 30p, which have no genlock.
 */
static uint32_t dm2_genlock_outputs(uint8_t reference)
{
    switch (reference) {
    case 3:  /* 525 */
        return BIT(7) | BIT(5) | BIT(11) | BIT(13) | BIT(17);
    case 4:  /* 625 */
        return BIT(16) | BIT(15);
    case 5:  /* 720 59.94p */
    case 7:  /* 1080 59.94i */
    case 17: /* 1035 59.94i */
        return BIT(5) | BIT(17) | BIT(7) | BIT(11) | BIT(13);
    case 6:  /* 720 60p */
        return BIT(6) | BIT(12) | BIT(14) | BIT(8);
    case 8:  /* 1080 60i */
        return BIT(8) | BIT(6) | BIT(12) | BIT(14);
    case 11: /* 1080 23.98p */
    case 13: /* 1080 23.98 PsF */
        return BIT(11) | BIT(13);
    case 12: /* 1080 24p */
    case 14: /* 1080 24 PsF */
        return BIT(12) | BIT(14);
    case 15: /* 1080 25p */
    case 16: /* 1080 50i */
        return BIT(15) | BIT(16);
    default:
        return 0;
    }
}

/*
 * The output timing a side reports: the property override when it names
 * this side's standard, else the code of the side's latest record.
 */
static uint8_t dm2_side_output(DM2State *s, unsigned side)
{
    DM2Vbob *v = &s->vbob;

    if (v->output_override && dm2_code_side(v->output_override) == side) {
        return v->output_override;
    }
    return v->have_record[side] ? dm2_record_code(v->record[side]) : 0;
}

/*
 * The input slot that carries a side's reference, by wire source ("Replies
 * and status", the input-signal table): HD output jack A byte 4, jack B
 * byte 5, analog byte 8; SD output byte 6, byte 7, byte 9.
 */
static unsigned dm2_reference_slot(unsigned side, unsigned source)
{
    static const uint8_t slots[SIDES][3] = { { 0, 1, 4 }, { 2, 3, 5 } };

    return slots[side][source - 1];
}

/*
 * Lock state and detected timing of one side. Internal genlock, and an
 * unknown source value, is locked with the side's output timing. An
 * external source is locked when its slot holds a reference that Table 5-3
 * (HD) or the same standard (SD) pairs with the output timing; otherwise it
 * is unlocked and the detected timing is none.
 */
static bool dm2_side_lock(DM2State *s, unsigned side, uint8_t *code)
{
    DM2Vbob *v = &s->vbob;
    uint8_t out = dm2_side_output(s, side);
    unsigned source = v->source[side];
    unsigned slot;
    uint8_t ref;

    if (source < 1 || source > 3) {
        *code = out;
        return true;
    }
    slot = dm2_reference_slot(side, source);
    ref = v->input_timings[slot];
    *code = TIMING_NONE;
    if (!(v->present & BIT(slot)) || !out) {
        return false;
    }
    if (side == SIDE_SD ? ref != out :
        !(dm2_genlock_outputs(ref) & BIT(out))) {
        return false;
    }
    *code = ref;
    return true;
}

/* The detected input timing of the HD and SD input slots. */
static void dm2_input_codes(DM2State *s, uint8_t codes[2])
{
    codes[0] = s->vbob.present & BIT(INPUT_SLOT_HD) ?
               s->vbob.input_timings[INPUT_SLOT_HD] : TIMING_NONE;
    codes[1] = s->vbob.present & BIT(INPUT_SLOT_SD) ?
               s->vbob.input_timings[INPUT_SLOT_SD] : TIMING_NONE;
}

/* Put the frame in the receive buffer and tell the frame's pipe. */
static void dm2_rx_deposit(DM2State *s, const DM2Frame *f)
{
    DM2Vbob *v = &s->vbob;
    unsigned pipe = f->bytes[1] ? PIPE_IN : PIPE_OUT;

    /*
     * A frame the host can never be told about must not occupy the buffer,
     * or no later frame would be deposited: drop it with its entry.
     */
    if (!dm2_ring_ready(s, pipe)) {
        qemu_log_mask(LOG_UNIMP, TYPE_SGI_XT_DIGVID ": VBOB frame for pipe "
                      "%u dropped: no status ring to report it in\n", pipe);
        return;
    }
    for (unsigned k = 0; k < VBOB_WORDS; k++) {
        s->gif[GIF_IDX(CLINK_RX_FIRST + 8 * k)] =
            (f->bytes[2 * k] << 8) | f->bytes[2 * k + 1];
    }
    v->rx_busy = true;
    dm2_ring_post(s, pipe, RING_FLAG_LINK_RX);
}

static void dm2_rx_send(DM2State *s, const DM2Frame *f)
{
    DM2Vbob *v = &s->vbob;

    if (!v->rx_busy) {
        dm2_rx_deposit(s, f);
        return;
    }
    if (v->rxq_count == VBOB_RXQ) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": VBOB frame "
                      "queue full; frame dropped\n");
        return;
    }
    v->rxq[(v->rxq_head + v->rxq_count++) % VBOB_RXQ] = *f;
}

/*
 * Deposit queued frames until one posts, or none is left: a frame whose
 * ring cannot take its entry is dropped and the next is tried.
 */
static void dm2_rx_next(DM2State *s)
{
    DM2Vbob *v = &s->vbob;

    while (!v->rx_busy && v->rxq_count) {
        DM2Frame f = v->rxq[v->rxq_head];

        v->rxq_head = (v->rxq_head + 1) % VBOB_RXQ;
        v->rxq_count--;
        dm2_rx_deposit(s, &f);
    }
}

static void dm2_rx_release(DM2State *s)
{
    s->vbob.rx_busy = false;
    dm2_rx_next(s);
}

/*
 * Report a change in a side's lock state with an 0xa6 frame on pipe 0's
 * ring, and in the input signal with an 0xa7 frame and status flags on
 * pipe 1's ("Genlock and lock reporting").
 */
static void dm2_vbob_notify(DM2State *s)
{
    DM2Vbob *v = &s->vbob;
    uint8_t codes[2];
    bool present = dm2_input_present(s);
    DM2Frame f = { { 0 } };

    for (unsigned side = 0; side < SIDES; side++) {
        uint8_t code;
        bool locked = dm2_side_lock(s, side, &code);

        if (locked == v->reported_locked[side]) {
            continue;
        }
        v->reported_locked[side] = locked;
        memset(&f, 0, sizeof(f));
        f.bytes[0] = 5;
        f.bytes[1] = PIPE_OUT;
        f.bytes[2] = MSG_NOTIFY_SYNC;
        f.bytes[3] = 0x80 | (locked ? (side == SIDE_SD ? 1 : 2) : 0);
        f.bytes[side == SIDE_SD ? 5 : 4] = code;
        dm2_rx_send(s, &f);
    }
    dm2_input_codes(s, codes);
    if (memcmp(codes, v->reported_input, sizeof(codes)) ||
        present != v->reported_present) {
        memcpy(v->reported_input, codes, sizeof(codes));
        v->reported_present = present;
        memset(&f, 0, sizeof(f));
        f.bytes[0] = 7;
        f.bytes[1] = PIPE_IN;
        f.bytes[2] = MSG_NOTIFY_INPUT;
        f.bytes[3] = 0x80;
        f.bytes[4] = codes[0];
        f.bytes[6] = codes[1];
        dm2_rx_send(s, &f);
        dm2_ring_post(s, PIPE_IN, present ? RING_FLAG_INPUT_CHANGE :
                      RING_FLAG_INPUT_CHANGE | RING_FLAG_INPUT_OFF);
    }
    dm2_pipe_update(s, PIPE_IN);
}

static void dm2_vbob_reset_state(DM2Vbob *v)
{
    v->tv_standard = 0;
    memset(v->source, 0, sizeof(v->source));
    memset(v->record, 0, sizeof(v->record));
    memset(v->have_record, 0, sizeof(v->have_record));
    v->active_side = SIDE_HD;
    memset(v->routing, 0, sizeof(v->routing));
    v->sync_enable = 0;
    v->input_enable = 0;
    v->misc = 0;
}

/* Take the state as already reported, as at reset. */
static void dm2_vbob_sync_reported(DM2State *s)
{
    DM2Vbob *v = &s->vbob;
    uint8_t code;

    for (unsigned side = 0; side < SIDES; side++) {
        v->reported_locked[side] = dm2_side_lock(s, side, &code);
    }
    dm2_input_codes(s, v->reported_input);
    v->reported_present = dm2_input_present(s);
}

/* Answer a message: the get replies and the acknowledgement. */
static void dm2_vbob_message(DM2State *s, const uint8_t *m)
{
    DM2Vbob *v = &s->vbob;
    unsigned reg = m[2] & 0x7f;
    bool write = m[2] & 0x80;
    unsigned pipe = m[1];
    DM2Frame reply = { { 0 } };
    bool replied = false;
    reply.bytes[1] = m[1];
    reply.bytes[2] = reg;
    switch (reg) {
    case MSG_TV_STANDARD:
        if (write) {
            v->tv_standard = m[4];
        }
        break;
    case MSG_GENLOCK_SOURCE:
        /* One nibble names the side; a zero nibble leaves the other. */
        if (write) {
            if ((m[3] >> 4) & 8) {
                v->source[SIDE_HD] = (m[3] >> 4) & 7;
            }
            if (m[3] & 8) {
                v->source[SIDE_SD] = m[3] & 7;
            }
        }
        break;
    case MSG_TIMING:
        if (write) {
            unsigned side = m[3] == 6 ? SIDE_SD : SIDE_HD;

            memcpy(v->record[side], m, sizeof(v->record[side]));
            v->have_record[side] = true;
            v->active_side = side;
        }
        break;
    case MSG_ROUTE_IN:
    case MSG_ROUTE_OUT: {
        unsigned which = reg == MSG_ROUTE_IN ? 1 : 0;

        if (m[3] >= VBOB_PORTS) {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": VBOB "
                          "route destination port %u is not a port\n", m[3]);
        } else if (write) {
            v->routing[which][m[3]] = m[4];
        } else if (reg == MSG_ROUTE_IN) {
            qemu_log_mask(LOG_UNIMP, TYPE_SGI_XT_DIGVID ": VBOB message "
                          "register 0x%02x (read) is not modelled\n", reg);
        } else {
            reply.bytes[0] = 4;
            reply.bytes[4] = v->routing[which][m[3]];
            replied = true;
        }
        break;
    }
    case MSG_SYNC_ENABLE:
        if (write) {
            v->sync_enable = m[3] & 0x80;
        } else {
            /* A reply has byte 3 bit 7 clear; a notification has it set. */
            uint8_t hd_code, sd_code;
            bool hd = dm2_side_lock(s, SIDE_HD, &hd_code);
            bool sd = dm2_side_lock(s, SIDE_SD, &sd_code);

            /*
             * Each detector reports its own timing; the lock bits are the
             * latest record's side's, as the page chooses.
             */
            reply.bytes[0] = 5;
            reply.bytes[2] = MSG_NOTIFY_SYNC;
            reply.bytes[3] = (v->active_side == SIDE_SD ? sd : hd) ? 3 : 0;
            reply.bytes[4] = hd_code;
            reply.bytes[5] = sd_code;
            replied = true;
        }
        break;
    case MSG_INPUT_ENABLE:
        if (write) {
            v->input_enable = m[3] & 0x80;
        } else {
            reply.bytes[0] = 9;
            reply.bytes[2] = MSG_NOTIFY_INPUT;
            for (unsigned i = 0; i < VBOB_INPUT_SLOTS; i++) {
                reply.bytes[4 + i] = v->present & BIT(i) ?
                                     v->input_timings[i] : TIMING_NONE;
            }
            replied = true;
        }
        break;
    case MSG_MISC:
        if (write) {
            v->misc = m[3];
        }
        break;
    case MSG_INFO:
        if (!write) {
            reply.bytes[0] = 1;
            replied = true;
        }
        break;
    case MSG_RESET:
        if (write && !memcmp(m + 3, "RESET", 5)) {
            dm2_vbob_reset_state(v);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, TYPE_SGI_XT_DIGVID ": VBOB message "
                      "register 0x%02x (%s) is not modelled\n", reg,
                      write ? "write" : "read");
        break;
    }
    /* Set messages are acknowledged with link transmit done only. */
    dm2_ring_post(s, pipe, RING_FLAG_LINK_TX);
    if (replied) {
        dm2_rx_send(s, &reply);
    }
    dm2_vbob_notify(s);
}

static void dm2_doorbell(DM2State *s)
{
    uint8_t m[VBOB_MSG_BYTES];

    for (unsigned k = 0; k < VBOB_WORDS; k++) {
        uint32_t w = s->gif[GIF_IDX(CLINK_TX_FIRST + 8 * k)];

        m[2 * k] = w >> 8;
        m[2 * k + 1] = w;
    }
    /* Byte 1 is the pipe, and the driver refuses a pipe above 1. */
    if (m[1] > PIPE_IN) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": VBOB message "
                      "names pipe %u; not acknowledged\n", m[1]);
        return;
    }
    dm2_vbob_message(s, m);
}

/* ---- FPGA programming ---- */

static uint32_t dm2_fpga_read(DM2State *s)
{
    uint32_t word = 0;

    for (unsigned i = 0; i < FPGA_COUNT; i++) {
        if (s->fpga[i].done) {
            word |= BIT(fpga_info[i].done_bit);
        }
        if (s->fpga[i].init) {
            word |= BIT(fpga_info[i].done_bit + 1);
        }
    }
    return word;
}

static void dm2_fpga_reset_pulse(DM2State *s, unsigned i)
{
    s->fpga[i].done = false;
    s->fpga[i].init = true;
    s->fpga[i].loading = i != FPGA_GIF;
    s->fpga[i].count = 0;
    if (i == FPGA_GIF) {
        for (unsigned p = 0; p < PIPE_COUNT; p++) {
            dm2_pipe_update(s, p);
        }
    }
}

/* "GIF load": clocks are counted from the strobe. */
static void dm2_fpga_write(DM2State *s, uint32_t value)
{
    uint32_t old = s->fpga_ctl;
    DM2Fpga *gif = &s->fpga[FPGA_GIF];

    s->fpga_ctl = value & 0x3ff;
    if ((value & FPGA_CLOCK) && !(value & FPGA_STROBE)) {
        dm2_fpga_reset_pulse(s, FPGA_GIF);
    } else if ((value & (FPGA_CLOCK | FPGA_STROBE)) ==
               (FPGA_CLOCK | FPGA_STROBE) && !(old & FPGA_CLOCK)) {
        if (gif->count < UINT32_MAX) {
            gif->count++;
        }
        if (gif->count >= GIF_LOAD_CLOCKS && !gif->done) {
            gif->done = true;
            for (unsigned p = 0; p < PIPE_COUNT; p++) {
                dm2_pipe_update(s, p);
            }
        }
    }
}

/* One configuration byte for the FPGA owning block @blk. */
static void dm2_fpga_byte(DM2State *s, unsigned blk)
{
    DM2Fpga *f = &s->fpga[blk];

    if (!f->loading) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": configuration "
                      "byte for the %s FPGA outside a load; dropped\n",
                      fpga_info[blk].name);
        return;
    }
    if (++f->count >= fpga_info[blk].bytes) {
        f->done = true;
        f->loading = false;
    }
}

/* ---- Register files ---- */

static void dm2_unimp(const char *what, hwaddr addr, bool write)
{
    qemu_log_mask(LOG_UNIMP, TYPE_SGI_XT_DIGVID ": %s of %s offset 0x%"
                  HWADDR_PRIx " is not modelled\n", write ? "write" : "read",
                  what, addr);
}

static uint32_t dm2_xg_read(DM2State *s, hwaddr addr, bool *known)
{
    switch (addr) {
    case XG_ID:
        return DM2_ID;
    case XG_STATUS:
        return 0;
    case XG_ERR_ADDR_UPPER:
        return s->misc[0];
    case XG_ERR_ADDR_LOWER:
        return s->misc[1];
    case XG_CONTROL:
        return s->control;
    case XG_REQ_TIMEOUT:
        return s->misc[2];
    case XG_INT_DEST_UPPER:
        return s->int_dest_upper;
    case XG_INT_DEST_LOWER:
        return s->int_dest_lower;
    case XG_ERR_CMD:
        return s->misc[3];
    case XG_LLP_CONFIG:
        return s->misc[4];
    case XG_FLUSH:
        return s->misc[5];
    case XG_VECTORS:
        return s->vectors;
    case XG_CONFIG:
        return XG_CONFIG_VALUE;
    case XG_ERROR_CAUSE:
        return s->error_cause;
    case XG_WATER_MARKS:
        return s->water_marks;
    case XG_MAP_ERR_ADDR:
        return s->map_err_addr;
    case XG_NIC:
        return s->nic_control;
    default:
        *known = false;
        return 0;
    }
}

static void dm2_nic_write(DM2State *s, uint32_t value)
{
    uint32_t pulse = extract32(value, NIC_PULSE_SHIFT, 10);
    uint32_t sample = extract32(value, NIC_SAMPLE_SHIFT, 8);
    bool level = sgi_onewire_slot(SGI_XIO_BOARD(s)->onewire_bus, pulse,
                                  sample);

    s->nic_control = (value & NIC_WRITABLE) | NIC_DONE |
                     (level ? NIC_LEVEL : 0);
}

static bool dm2_xg_write(DM2State *s, hwaddr addr, uint32_t value)
{
    switch (addr) {
    case XG_ID:
    case XG_STATUS:
    case XG_CONFIG:
    case XG_MAP_ERR_ADDR:
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": write of 0x%x "
                      "to read-only register 0x%" HWADDR_PRIx "\n", value,
                      addr);
        break;
    case XG_ERR_ADDR_UPPER:
        s->misc[0] = value;
        break;
    case XG_ERR_ADDR_LOWER:
        s->misc[1] = value;
        break;
    case XG_CONTROL:
        s->control = value;
        break;
    case XG_REQ_TIMEOUT:
        s->misc[2] = value;
        break;
    case XG_INT_DEST_UPPER:
        s->int_dest_upper = value;
        break;
    case XG_INT_DEST_LOWER:
        s->int_dest_lower = value;
        break;
    case XG_ERR_CMD:
        s->misc[3] = value;
        break;
    case XG_LLP_CONFIG:
        s->misc[4] = value;
        break;
    case XG_FLUSH:
        s->misc[5] = value;
        break;
    case XG_VECTORS:
        s->vectors = value;
        break;
    case XG_ERROR_CAUSE:
        /* Write one to clear; a store of zero is the module's delay. */
        s->error_cause &= ~value;
        break;
    case XG_WATER_MARKS:
        s->water_marks = value & XG_WATER_MASK;
        break;
    case XG_NIC:
        dm2_nic_write(s, value);
        break;
    default:
        return false;
    }
    return true;
}

static uint32_t dm2_gif_read(DM2State *s, uint32_t addr)
{
    unsigned blk = (addr - GIF_BASE) / FPGA_PORT_STRIDE;
    const GifReg *r;

    /* A kind 0 or 2 FPGA being loaded answers READY at its port. */
    if (blk && ((addr - GIF_BASE) % FPGA_PORT_STRIDE) == 4 &&
        s->fpga[blk].loading) {
        return FPGA_READY;
    }
    r = gif_find(addr);
    if (!r) {
        dm2_unimp("the GIF register file", addr, false);
        return 0;
    }
    switch (r->kind) {
    case GR_ID:
        return GIF_ID_VALUE;
    case GR_CTL_OUT:
        return dm2_ctl_read(s, PIPE_OUT);
    case GR_CTL_IN:
        return dm2_ctl_read(s, PIPE_IN);
    case GR_RING_OUT:
    case GR_RING_IN:
    case GR_RESET_OUT:
    case GR_RESET_IN:
    case GR_LUT_DATA:
    case GR_DOORBELL:
    case GR_RX_RELEASE:
        return 0;
    default:
        return s->gif[GIF_IDX(addr)];
    }
}

static void dm2_gif_write(DM2State *s, uint32_t addr, uint32_t value)
{
    unsigned blk = (addr - GIF_BASE) / FPGA_PORT_STRIDE;
    const GifReg *r;
    unsigned i;

    /* The colour converters take their configuration bytes here. */
    if (blk && ((addr - GIF_BASE) % FPGA_PORT_STRIDE) == 4 &&
        fpga_info[blk].kind == 2 && s->fpga[blk].loading) {
        dm2_fpga_byte(s, blk);
        return;
    }
    r = gif_find(addr);
    if (!r) {
        dm2_unimp("the GIF register file", addr, true);
        return;
    }
    switch (r->kind) {
    case GR_ID:
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": write of 0x%x "
                      "to the read-only GIF identification\n", value);
        break;
    case GR_SELECT:
        s->gif[GIF_IDX(addr)] = value & r->mask;
        /* The strobe's low period resets the FPGA the select names. */
        if (!(value & FPGA_SELECT_STROBE)) {
            for (i = 1; i < FPGA_COUNT; i++) {
                if ((value & 0x7f) == fpga_info[i].select) {
                    dm2_fpga_reset_pulse(s, i);
                }
            }
        }
        break;
    case GR_CTL_OUT:
        dm2_ctl_write(s, PIPE_OUT, value);
        break;
    case GR_CTL_IN:
        dm2_ctl_write(s, PIPE_IN, value);
        break;
    case GR_RING_OUT:
    case GR_RING_IN:
        if (value) {
            dm2_unimp("a status ring pointer reset with a non-zero value",
                      addr, true);
            break;
        }
        s->pipe[r->kind == GR_RING_IN].ring_head = 0;
        break;
    case GR_RESET_OUT:
    case GR_RESET_IN:
        if (value) {
            dm2_unimp("a pipe reset with a non-zero value", addr, true);
            break;
        }
        s->pipe[r->kind == GR_RESET_IN].stream_counter = 0;
        s->pipe[r->kind == GR_RESET_IN].field_ints = 0;
        break;
    case GR_ENABLE_OUT:
    case GR_ENABLE_IN:
        s->gif[GIF_IDX(addr)] = value & r->mask;
        dm2_pipe_update(s, r->kind == GR_ENABLE_IN);
        break;
    case GR_LUT_DATA:
        /* "Lookup tables": the writes are accepted and dropped. */
        break;
    case GR_RX:
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": write of 0x%x "
                      "to a read-only VBOB receive word\n", value);
        break;
    case GR_DOORBELL:
        if (value == 1) {
            dm2_doorbell(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": VBOB "
                          "doorbell written 0x%x, not 1\n", value);
        }
        break;
    case GR_RX_RELEASE:
        if (value == 1) {
            dm2_rx_release(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_XT_DIGVID ": VBOB "
                          "receive release written 0x%x, not 1\n", value);
        }
        break;
    default:
        s->gif[GIF_IDX(addr)] = value & r->mask;
        break;
    }
}

static uint64_t dm2_read(void *opaque, hwaddr addr, unsigned size)
{
    DM2State *s = opaque;
    bool known = true;
    uint32_t value = 0;

    if (addr < XG_SIZE) {
        value = dm2_xg_read(s, addr, &known);
        if (!known) {
            dm2_unimp("the widget block", addr, false);
        }
    } else if (addr >= MAP_BASE && addr < MAP_BASE + MAP_SIZE) {
        value = s->map[(addr - MAP_BASE) / 4];
    } else if (addr == FPGA_REG) {
        value = dm2_fpga_read(s);
    } else if (addr >= GIF_BASE && addr < GIF_BASE + GIF_SIZE &&
               s->fpga[FPGA_GIF].done) {
        value = dm2_gif_read(s, addr);
    } else {
        dm2_unimp("the window", addr, false);
    }
    return value;
}

static void dm2_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    DM2State *s = opaque;
    uint32_t value = data;

    if (addr < XG_SIZE) {
        if (!dm2_xg_write(s, addr, value)) {
            dm2_unimp("the widget block", addr, true);
        }
    } else if (addr >= MAP_BASE && addr < MAP_BASE + MAP_SIZE) {
        s->map[(addr - MAP_BASE) / 4] = value;
    } else if (addr == FPGA_REG) {
        dm2_fpga_write(s, value);
    } else if (addr >= GIF_BASE && addr < GIF_BASE + GIF_SIZE &&
               s->fpga[FPGA_GIF].done) {
        dm2_gif_write(s, addr, value);
    } else if (addr >= PORT_BASE && addr < PORT_BASE + GIF_SIZE &&
               s->fpga[FPGA_GIF].done) {
        unsigned blk = (addr - PORT_BASE) / FPGA_PORT_STRIDE;

        if (blk && ((addr - PORT_BASE) % FPGA_PORT_STRIDE) == 4) {
            dm2_fpga_byte(s, blk);
        } else {
            dm2_unimp("the FPGA data ports", addr, true);
        }
    } else {
        dm2_unimp("the window", addr, true);
    }
}

/*
 * Every access software makes is a 32-bit word ("Window map"), and the
 * board refuses other widths. memory_region_access_valid() rejects a wider
 * access from a processor, so a 64-bit load or store faults. Only the
 * physical-memory path and qtest's plain accesses split one into words.
 */
static const MemoryRegionOps dm2_ops = {
    .read = dm2_read,
    .write = dm2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- Lifecycle ---- */

static const SGIBridgeBoardRecord dm2_record = {
    .barcode = "EMUO00",
    .part = DM2_BOARD_PART,
    .revision = DM2_BOARD_REVISION,
    .name = DM2_BOARD_NAME,
};

static const SGIXioBoardIdentity dm2_identity = {
    .family = 0x0b,
    .own = {
        .child_name = "onewire-nic",
        .record = &dm2_record,
        .serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('O', 0),
    },
};

/* A board in each slot is a different unit: name its marked serial. */
static void dm2_identity_record(SGIXioBoardState *board,
                                const SGIXioBoardIdentityPart *part,
                                SGIBridgeBoardRecord *record, uint64_t *serial)
{
    DM2State *s = SGI_XT_DIGVID(board);

    *serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('O', board->slot);
    sgi_synthetic_barcode(s->barcode, 'O', *serial);
    record->barcode = s->barcode;
}

/*
 * "Attach and reset": a widget reset, which the Crossbow's link reset
 * delivers, returns the XG block to its power-on values and invalidates the
 * map RAM; the FPGAs stay configured and the GIF file keeps its registers.
 */
static void dm2_widget_reset(void *opaque, int line, int level)
{
    DM2State *s = opaque;

    if (!level) {
        return;
    }
    s->control = 0;
    s->int_dest_upper = s->int_dest_lower = 0;
    s->vectors = 0;
    s->water_marks = 0;
    memset(s->misc, 0, sizeof(s->misc));
    s->error_cause = 0;
    s->map_err_addr = 0;
    memset(s->map, 0, sizeof(s->map));
}

static bool dm2_build_widget(SGIXioBoardState *board, Error **errp)
{
    DM2State *s = SGI_XT_DIGVID(board);
    QEMUTimerCB *field_cb[PIPE_COUNT] = { dm2_field_tick_out,
                                          dm2_field_tick_in };
    QEMUTimerCB *poll_cb[PIPE_COUNT] = { dm2_ring_poll_out,
                                         dm2_ring_poll_in };

    if (!s->fabric) {
        error_setg(errp, TYPE_SGI_XT_DIGVID " needs the fabric its "
                   "interrupts and DMA travel");
        return false;
    }
    memory_region_init_io(&s->regs, OBJECT(s), &dm2_ops, s,
                          TYPE_SGI_XT_DIGVID ".regs", DM2_WINDOW_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->regs);
    address_space_init(&s->fabric_as, s->fabric, TYPE_SGI_XT_DIGVID ".fabric");
    for (unsigned p = 0; p < PIPE_COUNT; p++) {
        s->pipe[p].field_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, field_cb[p],
                                              s);
        s->pipe[p].poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, poll_cb[p],
                                             s);
    }
    board->cpu_face = &s->regs;
    board->packet_face = &s->regs;
    board->link_reset = qdev_get_gpio_in_named(DEVICE(s), "link-reset", 0);
    return true;
}

static void dm2_unrealize(DeviceState *dev)
{
    DM2Class *dc = SGI_XT_DIGVID_GET_CLASS(dev);
    DM2State *s = SGI_XT_DIGVID(dev);

    dc->parent_unrealize(dev);
    for (unsigned p = 0; p < PIPE_COUNT; p++) {
        timer_free(s->pipe[p].field_timer);
        timer_free(s->pipe[p].poll_timer);
        s->pipe[p].field_timer = s->pipe[p].poll_timer = NULL;
    }
    address_space_destroy(&s->fabric_as);
}

/*
 * Reset zeroes every register, the map RAM, the FPGA state and the pipes,
 * the model's choice for every value the page does not state. The
 * endpoint's world (its properties) is configuration and stays.
 */
static void dm2_reset_hold(Object *obj, ResetType type)
{
    DM2Class *dc = SGI_XT_DIGVID_GET_CLASS(obj);
    DM2State *s = SGI_XT_DIGVID(obj);
    DM2Vbob saved = s->vbob;

    if (dc->parent_phases.hold) {
        dc->parent_phases.hold(obj, type);
    }
    s->control = 0;
    s->int_dest_upper = s->int_dest_lower = 0;
    s->vectors = 0;
    s->water_marks = 0;
    memset(s->misc, 0, sizeof(s->misc));
    s->error_cause = 0;
    s->map_err_addr = 0;
    s->nic_control = NIC_DONE;
    memset(s->map, 0, sizeof(s->map));
    s->fpga_ctl = 0;
    memset(s->fpga, 0, sizeof(s->fpga));
    memset(s->gif, 0, sizeof(s->gif));
    for (unsigned p = 0; p < PIPE_COUNT; p++) {
        DM2Pipe *pp = &s->pipe[p];
        QEMUTimer *field = pp->field_timer, *poll = pp->poll_timer;

        if (field) {
            timer_del(field);
            timer_del(poll);
        }
        memset(pp, 0, sizeof(*pp));
        pp->field_timer = field;
        pp->poll_timer = poll;
    }
    memset(&s->vbob, 0, sizeof(s->vbob));
    s->vbob.output_override = saved.output_override;
    memcpy(s->vbob.input_timings, saved.input_timings,
           sizeof(s->vbob.input_timings));
    s->vbob.present = saved.present;
    dm2_vbob_sync_reported(s);
}

/* ---- Properties of the virtual endpoint ---- */

static void dm2_prop_apply(DM2State *s)
{
    if (DEVICE(s)->realized) {
        dm2_vbob_notify(s);
    }
}

static void dm2_get_output_timing(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint8_t value = SGI_XT_DIGVID(obj)->vbob.output_override;

    visit_type_uint8(v, name, &value, errp);
}

static void dm2_set_output_timing(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    DM2State *s = SGI_XT_DIGVID(obj);
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value && (value < TIMING_FIRST || value > TIMING_LAST)) {
        error_setg(errp, "x-vbob-output-timing is 0 or a wire code from %u "
                   "to %u", TIMING_FIRST, TIMING_LAST);
        return;
    }
    s->vbob.output_override = value;
    dm2_prop_apply(s);
}

static void dm2_get_present(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    uint8_t value = SGI_XT_DIGVID(obj)->vbob.present;

    visit_type_uint8(v, name, &value, errp);
}

static void dm2_set_present(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    DM2State *s = SGI_XT_DIGVID(obj);
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    s->vbob.present = value & (BIT(VBOB_INPUT_SLOTS) - 1);
    dm2_prop_apply(s);
}

/* One byte per slot, slot 0 in the low byte. */
static void dm2_get_inputs(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    DM2State *s = SGI_XT_DIGVID(obj);
    uint64_t value = 0;

    for (unsigned i = 0; i < VBOB_INPUT_SLOTS; i++) {
        value |= (uint64_t)s->vbob.input_timings[i] << (8 * i);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void dm2_set_inputs(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    DM2State *s = SGI_XT_DIGVID(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value >> (8 * VBOB_INPUT_SLOTS)) {
        error_setg(errp, "x-vbob-input-timings holds %u bytes",
                   VBOB_INPUT_SLOTS);
        return;
    }
    for (unsigned i = 0; i < VBOB_INPUT_SLOTS; i++) {
        s->vbob.input_timings[i] = value >> (8 * i);
    }
    dm2_prop_apply(s);
}

/*
 * Host-only qtest: report the GIF configured without the 1.4 million clocks
 * of its load ("GIF load"), so a test of the GIF register file does not pay
 * for them. The load itself has its own test. It is a run-time qom-set
 * only: the reset at machine start clears the state it sets.
 */
static bool dm2_get_gif_loaded(Object *obj, Error **errp)
{
    return SGI_XT_DIGVID(obj)->fpga[FPGA_GIF].done;
}

static void dm2_set_gif_loaded(Object *obj, bool value, Error **errp)
{
    DM2State *s = SGI_XT_DIGVID(obj);
    DM2Fpga *gif = &s->fpga[FPGA_GIF];

    gif->done = value;
    gif->init = value;
    gif->loading = false;
    gif->count = value ? GIF_LOAD_CLOCKS : 0;
    for (unsigned p = 0; p < PIPE_COUNT; p++) {
        dm2_pipe_update(s, p);
    }
}

static void dm2_instance_init(Object *obj)
{
    DM2State *s = SGI_XT_DIGVID(obj);

    memset(s->vbob.input_timings, TIMING_NONE, sizeof(s->vbob.input_timings));
    qdev_init_gpio_in_named(DEVICE(obj), dm2_widget_reset, "link-reset", 1);
}

/* ---- Migration ---- */

static int dm2_post_load(void *opaque, int version_id)
{
    DM2State *s = opaque;

    for (unsigned p = 0; p < PIPE_COUNT; p++) {
        DM2Pipe *pp = &s->pipe[p];

        if (pp->queued > SLOTS || pp->ring_head >= RING_ENTRIES ||
            pp->armed > CTL_VALID_MASK) {
            return -EINVAL;
        }
        for (unsigned i = 0; i < pp->queued; i++) {
            if (pp->queue[i] >= SLOTS) {
                return -EINVAL;
            }
        }
    }
    if (s->vbob.rxq_count > VBOB_RXQ || s->vbob.rxq_head >= VBOB_RXQ ||
        s->vbob.active_side >= SIDES) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_dm2_fpga = {
    .name = TYPE_SGI_XT_DIGVID "/fpga",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(done, DM2Fpga),
        VMSTATE_BOOL(init, DM2Fpga),
        VMSTATE_BOOL(loading, DM2Fpga),
        VMSTATE_UINT32(count, DM2Fpga),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_dm2_pipe = {
    .name = TYPE_SGI_XT_DIGVID "/pipe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctl, DM2Pipe),
        VMSTATE_BOOL(ie, DM2Pipe),
        VMSTATE_UINT8(armed, DM2Pipe),
        VMSTATE_UINT8_ARRAY(queue, DM2Pipe, SLOTS),
        VMSTATE_UINT8(queued, DM2Pipe),
        VMSTATE_UINT32(stream_counter, DM2Pipe),
        VMSTATE_UINT32(field_ints, DM2Pipe),
        VMSTATE_UINT8(ring_head, DM2Pipe),
        VMSTATE_BOOL(pending, DM2Pipe),
        VMSTATE_UINT32(pending_flags, DM2Pipe),
        VMSTATE_TIMER_PTR(field_timer, DM2Pipe),
        VMSTATE_TIMER_PTR(poll_timer, DM2Pipe),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_dm2_frame = {
    .name = TYPE_SGI_XT_DIGVID "/frame",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(bytes, DM2Frame, VBOB_MSG_BYTES),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_dm2_vbob = {
    .name = TYPE_SGI_XT_DIGVID "/vbob",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(output_override, DM2Vbob),
        VMSTATE_UINT8_ARRAY(input_timings, DM2Vbob, VBOB_INPUT_SLOTS),
        VMSTATE_UINT8(present, DM2Vbob),
        VMSTATE_UINT8(tv_standard, DM2Vbob),
        VMSTATE_UINT8_ARRAY(source, DM2Vbob, SIDES),
        VMSTATE_UINT8_2DARRAY(record, DM2Vbob, SIDES, VBOB_MSG_BYTES),
        VMSTATE_BOOL_ARRAY(have_record, DM2Vbob, SIDES),
        VMSTATE_UINT8(active_side, DM2Vbob),
        VMSTATE_UINT8_2DARRAY(routing, DM2Vbob, 2, VBOB_PORTS),
        VMSTATE_UINT8(sync_enable, DM2Vbob),
        VMSTATE_UINT8(input_enable, DM2Vbob),
        VMSTATE_UINT8(misc, DM2Vbob),
        VMSTATE_BOOL_ARRAY(reported_locked, DM2Vbob, SIDES),
        VMSTATE_UINT8_ARRAY(reported_input, DM2Vbob, 2),
        VMSTATE_BOOL(reported_present, DM2Vbob),
        VMSTATE_BOOL(rx_busy, DM2Vbob),
        VMSTATE_STRUCT_ARRAY(rxq, DM2Vbob, VBOB_RXQ, 1, vmstate_dm2_frame,
                             DM2Frame),
        VMSTATE_UINT8(rxq_head, DM2Vbob),
        VMSTATE_UINT8(rxq_count, DM2Vbob),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_dm2 = {
    .name = TYPE_SGI_XT_DIGVID,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dm2_post_load,
    .fields = (const VMStateField[]) {
        /* The slot, which the destination board's must match. */
        VMSTATE_STRUCT(parent_obj, DM2State, 0, vmstate_sgi_xio_board_slot,
                       SGIXioBoardState),
        VMSTATE_UINT32(control, DM2State),
        VMSTATE_UINT32(int_dest_upper, DM2State),
        VMSTATE_UINT32(int_dest_lower, DM2State),
        VMSTATE_UINT32(vectors, DM2State),
        VMSTATE_UINT32(water_marks, DM2State),
        VMSTATE_UINT32_ARRAY(misc, DM2State, 6),
        VMSTATE_UINT32(error_cause, DM2State),
        VMSTATE_UINT32(map_err_addr, DM2State),
        VMSTATE_UINT32(nic_control, DM2State),
        VMSTATE_UINT32_ARRAY(map, DM2State, MAP_ENTRIES),
        VMSTATE_UINT32(fpga_ctl, DM2State),
        VMSTATE_STRUCT_ARRAY(fpga, DM2State, FPGA_COUNT, 1, vmstate_dm2_fpga,
                             DM2Fpga),
        VMSTATE_UINT32_ARRAY(gif, DM2State, GIF_SLOTS),
        VMSTATE_STRUCT_ARRAY(pipe, DM2State, PIPE_COUNT, 1, vmstate_dm2_pipe,
                             DM2Pipe),
        VMSTATE_STRUCT(vbob, DM2State, 1, vmstate_dm2_vbob, DM2Vbob),
        VMSTATE_END_OF_LIST()
    },
};

static const Property dm2_properties[] = {
    DEFINE_PROP_LINK("fabric", DM2State, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void dm2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    DM2Class *dmc = SGI_XT_DIGVID_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    gif_lookup_init();
    bc->build_widget = dm2_build_widget;
    bc->identity_record = dm2_identity_record;
    sgi_xio_board_class_set_identity(oc, &dm2_identity);
    device_class_set_parent_unrealize(dc, dm2_unrealize,
                                      &dmc->parent_unrealize);
    resettable_class_set_parent_phases(rc, NULL, dm2_reset_hold, NULL,
                                       &dmc->parent_phases);
    dc->vmsd = &vmstate_dm2;
    dc->desc = "SGI DMediaPro DM2 (XT-DIGVID) digital video XIO board";
    /* Fitted to an empty XIO slot it names (slot=widget-10). */
    dc->user_creatable = true;
    device_class_set_props(dc, dm2_properties);
    object_class_property_add_bool(oc, "x-test-gif-loaded", dm2_get_gif_loaded,
                                   dm2_set_gif_loaded);
    object_class_property_set_description(oc, "x-test-gif-loaded",
        "Host-only qtest: report the GIF configured without its load; set "
        "at run time, since machine reset clears it");
    object_class_property_add(oc, "x-vbob-output-timing", "uint8",
                              dm2_get_output_timing, dm2_set_output_timing,
                              NULL, NULL);
    object_class_property_set_description(oc, "x-vbob-output-timing",
        "Wire code (3 to 17) that overrides the output timing the virtual "
        "VBOB takes from the host's timing record; 0 for no override");
    object_class_property_add(oc, "x-vbob-input-timings", "uint64",
                              dm2_get_inputs, dm2_set_inputs, NULL, NULL);
    object_class_property_set_description(oc, "x-vbob-input-timings",
        "Detected input timing wire code of each of the six jack slots, "
        "slot 0 in the low byte; 1 is none");
    object_class_property_add(oc, "x-vbob-present", "uint8", dm2_get_present,
                              dm2_set_present, NULL, NULL);
    object_class_property_set_description(oc, "x-vbob-present",
        "A signal is present on input slot n of the six, bit n; slots 0 and "
        "2 are the HD and SD inputs");
}

static const TypeInfo dm2_types[] = {
    {
        .name = TYPE_SGI_XT_DIGVID,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(DM2State),
        .instance_init = dm2_instance_init,
        .class_size = sizeof(DM2Class),
        .class_init = dm2_class_init,
    },
};

DEFINE_TYPES(dm2_types)
