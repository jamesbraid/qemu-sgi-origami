/*
 * Origin 200 machine topology and synthesized-firmware qtests.
 *
 * The expected values here are deliberately independent literals.  The model
 * compiler supplies the machine description consumed by QEMU, so using its
 * generated output as the expected side would let one compiler bug satisfy
 * both the machine and its test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
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
#define SN0_GRAPH_MAGIC         0xbeedbabeULL
#define SN0_BOARD_NODE          0x11
#define SN0_BOARD_MIDPLANE      0x41
#define SN0_BOARD_BASE_IO       0x21
#define SN0_COMP_PROCESSOR      1
#define SN0_COMP_HUB            2
#define SN0_COMP_MEMORY         3
#define SN0_COMP_BRIDGE         5
#define SN0_COMP_IO_CONTROLLER  6
#define SN0_COMP_SCSI           11
#define SN0_COMP_MODULE_SERIAL  26
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
#define SN0_BASE_PROFILE_MEMORY \
    "-M origin200,synthesized-firmware=off -m 64M " \
    "-object memory-backend-ram,id=ram0,size=64M "
#define SN0_BASE_PROFILE_ONE_CPU \
    SN0_BASE_PROFILE_MEMORY \
    "-numa node,nodeid=0,cpus=0,memdev=ram0 " \
    "-smp 1,sockets=1,cores=1,threads=1"
#define SN0_BASE_PROFILE_TWO_CPUS \
    SN0_BASE_PROFILE_MEMORY \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-smp 2,sockets=1,cores=2,threads=1"

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
#define SN0_PCI_SUBSYSTEM       0x2c
#define SN0_PCI_COMMAND_IO      0x0001
#define SN0_PCI_COMMAND_MEMORY  0x0002
#define SN0_PCI_COMMAND_MASTER  0x0004
#define SN0_PCI_BAR0            0x10
#define SN0_PCI_BAR1            0x14
#define SN0_PCI_SCSI0_MEM       0x08200000U
#define SN0_PCI_SCSI1_MEM       0x08400000U
#define SN0_PCI_IOC3_MEM        0x08600000U
#define SN0_IOC3_SUBSYSTEM      0xc30310a9U
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

    g_assert_cmphex(prid, ==, 0x0926);
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

    g_assert_cmphex(prid, ==, 0x0926);
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

    g_assert_cmphex(prid, ==, 0x0926);
    g_assert_cmphex(fpu_prid, ==, 0x0900);
    qtest_quit(qts);
    g_unlink(kernel);
}

static void assert_device_identity(QTestState *qts, unsigned int node)
{
    uint64_t hub = SN0_HUB_BY_NODE(node);
    uint64_t ni = qtest_readq(qts, hub + SN0_NI_STATUS);
    uint64_t hub_widget = qtest_readq(qts,
                                      hub + SN0_IO_WIDGET_ID);
    uint32_t bridge = qtest_readl(qts, SN0_BRIDGE_BASE(node) + 4);

    g_assert_cmpuint((ni >> 4) & 0xf, ==, 3);
    g_assert_cmpuint((hub_widget >> 28) & 0xf, ==, 1);
    g_assert_cmpuint(bridge >> 28, ==, 4);
    g_assert_cmpuint(qtest_readl(qts, SN0_CFG0(node, SN0_IOC3_DEVICE) +
                                      SN0_PCI_REVISION_ID) & 0xff, ==,
                     1);
    g_assert_cmphex(qtest_readl(qts, SN0_CFG0(node, SN0_IOC3_DEVICE) +
                                     SN0_PCI_SUBSYSTEM), ==,
                    SN0_IOC3_SUBSYSTEM);
    g_assert_cmpuint(qtest_readl(qts, SN0_CFG0(node, SN0_SCSI_DEVICE0) +
                                      SN0_PCI_REVISION_ID) & 0xff, ==,
                     5);
    g_assert_cmpuint(qtest_readl(qts, SN0_CFG0(node, SN0_SCSI_DEVICE1) +
                                      SN0_PCI_REVISION_ID) & 0xff, ==,
                     5);
}

static void test_baseline_device_identity(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    assert_device_identity(qts, 0);
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

/* The Hub's network interface, as far as renumbering a stock machine needs. */
#define SN0_NI_VECTOR_PARMS     0x600200
#define SN0_NI_VECTOR           0x600208
#define SN0_NI_VECTOR_DATA      0x600210
#define SN0_NI_REGION_SIZE      (1ULL << 17)
#define SN0_NI_NODE_MASK        (0x1ffULL << SN0_NI_NODE_SHIFT)
#define SN0_NI_PIOTYPE_WRITE    1

