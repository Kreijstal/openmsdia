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
MSF container, the PDB streams and the CodeView records, plus LLVM's
Microsoft demangler). Everything DIA-shaped on top of that is OpenMSDIA's
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
  callees, thunks, compilands, COFF groups...), line numbers, source
  files, address lookups, section contributions, tables and debug
  streams, on our own symbol model over LLVM's low-level PDB readers
  (`tools/check_layers.sh` keeps LLVM's high-level layer out). It is
  statically linked and imports only Windows system DLLs; it also runs
  under Wine 11 with the same output as on Windows. Unimplemented
  methods return E_NOTIMPL; see `docs/plan.md`.

### Score

CI (run 38562770962, GitHub `windows-latest`) walks 17 PDBs with the
runner's `msdia140.dll` 14.51.36257.0 and with ours: 9 built with clang 23
+ lld (`c_basic`, `c_more`, `cpp_classes`, `cpp_more`; `-O0`, `-O2`, and
32-bit x86 `c_basic_x86`, `cpp_more_x86`) and 8 with MSVC 14.51 (`/Od`,
`/O2`, x86):

| section | matched | total | % |
|---|---:|---:|---:|
| symbols | 1177745 | 1177749 | 100.0 |
| lines | 11832 | 11832 | 100.0 |
| files | 256 | 256 | 100.0 |
| byaddr | 43396 | 43396 | 100.0 |
| symbolsByAddr | 617 | 617 | 100.0 |
| tables | 13086 | 13103 | 99.9 |
| debugStreams | 21 | 21 | 100.0 |
| load | 255 | 255 | 100.0 |
| **overall** | 1247208 | 1247229 | 99.998 |
| fields where the real DLL returned a value (not S_FALSE) | 267813 | 267834 | 99.992 |

Every PDB is at 99.97% or more of its value fields. The 21 remaining
differences: the `count` of the Symbols table (17; it follows DIA's
internal symbol cache, not a PDB structure), and 4 complete undecorated
names of pointers to members, which need an undname of our own (see
`docs/plan.md`). For comparison, the DLL before the switch to our own
symbol model scored 98.4% (98.3% of value fields) on the same 17 PDBs and
99.7% (99.3%) on the 9 PDBs CI walked then, which are now at 100.0%
(155539/155550 value fields).

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
```

Requires `mingw-w64-clang-x86_64-{clang,lld,llvm,llvm-tools,libc++,zlib,zstd,python}`.

## Harvest locally (Windows, MSYS2 CLANG64)

```bash
phase0/scripts/harvest.sh "/c/path/to/msdia140.dll" /tmp/snapshot
```

The GitHub Actions workflow `phase0_harvest.yml` does the same with the DIA
SDK of the runner's Visual Studio for amd64, x86 and arm64.
