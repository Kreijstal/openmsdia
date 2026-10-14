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
tool ──COM──▶ COM objects (src/com.cpp; declarations generated from the type library)
                  │  DiaSource, IDiaSession, IDiaSymbol, enumerators, tables
                  ▼
           symbol model (src/model.*, src/types.cpp): OpenMSDIA's own
                  │  symbol table and ids, lexical/class parents, children,
                  │  address maps, lines, source files, DIA quirks
                  ▼
           PdbFile (src/pdb.*): stream access, RVA <-> section:offset
                  ▼
           LLVM low-level readers: MSF container, PDB streams, CodeView
           records, Microsoft demangler
```

- **LLVM is used for the file format only:** the MSF container
  (`PDBFile`, `MappedBlockStream`), the stream readers (`InfoStream`,
  `DbiStream` with module list, section contributions, section map and
  headers and debug streams, `TpiStream` for TPI and IPI,
  `ModuleDebugStreamRef` with its debug subsections and line tables,
  `GlobalsStream`, `PublicsStream`, `SymbolStream`, `PDBStringTable`),
  CodeView record deserialization (`SymbolDeserializer`,
  `TypeDeserializer`, `LazyRandomTypeCollection`, `CVTypeVisitor` for
  field lists, `TypeIndex`) and `microsoftDemangle`.
- **The DIA object model is ours.** LLVM's own DIA emulation
  (`NativeSession`, `NativeRawSymbol` and its subclasses, `SymbolCache`,
  `NativeEnum*`, the `IPDB*`/`PDBSymbol*` interfaces and their `PDB_*`
  enums) is not used: it models few symbol kinds, cannot express
  S_FALSE, and its ids and identities differ from DIA's (see the LLVM
  notes below). `tools/check_layers.sh` (part of `make check` and CI)
  rejects those headers and names in the sources and, given the linker
  map, any of their objects in the DLL.
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
- **Phase 3, implementation (useful subset done):** our own DIA model on
  LLVM's low-level PDB readers; see "Implementation" below. `tools/diff_walk.py` compares walker outputs;
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
- `src/pdb.h`, `src/pdb.cpp`: `PdbFile`, the only code that opens LLVM
  stream readers: MSF container, streams, cached module streams, RVA <->
  section:offset from the section headers.
- `src/model.h`, `src/model.cpp`: the symbol table, module-stream
  symbols (functions, blocks, data, inline sites, callees, thunks...),
  class members, globals and publics, children, address maps, lines,
  source files, names.
- `src/types.cpp`: the exe, compiland and type symbols (UDT, enum,
  pointer, array, base type, function type, typedef, vtable shape).
- `src/cvutil.h`: CodeView deserialization helpers.

### Design decisions

- **Our own symbol table.** Every symbol is a `Sym` subclass owned by
  the `Model`; `symIndexId` is its index in the table (0 is no symbol).
  Symbols are created on first use and memoized by what identifies them
  in the PDB:
  - types by type index, after resolving forward references to their
    definition (by unique name, else by name, over the whole TPI
    stream; `TpiStream`'s hash lookup misses some lld `-O2`
    definitions); a modifier (`const`, `volatile`) of a class, enum or
    base type is a symbol of its own, simple types get one symbol per
    type index;
  - module symbols by (module, record offset): functions, blocks, data,
    constants, inline sites, call sites, thunks, local typedefs, using
    namespaces, COFF groups, compiland details; callees by (module,
    S_CALLEES record, position); environment entries per module;
  - class members by (class, position in the field list); function
    arguments by (argument list, position), so function types sharing an
    argument list share its FunctionArgType symbols;
  - globals-stream data, constants and typedefs, and publics by their
    symbol-stream offset.
- **Parents and children.** Each class keeps its lexical and class
  parent; children are built per symbol in DIA's order (exe: compilands,
  UDTs, enums, pointers, arrays, function types in TPI order, typedefs,
  const/volatile base types, globals, function arguments, class members,
  enumerators, publics).
- **Per-enumeration views.** Some properties depend on how a symbol was
  reached, not only on its id. The model returns, with a children list,
  per-entry overrides that the COM enumerators attach to the symbol
  objects they hand out: a class method reached through its class
  answers its first `get_name` with the unqualified name and has the exe
  as lexical parent; a FunctionArgType reports as class parent the
  function type it was enumerated through.
- **Property availability.** DIA returns S_FALSE for properties a symbol
  does not have. Each symbol class answers exactly the properties the
  real DLL reports for its kind and returns false (S_FALSE) for the
  others; a value can also carry an HRESULT the getter fails with.
- **Observed quirks are emulated** when they are cheap and deterministic,
  for example: `findChildren` on FuncDebugStart/End and most CompilandEnv
  symbols reports the enclosing compiland's child count and enumerates
  nothing (callees: the caller's compiland count, 0 for the first callee
  of a record); `virtualTableShape` of a class without a vtable is a
  no-type base type; globals-stream data are separate symbols whose
  `findChildren` fails; static methods report `pure`; pointers to
  members report only the `is*Inheritance` property of their
  representation; references print as `& const` and rvalue references
  as `*` in synthesized names, the `...` of a variadic function as
  `<NoType>`.
- **Target version.** Where 14.30 and 14.51 differ (enumerator types,
  noexcept on member function types, cv properties of overloaded methods,
  coffGroup, `get_inlinee` failing with 0x806D000E, RVA lookups failing
  with E_UNEXPECTED in the last section of lld-linked images), OpenMSDIA
  follows 14.51, the version CI scores against.
- **Static, self-contained DLL.** LLVM, libc++ and libunwind are linked
  statically (`LLVM_BUILD_STATIC`, `-static`; without that define the
  installed headers declare LLVM's API dllimport and calls can go through
  bogus import thunks); the DLL imports only Windows system DLLs
  (kernel32, ole32, oleaut32, shell32, ntdll and the UCRT api sets).
  LLVM's static `libLLVMDemangle.a` carries dllexport directives,
  stripped at build time so only the `.def` names are exported.

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
  FunctionArgType, Typedef (global and function-local), BaseClass (incl.
  virtual bases), VTable, InlineSite, CoffGroup, UsingNamespace, CallSite,
  Callee (S_CALLEES), Thunk.
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
- Symbol kinds: Label, Caller, Friend, CustomType,
  ManagedType, Dimension, HeapAllocationSite, Export, Annotation; S_LOCAL
  variables are deliberately not listed (the real DLL does not either).
- Frame data (`IDiaFrameData`; the FrameData table only reports its
  count), injected sources, stack walking, x86 and arm64 builds of the
  DLL (x86 PDBs are read).

### Score

`make score` (local oracle) and CI (runner's 14.51) run
`tools/diff_walk.py`. Each field the real DLL reports for a symbol,
line, source file, address lookup, table or debug stream is one
comparison; getters that return S_FALSE count too, so the second line
(fields where the real DLL returned a value) is the stricter number.
See the README for the current numbers (CI run 38562770962: 17 PDBs,
1247208/1247229 fields, 267813/267834 value fields).

Remaining known differences against 14.51: the Symbols table count
(follows DIA's internal symbol cache), and the complete undecorated names
of pointers to members (`__ptr64` placement, see the demangler note
below). Not covered by the corpus at all: managed code, injected
sources, frame data records, OMAP-mapped (optimized/instrumented) images,
S_CALLERS, S_SEPCODE, labels of x64 clang code (clang emits none),
`IDiaStackWalker`.

### LLVM: worth upstreaming (documentation only)

Found while building on LLVM 22's native PDB reader. OpenMSDIA no longer
uses LLVM's high-level layer (items 1-6) and works around the others, so
nothing here has to be fixed upstream; the list is kept for whoever
works on LLVM's reader. Nothing is sent upstream from this project.

1. `NativeTypeFunctionSig::findChildren(FunctionArg)` creates argument
   symbols with SymIndexId 0 that are not in the `SymbolCache`, so they
   cannot be referenced by id.
2. `SymbolCache`'s builtin table maps `SignedCharacter` to `Char` (DIA:
   `btInt`, length 1), `Int32Long`/`UInt32Long` to `Int`/`UInt` (DIA:
   `btLong`/`btULong`), and has no entry for `SByte`/`Byte`, 16-bit ints,
   `Int128(Oct)`, `Float16`/`Float128`, wider booleans: those types get no
   symbol at all (id 0). `LF_MODIFIER` of a pointer yields id 0 too,
   which ends `NativeEnumTypes` early.
3. `NativeExeSymbol`: `getName`, `getMachineType` and `getSignature` are
   not implemented; `hasPrivateSymbols` returns `!isStripped` while the
   DIA backend maps it to `get_isStripped` (the two backends disagree).
4. `NativeCompilandSymbol` has no `findChildren` (functions, data,
   compiland details and environment); `NativeTypeUDT` has no
   `findChildren` (members, base classes, methods, vtable, nested types),
   no sealed/HFA flags, and `getLexicalParentId` returns 0 for types.
5. No native symbols for data (globals, statics, members, constants,
   locals), blocks, FuncDebugStart/End, compiland details/environment,
   base classes, vtables, COFF groups, call sites, callees, thunks;
   `NativePublicSymbol` has no code/function flags or length;
   `NativeFunctionSymbol` has no frame (`S_FRAMEPROC`) properties.
6. `SymbolCache::findSymbolBySectOffset` handles functions and publics
   only (the source has a FIXME for data); no blocks.
   `NativeSession::getRVAFromSectOffset` indexes one past the section
   headers for section numbers above their count.
7. `TpiStream::findFullDeclForForwardRef` misses definitions in
   lld-linked clang `-O2` PDBs where the forward reference has a unique
   name and the definition does not. (OpenMSDIA resolves forward
   references itself.)
8. The Microsoft demangler has no way to print `__ptr64` (undname's
   complete form), abbreviates special names ("`vector deleting
   dtor'"), glues cv-qualifiers to `*` and omits the space between a
   declarator's `*` and the name. OpenMSDIA post-processes its output for
   these. What post-processing cannot do and would need an undname of our
   own: the complete form of pointers to members, where undname prints
   the `__ptr64` of the pointer type and of the variable's storage class
   (`int C::* __ptr64 __ptr64 p`), and generally any qualifier the
   demangler's AST drops.
9. The installed headers declare LLVM's API `dllimport` unless
   `LLVM_BUILD_STATIC` is defined; linking the static libraries without
   it compiles but can call through a bogus import thunk.
10. `LazyRandomTypeCollection::contains` is false for records that exist
    but have not been visited yet; use `tryGetType`.

## Related, but not this project

`Microsoft.DiaSymReader.Native` is a *writer* used by crossgen2 `--pdb` and
the compilers. Wine already has `dlls/diasymreader`, with `CreateNGenPdbWriter` still a
stub. Implementing it there, on LLVM's PDB writer, is a separate and much
smaller task.
