#include "Simulation_Util.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

struct XorShift128State {
    uint x;
    uint y;
    uint z;
    uint w;
};

static constexpr XorShift128State s_DefXorShift128State = {123456789u, 362436069u,
                                                           521288629u, 88675123u};

// DCA345EA

static XorShift128State s_XorShift128State;

void diagnostic_message(const wchar_t *fmt, ...)
{
#if defined(_WIN32)
    va_list measure;
    va_start(measure, fmt);
    const int length = _vscwprintf(fmt, measure);
    va_end(measure);
    if (length < 0)
        return;
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    va_list ap;
    va_start(ap, fmt);
    _vswprintf_p(buffer.data(), buffer.size(), fmt, ap);
    va_end(ap);
    // The archived import is ANSI even though its formatting buffer is wide.
    OutputDebugStringA(reinterpret_cast<const char*>(buffer.data()));
#else
    std::vector<wchar_t> buffer(256);
    while (true)
    {
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vswprintf(buffer.data(), buffer.size(), fmt, ap);
        va_end(ap);
        if (n >= 0 && static_cast<size_t>(n) < buffer.size())
            break;
        buffer.resize(n >= 0 ? static_cast<size_t>(n) + 1 : buffer.size() * 2);
    }
    std::fwprintf(stderr, L"%ls", buffer.data());
    std::fflush(stderr);
#endif
}

