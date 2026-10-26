#!/usr/bin/env python3
"""AI-upscale the 24-bit BMP and 24/32-bit TGA textures inside .mas archives of a COPY of the game folder (Real-ESRGAN, ncnn/Vulkan).

  uv run --with pillow --with numpy python tools/upscale_mas.py ARCHIVE.mas [ARCHIVE.mas ...]
         [--scale 2] [--model realesrgan-x4plus] [--min 32] [--max-px 1048576] [--skip REGEX] [--method lanczos] [--grain 1.0]

Each archive is rewritten in place (ARCHIVE.mas.orig keeps the original; rerunning starts from it, so a run can be
repeated with other settings). Results are cached by the content of each texture. Never point this at your only copy
of the game. Needs work/realesrgan/bin/realesrgan-ncnn-vulkan and its models (Real-ESRGAN release v0.2.5.0, 20220424 build).

What it leaves alone (unless --grain, which gives them a plain resize plus synthesized grain): organic grit (grass, gravel, tarmac, foliage, sky: the model smooths the grain into mush, see ORGANIC)
and textures with no detail at all (gradients, shadow masks). What it protects: the BMPs' magenta colour key is filled in
before the model sees it and put back exactly afterwards; a TGA's alpha channel is upscaled too when it has detail (fences).
"""
import argparse, hashlib, io, os, re, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mas as M
import detail_synth
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "work", "realesrgan", "bin")
ORGANIC = r"^(GRA|GRV|ROAD|RDGR|RDG|RMBL|TREE|TREL|CLOUD|SKY|DIRT|SAND|BUSH|FOLI|HEDGE|WATER)"
KEY = (255, 0, 255)
TAG = "pad16|s4|key2|alpha1"   # bump when the processing changes: it is part of the cache key


def encode(img, kind):
    out = io.BytesIO()
    if kind == "tga":
        img.save(out, "TGA")   # uncompressed, bottom-left origin, 24 or 32 bit like the originals
    else:
        img.convert("RGB").save(out, "BMP")
    return out.getvalue()


def load(raw, name):
    """-> (kind, image) for a texture this tool handles (24-bit BMP, 24/32-bit uncompressed TGA), else None."""
    n = name.lower()
    try:
        if n.endswith(".bmp") and raw[:2] == b"BM":
            img = Image.open(io.BytesIO(raw))
            return ("bmp", img) if img.mode == "RGB" else None
        if n.endswith(".tga") and len(raw) > 18 and raw[2] == 2 and raw[16] in (24, 32):
            img = Image.open(io.BytesIO(raw))
            return ("tga", img) if img.mode in ("RGB", "RGBA") else None
    except Exception:
        pass
    return None


def key_mask(img):
    a = np.asarray(img.convert("RGB")).astype(np.int16)
    return (a[..., 0] >= 250) & (a[..., 1] <= 5) & (a[..., 2] >= 250)


def detail(plane, ignore=None):
    """Fraction of pixels that are not flat (0 = a gradient or solid, near 1 = busy); `plane` is H x W (or H x W x 3)."""
    g = np.asarray(plane).astype(np.int16)
    if g.ndim == 3:
        g = g.mean(axis=2)
    grad = np.maximum(np.abs(np.diff(g, axis=1))[:-1, :], np.abs(np.diff(g, axis=0))[:, :-1])
    ok = np.ones(grad.shape, bool) if ignore is None else ~ignore[:-1, :-1]
    return float(((grad >= 3) & ok).sum() / max(1, ok.sum()))


