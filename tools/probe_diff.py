#!/usr/bin/env python3
"""Compare call_probe outputs (real DLL vs ours): flat JSON maps, key by key.

Usage: probe_diff.py REAL.json OURS.json [...pairs] [--top N]
"""
import json, sys, collections

def main():
    args = sys.argv[1:]
    top = 6
    if "--top" in args:
        i = args.index("--top"); top = int(args[i + 1]); del args[i:i + 2]
    tot = collections.Counter(); bad = collections.Counter(); ex = collections.defaultdict(list)
    for r, o in zip(args[0::2], args[1::2]):
        R, O = json.load(open(r, encoding="utf-8")), json.load(open(o, encoding="utf-8"))
        for k in sorted(set(R) | set(O)):
            kind = k.split("|", 1)[0]
            tot[kind] += 1
            if R.get(k) != O.get(k):
                bad[kind] += 1
                if len(ex[kind]) < top:
                    ex[kind].append((r, k, R.get(k), O.get(k)))
    m = sum(tot.values()) - sum(bad.values())
    print(f"call patterns: {m}/{sum(tot.values())} keys identical")
    for kind in sorted(tot):
        print(f"  {kind:10s} {tot[kind] - bad[kind]}/{tot[kind]}")
    for kind in sorted(ex):
        for r, k, a, b in ex[kind]:
            print(f"  {k[:120]}\n    real={json.dumps(a)[:300]}\n    ours={json.dumps(b)[:300]}")

if __name__ == "__main__":
    main()
