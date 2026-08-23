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
#define NODE_STRIDE             0x100000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define BRIDGE_BASE_FOR_NODE(n) (BRIDGE_BASE + (uint64_t)(n) * NODE_STRIDE)
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"
#define MACHINE                 "-M origin200"
#define MACHINE_RECORDING       "-M origin200,diagnostic-recording=on"

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
#define NI_META_TABLE_LAST      0x6380f8
#define NI_LOCAL_TABLE          0x638100

/*
 * The block transfer engines' registers and fields — this file's own copy,
 * matching how every sn0-*-test.c file stands alone.
 */
#define BTE_LENGTH(e)           (0x410000 + (e) * 0x10000)
#define BTE_SOURCE(e)           (BTE_LENGTH(e) + 0x08)
#define BTE_DEST(e)             (BTE_LENGTH(e) + 0x10)
#define BTE_CONTROL(e)          (BTE_LENGTH(e) + 0x18)
#define BTE_NOTIFY(e)           (BTE_LENGTH(e) + 0x20)
#define BTE_INTERRUPT(e)        (BTE_LENGTH(e) + 0x28)
#define BTE_BUSY                (1ULL << 20)
#define BTE_CTL_NOTIFY          (1ULL << 4)
#define BTE_INT_LEVEL(l)        ((uint64_t)(l) << 16)
#define BTE_LINE                128
#define PI_INT_PEND0            0x000098
#define PI_REGION_PRESENT       0x000018

/* PCF8584 data and control registers inside the Hub-local window. */
#define I2C_DATA                0x220000
#define I2C_CTRL                0x220008
#define I2C_CTL_PIN             0x80
#define I2C_CTL_ESO             0x40
#define I2C_CTL_STA             0x04
#define I2C_CTL_STO             0x02
#define I2C_CTL_ACK             0x01
#define I2C_STS_LRB             0x08
#define ELSC_ADDR               0x08

/* PCI type 0 configuration space, and the window each device sits behind. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)
#define CFG0_FOR_NODE(node, device) \
    (BRIDGE_BASE_FOR_NODE(node) + BRIDGE_CFG0 + \
     (device) * CFG0_DEVICE_STRIDE)

/*
 * A card to probe with, the same in-tree device sn0-bridge-test.c uses for
 * the same reason: which card does not matter for a test about the window,
 * so pick one needing no backing file and no network.
 */
#define PROBE_SLOT              4
#define PROBE_CARD              " -device lsi53c895a,addr=" stringify(PROBE_SLOT)

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14

/* A 32-bit scratch register in the card's memory-mapped register file. */
#define CARD_SCRATCH            0x34

/* Sixteen megabytes apart, matching sn0-bridge-test.c's own layout. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/* Window 1 is the second 2 MB window, and is where slot 4 lands on it. */
#define PROBE_WINDOW             1
#define PROBE_WINDOW_OFFSET      0x400000
#define WINDOW(offset)           (BRIDGE_BASE + (offset))

#define IOC3_SLOT                2
#define IOC3_PCI_MMIO            0x10000000u
#define IOC3_WINDOW_OFFSET       0x600000
#define IOC3_SERIAL_A            0x020178
#define IOC3_SCRATCH             7
#define IOC3_SCRATCH_AT(node)    (BRIDGE_BASE_FOR_NODE(node) + \
                                  IOC3_WINDOW_OFFSET + IOC3_SERIAL_A + \
                                  IOC3_SCRATCH)

#define DUAL_CHASSIS_TOPOLOGY \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"
#define MACHINE_DUAL \
    "-M origin200,synthesized-firmware=off " DUAL_CHASSIS_TOPOLOGY

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

static QDict *query_origin_state(QTestState *qts)
{
    return qtest_qmp_assert_success_ref(
        qts, "{'execute': 'x-query-origin-state', "
             "'arguments': {'event-limit': 4096}}");
}

static bool has_hub_write(QDict *state, uint64_t address, uint64_t value)
{
    QDict *recorder = qdict_get_qdict(state, "flight-recorder");
    const QListEntry *entry;

    for (entry = qlist_first(qdict_get_qlist(recorder, "events")); entry;
         entry = qlist_next(entry)) {
        QDict *event = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(event, "component"), "hub") &&
            !strcmp(qdict_get_str(event, "operation"), "mmio-write") &&
            qdict_get_uint(event, "address") == address &&
            qdict_get_uint(event, "value") == value) {
            return true;
        }
    }
    return false;
}

