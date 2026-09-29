"""Contract tests for the fork's qtest registration check."""

import unittest
import runpy
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

checker = runpy.run_path(str(Path(__file__).with_name('check-qtest-registered.py')))
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
if not config_all_devices.has_key('CONFIG_SGI_XIO_TEST_TARGET')
  sn1_xbridge_qtests = []
elif not config_all_devices.has_key('CONFIG_SGI_BRIDGE_DMA_TEST')
  sn1_xbridge_qtests = ['sn1-xbridge-chip-test']
endif
qtests_mips64 = sn1_xbridge_qtests
"""
        status, output = self.check_tree(meson, ('sn1-xbridge-chip-test',))
        self.assertEqual(status, 0, output)


if __name__ == '__main__':
    unittest.main()
