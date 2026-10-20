/* SPDX-License-Identifier: BSD-3-Clause */
/* Bridge qtest PCI device for DMA, IRQ levels and programmable identity. */
/*
 * Seeded DMA crosses pages; IRQ level is held. Like pci-testdev, no machine
 * instantiates it. An extended iommu-testdev could replace it.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "system/dma.h"
#include "system/ram_addr.h"
#include "system/qtest.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci-64bit.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_SGI_BRIDGE_DMA_TEST "sgi-bridge-dma-test"
/* The same card as a 64-bit PCI agent. */
#define TYPE_SGI_BRIDGE_DMA_TEST_64BIT "sgi-bridge-dma-test-64bit"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBridgeDMATestState, SGI_BRIDGE_DMA_TEST)

/* Large enough for page-straddling and synchronous DMA benchmarks. */
#define SGI_BRIDGE_DMA_TEST_MAX 65536

/* The allocated Red Hat test-device identity. */
#define SGI_BRIDGE_DMA_TEST_VENDOR PCI_VENDOR_ID_REDHAT
#define SGI_BRIDGE_DMA_TEST_DEVICE PCI_DEVICE_ID_REDHAT_TEST

struct SGIBridgeDMATestState {
    PCIDevice parent_obj;

    MemoryRegion mmio;

    /*
     * A second BAR of 64-bit registers — a read-only constant and a writable
     * scratch — put deliberately behind the Bridge's programmed-access
     * correction, so a test can exercise a 64-bit device window: served one
     * eight-byte cycle, mirrored whole on the slot's swap bit. A fixed value
     * keeps the assertion about the Bridge's byte handling, not this device's
     * storage.
     */
    MemoryRegion wide_mmio;
    uint64_t wide_scratch;

    /*
     * A third BAR in PCI I/O space: one byte-addressable doubleword, so a
     * test can tell which byte of I/O space a bridge's I/O path reached.
     */
    MemoryRegion io;
    uint64_t io_scratch;
    uint32_t lane_scratch;

    /* 64-bit so a test can exercise the Bridge's 64-bit direct decode. */
    uint64_t address;
    uint32_t length;
    uint32_t seed;

    /* A copy tests the read direction without a second transform. */
    uint64_t source;

    /* The level this device is holding its interrupt pin at. */
    uint32_t interrupt;

    /* The identity this device presents, which a test may override. */
    uint16_t vendor_id;
    uint16_t device_id;

    bool dma_r32;
    bool pci_only_latched;
    bool ever_realized;
    MemoryRegion dma_view;
    AddressSpace dma_view_as;
    MemoryRegion *dma_memory;
    AddressSpace linked_dma_as;
    bool memory_only;
    bool fault_read;
    bool fault_write;
    uint32_t bar_reads;
    uint32_t bar_writes;
    uint32_t fault_result;
    uint32_t result;
    uint64_t read_data;
    uint32_t repetitions;

    /* Host-only observation of one translated RAM range; never migrated. */
    MemoryRegion *dirty_region;
    hwaddr dirty_offset;
    hwaddr dirty_length;

    /* Settable: the four-onto-eight pin fold needs more than one pin. */
    uint8_t interrupt_pin;

    /* REQ64# as sampled at the end of reset; only the 64-bit type sees it. */
    bool req64;
};

#define REG_ADDRESS     0x00
#define REG_LENGTH      0x04
#define REG_SEED        0x08
#define REG_GO          0x0c
/*
 * Drives and holds the pin at the level written: a pulse could not tell a
 * status register that follows from one that latches.
 */
