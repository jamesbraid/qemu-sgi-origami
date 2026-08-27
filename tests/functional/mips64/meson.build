# SPDX-License-Identifier: GPL-2.0-or-later

test_mips64_timeouts = {
  'malta' : 240,
  'mttcg_cause' : 60,
  # One test_<id> method per origin200-matrix row runs one after another in
  # this one script (test_origin200_matrix.py), each budgeted up to its own
  # cell.timeout before declaring FAIL: the asset-free smoke cell (120s)
  # plus the three real firmware-boot cells this budget was raised for
  # (700s each -- 4-CPU, two-node, mttcg), worst case all four running out
  # their own budget in sequence, plus VM launch/shutdown overhead. Matches
  # linux_kernel's own reasoning below for the same one-script,
  # several-budgets-in-sequence shape.
  'origin200_matrix' : 2500,
  'prom_boot' : 700,
  'prom_copy_speed' : 320,
  'prom_diagnostics' : 1800,
  # A single netboot pass, smaller than the two-pass factory-diagnostics
  # check's 1800s: BOOT_TIMEOUT_SECONDS in test_prom_sash.py keeps a 60s
  # margin under this.
  'prom_sash' : 900,
  # The 6.5.30 miniroot kernel is a 12 MiB image the PROM fetches over
  # SLIRP TFTP before it even enters, then boots to its release banner;
  # BOOT_TIMEOUT_SECONDS in test_irix_miniroot.py keeps a 60s margin under
  # this, the same margin test_prom_sash.py keeps against its own timeout.
  'irix_miniroot' : 900,
  # A single netboot pass plus the kernel's own primary-data-cache test
  # (announced as five minutes, about two emulated); BOOT_TIMEOUT_SECONDS in
  # test_prom_mdk.py keeps the usual 60s margin under this.
  'prom_mdk' : 1200,
  # The installed OpenBSD 6.9 disk's own boot to an authenticated root
  # login: a spike run took about 9 minutes under a contended host;
  # BOOT_TIMEOUT_SECONDS in test_openbsd_login.py keeps a 60s margin
  # under this, the same margin test_prom_sash.py keeps against its own
  # meson timeout.
  'openbsd_login' : 900,
  # Three test methods (one, two, and four CPUs) run one after another in
  # this one script. The one- and two-CPU boots are each budgeted up to
  # BOOT_TIMEOUT_SECONDS=90s in test_linux_kernel.py before declaring FAIL;
  # the four-CPU/two-node boot up to FOUR_CPU_BOOT_TIMEOUT_SECONDS=700s
  # (fixed instruction counting serializes all four CPUs, so it is far
  # slower). 1000s covers all three running out their own budget in
  # sequence plus VM launch/shutdown overhead, rather than the usual
  # single-BOOT_TIMEOUT-plus-a-bit margin this fork's single-test-method
  # files keep.
  'linux_kernel' : 1000,
  # The same boot as openbsd_login, with a network back end fitted, plus
  # a DHCP lease and three transfer legs on top of it. LOGIN_BUDGET_SECONDS
  # in test_openbsd_net_transfer.py keeps the same 840s login budget
  # openbsd_login proved sufficient; the margin above that here is wider
  # than the other tests' usual 60s, because the three legs each carry
  # their own fixed per-step timeout on top of the login budget (ported
  # from tools/net-transfer-diag) rather than sharing it.
  'openbsd_net_transfer' : 1400,
  # A full unattended OpenBSD/sgi 6.9 install (test_openbsd_install.py) is
  # 20-30 minutes under a quiet host, but a real run on a contended host
  # was still doing genuine work (confirmed via /proc/<pid>/io, not a
  # hang) in the final kernel-relink step past the 40-minute mark this
  # entry previously carried. 90 minutes gives that headroom;
  # INSTALL_TIMEOUT_SECONDS there keeps a 60s margin under this, the
  # same margin every other test in this directory keeps against its
  # own meson timeout.
  'openbsd_install' : 5400,
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
  'prom_copy_speed',
  'prom_diagnostics',
  'prom_sash',
  'prom_mdk',
  'irix_miniroot',
  'openbsd_login',
  'linux_kernel',
  'openbsd_net_transfer',
  'openbsd_install',
]
