// SPDX-License-Identifier: BSD-3-Clause

use std::ffi::c_void;

use util::prelude::*;

::trace::include_trace!("hw_mips_sgi");

const POST_TX: u8 = 0x01;
const TX_ENABLE: u8 = 0x02;
const TX_IRQ_ENABLE: u8 = 0x04;
const RX_IRQ_ENABLE: u8 = 0x08;
const CLOCK_ENABLE: u8 = 0x10;
const RESET: u8 = 0x20;
const TX_EMPTY: u8 = 0x08;
const RX_FULL: u8 = 0x10;
const PARITY_ERROR: u8 = 0x40;
const FRAMING_ERROR: u8 = 0x80;

pub const ACTION_NONE: i32 = 0;
pub const ACTION_POP_RX: i32 = 1;
pub const ACTION_SEND_KEYBOARD: i32 = 2;
pub const ACTION_SEND_MOUSE: i32 = 3;
pub const ACTION_DROP_MOUSE_STREAM: i32 = 4;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MigrationPort {
    control: u8,
    tx: u8,
    rx: u8,
    errors: u8,
    tx_pending: bool,
    rx_full: bool,
    inhibited: bool,
    reply: u16,
}

const _: () = assert!(std::mem::size_of::<MigrationPort>() == 10);
const _: () = assert!(std::mem::offset_of!(MigrationPort, reply) == 8);

#[derive(Clone, Copy)]
struct Port {
    control: u8,
    tx: u8,
    rx: u8,
    errors: u8,
    tx_pending: bool,
    rx_full: bool,
    inhibited: bool,
    reply: u16,
}

impl Port {
    fn new() -> Self {
        Self {
            control: CLOCK_ENABLE,
            tx: 0,
            rx: 0,
            errors: 0,
            tx_pending: false,
            rx_full: false,
            inhibited: false,
            reply: 0,
        }
    }

    fn clock_permitted(&self) -> bool {
        self.control & (CLOCK_ENABLE | RESET) == CLOCK_ENABLE && !self.inhibited && !self.rx_full
    }

    fn status(&self) -> u8 {
        self.errors
            | if self.clock_permitted() { 3 } else { 0 }
            | if self.control & RESET == 0 && !self.tx_pending {
                TX_EMPTY
            } else {
                0
            }
            | if self.rx_full { RX_FULL } else { 0 }
    }

    fn clear(&mut self) {
        self.tx = 0;
        self.rx = 0;
        self.errors = 0;
        self.tx_pending = false;
        self.rx_full = false;
        self.inhibited = false;
    }

    fn receive(&mut self) -> u16 {
        let value = u16::from(self.rx) | (u16::from(self.status()) << 8);
        self.rx_full = false;
        value
    }

    fn receive_frame(&mut self, data: u8, start: bool, parity: bool, stop: bool) -> bool {
        if !self.clock_permitted() || self.tx_pending {
            return false;
        }
        self.rx = data;
        self.errors = if (data.count_ones() + u32::from(parity)) & 1 == 0 {
            PARITY_ERROR
        } else {
            0
        } | if start || !stop { FRAMING_ERROR } else { 0 };
        self.rx_full = true;
        true
    }

    fn migration(&self) -> MigrationPort {
        MigrationPort {
            control: self.control,
            tx: self.tx,
            rx: self.rx,
            errors: self.errors,
            tx_pending: self.tx_pending,
            rx_full: self.rx_full,
            inhibited: self.inhibited,
            reply: self.reply,
        }
    }

    fn restore(&mut self, state: MigrationPort) {
        self.control = state.control;
        self.tx = state.tx;
        self.rx = state.rx;
        self.errors = state.errors;
        self.tx_pending = state.tx_pending;
        self.rx_full = state.rx_full;
        self.inhibited = state.inhibited;
        self.reply = state.reply;
    }

