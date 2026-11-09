/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* CP0 identity using original synthetic instructions, without a guest image. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"

#define MAILBOX 0x1000

static char *image_path;

static QTestState *start(const char *machine, const char *cpu,
                        const char *extra)
{
    g_autofree char *quoted = g_shell_quote(image_path);

    return qtest_initf("-M %s -m 128M -cpu %s -bios %s "
                       "-nic none -accel tcg -S %s",
                       machine, cpu, quoted, extra);
}

static void read_identity(QTestState *qts, uint32_t expected)
{
    SgiWait w;

    /* Clear completion while stopped to require a new executed CP0 read. */
    qtest_writel(qts, MAILBOX + 8, 0);
    qtest_writel(qts, MAILBOX + 12, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&w, "guest CP0 identity read");
    while (!qtest_readl(qts, MAILBOX + 8)) {
        sgi_wait_pause(&w, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    g_assert_cmphex(qtest_readl(qts, MAILBOX), ==, expected);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 4), ==, expected);
}

/* The FPU identification register the last read_identity() captured. */
static void assert_fpu_id(QTestState *qts, uint32_t expected)
{
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 20), ==, expected);
}

static void test_identity(gconstpointer data)
{
    const char *machine = data;
    uint32_t expected = !strcmp(machine, "octane") ? 0x927 : 0x926;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start(machine, "R10000", "");

    read_identity(qts, expected);
    for (unsigned int i = 0; i < 2; i++) {
        qtest_system_reset(qts);
        read_identity(qts, expected);
    }
    qtest_quit(qts);
}

static uint32_t cpu_property(QTestState *qts, const char *name)
{
    g_autoptr(QDict) cpus = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    QDict *cpu = qobject_to(QDict,
                            qlist_peek(qdict_get_qlist(cpus, "return")));
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        qdict_get_str(cpu, "qom-path"), name);

    return qdict_get_int(reply, "return");
}

/*
 * A plain Octane2 takes its R12000 profile from the catalogue: PRId and
 * FPU identification from one kernel's inventory record, and the
 * approximated reset word with the 2 MiB, 32-word cache it selects. A
 * -cpu property still overrides its own field.
 */
static void test_octane2_identity(gconstpointer data)
{
    bool override = GPOINTER_TO_INT(data);
    g_autofree char *quoted = g_shell_quote(image_path);
    uint32_t expected = override ? 0xe24 : 0xe35;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf("-M octane2 %s -bios %s -nic none -accel tcg -S",
                      override ? "-cpu R12000,r12000-prid=0xe24" : "",
                      quoted);
    read_identity(qts, expected);
    g_assert_cmphex(cpu_property(qts, "r12000-prid"), ==, expected);
    g_assert_cmphex(cpu_property(qts, "r12000-fpu-id"), ==, 0x900);
    g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, 0x1aa683);
    qtest_system_reset(qts);
    read_identity(qts, expected);
    qtest_quit(qts);
}

/*
 * Octane's catalogue PRId is a default: -cpu R10000,r10000-prid= names
 * another part, as it does on every SGI machine.
 */
static void test_octane_prid_override(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start("octane", "R10000,r10000-prid=0x926", "");
    read_identity(qts, 0x926);
    g_assert_cmphex(cpu_property(qts, "r10000-prid"), ==, 0x926);
    qtest_system_reset(qts);
    read_identity(qts, 0x926);
    qtest_quit(qts);
}

/*
 * The SN machines apply their catalogue part the same way: Origin 200's
 * catalogue PRId is 0x926, and -cpu R10000,r10000-prid= names another.
 * The board's mode bits still reach the CPU's reset-mode, which sizes the
 * secondary cache, whichever PRId it takes: Config0 bits 24:0 of the
 * catalogue's 0x1211a785, the 1 MB part.
 */
