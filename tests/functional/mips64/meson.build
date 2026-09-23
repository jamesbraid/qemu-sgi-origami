# SPDX-License-Identifier: GPL-2.0-or-later

test_mips64_timeouts = {
  # Nine bare-metal SMP scheduling checks, each its own short VM launch,
  # in one script (eight cells that depended on synthesized firmware
  # moved to qtests in the firmware deletion; the measured 41-42s below
  # is from before that move, on fourteen cells, so it is headroom now
  # rather than a current measurement). 240 gives better than four times
  # that for a heavily contended CI host; the previous unlisted default
  # of 90 left under half.
  'sn0_smp' : 240,
  # Ten short VM launches (three test classes -- three, three and four
  # cases respectively), most exercising twenty machine resets and the
  # last exercising twenty console power-down cycles, in one script
  # (test_sn_reset.py) merged from what were three separate scripts
  # (test_sn0_port_reset.py, test_sn0_elsc_reset.py, test_sn1_l1_reset.py):
  # a Hub port-reset register write, two full ELSC transactions
  # (authenticate, then "rst"), and one L1 UART frame (header,
  # destination, command, two check bytes, all escaped and flagged),
  # respectively, per reset. Each of the three kept its own measured total
  # on its own host load (18s at load 174, 10s at load 130, and the L1
  # section's own share of that order of magnitude), summing under 60s
  # together; each case still gives the guest 60s of its own before
  # declaring failure, so 900 (the three former budgets summed) covers all
  # ten running out that budget in sequence and still reporting which one
  # did, rather than the whole script being killed with nothing to read.
  'sn_reset' : 900,
  # Four methods in this one script now: the original two first-line-only
  # smoke boots (about a second each) plus two real boots through a
  # writable boot flash and the chassis/board/SPD identity records
  # (lib_origin300.py) to the stock PROM's own "ALL DIAGS PASSED" and
  # System Maintenance Menu -- the path the installed IRIX disk boots
  # through. Measured end to end against an already-built binary
  # (2026-09-22): all four methods together in 72s; the four-CPU menu
  # boot passed three runs in a row with no failure. 240 keeps well over
  # three times the measured total for a contended host.
  'origin300_prom' : 240,
  # One test method per origin200-matrix row, quick and thorough speeds
  # alike, in this one script (test_origin200_prom.py) -- merged from
  # what were four separate scripts (test_origin200_matrix.py,
  # test_origin200_matrix_thorough.py, test_prom_boot.py,
  # test_prom_copy_speed.py): every cell now boots the real PROM, so the
  # two fixed single-cell checks are the manifest's own smallest cell
  # (prom_full_boot_1cpu) with two more assertions on that one boot,
  # not a separate launch. 3120 sums the four absorbed scripts' own
  # budgets (300 + 1800 + 700 + 320) for the four cells now running in
  # sequence in this one script, the same shape linux_kernel's own
  # reasoning below keeps for several budgets run one after another.
  'origin200_prom' : 3120,
  # graphics-matrix's two rows (rad4_login_screen, rad4_login_screen_smp2):
  # boot IRIX 6.5.30 with RAD4 at its vendor-documented PCI slot, one CPU
  # and two, drive each to the console login prompt, wait a fixed settle
  # for xdm's own, independently-timed redraw (see test_graphics_matrix.py's
  # _settle_seconds -- longer on the 2-CPU row: pinning rr-kick-period-ns
  # above one CPU trades away wall-clock-to-guest-progress once the boot
  # itself is over), run a short input script, and check the resulting
  # screendump. Measured end to end against an already-built binary
  # (2026-09-17): the single-CPU row in 49s, the 2-CPU row (30s of that its
  # own settle) in 118s. 600 in each manifest row's own timeout column
  # leaves generous margin for a contended host reaching login at all; this
  # entry budgets both rows hitting that full 600s in sequence (1200s) plus
  # the same kind of margin on top for VM launch/shutdown and the
  # settle/input-script/screendump steps the measurements above show is not
  # usually needed, matching origin200_matrix_thorough's own reasoning for
  # its two real-boot cells below.
  'graphics_matrix_thorough' : 1400,
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
  # 128 MiB) with a scsi-tape and a scsi-cd attached, and run SGI's oldisk,
  # olmem, oltape and olcdrom online diagnostics in sequence on that one
  # boot, each to its own RSLT PASS. The two-node boot and all four
  # diagnostics are the long pole; BOOT_TIMEOUT_SECONDS in test_irix_diag.py
  # keeps a 60s margin under this -- the sum of this test's own former
  # budget (3600) plus the two absorbed oltape (1560) and olcdrom (1800)
  # legs, now run on the one boot instead of three.
  'irix_diag' : 6960,
  # Boot the plain installed IRIX disk (same 265s to root login as
  # irix_diag above -- root.qcow2 is the base root-diags.qcow2 is built
  # from) and require who -r/uname -aR to show run level 2 under the
  # expected release. BOOT_TIMEOUT_SECONDS in test_irix_installed_boot.py
  # keeps a 60s margin under this.
  'irix_installed_boot' : 960,
  # A full multiuser boot of the installed IRIX 6.5.30 disk on the real
  # PROM, then the eg (Alteon Tigon) adapter brought up and a
  # byte-checked 1 MiB TCP transfer, bulk TCP and UDP throughput runs
  # through ttcp, and eg0's own Ipkts receive counter read across the
  # TCP transfer -- the device-level receive proof that was
  # test_irix_eg_net.py's own distinguishing check, now folded onto
  # this boot instead of repeated on a second one. The boot is still
  # the long pole (real power-on diagnostics, kernel, init to a
  # console login); LOGIN_BUDGET_SECONDS in test_irix_eg.py keeps a
  # margin under this, and the traffic legs carry their own fixed
  # timeouts on top of it. Before: two targets, 1800 (irix_eg_net) and
  # 2400 (irix_eg_perf), summing 4200. After: one target, still 4200,
  # one boot instead of two.
  'irix_eg' : 4200,
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
}

tests_mips64_system_quick = [
  'sn0_smp',
  'sn_reset',
  'origin2000_cache_reset',
  # graphics-matrix carries no quick row yet (see graphics_matrix_thorough's
  # own comment below); registered anyway so a future device's quick
  # smoke-test row needs only a manifest line, not a meson.build edit.
  'graphics_matrix',
]

tests_mips64_system_thorough = [
  'origin200_prom',
  'graphics_matrix_thorough',
  'origin300_prom',
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
  'irix_installed_boot',
  'irix_eg',
  'irix_tg',
  'irix_tg_perf',
  'openbsd_login',
  'linux_kernel',
  'linux_multiuser',
  'openbsd_net_transfer',
  'openbsd_install',
  'xframe_pci',
]

# The unit tests of the lib_*.py helper modules the boot tests import,
# plus the one verdict companion that is a parser in its own right (the
# PROM's RSLT ledger). Plain unittest modules -- no QEMU, no assets, no
# qemu_test import -- so they run under the venv's bare python3 rather
# than through the qemu_test TAP machinery the lists above feed, and they
# belong to the quick suite: the whole set is seconds.
tests_mips64_unit = [
  'lib_evidence_capture',
  'lib_graphics_matrix',
  'lib_input',
  'lib_matrix',
  'lib_mips',
  'lib_net',
  'lib_prom',
  'lib_screen',
  'lib_translation_health',
  'prom_diagnostics_verdict',
]

foreach test : tests_mips64_unit
  test('func-mips64-' + test,
       python,
       args: [meson.current_source_dir() / 'test_' + test + '.py'],
       suite: ['func-quick', 'func-mips64'])
endforeach
