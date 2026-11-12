/*
 * A synthetic firmware image for the Sonic Solutions HDSP card's i960 Rx I/O
 * processor, for the register-contract tests. No vendor code: the image is
 * assembled here from the instruction formats in the Intel 80960Jx developer's
 * manual, with a boot record and process control block laid out as
 * docs/system/mips/sgi/hardware/sonic-hdsp.rst ("Boot") describes.
 *
 * The image stands in for the card's boot code and resident operating system.
 * It programs the ATU and the bridge unit the way the page says the stock code
 * does, clears the configuration-retry bit, then loops serving the messaging
 * unit, so the host-side contract can be checked end to end.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TESTS_QTEST_SGI_SONIC_HDSP_IMAGE_H
#define TESTS_QTEST_SGI_SONIC_HDSP_IMAGE_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"

/* ------------------------------------------------------------------ */
/* An i960 assembler, just enough for the test images                  */
/* ------------------------------------------------------------------ */

#define FLASH_SIZE      0x80000
#define FLASH_BASE      0xfef80000u
#define IBR_OFFSET      0x7ff30
#define CODE_START      0x28

#define G(n)            (16 + (n))

enum {
    M_STOB = 0x82, M_STOS = 0x8a, M_LDA = 0x8c, M_LDOB = 0x80, M_LDOS = 0x88,
    M_LD = 0x90, M_ST = 0x92, M_BX = 0x84,
    C_B = 0x08, C_CMPOBE = 0x32, C_CMPOBNE = 0x35,
    OP_AND = 0x581, OP_ADDO = 0x590, OP_SHRO = 0x598,
};

typedef struct Asm {
    uint8_t *flash;
    uint32_t pc;                /* flash offset of the next instruction */
} Asm;

static inline void emit(Asm *a, uint32_t w)
{
    stl_le_p(a->flash + a->pc, w);
    a->pc += 4;
}

static inline uint32_t memb(uint32_t op, unsigned r, unsigned abase,
                            unsigned mode)
{
    return (op << 24) | (r << 19) | (abase << 14) | (mode << 10);
}

static inline uint32_t mema(uint32_t op, unsigned r, unsigned abase,
                            uint32_t off)
{
    return (op << 24) | (r << 19) | (abase << 14) | (1u << 13) | (off & 0xfff);
}

static inline uint32_t reg3(uint32_t opc, unsigned s1, bool l1, unsigned s2,
                     unsigned d)
{
    return ((opc >> 4) << 24) | (d << 19) | (s2 << 14) |
           ((uint32_t)l1 << 11) | ((opc & 0xf) << 7) | s1;
}

static inline uint32_t cobr(uint32_t op, unsigned s1, bool l1, unsigned s2)
{
    return (op << 24) | (s1 << 19) | (s2 << 14) | ((uint32_t)l1 << 13);
}

static inline void ldc(Asm *a, unsigned reg, uint32_t val)
{
    emit(a, memb(M_LDA, reg, 0, 0xc));
    emit(a, val);
}

/* Absolute-address loads and stores, through the displacement mode. */
static inline void ld_abs(Asm *a, unsigned op, unsigned reg, uint32_t addr)
{
    emit(a, memb(op, reg, 0, 0xc));
    emit(a, addr);
}

static inline void st_abs(Asm *a, unsigned op, unsigned reg, uint32_t addr)
{
    emit(a, memb(op, reg, 0, 0xc));
    emit(a, addr);
}

static inline void st32(Asm *a, uint32_t addr, uint32_t val)
{
    ldc(a, G(0), val);
    st_abs(a, M_ST, G(0), addr);
}

static inline void st16(Asm *a, uint32_t addr, uint16_t val)
{
    ldc(a, G(0), val);
    st_abs(a, M_STOS, G(0), addr);
}

static inline void st8(Asm *a, uint32_t addr, uint8_t val)
{
    ldc(a, G(0), val);
    st_abs(a, M_STOB, G(0), addr);
}

/* A conditional branch placeholder, patched once the target is known. */
static inline uint32_t cobr_here(Asm *a, uint32_t op, unsigned s1, bool l1,
                          unsigned s2)
{
    uint32_t at = a->pc;

    emit(a, cobr(op, s1, l1, s2));
    return at;
}

