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
#include "qemu/queue.h"
#include "hw/pci/pci_device.h"
#include "hw/scsi/scsi.h"
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
#define ISP_REG_NVRAM       0x0e    /* RW: bit-banged serial EEPROM. */
#define ISP_REG_DMA_CMD_CFG   0x20  /* RW: command DMA block, config. */
#define ISP_REG_DMA_CMD_CTRL  0x22  /* WO: command DMA block, control. */
#define ISP_REG_DMA_DATA_CFG  0x40  /* RW: data DMA block, config. */
#define ISP_REG_DMA_DATA_CTRL 0x42  /* WO: data DMA block, control. */
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
 * The command and data DMA blocks' config registers, at the base of each
 * block. Everything else either block carries -- control, status, FIFO
 * status, transfer count, address -- has no guest on origami's path that
 * reads or writes it, so only the config register is named and stored; the
 * rest still falls through to the unimplemented-register logger.
 *
 * The one field that matters here is the same burst enable bus
 * configuration 1 already has a copy of for the SBus variant of this part:
 * OpenBSD's attach sequence read-modify-writes this bit into both blocks'
 * config registers whenever the FIFO/burst configuration it already wrote
 * to bus configuration 1 asked for bursts. Before this register was named,
 * that read-modify-write's read half always saw zero (the unimplemented
 * default) rather than a value reflecting what had actually been written,
 * so a guest that ever reads this register back to check what it set would
 * see something this device never actually stored -- a gap the OpenBSD
 * boot path does not depend on, since it never re-reads what it wrote, but
 * a stricter guest could.
 */
#define ISP_DMA_CFG_BURST_ENABLE    0x0002
#define ISP_DMA_CFG_WRITE_MASK      ISP_DMA_CFG_BURST_ENABLE

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
 * The NVRAM interface bit-bangs a 128-byte serial EEPROM: the host toggles
 * clock and chip select and shifts data out on these same three bits, and
 * the part is supposed to shift data back on the fourth. This model never
 * asserts that fourth bit, so every bit-banged read the guest performs comes
 * back as the same fixed pattern regardless of what command or address the
 * guest thought it was shifting out — indistinguishable, from the guest's
 * side, from an EEPROM that answers but whose contents fail validation, and
 * that is deliberate: the datasheet's own recommendation for this register
 * is to leave data-in low and let the guest's own fallback path handle it,
 * which OpenBSD does cleanly by printing that the NVRAM is corrupt and
 * proceeding on built-in defaults. Implementing the shift register and a
 * plausible 128 bytes behind it would let us set values a real Origin 200's
 * EEPROM contents were never sourced for; this device does not need to
 * answer to be correct, only to fail the way real silence would.
 */
#define ISP_NVRAM_CLOCK      0x0001
#define ISP_NVRAM_CHIP_SEL   0x0002
#define ISP_NVRAM_DATA_OUT   0x0004
#define ISP_NVRAM_DATA_IN    0x0008
#define ISP_NVRAM_WRITE_MASK (ISP_NVRAM_CLOCK | ISP_NVRAM_CHIP_SEL | \
                              ISP_NVRAM_DATA_OUT)

/*
 * Eight 16-bit mailboxes. Mailbox 0 carries a command in and its result out;
 * the rest are arguments and returns selected by per-command bit masks.
 *
 * Mailboxes 4 and 5 are also the queue pointers. The driver names four of them
 * — request in and out, response in and out — at two addresses, because each
 * address means the host's pointer when written and the adapter's when read.
 *
 * The request side really is plain storage: the guest publishes its producer
 * count by writing mailbox 4, and the adapter is the only reader, so a read
 * of that address answering back whatever was last written is correct — it
 * always is what was last written, by the only writer there is.
 *
 * The response side is not symmetric and is not plain storage. Mailbox 5 is
 * the adapter's producer count when read and the guest's consumer
 * acknowledgement when written, and the datasheet is explicit that a guest's
 * ack write must never be able to overwrite the adapter's own published
 * count: the two land at the same address only because one direction is the
 * read and the other the write, not because they share one number. A read of
 * this address answers from the adapter's own producer state directly; a
 * write to it records the guest's consumer position and never touches that
 * state. See the response-queue handling in qlogic_isp_reg_read(),
 * qlogic_isp_reg_write() and qlogic_isp_post_status() below.
 *
 * All of that applies only while a queue exists. Both addresses are ordinary
 * mailboxes until the guest initialises the queue behind them, and both have
 * to be: the firmware's mailbox register test writes a walking pattern across
 * mailboxes 1 through 7 and reads it back, long before it initialises
 * anything, and a queue pointer answered there instead of the pattern fails
 * that test. Each read and the response side's write are gated on the queue's
 * own length for exactly that reason.
 */
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX_COUNT       8
#define ISP_REG_MAILBOX_END     (ISP_REG_MAILBOX_BASE + ISP_MAILBOX_COUNT * 2)

/* The queue pointers, at these offsets from the mailbox base. */
#define ISP_MAILBOX_REQUEST_QUEUE   4
#define ISP_MAILBOX_RESPONSE_QUEUE  5
#define ISP_REG_REQUEST_QUEUE   (ISP_REG_MAILBOX_BASE + 0x08)
#define ISP_REG_RESPONSE_QUEUE  (ISP_REG_MAILBOX_BASE + 0x0a)

/*
 * After reset the adapter presents an ASCII signature "IS", "P " and two
 * spaces in mailboxes 1, 2 and 3, plus a fourth, non-ASCII word in mailbox
 * 4. This was previously documented here as a fact about OpenBSD's driver;
 * it is not. The pinned OpenBSD/sgi tree's qlw_softreset() never reads
 * mailboxes 1 through 4 as an identity check at all -- it resets, zeroes
 * state, and goes straight to a no-op mailbox liveness check. Presenting
 * this signature is harmless to OpenBSD, which never looks, but it is not
 * what OpenBSD requires.
 *
 * What actually requires it, fatally, is Linux's qla1280 driver:
 * qla1280_chip_diag() reads mailboxes 1 through 4 immediately after reset,
 * before any mailbox command is issued, and compares them against
 * PROD_ID_1 through PROD_ID_4 (drivers/scsi/qla1280.h) -- 0x4953, 0x0000 or
 * 0x5020, 0x2020 and 0x0001. A mismatch on any of the four aborts the
 * adapter's attach outright ("Wrong product ID"). 86Box's ql_reset_asic
 * reproduces the identical four values in a second, unrelated
 * implementation, which is corroboration this is a real hardware fact
 * about the ISP1040 family rather than one driver's idiosyncrasy.
 *
 * It is also the observable that proves reset ran at all. A PCI device is
 * reset through its bus, and a device whose registers decode perfectly
 * while its reset was never called reads every register at its
 * zero-initialised value, which for most registers here is also the reset
 * value and so says nothing. These four do not read zero, so the qtest
 * asserting them is what distinguishes a device that was reset from one
 * that merely answers.
 */
#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020
#define ISP_MAILBOX4_SIGNATURE  0x0001

/*
 * The mailbox command protocol: the host writes mailbox 0 and whatever
 * arguments the command needs, then sets the host interrupt, and the adapter
 * answers by writing a result into mailbox 0 and posting an interrupt. Complete
 * and invalid are the two outcomes every command shares; a command-specific
 * error is any other value.
 */
#define ISP_MBOX_CMD_NOP                0x0000
#define ISP_MBOX_CMD_LOAD_RAM           0x0001
#define ISP_MBOX_CMD_EXEC_FIRMWARE      0x0002
#define ISP_MBOX_CMD_DUMP_RAM           0x0003
#define ISP_MBOX_CMD_WRITE_RAM_WORD     0x0004
#define ISP_MBOX_CMD_READ_RAM_WORD      0x0005
#define ISP_MBOX_CMD_MAILBOX_REG_TEST   0x0006
#define ISP_MBOX_CMD_VERIFY_CHECKSUM    0x0007
#define ISP_MBOX_CMD_ABOUT_FIRMWARE     0x0008
#define ISP_MBOX_CMD_INIT_REQ_QUEUE     0x0010
#define ISP_MBOX_CMD_INIT_RSP_QUEUE     0x0011
#define ISP_MBOX_CMD_BUS_RESET          0x0018
#define ISP_MBOX_CMD_GET_FIRMWARE_STATUS 0x001f
#define ISP_MBOX_CMD_INIT_REQ_QUEUE_A64  0x0052
#define ISP_MBOX_CMD_INIT_RSP_QUEUE_A64  0x0053
#define ISP_MBOX_CMD_SET_INITIATOR_ID      0x0030
#define ISP_MBOX_CMD_SET_SELECTION_TIMEOUT 0x0031
#define ISP_MBOX_CMD_SET_RETRY_COUNT       0x0032
#define ISP_MBOX_CMD_SET_TAG_AGE_LIMIT      0x0033
#define ISP_MBOX_CMD_SET_CLOCK_RATE         0x0034
#define ISP_MBOX_CMD_SET_ACTIVE_NEGATION    0x0035
#define ISP_MBOX_CMD_SET_ASYNC_DATA_SETUP   0x0036
#define ISP_MBOX_CMD_SET_PCI_CONTROL        0x0037
#define ISP_MBOX_CMD_GET_TARGET_PARAMETERS  0x0028
#define ISP_MBOX_CMD_SET_TARGET_PARAMETERS  0x0038
#define ISP_MBOX_CMD_SET_DEVICE_QUEUE       0x0039
#define ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS 0x003a
#define ISP_MBOX_CMD_SET_SYSTEM_PARAMETER   0x0045
#define ISP_MBOX_CMD_SET_FIRMWARE_FEATURES  0x004a
#define ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY 0x005a

#define ISP_MBOX_COMPLETE           0x4000
#define ISP_MBOX_INVALID            0x4001
#define ISP_MBOX_TEST_FAILED        0x4003
#define ISP_MBOX_CMD_PARAM_ERROR    0x4006