/*
 * A stock-PROM machine boots with every Hub programmed zero, so a
 * node-one-qualified address names no Hub. Program node 1's real number
 * the way the PROM's global master does, a vector write over the
 * CrayLink, so the test can reach node 1's devices by address.
 */
static void assign_node1_number(QTestState *qts)
{
    qtest_writeq(qts, SN0_HUB_LOCAL + SN0_NI_VECTOR, 0);
    qtest_writeq(qts, SN0_HUB_LOCAL + SN0_NI_VECTOR_DATA,
                 SN0_NI_REGION_SIZE | (1ULL << SN0_NI_NODE_SHIFT));
    qtest_writeq(qts, SN0_HUB_LOCAL + SN0_NI_VECTOR_PARMS,
                 SN0_NI_STATUS | SN0_NI_PIOTYPE_WRITE);
    g_assert_cmphex(qtest_readq(qts, SN0_HUB_BY_NODE(1) + SN0_NI_STATUS) &
                    SN0_NI_NODE_MASK, ==, 1ULL << SN0_NI_NODE_SHIFT);
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

    assign_node1_number(qts);
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

typedef struct QOMChildExpectation {
    const char *name;
    const char *type;
} QOMChildExpectation;

static bool is_qom_infrastructure_child(const char *type)
{
    return g_str_equal(type, "child<container>") ||
           g_str_equal(type, "child<memory-region>") ||
           g_str_equal(type, "child<clock>") ||
           g_str_equal(type, "child<irq>") ||
           g_str_equal(type, "child<sgi-ioc3-bus>") ||
           g_str_equal(type, "child<qtest>") ||
           g_str_equal(type, "child<monitor-qmp>");
}

static void assert_qom_child_set(QTestState *qts, const char *path,
                                 const QOMChildExpectation *expected,
                                 size_t expected_count)
{
    g_autofree bool *seen = g_new0(bool, expected_count);
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                     " 'arguments': { 'path': %s } }",
                                     path);
    QList *entries;
    const QListEntry *entry;
    size_t child_count = 0;

    g_assert_true(qdict_haskey(response, "return"));
    entries = qdict_get_qlist(response, "return");
    for (entry = qlist_first(entries); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(child, "name");
        const char *type = qdict_get_str(child, "type");
        size_t expected_index;

        if (!g_str_has_prefix(type, "child<") ||
            is_qom_infrastructure_child(type)) {
            continue;
        }
        child_count++;
        for (expected_index = 0; expected_index < expected_count;
             expected_index++) {
            if (g_str_equal(name, expected[expected_index].name) &&
                g_str_equal(type, expected[expected_index].type)) {
                g_assert_false(seen[expected_index]);
                seen[expected_index] = true;
                break;
            }
        }
        if (expected_index == expected_count) {
            g_test_message("unexpected QOM child %s with type %s below %s",
                           name, type, path);
        }
        g_assert_cmpuint(expected_index, <, expected_count);
    }
    g_assert_cmpuint(child_count, ==, expected_count);
    for (child_count = 0; child_count < expected_count; child_count++) {
        g_assert_true(seen[child_count]);
    }
    qobject_unref(response);
}

