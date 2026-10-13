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

Phase 0 has started. The harvester records the interface of a real
`msdia140.dll` as text. The first snapshot is
`phase0/snapshots/msdia140-14.30.30626.0-amd64/`, taken from the copy that
ships with the Visual Studio Installer. It contains:

- library `Dia2Lib` 2.0 with 47 types;
- 37 interfaces, among them `IDiaSymbol` with 236 methods and `IDiaSession` with 59;
- 3 coclasses: `DiaSource`, `DiaSourceAlt` and `DiaStackWalker`;
- 6 exports, all of them standard COM DLL entry points.

See `docs/plan.md` for the phases and the design.

## Harvest locally (Windows, MSYS2 CLANG64)

```bash
phase0/scripts/harvest.sh "/c/path/to/msdia140.dll" /tmp/snapshot
```

The GitHub Actions workflow `phase0_harvest.yml` does the same with the DIA
SDK of the runner's Visual Studio for amd64, x86 and arm64.