static inline void patch_cobr(Asm *a, uint32_t at, uint32_t target)
{
    uint32_t w = ldl_le_p(a->flash + at);
    int32_t disp = (int32_t)target - (int32_t)at;

    g_assert_cmpint(disp, <, 4096);
    g_assert_cmpint(disp, >, -4096);
    w |= ((uint32_t)disp >> 2 & 0x7ff) << 2;
    stl_le_p(a->flash + at, w);
}

static inline void branch_to(Asm *a, uint32_t target)
{
    int32_t disp = (int32_t)target - (int32_t)a->pc;

    emit(a, (C_B << 24) | (((uint32_t)disp >> 2) & 0x3fffff) << 2);
}

/* ------------------------------------------------------------------ */
/* The test image                                                      */
/* ------------------------------------------------------------------ */

#define LOCAL_DRAM      0xa0000000u
#define SCRATCH         0xa0001f00u     /* words the image reports through */
#define S_UART_STATUS   (SCRATCH + 0x00)
#define S_BOARD_ID      (SCRATCH + 0x04)
#define S_UNCLAIMED     (SCRATCH + 0x08)
#define S_PROBE32       (SCRATCH + 0x0c)
#define S_PROBE64       (SCRATCH + 0x10)
#define S_GLUE          (SCRATCH + 0x14)
#define S_DSP0_ID       (SCRATCH + 0x18)
#define S_DSP3_ID       (SCRATCH + 0x1c)
#define S_PRIVATE_ID    (SCRATCH + 0x20)
#define S_FLASH         (SCRATCH + 0x24)
#define S_TRANSLATE     (SCRATCH + 0x28)
#define S_BANK1         (SCRATCH + 0x2c)
#define S_NVWORD        (SCRATCH + 0x30)
#define S_FAULT_AREA    (SCRATCH + 0x34)
#define S_BOOT_MODE     (SCRATCH + 0x38)
#define S_DSP0_CSID     (SCRATCH + 0x3c)
#define S_IPH_READ      (SCRATCH + 0x40)
#define S_MASK_READ     (SCRATCH + 0x44)
#define FRAME           0xa0001000u
#define QBAR            0xa0100000u

#define WAIT_CODE       0xa0001e00u     /* a routine run from DRAM */
#define WAIT_FLAG       0xa0001e20u
#define MSG_FLIP        0x0f11b
#define MSG_DMA         0x0d3a
#define DMA_DESC        0xa0002000u
#define TRANSFER_RAM    0x02000000u     /* the Bridge head's PCI-visible RAM */
#define MAGIC_32        0x32323232u
#define MAGIC_64        0x64646464u

enum {
    FW_FLASH_PROGRAM = 1,   /* store two bytes into a flash sector at start */
    FW_NO_RETRY_CLEAR = 2,  /* leave the configuration retry bit set */
    FW_RELEASE_DSPS = 4,    /* write the glue reset byte: all DSPs out */
    FW_RAISE_OMR = 8,       /* write outbound message 0 once set up */
    FW_SISR_FROM_EEPROM = 16, /* load SISR from the I2C EEPROM's word */
    FW_DMA_LOOP = 32,       /* a DMA chain whose descriptor is its own next */
    FW_DMA_BIG = 64,        /* the same, 16 MiB long, started by MSG_DMA */
};

/* The boot code's SISR: S_AD16 and S_AD22 to S_AD25 hidden (0x03c1). */
#define SISR_BOOT_ROM   0x03c1

typedef struct Image {
    unsigned flags;
    uint16_t sisr;
} Image;

/*
 * The boot record at the top of the flash and the process control block at
 * its bottom (sonic-hdsp.rst, "Boot"). The words checksum to zero with
 * end-around carry, as the manual requires.
 */
