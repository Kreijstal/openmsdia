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
  Their IIDs and method order have to come from Microsoft's public
  documentation and from observation, such as `QueryInterface` probing and
  vtable-call tracing with the oracle (phase 0b). Done for the four
  callback interfaces of `loadDataForExe` (see "Load callbacks" below):
  the real DLL queries the callback for their IIDs, so they can be
  observed. Interfaces the DLL implements itself (`IDiaEnumFrameData`,
  `IDiaSession2`...) cannot be found that way: a client has to ask for
  them, and their IIDs are in no public, non-header source.

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
           records
```

- **LLVM is used for the file format only:** the MSF container
  (`PDBFile`, `MappedBlockStream`), the stream readers (`InfoStream`,
  `DbiStream` with module list, section contributions, section map and
  headers and debug streams, `TpiStream` for TPI and IPI,
  `ModuleDebugStreamRef` with its debug subsections and line tables,
  `GlobalsStream`, `PublicsStream`, `SymbolStream`, `PDBStringTable`),
  CodeView record deserialization (`SymbolDeserializer`,
  `TypeDeserializer`, `LazyRandomTypeCollection`, `CVTypeVisitor` for
  field lists, `TypeIndex`). Names are undecorated by our own
  `src/undname.cpp`, not by LLVM's `microsoftDemangle`.
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
  docs and observation. Done for the load and read callbacks
  (`include/openmsdia/dia2_extra.h`).
- **Phase 1, behaviour oracle (done):** (plus, since then, the name,
  executable-loading and PGO tests below)
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
- `src/undname.h`, `src/undname.cpp`: the undecorator (no dependencies;
  `tests/undname_cli.cpp` runs it on any host).
- `src/exeload.h`, `src/exeload.cpp`: finding the PDB of an executable;
  `include/openmsdia/dia2_extra.h`: the callback interfaces it uses.
- `walker/name_probe.cpp`, `walker/exe_probe.cpp`: oracles for names and
  for `loadDataForExe`; `tools/undname_test.py`, `tools/exeload_test.py`
  compare their outputs.

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
- **Undecorated names.** `src/undname.cpp` parses a decorated name into
  a small tree and renders it C-declarator style around the name, with
  the real undecorator's spacing and quirks (for example `int * __ptr64
  __ptr64 x`: the pointer's and the variable's `__ptr64`; `(void)const `
  with a trailing space; the `const` of a variable's own pointer printed
  from the storage class, not from the pointer; `{for `A's `B'}`;
  `[thunk]: __cdecl X::`vcall'{8,{flat}}' }'`; class-type template
  arguments `struct Lit{int:7}`). `get_undecoratedName` is
  `get_undecoratedNameEx(0x20000)` (no `__ptr64`); DIA drops a leading
  `.` first. Option bits as observed: 0x1 drops leading underscores of
  keywords, 0x2 all Microsoft keywords, 0x4 return types, 0x10 the
  calling convention of the outermost function, 0x60 (only both bits)
  the `this` qualifiers, 0x80 access, 0x200 `static`/`virtual`, 0x1000
  everything but the name (template arguments without
  class/struct/union, enums keep `enum`), 0x8000 class/struct/union/enum
  keywords, 0x20000 `__ptr64`, 0x40000 prints `...` as `<ellipsis>`;
  0x8, 0x100, 0x400, 0x800, 0x4000, 0x10000 (but for one malformed
  string literal), 0x80000 change nothing. 0x2000 decodes the input as a
  type (`?AUBase@@` is `struct Base`); applied to a symbol name the real
  undecorator prints fragments (`this ?? :: ?? ::Z::YAXPAX`,
  `Access::b` for `Access::pub` with 0x3000) that are not emulated.
  14.30 differs from 14.51 in two places: with 0x2 a function type in a
  template argument loses its parameter list (`Class<int >`), and calling
  convention `C` is `__pascal` (14.51: `__preserve_none`).
- **The Symbols table** is fixed when it is created, not a cache: the
  exe, the compilands, each compiland's symbols depth first, then the
  globals, publics and types in TPI order, each type followed by its
  children again for every type that has them (a const-modified class
  repeats its class's members, function types sharing an argument list
  repeat its arguments). Its count also includes the S_LOCAL and
  S_FILESTATIC symbols, which are listed there and in no findChildren.
  Found by enumerating the real table (`IDiaTable` as
  `IDiaEnumSymbols`): its count is the same right after loading.
