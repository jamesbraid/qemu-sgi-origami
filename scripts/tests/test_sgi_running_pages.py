#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Keep the SGI running pages in step with the machine catalogue.

The pages render their tables from hw/mips/sgi/sgi-machines.json when the
manual is built. This checks what is written by hand: the command lines,
the product coverage and the functional tests the pages name. It needs no
build. tests/functional/mips64/test_sgi_manual_commands.py starts the same
commands on a built emulator.
"""

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import running_doc as rd

PAGES = {name: (rd.PAGES / f"{name}.rst").read_text()
         for name in (*rd.FAMILIES, "running")}
CAT = rd.load()


class RunningPageTests(unittest.TestCase):
    def test_every_command_matches_the_catalogue(self):
        problems = []
        for name, text in PAGES.items():
            problems += rd.static_problems(text, name, CAT)
        self.assertEqual(problems, [])

    def test_each_page_has_a_command(self):
        for name in rd.FAMILIES:
            with self.subTest(page=name):
                self.assertTrue(
                    [c for c in rd.commands(PAGES[name])
                     if c[0] == "qemu-system-mips64"],
                    "a running page shows a qemu-system-mips64 command")

    def test_every_product_is_covered_with_its_tables(self):
        self.assertEqual(rd.coverage_problems(PAGES, CAT), [])

    def test_tests_named_by_the_pages_exist_and_are_registered(self):
        text = "\n".join(PAGES.values())
        self.assertIn(":test:`", text)
        self.assertEqual(rd.test_references(text), [])

    def test_manual_commands_test_is_registered(self):
        meson = (ROOT / "tests/functional/mips64/meson.build").read_text()
        self.assertTrue("'sgi_manual_commands'" in meson,
                        "register sgi_manual_commands in meson.build")

    def test_every_generated_table_renders(self):
        for page, items in rd.FAMILIES.items():
            for product in items:
                for kind in ("machines", "inputs"):
                    with self.subTest(product=product, kind=kind):
                        self.assertIn("list-table", rd.render(kind, product))

    def test_generated_launch_commands_match_the_catalogue(self):
        for items in rd.FAMILIES.values():
            for product in items:
                if product == "o2":
                    continue
                with self.subTest(product=product):
                    text = rd.render("launch", product)
                    self.assertEqual(
                        rd.static_problems(text, product, CAT), [])
                    self.assertEqual(len(rd.commands(text)), 2)

    def test_the_o2_has_no_generated_launch(self):
        with self.assertRaises(ValueError):
            rd.render("launch", "o2")

    def test_unknown_product_is_refused(self):
        with self.assertRaises(ValueError):
            rd.render("machines", "indy")

    def test_a_stale_command_is_caught(self):
        stale = ("::\n\n    qemu-system-mips64 -M origin200,"
                 "topology=origin200-quad -bios ip27prom.img\n"
                 "    qemu-system-mips64 -M origin200 -cpu R12000 "
                 "-bios ip27prom.img\n"
                 "    qemu-system-mips64 -M origin200 -m 100M "
                 "-bios missing.img\n"
                 "    qemu-system-mips64 -M indy\n")
        problems = rd.static_problems(stale, "stale", CAT)
        self.assertEqual(len(problems), 5, problems)

    def test_a_product_without_a_page_is_caught(self):
        pages = dict(PAGES)
        del pages["running-ip32"]
        self.assertTrue(any("o2" in p for p in
                            rd.coverage_problems(pages, CAT)))

    def test_a_missing_test_is_caught(self):
        self.assertEqual(len(rd.test_references(":test:`no_such_test`")), 2)

    def test_a_test_only_named_by_a_timeout_is_not_a_listed_test(self):
        # irix_performance is a benchmark: a quoted name in meson.build,
        # but in neither test list.
        self.assertEqual(
            len(rd.test_references(":test:`irix_performance`")), 1)

    def test_tests_are_sorted_into_suites(self):
        self.assertEqual(rd.test_suite("octane_flash_execution"), "quick")
        self.assertEqual(rd.test_suite("irix_installed_boot"), "thorough")
        self.assertEqual(rd.test_suite("sgi_manual_commands"), "quick")
        self.assertIsNone(rd.test_suite("no_such_test"))

    def test_facts_follow_the_catalogue(self):
        cat = {"offerings": [{
            "product": "p", "topology": "t", "default-topology": "t",
            "processor": {"part": "R12000"},
            "memory-per-node-mib": {"accepted": [256]}}]}
        text = rd.render("facts", "p", cat)
        self.assertIn("names R12000", text)
        self.assertIn("exactly 256 MiB", text)
        cat["offerings"][0]["processor"]["part"] = "R14000"
        self.assertIn("names R14000", rd.render("facts", "p", cat))
        del cat["offerings"][0]["default-topology"]
        self.assertIn("no default topology", rd.render("facts", "p", cat))

    def test_continuation_lines_join_as_the_shell_does(self):
        text = "::\n\n    qemu-system-mips64 -M o2,a=1,\\\n    b=2 \\\n        -m 1G\n"
        self.assertEqual(rd.commands(text),
                         [["qemu-system-mips64", "-M", "o2,a=1,b=2", "-m",
                           "1G"]])

    def test_start_argv_keeps_stdio_for_the_monitor_only(self):
        argv = rd.start_argv(["qemu-system-mips64", "-M", "octane",
                              "-serial", "stdio", "-monitor", "none"])
        self.assertEqual(argv[-3:], ["-S", "-qmp", "stdio"])
        self.assertEqual(argv.count("stdio"), 1)


if __name__ == "__main__":
    unittest.main()
