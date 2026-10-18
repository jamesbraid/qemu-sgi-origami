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

``sgi_render_draw_checked()`` in ``hw/display/sgi/renderer.c`` accepts points,
lines and every polygon mode: separate triangles, triangle strips and fans,
quadrilaterals, quad strips and polygons, flat or smooth shaded, into one front
RGB or RGBA colour resource. The resource has ordered UNORM components of 1 to
16 bits. Its rows may start at either edge. A textured draw is still one
separate triangle.

The checked draw requires the raster rules every family uses and the scalar
comparisons establish: 8 subpixel bits, ``SGI_ROUND_HALF_AWAY``,
``SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN`` and ``SGI_ARITH_DOUBLE``. Other rules
return ``SGI_RENDER_UNSUPPORTED`` before the native snapshot.

Storage widths must also be ones whose stores are proven equal to the
scalar path's, or the draw returns ``SGI_RENDER_UNSUPPORTED`` before the
snapshot:

- The colour components must have the rules' colour bits.
- An RGBA resource's stored alpha must have the rules' alpha bits, and those
  must equal the scalar path's incoming alpha precision, the larger of two,
  the colour bits and the alpha bits. The scalar path rounds alpha to that
  precision and then truncates it to the store. For example, with 10-bit
  colour and 2-bit alpha, alpha 0.5 becomes 512 of 1023 and is stored as 1 of
  3, where rounding straight to the store gives 2.
- An RGBA resource under rules without alpha is refused too. There the scalar
  path hands the store canonical zero alpha, which the family may or may not
  store.
- An RGB resource is accepted under any alpha bits, because the scalar
  path's colour rounding does not depend on them.

The renderer splits each draw into independent points or triangles exactly as
the scalar rasterizer does. It keeps the scalar path's order, swaps the first
two vertices of odd strip triangles, draws quadrilaterals and polygons as fans
from their first vertex, and uses the same provoking vertex for flat shading.
A constant colour, that of a flat-shaded piece or a point, is rounded to the
native steps on the CPU in double, as the scalar path rounds it, so that a
product exactly on a half step in float32, such as 0.9 times 255, is not
rounded the other way by the worker.

A smooth-shaded or textured draw goes through the scalar path's own
triangles:

- The CPU clips each triangle, splits it into the scalar path's fan and
  projects and snaps it with the scalar functions
  (``sgi_raster_triangle_pieces()``).
- The worker draws each resulting triangle at those snapped window positions,
  through the fixed viewport (-4096, -4096, 16384, 16384). Its power-of-two
  scale keeps the vertices exact, so GL covers the scalar path's pixels. A
  vertex outside that viewport returns ``SGI_RENDER_UNSUPPORTED`` before the
  snapshot.
- The fragment program recomputes the scalar path's edge functions,
  barycentric weights and 1 / w correction in double, in its order of
  operations, before rounding to the native steps. A textured draw's program
  also interpolates the texture coordinates that way and samples as the
  scalar path does; see below.
- Fragments are bounded by the viewport, as the scalar path bounds them.

This is exact only where llvmpipe evaluates the program's doubles as IEEE
binary64 and the compiler has not fused the scalar path's own multiplies
and adds, as on the pinned llvmpipe build on x86-64 hosts that the unit
tests prove. GLSL does not require correctly rounded double division, and
compilers for other architectures may contract the scalar arithmetic.
Before a renderer draws its first smooth triangle, it draws
perspective-weighted smooth triangles, two of them sharing an edge through
pixel centres and one with an edge in the near plane, through both paths.
If coverage or any covered pixel's colour differs, smooth triangles stay
unsupported on that renderer. A host failure during the probe is returned
as that failure. Flat triangles near the near or far plane use the same
program, so where the program is missing or this probe fails they are
refused too.

A triangle frame is drawn upside down,
because llvmpipe applies its top-left rule with rows running down. Drawing it
upside down gives a pixel centre on a horizontal edge to the same triangle as
the scalar path's top-left rule in OpenGL's upward rows. Point and line
frames keep upward rows, where they already match.
The scalar path drops a fragment whose depth, interpolated in double from
a triangle's first vertex, rounds outside [0, 1]. On an edge lying in the
near plane that sum rounds a few ulps below 0 at some pixel centres, and the
scalar path drops them. The smooth-triangle program repeats the scalar
path's depth sum in double from the same clipped vertices and discards the
same fragments. A flat triangle whose depths differ and, after clipping,
come within 2^-32 of either plane is drawn by that program too, with its
provoking colour, so it drops the same pixels. Elsewhere the sum cannot
leave [0, 1]. The texture program repeats the drop too.