/*
 * The adapter runs QLogic microcode on an internal RISC processor, and the
 * guest downloads that microcode at every attach — OpenBSD 7.9's qlw always
 * does, since the build option that would skip it is set only for some Alpha
 * RAMDISK configurations and never for GENERIC-IP27, the kernel this fork
 * boots. This model does not run that microcode and must not try: it accepts
 * the download and answers it, the same way QEMU's other intelligent storage
 * controllers implement a message interface rather than executing vendor
 * firmware. Nothing here requires QLogic firmware to be present, distributed
 * or decoded, which is what makes accept-and-answer the only acceptable
 * design — the fork ships no vendor firmware.
 *
 * The download is one mailbox command per 16-bit word, so it runs thousands
 * of times per attach; the storage backing it has to be plain and cheap. A
 * flat array sized to the full 16-bit address space a mailbox argument can
 * name means every address a guest writes is valid by construction, with no
 * bounds check needed and no error path for an address a real adapter would
 * also have to accept.
 *
 * This retains what the guest downloads and computes checksums honestly over
 * it, rather than discarding words and answering success unconditionally.
 * The download is never executed either way; retaining it buys one thing —
 * a verify-checksum answered honestly catches a guest that sent a corrupt
 * image, which an unconditional success cannot. That is a recorded decision
 * from this device's design review, not a re-derivation of the datasheet's
 * own text, which frames discarding as the simpler default and retaining as
 * the fallback if a guest is ever seen to need it.
 */
#define ISP_RISC_RAM_WORDS      0x10000

/*
 * The request count a guest sizes its command queues from. The datasheet
 * calls this an open question with no source stating a real adapter's
 * number, and 512 is chosen because it is the driver's own cap — OpenBSD
 * clamps whatever this command reports to 512 immediately after reading it,
 * so reporting a larger number changes nothing the guest does, while 512
 * exercises the ring arithmetic at its actual ceiling instead of leaving
 * headroom untested.
 */
#define ISP_FIRMWARE_STATUS_MAX_REQUESTS 512

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

/*
 * Initialising a queue is a mailbox command like any other, but its address
 * argument is unusual: mailbox 1 is the entry count, and the queue's base
 * address is a 64-bit host address split across mailboxes 2, 3, 6 and 7
 * rather than the low pair alone. OpenBSD's own driver always sends this
 * split form — never the separate 64-bit-only command variant the datasheet
 * also names — so that is the only form this model needs to answer.
 *
 * Zero is rejected for either the address or the count. No guest on this
 * device's path sends either, so the check is unexercised by construction,
 * but it costs nothing and 86Box's model makes the same check, which is
 * evidence about the approach worth keeping even where it cannot be watched
 * failing here.
 */
#define ISP_MBOX_ADDR_LOW(mb)   ((uint64_t)(mb)[3])
#define ISP_MBOX_ADDR_MID(mb)   ((uint64_t)(mb)[2] << 16)
#define ISP_MBOX_ADDR_HIGH(mb)  ((uint64_t)(mb)[7] << 32)
#define ISP_MBOX_ADDR_TOP(mb)   ((uint64_t)(mb)[6] << 48)

/*
 * Queue entries are 64 bytes and every offset below is a byte offset into
 * one entry. Confirmed against OpenBSD's own packed struct declarations for
 * each entry kind, which leave no compiler padding to second-guess: a
 * request entry, its continuations, a status entry and a marker all total
 * exactly 64 bytes this way.
 */
#define ISP_QUEUE_ENTRY_SIZE    64

/* The entry header every kind of entry starts with. */
#define ISP_ENTRY_OFF_TYPE      0
#define ISP_ENTRY_OFF_COUNT     1

#define ISP_ENTRY_TYPE_REQUEST      0x01
#define ISP_ENTRY_TYPE_CONTINUATION 0x02
#define ISP_ENTRY_TYPE_STATUS       0x03
#define ISP_ENTRY_TYPE_MARKER       0x04

/*
 * A request entry. The device field packs bus, target and lun into one
 * 16-bit word rather than three separate bytes — confirmed from how
 * OpenBSD's driver builds the value it writes, (((bus << 7) | target) << 8)
 * | lun, not from the header's field layout alone, which only names the
 * field and does not spell out which bits mean what.
 */
#define ISP_REQ_OFF_HANDLE      4
#define ISP_REQ_OFF_DEVICE      8
#define ISP_REQ_OFF_CDBLEN      10
#define ISP_REQ_OFF_TIMEOUT     16
#define ISP_REQ_OFF_SEGCOUNT    18
#define ISP_REQ_OFF_CDB         20
#define ISP_REQ_CDB_LEN         12
#define ISP_REQ_OFF_SEGS        32
#define ISP_REQ_SEGS_INLINE     4

#define ISP_DEVICE_LUN(dev)     ((dev) & 0xff)
#define ISP_DEVICE_TARGET(dev)  (((dev) >> 8) & 0x7f)

/* A continuation entry: four bytes reserved, then seven more segments. */
#define ISP_CONT_OFF_SEGS       8
#define ISP_CONT_SEGS           7

/* One inline or continuation segment: a 32-bit address and a 32-bit length. */
#define ISP_SEG_SIZE            8

/*
 * The 64-bit addressing pair, Linux's only request format (datasheet's "64-bit
 * addressing entries" section). Confirmed directly against qla1280.h: offsets
 * 0-31 of cmd_a64_entry_t -- header, handle, the device word, cdb_len,
 * control_flags, reserved, timeout, dseg_count, and the 12-byte CDB -- are
 * byte-identical to the type-0 format's struct cmd_entry, field for field.
 * ISP_REQ_OFF_HANDLE/_DEVICE/_CDBLEN/_SEGCOUNT/_CDB above are reused as-is;
 * this is the same header both formats share, not a coincidence of two
 * formats independently landing on the same numbers. Only the segment area
 * from offset 32 on differs, because A64 needs the extra room a 64-bit
 * address costs.
 */
#define ISP_A64_ENTRY_TYPE_COMMAND    0x09
#define ISP_A64_ENTRY_TYPE_CONTINUE   0x0a

#define ISP_A64_REQ_OFF_SEGS        40
#define ISP_A64_REQ_SEGS_INLINE     2

/*
 * A 64-bit segment: a 64-bit address as two 32-bit words, low half first
 * (confirmed from qla1280_64bit_start_scsi()'s own write order --
 * lower_32_bits() before upper_32_bits(), the same convention this file's
 * A64 mailbox address macros already use), then a 32-bit length.
 */
#define ISP_A64_SEG_SIZE            12

/*
 * A64 continuation entry: no handle, no reserved gap -- five wider segments
 * start immediately at offset 4, where a command entry's handle would be.
 * Reading this offset as a handle is what made an unrecognized-entry
 * continuation dangerous rather than merely incomplete (task #106/#74
 * adversarial review): it is the low half of a DMA buffer address here, not
 * an identifier, so a continuation entry must never be dispatched on its
 * own -- entry_count always steps the ring walker past it as part of the
 * command that owns it, the same mechanism that already skips type-0
 * continuation entries.
 */
#define ISP_A64_CONT_OFF_SEGS        4
#define ISP_A64_CONT_SEGS            5

/*
 * A status entry, written by the adapter. Sense data validity rides in the
 * SCSI status word's own bit 9 rather than a separate flag, which is easy to
 * miss because nothing about the field's name says so.
 */
#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_STATE_FLAGS  12
#define ISP_STATUS_OFF_STATUS_FLAGS 14
#define ISP_STATUS_OFF_RSP_LEN      16
#define ISP_STATUS_OFF_SENSE_LEN    18
#define ISP_STATUS_OFF_RESID        20
#define ISP_STATUS_OFF_SENSE_DATA   32
#define ISP_STATUS_SENSE_MAX        32

#define ISP_SCSI_STATUS_SENSE_VALID 0x0200

#define ISP_IOCB_COMPLETE            0x0000
#define ISP_IOCB_INCOMPLETE          0x0001
#define ISP_IOCB_DATA_UNDERRUN       0x0015

/*
 * State flags: which phases of the transaction the adapter actually
 * reached, read by OpenBSD's and Linux's drivers alike only when the
 * completion code is incomplete, to decide what kind of failure this was
 * rather than treat every incomplete command the same way. Confirmed
 * byte-identical (offset 12, this bit layout) against both qlwreg.h's
 * struct qlw_iocb_status and qla1280.h's struct response -- two
 * independent drivers agreeing on the wire format of a part neither
 * vendor documented for us. Linux's qla1280_return_status() walks this
 * ladder top to bottom on an incomplete completion: no bus reached is
 * DID_NO_CONNECT, bus reached but no target is DID_BAD_TARGET, target
 * reached but the CDB, transfer, status or sense phase failed is
 * DID_ERROR. A model that leaves this field unwritten hands back
 * whatever a previous occupant of the ring slot left, so an incomplete
 * completion is misreported as whichever failure those stale bits
 * happen to spell -- this device's only current incomplete completion
 * (no SCSI device at the requested target/lun) reported as a plausible
 * but wrong reason instead of DID_BAD_TARGET, the one that is actually
 * true.
 */
#define ISP_STATE_FLAG_GOT_BUS            0x0100
#define ISP_STATE_FLAG_GOT_TARGET         0x0200
#define ISP_STATE_FLAG_SENT_CDB           0x0400
#define ISP_STATE_FLAG_TRANSFERRED_DATA   0x0800
#define ISP_STATE_FLAG_GOT_STATUS         0x1000
#define ISP_STATE_FLAG_GOT_SENSE          0x2000
#define ISP_STATE_FLAG_TRANSFER_COMPLETE  0x4000
#define ISP_STATE_FLAGS_ALL_PHASES  (ISP_STATE_FLAG_GOT_BUS | \
                                     ISP_STATE_FLAG_GOT_TARGET | \
                                     ISP_STATE_FLAG_SENT_CDB | \
                                     ISP_STATE_FLAG_TRANSFERRED_DATA | \
                                     ISP_STATE_FLAG_GOT_STATUS | \
                                     ISP_STATE_FLAG_GOT_SENSE | \
                                     ISP_STATE_FLAG_TRANSFER_COMPLETE)

/*
 * status_flags (offset 14) is a field distinct from state_flags: it reports
 * conditions about the completion rather than which phases the command
 * reached. IRIX reads its timeout bit to accept an incomplete completion as a
 * benign selection timeout -- a stricter check than the four public parallel-
 * SCSI ISP drivers, which key only on state_flags. The bit value is NetBSD
 * ispmbox.h's RQSTF_TIMEOUT; qla1280.h and qlwreg.h carry the field but
 * enumerate none of its bits, consistent with a reader they do not model.
 */
