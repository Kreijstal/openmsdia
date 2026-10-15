# OpenMSDIA

OpenMSDIA is a clean-room, binary-compatible reimplementation of Microsoft's
Debug Interface Access library, `msdia140.dll` (the DIA SDK). It is a COM
server: `DiaSource` → `IDiaDataSource` → `IDiaSession` → `IDiaSymbol` and
about thirty related interfaces. Tools use it to read PDB files.

**Why:** many Windows tools load `msdia140.dll` to read PDBs, but it is only
redistributed with Visual Studio. An open drop-in lets them run on Wine, on
ReactOS, or on a Windows machine without Visual Studio, and makes PDB
debugging possible from a fully open toolchain. Known users include
Ghidra's PDB analyzer on Windows. Others are believed to include PerfView/TraceEvent
and Breakpad's `dump_syms`, but that has not been checked here.

**How:** the PDB file format is read with LLVM's low-level readers (the
MSF container, the PDB streams and the CodeView records). Everything DIA-shaped on top of that is OpenMSDIA's
own: symbols and their ids, lexical and class parents, children, address
maps, line tables and source files, and the property semantics of the
real DLL, quirks included. LLVM's own DIA emulation (`NativeSession` and
friends) is not used. The COM side needs the exact interface IDs, vtable
order and calling conventions. Both the interfaces and the behaviour come
from harvesting the real `msdia140.dll` in CI as an oracle.

