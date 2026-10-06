# Shared Hub-window code for the bare-metal SN0 images: address loads,
# the master/slave strap read, and the node-numbering exchange that stands
# in for the PROM's own, so a node-qualified address names something.
#
# Plain Python on lib_mips, no qemu_test dependency.
#
# SPDX-License-Identifier: BSD-3-Clause

from lib_mips import R_AT, R_T0, R_T1, R_T2, R_ZERO, mips_i, mips_r


def load_u32(program, register, value):
    assert 0 <= value <= 0xffffffff
    program.emit(mips_i(0x0f, R_ZERO, register, value >> 16))
    program.emit(mips_i(0x0d, register, register, value & 0xffff))


def load_xkphys_address(program, register, prefix, offset):
    program.address_prefix(register, prefix)
    load_u32(program, R_AT, offset)
    program.emit(mips_r(register, R_AT, register, 0, 0x25))


def emit_local_hub_address(program, register, offset):
    load_xkphys_address(program, register, 0x9200, 0x01000000 + offset)


# hw/misc/sgi-hub.c's Hub network-interface node-number field, resetting
# to zero on every Hub, and the vector-routing registers that reach a
# direct peer before either side has a number -- the same offsets
# tests/qtest/sn0-user-image.h's sn0_program_nasid() uses, reached here
# the way this file's own CPU-issued code reaches every other Hub
# register, through the local (position-independent) window rather than
# the crosstalk-widget form the qtest backdoor files use.
HUB_NI_STATUS = 0x600000
HUB_NI_VECTOR_PARMS = 0x600200
HUB_NI_VECTOR = 0x600208
HUB_NI_VECTOR_DATA = 0x600210
HUB_NI_REGION_SIZE = 1 << 17
HUB_PIOTYPE_WRITE = 1

# hw/misc/sgi-hub.c's MD_SLOT_ID_USTAT, read elsewhere in this file's own
# sibling C qtests (sn0-hub-test.c's assert_dual_node_master_slave_sense):
# a real hardware strap fixed at construction from board position, unlike
# the Hub's own node number, which every Hub reads as zero until firmware
# assigns one.
HUB_MD_SLOT_ID_USTAT = 0x220048
MD_SLOT_MASTER_SHIFT = 7  # bit index of MD_SLOT_MASTER_MODULE


def emit_local_hub_read(program, reg, offset):
    """Load the 64-bit word at HUB_LOCAL + offset into reg, uncached."""
    emit_local_hub_address(program, reg, offset)
    program.emit(mips_i(0x37, reg, reg, 0))  # ld reg, 0(reg)


def emit_local_hub_write(program, addr_reg, value_reg, offset):
    """Store value_reg's 64-bit word to HUB_LOCAL + offset, uncached."""
    emit_local_hub_address(program, addr_reg, offset)
    program.emit(mips_i(0x3f, addr_reg, value_reg, 0))  # sd value_reg, 0(base)
    program.emit(0x0000000f)  # sync


def emit_read_is_slave(program, reg):
    """0 on the master module (-numa nodeid 0's board position), 1 on a
    slave module: MD_SLOT_ID_USTAT's master bit, inverted so it reads the
    way this file's builders already want their node bit shaped (every
    machine this file builds has exactly one slave module, node one).
    Safe to read before any Hub has a node number, unlike the identity
    scheme this replaces, which read that number itself.
    """
    emit_local_hub_read(program, reg, HUB_MD_SLOT_ID_USTAT)
    program.emit(mips_r(R_ZERO, reg, reg, MD_SLOT_MASTER_SHIFT, 0x3a))  # dsrl
    program.emit(mips_i(0x0c, reg, reg, 1))          # andi reg, reg, 1
    program.emit(mips_i(0x0e, reg, reg, 1))          # xori reg, reg, 1


def emit_assign_peer_nasid_if_master(program, nasid):
    """No PROM ran to number the direct peer's Hub, so a node-one-qualified
    address -- this file's own, and the host's read_state() polls -- names
    nothing until this runs: a vector-routed write over the CrayLink, the
    technique the Hub network-interface datasheet's "Vector routing"
    section and sn0_program_nasid() both use. Every CPU in this file's
    images runs this same code; only the master module's issue the write,
    since a vector operation always reaches the direct peer, and a slave
    module's CPU running it would renumber the master instead. Both of a
    master module's own CPUs take the branch and repeat the write; a
    second CrayLink write of the same value is a no-op, not a race.
    """
    is_slave = R_T2
    emit_read_is_slave(program, is_slave)
    skip = program.branch(0x05, is_slave, R_ZERO)  # bne is_slave, zero, skip

    emit_local_hub_write(program, R_T0, R_ZERO, HUB_NI_VECTOR)
    load_u32(program, R_T1, HUB_NI_REGION_SIZE | (nasid << 8))
    emit_local_hub_write(program, R_T0, R_T1, HUB_NI_VECTOR_DATA)
    load_u32(program, R_T1, HUB_NI_STATUS | HUB_PIOTYPE_WRITE)
    emit_local_hub_write(program, R_T0, R_T1, HUB_NI_VECTOR_PARMS)

    program.patch(skip, len(program.instructions))


def emit_wait_for_own_nasid_if_slave(program):
    """Hold a slave module's CPU until the master has numbered its Hub.

    Nothing orders the slave's CPUs after the master's vector-routed write
    in emit_assign_peer_nasid_if_master.  Until it lands the slave's Hub
    still believes it is node zero, and the Hub network-interface
    datasheet's "Vector routing" section is explicit that before numbers
    are assigned no node-numbered address means anything.  A slave CPU
    that raced ahead to a node-one-qualified store took a bus error and
    never published, so poll the local NI_STATUS node field (bits 16:8)
    until it is nonzero.  Master CPUs fall straight through.
    """
    emit_read_is_slave(program, R_T2)
    skip = program.branch(0x04, R_T2, R_ZERO)  # beq is_slave, zero, skip
    wait = len(program.instructions)
    emit_local_hub_read(program, R_T1, HUB_NI_STATUS)
    program.emit(mips_r(R_ZERO, R_T1, R_T1, 8, 0x3a))  # dsrl t1, t1, 8
    program.emit(mips_i(0x0c, R_T1, R_T1, 0x1ff))      # andi t1, t1, 0x1ff
    branch = program.branch(0x04, R_T1, R_ZERO)        # beq t1, zero, wait
    program.patch(branch, wait)
    program.patch(skip, len(program.instructions))
