#!/usr/bin/env python3
#
# SI/MGRAS graphical greeter: boot the Origin 2000's built-in graphics to
# the IRIX icon-chooser and require the 2D engine to have actually
# painted it
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The Origin 2000's built-in SI graphics (device sgi-mgras) gained a real 2D
paint path this task cycle: a working color map, glyph text, byte-transfer
icons and the DMA/copy engine behind them. Nothing in this fork's
functional-test suite guarded any of it -- test_graphics_matrix.py's own
manifest is architecturally scoped to a pluggable `-device` card on a
single-node Origin 200 (see lib_graphics_matrix.py's own GraphicsCell,
fixed at one node) and cannot express a built-in device on the Origin
2000's default four-node topology, so it was never extended here. This
test exists so a paint regression in any of those pieces fails a required
check instead of only showing up the next time someone looks at a
screenshot by hand.

Unlike test_irix_installed_boot.py, this test does not log in: the icon
chooser is itself the thing under test, and reaching it needs nothing
past the boot that produces it. The serial console's own `login:` prompt
is the existing suite's established signal that the graphical login
screen should also be about ready (test_graphics_matrix.py's own scenario
waits on the same text for the same reason); its own screendump settle
poll exists because that text and the guest's independently-timed X
redraw are two unsynchronized events, not one -- see
poll_screendump_until_checks_pass's own docstring in lib_screen.py, which
this test reuses rather than reimplementing the same wait.