#define REG_INTERRUPT   0x10
#define REG_SOURCE      0x14
/* Address high halves; the window is reached with 32-bit accesses. */
#define REG_ADDRESS_HI  0x18
#define REG_SOURCE_HI   0x1c
/* Little-endian words put 12 34 56 78 on PCI lanes 31:24 through 7:0. */
#define REG_LANE_FIXED  0x20
#define REG_LANE_SCRATCH 0x24
#define REG_RESULT      0x28
#define REG_READ_DATA   0x2c
#define REG_READ_DATA_HI 0x30
/* Write 1 to observe/clear this range, 0 to stop; reads consume its bitmap. */
#define REG_DIRTY       0x34
/* Read-only: 1 if REQ64# was asserted at the end of the last reset. */
#define REG_REQ64       0x38
#define REG_FAULT_READ  0x3c
#define REG_WINDOW      0x40   /* size of the register block */
#define IO_WINDOW       0x08   /* size of the I/O-space scratch */

/* What a write to REG_GO asks for. */
#define GO_FILL         1      /* write the seeded pattern at REG_ADDRESS */
#define GO_COPY         2      /* read REG_SOURCE, write REG_ADDRESS */

/*
 * The second BAR's registers. The fixed value is asymmetric end to end so a
 * byte-lane-neutral constant cannot hide a wrong swap.
 */
#define WIDE_FIXED      0x00   /* read-only constant */
#define WIDE_SCRATCH    0x08   /* read/write doubleword */
#define WIDE_WINDOW     0x10   /* size of the 64-bit register block */
#define WIDE_FIXED_VALUE 0x0123456789abcdefULL

/* A pattern a test can predict without this file telling it. */
static uint8_t sgi_bridge_dma_test_byte(uint32_t seed, unsigned int i)
{
    return (uint8_t)(seed + i);
}

static AddressSpace *sgi_bridge_dma_test_as(SGIBridgeDMATestState *s)
{
    return s->dma_memory ? &s->linked_dma_as :
                          pci_get_address_space(PCI_DEVICE(s));
}

static void sgi_bridge_dma_test_dirty_stop(SGIBridgeDMATestState *s)
{
    if (s->dirty_region) {
        memory_region_set_log(s->dirty_region, false, DIRTY_MEMORY_VGA);
        memory_region_unref(s->dirty_region);
        s->dirty_region = NULL;
    }
}

static void sgi_bridge_dma_test_dirty_start(SGIBridgeDMATestState *s)
{
    AddressSpace *as = sgi_bridge_dma_test_as(s);
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    MemoryRegion *mr;
    hwaddr offset, length = s->length;

    sgi_bridge_dma_test_dirty_stop(s);
    if (!length || length > SGI_BRIDGE_DMA_TEST_MAX ||
        s->address > UINT64_MAX - (length - 1)) {
        return;
    }
    attrs.memory = s->memory_only;
    rcu_read_lock();
    mr = address_space_translate(as, s->address, &offset, &length, true, attrs);
    /* Clearing another display client's bitmap would corrupt its state. */
    if (!memory_region_is_ram(mr) || memory_region_is_rom(mr) ||
        length < s->length ||
        memory_region_is_logging(mr, DIRTY_MEMORY_VGA)) {
        rcu_read_unlock();
        return;
    }
    memory_region_ref(mr);
    rcu_read_unlock();
    s->dirty_region = mr;
    s->dirty_offset = offset;
    s->dirty_length = s->length;
    memory_region_set_log(mr, true, DIRTY_MEMORY_VGA);
    memory_region_reset_dirty(mr, offset, s->dirty_length, DIRTY_MEMORY_VGA);
}

static uint32_t sgi_bridge_dma_test_dirty_read(SGIBridgeDMATestState *s)
{
    g_autofree DirtyBitmapSnapshot *snapshot = NULL;

    if (!s->dirty_region) {
        return UINT32_MAX;
    }
    /* A client that joined later owns the same bitmap; leave it untouched. */
    if (s->dirty_region->vga_logging_count != 1) {
        sgi_bridge_dma_test_dirty_stop(s);
        return UINT32_MAX;
    }
    snapshot = memory_region_snapshot_and_clear_dirty(s->dirty_region,
        s->dirty_offset, s->dirty_length, DIRTY_MEMORY_VGA);
    return memory_region_snapshot_get_dirty(s->dirty_region, snapshot,
        s->dirty_offset, s->dirty_length);
}

