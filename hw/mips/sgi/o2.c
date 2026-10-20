/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Synthetic O2 (IP32) register and wiring test profile.
 * This machine pairs catalogued O2 module 030-1247-001 (195 MHz R10000)
 * with 64 or 128 MiB diagnostic RAM, or an explicit synthetic 1 GiB
 * eight-bank fit. Reset straps and fitted DIMM mapping are unknown.
 * It has no fitted flash or product catalogue offering. An opt-in storage
 * diagnostic uses a synthetic PCI byte view, not fitted hardware behavior.
 * Serial A accepts an explicit host backend; flash write control is
 * unavailable, and migration has not been qualified for this profile.
 */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi/crime.h"
#include "hw/mips/sgi/mace.h"
#include "hw/mips/sgi/o2-rom.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/aic7880.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "target/mips/cpu.h"
#include "trace.h"

#define O2_RAM_SIZE (64 * MiB)
#define O2_RAM_OPTIONAL_SIZE (128 * MiB)
#define O2_RAM_BANKED_SIZE (1 * GiB)
#define O2_CRIME_BASE 0x14000000ULL
#define O2_GBE_BASE 0x16000000ULL
#define O2_AIC_PCI_VIEW 0x1a000000ULL
#define O2_PCI_HIGH_VIEW 0x280000000ULL
#define O2_MACE_BASE 0x1f000000ULL
#define O2_ROM_BASE 0x1fc00000ULL
#define O2_PRIVATE_SIZE 0x1008
#define O2_RESET_ROM_VADDR 0xffffffffbfc00000ULL
#define O2_PRIVATE_POLL_NS (1 * SCALE_MS)
#define O2_PROM_SB64_RESET_MODE 0x00198603

typedef struct O2MachineState {
    MachineState parent;
    char *rom_image;
    char *rom_at29_protection;
    char *rom_at29_lower_lock;
    char *rom_at29_upper_lock;
    char *storage_event_file;
    char *identity_test_serial;
    char *identity_eprom_file;
    char *identity_status;
    uint32_t rom_size;
    uint32_t rom_offset;
    uint16_t rom_pad_byte;
    MemoryRegion cpu_uncached;
    MemoryRegion cpu_physical;
    MemoryRegion cpu_cached;
    MemoryRegion cpu_cached_physical;
    MemoryRegion private_filter;
    MemoryRegion cpu_uncached_low_ram;
    MemoryRegion cpu_uncached_full_ram;
    MemoryRegion cpu_uncached_no_ecc_ram;
    Clock *cpuclk;
    PCIBus *pci_bus;
    PCIDevice *aic[2];
    DeviceState *storage_target;
    PCIDevice *mace_dma_card;
    Notifier machine_done;
    Notifier storage_exit;
    Error *migration_blocker;
    Error *storage_target_unplug_blocker;
    bool machine_done_registered;
    bool storage_exit_registered;
    bool vmstate_registered;
    bool rom_at29c040a;
    bool banked_ram;
    bool reset_low_ram_approx;
    bool empty_renderer_status;
    bool prom_private_window;
    bool prom_sb64_reset;
    bool gbe11_registers;
    bool gbe_one_tile_diagnostic;
    bool aic_pci_diagnostic;
    bool diagnostic_lower_pci_byte_identity;
    bool storage_event_failed;
    bool storage_event_short_write;
    bool storage_event_final_sync_fail;
    bool mace_bank_dma_fixture;
    bool identity_diagnostic;
    bool mace_misc_word_pio;
    bool mace_eth_low_dma_compat;
    bool prom_id_word;
    bool phy_ics1890;
    uint8_t phy_address;
    bool private_armed;
    bool private_active;
    bool private_sample_queued;
    bool private_incomplete_reported;
    bool private_low_exec_reported;
    MIPSCPU *private_cpu;
    QEMUTimer *private_timer;
    uint8_t private_data[O2_PRIVATE_SIZE];
    uint8_t private_backing[O2_PRIVATE_SIZE];
    int storage_event_fd;
    uint64_t storage_attempts;
    uint64_t storage_records;
} O2MachineState;

static bool o2_storage_event_write(O2MachineState *s, const char *line,
                                   size_t length, bool final_record)
{
    ssize_t written;

    if (s->storage_event_short_write) {
        s->storage_event_short_write = false;
        written = write(s->storage_event_fd, line, length / 2);
        errno = EIO;
        return false;
    }
    do {
        written = write(s->storage_event_fd, line, length);
    } while (written < 0 && errno == EINTR);
    if (written >= 0 && written != (ssize_t)length) {
        errno = EIO;
        return false;
    }
    if (final_record && s->storage_event_final_sync_fail &&
        written == (ssize_t)length) {
        errno = EIO;
        return false;
    }
    return written == (ssize_t)length && fdatasync(s->storage_event_fd) == 0;
}

static void o2_storage_event_fail(O2MachineState *s)
{
    if (s->storage_event_failed) {
        return;
    }
    s->storage_event_failed = true;
    error_report("o2-test storage diagnostic event write failed: %s",
                 g_strerror(errno));
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
}

static bool o2_storage_report(void *opaque, PCIDevice *requester,
                              uint32_t pci_addr, bool to_memory,
                              uint32_t length, MemTxResult result)
{
    O2MachineState *s = opaque;
    char line[320];
    int n;

    s->storage_attempts++;
    if (s->storage_event_failed || s->storage_event_fd < 0) {
        return false;
    }
    n = snprintf(line, sizeof(line),
                 "{\"type\":\"dma\",\"sequence\":%" PRIu64
                 ",\"requester\":\"%02x:%02x.%u\","
                 "\"pci_address\":\"0x%08" PRIx32
                 "\",\"direction\":\"%s\",\"length\":%" PRIu32
                 ",\"result\":%u,\"policy\":\"diagnostic-byte-identity\"}\n",
                 s->storage_attempts,
                 (unsigned)pci_bus_num(pci_get_bus(requester)),
                 PCI_SLOT(requester->devfn), PCI_FUNC(requester->devfn),
                 pci_addr, to_memory ? "to-memory" : "from-memory",
                 length, (unsigned)result);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        errno = EOVERFLOW;
        o2_storage_event_fail(s);
        return false;
    }
    if (!o2_storage_event_write(s, line, n, false)) {
        o2_storage_event_fail(s);
        return false;
    }
    s->storage_records++;
    return true;
}

static void o2_storage_exit(Notifier *notifier, void *data)
{
    O2MachineState *s = container_of(notifier, O2MachineState, storage_exit);
    char line[160];
    int n;
    int offset_errno;
    bool written = false;
    off_t complete_offset;

    if (s->storage_event_fd < 0) {
        return;
    }
    if (!s->storage_event_failed) {
        complete_offset = lseek(s->storage_event_fd, 0, SEEK_CUR);
        offset_errno = errno;
        n = snprintf(line, sizeof(line),
                     "{\"type\":\"complete\",\"attempts\":%" PRIu64
                     ",\"records\":%" PRIu64 ",\"complete\":true}\n",
                     s->storage_attempts, s->storage_records);
        if (complete_offset < 0) {
            errno = offset_errno;
        } else if (n < 0 || (size_t)n >= sizeof(line)) {
            errno = EOVERFLOW;
        } else {
            written = o2_storage_event_write(s, line, n, true);
        }
        if (!written) {
            int write_errno = errno;

            if (complete_offset >= 0 &&
                (ftruncate(s->storage_event_fd, complete_offset) < 0 ||
                 fdatasync(s->storage_event_fd) < 0)) {
                error_report("o2-test cannot remove incomplete storage "
                             "completion record: %s", g_strerror(errno));
            }
            error_report("o2-test storage diagnostic final event write "
                         "failed: %s", g_strerror(write_errno));
            close(s->storage_event_fd);
            s->storage_event_fd = -1;
            fflush(stderr);
            /* Exit notifiers run after QEMU has chosen its normal status. */
            _Exit(EXIT_FAILURE);
        }
    }
    close(s->storage_event_fd);
    s->storage_event_fd = -1;
}

