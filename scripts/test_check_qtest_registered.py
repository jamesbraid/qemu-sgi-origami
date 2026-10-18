# SPDX-License-Identifier: BSD-3-Clause
"""Contract tests for the fork's qtest registration check."""

import unittest
import runpy
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

checker = runpy.run_path(str(Path(__file__).with_name('check-qtest-registered.py')))
group_problems = checker['group_problems']
guarded_names = checker['guarded_names']
registration_names = checker['registration_names']
main = checker['main']


class RegistrationTests(unittest.TestCase):
    def check_tree(self, meson, sources):
        with TemporaryDirectory() as directory:
            qtest = Path(directory)
            (qtest / 'meson.build').write_text(meson)
            for source in sources:
                (qtest / f'{source}.c').write_text('int main(void) { return 0; }\n')
            output = StringIO()
            with mock.patch.dict(main.__globals__, {'QTEST': qtest}), \
                 redirect_stdout(output):
                result = main()
            return result, output.getvalue()

    def test_timeout_entry_is_not_a_registration(self):
        meson = """slow_qtests = {
  'sn0-missing-test': 120,
}
qtests_mips64 = (config_all_devices.has_key('CONFIG_SGI_SN0') ?
  ['sn0-present-test'] : [])
"""
        names = registration_names(meson)
        self.assertIn('sn0-present-test', names)
        self.assertNotIn('sn0-missing-test', names)

    def test_suite_tag_is_not_a_qtest_registration(self):
        meson = """qtests_mips64 = (config_all_devices.has_key('CONFIG_SGI_SN0') ?
  ['sn0-present-test'] : [])
sgi_suites = ['sgi', 'sgi-qtest']
"""
        names = registration_names(meson)
        self.assertIn('sn0-present-test', names)
        self.assertNotIn('sgi-qtest', names)
        status, output = self.check_tree(meson, ('sn0-present-test',))
        self.assertEqual(status, 0, output)

    def test_wrong_guard_does_not_register_the_device_test(self):
        meson = """qtests_mips64 = (config_all_devices.has_key('CONFIG_MALTA') ?
  ['sn0-wrong-test'] : []) + \\
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
  ['sn0-present-test'] : [])
"""
        self.assertEqual(guarded_names(meson, r'CONFIG_SGI_SN0'),
                         {'sn0-present-test'})

    def test_commented_guard_is_not_a_registration(self):
        meson = """# (config_all_devices.has_key('CONFIG_SGI_SN0') ?
#   ['sn0-retired-test'] : [])
"""
        self.assertEqual(guarded_names(meson, r'CONFIG_SGI_SN0'), set())

    def test_guarded_and_unguarded_copies_fail(self):
        meson = """qtests_mips64 = \\
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-duplicate-test'] : []) + \\
  ['sn0-duplicate-test']
"""
        status, output = self.check_tree(meson, ('sn0-duplicate-test',))
        self.assertEqual(status, 1)
        self.assertIn('sn0-duplicate-test is registered more than once', output)

    def test_conditional_rewrite_is_not_a_duplicate(self):
        meson = """sn1_xbridge_qtests = \\
  (config_all_devices.has_key('CONFIG_SGI_XBRIDGE') ?
    ['sn1-xbridge-chip-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES')
  sn1_xbridge_qtests = []
elif not config_all_devices.has_key('CONFIG_PCI')
  sn1_xbridge_qtests = ['sn1-xbridge-chip-test']
endif
qtests_mips64 = sn1_xbridge_qtests
"""
        status, output = self.check_tree(meson, ('sn1-xbridge-chip-test',))
        self.assertEqual(status, 0, output)



class GroupTests(unittest.TestCase):
    SOURCE = """
    qtest_add_func("sn1/x/plain", test_plain);
    qtest_add_func("sn1/x/"
                   "split", test_split);
    if (g_test_slow()) {
        qtest_add_data_func("sn1/x/suite/one", NULL, test_one);
    }
    qtest_add_func("sn1/x/suite/two", test_two);
"""

    def problems(self, groups):
        return group_problems('sn1-x-test', groups, self.SOURCE)

    def test_cases_and_a_suite_each_in_one_group(self):
        self.assertEqual(self.problems("""# A comment line.
a /mips64/sn1/x/plain
a /mips64/sn1/x/split
b /mips64/sn1/x/suite
"""), [])

    def test_case_in_no_group_fails(self):
        self.assertEqual(self.problems("""a /mips64/sn1/x/plain
b /mips64/sn1/x/suite
"""), ['sn1-x-test case sn1/x/split is in no group'])

    def test_case_in_two_groups_fails(self):
        self.assertEqual(self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/split
a /mips64/sn1/x/suite/one
b /mips64/sn1/x/suite
"""), ['sn1-x-test case sn1/x/suite/one is in more than one group'])

    def test_unregistered_path_fails(self):
        self.assertIn('sn1-x-test-groups.txt names sn1/x/gone, which '
                      'sn1-x-test does not register',
                      self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/split
a /mips64/sn1/x/gone
b /mips64/sn1/x/suite
"""))

    def test_malformed_line_fails(self):
        self.assertIn("sn1-x-test-groups.txt:1 is not 'GROUP /TARGET/PATH'",
                      self.problems("a\n"))


class GeneratedGroupTests(unittest.TestCase):
    SOURCE = """
    qtest_add_func("sn1/x/plain", test_plain);
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *path = g_strdup_printf("sn1/x/made/%u", i);

        add_data_case(path, GUINT_TO_POINTER(i), test_made);
    }
    add_modes("sn1/x/host/%u",
              "sn1/x/"
              "cpu/%u", test_mode);
    add_data_case("sn1/x/made/extra", NULL, test_extra);
    g_test_message("%u cases", count);
"""

    def problems(self, groups, source=SOURCE):
        return group_problems('sn1-x-test', groups, source)

    def test_templated_suites_each_in_one_group(self):
        self.assertEqual(self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/made
b /mips64/sn1/x/host
b /mips64/sn1/x/cpu
"""), [])

    def test_templated_suite_in_no_group_fails(self):
        self.assertEqual(self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/made
b /mips64/sn1/x/host
"""), ['sn1-x-test suite sn1/x/cpu is in no group'])

    def test_templated_suite_in_two_groups_fails(self):
        self.assertIn('sn1-x-test suite sn1/x/host is in more than one group',
                      self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/made
a /mips64/sn1/x/host
b /mips64/sn1/x/host
b /mips64/sn1/x/cpu
"""))

    def test_templated_case_alone_fails(self):
        # Its name is not known, so a group cannot take it by name.
        problems = self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/made
b /mips64/sn1/x/host/0
b /mips64/sn1/x/cpu
""")
        self.assertIn('sn1-x-test suite sn1/x/host is in no group', problems)
        self.assertIn('sn1-x-test-groups.txt names sn1/x/host/0, which '
                      'sn1-x-test does not register', problems)

    def test_conversion_in_suite_fails(self):
        source = self.SOURCE + """
    g_strdup_printf("sn1/x/%s/%u", suite, i);
"""
        self.assertIn('sn1-x-test template sn1/x/%s/%u hides its suite',
                      self.problems("""a /mips64/sn1/x/plain
a /mips64/sn1/x/made
b /mips64/sn1/x/host
b /mips64/sn1/x/cpu
""", source))


if __name__ == '__main__':
    unittest.main()
