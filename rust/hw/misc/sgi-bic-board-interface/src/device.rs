// SPDX-License-Identifier: BSD-3-Clause

use std::ffi::{c_uint, CStr};

use bql::prelude::*;
use hwcore::prelude::*;
use migration::{self, prelude::*};
use qom::prelude::*;
use system::bindings::{hwaddr, MemTxResult};
use util::prelude::*;

/// The two `MemTxResult` values used here; the bindings do not export them.
const MEMTX_OK: MemTxResult = 0;
const MEMTX_ERROR: MemTxResult = 1;

/// Offsets in the caller-selected remote Hub-special address space.
const IDENTITY_OFFSET: hwaddr = 0x3400_0000;
const LATCH_OFFSET: hwaddr = 0x3500_0000;

/// The one fitted controller destination, and its two latch commands.
const CONTROLLER_ADDRESS: u8 = 0x8c;
const COMMAND_LATCH_HIGH: u8 = 0x13;
const COMMAND_LATCH_LOW: u8 = 0x12;

#[repr(C)]
#[derive(qom::Object, hwcore::Device)]
pub struct BicBoardInterfaceState {
    parent_obj: ParentField<DeviceState>,
    #[property(rename = "identity", default = 0)]
    identity: u64,
    #[property(rename = "initial-latch", default = 0)]
    initial_latch: u16,
    #[property(rename = "initial-latch-set", default = false)]
    initial_latch_set: bool,
    high: BqlCell<u8>,
    low: BqlCell<u8>,
    high_valid: BqlCell<bool>,
    low_valid: BqlCell<bool>,
    // The three properties above as the source sent them: they must match.
    migrated_identity: BqlCell<u64>,
    migrated_initial_latch: BqlCell<u16>,
    migrated_initial_latch_set: BqlCell<bool>,
}

impl BicBoardInterfaceState {
    fn latch(&self) -> Option<u16> {
        (self.high_valid.get() && self.low_valid.get())
            .then(|| u16::from(self.high.get()) << 8 | u16::from(self.low.get()))
    }

    fn read(&self, offset: hwaddr, size: u32) -> Option<u64> {
        match (offset, size) {
            (IDENTITY_OFFSET, 8) => Some(self.identity),
            (LATCH_OFFSET, 2) => self.latch().map(u64::from),
            _ => {
                log_mask_ln!(
                    Log::GuestError,
                    "sgi-bic-board-interface: unsupported {size}-byte read at {offset:#x}"
                );
                None
            }
        }
    }

    fn controller_byte(&self, position: u32, high: bool, value: u8, completed: bool) -> bool {
        if position != 0 {
            log_mask_ln!(
                Log::GuestError,
                "sgi-bic-board-interface: absent controller position {position}"
            );
            return false;
        }
        if !completed {
            return false;
        }
        let (byte, valid) = if high {
            (&self.high, &self.high_valid)
        } else {
            (&self.low, &self.low_valid)
        };
        byte.set(value);
        valid.set(true);
        true
    }

    fn realize(&self) -> util::Result<()> {
        // Zero cannot silently supply an expansion identity.
        ensure!(
            self.identity & 0xf000 == 0xc000,
            "BIC experiment requires an explicit synthetic identity with \
             (identity & 0xf000) == 0xc000"
        );
        Ok(())
    }

    fn reset_enter(&self, _type: ResetType) {
        self.high.set((self.initial_latch >> 8) as u8);
        self.low.set(self.initial_latch as u8);
        self.high_valid.set(self.initial_latch_set);
        self.low_valid.set(self.initial_latch_set);
    }

    fn pre_save(&self) -> Result<(), migration::Infallible> {
        self.migrated_identity.set(self.identity);
        self.migrated_initial_latch.set(self.initial_latch);
        self.migrated_initial_latch_set.set(self.initial_latch_set);
        Ok(())
    }

    fn post_load(&self, _version_id: u8) -> Result<(), migration::InvalidError> {
        if self.migrated_identity.get() == self.identity
            && self.migrated_initial_latch.get() == self.initial_latch
            && self.migrated_initial_latch_set.get() == self.initial_latch_set
        {
            Ok(())
        } else {
            Err(migration::InvalidError)
        }
    }

    fn class_init(klass: &mut <Self as ObjectType>::Class) {
        klass.class_init::<Self>();
        // The bindings do not wrap these DeviceClass fields yet.
        klass.user_creatable = false;
        klass.desc = c"Isolated synthetic BIC board interface".as_ptr();
    }
}

qom_isa!(BicBoardInterfaceState: DeviceState, Object);