static const VMStateDescription vmstate_o2_banked = {
    .name = "o2-test/eight-bank-profile",
    .unmigratable = true,
};

static char *o2_get_bank_profile(Object *obj, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    return g_strdup(s->banked_ram ? "8x128m-synthetic" : "");
}

static void o2_set_bank_profile(Object *obj, const char *value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "bank-profile cannot change once the machine "
                         "is built");
        return;
    }
    if (strcmp(value, "8x128m-synthetic")) {
        error_setg(errp, "o2-test supports only bank-profile=8x128m-synthetic");
        return;
    }
    s->banked_ram = true;
}

static bool o2_get_reset_low_ram_approx(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->reset_low_ram_approx;
}

static void o2_set_reset_low_ram_approx(Object *obj, bool value,
                                        Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "reset-low-ram-approx cannot change once the "
                         "machine is built");
        return;
    }
    s->reset_low_ram_approx = value;
}

static bool o2_get_empty_renderer_status(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->empty_renderer_status;
}

static void o2_set_empty_renderer_status(Object *obj, bool value,
                                         Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "empty-renderer-status cannot change once the "
                         "machine is built");
        return;
    }
    s->empty_renderer_status = value;
}

static bool o2_get_prom_private_window(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->prom_private_window;
}

static void o2_set_prom_private_window(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "prom-private-window cannot change once the "
                         "machine is built");
        return;
    }
    s->prom_private_window = value;
}

static bool o2_get_prom_sb64_reset(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->prom_sb64_reset;
}

static void o2_set_prom_sb64_reset(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "prom-sb64-reset cannot change once the "
                         "machine is built");
        return;
    }
    s->prom_sb64_reset = value;
}

static bool o2_get_gbe11_registers(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->gbe11_registers;
}

static void o2_set_gbe11_registers(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "gbe11-registers cannot change once the "
                         "machine is built");
        return;
    }
    s->gbe11_registers = value;
}

static bool o2_get_gbe_one_tile_diagnostic(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->gbe_one_tile_diagnostic;
}

static void o2_set_gbe_one_tile_diagnostic(Object *obj, bool value,
                                           Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "gbe-one-tile-diagnostic cannot change once the "
                         "machine is built");
        return;
    }
    s->gbe_one_tile_diagnostic = value;
}

static bool o2_get_aic_pci_diagnostic(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->aic_pci_diagnostic;
}

static void o2_set_aic_pci_diagnostic(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "aic-pci-diagnostic cannot change once the "
                         "machine is built");
        return;
    }
    s->aic_pci_diagnostic = value;
}

static bool o2_get_diagnostic_lower_pci_byte_identity(Object *obj,
                                                       Error **errp)
{
    return ((O2MachineState *)obj)->diagnostic_lower_pci_byte_identity;
}

static void o2_set_diagnostic_lower_pci_byte_identity(Object *obj,
                                                       bool value,
                                                       Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "diagnostic-lower-pci-byte-identity cannot change "
                         "once the machine is built");
        return;
    }
    s->diagnostic_lower_pci_byte_identity = value;
}

static char *o2_get_storage_event_file(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->storage_event_file);
}

static void o2_set_storage_event_file(Object *obj, const char *value,
                                      Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "storage-diagnostic-event-file cannot change "
                         "once the machine is built");
        return;
    }
    g_free(s->storage_event_file);
    s->storage_event_file = g_strdup(value);
}

static bool o2_get_storage_event_short_write(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->storage_event_short_write;
}

static void o2_set_storage_event_short_write(Object *obj, bool value,
                                             Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (!qtest_enabled() || !s->diagnostic_lower_pci_byte_identity) {
        error_setg(errp, "storage event short-write injection requires "
                         "the QTest storage diagnostic");
        return;
    }
    s->storage_event_short_write = value;
}

static bool o2_get_storage_event_final_sync_fail(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->storage_event_final_sync_fail;
}

static void o2_set_storage_event_final_sync_fail(Object *obj, bool value,
                                                 Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (!qtest_enabled() || !s->diagnostic_lower_pci_byte_identity) {
        error_setg(errp, "storage final-sync injection requires "
                         "the QTest storage diagnostic");
        return;
    }
    s->storage_event_final_sync_fail = value;
}

static bool o2_get_mace_bank_dma_fixture(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->mace_bank_dma_fixture;
}

static void o2_set_mace_bank_dma_fixture(Object *obj, bool value,
                                         Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "mace-bank-dma-fixture cannot change once the "
                         "machine is built");
        return;
    }
    s->mace_bank_dma_fixture = value;
}

static bool o2_get_identity_diagnostic(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->identity_diagnostic;
}

static void o2_set_identity_diagnostic(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "identity-diagnostic cannot change once the "
                         "machine is built");
        return;
    }
    s->identity_diagnostic = value;
}

static bool o2_get_mace_misc_word_pio(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->mace_misc_word_pio;
}

static void o2_set_mace_misc_word_pio(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "mace-misc-word-pio cannot change once the "
                         "machine is built");
        return;
    }
    s->mace_misc_word_pio = value;
}

static bool o2_get_mace_eth_low_dma_compat(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->mace_eth_low_dma_compat;
}

static void o2_set_mace_eth_low_dma_compat(Object *obj, bool value,
                                            Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "mace-eth-low-dma-compat cannot change once the "
                         "machine is built");
        return;
    }
    s->mace_eth_low_dma_compat = value;
}

static char *o2_get_identity_test_serial(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->identity_test_serial);
}

static void o2_set_identity_test_serial(Object *obj, const char *value,
                                        Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "identity-test-serial cannot change once the "
                         "machine is built");
        return;
    }
    g_free(s->identity_test_serial);
    s->identity_test_serial = g_strdup(value);
}

static char *o2_get_identity_eprom_file(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->identity_eprom_file);
}

static void o2_set_identity_eprom_file(Object *obj, const char *value,
                                       Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "identity-eprom-file cannot change once the "
                         "machine is built");
        return;
    }
    g_free(s->identity_eprom_file);
    s->identity_eprom_file = g_strdup(value);
}

static char *o2_get_identity_status(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->identity_status);
}

static void o2_set_identity_status(Object *obj, const char *value,
                                   Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "identity-status cannot change once the "
                         "machine is built");
        return;
    }
    g_free(s->identity_status);
    s->identity_status = g_strdup(value);
}

static bool o2_get_prom_id_word(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->prom_id_word;
}

static void o2_set_prom_id_word(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "prom-id-word cannot change once the "
                         "machine is built");
        return;
    }
    s->prom_id_word = value;
}

static bool o2_private_pc_in_rom(const O2MachineState *s)
{
    uint64_t pc = s->private_cpu->env.active_tc.PC;

    return pc >= O2_RESET_ROM_VADDR &&
           pc < O2_RESET_ROM_VADDR + 4 * MiB;
}

