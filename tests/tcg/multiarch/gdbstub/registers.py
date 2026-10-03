# Exercise the register functionality by exhaustively iterating
# through all supported registers on the system.
#
# This is launched via tests/guest-debug/run-test.py but you can also
# call it directly if using it for debugging/introspection:
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import re
import gdb
import xml.etree.ElementTree as ET
from test_gdbstub import main, report


initial_vlen = 0


def fetch_xml_regmap():
    """
    Iterate through the XML descriptions and validate.

    We check for any duplicate registers and report them. Return a
    reg_map hash containing the names, regnums and initial values of
    all registers.
    """

    # First check the XML descriptions we have sent. Most arches
    # support XML but a few of the ancient ones don't in which case we
    # need to gracefully fail.

    try:
        xml = gdb.execute("maint print xml-tdesc", False, True)
    except (gdb.error):
        print("SKIP: target does not support XML")
        return None

    total_regs = 0
    reg_map = {}

    tree = ET.fromstring(xml)
    for f in tree.findall("feature"):
        name = f.attrib["name"]
        regs = f.findall("reg")

        total = len(regs)
        total_regs += total
        base = int(regs[0].attrib["regnum"])
        top = int(regs[-1].attrib["regnum"])

        print(f"feature: {name} has {total} registers from {base} to {top}")

        for r in regs:
            name = r.attrib["name"]
            regnum = int(r.attrib["regnum"])

            entry = {"name": name, "regnum": regnum,
                     "bitsize": int(r.attrib["bitsize"]),
                     "type": r.attrib.get("type")}

            if name in reg_map:
                report(False, f"duplicate register {entry} vs {reg_map[name]}")
                continue

            reg_map[name] = entry

    # Validate we match
    report(total_regs == len(reg_map.keys()),
           f"counted all {total_regs} registers in XML")

    return reg_map


