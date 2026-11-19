#!/bin/sh
# rustc's linker on Linux: drop the explicit -lstdc++ so g++ links it statically.
for a; do shift; [ "$a" = -lstdc++ ] || set -- "$@" "$a"; done
exec g++ -static-libstdc++ "$@"
