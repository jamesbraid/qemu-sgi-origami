# SPDX-License-Identifier: BSD-3-Clause

# The GPU kernels reproduce this code one rounding per operation. Without
# the flag, aarch64 compilers fuse a * b + c and change the words.
sgi_fp_lib = static_library('sgi-fp',
                            files('gl-context.c', 'gl-select.c', 'rasterizer.c', 'renderer.c') + genh,
                            # The two -D flags are system_ss's own.
                            c_args: ['-DCONFIG_SOFTMMU',
                                     '-DCOMPILING_SYSTEM_VS_USER',
                                     '-ffp-contract=off'],
                            dependencies: [glib],
                            build_by_default: false)
sgi_fp_dep = declare_dependency(link_whole: sgi_fp_lib)
