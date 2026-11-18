/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SGI SHub 1: the registers the reviewed chips pages define or the PROM's
 * reset path needs. Anything else stops the VM rather than answering.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/ia64/sgi-shub.h"
#include "hw/ia64/sgi-shub-regs.h"
#include "hw/ia64/sgi-shub-ii-regs.h"

typedef enum {
    K_STORE,            /* plain 64-bit storage in a slot */
    K_ELECT_SET,        /* write-only side of the election word */
    K_IPI_INT,
    K_RTC_INT_CONFIG,
    K_RTC_INT_ENABLE,
    K_EVENT,
    K_EVENT_ALIAS,
    K_SHUB_ID,
    K_PTC,
    K_INT_CMP,
    K_RTC,
    K_RTC_CTL,
    K_PI_CAM_CONTROL,
    K_PIO_WRITE_STATUS,
    K_PIO_WRITE_STATUS_ALIAS,
    K_LED,
    K_CHIP_ID,
    K_WRITE_ONLY,       /* writes accepted and ignored, reads stop */
    K_MBIST_CTL,
    K_CLK_SEL,
    K_BTE_LEN,
    K_BTE_CTL,
    K_SLICE_REC,
    K_SLICE_PRESENT,
    K_LOCK,
    K_LOCK_STATUS,
    K_LOCK2,
    K_LOCK2_STATUS,
} ShubKind;

/* The II block is reached at global offset 0x1800000 plus its register. */
#define II_REG(r) (0x1800000ULL + (r))

typedef struct {
    ShubKind kind;
    uint64_t base;
    unsigned count;
    uint64_t stride;
    int slot;           /* K_STORE: first slot, one per instance */
} ShubRegSpec;