static int sgi_bridge_dma_test_dirty_migration(void *opaque)
{
    sgi_bridge_dma_test_dirty_stop(opaque);
    return 0;
}

static MemTxResult dma_view_read(void *opaque, hwaddr addr, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    SGIBridgeDMATestState *s = opaque;
    uint8_t bytes[4] = { 0 };
    MemTxResult result;

    assert(size <= sizeof(bytes));
    result = address_space_read(sgi_bridge_dma_test_as(s),
                                addr ^ (4 - size), attrs, bytes, size);
    *value = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult dma_view_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIBridgeDMATestState *s = opaque;
    uint8_t bytes[4];

    assert(size <= sizeof(bytes));
    stn_be_p(bytes, size, value);
    return address_space_write(sgi_bridge_dma_test_as(s),
                               addr ^ (4 - size), attrs, bytes, size);
}

static const MemoryRegionOps dma_view_ops = {
    .read_with_attrs = dma_view_read,
    .write_with_attrs = dma_view_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static MemTxResult sgi_bridge_dma_test_transfer(SGIBridgeDMATestState *s,
                                               hwaddr addr, void *buf,
                                               bool write)
{
    AddressSpace *as = s->dma_r32 ? &s->dma_view_as :
                       sgi_bridge_dma_test_as(s);
    DMADirection dir = write ? DMA_DIRECTION_FROM_DEVICE :
                              DMA_DIRECTION_TO_DEVICE;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    attrs.memory = s->memory_only;
    dma_barrier(as, dir);
    return address_space_rw(as, addr, attrs, buf, s->length, write);
}

static void sgi_bridge_dma_test_go(SGIBridgeDMATestState *s)
{
    g_autofree uint8_t *buf = g_malloc(SGI_BRIDGE_DMA_TEST_MAX);
    unsigned int i;

    if (s->length > SGI_BRIDGE_DMA_TEST_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: asked for %u bytes, which is more "
                      "than this device will transfer\n", s->length);
        return;
    }

    for (i = 0; i < s->length; i++) {
        buf[i] = sgi_bridge_dma_test_byte(s->seed, i);
    }

    /* One call for the whole transfer: the per-page distinction lives here. */
    for (i = 0; i < s->repetitions; i++) {
        s->result = sgi_bridge_dma_test_transfer(s, s->address, buf, true);
    }
    s->read_data = 0;
    memcpy(&s->read_data, buf, MIN(s->length, sizeof(s->read_data)));
    s->read_data = le64_to_cpu(s->read_data);
}

/*
 * With different settings at the two ends exactly one transform sits in the
 * path, so the read direction is assertable on its own.
 */
static void sgi_bridge_dma_test_copy(SGIBridgeDMATestState *s)
{
    g_autofree uint8_t *buf = g_malloc(SGI_BRIDGE_DMA_TEST_MAX);
    MemTxResult result;

    if (s->length > SGI_BRIDGE_DMA_TEST_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: asked to copy %u bytes, which is "
                      "more than this device will transfer\n", s->length);
        return;
    }

    memset(buf, 0xa5, SGI_BRIDGE_DMA_TEST_MAX);
    result = sgi_bridge_dma_test_transfer(s, s->source, buf, false);
    s->result = result;
    s->read_data = ldq_le_p(buf);
    /* A read that failed has no data to copy, so nothing is written. */
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: the copy's read of 0x%" PRIx64
                      " failed, so it writes nothing\n", s->source);
        return;
    }
    s->result = sgi_bridge_dma_test_transfer(s, s->address, buf, true);
}

static void sgi_bridge_dma_test_command(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);
    uint32_t command;

    if (!visit_type_uint32(v, name, &command, errp)) {
        return;
    }
    switch (command) {
    case GO_FILL:
        sgi_bridge_dma_test_go(s);
        break;
    case GO_COPY:
        sgi_bridge_dma_test_copy(s);
        break;
    default:
        error_setg(errp, "unknown DMA test command %u", command);
    }
}