static float LegacyHalfTextureToFloat(uint16_t h) {
    // Preserve the original WaveWorks SIMD conversion. It treated the half
    // exponent as a biased normal exponent without special-casing zero,
    // subnormal, infinity, or NaN encodings. In particular, 0x0000 becomes
    // 2^-15; CPU readback compatibility depends on that behavior.
    const uint32_t sign = (uint32_t(h & 0x8000u) << 16u);
    const uint32_t exponent = (uint32_t((h >> 10u) & 0x1fu) + 112u) << 23u;
    const uint32_t mantissa = uint32_t(h & 0x03ffu) << 13u;
    const uint32_t bits = sign | exponent | mantissa;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static float getLane(const float4 &value, uint lane) {
    switch (lane) {
    case 0:
        return value.x;
    case 1:
        return value.y;
    case 2:
        return value.z;
    default:
        return value.w;
    }
}

static void setLane(float4 &value, uint lane, float laneValue) {
    switch (lane) {
    case 0:
        value.x = laneValue;
        break;
    case 1:
        value.y = laneValue;
        break;
    case 2:
        value.z = laneValue;
        break;
    default:
        value.w = laneValue;
        break;
    }
}

static void swapFloat4(float4 &a, float4 &b) {
    const float4 tmp = a;
    a = b;
    b = tmp;
}

uint32_t Simulation_Util::xor_shift_rand128(bool reset) {
    uint32_t x, y, z, w;
    if(reset) {
        x = s_DefXorShift128State.x;
        y = s_DefXorShift128State.y;
        z = s_DefXorShift128State.z;
        w = s_DefXorShift128State.w;
    } else {
        x = s_XorShift128State.x;
        y = s_XorShift128State.y;
        z = s_XorShift128State.z;
        w = s_XorShift128State.w;
    }
    s_XorShift128State.x = y;
    s_XorShift128State.y = z;
    s_XorShift128State.z = w;
    s_XorShift128State.w = x ^ (x << 11) ^ w ^ ((x ^ (x << 11) ^ (w >> 11)) >> 8);
    return s_XorShift128State.w;
}

float Simulation_Util::generate_uniform_01() {
    return (float)xor_shift_rand128(false) * 0x1p-32f;
}

void Simulation_Util::powerOf2(uint32_t floorVal, uint32_t *exponent, uint32_t *power) {
    uint32_t v = 1;
    uint32_t i = 0;
    for(; v < floorVal; ++i, v <<= 1);
    if(exponent)
        *exponent = i;
    if(power)
        *power = v;
}

void Simulation_Util::initGaussData(float2 *table) {

    const int dimension = 512;
    const int stride = 516;

    xor_shift_rand128(true);

    for (int i = 0; i <= dimension; ++i) {
        for(int j = 0; j <= dimension; ++j) {
            int idx = i * stride + j;

            table[idx].x = Gauss();
            table[idx].y = Gauss();
        }
    }
}

float Simulation_Util::Gauss() {
    uint x = s_XorShift128State.x ^ (s_XorShift128State.x << 11);
    s_XorShift128State.x = s_XorShift128State.z;
    uint w = x ^ s_XorShift128State.w ^ ((x ^ (s_XorShift128State.w >> 11)) >> 8);
    uint y = s_XorShift128State.y ^ (s_XorShift128State.y << 11);
    s_XorShift128State.y = s_XorShift128State.w;
    s_XorShift128State.z = w;
    s_XorShift128State.w = y ^ w ^ ((y ^ (w >> 11)) >> 8);

    const float radiusUniform =
        static_cast<float>(w) * 0x1p-32f;
    const float angleUniform =
        static_cast<float>(s_XorShift128State.w) * 0x1p-32f;
    return sqrtf(logf(fmaxf(1e-6f, radiusUniform)) * -2.0f) *
           cosf(angleUniform * TwoPif);
}

void Simulation_Util::initOmegaData(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade &cascade, float2 *samples) {

    int n = (int)cascade.textureSize;

    if (n >= 0) {
        const float deltaPhi = TwoPif / cascade.physicalSize;
        for (int i = 0; i <= n; ++i) {
            float y = float(i - n / 2) * deltaPhi;

            for (int j = 0; j <= n; ++j) {
                float x = float(j - n / 2) * deltaPhi;
                float r = sqrtf(x * x + y * y);

                int idx = i * (n + 4) + j;

                samples[idx].x = sqrtf(
                    r * r * r * 0.074000001f / 1000.0f +
                    r * 9.8100004f);
                samples[idx].y = atan2f(y, x);
            }
        }
    }
}

void Simulation_Util::FFTcSIMD(uint level, float4 *data, int sign) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:1626-1656.
    if (level == 0)
        return;

    const uint n = 1u << level;

    // Bit reversal permute the data in-place.
    uint j = 0;
    for (uint i = 0; i < n - 1; ++i) {
        if (i < j) {
            swapFloat4(data[2 * i], data[2 * j]);
            swapFloat4(data[2 * i + 1], data[2 * j + 1]);
        }

        uint bit = n >> 1;
        while (bit <= j) {
            j -= bit;
            bit >>= 1;
        }
        j += bit;
    }

    float wm_re = -1.0f;
    float wm_im = 0.0f;
    const float signFactor = sign == 1 ? -1.0f : 1.0f;

    uint span = 1;
    for (uint stage = 0; stage < level; ++stage) {
        uint step = span << 1;
        float w_re = 1.0f;
        float w_im = 0.0f;
        // span = 1 << stage;
        // step = 1 << (stage + 1);

        for (uint offset = 0; offset < span; ++offset) {
            for (uint i = offset; i < n; i += step) {
                float4 &a_re = data[2 * i];
                float4 &a_im = data[2 * i + 1];
                float4 &b_re = data[2 * (i + span)];
                float4 &b_im = data[2 * (i + span) + 1];

                const float4 old_a_re = a_re;
                const float4 old_a_im = a_im;
                const float4 old_b_re = b_re;
                const float4 old_b_im = b_im;

                const float4 t_re = old_b_re * w_re - old_b_im * w_im;
                const float4 t_im = old_b_re * w_im + old_b_im * w_re;

                b_re = old_a_re - t_re;
                b_im = old_a_im - t_im;
                a_re = old_a_re + t_re;
                a_im = old_a_im + t_im;

                // for (uint lane = 0; lane < 4; ++lane) {
                //     const float t_re =
                //         getLane(old_b_re, lane) * w_re - getLane(old_b_im, lane) * w_im;
                //     const float t_im =
                //         getLane(old_b_re, lane) * w_im + getLane(old_b_im, lane) * w_re;

                //     setLane(b_re, lane, getLane(old_a_re, lane) - t_re);
                //     setLane(b_im, lane, getLane(old_a_im, lane) - t_im);
                //     setLane(a_re, lane, getLane(old_a_re, lane) + t_re);
                //     setLane(a_im, lane, getLane(old_a_im, lane) + t_im);
                // }
            }

            const float prev_re = w_re;
            w_re = w_re * wm_re - w_im * wm_im;
            w_im = prev_re * wm_im + w_im * wm_re;
        }

        const float half_wm_re = 0.5f * wm_re;
        const float next_wm_re = sqrtf(half_wm_re + 0.5f);
        const float next_wm_im = sqrtf(0.5f - half_wm_re) * signFactor;

        wm_re = next_wm_re;
        wm_im = next_wm_im;
        span = step;
    }
}

