# SPDX-License-Identifier: GPL-2.0-or-later
_mace_ps2_rs = cargo_ws.package('mace_ps2').library()

rust_devices_ss.add(when: 'CONFIG_SGI_MACE', if_true: [declare_dependency(
  link_whole: [_mace_ps2_rs],
  variables: {'crate': 'mace_ps2'},
)])

rust.test('mace-ps2-rust-tests', _mace_ps2_rs,
          suite: ['unit', 'rust', 'sgi-unit'])
