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

- **Phase 0 (done):** harvest the public interface (type library, exports,
  version) of real `msdia140.dll` builds into `phase0/snapshots/`. CI does amd64,
  x86 and arm64 with the runner's Visual Studio. 14.51 exports a seventh
  name, `CompareRE`, which OpenMSDIA does not implement (signature unknown).
- **Phase 0b:** fill in the interfaces that are not in the type library, from
  docs and observation.
- **Phase 1, behaviour oracle (done):**
  - `corpus/`: freestanding C and C++ sources, built by `corpus/build.sh`
    with clang + lld (`-O0` and `-O2`, deterministic) and by
    `corpus/build_msvc.ps1` with cl/link in CI. PDBs are never committed;
  - `walker/dia_walk.cpp`: loads a given `msdia140.dll` by path, walks the
    whole symbol tree and every property getter (the getter list is
    generated from the type library), lines, source files, address
    lookups, symbols by address, tables, debug streams and load/validation
    error codes, and writes deterministic JSON keyed by lexical path;
  - `oracle/<dll-version-arch>/*.json`: the real DLL's output on the clang
    corpus (14.30 from the Visual Studio Installer copy, 14.51 from CI).
- **Phase 2, generated COM skeleton (done):** `tools/gen_dia.py` writes
  our C++ interface classes, E_NOTIMPL stubs, a getter table and an
  X-macro of symbol properties from the 14.51 snapshot. Tests: exports
  against the snapshot, every vtable slot of the 37 generated interfaces.
- **Phase 3, LLVM-backed implementation (useful subset done):** see
  "Implementation" below. `tools/diff_walk.py` compares walker outputs;
  CI scores OpenMSDIA against the runner's real 14.51 on every push.
- **Phase 4, real tools:** Ghidra's PDB analyzer on Windows and Wine, then
  `dump_syms` and PerfView/TraceEvent. Also `loadDataForExe` (PDB lookup by
  CodeView record and symbol path), `IDiaStackWalker` and injected sources.
- **Later:** a Wine-side story, either as a builtin DLL or a separate package.
  An msys2-ur package `mingw-w64-openmsdia`.

## Implementation

### Layout

- `src/dllmain.cpp`, `src/msdia140.def`: the six 14.30 exports, a class
  factory for `DiaSource` and `DiaSourceAlt`.
- `src/com.cpp`: the COM objects (data source, session, symbol, line
  number, source file, section contribution, tables, enumerators). Every
  method we do not implement comes from the generated stubs and returns
  E_NOTIMPL.
- `src/model.cpp`: the symbol model on top of LLVM.

### Design decisions

- **One id space.** The PDB is opened with LLVM's `NativeSession`. All
  symbols live in LLVM's `SymbolCache`: LLVM's own native symbols for the
  exe, compilands and types (UDT, enum, pointer, array, base type,
  function type, typedef, vtable shape), and our own `NativeRawSymbol`
  subclasses (created with `SymbolCache::createSymbol`) for everything
  the native reader does not model yet. `symIndexId` is the cache id.
- **Property availability per SymTag.** DIA returns S_FALSE for
  properties a symbol does not have; `IPDBRawSymbol` cannot express that
  (every getter returns a value). We keep, per tag, the set of properties
  the real DLL reports, plus explicit rules for conditional ones; values
  come from `IPDBRawSymbol` through the correspondence used by LLVM's own
  DIA backend (`src/rawsym_map.inc`), or from our symbol classes.
- **Observed quirks are emulated** when they are cheap and deterministic,
  for example: `findChildren` on FuncDebugStart/End and most CompilandEnv
  symbols reports the enclosing compiland's child count and enumerates
  nothing; a class method reached through its class answers its first
  `get_name` with the unqualified name and has the exe as lexical parent;
  `virtualTableShape` of a class without a vtable is a no-type base type;
  globals-stream data are separate symbols whose `findChildren` fails;
  references print as `& const` and rvalue references as `*` in
  synthesized names.
- **Target version.** Where 14.30 and 14.51 differ (enumerator types,
  noexcept on member function types, cv properties of overloaded methods,
  coffGroup), OpenMSDIA follows 14.51, the version CI scores against.
- **Static, self-contained DLL.** LLVM, libc++ and libunwind are linked
  statically (`LLVM_BUILD_STATIC`, `-static`); the DLL imports only
  Windows system DLLs (kernel32, ole32, oleaut32, shell32, ntdll and the
  UCRT api sets). LLVM's static `libLLVMDemangle.a` carries dllexport
  directives, stripped at build time so only the `.def` names are exported.

### Implemented

- `IDiaDataSource`/`IDiaDataSourceEx`: `loadDataFromPdb(Ex)`,
  `loadAndValidateDataFromPdb(Ex)`, `loadDataFromIStream(Ex)`,
  `openSession`, `get_lastError`. Error codes as observed: 0x806D0005 (not
  found), 0x806D0006 (GUID/signature mismatch), 0x806D0007 (age
  mismatch), 0x806D000C (not a PDB), E_UNEXPECTED (second load, session
  before load).
- `IDiaSession`: `get/put_loadAddress`, `get_globalScope`, `findChildren(Ex)`,
  `findSymbolByAddr/RVA/VA`, `findSymbolByRVAEx/VAEx`, `symbolById`,
  `symsAreEquiv`, `addressForRVA/VA`, `findFile`, `findFileById`,
  `findLines`, `findLinesByAddr/RVA/VA/Linenum`, `getEnumTables`,
  `getSymbolsByAddr`, `getEnumDebugStreams`.
