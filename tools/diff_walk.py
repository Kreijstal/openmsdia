#!/usr/bin/env python3
"""Compare two dia_walk outputs: the oracle (real msdia140.dll) and ours.

Usage: diff_walk.py ORACLE.json OURS.json [--top N] [--json score.json]
       diff_walk.py --dirs ORACLE_DIR OURS_DIR [...]   (all *.json by name)

Every field the oracle reports is one comparison: a symbol property, a line
number field, a by-address lookup, a source file property, a table field.
Getters absent from a symbol object returned S_FALSE (walker convention).
Values that are implementation-specific ids (symIndexId, *Id references,
uniqueId, ...) are not compared. "@seen" entries are resolved to the
symbol they refer to.
"""
import argparse, collections, json, os, re, sys

ID_PROPS = {"symIndexId", "lexicalParentId", "classParentId", "typeId", "arrayIndexTypeId",
            "baseSymbolId", "lowerBoundId", "upperBoundId", "subTypeId", "unmodifiedTypeId",
            "virtualTableShapeId", "oemSymbolId", "inlineeId", "compilandId", "sourceFileId",
            "uniqueId"}
GETTER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
META = {"children"}


def expand(obj, universe):
    """Symbol object -> {field: value} with S_FALSE defaults filled in."""
    out = {}
    for k, v in obj.items():
        if k.startswith("!"):
            for n in v:
                if n.startswith("<"):      # interface availability (DLL version)
                    continue
                out[n] = k
        elif k.startswith("@"):
            continue
        else:
            out[k] = v
    for g in universe:
        out.setdefault(g, "!S_FALSE")
    for k in list(out):
        if k in ID_PROPS:
            del out[k]
    return out


def getter_universe(*symbol_maps):
    u = set()
    for S in symbol_maps:
        for obj in S.values():
            for k, v in obj.items():
                if k.startswith("!"):
                    u.update(n for n in v if not n.startswith("<"))
                elif GETTER_RE.match(k) and k not in META:
                    u.add(k)
    return u


def lookup(S, path):
    """Find `path` in S, following @seen redirects of its ancestors: the two
    implementations may expand a shared symbol under different parents."""
    if path in S:
        return path
    parts = path.split("/")
    for i in range(len(parts) - 1, 0, -1):
        q = "/".join(parts[:i])
        obj = S.get(q)
        if obj is not None and "@seen" in obj:
            alt = obj["@seen"] + "/" + "/".join(parts[i:])
            if alt != path:
                return lookup(S, alt) if alt.count("/") < 64 else None
    return None


def resolve(S, path):
    path = lookup(S, path)
    if path is None:
        return None
    obj = S.get(path)
    for _ in range(4):
        if obj is None or "@seen" not in obj:
            break
        obj = S.get(obj["@seen"])
    return obj


class Score:
    def __init__(self):
        self.prop = collections.defaultdict(lambda: [0, 0])  # (section, field) -> [match, total]
        self.valued = [0, 0]   # comparisons where the oracle had a value (not S_FALSE)
        self.examples = collections.defaultdict(list)
        self.missing = collections.Counter()   # section -> records missing in ours
        self.extra = collections.Counter()
        self.missing_paths = []
        self.extra_paths = []

    def cmp(self, section, field, a, b, where):
        st = self.prop[(section, field)]
        st[1] += 1
        if a != "!S_FALSE":
            self.valued[1] += 1
            if a == b:
                self.valued[0] += 1
        if a == b:
            st[0] += 1
        elif len(self.examples[(section, field)]) < 3:
            self.examples[(section, field)].append((where, a, b))

    def totals(self, section=None):
        m = t = 0
        for (s, _), (a, b) in self.prop.items():
            if section is None or s == section:
                m += a
                t += b
        return m, t


def norm_line(e):
    return {k: v for k, v in e.items() if k not in ID_PROPS} if isinstance(e, dict) else {"value": e}


