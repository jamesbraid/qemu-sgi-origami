// SPDX-License-Identifier: BSD-3-Clause

use std::ffi::c_void;

use util::prelude::*;

::trace::include_trace!("hw_mips_sgi");

const FLASH_ENABLE: u64 = 1;
const PASSWORD_CLEAR: u64 = 1 << 1;
const RELEASE: u64 = 1 << 2;
const SAMPLE: u64 = 1 << 3;
const RED_LED: u64 = 1 << 4;
const GREEN_LED: u64 = 1 << 5;
const WRITABLE: u64 = FLASH_ENABLE | RELEASE | RED_LED | GREEN_LED;
const RESET_MIN_NS: i64 = 480_000;
const RESET_MAX_NS: i64 = 960_000;
const PRESENCE_DELAY_NS: i64 = 30_000;
const PRESENCE_END_NS: i64 = 150_000;
const DIAG_ONE_MAX_NS: i64 = 24_000;
const SLOT_NS: i64 = 60_000;
const WRITE_ZERO_NS: i64 = 60_000;
const SLOT_MAX_NS: i64 = 120_000;
const MMIO_DECODE_ERROR: i32 = -1;
const WRITE_NOOP: i32 = -2;

unsafe extern "C" {
    fn onewire_reset(bus: *mut c_void) -> bool;
    fn onewire_touch_bit(bus: *mut c_void, drive: bool) -> bool;
}

#[repr(C)]
pub struct MigrationState {
    pub password_clear_input: bool,
    pub flash_write_enabled: bool,
    pub released: bool,
    pub led_control: u32,
}
const _: () = assert!(std::mem::size_of::<MigrationState>() == 8);

#[repr(C)]
struct Core {
    bus: *mut c_void,
    part: bool,
    misc_word_pio: bool,
    password_clear_input: bool,
    flash_write_enabled: bool,
    released: bool,
    led_control: u32,
    low_start_ns: i64,
    startup_low_start_ns: i64,
    high_start_ns: i64,
    presence_start_ns: i64,
    presence_end_ns: i64,
    sample_start_ns: i64,
    sample_end_ns: i64,
    high_seen: bool,
    reset_ready: bool,
    startup_pending: bool,
    startup_low_valid: bool,
    session: bool,
    slot_pull: bool,
}

impl Core {
    fn clear_windows(&mut self) {
        self.presence_start_ns = 0;
        self.presence_end_ns = 0;
        self.sample_start_ns = 0;
        self.sample_end_ns = 0;
        self.slot_pull = false;
    }

    fn presence(&self, now: i64) -> bool {
        self.session && now >= self.presence_start_ns && now < self.presence_end_ns
    }

    fn sample_pull(&self, now: i64) -> bool {
        self.slot_pull && now >= self.sample_start_ns && now < self.sample_end_ns
    }

    fn value(&self, now: i64) -> u64 {
        let level = self.part && self.released && !self.presence(now) && !self.sample_pull(now);
        u64::from(self.led_control)
            | (if self.password_clear_input {
                PASSWORD_CLEAR
            } else {
                0
            })
            | (if self.flash_write_enabled {
                FLASH_ENABLE
            } else {
                0
            })
            | (if self.released { RELEASE } else { 0 })
            | (if level { SAMPLE } else { 0 })
    }

    fn reset(&mut self) {
        self.clear_windows();
        self.released = false;
        self.flash_write_enabled = false;
        self.led_control = GREEN_LED as u32;
        self.high_seen = false;
        self.reset_ready = false;
        self.startup_pending = true;
        self.startup_low_valid = false;
        self.session = false;
        self.low_start_ns = 0;
        self.startup_low_start_ns = 0;
        self.high_start_ns = 0;
        unsafe {
            onewire_reset(self.bus);
        }
    }

