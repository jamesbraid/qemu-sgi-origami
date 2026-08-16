#!/usr/bin/env python3
#
# Regression test for the CP0 Cause/Compare/Count BQL race
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
cpu_mips_store_cause() (target/mips/system/cp0.c) read-modify-writes the
whole CP0_Cause word with no lock.  Under -accel tcg,thread=multi the vCPU
thread runs that RMW concurrently with the main-loop thread, which raises
the CP0 timer IRQ (IP7, Cause bit 15) through cpu_mips_irq_request()
(hw/mips/mips_int.c, itself BQL-protected).  If the main-loop thread sets
IP7 between the vCPU thread's read and write-back of Cause, the vCPU's
store overwrites it with the stale pre-interrupt snapshot: the interrupt
is set, then silently erased before software ever observes it.

This test arms CP0 Compare for a stream of near-future ticks and, on a
single vCPU, hammers `mtc0 Cause` in a tight poll loop watching Cause's
IP7 bit.  Every period it is not clobbered, the loop counts it, re-arms
Compare (an mtc0 Compare, itself one of the guarded stores), and
continues; a lost update shows up as a period whose IP7 bit never
arrives, so the period counter stalls forever while the heartbeat
counter keeps climbing.  The counters and a completion marker are
published to fixed words in uncached RAM (kseg1) that the host polls
through the HMP `xp` command.

The hammer write toggles Cause's IV bit (bit 23, "use the general
exception vector"), not the software IP0/IP1 bits the design sketch
first reached for: writing IP0/IP1 calls cpu_mips_soft_irq(), which
calls the *already* BQL-protected cpu_mips_irq_request() in turn, and
that function takes the BQL whenever any Cause IP bit is set -- so every
other hammer iteration was itself briefly taking the BQL, serializing
the vCPU thread against the main-loop thread and closing the very
window this test needs open.  IV has no interrupt-controller side
effect: writing it only exercises the racy read-modify-write in
cpu_mips_store_cause(), with nothing else touching cpu->interrupt_request
on the vCPU side.

Once per period, before computing the next Compare deadline, the guest
also reads Count back into the same register and writes it straight back
unchanged -- the same no-op idiom cpu_mips_start_count() itself uses
internally, an MFC0 immediately followed by an MTC0 to the same register
with nothing in between. Without it, cpu_mips_store_count() -- the fourth
of the four guarded call sites this test claims below -- carried zero
coverage from this workload: the read side (cpu_mips_get_count()) was
already exercised by the jitter calculation's own Count read, but nothing
here had ever executed MTC0 Count. Once per period rather than in the
tight poll loop, so it does not touch the per-iteration cost the
period-length reasoning above depends on.

-smp 1 is enough: the race is the single vCPU's thread against the
main-loop thread that runs QEMU_CLOCK_VIRTUAL timer callbacks, not vCPU
against vCPU.

What this test does and does not demonstrate on its own. The unguarded
window inside cpu_mips_store_cause() -- the few machine instructions of
`env->CP0_Cause = (env->CP0_Cause & ~mask) | (val & mask)` -- is on the
order of a nanosecond. Against an unmodified, unwidened build this test
will NOT catch the missing lock by timing luck: ~85 runs and roughly
52,500 period-trials across five differently-shaped attempts (short and
long periods, jittered period length, both hammer-bit choices) all
passed clean on the unfixed tree. Two things did catch it, and this
test is the shared workload for both:

  - ThreadSanitizer. Configuring the fork with -Dtsan=true and running
    this test's guest program under thread=multi reports a genuine data
    race between cpu_mips_store_cause() (vCPU thread) and
    cpu_mips_irq_request() (main-loop thread, via the CP0 timer's
    cpu_mips_timer_expire()) on the unfixed tree, and no such report on
    the fixed one. TSan's happens-before analysis doesn't need the two
    accesses to collide in real time the way this test's polling does,
    so it needs no manifestation and no tuning.
  - A widened-window mutant. Splitting the read and the write-back in
    cpu_mips_store_cause() around a few-hundred-millisecond sleep, on
    the unfixed tree, makes this test fail promptly and reliably (lost
    IP7, stalled period, state dump) -- see the task's PR description
    for the transcript. The same mutant with the fix applied passes:
    the guard serializes the vCPU thread against the main-loop thread
    for the whole widened section, not just the unwidened one.