A point's size is rounded as the scalar path rounds it: to the nearest integer
and at least one, with zero meaning one. The worker reads
``GL_ALIASED_POINT_SIZE_RANGE`` when it creates its context. A size outside
that range returns ``SGI_RENDER_UNSUPPORTED`` before the native snapshot.

Aliased lines, line strips and line loops draw exactly as the scalar path
draws them, including width, stipple and smooth colour. Native GL lines
cannot match, so each segment is drawn as a two-triangle parallelogram:

- The CPU clips, projects and snaps the segment with the scalar rasterizer's
  own functions (``sgi_raster_line_span()``). With ``sgi_soft_line_covers()``
  it finds the range of major-axis steps that hold base pixels, and it refuses
  the draw if any step holds more than one.
- The parallelogram covers the band of half the width either side of the
  segment, where llvmpipe's triangle rules select exactly the scalar path's
  replicated rows. Its ends extend past that range by whole multiples of the
  segment's smallest step on the subpixel grid, so every vertex stays on the
  line.
- The worker maps window pixels through the fixed viewport (-4096, -4096,
  16384, 16384), whose power-of-two scale keeps the vertices exact. A vertex
  outside it returns ``SGI_RENDER_UNSUPPORTED`` before the snapshot.
- The fragment program keeps only that range. It applies the stipple bit of
  its base pixel. It computes colour in double from the pixel centre's
  clamped parameter along the segment, weighted by 1 / clip w, as the scalar
  path does.
- The CPU carries the stipple counter along strips and loops and through a
  borrowed phase across draws. It counts fragments outside the drawable too,
  as the scalar path does.
- Fragments are bounded by the viewport, as the scalar path bounds them.

Before a renderer draws its first line, it draws tie vectors through both
paths, with smooth colour and perspective weights. If coverage or any
covered pixel's colour differs, lines stay unsupported on that renderer.
Blended draws of more than one segment remain unsupported.

Smoothing follows the scalar path:

- Under rules whose ``smooth_primitives`` is false, the scalar path ignores
  the point, line and polygon smoothing enables and draws aliased, and so
  does this path.
- Under rules that smooth, the scalar path smooths points, lines and
  polygons, so a point, line or polygon draw with its enable set returns
  ``SGI_RENDER_UNSUPPORTED`` before the snapshot. Odyssey's rules smooth, so
  its smoothed lines stay refused.

Each enable applies only to its own class of primitive. Polygon modes
likewise apply only to polygons, so points ignore them.

Callers supply OpenGL window coordinates: window row 0 is the bottom
display row, and the scissor box is in those rows. A resource's
``bottom_origin`` flag changes only how its rows are stored. For a resource
whose flag is false, the renderer stores window row *y* at native row
*height - 1 - y*, and it flips drawable clip rectangles, which arrive in
native rows, the same way. A family whose native window rows run down the
display, such as Odyssey, maps them to OpenGL rows in its own adapter before
submission. Unlike the scalar rasterizer, which stores window row *y* at
plane row *y* whatever the plane's origin, the renderer does not accept
top-down window rows.

The decoded viewport is passed to the GL worker, including negative origins
and dimensions larger than the resource. Mesa clips to the native
attachment. A negative viewport width or height returns
``SGI_RENDER_UNSUPPORTED``. The worker reads ``GL_MAX_VIEWPORT_DIMS`` once
when it creates its context. A viewport larger than that limit returns
``SGI_RENDER_UNSUPPORTED`` before the native snapshot instead of letting GL
clamp the transform, and the renderer stays usable for later draws. A
polygon-mode array is accepted only when both faces select FILL. LINE and
POINT modes remain unsupported. A nonzero drawable origin remains
unsupported by this draw path.

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

