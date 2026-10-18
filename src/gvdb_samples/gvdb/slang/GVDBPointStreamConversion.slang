// Raw point conversion; one invocation owns one output word. SPDX-License-Identifier: Apache-2.0
struct Parameters {
    uint4 source, destination, work;
    float4 minimum, delta, scale, translation;
};
ConstantBuffer<Parameters> g_Parameters : register(b0);
ByteAddressBuffer g_Source : register(t0);
RWByteAddressBuffer g_Destination : register(u0);

uint readByte(uint address) { return (g_Source.Load(address & ~3u) >> ((address & 3u) * 8u)) & 255u; }
uint readWord(uint address) {
    uint value = readByte(address) | (readByte(address + 1u) << 8);
    return value | (readByte(address + 2u) << 16) | (readByte(address + 3u) << 24);
}
// IEEE binary64 -> binary32 round-to-nearest-even, using only SM5 uint arithmetic.
uint decodeDoubleBits(uint2 bits) {
    uint sign = bits.y & 0x80000000u, exponent = (bits.y >> 20) & 2047u, high = bits.y & 0xfffffu;
    if (exponent == 2047u)
        return sign | ((high != 0 || bits.x != 0) ? 0x7fc00000u : 0x7f800000u);
    if (exponent < 873u)
        return sign;
    if (exponent > 1150u)
        return sign | 0x7f800000u;
    high |= 0x100000u;
    if (exponent >= 897u) {
        uint mantissa = (high << 3) | (bits.x >> 29), remainder = bits.x & 0x1fffffffu;
        uint increment = uint(remainder > 0x10000000u || (remainder == 0x10000000u && (mantissa & 1u) != 0));
        return sign | (((exponent - 897u) << 23) + mantissa + increment);
    }
    uint shift = 926u - exponent, mantissa = 0;
    bool increment = false;
    if (shift < 32u) {
        mantissa = (high << (32u - shift)) | (bits.x >> shift);
        uint remainder = bits.x & ((1u << shift) - 1u), half = 1u << (shift - 1u);
        increment = remainder > half || (remainder == half && (mantissa & 1u) != 0);
    } else if (shift == 32u) {
        mantissa = high;
        increment = bits.x > 0x80000000u || (bits.x == 0x80000000u && (mantissa & 1u) != 0);
    } else {
        uint upperShift = shift - 32u;
        mantissa = high >> upperShift;
        uint remainder = high & ((1u << upperShift) - 1u), half = 1u << (upperShift - 1u);
        increment = remainder > half || (remainder == half && (bits.x != 0 || (mantissa & 1u) != 0));
    }
    return sign | (mantissa + uint(increment));
}
uint sourceBits(uint address, uint bytes) {
    if (bytes == 1u)
        return asuint(float(readByte(address)));
    if (bytes == 2u)
        return asuint(float(readByte(address) | (readByte(address + 1u) << 8)));
    if (bytes == 4u)
        return readWord(address);
    return decodeDoubleBits(uint2(readWord(address), readWord(address + 4u)));
}
uint2 widenFloatBits(uint bits) {
    uint sign = bits & 0x80000000u;
    uint exponent = (bits >> 23) & 255u, mantissa = bits & 0x7fffffu;
    if (exponent == 255u)
        return uint2(0, sign | (mantissa != 0 ? 0x7ff80000u : 0x7ff00000u));
    if (exponent == 0u) {
        if (mantissa == 0u)
            return uint2(0, sign);
        uint top = uint(firstbithigh(mantissa));
        uint remainder = mantissa ^ (1u << top);
        uint low = 0, high = 0;
        uint shift = 52u - top;
        if (shift >= 32u)
            high = remainder << (shift - 32u);
        else {
            low = remainder << shift;
            high = remainder >> (32u - shift);
        }
        return uint2(low, sign | ((top + 874u) << 20) | high);
    }
    return uint2(mantissa << 29, sign | ((exponent + 896u) << 20) | (mantissa >> 3));
}
float multiplyRounded(float a, float b) {
    __target_switch {
        case cuda:
            __intrinsic_asm "__fmul_rn($0, $1)";
        default:
            precise float result = a * b;
            return result;
    }
}
float addRounded(float a, float b) {
    __target_switch {
        case cuda:
            __intrinsic_asm "__fadd_rn($0, $1)";
        default:
            precise float result = a + b;
            return result;
    }
}
uint convertedBits(uint point, uint component) {
    uint address = g_Parameters.source.z + point * g_Parameters.source.w + component * g_Parameters.source.y;
    uint bits = sourceBits(address, g_Parameters.source.y), magnitude = bits & 0x7fffffffu;
    // Explicit exceptional classification keeps integer conversion deterministic
    // under FXC optimization. Parameters are finite, checked on the CPU.
    if (magnitude > 0x7f800000u)
        return 0x7fc00000u;
    if (magnitude == 0x7f800000u) {
        float delta = g_Parameters.delta[component], scale = g_Parameters.scale[component];
        if (delta == 0 || scale == 0)
            return 0x7fc00000u;
        return 0x7f800000u | ((bits ^ asuint(delta) ^ asuint(scale)) & 0x80000000u);
    }
    if (g_Parameters.minimum[component] == 0 && g_Parameters.delta[component] == 1 &&
        g_Parameters.scale[component] == 1 && g_Parameters.translation[component] == 0)
        return bits;
    float scaledInput = multiplyRounded(asfloat(bits), g_Parameters.delta[component]);
    float shifted = addRounded(g_Parameters.minimum[component], scaledInput);
    float scaled = multiplyRounded(shifted, g_Parameters.scale[component]);
    float result = addRounded(scaled, g_Parameters.translation[component]);
    return asuint(result);
}
uint outputByte(uint point, uint component, uint byteIndex) {
    uint bits = convertedBits(point, component);
    uint bytes = g_Parameters.destination.y, packed = 0;
    if (bytes <= 2u) {
        if ((bits & 0x7fffffffu) < 0x7f800000u)
            packed = uint(clamp(asfloat(bits), 0.0, bytes == 1u ? 255.0 : 65535.0));
    } else if (bytes == 4u)
        packed = bits;
    else {
        uint2 wide = widenFloatBits(bits);
        packed = byteIndex < 4u ? wide.x : wide.y;
        byteIndex &= 3u;
    }
    return (packed >> (byteIndex * 8u)) & 255u;
}
[shader("compute")][numthreads(256, 1, 1)] void convertPointStream(uint3 id
                                                                   : SV_DispatchThreadID) {
    uint index = id.x + id.y * (65535u * 256u);
    if (index >= g_Parameters.work.y)
        return;
    uint address = (g_Parameters.work.x + index) * 4u;
    uint value = g_Destination.Load(address);
    [unroll] for (uint lane = 0; lane < 4; ++lane) {
        uint at = address + lane;
        if (at >= g_Parameters.destination.z) {
            uint relative = at - g_Parameters.destination.z;
            uint point = relative / g_Parameters.destination.w, local = relative % g_Parameters.destination.w;
            if (point < g_Parameters.source.x &&
                local < g_Parameters.destination.x * g_Parameters.destination.y) {
                uint component = local / g_Parameters.destination.y,
                     byteIndex = local % g_Parameters.destination.y;
                uint byteValue = outputByte(point, component, byteIndex);
                value = (value & ~(255u << (lane * 8u))) | (byteValue << (lane * 8u));
            }
        }
    }
    g_Destination.Store(address, value);
}