unsafe impl ObjectType for BicBoardInterfaceState {
    type Class = <DeviceState as ObjectType>::Class;
    const TYPE_NAME: &'static CStr = crate::TYPE_SGI_BIC_BOARD_INTERFACE;
}

impl ObjectImpl for BicBoardInterfaceState {
    type ParentType = DeviceState;

    const CLASS_INIT: fn(&mut Self::Class) = Self::class_init;
}

const VMSTATE_BIC: VMStateDescription<BicBoardInterfaceState> =
    VMStateDescriptionBuilder::<BicBoardInterfaceState>::new()
        .name(c"sgi-bic-board-interface")
        .version_id(1)
        .minimum_version_id(1)
        .pre_save(&BicBoardInterfaceState::pre_save)
        .post_load(&BicBoardInterfaceState::post_load)
        .fields(vmstate_fields! {
            vmstate_of!(BicBoardInterfaceState, migrated_identity),
            vmstate_of!(BicBoardInterfaceState, migrated_initial_latch),
            vmstate_of!(BicBoardInterfaceState, migrated_initial_latch_set),
            vmstate_of!(BicBoardInterfaceState, high),
            vmstate_of!(BicBoardInterfaceState, low),
            vmstate_of!(BicBoardInterfaceState, high_valid),
            vmstate_of!(BicBoardInterfaceState, low_valid),
        })
        .build();

impl DeviceImpl for BicBoardInterfaceState {
    const VMSTATE: Option<VMStateDescription<Self>> = Some(VMSTATE_BIC);
    const REALIZE: Option<fn(&Self) -> util::Result<()>> = Some(Self::realize);
}

impl ResettablePhasesImpl for BicBoardInterfaceState {
    const ENTER: Option<fn(&Self, ResetType)> = Some(Self::reset_enter);
}

fn tx_result(ok: bool) -> MemTxResult {
    if ok {
        MEMTX_OK
    } else {
        MEMTX_ERROR
    }
}

/// Read the remote window at `offset`.
///
/// # Safety
///
/// `s` must point to a realized BIC and `value` to writable storage.
#[no_mangle]
pub unsafe extern "C" fn sgi_bic_board_read(
    s: *const BicBoardInterfaceState,
    offset: hwaddr,
    size: c_uint,
    value: *mut u64,
) -> MemTxResult {
    // SAFETY: the caller passes a valid device.
    let result = unsafe { &*s }.read(offset, size);
    if let Some(v) = result {
        // SAFETY: the caller passes writable storage, possibly uninitialised.
        unsafe {
            value.write(v);
        }
    }
    tx_result(result.is_some())
}

/// Write the remote window at `offset`; no register is writable.
///
/// # Safety
///
/// `s` must point to a realized BIC.
#[no_mangle]
pub unsafe extern "C" fn sgi_bic_board_write(
    _s: *const BicBoardInterfaceState,
    offset: hwaddr,
    size: c_uint,
    value: u64,
) -> MemTxResult {
    log_mask_ln!(
        Log::GuestError,
        "sgi-bic-board-interface: unsupported {size}-byte write at {offset:#x} value {value:#x}"
    );
    MEMTX_ERROR
}

/// Store one byte of the rack and bay latch from the fitted controller.
///
/// # Safety
///
/// `s` must point to a realized BIC.
#[no_mangle]
pub unsafe extern "C" fn sgi_bic_board_controller_byte(
    s: *const BicBoardInterfaceState,
    position: c_uint,
    high: bool,
    value: u8,
    completed: bool,
) -> MemTxResult {
    // SAFETY: the caller passes a valid device.
    tx_result(unsafe { &*s }.controller_byte(position, high, value, completed))
}

/// Controller operation adapter; accepts only the fitted destination.
///
/// # Safety
///
/// `receiver` must point to a realized BIC.
#[no_mangle]
pub unsafe extern "C" fn sgi_bic_board_controller_operation(
    receiver: *mut qom::bindings::Object,
    position: c_uint,
    bus: u8,
    address: u8,
    command: u8,
    value: u8,
) -> bool {
    if position != 0
        || bus != 0
        || address != CONTROLLER_ADDRESS
        || !matches!(command, COMMAND_LATCH_HIGH | COMMAND_LATCH_LOW)
    {
        return false;
    }
    // SAFETY: the caller passes a valid object.
    let object: &Object = unsafe { &*receiver.cast() };
    let s = object
        .downcast::<BicBoardInterfaceState>()
        .expect("receiver is a BIC board interface");
    s.controller_byte(position, command == COMMAND_LATCH_HIGH, value, true)
}