Blending accepts ``FUNC_ADD``, ``FUNC_SUBTRACT``, ``FUNC_REVERSE_SUBTRACT``,
``MIN`` and ``MAX`` with every OpenGL 1.2 factor except the four
constant-colour factors. ``SRC_ALPHA_SATURATE`` is accepted as a source factor
only. A blended draw must be a single point or triangle, and the resource and
rules must use 8-bit colour components. An RGB resource needs rules without
stored alpha, and the worker imports its missing destination alpha as one. An
RGBA resource needs 8-bit stored alpha and rules with 8 alpha bits. The worker
rounds the source colour to those steps before blending, as the scalar path
does. The ``LOGIC_OP`` equation and the logic operations remain unsupported.

A textured draw requires ``GL_TEXTURE_2D``, a front RGB8 or RGBA8 colour
resource, and one ``SGI_RENDER_TEXTURE`` resource matching a level-zero
``SGIGLTexImage``. The image has no stored border, positive power-of-two
width and height, and RGBA base format. It is neither one-dimensional nor
a volume, and has no shadow comparison: the scalar path drops a
one-dimensional image under ``GL_TEXTURE_2D``, and the worker has no
texture comparison. The texture resource has the same
dimensions, bottom-origin rows, and four ordered 8-bit UNORM components.
Colour and texture snapshots both finish before Mesa draws. Texture slot
staging above 16 MiB returns ``SGI_RENDER_UNSUPPORTED``.

Equal ``NEAREST`` or equal ``LINEAR`` minification and magnification filters
are supported. S and T independently accept ``REPEAT``, ``CLAMP``,
``CLAMP_TO_EDGE``, and ``CLAMP_TO_BORDER``. The program samples level zero
as the scalar path does, in double:

- It interpolates s, t and q with the scalar path's 1 / w weights and
  divides by q. A q that is not positive, or a coordinate that is not
  finite, drops the fragment, as the scalar path drops it.
- It reduces each coordinate by its wrap mode: the fractional part for
  ``REPEAT``, a clamp to [0, 1] for ``CLAMP``, to half a texel inside the
  image for ``CLAMP_TO_EDGE`` and half a texel outside it for
  ``CLAMP_TO_BORDER``.
- ``NEAREST`` takes the texel at the floor of size times coordinate, the
  last texel at exactly 1 except under ``CLAMP_TO_BORDER``. ``LINEAR``
  takes the four taps around size times coordinate minus one half, with
  the scalar path's weights, in its order, skipping zero weights.
- A repeated tap index wraps into the image. A tap still outside it reads
  the border colour clamped to [0, 1], separate from stored image-border
  texels. A texel is its 8-bit value divided by 255.
- ``REPLACE``, ``MODULATE``, ``DECAL`` and ``BLEND`` combine the primary
  colour and the texel in [0, 1] with the scalar path's formulas, the
  environment colour clamped to [0, 1]. A flat-shaded draw's primary colour
  is its provoking vertex's, unclamped, as in the scalar path.

Interpolating texture coordinates in float32, as GL varyings do, picked the
other texel where the scalar path's coordinate lies on a texel boundary, as
at a t of 0.5 within rounding on a 2 by 2 image. Mixed or mipmap filters,
stored borders, other texture formats, and other environment modes remain
unsupported.

The path also rejects colour-index drawing, depth and stencil tests, client
clip planes, ordered dithering, accumulation, and other draw states it has
not mapped to Mesa. It snapshots, uploads, reads back and commits only the
region the draw can write: the union of its clip rectangles bounded by the
geometry's window extent with two pixels of margin, where the extent comes
from the scalar path's window triangles, the line parallelograms, or each
triangle clipped to the view volume and each point's centre and size. Clip
rectangles are cut to the region, so the host cannot write outside it. When
a clipped vertex has no window position the region is the whole clip union.
The geometry, viewport and clips the host draws with are unchanged, so the
pixels are too. A draw renders into a 32-bit float host buffer, which is
rounded to the native steps only at readback. That is why a blended draw is limited to one primitive. One
private stencil bit marks the clip
union and another marks written pixels, so primitives of one draw that
overlap each still draw. The worker reads back colour and coverage before any
native commit. The shared code passes the bottom-origin binary write mask
to ``commit_covered``. The provider must leave zero-mask pixels untouched.
Equal-colour writes remain marked. A missing sparse callback returns
``SGI_RENDER_UNSUPPORTED`` before the snapshot. No family provider uses this
path yet, and KONA's tag meaning remains a family contract.