#define ISP_STATUS_FLAG_TIMEOUT           0x0040

/*
 * A status entry the response ring had no room for. Real hardware never
 * builds this list — the datasheet's own words are that it never reaches the
 * full case — but a model with no bus latency can, and dropping a completion
 * on the floor there would only relocate the same lost-completion bug onto
 * whichever command lost the race. Queued here, it waits for
 * qlogic_isp_drain_pending() to post it once the guest's next acknowledgement
 * frees a slot; nothing about the SCSI request itself needs to stay alive for
 * that, since the backend has already finished it by the time this is built.
 */
typedef struct QLogicISPPendingStatus {
    QTAILQ_ENTRY(QLogicISPPendingStatus) next;
    uint32_t handle;
    uint16_t scsi_status;
    uint16_t completion;
    uint16_t state_flags;
    uint16_t status_flags;
    uint32_t resid;
    uint8_t sense[ISP_STATUS_SENSE_MAX];
    int sense_len;
} QLogicISPPendingStatus;

struct QLogicISPState {
    PCIDevice parent_obj;

    MemoryRegion io;    /* Base address register 0, the I/O window. */
    MemoryRegion mmio;  /* Base address register 1, the memory window. */

    uint16_t bus_cfg1;
    uint16_t bus_ctrl;
    uint16_t bus_status;
    uint16_t semaphore;
    uint16_t nvram;    /* Echoes the clock/select/data-out bits it was sent. */
    uint16_t risc_flags;    /* What a read of the host command register says. */
    uint16_t dma_cmd_cfg;    /* Command DMA block's config register. */
    uint16_t dma_data_cfg;   /* Data DMA block's config register. */
    uint16_t mailbox[ISP_MAILBOX_COUNT];
    uint16_t risc_bank[ISP_SXP_RISC_COUNT];
    uint16_t sxp_bank[ISP_SXP_RISC_COUNT];
    uint16_t risc_ram[ISP_RISC_RAM_WORDS];    /* Downloaded firmware. */

    SCSIBus bus;

    hwaddr rqst_base;
    hwaddr resp_base;
    uint16_t rqst_len;
    uint16_t resp_len;
    uint16_t rqst_cons;    /* Next request entry this device will read. */
    uint16_t resp_prod;    /* Next response slot this device will write. */
    uint16_t resp_cons;    /* Next response slot the guest has acknowledged. */

    /*
     * Completions the response ring had no room for when they were ready to
     * post. Not covered by vmstate: a migration landing while this is
     * non-empty loses the queued completions, the same gap this device
     * already has for every other in-flight SCSI request, and the condition
     * these exist for is one the datasheet itself says real hardware never
     * reaches.
     */
    QTAILQ_HEAD(, QLogicISPPendingStatus) pending_status;
};

/*
 * State for one in-flight SCSI command, from the request entry that started
 * it to the status entry its completion posts. hba_private on the SCSIRequest
 * is how QEMU's generic SCSI layer hands this back on completion or
 * cancellation, the same mechanism every SCSI HBA model in the tree uses.
 */
typedef struct QLogicISPRequest {
    QLogicISPState *dev;
    SCSIRequest *sreq;
    QEMUSGList qsg;
    uint32_t handle;
} QLogicISPRequest;

/*
 * The adapter presents one PCI interrupt line and it is a level, so the line
 * is recomputed from scratch on every change rather than tracked
 * incrementally: whatever changed — bus interface status, the enables in bus
 * interface control, or a reset zeroing both — funnels through here, and the
 * line is exactly what this function says it should be afterwards.
 *
 * Only the RISC interrupt request bit and its own enable matter to the
 * physical line, not the plain interrupt request bit alongside it or the
 * global enable bit next to that. 86Box's model masks the computed level down
 * to the RISC bit alone before checking it, with a comment stating outright
 * that unmasking the other status bit does nothing on real hardware; the
 * datasheet documents a global enable bit without saying what it does, which
 * is consistent with 86Box's reading that it does nothing observable either.
 * A source that says a bit is inert is worth as much as one that says what a
 * bit does, and is why this model does not condition the line on it.
 */
static void qlogic_isp_update_irq(QLogicISPState *s)
{
    bool level = (s->bus_status & s->bus_ctrl &
                  ISP_BUS_STATUS_RISC_INT_REQ) != 0;

    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * Frees every completion still waiting for response-ring room, without
 * posting them. Shared by the two places that re-initialise the response
 * queue's state from under a non-empty list — a chip reset and the
 * mailbox command that re-initialises the response queue on its own — so
 * neither has to duplicate the walk.
 */
static void qlogic_isp_drop_pending_status(QLogicISPState *s)
{
    QLogicISPPendingStatus *pending;

    while ((pending = QTAILQ_FIRST(&s->pending_status)) != NULL) {
        QTAILQ_REMOVE(&s->pending_status, pending, next);
        g_free(pending);
    }
}

/*
 * The state a power-on or a soft reset both land on. The guest's soft reset is
 * a full re-initialisation of the chip rather than a narrower clear, so it and
 * the bus reset share this function instead of each clearing its own list of
 * registers and drifting apart.
 *
 * Mailboxes 4 and 5 are the queue pointers, and this is a place an earlier
 * decision here was wrong and is now corrected against a source that settles
 * it. 86Box's ql_reset_asic sets them to 1 and 7 after reset. That was
 * previously rejected in favor of zero, reasoning that OpenBSD's own reset
 * sequence explicitly zeroes all four queue-pointer names with dedicated
 * writes and that the datasheet's qtest checklist stated the same thing
 * independently -- but those two were never independent: the datasheet's
 * claim was written from watching OpenBSD's driver-side zero-write, not from
 * a source that observes the raw hardware default, so "OpenBSD does this"
 * and "the datasheet says this" were one source counted twice. Linux's
 * qla1280 broke the tie: qla1280_chip_diag() reads mailbox 4 as part of a
 * fatal product-ID check immediately after reset, before any driver write
 * reaches it, and requires exactly the value 86Box set. OpenBSD is
 * unaffected either way, since qlw_softreset() overwrites this register with
 * its own zero before ever treating it as a live pointer.
 *
 * Mailbox 4's signature lives in s->mailbox[4], where the register test can
 * also reach it. A read of that address answers this adapter's own consumer
 * position only once a request queue is live; before then it answers the
 * array, which is both what the product-ID check wants and what the mailbox
 * register test's echo needs.
 *
 * Mailbox 5 keeps its zero. Nothing we have observed needs 86Box's 7 there:
 * no guest this fork targets checks it as an identity field the way mailbox
 * 4 turned out to be, and zero is what the register test writes over. A raw
 * mailbox[5] reset value would only matter if some future guest read this
 * offset by its mailbox name before ever touching it as a queue pointer, and
 * no source we hold shows one doing that.
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
    s->nvram = 0;
    s->risc_flags = 0;
    s->dma_cmd_cfg = 0;
    s->dma_data_cfg = 0;

    memset(s->mailbox, 0, sizeof(s->mailbox));
    s->mailbox[1] = ISP_MAILBOX1_SIGNATURE;
    s->mailbox[2] = ISP_MAILBOX2_SIGNATURE;
    s->mailbox[3] = ISP_MAILBOX3_SIGNATURE;
    s->mailbox[4] = ISP_MAILBOX4_SIGNATURE;

    memset(s->risc_bank, 0, sizeof(s->risc_bank));
    memset(s->sxp_bank, 0, sizeof(s->sxp_bank));

    /*
     * The guest re-downloads firmware from scratch after every reset — it is
     * part of the same attach sequence that follows a soft reset, never
     * skipped — so nothing depends on RAM surviving one, and zeroing it here
     * is the same uncontradicted-default choice as every other register this
     * function clears.
     */
    memset(s->risc_ram, 0, sizeof(s->risc_ram));

    /*
     * The driver re-initialises both queues after every reset, as part of
     * the same attach sequence that re-downloads firmware, so a stale base
     * address or length from before the reset is never acted on — but
     * zeroing them here means this device cannot act on one either, rather
     * than relying on the driver's ordering to keep it safe.
     */
    s->rqst_base = 0;
    s->resp_base = 0;
    s->rqst_len = 0;
    s->resp_len = 0;
    /*
     * A real consumer position, zero. It is only what a read of mailbox 4
     * answers once a request queue exists; before that the mailbox array
     * answers, carrying the identity value Linux's product-ID check reads.
     */
    s->rqst_cons = 0;
    s->resp_prod = 0;
    s->resp_cons = 0;

    /*
     * Same reasoning as the queue state above: the driver re-initialises the
     * response queue after every reset, so nothing queued from before it is
     * ever going to be acted on.
     */
    qlogic_isp_drop_pending_status(s);

    qlogic_isp_update_irq(s);
}

/*
 * Moves a block of the adapter's own memory to or from host memory, which is
 * what the load-RAM and dump-RAM mailbox commands each ask for in one
 * direction. Mailbox 1 is the word offset within the adapter, mailbox 4 the
 * word count, and mailboxes 2 and 3 the host address high half first — the
 * same four the command rings already reassemble, so the address macros are
 * shared rather than restated and a 64-bit host address costs nothing extra.
 *
 * The offset advances as a uint16_t so it wraps within the adapter's own
 * memory rather than running past the end of it, the same reasoning the
 * checksum command's walk of the same storage already records. Every offset a
 * uint16_t can name indexes storage sized to that range, so there is nothing
 * to validate; the host address is the guest's business and the DMA layer's.
 *
 * Through the PCI DMA accessors, never cpu_physical_memory_*, so the transfer
 * is translated by the Bridge the adapter sits behind exactly as the command
 * rings are. Little-endian, matching those rings: this is a little-endian PCI
 * device, and the Bridge translates addresses without swapping DMA payload, so
 * the accessor chosen here is the byte order that lands in guest memory.
 */
