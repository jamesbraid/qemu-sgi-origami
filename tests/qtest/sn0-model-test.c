/*
 * The Origin 200 machine, checked against the model it is supposed to be.
 *
 * The architecture contract says the configuration graph the firmware
 * synthesises and the devices the machine instantiates come from one topology
 * and cannot disagree. Until now nothing checked that: it held as long as
 * whoever edited the machine file remembered to edit the firmware builder to
 * match, and the failure it guards against is a guest that boots, discovers a
 * machine slightly unlike the one that was built, and dies somewhere unrelated
 * with nothing in the log.
 *
 * This is that check. Both descriptions are read back out of a running machine
 * and compared against the model they are both supposed to be projections of:
 *
 *   - the configuration graph, read straight out of node memory, field by
 *     field, against what tools/gen-klconfig generates from the model;
 *   - the object graph, walked over the monitor, against the devices the model
 *     says this machine has.
 *
 * The expectations are generated rather than written here, so this file cannot
 * drift from the model by being edited: see sn0-model.h, which
 * tools/check-model-header proves is what regenerating produces.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

#include "sn0-model.h"
#include "sn0-model-dual-assert.h"

#include <elf.h>

#define SN0_PROCESSOR_MODE_BITS 0x1211a785U
#define SN0_PROCESSOR_MODE_MASK 0x01ffffffU
#define SN0_PROMDATA_PADDR      UINT64_C(0x00100000)
#define SN0_PROMDATA_SIZE       UINT64_C(0x00008000)
#define SN0_HUB_LOCAL           0x20001000000ULL
#define SN0_NODE_STRIDE         0x100000000ULL
#define SN0_HUB_NODE_WINDOW     0x01800000ULL
#define SN0_NI_STATUS           0x00600000ULL
#define SN0_IO_WIDGET_ID        0x00400000ULL
#define SN0_NI_LINK_UP          (1ULL << 29)
#define SN0_NI_NODE_SHIFT       8
#define SN0_PI_CPU_PRESENT_A    0x000040
#define SN0_PI_CPU_PRESENT_B    0x000048
#define SN0_GRAPH_NODE_BOARD    0x4100
#define SN0_GRAPH_BOARD_SIZE    0xc0
#define SN0_GRAPH_MIDPLANE      (SN0_GRAPH_NODE_BOARD + SN0_GRAPH_BOARD_SIZE)
#define SN0_GRAPH_BASE_IO       (SN0_GRAPH_MIDPLANE + SN0_GRAPH_BOARD_SIZE)
#define SN0_GRAPH_COMPONENTS    0x4400
#define SN0_GRAPH_COMP_STRIDE   0x80
#define SN0_BOARD_BASE_IO       0x21
#define SN0_COMP_PROCESSOR      1
#define SN0_COMP_ENABLED        1

#define SN0_DUAL_CHASSIS_TOPOLOGY \
    "-m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

#define SN0_DUAL_CHASSIS_ARGS \
    "-M origin200,synthesized-firmware=off " SN0_DUAL_CHASSIS_TOPOLOGY
#define SN0_DUAL_CHASSIS_FIRMWARE_ARGS \
    "-M origin200 " SN0_DUAL_CHASSIS_TOPOLOGY

#define SN0_SPACE_XTALK         0x20000000000ULL
#define SN0_HUB_BY_NODE(node)   (SN0_SPACE_XTALK + \
                                 (uint64_t)(node) * SN0_NODE_STRIDE + \
                                 SN0_HUB_NODE_WINDOW)
#define SN0_BRIDGE_WIDGET       8
#define SN0_BRIDGE_BASE(node)   (SN0_SPACE_XTALK + \
                                 (uint64_t)(node) * SN0_NODE_STRIDE + \
                                 ((uint64_t)SN0_BRIDGE_WIDGET << 24))
#define SN0_BRIDGE_CFG0         0x020000
#define SN0_CFG0_DEVICE_STRIDE  0x1000
#define SN0_CFG0(node, device)  (SN0_BRIDGE_BASE(node) + SN0_BRIDGE_CFG0 + \
                                 (device) * SN0_CFG0_DEVICE_STRIDE)
#define SN0_BRIDGE_REG_DATA     4
#define SN0_BRIDGE_DEVICE_REG(node, device) \
    (SN0_BRIDGE_BASE(node) + 0x200 + 8 * (device) + SN0_BRIDGE_REG_DATA)
#define SN0_BRIDGE_DEVICE2      0x600000
#define SN0_IOC3_SERIAL_A       0x020178
#define SN0_IOC3_SCRATCH        7

#define SN0_SCSI_DEVICE0        0
#define SN0_SCSI_DEVICE1        1
#define SN0_IOC3_DEVICE         2
#define SN0_QLOGIC_ID           0x10201077
#define SN0_IOC3_ID             0x000310a9

#define SN0_PCI_COMMAND         0x04
#define SN0_PCI_REVISION_ID     0x08
#define SN0_PCI_COMMAND_IO      0x0001
#define SN0_PCI_COMMAND_MEMORY  0x0002
#define SN0_PCI_COMMAND_MASTER  0x0004
#define SN0_PCI_BAR0            0x10
#define SN0_PCI_BAR1            0x14
#define SN0_PCI_SCSI0_MEM       0x08200000U
#define SN0_PCI_SCSI1_MEM       0x08400000U
#define SN0_PCI_IOC3_MEM        0x08600000U
#define SN0_TEST_IOC3_MEM       0x10000000U
#define SN0_CPU_ID_ADDR         0x00030000U
#define SN0_FPU_ID_ADDR         0x00030004U

#define SN0_DEV_ERROR_LOCK      (1u << 28)
#define SN0_DEV_PAGE_CROSS      (1u << 27)
#define SN0_DEV_MEMORY_SPACE    (1u << 12)

static void assert_pci_root_bus_name(QTestState *qts, const char *bridge,
                                     const char *expected)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     bridge);
    QList *children;
    const QListEntry *entry;
    const char *name = NULL;

    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *type = qdict_get_try_str(child, "type");

        if (type && g_str_equal(type, "child<PCI>")) {
            g_assert_null(name);
            name = qdict_get_str(child, "name");
        }
    }

    g_assert_nonnull(name);
    g_assert_cmpstr(name, ==, expected);
    qobject_unref(response);
}

static char *write_prid_guest(void)
{
    static const uint32_t code[] = {
        0x40087800,             /* mfc0 t0, PRId */
        0x3c099600,             /* lui t1, 0x9600 */
        0x0009483c,             /* dsll32 t1, t1, 0 */
        0x3c0a0003,             /* lui t2, 0x0003 */
        0x012a4825,             /* or t1, t1, t2 */
        0xad280000,             /* sw t0, 0(t1) */
        0x400a6000,             /* mfc0 t2, Status */
        0x3c0b2000,             /* lui t3, 0x2000 -- Status.CU1 */
        0x014b5025,             /* or t2, t2, t3 */
        0x408a6000,             /* mtc0 t2, Status */
        0x00000000,             /* nop */
        0x00000000,             /* nop */
        0x44480000,             /* cfc1 t0, FIR */
        0xad280004,             /* sw t0, 4(t1) */
        0x1000ffff,             /* b . */
        0x00000000,             /* nop */
    };
    uint8_t image[0x1000 + sizeof(code)] = { 0 };
    const uint64_t entry = 0x8000000000020000ULL;
    GError *err = NULL;
    char *path = NULL;
    int fd;
    size_t i;

    fd = g_file_open_tmp("sn0-prid-guest-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    image[0] = ELFMAG0;
    image[1] = ELFMAG1;
    image[2] = ELFMAG2;
    image[3] = ELFMAG3;
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stq_be_p(image + 24, entry);
    stq_be_p(image + 32, 64);
    stw_be_p(image + 52, 64);
    stw_be_p(image + 54, 56);
    stw_be_p(image + 56, 1);

    stl_be_p(image + 64, PT_LOAD);
    stl_be_p(image + 68, PF_R | PF_X);
    stq_be_p(image + 72, 0x1000);
    stq_be_p(image + 80, entry);
    stq_be_p(image + 88, entry);
    stq_be_p(image + 96, sizeof(code));
    stq_be_p(image + 104, sizeof(code));
    stq_be_p(image + 112, 0x1000);

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x1000 + i * sizeof(code[0]), code[i]);
    }

    g_assert_cmpint(write(fd, image, sizeof(image)), ==,
                    (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * The same guest as a 64-bit ELF that declares an entry point with its
 * upper word clear -- a legal image, and one no 32-bit rule may touch. The
 * address it names is in the segment the processor translates through the
 * table, so nothing can run there on a machine that has just come out of
 * reset. That is the point: the marker below appears only if something
 * rewrote the entry into the compatibility segment on the image's behalf.
 */
static char *write_prid_guest_elf64_low_entry(void)
{
    static const uint32_t code[] = {
        0x40087800,             /* mfc0 t0, PRId */
        0x3c099600,             /* lui t1, 0x9600 */
        0x0009483c,             /* dsll32 t1, t1, 0 */
        0x3c0a0003,             /* lui t2, 0x0003 */
        0x012a4825,             /* or t1, t1, t2 */
        0xad280000,             /* sw t0, 0(t1) */
        0x1000ffff,             /* b . */
        0x00000000,             /* nop */
    };
    uint8_t image[0x1000 + sizeof(code)] = { 0 };
    const uint64_t entry = 0x0000000080020000ULL;
    GError *err = NULL;
    char *path = NULL;
    int fd;
    size_t i;

    fd = g_file_open_tmp("sn0-prid-guest64-low-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    image[0] = ELFMAG0;
    image[1] = ELFMAG1;
    image[2] = ELFMAG2;
    image[3] = ELFMAG3;
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stq_be_p(image + 24, entry);
    stq_be_p(image + 32, 64);              /* e_phoff */
    stw_be_p(image + 52, 64);              /* e_ehsize */
    stw_be_p(image + 54, 56);              /* e_phentsize */
    stw_be_p(image + 56, 1);               /* e_phnum */

    stl_be_p(image + 64, PT_LOAD);
    stl_be_p(image + 68, PF_R | PF_X);
    stq_be_p(image + 72, 0x1000);          /* p_offset */
    stq_be_p(image + 80, entry);           /* p_vaddr */
    stq_be_p(image + 88, entry & 0x1fffffff); /* p_paddr: physical */
    stq_be_p(image + 96, sizeof(code));    /* p_filesz */
    stq_be_p(image + 104, sizeof(code));   /* p_memsz */
    stq_be_p(image + 112, 0x1000);         /* p_align */

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x1000 + i * sizeof(code[0]), code[i]);
    }

    g_assert_cmpint(write(fd, image, sizeof(image)), ==,
                    (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * The same guest linked as a 32-bit ELF, the class IRIX kernels ship as.
 * Its segment loads at a physical address and its entry point is the
 * kseg0 address whose upper word the ELF header cannot carry: the entry
 * must be sign-extended into the compatibility segment, or the processor
 * starts at an unmapped low address and the marker below never appears.
 */
static char *write_prid_guest_elf32(void)
{
    static const uint32_t code[] = {
        0x40087800,             /* mfc0 t0, PRId */
        0x3c099600,             /* lui t1, 0x9600 */
        0x0009483c,             /* dsll32 t1, t1, 0 */
        0x3c0a0003,             /* lui t2, 0x0003 */
        0x012a4825,             /* or t1, t1, t2 */
        0xad280000,             /* sw t0, 0(t1) */
        0x1000ffff,             /* b . */
        0x00000000,             /* nop */
    };
    uint8_t image[0x1000 + sizeof(code)] = { 0 };
    const uint32_t entry = 0x80020000U;
    GError *err = NULL;
    char *path = NULL;
    int fd;
    size_t i;

    fd = g_file_open_tmp("sn0-prid-guest32-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    image[0] = ELFMAG0;
    image[1] = ELFMAG1;
    image[2] = ELFMAG2;
    image[3] = ELFMAG3;
    image[EI_CLASS] = ELFCLASS32;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stl_be_p(image + 24, entry);
    stl_be_p(image + 28, 52);              /* e_phoff */
    stw_be_p(image + 40, 52);              /* e_ehsize */
    stw_be_p(image + 42, 32);              /* e_phentsize */
    stw_be_p(image + 44, 1);               /* e_phnum */

    stl_be_p(image + 52, PT_LOAD);
    stl_be_p(image + 56, 0x1000);          /* p_offset */
    stl_be_p(image + 60, entry);           /* p_vaddr: kseg0 */
    stl_be_p(image + 64, entry & 0x1fffffff); /* p_paddr: physical */
    stl_be_p(image + 68, sizeof(code));    /* p_filesz */
    stl_be_p(image + 72, sizeof(code));    /* p_memsz */
    stl_be_p(image + 76, PF_R | PF_X);
    stl_be_p(image + 80, 0x1000);          /* p_align */

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x1000 + i * sizeof(code[0]), code[i]);
    }

    g_assert_cmpint(write(fd, image, sizeof(image)), ==,
                    (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_a_32_bit_kernel_entry_is_sign_extended(const void *data)
{
    g_autofree char *kernel = write_prid_guest_elf32();
    QTestState *qts;
    uint32_t prid = 0;

    qts = qtest_initf("-M origin200,synthesized-firmware=off "
                      "-accel tcg -smp 1 -kernel %s", kernel);

    g_test_timer_start();
    while (prid == 0 && g_test_timer_elapsed() < 1) {
        prid = qtest_readl(qts, SN0_CPU_ID_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(prid, ==, SN0_MODEL_CPU_PRID);
    qtest_quit(qts);
    g_unlink(kernel);
}

/*
 * The other half of that rule: the class in the file decides, not the shape
 * of the entry value. A 64-bit image naming an address whose upper word is
 * clear means that address, so the machine must hand the processor exactly
 * what the image asked for -- which on a machine out of reset reaches
 * nothing, and the marker never appears. The 32-bit image runs immediately
 * afterwards under the same options and the same budget, so the silence
 * above is the entry point rather than a slow machine or a broken marker.
 */
static void test_a_64_bit_kernel_entry_is_left_alone(const void *data)
{
    g_autofree char *low_entry = write_prid_guest_elf64_low_entry();
    g_autofree char *elf32 = write_prid_guest_elf32();
    QTestState *qts;
    uint32_t prid = 0;

    qts = qtest_initf("-M origin200,synthesized-firmware=off "
                      "-accel tcg -smp 1 -kernel %s", low_entry);

    g_test_timer_start();
    while (prid == 0 && g_test_timer_elapsed() < 1) {
        prid = qtest_readl(qts, SN0_CPU_ID_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(prid, ==, 0);
    qtest_quit(qts);
    g_unlink(low_entry);

    qts = qtest_initf("-M origin200,synthesized-firmware=off "
                      "-accel tcg -smp 1 -kernel %s", elf32);

    g_test_timer_start();
    while (prid == 0 && g_test_timer_elapsed() < 1) {
        prid = qtest_readl(qts, SN0_CPU_ID_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(prid, ==, SN0_MODEL_CPU_PRID);
    qtest_quit(qts);
    g_unlink(elf32);
}

/*
 * A disk named onto the second adapter. Both adapters on a base I/O board
 * are ordinary SCSI buses, numbered in the order the machine creates them,
 * so -drive if=scsi,bus=1 names the second one -- and a machine that offers
 * its command-line drives to the first adapter alone leaves that drive
 * attached to nothing, which ends startup with "machine type does not
 * support if=scsi,bus=1". Asking for the block device's owner afterwards is
 * what distinguishes a machine that took the drive from one that merely
 * started.
 */
static void test_a_drive_on_the_second_adapter(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 "
                                 "-drive if=scsi,bus=1,file=null-co://,"
                                 "format=raw");
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-block' }");
    QList *blocks;
    const QListEntry *entry;
    bool found = false;

    g_assert_true(qdict_haskey(response, "return"));
    blocks = qdict_get_qlist(response, "return");
    for (entry = qlist_first(blocks); entry; entry = qlist_next(entry)) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));
        QDict *inserted = qdict_haskey(block, "inserted") ?
                          qdict_get_qdict(block, "inserted") : NULL;
        const char *file = inserted ? qdict_get_try_str(inserted, "file")
                                    : NULL;

        /*
         * This drive rather than any other backing the machine carries: the
         * file is what names it, and an owner on it is the whole assertion.
         * Only the adapter numbered one can have claimed a bus=1 drive, so
         * an owner at all says it landed where it was addressed.
         */
        if (file && g_str_equal(file, "null-co://")) {
            g_assert_false(found);
            found = true;
            g_assert_nonnull(qdict_get_try_str(block, "qdev"));
        }
    }

    g_assert_true(found);
    qobject_unref(response);
    qtest_quit(qts);
}

static void test_baseline_cpu_identity(const void *data)
{
    g_autofree char *kernel = write_prid_guest();
    QTestState *qts;
    uint32_t prid = 0;
    uint32_t fpu_prid = 0;

    qts = qtest_initf("-M origin200,synthesized-firmware=off "
                      "-accel tcg -smp 1 -kernel %s", kernel);

    /* The guest publishes PRId before it enables the FPU and publishes FIR. */
    g_test_timer_start();
    while ((prid == 0 || fpu_prid == 0) && g_test_timer_elapsed() < 1) {
        prid = qtest_readl(qts, SN0_CPU_ID_ADDR);
        fpu_prid = qtest_readl(qts, SN0_FPU_ID_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(prid, ==, SN0_MODEL_CPU_PRID);
    g_assert_cmphex(fpu_prid, ==, SN0_MODEL_FPU_PRID);
    qtest_quit(qts);
    g_unlink(kernel);
}

void sn0_model_assert_device_identity(QTestState *qts, unsigned int node,
                                      const SN0ModelDeviceIdentity *expected)
{
    uint64_t hub = SN0_HUB_BY_NODE(node);
    uint64_t ni = qtest_readq(qts, hub + SN0_NI_STATUS);
    uint64_t hub_widget = qtest_readq(qts,
                                      hub + SN0_IO_WIDGET_ID);
    uint32_t bridge = qtest_readl(qts, SN0_BRIDGE_BASE(node) + 4);

    g_assert_cmpuint((ni >> 4) & 0xf, ==, expected->hub_ni_revision);
    g_assert_cmpuint((hub_widget >> 28) & 0xf, ==,
                     expected->hub_widget_revision);
    g_assert_cmpuint(bridge >> 28, ==, expected->bridge_revision);
    g_assert_cmpuint(qtest_readl(qts, SN0_CFG0(node, SN0_IOC3_DEVICE) +
                                      SN0_PCI_REVISION_ID) & 0xff, ==,
                     expected->ioc3_pci_revision);
    g_assert_cmpuint(qtest_readl(qts, SN0_CFG0(node, SN0_SCSI_DEVICE0) +
                                      SN0_PCI_REVISION_ID) & 0xff, ==,
                     expected->isp1040_pci_revision);
    g_assert_cmpuint(qtest_readl(qts, SN0_CFG0(node, SN0_SCSI_DEVICE1) +
                                      SN0_PCI_REVISION_ID) & 0xff, ==,
                     expected->isp1040_pci_revision);
}

static void test_baseline_device_identity(const void *data)
{
    const SN0ModelDeviceIdentity expected =
        SN0_MODEL_DEVICE_IDENTITY_INIT;
    QTestState *qts = qtest_init("-M origin200");

    sn0_model_assert_device_identity(qts, 0, &expected);
    qtest_quit(qts);
}

/*
 * Each physical chassis owns a Bridge and therefore a separate PCI root bus.
 * Keep the original bus name for node 0, since it is part of existing device
 * migration paths, and give node 1 a stable identity of its own before it
 * grows devices at the same devfns.
 */
static void test_bridge_root_bus_names(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_ARGS);

    assert_pci_root_bus_name(qts, "/machine/bridge", "pci");
    assert_pci_root_bus_name(qts, "/machine/bridge1", "pci.1");

    qtest_quit(qts);
}

static void attach_ioc3(QTestState *qts, unsigned int node)
{
    qtest_writel(qts, SN0_CFG0(node, SN0_IOC3_DEVICE) + SN0_PCI_BAR0,
                 SN0_TEST_IOC3_MEM);
    qtest_writel(qts, SN0_CFG0(node, SN0_IOC3_DEVICE) + SN0_PCI_COMMAND,
                 SN0_PCI_COMMAND_MEMORY);
    qtest_writel(qts, SN0_BRIDGE_DEVICE_REG(node, SN0_IOC3_DEVICE),
                 SN0_DEV_MEMORY_SPACE | (SN0_TEST_IOC3_MEM >> 20));
}

static uint64_t ioc3_scratch(unsigned int node)
{
    return SN0_BRIDGE_BASE(node) + SN0_BRIDGE_DEVICE2 +
           SN0_IOC3_SERIAL_A + SN0_IOC3_SCRATCH;
}

/*
 * Each chassis carries a complete BaseIO assembly. Probe all three fixed PCI
 * identities through each node-qualified Bridge, then write different values
 * into the two IOC3s so an accidental alias cannot satisfy the test.
 */
static void test_dual_chassis_base_io(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_ARGS);
    unsigned int node;

    for (node = 0; node < 2; node++) {
        g_assert_cmphex(qtest_readl(qts,
                            SN0_CFG0(node, SN0_SCSI_DEVICE0)), ==,
                        SN0_QLOGIC_ID);
        g_assert_cmphex(qtest_readl(qts,
                            SN0_CFG0(node, SN0_SCSI_DEVICE1)), ==,
                        SN0_QLOGIC_ID);
        g_assert_cmphex(qtest_readl(qts,
                            SN0_CFG0(node, SN0_IOC3_DEVICE)), ==,
                        SN0_IOC3_ID);
        attach_ioc3(qts, node);
    }

    qtest_writeb(qts, ioc3_scratch(0), 0x5a);
    qtest_writeb(qts, ioc3_scratch(1), 0xa5);
    g_assert_cmphex(qtest_readb(qts, ioc3_scratch(0)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, ioc3_scratch(1)), ==, 0xa5);

    qtest_quit(qts);
}

static char *qom_get_string(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-get',"
             " 'arguments': { 'path': %s, 'property': %s } }",
        path, property);
    char *value;

    g_assert_true(qdict_haskey(response, "return"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return value;
}

/*
 * Each IOC3 carries a separately programmed station address in the one-wire
 * identity that the stock PROM reads.  A host backend is optional, so the
 * unbacked second controller still needs the board's SGI OUI and a distinct
 * suffix.  The IOC3 one-wire qtests separately prove that this QOM property
 * is the value returned by the identity part.
 */
static void test_dual_chassis_ioc3_network_identity(const void *data)
{
    g_autofree char *mac0 = NULL;
    g_autofree char *mac1 = NULL;
    g_autofree char *netdev0 = NULL;
    g_autofree char *netdev1 = NULL;
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_ARGS
                                 " -nic user,id=n0");

    mac0 = qom_get_string(qts, "/machine/bridge/pci/child[0]", "mac");
    mac1 = qom_get_string(qts, "/machine/bridge1/pci.1/child[0]", "mac");
    netdev0 = qom_get_string(qts, "/machine/bridge/pci/child[0]", "netdev");
    netdev1 = qom_get_string(qts, "/machine/bridge1/pci.1/child[0]", "netdev");

    g_assert_true(g_str_has_prefix(mac0, "08:00:69:"));
    g_assert_true(g_str_has_prefix(mac1, "08:00:69:"));
    g_assert_cmpstr(mac0, !=, mac1);
    g_assert_cmpstr(netdev0, ==, "n0");
    g_assert_cmpstr(netdev1, ==, "");

    qtest_quit(qts);

    g_clear_pointer(&mac0, g_free);
    g_clear_pointer(&mac1, g_free);
    g_clear_pointer(&netdev0, g_free);
    g_clear_pointer(&netdev1, g_free);

    qts = qtest_init(SN0_DUAL_CHASSIS_ARGS
                     " -nic user,id=n0,mac=02:11:22:33:44:50"
                     " -nic user,id=n1,mac=02:11:22:33:44:51");

    mac0 = qom_get_string(qts, "/machine/bridge/pci/child[0]", "mac");
    mac1 = qom_get_string(qts, "/machine/bridge1/pci.1/child[0]", "mac");
    netdev0 = qom_get_string(qts, "/machine/bridge/pci/child[0]", "netdev");
    netdev1 = qom_get_string(qts, "/machine/bridge1/pci.1/child[0]", "netdev");

    g_assert_cmpstr(mac0, ==, "02:11:22:33:44:50");
    g_assert_cmpstr(mac1, ==, "02:11:22:33:44:51");
    g_assert_cmpstr(netdev0, ==, "n0");
    g_assert_cmpstr(netdev1, ==, "n1");

    qtest_quit(qts);
}

static void assert_base_io_graph(QTestState *qts, unsigned int node)
{
    uint64_t node_base = node * SN0_NODE_STRIDE;

    g_assert_cmphex(qtest_readl(qts, node_base + SN0_GRAPH_MIDPLANE), ==,
                    SN0_GRAPH_BASE_IO);
    g_assert_cmphex(qtest_readb(qts, node_base + SN0_GRAPH_BASE_IO + 5), ==,
                    SN0_BOARD_BASE_IO);
    g_assert_cmpuint(qtest_readb(qts,
                         node_base + SN0_GRAPH_BASE_IO + 0x17), ==, 4);
}

static void assert_base_io_resources(QTestState *qts, unsigned int node)
{
    static const unsigned int device[] = {
        SN0_SCSI_DEVICE0, SN0_SCSI_DEVICE1, SN0_IOC3_DEVICE,
    };
    static const uint32_t memory[] = {
        SN0_PCI_SCSI0_MEM, SN0_PCI_SCSI1_MEM, SN0_PCI_IOC3_MEM,
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(device); i++) {
        uint32_t bar = device[i] == SN0_IOC3_DEVICE ?
                       SN0_PCI_BAR0 : SN0_PCI_BAR1;
        uint32_t command = device[i] == SN0_IOC3_DEVICE ?
                           SN0_PCI_COMMAND_MEMORY |
                           SN0_PCI_COMMAND_MASTER :
                           SN0_PCI_COMMAND_IO | SN0_PCI_COMMAND_MEMORY |
                           SN0_PCI_COMMAND_MASTER;

        g_assert_cmphex(qtest_readl(qts, SN0_CFG0(node, device[i]) + bar), ==,
                        memory[i]);
        g_assert_cmphex(qtest_readl(qts,
                            SN0_CFG0(node, device[i]) + SN0_PCI_COMMAND) &
                        0xffff, ==, command);
        g_assert_cmphex(qtest_readl(qts,
                            SN0_BRIDGE_DEVICE_REG(node, device[i])), ==,
                        SN0_DEV_ERROR_LOCK | SN0_DEV_PAGE_CROSS |
                        SN0_DEV_MEMORY_SPACE | (memory[i] >> 20));
    }
}

/* Firmware assignment must follow each device's own reset on every chassis. */
static void test_dual_chassis_base_io_survives_reset(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_FIRMWARE_ARGS);

    assert_base_io_resources(qts, 0);
    assert_base_io_resources(qts, 1);
    assert_base_io_graph(qts, 0);
    assert_base_io_graph(qts, 1);
    qtest_system_reset(qts);
    assert_base_io_resources(qts, 0);
    assert_base_io_resources(qts, 1);

    qtest_quit(qts);
}

static uint32_t read_config0(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *config = strstr(registers, "Config0 ");
    uint32_t value;

    g_assert_nonnull(config);
    g_assert_cmpint(sscanf(config, "Config0 0x%x", &value), ==, 1);
    return value;
}

/*
 * The processor latches these bits from the board while reset is asserted.
 * They are hardware state even when QEMU's synthesized firmware is disabled:
 * the stock PROM compares them with the same word in flash before it can run
 * its own reset sequence. Checking both power-on and a later machine reset
 * catches callbacks that are either registered too late or wired only to the
 * synthesized-firmware path.
 */
static void test_processor_mode_bits_are_board_state(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,synthesized-firmware=off "
                                 "-cpu R10000 -m 64M");

    g_assert_cmphex(read_config0(qts) & SN0_PROCESSOR_MODE_MASK, ==,
                    SN0_PROCESSOR_MODE_BITS & SN0_PROCESSOR_MODE_MASK);

    qtest_system_reset(qts);
    g_assert_cmphex(read_config0(qts) & SN0_PROCESSOR_MODE_MASK, ==,
                    SN0_PROCESSOR_MODE_BITS & SN0_PROCESSOR_MODE_MASK);

    qtest_quit(qts);
}

/*
 * The graph sits in node memory, and the memory space is carrier window zero,
 * so a node offset is the address the memory system sees unchanged.
 */
static uint64_t read_field(QTestState *qts, uint64_t at, unsigned width)
{
    switch (width) {
    case 1:
        return qtest_readb(qts, at);
    case 2:
        return qtest_readw(qts, at);
    case 4:
        return qtest_readl(qts, at);
    case 8:
        return qtest_readq(qts, at);
    default:
        g_assert_not_reached();
    }
}

static bool valid_graph_field_width(unsigned int width)
{
    return width == 1 || width == 2 || width == 4 || width == 8;
}

bool sn0_model_reconstruct_graph(const SN0ModelConfigGraph *graph,
                                 uint8_t *image, size_t image_size)
{
    g_autofree bool *written = NULL;
    size_t i;

    if (graph->span == 0 || graph->size < graph->span ||
        image_size < graph->span) {
        g_test_message("node %u has an invalid graph span", graph->nasid);
        return false;
    }

    memset(image, 0, graph->span);
    written = g_new0(bool, graph->span);
    for (i = 0; i < graph->field_count; i++) {
        const SN0ModelField *field = &graph->fields[i];
        uint64_t at;
        unsigned int byte;

        if (!valid_graph_field_width(field->width)) {
            g_test_message("node %u %s has unsupported width %u",
                           graph->nasid, field->field, field->width);
            return false;
        }
        if (field->offset < graph->base || field->width > graph->span ||
            field->offset - graph->base > graph->span - field->width) {
            g_test_message("node %u %s is outside the generated graph span",
                           graph->nasid, field->field);
            return false;
        }
        if (field->width < sizeof(field->value) &&
            field->value >> (field->width * 8)) {
            g_test_message("node %u %s value 0x%" PRIx64
                           " does not fit width %u",
                           graph->nasid, field->field, field->value,
                           field->width);
            return false;
        }

        at = field->offset - graph->base;
        for (byte = 0; byte < field->width; byte++) {
            if (written[at + byte]) {
                g_test_message("node %u %s overlaps another generated field "
                               "at 0x%" PRIx64,
                               graph->nasid, field->field,
                               field->offset + byte);
                return false;
            }
        }
        for (byte = 0; byte < field->width; byte++) {
            image[at + field->width - byte - 1] =
                field->value >> (byte * 8);
            written[at + byte] = true;
        }
    }
    return true;
}

static const SN0ModelField *find_divergent_field(
    const SN0ModelConfigGraph *graph, const SN0ModelDivergence *divergence)
{
    size_t i;

    for (i = 0; i < graph->field_count; i++) {
        const SN0ModelField *field = &graph->fields[i];

        if (field->offset == divergence->offset &&
            field->width == divergence->width &&
            g_str_equal(field->field, divergence->field)) {
            return field;
        }
    }
    return NULL;
}

bool sn0_model_apply_graph_divergences(
    const SN0ModelConfigGraph *graph,
    const SN0ModelDivergence *divergences,
    const uint8_t *got, uint8_t *expected)
{
    size_t i;

    for (i = 0; divergences[i].field != NULL; i++) {
        const SN0ModelDivergence *divergence = &divergences[i];
        const SN0ModelField *field;
        uint64_t at;
        uint64_t machine = 0;
        unsigned int byte;

        if (!valid_graph_field_width(divergence->width) ||
            divergence->offset < graph->base ||
            divergence->width > graph->span ||
            divergence->offset - graph->base >
            graph->span - divergence->width) {
            g_test_message("known divergence %s is outside this graph span",
                           divergence->field);
            return false;
        }
        field = find_divergent_field(graph, divergence);
        if (!field) {
            g_test_message("known divergence %s no longer identifies an "
                           "exact generated field", divergence->field);
            return false;
        }
        if (field->value == divergence->machine_value) {
            g_test_message("known divergence %s is stale: model and machine "
                           "value are both 0x%" PRIx64,
                           divergence->field, field->value);
            return false;
        }

        at = divergence->offset - graph->base;
        for (byte = 0; byte < divergence->width; byte++) {
            machine = (machine << 8) | got[at + byte];
        }
        if (machine != divergence->machine_value) {
            g_test_message("known divergence %s expects machine value 0x%"
                           PRIx64 ", but the machine now has 0x%" PRIx64,
                           divergence->field, divergence->machine_value,
                           machine);
            return false;
        }
        for (byte = 0; byte < divergence->width; byte++) {
            expected[at + divergence->width - byte - 1] =
                divergence->machine_value >> (byte * 8);
        }
    }
    return true;
}

static uint64_t expected_machine_value(
    const SN0ModelField *field, const SN0ModelDivergence *divergences)
{
    size_t i;

    for (i = 0; divergences[i].field != NULL; i++) {
        const SN0ModelDivergence *divergence = &divergences[i];

        if (divergence->offset == field->offset) {
            g_assert_cmpuint(divergence->width, ==, field->width);
            g_assert_cmpstr(divergence->field, ==, field->field);
            g_assert_cmphex(divergence->machine_value, !=, field->value);
            return divergence->machine_value;
        }
    }
    return field->value;
}

void sn0_model_assert_graph_fields(
    QTestState *qts, const SN0ModelConfigGraph *graph,
    const SN0ModelDivergence *divergences)
{
    uint64_t node_base = graph->nasid * SN0_NODE_STRIDE;
    g_autofree uint8_t *validation = g_malloc0(graph->span);
    unsigned int memory_records = 0;
    size_t i;

    g_assert_true(sn0_model_reconstruct_graph(graph, validation,
                                               graph->span));
    for (i = 0; i < graph->field_count; i++) {
        const SN0ModelField *field = &graph->fields[i];
        uint64_t got = read_field(qts, node_base + field->offset,
                                  field->width);
        uint64_t wanted = expected_machine_value(field, divergences);

        if (got != wanted) {
            g_test_message("node %u %s at 0x%" PRIx64
                           ": model says 0x%" PRIx64
                           ", machine has 0x%" PRIx64,
                           graph->nasid, field->field,
                           node_base + field->offset, wanted, got);
        }
        g_assert_cmphex(got, ==, wanted);
        if (g_str_equal(field->field, "memory_bank.total_mb")) {
            g_assert_cmpuint(field->value, ==, 64);
            memory_records++;
        }
    }
    g_assert_cmpuint(memory_records, ==, 1);
}

void sn0_model_assert_graph_image(
    QTestState *qts, const SN0ModelConfigGraph *graph,
    const SN0ModelDivergence *divergences)
{
    uint64_t node_base = graph->nasid * SN0_NODE_STRIDE;
    g_autofree uint8_t *got = g_malloc0(graph->span);
    g_autofree uint8_t *expected = g_malloc0(graph->span);
    uint64_t off;

    g_assert_true(sn0_model_reconstruct_graph(graph, expected,
                                               graph->span));
    qtest_memread(qts, node_base + graph->base, got, graph->span);

    /* A zero-filled failed read must not pass against zero-filled gaps. */
    g_assert_cmpuint(graph->span, >, 4);
    g_assert_cmpuint(got[4], ==, 0xbe);
    g_assert_true(sn0_model_apply_graph_divergences(graph, divergences,
                                                    got, expected));
    for (off = 0; off < graph->span; off++) {
        if (got[off] != expected[off]) {
            g_test_message("node %u graph byte at 0x%" PRIx64
                           ": model says 0x%x, machine has 0x%x",
                           graph->nasid, node_base + graph->base + off,
                           expected[off], got[off]);
        }
        g_assert_cmphex(got[off], ==, expected[off]);
    }

    for (off = graph->span; off < graph->size; off += 4096) {
        size_t len = MIN(4096, graph->size - off);
        g_autofree uint8_t *tail = g_malloc0(len);
        size_t i;

        qtest_memread(qts, node_base + graph->base + off, tail, len);
        for (i = 0; i < len; i++) {
            if (tail[i] != 0) {
                g_test_message("node %u graph byte at 0x%" PRIx64
                               " is 0x%x; the model says the populated span "
                               "ends at 0x%" PRIx64,
                               graph->nasid,
                               node_base + graph->base + off + i, tail[i],
                               graph->span);
            }
            g_assert_cmpuint(tail[i], ==, 0);
        }
    }
}

/*
 * Every field the model says the graph carries, read back from where the
 * machine actually put it. A mismatch names the record and field rather than
 * the address, because an address alone sends the reader hunting.
 */
static void test_config_graph(const void *data)
{
    QTestState *qts = qtest_initf("-M %s -m %dM",
                                  SN0_MODEL_MACHINE, SN0_MODEL_RAM_MB);

    g_assert_cmpuint(SN0_MODEL_CONFIG_GRAPH_COUNT, ==, 1);
    g_assert_cmpuint(ARRAY_SIZE(sn0_model_config_graphs), ==, 1);
    g_assert_cmpuint(sn0_model_config_graphs[0].nasid, ==, 0);
    sn0_model_assert_graph_fields(qts, &sn0_model_config_graphs[0],
                                  sn0_model_divergences);

    qtest_quit(qts);
}

/*
 * Every byte of the graph region, not only the fields the model names. A field
 * the model forgets and the machine also leaves zero would agree trivially, and
 * prove nothing; comparing the whole region instead catches anything the
 * machine writes that the model does not account for.
 *
 * Where the two are known to differ, the model says so and says who owns the
 * decision. Those exceptions are honoured here and are also checked in the
 * other direction, so an exception that has stopped being true fails the test
 * rather than sitting in the file forever.
 */
static void test_graph_whole_region(const void *data)
{
    QTestState *qts = qtest_initf("-M %s -m %dM",
                                  SN0_MODEL_MACHINE, SN0_MODEL_RAM_MB);

    sn0_model_assert_graph_image(qts, &sn0_model_config_graphs[0],
                                 sn0_model_divergences);

    qtest_quit(qts);
}

static void increment_type_count(GHashTable *counts, const char *type)
{
    unsigned int count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, type));

    g_hash_table_replace(counts, g_strdup(type), GUINT_TO_POINTER(count + 1));
}

/* Count every "type" in the object graph under one path, recursively. */
static void collect_type_counts(QTestState *qts, const char *path,
                                GHashTable *counts, int depth)
{
    QDict *response;
    QList *children;
    const QListEntry *entry;

    /*
     * The tree is shallow. Running out of depth means it is not the tree this
     * test thinks it is, which is a failure rather than a reason to stop and
     * check whatever was collected so far.
     */
    g_assert_cmpint(depth, <=, 8);

    response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                              " 'arguments': { 'path': %s } }", path);
    /*
     * A path that came out of qom-list must answer qom-list. Treating a failure
     * here as "nothing to add" would let a missing subtree pass silently, which
     * is exactly the kind of hole that makes a green test meaningless.
     */
    if (!qdict_haskey(response, "return")) {
        g_test_message("qom-list failed for '%s'", path);
    }
    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");

    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_try_str(child, "name");
        const char *type = qdict_get_try_str(child, "type");
        g_autofree char *sub = NULL;

        if (!name || !type) {
            continue;
        }
        /*
         * A link is a reference to an object that lives elsewhere in the tree,
         * so following it would walk the same objects twice and could loop.
         */
        if (g_str_has_prefix(type, "link<")) {
            continue;
        }
        if (g_str_has_prefix(type, "child<")) {
            g_autofree char *bare = g_strndup(type + 6, strlen(type) - 7);

            increment_type_count(counts, bare);
            sub = g_strdup_printf("%s/%s", path, name);
            collect_type_counts(qts, sub, counts, depth + 1);
        }
    }
    qobject_unref(response);
}

void sn0_model_assert_object_graph(QTestState *qts, const char *machine_type,
                                   const SN0ModelDevice *expected_devices,
                                   size_t expected_count)
{
    GHashTable *actual = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    GHashTable *expected = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    QDict *response;
    const char *root;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    size_t i;

    /*
     * The machine itself, first. Without this the test would accept any machine
     * that happened to contain the right device types among its children.
     */
    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine',"
                              " 'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    root = qdict_get_str(response, "return");
    g_assert_cmpstr(root, ==, machine_type);
    qobject_unref(response);

    collect_type_counts(qts, "/machine", actual, 0);
    for (i = 0; i < expected_count; i++) {
        increment_type_count(expected, expected_devices[i].qom_type);
    }

    g_hash_table_iter_init(&iter, expected);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *type = key;
        unsigned int wanted = GPOINTER_TO_UINT(value);
        unsigned int got = GPOINTER_TO_UINT(g_hash_table_lookup(actual, type));

        if (got != wanted) {
            g_test_message("the selected model profile requires %u instances "
                           "of QOM type '%s', and the object graph has %u",
                           wanted, type, got);
        }
        g_assert_cmpuint(got, ==, wanted);
    }

    g_hash_table_destroy(expected);
    g_hash_table_destroy(actual);
}

/*
 * Every device type selected by the model, at the exact frequency selected
 * by this machine profile. The test checks the machine type separately and
 * leaves internal object paths free to change.
 */
static void test_object_graph(const void *data)
{
    QTestState *qts = qtest_initf("-M %s -m %dM",
                                  SN0_MODEL_MACHINE, SN0_MODEL_RAM_MB);

    sn0_model_assert_object_graph(qts, SN0_MODEL_MACHINE_TYPE,
                                  sn0_model_devices,
                                  ARRAY_SIZE(sn0_model_devices));
    qtest_quit(qts);
}

static QDict *find_qom_type(QList *types, const char *name)
{
    const QListEntry *entry;

    for (entry = qlist_first(types); entry; entry = qlist_next(entry)) {
        QDict *type = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(type, "name"), name)) {
            return type;
        }
    }
    return NULL;
}

