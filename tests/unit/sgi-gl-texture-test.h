/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SGI_GL_TEXTURE_TEST_H
#define SGI_GL_TEXTURE_TEST_H

#include "hw/display/sgi/rasterizer.h"

bool sgi_gl_texture_test_derivatives(const double value[3],
    const double dx[3], const double dy[3], uint32_t ns, uint32_t nt,
    double out[5]);
bool sgi_gl_texture_test_select(uint32_t min, uint32_t mag,
    const SGIGLTexPyramid *p, uint32_t last, double rho, uint32_t out[3],
    double numeric[2]);
bool sgi_gl_texture_test_plane(const int64_t xy[3][2], unsigned sub,
    const double coords[3][4], const double iw[3], double value[3],
    double dx[3], double dy[3]);
bool sgi_gl_texture_test_sample(const SGIGLTexImage *im,
    const SGIGLTexLevel *level, uint32_t filter, double s, double t,
    double out[4]);
void sgi_gl_texture_test_mix(double value[4], const double other[4],
    double alpha);

#endif