static void qlogic_isp_ram_block_dma(QLogicISPState *s, bool to_adapter)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t base = s->mailbox[1];
    uint16_t count = s->mailbox[4];
    hwaddr host = ISP_MBOX_ADDR_TOP(s->mailbox)
                | ISP_MBOX_ADDR_HIGH(s->mailbox)
                | ISP_MBOX_ADDR_MID(s->mailbox)
                | ISP_MBOX_ADDR_LOW(s->mailbox);
    uint16_t i;

    for (i = 0; i < count; i++) {
        uint16_t *slot = &s->risc_ram[(uint16_t)(base + i)];

        if (to_adapter) {
            lduw_le_pci_dma(pci, host + i * 2, slot, MEMTXATTRS_UNSPECIFIED);
        } else {
            stw_le_pci_dma(pci, host + i * 2, *slot, MEMTXATTRS_UNSPECIFIED);
        }
    }
}

/*
 * Runs a mailbox command. The host writes mailbox 0 and sets the host
 * interrupt to mean "a command is ready", which is what calls this; the
 * adapter answers by writing a result into mailbox 0, taking the semaphore
 * lock, posting an interrupt, and letting the line reflect that.
 *
 * Every command implemented here is one OpenBSD's own attach sequence
 * actually issues, from the liveness check through firmware download and
 * bring-up. Everything else still answers invalid, which is accurate today
 * — the mailbox protocol has no way to leave a command pending — and will
 * need revisiting one command at a time as later commits give the adapter
 * more to do, not a placeholder that happens to work because nothing yet
 * asks for anything else.
 *
 * Processing synchronously, inside the register write that triggers it,
 * matches how QEMU's other message-interface storage controllers handle a
 * command that does not touch guest memory, and keeps a qtest deterministic:
 * the result is present the instant the write that requested it returns,
 * with no timer or scheduled callback to wait on.
 */
static void qlogic_isp_mailbox_command(QLogicISPState *s)
{
    switch (s->mailbox[0]) {
    case ISP_MBOX_CMD_NOP:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The guest sends one of these per 16-bit word of the firmware image, so
     * this path runs thousands of times per attach. Mailbox 1 is the address,
     * mailbox 2 the data; every address a uint16_t can name is a valid index
     * into storage sized to the same range, so there is nothing to validate.
     */
    case ISP_MBOX_CMD_WRITE_RAM_WORD:
        s->risc_ram[s->mailbox[1]] = s->mailbox[2];
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The mirror of the write above, and the same absence of validation for
     * the same reason. No OS driver issues this — Linux declares the opcode
     * and never sends it — but the SGI firmware's power-on SCSI diagnostic
     * writes a word and reads it back, and answering it with invalid is what
     * failed that test and cost the machine its disks.
     *
     * Mailbox 3 is answered too, and answered zero. No driver source shows
     * this command's outputs, but 86Box's model of the same chip declares
     * both mailboxes outputs and clears the second, and that model had to
     * satisfy real firmware. Leaving mailbox 3 alone hands back whatever the
     * previous command left there -- a value this command never wrote.
     */
    case ISP_MBOX_CMD_READ_RAM_WORD:
        s->mailbox[2] = s->risc_ram[s->mailbox[1]];
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The block forms of the same two directions, which the firmware's
     * diagnostic uses to push a pattern through the adapter's memory and read
     * it back over DMA. A driver would reach for load-RAM to download firmware
     * in one transfer instead of a mailbox command per word; ours never does,
     * which is why neither appeared until the firmware was traced.
     */
    case ISP_MBOX_CMD_LOAD_RAM:
        qlogic_isp_ram_block_dma(s, true);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    case ISP_MBOX_CMD_DUMP_RAM:
        qlogic_isp_ram_block_dma(s, false);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * A wraparound self-test: whatever the guest writes into mailboxes 1
     * through 7 along with this command is expected back unchanged, proving
     * the mailbox path round-trips before the guest trusts anything else
     * over it. Linux's qla1280_chip_diag() issues this unconditionally,
     * immediately after the product-ID check and before firmware download,
     * with a fixed walking pattern (0xAAAA, 0x5555, 0xAA55, 0x55AA, 0xA5A5,
     * 0x5A5A, 0x2525) and treats a mismatch as a dead adapter. There is
     * nothing to compute: the values are already sitting in s->mailbox[] by
     * the time this command runs, so completing without touching them is
     * the whole implementation.
     */
    case ISP_MBOX_CMD_MAILBOX_REG_TEST:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Mailbox 1 is the base address; the word at base+3 is the image's own
     * length in words, which is also what the guest's own download loop uses
     * to know when it has sent the last word — so reading it back here rather
     * than being told the length separately is what the guest's own protocol
     * already relies on. The sum is over exactly that many words, wrapping
     * within the storage rather than erroring on an out-of-range address,
     * because every address is in range by construction.
     *
     * A real ISP1040 firmware image is built so this sum is zero; answering
     * honestly is what lets a guest that sent a corrupt image see a failure
     * instead of an unconditional success papering over it.
     */
    case ISP_MBOX_CMD_VERIFY_CHECKSUM: {
        uint16_t start = s->mailbox[1];
        uint16_t len = s->risc_ram[(uint16_t)(start + 3)];
        uint16_t sum = 0;
        uint16_t i;

        for (i = 0; i < len; i++) {
            sum += s->risc_ram[(uint16_t)(start + i)];
        }

        /*
         * The sum is answered in mailbox 2 as well as mailbox 1. NetBSD's
         * mask for this command makes both outputs, and the SGI firmware
         * reads the one 86Box's model does not write: left alone, mailbox 2
         * still holds whatever the previous command put there, and the
         * firmware compares that against its own sum and reports a mismatch
         * against a value this command never produced.
         */
        s->mailbox[1] = sum;
        s->mailbox[2] = sum;
        s->mailbox[0] = (sum == 0) ? ISP_MBOX_COMPLETE : ISP_MBOX_TEST_FAILED;
        break;
    }

    /*
     * The download is accepted and answered, never executed — this is the
     * command after which the guest trusts the mailbox set below is live,
     * which for this model was already true from reset. Nothing distinguishes
     * "before" and "after" here because nothing here needs the distinction.
     */
    case ISP_MBOX_CMD_EXEC_FIRMWARE:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The driver prints this triple and aborts if the command fails, so it
     * must succeed; the values themselves do not have to match any real
     * QLogic release; OpenBSD's own attach code only ever prints them; it
     * does not branch on them for this part. A fixed, plainly synthetic
     * version costs nothing OpenBSD would notice, against the real cost of
     * parsing a version string out of the downloaded image the way 86Box
     * does — worth naming as the road not taken, since it is a deliberate
     * simplification rather than a gap: 86Box's approach is nicer when the
     * guest inspects the answer, and this guest does not.
     */
    case ISP_MBOX_CMD_ABOUT_FIRMWARE:
        s->mailbox[1] = 1;
        s->mailbox[2] = 0;
        s->mailbox[3] = 0;
        s->mailbox[6] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Sizes every queue the guest allocates and its command budget — see
     * the constant's own comment for why 512 is what is reported.
     */
    case ISP_MBOX_CMD_GET_FIRMWARE_STATUS:
        s->mailbox[1] = 0;
        s->mailbox[2] = ISP_FIRMWARE_STATUS_MAX_REQUESTS;
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Read back a target's negotiated transfer parameters (the target is in
     * mailbox 1). Linux's qla1280 issues it once per enumerated target. This
     * model negotiates nothing, so it reports every target asynchronous and
     * narrow with no options -- mailbox 2 the parameter flags, mailbox 3 the
     * period and offset, both zero -- the true state of a bus it does not
     * negotiate, and what set-target-parameters (0x38) would have written.
     */
    case ISP_MBOX_CMD_GET_TARGET_PARAMETERS:
        s->mailbox[2] = 0;
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The "config setters" the datasheet's own bring-up section names in
     * order: firmware features, clock rate, retry count, async data setup,
     * active negation, tag age limit and selection timeout for the adapter
     * as a whole, then initiator ID per bus and target parameters and
     * per-LUN queue depth per target. OpenBSD's qlw_attach() treats every
     * one of these as fatal: a single command answering invalid aborts the
     * whole attach before the SCSI bus is ever created. A real boot found
     * exactly that gap — this model answered every earlier command
     * correctly and then went silent here, so qlw0 attached but never grew
     * a scsibus and no disk behind it was ever enumerated.
     *
     * None of these carries behavior this model tracks: they configure
     * negotiation and timing parameters for a bus this model does not
     * negotiate and a target it does not throttle, and nothing later in
     * the boot path reads any of them back. The same reasoning already
     * used for bus reset applies to all of them — the only contract is
     * that the command completes.
     */
    case ISP_MBOX_CMD_SET_FIRMWARE_FEATURES:
    case ISP_MBOX_CMD_SET_CLOCK_RATE:
    case ISP_MBOX_CMD_SET_RETRY_COUNT:
    case ISP_MBOX_CMD_SET_ASYNC_DATA_SETUP:
    case ISP_MBOX_CMD_SET_ACTIVE_NEGATION:
    case ISP_MBOX_CMD_SET_TAG_AGE_LIMIT:
    case ISP_MBOX_CMD_SET_SELECTION_TIMEOUT:
    case ISP_MBOX_CMD_SET_INITIATOR_ID:
    case ISP_MBOX_CMD_SET_TARGET_PARAMETERS:
    case ISP_MBOX_CMD_SET_DEVICE_QUEUE:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * IRIX programs the SCSI bus-reset delay during adapter init. Like the
     * setters above it configures timing this model does not track, so the
     * only contract is that it completes.
     */
    case ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Three more config setters, issued only by Linux's qla1280_nvram_config()
     * (PCI DMA burst enables, the ISP parameter word, and the SCSI-bus-reset
     * data-overrun recovery mode) and previously unmodelled, which fell into
     * the default case below and answered invalid. Unlike the group above,
     * an invalid answer here cannot be load-bearing for either guest: OpenBSD's
     * qlwreg.h defines all three of these opcodes but qlw.c never issues any of
     * them (confirmed by an exhaustive grep of the file, not a name search that
     * merely failed to find a call site), and Linux's own caller,
     * qla1280_initialize_adapter(), calls qla1280_nvram_config() and discards
     * its returned status outright -- the OR of every mailbox result inside it,
     * including these three, is never assigned to anything the adapter-init
     * path checks. Answering invalid was pure noise: harmless to both drivers,
     * logged as an error nonetheless. Treating them the same as the group
     * above -- accepted, nothing tracked -- matches what both guests already
     * demonstrate about their own indifference to the result.
     */
    case ISP_MBOX_CMD_SET_PCI_CONTROL:
    case ISP_MBOX_CMD_SET_SYSTEM_PARAMETER:
    case ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The plain opcode and its A64 counterpart are two numbers for the same
     * operation on this hardware, not two protocols: OpenBSD's qlw_attach()
     * issues QLW_MBOX_INIT_REQ_QUEUE (0x10) and composes the queue's base
     * address across mailboxes 2, 3, 6 and 7 with qlw_mbox_putaddr(); Linux's
     * qla1280_init_rings() issues MBC_INIT_REQUEST_QUEUE_A64 (0x52) instead
     * -- its own source comments out the plain opcode rather than using it --
     * and composes the identical 64-bit address across the identical four
     * mailboxes. Only the opcode number differs between the two guests, so
     * both fall into this one case rather than needing separate logic.
     */
    case ISP_MBOX_CMD_INIT_REQ_QUEUE:
    case ISP_MBOX_CMD_INIT_REQ_QUEUE_A64: {
        uint64_t addr = ISP_MBOX_ADDR_TOP(s->mailbox) |
                        ISP_MBOX_ADDR_HIGH(s->mailbox) |
                        ISP_MBOX_ADDR_MID(s->mailbox) |
                        ISP_MBOX_ADDR_LOW(s->mailbox);

        if (addr == 0 || s->mailbox[1] == 0) {
            s->mailbox[0] = ISP_MBOX_CMD_PARAM_ERROR;
            break;
        }

        s->rqst_base = addr;
        s->rqst_len = s->mailbox[1];
        s->rqst_cons = 0;
        s->mailbox[ISP_MAILBOX_REQUEST_QUEUE] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;
    }

    case ISP_MBOX_CMD_INIT_RSP_QUEUE:
    case ISP_MBOX_CMD_INIT_RSP_QUEUE_A64: {
        uint64_t addr = ISP_MBOX_ADDR_TOP(s->mailbox) |
                        ISP_MBOX_ADDR_HIGH(s->mailbox) |
                        ISP_MBOX_ADDR_MID(s->mailbox) |
                        ISP_MBOX_ADDR_LOW(s->mailbox);

        if (addr == 0 || s->mailbox[1] == 0) {
            s->mailbox[0] = ISP_MBOX_CMD_PARAM_ERROR;
            break;
        }

        s->resp_base = addr;
        s->resp_len = s->mailbox[1];
        s->resp_prod = 0;
        s->resp_cons = 0;
        /*
         * No write to mailbox[ISP_MAILBOX_RESPONSE_QUEUE] here: from this
         * point the queue is live, so a read of that address answers from
         * resp_prod and the plain storage behind it is not consulted again
         * until a reset clears the queue.
         */
        qlogic_isp_drop_pending_status(s);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;
    }

    /*
     * Nothing in this model distinguishes a bus that has been reset from one
     * that has not — there are no in-flight commands to abort and no
     * negotiated transfer parameters to drop, since neither is modelled yet
     * — so the only contract to honour is the one the driver checks: that
     * the command completes.
     */
    case ISP_MBOX_CMD_BUS_RESET:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: mailbox command 0x%04x not modelled\n",
                      s->mailbox[0]);
        s->mailbox[0] = ISP_MBOX_INVALID;
        break;
    }

    s->semaphore = ISP_SEMAPHORE_LOCK;
    s->bus_status |= ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ;
    qlogic_isp_update_irq(s);
}