static const ShubRegSpec shub_regs[] = {
    { K_STORE, SHUB_MD_DQLP_DIR_PRIVILEGE_VECTOR, 1, 0, SHUB_SLOT_PRIV0 },
    { K_STORE, SHUB_MD_DQRP_DIR_PRIVILEGE_VECTOR, 1, 0, SHUB_SLOT_PRIV1 },
    { K_STORE, SHUB_IPI_ACCESS, 1, 0, SHUB_SLOT_IPI_ACCESS },
    { K_STORE, SHUB_CPU_HIST, SHUB_CPU_HIST_COUNT, SHUB_CPU_HIST_STRIDE,
      SHUB_SLOT_HIST },
    { K_STORE, SHUB_FW_WORD_1540, 1, 0, SHUB_SLOT_FW_1540 },
    { K_STORE, SHUB_CPU_BITS, 1, 0, SHUB_SLOT_CPU_BITS },
    { K_STORE, SHUB_FW_DEBUG_WORD, 1, 0, SHUB_SLOT_FW_DEBUG },
    { K_STORE, SHUB_PI_REG_2001_0100, 1, 0, SHUB_SLOT_PI_2001_0100 },
    { K_STORE, SHUB_LB_L1_STATE, 1, 0, SHUB_SLOT_L1_STATE },
    { K_STORE, SHUB_LB_ELECT, 1, 0, SHUB_SLOT_ELECT },
    /* A write here shows up in a read of the election register. */
    { K_ELECT_SET, SHUB_LB_ELECT_SET, 1, 0, SHUB_SLOT_ELECT },
    { K_RTC_CTL, SHUB_RTC_CTL, 1, 0, SHUB_SLOT_RTC_CTL },
    { K_IPI_INT, SHUB_IPI_INT, 1, 0, 0 },
    { K_RTC_INT_CONFIG, SHUB_RTC_INT_CONFIG, SHUB_RTC_INT_CONFIG_COUNT,
      SHUB_RTC_INT_CONFIG_STRIDE, 0 },
    { K_RTC_INT_ENABLE, SHUB_RTC_INT_ENABLE, SHUB_RTC_INT_ENABLE_COUNT,
      SHUB_RTC_INT_ENABLE_STRIDE, 0 },
    { K_EVENT, SHUB_EVENT_OCCURRED, 1, 0, 0 },
    { K_EVENT_ALIAS, SHUB_EVENT_OCCURRED_ALIAS, 1, 0, 0 },
    { K_SHUB_ID, SHUB_SHUB_ID, 1, 0, 0 },
    { K_PTC, SHUB_PTC, SHUB_PTC_COUNT, SHUB_PTC_STRIDE, 0 },
    { K_INT_CMP, SHUB_INT_CMP, SHUB_INT_CMP_COUNT, SHUB_INT_CMP_STRIDE, 0 },
    { K_RTC, SHUB_RTC, 1, 0, 0 },
    { K_PI_CAM_CONTROL, SHUB_PI_CAM_CONTROL, 1, 0, 0 },
    { K_PIO_WRITE_STATUS, SHUB_PIO_WRITE_STATUS, SHUB_PIO_WRITE_STATUS_COUNT,
      SHUB_PIO_WRITE_STATUS_STRIDE, 0 },
    { K_PIO_WRITE_STATUS_ALIAS, SHUB_PIO_WRITE_STATUS_ALIAS,
      SHUB_PIO_WRITE_STATUS_ALIAS_COUNT, SHUB_PIO_WRITE_STATUS_ALIAS_STRIDE,
      0 },
    { K_LED, SHUB_JUNK_BUS_LED, SHUB_JUNK_BUS_LED_COUNT,
      SHUB_JUNK_BUS_LED_STRIDE, 0 },
    { K_CHIP_ID, SHUB_CHIP_ID, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_LB_REG_1002_0000, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0003_0400, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0005_0400, 1, 0, 0 },
    { K_STORE, SHUB_MD_REG_0001_0068, 1, 0, SHUB_SLOT_MD_0068 },
    /* Memory bring-up: devices/shub/memory-init.md in the chips repository. */
    { K_STORE, SHUB_MD_BANK_CFG0, 1, 0, SHUB_SLOT_MD_BANK_CFG },
    { K_STORE, SHUB_MD_BANK_CFG1, 1, 0, SHUB_SLOT_MD_BANK_CFG + 1 },
    { K_STORE, SHUB_MD_BANK_CFG2, 1, 0, SHUB_SLOT_MD_BANK_CFG + 2 },
    { K_STORE, SHUB_MD_TIMING_2, 1, 0, SHUB_SLOT_MD_TIMING_2 },
    { K_STORE, SHUB_MD_TIMING_3, 1, 0, SHUB_SLOT_MD_TIMING_3 },
    { K_STORE, SHUB_MD_REFRESH_1, 1, 0, SHUB_SLOT_MD_REFRESH_1 },
    { K_STORE, SHUB_MD_REG_0003_0000, 1, 0, SHUB_SLOT_MD_0003_0000 },
    { K_STORE, SHUB_MD_REG_0005_0000, 1, 0, SHUB_SLOT_MD_0005_0000 },
    { K_STORE, SHUB_MD_MBIST_STATUS, 1, 0, SHUB_SLOT_MBIST_STATUS },
    { K_STORE, SHUB_MD_MBIST_CMD, 1, 0, SHUB_SLOT_MBIST_CMD },
    { K_STORE, SHUB_MD_MBIST_MASK, 1, 0, SHUB_SLOT_MBIST_MASK },
    { K_MBIST_CTL, SHUB_MD_MBIST_CTL, 1, 0, SHUB_SLOT_MBIST_CTL },
    { K_WRITE_ONLY, SHUB_MD_TIMING_0, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_TIMING_1, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REFRESH_0, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_CONST_58, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_CTL_60, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_CONST_78, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0003_0200, SHUB_MD_REG_0003_0200_COUNT,
      SHUB_MD_REG_0003_0200_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_REG_0005_0200, SHUB_MD_REG_0005_0200_COUNT,
      SHUB_MD_REG_0005_0200_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_BIST_DATA_A0, SHUB_MD_BIST_DATA_A0_COUNT,
      SHUB_MD_BIST_DATA_A0_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_BIST_DATA_B0, SHUB_MD_BIST_DATA_B0_COUNT,
      SHUB_MD_BIST_DATA_B0_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_BIST_DATA_A1, SHUB_MD_BIST_DATA_A1_COUNT,
      SHUB_MD_BIST_DATA_A1_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_BIST_DATA_B1, SHUB_MD_BIST_DATA_B1_COUNT,
      SHUB_MD_BIST_DATA_B1_STRIDE, 0 },
    { K_WRITE_ONLY, SHUB_MD_DIR_ACCESS_LP, 1, 0, 0 },
    { K_WRITE_ONLY, SHUB_MD_DIR_ACCESS_RP, 1, 0, 0 },
    { K_CLK_SEL, SHUB_MD_CLK_SEL, 1, 0, 0 },
    /* The two block transfer engines of the I/O interface. */
    { K_BTE_LEN, II_REG(SHUB_II_BTE_LENGTH_STATUS),
      SHUB_II_BTE_LENGTH_STATUS_COUNT, SHUB_II_BTE_LENGTH_STATUS_STRIDE,
      SHUB_SLOT_BTE_LEN },
    { K_STORE, II_REG(SHUB_II_BTE_SOURCE), SHUB_II_BTE_SOURCE_COUNT,
      SHUB_II_BTE_SOURCE_STRIDE, SHUB_SLOT_BTE_SRC },
    { K_STORE, II_REG(SHUB_II_BTE_DESTINATION), SHUB_II_BTE_DESTINATION_COUNT,
      SHUB_II_BTE_DESTINATION_STRIDE, SHUB_SLOT_BTE_DST },
    { K_BTE_CTL, II_REG(SHUB_II_BTE_CONTROL), SHUB_II_BTE_CONTROL_COUNT,
      SHUB_II_BTE_CONTROL_STRIDE, SHUB_SLOT_BTE_CTL },
    { K_STORE, II_REG(SHUB_II_BTE_NOTIFY), SHUB_II_BTE_NOTIFY_COUNT,
      SHUB_II_BTE_NOTIFY_STRIDE, SHUB_SLOT_BTE_NOTIFY },
    { K_STORE, II_REG(SHUB_II_BTE_INTERRUPT), SHUB_II_BTE_INTERRUPT_COUNT,
      SHUB_II_BTE_INTERRUPT_STRIDE, SHUB_SLOT_BTE_INT },
    { K_SLICE_REC, SHUB_LB_SLICE_REC, SHUB_LB_SLICE_REC_COUNT,
      SHUB_LB_SLICE_REC_STRIDE, 0 },
    { K_SLICE_PRESENT, SHUB_PI_SLICE_PRESENT, 1, 0, 0 },
    { K_LOCK, SHUB_LB_LOCK, 1, 0, 0 },
    { K_LOCK_STATUS, SHUB_LB_LOCK_STATUS, 1, 0, 0 },
    { K_LOCK2, SHUB_LB_LOCK2, 1, 0, 0 },
    { K_LOCK2_STATUS, SHUB_LB_LOCK2_STATUS, 1, 0, 0 },
};

