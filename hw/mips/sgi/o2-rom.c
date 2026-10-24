/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MACE's 4 MiB system ROM decode over an explicitly selected diagnostic
 * image. Capacity, image placement and padding describe a test selection;
 * they do not identify an O2's fitted flash part or its programming layout.
 */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "hw/mips/sgi/o2-rom.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "qemu/error-report.h"
#include "hw/core/resettable.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "trace/control.h"
#include "trace.h"

#define O2_ROM_WINDOW (4 * MiB)
#define O2_ROM_MIN_SIZE (512 * KiB)
#define O2_ROM_MAX_SIZE (2 * MiB)
#define O2_ROM_MAX_ALIASES (O2_ROM_WINDOW / O2_ROM_MIN_SIZE)
#define AT29_SIZE (512 * KiB)
#define AT29_STATE_SIZE 512
#define AT29_SECTOR_SIZE 256
#define AT29_BOOT_BLOCK_SIZE (16 * KiB)
#define AT29_LOAD_NS (150 * SCALE_US)
#define AT29_CYCLE_NS (10 * SCALE_MS)

typedef enum AT29Phase {
    AT29_IDLE,
    AT29_PREFIX_AA,
    AT29_PREFIX_55,
    AT29_PREFIX_READY,
    AT29_LOADING,
    AT29_BUSY,
} AT29Phase;

struct SGIO2ROMState {
    SysBusDevice parent_obj;

    MemoryRegion window;
    MemoryRegion storage;
    MemoryRegion aliases[O2_ROM_MAX_ALIASES - 1];
    uint32_t flash_size;
    uint32_t image_offset;
    uint16_t pad_byte;
    char *image;
    BlockBackend *blk;
    BlockBackend *state_blk;
    Error *backing_migration_blocker;
    bool backing_failed;
    bool write_enabled;
    bool mace_controlled;
    bool at29_diagnostic;
    bool at29_protected;
    bool at29_lower_lock;
    bool at29_upper_lock;
    bool at29_program_allowed;
    bool at29_enable_protection;
    bool at29_poll_toggle;
    char *at29_protection_setting;
    char *at29_lower_lock_setting;
    char *at29_upper_lock_setting;
    Error *at29_migration_blocker;
    QEMUTimer *at29_timer;
    AT29Phase at29_phase;
    int64_t at29_deadline_ns;
    uint32_t at29_sector;
    uint32_t at29_last_address;
    uint8_t at29_last_data;
    uint8_t at29_load[AT29_SECTOR_SIZE];
    uint64_t at29_loaded[AT29_SECTOR_SIZE / 64];
    unsigned at29_loaded_count;
    uint64_t at29_trace_load_attempts;
    uint64_t at29_trace_poll_reads;
    bool at29_trace_busy_write_seen;
};

void sgi_o2_rom_set_write_enable(SGIO2ROMState *rom, bool enabled)
{
    rom->write_enabled = enabled;
}

static bool o2_rom_get_write_enable(Object *obj, Error **errp)
{
    return SGI_O2_ROM(obj)->write_enabled;
}

static void o2_rom_set_write_enable(Object *obj, bool enabled, Error **errp)
{
    SGIO2ROMState *s = SGI_O2_ROM(obj);

    if (s->mace_controlled) {
        error_setg(errp, "sgi-o2-rom write-enable is driven by MACE");
        return;
    }
    sgi_o2_rom_set_write_enable(s, enabled);
}

static void o2_rom_flash_write_enable(void *opaque, int n, int level)
{
    sgi_o2_rom_set_write_enable(SGI_O2_ROM(opaque), level);
}

static bool at29_block_migration(SGIO2ROMState *s)
{
    Error *err = NULL;

    if (s->at29_migration_blocker) {
        return true;
    }
    error_setg(&s->at29_migration_blocker,
               "O2 AT29C040A write sequence is active");
    if (migrate_add_blocker(&s->at29_migration_blocker, &err) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-rom: AT29C040A write during migration\n");
        error_free(err);
        migrate_del_blocker(&s->at29_migration_blocker);
        return false;
    }
    return true;
}

