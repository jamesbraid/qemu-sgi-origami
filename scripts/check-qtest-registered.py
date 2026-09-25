#!/usr/bin/env python3
"""Reject qtest sources that are absent from the Meson test lists."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
QTEST = ROOT / "tests" / "qtest"
FAMILIES = ("sn0-", "sn1-", "sgi-", "mips64-")
# These device tests kept their upstream-style names when they moved onto
# the SGI Bridge test fixture. New SGI tests use the sgi- family above.
BRIDGE_CARD_TESTS = ("bcm570x-test", "tigon-test", "xframe-test",
                     "usb-lucent-ohci-test")
QOS_CARD_SOURCES = ("qlogic-isp-test.c", "scsi-tape-test.c")
FAMILY_GUARDS = {
    "sn0-": r"CONFIG_SGI_SN0",
    "sn1-": r"CONFIG_SGI_(?:SN1|BEDROCK(?:_[A-Z0-9_]+)?|L1|XBRIDGE)",
    "mips64-": r"CONFIG_MALTA",
}


def registration_names(meson: str) -> set[str]:
    text = "\n".join(line for line in meson.splitlines()
                     if not line.lstrip().startswith("#"))
    # A timeout or scheduling entry does not put a binary in a test suite.
    for table in ("slow_qtests", "qtest_priorities"):
        text = re.sub(rf"(?ms)^{table}\s*=\s*\{{.*?^\}}", "", text)
    return set(re.findall(r"'([^']+)'", text))


def guarded_names(meson: str, config_pattern: str) -> set[str]:
    """Names in a test list selected by the expected device guard."""
    meson = "\n".join(line for line in meson.splitlines()
                      if not line.lstrip().startswith("#"))
    blocks = re.finditer(
        rf"(?:{config_pattern})'\s*\)\s*\?\s*\[(.*?)\]", meson, re.S
    )
    return {name for block in blocks
            for name in re.findall(r"'([^']+)'", block.group(1))}


def main() -> int:
    meson = (QTEST / "meson.build").read_text()
    names = registration_names(meson)
    problems = []
    for prefix in FAMILIES:
        sources = {p.stem for p in QTEST.glob(f"{prefix}*-test.c") if p.is_file()}
        registered = {n for n in names if n.startswith(prefix)}
        problems.extend(f"{n}.c is not registered" for n in sources - registered)
        problems.extend(f"{n} has no source" for n in registered - sources)
        if prefix in FAMILY_GUARDS:
            guarded = guarded_names(meson, FAMILY_GUARDS[prefix])
            problems.extend(f"{n} is outside its device guard"
                            for n in (sources & registered) - guarded)
    for name in BRIDGE_CARD_TESTS:
        if (QTEST / f"{name}.c").is_file() and name not in names:
            problems.append(f"{name}.c is not registered")
        if name in names and not (QTEST / f"{name}.c").is_file():
            problems.append(f"{name} has no source")
    qos_sources = re.search(r"(?s)qos_test_ss\.add\(\s*(.*?)\n\)", meson)
    qos_names = set(re.findall(r"'([^']+)'", qos_sources.group(1))) \
        if qos_sources else set()
    for name in QOS_CARD_SOURCES:
        if (QTEST / name).is_file() and name not in qos_names:
            problems.append(f"{name} is not in the qos-test sources")
        if name in qos_names and not (QTEST / name).is_file():
            problems.append(f"{name} has no source")
    for problem in sorted(problems):
        print(f"FAIL {problem}")
    if problems:
        return 1
    print("qtest sources are registered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
