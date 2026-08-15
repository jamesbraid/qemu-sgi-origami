/*
 * Whether a device's state comes back correct on the far side of a
 * migration, including whatever a `.post_load` hook re-derives rather than
 * carries in the migration stream directly.
 *
 * This does not build on QEMU's own tests/qtest/migration/ framework, and
 * that is a deliberate choice rather than a gap. That framework's job is
 * proving a *running* guest survives a *live* migration: it embeds a tiny
 * hand-assembled counting workload per target
 * (tests/qtest/migration/bootfile.c) and watches the count keep advancing on
 * the far side, which is how it tells a migration that preserved execution
 * from one that silently dropped it. mips64 has no such workload in that
 * file, and writing one would answer a question this fork is not asking:
 * every sn0-*-test.c file already runs the machine under the qtest
 * accelerator (`-accel qtest`, in libqtest.c's own qtest_init), which never
 * executes guest code at all, so "live" is not a meaningful property here
 * to begin with. What we need is narrower and does not need a workload — a
 * stopped machine's device state, including anything derived rather than
 * stored, has to round-trip.
 *
 * The two migration steps below are a deliberately small rederivation of
 * what tests/qtest/migration/migration-qmp.c and migration-util.c already
 * do for the four architectures that use that framework, sized down to what
 * a device-only, no-workload test needs: start a source and a destination,
 * drive `migrate` over a Unix socket the destination is already listening
 * on, and poll `query-migrate` for "completed". tests/qtest/nvme-test.c
 * does the same thing this way, outside that framework, for the same
 * reason — proving one device's migration does not need the machinery
 * built for proving a live guest's.
 *
 * The concrete case here is the Bridge's device window, the one this
 * pattern was written to cover: task 53 found that `device_window_base`
 * and each window's memory-or-I/O selection are derived from the per-slot
 * register in `sgi_bridge_aim_window()` and are not independent migrated
 * facts, so a Bridge that migrates the register correctly but never re-runs
 * that derivation comes up with its windows aimed at the wrong PCI space.
 * That fix was verified by hand at the time — two qemu-system-mips64
 * processes, QMP over a bind-mounted Unix socket, a real OpenBSD boot to
 * program the windows, `info mtree` compared before and after — because no
 * automated coverage existed for it. This is that verification, automated,
 * using a real PCI card behind the window instead of a boot, so the
 * question the test answers is not "does the register's bit pattern
 * survive" (VMSTATE_UINT32_ARRAY already guarantees that on its own) but
 * "does a real access through the window still reach the card," which only
 * a correct `.post_load` can leave true.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

/*
 * Crosstalk space, the bridge's own widget window inside it, and the
 * per-slot device-window register — the same constants sn0-bridge-test.c
 * defines for itself, kept as this file's own copy rather than shared,
 * matching how every sn0-*-test.c file in this tree already stands alone.
 */
#define SPACE_XTALK             0x20000000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"
#define MACHINE                 "-M origin200"

/* The Hub's boot-flash view and AMD command offsets. */
#define SPACE_HUB               0x10000000000ULL
#define FLASH_RESET             0x1fc00000ULL
#define FLASH_HUB               (SPACE_HUB + 0x10000000)
#define FLASH_UNLOCK1           0x5555
#define FLASH_UNLOCK2           0x2aaa
#define FLASH_PROGRAM_TARGET    0xe0100

#define HUB_LOCAL               (SPACE_XTALK + 0x01000000)
#define NI_SCRATCH_0            0x600100
#define NI_SCRATCH_1            0x600108

/* PCI type 0 configuration space, and the window each device sits behind. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)

/*
 * A card to probe with, the same in-tree device sn0-bridge-test.c uses for
 * the same reason: which card does not matter for a test about the window,
 * so pick one needing no backing file and no network.
 */
#define PROBE_SLOT              4
#define PROBE_CARD              " -device lsi53c895a,addr=" stringify(PROBE_SLOT)

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR1                0x14

/* A 32-bit scratch register in the card's memory-mapped register file. */
#define CARD_SCRATCH            0x34