static void at29_idle(SGIO2ROMState *s)
{
    timer_del(s->at29_timer);
    if (s->at29_phase == AT29_BUSY) {
        memory_region_rom_device_set_romd(&s->storage, true);
    }
    s->at29_phase = AT29_IDLE;
    s->at29_loaded_count = 0;
    migrate_del_blocker(&s->at29_migration_blocker);
}

static void at29_schedule(SGIO2ROMState *s, int64_t deadline)
{
    s->at29_deadline_ns = deadline;
    timer_mod(s->at29_timer, deadline);
}

static uint32_t at29_trace_sector(SGIO2ROMState *s)
{
    if (s->at29_phase < AT29_LOADING) {
        return UINT32_MAX;
    }
    if (s->at29_phase == AT29_BUSY && !s->at29_program_allowed) {
        return s->at29_last_address & ~(AT29_SECTOR_SIZE - 1);
    }
    return s->at29_sector;
}

static void at29_trace_timeout(SGIO2ROMState *s, int64_t now,
                               bool timer_callback, const char *outcome)
{
    if (trace_event_get_state_backends(TRACE_O2_AT29_TIMEOUT)) {
        uint32_t sector = at29_trace_sector(s);
        uint32_t offset = sector == UINT32_MAX ? UINT32_MAX :
                          s->at29_last_address;

        trace_o2_at29_timeout(offset, s->at29_phase, sector,
                              s->at29_loaded_count, now,
                              s->at29_deadline_ns, timer_callback, outcome);
    }
}

static void at29_state_bytes(const uint8_t *array, uint8_t flags,
                              uint8_t state[AT29_STATE_SIZE])
{
    static const uint8_t magic[] = { 'O', '2', 'A', 'T', '2', '9', 0, 1 };
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize length = 32;

    memset(state, 0, AT29_STATE_SIZE);
    memcpy(state, magic, sizeof(magic));
    state[8] = flags;
    g_checksum_update(sum, array, AT29_SIZE);
    g_checksum_get_digest(sum, state + 32, &length);
    g_checksum_free(sum);
}

static bool at29_backend_check(BlockBackend *blk, int64_t size,
                                const char *what, Error **errp)
{
    int64_t length = blk_getlength(blk);

    if (length < 0) {
        error_setg_errno(errp, -length, "sgi-o2-rom: reading %s size", what);
        return false;
    }
    if (length != size) {
        error_setg(errp, "sgi-o2-rom: %s requires exactly %" PRId64
                   " bytes, got %" PRId64, what, size, length);
        return false;
    }
    if (!blk_supports_write_perm(blk)) {
        error_setg(errp, "sgi-o2-rom: %s must be writable", what);
        return false;
    }
    if (blk_get_flags(blk) & (BDRV_O_NO_FLUSH | BDRV_O_SNAPSHOT |
                              BDRV_O_TEMPORARY)) {
        error_setg(errp, "sgi-o2-rom: %s requires durable persistent writes",
                   what);
        return false;
    }
    if (blk_set_perm(blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                      0, errp) < 0) {
        return false;
    }
    blk_set_on_error(blk, BLOCKDEV_ON_ERROR_STOP, BLOCKDEV_ON_ERROR_STOP);
    blk_iostatus_enable(blk);
    return true;
}

