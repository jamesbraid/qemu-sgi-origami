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

/*
 * Revision 5 is the ISP1040C, the fastest member and the one modelled here.
 * This is the one value in the file with a real machine behind it rather
 * than driver source alone: OpenBSD 7.9 prints "rev 0x05" attaching this
 * adapter on a captured Origin 200 boot, and the firmware's own hardware
 * inventory on that machine names the same adapters revision 5.
 */
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
#define ISP_REG_BUS_CFG1    0x06    /* RW: FIFO threshold and burst enable. */
#define ISP_REG_BUS_CTRL    0x08    /* RW: soft reset and interrupt enables. */
#define ISP_REG_BUS_STATUS  0x0a    /* RO: which interrupt is pending. */
#define ISP_REG_SEMAPHORE   0x0c    /* RW: the mailbox-result lock. */
#define ISP_REG_HOST_CMD    0xc0    /* RW: command in, RISC state flags out. */

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

/*
 * The bits bus configuration 1 owns on the PCI variant: a FIFO threshold field,
 * a global burst enable, and the select that swaps which register bank the
 * window at 0x80 presents. Writes keep these and drop the rest, so a guest
 * reading the register back sees only what this part implements.
 *
 * The later ISP1080 generation puts a DMA bank select in this register too and
 * OpenBSD read-modify-writes it, but only down a path it takes for that
 * generation; on this part it programs the DMA blocks at their own offsets
 * instead. Dropping those bits here is therefore right for the part modelled
 * rather than a gap, which is worth saying because the register's field list
 * looks incomplete beside the driver's header.
 */
#define ISP_BUS_CFG1_FIFO_MASK      0x0070
#define ISP_BUS_CFG1_BANK_SELECT    0x0008
#define ISP_BUS_CFG1_BURST_ENABLE   0x0004
#define ISP_BUS_CFG1_WRITE_MASK     (ISP_BUS_CFG1_FIFO_MASK | \
                                     ISP_BUS_CFG1_BANK_SELECT | \
                                     ISP_BUS_CFG1_BURST_ENABLE)

/*
 * Bus interface control. Bit 0 is a soft reset that the adapter clears when it
 * completes, and the other two are interrupt enables the guest sets together.
 * The soft reset is a trigger rather than stored state: the guest polls this
 * register up to a thousand times waiting for the bit to read back clear and
 * fails the attach if it never does.
 */
#define ISP_BUS_CTRL_SOFT_RESET     0x0001
#define ISP_BUS_CTRL_INT_ENABLE     0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE 0x0004
#define ISP_BUS_CTRL_WRITE_MASK     (ISP_BUS_CTRL_INT_ENABLE | \
                                     ISP_BUS_CTRL_RISC_INT_ENABLE)

/* Bus interface status: which kind of interrupt is outstanding. */
#define ISP_BUS_STATUS_INT_REQ      0x0002
#define ISP_BUS_STATUS_RISC_INT_REQ 0x0004

/*
 * The semaphore. The adapter takes the lock when it posts a mailbox result or
 * an asynchronous event, and the guest releases it by writing the register
 * back to zero. Taking it is the mailbox engine's business; this commit
 * implements the register the guest reads and clears.
 */
#define ISP_SEMAPHORE_LOCK      0x0001
#define ISP_SEMAPHORE_STATUS    0x0002
#define ISP_SEMAPHORE_MASK      (ISP_SEMAPHORE_LOCK | ISP_SEMAPHORE_STATUS)

/*
 * Eight 16-bit mailboxes. Mailbox 0 carries a command in and its result out;
 * the rest are arguments and returns selected by per-command bit masks.
 *
 * Mailboxes 4 and 5 are also the queue pointers. The driver names four of them
 * — request in and out, response in and out — at two addresses, because each
 * address means the host's pointer when written and the adapter's when read.
 * That naming is a protocol meaning laid over two ordinary registers rather
 * than a second decode: the hardware stores what is written until the running
 * firmware overwrites it. With no firmware running yet these are plain
 * storage, and the ring engine is what gives them the second meaning.
 */
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX_COUNT       8
#define ISP_REG_MAILBOX_END     (ISP_REG_MAILBOX_BASE + ISP_MAILBOX_COUNT * 2)

/* The queue pointers, at these offsets from the mailbox base. */
#define ISP_MAILBOX_REQUEST_QUEUE   4
#define ISP_MAILBOX_RESPONSE_QUEUE  5