static inline void write_boot_record(uint8_t *flash, uint32_t entry)
{
    uint32_t prcb[10] = {
        0xf0040000, 0xfef80e80, 0x9000, 0x40000000, 0xa0000000,
        0xf00c0000, 0, 0xa007fe00, 0x10000, 0x1400,
    };
    uint8_t *ibr = flash + IBR_OFFSET;
    uint32_t acc = 0xffffffff, carry = 0;

    for (unsigned i = 0; i < 10; i++) {
        stl_le_p(flash + 4 * i, prcb[i]);
    }
    stl_le_p(ibr + 0x10, entry);
    stl_le_p(ibr + 0x14, FLASH_BASE);
    for (unsigned i = 0; i < 5; i++) {
        stl_le_p(ibr + 0x18 + 4 * i, 0x11111111u * (i + 1));
    }
    for (unsigned i = 0; i < 7; i++) {
        uint64_t t = (uint64_t)(uint32_t)ldl_le_p(ibr + 0x10 + 4 * i) +
                     acc + carry;

        acc = (uint32_t)t;
        carry = t >> 32;
    }
    stl_le_p(ibr + 0x2c, 0 - acc - carry);
}

/* A word the image copies from a local address into the scratch area. */
static inline void report_word(Asm *a, uint32_t from, uint32_t to)
{
    ld_abs(a, M_LD, G(1), from);
    st_abs(a, M_ST, G(1), to);
}

/* A secondary-bus configuration cycle: IDSEL lines, then the data port. */
static inline void sec_config_read(Asm *a, uint32_t addr, uint32_t to)
{
    st32(a, 0x12a8, addr);
    report_word(a, 0x12b0, to);
}