static void attach_node_ioc3(QTestState *qts, unsigned int node)
{
    qtest_writel(qts, CFG0_FOR_NODE(node, IOC3_SLOT) + PCI_BAR0,
                 IOC3_PCI_MMIO);
    qtest_writel(qts, CFG0_FOR_NODE(node, IOC3_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_BASE_FOR_NODE(node) + BRIDGE_DEVICE(IOC3_SLOT) +
                       REG_DATA,
                 DEV_MEMORY_SPACE | (IOC3_PCI_MMIO >> 20));
}

/* Two equal devfns on different root buses must migrate independently. */
static void test_dual_base_io_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_DUAL);
    attach_node_ioc3(from, 0);
    attach_node_ioc3(from, 1);
    qtest_writeb(from, IOC3_SCRATCH_AT(0), 0x5a);
    qtest_writeb(from, IOC3_SCRATCH_AT(1), 0xa5);
    g_assert_cmphex(qtest_readb(from, IOC3_SCRATCH_AT(0)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(from, IOC3_SCRATCH_AT(1)), ==, 0xa5);

    dest_cmdline = g_strdup_printf(MACHINE_DUAL " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readb(to, IOC3_SCRATCH_AT(0)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(to, IOC3_SCRATCH_AT(1)), ==, 0xa5);

    qtest_quit(to);
    qtest_quit(from);
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

/*
 * Route entries are guest-programmed network state. Their initial value is
 * also zero, so a migration test must write non-zero values before the move;
 * otherwise a destination that silently drops the version-16 arrays passes.
 */
static void test_network_routes_survive_migration(const void *data)
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
    qtest_writeq(from, HUB_LOCAL + NI_LOCAL_TABLE, 0x1b);
    qtest_writeq(from, HUB_LOCAL + NI_META_TABLE_LAST, 0x16);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + NI_LOCAL_TABLE), ==, 0xb);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + NI_META_TABLE_LAST), ==, 6);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_LOCAL_TABLE), ==, 0xb);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + NI_META_TABLE_LAST), ==, 6);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * A first START is split across two guest writes: the address reaches the
 * PCF8584 data register before the control write asserts START.  Migrate in
 * that interval, then advance the destination far enough for the ELSC's
 * arbitration timer to fire.  The staged-address state must move with the
 * byte or the next broadcast replaces it before START consumes it.
 */
static void test_staged_i2c_start_survives_migration(const void *data)
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
    qtest_writeq(from, HUB_LOCAL + I2C_CTRL, I2C_CTL_PIN | I2C_CTL_ESO);
    qtest_writeq(from, HUB_LOCAL + I2C_DATA, ELSC_ADDR << 1);
    g_assert_cmphex(qtest_readq(from, HUB_LOCAL + I2C_DATA), ==,
                    ELSC_ADDR << 1);

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    qtest_clock_step(to, 500 * 1000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_DATA), ==,
                    ELSC_ADDR << 1);
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL,
                 I2C_CTL_PIN | I2C_CTL_ESO | I2C_CTL_STA | I2C_CTL_ACK);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + I2C_CTRL) & I2C_STS_LRB,
                    ==, 0);
    qtest_writeq(to, HUB_LOCAL + I2C_CTRL,
                 I2C_CTL_PIN | I2C_CTL_ESO | I2C_CTL_STO);

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

/*
 * The block transfer engines' registers are the whole of an engine's state
 * -- a transfer completes inside the store that launches it, so there is
 * never an in-flight transfer for the stream to carry -- but "the bit
 * pattern survives" is the weak half of what has to hold. The strong half
 * is that the moved state is still live: an engine armed on the source must
 * launch on the destination from nothing but its migrated registers, so
 * this arms engine 0 (busy and length set, addresses and interrupt word
 * programmed, control not yet stored) before the move and stores control
 * only on the far side.
 */
