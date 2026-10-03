/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Odyssey (VPro) graphics board plugged into the Origin 300's XIO
 * connector. No SGI product shipped that composition; it is the host the
 * emulator can build today, and the register contract under test is the
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
#include "sgi-test.h"
#include "sgi-migrate.h"

#define MACHINE "-M origin300,topology=origin300-v12-direct " \
                "-cpu R14000 -smp 2 -S"

/*
 * The connector's crosstalk position, repeated from the machine's own
 * selection rather than borrowed from it. No source binds the connector
 * to a crossbar port; see the chip's SGI_XBRIDGE_XIO_PORT_DEFAULT.
 */
#define SLOT 11
#define WIDGET (SPACE_IO + (uint64_t)SLOT * UINT64_C(0x1000000))
#define BOARD "/machine/module0/rear-panel"

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
#define ODSY_SW_SYNC 0x107c
#define ODSY_BUZZ_1 0x1078
#define ODSY_BUZZ_2 0x1088
#define ODSY_BUZZ_3 0x1098
#define ODSY_BUZZ_4 0x10a8
#define ODSY_BUZZ_5 0x10b8
#define ODSY_BUZZ_6 0x10c0
/*
 * The swap-control register bank (hw/misc/sgi-odyssey.c,
 * docs/analysis/odyssey-swap-wake-mechanism.md), redeclared here the same way
 * every register offset above already is. Each is a mask with one bit per
 * window: ODSY_SWAP_ARM the windows to flip at the next retrace,
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
 * CPU-relative offsets tests/qtest/sn1-address-map-test.c's own
 * BEDROCK_MD_MEMORY_CONFIG/BEDROCK_MD_INSTALLED_SELECT2 constants reach.
 * Real firmware programs this before touching RAM; a halted board_start()
 * board never runs that firmware, so a test that DMAs into guest RAM has
 * to install a bank itself first, or every address in it resolves as an
 * unassigned hole rather than memory.
 */
#define BEDROCK_MD_MEMORY_CONFIG UINT64_C(0x780008)
#define BEDROCK_MD_INSTALLED UINT64_C(0x3300)
#define BEDROCK_MD_INSTALLED_SELECT2 \
    ((UINT64_C(2) << 32) | BEDROCK_MD_INSTALLED)

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
#define ODSY_TEST_ROUTE_VECTORS \
    ((ODSY_TEST_SOFTSYNC_VECTOR << ODSY_ROUTE_VECTOR_SOFTSYNC_SHIFT) | \
     (ODSY_TEST_RETRACE_VECTOR << ODSY_ROUTE_VECTOR_RETRACE_SHIFT) | \
     (ODSY_TEST_DMA_VECTOR << ODSY_ROUTE_VECTOR_DMA_SHIFT))

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
 * The one-bit pattern-quad text path (docs/analysis/odyssey-glyph-raster-
 * format.md), redeclared here the same way every register-select above is.
 * 0x3c4e loads pattern buffer words 0-15 and 0x3c4f words 16-31 (a 16-word
 * burst each, bit-field arity 15 -> 16); 0x043f selects the shape (0x4 = 32x32)
 * and 0x0404's bits 3 and 4 enable pattern mode.
 */
#define ODSY_CMD_PATTERN_LOW 0x00013c4e
#define ODSY_CMD_PATTERN_HIGH 0x00013c4f
#define ODSY_CMD_PATTERN_SHAPE 0x0001043f
#define ODSY_CMD_PATTERN_MODE 0x00010404
#define ODSY_PATTERN_SHAPE_32X32 0x4
#define ODSY_PATTERN_MODE_ENABLE 0x18

/*
 * The BEGIN-0xa image raster path (docs/analysis/odyssey-glyph-raster-
 * format.md): a transfer-trigger state word, BEGIN with primitive 0xa, the
 * 0xcf80a92f box record's ten floats, a (0, 0) vertex, a transfer header and
 * its tail, then the pixel bursts. A burst word is 0x00014000 | (words -
 * words/16) << 10 | fmt (fmt 0x11 one RGB pixel per word, 0x10 four colour
 * indices per word); the header's bits 23:20 hold the group's pixel count,
 * 15 standing for 16.
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
 * The page-table pixel/texture DMA descriptor (hw/misc/sgi-odyssey.c,
 * docs/analysis/odyssey-account-icons.md). Two 64-bit doublewords through the
 * command FIFO: the opcode (0xe01 for an image transfer in either direction,
 * 0xe03 from the diagnostic region path only) over the packed high half of
 * the table's physical address, then the packed low half
 * over a zero word (dma_descriptor() below packs it). A translation-table
 * entry is (byte_address << 16) | length, 16 KiB pages, the first entry
 * possibly starting part-way into its page, terminated by a zero-length
 * entry, the whole thing past a 0x20-byte header.
 */
#define ODSY_CMD_DMA_IMAGE 0x00000e01u
#define ODSY_CMD_DMA_REGION 0x00000e03u
/*
 * The two buffer-select doublewords pushed ahead of a swap
 * (hw/misc/sgi-odyssey.c,
 * docs/analysis/odyssey-swap-wake-mechanism.md): each 8-byte store reaches the
 * interpreter as an opcode word (0x608/0x609) plus one operand word (0x2000 in
 * the capture). Recognized so it is consumed cleanly, not reported as unmodeled.
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
 * DMA-done interrupt (docs/analysis/odyssey-dma-image-header.md).
 */
#define ODSY_DMA_HDR_CONTROL 0x1c
#define ODSY_DMA_IRQ_REQUEST 0x80000000u

/*
 * Reset values the stock IP35 BaseIO PROM's graphics diagnostic expects of a
 * healthy Tezro board (docs/research/graphics/odyssey-baseio-prom-graphics-
 * 20260925.md, via odyssey-fuel-identity-spike-20260925.md). The Buzz
 * registers carry no established reference and reset to zero.
 */
#define XT_REQ_TIMEOUT_RESET 0x000fffff
#define FLCTL_CONFIG_RESET 0x0001c000

/*
 * The display back end's indexed register file: a 64-bit push to the
 * selector port, read back through its own per-index window. Only the
 * opt port's I2C command/status pair is exercised here; the note's two
 * worked transfers both run on it
 * (docs/research/graphics/odyssey-i2c-engine-20260923.md).
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
 * (hw/misc/sgi-odyssey.c, docs/analysis/odyssey-colormap-format.md).
 * ODSY_BACKEND_DISPMODE_DID1 selects drawable 1's display-mode register
 * (index 0x2901, count 1); its data word ODSY_DISPMODE_GROUP17 names group
 * 17, the only populated colormap in the trace, at base 0x1100.
 * ODSY_BACKEND_CMAP_HDR opens a three-entry record at that base, and
 * ODSY_BACKEND_PAD is a count-0 no-op record.
 */
#define ODSY_BACKEND_DISPMODE_DID1 0x3a404001u
#define ODSY_DISPMODE_GROUP17 0x004b5f10u
#define ODSY_BACKEND_CMAP_HDR 0x34400003u
#define ODSY_BACKEND_PAD 0x30000000u
#define ODSY_LUT_RED 0x3ff00000u
#define ODSY_LUT_GREEN 0x000ffc00u
#define ODSY_LUT_BLUE 0x000003ffu
#define ODSY_CMD_GL_COLOR_INDEX 0xc580c406u

/*
 * The drawable-id plane write path (docs/analysis/odyssey-account-panel-black-
 * band.md), redeclared here the same way every command word above is. The
 * X server's drawable-id plane write sends a 0x1017 destination-select
 * block whose third operand is ODSY_DEST_ID_PLANES, a 0x1023 plane-mask
 * block whose first operand is the write mask << 12, and a COLOR_INDEX
 * word carrying the drawable id << 3. A colour fill instead carries
 * ODSY_DEST_COLOUR_INDEX as the 0x1017 third
 * operand, which is what un-selects the id-plane path for the fills that
 * follow. ODSY_BACKEND_DISPMODE_DID24 selects drawable 24's display-mode
 * register (index 0x2918); ODSY_DISPMODE_GROUP18 names group 18 (base 0x1200).
 */
#define ODSY_CMD_DEST_SELECT 0x00011017u
#define ODSY_CMD_PLANE_MASK 0x00011023u
#define ODSY_DEST_ID_PLANES 0x20080080u
#define ODSY_DEST_COLOUR_INDEX 0x20086a00u
#define ODSY_BACKEND_DISPMODE_DID24 0x3a460001u
#define ODSY_DISPMODE_GROUP18 0x00000120u

/*
 * Crosstalk part 0xc013, manufacturer 0x023, revision nibble 0xa -- the
 * value a live V12 board (030-1611-001 rev C) answers
 * (odyssey-octane2-v12-registers-20260926.md), and the model's default.
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

static QTestState *board_start(const char *extra)
{
    g_autofree char *quoted = g_shell_quote(prom_path);

    return qtest_initf(MACHINE " -bios %s%s", quoted, extra);
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
    credit_emit(image, pos, (13 << 26) | (reg << 16) | (value >> 48));
    for (unsigned shift = 32; ; shift -= 16) {
        credit_emit(image, pos, (reg << 16) | (reg << 11) | (16 << 6) | 0x38);
        credit_emit(image, pos, (13 << 26) | (reg << 21) | (reg << 16) |
                    ((value >> shift) & 0xffff));
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
    credit_emit(image, &pos, 0x40086000); /* mfc0 t0, status */
    credit_emit(image, &pos, 0x35080080); /* enable 64-bit kernel addresses */
    credit_emit(image, &pos, 0x40886000); /* mtc0 t0, status */
    for (unsigned i = 0; i < 4; i++) {
        credit_emit(image, &pos, 0);
    }
    credit_li64(image, &pos, 16, UINT64_C(0x9000000000000000) | box);
    loop = pos;
    credit_emit(image, &pos, 0xde090000); /* ld t1, 0(s0): store count */
    credit_emit(image, &pos, 0x1120fffe); /* wait while count is zero */
    credit_emit(image, &pos, 0);
    credit_emit(image, &pos, 0xde0a0008); /* ld t2, 8(s0): target */
    credit_emit(image, &pos, 0xfd400000); /* sd zero, 0(t2) */
    credit_emit(image, &pos, 0x6529ffff); /* decrement count */
    credit_emit(image, &pos, 0x1520fffd); /* repeat store */
    credit_emit(image, &pos, 0);
    credit_emit(image, &pos, 0xfe000000); /* publish completion */
    branch = pos;
    credit_emit(image, &pos, 0x10000000 |
                (uint16_t)(((int)loop - (int)branch - 4) / 4));
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
    qtest_quit(qts);
}

static uint32_t reg_read(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, WIDGET + off);
}

static void reg_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_writel(qts, WIDGET + off, value);
}

/* The indexed register file's write port: (selector << 32) | data. */
static void i2c_push(QTestState *qts, uint32_t selector, uint32_t data)
{
    qtest_writeq(qts, WIDGET + ODSY_BACKEND_SELECT,
                 ((uint64_t)selector << 32) | data);
}

/*
 * The backend word stream (hw/misc/sgi-odyssey.c): an 8-byte store is two
 * words, upper half first; a 4-byte store is one word
 * (docs/analysis/odyssey-colormap-format.md).
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

static void odsy_test_pixel_write(QTestState *qts, uint32_t x, uint32_t y,
                                  uint32_t value)
{
    odsy_test_pixel_select(qts, x, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel', 'value': %u}}", BOARD, value);
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
 * drawable, display mode and lookup RAM (docs/analysis/odyssey-colormap-
 * scanout.md), so a colour-index pixel's colour is only visible here.
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
 * zero and the screen black.
 */
#define ODSY_DISPMODE_RGB 0x004b59a6u
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

static uint32_t odsy_screendump_pixel(QTestState *qts, uint32_t x, uint32_t y)
{
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
    g_assert_true(g_str_has_prefix(contents, "P6\n1280 1024\n255\n"));
    pixels = g_strstr_len(contents, length, "\n255\n");
    g_assert_nonnull(pixels);
    pixels += 5;
    g_assert_cmpuint(x, <, 1280);
    g_assert_cmpuint(y, <, 1024);
    g_assert_cmpuint((gsize)(pixels - contents) + 1280 * 1024 * 3, <=,
                     length);
    pixels += (y * 1280 + x) * 3;
    unlink(path);
    return ((uint8_t)pixels[0] << 16) | ((uint8_t)pixels[1] << 8) |
           (uint8_t)pixels[2];
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
    qtest_quit(qts);

    qts = board_start(" -global sgi-odyssey.revision=3");
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, 0x3c013047);
    qtest_quit(qts);
}

/*
 * A live V12 board reads back the same value at a configuration-block
 * register's base (the upper half) as at base+4 (the low half) across the
 * whole block, not only on the identity register
 * (odyssey-octane2-v12-registers-20260926.md). Pin that on identity and on
 * a second, unrelated register so the behavior is block-wide rather than
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

    qtest_quit(qts);
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

    qtest_quit(qts);
}

/*
 * Memory size, bank count and column-address latency reach the driver
 * through one register, and IRIX publishes the marketed board name from
 * the size field alone. The zero low bits are the outstanding-operation
 * count and the data-FIFO level the driver waits on at nearly every step.
 * Bit 24 is the unexplained "don't-care copy field" a live V12 board
 * reads as 0x1 (odyssey-octane2-v12-registers-20260926.md); the model
 * carries it as an unconditional constant, so it appears in every profile
 * below. The default profile (128 MB, two banks, latency 3) is that same
 * board's measured 0xa1000000.
 */