/* Sixteen megabytes apart, matching sn0-bridge-test.c's own layout. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/* Window 1 is the second 2 MB window, and is where slot 4 lands on it. */
#define PROBE_WINDOW             1
#define PROBE_WINDOW_OFFSET      0x400000
#define WINDOW(offset)           (BRIDGE_BASE + (offset))

/*
 * The board identity part's one-wire register, and the slot widths a guest
 * drives it with — again this file's own copy, matching how the other
 * sn0-*-test.c files stand alone.
 */
#define BRIDGE_NIC               0x0b0
#define NIC_LEVEL                (1u << 0)
#define NIC_SAMPLE(us)           ((uint32_t)(us) << 2)
#define NIC_PULSE(us)            ((uint32_t)(us) << 10)
#define NIC_ROM_IDENTITY         0x360605040302010bULL
#define NIC_CRC_RESIDUE          0xb001

static void flash_command(QTestState *qts, uint32_t offset, uint8_t value)
{
    qtest_writeq(qts, FLASH_HUB + 8 * offset, value);
}

static void bridge_put_card_in_slot(QTestState *qts, int slot)
{
    qtest_writel(qts, CFG0(slot, 0) + PCI_BAR1, CARD_MMIO(slot));
    qtest_writel(qts, CFG0(slot, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
}

static void bridge_aim_window_at_card(QTestState *qts, int window, int slot)
{
    qtest_writel(qts, REG(BRIDGE_DEVICE(window)),
                DEV_MEMORY_SPACE | (CARD_MMIO(slot) >> 20));
}

/*
 * Drive a real migration between two already-connected qtest instances and
 * block until it either completes or fails. `to` must already be listening
 * on `uri` (an `-incoming` on its command line does that before qtest ever
 * gets a QMP handshake with it, so there is no window where `from` could
 * connect too early).
 */
static void migrate_stopped_vm(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }

        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

/*
 * The case task 53 fixed and nothing before this file automatically
 * re-checks: aim a window at a real card, migrate, and confirm the same
 * access that worked on the source still reaches the card on the
 * destination. Without `sgi_bridge_post_load()` re-deriving
 * `device_window_base` and the window's memory-or-I/O selection from the
 * migrated register, the destination's window comes up aimed at the wrong
 * PCI space (this was verified by hand at the time — mutating `post_load`
 * to a no-op put windows 0 and 1 back in PCI memory space instead of the
 * I/O space a real guest had put them in) and this read returns the bus's
 * unanswered value instead of the card's.
 */
static void test_a_device_window_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_BRIDGE PROBE_CARD);
    bridge_put_card_in_slot(from, PROBE_SLOT);
    bridge_aim_window_at_card(from, PROBE_WINDOW, PROBE_SLOT);

    qtest_writeq(from, HUB_LOCAL + NI_SCRATCH_0, 0x0123456789abcdefULL);
    qtest_writeq(from, HUB_LOCAL + NI_SCRATCH_1, 0xfedcba9876543210ULL);

    qtest_writel(from, WINDOW(PROBE_WINDOW_OFFSET) + CARD_SCRATCH, 0xc0ffee00);
    g_assert_cmphex(qtest_readl(from, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);

    /*
     * The destination starts with the same devices as the source — same
     * card, same slot — and listens for the incoming migration before qtest
     * ever hands it a QMP handshake, so there is no race with `from`
     * connecting before it is ready.
     */
    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE PROBE_CARD
                                   " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    migrate_stopped_vm(from, uri);

    /*
     * The actual claim: the same access that worked on the source still
     * reaches the same card on the destination, which only holds if the
     * window's derived state — not just the register a guest wrote — came
     * back correct.
     */
    g_assert_cmphex(qtest_readl(to, WINDOW(PROBE_WINDOW_OFFSET) +
                                CARD_SCRATCH), ==, 0xc0ffee00);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_SCRATCH_0), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_SCRATCH_1), ==,
                    0xfedcba9876543210ULL);

    qtest_quit(to);
    qtest_quit(from);
}