static void qlogic_isp_host_command(QLogicISPState *s, uint16_t val)
{
    unsigned cmd = val >> ISP_HOST_CMD_SHIFT;

    switch (cmd) {
    case ISP_HOST_CMD_NOP:
        break;

    /*
     * Resetting the RISC replaces every flag rather than adding one, which
     * this model did not originally do: an earlier version only set the
     * reset flag and left a pending host interrupt or a paused flag standing
     * alongside it. 86Box's state machine — evidence about behaviour, which
     * the datasheet marks as this model's own and worth reading, as distinct
     * from its register definitions, which are inherited from elsewhere —
     * treats a RISC reset as replacing the whole flag set with reset alone,
     * consistent with the flags being separate bits of one processor-state
     * latch bank that a reset pulse would clear together.
     */
    case ISP_HOST_CMD_RESET_RISC:
        s->risc_flags = ISP_RISC_FLAG_RESET;
        break;

    case ISP_HOST_CMD_PAUSE_RISC:
        s->risc_flags |= ISP_RISC_FLAG_PAUSED;
        break;

    /*
     * Releasing the RISC clears the paused flag, which the datasheet states,
     * and the reset flag, which it does not. The guest resets the processor and
     * then releases it as consecutive steps of its attach, so a release that
     * left the reset flag standing would report a processor held in reset for
     * the rest of the machine's life. That was this model's own reading before
     * it was corroborated: 86Box's independent state machine clears exactly
     * these two flags and no others on release, which is the second,
     * unrelated implementation this reading needed to move from inference to
     * observation.
     */
    case ISP_HOST_CMD_RELEASE_RISC:
        s->risc_flags &= ~(ISP_RISC_FLAG_PAUSED | ISP_RISC_FLAG_RESET);
        break;

    /*
     * Setting the host interrupt is how the driver tells the adapter a
     * mailbox command is ready, so it is also what triggers processing one.
     */
    case ISP_HOST_CMD_SET_HOST_INT:
        s->risc_flags |= ISP_RISC_FLAG_HOST_INT;
        qlogic_isp_mailbox_command(s);
        break;

    /*
     * This command does nothing on real hardware. 86Box's model says so
     * outright, from its own reverse-engineering notes rather than from
     * register-map inheritance; neither qla1280 nor OpenBSD's qlw, the two
     * drivers whose behaviour this file otherwise leans on, ever issues it.
     * An earlier version of this model cleared the host-interrupt flag here,
     * which was invented rather than sourced — the flag is cleared by
     * clearing the RISC interrupt instead, once that command is implemented.
     */
    case ISP_HOST_CMD_CLEAR_HOST_INT:
        break;

    /*
     * Clearing the RISC interrupt is the first half of every interrupt
     * acknowledgement, and 86Box's model clears exactly the host-interrupt
     * flag and the whole of bus interface status here — nothing else. For a
     * mailbox completion the driver also writes zero to the semaphore
     * separately, which is what actually releases the lock; this command
     * does not touch it.
     */
    case ISP_HOST_CMD_CLEAR_RISC_INT:
        s->risc_flags &= ~ISP_RISC_FLAG_HOST_INT;
        s->bus_status = 0;
        qlogic_isp_update_irq(s);
        break;

    /*
     * Accepted so the guest's attach proceeds, but with nothing behind them:
     * the parity and BIOS commands control paths this model does not have.
     */
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

/*
 * Descriptor reads.
 *
 * Plain little-endian, matching the rings the guest builds. Any byte-lane
 * permutation this machine needs happens in the Bridge, on the strength of the
 * requesting slot's own swap bits, so nothing here has to know which mapping
 * window the ring sits in or what the guest programmed.
 */
static uint32_t qlogic_isp_desc_ld32(PCIDevice *pci, hwaddr addr)
{
    uint32_t v = 0;

    ldl_le_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static uint16_t qlogic_isp_desc_ld16(PCIDevice *pci, hwaddr addr)
{
    uint16_t v = 0;

    lduw_le_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static uint8_t qlogic_isp_desc_ld8(PCIDevice *pci, hwaddr addr)
{
    uint8_t v = 0;

    ldub_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static hwaddr qlogic_isp_rqst_entry_addr(QLogicISPState *s, uint16_t index)
{
    return s->rqst_base + (hwaddr)index * ISP_QUEUE_ENTRY_SIZE;
}

static hwaddr qlogic_isp_resp_entry_addr(QLogicISPState *s, uint16_t index)
{
    return s->resp_base + (hwaddr)index * ISP_QUEUE_ENTRY_SIZE;
}

/*
 * Whether the response ring has room for one more entry without the
 * producer catching up to the consumer. Reserve one slot so producer equal
 * to consumer always means empty, never full — the datasheet's own
 * requirement: a ring that reserves nothing cannot tell the two apart, and a
 * full ring left indistinguishable from an empty one is exactly how a
 * completion goes missing without the guest or the adapter doing anything
 * wrong. A response queue nobody has initialised yet (resp_len == 0) has no
 * room by construction, checked here rather than left to divide by zero.
 */
static bool qlogic_isp_resp_has_room(QLogicISPState *s)
{
    if (s->resp_len == 0) {
        return false;
    }
    return (s->resp_prod + 1) % s->resp_len != s->resp_cons;
}

/*
 * Writes one status entry into the ring and raises the interrupt that tells
 * the host it is there. Unlike a mailbox result, an I/O completion does not
 * take the semaphore lock — the driver's own decode order depends on that,
 * reading the semaphore first and only falling through to bus interface
 * status when it finds the lock clear — so this sets only the status bits,
 * never the semaphore.
 *
 * Only called once the caller has confirmed a slot is free
 * (qlogic_isp_resp_has_room()): this is the write, not the decision of
 * whether it is safe to make.
 *
 * The whole 64-byte slot is zeroed before anything is written into it. This
 * ring is reused, and a slot's previous occupant leaves real bytes behind at
 * every offset this function does not explicitly overwrite. Response length
 * (offset 16, "time" in Linux's own struct and "rsp_len" in OpenBSD's — the
 * two names disagree, but neither driver reads this offset anywhere in
 * either source, so it is written zero along with everything else rather
 * than guessed) and the eight FCP-response/reserved bytes at offset 24 are
 * two fields with no confirmed reader; zeroing the whole slot first covers
 * them, so a future guest that does read one of them sees a defined value
 * rather than a previous command's leftovers, without this function needing
 * to know what that value should be. sense_len, state_flags and status_flags
 * are the fields a confirmed reader exists for — Linux's driver for the first
 * two, IRIX for the status_flags timeout bit — so each is written explicitly.
 */
static void qlogic_isp_post_one_status(QLogicISPState *s, uint32_t handle,
                                       uint16_t scsi_status,
                                       uint16_t completion,
                                       uint16_t state_flags,
                                       uint16_t status_flags, uint32_t resid,
                                       const uint8_t *sense, int sense_len)
{
    PCIDevice *pci = PCI_DEVICE(s);
    hwaddr addr = qlogic_isp_resp_entry_addr(s, s->resp_prod);
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint16_t clamped_sense_len = sense_len > 0
                                  ? MIN(sense_len, ISP_STATUS_SENSE_MAX) : 0;

    /*
     * Built whole, little-endian-canonical, then corrected to the ring's own
     * byte order and written once. Assembling it in a buffer rather than as a
     * run of stores is what lets one rule cover every field, the sub-word ones
     * included, and it also stops the slot ever being visible half-written.
     */
    memset(entry, 0, sizeof(entry));
    entry[ISP_ENTRY_OFF_TYPE] = ISP_ENTRY_TYPE_STATUS;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    stl_le_p(entry + ISP_STATUS_OFF_HANDLE, handle);
    stw_le_p(entry + ISP_STATUS_OFF_SCSI_STATUS, scsi_status);
    stw_le_p(entry + ISP_STATUS_OFF_COMPLETION, completion);
    stw_le_p(entry + ISP_STATUS_OFF_STATE_FLAGS, state_flags);
    stw_le_p(entry + ISP_STATUS_OFF_STATUS_FLAGS, status_flags);
    stw_le_p(entry + ISP_STATUS_OFF_SENSE_LEN, clamped_sense_len);
    stl_le_p(entry + ISP_STATUS_OFF_RESID, resid);
    if (clamped_sense_len > 0) {
        memcpy(entry + ISP_STATUS_OFF_SENSE_DATA, sense, clamped_sense_len);
    }

    pci_dma_write(pci, addr, entry, sizeof(entry));

    s->resp_prod = (s->resp_prod + 1) % s->resp_len;

    s->bus_status |= ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ;
    qlogic_isp_update_irq(s);
}

/*
 * Posts every completion still waiting for room, oldest first, until either
 * none are left or the ring is full again. The only event that can turn a
 * full ring back into one with room is the guest's own acknowledgement, so
 * this is called from the response-queue register's write handler and
 * nowhere else.
 */
static void qlogic_isp_drain_pending_status(QLogicISPState *s)
{
    QLogicISPPendingStatus *pending;

    while (qlogic_isp_resp_has_room(s) &&
           (pending = QTAILQ_FIRST(&s->pending_status)) != NULL) {
        QTAILQ_REMOVE(&s->pending_status, pending, next);
        qlogic_isp_post_one_status(s, pending->handle, pending->scsi_status,
                                   pending->completion, pending->state_flags,
                                   pending->status_flags, pending->resid,
                                   pending->sense_len > 0 ? pending->sense
                                                           : NULL,
                                   pending->sense_len);
        g_free(pending);
    }
}

/*
 * Posts a status entry, unless the response ring has no room for it right
 * now. Real hardware never reaches that case — it completes one command at
 * a time with real bus latency between them — but a model with no such
 * latency can post more completions than the guest has acknowledged, and
 * the datasheet's own requirement is to hold the surplus rather than let a
 * new completion overwrite one the guest has not consumed yet or drop it on
 * the floor: either loses the completion exactly the way this whole bug
 * report started. Held completions post as soon as
 * qlogic_isp_drain_pending_status() sees room again.
 */
static void qlogic_isp_post_status(QLogicISPState *s, uint32_t handle,
                                   uint16_t scsi_status, uint16_t completion,
                                   uint16_t state_flags, uint16_t status_flags,
                                   uint32_t resid,
                                   const uint8_t *sense, int sense_len)
{
    if (s->resp_len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: status posted before the response "
                      "queue was initialised\n");
        return;
    }

    if (!qlogic_isp_resp_has_room(s)) {
        QLogicISPPendingStatus *pending = g_new0(QLogicISPPendingStatus, 1);

        /*
         * Real hardware never reaches this — the datasheet's own words —
         * so a driver that gets here is either far outrunning the
         * acknowledgement pace any real adapter's bus latency would allow,
         * or the guest's own request pacing has a bug. Loud on purpose:
         * this path is unreachable under any driver this model currently
         * boots, and holding the completion rather than dropping it must
         * not also make the condition invisible if it ever does fire.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: response queue full, holding handle "
                      "0x%08" PRIx32 " until the guest acknowledges\n",
                      handle);

        pending->handle = handle;
        pending->scsi_status = scsi_status;
        pending->completion = completion;
        pending->state_flags = state_flags;
        pending->status_flags = status_flags;
        pending->resid = resid;
        pending->sense_len = sense_len > 0
                              ? MIN(sense_len, ISP_STATUS_SENSE_MAX) : 0;
        if (pending->sense_len > 0) {
            memcpy(pending->sense, sense, pending->sense_len);
        }
        QTAILQ_INSERT_TAIL(&s->pending_status, pending, next);
        return;
    }

    qlogic_isp_post_one_status(s, handle, scsi_status, completion,
                               state_flags, status_flags, resid,
                               sense, sense_len);
}

static void qlogic_isp_scsi_complete(SCSIRequest *sreq, size_t resid)
{
    QLogicISPRequest *req = sreq->hba_private;
    QLogicISPState *s = req->dev;
    uint8_t sense[SCSI_SENSE_BUF_SIZE];
    int sense_len;
    uint16_t scsi_status = sreq->status;
    uint16_t completion;

    sense_len = scsi_req_get_sense(sreq, sense, sizeof(sense));
    if (sense_len > 0) {
        scsi_status |= ISP_SCSI_STATUS_SENSE_VALID;
    }

    completion = resid ? ISP_IOCB_DATA_UNDERRUN : ISP_IOCB_COMPLETE;

    /*
     * state_flags is only read by either guest driver when the completion
     * code is incomplete (see the constants' own comment); this path never
     * produces that code, so the exact value here is unobserved either way.
     * The full set of phase bits is written anyway, since it is the true
     * record of what happened -- every phase this model has, this command
     * actually went through -- rather than an arbitrary placeholder chosen
     * because nothing currently reads it. status_flags is likewise a
     * no-error path here: the timeout bit belongs to the missing-target
     * completion, not to a command that reached its target, so it is zero.
     */
    qlogic_isp_post_status(s, req->handle, scsi_status, completion,
                           ISP_STATE_FLAGS_ALL_PHASES, 0, resid, sense,
                           sense_len);

    scsi_req_unref(req->sreq);
    qemu_sglist_destroy(&req->qsg);
    g_free(req);
}

static void qlogic_isp_scsi_cancel(SCSIRequest *sreq)
{
    QLogicISPRequest *req = sreq->hba_private;

    scsi_req_unref(req->sreq);
    qemu_sglist_destroy(&req->qsg);
    g_free(req);
}

/*
 * Handing the SCSI layer this request's already-built scatter-gather list
 * is what makes a data phase happen at all. Without it, every command that
 * carries data — which a bare liveness check like TEST_UNIT_READY never
 * does, and INQUIRY, the very first thing a real driver's target probe
 * issues, always does — reaches scsi_req_data() with no sg list attached,
 * and that function's fallback path is an unconditional call through
 * bus->info->transfer_data(), which this model has never defined. A real
 * OpenBSD boot found this the only way it could: the guest attached qlw0,
 * created its scsibus, and crashed the emulator itself on the first
 * INQUIRY of the first probed target, because nothing before that point in
 * this model's own test suite ever sent a command with a data phase.
 */
static QEMUSGList *qlogic_isp_get_sg_list(SCSIRequest *sreq)
{
    QLogicISPRequest *req = sreq->hba_private;

    return &req->qsg;
}

static const struct SCSIBusInfo qlogic_isp_scsi_info = {
    .tcq = false,
    .max_target = 16,
    .max_lun = 8,
    .get_sg_list = qlogic_isp_get_sg_list,
    .complete = qlogic_isp_scsi_complete,
    .cancel = qlogic_isp_scsi_cancel,
};

/*
 * Reads the segments a command entry carries — four inline, then seven per
 * continuation entry, in the order OpenBSD's own driver writes them, until
 * seg_count segments have been read or the entry_count the header declared
 * is exhausted. A command needing no continuation reads only the inline
 * four and touches nothing past the request entry itself.
 */
static void qlogic_isp_read_segments(QLogicISPState *s, QEMUSGList *qsg,
                                     hwaddr rqst_addr, uint8_t entry_count,
                                     uint16_t seg_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t collected = 0;
    uint16_t cont_index = s->rqst_cons;
    int conts_left = entry_count - 1;
    int i;

    for (i = 0; i < ISP_REQ_SEGS_INLINE && collected < seg_count; i++) {
        hwaddr seg_addr = rqst_addr + ISP_REQ_OFF_SEGS + i * ISP_SEG_SIZE;
        uint32_t addr, len;

        addr = qlogic_isp_desc_ld32(pci, seg_addr);
        len = qlogic_isp_desc_ld32(pci, seg_addr + 4);
        qemu_sglist_add(qsg, addr, len);
        collected++;
    }

    while (collected < seg_count && conts_left > 0) {
        hwaddr cont_addr;

        cont_index = (cont_index + 1) % s->rqst_len;
        cont_addr = qlogic_isp_rqst_entry_addr(s, cont_index);

        for (i = 0; i < ISP_CONT_SEGS && collected < seg_count; i++) {
            hwaddr seg_addr = cont_addr + ISP_CONT_OFF_SEGS + i * ISP_SEG_SIZE;
            uint32_t addr, len;

            addr = qlogic_isp_desc_ld32(pci, seg_addr);
            len = qlogic_isp_desc_ld32(pci, seg_addr + 4);
            qemu_sglist_add(qsg, addr, len);
            collected++;
        }
        conts_left--;
    }
}

/*
 * Dispatches one request entry: finds the target the device field names,
 * builds the scatter-gather list its segments describe, and hands both to
 * QEMU's generic SCSI layer. A target or lun nothing answers to is not this
 * model's error to report loudly — it is exactly what a real adapter would
 * meet issuing the same command to empty hardware — so it is answered with
 * an incomplete status entry rather than logged, the same outcome the
 * datasheet names for a request that cannot be carried out.
 */
static void qlogic_isp_handle_request(QLogicISPState *s, hwaddr addr,
                                      uint8_t entry_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t handle;
    uint16_t device;
    uint8_t target, lun;
    uint16_t cdb_len;
    uint16_t seg_count;
    uint8_t cdb[ISP_REQ_CDB_LEN];
    SCSIDevice *sdev;
    QLogicISPRequest *req;


    handle = qlogic_isp_desc_ld32(pci, addr + ISP_REQ_OFF_HANDLE);
    device = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_DEVICE);
    cdb_len = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_CDBLEN);
    seg_count = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_SEGCOUNT);
    pci_dma_read(pci, addr + ISP_REQ_OFF_CDB, cdb, sizeof(cdb));

    target = ISP_DEVICE_TARGET(device);
    lun = ISP_DEVICE_LUN(device);

    sdev = scsi_device_find(&s->bus, 0, target, lun);
    if (!sdev) {
        /*
         * The one call site in this file that produces an incomplete
         * completion, which is exactly the code the guest drivers read the
         * flag words to interpret. What actually happened here is that this
         * adapter looked for a device at the requested target and lun and
         * found none -- it reached the bus, so ISP_STATE_FLAG_GOT_BUS is
         * true, but got no further, so nothing past it is. Linux's own
         * qla1280_return_status() walks exactly this ladder and reports
         * DID_BAD_TARGET for precisely this state_flags pattern, which is
         * the correct reason and not the plausible-but-wrong one a previous
         * ring slot's leftover bits would have produced.
         *
         * state_flags alone satisfies the Linux, OpenBSD and NetBSD/FreeBSD
         * drivers, which read no further on an incomplete completion. IRIX
         * is stricter: it also checks status_flags and treats a missing
         * target as a benign empty slot only when the selection-timeout bit
         * is set there, faulting the probe otherwise. Both words therefore
         * must tell the same story -- reached the bus, selected nothing,
         * timed out -- so the timeout bit is set here alongside GOT_BUS.
         * This is what lets IRIX walk an ISP1040 bus with empty targets.
         */
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               ISP_STATE_FLAG_GOT_BUS, ISP_STATUS_FLAG_TIMEOUT,
                               0, NULL, 0);
        return;
    }

    req = g_new0(QLogicISPRequest, 1);
    req->dev = s;
    req->handle = handle;
    pci_dma_sglist_init(&req->qsg, pci, seg_count ? seg_count : 1);
    qlogic_isp_read_segments(s, &req->qsg, addr, entry_count, seg_count);

    req->sreq = scsi_req_new(sdev, 0, lun, cdb,
                             MIN(cdb_len, sizeof(cdb)), req);
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }
}