    fn write(&mut self, now: i64, value: u64, offset: u64, size: u32) {
        let release = value & RELEASE != 0;
        if value & !WRITABLE != 0 {
            let bits = value & !WRITABLE;
            log_mask_ln!(
                Log::Unimp,
                "sgi-mace-id: unsupported identity control bits 0x{bits:x}"
            );
        }
        self.led_control = (value & (RED_LED | GREEN_LED)) as u32;
        self.flash_write_enabled = value & FLASH_ENABLE != 0;
        if !release && self.startup_pending && !self.startup_low_valid {
            self.startup_low_valid = true;
            self.startup_low_start_ns = now;
        }
        if release == self.released {
            return;
        }
        let duration = if release {
            if self.high_seen {
                now - self.low_start_ns
            } else {
                -1
            }
        } else {
            now - self.high_start_ns
        };
        trace::trace_mace_id_drive(now, offset, size, release, duration);
        self.released = release;
        if !release {
            self.reset_ready = self.high_seen && now - self.high_start_ns >= RESET_MIN_NS;
            self.low_start_ns = now;
            return;
        }

        self.high_start_ns = now;
        if self.startup_pending {
            let startup_low_ns = if self.startup_low_valid {
                now - self.startup_low_start_ns
            } else {
                -1
            };
            self.startup_pending = false;
            self.startup_low_valid = false;
            if !self.high_seen {
                self.high_seen = true;
            }
            if startup_low_ns >= RESET_MIN_NS {
                self.clear_windows();
                self.session = false;
                unsafe {
                    onewire_reset(self.bus);
                }
                self.session = true;
                self.presence_start_ns = now + PRESENCE_DELAY_NS;
                self.presence_end_ns = now + PRESENCE_END_NS;
                trace::trace_mace_id_presence_schedule(
                    now,
                    self.presence_start_ns,
                    self.presence_end_ns,
                );
            }
            return;
        }
        if !self.high_seen {
            self.high_seen = true;
            return;
        }
        let low_ns = now - self.low_start_ns;
        trace::trace_mace_id_reset(
            now,
            low_ns,
            self.reset_ready,
            (RESET_MIN_NS..RESET_MAX_NS).contains(&low_ns) && self.reset_ready,
        );
        if low_ns >= RESET_MIN_NS {
            self.clear_windows();
            self.session = false;
            unsafe {
                onewire_reset(self.bus);
            }
            if low_ns < RESET_MAX_NS && self.reset_ready {
                self.session = true;
                self.presence_start_ns = now + PRESENCE_DELAY_NS;
                self.presence_end_ns = now + PRESENCE_END_NS;
                trace::trace_mace_id_presence_schedule(
                    now,
                    self.presence_start_ns,
                    self.presence_end_ns,
                );
            }
            return;
        }
        if !self.session || now < self.presence_end_ns {
            return;
        }
        if (WRITE_ZERO_NS..SLOT_MAX_NS).contains(&low_ns) {
            unsafe {
                onewire_touch_bit(self.bus, false);
            }
        } else if (1_000..DIAG_ONE_MAX_NS).contains(&low_ns) {
            self.slot_pull = !unsafe { onewire_touch_bit(self.bus, true) };
            self.sample_start_ns = now;
            self.sample_end_ns = self.low_start_ns + SLOT_NS;
        } else {
            log_mask_ln!(Log::Unimp, "sgi-mace-id: unsupported low pulse {low_ns} ns");
        }
    }
}

#[no_mangle]
/// # Safety
/// `bus` must be a live one-wire bus that outlives the returned state.
pub unsafe extern "C" fn sgi_mace_id_new(
    bus: *mut c_void,
    part: bool,
    misc_word_pio: bool,
    password_clear_input: bool,
) -> *mut c_void {
    Box::into_raw(Box::new(Core {
        bus,
        part,
        misc_word_pio,
        password_clear_input,
        flash_write_enabled: false,
        released: false,
        led_control: GREEN_LED as u32,
        low_start_ns: 0,
        startup_low_start_ns: 0,
        high_start_ns: 0,
        presence_start_ns: 0,
        presence_end_ns: 0,
        sample_start_ns: 0,
        sample_end_ns: 0,
        high_seen: false,
        reset_ready: false,
        startup_pending: false,
        startup_low_valid: false,
        session: false,
        slot_pull: false,
    }))
    .cast()
}