static void test_origin200_prid(gconstpointer data)
{
    bool override = GPOINTER_TO_INT(data);
    QTestState *qts = qtest_initf("-M origin200 -cpu %s -S",
                                  override ? "R10000,r10000-prid=0x927" :
                                             "R10000");

    g_assert_cmphex(cpu_property(qts, "r10000-prid"), ==,
                    override ? 0x927 : 0x926);
    g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, 0x11a785);
    qtest_quit(qts);
}

typedef struct Origin200Processor {
    const char *options;
    const char *prid_property;
    uint32_t prid;
    const char *mode_property;
    uint32_t mode;
} Origin200Processor;

/*
 * Each Origin 200 processor module brings its own part: the PRId its
 * inventory prints (revision 2.6, 3.4 or 2.3) and Config0 bits 24:0 of
 * its mode word, which select the cache size (SS, bits 18:16) and clock
 * divisors the module's inventory reports. Expected words are written
 * from those fields, not read back from the catalogue.
 */
static const Origin200Processor origin200_processors[] = {
    /* SS=2 (2 MiB), cache clock 1:1, system clock divided by 2. */
    { "origin200,processor=r10000-180mhz-2mb -cpu R10000",
      "r10000-prid", 0x926, "reset-mode", 0x0aa785 },
    /* SS=2, cache clock 1:1, system clock divided by 2.5. */
    { "origin200,processor=r10000-225mhz-2mb -cpu R10000 -smp 2",
      "r10000-prid", 0x934, "reset-mode", 0x0aa985 },
    /* SS=3 (4 MiB), cache clock divided by 1.5, system clock by 3. */
    { "origin200,processor=r12000-270mhz-4mb -cpu R12000 -smp 2",
      "r12000-prid", 0xe23, "reset-mode", 0x13ab85 },
};

static void test_origin200_processor(gconstpointer data)
{
    const Origin200Processor *p = data;
    QTestState *qts = qtest_initf("-M %s -S", p->options);

    g_assert_cmphex(cpu_property(qts, p->prid_property), ==, p->prid);
    g_assert_cmphex(cpu_property(qts, p->mode_property), ==, p->mode);
    if (g_str_equal(p->prid_property, "r12000-prid")) {
        g_assert_cmphex(cpu_property(qts, "r12000-fpu-id"), ==, 0x900);
    }
    qtest_quit(qts);
}

/*
 * Fuel's catalogue part is R14000 revision 2.3 and Origin 300's the
 * model's 0x0f24, each applied through r14000-prid, which -cpu
 * R14000,r14000-prid= overrides. No machine picks a CPU type by PRId.
 * Either way the board's mode bits, the 2 MB part's 0x9aa985, still reach
 * the CPU's reset-mode, which sizes the secondary cache.
 */
static void test_sn1_prid(gconstpointer data)
{
    const char *machine = data;
    bool fuel = g_str_equal(machine, "fuel");
    g_autofree char *prom = NULL;
    g_autofree uint8_t *zero = g_malloc0(0x1686a8);
    int fd = g_file_open_tmp("r14000-identity-prom-XXXXXX", &prom, NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, zero, 0x1686a8), ==, 0x1686a8);
    close(fd);
    qts = qtest_initf("-M %s -cpu R14000 -smp %s -m 512M -bios %s -S",
                      machine, fuel ? "1" : "2", prom);
    g_assert_cmphex(cpu_property(qts, "r14000-prid"), ==,
                    fuel ? 0xf23 : 0xf24);
    g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, 0x9aa985);
    qtest_quit(qts);
    qts = qtest_initf("-M %s -cpu R14000,r14000-prid=%s -smp %s -m 512M "
                      "-bios %s -S", machine, fuel ? "0xf24" : "0xf23",
                      fuel ? "1" : "2", prom);
    g_assert_cmphex(cpu_property(qts, "r14000-prid"), ==,
                    fuel ? 0xf24 : 0xf23);
    g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, 0x9aa985);
    qtest_quit(qts);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * Every board drives its processor's mode word at reset, so a -cpu
 * reset-mode that disagrees with it is refused rather than run with a
 * cache the board never selected.
 */