void Simulation_Util::FFT1DSIMD_X_4wide(float2 *data, uint n, int sign) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:1231-1343.
    if (n <= 1)
        return;

    uint level = 0;
    uint length = 1;
    while (length < n) {
        length <<= 1;
        ++level;
    }
    // if (length != n)
    //     return;

    std::vector<float4> buffer(2 * n);

    float2 *row0 = data;
    float2 *row1 = data + n;
    float2 *row2 = data + 2 * n;
    float2 *row3 = data + 3 * n;

    for (uint col = 0; col < n; ++col) {
        buffer[2 * col] = make_float4(row0[col].x, row1[col].x, row2[col].x, row3[col].x);
        buffer[2 * col + 1] =
            make_float4(row0[col].y, row1[col].y, row2[col].y, row3[col].y);
    }

    FFTcSIMD(level, buffer.data(), sign);

    for (uint col = 0; col < n; ++col) {
        row0[col].x = buffer[2 * col].x;
        row1[col].x = buffer[2 * col].y;
        row2[col].x = buffer[2 * col].z;
        row3[col].x = buffer[2 * col].w;

        row0[col].y = buffer[2 * col + 1].x;
        row1[col].y = buffer[2 * col + 1].y;
        row2[col].y = buffer[2 * col + 1].z;
        row3[col].y = buffer[2 * col + 1].w;
    }
}

void Simulation_Util::FFT1DSIMD_Y_4wide(float2 *data, uint n, int sign) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:1471-1480.

    if (n <= 1)
        return;

    uint level = 0;
    uint length = 1;
    while (length < n) {
        length <<= 1;
        ++level;
    }
    // if (length != n)
    //     return;

    std::vector<float4> buffer(2 * n);
    uint outIndex = 0;
    for (uint row = 0; row < n; ++row) {
        const float2 *src = data + row * n;
        buffer[outIndex] = make_float4(src[0].x, src[1].x, src[2].x, src[3].x);
        buffer[outIndex + 1] = make_float4(src[0].y, src[1].y, src[2].y, src[3].y);
        outIndex += 2;
    }

    FFTcSIMD(level, buffer.data(), sign);

    for (uint row = 0; row < n; ++row) {
        float2 *dst = data + row * n;
        dst[0] = make_float2(buffer[2 * row].x, buffer[2 * row + 1].x);
        dst[1] = make_float2(buffer[2 * row].y, buffer[2 * row + 1].y);
        dst[2] = make_float2(buffer[2 * row].z, buffer[2 * row + 1].z);
        dst[3] = make_float2(buffer[2 * row].w, buffer[2 * row + 1].w);
    }
}