static void test_memory_configuration_follows_properties(void)
{
    QTestState *qts = board_start("");

    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0xa1000000);
    qtest_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=32");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x21000000);
    qtest_quit(qts);

    qts = board_start(" -global sgi-odyssey.memory-size=64"
                      " -global sgi-odyssey.memory-banks=2"
                      " -global sgi-odyssey.cas-latency=2");
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0x71000000);
    qtest_quit(qts);
}

/*
 * "profile" is a named preset for the generation/capacity pairs the family
 * evidence record supports (docs/reviews/odyssey-family-behaviour-evidence-
 * 20261001.md): v12 and v10 share the second-generation revision nibble
 * 0xa, v8 and v6 the first generation's 0x1; v12 and v8 report 128 MB,
 * v10 and v6 32 MB. Only the v12 pair -- a measured live board
 * (odyssey-octane2-v12-registers-20260926.md) -- is independently grounded
 * hardware evidence; the other three are the same bit-field rules applied
 * to generation and capacity values the evidence record establishes
 * separately, not a second measured board, so this still checks them for
 * a regression, not as a hardware-accuracy claim.
 */
static void test_profile_selects_identity_and_memory(void)
{
    static const struct {
        const char *profile;
        uint32_t identity;
        uint32_t memory_config;
    } cases[] = {
        { "v12", IDENTITY_DEFAULT, 0xa1000000 },
        { "v10", IDENTITY_DEFAULT, 0x21000000 },
        { "v8",  0x1c013047,       0xa1000000 },
        { "v6",  0x1c013047,       0x21000000 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *extra =
            g_strdup_printf(" -global sgi-odyssey.profile=%s",
                            cases[i].profile);
        QTestState *qts = board_start(extra);

        g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, cases[i].identity);
        g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==,
                         cases[i].memory_config);
        qtest_quit(qts);
    }
}

/*
 * Bits 29 (two memory banks) and 28 (cas latency 2) of the memory
 * configuration word, decomposed from the V12 profile's measured
 * 0xa1000000 (odyssey-octane2-v12-registers-20260926.md): two banks (bit
 * 29 set), latency 3 rather than 2 (bit 28 clear). Bits 27:24 carry the
 * constant "copy field" (ODSY_CONFIG_COPY_FIELD), that same board's only
 * established value for it.
 */
static void test_v12_profile_memory_word_bits(void)
{
    QTestState *qts = board_start(" -global sgi-odyssey.profile=v12");
    uint32_t config = reg_read(qts, ODSY_MEMORY_CONFIG);

    g_assert_cmphex(config, ==, 0xa1000000);
    g_assert_cmphex(config & (1u << 29), ==, (1u << 29));
    g_assert_cmphex(config & (1u << 28), ==, 0);
    g_assert_cmphex((config >> 24) & 0xf, ==, 0x1);
    qtest_quit(qts);
}

/*
 * The driver's six-write memory-reset sequence (docs/datasheets/odyssey.md,
 * "0x1004"). Each write is accepted and logged by name under -d unimp,
 * rather than falling through to the generic "not modeled" message, and
 * the board keeps answering identity afterwards: the model does not reset
 * anything real, it only stops pretending the register does not exist.
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
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "0x1004 is not modeled"));
    g_assert_nonnull(strstr(log, "reset control register accepted"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * gfxinfo's displayed back-end version at 0x23380c: established as read,
 * pinned at 0 for every profile as a documented approximation rather than
 * left to the generic unimplemented fallback (see the model's
 * ODSY_BACKEND_VERSION comment). A write is a guest error, the same as
 * every other read-only register in this block.
 */
static void test_backend_version_is_pinned(void)
{
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

    g_assert_cmphex(qtest_readl(qts, WIDGET + ODSY_BACKEND_VERSION), ==, 0);
    qtest_writel(qts, WIDGET + ODSY_BACKEND_VERSION, 1);
    g_assert_cmphex(qtest_readl(qts, WIDGET + ODSY_BACKEND_VERSION), ==, 0);
    qtest_quit(qts);

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
    extra = g_strdup_printf(" -global sgi-odyssey.profile=v10"
                            " -d guest_errors -D %s", quoted);
    qts = board_start(extra);

    /*
     * A destination select's base is a board address in 16-byte units
     * (docs/datasheets/odyssey.md's command-stream table), so this
     * profile's 32 MiB is 0x200000 units: 0x200000 itself is the first
     * out-of-range value.
     */
    cmd_push(qts, ODSY_CMD_DEST_SELECT);
    cmd_push(qts, 0x200000);
    cmd_push(qts, 16);
    cmd_push(qts, 0);
    cmd_push(qts, 0);
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "destination base 0x200000 is beyond the "
                                 "32 MiB this board reports"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
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
    unsigned int i;

    g_assert_false(NIC_RESET(qts));         /* pulled low: the part answers */
    nic_write_byte(qts, 0xcc);              /* one part on the bus: skip ROM */
    nic_write_byte(qts, 0xf0);              /* read memory, address 0 */
    nic_write_byte(qts, 0x00);
    nic_write_byte(qts, 0x00);
    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }

    /*
     * A format byte, four spaces and the delimiter a scanning reader
     * needs before the first field name, then the record itself.
     */
    g_assert_cmphex(page[0], ==, 0x01);
    g_assert_cmphex(page[5], ==, ';');
    g_assert_cmpmem(&page[6], 20, "Name:ODY128VERSIONB;", 20);

    qtest_quit(qts);
}

/*
 * The flag, enable and acknowledge trio, and the rule that an enable on
 * its own raises nothing unless a source is already pending. There is no
 * sysbus IRQ line to intercept any more: a transition sends a crosstalk
 * interrupt-packet to whatever destination int_dest_upper/int_dest_lower
 * name (test_interrupt_packet_composition and
 * test_interrupt_packet_reaches_bedrock_pending below pin that packet's
 * shape and delivery), and x-test-last-interrupt-value's
 * ODSY_HUB_INT_MODIFY_SET bit is this test's stand-in for "asserted".
 */
static void test_interrupt_registers(void)
{
    QTestState *qts = board_start("");

    /* A destination programmed once, as real firmware would before enabling. */
    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);

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
    reg_write(qts, ODSY_SW_SYNC, 0x5a5a5a5a);
    g_assert_cmphex(reg_read(qts, ODSY_SW_SYNC), ==, 0x5a5a5a5a);

    qtest_quit(qts);
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

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);

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

    qtest_quit(qts);
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

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);
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

    qtest_quit(qts);
}

/*
 * The driver writes the flags register itself (the kernel driver's FIFO
 * high-water handler stores 0x200, a diagnostic clears bit 11 by
 * read-modify-write) with an effect no source settles. The model accepts
 * the write, reports it, and leaves the flags and the level as they were.
 */
static void test_interrupt_flags_write_is_accepted(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);
    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    reg_write(qts, ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);

    reg_write(qts, ODSY_INTR_FLAGS, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS), ==, 0);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    qtest_quit(qts);
}

/*
 * Low water where the driver has disabled it: a later submission latches the
 * flag, as the drained FIFO would, but sends no interrupt until the high-water
 * path enables the source again, and then it asserts at once.
 */
static void test_lowater_not_raised_while_disabled(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);
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
                    ODSY_INTR_FIFO_LOW_WATER, ==, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR);

    reg_write(qts, ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    qtest_quit(qts);
}

/*
 * The board status register the kernel's periodic gfx-credits check reads
 * (odsy status0, 0x1064). This model's command FIFO drains synchronously, so
 * it always reports enabled, empty and below the low-water mark. Reading a
 * clear low-water or enabled bit here — as the unmodeled default did,
 * answering zero — makes the driver log "odsy board 0: Graphics error" and
 * take its board-reset recovery path.
 */
static void test_board_status_reports_fifo_ready(void)
{
    QTestState *qts = board_start("");

    /* The FIFO-enabled, low-water and empty bits. */
    g_assert_cmphex(reg_read(qts, ODSY_STATUS0), ==, 0x10030000);

    qtest_quit(qts);
}

/*
 * The command-FIFO low-water interrupt the driver waits on to replenish its
 * graphics credits. The empty FIFO leaves it pending out of reset, so enabling
 * the source asserts at once; acknowledging clears it; and a later command
 * submission (the drain edge) raises it again. Without the reset-pending latch
 * the kernel's post-reset watchdog never sees it and gives up starting Xsgi.
 * There is no sysbus IRQ line any more (see test_interrupt_registers above):
 * this pins the same edges through the packet value's set bit instead.
 */
static void test_command_fifo_lowater_interrupt(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);

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

    /* A later command submission drains the FIFO and raises it again. */
    qtest_writel(qts, WIDGET + ODSY_CMD_FIFO, 0x00010046);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_FLAGS) &
                    ODSY_INTR_FIFO_LOW_WATER, ==, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    qtest_quit(qts);
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

    /* Programming the destination alone sends nothing without a new edge. */
    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);
    g_assert_cmphex(reg_read(qts, ODSY_INT_DEST_UPPER), ==,
                    ODSY_TEST_INT_DEST_UPPER);
    g_assert_cmphex(reg_read(qts, ODSY_INT_DEST_LOWER), ==,
                    ODSY_TEST_INT_DEST_LOWER);
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

    /* A drain event's rising edge readdresses the same destination, setting it. */
    qtest_writel(qts, WIDGET + ODSY_CMD_FIFO, ODSY_CMD_STROBE);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==,
                    ODSY_TEST_PACKET_FABRIC_ADDRESS);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-value"), ==,
                    ODSY_TEST_VECTOR | ODSY_HUB_INT_MODIFY_SET);

    qtest_quit(qts);
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

    reg_write(qts, ODSY_INT_DEST_UPPER, ODSY_TEST_INT_DEST_UPPER);
    reg_write(qts, ODSY_INT_DEST_LOWER, ODSY_TEST_INT_DEST_LOWER);

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

    qtest_quit(qts);
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

    qtest_quit(qts);
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
    const char *at;
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
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    at = strstr(log, "4-byte read at 0x110000 is not modeled");
    g_assert_nonnull(at);
    /* Once per offset, however many times the guest touches it. */
    g_assert_null(strstr(at + 1, "4-byte read at 0x110000 is not modeled"));
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

    qtest_quit(qts);
}

static void test_reset_restores_the_documented_values(void)
{
    QTestState *qts = board_start("");

    reg_write(qts, ODSY_CONTROL, 0xdeadbeef);
    reg_write(qts, ODSY_INTR_ENABLE_SET, 0xffffffff);
    reg_write(qts, ODSY_SW_SYNC, 0x12345678);
    reg_write(qts, ODSY_NIC, NIC_PULSE(520) | NIC_SAMPLE(65));
    reg_write(qts, ODSY_XT_REQ_TIMEOUT, 0xdeadbeef);
    reg_write(qts, ODSY_FLCTL_CONFIG, 0xdeadbeef);
    reg_write(qts, ODSY_BUZZ_1, 0xdeadbeef);
    reg_write(qts, ODSY_BUZZ_6, 0xdeadbeef);

    qtest_system_reset(qts);

    g_assert_cmphex(reg_read(qts, ODSY_CONTROL), ==, SLOT);
    g_assert_cmphex(reg_read(qts, ODSY_INTR_ENABLE_SET), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_SW_SYNC), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_NIC), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_XT_REQ_TIMEOUT), ==,
                     XT_REQ_TIMEOUT_RESET);
    g_assert_cmphex(reg_read(qts, ODSY_FLCTL_CONFIG), ==, FLCTL_CONFIG_RESET);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_1), ==, 0);
    g_assert_cmphex(reg_read(qts, ODSY_BUZZ_6), ==, 0);
    /* Reset does not disturb what the properties compose. */
    g_assert_cmphex(reg_read(qts, ODSY_IDENTITY), ==, IDENTITY_DEFAULT);
    g_assert_cmphex(reg_read(qts, ODSY_MEMORY_CONFIG), ==, 0xa1000000);

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    /* The screen (id plane 0, shown through drawable 1) in an RGB mode. */
    display_identity(qts, 1);

    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 5), ==, 0);

    odsy_test_pixel_write(qts, 5, 5, 0x11223344);
    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0x11223344);
    g_assert_cmphex(odsy_test_display_pixel(qts, 5, 5), ==, 0x00112233);
    g_assert_cmphex(odsy_screendump_pixel(qts, 5, 5), ==, 0x00112233);

    /* An untouched pixel elsewhere stays at its post-reset value. */
    g_assert_cmphex(odsy_test_pixel(qts, 6, 5), ==, 0);
    g_assert_cmphex(odsy_screendump_pixel(qts, 6, 5), ==, 0);

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
}

/*
 * odyssey_fillrect(): BEGIN(QUADS), a color, four corner vertices and END,
 * closed out with the quick flush a driver always issues after one draw
 * (docs/research/graphics/odyssey-openbsd-public-contract-20260922.md
 * section 4). The command FIFO is the only path that paints outside the
 * test-only pixel accessor, so this is also the first proof that path
 * reaches the framebuffer at all.
 */
/*
 * The kernel's window-placement block (docs/analysis/odyssey-gl-window-
 * placement.md), pushed on the kernel port the way the kernel driver's
 * clip-rectangle push does after every context switch: 0x841 with the
 * window origin, 0x840 with the constant and the drawable-id compare word
 * in one 8-byte store, and 0x204b with the four clip rectangles, one
 * 8-byte store each. A rectangle is x0 << 12 | y0 then x1 << 12 | y1,
 * inclusive; (1, 1) .. (0, 0) is empty.
 */