static bool o2_private_selected(O2MachineState *s, MemTxAttrs attrs)
{
    if (current_cpu != CPU(s->private_cpu) || attrs.mips_cca != 3) {
        return false;
    }
    if (s->private_active) {
        return true;
    }
    if (!s->private_armed) {
        return false;
    }
    if (!o2_private_pc_in_rom(s)) {
        s->private_armed = false;
        trace_o2_private_phase("missed", s->private_cpu->env.active_tc.PC,
                               s->private_cpu->env.active_tc.gpr[29],
                               attrs.unspecified);
        qemu_log_mask(LOG_UNIMP,
                      "o2 private window: missed reset-ROM trigger at "
                      "PC 0x%016" PRIx64 "\n",
                      s->private_cpu->env.active_tc.PC);
        return false;
    }
    if ((s->private_cpu->env.CP0_Config0 & 7) != 3) {
        return false;
    }
    if (address_space_read(&address_space_memory, 0,
                           MEMTXATTRS_UNSPECIFIED, s->private_data,
                           O2_PRIVATE_SIZE) != MEMTX_OK) {
        s->private_armed = false;
        trace_o2_private_phase("unavailable",
                               s->private_cpu->env.active_tc.PC,
                               s->private_cpu->env.active_tc.gpr[29],
                               attrs.unspecified);
        qemu_log_mask(LOG_UNIMP,
                      "o2 private window: decoded low RAM unavailable\n");
        return false;
    }
    s->private_active = true;
    s->private_armed = false;
    trace_o2_private_phase("enter", s->private_cpu->env.active_tc.PC,
                           s->private_cpu->env.active_tc.gpr[29],
                           attrs.unspecified);
    return true;
}

static bool o2_private_pc_in_window(O2MachineState *s)
{
    TranslateForDebugResult r;
    uint64_t pc = s->private_cpu->env.active_tc.PC;
    bool cached;

    /* MIPS debug translation returns a PA, but discards the cache algorithm. */
    if (pc >= UINT64_C(0xffffffff80000000) &&
        pc < UINT64_C(0xffffffffa0000000)) {
        cached = (s->private_cpu->env.CP0_Config0 & 7) == 3;
    } else if (pc >= UINT64_C(0x8000000000000000) &&
               pc < UINT64_C(0xc000000000000000)) {
        cached = ((pc >> 59) & 7) == 3;
    } else {
        return false;
    }
    return cached && cpu_translate_for_debug(CPU(s->private_cpu), pc, &r) &&
           r.physaddr < O2_PRIVATE_SIZE;
}

static bool o2_private_sp_in_uncached_ram(O2MachineState *s)
{
    uint64_t sp = s->private_cpu->env.active_tc.gpr[29];

    return sp >= UINT64_C(0xffffffffa0000000) &&
           sp < UINT64_C(0xffffffffb0000000);
}

static void o2_private_poll_cpu(CPUState *cs, run_on_cpu_data data)
{
    O2MachineState *s = data.host_ptr;
    uint64_t pc, sp;

    BQL_LOCK_GUARD();
    s->private_sample_queued = false;
    pc = s->private_cpu->env.active_tc.PC;
    sp = s->private_cpu->env.active_tc.gpr[29];
    if (s->private_armed && !o2_private_pc_in_rom(s)) {
        s->private_armed = false;
        trace_o2_private_phase("missed", pc, sp, false);
        qemu_log_mask(LOG_UNIMP,
                      "o2 private window: missed reset-ROM trigger at "
                      "PC 0x%016" PRIx64 "\n", pc);
    }
    if (!s->private_active) {
        return;
    }
    if (o2_private_pc_in_window(s)) {
        if (!s->private_low_exec_reported) {
            s->private_low_exec_reported = true;
            trace_o2_private_phase("low-exec", pc, sp, false);
            qemu_log_mask(LOG_UNIMP,
                          "o2 private window: unsupported execution at "
                          "PC 0x%016" PRIx64 "\n", pc);
        }
        return;
    }
    if (!o2_private_sp_in_uncached_ram(s)) {
        return;
    }
    if (address_space_read(&address_space_memory, 0,
                           MEMTXATTRS_UNSPECIFIED, s->private_backing,
                           O2_PRIVATE_SIZE) != MEMTX_OK ||
        memcmp(s->private_data, s->private_backing, O2_PRIVATE_SIZE)) {
        if (!s->private_incomplete_reported) {
            s->private_incomplete_reported = true;
            trace_o2_private_phase("incomplete", pc, sp, false);
            qemu_log_mask(LOG_UNIMP,
                          "o2 private window: KSEG1 stack switch before "
                          "private bytes reached decoded RAM\n");
        }
        return;
    }
    s->private_active = false;
    memory_region_set_enabled(&s->private_filter, false);
    trace_o2_private_phase("release", pc, sp, false);
}

static void o2_private_poll(void *opaque)
{
    O2MachineState *s = opaque;

    if (!s->private_armed && !s->private_active) {
        return;
    }
    if (!s->private_sample_queued) {
        s->private_sample_queued = true;
        async_run_on_cpu(CPU(s->private_cpu), o2_private_poll_cpu,
                         RUN_ON_CPU_HOST_PTR(s));
    }
    timer_mod(s->private_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + O2_PRIVATE_POLL_NS);
}

static void o2_private_reset(void *opaque)
{
    O2MachineState *s = opaque;

    s->private_armed = true;
    s->private_active = false;
    s->private_incomplete_reported = false;
    s->private_low_exec_reported = false;
    memset(s->private_data, 0, sizeof(s->private_data));
    memory_region_set_enabled(&s->private_filter, true);
    timer_mod(s->private_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + O2_PRIVATE_POLL_NS);
}

static MemTxResult o2_private_read(void *opaque, hwaddr addr, uint64_t *val,
                                    unsigned size, MemTxAttrs attrs)
{
    O2MachineState *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;

    if (o2_private_selected(s, attrs)) {
        *val = ldn_be_p(s->private_data + addr, size);
        return MEMTX_OK;
    }
    result = address_space_read(&address_space_memory, addr, attrs,
                                bytes, size);
    *val = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult o2_private_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size, MemTxAttrs attrs)
{
    O2MachineState *s = opaque;
    uint8_t bytes[8];

    if (o2_private_selected(s, attrs)) {
        stn_be_p(s->private_data + addr, size, val);
        return MEMTX_OK;
    }
    stn_be_p(bytes, size, val);
    return address_space_write(&address_space_memory, addr, attrs,
                               bytes, size);
}

static const MemoryRegionOps o2_private_ops = {
    .read_with_attrs = o2_private_read,
    .write_with_attrs = o2_private_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static void o2_map_uncached_flat_ram(O2MachineState *s, MemoryRegion *ram,
                                     uint64_t size)
{
    uint64_t base = 1ULL << MIPS_UNCACHED_WINDOW_SHIFT;

    memory_region_init_alias(&s->cpu_uncached_low_ram, OBJECT(s),
                             "o2.cpu-uncached.low-ram", ram, 0, size);
    memory_region_add_subregion(&s->cpu_uncached, base,
                                &s->cpu_uncached_low_ram);
    memory_region_init_alias(&s->cpu_uncached_full_ram, OBJECT(s),
                             "o2.cpu-uncached.full-ram", ram, 0, size);
    memory_region_add_subregion(&s->cpu_uncached, base + 0x40000000ULL,
                                &s->cpu_uncached_full_ram);
    memory_region_init_alias(&s->cpu_uncached_no_ecc_ram, OBJECT(s),
                             "o2.cpu-uncached.no-ecc-ram", ram, 0, size);
    memory_region_add_subregion(&s->cpu_uncached, base + 0x80000000ULL,
                                &s->cpu_uncached_no_ecc_ram);
}

static void o2_get_cpu_clock_hz(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;
    uint64_t hz = s->cpuclk ? clock_get_hz(s->cpuclk) : 0;

    visit_type_uint64(v, name, &hz, errp);
}

typedef struct O2PCIPopulation {
    O2MachineState *machine;
    unsigned count;
    bool unexpected;
} O2PCIPopulation;

static void o2_note_pci_device(PCIBus *bus, PCIDevice *dev, void *opaque)
{
    O2PCIPopulation *population = opaque;
    O2MachineState *s = population->machine;

    population->count++;
    population->unexpected |=
        dev != s->aic[0] && dev != s->aic[1] &&
        (!s->mace_bank_dma_fixture || dev != s->mace_dma_card);
}

static int o2_note_aic_child(DeviceState *dev, void *opaque)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_SCSI_DEVICE)) {
        (*(unsigned *)opaque)++;
    }
    return 0;
}

