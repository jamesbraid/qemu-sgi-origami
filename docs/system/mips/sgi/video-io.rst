.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-video-io:

====================================
Video outputs as displays and files
====================================

A board's video output is its own QEMU console, and can also be recorded to
a file. Today the DMediaPro DM2 (:ref:`sgi-xt-digvid`) is the only board
that has one; its input side is not wired yet.

The console
===========

The DM2's output pipe adds one graphic console to the machine, named for
the board's device id (``-device sgi-xt-digvid,slot=widget-10,id=dm2``).
Its size is the active raster of the latest timing record the guest sent
(720 by 486 for 525-line video) and it shows the last frame the guest
played out, an interlaced frame as its two fields woven into one picture.
Nothing is deinterlaced or scaled. It appears wherever QEMU shows consoles:

- GTK: another tab of the window.
- VNC: ``-vnc :1,display=dm2,head=0``.
- SPICE: as another display of the machine.
- ``screendump`` over QMP: ``device=dm2``, ``head=0``.

The console is empty until a timing record arrives and a frame completes.
The picture is converted from limited-range YCbCr with the BT.601 matrix up
to 720 samples wide and BT.709 above, chroma repeated across each pair of
pixels. Window capture of a console, such as OBS does, is sampled at the
capture tool's rate and is for monitoring; the file sink is exact.

The file sink
=============

.. code-block:: shell

   -device sgi-video-file-sink,id=rec,path=out.y4m,audio=out.wav
   -device sgi-xt-digvid,slot=widget-10,id=dm2,out-sink=rec

``path`` is a Y4M file: one ``FRAME`` per frame the board played out, the
header giving the raster, the frame rate as a ratio, ``It`` for an
interlaced frame (the first stored line is the earlier field's, which
the DM2 page rates an approximation) and ``C422p10`` (10-bit samples in
16-bit little-endian words) or ``C422`` for 8-bit packings. An interlaced
frame is written woven, as the guest stored it. FFmpeg and most video tools
read the file; compression is theirs to do, as QEMU has no codec. A frame
whose raster differs from the header's is dropped and logged, since Y4M
has one raster.

``audio`` is optional: a 48 kHz 16-bit stereo WAV of program audio from a
board that has some. The DM2 model has none yet, so the file holds only its
header.

The frames are written by a thread. In realtime a full queue (8 frames)
drops the frame and counts it; with ``-icount`` the board waits for room, so
a run writes every frame. The sink's ``frames`` (sent), ``written`` and
``dropped`` properties are readable with ``qom-get``.

The DM2 reads its frames in the packing named by ``x-packing`` (``8``,
``10``, ``10in16l`` or ``10in16r``); see
:ref:`sgi-xt-digvid-payload` for why that is a property.
