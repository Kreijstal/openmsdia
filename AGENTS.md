# Agent guide

## Clean room

- Never commit Microsoft binaries: `msdia140.dll` itself, the PDBs, `.lib` files or
  PE files produced with the oracle.
- Never commit, copy or paraphrase Microsoft headers or IDL from the DIA SDK
  (`dia2.h`, `cvconst.h`, `dia2.idl`).
- Only interface *facts* are used:
  - names, GUIDs, method order and signatures, and enum values;
  - their sources are the harvested type library, Microsoft's public
    documentation, and observed behaviour.
- Our IDL is written by us from those facts, the way Wine writes its own.
- Behaviour is learned by black-box observation. Load a PDB with the real DLL,
  record the property values as JSON, and make ours produce the same JSON.
  Never disassemble the DLL.

## Research data

- Commit only source, documentation and reviewable text: JSON, CSV and plain-text
  reports.
- Keep downloaded and generated binaries under `/tmp` or in temporary
  GitHub Actions artifacts.
- Name every snapshot after the exact DLL file version and architecture.
- Harvesters must be deterministic: no timestamps and no machine paths in their output.

## Implementation

- Build with MSYS2 CLANG64/UCRT64 clang. COM vtables are plain C function-pointer
  tables, so the C++ ABI does not matter here, unlike openmfc. Keep the COM
  surface in C or C-compatible C++ anyway.
- The PDB file format comes from LLVM's low-level readers (MSF container,
  PDB streams, CodeView records). Do not write a second parser.
- Decorated names are undecorated by our own `src/undname.cpp`, written
  from the published mangling scheme and the oracle (`oracle/undname`);
  it must match the real DLL's spelling, which LLVM's demangler does
  not. Do not copy Wine's undname (LGPL) or Microsoft's.
- The DIA object model is ours (`src/model.*`, `src/types.cpp`). Do not
  use LLVM's high-level DIA emulation (`NativeSession`, `NativeRawSymbol`
  and subclasses, `SymbolCache`, `NativeEnum*`, `IPDB*`, `PDBSymbol*`,
  `PDB_*` enums); `tools/check_layers.sh` enforces this.
- Changes that belong in Wine go to `Kreijstal/wine-kreijstal`, linked back
  here.
- No unrequested PRs to upstream projects.
