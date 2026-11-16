.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-display-shm:

=====================================================
Shared-memory display for external viewers
=====================================================

An SGI machine can publish the picture of each of its graphic heads, and
take keyboard and mouse input, from another process, so that a viewer
outside QEMU can show the consoles of several machines in one window. The
machine property ``display-shm`` turns this on. Pixels live in shared
memory that the viewer maps and reads directly, with no copy by QEMU. A
control block in shared memory says what each head shows and which lines
changed. A local stream socket passes the memory handles and carries the
viewer's input.

This page is the contract a viewer implements against. It describes
software conventions between QEMU and a viewer, not hardware, so it carries
no ratings. ``include/ui/sgi-display-shm.h`` is its C form; the two change
together, and ``tests/qtest/sgi-display-shm-test.c`` checks the layout
against the offsets this page states. This is protocol version 3.

All integers are little endian. QEMU refuses ``display-shm`` on a big-endian
host.

Machines
========

Every SGI machine type takes the properties: ``octane``, ``octane2``, ``o2``,
``origin200``, ``origin2000``, ``onyx2``, ``origin300``, ``origin3000``,
``origin350``, ``onyx4``, ``onyx350``, ``tezro`` and ``fuel``.

A head is one display output of an SGI graphics model. The models that draw
through the shared scanout (the VPro, the IMPACT, the InfiniteReality and
the O2's GBE) publish one head for each console they create, in the order
the board creates them. A machine with none publishes zero heads and QEMU
warns. Heads are fixed when the machine starts, at most 8. The PsiTech RAD1
and RAD4 cards do not use the shared scanout yet and publish nothing until
their own family exists (see `Families`_).

Enabling it
===========

.. list-table::
   :header-rows: 1

   * - Property
     - Meaning
   * - ``display-shm=NAME``
     - Turn the display on. NAME is 1 to 30 characters from
       ``A-Z a-z 0-9 . _ -`` and names the socket: ``NAME.sock``, unless
       ``display-shm-socket`` says otherwise.
   * - ``display-shm-socket=PATH``
     - The socket's path. Default ``NAME.sock`` in ``$XDG_RUNTIME_DIR``
       when it is set and non-empty, else in the host's temporary
       directory as GLib names it (``$TMPDIR``, ``$TMP`` or ``$TEMP``, then
       the system default). macOS sets no ``XDG_RUNTIME_DIR``, so there it
       is the per-user ``$TMPDIR``, whose path can be long. A path must fit
       in a ``sockaddr_un``, about 104 bytes on macOS and 108 on Linux and
       Windows; QEMU refuses a longer one, and ``display-shm-socket`` then
       names a shorter one.
   * - ``display-shm-gpu=UUID``
     - 32 hex digits, a Vulkan ``deviceUUID``: export scanout memory from
       that adapter, so a viewer on the same adapter imports it without a
       copy (see `GPU memory`_). Absent, or when QEMU cannot export (see
       `Fallback`_), regions are plain shared memory. QEMU says at start-up
       which it is doing; a request it cannot meet warns and is not an error.

The socket is an ``AF_UNIX`` stream socket on Linux, macOS and Windows 10
and later. QEMU creates it after the machine is built and prints one line
naming it. It removes the socket when it exits. QEMU creates it for its
owner only. There is no named shared-memory object: every region reaches the
viewer as a passed handle.

One reader at a time. A second connection replaces the first.

Regions
=======

A region is one allocation with one handle, mapped at offset 0 by both
sides.

.. list-table::
   :header-rows: 1

   * - Backend
     - Meaning
   * - 1, ``VK_FD``
     - A Vulkan ``OPAQUE_FD`` (Linux):
       host-visible device memory QEMU allocated and maps. It is meant for a
       Vulkan import and cannot be mapped with ``mmap()``; see `GPU memory`_.
   * - 2, ``VK_WIN32``
     - A Vulkan ``OPAQUE_WIN32`` NT handle (Windows), the same memory.
   * - 3, ``SHM_FD``
     - A sealed memfd on Linux (``F_SEAL_SHRINK`` and ``F_SEAL_GROW``); a
       Linux kernel without memfd gets the macOS object below, unsealed. On
       macOS, a POSIX shared-memory object whose name QEMU unlinks as soon
       as it is created: the viewer never sees a name.
   * - 4, ``SHM_WIN32``
     - A section handle backed by the paging file (Windows).

Rules:

- Every region size is a multiple of 65,536, which covers 4 KiB Linux
  pages, 16 KiB Apple silicon pages and 64 KiB Windows allocation
  granularity. Plane offsets inside a region are multiples of 256, the
  largest ``min_storage_buffer_offset_alignment`` any wgpu backend reports.
  Strides are multiples of 4.
- Linux and macOS: the file descriptor travels with its message as
  ``SCM_RIGHTS``, one descriptor per message, on the first byte of that
  message. A viewer reads the message's 8-byte header with ``recvmsg`` to
  collect it. Windows: QEMU ``DuplicateHandle``\ s the section into the
  viewer's process, which it opens with ``PROCESS_DUP_HANDLE`` using the
  ``pid`` in ``HELLO``; the message carries the handle value valid in the
  viewer. The viewer closes it.
- A region is never resized; QEMU replaces it with a new id. Ids start at 1
  and are never reused in one run. The viewer's handle or mapping keeps
  the memory alive, so QEMU may free its side as soon as no head refers to
  the region. ``REGION_GONE`` tells the viewer to drop its mapping and
  handle. There is no release handshake.
- Memory is zero when created. Pixels and tables are read live, with no
  fence: that is how the hardware scans out.
- Region 0 is always the control block, backend 3 or 4, 65,536 bytes. It
  arrives with ``WELCOME`` and is never imported by a GPU.

The kinds are: 1, plane store; 2, tables; 3, tile pool (the O2's GBE,
defined by its own family); 4, cursor image. Kinds 1 to 3 are scanout memory
and the ones QEMU exports on a GPU. The cursor image is 16 KiB of pixels
that a viewer copies into a texture, so it is always shared memory.

Socket messages
===============

Every message starts with an 8-byte header and has a fixed total size per
type. A receiver that reads an unknown type closes the connection.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 0
     - u16
     - ``type``
   * - 2
     - u16
     - ``flags``: bit 0, ack requested (``INPUT`` only)
   * - 4
     - u32
     - ``seq``: set by the sender of an ``INPUT`` and echoed by the ack;
       zero otherwise

A session is: the viewer connects and sends ``HELLO``; QEMU answers with
``WELCOME`` carrying the control block's handle, then one ``REGION`` for
each live region; after that ``REGION`` and ``REGION_GONE`` follow region
changes, ``WAKE`` follows wake requests and ``INPUT_ACK`` follows input. A
new connection, including a reconnect, starts again from ``HELLO``, and QEMU
resends every live region with its original id.

Reader to QEMU
--------------

``HELLO``, type ``0x0001``, 48 bytes. It must be the first message and is
sent once. QEMU closes the connection on a second ``HELLO``, on any message
before it, and on a ``version`` it does not speak.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 8
     - u32
     - ``version``: 3
   * - 12
     - u32
     - ``caps``: bit 0, Vulkan import; bit 1, Metal no-copy; bit 2, wants
       wake-ups
   * - 16
     - u32
     - ``pid``: the viewer's process id
   * - 20
     - u32
     - reserved, 0
   * - 24
     - u8[16]
     - ``device_uuid`` of the viewer's adapter, zero for Metal
   * - 40
     - u64
     - reserved, 0

``INPUT``, type ``0x0002``, 32 bytes. See `Input`_.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 8
     - u32
     - ``kind``: 1 key, 2 button, 3 relative, 4 absolute
   * - 12
     - u32
     - ``head``: absolute only
   * - 16
     - u32
     - ``code``: key, a Linux evdev code; button, a QAPI ``InputButton``
   * - 20
     - u32
     - ``value``: 1 down, 0 up
   * - 24
     - i32
     - ``x``: relative delta, or absolute head pixel
   * - 28
     - i32
     - ``y``

QEMU to reader
--------------

``WELCOME``, type ``0x0081``, 64 bytes. Sent once, after ``HELLO``, with the
control block's handle.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 8
     - u32
     - ``version``: 3
   * - 12
     - u32
     - ``backend`` used for plane, table and tile regions: 1 or 2 when at
       least one live region is exported on the GPU, else 3 or 4. A region
       whose export failed is shared memory in a session that is otherwise
       GPU, so the ``backend`` of each ``REGION`` is the one to act on.
   * - 16
     - u32
     - ``nheads`` when it was sent; the control block's ``nheads`` is
       current
   * - 20
     - u32
     - ``control_backend``: 3 or 4
   * - 24
     - u8[16]
     - ``device_uuid`` QEMU exported on (the ``display-shm-gpu`` value),
       zero for backends 3 and 4
   * - 40
     - u8[16]
     - ``driver_uuid`` of that adapter, zero for backends 3 and 4. An import
       works only into the same driver.
   * - 56
     - u64
     - ``control_handle``: the Windows handle value, 0 on POSIX (the
       descriptor is attached)

``REGION``, type ``0x0082``, 56 bytes. One for each live region, after
``WELCOME``, and again whenever QEMU creates one.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 8
     - u32
     - ``id``: 1 or more, never reused within a run
   * - 12
     - u32
     - ``kind``: 1 plane store, 2 tables, 3 tile pool, 4 cursor image
   * - 16
     - u32
     - ``backend``
   * - 20
     - u32
     - ``memory_type``: Vulkan memory type index QEMU allocated from
       (backends 1 and 2), else 0
   * - 24
     - u64
     - ``size``: bytes the planes use, a multiple of 65,536
   * - 32
     - u64
     - ``alloc_size``: Vulkan ``allocationSize`` QEMU allocated (backends 1
       and 2), at least ``size``, else ``size``
   * - 40
     - u32
     - ``flags``: bit 0, dedicated allocation (backends 1 and 2): the import
       must be a dedicated allocation for a buffer. Bit 1 is reserved and 0.
   * - 44
     - u32
     - reserved, 0
   * - 48
     - u64
     - ``handle``: the Windows handle value, 0 on POSIX (attached)

``REGION_GONE``, type ``0x0083``, 16 bytes: ``u32 id`` at offset 8, then 4
reserved bytes.

``WAKE``, type ``0x0084``, 16 bytes: ``u32 head`` at offset 8, then 4
reserved bytes. Sent only if the head's ``armed`` word is 1; QEMU clears it
when sending. A viewer that draws every frame never arms a head.

``INPUT_ACK``, type ``0x0085``, 8 bytes: the header alone, with ``seq``.

There are no frame messages. The dirty bitmap and ``content_gen`` replace
them.

QEMU queues the messages it cannot send at once, up to 1,024. A viewer
that falls further behind than that is disconnected, and reconnects.

Control block
=============

Region 0, 65,536 bytes.

Header
------

Offset 0, 256 bytes:

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 0
     - u32
     - ``magic``: ``0x33535347`` ("GSS3" in memory), written last
   * - 4
     - u32
     - ``version``: 3
   * - 8
     - u32
     - ``nheads``: 0 to 8
   * - 12
     - u32
     - ``head_bytes``: 4096
   * - 16
     - u32
     - ``head_offset``: 4096
   * - 20
     - u32
     - ``qemu_pid``
   * - 24
     - 232 bytes
     - reserved, 0

Heads
-----

Head ``h`` is at ``4096 + 4096 * h``, 4,096 bytes. A head that does not
exist is all zero.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 0
     - u32
     - ``active``: 1 while the head shows anything
   * - 4
     - u32
     - ``family``: 1 xrgb, 2 odyssey, 3 mgras, 4 kona, 5 gbe, 6 rad
   * - 8
     - u32
     - ``family_version``
   * - 12
     - u32
     - ``layout_gen``: odd while QEMU rewrites the layout fields (below)
   * - 16
     - u32
     - ``width``
   * - 20
     - u32
     - ``height``
   * - 24
     - u32
     - ``refresh_mhz``: retrace rate in millihertz, 0 when unknown
   * - 28
     - u32
     - ``flags``: bit 0, dirty lines exact; bit 1, recompose every viewer
       frame while visible
   * - 32
     - u64
     - ``content_gen``: bumped after pixels, tables or layout change
   * - 40
     - u32
     - ``state_gen``: seqlock over the family state at 512; odd while
       writing
   * - 44
     - u32
     - ``armed``: the viewer writes 1 to ask for a ``WAKE``
   * - 48
     - u32
     - ``cursor_gen``: bumped when the cursor image, hot spot, position or
       visibility changes; odd only while the image is being written
   * - 52
     - u32
     - ``cursor_on``
   * - 56
     - i32
     - ``cursor_x``: the hot spot, in head pixels
   * - 60
     - i32
     - ``cursor_y``
   * - 64
     - i32
     - ``cursor_hot_x``
   * - 68
     - i32
     - ``cursor_hot_y``
   * - 72
     - u32
     - ``cursor_w``: 0 to 64
   * - 76
     - u32
     - ``cursor_h``
   * - 80
     - 8 x 32 bytes
     - ``plane[8]``
   * - 336
     - 4 x 16 bytes
     - ``table[4]``: ``{u32 region, u32 size, u64 offset}``
   * - 400
     - 16 bytes
     - ``cursor_image``: ``{u32 region, u32 reserved, u64 offset}``
   * - 416
     - 96 bytes
     - reserved, 0
   * - 512
     - 1,024 bytes
     - family state: small registers, layout per family
   * - 1536
     - 512 bytes
     - ``dirty[64]``: u64 words; bit ``y`` is display line ``y``, 4,096
       lines
   * - 2048
     - 2,048 bytes
     - reserved, 0

A plane descriptor is 32 bytes. ``region`` 0 marks an unused plane.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
   * - 0
     - u32
     - ``region``
   * - 4
     - u32
     - ``format``: 1 x8r8g8b8 little endian, 2 RGBA8 word (VPro), 3 u8, 4
       u16, 5 u32 word, 6 tile pool (GBE)
   * - 8
     - u64
     - ``offset`` in the region, a multiple of 256
   * - 16
     - u32
     - ``width``
   * - 20
     - u32
     - ``height``
   * - 24
     - u32
     - ``stride``: bytes
   * - 28
     - u32
     - ``flags``: bit 0, big-endian words; bit 1, row 0 is the bottom
       display line

The cursor image is 64 by 64 ARGB (``0xAARRGGBB`` as a little-endian word),
rows 256 bytes apart whatever ``cursor_w`` is, with the image in the top left
``cursor_w`` by ``cursor_h`` pixels. Display lines are numbered from the top
of the monitor.

Rules
-----

**What QEMU writes, and what the viewer writes.** The viewer writes only
``armed`` and ``dirty``. It clears ``dirty`` by atomically exchanging each
64-bit word with 0 (acquire). QEMU sets bits with an atomic OR (release),
then bumps ``content_gen``. Everything else in the block is QEMU's. A viewer
treats what it reads as untrusted.

**Layout seqlock.** ``layout_gen`` is odd while QEMU rewrites ``active``,
``family``, ``family_version``, ``width``, ``height``, ``refresh_mhz``,
``flags``, ``plane[]``, ``table[]`` and ``cursor_image``. The other fields
between offsets 16 and 511 (``content_gen``, ``state_gen``, ``armed`` and
the cursor fields) are not covered: they change under their own rules or
independently. A mode change or a new region rewrites the layout, bumps
``content_gen`` and sets the ``dirty`` bit of every line of the new mode (up
to line 4,095).

**Frame read order**, for a viewer that composes a head each frame:

1. Read ``layout_gen``; retry while it is odd.
2. Exchange ``dirty``.
3. Read the family state under ``state_gen``: copy the 1,024 bytes of
   registers to a uniform, and retry if ``state_gen`` changed.
4. Compose the family over the dirty lines.
5. Check ``layout_gen`` again, and redo the frame in full if it moved.

**Dirty lines.** Bit 0 of ``flags`` means every line that changed since the
viewer last cleared the bitmap has its bit set; a bit can also be set for a
line that did not change. When it is clear, which is so for a head taller
than 4,096 lines, any change in ``content_gen`` means a full frame.

For the xrgb family QEMU resolves each dirty line into a spare row first
and compares it with the frame. It stores the line and sets its bit only
when the pixels differ, so a line that the model re-resolved to the same
pixels stays clear and the pixels are in place by the time a bit is set. For the other
families QEMU sets them when the model's planes change.

**Waking.** A viewer that sleeps between frames writes 1 to ``armed``, then
re-checks ``content_gen``, and waits for ``WAKE``. QEMU clears ``armed``
when it sends the message. Wake-ups are best effort: a viewer that has not
armed gets none.

**Cursor.** ``cursor_x``, ``cursor_y`` and ``cursor_on`` change as the guest
moves its cursor. Every such change, and every change of the image, its size
or its hot spot, bumps ``cursor_gen`` and wakes an armed viewer. A move adds
2, so the generation stays even; QEMU makes it odd while it writes the
image. To read the image: read ``cursor_gen``, retry while it is odd, copy
the image, read it again and retry if it moved. A viewer that only follows
the pointer reads the position when ``cursor_gen`` changes. The host cursor
is the guest's, so a viewer that shows it never repaints for it.
A model that draws its cursor into its own pixels (the InfiniteReality, the
VPro, the IMPACT and the O2's GBE) keeps ``cursor_on`` 0, so a viewer shows
no second cursor, and still publishes ``cursor_x`` and ``cursor_y`` for
absolute input, without a generation: the cursor is in the pixels and a move
marks lines. These four publish when the guest writes the cursor's position
register. The VPro, the IMPACT and the GBE have no hot spot register, so the
position is the glyph's top-left pixel, and the pointer lands the hot spot's
offset in the glyph away from the target (*approximation*). The IMPACT
reports at its next retrace, because its two coordinates are separate
register writes.

**Order of channels.** The control block and the socket are separate
channels. A plane or the cursor image can name a region before the viewer
has read that region's ``REGION`` message. A viewer waits for the message,
and composes nothing from a region it has not mapped.

Families
========

Each family's ``family_version``, plane slots, tables and state block are
defined by the change that adds the family, with its model's ``resolve_row``
as the reference meaning. This version defines one.

Family 1, ``xrgb``, ``family_version`` 1:

- plane 0 is x8r8g8b8 little endian: the model's presented frame, which is
  ``0x00RRGGBB`` words, offset 0, ``stride`` ``width * 4``;
- no tables and no family state;
- it is how a head reaches the viewer before its model has its own family.
  The model still resolves its pixels on the CPU, QEMU's console and
  ``screendump`` see the same frame, and the cursor is the host cursor.

The other numbers are reserved for the models that will use them: 2, the
VPro (``sgi-odyssey``); 3, the IMPACT (``sgi-mgras``); 4, the
InfiniteReality (``sgi-kona``); 5, the O2's GBE; 6, the PsiTech RAD1 and
RAD4.

GPU memory
==========

With ``display-shm-gpu=UUID`` QEMU allocates each scanout region (kinds 1 to
3) as host-visible Vulkan device memory instead of a shared-memory object,
maps it for the models to write, and passes its handle in ``REGION``. A viewer
on the same adapter imports the handle into its own Vulkan device and reads
the pixels with no copy. This is the standard cross-process path, Vulkan
1.1's opaque external memory with ``VK_KHR_external_memory_fd`` or
``VK_KHR_external_memory_win32``, and needs nothing from a particular driver.

QEMU does not link Vulkan. It opens the loader (``libvulkan.so.1``,
``vulkan-1.dll``) when the machine is built and uses about fifteen
entry points; the environment variable ``QEMU_SGI_VULKAN_LOADER`` names a
different loader file. It submits no work: its one device exists to own
memory. Memory for a region is made like this:

- the adapter is the physical device whose ``deviceUUID`` is the property's,
  at Vulkan 1.1 or later, with the external-memory extension for the host's
  handle type, and able to export the memory of a storage buffer as that
  handle (checked once, with ``vkGetPhysicalDeviceExternalBufferProperties``);
- a buffer of the region's size, usage storage buffer and transfer source,
  created for export, fixes which memory types are allowed;
- the memory type is among them, host visible and coherent, because nothing
  flushes. Cached types are preferred, so QEMU's frequent CPU reads of video
  memory do not go through a write-combined window, and then types that are
  not device local, which on a discrete GPU is the PCIe window;
- the driver may require a dedicated allocation for such a buffer, and
  ``REGION.flags`` bit 0 says so;
- QEMU allocates with the export info, maps the whole allocation, zeroes it
  and takes the handle.

``REGION`` carries ``memory_type`` and ``alloc_size`` from that allocation
and the viewer imports with them. It creates a buffer for the same handle
type, allocates with the import info (dedicated, when flagged), binds it and
reads. Opaque handles are not meant for ``mmap()``, and on NVIDIA's driver
cannot be mapped that way; a viewer imports into Vulkan or takes the
fallback. The import works only into the same physical device and driver, so
a viewer compares ``WELCOME.device_uuid`` and ``driver_uuid`` with its
adapter's and, when they differ, imports nothing and shows an error rather
than a picture.

Only scanout memory is exported, never guest RAM. For the xrgb family that
is the frame: 5 MiB for a 1280 by 1024 head.

macOS has no handle to export and QEMU never uses these backends there; a
viewer uses Metal on a shared-memory region, which is page aligned and sized
to 64 KiB.

Fallback
========

A viewer always receives plain shared memory when:

- the machine has no ``display-shm-gpu``;
- QEMU cannot load the GPU API, or runs without the adapter (a container
  that does not pass the GPU in, say), or no adapter has the UUID;
- the adapter cannot export memory: no external-memory extension, or no
  host-visible coherent type that can be exported;
- an export or an import fails.

QEMU does not read ``HELLO.device_uuid``: it exports on the adapter the
property names whoever connects. A viewer on a different adapter cannot
import those regions and has no shared-memory copy to fall back to, so it
must not be paired with a ``display-shm-gpu`` for another adapter. The viewer
that starts QEMU passes the UUID of its own adapter.

The first three are decided when QEMU starts, which says so in one warning,
and every region is shared memory. An allocation that fails later, after
others have succeeded, makes that one region shared memory and leaves the
rest exported. ``REGION.backend`` is what each region is, and
``WELCOME.backend`` is 1 or 2 only when at least one is exported. With backend
3 or 4 a viewer maps each region and copies only the changed lines of the raw
planes into its GPU buffer, then composes as it would from imported memory.

Input
=====

``INPUT`` messages feed QEMU's input layer directly, much faster than QMP
``input-send-event``. Everything one read of the socket returns is applied
in order, then QEMU sends one input sync, so a batch of events from one host
frame is one guest report. When any message in the batch has bit 0 of
``flags`` set, QEMU sends one ``INPUT_ACK`` with the ``seq`` of the last such
message. Messages that arrive before the ``HELLO``, or an unknown ``kind``,
are errors; a code or button outside the ranges below is dropped and logged
once.

An SGI machine has one PS/2 keyboard and one PS/2 mouse, so keys and
buttons name no head and go to the machine's own handlers. When the viewer
disconnects, QEMU releases every key and button it still holds down.

.. list-table::
   :header-rows: 1

   * - ``kind``
     - Meaning
   * - 1, key
     - ``code`` is a Linux evdev key code in 1 to ``0x2ff``; ``value`` 1 is
       down, 0 is up. QEMU maps it to the guest's scan codes. Codes outside
       the range are dropped.
   * - 2, button
     - ``code`` is a QAPI ``InputButton`` below ``INPUT_BUTTON__MAX``;
       ``value`` 1 is down, 0 is up. Others are dropped.
   * - 3, relative
     - ``x`` and ``y`` are deltas in guest pointer counts; positive ``y``
       is down.
   * - 4, absolute
     - ``head`` and the position ``x``, ``y`` in that head's pixels. A head
       that does not exist is dropped.

Every SGI pointer is a relative PS/2 mouse, so QEMU turns an absolute
position into relative motion by closing a loop on the guest cursor:

- The target is the position in the head's pixels. The cursor is what
  the head publishes in ``cursor_x`` and ``cursor_y`` (the origin, before the
  guest shows one).
- QEMU sends one step at a time, as one packet of at most 127 counts on
  each axis, the most QEMU's PS/2 mouse puts in one packet. A larger move
  would leave as several packets, and the guest moves its cursor once for
  each, so a step would have several answers.
- The answer to a step is the guest's cursor leaving where it was when the
  step went out; an update that leaves it there is not one. The answer is
  complete when the cursor has then been still for 40 ms of guest time, so
  an answer that comes in parts or late is measured whole. IRIX takes up to
  30 ms to act on a packet. A step not answered within 250 ms of guest time
  ends the move.
- Each axis sizes its next step from its last: the remaining error
  divided by the pixels a count moved on that step (the gain, within 1/16
  to 16, starting at 1 for every target), rounded to the nearest count with
  halves toward zero. The gain depends on the size of the step, because IRIX
  accelerates: on IRIX 6.5.30 with the VPro, observed one packet at a time,
  up to 4 counts move that many pixels and n counts above that move
  2n - 4, on each axis apart. So a gain is never carried to another target,
  and a step that overshoots the target bars every step as large on that
  axis for the rest of the target. The steps therefore shrink as the cursor
  closes in, and the last ones are small enough that IRIX moves them one to
  one.
- An error that rounds to no counts takes a step of one count, unless the
  last step on that axis was one count, in which case the axis is as close
  as it gets. A move of nothing or against the step teaches no gain: the
  axis is at the edge of the screen, or the guest changed the shape of its
  cursor, which moves the corner the VPro reports. An axis that moves
  nothing on 2 steps in a row stops.
- The move ends when neither axis has a step left, after 32 steps, or 2
  seconds of guest time after the target was given, however the cursor
  keeps moving. A relative message ends a pending move.
- A new absolute target for the same head while a step is in flight moves
  the target and nothing else: no packet goes out until the guest has
  answered the step in flight, and the next one is sized for the newest
  target. Each axis keeps its gain, forgets which steps overshot the old
  target, and the budget of 32 steps and 2 seconds starts again, so a
  pointer that keeps moving is followed for as long as it moves.
- While targets keep arriving (the last within 50 ms of guest time), QEMU
  aims ahead of the pointer along each axis by 0.6 times the distance the
  pointer covers in the time a step takes to land (an average of recent
  steps, at most 150 ms, and the lead at most 200 pixels), with the
  pointer's speed a running average over its targets at least 4 ms apart
  (half the old speed, half the newest). A pointer that has stopped is
  aimed at exactly, as is an axis that moved nothing on its last step (the
  lead may point past the edge of the screen), and a move that has nothing
  left to send at the lead is tried again at the target. The factor is an
  *approximation*: on IRIX 6.5.30 with the VPro, with the pointer moving at
  600 to 3000 pixels a second, 0.5 to 0.7 cut the mean distance between the
  guest cursor and the pointer to about 60 to 70 percent of aiming at the
  target itself, and 0.85 overshot on turns.
- A button message that arrives while a move is pending waits until the move
  ends (or 150 ms of guest time, whichever is first) so the click lands where
  the pointer is, and is then sent as a packet of its own for each message, in
  order. A key message that arrives while a button waits waits behind it, so
  a modifier change cannot overtake the click. At most 16 wait; the 17th sends
  them first.
- When the guest cursor is on another head than the target, QEMU does not
  know how the guest arranged its screens, so there is no target to close on.
  It sends the difference from the last absolute position the viewer gave for
  that head as relative motion, and nothing for the first.

Versions
========

The ``version`` field is 3. A viewer checks ``magic`` and ``version`` and
refuses any other version, rather than guess. Any change to an offset, a
size or the meaning of a field raises the version. Version 2, a slot-copying
shared-memory segment with input over QMP only, never shipped.

Limits
======

- One reader. A second connection replaces the first.
- At most 8 heads. A head taller than 4,096 lines has no exact dirty
  bitmap.
- Heads are fixed when the machine starts. Models other than the four
  above that draw through the shared scanout do not publish yet.
- The cursor is the guest's image, at most 64 by 64.
- The PsiTech RAD1 and RAD4 draw through their own console, not the shared
  scanout, and publish no head, so no cursor.
- The GPU export is tested against NVIDIA's proprietary driver and
  Mesa's lavapipe on Linux. AMD, Intel and nouveau drivers implement the
  same interface, and Windows with any adapter, have not been run.
  Windows and macOS builds are checked by the project's release builds for
  compiling, not by the qtests below, which run on hosts with POSIX
  descriptors.
- No audio.

.. _sgi-display-shm-tests:

Tests
=====

``tests/qtest/sgi-display-shm-test.c`` starts an Octane2 with a graphics
head and ``display-shm`` and plays a viewer. All but one of its tests need no
GPU:

- the control block and the first messages at their raw offsets, and the
  frame region against ``screendump``;
- a ``REGION`` message in full, the passed handle's seals and that two
  mappings share their bytes;
- the dirty bitmap (a changed line is marked, an unchanged one and a line
  past the head's last are not), ``content_gen`` and ``WAKE`` after a pixel
  change;
- the VPro's, the IMPACT's and the GBE's cursor position register writes
  appearing as ``cursor_x`` and ``cursor_y`` with ``cursor_on`` and
  ``cursor_gen`` unchanged;
- a guest cursor move bumping ``cursor_gen`` by 2 and waking an armed
  viewer, through ``x-display-shm-cursor``, a test-only machine property
  that moves head 0's host cursor as a model would;
- a new mode: the replacement region, ``REGION_GONE`` and the layout seqlock;
- reconnecting, including after a mode change replaced a region, and the
  protocol errors that close the connection;
- the fallback for ``display-shm-gpu``, with a UUID no adapter has and with
  a Vulkan loader that does not exist: shared memory in ``WELCOME`` and every
  ``REGION``, the Vulkan fields zero;
- the GPU export, which runs only on a host where Vulkan has a device that
  can export host-visible memory, and is skipped otherwise (as the
  unavailable prerequisite ``VULKAN_EXPORT``). For each such
  device it starts a machine on it and checks ``WELCOME`` and ``REGION``,
  then runs this binary again as a second process (``--vk-helper``) that
  imports the frame region's handle into its own Vulkan device, maps it, and
  checks the frame against ``screendump`` before and after the VPro model
  draws a pixel. ``SGI_DISPLAY_VK_TEST_UUID`` limits it to one device;
- the three properties on every SGI machine type;
- keys, buttons and relative and absolute motion arriving at the
  Octane2's keyboard and mouse controller, in batches with their acks, a
  held key released when the viewer drops, and the closed loop answering
  cursor updates;
- the closed loop against a simulated guest, through
  ``x-display-shm-cursor``: one that accelerates as IRIX does, a steeper one
  (one pixel for one count, 4n - 3 for more), and the IRIX one answering
  each packet late and in parts. From each corner to the opposite one and
  to the middle, and between far targets, every target lands exactly, one
  packet a step; and a guest that stops its cursor short of the right and
  bottom edges, where the move ends at the edge;
- ``x-display-shm-test-pattern``, a test-only machine property that makes
  head 0 a 720 by 486 picture with a frame code that counts at 30 frames a
  second of virtual time: the next frame number after each period, the
  code's 16 lines marked and ``content_gen`` bumped, with no guest running;
- the socket's removal at exit.
