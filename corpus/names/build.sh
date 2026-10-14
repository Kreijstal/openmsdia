#!/usr/bin/env bash
# build.sh <out-dir>
#
# Build the name corpus (MSYS2 CLANG64): names.cpp with clang-cl for x64
# and x86, and manglings.txt as labels of an assembly file, so that every
# listed decorated name becomes a public symbol of names_list.pdb. Only
# the public symbols matter (walker/name_probe.cpp); the MSVC builds of
# names.cpp come from corpus/build_msvc.ps1 in CI.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
if command -v cygpath >/dev/null; then
    out=$(cygpath -m "$out")
    export MSYS2_ARG_CONV_EXCL="*"  # every path below is already a Windows path
fi
cd "$here"
cl=(clang --driver-mode=cl -nologo)
ccflags=(-c -Z7 -GS- -Zl -GR -std:c++20 -Od -EHs-c- -Zc:threadSafeInit- -Wno-everything)
link() { # machine name obj [link options]
    local machine=$1 name=$2 obj=$3; shift 3
    lld-link -nologo -machine:$machine "$obj" -debug -entry:start -subsystem:console \
        -nodefaultlib -incremental:no -brepro "-pdb:$out/$name.pdb" "-out:$out/$name.exe" "$@"
}
"${cl[@]}" --target=x86_64-pc-windows-msvc "${ccflags[@]}" names.cpp "-Fo$out/names_clang.obj"
link x64 names_clang "$out/names_clang.obj"
"${cl[@]}" --target=i686-pc-windows-msvc "${ccflags[@]}" names.cpp "-Fo$out/names_clang_x86.obj"
link x86 names_clang_x86 "$out/names_clang_x86.obj" -safeseh:no
python3 - manglings.txt > "$out/names_list.s" <<'PY'
import sys
print('\t.text\n\t.globl start\nstart:\n\tret')
seen = set()
for line in open(sys.argv[1], encoding='utf-8'):
    n = line.strip()
    if not n or n.startswith('#') or n in seen:
        continue
    seen.add(n)
    print('\t.globl "%s"\n"%s":\n\tret' % (n, n))
PY
clang --target=x86_64-pc-windows-msvc -c "$out/names_list.s" -o "$out/names_list.obj"
link x64 names_list "$out/names_list.obj"
echo "name corpus built in $out"