static void test_board_reset_mode_conflict(void)
{
    const struct {
        const char *machine;
        const char *cpu;
        const char *smp;
        const char *memory;
        bool bios;
    } boards[] = {
        { "origin200", "R10000,reset-mode=0x11a784", "1", "128M" },
        { "origin200,processor=r12000-270mhz-4mb",
          "R12000,reset-mode=0x13ab84", "2", "128M" },
        { "octane2", "R12000,reset-mode=0x1aa682", "1", "128M", true },
        { "fuel", "R14000,reset-mode=0x9aa984", "1", "512M" },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(boards); i++) {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", boards[i].machine,
            "-cpu", boards[i].cpu, "-smp", boards[i].smp,
            "-m", boards[i].memory, "-display", "none", "-S",
            "-accel", "qtest", "-qtest", "null",
            boards[i].bios ? "-bios" : NULL, image_path, NULL,
        };
        g_autofree char *error_output = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                                   NULL, &error_output, &status, NULL));
        g_assert_false(g_spawn_check_exit_status(status, NULL));
        g_assert_nonnull(strstr(error_output, "conflicts with the board's"));
    }
}

static void test_invalid_identity(void)
{
    const char *profiles[] = {
        "R10000,r10000-prid=0xe27",
        "R10000,r10000-prid=0x10927",
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(profiles); i++) {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", "malta", "-cpu", profiles[i],
            "-display", "none", "-S", NULL,
        };
        g_autofree char *error_output = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                                   NULL, &error_output, &status, NULL));
        g_assert_false(g_spawn_check_exit_status(status, NULL));
        g_assert_nonnull(strstr(error_output, "requires implementation 0x09"));
    }
}

/*
 * R14000 parts differ by revision: the model's default PRId is 0x0f24 and
 * r14000-prid selects another, across reset as r10000-prid does. The FPU
 * identification register stays 0x0900, implementation 0x09 and revision 0,
 * whatever the PRId: that value is inferred for the C-brick parts (the only
 * raw read is a revision 2.4 Octane), and it is not derived from the PRId
 * revision, so a part printed as 1.4 or 2.3 does not change it.
 */
static void test_r14000_identity(gconstpointer data)
{
    static const uint32_t prids[] = { 0xf24, 0xf23, 0xf14 };
    uint32_t expected = prids[GPOINTER_TO_INT(data)];
    g_autofree char *cpu = g_strdup_printf("R14000,r14000-prid=0x%x",
                                           expected);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start("malta", GPOINTER_TO_INT(data) ? cpu : "R14000", "");
    read_identity(qts, expected);
    assert_fpu_id(qts, 0x900);
    g_assert_cmphex(cpu_property(qts, "r14000-prid"), ==, expected);
    qtest_system_reset(qts);
    read_identity(qts, expected);
    assert_fpu_id(qts, 0x900);
    qtest_quit(qts);
}

static void test_r14000_invalid_identity(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "malta", "-cpu",
        "R14000,r14000-prid=0x926", "-display", "none", "-S", NULL,
    };
    g_autofree char *error_output = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "r14000-prid requires "
                            "implementation 0x0f"));
}

/*
 * Each R16000 part is the CPU model plus the PRId revision its assembly
 * prints and the Config0 reset word of its clock and cache. Expected
 * words are written from the printed revision (major nibble is the
 * printed major plus 2) and the reset word's fields, not read back from
 * the machine catalogue. Config0 reads the model's 32 KiB primary caches
 * (IC and DC, 3 each) above the board's bits 24:0.
 */
typedef struct R16000Part {
    const char *name;
    uint32_t prid;
    uint32_t reset_mode;
} R16000Part;

