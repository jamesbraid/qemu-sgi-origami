# SPDX-License-Identifier: GPL-2.0-or-later

test_mips64_timeouts = {
  'malta' : 240,
  'mttcg_cause' : 60,
  # Fourteen bare-metal SMP scheduling checks, each its own short VM
  # launch, in one script: the whole run took 41-42s on a moderately
  # loaded host (measured twice, 2026-08-28). 240 gives better than
  # four times that for a heavily contended CI host; the previous
  # unlisted default of 90 left under half.
  'sn0_smp' : 240,
  # Three short VM launches, twenty machine resets each: 18s for the whole
  # script on a host at load 174 (measured 2026-09-13). Each case gives the
  # guest 60s of its own before declaring failure, so 300 covers all three
  # running out that budget in sequence and still reporting which one did,
  # rather than the whole script being killed with nothing to read.
  'sn0_port_reset' : 300,
  # Three short VM launches, twenty resets each, two full ELSC transactions
  # (authenticate, then "rst") per reset rather than the Hub's single
  # register write: 10s for the whole script on a host at load 130
  # (measured 2026-09-14). Same reasoning as sn0_port_reset above for the
  # 300s budget.
  'sn0_elsc_reset' : 300,
  # Three short VM launches, twenty resets each, one L1 UART frame
  # (header, destination, command, two check bytes, all escaped and
  # flagged) per reset rather than a register poke. Same reasoning as
  # sn0_elsc_reset's budget for the same shape of exercise.
  'sn1_l1_reset' : 300,
  # One test method per origin200-matrix row whose speed column says quick
  # runs after another in this one script (test_origin200_matrix.py), each
  # budgeted up to its own cell.timeout before declaring FAIL: today the
  # asset-free smoke cell (120s), plus VM launch/shutdown overhead and room
  # for a second quick cell.
  'origin200_matrix' : 300,
  # The thorough rows of the same manifest, in test_origin200_matrix_thorough
  # .py: the three real firmware-boot cells (700s each -- 4-CPU, two-node,
  # mttcg), worst case all three running out their own budget in sequence,
  # plus VM launch/shutdown overhead. Matches linux_kernel's own reasoning
  # below for the same one-script, several-budgets-in-sequence shape.
  'origin200_matrix_thorough' : 2500,
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
  # Boot the installed IRIX disk on two chassis (four CPUs across two nodes,
  # 128 MiB) and run SGI's oldisk and olmem online diagnostics to a RSLT PASS.
  # The two-node boot and both diagnostics are the long pole; oldisk ~256s and
  # olmem ~430s of quiet on top of the boot. BOOT_TIMEOUT_SECONDS in
  # test_irix_diag.py keeps a 60s margin under this.
  'irix_diag' : 3600,
  # Boot the installed IRIX disk with a scsi-tape attached and run SGI's
  # oltape rw/thrash/block to a RSLT PASS (seconds on a fresh image). The
  # boot is the long pole, same as irix_diag; BOOT_TIMEOUT_SECONDS in
  # test_irix_oltape.py keeps a 60s margin under this.
  'irix_oltape' : 1560,
  # Boot the installed IRIX disk with a scsi-cd holding the generated data
  # CD and run olcdrom -device over it (pattern and random-access passes).
  # Above irix_oltape's budget for the CD read passes on top of the boot;
  # BOOT_TIMEOUT_SECONDS in test_irix_olcdrom.py keeps a margin under this.
  'irix_olcdrom' : 1800,
  # Boot the plain installed IRIX disk (same 265s to root login as
  # irix_diag above -- root.qcow2 is the base root-diags.qcow2 is built
  # from) and require who -r/uname -aR to show run level 2 under the
  # expected release. BOOT_TIMEOUT_SECONDS in test_irix_installed_boot.py
  # keeps a 60s margin under this.
  'irix_installed_boot' : 960,
  # A full multiuser boot of the installed IRIX 6.5.30 disk on the real
  # PROM, then the eg (Alteon Tigon) adapter brought up and a 128 KiB
  # payload pulled over it by TFTP and checked byte-for-byte. The boot is
  # the long pole (real power-on diagnostics, kernel, init to a console
  # login); LOGIN_BUDGET_SECONDS in test_irix_eg_net.py keeps a margin
  # under this, with the transfer legs on their own fixed timeouts on top.
  'irix_eg_net' : 1800,
  # The same full multiuser boot of the installed IRIX 6.5.30 disk as
  # irix_eg_net, with more on top: a byte-checked 1 MiB TCP transfer, then
  # bulk TCP and UDP throughput runs through ttcp. The boot is still the long
  # pole -- LOGIN_BUDGET_SECONDS in test_irix_eg_perf.py keeps a margin under
  # this -- and the three traffic legs carry their own fixed timeouts on top
  # of it, so this sits above irix_eg_net's budget rather than at it.
  'irix_eg_perf' : 2400,
  # The same full multiuser boot of the installed IRIX 6.5.30 disk, with the
  # tg (Broadcom BCM570x) adapter brought up instead, its link awaited, and
  # the SLIRP gateway pinged so tg1's own packet counters can be read back.
  # The boot is the long pole, the same as irix_eg_net; LOGIN_BUDGET_SECONDS
  # in test_irix_tg.py keeps a margin under this, with the bring-up steps on
  # their own fixed timeouts on top.
  'irix_tg' : 1800,
  # Same boot, then bulk transfers on top of it: throughput needs the traffic
  # to run long enough to be worth timing.
  'irix_tg_perf' : 2700,
  # Boot to the POD prompt plus sixteen immediate commands; BOOT_TIMEOUT
  # in test_prom_pod.py keeps the usual 60s margin under this.
  'prom_pod' : 900,
  # Netboot fx and a 100,000-block write-compare exercise (19s emulated);
  # BOOT_TIMEOUT in test_prom_fx.py keeps the usual 60s margin under this.
  'prom_fx' : 900,
  # A single netboot pass plus the kernel's own primary-data-cache test
  # (announced as five minutes, about two emulated); BOOT_TIMEOUT_SECONDS in
  # test_prom_mdk.py keeps the usual 60s margin under this.
  'prom_mdk' : 1200,
  # The full memory.mdk battery in one boot: eight tests, the two long
  # ones near twenty minutes each under a heavily contended host;
  # BOOT_TIMEOUT_SECONDS in test_prom_mdk_memory.py keeps the usual 60s
  # margin under this.
  'prom_mdk_memory' : 2500,
  # A thousand self-checking instgen passes on two CPUs took about forty
  # minutes under a heavily contended host; BOOT_TIMEOUT_SECONDS in
  # test_prom_mdk_instgen.py keeps the usual 60s margin under this.
  'prom_mdk_instgen' : 3600,
  # Two-chassis boot plus the router link test; red on current trunk (the
  # second hub is never discovered), bounded so the wedge costs one budget.
  'prom_mdk_router' : 1800,
  # The installed OpenBSD 6.9 disk's own boot to an authenticated root
  # login: a spike run took about 9 minutes under a contended host;
  # BOOT_TIMEOUT_SECONDS in test_openbsd_login.py keeps a 60s margin
  # under this, the same margin test_prom_sash.py keeps against its own
  # meson timeout.
  'openbsd_login' : 900,
  # Three test methods (one, two, and four CPUs) run one after another in
  # this one script, each now netbooting the kernel through the real PROM
  # rather than a direct -kernel load. The one- and two-CPU boots are each
  # budgeted up to BOOT_TIMEOUT_SECONDS=240s in test_linux_kernel.py
  # before declaring FAIL; the four-CPU/two-node boot up to
  # FOUR_CPU_BOOT_TIMEOUT_SECONDS=900s (fixed instruction counting
  # serializes all four CPUs, so it is far slower). 1500s covers all
  # three running out their own budget in sequence plus VM
  # launch/shutdown overhead, rather than the usual
  # single-BOOT_TIMEOUT-plus-a-bit margin this fork's single-test-method
  # files keep.
  'linux_kernel' : 1500,
  # test_linux_multiuser.py boots the networked Gentoo disk to a multiuser
  # login and probes the ssh service. BOOT_TIMEOUT_SECONDS=1200 there (the
  # verified milestone boot reached a networked login in 754-845s); 1320
  # keeps a margin over that plus the post-login sshd settle and the banner
  # probe, the same shape the other tests keep over their own budgets.
  'linux_multiuser' : 1320,
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
  # Boots the stock OpenBSD 6.9 disk with an xframe adapter attached and
  # reads only until PCI autoconfiguration reports it -- a real measured
  # run reached PASS in about 43s end to end (see BOOT_TIMEOUT_SECONDS's
  # own comment in test_xframe_pci.py); 360 keeps the usual 60s margin
  # over that file's own 300s budget.
  'xframe_pci' : 360,
  # ASSET-GATED SCAFFOLD (test_openbsd_xge.py): no xge-enabled kernel is
  # provisioned yet, so this always skips before ever launching a VM (see
  # that file's own docstring) -- this timeout is for once that asset
  # exists. Reuses test_openbsd_net_transfer.py's own 1400s entry: the
  # same login budget plus one transfer leg's own fixed timeout, not
  # three.
  'openbsd_xge' : 1400,
}