/*
 * The A64 counterpart of qlogic_isp_read_segments(): two inline segments
 * instead of four, five per continuation instead of seven, each one 12
 * bytes instead of 8 because it carries a 64-bit address. The two 32-bit
 * words composing that address are combined low half first, matching
 * qla1280_64bit_start_scsi()'s own write order (confirmed in the datasheet's
 * "64-bit addressing entries" section) -- the same convention this file's
 * A64 mailbox address macros already use for queue-init addresses.
 */
static void qlogic_isp_read_segments_a64(QLogicISPState *s, QEMUSGList *qsg,
                                         hwaddr rqst_addr, uint8_t entry_count,
                                         uint16_t seg_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t collected = 0;
    uint16_t cont_index = s->rqst_cons;
    int conts_left = entry_count - 1;
    int i;

    for (i = 0; i < ISP_A64_REQ_SEGS_INLINE && collected < seg_count; i++) {
        hwaddr seg_addr = rqst_addr + ISP_A64_REQ_OFF_SEGS
                           + i * ISP_A64_SEG_SIZE;
        uint32_t lo, hi, len;

        lo = qlogic_isp_desc_ld32(pci, seg_addr);
        hi = qlogic_isp_desc_ld32(pci, seg_addr + 4);
        len = qlogic_isp_desc_ld32(pci, seg_addr + 8);
        qemu_sglist_add(qsg, lo | ((uint64_t)hi << 32), len);
        collected++;
    }

    while (collected < seg_count && conts_left > 0) {
        hwaddr cont_addr;

        cont_index = (cont_index + 1) % s->rqst_len;
        cont_addr = qlogic_isp_rqst_entry_addr(s, cont_index);

        for (i = 0; i < ISP_A64_CONT_SEGS && collected < seg_count; i++) {
            hwaddr seg_addr = cont_addr + ISP_A64_CONT_OFF_SEGS
                               + i * ISP_A64_SEG_SIZE;
            uint32_t lo, hi, len;

            lo = qlogic_isp_desc_ld32(pci, seg_addr);
            hi = qlogic_isp_desc_ld32(pci, seg_addr + 4);
            len = qlogic_isp_desc_ld32(pci, seg_addr + 8);
            qemu_sglist_add(qsg, lo | ((uint64_t)hi << 32), len);
            collected++;
        }
        conts_left--;
    }
}

