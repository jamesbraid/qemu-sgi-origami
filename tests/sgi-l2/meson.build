# SPDX-License-Identifier: BSD-3-Clause

# The L1 hub needs only Python. The kernel stand-in plugin's test also needs
# qemu-ppc and a PowerPC cross compiler for its test program; it skips
# without the compiler.
if have_tools and is_variable('qemu_sgi_l2_hub')
  test('sgi-l2-hub',
       python,
       args: [files('hub-test.py'), qemu_sgi_l2_hub],
       depends: [qemu_sgi_l2_hub],
       suite: ['sgi', 'sgi-unit'])
  if get_option('plugins') and 'qemu-ppc' in emulators
    test('sgi-l2-shim',
         python,
         args: [files('l2-shim-test.py'), emulators['qemu-ppc'],
                sgi_l2_shim, qemu_sgi_l2_hub],
         depends: [emulators['qemu-ppc'], sgi_l2_shim, qemu_sgi_l2_hub],
         suite: ['sgi', 'sgi-unit'])
  endif
endif