/*
 * After reset the adapter presents an ASCII signature in mailboxes 1, 2 and 3:
 * "IS", "P " and two spaces. A model that does not present it sends the guest
 * down a recovery path rather than failing outright, so this is about keeping
 * the guest on its ordinary path and its log clean.
 *
 * It is also the observable that proves reset ran at all. A PCI device is reset
 * through its bus, and a device whose registers decode perfectly while its
 * reset was never called reads every register at its zero-initialised value,
 * which for most registers here is also the reset value and so says nothing.
 * These three do not read zero, so the qtest asserting them is what
 * distinguishes a device that was reset from one that merely answers.
 *
 * Two independent sources agree on the exact bytes: OpenBSD's own driver
 * expects precisely this ASCII after reset, and 86Box's ql_reset_asic
 * reproduces the same three values in a second, unrelated implementation.
 * One of those is the guest this fork boots and the other is not derived
 * from it, which is stronger footing than either source gives alone.
 */
#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020

/*
 * Host command register. A write carries a command in the top four bits; a
 * read returns flags describing what the RISC processor is doing.
 */
#define ISP_HOST_CMD_SHIFT          12
#define ISP_HOST_CMD_NOP            0x0
#define ISP_HOST_CMD_RESET_RISC     0x1
#define ISP_HOST_CMD_PAUSE_RISC     0x2
#define ISP_HOST_CMD_RELEASE_RISC   0x3
#define ISP_HOST_CMD_MASK_PARITY    0x4
#define ISP_HOST_CMD_SET_HOST_INT   0x5
#define ISP_HOST_CMD_CLEAR_HOST_INT 0x6
#define ISP_HOST_CMD_CLEAR_RISC_INT 0x7
#define ISP_HOST_CMD_BIOS_ENABLE    0x9
#define ISP_HOST_CMD_ENABLE_PARITY  0xa
#define ISP_HOST_CMD_PARITY_ERROR   0xe

#define ISP_RISC_FLAG_HOST_INT      0x0080
#define ISP_RISC_FLAG_RESET         0x0040
#define ISP_RISC_FLAG_PAUSED        0x0020
#define ISP_RISC_FLAG_EXTERNAL      0x0010

/*
 * The SXP and RISC register blocks share the window from 0x80 up to the host
 * command register at 0xc0, and bus configuration 1 bit 3 chooses which one
 * answers there. No guest on origami's path reads either block — NetBSD's
 * driver does, and NetBSD is a cross-check source rather than a target guest
 * here — so no individual register in either block is named or decoded.
 *
 * The datasheet is explicit that the select still has to be implemented,
 * because a model that ignored it and always presented one bank would
 * mislead anyone who later needs the other. That means the two blocks need
 * real, separate storage rather than a bit that is stored but never acted
 * on: the observable a guest — or a qtest — has of the select working is
 * that the same offset holds two different values depending on it, and a
 * bit that only gates a log message could not show that.
 *
 * The trap the datasheet names is a model that wires bit 0 instead of bit 3,
 * inherited from a driver header that defines that constant but never
 * exercises it. Bit 0 already cannot reach this select: bus configuration
 * 1's write mask only stores bit 3 among the low four bits, so a guest
 * setting bit 0 changes nothing this device tracks, and the two-bank storage
 * here makes that a testable fact rather than an assertion about a mask.
 */
#define ISP_REG_SXP_RISC_BASE   0x80
#define ISP_REG_SXP_RISC_END    0xc0
#define ISP_SXP_RISC_COUNT      \
    ((ISP_REG_SXP_RISC_END - ISP_REG_SXP_RISC_BASE) / 2)

struct QLogicISPState {
    PCIDevice parent_obj;

    MemoryRegion io;    /* Base address register 0, the I/O window. */
    MemoryRegion mmio;  /* Base address register 1, the memory window. */

    uint16_t bus_cfg1;
    uint16_t bus_ctrl;
    uint16_t bus_status;
    uint16_t semaphore;
    uint16_t risc_flags;    /* What a read of the host command register says. */
    uint16_t mailbox[ISP_MAILBOX_COUNT];
    uint16_t risc_bank[ISP_SXP_RISC_COUNT];
    uint16_t sxp_bank[ISP_SXP_RISC_COUNT];
};

