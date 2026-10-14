#!/usr/bin/env bash
# consumers.sh <real msdia140.dll> <our msdia140.dll> <out-dir> <tool.exe>... -- <pdb>...
#
# Runs real DIA consumers (tools that create DiaSource through COM) once
# with each DLL and compares their outputs. Each DLL is loaded through
# registration-free COM: the tool is copied into a directory of its own
# beside the DLL and an application manifest (<tool>.exe.manifest) that
# maps the DiaSource class to msdia140.dll in that directory. The tool
# must not embed a manifest of its own (link /MANIFEST:NO).
#
# Writes <out-dir>/<tool>/<real|ours>/<pdb>.txt (compare with consumer_diff.py).
set -uo pipefail
real=$1 ours=$2 out=$3
shift 3
tools=()
while [ $# -gt 0 ] && [ "$1" != -- ]; do tools+=("$1"); shift; done
shift
pdbs=("$@")
here=$(cd "$(dirname "$0")" && pwd)

manifest() {  # manifest <exe-name>
    cat <<EOF
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="openmsdia.consumer.$1" version="1.0.0.0" processorArchitecture="amd64"/>
  <file name="msdia140.dll">
    <comClass clsid="{E6756135-1E65-4D17-8576-610761398C3C}" threadingModel="Both"/>
  </file>
</assembly>
EOF
}

for t in "${tools[@]}"; do
    name=$(basename "$t" .exe)
    for who in real ours empty; do
        d="$out/bin-$who/$name"
        mkdir -p "$d"
        cp "$t" "$d/"
        case $who in
            real) cp "$real" "$d/msdia140.dll" ;;
            ours) cp "$ours" "$d/msdia140.dll" ;;
            empty) : > "$d/msdia140.dll" ;;
        esac
        manifest "$name" > "$d/$name.exe.manifest"
    done
done

args_for() {  # the command line of a tool for a PDB
    case $1 in
        Dia2Dump) echo "-all" ;;
        pdb) echo "-fulloutput" ;;
        *) echo "" ;;
    esac
}

for t in "${tools[@]}"; do
    name=$(basename "$t" .exe)
    # Sanity: with an empty msdia140.dll beside it the tool must fail, so
    # the manifest is what loads the DLL (not a registered msdia140).
    first=${pdbs[0]}
    if timeout 60 "$out/bin-empty/$name/$name.exe" $(args_for "$name") "$(cygpath -w "$first")" > /dev/null 2>&1; then
        echo "$name: WARNING: ran with an empty msdia140.dll: the manifest is not used"
    else
        echo "$name: manifest checked (fails with an empty msdia140.dll)"
    fi
    for who in real ours; do
        mkdir -p "$out/$name/$who"
        # Ours records the interfaces the tool asks for and does not get.
        if [ $who = ours ]; then export OPENMSDIA_TRACE_QI="$(cygpath -w "$out/$name/qi.txt")"; else unset OPENMSDIA_TRACE_QI; fi
        for p in "${pdbs[@]}"; do
            n=$(basename "$p" .pdb)
            # Dia2Dump takes the options first, Ghidra's pdb.exe the file first.
            if [ "$name" = pdb ]; then
                timeout 1800 "$out/bin-$who/$name/$name.exe" "$(cygpath -w "$p")" $(args_for "$name") \
                    > "$out/$name/$who/$n.txt" 2> "$out/$name/$who/$n.err"
            else
                timeout 1800 "$out/bin-$who/$name/$name.exe" $(args_for "$name") "$(cygpath -w "$p")" \
                    > "$out/$name/$who/$n.txt" 2> "$out/$name/$who/$n.err"
            fi
            echo "$?" > "$out/$name/$who/$n.rc"
        done
    done
done
unset OPENMSDIA_TRACE_QI
for t in "${tools[@]}"; do
    name=$(basename "$t" .exe)
    [ -f "$out/$name/qi.txt" ] && { echo "$name asked ours for:"; sort "$out/$name/qi.txt" | uniq -c; }
done
# Compare with tools/consumer_diff.py.
