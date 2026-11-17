# SPDX-License-Identifier: BSD-3-Clause
_libsgi_bic_board_interface_rs = cargo_ws.package('sgi-bic-board-interface').library()

rust_devices_ss.add(when: 'CONFIG_X_SGI_BIC_BOARD_INTERFACE_RUST', if_true: [declare_dependency(
  link_whole: [_libsgi_bic_board_interface_rs],
  variables: {'crate': 'sgi_bic_board_interface'},
)])
