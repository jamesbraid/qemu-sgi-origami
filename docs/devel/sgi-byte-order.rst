==========================
Byte order in the SGI fork
==========================

The Origin 200 is a big-endian machine with a little-endian I/O bus,
and byte order has produced more wrong fixes here than any other
subject: swap code compensating for the build host, qtest accessors
manufacturing the transform they were testing, and access-width
"requirements" that dissolve on inspection. This page is the working
agreement. What each chip does lives on its page under ``docs/specs/``;
this is how to write and test against it without adding a swap.

The model
=========

Byte order is a property of a bus boundary, not of code. The fork
models exactly one order-changing boundary: the Bridge, between the
big-endian crosstalk fabric and little-endian PCI. Everything else
states its native order and moves raw bytes.

* PCI endpoint devices (IOC3 and its ethernet, the M48T35 through the
  byte bus, the QLogic ISP, the Xframe) declare
  ``DEVICE_LITTLE_ENDIAN``.
* Hub-side and system devices declare what their specification states
  -- ``DEVICE_BIG_ENDIAN`` for the Hub's 64-bit registers.
* ``DEVICE_NATIVE_ENDIAN`` and any ``HOST_BIG_ENDIAN`` conditional are
  banned. The fork deleted every host-endianness compensation when the
  endpoints were normalized; a patch reintroducing one is compensating
  for a bug that lives elsewhere.

Writing device and machine code
===============================

Move raw bytes across a boundary that repacks. The Bridge's window and
lane-swap paths read into a byte buffer with ``address_space_read()``
and repack with ``ldn_be_p()``/``stn_be_p()``: the answering region's
declared endianness is resolved before the code sees bytes, so exactly
one swap happens, at the window, independent of the host. The pair of
endiannesses looks redundant and is load-bearing; replacing the buffer
with a value accessor such as ``address_space_ldl_be()`` reintroduces
the region's endianness as a live term. DMA behind the Bridge moves
raw bytes the same way (``dma_memory_read``/``write``).

Do not add ``impl`` widening to "fix" word order. The memory core's
split of a wide access composes through the region's declared
endianness order-preservingly: an eight-byte access through a
little-endian region served by four-byte handlers recomposes to the
same bytes as one eight-byte cycle. A transposition observed in
practice is a device placing its two 32-bit halves at the wrong
offsets, or a hand-rolled split choosing the wrong half order (the
Bridge's device-window doubleword did exactly that: for a big-endian
device the lower-offset word is the high half) -- never the core's
split. A split the core can do should be left to the core. (Widening is still sometimes required
for side effects -- a read-to-clear register must not see two half
reads -- which is a different reason and says so in its comment.)

``ldm_p()`` aborts on a one-byte big-endian access. Dispatch per width
(``ldub_p``, ``lduw_be_p``, ``ldl_be_p``, ``ldq_be_p``).

Writing qtests
==============

``qtest_readw/l/q`` and their write pairs apply the guest's byte order
end to end. Against a big-endian device that is the identity; against
a little-endian device the accessor performs the transform a test may
be trying to observe. For any assertion about byte placement, read
memory raw with ``qtest_memread()`` and decode by hand -- the value
accessors would manufacture the difference under test.

Choose patterns that convict. A pattern of eight or more bytes that is
not palindromic makes whole-buffer, per-word and word-pair reversals
all produce distinct results, so the assertion identifies the
transform instead of merely detecting one. Run unchanged-data
assertions over a sentinel fill, since unchanged is also what
no-transfer looks like.

Where the facts live
====================

Per-chip byte behaviour is specified on the device's page under
``docs/specs/`` -- the Bridge page's byte-lane and device-window
sections carry the swap-bit and width rules. A byte-order claim that
appears only in a
commit message or a review thread is not yet a fact anyone can build
on; put it on the spec page.
