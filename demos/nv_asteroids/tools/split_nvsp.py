"""Split NVSP shader permutation blobs into individual DXIL containers and disassemble them.

NVSP layout (reversed from Asteroids.exe 0x1401ADB10 findPermutationInBlob / 0x1401AD8C0 enumerate):
  char magic[4] = "NVSP"
  repeated until end:
    u32 keyLength       length of permutation key string ("NAME=VALUE;NAME=VALUE;")
    u32 dataLength      length of the shader container
    u32 dataCrc         CRC32 of the shader container
    u32 keyCrc          CRC32 of the key string (built from the requested defines)
    u32 reserved
    char key[keyLength]
    u8   data[dataLength]
Files without the magic are a single shader container (the "<default>" permutation).
"""
import argparse
import binascii
import os
import struct
import subprocess
import sys

DXC = r"C:\Program Files\dxc\bin\x64\dxc.exe"


def parse_nvsp(blob):
    if blob[:4] != b"NVSP":
        return [("", blob)]
    out = []
    p = 4
    while len(blob) - p > 20:
        key_len, data_len, data_crc, key_crc, _ = struct.unpack_from("<5I", blob, p)
        if data_len == 0 or len(blob) - p < key_len + data_len + 20:
            break
        key = blob[p + 20:p + 20 + key_len].decode()
        data = blob[p + 20 + key_len:p + 20 + key_len + data_len]
        if binascii.crc32(data) != data_crc or binascii.crc32(key.encode()) != key_crc:
            raise ValueError(f"CRC mismatch for permutation {key!r}")
        out.append((key, data))
        p += 20 + key_len + data_len
    return out


def perm_name(key):
    return key.rstrip(";").replace(";", "__").replace("=", "-") or "default"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="directory with extracted shaders/** .bin files")
    ap.add_argument("out")
    args = ap.parse_args()
    for root, _, files in os.walk(args.src):
        for fn in files:
            if not fn.endswith(".bin"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, args.src)
            with open(path, "rb") as f:
                blob = f.read()
            dst_dir = os.path.join(args.out, os.path.splitext(rel)[0])
            os.makedirs(dst_dir, exist_ok=True)
            for key, data in parse_nvsp(blob):
                dst = os.path.join(dst_dir, perm_name(key) + ".dxil")
                with open(dst, "wb") as f:
                    f.write(data)
                r = subprocess.run([DXC, "-dumpbin", dst, "-Fc", dst + ".txt"], capture_output=True, text=True)
                status = "ok" if r.returncode == 0 else "DXC FAILED: " + r.stderr.strip()[:200]
                print(f"{rel} [{key or '<default>'}] {len(data)} {status}")


if __name__ == "__main__":
    sys.exit(main())
