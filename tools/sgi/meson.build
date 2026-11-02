# SPDX-License-Identifier: BSD-3-Clause

# Builds a new SGI machine's storage from the compiled catalogue with the
# machine's own resolver and initial-flash code, without building a
# machine: none of these sources touch QOM or devices.

# The SGI machine catalogue's QAPI types are generated for system builds.
if have_system and is_variable('sgi_machines_embed_c')
  qemu_sgi_machine_init = executable('qemu-sgi-machine-init',
    files('qemu-sgi-machine-init.c',
          '../../hw/mips/sgi/sgi-machines.c',
          '../../hw/mips/sgi/sn-model-runtime.c',
          '../../hw/mips/sgi/prom-image.c',
          '../../hw/mips/sgi/initial-flash.c',
          '../../hw/rtc/m48t35-ip27.c',
          '../../hw/nvram/sgi-elsc-nvram-image.c',
          '../../hw/misc/sgi-kona-flash-image.c') + [sgi_machines_embed_c, genh],
    dependencies: [qemuutil, zlib],
    install: true)
endif

# The host end of the L2's device nodes when the L2's own programs run under
# qemu-ppc: it needs only the link and hub headers and the common utilities.
# It uses accept4, SOCK_SEQPACKET and MSG_NOSIGNAL, so it builds on Linux hosts.
if host_os == 'linux'
  qemu_sgi_l2_hub = executable('qemu-sgi-l2-hub',
    files('qemu-sgi-l2-hub.c') + [genh],
    dependencies: [qemuutil],
    install: true)
endif