static void test_origin200_owns_the_ioc3_board_default(const void *data)
{
    QTestState *qts = qtest_init("-machine none");
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'query-machines',"
             " 'arguments': { 'compat-props': true } }");
    QList *machines = qdict_get_qlist(response, "return");
    const QListEntry *machine_entry;
    bool found_machine = false;
    bool found_property = false;

    for (machine_entry = qlist_first(machines); machine_entry;
         machine_entry = qlist_next(machine_entry)) {
        QDict *machine = qobject_to(
            QDict, qlist_entry_obj(machine_entry));
        QList *properties;
        const QListEntry *property_entry;

        if (!g_str_equal(qdict_get_str(machine, "name"), "origin200")) {
            continue;
        }
        found_machine = true;
        properties = qdict_get_qlist(machine, "compat-props");
        for (property_entry = qlist_first(properties); property_entry;
             property_entry = qlist_next(property_entry)) {
            QDict *property = qobject_to(
                QDict, qlist_entry_obj(property_entry));

            if (g_str_equal(qdict_get_str(property, "qom-type"),
                            "sgi-ioc3") &&
                g_str_equal(qdict_get_str(property, "property"),
                            "subsystem-id")) {
                g_assert_false(found_property);
                g_assert_cmpstr(qdict_get_str(property, "value"), ==,
                                "0xc303");
                found_property = true;
            }
        }
    }
    g_assert_true(found_machine);
    g_assert_true(found_property);
    qobject_unref(response);
    qtest_quit(qts);
}

typedef struct BaseProfileTopologyCase {
    const char *args;
    bool has_cpu1;
} BaseProfileTopologyCase;

static const BaseProfileTopologyCase base_profile_topologies[] = {
    { SN0_BASE_PROFILE_ONE_CPU, false },
    { SN0_BASE_PROFILE_TWO_CPUS, true },
};

/* Exact shipped population, without cataloguing unrelated QOM internals. */
static void test_base_profile_topology(const void *data)
{
    const BaseProfileTopologyCase *topology = data;
    static const QOMChildExpectation machine_children[] = {
        { "node0", "child<sn0-node>" },
        { "bridge", "child<sgi-bridge>" },
    };
    static const QOMChildExpectation one_cpu_children[] = {
        { "cpu0", "child<R10000-mips64-cpu>" },
        { "hub", "child<sgi-hub>" },
        { "ioc3", "child<sgi-ioc3>" },
        { "isp0", "child<qlogic-isp1040>" },
        { "isp1", "child<qlogic-isp1040>" },
    };
    static const QOMChildExpectation two_cpu_children[] = {
        { "cpu0", "child<R10000-mips64-cpu>" },
        { "cpu1", "child<R10000-mips64-cpu>" },
        { "hub", "child<sgi-hub>" },
        { "ioc3", "child<sgi-ioc3>" },
        { "isp0", "child<qlogic-isp1040>" },
        { "isp1", "child<qlogic-isp1040>" },
    };
    const QOMChildExpectation *node_children = topology->has_cpu1 ?
        two_cpu_children : one_cpu_children;
    size_t node_child_count = topology->has_cpu1 ?
        ARRAY_SIZE(two_cpu_children) : ARRAY_SIZE(one_cpu_children);
    QTestState *qts = qtest_init(topology->args);

    assert_qom_child_set(qts, "/machine", machine_children,
                         ARRAY_SIZE(machine_children));
    assert_qom_child_set(qts, "/machine/node0", node_children,
                         node_child_count);

    qtest_quit(qts);
}

static void test_one_active_cpu_with_two_possible_cpus(const void *data)
{
    static const QOMChildExpectation node_children[] = {
        { "cpu0", "child<R10000-mips64-cpu>" },
        { "hub", "child<sgi-hub>" },
        { "ioc3", "child<sgi-ioc3>" },
        { "isp0", "child<qlogic-isp1040>" },
        { "isp1", "child<qlogic-isp1040>" },
    };
    QTestState *qts = qtest_init(
        "-M origin200,synthesized-firmware=off -smp 1,maxcpus=2");

    assert_qom_child_set(qts, "/machine/node0", node_children,
                         ARRAY_SIZE(node_children));
    qtest_quit(qts);
}

