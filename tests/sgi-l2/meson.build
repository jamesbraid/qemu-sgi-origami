# SPDX-License-Identifier: BSD-3-Clause

if have_tools and is_variable('qemu_sgi_l2_hub')
  test('sgi-l2-hub',
       python,
       args: [files('hub-test.py'), qemu_sgi_l2_hub],
       depends: [qemu_sgi_l2_hub],
       suite: ['sgi', 'sgi-unit'])
endif
