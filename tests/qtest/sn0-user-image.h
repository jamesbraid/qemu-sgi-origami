/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hand-assembled bare-metal images for CPU-side Origin 200 qtests.
 *
 * Independent instructions, not code copied from a guest image.  An image
 * is loaded with -bios at 0xbfc00000, so offset 0 is the reset vector and
 * the boot-time exception vectors sit at 0x200 (TLB refill), 0x280 (XTLB
 * refill) and 0x380 (general).  Every CP0 write is followed by four nops
 * because the R10000 has no EHB.
 */

#ifndef SN0_USER_IMAGE_H
#define SN0_USER_IMAGE_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define SN0_IMAGE_SIZE      0x2000
#define SN0_IMAGE_BASE      0xbfc00000u
#define SN0_VEC_TLB_REFILL  0x200
#define SN0_VEC_XTLB_REFILL 0x280
#define SN0_VEC_GENERAL     0x380

/* The lowest two megabytes hold the image copy and boot data on node 0. */
#define SN0_USER_TEXT       0x00400000u
#define SN0_USER_PHYS       0x00200000u
#define SN0_USER_CODE       (SN0_USER_PHYS + 0x800)
#define SN0_USER_MAILBOX    (SN0_USER_PHYS + 0xc00)
#define SN0_KSEG1(phys)     (0xa0000000u | (phys))

/* Hub-local processor number: bit 0 is 0 for CPU A, 1 for CPU B. */
#define SN0_PI_CPU_NUM      0x9200000001000020ULL
/* Node identity word the host seeds through each node's own CPUs. */
#define SN0_NODE_ID_PHYS    0x02000000u

#define SN0_TWO_NODE_TOPOLOGY \
    "-m 128M " \
    "-smp 4"

/*
 * The Hub network-interface status register's node-number field, bits
 * 16:8 at offset 0x60_0000, resets to zero on every Hub
 * (docs/datasheets/hub-network-interface.md, "Status, revision and
 * identity", in the Origami project tree): "Writable, because firmware
 * assigns it." After a reset no Hub has a number, so none can be named
 * by an ordinary, node-qualified address -- the same datasheet's
 * "Vector routing" section covers exactly this: a vector operation
 * reaches a small subset of a directly-connected peer's registers by
 * describing the route rather than the destination, route 0x0 being
 * that peer, and "it is how the firmware discovers the fabric, elects
 * a master, assigns node numbers and programs the routing tables."
 * This shared helper does the same thing: a vector-routed write of the
 * target field into the direct peer's status register, issued from the
 * local Hub, the way the PROM's elected master assigns every other
 * node's number before ordinary addressing works at all.
 */
#define SN0_HUB_LOCAL             UINT64_C(0x20001000000)
#define SN0_NI_STATUS             0x600000
#define SN0_NI_STATUS_REGION_SIZE (UINT64_C(1) << 17)
#define SN0_NI_STATUS_NODE_SHIFT  8
#define SN0_NI_VECTOR_PARMS       0x600200
#define SN0_NI_VECTOR             0x600208
#define SN0_NI_VECTOR_DATA        0x600210
#define SN0_PIOTYPE_WRITE         1

static inline void sn0_program_nasid(QTestState *qts, unsigned int nasid)
{
    qtest_writeq(qts, SN0_HUB_LOCAL + SN0_NI_VECTOR, 0);
    qtest_writeq(qts, SN0_HUB_LOCAL + SN0_NI_VECTOR_DATA,
                SN0_NI_STATUS_REGION_SIZE |
                ((uint64_t)nasid << SN0_NI_STATUS_NODE_SHIFT));
    qtest_writeq(qts, SN0_HUB_LOCAL + SN0_NI_VECTOR_PARMS,
                SN0_NI_STATUS | SN0_PIOTYPE_WRITE);
}

#define SN0_R_ZERO  0
#define SN0_R_V0    2
#define SN0_R_A0    4
#define SN0_R_A1    5
#define SN0_R_A2    6
#define SN0_R_A3    7
#define SN0_R_T0    8
#define SN0_R_T1    9
#define SN0_R_T2    10
#define SN0_R_S0    16
#define SN0_R_S1    17
#define SN0_R_S2    18
#define SN0_R_T8    24
#define SN0_R_T9    25
#define SN0_R_K0    26
#define SN0_R_K1    27