static bool at29_restore_backing(SGIO2ROMState *s, uint8_t *array, Error **errp)
{
    uint8_t state[AT29_STATE_SIZE], expected[AT29_STATE_SIZE];
    int ret;

    if (!at29_backend_check(s->blk, AT29_SIZE, "array backend", errp) ||
        !at29_backend_check(s->state_blk, AT29_STATE_SIZE,
                            "state backend", errp)) {
        return false;
    }
    ret = blk_pread(s->blk, 0, AT29_SIZE, array, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "sgi-o2-rom: reading array backend");
        return false;
    }
    ret = blk_pread(s->state_blk, 0, sizeof(state), state, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "sgi-o2-rom: reading state backend");
        return false;
    }
    at29_state_bytes(array, state[8] & 7, expected);
    if (memcmp(state, expected, sizeof(state))) {
        error_setg(errp, "sgi-o2-rom: invalid backing state or array digest");
        return false;
    }
    s->at29_protected = state[8] & 1;
    s->at29_lower_lock = state[8] & 2;
    s->at29_upper_lock = state[8] & 4;
    error_setg(&s->backing_migration_blocker,
               "O2 persistent AT29C040A backing cannot migrate or snapshot");
    if (migrate_add_blocker(&s->backing_migration_blocker, errp) < 0) {
        return false;
    }
    return true;
}

static bool at29_write_backend(SGIO2ROMState *s, BlockBackend *blk,
                                uint64_t offset, const void *data, size_t size,
                                const char *what)
{
    const char *operation = "writing";
    int ret = blk_pwrite(blk, offset, size, data, 0);

    if (ret >= 0) {
        operation = "flushing";
        ret = blk_flush(blk);
    }
    if (ret < 0) {
        error_report("sgi-o2-rom: %s %s failed: %s",
                     operation, what, strerror(-ret));
        s->backing_failed = true;
        blk_error_action(blk, BLOCK_ERROR_ACTION_STOP, false, -ret);
        return false;
    }
    return true;
}

static bool at29_store_backing(SGIO2ROMState *s, const uint8_t *bytes)
{
    g_autofree uint8_t *next = g_memdup2(bytes, AT29_SIZE);
    uint8_t state[AT29_STATE_SIZE];
    uint8_t flags = (s->at29_protected || s->at29_enable_protection) |
                    (s->at29_lower_lock << 1) | (s->at29_upper_lock << 2);

    memcpy(next + s->at29_sector, s->at29_load, AT29_SECTOR_SIZE);
    at29_state_bytes(next, flags, state);
    return at29_write_backend(s, s->blk, s->at29_sector, s->at29_load,
                               AT29_SECTOR_SIZE, "array backend") &&
           at29_write_backend(s, s->state_blk, 0, state, sizeof(state),
                               "state backend");
}

static void at29_complete(SGIO2ROMState *s, int64_t now, bool timer_callback)
{
    if (s->at29_program_allowed) {
        uint8_t *bytes = memory_region_get_ram_ptr(&s->storage);

        if (s->blk && !at29_store_backing(s, bytes)) {
            at29_trace_timeout(s, now, timer_callback, "backing-error");
            at29_idle(s);
            return;
        }
        memcpy(bytes + s->at29_sector, s->at29_load, AT29_SECTOR_SIZE);
        memory_region_flush_rom_device(&s->storage, s->at29_sector,
                                       AT29_SECTOR_SIZE);
        if (s->at29_enable_protection) {
            s->at29_protected = true;
        }
    }
    at29_trace_timeout(s, now, timer_callback, s->at29_program_allowed ?
                       "program-complete" : "protected-poll-complete");
    at29_idle(s);
}

static void at29_advance(SGIO2ROMState *s, int64_t now,
                           bool timer_callback)
{
    while (s->at29_phase != AT29_IDLE &&
           now >= s->at29_deadline_ns) {
        switch (s->at29_phase) {
        case AT29_PREFIX_AA:
        case AT29_PREFIX_55:
        case AT29_PREFIX_READY:
            at29_trace_timeout(s, now, timer_callback, "prefix-expired");
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2-rom: incomplete AT29C040A command\n");
            at29_idle(s);
            break;
        case AT29_LOADING:
            at29_trace_timeout(s, now, timer_callback, "partial");
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2-rom: partial AT29C040A sector load\n");
            at29_idle(s);
            break;
        case AT29_BUSY:
            at29_complete(s, now, timer_callback);
            break;
        case AT29_IDLE:
            g_assert_not_reached();
        }
    }
}