template <>
void Simulation_Util::add_displacements<Simulation_Util::Float32InputPolicy>(
    Wind_Waves_Detailed_Simulation_Parameters::Cascade const &cascade, const uint8_t *input,
    uint rowPitch, const float2 *positions, float4 *output, uint start, uint count,
    float scale) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:585-854.
    auto loadFloat4 = [](const uint8_t *data) -> float4 {
        float4 value;
        std::memcpy(&value, data, sizeof(value));
        return value;
    };

    auto wrapIndex = [](int32_t index, uint32_t size) -> uint {
        int32_t mod = index % int32_t(size);
        if (mod < 0)
            mod += int32_t(size);
        return uint32_t(mod);
    };

    auto wrapCoordinate = [](float v, float size) -> float {
        float r = fmodf(v, size);
        if (r < 0.0f)
            r += size;
        return r;
    };

    if (count == 0)
        return;

    const uint32_t textureSize = cascade.textureSize;
    if (textureSize == 0)
        return;

    const float textureSizeF = float(textureSize);
    const float physicalSize = cascade.physicalSize;
    const float warpAmplitude = cascade.warpAmplitude;
    const float warpFrequency = cascade.wrapFrequency;

    const float invPhysicalScale = textureSizeF / physicalSize;
    const float warpScale = textureSizeF * warpAmplitude;
    const float warpFrequencyNormalized = warpFrequency / physicalSize;

    std::vector<float2> uvs(count);

    if (warpScale == 0.0f) {
        for (uint i = 0; i < count; ++i) {
            const float2 &pos = positions[start + i];
            uvs[i].x = pos.x * invPhysicalScale;
            uvs[i].y = pos.y * invPhysicalScale;
        }
    } else {
        for (uint i = 0; i < count; ++i) {
            const float2 &pos = positions[start + i];
            const float scaledX = pos.x * invPhysicalScale;
            const float scaledY = pos.y * invPhysicalScale;
            const float phaseX = pos.x * warpFrequencyNormalized;
            const float phaseY = pos.y * warpFrequencyNormalized;

            uvs[i].x = scaledX + warpScale /* * 0.5f */ * cosf(phaseY);
            uvs[i].y = scaledY + warpScale /* * 0.5f */ * sinf(phaseX);
        }
    }

    for (uint i = 0; i < count; ++i) {
        const float2 &uv = uvs[i];

        const float uWrapped = wrapCoordinate(uv.x, textureSizeF);
        const float vWrapped = wrapCoordinate(uv.y, textureSizeF);

        const float uFloor = floorf(uWrapped);
        const float vFloor = floorf(vWrapped);
        const float uFrac = uWrapped - uFloor;
        const float vFrac = vWrapped - vFloor;

        const uint32_t x0 = wrapIndex(int32_t(uFloor), textureSize);
        const uint32_t y0 = wrapIndex(int32_t(vFloor), textureSize);
        const uint32_t x1 = (x0 + 1) % textureSize;
        const uint32_t y1 = (y0 + 1) % textureSize;

        const uint8_t *row0 = input + size_t(rowPitch) * y0;
        const uint8_t *row1 = input + size_t(rowPitch) * y1;

        const float4 c00 = loadFloat4(row0 + size_t(x0) * sizeof(float4));
        const float4 c10 = loadFloat4(row0 + size_t(x1) * sizeof(float4));
        const float4 c01 = loadFloat4(row1 + size_t(x0) * sizeof(float4));
        const float4 c11 = loadFloat4(row1 + size_t(x1) * sizeof(float4));

        const float invU = 1.0f - uFrac;
        const float invV = 1.0f - vFrac;

        float4 sample;
        sample.x =
            invV * (invU * c00.x + uFrac * c10.x) + vFrac * (invU * c01.x + uFrac * c11.x);
        sample.y =
            invV * (invU * c00.y + uFrac * c10.y) + vFrac * (invU * c01.y + uFrac * c11.y);
        sample.z =
            invV * (invU * c00.z + uFrac * c10.z) + vFrac * (invU * c01.z + uFrac * c11.z);
        sample.w =
            invV * (invU * c00.w + uFrac * c10.w) + vFrac * (invU * c01.w + uFrac * c11.w);

        float4 &dst = output[start + i];
        dst.x += sample.x * scale;
        dst.y += sample.y * scale;
        dst.z += sample.z * scale;
        dst.w += sample.w * scale;
    }
}