#define ODSY_CMD_WINDOW_ORIGIN 0x00010841u
#define ODSY_CMD_DID_COMPARE 0x00010840u
#define ODSY_CMD_WINDOW_CLIP 0x0001204bu
#define ODSY_CLIP_WORD(x, y) (((uint32_t)(x) << 12) | (y))
#define ODSY_CLIP_EMPTY_LO 0x00001001u
#define ODSY_DID_COMPARE_ON 0x00010000u

static void window_origin(QTestState *qts, uint32_t x, uint32_t y)
{
    cmd_push(qts, ODSY_CMD_WINDOW_ORIGIN);
    cmd_push(qts, x);
    cmd_push(qts, y);
}

static void did_compare(QTestState *qts, uint32_t word)
{
    cmd_push(qts, ODSY_CMD_DID_COMPARE);
    cmd_push64(qts, 0x142000, word);
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
    /* RGB shows through an RGB mode (id plane 0 takes drawable 1's). */
    display_identity(qts, 1);
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

    qtest_quit(qts);
}

/*
 * The same fillrect sequence test_command_fifo_fillrect drives, paired
 * two words per store instead of one word per store. The command FIFO
 * accepts 64-bit stores too (docs/datasheets/odyssey.md qtest checklist
 * item 21): each carries the next two command words, upper half first
 * (odyssey_cmd_fifo_write(), hw/misc/sgi-odyssey.c) -- an approximation,
 * since no allowed source independently establishes which half is
 * logically first. Driving the identical sequence this way must reach
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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
    qtest_quit(qts);
}

/*
 * A screen-to-screen copy is not clipped by the window-clip rectangles
 * (docs/reviews/odyssey-window-move-copy-diagnosis-20261001.md, its
 * evidence review): IRIX 6.5.30's X server issues copies whose
 * destinations lie wholly outside the rectangles in force and expects
 * them to land, which is what moving a window relies on. Both the X
 * server's own tail words and OpenBSD's literal ones
 * (test_command_fifo_copyrect's) must still land under a clip disjoint
 * from the destination. The drawable-id compare still bounds a copy, and
 * an ordinary fill is still clipped: the fix must drop only the
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
     * The id compare still bounds a copy: back to a full-screen clip,
     * enable the compare for an id the destination's id plane does not
     * hold (reset is 0), and the destination stays untouched.
     */
    window_clip(qts, (const uint32_t[][4]) { { 0, 0, 1279, 1023 } }, 1);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            odsy_test_pixel_write(qts, dx + x, dy + y, 0);
        }
    }
    did_compare(qts, ODSY_DID_COMPARE_ON | 0xf800 | (5 << 3));
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
    did_compare(qts, 0);

    g_assert_cmphex(odsy_test_pixel(qts, dx, dy), ==, 0);

    /*
     * Control: an ordinary fill is still clipped by the disjoint
     * rectangle -- the fix must not drop the rectangle test everywhere.
     */
    window_clip(qts, (const uint32_t[][4]) { { 400, 500, 549, 649 } }, 1);
    rgb_fill(qts, 0x44, 0x55, 0x66, 100, 100, 110, 110);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, 0);

    qtest_quit(qts);
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
    qtest_quit(qts);

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
                 BEDROCK_MD_INSTALLED_SELECT2);

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

    qtest_quit(qts);
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
                 BEDROCK_MD_INSTALLED_SELECT2);

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

    qtest_quit(qts);

    /*
     * A descriptor whose entries disagree with its header -- one entry
     * claiming more bytes than the header's rectangle -- fails the transfer
     * before anything lands. The board still consumed the descriptor, so
     * the DMA-done source the header asks for fires (hw/misc/sgi-odyssey.c,
     * odyssey_dma_done()).
     */
    qts = board_start("");
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_SELECT2);
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

    qtest_quit(qts);

    /*
     * Before the driver programs the routing, a consumed descriptor sends
     * nothing: there is nowhere to send the DMA-done packet.
     */
    qts = board_start("");
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_SELECT2);
    qtest_memwrite(qts, table, tbl, sizeof(tbl));
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_qom_get_uint(qts, "x-test-last-interrupt-addr"), ==, 0);

    qtest_quit(qts);
}

/*
 * A DMA into the colour buffer (docs/analysis/odyssey-dma-image-header.md):
 * lay a table whose header names the buffer the X server's 0x1017 block
 * named, its rectangle, row length, row gap and pixel size, and check the
 * pixels land where the header says. Word 7 carries the interrupt request.
 */
static void dma_image_table(QTestState *qts, uint64_t table,
                            const uint32_t hdr[8], const uint64_t *pte,
                            unsigned n)
{
    uint8_t tbl[ODSY_DMA_TABLE_HEADER + 4 * 8];
    unsigned i;

    g_assert_cmpuint(n, <, 4);
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
    const uint8_t idx[4] = { 1, 2, 1, 2 };
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
                 BEDROCK_MD_INSTALLED_SELECT2);
    reg_write(qts, ODSY_ROUTE_DEST_UPPER, ODSY_TEST_ROUTE_DEST_UPPER);
    reg_write(qts, ODSY_ROUTE_DEST_LOWER, ODSY_TEST_ROUTE_DEST_LOWER);
    reg_write(qts, ODSY_ROUTE_VECTORS, ODSY_TEST_ROUTE_VECTORS);

    /* Drawable 1 -> group 17; index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
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
     * pixel's drawable (id plane 0 takes drawable 1's group 17).
     */
    qtest_memwrite(qts, page_a, idx, sizeof(idx));
    dma_image_table(qts, table, hdr8, pte8, 1);
    dma_descriptor(qts, ODSY_CMD_DMA_IMAGE, table);
    g_assert_cmphex(odsy_test_pixel(qts, 200, 310), ==, 1);
    g_assert_cmphex(odsy_test_pixel(qts, 201, 310), ==, 2);
    g_assert_cmphex(odsy_test_pixel(qts, 204, 310), ==, 0);
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
    g_assert_cmphex(odsy_shown(qts, 200, 310), ==, 0x00ff00ff);
    g_assert_cmphex(odsy_shown(qts, 201, 310), ==, 0x0000ffff);
    g_assert_cmphex(odsy_shown(qts, 202, 310), ==, 0x00ff00ff);
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

    qtest_quit(qts);
}

/*
 * The double-buffered swap (hw/misc/sgi-odyssey.c,
 * docs/analysis/odyssey-swap-wake-mechanism.md). The driver issues a swap by
 * writing ODSY_SWAP_ARM, a per-window toggle mask, and then waits for its
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
    qtest_quit(qts);
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

    qtest_quit(qts);

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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);

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
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "1-byte read at 0x8 is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * The kernel driver's context switch (docs/datasheets/odyssey.md's "context
 * shadow address" and "per-context command port"): a single 8-byte store
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
                 BEDROCK_MD_INSTALLED_SELECT2);

    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_LOWER, (uint32_t)base);

    g_assert_cmphex(qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET), ==,
                    0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT, ODSY_CTX_SWITCH_WORD);
    g_assert_cmphex(qtest_readl(qts, base + ODSY_CTX_SWITCH_DONE_OFFSET), !=,
                    0);

    qtest_quit(qts);
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
    qtest_quit(qts);

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
 * The colormap word stream (docs/analysis/odyssey-colormap-format.md). The X
 * server loads the PseudoColor palette through ODSY_BACKEND_SELECT as counted
 * records: a header word names an entry index and a count, then that many
 * data words land in consecutive lookup-RAM entries. This drives the note's
 * own suggested contract test -- a three-entry record at base 0x1100 split
 * across two 8-byte stores and closed by a 4-byte count-0 pad -- and proves
 * the whole indexed-fill path: a fill through drawable 1 (whose display-mode
 * selects group 17 at base 0x1100) resolves each index to the colour just
 * loaded. It also pins the critical constraint that a following count-1 I2C
 * selector still reaches the I2C command register, unchanged by the rework.
 */
static void test_backend_colormap_word_stream(void)
{
    QTestState *qts = board_start("");

    /* Drawable 1's display-mode word selects colormap group 17 (base 0x1100). */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);

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

    qtest_quit(qts);
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
 * The drawable-id plane and per-pixel colour resolution
 * (docs/analysis/odyssey-account-panel-black-band.md). The X server's
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

    qtest_quit(qts);
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
 * The one-bit pattern-quad text path (docs/analysis/odyssey-glyph-raster-
 * format.md). The greeter draws every text label this way: load a 32x32-bit
 * bitmap through the 0x3c4e/0x3c4f register bursts, select the 32x32 shape with
 * 0x043f, enable pattern mode with 0x0404 (bits 3 and 4), set a colour, then
 * draw the string's bounding box as a QUADS fill. A set pattern bit paints the
 * colour; a clear bit leaves whatever the background held. This pins that: a
 * green pattern (index 1) drawn over a red background (index 0) shows green
 * where a bit is set and red where it is clear, with bit 31 the leftmost pixel,
 * row 0 the top row, and word v the row-v bits of the 32x32 shape.
 */
static void test_command_fifo_pattern_quad(void)
{
    QTestState *qts = board_start("");
    const uint32_t x0 = 200, y0 = 300;
    uint32_t bitmap[32] = { 0 };
    unsigned int u;

    /* Drawable 1 -> group 17 (base 0x1100); index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
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

    qtest_quit(qts);
}

/* Clipping a quad must keep the pattern anchored to its signed first corner. */
static void test_command_fifo_pattern_quad_straddles_screen(void)
{
    QTestState *qts = board_start("");
    uint32_t bitmap[32] = { [1] = 1u << 30 };

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
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
    qtest_quit(qts);
}

/*
 * The 2D stream's line and polygon primitives and multi-rectangle quads
 * (docs/analysis/odyssey-account-icons.md): the greeter's account icons and
 * bevels. A line covers the pixels whose centres lie strictly between its
 * endpoints along the major axis, [min, max), whichever way it runs, so an X
 * span pushed left to right (the X server's solid span routine) and right
 * to left (the X server's stippled span routine) paints the same pixels;
 * a strip paints each joint once and not its final vertex. With 0x0404
 * bits 3 and 4 set, a line is stippled by
 * the 32-bit 0x0c48 operand: bit k for the k-th covered pixel from the first
 * vertex, k = 1 next to it.
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
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
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
    for (i = 202; i <= 209; i++) {
        g_assert_cmphex(odsy_shown(qts, i, 310), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 210, 310), ==, ODSY_RED);
    g_assert_cmphex(odsy_shown(qts, 201, 310), ==, ODSY_RED);
    for (i = 212; i <= 219; i++) {
        g_assert_cmphex(odsy_shown(qts, i, 310), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 220, 310), ==, ODSY_RED);
    g_assert_cmphex(odsy_shown(qts, 211, 310), ==, ODSY_RED);
    for (i = 315; i <= 319; i++) {
        g_assert_cmphex(odsy_shown(qts, 205, i), ==, ODSY_GREEN);
    }
    g_assert_cmphex(odsy_shown(qts, 205, 320), ==, ODSY_RED);

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
    for (i = 204; i <= 211; i++) {
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

    qtest_quit(qts);
}

/* Extreme endpoints must finish and keep stipple phase at the first vertex. */
static void test_command_fifo_lines_cross_screen_edge(void)
{
    QTestState *qts = board_start("");

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
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
    qtest_quit(qts);
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

    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_null(strstr(log, "truncated"));
    g_assert_null(strstr(log, "is not modeled"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
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
 * BEGIN-0xa image raster, format 0x11: the account icons. A small box filled
 * with distinct 0x00RRGGBB pixels pins the raster order -- left to right,
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
    cmd_push(qts, 0x00778899);
    cmd_push(qts, 0x00aabbcc);
    cmd_push(qts, ODSY_CMD_GL_END);

    /* Raster order across the 2x2 box, each word its own colour, alpha opaque. */
    g_assert_cmphex(odsy_test_pixel(qts, 100, 100), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 100), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 101), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 101, 101), ==, 0xaabbccff);
    /* RGB shows through an RGB mode (id plane 0 takes drawable 1's). */
    display_identity(qts, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 100, 100), ==, 0x00112233);
    /* Just past the right edge and below the bottom edge stay untouched. */
    g_assert_cmphex(odsy_test_pixel(qts, 102, 100), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 100, 102), ==, 0);

    qtest_quit(qts);
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, 0x00778899);
    cmd_push(qts, 0x00aabbcc);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 19, 19), ==, 0x112233ff);
    g_assert_cmphex(odsy_test_pixel(qts, 18, 19), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 19, 18), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 18, 18), ==, 0xaabbccff);
    g_assert_cmphex(odsy_test_pixel(qts, 20, 20), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 17, 19), ==, 0);

    qtest_quit(qts);
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
    cmd_push(qts, 0x00778899);
    cmd_push(qts, 0x00aabbcc);
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
    cmd_push(qts, 0x00ddeeff);
    cmd_push(qts, 0x00123456);
    cmd_push(qts, 0x00654321);
    cmd_push(qts, 0x00abcdef);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 40, 39), ==, 0xddeeffff);
    g_assert_cmphex(odsy_test_pixel(qts, 41, 39), ==, 0x123456ff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 38), ==, 0x654321ff);
    g_assert_cmphex(odsy_test_pixel(qts, 41, 38), ==, 0xabcdefff);
    g_assert_cmphex(odsy_test_pixel(qts, 40, 40), ==, 0);

    qtest_quit(qts);
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
    cmd_push(qts, ODSY_CMD_GL_END);

    image_begin(qts, 55, 55, 57, 55);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, 0x00778899);
    cmd_push(qts, 0x00aabbcc);
    cmd_push(qts, ODSY_CMD_GL_END);

    image_begin(qts, 60, 60, 62, 61);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(2, 0x4c));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(2, ODSY_IMAGE_FMT_RGB));
    cmd_push(qts, 0x00778899);
    cmd_push(qts, 0x00aabbcc);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 50, 50), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 50, 51), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 55, 55), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 55), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 60, 60), ==, 0x778899ff);
    g_assert_cmphex(odsy_test_pixel(qts, 61, 60), ==, 0xaabbccff);

    qtest_quit(qts);
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_test_pixel(qts, 1279, 10), ==, 0x445566ff);
    g_assert_cmphex(odsy_test_pixel(qts, 1278, 10), ==, 0);

    qtest_quit(qts);
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
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
    cmd_push(qts, 0x00ff0000);
    cmd_push(qts, 0x0000ff00);
    cmd_push(qts, 0x000000ff);
    cmd_push(qts, 0x00ffffff);
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
    cmd_push(qts, 0x00ff0000);
    cmd_push(qts, 0x0000ff00);
    cmd_push(qts, 0x000000ff);
    cmd_push(qts, 0x00ffffff);
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
    cmd_push(qts, 0x00abcdef);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c000, 2, 3), ==,
                    0xabcdefff);
    g_assert_cmphex(odsy_test_object_texel(qts, 0x16c000, 10, 12), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 12), ==, 0);

    qtest_quit(qts);
}