It is the same kind of project as
[openmfc](https://github.com/Kreijstal/openmfc) (a drop-in `mfc140u.dll`) and
follows [openterminal](https://github.com/Kreijstal/openterminal)'s
harvest-first rules.

## Status

Phases 0 to 3 of `docs/plan.md` are in place, up to a useful subset:

- **Interface snapshots** of `msdia140.dll` 14.30 and 14.51 (amd64, x86,
  arm64) in `phase0/snapshots/`.
- **Generated COM declarations** (`tools/gen_dia.py`, from the type
  library only): 37 interfaces, with tests for the exports and for every
  vtable slot.
- **A behaviour oracle:** a freestanding C/C++ test corpus (clang/lld at
  `-O0` and `-O2` and for x86; MSVC in CI, also x86), a walker that records everything
  observable through DIA as JSON, the real DLL's output for the clang
  corpus in `oracle/`, and `tools/diff_walk.py` to compare.
- **`msdia140.dll`**: data source, session, symbols (functions, blocks,
  data, publics, types, classes, base classes, methods, inline sites,
  callers and callees, thunks, compilands, COFF groups, labels, heap
  allocation sites...), line numbers, source files, address lookups, section contributions, tables
  (the Symbols table as the real DLL lists it, x86 frame data rows as
  `IDiaFrameData`) and debug streams, on our own symbol model over
  LLVM's low-level PDB readers (`tools/check_layers.sh` keeps LLVM's
  high-level layer out). `loadDataForExe` and `loadDataFromCodeViewInfo`
  find a PDB the way the real DLL does, with its load and read
  callbacks. Names are undecorated by our own undecorator
  (`src/undname.cpp`) with every `get_undecoratedNameEx` option. It is
  statically linked and imports only Windows system DLLs; it also runs
  under Wine 11 with the same output as on Windows. Unimplemented
  methods return E_NOTIMPL; see `docs/plan.md`.
- **Windows' own PDBs** (CI job `mspdb`): the public PDBs of the runner's
  ntdll, kernel32, kernelbase, user32, combase, ucrtbase, d3d11, mshtml
  and the 32-bit ntdll, kernel32 and user32, fetched from Microsoft's
  symbol server by `tools/fetch_ms_pdbs.py` (cached, never committed) and
  walked with both DLLs.
- **Real DIA consumers** (CI job `consumers`): Ghidra's PDB exporter
  (`pdb.exe`, built from Ghidra's sources) and the DIA SDK's Dia2Dump
  sample (built from the runner's Visual Studio, used as a black box)
  run with the real `msdia140.dll` and with ours, loaded by
  registration-free COM: `tools/consumers.sh` copies each tool beside the
  DLL with an application manifest mapping the `DiaSource` class to
  `msdia140.dll` (see `docs/plan.md`). The call patterns they showed are
  probed by `walker/call_probe.cpp`.

### Score

CI (run 38635883873, GitHub `windows-latest`) walks 18 PDBs with the
runner's `msdia140.dll` 14.51.36257.0 and with ours: 9 built with clang 23
+ lld (`c_basic`, `c_more`, `cpp_classes`, `cpp_more`; `-O0`, `-O2`, and
32-bit x86 `c_basic_x86`, `cpp_more_x86`) and 9 with MSVC 14.51 (`/Od`,
`/O2`, x86, and `msvc_features` with `/guard:cf`): 1391980/1391980
fields match (469583/469583 where the real DLL returned a value; the
walker now also records, per getter, whether a failing getter writes its
output, debug stream contents and all frame data rows).

Every field the walker records matches on all 18 PDBs, including the
frame data rows of the x86 PDBs (156 rows, with `execute` against a fake
stack frame) and `get_undecoratedNameEx` with three option sets on every
symbol. `msvc_features` holds what was first seen in the C runtime's
debug information: member typedefs, a lambda's function pointer
typedefs, a declared-only struct, const `std::nullptr_t`, 16-bit
constants of 0x8000 and more, a heap allocation site, control-flow-guard
flags and methods of local and anonymous-namespace classes.

Three more tests run in CI against the same real DLL:

- **Undecorated names** (`walker/name_probe.cpp`, `tools/undname_test.py`):
  2170 decorated names (the publics of the corpus, of a C++ name corpus
  built with MSVC and clang-cl, and the inputs of LLVM's demangler tests)
  times 26 `get_undecoratedNameEx` option sets. Ours matches on every
  name for all documented options (0, 0x1-0x10, 0x60, 0x80, 0x200,
  0x1000, 0x8000, 0x20000, 0x40000, 0x87FE, `get_undecoratedName`);
  `UNDNAME_NO_ARGUMENTS` (0x2000, alone 97.9%, combined with
  `NAME_ONLY` 16.8%) makes the real undecorator decode the name as a type
  and print fragments of anything else, which is not emulated.
- **`loadDataForExe`** (`walker/exe_probe.cpp`, `tools/exeload_test.py`):
  44 scenarios (PDB beside the executable, at the recorded path, in the
  search path, in a local symbol store, mismatched, missing; each load
  and read callback interface; CodeView info) on lld and MSVC
  executables, x64 and x86: return codes, `get_lastError` and every
  callback call match.
- **A PGO build linking the C runtime** (`corpus/pgo`, walked on its own):
  every one of its 14,246,519 fields matches (3,750,578 value fields;
  the previous release matched 97.6%). It is the only corpus PDB with
  the static CRT's debug information: 231 compilands, 476 heap allocation
  sites, assembler modules with labels and functions without frame data,
  linker-written import modules, classes defined twice, absolute
  symbols, member typedefs and S_CALLEES records.

- **Microsoft's public PDBs** (job `mspdb`, 11 PDBs, 183,802,687 fields):
  before this work (run 38599337708) 96.3% overall; now (run 38635883873)
  98.0%: user32, wntdll_x86, wkernel32_x86, wuser32_x86 100% exactly;
  ntdll, kernel32, kernelbase, mshtml 100.0% (all but 35 to 1,827 fields,
  folded functions); ucrtbase 99.9%, d3d11 99.8% (folded functions);
  combase 97.5% (95.7% before: a full LTCG PDB, see `docs/plan.md`).
- **Real consumers** (job `consumers`, 13 PDBs: corpus and ntdll,
  kernel32, user32, ucrtbase, 32-bit kernel32): Dia2Dump's output is
  identical on 5 PDBs and ≥ 98.7% identical on the others (at the start:
  83.5% to 99.99%); Ghidra's exporter's output is identical on 6 PDBs and
  ≥ 95.8% on the others once symbol ids are left aside (it prints
  symIndexIds, which differ). `call_probe`: 3216/3234 call patterns
  match 14.51.

Against the local 14.30.30626.0 oracle (`oracle/`, 9 clang PDBs) the
score is 99.9% overall, 99.7% on value fields; OpenMSDIA follows 14.51
where the two versions differ. The corpus is small and the
implementation was developed against it, so these numbers measure
agreement on the constructs it covers, not on arbitrary PDBs.

## Build and test (Windows, MSYS2 CLANG64)

```bash
make                    # build/msdia140.dll, build/dia_walk.exe, tests
make check              # exports, vtable layouts, no high-level LLVM PDB layer
make corpus             # build/corpus/*.pdb
make score              # walk the corpus with ours, compare with oracle/
make oracle MSDIA=/c/path/to/msdia140.dll   # walk with a real DLL
make undname-test       # the undecorator against oracle/undname (any host)
corpus/names/build.sh build/names   # the name corpus (clang-cl, lld)
```

Requires `mingw-w64-clang-x86_64-{clang,lld,llvm,llvm-tools,libc++,zlib,zstd,python}`.

## Harvest locally (Windows, MSYS2 CLANG64)

```bash
phase0/scripts/harvest.sh "/c/path/to/msdia140.dll" /tmp/snapshot
```

The GitHub Actions workflow `phase0_harvest.yml` does the same with the DIA
SDK of the runner's Visual Studio for amd64, x86 and arm64.

## License

Copyright (C) 2026 Kreijstal

openmsdia is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version. See [LICENSE](LICENSE).

The JSON files under `oracle/` record the observed output of Microsoft's
`msdia140.dll` on the test corpus; they contain no Microsoft code.