static void at29_timer_fire(void *opaque)
{
    SGIO2ROMState *s = opaque;

    at29_advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), true);
}

static bool at29_locked(SGIO2ROMState *s, uint32_t address)
{
    return (s->at29_lower_lock && address < AT29_BOOT_BLOCK_SIZE) ||
           (s->at29_upper_lock && address >= AT29_SIZE -
            AT29_BOOT_BLOCK_SIZE);
}

static MemTxResult at29_load_byte(SGIO2ROMState *s, uint32_t address,
                                  uint8_t value, int64_t now)
{
    unsigned index = address & (AT29_SECTOR_SIZE - 1);
    uint64_t mask = UINT64_C(1) << (index & 63);
    bool tracing = trace_event_get_state_backends(TRACE_O2_AT29_LOAD);
    uint64_t attempt = tracing ? ++s->at29_trace_load_attempts : 0;
    bool first_load;

    if ((address & ~(AT29_SECTOR_SIZE - 1)) != s->at29_sector) {
        if (tracing) {
            trace_o2_at29_load(address, s->at29_phase, s->at29_sector,
                               s->at29_loaded_count, attempt, now,
                               s->at29_deadline_ns,
                               s->at29_enable_protection, "cross-sector");
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2-rom: cross-sector AT29C040A load\n");
        at29_idle(s);
        return MEMTX_ERROR;
    }
    first_load = !(s->at29_loaded[index / 64] & mask);
    if (first_load) {
        s->at29_loaded[index / 64] |= mask;
        s->at29_loaded_count++;
    }
    s->at29_load[index] = value;
    s->at29_last_address = address;
    s->at29_last_data = value;
    if (first_load && s->at29_loaded_count == AT29_SECTOR_SIZE) {
        timer_del(s->at29_timer);
        s->at29_phase = AT29_BUSY;
        s->at29_poll_toggle = false;
        s->at29_trace_poll_reads = 0;
        s->at29_trace_busy_write_seen = false;
        memory_region_rom_device_set_romd(&s->storage, false);
        at29_schedule(s, now + AT29_CYCLE_NS);
    } else {
        at29_schedule(s, now + AT29_LOAD_NS);
    }
    if (tracing && (attempt <= 2 || !(attempt & (attempt - 1)) ||
                    s->at29_phase == AT29_BUSY)) {
        trace_o2_at29_load(address, s->at29_phase, s->at29_sector,
                           s->at29_loaded_count, attempt, now,
                           s->at29_deadline_ns, s->at29_enable_protection,
                           s->at29_phase == AT29_BUSY ? "program-start" :
                           first_load ? "accepted" : "reloaded");
    }
    return MEMTX_OK;
}

static MemTxResult at29_start_load(SGIO2ROMState *s, uint32_t address,
                                   uint8_t value, int64_t now,
                                   bool prefixed)
{
    if (at29_locked(s, address)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2-rom: AT29C040A boot block is locked\n");
        at29_idle(s);
        return MEMTX_ERROR;
    }
    if (!at29_block_migration(s)) {
        return MEMTX_ERROR;
    }
    s->at29_phase = AT29_LOADING;
    s->at29_sector = address & ~(AT29_SECTOR_SIZE - 1);
    s->at29_program_allowed = true;
    s->at29_enable_protection = prefixed;
    s->at29_loaded_count = 0;
    s->at29_trace_load_attempts = 0;
    memset(s->at29_loaded, 0, sizeof(s->at29_loaded));
    return at29_load_byte(s, address, value, now);
}

static MemTxResult at29_start_protected_poll(SGIO2ROMState *s,
                                             uint32_t address,
                                             uint8_t value, int64_t now)
{
    if (at29_locked(s, address)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2-rom: AT29C040A boot block is locked\n");
        return MEMTX_ERROR;
    }
    if (!at29_block_migration(s)) {
        return MEMTX_ERROR;
    }
    s->at29_phase = AT29_BUSY;
    s->at29_program_allowed = false;
    s->at29_last_address = address;
    s->at29_last_data = value;
    s->at29_poll_toggle = false;
    s->at29_trace_poll_reads = 0;
    s->at29_trace_busy_write_seen = false;
    memory_region_rom_device_set_romd(&s->storage, false);
    at29_schedule(s, now + AT29_CYCLE_NS);
    return MEMTX_OK;
}

static MemTxResult at29_write_byte(SGIO2ROMState *s, uint32_t address,
                                   uint8_t value)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t command_address = address & 0x7fff;

    at29_advance(s, now, false);
    if (s->at29_phase == AT29_BUSY) {
        if (!s->at29_trace_busy_write_seen &&
            trace_event_get_state_backends(TRACE_O2_AT29_LOAD)) {
            s->at29_trace_busy_write_seen = true;
            trace_o2_at29_load(address, s->at29_phase, at29_trace_sector(s),
                               s->at29_loaded_count, 0, now,
                               s->at29_deadline_ns,
                               s->at29_program_allowed &&
                               s->at29_enable_protection,
                               "busy-write-refused");
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2-rom: AT29C040A write during program cycle\n");
        return MEMTX_ERROR;
    }
    if (s->at29_phase == AT29_LOADING) {
        return at29_load_byte(s, address, value, now);
    }
    if (s->at29_phase == AT29_PREFIX_AA) {
        if (command_address != 0x2aaa || value != 0x55) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2-rom: invalid AT29C040A command address\n");
            at29_idle(s);
            return MEMTX_ERROR;
        }
        s->at29_phase = AT29_PREFIX_55;
        at29_schedule(s, now + AT29_LOAD_NS);
        return MEMTX_OK;
    }
    if (s->at29_phase == AT29_PREFIX_55) {
        if (command_address != 0x5555 || value != 0xa0) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2-rom: unsupported AT29C040A command\n");
            at29_idle(s);
            return MEMTX_ERROR;
        }
        s->at29_phase = AT29_PREFIX_READY;
        at29_schedule(s, now + AT29_LOAD_NS);
        return MEMTX_OK;
    }
    if (s->at29_phase == AT29_PREFIX_READY) {
        return at29_start_load(s, address, value, now, true);
    }
    if (command_address == 0x5555 && value == 0xaa) {
        if (!at29_block_migration(s)) {
            return MEMTX_ERROR;
        }
        s->at29_phase = AT29_PREFIX_AA;
        at29_schedule(s, now + AT29_LOAD_NS);
        return MEMTX_OK;
    }
    if (s->at29_protected) {
        return at29_start_protected_poll(s, address, value, now);
    }
    return at29_start_load(s, address, value, now, false);
}

