# SPDX-License-Identifier: BSD-3-Clause
#
# Build file for the MAME CHD core as vendored by the libchdman-rs crate (see
# subprojects/libchdman.wrap). The crate has no meson file; this one lists the
# sources its build.rs compiles. MAME itself is untouched.
project('libchdman', ['c', 'cpp'],
        default_options: ['warning_level=0', 'werror=false', 'c_std=gnu11',
                          'cpp_std=c++20', 'default_library=static'])

# QEMU builds with -Werror, which a subproject inherits from the command line;
# this is third-party code with warnings of its own.
no_werror = ['werror=false', 'warning_level=0']

# MAME's LZMA glue hands the encoder blocks from its own arena that are only
# 4-byte aligned for a struct that wants 8. That is harmless on x86-64 and
# arm64, which allow unaligned access, but an UBSan build (configure
# --enable-ubsan) stops on it, hiding the findings in our own code. Leave the
# check off for this third-party code.
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
c_quirks = cc.get_supported_arguments('-fno-sanitize=alignment')
cxx_quirks = cxx.get_supported_arguments('-fno-sanitize=alignment')

mame = 'mame-chd-src'
tp = mame / '3rdparty'
util = mame / 'src' / 'lib' / 'util'

# Link to QEMU's own zlib rather than the copy MAME bundles: two inflate()s in
# one binary would silently pick one for every user. zstd is the same story
# when QEMU links libzstd, so use the system library when it exists and the
# bundled one only when it does not.
zlib = dependency('zlib')
zstd = dependency('libzstd', required: false)

c_defs = []
if host_machine.system() == 'windows'
  c_defs += ['-DFLAC__NO_DLL', '-DUTF8PROC_STATIC']
endif

lzma = static_library('chd_lzma', files(
    tp / 'lzma/C/LzmaDec.c', tp / 'lzma/C/LzmaEnc.c', tp / 'lzma/C/Lzma2Dec.c',
    tp / 'lzma/C/Lzma2Enc.c', tp / 'lzma/C/7zCrc.c', tp / 'lzma/C/7zCrcOpt.c',
    tp / 'lzma/C/CpuArch.c', tp / 'lzma/C/LzFind.c', tp / 'lzma/C/Alloc.c'),
  include_directories: include_directories(tp / 'lzma/C'),
  c_args: ['-DZ7_ST'] + c_quirks, override_options: no_werror)

utf8proc = static_library('chd_utf8proc', files(tp / 'utf8proc/utf8proc.c'),
  include_directories: include_directories(tp / 'utf8proc'),
  c_args: c_defs + c_quirks,
  override_options: no_werror)

# Portable FLAC: the crate builds it with -mavx2 across the whole library,
# which lets the compiler emit AVX2 in generic files that have no CPU check.
# FLAC__NO_ASM keeps it generic C, so the binary runs on any x86-64 or arm64.
flac_src = []
foreach f : ['bitmath', 'bitreader', 'bitwriter', 'cpu', 'crc', 'fixed',
             'float', 'format', 'lpc', 'md5', 'memory', 'metadata_iterators',
             'metadata_object', 'stream_decoder', 'stream_encoder',
             'stream_encoder_framing', 'window']
  flac_src += tp / 'flac/src/libFLAC' / (f + '.c')
endforeach
if host_machine.system() == 'windows'
  flac_src += tp / 'flac/src/share/win_utf8_io/win_utf8_io.c'
endif
# On MinGW, FLAC's compat.h renames fseeko() to fseeko64() unless HAVE_FSEEKO
# is set, which then clashes with the declaration in MinGW's stdio.h.
flac = static_library('chd_flac', files(flac_src),
  include_directories: include_directories(tp / 'flac/include',
                                           tp / 'flac/src/libFLAC/include'),
  c_args: c_defs + ['-DHAVE_FSEEKO=1', '-DHAVE_CONFIG_H', '-DNDEBUG', '-DFLAC__HAS_OGG=0',
                    '-DFLAC__NO_ASM', '-DHAVE_LROUND=1', '-DHAVE_INTTYPES_H=1',
                    '-DHAVE_STDBOOL_H=1', '-DHAVE_STDINT_H=1',
                    '-DHAVE_STDIO_H=1', '-DHAVE_STDLIB_H=1',
                    '-DHAVE_STRING_H=1', '-DSIZE_T_MAX=UINT64_MAX'] + c_quirks,
  override_options: no_werror)