/*
 * The header's format code, not the burst tag, lays out the bytes
 * (docs/analysis/odyssey-gl-texturing.md, section 3): 0x4b one 0xRRGGBBAA
 * word a pixel, 0x49 three packed bytes (a pixel may span two words), 0x42
 * one luminance byte. All three under the same 0x11 tag the X server's
 * 0x00RRGGBB icons use.
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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
    qtest_quit(qts);
}

/*
 * BEGIN-0xa image raster, format 0x10: four colour indices per word. This pins
 * the colormap lookup, the caret-erase approximation (index 0 leaves the
 * background rather than stamping opaque black over it), and a bounded group's
 * padding being dropped so it never wraps into the next row.
 */
static void test_command_fifo_image_raster_colour_index(void)
{
    QTestState *qts = board_start("");

    /* Drawable 1 -> group 17; index 0 red, 1 green, 2 blue. */
    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_PAD);

    /* A green background (index 1) under the whole region the images cover. */
    index_fill(qts, 1, 200, 200, 204, 204);
    index_fill(qts, 1, 300, 300, 302, 303);
    g_assert_cmphex(odsy_shown(qts, 200, 200), ==, 0x00ff00ff);

    /*
     * A 4x1 image whose one word packs indices 0, 2, 0, 2 (leftmost in bits
     * 31:24). Index 0 must leave the green background; index 2 paints blue.
     */
    image_begin(qts, 200, 200, 204, 201);
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER);
    cmd_push(qts, ODSY_IMAGE_HEADER_OP(4, 0x41));
    cmd_push(qts, ODSY_CMD_IMAGE_HEADER_TAIL);
    cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_CI));
    cmd_push(qts, 0x00020002);
    cmd_push(qts, ODSY_CMD_GL_END);

    g_assert_cmphex(odsy_shown(qts, 200, 200), ==, 0x00ff00ff); /* index 0 */
    g_assert_cmphex(odsy_shown(qts, 201, 200), ==, 0x0000ffff); /* index 2 */
    g_assert_cmphex(odsy_shown(qts, 202, 200), ==, 0x00ff00ff); /* index 0 */
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

    qtest_quit(qts);
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
static void test_command_fifo_gl_triangle(void)
{
    QTestState *qts = board_start("");

    push_window_transform(qts, odsy_captured_window_transform);
    push_captured_triangle(qts);

    /* The centroid (61, 78) is inside, in the plane and on the display. */
    g_assert_cmphex(odsy_test_pixel(qts, 61, 78), ==, 0xffff00ff);
    /* RGB shows through an RGB mode (id plane 0 takes drawable 1's). */
    display_identity(qts, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 61, 78), ==, 0x00ffff00);
    /* Just under the apex is inside; just under the base is not. */
    g_assert_cmphex(odsy_test_pixel(qts, 56, 20), ==, 0xffff00ff);
    g_assert_cmphex(odsy_test_pixel(qts, 56, 110), ==, 0);
    /* Outside the window and the triangle stays clear. */
    g_assert_cmphex(odsy_test_pixel(qts, 5, 5), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 613, 402), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 1275, 1019), ==, 0);

    qtest_quit(qts);
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
    /* RGB shows through an RGB mode (id plane 0 takes drawable 1's). */
    display_identity(qts, 1);
    g_assert_cmphex(odsy_test_display_pixel(qts, 64, 100), ==, 0x000000ff);
    g_assert_cmphex(odsy_test_pixel(qts, 128, 0), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 0, 128), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 128, 128), ==, 0);

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
}

/*
 * A 200x150 window at (300, 200), placed the way the kernel places the probe's
 * (docs/analysis/odyssey-gl-window-placement.md): origin (300, 200), one
 * rectangle (300, 200) .. (499, 349), the compare off. The distinct x and y
 * catch swapped origin operands, which the probe's (256, 256) cannot.
 */
static void window_300_200(QTestState *qts)
{
    window_origin(qts, 300, 200);
    did_compare(qts, 0);
    window_clip(qts, (const uint32_t[][4]) { { 300, 200, 499, 349 } }, 1);
    push_window_transform(qts, odsy_window_transform_200x150);
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

    qtest_quit(qts);
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

    qtest_quit(qts);
}

/*
 * The drawable-id compare. Id 12 owns (300, 200) .. (399, 349), written with
 * the X server's id-plane words (id << 3 under mask 0xf8). With the compare
 * enabled for id 12 -- 0x10000 | 0xf8 << 8 | 12 << 3 -- a whole-window clear
 * paints only the pixels id 12 owns; with it off, the whole window.
 */
static void test_command_fifo_gl_id_compare(void)
{
    QTestState *qts = board_start("");

    id_plane_write(qts, 12, 0xf8, 300, 200, 400, 350);
    window_300_200(qts);
    did_compare(qts, ODSY_DID_COMPARE_ON | 0xf800 | (12 << 3));
    push_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_200,
               ODSY_F_150);

    g_assert_cmphex(odsy_test_pixel(qts, 320, 300), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 399, 349), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 400, 300), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 470, 300), ==, 0);

    did_compare(qts, 0);
    push_clear(qts, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_200,
               ODSY_F_150);

    g_assert_cmphex(odsy_test_pixel(qts, 320, 300), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 470, 300), ==, ODSY_PLANE_BLUE);

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
    did_compare(qts, 0);
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

    qtest_quit(qts);
}

/*
 * Depth (docs/analysis/odyssey-gl-shading-depth-lighting.md): 0x0404 bit 16
 * the depth test, 0x0420 bits 2:0 the comparison, 0x0452 bit 24 the depth
 * write and bits 23:20 the colour write mask; glVertex3f is 0x8080cc03, and
 * glClear of the depth buffer is the colour clear's rectangle with the depth
 * write bit set and the clear depth, d x 2S, as its z.
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
        qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);
    probe_window(to);
    push_triangle_3f(to, ODSY_F_ONE, 0, 0, odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(to, 326, 340), ==, 0xbf0040ff);
    qtest_quit(to);
    qtest_quit(from);
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

    qtest_quit(qts);
}

/*
 * Alpha test (docs/reviews/odyssey-fragment-state-evidence-20261001.md):
 * 0x0404 bit 14 is GL_ALPHA_TEST, and 0x041e carries the function in bits
 * 14:12 (GL_NEVER = 0 through GL_ALWAYS = 7) and the reference in bits 11:0
 * under the depth comparison's own 12-bit rule (value / 4096.0).
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

    qtest_quit(qts);
}

/*
 * Blend and logic op (docs/reviews/odyssey-fragment-state-evidence-
 * 20261001.md): 0x0404 bit 17 is a computed GL_BLEND enable (the
 * library also parks 0x0c21 at all zeros whenever this bit is clear),
 * bits 19 and 20 are GL_INDEX_LOGIC_OP and GL_COLOR_LOGIC_OP, 0x0c21
 * carries the blend factors, equation and constant colour, and 0x0422
 * the logic op mode.
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
     * The library parks 0x0c21 at zero whenever bit 17 is clear. Disable
     * blending, send that parked all-zero word, then re-enable blending
     * without resending 0x0c21: the GL_ONE/GL_ZERO factors above must still
     * be in effect, proving the parked zero word was never applied.
     */
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT);
    set_blend(qts, 0, 0, 0);
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_BLEND);
    push_triangle_4f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ONE,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_GREEN);

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

    qtest_quit(qts);
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

    qtest_quit(qts);
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
        qtest_quit(qts);
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
            did_compare(qts, 0);
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
        qtest_quit(qts);
    }
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

    qtest_quit(qts);
}

/*
 * Lighting (docs/analysis/odyssey-gl-shading-depth-lighting.md, section 1,
 * and odyssey-gl-geometry-program.md, section 7). light.c's words: the front
 * material, the light-model ambient and light 0's colours (class 0x32) and
 * geometry (class 0x31, a directional light down +z), loaded under the
 * 0x19a stream entry as the library does, then its triangle with a normal
 * before each vertex. Lighting is on only when the geometry program the
 * library sets is a lit one; the model recognises it by its routine
 * addresses.
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

/* Constant memory: select index i, store a, as ValidateProgram writes it. */
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

    qtest_quit(qts);
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
 * Stencil (docs/reviews/odyssey-fragment-state-evidence-20261001.md): 0x0404
 * bit 15 is GL_STENCIL_TEST, and 0x041f carries the function in bits 27:25
 * (GL_NEVER = 0 through GL_ALWAYS = 7), the reference in bits 7:0, the value
 * mask in bits 15:8, and three 3-bit ops -- fail 24:22, depth-fail 21:19,
 * depth-pass 18:16 -- KEEP 0, ZERO 1, REPLACE 2, INCR 3, DECR 4, INVERT 5.
 *
 * This model has no stencil planes, so OpenGL's own rule makes the test
 * always pass no matter what is decoded here: there is no stencil buffer a
 * qtest could inspect to tell a modeled function or op from an unmodeled one
 * apart. A register-level test can and does confirm two things instead: a
 * clean library-shaped word (ALWAYS, REPLACE/REPLACE/REPLACE) decodes with
 * no complaint, and the two op codes the board never defines, 6 and 7, are
 * reported rather than silently treated as one of the five real ones.
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
    qtest_quit(qts);

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
 * With no stencil planes, enabling the test changes nothing: a function
 * that would reject every fragment on real hardware still lets this
 * triangle through, matching OpenGL's own rule for a stencil-less
 * framebuffer. This pins that contract; it does not discriminate a modeled
 * stencil test from an unmodeled one, since neither can block the draw.
 */
static void test_command_fifo_gl_stencil_always_passes(void)
{
    QTestState *qts = board_start("");

    probe_window(qts);
    push_depth_clear(qts, ODSY_F_ONE, ODSY_F_ZERO, ODSY_F_ZERO, ODSY_F_ZERO);
    set_reg(qts, ODSY_REG_GL_MODE, ODSY_GL_MODE_FLAT | ODSY_MODE_STENCIL_TEST);
    /* func NEVER, ops KEEP/KEEP/KEEP, value mask 0xff, ref 0. */
    set_reg(qts, ODSY_REG_STENCIL_FUNC, 0x0000ff00);
    push_triangle_3f(qts, ODSY_F_ZERO, ODSY_F_ONE, ODSY_F_ZERO,
                     odsy_near_yellow);
    g_assert_cmphex(odsy_test_pixel(qts, 326, 340), ==, ODSY_PLANE_GREEN);

    qtest_quit(qts);
}

/*
 * Cull face (docs/reviews/odyssey-fragment-state-evidence-20261001.md,
 * proposal test 6): 0x404 bit 8 is GL_CULL_FACE, bits 10:9 the face it
 * culls, 01 GL_BACK, 10 GL_FRONT. With GL_BACK on, the back-facing (CW)
 * triangle is dropped and the front-facing (CCW) one survives; with
 * GL_FRONT the surviving face swaps. The front/back determination this
 * exercises is the same one the polygon-mode test below depends on: a
 * wrong front-face sense here would swap which triangle each mode drops.
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

    qtest_quit(qts);
}

/*
 * Polygon mode (docs/reviews/odyssey-fragment-state-evidence-20261001.md):
 * 0x0415 packs front-face mode in bits 3:2 and back-face mode in bits 1:0,
 * POINT = 0, LINE = 1, FILL = 2. Front LINE, back FILL on a CCW (front-
 * facing) triangle draws no interior; the same triangle wound the other
 * way is now the back face and fills.
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

    qtest_quit(qts);
}

/*
 * Line width and point size (docs/reviews/odyssey-fragment-state-evidence-
 * 20261001.md): 0x084a packs the line width in operand 0 and the point
 * size in operand 1, each trunc(size * 128) in bits 23:0. Counting how
 * many pixels a horizontal line or a single point lights up, rather than
 * asserting exact positions, is sensitive to whether the register reached
 * the shared layer's line_width/point_size fields without depending on
 * the rasterizer's own sub-pixel rounding convention.
 */
