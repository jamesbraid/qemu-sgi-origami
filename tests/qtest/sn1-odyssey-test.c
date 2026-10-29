/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Odyssey (VPro) graphics board in the Fuel's graphics slot, the VPro
 * target the SN1 tests share. The register contract under test is the
 * board's, not the machine's.
 *
 * Each test pins one step of the ladder the stock IP35 firmware and the
 * IRIX IP35 graphics driver walk before they will claim the board, plus
 * the rule that everything past that ladder stays visibly unmodeled.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-raster-bands.h"
#include "sgi-test.h"
#include "sgi-mips-emit.h"
#include "sgi-migrate.h"

/*
 * The board's register contract: its own property defaults, which -global
 * changes here, rather than the catalogue's V12, which a launch takes.
 */
#define MACHINE "-M fuel,x-uncatalogued-graphics=on " \
                "-cpu R14000 -smp 1 -m 512M -S " \
                "-device sgi-odyssey,slot=widget-13"

/*
 * The graphics slot's crosstalk position, repeated from the machine's own
 * selection rather than borrowed from it: XBridge widget 13, where a Fuel
 * IRIX inventory and an OpenBSD capture both report the board.
 */
#define SLOT 13
#define WIDGET (SPACE_IO + (uint64_t)SLOT * UINT64_C(0x1000000))
#define BOARD "/machine/module0/graphics-slot"

#define ODSY_IDENTITY 0x0004
#define ODSY_STATUS 0x000c
#define ODSY_CONTROL 0x0024
#define ODSY_XT_REQ_TIMEOUT 0x002c
#define ODSY_INT_DEST_UPPER 0x0034
#define ODSY_INT_DEST_LOWER 0x003c
#define ODSY_ERROR_COMMAND 0x0044
#define ODSY_RESET_CONTROL 0x1004
#define ODSY_INTR_FLAGS 0x1014
#define ODSY_INTR_CLEAR 0x101c
#define ODSY_INTR_ENABLE_SET 0x1034
#define ODSY_INTR_ENABLE_CLEAR 0x103c
#define ODSY_INTR_FIFO_LOW_WATER 0x00000200
/*
 * The mask the kernel driver's interrupt setup wrote, from the probe's
 * trace.
 */
#define ODSY_INTR_INIT_MASK 0x79e4259e
/*
 * The DMA-done flag, which the kernel driver's DMA submission clears
 * before a transfer.
 */
#define ODSY_INTR_DMA_DONE 0x00000800
/*
 * The routed-interrupt destination and per-source vector table, redeclared
 * here the same way ODSY_INT_DEST_UPPER/_LOWER above already is. Distinct
 * registers from those, used by the softsync, retrace and DMA-done sources
 * below.
 */
#define ODSY_ROUTE_DEST_UPPER 0x1044
#define ODSY_ROUTE_DEST_LOWER 0x104c
#define ODSY_ROUTE_VECTORS 0x1054
#define ODSY_ROUTE_VECTOR_SOFTSYNC_SHIFT 24
#define ODSY_ROUTE_VECTOR_RETRACE_SHIFT 16
#define ODSY_ROUTE_VECTOR_DMA_SHIFT 8
#define ODSY_STATUS0 0x1064
#define ODSY_MEMORY_CONFIG 0x106c
#define ODSY_FLCTL_CONFIG 0x1070
#define ODSY_SW_TOKEN 0x107c
#define ODSY_BUZZ_1 0x1078
#define ODSY_BUZZ_2 0x1088
#define ODSY_BUZZ_3 0x1098
#define ODSY_BUZZ_4 0x10a8
#define ODSY_BUZZ_5 0x10b8
#define ODSY_BUZZ_6 0x10c0
/*
 * The swap-control register bank (hw/misc/sgi-odyssey.c), redeclared here the
 * same way every register offset above already is. Each is a mask with one
 * bit per window: ODSY_SWAP_ARM the windows to flip at the next retrace,
 * ODSY_SWAP_CONTROL/_COMPANION the windows set to buffer 1/0 at once, and
 * ODSY_DISPLAYED_BUFFER the buffer each window shows, read back by the
 * driver's retrace ISR.
 */
#define ODSY_SWAP_ARM 0x10b4
#define ODSY_SWAP_PENDING 0x10bc
#define ODSY_SWAP_CONTROL 0x10c4
#define ODSY_SWAP_CONTROL_COMPANION 0x10cc
#define ODSY_DISPLAYED_BUFFER 0x10d4
/* The one-hot per-window arm bit (bit 24) the capture saw written. */
#define ODSY_SWAP_ARM_BIT 0x01000000
#define ODSY_NIC 0x10d8
/*
 * The DMA-sync command's (ODSY_CMD_HOST_WRITE_SELECT below) host-write
 * base, redeclared here the same way every register offset above already
 * is.
 */
#define ODSY_HOST_WRITE_ADDR_UPPER 0x4034
#define ODSY_HOST_WRITE_ADDR_LOWER 0x403c
#define ODSY_CMD_FIFO 0x110000

/*
 * The kernel driver's context switch save address and completion port,
 * redeclared here the same way every register offset above already is.
 * ODSY_CTX_SWITCH_WORD is the probe's boot-trace word: opcode 7 in bits
 * 47:40, the valid bit set, a new context id of 1 in bits 31:16.
 * ODSY_CTX_SWITCH_DONE_OFFSET is the driver's own poll offset into the
 * shadow page ODSY_CTX_SHADOW_ADDR_UPPER/_LOWER names.
 */
#define ODSY_CTX_SHADOW_ADDR_UPPER 0x4004
#define ODSY_CTX_SHADOW_ADDR_LOWER 0x400c
#define ODSY_CTX_SWITCH_PORT 0x118000
#define ODSY_CTX_SWITCH_DONE_OFFSET 0x8fc
#define ODSY_CTX_SWITCH_WORD UINT64_C(0x0000070000010001)

/*
 * The interrupt-packet destination's bit layout, redeclared here at the
 * qtest boundary the same way every register offset above already is
 * (hw/misc/sgi-odyssey.c's own ODSY_INT_DEST_* constants).
 */
#define ODSY_HUB_INT_MODIFY_SET 0x00000100
#define ODSY_INT_DEST_VECTOR_SHIFT 24

/*
 * This node's own Processor Interface PENDING_MODIFY/PENDING_0 pair, at the
 * CPU-relative offsets tests/qtest/sn1-address-map-test.c's own
 * BEDROCK_NODE0/PI_PENDING_MODIFY/PI_PENDING_0 constants reach the same
 * registers through (hw/misc/sgi-bedrock-pi.c).
 */
#define BEDROCK_NODE0 (SPACE_IO + UINT64_C(0x01800000))
#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_0 0x98

/*
 * The same node's memory-controller "installed banks" register, at the
 * CPU-relative offset tests/qtest/sn1-address-map-test.c's own
 * BEDROCK_MD_MEMORY_CONFIG constant reaches. Real firmware programs this
 * before touching RAM; a halted board_start() board never runs that
 * firmware, so a test that DMAs into guest RAM has to install a bank
 * itself first, or every address in it resolves as an unassigned hole
 * rather than memory. The Fuel fits 256 MB in each of locational banks 0
 * and 1 (size code 3, both enabled), which need no software-bank selection
 * to put RAM at physical 0.
 */
#define BEDROCK_MD_MEMORY_CONFIG UINT64_C(0x780008)
#define BEDROCK_MD_INSTALLED_BANKS UINT64_C(0x33)

/*
 * The crosstalk register-packet address for that same PENDING_MODIFY
 * register, in the format a widget's own outbound packet carries rather
 * than the CPU-relative format above: bit 47 selects register space, node
 * 0 in bits 46:39 is zero, and the low 39 bits are this node's own hub
 * window (0x1800000, II_XIO_HUB_WINDOW) with the register offset folded in
 * (hw/misc/sgi-bedrock-io.c's ii_inbound_register()). The canonical XIO
 * link carries a destination-prefixed packet space: widget 0xa names the
 * XBridge host port, with the complete address in its low 48 bits.
 */
#define BEDROCK_PACKET_PI_PENDING_MODIFY \
    ((UINT64_C(1) << 47) | UINT64_C(0x1800000) | PI_PENDING_MODIFY)

/* An arbitrary vector distinct from any bit these tests check elsewhere. */
#define ODSY_TEST_VECTOR 5
#define ODSY_TEST_DEST_WIDGET 0xa
#define ODSY_TEST_PACKET_FABRIC_ADDRESS \
    (((uint64_t)ODSY_TEST_DEST_WIDGET << 48) | \
     BEDROCK_PACKET_PI_PENDING_MODIFY)
#define ODSY_TEST_INT_DEST_UPPER \
    ((ODSY_TEST_VECTOR << ODSY_INT_DEST_VECTOR_SHIFT) | \
     (ODSY_TEST_DEST_WIDGET << 16) | \
     (uint32_t)(BEDROCK_PACKET_PI_PENDING_MODIFY >> 32))
#define ODSY_TEST_INT_DEST_LOWER \
    (uint32_t)BEDROCK_PACKET_PI_PENDING_MODIFY

/*
 * The same destination, programmed through the routed-interrupt table
 * instead: a distinct vector so a test can tell which path a packet came
 * through. Widget bits 28:25 select the board-linked fabric's host port;
 * bits 15:0 hold the address's high 16 bits.
 */
#define ODSY_TEST_SOFTSYNC_VECTOR 9
#define ODSY_TEST_ROUTE_DEST_UPPER \
    ((ODSY_TEST_DEST_WIDGET << 25) | \
     (uint32_t)(BEDROCK_PACKET_PI_PENDING_MODIFY >> 32))
#define ODSY_TEST_ROUTE_DEST_LOWER \
    (uint32_t)BEDROCK_PACKET_PI_PENDING_MODIFY
#define ODSY_TEST_RETRACE_VECTOR 7
#define ODSY_TEST_DMA_VECTOR 6
/* The flag source, low water among it, takes the low byte. */
#define ODSY_TEST_ROUTE_VECTORS \
    ((ODSY_TEST_SOFTSYNC_VECTOR << ODSY_ROUTE_VECTOR_SOFTSYNC_SHIFT) | \
     (ODSY_TEST_RETRACE_VECTOR << ODSY_ROUTE_VECTOR_RETRACE_SHIFT) | \
     (ODSY_TEST_DMA_VECTOR << ODSY_ROUTE_VECTOR_DMA_SHIFT) | \
     ODSY_TEST_VECTOR)

/*
 * The command-FIFO grammar's recognized words, redeclared here at the
 * qtest boundary the same way every register offset above already is.
 * hw/misc/sgi-odyssey.c describes each against OpenBSD's public odyssey(4)
 * driver.
 */
#define ODSY_CMD_GL_BEGIN 0x00014400
#define ODSY_CMD_GL_END 0x00014001
#define ODSY_CMD_GL_COLOR_3UB 0xc580cc08
#define ODSY_CMD_GL_VERTEX_2I 0x8080c800
#define ODSY_CMD_GL_QUADS 0x00000007
#define ODSY_CMD_QUICK_FLUSH_1 0x00010443
#define ODSY_CMD_QUICK_FLUSH_ARG 0x000000fa
#define ODSY_CMD_STROBE 0x00010046
/*
 * The real copy-rect sequence's register-select prefix and its one
 * operand, immediately ahead of the true trigger below -- not the trigger
 * itself (see ODSY_CMD_COPY_RECT_TRIGGER's own comment).
 */
#define ODSY_CMD_COPY_RECT_PREFIX 0x00010658
#define ODSY_CMD_COPY_RECT_PREFIX_ARG 0x00120000
#define ODSY_CMD_COPY_RECT_TRIGGER 0x00002031
/*
 * The one-bit pattern-quad text path, redeclared here the same way every
 * register-select above is. 0x3c4e loads pattern buffer words 0-15 and 0x3c4f
 * words 16-31 (a 16-word burst each, bit-field arity 15 -> 16); 0x043f
 * selects the shape (0x4 = 32x32) and 0x0404's bits 3 and 4 enable pattern
 * mode.
 */
#define ODSY_CMD_PATTERN_LOW 0x00013c4e
#define ODSY_CMD_PATTERN_HIGH 0x00013c4f
#define ODSY_CMD_PATTERN_SHAPE 0x0001043f
#define ODSY_CMD_PATTERN_MODE 0x00010404
#define ODSY_PATTERN_SHAPE_32X32 0x4
#define ODSY_PATTERN_MODE_ENABLE 0x18

/*
 * The BEGIN-0xa image raster path: a transfer-trigger state word, BEGIN with
 * primitive 0xa, the 0xcf80a92f box record's ten floats, a (0, 0) vertex, a
 * transfer header and its tail, then the pixel bursts. A burst word is
 * 0x00014000 | (words - words/16) << 10 | fmt (fmt 0x11 one RGB pixel per
 * word, 0x10 four colour indices per word); the header's bits 23:20 hold the
 * group's pixel count, 15 standing for 16.
 */
#define ODSY_CMD_IMAGE_STATE 0x2900812fu
#define ODSY_CMD_IMAGE_PRIMITIVE 0x0000000au
#define ODSY_CMD_IMAGE_BOX 0xcf80a92fu
#define ODSY_CMD_IMAGE_HEADER 0x00004570u
#define ODSY_CMD_IMAGE_HEADER_TAIL 0x00000071u
#define ODSY_IMAGE_FMT_CI 0x10u
#define ODSY_IMAGE_FMT_RGB 0x11u
#define ODSY_IMAGE_FMT_SHORT 0x12u
#define ODSY_IMAGE_BURST(words, fmt) \
    (0x00014000u | (uint32_t)(((words) - (words) / 16) << 10) | (fmt))
#define ODSY_IMAGE_HEADER_OP(group_len, fmt_low) \
    ((uint32_t)(((group_len) == 16 ? 15 : (group_len)) << 20) | (fmt_low))

/*
 * The host-write/sync command the kernel driver's DMA completion wait
 * uses: a register-select word for ODSY_REG_HOST_WRITE (0x0a5a), then two
 * operands, op0 = slot | ODSY_CMD_HOST_WRITE_INTERRUPT, op1 = a 16-bit
 * token.
 */
#define ODSY_CMD_HOST_WRITE_SELECT 0x00010a5a
#define ODSY_CMD_HOST_WRITE_INTERRUPT 0x8000

/*
 * The page-table pixel/texture DMA descriptor (hw/misc/sgi-odyssey.c). Two
 * 64-bit doublewords through the command FIFO: the opcode (0xe01 for an image
 * transfer in either direction, 0xe03 from the diagnostic region path only)
 * over the packed high half of the table's physical address, then the packed
 * low half over a zero word (dma_descriptor() below packs it). A
 * translation-table entry is (byte_address << 16) | length, 16 KiB pages, the
 * first entry possibly starting part-way into its page, terminated by a
 * zero-length entry, the whole thing past a 0x20-byte header.
 */
#define ODSY_CMD_DMA_IMAGE 0x00000e01u
#define ODSY_CMD_DMA_REGION 0x00000e03u
/*
 * The two buffer-select doublewords pushed ahead of a swap
 * (hw/misc/sgi-odyssey.c): each 8-byte store reaches the interpreter as an
 * opcode word (0x608/0x609) plus one operand word (0x2000 in the capture).
 * Recognized so it is consumed cleanly, not reported as unmodeled.
 */
#define ODSY_CMD_SWAP_SYNC_A 0x00000608u
#define ODSY_CMD_SWAP_SYNC_B 0x00000609u
#define ODSY_CMD_SWAP_SYNC_OPERAND 0x00002000u
#define ODSY_DMA_TABLE_HEADER 0x20
#define ODSY_DMA_PAGE_SIZE 0x4000
#define ODSY_DMA_PTE(addr, len) (((uint64_t)(addr) << 16) | (uint16_t)(len))
/*
 * The table header's control word (word 7, byte offset 0x1c): bits 18:16 the
 * bytes-per-pixel code (1 = 4 bytes, 5 = 1 byte), bit 31 the request for the
 * DMA-done interrupt.
 */
#define ODSY_DMA_HDR_CONTROL 0x1c
#define ODSY_DMA_IRQ_REQUEST 0x80000000u

/*
 * Reset values the stock IP35 BaseIO PROM's graphics diagnostic expects of a
 * healthy Tezro board. The Buzz registers carry no established reference and
 * reset to zero.
 */
#define XT_REQ_TIMEOUT_RESET 0x000fffff
#define FLCTL_CONFIG_RESET 0x0001c000

/*
 * The display back end's indexed register file: a 64-bit push to the selector
 * port, read back through its own per-index window. Only the opt port's I2C
 * command/status pair is exercised here; the note's two worked transfers both
 * run on it.
 */
#define ODSY_BACKEND_SELECT 0x400000
#define ODSY_CFIFO_SELFTEST 0x201594
#define ODSY_I2C_OPT_CMD 0x233a00
#define ODSY_I2C_OPT_STATUS 0x233a08
#define ODSY_I2C_OPT_SELECTOR 0x39d00001u
#define ODSY_BACKEND_VERSION 0x23380c

/*
 * Bits 13:10 of the status word: the engine-state field the kernel driver's
 * I2C engine reset polls.
 */
#define ODSY_I2C_STATE_SHIFT 10
#define ODSY_I2C_STATE_MASK (0xfu << ODSY_I2C_STATE_SHIFT)

/*
 * The backend word stream's counted-record headers (tag 0b11 in bits 29:28,
 * index in bits 27:14, count in bits 13:0) and the colour-index fill word,
 * redeclared here the same way every register offset above already is
 * (hw/misc/sgi-odyssey.c). ODSY_BACKEND_DISPMODE_DID1 selects drawable 1's
 * display-mode register (index 0x2901, count 1), and ODSY_BACKEND_DISPMODE_DID0
 * drawable 0's, the id every pixel has until an id-plane write; its data word
 * ODSY_DISPMODE_GROUP17 names group 17, the only populated colormap in the
 * trace, at base 0x1100. ODSY_BACKEND_CMAP_HDR opens a three-entry record at
 * that base, and ODSY_BACKEND_PAD is a count-0 no-op record.
 */
#define ODSY_BACKEND_DISPMODE_DID0 0x3a400001u
#define ODSY_BACKEND_DISPMODE_DID1 0x3a404001u
#define ODSY_DISPMODE_GROUP17 0x004b5f10u
#define ODSY_BACKEND_CMAP_HDR 0x34400003u
#define ODSY_BACKEND_PAD 0x30000000u
#define ODSY_LUT_RED 0x3ff00000u
#define ODSY_LUT_GREEN 0x000ffc00u
#define ODSY_LUT_BLUE 0x000003ffu
#define ODSY_CMD_GL_COLOR_INDEX 0xc580c406u

/*
 * The drawable-id plane write path, redeclared here the same way every
 * command word above is. The X server's drawable-id plane write sends a
 * 0x1017 destination-select block whose third operand is ODSY_DEST_ID_PLANES,
 * a 0x1023 plane-mask block whose first operand is the write mask << 12, and
 * a COLOR_INDEX word carrying the drawable id << 3. A colour fill instead
 * carries ODSY_DEST_COLOUR_INDEX as the 0x1017 third operand, which is what
 * un-selects the id-plane path for the fills that follow.
 * ODSY_BACKEND_DISPMODE_DID24 selects drawable 24's display-mode register
 * (index 0x2918); ODSY_DISPMODE_GROUP18 names group 18 (base 0x1200).
 */
#define ODSY_CMD_DEST_SELECT 0x00011017u
#define ODSY_CMD_PLANE_MASK 0x00011023u
#define ODSY_DEST_ID_PLANES 0x20080080u
#define ODSY_DEST_COLOUR_INDEX 0x20086a00u
#define ODSY_BACKEND_DISPMODE_DID24 0x3a460001u
#define ODSY_DISPMODE_GROUP18 0x00000120u

/*
 * Crosstalk part 0xc013, manufacturer 0x023, revision nibble 0xa -- the
 * value a live V12 board (030-1611-001 rev C) answers, and the model's
 * default.
 */
#define IDENTITY_DEFAULT 0xac013047

#define NIC_LEVEL (1u << 0)
#define NIC_DONE (1u << 1)
#define NIC_PULSE(n) ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)

/*
 * The machine refuses to build without a firmware payload. Nothing here
 * executes it -- every test starts halted and drives the board over the
 * test protocol -- so an erased image is enough.
 */
static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * Most cases need a board in its reset state, not a newly started machine,
 * and starting one costs ten times what a system reset does. Cases
 * registered with add_case() share one machine, reset before each use;
 * add_fresh_case() gives a case machines of its own.
 *
 * Each shared case was checked once against a migration of its machine
 * taken before the case ran: after the case and a system reset, every
 * device section of the stream matched byte for byte, and every guest page
 * matched. A reset does not clear memory, so a case that writes guest
 * memory, directly or through the board's DMA, stays fresh. So does a case
 * that migrates its machine, reads the board's last-interrupt record (a
 * host-only value no reset touches), starts the machine with options, or
 * is about the state a machine starts in. The host-only pixel and object
 * selectors are not machine state either, so they are put back here.
 */
static QTestState *shared_board;
static bool shared_case;
static bool shared_in_use;

static QTestState *board_launch(const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom_path);

    return qtest_initf(MACHINE " -bios %s%s", quoted, extra);
}

static QTestState *board_start(const char *extra)
{
    static const char *const selectors[] = {
        "x-test-pixel-x", "x-test-pixel-y", "x-test-pixel-buffer",
        "x-test-object-base", "x-test-pixel-count",
    };

    if (!shared_case || *extra || shared_in_use) {
        return board_launch(extra);
    }
    shared_in_use = true;
    if (!shared_board) {
        shared_board = board_launch("");
        return shared_board;
    }
    qtest_system_reset(shared_board);
    for (unsigned i = 0; i < ARRAY_SIZE(selectors); i++) {
        qtest_qmp_assert_success(shared_board,
            "{'execute': 'qom-set', 'arguments': {'path': %s, "
            "'property': %s, 'value': 0}}", BOARD, selectors[i]);
    }
    return shared_board;
}

static void board_quit(QTestState *qts)
{
    if (qts == shared_board) {
        shared_in_use = false;
    } else {
        qtest_quit(qts);
    }
}

static void shared_case_run(gconstpointer data)
{
    shared_case = true;
    ((GTestFunc)data)();
    shared_case = false;
    g_assert_false(shared_in_use);
}

/* A running processor polls a mailbox and issues the requested stores. */
static void credit_emit(uint8_t *image, unsigned *pos, uint32_t word)
{
    stl_be_p(image + *pos, word);
    *pos += 4;
}

static void credit_li64(uint8_t *image, unsigned *pos, unsigned reg,
                        uint64_t value)
{
    credit_emit(image, pos, mips_ori(reg, 0, value >> 48));
    for (unsigned shift = 32; ; shift -= 16) {
        credit_emit(image, pos, mips_dsll(reg, reg, 16));
        credit_emit(image, pos, mips_ori(reg, reg, (value >> shift) & 0xffff));
        if (!shift) {
            break;
        }
    }
}

static void test_processor_credit_return(void)
{
    const uint64_t box = UINT64_C(0x180000);
    const uint64_t pi = SPACE_IO + UINT64_C(0x01800000);
    const unsigned ports[] = {
        ODSY_CMD_FIFO, 0x100000, ODSY_BACKEND_SELECT,
        ODSY_CTX_SWITCH_PORT, ODSY_CONTROL - 4,
    };
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    unsigned pos = 0x400, loop, branch;
    int fd;

    stl_be_p(image, 0x100000ff); /* branch to 0x400 */
    credit_emit(image, &pos, mips_mfc0(8, 12)); /* mfc0 t0, status */
    credit_emit(image, &pos, mips_ori(8, 8, 0x80)); /* 64-bit kernel addrs */
    credit_emit(image, &pos, mips_mtc0(8, 12)); /* mtc0 t0, status */
    for (unsigned i = 0; i < 4; i++) {
        credit_emit(image, &pos, 0);
    }
    credit_li64(image, &pos, 16, UINT64_C(0x9000000000000000) | box);
    loop = pos;
    credit_emit(image, &pos, mips_ld(9, 16, 0)); /* ld t1, 0(s0): store count */
    credit_emit(image, &pos, mips_branch(MIPS_OPC_BEQ, 9, 0, -2)); /* wait */
    credit_emit(image, &pos, 0);
    credit_emit(image, &pos, mips_ld(10, 16, 8)); /* ld t2, 8(s0): target */
    credit_emit(image, &pos, mips_sd(0, 10, 0)); /* sd zero, 0(t2) */
    credit_emit(image, &pos, mips_daddiu(9, 9, -1)); /* decrement count */
    credit_emit(image, &pos, mips_branch(MIPS_OPC_BNE, 9, 0, -3)); /* repeat */
    credit_emit(image, &pos, 0);
    credit_emit(image, &pos, mips_sd(0, 16, 0)); /* publish completion */
    branch = pos;
    credit_emit(image, &pos, mips_branch(MIPS_OPC_BEQ, 0, 0,
                                         ((int)loop - (int)branch - 4) / 4));
    credit_emit(image, &pos, 0);
    fd = g_file_open_tmp("odyssey-credit-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf(MACHINE " -accel tcg,thread=multi -bios %s", path);
    unlink(path);
    qtest_writeq(qts, pi + 0x400140, SLOT);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    for (unsigned enabled = 0; enabled < 2; enabled++) {
        for (unsigned i = 0; i < ARRAY_SIZE(ports); i++) {
            uint64_t address = (uint64_t)SLOT * 0x1000000 + ports[i];
            int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

            qtest_writeq(qts, pi + 0x300, (address & UINT64_C(0xfffffe0000)) |
                         (enabled ? UINT64_C(1) << 40 : 0));
            qtest_writeq(qts, pi + 0x308, 100);
            qtest_writeq(qts, SPACE_HUB + box + 8,
                         UINT64_C(0x9200000000000000) | address);
            qtest_writeq(qts, SPACE_HUB + box, 3);
            while (qtest_readq(qts, SPACE_HUB + box)) {
                g_assert_cmpint(g_get_monotonic_time(), <, deadline);
                g_usleep(1000);
            }
            g_test_message("port 0x%x counted=%u", ports[i], enabled);
            g_assert_cmpuint(qtest_readq(qts, pi + 0x308), ==, 100);
        }
    }
    board_quit(qts);
}

static uint32_t reg_read(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, WIDGET + off);
}

static void reg_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_writel(qts, WIDGET + off, value);
}

/*
 * Where the flag source's interrupt goes: the routed destination, with the
 * flag vector in the low byte of the vector table.
 */
static void flag_dest_program(QTestState *qts)
{
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);
}

/* The indexed register file's write port: (selector << 32) | data. */
static void i2c_push(QTestState *qts, uint32_t selector, uint32_t data)
{
    qtest_writeq(qts, WIDGET + ODSY_BACKEND_SELECT,
                 ((uint64_t)selector << 32) | data);
}

/*
 * The backend word stream (hw/misc/sgi-odyssey.c): an 8-byte store is two
 * words, upper half first; a 4-byte store is one word.
 */
static void backend_push64(QTestState *qts, uint32_t first, uint32_t second)
{
    qtest_writeq(qts, WIDGET + ODSY_BACKEND_SELECT,
                 ((uint64_t)first << 32) | second);
}

static void backend_push32(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, WIDGET + ODSY_BACKEND_SELECT, word);
}

/*
 * One command-FIFO word, in program order (odyssey_cmd_word(),
 * hw/misc/sgi-odyssey.c).
 */
static void cmd_push(QTestState *qts, uint32_t word)
{
    reg_write(qts, ODSY_CMD_FIFO, word);
}

/*
 * Two command-FIFO words as a single 64-bit store -- the width the
 * datasheet's qtest checklist item 21 establishes alongside the 32-bit
 * store cmd_push() above, upper half first (odyssey_cmd_fifo_write(),
 * hw/misc/sgi-odyssey.c).
 */
static void cmd_push64(QTestState *qts, uint32_t first, uint32_t second)
{
    qtest_writeq(qts, WIDGET + ODSY_CMD_FIFO,
                 ((uint64_t)first << 32) | second);
}

/*
 * One word to the per-context USER command FIFO (0x100000), the port an X
 * client's DDX pushes its drawing stream through. It carries the same grammar
 * as the kernel port cmd_push() above and reaches the same interpreter
 * (odyssey_write(), hw/misc/sgi-odyssey.c); the true-colour GL path is driven
 * here to mirror where a real client draws.
 */
#define ODSY_USER_CMD_FIFO 0x100000

static void user_cmd_push(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, WIDGET + ODSY_USER_CMD_FIFO, word);
}

/*
 * The modeled framebuffer's host-only introspection: a fixed 1280x1024
 * surface (SGI_ODYSSEY_WIDTH/HEIGHT, include/hw/misc/sgi-odyssey.h), not a
 * guest-visible register. The command FIFO and data port stay
 * accept-and-trace only (test_unmodeled_offsets_are_visible), so this is
 * the only writer this increment has.
 */
static uint64_t odsy_qom_get_uint(QTestState *qts, const char *property)
{
    QDict *resp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", BOARD, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) s = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, s->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

static bool odsy_qom_get_bool(QTestState *qts, const char *property)
{
    QDict *resp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", BOARD, property);
    bool value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) s = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, s->str);
    }
    value = qdict_get_bool(resp, "return");
    qobject_unref(resp);
    return value;
}

static void odsy_test_pixel_select(QTestState *qts, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", BOARD, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", BOARD, y);
}

static uint32_t odsy_test_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    odsy_test_pixel_select(qts, x, y);
    return odsy_qom_get_uint(qts, "x-test-pixel");
}

/*
 * The w x h block of plane pixels at (x, y), row-major into @out: the probe
 * odsy_test_pixel() makes, cheaper. Each row is one QMP read of the device's
 * x-test-pixel-row bulk property, a string of eight hex digits per pixel,
 * where odsy_test_pixel() takes three commands a pixel. QMP commands are
 * nearly all a scan costs.
 */
static void odsy_test_pixels(QTestState *qts, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint32_t *out)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", BOARD, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-count', 'value': %u}}", BOARD, w);
    for (uint32_t row = 0; row < h; row++) {
        QDict *resp;
        const char *hex;

        qtest_qmp_assert_success(qts,
            "{'execute': 'qom-set', 'arguments': {'path': %s, "
            "'property': 'x-test-pixel-y', 'value': %u}}", BOARD, y + row);
        resp = qtest_qmp(qts,
            "{'execute': 'qom-get', 'arguments': {'path': %s, "
            "'property': 'x-test-pixel-row'}}", BOARD);
        g_assert_true(qdict_haskey(resp, "return"));
        hex = qdict_get_str(resp, "return");
        g_assert_cmpuint(strlen(hex), ==, (size_t)w * 8);
        for (uint32_t col = 0; col < w; col++) {
            char word[9] = { 0 };

            memcpy(word, hex + col * 8, 8);
            out[row * w + col] = g_ascii_strtoull(word, NULL, 16);
        }
        qobject_unref(resp);
    }
}

static void odsy_test_pixel_write(QTestState *qts, uint32_t x, uint32_t y,
                                  uint32_t value)
{
    odsy_test_pixel_select(qts, x, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel', 'value': %u}}", BOARD, value);
}

static void test_model_did_pixel_accessors(void)
{
    QTestState *qts = board_start("");
    uint32_t x, y;

    g_assert_false(odsy_qom_get_bool(qts, "x-mesa-renderer"));
    board_quit(qts);

    qts = board_start(" -global sgi-odyssey.x-mesa-renderer=on");
    g_assert_true(odsy_qom_get_bool(qts, "x-mesa-renderer"));
    /* Only a host without the renderer draws the whole device scalar. */
    if (g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_assert_false(odsy_qom_get_bool(qts, "x-mesa-unavailable"));
    }
    /* Model storage only: alternate write order; no GL draw is exercised. */
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            uint32_t index = y * 4 + x;

            if (!(index & 1)) {
                odsy_test_pixel_write(qts, x, y, 0x11223300u + index);
            }
            odsy_test_pixel_select(qts, x, y);
            qtest_qmp_assert_success(qts,
                "{'execute': 'qom-set', 'arguments': {'path': %s, "
                "'property': 'x-test-did', 'value': %u}}",
                BOARD, 0x40u + index);
            if (index & 1) {
                odsy_test_pixel_write(qts, x, y, 0x11223300u + index);
            }
        }
    }
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            uint32_t index = y * 4 + x;

            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==,
                            0x11223300u + index);
            odsy_test_pixel_select(qts, x, y);
            g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-did"), ==,
                            0x40u + index);
        }
    }
    board_quit(qts);
}

static uint32_t odsy_test_display_pixel(QTestState *qts, uint32_t x,
                                        uint32_t y)
{
    odsy_test_pixel_select(qts, x, y);
    return odsy_qom_get_uint(qts, "x-test-display-pixel");
}

/*
 * What the display shows at (x, y), in the plane's 0xRRGGBBAA form with an
 * opaque alpha: the board resolves colour at scanout through the pixel's
 * drawable, display mode and lookup RAM, so a colour-index pixel's colour is
 * only visible here.
 */
static uint32_t odsy_shown(QTestState *qts, uint32_t x, uint32_t y)
{
    return (odsy_test_display_pixel(qts, x, y) << 8) | 0xff;
}

/*
 * The identity gamma ramp at lookup-RAM 0x1a00 (the OpenBSD driver's ramp:
 * entry i holds i >> 2, ((i - 0x200) >> 1) + 0x80 or (i - 0x300) + 0x100
 * over the three ranges, in all three ten-bit channels) and drawable did's
 * display mode set to an RGB mode (group 26, the gamma table): what a guest
 * loads before any RGB pixel is visible. Out of reset the lookup RAM is
 * zero and the screen black. The mode is the X server's for its 24-bit
 * TrueColor visual, which shows every bit of each channel byte.
 */
#define ODSY_DISPMODE_RGB 0x005215a6u
#define ODSY_BACKEND_HDR(index, count) \
    (0x30000000u | ((uint32_t)(index) << 14) | (count))

static void display_identity(QTestState *qts, uint32_t did)
{
    uint32_t i;

    backend_push32(qts, ODSY_BACKEND_HDR(0x2900 + did, 1));
    backend_push32(qts, ODSY_DISPMODE_RGB);
    backend_push32(qts, ODSY_BACKEND_HDR(0x1a00, 0x600));
    for (i = 0; i < 0x600; i++) {
        uint32_t v = i < 0x200 ? i >> 2 :
                     i < 0x400 ? ((i - 0x200) >> 1) + 0x80 :
                     (i - 0x300) + 0x100;

        backend_push32(qts, v << 20 | v << 10 | v);
    }
}

/* Texel (x, y) of the board-memory object at base, as 0xRRGGBBAA. */
static uint32_t odsy_test_object_texel(QTestState *qts, uint32_t base,
                                       uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-object-base', 'value': %u}}", BOARD, base);
    odsy_test_pixel_select(qts, x, y);
    return odsy_qom_get_uint(qts, "x-test-object-texel");
}

static uint32_t odsy_screendump_pixel_sized(QTestState *qts, uint32_t x,
                                           uint32_t y, uint32_t w, uint32_t h)
{
    g_autofree char *header = g_strdup_printf("P6\n%u %u\n255\n", w, h);
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    char *pixels;
    gsize length;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-surface-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    {
        QDict *response = qtest_qmp(qts,
            "{'execute': 'screendump', 'arguments': {'filename': %s}}",
            path);
        if (!qdict_haskey(response, "return")) {
            g_autoptr(GString) text = qobject_to_json(QOBJECT(response));
            g_error("screendump failed: %s", text->str);
        }
        qobject_unref(response);
    }
    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, header));
    pixels = g_strstr_len(contents, length, "\n255\n");
    g_assert_nonnull(pixels);
    pixels += 5;
    g_assert_cmpuint(x, <, w);
    g_assert_cmpuint(y, <, h);
    g_assert_cmpuint((gsize)(pixels - contents) + w * h * 3, <=, length);
    pixels += (y * w + x) * 3;
    unlink(path);
    return ((uint8_t)pixels[0] << 16) | ((uint8_t)pixels[1] << 8) |
           (uint8_t)pixels[2];
}

static uint32_t odsy_screendump_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
    return odsy_screendump_pixel_sized(qts, x, y, 1280, 1024);
}

/* The "W H" of a screendump's PPM header. */
static char *odsy_screendump_size(QTestState *qts)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    char **lines;
    char *size;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-size-XXXXXX.ppm", &path, &error);
    g_assert_no_error(error);
    close(fd);
    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s}}", path);
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    unlink(path);
    lines = g_strsplit(contents, "\n", 3);
    g_assert_cmpstr(lines[0], ==, "P6");
    size = g_strdup(lines[1]);
    g_strfreev(lines);
    return size;
}

/*
 * One bit slot on the board-identity access register, with the widths
 * the stock IP35 PROM was captured composing: a long low drive is a bus
 * reset, a short one a write-one or a read, and one in between a
 * write-zero.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    uint32_t nic;

    reg_write(qts, ODSY_NIC, NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    nic = reg_read(qts, ODSY_NIC);
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q) nic_slot((q), 520, 65)
#define NIC_READ(q) nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q) nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

/* The identity the driver reads, and the revision nibble it dispatches on. */
static void test_identity_carries_part_and_revision(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    board_quit(qts);

    qts = board_start(" -global sgi-odyssey.revision=3");
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, 0x3c013047);
    board_quit(qts);
}

/*
 * A live V12 board reads back the same value at a configuration-block
 * register's base (the upper half) as at base+4 (the low half) across the whole
 * block, not only on the identity register. Pin that on identity and on a
 * second, unrelated register so the behavior is block-wide rather than
 * hardcoded to one offset.
 */
static void test_configuration_block_mirrors_both_halves(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(qtest_readl(qts, WIDGET + 0x0000), ==,
                     reg_read(qts, ODSY_IDENTITY));

    reg_write(qts, ODSY_CONTROL, 0x0f003200 | SLOT);
    g_assert_cmphex(qtest_readl(qts, WIDGET + 0x0020), ==,
                     reg_read(qts, ODSY_CONTROL));

    board_quit(qts);
}

/*
 * The driver recovers its own widget number from bits 3:0 of the control
 * register before anything writes it, so the reset value carries the
 * slot. The register itself is plain read/write, the way the Bridge's is:
 * nothing forces the nibble back.
 */
static void test_widget_control_reads_back(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_CONTROL) & 0xf, ==, SLOT);

    reg_write(qts, ODSY_CONTROL, 0x0f003200 | SLOT);
    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, 0x0f003200 | SLOT);
    reg_write(qts, ODSY_CONTROL, 0);
    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, 0);

    /* The error class the crosstalk error handler tests stays clear. */
    g_assert_cmphex(reg_read(qts, ODSY_STATUS) & 0x1c0, ==, 0);
    reg_write(qts, ODSY_ERROR_COMMAND, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_ERROR_COMMAND), ==, 0);

    board_quit(qts);
}

/*
 * Memory size, bank count and column-address latency reach the driver
 * through one register. The IRIX 6.5.30 kernel decodes it as bits 31:30 the
 * size code (32, 64 or 128 MB), bit 29 set for four banks and clear for two,
 * and bit 28 set for latency 2 and clear for latency 3; gfxinfo prints those
 * values back. The zero low bits are the outstanding-operation count and the
 * data-FIFO level the driver waits on at nearly every step. Bit 24 is the
 * unexplained "don't-care copy field" a live V12 board reads as 0x1; the
 * model carries it as an unconditional constant, so it appears in every
 * value below. The default (128 MB, four banks, latency 3) is that board's
 * measured 0xa1000000, which gfxinfo on a real V12 prints as "Banks: 4,
 * CAS latency: 3".
 */
static void test_memory_configuration_follows_properties(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0xa1000000);
    board_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=32"
                      " -global sgi-odyssey.memory-banks=2");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x01000000);
    board_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=64"
                      " -global sgi-odyssey.memory-banks=4"
                      " -global sgi-odyssey.cas-latency=2");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x71000000);
    board_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=64"
                      " -global sgi-odyssey.memory-banks=2"
                      " -global sgi-odyssey.cas-latency=2");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x51000000);
    board_quit(qts);
}

/*
 * "product" presents each marketed board's identity. The IRIX 6.5.30
 * kernel turns widget revision 1-3 into BUZZ A and 9-15 into BUZZ B, and
 * gfxinfo prints the minor number as the revision's offset less one, so
 * revision 2 is "A.1" and revision 10 is "B.1". Public captures show A.1
 * on a V6, B.1 on a V10 and on a V12 (the measured 0xa), and PB&J 1 on
 * each; the V8's A.1 is inferred from its generation. The 128 MB boards
 * report four banks and the 32 MB boards two, the kernel's bit 29.
 */
static void test_profile_selects_identity_and_memory(void)
{
    static const struct {
        const char *profile;
        uint32_t identity;
        uint32_t memory_config;
    } cases[] = {
        { "v12", IDENTITY_DEFAULT, 0xa1000000 },
        { "v10", IDENTITY_DEFAULT, 0x01000000 },
        { "v8",  0x2c013047,       0xa1000000 },
        { "v6",  0x2c013047,       0x01000000 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *extra =
            g_strdup_printf(" -global sgi-odyssey.product=%s",
                            cases[i].profile);
        QTestState *qts = board_start(extra);

        g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, cases[i].identity);
        g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==,
                         cases[i].memory_config);
        board_quit(qts);
    }
}

/*
 * Bits 29 (four memory banks) and 28 (cas latency 2) of the memory
 * configuration word, decomposed from the V12 profile's measured
 * 0xa1000000: four banks (bit 29 set), latency 3 rather than 2 (bit 28
 * clear), the "Banks: 4, CAS latency: 3" a real V12's gfxinfo prints. Bits
 * 27:24 carry the constant "copy field" (ODSY_CONFIG_COPY_FIELD), that same
 * board's only established value for it.
 */
static void test_v12_profile_memory_word_bits(void)
{
    QTestState *qts = board_start(" -global sgi-odyssey.product=v12");
    uint32_t config = reg_read(qts, ODSY_MEMORY_CONFIG);

    g_assert_cmphex(config, ==, 0xa1000000);
    g_assert_cmphex(config & (1u << 29), ==, (1u << 29));
    g_assert_cmphex(config & (1u << 28), ==, 0);
    g_assert_cmphex((config >> 24) & 0xf, ==, 0x1);
    board_quit(qts);
}

/*
 * The driver's six-write memory-reset sequence. Each write is accepted and
 * logged by name under -d unimp, rather than falling through to the generic
 * "not modeled" message, and the board keeps answering identity afterwards: the
 * model does not reset anything real, it only stops pretending the register
 * does not exist.
 */
static void test_reset_control_accepts_six_writes(void)
{
    static const uint32_t sequence[] = { 1, 0, 1, 0, 1, 0 };
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-reset-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    for (size_t i = 0; i < ARRAY_SIZE(sequence); i++) {
        reg_write(qts, ODSY_RESET_CONTROL, sequence[i]);
    }
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "0x1004 is not modeled"));
    g_assert_nonnull(strstr(log, "reset control register accepted"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * gfxinfo's "PB&J version" is bits 2:0 of 0x23380c. Captured V6, V10 and
 * V12 boards all print 1, so every profile answers 1. A write is a guest
 * error, the same as every other read-only register in this block, and
 * changes nothing.
 */
static void test_backend_version_is_pinned(void)
{
    static const char *const profiles[] = { "v12", "v10", "v8", "v6" };
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-backend-version-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors -D %s", quoted);
    qts = board_start(extra);

    /* The default, the v12 board: gfxinfo's "PB&J version 1". */
    g_assert_cmphex(qtest_readl(qts, WIDGET + ODSY_BACKEND_VERSION), ==, 1);
    qtest_writel(qts, WIDGET + ODSY_BACKEND_VERSION, 0);
    g_assert_cmphex(qtest_readl(qts, WIDGET + ODSY_BACKEND_VERSION), ==, 1);
    board_quit(qts);

    for (size_t i = 0; i < ARRAY_SIZE(profiles); i++) {
        g_autofree char *profile =
            g_strdup_printf(" -global sgi-odyssey.product=%s", profiles[i]);

        qts = board_start(profile);
        g_assert_cmphex(qtest_readl(qts, WIDGET + ODSY_BACKEND_VERSION),
                        ==, 1);
        board_quit(qts);
    }

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "read-only register at 0x23380c"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * A profile's board memory is a reported capacity (the "memory-size"
 * property), not real model storage: every off-screen object is backed
 * from one fixed, profile-independent pool (ODSY_OBJ_POOL_BYTES). A
 * destination base at or beyond what a 32 MiB profile reports is a guest
 * bug against the board it was told it has, kept observable rather than
 * invented: the selection is dropped, logged once, and the board keeps
 * answering normally afterwards.
 */
static void test_board_memory_bound_is_a_guest_error(void)
{
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-oob-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -global sgi-odyssey.product=v10"
                            " -d guest_errors -D %s", quoted);
    qts = board_start(extra);

    /*
     * A destination select's base is a board address in 16-byte units, so
     * this profile's 32 MiB is 0x200000 units: 0x200000 itself is the first
     * out-of-range value.
     */
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x200000);
    cmd_push(qts, 16);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "destination base 0x200000 is beyond the "
                                 "32 MiB this board reports"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/* Run presence, skip-ROM and a read of the identity part's first page. */
static void nic_read_page(QTestState *qts, uint8_t page[32])
{
    unsigned int i;

    g_assert_false(NIC_RESET(qts));         /* pulled low: the part answers */
    nic_write_byte(qts, 0xcc);              /* one part on the bus: skip ROM */
    nic_write_byte(qts, 0xf0);              /* read memory, address 0 */
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);
    for (i = 0; i < 32; i++) {
        page[i] = nic_read_byte(qts);
    }
}

/*
 * A real one-wire bus behind the access register, not a status bit: the
 * stock firmware runs presence, selection and a memory read, and stops
 * clocking after one 32-byte page. Reading that page back is what proves
 * the register drives a bus at all.
 */
static void test_board_identity_part_answers(void)
{
    QTestState *qts = board_start("");
    uint8_t page[32];

    nic_read_page(qts, page);

    /*
     * A format byte, four spaces and the delimiter a scanning reader
     * needs before the first field name, then the record itself.
     */
    g_assert_cmphex(page[0], ==, 0x01);
    g_assert_cmphex(page[5], ==, ';');
    g_assert_cmpmem(&page[6], 20, "Name:ODY128VERSIONB;", 20);

    board_quit(qts);
}

/*
 * Each profile's identity record names its Octane board, the name hinv -mv
 * prints for it: ODY32 (a V6, 030-1403), ODY128 (a V8, 030-1404),
 * ODY32VERSIONB (a V10, 030-1655) and ODY128VERSIONB (a V12, 030-1611).
 * An explicit nic-record still wins.
 */
static void test_profile_names_its_board(void)
{
    static const struct {
        const char *extra;
        const char *record;
    } cases[] = {
        { " -global sgi-odyssey.product=v12", "Name:ODY128VERSIONB;" },
        { " -global sgi-odyssey.product=v10", "Name:ODY32VERSIONB;" },
        { " -global sgi-odyssey.product=v8", "Name:ODY128;" },
        { " -global sgi-odyssey.product=v6", "Name:ODY32;" },
        { " -global sgi-odyssey.product=v6"
          " -global sgi-odyssey.nic-record=Name:TEST", "Name:TEST;" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = board_start(cases[i].extra);
        size_t length = strlen(cases[i].record);
        uint8_t page[32];

        nic_read_page(qts, page);
        g_assert_cmpmem(&page[6], length, cases[i].record, length);
        board_quit(qts);
    }
}

/*
 * The flag, enable and acknowledge trio, and the rule that an enable on
 * its own raises nothing unless a source is already pending. There is no
 * sysbus IRQ line to intercept any more: a transition sends a crosstalk
 * interrupt-packet to the routed destination with the flag source's vector
 * (test_interrupt_packet_composition and
 * test_interrupt_packet_reaches_bedrock_pending below pin that packet's
 * shape and delivery), and x-test-last-interrupt-value's
 * ODSY_HUB_INT_MODIFY_SET bit is this test's stand-in for "asserted".
 */
static void test_interrupt_registers(void)
{
    QTestState *qts = board_start("");

    /* A destination programmed once, as real firmware would before enabling. */
    flag_dest_program(qts);

    /* The empty FIFO leaves the low-water interrupt pending out of reset. */
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==,
                    ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value") &
                    ODSY_HUB_INT_MODIFY_SET, ==, 0);

    /* Enabling the sources asserts, since low water is already pending. */
    reg_write(qts, ODSY_INTR_ENABLE_SET, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==, 0xffffffff);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value") &
                    ODSY_HUB_INT_MODIFY_SET, ==, ODSY_HUB_INT_MODIFY_SET);

    /* Write-1-to-clear over the flags, and nothing readable behind it. */
    reg_write(qts, ODSY_INTR_CLEAR, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_CLEAR), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value") &
                    ODSY_HUB_INT_MODIFY_SET, ==, 0);
    /* Clearing flags leaves the enable mask alone. */
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==, 0xffffffff);

    /* The software-sync pair: a token written, then read back. */
    reg_write(qts, ODSY_SW_TOKEN, 0x5a5a5a5a);
    g_assert_cmphex(reg_read(qts, ODSY_SW_TOKEN), ==, 0x5a5a5a5a);

    board_quit(qts);
}

/*
 * The enable mask's two halves, the way the driver drives them: 0x1034 sets
 * the bits written and reads back the whole mask, 0x103c clears the bits
 * written. The kernel driver's interrupt setup writes its mask to 0x1034,
 * the kernel driver's FIFO high-water handler adds the low-water bit there,
 * the kernel driver's FIFO low-water handler removes it through 0x103c, and
 * the kernel driver's interrupt shutdown clears everything with all ones.
 * The level follows flags & enable through each step; clearing an enable
 * bit leaves its flag pending.
 */
static void test_interrupt_enable_set_and_clear(void)
{
    QTestState *qts = board_start("");

    flag_dest_program(qts);

    /* The driver's initial mask leaves low water off: no level. */
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_INIT_MASK);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==,
                    ODSY_INTR_INIT_MASK);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value") &
                    ODSY_HUB_INT_MODIFY_SET, ==, 0);

    /* The high-water path adds one bit; the rest of the mask stays. */
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==,
                    ODSY_INTR_INIT_MASK | ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    /* The low-water handler removes it: the level drops, the flag stays. */
    reg_write(qts, ODSY_INTR_ENABLE_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==,
                    ODSY_INTR_INIT_MASK);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) &
                    ODSY_INTR_FIFO_LOW_WATER, ==, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    /* Disabling with all ones empties the mask. */
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    reg_write(qts, ODSY_INTR_ENABLE_CLEAR, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    board_quit(qts);
}

/*
 * The flag handler's acknowledge: it writes the mask it dispatched on to
 * 0x101c, which clears those flags and nothing else. The kernel driver's
 * DMA submission writes 0x800 before a transfer, which clears a flag the
 * model never raises, so it changes nothing.
 */
static void test_interrupt_clear_acknowledges_flags(void)
{
    QTestState *qts = board_start("");

    flag_dest_program(qts);
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_DMA_DONE);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==,
                    ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==,
                    ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    board_quit(qts);
}

/*
 * The driver writes the flags register itself: the kernel driver's FIFO
 * high-water handler, finding the FIFO already below low water, stores
 * 0x200 before enabling that source, and a diagnostic clears bit 11 by
 * writing back the flags it read without it. The model chooses a
 * replacing write (inferred; set-only is the competing reading), so a
 * write raises the flags it carries and drops those it does not, and the
 * interrupt follows.
 */
static void test_interrupt_flags_take_written_value(void)
{
    QTestState *qts = board_start("");

    flag_dest_program(qts);
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    reg_write(qts, ODSY_INTR_FLAGS, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==,
                    ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    /* A write without the low-water bit drops it, as set-only would not. */
    reg_write(qts, ODSY_INTR_FLAGS, ODSY_INTR_DMA_DONE);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, ODSY_INTR_DMA_DONE);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    reg_write(qts, ODSY_INTR_FLAGS, 0);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    board_quit(qts);
}

/*
 * Low water where the driver has disabled it. A submission that does not
 * wait never lifts the FIFO to its low-water mark, so it raises nothing.
 * The driver's high-water path, finding the FIFO already below low water,
 * sets the flag itself through the flags register: that latches it with
 * no interrupt until the path enables the source, which asserts at once.
 */
static void test_lowater_not_raised_while_disabled(void)
{
    QTestState *qts = board_start("");

    flag_dest_program(qts);
    reg_write(qts, ODSY_INTR_ENABLE_SET,
              ODSY_INTR_INIT_MASK | ODSY_INTR_FIFO_LOW_WATER);

    /*
     * The kernel driver's FIFO low-water handler, then the flag handler's
     * common clear.
     */
    reg_write(qts, ODSY_INTR_ENABLE_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    qtest_writel(qts, WIDGET + ODSY_CMD_FIFO, 0x00010046);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) &
                    ODSY_INTR_FIFO_LOW_WATER, ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0) & 0x20000, ==, 0x20000);
    reg_write(qts, ODSY_INTR_FLAGS, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    board_quit(qts);
}

/*
 * The board status register the kernel's periodic gfx-credits check reads
 * (odsy status0, 0x1064). This model's command FIFO drains synchronously, so
 * it always reports enabled, empty and below the low-water mark. Reading a
 * clear low-water or enabled bit here — as the unmodeled default did,
 * answering zero — makes the driver log a graphics error for board 0 and
 * take its board-reset recovery path.
 */
static void test_board_status_reports_fifo_ready(void)
{
    QTestState *qts = board_start("");

    /* The FIFO-enabled, low-water and empty bits. */
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10030000);

    board_quit(qts);
}

/*
 * The command-FIFO low-water interrupt the driver waits on to replenish its
 * graphics credits. The empty FIFO leaves it pending out of reset, so enabling
 * the source asserts at once; acknowledging clears it; and a later command
 * submission that does not wait leaves it clear, since the FIFO never rose
 * to its low-water mark. Without the reset-pending latch
 * the kernel's post-reset watchdog never sees it and gives up starting Xsgi.
 * There is no sysbus IRQ line any more (see test_interrupt_registers above):
 * this pins the same edges through the packet value's set bit instead.
 */
static void test_command_fifo_lowater_interrupt(void)
{
    QTestState *qts = board_start("");

    flag_dest_program(qts);

    /* Pending out of reset (empty FIFO): enabling the source asserts. */
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) &
                    ODSY_INTR_FIFO_LOW_WATER, ==, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    /* Clearing the flag drops the level and sends the clearing packet. */
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) &
                    ODSY_INTR_FIFO_LOW_WATER, ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    /* A command submission that does not wait raises nothing. */
    qtest_writel(qts, WIDGET + ODSY_CMD_FIFO, 0x00010046);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) &
                    ODSY_INTR_FIFO_LOW_WATER, ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    board_quit(qts);
}

/*
 * A reset lowers the flag level with the flags and enables, quietly: the
 * destination is reset too, so there is nowhere to send a clearing packet,
 * and the reset must not try to (it used to, and logged a guest error for
 * the missing destination). The next edge after it is a rising one and
 * sends its set packet; a different flag vector after the reset tells that
 * packet from the one before.
 */
static void test_reset_clears_flag_level(void)
{
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-reset-level-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors -D %s", quoted);
    qts = board_start(extra);

    flag_dest_program(qts);
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    qtest_system_reset(qts);
    flag_dest_program(qts);
    reg_write(qts, ODSY_ROUTE_VECTORS,
              (ODSY_TEST_ROUTE_VECTORS & ~0xffu) | (ODSY_TEST_VECTOR + 1));
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    (ODSY_TEST_VECTOR + 1) | ODSY_HUB_INT_MODIFY_SET);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "no destination programmed"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * Part A's own proof: the packet an edge composes decodes int_dest_upper's
 * vector field and int_dest_lower/int_dest_upper's address exactly as
 * hw/misc/sgi-mgras.c's own fabric contract does, and nothing is sent before
 * an edge or with no destination programmed.
 */
static void test_interrupt_packet_composition(void)
{
    QTestState *qts = board_start("");

    /*
     * No destination programmed: the rising edge from enabling the
     * already-pending low-water flag logs a guest error and sends nothing.
     * interrupt_asserted still latches, so the next edge is the falling one.
     */
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);

    /*
     * The crosstalk error destination reads back and carries nothing; the
     * routed destination alone sends nothing without a new edge.
     */
    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);
    g_assert_cmphex(reg_read(qts, ODSY_INT_DEST_UPPER), ==,
                    ODSY_TEST_INT_DEST_UPPER);
    g_assert_cmphex(reg_read(qts, ODSY_INT_DEST_LOWER), ==,
                    ODSY_TEST_INT_DEST_LOWER);
    flag_dest_program(qts);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);

    /*
     * Acknowledging the still-pending flag is now the falling edge (flags
     * still read the low-water bit; nothing has drained it yet): it finds
     * the destination just programmed and composes its address, clearing
     * the vector's own bit.
     */
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    ODSY_TEST_PACKET_FABRIC_ADDRESS);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    /* The flag set again readdresses the same destination, setting it. */
    reg_write(qts, ODSY_INTR_FLAGS, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    ODSY_TEST_PACKET_FABRIC_ADDRESS);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    board_quit(qts);
}

/*
 * Part B's own proof: the packet a real edge sends does not just look right
 * from the board's own introspection above -- it lands on a real Bedrock's
 * own Processor Interface pending state, at the offset ii_inbound_register()
 * (hw/misc/sgi-bedrock-io.c) decodes it to, reached only because the machine
 * links this board through the XBridge packet fabric to host port 0xa.
 * Both the prefixed carrier address and the pending register are checked,
 * so introspection alone cannot hide a misrouted packet.
 */
static void test_interrupt_packet_reaches_bedrock_pending(void)
{
    QTestState *qts = board_start("");

    flag_dest_program(qts);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) &
                    (UINT64_C(1) << ODSY_TEST_VECTOR), ==, 0);

    /* The rising edge's packet sets the vector's own bit at the real Bedrock. */
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    ODSY_TEST_PACKET_FABRIC_ADDRESS);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) &
                    (UINT64_C(1) << ODSY_TEST_VECTOR), ==,
                    UINT64_C(1) << ODSY_TEST_VECTOR);

    /* The falling edge's packet clears the same bit. */
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) &
                    (UINT64_C(1) << ODSY_TEST_VECTOR), ==, 0);

    board_quit(qts);
}

/*
 * The kernel driver gives the routed table's low byte to its flag handler,
 * which runs its low-water handler, and the crosstalk error destination to
 * its crosstalk error handler, which reads only the widget status's error
 * bits and otherwise reports an unknown interrupt. So low water goes
 * through the routed destination with the flag vector, not the error
 * destination.
 */
static void test_flag_interrupt_uses_routed_vector(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, (ODSY_TEST_ROUTE_VECTORS & ~0xffu) | 4);

    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    4 | ODSY_HUB_INT_MODIFY_SET);
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    4);

    board_quit(qts);
}

/*
 * The registers the stock IP35 BaseIO PROM's graphics diagnostic reads
 * before it will print a passing board version, SDRAM size and bank count:
 * the crosstalk request timeout, the flow-control configuration and the six
 * "Buzz registers" the diagnostic exercises with a walking-ones/walking-
 * zero-complement self-test whose exact pass criterion is not established
 * by any available source. Plain read/write storage is enough for that
 * self-test to pass without inventing a bit layout the evidence does not
 * resolve.
 */
static void test_baseio_prom_diagnostic_registers(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_XT_REQ_TIMEOUT), ==,
                     XT_REQ_TIMEOUT_RESET);
    g_assert_cmphex(reg_read(qts, ODSY_FLCTL_CONFIG), ==, FLCTL_CONFIG_RESET);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_1), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_2), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_3), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_4), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_5), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_6), ==, 0);

    /* Walking ones, then each register's own read/write roundtrip. */
    reg_write(qts, ODSY_XT_REQ_TIMEOUT, 0xaaaaaaaa);
    g_assert_cmphex(reg_read(qts, ODSY_XT_REQ_TIMEOUT), ==, 0xaaaaaaaa);
    reg_write(qts, ODSY_FLCTL_CONFIG, 0x55555555);
    g_assert_cmphex(reg_read(qts, ODSY_FLCTL_CONFIG), ==, 0x55555555);
    reg_write(qts, ODSY_BUZZ_1, 0xaaaaaaaa);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_1), ==, 0xaaaaaaaa);
    reg_write(qts, ODSY_BUZZ_2, 0x55555555);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_2), ==, 0x55555555);
    reg_write(qts, ODSY_BUZZ_3, 0xaaaaaaaa);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_3), ==, 0xaaaaaaaa);
    reg_write(qts, ODSY_BUZZ_4, 0x55555555);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_4), ==, 0x55555555);
    reg_write(qts, ODSY_BUZZ_5, 0xaaaaaaaa);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_5), ==, 0xaaaaaaaa);
    reg_write(qts, ODSY_BUZZ_6, 0x55555555);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_6), ==, 0x55555555);

    board_quit(qts);
}

/* How many times @needle occurs in @haystack, without overlapping. */
static unsigned count_occurrences(const char *haystack, const char *needle)
{
    unsigned count = 0;

    for (const char *p = strstr(haystack, needle); p;
         p = strstr(p + strlen(needle), needle)) {
        count++;
    }
    return count;
}

/*
 * Everything past the modeled ladder -- the indirect write port, the
 * display back end, and the command FIFO's read side (it is write-only,
 * hw/misc/sgi-odyssey.c) -- reads zero, swallows writes and says so once
 * per offset in the log. The command FIFO's write side now recognizes
 * the fill/copy/glyph grammar (the dedicated command-FIFO tests below),
 * so this pins its two remaining unmodeled paths instead: a store width
 * the FIFO does not accept (datasheet qtest checklist item 21 establishes
 * 32-bit and 64-bit only), and a command word that shape-matches nothing
 * the grammar recognizes.
 */
static void test_unmodeled_offsets_are_visible(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-unimp-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    g_assert_cmphex(reg_read(qts, ODSY_CMD_FIFO), ==, 0);
    /* A store width the command FIFO does not accept. */
    qtest_writew(qts, WIDGET + ODSY_CMD_FIFO, 0x1234);
    /*
     * A command word that matches nothing the grammar recognizes: not a
     * register-select (top 16 bits are not 0x0001, so the bit-field arity
     * rule does not apply), not a pixel run, not a tagged or bare trigger.
     */
    cmd_push(qts, 0x00007777);
    g_assert_cmphex(reg_read(qts, ODSY_CMD_FIFO), ==, 0);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    /* Once per offset, however many times the guest touches it. */
    g_assert_cmpuint(count_occurrences(log,
                         "4-byte read at 0x110000 is not modeled"), ==, 1);
    g_assert_nonnull(strstr(log, "2-byte write at 0x110000 is not modeled"));
    g_assert_nonnull(strstr(log,
                            "command FIFO word 0x00007777 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The command-FIFO self-test. The kernel driver's command-FIFO
 * initialisation pushes a nine-word preamble all through the command FIFO
 * -- three 4-byte words, five 8-byte words, one 4-byte word -- waits 2 ms,
 * then reads 0x201594 and requires 0x003d0046 (the value a real V12
 * answers); both graphics init paths retry six times and give up
 * otherwise, so the whole bring-up depends on it. Before the preamble the
 * read-back is zero.
 */
static void test_command_fifo_selftest(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_CFIFO_SELFTEST), ==, 0);

    cmd_push(qts, 0x00010854);
    cmd_push(qts, 0x00000000);
    cmd_push(qts, 0x0fffffff);
    cmd_push64(qts, 0x000006b0, 0x00000000);
    cmd_push64(qts, 0x000006b1, 0x00000100);
    cmd_push64(qts, 0x000006b3, 0x00005e50);
    cmd_push64(qts, 0x000006b2, 0x00007dc0);
    cmd_push64(qts, 0x000007b5, 0x00000001);
    cmd_push(qts, 0x00010046);

    g_assert_cmphex(reg_read(qts, ODSY_CFIFO_SELFTEST), ==, 0x003d0046);

    board_quit(qts);
}

static void test_reset_restores_the_documented_values(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_CONTROL, 0xdeadbeef);
    reg_write(qts, ODSY_INTR_ENABLE_SET, 0xffffffff);
    reg_write(qts, ODSY_SW_TOKEN, 0x12345678);
    reg_write(qts, ODSY_NIC, NIC_PULSE(520) | NIC_SAMPLE(65));
    reg_write(qts, ODSY_XT_REQ_TIMEOUT, 0xdeadbeef);
    reg_write(qts, ODSY_FLCTL_CONFIG, 0xdeadbeef);
    reg_write(qts, ODSY_BUZZ_1, 0xdeadbeef);
    reg_write(qts, ODSY_BUZZ_6, 0xdeadbeef);

    qtest_system_reset(qts);

    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, SLOT);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_SW_TOKEN), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_NIC), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_XT_REQ_TIMEOUT), ==,
                     XT_REQ_TIMEOUT_RESET);
    g_assert_cmphex(reg_read(qts, ODSY_FLCTL_CONFIG), ==, FLCTL_CONFIG_RESET);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_1), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_6), ==, 0);
    /* Reset does not disturb what the properties compose. */
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0xa1000000);

    board_quit(qts);
}

/*
 * The kernel driver's I2C PLL register write (opt, register 2, value 0x34)
 * against device 0x1f, from the note's register-contract test. No
 * downstream device is modeled, so every acknowledge reads low and DONE
 * follows the last non-zero command word rather than a real transfer's
 * completion.
 */
static void test_i2c_opt_port_pll_register_write(void)
{
    QTestState *qts = board_start("");

    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00004000);
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00000000);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_CMD), ==, 0x00000000);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400000);

    /* Address byte 0x3e = 0x1f << 1, write. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x0000133e);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);

    /* Register number 2. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00001202);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);

    /* The value 0x34. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00001234);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);

    /* Bus release. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00001834);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);

    board_quit(qts);
}

/*
 * The kernel driver's I2C DDC register read (opt, register 5, &v) against
 * device 0x1f, from the note's register-contract test. Step 15 is the
 * command-register readback the driver composes its receive command from;
 * get that wrong and the driver's next push is garbage. The received byte
 * itself is always zero: no downstream device answers, so *v comes back 0
 * rather than a real register value.
 */
static void test_i2c_opt_port_ddc_register_read(void)
{
    QTestState *qts = board_start("");

    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00004000);
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00000000);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_CMD), ==, 0x00000000);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400000);

    /* Address byte 0x3f, read. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x0000133f);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);

    /* Register number 5. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00001205);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_CMD), ==, 0x00001205);

    /* Receive, answer with a no-acknowledge. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00000605);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS), ==, 0x00400200);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_STATUS) & 0xff, ==, 0x00);

    /* Bus release, no wait follows. */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00000a05);

    board_quit(qts);
}

/*
 * The kernel driver's I2C engine reset polls bits 13:10 of the status word
 * until it reads 0 or 0xa. This model never asserts that field, so the poll
 * a real driver runs must terminate on its very first read, after any
 * command and across a system reset.
 */
static void test_i2c_reset_state_poll_terminates(void)
{
    QTestState *qts = board_start("");
    uint32_t state;

    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00001234);
    state = (reg_read(qts, ODSY_I2C_OPT_STATUS) & ODSY_I2C_STATE_MASK)
            >> ODSY_I2C_STATE_SHIFT;
    g_assert_true(state == 0 || state == 0xa);

    qtest_system_reset(qts);
    state = (reg_read(qts, ODSY_I2C_OPT_STATUS) & ODSY_I2C_STATE_MASK)
            >> ODSY_I2C_STATE_SHIFT;
    g_assert_true(state == 0 || state == 0xa);

    board_quit(qts);
}

/*
 * The test-only pixel accessor is this increment's only writer into the
 * modeled framebuffer. Pin the round trip from a raw R,G,B,A write through
 * to the host scanout: the display conversion drops the alpha byte and
 * shifts (odyssey_resolve_row(), hw/misc/sgi-odyssey.c), so a value written as
 * 0xRRGGBBAA reads back unchanged through x-test-pixel and as 0x00RRGGBB
 * through both x-test-display-pixel and a screendump.
 */
static void test_framebuffer_pixel_reaches_display_and_screendump(void)
{
    QTestState *qts = board_start("");

    /* The screen (id plane 0) in an RGB mode. */
    display_identity(qts, 0);

    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 5), ==, 0);

    odsy_test_pixel_write(qts, 5, 5, 0x11223344);
    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0x11223344);
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 5), ==, 0x00112233);
    g_assert_cmphex(odsy_screendump_pixel(qts, 5, 5), ==, 0x00112233);

    /* An untouched pixel elsewhere stays at its post-reset value. */
    g_assert_cmphex(odsy_test_pixel(qts, 6, 5), ==, 0);
    g_assert_cmphex(odsy_screendump_pixel(qts, 6, 5), ==, 0);

    board_quit(qts);
}

/*
 * The display channel's active size. The kernel and the X server write the
 * selected video format's active pixels and lines to backend entries 0x2802
 * and 0x2803, with the channel origin and a zero word after them: 1280 and
 * 1024 at boot, 1280 and 720 once a V6's X server loads 1280x720_72, the
 * format gfxinfo then reports. They write zeros there while a format
 * changes. The scanout presents the latest nonzero size, kept within the
 * modeled planes, and reset returns it to 1280x1024.
 */
static void test_display_channel_size_sets_scanout(void)
{
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    char *size;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-channel-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);
    display_identity(qts, 0);

    size = odsy_screendump_size(qts);
    g_assert_cmpstr(size, ==, "1280 1024");
    g_free(size);

    backend_push32(qts, ODSY_BACKEND_HDR(0x2802, 5));
    backend_push32(qts, 1280);
    backend_push32(qts, 720);
    backend_push32(qts, 0);
    backend_push32(qts, 0);
    backend_push32(qts, 0);
    size = odsy_screendump_size(qts);
    g_assert_cmpstr(size, ==, "1280 720");
    g_free(size);
    odsy_test_pixel_write(qts, 5, 5, 0x11223344);
    g_assert_cmphex(odsy_screendump_pixel_sized(qts, 5, 5, 1280, 720), ==,
                    0x00112233);

    /* The zeros of a format change leave the picture as it was. */
    backend_push32(qts, ODSY_BACKEND_HDR(0x2802, 2));
    backend_push32(qts, 0);
    backend_push32(qts, 0);
    size = odsy_screendump_size(qts);
    g_assert_cmpstr(size, ==, "1280 720");
    g_free(size);

    /* A narrower channel presents the left of each line. */
    backend_push32(qts, ODSY_BACKEND_HDR(0x2802, 2));
    backend_push32(qts, 1024);
    backend_push32(qts, 768);
    size = odsy_screendump_size(qts);
    g_assert_cmpstr(size, ==, "1024 768");
    g_free(size);
    g_assert_cmphex(odsy_screendump_pixel_sized(qts, 5, 5, 1024, 768), ==,
                    0x00112233);

    /* Larger than the modeled planes: kept to them and reported. */
    backend_push32(qts, ODSY_BACKEND_HDR(0x2802, 2));
    backend_push32(qts, 1600);
    backend_push32(qts, 1200);
    size = odsy_screendump_size(qts);
    g_assert_cmpstr(size, ==, "1280 1024");
    g_free(size);

    backend_push32(qts, ODSY_BACKEND_HDR(0x2803, 1));
    backend_push32(qts, 720);
    qtest_system_reset(qts);
    size = odsy_screendump_size(qts);
    g_assert_cmpstr(size, ==, "1280 1024");
    g_free(size);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "display channel 1600x768 is larger than "
                                 "the modeled 1280x1024 planes"));
    g_assert_nonnull(strstr(log, "display channel 1280x1200 is larger than "
                                 "the modeled 1280x1024 planes"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * Reset clears the framebuffer (odyssey_reset_hold(), hw/misc/sgi-odyssey.c)
 * along with the registers test_reset_restores_the_documented_values
 * already covers.
 */
static void test_reset_clears_the_framebuffer(void)
{
    QTestState *qts = board_start("");

    odsy_test_pixel_write(qts, 5, 5, 0x11223344);
    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0x11223344);

    qtest_system_reset(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 5), ==, 0);

    board_quit(qts);
}

/*
 * The x/y selector properties are bounded by SGI_ODYSSEY_WIDTH/HEIGHT
 * (include/hw/misc/sgi-odyssey.h): an out-of-range coordinate reads a
 * defined zero rather than reading out of the allocation, and a write to
 * one is rejected instead of silently discarded or corrupting a
 * neighboring row.
 */
static void test_framebuffer_pixel_accessor_bounds(void)
{
    QTestState *qts = board_start("");
    QDict *resp;

    g_assert_cmphex(odsy_test_pixel(qts, 1280, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 1024), ==, 0);

    odsy_test_pixel_select(qts, 1280, 0);
    resp = qtest_qmp(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel', 'value': %u}}", BOARD, 0xffffffffu);
    g_assert_true(qdict_haskey(resp, "error"));
    qobject_unref(resp);

    board_quit(qts);
}

/*
 * odyssey_fillrect(): BEGIN(QUADS), a color, four corner vertices and END,
 * closed out with the quick flush a driver always issues after one draw. The
 * command FIFO is the only path that paints outside the test-only pixel
 * accessor, so this is also the first proof that path reaches the framebuffer
 * at all.
 */
/*
 * The kernel's window-placement block, pushed on the kernel port the way the
 * kernel driver's clip-rectangle push does after every context switch: 0x841
 * with the window origin and 0x204b with the four clip rectangles, one
 * 8-byte store each. A rectangle is x0 << 12 | y0 then x1 << 12 | y1,
 * inclusive; (1, 1) .. (0, 0) is empty.
 */
#define ODSY_CMD_WINDOW_ORIGIN 0x00010841u
#define ODSY_CMD_DID_COMPARE 0x00010840u
#define ODSY_CMD_WINDOW_CLIP 0x0001204bu
#define ODSY_CLIP_WORD(x, y) (((uint32_t)(x) << 12) | (y))
#define ODSY_CLIP_EMPTY_LO 0x00001001u

static void window_origin(QTestState *qts, uint32_t x, uint32_t y)
{
    cmd_push(qts, ODSY_CMD_WINDOW_ORIGIN);
    cmd_push(qts, x);
    cmd_push(qts, y);
}

/* Up to four inclusive rectangles {x0, y0, x1, y1}; the rest empty. */
static void window_clip(QTestState *qts, const uint32_t (*r)[4], unsigned n)
{
    unsigned i;

    cmd_push(qts, ODSY_CMD_WINDOW_CLIP);
    for (i = 0; i < 4; i++) {
        if (i < n) {
            cmd_push64(qts, ODSY_CLIP_WORD(r[i][0], r[i][1]),
                       ODSY_CLIP_WORD(r[i][2], r[i][3]));
        } else {
            cmd_push64(qts, ODSY_CLIP_EMPTY_LO, 0);
        }
    }
}

/* One solid RGB QUADS fill of the half-open box [x0, x1) x [y0, y1). */
static void rgb_fill(QTestState *qts, uint8_t r, uint8_t g, uint8_t b,
                     uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    cmd_push(qts, ODSY_CMD_GL_COLOR_3UB);
    cmd_push(qts, r);
    cmd_push(qts, g);
    cmd_push(qts, b);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x0);
    cmd_push(qts, y0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x1);
    cmd_push(qts, y0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x1);
    cmd_push(qts, y1);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x0);
    cmd_push(qts, y1);
    cmd_push(qts, ODSY_CMD_GL_END);
}

static void test_command_fifo_fillrect(void)
{
    QTestState *qts = board_start("");

    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    cmd_push(qts, ODSY_CMD_GL_COLOR_3UB);
    cmd_push(qts, 0x11);
    cmd_push(qts, 0x22);
    cmd_push(qts, 0x33);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 10);
    cmd_push(qts, 20);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 40);
    cmd_push(qts, 20);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 40);
    cmd_push(qts, 60);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 10);
    cmd_push(qts, 60);
    cmd_push(qts, ODSY_CMD_GL_END);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);

    g_assert_cmphex(odsy_test_pixel(qts, 10, 20), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 39, 59), ==, 0x112233ff);
    /* RGB shows through an RGB mode (drawable 0, the id plane at reset). */
    display_identity(qts, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 10, 20), ==, 0x00112233);
    /* Outside the rectangle on every side, and just past its far corner. */
    g_assert_cmphex(odsy_test_pixel(qts, 9, 20), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 19), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 20), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 60), ==, 0);

    /*
     * Under a 0x204b clip smaller than the quad, only the intersection is
     * painted: (100, 100) .. (119, 119) inclusive out of [90, 130).
     */
    window_clip(qts, (const uint32_t[][4]) { { 100, 100, 119, 119 } }, 1);
    rgb_fill(qts, 0x44, 0x55, 0x66, 90, 90, 130, 130);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 119, 119), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 99, 110), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 110, 99), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 120, 110), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 110, 120), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 90, 90), ==, 0);

    /*
     * The window origin moves a 2D integer vertex too: with the whole screen
     * kept and the origin at (500, 500), the box [0, 4) lands at
     * [500, 504).
     */
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    window_origin(qts, 500, 500);
    rgb_fill(qts, 0x77, 0x88, 0x99, 0, 0, 4, 4);
    g_assert_cmphex(odsy_test_pixel(qts, 500, 500), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 503, 503), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 504, 503), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);

    board_quit(qts);
}

/*
 * The same fillrect sequence test_command_fifo_fillrect drives, paired
 * two words per store instead of one word per store. The command FIFO
 * accepts 64-bit stores too: each carries the next two command words, upper
 * half first (odyssey_cmd_fifo_write(), hw/misc/sgi-odyssey.c) -- an
 * approximation, since no allowed source independently establishes which half
 * is logically first. Driving the identical sequence this way must reach
 * the same framebuffer result regardless, since the parser consumes one
 * word at a time either way. Twenty-three words split unevenly across
 * eleven 64-bit stores and one trailing 32-bit store, proving the two
 * widths interleave rather than only alternating in lockstep.
 */
static void test_command_fifo_accepts_64bit_stores(void)
{
    QTestState *qts = board_start("");

    cmd_push64(qts, ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS);
    cmd_push64(qts, ODSY_CMD_GL_COLOR_3UB, 0x11);
    cmd_push64(qts, 0x22, 0x33);
    cmd_push64(qts, ODSY_CMD_GL_VERTEX_2I, 10);
    cmd_push64(qts, 20, ODSY_CMD_GL_VERTEX_2I);
    cmd_push64(qts, 40, 20);
    cmd_push64(qts, ODSY_CMD_GL_VERTEX_2I, 40);
    cmd_push64(qts, 60, ODSY_CMD_GL_VERTEX_2I);
    cmd_push64(qts, 10, 60);
    cmd_push64(qts, ODSY_CMD_GL_END, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push64(qts, ODSY_CMD_QUICK_FLUSH_ARG, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);

    g_assert_cmphex(odsy_test_pixel(qts, 10, 20), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 39, 59), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 20), ==, 0);

    board_quit(qts);
}

/*
 * odyssey_copyrect(): the screen-to-screen blit console scrolling relies
 * on. A distinct color per source pixel proves the copy is pixel-exact,
 * not just a bounding-box fill, and the untouched source proves a
 * non-overlapping copy never disturbs it.
 */
static void test_command_fifo_copyrect(void)
{
    QTestState *qts = board_start("");
    const uint32_t sx = 5, sy = 5, w = 3, h = 2, dx = 50, dy = 60;
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            odsy_test_pixel_write(qts, sx + x, sy + y,
                                  0x10101010 * (y * w + x + 1));
        }
    }

    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (sy << 16) | sx);   /* SRC */
    cmd_push(qts, 0x80502050);
    cmd_push(qts, (h << 16) | w);     /* SIZE */
    cmd_push(qts, 0x82223042);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (dy << 16) | dx);   /* DST */
    cmd_push(qts, 0x3222204b);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            g_assert_cmphex(odsy_test_pixel(qts, dx + x, dy + y), ==,
                             0x10101010 * (y * w + x + 1));
        }
    }
    /* The source is undisturbed by a non-overlapping copy. */
    g_assert_cmphex(odsy_test_pixel(qts, sx, sy), ==, 0x10101010);

    board_quit(qts);
}

/*
 * A copy whose destination overlaps and is below its source -- the shape
 * a console-scroll relies on -- must read every row before the copy's
 * own writes reach it, the same guarantee memmove() gives overlapping
 * ranges (odyssey_copy_rect(), hw/misc/sgi-odyssey.c). Four distinct row
 * colors make a wrong iteration order (copying top-to-bottom over an
 * overlapping range, which would duplicate row 0 downward instead)
 * visible.
 */
static void test_command_fifo_copyrect_overlapping_shift(void)
{
    QTestState *qts = board_start("");
    uint32_t row;

    for (row = 0; row < 4; row++) {
        odsy_test_pixel_write(qts, 0, row, 0x01000000 * (row + 1));
    }

    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, 0);              /* SRC: (0, 0) */
    cmd_push(qts, 0x80502050);
    cmd_push(qts, (4u << 16) | 1); /* SIZE: 1 wide, 4 tall */
    cmd_push(qts, 0x82223042);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (1u << 16) | 0); /* DST: (0, 1) */
    cmd_push(qts, 0x3222204b);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);

    /* Row 0 is untouched; rows 1-4 hold what rows 0-3 held beforehand. */
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0x01000000);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 1), ==, 0x01000000);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 2), ==, 0x02000000);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 3), ==, 0x03000000);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 4), ==, 0x04000000);

    board_quit(qts);
}

/* A maximal copy still visits only destination pixels on the plane. */
static void test_command_fifo_copyrect_large_offscreen(void)
{
    QTestState *qts = board_start("");
    static const struct {
        uint32_t src, dst;
    } copies[] = {
        { 0, UINT32_MAX },
        { (1021u << 16) | 1277, (1022u << 16) | 1278 },
    };

    odsy_test_pixel_write(qts, 1277, 1021, 0x11111111);
    odsy_test_pixel_write(qts, 1278, 1021, 0x22222222);
    odsy_test_pixel_write(qts, 1277, 1022, 0x33333333);
    odsy_test_pixel_write(qts, 1278, 1022, 0x44444444);

    for (unsigned i = 0; i < G_N_ELEMENTS(copies); i++) {
        g_test_timer_start();
        cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
        cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
        cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
        cmd_push(qts, 0x00002000);
        cmd_push(qts, copies[i].src);
        cmd_push(qts, 0x80502050);
        cmd_push(qts, UINT32_MAX);
        cmd_push(qts, 0x82223042);
        cmd_push(qts, 0x00002000);
        cmd_push(qts, copies[i].dst);
        cmd_push(qts, 0x3222204b);
        cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
        cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
        cmd_push(qts, ODSY_CMD_STROBE);
        cmd_push(qts, ODSY_CMD_STROBE);
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 5.0);
    }

    g_assert_cmphex(odsy_test_pixel(qts, 1278, 1022), ==, 0x11111111);
    g_assert_cmphex(odsy_test_pixel(qts, 1279, 1022), ==, 0x22222222);
    g_assert_cmphex(odsy_test_pixel(qts, 1278, 1023), ==, 0x33333333);
    g_assert_cmphex(odsy_test_pixel(qts, 1279, 1023), ==, 0x44444444);
    board_quit(qts);
}

/*
 * A screen-to-screen copy is not clipped by the window-clip rectangles:
 * IRIX 6.5.30's X server issues copies whose destinations
 * lie wholly outside the rectangles in force and expects them to land, which
 * is what moving a window relies on. Both the X server's own tail words and
 * OpenBSD's literal ones (test_command_fifo_copyrect's) must still land under
 * a clip disjoint from the destination. An ordinary fill is still clipped:
 * the fix must drop only the
 * rectangle test on the copy path, not everywhere.
 */
static void test_command_fifo_copyrect_ignores_window_clip(void)
{
    QTestState *qts = board_start("");
    const uint32_t sx = 5, sy = 5, w = 3, h = 2, dx = 50, dy = 60;
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            odsy_test_pixel_write(qts, sx + x, sy + y,
                                  0x10101010 * (y * w + x + 1));
        }
    }

    /* Disjoint from both the source above and the destination below. */
    window_clip(qts, (const uint32_t[][4]) { { 400, 500, 549, 649 } }, 1);

    /* The X server's own block, traced in the window-move diagnosis. */
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (sy << 16) | sx);   /* SRC */
    cmd_push(qts, 0x80503050);
    cmd_push(qts, (h << 16) | w);     /* SIZE */
    cmd_push(qts, 0x83331008);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (dy << 16) | dx);   /* DST */
    cmd_push(qts, 0x32330041);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            g_assert_cmphex(odsy_test_pixel(qts, dx + x, dy + y), ==,
                             0x10101010 * (y * w + x + 1));
        }
    }

    /* OpenBSD's literal tail words, same destination, same disjoint clip. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            odsy_test_pixel_write(qts, dx + x, dy + y, 0);
        }
    }
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (sy << 16) | sx);   /* SRC */
    cmd_push(qts, 0x80502050);
    cmd_push(qts, (h << 16) | w);     /* SIZE */
    cmd_push(qts, 0x82223042);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (dy << 16) | dx);   /* DST */
    cmd_push(qts, 0x3222204b);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            g_assert_cmphex(odsy_test_pixel(qts, dx + x, dy + y), ==,
                             0x10101010 * (y * w + x + 1));
        }
    }

    /*
     * Control: an ordinary fill is still clipped by the disjoint
     * rectangle -- the fix must not drop the rectangle test everywhere.
     */
    window_clip(qts, (const uint32_t[][4]) { { 400, 500, 549, 649 } }, 1);
    rgb_fill(qts, 0x44, 0x55, 0x66, 100, 100, 110, 110);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, 0);

    board_quit(qts);
}

/*
 * ODSY_CMD_COPY_RECT_PREFIX (0x00010658) is an ordinary one-operand
 * register-select, not a blit trigger: the model used to key the whole
 * fixed-length blit state off it directly, which meant a driver command
 * that followed a copy in the real stream -- ODSY_CMD_HOST_WRITE_SELECT
 * among them -- got silently consumed as blit operands instead of parsed.
 * Pushing the prefix and its one operand, then a word this grammar does
 * not otherwise recognize, must leave the parser idle: the unrecognized
 * word is reported rather than swallowed.
 */
static void test_command_fifo_copyrect_prefix_is_a_plain_register(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-copyrect-prefix-XXXXXX.log", &log_path,
                          &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    /* Idle again: a word with no recognized shape is reported, not held. */
    cmd_push(qts, 0x00007777);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log,
                            "command FIFO word 0x00007777 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The host-write/sync command (ODSY_CMD_HOST_WRITE_SELECT, register
 * 0x0a5a) the kernel driver's DMA completion wait uses: a DMA write of
 * the command's own 16-bit token into guest RAM at the host-write base
 * plus twice the command's slot number, and, only when the command's own
 * interrupt bit is set, a one-shot softsync packet through
 * ODSY_ROUTE_DEST_UPPER/_LOWER/_VECTORS -- a separate, edgeless delivery
 * from the level-based interrupt test_interrupt_packet_composition pins
 * above. That same wait's interrupt handler is the only place that polls
 * the written halfword, so a real command reaching this register needs
 * both effects to unblock the driver (its 15 s DMA wait otherwise).
 * The base and slot are a worked trace's own values. board_start() halts
 * the CPU (-S), so nothing ever runs PROM's own memory-controller setup;
 * the explicit BEDROCK_MD_MEMORY_CONFIG write below installs a bank the
 * same way tests/qtest/sn1-address-map-test.c's machine_configure_memory()
 * does, so the DMA target resolves as RAM instead of an unassigned hole.
 */
static void test_command_fifo_host_write_sync(void)
{
    QTestState *qts = board_start("");
    const uint64_t base = 0xb70000;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);

    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, (uint32_t)base);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);

    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0);

    /* Slot 2, no interrupt bit: the write lands, but nothing is sent. */
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 2);
    cmd_push(qts, 0x5a00);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0x5a00);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);

    /* Slot 3, interrupt bit set: a second write, and the softsync packet. */
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, ODSY_CMD_HOST_WRITE_INTERRUPT | 3);
    cmd_push(qts, 0xa5a5);
    g_assert_cmphex(qtest_readw(qts, base + 6), ==, 0xa5a5);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    ODSY_TEST_PACKET_FABRIC_ADDRESS);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_SOFTSYNC_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    board_quit(qts);
}

/*
 * The way the kernel driver's DMA descriptor builder packs one 24-bit
 * half of a DMA table address: bits 16:0 stay, bits 23:17 move to 29:23.
 */
static uint32_t dma_pack_half(uint64_t half)
{
    return (half & 0x1ffff) | ((half & 0xfe0000) << 6);
}

/*
 * Push a two-doubleword DMA descriptor the way the driver builds it: the
 * opcode over the packed high half of the table address, then the packed low
 * half over a zero word.
 */
static void dma_descriptor(QTestState *qts, uint32_t opcode, uint64_t table)
{
    cmd_push64(qts, opcode, dma_pack_half(table >> 24));
    cmd_push64(qts, dma_pack_half(table & 0xffffff), 0);
}

/*
 * The page-table pixel DMA engine (hw/misc/sgi-odyssey.c). Lay a
 * scatter-gather buffer and its translation table in guest RAM, push a 0xe01
 * descriptor, and check the whole buffer lands, across the page boundary,
 * in the off-screen object the header's word 0 names, 16 texels a row. The
 * table address is packed as the driver packs it; the addresses here have
 * bits 23:17 set, which the packing moves, and the second table sits above
 * 16 MiB, so the high half is non-zero too. The first entry starts part-way
 * into its page, as the driver's tables for a buffer that is not
 * page-aligned do. The descriptor writes no fence (the driver follows it
 * with a 0x0a5a host-write for that) and raises the DMA-done interrupt. Then
 * a descriptor whose entries disagree with its header fails without landing
 * anything.
 *
 * board_start() halts the CPU (-S), so the explicit BEDROCK_MD_MEMORY_CONFIG
 * write installs a bank the same way test_command_fifo_host_write_sync does, so
 * the table, the buffer pages and the fence slot all resolve as RAM.
 */
/*
 * A DMA header naming a 16-pixel-wide RGBA8 object at ODSY_DMA_TEST_OBJECT:
 * origin (0, 0), rows rows of 64 bytes, 4 bytes a pixel (code 1 in bits
 * 18:16), source format 0x4b, and the DMA-done request when irq is set.
 */
#define ODSY_DMA_TEST_OBJECT 0x0016a800u

static void dma_object_header(uint8_t *tbl, uint32_t rows, bool irq)
{
    stl_be_p(&tbl[0x00], ODSY_DMA_TEST_OBJECT);
    stl_be_p(&tbl[0x04], 0);
    stl_be_p(&tbl[0x08], 1);
    stl_be_p(&tbl[0x0c], (rows << 16) | 16);
    stl_be_p(&tbl[0x18], 64);
    stl_be_p(&tbl[ODSY_DMA_HDR_CONTROL],
             (irq ? ODSY_DMA_IRQ_REQUEST : 0) | (1u << 16) | 0x4b);
}

/* Word i of the test object, 16 texels a row. */
static uint32_t odsy_test_dma_word(QTestState *qts, uint32_t i)
{
    return odsy_test_object_texel(qts, ODSY_DMA_TEST_OBJECT, i % 16, i / 16);
}

static void test_command_fifo_dma_trace(void)
{
    const uint64_t table = 0xb80000, source = 0xb84000;
    uint8_t pixels[4224], tbl[ODSY_DMA_TABLE_HEADER + 2 * 8] = { 0 };
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    uint64_t last = 0, kick = 0;
    unsigned accesses = 0, chunks = 0, payload = 0;
    bool read_seen = false, write_seen = false;
    int fd = g_file_open_tmp("odyssey-dma-trace-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    args = g_strdup_printf(" -trace enable=sgi_odyssey_mmio"
                            " -trace enable=sgi_odyssey_dma_chunk -D %s", path);
    qts = board_start(args);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    for (unsigned i = 0; i < sizeof(pixels); i++) {
        pixels[i] = i;
    }
    dma_object_header(tbl, 1, false);
    stl_be_p(tbl + 8, 66);
    stl_be_p(tbl + 12, (1u << 16) | 1056);
    stl_be_p(tbl + 24, sizeof(pixels));
    stq_be_p(tbl + ODSY_DMA_TABLE_HEADER,
             ODSY_DMA_PTE(source, sizeof(pixels)));
    qtest_memwrite(qts, table, tbl, sizeof(tbl));
    qtest_memwrite(qts, source, pixels, sizeof(pixels));
    reg_write(qts, ODSY_CONTROL, 11);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_test_dma_word(qts, 0), ==, 0x00010203);
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    board_quit(qts);
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        char *event = strstr(lines[i], "sgi_odyssey_mmio seq ");
        uint64_t seq, addr, value;
        unsigned size, write;

        if (event) {
            g_assert_cmpint(sscanf(event, "sgi_odyssey_mmio seq %" SCNu64
                                   " offset 0x%" SCNx64 " size %u value 0x%"
                                   SCNx64 " write=%u", &seq, &addr, &size,
                                   &value, &write), ==, 5);
            if (accesses++) {
                g_assert_cmpuint(seq, ==, last + 1);
            }
            last = seq;
            if (addr == ODSY_IDENTITY && !write) {
                g_assert_cmpuint(size, ==, 4);
                g_assert_cmphex(value, ==, IDENTITY_DEFAULT);
                read_seen = true;
            }
            if (addr == ODSY_CONTROL && write) {
                g_assert_cmpuint(size, ==, 4);
                g_assert_cmphex(value, ==, 11);
                write_seen = true;
            }

            if (addr == 0x110000 && size == 8 &&
                value == UINT64_C(0x2e00000000000000) && write) {
                kick = seq;
                g_test_message("%s", event);
            }
            continue;
        }
        event = strstr(lines[i], "sgi_odyssey_dma_chunk seq ");
        if (event) {
            unsigned len;
            int at = 0;
            const char *hex;

            g_assert_cmpint(sscanf(event, "sgi_odyssey_dma_chunk seq %" SCNu64
                                   " addr 0x%" SCNx64 " len %u hex %n",
                                   &seq, &addr, &len, &at), ==, 3);
            g_assert_cmpuint(seq, ==, kick);
            g_assert_cmpuint(seq, ==, last);
            g_assert_cmpuint(len, <=, 4096);
            hex = event + at;
            g_assert_cmpuint(strlen(hex), ==, len * 2);
            for (unsigned j = 0; j < len * 2; j++) {
                g_assert_true(g_ascii_isdigit(hex[j]) ||
                               (hex[j] >= 'a' && hex[j] <= 'f'));
            }
            if (addr >= source && addr < source + sizeof(pixels)) {
                g_assert_cmpuint(addr, ==, source + payload);
                for (unsigned j = 0; j < len; j++) {
                    unsigned byte = (g_ascii_xdigit_value(hex[j * 2]) << 4) |
                                     g_ascii_xdigit_value(hex[j * 2 + 1]);

                    g_assert_cmpuint(byte, ==, pixels[payload + j]);
                }
                payload += len;
            } else {
                g_assert_cmpuint(addr, >=, table);
                g_assert_cmpuint(addr + len, <=, table + sizeof(tbl));
                for (unsigned j = 0; j < len; j++) {
                    unsigned byte = (g_ascii_xdigit_value(hex[j * 2]) << 4) |
                                     g_ascii_xdigit_value(hex[j * 2 + 1]);

                    g_assert_cmpuint(byte, ==, tbl[addr - table + j]);
                }
            }
            if (!chunks) {
                g_test_message("%s", event);
            }
            chunks++;
        }
    }
    g_assert_true(read_seen);
    g_assert_true(write_seen);
    g_assert_cmpuint(accesses, >=, 3);
    /* Header, two entries, two payload pages. */
    g_assert_cmpuint(chunks, ==, 5);
    g_assert_cmpuint(payload, ==, sizeof(pixels));
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_command_fifo_dma_page_table(void)
{
    QTestState *qts = board_start("");
    const uint64_t fence_base = 0xb70000;
    const uint64_t table = 0xb80000;
    const uint64_t table_high = 0x1fe5300; /* high half 0x1, low 0xfe5300 */
    const uint64_t page0 = 0xb84000;    /* a 16 KiB page, used from 0x3000 */
    const uint64_t page1 = 0xb88000;    /* a partial trailing page */
    const uint32_t first_off = 0x3000;
    const uint32_t first_len = ODSY_DMA_PAGE_SIZE - first_off;
    const uint32_t page1_len = 0x40;    /* 16 pixels in the second page */
    g_autofree uint8_t *buf0 = g_malloc(first_len);
    uint8_t buf1[0x40];
    uint8_t tbl[ODSY_DMA_TABLE_HEADER + 3 * 8];
    unsigned i;

    g_assert_cmphex(dma_pack_half(table >> 24), ==, 0);
    g_assert_cmphex(dma_pack_half(table & 0xffffff), ==, 0x2e000000);

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);

    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, (uint32_t)fence_base);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);

    /* A distinct big-endian word per pixel, so a misordered fetch shows. */
    for (i = 0; i < first_len / 4; i++) {
        stl_be_p(&buf0[i * 4], 0xaa000000u | i);
    }
    for (i = 0; i < page1_len / 4; i++) {
        stl_be_p(&buf1[i * 4], 0xbb000000u | i);
    }
    qtest_memwrite(qts, page0 + first_off, buf0, first_len);
    qtest_memwrite(qts, page1, buf1, page1_len);

    /*
     * The table: a 0x20 header, two entries, a zero-length terminator. The
     * header names an object no 0x1017 selected, so the transfer creates it,
     * and asks for the DMA-done interrupt.
     */
    memset(tbl, 0, sizeof(tbl));
    dma_object_header(tbl, (first_len + page1_len) / 64, true);
    stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER + 0 * 8],
             ODSY_DMA_PTE(page0 + first_off, first_len));
    stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER + 1 * 8],
             ODSY_DMA_PTE(page1, page1_len));
    stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER + 2 * 8], 0);
    qtest_memwrite(qts, table, tbl, sizeof(tbl));

    g_assert_cmphex(odsy_test_dma_word(qts, 0), ==, 0);

    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);

    /* The whole buffer landed in the object, both pages in order. */
    g_assert_cmphex(odsy_test_dma_word(qts, 0), ==, 0xaa000000u);
    g_assert_cmphex(odsy_test_dma_word(qts, 1), ==, 0xaa000001u);
    g_assert_cmphex(odsy_test_dma_word(qts, first_len / 4 - 1), ==,
                    0xaa000000u | (first_len / 4 - 1));
    /* The first pixel of the second page, right after the page boundary. */
    g_assert_cmphex(odsy_test_dma_word(qts, first_len / 4), ==,
                    0xbb000000u);
    g_assert_cmphex(odsy_test_dma_word(qts,
                    first_len / 4 + page1_len / 4 - 1), ==,
                    0xbb000000u | (page1_len / 4 - 1));
    /* One texel past the transfer holds nothing. */
    g_assert_cmphex(odsy_test_dma_word(qts,
                    first_len / 4 + page1_len / 4), ==, 0);

    /* No fence: slot 0 of the host-write base is untouched, no softsync. */
    g_assert_cmphex(qtest_readw(qts, fence_base), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) &
                    (UINT64_C(1) << ODSY_TEST_SOFTSYNC_VECTOR), ==, 0);
    /* ...and the DMA-done source fired. */
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_DMA_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    /*
     * A table above 16 MiB, one row: the high half of the address reaches the
     * decode, and the row lands at the object's start.
     */
    memset(tbl, 0, sizeof(tbl));
    dma_object_header(tbl, 1, false);
    stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER], ODSY_DMA_PTE(page1, page1_len));
    qtest_memwrite(qts, table_high, tbl, sizeof(tbl));
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table_high);
    g_assert_cmphex(odsy_test_dma_word(qts, 0), ==, 0xbb000000u);
    g_assert_cmphex(odsy_test_dma_word(qts, page1_len / 4 - 1), ==,
                    0xbb000000u | (page1_len / 4 - 1));

    board_quit(qts);

    /*
     * A descriptor whose entries disagree with its header -- one entry
     * claiming more bytes than the header's rectangle -- fails the transfer
     * before anything lands. The board still consumed the descriptor, so
     * the DMA-done source the header asks for fires (hw/misc/sgi-odyssey.c,
     * odyssey_dma_done()).
     */
    qts = board_start("");
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);

    memset(tbl, 0, sizeof(tbl));
    dma_object_header(tbl, 1, true);
    /* One page, but 0xffff bytes: more than the header's one row. */
    stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER], ODSY_DMA_PTE(page0, 0xffff));
    stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER + 8], 0);
    qtest_memwrite(qts, table, tbl, sizeof(tbl));
    qtest_memwrite(qts, page0, buf1, sizeof(buf1));

    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);

    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_DMA_VECTOR | ODSY_HUB_INT_MODIFY_SET);
    g_assert_cmphex(odsy_test_dma_word(qts, 0), ==, 0);

    board_quit(qts);

    /*
     * Before the driver programs the routing, a consumed descriptor sends
     * nothing: there is nowhere to send the DMA-done packet.
     */
    qts = board_start("");
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    qtest_memwrite(qts, table, tbl, sizeof(tbl));
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==, 0);

    board_quit(qts);
}

/*
 * A DMA into the colour buffer: lay a table whose header names the buffer the
 * X server's 0x1017 block named, its rectangle, row length, row gap and pixel
 * size, and check the pixels land where the header says. Word 7 carries the
 * interrupt request.
 */
static void dma_image_table(QTestState *qts, uint64_t table,
                            const uint32_t hdr[8], const uint64_t *pte,
                            unsigned n)
{
    uint8_t tbl[ODSY_DMA_TABLE_HEADER + 16 * 8];
    unsigned i;

    g_assert_cmpuint(n, <, 16);
    memset(tbl, 0, sizeof(tbl));
    for (i = 0; i < 8; i++) {
        stl_be_p(&tbl[i * 4], hdr[i]);
    }
    for (i = 0; i < n; i++) {
        stq_be_p(&tbl[ODSY_DMA_TABLE_HEADER + i * 8], pte[i]);
    }
    qtest_memwrite(qts, table, tbl, sizeof(tbl));
}

static void test_command_fifo_dma_image(void)
{
    QTestState *qts = board_start("");
    const uint64_t table = 0xb80000;
    const uint64_t page_a = 0xb84000;
    const uint64_t page_b = 0xb88000;
    /*
     * A 3x2 image of 4-byte pixels at (200, 300) with a 4-byte gap after each
     * row: row 0 starts 8 bytes before the end of page A and finishes in page
     * B, then the gap, then row 1.
     */
    uint32_t hdr[8] = {
        0x2000, (300 << 16) | 200, 0x50, (2 << 16) | 3, 0x8223305a, 4, 12,
        0x32010000,
    };
    const uint64_t pte[] = {
        ODSY_DMA_PTE(page_a + 0x3ff8, 8), ODSY_DMA_PTE(page_b, 16),
    };
    uint8_t a[8], b[20];
    const uint8_t idx[4] = { 1, 2, 0, 2 };
    const uint64_t pte8[] = { ODSY_DMA_PTE(page_a, 4) };
    uint32_t hdr8[8] = {
        0x2000, (310 << 16) | 200, 0x50, (1 << 16) | 4, 0x83331008, 0, 4,
        0x32050000,
    };
    uint32_t hdr_obj[8] = {
        0x16a000, 0, 0x10, (1 << 16) | 3, 0x80025000, 0, 12, 0x32010000,
    };
    const uint64_t pte_obj[] = { ODSY_DMA_PTE(page_b, 12) };

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);

    /* Drawable 1 -> group 17; index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);

    /* The X server's colour destination-select names buffer 0x2000. */
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x2000);
    user_cmd_push(qts, 0x50);
    user_cmd_push(qts, 0x20006950);
    user_cmd_push(qts, 0);

    stl_be_p(&a[0], 0x00ff0000);
    stl_be_p(&a[4], 0x0000ff00);
    stl_be_p(&b[0], 0x000000ff);
    stl_be_p(&b[4], 0xdeadbeef);            /* the row gap: never a pixel */
    stl_be_p(&b[8], 0x00112233);
    stl_be_p(&b[12], 0x00445566);
    stl_be_p(&b[16], 0x00778899);
    qtest_memwrite(qts, page_a + 0x3ff8, a, sizeof(a));
    qtest_memwrite(qts, page_b, b, sizeof(b));
    dma_image_table(qts, table, hdr, pte, 2);

    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);

    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 300), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 202, 300), ==, 0x0000ffff);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 301), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 301), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 202, 301), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 203, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 302), ==, 0);
    /* The header did not ask for the interrupt: nothing was sent. */
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);
    g_assert_cmphex(odsy_test_dma_word(qts, 0), ==, 0);

    /* The same transfer asking for it: the pixels again, and the packet. */
    hdr[7] |= ODSY_DMA_IRQ_REQUEST;
    dma_image_table(qts, table, hdr, pte, 2);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 202, 301), ==, 0x778899ff);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_DMA_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    /*
     * One byte a pixel: colour indices, stored raw and shown through the
     * pixel's drawable (id plane 0, group 17). Index 0 is
     * written like any other over what was there.
     */
    odsy_test_pixel_write(qts, 202, 310, 1);
    qtest_memwrite(qts, page_a, idx, sizeof(idx));
    dma_image_table(qts, table, hdr8, pte8, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 310), ==, 1);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 310), ==, 2);
    g_assert_cmphex(odsy_test_pixel(qts, 202, 310), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 204, 310), ==, 0);
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    g_assert_cmphex(odsy_shown(qts, 200, 310), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, 201, 310), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 202, 310), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_shown(qts, 203, 310), ==, 0x0000ffff);

    /*
     * Another destination -- the greeter's off-screen object at 0x16a000,
     * 256 pixels a row by word 2 -- lands in that object and leaves the plane
     * alone. Its word 7 carries no format code, so the 4-byte pixels keep the
     * 0x00RRGGBB rule.
     */
    dma_image_table(qts, table, hdr_obj, pte_obj, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a000, 0, 0), ==,
                    0x0000ffff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a000, 1, 0), ==,
                    0xadbeefff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a000, 2, 0), ==,
                    0x112233ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a000, 3, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0xff0000ff);

    board_quit(qts);
}

/*
 * The double-buffered swap (hw/misc/sgi-odyssey.c). The driver issues a swap
 * by writing ODSY_SWAP_ARM, a per-window toggle mask, and then waits for its
 * vertical-retrace ISR, which completes window w's swap only when bit w of
 * ODSY_DISPLAYED_BUFFER shows the buffer it asked for. The ISR is bound to
 * byte 23:16 of ODSY_ROUTE_VECTORS; the top byte belongs to the softsync ISR.
 *
 * board_start() halts the CPU (-S), which pauses QEMU_CLOCK_VIRTUAL, so the
 * scanout's retrace timer never reaches a frame here. x-test-fire-retrace
 * fires exactly the hook that timer drives; the per-frame timer wiring itself
 * is the foundation's and is covered by a running guest (the swap-probe boot).
 */
static void fire_retrace(QTestState *qts)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-fire-retrace', 'value': true}}", BOARD);
}

static void route_to_pending(QTestState *qts)
{
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);
}

/*
 * The retrace interrupt carries the retrace byte's vector, not the softsync
 * byte's, and fires at every retrace once routing is programmed whether or
 * not a swap is armed -- the driver's vblank counters and interval-deferred
 * swaps advance only in that ISR. Before routing, a retrace sends nothing.
 */
static void test_swap_retrace_vector(void)
{
    QTestState *qts = board_start("");

    fire_retrace(qts);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);

    route_to_pending(qts);
    fire_retrace(qts);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    ODSY_TEST_PACKET_FABRIC_ADDRESS);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_RETRACE_VECTOR | ODSY_HUB_INT_MODIFY_SET);
    /* At the hub: the retrace ISR's bit, not the softsync ISR's. */
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) &
                    ((UINT64_C(1) << ODSY_TEST_RETRACE_VECTOR) |
                     (UINT64_C(1) << ODSY_TEST_SOFTSYNC_VECTOR)), ==,
                    UINT64_C(1) << ODSY_TEST_RETRACE_VECTOR);
    board_quit(qts);
}

/*
 * The arm is a toggle mask latched at the retrace: the captured swap's
 * window-24 bit flips bit 24 of the displayed-buffer status, a second swap
 * flips it back, and a two-window mask flips both bits. A retrace with
 * nothing armed flips nothing. The buffer-select doublewords the driver
 * pushes ahead of the arm (0x608/0x609) neither flip a buffer nor raise an
 * interrupt.
 */
static void test_swap_arm_toggles_displayed_buffer(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-swap-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    route_to_pending(qts);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0);

    cmd_push64(qts, ODSY_CMD_SWAP_SYNC_A, ODSY_CMD_SWAP_SYNC_OPERAND);
    cmd_push64(qts, ODSY_CMD_SWAP_SYNC_B, ODSY_CMD_SWAP_SYNC_OPERAND);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);

    reg_write(qts, ODSY_SWAP_ARM, ODSY_SWAP_ARM_BIT);
    g_assert_cmphex(reg_read(qts, ODSY_SWAP_ARM), ==, ODSY_SWAP_ARM_BIT);
    /* Nothing flips before the retrace. */
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0);

    fire_retrace(qts);
    g_assert_cmphex(reg_read(qts, ODSY_SWAP_ARM), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==,
                    ODSY_SWAP_ARM_BIT);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_RETRACE_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    /* An idle retrace flips nothing. */
    fire_retrace(qts);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==,
                    ODSY_SWAP_ARM_BIT);

    /* The next swap of the same window flips it back. */
    reg_write(qts, ODSY_SWAP_ARM, ODSY_SWAP_ARM_BIT);
    fire_retrace(qts);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0);

    /* A two-window mask flips both windows at one retrace. */
    reg_write(qts, ODSY_SWAP_ARM, 0x00000005);
    fire_retrace(qts);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0x00000005);

    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "command FIFO word 0x00000608 is not modeled"));
    g_assert_null(strstr(log, "command FIFO word 0x00000609 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * Arm writes between two retraces accumulate: separate windows issued before
 * one retrace all flip there, and the ISR's re-arm of a window still pending
 * does not cancel the other windows' flips.
 */
static void test_swap_arm_accumulates(void)
{
    QTestState *qts = board_start("");

    route_to_pending(qts);
    reg_write(qts, ODSY_SWAP_ARM, 0x00000001);
    reg_write(qts, ODSY_SWAP_ARM, ODSY_SWAP_ARM_BIT);
    g_assert_cmphex(reg_read(qts, ODSY_SWAP_ARM), ==,
                    ODSY_SWAP_ARM_BIT | 0x00000001);
    /* A re-arm of an already-pending window leaves the mask as it was. */
    reg_write(qts, ODSY_SWAP_ARM, ODSY_SWAP_ARM_BIT);
    g_assert_cmphex(reg_read(qts, ODSY_SWAP_ARM), ==,
                    ODSY_SWAP_ARM_BIT | 0x00000001);

    fire_retrace(qts);
    g_assert_cmphex(reg_read(qts, ODSY_SWAP_ARM), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==,
                    ODSY_SWAP_ARM_BIT | 0x00000001);
    board_quit(qts);
}

/*
 * The immediate path: the driver's reset clears every window to buffer 0
 * through the companion, and a swap that must not wait for retrace sets its
 * window's buffer through ODSY_SWAP_CONTROL (buffer 1) or the companion
 * (buffer 0), taking effect at once.
 */
static void test_swap_immediate_set_and_clear(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_SWAP_ARM, 0x00000003);
    fire_retrace(qts);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0x00000003);

    reg_write(qts, ODSY_SWAP_CONTROL_COMPANION, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0);

    reg_write(qts, ODSY_SWAP_CONTROL, 0x00000010);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0x00000010);
    reg_write(qts, ODSY_SWAP_CONTROL_COMPANION, 0x00000010);
    g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, 0);
    board_quit(qts);
}

/*
 * A command-FIFO word this grammar does not recognize is reported once,
 * by word rather than by address (odyssey_cmd_unimplemented(),
 * hw/misc/sgi-odyssey.c), and leaves the framebuffer untouched -- unlike
 * every established register-select, tagged opcode or literal template,
 * which either draws or is a documented no-op.
 */
static void test_command_fifo_unknown_word_is_unimplemented(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-cmd-unimp-XXXXXX.log", &log_path,
                          &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    /*
     * A word with no recognized shape: not a register-select (the top 16
     * bits are not 0x0001, so the bit-field arity rule does not apply), not
     * a pixel run, tagged opcode or literal template.
     */
    cmd_push(qts, 0x00007777);
    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log,
                            "command FIFO word 0x00007777 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * odyssey_unimplemented() and odyssey_cmd_unimplemented() used to share one
 * SGI_ODYSSEY_UNIMP_REPORTS-entry table: on a real boot the command FIFO's
 * own run of unmodeled words filled it before the driver ever touched an
 * unmodeled register, so that register access never produced a single -d
 * unimp line. Filling the FIFO-word budget here (redeclared as
 * ODSY_UNIMP_REPORT_BUDGET, include/hw/misc/sgi-odyssey.h's
 * SGI_ODYSSEY_UNIMP_REPORTS) and then touching one unmodeled register
 * proves the register budget is a separate table, untouched by it.
 */
#define ODSY_UNIMP_REPORT_BUDGET 64

static void test_unimplemented_reports_have_independent_budgets(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    unsigned int i;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-unimp-budget-XXXXXX.log", &log_path,
                          &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    /*
     * Distinct words with no recognized shape (top 16 bits 0x0000, so not
     * register-selects the bit-field arity rule would consume, and none a
     * pixel run, trigger or tagged opcode): each fills one slot of the
     * FIFO-word budget alone.
     */
    for (i = 0; i < ODSY_UNIMP_REPORT_BUDGET; i++) {
        cmd_push(qts, 0x00007700 | i);
    }
    /* An unmodeled register offset, distinct from every case above. */
    qtest_readb(qts, WIDGET + 0x0008);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "1-byte read at 0x8 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The kernel driver's context switch: a single 8-byte store
 * to ODSY_CTX_SWITCH_PORT completes instantly by writing a nonzero word
 * into the shadow page ODSY_CTX_SHADOW_ADDR_UPPER/_LOWER name, at the
 * offset the driver polls (ODSY_CTX_SWITCH_DONE_OFFSET) -- no interrupt,
 * no command-FIFO involvement.
 */
static void test_context_switch_completion(void)
{
    QTestState *qts = board_start("");
    const uint64_t base = 0xb70000;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);

    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, (uint32_t)base);

    g_assert_cmphex(qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET), ==,
                    0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_WORD);
    g_assert_cmphex(qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET), !=,
                    0);

    board_quit(qts);
}

/*
 * Command work after the store (hw/misc/sgi-odyssey.c). Once a processor's
 * command-FIFO store has run longer than a slice, the stores that follow
 * wait in a queue and run in slices on the board's thread, so the
 * processor can take its scheduler ticks while the board draws. qtest has
 * no processor; x-test-slice-words queues every command store and runs
 * that many a slice. The machine is let run (qtest executes no guest
 * code) so the slices' timer fires as the test steps the clock.
 */
/* The model's command-FIFO low-water mark, in waiting stores. */
#define WORK_CFIFO_LOW_WATER 16

static QTestState *work_start(void)
{
    QTestState *qts = board_launch("");

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, 0xb70000);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-slice-words', 'value': 1}}", BOARD);
    return qts;
}

/* The stores still waiting: the command FIFO's level. */
static uint32_t work_waiting(QTestState *qts)
{
    return odsy_qom_get_uint(qts, "x-test-work-waiting");
}

/*
 * One slice: step the clock past the slice's timer and wait for the
 * board's thread to run it, one store here.
 */
static void work_step(QTestState *qts)
{
    uint32_t before = work_waiting(qts);
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    g_assert_cmpuint(before, >, 0);
    qtest_clock_step(qts, 1000 * 1000);
    while (work_waiting(qts) == before) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmpuint(work_waiting(qts), ==, before - 1);
}

static bool work_pending(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': 'x-test-work-pending'}}", BOARD);
    bool pending = qdict_get_bool(resp, "return");

    qobject_unref(resp);
    return pending;
}

/* Two host-write fences, slot 2 then slot 3, each three words. */
static void work_push_fences(QTestState *qts)
{
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 2);
    cmd_push(qts, 0x5a00);
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 3);
    cmd_push(qts, 0xa5a5);
}

/*
 * The stores return with their work waiting; it runs one store a slice
 * as the clock steps, in order, and its fences land when it does.
 */
static void test_command_work_runs_in_slices(void)
{
    QTestState *qts = work_start();
    unsigned int steps = 0;

    work_push_fences(qts);
    g_assert_true(work_pending(qts));
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 6), ==, 0);
    while (work_pending(qts)) {
        g_assert_cmpuint(++steps, <, 100);
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmpuint(steps, >=, 3);
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 4), ==, 0x5a00);
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 6), ==, 0xa5a5);
    board_quit(qts);
}

/*
 * A context switch waits behind the commands before it: the kernel polls
 * the done word, which the board writes only once those have run.
 */
static void test_command_work_orders_context_switch(void)
{
    QTestState *qts = work_start();
    const uint64_t base = 0xb80000;

    work_push_fences(qts);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, (uint32_t)base);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_WORD);
    g_assert_cmphex(qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET), ==,
                    0);
    while (work_pending(qts)) {
        qtest_clock_step(qts, 1000 * 1000);
        if (qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET)) {
            /* The fences before the switch had already landed. */
            g_assert_cmphex(qtest_readw(qts, 0xb70000 + 6), ==, 0xa5a5);
        }
    }
    g_assert_cmphex(qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET), !=,
                    0);
    board_quit(qts);
}

/*
 * A read of a register the work may change finishes the work first; the
 * software token, which no command touches, is answered at once.
 */
static void test_command_work_read_waits(void)
{
    QTestState *qts = work_start();

    work_push_fences(qts);
    reg_write(qts, ODSY_SW_TOKEN, 1);
    g_assert_cmphex(reg_read(qts, ODSY_SW_TOKEN), ==, 1);
    g_assert_true(work_pending(qts));
    reg_read(qts, ODSY_MEMORY_CONFIG);
    g_assert_false(work_pending(qts));
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 6), ==, 0xa5a5);
    board_quit(qts);
}

/*
 * While stores wait the FIFO is neither empty nor below low water, in
 * the status the kernel's credit flow control reads before it sleeps on
 * the low-water interrupt; it reads ready again once they have run.
 */
static void test_command_work_status_shows_waiting_stores(void)
{
    QTestState *qts = work_start();
    unsigned int waiting = 0;

    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10030000);
    work_push_fences(qts);
    waiting += 6;
    /* Not empty, still below the low-water mark of 16 stores. */
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10020000);
    for (unsigned int i = 0; i < 2; i++) {
        work_push_fences(qts);
        waiting += 6;
    }
    g_assert_cmpuint(work_waiting(qts), ==, waiting);
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10000000);
    /* One store runs per slice: below the mark once 15 wait. */
    while (work_waiting(qts) > WORK_CFIFO_LOW_WATER) {
        work_step(qts);
        g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10000000);
    }
    work_step(qts);
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10020000);
    while (work_pending(qts)) {
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10030000);
    board_quit(qts);
}

/*
 * The kernel's credit flow control: it clears low water, finds the FIFO
 * above its low-water mark, enables the source and sleeps until the
 * interrupt. Low water is not raised while 16 or more stores wait, and is
 * raised, with its interrupt, as the level falls below that mark, while
 * stores still wait.
 */
static void test_command_work_low_water_after_drain(void)
{
    QTestState *qts = work_start();

    flag_dest_program(qts);
    for (unsigned int i = 0; i < 3; i++) {
        work_push_fences(qts);
    }
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    /* 18 wait; two slices leave 16, still at the mark. */
    work_step(qts);
    work_step(qts);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value") &
                    ODSY_HUB_INT_MODIFY_SET, ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) & ODSY_INTR_FIFO_LOW_WATER,
                    ==, 0);
    /* The next leaves 15: below the mark. */
    work_step(qts);
    g_assert_cmpuint(work_waiting(qts), ==, WORK_CFIFO_LOW_WATER - 1);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) & ODSY_INTR_FIFO_LOW_WATER,
                    ==, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);
    board_quit(qts);
}

/*
 * A counted store's credit comes back when the board takes it into its
 * command FIFO, not when its work runs: the oldest 64 waiting stores (the
 * model's FIFO depth) have their credits back, and each store behind them
 * gets its credit as the work ahead runs and it moves up. The node-widget
 * map sends the board's credits to PI0 slot A. A qtest store is not
 * charged on the way in, so the count rises by the credits returned.
 */
#define WORK_CFIFO_DEPTH 64
#define WORK_PI_CREDIT_A (BEDROCK_NODE0 + 0x308)
#define WORK_GFX_MAP_0 (BEDROCK_NODE0 + 0x400140)

static void test_command_work_credits_at_fifo_entry(void)
{
    QTestState *qts = work_start();
    unsigned int stores = 0;

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-counted-stores', 'value': 1}}", BOARD);
    qtest_writeq(qts, WORK_GFX_MAP_0, SLOT);
    qtest_writeq(qts, WORK_PI_CREDIT_A, 1000);

    /* Each fence pair is six stores; twelve of them overfill the FIFO. */
    for (unsigned int i = 0; i < 12; i++) {
        work_push_fences(qts);
        stores += 6;
    }
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==,
                     1000 + WORK_CFIFO_DEPTH);

    /* One store runs per slice here: the next one moves up and is repaid. */
    work_step(qts);
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==,
                     1000 + WORK_CFIFO_DEPTH + 1);

    while (work_pending(qts)) {
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==, 1000 + stores);

    /* A store that does not wait is repaid inside the store. */
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-slice-words', 'value': 0}}", BOARD);
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 2);
    cmd_push(qts, 0x5a00);
    g_assert_false(work_pending(qts));
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==,
                     1000 + stores + 3);
    board_quit(qts);
}

/*
 * A credit the board owes for a store still waiting outside its FIFO
 * settles when software rewrites the node-widget map entry naming the
 * board: it is delivered under the entry as it stood, and the board's later
 * repayment is dropped. IRIX's detach zeroes the entry and then reads the
 * counter into the flow's saved count, so without this the count would be
 * short by every owed credit (the node's model choice).
 */
static void test_command_work_map_rewrite_settles(void)
{
    QTestState *qts = work_start();

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-counted-stores', 'value': 1}}", BOARD);
    qtest_writeq(qts, WORK_GFX_MAP_0, SLOT);
    qtest_writeq(qts, WORK_PI_CREDIT_A, 1000);
    for (unsigned int i = 0; i < 12; i++) {
        work_push_fences(qts);
    }
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==,
                     1000 + WORK_CFIFO_DEPTH);

    /* The detach: the eight owed credits arrive before the entry clears. */
    qtest_writeq(qts, WORK_GFX_MAP_0, 0);
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==, 1000 + 72);

    /* Rewriting it again owes nothing more, and the repayments are dropped. */
    qtest_writeq(qts, WORK_GFX_MAP_0, SLOT);
    while (work_pending(qts)) {
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmpuint(qtest_readq(qts, WORK_PI_CREDIT_A), ==, 1000 + 72);
    board_quit(qts);
}

/* A stopping machine finishes the work; reset abandons it. */
static void test_command_work_stop_and_reset(void)
{
    QTestState *qts = work_start();

    work_push_fences(qts);
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    g_assert_false(work_pending(qts));
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 6), ==, 0xa5a5);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 4);
    cmd_push(qts, 0x1234);
    g_assert_true(work_pending(qts));
    qtest_system_reset(qts);
    g_assert_false(work_pending(qts));
    g_assert_cmphex(qtest_readw(qts, 0xb70000 + 8), ==, 0);
    board_quit(qts);
}

/*
 * With no shadow address programmed -- the reset value, and the shape a
 * board that never received the driver's 0x4004/0x400c writes would still
 * have -- the switch has nowhere to complete into. hw/misc/sgi-odyssey.c
 * logs a guest error and writes nothing, rather than guessing an address.
 */
static void test_context_switch_without_shadow_address_is_a_guest_error(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-ctxsw-noaddr-XXXXXX.log", &log_path,
                          &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors -D %s", quoted);
    qts = board_start(extra);

    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_WORD);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "no shadow address programmed"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/* An index-mode fill of a rectangle with one PseudoColor index. */
static void index_fill(QTestState *qts, uint32_t index, uint32_t x0,
                       uint32_t y0, uint32_t x1, uint32_t y1)
{
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    cmd_push(qts, index);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x0);
    cmd_push(qts, y0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x1);
    cmd_push(qts, y0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x1);
    cmd_push(qts, y1);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x0);
    cmd_push(qts, y1);
    cmd_push(qts, ODSY_CMD_GL_END);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
}

/*
 * The colormap word stream. The X server loads the PseudoColor palette
 * through ODSY_BACKEND_SELECT as counted records: a header word names an
 * entry index and a count, then that many data words land in consecutive
 * lookup-RAM entries. This drives the note's own suggested contract test -- a
 * three-entry record at base 0x1100 split across two 8-byte stores and closed
 * by a 4-byte count-0 pad -- and proves the whole indexed-fill path: a fill
 * through drawable 1 (whose display-mode selects group 17 at base 0x1100)
 * resolves each index to the colour just loaded. It also pins the critical
 * constraint that a following count-1 I2C selector still reaches the I2C
 * command register, unchanged by the rework.
 */
static void test_backend_colormap_word_stream(void)
{
    QTestState *qts = board_start("");

    /* Drawable 1's display-mode word selects colormap group 17 (base 0x1100). */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);

    /*
     * A count-3 record at index 0x1100: red, green then blue, split across
     * two 8-byte stores (header+first, then second+third) and closed by a
     * 4-byte pad record.
     */
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);

    /* Each index resolves to the colour loaded at 0x1100 + index. */
    index_fill(qts, 0, 10, 10, 20, 20);
    index_fill(qts, 1, 30, 10, 40, 20);
    index_fill(qts, 2, 50, 10, 60, 20);

    /* Native framebuffer is 0xRRGGBBAA: red, green, blue with opaque alpha. */
    g_assert_cmphex(odsy_shown(qts, 10, 10), ==, 0xff0000ff);
    g_assert_cmphex(odsy_shown(qts, 30, 10), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, 50, 10), ==, 0x0000ffff);

    /*
     * The I2C engine is untouched by the rework: a count-1 opt selector,
     * the shape enumeration uses, still lands in the opt command register.
     */
    i2c_push(qts, ODSY_I2C_OPT_SELECTOR, 0x00001234);
    g_assert_cmphex(reg_read(qts, ODSY_I2C_OPT_CMD), ==, 0x00001234);

    board_quit(qts);
}

/*
 * The four corner vertices of a QUADS rectangle and the flush tail every fill
 * ends with, shared by the id-plane-write helper below.
 */
static void quad_rect(QTestState *qts, uint32_t x0, uint32_t y0,
                      uint32_t x1, uint32_t y1)
{
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x0);
    cmd_push(qts, y0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x1);
    cmd_push(qts, y0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x1);
    cmd_push(qts, y1);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x0);
    cmd_push(qts, y1);
    cmd_push(qts, ODSY_CMD_GL_END);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
}

/*
 * The X server's drawable-id plane write over a rectangle: the 0x1023
 * plane-mask block, the 0x1017 destination-select block with the id-plane
 * constant as its third operand, a COLOR_INDEX word carrying id << 3, then
 * the QUADS rectangle.
 */
static void id_plane_write(QTestState *qts, uint32_t id, uint32_t mask,
                           uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    cmd_push(qts, ODSY_CMD_PLANE_MASK);
    cmd_push(qts, mask << 12);
    cmd_push(qts, 1);
    cmd_push(qts, 0);
    cmd_push(qts, 0xff);
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, ODSY_DEST_ID_PLANES);
    cmd_push(qts, 0);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    cmd_push(qts, id << 3);
    quad_rect(qts, x0, y0, x1, y1);
}

/*
 * A colour-index fill preceded by the colour destination-select a real fill
 * always re-sends, so the id-plane path stays selected only for the fill that
 * asked for it.
 */
static void colour_index_fill(QTestState *qts, uint32_t index, uint32_t x0,
                              uint32_t y0, uint32_t x1, uint32_t y1)
{
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, ODSY_DEST_COLOUR_INDEX);
    cmd_push(qts, 0);
    index_fill(qts, index, x0, y0, x1, y1);
}

/*
 * The drawable-id plane and per-pixel colour resolution. The X server's
 * drawable-id plane write carries the id-plane destination constant: the
 * model must write the did plane under the rectangle and leave the colour
 * framebuffer alone, then a later colour-index fill over the region must
 * resolve through the drawable the plane write assigned, not a fixed
 * drawable. This is exactly the account-panel bug: the model used to paint
 * the id-plane write as a colormap fill, producing the black band.
 */
static void test_command_fifo_id_plane_write(void)
{
    QTestState *qts = board_start("");
    const uint32_t x0 = 400, y0 = 400, x1 = 420, y1 = 420;
    const uint32_t colour_a = 0x102030ff; /* group 17, index 5 */
    const uint32_t colour_b = 0x804020ff; /* group 18, index 5 */

    /* Drawable 1 -> group 17 (base 0x1100), drawable 24 -> group 18 (0x1200). */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID24, ODSY_DISPMODE_GROUP18);

    /* index 5 is a distinct colour in each group's colormap. */
    backend_push64(qts, 0x34414001u, 0x040200c0u); /* 0x1105 = colour_a */
    backend_push64(qts, 0x34814001u, 0x20040080u); /* 0x1205 = colour_b */

    /*
     * Assign the whole region to drawable 1, the way the X server's
     * drawable-id plane write maps the root window before any greeter
     * fill. It must not paint: the framebuffer stays the reset colour.
     */
    id_plane_write(qts, 1, 0xf8, x0, y0, x1, y1);
    g_assert_cmphex(odsy_test_pixel(qts, x0, y0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, x1 - 1, y1 - 1), ==, 0);

    /* A colour-index fill stores 5, shown through drawable 1 -> group 17. */
    colour_index_fill(qts, 5, x0, y0, x1, y1);
    g_assert_cmphex(odsy_test_pixel(qts, x0, y0), ==, 5);
    g_assert_cmphex(odsy_shown(qts, x0, y0), ==, colour_a);
    g_assert_cmphex(odsy_shown(qts, x1 - 1, y1 - 1), ==, colour_a);

    /*
     * Reassign the region to drawable 24. Again no paint: the colour planes
     * still hold index 5, proving the id-plane write left the framebuffer
     * untouched. The display follows the pixel's own drawable at scanout,
     * so the same index now shows through drawable 24 -> group 18 without a
     * repaint.
     */
    id_plane_write(qts, 24, 0xf8, x0, y0, x1, y1);
    g_assert_cmphex(odsy_test_pixel(qts, x0, y0), ==, 5);
    g_assert_cmphex(odsy_test_pixel(qts, x1 - 1, y1 - 1), ==, 5);
    g_assert_cmphex(odsy_shown(qts, x0, y0), ==, colour_b);
    g_assert_cmphex(odsy_shown(qts, x1 - 1, y1 - 1), ==, colour_b);

    board_quit(qts);
}

/* A 32-word pattern bitmap load: 0x3c4e carries words 0-15, 0x3c4f 16-31. */
static void pattern_load(QTestState *qts, const uint32_t words[32])
{
    unsigned int i;

    cmd_push(qts, ODSY_CMD_PATTERN_LOW);
    for (i = 0; i < 16; i++) {
        cmd_push(qts, words[i]);
    }
    cmd_push(qts, ODSY_CMD_PATTERN_HIGH);
    for (i = 16; i < 32; i++) {
        cmd_push(qts, words[i]);
    }
}

/*
 * The one-bit pattern-quad text path. The greeter draws every text label this
 * way: load a 32x32-bit bitmap through the 0x3c4e/0x3c4f register bursts,
 * select the 32x32 shape with 0x043f, enable pattern mode with 0x0404 (bits 3
 * and 4), set a colour, then draw the string's bounding box as a QUADS fill.
 * A set pattern bit paints the colour; a clear bit leaves whatever the
 * background held. This pins that: a green pattern (index 1) drawn over a red
 * background (index 0) shows green where a bit is set and red where it is
 * clear, with bit 31 the leftmost pixel, row 0 the top row, and word v the
 * row-v bits of the 32x32 shape.
 */
static void test_command_fifo_pattern_quad(void)
{
    QTestState *qts = board_start("");
    const uint32_t x0 = 200, y0 = 300;
    uint32_t bitmap[32] = { 0 };
    unsigned int u;

    /* Drawable 1 -> group 17 (base 0x1100); index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);

    /* A red 32x32 background: a solid fill, pattern mode still off. */
    index_fill(qts, 0, x0, y0, x0 + 32, y0 + 32);
    g_assert_cmphex(odsy_shown(qts, x0, y0), ==, 0xff0000ff);

    /*
     * The bitmap: row 0 sets only its leftmost pixel, row 1 its two leftmost,
     * row 5 is solid, every other row is clear. Word v holds row v; bit 31 is
     * the leftmost pixel.
     */
    bitmap[0] = 0x80000000;
    bitmap[1] = 0xc0000000;
    bitmap[5] = 0xffffffff;

    pattern_load(qts, bitmap);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_32X32);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, ODSY_PATTERN_MODE_ENABLE);

    /* The same rectangle in green (index 1), drawn through the pattern. */
    index_fill(qts, 1, x0, y0, x0 + 32, y0 + 32);

    /* Row 0: only the leftmost pixel turned green; the rest stayed red. */
    g_assert_cmphex(odsy_shown(qts, x0 + 0, y0 + 0), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, x0 + 1, y0 + 0), ==, 0xff0000ff);
    g_assert_cmphex(odsy_shown(qts, x0 + 31, y0 + 0), ==, 0xff0000ff);
    /* Row 1: the two leftmost pixels are green, the third still red. */
    g_assert_cmphex(odsy_shown(qts, x0 + 0, y0 + 1), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, x0 + 1, y0 + 1), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, x0 + 2, y0 + 1), ==, 0xff0000ff);
    /* Row 2 is clear: the whole row stayed the red background. */
    g_assert_cmphex(odsy_shown(qts, x0 + 0, y0 + 2), ==, 0xff0000ff);
    /* Row 5 is solid: every pixel is green. */
    for (u = 0; u < 32; u++) {
        g_assert_cmphex(odsy_shown(qts, x0 + u, y0 + 5), ==, 0x00ff00ff);
    }
    /* Just past the quad's right edge is untouched. */
    g_assert_cmphex(odsy_test_pixel(qts, x0 + 32, y0 + 5), ==, 0);

    board_quit(qts);
}

/* Clipping a quad must keep the pattern anchored to its signed first corner. */
static void test_command_fifo_pattern_quad_straddles_screen(void)
{
    QTestState *qts = board_start("");
    uint32_t bitmap[32] = { [1] = 1u << 30 };

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);
    index_fill(qts, 0, 0, 0, 3, 3);
    pattern_load(qts, bitmap);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_32X32);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, ODSY_PATTERN_MODE_ENABLE);

    alarm(10);
    index_fill(qts, 1, UINT32_MAX, UINT32_MAX, 3, 3);
    alarm(0);
    g_assert_cmphex(odsy_shown(qts, 0, 0), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, 1, 0), ==, 0xff0000ff);
    g_assert_cmphex(odsy_shown(qts, 0, 1), ==, 0xff0000ff);
    board_quit(qts);
}

/*
 * Shape bit 4 makes the pattern opaque: clear bits take the background the X
 * server put in 0x0c21's constant colour, an index in operand 1 bits 23:12
 * or 12-bit red, green and blue. This is bitmap(1)'s arrow buttons, a
 * FillOpaqueStippled rectangle: shape 0x14, mode 0xa0018, 0x0c21 = 0x200,
 * background << 12, 1.
 */
#define ODSY_CMD_BLEND 0x00010c21
#define ODSY_PATTERN_SHAPE_OPAQUE 0x10

static void test_command_fifo_pattern_quad_opaque(void)
{
    QTestState *qts = board_start("");
    uint32_t bitmap[32] = { [0] = 0x80000000, [1] = 0x40000000 };

    index_fill(qts, 0x11, 200, 300, 232, 332);
    pattern_load(qts, bitmap);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_OPAQUE | ODSY_PATTERN_SHAPE_32X32);
    cmd_push(qts, ODSY_CMD_BLEND);
    cmd_push(qts, 0x200);
    cmd_push(qts, 7 << 12);
    cmd_push(qts, 1);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0xa0000 | ODSY_PATTERN_MODE_ENABLE);
    index_fill(qts, 0, 200, 300, 204, 302);

    /* Set bits take the foreground, clear bits the background. */
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 300), ==, 7);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 301), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 203, 301), ==, 7);
    /* Outside the quad nothing changed. */
    g_assert_cmphex(odsy_test_pixel(qts, 204, 300), ==, 0x11);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 302), ==, 0x11);

    /* An RGB quad: the constant colour's 12-bit channels, narrowed. */
    cmd_push(qts, ODSY_CMD_BLEND);
    cmd_push(qts, 0x200);
    cmd_push(qts, 0xfff000);
    cmd_push(qts, 0x808fff);
    rgb_fill(qts, 0x00, 0xff, 0x00, 300, 300, 302, 301);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 300), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 301, 300), ==, 0xff0080ff);

    /* Without bit 4 the clear bits keep what was there. */
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_32X32);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0);
    index_fill(qts, 0x11, 200, 300, 204, 302);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0xa0000 | ODSY_PATTERN_MODE_ENABLE);
    index_fill(qts, 0, 200, 300, 204, 302);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 300), ==, 0x11);

    board_quit(qts);
}

/*
 * Solid axial independent lines in the captured default-cap mode include
 * both endpoints. Strips retain half-open coverage, and stippled lines use
 * bit k for the k-th covered pixel from the first vertex, k = 1 next to it.
 */
#define ODSY_CMD_LINE_STIPPLE 0x00010c48
#define ODSY_GL_LINES 0x00000001
#define ODSY_GL_LINE_STRIP 0x00000003
#define ODSY_GL_POLYGON 0x00000009
#define ODSY_RED 0xff0000ff
#define ODSY_GREEN 0x00ff00ff
#define ODSY_BLUE 0x0000ffff

static void vertex2i(QTestState *qts, uint32_t x, uint32_t y)
{
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, x);
    cmd_push(qts, y);
}

static void colour_index(QTestState *qts, uint32_t index)
{
    cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    cmd_push(qts, index);
}

static void test_command_fifo_lines(void)
{
    QTestState *qts = board_start("");
    unsigned int i;

    /* Drawable 1 -> group 17 (base 0x1100); index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);

    index_fill(qts, 0, 200, 300, 232, 332);
    g_assert_cmphex(odsy_shown(qts, 200, 300), ==, ODSY_RED);

    /* Solid lines in both directions and both axes. */
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x80000);
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 202, 310);
    vertex2i(qts, 210, 310);
    vertex2i(qts, 220, 310);
    vertex2i(qts, 212, 310);
    vertex2i(qts, 205, 315);
    vertex2i(qts, 205, 320);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 202; i <= 210; i++) {
        g_assert_cmphex(odsy_shown(qts, i, 310), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 211, 310), ==, ODSY_RED);
    g_assert_cmphex(odsy_shown(qts, 201, 310), ==, ODSY_RED);
    for (i = 212; i <= 220; i++) {
        g_assert_cmphex(odsy_shown(qts, i, 310), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 221, 310), ==, ODSY_RED);
    g_assert_cmphex(odsy_shown(qts, 211, 310), ==, ODSY_RED);
    for (i = 315; i <= 320; i++) {
        g_assert_cmphex(odsy_shown(qts, 205, i), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 205, 321), ==, ODSY_RED);

    /* A strip paints its joint once and not its final vertex. */
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINE_STRIP);
    vertex2i(qts, 202, 325);
    vertex2i(qts, 206, 325);
    vertex2i(qts, 206, 329);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 202; i <= 205; i++) {
        g_assert_cmphex(odsy_shown(qts, i, 325), ==, ODSY_GREEN);
    }
    for (i = 325; i <= 328; i++) {
        g_assert_cmphex(odsy_shown(qts, 206, i), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 206, 329), ==, ODSY_RED);

    /* A stippled right-to-left span: bit 1 lands next to the first vertex. */
    cmd_push(qts, ODSY_CMD_LINE_STIPPLE);
    cmd_push(qts, 0xaaaa);
    cmd_push(qts, 0xaaaaaaaa);
    cmd_push(qts, 0x30f);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x80018);
    colour_index(qts, 2);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 212, 305);
    vertex2i(qts, 204, 305);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 205; i <= 211; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_BLUE);
    }
    for (i = 204; i <= 212; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_RED);
    }

    /* The same segment with the mode bits clear ignores the stipple. */
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x80000);
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 212, 305);
    vertex2i(qts, 204, 305);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 204; i <= 212; i++) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_GREEN);
    }

    /* Sixteen QUADS vertices are four disjoint rectangles. */
    colour_index(qts, 2);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    for (i = 0; i < 4; i++) {
        uint32_t x = 400 + 10 * i;

        vertex2i(qts, x, 400);
        vertex2i(qts, x + 4, 400);
        vertex2i(qts, x + 4, 404);
        vertex2i(qts, x, 404);
    }
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(odsy_shown(qts, 402 + 10 * i, 402), ==,
                        ODSY_BLUE);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 406, 402), ==, 0);

    /* A polygon fills the pixels whose centres are inside. */
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_POLYGON);
    vertex2i(qts, 300, 300);
    vertex2i(qts, 310, 300);
    vertex2i(qts, 305, 310);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_shown(qts, 305, 302), ==, ODSY_GREEN);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 309), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 310, 309), ==, 0);

    board_quit(qts);
}

/*
 * An opaque stippled span, as the X server fills a FillOpaqueStippled
 * polygon (4Dwm's icon faces): 0x043f bit 4 with the line stipple on, so
 * the clear bits take the 0x0c21 constant colour's index, here 2 (blue),
 * and the set ones the current index 1 (green).
 */
static void test_command_fifo_opaque_stippled_span(void)
{
    QTestState *qts = board_start("");
    unsigned int i;

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);
    index_fill(qts, 0, 200, 300, 232, 332);

    cmd_push(qts, ODSY_CMD_LINE_STIPPLE);
    cmd_push(qts, 0xaaaa);
    cmd_push(qts, 0xaaaaaaaa);
    cmd_push(qts, 0x30f);
    cmd_push(qts, ODSY_CMD_BLEND);
    cmd_push(qts, 0x200);
    cmd_push(qts, 0x2000);
    cmd_push(qts, 0x1);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_OPAQUE);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x10a0018);
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 212, 305);
    vertex2i(qts, 204, 305);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 205; i <= 211; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_GREEN);
    }
    for (i = 204; i <= 210; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_BLUE);
    }
    g_assert_cmphex(odsy_shown(qts, 212, 305), ==, ODSY_RED);
    board_quit(qts);
}

/*
 * The X server's stippled-span routine never writes 0x043f: it marks a
 * FillOpaqueStippled span by 0x0404 bit 17 and a 0x0c21 of ONE, ZERO with
 * the background as constant colour, and leaves bit 17 clear for
 * FillStippled. 4Dwm's Welcome_to_SGI
 * floor arrives under shape 0x5, left by an earlier pattern fill: index 8
 * over background 0xb. A stale shape without the opaque bit and bit 17 set
 * must paint the clear bits in the background (index 2, blue); a stale
 * opaque shape with bit 17 clear must leave them alone, and so must bit 17
 * with any blend factors but ONE, ZERO.
 */
static void test_command_fifo_opaque_span_ignores_shape(void)
{
    QTestState *qts = board_start("");
    unsigned int i;

    /* Drawables 0 and 1 both name group 17, whichever the id plane picks. */
    backend_push32(qts, ODSY_BACKEND_HDR(0x2900, 1));
    backend_push32(qts, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);
    index_fill(qts, 0, 200, 300, 232, 332);

    cmd_push(qts, ODSY_CMD_LINE_STIPPLE);
    cmd_push(qts, 0xaaaa);
    cmd_push(qts, 0xaaaaaaaa);
    cmd_push(qts, 0x30f);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, 0x5);
    cmd_push(qts, ODSY_CMD_BLEND);
    cmd_push(qts, 0x200);
    cmd_push(qts, 0x2000);
    cmd_push(qts, 0x1);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x10a0018);
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 212, 305);
    vertex2i(qts, 204, 305);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 205; i <= 211; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_GREEN);
    }
    for (i = 204; i <= 210; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 305), ==, ODSY_BLUE);
    }

    /* FillStippled: bit 17 clear under a stale opaque shape. */
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_OPAQUE);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x1080018);
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 212, 307);
    vertex2i(qts, 204, 307);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 205; i <= 211; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 307), ==, ODSY_GREEN);
    }
    for (i = 204; i <= 210; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 307), ==, ODSY_RED);
    }

    /*
     * Bit 17 with a real blend function, SRC_ALPHA, ONE_MINUS_SRC_ALPHA
     * (0x639), as a GL client blending integer stippled lines would send
     * it: not the X server's ONE, ZERO opaque span, so clear bits stay.
     */
    cmd_push(qts, ODSY_CMD_BLEND);
    cmd_push(qts, 0x639);
    cmd_push(qts, 0x2000);
    cmd_push(qts, 0x1);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x10a0018);
    colour_index(qts, 1);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 212, 309);
    vertex2i(qts, 204, 309);
    cmd_push(qts, ODSY_CMD_GL_END);
    for (i = 205; i <= 211; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 309), ==, ODSY_GREEN);
    }
    for (i = 204; i <= 210; i += 2) {
        g_assert_cmphex(odsy_shown(qts, i, 309), ==, ODSY_RED);
    }
    board_quit(qts);
}

/* Extreme endpoints must finish and keep stipple phase at the first vertex. */
static void test_command_fifo_lines_cross_screen_edge(void)
{
    QTestState *qts = board_start("");

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);
    index_fill(qts, 0, 0, 10, 4, 11);
    index_fill(qts, 0, 10, 0, 11, 4);
    index_fill(qts, 0, 1276, 20, 1280, 21);
    cmd_push(qts, ODSY_CMD_LINE_STIPPLE);
    cmd_push(qts, 0xaaaa);
    cmd_push(qts, 0xaaaaaaaa);
    cmd_push(qts, 0x30f);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x80018);
    colour_index(qts, 2);

    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 0x80000000, 10);
    alarm(10);
    vertex2i(qts, 4, 10);
    alarm(0);
    cmd_push(qts, ODSY_CMD_GL_END);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 10, 0x80000000);
    alarm(10);
    vertex2i(qts, 10, 4);
    alarm(0);
    cmd_push(qts, ODSY_CMD_GL_END);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 1276, 20);
    alarm(10);
    vertex2i(qts, INT32_MAX, 20);
    alarm(0);
    cmd_push(qts, ODSY_CMD_GL_END);

    for (unsigned i = 0; i < 4; i++) {
        uint32_t expected = i % 2 == 0 ? ODSY_BLUE : ODSY_RED;

        g_assert_cmphex(odsy_shown(qts, i, 10), ==, expected);
        g_assert_cmphex(odsy_shown(qts, 10, i), ==, expected);
        g_assert_cmphex(odsy_shown(qts, 1276 + i, 20), ==, expected);
    }
    board_quit(qts);
}

/*
 * Integer primitives stream: the command FIFO has no batch size, so a LINES
 * block, a strip and a polygon of more than 128 vertices draw every piece,
 * and integer POINTS paint the pixel each vertex names. Nothing is reported
 * truncated or unmodeled.
 */
#define ODSY_GL_POINTS 0x00000000

static void test_command_fifo_integer_primitives_stream(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    unsigned int i;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-vertices-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    cmd_push(qts, ODSY_CMD_GL_COLOR_3UB);
    cmd_push(qts, 0xff);
    cmd_push(qts, 0);
    cmd_push(qts, 0);

    /* 150 LINES spans, 300 vertices: the last span lands. */
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    for (i = 0; i < 150; i++) {
        vertex2i(qts, 100, 100 + i);
        vertex2i(qts, 102, 100 + i);
    }
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 249), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 102, 249), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 250), ==, 0);

    /* A 200-vertex strip along row 300 paints 10..208 and not its end. */
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINE_STRIP);
    for (i = 0; i < 200; i++) {
        vertex2i(qts, 10 + i, 300);
    }
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 300), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 208, 300), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 209, 300), ==, 0);

    /*
     * A 152-vertex polygon: 150 collinear vertices along its top edge, then
     * two corners. It fills the rectangle 400..548 x 500..519.
     */
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_POLYGON);
    for (i = 0; i < 150; i++) {
        vertex2i(qts, 400 + i, 500);
    }
    vertex2i(qts, 549, 520);
    vertex2i(qts, 400, 520);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 400, 500), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 548, 519), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 474, 510), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 549, 510), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 399, 510), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 474, 520), ==, 0);

    /* POINTS: each vertex paints its own pixel, the neighbours stay clear. */
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_POINTS);
    vertex2i(qts, 50, 60);
    vertex2i(qts, 52, 60);
    vertex2i(qts, 1279, 1023);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 60), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 52, 60), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 1279, 1023), ==, ODSY_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 51, 60), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 61), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 49, 59), ==, 0);

    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "truncated"));
    g_assert_null(strstr(log, "is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The X server's GC function on integer primitives: 0x0422 carries it and
 * 0x0404 bit 19 (index) or bit 20 (RGB) enables it. xdm's text-field caret
 * is a one-pixel QUADS rectangle drawn with GXxor in index text ^ background
 * (0x07 ^ 0x11 = 0x16), so drawing it twice must restore the field.
 * bitmap(1)'s grid lines and squares are GXxor too.
 */
#define ODSY_CMD_LOGIC_OP 0x00010422
#define ODSY_MODE_INDEX_LOGIC_OP (1u << 19)
#define ODSY_MODE_RGB_LOGIC_OP (1u << 20)

static void test_command_fifo_integer_logic_op(void)
{
    QTestState *qts = board_start("");

    index_fill(qts, 0x11, 200, 300, 232, 332);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, ODSY_MODE_INDEX_LOGIC_OP);
    cmd_push(qts, ODSY_CMD_LOGIC_OP);
    cmd_push(qts, 6);
    index_fill(qts, 0x16, 210, 300, 211, 332);
    g_assert_cmphex(odsy_test_pixel(qts, 210, 310), ==, 0x07);
    g_assert_cmphex(odsy_test_pixel(qts, 211, 310), ==, 0x11);
    index_fill(qts, 0x16, 210, 300, 211, 332);
    g_assert_cmphex(odsy_test_pixel(qts, 210, 310), ==, 0x11);

    /* A line takes the same operation: the bitmap editor's grid. */
    colour_index(qts, 0x07);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 202, 320);
    vertex2i(qts, 206, 320);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 204, 320), ==, 0x16);

    /* GXcopy (3) stores the index as it is. */
    cmd_push(qts, ODSY_CMD_LOGIC_OP);
    cmd_push(qts, 3);
    index_fill(qts, 0x16, 210, 300, 211, 332);
    g_assert_cmphex(odsy_test_pixel(qts, 210, 310), ==, 0x16);

    /* Bit 19 clear: the operation is not applied to an index. */
    cmd_push(qts, ODSY_CMD_LOGIC_OP);
    cmd_push(qts, 6);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0);
    index_fill(qts, 0x11, 212, 300, 213, 332);
    g_assert_cmphex(odsy_test_pixel(qts, 212, 310), ==, 0x11);

    /* Bit 20 applies it to an RGB fill and keeps the fourth byte opaque. */
    rgb_fill(qts, 0xff, 0x00, 0x00, 300, 300, 310, 310);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, ODSY_MODE_RGB_LOGIC_OP);
    rgb_fill(qts, 0xff, 0xff, 0x00, 300, 300, 305, 310);
    g_assert_cmphex(odsy_test_pixel(qts, 302, 305), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 307, 305), ==, 0xff0000ff);

    board_quit(qts);
}

/* A raster-box coordinate reaches the board as a float bit pattern. */
static uint32_t coord_bits(uint32_t coord)
{
    float f = (float)coord;
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/*
 * Open a BEGIN-0xa image over the box (x1, y1)-(x2, y2) (top-left inclusive,
 * bottom-right exclusive): the trigger state word, BEGIN 0xa, the ten box
 * floats with the corners at 4, 5, 7 and 8, and the (0, 0) vertex.
 */
static void image_begin(QTestState *qts, uint32_t x1, uint32_t y1,
                        uint32_t x2, uint32_t y2)
{
    cmd_push(qts, ODSY_CMD_IMAGE_STATE);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_IMAGE_PRIMITIVE);
    cmd_push(qts, ODSY_CMD_IMAGE_BOX);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
    cmd_push(qts, coord_bits(x1));
    cmd_push(qts, coord_bits(y1));
    cmd_push(qts, 0);
    cmd_push(qts, coord_bits(x2));
    cmd_push(qts, coord_bits(y2));
    cmd_push(qts, 0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
}

/*
 * The X server's 24-bit TrueColor pixel for colour 0xRRGGBB: red mask 0xff,
 * blue mask 0xff0000, so the word 0x00BBGGRR, which source code 0x4c carries
 * as the bytes A, B, G, R.
 */
#define ODSY_X_PIXEL(rgb) \
    ((((rgb) & 0xffu) << 16) | ((rgb) & 0xff00u) | (((rgb) >> 16) & 0xffu))

/*
 * BEGIN-0xa image raster, format 0x11: the account icons. A small box filled
 * with distinct X pixels pins the raster order -- left to right,
 * wrapping at the box's exclusive right edge one row down -- and that each
 * word paints directly as colour with an opaque alpha, leaving the pixels just
 * past the box untouched.
 */
static void test_command_fifo_image_raster_rgb(void)
{
    QTestState *qts = board_start("");
    const uint32_t x1 = 100, y1 = 100, x2 = 102, y2 = 102;

    image_begin(qts, x1, y1, x2, y2);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x4c)); /* first header: unbounded */
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_X_PIXEL(0x778899));
    cmd_push(qts, ODSY_X_PIXEL(0xaabbcc));
    cmd_push(qts, ODSY_CMD_GL_END);

    /* Raster order across the 2x2 box, each word its own colour, alpha opaque. */
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 100), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 101), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 101), ==, 0xaabbccff);
    /* RGB shows through an RGB mode (drawable 0, the id plane at reset). */
    display_identity(qts, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 100, 100), ==, 0x00112233);
    /* Just past the right edge and below the bottom edge stay untouched. */
    g_assert_cmphex(odsy_test_pixel(qts, 102, 100), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 102), ==, 0);

    board_quit(qts);
}

/* Decreasing box endpoints walk the same payload in the opposite directions. */
static void test_command_fifo_image_raster_descending(void)
{
    QTestState *qts = board_start("");

    image_begin(qts, 20, 20, 18, 18);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x778899));
    cmd_push(qts, ODSY_X_PIXEL(0xaabbcc));
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 19, 19), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 18, 19), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 19, 18), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 18, 18), ==, 0xaabbccff);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 20), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 17, 19), ==, 0);

    board_quit(qts);
}

/* Mixed endpoint directions still advance X first, then Y. */
static void test_command_fifo_image_raster_mixed_directions(void)
{
    QTestState *qts = board_start("");

    image_begin(qts, 30, 30, 28, 32);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_X_PIXEL(0x778899));
    cmd_push(qts, ODSY_X_PIXEL(0xaabbcc));
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 29, 30), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 28, 30), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 29, 31), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 28, 31), ==, 0xaabbccff);
    g_assert_cmphex(odsy_test_pixel(qts, 30, 30), ==, 0);

    image_begin(qts, 40, 40, 42, 38);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0xddeeff));
    cmd_push(qts, ODSY_X_PIXEL(0x123456));
    cmd_push(qts, ODSY_X_PIXEL(0x654321));
    cmd_push(qts, ODSY_X_PIXEL(0xabcdef));
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 40, 39), ==, 0xddeeffff);
    g_assert_cmphex(odsy_test_pixel(qts, 41, 39), ==, 0x123456ff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 38), ==, 0x654321ff);
    g_assert_cmphex(odsy_test_pixel(qts, 41, 38), ==, 0xabcdefff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 40), ==, 0);

    board_quit(qts);
}

/* An equal endpoint empties its axis without consuming the next image. */
static void test_command_fifo_image_raster_empty_extent(void)
{
    QTestState *qts = board_start("");

    image_begin(qts, 50, 50, 50, 52);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_CMD_GL_END);

    image_begin(qts, 55, 55, 57, 55);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x778899));
    cmd_push(qts, ODSY_X_PIXEL(0xaabbcc));
    cmd_push(qts, ODSY_CMD_GL_END);

    image_begin(qts, 60, 60, 62, 61);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x778899));
    cmd_push(qts, ODSY_X_PIXEL(0xaabbcc));
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 50, 50), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 51), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 55, 55), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 55), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 60, 60), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 61, 60), ==, 0xaabbccff);

    board_quit(qts);
}

/* A decreasing interval may start outside the screen and clip per pixel. */
static void test_command_fifo_image_raster_clips_descending(void)
{
    QTestState *qts = board_start("");

    image_begin(qts, 1281, 10, 1279, 11);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 1279, 10), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 1278, 10), ==, 0);

    board_quit(qts);
}

/* Screen image PIO adds the signed window origin before screen checks. */
static void test_command_fifo_image_raster_window_origin(void)
{
    QTestState *qts = board_start("");

    window_origin(qts, 8, 6);
    image_begin(qts, 2, 3, 4, 4);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 9), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 9), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 2, 3), ==, 0);

    window_origin(qts, 0x1ffe, 0x1ffe);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    image_begin(qts, 1, 1, 3, 3);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0xff0000));
    cmd_push(qts, ODSY_X_PIXEL(0x00ff00));
    cmd_push(qts, ODSY_X_PIXEL(0x0000ff));
    cmd_push(qts, ODSY_X_PIXEL(0xffffff));
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0xffffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 1), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1, 0), ==, 0);

    window_origin(qts, 100, 200);
    window_clip(qts, (const uint32_t[][4]) { { 102, 201, 102, 201 } }, 1);
    image_begin(qts, 1, 1, 3, 3);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0xff0000));
    cmd_push(qts, ODSY_X_PIXEL(0x00ff00));
    cmd_push(qts, ODSY_X_PIXEL(0x0000ff));
    cmd_push(qts, ODSY_X_PIXEL(0xffffff));
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 102, 201), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 201), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 102, 202), ==, 0);

    window_origin(qts, 8, 9);
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x16c000);
    cmd_push(qts, 2);
    cmd_push(qts, 0x080024d0);
    cmd_push(qts, 0);
    image_begin(qts, 2, 3, 3, 4);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(1, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0xabcdef));
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c000, 2, 3), ==,
                    0xabcdefff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c000, 10, 12), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 12), ==, 0);

    board_quit(qts);
}

/*
 * The header's format code, not the burst tag, lays out the bytes: 0x4b one
 * 0xRRGGBBAA word a pixel, 0x49 three packed bytes (a pixel may span two
 * words), 0x42 one luminance byte. All three under the same 0x11 tag the X
 * server's 0x00RRGGBB icons use.
 */
static void test_command_fifo_image_raster_formats(void)
{
    QTestState *qts = board_start("");

    image_begin(qts, 100, 100, 102, 101);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x4b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, 0x11223344);
    cmd_push(qts, 0x55667788);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 100), ==, 0x556677ff);

    image_begin(qts, 200, 200, 204, 201);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x49));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(3, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, 0x10203040);
    cmd_push(qts, 0x50607080);
    cmd_push(qts, 0x90a0b0c0);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 200), ==, 0x102030ff);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 200), ==, 0x405060ff);
    g_assert_cmphex(odsy_test_pixel(qts, 202, 200), ==, 0x708090ff);
    g_assert_cmphex(odsy_test_pixel(qts, 203, 200), ==, 0xa0b0c0ff);

    image_begin(qts, 300, 300, 304, 301);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x42));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x00408cff);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 300), ==, 0x000000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 301, 300), ==, 0x404040ff);
    g_assert_cmphex(odsy_test_pixel(qts, 302, 300), ==, 0x8c8c8cff);
    g_assert_cmphex(odsy_test_pixel(qts, 303, 300), ==, 0xffffffff);

    board_quit(qts);
}

/* SHORT groups count 32-bit words and carry two RGBA16 words per pixel. */
static void test_command_fifo_image_raster_short(void)
{
    QTestState *qts = board_start("");

    image_begin(qts, 64, 64, 66, 62);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x6b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_SHORT));
    cmd_push(qts, 0x11112222);
    cmd_push(qts, 0x3333ffff);
    cmd_push(qts, 0x44445555);
    cmd_push(qts, 0x6666ffff);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x6b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_SHORT));
    cmd_push(qts, 0x77778888);
    cmd_push(qts, 0x9999ffff);
    cmd_push(qts, 0xaaaabbbb);
    cmd_push(qts, 0xccccffff);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 64, 63), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 65, 63), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 62), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 65, 62), ==, 0xaabbccff);
    g_assert_cmphex(odsy_test_pixel(qts, 66, 63), ==, 0);

    image_begin(qts, 80, 80, 81, 81);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x6b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_SHORT));
    cmd_push(qts, 0x01000182);
    cmd_push(qts, 0x03ff0400);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 80, 80), ==, 0x01020404);

    board_quit(qts);
}

/* A partial SHORT pixel cannot cross its group or start another format. */
static void test_command_fifo_image_raster_short_incomplete(void)
{
    QTestState *qts = board_start("");

    odsy_test_pixel_write(qts, 70, 70, 0x12345678);
    image_begin(qts, 70, 70, 71, 71);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(1, 0x6b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_SHORT));
    cmd_push(qts, 0xdeadbeef);
    g_assert_cmphex(odsy_test_pixel(qts, 70, 70), ==, 0x12345678);

    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x6b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_SHORT));
    cmd_push(qts, 0x11112222);
    cmd_push(qts, 0x3333ffff);
    g_assert_cmphex(odsy_test_pixel(qts, 70, 70), ==, 0x112233ff);

    odsy_test_pixel_write(qts, 71, 70, 0x87654321);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_SHORT));
    cmd_push(qts, 0x11112222);
    cmd_push(qts, 0x3333ffff);
    g_assert_cmphex(odsy_test_pixel(qts, 71, 70), ==, 0x87654321);

    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(1, 0x6b));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x00112233);
    g_assert_cmphex(odsy_test_pixel(qts, 71, 70), ==, 0x87654321);
    cmd_push(qts, ODSY_CMD_GL_END);
    board_quit(qts);
}

/*
 * BEGIN-0xa image raster, format 0x10: four colour indices per word. This pins
 * the colormap lookup, index 0 painted like any other index (black in the X
 * server's colormap: a bitmap's foreground in xbiff and Inventor's viewer
 * buttons), and a bounded group's padding being dropped so it never wraps
 * into the next row.
 */
static void test_command_fifo_image_raster_colour_index(void)
{
    QTestState *qts = board_start("");

    /* Drawable 1 -> group 17; index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);

    /* A green background (index 1) under the whole region the images cover. */
    index_fill(qts, 1, 200, 200, 204, 204);
    index_fill(qts, 1, 300, 300, 302, 303);
    g_assert_cmphex(odsy_shown(qts, 200, 200), ==, 0x00ff00ff);

    /*
     * A 4x1 image whose one word packs indices 0, 2, 0, 2 (leftmost in bits
     * 31:24) over the green background: index 0 paints red, index 2 blue.
     */
    image_begin(qts, 200, 200, 204, 201);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x41));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x00020002);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_shown(qts, 200, 200), ==, 0xff0000ffu); /* index 0 */
    g_assert_cmphex(odsy_shown(qts, 201, 200), ==, 0x0000ffff); /* index 2 */
    g_assert_cmphex(odsy_shown(qts, 202, 200), ==, 0xff0000ffu); /* index 0 */
    g_assert_cmphex(odsy_shown(qts, 203, 200), ==, 0x0000ffff); /* index 2 */

    /*
     * A 2-wide, 3-tall box whose first group is 2 pixels but whose word holds
     * two more (indices 7, 7, an unpopulated black). The padding must be
     * dropped, not wrapped down into row 301, which stays the green background.
     */
    image_begin(qts, 300, 300, 302, 303);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x41));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x02020707);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_shown(qts, 300, 300), ==, 0x0000ffff); /* index 2 */
    g_assert_cmphex(odsy_shown(qts, 301, 300), ==, 0x0000ffff); /* index 2 */
    /* Row 301 keeps the green background: the padding never reached it. */
    g_assert_cmphex(odsy_shown(qts, 300, 301), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, 301, 301), ==, 0x00ff00ff);

    board_quit(qts);
}

/*
 * Source format 0x61, as IRIS GL writepixels sends it (mandel): a 16-bit
 * colour index a pixel, two to a word, high half first. A 3x1 image over an
 * index-1 background takes the indices 0x168, 0x002 and 0xfff, and the
 * fourth half, past the header's count of 3, is dropped. Read a byte at a
 * time, the word 0x01680002 would paint indices 1, 0x68 and 0.
 */
static void test_command_fifo_image_raster_colour_index16(void)
{
    QTestState *qts = board_start("");

    index_fill(qts, 1, 200, 200, 204, 201);
    image_begin(qts, 200, 200, 203, 201);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(3, 0x61));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x01680002);
    cmd_push(qts, 0x0fff0007);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 200, 200), ==, 0x168);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 200), ==, 0x002);
    g_assert_cmphex(odsy_test_pixel(qts, 202, 200), ==, 0xfff);
    g_assert_cmphex(odsy_test_pixel(qts, 203, 200), ==, 1);

    board_quit(qts);
}

/*
 * A colour-index image wider than one burst, as the X server sends its root
 * background: a 56x3 box whose first group is a header with count 0 and
 * three full 16-pixel bursts, and whose later groups each start with a
 * header of count 8 (bit 16 set), an 8-pixel burst that finishes the
 * previous row, then three full bursts. Every pixel of the first two rows
 * and the first 48 of the third is painted; the last 8 of the third stay
 * the background.
 */
static void test_command_fifo_image_raster_colour_index_groups(void)
{
    QTestState *qts = board_start("");
    unsigned int g, b, w;

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID0, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);
    index_fill(qts, 1, 400, 400, 456, 403);

    image_begin(qts, 400, 400, 456, 403);
    for (g = 0; g < 3; g++) {
        cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
        cmd_push(qts, g ? 0x0f811041 : 0x0f001041);
        cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
        if (g) {
            cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_CI));
            cmd_push(qts, 0x02020202);
            cmd_push(qts, 0x02020202);
        }
        for (b = 0; b < 3; b++) {
            cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_CI));
            for (w = 0; w < 4; w++) {
                cmd_push(qts, 0x02020202);
            }
        }
    }
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_shown(qts, 447, 400), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 455, 400), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 420, 401), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 455, 401), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 447, 402), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 448, 402), ==, 0x00ff00ff);

    board_quit(qts);
}

/*
 * The true-colour OpenGL path: a flat single triangle drawn through the shared
 * GL context and software rasterizer (hw/display/sgi/gl-context.h,
 * rasterizer.h), routed off the greeter's integer 2D path by its float words.
 * The word stream is the probe's identity triangle: glColor3f(1, 1, 0)
 * yellow, glBegin(GL_TRIANGLES), three glVertex2f in normalized device
 * coordinates, glEnd. Where it lands comes from the client's window
 * transform (matrix slot 0x00, the viewport) and the kernel's window
 * origin.
 */
#define ODSY_GL_COLOR_3F 0xc580cc0e
#define ODSY_GL_VERTEX_2F 0x8080c801
#define ODSY_GL_TRIANGLES 0x00000004
#define ODSY_GL_QUADS 0x00000007
#define ODSY_GL_QUAD_STRIP 0x00000008
#define ODSY_GL_TEX_RECT 0x8f80b968
#define ODSY_GL_MATRIX_SELECT_WINDOW 0x34008000
#define ODSY_GL_MATRIX_SELECT_TEXTURE 0x34008014
#define ODSY_GL_MATRIX_LOAD 0x9080bdc8
#define ODSY_GL_RECT 0x8f80a944

static void push_captured_triangle(QTestState *qts)
{
    /* glColor3f(1.0, 1.0, 0.0): yellow, as raw IEEE-754 float bits. */
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x00000000);
    /* glBegin(GL_TRIANGLES). */
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    /* glVertex2f(-0.75, -0.75). */
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, 0xbf400000);
    /* glVertex2f(0.75, -0.625). */
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0xbf200000);
    /* glVertex2f(-0.125, 0.734375): the apex. */
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbe000000);
    user_cmd_push(qts, 0x3f3c0000);
    /* glEnd(): the context assembled the triangle on the third vertex. */
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

/*
 * The window transform the probe loads through slot 0x00 for its 128x128
 * window, row-major on the wire: x' = 64x + 64, y' = -64y + 63.996, z
 * scaled by 2^32. Window row 0 is the top.
 */
static const uint32_t odsy_captured_window_transform[16] = {
    0x42800000, 0x00000000, 0x00000000, 0x42800000,
    0x00000000, 0xc2800000, 0x00000000, 0x427ffc00,
    0x00000000, 0x00000000, 0x4f7fffc0, 0x4f7fffc0,
    0x00000000, 0x00000000, 0x00000000, 0x3f800000,
};

/*
 * The same shape for a 200x150 window, as the library builds it:
 * 100, 0, 0, 100 / 0, -75, 0, 74.99609375 / 0, 0, 2^32, 2^32 / 0, 0, 0, 1.
 */
static const uint32_t odsy_window_transform_200x150[16] = {
    0x42c80000, 0x00000000, 0x00000000, 0x42c80000,
    0x00000000, 0xc2960000, 0x00000000, 0x4295fe00,
    0x00000000, 0x00000000, 0x4f800000, 0x4f800000,
    0x00000000, 0x00000000, 0x00000000, 0x3f800000,
};

static void push_window_transform(QTestState *qts, const uint32_t m[16])
{
    unsigned i;

    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_WINDOW);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    for (i = 0; i < 16; i++) {
        user_cmd_push(qts, m[i]);
    }
}

/*
 * The four floats the library loads behind matrix index 16 after every
 * combined matrix, (-0, -0, -0, fourth); the fourth's sign orients depth.
 */
#define ODSY_GL_MATRIX_SELECT_16 0x34008010
#define ODSY_GL_LOAD_4F 0x908091df

static void push_block_16(QTestState *qts, uint32_t fourth)
{
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_16);
    user_cmd_push(qts, ODSY_GL_LOAD_4F);
    user_cmd_push(qts, 0x80000000);
    user_cmd_push(qts, 0x80000000);
    user_cmd_push(qts, 0x80000000);
    user_cmd_push(qts, fourth);
}

/*
 * A 128x128 window at the screen origin under the reset clip: the triangle
 * lands in window pixels -- vertices (16, 112), (112, 104) and the apex
 * (56, 17), row 0 at the top -- so its apex is up, as the probe's GL
 * coordinates intend. The model's old whole-frame mapping drew it upside
 * down, apex at the bottom.
 */
static void test_command_fifo_gl_integer_triangle(void)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t draw[] = {
        0x00010405, 0x2000,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 30, 40,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 70, 50,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 40, 85,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    /* Literal public vertex2i packet, combined matrix and depth scale. */
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    for (unsigned i = 0; i < ARRAY_SIZE(draw); i++) {
        user_cmd_push(qts, draw[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 45), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 30, 40), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 70, 77), ==, 0);
    board_quit(qts);
}

/*
 * An unmodeled word still names its operand count in bits 13:10, and the
 * parser passes over that many words: here 0x3cf8, a 16-operand word the
 * kernel's context restore sends, whose 16 operands happen to spell a
 * whole yellow triangle. Read as commands, they drew it; passed over, they
 * draw nothing, and the same triangle sent next draws. A GL-class word with
 * bit 23 clear, the restore's 0x9000bda2, takes none: the triangle right
 * after it draws.
 */
static void test_command_fifo_unknown_word_skips_its_operands(void)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t triangle[16] = {
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        0x8080c800, 30, 40,
        0x8080c800, 70, 50,
        0x8080c800, 40, 85,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    user_cmd_push(qts, 0x00010405);
    user_cmd_push(qts, 0x2000);
    user_cmd_push(qts, 0x00003cf8);
    for (unsigned i = 0; i < ARRAY_SIZE(triangle); i++) {
        user_cmd_push(qts, triangle[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(triangle); i++) {
        user_cmd_push(qts, triangle[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 0xffff00ff);
    board_quit(qts);

    qts = board_start("");
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    user_cmd_push(qts, 0x00010405);
    user_cmd_push(qts, 0x2000);
    user_cmd_push(qts, 0x9000bda2);
    for (unsigned i = 0; i < ARRAY_SIZE(triangle); i++) {
        user_cmd_push(qts, triangle[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 0xffff00ff);
    board_quit(qts);
}

/*
 * A triangle queued behind the store that carries it is drawn on the
 * board's thread as the clock steps, without the big lock, to the pixels
 * the synchronous board drew (test_command_fifo_gl_integer_triangle).
 */
static void test_command_work_draws_on_board_thread(void)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t draw[] = {
        0x00010405, 0x2000,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 30, 40,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 70, 50,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 40, 85,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = work_start();

    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    for (unsigned i = 0; i < ARRAY_SIZE(draw); i++) {
        user_cmd_push(qts, draw[i]);
    }
    g_assert_true(work_pending(qts));
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 0);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-unlocked-draws"), ==, 0);
    while (work_pending(qts)) {
        qtest_clock_step(qts, 1000 * 1000);
    }
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-unlocked-draws"), ==, 1);
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 45), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 30, 40), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 70, 77), ==, 0);
    board_quit(qts);
}

/*
 * Blending is for RGBA fragments only (OpenGL 1.2.1, 4.1.6): a colour-index
 * drawable with 0x0404 bit 17 set and glBlendFunc(GL_SRC_ALPHA,
 * GL_ONE_MINUS_SRC_ALPHA) (0x0c21 0x639) draws its index unchanged and
 * reports no missing read-back planes.
 */
static void test_command_fifo_gl_index_does_not_blend(void)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t draw[] = {
        0x22008001,
        0x00010c21, 0x639, 0, 0,
        0x00010404, 0x20000,
        0xc580c406, 4,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        0x8080c800, 30, 40,
        0x8080c800, 70, 50,
        0x8080c800, 40, 85,
        ODSY_CMD_GL_END,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-index-blend-XXXXXX.log", &log_path,
                          &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    for (unsigned i = 0; i < ARRAY_SIZE(draw); i++) {
        user_cmd_push(qts, draw[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 46, 69), ==, 4);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "need planes that can be read back"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The X server's FillOpaqueStippled polygon in a colour-index drawable, as
 * 4Dwm fills its icons' checkered faces (words from a traced xrefresh):
 * 0x0404 0xa0018 sets the pattern bits, blend and index logic op, 0x0c21
 * names background index 0xb with factors ONE, ZERO, 0x043f 0x10 selects
 * the GL stipple, opaque, and a 50% checker is loaded before the integer
 * triangle in index 4. Set stipple bits take index 4 and clear ones 0xb.
 */
static void test_command_fifo_gl_opaque_stipple_index(void)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t setup[] = {
        0x22008001,
        0x00010404, 0xa0018,
        0x00010c21, 0x200, 0xb000, 0x1,
        0xc580c406, 4,
        0x00010422, 3,
        0x0001043f, 0x10,
    };
    const uint32_t draw[] = {
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        0x8080c800, 30, 40,
        0x8080c800, 70, 50,
        0x8080c800, 40, 85,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");
    uint32_t a, b;

    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    for (unsigned i = 0; i < ARRAY_SIZE(setup); i++) {
        user_cmd_push(qts, setup[i]);
    }
    for (unsigned load = 0; load < 2; load++) {
        user_cmd_push(qts, load ? 0x00013c4f : 0x00013c4e);
        for (unsigned i = 0; i < 16; i++) {
            user_cmd_push(qts, i & 1 ? 0x55555555 : 0xaaaaaaaa);
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(draw); i++) {
        user_cmd_push(qts, draw[i]);
    }
    a = odsy_test_pixel(qts, 46, 69);
    b = odsy_test_pixel(qts, 47, 69);
    g_assert_true((a == 4 && b == 0xb) || (a == 0xb && b == 4));
    g_assert_cmphex(odsy_test_pixel(qts, 46, 70), ==, b);
    g_assert_cmphex(odsy_test_pixel(qts, 30, 80), ==, 0);
    board_quit(qts);
}

static void test_command_fifo_gl_integer_points_transform(void)
{
    const uint32_t matrix[16] = {
        0x3fc00000, 0, 0, 0x41c20000,
        0, 0xbfc00000, 0, 0x42db3e00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t draw[] = {
        0x00010404, 0x02003200,
        0x33008000, 0x290080d6, 0x2a0080e0, 0x2c0080ea,
        ODSY_CMD_GL_BEGIN, 0,
        ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
        0x8080c800, 2, 3,
        ODSY_GL_COLOR_3F, 0, 0x3f800000, 0,
        0x8080c800, 10, 4,
        ODSY_GL_COLOR_3F, 0, 0, 0x3f800000,
        0x8080c800, 15, 10,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0,
        0x8080c800, 8, 18,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    /* Literal paired public points, full combined transform and depth sign. */
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    for (unsigned i = 0; i < ARRAY_SIZE(draw); i++) {
        user_cmd_push(qts, draw[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 27, 105), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 39, 103), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 46, 94), ==, 0x0000ffff);
    g_assert_cmphex(odsy_test_pixel(qts, 36, 82), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 2, 3), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 4), ==, 0);
    board_quit(qts);
}

static void integer_transform_pair(unsigned mode)
{
    const uint32_t matrix[16] = {
        0x3fc00000, 0, 0, 0x41c20000,
        0, 0xbfc00000, 0, 0x42db3e00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t colours[3][5][3] = {
        { { 0x3f800000, 0, 0 }, { 0, 0x3f800000, 0 },
          { 0, 0, 0x3f800000 }, { 0x3f800000, 0x3f800000, 0 } },
        { { 0x3f800000, 0, 0 }, { 0, 0x3f800000, 0 },
          { 0, 0, 0x3f800000 }, { 0x3f800000, 0x3f800000, 0x3f800000 } },
        { { 0x3f800000, 0, 0x3f800000 }, { 0x3f800000, 0, 0 },
          { 0, 0x3f800000, 0 }, { 0, 0, 0x3f800000 },
          { 0, 0x3f800000, 0x3f800000 } },
    };
    const uint32_t xy[3][5][2] = {
        { { 0, 0 }, { 20, 0 }, { 0, 6 }, { 0, 20 } },
        { { 0, 0 }, { 20, 0 }, { 20, 20 }, { 0, 20 } },
        { { 0, 0 }, { 20, 0 }, { 24, 12 }, { 16, 22 }, { 4, 20 } },
    };
    unsigned shape = mode == 1 ? 0 : mode == 7 ? 1 : 2;
    unsigned count = mode == 9 ? 5 : 4;

    /* Matched float control first, then the literal integer vertex shape. */
    for (unsigned integer = 0; integer < 2; integer++) {
        QTestState *qts = board_start("");

        push_window_transform(qts, matrix);
        push_block_16(qts, 0xbf800000);
        user_cmd_push(qts, 0x00010404);
        user_cmd_push(qts, 0x02003200);
        user_cmd_push(qts, 0x33008000);
        user_cmd_push(qts, 0x290080d6);
        user_cmd_push(qts, 0x2a0080e0);
        user_cmd_push(qts, 0x2c0080ea);
        user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
        user_cmd_push(qts, mode);
        for (unsigned v = 0; v < count; v++) {
            user_cmd_push(qts, ODSY_GL_COLOR_3F);
            for (unsigned c = 0; c < 3; c++) {
                user_cmd_push(qts, colours[shape][v][c]);
            }
            user_cmd_push(qts, integer ? 0x8080c800 : 0x8080c801);
            for (unsigned axis = 0; axis < 2; axis++) {
                uint32_t word = xy[shape][v][axis];

                if (!integer) {
                    float value = word;

                    memcpy(&word, &value, sizeof(word));
                }
                user_cmd_push(qts, word);
            }
        }
        user_cmd_push(qts, ODSY_CMD_GL_END);
        g_test_message("checking %s vertex input",
                       integer ? "integer" : "float");
        if (mode == 1) {
            g_assert_cmphex(odsy_test_pixel(qts, 40, 109), ==, 0x00ff00ff);
            g_assert_cmphex(odsy_test_pixel(qts, 24, 90), ==, 0xffff00ff);
        } else {
            g_assert_cmphex(odsy_test_pixel(qts, 40, 90), ==,
                            mode == 7 ? 0xffffffff : 0xff00ffff);
        }
        board_quit(qts);
    }
}

static void test_command_fifo_gl_integer_lines_transform(void)
{
    integer_transform_pair(1);
}

static void test_command_fifo_gl_integer_quads_transform(void)
{
    integer_transform_pair(7);
}

static void test_command_fifo_gl_integer_polygon_transform(void)
{
    integer_transform_pair(9);
}

/* Original translation fixture: (2, 3) becomes (42, 63). */
static void integer_routing_setup(QTestState *qts, uint32_t mode)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0x42200000,
        0, 0x3f800000, 0, 0x42700000,
        0, 0, 0x3f800000, 0,
        0, 0, 0, 0x3f800000,
    };

    push_window_transform(qts, matrix);
    user_cmd_push(qts, 0x00010404);
    user_cmd_push(qts, mode);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_3UB);
    user_cmd_push(qts, 255);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 255);
}

static void integer_routing_point(QTestState *qts)
{
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    user_cmd_push(qts, 2);
    user_cmd_push(qts, 3);
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

static void test_command_fifo_gl_integer_routing_modes(void)
{
    const uint32_t modes[] = {
        0x02003200, 0x02002200, 0x02013200, 0x02003200,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
        QTestState *qts = board_start("");

        integer_routing_setup(qts, modes[i]);
        if (i == 3) {
            user_cmd_push(qts, 0x22008001);
            user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
            user_cmd_push(qts, 7);
        }
        integer_routing_point(qts);
        g_assert_cmphex(odsy_test_pixel(qts, 42, 63), ==,
                        i == 3 ? 7 : 0xff00ffff);
        g_assert_cmphex(odsy_test_pixel(qts, 2, 3), ==, 0);
        board_quit(qts);
    }
}

/*
 * Both X11 formats ignore a combined matrix left by a preceding GL draw.
 * The captured CapNotLast line mode also exercises synthetic points and fills.
 */
static void test_command_fifo_x11_integer_after_gl(void)
{
    const uint32_t modes[] = { 0x00080000, 0x00100000, 0x02100000 };

    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
        QTestState *qts = board_start("");
        uint32_t expected = i == 0 ? 7 : 0xff00ffff;

        integer_routing_setup(qts, 0x02003200);
        integer_routing_point(qts);
        g_assert_cmphex(odsy_test_pixel(qts, 42, 63), ==, 0xff00ffff);
        user_cmd_push(qts, 0x00010404);
        user_cmd_push(qts, modes[i]);
        if (i == 0) {
            user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
            user_cmd_push(qts, 7);
        }
        g_test_message("X11 mode 0x%08x", modes[i]);
        window_origin(qts, 100, 100);
        integer_routing_point(qts);
        g_assert_cmphex(odsy_test_pixel(qts, 102, 103), ==, expected);
        g_assert_cmphex(odsy_test_pixel(qts, 142, 163), ==, 0);
        user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
        user_cmd_push(qts, ODSY_GL_LINES);
        vertex2i(qts, 4, 8);
        vertex2i(qts, 12, 8);
        user_cmd_push(qts, ODSY_CMD_GL_END);
        g_assert_cmphex(odsy_test_pixel(qts, 108, 108), ==, expected);
        for (unsigned mode = 7; mode <= 9; mode += 2) {
            unsigned x = mode == 7 ? 20 : 30;

            user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
            user_cmd_push(qts, mode);
            vertex2i(qts, x, 4);
            vertex2i(qts, x, 12);
            vertex2i(qts, x + 8, 12);
            vertex2i(qts, x + 8, 4);
            user_cmd_push(qts, ODSY_CMD_GL_END);
            g_assert_cmphex(odsy_test_pixel(qts, 104 + x, 108), ==, expected);
        }
        board_quit(qts);
    }
}

static void test_command_fifo_gl_integer_routing_migration_reset(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-integer-routing-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    integer_routing_setup(from, 0x02003200);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    integer_routing_point(to);
    g_assert_cmphex(odsy_test_pixel(to, 42, 63), ==, 0xff00ffff);
    g_assert_cmphex(odsy_test_pixel(to, 2, 3), ==, 0);
    qtest_system_reset(to);
    user_cmd_push(to, ODSY_CMD_GL_COLOR_3UB);
    user_cmd_push(to, 255);
    user_cmd_push(to, 0);
    user_cmd_push(to, 255);
    integer_routing_point(to);
    g_assert_cmphex(odsy_test_pixel(to, 2, 3), ==, 0xff00ffff);
    g_assert_cmphex(odsy_test_pixel(to, 42, 63), ==, 0);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void x11_captured_line_cap(bool not_last, unsigned shape, bool indexed)
{
    const uint32_t xy[][2][2] = {
        { { 4, 8 }, { 12, 8 } },
        { { 12, 8 }, { 4, 8 } },
        { { 10, 10 }, { 10, 10 } },
        { { 20, 4 }, { 20, 12 } },
        { { 20, 12 }, { 20, 4 } },
    };
    uint32_t mode = (indexed ? 0x00080000 : 0x00100000) |
                    (not_last ? 0x02000000 : 0);
    uint32_t white = indexed ? 7 : 0xffffffff;
    const uint32_t words[] = {
        0x00010404, mode,
        0x00010422, 3,
        0x00014400, 1,
        0x8080c800, xy[shape][0][0], xy[shape][0][1],
        0x8080c800, xy[shape][1][0], xy[shape][1][1],
        0x00014001,
    };
    QTestState *qts = board_start("");
    unsigned mismatches = 0;
    uint32_t scan[32 * 32];

    /* An original GL translation must not affect the captured X11 line. */
    integer_routing_setup(qts, 0x02003200);
    integer_routing_point(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 42, 63), ==, 0xff00ffff);
    if (indexed) {
        colour_index(qts, 7);
    } else {
        user_cmd_push(qts, 0xc580cc08);
        user_cmd_push(qts, 255);
        user_cmd_push(qts, 255);
        user_cmd_push(qts, 255);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        user_cmd_push(qts, words[i]);
    }
    g_test_message("shape %u, mode 0x%08x", shape, mode);
    odsy_test_pixels(qts, 0, 0, 32, 32, scan);
    for (unsigned y = 0; y < 32; y++) {
        for (unsigned x = 0; x < 32; x++) {
            bool covered = shape == 2 ? x == 10 && y == 10 :
                           shape >= 3 ? x == 20 && y >= 4 && y <= 12 :
                                        y == 8 && x >= 4 && x <= 12;
            uint32_t expected, actual;

            if (not_last && x == xy[shape][1][0] && y == xy[shape][1][1]) {
                covered = false;
            }
            expected = covered ? white : 0;
            actual = scan[y * 32 + x];
            if (actual != expected) {
                g_test_message("pixel (%u,%u): expected 0x%08x, got 0x%08x",
                               x, y, expected, actual);
                mismatches++;
            }
        }
    }
    board_quit(qts);
    g_assert_cmpuint(mismatches, ==, 0);
}

/* Default and round caps emitted identical packets in the TrueColor capture. */
static void test_command_fifo_x11_line_cap_default(void)
{
    x11_captured_line_cap(false, 0, false);
    x11_captured_line_cap(false, 0, true);
}

static void test_command_fifo_x11_line_cap_not_last(void)
{
    x11_captured_line_cap(true, 0, false);
    x11_captured_line_cap(true, 0, true);
}

static void test_command_fifo_x11_line_cap_reverse_default(void)
{
    x11_captured_line_cap(false, 1, false);
    x11_captured_line_cap(false, 1, true);
}

static void test_command_fifo_x11_line_cap_reverse_not_last(void)
{
    x11_captured_line_cap(true, 1, false);
    x11_captured_line_cap(true, 1, true);
}

static void test_command_fifo_x11_line_cap_zero_default(void)
{
    x11_captured_line_cap(false, 2, false);
    x11_captured_line_cap(false, 2, true);
}

static void test_command_fifo_x11_line_cap_zero_not_last(void)
{
    x11_captured_line_cap(true, 2, false);
    x11_captured_line_cap(true, 2, true);
}

/*
 * Vertical direction is a synthetic mechanism check, not a native cap capture.
 */
static void test_command_fifo_x11_line_cap_vertical(void)
{
    for (unsigned indexed = 0; indexed < 2; indexed++) {
        for (unsigned not_last = 0; not_last < 2; not_last++) {
            x11_captured_line_cap(not_last, 3, indexed);
            x11_captured_line_cap(not_last, 4, indexed);
        }
    }
}

/* Synthetic off-screen int32 endpoints must visit only framebuffer pixels. */
static void test_command_fifo_x11_line_cap_screen_bounds(void)
{
    for (unsigned not_last = 0; not_last < 2; not_last++) {
        uint32_t mode = 0x00100000 | (not_last ? 0x02000000 : 0);
        const uint32_t words[] = {
            0x00010404, mode, 0xc580cc08, 255, 255, 255,
            0x00010422, 3, 0x00014400, 1,
            0x8080c800, 0x80000000, 0, 0x8080c800, 0x7fffffff, 0,
            0x8080c800, 1279, 0x80000000, 0x8080c800, 1279, 0x7fffffff,
            0x8080c800, 0, 5, 0x8080c800, 1278, 5,
            0x8080c800, 1278, 1023, 0x8080c800, 1278, 1023,
            0x00014001,
        };
        QTestState *qts = board_start("");

        for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
            user_cmd_push(qts, words[i]);
        }
        g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0xffffffff);
        g_assert_cmphex(odsy_test_pixel(qts, 1279, 0), ==, 0xffffffff);
        g_assert_cmphex(odsy_test_pixel(qts, 1279, 1022), ==, 0xffffffff);
        g_assert_cmphex(odsy_test_pixel(qts, 1277, 5), ==, 0xffffffff);
        g_assert_cmphex(odsy_test_pixel(qts, 1278, 5), ==,
                        not_last ? 0 : 0xffffffff);
        /* The earlier vertical line paints the endpoint in either mode. */
        g_assert_cmphex(odsy_test_pixel(qts, 1279, 5), ==, 0xffffffff);
        g_assert_cmphex(odsy_test_pixel(qts, 1278, 1023), ==,
                        not_last ? 0 : 0xffffffff);
        board_quit(qts);
    }
}

static void test_command_fifo_gl_integer_assemblies(void)
{
    const uint32_t matrix[16] = {
        0x40000000, 0, 0, 0x42800000,
        0, 0xc0000000, 0, 0x42800000,
        0, 0, 0x4f7fffc0, 0x4f7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const int32_t vertex[4][2] = {
        { -20, -20 }, { 20, -20 }, { -20, 20 }, { 20, 20 },
    };
    const uint32_t primitive[] = { 4, 5, 6, 8 };
    const unsigned index[4][6] = {
        { 0, 1, 2, 1, 3, 2 }, { 0, 1, 2, 3 },
        { 0, 1, 3, 2 }, { 0, 1, 2, 3 },
    };

    for (unsigned n = 0; n < ARRAY_SIZE(primitive); n++) {
        QTestState *qts = board_start("");
        unsigned count = n == 0 ? 6 : 4;

        /* Signed coordinates are scaled and translated by the GL matrix. */
        push_window_transform(qts, matrix);
        user_cmd_push(qts, ODSY_GL_COLOR_3F);
        user_cmd_push(qts, 0x3f800000);
        user_cmd_push(qts, 0x3f800000);
        user_cmd_push(qts, 0);
        user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
        user_cmd_push(qts, primitive[n]);
        for (unsigned i = 0; i < count; i++) {
            user_cmd_push(qts, 0x8080c800);
            user_cmd_push(qts, vertex[index[n][i]][0]);
            user_cmd_push(qts, vertex[index[n][i]][1]);
        }
        user_cmd_push(qts, ODSY_CMD_GL_END);
        g_assert_cmphex(odsy_test_pixel(qts, 32, 32), ==, 0xffff00ff);
        g_assert_cmphex(odsy_test_pixel(qts, 96, 32), ==, 0xffff00ff);
        g_assert_cmphex(odsy_test_pixel(qts, 32, 96), ==, 0xffff00ff);
        g_assert_cmphex(odsy_test_pixel(qts, 96, 96), ==, 0xffff00ff);
        g_assert_cmphex(odsy_test_pixel(qts, 20, 64), ==, 0);
        g_assert_cmphex(odsy_test_pixel(qts, 108, 64), ==, 0);
        board_quit(qts);
    }
}

static void test_command_fifo_gl_triangle(void)
{
    QTestState *qts = board_start("");

    push_window_transform(qts, odsy_captured_window_transform);
    push_captured_triangle(qts);

    /* The centroid (61, 78) is inside, in the plane and on the display. */
    g_assert_cmphex(odsy_test_pixel(qts, 61, 78), ==, 0xffff00ff);
    /* RGB shows through an RGB mode (drawable 0, the id plane at reset). */
    display_identity(qts, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 61, 78), ==, 0x00ffff00);
    /* Just under the apex is inside; just under the base is not. */
    g_assert_cmphex(odsy_test_pixel(qts, 56, 20), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 110), ==, 0);
    /* Outside the window and the triangle stays clear. */
    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 613, 402), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1275, 1019), ==, 0);

    board_quit(qts);
}

/*
 * glClear of the colour buffer. The GL library clears a window by sending,
 * inside BEGIN 7, a screen-aligned rectangle: 0x8f80a944, the clear colour
 * R, G, B, A and two corners (x, y, z) in window pixels. These are the
 * probe's words from its glClearColor(0, 0, 1, 1) clear of its 128x128
 * window.
 */
static const uint32_t odsy_captured_clear[] = {
    ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS,
    ODSY_GL_RECT,
    0x00000000, 0x00000000, 0x3f800000, 0x3f800000,     /* R G B A */
    0x00000000, 0x00000000, 0x4fffffc0,                 /* 0, 0, z */
    0x43000000, 0x43000000, 0x4fffffc0,                 /* 128, 128, z */
    ODSY_CMD_GL_END,
};

static void push_captured_clear(QTestState *qts)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(odsy_captured_clear); i++) {
        user_cmd_push(qts, odsy_captured_clear[i]);
    }
}

/*
 * A clear of a whole window in colour (r, g, b), corners (0, 0) and (w, h)
 * as the GL library's glClear implementation sends them; each argument is
 * IEEE-754 float bits.
 */
static void push_clear(QTestState *qts, uint32_t r, uint32_t g, uint32_t b,
                       uint32_t w, uint32_t h)
{
    const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS,
        ODSY_GL_RECT,
        r, g, b, 0x3f800000,
        0x00000000, 0x00000000, 0x4fffffc0,
        w, h, 0x4fffffc0,
        ODSY_CMD_GL_END,
    };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(words); i++) {
        user_cmd_push(qts, words[i]);
    }
}

#define ODSY_F_ZERO 0x00000000
#define ODSY_F_ONE 0x3f800000
#define ODSY_F_150 0x43160000
#define ODSY_F_200 0x43480000
#define ODSY_PLANE_BLUE 0x0000ffffu
#define ODSY_PLANE_RED 0xff0000ffu
#define ODSY_PLANE_YELLOW 0xffff00ffu

/*
 * The drawable-register block as perfly's context switch sends it, eight
 * 8-byte stores of a word and its one operand: 0x608 to 0x60f. Each of
 * 0x60a and 0x60c to 0x60f takes its operand, so none of the operands is
 * parsed as a command, and a clear sent after them still draws. The first
 * of those words is reported once, with its operand.
 */
static void test_command_fifo_drawable_words(void)
{
    static const uint32_t block[][2] = {
        { 0x608, 0xff00ff00 }, { 0x60a, 0x02680008 },
        { 0x609, 0xff00ff00 }, { 0x60b, 0x02680008 },
        { 0x60c, 0x004caef3 }, { 0x60d, 0x004b6a8b },
        { 0x60e, 0 }, { 0x60f, 0 },
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-drawable-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    for (unsigned i = 0; i < ARRAY_SIZE(block); i++) {
        cmd_push64(qts, block[i][0], block[i][1]);
    }
    push_captured_clear(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 100), ==, ODSY_PLANE_BLUE);

    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "command FIFO word"));
    g_assert_null(strstr(log, "drawable word 0x60c"));
    g_assert_nonnull(strstr(log, "drawable word 0x60a with operand 0x2680008"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The kernel driver's buffer clear: inside BEGIN 0xa, one 0x8f80a92f record
 * per strip of the window -- colour R, G, B, A and two corners (x, y, z) --
 * as ivview's clears arrive. Two strips side by side fill [0, 128) x
 * [0, 128) blue and nothing beyond it, and no word of either record is
 * parsed as a command.
 */
static void test_command_fifo_kernel_clear_box(void)
{
    static const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, 0xa,
        0x8f80a92f, 0, 0, ODSY_F_ONE, ODSY_F_ONE,
        0, 0, 0x4fffffc0, 0x42800000, 0x43000000, 0x4fffffc0,
        ODSY_CMD_GL_END,
        ODSY_CMD_GL_BEGIN, 0xa,
        0x8f80a92f, 0, 0, ODSY_F_ONE, ODSY_F_ONE,
        0x42800000, 0, 0x4fffffc0, 0x43000000, 0x43000000, 0x4fffffc0,
        ODSY_CMD_GL_END,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-clear-box-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        user_cmd_push(qts, words[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 120), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 130, 10), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 130), ==, 0);

    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "command FIFO word"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * With the origin at (0, 0) the rectangle's window pixels are plane pixels:
 * exactly [0, 128) x [0, 128) turns blue (plane word 0x0000ffff), the upper
 * edges exclusive, and nothing else changes.
 */
static void test_command_fifo_gl_clear_rect(void)
{
    QTestState *qts = board_start("");

    push_captured_clear(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 127, 127), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 100), ==, ODSY_PLANE_BLUE);
    /* RGB shows through an RGB mode (drawable 0, the id plane at reset). */
    display_identity(qts, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 64, 100), ==, 0x000000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 128, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 128), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 128, 128), ==, 0);

    board_quit(qts);
}

/*
 * The probe's own order: window transform, clear, triangle. The clear covers
 * the window and the triangle lands on the blue ground, apex up.
 */
static void test_command_fifo_gl_clear_then_triangle(void)
{
    QTestState *qts = board_start("");

    push_window_transform(qts, odsy_captured_window_transform);
    push_captured_clear(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 127, 127), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 128, 127), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1279, 1023), ==, 0);

    push_captured_triangle(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 61, 78), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 20), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 110), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 2, 2), ==, ODSY_PLANE_BLUE);

    board_quit(qts);
}

/*
 * The combined matrix the swap probe loads through slot 0x00 under
 * glOrtho(-2, 2, -2, 2, -1, 1) for the same 128x128 window, as the
 * probe's: 32, 0, 0, 64 / 0, -32, 0, 63.996 / -0, -0, 2^32, -2^32 / 0, 0,
 * 0, 1. The ortho's 0.5 scale is folded in with the window map.
 */
static const uint32_t odsy_captured_ortho_combined[16] = {
    0x42000000, 0x00000000, 0x00000000, 0x42800000,
    0x00000000, 0xc2000000, 0x00000000, 0x427ffc00,
    0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
    0x00000000, 0x00000000, 0x00000000, 0x3f800000,
};

/* A yellow glVertex2f triangle; v holds three x, y pairs as float bits. */
static void push_triangle(QTestState *qts, const uint32_t v[6])
{
    unsigned i;

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x00000000);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        user_cmd_push(qts, ODSY_GL_VERTEX_2F);
        user_cmd_push(qts, v[2 * i]);
        user_cmd_push(qts, v[2 * i + 1]);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

/*
 * A guest reset mid-primitive leaves the shared GL context's open polygon
 * vertex buffer still allocated: push_triangle() above always closes with
 * ODSY_CMD_GL_END first, but BEGIN alone (no END) leaves it open.
 * sgi_gl_init() discards that pointer by overwriting the whole context with
 * a zeroed one rather than freeing it first, so no guest-observable register
 * shows the leak; an address-sanitizer build's LeakSanitizer is what catches
 * it, at process exit. This test's own job is pinning that the reset still
 * completes and leaves the device responsive with the primitive left open.
 */
static void test_reset_mid_primitive_does_not_hang(void)
{
    QTestState *qts = board_start("");

    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x42200000);
    user_cmd_push(qts, 0x42200000);
    /* No ODSY_CMD_GL_END: reset lands with the polygon still open. */

    qtest_system_reset(qts);

    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, SLOT);

    board_quit(qts);
}

/*
 * The projection arrives folded into slot 0x00. Under that ortho matrix
 * the probe's triangle lands at half size about the window centre --
 * (40, 88), (88, 84) and the apex (60, 40.5) -- so pixels the identity
 * triangle covers near its corners stay clear. A second triangle whose
 * object coordinates run past 1, (1, -1) .. (1.75, 0), still lies inside
 * the window, at (96, 96) .. (120, 64), and is drawn: the view volume is the
 * ortho's -2..2, not the model's earlier reading of the matrix as a smaller
 * viewport, which clipped at x 96.
 */
static void test_command_fifo_gl_ortho_combined_matrix(void)
{
    static const uint32_t outside_unit[6] = {
        0x3f800000, 0xbf800000,     /* (1, -1) */
        0x3fe00000, 0xbf800000,     /* (1.75, -1) */
        0x3fe00000, 0x00000000,     /* (1.75, 0) */
    };
    QTestState *qts = board_start("");

    push_window_transform(qts, odsy_captured_ortho_combined);
    /*
     * The library follows the load with the four floats behind index 16; an
     * ortho with the negated depth row sends -1, which keeps z = 0 at depth
     * 0.5 rather than behind the near plane.
     */
    push_block_16(qts, 0xbf800000);
    push_captured_triangle(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 62, 70), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 45, 85), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 60, 43), ==, ODSY_PLANE_YELLOW);
    /* Inside the identity triangle, outside the half-size one. */
    g_assert_cmphex(odsy_test_pixel(qts, 30, 105), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 20), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 62, 38), ==, 0);

    push_triangle(qts, outside_unit);
    g_assert_cmphex(odsy_test_pixel(qts, 115, 90), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 97, 95), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * Slot 0x14 is the texture matrix (the GL library's texture-matrix
 * validation), not the projection. A half-scale load there leaves the
 * identity triangle exactly where the combined matrix alone puts it.
 */
static void test_command_fifo_gl_texture_slot_leaves_geometry(void)
{
    static const uint32_t half[16] = {
        0x3f000000, 0, 0, 0,
        0, 0x3f000000, 0, 0,
        0, 0, 0x3f000000, 0,
        0, 0, 0, 0x3f800000,
    };
    QTestState *qts = board_start("");
    unsigned i;

    push_window_transform(qts, odsy_captured_window_transform);
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    for (i = 0; i < 16; i++) {
        user_cmd_push(qts, half[i]);
    }
    push_captured_triangle(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 61, 78), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 20), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 30, 105), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 110), ==, 0);

    board_quit(qts);
}

/*
 * A 200x150 window at (300, 200), placed the way the kernel places the
 * probe's: origin (300, 200), one rectangle (300, 200) .. (499, 349).
 * Distinct x and y catch swapped origin operands, which the
 * probe's (256, 256) cannot.
 */
static void window_300_200(QTestState *qts)
{
    window_origin(qts, 300, 200);
    window_clip(qts, (const uint32_t[][4]) { { 300, 200, 499, 349 } }, 1);
    push_window_transform(qts, odsy_window_transform_200x150);
}

/*
 * A clear of the 200x150 window is large enough to draw in bands of rows,
 * and the GL target lets it: the clear starts the rasterizer's band
 * workers, colours every pixel of the window, including the rows on each
 * side of every band boundary, and leaves the one-pixel frame around it.
 */
static void test_gl_large_clear_draws_in_bands(void)
{
    enum { X = 299, Y = 199, W = 202, H = 152 };
    g_autofree uint32_t *got = g_new(uint32_t, W * H);
    QTestState *qts = board_start("");
    int workers;

    window_300_200(qts);
    g_assert_cmpint(sgi_raster_band_workers(qts), <=, 0);
    push_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_200,
               ODSY_F_150);
    odsy_test_pixels(qts, X, Y, W, H, got);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bool inside = x > 0 && x < W - 1 && y > 0 && y < H - 1;

            g_assert_cmphex(got[y * W + x], ==,
                            inside ? ODSY_PLANE_BLUE : 0);
        }
    }
    workers = sgi_raster_band_workers(qts);
    board_quit(qts);
    if (workers < 0) {
        g_test_skip("needs Linux /proc and two host processors");
        return;
    }
    g_assert_cmpint(workers, >, 0);
}

/*
 * With x-raster-bands off the same clear draws on the calling thread
 * alone: no band worker is started, and the window still fills.
 */
static void test_gl_raster_bands_off_draws_on_one_thread(void)
{
    QTestState *qts = board_start(" -global sgi-odyssey.x-raster-bands=off");
    int workers;

    window_300_200(qts);
    push_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_200,
               ODSY_F_150);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 200), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 499, 349), ==, ODSY_PLANE_BLUE);
    workers = sgi_raster_band_workers(qts);
    board_quit(qts);
    if (workers < 0) {
        g_test_skip("needs Linux /proc and two host processors");
        return;
    }
    g_assert_cmpint(workers, ==, 0);
}

/*
 * The client's window-relative clear and triangle land inside the kernel's
 * window: the clear covers exactly (300, 200) .. (499, 349), and the triangle
 * -- vertices (325, 331.25), (475, 321.875) and the apex (387.5, 219.92) --
 * sits inside it apex up. Nothing outside the window changes, including the
 * old whole-frame centroid (613, 402).
 */
static void test_command_fifo_gl_window_origin_and_clip(void)
{
    QTestState *qts = board_start("");

    window_300_200(qts);
    push_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_200,
               ODSY_F_150);

    g_assert_cmphex(odsy_test_pixel(qts, 300, 200), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 499, 349), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 299, 200), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 199), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 500, 349), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 499, 350), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1279, 1023), ==, 0);

    push_captured_triangle(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 395, 291), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 387, 225), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 387, 340), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 305, 205), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 613, 402), ==, 0);

    board_quit(qts);
}

/*
 * Two kept rectangles, the window's left and right quarters: a whole-window
 * clear paints inside both and not the gap between. Then all four empty: a
 * red clear paints nothing at all.
 */
static void test_command_fifo_gl_clip_rectangles(void)
{
    QTestState *qts = board_start("");

    window_300_200(qts);
    window_clip(qts, (const uint32_t[][4]) {
                    { 300, 200, 349, 349 }, { 450, 200, 499, 349 } }, 2);
    push_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_200,
               ODSY_F_150);

    g_assert_cmphex(odsy_test_pixel(qts, 320, 300), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 470, 300), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 349, 349), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 350, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 400, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 449, 300), ==, 0);

    window_clip(qts, NULL, 0);
    push_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_200,
               ODSY_F_150);
    push_captured_triangle(qts);

    g_assert_cmphex(odsy_test_pixel(qts, 320, 300), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 470, 300), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 400, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 395, 291), ==, 0);

    board_quit(qts);
}

static void test_command_fifo_compare_incomplete(void)
{
    QTestState *qts = board_start("");
    const uint64_t base = 0xb70000;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, (uint32_t)base);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);
    odsy_test_pixel_write(qts, 10, 10, 0x01020304);
    cmd_push(qts, ODSY_CMD_DID_COMPARE);

    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x01020304);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, 0);
    cmd_push(qts, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x01020304);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, 0);
    cmd_push(qts, 0);
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 2);
    cmd_push(qts, 0x5a00);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0x5a00);
    rgb_fill(qts, 0xaa, 0xbb, 0xcc, 10, 10, 11, 11);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xaabbccff);
    qtest_quit(qts);
}

static void compare_push(QTestState *qts, bool user, uint32_t word)
{
    if (user) {
        user_cmd_push(qts, word);
    } else {
        cmd_push(qts, word);
    }
}

static void test_command_fifo_compare_neutral(bool user)
{
    QTestState *qts = board_start("");
    const uint64_t base = 0xb70000;
    uint64_t last_interrupt_addr;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, (uint32_t)base);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);
    odsy_test_pixel_write(qts, 10, 10, 0x01020304);
    qtest_writel(qts, base + 8, 0x87654321);
    last_interrupt_addr = odsy_qom_get_uint(qts,
                                            "x-test-last-interrupt-addr");
    g_assert_cmphex(last_interrupt_addr, ==, 0);

    compare_push(qts, user, ODSY_CMD_DID_COMPARE);
    compare_push(qts, user, 0);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, last_interrupt_addr);
    compare_push(qts, user, 0);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, last_interrupt_addr);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, base + 8), ==, 0x87654321);

    compare_push(qts, user, ODSY_CMD_HOST_WRITE_SELECT);
    compare_push(qts, user, 2);
    compare_push(qts, user, 0x5a00);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0x5a00);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, 0);
    rgb_fill(qts, 0xaa, 0xbb, 0xcc, 10, 10, 11, 11);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xaabbccff);
    qtest_quit(qts);
}

static void test_command_fifo_compare_neutral_kernel(void)
{
    test_command_fifo_compare_neutral(false);
}

static void test_command_fifo_compare_neutral_user(void)
{
    test_command_fifo_compare_neutral(true);
}

/*
 * A non-zero 0x840 pair whose second operand has bit 16 clear is a decoded,
 * disabled id test: nothing is reported and every later write lands.
 */
static void test_command_fifo_compare_nonzero(bool user, bool second)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    const uint64_t base = 0xb70000;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-compare-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, (uint32_t)base);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);
    odsy_test_pixel_write(qts, 10, 10, 0x01020304);

    compare_push(qts, user, ODSY_CMD_DID_COMPARE);
    compare_push(qts, user, second ? 0 : 1);
    compare_push(qts, user, second ? 1 : 0);

    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x01020304);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, 0);
    compare_push(qts, user, ODSY_CMD_HOST_WRITE_SELECT);
    compare_push(qts, user, 2);
    compare_push(qts, user, 0x5a00);
    g_assert_cmphex(qtest_readw(qts, base + 4), ==, 0x5a00);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                    ==, 0);
    rgb_fill(qts, 0xaa, 0xbb, 0xcc, 10, 10, 11, 11);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xaabbccff);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "0x00010840"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_command_fifo_compare_nonzero_first(void)
{
    test_command_fifo_compare_nonzero(false, false);
}

static void test_command_fifo_compare_nonzero_second(void)
{
    test_command_fifo_compare_nonzero(false, true);
}

static void test_command_fifo_compare_nonzero_first_user(void)
{
    test_command_fifo_compare_nonzero(true, false);
}

static void test_command_fifo_compare_nonzero_second_user(void)
{
    test_command_fifo_compare_nonzero(true, true);
}

static void test_command_fifo_compare_cancels_query_user(void)
{
    QTestState *qts = board_start("");
    const uint64_t page = 0xb72000;
    const uint32_t control_prefix[] = { 0x4c0, 0x11223344 };
    const uint32_t control_suffix[] = { 0x10a5a, 2, 0x2222 };
    const uint32_t prefix[] = { 0x4c0, 0x12345678 };
    const uint32_t suffix[] = { 0x10a5a, 3, 0x4321 };
    const uint32_t fresh[] = { 0x4c0, 0xabcdef01, 0x10a5a, 4, 0x5432 };
    uint8_t counter[4];

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, page + 0x800);
    qtest_memset(qts, page, 0xa5, 4096);

    for (unsigned i = 0; i < ARRAY_SIZE(control_prefix); i++) {
        user_cmd_push(qts, control_prefix[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(control_suffix); i++) {
        user_cmd_push(qts, control_suffix[i]);
    }
    qtest_memread(qts, page + 0x97c, counter, sizeof(counter));
    g_assert_cmphex((uint32_t)ldl_be_p(counter), ==, 0x11223344);
    qtest_memset(qts, page, 0xa5, 4096);

    for (unsigned i = 0; i < ARRAY_SIZE(prefix); i++) {
        user_cmd_push(qts, prefix[i]);
    }
    user_cmd_push(qts, ODSY_CMD_DID_COMPARE);
    user_cmd_push(qts, 1);
    user_cmd_push(qts, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(suffix); i++) {
        user_cmd_push(qts, suffix[i]);
    }
    qtest_memread(qts, page + 0x97c, counter, sizeof(counter));
    g_assert_cmphex((uint32_t)ldl_be_p(counter), ==, 0xa5a5a5a5);

    for (unsigned i = 0; i < ARRAY_SIZE(fresh); i++) {
        user_cmd_push(qts, fresh[i]);
    }
    qtest_memread(qts, page + 0x97c, counter, sizeof(counter));
    g_assert_cmphex((uint32_t)ldl_be_p(counter), ==, 0xabcdef01);
    qtest_quit(qts);
}

/*
 * The GL feature probes' window: 128x128 at (256, 256), one clip
 * rectangle, the probe's identity combined matrix and the four floats
 * the library loads behind matrix index 16 with it, (-0, -0, -0, 1).
 */
#define ODSY_REG_GL_MODE 0x00010404
#define ODSY_GL_MODE_SMOOTH 0x02002200
#define ODSY_GL_MODE_FLAT 0x02003200
#define ODSY_MODE_GL_LINE_STIPPLE (1u << 3)

static void probe_window(QTestState *qts)
{
    window_origin(qts, 256, 256);
    window_clip(qts, (const uint32_t[][4]) { { 256, 256, 383, 383 } }, 1);
    push_window_transform(qts, odsy_captured_window_transform);
    push_block_16(qts, 0x3f800000);
}

static void push_user_words(QTestState *qts, const uint32_t *w, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        user_cmd_push(qts, w[i]);
    }
}

/*
 * smooth.c's two triangles, as the probe's: each glColor3f travels before its
 * vertex, and only 0x0404 bit 12 tells the flat one from the smooth one.
 * Under GL_FLAT the whole right triangle takes its last vertex's white;
 * under GL_SMOOTH the left one blends red, green and blue. The smooth values
 * are the shared rasterizer's, which the offline replay of the capture
 * matches where sampled.
 */
static void test_command_fifo_gl_flat_shading(void)
{
    static const uint32_t flat_tri[] = {
        ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
        ODSY_GL_VERTEX_2F, 0x3dcccccd, 0xbf666666,
        ODSY_GL_COLOR_3F, 0, 0x3f800000, 0,
        ODSY_GL_VERTEX_2F, 0x3f666666, 0xbf666666,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0x3f800000,
        ODSY_GL_VERTEX_2F, 0x3f000000, 0x3f666666,
        ODSY_CMD_GL_END,
    };
    static const uint32_t smooth_tri[] = {
        ODSY_REG_GL_MODE, ODSY_GL_MODE_SMOOTH,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
        ODSY_GL_VERTEX_2F, 0xbf666666, 0xbf666666,
        ODSY_GL_COLOR_3F, 0, 0x3f800000, 0,
        ODSY_GL_VERTEX_2F, 0xbdcccccd, 0xbf666666,
        ODSY_GL_COLOR_3F, 0, 0, 0x3f800000,
        ODSY_GL_VERTEX_2F, 0xbf000000, 0x3f666666,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    push_user_words(qts, flat_tri, ARRAY_SIZE(flat_tri));
    g_assert_cmphex(odsy_test_pixel(qts, 352, 340), ==, 0xffffffffu);
    g_assert_cmphex(odsy_test_pixel(qts, 352, 360), ==, 0xffffffffu);

    push_user_words(qts, smooth_tri, ARRAY_SIZE(smooth_tri));
    g_assert_cmphex(odsy_test_pixel(qts, 288, 340), ==, 0x545952ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 265, 376), ==, 0xee0e02ffu);

    /* The flat triangle's words with bit 12 clear blend instead. */
    push_user_words(qts, smooth_tri, 2);
    push_user_words(qts, flat_tri + 2, ARRAY_SIZE(flat_tri) - 2);
    g_assert_cmphex(odsy_test_pixel(qts, 352, 360), !=, 0xffffffffu);

    board_quit(qts);
}

/*
 * Depth: 0x0404 bit 16 the depth test, 0x0420 bits 2:0 the comparison, 0x0452
 * bit 24 the depth write and bits 23:20 the colour write mask; glVertex3f is
 * 0x8080cc03, and glClear of the depth buffer is the colour clear's rectangle
 * with the depth write bit set and the clear depth, d x 2S, as its z.
 */
#define ODSY_GL_VERTEX_3F 0x8080cc03
#define ODSY_REG_DEPTH_FUNC 0x00010420
#define ODSY_REG_WRITE_MASK 0x00010452
#define ODSY_GL_MODE_DEPTH_FLAT 0x02013200
#define ODSY_WRITE_ALL 0x01fff000        /* the clear: depth and colour */
#define ODSY_WRITE_DRAW 0x01f00000       /* a draw: depth and colour */
#define ODSY_WRITE_DEPTH_ONLY 0x010ff000 /* a depth-only clear */
#define ODSY_GL_FUNC_LESS 1
#define ODSY_GL_FUNC_GREATER 4
#define ODSY_DEPTH_2S 0x4fffffc0         /* 2S: depth 1.0 */
#define ODSY_PLANE_CYAN 0x00ffffffu

/* The rectangle clear of the 128x128 window, colour (r, g, b), depth z. */
static void push_depth_clear(QTestState *qts, uint32_t r, uint32_t g,
                             uint32_t b, uint32_t z)
{
    const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS,
        ODSY_GL_RECT, r, g, b, ODSY_F_ONE,
        0, 0, z, 0x43000000, 0x43000000, z,
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

/* One flat glVertex3f triangle in colour (r, g, b); v holds 3 x, y, z. */
static void push_triangle_3f(QTestState *qts, uint32_t r, uint32_t g,
                             uint32_t b, const uint32_t v[9])
{
    unsigned i;

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, r);
    user_cmd_push(qts, g);
    user_cmd_push(qts, b);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        user_cmd_push(qts, ODSY_GL_VERTEX_3F);
        user_cmd_push(qts, v[3 * i]);
        user_cmd_push(qts, v[3 * i + 1]);
        user_cmd_push(qts, v[3 * i + 2]);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

#define ODSY_GL_COLOR_4F 0xc580d015 /* 4 floats: R, G, B, A */

/* One flat glVertex3f triangle in colour (r, g, b, a); v holds 3 x, y, z. */
static void push_triangle_4f(QTestState *qts, uint32_t r, uint32_t g,
                             uint32_t b, uint32_t a, const uint32_t v[9])
{
    unsigned i;

    user_cmd_push(qts, ODSY_GL_COLOR_4F);
    user_cmd_push(qts, r);
    user_cmd_push(qts, g);
    user_cmd_push(qts, b);
    user_cmd_push(qts, a);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        user_cmd_push(qts, ODSY_GL_VERTEX_3F);
        user_cmd_push(qts, v[3 * i]);
        user_cmd_push(qts, v[3 * i + 1]);
        user_cmd_push(qts, v[3 * i + 2]);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

/* Synthetic perspective coordinates test the shared clipping approximation. */
static void test_command_fifo_gl_crossing_eye(void)
{
    /* Half-pixel y translation avoids the line diamond boundary. */
    static const uint32_t matrix[16] = {
        0x42000000, 0, 0xc2800000, 0,
        0, 0xc2000000, 0xc2810000, 0,
        0, 0, 0xcfc00000, 0xcfc00000,
        0, 0, 0xbf800000, 0,
    };
    static const uint32_t crossing[9] = {
        0xbf800000, 0xbf800000, 0xc0000000,
        0x3f800000, 0xbf800000, 0xc0000000,
        0, 0x3f800000, 0x3f800000,
    };
    static const uint32_t behind[9] = {
        0xbf800000, 0xbf800000, 0x3f800000,
        0x3f800000, 0xbf800000, 0x3f800000,
        0, 0x3f800000, 0x3f800000,
    };
    static const uint32_t line[] = {
        ODSY_CMD_GL_BEGIN, ODSY_GL_LINES,
        ODSY_GL_VERTEX_3F, 0xbf800000, 0, 0xc0000000,
        ODSY_GL_VERTEX_3F, 0x3f800000, 0, 0x3f800000,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    push_window_transform(qts, matrix);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, behind);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 76), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 32), ==, 0);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, crossing);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 76), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 73), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 81), ==, 0);
    push_user_words(qts, line, ARRAY_SIZE(line));
    g_assert_cmphex(odsy_test_pixel(qts, 50, 63) |
                    odsy_test_pixel(qts, 50, 64), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 63) |
                    odsy_test_pixel(qts, 56, 64), ==, 0);
    board_quit(qts);
}

/* depth.c's pair: a yellow triangle at z -0.5, then a cyan one at +0.5. */
static const uint32_t odsy_near_yellow[9] = {
    0xbf4ccccd, 0xbf4ccccd, 0xbf000000,
    0x3ecccccd, 0xbf4ccccd, 0xbf000000,
    0xbe4ccccd, 0x3f19999a, 0xbf000000,
};
static const uint32_t odsy_far_cyan[9] = {
    0xbecccccd, 0xbf19999a, 0x3f000000,
    0x3f4ccccd, 0xbf19999a, 0x3f000000,
    0x3e4ccccd, 0x3f4ccccd, 0x3f000000,
};

static void push_pair(QTestState *qts, const uint32_t *yellow,
                      const uint32_t *cyan)
{
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, yellow);
    push_triangle_3f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE, cyan);
}

static void set_reg(QTestState *qts, uint32_t reg, uint32_t value)
{
    user_cmd_push(qts, reg);
    user_cmd_push(qts, value);
}

#define ODSY_REG_FOG 0x0001181a
#define ODSY_MODE_FOG (1u << 22)

static void push_fog(QTestState *qts, uint32_t scale, uint32_t bias,
                     uint32_t p01, uint32_t p23)
{
    const uint32_t words[] = {
        ODSY_REG_FOG, 0, 0x00ffffff, scale, bias, p01, p23,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

/* The local curve policy uses the fragment's window depth, 0.25 here. */
static void test_command_fifo_gl_fog_curve(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    push_fog(qts, 0x7c0000, 0, 0x1000ab, 0x055000);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_FOG);
    push_triangle_3f(qts, ODSY_F_ONE, 0, 0, odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, 0xbf0040ff);

    /* Reversing the cubic makes the same pixel mostly fog blue. */
    push_fog(qts, 0x7c0000, 0, 0x000055, 0x0ab100);
    push_triangle_3f(qts, ODSY_F_ONE, 0, 0, odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, 0x4000bfff);

    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    push_triangle_3f(qts, ODSY_F_ONE, 0, 0, odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, 0xff0000ff);

    qtest_system_reset(qts);
    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_FOG);
    push_triangle_3f(qts, ODSY_F_ONE, 0, 0, odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, 0x000000ff);
    board_quit(qts);
}

/* Native packets exercise the local curve policy, not a board measurement. */
static void test_command_fifo_gl_fog_native_packets(void)
{
    static const struct {
        uint32_t scale, bias, p01, p23;
        uint8_t red[3];
    } cases[] = {
        { 0x808000, 0,    0x1000aa, 0x055000, { 255, 191,   0 } },
        { 0x808000, 0,    0x000055, 0x0aa100, {   0,  64, 255 } },
        { 0x7c99e8, 0x20, 0x100fe1, 0x021005, { 163,  98,  20 } },
        { 0x80ea5b, 0x45, 0x1000e8, 0xff3008, { 197,  99,   8 } },
    };
    static const float ndc_depth[] = { -1.0f, -7.0f / 9, -1.0f / 9 };

    for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
        QTestState *qts = board_start("");

        probe_window(qts);
        push_fog(qts, cases[c].scale, cases[c].bias,
                 cases[c].p01, cases[c].p23);
        set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_FOG);
        for (unsigned i = 0; i < ARRAY_SIZE(ndc_depth); i++) {
            uint32_t tri[ARRAY_SIZE(odsy_near_yellow)];
            uint32_t z;

            memcpy(&z, &ndc_depth[i], sizeof(z));
            memcpy(tri, odsy_near_yellow, sizeof(tri));
            tri[2] = tri[5] = tri[8] = z;
            push_triangle_3f(qts, ODSY_F_ONE, 0, 0, tri);
            g_assert_cmpuint(odsy_test_pixel(qts, 326, 340) >> 24,
                             ==, cases[c].red[i]);
        }
        board_quit(qts);
    }
}

static void test_command_fifo_gl_fog_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-gl-fog-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    push_fog(from, 0x7c0000, 0, 0x1000ab, 0x055000);
    set_reg(from, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_FOG);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    probe_window(to);
    push_triangle_3f(to, ODSY_F_ONE, 0, 0, odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(to, 326, 340), ==, 0xbf0040ff);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * The probe's phases of depth.c. Under GL_LESS after a clear to 1.0 the
 * near yellow triangle stays in front where the far cyan one overlaps it;
 * cyan still covers what only it reaches. Under GL_GREATER after a clear to
 * 0.0 cyan wins the overlap. With the test off the last drawn, cyan, wins.
 */
static void test_command_fifo_gl_depth_test(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE,
                     ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_pair(qts, odsy_near_yellow, odsy_far_cyan);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 356, 352), ==, ODSY_PLANE_CYAN);
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, ODSY_PLANE_BLUE);

    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_GREATER);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, 0);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_pair(qts, odsy_near_yellow, odsy_far_cyan);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_CYAN);

    /* Test off: yellow first, then cyan over it. */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    push_pair(qts, odsy_near_yellow, odsy_far_cyan);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_CYAN);
    /* And with the test off the plain draw order holds the other way. */
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * Alpha test: 0x0404 bit 14 is GL_ALPHA_TEST, and 0x041e carries the function
 * in bits 14:12 (GL_NEVER = 0 through GL_ALWAYS = 7) and the reference in
 * bits 11:0 under the depth comparison's own 12-bit rule (value / 4096.0).
 */
#define ODSY_MODE_ALPHA_TEST (1u << 14)
#define ODSY_REG_ALPHA_FUNC 0x0001041e
/*
 * A stored fragment keeps its own alpha: round(0.25 * 255)
 * (odyssey_float_channel).
 */
#define ODSY_PLANE_YELLOW_QUARTER_ALPHA 0xffff0040u

static void test_command_fifo_gl_alpha_test(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);

    /* GL_GREATER, ref 0.5: alpha 0.25 fails, the blue clear stays. */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_ALPHA_TEST);
    set_reg(qts, ODSY_REG_ALPHA_FUNC, 0x4800);
    push_triangle_4f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, 0x3e800000,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

    /* Same function, ref 0.125: alpha 0.25 now passes. */
    set_reg(qts, ODSY_REG_ALPHA_FUNC, 0x4200);
    push_triangle_4f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, 0x3e800000,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==,
                    ODSY_PLANE_YELLOW_QUARTER_ALPHA);

    /* Test off: the same near-failing function and reference draw anyway. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    set_reg(qts, ODSY_REG_ALPHA_FUNC, 0x4fff);
    push_triangle_4f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, 0x3e800000,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==,
                    ODSY_PLANE_YELLOW_QUARTER_ALPHA);

    board_quit(qts);
}

/*
 * Blend and logic op: 0x0404 bit 17 is a computed GL_BLEND enable, bits 19
 * and 20 are GL_INDEX_LOGIC_OP and GL_COLOR_LOGIC_OP, 0x0c21 carries the
 * blend factors, equation and constant colour, kept whatever bit 17 says,
 * and 0x0422 the logic op mode.
 */
#define ODSY_MODE_BLEND (1u << 17)
#define ODSY_MODE_COLOR_LOGIC_OP (1u << 20)
#define ODSY_REG_BLEND 0x00010c21    /* 3 operands */
#define ODSY_REG_LOGIC_OP 0x00010422
#define ODSY_PLANE_GREEN 0x00ff00ffu
/*
 * push_depth_clear's rect always writes alpha 1, so a draw's own alpha 1
 * minus that background alpha is 0, not 1: white RGB survives, alpha
 * does not.
 */
#define ODSY_PLANE_WHITE_ZERO_ALPHA 0xffffff00u
#define ODSY_PLANE_ZERO 0x00000000u

static void set_blend(QTestState *qts, uint32_t word0, uint32_t word1,
                      uint32_t word2)
{
    user_cmd_push(qts, ODSY_REG_BLEND);
    user_cmd_push(qts, word0);
    user_cmd_push(qts, word1);
    user_cmd_push(qts, word2);
}

static void test_command_fifo_gl_blend(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);

    /* src GL_ZERO, dst GL_ONE: the draw leaves the red background alone. */
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND);
    set_blend(qts, 0x08, 0, 0);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_RED);

    /* Swap the two fields, src GL_ONE dst GL_ZERO: the draw replaces it. */
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_blend(qts, 0x200, 0, 0);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_GREEN);

    /*
     * The library sends 0x0c21 while bit 17 is clear and sets the bit later
     * without sending it again (tex_cube, mirrors). Factors GL_ZERO, GL_ONE
     * written with blending off are the ones in effect once the bit is set:
     * the draw leaves the red background.
     */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    set_blend(qts, 0x08, 0, 0);
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_RED);

    /*
     * The equation field, bits 15:13: 6 is GL_FUNC_SUBTRACT, source minus
     * destination; 7 is GL_FUNC_REVERSE_SUBTRACT, destination minus source.
     * An opaque white draw over an opaque black background: SUBTRACT leaves
     * white RGB with alpha 1 - 1 = 0; REVERSE_SUBTRACT clamps every channel,
     * alpha included, to zero.
     */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    /* (6 << 13) | GL_ONE << 6 | GL_ONE */
    set_blend(qts, 0xc208, 0, 0);
    push_triangle_4f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==,
                    ODSY_PLANE_WHITE_ZERO_ALPHA);

    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    /* (7 << 13) | GL_ONE << 6 | GL_ONE */
    set_blend(qts, 0xe208, 0, 0);
    push_triangle_4f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_ZERO);

    /*
     * Logic op. Bit 20 with 0x0422 = 6 (GL_XOR): two identical draws XOR
     * the background away and back, returning it exactly.
     */
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE,
            ODSY_GL_MODE_FLAT | ODSY_MODE_COLOR_LOGIC_OP);
    set_reg(qts, ODSY_REG_LOGIC_OP, 6);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_RED);

    /* With bit 20 and bit 17 both clear, the same 0x0422 has no effect. */
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_GREEN);

    /*
     * With bit 17 set and equation 5 (GL_LOGIC_OP) in 0x0c21 word 0, 0x0422
     * still carrying 6 XORs the second draw away again.
     */
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND);
    /* (5 << 13) | GL_ONE << 6 | GL_ZERO */
    set_blend(qts, 0xa200, 0, 0);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_RED);

    board_quit(qts);
}

/*
 * The sign behind index 16 orients depth. glOrtho(-1, 1, -1, 1, -1, 1)
 * negates the depth row (S z - S) and sends -1: z = +0.5 is then nearer,
 * so under GL_LESS cyan wins the overlap. glFrustum(-1, 1, -1, 1, 1, 3)
 * sends 3S z + 3S over w = -z with -3: the yellow triangle at eye z -1.5
 * is nearer than the cyan one at -2.5 and wins.
 */
static void test_command_fifo_gl_depth_sign(void)
{
    static const uint32_t ortho[16] = {
        0x42800000, 0, 0, 0x42800000,
        0, 0xc2800000, 0, 0x427ffc00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    static const uint32_t frustum[16] = {
        0x42800000, 0, 0xc2800000, 0,
        0, 0xc2800000, 0xc27ffc00, 0,
        0, 0, 0x503fffd0, 0x503fffd0,
        0, 0, 0xbf800000, 0,
    };
    static const uint32_t persp_yellow[9] = {
        0xbf99999a, 0xbf99999a, 0xbfc00000,
        0x3f19999a, 0xbf99999a, 0xbfc00000,
        0xbe99999a, 0x3f666667, 0xbfc00000,
    };
    static const uint32_t persp_cyan[9] = {
        0xbf800000, 0xbfc00000, 0xc0200000,
        0x40000000, 0xbfc00000, 0xc0200000,
        0x3f000000, 0x40000000, 0xc0200000,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    push_window_transform(qts, ortho);
    push_block_16(qts, 0xbf800000);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE,
                     ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_pair(qts, odsy_near_yellow, odsy_far_cyan);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_CYAN);

    push_window_transform(qts, frustum);
    push_block_16(qts, 0xc0400000);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE,
                     ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_pair(qts, persp_yellow, persp_cyan);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 356, 352), ==, ODSY_PLANE_CYAN);

    board_quit(qts);
}

/*
 * amesh's literal combined matrix and block 16, from a guest trace: it calls
 * gluPerspective with the model-view selected, so block 16 is the
 * model-view's w row times d, (-0, -6.84, -2.31, 65.93), the same as the
 * combined matrix's fourth row. The depth is row 2 over the whole row, 1 -
 * 2e-9 on the floor of its mesh and just past 1 by single-precision
 * rounding at y = 2. A wall at z = -25 from the floor to y = 2 then draws
 * from its base (50, 38) to near its apex (50, 6). Dividing by block 16's
 * fourth float alone put it at depth 1.67 to 1.88 and clipped all of it.
 */
static void test_command_fifo_gl_depth_model_view_perspective(void)
{
    static const uint32_t combined[16] = {
        0x42ab8f80, 0xc3ab0295, 0xc2e74f18, 0x4409e17c,
        0, 0xc51352bb, 0xc1cafba4, 0x458bae39,
        0, 0xd15ae45f, 0xd0940984, 0x5303dbf3,
        0, 0xc0dae496, 0xc01409a9, 0x4283dc14,
    };
    static const uint32_t block16[4] = {
        0x80000000, 0xc0dae496, 0xc01409a9, 0x4283dc14,
    };
    static const uint32_t wall[9] = {
        0, 0, 0xc1c80000,
        0x427c0000, 0, 0xc1c80000,
        0x42000000, 0x40000000, 0xc1c80000,
    };
    QTestState *qts = board_start("");

    push_window_transform(qts, combined);
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_16);
    user_cmd_push(qts, ODSY_GL_LOAD_4F);
    for (unsigned i = 0; i < ARRAY_SIZE(block16); i++) {
        user_cmd_push(qts, block16[i]);
    }
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, wall);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 38), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 6), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 44), ==, 0);
    board_quit(qts);
}

/* Native orthographic rows put the depth scale in block 16's fourth word. */
static void test_command_fifo_gl_depth_scale(void)
{
    static const struct {
        uint32_t row_z, row_w, block_sign;
        float eye_z, clear_depth;
    } cases[] = {
        { 0x4e7fffc0, 0x4e7fffbf, 0xbf000000, -2.0f, 0.2f },
        { 0x4d4a4555, 0x4d4a4554, 0xbe638e39, -2.0f, 0.1f },
        { 0x4c358a1b, 0x4c358a15, 0xbdd79436, -5.0f, 0.1f },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        uint32_t matrix[16];
        uint32_t triangle[ARRAY_SIZE(odsy_near_yellow)];
        uint32_t zbits, clear_bits;
        float clear_word = cases[i].clear_depth * (2 * 4294950912.0f);
        QTestState *qts = board_start("");

        memcpy(&zbits, &cases[i].eye_z, sizeof(zbits));
        memcpy(&clear_bits, &clear_word, sizeof(clear_bits));
        memcpy(matrix, odsy_captured_window_transform, sizeof(matrix));
        matrix[10] = cases[i].row_z;
        matrix[11] = cases[i].row_w;
        memcpy(triangle, odsy_near_yellow, sizeof(triangle));
        triangle[2] = triangle[5] = triangle[8] = zbits;

        probe_window(qts);
        push_window_transform(qts, matrix);
        push_block_16(qts, cases[i].block_sign);
        set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
        set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
        set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
        push_depth_clear(qts, 0, 0, ODSY_F_ONE, clear_bits);
        set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
        push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, triangle);
        g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

        set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
        push_depth_clear(qts, 0, 0, ODSY_F_ONE, ODSY_DEPTH_2S);
        set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
        push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, triangle);
        g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==,
                        ODSY_PLANE_YELLOW);
        board_quit(qts);
    }
}

/* Zero and non-finite block words keep the previous sign-only mapping. */
static void test_command_fifo_gl_depth_scale_fallback(void)
{
    static const uint32_t malformed[] = { 0, 0x7fc00000, 0x7f800000 };

    for (unsigned i = 0; i < ARRAY_SIZE(malformed) + 1; i++) {
        QTestState *qts = board_start("");

        for (unsigned pass = 0; pass < 2; pass++) {
            if (pass) {
                qtest_system_reset(qts);
            }
            window_origin(qts, 256, 256);
            window_clip(qts,
                        (const uint32_t[][4]) { { 256, 256, 383, 383 } }, 1);
            push_window_transform(qts, odsy_captured_window_transform);
            if (!pass && i < ARRAY_SIZE(malformed)) {
                push_block_16(qts, malformed[i]);
            }
            set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
            set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
            set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
            push_depth_clear(qts, 0, 0, ODSY_F_ONE, ODSY_DEPTH_2S);
            set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
            push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0,
                             odsy_near_yellow);
            g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==,
                            ODSY_PLANE_YELLOW);
        }
        board_quit(qts);
    }
}

/*
 * glFrustum(-1, 1, -1, 1, 1, 3)'s captured rows: the depth row 3S z + 3S
 * over the block-16 word -3 is the eye-linear depth -(z + 1) / 2, 0.5 at
 * eye z -2. OpenGL's depth there is 0.75, and the depth row over w and -3
 * would be 0.25, so clears at 0.4 and 0.6 tell all three apart.
 */
static void test_command_fifo_gl_depth_eye_linear(void)
{
    static const uint32_t frustum[16] = {
        0x42800000, 0, 0xc2800000, 0,
        0, 0xc2800000, 0xc27ffc00, 0,
        0, 0, 0x503fffd0, 0x503fffd0,
        0, 0, 0xbf800000, 0,
    };
    static const uint32_t at_two[9] = {
        0xbf800000, 0xbf800000, 0xc0000000,
        0x3f800000, 0xbf800000, 0xc0000000,
        0, 0x3f800000, 0xc0000000,
    };
    static const struct {
        float clear;
        uint32_t pixel;
    } cases[] = {
        { 0.4f, ODSY_PLANE_BLUE },
        { 0.6f, ODSY_PLANE_YELLOW },
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    push_window_transform(qts, frustum);
    push_block_16(qts, 0xc0400000);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        float clear_word = cases[i].clear * (2 * 4294950912.0f);
        uint32_t clear_bits;

        memcpy(&clear_bits, &clear_word, sizeof(clear_bits));
        set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
        push_depth_clear(qts, 0, 0, ODSY_F_ONE, clear_bits);
        set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
        push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, at_two);
        g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, cases[i].pixel);
    }
    board_quit(qts);
}

/*
 * pfCity's rows put its scene a few board units past depth 0: a depth row
 * of 0.2 z - 8.5 over the block-16 word -0.2 gives (-z + 42.5) / 2S, about
 * 1.7e-8 at eye z -100 and -101. A 24-bit fixed-point store holds both as
 * 0, and a float projection row cannot carry either as a difference from
 * w, so the nearer triangle, drawn second, would fail GL_LESS; the board's
 * floating-point depth keeps them apart.
 */
static void test_command_fifo_gl_depth_float(void)
{
    static const uint32_t rows[16] = {
        0x42800000, 0, 0xc2800000, 0,
        0, 0xc2800000, 0xc27ffc00, 0,
        0, 0, 0x3e4ccccd, 0xc1080000,
        0, 0, 0xbf800000, 0,
    };
    static const uint32_t near_yellow[9] = {
        0xc2c80000, 0xc2c80000, 0xc2c80000,
        0x42c80000, 0xc2c80000, 0xc2c80000,
        0, 0x42c80000, 0xc2c80000,
    };
    static const uint32_t far_cyan[9] = {
        0xc2ca0000, 0xc2ca0000, 0xc2ca0000,
        0x42ca0000, 0xc2ca0000, 0xc2ca0000,
        0, 0x42ca0000, 0xc2ca0000,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    push_window_transform(qts, rows);
    push_block_16(qts, 0xbe4ccccd);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, 0, 0, ODSY_F_ONE, ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_triangle_3f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE, far_cyan);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);
    push_triangle_3f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE, far_cyan);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

/*
 * glClear(GL_DEPTH_BUFFER_BIT) alone: the colour write mask is 0, so the
 * colour operands are ignored; the test passes arbitrary non-zero ones to
 * prove it. No colour changes; the depth becomes 0.25, so a GL_LESS draw
 * at depth 0.75 (z +0.5) is hidden and one at 0.05 (z -0.9) paints.
 */
static void test_command_fifo_gl_depth_only_clear(void)
{
    static const uint32_t far[9] = {
        0xbf4ccccd, 0xbf4ccccd, 0x3f000000,
        0x3ecccccd, 0xbf4ccccd, 0x3f000000,
        0xbe4ccccd, 0x3f19999a, 0x3f000000,
    };
    static const uint32_t nearest[9] = {
        0xbf4ccccd, 0xbf4ccccd, 0xbf666666,
        0x3ecccccd, 0xbf4ccccd, 0xbf666666,
        0xbe4ccccd, 0x3f19999a, 0xbf666666,
    };
    const uint32_t depth_only[] = {
        ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS,
        ODSY_GL_RECT, 0x3f800000, 0x3f000000, 0x3e800000, 0x3f800000,
        0, 0, 0x4effffc0, 0x43000000, 0x43000000, 0x4effffc0,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE,
                     ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DEPTH_ONLY);
    push_user_words(qts, depth_only, ARRAY_SIZE(depth_only));
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, ODSY_PLANE_BLUE);

    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, far);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, nearest);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * Lighting (section 1).
 * light.c's words: the front material, the light-model ambient and light 0's
 * colours (class 0x32) and geometry (class 0x31, a directional light down
 * +z), loaded under the 0x19a stream entry as the library does, then its
 * triangle with a normal before each vertex. Lighting is on only when the
 * geometry program the library sets is a lit one; the model recognises it by
 * its routine addresses.
 */
#define ODSY_GL_NORMAL_3F 0xc180cc19
#define ODSY_PROG(cls, v) \
    (((uint32_t)(cls) << 24) | 0x8000u | ((v) & 0x3ffu) | \
     ((((v) >> 10) & 1u) << 23))

static const uint32_t odsy_light_c_data[] = {
    0xc780d030, 0x3f800000, 0x3f000000, 0x3e800000, 0x3f800000,
    0xc680d02f, 0x3dcccccd, 0x3dcccccd, 0x3dcccccd, 0x3f800000,
    0x2900819a,
    0x32008018, 0x8e809015, 0, 0, 0, 0x3f800000,
    0x31008000, 0x9080bda2, 0, 0, 0x3f800000, 0x80000000, 0x80000000,
    0xbf800000, 0x3f800000, 0x3f800000, 0, 0, 0x80000000, 0x80000000,
    0x3f800000, 0x3f800000, 0, 0x4a800000,
    0x32008001, 0x8e809015, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
    0x32008002, 0x8e809015, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
    0x32008000, 0x8e809015, 0, 0, 0, 0x3f800000,
    0x290080d6,
};

/* The probes' lit triangle, smooth, under the current colour red. */
static const uint32_t odsy_lit_triangle[] = {
    ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
    ODSY_REG_GL_MODE, ODSY_GL_MODE_SMOOTH,
    ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
    ODSY_GL_NORMAL_3F, 0, 0, 0x3f800000,
    ODSY_GL_VERTEX_2F, 0xbf400000, 0xbf400000,
    ODSY_GL_NORMAL_3F, 0x3f19999a, 0, 0x3f4ccccd,
    ODSY_GL_VERTEX_2F, 0x3f400000, 0xbf200000,
    ODSY_GL_NORMAL_3F, 0x3f800000, 0, 0,
    ODSY_GL_VERTEX_2F, 0xbe000000, 0x3f3c0000,
    ODSY_CMD_GL_END,
};

/* Constant memory: select index i, store a, as the GL library writes it. */
static void push_constants(QTestState *qts, const uint32_t *a, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        user_cmd_push(qts, ODSY_PROG(0x28, i));
        user_cmd_push(qts, ODSY_PROG(0x60, a[i]));
    }
}

/* The per-vertex program: 0x33, then the three entries, 0x2c last. */
static void push_program(QTestState *qts, uint32_t k, uint32_t e29,
                         uint32_t e2a, uint32_t e2c)
{
    user_cmd_push(qts, ODSY_PROG(0x33, k));
    user_cmd_push(qts, ODSY_PROG(0x29, e29));
    user_cmd_push(qts, ODSY_PROG(0x2a, e2a));
    user_cmd_push(qts, ODSY_PROG(0x2c, e2c));
}

static QTestState *lit_start(char **log_path)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-light-XXXXXX.log", log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(*log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);
    probe_window(qts);
    push_user_words(qts, odsy_light_c_data, ARRAY_SIZE(odsy_light_c_data));
    return qts;
}

/* Quit, then return the unimplemented-feature log and remove it. */
static char *lit_finish(QTestState *qts, char *log_path)
{
    g_autoptr(GError) error = NULL;
    char *log = NULL;

    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_free(log_path);
    return log;
}

static unsigned count_substr(const char *hay, const char *needle)
{
    unsigned n = 0;

    while ((hay = strstr(hay, needle))) {
        n++;
        hay += strlen(needle);
    }
    return n;
}

/*
 * Stencil: 0x0404 bit 15 is GL_STENCIL_TEST, and 0x041f carries the function
 * in bits 27:25 (GL_NEVER = 0 through GL_ALWAYS = 7), the reference in bits
 * 7:0, the value mask in bits 15:8, and three 3-bit ops -- fail 24:22,
 * depth-fail 21:19, depth-pass 18:16 -- KEEP 0, ZERO 1, REPLACE 2, INCR 3,
 * DECR 4, INVERT 5.
 *
 * A register-level test confirms that a clean library-shaped word (ALWAYS,
 * REPLACE/REPLACE/REPLACE) decodes with no complaint, and that the two op
 * codes the board never defines, 6 and 7, are reported rather than silently
 * treated as one of the five real ones.
 */
#define ODSY_MODE_STENCIL_TEST (1u << 15)
#define ODSY_REG_STENCIL_FUNC 0x0001041f

static char *stencil_func_log(uint32_t word)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-stencil-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    probe_window(qts);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, word);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    return log;
}

static void test_command_fifo_gl_stencil_state(void)
{
    /* func ALWAYS, ops REPLACE/REPLACE/REPLACE, value mask 0xff, ref 0. */
    g_autofree char *clean = stencil_func_log(0x0e92ff00);
    /* The same word, but the fail op is 6, which the board never defines. */
    g_autofree char *bad = stencil_func_log(0x0f92ff00);

    g_assert_cmpuint(count_substr(clean, "stencil op word"), ==, 0);
    g_assert_cmpuint(count_substr(bad,
                      "stencil op word 0xf92ff00 is not modeled"), ==, 1);
}

/*
 * The stencil planes: glClear's rectangle under GL_ALWAYS and GL_REPLACE
 * with reference 0 clears them, the yellow triangle writes 1 under
 * GL_ALWAYS and GL_REPLACE, and a cyan triangle over the whole window under
 * GL_EQUAL 1 and GL_KEEP then paints only where the yellow one did.
 */
static void test_command_fifo_gl_stencil_masks_draws(void)
{
    static const uint32_t whole[9] = {
        0xbf800000, 0xbf800000, 0,
        0x40400000, 0xbf800000, 0,
        0xbf800000, 0x40400000, 0,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_STENCIL_TEST);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0e92ff00);
    push_depth_clear(qts, 0, 0, ODSY_F_ONE, ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0e92ff01);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, odsy_near_yellow);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0400ff01);
    push_triangle_3f(qts, 0, ODSY_F_ONE, ODSY_F_ONE, whole);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_CYAN);
    g_assert_cmphex(odsy_test_pixel(qts, 366, 276), ==, ODSY_PLANE_BLUE);

    /* A cleared reference of 1 lets the whole window through. */
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0e92ff01);
    push_depth_clear(qts, 0, 0, ODSY_F_ONE, ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0400ff01);
    push_triangle_3f(qts, 0, ODSY_F_ONE, ODSY_F_ONE, whole);
    g_assert_cmphex(odsy_test_pixel(qts, 366, 276), ==, ODSY_PLANE_CYAN);
    board_quit(qts);
}

/*
 * Cull face (proposal test 6): 0x404 bit 8 is GL_CULL_FACE, bits 10:9 the
 * face it culls, 01 GL_BACK, 10 GL_FRONT. With GL_BACK on, the back-facing
 * (CW) triangle is dropped and the front-facing (CCW) one survives; with
 * GL_FRONT the surviving face swaps. The front/back determination this
 * exercises is the same one the polygon-mode test below depends on: a wrong
 * front-face sense here would swap which triangle each mode drops.
 */
#define ODSY_MODE_CULL_FACE (1u << 8)
#define ODSY_MODE_CULL_BACK (1u << 9)
#define ODSY_MODE_CULL_FRONT (2u << 9)

/*
 * odsy_near_yellow with its last two vertices swapped: same triangle, the
 * opposite winding. Also reused by the polygon-mode test below.
 */
static const uint32_t odsy_near_yellow_cw[9] = {
    0xbf4ccccd, 0xbf4ccccd, 0xbf000000,
    0xbe4ccccd, 0x3f19999a, 0xbf000000,
    0x3ecccccd, 0xbf4ccccd, 0xbf000000,
};

static void test_command_fifo_gl_cull_face(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);

    /* GL_BACK: the CCW (front-facing) triangle survives, filled yellow. */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_MODE_CULL_FACE | ODSY_MODE_CULL_BACK);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    /* GL_BACK: the CW (back-facing) triangle is dropped, leaving the clear. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow_cw);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

    /* GL_FRONT: the CCW triangle is now the one dropped. */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_MODE_CULL_FACE | ODSY_MODE_CULL_FRONT);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

    /* GL_FRONT: the CW triangle now survives. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow_cw);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * Polygon mode: 0x0415 packs front-face mode in bits 3:2 and back-face mode
 * in bits 1:0, POINT = 0, LINE = 1, FILL = 2. Front LINE, back FILL on a CCW
 * (front- facing) triangle draws no interior; the same triangle wound the
 * other way is now the back face and fills.
 */
#define ODSY_REG_POLYGON_MODE 0x00010415

static void test_command_fifo_gl_polygon_mode(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);

    /* Front LINE, back FILL: the CCW (front-facing) triangle stays open. */
    set_reg(qts, ODSY_REG_POLYGON_MODE, 0x6);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

    /* The same triangle, wound the other way, is now the back face: filled. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow_cw);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * Line width and point size: 0x084a packs the line width in operand 0 and the
 * point size in operand 1, each trunc(size * 128) in bits 23:0. Counting how
 * many pixels a horizontal line or a single point lights up, rather than
 * asserting exact positions, is sensitive to whether the register reached the
 * shared layer's line_width/point_size fields without depending on the
 * rasterizer's own sub-pixel rounding convention.
 */
#define ODSY_REG_LINE_POINT_SIZE 0x0001084a
#define ODSY_REG_PATTERN_MODE 0x00010404
#define ODSY_MODE_POINT_SMOOTH (1u << 0)
#define ODSY_MODE_LINE_SMOOTH (1u << 1)

static void set_line_point_size(QTestState *qts, uint32_t line_width,
                                uint32_t point_size)
{
    user_cmd_push(qts, ODSY_REG_LINE_POINT_SIZE);
    user_cmd_push(qts, line_width);
    user_cmd_push(qts, point_size);
}

static void push_line_3f(QTestState *qts, uint32_t r, uint32_t g, uint32_t b,
                         const uint32_t v[6])
{
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, r);
    user_cmd_push(qts, g);
    user_cmd_push(qts, b);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_LINES);
    user_cmd_push(qts, ODSY_GL_VERTEX_3F);
    user_cmd_push(qts, v[0]);
    user_cmd_push(qts, v[1]);
    user_cmd_push(qts, v[2]);
    user_cmd_push(qts, ODSY_GL_VERTEX_3F);
    user_cmd_push(qts, v[3]);
    user_cmd_push(qts, v[4]);
    user_cmd_push(qts, v[5]);
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

static void push_point_3f(QTestState *qts, uint32_t r, uint32_t g, uint32_t b,
                          const uint32_t v[3])
{
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, r);
    user_cmd_push(qts, g);
    user_cmd_push(qts, b);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_POINTS);
    user_cmd_push(qts, ODSY_GL_VERTEX_3F);
    user_cmd_push(qts, v[0]);
    user_cmd_push(qts, v[1]);
    user_cmd_push(qts, v[2]);
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

static unsigned odsy_count_yellow(QTestState *qts, uint32_t x0, uint32_t x1,
                                  uint32_t y0, uint32_t y1)
{
    uint32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
    g_autofree uint32_t *scan = g_new(uint32_t, w * h);
    unsigned n = 0;

    odsy_test_pixels(qts, x0, y0, w, h, scan);
    for (uint32_t i = 0; i < w * h; i++) {
        if (scan[i] == ODSY_PLANE_YELLOW) {
            n++;
        }
    }
    return n;
}

static unsigned odsy_count_partial_red_over_blue(QTestState *qts,
                                                 uint32_t x0, uint32_t x1,
                                                 uint32_t y0, uint32_t y1)
{
    uint32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
    g_autofree uint32_t *scan = g_new(uint32_t, w * h);
    unsigned n = 0;

    odsy_test_pixels(qts, x0, y0, w, h, scan);
    for (uint32_t i = 0; i < w * h; i++) {
        if (scan[i] != ODSY_PLANE_BLUE && scan[i] != ODSY_PLANE_RED) {
            n++;
        }
    }
    return n;
}

static void assert_red_over_blue_coverage(QTestState *qts, uint32_t x,
                                          uint32_t y)
{
    uint32_t pixel = odsy_test_pixel(qts, x, y);
    uint32_t red = (pixel >> 24) & 0xff;
    uint32_t green = (pixel >> 16) & 0xff;
    uint32_t blue = (pixel >> 8) & 0xff;

    g_assert_cmpuint(red, >=, 198);
    g_assert_cmpuint(red, <=, 202);
    g_assert_cmpuint(green, ==, 0);
    g_assert_cmpuint(blue, >=, 53);
    g_assert_cmpuint(blue, <=, 57);
}

static void set_smoothing(QTestState *qts, uint32_t bits)
{
    user_cmd_push(qts, ODSY_REG_PATTERN_MODE);
    user_cmd_push(qts, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND | bits);
}

static void test_command_fifo_gl_line_point_size(void)
{
    /* A horizontal line and a point, both centred on local (64, 64). */
    static const uint32_t horizontal_line[6] = {
        0xbf000000, 0x00000000, 0xbf000000,
        0x3f000000, 0x00000000, 0xbf000000,
    };
    static const uint32_t centre_point[3] = {
        0x00000000, 0x00000000, 0xbf000000,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);

    /* Default width and size, 1.0 and 1.0: a single row, a single pixel. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, horizontal_line);
    g_assert_cmpuint(odsy_count_yellow(qts, 320, 320, 314, 326), ==, 1);

    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_point_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, centre_point);
    g_assert_cmpuint(odsy_count_yellow(qts, 316, 323, 316, 323), ==, 1);

    /* 0x180 = 3.0 line width: three rows. */
    set_line_point_size(qts, 0x180, 0x80);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, horizontal_line);
    g_assert_cmpuint(odsy_count_yellow(qts, 320, 320, 314, 326), ==, 3);

    /* 0x200 = 4.0 point size: a 4 x 4 square. */
    set_line_point_size(qts, 0x80, 0x200);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_point_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, centre_point);
    g_assert_cmpuint(odsy_count_yellow(qts, 316, 323, 316, 323), ==, 16);

    board_quit(qts);
}

static void test_command_fifo_gl_point_smoothing(void)
{
    static const uint32_t centre_point[3] = {
        0x00000000, 0x00000000, 0xbf000000,
    };
    QTestState *qts = board_start("");
    static const uint32_t sample_x[] = { 319, 320, 319, 320 };
    static const uint32_t sample_y[] = { 319, 319, 320, 320 };
    unsigned i;

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND);
    set_blend(qts, 0x639, 0, 0);
    set_line_point_size(qts, 0x80, 0x100);

    set_smoothing(qts, 0);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE);
    push_point_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, centre_point);
    for (i = 0; i < ARRAY_SIZE(sample_x); i++) {
        g_assert_cmphex(odsy_test_pixel(qts, sample_x[i], sample_y[i]), ==,
                        ODSY_PLANE_RED);
    }

    set_smoothing(qts, ODSY_MODE_POINT_SMOOTH);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE);
    push_point_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, centre_point);
    for (i = 0; i < ARRAY_SIZE(sample_x); i++) {
        assert_red_over_blue_coverage(qts, sample_x[i], sample_y[i]);
    }

    set_smoothing(qts, 0);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE);
    push_point_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, centre_point);
    for (i = 0; i < ARRAY_SIZE(sample_x); i++) {
        g_assert_cmphex(odsy_test_pixel(qts, sample_x[i], sample_y[i]), ==,
                        ODSY_PLANE_RED);
    }

    board_quit(qts);
}

static void test_command_fifo_gl_line_smoothing(void)
{
    static const uint32_t diagonal_line[6] = {
        0xbe800000, 0xbe800000, 0xbf000000,
        0x3e800000, 0x3e800000, 0xbf000000,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND);
    set_blend(qts, 0x639, 0, 0);
    set_line_point_size(qts, 0x80, 0x80);

    set_smoothing(qts, 0);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, diagonal_line);
    g_assert_cmpuint(odsy_count_partial_red_over_blue(qts, 296, 344, 296, 344),
                     ==, 0);

    set_smoothing(qts, ODSY_MODE_LINE_SMOOTH);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, diagonal_line);
    g_assert_cmpuint(odsy_count_partial_red_over_blue(qts, 296, 344, 296, 344),
                     >, 0);

    set_smoothing(qts, 0);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, diagonal_line);
    g_assert_cmpuint(odsy_count_partial_red_over_blue(qts, 296, 344, 296, 344),
                     ==, 0);

    board_quit(qts);
}

static void test_command_fifo_gl_line_stipple(void)
{
    static const uint32_t line[6] = {
        0xbf000000, 0, 0xbf000000,
        0x3f000000, 0, 0xbf000000,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    user_cmd_push(qts, ODSY_CMD_LINE_STIPPLE);
    user_cmd_push(qts, 0x00f3);
    user_cmd_push(qts, 0x00f3);
    user_cmd_push(qts, 1); /* Repeat each pattern bit twice. */
    set_reg(qts, ODSY_REG_GL_MODE,
            ODSY_GL_MODE_FLAT | ODSY_MODE_GL_LINE_STIPPLE);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, line);
    g_assert_cmpuint(odsy_count_yellow(qts, 290, 290, 319, 321), ==, 1);
    g_assert_cmpuint(odsy_count_yellow(qts, 294, 294, 319, 321), ==, 0);
    g_assert_cmpuint(odsy_count_yellow(qts, 300, 300, 319, 321), ==, 1);
    g_assert_cmpuint(odsy_count_yellow(qts, 310, 310, 319, 321), ==, 0);
    g_assert_cmpuint(odsy_count_yellow(qts, 322, 322, 319, 321), ==, 1);

    /* Clearing bit 3 makes the same float GL line solid. */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    push_line_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, line);
    g_assert_cmpuint(odsy_count_yellow(qts, 294, 294, 319, 321), ==, 1);
    g_assert_cmpuint(odsy_count_yellow(qts, 310, 310, 319, 321), ==, 1);
    board_quit(qts);
}

static void test_command_fifo_gl_line_stipple_migration(void)
{
    static const uint32_t line[6] = {
        0xbf000000, 0, 0xbf000000,
        0x3f000000, 0, 0xbf000000,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-gl-line-stipple-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    user_cmd_push(from, ODSY_CMD_LINE_STIPPLE);
    user_cmd_push(from, 0x00f3);
    user_cmd_push(from, 0x00f3);
    user_cmd_push(from, 1);
    set_reg(from, ODSY_REG_GL_MODE,
            ODSY_GL_MODE_FLAT | ODSY_MODE_GL_LINE_STIPPLE);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    probe_window(to);
    push_line_3f(to, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, line);
    g_assert_cmpuint(odsy_count_yellow(to, 290, 290, 319, 321), ==, 1);
    g_assert_cmpuint(odsy_count_yellow(to, 294, 294, 319, 321), ==, 0);
    g_assert_cmpuint(odsy_count_yellow(to, 300, 300, 319, 321), ==, 1);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * Polygon offset: 0x0404 bit 7 is the fill-polygon GL_POLYGON_OFFSET_FILL
 * enable, and 0x0458 is a signed 25-bit bias in bits 24:0, scaled by 2^-24
 * into normalized window depth (EXT_polygon_offset). Two coplanar triangles
 * under GL_LESS: without the offset the second one's equal depth ties and
 * loses; with a negative bias large enough to be unambiguous, it wins.
 */
#define ODSY_MODE_OFFSET_FILL (1u << 7)
#define ODSY_REG_POLYGON_OFFSET 0x00010458
/* sextract32(word, 0, 25) / 2^24 == -0.25: large next to any rounding noise. */
#define ODSY_OFFSET_BIAS_NEG_QUARTER 0x01c00000

/* odsy_near_yellow's footprint, flattened to NDC z 0.0 (window depth 0.5). */
static const uint32_t odsy_coplanar_tri[9] = {
    0xbf4ccccd, 0xbf4ccccd, 0x00000000,
    0x3ecccccd, 0xbf4ccccd, 0x00000000,
    0xbe4ccccd, 0x3f19999a, 0x00000000,
};

static void test_command_fifo_gl_polygon_offset(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);

    /* Same depth, offset disabled: the second (red) triangle ties, loses. */
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_coplanar_tri);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO,
                     odsy_coplanar_tri);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    /* Same depth, offset enabled with a negative bias: the second wins. */
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_coplanar_tri);
    set_reg(qts, ODSY_REG_GL_MODE,
            ODSY_GL_MODE_DEPTH_FLAT | ODSY_MODE_OFFSET_FILL);
    set_reg(qts, ODSY_REG_POLYGON_OFFSET, ODSY_OFFSET_BIAS_NEG_QUARTER);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO,
                     odsy_coplanar_tri);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_RED);

    board_quit(qts);
}

/*
 * Clip box: 0x081d packs two inclusive, window-relative fields per word,
 * x_min/x_max then y_min/y_max, each 12 bits, y counted down from the
 * window's top row. It reaches the shared layer as glScissor, with
 * GL_SCISSOR_TEST left always on (there is no enable bit; the library always
 * sends the intersection it wants drawn). A full-window fill restricted to
 * the probe window's top eight rows paints only that band; any min > max
 * field, including the library's own 0x1000, 0x1000 words, clips every
 * fragment.
 */
#define ODSY_REG_CLIP_BOX 0x0001081d

static void clip_box_set(QTestState *qts, uint32_t x_min, uint32_t x_max,
                         uint32_t y_min, uint32_t y_max)
{
    user_cmd_push(qts, ODSY_REG_CLIP_BOX);
    user_cmd_push(qts, (x_min << 12) | x_max);
    user_cmd_push(qts, (y_min << 12) | y_max);
}

/*
 * Two triangles spanning the probe window's full clip-space extent. The
 * clip-box test needs a vertex-based fill, not the family's own GL_RECT
 * (the screen-aligned-rectangle clear path, ODSY_GL_RECT): that op builds
 * its raster directly in absolute screen coordinates and is tested only
 * against this family's own clip rectangles (odyssey_gl_rect(),
 * sgi_soft_raster()'s SGI_RASTER_CLEAR case), never against the shared
 * per-fragment path, so it never consults the shared scissor box. A
 * triangle fill goes through sgi_soft_fragment_at() and does.
 */
static const uint32_t odsy_full_window_tri_a[9] = {
    0xbf800000, 0xbf800000, 0,          /* (-1, -1, 0) */
    0x3f800000, 0xbf800000, 0,          /* ( 1, -1, 0) */
    0x3f800000, 0x3f800000, 0,          /* ( 1,  1, 0) */
};
static const uint32_t odsy_full_window_tri_b[9] = {
    0xbf800000, 0xbf800000, 0,          /* (-1, -1, 0) */
    0x3f800000, 0x3f800000, 0,          /* ( 1,  1, 0) */
    0xbf800000, 0x3f800000, 0,          /* (-1,  1, 0) */
};

static void push_full_window_tris(QTestState *qts, uint32_t r, uint32_t g,
                                  uint32_t b)
{
    push_triangle_3f(qts, r, g, b, odsy_full_window_tri_a);
    push_triangle_3f(qts, r, g, b, odsy_full_window_tri_b);
}

static void test_command_fifo_gl_clip_box(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);

    /* Restricted to the top eight rows: only that band takes the fill. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    clip_box_set(qts, 0, 127, 0, 7);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 260), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

    /* A restored full-window box takes the fill everywhere again. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    clip_box_set(qts, 0, 127, 0, 127);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    /* The library's own empty encoding, min > max on both words: nothing. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    user_cmd_push(qts, ODSY_REG_CLIP_BOX);
    user_cmd_push(qts, 0x1000);
    user_cmd_push(qts, 0x1000);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);

    board_quit(qts);
}

/*
 * Clip planes: plane j is loaded at class 0x31 index 0x120 + 4j with
 * 0x908091bb and four eye-space floats, and 0x0405 bits 2:0 thermometer-code
 * how many of the loaded slots are tested (bit 2 set for n > 0). The probe
 * window's combined matrix is identity, so clip-space x is window-relative x
 * scaled to [-1, 1]; plane (1, 0, 0, 0) keeps only x >= 0, the window's right
 * half.
 */
#define ODSY_REG_TEX_MODE 0x00010405
#define ODSY_GL_CLIP_PLANE_LOAD 0x908091bb

static void clip_plane_load(QTestState *qts, unsigned j, uint32_t a,
                            uint32_t b, uint32_t c, uint32_t d)
{
    user_cmd_push(qts, ODSY_PROG(0x31, 0x120 + 4 * j));
    user_cmd_push(qts, ODSY_GL_CLIP_PLANE_LOAD);
    user_cmd_push(qts, a);
    user_cmd_push(qts, b);
    user_cmd_push(qts, c);
    user_cmd_push(qts, d);
}

static void test_command_fifo_gl_clip_plane(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    clip_plane_load(qts, 0, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);

    /* n = 0 (no bits set): the plane is loaded but not yet tested. */
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 270, 340), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    /* n = 1 (bit 2 set): only the right half, x >= 0, survives. */
    set_reg(qts, ODSY_REG_TEX_MODE, 0x4);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 270, 340), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    /* Disabled again (n = 0): the whole window takes the fill once more. */
    set_reg(qts, ODSY_REG_TEX_MODE, 0x0);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 270, 340), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * glVertex3i and glVertex3s send 0x8080cc02 and three raw signed integers.
 * Under a combined matrix that maps object x and y straight to window
 * pixels, the integer triangle (-100, 0) (60, 0) (-100, 120) covers window
 * pixel (10, 20) and not (100, 100). Read as unsigned, -100 would put two
 * corners past 4e9 and leave (10, 20) uncovered; read as floats, the
 * operands are denormals and draw nothing. A float triangle afterwards
 * still draws, so all three operands were consumed as data.
 */
static void test_command_fifo_gl_vertex_3i(void)
{
    static const uint32_t pixels[16] = {
        0x3f800000, 0, 0, 0,
        0, 0x3f800000, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0x3f800000,
    };
    static const uint32_t int_tri[] = {
        ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT,
        ODSY_GL_COLOR_3F, 0, 0x3f800000, 0,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        0x8080cc02, (uint32_t)-100, 0, 0,
        0x8080cc02, 60, 0, 0,
        0x8080cc02, (uint32_t)-100, 120, 7,
        ODSY_CMD_GL_END,
    };
    static const uint32_t float_tri[] = {
        ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_VERTEX_3F, 0x42c80000, 0x42c80000, 0,  /* (100, 100) */
        ODSY_GL_VERTEX_3F, 0x42f00000, 0x42c80000, 0,  /* (120, 100) */
        ODSY_GL_VERTEX_3F, 0x42c80000, 0x42f00000, 0,  /* (100, 120) */
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    push_window_transform(qts, pixels);
    push_block_16(qts, 0x3f800000);
    push_user_words(qts, int_tri, ARRAY_SIZE(int_tri));
    g_assert_cmphex(odsy_test_pixel(qts, 256 + 10, 256 + 20), ==,
                    ODSY_PLANE_GREEN);
    g_assert_cmphex(odsy_test_pixel(qts, 256 + 100, 256 + 100), !=,
                    ODSY_PLANE_GREEN);
    push_user_words(qts, float_tri, ARRAY_SIZE(float_tri));
    g_assert_cmphex(odsy_test_pixel(qts, 256 + 102, 256 + 102), ==,
                    0xff0000ffu);

    board_quit(qts);
}

/*
 * The record light.c selects: entries 0x5cb 0x5d8 0x5e5, one directional
 * light. The triangle is lit -- (1, 0.5, 0.25) where the normal faces the
 * light, 0.8 of it at the right corner, black edge-on -- the values the
 * offline replay of the capture draws. A lone 0x29 stream-entry change and a
 * light-colour load afterwards leave the program lit.
 */
static void test_command_fifo_gl_program_record_lit(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    static const uint32_t reload[] = {
        0x2900819a, 0x32008018, 0x8e809015, 0, 0, 0, 0x3f800000, 0x290080d6,
    };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 360), ==, 0xe37139ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x090402ffu);

    push_user_words(qts, reload, ARRAY_SIZE(reload));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * The library's other precompiled lit records with resident entries, each
 * with the list the library loads beside it (record, then the one-shot
 * routines). ivview selects record 9 for its headlight under
 * GL_NORMALIZE and perfly record 11. Each lights light.c's triangle as
 * record 3 does; an unrecognised program would draw it in the current
 * red.
 */
static void test_command_fifo_gl_program_records_lit(void)
{
    static const struct {
        uint32_t e[3];
        uint32_t list[5];
    } records[] = {
        { { 0x62c, 0x63d, 0x64d }, { 0x49a, 0x203, 0x4ca, 0x4c5 } },
        { { 0x6d8, 0x6eb, 0x6fd }, { 0x49f, 0x203, 0x4ca, 0x4f1, 0x4c5 } },
        { { 0x5f0, 0x605, 0x619 }, { 0x49a, 0x203, 0x4ca, 0x4c5 } },
        { { 0x657, 0x66e, 0x684 }, { 0x49a, 0x203, 0x4ca, 0x4c5 } },
        { { 0x699, 0x6b4, 0x6ce }, { 0x49a, 0x203, 0x4ca, 0x4c5 } },
    };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    for (unsigned i = 0; i < ARRAY_SIZE(records); i++) {
        unsigned n = records[i].list[4] ? 5 : 4;

        push_program(qts, 0, 0x0d6, 0x0e0, 0x0ea);
        push_constants(qts, records[i].list, n);
        push_program(qts, 0, records[i].e[0], records[i].e[1],
                     records[i].e[2]);
        push_user_words(qts, odsy_lit_triangle,
                        ARRAY_SIZE(odsy_lit_triangle));
        g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);
        g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x090402ffu);
    }

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * Colour material (the class 0x35 word): light.c's lit record, whose front
 * diffuse material is (1, 0.5, 0.25), drawn under the current colour red.
 * The library's word for GL_FRONT, GL_AMBIENT_AND_DIFFUSE (3) lights the
 * triangle red; a back-only word (0x30) leaves the front material in charge,
 * and 0, sent when GL_COLOR_MATERIAL is disabled, restores it. Bits past the
 * eight the library uses are reported.
 */
static void test_command_fifo_gl_colour_material(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    user_cmd_push(qts, ODSY_PROG(0x35, 0x3));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe0000ffu);

    user_cmd_push(qts, ODSY_PROG(0x35, 0x30));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);

    user_cmd_push(qts, ODSY_PROG(0x35, 0x3));
    user_cmd_push(qts, ODSY_PROG(0x35, 0));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);

    user_cmd_push(qts, ODSY_PROG(0x35, 0x102));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "command FIFO word"));
    g_assert_nonnull(strstr(log, "colour-material word value 0x102"));
}

/*
 * The integer colour words: glColor4ub as pointblank sends it, and one each
 * of the three-component unsigned short and four-component signed byte
 * forms, all with the component type in the word's low six bits and one raw
 * component a word. Under the unlit record each draws light.c's triangle in
 * its colour. Under light.c's lit record with colour material on the front
 * diffuse (0x35 value 2), glColor4ub's colour must reach the lighting: the
 * pastel red lights where the material's (1, 0.5, 0.25) would.
 */
static void test_command_fifo_gl_integer_colour(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    static const struct {
        uint32_t word, op[4];
        uint32_t pixel;
    } colours[] = {
        { 0xc580d00f, { 0x33, 0x66, 0x99, 0xff }, 0x336699ffu },
        { 0xc580cc09, { 0xffff, 0x8000, 0 }, 0xff8000ffu },
        { 0xc580d012, { 0x7f, 0xffffff80, 0, 0x7f }, 0xff0001ffu },
    };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);
    uint32_t tri[ARRAY_SIZE(odsy_lit_triangle) + 1];

    for (unsigned i = 0; i < ARRAY_SIZE(colours); i++) {
        unsigned n = (colours[i].word >> 10) & 0xf;

        push_program(qts, 0, 0x0d6, 0x0e0, 0x0ea);
        user_cmd_push(qts, colours[i].word);
        push_user_words(qts, colours[i].op, n);
        /* light.c's triangle without its glColor3f. */
        push_user_words(qts, odsy_lit_triangle + 4,
                        ARRAY_SIZE(odsy_lit_triangle) - 4);
        g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, colours[i].pixel);
    }

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    user_cmd_push(qts, ODSY_PROG(0x35, 0x2));
    tri[0] = 0xc580d00f;
    tri[1] = 0xcc;
    tri[2] = 0x66;
    tri[3] = 0x66;
    tri[4] = 0xff;
    memcpy(&tri[5], odsy_lit_triangle + 4,
           (ARRAY_SIZE(odsy_lit_triangle) - 4) * sizeof(uint32_t));
    push_user_words(qts, tri, ARRAY_SIZE(tri));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xcb6565ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "command FIFO word"));
}

/*
 * light.c's triangle lit under one normal sent before BEGIN, given as a
 * command word and its three operands, or under the current normal when
 * normal is NULL; returns the pixel inside it.
 */
static uint32_t lit_one_normal(QTestState *qts, const uint32_t normal[4])
{
    static const uint32_t triangle[] = {
        ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
        ODSY_REG_GL_MODE, ODSY_GL_MODE_SMOOTH,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_VERTEX_2F, 0xbf400000, 0xbf400000,
        ODSY_GL_VERTEX_2F, 0x3f400000, 0xbf200000,
        ODSY_GL_VERTEX_2F, 0xbe000000, 0x3f3c0000,
        ODSY_CMD_GL_END,
    };

    if (normal) {
        push_user_words(qts, normal, 4);
    }
    push_user_words(qts, triangle, ARRAY_SIZE(triangle));
    return odsy_test_pixel(qts, 300, 360);
}

/* Whether each 8-bit channel of a and b differs by at most one. */
static bool pixel_near(uint32_t a, uint32_t b)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        int d = (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);

        if (abs(d) > 1) {
            return false;
        }
    }
    return true;
}

/*
 * The integer normal words decode by the GL library's rules -- byte
 * (2v + 1) / 255, short (2v + 1) / 65535, int (2v + 1) / (2^32 - 2560) --
 * over the whole signed operand word, and the model lights the normalised
 * result. The byte form's zero components decode to 1/255, so its +z normal
 * is slightly off axis and lands within one unit.
 * Each integer normal follows a float one that lights visibly differently,
 * so a model that accepted a word but kept the previous normal fails. A
 * model that read the operands as floats (0x7fffffff is a NaN), read
 * 0xffffff81 as an unsigned byte (0x81 decodes positive), or truncated the
 * byte operand 0x100 to eight bits (zero, decoding to 1/255 on every axis)
 * lights differently too.
 */
static void test_command_fifo_gl_integer_normals(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    static const uint32_t up_f[4] = { 0xc180cc19, 0, 0, 0x3f800000 };
    static const uint32_t up_i[4] = { 0xc180cc18, 0, 0, 0x7fffffff };
    static const uint32_t up_s[4] = { 0xc180cc17, 0, 0, 0x7fff };
    static const uint32_t up_b[4] = { 0xc180cc16, 0, 0, 0x7f };
    static const uint32_t wide_b[4] = { 0xc180cc16, 0, 0, 0x100 };
    static const uint32_t slant_f[4] = {
        0xc180cc19, 0x3f19999a, 0, 0x3f4ccccd,
    };
    static const uint32_t slant_s[4] = { 0xc180cc17, 0x4ccc, 0, 0x6666 };
    static const uint32_t down_f[4] = { 0xc180cc19, 0, 0, 0xbf800000 };
    static const uint32_t down_b[4] = { 0xc180cc16, 0, 0, 0xffffff81 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);
    uint32_t up, slant, down;

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    up = lit_one_normal(qts, up_f);
    g_assert_cmphex(up, ==, 0xff8040ffu);
    down = lit_one_normal(qts, down_f);
    g_assert_false(pixel_near(down, up));
    slant = lit_one_normal(qts, slant_f);
    g_assert_false(pixel_near(slant, up));
    g_assert_false(pixel_near(slant, down));

    g_assert_cmphex(lit_one_normal(qts, up_i), ==, up);
    lit_one_normal(qts, down_f);
    g_assert_cmphex(lit_one_normal(qts, up_s), ==, up);
    lit_one_normal(qts, down_f);
    g_assert_true(pixel_near(lit_one_normal(qts, up_b), up));
    lit_one_normal(qts, down_f);
    g_assert_true(pixel_near(lit_one_normal(qts, wide_b), up));
    g_assert_true(pixel_near(lit_one_normal(qts, slant_s), slant));
    g_assert_true(pixel_near(lit_one_normal(qts, down_b), down));

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "0xc180cc16"));
}

/*
 * The unlit record every probe starts with, 0x0d6 0x0e0 0x0ea: the same
 * triangle takes the current colour, red, not the lit values.
 */
static void test_command_fifo_gl_program_record_unlit(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4c5 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x0d6, 0x0e0, 0x0ea);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0xff0000ffu);

    /* Lit, then unlit again: the unlit draw takes red back. */
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    push_user_words(qts, odsy_lit_triangle + 4,
                    ARRAY_SIZE(odsy_lit_triangle) - 4);
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);
    push_program(qts, 0, 0x0d6, 0x0e0, 0x0ea);
    push_user_words(qts, odsy_lit_triangle + 4,
                    ARRAY_SIZE(odsy_lit_triangle) - 4);
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * light2.c's assembled program list (head 0x290: infinite viewer, first
 * light directional) and its light, material and light-model words: light 0
 * directional with ambient, diffuse and specular; light 1 a positional,
 * attenuated spot whose 45 degree cone misses every vertex of the probe's
 * triangle; specular, emission and shininess 10 in the material.
 */
static const uint32_t odsy_light2_list[] = {
    0x290, 0x2b6, 0x2cf, 0x398, 0x2de, 0x300, 0x3f5, 0x49a, 0x203,
    0x25e, 0x4c5,
};

static const uint32_t odsy_light2_data[] = {
    0xc780d030, 0x3f800000, 0x3f000000, 0x3e800000, 0x3f800000,
    0xc680d02f, 0x3dcccccd, 0x3e19999a, 0x3e4ccccd, 0x3f800000,
    0xc880d031, 0x3e4ccccd, 0x3e99999a, 0x3ecccccd, 0x3f800000,
    0xc980d032, 0x3d4ccccd, 0, 0, 0x3f800000,
    0xd080c433, 0x404a62c2,
    0x908091ea, 0x404a62c2, 0xc04a62c2, 0, 0x80000000,
    0x2900819a,
    0x32008018, 0x8e809015, 0x3c23d70a, 0x3ca3d70a, 0x3cf5c28f,
    0x3f800000,
    0x31008000, 0x9080bda2, 0x3eb851ec, 0x3ef5c28f, 0x3f4ccccd,
    0xbe424a54, 0xbe8186e2, 0xbf72dce8, 0x3f72dce8, 0x3f800000, 0, 0,
    0x80000000, 0x80000000, 0x3f800000, 0x3f800000, 0, 0x4a800000,
    0x32008001, 0x8e809015, 0x3f666666, 0x3f4ccccd, 0x3f333333,
    0x3f800000,
    0x32008002, 0x8e809015, 0x3f000000, 0x3ecccccd, 0x3e99999a,
    0x3f800000,
    0x32008000, 0x8e809015, 0x3d4ccccd, 0x3d75c28f, 0x3d8f5c29,
    0x3f800000,
    0x31008010, 0x9080bda2, 0x3f800000, 0x40000000, 0x40400000, 0, 0, 0,
    0, 0x3f800000, 0x3f000000, 0x3e800000, 0xbf19999a, 0x80000000,
    0x3f4ccccd, 0xbf3504f3, 0xc0000000, 0x4a800000,
    0x32008004, 0x8e809015, 0x3de147ae, 0x3e6147ae, 0x3ea8f5c3,
    0x3f800000,
    0x32008005, 0x8e809015, 0, 0, 0, 0x3f800000,
    0x32008003, 0x8e809015, 0, 0, 0, 0x3f800000,
    0x290080d6,
};

/*
 * light2.c's assembled list, 0x2a and 0x2c the terminator: head 0x290 (lit,
 * infinite viewer), a positional attenuated second light, one-sided finish.
 * Its lights: 0 directional with ambient, diffuse and specular; 1 a
 * positional spot whose cone misses every vertex; a light-model ambient, and
 * a material with specular, emission and shininess 10. The triangle matches
 * the offline replay's frame. Then light 1's colours zeroed and record 0x5cb
 * again, as after glDisable(GL_LIGHT1): the same colours, light 1 having
 * added nothing.
 */
static void test_command_fifo_gl_program_list_lit(void)
{
    static const uint32_t one_light[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    static const uint32_t light1_off[] = {
        0x2900819a, 0x32008004, 0x8e809015, 0, 0, 0, 0x3f800000, 0x290080d6,
    };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_user_words(qts, odsy_light2_data, ARRAY_SIZE(odsy_light2_data));
    push_constants(qts, odsy_light2_list, ARRAY_SIZE(odsy_light2_list));
    push_program(qts, 1, 0x290, 0x4c5, 0x4c5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xd4663bffu);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 360), ==, 0xd06237ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x652a16ffu);

    push_user_words(qts, light1_off, ARRAY_SIZE(light1_off));
    push_constants(qts, one_light, ARRAY_SIZE(one_light));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xd4663bffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x652a16ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * Light 1's own record, pushed after odsy_light2_data to override it:
 * the selector and header of ODSY_GL_LIGHT_VEC_LOAD, then sixteen floats
 * in the ODSY_LIGHT_POS/HALF/ATTEN/SPOT_DIR/SPOT_COS/SPOT_EXP layout
 * documented in hw/misc/sgi-odyssey.c, followed by ODSY_GL_LIGHT_COLOUR_LOAD.
 * Position, attenuation and spot direction match light2.c's own light 1;
 * the cutoff word (ODSY_LIGHT_SPOT_COS, minus the cosine) is 1, a 180
 * degree cutoff and so no spot, and the diffuse colour is raised to
 * (1, 1, 1, 1).
 */
static const uint32_t odsy_light2_light1_wide[] = {
    0x31008010, 0x9080bda2, 0x3f800000, 0x40000000, 0x40400000, 0, 0, 0,
    0, 0x3f800000, 0x3f000000, 0x3e800000, 0xbf19999a, 0x80000000,
    0x3f4ccccd, 0x3f800000, 0xc0000000, 0x4a800000,
    0x32008004, 0x8e809015, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
};

/*
 * The same light's vector record with its original, narrow cutoff
 * (ODSY_LIGHT_SPOT_COS is minus the cosine of about 45 degrees), pushed
 * to restore it.
 */
static const uint32_t odsy_light2_light1_narrow[] = {
    0x31008010, 0x9080bda2, 0x3f800000, 0x40000000, 0x40400000, 0, 0, 0,
    0, 0x3f800000, 0x3f000000, 0x3e800000, 0xbf19999a, 0x80000000,
    0x3f4ccccd, 0xbf3504f3, 0xc0000000, 0x4a800000,
};

/*
 * The local viewer: light2.c's list with head 0x281 (local viewer, first
 * light directional) in place of 0x290, so the half vector is
 * normalize(L + normalize(-eye)) rather than L + (0, 0, 1). The expected
 * pixels are SPEC-DERIVED and HARDWARE-UNVERIFIED: no capture used a local
 * viewer. They come from the offline replay extended with the local half
 * vector, whose vertex colours (0.776, 0.332, 0.160), (0.826, 0.354, 0.170)
 * and (0.380, 0.156, 0.083) equal an independent evaluation of OpenGL
 * 1.2.1 equation 2.5 from light2.c's parameters. The first two differ
 * from the infinite viewer's (212, 102, 59) and (208, 98, 55); the apex,
 * edge-on to light 0, has no specular term either way.
 */
static void test_command_fifo_gl_program_list_lit_local_viewer(void)
{
    uint32_t list[ARRAY_SIZE(odsy_light2_list)];
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    memcpy(list, odsy_light2_list, sizeof(list));
    list[0] = 0x281;
    push_user_words(qts, odsy_light2_data, ARRAY_SIZE(odsy_light2_data));
    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 1, 0x281, 0x4c5, 0x4c5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xc55429ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 360), ==, 0xc45428ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x652a16ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * A positional light that reaches its vertices: light2.c's light 1 with its
 * cutoff word (float 13, minus the cosine) set to 1, a 180 degree cutoff and
 * so no spot, and its diffuse raised to (1, 1, 1), so the attenuation
 * 1 / (1 + 0.5 d + 0.25 d^2) and L = P - eye decide the colour. The expected
 * pixels are SPEC-DERIVED and HARDWARE-UNVERIFIED: they come from the
 * offline replay on these words, whose vertex colours (0.918, 0.445,
 * 0.252), (0.943, 0.431, 0.223) and (0.437, 0.185, 0.097) equal an
 * independent evaluation of OpenGL 1.2.1 equation 2.5 with the same
 * change. Then the probe's 45 degree cutoff again: the cone misses every
 * vertex and the pixels are light2.c's own, which proves the cone test.
 */
static void test_command_fifo_gl_program_list_lit_positional(void)
{
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_user_words(qts, odsy_light2_data, ARRAY_SIZE(odsy_light2_data));
    push_user_words(qts, odsy_light2_light1_wide,
                     ARRAY_SIZE(odsy_light2_light1_wide));
    push_constants(qts, odsy_light2_list, ARRAY_SIZE(odsy_light2_list));
    push_program(qts, 1, 0x290, 0x4c5, 0x4c5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xea7140ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 360), ==, 0xe56d3cffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x74321affu);

    push_user_words(qts, odsy_light2_light1_narrow,
                     ARRAY_SIZE(odsy_light2_light1_narrow));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xd4663bffu);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 360), ==, 0xd06237ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 276), ==, 0x652a16ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * The positional setup above, light 1 reaching every vertex, so a vertex's
 * colour depends on where it is: attenuation and L = P - eye both follow it.
 */
static QTestState *lit_positional_start(char **log_path)
{
    QTestState *qts = lit_start(log_path);

    push_user_words(qts, odsy_light2_data, ARRAY_SIZE(odsy_light2_data));
    push_user_words(qts, odsy_light2_light1_wide,
                     ARRAY_SIZE(odsy_light2_light1_wide));
    push_constants(qts, odsy_light2_list, ARRAY_SIZE(odsy_light2_list));
    push_program(qts, 1, 0x290, 0x4c5, 0x4c5);
    return qts;
}

/*
 * Window pixels inside the triangle (-1, -1) (1, -1) (-1, 1), which covers
 * the lower-left half of the probe's window, away from its edges.
 */
static const unsigned odsy_lower_left_pixels[][2] = {
    { 270, 370 }, { 300, 340 }, { 330, 370 }, { 270, 320 },
};

/*
 * Draw that triangle smooth under a +z normal, its three vertices given as
 * the word and operands in v (four words each, or three when stride is 3),
 * and read back odsy_lower_left_pixels.
 */
static void lit_lower_left(QTestState *qts, const uint32_t *v,
                           unsigned stride, uint32_t out[4])
{
    static const uint32_t head[] = {
        ODSY_GL_COLOR_3F, 0x3f800000, 0, 0,
        ODSY_REG_GL_MODE, ODSY_GL_MODE_SMOOTH,
        ODSY_GL_NORMAL_3F, 0, 0, 0x3f800000,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
    };

    push_user_words(qts, head, ARRAY_SIZE(head));
    push_user_words(qts, v, 3 * stride);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    for (unsigned i = 0; i < ARRAY_SIZE(odsy_lower_left_pixels); i++) {
        out[i] = odsy_test_pixel(qts, odsy_lower_left_pixels[i][0],
                                 odsy_lower_left_pixels[i][1]);
    }
}

/*
 * A lit integer 3D vertex lights where it is drawn: glVertex3i's triangle
 * under the positional light takes exactly the colours of the same
 * triangle sent as floats. Read as float bits, -1 would be a NaN and 1 a
 * denormal, so attenuation and the light direction would come out wrong.
 * The first two sampled pixels differ, so the position matters here.
 */
static void test_command_fifo_gl_vertex_3i_lit(void)
{
    static const uint32_t ints[] = {
        0x8080cc02, (uint32_t)-1, (uint32_t)-1, 0,
        0x8080cc02, 1, (uint32_t)-1, 0,
        0x8080cc02, (uint32_t)-1, 1, 0,
    };
    static const uint32_t floats[] = {
        ODSY_GL_VERTEX_3F, 0xbf800000, 0xbf800000, 0,
        ODSY_GL_VERTEX_3F, 0x3f800000, 0xbf800000, 0,
        ODSY_GL_VERTEX_3F, 0xbf800000, 0x3f800000, 0,
    };
    uint32_t want[4], got[4];
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_positional_start(&log_path);

    lit_lower_left(qts, floats, 4, want);
    g_assert_cmphex(want[0], !=, want[1]);
    lit_lower_left(qts, ints, 4, got);
    for (unsigned i = 0; i < ARRAY_SIZE(got); i++) {
        g_assert_cmphex(got[i], ==, want[i]);
    }

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * The same for the 2D integer word GL triangles use: glVertex2i's triangle
 * under the positional light matches the glVertex2f one pixel for pixel.
 */
static void test_command_fifo_gl_vertex_2i_lit(void)
{
    static const uint32_t ints[] = {
        ODSY_CMD_GL_VERTEX_2I, (uint32_t)-1, (uint32_t)-1,
        ODSY_CMD_GL_VERTEX_2I, 1, (uint32_t)-1,
        ODSY_CMD_GL_VERTEX_2I, (uint32_t)-1, 1,
    };
    static const uint32_t floats[] = {
        ODSY_GL_VERTEX_2F, 0xbf800000, 0xbf800000,
        ODSY_GL_VERTEX_2F, 0x3f800000, 0xbf800000,
        ODSY_GL_VERTEX_2F, 0xbf800000, 0x3f800000,
    };
    uint32_t want[4], got[4];
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_positional_start(&log_path);

    lit_lower_left(qts, floats, 3, want);
    g_assert_cmphex(want[0], !=, want[1]);
    lit_lower_left(qts, ints, 3, got);
    for (unsigned i = 0; i < ARRAY_SIZE(got); i++) {
        g_assert_cmphex(got[i], ==, want[i]);
    }

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * Lighting takes its eye position from the model-view (slot 0x24), which no
 * longer moves the geometry. The positional setup above under a model-view
 * translating by (1, 1, 2): the triangle stays where the combined matrix
 * puts it, and vertex 0, now at eye (0.25, 0.25, 2) nearer the light at
 * (1, 2, 3), lights (0.979, 0.475, 0.267) instead of (0.918, 0.445,
 * 0.252), that is (250, 121, 68) rather than (234, 113, 64). SPEC-DERIVED:
 * OpenGL 1.2.1 equation 2.5 on light2.c's parameters with the same change,
 * evaluated offline, not from a capture. The pixel sampled lies one row
 * inside vertex 0, so each channel is allowed one step of interpolation.
 */
static void assert_rgb_near(uint32_t pixel, uint32_t r, uint32_t g,
                            uint32_t b)
{
    g_assert_cmpint(abs((int)(pixel >> 24) - (int)r), <=, 1);
    g_assert_cmpint(abs((int)((pixel >> 16) & 0xff) - (int)g), <=, 1);
    g_assert_cmpint(abs((int)((pixel >> 8) & 0xff) - (int)b), <=, 1);
}

static void test_command_fifo_gl_lighting_uses_modelview(void)
{
    static const uint32_t translate[] = {
        0x34008024, 0x9080bdc8,
        0x3f800000, 0, 0, 0x3f800000,
        0, 0x3f800000, 0, 0x3f800000,
        0, 0, 0x3f800000, 0x40000000,
        0, 0, 0, 0x3f800000,
    };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_user_words(qts, odsy_light2_data, ARRAY_SIZE(odsy_light2_data));
    push_user_words(qts, odsy_light2_light1_wide,
                     ARRAY_SIZE(odsy_light2_light1_wide));
    push_constants(qts, odsy_light2_list, ARRAY_SIZE(odsy_light2_list));
    push_program(qts, 1, 0x290, 0x4c5, 0x4c5);
    push_user_words(qts, translate, ARRAY_SIZE(translate));
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    assert_rgb_near(odsy_test_pixel(qts, 272, 367), 250, 121, 68);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * The once-only stage reports: an assembled lit list with the given
 * routines lights light.c's triangle as usual and reports its stage exactly
 * once, whatever the program is set again.
 */
static void check_lit_stage(const uint32_t *list, unsigned n,
                            const char *report)
{
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, n);
    push_program(qts, 1, list[0], 0x4c5, 0x4c5);
    push_program(qts, 1, list[0], 0x4c5, 0x4c5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);

    log = lit_finish(qts, log_path);
    g_assert_cmpuint(count_substr(log, report), ==, 1);
    g_assert_null(strstr(log, "not recognised"));
}

/* Two-sided: the 0x3e5 and 0x415 finishes and the 0x49f end. */
static void test_command_fifo_gl_program_two_sided(void)
{
    static const uint32_t list[] = {
        0x290, 0x2b6, 0x2cf, 0x3e5, 0x415, 0x49f, 0x203, 0x25e, 0x4c5,
    };

    check_lit_stage(list, ARRAY_SIZE(list), "two-sided lighting");
}

/* Texture coordinates: the 0x91 fragment. */
static void test_command_fifo_gl_program_texgen(void)
{
    static const uint32_t list[] = {
        0x290, 0x091, 0x2b6, 0x2cf, 0x3cf, 0x3f5, 0x49a, 0x203, 0x25e, 0x4c5,
    };

    check_lit_stage(list, ARRAY_SIZE(list), "texture-coordinate generation");
}

/* The texture-mode finish: 0x3da and 0x40b. */
static void test_command_fifo_gl_program_texture_finish(void)
{
    static const uint32_t list[] = {
        0x290, 0x2b6, 0x2cf, 0x3da, 0x40b, 0x49a, 0x203, 0x25e, 0x4c5,
    };

    check_lit_stage(list, ARRAY_SIZE(list), "texture-mode lighting finish");
}

/*
 * The assembled unlit list, head 0x4ae then 0x4c0: drawn in the current
 * colour, and not reported, since unlit is its correct rendering.
 */
static void test_command_fifo_gl_program_list_unlit(void)
{
    static const uint32_t list[] = { 0x4ae, 0x4c0, 0x203, 0x25e, 0x4c5 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 1, 0x4ae, 0x4c5, 0x4c5);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/* The recognised unlit record reports unsupported clip planes once. */
static void test_command_fifo_gl_program_record_clip_head(void)
{
    static const uint32_t list[] = {
        0x0f3, 0x10c, 0x000, 0x00c, 0x49a, 0x203, 0x4c5,
    };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 1, 0x0f3, 0x100, 0x10c);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_cmpuint(count_substr(log, "clip planes in geometry program"),
                     ==, 1);
    g_assert_null(strstr(log, "not recognised"));
}

/*
 * A program the model does not recognise draws unlit and is reported once
 * with its entries; the same program again is not reported again.
 */
static void test_command_fifo_gl_program_unknown(void)
{
    static const uint32_t list[] = { 0x000, 0x4c5 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x111, 0x222, 0x333);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);
    push_program(qts, 0, 0x111, 0x222, 0x333);

    log = lit_finish(qts, log_path);
    g_assert_cmpuint(count_substr(log, "geometry program 0x111 0x222 0x333"),
                     ==, 1);
}

/*
 * The context save. At a context switch the board writes the outgoing
 * context's current colour and index into the hardware shadow page named by
 * 0x4004/0x400c, before the completion word, and the kernel replays each
 * saved word from the page at the next switch-in with the operand count its
 * bits 13:10 give. The model saves the last index and colour command words
 * with their raw operands, an approximation of the unknown physical encoding.
 * The page starts as the kernel's template -- its words 0xce80c406 and
 * 0xc580cc0d, zero colour and index -- except that the pipe state is
 * non-idle, so an idle reading proves the board wrote it.
 */
#define ODSY_SHADOW_PAGE 0xb70000
#define ODSY_HW_PIPE_STATE 0x80
#define ODSY_HW_INDEX_WORD 0x240
#define ODSY_HW_COLOUR_WORD 0x250
#define ODSY_HW_INDEX_OPERANDS 0x800
#define ODSY_HW_COLOUR_OPERANDS 0x840
#define ODSY_GL_COLOR_INDEX_USER 0xc580c406
#define ODSY_GL_REPLAY_INDEX 0xce80c406
/* Three signed integers, decoded as ((float)v * 2 + 1) / (2^32 - 2304). */
#define ODSY_GL_COLOR_3I 0xc580cc0d
#define ODSY_I32_ONE 0x7fffffff
/* Switch to context 1: (7 << 40) | (1 << 16) | valid. */
#define ODSY_CTX_SWITCH_TO_1 UINT64_C(0x0000070000010001)

static QTestState *shadow_start(const char *extra)
{
    QTestState *qts = board_start(extra);
    const uint64_t p = ODSY_SHADOW_PAGE;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, (uint32_t)p);
    /*
     * Not the template's idle 0xf: a primitive type, so a later idle reading
     * is the board's own write.
     */
    qtest_writel(qts, p + ODSY_HW_PIPE_STATE, 0x4);
    qtest_writel(qts, p + ODSY_HW_INDEX_WORD, ODSY_GL_REPLAY_INDEX);
    qtest_writel(qts, p + ODSY_HW_COLOUR_WORD, ODSY_GL_COLOR_3I);
    probe_window(qts);
    return qts;
}

/* The probe's triangle's vertices without a colour word of its own. */
static void push_uncoloured_triangle(QTestState *qts)
{
    static const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_VERTEX_2F, 0xbf400000, 0xbf400000,
        ODSY_GL_VERTEX_2F, 0x3f400000, 0xbf200000,
        ODSY_GL_VERTEX_2F, 0xbe000000, 0x3f3c0000,
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static const uint32_t back_material_commands[5] = {
    0xca80d035, 0xcb80d036, 0xcc80d037, 0xcd80d038, 0xd080c439,
};

static const uint32_t front_material_commands[5] = {
    0xc680d02f, 0xc780d030, 0xc880d031, 0xc980d032, 0xd080c433,
};

/* Original raw sentinels, including signed zero, NaNs and infinities. */
static const uint32_t back_material_values[17] = {
    0x80000000, 0x7fc12345, 0x7f800000, 0xff800000,
    0x3f123456, 0x00000001, 0x00800000, 0x7f7fffff,
    0xbf123456, 0x01234567, 0x89abcdef, 0x00100000,
    0x3f400000, 0x3f800000, 0x40000000, 0x40400000,
    0x7fc23456,
};

static const uint32_t material_defaults[17] = {
    0x3e4ccccd, 0x3e4ccccd, 0x3e4ccccd, 0x3f800000,
    0x3f4ccccd, 0x3f4ccccd, 0x3f4ccccd, 0x3f800000,
    0, 0, 0, 0x3f800000,
    0, 0, 0, 0x3f800000,
    0,
};

static void push_material(QTestState *qts, const uint32_t commands[5],
                          const uint32_t values[17])
{
    unsigned i, j;

    for (i = 0; i < 5; i++) {
        unsigned n = i < 4 ? 4 : 1;

        cmd_push64(qts, commands[i], values[4 * i]);
        for (j = 1; j < n; j++) {
            cmd_push(qts, values[4 * i + j]);
        }
    }
}

static void material_snapshot(QTestState *qts, unsigned context)
{
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, ODSY_SHADOW_PAGE);
    qtest_memset(qts, ODSY_SHADOW_PAGE, 0xa5, 4096);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 UINT64_C(0x0000070000000001) | ((uint64_t)context << 16));
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
}

static void assert_material(QTestState *qts, unsigned attribute,
                            const uint32_t commands[5],
                            const uint32_t values[17])
{
    unsigned i, j;

    for (i = 0; i < 5; i++) {
        uint64_t word = ODSY_SHADOW_PAGE + 0x100 + 16 * (attribute + i);
        uint64_t ops = ODSY_SHADOW_PAGE + 0x300 + 64 * (attribute + i);
        unsigned n = i < 4 ? 4 : 1;

        g_assert_cmphex(qtest_readl(qts, word), ==, commands[i]);
        for (j = 0; j < n; j++) {
            g_assert_cmphex(qtest_readl(qts, ops + 4 * j), ==,
                            values[4 * i + j]);
        }
        /* Other ring slots and unused operand slots stay untouched. */
        for (j = 1; j < 4; j++) {
            g_assert_cmphex(qtest_readl(qts, word + 4 * j), ==, 0xa5a5a5a5);
        }
        for (j = n; j < 16; j++) {
            g_assert_cmphex(qtest_readl(qts, ops + 4 * j), ==, 0xa5a5a5a5);
        }
    }
}

static void test_context_save_back_material(void)
{
    uint32_t front[17], other[17];
    QTestState *qts = shadow_start("");
    unsigned i;

    for (i = 0; i < 17; i++) {
        front[i] = 0x3e000000 + i;
        other[i] = 0x3f000000 + i;
    }
    push_material(qts, front_material_commands, front);
    push_material(qts, back_material_commands, other);
    push_material(qts, back_material_commands, back_material_values);
    material_snapshot(qts, 1);
    assert_material(qts, 8, front_material_commands, front);
    assert_material(qts, 14, back_material_commands, back_material_values);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE + 0x1d0), ==,
                    0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE + 0x640), ==,
                    0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE + 0x230), ==,
                    0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE + 0x810), ==,
                    0xa5a5a5a5);

    /* A second client's changes, then replay the saved commands verbatim. */
    push_material(qts, back_material_commands, other);
    for (i = 0; i < 5; i++) {
        uint64_t word = ODSY_SHADOW_PAGE + 0x1e0 + 16 * i;
        uint64_t ops = ODSY_SHADOW_PAGE + 0x680 + 64 * i;
        unsigned j, n = i < 4 ? 4 : 1;

        cmd_push(qts, qtest_readl(qts, word));
        for (j = 0; j < n; j++) {
            cmd_push(qts, qtest_readl(qts, ops + 4 * j));
        }
    }
    material_snapshot(qts, 0x7f);
    assert_material(qts, 8, front_material_commands, front);
    assert_material(qts, 14, back_material_commands, back_material_values);
    board_quit(qts);
}

static void test_context_save_back_material_reset(void)
{
    QTestState *qts = shadow_start("");

    material_snapshot(qts, 0x7f);
    assert_material(qts, 14, back_material_commands, material_defaults);
    push_material(qts, back_material_commands, back_material_values);
    qtest_system_reset(qts);
    material_snapshot(qts, 1);
    assert_material(qts, 14, back_material_commands, material_defaults);
    board_quit(qts);
}

/* Ring 3 slot 0 of the snapshot page: the word and three operands. */
static void assert_normal_ring(QTestState *qts, const uint32_t normal[4])
{
    const uint64_t p = ODSY_SHADOW_PAGE;

    g_assert_cmphex(qtest_readl(qts, p + 0x130), ==, normal[0]);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, p + 0x3c0 + 4 * i), ==,
                        normal[1 + i]);
    }
    g_assert_cmphex(qtest_readl(qts, p + 0x3cc), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, p + 0x134), ==, 0xa5a5a5a5);
}

/*
 * The save writes the normal record's own word and raw operands to ring 3
 * slot 0, not the float word with converted values, and the reset default
 * 0xc180cc19 0 0 1.0 when no normal was set. Replaying that ring entry on
 * the kernel port after another client's normal restores the lit colour.
 */
static void test_context_save_normal_ring(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    static const uint32_t never_set[4] = { 0xc180cc19, 0, 0, 0x3f800000 };
    static const uint32_t ints[4] = {
        0xc180cc18, 0x12345678, 0x80000000, 0x7ffffffe,
    };
    static const uint32_t bytes[4] = { 0xc180cc16, 0x7f, 0xffffff80, 0 };
    static const uint32_t slant[4] = {
        0xc180cc18, 0x4ccccccc, 0, 0x66666666,
    };
    static const uint32_t other[4] = { 0xc180cc19, 0x3f800000, 0, 0 };
    const uint64_t p = ODSY_SHADOW_PAGE;
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);
    uint32_t before;

    material_snapshot(qts, 0x7f);
    assert_normal_ring(qts, never_set);
    push_user_words(qts, ints, 4);
    material_snapshot(qts, 0x7f);
    assert_normal_ring(qts, ints);
    push_user_words(qts, bytes, 4);
    material_snapshot(qts, 0x7f);
    assert_normal_ring(qts, bytes);

    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    before = lit_one_normal(qts, slant);
    material_snapshot(qts, 0x7f);
    assert_normal_ring(qts, slant);
    g_assert_cmphex(lit_one_normal(qts, other), !=, before);

    /* The kernel's replay of ring 3 slot 0, then a draw with no normal. */
    cmd_push(qts, qtest_readl(qts, p + 0x130));
    for (unsigned i = 0; i < 3; i++) {
        cmd_push(qts, qtest_readl(qts, p + 0x3c0 + 4 * i));
    }
    g_assert_cmphex(lit_one_normal(qts, NULL), ==, before);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/* One raw subsection round trip and one absent-subsection default load. */
static void back_material_migration(bool defaults)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = shadow_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-back-material-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    if (!defaults) {
        push_material(from, back_material_commands, back_material_values);
    }
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = shadow_start(" -incoming defer");
    /* Loading an absent subsection must replace live destination values. */
    push_material(to, back_material_commands, back_material_values);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    material_snapshot(to, 0x7f);
    assert_material(to, 14, back_material_commands,
                    defaults ? material_defaults : back_material_values);
    board_quit(from);
    board_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_back_material_migration(void)
{
    back_material_migration(false);
}

static void test_back_material_migration_defaults(void)
{
    back_material_migration(true);
}

/*
 * The retained index and colour records migrate with their raw words: a save
 * on the destination writes the source client's three-int colour and float
 * index, not reset defaults or the destination's own live values. A source
 * still at its defaults replaces the destination's records with them.
 */
static void query_record_migration(bool defaults)
{
    static const uint32_t index[] = { 0xc580c407, 0x40a00000 };
    static const uint32_t colour[] = {
        ODSY_GL_COLOR_3I, ODSY_I32_ONE, 0x80000000, 0x12345678,
    };
    static const uint32_t index_defaults[] = { ODSY_GL_COLOR_INDEX_USER, 1 };
    static const uint32_t colour_defaults[] = {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    static const uint32_t other[] = {
        ODSY_GL_COLOR_INDEX_USER, 9,
        ODSY_GL_COLOR_3F, 0, ODSY_F_ONE, 0,
    };
    const uint32_t *want_index = defaults ? index_defaults : index;
    const uint32_t *want_colour = defaults ? colour_defaults : colour;
    const uint64_t p = ODSY_SHADOW_PAGE;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = shadow_start("");
    QTestState *to;
    unsigned i;
    int fd = g_file_open_tmp("odyssey-query-records-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    if (!defaults) {
        push_user_words(from, index, ARRAY_SIZE(index));
        push_user_words(from, colour, ARRAY_SIZE(colour));
    }
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = shadow_start(" -incoming defer");
    /* Loading must replace the destination's own live records. */
    push_user_words(to, other, ARRAY_SIZE(other));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    material_snapshot(to, 0x7f);

    g_assert_cmphex(qtest_readl(to, p + ODSY_HW_INDEX_WORD), ==,
                    want_index[0]);
    g_assert_cmphex(qtest_readl(to, p + ODSY_HW_INDEX_OPERANDS), ==,
                    want_index[1]);
    g_assert_cmphex(qtest_readl(to, p + 0x110), ==, want_index[0]);
    g_assert_cmphex(qtest_readl(to, p + 0x340), ==, want_index[1]);
    g_assert_cmphex(qtest_readl(to, p + ODSY_HW_COLOUR_WORD), ==,
                    want_colour[0]);
    g_assert_cmphex(qtest_readl(to, p + 0x120), ==, want_colour[0]);
    for (i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(to, p + ODSY_HW_COLOUR_OPERANDS + 4 * i),
                        ==, want_colour[1 + i]);
        g_assert_cmphex(qtest_readl(to, p + 0x380 + 4 * i), ==,
                        want_colour[1 + i]);
    }
    /* Exactly the counted operands; the slots after them stay untouched. */
    g_assert_cmphex(qtest_readl(to, p + ODSY_HW_INDEX_OPERANDS + 4), ==,
                    0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(to, p + ODSY_HW_COLOUR_OPERANDS + 12), ==,
                    0xa5a5a5a5);
    board_quit(from);
    board_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
}

/* Literal public 4F setters; response layout is the existing model encoding. */
static const uint32_t query_texture_4f[2][5] = {
    { 0xc380d025, 0x3f400000, 0x3e800000, 0x3f200000, 0x3f800000 },
    { 0xc380d025, 0x3e000000, 0x3f400000, 0x3f600000, 0x3f800000 },
};

static void texture_current_query(QTestState *qts, const uint32_t *record,
                                  unsigned words, uint32_t count,
                                  uint32_t slot, uint32_t sequence)
{
    const uint64_t page = ODSY_SHADOW_PAGE + 0x2000;
    const uint32_t request[] = { 0x4c0, count, 0x10a5a, slot, sequence };
    uint8_t bytes[28], counter[4], sync[2];

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, page + 0x800);
    qtest_memset(qts, page, 0xa5, 4096);
    push_user_words(qts, request, ARRAY_SIZE(request));
    qtest_memread(qts, page + 0x94c, bytes, sizeof(bytes));
    g_assert_cmphex((uint32_t)ldl_be_p(bytes), ==, 0xa5a5a5a5);
    for (unsigned i = 0; i < words; i++) {
        g_assert_cmphex((uint32_t)ldl_be_p(bytes + 4 * (i + 1)), ==, record[i]);
    }
    for (unsigned i = words + 1; i < sizeof(bytes) / 4; i++) {
        g_assert_cmphex((uint32_t)ldl_be_p(bytes + 4 * i), ==, 0xa5a5a5a5);
    }
    qtest_memread(qts, page + 0x97c, counter, sizeof(counter));
    g_assert_cmphex((uint32_t)ldl_be_p(counter), ==, count);
    qtest_memread(qts, page + 0x800 + 2 * slot, sync, sizeof(sync));
    g_assert_cmphex(lduw_be_p(sync), ==, sequence);
}

static void test_texture_current_query_4f(void)
{
    QTestState *qts = board_start("");

    for (unsigned owner = 0; owner < 2; owner++) {
        push_user_words(qts, query_texture_4f[owner], 5);
        texture_current_query(qts, query_texture_4f[owner], 5, 2,
                              owner ? 8 : 5, 1);
    }
    /* Original raw 4F records replay through the kernel command port. */
    for (unsigned owner = 0; owner < 2; owner++) {
        for (unsigned i = 0; i < 5; i++) {
            cmd_push(qts, query_texture_4f[owner][i]);
        }
        texture_current_query(qts, query_texture_4f[owner], 5, 3,
                              owner ? 8 : 5, 4);
    }
    qtest_system_reset(qts);
    texture_current_query(qts, (uint32_t[]) { 0xc380c821, 0, 0 },
                          3, 2, 5, 1);
    board_quit(qts);
}

static const uint32_t query_texture_1f[2][2] = {
    { 0xc380c41f, 0x3f400000 },
    { 0xc380c41f, 0x3e000000 },
};

static const uint32_t query_texture_3f[2][4] = {
    { 0xc380cc23, 0x3f400000, 0x3e800000, 0x3f200000 },
    { 0xc380cc23, 0x3e000000, 0x3f400000, 0x3f600000 },
};

static void texture_current_query_short(bool three, bool replay)
{
    QTestState *qts = board_start("");
    unsigned words = three ? 4 : 2;

    push_user_words(qts, query_texture_4f[0], 5);
    for (unsigned owner = 0; owner < 2; owner++) {
        const uint32_t *record = three ? query_texture_3f[owner] :
                                         query_texture_1f[owner];

        if (replay) {
            /* Original synthetic records use the kernel command port. */
            for (unsigned i = 0; i < words; i++) {
                cmd_push(qts, record[i]);
            }
        } else {
            push_user_words(qts, record, words);
        }
        texture_current_query(qts, record, words, 2, owner ? 8 : 5, 1);
    }
    qtest_system_reset(qts);
    texture_current_query(qts, (uint32_t[]) { 0xc380c821, 0, 0 },
                          3, 2, 5, 1);
    board_quit(qts);
}

/*
 * An integer texture coordinate is a current-value record like the float
 * ones: the query answers 0xc380c41e and its integer as sent, so the
 * kernel can replay it. Refusing it left the snapshot unanswered, and the
 * guest waited on it for good.
 */
static void test_texture_current_query_1i(void)
{
    static const uint32_t record[] = { 0xc380c41e, 0xfffffffb };
    QTestState *qts = board_start("");

    push_user_words(qts, query_texture_4f[0], 5);
    push_user_words(qts, record, ARRAY_SIZE(record));
    texture_current_query(qts, record, ARRAY_SIZE(record), 2, 5, 1);
    board_quit(qts);
}

static void test_texture_current_query_1f(void)
{
    texture_current_query_short(false, false);
}

static void test_texture_current_query_1f_kernel(void)
{
    texture_current_query_short(false, true);
}

static void test_texture_current_query_3f(void)
{
    texture_current_query_short(true, false);
}

static void test_texture_current_query_3f_kernel(void)
{
    texture_current_query_short(true, true);
}

static void texture_current_query_short_migration(bool three)
{
    const uint32_t *record = three ? query_texture_3f[1] : query_texture_1f[1];
    unsigned words = three ? 4 : 2;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-current-short-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    push_user_words(from, query_texture_4f[0], 5);
    push_user_words(from, record, words);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    push_user_words(to, query_texture_4f[1], 5);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    texture_current_query(to, record, words, 2, 8, 2);
    board_quit(from);
    board_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_current_query_1f_migration(void)
{
    texture_current_query_short_migration(false);
}

static void test_texture_current_query_3f_migration(void)
{
    texture_current_query_short_migration(true);
}

static void texture_current_query_4f_migration(bool defaults)
{
    const uint32_t initial[] = { 0xc380c821, 0, 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-current-4f-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    if (!defaults) {
        push_user_words(from, query_texture_4f[1], 5);
    }
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    push_user_words(to, query_texture_4f[0], 5);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    texture_current_query(to, defaults ? initial : query_texture_4f[1],
                          defaults ? 3 : 5, 2, 8, 2);
    board_quit(from);
    board_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_current_query_4f_migration(void)
{
    texture_current_query_4f_migration(false);
}

static void test_texture_current_query_4f_migration_defaults(void)
{
    texture_current_query_4f_migration(true);
}

static void test_query_record_migration(void)
{
    query_record_migration(false);
}

static void test_query_record_migration_defaults(void)
{
    query_record_migration(true);
}

/*
 * One client sets red and index 5; the switch saves them. Another client
 * sets green. The kernel's replay, read back from the page, restores red,
 * so a triangle drawn with no colour word of its own is red, not green.
 */
static void test_command_fifo_context_save_colour(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");
    unsigned i;

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_GL_COLOR_INDEX_USER);
    user_cmd_push(qts, 5);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);

    g_assert_cmphex(qtest_readl(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
    /* The setters' own words and raw operands, here and in ring slot 0. */
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_WORD), ==,
                    ODSY_GL_COLOR_3F);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS), ==,
                    0x3f800000);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_INDEX_WORD), ==,
                    ODSY_GL_COLOR_INDEX_USER);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_INDEX_OPERANDS), ==, 5);
    g_assert_cmphex(qtest_readl(qts, p + 0x110), ==, ODSY_GL_COLOR_INDEX_USER);
    g_assert_cmphex(qtest_readl(qts, p + 0x340), ==, 5);
    g_assert_cmphex(qtest_readl(qts, p + 0x120), ==, ODSY_GL_COLOR_3F);
    g_assert_cmphex(qtest_readl(qts, p + 0x380), ==, 0x3f800000);
    g_assert_cmphex(qtest_readl(qts, p + 0x384), ==, 0);
    g_assert_cmphex(qtest_readl(qts, p + 0x388), ==, 0);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE) & 0xf, ==, 0xf);

    /* Another client's green, then the kernel's replay from the page. */
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    cmd_push(qts, qtest_readl(qts, p + ODSY_HW_INDEX_WORD));
    cmd_push(qts, qtest_readl(qts, p + ODSY_HW_INDEX_OPERANDS));
    cmd_push(qts, qtest_readl(qts, p + ODSY_HW_COLOUR_WORD));
    for (i = 0; i < 3; i++) {
        cmd_push(qts, qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 4 * i));
    }
    push_uncoloured_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_RED);

    /* Three-int red is saved as its own integers and replays as red. */
    user_cmd_push(qts, ODSY_GL_COLOR_3I);
    user_cmd_push(qts, ODSY_I32_ONE);
    user_cmd_push(qts, 0x80000000);
    user_cmd_push(qts, 0);
    qtest_writel(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_WORD), ==,
                    ODSY_GL_COLOR_3I);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS), ==,
                    ODSY_I32_ONE);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 4), ==,
                    0x80000000);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 8), ==, 0);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    cmd_push(qts, qtest_readl(qts, p + ODSY_HW_COLOUR_WORD));
    for (i = 0; i < 3; i++) {
        cmd_push(qts, qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 4 * i));
    }
    push_uncoloured_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_RED);

    board_quit(qts);
}

/*
 * An idle save writes type 0xf. An open triangle writes the reviewed state
 * word and raw held vertices. A shadow snapshot leaves the assembler open,
 * so the third vertex still completes the triangle without replay words.
 */
static void test_command_fifo_context_save_idle_state(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-ctxsave-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = shadow_start(extra);

    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE) & 0xf, ==, 0xf);

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0xbf200000);
    qtest_writel(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(0x7f) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE), ==,
                    0x0040a0a4);
    g_assert_cmphex(qtest_readl(qts, p + 0x100), ==, ODSY_GL_VERTEX_2F);
    g_assert_cmphex(qtest_readl(qts, p + 0x104), ==, ODSY_GL_VERTEX_2F);
    g_assert_cmphex(qtest_readl(qts, p + 0x300), ==, 0xbf400000);
    g_assert_cmphex(qtest_readl(qts, p + 0x310), ==, 0x3f400000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbe000000);
    user_cmd_push(qts, 0x3f3c0000);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_RED);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "context switch inside primitive 0x4"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_context_restart_triangle(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0xbf200000);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE), ==,
                    0x0040a0a4);

    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x7f00f);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10400);
    cmd_push(qts, 4);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x00101004);
    cmd_push(qts, 0);
    cmd_push(qts, 0x740);
    cmd_push(qts, 0x1e0);
    cmd_push(qts, 0x741);
    cmd_push(qts, 4);
    for (unsigned slot = 0; slot < 2; slot++) {
        cmd_push(qts, qtest_readl(qts, p + 0x100 + 4 * slot));
        cmd_push(qts, qtest_readl(qts, p + 0x300 + 16 * slot));
        cmd_push(qts, qtest_readl(qts, p + 0x304 + 16 * slot));
    }
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbe000000);
    user_cmd_push(qts, 0x3f3c0000);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_RED);
    board_quit(qts);
}

static void test_context_restart_edited_page(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0xbf200000);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    /* Collapse the triangle through guest RAM, after the model saved it. */
    qtest_writel(qts, p + 0x310, 0xbf400000);
    qtest_writel(qts, p + 0x314, 0xbf400000);
    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10400);
    cmd_push(qts, 4);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x00101004);
    cmd_push(qts, 0);
    cmd_push(qts, 0x740);
    cmd_push(qts, 0x1e0);
    cmd_push(qts, 0x741);
    cmd_push(qts, 4);
    for (unsigned slot = 0; slot < 2; slot++) {
        cmd_push(qts, qtest_readl(qts, p + 0x100 + 4 * slot));
        cmd_push(qts, qtest_readl(qts, p + 0x300 + 16 * slot));
        cmd_push(qts, qtest_readl(qts, p + 0x304 + 16 * slot));
    }
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbe000000);
    user_cmd_push(qts, 0x3f3c0000);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, 0);
    board_quit(qts);
}

static void test_context_save_primitive_records(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    const uint32_t colour[4][3] = {
        { 0x3f800000, 0, 0 }, { 0, 0x3f800000, 0 },
        { 0, 0, 0x3f800000 }, { 0x3f800000, 0x3f800000, 0 },
    };
    QTestState *qts = shadow_start("");

    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_QUADS);
    for (unsigned i = 0; i < 4; i++) {
        user_cmd_push(qts, ODSY_GL_COLOR_3F);
        push_user_words(qts, colour[i], 3);
        if (i < 3) {
            user_cmd_push(qts, ODSY_GL_VERTEX_2F);
            user_cmd_push(qts, i << 20);
            user_cmd_push(qts, i << 20);
        }
    }
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE), ==,
                    (3u << 21) | (3u << 14) |
                    (3u << 6) | (3u << 4) | 7u);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t descriptor = qtest_readl(qts, p +
                                          (i == 3 ? 0x84 : 0x8c + 8 * i));

        g_assert_cmphex((descriptor >> 28) & 3, ==, i);
        g_assert_cmphex(qtest_readl(qts, p + 0x120 + 4 * i), ==,
                        ODSY_GL_COLOR_3F);
        for (unsigned j = 0; j < 3; j++) {
            g_assert_cmphex(qtest_readl(qts, p + 0x380 + 16 * i + 4 * j),
                            ==, colour[i][j]);
        }
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
    board_quit(qts);
}

/*
 * Every descriptor an open-primitive save writes must select, for each
 * attribute the IRIX 6.5.30 IP30 kernel replays (1 to 12 and 14 to 18, read
 * from its disassembly), a ring word the kernel accepts: one with operands,
 * or an edge-flag word. Attributes 5 and 6 carry no model record, so their
 * selectors must keep slot 0, where the kernel's template words stay. A
 * triangle strip interrupted after two vertices is perfly's case: selecting
 * the second vertex's or the current slot made the kernel log an illegal
 * restore command for both attributes.
 */
static bool odsy_replay_word_accepted(uint32_t word)
{
    return ((word >> 10) & 0xf) ||
           (word & ~UINT32_C(0x007f0000)) == 0x2f00c02e ||
           (word & ~UINT32_C(0x007f0000)) == 0x3000c02e;
}

static void test_context_save_unrecorded_attributes(void)
{
    static const uint32_t template_word[2] = { 0xc380c41e, 0xc480c426 };
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");
    uint32_t state, n, r;
    uint64_t descriptor[4];
    unsigned count = 0;

    for (unsigned i = 0; i < 2; i++) {
        qtest_writel(qts, p + 0x100 + 16 * (5 + i), template_word[i]);
    }
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    push_user_words(qts, (const uint32_t[]) { ODSY_F_ONE, 0, 0 }, 3);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, 5);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    push_user_words(qts, (const uint32_t[]) { 0xbf400000, 0xbf400000 }, 2);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    push_user_words(qts, (const uint32_t[]) { 0x3f400000, 0xbf200000 }, 2);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    state = qtest_readl(qts, p + ODSY_HW_PIPE_STATE);
    g_assert_cmphex(state & 0xf, ==, 5);
    n = (state >> 4) & 3;
    r = (state >> 6) & 3;
    g_assert_cmpuint(n, ==, 2);
    for (unsigned k = 0; k < n; k++) {
        descriptor[count++] = p + 0x8c + 8 * ((r - n + k + 4) % 4);
    }
    descriptor[count++] = p + 0x84;

    for (unsigned d = 0; d < count; d++) {
        uint32_t word[2] = {
            qtest_readl(qts, descriptor[d]),
            qtest_readl(qts, descriptor[d] + 4),
        };

        for (unsigned a = 1; a <= 18; a++) {
            unsigned slot;
            uint32_t ring;

            if (a == 13) {
                continue;
            }
            slot = a <= 16 ? (word[0] >> (2 * (16 - a))) & 3 :
                             (word[1] >> (2 * (32 - a))) & 3;
            ring = qtest_readl(qts, p + 0x100 + 16 * a + 4 * slot);
            if (!odsy_replay_word_accepted(ring)) {
                g_test_message("descriptor 0x%" PRIx64 " attribute %u "
                               "slot %u word 0x%08x", descriptor[d] - p, a,
                               slot, ring);
            }
            g_assert_true(odsy_replay_word_accepted(ring));
            if (a == 5 || a == 6) {
                g_assert_cmphex(ring, ==, template_word[a - 5]);
            }
        }
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
    board_quit(qts);
}

static void test_context_save_zero_vertex_primitive(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");

    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, 5);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE), ==, 5);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE), ==, 0xf);
    board_quit(qts);
}

/*
 * Bit 11 of an open triangle strip's pipe state says its next triangle is
 * odd: set after 3 and 5 vertices, clear after 2 and 4. The GL library's
 * near-plane workaround reads it to replay the held vertices in order.
 */
static void test_context_save_strip_parity(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;

    for (unsigned n = 2; n <= 5; n++) {
        QTestState *qts = shadow_start("");

        user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
        user_cmd_push(qts, 5);
        for (unsigned i = 0; i < n; i++) {
            user_cmd_push(qts, ODSY_GL_VERTEX_2F);
            user_cmd_push(qts, (i & 1) ? 0x3f000000 : 0xbf000000);
            user_cmd_push(qts, i << 22);
        }
        qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                     ODSY_CTX_SWITCH_TO_1);
        g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE) &
                        ((1u << 11) | 0xf), ==,
                        (n & 1 ? 1u << 11 : 0) | 5);
        board_quit(qts);
    }
}

static void test_context_restart_one_vertex_strip(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");
    uint32_t first;

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_MODE_CULL_FACE | ODSY_MODE_CULL_BACK);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, ODSY_F_ONE);
    user_cmd_push(qts, ODSY_F_ONE);
    user_cmd_push(qts, ODSY_F_ZERO);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, 5);
    user_cmd_push(qts, ODSY_GL_VERTEX_3F);
    push_user_words(qts, odsy_near_yellow, 3);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    first = qtest_readl(qts, p + ODSY_HW_PIPE_STATE);
    g_assert_cmphex(first, ==,
                    (1u << 21) | (1u << 14) | (1u << 12) |
                    (1u << 6) | (1u << 4) | 5u);
    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10400);
    cmd_push(qts, 5);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x00081005);
    cmd_push(qts, 0);
    cmd_push(qts, 0x740);
    cmd_push(qts, 0x1e0);
    cmd_push(qts, 0x741);
    cmd_push(qts, 0);
    cmd_push(qts, qtest_readl(qts, p + 0x100));
    for (unsigned i = 0; i < 3; i++) {
        cmd_push(qts, qtest_readl(qts, p + 0x300 + 4 * i));
    }
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, p + 0x1000);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + 0x1000 + ODSY_HW_PIPE_STATE), ==,
                    first);
    for (unsigned vertex = 1; vertex < 3; vertex++) {
        user_cmd_push(qts, ODSY_GL_VERTEX_3F);
        push_user_words(qts, &odsy_near_yellow[3 * vertex], 3);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

static void test_context_restart_quad_strip_after_wrap(void)
{
    static const uint32_t vertex[6][2] = {
        { 0xbf400000, 0xbf400000 }, { 0xbf400000, 0x3f400000 },
        { 0, 0xbf400000 }, { 0, 0x3f400000 },
        { 0x3f400000, 0xbf400000 }, { 0x3f400000, 0x3f400000 },
    };
    static const uint32_t colour[6][3] = {
        { ODSY_F_ONE, 0, 0 }, { 0, ODSY_F_ONE, 0 },
        { 0, 0, ODSY_F_ONE }, { ODSY_F_ONE, ODSY_F_ONE, 0 },
        { ODSY_F_ONE, 0, ODSY_F_ONE }, { 0, ODSY_F_ONE, ODSY_F_ONE },
    };
    static const unsigned replay_slot[3] = { 2, 3, 0 };
    static const uint32_t pixel[][2] = {
        { 326, 280 }, { 340, 300 }, { 350, 340 }, { 360, 360 },
    };
    uint32_t uninterrupted[ARRAY_SIZE(pixel)];
    QTestState *qts = board_start("");

    probe_window(qts);
    push_user_words(qts, (const uint32_t[]) {
        ODSY_CMD_GL_BEGIN, ODSY_GL_QUAD_STRIP,
    }, 2);
    for (unsigned i = 0; i < 6; i++) {
        user_cmd_push(qts, ODSY_GL_COLOR_3F);
        push_user_words(qts, colour[i], ARRAY_SIZE(colour[i]));
        user_cmd_push(qts, ODSY_GL_VERTEX_2F);
        push_user_words(qts, vertex[i], ARRAY_SIZE(vertex[i]));
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
    for (unsigned i = 0; i < ARRAY_SIZE(pixel); i++) {
        uninterrupted[i] = odsy_test_pixel(qts, pixel[i][0], pixel[i][1]);
        g_assert_cmphex(uninterrupted[i], !=, 0);
    }
    board_quit(qts);

    qts = shadow_start("");
    probe_window(qts);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_QUAD_STRIP);
    for (unsigned i = 0; i < 5; i++) {
        user_cmd_push(qts, ODSY_GL_COLOR_3F);
        push_user_words(qts, colour[i], ARRAY_SIZE(colour[i]));
        user_cmd_push(qts, ODSY_GL_VERTEX_2F);
        push_user_words(qts, vertex[i], ARRAY_SIZE(vertex[i]));
    }
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE + ODSY_HW_PIPE_STATE),
                    ==, 0x00a06078);
    g_assert_cmphex((qtest_readl(qts, ODSY_SHADOW_PAGE + 0x8c + 8 * 2) >> 28)
                    & 3, ==, 0);
    g_assert_cmphex((qtest_readl(qts, ODSY_SHADOW_PAGE + 0x8c + 8 * 3) >> 28)
                    & 3, ==, 1);
    g_assert_cmphex((qtest_readl(qts, ODSY_SHADOW_PAGE + 0x8c) >> 28) & 3,
                    ==, 2);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t ring = ODSY_SHADOW_PAGE + 0x120 + 4 * i;
        uint64_t operands = ODSY_SHADOW_PAGE + 0x380 + 16 * i;

        g_assert_cmphex(qtest_readl(qts, ring), ==, ODSY_GL_COLOR_3F);
        for (unsigned j = 0; j < 3; j++) {
            g_assert_cmphex(qtest_readl(qts, operands + 4 * j), ==,
                            colour[i + 2][j]);
        }
    }

    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10400);
    cmd_push(qts, ODSY_GL_QUAD_STRIP);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x00180008);
    cmd_push(qts, 0);
    cmd_push(qts, 0x740);
    cmd_push(qts, 0x1e0);
    cmd_push(qts, 0x741);
    cmd_push(qts, 0x0040a000);
    for (unsigned i = 0; i < ARRAY_SIZE(replay_slot); i++) {
        unsigned slot = replay_slot[i];
        unsigned descriptor_slot = (qtest_readl(qts, ODSY_SHADOW_PAGE +
                                                  0x8c + 8 * slot) >> 28) & 3;
        uint64_t vertex_word = ODSY_SHADOW_PAGE + 0x100 + 4 * slot;
        uint64_t vertex_operands = ODSY_SHADOW_PAGE + 0x300 + 16 * slot;
        uint64_t colour_record = ODSY_SHADOW_PAGE + 0x120 +
                                 4 * descriptor_slot;
        uint64_t colour_operands = ODSY_SHADOW_PAGE + 0x380 +
                                   16 * descriptor_slot;

        cmd_push(qts, qtest_readl(qts, colour_record));
        for (unsigned j = 0; j < 3; j++) {
            cmd_push(qts, qtest_readl(qts, colour_operands + 4 * j));
        }
        cmd_push(qts, qtest_readl(qts, vertex_word));
        for (unsigned j = 0; j < 2; j++) {
            cmd_push(qts, qtest_readl(qts, vertex_operands + 4 * j));
        }
    }
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    push_user_words(qts, colour[5], ARRAY_SIZE(colour[5]));
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    push_user_words(qts, vertex[5], ARRAY_SIZE(vertex[5]));
    user_cmd_push(qts, ODSY_CMD_GL_END);
    for (unsigned i = 0; i < ARRAY_SIZE(pixel); i++) {
        g_assert_cmphex(odsy_test_pixel(qts, pixel[i][0], pixel[i][1]), ==,
                        uninterrupted[i]);
    }
    board_quit(qts);
}

static void test_context_switch_integer_quad(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");

    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    cmd_push(qts, ODSY_CMD_GL_COLOR_3UB);
    cmd_push(qts, 0x11);
    cmd_push(qts, 0x22);
    cmd_push(qts, 0x33);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 10);
    cmd_push(qts, 20);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 40);
    cmd_push(qts, 20);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_PIPE_STATE), ==, 0xf);
    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x7f00f);
    cmd_push(qts, 0);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 40);
    cmd_push(qts, 60);
    cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
    cmd_push(qts, 10);
    cmd_push(qts, 60);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 266, 276), ==, 0x112233ff);
    board_quit(qts);
}

static void test_context_switch_polygon_assembly(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("odyssey-polygon-switch-XXXXXX", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = shadow_start(extra);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, 9);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0xbf200000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbe000000);
    user_cmd_push(qts, 0x3f3c0000);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                                ODSY_HW_PIPE_STATE), ==, 0xf);
    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x7f00f);
    cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_RED);
    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_cmpuint(count_substr(log, "context switch inside primitive 0x9"),
                     ==, 1);
}

/*
 * The kernel's context swap in the order the IRIX 6.5.30 kernel sends it
 * (disassembly of its swap): the switch store, the drop of the outgoing
 * primitive (0x10444 with operand 0, 0x10850 with 0x7f00f), then the raster
 * restore, here the owner's polygon mode FILL and line and point size 1. The
 * model holds an open GL_POLYGON across the drop
 * (context-switch-polygon-assembly), so the restored registers arrive while
 * it is still open. They are taken as the board's registers, not reported
 * as GL calls inside glBegin, and the polygon still fills when it ends.
 */
static void test_context_switch_polygon_raster_restore(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("odyssey-polygon-restore-XXXXXX", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors -D %s", quoted);
    qts = shadow_start(extra);
    user_cmd_push(qts, ODSY_REG_POLYGON_MODE);
    user_cmd_push(qts, 0xa);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, 9);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, 0xbf400000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0xbf200000);
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0xbe000000);
    user_cmd_push(qts, 0x3f3c0000);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    cmd_push(qts, 0x10444);
    cmd_push(qts, 0);
    cmd_push(qts, 0x10850);
    cmd_push(qts, 0x7f00f);
    cmd_push(qts, ODSY_REG_POLYGON_MODE);
    cmd_push(qts, 0xa);
    cmd_push(qts, 0x0001084a);
    cmd_push(qts, 128);
    cmd_push(qts, 128);
    user_cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_RED);
    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_null(strstr(log, "inside glBegin"));
}

static void test_context_restart_unsupported_words(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("odyssey-restart-words-XXXXXX", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = shadow_start(extra);
    for (unsigned i = 0; i < 2; i++) {
        cmd_push(qts, 0x740);
        cmd_push(qts, 0x1ed);
        cmd_push(qts, 0x741);
        cmd_push(qts, 0);
        cmd_push(qts, 0x10850);
        cmd_push(qts, 0xa);
        cmd_push(qts, 0);
    }
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                                ODSY_HW_PIPE_STATE), ==, 0xf);
    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_cmpuint(count_substr(log, "word 0x00000740"), ==, 1);
    g_assert_cmpuint(count_substr(log, "word 0x00000741"), ==, 1);
    g_assert_cmpuint(count_substr(log, "word 0x00010850"), ==, 1);
}

/*
 * The window restore. The GL library overrides the window on the user port
 * to render into an off-screen object, then sends the bare word 0x90008116
 * before its next screen draw. The override is real: a clear after it
 * lands at the screen origin. After 0x90008116 the kernel's committed
 * window is back and the same clear lands in the probe window (the restore
 * is inferred).
 */
#define ODSY_CMD_WINDOW_RESTORE 0x90008116

static void user_window_override(QTestState *qts)
{
    static const uint32_t words[] = {
        ODSY_CMD_WINDOW_ORIGIN, 0, 0,
        ODSY_CMD_WINDOW_CLIP, 0, 0x00ffffff, 0, 0, 0, 0, 0, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void test_command_fifo_gl_window_restore(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    user_window_override(qts);
    push_captured_clear(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 300), ==, 0);
    board_quit(qts);

    qts = board_start("");
    probe_window(qts);
    user_window_override(qts);
    user_cmd_push(qts, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 256, 256), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 383, 383), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 384, 300), ==, 0);
    board_quit(qts);
}

/*
 * The kernel's off-screen tail. For a context drawing off-screen, the
 * kernel's window push ends with 0x841 (0, 0), the full-range 0x204b and a
 * zero 0x840 instead of 0x90008116. The library's later restore must bring
 * back the window pushed just before that tail, so its clear lands in the
 * probe window, not at the screen origin. A kernel window that really is at
 * (0, 0), with ordinary rectangles, is still committed: the same restore
 * then puts the clear at the screen origin.
 */
static void kernel_offscreen_tail(QTestState *qts)
{
    window_origin(qts, 0, 0);
    cmd_push(qts, ODSY_CMD_WINDOW_CLIP);
    cmd_push(qts, 0);
    cmd_push(qts, 0x00ffffff);
    for (unsigned i = 0; i < 6; i++) {
        cmd_push(qts, 0);
    }
    cmd_push(qts, ODSY_CMD_DID_COMPARE);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
}

static void test_command_fifo_gl_window_offscreen_tail(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    kernel_offscreen_tail(qts);
    user_cmd_push(qts, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 256, 256), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 383, 383), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 384, 300), ==, 0);
    board_quit(qts);

    qts = board_start("");
    probe_window(qts);
    window_origin(qts, 0, 0);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 127, 127 } }, 1);
    user_window_override(qts);
    user_cmd_push(qts, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 256, 256), ==, 0);
    board_quit(qts);
}

/*
 * The tail's pending origin across migration. Saved between the tail's
 * 0x841 and its 0x204b, the destination still recognises the 0x204b as the
 * tail and the restore brings back the probe window. Loaded from a source
 * with nothing pending, a destination that had a 0x841 pending of its own
 * drops it: the following off-screen 0x204b commits nothing old, and the
 * restore keeps the source's window at (0, 0).
 */
static void kernel_offscreen_clip(QTestState *qts)
{
    cmd_push(qts, ODSY_CMD_WINDOW_CLIP);
    cmd_push(qts, 0);
    cmd_push(qts, 0x00ffffff);
    for (unsigned i = 0; i < 6; i++) {
        cmd_push(qts, 0);
    }
}

static void window_tail_migration(bool pending)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-window-tail-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    probe_window(from);
    window_origin(from, 0, 0);
    if (!pending) {
        /* A real window at (0, 0): the compare pair ends the pending 0x841. */
        cmd_push(from, ODSY_CMD_DID_COMPARE);
        cmd_push(from, 0);
        cmd_push(from, 0);
        window_clip(from, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    }
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    if (!pending) {
        /* Live destination state the absent subsection must replace. */
        window_origin(to, 500, 500);
        window_origin(to, 0, 0);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    kernel_offscreen_clip(to);
    user_cmd_push(to, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(to);
    if (pending) {
        g_assert_cmphex(odsy_test_pixel(to, 256, 256), ==, ODSY_PLANE_BLUE);
        g_assert_cmphex(odsy_test_pixel(to, 10, 10), ==, 0);
    } else {
        g_assert_cmphex(odsy_test_pixel(to, 10, 10), ==, ODSY_PLANE_BLUE);
        g_assert_cmphex(odsy_test_pixel(to, 510, 510), ==, 0);
    }
    board_quit(from);
    board_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_command_fifo_gl_window_tail_migration(void)
{
    window_tail_migration(true);
}

static void test_command_fifo_gl_window_tail_migration_absent(void)
{
    window_tail_migration(false);
}

/*
 * Texture uploads. The GL library overrides the window to (0, 0), selects
 * the texture level's board-memory object with 0x1017 (base in 16-byte
 * units, row length in 16-texel units, an 0x08... format word) and sends a
 * BEGIN-0xa image whose header's low byte gives the texel layout. The
 * texels land in the object, not on the screen, and 0x90008116 then
 * restores the kernel's window. The 0x1453 and 0x081d words are opaque to
 * the model; both are among the words each textured draw or upload also
 * pushes, and their contents are unknown.
 */
static void upload_begin_xy(QTestState *qts, uint32_t base, uint32_t wunits,
                            uint32_t fmt, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{
    const uint32_t words[] = {
        0x00011453, 1, 0, 0, 0, 0,
        ODSY_REG_GL_MODE, 0,
        0x00010405, 0x00002400,
        ODSY_CMD_WINDOW_ORIGIN, 0, 0,
        ODSY_CMD_WINDOW_CLIP, 0, 0x00ffffff, 0, 0, 0, 0, 0, 0,
        ODSY_CMD_DEST_SELECT, base, wunits, fmt, 0,
        ODSY_REG_WRITE_MASK, 0x01ffffff,
        0x0001081d, (x << 12) | (x + w), (y << 12) | (y + h),
        ODSY_CMD_IMAGE_STATE,
        ODSY_CMD_GL_BEGIN, ODSY_CMD_IMAGE_PRIMITIVE,
        ODSY_CMD_IMAGE_BOX, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
        coord_bits(x), coord_bits(y), 0,
        coord_bits(x + w), coord_bits(y + h), 0,
        ODSY_CMD_GL_VERTEX_2I, 0, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void upload_begin_at(QTestState *qts, uint32_t base, uint32_t wunits,
                            uint32_t fmt, uint32_t x, uint32_t w, uint32_t h)
{
    upload_begin_xy(qts, base, wunits, fmt, x, 0, w, h);
}

static void upload_begin(QTestState *qts, uint32_t base, uint32_t wunits,
                         uint32_t fmt, uint32_t w, uint32_t h)
{
    upload_begin_at(qts, base, wunits, fmt, 0, w, h);
}

/* The upload's end and the screen select a draw after it sends. */
static void upload_end(QTestState *qts)
{
    static const uint32_t words[] = {
        ODSY_CMD_GL_END,
        ODSY_CMD_WINDOW_RESTORE,
        ODSY_CMD_DEST_SELECT, 0x2000, 0x50, 0x20006850, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

/* One row-group of an 8-texel row: its header, tail and burst. */
static void upload_row(QTestState *qts, uint32_t code, const uint32_t *w,
                       unsigned n)
{
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    user_cmd_push(qts, 0x0f810000 | code);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    user_cmd_push(qts, ODSY_IMAGE_BURST(n, code == 0x42 ? ODSY_IMAGE_FMT_CI :
                                        ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, w, n);
}

static void upload_first_header(QTestState *qts, uint32_t code)
{
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    user_cmd_push(qts, 0x0f001000 | code);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
}

/* The screen corner and the window stay as they were: nothing drew there. */
static void assert_screen_untouched(QTestState *qts)
{
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 7, 7), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 256, 256), ==, 0);
}

/*
 * The 8x8 GL_LUMINANCE texture of the probe's phase H, texel (x, y) =
 * 4 (x + 8 y), one byte a texel in two-word rows: the object at 0x16b000
 * holds each texel as its byte in every colour channel.
 */
static void test_command_fifo_gl_texture_luminance(void)
{
    QTestState *qts = board_start("");
    uint32_t x, y;

    probe_window(qts);
    upload_begin(qts, 0x16b000, 2, 0x08000020, 8, 8);
    upload_first_header(qts, 0x42);
    for (y = 0; y < 8; y++) {
        uint32_t row[2];

        for (x = 0; x < 2; x++) {
            uint32_t l = 4 * (4 * x + 8 * y);

            row[x] = l << 24 | (l + 4) << 16 | (l + 8) << 8 | (l + 12);
        }
        upload_row(qts, 0x42, row, 2);
    }
    upload_end(qts);

    for (y = 0; y < 8; y += 7) {
        for (x = 0; x < 8; x += 3) {
            uint32_t l = 4 * (x + 8 * y);

            g_assert_cmphex(odsy_test_object_texel(qts, 0x16b000, x, y), ==,
                            l << 24 | l << 16 | l << 8 | 0xff);
        }
    }
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16b000, 3, 3), ==,
                    0x6c6c6cff);
    assert_screen_untouched(qts);
    board_quit(qts);
}

/*
 * The 8x8 GL_RGB texture of phase G, texel (x, y) = (32 x, 32 y, 128), three
 * packed bytes a texel in six-word rows, into the object at 0x16ad00 (two
 * width units, 32 texels a row) with alpha 255.
 */
static void test_command_fifo_gl_texture_rgb(void)
{
    QTestState *qts = board_start("");
    uint32_t x, y;

    probe_window(qts);
    upload_begin(qts, 0x16ad00, 2, 0x080024d0, 8, 8);
    upload_first_header(qts, 0x49);
    for (y = 0; y < 8; y++) {
        uint8_t bytes[24];
        uint32_t row[6];

        for (x = 0; x < 8; x++) {
            bytes[3 * x] = 32 * x;
            bytes[3 * x + 1] = 32 * y;
            bytes[3 * x + 2] = 128;
        }
        for (x = 0; x < 6; x++) {
            row[x] = ldl_be_p(&bytes[4 * x]);
        }
        upload_row(qts, 0x49, row, 6);
    }
    upload_end(qts);

    g_assert_cmphex(odsy_test_object_texel(qts, 0x16ad00, 0, 0), ==,
                    0x000080ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16ad00, 4, 3), ==,
                    0x806080ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16ad00, 7, 7), ==,
                    0xe0e080ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16ad00, 8, 0), ==, 0);
    assert_screen_untouched(qts);
    board_quit(qts);
}

/* tex.c's 64x64 RGBA texel: (4 x, 4 y, the 8x8-cell checker, 255). */
static uint32_t tex_rgba(uint32_t x, uint32_t y)
{
    uint32_t b = ((x / 8 + y / 8) & 1) ? 0xff : 0;

    return (4 * x) << 24 | (4 * y) << 16 | b << 8 | 0xff;
}

/*
 * The 64x64 GL_RGBA texture of phase A: one 0xRRGGBBAA word a texel, 256
 * sixteen-word bursts under the tag the X server's 0x00RRGGBB icons use.
 * The object at 0x16a800 holds the words as sent; the screen corner the
 * model used to paint them on stays clear.
 */
static void test_command_fifo_gl_texture_rgba(void)
{
    QTestState *qts = board_start("");
    uint32_t i, j;

    probe_window(qts);
    upload_begin(qts, 0x16a800, 4, 0x080024d0, 64, 64);
    upload_first_header(qts, 0x4b);
    for (i = 0; i < 64 * 64; i += 16) {
        user_cmd_push(qts, ODSY_IMAGE_BURST(16, ODSY_IMAGE_FMT_RGB));
        for (j = i; j < i + 16; j++) {
            user_cmd_push(qts, tex_rgba(j % 64, j / 64));
        }
    }
    upload_end(qts);

    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 0), ==,
                    0x000000ffu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 8, 0), ==,
                    0x2000ffffu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 32, 31), ==,
                    tex_rgba(32, 31));
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 63, 63), ==,
                    tex_rgba(63, 63));
    assert_screen_untouched(qts);
    board_quit(qts);
}

/*
 * A 256x256 texture goes by DMA: the header caught at the kernel's push
 * names the level-0 object 0x16a800, 0x10 width units, rows of 1024 bytes
 * and source format 0x4b. One entry over two rows of (x, y, x ^ y, 255)
 * lands in the object; the screen is unchanged and, with bit 31 of word 7
 * clear, no DMA-done interrupt is sent.
 */
static void test_dma_texture_object(void)
{
    QTestState *qts = board_start("");
    const uint64_t table = 0xb80000;
    const uint64_t page = 0xb84000;
    const uint32_t hdr[8] = {
        0x0016a800, 0, 0x10, (2 << 16) | 0x100, 0x80013000, 0, 0x400,
        0x3201004b,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(page, 2048) };
    uint8_t rows[2048];
    uint32_t x, y;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    /*
     * An earlier, smaller texture at the same base -- another client's 64x64
     * level, four width units: the DMA header's own row length replaces it.
     */
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x16a800);
    user_cmd_push(qts, 4);
    user_cmd_push(qts, 0x080024d0);
    user_cmd_push(qts, 0);
    for (y = 0; y < 2; y++) {
        for (x = 0; x < 256; x++) {
            stl_be_p(&rows[4 * (256 * y + x)],
                     x << 24 | y << 16 | (x ^ y) << 8 | 0xff);
        }
    }
    qtest_memwrite(qts, page, rows, sizeof(rows));
    dma_image_table(qts, table, hdr, pte, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);

    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 0), ==,
                    0x000000ffu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 129, 0), ==,
                    0x810081ffu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 84, 1), ==,
                    0x540155ffu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 2), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 129, 0), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);
    board_quit(qts);
}

/*
 * buttonfly's label texture: a 256x256 luminance-alpha image by DMA, two
 * bytes a pixel (size index 0) under source format 0x48, with header words
 * 4 and 7 as the guest sent them. Each (L, A) pair lands as one texel word
 * (L, L, L, A). Before, the two-byte size was refused and the object stayed
 * empty, so the label drew as solid blocks.
 */
static void test_dma_texture_luminance_alpha(void)
{
    QTestState *qts = board_start("");
    const uint64_t table = 0xb80000;
    const uint64_t page = 0xb84000;
    const uint32_t hdr[8] = {
        0x0016c800, 0, 0x10, (2 << 16) | 0x100, 0x80001000, 0, 0x200,
        0x32000048,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(page, 1024) };
    uint8_t rows[1024];
    uint32_t x, y;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    for (y = 0; y < 2; y++) {
        for (x = 0; x < 256; x++) {
            rows[2 * (256 * y + x)] = x;
            rows[2 * (256 * y + x) + 1] = x ^ (y ? 0xff : 0x0f);
        }
    }
    qtest_memwrite(qts, page, rows, sizeof(rows));
    dma_image_table(qts, table, hdr, pte, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);

    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c800, 0, 0), ==,
                    0x0000000fu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c800, 0x81, 0), ==,
                    0x8181818eu);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c800, 0xff, 1), ==,
                    0xffffff00u);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c800, 0, 2), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    board_quit(qts);
}

/*
 * One two-texel DMA row of a 12-bit texture source into the object at base:
 * word 7 carries the source code and its size index (bits 18:16).
 */
static void dma_texel12_row(QTestState *qts, uint32_t base, uint32_t code,
                            uint32_t size_index, uint32_t bpp,
                            const uint8_t *bytes)
{
    const uint64_t table = 0xb80000;
    const uint64_t page = 0xb84000;
    const uint32_t hdr[8] = {
        base, 0, 2, (1 << 16) | 2, 0x80015000, 0, 2 * bpp,
        0x3a000000 | size_index << 16 | code,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(page, 2 * bpp) };

    qtest_memwrite(qts, page, bytes, 2 * bpp);
    dma_image_table(qts, table, hdr, pte, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
}

/*
 * The GL library's uploads into a 12-bit texture, from a guest probe that
 * sent distinct component values under each internal format, source format
 * and type. The code names the components in the source's order (0x62 L,
 * 0x68 L A, 0x69 R G B, 0x6b R G B A, 0x6c A B G R); the size index says
 * whether each is a byte (3, 4) or a big-endian 16-bit word (2, 4, 6, 8
 * bytes a pixel). 16-bit components keep their high byte, rounded. Before
 * these codes were read, every one but the byte-sized 0x6b was dropped or
 * laid out as an X server 0x00RRGGBB word.
 */
static void test_dma_texture_12bit_sources(void)
{
    static const struct {
        uint32_t base, code, size_index, bpp;
        uint8_t bytes[16];
        uint32_t texel[2];
    } cases[] = {
        { 0x180000, 0x6c, 2, 8,
          { 0x11, 0x22, 0x55, 0x66, 0x99, 0xaa, 0xdd, 0xee,
            0xff, 0xff, 0x22, 0x02, 0x3e, 0xb4, 0x46, 0xfd },
          { 0xdd995511, 0x473e22ff } },
        { 0x181000, 0x6b, 2, 8,
          { 0x11, 0x22, 0x55, 0x66, 0x99, 0xaa, 0xdd, 0xee,
            0x00, 0x00, 0xff, 0xff, 0x80, 0x00, 0x00, 0x7f },
          { 0x115599dd, 0x00ff8000 } },
        { 0x182000, 0x69, 6, 6,
          { 0x11, 0x22, 0x55, 0x66, 0x99, 0xaa,
            0xdf, 0xdf, 0xa0, 0xa0, 0x00, 0x00 },
          { 0x115599ff, 0xdfa000ff } },
        { 0x183000, 0x69, 7, 3,
          { 0x11, 0x55, 0x99, 0xd4, 0xa0, 0x00 },
          { 0x115599ff, 0xd4a000ff } },
        { 0x184000, 0x68, 1, 4,
          { 0x11, 0x22, 0x55, 0x66, 0xd3, 0xd3, 0x7a, 0x7a },
          { 0x11111155, 0xd3d3d37a } },
        { 0x185000, 0x62, 0, 2,
          { 0xa6, 0x8c, 0x87, 0x0d },
          { 0xa6a6a6ff, 0x878787ff } },
    };
    QTestState *qts = board_start("");

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        dma_texel12_row(qts, cases[i].base, cases[i].code,
                        cases[i].size_index, cases[i].bpp, cases[i].bytes);
        for (unsigned x = 0; x < 2; x++) {
            g_assert_cmphex(odsy_test_object_texel(qts, cases[i].base, x, 0),
                            ==, cases[i].texel[x]);
        }
    }
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    board_quit(qts);
}

/*
 * The same sources as pixel bursts: 0x62 two 16-bit luminances a word and
 * 0x68 a 16-bit luminance-alpha pair a word under tag format 0x10; 0x69 and
 * 0x6c as a stream of 16-bit halves under 0x12, 0x69's three halves a
 * pixel straddling words. Last, distort's row ending: a four-word group
 * carries the row's last two 16-bit ABGR pixels, and the next row's burst
 * follows with no header of its own and must still land.
 */
static void test_burst_texture_12bit_sources(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    upload_begin(qts, 0x180000, 2, 0x080024d0, 4, 1);
    upload_first_header(qts, 0x62);
    user_cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_CI));
    push_user_words(qts, (uint32_t[]) { 0xa68c870d, 0x1122ffff }, 2);
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 0), ==,
                    0xa6a6a6ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 1, 0), ==,
                    0x878787ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 2, 0), ==,
                    0x111111ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 3, 0), ==,
                    0xffffffff);

    upload_begin(qts, 0x181000, 2, 0x080024d0, 2, 1);
    upload_first_header(qts, 0x68);
    user_cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_CI));
    push_user_words(qts, (uint32_t[]) { 0x11225566, 0xd3d37a7a }, 2);
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 0, 0), ==,
                    0x11111155);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 1, 0), ==,
                    0xd3d3d37a);

    upload_begin(qts, 0x182000, 2, 0x080024d0, 2, 1);
    upload_first_header(qts, 0x69);
    user_cmd_push(qts, ODSY_IMAGE_BURST(3, ODSY_IMAGE_FMT_SHORT));
    push_user_words(qts, (uint32_t[]) {
        0x11225566, 0x99aadfdf, 0xa0a00000,
    }, 3);
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x182000, 0, 0), ==,
                    0x115599ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x182000, 1, 0), ==,
                    0xdfa000ff);

    /* Two rows of three: one unbounded pixel, a two-pixel group, a row. */
    upload_begin(qts, 0x183000, 2, 0x080024d0, 3, 2);
    upload_first_header(qts, 0x6c);
    user_cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_SHORT));
    push_user_words(qts, (uint32_t[]) { 0xffff2202, 0x3eb446fd }, 2);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    user_cmd_push(qts, 0x0f41106c);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    user_cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_SHORT));
    push_user_words(qts, (uint32_t[]) {
        0x80000000, 0x0000ffff, 0x11225566, 0x99aaddee,
    }, 4);
    user_cmd_push(qts, ODSY_IMAGE_BURST(6, ODSY_IMAGE_FMT_SHORT));
    push_user_words(qts, (uint32_t[]) {
        0xffff0000, 0x0000ffff, 0xffff0000, 0xffff0000,
        0xffffffff, 0x00000000,
    }, 6);
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 0, 0), ==,
                    0x473e22ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 1, 0), ==,
                    0xff000080);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 2, 0), ==,
                    0xdd995511);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 0, 1), ==,
                    0xff0000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 1, 1), ==,
                    0x00ff00ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 2, 1), ==,
                    0x0000ffff);
    assert_screen_untouched(qts);
    board_quit(qts);
}

/* One RGBA8 texel by DMA at (x, y) of the object at base, units wide. */
static void dma_object_texel(QTestState *qts, uint32_t base, uint32_t units,
                             uint32_t x, uint32_t y, uint32_t rgba)
{
    const uint64_t table = 0xb80000;
    const uint64_t page = 0xb84000;
    const uint32_t hdr[8] = {
        base, y << 16 | x, units, (1 << 16) | 1, 0x80013000, 0, 4,
        0x3201004b,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(page, 4) };
    uint8_t bytes[4];

    stl_be_p(bytes, rgba);
    qtest_memwrite(qts, page, bytes, sizeof(bytes));
    dma_image_table(qts, table, hdr, pte, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
}

/*
 * The object store recycles. Two 4096-texel-wide objects of 3000 rows
 * take most of the 128 MiB store; reshaping the first leaves its rows
 * behind as a hole that a third such object reclaims without losing the
 * second. A fourth, larger object then forgets the least recently used
 * object (the second) and keeps the rest. Then more objects than the table
 * holds: the oldest is forgotten and the newest still lands. Both kinds of
 * forgetting are reported once, naming the object. Before, the
 * 64-entry table dropped every later object and the 16 MiB store refused
 * the first large one.
 */
static void test_object_store_reclaims(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-store-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    dma_object_texel(qts, 0x200000, 256, 5, 2999, 0x11111111);
    dma_object_texel(qts, 0x300000, 256, 6, 2999, 0x22222222);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x200000, 5, 2999), ==,
                    0x11111111);
    /* A new row length discards the first object's rows. */
    dma_object_texel(qts, 0x200000, 2, 1, 0, 0x33333333);
    dma_object_texel(qts, 0x400000, 256, 7, 2999, 0x44444444);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x200000, 1, 0), ==,
                    0x33333333);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x300000, 6, 2999), ==,
                    0x22222222);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x400000, 7, 2999), ==,
                    0x44444444);

    dma_object_texel(qts, 0x500000, 256, 8, 3999, 0x55555555);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x500000, 8, 3999), ==,
                    0x55555555);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x300000, 6, 2999), ==, 0);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x200000, 1, 0), ==,
                    0x33333333);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x400000, 7, 2999), ==,
                    0x44444444);

    for (uint32_t i = 0; i < 1024; i++) {
        user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
        user_cmd_push(qts, 0x600000 + i * 0x100);
        user_cmd_push(qts, 2);
        user_cmd_push(qts, 0x080024d0);
        user_cmd_push(qts, 0);
    }
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==,
                     1024);
    dma_object_texel(qts, 0x600000 + 1023 * 0x100, 2, 2, 2, 0x66666666);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x600000 + 1023 * 0x100,
                                           2, 2), ==, 0x66666666);
    /* The large objects were the least recently used, so they went first. */
    g_assert_cmphex(odsy_test_object_texel(qts, 0x500000, 8, 3999), ==, 0);
    board_quit(qts);

    /* Each kind of forgetting is reported, naming the first object. */
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "board-memory store is full; the least "
                            "recently used object, 0x300000, is forgotten"));
    g_assert_nonnull(strstr(log, "more than 1024 board-memory objects; the "
                            "least recently used, 0x200000, is forgotten"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * Growing an object that is not last in a full store forgets only what the
 * new size needs. Two 3000-row objects fill most of the store; growing the
 * first to 4000 rows fits beside the second once the store is packed with
 * the first last, so the second survives and the first keeps its rows.
 * Before, each round packed the store and then forgot the second anyway.
 */
static void test_object_store_grows_without_forgetting(void)
{
    QTestState *qts = board_start("");

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    dma_object_texel(qts, 0x200000, 256, 5, 2999, 0x11111111);
    dma_object_texel(qts, 0x300000, 256, 6, 2999, 0x22222222);
    dma_object_texel(qts, 0x200000, 256, 7, 3999, 0x33333333);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x200000, 5, 2999), ==,
                    0x11111111);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x200000, 7, 3999), ==,
                    0x33333333);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x300000, 6, 2999), ==,
                    0x22222222);
    board_quit(qts);
}

/* Save a board to a file and return the stream's size in bytes. */
static gint64 object_store_save(QTestState *from, const char *path)
{
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    GStatBuf st;

    sgi_migrate_stopped(from, uri);
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    return st.st_size;
}

/*
 * A save carries only the used part of the 128 MiB object store, and the
 * objects come back whole: a board holding 48 MiB more of objects saves a
 * stream about that much longer. Before, every save carried the whole
 * store, so the two streams were the same length.
 */
static void test_object_store_migrates_used_part(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *small_path = NULL;
    g_autofree char *large_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *small = board_start("");
    QTestState *large = board_start("");
    QTestState *to;
    gint64 small_size, large_size;
    int fd;

    fd = g_file_open_tmp("odyssey-store-small-XXXXXX", &small_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("odyssey-store-large-XXXXXX", &large_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    for (unsigned b = 0; b < 2; b++) {
        QTestState *qts = b ? large : small;

        qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                     BEDROCK_MD_INSTALLED_BANKS);
        dma_object_texel(qts, 0x200000, 2, 3, 40, 0x11223344);
    }
    dma_object_texel(large, 0x300000, 256, 5, 2999, 0x55667788);
    small_size = object_store_save(small, small_path);
    large_size = object_store_save(large, large_path);
    g_test_message("streams %" G_GINT64_FORMAT " and %" G_GINT64_FORMAT
                   " bytes", small_size, large_size);
    /*
     * The streams differ by the large object's words, within slack for
     * the run-to-run variation of a few bytes in the rest of the stream.
     */
    g_assert_cmpint(large_size - small_size, >=,
                    4096 * 3000 * 4 - 64 * 1024);
    g_assert_cmpint(large_size - small_size, <=,
                    4096 * 3000 * 4 + 1024 * 1024);

    uri = g_strdup_printf("file:%s", large_path);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(odsy_test_object_texel(to, 0x200000, 3, 40), ==,
                    0x11223344);
    g_assert_cmphex(odsy_test_object_texel(to, 0x300000, 5, 2999), ==,
                    0x55667788);
    g_assert_cmphex(odsy_test_object_texel(to, 0x300000, 6, 2999), ==, 0);
    board_quit(to);
    board_quit(large);
    board_quit(small);
    g_assert_cmpint(unlink(small_path), ==, 0);
    g_assert_cmpint(unlink(large_path), ==, 0);
}

/*
 * The textured draw after an upload (phase H's words): the state words are
 * accepted, the draw lands inside the probe window after 0x90008116, not at
 * the screen origin the upload's override named, and -- until the shared
 * rasterizer samples -- it is drawn in the current colour and reported once.
 */
static void test_command_fifo_gl_texture_state(void)
{
    static const uint32_t draw[] = {
        0x00010405, 0x00002410,
        0x00011c16, 0x08000c60, 8, 8, 1, 0x11000000, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x0016b000, 0x00400002, 0,
        0x00010406, 0,
        0x00010452, 0x00f00000,
        ODSY_GL_COLOR_3F, 0x3f800000, 0x3f800000, 0x3f800000,
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        0xc380c821, 0, 0, ODSY_GL_VERTEX_2F, 0xbf666666, 0xbf666666,
        0xc380c821, 0x3f800000, 0, ODSY_GL_VERTEX_2F, 0x3f666666, 0xbf666666,
        0xc380c821, 0x3f800000, 0x3f800000,
        ODSY_GL_VERTEX_2F, 0x3f666666, 0x3f666666,
        ODSY_CMD_GL_END,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-tex-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    probe_window(qts);
    upload_begin(qts, 0x16b000, 2, 0x08000020, 8, 8);
    upload_first_header(qts, 0x42);
    upload_end(qts);
    /*
     * upload_begin()'s own 0x81d word was that upload's clip box (the object's
     * 8x8 extent), not a lingering window setting -- as observed, the register
     * "arrives with every draw, enabled or not". Restore it to the probe
     * window's own full 128x128 extent before this draw, the same way a real
     * client's draw setup would.
     */
    clip_box_set(qts, 0, 127, 0, 127);
    push_user_words(qts, draw, ARRAY_SIZE(draw));
    g_assert_cmphex(odsy_test_pixel(qts, 370, 300), ==, 0xffffffffu);
    g_assert_cmphex(odsy_test_pixel(qts, 114, 44), ==, 0);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(count_substr(log, "incomplete texture image"), ==, 1);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The 0x0452 colour write mask per channel: bit 20 red, 21 green, 22 blue,
 * 23 alpha (the GL library's glColorMask). Under glColorMask(GL_TRUE, GL_FALSE,
 * GL_TRUE, GL_TRUE), 0x00d00000, a red triangle over a cyan window keeps the
 * plane's green: the pixel becomes yellow, not red. The rectangle takes the
 * same mask.
 */
static void test_command_fifo_gl_colour_mask(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00f00000);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_CYAN);

    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00d00000);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, ODSY_PLANE_CYAN);

    /* The rectangle: red over cyan with green masked is yellow everywhere. */
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 383, 383), ==, ODSY_PLANE_YELLOW);

    /* Only blue written: blue over yellow keeps red and green, so white. */
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00400000);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, 0xffffffffu);
    board_quit(qts);
}

/*
 * GL_DITHER is 0x0404 bit 18. With the bit clear a mid-grey triangle (0.5,
 * 127.5 a channel) is one value over an aligned 4x4 block, and nothing is
 * reported. With it set the model draws the same, undithered, and reports
 * that once however many draws follow: the report proves the bit reaches the
 * device state and follows 0x0404.
 *
 * Waiting on the shared dither rule (not asserted yet): with bit 18 set,
 * exactly 8 of the block's 16 channels are 128 and 8 are 127 under the 4x4
 * Bayer matrix; count, not positions, since the matrix's anchor is unknown.
 */
/* Draw the grey triangle under 0x0404 = mode and return the -d unimp log. */
static char *dither_draw(uint32_t mode, unsigned draws, bool check_block)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    const uint32_t grey = 0x3f000000;
    char *log = NULL;
    QTestState *qts;
    uint32_t x, y;
    unsigned i;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-dither-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    probe_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, mode);
    for (i = 0; i < draws; i++) {
        push_triangle_3f(qts, grey, grey, grey, odsy_near_yellow);
    }
    if (check_block) {
        for (y = 332; y < 336; y++) {
            for (x = 316; x < 320; x++) {
                g_assert_cmphex(odsy_test_pixel(qts, x, y), ==, 0x808080ffu);
            }
        }
    }
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    return log;
}

static void test_command_fifo_gl_dither(void)
{
    g_autofree char *off = dither_draw(0x02002200, 1, true);
    g_autofree char *on = dither_draw(0x02042200, 2, false);

    g_assert_cmpuint(count_substr(off, "dither not modelled"), ==, 0);
    g_assert_cmpuint(count_substr(on, "dither not modelled"), ==, 1);
}

/*
 * Colour resolves at scanout. The plane keeps the index or RGB the guest
 * drew; what the display shows follows the pixel's drawable, that drawable's
 * display mode and the lookup RAM as they stand now. Each test names a pixel
 * the old paint-time resolution gets wrong.
 */
static void lut_write(QTestState *qts, uint32_t index, uint32_t value)
{
    backend_push32(qts, ODSY_BACKEND_HDR(index, 1));
    backend_push32(qts, value);
}

static void dispmode_write(QTestState *qts, uint32_t did, uint32_t mode)
{
    lut_write(qts, 0x2900 + did, mode);
}

#define ODSY_DISPMODE_GROUP18_CI 0x004b5f20u
#define ODSY_SHOWN_BLACK 0x000000ffu

/*
 * The X server's blank zeroes colormap group 17; a pixel painted during the
 * blank must come back when the unblank reloads it. Paint-time resolution
 * left it black: that is the captured desktop.
 */
static void test_scanout_wipe_repaint_reload(void)
{
    QTestState *qts = board_start("");
    uint32_t i;

    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    lut_write(qts, 0x1105, ODSY_LUT_RED);
    id_plane_write(qts, 1, 0xf8, 100, 100, 116, 116);
    colour_index_fill(qts, 5, 100, 100, 116, 116);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, 0xff0000ffu);

    /* The blank: group 17's 256 entries zeroed; no repaint. */
    backend_push32(qts, ODSY_BACKEND_HDR(0x1100, 0x100));
    for (i = 0; i < 0x100; i++) {
        backend_push32(qts, 0);
    }
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, ODSY_SHOWN_BLACK);

    /* Painted during the blank: black too. */
    colour_index_fill(qts, 5, 100, 100, 116, 116);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, ODSY_SHOWN_BLACK);

    /* The unblank reloads the entry: red again with no repaint. */
    lut_write(qts, 0x1105, ODSY_LUT_RED);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_shown(qts, 100, 115), ==, 0xff0000ffu);
    board_quit(qts);
}

/*
 * The X server's colormap install rewrites the drawable's display mode;
 * the drawable's pixels change colour with no repaint.
 */
static void test_scanout_display_mode_recolours(void)
{
    QTestState *qts = board_start("");

    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    dispmode_write(qts, 2, ODSY_DISPMODE_GROUP18_CI);
    lut_write(qts, 0x1105, ODSY_LUT_RED);
    lut_write(qts, 0x1205, ODSY_LUT_BLUE);
    id_plane_write(qts, 1, 0xf8, 100, 100, 116, 116);
    id_plane_write(qts, 2, 0xf8, 200, 100, 216, 116);
    colour_index_fill(qts, 5, 100, 100, 116, 116);
    colour_index_fill(qts, 5, 200, 100, 216, 116);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_shown(qts, 208, 108), ==, 0x0000ffffu);

    dispmode_write(qts, 2, ODSY_DISPMODE_GROUP17);
    g_assert_cmphex(odsy_shown(qts, 208, 108), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_test_pixel(qts, 208, 108), ==, 5);
    board_quit(qts);
}

/*
 * An RGB pixel passes through the gamma table at 0x1a00, which the RGB
 * display modes name as their group (26): zeroing the table blackens it,
 * reloading brings it back.
 */
static void test_scanout_rgb_through_gamma(void)
{
    QTestState *qts = board_start("");
    uint32_t i;

    display_identity(qts, 8);
    id_plane_write(qts, 8, 0xf8, 100, 100, 116, 116);
    /* Back to the colour buffer, as every real fill re-selects it. */
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x2000);
    cmd_push(qts, 0x50);
    cmd_push(qts, 0x20006950);
    cmd_push(qts, 0);
    rgb_fill(qts, 0x11, 0x22, 0x33, 100, 100, 116, 116);
    g_assert_cmphex(odsy_test_pixel(qts, 108, 108), ==, 0x112233ffu);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, 0x112233ffu);

    backend_push32(qts, ODSY_BACKEND_HDR(0x1a00, 0x600));
    for (i = 0; i < 0x600; i++) {
        backend_push32(qts, 0);
    }
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, ODSY_SHOWN_BLACK);

    display_identity(qts, 8);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, 0x112233ffu);
    board_quit(qts);
}

/*
 * A narrow TrueColor visual's display mode shows only the top bits of each
 * channel byte, repeated to full scale: under the 12-bit visual's mode a
 * window filled with 0x50 0x80 0xa0 shows what the 24-bit mode shows for
 * 0x55 0x88 0xaa, the colour its client means by those bytes, and under the
 * 15-bit visual's mode what it shows for 0x52 0x84 0xa5. The planes keep the
 * bytes drawn.
 */
#define ODSY_DISPMODE_RGB12 0x004b59a6u
#define ODSY_DISPMODE_RGB15 0x009657a7u

static void narrow_fill(QTestState *qts, uint32_t did, uint32_t r,
                        uint32_t g, uint32_t b, uint32_t x0)
{
    id_plane_write(qts, did, 0xf8, x0, 100, x0 + 16, 116);
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x2000);
    cmd_push(qts, 0x50);
    cmd_push(qts, 0x20006950);
    cmd_push(qts, 0);
    rgb_fill(qts, r, g, b, x0, 100, x0 + 16, 116);
}

static void test_scanout_rgb_narrow_visuals(void)
{
    static const uint32_t passthrough[] = { 1, 6, 7 };
    QTestState *qts = board_start("");
    uint32_t wide;
    size_t i;

    display_identity(qts, 8);
    display_identity(qts, 9);
    display_identity(qts, 10);
    narrow_fill(qts, 8, 0x50, 0x80, 0xa0, 100);
    narrow_fill(qts, 9, 0x55, 0x88, 0xaa, 200);
    narrow_fill(qts, 10, 0x52, 0x84, 0xa5, 300);
    wide = odsy_shown(qts, 108, 108);
    g_assert_cmphex(wide, !=, odsy_shown(qts, 208, 108));
    g_assert_cmphex(wide, !=, odsy_shown(qts, 308, 108));

    dispmode_write(qts, 8, ODSY_DISPMODE_RGB12);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, odsy_shown(qts, 208, 108));
    g_assert_cmphex(odsy_test_pixel(qts, 108, 108), ==, 0x5080a0ffu);

    dispmode_write(qts, 8, ODSY_DISPMODE_RGB15);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, odsy_shown(qts, 308, 108));

    /* A 30-bit field (1 or 6) or one no visual uses shows the byte drawn. */
    for (i = 0; i < G_N_ELEMENTS(passthrough); i++) {
        dispmode_write(qts, 8, (ODSY_DISPMODE_RGB12 & ~(7u << 9)) |
                               (passthrough[i] << 9));
        g_assert_cmphex(odsy_shown(qts, 108, 108), ==, wide);
    }
    board_quit(qts);
}

/* Paint first, program the drawable and colormap after: the colour shows. */
static void test_scanout_paint_order(void)
{
    QTestState *qts = board_start("");

    id_plane_write(qts, 1, 0xf8, 100, 100, 116, 116);
    colour_index_fill(qts, 5, 100, 100, 116, 116);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, ODSY_SHOWN_BLACK);

    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    lut_write(qts, 0x1105, ODSY_LUT_GREEN);
    g_assert_cmphex(odsy_shown(qts, 108, 108), ==, 0x00ff00ffu);
    board_quit(qts);
}

/*
 * Colour-index OpenGL. A context in index mode -- colour-mode words 0x22 = 1
 * and 0x23 = 3 -- draws its current index, not an RGB colour: the plane keeps
 * the raw index and the display resolves it through the window's drawable,
 * here drawable 24 whose display mode names group 18 (lookup RAM 0x1200).
 * glClearIndex travels as the rectangle's first float, glIndexi as
 * 0xc580c406, glIndexf as 0xc580c407 with a float, and the kernel's replay
 * forms as 0xce80c406 and 0xce80c407. Back in RGBA mode the same triangle
 * stores its RGB colour.
 */
#define ODSY_GL_MODE_INDEX_A 0x22008001u
#define ODSY_GL_MODE_INDEX_B 0x23008003u
#define ODSY_GL_MODE_RGBA_A 0x22008000u
#define ODSY_GL_MODE_RGBA_B 0x23008002u
#define ODSY_GL_INDEX_F 0xc580c407u
#define ODSY_GL_REPLAY_INDEX_F 0xce80c407u
#define ODSY_GL_INDEX_MODE_COLOR_3F 0xce80cc0eu

/* Lookup-RAM entry for index i of the test palette, and what it shows. */
static uint32_t ci_lut(uint32_t i)
{
    return ((i * 30) << 22) | ((255 - i * 30) << 12) | ((i * 10) << 2);
}

static uint32_t ci_shown(uint32_t i)
{
    return ((i * 30) << 24) | ((255 - i * 30) << 16) | ((i * 10) << 8) | 0xff;
}

/* The probe's triangle's three vertices, with no colour word before them. */
static void push_bare_triangle(QTestState *qts)
{
    static const uint32_t v[6] = {
        0xbf400000, 0xbf400000, 0x3f400000, 0xbf200000,
        0xbe000000, 0x3f3c0000,
    };
    unsigned i;

    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    for (i = 0; i < 3; i++) {
        user_cmd_push(qts, ODSY_GL_VERTEX_2F);
        user_cmd_push(qts, v[2 * i]);
        user_cmd_push(qts, v[2 * i + 1]);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

/* The x-mesa-fallbacks-* counters, one per refusal reason. */
static const char *const odsy_mesa_fallback_reasons[] = {
    "rules", "dither", "primitive", "smoothing", "colour-index", "buffers",
    "fragment-tests", "fog", "facing", "clip-planes", "logic-op",
    "polygon-offset", "window", "geometry", "line-shape",
    "resource", "store-width", "blend", "blend-overlap", "texture",
    "provider", "raster-op", "mixed-frame", "host-viewport",
    "host-point-size", "host-program", "host-probe", "host-staging",
    "snapshot", "precision",
};

static uint64_t odsy_mesa_fallbacks(QTestState *qts, const char *reason)
{
    g_autofree char *name = g_strdup_printf("x-mesa-fallbacks-%s", reason);

    return odsy_qom_get_uint(qts, name);
}

static uint64_t odsy_mesa_fallback_total(QTestState *qts)
{
    uint64_t total = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(odsy_mesa_fallback_reasons); i++) {
        total += odsy_mesa_fallbacks(qts, odsy_mesa_fallback_reasons[i]);
    }
    return total;
}

/*
 * The probe window's plane pixels after an RGBA triangle, and after a
 * colour-index triangle over it. With x-mesa-renderer=on, @mesa says the
 * renderer exists; the RGBA triangle is then drawn on Mesa and the
 * colour-index one, which Mesa refuses, on the scalar path, counted once
 * by its reason.
 */
static void mesa_fallback_scene(QTestState *qts, bool mesa,
                                uint32_t *rgba, uint32_t *index)
{
    probe_window(qts);
    push_captured_triangle(qts);
    odsy_test_pixels(qts, 256, 256, 128, 128, rgba);
    if (mesa) {
        g_assert_cmpuint(odsy_qom_get_uint(qts, "x-mesa-draws"), ==, 1);
        g_assert_cmpuint(odsy_mesa_fallback_total(qts), ==, 0);
    }
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 5);
    push_bare_triangle(qts);
    odsy_test_pixels(qts, 256, 256, 128, 128, index);
    if (mesa) {
        g_assert_cmpuint(odsy_qom_get_uint(qts, "x-mesa-draws"), ==, 1);
        g_assert_cmpuint(odsy_mesa_fallbacks(qts, "colour-index"), ==, 1);
        g_assert_cmpuint(odsy_mesa_fallback_total(qts), ==, 1);
    }
}

/*
 * Per-draw fallback: with the Mesa renderer on, a draw it refuses is drawn
 * on the scalar path inside the same callback, so the frame is the one a
 * scalar-only board draws, pixel for pixel, and the refusal is counted.
 */
static void test_mesa_per_draw_fallback(void)
{
    g_autofree uint32_t *scalar_rgba = g_new(uint32_t, 128 * 128);
    g_autofree uint32_t *scalar_index = g_new(uint32_t, 128 * 128);
    g_autofree uint32_t *mesa_rgba = g_new(uint32_t, 128 * 128);
    g_autofree uint32_t *mesa_index = g_new(uint32_t, 128 * 128);
    QTestState *qts = board_start("");
    bool mesa;

    mesa_fallback_scene(qts, false, scalar_rgba, scalar_index);
    board_quit(qts);
    /* The scene draws: yellow inside the triangle, then index 5 over it. */
    g_assert_cmphex(scalar_rgba[(333 - 256) * 128 + 317 - 256], ==,
                    0xffff00ff);
    g_assert_cmphex(scalar_index[(333 - 256) * 128 + 317 - 256], ==, 5);

    qts = board_start(" -global sgi-odyssey.x-mesa-renderer=on");
    mesa = !odsy_qom_get_bool(qts, "x-mesa-unavailable");
    if (!mesa && g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_error("QEMU_TEST_SGI_REQUIRE_MESA is set, but the device found "
                "no Mesa renderer");
    }
    mesa_fallback_scene(qts, mesa, mesa_rgba, mesa_index);
    board_quit(qts);
    g_assert_cmpmem(mesa_rgba, 128 * 128 * 4, scalar_rgba, 128 * 128 * 4);
    g_assert_cmpmem(mesa_index, 128 * 128 * 4, scalar_index, 128 * 128 * 4);
    if (!mesa) {
        g_test_skip("No Mesa renderer: drew every draw on the scalar path");
    }
}

/*
 * A colour-index context that never sets an index draws GL vertices in
 * index 1, GL's initial index, which is also what the save writes and a
 * replay restores: the never-set draw, the draw after 0xc580c406 1 and the
 * draw after replaying the reset snapshot's index all agree, and index 0
 * differs.
 */
static void test_command_fifo_gl_never_set_index(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = board_start("");
    uint32_t never_set;

    probe_window(qts);
    material_snapshot(qts, 0x7f);
    g_assert_cmphex(qtest_readl(qts, p + 0x240), ==, 0xc580c406);
    g_assert_cmphex(qtest_readl(qts, p + 0x800), ==, 1);
    g_assert_cmphex(qtest_readl(qts, p + 0x110), ==, 0xc580c406);
    g_assert_cmphex(qtest_readl(qts, p + 0x340), ==, 1);

    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    push_bare_triangle(qts);
    never_set = odsy_test_pixel(qts, 317, 334);
    g_assert_cmphex(never_set, ==, 1);

    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 0);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), !=, never_set);

    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 1);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, never_set);

    /* Back to 0, then the kernel's replay of the reset snapshot's index. */
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 0);
    cmd_push(qts, qtest_readl(qts, p + 0x240));
    cmd_push(qts, qtest_readl(qts, p + 0x800));
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, never_set);

    /* A system reset after a client set index 0 draws index 1 again. */
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 0);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), !=, never_set);
    qtest_system_reset(qts);
    probe_window(qts);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, never_set);
    board_quit(qts);
}

/*
 * The overlay. The X server's writes to the id-plane destination reach the
 * id byte under drawing mask 0x3 -- the main id in bits 7:3, the overlay id
 * in bits 2:0 -- and the overlay pixel byte under mask 0xc. Scanout shows
 * overlay colormap entry 256 * group + pixel where the pixel's overlay id
 * has an enabled mode at backend entry 0x2920 + id (group in bits 7:3, as
 * flight's 0xb1 names group 22) and the overlay pixel is not zero; the main
 * planes show elsewhere. An overlay GL context's colour-index clear to the
 * same destination, bound as its draw drawable, fills the overlay byte and
 * leaves the colour buffers and the id byte alone.
 */
static void draw_mask(QTestState *qts, uint32_t mask);
static uint32_t odsy_buffer_pixel(QTestState *qts, uint32_t buffer,
                                  uint32_t x, uint32_t y);

static void aux_write(QTestState *qts, uint32_t value, uint32_t mask,
                      uint32_t buffers, uint32_t x0, uint32_t y0,
                      uint32_t x1, uint32_t y1)
{
    draw_mask(qts, buffers);
    cmd_push(qts, ODSY_CMD_PLANE_MASK);
    cmd_push(qts, mask << 12);
    cmd_push(qts, 1);
    cmd_push(qts, 0);
    cmd_push(qts, 0xff);
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, ODSY_DEST_ID_PLANES);
    cmd_push(qts, 0);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_CMD_GL_QUADS);
    cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    cmd_push(qts, value);
    quad_rect(qts, x0, y0, x1, y1);
}

static uint32_t odsy_did(QTestState *qts, uint32_t x, uint32_t y)
{
    odsy_test_pixel_select(qts, x, y);
    return odsy_qom_get_uint(qts, "x-test-did");
}

static void test_scanout_overlay_planes(void)
{
    QTestState *qts = board_start("");

    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    lut_write(qts, 0x1105, ODSY_LUT_RED);
    id_plane_write(qts, 1, 0xf8, 100, 100, 140, 140);
    colour_index_fill(qts, 5, 100, 100, 140, 140);
    g_assert_cmphex(odsy_shown(qts, 110, 110), ==, 0xff0000ffu);

    /* Overlay id 5 over the window, overlay pixel 0x30 over its left half. */
    aux_write(qts, 5, 0x07, 0x3, 100, 100, 140, 140);
    aux_write(qts, 0x30, 0xff, 0xc, 100, 100, 120, 140);
    g_assert_cmphex(odsy_did(qts, 110, 110), ==, (1 << 3) | 5);
    g_assert_cmphex(odsy_test_pixel(qts, 110, 110), ==, 5);
    /* No mode for overlay id 5 yet: the main planes show. */
    g_assert_cmphex(odsy_shown(qts, 110, 110), ==, 0xff0000ffu);

    lut_write(qts, 0x2925, 0xb1);
    lut_write(qts, 0x1630, ODSY_LUT_BLUE);
    g_assert_cmphex(odsy_shown(qts, 110, 110), ==, 0x0000ffffu);
    /* Overlay pixel 0 is transparent. */
    g_assert_cmphex(odsy_shown(qts, 130, 110), ==, 0xff0000ffu);

    /*
     * An overlay GL context's glClearIndex(48), glClear, with the kernel's
     * 0x608 binding the id-plane destination as its draw drawable.
     */
    aux_write(qts, 5, 0x07, 0x3, 256, 256, 384, 384);
    probe_window(qts);
    cmd_push(qts, 0x608);
    cmd_push(qts, 0x80142000);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    draw_mask(qts, 0xc);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x142000);
    user_cmd_push(qts, 0x50);
    user_cmd_push(qts, ODSY_DEST_ID_PLANES);
    user_cmd_push(qts, 0);
    push_clear(qts, 0x42400000, ODSY_F_ZERO, ODSY_F_ZERO, 0x43000000,
               0x43000000);
    g_assert_cmphex(odsy_shown(qts, 300, 300), ==, 0x0000ffffu);
    g_assert_cmphex(odsy_did(qts, 300, 300), ==, 5);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 300, 300), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 300, 300), ==, 0);
    board_quit(qts);
}

/*
 * A GL draw at the id-plane destination that the model refuses -- an index
 * draw to the id byte, or any RGBA draw -- stores nothing and is reported
 * once, however many draws follow. The report comes from the draw, before
 * any pixel, so the store hooks only store.
 */
static char *gl_aux_refusal_log(bool index, uint32_t *did, uint32_t *colour)
{
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-aux-refusal-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    aux_write(qts, 5, 0x07, 0x3, 256, 256, 384, 384);
    probe_window(qts);
    cmd_push(qts, 0x608);
    cmd_push(qts, 0x80142000);
    if (index) {
        user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
        user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    }
    draw_mask(qts, 0x3);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x142000);
    user_cmd_push(qts, 0x50);
    user_cmd_push(qts, ODSY_DEST_ID_PLANES);
    user_cmd_push(qts, 0);
    for (unsigned i = 0; i < 2; i++) {
        push_clear(qts, 0x42400000, ODSY_F_ZERO, ODSY_F_ZERO, 0x43000000,
                   0x43000000);
    }
    *did = odsy_did(qts, 300, 300);
    *colour = odsy_buffer_pixel(qts, 0, 300, 300);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    return log;
}

static void test_gl_aux_refusals_reported_once(void)
{
    uint32_t did, colour;
    g_autofree char *index_log = gl_aux_refusal_log(true, &did, &colour);
    g_autofree char *rgba_log = NULL;

    g_assert_cmphex(did, ==, 5);
    g_assert_cmphex(colour, ==, 0);
    g_assert_cmpuint(count_substr(index_log, "a GL id-byte write to the "
                                  "id-plane destination is not modeled"),
                     ==, 1);
    g_assert_cmpuint(count_substr(index_log, "an RGBA draw"), ==, 0);

    rgba_log = gl_aux_refusal_log(false, &did, &colour);
    g_assert_cmphex(did, ==, 5);
    g_assert_cmphex(colour, ==, 0);
    g_assert_cmpuint(count_substr(rgba_log, "an RGBA draw to the id-plane "
                                  "destination is not modeled"), ==, 1);
    g_assert_cmpuint(count_substr(rgba_log, "id-byte"), ==, 0);
}

/*
 * An id-plane write whose drawing mask selects neither the id byte (buffer
 * 0) nor the overlay byte (buffer 1) changes nothing and is reported once.
 */
static void test_scanout_overlay_unselected_mask(void)
{
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-aux-mask-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    aux_write(qts, 0xff, 0xff, 0x0, 100, 100, 104, 104);
    aux_write(qts, 0xff, 0xff, 0x0, 100, 100, 104, 104);
    g_assert_cmphex(odsy_did(qts, 101, 101), ==, 0);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(count_substr(log, "selects neither the id nor the "
                                       "overlay byte"), ==, 1);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * 4Dwm's menus are windows in the overlay visual: id 5 with mode 0xb1 here,
 * over a background of overlay pixel 0x30.
 */
static QTestState *overlay_menu_start(void)
{
    QTestState *qts = board_start("");

    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    lut_write(qts, 0x1105, ODSY_LUT_RED);
    id_plane_write(qts, 1, 0xf8, 100, 100, 140, 140);
    colour_index_fill(qts, 5, 100, 100, 140, 140);
    aux_write(qts, 5, 0x07, 0x3, 100, 100, 140, 140);
    aux_write(qts, 0x30, 0xff, 0xc, 100, 100, 140, 140);
    lut_write(qts, 0x2925, 0xb1);
    lut_write(qts, 0x1630, ODSY_LUT_BLUE);
    lut_write(qts, 0x1602, ODSY_LUT_GREEN);
    g_assert_cmphex(odsy_shown(qts, 101, 100), ==, 0x0000ffffu);
    return qts;
}

/* An X server destination select of the id planes with third word value. */
static void aux_select(QTestState *qts, uint32_t value)
{
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, value);
    cmd_push(qts, 0);
}

/*
 * A one-bit glyph at (100, 100) in index 2: its set bit shows the overlay
 * entry 2, its clear bits keep the background, and the colour buffers and
 * the id byte are untouched.
 */
static void overlay_menu_glyph(QTestState *qts)
{
    uint32_t bitmap[32] = { [0] = 0x80000000 };

    pattern_load(qts, bitmap);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_32X32);
    cmd_push(qts, ODSY_CMD_LOGIC_OP);
    cmd_push(qts, 3);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x80000 | ODSY_PATTERN_MODE_ENABLE);
    index_fill(qts, 2, 100, 100, 104, 102);
    g_assert_cmphex(odsy_shown(qts, 100, 100), ==, 0x00ff00ffu);
    g_assert_cmphex(odsy_shown(qts, 101, 100), ==, 0x0000ffffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 100, 100), ==, 5);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 100, 100), ==, 0);
    g_assert_cmphex(odsy_did(qts, 100, 100), ==, (1 << 3) | 5);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, 0x80000);
}

/*
 * The X server draws a menu's text as pattern quads, its bevels as lines
 * and its cascade arrows as index images, at the id-plane destination under
 * drawing mask 0xc and plane mask 0xff. Each must land in the overlay byte
 * over the menu's background and leave the colour buffers alone.
 */
static void test_scanout_overlay_menu(void)
{
    QTestState *qts = overlay_menu_start();

    aux_select(qts, ODSY_DEST_ID_PLANES);
    overlay_menu_glyph(qts);

    /* A bevel line. */
    colour_index(qts, 2);
    cmd_push(qts, ODSY_CMD_GL_BEGIN);
    cmd_push(qts, ODSY_GL_LINES);
    vertex2i(qts, 110, 110);
    vertex2i(qts, 120, 110);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_shown(qts, 115, 110), ==, 0x00ff00ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 115, 110), ==, 0);

    /* A cascade arrow: indices 2, 0x30, 2, 0x30. */
    image_begin(qts, 130, 120, 134, 121);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x41));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x02300230);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_shown(qts, 130, 120), ==, 0x00ff00ffu);
    g_assert_cmphex(odsy_shown(qts, 131, 120), ==, 0x0000ffffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 130, 120), ==, 0);
    board_quit(qts);
}

/*
 * The X server sets bit 29 of a 0x1017 select's third word only when the GC
 * function does not read the destination, and bit 23 in the select that
 * follows a select of word 0. Neither names a buffer. 4Dwm's menu text
 * arrives under 0x00080080 and lands in the overlay as under 0x20080080;
 * selecting the colour buffer or the id planes with either bit, or a word
 * of 0, makes no off-screen object, while a texture select still does.
 */
static void test_command_fifo_dest_select_flags(void)
{
    static const uint32_t selects[][2] = {
        { 0x2000, ODSY_DEST_COLOUR_INDEX },
        { 0x2000, ODSY_DEST_COLOUR_INDEX & ~(1u << 29) },
        { 0x2000, 0 },
        { 0x2000, ODSY_DEST_COLOUR_INDEX | (1u << 23) },
        { 0x142000, ODSY_DEST_ID_PLANES & ~(1u << 29) },
        { 0x142000, 0 },
        { 0x142000, ODSY_DEST_ID_PLANES | (1u << 23) },
    };
    QTestState *qts = overlay_menu_start();

    aux_select(qts, ODSY_DEST_ID_PLANES & ~(1u << 29));
    overlay_menu_glyph(qts);
    for (unsigned int i = 0; i < ARRAY_SIZE(selects); i++) {
        cmd_push(qts, ODSY_CMD_DEST_SELECT);
        cmd_push(qts, selects[i][0]);
        cmd_push(qts, 0x50);
        cmd_push(qts, selects[i][1]);
        cmd_push(qts, 0);
        g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);
    }
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x300000);
    cmd_push(qts, 2);
    cmd_push(qts, 0x080024d0);
    cmd_push(qts, 0);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 1);
    board_quit(qts);
}

/*
 * The 0x840 drawable-id test. When a window's visible region needs more than
 * four rectangles, the X server sends its bounding rectangle alone and the
 * kernel enables the test in 0x840's second operand: bit 16 on, the id mask
 * in bits 15:8 (0xf8 for the main id) and the window's id in bits 7:0
 * (id << 3). Writes then land only where the id byte matches. Here the probe
 * window has id 25 and a window with id 3 covers part of it; the clear
 * spans the probe window's whole rectangle but leaves the other window's
 * pixels alone. With bit 16 clear nothing is tested, whatever else the word
 * holds; the low-field form (mask 0x07) compares only the id's low field.
 */
#define ODSY_ID_PLANE_BASE 0x142000
#define ODSY_ID_COMPARE_ON 0x00010000u
#define ODSY_ID_COMPARE_MAIN(id) (ODSY_ID_COMPARE_ON | 0xf800u | ((id) << 3))

static void id_compare(QTestState *qts, bool user, uint32_t test)
{
    compare_push(qts, user, ODSY_CMD_DID_COMPARE);
    compare_push(qts, user, ODSY_ID_PLANE_BASE);
    compare_push(qts, user, test);
}

/* The probe window, id 25, under an id-3 window at [300, 340) x [320, 350). */
static void id_compare_windows(QTestState *qts)
{
    id_plane_write(qts, 25, 0xf8, 256, 256, 384, 384);
    id_plane_write(qts, 3, 0xf8, 300, 320, 340, 350);
}

static bool id_compare_covered(uint32_t x, uint32_t y)
{
    return x >= 300 && x < 340 && y >= 320 && y < 350;
}

static void assert_id_compare_clear(QTestState *qts, bool tested)
{
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 299, 320), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 340, 349), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 383, 383), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 320), ==,
                    tested ? 0 : ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 339, 349), ==,
                    tested ? 0 : ODSY_PLANE_BLUE);
}

static void test_command_fifo_gl_window_id_compare(void)
{
    QTestState *qts = board_start("");

    id_compare_windows(qts);
    probe_window(qts);
    id_compare(qts, false, ODSY_ID_COMPARE_MAIN(25));
    push_captured_clear(qts);
    assert_id_compare_clear(qts, true);
    board_quit(qts);

    qts = board_start("");
    id_compare_windows(qts);
    probe_window(qts);
    id_compare(qts, false, 0x2ee4f8c8);
    push_captured_clear(qts);
    assert_id_compare_clear(qts, false);
    board_quit(qts);

    /* Low field 5 under the probe window, 2 under the other; bits 31:17 set. */
    qts = board_start("");
    id_compare_windows(qts);
    aux_write(qts, 5, 0x07, 0x3, 256, 256, 384, 384);
    aux_write(qts, 2, 0x07, 0x3, 300, 320, 340, 350);
    probe_window(qts);
    id_compare(qts, false, 0xfffe0000u | ODSY_ID_COMPARE_ON | 0x0705);
    push_captured_clear(qts);
    assert_id_compare_clear(qts, true);
    board_quit(qts);
}

/*
 * The 2D fills apply the window's pixel rules a row at a time, so pin them
 * where a row walk could go wrong: clip rectangles that overlap, touch or
 * stand apart, the id test inside them, and a logical operation that would
 * show a pixel painted twice. XOR over an overlap must still flip a pixel
 * once. The expected image is computed here from the rules, pixel by pixel.
 * A copy along its own row must read each pixel before it is overwritten.
 */
#define FILL_RUNS_X 100
#define FILL_RUNS_Y 100
#define FILL_RUNS_W 64
#define FILL_RUNS_H 32

static const uint32_t fill_runs_clip[4][4] = {
    { 104, 104, 131, 119 },     /* overlaps the next */
    { 120, 110, 147, 127 },
    { 148, 104, 151, 107 },     /* touches the one above */
    { 156, 120, 159, 123 },     /* apart */
};

/* Rows that cross each rectangle's edges, for the slower id-plane probe. */
static const uint32_t fill_runs_rows[] = { 103, 104, 107, 108, 110, 119, 120,
                                           123, 127 };

static bool fill_runs_in_clip(uint32_t x, uint32_t y)
{
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t *r = fill_runs_clip[i];

        if (x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3]) {
            return true;
        }
    }
    return false;
}

static bool fill_runs_allowed(uint32_t x, uint32_t y)
{
    /* The id-3 window fails the id test. */
    return !(x >= 140 && x < 150) && fill_runs_in_clip(x, y);
}

static void fill_runs_check(QTestState *qts, uint32_t xor_from_pattern)
{
    g_autofree uint32_t *got = g_new(uint32_t, FILL_RUNS_W * FILL_RUNS_H);

    odsy_test_pixels(qts, FILL_RUNS_X, FILL_RUNS_Y, FILL_RUNS_W, FILL_RUNS_H,
                     got);
    for (uint32_t row = 0; row < FILL_RUNS_H; row++) {
        for (uint32_t col = 0; col < FILL_RUNS_W; col++) {
            uint32_t x = FILL_RUNS_X + col, y = FILL_RUNS_Y + row;
            uint32_t want = 0x11;

            if (fill_runs_allowed(x, y)) {
                want ^= 0x16;
                /* Pattern bits: every other column, from the leftmost. */
                if (xor_from_pattern && col < 32 && !(col & 1)) {
                    want ^= xor_from_pattern;
                }
            }
            g_assert_cmphex(got[row * FILL_RUNS_W + col], ==, want);
        }
    }
}

static void test_command_fifo_fill_runs_match_pixel_rules(void)
{
    QTestState *qts = board_start("");
    uint32_t bitmap[32];
    uint32_t row[8];

    index_fill(qts, 0x11, FILL_RUNS_X, FILL_RUNS_Y, FILL_RUNS_X + FILL_RUNS_W,
               FILL_RUNS_Y + FILL_RUNS_H);
    id_plane_write(qts, 25, 0xf8, FILL_RUNS_X, FILL_RUNS_Y,
                   FILL_RUNS_X + FILL_RUNS_W, FILL_RUNS_Y + FILL_RUNS_H);
    id_plane_write(qts, 3, 0xf8, 140, FILL_RUNS_Y, 150,
                   FILL_RUNS_Y + FILL_RUNS_H);
    window_clip(qts, fill_runs_clip, 4);
    id_compare(qts, false, ODSY_ID_COMPARE_MAIN(25));

    /* GXxor over the whole box: one flip wherever the rules allow. */
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, ODSY_MODE_INDEX_LOGIC_OP);
    cmd_push(qts, ODSY_CMD_LOGIC_OP);
    cmd_push(qts, 6);
    colour_index_fill(qts, 0x16, FILL_RUNS_X, FILL_RUNS_Y,
                      FILL_RUNS_X + FILL_RUNS_W, FILL_RUNS_Y + FILL_RUNS_H);
    fill_runs_check(qts, 0);

    /* The same through a transparent pattern of every other column. */
    for (unsigned i = 0; i < 32; i++) {
        bitmap[i] = 0xaaaaaaaa;
    }
    pattern_load(qts, bitmap);
    cmd_push(qts, ODSY_CMD_PATTERN_SHAPE);
    cmd_push(qts, ODSY_PATTERN_SHAPE_32X32);
    cmd_push(qts, ODSY_CMD_PATTERN_MODE);
    cmd_push(qts, ODSY_PATTERN_MODE_ENABLE | ODSY_MODE_INDEX_LOGIC_OP);
    colour_index_fill(qts, 0x01, FILL_RUNS_X, FILL_RUNS_Y, FILL_RUNS_X + 32,
                      FILL_RUNS_Y + FILL_RUNS_H);
    fill_runs_check(qts, 0x01);
    board_quit(qts);

    /* An id-plane write under the same rectangles, the id test off. */
    qts = board_start("");
    window_clip(qts, fill_runs_clip, 4);
    id_plane_write(qts, 9, 0xf8, FILL_RUNS_X, FILL_RUNS_Y,
                   FILL_RUNS_X + FILL_RUNS_W, FILL_RUNS_Y + FILL_RUNS_H);
    for (unsigned i = 0; i < ARRAY_SIZE(fill_runs_rows); i++) {
        uint32_t y = fill_runs_rows[i];

        for (uint32_t x = FILL_RUNS_X; x < FILL_RUNS_X + FILL_RUNS_W; x++) {
            g_assert_cmphex(odsy_did(qts, x, y), ==,
                            fill_runs_in_clip(x, y) ? 9 << 3 : 0);
        }
    }
    board_quit(qts);

    /* A copy three pixels right along its own row: memmove's result. */
    qts = board_start("");
    for (unsigned i = 0; i < 8; i++) {
        odsy_test_pixel_write(qts, 600 + i, 600, 0x01020300u + i);
    }
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (600u << 16) | 600);  /* SRC */
    cmd_push(qts, 0x80502050);
    cmd_push(qts, (1u << 16) | 8);      /* SIZE: 8 wide, 1 tall */
    cmd_push(qts, 0x82223042);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (600u << 16) | 603);  /* DST */
    cmd_push(qts, 0x3222204b);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_1);
    cmd_push(qts, ODSY_CMD_QUICK_FLUSH_ARG);
    cmd_push(qts, ODSY_CMD_STROBE);
    cmd_push(qts, ODSY_CMD_STROBE);
    odsy_test_pixels(qts, 600, 600, 8, 1, row);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(odsy_test_pixel(qts, 603 + i, 600), ==,
                        0x01020300u + i);
        g_assert_cmphex(row[i], ==, 0x01020300u + (i < 3 ? i : i - 3));
    }
    board_quit(qts);
}

/*
 * The test is part of the window the kernel pushes. The off-screen tail's
 * zero 0x840 does not replace it, a user-port 0x840 overrides it only until
 * the library's 0x90008116, and the restore brings it back with the window.
 */
static void test_command_fifo_gl_window_id_compare_restore(void)
{
    QTestState *qts = board_start("");

    id_compare_windows(qts);
    probe_window(qts);
    id_compare(qts, false, ODSY_ID_COMPARE_MAIN(25));
    kernel_offscreen_tail(qts);
    user_window_override(qts);
    id_compare(qts, true, 0);
    user_cmd_push(qts, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(qts);
    assert_id_compare_clear(qts, true);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0);
    board_quit(qts);
}

/*
 * Saved between the tail's 0x204b and its 0x840, the destination still
 * knows the next kernel 0x840 belongs to the tail and keeps the window's
 * test for the restore.
 */
static void test_command_fifo_gl_window_id_compare_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-id-compare-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    id_compare_windows(from);
    probe_window(from);
    id_compare(from, false, ODSY_ID_COMPARE_MAIN(25));
    window_origin(from, 0, 0);
    kernel_offscreen_clip(from);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    id_compare(to, false, 0);
    user_cmd_push(to, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(to);
    assert_id_compare_clear(to, true);
    board_quit(from);
    board_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A GL triangle under the test: every pixel is the untested draw's where the
 * id matches and untouched where it does not. With the Mesa renderer on, the
 * draw is refused for the window test and drawn on the scalar path.
 */
static void id_compare_triangle(QTestState *qts, bool tested, uint32_t *out)
{
    id_compare_windows(qts);
    probe_window(qts);
    if (tested) {
        id_compare(qts, false, ODSY_ID_COMPARE_MAIN(25));
    }
    push_captured_triangle(qts);
    odsy_test_pixels(qts, 256, 256, 128, 128, out);
}

static void test_command_fifo_gl_window_id_compare_triangle(void)
{
    g_autofree uint32_t *plain = g_new(uint32_t, 128 * 128);
    g_autofree uint32_t *tested = g_new(uint32_t, 128 * 128);
    g_autofree uint32_t *mesa = g_new(uint32_t, 128 * 128);
    unsigned covered_drawn = 0;
    QTestState *qts = board_start("");
    bool have_mesa;

    id_compare_triangle(qts, false, plain);
    board_quit(qts);
    qts = board_start("");
    id_compare_triangle(qts, true, tested);
    board_quit(qts);
    for (uint32_t y = 0; y < 128; y++) {
        for (uint32_t x = 0; x < 128; x++) {
            uint32_t i = y * 128 + x;

            if (id_compare_covered(256 + x, 256 + y)) {
                covered_drawn += plain[i] != 0;
                g_assert_cmphex(tested[i], ==, 0);
            } else {
                g_assert_cmphex(tested[i], ==, plain[i]);
            }
        }
    }
    /* The triangle does reach the covered window. */
    g_assert_cmpuint(covered_drawn, >, 0);

    qts = board_start(" -global sgi-odyssey.x-mesa-renderer=on");
    have_mesa = !odsy_qom_get_bool(qts, "x-mesa-unavailable");
    if (!have_mesa && g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_error("QEMU_TEST_SGI_REQUIRE_MESA is set, but the device found "
                "no Mesa renderer");
    }
    id_compare_triangle(qts, true, mesa);
    if (have_mesa) {
        g_assert_cmpuint(odsy_mesa_fallbacks(qts, "window"), ==, 1);
        g_assert_cmpuint(odsy_qom_get_uint(qts, "x-mesa-draws"), ==, 0);
    }
    board_quit(qts);
    g_assert_cmpmem(mesa, 128 * 128 * 4, tested, 128 * 128 * 4);
}

static void test_command_fifo_gl_colour_index(void)
{
    QTestState *qts = board_start("");
    uint32_t i;

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID24, ODSY_DISPMODE_GROUP18);
    backend_push32(qts, ODSY_BACKEND_HDR(0x1200, 8));
    for (i = 0; i < 8; i++) {
        backend_push32(qts, ci_lut(i));
    }
    id_plane_write(qts, 24, 0xf8, 256, 256, 384, 384);
    probe_window(qts);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);

    /* glClearIndex(3), glClear: the whole window stores index 3. */
    push_clear(qts, 0x40400000, ODSY_F_ZERO, ODSY_F_ZERO, 0x43000000,
               0x43000000);
    g_assert_cmphex(odsy_test_pixel(qts, 256, 256), ==, 3);
    g_assert_cmphex(odsy_test_pixel(qts, 383, 383), ==, 3);
    g_assert_cmphex(odsy_shown(qts, 256, 256), ==, ci_shown(3));
    g_assert_cmphex(odsy_test_pixel(qts, 384, 300), ==, 0);

    /* glIndexi(5): an RGBA colour word in between changes nothing. */
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 5);
    user_cmd_push(qts, ODSY_GL_INDEX_MODE_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x3f800000);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, 5);
    g_assert_cmphex(odsy_shown(qts, 317, 333), ==, ci_shown(5));
    g_assert_cmphex(odsy_test_pixel(qts, 260, 260), ==, 3);

    /* glIndexf(2.0). */
    user_cmd_push(qts, ODSY_GL_INDEX_F);
    user_cmd_push(qts, 0x40000000);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, 2);
    g_assert_cmphex(odsy_shown(qts, 317, 333), ==, ci_shown(2));

    /* Out-of-range floats wrap to twelve bits: -1.0 and 1e20. */
    user_cmd_push(qts, ODSY_GL_INDEX_F);
    user_cmd_push(qts, 0xbf800000);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, 0xfff);
    user_cmd_push(qts, ODSY_GL_INDEX_F);
    user_cmd_push(qts, 0x60ad78ec);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, 0);

    /* The replay forms. */
    user_cmd_push(qts, ODSY_GL_REPLAY_INDEX);
    user_cmd_push(qts, 7);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, 7);
    user_cmd_push(qts, ODSY_GL_REPLAY_INDEX_F);
    user_cmd_push(qts, 0x40800000);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, 4);

    /* RGBA mode: the same triangle stores its colour, not the index. */
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_A);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_B);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0x00000000);
    user_cmd_push(qts, 0x00000000);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 333), ==, ODSY_PLANE_RED);

    board_quit(qts);
}

/*
 * Index write mask: 0x0452 bits 11:0, alongside the colour and depth write
 * masks the same register already carries. No retained capture shows what a
 * real colour-index drawable sends there, and the board's own reset value
 * leaves the bits at 0, not GL's own all-ones "fully open" mask, so
 * odyssey_gl_store_colour() decodes the field and reports whenever it reads
 * as anything but all-ones rather than guess at a masking rule no evidence
 * supports. Two known indices drawn under a non-all-ones mask both still land
 * unmasked, confirming the gap is reported, not silently applied as a real
 * mask would be.
 */
#define ODSY_WRITE_INDEX_MASK 0xfff

static char *index_write_mask_log(uint32_t mask, uint32_t a, uint32_t b)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-index-mask-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    probe_window(qts);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW | mask);

    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, a);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, a);

    /* A second draw under the same mask: the index is still unmasked. */
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, b);
    push_bare_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 317, 334), ==, b);

    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    return log;
}

static void test_command_fifo_gl_colour_index_write_mask(void)
{
    g_autofree char *unmasked =
        index_write_mask_log(ODSY_WRITE_INDEX_MASK, 10, 3);
    g_autofree char *partial = index_write_mask_log(0x0f0, 10, 3);

    g_assert_cmpuint(count_substr(unmasked, "index write mask"), ==, 0);
    g_assert_cmpuint(count_substr(partial,
                      "index write mask 0x0f0 is decoded but not applied"),
                      ==, 1);
}

static void push_integer_quad(QTestState *qts, uint32_t x0, uint32_t y0,
                              uint32_t x1, uint32_t y1)
{
    const uint32_t v[][2] = { { x0, y0 }, { x1, y0 },
                             { x1, y1 }, { x0, y1 } };
    unsigned i;

    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_CMD_GL_QUADS);
    for (i = 0; i < ARRAY_SIZE(v); i++) {
        user_cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
        user_cmd_push(qts, v[i][0]);
        user_cmd_push(qts, v[i][1]);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

/* Float and byte RGB share current colour across integer vertices. */
static void test_command_fifo_gl_float_colour_integer_vertices(void)
{
    QTestState *qts = board_start("");
    const uint32_t clip[][4] = { { 312, 212, 347, 227 } };

    window_origin(qts, 300, 200);
    window_clip(qts, clip, ARRAY_SIZE(clip));

    /* The explicit index-mode RGB word preserves the preceding index. */
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 7);
    user_cmd_push(qts, ODSY_GL_INDEX_MODE_COLOR_3F);
    user_cmd_push(qts, 0x3e800000);
    user_cmd_push(qts, 0x3f000000);
    user_cmd_push(qts, 0x3f400000);
    push_integer_quad(qts, 42, 10, 50, 30);
    g_assert_cmphex(odsy_test_pixel(qts, 342, 212), ==, 7);
    g_assert_cmphex(odsy_test_pixel(qts, 347, 227), ==, 7);
    g_assert_cmphex(odsy_test_pixel(qts, 348, 220), ==, 0);

    /* Saved index and RGB are both replayed, even for an index context. */
    user_cmd_push(qts, ODSY_GL_REPLAY_INDEX);
    user_cmd_push(qts, 11);
    user_cmd_push(qts, ODSY_GL_COLOR_3I);
    user_cmd_push(qts, ODSY_I32_ONE);
    user_cmd_push(qts, 0x3fffffff);
    user_cmd_push(qts, 0x1fffffff);
    push_integer_quad(qts, 42, 10, 50, 30);
    g_assert_cmphex(odsy_test_pixel(qts, 342, 212), ==, 11);
    g_assert_cmphex(odsy_test_pixel(qts, 347, 227), ==, 11);

    user_cmd_push(qts, ODSY_GL_MODE_RGBA_A);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_B);
    rgb_fill(qts, 64, 128, 191, 10, 10, 20, 30);
    g_assert_cmphex(odsy_test_pixel(qts, 312, 212), ==, 0x4080bfff);
    g_assert_cmphex(odsy_test_pixel(qts, 319, 227), ==, 0x4080bfff);
    g_assert_cmphex(odsy_test_pixel(qts, 311, 220), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 315, 228), ==, 0);

    /* A stale index must not supply the following RGB quad's colour. */
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 5);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3e800000);
    user_cmd_push(qts, 0x3f000000);
    user_cmd_push(qts, 0x3f400000);
    push_integer_quad(qts, 30, 10, 40, 30);
    g_assert_cmphex(odsy_test_pixel(qts, 330, 212), ==, 0x4080bfff);
    g_assert_cmphex(odsy_test_pixel(qts, 339, 227), ==, 0x4080bfff);
    g_assert_cmphex(odsy_test_pixel(qts, 335, 211), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 335, 228), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 30, 10), ==, 0);

    /* RGB updates leave the raw index used by drawable-id writes intact. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID24, ODSY_DISPMODE_GROUP18);
    backend_push64(qts, 0x34814001u, 0x20040080u);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 24 << 3);
    user_cmd_push(qts, ODSY_GL_COLOR_3I);
    user_cmd_push(qts, ODSY_I32_ONE);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    cmd_push(qts, ODSY_CMD_PLANE_MASK);
    cmd_push(qts, 0xf8 << 12);
    cmd_push(qts, 1);
    cmd_push(qts, 0);
    cmd_push(qts, 0xff);
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, ODSY_DEST_ID_PLANES);
    cmd_push(qts, 0);
    push_integer_quad(qts, 30, 10, 40, 30);
    g_assert_cmphex(odsy_test_pixel(qts, 330, 212), ==, 0x4080bfff);
    colour_index_fill(qts, 5, 30, 10, 40, 30);
    g_assert_cmphex(odsy_shown(qts, 330, 212), ==, 0x804020ff);
    g_assert_cmphex(odsy_shown(qts, 339, 227), ==, 0x804020ff);

    board_quit(qts);
}

static void byte_colour(QTestState *qts, uint32_t r, uint32_t g, uint32_t b)
{
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_3UB);
    user_cmd_push(qts, r);
    user_cmd_push(qts, g);
    user_cmd_push(qts, b);
}

static void byte_colour_triangle(QTestState *qts)
{
    static const uint32_t triangle[] = {
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_VERTEX_2F, 0xbf000000, 0xbf000000,
        ODSY_GL_VERTEX_2F, 0x3f000000, 0xbf000000,
        ODSY_GL_VERTEX_2F, 0, 0x3f000000,
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, triangle, ARRAY_SIZE(triangle));
}

/* Byte RGB must reach float vertices and the idle context replay. */
static void test_command_fifo_gl_byte_colour_float_and_replay(void)
{
    const uint64_t p = ODSY_SHADOW_PAGE;
    QTestState *qts = shadow_start("");
    uint32_t integer_pixel, float_pixel, replay_pixel, saved[3], index;
    unsigned i;

    user_cmd_push(qts, ODSY_GL_MODE_RGBA_A);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_B);
    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0x3f800000);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 5);
    byte_colour(qts, 0, 255, 0);

    push_integer_quad(qts, 10, 10, 20, 20);
    integer_pixel = odsy_test_pixel(qts, 271, 271);
    byte_colour_triangle(qts);
    float_pixel = odsy_test_pixel(qts, 320, 320);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    index = qtest_readl(qts, p + ODSY_HW_INDEX_OPERANDS);
    for (i = 0; i < ARRAY_SIZE(saved); i++) {
        saved[i] = qtest_readl(qts, p + ODSY_HW_COLOUR_OPERANDS + 4 * i);
    }
    g_assert_cmphex(qtest_readl(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
    g_assert_cmphex(qtest_readl(qts, p + ODSY_HW_COLOUR_WORD), ==,
                    ODSY_CMD_GL_COLOR_3UB);

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0x3f800000);
    cmd_push(qts, qtest_readl(qts, p + ODSY_HW_INDEX_WORD));
    cmd_push(qts, index);
    cmd_push(qts, qtest_readl(qts, p + ODSY_HW_COLOUR_WORD));
    for (i = 0; i < ARRAY_SIZE(saved); i++) {
        cmd_push(qts, saved[i]);
    }
    push_integer_quad(qts, 22, 10, 32, 20);
    replay_pixel = odsy_test_pixel(qts, 283, 271);
    g_test_message("byte RGB integer pixel: 0x%08x", integer_pixel);
    g_test_message("byte RGB float pixel: 0x%08x", float_pixel);
    g_test_message("saved RGB: 0x%08x 0x%08x 0x%08x; raw index: %u",
                   saved[0], saved[1], saved[2], index);
    g_test_message("byte RGB replay integer pixel: 0x%08x", replay_pixel);
    board_quit(qts);

    g_assert_cmphex(integer_pixel, ==, 0x00ff00ff);
    g_assert_cmphex(index, ==, 5);
    g_assert_cmphex(float_pixel, ==, 0x00ff00ff);
    g_assert_cmphex(saved[0], ==, 0);
    g_assert_cmphex(saved[1], ==, 255);
    g_assert_cmphex(saved[2], ==, 0);
    g_assert_cmphex(replay_pixel, ==, 0x00ff00ff);
}

/*
 * Shared byte normalization uses the low byte, independently of 2D packing.
 * The save keeps the setter's word and raw bytes, even out-of-range ones.
 */
static void test_byte_colour_conversion(void)
{
    static const struct {
        uint32_t bytes[3], pixel, packed;
    } cases[] = {
        { { 0, 0, 0 }, 0x000000ff, 0x000000ff },
        { { 255, 255, 255 }, 0xffffffff, 0xffffffff },
        { { 64, 128, 192 }, 0x4080c0ff, 0x4080c0ff },
        { { 192, 64, 128 }, 0xc04080ff, 0xc04080ff },
        { { 0x100, 0x1ff, 0x100 }, 0x00ff00ff, 0x01ff00ff },
    };
    QTestState *qts = shadow_start("");

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        const uint32_t *rgb = cases[i].bytes;

        byte_colour(qts, rgb[0], rgb[1], rgb[2]);
        push_integer_quad(qts, 10, 10, 20, 20);
        g_assert_cmphex(odsy_test_pixel(qts, 271, 271), ==, cases[i].packed);
        byte_colour_triangle(qts);
        g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, cases[i].pixel);
        qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
        g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                                   ODSY_HW_COLOUR_WORD), ==,
                        ODSY_CMD_GL_COLOR_3UB);
        for (unsigned j = 0; j < 3; j++) {
            g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                                       ODSY_HW_COLOUR_OPERANDS + 4 * j), ==,
                            rgb[j]);
        }
    }
    board_quit(qts);
}

/* Index and lit vertices must not resurrect RGB from before a byte update. */
static void test_byte_colour_override(void)
{
    const uint32_t material[] = {
        0xc680d02f, 0, 0, 0, ODSY_F_ONE,
        0xc980d032, 0, 0, ODSY_F_ONE, ODSY_F_ONE,
    };
    QTestState *qts = shadow_start("");

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, ODSY_F_ONE);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 5);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 5);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_A);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_B);
    byte_colour(qts, 0, 255, 0);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 0x00ff00ff);

    push_user_words(qts, material, ARRAY_SIZE(material));
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 0x0000ffff);
    push_program(qts, 0, 0, 0, 0);
    byte_colour(qts, 255, 0, 0);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 0xff0000ff);
    board_quit(qts);
}

/*
 * Index-mode bytes retain the existing packed draw. The save keeps the last
 * colour setter, here the bytes, as sent.
 */
static void test_byte_colour_index_guard(void)
{
    QTestState *qts = shadow_start("");

    user_cmd_push(qts, ODSY_GL_COLOR_3F);
    user_cmd_push(qts, ODSY_F_ONE);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 11);
    byte_colour(qts, 0, 255, 0);
    push_integer_quad(qts, 10, 10, 20, 20);
    g_assert_cmphex(odsy_test_pixel(qts, 271, 271), ==, 0x00ff00ff);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 11);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_HW_INDEX_OPERANDS), ==, 11);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_HW_COLOUR_WORD), ==, ODSY_CMD_GL_COLOR_3UB);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_HW_COLOUR_OPERANDS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_HW_COLOUR_OPERANDS + 4), ==, 255);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_HW_COLOUR_OPERANDS + 8), ==, 0);

    cmd_push(qts, ODSY_GL_REPLAY_INDEX);
    cmd_push(qts, 11);
    cmd_push(qts, ODSY_GL_COLOR_3I);
    cmd_push(qts, ODSY_I32_ONE);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
    push_integer_quad(qts, 10, 10, 20, 20);
    g_assert_cmphex(odsy_test_pixel(qts, 271, 271), ==, 11);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 11);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_A);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_B);
    byte_colour_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 320, 320), ==, 0xff0000ff);
    board_quit(qts);
}

/* Byte RGB changes neither the drawable-id source nor an id write's plane. */
static void test_byte_colour_drawable_id(void)
{
    QTestState *qts = shadow_start("");

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID24, ODSY_DISPMODE_GROUP18);
    backend_push64(qts, 0x34814001u, 0x20040080u);
    user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
    user_cmd_push(qts, 24 << 3);
    byte_colour(qts, 0, 255, 0);
    push_integer_quad(qts, 10, 10, 20, 20);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                               ODSY_HW_INDEX_OPERANDS), ==, 24 << 3);
    g_assert_cmphex(odsy_test_pixel(qts, 271, 271), ==, 0x00ff00ff);
    cmd_push(qts, ODSY_CMD_PLANE_MASK);
    cmd_push(qts, 0xf8 << 12);
    cmd_push(qts, 1);
    cmd_push(qts, 0);
    cmd_push(qts, 0xff);
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, ODSY_DEST_ID_PLANES);
    cmd_push(qts, 0);
    push_integer_quad(qts, 10, 10, 20, 20);
    g_assert_cmphex(odsy_test_pixel(qts, 271, 271), ==, 0x00ff00ff);
    qtest_quit(qts);
}

/*
 * The shared eight-bit channel model clamps, then rounds half up. A three-int
 * colour is first decoded as the GL library's query does, ((float)v * 2 + 1)
 * / (2^32 - 2304), so 0x3f800000 is 0.496 there, not 1.0.
 */
static void test_command_fifo_gl_float_colour_integer_conversion(void)
{
    static const struct {
        uint32_t tag, rgb[3];
        uint32_t pixel;
    } cases[] = {
        { ODSY_GL_COLOR_3F, { 0x7fc12345, 0xff800000, 0x7f800000 },
          0x0000ffff },
        { ODSY_GL_COLOR_3F, { 0xbf800000, 0x40000000, 0x3f000000 },
          0x00ff80ff },
        { ODSY_GL_COLOR_3F, { 0x3e800000, 0x3f000000, 0x3f400000 },
          0x4080bfff },
        { ODSY_GL_COLOR_3F, { 0x80000000, 0x00000001, 0x3f800000 },
          0x0000ffff },
        { ODSY_GL_COLOR_3I, { ODSY_I32_ONE, 0x3fffffff, 0 }, 0xff8000ff },
        { ODSY_GL_COLOR_3I, { 0x80000000, 0x3f800000, 0x7ffffb00 },
          0x007fffff },
    };
    QTestState *qts = board_start("");
    unsigned j, k;

    window_origin(qts, 300, 200);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_A);
    user_cmd_push(qts, ODSY_GL_MODE_RGBA_B);
    for (j = 0; j < ARRAY_SIZE(cases); j++) {
        uint32_t x = 10 + j * 8, y = 10;

        user_cmd_push(qts, ODSY_CMD_GL_COLOR_INDEX);
        user_cmd_push(qts, 5);
        user_cmd_push(qts, cases[j].tag);
        for (k = 0; k < ARRAY_SIZE(cases[j].rgb); k++) {
            user_cmd_push(qts, cases[j].rgb[k]);
        }
        push_integer_quad(qts, x, y, x + 4, y + 4);
        g_assert_cmphex(odsy_test_pixel(qts, 300 + x, 200 + y),
                        ==, cases[j].pixel);
        g_assert_cmphex(odsy_test_pixel(qts, 303 + x, 203 + y),
                        ==, cases[j].pixel);
    }
    board_quit(qts);
}

static void context_switch_to(QTestState *qts, uint32_t id)
{
    qtest_writel(qts, ODSY_SHADOW_PAGE + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 UINT64_C(0x0000070000000001) | (uint64_t)id << 16);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                                ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
}

/* Synthetic halfwords exercise the unsupported uploaded-program path. */
static void push_synthetic_program_entry(QTestState *qts, uint32_t value)
{
    unsigned i;

    user_cmd_push(qts, ODSY_PROG(0x27, 0x738));
    for (i = 0; i < 6; i++) {
        user_cmd_push(qts, ODSY_PROG(0x40, value + i));
    }
}

static void test_command_fifo_gl_program_window_unknown(void)
{
    static const uint32_t list[] = { 0x49a, 0x203, 0x50f, 0x4c5 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, ODSY_SHADOW_PAGE);
    context_switch_to(qts, 1);

    push_synthetic_program_entry(qts, 0);
    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x738, 0x760, 0x787);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    /* A second arbitrary body in another context must also stay unlit. */
    context_switch_to(qts, 2);
    push_synthetic_program_entry(qts, 0x100);
    push_program(qts, 0, 0x738, 0x760, 0x787);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    context_switch_to(qts, 1);
    push_program(qts, 0, 0x738, 0x760, 0x787);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_cmpuint(count_substr(log, "is not recognised"), ==, 1);
    g_assert_nonnull(strstr(log, "uploaded program is unsupported"));
    g_assert_null(strstr(log, "texture-mode lighting finish"));
}

/*
 * Upload a whole window body: count entries from 0x738, each six halfwords
 * in image order, sent last halfword first as the GL library sends them.
 */
static void push_window_body(QTestState *qts, const uint16_t *h,
                             unsigned count)
{
    user_cmd_push(qts, ODSY_PROG(0x27, 0x738));
    for (unsigned e = 0; e < count; e++) {
        if (e) {
            user_cmd_push(qts, ODSY_PROG(0x27, 0x738 + e));
        }
        for (int i = 5; i >= 0; i--) {
            uint16_t v = h[e * 6 + i];

            user_cmd_push(qts, 0x40000000u | (uint32_t)(v >> 10) << 23 |
                               0x8000u | (v & 0x3ffu));
        }
    }
}

/*
 * A window program is lit only when its entries arrive with exactly the
 * body the GL library uploads for a lit precompiled record. The test cannot
 * carry the shipped body, so it uploads a synthetic 199-entry body, zero
 * but for its last four halfwords, chosen so its CRC-64/ECMA-182 equals
 * record 23's (glfalls' two-light record, entries 738 760 787). That
 * window with record 23's entries lights light.c's triangle as record 3
 * does; one changed halfword, or record 23's body under another record's
 * entries, leaves it unlit in the current red.
 */
static void test_command_fifo_gl_program_window_record(void)
{
    static const uint32_t list[] = { 0x49a, 0x203, 0x50f, 0x4c5 };
    static uint16_t body[199 * 6];
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    body[199 * 6 - 4] = 0xef1b;
    body[199 * 6 - 3] = 0x0790;
    body[199 * 6 - 2] = 0x2469;
    body[199 * 6 - 1] = 0x0015;
    push_window_body(qts, body, 199);
    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x738, 0x760, 0x787);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xfe7f3fffu);

    /* Record 17's entries over record 23's body: not a record. */
    push_program(qts, 0, 0x738, 0x75b, 0x77d);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    /* One halfword different: not record 23's body. */
    body[0] = 1;
    push_window_body(qts, body, 1);
    push_program(qts, 0, 0x738, 0x760, 0x787);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xff0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_nonnull(strstr(log, "uploaded program is unsupported"));
}

/*
 * Record 18, a local-viewer window record: light2.c's lights and material
 * under record 18's entries 738 75b 77d and a synthetic body whose last four
 * halfwords give record 18's CRC. It lights with the local viewer's half
 * vector, the pixel the resident local-viewer test expects, not the
 * infinite viewer's 0xd4663bff.
 */
static void test_command_fifo_gl_program_window_local_viewer(void)
{
    static const uint32_t list[] = { 0x49a, 0x203, 0x50f, 0x4c5 };
    static uint16_t body[199 * 6];
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    body[199 * 6 - 4] = 0x4659;
    body[199 * 6 - 3] = 0xf8c6;
    body[199 * 6 - 2] = 0xc9bc;
    body[199 * 6 - 1] = 0xb12a;
    push_user_words(qts, odsy_light2_data, ARRAY_SIZE(odsy_light2_data));
    push_window_body(qts, body, 199);
    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x738, 0x75b, 0x77d);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), ==, 0xc55429ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "geometry program"));
}

/*
 * The combined matrix already carries the model-view (glfalls' probe):
 * a model-view loaded into slot 0x24 does not move geometry again. Under
 * the probe window's combined matrix and a model-view translating x by 1,
 * the probe's triangle stays where the combined matrix alone puts it --
 * (281, 364) is covered -- and not 64 pixels right, where (376, 356)
 * would be.
 */
#define ODSY_GL_MATRIX_SELECT_MODELVIEW 0x34008024

static void test_command_fifo_gl_combined_carries_modelview(void)
{
    static const uint32_t translate_x[16] = {
        0x3f800000, 0, 0, 0x3f800000,
        0, 0x3f800000, 0, 0,
        0, 0, 0x3f800000, 0,
        0, 0, 0, 0x3f800000,
    };
    QTestState *qts = board_start("");
    unsigned i;

    probe_window(qts);
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_MODELVIEW);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    for (i = 0; i < 16; i++) {
        user_cmd_push(qts, translate_x[i]);
    }
    push_captured_triangle(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 281, 364), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 376, 356), ==, 0);

    board_quit(qts);
}

/*
 * A clip plane arrives in eye coordinates and the model-view that maps object
 * coordinates there arrives in slot 0x24 (mirrors' mirror frames). Under the
 * probe window, where object x -1 to 1 spans window x 256 to 383, and a
 * model-view translating x by 0.5, eye plane (1, 0, 0, 0) keeps object
 * x >= -0.5, window x 288 and right: (300, 340) is drawn and (270, 340) is
 * not. A later model-view translating x by -0.5 moves the same eye plane to
 * object x >= 0.5, window x 352 and right, without a new plane load. Taken
 * as object-space planes, both would keep window x 320 and right.
 */
static void load_modelview(QTestState *qts, const uint32_t m[16])
{
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_MODELVIEW);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    for (unsigned i = 0; i < 16; i++) {
        user_cmd_push(qts, m[i]);
    }
}

static void test_command_fifo_gl_clip_plane_eye_space(void)
{
    static const uint32_t right_half[16] = {
        0x3f800000, 0, 0, 0x3f000000,
        0, 0x3f800000, 0, 0,
        0, 0, 0x3f800000, 0,
        0, 0, 0, 0x3f800000,
    };
    static const uint32_t left_half[16] = {
        0x3f800000, 0, 0, 0xbf000000,
        0, 0x3f800000, 0, 0,
        0, 0, 0x3f800000, 0,
        0, 0, 0, 0x3f800000,
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    load_modelview(qts, right_half);
    clip_plane_load(qts, 0, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_TEX_MODE, 0x4);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 270, 340), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 300, 340), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_YELLOW);

    load_modelview(qts, left_half);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_full_window_tris(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 360, 340), ==, ODSY_PLANE_YELLOW);

    board_quit(qts);
}

/*
 * The eye-space plane and the model-view survive migration: the destination
 * clips as the source would, keeping window x 288 and right.
 */
static void test_command_fifo_gl_clip_plane_migration(void)
{
    static const uint32_t right_half[16] = {
        0x3f800000, 0, 0, 0x3f000000,
        0, 0x3f800000, 0, 0,
        0, 0, 0x3f800000, 0,
        0, 0, 0, 0x3f800000,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-gl-clip-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    load_modelview(from, right_half);
    clip_plane_load(from, 0, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO,
                    ODSY_F_ZERO);
    set_reg(from, ODSY_REG_TEX_MODE, 0x4);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    probe_window(to);
    push_depth_clear(to, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO);
    push_full_window_tris(to, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO);
    g_assert_cmphex(odsy_test_pixel(to, 270, 340), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(to, 300, 340), ==, ODSY_PLANE_YELLOW);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * Main id 0 is an ordinary drawable id: a pixel whose id byte is 0 shows
 * through drawable 0's own display mode. The X server allocates id 0 like
 * any other and gives ids 0 to 3 the modes 0x4b5f00 to 0x4b5f30, groups 16
 * to 19; after a long session searchbook's StaticColor windows took id 0
 * with their colormap in group 16, and showed black through drawable 1's
 * group 17. Here drawable 1 names group 17 (0x1100, index 2 blue) and
 * drawable 0 group 16 (0x1000, index 2 white); an index-2 pixel over id 0
 * shows white.
 */
static void test_scanout_main_id_0_uses_its_own_mode(void)
{
    QTestState *qts = board_start("");

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_HDR(0x2900, 1));
    backend_push32(qts, 0x004b5f00);
    backend_push32(qts, ODSY_BACKEND_HDR(0x1002, 1));
    backend_push32(qts, ODSY_LUT_RED | ODSY_LUT_GREEN | ODSY_LUT_BLUE);

    odsy_test_pixel_write(qts, 100, 100, 2);
    g_assert_cmphex(odsy_shown(qts, 100, 100), ==, 0xffffffff);

    board_quit(qts);
}

/*
 * Original synthetic checks of the proposed shape-4 rectangle policy.
 * Enable and anchoring are model choices, not measured hardware behavior.
 */
static uint32_t bitmap_rect_float(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void bitmap_rect(QTestState *qts, float r, float g, float b,
                        float x0, float y0, float x1, float y1)
{
    const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS, ODSY_GL_RECT,
        bitmap_rect_float(r), bitmap_rect_float(g), bitmap_rect_float(b),
        ODSY_F_ONE,
        bitmap_rect_float(x0), bitmap_rect_float(y0), ODSY_DEPTH_2S,
        bitmap_rect_float(x1), bitmap_rect_float(y1), ODSY_DEPTH_2S,
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void bitmap_rect_pattern(QTestState *qts, const uint32_t words[32],
                                uint32_t mode)
{
    pattern_load(qts, words);
    set_reg(qts, ODSY_CMD_PATTERN_SHAPE, ODSY_PATTERN_SHAPE_32X32);
    set_reg(qts, ODSY_REG_GL_MODE, mode);
}

static void test_command_fifo_gl_bitmap_rect_reverse_y(void)
{
    const uint32_t pattern[32] = {
        [27] = 1u << 31,
        [31] = 1u << 26,
    };
    QTestState *qts = board_start("");
    unsigned x, y;

    bitmap_rect(qts, 0, 0, 1, 10, 35, 18, 40);
    bitmap_rect_pattern(qts, pattern, 0x10);
    bitmap_rect(qts, 1, 0, 0, 10, 40, 18, 35);

    for (y = 34; y <= 40; y++) {
        for (x = 9; x <= 18; x++) {
            uint32_t expected = 0;

            if (x >= 10 && x < 18 && y >= 35 && y < 40) {
                expected = (x == 10 && y == 35) || (x == 15 && y == 39)
                           ? ODSY_PLANE_RED : ODSY_PLANE_BLUE;
            }
            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==, expected);
        }
    }
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_disabled(void)
{
    const uint32_t empty[32] = { 0 };
    const uint32_t modes[] = { 0, 0x08 };
    QTestState *qts = board_start("");
    unsigned i, x, y;

    for (i = 0; i < ARRAY_SIZE(modes); i++) {
        set_reg(qts, ODSY_REG_GL_MODE, 0);
        bitmap_rect(qts, 0, 0, 1, 10, 35, 18, 40);
        bitmap_rect_pattern(qts, empty, modes[i]);
        bitmap_rect(qts, 1, 0, 0, 10, 40, 18, 35);
        for (y = 35; y < 40; y++) {
            for (x = 10; x < 18; x++) {
                g_assert_cmphex(odsy_test_pixel(qts, x, y), ==,
                                ODSY_PLANE_RED);
            }
        }
    }
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_origin_clip(void)
{
    const uint32_t pattern[32] = {
        [27] = 1u << 31,
        [31] = 1u << 26,
    };
    const uint32_t clip[][4] = { { 314, 235, 317, 239 } };
    QTestState *qts = board_start("");
    unsigned x, y;

    window_origin(qts, 300, 200);
    bitmap_rect(qts, 0, 0, 1, 10, 35, 18, 40);
    window_clip(qts, clip, ARRAY_SIZE(clip));
    bitmap_rect_pattern(qts, pattern, 0x10);
    bitmap_rect(qts, 1, 0, 0, 10, 40, 18, 35);

    for (y = 235; y < 240; y++) {
        for (x = 310; x < 318; x++) {
            uint32_t expected = x == 315 && y == 239
                                ? ODSY_PLANE_RED : ODSY_PLANE_BLUE;

            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==, expected);
        }
    }
    g_assert_cmphex(odsy_test_pixel(qts, 10, 35), ==, 0);
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_wrap_zero(void)
{
    const uint32_t pattern[32] = {
        [0] = 1u << 31,
        [1] = 1u << 29,
    };
    const uint32_t empty[32] = { 0 };
    QTestState *qts = board_start("");
    uint32_t scan[70 * 34];
    unsigned x, y;

    bitmap_rect(qts, 0, 0, 1, 10, 35, 80, 69);
    bitmap_rect_pattern(qts, pattern, 0x10);
    bitmap_rect(qts, 1, 0, 0, 10, 35, 80, 69);
    odsy_test_pixels(qts, 10, 35, 70, 34, scan);
    for (y = 35; y < 69; y++) {
        for (x = 10; x < 80; x++) {
            unsigned row = (y - 35) % 32;
            unsigned col = (x - 10) % 32;
            uint32_t expected = (row == 0 && col == 0) ||
                                (row == 1 && col == 2)
                                ? ODSY_PLANE_RED : ODSY_PLANE_BLUE;

            g_assert_cmphex(scan[(y - 35) * 70 + x - 10], ==, expected);
        }
    }
    bitmap_rect_pattern(qts, empty, 0x10);
    bitmap_rect(qts, 0, 1, 0, 10, 35, 80, 69);
    /* A zero pattern preserves both set and clear colour locations. */
    g_assert_cmphex(odsy_test_pixel(qts, 10, 35), ==, ODSY_PLANE_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 35), ==, ODSY_PLANE_BLUE);
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_unsupported(void)
{
    const uint32_t pattern[32] = { [0] = UINT32_MAX };
    const float corners[][4] = {
        { 10.5f, 35, 18, 40 },
        { 10, 35.5f, 18, 40 },
        { 10, 35, 18.5f, 40 },
        { 10, 35, 18, 40.5f },
        { -INFINITY, 35, 18, 40 },
        { 10, -INFINITY, 18, 40 },
        { 10, 35, INFINITY, 40 },
        { 10, 35, 18, NAN },
    };
    const uint32_t shapes[] = { 0, 5, 6, UINT32_MAX };
    QTestState *qts = board_start("");
    unsigned i, x, y;

    bitmap_rect(qts, 0, 0, 1, 10, 35, 18, 40);
    bitmap_rect_pattern(qts, pattern, 0x18);
    for (i = 0; i < ARRAY_SIZE(corners); i++) {
        bitmap_rect(qts, 1, 0, 0, corners[i][0], corners[i][1],
                    corners[i][2], corners[i][3]);
    }
    for (i = 0; i < ARRAY_SIZE(shapes); i++) {
        set_reg(qts, ODSY_CMD_PATTERN_SHAPE, shapes[i]);
        bitmap_rect(qts, 1, 0, 0, 10, 35, 18, 40);
    }
    for (y = 35; y < 40; y++) {
        for (x = 10; x < 18; x++) {
            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==,
                            ODSY_PLANE_BLUE);
        }
    }
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_large_anchor(void)
{
    const uint32_t pattern[32] = { [3] = 1u << 29 };
    QTestState *qts = board_start("");

    bitmap_rect(qts, 0, 0, 1, 0, 0, 8, 8);
    bitmap_rect_pattern(qts, pattern, 0x18);
    /* Large integral float anchors remain zero modulo 32. */
    bitmap_rect(qts, 1, 0, 0, -0x1p100f, -0x1p100f, 8, 8);
    g_assert_cmphex(odsy_test_pixel(qts, 2, 3), ==, ODSY_PLANE_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 3, 3), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 2, 4), ==, ODSY_PLANE_BLUE);
    board_quit(qts);
}

/* An original pixel-coordinate transform and triangle; no geometry program. */
static void bitmap_rect_depth_window(QTestState *qts)
{
    const uint32_t matrix[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, ODSY_F_ONE, 0, 0,
        0, 0, bitmap_rect_float(0x1.ffff8p31f),
        bitmap_rect_float(0x1.ffff8p31f),
        0, 0, 0, ODSY_F_ONE,
    };

    push_window_transform(qts, matrix);
    push_block_16(qts, ODSY_F_ONE);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, 0, 0, ODSY_F_ONE, ODSY_DEPTH_2S);
}

static void bitmap_rect_depth_zero(QTestState *qts)
{
    const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS, ODSY_GL_RECT,
        ODSY_F_ONE, 0, 0, ODSY_F_ONE,
        0, 0, 0, bitmap_rect_float(32), bitmap_rect_float(32), 0,
        ODSY_CMD_GL_END,
    };

    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DEPTH_ONLY);
    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void bitmap_rect_depth_triangle(QTestState *qts)
{
    const uint32_t vertices[9] = {
        bitmap_rect_float(8), bitmap_rect_float(8), 0,
        bitmap_rect_float(30), bitmap_rect_float(8), 0,
        bitmap_rect_float(8), bitmap_rect_float(30), 0,
    };

    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, vertices);
}

static void test_command_fifo_gl_bitmap_rect_zero_preserves_depth(void)
{
    const uint32_t empty[32] = { 0 };
    QTestState *qts = board_start("");

    bitmap_rect_depth_window(qts);
    bitmap_rect_pattern(qts, empty, 0x10);
    bitmap_rect_depth_zero(qts);
    bitmap_rect_depth_triangle(qts);
    /* Depth 0.5 passes the original depth 1, but would fail a depth-0 clear. */
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, ODSY_PLANE_YELLOW);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 12), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_depth_only(void)
{
    const uint32_t pattern[32] = { [12] = 1u << 19 };
    QTestState *qts = board_start("");

    bitmap_rect_depth_window(qts);
    bitmap_rect_pattern(qts, pattern, 0x18);
    bitmap_rect_depth_zero(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 12), ==, ODSY_PLANE_BLUE);
    bitmap_rect_depth_triangle(qts);
    /* Only the admitted pattern bit has depth 0 and rejects the triangle. */
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 12), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_index(void)
{
    const uint32_t pattern[32] = {
        [27] = 1u << 31,
        [31] = 1u << 26,
    };
    QTestState *qts = board_start("");
    unsigned x, y;

    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    bitmap_rect(qts, 3, 0, 0, 10, 35, 18, 40);
    bitmap_rect_pattern(qts, pattern, 0x10);
    bitmap_rect(qts, 5.75f, 0, 0, 10, 40, 18, 35);
    for (y = 35; y < 40; y++) {
        for (x = 10; x < 18; x++) {
            uint32_t expected = (x == 10 && y == 35) || (x == 15 && y == 39)
                                ? 5 : 3;

            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==, expected);
        }
    }
    board_quit(qts);
}

/* Original texels: row zero is the lowest T, with padded object rows. */
static void texture_adapter_upload(QTestState *qts, uint32_t base,
                                    const uint32_t pixels[4])
{
    upload_begin(qts, base, 2, 0x080024d0, 2, 2);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, pixels, 4);
    upload_end(qts);
}

static void texture_adapter_upload_detail_dimensions(QTestState *qts,
                                                      uint32_t base,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      const uint32_t *pixels)
{
    upload_begin(qts, base, 2, 0x080024d0, width, height);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(width * height,
                                        ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, pixels, width * height);
    upload_end(qts);
}

static void texture_adapter_upload_detail_function(QTestState *qts,
                                                    uint32_t value)
{
    uint32_t words[16];

    memset(words, 0, sizeof(words));
    for (unsigned block = 0; block < 16; block++) {
        for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
            words[i] = value;
        }
        user_cmd_push(qts, 0x00013e40 | block);
        push_user_words(qts, words, ARRAY_SIZE(words));
    }
}

static void texture_adapter_bind_env(QTestState *qts, uint32_t base,
                                      uint32_t sampler,
                                      const uint32_t env[5])
{
    const uint32_t words[] = {
        0x00010405, 0x10,
        0x00011c16, 0x08000420, 2, 2, 1, sampler, 0, 0,
        0x0001141b, env[0], env[1], env[2], env[3], env[4],
        0x00010e80, base, 0, 0,
        0x00010406, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_bind_border(QTestState *qts, uint32_t base,
                                         uint32_t width_word,
                                         uint32_t height_word,
                                         uint32_t border_rg,
                                         uint32_t border_ba)
{
    const uint32_t words[] = {
        0x00010405, 0x10,
        0x00011c16, 0x08000420, width_word, height_word, 1,
        0x13003c00, border_rg, border_ba,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, base, 0, 0,
        0x00010406, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_bind(QTestState *qts, uint32_t base,
                                  uint32_t sampler, uint32_t env)
{
    const uint32_t words[] = { env, 0, 0, 0, 0 };

    texture_adapter_bind_env(qts, base, sampler, words);
}

static void texture_adapter_point(QTestState *qts, int x, int y,
                                   float s, float t)
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, 0,
        ODSY_CMD_GL_BEGIN, 0,
        0xc380c821, bitmap_rect_float(s), bitmap_rect_float(t),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 0.5f),
        bitmap_rect_float(y + 0.5f), ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_point_rgba(QTestState *qts, int x, int y,
                                       float s, float t,
                                       const uint32_t colour[4])
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_4F, colour[0], colour[1], colour[2], colour[3],
        ODSY_CMD_GL_BEGIN, 0,
        0xc380c821, bitmap_rect_float(s), bitmap_rect_float(t),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 0.5f),
        bitmap_rect_float(y + 0.5f), ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_upload_3d_slice(QTestState *qts, uint32_t base,
                                             const uint32_t *pixels,
                                             uint32_t width, uint32_t height)
{
    uint32_t count = width * height;

    upload_begin(qts, base, 2, 0x080024d0, width, height);
    upload_first_header(qts, 0x4b);
    for (uint32_t offset = 0; offset < count; ) {
        uint32_t burst = MIN(16, count - offset);

        user_cmd_push(qts, ODSY_IMAGE_BURST(burst, ODSY_IMAGE_FMT_RGB));
        push_user_words(qts, &pixels[offset], burst);
        offset += burst;
    }
    upload_end(qts);
}

static void texture_adapter_bind_3d(QTestState *qts, uint32_t base,
                                    uint32_t height, uint32_t depth,
                                    uint32_t stride_units, uint32_t level_word2,
                                    uint32_t environment)
{
    uint32_t sampler[7] = {
        0x08000000 | (ctz32(2) << 10) | (ctz32(height) << 5) | ctz32(depth),
        2, height, depth, 0x13000000, 0, 0,
    };
    uint32_t level[3] = { base, 0x41100000 | stride_units, level_word2 };
    const uint32_t env[5] = { environment, 0, 0, 0, 0 };
    const uint32_t words[] = {
        0x00010405, 0x20,
        0x00011c16,
        sampler[0], sampler[1], sampler[2], sampler[3],
        sampler[4], sampler[5], sampler[6],
        0x0001141b, env[0], env[1], env[2], env[3], env[4],
        0x00010e80, level[0], level[1], level[2],
        0x00010406, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_point_3d(QTestState *qts, int x, int y,
                                     float s, float t, float r)
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, 0,
        ODSY_CMD_GL_BEGIN, 0,
        0xc380cc23, bitmap_rect_float(s), bitmap_rect_float(t),
        bitmap_rect_float(r),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 0.5f),
        bitmap_rect_float(y + 0.5f), ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_point_3d_rgba(QTestState *qts, int x, int y,
                                          float s, float t, float r,
                                          const uint32_t colour[4])
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_4F, colour[0], colour[1], colour[2], colour[3],
        ODSY_CMD_GL_BEGIN, 0,
        0xc380cc23, bitmap_rect_float(s), bitmap_rect_float(t),
        bitmap_rect_float(r),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 0.5f),
        bitmap_rect_float(y + 0.5f), ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void texture_adapter_upload_scalar(QTestState *qts, uint32_t base)
{
    upload_begin(qts, base, 2, 0x080024d0, 2, 2);
    upload_first_header(qts, 0x42);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, 0x004080c0);
    upload_end(qts);
}

static void texture_adapter_upload_l8_table_input(QTestState *qts,
                                                   uint32_t base)
{
    upload_begin(qts, base, 2, 0x080024d0, 2, 2);
    upload_first_header(qts, 0x42);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, 0x004080ff);
    upload_end(qts);
}

static uint32_t texture_table_index_word(unsigned index)
{
    return (index * 4095 + 127) / 255;
}

static uint32_t texture_table_pio_word(unsigned index, bool plane1,
                                       bool alternate)
{
    uint32_t ramp = texture_table_index_word(index);
    uint32_t red = alternate ? 4095 - ramp : ramp;
    uint32_t green = alternate ? ramp : 4095 - ramp;
    uint32_t blue = !alternate ?
                    (index * index * 4095 + 32512) / (255 * 255) : 0;

    if (plane1) {
        return (blue << 12) | ramp;
    }
    return (red << 12) | green;
}

static void texture_adapter_upload_colour_table(QTestState *qts,
                                                bool alternate)
{
    uint32_t words[16];

    for (unsigned plane = 0; plane < 2; plane++) {
        for (unsigned block = 0; block < 16; block++) {
            unsigned base = 16 * block;

            for (unsigned j = 0; j < ARRAY_SIZE(words); j++) {
                words[j] = texture_table_pio_word(base + j, plane,
                                                  alternate);
            }
            user_cmd_push(qts, 0x00013e00 | (plane << 4) | block);
            push_user_words(qts, words, ARRAY_SIZE(words));
        }
    }
}

static void texture_adapter_upload_rgba8_colour_table(QTestState *qts,
                                                       bool rgba)
{
    uint32_t words[16];

    for (unsigned plane = 0; plane < 2; plane++) {
        for (unsigned block = 0; block < 16; block++) {
            unsigned base = 16 * block;

            for (unsigned j = 0; j < ARRAY_SIZE(words); j++) {
                unsigned index = base + j;
                uint32_t red = texture_table_index_word(index / 2);
                uint32_t green = texture_table_index_word(255 - index);
                uint32_t blue = texture_table_index_word(index * index / 255);
                uint32_t fourth = texture_table_index_word(rgba ?
                                                           255 - index :
                                                           index);

                words[j] = plane ? (blue << 12) | fourth :
                           (red << 12) | green;
            }
            user_cmd_push(qts, 0x00013e00 | (plane << 4) | block);
            push_user_words(qts, words, ARRAY_SIZE(words));
        }
    }
}

static void texture_adapter_window(QTestState *qts);

/*
 * pfCity's volume form: RGBA, linear and not mipmapped (word 4 0x13000022),
 * slices 64 << level word 2 address units apart. A 2x2x4 volume with word 2
 * of 3 puts its slices 0x200 apart, where the replaced rule (row length
 * times word 2 times 64) put them 0x180 apart and found none past the
 * first. A slice centre gives that slice's colour; halfway between slices
 * 0 and 1 gives their mean.
 */
static void test_texture_volume_slice_spacing(void)
{
    static const uint32_t colours[4] = {
        0x400000ff, 0x00c000ff, 0x0000a0ff, 0x808080ff,
    };
    const uint32_t words[] = {
        0x00010405, 0x20,
        0x00011c16, 0x08000000 | (1 << 10) | (1 << 5) | 2, 2, 2, 4,
        0x13000022, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x190000, 0x41100002, 3,
        0x00010406, 0,
    };
    QTestState *qts = board_start("");

    for (unsigned z = 0; z < 4; z++) {
        const uint32_t slice[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(qts, 0x190000 + z * 0x200, slice,
                                        2, 2);
    }
    texture_adapter_window(qts);
    push_user_words(qts, words, ARRAY_SIZE(words));
    for (unsigned z = 0; z < 4; z++) {
        odsy_test_pixel_write(qts, 10 + z, 10, 0x12345678);
        texture_adapter_point_3d(qts, 10 + z, 10, 0.5f, 0.5f,
                                 (z + 0.5f) / 4);
        g_assert_cmphex(odsy_test_pixel(qts, 10 + z, 10), ==, colours[z]);
    }
    texture_adapter_point_3d(qts, 14, 10, 0.5f, 0.5f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, 0x206000ff);
    board_quit(qts);
}

static void texture_adapter_test_rgba8_colour_table(uint32_t sampler,
                                                     bool rgba, bool blend)
{
    static const uint32_t pixels[4] = {
        0xff000000, 0x00ff0040, 0x0000ff80, 0xffffffff,
    };
    static const uint32_t white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    static const float coord[4][2] = {
        { 0.25f, 0.25f }, { 0.75f, 0.25f },
        { 0.25f, 0.75f }, { 0.75f, 0.75f },
    };
    static const uint32_t rgb[4] = {
        0x7fff0000, 0x00000040, 0x00ffff80, 0x7f00ffff,
    };
    static const uint32_t rgba_output[4] = {
        0x7fff00ff, 0x000000bf, 0x00ffff7f, 0x7f00ff00,
    };
    static const uint32_t rgb_blend[4] = {
        0, 0, 0x00808000, 0x7f00ff00,
    };
    static const uint32_t rgba_blend[4] = {
        0x7fff0000, 0, 0x007f7f00, 0,
    };
    const uint32_t *expected = blend ?
        (rgba ? rgba_blend : rgb_blend) : (rgba ? rgba_output : rgb);
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    texture_adapter_upload_rgba8_colour_table(qts, rgba);
    texture_adapter_bind(qts, 0x180000, sampler, 1);
    push_depth_clear(qts, 0, 0, 0, 0);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT |
            (blend ? ODSY_MODE_BLEND : 0));
    if (blend) {
        set_blend(qts, 0x639, 0, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(coord); i++) {
        texture_adapter_point_rgba(qts, 20 + i, 10, coord[i][0],
                                   coord[i][1], white);
        if (blend) {
            g_assert_cmphex(odsy_test_pixel(qts, 20 + i, 10) & 0xffffff00,
                            ==, expected[i]);
        } else {
            g_assert_cmphex(odsy_test_pixel(qts, 20 + i, 10), ==,
                            expected[i]);
        }
    }
    board_quit(qts);
}

static void texture_adapter_upload_luminance_alpha(QTestState *qts,
                                                    uint32_t base,
                                                    const uint32_t rows[2])
{
    upload_begin(qts, base, 2, 0x080024d0, 2, 2);
    for (unsigned y = 0; y < 2; y++) {
        user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
        user_cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x48));
        user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
        user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
        user_cmd_push(qts, rows[y]);
    }
    upload_end(qts);
}

static void texture_adapter_window(QTestState *qts)
{
    const uint32_t identity[16] = {
        ODSY_F_ONE, 0, 0, 0, 0, ODSY_F_ONE, 0, 0,
        0, 0, ODSY_F_ONE, 0, 0, 0, 0, ODSY_F_ONE,
    };

    bitmap_rect_depth_window(qts);
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    push_user_words(qts, identity, ARRAY_SIZE(identity));
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00f00000);
    /*
     * texture_adapter_upload()'s own upload_begin() already sent this
     * same 0x81d register with the object's width and height -- that is
     * its own draw's clip box, per the "arrives with
     * every draw" rule, not a lingering window setting. Restore it to
     * this family's full default window before the point draws below,
     * the same way a real client's own next draw would.
     */
    clip_box_set(qts, 0, 1279, 0, 1023);
}

/* Draw with the retained coordinate, without supplying another one. */
static void texture_adapter_current_point(QTestState *qts, int x, int y)
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, 0,
        ODSY_CMD_GL_BEGIN, 0,
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 0.5f),
        bitmap_rect_float(y + 0.5f), ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

/*
 * A quad of four vertices (x, y, s, t) in one colour, textured with the
 * bound texture, through the client's quad draw.
 */
static void band_textured_quad(QTestState *qts, const uint32_t colour[4],
                               const float v[4][4])
{
    push_user_words(qts, (uint32_t[]) {
        ODSY_GL_COLOR_4F, colour[0], colour[1], colour[2], colour[3],
        ODSY_CMD_GL_BEGIN, 7,
    }, 7);
    for (unsigned i = 0; i < 4; i++) {
        push_user_words(qts, (uint32_t[]) {
            0xc380c821, bitmap_rect_float(v[i][2]),
            bitmap_rect_float(v[i][3]),
            ODSY_GL_VERTEX_2F, bitmap_rect_float(v[i][0]),
            bitmap_rect_float(v[i][1]),
        }, 6);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

/*
 * A 2x2 texture of half-alpha texels over a quad, blended, that writes
 * stencil 1, then a second quad of the green texel drawn only where the
 * stencil is 1; the 64x64 corner they sit in read back. Each quad covers
 * fewer pixels than the rasterizer's own band threshold, so only
 * x-test-raster-band-pixels puts it on band workers. Free the result.
 */
static uint32_t *band_scene(const char *extra, int *workers)
{
    static const uint32_t texels[4] = {
        0xff000080, 0x00ff0080, 0x0000ff80, 0xffff0080,
    };
    static const uint32_t half_white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    static const uint32_t half_green[4] = {
        0, ODSY_F_ONE, 0, 0x3f000000,
    };
    static const float under[4][4] = {
        { 10, 10, 0, 0 }, { 48, 10, 1, 0 }, { 48, 48, 1, 1 },
        { 10, 48, 0, 1 },
    };
    static const float over[4][4] = {
        { 30, 20, 0.75, 0.25 }, { 60, 30, 0.75, 0.25 },
        { 50, 60, 0.75, 0.25 }, { 25, 50, 0.75, 0.25 },
    };
    static const uint32_t identity[16] = {
        ODSY_F_ONE, 0, 0, 0, 0, ODSY_F_ONE, 0, 0,
        0, 0, ODSY_F_ONE, 0, 0, 0, 0, ODSY_F_ONE,
    };
    const uint32_t window[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, ODSY_F_ONE, 0, 0,
        0, 0, bitmap_rect_float(0x1.ffff8p31f),
        bitmap_rect_float(0x1.ffff8p31f),
        0, 0, 0, ODSY_F_ONE,
    };
    QTestState *qts = board_start(extra);
    uint32_t *corner = g_new(uint32_t, 64 * 64);

    texture_adapter_upload(qts, 0x180000, texels);
    /*
     * texture_adapter_window() without its clear, which is large enough
     * to start the band workers whatever the threshold.
     */
    push_window_transform(qts, window);
    push_block_16(qts, ODSY_F_ONE);
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    push_user_words(qts, identity, ARRAY_SIZE(identity));
    clip_box_set(qts, 0, 1279, 0, 1023);
    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND |
            ODSY_MODE_STENCIL_TEST);
    set_blend(qts, 0x639, 0, 0);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0e92ff01);
    band_textured_quad(qts, half_white, under);
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0400ff01);
    band_textured_quad(qts, half_green, over);
    odsy_test_pixels(qts, 0, 0, 64, 64, corner);
    *workers = sgi_raster_band_workers(qts);
    board_quit(qts);
    return corner;
}

/*
 * The scene drawn with every draw in bands (x-test-raster-band-pixels=1)
 * leaves the same pixels as drawn on one thread (x-raster-bands=off),
 * through the texel readers and the blend and stencil callbacks; the
 * blended texels and the stencilled green show, and band workers drew.
 */
static void test_gl_bands_match_one_thread(void)
{
    int workers, serial_workers;
    g_autofree uint32_t *banded =
        band_scene(" -global sgi-odyssey.x-test-raster-band-pixels=1",
                   &workers);
    g_autofree uint32_t *serial =
        band_scene(" -global sgi-odyssey.x-raster-bands=off",
                   &serial_workers);

    for (int i = 0; i < 64 * 64; i++) {
        g_assert_cmphex(banded[i], ==, serial[i]);
    }
    /*
     * Two texels of the under quad blend over the background differently,
     * the green blends again where both quads lie, and where the green
     * quad lies outside the under one the stencil keeps the background.
     */
    g_assert_cmphex(serial[15 * 64 + 15], !=, serial[5 * 64 + 5]);
    g_assert_cmphex(serial[15 * 64 + 15], !=, serial[15 * 64 + 43]);
    g_assert_cmphex(serial[40 * 64 + 40], !=, serial[15 * 64 + 43]);
    g_assert_cmphex(serial[40 * 64 + 40], !=, serial[40 * 64 + 15]);
    g_assert_cmphex(serial[40 * 64 + 55], ==, serial[5 * 64 + 5]);
    if (workers < 0) {
        g_test_skip("needs Linux /proc and two host processors");
        return;
    }
    g_assert_cmpint(workers, >, 0);
    g_assert_cmpint(serial_workers, ==, 0);
}

static void texture_chain_upload_solid(QTestState *qts, uint32_t base,
                                       uint32_t width, uint32_t height,
                                       uint32_t pixel)
{
    uint32_t remaining = width * height;

    upload_begin(qts, base, 2, 0x080024d0, width, height);
    upload_first_header(qts, 0x4b);
    while (remaining) {
        uint32_t count = MIN(remaining, 16);

        user_cmd_push(qts, ODSY_IMAGE_BURST(count, ODSY_IMAGE_FMT_RGB));
        for (uint32_t i = 0; i < count; i++) {
            user_cmd_push(qts, pixel);
        }
        remaining -= count;
    }
    upload_end(qts);
}

static void texture_integer_quad(QTestState *qts, uint32_t x0, uint32_t y0,
                                 uint32_t x1, uint32_t y1, bool one_d)
{
    const uint32_t vertex[][4] = {
        { x0, y0, 0, 0 }, { x1, y0, ODSY_F_ONE, 0 },
        { x1, y1, ODSY_F_ONE, ODSY_F_ONE },
        { x0, y1, 0, ODSY_F_ONE },
    };

    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_CMD_GL_QUADS);
    for (unsigned i = 0; i < ARRAY_SIZE(vertex); i++) {
        user_cmd_push(qts, one_d ? 0xc380c41f : 0xc380c821);
        user_cmd_push(qts, vertex[i][2]);
        if (!one_d) {
            user_cmd_push(qts, vertex[i][3]);
        }
        user_cmd_push(qts, ODSY_CMD_GL_VERTEX_2I);
        user_cmd_push(qts, vertex[i][0]);
        user_cmd_push(qts, vertex[i][1]);
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
}

static void test_texture_chain_integer_quads(void)
{
    const uint32_t binding[] = {
        0x00010405, 0x10,
        0x00011c16, 0x08000840, 4, 4, 1, 0x13000040, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x180000, 0x41100002, 0,
        0x00010e81, 0x181000, 0x41100002, 0,
        0x00010e82, 0x182000, 0x41100002, 0,
        0x00010406, 0,
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    const uint32_t colour[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff,
    };
    const uint32_t flipped[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, 0xbf800000, 0, 0x43000000,
        0, 0, 0x4f7fffc0, 0x4f7fffc0,
        0, 0, 0, ODSY_F_ONE,
    };
    const uint32_t lod0[] = { 0x00011013, 0, 0, 0, 0 };
    const uint32_t clip[][4] = { { 100, 200, 227, 327 } };
    QTestState *qts = board_start("");

    texture_chain_upload_solid(qts, 0x180000, 4, 4, colour[0]);
    texture_chain_upload_solid(qts, 0x181000, 2, 2, colour[1]);
    texture_chain_upload_solid(qts, 0x182000, 1, 1, colour[2]);
    texture_adapter_window(qts);
    odsy_test_pixel_write(qts, 80, 15, 0);
    push_user_words(qts, binding, ARRAY_SIZE(binding));
    for (unsigned i = 0; i < ARRAY_SIZE(colour); i++) {
        const uint32_t lod[] = {
            0x00011013, 0, 0, i << 8, i << 8,
        };
        uint32_t x = 10 + i * 20;

        push_user_words(qts, lod, ARRAY_SIZE(lod));
        texture_integer_quad(qts, x, 10, x + 10, 20, false);
        g_assert_cmphex(odsy_test_pixel(qts, x + 5, 15), ==, colour[i]);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 80, 15), ==, 0);

    /* The GL matrix flips Y inside the placed window; the X path does not. */
    window_origin(qts, 100, 200);
    window_clip(qts, clip, ARRAY_SIZE(clip));
    push_window_transform(qts, flipped);
    push_user_words(qts, lod0, ARRAY_SIZE(lod0));
    odsy_test_pixel_write(qts, 115, 215, 0);
    texture_integer_quad(qts, 10, 10, 20, 20, false);
    g_assert_cmphex(odsy_test_pixel(qts, 115, 313), ==, colour[0]);
    g_assert_cmphex(odsy_test_pixel(qts, 115, 215), ==, 0);
    board_quit(qts);
}

/*
 * A three-level chain (red 4x4, green 2x2, blue 1x1) under nearest-mipmap-
 * nearest, drawn as a quad with S and T running 0 to 1, so each axis's
 * texel-per-pixel ratio is 4 over the quad's width or height. The LOD bias
 * words are 14-bit two's complement with 8 fraction bits and scale their
 * axis's derivatives by 2^bias (GL_SGIX_texture_lod_bias).
 */
static uint32_t lod_bias_quad(QTestState *qts, uint32_t bias_s,
                              uint32_t bias_t, uint32_t min_lod,
                              uint32_t width, uint32_t height)
{
    const uint32_t lod[] = {
        0x00011013, bias_s, bias_t, min_lod, 0x0f00,
    };

    push_user_words(qts, lod, ARRAY_SIZE(lod));
    odsy_test_pixel_write(qts, 10 + width / 2, 10 + height / 2, 0x12345678);
    texture_integer_quad(qts, 10, 10, 10 + width, 10 + height, false);
    return odsy_test_pixel(qts, 10 + width / 2, 10 + height / 2);
}

static void test_texture_lod_bias(void)
{
    const uint32_t binding[] = {
        0x00010405, 0x10,
        0x00011c16, 0x08000840, 4, 4, 1, 0x13000040, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x180000, 0x41100002, 0,
        0x00010e81, 0x181000, 0x41100002, 0,
        0x00010e82, 0x182000, 0x41100002, 0,
        0x00010406, 0,
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    const uint32_t red = 0xff0000ff, green = 0x00ff00ff, blue = 0x0000ffff;
    QTestState *qts = board_start("");

    texture_chain_upload_solid(qts, 0x180000, 4, 4, red);
    texture_chain_upload_solid(qts, 0x181000, 2, 2, green);
    texture_chain_upload_solid(qts, 0x182000, 1, 1, blue);
    texture_adapter_window(qts);
    push_user_words(qts, binding, ARRAY_SIZE(binding));

    /* 10x10 pixels: LOD log2(0.4), about -1.32, magnifies level 0. */
    g_assert_cmphex(lod_bias_quad(qts, 0, 0, 0, 10, 10), ==, red);
    /* 2.875 on both axes: LOD 1.55 rounds to level 2; 2.0 would give 1. */
    g_assert_cmphex(lod_bias_quad(qts, 0x2e0, 0x2e0, 0, 10, 10), ==, blue);
    g_assert_cmphex(lod_bias_quad(qts, 0x200, 0x200, 0, 10, 10), ==, green);
    /*
     * 10 wide, 40 tall: S scales 0.4 and T 0.1 texels a pixel. Biasing S
     * by 2.875 gives LOD 1.55, level 2; the same bias on T reaches only
     * 0.73 texels a pixel and still magnifies.
     */
    g_assert_cmphex(lod_bias_quad(qts, 0x2e0, 0, 0, 10, 40), ==, blue);
    g_assert_cmphex(lod_bias_quad(qts, 0, 0x2e0, 0, 10, 40), ==, red);
    /* 2x2 pixels: LOD 1, level 1; a bias of -1 (0x3f00) brings level 0. */
    g_assert_cmphex(lod_bias_quad(qts, 0, 0, 0, 2, 2), ==, green);
    g_assert_cmphex(lod_bias_quad(qts, 0x3f00, 0x3f00, 0, 2, 2), ==, red);
    /*
     * A minimum LOD past the library's +/-15 (20, 0x1400) clamps to 15 and
     * selects the last level rather than dropping the draw.
     */
    g_assert_cmphex(lod_bias_quad(qts, 0, 0, 0x1400, 10, 10), ==, blue);
    /* Bits above the 14-bit field are unknown state: the draw drops. */
    g_assert_cmphex(lod_bias_quad(qts, 0x4000, 0, 0, 10, 10), ==,
                    0x12345678);
    board_quit(qts);
}

/*
 * An 8x8 image, red in columns 0-3 and green in 4-7, sampled nearest at
 * S = 0.9. Sampler word 1 bits 16:0 hold the S maximum clamp in whole
 * texels (8 for the default 1.0). Under GL_CLAMP a clamp of 3 limits S to
 * 3/8, red; under GL_REPEAT the extension ignores it, green.
 */
static uint32_t max_clamp_point(QTestState *qts, int x, uint32_t wrap,
                                uint32_t clamp_s)
{
    const uint32_t sampler[] = {
        0x00011c16, 0x08000c60, clamp_s, 8, 1,
        0x13000000 | wrap << 12 | wrap << 10, 0, 0,
    };

    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    odsy_test_pixel_write(qts, x, 10, 0x12345678);
    texture_adapter_point(qts, x, 10, 0.9f, 0.5f);
    return odsy_test_pixel(qts, x, 10);
}

static void test_texture_max_clamp(void)
{
    const uint32_t red = 0xff0000ff, green = 0x00ff00ff;
    const uint32_t binding[] = {
        0x00010405, 0x10,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x180000, 0x41100002, 0,
        0x00010406, 0,
        0x00011013, 0, 0, 0, 0,
    };
    uint32_t pixels[64];
    QTestState *qts = board_start("");

    for (unsigned i = 0; i < ARRAY_SIZE(pixels); i++) {
        pixels[i] = i % 8 < 4 ? red : green;
    }
    texture_adapter_upload_3d_slice(qts, 0x180000, pixels, 8, 8);
    texture_adapter_window(qts);
    push_user_words(qts, binding, ARRAY_SIZE(binding));
    g_assert_cmphex(max_clamp_point(qts, 10, 2, 8), ==, green);
    g_assert_cmphex(max_clamp_point(qts, 11, 2, 3), ==, red);
    g_assert_cmphex(max_clamp_point(qts, 12, 0, 3), ==, green);
    board_quit(qts);
}

static void test_texture_chain_explicit_level(void)
{
    QTestState *qts = board_start("");
    const uint32_t words[] = {
        0x00010405, 0x10,
        0x00011c16, 0x08000420, 2, 2, 1, 0x13000040, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x180000, 0x41100002, 0,
        0x00010e81, 0x181000, 0x41100002, 0,
        0x00010406, 0,
        0x00011013, 0, 0, 0x0100, 0x0f00,
    };

    texture_chain_upload_solid(qts, 0x180000, 2, 2, 0xff0000ff);
    texture_chain_upload_solid(qts, 0x181000, 1, 1, 0x0000ffff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 0), ==,
                    0xff0000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 0, 0), ==,
                    0x0000ffff);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0);
    texture_adapter_window(qts);
    odsy_test_pixel_write(qts, 10, 10, 0x12345678);
    push_user_words(qts, words, ARRAY_SIZE(words));
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x0000ffff);
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t sampler[] = {
            0x08000420, 2, 2, 1, 0x13000040 + i * 0x20, 0, 0,
        };

        user_cmd_push(qts, 0x00011c16);
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        texture_adapter_point(qts, 20 + i, 10, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 20 + i, 10), ==, 0x0000ffff);
    }

    /* A zero upper LOD excludes the stale second entry. */
    const uint32_t cap[] = { 0x00011013, 0, 0, 0, 0 };
    push_user_words(qts, cap, ARRAY_SIZE(cap));
    texture_adapter_point(qts, 11, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0xff0000ff);

    /* Missing storage disables texturing; unknown state drops the draw. */
    const uint32_t full[] = { 0x00011013, 0, 0, 0x0100, 0x0100 };
    const uint32_t absent[] = { 0x00010e81, 0, 0x41100000, 0 };
    const uint32_t unknown[] = { 0x00010e80, 0x180000, 0x45100002, 0 };
    push_user_words(qts, full, ARRAY_SIZE(full));
    push_user_words(qts, absent, ARRAY_SIZE(absent));
    texture_adapter_point(qts, 12, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, ODSY_PLANE_YELLOW);
    odsy_test_pixel_write(qts, 13, 10, 0x12345678);
    push_user_words(qts, unknown, ARRAY_SIZE(unknown));
    texture_adapter_point(qts, 13, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), ==, 0x12345678);
    board_quit(qts);
}

static void test_texture_one_dimensional_mode(void)
{
    const uint32_t pixels[4] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint32_t words[] = {
        0x00010405, 0x8,
        0x00011c16, 0x08000800, 4, 1, 1, 0x13000000, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x180000, 0x41100002, 0,
        0x00010406, 0,
    };
    QTestState *qts = board_start("");

    upload_begin(qts, 0x180000, 2, 0x080024d0, 4, 1);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, pixels, ARRAY_SIZE(pixels));
    upload_end(qts);
    texture_adapter_window(qts);
    push_user_words(qts, words, ARRAY_SIZE(words));
    texture_adapter_point(qts, 10, 10, 0.125f, 0.25f);
    texture_adapter_point(qts, 11, 10, 0.375f, 0.75f);
    texture_adapter_point(qts, 12, 10, 0.625f, -3.0f);
    texture_adapter_point(qts, 13, 10, 0.875f, 10.0f);
    for (unsigned i = 0; i < ARRAY_SIZE(pixels); i++) {
        g_assert_cmphex(odsy_test_pixel(qts, 10 + i, 10), ==, pixels[i]);
    }
    texture_integer_quad(qts, 32, 32, 96, 96, true);
    for (unsigned i = 0; i < ARRAY_SIZE(pixels); i++) {
        g_assert_cmphex(odsy_test_pixel(qts, 40 + 16 * i, 48), ==, pixels[i]);
    }
    {
        const uint32_t matrix[16] = {
            ODSY_F_ONE, ODSY_F_ONE, 0, 0,
            0, 0x42c80000, 0, 0,
            0, 0, ODSY_F_ONE, 0,
            0, 0, 0, ODSY_F_ONE,
        };

        user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_TEXTURE);
        user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
        push_user_words(qts, matrix, ARRAY_SIZE(matrix));
        texture_adapter_point(qts, 14, 10, 0.125f, 0.25f);
        texture_adapter_point(qts, 15, 10, 0.125f, 0.75f);
        g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, pixels[1]);
        g_assert_cmphex(odsy_test_pixel(qts, 15, 10), ==, pixels[3]);
    }
    board_quit(qts);
}

static void texture_one_dimensional_clamp(bool linear, bool stored, bool wide)
{
    const uint32_t pixels[4] = {
        0x20c040ff, 0xe02060ff, 0x4040e0ff, 0xc0a020ff,
    };
    const uint32_t wide_pixels[6] = {
        0x1050f0ff, 0xf01020ff, 0x20e050ff,
        0x4030f0ff, 0xe0a010ff, 0xa010c0ff,
    };
    const uint32_t wide_nearest[4] = {
        0xf01020ff, 0x20e050ff, 0xe0a010ff, 0xe0a010ff,
    };
    const uint32_t wide_filtered[4] = {
        0x803088ff, 0x887838ff, 0x906880ff, 0xc05868ff,
    };
    const uint32_t nearest[4] = {
        0xe02060ff, 0xe02060ff, 0x4040e0ff, 0x4040e0ff,
    };
    const uint32_t filtered[4] = {
        0x7838a8ff, 0xe02060ff, 0x4040e0ff, 0x2848e8ff,
    };
    const uint32_t stored_filtered[4] = {
        0x807050ff, 0xe02060ff, 0x4040e0ff, 0x807080ff,
    };
    const uint32_t coords[4] = { 0, 0x3e800000, 0x3f400000, 0x3f800000 };
    const unsigned starts[4] = { 16, 40, 64, 88 };
    const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2408,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010e80, 0x16a800, stored ? 0x41100004 : 0x41100002, 0,
        0x00010406, 0,
        0x0001141b, 1, 0, 0, 0, 0,
    };
    uint32_t sampler[] = {
        0x00011c16, wide ? 0x08000800 : 0x08000400,
        wide ? 4 : 2, 1, 1,
        stored ? (linear ? 0x03002022 : 0x03002000) :
                 (linear ? 0x13002022 : 0x13002000),
        0x00101505, stored ? 0x01f0ffff : 0x00f0ffff,
    };
    QTestState *qts = board_start("");
    g_autofree uint32_t *scan = g_new(uint32_t, 128 * 128);
    unsigned mismatches = 0;

    /* Captured PIO geometry preserves the original public texel order. */
    upload_begin_at(qts, 0x16a800, stored ? 4 : 2, 0x080024d0,
                    stored ? 15 : 0, wide ? 6 : stored ? 4 : 2, 1);
    push_user_words(qts, (uint32_t[]) {
        0x00004570, wide ? 0x0f61104b :
                          stored ? 0x0f41104b : 0x0f21104b, 0x00000071,
        wide ? 0x00015811 : stored ? 0x00015011 : 0x00014811,
    }, 4);
    push_user_words(qts, wide ? wide_pixels : stored ? pixels : pixels + 1,
                    wide ? 6 : stored ? 4 : 2);
    upload_end(qts);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    push_user_words(qts, (uint32_t[]) {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    }, 4);
    for (unsigned i = 0; i < ARRAY_SIZE(starts); i++) {
        push_user_words(qts, (uint32_t[]) {
            0x00014400, 7, 0xc380c41f, coords[i],
            0x8080c800, starts[i], 48,
            0x8080c800, starts[i] + 16, 48,
            0x8080c800, starts[i] + 16, 80,
            0x8080c800, starts[i], 80, 0x00014001,
        }, 17);
    }
    odsy_test_pixels(qts, 0, 0, 128, 128, scan);
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            uint32_t expected = 0x000000ff;
            uint32_t actual = scan[y * 128 + x];

            for (unsigned i = 0; i < ARRAY_SIZE(starts); i++) {
                if (x >= starts[i] && x < starts[i] + 16 &&
                    y >= 48 && y < 80) {
                    expected = wide ? (linear ? wide_filtered[i] :
                                                wide_nearest[i]) :
                               linear ? (stored ? stored_filtered[i] :
                                                  filtered[i]) : nearest[i];
                }
            }
            if (actual != expected) {
                if (mismatches < 8) {
                    g_test_message("pixel (%u,%u): expected %08x, got %08x",
                                   x, y, expected, actual);
                }
                mismatches++;
            }
        }
    }
    g_test_message("1D %s %s clamp mismatches %u/16384",
                   stored ? "stored" : "zero-border",
                   linear ? "linear" : "nearest", mismatches);
    /* Unknown flag pairs, filters and high fields stay unsupported. */
    for (unsigned bad = 0; bad < 3; bad++) {
        sampler[5] = bad == 0 ? (linear ? 0x03002022 : 0x03002000) :
                     bad == 1 ? 0x13002020 :
                     linear ? 0x13002022 : 0x13002000;
        sampler[7] = bad == 2 ? 0x02f0ffff : 0x00f0ffff;
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        odsy_test_pixel_write(qts, 10, 10, 0x123456ff);
        texture_adapter_point(qts, 10, 10, 0.25f, 0.5f);
        g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x123456ff);
    }
    if (stored) {
        const uint32_t unsupported_levels[][4] = {
            { 0x00010e80, 0x16a800, 0x41100000, 0 },
            { 0x00010e80, 0x16a800, 0x41100002, 0 },
            { 0x00010e80, 0x16a800, 0x41100006, 0 },
            { 0x00010e80, 0x16a800, 0x41100004, 1 },
        };

        sampler[5] = linear ? 0x03002022 : 0x03002000;
        sampler[7] = 0x01f0ffff;
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        for (unsigned i = 0; i < ARRAY_SIZE(unsupported_levels); i++) {
            push_user_words(qts, unsupported_levels[i], 4);
            odsy_test_pixel_write(qts, 10, 10, 0x123456ff);
            texture_adapter_point(qts, 10, 10, 0.0f, 0.5f);
            g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x123456ff);
        }
        push_user_words(qts, (uint32_t[]) {
            0x00010e80, 0x16a800, 0x41100004, 0,
        }, 4);
        /* Captured widths two and four do not establish larger placement. */
        sampler[1] = 0x08000c00;
        sampler[2] = 8;
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        odsy_test_pixel_write(qts, 10, 10, 0x123456ff);
        texture_adapter_point(qts, 10, 10, 0.0f, 0.5f);
        g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x123456ff);
    }
    board_quit(qts);
    g_assert_cmpuint(mismatches, ==, 0);
}

/*
 * glTexCoord1i sends 0xc380c41e and s as a signed integer, which selects a
 * texel as the float s does: the two-texel zero-border texture's nearest
 * texel at s = 0 and 1 (texture_one_dimensional_clamp()'s nearest[0] and
 * nearest[3]), and a negative s clamps to the first. Each quad follows a
 * glTexCoord1f to the other end, so a word that changed nothing would draw
 * that end's texel.
 */
static void test_texture_one_dimensional_integer_coordinate(void)
{
    static const uint32_t texels[2] = { 0xe02060ff, 0x4040e0ff };
    static const struct {
        uint32_t before, s, expected;
    } cases[] = {
        { ODSY_F_ONE, 0, 0xe02060ff },
        { 0, 1, 0x4040e0ff },
        { ODSY_F_ONE, 0xffffffff, 0xe02060ff },
    };
    static const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2408,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010e80, 0x16a800, 0x41100002, 0,
        0x00010406, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00011c16, 0x08000400, 2, 1, 1, 0x13002000, 0x00101505, 0x00f0ffff,
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    QTestState *qts = board_start("");

    upload_begin_at(qts, 0x16a800, 2, 0x080024d0, 0, 2, 1);
    push_user_words(qts, (uint32_t[]) {
        0x00004570, 0x0f21104b, 0x00000071, 0x00014811,
    }, 4);
    push_user_words(qts, texels, 2);
    upload_end(qts);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        push_user_words(qts, (uint32_t[]) {
            0xc380c41f, cases[i].before,
            0x00014400, 7, 0xc380c41e, cases[i].s,
            0x8080c800, 40, 48,
            0x8080c800, 56, 48,
            0x8080c800, 56, 80,
            0x8080c800, 40, 80, 0x00014001,
        }, 19);
        g_assert_cmphex(odsy_test_pixel(qts, 48, 64), ==, cases[i].expected);
    }
    board_quit(qts);
}

static void test_texture_one_dimensional_clamp_nearest(void)
{
    texture_one_dimensional_clamp(false, false, false);
}

static void test_texture_one_dimensional_clamp_linear(void)
{
    texture_one_dimensional_clamp(true, false, false);
}

static void test_texture_one_dimensional_stored_nearest(void)
{
    texture_one_dimensional_clamp(false, true, false);
}

static void test_texture_one_dimensional_stored_linear(void)
{
    texture_one_dimensional_clamp(true, true, false);
}

static void test_texture_one_dimensional_wide_stored_nearest(void)
{
    texture_one_dimensional_clamp(false, true, true);
}

static void test_texture_one_dimensional_wide_stored_linear(void)
{
    texture_one_dimensional_clamp(true, true, true);
}

static void texture_one_dimensional_stored_upload(bool wide)
{
    const uint32_t pixels[4] = {
        0x20c040ff, 0xe02060ff, 0x4040e0ff, 0xc0a020ff,
    };
    const uint32_t wide_pixels[6] = {
        0x1050f0ff, 0xf01020ff, 0x20e050ff,
        0x4030f0ff, 0xe0a010ff, 0xa010c0ff,
    };
    const uint32_t sampler[] = {
        0x00010405, 8,
        0x00011c16, 0x08001800, 64, 1, 1, 0x13000000, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x16a800, 0x41100004, 0,
        0x00010406, 0,
    };
    QTestState *qts = board_start("");

    /* Literal public PIO box, pitch, header and authored RGBA words. */
    upload_begin_at(qts, 0x16a800, 4, 0x080024d0, 15, wide ? 6 : 4, 1);
    push_user_words(qts, (uint32_t[]) {
        0x00004570, wide ? 0x0f61104b : 0x0f41104b, 0x00000071,
        wide ? 0x00015811 : 0x00015011,
    }, 4);
    push_user_words(qts, wide ? wide_pixels : pixels, wide ? 6 : 4);
    upload_end(qts);
    texture_adapter_window(qts);
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    /* Ordinary sampling checks the copy independently of stored borders. */
    for (unsigned i = 0; i < (wide ? 6 : 4); i++) {
        texture_adapter_point(qts, 10 + i, 10, (15.5f + i) / 64, 0.5f);
        g_assert_cmphex(odsy_test_pixel(qts, 10 + i, 10), ==,
                        wide ? wide_pixels[i] : pixels[i]);
    }
    board_quit(qts);
}

static void test_texture_one_dimensional_stored_upload(void)
{
    texture_one_dimensional_stored_upload(false);
}

static void test_texture_one_dimensional_wide_stored_upload(void)
{
    texture_one_dimensional_stored_upload(true);
}

static const uint32_t public_border_1d_width8[] = {
    0xf01020ff, 0x203458ff, 0x64e0ccff, 0xa88c40ff,
    0xec38b4ff, 0x30e428ff, 0x74909cff, 0xb83c10ff,
    0xfce884ff, 0x20e050ff,
};

static const uint32_t public_border_1d_width64[] = {
    0xf01020ff, 0x203458ff, 0x64e0ccff, 0xa88c40ff,
    0xec38b4ff, 0x30e428ff, 0x74909cff, 0xb83c10ff,
    0xfce884ff, 0x4094f8ff, 0x84406cff, 0xc8ece0ff,
    0x0c9854ff, 0x5044c8ff, 0x94f03cff, 0xd89cb0ff,
    0x1c4824ff, 0x60f498ff, 0xa4a00cff, 0xe84c80ff,
    0x2cf8f4ff, 0x70a468ff, 0xb450dcff, 0xf8fc50ff,
    0x3ca8c4ff, 0x805438ff, 0xc400acff, 0x08ac20ff,
    0x4c5894ff, 0x900408ff, 0xd4b07cff, 0x185cf0ff,
    0x5c0864ff, 0xa0b4d8ff, 0xe4604cff, 0x280cc0ff,
    0x6cb834ff, 0xb064a8ff, 0xf4101cff, 0x38bc90ff,
    0x7c6804ff, 0xc01478ff, 0x04c0ecff, 0x486c60ff,
    0x8c18d4ff, 0xd0c448ff, 0x1470bcff, 0x581c30ff,
    0x9cc8a4ff, 0xe07418ff, 0x24208cff, 0x68cc00ff,
    0xac7874ff, 0xf024e8ff, 0x34d05cff, 0x787cd0ff,
    0xbc2844ff, 0x00d4b8ff, 0x44802cff, 0x882ca0ff,
    0xccd814ff, 0x108488ff, 0x5430fcff, 0x98dc70ff,
    0xdc88e4ff, 0x20e050ff,
};

static const uint32_t public_border_2d_4x2[4][6] = {
    { 0xf01020ff, 0x10f030ff, 0x2010f0ff, 0xf0e010ff,
      0x1020f0ff, 0xe010c0ff },
    { 0x1030f0ff, 0x0c2454ff, 0x2c6494ff, 0x58a0d0ff,
      0x80dc10ff, 0xf010c0ff },
    { 0x10f0e0ff, 0xc01c3cff, 0xe04c6cff, 0xf8889cff,
      0x44c4ecff, 0xe01080ff },
    { 0xf02010ff, 0x20f010ff, 0x1020f0ff, 0xe010f0ff,
      0xe020a0ff, 0x10e0e0ff },
};

static void texture_public_border_upload_1d(QTestState *qts, bool wide)
{
    const uint32_t *pixels = wide ? public_border_1d_width64 :
                                    public_border_1d_width8;
    unsigned count = wide ? ARRAY_SIZE(public_border_1d_width64) :
                            ARRAY_SIZE(public_border_1d_width8);

    upload_begin_at(qts, 0x16a800, wide ? 6 : 4, 0x080024d0, 15, count, 1);
    upload_first_header(qts, 0x4b);
    for (unsigned i = 0; i < count; ) {
        /* Width 64 uses synthetic four-texel transport chunks. */
        unsigned n = MIN(count - i, wide ? 4 : 16);

        user_cmd_push(qts, ODSY_IMAGE_BURST(n, ODSY_IMAGE_FMT_RGB));
        push_user_words(qts, &pixels[i], n);
        i += n;
    }
    upload_end(qts);
}

static void texture_public_border_upload_2d(QTestState *qts, unsigned rows)
{
    upload_begin_xy(qts, 0x16a800, 4, 0x080024d0, 15, 15, 6, rows);
    for (unsigned row = 0; row < rows; row++) {
        upload_row(qts, 0x4b, public_border_2d_4x2[row], 6);
    }
    upload_end(qts);
}

static void texture_public_border_assert_1d(QTestState *qts, bool wide)
{
    const uint32_t *pixels = wide ? public_border_1d_width64 :
                                    public_border_1d_width8;
    unsigned count = wide ? ARRAY_SIZE(public_border_1d_width64) :
                            ARRAY_SIZE(public_border_1d_width8);

    for (unsigned x = 0; x < count; x++) {
        g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 15 + x, 0), ==,
                        pixels[x]);
    }
}

static void test_texture_public_border_copied_rows(void)
{
    QTestState *qts = board_start("");

    texture_public_border_upload_1d(qts, false);
    texture_public_border_assert_1d(qts, false);
    board_quit(qts);

    qts = board_start("");
    texture_public_border_upload_1d(qts, true);
    texture_public_border_assert_1d(qts, true);
    board_quit(qts);

    qts = board_start("");
    texture_public_border_upload_2d(qts, 4);
    for (unsigned y = 0; y < ARRAY_SIZE(public_border_2d_4x2); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(public_border_2d_4x2[0]); x++) {
            g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800,
                                                  15 + x, 15 + y), ==,
                            public_border_2d_4x2[y][x]);
        }
    }
    board_quit(qts);
}

static void texture_public_border_sample_1d(QTestState *qts, bool wide,
                                             bool linear)
{
    const uint32_t expected[2][2][4] = {
        {
            { 0x203458ff, 0xa88c40ff, 0xb83c10ff, 0xfce884ff },
            { 0x88223cff, 0x86b686ff, 0x966656ff, 0x8ee46aff },
        },
        {
            { 0x203458ff, 0x60f498ff, 0xe07418ff, 0xdc88e4ff },
            { 0x88223cff, 0x3e9e5eff, 0xbe9e5eff, 0x7eb49aff },
        },
    };
    const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2408,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010e80, 0x16a800, wide ? 0x41100006 : 0x41100004, 0,
        0x00010406, 0,
        0x0001141b, 1, 0, 0, 0, 0,
    };
    const uint32_t sampler[] = {
        0x00011c16, wide ? 0x08001800 : 0x08000c00,
        wide ? 64 : 8, 1, 1,
        linear ? 0x03002022 : 0x03002000,
        0x00101505, 0x01f0ffff,
    };
    const float coords[4] = { 0.0f, 0.25f, 0.75f, 1.0f };

    texture_public_border_upload_1d(qts, wide);
    texture_public_border_assert_1d(qts, wide);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    for (unsigned i = 0; i < ARRAY_SIZE(coords); i++) {
        texture_adapter_point(qts, 24 + 24 * i, 64, coords[i], 0.5f);
        g_assert_cmphex(odsy_test_pixel(qts, 24 + 24 * i, 64), ==,
                        expected[wide][linear][i]);
    }
}

static void test_texture_public_border_1d_width8_nearest(void)
{
    QTestState *qts = board_start("");
    texture_public_border_sample_1d(qts, false, false);
    board_quit(qts);
}

static void test_texture_public_border_1d_width8_linear(void)
{
    QTestState *qts = board_start("");
    texture_public_border_sample_1d(qts, false, true);
    board_quit(qts);
}

static void test_texture_public_border_1d_width64_nearest(void)
{
    QTestState *qts = board_start("");
    texture_public_border_sample_1d(qts, true, false);
    board_quit(qts);
}

static void test_texture_public_border_1d_width64_linear(void)
{
    QTestState *qts = board_start("");
    texture_public_border_sample_1d(qts, true, true);
    board_quit(qts);
}

static void texture_public_border_sample_2d(bool linear)
{
    const uint32_t expected[2][4][4] = {
        {
            { 0x0c2454ff, 0x2c6494ff, 0x80dc10ff, 0x80dc10ff },
            { 0x0c2454ff, 0x2c6494ff, 0x80dc10ff, 0x80dc10ff },
            { 0xc01c3cff, 0xe04c6cff, 0x44c4ecff, 0x44c4ecff },
            { 0xc01c3cff, 0xe04c6cff, 0x44c4ecff, 0x44c4ecff },
        },
        {
            { 0x475565ff, 0x1a6282ff, 0x769f78ff, 0x9847a0ff },
            { 0x0e2aa2ff, 0x1c4474ff, 0x6cbe70ff, 0xb87668ff },
            { 0x68868eff, 0xd03454ff, 0x9ea6c4ff, 0x926ab6ff },
            { 0x78874fff, 0x745e6aff, 0xbf5fc6ff, 0x8575bbff },
        },
    };
    const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2410,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010e80, 0x16a800, 0x41100004, 0,
        0x00010406, 0,
        0x0001141b, 1, 0, 0, 0, 0,
    };
    const uint32_t sampler[] = {
        0x00011c16, 0x08000820, 4, 2, 1,
        linear ? 0x03002822 : 0x03002800,
        0x00101202, 0x01303fff,
    };
    const float coords[4] = { 0.0f, 0.25f, 0.75f, 1.0f };
    QTestState *qts = board_start("");

    texture_public_border_upload_2d(qts, 4);
    for (unsigned y = 0; y < ARRAY_SIZE(public_border_2d_4x2); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(public_border_2d_4x2[0]); x++) {
            g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800,
                                                  15 + x, 15 + y), ==,
                            public_border_2d_4x2[y][x]);
        }
    }
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    for (unsigned row = 0; row < ARRAY_SIZE(coords); row++) {
        for (unsigned column = 0; column < ARRAY_SIZE(coords); column++) {
            int x = 16 + 28 * column;
            int y = 16 + 28 * row;

            texture_adapter_point(qts, x, y, coords[column], coords[row]);
            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==,
                            expected[linear][row][column]);
        }
    }
    board_quit(qts);
}

static void test_texture_public_border_2d_nearest(void)
{
    texture_public_border_sample_2d(false);
}

static void test_texture_public_border_2d_linear(void)
{
    texture_public_border_sample_2d(true);
}

static void test_texture_public_border_2d_incomplete(void)
{
    const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2410,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010e80, 0x16a800, 0x41100004, 0,
        0x00010406, 0,
        0x0001141b, 1, 0, 0, 0, 0,
    };
    const uint32_t sampler[] = {
        0x00011c16, 0x08000820, 4, 2, 1, 0x03002800,
        0x00101202, 0x01303fff,
    };
    QTestState *qts = board_start("");

    texture_public_border_upload_2d(qts, 3);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    odsy_test_pixel_write(qts, 10, 10, 0x123456ff);
    texture_adapter_point(qts, 10, 10, 0.75f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x123456ff);
    board_quit(qts);
}

static uint32_t texture_public_border_volume_texel(unsigned x, unsigned y,
                                                    unsigned z)
{
    return ((0x10 + x) << 24) | (y << 16) | (z << 8) | 0xff;
}

static void texture_public_border_upload_3d(QTestState *qts,
                                            unsigned width, unsigned height,
                                            unsigned depth, bool stored,
                                            unsigned missing)
{
    unsigned x0 = stored ? 15 : 0;
    unsigned y0 = stored ? 15 : 0;
    unsigned w = width + (stored ? 2 : 0);
    unsigned h = height + (stored ? 2 : 0);
    unsigned d = depth + (stored ? 2 : 0);

    for (unsigned z = 0; z < d; z++) {
        uint32_t base = 0x16a800 + z * (stored ? 0x400 : 0x80);
        /* Only this captured 2x2x2 stored profile uses the high slices. */
        if (stored && width == 2 && height == 2 && depth == 2) {
            base = 0x16e400 + z * 0x400;
        }

        if (stored && missing == 1 && z == 0) {
            continue;
        }
        upload_begin_xy(qts, base, stored ? 4 : 2, 0x080024d0,
                        x0, y0, w, h);
        for (unsigned y = 0; y < h; y++) {
            uint32_t row[8];
            unsigned row_width = w;

            for (unsigned x = 0; x < w; x++) {
                row[x] = texture_public_border_volume_texel(x, y, z);
            }
            if (stored && missing == 2 && z == 1 && y == 1) {
                row_width--;
            }
            if (stored && missing == 3 && z == 0 && y == 0) {
                row_width--;
            }
            upload_row(qts, 0x4b, row, row_width);
        }
        upload_end(qts);
    }
}

static void texture_public_border_bind_3d(QTestState *qts,
                                          unsigned width, unsigned height,
                                          unsigned depth, bool stored,
                                          bool linear)
{
    uint32_t sampler[] = {
        0x08000000 | (ctz32(width) << 10) | (ctz32(height) << 5) |
        ctz32(depth), width, height, depth,
        stored ? (linear ? 0x03002a22 : 0x03002a00) :
                 (linear ? 0x13002a22 : 0x13002a00),
        stored ? 0x00101505 : 0, stored ? 0x01f0ffff : 0,
    };
    const uint32_t env[] = { 1, 0, 0, 0, 0 };
    const uint32_t level[] = {
        0x16a800, stored ? 0x41100004 : 0x41100002, stored ? 4 : 1,
    };
    const uint32_t words[] = {
        0x00010405, 0x20,
        0x00011c16,
        sampler[0], sampler[1], sampler[2], sampler[3],
        sampler[4], sampler[5], sampler[6],
        0x0001141b, env[0], env[1], env[2], env[3], env[4],
        0x00010e80, level[0], level[1], level[2],
        0x00010406, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

/*
 * The texels are the public probe's authored 4x4x4 grid. Their placement,
 * level_base + (15 + z) * 0x400, is the Odyssey datasheet's bounded policy
 * for this captured stored RGBA8, border-one, 2x2x2 profile: it approximates
 * software writes seen in Octane2 V12 model traces at the level bases
 * 0x16a800 and 0x16b800. Physical V12 texture layout is unqualified.
 */
#define PUBLIC_VOLUME_STRIDE 0x400
/* Each slice's upload box starts at (15, 15) inside its object. */
#define PUBLIC_VOLUME_BOX 15
#define PUBLIC_VOLUME_FIRST_BASE 0x16a800
#define PUBLIC_VOLUME_SECOND_BASE 0x16b800

static const struct {
    float s, t, r;
    uint32_t nearest, linear;
} public_volume_samples[] = {
    { 0.25f, 0.25f, 0.25f, 0x0080c0ff, 0x0080c0ff },
    { 0.125f, 0.125f, 0.125f, 0x0080c0ff, 0x546c78ff },
    { 0.875f, 0.125f, 0.125f, 0x400080ff, 0x544458ff },
    { 0.125f, 0.875f, 0.125f, 0x80c040ff, 0x5c6c68ff },
    { 0.875f, 0.875f, 0.125f, 0xc04000ff, 0x7c6448ff },
    { 0.125f, 0.125f, 0.875f, 0x404040ff, 0x544c68ff },
    { 0.875f, 0.125f, 0.875f, 0x80c000ff, 0x748448ff },
    { 0.125f, 0.875f, 0.875f, 0xc080c0ff, 0x7c6c78ff },
    { 0.875f, 0.875f, 0.875f, 0x000080ff, 0x3c4458ff },
    { -0.125f, 0.75f, 0.25f, 0x80c040ff, 0x608060ff },
    { 1.125f, 0.75f, 0.25f, 0xc04000ff, 0x608060ff },
    { 0.25f, -0.125f, 0.75f, 0x404040ff, 0x802080ff },
    { 0.25f, 1.125f, 0.75f, 0xc080c0ff, 0x80a080ff },
    { 0.25f, 0.75f, -0.125f, 0x80c040ff, 0x606080ff },
    { 0.25f, 0.75f, 1.125f, 0xc080c0ff, 0x606080ff },
};

/* The captured stored-border sampler and level words, nearest filter. */
static const uint32_t public_volume_sampler[7] = {
    0x08000421, 2, 2, 2, 0x03002a00, 0x00000404, 0x01c0cfff,
};
static const uint32_t public_volume_level[2] = { 0x41100004, 4 };
/* The zero-border capture's level words: a different decoded stride. */
static const uint32_t public_volume_zero_border_level[2] = { 0x41100002, 1 };

static uint32_t public_stored_volume_texel(unsigned x, unsigned y, unsigned z)
{
    return (((x + 2 * y + z) % 4) * 64) << 24 |
           (((2 * x + y + 3 * z) % 4) * 64) << 16 |
           (((3 * x + 2 * y + 2 * z) % 4) * 64) << 8 | 0xff;
}

static uint32_t public_stored_volume_slice(uint32_t level_base, unsigned z)
{
    return level_base + (15 + z) * PUBLIC_VOLUME_STRIDE;
}

static void public_stored_volume_object(QTestState *qts, uint32_t base,
                                        unsigned slice, bool authored,
                                        uint32_t decoy)
{
    upload_begin_xy(qts, base, 4, 0x080024d0, PUBLIC_VOLUME_BOX,
                    PUBLIC_VOLUME_BOX, 4, 4);
    for (unsigned y = 0; y < 4; y++) {
        const uint32_t header[] = {
            ODSY_CMD_IMAGE_HEADER, 0x0f41104b, ODSY_CMD_IMAGE_HEADER_TAIL,
            ODSY_IMAGE_BURST(4, ODSY_IMAGE_FMT_RGB),
        };
        uint32_t row[4];

        for (unsigned x = 0; x < 4; x++) {
            row[x] = authored ? public_stored_volume_texel(x, y, slice) :
                                decoy;
        }
        /* Row framing and payload match the retained public upload. */
        push_user_words(qts, header, ARRAY_SIZE(header));
        push_user_words(qts, row, ARRAY_SIZE(row));
    }
    upload_end(qts);
}

/* Slice z's 16 authored texels sit inside the upload box of the object. */
static void public_stored_volume_check(QTestState *qts, uint32_t base,
                                       unsigned z)
{
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            g_assert_cmphex(odsy_test_object_texel(
                qts, base, PUBLIC_VOLUME_BOX + x, PUBLIC_VOLUME_BOX + y), ==,
                public_stored_volume_texel(x, y, z));
        }
    }
}

/* Upload authored slice z at base and confirm it landed. */
static void public_stored_volume_put(QTestState *qts, uint32_t base,
                                     unsigned z)
{
    public_stored_volume_object(qts, base, z, true, 0);
    public_stored_volume_check(qts, base, z);
}

/*
 * Complete decoys at the level base, one stride below the first high slice
 * and one above the last must never stand in for high slices. Each has its
 * own colour, and none is an authored texel.
 */
static void public_stored_volume_decoys(QTestState *qts, uint32_t level_base)
{
    for (unsigned z = 0; z < 4; z++) {
        public_stored_volume_object(qts,
                                    level_base + z * PUBLIC_VOLUME_STRIDE,
                                    z, false, 0xff00ffff | z << 16);
    }
    public_stored_volume_object(qts, level_base + 14 * PUBLIC_VOLUME_STRIDE,
                                0, false, 0x00ffffff);
    public_stored_volume_object(qts, level_base + 19 * PUBLIC_VOLUME_STRIDE,
                                0, false, 0xffff00ff);
}

/*
 * Mode and level-entry framing come from the retained public transport.
 * The environment word 0x141b = 1 does not: neither the transport nor the
 * datasheet records it. It is a precondition taken from the model, whose
 * texture setup drops a draw without an environment and decodes 1 as
 * replace. It is not capture or hardware evidence. Replace makes the
 * expected pixels the texels themselves rather than texels modulated by
 * the yellow vertex colour.
 */
static void public_stored_volume_bind_with(QTestState *qts,
                                           const uint32_t sampler[7],
                                           uint32_t level_base,
                                           const uint32_t level[2])
{
    const uint32_t words[] = {
        0x00010404, 0x02042200,
        0x00010405, 0x00002420,
        0x00011c16, sampler[0], sampler[1], sampler[2], sampler[3],
        sampler[4], sampler[5], sampler[6],
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, level_base, level[0], level[1],
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void public_stored_volume_bind(QTestState *qts, uint32_t level_base,
                                      bool linear)
{
    uint32_t sampler[7];

    memcpy(sampler, public_volume_sampler, sizeof(sampler));
    if (linear) {
        sampler[4] = 0x03002a22;
    }
    public_stored_volume_bind_with(qts, sampler, level_base,
                                   public_volume_level);
}

static void public_stored_volume_draw(QTestState *qts)
{
    texture_adapter_point_3d(qts, 101, 111, public_volume_samples[0].s,
                             public_volume_samples[0].t,
                             public_volume_samples[0].r);
}

/*
 * The draw just bound must leave the destination pixel alone. The model
 * reports an unsupported profile and missing backing through the same
 * dropped draw, and exposes no state that tells them apart, so these
 * cases check only that nothing was sampled.
 */
static void public_stored_volume_dropped(QTestState *qts)
{
    uint32_t before = odsy_test_pixel(qts, 101, 111);

    public_stored_volume_draw(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 111), ==, before);
}

/* Positive control: the captured binding samples the high slices. */
static void public_stored_volume_samples_high(QTestState *qts,
                                              uint32_t level_base)
{
    texture_adapter_window(qts);
    public_stored_volume_bind(qts, level_base, false);
    public_stored_volume_draw(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 111), ==,
                    public_volume_samples[0].nearest);
}

static void public_stored_volume_complete(uint32_t level_base)
{
    QTestState *qts = board_start("");

    public_stored_volume_decoys(qts, level_base);
    for (unsigned z = 0; z < 4; z++) {
        public_stored_volume_put(qts, public_stored_volume_slice(level_base, z),
                                 z);
    }
    texture_adapter_window(qts);
    for (unsigned linear = 0; linear < 2; linear++) {
        public_stored_volume_bind(qts, level_base, linear);
        for (unsigned i = 0; i < ARRAY_SIZE(public_volume_samples); i++) {
            uint32_t expected = linear ? public_volume_samples[i].linear :
                                         public_volume_samples[i].nearest;

            texture_adapter_point_3d(qts, 100 + i, 110 + linear,
                                     public_volume_samples[i].s,
                                     public_volume_samples[i].t,
                                     public_volume_samples[i].r);
            g_assert_cmphex(odsy_test_pixel(qts, 100 + i, 110 + linear), ==,
                            expected);
        }
    }
    board_quit(qts);
}

/*
 * The high slices in absent are not uploaded, while every decoy is
 * complete: no nearby object may stand in. Uploading them afterwards makes
 * the same draw sample.
 */
static void public_stored_volume_missing(uint32_t level_base, unsigned absent)
{
    QTestState *qts = board_start("");

    public_stored_volume_decoys(qts, level_base);
    for (unsigned z = 0; z < 4; z++) {
        if (!(absent & 1u << z)) {
            public_stored_volume_put(qts,
                                     public_stored_volume_slice(level_base, z),
                                     z);
        }
    }
    texture_adapter_window(qts);
    public_stored_volume_bind(qts, level_base, false);
    public_stored_volume_dropped(qts);

    for (unsigned z = 0; z < 4; z++) {
        if (absent & 1u << z) {
            public_stored_volume_put(qts,
                                     public_stored_volume_slice(level_base, z),
                                     z);
        }
    }
    public_stored_volume_samples_high(qts, level_base);
    board_quit(qts);
}

/*
 * Bind with the given sampler and level words over a complete high-origin
 * volume and its decoys, plus deep complete decoys at level_base + z * 0x400
 * for z from 4 up to (but not including) deep; the draw must not sample. The
 * captured binding then samples the same volume.
 */
static void public_stored_volume_rejects(const uint32_t sampler[7],
                                         const uint32_t level[2],
                                         unsigned deep)
{
    uint32_t level_base = PUBLIC_VOLUME_FIRST_BASE;
    QTestState *qts = board_start("");

    public_stored_volume_decoys(qts, level_base);
    for (unsigned z = 4; z < deep; z++) {
        public_stored_volume_object(qts,
                                    level_base + z * PUBLIC_VOLUME_STRIDE,
                                    z, false, 0xff00ffff | z << 16);
    }
    for (unsigned z = 0; z < 4; z++) {
        public_stored_volume_put(qts, public_stored_volume_slice(level_base, z),
                                 z);
    }
    texture_adapter_window(qts);
    public_stored_volume_bind_with(qts, sampler, level_base, level);
    public_stored_volume_dropped(qts);
    public_stored_volume_samples_high(qts, level_base);
    board_quit(qts);
}

static void test_public_stored_volume_first_base(void)
{
    public_stored_volume_complete(PUBLIC_VOLUME_FIRST_BASE);
}

static void test_public_stored_volume_second_base(void)
{
    public_stored_volume_complete(PUBLIC_VOLUME_SECOND_BASE);
}

static void test_public_stored_volume_first_base_missing_0(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_FIRST_BASE, 1u << 0);
}

static void test_public_stored_volume_first_base_missing_1(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_FIRST_BASE, 1u << 1);
}

static void test_public_stored_volume_first_base_missing_2(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_FIRST_BASE, 1u << 2);
}

static void test_public_stored_volume_first_base_missing_3(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_FIRST_BASE, 1u << 3);
}

static void test_public_stored_volume_first_base_missing_all(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_FIRST_BASE, 0xf);
}

static void test_public_stored_volume_second_base_missing_0(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_SECOND_BASE, 1u << 0);
}

static void test_public_stored_volume_second_base_missing_1(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_SECOND_BASE, 1u << 1);
}

static void test_public_stored_volume_second_base_missing_2(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_SECOND_BASE, 1u << 2);
}

static void test_public_stored_volume_second_base_missing_3(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_SECOND_BASE, 1u << 3);
}

static void test_public_stored_volume_second_base_missing_all(void)
{
    public_stored_volume_missing(PUBLIC_VOLUME_SECOND_BASE, 0xf);
}

/*
 * The same encoded profile with complete authored slices only at
 * level_base + z * 0x400 and nothing at the high origin. The datasheet
 * gives no selector for this low placement, so it must not be sampled.
 * The high slices added afterwards are.
 */
static void test_public_stored_volume_low_origin(void)
{
    uint32_t level_base = PUBLIC_VOLUME_FIRST_BASE;
    QTestState *qts = board_start("");

    for (unsigned z = 0; z < 4; z++) {
        public_stored_volume_put(qts, level_base + z * PUBLIC_VOLUME_STRIDE,
                                 z);
    }
    texture_adapter_window(qts);
    public_stored_volume_bind(qts, level_base, false);
    public_stored_volume_dropped(qts);
    for (unsigned z = 0; z < 4; z++) {
        public_stored_volume_put(qts, public_stored_volume_slice(level_base, z),
                                 z);
    }
    public_stored_volume_samples_high(qts, level_base);
    board_quit(qts);
}

/*
 * The zero-border capture's level words, 0x41100002,1, with the stored
 * descriptor: a decoded slice stride other than the selected 0x400.
 */
static void test_public_stored_volume_other_stride(void)
{
    public_stored_volume_rejects(public_volume_sampler,
                                 public_volume_zero_border_level, 0);
}

/*
 * The zero-border capture's final sampler word, 0x00c0cfff, in place of the
 * stored border's 0x01c0cfff: no longer the captured stored profile.
 */
static void test_public_stored_volume_wrong_border(void)
{
    uint32_t sampler[7];

    memcpy(sampler, public_volume_sampler, sizeof(sampler));
    sampler[6] = 0x00c0cfff;
    public_stored_volume_rejects(sampler, public_volume_level, 0);
}

/*
 * A depth word of 4 where the captured descriptor says 2, leaving the first
 * sampler word unchanged: a malformed, non-captured size. A depth-4 stored
 * volume read from the level base would span six slice objects, so complete
 * decoys fill base + 4 and base + 5 strides too: only the size check, not a
 * missing object, can reject this draw.
 */
static void test_public_stored_volume_wrong_size(void)
{
    uint32_t sampler[7];

    memcpy(sampler, public_volume_sampler, sizeof(sampler));
    sampler[3] = 4;
    public_stored_volume_rejects(sampler, public_volume_level, 6);
}

static void test_texture_public_border_3d_copied_rows(void)
{
    QTestState *qts = board_start("");

    texture_public_border_upload_3d(qts, 2, 2, 2, true, 0);
    for (unsigned z = 0; z < 4; z++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(odsy_test_object_texel(
                                    qts, 0x16e400 + z * 0x400,
                                    15 + x, 15 + y), ==,
                                texture_public_border_volume_texel(x, y, z));
            }
        }
    }
    board_quit(qts);
}

static void texture_public_border_sample_3d(bool linear)
{
    static const unsigned interior[8][3] = {
        { 1, 1, 1 }, { 2, 1, 1 }, { 1, 2, 1 }, { 2, 2, 1 },
        { 1, 1, 2 }, { 2, 1, 2 }, { 1, 2, 2 }, { 2, 2, 2 },
    };
    static const unsigned nearest[15][3] = {
        { 0, 1, 1 }, { 3, 1, 1 }, { 1, 0, 1 }, { 1, 3, 1 },
        { 1, 1, 0 }, { 1, 1, 3 }, { 0, 0, 1 }, { 3, 3, 2 },
        { 0, 2, 0 }, { 3, 1, 3 }, { 1, 0, 0 }, { 2, 3, 3 },
        { 0, 0, 0 }, { 3, 3, 3 }, { 2, 2, 2 },
    };
    static const float coordinate[] = { -0.25f, 0.25f, 0.75f, 1.25f };
    /*
     * With width 2, linear filtering uses u = 2*s - 0.5. At s=0,
     * x=-1 and x=0 each have weight 0.5; t=r=0.25 select y=z=0.
     * The stored grid values are (0,1,1) and (1,1,1), so red is
     * halfway between 16 and 17 while G/B stay 1. Permit that one
     * red byte to round either way. The s=0.125 control gives weights
     * 0.25/0.75 and a non-tie result. The positive-edge cases mirror
     * those weights with stored grid values (2,2,2) and (3,2,2).
     */
    static const struct {
        float s, t, r;
        uint32_t expected, alternate;
    } linear_border[] = {
        { 0.0f, 0.25f, 0.25f, 0x100101ff, 0x110101ff },
        { 0.125f, 0.25f, 0.25f, 0x110101ff, 0 },
        { 1.0f, 0.75f, 0.75f, 0x120202ff, 0x130202ff },
        { 0.875f, 0.75f, 0.75f, 0x120202ff, 0 },
        { 0.125f, 0.125f, 0.25f, 0x110101ff, 0 },
        { 0.125f, 0.125f, 0.125f, 0x110101ff, 0 },
    };
    QTestState *qts = board_start("");
    const unsigned (*points)[3] = linear ? interior : nearest;
    unsigned count = linear ? ARRAY_SIZE(interior) : ARRAY_SIZE(nearest);

    texture_public_border_upload_3d(qts, 2, 2, 2, true, 0);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 2, 2, 2, true, linear);
    for (unsigned i = 0; i < count; i++) {
        int x = 16 + 28 * i;
        float s = coordinate[points[i][0]];
        float t = coordinate[points[i][1]];
        float r = coordinate[points[i][2]];

        texture_adapter_point_3d(qts, x, 64, s, t, r);
        g_assert_cmphex(odsy_test_pixel(qts, x, 64), ==,
                        texture_public_border_volume_texel(
                            MIN(MAX(points[i][0], 1u), 2u),
                            MIN(MAX(points[i][1], 1u), 2u),
                            MIN(MAX(points[i][2], 1u), 2u)));
    }
    if (linear) {
        for (unsigned i = 0; i < ARRAY_SIZE(linear_border); i++) {
            int x = 16 + 28 * i;

            texture_adapter_point_3d(qts, x, 96, linear_border[i].s,
                                     linear_border[i].t, linear_border[i].r);
            uint32_t pixel = odsy_test_pixel(qts, x, 96);

            if (linear_border[i].alternate) {
                g_assert_true(pixel == linear_border[i].expected ||
                              pixel == linear_border[i].alternate);
            } else {
                g_assert_cmphex(pixel, ==, linear_border[i].expected);
            }
        }
    }
    board_quit(qts);
}

static void test_texture_public_border_3d_nearest(void)
{
    texture_public_border_sample_3d(false);
}

static void test_texture_public_border_3d_linear(void)
{
    texture_public_border_sample_3d(true);
}

static void test_texture_public_border_3d_incomplete(void)
{
    for (unsigned missing = 1; missing <= 3; missing++) {
        QTestState *qts = board_start("");

        texture_public_border_upload_3d(qts, 2, 2, 2, true, missing);
        texture_adapter_window(qts);
        push_depth_clear(qts, 0, 0, 0, 0);
        texture_public_border_bind_3d(qts, 2, 2, 2, true, false);
        odsy_test_pixel_write(qts, 10, 10, 0x123456ff);
        texture_adapter_point_3d(qts, 10, 10, 0.25f, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x123456ff);
        board_quit(qts);
    }
}

static void test_texture_public_border_3d_wide(void)
{
    QTestState *qts = board_start("");

    texture_public_border_upload_3d(qts, 4, 2, 2, true, 0);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 4, 2, 2, true, false);
    texture_adapter_point_3d(qts, 10, 10, 0.625f, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==,
                    texture_public_border_volume_texel(3, 1, 1));
    board_quit(qts);
}

static void test_texture_public_border_3d_zero_nearest(void)
{
    QTestState *qts = board_start("");

    texture_public_border_upload_3d(qts, 2, 2, 2, false, 0);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 0), ==,
                    texture_public_border_volume_texel(0, 0, 0));
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 2, 2, 2, false, false);
    texture_adapter_point_3d(qts, 10, 10, 0.25f, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==,
                    texture_public_border_volume_texel(0, 0, 0));
    board_quit(qts);
}

static void test_texture_public_border_3d_zero_linear(void)
{
    QTestState *qts = board_start("");

    texture_public_border_upload_3d(qts, 2, 2, 2, false, 0);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 2, 2, 2, false, true);
    texture_adapter_point_3d(qts, 10, 10, 0.25f, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==,
                    texture_public_border_volume_texel(0, 0, 0));
    board_quit(qts);
}






/*
 * These are original synthetic distinct-colour RGBA words for the captured
 * address and sampler test. They do not replay the public probe's texels or
 * the guest's upload words.
 */






static void test_texture_public_border_3d(void)
{
    test_texture_public_border_3d_copied_rows();
    test_texture_public_border_3d_nearest();
    test_texture_public_border_3d_linear();
    test_texture_public_border_3d_incomplete();
    test_texture_public_border_3d_wide();
    test_texture_public_border_3d_zero_nearest();
    test_texture_public_border_3d_zero_linear();
}

static void texture_two_dimensional_border_upload(QTestState *qts,
                                                   bool stored, bool truncated)
{
    const uint32_t pixels[4][4] = {
        { 0xf01020ff, 0x10f020ff, 0x2010f0ff, 0xf0e010ff },
        { 0x1040f0ff, 0x405060ff, 0x708090ff, 0xe01080ff },
        { 0x10f0e0ff, 0xa0b0c0ff, 0xd0e0f0ff, 0xf010e0ff },
        { 0xf02010ff, 0x20f010ff, 0x1020f0ff, 0xe010f0ff },
    };
    const uint32_t zero_pixels[2][2] = {
        { 0x405060ff, 0x708090ff }, { 0xa0b0c0ff, 0xd0e0f0ff },
    };
    unsigned origin = stored ? 15 : 0, size = stored ? 4 : 2;
    unsigned rows = size - truncated;

    upload_begin_xy(qts, 0x16a800, stored ? 4 : 2, 0x080024d0,
                    origin, origin, size, rows);
    for (unsigned row = 0; row < rows; row++) {
        push_user_words(qts, (uint32_t[]) {
            0x00004570, stored ? 0x0f41104b : 0x0f21104b, 0x00000071,
            stored ? 0x00015011 : 0x00014811,
        }, 4);
        push_user_words(qts, stored ? pixels[row] : zero_pixels[row], size);
    }
    upload_end(qts);
    /* Verify every authored row independently of texture sampling. */
    for (unsigned y = 0; y < rows; y++) {
        for (unsigned x = 0; x < size; x++) {
            g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800,
                                                  origin + x, origin + y), ==,
                            stored ? pixels[y][x] : zero_pixels[y][x]);
        }
    }
}

static void test_texture_two_dimensional_border_upload(void)
{
    for (unsigned stored = 0; stored < 2; stored++) {
        QTestState *qts = board_start("");

        texture_two_dimensional_border_upload(qts, stored, false);
        board_quit(qts);
    }
}

static void texture_two_dimensional_clamp(bool linear, bool stored)
{
    const uint32_t nearest[16] = {
        0x405060ff, 0x405060ff, 0x708090ff, 0x708090ff,
        0x405060ff, 0x405060ff, 0x708090ff, 0x708090ff,
        0xa0b0c0ff, 0xa0b0c0ff, 0xd0e0f0ff, 0xd0e0f0ff,
        0xa0b0c0ff, 0xa0b0c0ff, 0xd0e0f0ff, 0xd0e0f0ff,
    };
    const uint32_t zero_linear[16] = {
        0x1c2c3cff, 0x283848ff, 0x405060ff, 0x283848ff,
        0x283848ff, 0x405060ff, 0x708090ff, 0x405060ff,
        0x586878ff, 0xa0b0c0ff, 0xd0e0f0ff, 0x708090ff,
        0x344454ff, 0x586878ff, 0x708090ff, 0x405060ff,
    };
    const uint32_t stored_linear[16] = {
        0x546464ff, 0x28a040ff, 0x4848c0ff, 0x986084ff,
        0x2848a8ff, 0x405060ff, 0x708090ff, 0xa84888ff,
        0x58d0d0ff, 0xa0b0c0ff, 0xd0e0f0ff, 0xe078e8ff,
        0x70ac70ff, 0x60d068ff, 0x7080f0ff, 0xac48ecff,
    };
    const uint32_t coords[4] = { 0, 0x3e800000, 0x3f400000, 0x3f800000 };
    const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2410,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010e80, 0x16a800, stored ? 0x41100004 : 0x41100002, 0,
        0x00010406, 0,
        0x0001141b, 1, 0, 0, 0, 0,
    };
    uint32_t sampler[] = {
        0x00011c16, 0x08000420, 2, 2, 1,
        stored ? (linear ? 0x03002822 : 0x03002800) :
                 (linear ? 0x13002822 : 0x13002800),
        0x00101202, stored ? 0x01303fff : 0x00303fff,
    };
    QTestState *qts = board_start("");
    unsigned mismatches = 0;
    g_autofree uint32_t *scan = g_new(uint32_t, 128 * 128);

    texture_two_dimensional_border_upload(qts, stored, false);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    push_user_words(qts, (uint32_t[]) {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    }, 4);
    for (unsigned row = 0; row < 4; row++) {
        for (unsigned col = 0; col < 4; col++) {
            unsigned x = 8 + col * 28, y = 8 + row * 28;

            push_user_words(qts, (uint32_t[]) {
                0x00014400, 7, 0xc380c821, coords[col], coords[row],
                0x8080c800, x, y,
                0x8080c800, x + 16, y,
                0x8080c800, x + 16, y + 16,
                0x8080c800, x, y + 16, 0x00014001,
            }, 18);
        }
    }
    odsy_test_pixels(qts, 0, 0, 128, 128, scan);
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            uint32_t expected = 0x000000ff, actual = scan[y * 128 + x];

            if (x >= 8 && y >= 8 && x < 108 && y < 108 &&
                (x - 8) % 28 < 16 && (y - 8) % 28 < 16) {
                unsigned i = ((y - 8) / 28) * 4 + (x - 8) / 28;

                expected = !linear ? nearest[i] :
                           stored ? stored_linear[i] : zero_linear[i];
            }
            if (actual != expected) {
                if (mismatches < 8) {
                    g_test_message("pixel (%u,%u): expected %08x, got %08x",
                                   x, y, expected, actual);
                }
                mismatches++;
            }
        }
    }
    g_test_message("2D %s %s clamp mismatches %u/16384",
                   stored ? "stored" : "zero-border",
                   linear ? "linear" : "nearest", mismatches);
    /*
     * Unpaired flags and high fields stay unsupported. (Other filters
     * under GL_CLAMP on both axes are sampled by the OpenGL rule, so
     * minification-linear 0x13002820 is no longer among them.)
     */
    for (unsigned bad = 0; bad < 2; bad++) {
        sampler[5] = bad == 0 ? (linear ? 0x03002822 : 0x03002800) :
                     linear ? 0x13002822 : 0x13002800;
        sampler[7] = bad == 1 ? 0x02303fff : 0x00303fff;
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        odsy_test_pixel_write(qts, 4, 4, 0x123456ff);
        texture_adapter_point(qts, 4, 4, 0.0f, 0.0f);
        g_assert_cmphex(odsy_test_pixel(qts, 4, 4), ==, 0x123456ff);
    }
    /*
     * A 4x4 stored-border descriptor over this 2x2 upload: the zero-border
     * table entry has the wrong row length and is refused; the stored one
     * names an allocation too short for a 4x4 level, so the image is
     * incomplete and draws untextured, as any bordered image missing a
     * level does.
     */
    sampler[1] = 0x08000840;
    sampler[2] = sampler[3] = 4;
    sampler[5] = linear ? 0x03002822 : 0x03002800;
    sampler[7] = 0x01303fff;
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    odsy_test_pixel_write(qts, 4, 4, 0x123456ff);
    texture_adapter_point(qts, 4, 4, 0.0f, 0.0f);
    g_assert_cmphex(odsy_test_pixel(qts, 4, 4), ==,
                    stored ? 0xffff00ff : 0x123456ff);
    board_quit(qts);
    g_assert_cmpuint(mismatches, ==, 0);
}

static void test_texture_two_dimensional_missing_border_row(void)
{
    const uint32_t setup[] = {
        0x00010404, 0x02042200, 0x00010405, 0x2410,
        0x00011c16, 0x08000420, 2, 2, 1, 0x03002822, 0x00101202, 0x01303fff,
        0x00010e80, 0x16a800, 0x41100004, 0,
        0x00010406, 0, 0x0001141b, 1, 0, 0, 0, 0,
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    const uint32_t white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    QTestState *qts = board_start("");

    /* Synthetic incomplete backing omits the captured upper border row. */
    texture_two_dimensional_border_upload(qts, true, true);
    texture_adapter_window(qts);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    odsy_test_pixel_write(qts, 4, 4, 0x123456ff);
    texture_adapter_point_rgba(qts, 4, 4, 0.25f, 0.25f, white);
    odsy_test_pixel_write(qts, 5, 4, 0x234567ff);
    texture_adapter_point_rgba(qts, 5, 4, 1.0f, 1.0f, white);

    g_assert_cmphex(odsy_test_pixel(qts, 4, 4), ==, 0x123456ff);
    g_assert_cmphex(odsy_test_pixel(qts, 5, 4), ==, 0x234567ff);
    board_quit(qts);
}

static void test_texture_two_dimensional_clamp_nearest(void)
{
    texture_two_dimensional_clamp(false, false);
}

static void test_texture_two_dimensional_clamp_linear(void)
{
    texture_two_dimensional_clamp(true, false);
}

static void test_texture_two_dimensional_stored_nearest(void)
{
    texture_two_dimensional_clamp(false, true);
}

static void test_texture_two_dimensional_stored_linear(void)
{
    texture_two_dimensional_clamp(true, true);
}

static void test_texture_chain_height_one(void)
{
    const uint32_t words[] = {
        0x00010405, 0x10,
        0x00011c16, 0x08000800, 4, 1, 1, 0x13000040, 0, 0,
        0x0001141b, 1, 0, 0, 0, 0,
        0x00010e80, 0x180000, 0x41100002, 0,
        0x00010e81, 0x181000, 0x41100002, 0,
        0x00010406, 0,
        0x00011013, 0, 0, 0x0100, 0x0100,
    };
    QTestState *qts = board_start("");

    texture_chain_upload_solid(qts, 0x180000, 4, 1, 0xff0000ff);
    texture_chain_upload_solid(qts, 0x181000, 2, 1, 0x0000ffff);
    texture_adapter_window(qts);
    odsy_test_pixel_write(qts, 10, 10, 0x12345678);
    push_user_words(qts, words, ARRAY_SIZE(words));
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x0000ffff);
    board_quit(qts);
}

static void test_texture_chain_construction(void)
{
    const uint32_t pixels[16] = {
        0xff0000ff, 0, 0x0000ffff, 0,
        0, 0, 0, 0,
        0x00ff00ff, 0, 0xffffffff, 0,
        0, 0, 0, 0,
    };
    const uint32_t words[] = {
        ODSY_CMD_DEST_SELECT, 0x181000, 2, 0x080024d0, 0,
        0x00010405, 0x10,
        0x00010ebc, 0x180000, 0x41100002, 0,
        0x00010406, 60,
        0x00011c16, 0x00000840, 4, 4, 0, 0x13640000, 0, 0,
        ODSY_CMD_GL_BEGIN, ODSY_GL_QUADS,
        ODSY_GL_TEX_RECT,
        0, 0, 0, 0,
        0, 0, 0,
        0x40000000, 0x40000000, 0,
        0, 0, 0x3f400000, 0x3f400000,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");

    upload_begin(qts, 0x180000, 2, 0x080024d0, 4, 4);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(16, ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, pixels, ARRAY_SIZE(pixels));
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 0), ==,
                    0xff0000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 2, 2), ==,
                    0xffffffff);
    odsy_test_pixel_write(qts, 10, 10, 0x12345678);
    push_user_words(qts, words, ARRAY_SIZE(words));
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 0, 0), ==,
                    0xff0000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 1, 0), ==,
                    0x0000ffff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 0, 1), ==,
                    0x00ff00ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 1, 1), ==,
                    0xffffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x12345678);

    uint32_t bad[ARRAY_SIZE(words)];
    memcpy(bad, words, sizeof(bad));
    bad[1] = 0x182000;
    bad[ARRAY_SIZE(bad) - 3] = 0x3f000000;
    push_user_words(qts, bad, ARRAY_SIZE(bad));
    g_assert_cmphex(odsy_test_object_texel(qts, 0x182000, 0, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x12345678);

    /*
     * pointblank's RGB form: word 4 0x12e40000 and entry format 0x00c.
     * The RGBA entry format under the RGB word stays refused.
     */
    for (unsigned form = 0; form < 2; form++) {
        uint32_t rgb[ARRAY_SIZE(words)];
        uint32_t dest = form ? 0x184000 : 0x183000;

        memcpy(rgb, words, sizeof(rgb));
        rgb[1] = dest;
        rgb[9] = form ? 0x41100002 : 0x00c00002;
        rgb[18] = 0x12e40000;
        push_user_words(qts, rgb, ARRAY_SIZE(rgb));
        g_assert_cmphex(odsy_test_object_texel(qts, dest, 1, 0), ==,
                        form ? 0 : 0x0000ffff);
        g_assert_cmphex(odsy_test_object_texel(qts, dest, 0, 1), ==,
                        form ? 0 : 0x00ff00ff);
    }

    /*
     * buttonfly's luminance-alpha form: word 4 0x11e40000 and entry format
     * 0x00b. The RGB entry format under the luminance-alpha word stays
     * refused.
     */
    for (unsigned form = 0; form < 2; form++) {
        uint32_t la[ARRAY_SIZE(words)];
        uint32_t dest = form ? 0x186000 : 0x185000;

        memcpy(la, words, sizeof(la));
        la[1] = dest;
        la[9] = form ? 0x00c00002 : 0x00b00002;
        la[18] = 0x11e40000;
        push_user_words(qts, la, ARRAY_SIZE(la));
        g_assert_cmphex(odsy_test_object_texel(qts, dest, 1, 0), ==,
                        form ? 0 : 0x0000ffff);
        g_assert_cmphex(odsy_test_object_texel(qts, dest, 1, 1), ==,
                        form ? 0 : 0xffffffff);
    }
    board_quit(qts);
}

/* Read the selected raw attribute-4 entry through the kernel FIFO. */
static void texture_coordinate_replay(QTestState *qts, uint64_t page)
{
    unsigned slot = (qtest_readl(qts, page + 0x84) >> 24) & 3;
    uint32_t command = qtest_readl(qts, page + 0x140 + 4 * slot);
    unsigned count = (command >> 10) & 0xf;

    g_assert_cmpuint(count, <=, 4);
    cmd_push(qts, command);
    for (unsigned i = 0; i < count; i++) {
        cmd_push(qts, qtest_readl(qts, page + 0x400 + 16 * slot + 4 * i));
    }
}

static void context_save_texture_coordinate_inactive(bool nonunit,
                                                     bool negative_zero)
{
    const uint64_t pages[2] = { ODSY_SHADOW_PAGE, ODSY_SHADOW_PAGE + 0x1000 };
    uint32_t coords[2][4] = {
        { 0x3f400000, 0x3e800000, 0x3f200000, 0x3f800000 },
        { 0x3e000000, 0x3f400000, 0x3f600000, 0x3f800000 },
    };
    uint32_t records[2][5];
    unsigned words[2];
    QTestState *qts = board_start("");

    if (nonunit) {
        coords[0][3] = 0x40000000;
        coords[1][3] = 0x3f000000;
    }
    if (negative_zero) {
        coords[0][2] = 0x80000000;
        coords[1][2] = 0;
    }
    for (unsigned owner = 0; owner < 2; owner++) {
        words[owner] = nonunit ? 5 : coords[owner][2] ? 4 : 3;
        records[owner][0] = nonunit ? 0xc380d025 :
                            coords[owner][2] ? 0xc380cc23 : 0xc380c821;
        memcpy(&records[owner][1], coords[owner], 4 * sizeof(uint32_t));
    }
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    for (unsigned slot = 0; slot < 4; slot++) {
        for (unsigned owner = 0; owner < 2; owner++) {
            uint64_t page = pages[owner];
            uint32_t descriptor = (0xa5a5a5a5 & ~0x03000000) | (slot << 24);

            qtest_memset(qts, page, 0xa5, 4096);
            qtest_writel(qts, page + 0x84, descriptor);
            qtest_writel(qts, page + 0x88, 0x5a5a5a5a);
            qtest_writel(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
            /* Texture targets are inactive, independently of current STRQ. */
            push_user_words(qts, (uint32_t[]) { ODSY_REG_TEX_MODE, 0 }, 2);
            push_user_words(qts, (uint32_t[]) {
                0xc380d025, coords[owner][0], coords[owner][1],
                coords[owner][2], coords[owner][3],
            }, 5);
            reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
            qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                         (UINT64_C(7) << 40) |
                         ((uint64_t)(owner ? 1 : 2) << 16) | 1);
            g_assert_cmphex(qtest_readl(qts, page +
                                        ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
            g_assert_cmphex(qtest_readl(qts, page + 0x84), ==, 0);
            g_assert_cmphex(qtest_readl(qts, page + 0x88), ==, 0);
            g_assert_cmphex(qtest_readl(qts, page + 0x140), ==,
                            records[owner][0]);
            for (unsigned i = 0; i < 4; i++) {
                g_assert_cmphex(qtest_readl(qts, page + 0x400 + 4 * i), ==,
                                i < words[owner] - 1 ? coords[owner][i] :
                                0xa5a5a5a5);
            }
            for (unsigned other = 1; other < 4; other++) {
                g_assert_cmphex(qtest_readl(qts, page + 0x140 + 4 * other),
                                ==, 0xa5a5a5a5);
                for (unsigned i = 0; i < 4; i++) {
                    g_assert_cmphex(qtest_readl(qts, page + 0x400 +
                                                16 * other + 4 * i), ==,
                                    0xa5a5a5a5);
                }
            }
        }
        for (unsigned owner = 0; owner < 2; owner++) {
            texture_coordinate_replay(qts, pages[owner]);
            texture_current_query(qts, records[owner], words[owner],
                                  2, owner ? 8 : 5, 1);
        }
        /* The restored R must come from guest RAM, including its raw sign. */
        qtest_writel(qts, pages[0] + 0x408, 0x3ec00000);
        records[0][3] = 0x3ec00000;
        texture_coordinate_replay(qts, pages[0]);
        texture_current_query(qts, records[0], words[0], 2, 5, 1);
        records[0][3] = coords[0][2];
    }
    board_quit(qts);
}

static void test_context_save_texture_coordinate_inactive_r(void)
{
    context_save_texture_coordinate_inactive(false, false);
}

static void test_context_save_texture_coordinate_inactive_q(void)
{
    context_save_texture_coordinate_inactive(true, false);
}

static void test_context_save_texture_coordinate_inactive_negative_zero_r(void)
{
    context_save_texture_coordinate_inactive(false, true);
}

static void test_context_save_texture_coordinate(void)
{
    const uint32_t pixels[4] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint64_t pages[2] = { ODSY_SHADOW_PAGE, ODSY_SHADOW_PAGE + 0x1000 };
    const uint32_t coords[2][2] = {
        { 0x80000000, 0x3f400000 }, /* A: negative zero, 0.75 -> blue. */
        { 0x3f400000, 0x3e800000 }, /* B: 0.75, 0.25 -> green. */
    };
    uint32_t saved[4][2][3], raw[4][5], shown[4][5], reset[3];
    QTestState *qts = board_start("");
    const uint32_t word = 0x140, ops = 0x400;

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    display_identity(qts, 0);
    /*
     * slot is the selector the page held before the save. The save writes
     * zero descriptors and the coordinate to slot 0 whatever it was.
     */
    for (unsigned slot = 0; slot < 4; slot++) {
        uint32_t descriptor = (0xa5a5a5a5 & ~0x03000000) | (slot << 24);

        for (unsigned owner = 0; owner < 2; owner++) {
            uint64_t page = pages[owner];
            uint64_t incoming = owner ? 1 : 2;

            qtest_memset(qts, page, 0xa5, 4096);
            qtest_writel(qts, page + 0x84, descriptor);
            qtest_writel(qts, page + 0x88, 0x5a5a5a5a);
            /* Valid stale data makes a missing save leak the other owner. */
            qtest_writel(qts, page + word, 0xc380c821);
            qtest_writel(qts, page + ops, coords[1 - owner][0]);
            qtest_writel(qts, page + ops + 4, coords[1 - owner][1]);
            qtest_writel(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
            user_cmd_push(qts, 0xc380c821);
            push_user_words(qts, coords[owner], 2);
            texture_adapter_current_point(qts, 10 + owner, 10 + slot);
            raw[slot][owner] = odsy_test_pixel(qts, 10 + owner, 10 + slot);
            shown[slot][owner] = odsy_test_display_pixel(qts, 10 + owner,
                                                        10 + slot);
            reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
            qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                         (UINT64_C(7) << 40) | (incoming << 16) | 1);
            g_assert_cmphex(qtest_readl(qts, page +
                                        ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
            g_assert_cmphex(qtest_readl(qts, page + ODSY_HW_PIPE_STATE),
                            ==, 0xf);
            saved[slot][owner][0] = qtest_readl(qts, page + word);
            saved[slot][owner][1] = qtest_readl(qts, page + ops);
            saved[slot][owner][2] = qtest_readl(qts, page + ops + 4);
            g_assert_cmphex(qtest_readl(qts, page + 0x84), ==, 0);
            g_assert_cmphex(qtest_readl(qts, page + 0x88), ==, 0);
            for (unsigned other = 1; other < 4; other++) {
                g_assert_cmphex(qtest_readl(qts, page + 0x140 + 4 * other),
                                ==, 0xa5a5a5a5);
                for (unsigned i = 0; i < 4; i++) {
                    g_assert_cmphex(qtest_readl(qts, page + 0x400 +
                                                16 * other + 4 * i), ==,
                                    0xa5a5a5a5);
                }
            }
            g_assert_cmphex(qtest_readl(qts, page + ops + 8), ==, 0xa5a5a5a5);
            g_assert_cmphex(qtest_readl(qts, page + ops + 12), ==, 0xa5a5a5a5);
            g_test_message("slot %u owner %u saved %08x %08x %08x; "
                           "pixel %08x/%06x", slot, owner + 1,
                           saved[slot][owner][0], saved[slot][owner][1],
                           saved[slot][owner][2], raw[slot][owner],
                           shown[slot][owner]);
        }
        /* Replay attr4 required when restoring different nonzero owners. */
        for (unsigned owner = 0; owner < 2; owner++) {
            texture_coordinate_replay(qts, pages[owner]);
            texture_adapter_current_point(qts, 12 + owner, 10 + slot);
            raw[slot][2 + owner] = odsy_test_pixel(qts, 12 + owner, 10 + slot);
            shown[slot][2 + owner] = odsy_test_display_pixel(qts, 12 + owner,
                                                            10 + slot);
            g_test_message("slot %u owner %u replay pixel %08x/%06x", slot,
                           owner + 1, raw[slot][2 + owner],
                           shown[slot][2 + owner]);
        }
        /* Guest RAM edits govern replay, independently of any host cache. */
        qtest_writel(qts, pages[0] + ops, 0x3f400000);
        qtest_writel(qts, pages[0] + ops + 4, 0x3f400000);
        texture_coordinate_replay(qts, pages[0]);
        texture_adapter_current_point(qts, 14, 10 + slot);
        raw[slot][4] = odsy_test_pixel(qts, 14, 10 + slot);
        shown[slot][4] = odsy_test_display_pixel(qts, 14, 10 + slot);
        g_test_message("slot %u edited RAM replay pixel %08x/%06x", slot,
                       raw[slot][4], shown[slot][4]);
    }
    qtest_system_reset(qts);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, pages[0]);
    qtest_writel(qts, pages[0] + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(0x7f) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, pages[0] +
                                ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
    reset[0] = qtest_readl(qts, pages[0] + word);
    reset[1] = qtest_readl(qts, pages[0] + ops);
    reset[2] = qtest_readl(qts, pages[0] + ops + 4);
    g_test_message("reset saved %08x %08x %08x", reset[0], reset[1], reset[2]);

    for (unsigned slot = 0; slot < 4; slot++) {
        g_assert_cmphex(raw[slot][0], ==, 0x0000ffff);
        g_assert_cmphex(raw[slot][1], ==, 0x00ff00ff);
        g_assert_cmphex(shown[slot][0], ==, 0x0000ff);
        g_assert_cmphex(shown[slot][1], ==, 0x00ff00);
        g_assert_cmphex(raw[slot][2], ==, 0x0000ffff);
        g_assert_cmphex(raw[slot][3], ==, 0x00ff00ff);
        g_assert_cmphex(shown[slot][2], ==, 0x0000ff);
        g_assert_cmphex(shown[slot][3], ==, 0x00ff00);
        g_assert_cmphex(raw[slot][4], ==, 0xffffffff);
        g_assert_cmphex(shown[slot][4], ==, 0xffffff);
        for (unsigned owner = 0; owner < 2; owner++) {
            g_assert_cmphex(saved[slot][owner][0], ==, 0xc380c821);
            g_assert_cmphex(saved[slot][owner][1], ==, coords[owner][0]);
            g_assert_cmphex(saved[slot][owner][2], ==, coords[owner][1]);
        }
    }
    g_assert_cmphex(reset[0], ==, 0xc380c821);
    g_assert_cmphex(reset[1], ==, 0);
    g_assert_cmphex(reset[2], ==, 0);
    board_quit(qts);
}

/*
 * Every save writes the ten descriptor words +0x84 to +0xab, zero while idle,
 * whatever the page held, and the coordinate goes to ring 4 slot 0. A
 * descriptor in absent memory is a failed store like any other: reported
 * once, and the rest of the save and completion are still written.
 */
static void test_context_save_idle_descriptors(void)
{
    static const uint32_t coord[] = { 0xc380c821, 0x3e800000, 0x80000000 };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    const uint64_t p = ODSY_SHADOW_PAGE;
    const uint64_t page = 0x3fffff00;
    QTestState *qts = shadow_start("");
    uint32_t done, colour;
    int fd;

    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < 10; i++) {
            qtest_writel(qts, p + 0x84 + 4 * i, 0xa5a5a5a5 ^ i);
        }
        /*
         * A sentinel past the last descriptor word: an over-long store
         * loop overwrites it.
         */
        qtest_writel(qts, p + 0xac, 0xa5a5a5a5 ^ 10);
        qtest_writel(qts, p + 0x140, 0x5a5a5a5a);
        qtest_writel(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
        qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
        g_assert_cmphex(qtest_readl(qts, p + ODSY_CTX_SWITCH_DONE_OFFSET),
                        ==, 1);
        for (unsigned i = 0; i < 10; i++) {
            g_assert_cmphex(qtest_readl(qts, p + 0x84 + 4 * i), ==, 0);
        }
        g_assert_cmphex(qtest_readl(qts, p + 0xac), ==, 0xa5a5a5a5 ^ 10);
        g_assert_cmphex(qtest_readl(qts, p + 0x140), ==, 0xc380c821);
        g_assert_cmphex(qtest_readl(qts, p + 0x400), ==,
                        pass ? coord[1] : 0);
        g_assert_cmphex(qtest_readl(qts, p + 0x404), ==,
                        pass ? coord[2] : 0);
        push_user_words(qts, coord, ARRAY_SIZE(coord));
    }
    board_quit(qts);

    fd = g_file_open_tmp("odyssey-descriptor-store-XXXXXX", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors -D %s", quoted);
    qts = board_start(extra);
    /* Bank 0 is absent; bank 1 holds completion and saved attributes. */
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 UINT64_C(0x32));
    qtest_writel(qts, page + ODSY_HW_COLOUR_WORD, 0xa5a5a5a5);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    for (unsigned pass = 0; pass < 2; pass++) {
        qtest_writel(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET, 0x12345678);
        qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
        done = qtest_readl(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET);
        colour = qtest_readl(qts, page + ODSY_HW_COLOUR_WORD);
        g_assert_cmphex(done, ==, 1);
        g_assert_cmphex(colour, ==, 0xc580cc0e);
    }
    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_cmpuint(count_substr(log, "context shadow store"), ==, 1);
    g_assert_null(strstr(log, "descriptor read"));
}

static void test_context_save_texture_coordinate_active_primitive(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    const uint64_t page = 0x3fffff00;
    QTestState *qts;
    uint32_t done;
    int fd;

    fd = g_file_open_tmp("odyssey-texture-open-XXXXXX", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors,unimp -D %s", quoted);
    qts = board_start(extra);
    /*
     * Bank 0 is absent, so the descriptor store fails; all four texture
     * entries stay writable.
     */
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 UINT64_C(0x32));
    for (unsigned slot = 0; slot < 4; slot++) {
        qtest_writel(qts, page + 0x140 + 4 * slot, 0xa5a5a5a5);
        for (unsigned i = 0; i < 4; i++) {
            qtest_writel(qts, page + 0x400 + 16 * slot + 4 * i, 0x5a5a5a5a);
        }
    }
    qtest_writel(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET, 0x12345678);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    user_cmd_push(qts, 0xc380c821);
    user_cmd_push(qts, 0x3f400000);
    user_cmd_push(qts, 0x3e800000);
    user_cmd_push(qts, ODSY_CMD_GL_BEGIN);
    user_cmd_push(qts, ODSY_GL_TRIANGLES);
    /* The shared assembler opens on its first float vertex. */
    user_cmd_push(qts, ODSY_GL_VERTEX_2F);
    user_cmd_push(qts, 0);
    user_cmd_push(qts, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_TO_1);
    done = qtest_readl(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET);
    for (unsigned slot = 0; slot < 4; slot++) {
        g_assert_cmphex(qtest_readl(qts, page + 0x140 + 4 * slot), ==,
                        slot == 0 || slot == 3 ? 0xc380c821 : 0xa5a5a5a5);
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmphex(qtest_readl(qts, page + 0x400 +
                                        16 * slot + 4 * i), ==,
                            (slot == 0 || slot == 3) && i < 2 ?
                            (i ? 0x3e800000 : 0x3f400000) : 0x5a5a5a5a);
        }
    }
    user_cmd_push(qts, ODSY_CMD_GL_END);
    board_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_test_message("active primitive: completion %08x; %s", done, log);
    g_assert_cmphex(done, ==, 1);
    g_assert_null(strstr(log, "context switch inside primitive 0x4"));
    g_assert_cmpuint(count_substr(log, "context shadow store"), ==, 1);
}

static void test_texture_adapter_rgba12_byte_upload(void)
{
    const uint32_t row0[2] = { 0xff0000ff, 0x00ff00ff };
    const uint32_t row1[2] = { 0x0000ffff, 0xffffffff };
    QTestState *qts = board_start("");

    upload_begin(qts, 0x180000, 2, 0x080024d0, 2, 2);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    user_cmd_push(qts, 0x0f00116b);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    user_cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, row0, ARRAY_SIZE(row0));
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    user_cmd_push(qts, 0x0f21116b);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    user_cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, row1, ARRAY_SIZE(row1));
    upload_end(qts);

    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 0), ==,
                    row0[0]);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 1, 0), ==,
                    row0[1]);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 1), ==,
                    row1[0]);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 1, 1), ==,
                    row1[1]);
    board_quit(qts);
}

static void test_texture_adapter_nearest(void)
{
    const uint32_t pixels[4] = {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    texture_adapter_point(qts, 11, 10, 0.75f, 0.25f);
    texture_adapter_point(qts, 10, 11, 0.25f, 0.75f);
    texture_adapter_point(qts, 11, 11, 0.75f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x00ff0080);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 11), ==, 0x0000ffc0);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 11), ==, 0xffffffff);
    board_quit(qts);
}

static void test_texture_adapter_detail_zero_identity(void)
{
    const uint32_t primary[4] = {
        0x8040c080, 0x8040c080, 0x8040c080, 0x8040c080,
    };
    const uint32_t white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    const uint32_t alternate[4] = {
        0x2160a040, 0x2160a040, 0x2160a040, 0x2160a040,
    };
    const uint32_t detail[4] = {
        0xff0000ff, 0x00ffffff, 0xffff0000, 0x0000ffff,
    };
    const uint32_t detail_rectangular[8] = {
        0xff0000ff, 0x00ffffff, 0xffff0000, 0x0000ffff,
        0x00ff00ff, 0xff00ff00, 0x000000ff, 0xffffffff,
    };
    const uint32_t state[] = {
        0x00010405, 0x10,
        0x00011c16, 0x04108000, 1, 1, 1, 0x13000044, 0, 0,
        0x0001141b, 2, 0, 0, 0, 0,
        0x00010e80, 0x0016a800, 0x41100002, 0,
        0x00010ebe, 0x0016a880, 0x41100002, 0,
        0x00010406, 0,
    };
    const uint32_t ordinary_sampler[] = {
        0x00011c16, 0x08000000, 1, 1, 1, 0x13000040, 0, 0,
    };
    const uint32_t quarter_modes[][8] = {
        { 0x00011c16, 0x04108000, 1, 1, 1, 0x13000044, 0, 0 },
        { 0x00011c16, 0x84108000, 1, 1, 1, 0x13000044, 0, 0 },
        { 0x00011c16, 0x04108000, 1, 1, 1, 0x13000048, 0, 0 },
        { 0x00011c16, 0x04108000, 1, 1, 1, 0x13000046, 0, 0 },
    };
    const uint32_t rectangular_sampler[][8] = {
        { 0x00011c16, 0x04208000, 1, 1, 1, 0x13000044, 0, 0 },
        { 0x00011c16, 0x04110000, 1, 1, 1, 0x13000044, 0, 0 },
    };
    const uint32_t rectangular_ordinary[][22] = {
        { 0x00010405, 0x10, 0x00011c16, 0x08000820, 4, 2, 1,
          0x13000000, 0, 0, 0x0001141b, 1, 0, 0, 0, 0,
          0x00010e80, 0x0016a880, 0x41100002, 0, 0x00010406, 0 },
        { 0x00010405, 0x10, 0x00011c16, 0x08000440, 2, 4, 1,
          0x13000000, 0, 0, 0x0001141b, 1, 0, 0, 0, 0,
          0x00010e80, 0x0016a880, 0x41100002, 0, 0x00010406, 0 },
    };
    /* GL_DITHER stays disabled; the weighted ADD outputs round half-up. */
    const uint32_t rectangular_expected[8] = {
        0xc00080c0, 0x4080ffc0, 0xc0808040, 0x4000ffc0,
        0x408080c0, 0xc000ff40, 0x400080c0, 0xc080ffc0,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    uint32_t primary_pixel, alternate_pixel;
    uint32_t ordinary_pixel, alternate_ordinary_pixel, missing_pixel;
    uint32_t detail_pixels[ARRAY_SIZE(quarter_modes)];
    uint32_t rectangular_pixels[2][8];
    uint32_t rectangular_ordinary_pixels[2][8];
    uint32_t rectangular_zero_pixels[2][8];
    uint32_t detail_fixture_pixel;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-detail-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    upload_begin(qts, 0x16a800, 2, 0x080024d0, 1, 1);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, primary[0]);
    upload_end(qts);
    texture_adapter_upload(qts, 0x16a880, detail);
    /* IMAGE_BURST seeds these destinations through the image primitive. */
    texture_adapter_upload_detail_dimensions(qts, 0x16a900, 4, 2,
                                              detail_rectangular);
    texture_adapter_upload_detail_dimensions(qts, 0x16a980, 2, 4,
                                              detail_rectangular);
    texture_adapter_window(qts);
    push_user_words(qts, state, ARRAY_SIZE(state));
    texture_adapter_upload_detail_function(qts, 0);
    texture_adapter_point(qts, 10, 10, 0.125f, 0.125f);
    primary_pixel = odsy_test_pixel(qts, 10, 10);
    push_user_words(qts, ordinary_sampler, ARRAY_SIZE(ordinary_sampler));
    texture_adapter_point(qts, 14, 10, 0.125f, 0.125f);
    ordinary_pixel = odsy_test_pixel(qts, 14, 10);
    texture_adapter_bind(qts, 0x16a880, 0x13000000, 1);
    texture_adapter_point(qts, 12, 10, 0.25f, 0.25f);
    detail_fixture_pixel = odsy_test_pixel(qts, 12, 10);

    for (unsigned mode = 0; mode < ARRAY_SIZE(quarter_modes); mode++) {
        push_user_words(qts, state, ARRAY_SIZE(state));
        push_user_words(qts, quarter_modes[mode],
                        ARRAY_SIZE(quarter_modes[mode]));
        texture_adapter_upload_detail_function(qts, 0x00400000);
        texture_adapter_point(qts, 16 + mode, 10, 0.125f, 0.125f);
        detail_pixels[mode] = odsy_test_pixel(qts, 16 + mode, 10);
    }

    for (unsigned shape = 0; shape < ARRAY_SIZE(rectangular_sampler); shape++) {
        uint32_t width = shape ? 2 : 4;
        uint32_t height = shape ? 4 : 2;
        uint32_t base = shape ? 0x16a980 : 0x16a900;
        uint32_t ordinary_state[ARRAY_SIZE(rectangular_ordinary[shape])];
        uint32_t detail_state[ARRAY_SIZE(state)];
        unsigned sample = 0;

        memcpy(ordinary_state, rectangular_ordinary[shape],
               sizeof(ordinary_state));
        ordinary_state[17] = base;
        push_user_words(qts, ordinary_state, ARRAY_SIZE(ordinary_state));
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                float s = (x + 0.5f) / width;
                float t = (y + 0.5f) / height;
                unsigned pixel_x = 24 + sample;

                texture_adapter_point_rgba(qts, pixel_x, 12 + shape, s, t,
                                           white);
                rectangular_ordinary_pixels[shape][sample++] =
                    odsy_test_pixel(qts, pixel_x, 12 + shape);
            }
        }

        memcpy(detail_state, state, sizeof(detail_state));
        detail_state[21] = base;
        push_user_words(qts, detail_state, ARRAY_SIZE(detail_state));
        push_user_words(qts, rectangular_sampler[shape],
                        ARRAY_SIZE(rectangular_sampler[shape]));
        texture_adapter_upload_detail_function(qts, 0);
        sample = 0;
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                float s = (x + 0.5f) / 4;
                float t = (y + 0.5f) / 4;
                unsigned pixel_x = 24 + sample;

                texture_adapter_point_rgba(qts, pixel_x, 10 + shape, s, t,
                                           white);
                rectangular_zero_pixels[shape][sample++] =
                    odsy_test_pixel(qts, pixel_x, 10 + shape);
            }
        }
        texture_adapter_upload_detail_function(qts, 0x00400000);
        sample = 0;
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                float s = (x + 0.5f) / 4;
                float t = (y + 0.5f) / 4;
                unsigned pixel_x = 24 + sample;

                texture_adapter_point_rgba(qts, pixel_x, 10 + shape, s, t,
                                           white);
                rectangular_pixels[shape][sample++] =
                    odsy_test_pixel(qts, pixel_x, 10 + shape);
            }
        }
    }

    upload_begin(qts, 0x16a800, 2, 0x080024d0, 1, 1);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, alternate[0]);
    upload_end(qts);
    push_user_words(qts, state, ARRAY_SIZE(state));
    texture_adapter_upload_detail_function(qts, 0);
    texture_adapter_point(qts, 11, 10, 0.125f, 0.125f);
    alternate_pixel = odsy_test_pixel(qts, 11, 10);
    push_user_words(qts, ordinary_sampler, ARRAY_SIZE(ordinary_sampler));
    texture_adapter_point(qts, 15, 10, 0.125f, 0.125f);
    alternate_ordinary_pixel = odsy_test_pixel(qts, 15, 10);

    odsy_test_pixel_write(qts, 13, 10, 0x2468ace0);
    push_user_words(qts, state, ARRAY_SIZE(state));
    texture_adapter_upload_detail_function(qts, 0);
    user_cmd_push(qts, 0x00010ebe);
    push_user_words(qts, (uint32_t[]) { 0, 0, 0 }, 3);
    texture_adapter_point(qts, 13, 10, 0.125f, 0.125f);
    missing_pixel = odsy_test_pixel(qts, 13, 10);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    for (unsigned shape = 0; shape < ARRAY_SIZE(rectangular_pixels); shape++) {
        for (unsigned i = 0; i < ARRAY_SIZE(rectangular_pixels[shape]); i++) {
            g_assert_cmphex(rectangular_ordinary_pixels[shape][i], ==,
                            detail_rectangular[i]);
            g_assert_cmphex(rectangular_zero_pixels[shape][i], ==,
                            primary[0]);
        }
    }
    /*
     * The captured 0x4b image header is unmodeled; RGB bursts use the XRGB
     * fallback, so pin the uploaded texels through the ordinary sampler.
     */
    g_assert_cmphex(primary_pixel, ==, 0x80400080);
    g_assert_cmphex(ordinary_pixel, ==, 0x80400080);
    g_assert_cmphex(detail_fixture_pixel, ==, 0xff0000ff);
    g_assert_null(strstr(log,
                        "command FIFO word 0x00013e40 is not modeled"));
    g_assert_null(strstr(log,
                        "command FIFO word 0x00003e40 is not modeled"));
    g_assert_cmphex(primary_pixel, ==, ordinary_pixel);
    g_assert_cmphex(detail_pixels[0], ==, 0xc00000c0);
    g_assert_cmphex(detail_pixels[1], ==, 0xa03000a0);
    g_assert_cmphex(detail_pixels[2], ==, 0xc0000080);
    g_assert_cmphex(detail_pixels[3], ==, 0x804000c0);
    for (unsigned shape = 0; shape < ARRAY_SIZE(rectangular_pixels); shape++) {
        for (unsigned i = 0; i < ARRAY_SIZE(rectangular_expected); i++) {
            g_assert_cmphex(rectangular_pixels[shape][i], ==,
                            rectangular_expected[i]);
        }
    }
    g_assert_cmphex(alternate_pixel, ==, alternate_ordinary_pixel);
    g_assert_cmphex(missing_pixel, ==, 0x2468ace0);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_texture_adapter_detail_zero_alpha_reference(void)
{
    const uint32_t base = 0x8040c080;
    const uint32_t detail[4] = {
        0xff0000ff, 0x00ffffff, 0xffff0000, 0x0000ffff,
    };
    const uint32_t state[] = {
        0x00010405, 0x10,
        0x00011c16, 0x04108000, 1, 1, 1, 0x13000044, 0, 0,
        0x0001141b, 2, 0, 0, 0, 0,
        0x00010e80, 0x0016a800, 0x41100002, 0,
        0x00010ebe, 0x0016a880, 0x41100002, 0,
        0x00010406, 0,
    };
    const uint32_t ordinary_sampler[] = {
        0x00011c16, 0x08000000, 1, 1, 1, 0x13000040, 0, 0,
    };
    QTestState *qts = board_start("");

    upload_begin(qts, 0x16a800, 2, 0x080024d0, 1, 1);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, base);
    upload_end(qts);
    texture_adapter_upload(qts, 0x16a880, detail);
    texture_adapter_window(qts);
    push_user_words(qts, state, ARRAY_SIZE(state));
    texture_adapter_upload_detail_function(qts, 0);

    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    push_user_words(qts, ordinary_sampler, ARRAY_SIZE(ordinary_sampler));
    texture_adapter_point(qts, 10, 10, 0.125f, 0.125f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x80400080);

    set_reg(qts, ODSY_REG_GL_MODE, 0x02046200);
    set_reg(qts, ODSY_REG_ALPHA_FUNC, 0x4c00);
    texture_adapter_point(qts, 11, 10, 0.125f, 0.125f);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x0000ffff);

    set_reg(qts, ODSY_REG_ALPHA_FUNC, 0x4800);
    texture_adapter_point(qts, 12, 10, 0.125f, 0.125f);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, 0x80400080);

    push_user_words(qts, state, ARRAY_SIZE(state));
    texture_adapter_upload_detail_function(qts, 0);
    set_reg(qts, ODSY_REG_GL_MODE, 0x02046200);
    set_reg(qts, ODSY_REG_ALPHA_FUNC, 0x4800);
    texture_adapter_point(qts, 13, 10, 0.125f, 0.125f);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), ==, 0x80400080);

    board_quit(qts);
}

/*
 * A 2x2-pixel quad whose four vertices share one texture coordinate. Its
 * derivatives are zero, so the sampler takes the minimum LOD; a point would
 * not, because the shared rasterizer gives points a fixed footprint.
 */
static void texture_adapter_flat_quad(QTestState *qts, int x, int y,
                                      float s, float t)
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, 0,
        ODSY_CMD_GL_BEGIN, 7,
        0xc380c821, bitmap_rect_float(s), bitmap_rect_float(t),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x), bitmap_rect_float(y),
        0xc380c821, bitmap_rect_float(s), bitmap_rect_float(t),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 2), bitmap_rect_float(y),
        0xc380c821, bitmap_rect_float(s), bitmap_rect_float(t),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x + 2),
        bitmap_rect_float(y + 2),
        0xc380c821, bitmap_rect_float(s), bitmap_rect_float(t),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x), bitmap_rect_float(y + 2),
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

/*
 * The stock driver sends the two-point function (-4, 1/4), (0, 1/2) as a
 * table holding 1/2 at entry 0, falling to 1/4 at entry 128 and holding 1/4
 * to the end. SGIS_detail_texture fixes the weight beyond each end point, so
 * a draw at the lowest LOD must use 1/4 and a draw at LOD 0 must use 1/2.
 * Weights between the ends are not known and must be refused.
 */
static void test_texture_adapter_detail_function_endpoints(void)
{
    const uint32_t detail[4] = {
        0xff0000ff, 0x00ffffff, 0xffff0000, 0x0000ffff,
    };
    const uint32_t state[] = {
        0x00010405, 0x10,
        0x00011c16, 0x04108000, 1, 1, 1, 0x13000044, 0, 0,
        0x0001141b, 2, 0, 0, 0, 0,
        0x00010e80, 0x0016a800, 0x41100002, 0,
        0x00010ebe, 0x0016a880, 0x41100002, 0,
        0x00010406, 0,
    };
    /* Minimum LOD -15, the reset value, then 0 and -2; maximum LOD 15. */
    const uint32_t lod_lowest[] = { 0x00011013, 0, 0, 0x3100, 0x0f00 };
    const uint32_t lod_zero[] = { 0x00011013, 0, 0, 0, 0x0f00 };
    const uint32_t lod_between[] = { 0x00011013, 0, 0, 0x3e00, 0x0f00 };
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    uint32_t words[16];
    uint32_t quarter, half, lowest, zero, between;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-detail-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    upload_begin(qts, 0x16a800, 2, 0x080024d0, 1, 1);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, 0x8040c080);
    upload_end(qts);
    texture_adapter_upload(qts, 0x16a880, detail);
    texture_adapter_window(qts);

    push_user_words(qts, state, ARRAY_SIZE(state));
    texture_adapter_upload_detail_function(qts, 0x00400000);
    texture_adapter_flat_quad(qts, 10, 10, 0.125f, 0.125f);
    quarter = odsy_test_pixel(qts, 10, 10);
    texture_adapter_upload_detail_function(qts, 0x00800000);
    texture_adapter_flat_quad(qts, 12, 10, 0.125f, 0.125f);
    half = odsy_test_pixel(qts, 12, 10);

    for (unsigned block = 0; block < 16; block++) {
        for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
            unsigned entry = block * ARRAY_SIZE(words) + i;

            words[i] = entry < 128 ? 0x00800000 - entry * 0x8000 :
                                     0x00400000;
        }
        user_cmd_push(qts, 0x00013e40 | block);
        push_user_words(qts, words, ARRAY_SIZE(words));
    }

    odsy_test_pixel_write(qts, 14, 10, 0x2468ace0);
    push_user_words(qts, lod_lowest, ARRAY_SIZE(lod_lowest));
    texture_adapter_flat_quad(qts, 14, 10, 0.125f, 0.125f);
    lowest = odsy_test_pixel(qts, 14, 10);

    odsy_test_pixel_write(qts, 16, 10, 0x2468ace0);
    push_user_words(qts, lod_zero, ARRAY_SIZE(lod_zero));
    texture_adapter_flat_quad(qts, 16, 10, 0.125f, 0.125f);
    zero = odsy_test_pixel(qts, 16, 10);

    odsy_test_pixel_write(qts, 18, 10, 0x2468ace0);
    push_user_words(qts, lod_between, ARRAY_SIZE(lod_between));
    texture_adapter_flat_quad(qts, 18, 10, 0.125f, 0.125f);
    between = odsy_test_pixel(qts, 18, 10);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    /* The constant-table controls must themselves differ. */
    g_assert_cmphex(quarter, ==, 0xc00000c0);
    g_assert_cmphex(half, !=, quarter);
    g_assert_cmphex(lowest, ==, quarter);
    g_assert_cmphex(zero, ==, half);
    g_assert_cmphex(between, ==, 0x2468ace0);
    g_assert_nonnull(strstr(log, "invalid filter weight"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_texture_adapter_detail_rectangular_migration(void)
{
    const uint32_t primary[4] = {
        0x8040c080, 0x8040c080, 0x8040c080, 0x8040c080,
    };
    const uint32_t white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    const uint32_t detail[8] = {
        0xff0000ff, 0x00ffffff, 0xffff0000, 0x0000ffff,
        0x00ff00ff, 0xff00ff00, 0x000000ff, 0xffffffff,
    };
    const uint32_t state[] = {
        0x00010405, 0x10,
        0x00011c16, 0x04108000, 1, 1, 1, 0x13000044, 0, 0,
        0x0001141b, 2, 0, 0, 0, 0,
        0x00010e80, 0x0016a800, 0x41100002, 0,
        0x00010ebe, 0x0016a900, 0x41100002, 0,
        0x00010406, 0,
    };
    const uint32_t rectangular_sampler[] = {
        0x00011c16, 0x04208000, 1, 1, 1, 0x13000044, 0, 0,
    };
    const uint32_t expected = 0xc00080c0;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-detail-migration-XXXXXX", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_adapter_upload(from, 0x16a800, primary);
    texture_adapter_upload_detail_dimensions(from, 0x16a900, 4, 2, detail);
    texture_adapter_window(from);
    push_user_words(from, state, ARRAY_SIZE(state));
    push_user_words(from, rectangular_sampler,
                    ARRAY_SIZE(rectangular_sampler));
    texture_adapter_upload_detail_function(from, 0x00400000);

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    /* The migrated sampler and detail table must replace this sentinel. */
    odsy_test_pixel_write(to, 64, 64, 0x12345678);
    texture_adapter_point_rgba(to, 64, 64, 0.125f, 0.125f, white);
    g_assert_cmphex(odsy_test_pixel(to, 64, 64), ==, expected);

    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_nearest_clamp(void)
{
    const uint32_t pixels[4] = {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);

    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    texture_adapter_point(qts, 10, 10, -0.25f, 0.25f);
    texture_adapter_point(qts, 11, 10, 1.25f, 0.25f);
    texture_adapter_point(qts, 12, 10, 0.25f, -0.25f);
    texture_adapter_point(qts, 13, 10, 0.25f, 1.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x00ff0080);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, 0x0000ffc0);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), ==, 0xff000040);

    texture_adapter_bind(qts, 0x180000, 0x13001400, 1);
    texture_adapter_point(qts, 14, 10, -0.25f, 0.25f);
    texture_adapter_point(qts, 15, 10, 1.25f, 0.25f);
    texture_adapter_point(qts, 16, 10, 0.25f, -0.25f);
    texture_adapter_point(qts, 17, 10, 0.25f, 1.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 15, 10), ==, 0x00ff0080);
    g_assert_cmphex(odsy_test_pixel(qts, 16, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 17, 10), ==, 0x0000ffc0);

    texture_adapter_bind(qts, 0x180000, 0x13002800, 1);
    texture_adapter_point(qts, 18, 10, -0.25f, 0.25f);
    texture_adapter_point(qts, 19, 10, 1.25f, 0.25f);
    texture_adapter_point(qts, 20, 10, 0.25f, -0.25f);
    texture_adapter_point(qts, 21, 10, 0.25f, 1.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 18, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 19, 10), ==, 0x00ff0080);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 21, 10), ==, 0x0000ffc0);

    /*
     * Linear magnification under GL_CLAMP: a tap past the edge takes the
     * border colour, words 5 and 6 (zero here), as OpenGL 1.2.1 3.8.4
     * gives, not the edge texel clamp to edge would repeat.
     */
    texture_adapter_bind(qts, 0x180000, 0x13002802, 1);
    texture_adapter_point(qts, 22, 10, 0.25f, 0.25f);
    texture_adapter_point(qts, 23, 10, 0.5f, 0.25f);
    texture_adapter_point(qts, 24, 10, 0.0f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 22, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 23, 10), ==, 0x80800060);
    g_assert_cmphex(odsy_test_pixel(qts, 24, 10), ==, 0x80000020);
    board_quit(qts);
}

/*
 * Clamp to edge (wrap field 1) on an axis comes with 0x4 in bits 23:16 of
 * that axis's size word, as Performer's town sends; the flag is not part
 * of the size. The flag on a repeat axis is still refused.
 */
static void test_texture_adapter_edge_clamp_size_flag(void)
{
    const uint32_t pixels[4] = {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    for (unsigned flagged = 0; flagged < 3; flagged++) {
        const uint32_t words[] = {
            0x00010405, 0x10,
            0x00011c16, 0x08000420,
            flagged ? 0x00040002 : 2, flagged == 1 ? 0x00040002 : 2, 1,
            flagged == 2 ? 0x13000400 : 0x13001400, 0, 0,
            0x0001141b, 1, 0, 0, 0, 0,
            0x00010e80, 0x180000, 0, 0,
            0x00010406, 0,
        };

        push_user_words(qts, words, ARRAY_SIZE(words));
        odsy_test_pixel_write(qts, 14, 11, 0x12345678);
        texture_adapter_point(qts, 14, 11, -0.25f, 0.25f);
        texture_adapter_point(qts, 15, 11, 1.25f, 0.25f);
        if (flagged == 2) {
            /* Wrap field 0 on S with the flag on S: refused, no draw. */
            g_assert_cmphex(odsy_test_pixel(qts, 14, 11), ==, 0x12345678);
            continue;
        }
        g_assert_cmphex(odsy_test_pixel(qts, 14, 11), ==, 0xff000040);
        g_assert_cmphex(odsy_test_pixel(qts, 15, 11), ==, 0x00ff0080);
    }
    board_quit(qts);
}

/*
 * 0x8080d005 with four floats is glVertex4f: the point lands where its
 * homogeneous coordinates divide to, here (x + 0.5, y + 0.5) given as
 * twice that over W 2. Before, the word was unknown and nothing drew.
 */
static void test_command_fifo_gl_vertex_4f(void)
{
    QTestState *qts = board_start("");

    texture_adapter_window(qts);
    odsy_test_pixel_write(qts, 10, 20, 0x12345678);
    push_user_words(qts, (uint32_t[]) {
        ODSY_GL_COLOR_3F, ODSY_F_ONE, 0, ODSY_F_ONE,
        ODSY_CMD_GL_BEGIN, 0,
        0x8080d005, bitmap_rect_float(21.0f), bitmap_rect_float(41.0f), 0,
        bitmap_rect_float(2.0f), ODSY_CMD_GL_END,
    }, 12);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 20), ==, 0xff00ffff);
    board_quit(qts);
}

/*
 * A glBegin/glEnd pair with nothing but a texture coordinate between, as
 * Performer sends for triangles and strips, draws nothing and is not an
 * unmodeled primitive.
 */
static void test_command_fifo_gl_empty_primitive(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-empty-prim-XXXXXX.log", &log_path,
                          &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);
    texture_adapter_window(qts);
    odsy_test_pixel_write(qts, 20, 24, 0x12345678);
    for (uint32_t prim = 4; prim <= 5; prim++) {
        push_user_words(qts, (uint32_t[]) {
            ODSY_CMD_GL_BEGIN, prim,
            0xc380c821, bitmap_rect_float(20.0f), bitmap_rect_float(24.0f),
            ODSY_CMD_GL_END,
        }, 6);
    }
    g_assert_cmphex(odsy_test_pixel(qts, 20, 24), ==, 0x12345678);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "OpenGL primitive"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/* One bordered level: its w+2 square of stored texels at (15, 15). */
static void texture_bordered_level_upload(QTestState *qts, uint32_t base,
                                          unsigned w)
{
    unsigned n = w + 2;

    upload_begin_xy(qts, base, 4, 0x080024d0, 15, 15, n, n);
    for (unsigned y = 0; y < n; y++) {
        user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
        user_cmd_push(qts, 0x0f01104b | n << 20);
        user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
        user_cmd_push(qts, ODSY_IMAGE_BURST(n, ODSY_IMAGE_FMT_RGB));
        for (unsigned x = 0; x < n; x++) {
            user_cmd_push(qts, (0x20 * x) << 24 | (0x20 * y) << 16 |
                               (0x40 + w) << 8 | 0xff);
        }
    }
    upload_end(qts);
}

/*
 * distort's form: a mipmapped GL_CLAMP RGBA image whose every level keeps
 * a one-texel border, word 4's bit 28 clear and word 6's top byte 1, each
 * level 16 texels in from its allocation's corner with a row of its width
 * plus 32 texels in 16-texel units rounded up to even (four here, as the
 * captured 2x2 and 1D stored-border uploads use). A linear tap past the
 * edge reads the stored border texel, not the border colour or the edge.
 * With a level missing the image is incomplete and draws untextured.
 */
static void test_texture_bordered_mipmap(void)
{
    QTestState *qts = board_start("");

    texture_bordered_level_upload(qts, 0x180000, 4);
    texture_bordered_level_upload(qts, 0x181000, 2);
    texture_bordered_level_upload(qts, 0x182000, 1);
    texture_adapter_window(qts);
    for (unsigned complete = 0; complete < 2; complete++) {
        const uint32_t words[] = {
            0x00010405, 0x10,
            0x00011c16, 0x08000840, 4, 4, 1, 0x030028a2, 0, 0x01000000,
            0x0001141b, 1, 0, 0, 0, 0,
            0x00011013, 0, 0, 0x3100, 0x0f00,
            0x00010e80, 0x180000, 0x41100004, 0,
            0x00010e81, 0x181000, 0x41100004, 0,
            /* Without its last level, at a base nothing was uploaded to. */
            0x00010e82, complete ? 0x182000 : 0x183000, 0x41100004, 0,
            0x00010406, 0,
        };

        push_user_words(qts, words, ARRAY_SIZE(words));
        odsy_test_pixel_write(qts, 30, 12, 0x12345678);
        texture_adapter_point(qts, 30, 12, 0.125f, 0.125f);
        texture_adapter_point(qts, 31, 12, 0.0f, 0.125f);
        if (!complete) {
            g_assert_cmphex(odsy_test_pixel(qts, 30, 12), ==, 0xffff00ff);
            continue;
        }
        g_assert_cmphex(odsy_test_pixel(qts, 30, 12), ==, 0x202044ff);
        g_assert_cmphex(odsy_test_pixel(qts, 31, 12), ==, 0x102044ff);
    }
    board_quit(qts);
}

static void test_texture_adapter_nearest_clamp_border(void)
{
    static const struct {
        uint32_t rg, ba, expected;
    } colours[] = {
        { 0x00fff000, 0x00000fff, 0xff0000ff },
        { 0x00000fff, 0x00000fff, 0x00ff00ff },
        { 0, 0x00ffffff, 0x0000ffff },
        { 0x00000fff, 0x00fff800, 0x00ffff80 },
        { 0x00000fff, 0x00fff000, 0x00ffff00 },
    };
    const uint64_t page = ODSY_SHADOW_PAGE;
    QTestState *qts = board_start("");

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    qtest_writel(qts, page + ODSY_HW_PIPE_STATE, 0x4);
    qtest_writel(qts, page + ODSY_HW_INDEX_WORD, ODSY_GL_REPLAY_INDEX);
    qtest_writel(qts, page + ODSY_HW_COLOUR_WORD, ODSY_GL_COLOR_3I);
    texture_adapter_upload(qts, 0x180000, (const uint32_t[4]) {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    });
    texture_adapter_window(qts);
    context_switch_to(qts, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(colours); i++) {
        texture_adapter_bind_border(qts, 0x180000, 0x00fc0002,
                                     0x00fc0002, colours[i].rg,
                                     colours[i].ba);
        texture_adapter_point(qts, 10 + i, 10, -0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 10 + i, 10), ==,
                        colours[i].expected);
    }
    texture_adapter_bind_border(qts, 0x180000, 0x00fc0002, 0x00fc0002,
                                 0x00000fff, 0x00ffffff);
    texture_adapter_point(qts, 20, 10, 0.25f, 0.25f);
    texture_adapter_point(qts, 21, 10, 1.25f, 0.25f);
    texture_adapter_point(qts, 22, 10, 0.25f, -0.25f);
    texture_adapter_point(qts, 23, 10, 0.25f, 1.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 21, 10), ==, 0x00ffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 22, 10), ==, 0x00ffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 23, 10), ==, 0x00ffffff);

    /* Each context must upload its own border words after switching. */
    context_switch_to(qts, 2);
    texture_adapter_bind_border(qts, 0x180000, 0x00fc0002, 0x00fc0002,
                                 0x00fff000, 0x00000fff);
    texture_adapter_point(qts, 24, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 24, 10), ==, 0xff0000ff);
    context_switch_to(qts, 1);
    texture_adapter_bind_border(qts, 0x180000, 0x00fc0002, 0x00fc0002,
                                 0x00000fff, 0x00ffffff);
    texture_adapter_point(qts, 25, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 25, 10), ==, 0x00ffffff);

    /* Reject mismatched dimensions and reserved high bits. */
    odsy_test_pixel_write(qts, 26, 10, 0x12345678);
    texture_adapter_bind_border(qts, 0x180000, 0x00fc0004, 0x00fc0002,
                                 0, 0);
    texture_adapter_point(qts, 26, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 26, 10), ==, 0x12345678);
    texture_adapter_bind_border(qts, 0x180000, 0x00fd0002, 0x00fc0002,
                                 0, 0);
    texture_adapter_point(qts, 26, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 26, 10), ==, 0x12345678);
    texture_adapter_bind_border(qts, 0x180000, 0x00fc0002, 0x00fc0002,
                                 0x01000000, 0);
    texture_adapter_point(qts, 26, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 26, 10), ==, 0x12345678);
    texture_adapter_bind_border(qts, 0xdead00, 0x00fc0002, 0x00fc0002,
                                 0, 0);
    texture_adapter_point(qts, 26, 10, 0.25f, 0.25f);
    /* A valid sampler without uploaded storage draws untextured. */
    g_assert_cmphex(odsy_test_pixel(qts, 26, 10), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

static void test_texture_adapter_clamp_border_state_restore(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-clamp-border-state-XXXXXX", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_adapter_upload(from, 0x180000, (const uint32_t[4]) {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    });
    texture_adapter_window(from);
    texture_adapter_bind_border(from, 0x180000, 0x00fc0002,
                                 0x00fc0002, 0x00000fff,
                                 0x00fff800);
    texture_adapter_point(from, 10, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(from, 10, 10), ==, 0x00ffff80);

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    /* Raw sampler words and backing objects survive without reupload. */
    texture_adapter_point(to, 11, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0x00ffff80);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_binding_filters_matrix(void)
{
    const uint32_t pixels[4] = {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    };
    const uint32_t other[4] = {
        0x008000ff, 0x008000ff, 0x008000ff, 0x008000ff,
    };
    /* STRQ becomes (2S + 1/4, T, 0, 2), before final division by Q. */
    const uint32_t matrix[16] = {
        0x40000000, 0, 0, 0x3e800000,
        0, ODSY_F_ONE, 0, 0, 0, 0, ODSY_F_ONE, 0,
        0, 0, 0, 0x40000000,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_upload(qts, 0x181000, other);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x180000, 0x13000022, 1);
    texture_adapter_point(qts, 10, 10, 0.5f, 0.5f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x808080a0);
    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    texture_adapter_point(qts, 11, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x00ff0080);
    texture_adapter_bind(qts, 0x180000, 0x13001400, 1);
    texture_adapter_point(qts, 12, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, 0xff000040);
    texture_adapter_bind(qts, 0x181000, 0x13000000, 1);
    texture_adapter_point(qts, 13, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), ==, 0x008000ff);
    texture_adapter_upload(qts, 0x181000, pixels);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x181000, 0x13000000, 1);
    texture_adapter_point(qts, 14, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, 0xff000040);
    user_cmd_push(qts, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(qts, ODSY_GL_MATRIX_LOAD);
    push_user_words(qts, matrix, ARRAY_SIZE(matrix));
    texture_adapter_point(qts, 15, 10, 0.25f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(qts, 15, 10), ==, 0xff000040);
    g_assert_cmphex(odsy_test_pixel(qts, 16, 10), ==, ODSY_PLANE_BLUE);
    board_quit(qts);
}

/* Change only the discriminated generation fragment, not a whole program. */
static void texture_linear_program(QTestState *qts, unsigned axes)
{
    const uint32_t words[] = {
        0x28008000, 0x60008033,
        0x28008001, 0x60008000 | (axes == 1 ? 0x4d :
                                  axes == 2 ? 0x57 : 0x6b),
        0x28008002, axes == 3 ? 0x60008077 : 0x6000806c,
        0x28008003, axes == 3 ? 0x608080c5 : 0x60008077,
        0x28008004, 0x608080c5,
        0x33008000, 0x290080f3, 0x2a008100, 0x2c00810c,
    };
    const uint32_t disabled[] = {
        0x28008000, 0x6080809a, 0x28008001, 0x60008203,
        0x28008002, 0x608080c5,
        0x33008000, 0x290080f3, 0x2a008100, 0x2c00810c,
    };

    push_user_words(qts, axes ? words : disabled,
                     axes ? ARRAY_SIZE(words) : ARRAY_SIZE(disabled));
}

static void test_texture_adapter_linear_st(void)
{
    const uint32_t pixels[4] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint32_t planes[] = {
        0x31008100, 0x908091bb, 0x3c800000, 0, 0, 0xbf000000,
        0x31008104, 0x908091bb, 0, 0x3c800000, 0, 0xbf000000,
    };
    /* Literal setup and integer quad from the failing public native FIFO. */
    const uint32_t native_setup[] = {
        0x28008000, 0x600080f3, 0x28008001, 0x6000810c,
        0x28008002, 0x60008033, 0x28008003, 0x6000806b,
        0x28008004, 0x60008077, 0x28008005, 0x6080809a,
        0x28008006, 0x60008203, 0x28008007, 0x608080c5,
        0x33008001, 0x290080f3, 0x2a008100, 0x2c00810c,
    };
    const uint32_t native_quad[] = {
        0x00014400, 7,
        0x8080c800, 32, 32, 0x8080c800, 96, 32,
        0x8080c800, 96, 96, 0x8080c800, 32, 96, 0x00014001,
    };
    const uint32_t translated[16] = {
        ODSY_F_ONE, 0, 0, 0x3f000000,
        0, ODSY_F_ONE, 0, 0,
        0, 0, ODSY_F_ONE, 0,
        0, 0, 0, ODSY_F_ONE,
    };
    const uint32_t eye_s[] = {
        0x31008100, 0x908091bb, 0x3c800000, 0, 0, 0xbe800000,
    };
    const uint32_t changed[] = {
        0x31008100, 0x908091bb, 0x3c000000, 0, 0, 0,
        0x31008104, 0x908091bb, 0, 0x3c000000, 0, 0,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-linear-st-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_adapter_upload(from, 0x180000, pixels);
    texture_adapter_window(from);
    texture_adapter_bind(from, 0x180000, 0x13000000, 1);
    push_user_words(from, planes, ARRAY_SIZE(planes));
    push_user_words(from, native_setup, ARRAY_SIZE(native_setup));
    push_user_words(from, native_quad, ARRAY_SIZE(native_quad));
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(odsy_test_pixel(from, 48 + 32 * (i & 1),
                                        48 + 32 * (i >> 1)), ==, pixels[i]);
    }
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    texture_adapter_point(to, 81, 80, 0, 0);
    g_assert_cmphex(odsy_test_pixel(to, 81, 80), ==, pixels[3]);
    texture_linear_program(to, 1);
    push_user_words(to, native_quad, ARRAY_SIZE(native_quad));
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[1]);
    texture_adapter_point(to, 80, 80, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[1]);
    texture_linear_program(to, 2);
    push_user_words(to, native_quad, ARRAY_SIZE(native_quad));
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[2]);
    texture_adapter_point(to, 80, 80, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[2]);
    texture_linear_program(to, 0);
    push_user_words(to, native_quad, ARRAY_SIZE(native_quad));
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[0]);
    texture_adapter_point(to, 80, 80, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[0]);
    texture_linear_program(to, 3);
    user_cmd_push(to, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(to, ODSY_GL_MATRIX_LOAD);
    push_user_words(to, translated, ARRAY_SIZE(translated));
    push_user_words(to, native_quad, ARRAY_SIZE(native_quad));
    g_assert_cmphex(odsy_test_pixel(to, 48, 48), ==, pixels[1]);
    texture_adapter_point(to, 49, 48, 0, 0);
    g_assert_cmphex(odsy_test_pixel(to, 49, 48), ==, pixels[1]);
    texture_adapter_window(to);
    push_user_words(to, eye_s, ARRAY_SIZE(eye_s));
    push_user_words(to, native_quad, ARRAY_SIZE(native_quad));
    g_assert_cmphex(odsy_test_pixel(to, 48, 48), ==, pixels[1]);
    texture_adapter_point(to, 48, 48, 0, 0);
    g_assert_cmphex(odsy_test_pixel(to, 48, 48), ==, pixels[1]);
    push_user_words(to, changed, ARRAY_SIZE(changed));
    push_user_words(to, native_quad, ARRAY_SIZE(native_quad));
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[3]);
    texture_adapter_point(to, 80, 80, 0, 0);
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[3]);
    {
        const uint32_t negative_quad[] = {
            0x00014400, 7,
            0x8080c800, 0xffffffa0, 0xffffffa0,
            0x8080c800, 0xffffffe0, 0xffffffa0,
            0x8080c800, 0xffffffe0, 0xffffffe0,
            0x8080c800, 0xffffffa0, 0xffffffe0, 0x00014001,
        };
        const uint32_t negative_planes[] = {
            0x31008100, 0x908091bb, 0x3c800000, 0, 0, 0x3fc00000,
            0x31008104, 0x908091bb, 0, 0x3c800000, 0, 0x3fc00000,
        };
        const uint32_t window[16] = {
            ODSY_F_ONE, 0, 0, 0x43000000,
            0, ODSY_F_ONE, 0, 0x43000000,
            0, 0, bitmap_rect_float(0x1.ffff8p31f),
            bitmap_rect_float(0x1.ffff8p31f), 0, 0, 0, ODSY_F_ONE,
        };

        push_window_transform(to, window);
        push_user_words(to, negative_planes, ARRAY_SIZE(negative_planes));
        push_user_words(to, negative_quad, ARRAY_SIZE(negative_quad));
        for (unsigned i = 0; i < 4; i++) {
            g_assert_cmphex(odsy_test_pixel(to, 48 + 32 * (i & 1),
                                            48 + 32 * (i >> 1)), ==, pixels[i]);
        }
    }
    qtest_system_reset(to);
    texture_adapter_upload(to, 0x180000, pixels);
    texture_adapter_window(to);
    texture_adapter_bind(to, 0x180000, 0x13000000, 1);
    /* Reloading a program cannot reuse planes from before reset. */
    texture_linear_program(to, 3);
    texture_adapter_point(to, 80, 80, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(to, 80, 80), ==, pixels[0]);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_components_environment(void)
{
    const uint32_t pixels[4] = {
        0x80402080, 0x80402080, 0x80402080, 0x80402080,
    };
    const struct {
        uint32_t sampler, environment, expected;
    } cases[] = {
        { 0x13000000, 1, 0x80402080 },
        { 0x13000000, 2, 0x80400080 },
        { 0x13000000, 3, 0xbf9f10ff },
        { 0x12800000, 1, 0x804020ff },
        { 0x12800000, 3, 0x804020ff },
        { 0x11000000, 1, 0x808080ff },
        { 0x11000000, 2, 0x808000ff },
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        texture_adapter_bind(qts, 0x180000, cases[i].sampler,
                              cases[i].environment);
        texture_adapter_point(qts, 10 + i, 10, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 10 + i, 10), ==,
                        cases[i].expected);
    }
    /* Disabled texturing bypasses even unsupported sampler and environment. */
    texture_adapter_bind(qts, 0xdead00, 0xffffffff, 5);
    set_reg(qts, 0x00010405, 0);
    texture_adapter_point(qts, 20, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 10), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

/* RGB table lookup follows L8 filtering and preserves the incoming alpha. */
static void test_texture_adapter_colour_table(void)
{
    const uint32_t half_alpha[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload_l8_table_input(qts, 0x180000);
    texture_adapter_window(qts);
    texture_adapter_upload_colour_table(qts, false);
    texture_adapter_bind(qts, 0x180000, 0x11500001, 1);

    texture_adapter_point_rgba(qts, 10, 10, 0.25f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x00ff0080);
    texture_adapter_point_rgba(qts, 11, 10, 0.75f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x40bf1080);
    texture_adapter_point_rgba(qts, 12, 10, 0.75f, 0.75f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, 0xff00ff80);

    /*
     * The four equally weighted L8 taps average to 111.75, which selects
     * index 112. Its quadratic blue entry rounds to 49; looking up each tap
     * before filtering would instead average blue values 0, 16, 64, and 255
     * to 84.
     */
    texture_adapter_bind(qts, 0x180000, 0x11500003, 1);
    texture_adapter_point_rgba(qts, 13, 10, 0.5f, 0.5f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), ==, 0x708f3180);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), !=, 0x708f5480);

    texture_adapter_bind(qts, 0x180000, 0x11500000, 1);
    texture_adapter_point_rgba(qts, 14, 10, 0.75f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, 0x40404080);
    texture_adapter_upload_colour_table(qts, true);
    texture_adapter_bind(qts, 0x180000, 0x11500000, 1);
    texture_adapter_point_rgba(qts, 15, 10, 0.75f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 15, 10), ==, 0x40404080);
    texture_adapter_bind(qts, 0x180000, 0x11500001, 1);
    texture_adapter_point_rgba(qts, 16, 10, 0.75f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 16, 10), ==, 0xbf400080);
    texture_adapter_point_rgba(qts, 17, 10, 0.75f, 0.75f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 17, 10), ==, 0x00ff0080);
    board_quit(qts);
}

static void texture_adapter_l8_table_alpha(bool rgba)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t sampler = rgba ? 0x11600001 : 0x11500001;
    const uint32_t setup[] = {
        0x00010404, 0x02062200, 0x00010405, 0x2410,
        0x00011c16, 0x08000420, 2, 2, 1, sampler, 0, 0,
        0x0001141b, 2, 0, 0, 0, 0,
        0x00011013, 0, 0, 0x3100, 0,
        0x00010406, 0, 0x00010c21, 0x639, 0, 0,
        0xc580d015, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    const uint32_t quad[] = {
        0x00014400, 7,
        0xc380c821, 0, 0, 0x8080c800, 32, 32,
        0xc380c821, 0x3f800000, 0, 0x8080c800, 96, 32,
        0xc380c821, 0x3f800000, 0x3f800000, 0x8080c800, 96, 96,
        0xc380c821, 0, 0x3f800000, 0x8080c800, 32, 96,
        0x00014001,
    };
    QTestState *qts = board_start("");
    uint32_t scan[64 * 64];
    unsigned mismatches = 0;

    /* Original L8 checker; only the captured table endpoints are sampled. */
    upload_begin(qts, 0x180000, 2, 0x080024d0, 2, 2);
    upload_first_header(qts, 0x42);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, 0x00ffff00);
    upload_end(qts);
    texture_adapter_window(qts);
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    texture_adapter_bind(qts, 0x180000, sampler, 2);
    push_depth_clear(qts, 0, 0, 0, 0);
    for (unsigned plane = 0; plane < 2; plane++) {
        for (unsigned block = 0; block < 16; block++) {
            uint32_t words[16] = { 0 };

            /* Unused interior entries are original zero-valued fixtures. */
            if (block == 0) {
                words[0] = plane ? (rgba ? 0x00c0bfff : 0x00c0b000) :
                                   0x00404808;
            }
            if (block == 15) {
                words[15] = plane ? (rgba ? 0x00808000 : 0x00808fff) :
                                    0x00c0b404;
            }
            user_cmd_push(qts, 0x00013e00 | (plane << 4) | block);
            push_user_words(qts, words, ARRAY_SIZE(words));
        }
    }
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    odsy_test_pixels(qts, 32, 32, 64, 64, scan);
    for (unsigned y = 32; y < 96; y++) {
        for (unsigned x = 32; x < 96; x++) {
            bool high = (x < 64) == (y < 64);
            uint32_t expected = high ? 0x60204000 : 0x20406000;
            uint32_t actual = scan[(y - 32) * 64 + x - 32] & 0xffffff00;

            if (actual != expected) {
                if (mismatches < 8) {
                    g_test_message("pixel (%u,%u): expected %08x, got %08x",
                                   x, y, expected, actual);
                }
                mismatches++;
            }
        }
    }
    g_test_message("L8 %s table blend mismatches %u/4096",
                   rgba ? "RGBA" : "RGB", mismatches);
    /* The local LUMINANCE environment policy preserves primary alpha. */
    user_cmd_push(qts, 0x00010404);
    user_cmd_push(qts, 0x02042200);
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    g_assert_cmphex(odsy_test_pixel(qts, 40, 40), ==, 0xc0408080);
    g_assert_cmphex(odsy_test_pixel(qts, 80, 40), ==, 0x4080c080);
    push_user_words(qts, (uint32_t[]) {
        0xc580d015, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3e800000,
    }, 5);
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    g_assert_cmphex(odsy_test_pixel(qts, 40, 40), ==, 0xc0408040);
    g_assert_cmphex(odsy_test_pixel(qts, 80, 40), ==, 0x4080c040);
    board_quit(qts);
    g_assert_cmpuint(mismatches, ==, 0);
}

static void test_texture_adapter_l8_table_rgb_alpha(void)
{
    texture_adapter_l8_table_alpha(false);
}

static void test_texture_adapter_l8_table_rgba_alpha(void)
{
    texture_adapter_l8_table_alpha(true);
}

static void test_texture_adapter_colour_table_rgb_basis(void)
{
    const uint32_t white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload_l8_table_input(qts, 0x180000);
    texture_adapter_window(qts);
    for (unsigned plane = 0; plane < 2; plane++) {
        for (unsigned block = 0; block < 16; block++) {
            uint32_t words[16] = { 0 };

            if (block == 15) {
                words[15] = plane ? 0x00ffffff : 0x007f7000;
            }
            user_cmd_push(qts, 0x00013e00 | (plane << 4) | block);
            push_user_words(qts, words, ARRAY_SIZE(words));
        }
    }
    texture_adapter_bind(qts, 0x180000, 0x11500001, 1);
    texture_adapter_point_rgba(qts, 10, 10, 0.75f, 0.75f, white);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x7f00ffff);
    board_quit(qts);
}

static void test_texture_adapter_colour_table_rgba8_rgb_basis(void)
{
    texture_adapter_test_rgba8_colour_table(0x13500001, false, false);
}

static void test_texture_adapter_colour_table_rgba8_rgba_basis(void)
{
    texture_adapter_test_rgba8_colour_table(0x13600001, true, false);
}

static void test_texture_adapter_colour_table_rgba8_rgb_alpha(void)
{
    texture_adapter_test_rgba8_colour_table(0x13500001, false, true);
}

static void test_texture_adapter_colour_table_rgba8_rgba_alpha(void)
{
    texture_adapter_test_rgba8_colour_table(0x13600001, true, true);
}

static void test_texture_adapter_colour_table_incomplete(void)
{
    const uint32_t half_alpha[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload_l8_table_input(qts, 0x180000);
    texture_adapter_window(qts);
    {
        uint32_t words[16];

        for (unsigned j = 0; j < ARRAY_SIZE(words); j++) {
            words[j] = texture_table_pio_word(j, false, false);
        }
        user_cmd_push(qts, 0x00013e00);
        push_user_words(qts, words, ARRAY_SIZE(words));
    }
    texture_adapter_bind(qts, 0x180000, 0x11500001, 1);
    odsy_test_pixel_write(qts, 10, 10, 0x12345678);
    texture_adapter_point_rgba(qts, 10, 10, 0.75f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x12345678);
    board_quit(qts);
}

static void test_texture_adapter_colour_table_reset(void)
{
    const uint32_t half_alpha[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload_l8_table_input(qts, 0x180000);
    texture_adapter_window(qts);
    texture_adapter_upload_colour_table(qts, false);
    texture_adapter_bind(qts, 0x180000, 0x11500001, 1);
    texture_adapter_point_rgba(qts, 10, 10, 0.25f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x00ff0080);

    qtest_system_reset(qts);
    texture_adapter_upload_l8_table_input(qts, 0x180000);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x180000, 0x11500001, 1);
    odsy_test_pixel_write(qts, 11, 10, 0x12345678);
    texture_adapter_point_rgba(qts, 11, 10, 0.25f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x12345678);
    board_quit(qts);
}

static void test_texture_adapter_colour_table_migration(void)
{
    const uint32_t half_alpha[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-colour-table-state-XXXXXX", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_adapter_upload_l8_table_input(from, 0x180000);
    texture_adapter_window(from);
    texture_adapter_upload_colour_table(from, false);
    texture_adapter_bind(from, 0x180000, 0x11500001, 1);
    texture_adapter_point_rgba(from, 10, 10, 0.25f, 0.25f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(from, 10, 10), ==, 0x00ff0080);

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    texture_adapter_point_rgba(to, 11, 10, 0.75f, 0.75f, half_alpha);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0xff00ff80);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_scalar_components(void)
{
    const uint32_t half_white[4] = {
        ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3f000000,
    };
    const uint32_t scalar_env[5] = { 1, 0, 0, 0, 0 };
    QTestState *qts = board_start("");

    texture_adapter_upload_scalar(qts, 0x180000);
    texture_adapter_window(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 1, 0), ==,
                    0x404040ff);

    texture_adapter_bind_env(qts, 0x180000, 0x10800000, scalar_env);
    odsy_test_pixel_write(qts, 10, 10, ODSY_PLANE_YELLOW);
    odsy_test_pixel_write(qts, 11, 10, ODSY_PLANE_YELLOW);
    texture_adapter_point_rgba(qts, 10, 10, 0.25f, 0.25f, half_white);
    texture_adapter_point_rgba(qts, 11, 10, 0.75f, 0.25f, half_white);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xffffff00);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0xffffff40);

    texture_adapter_bind_env(qts, 0x180000, 0x10800000,
                             (const uint32_t[5]) { 2, 0, 0, 0, 0 });
    odsy_test_pixel_write(qts, 12, 10, ODSY_PLANE_YELLOW);
    texture_adapter_point_rgba(qts, 12, 10, 0.75f, 0.25f, half_white);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, 0xffffff20);

    texture_adapter_bind_env(qts, 0x180000, 0x12000000, scalar_env);
    odsy_test_pixel_write(qts, 13, 10, ODSY_PLANE_YELLOW);
    odsy_test_pixel_write(qts, 14, 10, ODSY_PLANE_YELLOW);
    texture_adapter_point(qts, 13, 10, 0.25f, 0.25f);
    texture_adapter_point(qts, 14, 10, 0.75f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 10), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, 0x40404040);

    texture_adapter_bind_env(qts, 0x180000, 0x12000000,
                             (const uint32_t[5]) { 2, 0, 0, 0, 0 });
    odsy_test_pixel_write(qts, 15, 10, ODSY_PLANE_YELLOW);
    texture_adapter_point_rgba(qts, 15, 10, 0.75f, 0.25f, half_white);
    g_assert_cmphex(odsy_test_pixel(qts, 15, 10), ==, 0x40404020);

    odsy_test_pixel_write(qts, 16, 10, 0x12345678);
    texture_adapter_bind_env(qts, 0x180000, 0x10800000,
                             (const uint32_t[5]) { 3, 0, 0, 0, 0 });
    texture_adapter_point(qts, 16, 10, 0.75f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 16, 10), ==, 0x12345678);

    odsy_test_pixel_write(qts, 17, 10, 0x12345678);
    texture_adapter_bind_env(qts, 0x180000, 0x12000000,
                             (const uint32_t[5]) { 3, 0, 0, 0, 0 });
    texture_adapter_point(qts, 17, 10, 0.75f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 17, 10), ==, 0x12345678);
    board_quit(qts);
}

static void test_texture_adapter_luminance_alpha(void)
{
    const uint32_t capture_rows[2] = { 0x00ffffff, 0xffff00ff };
    const uint32_t synthetic_rows[2] = { 0x12345678, 0x9abcdef0 };
    const uint32_t env[5] = { 1, 0, 0, 0, 0 };
    QTestState *qts = board_start("");

    texture_adapter_upload_luminance_alpha(qts, 0x180000, capture_rows);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 0), ==,
                    0x000000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 1, 0), ==,
                    0xffffffff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 0, 1), ==,
                    0xffffffff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x180000, 1, 1), ==,
                    0x000000ff);

    texture_adapter_window(qts);
    texture_adapter_bind_env(qts, 0x180000, 0x11800000, env);
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    texture_adapter_point(qts, 11, 10, 0.75f, 0.25f);
    texture_adapter_point(qts, 10, 11, 0.25f, 0.75f);
    texture_adapter_point(qts, 11, 11, 0.75f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x000000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0xffffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 11), ==, 0xffffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 11), ==, 0x000000ff);

    odsy_test_pixel_write(qts, 12, 10, 0x12345678);
    texture_adapter_bind(qts, 0x180000, 0x11800000, 3);
    texture_adapter_point(qts, 12, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, 0x12345678);

    texture_adapter_upload_luminance_alpha(qts, 0x181000, synthetic_rows);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 0, 0), ==,
                    0x12121234);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 1, 0), ==,
                    0x56565678);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 0, 1), ==,
                    0x9a9a9abc);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x181000, 1, 1), ==,
                    0xdededef0);

    /* One valid pair plus a padded pair must write only one texel. */
    upload_begin(qts, 0x182000, 2, 0x080024d0, 2, 2);
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    user_cmd_push(qts, ODSY_IMAGE_HEADER_OP(1, 0x48));
    user_cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    user_cmd_push(qts, 0x1234dead);
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x182000, 0, 0), ==,
                    0x12121234);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x182000, 1, 0), ==, 0);

    /* LA8 data paired with the RGB word format is unsupported. */
    upload_begin(qts, 0x183000, 2, 0x080024d0, 2, 2);
    upload_first_header(qts, 0x48);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, 0x12345678);
    upload_end(qts);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x183000, 0, 0), ==, 0);
    board_quit(qts);
}

static void volume_mipmap_upload(QTestState *qts, int missing)
{
    const uint32_t colours[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffff00ff,
    };
    uint32_t base = 0x16a800;
    unsigned object = 0;

    /* Public 8-cube probe: successive mip levels occupy successive slices. */
    for (unsigned level = 0, size = 8; level < 4; level++, size >>= 1) {
        uint32_t pixels[64];

        for (unsigned i = 0; i < size * size; i++) {
            pixels[i] = colours[level];
        }
        for (unsigned z = 0; z < size; z++, base += 0x80, object++) {
            if (object == missing) {
                continue;
            }
            texture_adapter_upload_3d_slice(qts, base, pixels, size, size);
            g_assert_cmphex(odsy_test_object_texel(qts, base, size - 1,
                                                   size - 1), ==,
                            colours[level]);
            g_assert_cmphex(odsy_test_object_texel(qts, base, size, 0), ==, 0);
        }
    }
}

static void volume_mipmap_bind(QTestState *qts)
{
    const uint32_t words[] = {
        0x00010404, 0x02042200,
        0x00010405, 0x2420,
        0x00011c16, 0x08000c63, 8, 8, 8, 0x12800040, 0, 0,
        0x0001141b, 2, 0, 0, 0, 0,
        0x00011013, 0, 0, 0x3100, 0x0f00,
        0x00010e80, 0x16a800, 0x41100002, 1,
        0x00010406, 0,
        ODSY_GL_COLOR_3F, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE,
    };

    texture_adapter_window(qts);
    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void volume_mipmap_quad(QTestState *qts, uint32_t span, uint32_t end)
{
    const uint32_t matrix[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, ODSY_F_ONE,
    };
    const uint32_t words[] = {
        0x33008000, 0x290080f3, 0x2a008100, 0x2c00810c,
        ODSY_CMD_GL_BEGIN, 7,
        0xc380cc23, 0, 0, 0x3f000000, 0x8080c800, 32, 32,
        0xc380cc23, span, 0, 0x3f000000, 0x8080c800, end, 32,
        0xc380cc23, span, span, 0x3f000000,
        0x8080c800, end, end,
        0xc380cc23, 0, span, 0x3f000000, 0x8080c800, 32, end,
        ODSY_CMD_GL_END,
    };

    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void volume_mipmap_native_quad(QTestState *qts)
{
    volume_mipmap_quad(qts, 0x41400000, 96);
}

static void volume_mipmap_control(unsigned control)
{
    const uint32_t trilinear[] = {
        0x00011c16, 0x08000c63, 8, 8, 8, 0x128000a0, 0, 0,
    };
    const uint32_t base_level[] = {
        0x00011c16, 0x08000421, 2, 2, 2, 0x12800040, 0, 0,
        0x00010e80, 0x16ae00, 0x41100002, 1,
    };
    QTestState *qts = board_start("");
    uint32_t expected = control == 0 ? 0x6a9500ff :
                        control == 1 ? 0xffff00ff : 0x0000ffff;
    unsigned end = control == 1 ? 48 : 96;
    uint32_t span = control == 0 ? 0x41400000 :
                    control == 1 ? 0x41800000 : ODSY_F_ONE;
    g_autofree uint32_t *scan = g_new(uint32_t, 64 * 64);

    volume_mipmap_upload(qts, -1);
    volume_mipmap_bind(qts);
    if (control == 0) {
        push_user_words(qts, trilinear, ARRAY_SIZE(trilinear));
    } else if (control == 2) {
        push_user_words(qts, base_level, ARRAY_SIZE(base_level));
    }
    /* The base-level result differs from a dropped draw's blue background. */
    if (control == 2) {
        odsy_test_pixel_write(qts, 64, 64, 0x12345678);
    }
    volume_mipmap_quad(qts, span, end);
    odsy_test_pixels(qts, 32, 128 - end, end - 32, end - 32, scan);
    for (unsigned y = 128 - end; y < 96; y++) {
        for (unsigned x = 32; x < end; x++) {
            uint32_t actual = scan[(y - (128 - end)) * (end - 32) + x - 32];

            if (control == 0) {
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    int delta = ((actual >> shift) & 255) -
                                ((expected >> shift) & 255);

                    g_assert_cmpint(abs(delta), <=, 1);
                }
            } else {
                g_assert_cmphex(actual, ==, expected);
            }
        }
    }
    g_assert_cmphex(odsy_test_pixel(qts, 31, 64), ==, ODSY_PLANE_BLUE);
    board_quit(qts);
}

static void test_texture_adapter_volume_mipmap_trilinear(void)
{
    volume_mipmap_control(0);
}

static void test_texture_adapter_volume_mipmap_depth_one(void)
{
    volume_mipmap_control(1);
}

static void test_texture_adapter_volume_mipmap_base_level(void)
{
    volume_mipmap_control(2);
}

static void test_texture_adapter_volume_mipmap_nearest(void)
{
    const uint32_t colours[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffff00ff,
    };
    QTestState *qts = board_start("");

    volume_mipmap_upload(qts, -1);
    volume_mipmap_bind(qts);
    volume_mipmap_native_quad(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 64), ==, colours[1]);
    g_assert_cmphex(odsy_test_pixel(qts, 31, 64), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 96, 64), ==, ODSY_PLANE_BLUE);
    texture_adapter_window(qts);
    for (unsigned level = 0; level < 4; level++) {
        const uint32_t lod[] = { 0x00011013, 0, 0, level << 8, level << 8 };

        push_user_words(qts, lod, ARRAY_SIZE(lod));
        texture_adapter_point_3d_rgba(qts, 10 + level, 10,
                                     0.99f, 0.99f, 0.99f,
                                     (const uint32_t[]) {
                                         ODSY_F_ONE, ODSY_F_ONE,
                                         ODSY_F_ONE, ODSY_F_ONE,
                                     });
        g_assert_cmphex(odsy_test_pixel(qts, 10 + level, 10), ==,
                        colours[level]);
    }
    board_quit(qts);
}

/*
 * Each draw samples a volume's slices where texture setup found them. Give
 * the eight level-0 slices distinct colours, upload them last slice first,
 * so slice order and object order disagree, and sample every slice. Then
 * grow slice 3's object so the store moves it, and sample again: the next
 * draw must read the moved slice and the others as before.
 */
static void volume_slices_sample(QTestState *qts, const uint32_t *colours)
{
    const uint32_t lod[] = { 0x00011013, 0, 0, 0, 0 };

    texture_adapter_window(qts);
    push_user_words(qts, lod, ARRAY_SIZE(lod));
    for (unsigned z = 0; z < 8; z++) {
        texture_adapter_point_3d_rgba(qts, 10 + z, 12, 0.5f, 0.5f,
                                     (z + 0.5f) / 8,
                                     (const uint32_t[]) {
                                         ODSY_F_ONE, ODSY_F_ONE,
                                         ODSY_F_ONE, ODSY_F_ONE,
                                     });
        g_assert_cmphex(odsy_test_pixel(qts, 10 + z, 12), ==, colours[z]);
    }
}

static void test_texture_adapter_volume_slices_follow_each_draw(void)
{
    QTestState *qts = board_start("");
    uint32_t colours[8];
    uint32_t pixels[16 * 8];

    volume_mipmap_upload(qts, -1);
    for (int z = 7; z >= 0; z--) {
        colours[z] = ((0x20u * (z + 1)) << 24) | (0x10u * z) << 8 | 0xff;
        for (unsigned i = 0; i < 64; i++) {
            pixels[i] = colours[z];
        }
        texture_adapter_upload_3d_slice(qts, 0x16a800 + 0x80 * z, pixels, 8,
                                        8);
    }
    volume_mipmap_bind(qts);
    volume_slices_sample(qts, colours);

    colours[3] = 0x123456ff;
    for (unsigned i = 0; i < ARRAY_SIZE(pixels); i++) {
        pixels[i] = colours[3];
    }
    texture_adapter_upload_3d_slice(qts, 0x16a800 + 0x80 * 3, pixels, 8, 16);
    volume_mipmap_bind(qts);
    volume_slices_sample(qts, colours);
    board_quit(qts);
}

static void test_texture_adapter_volume_mipmap_missing_slice(void)
{
    const unsigned missing[] = { 7, 11, 13, 14 };

    for (unsigned i = 0; i < ARRAY_SIZE(missing); i++) {
        QTestState *qts = board_start("");

        volume_mipmap_upload(qts, missing[i]);
        volume_mipmap_bind(qts);
        odsy_test_pixel_write(qts, 64, 64, 0x12345678);
        volume_mipmap_native_quad(qts);
        g_assert_cmphex(odsy_test_pixel(qts, 64, 64), ==, 0x12345678);
        board_quit(qts);
    }
}

static void test_texture_adapter_volume_mipmap_state_restore(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-volume-mipmap-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    volume_mipmap_upload(from, -1);
    volume_mipmap_bind(from);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    volume_mipmap_native_quad(to);
    g_assert_cmphex(odsy_test_pixel(to, 64, 64), ==, 0x00ff00ff);
    qtest_system_reset(to);
    volume_mipmap_bind(to);
    odsy_test_pixel_write(to, 64, 64, 0x12345678);
    volume_mipmap_native_quad(to);
    g_assert_cmphex(odsy_test_pixel(to, 64, 64), ==, 0x12345678);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_level_zero_3d(void)
{
    const uint32_t slice_colours[4] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    QTestState *qts = board_start("");

    for (unsigned volume = 0; volume < 2; volume++) {
        uint32_t height = volume ? 32 : 2;
        uint32_t level_word2 = volume ? 2 : 1;
        uint32_t spacing = volume ? 0x100 : 0x80;

        for (unsigned z = 0; z < 4; z++) {
            uint32_t pixels[64];

            for (unsigned i = 0; i < 2 * height; i++) {
                pixels[i] = slice_colours[z];
            }
            texture_adapter_upload_3d_slice(qts, 0x190000 + z * spacing,
                                             pixels, 2, height);
            g_assert_cmphex(odsy_test_object_texel(qts,
                                                   0x190000 + z * spacing,
                                                   0, 0), ==, slice_colours[z]);
        }
        texture_adapter_window(qts);
        texture_adapter_bind_3d(qts, 0x190000, height, 4, 2, level_word2, 1);
        for (unsigned z = 0; z < 4; z++) {
            texture_adapter_point_3d(qts, 10 + z, 10,
                                    0.25f, 0.25f, (z + 0.5f) / 4);
            g_assert_cmphex(odsy_test_pixel(qts, 10 + z, 10), ==,
                            slice_colours[z]);
        }
    }

    /* A one-slice 3D binding must still select the volume callback. */
    texture_adapter_bind_3d(qts, 0x190000, 32, 1, 2, 2, 1);
    odsy_test_pixel_write(qts, 18, 10, 0x12345678);
    texture_adapter_point_3d(qts, 18, 10, 0.25f, 0.25f, 1.75f);
    g_assert_cmphex(odsy_test_pixel(qts, 18, 10), ==, slice_colours[0]);

    /* MODULATE must multiply a nonwhite current vertex, including alpha. */
    {
        const uint32_t colour[4] = {
            bitmap_rect_float(0.5f), bitmap_rect_float(0.25f),
            bitmap_rect_float(0.75f), bitmap_rect_float(0.5f),
        };

        texture_adapter_bind_3d(qts, 0x190000, 32, 4, 2, 2, 2);
        texture_adapter_point_3d_rgba(qts, 14, 10, 0.25f, 0.25f, 0.875f,
                                      colour);
        g_assert_cmphex(odsy_test_pixel(qts, 14, 10), ==, 0x8040bf80);

        for (uint32_t environment = 3; environment <= 4; environment++) {
            uint32_t x = 12 + environment;

            texture_adapter_bind_3d(qts, 0x190000, 32, 4, 2, 2,
                                    environment);
            odsy_test_pixel_write(qts, x, 10, 0x12345678);
            texture_adapter_point_3d_rgba(qts, x, 10, 0.25f, 0.25f,
                                          0.875f, colour);
            g_assert_cmphex(odsy_test_pixel(qts, x, 10), ==, 0x12345678);
        }
    }

    /* Missing slices and invalid dimensions or stride drop the whole draw. */
    {
        const uint32_t pixel[4] = {
            0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff,
        };
        uint32_t sampler[7] = { 0x08000422, 2, 2, 4, 0x13000000, 0, 0 };
        uint32_t level[3] = { 0x191000, 0x41100002, 1 };
        const uint32_t env[5] = { 1, 0, 0, 0, 0 };

        texture_adapter_upload_3d_slice(qts, 0x191000, pixel, 2, 2);
        texture_adapter_upload_3d_slice(qts, 0x191080, pixel, 2, 2);
        texture_adapter_upload_3d_slice(qts, 0x191180, pixel, 2, 2);
        texture_adapter_window(qts);
        texture_adapter_bind_3d(qts, 0x191000, 2, 4, 2, 1, 1);
        odsy_test_pixel_write(qts, 20, 10, ODSY_PLANE_YELLOW);
        texture_adapter_point_3d(qts, 20, 10, 0.25f, 0.25f, 0.625f);
        g_assert_cmphex(odsy_test_pixel(qts, 20, 10), ==,
                        ODSY_PLANE_YELLOW);

        sampler[1] = 3;
        sampler[0] = 0x08000022;
        user_cmd_push(qts, 0x00011c16);
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        odsy_test_pixel_write(qts, 21, 10, ODSY_PLANE_YELLOW);
        texture_adapter_point_3d(qts, 21, 10, 0.25f, 0.25f, 0.125f);
        g_assert_cmphex(odsy_test_pixel(qts, 21, 10), ==,
                        ODSY_PLANE_YELLOW);

        sampler[1] = 2;
        sampler[0] = 0x08000422;
        sampler[4] = 0x13000002;
        user_cmd_push(qts, 0x00011c16);
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        odsy_test_pixel_write(qts, 22, 10, ODSY_PLANE_YELLOW);
        texture_adapter_point_3d(qts, 22, 10, 0.25f, 0.25f, 0.125f);
        g_assert_cmphex(odsy_test_pixel(qts, 22, 10), ==,
                        ODSY_PLANE_YELLOW);

        sampler[4] = 0x13000000;
        user_cmd_push(qts, 0x00011c16);
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        level[1] = 0x41100004;
        user_cmd_push(qts, 0x00010e80);
        push_user_words(qts, level, ARRAY_SIZE(level));
        user_cmd_push(qts, 0x0001141b);
        push_user_words(qts, env, ARRAY_SIZE(env));
        odsy_test_pixel_write(qts, 23, 10, ODSY_PLANE_YELLOW);
        texture_adapter_point_3d(qts, 23, 10, 0.25f, 0.25f, 0.125f);
        g_assert_cmphex(odsy_test_pixel(qts, 23, 10), ==,
                        ODSY_PLANE_YELLOW);
    }
    board_quit(qts);
}

static void test_texture_adapter_linear_r(void)
{
    const uint32_t colours[4] = {
        0xff0000ff, 0xffff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t setup[] = {
        0x28008000, 0x600080f3, 0x28008001, 0x6000810c,
        0x28008002, 0x60008033, 0x28008003, 0x60008061,
        0x28008004, 0x6000806c, 0x28008005, 0x60008077,
        0x28008006, 0x6080809a, 0x28008007, 0x60008203,
        0x28008008, 0x608080c5,
        0x31008108, 0x908091bb, 0x3d000000, 0, 0, 0xbf800000,
        0x31008000,
        0x33008001, 0x290080f3, 0x2a008100, 0x2c00810c,
    };
    const uint32_t quad[] = {
        0x00014400, 7,
        0xc380d025, 0, 0, 0, 0x3f800000, 0x8080c800, 32, 32,
        0xc380d025, 0x3f800000, 0, 0, 0x3f800000,
        0x8080c800, 96, 32,
        0xc380d025, 0x3f800000, 0x3f800000, 0, 0x3f800000,
        0x8080c800, 96, 96,
        0xc380d025, 0, 0x3f800000, 0, 0x3f800000,
        0x8080c800, 32, 96, 0x00014001,
    };
    const uint32_t shifted_r[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, ODSY_F_ONE, 0, 0,
        0, 0, ODSY_F_ONE, 0x3e800000,
        0, 0, 0, ODSY_F_ONE,
    };
    const uint64_t page = ODSY_SHADOW_PAGE;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *qts = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-linear-r-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    for (unsigned z = 0; z < ARRAY_SIZE(colours); z++) {
        const uint32_t pixels[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(qts, 0x16a800 + z * 0x80,
                                        pixels, 2, 2);
    }
    texture_adapter_window(qts);
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    texture_adapter_bind_3d(qts, 0x16a800, 2, 4, 2, 1, 1);
    /* Literal public R-plane, selectors and STRQ integer quad. */
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(odsy_test_pixel(qts, 36 + 8 * i, 64), ==,
                        colours[i % 4]);
    }
    /* Generated R must not overwrite the explicit current coordinate. */
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    qtest_memset(qts, page, 0xa5, 4096);
    qtest_writel(qts, page + 0x84, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(1) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, page + 0x140), ==, 0xc380cc23);
    g_assert_cmphex(qtest_readl(qts, page + 0x408), ==, 0);
    user_cmd_push(qts, 0xc380cc23);
    push_user_words(qts, (uint32_t[]) {
        0x3e800000, 0x3e800000, 0x3f200000,
    }, 3);
    texture_coordinate_replay(qts, page);
    texture_linear_program(qts, 0);
    texture_adapter_current_point(qts, 44, 63);
    g_assert_cmphex(odsy_test_pixel(qts, 44, 64), ==, colours[0]);

    /* Retaining the plane alone cannot enable generation. */
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    g_assert_cmphex(odsy_test_pixel(qts, 44, 64), ==, colours[0]);
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    user_cmd_push(qts, 0xc380d025);
    push_user_words(qts, (uint32_t[]) {
        0x3e800000, 0x3e800000, 0, 0x40000000,
    }, 4);
    texture_adapter_current_point(qts, 52, 63);
    g_assert_cmphex(odsy_test_pixel(qts, 52, 64), ==, colours[1]);

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(qts, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    texture_adapter_current_point(to, 60, 63);
    g_assert_cmphex(odsy_test_pixel(to, 60, 64), ==, colours[1]);

    /* The texture matrix follows generation, before the retained Q divide. */
    user_cmd_push(to, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(to, ODSY_GL_MATRIX_LOAD);
    push_user_words(to, shifted_r, ARRAY_SIZE(shifted_r));
    texture_adapter_current_point(to, 60, 63);
    g_assert_cmphex(odsy_test_pixel(to, 60, 64), ==, colours[2]);

    qtest_system_reset(to);
    for (unsigned z = 0; z < ARRAY_SIZE(colours); z++) {
        const uint32_t pixels[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(to, 0x16a800 + z * 0x80,
                                        pixels, 2, 2);
    }
    texture_adapter_window(to);
    texture_adapter_bind_3d(to, 0x16a800, 2, 4, 2, 1, 1);
    /* Reload the numeric list selectors, without reloading the plane. */
    push_user_words(to, setup, 18);
    push_user_words(to, &setup[25], 4);
    texture_adapter_point_3d(to, 52, 64, 0.25f, 0.25f, 0.125f);
    g_assert_cmphex(odsy_test_pixel(to, 52, 64), ==, colours[0]);
    board_quit(to);
    board_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

/* Captured combined selectors and planes, with an original synthetic volume. */
static void texture_adapter_linear_strq(bool t_enabled, bool bulk)
{
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t setup_a[] = {
        0x28008000, 0x600080f3, 0x28008001, 0x6000810c,
        0x28008002, 0x60008033, 0x28008003, 0x6000804d,
        0x28008004, 0x60008057, 0x28008005, 0x60008061,
        0x28008006, 0x60008066, 0x28008007, 0x6000806c,
        0x28008008, 0x60008077, 0x28008009, 0x6080809a,
        0x2800800a, 0x60008203, 0x2800800b, 0x608080c5,
        0x31008100, 0x908091bb, 0x3c800000, 0, 0, 0xbf000000,
        0x31008104, 0x908091bb, 0, 0x3c800000, 0, 0xbf000000,
        0x31008108, 0x908091bb, 0x3d000000, 0, 0, 0xbf800000,
        0x3100810c, 0x908091bb, 0x3c000000, 0, 0, 0x3f000000,
        0x31008000,
        0x33008001, 0x290080f3, 0x2a008100, 0x2c00810c,
    };
    const uint32_t setup_b[] = {
        0x28008000, 0x600080f3, 0x28008001, 0x6000810c,
        0x28008002, 0x60008033, 0x28008003, 0x6000804d,
        0x28008004, 0x60008061, 0x28008005, 0x60008066,
        0x28008006, 0x6000806c, 0x28008007, 0x60008077,
        0x28008008, 0x6080809a, 0x28008009, 0x60008203,
        0x2800800a, 0x608080c5,
        0x31008100, 0x908091bb, 0x3d000000, 0, 0, 0xbf800000,
        0x31008108, 0x908091bb, 0, 0x3d000000, 0, 0xbf800000,
        0x3100810c, 0x908091bb, 0, 0x3c000000, 0, 0x3f000000,
        0x31008000,
        0x3300800a, 0x33008001, 0x290080f3, 0x2a008100, 0x2c00810c,
    };
    const uint32_t planes[2][18] = {
        { 0x31008100, 0x9080bda2,
          0x3c800000, 0, 0, 0xbf000000,
          0, 0x3c800000, 0, 0xbf000000,
          0x3d000000, 0, 0, 0xbf800000,
          0x3c000000, 0, 0, 0x3f000000 },
        { 0x31008100, 0x9080bda2,
          0x3d000000, 0, 0, 0xbf800000,
          0, 0, 0, 0,
          0, 0x3d000000, 0, 0xbf800000,
          0, 0x3c000000, 0, 0x3f000000 },
    };
    const uint32_t quad[] = {
        0x00014400, 7,
        0x8080c800, 32, 32, 0x8080c800, 96, 32,
        0x8080c800, 96, 96, 0x8080c800, 32, 96, 0x00014001,
    };
    uint32_t pixels[4][4];
    unsigned mismatches = 0;
    uint32_t scan[64 * 64];
    QTestState *qts = board_start("");

    for (unsigned z = 0; z < 4; z++) {
        for (unsigned t = 0; t < 2; t++) {
            for (unsigned r = 0; r < 2; r++) {
                pixels[z][2 * t + r] =
                    ((16 + 48 * z + 24 * t + 12 * r) << 24) |
                    ((32 + 48 * r) << 16) | ((48 + 64 * t) << 8) | 0xff;
            }
        }
        texture_adapter_upload_3d_slice(qts, 0x16a800 + z * 0x80,
                                        pixels[z], 2, 2);
    }
    texture_adapter_window(qts);
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    texture_adapter_bind_3d(qts, 0x16a800, 2, 4, 2, 1, 1);
    push_user_words(qts, query_texture_4f[t_enabled ? 0 : 1], 5);
    if (bulk) {
        const uint32_t *setup = t_enabled ? setup_a : setup_b;
        unsigned size = t_enabled ? ARRAY_SIZE(setup_a) : ARRAY_SIZE(setup_b);
        unsigned first_plane = t_enabled ? 24 : 22;
        unsigned tail = size - (t_enabled ? 5 : 6);

        /* Start with the opposite planes, then reload through the kernel. */
        push_user_words(qts, (uint32_t[]) {
            0x31008104, 0x908091bb, 0, 0, 0, 0,
        }, 6);
        push_user_words(qts, t_enabled ? setup_b : setup_a,
                        t_enabled ? ARRAY_SIZE(setup_b) : ARRAY_SIZE(setup_a));
        texture_linear_program(qts, 0);
        for (unsigned i = 0; i < ARRAY_SIZE(planes[0]); i++) {
            cmd_push(qts, planes[t_enabled ? 0 : 1][i]);
        }
        /* Loading all four planes cannot enable coordinate generation. */
        push_user_words(qts, quad, ARRAY_SIZE(quad));
        g_assert_cmphex(odsy_test_pixel(qts, 36, 64), ==,
                        t_enabled ? pixels[2][1] : pixels[3][2]);
        push_user_words(qts, setup, first_plane);
        push_user_words(qts, setup + tail, size - tail);
    } else if (t_enabled) {
        push_user_words(qts, setup_a, ARRAY_SIZE(setup_a));
    } else {
        /* Retain the earlier A plane while B leaves T generation disabled. */
        push_user_words(qts, (uint32_t[]) {
            0x31008104, 0x908091bb, 0, 0x3c800000, 0, 0xbf000000,
        }, 6);
        push_user_words(qts, setup_b, ARRAY_SIZE(setup_b));
    }
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    odsy_test_pixels(qts, 32, 32, 64, 64, scan);
    for (unsigned y = 32; y < 96; y++) {
        for (unsigned x = 32; x < 96; x++) {
            /* Public linear planes evaluated at the pixel's object position. */
            double px = x + 0.5, py = 127.99609375 - (y + 0.5);
            double q = (t_enabled ? px : py) / 128 + 0.5;
            double str[3] = {
                (t_enabled ? px / 64 - 0.5 : px / 32 - 1) / q,
                (t_enabled ? py / 64 - 0.5 : 0.75) / q,
                ((t_enabled ? px : py) / 32 - 1) / q,
            };
            unsigned s_coord = (unsigned)floor(str[0] * 2) % 2;
            unsigned t_coord = (unsigned)floor(str[1] * 2) % 2;
            unsigned r_coord = (unsigned)floor(str[2] * 4) % 4;
            uint32_t expected = pixels[r_coord][2 * t_coord + s_coord];
            uint32_t actual = scan[(y - 32) * 64 + x - 32];

            if (actual != expected) {
                if (mismatches < 8) {
                    g_test_message("pixel (%u,%u): expected %08x, got %08x",
                                   x, y, expected, actual);
                }
                mismatches++;
            }
        }
    }
    g_test_message("combined %s mismatches %u/4096",
                   t_enabled ? "STRQ" : "SRQ", mismatches);
    /* Generated values must leave the explicit current coordinate intact. */
    texture_current_query(qts, query_texture_4f[t_enabled ? 0 : 1],
                          5, 2, 5, 1);
    board_quit(qts);
    g_assert_cmpuint(mismatches, ==, 0);
}

static void test_texture_adapter_linear_strq(void)
{
    texture_adapter_linear_strq(true, false);
}

static void test_texture_adapter_linear_srq(void)
{
    texture_adapter_linear_strq(false, false);
}

static void test_texture_adapter_linear_strq_bulk(void)
{
    texture_adapter_linear_strq(true, true);
}

static void test_texture_adapter_linear_srq_bulk(void)
{
    texture_adapter_linear_strq(false, true);
}

static void test_texture_adapter_linear_q(void)
{
    const uint32_t colours[4] = {
        0xff0000ff, 0xffff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint32_t matrix[16] = {
        0x3f800000, 0, 0, 0,
        0, 0xbf800000, 0, 0x42fffe00,
        0x80000000, 0x80000000, 0x4f7fffc0, 0xcf7fffc0,
        0, 0, 0, 0x3f800000,
    };
    const uint32_t setup[] = {
        0x28008000, 0x600080f3, 0x28008001, 0x6000810c,
        0x28008002, 0x60008033, 0x28008003, 0x60008066,
        0x28008004, 0x6000806c, 0x28008005, 0x60008077,
        0x28008006, 0x6080809a, 0x28008007, 0x60008203,
        0x28008008, 0x608080c5,
        0x3100810c, 0x908091bb, 0x3c800000, 0, 0, 0,
        0x31008000,
        0x33008001, 0x290080f3, 0x2a008100, 0x2c00810c,
    };
    const uint32_t quad[] = {
        0x00014400, 7,
        0xc380d025, 0x3f400000, 0x3e800000, 0x3f200000, 0x3f800000,
        0x8080c800, 32, 32,
        0xc380d025, 0x3f400000, 0x3e800000, 0x3f200000, 0x3f800000,
        0x8080c800, 96, 32,
        0xc380d025, 0x3f400000, 0x3e800000, 0x3f200000, 0x3f800000,
        0x8080c800, 96, 96,
        0xc380d025, 0x3f400000, 0x3e800000, 0x3f200000, 0x3f800000,
        0x8080c800, 32, 96, 0x00014001,
    };
    const uint32_t shifted_q[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, ODSY_F_ONE, 0, 0,
        0, 0, ODSY_F_ONE, 0,
        0, 0, 0, 0x3fc00000,
    };
    const uint64_t page = ODSY_SHADOW_PAGE;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *qts = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-linear-q-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);

    for (unsigned z = 0; z < ARRAY_SIZE(colours); z++) {
        const uint32_t pixels[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(qts, 0x16a800 + z * 0x80,
                                        pixels, 2, 2);
    }
    texture_adapter_window(qts);
    push_window_transform(qts, matrix);
    push_block_16(qts, 0xbf800000);
    texture_adapter_bind_3d(qts, 0x16a800, 2, 4, 2, 1, 1);
    /* Literal public Q-plane, numeric list and complete STRQ integer quad. */
    push_user_words(qts, setup, ARRAY_SIZE(setup));
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    g_assert_cmphex(odsy_test_pixel(qts, 36, 64), ==, colours[0]);
    g_assert_cmphex(odsy_test_pixel(qts, 48, 64), ==, colours[3]);
    g_assert_cmphex(odsy_test_pixel(qts, 64, 64), ==, colours[2]);
    g_assert_cmphex(odsy_test_pixel(qts, 88, 64), ==, colours[1]);
    /* Retaining the plane alone cannot enable Q generation. */
    texture_linear_program(qts, 0);
    push_user_words(qts, quad, ARRAY_SIZE(quad));
    g_assert_cmphex(odsy_test_pixel(qts, 36, 64), ==, colours[2]);
    push_user_words(qts, setup, ARRAY_SIZE(setup));

    /* Generated Q must leave the explicit current STRQ unchanged. */
    user_cmd_push(qts, 0xc380d025);
    push_user_words(qts, (uint32_t[]) {
        0x3f400000, 0x3e800000, 0x3f200000, 0x40000000,
    }, 4);
    texture_adapter_current_point(qts, 36, 63);
    g_assert_cmphex(odsy_test_pixel(qts, 36, 64), ==, colours[0]);
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    qtest_memset(qts, page, 0xa5, 4096);
    qtest_writel(qts, page + 0x84, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(1) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, page + 0x140), ==, 0xc380d025);
    g_assert_cmphex(qtest_readl(qts, page + 0x40c), ==, 0x40000000);
    texture_adapter_point_3d(qts, 40, 63, 0.25f, 0.25f, 0.125f);
    texture_coordinate_replay(qts, page);
    texture_linear_program(qts, 0);
    texture_adapter_current_point(qts, 36, 63);
    g_assert_cmphex(odsy_test_pixel(qts, 36, 64), ==, colours[1]);
    push_user_words(qts, setup, ARRAY_SIZE(setup));

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(qts, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    texture_adapter_current_point(to, 36, 63);
    g_assert_cmphex(odsy_test_pixel(to, 36, 64), ==, colours[0]);

    /* The texture matrix scales generated Q before projective division. */
    user_cmd_push(to, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(to, ODSY_GL_MATRIX_LOAD);
    push_user_words(to, shifted_q, ARRAY_SIZE(shifted_q));
    texture_adapter_current_point(to, 36, 63);
    g_assert_cmphex(odsy_test_pixel(to, 36, 64), ==, colours[2]);

    qtest_system_reset(to);
    for (unsigned z = 0; z < ARRAY_SIZE(colours); z++) {
        const uint32_t pixels[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(to, 0x16a800 + z * 0x80,
                                        pixels, 2, 2);
    }
    texture_adapter_window(to);
    texture_adapter_bind_3d(to, 0x16a800, 2, 4, 2, 1, 1);
    /* Reload the stage selectors without reloading the Q plane. */
    push_user_words(to, setup, 18);
    push_user_words(to, &setup[25], 4);
    texture_adapter_point_3d(to, 36, 64, 0.75f, 0.25f, 0.625f);
    g_assert_cmphex(odsy_test_pixel(to, 36, 64), ==, colours[2]);
    board_quit(to);
    board_quit(qts);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_projective_3d(void)
{
    const uint32_t colours[4] = {
        0xff0000ff, 0xffff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint32_t r[3] = { 0x3e800000, 0x3fa00000, 0x3fe00000 };
    QTestState *qts = board_start("");

    for (unsigned z = 0; z < ARRAY_SIZE(colours); z++) {
        const uint32_t pixels[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(qts, 0x16a800 + z * 0x80,
                                        pixels, 2, 2);
    }
    texture_adapter_window(qts);
    texture_adapter_bind_3d(qts, 0x16a800, 2, 4, 2, 1, 1);

    /* Public glTexCoord4f draw: doubled STR and Q=2 precede integer XY. */
    user_cmd_push(qts, 0x00014400);
    user_cmd_push(qts, 7);
    for (unsigned quad = 0; quad < 3; quad++) {
        for (unsigned corner = 0; corner < 4; corner++) {
            bool right = corner == 1 || corner == 2;
            bool top = corner >= 2;
            uint32_t vertex[] = {
                0xc380d025, right ? 0x40000000 : 0,
                top ? 0x40000000 : 0, r[quad], 0x40000000,
                0x8080c800, 4 + 40 * (quad + right), top ? 96 : 32,
            };

            push_user_words(qts, vertex, ARRAY_SIZE(vertex));
        }
    }
    user_cmd_push(qts, 0x00014001);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 40), ==, colours[0]);
    g_assert_cmphex(odsy_test_pixel(qts, 52, 40), ==, colours[2]);
    g_assert_cmphex(odsy_test_pixel(qts, 92, 40), ==, colours[3]);

    /* A three-component coordinate restores the implicit Q=1. */
    texture_adapter_point_3d(qts, 12, 12, 0.25f, 0.25f, 0.875f);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, colours[3]);
    board_quit(qts);
}

static void test_texture_adapter_3d_state_restore(void)
{
    const uint32_t colours[4] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    const uint64_t spacing = 0x80;
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-texture-3d-state-XXXXXX", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    for (unsigned z = 0; z < ARRAY_SIZE(colours); z++) {
        const uint32_t pixels[4] = {
            colours[z], colours[z], colours[z], colours[z],
        };

        texture_adapter_upload_3d_slice(from, 0x1a0000 + z * spacing,
                                         pixels, 2, 2);
    }
    texture_adapter_window(from);
    texture_adapter_bind_3d(from, 0x1a0000, 2, 4, 2, 1, 1);
    user_cmd_push(from, 0xc380d025);
    push_user_words(from, (uint32_t[]) {
        0x3f000000, 0x3f000000, 0x3fa00000, 0x40000000,
    }, 4);
    texture_adapter_current_point(from, 10, 10);
    g_assert_cmphex(odsy_test_pixel(from, 10, 10), ==, colours[2]);

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    /* The family restores R/Q with the object and sampler state. */
    texture_adapter_current_point(to, 11, 10);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, colours[2]);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_context_save_texture_coordinate_3d(void)
{
    const uint64_t page = ODSY_SHADOW_PAGE;
    const uint32_t coordinate[3] = { 0x3f000000, 0x3f400000, 0x3f200000 };
    QTestState *qts = board_start("");

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    qtest_memset(qts, page, 0xa5, 4096);
    qtest_writel(qts, page + 0x84, 0);
    qtest_writel(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
    user_cmd_push(qts, 0x00010405);
    user_cmd_push(qts, 0x20);
    user_cmd_push(qts, 0xc380cc23);
    push_user_words(qts, coordinate, ARRAY_SIZE(coordinate));
    texture_adapter_current_point(qts, 10, 10);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, page);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(1) << 16) | 1);

    g_assert_cmphex(qtest_readl(qts, page + ODSY_CTX_SWITCH_DONE_OFFSET),
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, page + 0x140), ==, 0xc380cc23);
    g_assert_cmphex(qtest_readl(qts, page + 0x400), ==, coordinate[0]);
    g_assert_cmphex(qtest_readl(qts, page + 0x404), ==, coordinate[1]);
    g_assert_cmphex(qtest_readl(qts, page + 0x408), ==, coordinate[2]);

    user_cmd_push(qts, 0xc380d025);
    push_user_words(qts, (uint32_t[]) {
        0x3f000000, 0x3f000000, 0x3fa00000, 0x40000000,
    }, 4);
    qtest_writel(qts, page + 0x84, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(1) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, page + 0x140), ==, 0xc380d025);
    g_assert_cmphex(qtest_readl(qts, page + 0x408), ==, 0x3fa00000);
    g_assert_cmphex(qtest_readl(qts, page + 0x40c), ==, 0x40000000);
    user_cmd_push(qts, 0xc380cc23);
    push_user_words(qts, coordinate, ARRAY_SIZE(coordinate));
    texture_coordinate_replay(qts, page);
    qtest_writel(qts, page + 0x84, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 (UINT64_C(7) << 40) | (UINT64_C(1) << 16) | 1);
    g_assert_cmphex(qtest_readl(qts, page + 0x140), ==, 0xc380d025);
    g_assert_cmphex(qtest_readl(qts, page + 0x40c), ==, 0x40000000);
    board_quit(qts);
}

static void test_texture_adapter_blend_environment(void)
{
    const uint32_t white[4] = {
        0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    };
    const uint32_t mixed_env[5] = {
        4, 0x00111222, 0x00333444, 0, 0,
    };
    const uint32_t changed_env[5] = {
        4, 0x00000fff, 0x00800000, 0, 0,
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x181000, white);
    texture_adapter_window(qts);
    texture_adapter_bind_env(qts, 0x181000, 0x11000000, mixed_env);
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x112233ff);

    texture_adapter_bind_env(qts, 0x181000, 0x11000000, changed_env);
    texture_adapter_point(qts, 11, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x00ff80ff);

    for (unsigned field = 0; field < 4; field++) {
        uint32_t invalid_env[5] = { 4, 0, 0, 0, 0 };
        uint32_t x = 12 + field;

        switch (field) {
        case 0:
            invalid_env[1] = 0x01000000;
            break;
        case 1:
            invalid_env[2] = 0x01000000;
            break;
        case 2:
            invalid_env[3] = 1;
            break;
        case 3:
            invalid_env[4] = 1;
            break;
        }
        odsy_test_pixel_write(qts, x, 10, 0xa1b2c3d4);
        texture_adapter_bind_env(qts, 0x181000, 0x11000000, invalid_env);
        texture_adapter_point(qts, x, 10, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, x, 10), ==, 0xa1b2c3d4);
    }
    board_quit(qts);
}

static void test_texture_adapter_blend_state_restore(void)
{
    const uint32_t white[4] = {
        0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    };
    const uint32_t env[5] = { 4, 0x00111222, 0x00333444, 0, 0 };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-texture-blend-state-XXXXXX",
                             &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_adapter_upload(from, 0x181000, white);
    texture_adapter_window(from);
    texture_adapter_bind_env(from, 0x181000, 0x11000000, env);
    texture_adapter_point(from, 10, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(from, 10, 10), ==, 0x112233ff);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    /* No sampler or environment reupload: family state rebuilds the draw. */
    texture_adapter_point(to, 11, 10, 0.75f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0x112233ff);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_storage_dimensions(void)
{
    const uint32_t pixels[8] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
        0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff,
    };
    uint32_t sampler[] = { 0x08000c00, 8, 1, 1, 0x13000000, 0, 0 };
    QTestState *qts = board_start("");

    upload_begin(qts, 0x180000, 2, 0x080024d0, 8, 1);
    upload_first_header(qts, 0x4b);
    user_cmd_push(qts, ODSY_IMAGE_BURST(8, ODSY_IMAGE_FMT_RGB));
    push_user_words(qts, pixels, ARRAY_SIZE(pixels));
    upload_end(qts);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    user_cmd_push(qts, 0x00011c16);
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    texture_adapter_point(qts, 10, 10, 0.4375f, 0.5f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xffffffff);
    /* An eight-wide object has only one row: two rows are incomplete. */
    sampler[0] = 0x08000c20;
    sampler[2] = 2;
    user_cmd_push(qts, 0x00011c16);
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    texture_adapter_point(qts, 11, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, ODSY_PLANE_YELLOW);
    /* A supported image wider than its object stride also falls back. */
    sampler[0] = 0x08001800;
    sampler[1] = 64;
    sampler[2] = 1;
    user_cmd_push(qts, 0x00011c16);
    push_user_words(qts, sampler, ARRAY_SIZE(sampler));
    texture_adapter_point(qts, 12, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 10), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

/* Family image state travels; descriptors are rebuilt after load. */
static void test_texture_adapter_state_restore(void)
{
    const uint32_t pixels[4] = {
        0xff000040, 0x00ff0080, 0x0000ffc0, 0xffffffff,
    };
    const uint32_t matrix[16] = {
        0x40000000, 0, 0, 0x3e800000,
        0, ODSY_F_ONE, 0, 0, 0, 0, ODSY_F_ONE, 0,
        0, 0, 0, 0x40000000,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-texture-state-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_adapter_upload(from, 0x180000, pixels);
    texture_adapter_window(from);
    texture_adapter_bind(from, 0x180000, 0x13000000, 1);
    user_cmd_push(from, ODSY_GL_MATRIX_SELECT_TEXTURE);
    user_cmd_push(from, ODSY_GL_MATRIX_LOAD);
    push_user_words(from, matrix, ARRAY_SIZE(matrix));
    texture_adapter_point(from, 10, 10, 0.25f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(from, 10, 10), ==, 0xff000040);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    /* No ST or matrix reupload: the family must restore both. */
    user_cmd_push(to, ODSY_CMD_GL_BEGIN);
    user_cmd_push(to, 0);
    user_cmd_push(to, ODSY_GL_VERTEX_2F);
    user_cmd_push(to, bitmap_rect_float(11.5f));
    user_cmd_push(to, bitmap_rect_float(10.5f));
    user_cmd_push(to, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0xff000040);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_adapter_unsupported_missing(void)
{
    const uint32_t excluded[] = {
        0x13002000, 0x13000c00,
        0x13040000, 0x03000000, 0x14000000,
    };
    QTestState *qts = board_start("");

    texture_adapter_window(qts);
    /* An existing object with no uploaded rows is incomplete too. */
    upload_begin(qts, 0x180000, 2, 0x080024d0, 2, 2);
    upload_end(qts);
    texture_adapter_window(qts);
    for (unsigned backing = 0; backing < 2; backing++) {
        for (unsigned i = 0; i < ARRAY_SIZE(excluded); i++) {
            texture_adapter_bind(qts, backing ? 0x180000 : 0xdead00,
                                  excluded[i], 1);
            texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
            g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
        }
    }
    for (unsigned backing = 0; backing < 2; backing++) {
        uint32_t base = backing ? 0x180000 : 0xdead00;

        for (unsigned env = 5; env <= 6; env++) {
            texture_adapter_bind(qts, base, 0x13000000, env);
            texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
            g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
        }
        texture_adapter_bind(qts, base, 0x11000000, 3); /* luminance DECAL */
        texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
    }
    for (unsigned field = 0; field < 7; field++) {
        uint32_t sampler[] = { 0x08000420, 2, 2, 1, 0x13000000, 0, 0 };

        texture_adapter_bind(qts, 0xdead00, 0x13000000, 1);
        sampler[field] ^= field == 1 ? 0x80000000 : 1;
        user_cmd_push(qts, 0x00011c16);
        push_user_words(qts, sampler, ARRAY_SIZE(sampler));
        texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
    }
    /* Colour-index texturing is an unsupported family draw policy. */
    user_cmd_push(qts, 0x22008001);
    texture_adapter_bind(qts, 0xdead00, 0x13000000, 1);
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_BLUE);
    user_cmd_push(qts, 0x22008000);
    /* Only supported state may fall back to the untextured yellow colour. */
    texture_adapter_bind(qts, 0xdead00, 0x13000000, 1);
    texture_adapter_point(qts, 10, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, ODSY_PLANE_YELLOW);
    board_quit(qts);
}

/* Original RGBA rectangle operands in the established ten-float format. */
static void rgba_rect(QTestState *qts, float r, float g, float b, float a,
                      float x0, float y0, float x1, float y1)
{
    const uint32_t words[] = {
        ODSY_CMD_GL_BEGIN, ODSY_CMD_GL_QUADS, ODSY_GL_RECT,
        bitmap_rect_float(r), bitmap_rect_float(g), bitmap_rect_float(b),
        bitmap_rect_float(a), bitmap_rect_float(x0), bitmap_rect_float(y0), 0,
        bitmap_rect_float(x1), bitmap_rect_float(y1), 0, ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void push_native_float_quad(QTestState *qts, uint32_t r, uint32_t g,
                                   uint32_t b, float x0, float y0,
                                   float x1, float y1)
{
    const uint32_t words[] = {
        ODSY_GL_COLOR_3F, r, g, b,
        ODSY_CMD_GL_BEGIN, ODSY_GL_QUADS,
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x0), bitmap_rect_float(y0),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x1), bitmap_rect_float(y0),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x1), bitmap_rect_float(y1),
        ODSY_GL_VERTEX_2F, bitmap_rect_float(x0), bitmap_rect_float(y1),
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
}

static void test_rgba_clear_masks(void)
{
    const uint32_t masks[] = { 1, 2, 4, 8, 7, 0 };
    const uint32_t expected[] = {
        0xff141e28, 0x0aff1e28, 0x0a14ff28,
        0x0a141e80, 0xffffff28, 0x0a141e28,
    };
    const float alpha[] = { 0, 1, 0.5f };
    const uint32_t blue[] = { 0x0000ff00, 0x0000ffff, 0x0000ff80 };
    QTestState *qts = board_start("");

    for (unsigned i = 0; i < ARRAY_SIZE(alpha); i++) {
        rgba_rect(qts, 0, 0, 1, alpha[i], 10, 10, 14, 14);
        g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, blue[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
        odsy_test_pixel_write(qts, 12, 12, 0x0a141e28);
        set_reg(qts, ODSY_REG_WRITE_MASK, masks[i] << 20);
        rgba_rect(qts, 1, 1, 1, 0.5f, 10, 10, 14, 14);
        g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, expected[i]);
    }
    board_quit(qts);
}

static void test_rgba_material_masks(void)
{
    const uint32_t material[] = {
        0xc780d030, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3e800000,
    };
    const uint32_t triangle[] = {
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_VERTEX_2F, 0x41000000, 0x41000000,
        ODSY_GL_VERTEX_2F, 0x41f00000, 0x41000000,
        ODSY_GL_VERTEX_2F, 0x41000000, 0x41f00000,
        ODSY_CMD_GL_END,
    };
    QTestState *qts = board_start("");
    uint32_t colour;

    bitmap_rect_depth_window(qts);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00f00000);
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    push_user_words(qts, material, ARRAY_SIZE(material));
    push_user_words(qts, triangle, ARRAY_SIZE(triangle));
    colour = odsy_test_pixel(qts, 12, 12);
    g_assert_cmphex(colour & 0xff, ==, 0x40);

    odsy_test_pixel_write(qts, 12, 12, 0x0a141e28);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00800000);
    push_user_words(qts, triangle, ARRAY_SIZE(triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0x0a141e40);
    odsy_test_pixel_write(qts, 12, 12, 0x0a141e28);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00700000);
    push_user_words(qts, triangle, ARRAY_SIZE(triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==,
                    (colour & ~0xffu) | 0x28);
    board_quit(qts);
}

static void test_rgba_pattern_origin_ownership(void)
{
    const uint32_t pattern[32] = { [1] = 1u << 30 };
    const uint32_t clip[][4] = { { 311, 211, 312, 212 } };
    QTestState *qts = board_start("");

    window_origin(qts, 300, 200);
    window_clip(qts, clip, ARRAY_SIZE(clip));
    for (unsigned y = 210; y < 214; y++) {
        for (unsigned x = 310; x < 314; x++) {
            odsy_test_pixel_write(qts, x, y, 0x0a141e28);
        }
    }
    bitmap_rect_pattern(qts, pattern, 0x10);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00800000);
    rgba_rect(qts, 1, 1, 1, 0.5f, 10, 10, 14, 14);
    for (unsigned y = 210; y < 214; y++) {
        for (unsigned x = 310; x < 314; x++) {
            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==,
                            x == 311 && y == 211 ? 0x0a141e80 : 0x0a141e28);
        }
    }
    qtest_quit(qts);
}

static void test_rgba_pattern_float_rectangle(void)
{
    const uint32_t pattern[32] = {
        [21] = (1u << 10) | (1u << 20),
        [20] = 1u << 13,
        [18] = 1u << 12,
    };
    const uint32_t matrix[16] = {
        ODSY_F_ONE, 0, 0, 0,
        0, ODSY_F_ONE, 0, 0,
        0, 0, 0x4f7fffc0, 0x4f7fffc0,
        0, 0, 0, ODSY_F_ONE,
    };
    QTestState *qts = board_start("");

    push_window_transform(qts, matrix);
    push_block_16(qts, ODSY_F_ONE);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    set_reg(qts, ODSY_CMD_PATTERN_MODE, 0);
    push_native_float_quad(qts, 0, 0, ODSY_F_ONE, 10, 10, 24, 14);
    pattern_load(qts, pattern);
    set_reg(qts, ODSY_CMD_PATTERN_SHAPE, 0);
    set_reg(qts, ODSY_CMD_PATTERN_MODE, 0x02042210);
    user_cmd_push(qts, 0x00010405);
    user_cmd_push(qts, 0x2000);
    push_native_float_quad(qts, ODSY_F_ONE, 0, 0, 10, 10, 14, 14);

    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0x0000ffff);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 11), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 13), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 13), ==, 0x0000ffff);

    push_native_float_quad(qts, 0, ODSY_F_ONE, 0, 20, 10, 24, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 10), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 21, 10), ==, 0x0000ffff);
    push_native_float_quad(qts, ODSY_F_ONE, ODSY_F_ONE, 0, 12, 10, 22, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 11), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 10), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 15, 11), ==, 0x0000ffff);
    set_reg(qts, ODSY_CMD_PATTERN_MODE, 0x02042200);
    push_native_float_quad(qts, 0, ODSY_F_ONE, ODSY_F_ONE, 12, 10, 22, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 15, 11), ==, 0x00ffffff);
    g_assert_cmphex(odsy_test_pixel(qts, 13, 11), ==, 0x00ffffff);

    qtest_system_reset(qts);
    bitmap_rect_depth_window(qts);
    push_native_float_quad(qts, ODSY_F_ONE, 0, 0, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 10), ==, 0xff0000ff);
    board_quit(qts);
}

static void test_rgba_pattern_float_rectangle_migration(void)
{
    const uint32_t pattern[32] = {
        [21] = 1u << 10,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-pattern-stipple-XXXXXX", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    bitmap_rect_depth_window(from);
    push_native_float_quad(from, 0, 0, ODSY_F_ONE, 10, 10, 14, 14);
    pattern_load(from, pattern);
    set_reg(from, ODSY_CMD_PATTERN_SHAPE, 0);
    set_reg(from, ODSY_CMD_PATTERN_MODE, 0x02042210);
    user_cmd_push(from, 0x00010405);
    user_cmd_push(from, 0x2000);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");
    push_native_float_quad(to, ODSY_F_ONE, 0, 0, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(to, 10, 10), ==, 0xff0000ff);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0x0000ffff);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void four_float_triangle(QTestState *qts, const uint32_t *colour,
                                unsigned n)
{
    static const uint32_t triangle[] = {
        ODSY_CMD_GL_BEGIN, ODSY_GL_TRIANGLES,
        ODSY_GL_VERTEX_2F, 0x41000000, 0x41000000,
        ODSY_GL_VERTEX_2F, 0x41f00000, 0x41000000,
        ODSY_GL_VERTEX_2F, 0x41000000, 0x41f00000,
        ODSY_CMD_GL_END,
    };

    push_user_words(qts, colour, n);
    push_user_words(qts, triangle, ARRAY_SIZE(triangle));
}

/*
 * glColor4f's 0xc580d015 and four floats: in RGBA mode the alpha reaches
 * the stored fourth byte under 0x452 bit 23, rounded as floor(a * 255 +
 * 0.5); three-float colours store 0xff. The index-context twin 0xce80d015
 * draws nothing in either mode. These guard the four-float colour contract;
 * the model needed no change for it.
 */
static void test_rgba_four_float_alpha(void)
{
    static const uint32_t half[] = { 0xc580d015, ODSY_F_ONE, 0, 0, 0x3f000000 };
    static const uint32_t opaque[] = {
        0xc580d015, ODSY_F_ONE, 0, 0, ODSY_F_ONE,
    };
    static const uint32_t three[] = { ODSY_GL_COLOR_3F, ODSY_F_ONE, 0, 0 };
    static const uint32_t green[] = { ODSY_GL_COLOR_3F, 0, ODSY_F_ONE, 0 };
    static const uint32_t index5[] = { ODSY_CMD_GL_COLOR_INDEX, 5 };
    static const uint32_t scratch[] = {
        0xce80d015, ODSY_F_ONE, 0, 0, 0x3f000000,
    };
    QTestState *qts = board_start("");

    bitmap_rect_depth_window(qts);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00f00000);
    rgba_rect(qts, 0, 0, 0, 0, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0);
    four_float_triangle(qts, half, ARRAY_SIZE(half));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0xff000080);
    four_float_triangle(qts, opaque, ARRAY_SIZE(opaque));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0xff0000ff);
    four_float_triangle(qts, three, ARRAY_SIZE(three));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0xff0000ff);

    /* Alpha masked off: the cleared alpha byte stays. */
    rgba_rect(qts, 0, 0, 0, 0, 10, 10, 14, 14);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00700000);
    four_float_triangle(qts, half, ARRAY_SIZE(half));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0xff000000);

    /* The scratch form changes nothing drawn. */
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00f00000);
    push_user_words(qts, green, ARRAY_SIZE(green));
    four_float_triangle(qts, scratch, ARRAY_SIZE(scratch));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0x00ff00ff);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    push_user_words(qts, index5, ARRAY_SIZE(index5));
    four_float_triangle(qts, scratch, ARRAY_SIZE(scratch));
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 5);
    board_quit(qts);
}

static void test_rgba_index_transitions(void)
{
    QTestState *qts = board_start("");

    rgba_rect(qts, 0, 0, 1, 0.5f, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0x0000ff80);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_A);
    user_cmd_push(qts, ODSY_GL_MODE_INDEX_B);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00100000);
    rgba_rect(qts, 3, 0, 0, 0.5f, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 3);
    user_cmd_push(qts, 0x22008000);
    user_cmd_push(qts, 0x23008002);
    set_reg(qts, ODSY_REG_WRITE_MASK, 0x00800000);
    rgba_rect(qts, 1, 1, 1, 0.5f, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, 0x00000080);
    board_quit(qts);
}

static void test_rgba_texture_alpha(void)
{
    const uint32_t pixels[4] = {
        0x80402080, 0x80402080, 0x80402080, 0x80402080,
    };
    const uint32_t material[] = {
        0xc780d030, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x3e800000,
    };
    const struct {
        uint32_t sampler, environment, alpha;
    } cases[] = {
        { 0x13000000, 1, 128 },
        { 0x13000000, 2, 32 },
        { 0x13000000, 3, 64 },
        { 0x12800000, 1, 64 },
        { 0x12800000, 3, 64 },
        { 0x11000000, 1, 64 },
        { 0x11000000, 2, 64 },
    };
    QTestState *qts = board_start("");

    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    push_program(qts, 0, 0x5cb, 0x5d8, 0x5e5);
    push_user_words(qts, material, ARRAY_SIZE(material));
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        texture_adapter_bind(qts, 0x180000, cases[i].sampler,
                              cases[i].environment);
        texture_adapter_point(qts, 10 + i, 10, 0.25f, 0.25f);
        g_assert_cmphex(odsy_test_pixel(qts, 10 + i, 10) & 0xff, ==,
                        cases[i].alpha);
    }
    board_quit(qts);
}

static void test_rgba_disabled_colour_depth(void)
{
    const uint32_t vertices[9] = {
        0x41000000, 0x41000000, 0,
        0x41f00000, 0x41000000, 0,
        0x41000000, 0x41f00000, 0,
    };
    const uint32_t nearer[9] = {
        0x41000000, 0x41000000, 0xbf000000,
        0x41f00000, 0x41000000, 0xbf000000,
        0x41000000, 0x41f00000, 0xbf000000,
    };
    QTestState *qts = board_start("");

    bitmap_rect_depth_window(qts);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DEPTH_ONLY);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, vertices);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, ODSY_PLANE_BLUE);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    /* Equal depth now fails: disabled colour still allowed the depth write. */
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, 0, vertices);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, ODSY_PLANE_BLUE);
    push_triangle_3f(qts, ODSY_F_ONE, 0, 0, nearer);
    g_assert_cmphex(odsy_test_pixel(qts, 12, 12), ==, ODSY_PLANE_RED);
    board_quit(qts);
}

static void test_rgba_read_bounds(void)
{
    const struct {
        uint32_t x, y;
        bool readable;
    } cases[] = {
        { 0, 0, true },
        { 1279, 1023, true },
        { 1280, 0, false },
        { 0, 1024, false },
        { UINT32_MAX, 0, false },
    };
    QTestState *qts = board_start("");

    /* Valid zero data must remain distinguishable from failed reads. */
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QDict *resp;

        odsy_test_pixel_select(qts, cases[i].x, cases[i].y);
        resp = qtest_qmp(qts,
            "{'execute':'qom-get', 'arguments':{'path':%s, "
            "'property':'x-test-rgba-readable'}}", BOARD);
        g_assert_true(qdict_haskey(resp, "return"));
        g_assert_cmpint(qdict_get_bool(resp, "return"), ==, cases[i].readable);
        qobject_unref(resp);
    }
    board_quit(qts);
}

/*
 * Separate colour buffers (hw/misc/sgi-odyssey.c, ODSY_REG_DRAW_MASK). The
 * model names its two buffers by the displayed-buffer bits' sense: a window
 * whose ODSY_DISPLAYED_BUFFER bit is clear shows buffer 0, set buffer 1.
 * Register 0x0424 carries the drawing mask the kernel resolves: bits 1:0
 * buffer 0, bits 3:2 buffer 1, left eye before right. x-test-pixel-buffer
 * picks the buffer x-test-pixel reads.
 */
#define ODSY_CMD_DRAW_MASK 0x00010424u
#define ODSY_DRAW_BUFFER0 0x1u
#define ODSY_DRAW_BUFFER1 0x4u
#define ODSY_DRAW_BUFFER1_RIGHT 0x8u
#define ODSY_DRAW_BOTH 0x5u
#define ODSY_DRAW_NONE 0x0u

static void draw_mask(QTestState *qts, uint32_t mask)
{
    cmd_push(qts, ODSY_CMD_DRAW_MASK);
    cmd_push(qts, mask);
}

static uint32_t odsy_buffer_pixel(QTestState *qts, uint32_t buffer,
                                  uint32_t x, uint32_t y)
{
    uint32_t value;

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-buffer', 'value': %u}}", BOARD, buffer);
    value = odsy_test_pixel(qts, x, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-buffer', 'value': 0}}", BOARD);
    return value;
}

/* Store plane word value at (x, y) of colour buffer buffer. */
static void odsy_buffer_pixel_write(QTestState *qts, uint32_t buffer,
                                    uint32_t x, uint32_t y, uint32_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-buffer', 'value': %u}}", BOARD, buffer);
    odsy_test_pixel_write(qts, x, y, value);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-buffer', 'value': 0}}", BOARD);
}

/*
 * The same box drawn red into buffer 0 and blue into buffer 1 holds both
 * colours at once. The window shows buffer 0 until its swap latches at a
 * retrace, then buffer 1. A mask naming both buffers writes both, and a zero
 * mask writes neither. Under one colour plane the second fill overwrote the
 * first and the display never changed at the swap.
 */
static void test_buffers_hold_opposite_colours(void)
{
    QTestState *qts = board_start("");

    display_identity(qts, 0);
    rgb_fill(qts, 0xff, 0, 0, 100, 100, 110, 110);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    rgb_fill(qts, 0, 0, 0xff, 100, 100, 110, 110);

    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 105, 105), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 105, 105), ==, 0x0000ffffu);
    g_assert_cmphex(odsy_test_display_pixel(qts, 105, 105), ==, 0x00ff0000);

    /* The id plane is 0 here: window 0's bit selects the buffer. */
    reg_write(qts, ODSY_SWAP_ARM, 0x1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 105, 105), ==, 0x00ff0000);
    fire_retrace(qts);
    g_assert_cmphex(odsy_test_display_pixel(qts, 105, 105), ==, 0x000000ff);

    draw_mask(qts, ODSY_DRAW_BOTH);
    rgb_fill(qts, 0, 0xff, 0, 100, 100, 102, 102);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 100, 100), ==, 0x00ff00ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 100, 100), ==, 0x00ff00ffu);

    draw_mask(qts, ODSY_DRAW_NONE);
    rgb_fill(qts, 0xff, 0xff, 0xff, 100, 100, 110, 110);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 105, 105), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 105, 105), ==, 0x0000ffffu);
    board_quit(qts);
}

/*
 * A double-buffered client's frames: each is drawn in two halves into the
 * buffer the window does not show, the swap is armed, and only the retrace
 * shows it. The kernel exchanges the mask's pairs once the drawable's
 * buffer state changes, so the client's "back" alternates between buffer 1
 * and buffer 0. Neither a half-drawn frame nor an armed swap reaches the
 * display before the retrace, over four swaps.
 */
static void test_buffers_swap_shows_only_complete_frames(void)
{
    static const uint8_t frame[4][3] = {
        { 0xff, 0, 0 }, { 0, 0xff, 0 }, { 0, 0, 0xff }, { 0xff, 0xff, 0 },
    };
    QTestState *qts = board_start("");
    uint32_t shown = 0x00000000;

    display_identity(qts, 0);
    route_to_pending(qts);
    for (unsigned i = 0; i < ARRAY_SIZE(frame); i++) {
        uint32_t back = (i & 1) ? 0 : 1;
        uint32_t colour = frame[i][0] << 16 | frame[i][1] << 8 | frame[i][2];

        draw_mask(qts, back ? ODSY_DRAW_BUFFER1 : ODSY_DRAW_BUFFER0);
        rgb_fill(qts, frame[i][0], frame[i][1], frame[i][2], 0, 0, 64, 32);
        g_assert_cmphex(odsy_test_display_pixel(qts, 10, 10), ==, shown);
        rgb_fill(qts, frame[i][0], frame[i][1], frame[i][2], 0, 32, 64, 64);
        g_assert_cmphex(odsy_test_display_pixel(qts, 10, 40), ==, shown);

        reg_write(qts, ODSY_SWAP_ARM, 0x1);
        g_assert_cmphex(odsy_test_display_pixel(qts, 10, 10), ==, shown);
        g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, !back);

        fire_retrace(qts);
        g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, back);
        g_assert_cmphex(odsy_test_display_pixel(qts, 10, 10), ==, colour);
        g_assert_cmphex(odsy_test_display_pixel(qts, 10, 40), ==, colour);
        /* An idle retrace keeps the frame. */
        fire_retrace(qts);
        g_assert_cmphex(odsy_test_display_pixel(qts, 10, 40), ==, colour);
        shown = colour;
    }
    board_quit(qts);
}

/* A colour destination select, ending an id-plane write's selection. */
static void colour_destination(QTestState *qts)
{
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x142000);
    cmd_push(qts, 0x50);
    cmd_push(qts, ODSY_DEST_COLOUR_INDEX);
    cmd_push(qts, 0);
}

/*
 * Each window shows its own buffer: pixels whose main drawable id is 1 and 2
 * sit side by side over the same two buffers. A swap of window 2 changes
 * only its pixels, and the immediate set and clear registers change one
 * window each at once, without a retrace.
 */
static void test_buffers_display_selection_per_window(void)
{
    QTestState *qts = board_start("");

    display_identity(qts, 1);
    display_identity(qts, 2);
    id_plane_write(qts, 1, 0xf8, 0, 0, 50, 50);
    id_plane_write(qts, 2, 0xf8, 50, 0, 100, 50);
    colour_destination(qts);
    rgb_fill(qts, 0xff, 0, 0, 0, 0, 100, 50);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    rgb_fill(qts, 0, 0, 0xff, 0, 0, 100, 50);

    g_assert_cmphex(odsy_test_display_pixel(qts, 25, 25), ==, 0x00ff0000);
    g_assert_cmphex(odsy_test_display_pixel(qts, 75, 25), ==, 0x00ff0000);

    reg_write(qts, ODSY_SWAP_ARM, 1u << 2);
    fire_retrace(qts);
    g_assert_cmphex(odsy_test_display_pixel(qts, 25, 25), ==, 0x00ff0000);
    g_assert_cmphex(odsy_test_display_pixel(qts, 75, 25), ==, 0x000000ff);

    reg_write(qts, ODSY_SWAP_CONTROL, 1u << 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 25, 25), ==, 0x000000ff);
    g_assert_cmphex(odsy_test_display_pixel(qts, 75, 25), ==, 0x000000ff);
    reg_write(qts, ODSY_SWAP_CONTROL_COMPANION, 1u << 2);
    g_assert_cmphex(odsy_test_display_pixel(qts, 25, 25), ==, 0x000000ff);
    g_assert_cmphex(odsy_test_display_pixel(qts, 75, 25), ==, 0x00ff0000);
    board_quit(qts);
}

/*
 * Drawing into buffer 1 keeps every per-pixel rule a buffer-0 write has:
 * the window clip, the GL colour write mask and a copy stay within the
 * selected buffer, and buffer 0 is untouched throughout. A right-eye bit
 * draws its buffer, stereo being folded into the left eye.
 */
static void test_buffers_draw_selection_keeps_clip_and_masks(void)
{
    QTestState *qts = board_start("");

    rgb_fill(qts, 0x10, 0x20, 0x30, 0, 0, 200, 200);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    window_clip(qts, (const uint32_t[][4]) { { 20, 20, 39, 39 } }, 1);
    rgb_fill(qts, 0x44, 0x55, 0x66, 0, 0, 100, 100);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 20, 20), ==, 0x445566ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 39, 39), ==, 0x445566ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 19, 30), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 40, 30), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 30, 30), ==, 0x102030ffu);

    /* A GL clear under a red-only write mask changes red alone. */
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    user_cmd_push(qts, ODSY_REG_WRITE_MASK);
    user_cmd_push(qts, 0x00100000);
    push_clear(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ONE, 0x43000000,
               0x43000000);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 30, 30), ==, 0xff5566ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 100, 100), ==, 0xff000000u);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 30, 30), ==, 0x102030ffu);

    /* A screen copy runs inside buffer 1. */
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, 20 | (20 << 16));
    cmd_push(qts, 0x80502050);
    cmd_push(qts, 4 | (4 << 16));
    cmd_push(qts, 0x82223042);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, 300 | (300 << 16));
    cmd_push(qts, 0x3222204b);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 300, 300), ==, 0xff5566ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 300, 300), ==, 0);

    draw_mask(qts, ODSY_DRAW_BUFFER1_RIGHT);
    rgb_fill(qts, 0x77, 0x88, 0x99, 500, 500, 501, 501);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 500, 500), ==, 0x778899ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 500, 500), ==, 0);
    board_quit(qts);
}

/*
 * The bound drawing (0x608) and reading (0x609) drawable words are kept
 * independently for the pixel-readback path, and neither moves drawing:
 * their decoding is not known, so the drawing mask alone selects the
 * buffer. Reset clears both.
 */
static void test_buffers_read_and_draw_words_are_independent(void)
{
    QTestState *qts = board_start("");

    cmd_push64(qts, ODSY_CMD_SWAP_SYNC_A, 0x80002000u);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-draw-word"), ==,
                    0x80002000u);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-read-word"), ==, 0);
    cmd_push64(qts, ODSY_CMD_SWAP_SYNC_B, 0x00002000u);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-draw-word"), ==,
                    0x80002000u);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-read-word"), ==,
                    0x00002000u);

    rgb_fill(qts, 0x12, 0x34, 0x56, 0, 0, 1, 1);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 0, 0), ==, 0x123456ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 0, 0), ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-draw-word"), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-read-word"), ==, 0);
    board_quit(qts);
}

/*
 * Drawing-mask bits the model does not handle are observable once each:
 * bits above 3, which are ignored, and the right-eye bits, which fold into
 * their left-eye buffer. A guest repeating such a write must not flood the
 * log. The supported low bits of an oversized mask still select buffers.
 */
static void test_buffers_unsupported_mask_reported_once(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-mask-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    draw_mask(qts, 0x15);
    draw_mask(qts, 0x15);
    rgb_fill(qts, 0x12, 0x34, 0x56, 0, 0, 1, 1);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 0, 0), ==, 0x123456ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 0, 0), ==, 0x123456ffu);
    draw_mask(qts, ODSY_DRAW_BUFFER1_RIGHT);
    draw_mask(qts, 0xa);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(count_substr(log, "bits above 3 are not modeled"), ==, 1);
    g_assert_cmpuint(count_substr(log, "stereo is not modeled"), ==, 1);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The shared rasterizer's vertex path follows the drawing mask: the context's
 * draw buffers are fed from 0x0424 before each command, so a triangle lands
 * only in the buffers the mask names. The clear rectangle builds its own
 * targets and does not exercise that path. Under a zero mask the triangle
 * still writes depth, which hides a later farther triangle in both buffers.
 */
static void test_buffers_gl_triangle_follows_mask(void)
{
    QTestState *qts = board_start("");

    push_window_transform(qts, odsy_captured_window_transform);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    push_captured_triangle(qts);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 61, 78), ==, 0xffff00ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 61, 78), ==, 0);
    board_quit(qts);

    qts = board_start("");
    probe_window(qts);
    draw_mask(qts, ODSY_DRAW_BOTH);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE,
                     ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 340), ==,
                    ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 326, 340), ==,
                    ODSY_PLANE_BLUE);

    /* No colour buffer: neither changes, but the depth is written. */
    draw_mask(qts, ODSY_DRAW_NONE);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 340), ==,
                    ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 326, 340), ==,
                    ODSY_PLANE_BLUE);

    /*
     * Both buffers: the far cyan triangle reaches both where only it lies,
     * and the invisible near triangle's depth hides it in the overlap.
     */
    draw_mask(qts, ODSY_DRAW_BOTH);
    push_triangle_3f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE,
                     odsy_far_cyan);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 356, 352), ==,
                    ODSY_PLANE_CYAN);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 356, 352), ==,
                    ODSY_PLANE_CYAN);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 340), ==,
                    ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 326, 340), ==,
                    ODSY_PLANE_BLUE);
    board_quit(qts);
}

/*
 * Buffer 1's contents, the drawing mask and the 0x608/0x609 words survive
 * migration: after it, a fill sent without re-sending the mask still lands
 * in buffer 1 only.
 */
static void test_buffers_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-buffers-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    cmd_push64(from, ODSY_CMD_SWAP_SYNC_A, 0x80002000u);
    cmd_push64(from, ODSY_CMD_SWAP_SYNC_B, 0x00002000u);
    draw_mask(from, ODSY_DRAW_BUFFER1);
    rgb_fill(from, 0, 0, 0xff, 0, 0, 8, 8);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    g_assert_cmphex(odsy_qom_get_uint(to, "x-test-draw-word"), ==,
                    0x80002000u);
    g_assert_cmphex(odsy_qom_get_uint(to, "x-test-read-word"), ==,
                    0x00002000u);
    g_assert_cmphex(odsy_buffer_pixel(to, 1, 4, 4), ==, 0x0000ffffu);
    g_assert_cmphex(odsy_buffer_pixel(to, 0, 4, 4), ==, 0);
    rgb_fill(to, 0xff, 0, 0, 0, 0, 8, 8);
    g_assert_cmphex(odsy_buffer_pixel(to, 1, 4, 4), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_buffer_pixel(to, 0, 4, 4), ==, 0);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_public_border_1d_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-texture-border-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_public_border_sample_1d(from, false, false);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    texture_adapter_point(to, 24, 64, 0.0f, 0.5f);
    g_assert_cmphex(odsy_test_pixel(to, 24, 64), ==, 0x203458ff);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A DMA transfer to the screen and a BEGIN 0xa image raster both reach the
 * buffer the drawing mask names and leave the other untouched.
 */
static void test_texture_public_border_3d_migration(void)
{
    /* GL_CLAMP nearest selects stored index 1 on each negative axis. */
    static const struct {
        float s, t, r;
        unsigned x, y, z;
    } samples[] = {
        { -0.25f, 0.25f, 0.25f, 1, 1, 1 },
        { -0.25f, -0.25f, 0.25f, 1, 1, 1 },
        { -0.25f, -0.25f, -0.25f, 1, 1, 1 },
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-texture-volume-border-XXXXXX",
                             &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    texture_public_border_upload_3d(from, 2, 2, 2, true, 0);
    texture_adapter_window(from);
    push_depth_clear(from, 0, 0, 0, 0);
    texture_public_border_bind_3d(from, 2, 2, 2, true, false);
    for (unsigned i = 0; i < ARRAY_SIZE(samples); i++) {
        int x = 16 + 28 * i;

        texture_adapter_point_3d(from, x, 64, samples[i].s, samples[i].t,
                                 samples[i].r);
        g_assert_cmphex(odsy_test_pixel(from, x, 64), ==,
                        texture_public_border_volume_texel(samples[i].x,
                                                           samples[i].y,
                                                           samples[i].z));
    }
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    for (unsigned i = 0; i < ARRAY_SIZE(samples); i++) {
        int x = 16 + 28 * i;

        texture_adapter_point_3d(to, x, 96, samples[i].s, samples[i].t,
                                 samples[i].r);
        g_assert_cmphex(odsy_test_pixel(to, x, 96), ==,
                        texture_public_border_volume_texel(samples[i].x,
                                                           samples[i].y,
                                                           samples[i].z));
    }
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_texture_public_border_3d_reset(void)
{
    QTestState *qts = board_start("");

    texture_public_border_upload_3d(qts, 2, 2, 2, true, 0);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 2, 2, 2, true, false);
    odsy_test_pixel_write(qts, 10, 10, 0x12345678);
    texture_adapter_point_3d(qts, 10, 10, 0.25f, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0x110101ff);

    qtest_system_reset(qts);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 2, 2, 2, true, false);
    odsy_test_pixel_write(qts, 38, 10, 0x12345678);
    texture_adapter_point_3d(qts, 38, 10, 0.25f, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 38, 10), ==, 0x12345678);

    texture_public_border_upload_3d(qts, 2, 2, 2, true, 0);
    texture_adapter_window(qts);
    push_depth_clear(qts, 0, 0, 0, 0);
    texture_public_border_bind_3d(qts, 2, 2, 2, true, false);
    odsy_test_pixel_write(qts, 66, 10, 0x12345678);
    texture_adapter_point_3d(qts, 66, 10, 0.25f, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 66, 10), ==, 0x110101ff);
    qtest_quit(qts);
}

static void test_buffers_transfers_follow_mask(void)
{
    QTestState *qts = board_start("");
    const uint64_t table = 0xb80000;
    const uint64_t page = 0xb84000;
    /* A 2x1 image of 4-byte pixels at (200, 300). */
    const uint32_t hdr[8] = {
        0x2000, (300 << 16) | 200, 0x50, (1 << 16) | 2, 0x8223305a, 0, 8,
        0x32010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(page, 8) };
    uint8_t px[8];

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x2000);
    user_cmd_push(qts, 0x50);
    user_cmd_push(qts, 0x20006950);
    user_cmd_push(qts, 0);
    draw_mask(qts, ODSY_DRAW_BUFFER1);

    stl_be_p(&px[0], 0x00ff0000);
    stl_be_p(&px[4], 0x0000ff00);
    qtest_memwrite(qts, page, px, sizeof(px));
    dma_image_table(qts, table, hdr, pte, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 200, 300), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 201, 300), ==, 0x00ff00ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 200, 300), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 201, 300), ==, 0);

    image_begin(qts, 100, 100, 102, 101);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(0, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, ODSY_X_PIXEL(0x112233));
    cmd_push(qts, ODSY_X_PIXEL(0x445566));
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 100, 100), ==, 0x112233ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 101, 100), ==, 0x445566ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 100, 100), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 101, 100), ==, 0);
    board_quit(qts);
}

/* The native move emits both colour-plane copies in order. */
static void captured_buffer_copy(QTestState *qts, uint32_t source_plane,
                                 uint32_t dest_plane, uint32_t sy,
                                 uint32_t dy)
{
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX);
    cmd_push(qts, ODSY_CMD_COPY_RECT_PREFIX_ARG);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (sy << 16) | 326);
    cmd_push(qts, 0x80500050 | (source_plane << 12));
    cmd_push(qts, (2u << 16) | 1);
    cmd_push(qts, 0x80035004 | (source_plane << 24) |
                  (dest_plane << 20));
    cmd_push(qts, 0x00002000);
    cmd_push(qts, (dy << 16) | 326);
    cmd_push(qts, 0xf2034000 | (dest_plane << 20));
}

/*
 * The two captured overlapping moves select buffer 0 then buffer 1. Each
 * destination must use that pass's original source, in either vertical
 * direction; the source and row beyond the destination remain unchanged.
 */
static void test_buffers_captured_copy_preserves_both_sources(void)
{
    static const struct {
        uint32_t sy, dy;
        uint32_t guard_y, untouched_source_y, untouched_source_index;
        uint32_t colour[2][2];
        uint32_t guard[2];
    } cases[] = {
        { 339, 340, 342, 339, 0, { { 0xff0000ff, 0x00ff00ff },
                      { 0x0000ffff, 0xffff00ff } },
          { 0xffffffff, 0x777777ff } },
        { 401, 400, 399, 402, 1, { { 0x102030ff, 0x405060ff },
                      { 0x708090ff, 0xa0b0c0ff } },
          { 0xdeadbeff, 0xcafeb0ff } },
    };
    QTestState *qts = board_start("");

    probe_window(qts);
    display_identity(qts, 1);
    id_plane_write(qts, 1, 0xf8, 60, 70, 80, 150);
    colour_destination(qts);

    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_DEPTH_FLAT);
    set_reg(qts, ODSY_REG_DEPTH_FUNC, ODSY_GL_FUNC_LESS);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_ALL);
    push_depth_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE,
                     ODSY_DEPTH_2S);
    set_reg(qts, ODSY_REG_WRITE_MASK, ODSY_WRITE_DRAW);
    push_triangle_3f(qts, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned b = 0; b < 2; b++) {
            odsy_buffer_pixel_write(qts, b, 326, cases[i].sy,
                                    cases[i].colour[b][0]);
            odsy_buffer_pixel_write(qts, b, 326, cases[i].sy + 1,
                                    cases[i].colour[b][1]);
            odsy_buffer_pixel_write(qts, b, 326, cases[i].guard_y,
                                    cases[i].guard[b]);
        }

        captured_buffer_copy(qts, 0, 0, cases[i].sy, cases[i].dy);
        captured_buffer_copy(qts, 4, 4, cases[i].sy, cases[i].dy);

        for (unsigned b = 0; b < 2; b++) {
            uint32_t untouched_source =
                cases[i].colour[b][cases[i].untouched_source_index];

            g_assert_cmphex(odsy_buffer_pixel(qts, b, 326,
                                              cases[i].untouched_source_y), ==,
                            untouched_source);
            g_assert_cmphex(odsy_buffer_pixel(qts, b, 326, cases[i].dy), ==,
                            cases[i].colour[b][0]);
            g_assert_cmphex(odsy_buffer_pixel(qts, b, 326, cases[i].dy + 1),
                            ==, cases[i].colour[b][1]);
            g_assert_cmphex(odsy_buffer_pixel(qts, b, 326, cases[i].guard_y),
                            ==, cases[i].guard[b]);
        }
    }

    /* The two selector fields also route across buffers independently. */
    odsy_buffer_pixel_write(qts, 0, 326, 601, 0x112233ff);
    odsy_buffer_pixel_write(qts, 0, 326, 602, 0x445566ff);
    odsy_buffer_pixel_write(qts, 1, 326, 601, 0x778899ff);
    odsy_buffer_pixel_write(qts, 1, 326, 602, 0xaabbccff);
    odsy_buffer_pixel_write(qts, 0, 326, 612, 0xdeadbeff);
    odsy_buffer_pixel_write(qts, 1, 326, 612, 0xcafeb0ff);
    captured_buffer_copy(qts, 0, 4, 601, 610);
    captured_buffer_copy(qts, 4, 0, 601, 610);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 610), ==, 0x778899ff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 611), ==, 0xaabbccff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 326, 610), ==, 0x112233ff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 326, 611), ==, 0x445566ff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 612), ==, 0xdeadbeff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 326, 612), ==, 0xcafeb0ff);

    /* The copy leaves DID selection and the depth written by the triangle. */
    g_assert_cmphex(odsy_test_display_pixel(qts, 326, 340), ==, 0xff0000);
    draw_mask(qts, ODSY_DRAW_BUFFER0);
    push_triangle_3f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ONE,
                     odsy_far_cyan);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 326, 340), ==, 0xff0000ff);

    board_quit(qts);
}

/*
 * Board-to-host transfers (hw/misc/sgi-odyssey.c, ODSY_DMA_TO_HOST; the rules
 * are software behaviour, not board measurements). A header with word 7 bit 24
 * set reads its rectangle out of the board into the table's entries. Every
 * test seeds the host bytes and their surroundings with a guard byte and
 * checks each byte afterwards.
 */
#define RB_TABLE 0xb80000
#define RB_PAGE_A 0xb84000
#define RB_PAGE_B 0xb90000
#define RB_FENCE 0xb70000
#define RB_GUARD 0xa5
#define RB_READ_SENTINEL 0x30000019u
#define RB_DRAW_SENTINEL 0x30000010u
/* GL's front-buffer RGBA/UNSIGNED_BYTE read word 4: class 3, code 1. */
#define RB_GL_FRONT_RGBA 0x9003500bu
/* The probe's window origin: drawable and screen coordinates differ. */
#define RB_ORIGIN_X 512
#define RB_ORIGIN_Y 256

static QTestState *readback_start(const char *extra)
{
    QTestState *qts = board_start(extra);

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_BANKS);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_UPPER, 0);
    reg_write(qts, ODSY_HOST_WRITE_ADDR_LOWER, RB_FENCE);
    route_to_pending(qts);
    /* The X server's colour destination select names buffer 0x2000. */
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x2000);
    user_cmd_push(qts, 0x50);
    user_cmd_push(qts, 0x20006950);
    user_cmd_push(qts, 0);
    window_origin(qts, RB_ORIGIN_X, RB_ORIGIN_Y);
    return qts;
}

/* Fill len guest bytes at addr with the guard byte, and expect[] with it. */
static void readback_guard(QTestState *qts, uint64_t addr, uint8_t *expect,
                           size_t len)
{
    memset(expect, RB_GUARD, len);
    qtest_memset(qts, addr, RB_GUARD, len);
}

static void readback_expect(QTestState *qts, uint64_t addr,
                            const uint8_t *expect, size_t len)
{
    g_autofree uint8_t *got = g_malloc(len);

    qtest_memread(qts, addr, got, len);
    g_assert_cmpmem(got, len, expect, len);
}

static void readback_run(QTestState *qts, const uint32_t hdr[8],
                         const uint64_t *pte, unsigned n)
{
    dma_image_table(qts, RB_TABLE, hdr, pte, n);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, RB_TABLE);
}

/*
 * A softsync packet through a 0x0a5a fence with its interrupt bit: the
 * last delivered interrupt becomes the softsync vector, so a later DMA-done
 * shows as a change back to the DMA vector.
 */
static void readback_mark(QTestState *qts)
{
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, ODSY_CMD_HOST_WRITE_INTERRUPT | 7);
    cmd_push(qts, 0x5a07);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_SOFTSYNC_VECTOR | ODSY_HUB_INT_MODIFY_SET);
}

/* How many times needle occurs in log. */
static unsigned readback_count(const char *log, const char *needle)
{
    unsigned n = 0;

    for (const char *p = strstr(log, needle); p; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

static bool readback_dma_done(QTestState *qts)
{
    return odsy_qom_get_uint(qts, "x-test-last-interrupt-value") ==
           (ODSY_TEST_DMA_VECTOR | ODSY_HUB_INT_MODIFY_SET);
}

/*
 * Backing-store pages. The IRIX 6.5.30 kernel's board-memory manager pages
 * an object out to host memory and back in with raw copies, word 4 0x1001
 * at 2 bytes a texel and 0x13002 at 4, word 7 0x3x000000 and 0x3x010000
 * (its header builder, from disassembly). The host bytes are only stored
 * and returned, so the model packs a texel at 2 bytes as R, G, B, A
 * nibbles, an APPROXIMATION, and carries the stored word at 4. Each round
 * trip into a second object restores the texels to that precision. The
 * 8-byte form, word 4 0x25004, is reported and writes nothing.
 */
static void test_readback_raw_texture_paging(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 4) };
    const uint64_t pte8[] = { ODSY_DMA_PTE(RB_PAGE_A, 8) };
    const uint64_t pte16[] = { ODSY_DMA_PTE(RB_PAGE_A, 16) };
    const uint32_t out16[8] = {
        0x100800, 0, 1, (1u << 16) | 2, 0x1001, 0, 4, 0x33000000,
    };
    const uint32_t in16[8] = {
        0x101000, 0, 1, (1u << 16) | 2, 0x1001, 0, 4, 0x32000000,
    };
    const uint32_t out32[8] = {
        0x100800, 0, 1, (1u << 16) | 2, 0x13002, 0, 8, 0x33010000,
    };
    const uint32_t in32[8] = {
        0x101800, 0, 1, (1u << 16) | 2, 0x13002, 0, 8, 0x32010000,
    };
    const uint32_t out64[8] = {
        0x100800, 0, 1, (1u << 16) | 2, 0x25004, 0, 16, 0x33020000,
    };
    const uint8_t packed[4] = { 0xf0, 0x0f, 0x0f, 0x88 };
    const uint8_t words[8] = { 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x80, 0x80 };
    uint8_t guard[16];
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-paging-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = readback_start(extra);

    dma_object_texel(qts, 0x100800, 1, 0, 0, 0xff0000ff);
    dma_object_texel(qts, 0x100800, 1, 1, 0, 0x00ff8080);

    qtest_memset(qts, RB_PAGE_A, RB_GUARD, 16);
    readback_run(qts, out16, pte, 1);
    readback_expect(qts, RB_PAGE_A, packed, sizeof(packed));
    readback_run(qts, in16, pte, 1);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x101000, 0, 0), ==,
                    0xff0000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x101000, 1, 0), ==,
                    0x00ff8888);

    readback_run(qts, out32, pte8, 1);
    readback_expect(qts, RB_PAGE_A, words, sizeof(words));
    readback_run(qts, in32, pte8, 1);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x101800, 0, 0), ==,
                    0xff0000ff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x101800, 1, 0), ==,
                    0x00ff8080);

    readback_guard(qts, RB_PAGE_A, guard, sizeof(guard));
    readback_run(qts, out64, pte16, 1);
    readback_expect(qts, RB_PAGE_A, guard, sizeof(guard));
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "8-byte backing-store page of object "
                                 "0x100800 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * Direction is word 7 bit 24 alone. The X server's 24-bit visual header
 * with the 0x4c format byte reads a screen pixel back as the bytes A, B, G,
 * R, alpha zero -- the layout the upload path decodes for that byte --
 * and leaves the plane alone. The same header with bit 24 clear paints the
 * host bytes and writes nothing to the guest. With a zero format byte the
 * GL vocabulary applies, where this word 4 (class 2, code 0xb) is not
 * modeled: the read is refused and writes nothing.
 */
static void test_readback_direction(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        0x2000, (300 << 16) | 200, 0x50, 0x10001, 0x8223305a, 0, 4,
        0x3301004c,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    const uint8_t seed[4] = { 0x00, 0x11, 0x22, 0x33 };
    uint8_t expect[36];

    odsy_buffer_pixel_write(qts, 0, 200, 300, 0xc0ffeeffu);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    expect[16] = 0x00;
    expect[17] = 0xee;
    expect[18] = 0xff;
    expect[19] = 0xc0;
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0xc0ffeeffu);
    /* Bit 31 clear: no DMA-done. */
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);

    /* Bit 24 clear: an upload of the seed, and the guest stays as it was. */
    qtest_memwrite(qts, RB_PAGE_A + 16, seed, sizeof(seed));
    memcpy(&expect[16], seed, sizeof(seed));
    hdr[7] = 0x3201004c;
    readback_run(qts, hdr, pte, 1);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0x332211ffu);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* A zero format byte with this word 4: refused, nothing written. */
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    hdr[7] = 0x33010000;
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0x332211ffu);

    board_quit(qts);
}

/*
 * The X server's 24-bit TrueColor visuals have red mask 0xff and blue mask
 * 0xff0000, so an XPutImage pixel is 0x00BBGGRR and the server's DMA put
 * names it with source code 0x4c, A, B, G, R. clogin's EZsetup icon, traced:
 * word 4 0x8223305a, word 7 0x3201004c, and an orange pixel (red 0xfd,
 * green 0x9b, blue 0x01) arriving as the bytes 00 01 9b fd. It must land
 * orange, not blue.
 */
static void test_readback_put_abgr_x_image(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        0x2000, (300 << 16) | 200, 0x50, 0x10001, 0x8223305a, 0, 4,
        0x3201004c,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 4) };
    const uint8_t pixel[4] = { 0x00, 0x01, 0x9b, 0xfd };

    qtest_memwrite(qts, RB_PAGE_A, pixel, sizeof(pixel));
    readback_run(qts, hdr, pte, 1);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0xfd9b01ffu);

    board_quit(qts);
}

static void test_draw_sentinel_short_upload(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        RB_DRAW_SENTINEL, (2 << 16) | 3, 0x50, (2 << 16) | 2,
        0x02635000, 0, 16, 0x7a02006b,
    };
    const uint64_t pte[] = {
        ODSY_DMA_PTE(RB_PAGE_A, 3), ODSY_DMA_PTE(RB_PAGE_B, 29),
    };
    const uint8_t pixels[32] = {
        0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0x40, 0x00,
        0xff, 0xff, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff,
        0x80, 0x00, 0x40, 0x00, 0x20, 0x00, 0x00, 0x00,
        0x7f, 0xff, 0xff, 0xff, 0x00, 0x01, 0x80, 0x00,
    };

    window_origin(qts, 100, 200);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);
    qtest_memwrite(qts, RB_PAGE_A, pixels, 3);
    qtest_memwrite(qts, RB_PAGE_B, pixels + 3, sizeof(pixels) - 3);

    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));

    /* Source row zero lands on the lower screen row; each pixel is RGBA16. */
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 203), ==, 0x0080ff40);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 104, 203), ==, 0xff0080ff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 202), ==, 0x80402000);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 104, 202), ==, 0x7fff0080);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 105, 203), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 103, 203), ==, 0);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);

    board_quit(qts);
}

/*
 * The GL library's ABGR image write (ipaste, bgpaste): code 0x6c, four
 * bytes a pixel in A, B, G, R order, bottom row first, word 1 relative to
 * the window. Callers differ in word 7 bit 27; both land. Any other bit of
 * words 4 and 7, bit 30 or bit 31 included, leaves the write unpainted.
 */
static void test_draw_sentinel_abgr_upload(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        RB_DRAW_SENTINEL, (2 << 16) | 3, 0x50, (2 << 16) | 2,
        0x80035000, 0, 8, 0x7201016c,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 16) };
    const uint8_t pixels[16] = {
        0xff, 0x7f, 0x80, 0x97, 0x80, 0x00, 0x00, 0xff,
        0x40, 0x00, 0xff, 0x00, 0x00, 0x11, 0x22, 0x33,
    };

    window_origin(qts, 100, 200);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    qtest_memwrite(qts, RB_PAGE_A, pixels, sizeof(pixels));
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));

    /* Source row zero lands on the lower row; each word is A, B, G, R. */
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 203), ==, 0x97807fff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 104, 203), ==, 0xff000080);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 202), ==, 0x00ff0040);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 104, 202), ==, 0x33221100);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 105, 203), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 103, 203), ==, 0);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);

    /* bgpaste's word 7 differs in bit 27 and lands the same way. */
    window_origin(qts, 300, 400);
    hdr[7] = 0x7a01016c;
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 303, 403), ==, 0x97807fff);

    /* Without bit 30, or with an unknown bit such as 31, nothing lands. */
    window_origin(qts, 500, 600);
    hdr[7] = 0x3201016c;
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 503, 603), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 503, 602), ==, 0);
    hdr[7] = 0xc201016c;
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 503, 603), ==, 0);
    /* Another word 4 is refused too. */
    hdr[7] = 0x7201016c;
    hdr[4] = 0x80035001;
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 503, 603), ==, 0);

    board_quit(qts);
}

/*
 * dmplay's movie write: the ABGR bytes under code 0x4c with word 4
 * 0x82233000. Its y counts up from the window's bottom, taken as the 0x81d
 * clip box's last row, and the first source row is the top of the tile:
 * in a 10-row window, a 2x2 tile at y 6 covers window rows 2 and 3, source
 * row zero on row 2. Another word 4 leaves the write unpainted. Before,
 * every movie frame was refused and the window stayed black.
 */
static void test_draw_sentinel_movie_upload(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        RB_DRAW_SENTINEL, (6 << 16) | 3, 0x50, (2 << 16) | 2,
        0x82233000, 0, 8, 0x7a01004c,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 16) };
    const uint8_t pixels[16] = {
        0xff, 0x7f, 0x80, 0x97, 0x80, 0x00, 0x00, 0xff,
        0x40, 0x00, 0xff, 0x00, 0x00, 0x11, 0x22, 0x33,
    };

    window_origin(qts, 100, 200);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    draw_mask(qts, ODSY_DRAW_BUFFER1);
    clip_box_set(qts, 0, 319, 0, 9);
    qtest_memwrite(qts, RB_PAGE_A, pixels, sizeof(pixels));
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));

    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 202), ==, 0x97807fff);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 104, 202), ==, 0xff000080);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 203), ==, 0x00ff0040);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 104, 203), ==, 0x33221100);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 201), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 103, 204), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 105, 202), ==, 0);

    window_origin(qts, 500, 600);
    hdr[4] = 0x82233001;
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 503, 602), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 503, 603), ==, 0);

    board_quit(qts);
}

static void test_draw_sentinel_short_row_gap(void)
{
    QTestState *qts = readback_start("");
    const uint32_t rowbytes = 64 * 8;
    const uint32_t skip = 512;
    const uint32_t height = 128;
    const uint32_t active_bytes = rowbytes * height;
    const uint32_t max_source_bytes = active_bytes + skip * (height - 1);
    const uint64_t source[2] = {
        RB_PAGE_A + 0x20000, RB_PAGE_A + 0x50000,
    };
    const uint32_t entry_lengths[2][9] = {
        { 512, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 7680 },
        { 400, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 7792 },
    };
    const uint8_t guard[16] = {
        RB_GUARD, RB_GUARD, RB_GUARD, RB_GUARD,
        RB_GUARD, RB_GUARD, RB_GUARD, RB_GUARD,
        RB_GUARD, RB_GUARD, RB_GUARD, RB_GUARD,
        RB_GUARD, RB_GUARD, RB_GUARD, RB_GUARD,
    };

    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    draw_mask(qts, ODSY_DRAW_BUFFER1);

    for (unsigned chunk = 0; chunk < 2; chunk++) {
        uint64_t logical_start[9], physical_start[9];
        uint64_t logical = 0, physical = 0;
        uint64_t pte[9];
        uint64_t base = source[chunk];
        g_autofree uint8_t *pixels = g_malloc(max_source_bytes);
        uint32_t hdr[8] = {
            RB_DRAW_SENTINEL, chunk ? 64 : 0, 0x50,
            (height << 16) | 64, 0x02635000, skip, rowbytes,
            0x7a02006b,
        };

        for (unsigned i = 0; i < ARRAY_SIZE(pte); i++) {
            uint64_t end = logical + entry_lengths[chunk][i];
            uint64_t boundary = (logical / rowbytes + 1) * rowbytes;
            unsigned gaps = 0;

            logical_start[i] = logical;
            physical_start[i] = physical;
            while (boundary < end) {
                gaps++;
                boundary += rowbytes;
            }
            pte[i] = ODSY_DMA_PTE(base + physical,
                                  entry_lengths[chunk][i]);
            physical += entry_lengths[chunk][i] + (uint64_t)gaps * skip;
            logical = end;
        }
        g_assert_cmpuint(logical, ==, active_bytes);

        memset(pixels, 0xa5, physical);
        for (uint32_t r = 0; r < height; r++) {
            for (uint32_t c = 0; c < 64; c++) {
                uint64_t active = (uint64_t)r * rowbytes + c * 8;
                unsigned i;
                uint64_t gap_count = 0, boundary, end;
                uint8_t *pixel;
                uint16_t red = r ? (r + 1) * 257 : 0x4000;
                uint16_t green = c ? (c + 1) * 257 : 0x2000;
                uint16_t blue = c ? (r ^ c) * 257 : 0;
                uint16_t alpha = r ? UINT16_MAX : 0x6000;

                for (i = 0; i + 1 < ARRAY_SIZE(pte); i++) {
                    if (active < logical_start[i] +
                                 entry_lengths[chunk][i]) {
                        break;
                    }
                }
                end = logical_start[i] + entry_lengths[chunk][i];
                boundary = (logical_start[i] / rowbytes + 1) * rowbytes;
                while (boundary <= active && boundary < end) {
                    gap_count++;
                    boundary += rowbytes;
                }
                pixel = pixels + physical_start[i] +
                        active - logical_start[i] + gap_count * skip;
                stw_be_p(&pixel[0], red);
                stw_be_p(&pixel[2], green);
                stw_be_p(&pixel[4], blue);
                stw_be_p(&pixel[6], alpha);
            }
        }
        qtest_memwrite(qts, base, pixels, physical);
        qtest_memwrite(qts, base - sizeof(guard), guard, sizeof(guard));
        qtest_memwrite(qts, base + physical, guard, sizeof(guard));

        for (unsigned origin = 0; origin < 2; origin++) {
            uint32_t origin_x = origin ? 256 : 64;
            uint32_t origin_y = origin ? 256 : 64;

            window_origin(qts, origin_x, origin_y);
            readback_run(qts, hdr, pte, ARRAY_SIZE(pte));

            g_assert_cmphex(odsy_buffer_pixel(qts, 1,
                                              origin_x + (chunk ? 64 : 0),
                                              origin_y + height - 1), ==,
                            0x40200060);
            g_assert_cmphex(odsy_buffer_pixel(qts, 1,
                                              origin_x + (chunk ? 64 : 0),
                                              origin_y + height - 2), ==,
                            0x022000ff);
            g_assert_cmphex(odsy_buffer_pixel(qts, 1,
                                              origin_x + (chunk ? 64 : 0),
                                              origin_y), ==,
                            0x802000ff);
        }
        readback_expect(qts, base - sizeof(guard), guard, sizeof(guard));
        readback_expect(qts, base + physical, guard, sizeof(guard));
    }

    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);
    board_quit(qts);
}

static void test_draw_sentinel_short_zero_origin_clip(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        RB_DRAW_SENTINEL, 0, 0x50, (2 << 16) | 2,
        0x02635000, 0, 16, 0x7a02006b,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 32) };
    const uint8_t pixels[32] = {
        0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0x40, 0x00,
        0xff, 0xff, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff,
        0x80, 0x00, 0x40, 0x00, 0x20, 0x00, 0x00, 0x00,
        0x7f, 0xff, 0xff, 0xff, 0x00, 0x01, 0x80, 0x00,
    };

    window_origin(qts, 0, 0);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 0, 0 } }, 1);
    qtest_memwrite(qts, RB_PAGE_A, pixels, sizeof(pixels));
    readback_run(qts, hdr, pte, ARRAY_SIZE(pte));

    /* Bottom-up row one maps to screen row zero; clip keeps one pixel. */
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0x80402000);
    g_assert_cmphex(odsy_test_pixel(qts, 1, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 1), ==, 0);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);

    board_quit(qts);
}

static void test_draw_sentinel_short_rejections(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        RB_DRAW_SENTINEL, 0, 0x50, (2 << 16) | 2,
        0x02635000, 0, 16, 0x7a02006b,
    };
    const uint64_t good_pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 32) };
    const uint64_t short_pte[] = { ODSY_DMA_PTE(RB_PAGE_A, 31) };
    const uint64_t failed_pte[] = {
        ODSY_DMA_PTE(RB_PAGE_A, 16), ODSY_DMA_PTE(0x80000000, 16),
    };
    const uint8_t pixels[32] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };

    window_origin(qts, 0, 0);
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    qtest_memwrite(qts, RB_PAGE_A, pixels, sizeof(pixels));

    /* An unsupported control is observable but cannot paint. */
    hdr[7] ^= 1;
    readback_run(qts, hdr, good_pte, ARRAY_SIZE(good_pte));
    hdr[7] ^= 1;

    /* An unsupported format remains refused even with a valid control. */
    hdr[4] ^= 1;
    readback_run(qts, hdr, good_pte, ARRAY_SIZE(good_pte));
    hdr[4] ^= 1;

    /* Wrong row bytes and a short entry payload are rejected before reads. */
    hdr[6] = 8;
    readback_run(qts, hdr, good_pte, ARRAY_SIZE(good_pte));
    hdr[6] = 16;
    readback_run(qts, hdr, short_pte, ARRAY_SIZE(short_pte));

    /* A later DMA read failure leaves the earlier staged row unpainted. */
    readback_run(qts, hdr, failed_pte, ARRAY_SIZE(failed_pte));

    g_assert_cmphex(odsy_test_pixel(qts, 0, 1), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1, 1), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1, 0), ==, 0);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);

    board_quit(qts);
}

/*
 * The GL library's one-pixel read of the read drawable, as captured: word 1
 * is drawable-relative, so the window origin is added and the pixel at
 * origin + (64, 63) is read, not the one at screen (64, 63). R, G, B, A
 * land in address order, guards untouched, DMA-done raised, and no
 * board-memory object is created for the sentinel. A 0x0a5a fence pushed
 * behind the descriptor finds the bytes already there; ordering is visible
 * to a qtest only as both being present when the fence is.
 */
static void test_readback_sentinel_drawable_relative(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        RB_READ_SENTINEL, 0x003f0040, 0x50, 0x10001, RB_GL_FRONT_RGBA, 0, 4,
        0xf3010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 0xdf0, 4) };
    uint8_t expect[36];

    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                            0x11223344);
    odsy_buffer_pixel_write(qts, 0, 64, 63, 0x55667788);
    readback_guard(qts, RB_PAGE_A + 0xde0, expect, sizeof(expect));
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);

    readback_run(qts, hdr, pte, 1);
    cmd_push(qts, ODSY_CMD_HOST_WRITE_SELECT);
    cmd_push(qts, 1);
    cmd_push(qts, 0x5a01);

    g_assert_cmphex(qtest_readw(qts, RB_FENCE + 2), ==, 0x5a01);
    expect[16] = 0x11;
    expect[17] = 0x22;
    expect[18] = 0x33;
    expect[19] = 0x44;
    readback_expect(qts, RB_PAGE_A + 0xde0, expect, sizeof(expect));
    g_assert_true(readback_dma_done(qts));
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63),
                    ==, 0x11223344);

    board_quit(qts);
}

/*
 * A block with bit 24 and without bit 31 -- the shape of every block but
 * the last of a read the kernel splits -- still writes its bytes, and
 * raises no DMA-done.
 */
static void test_readback_intermediate_block(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        RB_READ_SENTINEL, 0x003f0040, 0x50, 0x10001, RB_GL_FRONT_RGBA, 0, 4,
        0x73010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    uint8_t expect[36];

    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                            0x0a0b0c0d);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    expect[16] = 0x0a;
    expect[17] = 0x0b;
    expect[18] = 0x0c;
    expect[19] = 0x0d;
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    0);
    board_quit(qts);
}

/*
 * Word 4 bit 31 clear reads the back buffer, the one the pixel's drawable
 * does not show. APPROXIMATION under test: the board's use of bit 31
 * against the bound drawable's buffer state is unknown.
 */
static void test_readback_back_buffer(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        RB_READ_SENTINEL, 0x003f0040, 0x50, 0x10001, 0x1003500b, 0, 4,
        0xf3010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    uint8_t expect[36];

    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                            0x11223344);
    odsy_buffer_pixel_write(qts, 1, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                            0x99aabbcc);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    expect[16] = 0x99;
    expect[17] = 0xaa;
    expect[18] = 0xbb;
    expect[19] = 0xcc;
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    hdr[4] = RB_GL_FRONT_RGBA;
    readback_run(qts, hdr, pte, 1);
    expect[16] = 0x11;
    expect[17] = 0x22;
    expect[18] = 0x33;
    expect[19] = 0x44;
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    board_quit(qts);
}

/*
 * A 2x2 read, words 3 and 6 0x20002 and 8, one 16-byte entry: with word 7
 * bit 30 clear host row 0 is the top board row, with it set the bottom one.
 */
/*
 * A double-buffered draw, read, swap loop. Each frame draws a new colour
 * into the buffer the window does not show and leaves the shown one
 * alone; a back read returns the frame just drawn, a front read the shown
 * one, and an address read of 0x2000 the shown one even with word 4 bit 31
 * clear. After the swap latches at the retrace, the frame just drawn is
 * shown and a front read returns it. Main id 0 shows by bit 0 of
 * ODSY_DISPLAYED_BUFFER, which alternates every frame.
 */
static void test_readback_front_follows_displayed_buffer(void)
{
    QTestState *qts = readback_start("");
    const uint32_t px = RB_ORIGIN_X + 64, py = RB_ORIGIN_Y + 63;
    uint32_t sentinel[8] = {
        RB_READ_SENTINEL, 0x003f0040, 0x50, 0x10001, 0, 0, 4, 0xf3010000,
    };
    const uint32_t address[8] = {
        0x2000, (py << 16) | px, 0x50, 0x10001, 0x0223305a, 0, 4,
        0xb301004c,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    uint32_t shown_word = 0x101010ff;
    uint8_t expect[36];
    unsigned frame;

    for (frame = 0; frame < 4; frame++) {
        uint32_t shown = frame & 1;
        uint32_t drawn = 0x20000000u * (frame + 1) | 0x3344ff;

        g_assert_cmphex(reg_read(qts, ODSY_DISPLAYED_BUFFER), ==, shown);
        odsy_buffer_pixel_write(qts, shown, px, py, shown_word);
        odsy_buffer_pixel_write(qts, !shown, px, py, drawn);

        /* Back: the frame just drawn. */
        sentinel[4] = 0x1003500b;
        readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
        readback_run(qts, sentinel, pte, 1);
        stl_be_p(&expect[16], drawn);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

        /* Front: the shown frame. */
        sentinel[4] = RB_GL_FRONT_RGBA;
        readback_run(qts, sentinel, pte, 1);
        stl_be_p(&expect[16], shown_word);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

        /* An address read ignores word 4 and reads the shown buffer. */
        readback_run(qts, address, pte, 1);
        stl_be_p(&expect[16], ODSY_X_PIXEL(shown_word >> 8));
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

        /* Swap: the drawn frame is now the front. */
        reg_write(qts, ODSY_SWAP_ARM, 1);
        fire_retrace(qts);
        readback_run(qts, sentinel, pte, 1);
        stl_be_p(&expect[16], drawn);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
        shown_word = drawn;
    }
    board_quit(qts);
}

static void test_readback_two_by_two_row_order(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        RB_READ_SENTINEL, (20 << 16) | 10, 0x50, 0x20002, RB_GL_FRONT_RGBA, 0,
        8, 0xb3010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 16) };
    const uint8_t top_down[16] = {
        0x10, 0x00, 0x00, 0xff, 0x20, 0x00, 0x00, 0xff,
        0x30, 0x00, 0x00, 0xff, 0x40, 0x00, 0x00, 0xff,
    };
    const uint8_t bottom_up[16] = {
        0x30, 0x00, 0x00, 0xff, 0x40, 0x00, 0x00, 0xff,
        0x10, 0x00, 0x00, 0xff, 0x20, 0x00, 0x00, 0xff,
    };
    uint8_t expect[48];

    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 10, RB_ORIGIN_Y + 20,
                            0x100000ff);
    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 11, RB_ORIGIN_Y + 20,
                            0x200000ff);
    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 10, RB_ORIGIN_Y + 21,
                            0x300000ff);
    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 11, RB_ORIGIN_Y + 21,
                            0x400000ff);

    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    memcpy(&expect[16], top_down, sizeof(top_down));
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_true(readback_dma_done(qts));

    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    hdr[7] = 0xf3010000;
    readback_run(qts, hdr, pte, 1);
    memcpy(&expect[16], bottom_up, sizeof(bottom_up));
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    board_quit(qts);
}

/*
 * A 3x2 read with a 4-byte gap after each host row (word 5) and bit 30:
 * host row 0 is board row 1, the gap is skipped without being written, and
 * the entry counts the 24 pixel bytes only.
 */
static void test_readback_row_gap_bottom_up(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        RB_READ_SENTINEL, (40 << 16) | 30, 0x50, (2 << 16) | 3,
        RB_GL_FRONT_RGBA, 4, 12, 0xf3010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 24) };
    uint8_t expect[60];
    unsigned c, r;

    for (r = 0; r < 2; r++) {
        for (c = 0; c < 3; c++) {
            odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 30 + c,
                                    RB_ORIGIN_Y + 40 + r,
                           (r + 1) << 24 | (c + 1) << 16 | 0x55ff);
        }
    }
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    for (r = 0; r < 2; r++) {
        for (c = 0; c < 3; c++) {
            uint8_t *p = &expect[16 + 16 * r + 4 * c];

            /* Host row r holds board row 1 - r. */
            p[0] = 2 - r;
            p[1] = c + 1;
            p[2] = 0x55;
            p[3] = 0xff;
        }
    }
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    board_quit(qts);
}

/*
 * A read whose host rows cross from one entry to the next: row 0 starts
 * eight bytes before the end of one 16 KiB page and finishes in a second,
 * non-adjacent one, then the gap, then row 1. The bytes past the first
 * page and around the second stay guards.
 */
static void test_readback_page_crossing(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        0x2000, (300 << 16) | 200, 0x50, (2 << 16) | 3, 0x8223305a, 4, 12,
        0xf301004c,
    };
    const uint64_t pte[] = {
        ODSY_DMA_PTE(RB_PAGE_A + 0x3ff8, 8), ODSY_DMA_PTE(RB_PAGE_B, 16),
    };
    uint8_t expect_a[32], expect_b[40];
    unsigned c, r;

    for (r = 0; r < 2; r++) {
        for (c = 0; c < 3; c++) {
            odsy_buffer_pixel_write(qts, 0, 200 + c, 300 + r,
                           (r + 1) << 24 | (c + 1) << 16 | 0x77ff);
        }
    }
    readback_guard(qts, RB_PAGE_A + 0x3ff0, expect_a, sizeof(expect_a));
    readback_guard(qts, RB_PAGE_B, expect_b, sizeof(expect_b));
    readback_run(qts, hdr, pte, 2);

    /*
     * An address read with bit 30 set: host row 0 is board row 1. The 0x4c
     * layout is the bytes A, B, G, R a pixel, alpha zero.
     */
    {
        uint8_t row0[12], row1[12];

        for (c = 0; c < 3; c++) {
            row0[4 * c] = 0;
            row0[4 * c + 1] = 0x77;
            row0[4 * c + 2] = c + 1;
            row0[4 * c + 3] = 2;
            row1[4 * c] = 0;
            row1[4 * c + 1] = 0x77;
            row1[4 * c + 2] = c + 1;
            row1[4 * c + 3] = 1;
        }
        memcpy(&expect_a[8], row0, 8);
        memcpy(&expect_b[0], row0 + 8, 4);
        memcpy(&expect_b[8], row1, 12);
    }
    readback_expect(qts, RB_PAGE_A + 0x3ff0, expect_a, sizeof(expect_a));
    readback_expect(qts, RB_PAGE_B, expect_b, sizeof(expect_b));
    g_assert_true(readback_dma_done(qts));
    board_quit(qts);
}

/*
 * The login greeter's first kernel-port transfer: the X server reads the
 * 6x20 caret cell's colour indices into a 12-byte-stride buffer. 120 index
 * bytes land across the gaps, DMA-done is raised, and the plane keeps what
 * it held: the host buffer's own bytes (all guards here, colour index 0xa5)
 * are no longer painted onto it.
 */
/*
 * An address read of an off-screen object: an upload creates the object at
 * 0x16a000 (256 pixels a row by word 2), and a read of the same rectangle
 * returns its bytes at the header's own coordinates -- no window origin is
 * added, although one is set -- without creating another object. A read
 * whose x + w passes the object's width, or whose rows pass those the
 * object has stored, is refused and leaves the guards.
 */
static void test_readback_object_source(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        0x16a000, (1 << 16) | 5, 0x10, (2 << 16) | 3, 0x80025000, 0, 12,
        0x3201004c,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 24) };
    uint8_t px[24], expect[56];
    unsigned i;

    for (i = 0; i < 6; i++) {
        stl_be_p(&px[4 * i], ODSY_X_PIXEL(0x102030 + i * 0x010101));
    }
    qtest_memwrite(qts, RB_PAGE_A + 16, px, sizeof(px));
    readback_run(qts, hdr, pte, 1);
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 1);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a000, 5, 1), ==,
                    0x102030ffu);

    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    hdr[7] = 0xb301004c;
    readback_run(qts, hdr, pte, 1);
    memcpy(&expect[16], px, sizeof(px));
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_true(readback_dma_done(qts));
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 1);

    /* Past the object's 256-pixel width: refused. */
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    hdr[1] = (1 << 16) | 254;
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* Rows 2 and 3, but only rows 0 to 2 are stored: refused. */
    hdr[1] = (2 << 16) | 5;
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 1);
    board_quit(qts);
}

/* A V6 GL read of one RGBA8 texel uses class 2, code 8 and no format byte. */
static void test_readback_gl_rgba8_object(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        0x16bc00, 0, 0x20, 0x10001, 0x80013042, 0, 4, 0x3201004b,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    const uint8_t texel[] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t expect[36];

    qtest_memwrite(qts, RB_PAGE_A + 16, texel, sizeof(texel));
    readback_run(qts, hdr, pte, 1);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16bc00, 0, 0), ==,
                    0x11223344);

    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    hdr[7] = 0xb3010000;
    readback_run(qts, hdr, pte, 1);
    memcpy(&expect[16], texel, sizeof(texel));
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_true(readback_dma_done(qts));
    board_quit(qts);
}

/* A native RGBA/UNSIGNED_SHORT read expands each stored channel to 16 bits. */
static void test_readback_gl_rgba16_object(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        0x16a800, 0, 0x20, 0x10001, 0x8002504b, 0, 4, 0x3201004b,
    };
    const uint64_t upload_pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    const uint64_t read_pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 8) };
    static const struct {
        uint8_t rgba[4];
        uint8_t want[8];
    } cases[] = {
        { { 0xff, 0x00, 0xff, 0xff },
          { 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff } },
        { { 0x12, 0x34, 0x56, 0x80 },
          { 0x12, 0x12, 0x34, 0x34, 0x56, 0x56, 0x80, 0x80 } },
    };
    uint8_t expect[40];

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        qtest_memwrite(qts, RB_PAGE_A + 16, cases[i].rgba, 4);
        readback_run(qts, hdr, upload_pte, 1);
        readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
        hdr[6] = 8;
        hdr[7] = 0xb3020000;
        readback_run(qts, hdr, read_pte, 1);
        memcpy(&expect[16], cases[i].want, 8);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
        g_assert_true(readback_dma_done(qts));
        hdr[6] = 4;
        hdr[7] = 0x3201004b;
    }
    board_quit(qts);
}

/* The same V12 copy reads the selected front or back drawable. */
static void test_readback_gl_rgba16_scratch_copy(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        0x16a800, 0, 0x20, 0x10001, 0x8002504b, 0, 8, 0xb3020000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 8) };
    const uint8_t magenta16[8] = {
        0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff,
    };
    uint8_t expect[40];

    cmd_push64(qts, 0x60b, (RB_ORIGIN_Y << 16) | RB_ORIGIN_X);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x16a800);
    user_cmd_push(qts, 0x20);
    user_cmd_push(qts, 0x080024d0);
    user_cmd_push(qts, 0);
    for (unsigned front = 0; front < 2; front++) {
        odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64,
                                RB_ORIGIN_Y + 63,
                                front ? 0xff00ffffu : 0x00ff00ffu);
        odsy_buffer_pixel_write(qts, 1, RB_ORIGIN_X + 64,
                                RB_ORIGIN_Y + 63,
                                front ? 0x00ff00ffu : 0xff00ffffu);
        cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
        cmd_push(qts, RB_READ_SENTINEL);
        cmd_push(qts, 0x003f0040);
        cmd_push(qts, 0x80200050);
        cmd_push(qts, 0x00010001);
        cmd_push(qts, front ? 0x92633042 : 0x12633042);
        cmd_push(qts, 0x16a800);
        cmd_push(qts, 0);
        cmd_push(qts, 0x3802204b);
        g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 0), ==,
                        0xff00ffffu);
        readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
        readback_run(qts, hdr, pte, 1);
        memcpy(&expect[16], magenta16, sizeof(magenta16));
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    }
    board_quit(qts);
}

/* Captured V12 copies fill one 128x64 object chunk before each short read. */
static void test_readback_gl_rgba16_scratch_copy_chunks(void)
{
    QTestState *qts = readback_start("");
    const uint32_t sample_x[] = { 0, 17, 64, 127 };
    const uint32_t sample_y[] = { 0, 23, 32, 63 };
    const uint32_t chunk_y[] = { 64, 0, 64, 0 };
    const uint32_t unsupported_tails[] = { 0xf802204a, 0xb802204b };
    const uint64_t pte[] = {
        ODSY_DMA_PTE(RB_PAGE_A, 0x8000),
        ODSY_DMA_PTE(RB_PAGE_A + 0x8000, 0x8000),
    };
    const size_t payload = 128 * 64 * 8;
    uint32_t hdr[8] = {
        0x16a800, 0, 0x20, (64 << 16) | 128, 0x8002504b, 0, 0x400,
        0xb3020000,
    };
    g_autofree uint8_t *expect = g_malloc(payload + 32);
    uint32_t colours[ARRAY_SIZE(sample_x)];

    cmd_push64(qts, 0x60b, (RB_ORIGIN_Y << 16) | RB_ORIGIN_X);
    window_origin(qts, 17, 29);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x16a800);
    user_cmd_push(qts, 0x20);
    user_cmd_push(qts, 0x080024d0);
    user_cmd_push(qts, 0);

    for (unsigned chunk = 0; chunk < ARRAY_SIZE(chunk_y); chunk++) {
        uint32_t copy_src = chunk_y[chunk] << 16;

        for (unsigned i = 0; i < ARRAY_SIZE(sample_x); i++) {
            uint32_t x = RB_ORIGIN_X + sample_x[i];
            uint32_t y = RB_ORIGIN_Y + chunk_y[chunk] + sample_y[i];

            colours[i] = 0x10203040 + chunk * 0x10101000 + i * 0x01020304;
            odsy_buffer_pixel_write(qts, 0, x, y, colours[i] ^ 0x00ffffff);
            odsy_buffer_pixel_write(qts, 1, x, y, colours[i]);
        }

        cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
        cmd_push(qts, RB_READ_SENTINEL);
        cmd_push(qts, copy_src);
        cmd_push(qts, 0x80200050);
        cmd_push(qts, (64 << 16) | 128);
        cmd_push(qts, 0x12633042);
        cmd_push(qts, 0x16a800);
        cmd_push(qts, 0);
        cmd_push(qts, 0xf802204b);

        g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==,
                         1);
        for (unsigned i = 0; i < ARRAY_SIZE(sample_x); i++) {
            g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800,
                                                   sample_x[i], sample_y[i]),
                            ==, colours[i]);
        }

        readback_guard(qts, RB_PAGE_A - 16, expect, payload + 32);
        memset(expect + 16, 0, payload);
        for (unsigned i = 0; i < ARRAY_SIZE(sample_x); i++) {
            size_t at = 16 + sample_y[i] * 1024 + sample_x[i] * 8;

            for (unsigned channel = 0; channel < 4; channel++) {
                uint8_t byte =
                    (uint8_t)(colours[i] >> (24 - channel * 8));

                expect[at + channel * 2] = byte;
                expect[at + channel * 2 + 1] = byte;
            }
        }
        readback_run(qts, hdr, pte, ARRAY_SIZE(pte));
        readback_expect(qts, RB_PAGE_A - 16, expect, payload + 32);
        g_assert_true(readback_dma_done(qts));
    }

    odsy_buffer_pixel_write(qts, 1, RB_ORIGIN_X,
                            RB_ORIGIN_Y + 128, 0x76543210);
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, RB_READ_SENTINEL);
    cmd_push(qts, 128 << 16);
    cmd_push(qts, 0x80200050);
    cmd_push(qts, 0x00010001);
    cmd_push(qts, 0x12633042);
    cmd_push(qts, 0x16a800);
    cmd_push(qts, 511);
    cmd_push(qts, 0xf802204b);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 511, 0), ==,
                    0x76543210);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 127, 63), ==,
                    colours[3]);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 64), ==, 0);

    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, RB_READ_SENTINEL);
    cmd_push(qts, 128 << 16);
    cmd_push(qts, 0x80200050);
    cmd_push(qts, 0x00010001);
    cmd_push(qts, 0x12633042);
    cmd_push(qts, 0x16a800);
    cmd_push(qts, 512);
    cmd_push(qts, 0xf802204b);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 512, 0), ==, 0);

    for (unsigned i = 0; i < ARRAY_SIZE(unsupported_tails); i++) {
        uint32_t tail = unsupported_tails[i];

        cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
        cmd_push(qts, RB_READ_SENTINEL);
        cmd_push(qts, 128 << 16);
        cmd_push(qts, 0x80200050);
        cmd_push(qts, 0x00010001);
        cmd_push(qts, 0x12633042);
        cmd_push(qts, 0x16a800);
        cmd_push(qts, 510 - i);
        cmd_push(qts, tail);
    }
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 510, 0), ==, 0);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 509, 0), ==, 0);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 511, 0), ==,
                    0x76543210);
    board_quit(qts);
}

/* The V6 scratch read follows a drawable-to-object copy, then a DMA read. */
static void test_readback_gl_rgba8_scratch_copy(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        0x16bc00, 0, 0x20, 0x10002, 0x80013042, 0, 8, 0xb3010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 8) };
    const uint64_t captured_pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };
    const uint32_t colours[] = { 0x12345678, 0x90abcdef };
    const uint32_t captured_hdr[8] = {
        0x16a800, 0, 0x20, 0x10001, 0x80013042, 0, 4, 0xb3010000,
    };
    uint8_t expect[40];

    cmd_push64(qts, 0x60b, (RB_ORIGIN_Y << 16) | RB_ORIGIN_X);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x16bc00);
    user_cmd_push(qts, 0x20);
    user_cmd_push(qts, 0x080024d0);
    user_cmd_push(qts, 0);
    window_origin(qts, 0, 0);
    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                            colours[0]);
    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 65, RB_ORIGIN_Y + 63,
                            colours[1]);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    for (unsigned i = 0; i < ARRAY_SIZE(colours); i++) {
        cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
        cmd_push(qts, RB_READ_SENTINEL);
        cmd_push(qts, 0x003f0040 + i);
        cmd_push(qts, 0x80200050);
        cmd_push(qts, 0x00010001);
        cmd_push(qts, 0x92233002);
        cmd_push(qts, 0x16bc00);
        cmd_push(qts, i);
        cmd_push(qts, 0x3801210b);
    }
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16bc00, 0, 0), ==,
                    colours[0]);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16bc00, 1, 0), ==,
                    colours[1]);
    readback_run(qts, hdr, pte, 1);
    stl_be_p(&expect[16], colours[0]);
    stl_be_p(&expect[20], colours[1]);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    hdr[1] = 511;
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* The captured read origin plus source offset selects (320,319). */
    cmd_push64(qts, 0x60b, 0x01000100);
    user_cmd_push(qts, ODSY_CMD_DEST_SELECT);
    user_cmd_push(qts, 0x16a800);
    user_cmd_push(qts, 0x20);
    user_cmd_push(qts, 0x080024d0);
    user_cmd_push(qts, 0);
    odsy_buffer_pixel_write(qts, 0, 320, 319, 0x2468ace0);
    odsy_buffer_pixel_write(qts, 1, 320, 319, 0x2468ace0);

    /* Accepted RGBA8 control: inspect the object before reading it back. */
    cmd_push(qts, ODSY_CMD_COPY_RECT_TRIGGER);
    cmd_push(qts, RB_READ_SENTINEL);
    cmd_push(qts, 0x003f0040);
    cmd_push(qts, 0x80200050);
    cmd_push(qts, 0x00010001);
    cmd_push(qts, 0x92233002);
    cmd_push(qts, 0x16a800);
    cmd_push(qts, 0);
    cmd_push(qts, 0x3801210b);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16a800, 0, 0), ==,
                    0x2468ace0);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, captured_hdr, captured_pte, 1);
    stl_be_p(&expect[16], 0x2468ace0);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    qtest_quit(qts);
}

/*
 * An X index read (format byte 0x41) takes the plane word as an index only
 * where scanout does: the pixel's drawable (main id 0 here) is in an index
 * group and the word fits eight bits.
 * The same word 0x000000ff is refused under an RGB mode and read as index
 * 0xff under group 17; a 12-bit index 0x1ff under group 0 is refused. An
 * index read of a board-memory object is refused: objects record no
 * display mode. Each refusal leaves the guards and is reported once.
 */
static void test_readback_index_follows_display_mode(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    uint32_t hdr[8] = {
        0x2000, (400 << 16) | 300, 0x50, 0x10001, 0x83331008, 0, 1,
        0xb3050041,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 1) };
    uint8_t expect[33];
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-readback-index-XXXXXX.log", &log_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp,guest_errors -D %s", quoted);
    qts = readback_start(extra);

    odsy_buffer_pixel_write(qts, 0, 300, 400, 0x000000ff);

    /* An RGB mode: the word is a colour, so no index byte is read. */
    dispmode_write(qts, 0, ODSY_DISPMODE_RGB);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* An index group: the same word reads as index 0xff. */
    dispmode_write(qts, 0, ODSY_DISPMODE_GROUP17);
    readback_run(qts, hdr, pte, 1);
    expect[16] = 0xff;
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* Group 0's 12-bit index above 0xff does not fit the byte. */
    dispmode_write(qts, 0, 0);
    odsy_buffer_pixel_write(qts, 0, 300, 400, 0x1ff);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* An object: no display mode, so no index read. */
    {
        uint32_t up[8] = {
            0x16a000, 0, 0x10, 0x10001, 0x83331008, 0, 1, 0x32050041,
        };
        uint8_t one = 7;

        qtest_memwrite(qts, RB_PAGE_A + 16, &one, 1);
        readback_run(qts, up, pte, 1);
        g_assert_cmphex(odsy_test_object_texel(qts, 0x16a000, 0, 0), ==, 7);
        readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
        up[7] = 0xb3050041;
        readback_run(qts, up, pte, 1);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    }
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(readback_count(log, "not an eight-bit index"), ==, 1);
    g_assert_cmpuint(readback_count(log, "in display-mode group 26"), ==, 1);
    g_assert_cmpuint(readback_count(log, "records no display mode"), ==, 1);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

static void test_readback_greeter_caret_cell(void)
{
    QTestState *qts = readback_start("");
    const uint32_t hdr[8] = {
        0x2000, (700 << 16) | 454, 0x50, (20 << 16) | 6, 0x83331008, 6, 6,
        0xb3050041,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 0x100, 120) };
    uint8_t expect[19 * 12 + 6 + 32];
    unsigned c, r;

    for (r = 0; r < 20; r++) {
        for (c = 0; c < 6; c++) {
            odsy_buffer_pixel_write(qts, 0, 454 + c, 700 + r,
                                    1 + (r * 6 + c) % 200);
        }
    }
    readback_guard(qts, RB_PAGE_A + 0xf0, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    for (r = 0; r < 20; r++) {
        for (c = 0; c < 6; c++) {
            expect[16 + 12 * r + c] = 1 + (r * 6 + c) % 200;
        }
    }
    readback_expect(qts, RB_PAGE_A + 0xf0, expect, sizeof(expect));
    g_assert_true(readback_dma_done(qts));
    for (r = 0; r < 20; r++) {
        for (c = 0; c < 6; c++) {
            g_assert_cmphex(odsy_test_pixel(qts, 454 + c, 700 + r), ==,
                            1 + (r * 6 + c) % 200);
        }
    }
    board_quit(qts);
}

/*
 * The other GL transfer codes for a 12-bit RGBA window: 6 writes A, B, G,
 * R; 2 writes R, G, B and 7 B, G, R, each three-byte pair under word 7's
 * three-byte size code.
 */
static void test_readback_gl_layouts(void)
{
    QTestState *qts = readback_start("");
    static const struct {
        uint32_t fmt, control, bytes;
        uint8_t want[4];
    } cases[] = {
        { 0x90035033, 0xf3010000, 4, { 0x44, 0x33, 0x22, 0x11 } },
        { 0x90035013, 0xf3070000, 3, { 0x11, 0x22, 0x33 } },
        { 0x9003503b, 0xf3070000, 3, { 0x33, 0x22, 0x11 } },
    };
    unsigned i;

    odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                            0x11223344);
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        const uint32_t hdr[8] = {
            RB_READ_SENTINEL, 0x003f0040, 0x50, 0x10001, cases[i].fmt, 0,
            cases[i].bytes, cases[i].control,
        };
        const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, cases[i].bytes) };
        uint8_t expect[36];

        readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
        readback_run(qts, hdr, pte, 1);
        memcpy(&expect[16], cases[i].want, cases[i].bytes);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    }
    board_quit(qts);
}

/*
 * Every refusal writes nothing to the guest, creates no board-memory
 * object, still raises DMA-done when bit 31 asks for it (no completion
 * record carries a status, and the kernel's own fence ends its wait
 * anyway), and is reported once however often it recurs. The draw sentinel
 * is read like the read sentinel and reported once as never captured; an
 * upload naming a sentinel is reported and not performed.
 */
static void test_readback_failures(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *log_path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    static const struct {
        uint32_t base, origin, size, fmt, rowbytes, control, entry;
        const char *report;
    } cases[] = {
        /* A bit-29 word 0 other than the two sentinels. */
        { 0x30000018, 0x003f0040, 0x10001, RB_GL_FRONT_RGBA, 4, 0xf3010000,
          4, "DMA read of sentinel 0x30000018" },
        /* The read sentinel without its word 4 bits 30:28 = 1. */
        { RB_READ_SENTINEL, 0x003f0040, 0x10001, 0x8003500b, 4, 0xf3010000,
          4, NULL },
        /* The draw sentinel with the read selector. */
        { RB_DRAW_SENTINEL, 0x003f0040, 0x10001, RB_GL_FRONT_RGBA, 4,
          0xf3010000, 4, NULL },
        /* GL code 9 needs eight bytes, so a four-byte read is refused. */
        { RB_READ_SENTINEL, 0x003f0040, 0x10001, 0x9003504b, 4, 0xf3010000,
          4, "DMA read with format byte 0x0, word 4 0x9003504b" },
        /* A GL source class other than 3. */
        { RB_READ_SENTINEL, 0x003f0040, 0x10001, 0x9003500a, 4, 0xf3010000,
          4, NULL },
        /* Code 1's four bytes under the three-byte size code. */
        { RB_READ_SENTINEL, 0x003f0040, 0x10001, RB_GL_FRONT_RGBA, 3,
          0xf3070000, 3, NULL },
        /* The X luminance byte, which no read inverts. */
        { 0x2000, (300 << 16) | 200, 0x10001, 0x83331008, 1, 0xb3050042, 1,
          NULL },
        /* Entries holding more than the rectangle. */
        { RB_READ_SENTINEL, 0x003f0040, 0x10001, RB_GL_FRONT_RGBA, 4,
          0xf3010000, 8, "disagrees with itself" },
        /* A rectangle past the plane's right edge. */
        { RB_READ_SENTINEL, (0 << 16) | 767, 0x10002, RB_GL_FRONT_RGBA, 8,
          0xf3010000, 8, "leaves its source" },
        /* A board base that holds no buffer. */
        { 0x16a000, 0, 0x10001, 0x80025000, 4, 0xf301004c, 4,
          "which holds no buffer" },
        /* An index read over a pixel holding a colour word. */
        { 0x2000, (310 << 16) | 200, 0x10001, 0x83331008, 1, 0xb3050041, 1,
          "finds word 0x123456ff" },
    };
    uint8_t expect[36];
    unsigned i, pass;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-readback-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp,guest_errors -D %s", quoted);
    qts = readback_start(extra);

    odsy_buffer_pixel_write(qts, 0, 200, 310, 0x123456ffu);
    /* Twice over: the second pass must add no report. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < ARRAY_SIZE(cases); i++) {
            const uint32_t hdr[8] = {
                cases[i].base, cases[i].origin, 0x50, cases[i].size,
                cases[i].fmt, 0, cases[i].rowbytes, cases[i].control,
            };
            const uint64_t pte[] = {
                ODSY_DMA_PTE(RB_PAGE_A + 16, cases[i].entry),
            };

            readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
            readback_mark(qts);
            readback_run(qts, hdr, pte, 1);
            readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
            g_assert_true(readback_dma_done(qts));
            g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"),
                             ==, 0);
        }
    }
    g_assert_cmphex(odsy_test_pixel(qts, 200, 310), ==, 0x123456ffu);

    /* The draw sentinel with its own selector reads the window, reported. */
    {
        const uint32_t hdr[8] = {
            RB_DRAW_SENTINEL, 0x003f0040, 0x50, 0x10001, 0x8003500b, 0, 4,
            0xf3010000,
        };
        const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };

        odsy_buffer_pixel_write(qts, 0, RB_ORIGIN_X + 64, RB_ORIGIN_Y + 63,
                                0x01020304);
        readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
        readback_run(qts, hdr, pte, 1);
        readback_run(qts, hdr, pte, 1);
        expect[16] = 1;
        expect[17] = 2;
        expect[18] = 3;
        expect[19] = 4;
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    }

    /* An upload naming the draw sentinel: reported, nothing painted. */
    {
        const uint32_t hdr[8] = {
            RB_DRAW_SENTINEL, 0x003f0040, 0x50, 0x10001, 0x8003500b, 0, 4,
            0x3a01004c,
        };
        const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 4) };

        readback_run(qts, hdr, pte, 1);
        g_assert_cmphex(odsy_test_pixel(qts, RB_ORIGIN_X + 64,
                                        RB_ORIGIN_Y + 63), ==, 0x01020304);
        g_assert_cmpuint(odsy_qom_get_uint(qts, "x-test-object-count"), ==, 0);
    }
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        if (cases[i].report) {
            g_assert_cmpuint(readback_count(log, cases[i].report), ==, 1);
        }
    }
    g_assert_cmpuint(readback_count(log, "DMA read of sentinel"), ==, 1);
    g_assert_cmpuint(readback_count(log, "DMA read with format byte"), ==, 1);
    g_assert_cmpuint(readback_count(log, "DMA read of the draw drawable"), ==,
                     1);
    g_assert_cmpuint(readback_count(log, "DMA upload to sentinel 0x30000010"),
                     ==, 1);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The display back end's hardware cursor (the datasheet's
 * "Hardware cursor"; hw/misc/sgi-odyssey.c). The test glyph's top row is
 * values 1, 2, 0, 1 in columns 0 to 3, and its plane is 3 under columns 0 to
 * 2 and 0 from column 3 on, so along that row the cursor shows colour word
 * 1, word 2, word 0, then nothing over a glyph value of 1. Word 1 holds a
 * ten-bit green of 0x2c4, which shows as its top eight bits, 0xb1; its low
 * eight bits, 0xc4, would tell a wrong truncation apart.
 */
#define ODSY_CURSOR_GLYPH 0x2500
#define ODSY_CURSOR_POSITION 0x2580
#define ODSY_CURSOR_CONTROL 0x2581
#define ODSY_CURSOR_COLOURS 0x2590
#define ODSY_CURSOR_PLANE 0x25c0
#define ODSY_CURSOR_WORD0_RGB 0xff0000u
#define ODSY_CURSOR_WORD1_RGB 0x00b100u
#define ODSY_CURSOR_WORD2_RGB 0x0000ffu
#define ODSY_CURSOR_BACKGROUND 0x406080ffu

static void cursor_entry(QTestState *qts, uint32_t index, uint32_t word)
{
    backend_push32(qts, ODSY_BACKEND_HDR(index, 1));
    backend_push32(qts, word);
}

/* What a cursor entry reads back at the back end's read window. */
static uint32_t cursor_readback(QTestState *qts, uint32_t index)
{
    return qtest_readl(qts, WIDGET + 0x220000 + ((uint64_t)index << 3));
}

/*
 * An RGB screen with a known colour over the corner the cursor uses, the
 * test glyph, plane and colours, and the cursor at (x, y), still off.
 */
static void cursor_load(QTestState *qts, uint32_t x, uint32_t y)
{
    display_identity(qts, 0);
    for (uint32_t py = 0; py < 8; py++) {
        for (uint32_t px = 0; px < 12; px++) {
            odsy_test_pixel_write(qts, px, py, ODSY_CURSOR_BACKGROUND);
        }
    }
    backend_push32(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_GLYPH, 128));
    for (unsigned i = 0; i < 128; i++) {
        backend_push32(qts, i == 0 ? 0x1021 : 0);
    }
    backend_push32(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_PLANE, 64));
    for (unsigned i = 0; i < 64; i++) {
        backend_push32(qts, i == 0 ? 0x3f : 0);
    }
    backend_push32(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_COLOURS, 15));
    backend_push32(qts, 0x3ffu << 20);
    backend_push32(qts, 0x2c4u << 10);
    backend_push32(qts, 0x3ffu);
    for (unsigned i = 3; i < 15; i++) {
        backend_push32(qts, 0);
    }
    cursor_entry(qts, ODSY_CURSOR_POSITION, (x & 0xffff) << 16 | (y & 0xffff));
}

/* The background as it shows, before any cursor is drawn over it. */
static uint32_t cursor_background(QTestState *qts)
{
    return odsy_test_display_pixel(qts, 11, 7);
}

/*
 * Plane 3 shows glyph value k in colour word k and glyph 0 in word 0; plane
 * 0 is transparent whatever the glyph, the model's reading 1. The cursor is
 * drawn after colour resolution, and every entry reads back as written.
 */
static void test_cursor_glyph_and_plane(void)
{
    QTestState *qts = board_start("");
    uint32_t bg;

    cursor_load(qts, 2, 3);
    bg = cursor_background(qts);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==, bg);

    cursor_entry(qts, ODSY_CURSOR_CONTROL, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 3, 3), ==,
                    ODSY_CURSOR_WORD2_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 4, 3), ==,
                    ODSY_CURSOR_WORD0_RGB);
    /* Glyph 1 under plane 0, then glyph 0 under plane 0. */
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 6, 3), ==, bg);
    /* Row 1 of the glyph and plane is empty, and so is the row above. */
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 4), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 2), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 1, 3), ==, bg);

    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_GLYPH), ==, 0x1021);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_PLANE), ==, 0x3f);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_COLOURS + 1), ==,
                    0x2c4u << 10);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_POSITION), ==,
                    0x00020003);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_CONTROL), ==, 1);

    /* Clearing bit 0 restores the picture. */
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 0);
    for (uint32_t x = 2; x < 7; x++) {
        g_assert_cmphex(odsy_test_display_pixel(qts, x, 3), ==, bg);
    }
    board_quit(qts);
}

/*
 * The position is the glyph's top-left pixel, read as signed 16-bit
 * components and clipped to the raster. A move repaints the old place.
 */
static void test_cursor_moves_and_clips(void)
{
    QTestState *qts = board_start("");
    uint32_t bg;

    cursor_load(qts, 2, 3);
    bg = cursor_background(qts);
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);

    /* x = -1: column 0 of the glyph is off the left edge. */
    cursor_entry(qts, ODSY_CURSOR_POSITION, 0xffff0005);
    g_assert_cmphex(odsy_test_display_pixel(qts, 0, 5), ==,
                    ODSY_CURSOR_WORD2_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 1, 5), ==,
                    ODSY_CURSOR_WORD0_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 5), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 3, 3), ==, bg);

    /*
     * y = -2: glyph row 2 lands on display line 0. Read unsigned, 0xfffe
     * would put the whole cursor below the raster and draw nothing.
     */
    cursor_entry(qts, ODSY_CURSOR_GLYPH + 2 * 4, 0x1);
    cursor_entry(qts, ODSY_CURSOR_PLANE + 2 * 2, 0x3);
    cursor_entry(qts, ODSY_CURSOR_POSITION, 0x0002fffe);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 0), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 3, 0), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 1), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 0, 5), ==, bg);

    /*
     * The PROM's first position after its enable, x 1280 y 0: the first
     * column right of the raster, which draws nothing.
     */
    cursor_entry(qts, ODSY_CURSOR_POSITION, 0x05000000);
    g_assert_cmphex(odsy_test_display_pixel(qts, 0, 5), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 1, 5), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 0), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 1279, 0), ==, 0);
    board_quit(qts);
}

/*
 * Control word 5, bits 0 and 2, is the X server's crosshair: the model's
 * approximation is a one-pixel line across the raster each way through the
 * position, in colour word 1. Word 4, bit 2 alone, draws nothing.
 */
static void test_cursor_crosshair(void)
{
    QTestState *qts = board_start("");
    uint32_t bg;

    cursor_load(qts, 2, 3);
    bg = cursor_background(qts);

    cursor_entry(qts, ODSY_CURSOR_CONTROL, 4);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 9, 3), ==, bg);

    cursor_entry(qts, ODSY_CURSOR_CONTROL, 5);
    g_assert_cmphex(odsy_test_display_pixel(qts, 0, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 9, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 1279, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 0), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 1023), ==,
                    ODSY_CURSOR_WORD1_RGB);
    /* No glyph: the glyph's word 2 pixel beside the point stays clear. */
    g_assert_cmphex(odsy_test_display_pixel(qts, 3, 4), ==, bg);

    /* On the second display channel the crosshair draws nothing either. */
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 0x25);
    g_assert_cmphex(odsy_test_display_pixel(qts, 9, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 6), ==, bg);

    cursor_entry(qts, ODSY_CURSOR_CONTROL, 4);
    g_assert_cmphex(odsy_test_display_pixel(qts, 9, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 6), ==, bg);
    board_quit(qts);
}

/*
 * The cursor is drawn over a shown overlay pixel too, and where it is
 * transparent the overlay still shows.
 */
static void test_cursor_over_overlay(void)
{
    QTestState *qts = board_start("");

    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    lut_write(qts, 0x1105, ODSY_LUT_RED);
    id_plane_write(qts, 1, 0xf8, 100, 100, 140, 140);
    colour_index_fill(qts, 5, 100, 100, 140, 140);
    aux_write(qts, 5, 0x07, 0x3, 100, 100, 140, 140);
    aux_write(qts, 0x30, 0xff, 0xc, 100, 100, 120, 140);
    lut_write(qts, 0x2925, 0xb1);
    lut_write(qts, 0x1630, ODSY_LUT_BLUE);
    g_assert_cmphex(odsy_shown(qts, 110, 110), ==, 0x0000ffffu);

    cursor_load(qts, 110, 110);
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 110, 110), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 112, 110), ==,
                    ODSY_CURSOR_WORD0_RGB);
    /* Column 4 is under plane 0: the overlay's blue shows. */
    g_assert_cmphex(odsy_test_display_pixel(qts, 114, 110), ==, 0x0000ff);
    board_quit(qts);
}

/* Reset clears every entry, so the cursor is off. */
static void test_cursor_reset_leaves_it_off(void)
{
    QTestState *qts = board_start("");

    cursor_load(qts, 2, 3);
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);

    qtest_system_reset(qts);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==, 0);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_CONTROL), ==, 0);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_POSITION), ==, 0);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_GLYPH), ==, 0);
    g_assert_cmphex(cursor_readback(qts, ODSY_CURSOR_COLOURS + 1), ==, 0);
    /* The glyph, plane and colours went too: turning it on draws nothing. */
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==, 0);
    board_quit(qts);
}

/*
 * Values with no known meaning are reported once each: glyph value 15,
 * plane values 1 and 2, the second display channel (bit 5, which draws
 * nothing), the crosshair, control bits outside 0-5 and 31:30, and an
 * entry no software writes. Glyph 15 and planes 1 and 2 draw nothing.
 */
static void test_cursor_unknowns_reported_once(void)
{
    g_autofree char *log_path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *log = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    uint32_t bg;
    int fd;

    fd = g_file_open_tmp("sn1-odyssey-cursor-XXXXXX.log", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d unimp -D %s", quoted);
    qts = board_start(extra);

    cursor_load(qts, 2, 3);
    bg = cursor_background(qts);
    for (int i = 0; i < 2; i++) {
        /* Glyph 15 under plane 3, and glyph 1 under planes 1 and 2. */
        cursor_entry(qts, ODSY_CURSOR_GLYPH, 0x111f);
        cursor_entry(qts, ODSY_CURSOR_PLANE, 0x9f);
        cursor_entry(qts, ODSY_CURSOR_CONTROL, 0x21);
        cursor_entry(qts, ODSY_CURSOR_CONTROL, 0x41);
        cursor_entry(qts, ODSY_CURSOR_CONTROL, 5);
        cursor_entry(qts, 0x25a0, 0x1234);
    }
    g_assert_cmphex(cursor_readback(qts, 0x25a0), ==, 0x1234);

    cursor_entry(qts, ODSY_CURSOR_CONTROL, 0x21);
    g_assert_cmphex(odsy_test_display_pixel(qts, 3, 3), ==, bg);
    cursor_entry(qts, ODSY_CURSOR_CONTROL, 0xc0000003);
    g_assert_cmphex(odsy_test_display_pixel(qts, 2, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 3, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(qts, 4, 3), ==, bg);
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 3), ==, bg);
    board_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(count_occurrences(log, "glyph value 15"), ==, 1);
    g_assert_cmpuint(count_occurrences(log, "plane values 1 and 2"), ==, 1);
    g_assert_cmpuint(count_occurrences(log, "second display channel"), ==, 1);
    g_assert_cmpuint(count_occurrences(log, "crosshair drawn"), ==, 1);
    g_assert_cmpuint(count_occurrences(log, "control bits outside"), ==, 1);
    g_assert_cmpuint(count_occurrences(log, "no known function"), ==, 1);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/* The cursor's entries migrate, and the destination draws it. */
static void test_cursor_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = board_start("");
    QTestState *to;
    int fd = g_file_open_tmp("odyssey-cursor-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    cursor_load(from, 2, 3);
    cursor_entry(from, ODSY_CURSOR_CONTROL, 1);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(to, "completed");

    g_assert_cmphex(odsy_test_display_pixel(to, 2, 3), ==,
                    ODSY_CURSOR_WORD1_RGB);
    g_assert_cmphex(odsy_test_display_pixel(to, 4, 3), ==,
                    ODSY_CURSOR_WORD0_RGB);
    g_assert_cmphex(cursor_readback(to, ODSY_CURSOR_POSITION), ==,
                    0x00020003);
    board_quit(to);
    board_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * Meson runs this binary once per group in sn1-odyssey-test-groups.txt,
 * naming the group's GLib paths with --group-path, so that its groups run
 * at once. Only the cases a group takes are registered. GLib's own -p
 * selects as well, but it reports each path as a separate TAP plan, which
 * Meson rejects. A path names one case or a suite whose cases directly
 * under it all join the group. Without --group-path, every case runs.
 */
static GPtrArray *group_paths;

static void take_group_paths(int *argc, char **argv)
{
    int kept = 1;

    for (int i = 1; i < *argc; i++) {
        if (g_str_equal(argv[i], "--group-path") && i + 1 < *argc) {
            if (!group_paths) {
                group_paths = g_ptr_array_new();
            }
            g_ptr_array_add(group_paths, argv[++i]);
        } else {
            argv[kept++] = argv[i];
        }
    }
    argv[kept] = NULL;
    *argc = kept;
}

static bool in_group(const char *path)
{
    g_autofree char *full = g_strdup_printf("/%s/%s", qtest_get_arch(),
                                            path);
    g_autofree char *suite = g_path_get_dirname(full);

    if (!group_paths) {
        return true;
    }
    for (guint i = 0; i < group_paths->len; i++) {
        const char *taken = g_ptr_array_index(group_paths, i);

        if (g_str_equal(taken, full) || g_str_equal(taken, suite)) {
            return true;
        }
    }
    return false;
}

static void add_case(const char *path, GTestFunc fn)
{
    if (in_group(path)) {
        qtest_add_data_func(path, fn, shared_case_run);
    }
}

static void add_fresh_case(const char *path, GTestFunc fn)
{
    if (in_group(path)) {
        qtest_add_func(path, fn);
    }
}

int main(int argc, char **argv)
{
    int result;

    take_group_paths(&argc, argv);
    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    add_case("sn1/odyssey/rgba/texture-alpha",
             test_rgba_texture_alpha);
    add_case("sn1/odyssey/rgba/disabled-colour-depth",
             test_rgba_disabled_colour_depth);
    add_case("sn1/odyssey/rgba/read-bounds",
             test_rgba_read_bounds);
    add_case("sn1/odyssey/rgba/clear-masks",
             test_rgba_clear_masks);
    add_case("sn1/odyssey/rgba/material-masks",
             test_rgba_material_masks);
    add_fresh_case("sn1/odyssey/rgba/pattern-origin-ownership",
                   test_rgba_pattern_origin_ownership);
    add_case("sn1/odyssey/rgba/pattern-float-rectangle",
             test_rgba_pattern_float_rectangle);
    add_fresh_case("sn1/odyssey/rgba/pattern-float-rectangle-migration",
                   test_rgba_pattern_float_rectangle_migration);
    add_case("sn1/odyssey/rgba/index-transitions",
             test_rgba_index_transitions);
    add_case("sn1/odyssey/rgba/four-float-alpha",
             test_rgba_four_float_alpha);
    add_fresh_case("sn1/odyssey/identity-carries-part-and-revision",
                   test_identity_carries_part_and_revision);
    add_fresh_case("sn1/odyssey/configuration-block-mirrors-both-halves",
                   test_configuration_block_mirrors_both_halves);
    add_fresh_case("sn1/odyssey/widget-control-reads-back",
                   test_widget_control_reads_back);
    add_fresh_case("sn1/odyssey/memory-configuration-follows-properties",
                   test_memory_configuration_follows_properties);
    add_fresh_case("sn1/odyssey/profile-selects-identity-and-memory",
                   test_profile_selects_identity_and_memory);
    add_fresh_case("sn1/odyssey/v12-profile-memory-word-bits",
                   test_v12_profile_memory_word_bits);
    add_fresh_case("sn1/odyssey/reset-control-accepts-six-writes",
                   test_reset_control_accepts_six_writes);
    add_fresh_case("sn1/odyssey/backend-version-is-pinned",
                   test_backend_version_is_pinned);
    add_fresh_case("sn1/odyssey/board-memory-bound-is-a-guest-error",
                   test_board_memory_bound_is_a_guest_error);
    add_fresh_case("sn1/odyssey/board-identity-part-answers",
                   test_board_identity_part_answers);
    add_fresh_case("sn1/odyssey/profile-names-its-board",
                   test_profile_names_its_board);
    add_fresh_case("sn1/odyssey/interrupt-registers",
                   test_interrupt_registers);
    add_fresh_case("sn1/odyssey/interrupt-enable-set-and-clear",
                   test_interrupt_enable_set_and_clear);
    add_fresh_case("sn1/odyssey/interrupt-clear-acknowledges-flags",
                   test_interrupt_clear_acknowledges_flags);
    add_fresh_case("sn1/odyssey/interrupt-flags-take-written-value",
                   test_interrupt_flags_take_written_value);
    add_fresh_case("sn1/odyssey/lowater-not-raised-while-disabled",
                   test_lowater_not_raised_while_disabled);
    add_fresh_case("sn1/odyssey/board-status-reports-fifo-ready",
                   test_board_status_reports_fifo_ready);
    add_fresh_case("sn1/odyssey/reset-clears-flag-level",
                   test_reset_clears_flag_level);
    add_fresh_case("sn1/odyssey/command-fifo-lowater-interrupt",
                   test_command_fifo_lowater_interrupt);
    add_fresh_case("sn1/odyssey/interrupt-packet-composition",
                   test_interrupt_packet_composition);
    add_fresh_case("sn1/odyssey/flag-interrupt-uses-routed-vector",
                   test_flag_interrupt_uses_routed_vector);
    add_fresh_case("sn1/odyssey/interrupt-packet-reaches-bedrock-pending",
                   test_interrupt_packet_reaches_bedrock_pending);
    add_fresh_case("sn1/odyssey/baseio-prom-diagnostic-registers",
                   test_baseio_prom_diagnostic_registers);
    add_fresh_case("sn1/odyssey/unmodeled-offsets-are-visible",
                   test_unmodeled_offsets_are_visible);
    add_case("sn1/odyssey/command-fifo-selftest",
             test_command_fifo_selftest);
    add_fresh_case("sn1/odyssey/reset-restores-documented-values",
                   test_reset_restores_the_documented_values);
    add_case("sn1/odyssey/i2c-opt-port-pll-register-write",
             test_i2c_opt_port_pll_register_write);
    add_case("sn1/odyssey/i2c-opt-port-ddc-register-read",
             test_i2c_opt_port_ddc_register_read);
    add_case("sn1/odyssey/i2c-reset-state-poll-terminates",
             test_i2c_reset_state_poll_terminates);
    add_case("sn1/odyssey/framebuffer-pixel-reaches-scanout",
             test_framebuffer_pixel_reaches_display_and_screendump);
    add_fresh_case("sn1/odyssey/display-channel-size-sets-scanout",
                   test_display_channel_size_sets_scanout);
    add_fresh_case("sn1/odyssey/reset-clears-the-framebuffer",
                   test_reset_clears_the_framebuffer);
    add_fresh_case("sn1/odyssey/reset-mid-primitive-does-not-hang",
                   test_reset_mid_primitive_does_not_hang);
    add_case("sn1/odyssey/framebuffer-pixel-accessor-bounds",
             test_framebuffer_pixel_accessor_bounds);
    add_case("sn1/odyssey/scanout-wipe-repaint-reload",
             test_scanout_wipe_repaint_reload);
    add_case("sn1/odyssey/scanout-display-mode-recolours",
             test_scanout_display_mode_recolours);
    add_case("sn1/odyssey/scanout-rgb-through-gamma",
             test_scanout_rgb_through_gamma);
    add_case("sn1/odyssey/scanout-rgb-narrow-visuals",
             test_scanout_rgb_narrow_visuals);
    add_case("sn1/odyssey/scanout-paint-order",
             test_scanout_paint_order);
    add_case("sn1/odyssey/cursor/glyph-and-plane",
             test_cursor_glyph_and_plane);
    add_case("sn1/odyssey/cursor/moves-and-clips",
             test_cursor_moves_and_clips);
    add_case("sn1/odyssey/cursor/crosshair", test_cursor_crosshair);
    add_case("sn1/odyssey/cursor/over-overlay", test_cursor_over_overlay);
    add_case("sn1/odyssey/cursor/reset-leaves-it-off",
             test_cursor_reset_leaves_it_off);
    add_fresh_case("sn1/odyssey/cursor/unknowns-reported-once",
                   test_cursor_unknowns_reported_once);
    add_fresh_case("sn1/odyssey/cursor/migration", test_cursor_migration);
    add_case("sn1/odyssey/command-fifo-fillrect",
             test_command_fifo_fillrect);
    add_case("sn1/odyssey/backend-colormap-word-stream",
             test_backend_colormap_word_stream);
    add_case("sn1/odyssey/command-fifo-id-plane-write",
             test_command_fifo_id_plane_write);
    add_case("sn1/odyssey/command-fifo-accepts-64bit-stores",
             test_command_fifo_accepts_64bit_stores);
    add_case("sn1/odyssey/command-fifo-copyrect",
             test_command_fifo_copyrect);
    add_case("sn1/odyssey/command-fifo-copyrect-overlapping-shift",
             test_command_fifo_copyrect_overlapping_shift);
    add_case("sn1/odyssey/command-fifo-copyrect-large-offscreen",
             test_command_fifo_copyrect_large_offscreen);
    add_case("sn1/odyssey/command-fifo-copyrect-ignores-window-clip",
             test_command_fifo_copyrect_ignores_window_clip);
    add_fresh_case("sn1/odyssey/command-fifo-copyrect-prefix-is-a-register",
                   test_command_fifo_copyrect_prefix_is_a_plain_register);
    add_fresh_case("sn1/odyssey/command-fifo-host-write-sync",
                   test_command_fifo_host_write_sync);
    add_fresh_case("sn1/odyssey/command-fifo-dma-trace",
                   test_command_fifo_dma_trace);
    add_fresh_case("sn1/odyssey/command-fifo-dma-page-table",
                   test_command_fifo_dma_page_table);
    add_fresh_case("sn1/odyssey/command-fifo-dma-image",
                   test_command_fifo_dma_image);
    add_fresh_case("sn1/odyssey/readback/direction", test_readback_direction);
    add_fresh_case("sn1/odyssey/readback/raw-texture-paging",
                   test_readback_raw_texture_paging);
    add_fresh_case("sn1/odyssey/readback/put-abgr-x-image",
                   test_readback_put_abgr_x_image);
    add_fresh_case("sn1/odyssey/dma/draw-sentinel-short",
                   test_draw_sentinel_short_upload);
    add_fresh_case("sn1/odyssey/dma/draw-sentinel-abgr",
                   test_draw_sentinel_abgr_upload);
    add_fresh_case("sn1/odyssey/dma/draw-sentinel-movie",
                   test_draw_sentinel_movie_upload);
    add_fresh_case("sn1/odyssey/dma/draw-sentinel-short-row-gap",
                   test_draw_sentinel_short_row_gap);
    add_fresh_case("sn1/odyssey/dma/draw-sentinel-short-zero-origin-clip",
                   test_draw_sentinel_short_zero_origin_clip);
    add_fresh_case("sn1/odyssey/dma/draw-sentinel-short-rejections",
                   test_draw_sentinel_short_rejections);
    add_fresh_case("sn1/odyssey/readback/sentinel-drawable-relative",
                   test_readback_sentinel_drawable_relative);
    add_fresh_case("sn1/odyssey/readback/intermediate-block",
                   test_readback_intermediate_block);
    add_fresh_case("sn1/odyssey/readback/back-buffer",
                   test_readback_back_buffer);
    add_fresh_case("sn1/odyssey/readback/front-follows-displayed-buffer",
                   test_readback_front_follows_displayed_buffer);
    add_fresh_case("sn1/odyssey/readback/two-by-two-row-order",
                   test_readback_two_by_two_row_order);
    add_fresh_case("sn1/odyssey/readback/row-gap-bottom-up",
                   test_readback_row_gap_bottom_up);
    add_fresh_case("sn1/odyssey/readback/page-crossing",
                   test_readback_page_crossing);
    add_fresh_case("sn1/odyssey/readback/object-source",
                   test_readback_object_source);
    add_fresh_case("sn1/odyssey/readback/gl-rgba8-object",
                   test_readback_gl_rgba8_object);
    add_fresh_case("sn1/odyssey/readback/gl-rgba16-object",
                   test_readback_gl_rgba16_object);
    add_fresh_case("sn1/odyssey/readback/gl-rgba16-scratch-copy",
                   test_readback_gl_rgba16_scratch_copy);
    add_fresh_case("sn1/odyssey/readback/gl-rgba16-scratch-copy-chunks",
                   test_readback_gl_rgba16_scratch_copy_chunks);
    add_fresh_case("sn1/odyssey/readback/gl-rgba8-scratch-copy",
                   test_readback_gl_rgba8_scratch_copy);
    add_fresh_case("sn1/odyssey/readback/index-follows-display-mode",
                   test_readback_index_follows_display_mode);
    add_fresh_case("sn1/odyssey/readback/greeter-caret-cell",
                   test_readback_greeter_caret_cell);
    add_fresh_case("sn1/odyssey/readback/gl-layouts",
                   test_readback_gl_layouts);
    add_fresh_case("sn1/odyssey/readback/failures", test_readback_failures);
    add_fresh_case("sn1/odyssey/swap-retrace-vector",
                   test_swap_retrace_vector);
    add_fresh_case("sn1/odyssey/command-fifo-kernel-clear-box",
                   test_command_fifo_kernel_clear_box);
    add_fresh_case("sn1/odyssey/command-fifo-drawable-words",
                   test_command_fifo_drawable_words);
    add_fresh_case("sn1/odyssey/swap-arm-toggles-displayed-buffer",
                   test_swap_arm_toggles_displayed_buffer);
    add_case("sn1/odyssey/swap-arm-accumulates",
             test_swap_arm_accumulates);
    add_case("sn1/odyssey/swap-immediate-set-and-clear",
             test_swap_immediate_set_and_clear);
    add_case("sn1/odyssey/buffers-hold-opposite-colours",
             test_buffers_hold_opposite_colours);
    add_case("sn1/odyssey/buffers-swap-shows-only-complete-frames",
             test_buffers_swap_shows_only_complete_frames);
    add_case("sn1/odyssey/buffers-display-selection-per-window",
             test_buffers_display_selection_per_window);
    add_case("sn1/odyssey/buffers-draw-selection-keeps-clip-and-masks",
             test_buffers_draw_selection_keeps_clip_and_masks);
    add_case("sn1/odyssey/buffers-read-and-draw-words-are-independent",
             test_buffers_read_and_draw_words_are_independent);
    add_fresh_case("sn1/odyssey/buffers-unsupported-mask-reported-once",
                   test_buffers_unsupported_mask_reported_once);
    add_fresh_case("sn1/odyssey/mesa-selector-and-model-did",
                   test_model_did_pixel_accessors);
    add_case("sn1/odyssey/buffers-gl-triangle-follows-mask",
             test_buffers_gl_triangle_follows_mask);
    add_fresh_case("sn1/odyssey/buffers-migration",
                   test_buffers_migration);
    add_fresh_case("sn1/odyssey/buffers-transfers-follow-mask",
                   test_buffers_transfers_follow_mask);
    add_case("sn1/odyssey/buffers-captured-copy-preserves-both-sources",
             test_buffers_captured_copy_preserves_both_sources);
    add_case("sn1/odyssey/command-fifo-pattern-quad",
             test_command_fifo_pattern_quad);
    add_case("sn1/odyssey/command-fifo-pattern-quad-straddles-screen",
             test_command_fifo_pattern_quad_straddles_screen);
    add_case("sn1/odyssey/command-fifo-pattern-quad-opaque",
             test_command_fifo_pattern_quad_opaque);
    add_case("sn1/odyssey/command-fifo-integer-logic-op",
             test_command_fifo_integer_logic_op);
    add_case("sn1/odyssey/command-fifo-fill-runs-match-pixel-rules",
             test_command_fifo_fill_runs_match_pixel_rules);
    add_case("sn1/odyssey/command-fifo-image-raster-rgb",
             test_command_fifo_image_raster_rgb);
    add_case("sn1/odyssey/command-fifo-image-raster-descending",
             test_command_fifo_image_raster_descending);
    add_case("sn1/odyssey/command-fifo-image-raster-mixed-directions",
             test_command_fifo_image_raster_mixed_directions);
    add_case("sn1/odyssey/command-fifo-image-raster-empty-extent",
             test_command_fifo_image_raster_empty_extent);
    add_case("sn1/odyssey/command-fifo-image-raster-clips-descending",
             test_command_fifo_image_raster_clips_descending);
    add_case("sn1/odyssey/command-fifo-image-raster-window-origin",
             test_command_fifo_image_raster_window_origin);
    add_case("sn1/odyssey/command-fifo-image-raster-formats",
             test_command_fifo_image_raster_formats);
    add_case("sn1/odyssey/command-fifo-image-raster-short",
             test_command_fifo_image_raster_short);
    add_case("sn1/odyssey/command-fifo-image-raster-short-incomplete",
             test_command_fifo_image_raster_short_incomplete);
    add_case("sn1/odyssey/command-fifo-image-raster-colour-index",
             test_command_fifo_image_raster_colour_index);
    add_case("sn1/odyssey/command-fifo-image-raster-colour-index-groups",
             test_command_fifo_image_raster_colour_index_groups);
    add_case("sn1/odyssey/command-fifo-image-raster-colour-index16",
             test_command_fifo_image_raster_colour_index16);
    add_case("sn1/odyssey/command-fifo-gl-crossing-eye",
             test_command_fifo_gl_crossing_eye);
    add_case("sn1/odyssey/command-fifo-gl-integer-points-transform",
             test_command_fifo_gl_integer_points_transform);
    add_case("sn1/odyssey/command-fifo-gl-integer-lines-transform",
             test_command_fifo_gl_integer_lines_transform);
    add_case("sn1/odyssey/command-fifo-gl-integer-quads-transform",
             test_command_fifo_gl_integer_quads_transform);
    add_case("sn1/odyssey/command-fifo-gl-integer-polygon-transform",
             test_command_fifo_gl_integer_polygon_transform);
    add_case("sn1/odyssey/command-fifo-gl-integer-routing-modes",
             test_command_fifo_gl_integer_routing_modes);
    add_case("sn1/odyssey/command-fifo-x11-integer-after-gl",
             test_command_fifo_x11_integer_after_gl);
    add_fresh_case("sn1/odyssey/"
                   "command-fifo-gl-integer-routing-migration-reset",
                   test_command_fifo_gl_integer_routing_migration_reset);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-default",
             test_command_fifo_x11_line_cap_default);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-not-last",
             test_command_fifo_x11_line_cap_not_last);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-reverse-default",
             test_command_fifo_x11_line_cap_reverse_default);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-reverse-not-last",
             test_command_fifo_x11_line_cap_reverse_not_last);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-zero-default",
             test_command_fifo_x11_line_cap_zero_default);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-zero-not-last",
             test_command_fifo_x11_line_cap_zero_not_last);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-vertical",
             test_command_fifo_x11_line_cap_vertical);
    add_case("sn1/odyssey/command-fifo-x11-line-cap-screen-bounds",
             test_command_fifo_x11_line_cap_screen_bounds);
    add_case("sn1/odyssey/command-fifo-gl-integer-assemblies",
             test_command_fifo_gl_integer_assemblies);
    add_case("sn1/odyssey/command-fifo-gl-integer-triangle",
             test_command_fifo_gl_integer_triangle);
    add_case("sn1/odyssey/command-fifo-gl-triangle",
             test_command_fifo_gl_triangle);
    add_case("sn1/odyssey/mesa-per-draw-fallback",
             test_mesa_per_draw_fallback);
    add_case("sn1/odyssey/command-fifo-opaque-stippled-span",
             test_command_fifo_opaque_stippled_span);
    add_case("sn1/odyssey/command-fifo-opaque-span-ignores-shape",
             test_command_fifo_opaque_span_ignores_shape);
    add_case("sn1/odyssey/command-fifo-lines",
             test_command_fifo_lines);
    add_case("sn1/odyssey/command-fifo-lines-cross-screen-edge",
             test_command_fifo_lines_cross_screen_edge);
    add_fresh_case("sn1/odyssey/command-fifo-integer-primitives-stream",
                   test_command_fifo_integer_primitives_stream);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-reverse-y",
             test_command_fifo_gl_bitmap_rect_reverse_y);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-disabled",
             test_command_fifo_gl_bitmap_rect_disabled);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-origin-clip",
             test_command_fifo_gl_bitmap_rect_origin_clip);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-wrap-zero",
             test_command_fifo_gl_bitmap_rect_wrap_zero);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-unsupported",
             test_command_fifo_gl_bitmap_rect_unsupported);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-large-anchor",
             test_command_fifo_gl_bitmap_rect_large_anchor);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-zero-depth",
             test_command_fifo_gl_bitmap_rect_zero_preserves_depth);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-depth-only",
             test_command_fifo_gl_bitmap_rect_depth_only);
    add_case("sn1/odyssey/command-fifo-gl-bitmap-rect-index",
             test_command_fifo_gl_bitmap_rect_index);
    add_case("sn1/odyssey/command-fifo-gl-clear-rect",
             test_command_fifo_gl_clear_rect);
    add_case("sn1/odyssey/command-fifo-gl-clear-then-triangle",
             test_command_fifo_gl_clear_then_triangle);
    add_fresh_case("sn1/odyssey/gl-large-clear-draws-in-bands",
                   test_gl_large_clear_draws_in_bands);
    add_fresh_case("sn1/odyssey/gl-raster-bands-off-draws-on-one-thread",
                   test_gl_raster_bands_off_draws_on_one_thread);
    add_fresh_case("sn1/odyssey/gl-bands-match-one-thread",
                   test_gl_bands_match_one_thread);
    add_case("sn1/odyssey/command-fifo-gl-window-origin-and-clip",
             test_command_fifo_gl_window_origin_and_clip);
    add_case("sn1/odyssey/command-fifo-gl-clip-rectangles",
             test_command_fifo_gl_clip_rectangles);
    add_case("sn1/odyssey/command-fifo-gl-ortho-combined-matrix",
             test_command_fifo_gl_ortho_combined_matrix);
    add_case("sn1/odyssey/command-fifo-gl-texture-slot-leaves-geometry",
             test_command_fifo_gl_texture_slot_leaves_geometry);
    add_fresh_case("sn1/odyssey/command-fifo-compare-incomplete",
                   test_command_fifo_compare_incomplete);
    add_fresh_case("sn1/odyssey/command-fifo-compare-neutral-kernel",
                   test_command_fifo_compare_neutral_kernel);
    add_fresh_case("sn1/odyssey/command-fifo-compare-neutral-user",
                   test_command_fifo_compare_neutral_user);
    add_fresh_case("sn1/odyssey/command-fifo-compare-nonzero-first",
                   test_command_fifo_compare_nonzero_first);
    add_fresh_case("sn1/odyssey/command-fifo-compare-nonzero-second",
                   test_command_fifo_compare_nonzero_second);
    add_fresh_case("sn1/odyssey/command-fifo-compare-nonzero-first-user",
                   test_command_fifo_compare_nonzero_first_user);
    add_fresh_case("sn1/odyssey/command-fifo-compare-nonzero-second-user",
                   test_command_fifo_compare_nonzero_second_user);
    add_fresh_case("sn1/odyssey/command-fifo-compare-cancels-query-user",
                   test_command_fifo_compare_cancels_query_user);
    add_case("sn1/odyssey/"
             "command-fifo-gl-float-colour-integer-conversion",
             test_command_fifo_gl_float_colour_integer_conversion);
    add_fresh_case("sn1/odyssey/byte-colour/conversion",
                   test_byte_colour_conversion);
    add_fresh_case("sn1/odyssey/byte-colour/override",
                   test_byte_colour_override);
    add_fresh_case("sn1/odyssey/byte-colour/index-guard",
                   test_byte_colour_index_guard);
    add_fresh_case("sn1/odyssey/byte-colour/drawable-id",
                   test_byte_colour_drawable_id);
    add_fresh_case("sn1/odyssey/"
                   "command-fifo-gl-byte-colour-float-and-replay",
                   test_command_fifo_gl_byte_colour_float_and_replay);
    add_case("sn1/odyssey/command-fifo-gl-float-colour-integer-vertices",
             test_command_fifo_gl_float_colour_integer_vertices);
    add_case("sn1/odyssey/scanout-overlay-planes",
             test_scanout_overlay_planes);
    add_case("sn1/odyssey/scanout-overlay-unselected-mask",
             test_scanout_overlay_unselected_mask);
    add_case("sn1/odyssey/gl-aux-refusals-reported-once",
             test_gl_aux_refusals_reported_once);
    add_case("sn1/odyssey/scanout-overlay-menu", test_scanout_overlay_menu);
    add_case("sn1/odyssey/command-fifo-dest-select-flags",
             test_command_fifo_dest_select_flags);
    add_case("sn1/odyssey/command-fifo-gl-colour-index",
             test_command_fifo_gl_colour_index);
    add_fresh_case("sn1/odyssey/command-fifo-gl-colour-index-write-mask",
             test_command_fifo_gl_colour_index_write_mask);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-window-unknown",
             test_command_fifo_gl_program_window_unknown);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-window-record",
                   test_command_fifo_gl_program_window_record);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-window-local-viewer",
                   test_command_fifo_gl_program_window_local_viewer);
    add_case("sn1/odyssey/command-fifo-gl-combined-carries-modelview",
             test_command_fifo_gl_combined_carries_modelview);
    add_case("sn1/odyssey/scanout-main-id-0-uses-its-own-mode",
             test_scanout_main_id_0_uses_its_own_mode);
    add_case("sn1/odyssey/command-fifo-gl-flat-shading",
             test_command_fifo_gl_flat_shading);
    add_case("sn1/odyssey/command-fifo-gl-vertex-3i",
             test_command_fifo_gl_vertex_3i);
    add_case("sn1/odyssey/command-fifo-gl-fog-curve",
             test_command_fifo_gl_fog_curve);
    add_case("sn1/odyssey/command-fifo-gl-fog-native-packets",
             test_command_fifo_gl_fog_native_packets);
    add_fresh_case("sn1/odyssey/command-fifo-gl-fog-migration",
                   test_command_fifo_gl_fog_migration);
    add_fresh_case("sn1/odyssey/command-fifo-gl-dither",
                   test_command_fifo_gl_dither);
    add_case("sn1/odyssey/command-fifo-gl-depth-test",
             test_command_fifo_gl_depth_test);
    add_case("sn1/odyssey/command-fifo-gl-alpha-test",
             test_command_fifo_gl_alpha_test);
    add_case("sn1/odyssey/command-fifo-gl-blend",
             test_command_fifo_gl_blend);
    add_fresh_case("sn1/odyssey/command-fifo-gl-stencil-state",
                   test_command_fifo_gl_stencil_state);
    add_case("sn1/odyssey/command-fifo-gl-stencil-masks-draws",
             test_command_fifo_gl_stencil_masks_draws);
    add_case("sn1/odyssey/command-fifo-gl-cull-face",
             test_command_fifo_gl_cull_face);
    add_case("sn1/odyssey/command-fifo-gl-polygon-mode",
             test_command_fifo_gl_polygon_mode);
    add_case("sn1/odyssey/command-fifo-gl-line-point-size",
             test_command_fifo_gl_line_point_size);
    add_case("sn1/odyssey/command-fifo-gl-point-smoothing",
             test_command_fifo_gl_point_smoothing);
    add_case("sn1/odyssey/command-fifo-gl-line-smoothing",
             test_command_fifo_gl_line_smoothing);
    add_case("sn1/odyssey/command-fifo-gl-line-stipple",
             test_command_fifo_gl_line_stipple);
    add_fresh_case("sn1/odyssey/command-fifo-gl-line-stipple-migration",
                   test_command_fifo_gl_line_stipple_migration);
    add_case("sn1/odyssey/command-fifo-gl-polygon-offset",
             test_command_fifo_gl_polygon_offset);
    add_case("sn1/odyssey/command-fifo-gl-clip-box",
             test_command_fifo_gl_clip_box);
    add_case("sn1/odyssey/command-fifo-gl-clip-plane",
             test_command_fifo_gl_clip_plane);
    add_case("sn1/odyssey/command-fifo-gl-clip-plane-eye-space",
             test_command_fifo_gl_clip_plane_eye_space);
    add_fresh_case("sn1/odyssey/command-fifo-gl-clip-plane-migration",
                   test_command_fifo_gl_clip_plane_migration);
    add_case("sn1/odyssey/command-fifo-unknown-word-skips-its-operands",
             test_command_fifo_unknown_word_skips_its_operands);
    add_case("sn1/odyssey/command-fifo-gl-index-does-not-blend",
             test_command_fifo_gl_index_does_not_blend);
    add_case("sn1/odyssey/command-fifo-gl-opaque-stipple-index",
             test_command_fifo_gl_opaque_stipple_index);
    add_case("sn1/odyssey/command-fifo-gl-depth-sign",
             test_command_fifo_gl_depth_sign);
    add_case("sn1/odyssey/command-fifo-gl-depth-model-view-perspective",
             test_command_fifo_gl_depth_model_view_perspective);
    add_case("sn1/odyssey/command-fifo-gl-depth-scale",
             test_command_fifo_gl_depth_scale);
    add_case("sn1/odyssey/command-fifo-gl-depth-eye-linear",
             test_command_fifo_gl_depth_eye_linear);
    add_case("sn1/odyssey/command-fifo-gl-depth-float",
             test_command_fifo_gl_depth_float);
    add_case("sn1/odyssey/command-fifo-gl-depth-scale-fallback",
             test_command_fifo_gl_depth_scale_fallback);
    add_case("sn1/odyssey/command-fifo-gl-colour-mask",
             test_command_fifo_gl_colour_mask);
    add_case("sn1/odyssey/command-fifo-gl-depth-only-clear",
             test_command_fifo_gl_depth_only_clear);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-record-lit",
                   test_command_fifo_gl_program_record_lit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-records-lit",
                   test_command_fifo_gl_program_records_lit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-colour-material",
                   test_command_fifo_gl_colour_material);
    add_fresh_case("sn1/odyssey/command-fifo-gl-integer-colour",
                   test_command_fifo_gl_integer_colour);
    add_fresh_case("sn1/odyssey/command-fifo-gl-integer-normals",
                   test_command_fifo_gl_integer_normals);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-record-unlit",
                   test_command_fifo_gl_program_record_unlit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-list-lit",
                   test_command_fifo_gl_program_list_lit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-list-lit-local-viewer",
                   test_command_fifo_gl_program_list_lit_local_viewer);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-list-lit-positional",
                   test_command_fifo_gl_program_list_lit_positional);
    add_fresh_case("sn1/odyssey/command-fifo-gl-vertex-3i-lit",
                   test_command_fifo_gl_vertex_3i_lit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-vertex-2i-lit",
                   test_command_fifo_gl_vertex_2i_lit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-lighting-uses-modelview",
                   test_command_fifo_gl_lighting_uses_modelview);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-two-sided",
                   test_command_fifo_gl_program_two_sided);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-texgen",
                   test_command_fifo_gl_program_texgen);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-texture-finish",
                   test_command_fifo_gl_program_texture_finish);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-list-unlit",
                   test_command_fifo_gl_program_list_unlit);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-record-clip-head",
                   test_command_fifo_gl_program_record_clip_head);
    add_fresh_case("sn1/odyssey/command-fifo-gl-program-unknown",
                   test_command_fifo_gl_program_unknown);
    add_fresh_case("sn1/odyssey/command-fifo-unknown-word-is-unimplemented",
                   test_command_fifo_unknown_word_is_unimplemented);
    add_fresh_case("sn1/odyssey/unimplemented-reports-independent-budgets",
                   test_unimplemented_reports_have_independent_budgets);
    add_case("sn1/odyssey/command-fifo-gl-window-restore",
             test_command_fifo_gl_window_restore);
    add_case("sn1/odyssey/command-fifo-gl-window-offscreen-tail",
             test_command_fifo_gl_window_offscreen_tail);
    add_case("sn1/odyssey/command-fifo-gl-window-tail-migration",
             test_command_fifo_gl_window_tail_migration);
    add_case("sn1/odyssey/command-fifo-gl-window-tail-migration-absent",
             test_command_fifo_gl_window_tail_migration_absent);
    add_case("sn1/odyssey/command-fifo-gl-window-id-compare",
             test_command_fifo_gl_window_id_compare);
    add_case("sn1/odyssey/command-fifo-gl-window-id-compare-restore",
             test_command_fifo_gl_window_id_compare_restore);
    add_case("sn1/odyssey/command-fifo-gl-window-id-compare-migration",
             test_command_fifo_gl_window_id_compare_migration);
    add_case("sn1/odyssey/command-fifo-gl-window-id-compare-triangle",
             test_command_fifo_gl_window_id_compare_triangle);
    add_case("sn1/odyssey/command-fifo-gl-texture-luminance",
             test_command_fifo_gl_texture_luminance);
    add_case("sn1/odyssey/command-fifo-gl-texture-rgb",
             test_command_fifo_gl_texture_rgb);
    add_case("sn1/odyssey/command-fifo-gl-texture-rgba",
             test_command_fifo_gl_texture_rgba);
    add_fresh_case("sn1/odyssey/dma-texture-object",
                   test_dma_texture_object);
    add_fresh_case("sn1/odyssey/dma-texture-luminance-alpha",
                   test_dma_texture_luminance_alpha);
    add_fresh_case("sn1/odyssey/dma-texture-12bit-sources",
                   test_dma_texture_12bit_sources);
    add_case("sn1/odyssey/burst-texture-12bit-sources",
             test_burst_texture_12bit_sources);
    add_fresh_case("sn1/odyssey/object-store-reclaims",
                   test_object_store_reclaims);
    add_fresh_case("sn1/odyssey/object-store-grows-without-forgetting",
                   test_object_store_grows_without_forgetting);
    add_fresh_case("sn1/odyssey/object-store-migrates-used-part",
                   test_object_store_migrates_used_part);
    add_case("sn1/odyssey/texture-adapter/rgba12-byte-upload",
             test_texture_adapter_rgba12_byte_upload);
    add_case("sn1/odyssey/texture-adapter/nearest",
             test_texture_adapter_nearest);
    add_fresh_case("sn1/odyssey/texture-adapter/detail-zero-identity",
                   test_texture_adapter_detail_zero_identity);
    add_case("sn1/odyssey/texture-adapter/detail-zero-alpha-reference",
             test_texture_adapter_detail_zero_alpha_reference);
    add_fresh_case("sn1/odyssey/texture-adapter/detail-rectangular-migration",
                   test_texture_adapter_detail_rectangular_migration);
    add_fresh_case("sn1/odyssey/texture-adapter/detail-function-endpoints",
                   test_texture_adapter_detail_function_endpoints);
    add_case("sn1/odyssey/texture-adapter/nearest-clamp",
             test_texture_adapter_nearest_clamp);
    add_case("sn1/odyssey/texture-adapter/edge-clamp-size-flag",
             test_texture_adapter_edge_clamp_size_flag);
    add_case("sn1/odyssey/texture-adapter/bordered-mipmap",
             test_texture_bordered_mipmap);
    add_case("sn1/odyssey/texture-adapter/volume-slice-spacing",
             test_texture_volume_slice_spacing);
    add_case("sn1/odyssey/command-fifo-gl-vertex-4f",
             test_command_fifo_gl_vertex_4f);
    add_fresh_case("sn1/odyssey/command-fifo-gl-empty-primitive",
                   test_command_fifo_gl_empty_primitive);
    add_fresh_case("sn1/odyssey/texture-adapter/nearest-clamp-border",
                   test_texture_adapter_nearest_clamp_border);
    add_fresh_case("sn1/odyssey/texture-adapter/clamp-border-state-restore",
                   test_texture_adapter_clamp_border_state_restore);
    add_case("sn1/odyssey/texture-adapter/explicit-level",
             test_texture_chain_explicit_level);
    add_case("sn1/odyssey/texture-adapter/integer-quads",
             test_texture_chain_integer_quads);
    add_case("sn1/odyssey/texture-adapter/lod-bias",
             test_texture_lod_bias);
    add_case("sn1/odyssey/texture-adapter/max-clamp",
             test_texture_max_clamp);
    add_case("sn1/odyssey/texture-adapter/one-dimensional-mode",
             test_texture_one_dimensional_mode);
    /*
     * The clamp cases cross the edge mode, a clamped or a stored border,
     * with nearest and linear filtering, in one and two dimensions, and
     * filter a wide one-dimensional stored image both ways. The quick setup
     * takes each mode once per dimension, with the filters swapped between
     * the two, and the wide image linear; the thorough setup takes them all.
     */
    if (g_test_slow()) {
        add_case("sn1/odyssey/texture-adapter/"
             "one-dimensional-clamp-nearest",
             test_texture_one_dimensional_clamp_nearest);
    }
    add_case("sn1/odyssey/texture-adapter/one-dimensional-clamp-linear",
             test_texture_one_dimensional_clamp_linear);
    add_case("sn1/odyssey/texture-adapter/one-dimensional-integer-coordinate",
             test_texture_one_dimensional_integer_coordinate);
    add_case("sn1/odyssey/texture-adapter/one-dimensional-stored-nearest",
             test_texture_one_dimensional_stored_nearest);
    if (g_test_slow()) {
        add_case("sn1/odyssey/texture-adapter/"
             "one-dimensional-stored-linear",
             test_texture_one_dimensional_stored_linear);
    }
    add_case("sn1/odyssey/texture-adapter/one-dimensional-stored-upload",
             test_texture_one_dimensional_stored_upload);
    add_case("sn1/odyssey/texture-adapter/"
             "one-dimensional-wide-stored-upload",
             test_texture_one_dimensional_wide_stored_upload);
    if (g_test_slow()) {
        add_case("sn1/odyssey/texture-adapter/"
             "one-dimensional-wide-stored-nearest",
             test_texture_one_dimensional_wide_stored_nearest);
    }
    add_case("sn1/odyssey/texture-adapter/"
             "one-dimensional-wide-stored-linear",
             test_texture_one_dimensional_wide_stored_linear);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-copies",
             test_texture_public_border_copied_rows);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-1d-width8-nearest",
             test_texture_public_border_1d_width8_nearest);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-1d-migration",
                   test_texture_public_border_1d_migration);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-1d-width8-linear",
             test_texture_public_border_1d_width8_linear);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-1d-width64-nearest",
             test_texture_public_border_1d_width64_nearest);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-1d-width64-linear",
             test_texture_public_border_1d_width64_linear);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-2d-nearest",
             test_texture_public_border_2d_nearest);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-2d-linear",
             test_texture_public_border_2d_linear);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-2d-incomplete",
             test_texture_public_border_2d_incomplete);
    add_case("sn1/odyssey/texture-adapter/"
             "public-stored-border-3d",
             test_texture_public_border_3d);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-first-base",
                   test_public_stored_volume_first_base);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-second-base",
                   test_public_stored_volume_second_base);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-first-base-missing-0",
                   test_public_stored_volume_first_base_missing_0);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-first-base-missing-1",
                   test_public_stored_volume_first_base_missing_1);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-first-base-missing-2",
                   test_public_stored_volume_first_base_missing_2);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-first-base-missing-3",
                   test_public_stored_volume_first_base_missing_3);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-first-base-missing-all",
                   test_public_stored_volume_first_base_missing_all);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-second-base-missing-0",
                   test_public_stored_volume_second_base_missing_0);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-second-base-missing-1",
                   test_public_stored_volume_second_base_missing_1);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-second-base-missing-2",
                   test_public_stored_volume_second_base_missing_2);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-second-base-missing-3",
                   test_public_stored_volume_second_base_missing_3);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-second-base-missing-all",
                   test_public_stored_volume_second_base_missing_all);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-low-origin",
                   test_public_stored_volume_low_origin);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-other-stride",
                   test_public_stored_volume_other_stride);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-wrong-border",
                   test_public_stored_volume_wrong_border);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-wrong-size",
                   test_public_stored_volume_wrong_size);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-migration",
                   test_texture_public_border_3d_migration);
    add_fresh_case("sn1/odyssey/texture-adapter/"
                   "public-stored-border-3d-reset",
                   test_texture_public_border_3d_reset);
    add_case("sn1/odyssey/texture-adapter/"
             "two-dimensional-border-upload",
             test_texture_two_dimensional_border_upload);
    add_case("sn1/odyssey/texture-adapter/"
             "two-dimensional-clamp-nearest",
             test_texture_two_dimensional_clamp_nearest);
    if (g_test_slow()) {
        add_case("sn1/odyssey/texture-adapter/"
             "two-dimensional-clamp-linear",
             test_texture_two_dimensional_clamp_linear);
        add_case("sn1/odyssey/texture-adapter/"
             "two-dimensional-stored-nearest",
             test_texture_two_dimensional_stored_nearest);
    }
    add_case("sn1/odyssey/texture-adapter/"
             "two-dimensional-stored-linear",
             test_texture_two_dimensional_stored_linear);
    add_case("sn1/odyssey/texture-adapter/"
             "two-dimensional-missing-border-row",
             test_texture_two_dimensional_missing_border_row);
    add_case("sn1/odyssey/texture-adapter/height-one",
             test_texture_chain_height_one);
    add_case("sn1/odyssey/texture-adapter/construction",
             test_texture_chain_construction);
    add_case("sn1/odyssey/texture-adapter/binding-filters-matrix",
             test_texture_adapter_binding_filters_matrix);
    add_case("sn1/odyssey/texture-adapter/components-environment",
             test_texture_adapter_components_environment);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-st",
                   test_texture_adapter_linear_st);
    add_case("sn1/odyssey/texture-adapter/colour-table",
             test_texture_adapter_colour_table);
    add_case("sn1/odyssey/texture-adapter/colour-table-l8-rgb-alpha",
             test_texture_adapter_l8_table_rgb_alpha);
    add_case("sn1/odyssey/texture-adapter/colour-table-l8-rgba-alpha",
             test_texture_adapter_l8_table_rgba_alpha);
    add_case("sn1/odyssey/texture-adapter/colour-table-rgb-basis",
             test_texture_adapter_colour_table_rgb_basis);
    add_case("sn1/odyssey/texture-adapter/colour-table-rgba8-rgb-basis",
             test_texture_adapter_colour_table_rgba8_rgb_basis);
    add_case("sn1/odyssey/texture-adapter/colour-table-rgba8-rgba-basis",
             test_texture_adapter_colour_table_rgba8_rgba_basis);
    add_case("sn1/odyssey/texture-adapter/colour-table-rgba8-rgb-alpha",
             test_texture_adapter_colour_table_rgba8_rgb_alpha);
    add_case("sn1/odyssey/texture-adapter/colour-table-rgba8-rgba-alpha",
             test_texture_adapter_colour_table_rgba8_rgba_alpha);
    add_case("sn1/odyssey/texture-adapter/colour-table-incomplete",
             test_texture_adapter_colour_table_incomplete);
    add_case("sn1/odyssey/texture-adapter/colour-table-reset",
             test_texture_adapter_colour_table_reset);
    add_fresh_case("sn1/odyssey/texture-adapter/colour-table-migration",
                   test_texture_adapter_colour_table_migration);
    add_case("sn1/odyssey/texture-adapter/scalar-components",
             test_texture_adapter_scalar_components);
    add_case("sn1/odyssey/texture-adapter/luminance-alpha",
             test_texture_adapter_luminance_alpha);
    add_case("sn1/odyssey/texture-adapter/volume-mipmap-trilinear",
             test_texture_adapter_volume_mipmap_trilinear);
    add_case("sn1/odyssey/texture-adapter/volume-mipmap-depth-one",
             test_texture_adapter_volume_mipmap_depth_one);
    add_case("sn1/odyssey/texture-adapter/volume-mipmap-base-level",
             test_texture_adapter_volume_mipmap_base_level);
    add_case("sn1/odyssey/texture-adapter/volume-mipmap-nearest",
             test_texture_adapter_volume_mipmap_nearest);
    add_case("sn1/odyssey/texture-adapter/volume-slices-follow-each-draw",
             test_texture_adapter_volume_slices_follow_each_draw);
    add_case("sn1/odyssey/texture-adapter/volume-mipmap-missing-slice",
             test_texture_adapter_volume_mipmap_missing_slice);
    add_fresh_case("sn1/odyssey/texture-adapter/volume-mipmap-state-restore",
                   test_texture_adapter_volume_mipmap_state_restore);
    add_case("sn1/odyssey/texture-adapter/level-zero-3d",
             test_texture_adapter_level_zero_3d);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-r",
                   test_texture_adapter_linear_r);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-strq-bulk",
                   test_texture_adapter_linear_strq_bulk);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-srq-bulk",
                   test_texture_adapter_linear_srq_bulk);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-srq",
                   test_texture_adapter_linear_srq);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-strq",
                   test_texture_adapter_linear_strq);
    add_fresh_case("sn1/odyssey/texture-adapter/linear-q",
                   test_texture_adapter_linear_q);
    add_case("sn1/odyssey/texture-adapter/projective-3d",
             test_texture_adapter_projective_3d);
    add_fresh_case("sn1/odyssey/texture-adapter/3d-state-restore",
                   test_texture_adapter_3d_state_restore);
    add_fresh_case("sn1/odyssey/context-save-texture-coordinate-3d",
                   test_context_save_texture_coordinate_3d);
    add_case("sn1/odyssey/texture-adapter/blend-environment",
             test_texture_adapter_blend_environment);
    add_fresh_case("sn1/odyssey/texture-adapter/blend-state-restore",
                   test_texture_adapter_blend_state_restore);
    add_case("sn1/odyssey/texture-adapter/storage-dimensions",
             test_texture_adapter_storage_dimensions);
    add_fresh_case("sn1/odyssey/texture-adapter/state-restore",
                   test_texture_adapter_state_restore);
    add_case("sn1/odyssey/texture-adapter/unsupported-missing",
             test_texture_adapter_unsupported_missing);
    add_fresh_case("sn1/odyssey/command-fifo-gl-texture-state",
                   test_command_fifo_gl_texture_state);
    add_fresh_case("sn1/odyssey/back-material-migration",
                   test_back_material_migration);
    add_fresh_case("sn1/odyssey/back-material-migration-defaults",
                   test_back_material_migration_defaults);
    add_fresh_case("sn1/odyssey/texture-current-query-4f-migration",
                   test_texture_current_query_4f_migration);
    add_fresh_case("sn1/odyssey/texture-current-query-4f-migration-defaults",
                   test_texture_current_query_4f_migration_defaults);
    add_fresh_case("sn1/odyssey/texture-current-query-1i",
                   test_texture_current_query_1i);
    add_fresh_case("sn1/odyssey/texture-current-query-1f",
                   test_texture_current_query_1f);
    add_fresh_case("sn1/odyssey/texture-current-query-1f-kernel",
                   test_texture_current_query_1f_kernel);
    add_fresh_case("sn1/odyssey/texture-current-query-1f-migration",
                   test_texture_current_query_1f_migration);
    add_fresh_case("sn1/odyssey/texture-current-query-3f",
                   test_texture_current_query_3f);
    add_fresh_case("sn1/odyssey/texture-current-query-3f-kernel",
                   test_texture_current_query_3f_kernel);
    add_fresh_case("sn1/odyssey/texture-current-query-3f-migration",
                   test_texture_current_query_3f_migration);
    add_fresh_case("sn1/odyssey/texture-current-query-4f",
                   test_texture_current_query_4f);
    add_fresh_case("sn1/odyssey/query-record-migration",
                   test_query_record_migration);
    add_fresh_case("sn1/odyssey/query-record-migration-defaults",
                   test_query_record_migration_defaults);
    add_fresh_case("sn1/odyssey/context-save-texture-coordinate-inactive-r",
                   test_context_save_texture_coordinate_inactive_r);
    add_fresh_case("sn1/odyssey/context-save-texture-coordinate-inactive-q",
                   test_context_save_texture_coordinate_inactive_q);
    add_fresh_case(
        "sn1/odyssey/context-save-texture-coordinate-inactive-negative-zero-r",
        test_context_save_texture_coordinate_inactive_negative_zero_r);
    add_fresh_case("sn1/odyssey/context-save-texture-coordinate",
                   test_context_save_texture_coordinate);
    add_fresh_case("sn1/odyssey/context-save-idle-descriptors",
                   test_context_save_idle_descriptors);
    add_fresh_case("sn1/odyssey/"
                   "context-save-texture-coordinate-active-primitive",
                   test_context_save_texture_coordinate_active_primitive);
    add_fresh_case("sn1/odyssey/context-save-back-material",
                   test_context_save_back_material);
    add_fresh_case("sn1/odyssey/context-save-back-material-reset",
                   test_context_save_back_material_reset);
    add_fresh_case("sn1/odyssey/context-save-normal-ring",
                   test_context_save_normal_ring);
    add_fresh_case("sn1/odyssey/command-fifo-gl-never-set-index",
                   test_command_fifo_gl_never_set_index);
    add_fresh_case("sn1/odyssey/command-fifo-context-save-colour",
                   test_command_fifo_context_save_colour);
    add_fresh_case("sn1/odyssey/command-fifo-context-save-idle-state",
                   test_command_fifo_context_save_idle_state);
    add_fresh_case("sn1/odyssey/context-restart-triangle",
                   test_context_restart_triangle);
    add_fresh_case("sn1/odyssey/context-restart-edited-page",
                   test_context_restart_edited_page);
    add_fresh_case("sn1/odyssey/context-save-primitive-records",
                   test_context_save_primitive_records);
    add_fresh_case("sn1/odyssey/context-save-unrecorded-attributes",
                   test_context_save_unrecorded_attributes);
    add_fresh_case("sn1/odyssey/context-save-zero-vertex-primitive",
                   test_context_save_zero_vertex_primitive);
    add_fresh_case("sn1/odyssey/context-save-strip-parity",
                   test_context_save_strip_parity);
    add_fresh_case("sn1/odyssey/context-restart-one-vertex-strip",
                   test_context_restart_one_vertex_strip);
    add_fresh_case("sn1/odyssey/context-restart-quad-strip-after-wrap",
                   test_context_restart_quad_strip_after_wrap);
    add_fresh_case("sn1/odyssey/context-switch-integer-quad",
                   test_context_switch_integer_quad);
    add_fresh_case("sn1/odyssey/context-switch-polygon-raster-restore",
                   test_context_switch_polygon_raster_restore);
    add_fresh_case("sn1/odyssey/context-switch-polygon-assembly",
                   test_context_switch_polygon_assembly);
    add_fresh_case("sn1/odyssey/context-restart-unsupported-words",
                   test_context_restart_unsupported_words);
    add_fresh_case("sn1/odyssey/context-switch-completion",
                   test_context_switch_completion);
    add_fresh_case("sn1/odyssey/command-work-runs-in-slices",
                   test_command_work_runs_in_slices);
    add_fresh_case("sn1/odyssey/command-work-orders-context-switch",
                   test_command_work_orders_context_switch);
    add_fresh_case("sn1/odyssey/command-work-read-waits",
                   test_command_work_read_waits);
    add_fresh_case("sn1/odyssey/command-work-status-shows-waiting-stores",
                   test_command_work_status_shows_waiting_stores);
    add_fresh_case("sn1/odyssey/command-work-low-water-after-drain",
                   test_command_work_low_water_after_drain);
    add_fresh_case("sn1/odyssey/command-work-credits-at-fifo-entry",
                   test_command_work_credits_at_fifo_entry);
    add_fresh_case("sn1/odyssey/command-work-map-rewrite-settles",
                   test_command_work_map_rewrite_settles);
    add_fresh_case("sn1/odyssey/command-work-stop-and-reset",
                   test_command_work_stop_and_reset);
    add_fresh_case("sn1/odyssey/command-work-draws-on-board-thread",
                   test_command_work_draws_on_board_thread);
    add_fresh_case("sn1/odyssey/context-switch-without-shadow-address",
                   test_context_switch_without_shadow_address_is_a_guest_error);

    add_fresh_case("sn1/odyssey/processor-credit-return",
                   test_processor_credit_return);

    result = g_test_run();
    if (shared_board) {
        qtest_quit(shared_board);
    }
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
