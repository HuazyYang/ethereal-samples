# SPDX-License-Identifier: Apache-2.0
"""Regenerate fixed binary32 division goldens using an independent binary64 oracle.

Only the Python standard library is required. Operands are finite binary32, with
positive nonzero denominators. Binary64 has enough precision to distinguish the
binary32 rounding boundaries for ratios of these 24-bit significands.
"""
from pathlib import Path
import random
import struct


def value(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def expected(numerator, denominator):
    quotient = value(numerator) / value(denominator)
    try:
        return struct.unpack("<I", struct.pack("<f", quotient))[0]
    except OverflowError:
        return 0xFF800000 if quotient < 0 else 0x7F800000


def main():
    random_source = random.Random(19835)
    pairs = [(random_source.randrange(0x7F800000) | (random_source.randrange(2) << 31),
              random_source.randrange(1, 0x7F800000)) for _ in range(4096)]
    pairs += [(n, d) for n in (0, 0x80000000, 1, 0x7FFFFF, 0x800000, 0x3F800000, 0x7F7FFFFF)
              for d in (1, 0x800000, 0x3F800000, 0x40000000, 0x7F7FFFFF)]
    header = ("// SPDX-License-Identifier: Apache-2.0\n"
              "// Seed 19835; Python binary64 quotient rounded to binary32. Includes signed zero/subnormal/overflow.\n"
              "#pragma once\n#include <cstdint>\ninline constexpr uint32_t voxelDivisionGoldens[][3]={\n")
    header += "".join("{0x%08xu,0x%08xu,0x%08xu},\n" % (n, d, expected(n, d)) for n, d in pairs)
    header += "};\n"
    output = Path(__file__).with_name("VoxelizeDivisionGoldens.h")
    encoded = header.encode("utf-8")
    if not output.exists() or output.read_bytes() != encoded:
        output.write_bytes(encoded)
    print(f"Verified {len(pairs)} deterministic division goldens")


if __name__ == "__main__":
    main()