#define ODSY_REG_LINE_POINT_SIZE 0x0001084a

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
    unsigned n = 0;
    uint32_t x, y;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            if (odsy_test_pixel(qts, x, y) == ODSY_PLANE_YELLOW) {
                n++;
            }
        }
    }
    return n;
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

    qtest_quit(qts);
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
    qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);
    probe_window(to);
    push_line_3f(to, ODSY_F_ONE, ODSY_F_ONE, ODSY_F_ZERO, line);
    g_assert_cmpuint(odsy_count_yellow(to, 290, 290, 319, 321), ==, 1);
    g_assert_cmpuint(odsy_count_yellow(to, 294, 294, 319, 321), ==, 0);
    g_assert_cmpuint(odsy_count_yellow(to, 300, 300, 319, 321), ==, 1);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * Polygon offset (docs/reviews/odyssey-fragment-state-evidence-20261001.md):
 * 0x0404 bit 7 is the fill-polygon GL_POLYGON_OFFSET_FILL enable, and 0x0458
 * is a signed 25-bit bias in bits 24:0, scaled by 2^-24 into normalized
 * window depth (EXT_polygon_offset). Two coplanar triangles under GL_LESS:
 * without the offset the second one's equal depth ties and loses; with a
 * negative bias large enough to be unambiguous, it wins.
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

    qtest_quit(qts);
}

/*
 * Clip box (docs/reviews/odyssey-fragment-state-evidence-20261001.md): 0x081d
 * packs two inclusive, window-relative fields per word, x_min/x_max then
 * y_min/y_max, each 12 bits, y counted down from the window's top row. It
 * reaches the shared layer as glScissor, with GL_SCISSOR_TEST left always on
 * (there is no enable bit; the library always sends the intersection it
 * wants drawn). A full-window fill restricted to the probe window's top
 * eight rows paints only that band; any min > max field, including the
 * library's own 0x1000, 0x1000 words, clips every fragment.
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

    qtest_quit(qts);
}

/*
 * Clip planes (docs/reviews/odyssey-fragment-state-evidence-20261001.md):
 * plane j is loaded at class 0x31 index 0x120 + 4j with 0x908091bb and four
 * eye-space floats, and 0x0405 bits 2:0 thermometer-code how many of the
 * loaded slots are tested (bit 2 set for n > 0). The probe window's combined
 * matrix is identity, so clip-space x is window-relative x scaled to
 * [-1, 1]; plane (1, 0, 0, 0) keeps only x >= 0, the window's right half.
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

    qtest_quit(qts);
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
 * The context save (docs/analysis/odyssey-context-state.md). At a context
 * switch the board writes the outgoing context's current colour and index
 * into the hardware shadow page named by 0x4004/0x400c, before the
 * completion word, and the kernel replays each saved word from the page at
 * the next switch-in with the operand count its bits 13:10 give. The model
 * saves the last index and colour command words with their raw operands, an
 * approximation of the unknown physical encoding. The page starts as the
 * kernel's template -- its words 0xce80c406 and 0xc580cc0d, zero colour and
 * index -- except that the pipe state is non-idle, so an idle reading proves
 * the board wrote it.
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);
    material_snapshot(to, 0x7f);
    assert_material(to, 14, back_material_commands,
                    defaults ? material_defaults : back_material_values);
    qtest_quit(from);
    qtest_quit(to);
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
    sgi_wait_migration_status(to, "completed", 30);
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
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(unlink(path), ==, 0);
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

    qtest_quit(qts);
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
    qtest_quit(qts);

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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(log_path), ==, 0);
    g_assert_cmpuint(count_substr(log, "context switch inside primitive 0x9"),
                     ==, 1);
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
    qtest_quit(qts);
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
    qtest_quit(qts);

    qts = board_start("");
    probe_window(qts);
    user_window_override(qts);
    user_cmd_push(qts, ODSY_CMD_WINDOW_RESTORE);
    push_captured_clear(qts);
    g_assert_cmphex(odsy_test_pixel(qts, 256, 256), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 383, 383), ==, ODSY_PLANE_BLUE);
    g_assert_cmphex(odsy_test_pixel(qts, 10, 10), ==, 0);
    g_assert_cmphex(odsy_test_pixel(qts, 384, 300), ==, 0);
    qtest_quit(qts);
}

/*
 * Texture uploads. The GL library overrides the window to (0, 0), selects
 * the texture level's board-memory object with 0x1017 (base in 16-byte
 * units, row length in 16-texel units, an 0x08... format word) and sends a
 * BEGIN-0xa image whose header's low byte gives the texel layout. The
 * texels land in the object, not on the screen, and 0x90008116 then
 * restores the kernel's window. The 0x1453 and 0x081d words are opaque to
 * the model; the "Texturing" section of docs/datasheets/odyssey.md lists
 * both among the words each textured draw or upload also pushes, and
 * leaves their contents unknown.
 */
static void upload_begin(QTestState *qts, uint32_t base, uint32_t wunits,
                         uint32_t fmt, uint32_t w, uint32_t h)
{
    const uint32_t words[] = {
        0x00011453, 1, 0, 0, 0, 0,
        ODSY_REG_GL_MODE, 0,
        0x00010405, 0x00002400,
        ODSY_CMD_WINDOW_ORIGIN, 0, 0,
        ODSY_CMD_WINDOW_CLIP, 0, 0x00ffffff, 0, 0, 0, 0, 0, 0,
        ODSY_CMD_DID_COMPARE, 0, 0,
        ODSY_CMD_DEST_SELECT, base, wunits, fmt, 0,
        ODSY_REG_WRITE_MASK, 0x01ffffff,
        0x0001081d, w, h,
        ODSY_CMD_IMAGE_STATE,
        ODSY_CMD_GL_BEGIN, ODSY_CMD_IMAGE_PRIMITIVE,
        ODSY_CMD_IMAGE_BOX, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
        0, 0, 0, coord_bits(w), coord_bits(h), 0,
        ODSY_CMD_GL_VERTEX_2I, 0, 0,
    };

    push_user_words(qts, words, ARRAY_SIZE(words));
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    qtest_quit(qts);
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
     * upload_begin()'s own 0x81d word was that upload's clip box (the
     * object's 8x8 extent), not a lingering window setting -- per the
     * evidence review, the register "arrives with every draw, enabled
     * or not". Restore it to the probe window's own full 128x128 extent
     * before this draw, the same way a real client's draw setup would.
     */
    clip_box_set(qts, 0, 127, 0, 127);
    push_user_words(qts, draw, ARRAY_SIZE(draw));
    g_assert_cmphex(odsy_test_pixel(qts, 370, 300), ==, 0xffffffffu);
    g_assert_cmphex(odsy_test_pixel(qts, 114, 44), ==, 0);
    qtest_quit(qts);

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
    qtest_quit(qts);
}

/*
 * GL_DITHER is 0x0404 bit 18 (docs/analysis/odyssey-gl-dither.md). With the
 * bit clear a mid-grey triangle (0.5, 127.5 a channel) is one value over an
 * aligned 4x4 block, and nothing is reported. With it set the model draws
 * the same, undithered, and reports that once however many draws follow:
 * the report proves the bit reaches the device state and follows 0x0404.
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
    qtest_quit(qts);

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
 * Colour resolves at scanout (docs/analysis/odyssey-colormap-scanout.md,
 * "Discriminating qtests"). The plane keeps the index or RGB the guest drew;
 * what the display shows follows the pixel's drawable, that drawable's
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
}

/*
 * Colour-index OpenGL (docs/analysis/odyssey-gl-demos.md, failure 1). A
 * context in index mode -- colour-mode words 0x22 = 1 and 0x23 = 3 -- draws
 * its current index, not an RGB colour: the plane keeps the raw index and the
 * display resolves it through the window's drawable, here drawable 24 whose
 * display mode names group 18 (lookup RAM 0x1200). glClearIndex travels as
 * the rectangle's first float, glIndexi as 0xc580c406, glIndexf as
 * 0xc580c407 with a float, and the kernel's replay forms as 0xce80c406 and
 * 0xce80c407. Back in RGBA mode the same triangle stores its RGB colour.
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
    qtest_quit(qts);
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

    qtest_quit(qts);
}

/*
 * Index write mask (docs/reviews/odyssey-fragment-state-evidence-
 * 20261001.md): 0x0452 bits 11:0, alongside the colour and depth write
 * masks the same register already carries. No retained capture shows what
 * a real colour-index drawable sends there, and the board's own reset
 * value leaves the bits at 0, not GL's own all-ones "fully open" mask, so
 * odyssey_gl_store_colour() decodes the field and reports whenever it
 * reads as anything but all-ones rather than guess at a masking rule no
 * evidence supports. Two known indices drawn under a non-all-ones mask
 * both still land unmasked, confirming the gap is reported, not silently
 * applied as a real mask would be.
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

    qtest_quit(qts);
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
    did_compare(qts, 0);
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

    qtest_quit(qts);
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
    qtest_quit(qts);

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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    did_compare(qts, ODSY_DID_COMPARE_ON | 0xf800 | (24 << 3));
    colour_index_fill(qts, 5, 10, 10, 20, 20);
    g_assert_cmphex(odsy_test_pixel(qts, 271, 271), ==, 5);
    g_assert_cmphex(odsy_shown(qts, 271, 271), ==, 0x804020ff);
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
    did_compare(qts, 0);
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
    qtest_quit(qts);
}

static void context_switch_to(QTestState *qts, uint32_t id)
{
    qtest_writel(qts, ODSY_SHADOW_PAGE + ODSY_CTX_SWITCH_DONE_OFFSET, 0);
    qtest_writeq(qts, WIDGET + ODSY_CTX_SWITCH_PORT,
                 UINT64_C(0x0000070000000001) | (uint64_t)id << 16);
    g_assert_cmphex(qtest_readl(qts, ODSY_SHADOW_PAGE +
                                ODSY_CTX_SWITCH_DONE_OFFSET), ==, 1);
}

/* Synthetic instruction values must not match a recognised image body. */
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    g_assert_nonnull(strstr(log, "not a known image entry"));
    g_assert_null(strstr(log, "texture-mode lighting finish"));
}

/*
 * A 16-bit program-memory halfword push (odyssey_gl_program_halfword(),
 * hw/misc/sgi-odyssey.c): class 0x40, tag bit 15 set, the value's low ten
 * bits in the word's low ten bits and its high six bits at word bits
 * 23-28. ODSY_PROG() above only reaches an 11-bit value, too narrow for
 * the halfwords below.
 */
#define ODSY_PROG_HALFWORD(h) \
    (0x40000000u | (((uint32_t)(h) & 0xfc00u) << 13) | 0x8000u | \
     ((uint32_t)(h) & 0x3ffu))

/*
 * The twelve bytes of the one image entry odyssey_ucode_bodies[] in
 * hw/misc/sgi-odyssey.c recognises, big-endian halfword 0 first
 * (provenance recorded in docs/provenance.md). Each class-0x40 push lands
 * at the entry's last free halfword and works backward
 * (odyssey_gl_program_halfword()), so the six pushes below run from the
 * entry's last halfword to its first.
 */
static void push_known_program_entry(QTestState *qts)
{
    static const uint16_t body[6] = {
        0x06c4, 0x2000, 0x01e2, 0xc000, 0x0008, 0x0000,
    };
    int i;

    user_cmd_push(qts, ODSY_PROG(0x27, 0x738));
    for (i = 5; i >= 0; i--) {
        user_cmd_push(qts, ODSY_PROG_HALFWORD(body[i]));
    }
}

/*
 * The positive case test_command_fifo_gl_program_window_unknown lacks: the
 * image's own entry, uploaded at the program window's first slot, is
 * recognised and relocated to the lit, textured record it names
 * (ODSY_UCODE_LIT_POS2_TEX and its two companions), so the triangle is lit
 * instead of staying unlit, and the texture-mode finish the model does not
 * implement is logged once.
 */
static void test_command_fifo_gl_program_window_known(void)
{
    static const uint32_t list[] = { 0x481, 0x203, 0x4ca, 0x4c5 };
    char *log_path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts = lit_start(&log_path);

    push_known_program_entry(qts);
    push_constants(qts, list, ARRAY_SIZE(list));
    push_program(qts, 0, 0x738, 0x760, 0x787);
    push_user_words(qts, odsy_lit_triangle, ARRAY_SIZE(odsy_lit_triangle));
    g_assert_cmphex(odsy_test_pixel(qts, 272, 367), !=, 0xff0000ffu);

    log = lit_finish(qts, log_path);
    g_assert_null(strstr(log, "is not recognised"));
    g_assert_null(strstr(log, "not a known image entry"));
    g_assert_nonnull(strstr(log, "texture-mode lighting finish"));
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

    qtest_quit(qts);
}

/*
 * A pixel whose id plane is unwritten -- main id 0 -- resolves through
 * drawable 1's display mode, the approximation odyssey_resolve_row()
 * documents (ODSY_DID_ROOT_MODE), not through drawable 0's own mode. Drawable
 * 1 names group 17 (0x1100, index 2 blue) and drawable 0 group 18 (0x1200,
 * index 2 white); an index-2 pixel over a zero id plane shows blue.
 */