Machine profile: `origin2000,topology=origin2000-one-node,debug-leds=off`
(SI attaches to the topology's own xio-slot-3 automatically --
sgi_machine_build_mgras() in hw/mips/sgi/sn-machine.c -- unlike a
`-device`-plugged card; the one-node module's own catalogue entry in
hw/mips/sgi/sn-catalogue.json already bakes in an io6g BaseIO, so unlike
the default four-node topology this needs no `baseio-preset=` override
for the second IOC3 an interactive session's mouse needs), `-cpu R10000
-smp 2 -m 256`, multi-threaded TCG, `-S` then a `cont` once the machine
has launched (test_irix_diag.py's own `_launch()` uses the
same pause-then-continue for every Origin 2000 topology it starts,
including this one, against the RESET event a quick early PROM reset can
raise before the QMP client's capabilities reply lands). This test ran on
the default four-node topology (`baseio-preset=io6g`, `-smp 8`) until
QEMU commit 2324ead1d3 ("hw/misc/sgi-xbow: keep a restored link's routes
open through its reset", landed as PR 833): a guest link reset withdrew
the link's routes for the whole reset pulse, and IRIX 6.5.30's SI reset
path does not poll link status the way its generic widget-reset path
does, so a board access during that window could fall through to the
Hub's error floor. That was an intermittent, host-timing-sensitive fault
on any topology carrying the SI head, including this one; independently
observed against this exact one-node profile as a data bus error at PC
0xc000000000377ebc, widget 0xf offset 0x20100 -- an exact match to the
commit's own cited fault. With the fix landed, this test moves to the
simpler one-node module rather than staying on the four-node one.
tools/origami_lib/irix.py's own general Origin 2000 launch path uses the
same `tcg,thread=multi` for any multi-CPU IRIX boot; its interactive
`node_prom_drives()` path additionally wires a per-node pflash drive so
PROM state survives between separate launches, which this test does not
need (each run starts from the same golden, unwritten disk) and does not
use -- QEMU's own Hub flash loader (sgi_machine_load_flash() in
hw/mips/sgi/sn-machine.c) fills a node's boot flash from `-bios` whenever
no pflash drive already backs it, the same plain path
test_irix_diag_origin2000_one_node.py's boot uses against this
identical topology.
docs/notebooks/si-target-first-chooser-20260921.md's "Live chooser" first
recorded reaching this same chooser on the four-node profile (run
irix-20260922T032556-b7eda4, `origin2000` compatibility composition,
BaseIO6G, eight R10000s); that notebook's own run record shows the paint
pipeline renders correctly on landed code (PR 717/732/735), not just
under replay instrumentation. The screendump geometry this file checks
(see below) is SI's own framebuffer layout, independent of node or CPU
count, so the four-node capture's measurements still apply here; this
test's own qualification runs are the evidence for the one-node case.

root.qcow2, the generic install disk this test used at first, has no SI
graphics configured and produced exactly the degenerate capture the
paragraph below used to describe: a black frame with one stray painted
icon and no gray panel anywhere. That was not a 2D-engine regression --
it was the wrong disk. root-si.qcow2 is a dedicated SI-configured
install (provenance: reflinked from si-graphics-install.qcow2, the
project's SI install golden, sha256 9773e3a13168118d5f836a850449796860
b28e390593bed66c78f4cf2c7e4cbd; booted once on this test's own machine
profile to confirm it reaches the greeter, then stopped without logging
in). Booting it on this same profile (origin2000,baseio-preset=io6g,
4 nodes, 8 CPUs, 256 MiB) produces a completely different, correctly
structured frame: a centered gray chooser panel (roughly (329, 201) to
(936, 665)) holding four account entries in a row -- root, EZsetup,
demos, guest -- each a diamond icon (root's rendered black, the other
three white) except EZsetup, whose icon is a real shaded 3D box with
"sgi" lettering and a diagonal fold line, and each with its own label
box below it.

The screendump checks below and their thresholds come from directly
measuring that capture with a pixel-counting script (analyze_candidate2.py,
kept under this task's own scratch directory, not committed -- the
pixel-level provenance a future contributor would want lives in this
docstring instead). Three regions, three structural properties:

  - The panel itself: rect (329, 201, 608, 465), the gray fill's own
    measured bounding box inset a few pixels from its border. 98.9% of
    its pixels have some channel above 16 (a black frame -- the old
    disk's own failure mode -- would collapse this near 0), and it
    carries 231 distinct colors (a flat unpainted fill would carry
    approximately 1).
  - The EZsetup icon: rect (459, 214, 98, 98) -- the same numeric rect
    the old single-icon capture used, because this account's icon sits
    at the same screen position on both disks. 230 distinct colors
    (shading, the black outline, the fold line, the "sgi" text) and a
    6.1% dark-pixel (channel <= 64) fraction against the icon's mostly
    white fill -- a regression that flattened the artwork back into a
    solid fill would drop both toward the degenerate single-color case.
    A later DAC gamma-correction commit (landed on trunk after this
    capture) remapped raw pixel values to displayed brightness and, on
    a rebase-and-rerun, dropped what this same rect measures at
    channel <= 64 to 2.1% -- the same shading pixels are still there,
    the gamma curve just spread them across a wider brightness range.
    ICON_DARK_MAX_CHANNEL below reflects that remeasurement.
  - A label-row strip: rect (376, 319, 388, 18), crossing the tail of
    root's label box through most of guest's, alternating the panel's
    own gray, each label's lighter background, and black glyph text.
    4 distinct colors and a 14.3% dark-pixel fraction (the glyph
    strokes) against the label boxes' light fill.

Deliberately not asserted: a specific hue or absolute RGB range for any
region, or that all four icons render as boxes (only EZsetup's does on
this capture too -- a separate, open question about the other three
accounts' artwork that this test does not depend on). What should hold
across an ordinary color-pipeline change is structure: each region is
substantially painted and internally varied, so a regression that
flattened a real bitmap or glyph edge back into a flat fill still fails
even if the surviving fill's own color shifted. LitPixelFractionCheck
(panel, dark-background-relative-to-content polarity) and
DarkPixelFractionCheck (icon and label row, light-background polarity --
see lib_screen.py's own docstring for why the fraction check's direction
has to match each region's own background) assert the paint-coverage
half of that; ColorVarietyCheck asserts the internal-variety half in all
three regions, since a fraction check alone can still pass a region that
lost its real bitmap but kept a uniformly bright (or dark) flat fill.
NotAllBlackCheck guards the coarsest failure, an entirely unpainted
screen.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import (FORK_ROOT, MACHINES, drive_console, require_assets,
                      run_record, utc_now)
from lib_screen import (ColorVarietyCheck, DarkPixelFractionCheck,
                        LitPixelFractionCheck, NotAllBlackCheck,
                        ScreenCheckError, poll_screendump_until_checks_pass)

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
# root.qcow2 (the generic install disk) has no SI/MGRAS graphics
# configured and never reaches a painted greeter at all -- reaching one
# needs an SI-configured install, the same way RAD4's own functional test
# needs root-rad4.qcow2 rather than the generic disk. root-si.qcow2 is
# that dedicated asset (see this file's own docstring for its
# provenance); until it lands in the manifest, require_assets() below
# fails loudly (ORIGAMI_ASSETS set, file missing) rather than silently
# skipping, the same as any other unprovisioned asset.
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-si.qcow2')

# The Origin 2000's one-node module: one node, two CPUs, matching
# the origin2000_one_node row of irix-diag-matrix.
# The one-node topology's own catalogue entry in sn-catalogue.json
# already bakes in an io6g BaseIO, so unlike the default four-node
# topology this machine string needs no baseio-preset= override for the
# second IOC3 an interactive session's keyboard and mouse need; SI itself
# attaches automatically to the topology's xio-slot-3
# (sgi_machine_build_mgras() in hw/mips/sgi/sn-machine.c), independent of
# that preset either way. This test ran on the default four-node topology
# (baseio-preset=io6g, -smp 8) until QEMU commit 2324ead1d3 fixed the
# Crossbow link-reset race that made any SI-carrying topology intermittently
# fault during boot -- see this file's own docstring.
MACHINE = MACHINES['origin2000'] + ',topology=origin2000-one-node,debug-leds=off'
SMP = '2'
MEMORY_MIB = 256

# BOOT_TIMEOUT_SECONDS for this profile (one node, two CPUs, 256 MiB, the
# graphics stack initializing before the console even prints its login
# prompt). test_irix_diag_origin2000_one_node.py -- the same topology,
# a lighter root-diags.qcow2 disk and no graphical greeter to wait for --
# gives its boot a 1500s budget.
# This test's own four-node predecessor measured 282s-12min real to the
# same login: prompt across host-load conditions on eight CPUs. Measured
# against this one-node profile instead (three qualification runs on the
# rebased tree, after QEMU commit 2324ead1d3 fixed the link-reset race
# noted above): whole-test wall time, boot through the final screendump
# check, was 124s, 126s and 142s -- well under even the four-node
# profile's own best case, not just its worst. One node with a quarter
# the CPUs boots faster here, not slower: this profile skips the other
# three nodes' own SMP bring-up and link-up entirely rather than doing
# proportionally less of it. 1200 keeps roughly 8x headroom above the
# slowest of those three runs for host-load variance rather than shaving
# to the observed minimum; see the meson.build entry for this test's own
# overall budget.
BOOT_TIMEOUT_SECONDS = 1200

# Drive the PROM menu to Start System, then wait for the serial console's
# own login prompt -- see this file's own docstring for why that, not a
# graphical-state check, is the existing suite's signal that the greeter
# should also be about ready.
DRIVER_STEPS = (
    (r"Option\?", "1\n"),
)
LOGIN_PROMPT_RE = re.compile(rb"login:")

# The screendump settle poll's own interval and timeout. attempt-4 (root.qcow2,
# the wrong disk) reached the serial login: prompt and then held the icon
# rect at pure black for the entire original 30.0s/38.00s-elapsed window
# with zero movement -- not a near miss, no partial paint; that ruled out
# a 30s window without saying anything about SCREEN_CHECKS itself (see this
# file's own docstring for the disk swap that fixed the actual cause).
# Against root-si.qcow2, on the real container harness and the build-fast
# binary, under concurrent host load from several sibling background
# agents, the poll settled after 34.48s across 8 screendumps. 180.0 keeps
# roughly 5x headroom above that observation for further host contention,
# the same margin style BOOT_TIMEOUT_SECONDS's own comment uses.
SETTLE_INTERVAL_SECONDS = 2.0
SETTLE_TIMEOUT_SECONDS = 180.0

# The chooser panel's own gray fill, measured as the bounding box of its
# two dominant fill colors (see this file's own docstring): 98.9% of its
# pixels lit (channel > 16) against a 0.99 headroom-free ceiling,
# 231 distinct colors. PANEL_MIN_LIT_FRACTION=0.8 and PANEL_MIN_COLORS=20
# both keep generous headroom below those measurements while still
# failing hard on the old disk's own black-frame failure mode (lit
# fraction near 0) or a flat-fill regression (colors collapsing to ~1).
PANEL_RECT = (329, 201, 608, 465)
PANEL_MIN_LIT_CHANNEL = 16
PANEL_MIN_LIT_FRACTION = 0.8
PANEL_MIN_COLORS = 20

# The EZsetup account's icon: a real shaded 3D box bitmap, not a flat
# fill. Measured 230 distinct colors in this rect at the disk's first
# capture (see this file's own docstring). ICON_DARK_MAX_CHANNEL was
# originally 64, matching a 6.1% measured dark-pixel fraction at that
# capture -- generous headroom above the 0.02 floor. A DAC
# gamma-correction commit that later landed on trunk (same SI model
# code this test exercises) remapped raw pixel values to displayed
# brightness; rebasing past it and rerunning showed the same rect's
# channel <= 64 fraction had dropped to 0.021 -- real shading pixels
# spread wider across the brightness range by the new curve, not a
# rendering regression, but only 5% relative headroom above the floor,
# not "generous." A histogram of this same rect at that same capture
# (across max_channel 16 through 255) found a stable plateau around
# channel <= 128 measuring 5.9% -- close to the original pre-gamma
# 6.1% figure at the old threshold, i.e. roughly the same physical
# shading fraction, just relocated on the channel axis. 128 keeps that
# real ~3x headroom back; ICON_MIN_DARK_FRACTION stays at 0.02, the
# same floor, since a flattened-to-white icon still measures ~0.0 and
# a flattened-to-black one is still caught by ICON_MIN_COLORS below.
# LABEL_ROW_DARK_MAX_CHANNEL did not need the same fix: the same
# histogram exercise on label-row measured an identical 14.3% dark
# fraction at every channel from 16 through 208, so that check has no
# comparable fragility.
ICON_RECT = (459, 214, 98, 98)
ICON_DARK_MAX_CHANNEL = 128
ICON_MIN_DARK_FRACTION = 0.02
ICON_MIN_COLORS = 20

# A strip crossing the tail of root's label box through most of guest's,
# alternating panel gray, each label's own lighter fill, and black glyph
# text. Measured a 14.3% dark-pixel (channel <= 64) fraction and 4
# distinct colors in this rect (see this file's own docstring).
# LABEL_ROW_MIN_DARK_FRACTION=0.05 keeps generous headroom below that
# measured fraction; LABEL_ROW_MIN_COLORS=2 is the same floor the
# original single-icon capture used and would drop to 1 if every label's
# text vanished.
LABEL_ROW_RECT = (376, 319, 388, 18)
LABEL_ROW_DARK_MAX_CHANNEL = 64
LABEL_ROW_MIN_DARK_FRACTION = 0.05
LABEL_ROW_MIN_COLORS = 2

SCREEN_CHECKS = (
    NotAllBlackCheck(),
    LitPixelFractionCheck('chooser-panel', rect=PANEL_RECT,
                          min_channel=PANEL_MIN_LIT_CHANNEL,
                          min_fraction=PANEL_MIN_LIT_FRACTION),
    ColorVarietyCheck('chooser-panel', rect=PANEL_RECT,
                      min_colors=PANEL_MIN_COLORS),
    DarkPixelFractionCheck('account-icon', rect=ICON_RECT,
                           max_channel=ICON_DARK_MAX_CHANNEL,
                           min_fraction=ICON_MIN_DARK_FRACTION),
    ColorVarietyCheck('account-icon', rect=ICON_RECT,
                      min_colors=ICON_MIN_COLORS),
    DarkPixelFractionCheck('label-row', rect=LABEL_ROW_RECT,
                           max_channel=LABEL_ROW_DARK_MAX_CHANNEL,
                           min_fraction=LABEL_ROW_MIN_DARK_FRACTION),
    ColorVarietyCheck('label-row', rect=LABEL_ROW_RECT,
                      min_colors=LABEL_ROW_MIN_COLORS),
)


if QemuSystemTest is not None:

    class MgrasGreeter(QemuSystemTest):
        """Boots the installed IRIX disk on the Origin 2000's one-node
        module with SI/MGRAS graphics to the icon-chooser greeter and
        requires the 2D engine to have actually painted the account icon
        and its label."""

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_mgras_greeter(self):
            assets = require_assets(self, FIRMWARE_REL, DISK_REL)
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]

            vm = self.get_vm(name='mgras_greeter')
            vm.set_machine(MACHINE)
            vm.set_console()
            vm.add_args(
                '-accel', 'tcg,thread=multi',
                '-cpu', 'R10000', '-smp', SMP, '-m', str(MEMORY_MIB),
                '-bios', str(prom),
                '-audio', 'none',
                '-drive',
                f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-nic', 'user,id=net0',
                '-S')

            started = utc_now()
            vm.launch()
            vm.cmd('cont')
            log_path = self.log_file('mgras-greeter.log')
            try:
                transcript, reached = drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    DRIVER_STEPS,
                    until=LOGIN_PROMPT_RE, vm=vm)
                if not transcript:
                    self.fail(
                        'the guest printed nothing at all -- a run that '
                        'did not start, not a boot that failed early')
                if not reached:
                    self.fail(
                        'the serial console never printed a login prompt '
                        f'within {BOOT_TIMEOUT_SECONDS}s -- console log: '
                        f'{log_path}')

                screendump_path = self.scratch_file('mgras-greeter.ppm')
                try:
                    messages = poll_screendump_until_checks_pass(
                        self, vm, screendump_path, SCREEN_CHECKS,
                        interval_seconds=SETTLE_INTERVAL_SECONDS,
                        timeout_seconds=SETTLE_TIMEOUT_SECONDS)
                except ScreenCheckError as exc:
                    self.fail(
                        'the icon-chooser greeter never settled into its '
                        f'expected painted state: {exc} -- console log: '
                        f'{log_path}')
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = utc_now()

            run_path = self.log_file('run.json')
            with open(run_path, 'w', encoding='utf-8') as run_file:
                json.dump({
                    'schema': 1, 'started_utc': started, 'ended_utc': ended,
                    'reached_login_prompt': reached,
                    'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                    'screen_checks': messages,
                    'identity': run_record(FORK_ROOT, self.qemu_bin,
                                          prom=prom, disk=disk),
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
