#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Origin 300 node-local reset through the Bedrock network interface.

Every IP35 consumer that reboots does it the same way: write 1 to the
network interface's reset enable, then local plus port reset (0x6) to its
port reset register, and never jump to the firmware itself. The stock IP35
PROM prints "Resetting the system..." and does exactly that, then branches
back into a loop; IRIX 6.5.30's restart path writes its request word to
local-block scratch 2 on every node and then does the same pair of writes
every 3 s of the real-time counter until something happens; OpenBSD's IP35
reboot writes local plus link reset. A model that resets only the Bedrock's
own registers leaves every processor in those loops, which is the Origin
300 reboot stall (jamesb/qemu#875).

The program counts its own starts in RAM, which a reset leaves alone.
Processor A makes the consumers' two writes on its first start and then
spins, as the PROM does; on any later start it only records the count. The
other processor spins from the start. The model must bring A back through
the reset vector, so the count reaches 2. Run under multi-threaded TCG and
under the round-robin scheduler, because the reset is requested by one
processor's thread while the other is running.
"""

import re
import time

from lib_mips import R_S0, R_S1, R_T0, R_T1, R_T2, R_ZERO, PromImage, mips_i
from lib_prom import MACHINES
from qemu_test import QemuSystemTest

HUB_WINDOW = 0x9200000001000000
PI_CPU_NUMBER = 0x20
NI_PORT_RESET = 0x680008
NI_RESET_ENABLE = 0x680010

# local reset (bit 2) plus port reset (bit 1), as every consumer writes
PORT_RESET_LOCAL_AND_PORT = 0x6

# The start count, in low RAM clear of the flash and of the firmware
# filter's window at 1 MiB, through Hub special space's uncached alias of
# the node's own memory, which the host reads the same way: plain node
# memory sits behind the memory unit's bank translation, which answers
# nothing until firmware configures it.
STATE_PHYS = 0x180000
SPACE_HUB = 0x10000000000
STATE_HOST = SPACE_HUB + STATE_PHYS

STATUS_KX = 0x80


def build_program():
    program = PromImage()

    # 64-bit kernel addressing, without which the pointers below fault.
    program.emit(0x40000000 | R_T0 << 16 | 12 << 11)             # mfc0
    program.emit(mips_i(0x0d, R_T0, R_T0, STATUS_KX))            # ori
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)             # mtc0
    for _ in range(4):
        program.emit(0)

    program.li64(R_S0, HUB_WINDOW)
    program.li64(R_S1, 0x9000000000000000 | STATE_PHYS)

    # Only processor A counts its starts and resets.
    program.emit(mips_i(0x37, R_S0, R_T2, PI_CPU_NUMBER))        # ld
    program.emit(mips_i(0x0c, R_T2, R_T2, 3))                    # andi
    other = program.branch(0x05, R_T2, R_ZERO)                   # bne

    program.emit(mips_i(0x37, R_S1, R_T0, 0))                    # ld
    program.emit(mips_i(0x19, R_T0, R_T0, 1))                    # daddiu
    program.emit(mips_i(0x3f, R_S1, R_T0, 0))                    # sd
    program.emit(mips_i(0x0d, R_ZERO, R_T1, 1))                  # ori
    restarted = program.branch(0x05, R_T0, R_T1)                 # bne

    # The consumers' reboot: reset enable, then local plus port reset.
    program.li64(R_S0, HUB_WINDOW | NI_RESET_ENABLE)
    program.emit(mips_i(0x0d, R_ZERO, R_T1, 1))
    program.emit(mips_i(0x3f, R_S0, R_T1, 0))
    program.li64(R_S0, HUB_WINDOW | NI_PORT_RESET)
    program.emit(mips_i(0x0d, R_ZERO, R_T1, PORT_RESET_LOCAL_AND_PORT))
    program.emit(mips_i(0x3f, R_S0, R_T1, 0))

    program.patch(other, program.here())
    program.patch(restarted, program.here())
    program.spin()
    return program


class SN1WarmReset(QemuSystemTest):

    def read_starts(self):
        result = self.vm.cmd('human-monitor-command',
                             command_line=f'xp /1gx {STATE_HOST:#x}')
        match = re.search(r'0x([0-9a-fA-F]{16})', result)
        self.assertIsNotNone(match, result)
        return int(match.group(1), 16)

    def run_reset(self, *accel):
        self.set_machine('origin300')
        prom = self.scratch_file('sn1-warm-reset-prom.bin')
        build_program().write(prom)

        self.vm.set_machine(MACHINES['origin300'])
        self.vm.add_args('-cpu', 'R14000', '-smp', '2', *accel,
                         '-bios', prom, '-no-shutdown')
        self.vm.launch()

        # Completion is a guest observation, independent of host speed:
        # the test's own timeout bounds a hang, and a timeout alone does
        # not say the processor was lost.
        deadline = time.monotonic() + 120
        starts = self.read_starts()
        while starts < 2 and time.monotonic() < deadline:
            time.sleep(0.1)
            starts = self.read_starts()
        # Once from power-on and once more from its own reset.
        self.assertEqual(starts, 2)

    def test_local_reset_restarts_processors_mttcg(self):
        self.run_reset('-accel', 'tcg,thread=multi')

    def test_local_reset_restarts_processors_rr(self):
        self.run_reset('-accel', 'tcg,thread=single')


if __name__ == '__main__':
    QemuSystemTest.main()