- **Frame data** (x86 NEWFPO): the FrameData table's rows, by
  `IDiaTable::Item` or `IEnumUnknown::Next`, are `IDiaFrameData` objects;
  `get_functionParent` fails with E_UNEXPECTED for every row and
  `get_allocatesBasePointer` is S_FALSE. `execute` reproduces what the
  real DLL does with the walker's fake frame: it reads the general
  registers and the 7 code bytes before EIP, fails with E_UNEXPECTED on
  a block that does not start a function, pops the return address at a
  function's first byte (setting the virtual frame and parameter base;
  an EBP-based frame program counts the saved EBP) and unwinds through
  EBP elsewhere. The real DLL was not seen to evaluate the program
  string in these cases. x64 images have no frame data rows.
- **Load callbacks.** `loadDataForExe` and `loadDataFromCodeViewInfo`
  query the callback for `IDiaLoadCallback`
  (C32ADB82-73F4-421B-95D5-A4706EDF5DBE), `IDiaLoadCallback2`
  (4688A074-5A4D-4486-AEA8-7B90711D9F7C), `IDiaReadExeAtOffsetCallback`
  (587A461C-B80B-4F54-9194-5032589A6319) and `IDiaReadExeAtRVACallback`
  (8E3F80CA-7517-432A-BA07-285134AAEA8E), in this order. Slots, from
  which slot is called with which arguments: `NotifyDebugDir` 3 (once per
  28-byte debug directory entry), `NotifyOpenDBG` 4, `NotifyOpenPDB` 5
  (every path tried, with E_PDB_NOT_FOUND 0x806D0005, E_PDB_INVALID_SIG
  0x806D0006 or S_OK), `RestrictRegistryAccess` 6,
  `RestrictSymbolServerAccess` 7, then in `IDiaLoadCallback2`
  `RestrictOriginalPathAccess` 8, `RestrictReferencePathAccess` 9,
  `RestrictDBGAccess` 10, `RestrictSystemRootAccess` 11 (S_OK allows);
  `ReadExecutableAt` and `ReadExecutableAtRVA` are slot 3 of theirs.
  `NotifyDebugDir` is only called through an `IDiaLoadCallback`
  obtained by QueryInterface. With a read callback the executable is not
  opened: the DOS header, the NT headers (264 bytes), the section headers
  one at a time (offset reads only), the debug directory entry by entry
  and the CodeView record are read through it.