static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    qtest_writel(qts, REG(BRIDGE_NIC), NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    return qtest_readl(qts, REG(BRIDGE_NIC)) & NIC_LEVEL;
}

#define NIC_RESET(q)      nic_slot((q), 520, 65)
#define NIC_READ(q)       nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
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
    uint8_t byte = 0;
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

static uint16_t nic_crc(uint16_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

/*
 * A device with a conversation in it has state a snapshot at rest cannot
 * see, and this file could not see it either.
 *
 * The existing test migrates a machine whose devices are between accesses,
 * where everything that matters is already in a register. The board identity
 * part is not like that: a guest drives it one bit per register write, so
 * which phase it is in, how many bits of the current byte have arrived, what
 * they spell, and how far through an answer it is all live between accesses
 * and none of it is in any register. A snapshot taken at rest restores that
 * correctly by accident, because at rest there is nothing to restore.
 *
 * So this reads half a page on the source and the other half on the
 * destination, and checks the two halves as one page. The part number spans
 * the boundary and the page's own check covers all thirty-two bytes, so a
 * destination that came up having forgotten the conversation cannot produce
 * a page that passes -- it answers the idle wire instead, and idle reads as
 * ones.
 *
 * This fork has more than one device with a conversation in it, and the
 * general point is the reason to keep this test rather than the particular
 * bytes: a migration test that only ever snapshots at rest certifies a
 * property weaker than the one that has to hold.
 */
static void test_a_conversation_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t page[32];
    unsigned int i;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_WITH_BRIDGE);

    /* Address the part by name, then ask for the first page. */
    g_assert_false(NIC_RESET(from));
    nic_write_byte(from, 0x55);
    for (i = 0; i < 64; i++) {
        if ((NIC_ROM_IDENTITY >> i) & 1) {
            NIC_WRITE_ONE(from);
        } else {
            NIC_WRITE_ZERO(from);
        }
    }
    nic_write_byte(from, 0xf0);
    nic_write_byte(from, 0x00);
    nic_write_byte(from, 0x00);

    /* Half the page here... */
    for (i = 0; i < 16; i++) {
        page[i] = nic_read_byte(from);
    }

    dest_cmdline = g_strdup_printf(MACHINE_WITH_BRIDGE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    migrate_stopped_vm(from, uri);

    /* ...and the rest there, mid-byte-stream, with nothing re-sent. */
    for (i = 16; i < sizeof(page); i++) {
        page[i] = nic_read_byte(to);
    }

    /*
     * Checked as one page rather than byte by byte: the part number spans
     * the halves and the page's own check covers all of it, so this is the
     * whole value and not a property of it.
     */
    g_assert_cmpmem(&page[11], 12, "030-1244-001", 12);
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_flash_autoselect_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    flash_command(from, FLASH_UNLOCK1, 0xaa);
    flash_command(from, FLASH_UNLOCK2, 0x55);
    flash_command(from, FLASH_UNLOCK1, 0x90);
    g_assert_cmphex(qtest_readl(from, FLASH_HUB), ==, 0x01d5ffff);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readl(to, FLASH_RESET), ==, 0x01d5ffff);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_flash_program_command_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);
    flash_command(from, FLASH_UNLOCK1, 0xaa);
    flash_command(from, FLASH_UNLOCK2, 0x55);
    flash_command(from, FLASH_UNLOCK1, 0xa0);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    flash_command(to, FLASH_PROGRAM_TARGET, 0x5a);
    g_assert_cmphex(qtest_readb(to, FLASH_RESET + FLASH_PROGRAM_TARGET), ==,
                    0x5a);

    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/migration/device-window-survives-migration",
                        NULL, test_a_device_window_survives_migration);
    qtest_add_data_func("sn0/migration/conversation-survives-migration",
                        NULL, test_a_conversation_survives_migration);
    qtest_add_data_func("sn0/migration/flash-autoselect-survives-migration",
                        NULL, test_flash_autoselect_survives_migration);
    qtest_add_data_func("sn0/migration/flash-program-command-"
                        "survives-migration",
                        NULL, test_flash_program_command_survives_migration);

    return g_test_run();
}