static MemTxResult o2_rom_read(void *opaque, hwaddr offset, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    SGIO2ROMState *s = opaque;
    const uint8_t *bytes = memory_region_get_ram_ptr(&s->storage);
    int64_t now = 0;
    bool busy;

    if (s->at29_diagnostic) {
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        at29_advance(s, now, false);
    }
    busy = s->at29_phase == AT29_BUSY;
    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = bytes[offset + i];

        if (busy) {
            if (offset + i == s->at29_last_address) {
                /* Only the last loaded address reports complementary I/O7. */
                byte = s->at29_last_data ^ 0x80;
            }
            if (s->at29_poll_toggle) {
                byte ^= 0x40;
            }
        }
        *value = (*value << 8) | byte;
    }
    if (busy) {
        if (trace_event_get_state_backends(TRACE_O2_AT29_POLL)) {
            uint64_t reads = ++s->at29_trace_poll_reads;

            if (reads <= 2 || !(reads & (reads - 1))) {
                bool last = offset <= s->at29_last_address &&
                            s->at29_last_address < offset + size;

                trace_o2_at29_poll(offset, size, s->at29_phase,
                                   at29_trace_sector(s), s->at29_loaded_count,
                                   reads, now, s->at29_deadline_ns,
                                   s->at29_poll_toggle,
                                   last ? "data-poll" : "toggle-only");
            }
        }
        s->at29_poll_toggle = !s->at29_poll_toggle;
    }
    return MEMTX_OK;
}