typedef struct O2StorageTargets {
    unsigned count;
    bool unsupported;
    DeviceState *target;
} O2StorageTargets;

static int o2_note_storage_target(DeviceState *dev, void *opaque)
{
    O2StorageTargets *targets = opaque;
    SCSIDevice *scsi;

    if (!object_dynamic_cast(OBJECT(dev), TYPE_SCSI_DEVICE)) {
        return 0;
    }
    scsi = SCSI_DEVICE(dev);
    targets->count++;
    if (!targets->target) {
        targets->target = dev;
    }
    targets->unsupported |= scsi->channel != 0 || scsi->lun != 0 ||
                            scsi->id > 6 ||
                            (!object_dynamic_cast(OBJECT(dev), "scsi-cd") &&
                             !object_dynamic_cast(OBJECT(dev), "scsi-hd"));
    return 0;
}

static void o2_machine_init_done(Notifier *notifier, void *data)
{
    O2MachineState *s = container_of(notifier, O2MachineState, machine_done);
    O2PCIPopulation population = { .machine = s };
    unsigned targets = 0;
    O2StorageTargets storage_targets = { 0 };

    pci_for_each_device_under_bus(s->pci_bus, o2_note_pci_device,
                                  &population);
    if (population.unexpected ||
        population.count != 2 + !!s->mace_bank_dma_fixture) {
        error_report("o2-test does not support attached PCI devices");
        exit(1);
    }
    if (s->aic_pci_diagnostic || s->mace_bank_dma_fixture) {
        for (unsigned i = 0; i < 2; i++) {
            qdev_walk_children(DEVICE(s->aic[i]), NULL, NULL,
                               o2_note_aic_child, NULL, &targets);
        }
        if (targets) {
            error_report(s->mace_bank_dma_fixture ?
                         "o2-test MACE bank DMA fixture rejects SCSI targets" :
                         "o2-test AIC PCI diagnostic rejects SCSI targets");
            exit(1);
        }
    } else if (s->diagnostic_lower_pci_byte_identity) {
        for (unsigned i = 0; i < 2; i++) {
            qdev_walk_children(DEVICE(s->aic[i]), NULL, NULL,
                               o2_note_storage_target, NULL,
                               &storage_targets);
        }
        if (storage_targets.count > 1 || storage_targets.unsupported) {
            error_report("o2-test storage diagnostic accepts one SCSI "
                         "CD or disk at LUN 0, target 0-6");
            exit(1);
        }
        if (storage_targets.target) {
            s->storage_target = storage_targets.target;
            object_ref(OBJECT(s->storage_target));
            error_setg(&s->storage_target_unplug_blocker,
                       "o2-test storage diagnostic target cannot be "
                       "unplugged");
            qdev_add_unplug_blocker(s->storage_target,
                                    s->storage_target_unplug_blocker);
        }
        s->storage_exit.notify = o2_storage_exit;
        qemu_add_exit_notifier(&s->storage_exit);
        s->storage_exit_registered = true;
    } else {
        for (unsigned i = 0; i < 2; i++) {
            qdev_walk_children(DEVICE(s->aic[i]), NULL, NULL,
                               o2_note_storage_target, NULL,
                               &storage_targets);
        }
        if (storage_targets.count > 1 || storage_targets.unsupported) {
            error_report("o2-test accepts one SCSI CD or disk at LUN 0, "
                         "target 0-6");
            exit(1);
        }
    }
}

static char *o2_get_rom_image(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->rom_image);
}

static bool o2_rom_settings_locked(O2MachineState *s, Error **errp)
{
    if (s->banked_ram && phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "o2-test eight-bank ROM settings cannot change "
                         "once the machine is built");
        return true;
    }
    return false;
}

static void o2_set_rom_image(Object *obj, const char *value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (o2_rom_settings_locked(s, errp)) {
        return;
    }
    g_free(s->rom_image);
    s->rom_image = g_strdup(value);
}

static bool o2_get_rom_at29c040a(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->rom_at29c040a;
}

static void o2_set_rom_at29c040a(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "rom-at29c040a cannot change after O2 initialization");
        return;
    }
    s->rom_at29c040a = value;
}

static bool o2_at29_settings_locked(Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "O2 AT29C040A settings cannot change after "
                         "initialization");
        return true;
    }
    return false;
}

static char *o2_get_rom_at29_protection(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->rom_at29_protection);
}

static void o2_set_rom_at29_protection(Object *obj, const char *value,
                                       Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (o2_at29_settings_locked(errp)) {
        return;
    }
    g_free(s->rom_at29_protection);
    s->rom_at29_protection = g_strdup(value);
}

static char *o2_get_rom_at29_lower_lock(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->rom_at29_lower_lock);
}

static void o2_set_rom_at29_lower_lock(Object *obj, const char *value,
                                       Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (o2_at29_settings_locked(errp)) {
        return;
    }
    g_free(s->rom_at29_lower_lock);
    s->rom_at29_lower_lock = g_strdup(value);
}

static char *o2_get_rom_at29_upper_lock(Object *obj, Error **errp)
{
    return g_strdup(((O2MachineState *)obj)->rom_at29_upper_lock);
}

static void o2_set_rom_at29_upper_lock(Object *obj, const char *value,
                                       Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (o2_at29_settings_locked(errp)) {
        return;
    }
    g_free(s->rom_at29_upper_lock);
    s->rom_at29_upper_lock = g_strdup(value);
}

static uint32_t *o2_rom_uint32_field(O2MachineState *s, const char *name)
{
    if (!strcmp(name, "rom-size")) {
        return &s->rom_size;
    }
    g_assert(!strcmp(name, "rom-offset"));
    return &s->rom_offset;
}

static void o2_get_rom_uint32(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    uint32_t value = *o2_rom_uint32_field((O2MachineState *)obj, name);

    visit_type_uint32(v, name, &value, errp);
}

static void o2_set_rom_uint32(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;
    uint32_t value;

    if (o2_rom_settings_locked(s, errp) ||
        !visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    *o2_rom_uint32_field(s, name) = value;
}

static void o2_get_rom_pad_byte(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint16_t value = ((O2MachineState *)obj)->rom_pad_byte;

    visit_type_uint16(v, name, &value, errp);
}

static void o2_set_rom_pad_byte(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;
    uint16_t value;

    if (o2_rom_settings_locked(s, errp) ||
        !visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    s->rom_pad_byte = value;
}

static bool o2_get_phy_ics1890(Object *obj, Error **errp)
{
    return ((O2MachineState *)obj)->phy_ics1890;
}

static void o2_set_phy_ics1890(Object *obj, bool value, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "phy-ics1890 cannot change after O2 initialization");
        return;
    }
    s->phy_ics1890 = value;
}

static void o2_get_phy_address(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint8_t value = ((O2MachineState *)obj)->phy_address;

    visit_type_uint8(v, name, &value, errp);
}

