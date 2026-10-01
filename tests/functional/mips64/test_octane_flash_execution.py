#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 James Braid

"""Execute public instructions through Octane's reset flash aperture."""

import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest
from lib_translation_health import evaluate


class OctaneFlashExecution(QemuSystemTest):
    def test_reset_flash_builds_multi_instruction_block(self):
        image = self.scratch_file('octane-flash.bin')
        instructions = [
            0x24080001,  # addiu t0, zero, 1
            0x25080001,  # addiu t0, t0, 1
            0x25080001,  # addiu t0, t0, 1
            0x1000ffff,  # beq zero, zero, .
            0x00000000,  # nop
        ]
        Path(image).write_bytes(struct.pack('>5I', *instructions))
        self.set_machine('octane')
        self.vm.add_args('-accel', 'tcg,thread=single', '-cpu', 'R10000',
                         '-m', '128M', '-bios', image, '-nodefaults')
        self.vm.launch()

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            sample = self.vm.cmd('human-monitor-command',
                                 command_line='info jit')
            health = evaluate(sample)
            if health.tb_count:
                break
            time.sleep(0.05)
        else:
            self.fail('TCG did not translate the synthetic reset fixture')

        self.assertFalse(health.one_insn_per_tb, sample)
        self.assertFalse(health.icount_enabled, sample)
        self.assertGreater(health.max_target_size, 4, sample)


if __name__ == '__main__':
    QemuSystemTest.main()
