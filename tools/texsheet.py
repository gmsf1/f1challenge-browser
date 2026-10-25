"""Decode RC_DUMP_TEX raw textures (level 0) into contact sheets: python3 -I tools/texsheet.py <dir> <out-prefix>"""
import os, struct, sys
from PIL import Image

def dxt1(d, w, h):
    im = Image.new("RGBA", (w, h))
    px = im.load()
    bw = (w + 3) // 4
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            c0, c1, bits = struct.unpack_from("<HHI", d, o)
            def rgb(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
            a, b = rgb(c0), rgb(c1)
            if c0 > c1: pal = [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,), tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
            else: pal = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h: px[x, y] = pal[(bits >> (2 * i)) & 3]
    return im

def raw(d, w, h, fmt):
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if fmt == 0x15 or fmt == 0x16:
                b, g, r, a = d[(y * w + x) * 4:(y * w + x) * 4 + 4]; px[x, y] = (r, g, b, a if fmt == 0x15 else 255)
            else:
                v = struct.unpack_from("<H", d, (y * w + x) * 2)[0]
                if fmt == 0x19: px[x, y] = (((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255 if v & 0x8000 else 0)
                else: px[x, y] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)
    return im

d, out = sys.argv[1], sys.argv[2]
tiles = []
for f in sorted(os.listdir(d)):
    p = f.split("_"); w, h = map(int, p[2].split("x")); fmt = int(p[3], 16)
    data = open(os.path.join(d, f), "rb").read()
    try: im = dxt1(data, w, h) if fmt == 0x31545844 else raw(data, w, h, fmt)
    except Exception as e: print("skip", f, e); continue
    im = im.resize((128, 128), Image.NEAREST) if max(w, h) < 128 else im; im.thumbnail((128, 128)); tiles.append((f, im))
per = 100
for s in range(0, len(tiles), per):
    sheet = Image.new("RGBA", (10 * 132, ((min(per, len(tiles) - s) + 9) // 10) * 132), (255, 0, 255, 255))
    for i, (f, im) in enumerate(tiles[s:s + per]): sheet.paste(im, ((i % 10) * 132, (i // 10) * 132))
    sheet.convert("RGB").save("%s_%d.png" % (out, s // per)); print(out, s // per)