static void o2_set_phy_address(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    O2MachineState *s = (O2MachineState *)obj;
    uint8_t value;

    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "phy-address cannot change after O2 initialization");
        return;
    }
    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > 31) {
        error_setg(errp, "phy-address must be 0 to 31");
        return;
    }
    s->phy_address = value;
}

static void o2_instance_init(Object *obj)
{
    O2MachineState *s = (O2MachineState *)obj;

    s->rom_offset = UINT32_MAX;
    s->rom_pad_byte = UINT16_MAX;
    s->phy_address = UINT8_MAX;
    s->storage_event_fd = -1;
}

static void o2_instance_finalize(Object *obj)
{
    O2MachineState *s = (O2MachineState *)obj;

    if (s->private_timer) {
        timer_free(s->private_timer);
    }
    if (s->machine_done_registered) {
        qemu_remove_machine_init_done_notifier(&s->machine_done);
    }
    if (s->storage_exit_registered) {
        qemu_remove_exit_notifier(&s->storage_exit);
    }
    if (s->storage_event_fd >= 0) {
        close(s->storage_event_fd);
    }
    if (s->storage_target) {
        qdev_del_unplug_blocker(s->storage_target,
                                s->storage_target_unplug_blocker);
        error_free(s->storage_target_unplug_blocker);
        object_unref(OBJECT(s->storage_target));
    }
    if (s->vmstate_registered) {
        vmstate_unregister(NULL, &vmstate_o2_banked, s);
    }
    migrate_del_blocker(&s->migration_blocker);
    g_free(s->rom_image);
    g_free(s->rom_at29_protection);
    g_free(s->rom_at29_lower_lock);
    g_free(s->rom_at29_upper_lock);
    g_free(s->storage_event_file);
    g_free(s->identity_test_serial);
    g_free(s->identity_eprom_file);
    g_free(s->identity_status);
}