#define MBIST_GO (1ULL << 40)
#define MBIST_FAIL (1ULL << 41)
#define MBIST_OP(v) (((v) >> 32) & 0x7f)

/* Part number the PROM's method table needs; a wrong one calls NULL. */
#define SHUB_CHIP_ID_PART 0x4363ULL

#define SHUB_ID_WRITABLE \
    ((SHUB_SHUB_ID_NASID_MASK << SHUB_SHUB_ID_NASID_SHIFT) | \
     (SHUB_SHUB_ID_SIZE_CODE_MASK << SHUB_SHUB_ID_SIZE_CODE_SHIFT) | \
     (SHUB_SHUB_ID_NODES_PER_BIT_MASK << SHUB_SHUB_ID_NODES_PER_BIT_SHIFT))

#define SHUB_RTC_PERIOD_FIELD(ctl) \
    (((ctl) >> SHUB_RTC_CTL_PERIOD_SHIFT) & SHUB_RTC_CTL_PERIOD_MASK)

typedef struct ShubDecoded {
    const ShubRegSpec *spec;
    unsigned index;
} ShubDecoded;

/* Offset to register and index, built once; the first matching spec wins. */
static GHashTable *shub_decode_table;

static void shub_decode_table_build(void)
{
    shub_decode_table = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              g_free, g_free);
    for (unsigned i = 0; i < ARRAY_SIZE(shub_regs); i++) {
        const ShubRegSpec *spec = &shub_regs[i];

        for (unsigned n = 0; n < spec->count; n++) {
            int64_t k = spec->base + n * spec->stride;
            int64_t *key;
            ShubDecoded *d;

            if (g_hash_table_contains(shub_decode_table, &k)) {
                continue;
            }
            key = g_new(int64_t, 1);
            d = g_new(ShubDecoded, 1);
            *key = k;
            *d = (ShubDecoded){ spec, n };
            g_hash_table_insert(shub_decode_table, key, d);
        }
    }
}