#define SN0_CP0_INDEX       0
#define SN0_CP0_ENTRYLO0    2
#define SN0_CP0_ENTRYLO1    3
#define SN0_CP0_PAGEMASK    5
#define SN0_CP0_WIRED       6
#define SN0_CP0_BADVADDR    8
#define SN0_CP0_ENTRYHI     10
#define SN0_CP0_STATUS      12
#define SN0_CP0_CAUSE       13
#define SN0_CP0_EPC         14
#define SN0_CP0_ERROREPC    30

#define SN0_OPC_BEQ         4
#define SN0_OPC_BNE         5
#define SN0_OPC_ADDIU       9
#define SN0_OPC_ANDI        12
#define SN0_OPC_ORI         13
#define SN0_OPC_LUI         15
#define SN0_OPC_LW          35
#define SN0_OPC_SW          43
#define SN0_OPC_LD          55
#define SN0_OPC_SD          63

/* Status: BEV and KX with ERL clear, for kernel code before the first ERET. */
#define SN0_STATUS_KERNEL   0x00400080u
/* Status: BEV, KX, user KSU and EXL, for the ERET into user code. */
#define SN0_STATUS_TO_USER  0x00400092u

typedef struct SN0Image {
    uint8_t bytes[SN0_IMAGE_SIZE];
    unsigned int pos;
} SN0Image;

static inline void sn0_emit(SN0Image *p, uint32_t word)
{
    g_assert_cmpuint(p->pos + 4, <=, sizeof(p->bytes));
    stl_be_p(p->bytes + p->pos, word);
    p->pos += 4;
}

static inline void sn0_emit_i(SN0Image *p, unsigned int op, unsigned int rs,
                              unsigned int rt, uint16_t imm)
{
    sn0_emit(p, (op << 26) | (rs << 21) | (rt << 16) | imm);
}

static inline void sn0_emit_nop(SN0Image *p)
{
    sn0_emit(p, 0);
}

static inline void sn0_emit_li(SN0Image *p, unsigned int rt, uint32_t value)
{
    sn0_emit_i(p, SN0_OPC_LUI, 0, rt, value >> 16);
    sn0_emit_i(p, SN0_OPC_ORI, rt, rt, value);
}

static inline void sn0_emit_li64(SN0Image *p, unsigned int rt, uint64_t value)
{
    sn0_emit_i(p, SN0_OPC_ORI, 0, rt, value >> 48);
    for (unsigned int shift = 32; ; shift -= 16) {
        sn0_emit(p, (rt << 16) | (rt << 11) | (16 << 6) | 0x38); /* dsll 16 */
        sn0_emit_i(p, SN0_OPC_ORI, rt, rt, value >> shift);
        if (shift == 0) {
            break;
        }
    }
}

static inline void sn0_emit_mtc0(SN0Image *p, unsigned int rt, unsigned int rd)
{
    sn0_emit(p, 0x40800000 | (rt << 16) | (rd << 11));
    for (unsigned int i = 0; i < 4; i++) {
        sn0_emit_nop(p);
    }
}

static inline void sn0_emit_dmtc0(SN0Image *p, unsigned int rt,
                                  unsigned int rd)
{
    sn0_emit(p, 0x40a00000 | (rt << 16) | (rd << 11));
    for (unsigned int i = 0; i < 4; i++) {
        sn0_emit_nop(p);
    }
}

static inline void sn0_emit_mfc0(SN0Image *p, unsigned int rt, unsigned int rd)
{
    sn0_emit(p, 0x40000000 | (rt << 16) | (rd << 11));
}

static inline void sn0_emit_or(SN0Image *p, unsigned int rd, unsigned int rs,
                               unsigned int rt)
{
    sn0_emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x25);
}

static inline void sn0_emit_addu(SN0Image *p, unsigned int rd, unsigned int rs,
                                 unsigned int rt)
{
    sn0_emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x21);
}

static inline void sn0_emit_sll(SN0Image *p, unsigned int rd, unsigned int rt,
                                unsigned int sa)
{
    sn0_emit(p, (rt << 16) | (rd << 11) | (sa << 6));
}

