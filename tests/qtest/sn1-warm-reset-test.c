/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A node-local reset through the Bedrock network interface restarts the
 * processors at the reset vector.
 *
 * Every IP35 consumer that reboots does it the same way: write 1 to the
 * network interface's reset enable, then local plus port reset (0x6) to
 * its port reset register, and never jump to the firmware itself. The
 * stock IP35 PROM prints "Resetting the system..." and does exactly that
 * (func_1fc642b8), then branches back into a loop; IRIX 6.5.30's restart
 * path writes its request word to local-block scratch 2 on every node and
 * then does the same pair of writes every 3 s of the real-time counter
 * until something happens; OpenBSD's IP35 reboot writes local plus link
 * reset. A model that resets only the Bedrock's own registers leaves
 * every processor in those loops, which is the Origin 300 reboot stall
 * (jamesb/qemu#875).
 *
 * The program counts its own starts in RAM, which a reset leaves alone.
 * Processor A makes the consumers' two writes on its first start and then
 * spins, as the PROM does; on any later start it only records the count.
 * The other processor spins from the start. The model must bring A back
 * through the reset vector, so the count reaches 2.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"

#define HUB_WINDOW UINT64_C(0x9200000001000000)
#define PI_CPU_NUMBER UINT64_C(0x20)
#define NI_PORT_RESET UINT64_C(0x680008)
#define NI_RESET_ENABLE UINT64_C(0x680010)

/* local reset (bit 2) plus port reset (bit 1), as every consumer writes */
#define PORT_RESET_LOCAL_AND_PORT 0x6

/*
 * The start count, in low RAM clear of the flash and of the firmware
 * filter's window at 1 MiB, through Hub special space's uncached alias of
 * the node's own memory, which the host reads the same way: plain node
 * memory sits behind the memory unit's bank translation, which answers
 * nothing until firmware configures it.
 */
#define STATE_PHYS UINT64_C(0x180000)
#define STATE_HOST (SPACE_HUB + STATE_PHYS)

#define POLL_TRIES 500
#define POLL_INTERVAL_US 10000

#define R_ZERO 0
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_S0 16
#define R_S1 17

#define OPC_BEQ (4u << 26)
#define OPC_BNE (5u << 26)
#define OPC_ANDI 12
#define OPC_ORI 13
#define OPC_DADDIU 25

#define CP0_STATUS 12
#define STATUS_KX 0x80

typedef struct Image {
    uint8_t *bytes;
    unsigned int pos;
} Image;

static void emit(Image *p, uint32_t word)
{
    g_assert_cmpuint(p->pos + 4, <=, PROM_SIZE);
    stl_be_p(p->bytes + p->pos, word);
    p->pos += 4;
}

static void emit_i(Image *p, unsigned int op, unsigned int rs,
                   unsigned int rt, uint16_t imm)
{
    emit(p, (op << 26) | (rs << 21) | (rt << 16) | imm);
}

/* Load a 64-bit constant: high halfword, then three shift-and-or steps. */
static void emit_li64(Image *p, unsigned int rt, uint64_t value)
{
    emit_i(p, OPC_ORI, R_ZERO, rt, value >> 48);
    for (unsigned int shift = 32; ; shift -= 16) {
        emit(p, (rt << 16) | (rt << 11) | (16 << 6) | 0x38); /* dsll 16 */
        emit_i(p, OPC_ORI, rt, rt, value >> shift);
        if (shift == 0) {
            break;
        }
    }
}

/* A forward branch with a delay-slot nop; the target is patched later. */
static unsigned int emit_branch(Image *p, uint32_t op, unsigned int rs,
                                unsigned int rt)
{
    unsigned int at = p->pos;

    emit(p, mips_branch(op, rs, rt, 0));
    emit(p, mips_nop());
    return at;
}

static void patch_branch(Image *p, unsigned int at, unsigned int dest)
{
    int delta = ((int)dest - (int)at - 4) / 4;

    g_assert_cmpint(delta, >=, INT16_MIN);
    g_assert_cmpint(delta, <=, INT16_MAX);
    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) | (uint16_t)delta);
}

static void build_program(Image *p)
{
    unsigned int other, restarted;

    stl_be_p(p->bytes + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(p->bytes + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    p->pos = CODE_START;

    /* 64-bit kernel addressing, without which the pointers below fault. */
    emit(p, mips_mfc0(R_T0, CP0_STATUS));
    emit_i(p, OPC_ORI, R_T0, R_T0, STATUS_KX);
    emit(p, mips_mtc0(R_T0, CP0_STATUS));
    for (unsigned int i = 0; i < 4; i++) {
        emit(p, mips_nop());
    }

    emit_li64(p, R_S0, HUB_WINDOW);
    emit_li64(p, R_S1, UINT64_C(0x9000000000000000) | STATE_PHYS);

    /* Only processor A counts its starts and resets. */
    emit(p, mips_ld(R_T2, R_S0, PI_CPU_NUMBER));
    emit_i(p, OPC_ANDI, R_T2, R_T2, 3);
    other = emit_branch(p, OPC_BNE, R_T2, R_ZERO);

    emit(p, mips_ld(R_T0, R_S1, 0));
    emit_i(p, OPC_DADDIU, R_T0, R_T0, 1);
    emit(p, mips_sd(R_T0, R_S1, 0));
    emit_i(p, OPC_ORI, R_ZERO, R_T1, 1);
    restarted = emit_branch(p, OPC_BNE, R_T0, R_T1);

    /* The consumers' reboot: reset enable, then local plus port reset. */
    emit_li64(p, R_S0, HUB_WINDOW | NI_RESET_ENABLE);
    emit_i(p, OPC_ORI, R_ZERO, R_T1, 1);
    emit(p, mips_sd(R_T1, R_S0, 0));
    emit_li64(p, R_S0, HUB_WINDOW | NI_PORT_RESET);
    emit_i(p, OPC_ORI, R_ZERO, R_T1, PORT_RESET_LOCAL_AND_PORT);
    emit(p, mips_sd(R_T1, R_S0, 0));

    patch_branch(p, other, p->pos);
    patch_branch(p, restarted, p->pos);
    emit(p, 0x1000ffff); /* beq zero, zero, self */
    emit(p, mips_nop());
}

static void write_exact(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(n, >, 0);
        sent += n;
    }
}

static char *write_image(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *bytes = g_malloc0(PROM_SIZE);
    Image image = { .bytes = bytes };
    char *path = NULL;
    int fd;

    build_program(&image);
    fd = g_file_open_tmp("sn1-warm-reset-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_exact(fd, bytes, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void check_local_reset_restarts_the_processors(const void *data)
{
    const char *accel = data;
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t starts = 0;

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    prom = write_image();
    quoted = g_shell_quote(prom);
    /* The accelerator named here wins over the -accel qtest appended. */
    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s %s "
                    "-no-shutdown -S", quoted, accel);
    qtest_writeq(s, STATE_HOST, 0);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        starts = qtest_readq(s, STATE_HOST);
        if (starts >= 2) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_test_message("processor A started %" PRIu64 " time(s)", starts);
    /* Once from power-on and once more from its own reset. */
    g_assert_cmpuint(starts, ==, 2);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn1/warm-reset/local-reset-restarts-processors/mttcg",
                        "-accel tcg,thread=multi",
                        check_local_reset_restarts_the_processors);
    qtest_add_data_func("sn1/warm-reset/local-reset-restarts-processors/rr",
                        "-accel tcg,thread=single",
                        check_local_reset_restarts_the_processors);
    return g_test_run();
}