/*
 * Product identity and shared architecture are separate QOM boundaries. The
 * command line still selects origin200, but its implementation derives from
 * an abstract SN0 machine and contains a node object that owns the Hub.
 */
static void test_architecture_boundaries(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    QDict *response;
    QList *types;
    QDict *type;

    response = qtest_qmp(qts, "{ 'execute': 'qom-list-types',"
                              " 'arguments': { 'implements': 'machine',"
                              "                  'abstract': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    types = qdict_get_qlist(response, "return");
    type = find_qom_type(types, "sn0-machine");
    g_assert_nonnull(type);
    g_assert_true(qdict_get_bool(type, "abstract"));
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-list-types',"
                              " 'arguments': { 'implements': 'sn0-machine',"
                              "                  'abstract': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    types = qdict_get_qlist(response, "return");
    type = find_qom_type(types, SN0_MODEL_MACHINE_TYPE);
    g_assert_nonnull(type);
    g_assert_false(qdict_haskey(type, "abstract") &&
                   qdict_get_bool(type, "abstract"));
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "sn0-node");
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0/hub',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "sgi-hub");
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0/cpu0',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "R10000-mips64-cpu");
    qobject_unref(response);

    qtest_quit(qts);
}

/*
 * The PROM's early C stack lives at the 0xa800... XKPHYS alias. Its cache
 * algorithm maps that alias to ordinary physical memory at 1 MiB. QEMU has no
 * primary data cache to keep those writes private, so each CPU's address space
 * must intercept that physical range before it reaches shared node RAM.
 */