static void test_dual_profile_topology(const void *data)
{
    static const QOMChildExpectation machine_children[] = {
        { "node0", "child<sn0-node>" },
        { "node1", "child<sn0-node>" },
        { "bridge", "child<sgi-bridge>" },
        { "bridge1", "child<sgi-bridge>" },
    };
    static const QOMChildExpectation node_children[] = {
        { "cpu0", "child<R10000-mips64-cpu>" },
        { "cpu1", "child<R10000-mips64-cpu>" },
        { "hub", "child<sgi-hub>" },
        { "ioc3", "child<sgi-ioc3>" },
        { "isp0", "child<qlogic-isp1040>" },
        { "isp1", "child<qlogic-isp1040>" },
    };
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_ARGS);
    unsigned int node;

    assert_qom_child_set(qts, "/machine", machine_children,
                         ARRAY_SIZE(machine_children));
    for (node = 0; node < 2; node++) {
        g_autofree char *node_path = g_strdup_printf("/machine/node%u", node);

        assert_qom_child_set(qts, node_path, node_children,
                             ARRAY_SIZE(node_children));
    }
    qtest_quit(qts);
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

    mac0 = qom_get_string(qts, "/machine/node0/ioc3", "mac");
    mac1 = qom_get_string(qts, "/machine/node1/ioc3", "mac");
    netdev0 = qom_get_string(qts, "/machine/node0/ioc3", "netdev");
    netdev1 = qom_get_string(qts, "/machine/node1/ioc3", "netdev");

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

    mac0 = qom_get_string(qts, "/machine/node0/ioc3", "mac");
    mac1 = qom_get_string(qts, "/machine/node1/ioc3", "mac");
    netdev0 = qom_get_string(qts, "/machine/node0/ioc3", "netdev");
    netdev1 = qom_get_string(qts, "/machine/node1/ioc3", "netdev");

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

static void assert_graph_header(QTestState *qts, uint64_t base,
                                unsigned int nasid)
{
    g_assert_cmphex(read_field(qts, base + 0x4000, 8), ==, SN0_GRAPH_MAGIC);
    g_assert_cmphex(read_field(qts, base + 0x4008, 4), ==, 0);
    g_assert_cmphex(read_field(qts, base + 0x400c, 4), ==, 0x48);
    g_assert_cmphex(read_field(qts, base + 0x4010, 4), ==, 0x18);
    g_assert_cmphex(read_field(qts, base + 0x4014, 4), ==, 0x4100);
    g_assert_cmphex(read_field(qts, base + 0x4018, 8), ==,
                    0x9200000008620178ULL);
    g_assert_cmphex(read_field(qts, base + 0x4020, 8), ==,
                    0x9200000008022000ULL);
    g_assert_cmphex(read_field(qts, base + 0x4028, 8), ==,
                    0x9200000008600000ULL);
    g_assert_cmpuint(read_field(qts, base + 0x4030, 2), ==, 9600);
    g_assert_cmpuint(read_field(qts, base + 0x4038, 2), ==, nasid);
    g_assert_cmpuint(read_field(qts, base + 0x403a, 1), ==, 8);
    g_assert_cmpuint(read_field(qts, base + 0x403b, 1), ==, 2);
}

static void assert_board(QTestState *qts, uint64_t base, uint64_t offset,
                         uint32_t next, uint8_t type, uint8_t version,
                         uint8_t declared_components, unsigned int nasid)
{
    g_assert_cmphex(read_field(qts, base + offset, 4), ==, next);
    g_assert_cmpuint(read_field(qts, base + offset + 0x04, 1), ==, 1);
    g_assert_cmpuint(read_field(qts, base + offset + 0x05, 1), ==, type);
    g_assert_cmpuint(read_field(qts, base + offset + 0x06, 1), ==, version);
    g_assert_cmpuint(read_field(qts, base + offset + 0x17, 1), ==,
                     declared_components);
    g_assert_cmpuint(read_field(qts, base + offset + 0x20, 2), ==, nasid);
}

