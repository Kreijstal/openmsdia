#!/usr/bin/env python3
"""Compare exe_probe outputs (walker/exe_probe.cpp): the real DLL's
loadDataForExe / loadDataFromCodeViewInfo behaviour against ours.

    exeload_test.py [--markdown FILE] REAL.json OURS.json [REAL2.json OURS2.json ...]

Each scenario field (hr, lastError, loaded, callback) is one comparison.
"""
import argparse
import json
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--markdown")
    a = ap.parse_args()
    if len(a.files) % 2:
        ap.error("pairs of REAL OURS expected")
    total = matched = 0
    lines = []
    rows = []
    for i in range(0, len(a.files), 2):
        real = json.load(open(a.files[i], encoding="utf-8"))
        ours = json.load(open(a.files[i + 1], encoding="utf-8"))
        m = t = 0
        for scen, r in real.items():
            o = ours.get(scen)
            if not isinstance(r, dict):
                r, o = {"value": r}, {"value": o}
            o = o if isinstance(o, dict) else {}
            for k, v in r.items():
                t += 1
                if o.get(k) == v:
                    m += 1
                else:
                    lines.append("%s: %s.%s\n    real=%s\n    ours=%s" % (a.files[i], scen, k, json.dumps(v), json.dumps(o.get(k))))
        rows.append((a.files[i], m, t))
        matched += m
        total += t
    for f, m, t in rows:
        print("%-50s %4d/%-4d" % (f, m, t))
    print("exeload: %d/%d = %.1f%%" % (matched, total, 100.0 * matched / max(total, 1)))
    print("\n".join(lines))
    if a.markdown:
        with open(a.markdown, "w", encoding="utf-8") as f:
            f.write("### loadDataForExe\n\n| probe | matched | total |\n|---|---:|---:|\n")
            for n, m, t in rows:
                f.write("| %s | %d | %d |\n" % (n, m, t))
            f.write("| **all** | %d | %d |\n" % (matched, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
