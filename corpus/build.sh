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
common=(-g -gcodeview -ffreestanding
        -fno-stack-protector -fno-builtin -fdebug-compilation-dir=. -mno-stack-arg-probe)
link=(-nostdlib -Wl,--entry=start -Wl,--subsystem,console -Wl,--no-insert-timestamp
      -Wl,-Xlink=-pdbsourcepath:C:\\corpus -fuse-ld=lld)

build() { # name lang opt sources... (TARGET overrides the target triple)
    local name=$1 lang=$2 opt=$3; shift 3
    local target=${TARGET:-x86_64-w64-windows-gnu}
    local objs=()
    mkdir -p "$out/$name.obj"
    for src in "$@"; do
        local o="$name.obj/$(basename "$src").o"
        if [ "$lang" = c ]; then
            "$CC" --target=$target "${common[@]}" "$opt" -std=c11 -c "$src" -o "$out/$o"
        else
            "$CXX" --target=$target "${common[@]}" "$opt" -std=c++17 -fno-rtti -fno-exceptions \
                -fno-threadsafe-statics -c "$src" -o "$out/$o"
        fi
        objs+=("$o")
    done
    (cd "$out" && "$CXX" --target=$target "${link[@]}" \
        -Wl,--pdb="$name.pdb" -o "$name.exe" "${objs[@]}")
}

build c_basic c -O0 c_basic/main.c c_basic/util.c
build cpp_classes cxx -O0 cpp_classes/main.cpp cpp_classes/shapes.cpp cpp_classes/templates.cpp
build c_more c -O0 c_more/more.c
# Optimized builds: inline sites, register and optimized-out locals.
build c_basic_O2 c -O2 c_basic/main.c c_basic/util.c
build cpp_classes_O2 cxx -O2 cpp_classes/main.cpp cpp_classes/shapes.cpp cpp_classes/templates.cpp
# Thunks, variadic functions, labels, class bit fields, member pointers.
build cpp_more cxx -O0 cpp_more/more.cpp
build cpp_more_O2 cxx -O2 cpp_more/more.cpp
# 32-bit x86: other pointer sizes, registers and calling conventions.
TARGET=i686-w64-windows-gnu build c_basic_x86 c -O0 c_basic/main.c c_basic/util.c x86/rt.c
TARGET=i686-w64-windows-gnu build cpp_more_x86 cxx -O0 cpp_more/more.cpp
echo "corpus built in $out"