static const ShubRegSpec *shub_decode(hwaddr offset, unsigned *index)
{
    int64_t key;
    const ShubDecoded *d;

    /* The local window reaches the II block 0x800000 lower than the global. */
    if (offset >= 0x1400000 && offset < 0x1440000) {
        offset += 0x800000;
    }
    key = offset;
    d = g_hash_table_lookup(shub_decode_table, &key);
    if (!d) {
        return NULL;
    }
    *index = d->index;
    return d->spec;
}

/*
 * An access the model does not implement: log it and fail the transaction.
 * @r is MEMTX_DECODE_ERROR when no register is there, else MEMTX_ERROR.
 */
static MemTxResult shub_refuse(hwaddr offset, unsigned size, bool write,
                               uint64_t value, const char *why, MemTxResult r)
{
    qemu_log_mask(LOG_UNIMP, "sgi-shub: unsupported %s at 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 ": %s\n",
                  write ? "write" : "read", offset, size, value, why);
    return r;
}

static MemTxResult shub_unmodelled(hwaddr offset, unsigned size, bool write,
                                   uint64_t value, const char *why)
{
    return shub_refuse(offset, size, write, value, why, MEMTX_ERROR);
}

static unsigned shub_rtc_period_ns(SGIShubState *s)
{
    return 10 * (SHUB_RTC_PERIOD_FIELD(s->slot[SHUB_SLOT_RTC_CTL].value) + 1);
}

/* The counter at the current rate. Callers check the control word first. */
static uint64_t shub_rtc_count(SGIShubState *s)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->rtc_base_ns;

    return (s->rtc_base_count + ns / shub_rtc_period_ns(s)) & SHUB_RTC_MASK;
}

static void shub_global_remap(SGIShubState *s)
{
    uint64_t nasid = (s->shub_id >> SHUB_SHUB_ID_NASID_SHIFT) &
                     SHUB_SHUB_ID_NASID_MASK;

    memory_region_set_address(&s->global_mmr, SGI_SHUB_GLOBAL_MMR_BASE +
                              (nasid << SGI_SHUB_NASID_SHIFT));
}

