#!/usr/bin/env python3
"""Compare two name_probe outputs (walker/name_probe.cpp): the real DLL's
undecorated names against ours, per get_undecoratedNameEx option set.

    undname_test.py REAL.json OURS.json [--top N] [--markdown FILE] [--json FILE]

Each (decorated name, option set) pair is one comparison; get_name and
get_undecoratedName are reported as their own rows. A name missing from
OURS counts as a mismatch for every option set.
"""
import argparse
import json
import sys
from collections import OrderedDict


def value(entry, flag):
    ex = entry.get("ex", {})
    return ex.get(flag, ex.get("0x0"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("real")
    ap.add_argument("ours")
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--markdown")
    ap.add_argument("--json")
    a = ap.parse_args()
    real = json.load(open(a.real, encoding="utf-8"))
    ours = json.load(open(a.ours, encoding="utf-8"))
    flags = sorted({f for e in real.values() for f in e.get("ex", {})}, key=lambda f: int(f, 16))
    rows = OrderedDict()
    examples = {}

    def count(row, name, r, o):
        m, t = rows.get(row, (0, 0))
        rows[row] = (m + (r == o), t + 1)
        if r != o:
            examples.setdefault(row, []).append((name, r, o))

    for name, e in real.items():
        oe = ours.get(name, {})
        for key in ("name", "undecoratedName"):
            if key in e:
                count(key, name, e[key], oe.get(key))
        for f in flags:
            count("ex " + f, name, value(e, f), value(oe, f) if oe else None)
    tm = sum(m for m, _ in rows.values())
    tt = sum(t for _, t in rows.values())
    out = ["names: %d, comparisons: %d/%d = %.2f%%" % (len(real), tm, tt, 100.0 * tm / max(tt, 1))]
    for row, (m, t) in rows.items():
        out.append("  %-18s %6d/%-6d = %6.2f%%" % (row, m, t, 100.0 * m / max(t, 1)))
    for row, ex in examples.items():
        out.append("%s: %d mismatches" % (row, len(ex)))
        for name, r, o in ex[: a.top]:
            out.append("    %s\n      real=%s\n      ours=%s" % (name, json.dumps(r), json.dumps(o)))
    print("\n".join(out))
    if a.markdown:
        with open(a.markdown, "w", encoding="utf-8") as f:
            f.write("### Undecorated names (%d names)\n\n| option set | matched | total | %% |\n|---|---:|---:|---:|\n" % len(real))
            for row, (m, t) in rows.items():
                f.write("| %s | %d | %d | %.2f |\n" % (row, m, t, 100.0 * m / max(t, 1)))
            f.write("| **all** | %d | %d | %.2f |\n" % (tm, tt, 100.0 * tm / max(tt, 1)))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"rows": {k: list(v) for k, v in rows.items()}, "matched": tm, "total": tt}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
