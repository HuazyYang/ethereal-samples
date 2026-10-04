// VectorMath.h
//
// Small vector math library used by the context to fill the shader constant buffers.
// Column-vector convention: matrices store 4 columns, Transform(M, v) = M * v.
// The helpers mirror the (non-inlined, debug-built) functions of the original DLL; floating point
// operations are performed in the same order so results are bit-identical.

#pragma once

#include <Nv/VolumetricLighting/NvFoundationTypes.h>

#include <math.h>
#include <stdint.h>

namespace Nv
{
namespace VolumetricLighting
{

struct ZeroTag {};
struct IdentityTag {};

// NvVolumetricLighting.d3d11.dll: 0x1800010D0 (ctor), 0x180001110 (copy)
struct Vec2
{
    float x, y;

    Vec2() {}
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

// NvVolumetricLighting.d3d11.dll: 0x180001150 (ctor), 0x1800011A0 / 0x1800011F0 (copy), 0x180001EE0 (from NvcVec3)
struct Vec3
{
    float x, y, z;

    Vec3() {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3(const NvcVec3& v) : x(v.x), y(v.y), z(v.z) {}

    // NvVolumetricLighting.d3d11.dll: 0x1800013B0
    Vec3& operator+=(const Vec3& rhs)
    {
        x = x + rhs.x;
        y = y + rhs.y;
        z = z + rhs.z;
        return *this;
    }
};

// NvVolumetricLighting.d3d11.dll: 0x1800012B0
inline Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

// NvVolumetricLighting.d3d11.dll: 0x180001330
inline Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

// NvVolumetricLighting.d3d11.dll: 0x180001240
inline float LengthSq(const Vec3& v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// NvVolumetricLighting.d3d11.dll: 0x180001290 (calls the float sqrt overload 0x180001060 -> sqrtf)
inline float Length(const Vec3& v)
{
    return sqrtf(LengthSq(v));
}

// NvVolumetricLighting.d3d11.dll: 0x180001490 (ctor), 0x180001430 (zero ctor), 0x1800014F0 / 0x180001550 (copy)
struct Vec4
{
    float x, y, z, w;

    Vec4() {}
    explicit Vec4(ZeroTag) : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

    // NvVolumetricLighting.d3d11.dll: 0x1800015B0
    float& operator[](uint32_t i) { return (&x)[i]; }
    const float& operator[](uint32_t i) const { return (&x)[i]; }

    // NvVolumetricLighting.d3d11.dll: 0x180001780
    Vec3 xyz() const { return Vec3(x, y, z); }
};

// NvVolumetricLighting.d3d11.dll: 0x1800015D0
inline Vec4 operator+(const Vec4& a, const Vec4& b)
{
    return Vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}

// NvVolumetricLighting.d3d11.dll: 0x180001660
inline Vec4 operator*(const Vec4& a, float s)
{
    return Vec4(a.x * s, a.y * s, a.z * s, a.w * s);
}

// NvVolumetricLighting.d3d11.dll: 0x1800016E0
inline Vec4 operator/(const Vec4& a, float s)
{
    return Vec4(a.x * (1.0f / s), a.y * (1.0f / s), a.z * (1.0f / s), a.w * (1.0f / s));
}

// 4x4 matrix made of 4 column vectors.
// NvVolumetricLighting.d3d11.dll: 0x1800017C0 (default ctor), 0x180001820 (identity), 0x1800018E0 (zero),
// 0x180001950 (from columns), 0x1800019D0 (from 16 floats), 0x180001B80 / 0x180001C00 (copy),
// 0x180001F10 (from NvcMat44), 0x180001EC0 (column access), 0x180001D60 (element access)
struct Mat44
{
    Vec4 column[4];

    Mat44() {}

    explicit Mat44(IdentityTag)
    {
        column[0] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        column[1] = Vec4(0.0f, 1.0f, 0.0f, 0.0f);
        column[2] = Vec4(0.0f, 0.0f, 1.0f, 0.0f);
        column[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    explicit Mat44(ZeroTag)
    {
        column[0] = Vec4(ZeroTag());
        column[1] = Vec4(ZeroTag());
        column[2] = Vec4(ZeroTag());
        column[3] = Vec4(ZeroTag());
    }

    Mat44(const Vec4& c0, const Vec4& c1, const Vec4& c2, const Vec4& c3)
    {
        column[0] = c0;
        column[1] = c1;
        column[2] = c2;
        column[3] = c3;
    }

    explicit Mat44(const float* m)
    {
        column[0] = Vec4(m[0], m[1], m[2], m[3]);
        column[1] = Vec4(m[4], m[5], m[6], m[7]);
        column[2] = Vec4(m[8], m[9], m[10], m[11]);
        column[3] = Vec4(m[12], m[13], m[14], m[15]);
    }

    Mat44(const NvcMat44& m)
    {
        column[0] = Vec4(m.column0.x, m.column0.y, m.column0.z, m.column0.w);
        column[1] = Vec4(m.column1.x, m.column1.y, m.column1.z, m.column1.w);
        column[2] = Vec4(m.column2.x, m.column2.y, m.column2.z, m.column2.w);
        column[3] = Vec4(m.column3.x, m.column3.y, m.column3.z, m.column3.w);
    }

    Vec4& operator[](uint32_t c) { return column[c]; }
    const Vec4& operator[](uint32_t c) const { return column[c]; }

    // Element at (row, col).
    float& operator()(uint32_t row, uint32_t col) { return column[col][row]; }
    const float& operator()(uint32_t row, uint32_t col) const { return column[col][row]; }

    const float* data() const { return &column[0].x; }
};

// M * v. NvVolumetricLighting.d3d11.dll: 0x180001DA0
inline Vec4 Transform(const Mat44& m, const Vec4& v)
{
    return m.column[0] * v.x + m.column[1] * v.y + m.column[2] * v.z + m.column[3] * v.w;
}

// a * b. NvVolumetricLighting.d3d11.dll: 0x180001C80
inline Mat44 operator*(const Mat44& a, const Mat44& b)
{
    Vec4 c3 = Transform(a, b.column[3]);
    Vec4 c2 = Transform(a, b.column[2]);
    Vec4 c1 = Transform(a, b.column[1]);
    Vec4 c0 = Transform(a, b.column[0]);
    return Mat44(c0, c1, c2, c3);
}

// General 4x4 inverse (cofactor expansion); returns the zero matrix when singular.
// NvVolumetricLighting.d3d11.dll: 0x180001F40
Mat44 Inverse(const Mat44& matrix);

// Radical inverse of (index + 1) in the given base (Halton sequence).
// NvVolumetricLighting.d3d11.dll: 0x180004120
float Halton(int index, int base);

} // namespace VolumetricLighting
} // namespace Nv
