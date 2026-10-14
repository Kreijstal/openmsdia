#!/usr/bin/env python3
"""Compare the outputs of DIA consumers run with the real DLL and with ours.

Usage: consumer_diff.py OUT_DIR TOOL... [--markdown FILE]

OUT_DIR/<tool>/<real|ours>/<pdb>.txt (and .rc, the exit code) come from
tools/consumers.sh. For each tool and PDB: the exit codes, the number of
output lines, and the lines that differ (GNU diff), with the first hunks.
"""
import os, subprocess, sys

def lines(path):
    try:
        with open(path, "rb") as f:
            return f.read().count(b"\n")
    except OSError:
        return 0

def main():
    args = sys.argv[1:]
    md = None
    if "--markdown" in args:
        i = args.index("--markdown")
        md = args[i + 1]
        del args[i:i + 2]
    out, tools = args[0], [t[:-4] if t.endswith(".exe") else t for t in args[1:]]
    rows = []
    for t in tools:
        rdir = os.path.join(out, t, "real")
        if not os.path.isdir(rdir):
            continue
        for f in sorted(os.listdir(rdir)):
            if not f.endswith(".txt"):
                continue
            n = f[:-4]
            r, o = os.path.join(rdir, f), os.path.join(out, t, "ours", f)
            rc = lambda p: open(p[:-4] + ".rc").read().strip() if os.path.exists(p[:-4] + ".rc") else "?"
            d = subprocess.run(["diff", r, o], capture_output=True)
            text = d.stdout.decode("utf-8", "replace").splitlines()
            minus = sum(1 for l in text if l.startswith("<"))
            plus = sum(1 for l in text if l.startswith(">"))
            total = lines(r)
            same = max(total - minus, 0)
            rows.append((t, n, rc(r), rc(o), total, same, minus, plus))
            print(f"== {t} {n}: exit real={rc(r)} ours={rc(o)}; {total} lines, {same} identical "
                  f"({100.0 * same / total if total else 100.0:.2f}%), {minus} differ/missing, {plus} differ/extra")
            for l in text[:24]:
                print("   " + l[:200])
    if md:
        with open(md, "w") as fh:
            fh.write("| tool | PDB | exit (real/ours) | lines | identical | % |\n|---|---|---|---:|---:|---:|\n")
            for t, n, a, b, total, same, minus, plus in rows:
                fh.write(f"| {t} | {n} | {a}/{b} | {total} | {same} | {100.0 * same / total if total else 100.0:.2f} |\n")

if __name__ == "__main__":
    main()