- **PDB search order:** the executable's directory, the recorded path
  (with backslashes), then for every `;`-separated search path entry D:
  `D\symbols\<ext>\<pdb>`, `D\<ext>\<pdb>`, `D\<pdb>` (`<ext>` the
  executable's extension; no trimming of spaces), `srv*` entries as local
  symbol stores (`<store>\<pdb>\<GUID><age>\<pdb>`; 14.51 has a symbol
  server DLL beside it, 14.30 on the test machine did not find them),
  `cache*` entries skipped, then the registry (not used here), then
  `%SystemRoot%` like a search path entry. `loadDataFromCodeViewInfo`
  skips the executable's directory and has no `<ext>`. Not found:
  E_PDB_NOT_FOUND with the recorded path as `get_lastError`; a missing
  executable: E_PDB_NOT_FOUND with its path; not a PE image:
  E_PDB_INVALID_EXECUTABLE 0x806D0012; no CodeView record:
  E_PDB_NO_DEBUG_INFO 0x806D0014; a second load: E_UNEXPECTED.
  `_NT_SYMBOL_PATH` and `_NT_ALT_SYMBOL_PATH` are not read.
- **Short PDB files.** MSVC's linker can leave a PDB one partial block
  short of a whole block; LLVM's MSF reader rejects that, so the buffer
  is padded with zeros.
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
  `loadDataForExe(Ex)`, `loadDataFromCodeViewInfo`, `openSession`,
  `get_lastError`. Error codes as observed: 0x806D0005 (not
  found), 0x806D0006 (GUID/signature mismatch), 0x806D0007 (age
  mismatch), 0x806D000C (not a PDB), E_UNEXPECTED (second load, session
  before load).
- `IDiaSession`: `get/put_loadAddress`, `get_globalScope`, `findChildren(Ex)`,
  `findSymbolByAddr/RVA/VA`, `findSymbolByRVAEx/VAEx`, `symbolById`,
  `symsAreEquiv`, `addressForRVA/VA`, `findFile`, `findFileById`,
  `findLines`, `findLinesByAddr/RVA/VA/Linenum`, `getEnumTables`,
  `getSymbolsByAddr`, `getEnumDebugStreams`.
- `IDiaSymbol`..`IDiaSymbol11`: every single-value property getter goes
  through the model; `findChildren(Ex)`, `get_undecoratedNameEx` with
  all options.
  Symbol kinds: Exe, Compiland, CompilandDetails, CompilandEnv, Function
  (with frame data and class-method binding), declaration-only methods,
  Block, FuncDebugStart/End, Data (globals, file statics, static locals,
  TLS, members, bit fields, static members, constants, enumerators,
  register-relative and enregistered locals and parameters), PublicSymbol,
  UDT, Enum, nested types, PointerType, ArrayType, BaseType, FunctionType,
  FunctionArgType, Typedef (global and function-local), BaseClass (incl.
  virtual bases), VTable, InlineSite, CoffGroup, UsingNamespace, CallSite,
  Callee (S_CALLEES), Caller (S_CALLERS, written but not in any corpus
  PDB), Thunk.
- Enumerators: `IDiaEnumSymbols`, `IDiaEnumLineNumbers`,
  `IDiaEnumSourceFiles`, `IDiaEnumSectionContribs`, `IDiaEnumTables`,
  `IDiaEnumSymbolsByAddr` (`symbolByAddr/RVA/VA`, `Next`, `Prev`),
  `IDiaEnumDebugStreams`, `IDiaEnumDebugStreamData`, and empty
  `IDiaEnumInjectedSources`/`IDiaEnumInputAssemblyFiles`.
- `IDiaLineNumber`, `IDiaSourceFile`, `IDiaSectionContrib`, `IDiaTable`
  (`Item` and `Next` for the FrameData table), `IDiaFrameData` (all
  getters and `execute`).

### Not implemented (E_NOTIMPL or missing)

- `loadDataFromMiscInfo` (.dbg files), `getStreamSize`/`getStreamRawData`,
  `ValidatePdb`, registration; symbol server downloads (only local
  symbol stores are searched); the registry search paths.
- `IDiaEnumFrameData` (its IID is not observable, see above); frame
  data lookups by address; `IDiaTable::Item` for tables other than
  FrameData.
- Session: `findChildrenExByAddr/VA/RVA`, `findSymbolByToken`, inline
  frame and inlinee line queries, IL offsets, input assemblies, MD token
  maps, function fragments, exports, heap allocation sites.
- Symbol: `get_types`/`get_typeIds` (S_FALSE), `get_dataBytes`,
  `findInlineFrames*`, `findInlineeLines*`, `getSrcLineOnTypeDefn`,
  `get_sourceLink`, the 14.51 tagged-union methods.
- Symbol kinds: Label, Caller, Friend, CustomType,
  ManagedType, Dimension, HeapAllocationSite, Export, Annotation; S_LOCAL
  variables are deliberately not listed (the real DLL does not either).
- Injected sources, stack walking (`IDiaStackWalker`), x86 and arm64
  builds of the DLL (x86 PDBs are read), HeapAllocationSite symbols.

### Score

`make score` (local oracle) and CI (runner's 14.51) run
`tools/diff_walk.py`. Each field the real DLL reports for a symbol,
line, source file, address lookup, table or debug stream is one
comparison; getters that return S_FALSE count too, so the second line
(fields where the real DLL returned a value) is the stricter number.
See the README for the current numbers (CI run 38577421642: 17 PDBs,
1255651/1255651 fields, 276256/276256 value fields).

Further tests in CI: the undecorator on 2170 names times 26 option sets
(`tools/undname_test.py`; every name matches for every documented
option), `loadDataForExe` in 44 scenarios on 4 executables
(`tools/exeload_test.py`), and the PGO build that links the C runtime
(`corpus/pgo`, 97.6% of 14.2 million fields).

Not covered by the corpus at all: managed code, injected sources,
OMAP-mapped (optimized/instrumented) images, S_CALLERS, S_SEPCODE,
labels of x64 clang code (clang emits none), `IDiaStackWalker`. S_CALLERS
and S_SEPCODE could not be produced: an MSVC 14.51 x64 PGO build
(instrumented build, training run that never takes the error paths,
optimized build) with small and with large never-executed blocks emits
S_CALLEES (3528, mostly in the CRT) and S_INLINEES but neither record.

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
   declarator's `*` and the name; its AST merges the `__ptr64` of a
   pointer and of the variable's storage class (`int C::* __ptr64
   __ptr64 p`). OpenMSDIA now has its own undecorator and no longer uses
   it.
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
