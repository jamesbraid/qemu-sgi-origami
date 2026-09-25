"""Contract tests for the fork's qtest registration check."""

import unittest
import runpy
from pathlib import Path

checker = runpy.run_path(str(Path(__file__).with_name('check-qtest-registered.py')))
guarded_names = checker['guarded_names']
registration_names = checker['registration_names']


class RegistrationTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
