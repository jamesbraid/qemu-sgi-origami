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
  # Three methods in this script: the original two first-line-only
  # smoke boots (about a second each) plus a four-CPU boot through a
  # writable boot flash and the chassis/board/SPD identity records
  # (lib_origin300.py) to the stock PROM's own "ALL DIAGS PASSED" and
  # System Maintenance Menu. The installed IRIX test checks the same
  # outcome for two CPUs. Before that test absorbed the two-CPU menu
  # boot, all four methods took 72s (2026-09-22); the four-CPU menu
  # boot passed three runs in a row. Keep 240s until the shorter script
  # is measured in CI.
  'origin300_prom' : 240,
  # One stock-PROM boot of the two-chassis pair to the menu: 364 s measured
  # against the debug build at host load 60 (test_origin300_two_chassis_
  # prom.py's own docstring); 1200 is that script's own class budget.
  'origin300_two_chassis_prom' : 1200,
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
  # Two stock-PROM boots to "ALL DIAGS PASSED", each a few minutes on a
  # loaded host; 900 is the test class's own budget per boot.
  'origin200_gigachannel_prom' : 1800,
  # One stock-PROM boot each to the command monitor's hinv -v, a few
  # minutes on a loaded host; 900 is each test class's own budget.
  'onyx2_deskside_prom' : 900,
  'onyx2_rack_prom' : 900,
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
  # The 6.5.30 miniroot kernel is a 12 MiB image the PROM fetches over
  # SLIRP TFTP before it even enters, then boots to its release banner;
  # BOOT_TIMEOUT_SECONDS in test_irix_miniroot.py keeps a 60s margin under
  # this.
  'irix_miniroot' : 900,
  # One target per irix-diag-matrix row (test_irix_diag.py and the
  # test_irix_diag_<flavour>.py beside it), each selectable on its own so a
  # diagnostics-only nightly job can run one or two rows. The nightly job
  # is cancelled at 200 minutes (.forgejo/workflows/nightly.yml), and no
  # timeout here is above 9000s, leaving the build, the asset downloads and
  # the result steps 3000s. Each timeout is the row's budget in
  # irix-diag-matrix plus 120s for its summary and the machines' shutdown,
  # and every wait inside a row (boots, shell steps, diagnostics, reboot,
  # stop) is cut to what is left of that budget, so a slow run marks its
  # remaining checks not run and still writes <row>-summary.json before
  # meson's timeout: 1500 + 120 = 1620 for origin200 and 2880 + 120 = 3000
  # for the other five rows. The slowest whole runs measured, with matrix
  # code not yet committed (2026-09-26), were 896s (origin200), 1755s
  # (two-chassis), 2382s (one-node), 3032s (shoehorn, before its reboot
  # and relaunch were dropped), 1934s (carrier) and 2608s (origin300,
  # including a 300s wait on its stalled reboot, #875, now 180s); the
  # one-node maximum came from a build carrying jamesb/qemu#866's missed
  # Compare matches, which slowed oldisk up to ninefold. With the committed
  # code on trunk 75f534457b (2026-09-27) the rows took 1071s, 1659s,
  # 2085s, 2060s, 2124s and 1835s of their budgets, in that order; the
  # one-node's and carrier's 2085s and 2124s were within 200s of the
  # 2280s they had, so they now have 2880s like the others.
  #
  # The Origin 300 rows gained olpci, olperi, olsio, olusb, olvst and
  # pandora, which took about 2000s together at two and four processors in
  # the project checkout's online-diagnostics runs (2026-09-23 to 27), 1792s
  # of it pandora's own thirty-minute default. With them the two- and
  # four-processor rows took 2657s and 2562s (2026-09-27, build-fast) and
  # have 5700s. torpedo took 5115s at four processors and 5974s at two, so
  # it has rows of its own at 8700s: a boot and torpedo alone. The
  # two-chassis row, which used to leave pandora out, took 1289s without it
  # after a boot of 318s, where another boot took 616s; on the pair torpedo
  # measured only 1634s, short enough to run inside the row instead of a
  # row of its own. With the promdata alias fix (a4c6ca9d8a) pandora also
  # passes on the pair, so the row now carries both: the single-chassis
  # rows' 5700s, which already covers pandora and everything else the pair
  # shares with them, plus torpedo's own measured 1634s, giving 7334s
  # rather than a fresh whole-row measurement of the three together.
  'irix_diag' : 3000,
  'irix_diag_origin200' : 1620,
  'irix_diag_origin2000_one_node' : 3000,
  'irix_diag_origin2000_pci_shoehorn' : 3000,
  'irix_diag_origin2000_pci_carrier' : 3000,
  'irix_diag_origin300' : 5820,
  'irix_diag_origin300_4cpu' : 5820,
  'irix_diag_origin300_pair' : 7454,
  'irix_diag_origin300_torpedo' : 8820,
  'irix_diag_origin300_4cpu_torpedo' : 8820,
  # Boot the plain installed IRIX disk (265s to root login when last
  # measured; root.qcow2 is the base root-diags.qcow2 is built from) and
  # require who -r/uname -aR to show run level 2 under the
  # expected release. BOOT_TIMEOUT_SECONDS in test_irix_installed_boot.py
  # keeps a 60s margin under this.
  'irix_installed_boot' : 960,
  # Boot the two-processor Origin 300 from its installed IRIX 6.5.30
  # disk through the stock IP35 PROM to a root shell and run level 2.
  'irix_origin300_installed_boot' : 960,
  # One IRIX 6.5.30 boot checks the painted SI greeter before login,
  # then carries through a graphical login to check the desktop:
  # test_mgras_desktop.py's SERIAL_TIMEOUT_SECONDS (1320, the boot plus a
  # root login and passwd on the serial console), GREETER_SETTLE_SECONDS
  # (300) for the greeter's colours and DESKTOP_SETTLE_SECONDS (600) for
  # the desktop's, 2220 in sequence; 2400 leaves the rest for launch,
  # typing and shutdown.
  'mgras_desktop' : 2400,
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
  # One target per smdk-matrix row (test_prom_smdk_origin300.py and the
  # test_prom_smdk_origin300_<row>.py beside it), each the row's budget
  # plus 120s, the same shape as the irix-diag-matrix rows above. With
  # sct and CPU_Test dropped and ndir added, the two- and four-processor
  # rows took 604s and 967s of their 2880s (2026-09-27, build-fast,
  # measured before ndir replaced sct; the two diagnostics' own times are
  # close enough that 2880s still holds). The pair row used to measure
  # 1111s running sct, scct, System_Level_Test and xbg without nmem or
  # ndir; with nmem needing about 3600s there to sync the two chassis and
  # ndir about 1500s, replacing sct and adding both to the row, 7200s
  # gives the row headroom over that addition rather than a fresh
  # whole-row measurement of the five diagnostics together.
  'prom_smdk_origin300' : 3000,
  'prom_smdk_origin300_4cpu' : 3000,
  'prom_smdk_origin300_pair' : 7320,
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
  # The OpenBSD boot reaches an authenticated root shell, then leases an
  # address and runs three transfer legs. LOGIN_BUDGET_SECONDS in
  # test_openbsd_net_transfer.py allows 840s for the login; the margin is wider
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
]

