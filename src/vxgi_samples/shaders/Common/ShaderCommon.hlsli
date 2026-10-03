#ifndef SHADERCOMMON_HLSLI
#define SHADERCOMMON_HLSLI
#include <donut/shaders/binding_helpers.hlsli>
#include "VXGIPreset.hlsli"

#pragma pack_matrix(row_major)

#define NONE 0
#define UNORM8 1
#define FLOAT16 2
#define FLOAT16_NVAPI 3
#define FLOAT32 4
#ifndef EMITTANCE_FORMAT
#define EMITTANCE_FORMAT UNORM8
#endif
#if EMITTANCE_FORMAT == FLOAT16
#define STORE_EMITTANCE(e) e
#define EMITTANCE_STORAGE_TYPE float4
#define LOAD_EMITTANCE_FROM_UAV(uav, address) uav[address]
#define STORE_EMITTANCE_TO_UAV(uav, address, value) (uav[address] = value)
#define SAMPLE_EMITTANCE(srv, sampler, address) srv.SampleLevel(sampler, address, 0)
#elif EMITTANCE_FORMAT == FLOAT16_NVAPI
#define STORE_EMITTANCE(e) e
#define EMITTANCE_STORAGE_TYPE float4
#define LOAD_EMITTANCE_FROM_UAV(uav, address) NvLoadUavTyped(uav, address)
#define STORE_EMITTANCE_TO_UAV(uav, address, value) (uav[address] = value)
#define SAMPLE_EMITTANCE(srv, sampler, address) srv.SampleLevel(sampler, address, 0)
#elif EMITTANCE_FORMAT == FLOAT32
#define STORE_EMITTANCE(e) e
#define EMITTANCE_STORAGE_TYPE uint3
#define LOAD_EMITTANCE_FROM_UAV(uav, address) uint3(uav##R[address].r, uav##G[address].r, uav##B[address].r)
#define STORE_EMITTANCE_TO_UAV(uav, address, value) \
    (uav##R[address] = value.rrrr);  \
    (uav##G[address] = value.gggg);  \
    (uav##B[address] = value.bbbb)
#define SAMPLE_EMITTANCE(srv, sampler, address) float4(srv##R.SampleLevel(sampler, address, 0).r, srv##G.SampleLevel(sampler, address, 0).r, srv##B.SampleLevel(sampler, address, 0).r, 0)
#else
#define STORE_EMITTANCE(e) e
#define EMITTANCE_STORAGE_TYPE uint
#define LOAD_EMITTANCE_FROM_UAV(uav, address) uav[address].x
#define STORE_EMITTANCE_TO_UAV(uav, address, value) (uav[address] = STORE_EMITTANCE(value))
#define SAMPLE_EMITTANCE(srv, sampler, address) srv.SampleLevel(sampler, address, 0)
#endif
#define AMAP_PRESENT_BIT 0x01
#define AMAP_EMISSIVE_BIT 0x02
#define AMAP_GEOMETRY_DIRTY_BIT 0x04
#define AMAP_LIGHTING_DIRTY_BIT 0x08
#define AMAP_DILATED_EMITTANCE_BIT 0x10
#define PAGE_DATA_VALID_BIT 0x80000000u
#define TOROIDAL_ADDRESS(localPos, offset, textureSize) ((localPos) + (offset)) & ((textureSize) - 1)
#define EMITTANCE_DIRECTIONS 6
#define EMITTANCE_FIXED_POINT_BITS 20
#define MARK_EMITTANCE_IN_OPACITY 1
#define MARK_OPACITY_MASK 0xC0000000u
#define INJECTION_FIXED_POINT_SCALE 65536
#if MARK_EMITTANCE_IN_OPACITY
#define MARK_OPACITY(_address_) (u_Opacity_Pos[_address_] = u_Opacity_Pos[_address_] | MARK_OPACITY_MASK)
#define CLEAR_OPACITY(_address_) (u_Opacity_Pos[_address_] = u_Opacity_Pos[_address_] & ~MARK_OPACITY_MASK)
#else
#define MARK_OPACITY(_address_)
#define CLEAR_OPACITY(_address_)
#endif
static const float VxgiPI = 3.14159265;
float2 float2scalar(float x) { return float2(x, x); }
float3 float3scalar(float x) { return float3(x, x, x); }
float4 float4scalar(float x) { return float4(x, x, x, x); }
int2 int2scalar(int x) { return int2(x, x); }
int3 int3scalar(int x) { return int3(x, x, x); }
int4 int4scalar(int x) { return int4(x, x, x, x); }
uint2 uint2scalar(uint x) { return uint2(x, x); }
uint3 uint3scalar(uint x) { return uint3(x, x, x); }
uint4 uint4scalar(uint x) { return uint4(x, x, x, x); }
struct VxgiFullScreenQuadOutput {
    float2 uv : TEXCOORD;
    float4 posProj : RAY;
    float instanceID : INSTANCEID;
};
float3 VxgiProjToRay(float4 posProj, float3 cameraPos, float4x4 viewProjMatrixInv)
{
    float4 farPoint = mul(posProj, viewProjMatrixInv);
    farPoint.xyz /= farPoint.w;
    return normalize(farPoint.xyz - cameraPos.xyz);
}
struct VxgiBox4i {
    int4 lower;
    int4 upper;
};
struct VxgiBox4f {
    float4 lower;
    float4 upper;
};
bool VxgiPartOfRegion(int3 coords, VxgiBox4i region)
{
    return coords.x <= region.upper.x && coords.y <= region.upper.y && coords.z <= region.upper.z &&
           coords.x >= region.lower.x && coords.y >= region.lower.y && coords.z >= region.lower.z;
}
bool VxgiPartOfRegionWithBorder(int3 coords, VxgiBox4i region, int border)
{
    return coords.x <= region.upper.x + border && coords.y <= region.upper.y + border && coords.z <= region.upper.z + border &&
           coords.x >= region.lower.x - border && coords.y >= region.lower.y - border && coords.z >= region.lower.z - border;
}
bool VxgiPartOfRegionf(float3 coords, float3 lower, float3 upper)
{
    return coords.x <= upper.x && coords.y <= upper.y && coords.z <= upper.z &&
           coords.x >= lower.x && coords.y >= lower.y && coords.z >= lower.z;
}
float VxgiToSRGB(float x)
{
    x = saturate(x);
    return x <= 0.0031308f ? x * 12.92f : saturate(1.055f * pow(x, 0.4166667f) - 0.055f);
}
float VxgiFromSRGB(float x)
{
    return x <= 0.04045f ? saturate(x * 0.0773994f) : pow(saturate(x * 0.9478672f + 0.0521327f), 2.4f);
}
EMITTANCE_STORAGE_TYPE VxgiPackEmittance(float4 v)
{
#if EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
    return EMITTANCE_STORAGE_TYPE(v);
#elif EMITTANCE_FORMAT == FLOAT32
    return asuint(v.rgb);
#else
    v.rgb = float3(VxgiToSRGB(v.r), VxgiToSRGB(v.g), VxgiToSRGB(v.b));
    uint result = uint(255.0f * v.x) | (uint(255.0f * v.y) << 8) | (uint(255.0f * v.z) << 16);
    return result;
#endif
}
float4 VxgiUnpackEmittance(EMITTANCE_STORAGE_TYPE n)
{
#if EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
    return float4(n.rgba);
#elif EMITTANCE_FORMAT == FLOAT32
    return float4(asfloat(n.rgb), 0);
#else
    float3 result;
    result.x = ((n >> 0) & 0xFF) / 255.0f;
    result.y = ((n >> 8) & 0xFF) / 255.0f;
    result.z = ((n >> 16) & 0xFF) / 255.0f;
    result = float3(VxgiFromSRGB(result.r), VxgiFromSRGB(result.g), VxgiFromSRGB(result.b));
    return float4(result, 0);
#endif
}
#if EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
uint2 VxgiPackEmittanceForAtomic(float4 v)
{
    uint4 parts = f32tof16(v);
    return uint2(parts.x | (parts.y << 16), parts.z | (parts.w << 16));
}
#else
uint3 VxgiPackEmittanceForAtomic(float3 v)
{
    float fixedPointScale = 1 << EMITTANCE_FIXED_POINT_BITS;
    return uint3(v.rgb * fixedPointScale);
}
#endif
uint VxgiPackOpacity(float3 opacity)
{
    return uint(1023 * opacity.x) | (uint(1023 * opacity.y) << 10) | (uint(1023 * opacity.z) << 20);
}
float4 VxgiUnpackOpacity(uint opacity)
{
    float4 fOpacity;
    fOpacity.x = float((opacity) & 0x3ff) / 1023.0;
    fOpacity.y = float((opacity >> 10) & 0x3ff) / 1023.0;
    fOpacity.z = float((opacity >> 20) & 0x3ff) / 1023.0;
    fOpacity.w = 0;
    return fOpacity;
}
float VxgiGetNormalProjection(float3 normal, uint direction)
{
    float normalProjection = 0;
    switch (direction) {
    case EMITTANCE_POSITIVE_X:
        normalProjection = saturate(normal.x);
        break;
    case EMITTANCE_NEGATIVE_X:
        normalProjection = saturate(-normal.x);
        break;
    case EMITTANCE_POSITIVE_Y:
        normalProjection = saturate(normal.y);
        break;
    case EMITTANCE_NEGATIVE_Y:
        normalProjection = saturate(-normal.y);
        break;
    case EMITTANCE_POSITIVE_Z:
        normalProjection = saturate(normal.z);
        break;
    case EMITTANCE_NEGATIVE_Z:
        normalProjection = saturate(-normal.z);
        break;
    }
    return normalProjection;
}
float VxgiAverage4(float a, float b, float c, float d)
{
    return (a + b + c + d) / 4;
}
float VxgiMultiplyComplements(float a, float b)
{
    return 1 - (1 - a) * (1 - b);
}
bool VxgiIsOdd(int x)
{
    return (x & 1) != 0;
}
#endif /* SHADERCOMMON_HLSLI */