#[no_mangle]
/// # Safety
/// `core` must be a live state returned by `sgi_mace_id_new`, with no other
/// owner retaining it after this call.
pub unsafe extern "C" fn sgi_mace_id_free(core: *mut c_void) {
    if !core.is_null() {
        drop(unsafe { Box::from_raw(core.cast::<Core>()) });
    }
}

#[no_mangle]
/// # Safety
/// `core` must be live and exclusively accessed while QEMU holds the BQL.
pub unsafe extern "C" fn sgi_mace_id_set_input(core: *mut c_void, level: bool) {
    unsafe {
        (*core.cast::<Core>()).password_clear_input = level;
    }
}

#[no_mangle]
/// # Safety
/// `core` must be live and exclusively accessed while QEMU holds the BQL.
pub unsafe extern "C" fn sgi_mace_id_reset(core: *mut c_void) -> bool {
    let state = unsafe { &mut *core.cast::<Core>() };
    state.reset();
    state.flash_write_enabled
}

#[no_mangle]
/// # Safety
/// `core` must be live; `value` must point to a writable `u64` and not overlap
/// `core`. QEMU holds the BQL for this call.
pub unsafe extern "C" fn sgi_mace_id_read(
    core: *mut c_void,
    now: i64,
    offset: u64,
    size: u32,
    value: *mut u64,
) -> i32 {
    let state = unsafe { &*core.cast::<Core>() };
    let result = unsafe { &mut *value };
    *result = 0;
    if state.misc_word_pio && size == 4 && offset == 0 {
        *result = 0;
    } else if (size == 8 && offset == 0) || (state.misc_word_pio && size == 4 && offset == 4) {
        *result = state.value(now)
            & if size == 4 {
                u64::from(u32::MAX)
            } else {
                u64::MAX
            };
    } else {
        return MMIO_DECODE_ERROR;
    }
    trace::trace_mace_id_sample(
        now,
        offset,
        size,
        *result & SAMPLE != 0,
        state.presence(now),
    );
    0
}

#[no_mangle]
/// # Safety
/// `core` must be live and exclusively accessed while QEMU holds the BQL.
pub unsafe extern "C" fn sgi_mace_id_write(
    core: *mut c_void,
    now: i64,
    offset: u64,
    size: u32,
    value: u64,
) -> i32 {
    let state = unsafe { &mut *core.cast::<Core>() };
    if state.misc_word_pio && size == 4 && offset == 0 {
        if value != 0 {
            log_mask_ln!(
                Log::Unimp,
                "sgi-mace-id: unsupported nonzero Misc high word 0x{value:x}"
            );
        }
        return WRITE_NOOP;
    }
    if !((size == 8 && offset == 0) || (state.misc_word_pio && size == 4 && offset == 4)) {
        return MMIO_DECODE_ERROR;
    }
    state.write(now, value, offset, size);
    i32::from(state.flash_write_enabled)
}

#[no_mangle]
/// # Safety
/// `core` must be live; `out` must point to writable migration state and not
/// overlap `core`. QEMU holds the BQL for this call.
pub unsafe extern "C" fn sgi_mace_id_export(core: *mut c_void, out: *mut MigrationState) {
    let state = unsafe { &*core.cast::<Core>() };
    unsafe {
        *out = MigrationState {
            password_clear_input: state.password_clear_input,
            flash_write_enabled: state.flash_write_enabled,
            released: state.released,
            led_control: state.led_control,
        };
    }
}

#[no_mangle]
/// # Safety
/// `core` must be live and exclusively accessed, and `input` must point to
/// readable, non-overlapping migration state; QEMU holds the BQL for this call.
pub unsafe extern "C" fn sgi_mace_id_import(
    core: *mut c_void,
    input: *const MigrationState,
) -> bool {
    let state = unsafe { &mut *core.cast::<Core>() };
    let input = unsafe { &*input };
    state.password_clear_input = input.password_clear_input;
    state.flash_write_enabled = input.flash_write_enabled;
    state.released = input.released;
    state.led_control = input.led_control;
    state.flash_write_enabled
}
