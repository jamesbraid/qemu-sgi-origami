.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-psitech-rad1:

===============================
PsiTech RAD1: PCI graphics card
===============================

Used in :ref:`sgi-ip27` (Origin 200). The PsiTech RAD1 is a third-party
PCI graphics card for the Origin 200, built on the TI/3Dlabs TVP4020
(Permedia 2) with 8 MB of SGRAM (:src:`psitech-rad1-ds-1099 p. 1`) and the
chip's own integrated RAMDAC (:src:`ti-tvp4020-programmers-reference p.
304`). It has no separate bus bridge or RAMDAC, unlike the
:doc:`psitech-rad4`. It is unrelated to SGI's RAD1 audio controller
(:doc:`rad1-audio`). QEMU models it as ``psitech-rad1`` in
``hw/display/psitech-rad1.c``.

.. seealso::

   Platform pages: :ref:`sgi-ip27`.

The model is a functional profile for the software that drives the card
in IRIX 6.5.30: the PsiTech RAD1 kernel driver and the IO6 PROM's graphics
textport. It implements the flat rectangle copy, point, rectangle and
trapezoid fills, palette and bypass DMA paths that software uses, not the
Permedia 2's 3D pipeline. No capture of a real card exists and the chip
revision is not known. Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-psitech-rad1-sources:

Sources
=======

- The *RAD1 Graphics Head* product sheet, publication RAD1-DS-1099,
  :src:`psitech-rad1-ds-1099`: the chip, memory and the three listed
  resolutions.
- The TI/3Dlabs *TVP4020 (Permedia 2) Programmer's Reference Manual*,
  :src:`ti-tvp4020-programmers-reference`: the Region Zero map and the
  rasterizer, texture and framebuffer registers. Page numbers are the
  PDF's own count, about nine above the printed numbers.
- The TI *TVP4020 Hardware Reference Manual*, Issue 6: the interrupt and
  bypass DMA registers. No public copy is known, so it is cited by title
  and page.
- Linux ``include/linux/pci_ids.h``, ``include/video/permedia2.h`` and
  ``drivers/video/fbdev/pm2fb.c`` at ``db2ddb871435``, as software
  expectations: the PCI identities, the FIFO-space, memory-control,
  video-timing and palette register offsets, the render primitive bits
  and the palette loading sequence.
- The PsiTech RAD1 kernel driver and textport library shipped with IRIX
  6.5.30, and the IO6 PROM 6.156 shipped with it, read as binaries, as
  software expectations.

.. _sgi-psitech-rad1-identity:

Identity and placement
======================

.. list-table::
   :header-rows: 1

   * - Field
     - Model
     - Rating
   * - Vendor, device
     - ``3d3d:0009``
     - *approximation*
   * - Subsystem vendor, device
     - ``12d1:0101``
     - *approximation*
   * - Revision
     - ``0x00``
     - *approximation*
   * - Class
     - ``0x0380``, other display controller
     - *approximation*
   * - Interrupt pin
     - INTA
     - *approximation*
   * - BAR0
     - 128 KiB memory, Region Zero registers
     - size *software expectation*
   * - BAR1
     - 8 MiB memory, framebuffer
     - *approximation*
   * - BAR2
     - 64 KiB memory, flash
     - *approximation*

The IRIX 6.5.30 RAD1 kernel driver registers for two PCI identities,
``104c:3d07`` and ``3d3d:0009``, and never for a PsiTech vendor ID, so
IRIX attaches it only to a card presenting one of those, *software
expectation*. The IO6 PROM's graphics console code recognises device
``0x0009`` and device ``0x3d07`` and checks no vendor ID, *software
expectation*. Linux names ``104c:3d07`` the TVP4020 and ``3d3d:0009`` the
Permedia 2V (``include/linux/pci_ids.h``), a different part. The product
sheet names the TVP4020, so it favours ``104c:3d07``. Which identity a
real RAD1 presents is *unknown*; the model presents ``3d3d:0009`` only
because both the driver and the PROM accept it. A configuration-space
dump of a real card would settle it.

The subsystem pair ``12d1:0101`` is a model choice, an *approximation*.
No retained source prints the board's subsystem identity, and no driver
path is known to read it. A configuration-space dump of a real card would
settle it.

The driver requests a 128 KiB resource for the register region, which
matches the manual's Region Zero map, *software expectation*. Which PCI
resources it uses for the framebuffer and the flash is *unknown*; BAR1 and
BAR2 are the model's choice.

The card is placed as an ordinary PCI device on an Origin 200 Bridge,
``-device psitech-rad1,bus=pci,addr=N``. It is not part of the graphics
catalogue that ``query-sgi-machines`` reports
(:ref:`sgi-sn-graphics-pipes`).

.. _sgi-psitech-rad1-registers:

Register map
============

BAR0 is the Permedia 2's Region Zero
(:src:`ti-tvp4020-programmers-reference p. 15, Table 3.1`),
*established*. Every driver access is an aligned 32-bit load or store,
*software expectation*, and the model accepts only that width; another
width fails as a decode error. Unlisted offsets in a block read as zero,
ignore writes and log ``LOG_UNIMP``. The GP FIFO, video streams, SVGA and
reserved blocks, ``0x2000``-``0x2fff`` and ``0x5000``-``0x7fff``, are
not implemented.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x0008``
     - IntEnable
     - ``0``
     - RW
     - Stored. Does not gate the interrupt.
   * - ``0x0010``
     - IntFlags
     - ``0``
     - RW, write one to clear
     - See :ref:`sgi-psitech-rad1-interrupts`.
   * - ``0x0018``
     - InFIFOSpace
     - ``0x00010000``
     - RO
     - Always reports ample space. Writes log ``LOG_GUEST_ERROR``.
   * - ``0x0040``
     - Attach check
     - ``0x00000001``
     - RO
     - See below.
   * - ``0x00a0``-``0x00d8``
     - Bypass DMA
     - ``0``
     - RW
     - See :ref:`sgi-psitech-rad1-bypass-dma`.
   * - ``0x1100``
     - Bypass write mask
     - ``0``
     - RW
     - Stored only.
   * - ``0x3000``-``0x3078``
     - Video timing
     - ``0``
     - RW
     - Fourteen registers, stored only.
   * - ``0x4000``
     - Palette write address
     - ``0``
     - RW
     - See :ref:`sgi-psitech-rad1-palette`.
   * - ``0x4008``
     - Palette data
     - ``0``
     - RW
     -
   * - ``0x4018``
     - Palette read address
     - ``0``
     - RW
     -
   * - ``0x4010``-``0x4050``
     - Other RAMDAC registers
     - ``0``
     - RW
     - ``0x4010``, ``0x4020``, ``0x4028``, ``0x4030``, ``0x4038`` and
       ``0x4050``, stored only.
   * - ``0x8000``-``0x9300``
     - GP registers
     - ``0``
     - RW
     - See :ref:`sgi-psitech-rad1-render`. Write mask resets to
       ``0xffffffff``. Render is write-only: reading it, or any GP
       offset the model does not store, logs ``LOG_UNIMP`` and returns
       zero.

The manual names ``0x0008`` IntEnable and ``0x0010`` IntFlags
(TVP4020 manual p. 34-35), *established*. Linux
names ``0x0018`` InFIFOSpace, ``0x1100`` the bypass write mask and
``0x3000``-``0x3078`` the screen base, stride and timing registers,
*software expectation*. The driver polls ``0x0018`` against thresholds up
to 100 before each batch of GP writes, *software expectation*; the model
runs every operation inside the triggering write, so it always reports
more space than that.

The kernel driver reads ``0x0040`` at attach and continues only if it is
nonzero, *software expectation*. Which register lives there is *unknown*;
the model returns 1, an *approximation*.

The video timing registers are stored but not applied: the display does
not change geometry, base or stride when they are written, and the video
enable bit does not blank it, an *approximation*.

.. _sgi-psitech-rad1-render:

Rendering
=========

The model runs each ``Render`` command (``0x8038``) synchronously and
dispatches on its primitive field, bits 7-6 (Linux
``include/video/permedia2.h``). The registers it uses:

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Use in the model
   * - ``0x8000``-``0x8030``
     - StartXDom, dXDom, StartXSub, dXSub, StartY, dY, Count
     - Trapezoid and point coordinates, signed 16.16; Count 12 bits.
   * - ``0x8038``
     - Render
     - Starts the operation.
   * - ``0x8088``
     - WaitForCompletion
     - Write-only; reads zero.
   * - ``0x80d0``
     - RectangleOrigin
     - X bits 11-0, Y bits 27-16, signed.
   * - ``0x80d8``
     - RectangleSize
     - Width bits 15-0, height bits 31-16.
   * - ``0x8388``, ``0x83a0``
     - SStart, TStart
     - Source origin of a copy, in pixels.
   * - ``0x8580``
     - TextureBaseAddress
     - Source address of a copy; bit 30 selects host memory.
   * - ``0x8828``
     - LogicalOpMode
     - Bit 5 selects FBWriteData as the fill colour.
   * - ``0x8830``
     - FBWriteData
     - Fill colour.
   * - ``0x8a80``
     - FBReadMode
     - Bit 10 selects read-back to host memory.
   * - ``0x8ac0``
     - FBHardwareWriteMask
     - Plane mask on every framebuffer write.
   * - ``0x8ac8``
     - FBBlockColor
     - Fill colour under FastFill.

The other GP registers the driver writes (texture address mode, the
texture steps, data format, read mode, texel LUT and colour modes, alpha
blend, framebuffer write mode, filter mode, sync, rasterizer mode and
four initialisation-only registers at ``0x8c08``, ``0x8d80``, ``0x8f00``
and ``0x9300``) are stored but have no effect.

The register offsets, the rectangle field packing and signedness, the
write mask, bit 30 of TextureBaseAddress and the 16.16 coordinate format
come from the manual (:src:`ti-tvp4020-programmers-reference p. 41, 70,
73, 209, 211, 241, 260, 275`), *established*. The model's use of them is
narrower:

- **Rectangle with TextureEnable (bit 13), or with FBReadMode bit 10
  set.** A flat copy of ``RectangleSize`` pixels. Forward, it reads from
  host memory over PCI when TextureBaseAddress bit 30 is set, or from the
  framebuffer otherwise, at ``SStart, TStart`` with a source stride equal to
  the width, and writes the rectangle at ``RectangleOrigin`` under the write
  mask. With FBReadMode bit 10 set it copies the framebuffer rectangle to
  the host address in TextureBaseAddress instead. The kernel driver's
  rectangle moves use this path, *software expectation*. That bit 10 means
  "read back to the host" is the model's reading, an *approximation*; the
  manual calls it read destination enable. The texture steps are ignored.
- **Rectangle without TextureEnable, and point.** A constant fill: from
  FBBlockColor when FastFill (bit 3) is set, from FBWriteData when
  LogicalOpMode bit 5 is set, otherwise nothing is drawn and
  ``LOG_UNIMP`` is logged. A point writes one pixel at
  ``StartXDom, StartY``. The IO6 PROM textport draws text one point per
  glyph pixel and boxes as rectangle fills, *software expectation*. Only
  rectangle fills towards increasing X and Y (bits 21 and 22) are drawn;
  others log ``LOG_UNIMP``. Points and trapezoids ignore those bits.
- **Trapezoid.** For each of Count scanlines, the span between the two X
  edges is filled with the constant colour and the edges and Y advance by
  their deltas. Which pixels a real chip includes at a fractional edge is
  an *approximation*.
- **Line.** Logs ``LOG_UNIMP``.

Pixels are 32-bit words shown as ``0x00RRGGBB`` with no palette or gamma
lookup, an *approximation*: the card's pixel format is not established.
Rows are addressed with mode 0's stride. A fill pixel outside mode 0's
width or the 8 MiB memory is dropped. A copy whose rectangle has a
negative origin or does not fit is refused with ``LOG_GUEST_ERROR``, and a
failed host access ends it with ``LOG_GUEST_ERROR``. Every ``Render``
except a line then sets IntFlags bits 0, 1 and 7, even when nothing was
drawn.

.. _sgi-psitech-rad1-palette:

Palette
=======

Linux clears the palette by writing the write address once and then
red, green and blue to the data register for each index in turn
(``drivers/video/fbdev/pm2fb.c``), *software expectation*: the index
advances after each three-byte entry. The model implements that, and the
same protocol for reading through the read address. The 256-entry
palette is stored and migrated but does not affect the displayed image.

.. _sgi-psitech-rad1-bypass-dma:

Bypass DMA
==========

The bypass DMA engine copies a rectangle from host memory into local
memory (TVP4020 manual p. 48-52), *established*.
The kernel driver posts a transfer through its six registers in order and
waits for IntFlags bit 7, *software expectation*.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Model
   * - ``0x00a0``
     - ByDMAAddress
     - PCI source address.
   * - ``0x00b8``
     - ByDMAStride
     - Source bytes per row.
   * - ``0x00c0``
     - ByDMAMemAddr
     - Destination in pixels, bits 23-0.
   * - ``0x00c8``
     - ByDMASize
     - Width bits 11-0, height bits 27-16.
   * - ``0x00d0``
     - ByDMAByteMask
     - Byte lanes written in the first column (bits 3-0) and last column
       (bits 19-16).
   * - ``0x00d8``
     - ByDMAControl
     - Stored; every write starts a transfer.

The transfer writes rows at mode 0's stride and sets IntFlags bit 7 when
it ends, including after a failed host read. A zero width or height does
nothing and raises no interrupt. A destination outside the framebuffer is
refused with ``LOG_GUEST_ERROR`` and raises no interrupt. The mode,
restart, format and patch fields of ByDMAControl are not applied, and the
byte mask is applied only at the edge columns. The driver uses only the
fully open mask ``0x000f000f``, so other values are an *approximation*.
ByDMAComplete at ``0x00e8`` is not implemented.

.. _sgi-psitech-rad1-flash:

Flash
=====

The kernel driver programs a parallel NOR flash byte by byte with the
standard two-cycle unlock: ``0xaa``, ``0x55``, then ``0xa0`` and the data
byte, then polls the byte until it reads back, *software expectation*.
The model's BAR2 is 64 KiB of byte-wide flash. An unlock at ``0x5555`` and
``0x2aaa`` arms one program, which ANDs the byte into the stored value, as
a NOR program can only clear bits. A write that breaks the sequence
returns it to idle. Reads return the stored byte.

The driver's own addresses are ``0x5556`` and ``0x2aa9``. A byte store
at offset *k* reaches PCI byte *k* XOR 3 (:ref:`sgi-bridge-pio`), which
turns them into ``0x5555`` and ``0x2aaa``,
*inferred*. The flash's size, part and PCI resource are *unknown*, and
the model fills it with ``0xff`` at every reset rather than keeping it,
an *approximation*. Erase is not implemented.

.. _sgi-psitech-rad1-display:

Display
=======

The product sheet lists 1280 x 1024, 1024 x 768 and 1024 x 1024, and the
driver holds three mode records, *software expectation*. How the driver
picks one at start is *unknown*. The model always displays mode 0,
1280 x 1024, from the start of the framebuffer, and logs ``LOG_UNIMP`` at
each reset to say so. The whole frame is converted at every display
refresh.

.. _sgi-psitech-rad1-interrupts:

Interrupts
==========

The kernel driver's interrupt handler reads IntFlags, writes the value
back to clear it, clears IntEnable, and wakes a different waiter for each
of bits 0, 1 and 7, *software expectation*. The manual names bit 7 the
bypass DMA interrupt flag
(TVP4020 manual p. 35), *established*. Which
completions set bits 0 and 1 is *unknown*.
The model asserts INTA while any IntFlags bit is set. A write to IntFlags
clears the bits written as one. IntEnable does not gate the interrupt, an
*approximation*: no driver path is known to depend on it. Every
``Render`` other than a line sets bits 0, 1 and 7 together and bypass DMA
sets bit 7, so bit 7 does not tell the two apart as it would on the chip.

.. _sgi-psitech-rad1-reset:

Reset and migration
===================

Reset clears every register, the palette and the framebuffer, sets the
write mask to all ones, erases the flash to ``0xff`` and lowers the
interrupt. The model's reset values are its own; real cold, warm and PCI
reset are *unknown*. The driver's initialisation writes zero to most GP
registers, which is software setup, not evidence of a reset value.

``psitech-rad1`` migrates its PCI state, the framebuffer, every register,
the palette and its indices, the flash contents and the flash sequence
state. Migration works only between identical builds.

.. _sgi-psitech-rad1-limits:

Model limits
============

- The PCI identity, subsystem, revision, class and the framebuffer and
  flash BAR numbers and sizes are model choices.
- Only mode 0 is displayed. The video timing registers, the palette and
  the video enable bit do not affect the image. The pixel format is
  assumed to be 32-bit true colour.
- No 3D rendering, blending, logical operations, texture filtering or
  scaling, stipple, Color DDA colours, lines or decreasing-direction
  fills.
- Every operation completes inside the write that starts it.
- IntFlags bits are not attributed to their real sources and IntEnable
  does not gate the interrupt.
- The flash does not keep its contents across reset and has no erase.
- Unlisted register offsets log ``LOG_UNIMP``; refused copies and bypass
  DMA transfers and failed host accesses log ``LOG_GUEST_ERROR``; accesses
  of other widths to Region Zero or the framebuffer, and wider accesses
  to the flash, fail as decode errors.

.. _sgi-psitech-rad1-tests:

Tests
=====

``tests/qtest/psitech-rad1-test.c`` covers the PCI identity, subsystem
and BAR sizes; the control and status registers and a safe unsupported
offset; the palette round trip; framebuffer copies from local and host
memory, under the write mask, and back to the host; a negative origin in
the copy and fill paths; a point, a rectangle fill, FastFill colour and a
fill with no colour source; a trapezoid; the bypass DMA registers, a
transfer and its edge byte masks; IntFlags write-to-clear; flash
programming with and without the unlock; and reset.