static MemTxResult o2_rom_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    SGIO2ROMState *s = opaque;

    if (size != 1) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2-rom: unsupported multi-byte flash write "
                      "(%u bytes) at offset 0x%" HWADDR_PRIx "\n",
                      size, offset);
        return MEMTX_ERROR;
    }
    if (!s->write_enabled) {
        return MEMTX_OK;
    }
    if (s->backing_failed) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-rom: programming refused after backing failure\n");
        return MEMTX_ERROR;
    }
    if (s->at29_diagnostic) {
        return at29_write_byte(s, offset, value);
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-o2-rom: unsupported enabled flash write of "
                  "0x%02" PRIx64 " at offset 0x%" HWADDR_PRIx "\n",
                  value & 0xff, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps o2_rom_ops = {
    .read_with_attrs = o2_rom_read,
    .write_with_attrs = o2_rom_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static bool at29_parse_state(const char *value, const char *on,
                             const char *off, bool *selected,
                             const char *name, Error **errp)
{
    if (!value) {
        error_setg(errp, "sgi-o2-rom AT29C040A requires %s", name);
        return false;
    }
    if (!strcmp(value, on)) {
        *selected = true;
        return true;
    }
    if (!strcmp(value, off)) {
        *selected = false;
        return true;
    }
    error_setg(errp, "sgi-o2-rom AT29C040A %s must be %s or %s",
               name, on, off);
    return false;
}

static void o2_rom_realize(DeviceState *dev, Error **errp)
{
    SGIO2ROMState *s = SGI_O2_ROM(dev);
    g_autofree char *contents = NULL;
    gsize length;
    GError *io_error = NULL;
    GStatBuf image_stat;
    uint8_t *bytes;

    if (s->flash_size != 512 * KiB && s->flash_size != 1 * MiB &&
        s->flash_size != 2 * MiB) {
        error_setg(errp, "sgi-o2-rom requires flash-size of 512 KiB, "
                   "1 MiB or 2 MiB");
        return;
    }
    if (s->at29_diagnostic) {
        if (s->flash_size != AT29_SIZE || s->image_offset != 0) {
            error_setg(errp, "sgi-o2-rom AT29C040A needs 512 KiB at "
                             "image offset zero");
            return;
        }
        if (!at29_parse_state(s->at29_protection_setting,
                              "enabled", "disabled", &s->at29_protected,
                              "protection", errp) ||
            !at29_parse_state(s->at29_lower_lock_setting,
                              "locked", "unlocked", &s->at29_lower_lock,
                              "lower-lock", errp) ||
            !at29_parse_state(s->at29_upper_lock_setting,
                              "locked", "unlocked", &s->at29_upper_lock,
                              "upper-lock", errp)) {
            return;
        }
    } else if (s->at29_protection_setting || s->at29_lower_lock_setting ||
               s->at29_upper_lock_setting) {
        error_setg(errp, "sgi-o2-rom AT29C040A states require the "
                         "diagnostic profile");
        return;
    }
    if (s->blk || s->state_blk) {
        if (!s->blk || !s->state_blk || !s->at29_diagnostic || s->image ||
            s->image_offset != 0 || s->pad_byte > UINT8_MAX) {
            error_setg(errp, "sgi-o2-rom: backing requires both drive and "
                             "state-drive on AT29C040A without image");
            return;
        }
        contents = g_malloc(AT29_SIZE);
        if (!at29_restore_backing(s, (uint8_t *)contents, errp)) {
            return;
        }
        length = AT29_SIZE;
    } else {
        if (!s->image || s->image_offset == UINT32_MAX ||
            s->pad_byte > UINT8_MAX) {
            error_setg(errp, "sgi-o2-rom requires image, image-offset and "
                       "pad-byte properties");
            return;
        }
        if (s->image_offset > s->flash_size) {
            error_setg(errp, "sgi-o2-rom image-offset exceeds flash-size");
            return;
        }
        if (g_stat(s->image, &image_stat) < 0) {
            error_setg_errno(errp, errno, "sgi-o2-rom cannot stat image '%s'",
                             s->image);
            return;
        }
        if (image_stat.st_size < 0 ||
            (uint64_t)image_stat.st_size > s->flash_size - s->image_offset) {
            error_setg(errp, "sgi-o2-rom image '%s' does not fit at offset "
                       "0x%x in a %u-byte device", s->image, s->image_offset,
                       s->flash_size);
            return;
        }
        if (!g_file_get_contents(s->image, &contents, &length, &io_error)) {
            error_setg(errp, "sgi-o2-rom cannot read image '%s': %s", s->image,
                       io_error->message);
            g_clear_error(&io_error);
            return;
        }
        if (length > s->flash_size - s->image_offset) {
            error_setg(errp, "sgi-o2-rom image '%s' does not fit at offset "
                       "0x%x in a %u-byte device", s->image, s->image_offset,
                       s->flash_size);
            return;
        }
        if (s->at29_diagnostic && length != AT29_SIZE) {
            error_setg(errp, "sgi-o2-rom AT29C040A requires a full 512 KiB "
                             "image");
            return;
        }
    }
    if (!memory_region_init_rom_device(&s->storage, OBJECT(s), &o2_rom_ops,
                                       s, "sgi-o2-rom.storage",
                                       s->flash_size, errp)) {
        return;
    }
    bytes = memory_region_get_ram_ptr(&s->storage);
    memset(bytes, s->pad_byte, s->flash_size);
    memcpy(bytes + s->image_offset, contents, length);
    memory_region_flush_rom_device(&s->storage, 0, s->flash_size);

    memory_region_init(&s->window, OBJECT(s), "sgi-o2-rom.window",
                       O2_ROM_WINDOW);
    memory_region_add_subregion(&s->window, 0, &s->storage);
    for (unsigned i = 1; i < O2_ROM_WINDOW / s->flash_size; i++) {
        g_autofree char *name = g_strdup_printf("sgi-o2-rom.alias%u", i);
        MemoryRegion *alias = &s->aliases[i - 1];

        memory_region_init_alias(alias, OBJECT(s), name, &s->storage,
                                 0, s->flash_size);
        memory_region_add_subregion(&s->window, i * s->flash_size, alias);
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->window);
}

static void o2_rom_reset_enter(Object *obj, ResetType type)
{
    sgi_o2_rom_set_write_enable(SGI_O2_ROM(obj), false);
}

static void o2_rom_init(Object *obj)
{
    SGIO2ROMState *s = SGI_O2_ROM(obj);

    qdev_init_gpio_in_named(DEVICE(obj), o2_rom_flash_write_enable,
                            "flash-write-enable", 1);
    object_property_add_bool(obj, "write-enable", o2_rom_get_write_enable,
                             o2_rom_set_write_enable);
    s->at29_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, at29_timer_fire, s);
}

static void o2_rom_finalize(Object *obj)
{
    SGIO2ROMState *s = SGI_O2_ROM(obj);

    migrate_del_blocker(&s->at29_migration_blocker);
    migrate_del_blocker(&s->backing_migration_blocker);
    timer_free(s->at29_timer);
}

static const Property o2_rom_properties[] = {
    DEFINE_PROP_DRIVE("drive", SGIO2ROMState, blk),
    DEFINE_PROP_DRIVE("state-drive", SGIO2ROMState, state_blk),
    DEFINE_PROP_UINT32("flash-size", SGIO2ROMState, flash_size, 0),
    DEFINE_PROP_STRING("image", SGIO2ROMState, image),
    DEFINE_PROP_UINT32("image-offset", SGIO2ROMState, image_offset,
                       UINT32_MAX),
    DEFINE_PROP_UINT16("pad-byte", SGIO2ROMState, pad_byte, UINT16_MAX),
    DEFINE_PROP_BOOL("mace-controlled", SGIO2ROMState,
                     mace_controlled, false),
    DEFINE_PROP_BOOL("at29c040a-diagnostic", SGIO2ROMState,
                     at29_diagnostic, false),
    DEFINE_PROP_STRING("at29c040a-protection", SGIO2ROMState,
                       at29_protection_setting),
    DEFINE_PROP_STRING("at29c040a-lower-lock", SGIO2ROMState,
                       at29_lower_lock_setting),
    DEFINE_PROP_STRING("at29c040a-upper-lock", SGIO2ROMState,
                       at29_upper_lock_setting),
};

static int o2_rom_pre_save(void *opaque)
{
    SGIO2ROMState *s = opaque;

    if (s->blk) {
        return -ENOTSUP;
    }
    return s->at29_phase == AT29_IDLE ? 0 : -EBUSY;
}

static int o2_rom_pre_load(void *opaque)
{
    return SGI_O2_ROM(opaque)->blk ? -ENOTSUP : 0;
}

static int o2_rom_post_load(void *opaque, int version_id)
{
    SGIO2ROMState *s = opaque;
    bool selected_here = s->at29_protection_setting != NULL;

    if (s->at29_phase != AT29_IDLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-rom: active AT29C040A target refuses "
                      "incoming migration\n");
        return -EBUSY;
    }
    if (s->at29_diagnostic != selected_here ||
        (selected_here &&
         (s->at29_lower_lock !=
          g_str_equal(s->at29_lower_lock_setting, "locked") ||
          s->at29_upper_lock !=
          g_str_equal(s->at29_upper_lock_setting, "locked")))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-rom: AT29C040A migration profile mismatch\n");
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_o2_rom = {
    .name = TYPE_SGI_O2_ROM,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = o2_rom_pre_save,
    .pre_load = o2_rom_pre_load,
    .post_load = o2_rom_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(flash_size, SGIO2ROMState),
        VMSTATE_UINT32_EQUAL(image_offset, SGIO2ROMState),
        VMSTATE_UINT16_EQUAL(pad_byte, SGIO2ROMState),
        VMSTATE_BOOL(write_enabled, SGIO2ROMState),
        VMSTATE_BOOL(at29_diagnostic, SGIO2ROMState),
        VMSTATE_BOOL(at29_protected, SGIO2ROMState),
        VMSTATE_BOOL(at29_lower_lock, SGIO2ROMState),
        VMSTATE_BOOL(at29_upper_lock, SGIO2ROMState),
        VMSTATE_END_OF_LIST()
    },
};

static void o2_rom_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "O2 MACE system ROM diagnostic view";
    dc->user_creatable = true;
    dc->realize = o2_rom_realize;
    dc->vmsd = &vmstate_o2_rom;
    device_class_set_props(dc, o2_rom_properties);
    rc->phases.enter = o2_rom_reset_enter;
}

static const TypeInfo o2_rom_info = {
    .name = TYPE_SGI_O2_ROM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIO2ROMState),
    .instance_init = o2_rom_init,
    .instance_finalize = o2_rom_finalize,
    .class_init = o2_rom_class_init,
};

static void o2_rom_register_types(void)
{
    type_register_static(&o2_rom_info);
}

type_init(o2_rom_register_types)