    fn valid(&self) -> bool {
        self.control & !0x3f == 0
            && self.errors & !0xc0 == 0
            && (!self.inhibited || self.control & POST_TX != 0)
            && (self.control & RESET == 0
                || !(self.tx_pending || self.rx_full || self.inhibited || self.errors != 0))
    }
}

struct Core {
    port: [Port; 2],
}

impl Core {
    fn new() -> Self {
        Self {
            port: [Port::new(), Port::new()],
        }
    }

    fn read(&mut self, addr: u64, size: u32) -> (u64, bool) {
        let port_num = ((addr >> 5) & 1) as usize;
        let port = &mut self.port[port_num];
        if size != 8 || addr & 7 != 0 {
            Self::unsupported_read(addr, size);
            return (0, false);
        }
        match addr & 0x18 {
            8 => {
                let had_full = port.rx_full;
                let value = if had_full {
                    u64::from(port.receive())
                } else {
                    u64::from(port.rx) | (u64::from(port.status()) << 8)
                };
                trace::trace_mace_ps2_read(
                    port_num as u32,
                    value,
                    had_full,
                    port.rx_full,
                    port.errors,
                );
                (value, had_full)
            }
            16 => (u64::from(port.control), false),
            24 => (u64::from(port.status()), false),
            _ => {
                Self::unsupported_read(addr, size);
                (0, false)
            }
        }
    }

    fn unsupported_read(addr: u64, size: u32) {
        log_mask_ln!(
            Log::GuestError,
            "sgi-mace-ps2: unsupported read at 0x{addr:x} size {size}"
        );
    }

    fn write(&mut self, addr: u64, value: u64, size: u32) -> bool {
        let port_num = ((addr >> 5) & 1) as usize;
        let port = &mut self.port[port_num];
        if size != 8 || addr & 7 != 0 {
            Self::unsupported_write(addr, value, size);
            return false;
        }
        let writable = match addr & 0x18 {
            0 if !port.tx_pending && port.control & RESET == 0 => {
                port.tx = value as u8;
                port.tx_pending = true;
                0xff
            }
            16 => {
                port.control = (value & 0x3f) as u8;
                if port.control & RESET != 0 {
                    port.clear();
                } else if port.control & POST_TX == 0 {
                    port.inhibited = false;
                }
                0x3f
            }
            _ => {
                Self::unsupported_write(addr, value, size);
                return false;
            }
        };
        if value & !writable != 0 {
            log_mask_ln!(
                Log::GuestError,
                "sgi-mace-ps2: reserved write bits at 0x{addr:x} size {size} value 0x{value:x}"
            );
        }
        true
    }

    fn unsupported_write(addr: u64, value: u64, size: u32) {
        log_mask_ln!(
            Log::GuestError,
            "sgi-mace-ps2: unsupported write at 0x{addr:x} size {size} value 0x{value:x}"
        );
    }

    fn next_action(&self, port_num: usize, child_empty: bool) -> (i32, u8) {
        if port_num >= self.port.len() {
            return (ACTION_NONE, 0);
        }
        let port = &self.port[port_num];
        if !port.clock_permitted() {
            return (ACTION_NONE, 0);
        }
        if port.tx_pending {
            if port.control & TX_ENABLE == 0 {
                return (ACTION_NONE, 0);
            }
            if port_num == 1 && port.reply == 0 && !child_empty {
                return (ACTION_DROP_MOUSE_STREAM, 0);
            }
            return (
                if port_num == 0 {
                    ACTION_SEND_KEYBOARD
                } else {
                    ACTION_SEND_MOUSE
                },
                port.tx,
            );
        }
        if !child_empty {
            (ACTION_POP_RX, 0)
        } else {
            (ACTION_NONE, 0)
        }
    }

    fn transmit_complete(&mut self, port_num: usize, reply_count: u32) {
        if port_num >= self.port.len() {
            return;
        }
        let port = &mut self.port[port_num];
        port.tx_pending = false;
        if port.control & POST_TX != 0 {
            port.inhibited = true;
        }
        if port_num == 1 {
            port.reply = reply_count.min(u32::from(u16::MAX)) as u16;
        }
    }