- `IDiaSymbol`..`IDiaSymbol11`: every single-value property getter goes
  through the model; `findChildren(Ex)`, `get_undecoratedNameEx`.
  Symbol kinds: Exe, Compiland, CompilandDetails, CompilandEnv, Function
  (with frame data and class-method binding), declaration-only methods,
  Block, FuncDebugStart/End, Data (globals, file statics, static locals,
  TLS, members, bit fields, static members, constants, enumerators,
  register-relative and enregistered locals and parameters), PublicSymbol,
  UDT, Enum, nested types, PointerType, ArrayType, BaseType, FunctionType,
  FunctionArgType, Typedef, BaseClass (incl. virtual bases), VTable,
  InlineSite, CoffGroup, UsingNamespace, CallSite.
- Enumerators: `IDiaEnumSymbols`, `IDiaEnumLineNumbers`,
  `IDiaEnumSourceFiles`, `IDiaEnumSectionContribs`, `IDiaEnumTables`,
  `IDiaEnumSymbolsByAddr` (`symbolByAddr/RVA/VA`, `Next`, `Prev`),
  `IDiaEnumDebugStreams`, `IDiaEnumDebugStreamData`, and empty
  `IDiaEnumInjectedSources`/`IDiaEnumInputAssemblyFiles`.
- `IDiaLineNumber`, `IDiaSourceFile`, `IDiaSectionContrib`, `IDiaTable`.

### Not implemented (E_NOTIMPL or missing)

- `loadDataForExe`, `loadDataFromCodeViewInfo`, `loadDataFromMiscInfo`,
  `getStreamSize`/`getStreamRawData`, `ValidatePdb`, registration.
- Session: `findChildrenExByAddr/VA/RVA`, `findSymbolByToken`, inline
  frame and inlinee line queries, IL offsets, input assemblies, MD token
  maps, function fragments, exports, heap allocation sites.
- Symbol: `get_types`/`get_typeIds` (S_FALSE), `get_dataBytes`,
  `findInlineFrames*`, `findInlineeLines*`, `getSrcLineOnTypeDefn`,
  `get_sourceLink`, the 14.51 tagged-union methods.
- Symbol kinds: Label, Thunk, Callee/Caller, Friend, CustomType,
  ManagedType, Dimension, HeapAllocationSite, Export, Annotation; S_LOCAL
  variables are deliberately not listed (the real DLL does not either).
- Frame data (`IDiaFrameData`), injected sources, stack walking, x86 and
  arm64 builds of the DLL.

### Score

`make score` (local oracle) and CI (runner's 14.51) run
`tools/diff_walk.py`. Each field the real DLL reports for a symbol,
line, source file, address lookup, table or debug stream is one
comparison; getters that return S_FALSE count too, so the second line
(fields where the real DLL returned a value) is the stricter number.
See the README for the current numbers.

### LLVM: worth upstreaming

Found while building on LLVM 22's native PDB reader:

1. `NativeTypeFunctionSig::findChildren(FunctionArg)` creates argument
   symbols with SymIndexId 0 that are not in the `SymbolCache`, so they
   cannot be referenced by id.
2. `SymbolCache`'s builtin table maps `SignedCharacter` to `Char` (DIA:
   `btInt`, length 1), `Int32Long`/`UInt32Long` to `Int`/`UInt` (DIA:
   `btLong`/`btULong`), and has no entry for `SByte`/`Byte`, 16-bit ints,
   `Int128(Oct)`, `Float16`/`Float128`, wider booleans: those types get no
   symbol at all (id 0).
3. `NativeExeSymbol`: `getName`, `getMachineType` and `getSignature` are
   not implemented; `hasPrivateSymbols` returns `!isStripped` while the
   DIA backend maps it to `get_isStripped` (the two backends disagree).
4. `NativeCompilandSymbol` has no `findChildren` (functions, data,
   compiland details and environment); `NativeTypeUDT` has no
   `findChildren` (members, base classes, methods, vtable, nested types),
   no sealed/HFA flags, and `getLexicalParentId` returns 0 for types.
5. No native symbols for data (globals, statics, members, constants,
   locals), blocks, FuncDebugStart/End, compiland details/environment,
   base classes, vtables, COFF groups, call sites; `NativePublicSymbol`
   has no code/function flags or length; `NativeFunctionSymbol` has no
   frame (`S_FRAMEPROC`) properties.
6. `SymbolCache::findSymbolBySectOffset` handles functions and publics
   only (the source has a FIXME for data); no blocks.
7. `TpiStream::findFullDeclForForwardRef` misses definitions in
   lld-linked clang `-O2` PDBs where the forward reference has a unique
   name and the definition does not.
8. The Microsoft demangler has no way to print `__ptr64` (undname's
   complete form) and abbreviates special names ("`vector deleting
   dtor'").
9. The installed headers declare LLVM's API `dllimport` unless
   `LLVM_BUILD_STATIC` is defined; linking the static libraries without
   it compiles but can call through a bogus import thunk.

## Related, but not this project

`Microsoft.DiaSymReader.Native` is a *writer* used by crossgen2 `--pdb` and
the compilers. Wine already has `dlls/diasymreader`, with `CreateNGenPdbWriter` still a
stub. Implementing it there, on LLVM's PDB writer, is a separate and much
smaller task.