chd_deps = [zlib]
chd_libs = [lzma, utf8proc, flac]
chd_incs = [include_directories('.', util, mame / 'src/osd', tp, tp / 'lzma/C',
                                tp / 'flac/include', tp / 'utf8proc')]
if zstd.found()
  chd_deps += zstd
else
  # The sources of the zstd the crate bundles, listed so that the build needs
  # no shell.
  zstd_files = [
    'common/debug.c', 'common/entropy_common.c', 'common/error_private.c',
    'common/fse_decompress.c', 'common/pool.c', 'common/threading.c',
    'common/xxhash.c', 'common/zstd_common.c',
    'decompress/huf_decompress.c', 'decompress/zstd_ddict.c',
    'decompress/zstd_decompress.c', 'decompress/zstd_decompress_block.c',
    'compress/fse_compress.c', 'compress/hist.c', 'compress/huf_compress.c',
    'compress/zstd_compress.c', 'compress/zstd_compress_literals.c',
    'compress/zstd_compress_sequences.c',
    'compress/zstd_compress_superblock.c', 'compress/zstd_double_fast.c',
    'compress/zstd_fast.c', 'compress/zstd_lazy.c', 'compress/zstd_ldm.c',
    'compress/zstd_opt.c', 'compress/zstdmt_compress.c',
  ]
  zstd_src = []
  foreach f : zstd_files
    zstd_src += tp / 'zstd/lib' / f
  endforeach
  chd_libs += static_library('chd_zstd', files(zstd_src),
    include_directories: include_directories(tp / 'zstd/lib', tp / 'zstd/lib/common'),
    c_args: ['-DZSTD_DISABLE_ASM'] + c_quirks,
    override_options: no_werror)
  chd_incs += include_directories(tp / 'zstd/lib')
endif

cpp_defs = ['-DLSB_FIRST']
if host_machine.endian() == 'big'
  cpp_defs = []
endif
if host_machine.system() == 'windows'
  cpp_defs += ['-DCRLF=3', '-DOSD_WINDOWS', '-DUNICODE', '-D_UNICODE',
               '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0602',
               '-DFLAC__NO_DLL', '-DUTF8PROC_STATIC']
else
  cpp_defs += ['-DCRLF=2', '-DSDLMAME_UNIX']
  cpp_defs += host_machine.system() == 'darwin' ? ['-DSDLMAME_MACOSX', '-DSDLMAME_DARWIN'] : ['-DSDLMAME_LINUX']
endif

mame_files = []
foreach f : ['chd', 'chdcodec', 'hashing', 'ioprocs', 'md5', 'flac', 'huffman',
             'avhuff', 'cdrom', 'strformat', 'corestr', 'unicode', 'bitmap',
             'palette', 'corefile', 'vecstream', 'path']
  mame_files += util / (f + '.cpp')
endforeach
mame_files += mame / 'src/osd/strconv.cpp'
# The crate's stand-in for MAME's OSD layer: a thread work queue and stdio files.
mame_files += 'sys/minimal_osd.cpp'

libchdman = static_library('chdman',
  files(mame_files) + files('qemu-chd-shim.cpp'),
  include_directories: chd_incs + include_directories('sys'),
  dependencies: chd_deps,
  link_with: chd_libs,
  cpp_args: cpp_defs + cxx_quirks, override_options: no_werror)

libchdman_dep = declare_dependency(link_with: libchdman,
  dependencies: chd_deps,
  include_directories: include_directories('.'))
meson.override_dependency('libchdman', libchdman_dep)

# Makes and inspects CHDs for tests/qemu-iotests/tests/chd. The static library
# is C++, so the link has to be too. Only the tests need it, and they depend on
# it, so a plain build leaves it out.
chd_fixture = executable('chd-fixture', 'chd-fixture.c',
  dependencies: libchdman_dep, link_language: 'cpp',
  build_by_default: false, override_options: no_werror)
