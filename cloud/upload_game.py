#!/usr/bin/env python3
"""Upload your own copy of the game to a private R2 bucket (served by cloud/relay behind a token).

  python3 cloud/upload_game.py work/run_web [--bucket f1challenge-game] [--jobs 8]

Skips what the page never reads or what is yours alone: installer/redistributables/translated copies (as the page's
own SKIP list), logs, replays, your profile (Save/GMS) and leftovers. Writes manifest.json (path + size of every file) last.
"""
import argparse, json, os, re, subprocess, sys, tempfile, urllib.parse
from concurrent.futures import ThreadPoolExecutor

SKIP = re.compile(r"^(DirectX|Redist|Support|autorun|French|German|Italian|LOG|Replays|Save/GMS)/|^(0000000\d\.\w+|main\.dmp|.*\.zip)$|/\.DS_Store$|^\.", re.I)


def put(bucket, key, path):
    q = urllib.parse.quote(key, safe="/")
    for attempt in range(3):
        r = subprocess.run(["cf", "r2", "objects", "put", q, "--bucket-name", bucket, "--file", path,
                            "--content-type", "application/octet-stream"], capture_output=True, text=True)
        if r.returncode == 0:
            return None
        err = r.stderr.strip()[-300:]
    return f"{key}: {err}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--bucket", default="f1challenge-game")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()
    files = []
    for d, _, fs in os.walk(a.game):
        for f in fs:
            p = os.path.join(d, f)
            rel = os.path.relpath(p, a.game).replace(os.sep, "/")
            if not SKIP.search(rel):
                files.append((rel, p, os.path.getsize(p)))
    if not any(r.lower().endswith(".exe") for r, _, _ in files):
        sys.exit("no .exe in that folder")
    print(f"{len(files)} files, {sum(s for _, _, s in files) / 1e6:.0f} MB to {a.bucket}")
    failed, done = [], 0
    with ThreadPoolExecutor(a.jobs) as ex:
        for err in ex.map(lambda f: put(a.bucket, f[0], f[1]), files):
            done += 1
            if err:
                failed.append(err)
                print("FAILED", err, flush=True)
            elif done % 100 == 0:
                print(f"{done}/{len(files)}", flush=True)
    if failed:
        sys.exit(f"{len(failed)} uploads failed; run again to retry (nothing is published until manifest.json is written)")
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as m:
        json.dump([{"path": r, "size": s} for r, _, s in files], m)
    err = put(a.bucket, "manifest.json", m.name)
    os.unlink(m.name)
    sys.exit(err) if err else print("done")


if __name__ == "__main__":
    main()