static MemTxResult shub_read(void *opaque, hwaddr offset, uint64_t *data,
                             unsigned size, MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    const ShubRegSpec *spec;
    unsigned n;

    *data = 0;
    spec = shub_decode(offset, &n);
    if (!spec) {
        return shub_refuse(offset, size, false, 0, "register not modelled",
                           MEMTX_DECODE_ERROR);
    }
    if (spec->kind == K_LED) {
        return shub_unmodelled(offset, size, false, 0,
                               "register is not readable");
    }
    if (size != 8) {
        return shub_unmodelled(offset, size, false, 0,
                               "register is 64 bits wide");
    }

    switch (spec->kind) {
    case K_STORE:
    case K_RTC_CTL:
    case K_MBIST_CTL:
    case K_BTE_LEN: {
        SGIShubSlot *slot = &s->slot[spec->slot + n];

        if (!slot->valid) {
            return shub_unmodelled(offset, size, false, 0,
                                   "reset value unknown and not yet written");
        }
        *data = slot->value;
        return MEMTX_OK;
    }
    case K_RTC_INT_CONFIG:
        *data = s->rtc_int_config[n];
        return MEMTX_OK;
    case K_RTC_INT_ENABLE:
        /* Enabling is refused on write, so the enable is always clear. */
        *data = 0;
        return MEMTX_OK;
    case K_INT_CMP:
        *data = s->int_cmp[n];
        return MEMTX_OK;
    case K_SHUB_ID:
        *data = s->shub_id;
        return MEMTX_OK;
    case K_CLK_SEL:
        /* Bits 1:0 pick the bus clock; 0 is the simplest supported one. */
        *data = 0;
        return MEMTX_OK;
    case K_RTC:
        if (!s->slot[SHUB_SLOT_RTC_CTL].valid) {
            return shub_unmodelled(offset, size, false, 0,
                                   "tick period not programmed yet");
        }
        *data = shub_rtc_count(s);
        return MEMTX_OK;
    case K_PIO_WRITE_STATUS:
        /* PIO writes complete synchronously, so the count is always idle. */
        *data = SHUB_PIO_WRITE_STATUS_IDLE;
        return MEMTX_OK;
    case K_CHIP_ID:
        *data = (SHUB_CHIP_ID_PART << SHUB_CHIP_ID_PART_NUMBER_SHIFT) |
                (((s->shub_id >> SHUB_SHUB_ID_REVISION_SHIFT) &
                  SHUB_SHUB_ID_REVISION_MASK) << SHUB_CHIP_ID_REVISION_SHIFT);
        return MEMTX_OK;
    case K_SLICE_REC:
        /* Any value satisfies the PROM; it only becomes a progress code. */
        *data = 0;
        return MEMTX_OK;
    case K_SLICE_PRESENT:
        /* One processor, in slice 0. */
        *data = 1;
        return MEMTX_OK;
    case K_LOCK_STATUS:
    case K_LOCK2_STATUS:
        /* Always free: one processor never contends for the lock. */
        *data = 0;
        return MEMTX_OK;
    case K_EVENT:
        return shub_unmodelled(offset, size, false, 0,
                               "reset value and event sources not modelled");
    default:
        return shub_unmodelled(offset, size, false, 0,
                               "register is not readable");
    }
}