static void test_promdata_uses_memory_space(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,synthesized-firmware=off "
                                 "-smp 2");
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *cpu0_mapping = g_strdup_printf(
        "%016" PRIx64 "-%016" PRIx64
        " (prio 2, i/o): sn0.node0.cpu0.promdata-filter",
        SN0_PROMDATA_PADDR,
        SN0_PROMDATA_PADDR + SN0_PROMDATA_SIZE - 1);
    g_autofree char *cpu1_mapping = g_strdup_printf(
        "%016" PRIx64 "-%016" PRIx64
        " (prio 2, i/o): sn0.node0.cpu1.promdata-filter",
        SN0_PROMDATA_PADDR,
        SN0_PROMDATA_PADDR + SN0_PROMDATA_SIZE - 1);

    g_assert_nonnull(strstr(mtree, cpu0_mapping));
    g_assert_nonnull(strstr(mtree, cpu1_mapping));

    qtest_quit(qts);
}

/*
 * An IP29 node has two processor slices.  Selecting both must create both CPU
 * objects and tell the node's Hub that both sockets are populated; accepting
 * -smp 2 without either half would expose a topology that firmware cannot use.
 */
static void test_one_node_smp(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -smp 2");
    QDict *response;

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0/cpu0',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "R10000-mips64-cpu");
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0/cpu1',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "R10000-mips64-cpu");
    qobject_unref(response);

    g_assert_cmphex(qtest_readq(qts, SN0_HUB_LOCAL + SN0_PI_CPU_PRESENT_A),
                    ==, 1);
    g_assert_cmphex(qtest_readq(qts, SN0_HUB_LOCAL + SN0_PI_CPU_PRESENT_B),
                    ==, 1);

    g_assert_cmpuint(qtest_readb(qts, SN0_GRAPH_NODE_BOARD + 0x17), ==, 4);
    g_assert_cmphex(qtest_readl(qts, SN0_GRAPH_NODE_BOARD + 0x28), ==,
                    SN0_GRAPH_COMPONENTS + SN0_GRAPH_COMP_STRIDE);
    g_assert_cmpuint(qtest_readb(qts, SN0_GRAPH_COMPONENTS +
                                     SN0_GRAPH_COMP_STRIDE), ==,
                     SN0_COMP_PROCESSOR);
    g_assert_cmpuint(qtest_readb(qts, SN0_GRAPH_COMPONENTS +
                                     SN0_GRAPH_COMP_STRIDE + 0x02), ==,
                     SN0_COMP_ENABLED);
    g_assert_cmpuint(qtest_readb(qts, SN0_GRAPH_COMPONENTS +
                                     SN0_GRAPH_COMP_STRIDE + 0x18), ==, 1);
    g_assert_cmphex(qtest_readl(qts, SN0_GRAPH_COMPONENTS +
                                    SN0_GRAPH_COMP_STRIDE + 0x1c), ==, 1);

    qtest_quit(qts);
}