static const R16000Part r16000_parts[] = {
    /* Revision 2.1, 700 MHz, 4 MiB: SS 3, EC 6, tap 0xc outside the word. */
    { "R16000", 0x0f41, 0x009bad85 },
    /* Revision 2.2, 800 MHz, 4 MiB: SS 3, EC 7. */
    { "R16000", 0x0f42, 0x009baf85 },
    /* Revision 2.2, 800 MHz, 8 MiB: SS 4, EC 7. */
    { "R16000", 0x0f42, 0x009caf85 },
    /* The 1.0 GHz part keeps the model's PRId: revision 3.0, 16 MiB. */
    { "R16000", 0x0f50, 0x00edb385 },
};

/* Whether the CPU definition table offers @model, by query-cpu-definitions. */
static bool cpu_model_offered(QTestState *qts, const char *model)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
                                       "{'execute':'query-cpu-definitions'}");
    QList *list = qdict_get_qlist(reply, "return");

    for (const QListEntry *e = qlist_first(list); e; e = qlist_next(e)) {
        QDict *def = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(def, "name"), model)) {
            return true;
        }
    }
    return false;
}

static void test_r16000_part(gconstpointer data)
{
    const R16000Part *part = data;
    g_autofree char *cpu = g_strdup_printf(
        "%s,big-endian=on,r16000-prid=0x%x,reset-mode=0x%x",
        part->name, part->prid, part->reset_mode);
    uint32_t config0 = (3u << 29) | (3u << 26) | part->reset_mode;
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start("malta", cpu, "");
    g_assert_true(cpu_model_offered(qts, part->name));
    read_identity(qts, part->prid);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 16), ==, config0);
    g_assert_cmphex(cpu_property(qts, "r16000-prid"), ==, part->prid);
    g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, part->reset_mode);
    qtest_system_reset(qts);
    read_identity(qts, part->prid);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 16), ==, config0);
    qtest_quit(qts);
}

/* Without r16000-prid the model keeps the 1.0 GHz part's PRId. */
static void test_r16000_default_prid(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start("malta", "R16000,big-endian=on,reset-mode=0x00edb385", "");
    read_identity(qts, 0x0f50);
    g_assert_cmphex(cpu_property(qts, "r16000-prid"), ==, 0x0f50);
    qtest_quit(qts);
}

static void test_r16000_invalid_identity(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "malta", "-cpu",
        "R16000,big-endian=on,reset-mode=0x009bad85,r16000-prid=0x926",
        "-display", "none", "-S", NULL,
    };
    g_autofree char *error_output = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(error_output, "r16000-prid requires "
                            "implementation 0x0f"));
}

/*
 * The IP53 machines' 700 MHz profiles fit the revision 2.1 part their
 * assemblies print, not the 1.0 GHz part's revision 3.0; -cpu
 * r16000-prid= still names another. The reset word is the board's.
 */
typedef struct Ip53Machine {
    const char *machine;
    const char *memory;
} Ip53Machine;

static const Ip53Machine ip53_machines[] = {
    { "origin350", "2G" },
    { "tezro,topology=tezro-ip53", "4G" },
};

