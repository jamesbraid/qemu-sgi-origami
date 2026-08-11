/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QLogic ISP1020 family Fast/Wide SCSI host adapter.
 *
 * The adapter is not a register-level SCSI sequencer. It is an intelligent
 * controller running vendor microcode on an internal RISC processor: the host
 * hands it whole SCSI commands as descriptors in a memory ring and it performs
 * selection, message and data phases itself, posting completions into a second
 * ring. This model implements what that microcode does — the mailbox command
 * set and the two descriptor rings — and never executes vendor code, the same
 * way QEMU's other intelligent storage controllers implement a message
 * interface rather than running firmware. Guests download microcode at every
 * attach, so the model accepts that download and answers it; no vendor firmware
 * is present or required.
 *
 * One PCI device identification answers for every member of the family, from
 * the ISP1020 through the ISP1040C, and the configuration-space revision byte
 * is what selects the part. That is why this file is named for the family while
 * the QOM type names a part.
 *
 * A guest reads a revision from two different places and they are numbered from
 * different origins, which is the trap this device sets. OpenBSD and IRIX read
 * the PCI configuration-space byte, and OpenBSD derives the adapter's transfer
 * clock from it — 40 MHz below revision 2 and 60 MHz at revision 2 and above —
 * so a model reporting the wrong value there negotiates every synchronous
 * transfer against a reference it does not have. Linux instead reads the low
 * four bits of bus configuration 0, a field that runs one ahead of the PCI byte
 * across the whole family. The two are therefore written here as one value and
 * its derivation rather than as two constants that have to be kept in step.
 *
 * Registers the model does not implement log the access and answer, rather than
 * raising a bus error. Most of this window is registers later commits fill in,
 * and the ones the adapter genuinely does not use — the GPIO pair carrying
 * termination control on later parts of the family — are ones a guest may touch
 * without meaning anything by it.
 *
 * Written from the reviewed datasheet docs/datasheets/isp1040.md, whose
 * register contract is driver-sourced because no vendor programming manual
 * for this part exists: a Linux qla1280 against NetBSD isp cross-check for
 * the map, and OpenBSD 7.9's qlw for the behaviour. We read those to learn
 * the facts and wrote our own code; no vendor or driver code, comments or
 * file structure is reproduced.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_QLOGIC_ISP1040 "qlogic-isp1040"
typedef struct QLogicISPState QLogicISPState;
DECLARE_INSTANCE_CHECKER(QLogicISPState, QLOGIC_ISP1040, TYPE_QLOGIC_ISP1040)

#define QLOGIC_ISP_VENDOR_ID    0x1077
#define QLOGIC_ISP_DEVICE_ID    0x1020

/* Revision 5 is the ISP1040C, the fastest member and the one modelled here. */
#define QLOGIC_ISP_REVISION     0x05

/*
 * The whole register window is 0x100 bytes and every register in it is 16 bits
 * wide. The adapter presents the window through two base address registers that
 * reach the same registers: an I/O window at configuration offset 0x10 and a
 * memory window at 0x14. OpenBSD tries the memory window first and falls
 * back to the I/O window, so both must decode.
 */
#define QLOGIC_ISP_REG_WINDOW_SIZE  0x100

/* Register offsets within that window. */
#define ISP_REG_ID_LOW      0x00    /* RO: chip identity, low half. */
#define ISP_REG_ID_HIGH     0x02    /* RO: chip identity, high half. */
#define ISP_REG_BUS_CFG0    0x04    /* RO: low four bits are a revision. */

/*
 * Bus configuration 0's hardware-revision field, derived rather than written
 * down, because it and the configuration-space revision byte are two encodings
 * of one fact and a model that states them separately can state them
 * inconsistently. They run one apart across every member of the family, so
 * revision 5 in configuration space is field value 6 here.
 *
 * Linux is the only guest that reads the field, in three lines that compare it
 * against the ISP1040A's value and store nothing: it decides one thing, whether
 * to suppress a FIFO-threshold write as a workaround for that part's broken
 * FIFO. Six is correct, since it names the part modelled, and safe, since any
 * value but the ISP1040A's four takes the ordinary path.
 *
 * The one-apart relationship is the datasheet's, and the datasheet marks it as
 * cross-checked between drivers but unverified against hardware.
 */
#define ISP_BUS_CFG0_HWREV      (QLOGIC_ISP_REVISION + 1)

struct QLogicISPState {
    PCIDevice parent_obj;

    MemoryRegion io;    /* Base address register 0, the I/O window. */
    MemoryRegion mmio;  /* Base address register 1, the memory window. */
};

static uint64_t qlogic_isp_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case ISP_REG_ID_LOW:
        return QLOGIC_ISP_VENDOR_ID;
    case ISP_REG_ID_HIGH:
        return QLOGIC_ISP_DEVICE_ID;
    case ISP_REG_BUS_CFG0:
        return ISP_BUS_CFG0_HWREV;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: read of unimplemented register "
                      "0x%02" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void qlogic_isp_reg_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    switch (addr) {
    case ISP_REG_ID_LOW:
    case ISP_REG_ID_HIGH:
    case ISP_REG_BUS_CFG0:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: write 0x%04x to read-only register "
                      "0x%02" HWADDR_PRIx "\n", (unsigned)val, addr);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: write 0x%04x to unimplemented register "
                      "0x%02" HWADDR_PRIx "\n", (unsigned)val, addr);
        break;
    }
}

/*
 * The registers are little-endian on the bus, so the window says so and the
 * memory core performs whatever swap a given target needs. Nothing here swaps
 * by hand.
 *
 * The hardware accepts 16-bit accesses only, and impl is declared alongside
 * valid rather than left to its defaults because the two defaults fail in
 * opposite directions: an undeclared maximum is 4, which would chop a wider
 * access into repeated calls to handlers that later commits give side effects
 * to, and an undeclared minimum is 1. Pinning both ends to two bytes makes one
 * guest access reach the handler exactly once.
 */
static const MemoryRegionOps qlogic_isp_reg_ops = {
    .read = qlogic_isp_reg_read,
    .write = qlogic_isp_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void qlogic_isp_realize(PCIDevice *dev, Error **errp)
{
    QLogicISPState *s = QLOGIC_ISP1040(dev);

    /* The adapter presents one interrupt line, and it is a level. */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init_io(&s->io, OBJECT(s), &qlogic_isp_reg_ops, s,
                          "qlogic-isp-io", QLOGIC_ISP_REG_WINDOW_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(s), &qlogic_isp_reg_ops, s,
                          "qlogic-isp-mmio", QLOGIC_ISP_REG_WINDOW_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static const VMStateDescription vmstate_qlogic_isp = {
    .name = "qlogic-isp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QLogicISPState),
        VMSTATE_END_OF_LIST()
    }
};

static void qlogic_isp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = qlogic_isp_realize;
    pc->vendor_id = QLOGIC_ISP_VENDOR_ID;
    pc->device_id = QLOGIC_ISP_DEVICE_ID;
    pc->revision = QLOGIC_ISP_REVISION;
    pc->class_id = PCI_CLASS_STORAGE_SCSI;
    dc->vmsd = &vmstate_qlogic_isp;
    dc->desc = "QLogic ISP1040 SCSI host adapter";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo qlogic_isp_types[] = {
    {
        .name = TYPE_QLOGIC_ISP1040,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(QLogicISPState),
        .class_init = qlogic_isp_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(qlogic_isp_types)