/*
 * The state a power-on or a soft reset both land on. The guest's soft reset is
 * a full re-initialisation of the chip rather than a narrower clear, so it and
 * the bus reset share this function instead of each clearing its own list of
 * registers and drifting apart.
 *
 * Mailboxes 4 and 5 are the queue pointers, and this is a place two sources
 * disagree: 86Box's model, evidence about the accept-and-answer approach
 * rather than about register values, sets them to 1 and 7 after reset. But
 * OpenBSD's own reset sequence explicitly zeroes all four queue-pointer names
 * with dedicated writes, and the datasheet's qtest checklist states the same
 * thing independently, so the higher-authority source here is behavioural
 * agreement between the guest and the datasheet, not the secondary model.
 * Zero is what this function sets, and this comment is the record of having
 * chosen it over the alternative rather than never having seen the alternative.
 *
 * Every other register this function zeroes — bus configuration 1, bus
 * interface control and status, the semaphore, the RISC flags, mailbox 0 and
 * 6/7, and both SXP/RISC banks — is doing so under the datasheet's own
 * standard for a value nothing confirms: no vendor register table exists for
 * this part, so zero stands because no driver source contradicts it, not
 * because a specification states it. The mailbox signature above is the
 * exception in this function, being driver-confirmed rather than merely
 * uncontradicted.
 */
static void qlogic_isp_reset_state(QLogicISPState *s)
{
    s->bus_cfg1 = 0;
    s->bus_ctrl = 0;
    s->bus_status = 0;
    s->semaphore = 0;
    s->risc_flags = 0;

    memset(s->mailbox, 0, sizeof(s->mailbox));
    s->mailbox[1] = ISP_MAILBOX1_SIGNATURE;
    s->mailbox[2] = ISP_MAILBOX2_SIGNATURE;
    s->mailbox[3] = ISP_MAILBOX3_SIGNATURE;

    memset(s->risc_bank, 0, sizeof(s->risc_bank));
    memset(s->sxp_bank, 0, sizeof(s->sxp_bank));
}

static void qlogic_isp_host_command(QLogicISPState *s, uint16_t val)
{
    unsigned cmd = val >> ISP_HOST_CMD_SHIFT;

    switch (cmd) {
    case ISP_HOST_CMD_NOP:
        break;

    case ISP_HOST_CMD_RESET_RISC:
        s->risc_flags |= ISP_RISC_FLAG_RESET;
        break;

    case ISP_HOST_CMD_PAUSE_RISC:
        s->risc_flags |= ISP_RISC_FLAG_PAUSED;
        break;

    /*
     * Releasing the RISC clears the paused flag, which the datasheet states,
     * and the reset flag, which it does not. The guest resets the processor and
     * then releases it as consecutive steps of its attach, so a release that
     * left the reset flag standing would report a processor held in reset for
     * the rest of the machine's life. That is this model's reading of what
     * releasing means rather than an observed behaviour.
     */
    case ISP_HOST_CMD_RELEASE_RISC:
        s->risc_flags &= ~(ISP_RISC_FLAG_PAUSED | ISP_RISC_FLAG_RESET);
        break;

    case ISP_HOST_CMD_SET_HOST_INT:
        s->risc_flags |= ISP_RISC_FLAG_HOST_INT;
        break;

    case ISP_HOST_CMD_CLEAR_HOST_INT:
        s->risc_flags &= ~ISP_RISC_FLAG_HOST_INT;
        break;

    /*
     * Accepted so the guest's attach proceeds, but with nothing behind them
     * yet: clearing the RISC interrupt is the first half of an interrupt
     * acknowledgement and there is no interrupt to acknowledge until the
     * mailbox engine exists, and the parity and BIOS commands control paths
     * this model does not have.
     */
    case ISP_HOST_CMD_CLEAR_RISC_INT:
    case ISP_HOST_CMD_MASK_PARITY:
    case ISP_HOST_CMD_BIOS_ENABLE:
    case ISP_HOST_CMD_ENABLE_PARITY:
    case ISP_HOST_CMD_PARITY_ERROR:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: host command 0x%x accepted, not modelled\n",
                      cmd);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: unknown host command 0x%x\n", cmd);
        break;
    }
}

/*
 * Bus configuration 1 bit 3 picks which of the two storage arrays the window
 * from 0x80 to 0xbf resolves to. Reading the bit fresh out of bus_cfg1 rather
 * than caching a selection is what makes a write to bus configuration 1 take
 * effect on the very next access to the window, which is the order NetBSD's
 * own access pattern relies on: select the bank, then touch the window.
 */
