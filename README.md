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

**How:** the PDB format itself is already implemented in the open, by LLVM's
native PDB reader (`llvm/DebugInfo/PDB/Native`). That reader was designed to
mirror DIA's object model. OpenMSDIA is therefore mainly a COM layer. It needs
the exact interface IDs, vtable order and calling conventions, and a property
semantics that matches the real DLL. Both come from harvesting the real
`msdia140.dll` in CI as an oracle.

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
  `-O0` and `-O2`; MSVC in CI), a walker that records everything
  observable through DIA as JSON, the real DLL's output for the clang
  corpus in `oracle/`, and `tools/diff_walk.py` to compare.
- **`msdia140.dll`** built on LLVM's native PDB reader: data source,
  session, symbols (functions, blocks, data, publics, types, classes,
  base classes, methods, inline sites, compilands, COFF groups...), line
  numbers, source files, address lookups, section contributions, tables
  and debug streams. It is statically linked and imports only Windows
  system DLLs; it also runs under Wine 11 with the same output as on
  Windows. Unimplemented methods return E_NOTIMPL; see `docs/plan.md`.

### Score

CI (run 38362568837, GitHub `windows-latest`) walks 9 PDBs (5 built with
clang 23 + lld, 4 with MSVC 14.51, `/Od` and `/O2`) with the runner's
`msdia140.dll` 14.51.36257.0 and with ours:

| section | matched | total | % |
|---|---:|---:|---:|
| symbols | 707583 | 709607 | 99.7 |
| lines | 6840 | 6840 | 100.0 |
| files | 108 | 108 | 100.0 |
| byaddr | 23959 | 24112 | 99.4 |
| symbolsByAddr | 367 | 367 | 100.0 |
| tables | 7758 | 7767 | 99.9 |
| debugStreams | 9 | 9 | 100.0 |
| load | 135 | 135 | 100.0 |
| **overall** | 746759 | 748945 | 99.7 |
| fields where the real DLL returned a value (not S_FALSE) | 154515 | 155550 | 99.3 |

Per PDB, value fields: clang `c_basic` 100.0%, `c_basic_O2` 97.7%,
`c_more` 99.5%, `cpp_classes` 99.9%, `cpp_classes_O2` 99.9%; MSVC
`c_basic` 100.0%, `c_basic_O2` 95.0%, `cpp_classes` 99.9%,
`cpp_classes_O2` 99.0%. Against the local 14.30.30626.0 oracle (clang 22
corpus) the score is 98.9% overall, 98.8% on value fields; OpenMSDIA
follows 14.51 where the two versions differ.

The corpus is small and the implementation was developed against it, so
these numbers measure agreement on the constructs it covers, not on
arbitrary PDBs. Main known gaps: Callee symbols of optimized MSVC code,
the 14.51 error for `get_inlinee`, a few address lookups where 14.51
returns E_UNEXPECTED, and class parents of argument types shared between
function types.

## Build and test (Windows, MSYS2 CLANG64)

```bash
make                    # build/msdia140.dll, build/dia_walk.exe, tests
make check              # exports and vtable layouts
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
