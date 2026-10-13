#!/usr/bin/env bash
# run_corpus.sh <msdia140.dll> <corpus-build-dir> <out-dir>
# Walk every PDB of the built corpus with the given DLL. Machine paths are
# replaced by <ROOT> (the repository) and <CLANG64> (the toolchain prefix).
set -euo pipefail
dll=$1 corpus=$2 out=$3
here=$(cd "$(dirname "$0")/.." && pwd)
walk=${DIA_WALK:-$here/build/dia_walk.exe}
mkdir -p "$out"
maps=(--map "$(cygpath -m "$here")=<ROOT>" --map "$(cygpath -m /clang64)=<CLANG64>"
      --map "$(cygpath -m "$corpus")=<CORPUS>")
for pdb in "$corpus"/*.pdb; do
    name=$(basename "$pdb" .pdb)
    "$walk" "$dll" "$(cygpath -m "$pdb")" "${maps[@]}" > "$out/$name.json"
    echo "$name: $(wc -c < "$out/$name.json") bytes"
done