template<>
void Simulation_Util::add_displacements<Simulation_Util::Float16InputPolicy>(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade &cascade, const uint8_t *input,
    uint rowPitch, const float2 *positions, float4 *output, uint start,
    uint count, float scale) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:134-464.

    auto loadFloat4Half = [&](const uint8_t *data) -> float4 {
        const uint16_t *p = reinterpret_cast<const uint16_t *>(data);
        float4 v;
        v.x = LegacyHalfTextureToFloat(p[0]);
        v.y = LegacyHalfTextureToFloat(p[1]);
        v.z = LegacyHalfTextureToFloat(p[2]);
        v.w = LegacyHalfTextureToFloat(p[3]);
        return v;
    };

    auto wrapIndex = [](int32_t index, uint32_t size) -> uint {
        int32_t mod = index % int32_t(size);
        if (mod < 0) mod += int32_t(size);
        return uint32_t(mod);
    };

    auto wrapCoordinate = [](float v, float size) -> float {
        float r = fmodf(v, size);
        if (r < 0.0f) r += size;
        return r;
    };

    if (count == 0) return;
    const uint32_t textureSize = cascade.textureSize;
    if (textureSize == 0) return;

    const float textureSizeF = float(textureSize);
    const float physicalSize = cascade.physicalSize;
    const float warpAmplitude = cascade.warpAmplitude;
    const float warpFrequency = cascade.wrapFrequency;

    const float invPhysicalScale = textureSizeF / physicalSize;
    const float warpScale = textureSizeF * warpAmplitude;
    const float warpFrequencyNormalized = warpFrequency / physicalSize;

    std::vector<float2> uvs(count);

    if (warpScale == 0.0f) {
        for (uint i = 0; i < count; ++i) {
            const float2 &pos = positions[start + i];
            uvs[i].x = pos.x * invPhysicalScale;
            uvs[i].y = pos.y * invPhysicalScale;
        }
    } else {
        for (uint i = 0; i < count; ++i) {
            const float2 &pos = positions[start + i];
            const float scaledX = pos.x * invPhysicalScale;
            const float scaledY = pos.y * invPhysicalScale;
            const float phaseX = pos.x * warpFrequencyNormalized;
            const float phaseY = pos.y * warpFrequencyNormalized;
            uvs[i].x = scaledX + warpScale * cosf(phaseY);
            uvs[i].y = scaledY + warpScale * sinf(phaseX);
        }
    }

    const size_t pixelBytes = 4 * sizeof(uint16_t); // 4 components, 16-bit each

    for (uint i = 0; i < count; ++i) {
        const float2 &uv = uvs[i];
        const float uWrapped = wrapCoordinate(uv.x, textureSizeF);
        const float vWrapped = wrapCoordinate(uv.y, textureSizeF);

        const float uFloor = floorf(uWrapped);
        const float vFloor = floorf(vWrapped);
        const float uFrac = uWrapped - uFloor;
        const float vFrac = vWrapped - vFloor;

        const uint32_t x0 = wrapIndex(int32_t(uFloor), textureSize);
        const uint32_t y0 = wrapIndex(int32_t(vFloor), textureSize);
        const uint32_t x1 = (x0 + 1) % textureSize;
        const uint32_t y1 = (y0 + 1) % textureSize;

        const uint8_t *row0 = input + size_t(rowPitch) * y0;
        const uint8_t *row1 = input + size_t(rowPitch) * y1;

        const float4 c00 = loadFloat4Half(row0 + size_t(x0) * pixelBytes);
        const float4 c10 = loadFloat4Half(row0 + size_t(x1) * pixelBytes);
        const float4 c01 = loadFloat4Half(row1 + size_t(x0) * pixelBytes);
        const float4 c11 = loadFloat4Half(row1 + size_t(x1) * pixelBytes);

        const float invU = 1.0f - uFrac;
        const float invV = 1.0f - vFrac;

        float4 sample;
        sample.x = invV * (invU * c00.x + uFrac * c10.x) + vFrac * (invU * c01.x + uFrac * c11.x);
        sample.y = invV * (invU * c00.y + uFrac * c10.y) + vFrac * (invU * c01.y + uFrac * c11.y);
        sample.z = invV * (invU * c00.z + uFrac * c10.z) + vFrac * (invU * c01.z + uFrac * c11.z);
        sample.w = invV * (invU * c00.w + uFrac * c10.w) + vFrac * (invU * c01.w + uFrac * c11.w);

        float4 &dst = output[start + i];
        dst.x += sample.x * scale;
        dst.y += sample.y * scale;
        dst.z += sample.z * scale;
        dst.w += sample.w * scale;
    }
}

