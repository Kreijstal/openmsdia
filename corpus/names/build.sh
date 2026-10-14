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
command -v cygpath >/dev/null && out=$(cygpath -m "$out")
cd "$here"
cl=(-nologo -c -Z7 -GS- -Zl -GR -std:c++20 -Od -EHs-c- -Zc:threadSafeInit- -Wno-everything)
link=(-nologo -debug -entry:start -subsystem:console -nodefaultlib -incremental:no -brepro)
clang-cl --target=x86_64-pc-windows-msvc "${cl[@]}" names.cpp "-Fo$out/names_clang.obj"
lld-link "${link[@]}" "-pdb:$out/names_clang.pdb" "-out:$out/names_clang.exe" "$out/names_clang.obj"
clang-cl --target=i686-pc-windows-msvc "${cl[@]}" names.cpp "-Fo$out/names_clang_x86.obj"
lld-link "${link[@]}" -safeseh:no "-pdb:$out/names_clang_x86.pdb" "-out:$out/names_clang_x86.exe" \
    "$out/names_clang_x86.obj"
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
lld-link "${link[@]}" "-pdb:$out/names_list.pdb" "-out:$out/names_list.exe" "$out/names_list.obj"
echo "name corpus built in $out"