/*
 * The A64 counterpart of qlogic_isp_handle_request(): same dispatch, same
 * incomplete-on-no-device answer, and the same header fields at the same
 * offsets (handle, device, cdb_len, seg_count, cdb) -- confirmed
 * byte-identical to the type-0 format through offset 31, so this reuses
 * the type-0 offset constants directly rather than a parallel set that
 * would just repeat the same numbers under new names. Only the segment
 * area past offset 31 needs A64-specific handling.
 */
static void qlogic_isp_handle_request_a64(QLogicISPState *s, hwaddr addr,
                                          uint8_t entry_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t handle;
    uint16_t device;
    uint8_t target, lun;
    uint16_t cdb_len;
    uint16_t seg_count;
    uint8_t cdb[ISP_REQ_CDB_LEN];
    SCSIDevice *sdev;
    QLogicISPRequest *req;


    handle = qlogic_isp_desc_ld32(pci, addr + ISP_REQ_OFF_HANDLE);
    device = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_DEVICE);
    cdb_len = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_CDBLEN);
    seg_count = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_SEGCOUNT);
    pci_dma_read(pci, addr + ISP_REQ_OFF_CDB, cdb, sizeof(cdb));

    target = ISP_DEVICE_TARGET(device);
    lun = ISP_DEVICE_LUN(device);

    sdev = scsi_device_find(&s->bus, 0, target, lun);
    if (!sdev) {
        /*
         * The 64-bit-addressing request path's own missing-target case,
         * identical in meaning to the 32-bit one: the adapter reached the
         * bus and selected nothing. GOT_BUS in state_flags and the
         * selection-timeout bit in status_flags together make an empty
         * target a benign no-device slot for every guest driver, IRIX's
         * stricter status_flags check included.
         */
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               ISP_STATE_FLAG_GOT_BUS, ISP_STATUS_FLAG_TIMEOUT,
                               0, NULL, 0);
        return;
    }

    req = g_new0(QLogicISPRequest, 1);
    req->dev = s;
    req->handle = handle;
    pci_dma_sglist_init(&req->qsg, pci, seg_count ? seg_count : 1);
    qlogic_isp_read_segments_a64(s, &req->qsg, addr, entry_count, seg_count);

    req->sreq = scsi_req_new(sdev, 0, lun, cdb,
                             MIN(cdb_len, sizeof(cdb)), req);
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }
}

/*
 * Walks every new entry between where this device last stopped reading and
 * the request-in index the host just wrote, which is what a write to that
 * register means: process everything posted since the last time. A marker
 * is consumed and produces nothing; a request entry is dispatched and its
 * continuations are skipped over here because reading segments already
 * walked them.
 *
 * The request-in value is whatever the guest last wrote to a plain 16-bit
 * register — nothing here validated it on the way in. A real driver always
 * wraps it modulo the queue length before writing (OpenBSD's own
 * sc_next_req_id does exactly that), so in real traffic it is already in
 * range. But rqst_cons, this device's own read cursor, is *always* kept
 * wrapped, and comparing a wrapped cursor against an unwrapped guest value
 * is a check with a gap: any prod at or above rqst_len — a guest bug, a
 * fuzzer, a malicious guest, or simply a future guest that does not share
 * this wrapping convention — makes rqst_cons != prod true forever, since
 * rqst_cons can never leave [0, rqst_len). That is an infinite loop in the
 * emulator itself, driven entirely by guest-supplied data. Wrapping prod
 * here, the same way rqst_cons is always wrapped, removes the gap instead
 * of adding a bounds check for it: the loop terminates by construction for
 * every possible 16-bit value, not just the ones a well-behaved driver
 * sends.
 */
static void qlogic_isp_process_requests(QLogicISPState *s)
{
    uint16_t prod;

    if (s->rqst_len == 0) {
        return;
    }

    prod = s->mailbox[ISP_MAILBOX_REQUEST_QUEUE] % s->rqst_len;

    while (s->rqst_cons != prod) {
        PCIDevice *pci = PCI_DEVICE(s);
        hwaddr addr = qlogic_isp_rqst_entry_addr(s, s->rqst_cons);
        uint8_t entry_type;
        uint8_t entry_count;


        entry_type = qlogic_isp_desc_ld8(pci, addr + ISP_ENTRY_OFF_TYPE);
        entry_count = qlogic_isp_desc_ld8(pci, addr + ISP_ENTRY_OFF_COUNT);
        if (entry_count == 0) {
            entry_count = 1;
        }

        switch (entry_type) {
        case ISP_ENTRY_TYPE_REQUEST:
            qlogic_isp_handle_request(s, addr, entry_count);
            break;

        /*
         * Linux/IP27's only request format (task #114, datasheet's "64-bit
         * addressing entries" section) -- COMMAND_A64_TYPE is entry type 9.
         * Real hardware and this model agree that its continuation entries
         * (CONTINUE_A64_TYPE, 0xA) are never dispatched independently: the
         * entry_count advance below steps the ring walker past them, the
         * same mechanism that already skips the type-0 pair's continuation
         * entries, and qlogic_isp_read_segments_a64() is what actually
         * visits them.
         */
        case ISP_A64_ENTRY_TYPE_COMMAND:
            qlogic_isp_handle_request_a64(s, addr, entry_count);
            break;

        case ISP_ENTRY_TYPE_MARKER:
            break;

        /*
         * An entry type this model does not implement. Answered incomplete
         * with the handle read at the offset every entry type carrying one
         * uses (offset 4) rather than consumed silently -- the fail-visibly
         * fix from adversarial review task #74/#97 finding #2/#4, which
         * this default case existed to cover before COMMAND_A64_TYPE got a
         * real implementation above. A CONTINUE_A64_TYPE entry never reaches
         * here on its own (see the comment on the command case), so offset 4
         * is safe to read as a handle for whatever unimplemented type does.
         */
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp: unexpected request entry type "
                          "0x%02x at index %u\n", entry_type,
                          s->rqst_cons);
            {
                uint32_t handle;

                handle = qlogic_isp_desc_ld32(
                    pci,
                    addr + ISP_REQ_OFF_HANDLE);
                qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                                       0, 0, 0, NULL, 0);
            }
            break;
        }

        s->rqst_cons = (s->rqst_cons + entry_count) % s->rqst_len;
    }
}