static void test_ip53_prid(gconstpointer data)
{
    const Ip53Machine *m = data;
    /* Machine validation wants a full-size PROM, the IO9 its two stores. */
    g_autofree char *dir = g_dir_make_tmp("r16000-identity-XXXXXX", NULL);
    g_autofree char *prom = g_build_filename(dir, "prom.bin", NULL);
    g_autofree char *data_store = g_build_filename(dir, "ds1744.bin", NULL);
    g_autofree char *clock_store = g_build_filename(dir, "ds1742.bin", NULL);
    g_autofree uint8_t *zero = g_malloc0(1476264);
    const char *cpus[] = { "R16000", "R16000,r16000-prid=0x0f42" };
    const uint32_t prids[] = { 0x0f41, 0x0f42 };

    g_assert_true(g_file_set_contents(prom, (char *)zero, 1476264, NULL));
    g_assert_true(g_file_set_contents(data_store, (char *)zero, 0x8000, NULL));
    g_assert_true(g_file_set_contents(clock_store, (char *)zero, 16, NULL));
    for (size_t i = 0; i < G_N_ELEMENTS(cpus); i++) {
        QTestState *qts = qtest_initf(
            "-M %s,nvram0=sgi-ds1742-data-io9,"
            "nvram0-clock=sgi-ds1742-clock-io9 -cpu %s -smp 4 -m %s "
            "-bios %s -drive if=none,id=sgi-ds1742-data-io9,file=%s,"
            "format=raw -drive if=none,id=sgi-ds1742-clock-io9,file=%s,"
            "format=raw -S", m->machine, cpus[i], m->memory, prom,
            data_store, clock_store);

        g_assert_cmphex(cpu_property(qts, "r16000-prid"), ==, prids[i]);
        g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, 0x009bad85);
        qtest_quit(qts);
    }
    g_assert_cmpint(g_unlink(prom), ==, 0);
    g_assert_cmpint(g_unlink(data_store), ==, 0);
    g_assert_cmpint(g_unlink(clock_store), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/*
 * Every IP53 offering fits the part its board prints: the revision's PRId
 * and the Config0 reset word of its clock and cache. Expected values are
 * written from the printed revision (major nibble is the printed major
 * plus 2) and the reset word's fields (SS 3 or 4 for 4 or 8 MiB, system
 * clock divisor and cache clock divisor from the clocks), not read back
 * from the catalogue. The flash word is the stock PROM's table row for the
 * clock and cache, whose bits 28:25 are the tap the PROM prints. Config0
 * reads the model's 32 KiB primary caches (IC and DC, 3 each) above the
 * board's bits 24:0.
 */
typedef struct Ip53Offering {
    const char *machine;
    const char *memory;
    uint32_t prid;
    uint32_t reset_mode;
    uint32_t flash_word;
} Ip53Offering;

static const Ip53Offering ip53_offerings[] = {
    /* Revision 2.1, 700 MHz, 4 MiB (the default): table row 26, tap 0xc. */
    { "origin350,topology=origin350-ip53", "2G", 0x0f41, 0x009bad85, 0x589bad85 },
    { "tezro,topology=tezro-ip53", "4G", 0x0f41, 0x009bad85, 0x589bad85 },
    { "tezro,topology=tezro-rack-ip53", "4G", 0x0f41, 0x009bad85, 0x589bad85 },
    /* Revision 2.2, 800 MHz, 8 MiB: SS 4, SysClkDiv 7; row 21, tap 0xa. */
    { "origin350,topology=origin350-ip53,processor=r16000-800mhz-8mb-rev22",
      "2G", 0x0f42, 0x009caf85, 0x549caf85 },
    { "tezro,topology=tezro-ip53,processor=r16000-800mhz-8mb-rev22",
      "4G", 0x0f42, 0x009caf85, 0x549caf85 },
    { "tezro,topology=tezro-rack-ip53,processor=r16000-800mhz-8mb-rev22",
      "4G", 0x0f42, 0x009caf85, 0x549caf85 },
    /* Revision 2.2, 800 MHz, 4 MiB: SS 3, SysClkDiv 7; row 20, tap 0xa. */
    { "tezro,topology=tezro-ip53,processor=r16000-800mhz-4mb-rev22",
      "4G", 0x0f42, 0x009baf85, 0x549baf85 },
    { "tezro,topology=tezro-rack-ip53,processor=r16000-800mhz-4mb-rev22",
      "4G", 0x0f42, 0x009baf85, 0x549baf85 },
};

/* Config0 as the monitor prints the first CPU's registers. */
static uint32_t cpu_config0(QTestState *qts)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers");
    const char *at = strstr(regs, "Config0 ");

    g_assert_nonnull(at);
    return g_ascii_strtoull(at + strlen("Config0 "), NULL, 16);
}