static void o2_init(MachineState *machine)
{
    O2MachineState *s = (O2MachineState *)machine;
    Clock *cpuclk;
    MIPSCPU *cpu;
    DeviceState *cpu_dev;
    DeviceState *crime;
    DeviceState *gbe;
    DeviceState *mace;
    DeviceState *rom;
    Error *err = NULL;
    GStatBuf image_stat;

    if (!s->identity_diagnostic &&
        (s->identity_test_serial || s->identity_eprom_file ||
         s->identity_status)) {
        error_report("o2-test identity inputs require "
                     "identity-diagnostic=on");
        exit(1);
    }
    if (s->mace_misc_word_pio && !s->identity_diagnostic) {
        error_report("o2-test mace-misc-word-pio requires "
                     "identity-diagnostic=on");
        exit(1);
    }
    if (s->identity_diagnostic &&
        (!s->identity_test_serial || !s->identity_eprom_file ||
         !s->identity_status)) {
        error_report("o2-test identity diagnostic requires "
                     "identity-test-serial, identity-eprom-file and "
                     "identity-status");
        exit(1);
    }
    if (s->diagnostic_lower_pci_byte_identity &&
        (!s->banked_ram || machine->ram_size != O2_RAM_BANKED_SIZE ||
         !s->reset_low_ram_approx || !s->empty_renderer_status ||
         !s->prom_private_window || !s->prom_sb64_reset ||
         !s->gbe11_registers || !s->prom_id_word ||
         !s->rom_image || !s->rom_size ||
         s->rom_offset == UINT32_MAX ||
         s->rom_pad_byte == UINT16_MAX ||
         s->aic_pci_diagnostic || s->mace_bank_dma_fixture ||
         (!qtest_enabled() && !tcg_enabled()))) {
        error_report("o2-test diagnostic-lower-pci-byte-identity requires "
                     "the complete 1 GiB eight-bank PROM diagnostic, TCG "
                     "or QTest, and no other PCI diagnostic");
        exit(1);
    }
    if (s->diagnostic_lower_pci_byte_identity) {
        if (!s->storage_event_file ||
            !g_path_is_absolute(s->storage_event_file)) {
            error_report("o2-test storage diagnostic requires an absolute "
                         "storage-diagnostic-event-file path");
            exit(1);
        }
        s->storage_event_fd = g_open(s->storage_event_file,
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                     0600);
        if (s->storage_event_fd < 0) {
            error_report("o2-test cannot create storage diagnostic event "
                         "file '%s': %s", s->storage_event_file,
                         g_strerror(errno));
            exit(1);
        }
    } else if (s->storage_event_file) {
        error_report("o2-test storage-diagnostic-event-file requires "
                     "diagnostic-lower-pci-byte-identity=on");
        exit(1);
    }

    if (s->mace_bank_dma_fixture &&
        (!qtest_enabled() || !s->banked_ram ||
         machine->ram_size != O2_RAM_BANKED_SIZE || s->rom_image ||
         s->aic_pci_diagnostic)) {
        error_report("o2-test mace-bank-dma-fixture requires QTest, the "
                     "1 GiB eight-bank profile and no ROM or AIC diagnostic");
        exit(1);
    }

    if (s->mace_eth_low_dma_compat &&
        (!s->banked_ram || machine->ram_size != O2_RAM_BANKED_SIZE)) {
        error_report("o2-test mace-eth-low-dma-compat requires the "
                     "1 GiB eight-bank profile");
        exit(1);
    }

    if (s->aic_pci_diagnostic &&
        (!qtest_enabled() || machine->ram_size != O2_RAM_SIZE ||
         s->banked_ram || s->rom_image || s->rom_size ||
         s->rom_offset != UINT32_MAX ||
         s->rom_pad_byte != UINT16_MAX)) {
        error_report("o2-test aic-pci-diagnostic requires QTest, 64 MiB "
                     "RAM and no ROM image");
        exit(1);
    }

    if (s->reset_low_ram_approx && !s->banked_ram) {
        error_report("o2-test reset-low-ram-approx requires "
                     "bank-profile=8x128m-synthetic");
        exit(1);
    }
    if (s->empty_renderer_status && !s->banked_ram) {
        error_report("o2-test empty-renderer-status requires "
                     "bank-profile=8x128m-synthetic");
        exit(1);
    }
    if (s->empty_renderer_status && !s->reset_low_ram_approx) {
        error_report("o2-test empty-renderer-status requires "
                     "reset-low-ram-approx=on");
        exit(1);
    }
    if (s->prom_private_window &&
        (!s->banked_ram || !s->reset_low_ram_approx ||
         !s->empty_renderer_status)) {
        error_report("o2-test prom-private-window requires the 1 GiB "
                     "eight-bank profile, reset-low-ram-approx=on and "
                     "empty-renderer-status=on");
        exit(1);
    }
    if (s->prom_id_word &&
        (!s->banked_ram || machine->ram_size != O2_RAM_BANKED_SIZE ||
         !s->reset_low_ram_approx || !s->empty_renderer_status ||
         !s->prom_private_window || !s->prom_sb64_reset ||
         !s->gbe11_registers || !s->rom_image || !s->rom_size ||
         s->rom_offset == UINT32_MAX ||
         s->rom_pad_byte == UINT16_MAX)) {
        error_report("o2-test prom-id-word requires the 1 GiB "
                     "eight-bank PROM diagnostic with reset-low-ram-approx=on, "
                     "empty-renderer-status=on, prom-private-window=on, "
                     "prom-sb64-reset=on, gbe11-registers=on and complete "
                     "ROM settings");
        exit(1);
    }
    if (s->gbe11_registers &&
        (!s->banked_ram || !s->reset_low_ram_approx ||
         !s->empty_renderer_status || !s->prom_private_window ||
         !s->prom_sb64_reset || !s->rom_image || !s->rom_size ||
         s->rom_offset == UINT32_MAX ||
         s->rom_pad_byte == UINT16_MAX)) {
        error_report("o2-test gbe11-registers requires the 1 GiB "
                     "eight-bank PROM diagnostic with reset-low-ram-approx=on, "
                     "empty-renderer-status=on, prom-private-window=on, "
                     "prom-sb64-reset=on and complete ROM settings");
        exit(1);
    }
    if (s->gbe_one_tile_diagnostic && !s->gbe11_registers) {
        error_report("o2-test gbe-one-tile-diagnostic requires "
                     "gbe11-registers=on and its 1 GiB PROM diagnostic "
                     "prerequisites");
        exit(1);
    }
    if (s->prom_sb64_reset &&
        (!s->banked_ram || !s->reset_low_ram_approx ||
         !s->empty_renderer_status || !s->prom_private_window ||
         !s->rom_image || !s->rom_size ||
         s->rom_offset == UINT32_MAX ||
         s->rom_pad_byte == UINT16_MAX)) {
        error_report("o2-test prom-sb64-reset requires the 1 GiB "
                     "eight-bank profile, reset-low-ram-approx=on, "
                     "empty-renderer-status=on, prom-private-window=on "
                     "and the ROM image/size/offset/pad settings");
        exit(1);
    }
    if (s->banked_ram) {
        if (machine->ram_size != O2_RAM_BANKED_SIZE) {
            error_report("o2-test eight-bank profile requires 1 GiB of RAM");
            exit(1);
        }
    } else if (machine->ram_size != O2_RAM_SIZE &&
               machine->ram_size != O2_RAM_OPTIONAL_SIZE) {
        error_report("o2-test requires 64 or 128 MiB of RAM");
        exit(1);
    }
    if (machine->ram_slots || machine->maxram_size != machine->ram_size) {
        error_report("o2-test does not support memory slots or extra RAM");
        exit(1);
    }
    if (machine->smp.cpus != 1) {
        error_report("o2-test requires one CPU");
        exit(1);
    }
    if (machine->usb) {
        error_report("o2-test does not support USB devices");
        exit(1);
    }
    if (serial_hd(1)) {
        error_report("o2-test supports only one host serial backend");
        exit(1);
    }
    if (machine->firmware) {
        error_report("o2-test does not support -bios");
        exit(1);
    }
    if (machine->kernel_filename) {
        error_report("o2-test does not support -kernel");
        exit(1);
    }
    if (machine->initrd_filename) {
        error_report("o2-test does not support -initrd");
        exit(1);
    }
    if (machine->kernel_cmdline && machine->kernel_cmdline[0]) {
        error_report("o2-test does not support -append");
        exit(1);
    }
    if (s->banked_ram && s->rom_image && !s->reset_low_ram_approx) {
        error_report("o2-test eight-bank profile does not support ROM images");
        exit(1);
    }
    if (s->rom_at29c040a &&
        (!s->rom_image || s->rom_size != 512 * KiB || s->rom_offset != 0 ||
         !s->rom_at29_protection || !s->rom_at29_lower_lock ||
         !s->rom_at29_upper_lock)) {
        error_report("o2-test rom-at29c040a requires a full 512 KiB ROM "
                     "image at offset zero and explicit protection and "
                     "boot-block lock states");
        exit(1);
    }
    if (!s->rom_at29c040a &&
        (s->rom_at29_protection || s->rom_at29_lower_lock ||
         s->rom_at29_upper_lock)) {
        error_report("o2-test AT29C040A states require rom-at29c040a=on");
        exit(1);
    }
    if (!s->rom_image && (s->rom_size || s->rom_offset != UINT32_MAX ||
                          s->rom_pad_byte != UINT16_MAX)) {
        error_report("o2-test ROM settings require rom-image");
        exit(1);
    }
    if (s->rom_image && (!s->rom_size || s->rom_offset == UINT32_MAX ||
                         s->rom_pad_byte == UINT16_MAX)) {
        error_report("o2-test ROM requires rom-size, rom-offset and "
                     "rom-pad-byte");
        exit(1);
    }
    if (s->reset_low_ram_approx && s->rom_image &&
        g_stat(s->rom_image, &image_stat) == 0 &&
        !S_ISREG(image_stat.st_mode)) {
        error_report("o2-test ROM image must be a regular file");
        exit(1);
    }
    if (s->banked_ram) {
        if (vmstate_register_any(NULL, &vmstate_o2_banked, s) < 0) {
            error_report("o2-test cannot register eight-bank migration state");
            exit(1);
        }
        s->vmstate_registered = true;
        error_setg(&s->migration_blocker,
                   "O2 eight-bank profile does not support migration");
        if (migrate_add_blocker(&s->migration_blocker, &err) < 0) {
            error_report_err(err);
            exit(1);
        }
    } else if (s->aic_pci_diagnostic) {
        error_setg(&s->migration_blocker,
                   "O2 AIC PCI diagnostic does not support migration");
        if (migrate_add_blocker(&s->migration_blocker, &err) < 0) {
            error_report_err(err);
            exit(1);
        }
    }

    /* Catalogued speed; external SysClk and reset straps are unknown. */
    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, 195 * 1000 * 1000);
    s->cpuclk = cpuclk;
    memory_region_init(&s->cpu_uncached, OBJECT(machine),
                       "o2.cpu-uncached", UINT64_MAX);
    memory_region_init_alias(&s->cpu_physical, OBJECT(machine),
                             "o2.cpu-physical", get_system_memory(),
                             0, 1ULL << MIPS_UNCACHED_WINDOW_SHIFT);
    memory_region_add_subregion(&s->cpu_uncached, 0, &s->cpu_physical);
    if (!s->banked_ram) {
        /* The flat diagnostic fits do not decode bank controls. */
        o2_map_uncached_flat_ram(s, machine->ram, machine->ram_size);
    }
    cpu_dev = qdev_new(machine->cpu_type);
    if (object_property_get_bool(OBJECT(cpu_dev),
                                 "cache-data-visibility", &error_fatal)) {
        error_report("o2-test requires cache-data-visibility=off");
        exit(1);
    }
    qdev_connect_clock_in(cpu_dev, "clk-in", cpuclk);
    object_property_set_bool(OBJECT(cpu_dev), "big-endian", true,
                             &error_fatal);
    if (s->prom_sb64_reset) {
        if (MIPS_CPU(cpu_dev)->r10k_profile->reset_mode_set) {
            error_report("o2-test prom-sb64-reset rejects CPU reset-mode");
            exit(1);
        }
        object_property_set_uint(OBJECT(cpu_dev), "reset-mode",
                                 O2_PROM_SB64_RESET_MODE, &error_fatal);
    }
    if (s->prom_private_window) {
        memory_region_init(&s->cpu_cached, OBJECT(machine),
                           "o2.cpu-cached", UINT64_MAX);
        memory_region_init_alias(&s->cpu_cached_physical, OBJECT(machine),
                                 "o2.cpu-cached.physical",
                                 get_system_memory(), 0, UINT64_MAX);
        memory_region_add_subregion(&s->cpu_cached, 0,
                                    &s->cpu_cached_physical);
        memory_region_init_io(&s->private_filter, OBJECT(machine),
                              &o2_private_ops, s,
                              "o2.prom-private-window", O2_PRIVATE_SIZE);
        memory_region_add_subregion_overlap(&s->cpu_cached, 0,
                                            &s->private_filter, 1);
        object_property_set_link(OBJECT(cpu_dev), "memory",
                                 OBJECT(&s->cpu_cached), &error_fatal);
        s->private_cpu = MIPS_CPU(cpu_dev);
        s->private_armed = true;
    }
    object_property_set_link(OBJECT(cpu_dev), "uncached-memory",
                             OBJECT(&s->cpu_uncached), &error_fatal);
    if (!qdev_realize_and_unref(cpu_dev, NULL, &err)) {
        error_report_err(err);
        exit(1);
    }
    cpu = MIPS_CPU(cpu_dev);
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);
    if (s->prom_private_window) {
        s->private_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        o2_private_poll, s);
        timer_mod(s->private_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  O2_PRIVATE_POLL_NS);
    }

    crime = qdev_new(TYPE_SGI_CRIME);
    object_property_add_child(OBJECT(machine), "crime", OBJECT(crime));
    qdev_prop_set_uint8(crime, "interface-revision", 1);
    qdev_prop_set_bit(crime, "eight-128m-banks", s->banked_ram);
    qdev_prop_set_bit(crime, "reset-low-ram-approx",
                      s->reset_low_ram_approx);
    qdev_prop_set_bit(crime, "empty-renderer-status",
                      s->empty_renderer_status);
    qdev_prop_set_bit(crime, "prom-id-word", s->prom_id_word);
    if (s->banked_ram) {
        /* CRIME detaches its UC=0 aliases on unrealize. */
        object_property_set_link(OBJECT(crime), "decoded-carrier",
                                 OBJECT(&s->cpu_uncached), &error_fatal);
        object_property_set_uint(OBJECT(crime), "decoded-base",
                                 1ULL << MIPS_UNCACHED_WINDOW_SHIFT,
                                 &error_fatal);
    }
    object_property_set_link(OBJECT(crime), "ram", OBJECT(machine->ram),
                             &error_fatal);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(crime), &err)) {
        error_report_err(err);
        exit(1);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(crime), 0, O2_CRIME_BASE);
    for (unsigned ip = 0; ip < 5; ip++) {
        qdev_connect_gpio_out_named(crime, "cpu-ip", ip,
                                    cpu->env.irq[ip + 2]);
    }

    if (s->gbe11_registers) {
        gbe = qdev_new("sgi-o2-gbe");
        object_property_add_child(OBJECT(machine), "gbe", OBJECT(gbe));
        if (s->gbe_one_tile_diagnostic) {
            qdev_prop_set_bit(gbe, "synthetic-single-tile", true);
            object_property_set_link(
                OBJECT(gbe), "ram",
                OBJECT(&SGI_CRIME(crime)->decoded_ram), &error_fatal);
        }
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(gbe), &err)) {
            error_report_err(err);
            exit(1);
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(gbe), 0, O2_GBE_BASE);
    }

    mace = qdev_new("sgi-mace");
    qdev_prop_set_bit(mace, "phy-ics1890", s->phy_ics1890);
    qdev_prop_set_uint8(mace, "phy-address", s->phy_address);
    qdev_prop_set_bit(mace, "identity-diagnostic", s->identity_diagnostic);
    qdev_prop_set_bit(mace, "misc-word-pio", s->mace_misc_word_pio);
    qdev_prop_set_bit(mace, "mace-eth-low-dma-compat",
                     s->mace_eth_low_dma_compat);
    if (s->identity_diagnostic) {
        qdev_prop_set_string(mace, "identity-test-serial",
                             s->identity_test_serial);
        qdev_prop_set_string(mace, "identity-eprom-file",
                             s->identity_eprom_file);
        qdev_prop_set_string(mace, "identity-status", s->identity_status);
    }
    object_property_add_child(OBJECT(machine), "mace", OBJECT(mace));
    qdev_prop_set_bit(mace, "aic-pci-diagnostic", s->aic_pci_diagnostic);
    qdev_prop_set_bit(mace, "pci-native-word-swap",
                     !s->aic_pci_diagnostic &&
                     !s->diagnostic_lower_pci_byte_identity);
    if (s->diagnostic_lower_pci_byte_identity) {
        sgi_mace_enable_storage_diagnostic(mace, &err);
        if (err) {
            error_report_err(err);
            exit(1);
        }
    }
    object_property_set_link(OBJECT(mace), "ram",
        s->banked_ram ? OBJECT(&SGI_CRIME(crime)->decoded_ram) :
                        OBJECT(machine->ram), &error_fatal);
    if (serial_hd(0)) {
        qdev_prop_set_chr(mace, "chardev-a", serial_hd(0));
    }
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(mace), &err)) {
        error_report_err(err);
        exit(1);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(mace), 0, O2_MACE_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(mace), 1, O2_AIC_PCI_VIEW);
    sysbus_mmio_map(SYS_BUS_DEVICE(mace), 2, O2_PCI_HIGH_VIEW);
    for (unsigned cause = 0; cause < 16; cause++) {
        qdev_connect_gpio_out_named(mace, "crime", cause,
                                    qdev_get_gpio_in_named(crime, "mace",
                                                           cause));
    }
    s->pci_bus = PCI_HOST_BRIDGE(mace)->bus;
    if (s->mace_bank_dma_fixture) {
        s->mace_dma_card = pci_new(PCI_DEVFN(3, 0), "sgi-bridge-dma-test");
        object_property_add_child(OBJECT(machine), "mace-dma-fixture",
                                  OBJECT(s->mace_dma_card));
        qdev_prop_set_bit(DEVICE(s->mace_dma_card), "dma-r32", false);
        object_property_set_link(OBJECT(s->mace_dma_card), "dma-memory",
                                 NULL, &error_fatal);
        object_property_set_bool(OBJECT(s->mace_dma_card), "pci-only-latch",
                                 true, &error_fatal);
        if (!pci_realize_and_unref(s->mace_dma_card, s->pci_bus, &err)) {
            error_report_err(err);
            exit(1);
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        s->aic[i] = pci_new(PCI_DEVFN(i + 1, 0), "aic7880");
        DEVICE(s->aic[i])->id = g_strdup_printf("aic%u", i);
        if (s->aic_pci_diagnostic) {
            qdev_prop_set_bit(DEVICE(s->aic[i]),
                              "o2-pci-diagnostic", true);
        }
        if (s->diagnostic_lower_pci_byte_identity) {
            qdev_prop_set_bit(DEVICE(s->aic[i]),
                              "o2-storage-diagnostic", true);
            aic7880_set_storage_diagnostic_reporter(s->aic[i],
                                                    o2_storage_report, s);
        }
        if (!pci_realize_and_unref(s->aic[i], s->pci_bus, &err)) {
            error_report_err(err);
            exit(1);
        }
    }
    s->machine_done.notify = o2_machine_init_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
    s->machine_done_registered = true;

    if (s->rom_image) {
        rom = qdev_new(TYPE_SGI_O2_ROM);
        object_property_add_child(OBJECT(machine), "rom", OBJECT(rom));
        qdev_prop_set_bit(rom, "mace-controlled", true);
        qdev_prop_set_bit(rom, "at29c040a-diagnostic", s->rom_at29c040a);
        if (s->rom_at29c040a) {
            qdev_prop_set_string(rom, "at29c040a-protection",
                                 s->rom_at29_protection);
            qdev_prop_set_string(rom, "at29c040a-lower-lock",
                                 s->rom_at29_lower_lock);
            qdev_prop_set_string(rom, "at29c040a-upper-lock",
                                 s->rom_at29_upper_lock);
        }
        qdev_prop_set_string(rom, "image", s->rom_image);
        qdev_prop_set_uint32(rom, "flash-size", s->rom_size);
        qdev_prop_set_uint32(rom, "image-offset", s->rom_offset);
        qdev_prop_set_uint16(rom, "pad-byte", s->rom_pad_byte);
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(rom), &err)) {
            error_report_err(err);
            exit(1);
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(rom), 0, O2_ROM_BASE);
        qdev_connect_gpio_out_named(mace, "flash-write-enable", 0,
            qdev_get_gpio_in_named(rom, "flash-write-enable", 0));
    }
}