static void test_bte_state_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    enum { LINES = 2, SIZE = LINES * BTE_LINE };
    uint8_t expect[SIZE], got[SIZE];
    unsigned int i;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE);

    for (i = 0; i < SIZE; i++) {
        expect[i] = 0x5c ^ i;
    }
    qtest_bufwrite(from, 0x00700000, expect, SIZE);
    qtest_memset(from, 0x00740000, 0x3d, SIZE);

    /* Engine 1 carries plain register values, distinct in every field. */
    qtest_writeq(from, HUB_LOCAL + BTE_CONTROL(1), BTE_CTL_NOTIFY);
    qtest_writeq(from, HUB_LOCAL + BTE_LENGTH(1), 0x0abc);
    qtest_writeq(from, HUB_LOCAL + BTE_SOURCE(1), 0x00600000);
    qtest_writeq(from, HUB_LOCAL + BTE_DEST(1), 0x00610000);
    qtest_writeq(from, HUB_LOCAL + BTE_NOTIFY(1), 0x00620000);
    qtest_writeq(from, HUB_LOCAL + BTE_INTERRUPT(1), BTE_INT_LEVEL(41) | 3);

    /* Engine 0 is armed and not yet launched. */
    qtest_writeq(from, HUB_LOCAL + BTE_LENGTH(0), BTE_BUSY | LINES);
    qtest_writeq(from, HUB_LOCAL + BTE_SOURCE(0), 0x00700000);
    qtest_writeq(from, HUB_LOCAL + BTE_DEST(0), 0x00740000);
    qtest_writeq(from, HUB_LOCAL + BTE_INTERRUPT(0), BTE_INT_LEVEL(20));

    dest_cmdline = g_strdup_printf(MACHINE " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_CONTROL(1)), ==,
                    BTE_CTL_NOTIFY);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_LENGTH(1)), ==, 0x0abc);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_SOURCE(1)), ==,
                    0x00600000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_DEST(1)), ==,
                    0x00610000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_NOTIFY(1)), ==,
                    0x00620000);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_INTERRUPT(1)), ==,
                    BTE_INT_LEVEL(41) | 3);

    /* The armed engine launches on the destination. */
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_LENGTH(0)), ==,
                    BTE_BUSY | LINES);
    qtest_writeq(to, HUB_LOCAL + BTE_CONTROL(0), 0);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + BTE_LENGTH(0)), ==, 0);
    qtest_bufread(to, 0x00740000, got, SIZE);
    g_assert_cmpmem(got, SIZE, expect, SIZE);
    g_assert_cmphex(qtest_readq(to, HUB_LOCAL + PI_INT_PEND0), ==,
                    1ULL << 20);

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

static void test_enabled_recorder_remains_wired_after_migration(
    const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    QDict *source_state;
    QDict *destination_state;

    tmpfs = g_dir_make_tmp("sn0-migration-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MACHINE_RECORDING);
    qtest_writeq(from, HUB_LOCAL + PI_REGION_PRESENT,
                 0x0123456789abcdefULL);
    source_state = query_origin_state(from);
    g_assert_true(has_hub_write(source_state, PI_REGION_PRESENT,
                                0x0123456789abcdefULL));

    dest_cmdline = g_strdup_printf(MACHINE_RECORDING " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    migrate_stopped_vm(from, uri);

    destination_state = query_origin_state(to);
    g_assert_true(qdict_get_bool(qdict_get_qdict(destination_state,
                                                "flight-recorder"),
                                 "enabled"));
    g_assert_false(has_hub_write(destination_state, PI_REGION_PRESENT,
                                 0x0123456789abcdefULL));
    qobject_unref(destination_state);

    qtest_writeq(to, HUB_LOCAL + PI_REGION_PRESENT,
                 0xfedcba9876543210ULL);
    destination_state = query_origin_state(to);
    g_assert_true(has_hub_write(destination_state, PI_REGION_PRESENT,
                                0xfedcba9876543210ULL));

    qobject_unref(destination_state);
    qobject_unref(source_state);
    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/migration/device-window-survives-migration",
                        NULL, test_a_device_window_survives_migration);
    qtest_add_data_func("sn0/migration/dual-base-io-survives-migration",
                        NULL, test_dual_base_io_survives_migration);
    qtest_add_data_func("sn0/migration/network-routes-survive-migration",
                        NULL, test_network_routes_survive_migration);
    qtest_add_data_func("sn0/migration/staged-i2c-start-survives-migration",
                        NULL, test_staged_i2c_start_survives_migration);
    qtest_add_data_func("sn0/migration/conversation-survives-migration",
                        NULL, test_a_conversation_survives_migration);
    qtest_add_data_func("sn0/migration/bte-state-survives-migration",
                        NULL, test_bte_state_survives_migration);
    qtest_add_data_func("sn0/migration/flash-autoselect-survives-migration",
                        NULL, test_flash_autoselect_survives_migration);
    qtest_add_data_func("sn0/migration/flash-program-command-"
                        "survives-migration",
                        NULL, test_flash_program_command_survives_migration);
    qtest_add_data_func("sn0/migration/enabled-recorder-remains-wired",
                        NULL,
                        test_enabled_recorder_remains_wired_after_migration);

    return g_test_run();
}
