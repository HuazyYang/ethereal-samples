"""Convert the original 2018 NVSP shader blobs (assets/shaders/**.bin) to the ShaderMake blob format that the rebuilt
app loads, so that individual original shaders can replace the rebuilt ones (Asteroids.exe -shaderOverride DIR).

2018 entry (see split_nvsp.py): u32 keyLength, dataLength, dataCrc, keyCrc, reserved; key "NAME=VALUE;NAME=VALUE;"; data.
ShaderMake entry (external/donut/ShaderMake/ShaderMake/ShaderBlob.cpp): u32 permutationSize, dataSize; key "A=0 B=1"
with the names sorted; data.  Files that are a single container (no NVSP magic) are copied unchanged.

usage: nvsp2shadermake.py <assets/shaders> <out> [--only demo/gbuffer_ps ...]
"""
import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from split_nvsp import parse_nvsp  # noqa: E402


def convert_key(key):
    items = [kv for kv in key.split(";") if kv]
    pairs = sorted(kv.split("=", 1) for kv in items)
    return " ".join(f"{n}={v}" for n, v in pairs)


def convert(blob):
    if blob[:4] != b"NVSP":
        return blob
    out = bytearray(b"NVSP")
    for key, data in parse_nvsp(blob):
        k = convert_key(key).encode()
        out += struct.pack("<II", len(k), len(data)) + k + data
    return bytes(out)


NVAPI_MESH_SHADERS = {"asteroidMS_ms_main", "asteroidTS_ts_main", "basicMS_ms_main", "basicTS_ts_main",
                      "debugMS_ms_main", "debugTS_ts_main", "particles_ms_main"}


def app_path(rel):
    """Maps an original path (<group>/<name>.bin) to where the rebuilt app looks for the same shader."""
    group, name = rel.split("/", 1)
    stem = name[:-4]
    if group == "demo" and stem in NVAPI_MESH_SHADERS:
        return f"demo/nvapi/{name}"       # the 2018 task/mesh shaders are NVAPI vertex-stage DXIL
    if group == "framework" and stem.startswith("passes_"):
        return f"framework/passes/{stem[len('passes_'):]}.bin"
    return rel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*", help="only convert these <group>/<name> entries (no extension)")
    args = ap.parse_args()
    count = 0
    for root, _, files in os.walk(args.src):
        for fn in files:
            if not fn.endswith(".bin"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, args.src).replace("\\", "/")
            if args.only and os.path.splitext(rel)[0] not in args.only:
                continue
            dst = os.path.join(args.out, app_path(rel))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(path, "rb") as f:
                blob = f.read()
            with open(dst, "wb") as f:
                f.write(convert(blob))
            count += 1
    print(f"converted {count} blobs -> {args.out}")


if __name__ == "__main__":
    main()
