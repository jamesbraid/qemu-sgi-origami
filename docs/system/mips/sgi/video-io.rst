.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-video-io:

====================================
Video outputs as displays and files
====================================

A board's video output is its own QEMU console, and can also be recorded to
a file. Today the DMediaPro DM2 (:ref:`sgi-xt-digvid`) is the only board
that has one; its input can be fed from a file.

The console
===========

The DM2's output pipe adds one graphic console to the machine, named for
the board's device id (``-device sgi-xt-digvid,slot=widget-10,id=dm2``).
Its size is the active raster of the latest timing record the guest sent
(720 by 487 for 525-line video) and it shows the last frame the guest
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

   -device sgi-video-file-sink,id=rec,path=out.y4m
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

The frames are written by a thread. In realtime a full queue (8 frames)
drops the frame and counts it. With ``-icount``, or ``lockstep=on`` on the
sink, the board waits for room instead, for at most 10 seconds, after which it
logs the stuck writer and drops the frame. A write error is logged once and
every later frame is dropped and counted. Lockstep without ``-icount`` waits
on the host's writer, so the run is not deterministic; ``-icount`` makes
virtual time follow the instructions run. The sink's ``frames`` (sent),
``written`` and ``dropped`` properties are readable with ``qom-get``. Its
properties are ``path`` (required) and ``lockstep``. A raster the guest's
timing record sets is limited to 4096 by 2304.

The file source
===============

.. code-block:: shell

   -device sgi-video-file-source,id=clip,path=in.y4m,loop=on,timecode=01:00:00:00
   -device sgi-xt-digvid,slot=widget-10,id=dm2,in-source=clip

The file's raster and rate become the DM2 input signal, for the rasters the
DM2 page lists (a 486- or 487-line 525 file both count); a file is never
scaled, so a guest that asked for another standard sees the library's
mismatch. The properties are:

``path`` (required)
  a Y4M file with 4:2:2 samples (``C422`` or ``C422p10``) and plain
  ``FRAME`` markers (frame parameters are not supported), or a raw file
  with ``format=raw``, or a named pipe.
``format``, ``width``, ``height``, ``depth``, ``rate``, ``fields``
  for ``format=raw`` only: ``y4m`` (the default) or ``raw``; the raster up to
  4096 by 2304; ``depth`` 8 or 10 (default 10); ``rate`` as ``30000/1001``
  (the default); ``fields`` ``progressive`` (the default), ``tff`` or
  ``bff``. Raw frames are the same planar layout as Y4M.
``loop``
  ``on`` starts the file again at its end. Without it the signal is lost
  after the last frame, as a tape that runs out.
``timecode``
  the SMPTE timecode of the first frame, ``hh:mm:ss:ff``, or
  ``hh:mm:ss;ff`` with drop-frame numbering at 29.97 and 59.94 Hz (quote the
  ``;`` in a shell). The DM2 has no timecode path, so it is counted for
  ``timecode-now`` and not handed to the guest.
``lockstep``
  see below.

A thread reads four frames ahead. In realtime a source that falls behind
repeats its last frame and counts it (``repeated``); it never drops or
skips. With ``-icount``, or ``lockstep=on``, the board waits for the next
frame instead, for at most 10 seconds, then logs the starved source and
repeats. Lockstep without ``-icount`` waits on the host's reader, so the run
is not deterministic. A corrupt or truncated frame, or a read error, is
logged and ends the file: the signal is lost and nothing retries. ``frames``
counts the frames delivered, ``queued`` the frames read ahead and
``timecode-now`` is the timecode of the last frame delivered. A restored
snapshot seeks a regular file to the frame it had reached in one step; a
named pipe cannot seek, so only a position of zero restores.

The DM2 reads and writes its frames in the packing named by ``x-packing``
(``8``, ``10``, ``10in16l`` or ``10in16r``); see
:ref:`sgi-xt-digvid-payload` for why that is a property.