static bool sgi_bridge_dma_test_master_get(Object *obj, Error **errp)
{
    PCIDevice *dev = PCI_DEVICE(obj);

    return !!(pci_default_read_config(dev, PCI_COMMAND, 2) &
              PCI_COMMAND_MASTER);
}

static void sgi_bridge_dma_test_master_set(Object *obj, bool enabled,
                                           Error **errp)
{
    PCIDevice *dev = PCI_DEVICE(obj);
    uint16_t command;

    if (!qtest_enabled()) {
        error_setg(errp, "test-master-enable requires qtest");
        return;
    }
    if (!qdev_is_realized(DEVICE(obj)) ||
        !memory_region_is_mapped(&dev->bus_master_enable_region)) {
        error_setg(errp, "test-master-enable requires machine initialization");
        return;
    }
    command = pci_default_read_config(dev, PCI_COMMAND, 2);
    command = enabled ? command | PCI_COMMAND_MASTER :
                        command & ~PCI_COMMAND_MASTER;
    pci_default_write_config(dev, PCI_COMMAND, command, 2);
}

static bool sgi_bridge_dma_test_r32_get(Object *obj, Error **errp)
{
    return SGI_BRIDGE_DMA_TEST(obj)->dma_r32;
}

static void sgi_bridge_dma_test_r32_set(Object *obj, bool value, Error **errp)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);

    if (DEVICE(obj)->realized) {
        qdev_prop_set_after_realize(DEVICE(obj), "dma-r32", errp);
    } else if (s->pci_only_latched) {
        error_setg(errp, "fixture PCI-only DMA route is locked");
    } else {
        s->dma_r32 = value;
    }
}

static void sgi_bridge_dma_test_memory_set_check(const Object *obj,
                                                  const char *name,
                                                  Object *value,
                                                  Error **errp)
{
    const SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);

    if (DEVICE(obj)->realized) {
        qdev_prop_allow_set_link_before_realize(obj, name, value, errp);
    } else if (s->pci_only_latched) {
        error_setg(errp, "fixture PCI-only DMA route is locked");
    }
}

static bool sgi_bridge_dma_test_latch_get(Object *obj, Error **errp)
{
    return SGI_BRIDGE_DMA_TEST(obj)->pci_only_latched;
}

static void sgi_bridge_dma_test_latch_set(Object *obj, bool value,
                                           Error **errp)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);

    if (s->pci_only_latched) {
        error_setg(errp, "fixture PCI-only DMA route is locked");
    } else if (DEVICE(obj)->realized) {
        qdev_prop_set_after_realize(DEVICE(obj), "pci-only-latch", errp);
    } else if (value) {
        if (s->dma_r32 || s->dma_memory) {
            error_setg(errp, "fixture PCI-only DMA route needs both bypass "
                             "properties disabled");
        } else {
            s->pci_only_latched = true;
        }
    }
}

static void sgi_bridge_dma_test_init(Object *obj)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);

    object_property_add_bool(obj, "dma-r32", sgi_bridge_dma_test_r32_get,
                             sgi_bridge_dma_test_r32_set);
    object_property_add_link(obj, "dma-memory", TYPE_MEMORY_REGION,
                             (Object **)&s->dma_memory,
                             sgi_bridge_dma_test_memory_set_check,
                             OBJ_PROP_LINK_STRONG);
    object_property_add_bool(obj, "pci-only-latch",
                             sgi_bridge_dma_test_latch_get,
                             sgi_bridge_dma_test_latch_set);
    object_property_add_uint64_ptr(obj, "test-address", &s->address,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "test-source", &s->source,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "test-length", &s->length,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "test-seed", &s->seed,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "test-result", &s->result,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "test-read-data", &s->read_data,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "test-command", "uint32", NULL,
                        sgi_bridge_dma_test_command, NULL, NULL);
    object_property_add_bool(obj, "test-master-enable",
                             sgi_bridge_dma_test_master_get,
                             sgi_bridge_dma_test_master_set);
    object_property_add_uint32_ptr(obj, "x-test-bar-read-count",
                                   &s->bar_reads, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-bar-write-count",
                                   &s->bar_writes, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-fault-result",
                                   &s->fault_result, OBJ_PROP_FLAG_READ);
}