static uint16_t *qlogic_isp_active_bank(QLogicISPState *s)
{
    return (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ?
           s->sxp_bank : s->risc_bank;
}

static uint64_t qlogic_isp_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    QLogicISPState *s = opaque;

    if (addr >= ISP_REG_MAILBOX_BASE && addr < ISP_REG_MAILBOX_END) {
        return s->mailbox[(addr - ISP_REG_MAILBOX_BASE) / 2];
    }

    if (addr >= ISP_REG_SXP_RISC_BASE && addr < ISP_REG_SXP_RISC_END) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: read of unimplemented %s register "
                      "0x%02" HWADDR_PRIx "\n",
                      (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ? "SXP" : "RISC",
                      addr);
        return qlogic_isp_active_bank(s)[(addr - ISP_REG_SXP_RISC_BASE) / 2];
    }

    switch (addr) {
    case ISP_REG_ID_LOW:
        return QLOGIC_ISP_VENDOR_ID;
    case ISP_REG_ID_HIGH:
        return QLOGIC_ISP_DEVICE_ID;
    case ISP_REG_BUS_CFG0:
        return ISP_BUS_CFG0_HWREV;
    case ISP_REG_BUS_CFG1:
        return s->bus_cfg1;
    case ISP_REG_BUS_CTRL:
        return s->bus_ctrl;
    case ISP_REG_BUS_STATUS:
        return s->bus_status;
    case ISP_REG_SEMAPHORE:
        return s->semaphore;
    case ISP_REG_HOST_CMD:
        return s->risc_flags;
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
    QLogicISPState *s = opaque;

    if (addr >= ISP_REG_MAILBOX_BASE && addr < ISP_REG_MAILBOX_END) {
        s->mailbox[(addr - ISP_REG_MAILBOX_BASE) / 2] = val;
        return;
    }

    if (addr >= ISP_REG_SXP_RISC_BASE && addr < ISP_REG_SXP_RISC_END) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: write 0x%04x to unimplemented %s register "
                      "0x%02" HWADDR_PRIx "\n", (unsigned)val,
                      (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ? "SXP" : "RISC",
                      addr);
        qlogic_isp_active_bank(s)[(addr - ISP_REG_SXP_RISC_BASE) / 2] = val;
        return;
    }

    switch (addr) {
    case ISP_REG_BUS_CFG1:
        s->bus_cfg1 = val & ISP_BUS_CFG1_WRITE_MASK;
        break;

    /*
     * A soft reset takes effect immediately and leaves its own bit clear, so
     * the guest's poll for the bit to clear succeeds on its first read. It is
     * applied after the enables are stored so that the reset wins, which is
     * the order the write itself implies.
     */
    case ISP_REG_BUS_CTRL:
        s->bus_ctrl = val & ISP_BUS_CTRL_WRITE_MASK;
        if (val & ISP_BUS_CTRL_SOFT_RESET) {
            qlogic_isp_reset_state(s);
        }
        break;

    case ISP_REG_SEMAPHORE:
        s->semaphore = val & ISP_SEMAPHORE_MASK;
        break;

    case ISP_REG_HOST_CMD:
        qlogic_isp_host_command(s, val);
        break;

    case ISP_REG_ID_LOW:
    case ISP_REG_ID_HIGH:
    case ISP_REG_BUS_CFG0:
    case ISP_REG_BUS_STATUS:
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
 * access into repeated calls to a handler that now has side effects, and an
 * undeclared minimum is 1. Pinning both ends to two bytes makes one guest
 * access reach the handler exactly once, which matters here because a write to
 * the host command register and a write that soft-resets the chip both act
 * rather than store.
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

static void qlogic_isp_reset_hold(Object *obj, ResetType type)
{
    QLogicISPState *s = QLOGIC_ISP1040(obj);

    qlogic_isp_reset_state(s);
}

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
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QLogicISPState),
        VMSTATE_UINT16(bus_cfg1, QLogicISPState),
        VMSTATE_UINT16(bus_ctrl, QLogicISPState),
        VMSTATE_UINT16(bus_status, QLogicISPState),
        VMSTATE_UINT16(semaphore, QLogicISPState),
        VMSTATE_UINT16(risc_flags, QLogicISPState),
        VMSTATE_UINT16_ARRAY(mailbox, QLogicISPState, ISP_MAILBOX_COUNT),
        VMSTATE_UINT16_ARRAY(risc_bank, QLogicISPState, ISP_SXP_RISC_COUNT),
        VMSTATE_UINT16_ARRAY(sxp_bank, QLogicISPState, ISP_SXP_RISC_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static void qlogic_isp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = qlogic_isp_realize;
    pc->vendor_id = QLOGIC_ISP_VENDOR_ID;
    pc->device_id = QLOGIC_ISP_DEVICE_ID;
    pc->revision = QLOGIC_ISP_REVISION;
    pc->class_id = PCI_CLASS_STORAGE_SCSI;
    rc->phases.hold = qlogic_isp_reset_hold;
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
