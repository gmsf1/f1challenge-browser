#!/usr/bin/env python3
"""List, extract and repack the game's CUBEMAS4.10 archives (.mas): a 12-byte magic, 4 unused bytes, a u32 entry count,
a u32 total, then 256-byte entries from offset 0x18 (u32 kind, u32 data offset, u32 size, u32 stored size, u32 id,
name) and the zlib-compressed files back to back after the table.

  tools/mas.py list  FILE.mas
  tools/mas.py unpack FILE.mas OUTDIR
  tools/mas.py pack  OUTDIR FILE.mas --like ORIGINAL.mas     # same names, order and header values as ORIGINAL
"""
import argparse, os, struct, sys, zlib

MAGIC = b"CUBEMAS4.10\0"


def read(path):
    d = open(path, "rb").read()
    if d[:12] != MAGIC:
        raise SystemExit(f"{path}: not a CUBEMAS4.10 archive ({d[:12]!r})")
    n = struct.unpack_from("<I", d, 0x10)[0]
    base = 0x18 + n * 0x100
    ents = []
    for i in range(n):
        o = 0x18 + i * 0x100
        kind, off, size, stored, ident = struct.unpack_from("<5I", d, o)
        ents.append(dict(kind=kind, off=off, size=size, stored=stored, ident=ident, name=d[o + 20:o + 0x100].split(b"\0")[0].decode("latin1"), entry=d[o:o + 0x100]))
    return d, base, ents


def blob(d, base, e):
    return zlib.decompress(d[base + e["off"]:base + e["off"] + e["stored"]])


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").add_argument("mas")
    p = sub.add_parser("unpack"); p.add_argument("mas"); p.add_argument("out")
    p = sub.add_parser("pack"); p.add_argument("dir"); p.add_argument("mas"); p.add_argument("--like", required=True)
    a = ap.parse_args()
    if a.cmd == "list":
        d, base, ents = read(a.mas)
        for e in ents:
            print(f"{e['size']:>9} {e['stored']:>9}  {e['name']}")
    elif a.cmd == "unpack":
        d, base, ents = read(a.mas)
        os.makedirs(a.out, exist_ok=True)
        for e in ents:
            open(os.path.join(a.out, e["name"]), "wb").write(blob(d, base, e))
    else:
        d, base, ents = read(a.like)
        head = bytearray(d[:0x18 + len(ents) * 0x100])
        body = bytearray()
        for i, e in enumerate(ents):
            path = os.path.join(a.dir, e["name"])
            raw = open(path, "rb").read()
            z = zlib.compress(raw, 9)
            o = 0x18 + i * 0x100
            struct.pack_into("<4I", head, o, e["kind"], len(body), len(raw), len(z))   # keep the id field as it was
            body += z
        # header field 0x14: the total of the stored sizes (plus a fixed amount in some files): keep the original's difference
        delta = struct.unpack_from("<I", d, 0x14)[0] - sum(e["stored"] for e in ents)
        struct.pack_into("<I", head, 0x14, len(body) + delta)
        open(a.mas, "wb").write(bytes(head) + bytes(body))


if __name__ == "__main__":
    main()