static uint64_t sgi_bridge_dma_test_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    switch (addr) {
    case REG_ADDRESS:
        return (uint32_t)s->address;
    case REG_ADDRESS_HI:
        return s->address >> 32;
    case REG_LENGTH:
        return s->length;
    case REG_SEED:
        return s->seed;
    case REG_SOURCE:
        return (uint32_t)s->source;
    case REG_SOURCE_HI:
        return s->source >> 32;
    case REG_INTERRUPT:
        return s->interrupt;
    case REG_RESULT:
        return s->result;
    case REG_READ_DATA:
        return (uint32_t)s->read_data;
    case REG_READ_DATA_HI:
        return s->read_data >> 32;
    case REG_DIRTY:
        return sgi_bridge_dma_test_dirty_read(s);
    case REG_REQ64:
        return s->req64;
    case REG_LANE_FIXED:
        return 0x12345678;
    case REG_LANE_SCRATCH:
        return s->lane_scratch;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: read of undefined register 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static MemTxResult sgi_bridge_dma_test_read_attrs(void *opaque,
    hwaddr addr, uint64_t *value, unsigned size, MemTxAttrs attrs)
{
    SGIBridgeDMATestState *s = opaque;

    if (s->fault_read) {
        s->bar_reads++;
    }
    if (s->fault_read && addr == REG_FAULT_READ) {
        s->fault_result = MEMTX_ERROR;
        *value = 0;
        return MEMTX_ERROR;
    }
    *value = sgi_bridge_dma_test_read(opaque, addr, size);
    return MEMTX_OK;
}

static void sgi_bridge_dma_test_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    switch (addr) {
    case REG_ADDRESS:
        s->address = deposit64(s->address, 0, 32, val);
        break;
    case REG_ADDRESS_HI:
        s->address = deposit64(s->address, 32, 32, val);
        break;
    case REG_LENGTH:
        s->length = val;
        break;
    case REG_SEED:
        s->seed = val;
        break;
    case REG_DIRTY:
        if (val == 1) {
            sgi_bridge_dma_test_dirty_start(s);
        } else {
            sgi_bridge_dma_test_dirty_stop(s);
        }
        break;
    case REG_GO:
        if (val == GO_COPY) {
            sgi_bridge_dma_test_copy(s);
        } else {
            sgi_bridge_dma_test_go(s);
        }
        break;
    case REG_SOURCE:
        s->source = deposit64(s->source, 0, 32, val);
        break;
    case REG_SOURCE_HI:
        s->source = deposit64(s->source, 32, 32, val);
        break;
    case REG_INTERRUPT:
        s->interrupt = val ? 1 : 0;
        pci_set_irq(PCI_DEVICE(s), s->interrupt);
        break;
    case REG_LANE_SCRATCH:
        s->lane_scratch = val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: write of undefined register 0x%"
                      HWADDR_PRIx "\n", addr);
        break;
    }
}

static MemTxResult sgi_bridge_dma_test_write_attrs(void *opaque,
    hwaddr addr, uint64_t value, unsigned size, MemTxAttrs attrs)
{
    SGIBridgeDMATestState *s = opaque;

    if (s->fault_write) {
        s->bar_writes++;
    }
    if (s->fault_write && addr == REG_FAULT_READ) {
        s->fault_result = MEMTX_ERROR;
        return MEMTX_ERROR;
    }
    sgi_bridge_dma_test_write(opaque, addr, value, size);
    return MEMTX_OK;
}

