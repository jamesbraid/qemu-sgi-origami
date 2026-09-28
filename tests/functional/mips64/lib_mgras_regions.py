# Expected colours for named regions of IRIX 6.5's xdm greeter and
# default desktop, as SI/MGRAS graphics should display them
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
IRIX 6.5.30's stock xdm greeter and its default 4Dwm desktop draw the
same chrome at the same 1280x1024 positions on every boot, whatever
graphics board renders them. That makes a handful of flat, stable patches
of each screen (a panel fill, a button body, the granite backdrop, an
icon's label plate) a colour reference that does not depend on the cursor
or on where a session leaves its windows.

The boxes, expected colours and tolerances below are fixed in this test
tree, so its pass condition changes only with a QEMU commit. The boxes
use [x0, y0, x1, y1] with x1 and y1 exclusive; _box() turns each into
lib_screen's (x, y, w, h).

Solid Impact's DAC runs every displayed pixel through a gamma-1.7 encode
ramp, so a QEMU screendump of the SI head is a post-DAC image. The
references are not all on that side of the ramp:

  - The greeter's grey chrome was measured directly on emulated RAD4
    greeter captures and compares with no transform.
  - The greeter's icon mats are 24-bit image data measured on RAD4, which
    applies no DAC ramp, so their expected colours go up through the ramp
    before comparing (encode_gamma).
  - The desktop's granite backdrop, icon label plates and Toolchest menu
    body are 8-bit colour-index chrome whose expected colours are the
    IRIX colour scheme's own framebuffer values, agreeing with real IRIX
    6.5 desktop captures. After a graphical login Xsgi loads the DAC ramp,
    so the found colours come down through the inverse ramp before
    comparing (decode_gamma).
  - The Toolchest title bar's grey is RAD4's own unfocused-window reading
    and compares with no transform.

Left out: regions useful only for comparing two RAD4 captures; regions
meaningful only while a particular window is open or
focused at a fixed position; icon artwork, which has no single expected
colour; and the Console icon label, whose own record says a mismatch
there is inconclusive. This tree asserts only what a fresh boot and a
fresh login always show.
"""

from lib_screen import DominantColorCheck

# Solid Impact's DAC ramp, fitted against a captured DAC table load.
SI_DAC_GAMMA = 1.7


def _box(x0, y0, x1, y1):
    return (x0, y0, x1 - x0, y1 - y0)


GREY_217 = (217, 217, 217)
BLACK = (0, 0, 0)
BUTTON_GREY = (189, 189, 189)
FORM_GREY = (230, 230, 230)
MAT_TONES = (('dark', (85, 85, 85)), ('light', (255, 255, 255)),
             ('mid', (170, 170, 170)))

# The ten scored regions of the clogin icon-chooser greeter.
GREETER_COLOR_CHECKS = (
    DominantColorCheck('greeter-panel-fill', _box(400, 450, 900, 650),
                       (('panel', GREY_217),), tolerance=6),
    DominantColorCheck('greeter-icon-mat-root', _box(375, 299, 405, 309),
                       MAT_TONES, tolerance=12, encode_gamma=SI_DAC_GAMMA),
    DominantColorCheck('greeter-icon-mat-guest', _box(725, 299, 755, 309),
                       MAT_TONES, tolerance=12, encode_gamma=SI_DAC_GAMMA),
    DominantColorCheck('greeter-outer-form', _box(400, 730, 780, 795),
                       (('form', FORM_GREY),), tolerance=8),
    DominantColorCheck('greeter-name-field', _box(460, 695, 660, 722),
                       (('field', (211, 181, 181)),), tolerance=15),
    DominantColorCheck('greeter-hostname-text', _box(327, 805, 610, 833),
                       (('bg', FORM_GREY), ('ink', (179, 179, 179))),
                       tolerance=20),
    DominantColorCheck('greeter-login-button', _box(802, 810, 869, 834),
                       (('body', BUTTON_GREY), ('ink', BLACK)),
                       tolerance=15),
    DominantColorCheck('greeter-help-button', _box(883, 810, 950, 834),
                       (('body', BUTTON_GREY), ('ink', BLACK)),
                       tolerance=15),
    DominantColorCheck('greeter-scrollbar-track', _box(945, 220, 958, 650),
                       (('track', BUTTON_GREY),), tolerance=15),
    DominantColorCheck('greeter-label-root', _box(372, 320, 410, 333),
                       (('panel', GREY_217), ('ink', BLACK)), tolerance=12),
)

LABEL_COLORS = (('ink', (33, 0, 48)), ('plate', (193, 193, 193)))

# The default desktop after a fresh graphical login: backdrop, the three
# home-directory icon labels down the right edge, and the Toolchest.
DESKTOP_COLOR_CHECKS = (
    DominantColorCheck('granite-backdrop', _box(300, 300, 500, 450),
                       (('tone1', (85, 85, 85)), ('tone2', (56, 142, 142))),
                       tolerance=15, decode_gamma=SI_DAC_GAMMA),
    DominantColorCheck('iconlabel-guest', _box(1194, 53, 1228, 70),
                       LABEL_COLORS, tolerance=20,
                       decode_gamma=SI_DAC_GAMMA),
    DominantColorCheck('iconlabel-dumpster', _box(1186, 129, 1235, 146),
                       LABEL_COLORS, tolerance=20,
                       decode_gamma=SI_DAC_GAMMA),
    DominantColorCheck('iconlabel-welcome', _box(1154, 205, 1268, 223),
                       LABEL_COLORS, tolerance=20,
                       decode_gamma=SI_DAC_GAMMA),
    DominantColorCheck('toolchest-titlebar', _box(8, 8, 113, 24),
                       (('body', (170, 170, 170)), ('ink', BLACK)),
                       tolerance=20),
    DominantColorCheck('toolchest-menu-body', _box(8, 26, 113, 192),
                       (('body', (193, 193, 193)), ('ink', BLACK)),
                       tolerance=20, decode_gamma=SI_DAC_GAMMA),
)