static void assert_component(QTestState *qts, uint64_t base, uint64_t offset,
                             uint8_t type, uint8_t revision,
                             uint8_t physical_id, uint32_t virtual_id,
                             uint8_t widget, unsigned int nasid)
{
    g_assert_cmpuint(read_field(qts, base + offset + 0x00, 1), ==, type);
    g_assert_cmpuint(read_field(qts, base + offset + 0x01, 1), ==, 1);
    g_assert_cmpuint(read_field(qts, base + offset + 0x02, 1), ==,
                     SN0_COMP_ENABLED);
    g_assert_cmpuint(read_field(qts, base + offset + 0x03, 1), ==, revision);
    g_assert_cmpuint(read_field(qts, base + offset + 0x18, 1), ==,
                     physical_id);
    g_assert_cmpuint(read_field(qts, base + offset + 0x1c, 4), ==,
                     virtual_id);
    g_assert_cmpuint(read_field(qts, base + offset + 0x20, 1), ==, widget);
    g_assert_cmpuint(read_field(qts, base + offset + 0x22, 2), ==, nasid);
    g_assert_cmphex(read_field(qts, base + offset + 0x28, 8), ==, 0);
}

static uint8_t hub_ni_revision(QTestState *qts, unsigned int nasid)
{
    return (qtest_readq(qts, SN0_HUB_BY_NODE(nasid) + SN0_NI_STATUS) >> 4) &
           0xf;
}

static void assert_cpu_record(QTestState *qts, uint64_t base, uint64_t offset,
                              unsigned int slice, unsigned int virtual_id,
                              unsigned int nasid)
{
    assert_component(qts, base, offset, SN0_COMP_PROCESSOR, 0xff, slice,
                     virtual_id, 0, nasid);
    g_assert_cmphex(read_field(qts, base + offset + 0x38, 2), ==, 0x0926);
    g_assert_cmphex(read_field(qts, base + offset + 0x3a, 2), ==, 0x0900);
    g_assert_cmpuint(read_field(qts, base + offset + 0x3c, 2), ==, 180);
    g_assert_cmpuint(read_field(qts, base + offset + 0x3e, 2), ==, 1);
    g_assert_cmpuint(read_field(qts, base + offset + 0x40, 2), ==, 90);
}

static void assert_memory_record(QTestState *qts, uint64_t base,
                                 uint64_t offset, unsigned int nasid,
                                 uint16_t total_mb, uint16_t bank0_mb,
                                 uint16_t bank1_mb)
{
    unsigned int bank;

    assert_component(qts, base, offset, SN0_COMP_MEMORY, 0xff, 0, 0, 0,
                     nasid);
    g_assert_cmpuint(read_field(qts, base + offset + 0x38, 2), ==, total_mb);
    g_assert_cmphex(read_field(qts, base + offset + 0x3a, 2), ==, 0);
    g_assert_cmpuint(read_field(qts, base + offset + 0x3c, 2), ==, bank0_mb);
    g_assert_cmpuint(read_field(qts, base + offset + 0x3e, 2), ==, bank1_mb);
    for (bank = 2; bank < 8; bank++) {
        g_assert_cmphex(read_field(qts, base + offset + 0x3c + bank * 2, 2),
                        ==, 0);
    }
}

static void assert_base_io_records(QTestState *qts, uint64_t base,
                                   uint64_t first, unsigned int nasid)
{
    uint8_t scsi0_revision =
        qtest_readl(qts, SN0_CFG0(nasid, SN0_SCSI_DEVICE0) +
                         SN0_PCI_REVISION_ID) & 0xff;
    uint8_t scsi1_revision =
        qtest_readl(qts, SN0_CFG0(nasid, SN0_SCSI_DEVICE1) +
                         SN0_PCI_REVISION_ID) & 0xff;
    uint8_t ioc3_revision =
        qtest_readl(qts, SN0_CFG0(nasid, SN0_IOC3_DEVICE) +
                         SN0_PCI_REVISION_ID) & 0xff;

    assert_component(qts, base, first, SN0_COMP_BRIDGE, 4, 8, 0, 8, nasid);
    assert_component(qts, base, first + 0x80, SN0_COMP_SCSI,
                     scsi0_revision, 0, 0, 8, nasid);
    assert_component(qts, base, first + 0x100, SN0_COMP_SCSI,
                     scsi1_revision, 1, 1, 8, nasid);
    assert_component(qts, base, first + 0x180, SN0_COMP_IO_CONTROLLER,
                     ioc3_revision, 2, 0, 8, nasid);
}