/* 32-bit accesses only; both bounds declared so the core enforces it. */
static const MemoryRegionOps sgi_bridge_dma_test_ops = {
    .read_with_attrs = sgi_bridge_dma_test_read_attrs,
    .write_with_attrs = sgi_bridge_dma_test_write_attrs,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The 64-bit register window: a read-only constant and a writable scratch,
 * each returning the size-byte little-endian slice at the accessed offset, so
 * a split read reassembles the same value a one-cycle read gives. What
 * differs is the Bridge's whole-doubleword swap, which is what a test reads
 * to see. The eight-byte impl size lets a one-cycle doubleword reach the
 * handler whole.
 */
static uint64_t sgi_bridge_dma_test_wide_read(void *opaque, hwaddr addr,
                                              unsigned size)
{
    SGIBridgeDMATestState *s = opaque;
    uint64_t reg = addr < WIDE_SCRATCH ? WIDE_FIXED_VALUE : s->wide_scratch;

    return extract64(reg, (addr & 7) * 8, size * 8);
}

static void sgi_bridge_dma_test_wide_write(void *opaque, hwaddr addr,
                                           uint64_t val, unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    if (addr < WIDE_SCRATCH) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge-dma-test: write to the read-only 64-bit "
                      "register 0x%" HWADDR_PRIx "\n", addr);
        return;
    }
    s->wide_scratch = deposit64(s->wide_scratch, (addr & 7) * 8, size * 8, val);
}

/*
 * Down to a byte, because the constant's eight bytes all differ: a narrow
 * read through a device window says which byte of the register it reached,
 * which is what makes the bridge's byte-lane addressing assertable. A wide
 * access still arrives whole, so the doubleword tests are unaffected.
 */
static const MemoryRegionOps sgi_bridge_dma_test_wide_ops = {
    .read = sgi_bridge_dma_test_wide_read,
    .write = sgi_bridge_dma_test_wide_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static uint64_t sgi_bridge_dma_test_io_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    return extract64(s->io_scratch, (addr & 7) * 8, size * 8);
}

static void sgi_bridge_dma_test_io_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    SGIBridgeDMATestState *s = opaque;

    s->io_scratch = deposit64(s->io_scratch, (addr & 7) * 8, size * 8, val);
}

/* Down to a byte for the same reason as the wide registers. */
static const MemoryRegionOps sgi_bridge_dma_test_io_ops = {
    .read = sgi_bridge_dma_test_io_read,
    .write = sgi_bridge_dma_test_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void sgi_bridge_dma_test_irq_in(void *opaque, int n, int level)
{
    SGIBridgeDMATestState *s = opaque;

    s->interrupt = !!level;
    pci_set_irq(PCI_DEVICE(s), s->interrupt);
}

static void sgi_bridge_dma_test_realize(PCIDevice *dev, Error **errp)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(dev);

    if (s->pci_only_latched && s->ever_realized) {
        error_setg(errp, "fixture PCI-only DMA route cannot be realized "
                         "again");
        return;
    }
    qdev_init_gpio_in_named(DEVICE(dev), sgi_bridge_dma_test_irq_in,
                            "test-irq", 1);

    if (s->dma_memory) {
        address_space_init(&s->linked_dma_as, s->dma_memory,
                            "sgi-bridge-dma-test.linked");
    }
    if (s->dma_r32) {
        memory_region_init_io(&s->dma_view, OBJECT(s), &dma_view_ops, s,
                              "sgi-bridge-dma-test.r32", UINT64_MAX);
        s->dma_view.disable_reentrancy_guard = true;
        address_space_init(&s->dma_view_as, &s->dma_view,
                            "sgi-bridge-dma-test.r32");
    }

    /* Say in configuration space which pin this device uses. */
    pci_config_set_interrupt_pin(dev->config, s->interrupt_pin);

    /* The claimed identity; a test overrides it to stand in for a part. */
    pci_config_set_vendor_id(dev->config, s->vendor_id);
    pci_config_set_device_id(dev->config, s->device_id);

    memory_region_init_io(&s->mmio, OBJECT(s), &sgi_bridge_dma_test_ops, s,
                          "sgi-bridge-dma-test.mmio", REG_WINDOW);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    memory_region_init_io(&s->wide_mmio, OBJECT(s),
                          &sgi_bridge_dma_test_wide_ops, s,
                          "sgi-bridge-dma-test.wide", WIDE_WINDOW);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->wide_mmio);

    memory_region_init_io(&s->io, OBJECT(s), &sgi_bridge_dma_test_io_ops, s,
                          "sgi-bridge-dma-test.io", IO_WINDOW);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    s->ever_realized = true;
}