static uint64_t qlogic_isp_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    QLogicISPState *s = opaque;

    /*
     * The response-in value must always be this adapter's own current
     * producer count, never whatever a guest last wrote to this address —
     * the datasheet is explicit that plain storage shared with the guest's
     * ack write reproduces the exact aliasing reserve-one-slot exists to
     * prevent. resp_prod is that count; it is answered directly rather than
     * through s->mailbox[], which this address no longer backs.
     */
    if (addr == ISP_REG_RESPONSE_QUEUE) {
        /*
         * Only once a response queue exists does this address carry a queue
         * pointer. Before that it is a plain mailbox, which is what the
         * firmware's mailbox register test writes a pattern into and reads
         * back -- a test that runs long before any queue is initialised.
         */
        if (s->resp_len == 0) {
            return s->mailbox[ISP_MAILBOX_RESPONSE_QUEUE];
        }
        return s->resp_prod;
    }

    /*
     * The request queue has the identical in-and-out-share-one-address shape
     * as the response queue, with the two directions swapped: the guest is
     * the producer here, and a read must answer this adapter's own consumer
     * position, never the guest's own last write. Linux recomputes its free
     * request-ring space from exactly this read (qla1280_start_scsi(),
     * qla1280.c) as `cnt - ha->req_ring_index`, where ha->req_ring_index is
     * the driver's own locally-tracked producer and cnt is what this read
     * returns -- if a read answered back the guest's own write instead, the
     * two would be equal on every recalculation and the driver would compute
     * a free-space boundary that reflects nothing this adapter has actually
     * consumed. rqst_cons is this adapter's consumer position; answered
     * directly rather than through s->mailbox[], which qlogic_isp_process_
     * requests() still reads for the guest's producer value on the way in.
     *
     * This was the one queue direction the original response-queue fix did
     * not carry over, because that fix was written against the response-
     * queue failure that had actually been observed. It stayed masked here
     * because completions are normally consumed eagerly enough that producer
     * and consumer read as equal regardless of which one this returned;
     * the backpressure that fix introduced -- holding a completion when the
     * response ring has no room -- is what finally makes the two differ
     * often enough for a real guest to notice.
     */
    if (addr == ISP_REG_REQUEST_QUEUE) {
        /* The same gate, and the same reason, as the response queue above. */
        if (s->rqst_len == 0) {
            return s->mailbox[ISP_MAILBOX_REQUEST_QUEUE];
        }
        return s->rqst_cons;
    }

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
    case ISP_REG_NVRAM:
        return s->nvram;
    case ISP_REG_DMA_CMD_CFG:
        return s->dma_cmd_cfg;
    case ISP_REG_DMA_DATA_CFG:
        return s->dma_data_cfg;
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

    /*
     * The guest's write here is its acknowledgement of everything up to the
     * value it read, not a courtesy the adapter can ignore or a value it can
     * store over its own published producer count — the two share this
     * address only because one direction is the read and the other the
     * write. Recorded into resp_cons, never into s->mailbox[], which this
     * address no longer backs. Wrapped defensively the same way the
     * request-in value is, since nothing here can assume a guest write is
     * already a valid ring index.
     */
    if (addr == ISP_REG_RESPONSE_QUEUE) {
        /*
         * With no queue there is no acknowledgement to record and nothing to
         * drain, so the write is what it looks like: a mailbox store. Letting
         * it fall through to the array is what lets the register test read
         * its own pattern back.
         */
        if (s->resp_len == 0) {
            s->mailbox[ISP_MAILBOX_RESPONSE_QUEUE] = val;
            return;
        }
        s->resp_cons = val % s->resp_len;
        qlogic_isp_drain_pending_status(s);
        return;
    }

    if (addr >= ISP_REG_MAILBOX_BASE && addr < ISP_REG_MAILBOX_END) {
        s->mailbox[(addr - ISP_REG_MAILBOX_BASE) / 2] = val;
        /*
         * Writing the request-in pointer is the trigger, not a host command:
         * the datasheet's own steady-state protocol has the host advance
         * this register and the adapter start consuming from it, with
         * nothing else in between.
         */
        if (addr == ISP_REG_REQUEST_QUEUE) {
            qlogic_isp_process_requests(s);
        }
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
     * the order the write itself implies. The interrupt line is recomputed
     * either way, since the enables this register owns are half of what
     * decides it and changing them can raise or lower the line on their own,
     * with nothing else about the write signalling that it should.
     */
    case ISP_REG_BUS_CTRL:
        s->bus_ctrl = val & ISP_BUS_CTRL_WRITE_MASK;
        if (val & ISP_BUS_CTRL_SOFT_RESET) {
            qlogic_isp_reset_state(s);
        }
        qlogic_isp_update_irq(s);
        break;

    case ISP_REG_SEMAPHORE:
        s->semaphore = val & ISP_SEMAPHORE_MASK;
        break;

    /*
     * Data-in is never part of the write mask, so it can never be stored —
     * which is what makes a plain readback of this register always show it
     * clear, without a separate mask on the read path. The guest is only
     * ever writing what it thinks it is shifting out; nothing here shifts
     * anything back.
     */
    case ISP_REG_NVRAM:
        s->nvram = val & ISP_NVRAM_WRITE_MASK;
        break;

    case ISP_REG_DMA_CMD_CFG:
        s->dma_cmd_cfg = val & ISP_DMA_CFG_WRITE_MASK;
        break;

    case ISP_REG_DMA_DATA_CFG:
        s->dma_data_cfg = val & ISP_DMA_CFG_WRITE_MASK;
        break;

    /*
     * Each block's control register, which quiesces the engine and drops its
     * pending interrupt. The SGI firmware's power-on SCSI diagnostic writes
     * both once per adapter and never reads either back, and neither bit names
     * anything this model has: there is no transfer in flight to stop, because
     * every transfer here completes inside the write that started it, and no
     * interrupt of the block's own to clear. So the write is accepted and
     * nothing is kept — recognised rather than left to the unimplemented
     * default, which otherwise reports a register the firmware is entitled to
     * write as a gap in the model on every boot.
     */
    case ISP_REG_DMA_CMD_CTRL:
    case ISP_REG_DMA_DATA_CTRL:
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

    /*
     * PCI Status reset value: config 0x04 must read back Status 0x0200
     * (bit 9, medium DEVSEL) so a 32-bit read at reset returns 0x02000000.
     * The single-CPU IRIX miniroot's io_config_space diagnostic compares
     * this whole dword and fails the I/O slot on a zero Status half. The
     * bit is read-only -- pci_init_wmask leaves wmask[PCI_STATUS] clear and
     * only the error latches are write-one-to-clear -- so it survives the
     * reset that clears the writable and w1c bits. See
     * docs/datasheets/isp1040.md.
     */
    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);

    memory_region_init_io(&s->io, OBJECT(s), &qlogic_isp_reg_ops, s,
                          "qlogic-isp-io", QLOGIC_ISP_REG_WINDOW_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(s), &qlogic_isp_reg_ops, s,
                          "qlogic-isp-mmio", QLOGIC_ISP_REG_WINDOW_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    QTAILQ_INIT(&s->pending_status);

    scsi_bus_init(&s->bus, sizeof(s->bus), DEVICE(dev),
                 &qlogic_isp_scsi_info);
}

/*
 * Version 7 adds resp_cons: before this fix the response-queue register was
 * plain storage shared with the guest's ack write, so nothing tracked the
 * guest's consumer position separately and there was no field for it to
 * migrate. A stream from an older version has no value to give it; it is
 * accepted anyway, at 0, because a freshly-migrated guest that has not yet
 * acknowledged anything is exactly what resp_cons == 0 already means, and
 * demanding version 7 to migrate this device at all would be a much larger
 * cost for the same outcome.
 *
 * Pending completions queued for lack of ring room (QLogicISPPendingStatus)
 * are not migrated. A migration landing while that list is non-empty loses
 * them — the same gap this device already has for every other in-flight
 * SCSI request, and the condition that fills the list is one the datasheet
 * itself says real hardware never reaches.
 */
static const VMStateDescription vmstate_qlogic_isp = {
    .name = "qlogic-isp",
    .version_id = 8,
    .minimum_version_id = 6,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QLogicISPState),
        VMSTATE_UINT16(bus_cfg1, QLogicISPState),
        VMSTATE_UINT16(bus_ctrl, QLogicISPState),
        VMSTATE_UINT16(bus_status, QLogicISPState),
        VMSTATE_UINT16(semaphore, QLogicISPState),
        VMSTATE_UINT16(nvram, QLogicISPState),
        VMSTATE_UINT16(risc_flags, QLogicISPState),
        VMSTATE_UINT16_V(dma_cmd_cfg, QLogicISPState, 8),
        VMSTATE_UINT16_V(dma_data_cfg, QLogicISPState, 8),
        VMSTATE_UINT16_ARRAY(mailbox, QLogicISPState, ISP_MAILBOX_COUNT),
        VMSTATE_UINT16_ARRAY(risc_bank, QLogicISPState, ISP_SXP_RISC_COUNT),
        VMSTATE_UINT16_ARRAY(sxp_bank, QLogicISPState, ISP_SXP_RISC_COUNT),
        VMSTATE_UINT16_ARRAY(risc_ram, QLogicISPState, ISP_RISC_RAM_WORDS),
        VMSTATE_UINT64(rqst_base, QLogicISPState),
        VMSTATE_UINT64(resp_base, QLogicISPState),
        VMSTATE_UINT16(rqst_len, QLogicISPState),
        VMSTATE_UINT16(resp_len, QLogicISPState),
        VMSTATE_UINT16(rqst_cons, QLogicISPState),
        VMSTATE_UINT16(resp_prod, QLogicISPState),
        VMSTATE_UINT16_V(resp_cons, QLogicISPState, 7),
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
