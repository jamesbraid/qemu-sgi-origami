# SPDX-License-Identifier: GPL-2.0-or-later

# Builds a new SGI machine's storage from the compiled catalogue with the
# machine's own resolver and initial-flash code, without building a
# machine: none of these sources touch QOM or devices.
qemu_sgi_machine_init = executable('qemu-sgi-machine-init',
  files('qemu-sgi-machine-init.c',
        '../../hw/mips/sgi/sgi-machines.c',
        '../../hw/mips/sgi/sn-model-runtime.c',
        '../../hw/mips/sgi/prom-image.c',
        '../../hw/mips/sgi/initial-flash.c',
        '../../hw/rtc/m48t35-ip27.c') + [sgi_machines_embed_c, genh],
  dependencies: [qemuutil, zlib],
  install: true)
