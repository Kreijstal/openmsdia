#!/usr/bin/env bash
# harvest.sh <msdia140.dll> <out-dir>
#
# Record the public interface of a real msdia140.dll as text:
#   version.txt   file version and machine
#   exports.txt   exported function names
#   typelib.json  the embedded Dia2Lib type library (typelib_dump.c)
# Runs on Windows under MSYS2 (CLANG64 or UCRT64). The DLL itself is never
# copied into the output.
set -euo pipefail
dll=$1 out=$2
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "${tmp:?}"' EXIT
mkdir -p "$out"

cc -O2 -o "$tmp/typelib_dump.exe" "$here/typelib_dump.c" -loleaut32 -lole32 -luuid

win=$(cygpath -w "$dll")
ver=$(powershell -NoProfile -Command "(Get-Item -LiteralPath '$win').VersionInfo.FileVersion" | tr -d '\r')
machine=$(llvm-readobj --file-headers "$dll" | sed -n 's/^ *Machine: *//p' | head -1)
printf 'file_version: %s\nmachine: %s\n' "$ver" "$machine" > "$out/version.txt"

llvm-readobj --coff-exports "$dll" | sed -n 's/^ *Name: *//p' | sort > "$out/exports.txt"

"$tmp/typelib_dump.exe" "$dll" > "$tmp/typelib.json"
python3 -m json.tool --indent 1 "$tmp/typelib.json" > "$out/typelib.json"
echo "harvested $(wc -l < "$out/exports.txt") exports, $(grep -c '"kind": "interface"' "$out/typelib.json") interfaces into $out"
