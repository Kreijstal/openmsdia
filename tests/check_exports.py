#!/usr/bin/env python3
"""Compare a DLL's exported names with a snapshot's exports.txt."""
import subprocess, sys
dll, snap = sys.argv[1], sys.argv[2]
out = subprocess.run(["llvm-readobj", "--coff-exports", dll], capture_output=True, text=True, check=True).stdout
ours = sorted(l.split(":", 1)[1].strip() for l in out.splitlines() if l.strip().startswith("Name:"))
want = sorted(l.strip() for l in open(snap) if l.strip())
if ours != want:
    print("exports differ:\n  ours: %s\n  real: %s" % (ours, want))
    sys.exit(1)
print("exports match: %d names" % len(ours))