static void o2_reset(MachineState *machine, ResetType type)
{
    O2MachineState *s = (O2MachineState *)machine;
    CPUState *cpu;

    /* The loader may select a RAM entry point in its device reset callback. */
    CPU_FOREACH(cpu) {
        cpu_reset(cpu);
    }
    qemu_devices_reset(type);
    if (s->prom_private_window) {
        o2_private_reset(s);
    }
}

static bool o2_hotplug_allowed(MachineState *machine, DeviceState *dev,
                               Error **errp)
{
    O2MachineState *s = (O2MachineState *)machine;

    if (((s->aic_pci_diagnostic || s->mace_bank_dma_fixture ||
          s->diagnostic_lower_pci_byte_identity) &&
         object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) ||
        ((s->aic_pci_diagnostic || s->mace_bank_dma_fixture ||
          s->diagnostic_lower_pci_byte_identity) &&
         object_dynamic_cast(OBJECT(dev), TYPE_SCSI_DEVICE))) {
        error_setg(errp, "o2-test %s rejects PCI and SCSI hotplug",
                   s->aic_pci_diagnostic ? "AIC PCI diagnostic" :
                   s->diagnostic_lower_pci_byte_identity ?
                   "storage diagnostic" : "MACE bank DMA fixture");
        return false;
    }
    return true;
}