Proven against the scalar path
------------------------------

A frame that mixes Mesa and scalar draws equals an all-scalar frame only for
state that a named unit case proves pixel-identical to ``sgi_soft_draw()`` or
``sgi_soft_raster()``. Every case below runs on the pinned Linux llvmpipe
that ``test-sgi-renderer-gl`` selects, on a 16 by 16 front colour resource,
with the rules named above. Any other driver is unproven.

- ``scalar-match-points``: points of size 1 to 4, 8-bit RGBA.
- ``scalar-match-lines``, ``scalar-match-line-ties``,
  ``scalar-match-line-state``, ``scalar-match-line-stipple`` and
  ``scalar-match-line-sweep``: separate lines, strips and loops at widths 1
  to 6, flat and smooth. They cover:

  - endpoints exactly on diamond edges and corners in every direction, rows
    and columns through pixel centres, slopes of one, and one-subpixel moves
    either side;
  - clip w of 0.5, 1 and 2, clips, scissor and viewport bounds, and segments
    3000 pixels long;
  - one blended segment;
  - stipple patterns and repeats along strips and through a borrowed phase,
    including a draw with no visible pixel;
  - 600 seeded random segments, some cut by view-volume clipping.

  ``lines-refused`` checks the refusals, including the probe.
- ``scalar-match-cross``: every proven width, RGB and RGBA, with every
  polygon mode, flat and smooth, and lines of widths 1, 3 and 6 at 8 bits,
  each under several clips, a scissor, offset and oversized viewports,
  orthographic and perspective matrices and top-origin rows.
- ``mixed-frame-set`` and ``attempt-clear``: the mixed-frame flag refuses
  each limit above before any snapshot, and draws inside the set match the
  scalar path.
- ``smooth-probe-refused``: a renderer whose smooth triangles are moved one
  subpixel fails its probe, refuses smooth triangles and still draws flat
  ones; a host failure during the probe is a failure.
- ``smoothing-parity``: points, lines, strips, loops, triangles, quads and
  polygons with every combination of smoothing enables, flat and smooth,
  under rules that do and do not smooth.
- ``scalar-match-polygons``: flat and smooth triangle strips, triangle fans,
  quadrilaterals, quad strips and polygons, 8-bit RGBA, unblended, with
  identity matrices, a viewport equal to the resource and one full clip.
- ``scalar-match-blend``: one separate triangle, 8-bit RGB and RGBA, every
  accepted blend equation and factor.
- ``scalar-match-state``: flat- and smooth-shaded draws with disjoint,
  touching and overlapping clip rectangles, scissor boxes inside, crossing
  and empty, offset and oversized viewports including the 16384-pixel reach
  viewport, and orthographic and perspective matrices with non-integer
  vertices, each on a bottom-origin resource and on a top-origin resource
  whose rows the family maps into OpenGL rows.
- ``scalar-match-depth-planes``: flat and smooth triangles with an edge in
  the near or far plane, through pixel centres, in every vertex order,
  match, including the pixels the scalar path drops, flat, smooth and
  textured. ``scalar-match-depth-clipped``: 600 random flat and smooth
  triangles on, past and between the planes, with clip w from 0.5 to 2,
  match.
- ``scalar-match-colour-ties``: flat fans and points whose colours are exact
  half steps in float32.
- ``scalar-match-shared-edges``: two triangles sharing a horizontal,
  vertical, diagonal or steep edge through pixel centres, one drawn through
  each path in either order, flat and smooth, on both row origins. The image
  equals the all-scalar image and no pixel is written twice.
- ``scalar-match-widths``: unblended flat and smooth fans and points at 1, 2,
  4, 5, 6 and 10 bits per component, RGB and RGBA, with rules of the same
  width.
- ``scalar-match-texture``: flat-shaded textured triangles for the four
  environments, both filters, every accepted wrap mode, 2 by 2 and 4 by 4
  images, and RGB8 and RGBA8 targets.
- ``scalar-match-texture-boundary``: the triangle whose texel row the
  float32 interpolation got wrong at pixel (4,10).
