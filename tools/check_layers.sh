#!/usr/bin/env bash
# check_layers.sh [link-map]
#
# OpenMSDIA uses only LLVM's low-level PDB layer (MSF, stream readers,
# CodeView records, the demangler). The DIA object model is ours: nothing
# may use LLVM's high-level DIA emulation (NativeSession, NativeRawSymbol
# and its subclasses, SymbolCache, NativeEnum*, the IPDB* / PDBSymbol*
# interfaces and their PDB_* enums). This checks the sources and, given the
# linker map of msdia140.dll, that none of those objects got linked.
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0

inc='llvm/DebugInfo/PDB/(Native/Native[A-Za-z]*\.h|Native/SymbolCache\.h|IPDB[A-Za-z]*\.h|PDBSymbol[A-Za-z]*\.h|PDBTypes\.h|PDBContext\.h|PDB\.h|DIA/)'
if grep -nE "#include *[\"<]$inc" src/* walker/* tests/*; then
    echo "error: high-level LLVM PDB headers included (see above)"
    fail=1
fi
ids='\b(NativeSession|NativeRawSymbol|SymbolCache|NativeEnum[A-Za-z]*|Native(Exe|Compiland|Function|Public|InlineSite|Type[A-Za-z]*)Symbol|NativeType[A-Za-z]+|IPDB[A-Za-z]+|PDBSymbol[A-Za-z]*|PDB_(SymType|DataKind|LocType|UdtType|BuiltinType|VariantType|CallingConv))\b'
if grep -nE "$ids" src/*.cpp src/*.h; then
    echo "error: high-level LLVM PDB names used (see above)"
    fail=1
fi
if [ $# -ge 1 ]; then
    objs='(Native[A-Za-z]*Symbol|NativeSession|NativeEnum[A-Za-z]*|NativeType[A-Za-z]*|SymbolCache|PDBSymbol[A-Za-z]*|PDBContext|PDB|IPDB[A-Za-z]*|UDTLayout|DIA[A-Za-z]*)\.cpp\.obj'
    if grep -oE "\b$objs" "$1" | sort -u | grep .; then
        echo "error: high-level LLVM PDB objects linked into the DLL (see above)"
        fail=1
    fi
fi
[ $fail = 0 ] && echo "layer check: OK"
exit $fail
