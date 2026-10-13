# Plan

## What msdia140.dll is

- **Exports:** six COM entry points, `DllGetClassObject`, `DllCanUnloadNow`,
  `Dll(Un)RegisterServer` and `VSDll(Un)RegisterServer`. Everything else goes
  through COM.
- **Type library:** `Dia2Lib` 2.0 (`106173A0-0173-4E5C-84E7-E915422BE997`),
  embedded as a resource. The 14.30.30626.0 snapshot holds:
  - **coclasses:** `DiaSource`, `DiaSourceAlt` (no COM-wide locking) and
    `DiaStackWalker`;
  - **core interfaces:** `IDiaDataSource` (8 methods), `IDiaSession` (59),
    `IDiaSymbol` (236 properties and methods) through `IDiaSymbol9`, and
    `IDiaLineNumber`, `IDiaSourceFile`, `IDiaSectionContrib`, `IDiaFrameData`,
    `IDiaInjectedSource`, `IDiaTable`;
  - **enumerators:** the `IDiaEnum*` family;
  - **stack walking:** `IDiaStackWalker`, `IDiaStackWalkHelper`,
    `IDiaStackFrame` and `IDiaEnumStackFrames`;
  - **enums:** `SymTagEnum`, `MemoryTypeEnum` and `CV_CPU_TYPE_e`.
- **x86, amd64 and arm64 match:** their type libraries are identical apart from `syskind`.
  The arm64 build is an ARM64X binary.
- **14.51.36257.0 vs 14.30.30626.0:** the CI snapshots come from the Visual
  Studio on the CI runners. The newer library only *appends*; no existing
  method moved. It adds:
  - `IDiaDataSourceEx`, which loads with validation, has raw stream access and
    takes a mini-PDB error callback. One of its slots has only a MIDL placeholder
    name, `__MIDL____MIDL_itf_dia2_0000_00050000`;
  - `IDiaSymbol10` (`get_sourceLink`);
  - `IDiaSymbol11` (`get_discriminatedUnionTag`, `get_tagRanges`) and the
    `DiaTagValue` record;
  - `SymTagTaggedUnionCase` and `CV_CFL_UNKNOWN` as new enum values.
- **Not in the type library:** some interfaces that tools use exist only in the
  SDK headers. Examples are `IDiaSession2`/`3`, `IDiaLoadCallback`/`2`,
  `IDiaReadExeAtOffsetCallback`, `IDiaReadExeAtRVACallback`, `IDiaAddressMap`,
  `IDiaPropertyStorage`, `IDiaImageData`, `IDiaEnumFrameData` and `IDiaStackWalkHelper2`.
  This list is from memory and has not been checked. Their IIDs and method
  order have to come from Microsoft's public documentation and from
  observation, such as `QueryInterface` probing and vtable-call tracing with
  the oracle. Phase 0b covers that.

## Design

```
tool ──COM──▶ openmsdia (DiaSource, IDiaSession, IDiaSymbol, …)
                  │  thin C/C++ COM objects, our own IDL
                  ▼
           LLVM NativeSession / NativeRawSymbol (IPDBSession, IPDBRawSymbol)
                  ▼
           LLVM PDB/MSF/CodeView readers
```

- **LLVM's interfaces already follow DIA.** `IPDBRawSymbol` mirrors
  `IDiaSymbol` property for property, and LLVM's own optional DIA backend
  (`llvm/lib/DebugInfo/PDB/DIA/DIARawSymbol.cpp`, Apache-2.0 with LLVM
  exception) maps the two one to one. Reading that mapping backwards gives
  most of the `IDiaSymbol` layer.
- **The gaps** are what the native reader does not implement yet. Many
  `NativeRawSymbol` getters return defaults. They get filled in LLVM, and
  can be upstreamed there, or in this layer.
- **Static linking:** LLVM is linked statically. The DLL must be
  self-contained so that it can be dropped next to a tool or into a Wine prefix.

## Phases

- **Phase 0 (started):** harvest the public interface (type library, exports,
  version) of real `msdia140.dll` builds into `phase0/snapshots/`. CI does amd64,
  x86 and arm64 with the runner's Visual Studio.
- **Phase 0b:** fill in the interfaces that are not in the type library, from
  docs and observation.
- **Phase 1, behaviour oracle:**
  - a test-PDB corpus built from source with clang-cl/lld-link and with MSVC in CI.
    The PDBs stay CI artifacts; the corpus *sources* are committed;
  - a walker that opens each PDB with the real DLL, enumerates every symbol,
    line, table, section contribution and frame-data record, and writes all
    property values as JSON.
- **Phase 2, generated COM skeleton:** generate our IDL, headers and stub
  vtables from the snapshots. `DllGetClassObject`/`DiaSource`/
  `loadDataFromPdb`/`openSession` come first.
- **Phase 3, LLVM-backed implementation:**
  - start with `IDiaSession`, `IDiaSymbol`, `findChildren`, `findSymbolByRVA`,
    line numbers and source files;
  - the score is how much of the phase 1 JSON matches when the walker runs
    against openmsdia;
  - this phase also adds a `diff_walk.py` that compares the two walkers' output.
- **Phase 4, real tools:** Ghidra's PDB analyzer on Windows and Wine, then
  `dump_syms` and PerfView/TraceEvent. Also `loadDataForExe` (PDB lookup by
  CodeView record and symbol path), `IDiaStackWalker` and injected sources.
- **Later:** a Wine-side story, either as a builtin DLL or a separate package.
  An msys2-ur package `mingw-w64-openmsdia`.

## Related, but not this project

`Microsoft.DiaSymReader.Native` is a *writer* used by crossgen2 `--pdb` and
the compilers. Wine already has `dlls/diasymreader`, with `CreateNGenPdbWriter` still a
stub. Implementing it there, on LLVM's PDB writer, is a separate and much
smaller task.
