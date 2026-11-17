// SPDX-License-Identifier: BSD-3-Clause

//! Isolated synthetic BIC board interface, the Rust implementation of
//! `hw/misc/sgi-bic-board-interface.c`.

pub mod device;

pub const TYPE_SGI_BIC_BOARD_INTERFACE: &::std::ffi::CStr = c"sgi-bic-board-interface";
