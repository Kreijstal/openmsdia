#!/usr/bin/env bash
# build.sh <out-dir>
#
# Build the test-PDB corpus with clang + lld (MSYS2 CLANG64, x86_64 MinGW
# target, CodeView debug info). Freestanding, so no CRT objects or import
# libraries end up in the PDB. Deterministic: relative source paths,
# -fdebug-compilation-dir=., /Brepro and a fixed /pdbsourcepath, so neither
# the PDB nor the walker output contains machine paths or timestamps.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
cd "$here"
CC=${CC:-clang}
CXX=${CXX:-clang++}
common=(--target=x86_64-w64-windows-gnu -g -gcodeview -O0 -ffreestanding
        -fno-stack-protector -fno-builtin -fdebug-compilation-dir=. -mno-stack-arg-probe)
link=(-nostdlib -Wl,--entry=start -Wl,--subsystem,console -Wl,--no-insert-timestamp
      -Wl,-Xlink=-pdbsourcepath:C:\\corpus -fuse-ld=lld)

build() { # name lang sources...
    local name=$1 lang=$2; shift 2
    local objs=()
    mkdir -p "$out/$name.obj"
    for src in "$@"; do
        local o="$name.obj/$(basename "$src").o"
        if [ "$lang" = c ]; then
            "$CC" "${common[@]}" -std=c11 -c "$src" -o "$out/$o"
        else
            "$CXX" "${common[@]}" -std=c++17 -fno-rtti -fno-exceptions \
                -fno-threadsafe-statics -c "$src" -o "$out/$o"
        fi
        objs+=("$o")
    done
    (cd "$out" && "$CXX" --target=x86_64-w64-windows-gnu "${link[@]}" \
        -Wl,--pdb="$name.pdb" -o "$name.exe" "${objs[@]}")
}

build c_basic c c_basic/main.c c_basic/util.c
build cpp_classes cxx cpp_classes/main.cpp cpp_classes/shapes.cpp cpp_classes/templates.cpp
echo "corpus built in $out"