static void test_scanout_main_id_0_uses_drawable_1_mode(void)
{
    QTestState *qts = board_start("");

    backend_push64(qts, ODSY_BACKEND_DISPMODE_DID1, ODSY_DISPMODE_GROUP17);
    backend_push64(qts, ODSY_BACKEND_CMAP_HDR, ODSY_LUT_RED);
    backend_push64(qts, ODSY_LUT_GREEN, ODSY_LUT_BLUE);
    backend_push32(qts, ODSY_BACKEND_HDR(0x2900, 1));
    backend_push32(qts, ODSY_DISPMODE_GROUP18);
    backend_push32(qts, ODSY_BACKEND_HDR(0x1202, 1));
    backend_push32(qts, ODSY_LUT_RED | ODSY_LUT_GREEN | ODSY_LUT_BLUE);

    odsy_test_pixel_write(qts, 100, 100, 2);
    g_assert_cmphex(odsy_shown(qts, 100, 100), ==, ODSY_BLUE);

    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
}

static void test_command_fifo_gl_bitmap_rect_wrap_zero(void)
{
    const uint32_t pattern[32] = {
        [0] = 1u << 31,
        [1] = 1u << 29,
    };
    const uint32_t empty[32] = { 0 };
    QTestState *qts = board_start("");
    unsigned x, y;

    bitmap_rect(qts, 0, 0, 1, 10, 35, 80, 69);
    bitmap_rect_pattern(qts, pattern, 0x10);
    bitmap_rect(qts, 1, 0, 0, 10, 35, 80, 69);
    for (y = 35; y < 69; y++) {
        for (x = 10; x < 80; x++) {
            unsigned row = (y - 35) % 32;
            unsigned col = (x - 10) % 32;
            uint32_t expected = (row == 0 && col == 0) ||
                                (row == 1 && col == 2)
                                ? ODSY_PLANE_RED : ODSY_PLANE_BLUE;

            g_assert_cmphex(odsy_test_pixel(qts, x, y), ==, expected);
        }
    }
    bitmap_rect_pattern(qts, empty, 0x10);
    bitmap_rect(qts, 0, 1, 0, 10, 35, 80, 69);
    /* A zero pattern preserves both set and clear colour locations. */
    g_assert_cmphex(odsy_test_pixel(qts, 10, 35), ==, ODSY_PLANE_RED);
    g_assert_cmphex(odsy_test_pixel(qts, 11, 35), ==, ODSY_PLANE_BLUE);
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
                                    uint32_t stride_units, uint32_t level_word2)
{
    uint32_t sampler[7] = {
        0x08000000 | (ctz32(2) << 10) | (ctz32(height) << 5) | ctz32(depth),
        2, height, depth, 0x13000000, 0, 0,
    };
    uint32_t level[3] = { base, 0x41100000 | stride_units, level_word2 };
    const uint32_t env[5] = { 1, 0, 0, 0, 0 };
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

static void texture_adapter_upload_scalar(QTestState *qts, uint32_t base)
{
    upload_begin(qts, base, 2, 0x080024d0, 2, 2);
    upload_first_header(qts, 0x42);
    user_cmd_push(qts, ODSY_IMAGE_BURST(1, ODSY_IMAGE_FMT_RGB));
    user_cmd_push(qts, 0x004080c0);
    upload_end(qts);
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
     * its own draw's clip box, per the evidence review's "arrives with
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
                 BEDROCK_MD_INSTALLED_SELECT2);
    reg_write(qts, ODSY_CTX_SHADOW_ADDR_UPPER, 0);
    texture_adapter_upload(qts, 0x180000, pixels);
    texture_adapter_window(qts);
    texture_adapter_bind(qts, 0x180000, 0x13000000, 1);
    display_identity(qts, 1);
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    qtest_quit(qts);
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
    qtest_quit(qts);

    fd = g_file_open_tmp("odyssey-descriptor-store-XXXXXX", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(log_path);
    extra = g_strdup_printf(" -d guest_errors -D %s", quoted);
    qts = board_start(extra);
    /* Bank 0 is absent; bank 1 holds completion and saved attributes. */
    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 (UINT64_C(2) << 32) | UINT64_C(0x3200));
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
    qtest_quit(qts);
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
                 (UINT64_C(2) << 32) | UINT64_C(0x3200));
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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

    /* Unsupported filtering drops the draw before looking up this image. */
    texture_adapter_bind(qts, 0xdead00, 0x13002802, 1);
    odsy_test_pixel_write(qts, 22, 10, 0x12345678);
    texture_adapter_point(qts, 22, 10, 0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(qts, 22, 10), ==, 0x12345678);
    qtest_quit(qts);
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);
    /* Raw sampler words and backing objects survive without reupload. */
    texture_adapter_point(to, 11, 10, -0.25f, 0.25f);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0x00ffff80);
    qtest_quit(to);
    qtest_quit(from);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
        texture_adapter_bind_3d(qts, 0x190000, height, 4, 2, level_word2);
        for (unsigned z = 0; z < 4; z++) {
            texture_adapter_point_3d(qts, 10 + z, 10,
                                    0.25f, 0.25f, (z + 0.5f) / 4);
            g_assert_cmphex(odsy_test_pixel(qts, 10 + z, 10), ==,
                            slice_colours[z]);
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
        texture_adapter_bind_3d(qts, 0x191000, 2, 4, 2, 1);
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
    qtest_quit(qts);
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
    texture_adapter_bind_3d(from, 0x1a0000, 2, 4, 2, 1);
    texture_adapter_point_3d(from, 10, 10, 0.25f, 0.25f, 0.625f);
    g_assert_cmphex(odsy_test_pixel(from, 10, 10), ==, colours[2]);

    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(from, uri);
    to = board_start(" -incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(to, "completed", 30);

    /* The family restores R with the object and sampler state. */
    texture_adapter_current_point(to, 11, 10);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, colours[2]);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_context_save_texture_coordinate_3d(void)
{
    const uint64_t page = ODSY_SHADOW_PAGE;
    const uint32_t coordinate[3] = { 0x3f000000, 0x3f400000, 0x3f200000 };
    QTestState *qts = board_start("");

    qtest_writeq(qts, BEDROCK_NODE0 + BEDROCK_MD_MEMORY_CONFIG,
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);

    /* No sampler or environment reupload: family state rebuilds the draw. */
    texture_adapter_point(to, 11, 10, 0.75f, 0.75f);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0x112233ff);
    qtest_quit(to);
    qtest_quit(from);
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
    qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);
    /* No ST or matrix reupload: the family must restore both. */
    user_cmd_push(to, ODSY_CMD_GL_BEGIN);
    user_cmd_push(to, 0);
    user_cmd_push(to, ODSY_GL_VERTEX_2F);
    user_cmd_push(to, bitmap_rect_float(11.5f));
    user_cmd_push(to, bitmap_rect_float(10.5f));
    user_cmd_push(to, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_test_pixel(to, 11, 10), ==, 0xff000040);
    qtest_quit(to);
    qtest_quit(from);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    set_reg(qts, ODSY_REG_GL_MODE, 0);
    did_compare(qts, ODSY_DID_COMPARE_ON | 0xf800 | (3 << 3));
    rgba_rect(qts, 0, 0, 0, 0, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 311, 211), ==, 0x0a141e80);
    did_compare(qts, 0);
    rgba_rect(qts, 0, 0, 0, 0, 10, 10, 14, 14);
    g_assert_cmphex(odsy_test_pixel(qts, 311, 211), ==, 0x0a141e00);
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    did_compare(qts, ODSY_DID_COMPARE_ON | 0xf800 | (3 << 3));
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
    qtest_quit(qts);
}

/*
 * Separate colour buffers (hw/misc/sgi-odyssey.c, ODSY_REG_DRAW_MASK;
 * docs/datasheets/odyssey.md, "Buffer-selection software contract"). The
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

    display_identity(qts, 1);
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
    qtest_quit(qts);
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

    display_identity(qts, 1);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);

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
    qtest_quit(qts);

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
    qtest_quit(qts);
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
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(odsy_qom_get_uint(to, "x-test-draw-word"), ==,
                    0x80002000u);
    g_assert_cmphex(odsy_qom_get_uint(to, "x-test-read-word"), ==,
                    0x00002000u);
    g_assert_cmphex(odsy_buffer_pixel(to, 1, 4, 4), ==, 0x0000ffffu);
    g_assert_cmphex(odsy_buffer_pixel(to, 0, 4, 4), ==, 0);
    rgb_fill(to, 0xff, 0, 0, 0, 0, 8, 8);
    g_assert_cmphex(odsy_buffer_pixel(to, 1, 4, 4), ==, 0xff0000ffu);
    g_assert_cmphex(odsy_buffer_pixel(to, 0, 4, 4), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(unlink(path), ==, 0);
}

/*
 * A DMA transfer to the screen and a BEGIN 0xa image raster both reach the
 * buffer the drawing mask names and leave the other untouched.
 */
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
    cmd_push(qts, 0x00112233);
    cmd_push(qts, 0x00445566);
    cmd_push(qts, ODSY_CMD_GL_END);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 100, 100), ==, 0x112233ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 1, 101, 100), ==, 0x445566ffu);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 100, 100), ==, 0);
    g_assert_cmphex(odsy_buffer_pixel(qts, 0, 101, 100), ==, 0);
    qtest_quit(qts);
}

/*
 * Board-to-host transfers (hw/misc/sgi-odyssey.c, ODSY_DMA_TO_HOST; the
 * rules are the sanitized handoff of docs/reviews/odyssey-read-header-
 * encoding-evidence-20260930.md, software behaviour, not board
 * measurements). A header with word 7 bit 24 set reads its rectangle out of
 * the board into the table's entries. Every test seeds the host bytes and
 * their surroundings with a guard byte and checks each byte afterwards.
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
                 BEDROCK_MD_INSTALLED_SELECT2);
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
 * Direction is word 7 bit 24 alone. The X server's 24-bit visual header
 * with the 0x4c format byte reads a screen pixel back as one big-endian
 * 0x00RRGGBB word -- the layout the upload path decodes for that byte --
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
    expect[17] = 0xc0;
    expect[18] = 0xff;
    expect[19] = 0xee;
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
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0x112233ffu);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* A zero format byte with this word 4: refused, nothing written. */
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    hdr[7] = 0x33010000;
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    g_assert_cmphex(odsy_test_pixel(qts, 200, 300), ==, 0x112233ffu);

    qtest_quit(qts);
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

    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
        stl_be_p(&expect[16], shown_word >> 8);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

        /* Swap: the drawn frame is now the front. */
        reg_write(qts, ODSY_SWAP_ARM, 1);
        fire_retrace(qts);
        readback_run(qts, sentinel, pte, 1);
        stl_be_p(&expect[16], drawn);
        readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
        shown_word = drawn;
    }
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
     * layout is 0x00RRGGBB a pixel.
     */
    {
        uint8_t row0[12], row1[12];

        for (c = 0; c < 3; c++) {
            row0[4 * c] = 0;
            row0[4 * c + 1] = 2;
            row0[4 * c + 2] = c + 1;
            row0[4 * c + 3] = 0x77;
            row1[4 * c] = 0;
            row1[4 * c + 1] = 1;
            row1[4 * c + 2] = c + 1;
            row1[4 * c + 3] = 0x77;
        }
        memcpy(&expect_a[8], row0, 8);
        memcpy(&expect_b[0], row0 + 8, 4);
        memcpy(&expect_b[8], row1, 12);
    }
    readback_expect(qts, RB_PAGE_A + 0x3ff0, expect_a, sizeof(expect_a));
    readback_expect(qts, RB_PAGE_B, expect_b, sizeof(expect_b));
    g_assert_true(readback_dma_done(qts));
    qtest_quit(qts);
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
        stl_be_p(&px[4 * i], 0x00102030 + i * 0x010101);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);
}

/* The V6 scratch read follows a drawable-to-object copy, then a DMA read. */
static void test_readback_gl_rgba8_scratch_copy(void)
{
    QTestState *qts = readback_start("");
    uint32_t hdr[8] = {
        0x16bc00, 0, 0x20, 0x10002, 0x80013042, 0, 8, 0xb3010000,
    };
    const uint64_t pte[] = { ODSY_DMA_PTE(RB_PAGE_A + 16, 8) };
    const uint32_t colours[] = { 0x12345678, 0x90abcdef };
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
    qtest_quit(qts);
}