static void test_bridge_revision_matches_config_graph(const void *data)
{
    QTestState *qts = qtest_init("-M origin200,widget0-revision=3 -m 64M");
    uint32_t bridge = qtest_readl(qts, SN0_BRIDGE_BASE(0) + 4);

    g_assert_cmpuint(bridge >> 28, ==, 3);
    assert_component(qts, 0, 0x4600, SN0_COMP_BRIDGE, 3, 8, 0, 8, 0);
    qtest_quit(qts);
}

static void assert_one_cpu_graph(QTestState *qts, uint16_t total_mb,
                                 uint16_t bank0_mb, uint16_t bank1_mb)
{
    assert_graph_header(qts, 0, 0);
    assert_board(qts, 0, 0x4100, 0x41c0, SN0_BOARD_NODE, 3, 3, 0);
    g_assert_cmphex(read_field(qts, 0x4124, 4), ==, 0x4400);
    g_assert_cmphex(read_field(qts, 0x4128, 4), ==, 0x4480);
    g_assert_cmphex(read_field(qts, 0x412c, 4), ==, 0x4500);
    assert_cpu_record(qts, 0, 0x4400, 0, 0, 0);
    assert_component(qts, 0, 0x4480, SN0_COMP_HUB,
                     hub_ni_revision(qts, 0), 0, 0, 0, 0);
    g_assert_cmphex(read_field(qts, 0x44bc, 8), ==,
                    0xffff000000000000ULL);
    g_assert_cmpuint(read_field(qts, 0x44d8, 8), ==, 90000000);
    assert_memory_record(qts, 0, 0x4500, 0, total_mb, bank0_mb, bank1_mb);
    assert_board(qts, 0, 0x41c0, 0x4280, SN0_BOARD_MIDPLANE, 2, 0, 0);
    g_assert_cmphex(read_field(qts, 0x41e4, 4), ==, 0x4580);
    assert_component(qts, 0, 0x4580, SN0_COMP_MODULE_SERIAL, 0xff, 0, 0,
                     0, 0);
    assert_board(qts, 0, 0x4280, 0, SN0_BOARD_BASE_IO, 2, 4, 0);
    g_assert_cmphex(read_field(qts, 0x42a4, 4), ==, 0x4600);
    g_assert_cmphex(read_field(qts, 0x42a8, 4), ==, 0x4680);
    g_assert_cmphex(read_field(qts, 0x42ac, 4), ==, 0x4700);
    g_assert_cmphex(read_field(qts, 0x42b0, 4), ==, 0x4780);
    assert_base_io_records(qts, 0, 0x4600, 0);
}

static void assert_two_cpu_graph(QTestState *qts, unsigned int nasid,
                                 unsigned int first_virtual_id)
{
    uint64_t base = (uint64_t)nasid * SN0_NODE_STRIDE;

    assert_graph_header(qts, base, nasid);
    assert_board(qts, base, 0x4100, 0x41c0, SN0_BOARD_NODE, 3, 4, nasid);
    g_assert_cmphex(read_field(qts, base + 0x4124, 4), ==, 0x4400);
    g_assert_cmphex(read_field(qts, base + 0x4128, 4), ==, 0x4480);
    g_assert_cmphex(read_field(qts, base + 0x412c, 4), ==, 0x4500);
    g_assert_cmphex(read_field(qts, base + 0x4130, 4), ==, 0x4580);
    assert_cpu_record(qts, base, 0x4400, 0, first_virtual_id, nasid);
    assert_cpu_record(qts, base, 0x4480, 1, first_virtual_id + 1, nasid);
    assert_component(qts, base, 0x4500, SN0_COMP_HUB,
                     hub_ni_revision(qts, nasid), nasid, 0, 0, nasid);
    g_assert_cmphex(read_field(qts, base + 0x453c, 8), ==,
                    0xffff000000000000ULL);
    g_assert_cmpuint(read_field(qts, base + 0x4558, 8), ==, 90000000);
    assert_memory_record(qts, base, 0x4580, nasid, 64, 64, 0);
    assert_board(qts, base, 0x41c0, 0x4280, SN0_BOARD_MIDPLANE, 2, 0,
                 nasid);
    g_assert_cmphex(read_field(qts, base + 0x41e4, 4), ==, 0x4600);
    assert_component(qts, base, 0x4600, SN0_COMP_MODULE_SERIAL, 0xff, 0, 0,
                     0, nasid);
    assert_board(qts, base, 0x4280, 0, SN0_BOARD_BASE_IO, 2, 4, nasid);
    g_assert_cmphex(read_field(qts, base + 0x42a4, 4), ==, 0x4680);
    g_assert_cmphex(read_field(qts, base + 0x42a8, 4), ==, 0x4700);
    g_assert_cmphex(read_field(qts, base + 0x42ac, 4), ==, 0x4780);
    g_assert_cmphex(read_field(qts, base + 0x42b0, 4), ==, 0x4800);
    assert_base_io_records(qts, base, 0x4680, nasid);
}