static inline void build_image(uint8_t *flash, const Image *img)
{
    Asm a = { flash, CODE_START };
    unsigned flags = img->flags;
    uint32_t loop, skip_a, skip_msg, skip_post;
    uint32_t sizing_a, sizing_b;

    write_boot_record(flash, FLASH_BASE + CODE_START);

    /* The debug UART: transmit-ready status, the data register. */
    ld_abs(&a, M_LDOB, G(1), 0xd0090003);
    st_abs(&a, M_ST, G(1), S_UART_STATUS);
    st8(&a, 0xd00b0003, 'H');
    st8(&a, 0xd00b0003, 'I');
    ld_abs(&a, M_LDOB, G(1), 0xe0040000);
    st_abs(&a, M_ST, G(1), S_BOARD_ID);
    report_word(&a, 0x20000000, S_UNCLAIMED);
    report_word(&a, 0xf0040050, S_FAULT_AREA);

    /* Size the DRAM by readback, as the boot code's pattern tests do. */
    ldc(&a, G(2), MAGIC_32);
    st_abs(&a, M_ST, G(2), LOCAL_DRAM + 32 * MiB - 4);
    ldc(&a, G(2), MAGIC_64);
    st_abs(&a, M_ST, G(2), LOCAL_DRAM + 64 * MiB - 4);
    ld_abs(&a, M_LD, G(3), LOCAL_DRAM + 32 * MiB - 4);
    st_abs(&a, M_ST, G(3), S_PROBE32);
    ld_abs(&a, M_LD, G(4), LOCAL_DRAM + 64 * MiB - 4);
    st_abs(&a, M_ST, G(4), S_PROBE64);
    /* g5 = size: 16 MiB, 32 if the 32 MiB probe held, 64 if both did. */
    ldc(&a, G(5), 16 * MiB);
    ldc(&a, G(6), MAGIC_32);
    sizing_a = cobr_here(&a, C_CMPOBNE, G(6), false, G(3));
    ldc(&a, G(5), 32 * MiB);
    patch_cobr(&a, sizing_a, a.pc);
    ldc(&a, G(6), MAGIC_64);
    sizing_b = cobr_here(&a, C_CMPOBNE, G(6), false, G(4));
    ldc(&a, G(5), 64 * MiB);
    patch_cobr(&a, sizing_b, a.pc);

    /* g7 = half the DRAM, g8 = the OS inbound translate (base + half). */
    emit(&a, reg3(OP_SHRO, 1, true, G(5), G(7)));   /* shro 1, g5 -> g7 */
    ldc(&a, G(9), LOCAL_DRAM);
    emit(&a, reg3(OP_ADDO, G(7), false, G(9), G(8)));
    st_abs(&a, M_ST, G(8), S_TRANSLATE);

    /* The identities the boot code programs (page, "PCI identity"). */
    st32(&a, 0x1000, 0x060e1263);
    st32(&a, 0x1034, 0x060e1263);
    st32(&a, 0x1200, 0x05dc1263);
    st32(&a, 0x122c, 0x05dc1263);
    st32(&a, 0x1208, 0x05080000);       /* class 0x050800 */
    st8(&a, 0x123d, 2);                 /* interrupt pin: INTB */
    st32(&a, 0x1240, 0xf8000000);       /* inbound limit: 128 MiB */
    st32(&a, 0x1244, LOCAL_DRAM);       /* boot-time translate */
    if (!(flags & FW_SISR_FROM_EEPROM)) {
        st16(&a, 0x1042, img->sisr);    /* SISR */
    }

    /* Memory controller bank 1: first a placeholder, then the glue base. */
    st32(&a, 0x1510, 0xa4000000);
    ldc(&a, G(9), 0x04000000);
    emit(&a, reg3(OP_ADDO, G(7), false, G(9), G(9)));   /* half + 64 MiB */
    ldc(&a, G(10), LOCAL_DRAM);
    emit(&a, reg3(OP_ADDO, G(9), false, G(10), G(9)));
    st_abs(&a, M_ST, G(9), 0x1510);
    st_abs(&a, M_ST, G(9), S_BANK1);

    /* The boot-mode byte at bank 1 + 0x18003. */
    ldc(&a, G(12), 0x18003);
    emit(&a, reg3(OP_ADDO, G(9), false, G(12), G(12)));
    emit(&a, mema(M_LDOB, G(4), G(12), 0));
    st_abs(&a, M_ST, G(4), S_BOOT_MODE);

    if (flags & FW_SISR_FROM_EEPROM) {
        /*
         * Read the nv word the way the page describes: START with 0xa0, the
         * offset, a repeated START with 0xa1, then four reads, the last with
         * not-acknowledge and STOP. Then SISR = word & 0x3ff when the upper
         * half is 0x9876, else 0.
         */
        uint32_t skip;

        st32(&a, 0x1680, 0x4000);
        st32(&a, 0x1684, 0x7f0);
        st32(&a, 0x1680, 0x60);
        st32(&a, 0x168c, 0xa0);
        st32(&a, 0x1680, 0x69);
        st32(&a, 0x168c, 0x00);
        st32(&a, 0x1680, 0x68);
        st32(&a, 0x168c, 0xa1);
        st32(&a, 0x1680, 0x69);
        for (unsigned i = 0; i < 4; i++) {
            st32(&a, 0x1680, i == 3 ? 0x6e : 0x68);
            ld_abs(&a, M_LD, G(1), 0x168c);
            st_abs(&a, M_STOB, G(1), S_NVWORD + i);
        }
        st16(&a, 0x1042, 0);
        ld_abs(&a, M_LD, G(1), S_NVWORD);
        emit(&a, reg3(OP_SHRO, 16, true, G(1), G(2)));
        ldc(&a, G(3), 0x9876);
        skip = cobr_here(&a, C_CMPOBNE, G(3), false, G(2));
        ldc(&a, G(5), 0x3ff);
        emit(&a, reg3(OP_AND, G(5), false, G(1), G(4)));
        st_abs(&a, M_STOS, G(4), 0x1042);
        patch_cobr(&a, skip, a.pc);
    }

    if (flags & FW_RELEASE_DSPS) {
        ldc(&a, G(12), 0xa0003);
        emit(&a, reg3(OP_ADDO, G(9), false, G(12), G(12)));
        ldc(&a, G(0), 0);
        emit(&a, mema(M_STOB, G(0), G(12), 0));
    }

    /* Messaging unit: queues enabled, one free request frame posted. */
    st32(&a, 0x1350, 0x3);
    st32(&a, 0x1354, QBAR);
    st32(&a, 0x1364, 0);
    st32(&a, 0x1368, 0x4000);
    st32(&a, 0x136c, 0x4000);
    st32(&a, 0x1370, 0xc000);
    st32(&a, 0x1374, 0xc000);
    st32(&a, 0x1378, 0x8000);
    st32(&a, 0x137c, 0x8000);
    report_word(&a, 0x1368, S_IPH_READ);        /* QBAR | 0x4000 */
    st32(&a, 0x1370, 0xdeadbeef);               /* only bits 19:2 stick */
    report_word(&a, 0x1370, S_MASK_READ);
    st32(&a, 0x1370, 0xc000);
    ldc(&a, G(1), FRAME);
    st_abs(&a, M_ST, G(1), QBAR);
    st32(&a, 0x1360, 4);
    st32(&a, 0x1334, 0xffffffff);       /* the library masks every source */
    st32(&a, 0x1334, 0);

    /* Setup done: bridge control, upstream prefetch, retry off. */
    if (!(flags & FW_NO_RETRY_CLEAR)) {
        st16(&a, 0x1040, 0x0008);
    }

    if (flags & FW_FLASH_PROGRAM) {
        /*
         * A program cycle makes the whole part answer reads with polling
         * data, so the image waits it out in DRAM: a routine that spins on a
         * flag word the host sets, then jumps back into the flash.
         */
        uint32_t imm;

        st8(&a, FLASH_BASE + 0x40000, 0x11);
        st8(&a, FLASH_BASE + 0x40001, 0x22);
        st32(&a, WAIT_CODE + 0, memb(M_LD, G(1), 0, 0xc));
        st32(&a, WAIT_CODE + 4, WAIT_FLAG);
        st32(&a, WAIT_CODE + 8, cobr(C_CMPOBE, 0, true, G(1)) |
                                (((uint32_t)-8 >> 2 & 0x7ff) << 2));
        st32(&a, WAIT_CODE + 12, memb(M_BX, 0, 0, 0xc));
        emit(&a, memb(M_LDA, G(0), 0, 0xc));
        imm = a.pc;
        emit(&a, 0);                    /* patched: where the flash resumes */
        st_abs(&a, M_ST, G(0), WAIT_CODE + 16);
        emit(&a, memb(M_BX, 0, 0, 0xc));
        emit(&a, WAIT_CODE);
        stl_le_p(a.flash + imm, FLASH_BASE + a.pc);
    }

    if (flags & FW_RAISE_OMR) {
        st32(&a, 0x1318, 0x69a);
    }

    if (flags & FW_DMA_LOOP) {
        /* Descriptor: next = itself, no bytes, PCI memory read. */
        st32(&a, 0xa0002000, 0xa0002000);
        st32(&a, 0xa0002004, 0);
        st32(&a, 0xa0002008, 0);
        st32(&a, 0xa000200c, 0xa0003000);
        st32(&a, 0xa0002010, 0);
        st32(&a, 0xa0002014, 0x6);
        st32(&a, 0x1410, 0xa0002000);
        st32(&a, 0x1400, 1);
    }

    if (flags & FW_DMA_BIG) {
        /* Descriptor: next = itself, 16 MiB from the head's RAM into DRAM. */
        st32(&a, DMA_DESC, DMA_DESC);
        st32(&a, DMA_DESC + 4, TRANSFER_RAM);
        st32(&a, DMA_DESC + 8, 0);
        st32(&a, DMA_DESC + 12, 0xa0800000);
        st32(&a, DMA_DESC + 16, 0xffffff);
        st32(&a, DMA_DESC + 20, 0x6);
    }

    /* Serve the messaging unit forever. */
    loop = a.pc;
    ld_abs(&a, M_LD, G(1), 0x1324);                     /* IISR */
    emit(&a, reg3(OP_AND, 1, true, G(1), G(2)));
    skip_msg = cobr_here(&a, C_CMPOBE, 0, true, G(2));
    ld_abs(&a, M_LD, G(3), 0x1310);                     /* IMR0 */

    /* What the i960 sees of the glue and of the secondary bus. */
    ld_abs(&a, M_LD, G(11), 0x1510);                    /* bank 1 base */
    ldc(&a, G(12), 0xa0003);
    emit(&a, reg3(OP_ADDO, G(11), false, G(12), G(11)));
    emit(&a, mema(M_LDOB, G(4), G(11), 0));
    st_abs(&a, M_ST, G(4), S_GLUE);
    sec_config_read(&a, 1u << 17, S_DSP0_ID);
    sec_config_read(&a, 1u << 20, S_DSP3_ID);
    sec_config_read(&a, (1u << 17) | 0x2c, S_DSP0_CSID);
    sec_config_read(&a, 1u << 12, S_PRIVATE_ID);
    ld_abs(&a, M_LDOS, G(4), FLASH_BASE + 0x40000);
    st_abs(&a, M_ST, G(4), S_FLASH);

    /* MSG_FLIP moves the inbound translate to the OS view. */
    ldc(&a, G(6), MSG_FLIP);
    skip_a = cobr_here(&a, C_CMPOBNE, G(6), false, G(3));
    ld_abs(&a, M_LD, G(5), S_TRANSLATE);
    st_abs(&a, M_ST, G(5), 0x1244);
    ldc(&a, G(6), 0x0e000100);                          /* I2O class */
    st_abs(&a, M_ST, G(6), 0x1208);
    patch_cobr(&a, skip_a, a.pc);

    /* MSG_DMA starts the 16 MiB self-looping chain. */
    ldc(&a, G(6), MSG_DMA);
    skip_a = cobr_here(&a, C_CMPOBNE, G(6), false, G(3));
    st32(&a, 0x1410, DMA_DESC);
    st32(&a, 0x1400, 1);
    patch_cobr(&a, skip_a, a.pc);

    /* The reply, then acknowledge the inbound message interrupt. */
    ldc(&a, G(7), 1);
    emit(&a, reg3(OP_ADDO, G(3), false, G(7), G(7)));
    st_abs(&a, M_ST, G(7), 0x1318);                     /* OMR0 */
    ldc(&a, G(7), 1);
    st_abs(&a, M_ST, G(7), 0x1324);
    patch_cobr(&a, skip_msg, a.pc);

    /* An inbound post-queue entry: read the frame, post frame word + 1. */
    ld_abs(&a, M_LD, G(1), 0x1324);
    emit(&a, reg3(OP_AND, 0x10, true, G(1), G(2)));
    skip_post = cobr_here(&a, C_CMPOBE, 0, true, G(2));
    ld_abs(&a, M_LD, G(11), 0x136c);            /* IPTPR: a local address */
    emit(&a, reg3(0x5cc, G(11), false, 0, G(13)));
    emit(&a, mema(M_LD, G(4), G(13), 0));               /* the frame address */
    emit(&a, mema(M_LD, G(5), G(4), 0));                /* its first word */
    emit(&a, reg3(OP_ADDO, 4, true, G(11), G(11)));
    st_abs(&a, M_ST, G(11), 0x136c);
    ld_abs(&a, M_LD, G(11), 0x1378);            /* OPHPR: a local address */
    emit(&a, reg3(0x5cc, G(11), false, 0, G(13)));
    emit(&a, reg3(OP_ADDO, 1, true, G(5), G(5)));
    emit(&a, mema(M_ST, G(5), G(13), 0));
    emit(&a, reg3(OP_ADDO, 4, true, G(11), G(11)));
    st_abs(&a, M_ST, G(11), 0x1378);
    ldc(&a, G(7), 0x10);
    st_abs(&a, M_ST, G(7), 0x1324);
    patch_cobr(&a, skip_post, a.pc);
    branch_to(&a, loop);
    g_assert_cmpuint(a.pc, <, 0xe80);
}

static inline char *flash_file(const Image *img)
{
    g_autofree uint8_t *flash = g_malloc0(FLASH_SIZE);
    GError *err = NULL;
    char *path;
    int fd = g_file_open_tmp("sonic-hdsp-flash-XXXXXX", &path, &err);

    g_assert_no_error(err);
    build_image(flash, img);
    g_assert_cmpint(write(fd, flash, FLASH_SIZE), ==, FLASH_SIZE);
    close(fd);
    return path;
}


#endif /* TESTS_QTEST_SGI_SONIC_HDSP_IMAGE_H */
