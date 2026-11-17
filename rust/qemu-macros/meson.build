# A proc macro runs on the build machine, so a cross build asks for that package.
if meson.is_cross_build()
  _qemu_macros_pkg = cargo_ws.package('qemu_macros', native: true)
else
  _qemu_macros_pkg = cargo_ws.package('qemu_macros')
endif
_qemu_macros_rs = _qemu_macros_pkg.proc_macro()
_qemu_macros_pkg.override_dependency(declare_dependency(link_with: _qemu_macros_rs))

qemu_macros = declare_dependency(
  link_with: _qemu_macros_rs,
)

rust.test('rust-qemu-macros-tests', _qemu_macros_rs,
          suite: ['unit', 'rust'])
