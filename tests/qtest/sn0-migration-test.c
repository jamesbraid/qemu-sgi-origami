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

    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/migration/device-window-survives-migration",
                        NULL, test_a_device_window_survives_migration);

    return g_test_run();
}