def check_mips_g_packet(reg_map):
    report("qemu_unused" in reg_map,
           "MIPS target description names legacy slot 72")
    report(len(reg_map) == 73, "MIPS target description has 73 registers")
    regnums = {entry["regnum"] for entry in reg_map.values()}
    report(regnums == set(range(73)),
           "MIPS target description covers register numbers 0 through 72")
    widths = {entry["bitsize"] for entry in reg_map.values()}
    report(len(widths) == 1 and next(iter(widths)) in (32, 64),
           "MIPS core registers use one target-long width")
    if len(widths) != 1:
        return
    width = next(iter(widths))
    packet = gdb.execute("maintenance packet g", False, True)
    match = re.search(r'received: "([0-9a-fA-F]+)"', packet)
    report(match is not None, "GDB reads the complete MIPS g packet")
    if match:
        report(len(match.group(1)) == 73 * (width // 4),
               "MIPS g packet retains all 73 target-long slots")
    expected_fpu_type = "ieee_single" if width == 32 else "ieee_double"
    fpu_regs = [entry for entry in reg_map.values()
                if 38 <= entry["regnum"] <= 69]
    report(len(fpu_regs) == 32 and all(
        entry["type"] == expected_fpu_type for entry in fpu_regs),
        f"MIPS FPU registers use {expected_fpu_type} types")


def check_mips_abi_registers(frame):
    """Check the standard MIPS ABI names against the test program."""
    expected = {
        "a0": 0,
        "a1": 0x1234567,
        "a2": 0x2345678,
        "a3": 0x3456789,
        "gp": 0x456789a,
        "s2": 0x56789ab,
        "s3": 0x6789abc,
        "v1": 0x789abcd,
    }

    for name, value in expected.items():
        try:
            actual = int(frame.read_register(name))
        except (ValueError, gdb.error) as e:
            report(False, f"failed to read MIPS ABI register {name}: {e}")
            continue
        report(actual == value,
               f"MIPS ABI register {name} is 0x{actual:x}, "
               f"expected 0x{value:x}")


def is_mips_target():
    try:
        arch = gdb.selected_frame().architecture().name().lower()
        return arch.startswith("mips")
    except gdb.error:
        return False


def get_register_by_regnum(reg_map, regnum):
    """
    Helper to find a register from the map via its XML regnum
    """
    for regname, entry in reg_map.items():
        if entry['regnum'] == regnum:
            return entry
    return None


def crosscheck_remote_xml(reg_map):
    """
    Cross-check the list of remote-registers with the XML info.
    """

    remote = gdb.execute("maint print remote-registers", False, True)
    r_regs = remote.split("\n")

    total_regs = len(reg_map.keys())
    total_r_regs = 0
    total_r_elided_regs = 0

    for r in r_regs:
        r = r.replace("long long", "long_long")
        r = r.replace("long double", "long_double")
        fields = r.split()
        # Some of the registers reported here are "pseudo" registers that
        # gdb invents based on actual registers so we need to filter them
        # out.
        if len(fields) == 8:
            r_name = fields[0]
            r_regnum = int(fields[6])

            # Some registers are "hidden" so don't have a name
            # although they still should have a register number
            if r_name == "''":
                total_r_elided_regs += 1
                x_reg = get_register_by_regnum(reg_map, r_regnum)
                if x_reg is not None:
                    x_reg["hidden"] = True
                continue

            # check in the XML
            try:
                x_reg = reg_map[r_name]
            except KeyError:
                report(False, f"{r_name} not in XML description")
                continue

            x_reg["seen"] = True
            x_regnum = x_reg["regnum"]
            if r_regnum != x_regnum:
                report(False, f"{r_name} {r_regnum} == {x_regnum} (xml)")
            else:
                total_r_regs += 1

    report(total_regs == total_r_regs + total_r_elided_regs,
           "All XML Registers accounted for")

    print(f"xml-tdesc has {total_regs} registers")
    print(f"remote-registers has {total_r_regs} registers")
    print(f"of which {total_r_elided_regs} are hidden")

    for x_key in reg_map.keys():
        x_reg = reg_map[x_key]
        if "hidden" in x_reg:
            print(f"{x_reg} elided by gdb")
        elif "seen" not in x_reg:
            print(f"{x_reg} wasn't seen in remote-registers")


def initial_register_read(reg_map):
    """
    Do an initial read of all registers that we know gdb cares about
    (so ignore the elided ones).
    """
    frame = gdb.selected_frame()

    for e in reg_map.values():
        name = e["name"]
        regnum = e["regnum"]

        try:
            if "hidden" in e:
                value = frame.read_register(regnum)
                e["initial"] = value.bytes
            elif "seen" in e:
                value = frame.read_register(name)
                e["initial"] = value.bytes

        except ValueError:
                report(False, f"failed to read reg: {name}")


def complete_and_diff(reg_map):
    """
    Let the program run to (almost) completion and then iterate
    through all the registers we know about and report which ones have
    changed.
    """
    # Let the program get to the end and we can check what changed
    b = gdb.Breakpoint("_exit")
    if b.pending: # workaround Microblaze weirdness
        b.delete()
        gdb.Breakpoint("_Exit")

    gdb.execute("continue")

    frame = gdb.selected_frame()
    changed = 0

    for e in reg_map.values():
        if "initial" in e:
            name = e["name"]
            reg = e["regnum"] if "hidden" in e else name
            old_bytes = e["initial"]

            try:
                new_val = frame.read_register(reg)
            except ValueError:
                report(False, f"failed to read {name} at end of run")
                continue

            # Some target descriptions include zero-width bookkeeping slots.
            # Comparing their gdb.Value objects can overflow GDB's type logic.
            new_bytes = new_val.bytes
            if new_bytes != old_bytes:
                print(f"{name} changes from {old_bytes.hex()} "
                      f"to {new_bytes.hex()}")
                changed += 1

    if (is_mips_target() and
            os.environ.get("MIPS_REGISTER_FIXTURE") == "1"):
        check_mips_abi_registers(frame)

    # as long as something changed we can be confident its working
    report(changed > 0, f"{changed} registers were changed")


def run_test():
    "Run through the tests"

    mips_target = is_mips_target()
    reg_map = fetch_xml_regmap()

    if reg_map is None:
        if mips_target:
            report(False, "MIPS target description is available to GDB")
        return

    if mips_target:
        check_mips_g_packet(reg_map)
    crosscheck_remote_xml(reg_map)
    initial_register_read(reg_map)
    complete_and_diff(reg_map)


main(run_test)
