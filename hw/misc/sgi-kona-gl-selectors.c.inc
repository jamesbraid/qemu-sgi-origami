/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality OpenGL doorbell selector table.
 * Maintain these entries alongside tests/unit/test-sgi-kona-gl-decode.c,
 * which checks unresolved layouts, argument counts, and known literal
 * values.
 */

static const SGIKonaGLSelectorInfo sgi_kona_gl_selectors[] = {
    {
        .selector = 0x72,
        .call = "glIndexd",
        .argc = 2,
    },
    {
        .selector = 0x73,
        .call = "glIndexf",
        .argc = 1,
    },
    {
        .selector = 0x74,
        .call = "glIndexi",
        .argc = 1,
    },
    {
        /* The short and unsigned-byte senders share this framing. */
        .selector = 0x75,
        .call = "glIndexs or glIndexub",
        .argc = 1,
    },
    {
        .selector = 0x34,
        .call = "float pixel map header",
        .argc = 4,
    },
    {
        .selector = 0x35,
        .call = "unsigned integer pixel map header",
        .argc = 4,
    },
    {
        .selector = 0x36,
        .call = "unsigned short pixel map header",
        .argc = 4,
    },
    {
        .selector = 0x17b,
        .call = "volume mipmap binding",
        .argc = 14,
    },
    {
        .selector = 0x17c,
        .call = "volume image binding",
        .argc = 4,
    },
    {
        .selector = 0x179,
        .call = "texture sampler record",
        .argc = 22,
    },
    {
        .selector = 0x17d,
        .call = "texture detail binding",
        .argc = 6,
    },
    {
        /*
         * Unresolved. A lone zero word then one computed store. Dozens of
         * other emitters push a literal zero as an argument, and glBegin's
         * second position pushes 0x500 alone, so which word is the selector
         * and how many words follow is not settled.
         */
        .selector = 0x0,
        .call = "glBegin",
        .unresolved = true,
        .argc = 0,
    },
    {
        /*
         * Inferred. One computed store for the one GLbitfield mask.
         */
        .selector = 0x1a,
        .call = "glClear",
        .argc = 1,
    },
    {
        /*
         * Inferred. Four computed stores for four GLfloat components.
         */
        .selector = 0x1b,
        .call = "glClearAccum",
        .argc = 4,
    },
    {
        /*
         * Inferred. One computed store for one GLfloat index.
         */
        .selector = 0x1c,
        .call = "glClearIndex",
        .argc = 1,
    },
    {
        /*
         * Inferred. Four computed stores for four GLclampf components.
         */
        .selector = 0x1d,
        .call = "glClearColor",
        .argc = 4,
    },
    {
        /*
         * Inferred. One computed store for one GLint.
         */
        .selector = 0x1e,
        .call = "glClearStencil",
        .argc = 1,
    },
    {
        /*
         * Unresolved. One computed store of a GLclampd argument: one word if
         * the library narrows it first, two if it stores the double. No
         * argument width is established.
         */
        .selector = 0x1f,
        .call = "glClearDepth",
        .unresolved = true,
        .argc = 0,
    },
    {
        .selector = 0x38,
        .call = "glCopyPixels",
        .argc = 7,
    },
    {
        .selector = 0x39,
        .call = "glReadPixels",
        .argc = 7,
    },
    {
        /*
         * Established. Selector and a literal zero, nothing computed after
         * them.
         */
        .selector = 0x71,
        .call = "glEnd",
        .argc = 1,
        .num_arg0_values = 1,
        .arg0_values = { 0x0 },
    },
    {
        /*
         * Inferred. Two stores for two GLfloat from glVertex2f; the vector
         * form's one store must then be 64 bits wide.
         */
        .selector = 0x8c,
        .call = "glVertex2f",
        .argc = 2,
    },
    {
        /*
         * Inferred. Three stores for three GLfloat from glVertex3f; the
         * vector form's two stores must then be one 64-bit and one 32-bit.
         */
        .selector = 0x90,
        .call = "glVertex3f",
        .argc = 3,
    },
    {
        /*
         * Established. Selector and a literal word: 1 from glFinish, 0 from
         * the swap emitter.
         */
        .selector = 0xc6,
        .call = "glFinish, glXSwapBuffers",
        .argc = 1,
        .num_arg0_values = 2,
        .arg0_values = { 0x0, 0x1 },
    },
    {
        /*
         * Established. Selector and a literal zero, nothing computed after
         * them.
         */
        .selector = 0xc7,
        .call = "glFlush",
        .argc = 1,
        .num_arg0_values = 1,
        .arg0_values = { 0x0 },
    },
    {
        .selector = 0x110,
        .call = "pixel copy overlap query",
        .argc = 4,
    },
    {
        .selector = 0x115,
        .call = "pixel copy convolution query",
        .argc = 4,
    },
    {
        /*
         * Established. The table records four argument words.
         */
        .selector = 0x116,
        .call = "glReadPixels readback",
        .argc = 4,
    },
    {
        /*
         * Unresolved. A lone selector followed by 13 and 14 computed stores
         * in its two emitters, more than one packet's worth; the count is not
         * settled.
         */
        .selector = 0x119,
        .call = "context resolve",
        .unresolved = true,
        .argc = 0,
    },
    {
        /*
         * Established. Selector and a literal zero in both emitters, nothing
         * computed after them.
         */
        .selector = 0x1ca,
        .call = "context resolve",
        .argc = 1,
        .num_arg0_values = 1,
        .arg0_values = { 0x0 },
    },
    {
        /*
         * Established. Selector and a literal zero, nothing computed after
         * them.
         */
        .selector = 0x1cc,
        .call = "glFlushRasterSGIX",
        .argc = 1,
        .num_arg0_values = 1,
        .arg0_values = { 0x0 },
    },
    {
        /*
         * Unresolved. Selector and a literal zero, then 32 computed stores
         * before the next literal run; whether those belong to this packet is
         * not settled.
         */
        .selector = 0x1d6,
        .call = "context resolve",
        .unresolved = true,
        .argc = 0,
    },
    {
        /*
         * Unresolved. Pushed alone by glBegin and also after state words by
         * the fog, light, blend, depth and matrix-mode setters; its role and
         * count are not settled.
         */
        .selector = 0x500,
        .call = "glBegin",
        .unresolved = true,
        .argc = 0,
    },
};
