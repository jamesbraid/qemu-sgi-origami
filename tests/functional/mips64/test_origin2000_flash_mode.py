#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Say so when a node's boot flash was provisioned for a different board.

The processor latches its mode bits from the node board. The board's boot
flash separately records the configuration the board was provisioned for.
Firmware compares the two and stops with indicator 0xb4 before it opens a
console, so a mismatched pair looks exactly like a dead machine. These
checks require the machine to name the disagreement instead.
"""

import re
import struct
from pathlib import Path

from qemu_test import QemuSystemTest


FLASH_BYTES = 1024 * 1024
CONFIG_OFFSET = 0x60
CONFIG_MAGIC = b'ip27conf'
MODE_MASK = 0x01ffffff
# The secondary-cache size field, the one a real provisioning difference hit.
MODE_DIFFERENCE = 0x00020000
WARNING = re.compile(r'node (\d+) boot flash is provisioned for a different '
                     r'node board: it describes processor mode 0x([0-9a-f]{8}) '
                     r'where this board latches 0x([0-9a-f]{8}), differing in '
                     r'0x([0-9a-f]{8})')


def node_flash(mode_word):
    """A flash image carrying nothing but a parked processor and a config."""
    image = bytearray(b'\xff' * FLASH_BYTES)
    struct.pack_into('>II', image, 0, 0x1000ffff, 0)  # b . ; nop
    struct.pack_into('>II', image, CONFIG_OFFSET, 0xb, mode_word)
    image[CONFIG_OFFSET + 8:CONFIG_OFFSET + 16] = CONFIG_MAGIC
    struct.pack_into('>QQQ', image, CONFIG_OFFSET + 0x10,
                     180000000, 90000000, 1250000)
    return bytes(image)


class Origin2000FlashMode(QemuSystemTest):
    timeout = 90

    def boot(self, tag, mode_word):
        """Start the machine on four identical node flash images."""
        vm = self.get_vm(name=tag)
        vm.set_machine('origin2000')
        vm.add_args('-machine', 'synthesized-firmware=off',
                    '-accel', 'tcg,thread=single', '-cpu', 'R10000',
                    '-m', '256M', '-smp', '8,sockets=4,cores=2,threads=1',
                    '-nodefaults', '-S')
        for node in range(4):
            image = self.scratch_file(f'{tag}-node{node}.bin')
            Path(image).write_bytes(node_flash(mode_word))
            vm.add_args('-drive',
                        f'if=pflash,index={node},file={image},format=raw')
        vm.launch()
        registers = vm.cmd('human-monitor-command',
                           command_line='info registers')
        vm.shutdown()
        latched = re.search(r'Config0 0x([0-9a-f]+)', registers)
        self.assertIsNotNone(latched, registers)
        return int(latched.group(1), 16) & MODE_MASK, vm.get_log()

    def test_mismatched_flash_is_named(self):
        # Zero is not a mode word any R10000 board drives, so this first boot
        # both learns what the board latches and guarantees a disagreement.
        latched, log = self.boot('probe', 0)
        self.assertNotEqual(latched, 0)
        reported = {}
        for node, flash, board, difference in WARNING.findall(log):
            reported[int(node)] = (int(flash, 16), int(board, 16),
                                   int(difference, 16))
        self.assertEqual(sorted(reported), [0, 1, 2, 3], log)
        for node, (flash, board, difference) in reported.items():
            self.assertEqual(flash, 0, node)
            self.assertEqual(board & MODE_MASK, latched, node)
            self.assertEqual(difference, latched, node)

        # One field apart is the real failure's shape, and still reported.
        near = latched ^ MODE_DIFFERENCE
        _, log = self.boot('near', near)
        self.assertEqual(len(WARNING.findall(log)), 4, log)
        for _, flash, _, difference in WARNING.findall(log):
            self.assertEqual(int(flash, 16), near)
            self.assertEqual(int(difference, 16), MODE_DIFFERENCE)

    def test_matching_flash_is_quiet(self):
        latched, _ = self.boot('learn', 0)
        self.assertNotEqual(latched, 0)
        _, log = self.boot('matched', latched)
        self.assertNotRegex(log, WARNING)


if __name__ == '__main__':
    QemuSystemTest.main()
