.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-mesa-renderer:

Shared SGI Mesa renderer
========================

The shared renderer accepts decoded SGI drawing work and returns pixels to a
family-owned resource provider. It does not decode device commands, translate
guest addresses, own native planes, or prove hardware behaviour. No family
device calls the checked Mesa draw path yet.

Current drawing support
-----------------------

``sgi_render_draw_checked()`` in ``hw/display/sgi/renderer.c`` accepts one
flat- or smooth-shaded triangle and one front RGB or RGBA colour resource. The
resource must have bottom-origin rows and ordered UNORM components of 1 to 16
bits. The viewport must cover the resource.

A bound drawable with no clip rectangles owns no visible pixels. Its ``empty``
flag does not change this draw behavior. Up to 16 inclusive plane rectangles
may overlap or extend outside the resource. The renderer intersects them with
the resource and draws once through their union, so overlap does not apply
blending twice. If the union is empty, it returns without a native snapshot or
commit.

An enabled scissor box intersects that union in window coordinates. A box
with zero width or height leaves no pixels to draw. For a visible drawable,
negative dimensions return a resource error. Scissor and clip rectangles are
resolved before the native snapshot.

Blending is limited to an RGB8 resource without stored alpha, ``FUNC_ADD``,
and these source/destination factor pairs: ``SRC_ALPHA`` with
``ONE_MINUS_SRC_ALPHA``, ``DST_ALPHA`` with ``ZERO``, ``ZERO`` with
``ONE_MINUS_DST_ALPHA``, and ``SRC_ALPHA_SATURATE`` with ``ONE``. The worker
imports missing native destination alpha as one. Other factor pairs,
equations, and colour layouts remain unsupported for blended draws.

A textured draw requires ``GL_TEXTURE_2D``, a front RGB8 or RGBA8 colour
resource, and one ``SGI_RENDER_TEXTURE`` resource matching a level-zero
``SGIGLTexImage``. The image has no stored border, positive power-of-two
width and height, and RGBA base format. The texture resource has the same
dimensions, bottom-origin rows, and four ordered 8-bit UNORM components.
Colour and texture snapshots both finish before Mesa draws. Texture slot
staging above 16 MiB returns ``SGI_RENDER_UNSUPPORTED``.

Equal ``NEAREST`` or equal ``LINEAR`` minification and magnification filters
are supported. S and T independently accept ``REPEAT``, ``CLAMP``,
``CLAMP_TO_EDGE``, and ``CLAMP_TO_BORDER``. The worker filters raw integer
RGBA8 texels before ``REPLACE``, ``MODULATE``, or ``DECAL``. For ``LINEAR``,
it combines four taps with double-precision weights. The finite border
colour is clamped to [0,1] and supplies out-of-range taps. It is separate
from stored image-border texels. Mixed or mipmap filters, stored borders,
other texture formats, and other environment modes remain unsupported.

The path also rejects colour-index drawing, depth and stencil tests, client
clip planes, ordered dithering, accumulation, and other draw states it has
not mapped to Mesa. It snapshots the full colour resource and draws through
an RGBA16 host buffer. Private stencil marks the clip union and pixels
written by the same draw. It reads back colour and coverage before any
native commit. The shared code passes the bottom-origin binary write mask
to ``commit_covered``. The provider must leave zero-mask pixels untouched.
Equal-colour writes remain marked. A missing sparse callback returns
``SGI_RENDER_UNSUPPORTED`` before the snapshot. No family provider uses this
path yet, and KONA's tag meaning remains a family contract.

Decoded CLEAR
-------------

``sgi_render_raster_checked()`` accepts decoded colour, depth, and stencil
CLEAR. Colour resources use three or four ordered UNORM components of 1 to 16
bits. Depth and stencil use separate unsigned, single-component resources at
exactly 24 and 8 bits. Colour-index CLEAR and other raster operations remain
unsupported. The clear masks and inclusive clip rectangles determine which
native pixels may be written.

The path validates selected descriptors and snapshots every resource with
nonempty clipped coverage before Mesa work. It finishes host CLEAR and
readback before committing native colour, depth, then stencil. On a successful
commit, the union of clipped pixels is written once per resource. A union
covering the full plane uses one dense commit. A later native commit failure
retains earlier writes, possibly including a partial pixel. A snapshot or
host failure commits nothing. The family still owns dirty and alias updates
and guest-visible completion.

The shared type declarations cover more roles and numeric formats than the
current Mesa draw and CLEAR paths support.

Renderer lifetime and ownership
-------------------------------

``sgi_renderer_create(&renderer, errp)`` returns ``SGI_RENDER_CREATE_OK``
with a renderer. It returns ``SGI_RENDER_CREATE_UNAVAILABLE``,
``SGI_RENDER_CREATE_OOM``, or ``SGI_RENDER_CREATE_HOST_ERROR`` with no
renderer and an error. Only ``SGI_RENDER_CREATE_UNAVAILABLE`` permits an
optional caller to choose another renderer. The legacy
``sgi_renderer_new()`` wrapper returns ``NULL`` for every failure and loses
that distinction.

A created renderer has a surfaceless EGL context and one worker thread.
``sgi_renderer_free()`` joins the worker and releases its host resources.
Submissions are synchronous. The worker uses host arrays and immutable draw
data for each call. It does not access guest memory, call family resource
callbacks, or acquire QEMU's big lock. A host draw failure latches the
renderer's failed state, so later draw requests fail.

