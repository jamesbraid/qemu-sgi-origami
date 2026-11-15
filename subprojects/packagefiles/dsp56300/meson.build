# SPDX-License-Identifier: BSD-3-Clause
# The crate's C static library via cargo; outputs change only if bytes do.
project('dsp56300', 'c', version: '1.0.0', license: 'MIT')

cargo = find_program('cargo')
sh = find_program('sh')
objcopy = find_program('objcopy')

script = '''
set -e
cargo=$1 src=$2 lib=$3 hdr=$4 objcopy=$5
tgt=$(dirname "$lib")/cargo
CARGO_TARGET_DIR=$tgt $cargo build --release --locked --manifest-path $src/Cargo.toml -p dsp56300-emu-ffi
$objcopy --redefine-sym dsp56300_reset=dsp56300_jit_reset --redefine-sym dsp56300_run=dsp56300_jit_run --redefine-sym dsp56300_step=dsp56300_jit_step $tgt/release/libdsp56300_emu_ffi.a $lib.new
cmp -s $lib.new $lib && rm $lib.new || mv $lib.new $lib
cmp -s $src/crates/emu-ffi/include/dsp56300.h $hdr || cp $src/crates/emu-ffi/include/dsp56300.h $hdr
'''

lib = custom_target('dsp56300-emu-ffi',
    output: ['libdsp56300_emu_ffi.a', 'dsp56300.h'],
    command: [sh, '-c', script, 'sh', cargo, meson.current_source_dir(),
              '@OUTPUT0@', '@OUTPUT1@', objcopy],
    build_always_stale: true,
    build_by_default: true)

dsp56300_emu_ffi_dep = declare_dependency(
    sources: lib[1],
    link_with: lib[0],
    link_args: ['-lpthread', '-ldl', '-lm'],
    include_directories: include_directories('.'))
meson.override_dependency('dsp56300-emu-ffi', dsp56300_emu_ffi_dep)