/*
 * An X index read (format byte 0x41) takes the plane word as an index only
 * where scanout does: the pixel's drawable (main id 0 here, which takes
 * drawable 1's mode) is in an index group and the word fits eight bits.
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
    dispmode_write(qts, 1, ODSY_DISPMODE_RGB);
    readback_guard(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));
    readback_run(qts, hdr, pte, 1);
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* An index group: the same word reads as index 0xff. */
    dispmode_write(qts, 1, ODSY_DISPMODE_GROUP17);
    readback_run(qts, hdr, pte, 1);
    expect[16] = 0xff;
    readback_expect(qts, RB_PAGE_A, expect, sizeof(expect));

    /* Group 0's 12-bit index above 0xff does not fit the byte. */
    dispmode_write(qts, 1, 0);
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
    qtest_quit(qts);

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
    qtest_quit(qts);
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
    qtest_quit(qts);
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
    qtest_quit(qts);

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

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    qtest_add_func("sn1/odyssey/rgba/texture-alpha",
                   test_rgba_texture_alpha);
    qtest_add_func("sn1/odyssey/rgba/disabled-colour-depth",
                   test_rgba_disabled_colour_depth);
    qtest_add_func("sn1/odyssey/rgba/read-bounds",
                   test_rgba_read_bounds);
    qtest_add_func("sn1/odyssey/rgba/clear-masks",
                   test_rgba_clear_masks);
    qtest_add_func("sn1/odyssey/rgba/material-masks",
                   test_rgba_material_masks);
    qtest_add_func("sn1/odyssey/rgba/pattern-origin-ownership",
                   test_rgba_pattern_origin_ownership);
    qtest_add_func("sn1/odyssey/rgba/index-transitions",
                   test_rgba_index_transitions);
    qtest_add_func("sn1/odyssey/rgba/four-float-alpha",
                   test_rgba_four_float_alpha);
    qtest_add_func("sn1/odyssey/identity-carries-part-and-revision",
                   test_identity_carries_part_and_revision);
    qtest_add_func("sn1/odyssey/configuration-block-mirrors-both-halves",
                   test_configuration_block_mirrors_both_halves);
    qtest_add_func("sn1/odyssey/widget-control-reads-back",
                   test_widget_control_reads_back);
    qtest_add_func("sn1/odyssey/memory-configuration-follows-properties",
                   test_memory_configuration_follows_properties);
    qtest_add_func("sn1/odyssey/profile-selects-identity-and-memory",
                   test_profile_selects_identity_and_memory);
    qtest_add_func("sn1/odyssey/v12-profile-memory-word-bits",
                   test_v12_profile_memory_word_bits);
    qtest_add_func("sn1/odyssey/reset-control-accepts-six-writes",
                   test_reset_control_accepts_six_writes);
    qtest_add_func("sn1/odyssey/backend-version-is-pinned",
                   test_backend_version_is_pinned);
    qtest_add_func("sn1/odyssey/board-memory-bound-is-a-guest-error",
                   test_board_memory_bound_is_a_guest_error);
    qtest_add_func("sn1/odyssey/board-identity-part-answers",
                   test_board_identity_part_answers);
    qtest_add_func("sn1/odyssey/interrupt-registers",
                   test_interrupt_registers);
    qtest_add_func("sn1/odyssey/interrupt-enable-set-and-clear",
                   test_interrupt_enable_set_and_clear);
    qtest_add_func("sn1/odyssey/interrupt-clear-acknowledges-flags",
                   test_interrupt_clear_acknowledges_flags);
    qtest_add_func("sn1/odyssey/interrupt-flags-write-is-accepted",
                   test_interrupt_flags_write_is_accepted);
    qtest_add_func("sn1/odyssey/lowater-not-raised-while-disabled",
                   test_lowater_not_raised_while_disabled);
    qtest_add_func("sn1/odyssey/board-status-reports-fifo-ready",
                   test_board_status_reports_fifo_ready);
    qtest_add_func("sn1/odyssey/command-fifo-lowater-interrupt",
                   test_command_fifo_lowater_interrupt);
    qtest_add_func("sn1/odyssey/interrupt-packet-composition",
                   test_interrupt_packet_composition);
    qtest_add_func("sn1/odyssey/interrupt-packet-reaches-bedrock-pending",
                   test_interrupt_packet_reaches_bedrock_pending);
    qtest_add_func("sn1/odyssey/baseio-prom-diagnostic-registers",
                   test_baseio_prom_diagnostic_registers);
    qtest_add_func("sn1/odyssey/unmodeled-offsets-are-visible",
                   test_unmodeled_offsets_are_visible);
    qtest_add_func("sn1/odyssey/command-fifo-selftest",
                   test_command_fifo_selftest);
    qtest_add_func("sn1/odyssey/reset-restores-documented-values",
                   test_reset_restores_the_documented_values);
    qtest_add_func("sn1/odyssey/i2c-opt-port-pll-register-write",
                   test_i2c_opt_port_pll_register_write);
    qtest_add_func("sn1/odyssey/i2c-opt-port-ddc-register-read",
                   test_i2c_opt_port_ddc_register_read);
    qtest_add_func("sn1/odyssey/i2c-reset-state-poll-terminates",
                   test_i2c_reset_state_poll_terminates);
    qtest_add_func("sn1/odyssey/framebuffer-pixel-reaches-scanout",
                   test_framebuffer_pixel_reaches_display_and_screendump);
    qtest_add_func("sn1/odyssey/reset-clears-the-framebuffer",
                   test_reset_clears_the_framebuffer);
    qtest_add_func("sn1/odyssey/reset-mid-primitive-does-not-hang",
                   test_reset_mid_primitive_does_not_hang);
    qtest_add_func("sn1/odyssey/framebuffer-pixel-accessor-bounds",
                   test_framebuffer_pixel_accessor_bounds);
    qtest_add_func("sn1/odyssey/scanout-wipe-repaint-reload",
                   test_scanout_wipe_repaint_reload);
    qtest_add_func("sn1/odyssey/scanout-display-mode-recolours",
                   test_scanout_display_mode_recolours);
    qtest_add_func("sn1/odyssey/scanout-rgb-through-gamma",
                   test_scanout_rgb_through_gamma);
    qtest_add_func("sn1/odyssey/scanout-paint-order",
                   test_scanout_paint_order);
    qtest_add_func("sn1/odyssey/command-fifo-fillrect",
                   test_command_fifo_fillrect);
    qtest_add_func("sn1/odyssey/backend-colormap-word-stream",
                   test_backend_colormap_word_stream);
    qtest_add_func("sn1/odyssey/command-fifo-id-plane-write",
                   test_command_fifo_id_plane_write);
    qtest_add_func("sn1/odyssey/command-fifo-accepts-64bit-stores",
                   test_command_fifo_accepts_64bit_stores);
    qtest_add_func("sn1/odyssey/command-fifo-copyrect",
                   test_command_fifo_copyrect);
    qtest_add_func("sn1/odyssey/command-fifo-copyrect-overlapping-shift",
                   test_command_fifo_copyrect_overlapping_shift);
    qtest_add_func("sn1/odyssey/command-fifo-copyrect-large-offscreen",
                   test_command_fifo_copyrect_large_offscreen);
    qtest_add_func("sn1/odyssey/command-fifo-copyrect-ignores-window-clip",
                   test_command_fifo_copyrect_ignores_window_clip);
    qtest_add_func("sn1/odyssey/command-fifo-copyrect-prefix-is-a-register",
                   test_command_fifo_copyrect_prefix_is_a_plain_register);
    qtest_add_func("sn1/odyssey/command-fifo-host-write-sync",
                   test_command_fifo_host_write_sync);
    qtest_add_func("sn1/odyssey/command-fifo-dma-page-table",
                   test_command_fifo_dma_page_table);
    qtest_add_func("sn1/odyssey/command-fifo-dma-image",
                   test_command_fifo_dma_image);
    qtest_add_func("sn1/odyssey/readback/direction", test_readback_direction);
    qtest_add_func("sn1/odyssey/readback/sentinel-drawable-relative",
                   test_readback_sentinel_drawable_relative);
    qtest_add_func("sn1/odyssey/readback/intermediate-block",
                   test_readback_intermediate_block);
    qtest_add_func("sn1/odyssey/readback/back-buffer",
                   test_readback_back_buffer);
    qtest_add_func("sn1/odyssey/readback/front-follows-displayed-buffer",
                   test_readback_front_follows_displayed_buffer);
    qtest_add_func("sn1/odyssey/readback/two-by-two-row-order",
                   test_readback_two_by_two_row_order);
    qtest_add_func("sn1/odyssey/readback/row-gap-bottom-up",
                   test_readback_row_gap_bottom_up);
    qtest_add_func("sn1/odyssey/readback/page-crossing",
                   test_readback_page_crossing);
    qtest_add_func("sn1/odyssey/readback/object-source",
                   test_readback_object_source);
    qtest_add_func("sn1/odyssey/readback/gl-rgba8-object",
                   test_readback_gl_rgba8_object);
    qtest_add_func("sn1/odyssey/readback/gl-rgba16-object",
                   test_readback_gl_rgba16_object);
    qtest_add_func("sn1/odyssey/readback/gl-rgba16-scratch-copy",
                   test_readback_gl_rgba16_scratch_copy);
    qtest_add_func("sn1/odyssey/readback/gl-rgba8-scratch-copy",
                   test_readback_gl_rgba8_scratch_copy);
    qtest_add_func("sn1/odyssey/readback/index-follows-display-mode",
                   test_readback_index_follows_display_mode);
    qtest_add_func("sn1/odyssey/readback/greeter-caret-cell",
                   test_readback_greeter_caret_cell);
    qtest_add_func("sn1/odyssey/readback/gl-layouts",
                   test_readback_gl_layouts);
    qtest_add_func("sn1/odyssey/readback/failures", test_readback_failures);
    qtest_add_func("sn1/odyssey/swap-retrace-vector",
                   test_swap_retrace_vector);
    qtest_add_func("sn1/odyssey/swap-arm-toggles-displayed-buffer",
                   test_swap_arm_toggles_displayed_buffer);
    qtest_add_func("sn1/odyssey/swap-arm-accumulates",
                   test_swap_arm_accumulates);
    qtest_add_func("sn1/odyssey/swap-immediate-set-and-clear",
                   test_swap_immediate_set_and_clear);
    qtest_add_func("sn1/odyssey/buffers-hold-opposite-colours",
                   test_buffers_hold_opposite_colours);
    qtest_add_func("sn1/odyssey/buffers-swap-shows-only-complete-frames",
                   test_buffers_swap_shows_only_complete_frames);
    qtest_add_func("sn1/odyssey/buffers-display-selection-per-window",
                   test_buffers_display_selection_per_window);
    qtest_add_func("sn1/odyssey/buffers-draw-selection-keeps-clip-and-masks",
                   test_buffers_draw_selection_keeps_clip_and_masks);
    qtest_add_func("sn1/odyssey/buffers-read-and-draw-words-are-independent",
                   test_buffers_read_and_draw_words_are_independent);
    qtest_add_func("sn1/odyssey/buffers-unsupported-mask-reported-once",
                   test_buffers_unsupported_mask_reported_once);
    qtest_add_func("sn1/odyssey/buffers-gl-triangle-follows-mask",
                   test_buffers_gl_triangle_follows_mask);
    qtest_add_func("sn1/odyssey/buffers-migration",
                   test_buffers_migration);
    qtest_add_func("sn1/odyssey/buffers-transfers-follow-mask",
                   test_buffers_transfers_follow_mask);
    qtest_add_func("sn1/odyssey/command-fifo-pattern-quad",
                   test_command_fifo_pattern_quad);
    qtest_add_func("sn1/odyssey/command-fifo-pattern-quad-straddles-screen",
                   test_command_fifo_pattern_quad_straddles_screen);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-rgb",
                   test_command_fifo_image_raster_rgb);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-descending",
                   test_command_fifo_image_raster_descending);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-mixed-directions",
                   test_command_fifo_image_raster_mixed_directions);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-empty-extent",
                   test_command_fifo_image_raster_empty_extent);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-clips-descending",
                   test_command_fifo_image_raster_clips_descending);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-window-origin",
                   test_command_fifo_image_raster_window_origin);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-formats",
                   test_command_fifo_image_raster_formats);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-short",
                   test_command_fifo_image_raster_short);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-short-incomplete",
                   test_command_fifo_image_raster_short_incomplete);
    qtest_add_func("sn1/odyssey/command-fifo-image-raster-colour-index",
                   test_command_fifo_image_raster_colour_index);
    qtest_add_func("sn1/odyssey/command-fifo-gl-crossing-eye",
                   test_command_fifo_gl_crossing_eye);
    qtest_add_func("sn1/odyssey/command-fifo-gl-triangle",
                   test_command_fifo_gl_triangle);
    qtest_add_func("sn1/odyssey/command-fifo-lines",
                   test_command_fifo_lines);
    qtest_add_func("sn1/odyssey/command-fifo-lines-cross-screen-edge",
                   test_command_fifo_lines_cross_screen_edge);
    qtest_add_func("sn1/odyssey/command-fifo-integer-primitives-stream",
                   test_command_fifo_integer_primitives_stream);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-reverse-y",
                   test_command_fifo_gl_bitmap_rect_reverse_y);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-disabled",
                   test_command_fifo_gl_bitmap_rect_disabled);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-origin-clip",
                   test_command_fifo_gl_bitmap_rect_origin_clip);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-wrap-zero",
                   test_command_fifo_gl_bitmap_rect_wrap_zero);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-unsupported",
                   test_command_fifo_gl_bitmap_rect_unsupported);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-large-anchor",
                   test_command_fifo_gl_bitmap_rect_large_anchor);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-zero-depth",
                   test_command_fifo_gl_bitmap_rect_zero_preserves_depth);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-depth-only",
                   test_command_fifo_gl_bitmap_rect_depth_only);
    qtest_add_func("sn1/odyssey/command-fifo-gl-bitmap-rect-index",
                   test_command_fifo_gl_bitmap_rect_index);
    qtest_add_func("sn1/odyssey/command-fifo-gl-clear-rect",
                   test_command_fifo_gl_clear_rect);
    qtest_add_func("sn1/odyssey/command-fifo-gl-clear-then-triangle",
                   test_command_fifo_gl_clear_then_triangle);
    qtest_add_func("sn1/odyssey/command-fifo-gl-window-origin-and-clip",
                   test_command_fifo_gl_window_origin_and_clip);
    qtest_add_func("sn1/odyssey/command-fifo-gl-clip-rectangles",
                   test_command_fifo_gl_clip_rectangles);
    qtest_add_func("sn1/odyssey/command-fifo-gl-ortho-combined-matrix",
                   test_command_fifo_gl_ortho_combined_matrix);
    qtest_add_func("sn1/odyssey/command-fifo-gl-texture-slot-leaves-geometry",
                   test_command_fifo_gl_texture_slot_leaves_geometry);
    qtest_add_func("sn1/odyssey/command-fifo-gl-id-compare",
                   test_command_fifo_gl_id_compare);
    qtest_add_func("sn1/odyssey/"
                   "command-fifo-gl-float-colour-integer-conversion",
                   test_command_fifo_gl_float_colour_integer_conversion);
    qtest_add_func("sn1/odyssey/byte-colour/conversion",
                   test_byte_colour_conversion);
    qtest_add_func("sn1/odyssey/byte-colour/override",
                   test_byte_colour_override);
    qtest_add_func("sn1/odyssey/byte-colour/index-guard",
                   test_byte_colour_index_guard);
    qtest_add_func("sn1/odyssey/byte-colour/drawable-id",
                   test_byte_colour_drawable_id);
    qtest_add_func("sn1/odyssey/"
                   "command-fifo-gl-byte-colour-float-and-replay",
                   test_command_fifo_gl_byte_colour_float_and_replay);
    qtest_add_func("sn1/odyssey/command-fifo-gl-float-colour-integer-vertices",
                   test_command_fifo_gl_float_colour_integer_vertices);
    qtest_add_func("sn1/odyssey/command-fifo-gl-colour-index",
                   test_command_fifo_gl_colour_index);
    qtest_add_func("sn1/odyssey/command-fifo-gl-colour-index-write-mask",
                   test_command_fifo_gl_colour_index_write_mask);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-window-unknown",
                   test_command_fifo_gl_program_window_unknown);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-window-known",
                   test_command_fifo_gl_program_window_known);
    qtest_add_func("sn1/odyssey/command-fifo-gl-combined-carries-modelview",
                   test_command_fifo_gl_combined_carries_modelview);
    qtest_add_func("sn1/odyssey/scanout-main-id-0-uses-drawable-1-mode",
                   test_scanout_main_id_0_uses_drawable_1_mode);
    qtest_add_func("sn1/odyssey/command-fifo-gl-flat-shading",
                   test_command_fifo_gl_flat_shading);
    qtest_add_func("sn1/odyssey/command-fifo-gl-fog-curve",
                   test_command_fifo_gl_fog_curve);
    qtest_add_func("sn1/odyssey/command-fifo-gl-fog-native-packets",
                   test_command_fifo_gl_fog_native_packets);
    qtest_add_func("sn1/odyssey/command-fifo-gl-fog-migration",
                   test_command_fifo_gl_fog_migration);
    qtest_add_func("sn1/odyssey/command-fifo-gl-dither",
                   test_command_fifo_gl_dither);
    qtest_add_func("sn1/odyssey/command-fifo-gl-depth-test",
                   test_command_fifo_gl_depth_test);
    qtest_add_func("sn1/odyssey/command-fifo-gl-alpha-test",
                   test_command_fifo_gl_alpha_test);
    qtest_add_func("sn1/odyssey/command-fifo-gl-blend",
                   test_command_fifo_gl_blend);
    qtest_add_func("sn1/odyssey/command-fifo-gl-stencil-state",
                   test_command_fifo_gl_stencil_state);
    qtest_add_func("sn1/odyssey/command-fifo-gl-stencil-always-passes",
                   test_command_fifo_gl_stencil_always_passes);
    qtest_add_func("sn1/odyssey/command-fifo-gl-cull-face",
                   test_command_fifo_gl_cull_face);
    qtest_add_func("sn1/odyssey/command-fifo-gl-polygon-mode",
                   test_command_fifo_gl_polygon_mode);
    qtest_add_func("sn1/odyssey/command-fifo-gl-line-point-size",
                   test_command_fifo_gl_line_point_size);
    qtest_add_func("sn1/odyssey/command-fifo-gl-line-stipple",
                   test_command_fifo_gl_line_stipple);
    qtest_add_func("sn1/odyssey/command-fifo-gl-line-stipple-migration",
                   test_command_fifo_gl_line_stipple_migration);
    qtest_add_func("sn1/odyssey/command-fifo-gl-polygon-offset",
                   test_command_fifo_gl_polygon_offset);
    qtest_add_func("sn1/odyssey/command-fifo-gl-clip-box",
                   test_command_fifo_gl_clip_box);
    qtest_add_func("sn1/odyssey/command-fifo-gl-clip-plane",
                   test_command_fifo_gl_clip_plane);
    qtest_add_func("sn1/odyssey/command-fifo-gl-depth-sign",
                   test_command_fifo_gl_depth_sign);
    qtest_add_func("sn1/odyssey/command-fifo-gl-depth-scale",
                   test_command_fifo_gl_depth_scale);
    qtest_add_func("sn1/odyssey/command-fifo-gl-depth-scale-fallback",
                   test_command_fifo_gl_depth_scale_fallback);
    qtest_add_func("sn1/odyssey/command-fifo-gl-colour-mask",
                   test_command_fifo_gl_colour_mask);
    qtest_add_func("sn1/odyssey/command-fifo-gl-depth-only-clear",
                   test_command_fifo_gl_depth_only_clear);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-record-lit",
                   test_command_fifo_gl_program_record_lit);
    qtest_add_func("sn1/odyssey/command-fifo-gl-integer-normals",
                   test_command_fifo_gl_integer_normals);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-record-unlit",
                   test_command_fifo_gl_program_record_unlit);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-list-lit",
                   test_command_fifo_gl_program_list_lit);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-list-lit-local-viewer",
                   test_command_fifo_gl_program_list_lit_local_viewer);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-list-lit-positional",
                   test_command_fifo_gl_program_list_lit_positional);
    qtest_add_func("sn1/odyssey/command-fifo-gl-lighting-uses-modelview",
                   test_command_fifo_gl_lighting_uses_modelview);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-two-sided",
                   test_command_fifo_gl_program_two_sided);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-texgen",
                   test_command_fifo_gl_program_texgen);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-texture-finish",
                   test_command_fifo_gl_program_texture_finish);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-list-unlit",
                   test_command_fifo_gl_program_list_unlit);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-record-clip-head",
                   test_command_fifo_gl_program_record_clip_head);
    qtest_add_func("sn1/odyssey/command-fifo-gl-program-unknown",
                   test_command_fifo_gl_program_unknown);
    qtest_add_func("sn1/odyssey/command-fifo-unknown-word-is-unimplemented",
                   test_command_fifo_unknown_word_is_unimplemented);
    qtest_add_func("sn1/odyssey/unimplemented-reports-independent-budgets",
                   test_unimplemented_reports_have_independent_budgets);
    qtest_add_func("sn1/odyssey/command-fifo-gl-window-restore",
                   test_command_fifo_gl_window_restore);
    qtest_add_func("sn1/odyssey/command-fifo-gl-texture-luminance",
                   test_command_fifo_gl_texture_luminance);
    qtest_add_func("sn1/odyssey/command-fifo-gl-texture-rgb",
                   test_command_fifo_gl_texture_rgb);
    qtest_add_func("sn1/odyssey/command-fifo-gl-texture-rgba",
                   test_command_fifo_gl_texture_rgba);
    qtest_add_func("sn1/odyssey/dma-texture-object",
                   test_dma_texture_object);
    qtest_add_func("sn1/odyssey/texture-adapter/rgba12-byte-upload",
                   test_texture_adapter_rgba12_byte_upload);
    qtest_add_func("sn1/odyssey/texture-adapter/nearest",
                   test_texture_adapter_nearest);
    qtest_add_func("sn1/odyssey/texture-adapter/nearest-clamp",
                   test_texture_adapter_nearest_clamp);
    qtest_add_func("sn1/odyssey/texture-adapter/nearest-clamp-border",
                   test_texture_adapter_nearest_clamp_border);
    qtest_add_func("sn1/odyssey/texture-adapter/clamp-border-state-restore",
                   test_texture_adapter_clamp_border_state_restore);
    qtest_add_func("sn1/odyssey/texture-adapter/explicit-level",
                   test_texture_chain_explicit_level);
    qtest_add_func("sn1/odyssey/texture-adapter/integer-quads",
                   test_texture_chain_integer_quads);
    qtest_add_func("sn1/odyssey/texture-adapter/one-dimensional-mode",
                   test_texture_one_dimensional_mode);
    qtest_add_func("sn1/odyssey/texture-adapter/height-one",
                   test_texture_chain_height_one);
    qtest_add_func("sn1/odyssey/texture-adapter/construction",
                   test_texture_chain_construction);
    qtest_add_func("sn1/odyssey/texture-adapter/binding-filters-matrix",
                   test_texture_adapter_binding_filters_matrix);
    qtest_add_func("sn1/odyssey/texture-adapter/components-environment",
                   test_texture_adapter_components_environment);
    qtest_add_func("sn1/odyssey/texture-adapter/scalar-components",
                   test_texture_adapter_scalar_components);
    qtest_add_func("sn1/odyssey/texture-adapter/luminance-alpha",
                   test_texture_adapter_luminance_alpha);
    qtest_add_func("sn1/odyssey/texture-adapter/level-zero-3d",
                   test_texture_adapter_level_zero_3d);
    qtest_add_func("sn1/odyssey/texture-adapter/3d-state-restore",
                   test_texture_adapter_3d_state_restore);
    qtest_add_func("sn1/odyssey/context-save-texture-coordinate-3d",
                   test_context_save_texture_coordinate_3d);
    qtest_add_func("sn1/odyssey/texture-adapter/blend-environment",
                   test_texture_adapter_blend_environment);
    qtest_add_func("sn1/odyssey/texture-adapter/blend-state-restore",
                   test_texture_adapter_blend_state_restore);
    qtest_add_func("sn1/odyssey/texture-adapter/storage-dimensions",
                   test_texture_adapter_storage_dimensions);
    qtest_add_func("sn1/odyssey/texture-adapter/state-restore",
                   test_texture_adapter_state_restore);
    qtest_add_func("sn1/odyssey/texture-adapter/unsupported-missing",
                   test_texture_adapter_unsupported_missing);
    qtest_add_func("sn1/odyssey/command-fifo-gl-texture-state",
                   test_command_fifo_gl_texture_state);
    qtest_add_func("sn1/odyssey/back-material-migration",
                   test_back_material_migration);
    qtest_add_func("sn1/odyssey/back-material-migration-defaults",
                   test_back_material_migration_defaults);
    qtest_add_func("sn1/odyssey/query-record-migration",
                   test_query_record_migration);
    qtest_add_func("sn1/odyssey/query-record-migration-defaults",
                   test_query_record_migration_defaults);
    qtest_add_func("sn1/odyssey/context-save-texture-coordinate",
                   test_context_save_texture_coordinate);
    qtest_add_func("sn1/odyssey/context-save-idle-descriptors",
                   test_context_save_idle_descriptors);
    qtest_add_func("sn1/odyssey/"
                   "context-save-texture-coordinate-active-primitive",
                   test_context_save_texture_coordinate_active_primitive);
    qtest_add_func("sn1/odyssey/context-save-back-material",
                   test_context_save_back_material);
    qtest_add_func("sn1/odyssey/context-save-back-material-reset",
                   test_context_save_back_material_reset);
    qtest_add_func("sn1/odyssey/context-save-normal-ring",
                   test_context_save_normal_ring);
    qtest_add_func("sn1/odyssey/command-fifo-gl-never-set-index",
                   test_command_fifo_gl_never_set_index);
    qtest_add_func("sn1/odyssey/command-fifo-context-save-colour",
                   test_command_fifo_context_save_colour);
    qtest_add_func("sn1/odyssey/command-fifo-context-save-idle-state",
                   test_command_fifo_context_save_idle_state);
    qtest_add_func("sn1/odyssey/context-restart-triangle",
                   test_context_restart_triangle);
    qtest_add_func("sn1/odyssey/context-restart-edited-page",
                   test_context_restart_edited_page);
    qtest_add_func("sn1/odyssey/context-save-primitive-records",
                   test_context_save_primitive_records);
    qtest_add_func("sn1/odyssey/context-save-zero-vertex-primitive",
                   test_context_save_zero_vertex_primitive);
    qtest_add_func("sn1/odyssey/context-restart-one-vertex-strip",
                   test_context_restart_one_vertex_strip);
    qtest_add_func("sn1/odyssey/context-switch-integer-quad",
                   test_context_switch_integer_quad);
    qtest_add_func("sn1/odyssey/context-switch-polygon-assembly",
                   test_context_switch_polygon_assembly);
    qtest_add_func("sn1/odyssey/context-restart-unsupported-words",
                   test_context_restart_unsupported_words);
    qtest_add_func("sn1/odyssey/context-switch-completion",
                   test_context_switch_completion);
    qtest_add_func("sn1/odyssey/context-switch-without-shadow-address",
                   test_context_switch_without_shadow_address_is_a_guest_error);

    qtest_add_func("sn1/odyssey/processor-credit-return",
                   test_processor_credit_return);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