- ``scalar-match-texture-sweep``: textured triangles with diagonal and
  axis-aligned edges through pixel centres, every vertex order, clip w of
  0.5, 1 and 2, flat and smooth, every environment, both filters and every
  wrap pairing, then 1200 seeded random triangles with images of 1 to 16
  texels a side, square or not, on top-origin rows or under a scissor,
  several clips, an offset viewport or orthographic and perspective
  matrices.
- ``draw-region``: flat and smooth fans, points, lines, a blended and a
  textured triangle, several clips and a scissor on a 300 by 200 resource of
  both row origins equal the scalar path everywhere, and their snapshot and
  commit rectangles stay within 40 by 40 pixels of the geometry.
- ``scalar-match-store-widths``: refused width pairings for draws and CLEAR,
  and RGB stores under alpha rules.
- ``scalar-match-clear``: colour, depth and stencil CLEAR at 4, 8 and 10 bits,
  RGB and RGBA, with one or several clips, a scissor-bounded rectangle and a
  clip past the plane, under full and partial colour masks. The op's
  rectangle is the clips' union bounded by the scissor, as ``sgi_gl_clear()``
  builds it. With a single clip the scalar path writes the whole rectangle
  without testing it against that clip, so it relies on that shape.

Open feature rows
-----------------

These features the shared design requires stay unsupported or unproven. In
each, Mesa differs from the scalar rasterizer, and the scalar rule is the
reviewed family contract. Each refusal returns ``SGI_RENDER_UNSUPPORTED``
before the native snapshot. The ``scalar-match-blend`` unit case keeps the
vectors that showed the differences.

Components wider than 10 bits have no scalar comparison, because the scalar
rules stop at 10 bits. Unblended draws at 12 and 16 bits are accepted but
are outside the proven set.

Constant-colour blend factors, ``CONSTANT_COLOR``, ``ONE_MINUS_CONSTANT_COLOR``,
``CONSTANT_ALPHA`` and ``ONE_MINUS_CONSTANT_ALPHA``, round exact half steps
the other way:

- In 293 of 2100 factor and equation cases, results differ by one step in one
  component.
- With blend colour 0.9 and a stored blue of 195, the exact result is 175.5.
  The scalar path computes in double from the float 0.9 and stores 175.
  llvmpipe computes in float32 and stores 176.
- For one primitive at 8 bits, every other factor matches the scalar path
  pixel for pixel.

Evidence of how the hardware rounds a blended result at a half step would
settle this. It would decide whether to round the constant colour to the
colour steps in both paths or to accept llvmpipe's rounding.

Blending is also limited to one primitive per draw and to 8-bit components:

- **Overlap.** The scalar path stores each primitive's result clamped and
  rounded, so the next primitive blends against the stored value. The float
  host buffer rounds only at readback. Two coincident triangles blending black
  at alpha 1/255 over a stored 64 give 63.749 twice. The scalar path stores 64
  both times, but the float buffer reads back 63. Rendering blended draws into
  an RGBA8 buffer instead rounds each primitive. llvmpipe's 8-bit blend then
  differs from the scalar path by one step, even for a single primitive, in
  320 of 2100 factor and equation cases.
- **Widths other than 8 bits.** The destination crosses the worker as 16-bit
  UNORM, which cannot hold every other width exactly. With 10-bit components,
  black at alpha 1/1023 over a stored 504 is 503.507. The scalar path stores
  504 and the worker 503.

Rounding each primitive's result to the native steps before the next
primitive blends would settle the first. Carrying the destination at its own
width would settle the second.

Untextured draws need a small colour program that the worker builds when it
creates its context. If that program fails to compile, renderer creation
fails, so clears through the same renderer are unavailable too. The
texture, smooth triangle and line programs need OpenGL 4.5. On an older
host the draws that need them return ``SGI_RENDER_UNSUPPORTED``, a host
limit. If one fails to build on a host that has OpenGL 4.5, creation
returns ``SGI_RENDER_CREATE_HOST_ERROR``, because a later refusal would
hide a broken host behind the scalar path.

Decoded CLEAR
-------------