def compare(real, ours, sc, name):
    RS, OS = real.get("symbols", {}), ours.get("symbols", {})
    U = getter_universe(RS, OS)
    for k, v in real.get("load", {}).items():
        sc.cmp("load", k, v, ours.get("load", {}).get(k), name)
    for path in RS:
        r = resolve(RS, path)
        if r is None:
            continue
        r = expand(r, U)
        o = resolve(OS, path)
        if o is None:
            sc.missing["symbols"] += 1
            sc.missing_paths.append(f"{name}:{path}")
            o = {}
        else:
            o = expand(o, U)
        for f, v in r.items():
            sc.cmp("symbols", f, v, o.get(f, "<missing symbol>" if not o else None), f"{name}:{path}")
    for path in OS:
        if lookup(RS, path) is None:
            sc.extra["symbols"] += 1
            sc.extra_paths.append(f"{name}:{path}")
    # Lines: per (compiland|file) key, entries in order.
    for sect in ("lines",):
        R, O = real.get(sect, {}), ours.get(sect, {})
        for key, rv in R.items():
            ov = O.get(key)
            if ov is None:
                sc.missing[sect] += 1
            if not isinstance(rv, list):
                sc.cmp(sect, "result", rv, ov, f"{name}:{key}")
                continue
            ol = ov if isinstance(ov, list) else []
            for i, e in enumerate(rv):
                re_ = norm_line(e)
                oe = norm_line(ol[i]) if i < len(ol) else {}
                for f, v in re_.items():
                    sc.cmp(sect, f, v, oe.get(f), f"{name}:{key}[{i}]")
            if len(ol) > len(rv):
                sc.extra[sect] += len(ol) - len(rv)
    # Keyed records with fields.
    for sect in ("files", "byaddr", "tables", "debugStreams"):
        R, O = real.get(sect, {}), ours.get(sect, {})
        for key, rv in R.items():
            ov = O.get(key)
            if ov is None:
                sc.missing[sect] += 1
            if isinstance(rv, dict):
                od = ov if isinstance(ov, dict) else {}
                for f, v in rv.items():
                    if f in ID_PROPS:
                        continue
                    if isinstance(v, list) and v and isinstance(v[0], dict):
                        ol = od.get(f) if isinstance(od.get(f), list) else []
                        for i, e in enumerate(v):
                            oe = norm_line(ol[i]) if i < len(ol) else {}
                            for g, w in norm_line(e).items():
                                sc.cmp(sect, f + "." + g, w, oe.get(g), f"{name}:{key}[{i}]")
                    else:
                        sc.cmp(sect, f, v, od.get(f), f"{name}:{key}")
            else:
                sc.cmp(sect, "value", rv, ov, f"{name}:{key}")
    # Address-ordered symbols: compare as a multiset.
    R = collections.Counter(real.get("symbolsByAddr", []))
    O = collections.Counter(ours.get("symbolsByAddr", []))
    for k, n in R.items():
        for i in range(n):
            sc.cmp("symbolsByAddr", "entry", True, i < O.get(k, 0), f"{name}:{k}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("real")
    ap.add_argument("ours")
    ap.add_argument("--dirs", action="store_true", help="arguments are directories of *.json")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--json", help="write the score as JSON")
    ap.add_argument("--markdown", help="write a Markdown summary")
    a = ap.parse_args()
    pairs = []
    if a.dirs:
        for f in sorted(os.listdir(a.real)):
            if f.endswith(".json"):
                pairs.append((f[:-5], os.path.join(a.real, f), os.path.join(a.ours, f)))
    else:
        pairs.append((os.path.basename(a.real)[:-5], a.real, a.ours))
    sc = Score()
    for name, r, o in pairs:
        real = json.load(open(r))
        try:
            ours = json.load(open(o))
        except (OSError, ValueError) as e:
            print(f"{name}: cannot read ours: {e}")
            ours = {}
        compare(real, ours, sc, name)

    def pct(m, t):
        return 100.0 * m / t if t else 100.0

    m, t = sc.totals()
    vm, vt = sc.valued
    lines = [f"overall: {m}/{t} = {pct(m, t):.1f}%",
             f"oracle values other than S_FALSE: {vm}/{vt} = {pct(vm, vt):.1f}%"]
    for s in ("load", "symbols", "lines", "files", "byaddr", "symbolsByAddr", "tables", "debugStreams"):
        sm, st = sc.totals(s)
        if st:
            extra = f", {sc.missing[s]} records missing, {sc.extra[s]} extra" if (sc.missing[s] or sc.extra[s]) else ""
            lines.append(f"  {s:14s} {sm}/{st} = {pct(sm, st):.1f}%{extra}")
    print("\n".join(lines))
    def tags(paths):
        c = collections.Counter(p.rsplit("/", 1)[-1].split(":", 1)[0] for p in paths)
        return ", ".join(f"{k} {v}" for k, v in c.most_common())
    if sc.missing_paths:
        print(f"\nmissing symbols by tag: {tags(sc.missing_paths)}")
        for p in sc.missing_paths[: a.top // 2]:
            print("  -", p)
    if sc.extra_paths:
        print(f"extra symbols by tag: {tags(sc.extra_paths)}")
        for p in sc.extra_paths[: a.top // 2]:
            print("  +", p)
    worst = sorted(sc.prop.items(), key=lambda kv: kv[1][0] - kv[1][1])
    print(f"\ntop mismatches (field: matched/total):")
    for (s, f), (mm, tt) in worst[: a.top]:
        if mm == tt:
            break
        print(f"  {s}.{f}: {mm}/{tt}")
        for where, rv, ov in sc.examples[(s, f)][:2]:
            print(f"      {where}\n        real={json.dumps(rv)[:150]}\n        ours={json.dumps(ov)[:150]}")
    if a.json:
        out = {"overall": [m, t], "valued": sc.valued, "sections": {}, "fields": {}}
        for s in ("load", "symbols", "lines", "files", "byaddr", "symbolsByAddr", "tables", "debugStreams"):
            out["sections"][s] = list(sc.totals(s))
        for (s, f), v in sorted(sc.prop.items()):
            out["fields"][f"{s}.{f}"] = v
        json.dump(out, open(a.json, "w"), indent=1, sort_keys=True)
    if a.markdown:
        with open(a.markdown, "w") as fh:
            fh.write("| section | matched | total | % |\n|---|---:|---:|---:|\n")
            for s in ("symbols", "lines", "files", "byaddr", "symbolsByAddr", "tables", "debugStreams", "load"):
                sm, st = sc.totals(s)
                if st:
                    fh.write(f"| {s} | {sm} | {st} | {pct(sm, st):.1f} |\n")
            fh.write(f"| **overall** | {m} | {t} | {pct(m, t):.1f} |\n")
            fh.write(f"| oracle values other than S_FALSE | {vm} | {vt} | {pct(vm, vt):.1f} |\n")


if __name__ == "__main__":
    main()
