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
  callers and callees, thunks, compilands, COFF groups...), line
  numbers, source files, address lookups, section contributions, tables
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

### Score

CI (run 38577421642, GitHub `windows-latest`) walks 17 PDBs with the
runner's `msdia140.dll` 14.51.36257.0 and with ours: 9 built with clang 23
+ lld (`c_basic`, `c_more`, `cpp_classes`, `cpp_more`; `-O0`, `-O2`, and
32-bit x86 `c_basic_x86`, `cpp_more_x86`) and 8 with MSVC 14.51 (`/Od`,
`/O2`, x86):

| section | matched | total | % |
|---|---:|---:|---:|
| symbols | 1183363 | 1183363 | 100.0 |
| lines | 11832 | 11832 | 100.0 |
| files | 256 | 256 | 100.0 |
| byaddr | 43396 | 43396 | 100.0 |
| symbolsByAddr | 617 | 617 | 100.0 |
| tables | 15911 | 15911 | 100.0 |
| debugStreams | 21 | 21 | 100.0 |
| load | 255 | 255 | 100.0 |
| **overall** | 1255651 | 1255651 | 100.0 |
| fields where the real DLL returned a value (not S_FALSE) | 276256 | 276256 | 100.0 |

Every field the walker records matches on all 17 PDBs, including the
frame data rows of the x86 PDBs (156 rows, with `execute` against a fake
stack frame) and `get_undecoratedNameEx` with three option sets on every
symbol. The previous release scored 1247208/1247229 (21 differences: the
Symbols table count and 4 complete names of pointers to members).

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
  97.6% of 14.2 million fields (96.6% of value fields). It is the first
  corpus PDB with the static CRT's debug information and shows what the
  small corpus does not cover yet (heap allocation sites, labels, CRT
  typedefs, control-flow-guard and noexcept function flags, symbol ids in
  large PDBs).

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
