#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# build_staticlib.sh CRATE_DIR TARGET_DIR OUTPUT [TRIPLE]: build a standalone
# cargo staticlib (for TRIPLE when cross compiling) and copy it to OUTPUT only
# when its bytes changed.
set -e
crate=$1 target=$2 out=$3 triple=$4
cargo build --release --locked --manifest-path "$crate/Cargo.toml" \
    --target-dir "$target" ${triple:+--target "$triple"} >&2
lib=$target/${triple:+$triple/}release/$(basename "$out")
cmp -s "$lib" "$out" 2>/dev/null || cp "$lib" "$out"
