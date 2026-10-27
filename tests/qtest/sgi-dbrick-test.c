/*
 * SGI D-brick bay population: what the enclosure accepts in its bays.
 * Discovery of the bays' disks on the loop is in qlogic-isp2200-test.c.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "libqos/pci-sgi-bridge.h"

/* An ISP2200 on the libqos Bridge, with null drives d0.. to fit. */
#define DBRICK_HEAD \
    QPCI_SGI_BRIDGE_HEAD \
    "-device qlogic-isp2200,addr=04.0,id=isp0 " \
    "-drive id=d0,if=none,format=raw,file=null-co:// " \
    "-drive id=d1,if=none,format=raw,file=null-co:// " \
    "-drive id=d2,if=none,format=raw,file=null-co:// "

/* scsi-hd @id on @bus at loop ID @loop and @lun, with its own names. */
#define DISK(id, drive, bus, loop, lun) \
    "-device scsi-hd,id=" id ",drive=" drive ",bus=" bus ".0," \
    "scsi-id=" #loop ",lun=" #lun ",port_wwn=0x2100000000000" #loop "0" \
    #lun ",wwn=0x2000000000000" #loop "0" #lun " "

typedef struct Refusal {
    const char *name;
    const char *args;
    const char *message;
} Refusal;

static const Refusal refusals[] = {
    {
        "no-corner-disks",
        DISK("p0", "d0", "isp0", 1, 0)
        "-device sgi-dbrick,bay2-1=p0",
        "needs disks in bays 1/3 and 4/3",
    }, {
        "one-corner-disk",
        DISK("p0", "d0", "isp0", 1, 0)
        "-device sgi-dbrick,bay1-3=p0",
        "needs disks in bays 1/3 and 4/3",
    }, {
        "not-a-disk",
        DISK("p0", "d0", "isp0", 1, 0)
        "-device scsi-cd,id=p1,drive=d1,bus=isp0.0,scsi-id=2,"
        "port_wwn=0x2100000000000200,wwn=0x2000000000000200 "
        "-device sgi-dbrick,bay1-3=p0,bay4-3=p1",
        "bay 4/3 holds a scsi-cd",
    }, {
        "same-disk-twice",
        DISK("p0", "d0", "isp0", 1, 0)
        "-device sgi-dbrick,bay1-3=p0,bay4-3=p0",
        "bays 1/3 and 4/3 hold the same disk",
    }, {
        "not-on-a-loop",
        DISK("p0", "d0", "isp0", 1, 0)
        "-device lsi53c895a,addr=05.0,id=lsi0 "
        DISK("p1", "d1", "lsi0", 2, 0)
        "-device sgi-dbrick,bay1-3=p0,bay4-3=p1",
        "bay 4/3: the disk is on lsi0.0, not on a QLogic ISP2200 loop",
    }, {
        "two-loops",
        "-device qlogic-isp2200,addr=05.0,id=isp1 "
        DISK("p0", "d0", "isp0", 1, 0)
        DISK("p1", "d1", "isp1", 2, 0)
        "-device sgi-dbrick,bay1-3=p0,bay4-3=p1",
        "bays 1/3 and 4/3 are on different loops, isp0.0 and isp1.0",
    }, {
        "second-lun",
        DISK("p0", "d0", "isp0", 1, 0)
        DISK("p1", "d1", "isp0", 2, 1)
        "-device sgi-dbrick,bay1-3=p0,bay4-3=p1",
        "bay 4/3: the disk is LUN 1",
    }, {
        "shared-port",
        DISK("p0", "d0", "isp0", 1, 0)
        DISK("p1", "d1", "isp0", 2, 0)
        "-device scsi-hd,id=p2,drive=d2,bus=isp0.0,scsi-id=2,lun=1,"
        "port_wwn=0x2100000000000200,wwn=0x2000000000000200 "
        "-device sgi-dbrick,bay1-3=p0,bay4-3=p1",
        "bay 4/3: loop ID 2 also holds scsi-hd at LUN 1",
    },
};

static void test_refused(const void *data)
{
    const Refusal *r = data;

    if (!g_test_subprocess()) {
        g_autofree char *message = g_strdup_printf("*%s*", r->message);

        g_test_trap_subprocess(NULL, 60 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(message);
        return;
    }
    qtest_quit(qtest_initf("-M none " DBRICK_HEAD "%s", r->args));
}

/* Both corners filled is the smallest brick the guides allow. */
static void test_two_disks(void)
{
    QTestState *qts = qtest_initf("-M none " DBRICK_HEAD
                                  DISK("p0", "d0", "isp0", 1, 0)
                                  DISK("p1", "d1", "isp0", 2, 0)
                                  "-device sgi-dbrick,id=dbrick0,"
                                  "bay1-3=p0,bay4-3=p1");
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                           "{ 'path': '/machine/peripheral/dbrick0', "
                           "'property': 'bay4-3' } }");

    g_assert_cmpstr(qdict_get_str(rsp, "return"), ==,
                    "/machine/peripheral/p1");
    qobject_unref(rsp);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-dbrick/two-disks", test_two_disks);
    for (size_t i = 0; i < ARRAY_SIZE(refusals); i++) {
        g_autofree char *path = g_strdup_printf("sgi-dbrick/refused/%s",
                                                refusals[i].name);

        qtest_add_data_func(path, &refusals[i], test_refused);
    }
    return g_test_run();
}