def fill_key(img, mask):
    """Replace colour-key pixels by the colours around them, so the model never sees (or shades) the key."""
    a = np.asarray(img.convert("RGB")).astype(np.float32).copy()
    m = mask.copy()
    for _ in range(64):
        if not m.any():
            break
        valid = (~m).astype(np.float32)
        acc, cnt = np.zeros_like(a), np.zeros(m.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                acc += np.roll(a * valid[..., None], (dy, dx), (0, 1))
                cnt += np.roll(valid, (dy, dx), (0, 1))
        fillable = m & (cnt > 0)
        a[fillable] = acc[fillable] / cnt[fillable][:, None]
        m = m & ~fillable
    return Image.fromarray(a.clip(0, 255).astype(np.uint8))


def wrap_pad(img, p):
    """Tile the image into a border of p pixels, so the model sees the neighbours a repeating texture has."""
    w, h = img.size
    canvas = Image.new(img.mode, (w + 2 * p, h + 2 * p))
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            canvas.paste(img, (p + dx * w, p + dy * h))
    return canvas


def run_model(indir, outdir, a):
    # always 4x: with -s 2 this tool crops the 4x result instead of resizing it; callers shrink to the wanted scale
    subprocess.run([os.path.join(BIN, "realesrgan-ncnn-vulkan"), "-i", indir, "-o", outdir, "-n", a.model, "-s", "4", "-f", "png"],
                   cwd=BIN, check=True, capture_output=True)


def grain(raw, name, kind, img, a):
    """Organic texture -> Lanczos `scale`x plus synthesized grain (tools/detail_synth.py); cached, key and alpha handled."""
    cached = os.path.join(a.cache, hashlib.sha1(raw + f"|grain{a.grain}|{a.scale}".encode()).hexdigest() + "." + kind)
    if os.path.exists(cached):
        return open(cached, "rb").read()
    sc, (w, h) = a.scale, img.size
    km = key_mask(img) if kind == "bmp" else None
    has_key = km is not None and km.mean() > 0.002
    base = fill_key(img, km) if has_key else img.convert("RGB")
    pad = min(16, w // 2, h // 2)
    up = detail_synth.enhance(wrap_pad(base, pad), sc, a.grain).crop((pad * sc, pad * sc, (pad + w) * sc, (pad + h) * sc))
    if img.mode == "RGBA":
        up.putalpha(img.getchannel("A").resize((w * sc, h * sc), Image.LANCZOS))
    if has_key:
        up.paste(KEY, mask=Image.fromarray((km * 255).astype(np.uint8)).resize((w * sc, h * sc), Image.NEAREST))
    out = encode(up, kind)
    open(cached, "wb").write(out)
    return out


def process(path, a):
    orig = path + ".orig"
    if not os.path.exists(orig):
        shutil.copy2(path, orig)
    d, base, ents = M.read(orig)
    os.makedirs(a.cache, exist_ok=True)
    sc = a.scale
    h = lambda raw, extra="": hashlib.sha1(raw + f"|{a.model}|{sc}|{a.method}|{TAG}|{extra}".encode()).hexdigest()
    files, jobs, need = {}, [], {}   # need: cache key -> (padded PIL image to feed the model, pad)
    for e in ents:
        raw = M.blob(d, base, e)
        files[e["name"]] = raw
        got = load(raw, e["name"])
        if not got:
            continue
        kind, img = got
        if min(img.size) < a.min or img.width * img.height > a.max_px:
            continue
        if re.match(a.skip, e["name"], re.I):
            if a.grain:   # organic grit: plain resize plus synthesized grain instead of the model
                files[e["name"]] = grain(raw, e["name"], kind, img, a)
            continue
        km = key_mask(img) if kind == "bmp" else None
        has_key = km is not None and km.mean() > 0.002
        alpha = img.getchannel("A") if img.mode == "RGBA" else None
        colour_busy = detail(np.asarray(img.convert("RGB")), km if has_key else None) > 0.1
        alpha_busy = alpha is not None and detail(np.asarray(alpha)) > 0.1
        if not colour_busy and not alpha_busy:
            continue
        k = h(raw)
        cached = os.path.join(a.cache, k + "." + kind)
        if os.path.exists(cached):
            files[e["name"]] = open(cached, "rb").read()
            continue
        pad = min(16, img.width // 2, img.height // 2)
        job = dict(name=e["name"], k=k, kind=kind, img=img, pad=pad, km=km if has_key else None, ka=None)
        need[k] = (wrap_pad(fill_key(img, km) if has_key else img.convert("RGB"), pad), pad)
        if alpha_busy:
            job["ka"] = k + "a"
            need[job["ka"]] = (wrap_pad(Image.merge("RGB", (alpha, alpha, alpha)), pad), pad)
        jobs.append(job)
    results = {}   # key -> upscaled padded image
    if a.method == "lanczos":
        for k, (im, pad) in need.items():
            results[k] = im.resize((im.width * sc, im.height * sc), Image.LANCZOS)
    elif need:
        with tempfile.TemporaryDirectory() as t:
            ind, outd = os.path.join(t, "in"), os.path.join(t, "out")
            os.makedirs(ind), os.makedirs(outd)
            for k, (im, pad) in need.items():
                im.save(os.path.join(ind, k + ".png"))
            run_model(ind, outd, a)
            for k, (im, pad) in need.items():
                results[k] = Image.open(os.path.join(outd, k + ".png")).convert("RGB")
    for j in jobs:
        w, hh, pad = j["img"].width, j["img"].height, j["pad"]
        full = ((w + 2 * pad) * sc, (hh + 2 * pad) * sc)
        crop = (pad * sc, pad * sc, (pad + w) * sc, (pad + hh) * sc)

        def finish(im):
            im = im if im.size == full else im.resize(full, Image.LANCZOS)
            return im.crop(crop)
        up = finish(results[j["k"]])
        if j["img"].mode == "RGBA":
            al = finish(results[j["ka"]]).convert("L") if j["ka"] else j["img"].getchannel("A").resize((w * sc, hh * sc), Image.LANCZOS)
            up.putalpha(al)
        if j["km"] is not None:   # put the colour key back, exactly, wherever the original had it
            up.paste(KEY, mask=Image.fromarray((j["km"] * 255).astype(np.uint8)).resize((w * sc, hh * sc), Image.NEAREST))
        files[j["name"]] = encode(up, j["kind"])
        open(os.path.join(a.cache, j["k"] + "." + j["kind"]), "wb").write(files[j["name"]])
    with tempfile.TemporaryDirectory() as t:
        for name, raw in files.items():
            open(os.path.join(t, name), "wb").write(raw)
        tmp = os.path.join(os.path.dirname(t), "new-%d.mas" % os.getpid())
        sys.argv = ["mas", "pack", t, tmp, "--like", orig]
        M.main()
        shutil.move(tmp, path)
    return len(jobs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mas", nargs="+")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--method", default="ai", choices=["ai", "lanczos"])
    ap.add_argument("--skip", default=ORGANIC, help="regex of texture names to leave alone")
    ap.add_argument("--grain", type=float, default=0, metavar="STRENGTH", help="give the --skip (organic) textures a Lanczos resize plus synthesized grain of this strength (try 1.0) instead of leaving them alone")
    ap.add_argument("--min", type=int, default=32, help="skip textures smaller than this (either side)")
    ap.add_argument("--max-px", type=int, default=1 << 20, help="skip textures larger than this many pixels (1024x1024 by default)")
    ap.add_argument("--cache", default=os.path.join(ROOT, "work", "realesrgan", "cache"))
    a = ap.parse_args()
    for i, p in enumerate(a.mas, 1):
        print(f"[{i}/{len(a.mas)}] {p}: {process(p, a)} textures enhanced", flush=True)


if __name__ == "__main__":
    main()
