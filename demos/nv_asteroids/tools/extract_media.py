"""Extract every file from media.db into an output directory, preserving names and mtimes."""
import argparse
import os
import sys
import time

from mediadb import MediaDB

MAGICS = {
    ".png": [b"\x89PNG"],
    ".jpg": [b"\xff\xd8\xff"],
    ".dds": [b"DDS "],
    ".wav": [b"RIFF"],
    ".ttf": [b"\x00\x01\x00\x00", b"true", b"OTTO"],
    ".fbx": [b"Kaydara FBX Binary", b"; FBX"],
    ".chk": [b"NVDACHNK"],
    ".bin": [b"DXBC", b"NVSP"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db")
    ap.add_argument("out")
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    db = MediaDB(args.db)
    bad = 0
    t0 = time.time()
    for name, mtime, compressed, size in db.entries():
        if args.filter and args.filter not in name:
            continue
        dst = os.path.join(args.out, *name.split("/"))
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            continue
        data = db.read(name)
        ok = len(data) == size
        ext = os.path.splitext(name)[1].lower()
        if ext in MAGICS and not any(data.startswith(m) for m in MAGICS[ext]):
            ok = False
        if not ok:
            bad += 1
            print(f"WARN {name}: len={len(data)} expected={size} head={data[:8]!r}", file=sys.stderr)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        os.utime(dst, (mtime, mtime))
        print(f"{time.time() - t0:7.1f}s {name} ({size})", flush=True)
    print(f"done, {bad} warnings")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
