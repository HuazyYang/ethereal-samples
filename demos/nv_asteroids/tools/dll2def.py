"""Write a module-definition (.def) file listing the named exports of a PE DLL.

Used to build import libraries for the prebuilt third-party DLLs shipped with the
demo (PhysX 3.4 CHECKED, assimp, GFSDK_SSAO_D3D12) without their SDK .lib files.

usage: dll2def.py <input.dll> <output.def>
"""
import os
import struct
import sys


def exports(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, _ = struct.unpack_from("<II", data, dd)
    sections = []
    sec = opt + opt_size
    for i in range(nsec):
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        sections.append((va, max(vsize, raw_size), raw_ptr))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"rva {rva:#x} not mapped")

    if not exp_rva:
        return []
    e = off(exp_rva)
    nnames = struct.unpack_from("<I", data, e + 24)[0]
    names_rva = struct.unpack_from("<I", data, e + 32)[0]
    out = []
    for i in range(nnames):
        name_rva = struct.unpack_from("<I", data, off(names_rva) + 4 * i)[0]
        p = off(name_rva)
        out.append(data[p:data.index(b"\0", p)].decode())
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    names = exports(src)
    with open(dst, "w") as f:
        f.write(f"LIBRARY {os.path.basename(src)}\nEXPORTS\n")
        for n in names:
            f.write(f"    {n}\n")
    print(f"{len(names)} exports -> {dst}")


if __name__ == "__main__":
    main()