template <>
void Simulation_Util::add_local_displacements<Simulation_Util::Float32InputPolicy>(
    const Local_Waves_Detailed_Simulation_Parameters &params, const uint8_t *input,
    uint rowPitch, const float2 *positions, float4 *output, uint start,
    uint count) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:1090-1096.
    if (count == 0 || params.textureSize == 0)
        return;

    const uint32_t textureSize = params.textureSize;
    const float invPhysicalScale = float(textureSize) / params.physicalSize;
    const float halfTexture = float(textureSize) * 0.5f;
    const float maxIndex = float(textureSize - 1);

    auto loadFloat4 = [](const uint8_t *data) -> float4 {
        float4 value;
        std::memcpy(&value, data, sizeof(value));
        return value;
    };

    const float originX = params.offsetX;
    const float originY = params.offsetY;

    const float2 *pos = positions + start;
    float4 *dst = output + start;

    for (uint i = 0; i < count; ++i) {
        float x = (pos[i].x - originX) * invPhysicalScale - 0.5f + halfTexture;
        float y = (pos[i].y - originY) * invPhysicalScale - 0.5f + halfTexture;

        if (x <= 0.0f || y <= 0.0f || x >= maxIndex || y >= maxIndex) {
            dst[i] = make_float4(0.0f);
            continue;
        }

        int ix = int(x);
        int iy = int(y);

        const uint8_t *row0 = input + size_t(rowPitch) * iy;
        const uint8_t *row1 = input + size_t(rowPitch) * (iy + 1);

        const float4 c00 = loadFloat4(row0 + size_t(ix) * sizeof(float4));
        const float4 c10 = loadFloat4(row0 + size_t(ix + 1) * sizeof(float4));
        const float4 c01 = loadFloat4(row1 + size_t(ix) * sizeof(float4));
        const float4 c11 = loadFloat4(row1 + size_t(ix + 1) * sizeof(float4));

        const float u = x - float(ix);
        const float v = y - float(iy);
        const float invU = 1.0f - u;
        const float invV = 1.0f - v;

        float4 sample;
        sample.x = invV * (invU * c00.x + u * c10.x) + v * (invU * c01.x + u * c11.x);
        sample.y = invV * (invU * c00.y + u * c10.y) + v * (invU * c01.y + u * c11.y);
        sample.z = invV * (invU * c00.z + u * c10.z) + v * (invU * c01.z + u * c11.z);
        sample.w = invV * (invU * c00.w + u * c10.w) + v * (invU * c01.w + u * c11.w);

        float4 &dstPixel = dst[i];
        dstPixel.x += sample.x;
        dstPixel.y += sample.y;
        dstPixel.z += sample.z;
        dstPixel.w += sample.w;
    }
}