    fn receive(&mut self, port_num: usize, data: u8, child: *mut c_void) -> bool {
        if port_num >= self.port.len() {
            return false;
        }
        let port = &mut self.port[port_num];
        if port_num == 1 && port.reply != 0 {
            port.reply -= 1;
        }
        let accepted = port.receive_frame(data, false, data.count_ones() & 1 == 0, true);
        trace::trace_mace_ps2_receive(
            child.cast(),
            port_num as u32,
            data,
            accepted,
            port.rx,
            port.errors,
            port.rx_full,
        );
        accepted
    }

    fn outputs(&self, port_num: usize, resetting: bool) -> (bool, bool) {
        let port = &self.port[port_num];
        let ready = port.control & RESET == 0 && !resetting;
        let tx = ready && !port.tx_pending;
        let rx = ready && port.rx_full;
        let irq =
            tx && port.control & TX_IRQ_ENABLE != 0 || rx && port.control & RX_IRQ_ENABLE != 0;
        let poll = tx || rx;
        trace::trace_mace_ps2_outputs(
            port_num as u32,
            port.control,
            port.tx_pending,
            port.rx_full,
            ready,
            irq,
            poll,
        );
        (irq, poll)
    }

    fn export(&self, out: &mut [MigrationPort; 2]) {
        for (dest, port) in out.iter_mut().zip(self.port.iter()) {
            *dest = port.migration();
        }
    }

    fn import(&mut self, state: &[MigrationPort; 2]) -> bool {
        for (port, snapshot) in self.port.iter_mut().zip(state.iter().copied()) {
            port.restore(snapshot);
        }
        self.port.iter().all(Port::valid)
    }

    fn reset(&mut self) {
        for port in &mut self.port {
            port.clear();
            port.reply = 0;
            port.control = CLOCK_ENABLE;
        }
    }
}

#[no_mangle]
pub extern "C" fn sgi_mace_ps2_new() -> *mut c_void {
    Box::into_raw(Box::new(Core::new())).cast()
}

/// # Safety
/// `core` must be a state returned by [`sgi_mace_ps2_new`] with no live caller.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_free(core: *mut c_void) {
    if !core.is_null() {
        drop(unsafe { Box::from_raw(core.cast::<Core>()) });
    }
}

/// # Safety
/// `core` must be live and exclusively accessed; `value` must be writable.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_read(
    core: *mut c_void,
    addr: u64,
    size: u32,
    value: *mut u64,
) -> bool {
    if core.is_null() || value.is_null() {
        return false;
    }
    let state = unsafe { &mut *core.cast::<Core>() };
    let (result, progress) = state.read(addr, size);
    unsafe {
        *value = result;
    }
    progress
}

/// # Safety
/// `core` must be live and exclusively accessed.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_write(
    core: *mut c_void,
    addr: u64,
    value: u64,
    size: u32,
) -> bool {
    if core.is_null() {
        return false;
    }
    unsafe { &mut *core.cast::<Core>() }.write(addr, value, size)
}

/// # Safety
/// `core` must be live and exclusively accessed; `data` must be writable.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_next_action(
    core: *mut c_void,
    port: u32,
    child_empty: bool,
    data: *mut u8,
) -> i32 {
    if core.is_null() || data.is_null() {
        return ACTION_NONE;
    }
    let (action, value) = unsafe { &*core.cast::<Core>() }.next_action(port as usize, child_empty);
    unsafe {
        *data = value;
    }
    action
}

/// # Safety
/// `core` must be live and exclusively accessed.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_transmit_complete(
    core: *mut c_void,
    port: u32,
    reply_count: u32,
) {
    if !core.is_null() {
        unsafe { &mut *core.cast::<Core>() }.transmit_complete(port as usize, reply_count);
    }
}

