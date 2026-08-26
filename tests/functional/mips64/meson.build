# SPDX-License-Identifier: GPL-2.0-or-later

test_mips64_timeouts = {
  'malta' : 240,
  'mttcg_cause' : 60,
  'origin200_matrix' : 180,
  'prom_boot' : 700,
  'diag' : 1800,
  # A single netboot pass, smaller than the two-pass factory-diagnostics
  # check's 1800s: BOOT_TIMEOUT_SECONDS in test_prom_sash.py keeps a 60s
  # margin under this.
  'prom_sash' : 900,
  # Two test methods (one and two CPUs) run one after another in this one
  # script, each budgeted up to BOOT_TIMEOUT_SECONDS=90s in
  # test_linux_kernel.py before declaring FAIL; 240s covers both running
  # out their own budget in sequence plus VM launch/shutdown overhead,
  # rather than the usual single-BOOT_TIMEOUT-plus-a-bit margin this
  # fork's single-test-method files keep.
  'linux_kernel' : 240,
}

tests_mips64_system_quick = [
  'sn0_smp',
  'mttcg_cause',
  'origin200_matrix',
]

tests_mips64_system_thorough = [
  'malta',
  'tuxrun',
  'prom_boot',
  'diag',
  'prom_sash',
  'linux_kernel',
]