static void o2_class_init(ObjectClass *oc, const void *data)
{
    static const char * const cpu_types[] = {
        MIPS_CPU_TYPE_NAME("R10000"), NULL,
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "rom-image", o2_get_rom_image,
                                  o2_set_rom_image);
    object_class_property_add_bool(oc, "rom-at29c040a",
                                   o2_get_rom_at29c040a,
                                   o2_set_rom_at29c040a);
    object_class_property_add_str(oc, "rom-at29c040a-protection",
                                  o2_get_rom_at29_protection,
                                  o2_set_rom_at29_protection);
    object_class_property_add_str(oc, "rom-at29c040a-lower-lock",
                                  o2_get_rom_at29_lower_lock,
                                  o2_set_rom_at29_lower_lock);
    object_class_property_add_str(oc, "rom-at29c040a-upper-lock",
                                  o2_get_rom_at29_upper_lock,
                                  o2_set_rom_at29_upper_lock);
    object_class_property_add_str(oc, "bank-profile", o2_get_bank_profile,
                                  o2_set_bank_profile);
    object_class_property_add_bool(oc, "reset-low-ram-approx",
                                   o2_get_reset_low_ram_approx,
                                   o2_set_reset_low_ram_approx);
    object_class_property_add_bool(oc, "empty-renderer-status",
                                   o2_get_empty_renderer_status,
                                   o2_set_empty_renderer_status);
    object_class_property_add_bool(oc, "prom-private-window",
                                   o2_get_prom_private_window,
                                   o2_set_prom_private_window);
    object_class_property_add_bool(oc, "prom-sb64-reset",
                                   o2_get_prom_sb64_reset,
                                   o2_set_prom_sb64_reset);
    object_class_property_add_bool(oc, "gbe11-registers",
                                   o2_get_gbe11_registers,
                                   o2_set_gbe11_registers);
    object_class_property_add_bool(oc, "gbe-one-tile-diagnostic",
                                   o2_get_gbe_one_tile_diagnostic,
                                   o2_set_gbe_one_tile_diagnostic);
    object_class_property_add_bool(oc, "phy-ics1890",
                                   o2_get_phy_ics1890, o2_set_phy_ics1890);
    object_class_property_add(oc, "phy-address", "uint8",
                              o2_get_phy_address, o2_set_phy_address,
                              NULL, NULL);
    object_class_property_add_bool(oc, "aic-pci-diagnostic",
                                   o2_get_aic_pci_diagnostic,
                                   o2_set_aic_pci_diagnostic);
    object_class_property_add_bool(oc, "diagnostic-lower-pci-byte-identity",
                                   o2_get_diagnostic_lower_pci_byte_identity,
                                   o2_set_diagnostic_lower_pci_byte_identity);
    object_class_property_add_str(oc, "storage-diagnostic-event-file",
                                  o2_get_storage_event_file,
                                  o2_set_storage_event_file);
    object_class_property_add_bool(oc, "x-test-storage-event-short-write",
                                   o2_get_storage_event_short_write,
                                   o2_set_storage_event_short_write);
    object_class_property_add_bool(oc, "x-test-storage-final-sync-fail",
                                   o2_get_storage_event_final_sync_fail,
                                   o2_set_storage_event_final_sync_fail);
    object_class_property_add_bool(oc, "mace-bank-dma-fixture",
                                   o2_get_mace_bank_dma_fixture,
                                   o2_set_mace_bank_dma_fixture);
    object_class_property_add_bool(oc, "identity-diagnostic",
                                   o2_get_identity_diagnostic,
                                   o2_set_identity_diagnostic);
    object_class_property_add_bool(oc, "mace-misc-word-pio",
                                   o2_get_mace_misc_word_pio,
                                   o2_set_mace_misc_word_pio);
    object_class_property_add_bool(oc, "mace-eth-low-dma-compat",
                                   o2_get_mace_eth_low_dma_compat,
                                   o2_set_mace_eth_low_dma_compat);
    object_class_property_add_str(oc, "identity-test-serial",
                                  o2_get_identity_test_serial,
                                  o2_set_identity_test_serial);
    object_class_property_add_str(oc, "identity-eprom-file",
                                  o2_get_identity_eprom_file,
                                  o2_set_identity_eprom_file);
    object_class_property_add_str(oc, "identity-status",
                                  o2_get_identity_status,
                                  o2_set_identity_status);
    object_class_property_add_bool(oc, "prom-id-word",
                                   o2_get_prom_id_word,
                                   o2_set_prom_id_word);
    object_class_property_add(oc, "cpu-clock-hz", "uint64",
                              o2_get_cpu_clock_hz, NULL, NULL, NULL);
    object_class_property_add(oc, "rom-size", "uint32",
                              o2_get_rom_uint32, o2_set_rom_uint32,
                              NULL, NULL);
    object_class_property_add(oc, "rom-offset", "uint32",
                              o2_get_rom_uint32, o2_set_rom_uint32,
                              NULL, NULL);
    object_class_property_add(oc, "rom-pad-byte", "uint16",
                              o2_get_rom_pad_byte, o2_set_rom_pad_byte,
                              NULL, NULL);
    mc->desc = "O2 (IP32) synthetic register test profile";
    mc->init = o2_init;
    mc->reset = o2_reset;
    mc->hotplug_allowed = o2_hotplug_allowed;
    mc->valid_cpu_types = cpu_types;
    mc->default_cpu_type = cpu_types[0];
    mc->default_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = O2_RAM_SIZE;
    mc->default_ram_id = "o2.ram";
    mc->default_nic = "sgi-mace-eth";
    mc->default_display = "none";
    mc->no_serial = true;
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->auto_create_sdcard = false;
}

static const TypeInfo o2_machine_type = {
    .name = MACHINE_TYPE_NAME("o2-test"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(O2MachineState),
    .instance_init = o2_instance_init,
    .instance_finalize = o2_instance_finalize,
    .class_init = o2_class_init,
};

static void o2_machine_register(void)
{
    type_register_static(&o2_machine_type);
}

type_init(o2_machine_register)
