# SPDX-License-Identifier: GPL-2.0-or-later
_libmace_id_rs = cargo_ws.package('mace_id').library()

rust_devices_ss.add(when: 'CONFIG_SGI_MACE', if_true: [declare_dependency(
  link_whole: [_libmace_id_rs],
  variables: {'crate': 'mace_id'},
)])