``sgi_render_raster_checked()`` accepts decoded colour, depth, and stencil
CLEAR. Colour resources use three or four ordered UNORM components of 1 to 16
bits. Depth and stencil use separate unsigned, single-component resources at
exactly 24 and 8 bits. Colour-index CLEAR and other raster operations remain
unsupported. The clear masks and inclusive clip rectangles determine which
native pixels may be written. The clear colour is rounded to the native steps
on the CPU in double, as the scalar path rounds it. Storage widths follow the
same rule as draws.

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

The worker keeps the float colour texture, stencil and framebuffer a frame
draws into for the next frame of the same size, and uploads, clears and
reads back only the frame's region, with the draw scissored to it. After a
host failure it rebuilds them.

A legacy request is one 128 by 128 tile of a triangle, and a family splits
each triangle over every tile of its window. A request whose clip holds no
pixel centre inside the triangle's bounding box stages zeros at once,
without a worker round trip, as the host and the software check would both
leave it; it still counts as a draw for the test fault hook.

A family adopting this API must own the renderer for the head's lifetime. It
fails device realization when creation reports ``SGI_RENDER_CREATE_OOM`` or
``SGI_RENDER_CREATE_HOST_ERROR`` and the family requires Mesa. Only
``SGI_RENDER_CREATE_UNAVAILABLE`` lets it draw the whole device on its
scalar path. The family
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

For a checked draw, the family provider must implement
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
GL out-of-memory. An unexpected failure of already validated work must stop
the VM visibly under the big lock, with its host diagnostic, rather than
report guest success or retry through the software rasterizer.

Attempts and per-draw fallback
------------------------------

A family may turn Mesa on while the renderer still refuses some of its
state. Each draw then runs wholly on Mesa or wholly on the family's scalar
path. ``sgi_render_draw_attempt()`` and ``sgi_render_raster_attempt()``
take the same inputs as the checked calls, plus borrowed
``SGIRenderAttemptOptions``, and return ``SGIRenderDrawResult``:

- ``SGI_RENDER_DRAW_OK``: Mesa committed every required pixel.
- ``SGI_RENDER_DRAW_REFUSED``: nothing visible happened and no failure
  latched. ``SGIRenderAttemptOutcome`` carries ``SGI_RENDER_UNSUPPORTED``
  and an ``SGIRenderRefusal`` reason. The next draw tries Mesa again.
- ``SGI_RENDER_DRAW_FAILED``: the outcome carries ``SGI_RENDER_OOM``,
  ``SGI_RENDER_RESOURCE_ERROR`` or ``SGI_RENDER_HOST_ERROR``. The rules
  above apply, and the draw is never retried on the scalar path. A provider
  that returns ``SGI_RENDER_UNSUPPORTED`` from a commit has failed after
  native work began, so the attempt reports a resource error.

``sgi_render_draw_checked()`` and ``sgi_render_raster_checked()`` are the
attempts without options and keep their ``SGIRenderResult``.

A refusal has one of three sources, each decided before any native write:

- The shared renderer refuses state before the native snapshot, so before
  any native access.
- A family snapshot callback returns ``SGI_RENDER_UNSUPPORTED`` when the
  view cannot represent what it read, reason ``SNAPSHOT``. It must only
  have read native storage. A failed or invalid read is a resource error,
  never a refusal.
- The optional ``check_staged`` callback sees each resource's staged slots
  and write coverage after host readback and before the first native
  commit. ``SGI_RENDER_UNSUPPORTED`` refuses with reason ``PRECISION`` and
  commits nothing; any other failure fails the attempt.

Each reason is one feature row, with a stable name from
``sgi_render_refusal_name()`` for counters and traces, and a class from
``sgi_render_refusal_class()``: unsupported state, a host limit that varies
by host, such as the viewport or point-size range, an OpenGL older than
4.5, or a probe that found Mesa differs, or a precision mismatch. A host
failure while a probe runs is a failure, not a refusal.

The fallback runs inside the family's ``draw_checked`` sink callback, or
its raster callback, before that callback returns. On a refusal the
callback draws the same draw on its scalar path and returns that draw's own
``SGIGLDrawResult``, so a scalar out-of-memory result still reaches the
shared GL context, and the context never sees the refusal. Borrowed draw
data, such as the line-stipple phase pointer, is valid only during the
callback, and the renderer leaves a refused draw's phase for the scalar
draw to count. The attempt and its fallback run in the big-lock section
that issued the draw, so no guest access, reset or migration observes the
refused draw between them. A family must defer once-per-draw reports and
validation flags until the draw's path is final.