A family adopting this API must own the renderer for the head's lifetime and
fail device realization if its required renderer cannot be created. The family
still owns command decoding, ``SGIGLContext``, target selection, native
storage, dirty tracking, migration, and guest-visible completion. It must
resolve those inputs before calling the shared code. Reset and migration
must drain synchronous submissions and rebuild host GL objects from guest
state. Host handles are not migration state.

Typed resources and commit order
--------------------------------

``SGIRasterTarget`` lends a ``SGIRenderResource`` array and
``SGIRenderResourceOps`` callbacks for one call. A descriptor states the role,
colour target, front/back/auxiliary buffer, eye, dimensions, row origin, and
component channels, widths, and numeric interpretation. The backing ID and
generation name native storage. The view ID and generation name one typed
interpretation of that storage. The shared validator checks descriptor shape,
rectangle bounds, scalar-slot stride and capacity, and component values. It
does not infer aliases, advance generations, or mark scanout dirty. The family
provider must update those after every native write, including a partial
commit, and invalidate other views of the same backing.

Snapshots and commits use inclusive rectangles. Each declared component has
one zero-extended ``uint32_t`` slot. Components are interleaved per pixel,
and stride counts slots rather than bytes. The provider handles native
packing, endianness, and guest-address translation. A successful snapshot
fills every requested slot. A successful commit reports the rectangle's full
pixel count. A failed commit reports its complete pixel prefix, possible
partial next pixel, and whether the extent is known. Native writes already
made by the provider remain made. There is no rollback.

For the checked triangle, the family provider must implement
``commit_covered``. A dense ``commit`` callback alone is insufficient: the
draw returns ``SGI_RENDER_UNSUPPORTED`` before native snapshot. The covered
callback receives one byte of write coverage per pixel. The shared wrapper
validates mask capacity and binary values before calling the provider, then
checks its partial result.
``complete_pixels`` counts the scanned row-major prefix, including skipped
pixels. The family provider must preserve mask-zero native storage, apply its
reviewed colour and coupled-plane rules to mask-one pixels, and update dirty
and alias state after partial writes.

For a future draw with several attachments, resolve and snapshot every
required resource under the big lock before any native commit. Complete all
host rendering and readback before committing attachments in order. A
snapshot or host failure commits nothing. A later native commit failure
retains earlier commits and returns an error. The provider must dirty the
known changed region or the whole resource when the extent is unknown.
Publish a guest fence, reply, or interrupt only after every required commit
succeeds.

``SGIRenderResult`` distinguishes ``SGI_RENDER_OK``, ``SGI_RENDER_OOM``,
``SGI_RENDER_UNSUPPORTED``, ``SGI_RENDER_RESOURCE_ERROR``, and
``SGI_RENDER_HOST_ERROR``. Only a real allocation failure can become guest
GL out-of-memory. Unsupported mandatory work blocks family adoption. An
unexpected failure of already validated work must stop the VM visibly under
the big lock, with its host diagnostic, rather than report guest success or
retry through the software rasterizer.

Build and test
--------------

From an Origami worktree, select the QEMU source worktree explicitly. These
wrappers run the build and tests in the project's rootless Podman environment:

.. code-block:: sh

   cd /absolute/path/to/origami-worktree
   tools/build-env/qemu-build --src /absolute/path/to/qemu-worktree \
       --build build -j4
   UV_CACHE_DIR=/storage/nvme/origami/uv QEMU_TEST_SGI_REQUIRE_MESA=1 \
       tools/qtest --src /absolute/path/to/qemu-worktree --suite unit \
       test-sgi-renderer-gl test-sgi-render-resource

On this host, ``/storage/nvme/origami/uv`` is the configured uv cache.
``tools/qtest`` starts through a uv shebang before it can read Origami's
scratch configuration, so set ``UV_CACHE_DIR`` on the command line. Use the
configured cache path on another host. The ``build`` flavor is the one used
for tests. ``-j4`` limits compilation to four workers.

The renderer test selects software llvmpipe and checks the reported GL
renderer. Origami's ``qtest`` wrapper forwards ``QEMU_TEST_SGI_REQUIRE_MESA``
into the test container, so missing surfaceless Mesa fails instead of skipping.
The optional render-node case needs ``QEMU_TEST_SGI_RENDER_NODE`` and device
access inside that container. Setting the variable only on the host does not
configure either. The required SGI unit selection is documented in
:doc:`testing/sgi`. The renderer test exercises Linux surfaceless EGL.
Neither named test exercises a GBM render node or qualifies macOS or Windows
context creation.

``qemu-build`` compiles ``qemu-system-mips64`` from the selected QEMU worktree.
``qtest --suite unit`` rebuilds the named unit binaries but does not rebuild
that emulator. The wrapper retains source revision, local changes, binary
hashes, and test results under the configured scratch root. Check that
evidence before claiming a pass. Another worktree's build or an older binary
does not verify the edited source.

Evidence boundary
-----------------

Synthetic tests establish the shared code's behaviour, not physical SGI
hardware behaviour. Hardware-visible claims need the family's reviewed
contract, public evidence, and separate device register and guest acceptance
tests. Do not copy SGI or IRIX source text or identifiers into this tree.
Someone exposed to restricted source for a device cannot implement or
independently review that device's emulator code. Record hardware facts in
the owning reviewed datasheet with citations to permitted sources.
