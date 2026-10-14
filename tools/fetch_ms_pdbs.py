#!/usr/bin/env python3
"""Download the public PDBs of Windows binaries from Microsoft's symbol server.

Usage: fetch_ms_pdbs.py OUT_DIR BINARY...

For each PE file, read its CodeView (RSDS) debug record and fetch
https://msdl.microsoft.com/download/symbols/<pdb>/<GUID><age>/<pdb> into
OUT_DIR/<stem>[_x86].pdb (a 32-bit image gets an `_x86` suffix, so ntdll of
System32 and of SysWOW64 can sit side by side). An existing file of the right
GUID and age is kept, so OUT_DIR can be a cache. Prints one line per binary.

The PDBs are Microsoft's: they are only ever downloaded, never committed.
"""
import os, struct, sys, time, urllib.request, uuid

SERVER = "https://msdl.microsoft.com/download/symbols"


def codeview(path):
    """(pdb name, symbol server key, is 32-bit) of a PE image, or None."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    is32 = magic == 0x10B
    ddir = opt + (96 if is32 else 112)
    dbg_rva, dbg_size = struct.unpack_from("<II", data, ddir + 6 * 8)
    secs = []
    for i in range(nsec):
        s = opt + optsize + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        secs.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return rva - va + raw
        return None

    o = off(dbg_rva)
    if o is None:
        return None
    for i in range(dbg_size // 28):
        typ, size, rva, ptr = struct.unpack_from("<IIII", data, o + 28 * i + 12)
        if typ != 2 or data[ptr:ptr + 4] != b"RSDS":
            continue
        guid = uuid.UUID(bytes_le=data[ptr + 4:ptr + 20])
        age, = struct.unpack_from("<I", data, ptr + 20)
        name = data[ptr + 24:data.index(b"\0", ptr + 24)].decode("utf-8", "replace")
        name = name.replace("\\", "/").rsplit("/", 1)[-1]
        key = guid.hex.upper() + "%X" % age
        return name, key, is32
    return None


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    rc = 0
    for exe in sys.argv[2:]:
        cv = codeview(exe) if os.path.isfile(exe) else None
        if not cv:
            print(f"{exe}: no CodeView record")
            continue
        name, key, is32 = cv
        stem = name.rsplit(".", 1)[0].lower() + ("_x86" if is32 else "")
        dest = os.path.join(out, stem + ".pdb")
        stamp = dest + ".key"
        if os.path.exists(dest) and os.path.exists(stamp) and open(stamp).read() == key:
            print(f"{stem}: cached {key}")
            continue
        url = f"{SERVER}/{name}/{key}/{name}"
        req = urllib.request.Request(url, headers={"User-Agent": "Microsoft-Symbol-Server/10.0.0.0"})
        err = None
        for attempt in range(5):
            try:
                with urllib.request.urlopen(req, timeout=300) as r, open(dest + ".part", "wb") as f:
                    while True:
                        chunk = r.read(1 << 20)
                        if not chunk:
                            break
                        f.write(chunk)
                err = None
                break
            except Exception as e:  # noqa: BLE001 - retry, then report and go on
                err = e
                time.sleep(2 + 3 * attempt)
        if err is not None:
            print(f"{stem}: {url}: {err}")
            rc = 1
            continue
        os.replace(dest + ".part", dest)
        with open(stamp, "w") as f:
            f.write(key)
        print(f"{stem}: {key} {os.path.getsize(dest)} bytes")
    return rc


if __name__ == "__main__":
    sys.exit(main())