static void sgi_bridge_dma_test_exit(PCIDevice *dev)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(dev);

    sgi_bridge_dma_test_dirty_stop(s);

    if (s->dma_r32) {
        address_space_destroy(&s->dma_view_as);
    }
    if (s->dma_memory) {
        address_space_destroy(&s->linked_dma_as);
    }
}

static void sgi_bridge_dma_test_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeDMATestState *s = SGI_BRIDGE_DMA_TEST(obj);
    PCIDevice *dev = PCI_DEVICE(s);

    if (memory_region_is_mapped(&dev->bus_master_enable_region)) {
        uint16_t command = pci_default_read_config(dev, PCI_COMMAND, 2);

        pci_default_write_config(dev, PCI_COMMAND,
                                 command & ~PCI_COMMAND_MASTER, 2);
    }

    sgi_bridge_dma_test_dirty_stop(s);

    s->address = 0;
    s->length = 0;
    s->seed = 0;
    s->source = 0;
    s->wide_scratch = 0;
    s->io_scratch = 0;
    s->lane_scratch = 0;
    s->interrupt = 0;
    s->result = MEMTX_OK;
    s->read_data = 0;
    s->req64 = false;
    pci_set_irq(PCI_DEVICE(s), 0);
}

static void sgi_bridge_dma_test_req64_sampled(PCIDevice *dev, bool asserted)
{
    SGI_BRIDGE_DMA_TEST(dev)->req64 = asserted;
}

static const VMStateDescription vmstate_sgi_bridge_dma_test = {
    .name = "sgi-bridge-dma-test",
    .version_id = 8,
    .minimum_version_id = 8,
    .pre_save = sgi_bridge_dma_test_dirty_migration,
    .pre_load = sgi_bridge_dma_test_dirty_migration,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIBridgeDMATestState),
        VMSTATE_UINT64(address, SGIBridgeDMATestState),
        VMSTATE_UINT64(source, SGIBridgeDMATestState),
        VMSTATE_UINT64(wide_scratch, SGIBridgeDMATestState),
        VMSTATE_UINT64(io_scratch, SGIBridgeDMATestState),
        VMSTATE_UINT32(lane_scratch, SGIBridgeDMATestState),
        VMSTATE_UINT32(length, SGIBridgeDMATestState),
        VMSTATE_UINT32(seed, SGIBridgeDMATestState),
        VMSTATE_UINT32(interrupt, SGIBridgeDMATestState),
        VMSTATE_UINT32(result, SGIBridgeDMATestState),
        VMSTATE_UINT64(read_data, SGIBridgeDMATestState),
        VMSTATE_BOOL(req64, SGIBridgeDMATestState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_bridge_dma_test_properties[] = {
    DEFINE_PROP_UINT8("interrupt-pin", SGIBridgeDMATestState, interrupt_pin, 1),
    /* The part a test stands this card in for is named by its identity. */
    DEFINE_PROP_UINT16("vendor-id", SGIBridgeDMATestState, vendor_id,
                       SGI_BRIDGE_DMA_TEST_VENDOR),
    DEFINE_PROP_UINT16("device-id", SGIBridgeDMATestState, device_id,
                       SGI_BRIDGE_DMA_TEST_DEVICE),
    DEFINE_PROP_UINT32("repetitions", SGIBridgeDMATestState, repetitions, 1),
    DEFINE_PROP_BOOL("memory-only", SGIBridgeDMATestState, memory_only, false),
    DEFINE_PROP_BOOL("x-test-fault-read", SGIBridgeDMATestState,
                     fault_read, false),
    DEFINE_PROP_BOOL("x-test-fault-write", SGIBridgeDMATestState,
                     fault_write, false),
};

static void sgi_bridge_dma_test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    k->realize = sgi_bridge_dma_test_realize;
    k->exit = sgi_bridge_dma_test_exit;
    k->vendor_id = SGI_BRIDGE_DMA_TEST_VENDOR;
    k->device_id = SGI_BRIDGE_DMA_TEST_DEVICE;
    k->class_id = PCI_CLASS_OTHERS;
    dc->desc = "transfer source for testing a host bridge's translation";
    dc->vmsd = &vmstate_sgi_bridge_dma_test;
    rc->phases.hold = sgi_bridge_dma_test_reset_hold;
    device_class_set_props(dc, sgi_bridge_dma_test_properties);
    /* A command line may ask for it; no machine builds one. */
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void sgi_bridge_dma_test_64bit_class_init(ObjectClass *oc,
                                                 const void *data)
{
    PCI64BitDeviceClass *pc = PCI_64BIT_DEVICE_CLASS(oc);

    DEVICE_CLASS(oc)->desc = "64-bit transfer source for testing a host "
                             "bridge's translation";
    pc->req64_sampled = sgi_bridge_dma_test_req64_sampled;
}

#define TYPE_SGI_MACE_PIO_TEST "sgi-mace-pio-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMacePIOTestState, SGI_MACE_PIO_TEST)

struct SGIMacePIOTestState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint8_t bytes[0x100];
    uint32_t last_offset;
    uint32_t last_size;
    uint32_t read_count;
    uint32_t write_count;
};

