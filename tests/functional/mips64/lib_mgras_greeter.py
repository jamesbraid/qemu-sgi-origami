# Structural checks for the IRIX 6.5.30 SI/MGRAS greeter
#
# SPDX-License-Identifier: BSD-3-Clause
"""Check the chooser's painted panel, account icon and label before login.

The desktop functional test applies these checks to its greeter screenshot.
They came from the SI-configured root-si.qcow2 capture on an Origin 2000
one-node module with two R10000 CPUs, after the Crossbow reset fix in
QEMU 2324ead1d3. The generic root.qcow2 disk has no SI graphics setup and
produced a black frame. The configured disk showed a 98.9%-lit, 231-colour
chooser panel, a 230-colour shaded EZsetup icon, and a label strip with
four colours and 14.3% dark pixels. A later DAC gamma change moved the
icon's fraction at channel <= 64 from 6.1% to 2.1%, so its threshold
uses channel 128. The earlier greeter-only test measured whole-test times
of 124, 126 and 142 seconds on this profile; the desktop test now checks
these same regions before its graphical login.

The greeter colours come from lib_mgras_regions. These extra checks catch
a flat fill that preserves a colour but loses bitmap or glyph detail.
"""

from lib_mgras_regions import GREETER_COLOR_CHECKS
from lib_screen import (ColorVarietyCheck, DarkPixelFractionCheck,
                        LitPixelFractionCheck, NotAllBlackCheck)


# The chooser panel's own gray fill, measured as the bounding box of its
# two dominant fill colors (see the capture measurements above): 98.9% of its
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
# capture (see the capture measurements above). ICON_DARK_MAX_CHANNEL was
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
# distinct colors in this rect (see the capture measurements above).
# LABEL_ROW_MIN_DARK_FRACTION=0.05 keeps generous headroom below that
# measured fraction; LABEL_ROW_MIN_COLORS=2 is the same floor the
# original single-icon capture used and would drop to 1 if every label's
# text vanished.
LABEL_ROW_RECT = (376, 319, 388, 18)
LABEL_ROW_DARK_MAX_CHANNEL = 64
LABEL_ROW_MIN_DARK_FRACTION = 0.05
LABEL_ROW_MIN_COLORS = 2

GREETER_SCREEN_CHECKS = (
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
) + GREETER_COLOR_CHECKS