static MemTxResult shub_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    SGIShubState *s = opaque;
    const ShubRegSpec *spec;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned n;

    spec = shub_decode(offset, &n);
    if (!spec) {
        return shub_refuse(offset, size, true, value, "register not modelled",
                           MEMTX_DECODE_ERROR);
    }
    if (size != (spec->kind == K_LED ? 2 : 8)) {
        return shub_unmodelled(offset, size, true, value,
                               "wrong access width");
    }

    switch (spec->kind) {
    case K_ELECT_SET:
        /* The alias takes a value only while SH_SCRATCH1 reads 0. */
        if (s->slot[spec->slot + n].value) {
            return MEMTX_OK;
        }
        /* fall through */
    case K_STORE:
        s->slot[spec->slot + n].value = value;
        s->slot[spec->slot + n].valid = true;
        return MEMTX_OK;
    case K_MBIST_CTL: {
        uint64_t op = MBIST_OP(value);

        /* Only the operations the PROM issues; the rest have no contract. */
        if ((value & MBIST_GO) && op != 0 && op != 1 && op != 2 && op != 7 &&
            op != 16 && op != 48 && op != 64) {
            return shub_unmodelled(offset, size, true, value,
                                   "engine operation not in the PROM's list");
        }
        /*
         * Every run completes at once and passes: go and fail read clear.
         * No DRAM is modelled, so a compare cannot fail.
         */
        s->slot[spec->slot].value = value & ~(MBIST_GO | MBIST_FAIL);
        s->slot[spec->slot].valid = true;
        return MEMTX_OK;
    }
    case K_BTE_LEN:
        /* A write loads the length and sets busy. */
        s->slot[spec->slot + n].value =
            (value & SHUB_II_BTE_LENGTH_STATUS_LENGTH_MASK) |
            (SHUB_II_BTE_LENGTH_STATUS_BUSY_MASK <<
             SHUB_II_BTE_LENGTH_STATUS_BUSY_SHIFT);
        s->slot[spec->slot + n].valid = true;
        return MEMTX_OK;
    case K_BTE_CTL: {
        const uint64_t zerofill = SHUB_II_BTE_CONTROL_ZEROFILL_INPLACE;
        const uint64_t notify = SHUB_II_BTE_CONTROL_NOTIFY_INPLACE;

        /* Only a zero fill, which RAM that starts zeroed already satisfies. */
        if ((value & ~(zerofill | notify)) || !(value & zerofill)) {
            return shub_unmodelled(offset, size, true, value,
                                   "block transfer mode not modelled");
        }
        s->slot[spec->slot + n].value = value;
        s->slot[spec->slot + n].valid = true;
        /* RAM starts zeroed, so a fill writes nothing and ends at once. */
        s->slot[SHUB_SLOT_BTE_LEN + n].value = 0;
        return MEMTX_OK;
    }
    case K_RTC_CTL: {
        SGIShubSlot *ctl = &s->slot[SHUB_SLOT_RTC_CTL];

        /* Keep the count continuous across a period change. */
        if (ctl->valid) {
            s->rtc_base_count = shub_rtc_count(s);
        }
        ctl->value = value;
        ctl->valid = true;
        s->rtc_base_ns = now;
        return MEMTX_OK;
    }
    case K_RTC:
        if (value & ~SHUB_RTC_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits above the 55-bit counter");
        }
        s->rtc_base_count = value;
        s->rtc_base_ns = now;
        return MEMTX_OK;
    case K_SHUB_ID:
        /* The PROM rewrites only the NASID, size code and nodes-per-bit. */
        if ((value ^ s->shub_id) & ~SHUB_ID_WRITABLE) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the NASID, size and node fields");
        }
        s->shub_id = value;
        shub_global_remap(s);
        return MEMTX_OK;
    case K_RTC_INT_CONFIG:
        if (value & ~SHUB_RTC_CONFIG_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the defined fields");
        }
        s->rtc_int_config[n] = value;
        return MEMTX_OK;
    case K_RTC_INT_ENABLE:
        if (value & SHUB_RTC_ENABLE_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "comparator interrupt delivery not modelled");
        }
        if (value) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the enable field");
        }
        return MEMTX_OK;
    case K_INT_CMP:
        if (value & ~SHUB_INT_CMP_COMPARE_MASK) {
            return shub_unmodelled(offset, size, true, value,
                                   "bits outside the compare field");
        }
        s->int_cmp[n] = value;
        return MEMTX_OK;
    case K_PI_CAM_CONTROL:
        /* Only the value Linux writes is known; its effect is not modelled. */
        if (value != SHUB_PI_CAM_WAR_VALUE) {
            return shub_unmodelled(offset, size, true, value,
                                   "value not known");
        }
        qemu_log_mask(LOG_UNIMP, "sgi-shub: pi_cam_control write ignored: "
                      "CRB CAM not modelled\n");
        return MEMTX_OK;
    case K_PIO_WRITE_STATUS_ALIAS:
        /* Only the kernel's clear value is known; nothing is pending. */
        if (value != SHUB_PIO_WRITE_STATUS_ALIAS_CLEAR) {
            return shub_unmodelled(offset, size, true, value,
                                   "value not known");
        }
        return MEMTX_OK;
    case K_LED:
        s->led[n] = value;
        return MEMTX_OK;
    case K_WRITE_ONLY:
        /* The PROM never reads these; the effect of the write is unknown. */
        qemu_log_mask(LOG_UNIMP, "sgi-shub: write at 0x%" HWADDR_PRIx
                      " ignored: effect unknown\n", offset);
        return MEMTX_OK;
    case K_LOCK:
    case K_LOCK2:
        /* Release only: the lock is never held, so freeing it is a no-op. */
        if (value) {
            return shub_unmodelled(offset, size, true, value,
                                   "value not known");
        }
        return MEMTX_OK;
    case K_IPI_INT:
        return shub_unmodelled(offset, size, true, value,
                               "interprocessor interrupt delivery not modelled");
    case K_PTC:
        return shub_unmodelled(offset, size, true, value,
                               "purge translation cache effect unknown");
    case K_EVENT_ALIAS:
        return shub_unmodelled(offset, size, true, value,
                               "event clearing not modelled");
    default:
        return shub_unmodelled(offset, size, true, value,
                               "register is not writable");
    }
}