So this test's demonstrated detection power is against the widened
mutant, not the real, nanosecond-scale unfixed race. It stays in the
suite as a real, always-green regression check exercising the four
guarded call sites (Cause, Compare, Count, the inline count-expire) and
as the fixed workload for TSan and mutation-widening soak runs, not as
a claim that it alone would catch this specific bug coming back.
"""

import re
import struct
import time

from qemu_test import QemuSystemTest


# ---------------------------------------------------------------------------
# A minimal big-endian MIPS32 (o32) assembler: just enough instruction
# encodings for this test's guest program, plus a two-pass label/branch
# resolver so forward branches can be written naturally.

ZERO = 0
T0, T1, T2, T3, T4, T5, T6, T7 = range(8, 16)
S0, S1, S2 = 16, 17, 18

OP_SPECIAL = 0x00
OP_COP0 = 0x10
OP_BEQ = 0x04
OP_BNE = 0x05
OP_ADDIU = 0x09
OP_ANDI = 0x0C
OP_ORI = 0x0D
OP_XORI = 0x0E
OP_LUI = 0x0F
OP_SW = 0x2B

FN_SLL = 0x00
FN_ADDU = 0x21

MFC0 = 0x00
MTC0 = 0x04


def itype(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def rtype(rs, rt, rd, sh, fn):
    return (OP_SPECIAL << 26) | (rs << 21) | (rt << 16) | (rd << 11) | \
           (sh << 6) | fn


def cop0(sub, rt, rd, sel):
    return (OP_COP0 << 26) | (sub << 21) | (rt << 16) | (rd << 11) | \
           (sel & 0x7)


class Program:
    """emit() appends instruction words.  branch() reserves a branch and
    its delay slot, resolved against label() targets once the whole
    program has been emitted (so forward branches just work)."""

    def __init__(self):
        self.words = []
        self.labels = {}
        self.fixups = []

    def here(self):
        return len(self.words)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.here()

    def emit(self, word):
        self.words.append(word)

    def branch(self, op, rs, rt, target_label):
        idx = self.here()
        self.emit(0)  # branch, patched in resolve()
        self.emit(0)  # delay slot (nop)
        self.fixups.append((idx, op, rs, rt, target_label))

    def resolve(self):
        for idx, op, rs, rt, target_label in self.fixups:
            target = self.labels[target_label]
            # MIPS branch offset is relative to the delay slot's own
            # address (the instruction "following the branch"), in
            # instruction words: target = branch_idx + 1 + offset.
            disp = target - idx - 1
            assert -0x8000 <= disp < 0x8000, disp
            self.words[idx] = itype(op, rs, rt, disp)

    def to_bytes(self):
        self.resolve()
        return b''.join(struct.pack('>I', w) for w in self.words)


# ---------------------------------------------------------------------------
# Guest program: hammer Cause while polling for the timer's IP7 bit.

STATE_VADDR = 0xA0030000          # kseg1 (uncached) mirror of STATE_PHYS
STATE_PHYS = 0x00030000
STATE_HEARTBEAT = STATE_PHYS + 0x0  # incremented every poll/hammer iteration
STATE_PERIODS = STATE_PHYS + 0x4    # periods whose IP7 bit was observed
STATE_DONE = STATE_PHYS + 0x8       # DONE_MAGIC once every period completed


# kseg0 (cached), well clear of the ROM area.  A plain 32-bit ELF's entry
# point is zero-extended by the loader (hw/core/loader.c casts a 32-bit
# Elf32_Word into a 64-bit uint64_t *pentry), landing outside the
# sign-extended compatibility range MIPS64 kernel mode requires to
# recognize kseg0/kseg1 -- so this has to be a 64-bit ELF with the
# canonical sign-extended address baked in, not a 32-bit one.
CODE_VADDR = 0xFFFFFFFF80020000
CODE_PHYS = 0x00020000

DONE_MAGIC = 0xC0FFEE42
IP7_MASK = 0x8000                  # Cause bit 15: hardware interrupt 5 (timer)
N_PERIODS = 100
DELTA_COUNT_TICKS = 1_000_000      # ~5ms/period on the 20Kc's 200MHz Count
JITTER_MASK = 0x3FF                # up to ~5us of period-length jitter


def build_cause_race_program():
    p = Program()

    p.emit(itype(OP_LUI, 0, T6, (STATE_VADDR >> 16) & 0xFFFF))
    p.emit(itype(OP_ADDIU, ZERO, S0, N_PERIODS))
    p.emit(itype(OP_LUI, 0, S1, (DELTA_COUNT_TICKS >> 16) & 0xFFFF))
    p.emit(itype(OP_ORI, S1, S1, DELTA_COUNT_TICKS & 0xFFFF))
    p.emit(rtype(0, 0, S2, 0, FN_ADDU))    # periods_done = 0
    p.emit(rtype(0, 0, T7, 0, FN_ADDU))    # heartbeat = 0
    p.emit(rtype(0, 0, T3, 0, FN_ADDU))    # hammer toggle flag = 0
    p.emit(itype(OP_SW, T6, ZERO, 0))      # zero the published state words
    p.emit(itype(OP_SW, T6, ZERO, 4))
    p.emit(itype(OP_SW, T6, ZERO, 8))

    p.label('period_loop')
    p.emit(cop0(MFC0, T0, 9, 0))                    # t0 = Count
    p.emit(cop0(MTC0, T0, 9, 0))                    # Count = t0 (see below)
    # Jitter the period length with Count's own low bits.  A fixed period
    # lets the two threads' loops settle into a stable phase relationship
    # (each one's per-iteration cost is regular enough that they can stay
    # in lockstep for the whole run) that can keep the write outside the
    # race window on every single period; a period length that keeps
    # shifting sweeps through phases instead of parking on whichever one
    # the run happened to start on.
    p.emit(itype(OP_ANDI, T0, T2, JITTER_MASK))
    p.emit(rtype(T0, S1, T0, 0, FN_ADDU))           # t0 += DELTA
    p.emit(rtype(T0, T2, T0, 0, FN_ADDU))           # t0 += jitter
    p.emit(cop0(MTC0, T0, 11, 0))                   # Compare = t0 (re-arm)

    p.label('poll_loop')
    p.emit(cop0(MFC0, T1, 13, 0))                   # t1 = Cause
    p.emit(itype(OP_ANDI, T1, T2, IP7_MASK))
    p.branch(OP_BNE, T2, ZERO, 'period_done')       # IP7 observed -> break

    p.emit(itype(OP_XORI, T3, T3, 1))               # toggle hammer value
    p.emit(rtype(0, T3, T4, 23, FN_SLL))            # t4 = flag << 23 (IV)
    p.emit(cop0(MTC0, T4, 13, 0))                   # Cause = t4  (the race)
    p.emit(itype(OP_ADDIU, T7, T7, 1))
    p.emit(itype(OP_SW, T6, T7, 0))                 # publish heartbeat
    p.branch(OP_BEQ, ZERO, ZERO, 'poll_loop')

    p.label('period_done')
    p.emit(itype(OP_ADDIU, S2, S2, 1))
    p.emit(itype(OP_SW, T6, S2, 4))                 # publish periods_done
    p.branch(OP_BNE, S2, S0, 'period_loop')

    p.emit(itype(OP_LUI, 0, T5, (DONE_MAGIC >> 16) & 0xFFFF))
    p.emit(itype(OP_ORI, T5, T5, DONE_MAGIC & 0xFFFF))
    p.emit(itype(OP_SW, T6, T5, 8))                 # publish DONE_MAGIC

    p.label('spin_forever')
    p.branch(OP_BEQ, ZERO, ZERO, 'spin_forever')

    return p.to_bytes()


# ---------------------------------------------------------------------------
# A hand-built ELF64 big-endian MIPS executable: the malta board's -kernel
# loader (load_kernel() in hw/mips/malta.c) wants an ELF whose entry point
# is a kseg0 address, and jumps straight to it.  ELF64 (not ELF32) so the
# sign-extended 64-bit address can be written directly, rather than relying
# on the loader to extend a 32-bit field -- see the CODE_VADDR comment.

ELF_EHDR_SIZE = 0x40
ELF_PHDR_SIZE = 0x38


def build_elf64_be(entry, vaddr, code):
    data_off = ELF_EHDR_SIZE + ELF_PHDR_SIZE

    e_ident = b'\x7fELF' + bytes([2, 2, 1]) + b'\x00' * 9
    ehdr = e_ident + struct.pack(
        '>HHIQQQIHHHHHH',
        2,                 # e_type = ET_EXEC
        8,                 # e_machine = EM_MIPS
        1,                 # e_version
        entry,             # e_entry
        ELF_EHDR_SIZE,     # e_phoff
        0,                 # e_shoff
        0,                 # e_flags
        ELF_EHDR_SIZE,     # e_ehsize
        ELF_PHDR_SIZE,     # e_phentsize
        1,                 # e_phnum
        0, 0, 0)           # e_shentsize, e_shnum, e_shstrndx

    phdr = struct.pack(
        '>IIQQQQQQ',
        1,                 # p_type = PT_LOAD
        5,                 # p_flags = PF_R | PF_X
        data_off,          # p_offset
        vaddr,             # p_vaddr
        vaddr,             # p_paddr
        len(code),         # p_filesz
        len(code),         # p_memsz
        0x1000)            # p_align

    return ehdr + phdr + code


# ---------------------------------------------------------------------------

DEADLINE_SECONDS = 30


class MTTCGCauseRaceTest(QemuSystemTest):

    def read_word(self, addr):
        result = self.vm.cmd('human-monitor-command',
                             command_line=f'xp/1xw {addr:#x}')
        m = re.search(r'0x([0-9a-fA-F]{8})', result)
        self.assertIsNotNone(m, f'unexpected xp output: {result!r}')
        return int(m.group(1), 16)

    def test_cause_write_does_not_lose_timer_interrupt(self):
        self.set_machine('malta')

        kernel_path = self.scratch_file('mttcg-cause-race.elf')
        code = build_cause_race_program()
        elf = build_elf64_be(CODE_VADDR, CODE_VADDR, code)
        with open(kernel_path, 'wb') as f:
            f.write(elf)

        self.vm.add_args('-cpu', '20Kc', '-m', '16M',
                         '-accel', 'tcg,thread=multi',
                         '-smp', '1',
                         '-kernel', kernel_path,
                         '-no-reboot', '-no-shutdown')
        self.vm.launch()

        deadline = time.monotonic() + DEADLINE_SECONDS
        while time.monotonic() < deadline:
            if self.read_word(STATE_DONE) == DONE_MAGIC:
                self.vm.shutdown()
                return
            time.sleep(0.02)

        heartbeat = self.read_word(STATE_HEARTBEAT)
        periods = self.read_word(STATE_PERIODS)
        done = self.read_word(STATE_DONE)
        self.vm.shutdown()
        self.fail(
            'cause race test stalled: heartbeat=%#x periods=%d/%d '
            'done=%#x (expected periods=%d done=%#x)' %
            (heartbeat, periods, N_PERIODS, done, N_PERIODS, DONE_MAGIC))


if __name__ == '__main__':
    QemuSystemTest.main()