static uint64_t sgi_mace_pio_test_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    SGIMacePIOTestState *s = opaque;

    s->last_offset = offset;
    s->last_size = size;
    s->read_count++;
    return size == 1 ? s->bytes[offset] : 0;
}

static void sgi_mace_pio_test_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    SGIMacePIOTestState *s = opaque;

    s->last_offset = offset;
    s->last_size = size;
    s->write_count++;
    if (size == 1) {
        s->bytes[offset] = value;
    }
}

static const MemoryRegionOps sgi_mace_pio_test_ops = {
    .read = sgi_mace_pio_test_read,
    .write = sgi_mace_pio_test_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void sgi_mace_pio_test_realize(PCIDevice *dev, Error **errp)
{
    SGIMacePIOTestState *s = SGI_MACE_PIO_TEST(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &sgi_mace_pio_test_ops, s,
                          "sgi-mace-pio-test.mmio", sizeof(s->bytes));
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void sgi_mace_pio_test_init(Object *obj)
{
    SGIMacePIOTestState *s = SGI_MACE_PIO_TEST(obj);

    for (unsigned i = 0; i < 4; i++) {
        s->bytes[i] = 0x10 + i;
        s->bytes[0x84 + i] = 0x84 + i;
    }
    object_property_add_uint32_ptr(obj, "test-pio-last-offset",
                                   &s->last_offset, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "test-pio-last-size",
                                   &s->last_size, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "test-pio-read-count",
                                   &s->read_count, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "test-pio-write-count",
                                   &s->write_count, OBJ_PROP_FLAG_READ);
}

static void sgi_mace_pio_test_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    pc->realize = sgi_mace_pio_test_realize;
    pc->vendor_id = PCI_VENDOR_ID_REDHAT;
    pc->device_id = PCI_DEVICE_ID_REDHAT_TEST;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "byte-addressable PCI BAR for MACE PIO tests";
    dc->user_creatable = true;
}

static const TypeInfo sgi_bridge_dma_test_types[] = {
    {
        .name = TYPE_SGI_BRIDGE_DMA_TEST,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIBridgeDMATestState),
        .instance_init = sgi_bridge_dma_test_init,
        .class_init = sgi_bridge_dma_test_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }, {
        .name = TYPE_SGI_BRIDGE_DMA_TEST_64BIT,
        .parent = TYPE_SGI_BRIDGE_DMA_TEST,
        .class_init = sgi_bridge_dma_test_64bit_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_PCI_64BIT_DEVICE },
            { },
        },
    }, {
        .name = TYPE_SGI_MACE_PIO_TEST,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIMacePIOTestState),
        .instance_init = sgi_mace_pio_test_init,
        .class_init = sgi_mace_pio_test_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(sgi_bridge_dma_test_types)