static inline void sn0_emit_srl(SN0Image *p, unsigned int rd, unsigned int rt,
                                unsigned int sa)
{
    sn0_emit(p, (rt << 16) | (rd << 11) | (sa << 6) | 0x02);
}

/* Branch with a delay-slot nop; the target is patched in later. */
static inline unsigned int sn0_emit_branch(SN0Image *p, unsigned int op,
                                           unsigned int rs, unsigned int rt)
{
    unsigned int at = p->pos;

    sn0_emit_i(p, op, rs, rt, 0);
    sn0_emit_nop(p);
    return at;
}

static inline void sn0_patch_branch(SN0Image *p, unsigned int at,
                                    unsigned int dest)
{
    int delta = ((int)dest - (int)at - 4) / 4;

    g_assert_cmpint(delta, >=, INT16_MIN);
    g_assert_cmpint(delta, <=, INT16_MAX);
    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) | (uint16_t)delta);
}

static inline void sn0_emit_park(SN0Image *p)
{
    sn0_emit(p, 0x1000ffff); /* beq zero, zero, self */
    sn0_emit_nop(p);
}

static inline void sn0_emit_eret(SN0Image *p)
{
    sn0_emit(p, 0x42000018);
    sn0_emit_nop(p);
}

static inline void sn0_emit_syscall(SN0Image *p)
{
    sn0_emit(p, 0x0000000c);
}

static inline void sn0_emit_break(SN0Image *p)
{
    sn0_emit(p, 0x0000000d);
}

static inline void sn0_emit_tlbwi(SN0Image *p)
{
    sn0_emit(p, 0x42000002);
    for (unsigned int i = 0; i < 4; i++) {
        sn0_emit_nop(p);
    }
}

static inline void sn0_emit_tlbwr(SN0Image *p)
{
    sn0_emit(p, 0x42000006);
    for (unsigned int i = 0; i < 4; i++) {
        sn0_emit_nop(p);
    }
}

/*
 * Map one 4 KiB page pair at @vaddr onto @paddr, cached, dirty, valid and
 * global, with @asid left in EntryHi afterwards.  Clobbers t0.  Returns
 * the image offset of the tlbwi itself.
 */
static inline unsigned int sn0_emit_map_page(SN0Image *p, unsigned int index,
                                             uint32_t vaddr, uint32_t paddr,
                                             unsigned int asid)
{
    unsigned int tlbwi;

    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, index);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_INDEX);
    sn0_emit_mtc0(p, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(p, SN0_R_T0, vaddr | asid);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYHI);
    sn0_emit_li(p, SN0_R_T0, (paddr >> 6) | 0x1f); /* C=3, D, V, G */
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_i(p, SN0_OPC_ADDIU, 0, SN0_R_T0, 1); /* odd half: G only */
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_ENTRYLO1);
    tlbwi = p->pos;
    sn0_emit_tlbwi(p);
    return tlbwi;
}

/* Load this processor's Hub-local number (0 or 1) into @rt.  Clobbers t0. */
static inline void sn0_emit_cpu_number(SN0Image *p, unsigned int rt)
{
    sn0_emit_li64(p, SN0_R_T0, SN0_PI_CPU_NUM);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_T0, rt, 0);
}

/* Write the image to a temporary file and return its path. */
static inline char *sn0_image_write(const SN0Image *p, const char *stem)
{
    g_autofree char *tmpl = g_strdup_printf("%s-XXXXXX", stem);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp(tmpl, &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, p->bytes, sizeof(p->bytes)), ==,
                    sizeof(p->bytes));
    close(fd);
    return path;
}

/* Poll a big-endian mailbox word through @cpu's own address space. */
static inline uint32_t sn0_wait_word(QTestState *qts, unsigned int cpu,
                                     uint64_t addr, unsigned int seconds)
{
    int64_t deadline = g_get_monotonic_time() + seconds * G_TIME_SPAN_SECOND;
    uint8_t word[4];

    do {
        g_assert_true(qtest_memread_cpu(qts, cpu, addr, word, sizeof(word)));
        if (ldl_be_p(word)) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    return ldl_be_p(word);
}

#endif