template <>
void Simulation_Util::add_local_displacements<Simulation_Util::Float16InputPolicy>(
    const Local_Waves_Detailed_Simulation_Parameters &params, const uint8_t *input,
    uint rowPitch, const float2 *positions, float4 *output, uint start, uint count) {
    // TODO: improve me using SIMD, see _Simulation_Util.cpp:933-1004.
    if (count == 0 || params.textureSize == 0)
        return;

    const uint32_t textureSize = params.textureSize;
    const float invPhysicalScale = float(textureSize) / params.physicalSize;
    const float halfTexture = float(textureSize) * 0.5f;
    const float maxIndex = float(textureSize - 1);

    auto loadFloat4Half = [&](const uint8_t *data) -> float4 {
        const uint16_t *p = reinterpret_cast<const uint16_t *>(data);
        float4 v;
        v.x = LegacyHalfTextureToFloat(p[0]);
        v.y = LegacyHalfTextureToFloat(p[1]);
        v.z = LegacyHalfTextureToFloat(p[2]);
        v.w = LegacyHalfTextureToFloat(p[3]);
        return v;
    };

    const float originX = params.offsetX;
    const float originY = params.offsetY;

    const float2 *pos = positions + start;
    float4 *dst = output + start;
    const size_t pixelBytes = 4 * sizeof(uint16_t);

    for (uint i = 0; i < count; ++i) {
        float x = (pos[i].x - originX) * invPhysicalScale - 0.5f + halfTexture;
        float y = (pos[i].y - originY) * invPhysicalScale - 0.5f + halfTexture;

        if (x <= 0.0f || y <= 0.0f || x >= maxIndex || y >= maxIndex) {
            dst[i] = make_float4(0.0f);
            continue;
        }

        int ix = int(x);
        int iy = int(y);

        const uint8_t *row0 = input + size_t(rowPitch) * iy;
        const uint8_t *row1 = input + size_t(rowPitch) * (iy + 1);

        const float4 c00 = loadFloat4Half(row0 + size_t(ix) * pixelBytes);
        const float4 c10 = loadFloat4Half(row0 + size_t(ix + 1) * pixelBytes);
        const float4 c01 = loadFloat4Half(row1 + size_t(ix) * pixelBytes);
        const float4 c11 = loadFloat4Half(row1 + size_t(ix + 1) * pixelBytes);

        const float u = x - float(ix);
        const float v = y - float(iy);
        const float invU = 1.0f - u;
        const float invV = 1.0f - v;

        float4 sample;
        sample.x = invV * (invU * c00.x + u * c10.x) + v * (invU * c01.x + u * c11.x);
        sample.y = invV * (invU * c00.y + u * c10.y) + v * (invU * c01.y + u * c11.y);
        sample.z = invV * (invU * c00.z + u * c10.z) + v * (invU * c01.z + u * c11.z);
        sample.w = invV * (invU * c00.w + u * c10.w) + v * (invU * c01.w + u * c11.w);

        float4 &dstPixel = dst[i];
        dstPixel.x += sample.x;
        dstPixel.y += sample.y;
        dstPixel.z += sample.z;
        dstPixel.w += sample.w;
    }
}

void Simulation_Util::addLocalDisplacements(
    const Local_Waves_Detailed_Simulation_Parameters &params, const uint8_t *input,
    uint rowPitch, const float2 *positions, float4 *output, uint start, uint count,
    bool fp16) {
    if (fp16)
        return add_local_displacements<Float16InputPolicy>(params, input, rowPitch,
                                                           positions, output, start, count);
    else
        return add_local_displacements<Float32InputPolicy>(params, input, rowPitch,
                                                           positions, output, start, count);
}

void Simulation_Util::addDisplacements(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade &cascade, const uint8_t *input,
    int rowPitch, const float2 *positions, float4 *output, uint start, uint count,
    float scale, bool fp16) {
    if (fp16)
        return add_displacements<Float16InputPolicy>(cascade, input, rowPitch, positions,
                                                     output, start, count, scale);
    else
        return add_displacements<Float32InputPolicy>(cascade, input, rowPitch, positions,
                                                     output, start, count, scale);
}