/*
 * QEMU's standard CPU-topology interface must describe the IP29 sockets, not
 * merely accept a scalar -smp value.  One Origin 200 chassis is one topology
 * socket, and its two processor slices are the two cores in that socket.
 */
static void test_one_node_cpu_topology(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -smp 2,sockets=1,cores=2,threads=1");
    QDict *response;
    QList *cpus;
    QObject *entry;
    unsigned int seen = 0;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert_true(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");

    while ((entry = qlist_pop(cpus))) {
        QDict *cpu = qobject_to(QDict, entry);
        QDict *props = qdict_get_qdict(cpu, "props");
        int64_t index = qdict_get_int(cpu, "cpu-index");

        g_assert_cmpint(index, >=, 0);
        g_assert_cmpint(index, <, 2);
        g_assert_cmpint(qdict_get_int(props, "socket-id"), ==, 0);
        g_assert_cmpint(qdict_get_int(props, "core-id"), ==, index);
        g_assert_cmpint(qdict_get_int(props, "thread-id"), ==, 0);
        seen |= 1U << index;
        qobject_unref(entry);
    }
    g_assert_cmphex(seen, ==, 0x3);

    qobject_unref(response);
    qtest_quit(qts);
}

/*
 * An explicit NUMA description is the operator-facing way to select physical
 * Origin 200 nodes.  Even the one-node form matters: it proves that a memory
 * backend survives QEMU's generic NUMA setup and that the standard CPU
 * topology callbacks attach both IP29 slices to the declared node.
 */
static void test_explicit_one_node_numa(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 64M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-smp 2,sockets=1,cores=2,threads=1");
    QDict *response;
    QList *cpus;
    QObject *entry;
    unsigned int seen = 0;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert_true(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");

    while ((entry = qlist_pop(cpus))) {
        QDict *cpu = qobject_to(QDict, entry);
        QDict *props = qdict_get_qdict(cpu, "props");
        int64_t index = qdict_get_int(cpu, "cpu-index");

        g_assert_cmpint(index, >=, 0);
        g_assert_cmpint(index, <, 2);
        g_assert_cmpint(qdict_get_int(props, "node-id"), ==, 0);
        seen |= 1U << index;
        qobject_unref(entry);
    }
    g_assert_cmphex(seen, ==, 0x3);

    qobject_unref(response);
    qtest_quit(qts);
}

static void test_dual_chassis_numa(const void *data)
{
    QTestState *qts = qtest_init(
        "-M origin200 -m 128M "
        "-object memory-backend-ram,id=ram0,size=64M "
        "-object memory-backend-ram,id=ram1,size=64M "
        "-numa node,nodeid=0,cpus=0-1,memdev=ram0 "
        "-numa node,nodeid=1,cpus=2-3,memdev=ram1 "
        "-smp 4,sockets=2,cores=2,threads=1");
    QDict *response;
    QList *cpus;
    QObject *entry;
    unsigned int seen = 0;
    uint64_t node1_hub = SN0_HUB_BY_NODE(1);
    uint64_t status;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert_true(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    while ((entry = qlist_pop(cpus))) {
        QDict *cpu = qobject_to(QDict, entry);
        QDict *props = qdict_get_qdict(cpu, "props");
        int64_t index = qdict_get_int(cpu, "cpu-index");

        g_assert_cmpint(index, >=, 0);
        g_assert_cmpint(index, <, 4);
        g_assert_cmpint(qdict_get_int(props, "node-id"), ==, index / 2);
        g_assert_cmpint(qdict_get_int(props, "socket-id"), ==, index / 2);
        g_assert_cmpint(qdict_get_int(props, "core-id"), ==, index % 2);
        seen |= 1U << index;
        qobject_unref(entry);
    }
    g_assert_cmphex(seen, ==, 0xf);
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node1',"
                              " 'property': 'type' } }");
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "sn0-node");
    qobject_unref(response);

    qtest_writel(qts, 0x1000, 0x11223344);
    qtest_writel(qts, SN0_NODE_STRIDE + 0x1000, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, 0x1000), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, SN0_NODE_STRIDE + 0x1000),
                    ==, 0x55667788);

    status = qtest_readq(qts, node1_hub + SN0_NI_STATUS);
    g_assert_cmphex(status & SN0_NI_LINK_UP, ==, SN0_NI_LINK_UP);
    g_assert_cmphex((status >> SN0_NI_NODE_SHIFT) & 0x1ff, ==, 1);

    qtest_quit(qts);
}

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