tests_mips64_system_quick = [
  'sn0_smp',
  'sn0_port_reset',
  'sn0_elsc_reset',
  'sn1_l1_reset',
  'mttcg_cause',
  'origin200_matrix',
  'origin2000_cache_reset',
  'origin2000_flash_mode',
]

tests_mips64_system_thorough = [
  'malta',
  'tuxrun',
  'origin200_matrix_thorough',
  'prom_boot',
  'prom_copy_speed',
  'prom_diagnostics',
  'prom_sash',
  'prom_pod',
  'prom_fx',
  'prom_mdk',
  'prom_mdk_memory',
  'prom_mdk_instgen',
  'prom_mdk_router',
  'irix_miniroot',
  'irix_diag',
  'irix_oltape',
  'irix_olcdrom',
  'irix_installed_boot',
  'irix_eg_net',
  'irix_eg_perf',
  'irix_tg',
  'irix_tg_perf',
  'openbsd_login',
  'linux_kernel',
  'linux_multiuser',
  'openbsd_net_transfer',
  'openbsd_install',
  'xframe_pci',
  'openbsd_xge',
]

# The boot tests' pure-logic companions: the *_verdict.py suites that pin
# each test's pass condition against real captured transcripts, and the
# unit tests of the lib_*.py helper modules they import. Plain unittest
# modules -- no QEMU, no assets, no qemu_test import -- so they run under
# the venv's bare python3 rather than through the qemu_test TAP machinery
# the lists above feed, and they belong to the quick suite: the whole set
# is seconds.
tests_mips64_unit = [
  'lib_matrix',
  'lib_prom',
  'lib_translation_health',
  'irix_eg_net_verdict',
  'irix_eg_perf_helpers',
  'irix_eg_perf_verdict',
  'irix_tg_verdict',
  'irix_tg_perf_verdict',
  'linux_kernel_verdict',
  'linux_multiuser_verdict',
  'openbsd_install_verdict',
  'openbsd_login_verdict',
  'openbsd_net_transfer_verdict',
  'openbsd_xge_verdict',
  'prom_boot_verdict',
  'prom_copy_speed_verdict',
  'prom_diagnostics_verdict',
  'prom_fx_verdict',
  'irix_diag_verdict',
  'irix_oltape_verdict',
  'irix_olcdrom_verdict',
  'prom_mdk_instgen_verdict',
  'prom_mdk_memory_verdict',
  'prom_mdk_router_verdict',
  'prom_mdk_verdict',
  'prom_pod_verdict',
  'prom_sash_verdict',
  'xframe_pci_verdict',
]

foreach test : tests_mips64_unit
  test('func-mips64-' + test,
       python,
       args: [meson.current_source_dir() / 'test_' + test + '.py'],
       suite: ['func-quick', 'func-mips64'])
endforeach