/// # Safety
/// `core` must be live and exclusively accessed; `child` is an opaque trace identity.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_receive(
    core: *mut c_void,
    port: u32,
    data: u8,
    child: *mut c_void,
) -> bool {
    if core.is_null() {
        return false;
    }
    unsafe { &mut *core.cast::<Core>() }.receive(port as usize, data, child)
}

#[no_mangle]
pub extern "C" fn sgi_mace_ps2_stream_dropped(count: u32) {
    trace::trace_mace_ps2_stream_dropped(count);
}

/// # Safety
/// `core` must be live and exclusively accessed; output pointers must be writable.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_outputs(
    core: *mut c_void,
    port: u32,
    resetting: bool,
    irq: *mut bool,
    poll: *mut bool,
) {
    if core.is_null() || irq.is_null() || poll.is_null() || port >= 2 {
        return;
    }
    let (irq_level, poll_level) =
        unsafe { &*core.cast::<Core>() }.outputs(port as usize, resetting);
    unsafe {
        *irq = irq_level;
        *poll = poll_level;
    }
}

/// # Safety
/// `core` must be live and exclusively accessed.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_reset(core: *mut c_void) {
    if !core.is_null() {
        unsafe { &mut *core.cast::<Core>() }.reset();
    }
}

/// # Safety
/// `core` must be live and exclusively accessed; `out` points to two writable entries.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_export(core: *mut c_void, out: *mut MigrationPort) {
    if core.is_null() || out.is_null() {
        return;
    }
    let state = unsafe { &*core.cast::<Core>() };
    let snapshot = unsafe { &mut *out.cast::<[MigrationPort; 2]>() };
    state.export(snapshot);
}

/// # Safety
/// `core` must be live and exclusively accessed; `input` points to two readable entries.
#[no_mangle]
pub unsafe extern "C" fn sgi_mace_ps2_import(
    core: *mut c_void,
    input: *const MigrationPort,
) -> bool {
    if core.is_null() || input.is_null() {
        return false;
    }
    let state = unsafe { &mut *core.cast::<Core>() };
    let snapshot = unsafe { &*input.cast::<[MigrationPort; 2]>() };
    state.import(snapshot)
}

#[cfg(test)]
mod tests {
    use super::{Port, FRAMING_ERROR, PARITY_ERROR};

    #[test]
    fn receive_frame_faults_and_admission() {
        let mut port = Port::new();
        port.control = 0x10;
        let data: [u8; 4] = [0, 0xff, 0x37, 0x80];

        for byte in data {
            let parity = byte.count_ones() & 1 == 0;
            for fault in 0..4 {
                port.rx_full = false;
                assert!(port.receive_frame(byte, fault == 2, parity ^ (fault == 1), fault != 3,));
                assert!(port.rx_full);
                assert_eq!(port.rx, byte);
                assert_eq!(
                    port.errors,
                    match fault {
                        1 => PARITY_ERROR,
                        2 | 3 => FRAMING_ERROR,
                        _ => 0,
                    }
                );
                assert!(!port.receive_frame(0x55, false, true, true));
                let errors = port.errors;
                let value = port.receive();
                assert_eq!(value, u16::from(byte) | (u16::from(0x18 | errors) << 8));
                assert!(!port.rx_full);
                assert_eq!(port.status(), 0x0b | errors);

                port.control = 0;
                assert!(!port.receive_frame(0x55, false, true, true));
                port.control = 0x30;
                assert!(!port.receive_frame(0x55, false, true, true));
                port.control = 0x10;
                port.inhibited = true;
                assert!(!port.receive_frame(0x55, false, true, true));
                port.inhibited = false;
                port.tx_pending = true;
                assert!(!port.receive_frame(0x55, false, true, true));
                port.tx_pending = false;
                assert_eq!(port.errors, errors);
                assert!(port.receive_frame(0x55, false, true, true));
                assert_eq!(port.errors, 0);
                port.errors = 0xc0;
                port.clear();
                assert_eq!(port.status(), 0x0b);
                assert_eq!(port.receive(), 0x0b00);
            }
        }
    }
}