static void test_config_graph(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 64M");

    assert_one_cpu_graph(qts, 64, 64, 0);
    qtest_quit(qts);
}

static void test_larger_memory_config_graph(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 512M");

    assert_one_cpu_graph(qts, 512, 256, 256);
    qtest_quit(qts);
}

static void test_dual_config_graphs(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_FIRMWARE_ARGS);

    assert_two_cpu_graph(qts, 0, 0);
    assert_two_cpu_graph(qts, 1, 2);
    qtest_quit(qts);
}

static void test_dual_device_identity(const void *data)
{
    QTestState *qts = qtest_init(SN0_DUAL_CHASSIS_FIRMWARE_ARGS);

    assert_device_identity(qts, 0);
    assert_device_identity(qts, 1);
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
    type = find_qom_type(types, "origin200-machine");
    g_assert_nonnull(type);
    g_assert_false(qdict_haskey(type, "abstract") &&
                   qdict_get_bool(type, "abstract"));
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

    g_assert_cmphex(prid, ==, 0x0926);
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

static void test_reject_split_numa_topology(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "128M",
        "-object", "memory-backend-ram,id=ram0,size=64M",
        "-object", "memory-backend-ram,id=ram1,size=64M",
        "-numa", "node,nodeid=0,cpus=0,memdev=ram0",
        "-numa", "node,nodeid=1,cpus=1,memdev=ram1",
        "-smp", "2,sockets=2,cores=1,threads=1",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };

    assert_qemu_rejects(args,
                        "origin200: 2 chassis require 4 CPUs as 2 sockets "
                        "with 2 cores and one thread each");
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
                        "at most 2 cores and one thread per core");
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
    qtest_add_data_func("sn0/model/larger-memory-config-graph", NULL,
                        test_larger_memory_config_graph);
    qtest_add_data_func("sn0/model/dual-config-graphs", NULL,
                        test_dual_config_graphs);
    qtest_add_data_func("sn0/model/dual-device-identity", NULL,
                        test_dual_device_identity);
    qtest_add_data_func("sn0/model/origin200-ioc3-board-default", NULL,
                        test_origin200_owns_the_ioc3_board_default);
    qtest_add_data_func("sn0/model/bridge-revision-matches-config-graph", NULL,
                        test_bridge_revision_matches_config_graph);
    qtest_add_data_func("sn0/model/base-profile-topology/one-cpu",
                        &base_profile_topologies[0],
                        test_base_profile_topology);
    qtest_add_data_func("sn0/model/base-profile-topology/two-cpus",
                        &base_profile_topologies[1],
                        test_base_profile_topology);
    qtest_add_data_func("sn0/model/one-active-cpu-two-possible", NULL,
                        test_one_active_cpu_with_two_possible_cpus);
    qtest_add_data_func("sn0/model/dual-profile-topology", NULL,
                        test_dual_profile_topology);
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
    qtest_add_data_func("sn0/model/reject-split-numa-topology", NULL,
                        test_reject_split_numa_topology);

    return g_test_run();
}