static const MemoryRegionOps shub_ops = {
    .read_with_attrs = shub_read,
    .write_with_attrs = shub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void shub_reset_hold(Object *obj, ResetType type)
{
    SGIShubState *s = SGI_SHUB(obj);

    s->shub_id = s->shub_id_reset;
    shub_global_remap(s);
    s->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rtc_base_count = 0;
    memset(s->slot, 0, sizeof(s->slot));
    /*
     * Registers the PROM reads before it writes and accepts any value from:
     * their real reset value is unknown, and zero is an approximation.
     */
    for (int i = SHUB_SLOT_HIST; i < SHUB_SLOT_COUNT; i++) {
        s->slot[i].valid = true;
    }
    /* The kernel header's reset value for these families is zero. */
    memset(s->rtc_int_config, 0, sizeof(s->rtc_int_config));
    memset(s->int_cmp, 0, sizeof(s->int_cmp));
    memset(s->led, 0, sizeof(s->led));
}

static void shub_realize(DeviceState *dev, Error **errp)
{
    SGIShubState *s = SGI_SHUB(dev);

    if (s->shub_id_reset & ~(SHUB_ID_WRITABLE |
        (SHUB_SHUB_ID_REVISION_MASK << SHUB_SHUB_ID_REVISION_SHIFT))) {
        error_setg(errp, "shub-id sets bits the model cannot qualify");
    }
}

static void shub_init(Object *obj)
{
    SGIShubState *s = SGI_SHUB(obj);

    memory_region_init_io(&s->local_mmr, obj, &shub_ops, s,
                          "sgi-shub.local-mmr", SGI_SHUB_MMR_SIZE);
    memory_region_init_io(&s->global_mmr, obj, &shub_ops, s,
                          "sgi-shub.global-mmr", SGI_SHUB_MMR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->local_mmr);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->global_mmr);
}

static const Property shub_properties[] = {
    DEFINE_PROP_UINT64("shub-id", SGIShubState, shub_id_reset,
                       SGI_SHUB_ID_DEFAULT),
};

static int shub_post_load(void *opaque, int version_id)
{
    shub_global_remap(opaque);
    return 0;
}

static const VMStateDescription vmstate_shub_slot = {
    .name = "sgi-shub/slot",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(value, SGIShubSlot),
        VMSTATE_BOOL(valid, SGIShubSlot),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription shub_vmstate = {
    .name = "sgi-shub",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = shub_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(shub_id, SGIShubState),
        VMSTATE_INT64(rtc_base_ns, SGIShubState),
        VMSTATE_UINT64(rtc_base_count, SGIShubState),
        VMSTATE_STRUCT_ARRAY(slot, SGIShubState, SHUB_SLOT_COUNT, 1,
                             vmstate_shub_slot, SGIShubSlot),
        VMSTATE_UINT64_ARRAY(rtc_int_config, SGIShubState, 3),
        VMSTATE_UINT64_ARRAY(int_cmp, SGIShubState, 3),
        VMSTATE_UINT16_ARRAY(led, SGIShubState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static void shub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    shub_decode_table_build();
    dc->desc = "SGI SHub 1 node controller (local and global MMR windows)";
    dc->realize = shub_realize;
    dc->vmsd = &shub_vmstate;
    device_class_set_props(dc, shub_properties);
    rc->phases.hold = shub_reset_hold;
}

static const TypeInfo shub_type = {
    .name = TYPE_SGI_SHUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIShubState),
    .instance_init = shub_init,
    .class_init = shub_class_init,
};

static void shub_register_types(void)
{
    type_register_static(&shub_type);
}

type_init(shub_register_types)