/* The word the node's flash configuration block carries. */
static uint32_t node_flash_word(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/node0',"
        "'property':'firmware-mode-word'}}");

    return qdict_get_int(reply, "return");
}

static void test_ip53_offering(gconstpointer data)
{
    const Ip53Offering *o = data;
    g_autofree char *dir = g_dir_make_tmp("r16000-identity-XXXXXX", NULL);
    g_autofree char *prom = g_build_filename(dir, "prom.bin", NULL);
    g_autofree char *data_store = g_build_filename(dir, "ds1744.bin", NULL);
    g_autofree char *clock_store = g_build_filename(dir, "ds1742.bin", NULL);
    g_autofree uint8_t *zero = g_malloc0(1476264);
    QTestState *qts;

    g_assert_true(g_file_set_contents(prom, (char *)zero, 1476264, NULL));
    g_assert_true(g_file_set_contents(data_store, (char *)zero, 0x8000, NULL));
    g_assert_true(g_file_set_contents(clock_store, (char *)zero, 16, NULL));
    qts = qtest_initf(
        "-M %s,nvram0=sgi-ds1742-data-io9,"
        "nvram0-clock=sgi-ds1742-clock-io9 -cpu R16000 -smp 4 -m %s "
        "-bios %s -drive if=none,id=sgi-ds1742-data-io9,file=%s,"
        "format=raw -drive if=none,id=sgi-ds1742-clock-io9,file=%s,"
        "format=raw -S", o->machine, o->memory, prom, data_store,
        clock_store);

    g_assert_cmphex(cpu_property(qts, "r16000-prid"), ==, o->prid);
    g_assert_cmphex(cpu_property(qts, "reset-mode"), ==, o->reset_mode);
    g_assert_cmphex(cpu_config0(qts) & 0x01ffffff, ==, o->reset_mode);
    g_assert_cmphex(cpu_config0(qts) >> 26, ==, 0x1b);
    g_assert_cmphex(node_flash_word(qts), ==, o->flash_word);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
    g_assert_cmpint(g_unlink(data_store), ==, 0);
    g_assert_cmpint(g_unlink(clock_store), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_immutable_identity(void)
{
    QTestState *qts;
    g_autoptr(QDict) cpus = NULL;
    g_autoptr(QDict) reply = NULL;
    QDict *cpu, *error;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start("malta", "R10000", "");
    cpus = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    cpu = qobject_to(QDict, qlist_peek(qdict_get_qlist(cpus, "return")));
    reply = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'r10000-prid','value':2343}}",
        qdict_get_str(cpu, "qom-path"));
    error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "cannot change after CPU realization"));
    read_identity(qts, 0x926);
    qtest_quit(qts);
}

/*
 * One migration of a CPU's PRId: the source's -cpu, the destination's, and
 * whether the destination must refuse the stream. A refused stream is one
 * whose PRId differs from the value the destination's reset would install.
 * The Fuel case runs the R14000 profile on its own machine, with the blank
 * PROM the SN1 case uses, so it reads the property rather than executing.
 */
typedef struct MigrationCase {
    const char *machine;
    const char *from_cpu;
    const char *to_cpu;
    const char *property;
    uint32_t prid;
    bool mismatch;
} MigrationCase;

static const MigrationCase r10000_mismatch = {
    "malta", "R10000,r10000-prid=0x927", "R10000", "r10000-prid", 0x927, true,
};
static const MigrationCase r10000_octane = {
    "octane", "R10000,r10000-prid=0x927", "R10000", "r10000-prid", 0x927,
    false,
};
static const MigrationCase r14000_mismatch = {
    "malta", "R14000,r14000-prid=0xf23", "R14000", "r14000-prid", 0xf23, true,
};
static const MigrationCase r14000_fuel = {
    "fuel", "R14000", "R14000", "r14000-prid", 0xf23, false,
};