/*
 * The Bridge wires eight slots to eight interrupt pins; nothing past slot 7
 * exists on the chip. A device placed there must be refused when it is
 * added -- accepted, it would sit unreachable until its first interrupt
 * asserted a pin the bus never wired and aborted the emulator.
 */
/*
 * Crafted ECOFF kernels whose headers reach outside the file. Each of
 * these used to make the loader read past its buffer -- a verdict from
 * heap garbage at best, an abort under sanitizers -- and each must now be
 * refused with a clean load error before anything is read out of bounds.
 */
static char *write_truncated_ecoff(unsigned int variant)
{
    uint8_t image[0x100] = { 0 };
    size_t len = 20;
    GError *err = NULL;
    char *path = NULL;
    int fd;

    stw_be_p(image + 0x00, 0x0163);        /* big-endian MIPS ECOFF */
    stw_be_p(image + 0x02, 1);             /* one section */
    stw_be_p(image + 0x10, 0x38);          /* optional-header size */

    if (variant >= 1) {
        /* A full OMAGIC optional header follows the file header. */
        stw_be_p(image + 20 + 0x00, 0407);
        stl_be_p(image + 20 + 0x10, 0x80020000);
        stl_be_p(image + 20 + 0x34, 0x80030000);
        len = 20 + 0x38;
    }
    if (variant == 1) {
        /* A hundred section headers, none of them in the file. */
        stw_be_p(image + 0x02, 100);
    }
    if (variant == 2) {
        /* One section header whose contents run past the file's end. */
        stl_be_p(image + len + 0x0c, 0x1000);   /* vaddr */
        stl_be_p(image + len + 0x10, 0x1000);   /* size */
        stl_be_p(image + len + 0x14, 0x5000);   /* scnptr, past the end */
        len += 40;
    }

    fd = g_file_open_tmp("sn0-ecoff-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(write(fd, image, len), ==, (ssize_t)len);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * A minimal valid OMAGIC image: one text section holding the same
 * marker-writing guest the ELF tests boot. sashARCS is the real consumer
 * of this path; this pins the load mechanics without needing its binary.
 */
static char *write_prid_guest_ecoff(void)
{
    /*
     * The sash hand-off leaves CP0 Status at its reset state, so the
     * marker store goes through kseg1 -- the 32-bit uncached window a
     * fresh processor can always reach -- not the 64-bit space the ELF
     * guests use.
     */
    static const uint32_t code[] = {
        0x40087800,             /* mfc0 t0, PRId */
        0x3c09a003,             /* lui t1, 0xa003 -- kseg1 + 0x30000 */
        0xad280000,             /* sw t0, 0(t1) */
        0x1000ffff,             /* b . */
        0x00000000,             /* nop */
    };
    const uint32_t vaddr = 0x00030000 + 0x10000;
    uint8_t image[20 + 0x38 + 40 + sizeof(code)] = { 0 };
    size_t scnptr = 20 + 0x38 + 40;
    GError *err = NULL;
    char *path = NULL;
    unsigned int i;
    int fd;

    stw_be_p(image + 0x00, 0x0163);        /* big-endian MIPS ECOFF */
    stw_be_p(image + 0x02, 1);             /* one section */
    stw_be_p(image + 0x10, 0x38);          /* optional-header size */
    stw_be_p(image + 20 + 0x00, 0407);     /* OMAGIC */
    stl_be_p(image + 20 + 0x10, vaddr);    /* entry */
    stl_be_p(image + 20 + 0x34, vaddr);    /* gp, unused by this guest */
    stl_be_p(image + 20 + 0x38 + 0x0c, vaddr);
    stl_be_p(image + 20 + 0x38 + 0x10, sizeof(code));
    stl_be_p(image + 20 + 0x38 + 0x14, scnptr);
    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + scnptr + i * sizeof(code[0]), code[i]);
    }

    fd = g_file_open_tmp("sn0-ecoff-guest-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==,
                    (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_an_ecoff_kernel_boots(const void *data)
{
    g_autofree char *kernel = write_prid_guest_ecoff();
    QTestState *qts;
    uint32_t prid = 0;

    qts = qtest_initf("-M origin200,synthesized-firmware=on "
                      "-accel tcg -smp 1 -kernel %s", kernel);

    g_test_timer_start();
    while (prid == 0 && g_test_timer_elapsed() < 5) {
        prid = qtest_readl(qts, SN0_CPU_ID_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(prid, ==, SN0_MODEL_CPU_PRID);
    qtest_quit(qts);
    g_unlink(kernel);
}

static void test_reject_a_truncated_ecoff_kernel(const void *data)
{
    unsigned int variant;

    for (variant = 0; variant < 3; variant++) {
        g_autofree char *kernel = write_truncated_ecoff(variant);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", "origin200",
            "-kernel", kernel, "-display", "none", NULL
        };

        assert_qemu_rejects(args, "the ECOFF headers reach outside the file");
        g_unlink(kernel);
    }
}

/*
 * A default drive belongs on the first SCSI adapter. It used to be
 * accepted and attached to nothing: the drive existed, no device claimed
 * it, and a guest booting from "the first disk" found an empty bus.
 */
static void test_default_drive_reaches_the_first_scsi_adapter(const void *data)
{
    g_autofree char *img = NULL;
    GError *err = NULL;
    int fd = g_file_open_tmp("sn0-hda-XXXXXX", &img, &err);
    QTestState *qts;
    QDict *response;
    QList *blocks;
    const QListEntry *entry;
    bool attached = false;

    g_assert_no_error(err);
    g_assert_cmpint(ftruncate(fd, 1 * 1024 * 1024), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    qts = qtest_initf("-M origin200,io-bridge=on "
                      "-drive file=%s,format=raw,index=0", img);

    response = qtest_qmp(qts, "{ 'execute': 'query-block' }");
    g_assert_true(qdict_haskey(response, "return"));
    blocks = qdict_get_qlist(response, "return");
    for (entry = qlist_first(blocks); entry; entry = qlist_next(entry)) {
        QDict *block = qobject_to(QDict, qlist_entry_obj(entry));
        const char *qdev = qdict_get_try_str(block, "qdev");

        if (qdev && qdev[0] != '\0') {
            attached = true;
        }
    }
    g_assert_true(attached);
    qobject_unref(response);

    qtest_quit(qts);
    g_unlink(img);
}

static void test_reject_a_device_beyond_the_bridge_slots(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,io-bridge=on",
        "-device", "sgi-bridge-dma-test,addr=8",
        "-display", "none", NULL
    };

    if (!qtest_has_device("sgi-bridge-dma-test")) {
        g_test_skip("sgi-bridge-dma-test is not in this build");
        return;
    }

    assert_qemu_rejects(args,
                        "not available for sgi-bridge-dma-test, reserved");
}

static void test_reject_three_numa_nodes(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "192M",
        "-object", "memory-backend-ram,id=ram0,size=64M",
        "-object", "memory-backend-ram,id=ram1,size=64M",
        "-object", "memory-backend-ram,id=ram2,size=64M",
        "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0",
        "-numa", "node,nodeid=1,cpus=2-3,memdev=ram1",
        "-numa", "node,nodeid=2,memdev=ram2",
        "-smp", "4,sockets=2,cores=2,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "origin200: at most 2 NUMA nodes are supported");
}

static void test_reject_partial_cpu_numa_mapping(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "128M",
        "-object", "memory-backend-ram,id=ram0,size=64M",
        "-object", "memory-backend-ram,id=ram1,size=64M",
        "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0",
        "-numa", "node,nodeid=1,memdev=ram1",
        "-smp", "4,sockets=2,cores=2,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "origin200: CPU 2 must be assigned to NUMA node 1");
}

static void test_reject_invalid_node_memory(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "96M",
        "-object", "memory-backend-ram,id=ram0,size=64M",
        "-object", "memory-backend-ram,id=ram1,size=32M",
        "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0",
        "-numa", "node,nodeid=1,cpus=2-3,memdev=ram1",
        "-smp", "4,sockets=2,cores=2,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "node 1's 32 MiB cannot be populated from IP29");
}

static void test_reject_split_one_node_topology(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200",
        "-smp", "2,sockets=2,cores=1,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    /*
     * Two sockets now ask for two chassis outright -- each socket is one
     * chassis -- so the machine rejects this as an under-populated
     * dual-chassis request rather than as a malformed one-chassis one.
     */
    g_assert_nonnull(strstr(error_output,
                           "2 chassis require 4 CPUs as 2 sockets with "
                           "2 cores and one thread each"));
}

static void test_reject_three_cores_one_chassis(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200",
        "-smp", "3,sockets=1,cores=3,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "origin200: one chassis requires one socket with "
                        "at most two cores and one thread per core");
}

static void test_reject_three_sockets(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200",
        "-smp", "3,sockets=3,cores=1,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "origin200: at most 2 chassis are supported, and "
                        "each -smp socket is one chassis");
}

static void test_reject_uneven_no_numa_split(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "129M",
        "-smp", "4,sockets=2,cores=2,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "origin200: 2 chassis split -m evenly into whole "
                        "MiB; use -numa for an uneven fit");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/model/config-graph", NULL, test_config_graph);
    qtest_add_data_func("sn0/model/baseline-device-identity", NULL,
                        test_baseline_device_identity);
    qtest_add_data_func("sn0/model/baseline-cpu-identity", NULL,
                        test_baseline_cpu_identity);
    qtest_add_data_func("sn0/model/graph-whole-region", NULL,
                        test_graph_whole_region);
    qtest_add_data_func("sn0/model/object-graph", NULL, test_object_graph);
    qtest_add_data_func("sn0/model/dual-config-graphs", NULL,
                        sn0_model_test_dual_config_graph);
    qtest_add_data_func("sn0/model/dual-device-identity", NULL,
                        sn0_model_test_dual_device_identity);
    qtest_add_data_func("sn0/model/dual-object-graph", NULL,
                        sn0_model_test_dual_object_graph);
    qtest_add_data_func("sn0/model/dual-divergence-masking", NULL,
                        sn0_model_test_dual_divergence_masking);
    qtest_add_data_func("sn0/model/architecture-boundaries", NULL,
                        test_architecture_boundaries);
    qtest_add_data_func("sn0/model/bridge-root-bus-names", NULL,
                        test_bridge_root_bus_names);
    qtest_add_data_func("sn0/model/dual-chassis-base-io", NULL,
                        test_dual_chassis_base_io);
    qtest_add_data_func("sn0/model/dual-chassis-ioc3-network-identity", NULL,
                        test_dual_chassis_ioc3_network_identity);
    qtest_add_data_func("sn0/model/dual-chassis-base-io-survives-reset", NULL,
                        test_dual_chassis_base_io_survives_reset);
    qtest_add_data_func("sn0/model/promdata-uses-memory-space", NULL,
                        test_promdata_uses_memory_space);
    qtest_add_data_func("sn0/model/processor-mode-bits", NULL,
                        test_processor_mode_bits_are_board_state);
    qtest_add_data_func("sn0/model/one-node-smp", NULL,
                        test_one_node_smp);
    qtest_add_data_func("sn0/model/one-node-cpu-topology", NULL,
                        test_one_node_cpu_topology);
    qtest_add_data_func("sn0/model/explicit-one-node-numa", NULL,
                        test_explicit_one_node_numa);
    qtest_add_data_func("sn0/model/dual-chassis-numa", NULL,
                        test_dual_chassis_numa);
    qtest_add_data_func("sn0/model/reject-three-cores-one-chassis", NULL,
                        test_reject_three_cores_one_chassis);
    qtest_add_data_func("sn0/model/reject-three-sockets", NULL,
                        test_reject_three_sockets);
    qtest_add_data_func("sn0/model/reject-uneven-no-numa-split", NULL,
                        test_reject_uneven_no_numa_split);
    qtest_add_data_func("sn0/model/32-bit-kernel-entry-sign-extended", NULL,
                        test_a_32_bit_kernel_entry_is_sign_extended);
    qtest_add_data_func("sn0/model/64-bit-kernel-entry-left-alone", NULL,
                        test_a_64_bit_kernel_entry_is_left_alone);
    qtest_add_data_func("sn0/model/drive-on-the-second-adapter", NULL,
                        test_a_drive_on_the_second_adapter);
    qtest_add_data_func("sn0/model/ecoff-kernel-boots", NULL,
                        test_an_ecoff_kernel_boots);
    qtest_add_data_func("sn0/model/reject-truncated-ecoff-kernel", NULL,
                        test_reject_a_truncated_ecoff_kernel);
    qtest_add_data_func("sn0/model/default-drive-reaches-first-adapter", NULL,
                        test_default_drive_reaches_the_first_scsi_adapter);
    qtest_add_data_func("sn0/model/reject-device-beyond-bridge-slots", NULL,
                        test_reject_a_device_beyond_the_bridge_slots);
    qtest_add_data_func("sn0/model/reject-three-numa-nodes", NULL,
                        test_reject_three_numa_nodes);
    qtest_add_data_func("sn0/model/reject-partial-cpu-numa-mapping", NULL,
                        test_reject_partial_cpu_numa_mapping);
    qtest_add_data_func("sn0/model/reject-invalid-node-memory", NULL,
                        test_reject_invalid_node_memory);
    qtest_add_data_func("sn0/model/reject-split-one-node-topology", NULL,
                        test_reject_split_one_node_topology);

    return g_test_run();
}