A frame mixing Mesa and scalar draws equals an all-scalar frame only if
every Mesa draw writes exactly the pixels its scalar draw would. A family
that falls back sets ``mixed_frame`` in the options, and the renderer then
also refuses, with reason ``MIXED_FRAME`` before the snapshot, every draw
outside the set the cases under `Proven against the scalar path`_ prove:

- raster rules the scalar rasterizer implements, with 1, 2, 4, 5, 6, 8 or
  10 colour bits;
- untextured polygons under any state the renderer accepts, but blended
  ones only in the comparisons' fixture: one clip covering a bottom-origin
  resource, no scissor, the viewport equal to the resource and identity
  matrices;
- lines at 8 bits and widths up to 6, under any accepted state;
- unblended points of size up to 4, in that fixture;
- textured triangles under any accepted state, with images of at most 16
  by 16 texels, the largest the texture sweep draws;
- CLEAR at 4, 8 and 10 colour bits.

A family may instead refuse such state itself before calling. Widening the
set needs a scalar-comparison case, or a reviewed change to the scalar
contract.

The set holds only on the reference driver the cases run on: llvmpipe from
Mesa ``SGI_RENDERER_REFERENCE_MESA``, built with LLVM
``SGI_RENDERER_REFERENCE_LLVM``, with ``SGI_RENDERER_REFERENCE_VECTOR_BITS``
bit vectors (``include/ui/sgi-renderer.h``). The renderer reads the GL
vendor, renderer and version strings when it creates its context and
records them, with whether they name that build, in
``sgi_renderer_identity()``. On any other driver, including a
distribution's own Mesa build, the mixed-frame flag refuses every draw and
CLEAR before the snapshot with reason ``HOST_DRIVER``, a host limit, so a
mixing family draws everything on its scalar path. Two draws return OK
before that check, with nothing drawn: one with no window bound, and one
with no clip rectangles and no borrowed stipple phase. The scalar path
draws nothing for either, so frame contents are unaffected. Malformed
input still fails as it does on the reference driver. Another driver joins
only with its own scalar-comparison results recorded here. A family's
precision check through ``check_staged`` runs without the flag and is not a
route to mixing. CPU features such as fused multiply-add are not in the
strings; the runtime probes guard them. The ``driver-identity`` unit case
fails if the tests run on a different build.

Shadow comparison
-----------------

Shadow comparison is a test mode that measures Mesa against the scalar path
on real guest work. The scalar path stays authoritative and is the only
path that writes native storage, so the guest sees exactly scalar
behaviour. ``include/hw/display/sgi/render-shadow.h`` holds the shared
part; a family enables it with a test property and supplies a private
target, the same resources in the same order with callbacks over private
host storage: a snapshot, a dense commit and a covered commit.

For each draw the family:

1. calls ``sgi_render_shadow_capture()`` before the scalar draw, which
   snapshots every native resource at full extent, the texture included,
   and commits it densely into the private copy;
2. runs the scalar draw against native storage as usual;
3. calls ``sgi_render_shadow_draw()`` with the stipple phase as it was
   before the scalar draw, or ``sgi_render_shadow_raster()`` for a decoded
   raster operation such as CLEAR.

The shadow call attempts the draw on its own renderer against the private
target, with the mixed-frame flag clear so that every state Mesa accepts is
exercised, and with the ``quiet`` option, which suppresses the renderer's
own once-per-draw reports. The family's commit callbacks write the result
into the private copy under the family's commit rules. The attempt counts a
borrowed stipple phase in its own copy, started from the value given, and
leaves the scalar draw's phase alone. The call then compares every
non-texture resource of the private copy with native storage, slot by slot
in resource, row and column order, and the two final phases.

It also compares write coverage. The shadow wraps the private target's
commit callbacks and records every pixel the Mesa attempt writes. The
family records the pixels its scalar store callbacks write during the
scalar draw, one byte a pixel per resource in the resource's row order,
and passes them as ``scalar_writes``. A pixel one path wrote and the other
did not is then a mismatch even where both leave the same value: a write
of the colour already held, as in an edge-ownership swap between two
triangles of one colour, or a store whose side effect, such as a tag, the
other path lacks. Without masks for every compared resource only contents
are compared; the outcome's ``coverage_compared`` is false and such draws
are counted apart as ``without_coverage``. A session counts as evidence for
the default only with coverage compared. The call returns:

- ``MATCH``;
- ``MISMATCH``, with the first differing pixel, each path's slots and
  whether each wrote it, or the two phases;
- ``REFUSED``, with the refusal reason; nothing is compared, and it is a
  shadow refusal, not a fallback;
- ``FAILED``, with the cause. A Mesa failure never stops the VM, never
  becomes guest out-of-memory and latches nothing the scalar path uses: the
  shadow renderer is freed and a new one created for the next draw. A
  failed family callback is a failure too, but keeps the renderer;
- ``DISABLED`` when the shadow renderer could not be created, whose reason
  ``sgi_render_shadow_disabled()`` reports for the session.

Each compared draw carries the version 1 signature from
``sgi_render_draw_signature()``: canonical text naming the primitive class,
aliased point size, line width and stipple enables, shade model, blending,
texture environment, filters, wraps and image size, the raster rules, the
colour and alpha storage widths, the row origin, whether the viewport,
scissor, clips and matrices each meet the comparison fixture, and whether
any clipped vertex has a depth within 2^-32 of a plane. Rules are written
by name, so renumbering an enum cannot change a signature, and text that
would not fit is counted as the one signature ``v1 overflow`` rather than
cut. A decoded raster operation's signature names the operation, the
buffers it clears, the colour storage widths and row origin and the rules'
bit widths. Depth and stencil
functions, colour masks, logic op, alpha test and dither are not in
version 1; adding a field the mixed-frame check accepts increments the
version. The signature also says whether the draw is in-set, that is
whether the mixed-frame check accepts it on the reference driver; the tag
and the draw path call the same acceptance function, so they agree. The
session counts matches, mismatches, refusals and failures per signature
(``sgi_render_shadow_counts()``, ``sgi_render_shadow_foreach()``) and in
total, and emits the trace events ``sgi_render_shadow_mismatch``, naming
the signature and the first differing pixel, ``sgi_render_shadow_refused``,
``sgi_render_shadow_failed`` and ``sgi_render_shadow_disabled``. Keeping a
mismatching draw's inputs as a replay vector is the family's part, since
only the family can serialize its draw. Shadow comparison doubles the
rendering work and is a test mode only.

``draw-signature`` checks the signature's stability, its fields, the rule
names, the overflow signature and the in-set tag. ``shadow-comparison``
checks a match, a mismatch from smooth triangles moved one subpixel with
its first differing pixel, a refusal, a failure that does not latch, the
stipple phase compared on its own copy, the count without coverage and a
private target with other rows. ``shadow-coverage`` checks coverage
mismatches where contents agree, and ``shadow-raster`` a matching,
refused, unmasked and coverage-mismatched CLEAR.

Build and test
--------------

Configure a QEMU build with OpenGL and the MIPS64 system target as described
in :doc:`testing/sgi`. From the QEMU source root, run the named unit tests:

.. code-block:: sh

   QEMU_TEST_SGI_REQUIRE_MESA=1 build/pyvenv/bin/meson test -C build \
       --print-errorlogs test-sgi-renderer-gl test-sgi-render-resource

The renderer test selects software llvmpipe and checks the reported GL
renderer. Setting ``QEMU_TEST_SGI_REQUIRE_MESA`` makes missing surfaceless
Mesa fail instead of skipping. The optional render-node case also needs
``QEMU_TEST_SGI_RENDER_NODE`` and device access in the test environment.
These tests exercise Linux surfaceless EGL. They do not qualify GBM render
nodes or macOS and Windows context creation.

Evidence boundary
-----------------

Synthetic tests establish the shared code's behaviour, not physical SGI
hardware behaviour. Hardware-visible claims need the family's reviewed
contract, public evidence, and separate device register and guest acceptance
tests. Do not copy SGI or IRIX source text or identifiers into this tree.
Someone exposed to restricted source for a device cannot implement or
independently review that device's emulator code. Record hardware facts in
the owning public specification with citations to permitted sources.