static QTestState *start_migration_side(const MigrationCase *c,
                                        const char *cpu, const char *prom,
                                        const char *extra)
{
    if (prom) {
        return qtest_initf("-M %s -cpu %s -smp 1 -m 512M -bios %s "
                           "-nic none -accel tcg -S %s",
                           c->machine, cpu, prom, extra);
    }
    return start(c->machine, cpu, extra);
}

static void test_migration(gconstpointer data)
{
    const MigrationCase *c = data;
    bool sn1 = g_str_equal(c->machine, "fuel");
    g_autofree char *dir = g_dir_make_tmp("r10000-identity-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    g_autofree char *prom = NULL;
    QTestState *from, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        g_assert_cmpint(g_rmdir(dir), ==, 0);
        return;
    }
    if (sn1) {
        g_autofree uint8_t *zero = g_malloc0(0x1686a8);

        prom = g_build_filename(dir, "prom.bin", NULL);
        g_assert_true(g_file_set_contents(prom, (char *)zero, 0x1686a8,
                                          NULL));
    }
    from = start_migration_side(c, c->from_cpu, prom, "");
    to = start_migration_side(c, c->to_cpu, prom, incoming);

    if (sn1) {
        g_assert_cmphex(cpu_property(from, c->property), ==, c->prid);
    } else {
        read_identity(from, c->prid);
    }
    if (c->mismatch) {
        SgiWait w;

        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        sgi_wait_start(&w, "destination exit after a refused migration");
        while (qtest_probe_child(to)) {
            sgi_wait_pause(&w, 1000);
        }
    } else {
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        if (sn1) {
            g_assert_cmphex(cpu_property(to, c->property), ==, c->prid);
            qtest_system_reset(to);
            g_assert_cmphex(cpu_property(to, c->property), ==, c->prid);
        } else {
            read_identity(to, c->prid);
            qtest_system_reset(to);
            read_identity(to, c->prid);
        }
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    if (prom) {
        g_unlink(prom);
    }
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    /*
     * Each loop reads PRId, attempts a write, rereads it, reads Config0,
     * enables the FPU and reads its identification register, then
     * publishes the results.
     */
    static const uint32_t code[] = {
        0x3c1aa000,             /* lui k0, 0xa000 */
        0x375a1000,             /* ori k0, k0, 0x1000 */
        0x8f48000c,             /* lw t0, 12(k0): wait for a fresh request */
        0x1100fffe,             /* beq t0, zero, request loop */
        0,                     /* delay slot */
        0xaf40000c,             /* sw zero, 12(k0): consume request */
        0x40087800,             /* mfc0 t0, PRId */
        0xaf480000,             /* sw t0, 0(k0) */
        0x2409ffff,             /* addiu t1, zero, -1 */
        0x40897800,             /* mtc0 t1, PRId */
        0, 0, 0, 0,           /* CP0 write hazard spacing */
        0x40087800,             /* mfc0 t0, PRId */
        0xaf480004,             /* sw t0, 4(k0) */
        0x40088000,             /* mfc0 t0, Config0 */
        0xaf480010,             /* sw t0, 16(k0) */
        0x40086000,             /* mfc0 t0, Status */
        0x3c092000,             /* lui t1, 0x2000: CU1 */
        0x01094025,             /* or t0, t0, t1 */
        0x40886000,             /* mtc0 t0, Status */
        0, 0, 0, 0,           /* CP0 write hazard spacing */
        0x44480000,             /* cfc1 t0, FIR */
        0,                     /* load delay */
        0xaf480014,             /* sw t0, 20(k0) */
        0x24080001,             /* addiu t0, zero, 1 */
        0xaf480008,             /* sw t0, 8(k0) */
        0x1000ffe2,             /* branch to request loop */
        0,                     /* delay slot */
    };
    uint8_t image[0x2000] = { 0 };
    int fd, result;

    g_test_init(&argc, &argv, NULL);
    for (unsigned int i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + i * 4, code[i]);
    }
    fd = g_file_open_tmp("r10000-identity-code-XXXXXX", &image_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    qtest_add_data_func("/r10000/identity/octane", "octane", test_identity);
    qtest_add_func("/r10000/identity/octane-override",
                   test_octane_prid_override);
    qtest_add_data_func("/r10000/identity/fuel", "fuel", test_sn1_prid);
    qtest_add_data_func("/r10000/identity/origin300", "origin300",
                        test_sn1_prid);
    qtest_add_data_func("/r10000/identity/origin200", GINT_TO_POINTER(0),
                        test_origin200_prid);
    qtest_add_data_func("/r10000/identity/origin200-override",
                        GINT_TO_POINTER(1), test_origin200_prid);
    for (size_t i = 0; i < G_N_ELEMENTS(origin200_processors); i++) {
        g_autofree char *name = g_strdup_printf(
            "/r10000/identity/origin200-processor/%zu", i);

        qtest_add_data_func(name, &origin200_processors[i],
                            test_origin200_processor);
    }
    qtest_add_func("/r10000/identity/board-reset-mode-conflict",
                   test_board_reset_mode_conflict);
    qtest_add_data_func("/r10000/identity/octane2", GINT_TO_POINTER(0),
                        test_octane2_identity);
    qtest_add_data_func("/r10000/identity/octane2-override",
                        GINT_TO_POINTER(1), test_octane2_identity);
    if (qtest_has_machine("malta")) {
        qtest_add_data_func("/r10000/identity/default", "malta", test_identity);
        qtest_add_func("/r10000/identity/invalid", test_invalid_identity);
        qtest_add_func("/r10000/identity/immutable", test_immutable_identity);
        qtest_add_data_func("/r10000/identity/r14000-default",
                            GINT_TO_POINTER(0), test_r14000_identity);
        qtest_add_data_func("/r10000/identity/r14000-override",
                            GINT_TO_POINTER(1), test_r14000_identity);
        qtest_add_data_func("/r10000/identity/r14000-override-rev14",
                            GINT_TO_POINTER(2), test_r14000_identity);
        qtest_add_func("/r10000/identity/r14000-invalid",
                       test_r14000_invalid_identity);
        for (size_t i = 0; i < G_N_ELEMENTS(r16000_parts); i++) {
            g_autofree char *name = g_strdup_printf(
                "/r10000/identity/r16000-part/0x%x-0x%x", r16000_parts[i].prid,
                r16000_parts[i].reset_mode);

            qtest_add_data_func(name, &r16000_parts[i], test_r16000_part);
        }
        qtest_add_func("/r10000/identity/r16000-default",
                       test_r16000_default_prid);
        qtest_add_func("/r10000/identity/r16000-invalid",
                       test_r16000_invalid_identity);
        qtest_add_data_func("/r10000/identity/migration-mismatch",
                            &r10000_mismatch, test_migration);
        qtest_add_data_func("/r10000/identity/r14000-migration-mismatch",
                            &r14000_mismatch, test_migration);
    }
    qtest_add_data_func("/r10000/identity/origin350-prid",
                        &ip53_machines[0], test_ip53_prid);
    qtest_add_data_func("/r10000/identity/tezro-prid",
                        &ip53_machines[1], test_ip53_prid);
    for (size_t i = 0; i < G_N_ELEMENTS(ip53_offerings); i++) {
        g_autofree char *name = g_strdup_printf(
            "/r10000/identity/ip53-offering/%s", ip53_offerings[i].machine);

        qtest_add_data_func(name, &ip53_offerings[i], test_ip53_offering);
    }
    qtest_add_data_func("/r10000/identity/migration",
                        &r10000_octane, test_migration);
    qtest_add_data_func("/r10000/identity/r14000-migration-fuel",
                        &r14000_fuel, test_migration);
    result = g_test_run();
    unlink(image_path);
    g_free(image_path);
    return result;
}