tests_mips64_system_thorough = [
  'origin200_prom',
  'origin200_gigachannel_prom',
  'onyx2_deskside_prom',
  'onyx2_rack_prom',
  'graphics_matrix_thorough',
  'origin300_prom',
  'origin300_two_chassis_prom',
  'prom_diagnostics',
  'prom_pod',
  'prom_fx',
  'prom_mdk',
  'prom_mdk_memory',
  'prom_mdk_instgen',
  'prom_mdk_router',
  'prom_smdk_origin300',
  'prom_smdk_origin300_4cpu',
  'prom_smdk_origin300_pair',
  'irix_miniroot',
  'irix_diag',
  'irix_diag_origin200',
  'irix_diag_origin2000_one_node',
  'irix_diag_origin2000_pci_shoehorn',
  'irix_diag_origin2000_pci_carrier',
  'irix_diag_origin300',
  'irix_diag_origin300_4cpu',
  'irix_diag_origin300_pair',
  'irix_diag_origin300_torpedo',
  'irix_diag_origin300_4cpu_torpedo',
  'irix_installed_boot',
  'irix_origin300_installed_boot',
  'mgras_desktop',
  'irix_eg',
  'irix_tg',
  'irix_tg_perf',
  'linux_kernel',
  'linux_multiuser',
  'openbsd_net_transfer',
  'openbsd_install',
  'xframe_pci',
]

subdir('roms')
if sgi_roms_available
  tests_mips64_system_thorough += ['sgi_roms']
  test_mips64_timeouts += {'sgi_roms': 18000}
endif

# The unit tests of the lib_*.py helper modules the boot tests import,
# plus the one verdict companion that is a parser in its own right (the
# PROM's RSLT ledger). These unittest modules need no QEMU or assets, but
# their imports use the same Meson Python environment as functional tests.
# They belong to the quick suite: the whole set is seconds.
tests_mips64_unit = [
  'lib_evidence_capture',
  'lib_graphics_matrix',
  'lib_host_cpus',
  'lib_input',
  'lib_irix_diag',
  'lib_matrix',
  'lib_mgras_regions',
  'lib_mips',
  'lib_net',
  'lib_origin300',
  'lib_prom',
  'lib_prom_smdk',
  'lib_screen',
  'lib_translation_health',
  'prom_diagnostics_verdict',
]

foreach test : tests_mips64_unit
  test_env = environment()
  test_env.set('MESON_BUILD_ROOT', meson.project_build_root())
  test_env.prepend('PYTHONPATH',
                   meson.project_source_root() / 'python',
                   meson.current_source_dir() / '..',
                   meson.current_source_dir(),
                   separator : ':')
  test('func-mips64-' + test,
       python,
       env: test_env,
       args: [meson.current_source_dir() / 'test_' + test + '.py'],
       suite: ['func-quick', 'func-mips64'])
endforeach
