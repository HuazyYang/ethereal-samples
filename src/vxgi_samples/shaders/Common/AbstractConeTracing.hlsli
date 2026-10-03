#include "ShaderCommon.hlsli"
#include "AbstractConeTracingConstants.hlsli"

SamplerState s_VoxelTextureSampler : REGISTER_SAMPLER(VXGI_VOXELTEX_SAMPLER_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_OpacityMap_Pos : REGISTER_SRV(VXGI_OPACITY_POS_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_OpacityMap_Neg : REGISTER_SRV(VXGI_OPACITY_NEG_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
#if EMITTANCE_FORMAT == FLOAT32
Texture3D<float4> t_EmittanceEvenR : REGISTER_SRV(VXGI_EMITTANCE_EVEN_R_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_EmittanceEvenG : REGISTER_SRV(VXGI_EMITTANCE_EVEN_G_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_EmittanceEvenB : REGISTER_SRV(VXGI_EMITTANCE_EVEN_B_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_EmittanceOddR  : REGISTER_SRV(VXGI_EMITTANCE_ODD_R_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_EmittanceOddG  : REGISTER_SRV(VXGI_EMITTANCE_ODD_G_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_EmittanceOddB  : REGISTER_SRV(VXGI_EMITTANCE_ODD_B_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
#elif EMITTANCE_FORMAT == UNORM8
Texture3D<float4> t_EmittanceEven : REGISTER_SRV(VXGI_EMITTANCE_EVEN_R_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
Texture3D<float4> t_EmittanceOdd :  REGISTER_SRV(VXGI_EMITTANCE_ODD_R_SRV_SLOT, VXGI_ACT_RESOURCE_SPACE);
#endif
struct VxgiAbstractTracingConstants {
    float4 rOpacityTextureSize;
    float4 rEmittanceTextureSize;
    float4 ClipmapAnchor;
    float4 SceneBoundaryLower;
    float4 SceneBoundaryUpper;
    float4 ClipmapCenter;
    float4 TracingToroidalOffset;
    float EmittancePackingStride;
    float FinestVoxelSize;
    float StackTextureSize;
    float rNearestLevel0Boundary;
    float MaxMipmapLevel;
    float rEmittanceStorageScale;
    float rClipmapSizeWorld;
    uint Use6DOpacity;
};
cbuffer AbstractTracingCB : REGISTER_CBUFFER(VXGI_CONE_TRACING_CB_SLOT, VXGI_ACT_RESOURCE_SPACE)
{
    VxgiAbstractTracingConstants g_VxgiAbstractTracingCB;
};
cbuffer TranslationCB : REGISTER_CBUFFER(VXGI_CONE_TRACING_TRANSLATION_CB_SLOT, VXGI_ACT_RESOURCE_SPACE)
{
    float4 g_VxgiTranslationParameters[MAX_TOTAL_LEVELS];
    float4 g_VxgiTranslationParameters2[MAX_TOTAL_LEVELS];
};
static const int g_VxgiPoissonDiskSize = 16;
static const float2 g_VxgiPoissonDisk[] = {
    float2(-0.7593753f, 0.518795f),
    float2(0.5322764f, 0.2350069f),
    float2(0.8114883f, -0.458026f),
    float2(-0.3093514f, -0.749256f),
    float2(0.2293134f, 0.7607011f),
    float2(0.08265103f, -0.8939569f),
    float2(0.09813362f, 0.192451f),
    float2(-0.3114384f, -0.3017288f),
    float2(0.6505286f, 0.6297367f),
    float2(-0.3022015f, 0.297664f),
    float2(-0.7386893f, -0.5215692f),
    float2(-0.3935238f, 0.7530643f),
    float2(-0.6928226f, 0.07119545f),
    float2(0.8581018f, -0.01624052f),
    float2(0.3988827f, -0.617012f),
    float2(0.2837671f, -0.179743f)};
float VxgiSqr(float x) { return x * x; }
float VxgiGetDistanceFromAnchor(float3 position)
{
    float3 centerOffset = abs(position - g_VxgiAbstractTracingCB.ClipmapAnchor.xyz);
    return max(centerOffset.x, max(centerOffset.y, centerOffset.z));
}
float VxgiGetMinSampleSizeInternal(float distanceFromAnchor)
{
    return max(2 * distanceFromAnchor * g_VxgiAbstractTracingCB.rNearestLevel0Boundary, 1);
}
float VxgiGetMinSampleSizeInVoxels(float3 position)
{
    return VxgiGetMinSampleSizeInternal(VxgiGetDistanceFromAnchor(position));
}
float VxgiGetFinestVoxelSize()
{
    return g_VxgiAbstractTracingCB.FinestVoxelSize;
}
float VxgiProjectDirectionalOpacities(float3 opacity, float3 normal)
{
    return saturate(saturate(opacity.x * normal.x) + saturate(opacity.y * normal.y) + saturate(opacity.z * normal.z));
}
void VxgiGetLevelCoordinates(float3 position, float level, out float3 opacityCoords, out float3 emittanceCoords)
{
    float4 translationParams = g_VxgiTranslationParameters[int(level)];
    float4 translationParams2 = g_VxgiTranslationParameters2[int(level)];
    float3 positionInClipmap = (position - g_VxgiAbstractTracingCB.ClipmapCenter.xyz) * translationParams.x + 0.5;
    float3 fVoxelCoord = frac(positionInClipmap + translationParams2.xyz);
    float3 iVoxelCoord = fVoxelCoord * translationParams.y;
    opacityCoords = (iVoxelCoord + float3(0, 0, translationParams.z)) * g_VxgiAbstractTracingCB.rOpacityTextureSize.xyz;
    emittanceCoords = (iVoxelCoord + float3(1, 0, translationParams.w)) * g_VxgiAbstractTracingCB.rEmittanceTextureSize.xyz;
}
float VxgiSampleOpacityTextures(float3 coords, float3 direction, bool flipOpacityDirections, out bool sampleEmittance)
{
    float opacity;
    float4 pos = t_OpacityMap_Pos.SampleLevel(s_VoxelTextureSampler, coords, 0);
    if (bool(g_VxgiAbstractTracingCB.Use6DOpacity)) {
        float4 neg = t_OpacityMap_Neg.SampleLevel(s_VoxelTextureSampler, coords, 0);
        if (flipOpacityDirections) {
            direction = -direction;
        }
        opacity = VxgiProjectDirectionalOpacities(pos.xyz, -direction) + VxgiProjectDirectionalOpacities(neg.xyz, direction);
    } else {
        opacity = VxgiProjectDirectionalOpacities(pos.xyz, abs(direction));
    }
#if EMITTANCE_FORMAT == NONE
    sampleEmittance = false;
#elif MARK_EMITTANCE_IN_OPACITY
    sampleEmittance = pos.w != 0 ? true : false;
#else
    sampleEmittance = true;
#endif
    return opacity;
}
#if EMITTANCE_FORMAT != NONE
float3 VxgiSampleEmittanceTextures(float3 coords, float3 direction, bool VxgiIsOdd)
{
    float3 emittanceX, emittanceY, emittanceZ;
    float offsetX = direction.x > 0 ? EMITTANCE_NEGATIVE_X : EMITTANCE_POSITIVE_X;
    float offsetY = direction.y > 0 ? EMITTANCE_NEGATIVE_Y : EMITTANCE_POSITIVE_Y;
    float offsetZ = direction.z > 0 ? EMITTANCE_NEGATIVE_Z : EMITTANCE_POSITIVE_Z;
    float3 coordsX = coords + float3(offsetX * g_VxgiAbstractTracingCB.EmittancePackingStride, 0, 0);
    float3 coordsY = coords + float3(offsetY * g_VxgiAbstractTracingCB.EmittancePackingStride, 0, 0);
    float3 coordsZ = coords + float3(offsetZ * g_VxgiAbstractTracingCB.EmittancePackingStride, 0, 0);
    if (VxgiIsOdd) {
        emittanceX = SAMPLE_EMITTANCE(t_EmittanceOdd, s_VoxelTextureSampler, coordsX).rgb;
        emittanceY = SAMPLE_EMITTANCE(t_EmittanceOdd, s_VoxelTextureSampler, coordsY).rgb;
        emittanceZ = SAMPLE_EMITTANCE(t_EmittanceOdd, s_VoxelTextureSampler, coordsZ).rgb;
    } else {
        emittanceX = SAMPLE_EMITTANCE(t_EmittanceEven, s_VoxelTextureSampler, coordsX).rgb;
        emittanceY = SAMPLE_EMITTANCE(t_EmittanceEven, s_VoxelTextureSampler, coordsY).rgb;
        emittanceZ = SAMPLE_EMITTANCE(t_EmittanceEven, s_VoxelTextureSampler, coordsZ).rgb;
    }
    return abs(direction.x) * emittanceX + abs(direction.y) * emittanceY + abs(direction.z) * emittanceZ;
}
#endif
void VxgiCalculateSampleParameters(float t, float coneFactor, float tracingStep, float minSampleSize, out float tStep, out float fLevel, out float sampleSize)
{
    sampleSize = t * coneFactor;
    bool isClamped = sampleSize < minSampleSize;
    sampleSize = isClamped ? minSampleSize : sampleSize;
    if (isClamped)
        tStep = max((2 * t + minSampleSize) / (2 - coneFactor), t + minSampleSize) - t;
    else
        tStep = t * ((2 + coneFactor) / (2 - coneFactor) - 1);
    tStep *= tracingStep;
    fLevel = log2(sampleSize);
}
#define EXCHANGE(TYPE, A, B) \
    {                        \
        TYPE temp = A;       \
        A = B;               \
        B = temp;            \
    }
void VxgiSampleVoxelData(float3 curPosition, float fLevel, float3 direction, float weight, bool flipOpacityDirections, out float opacity, out float3 emittance, out bool anyEmittance)
{
    bool sampleEmittance1, sampleEmittance2;
    float iLevel = floor(fLevel);
    float fracLevel = fLevel - iLevel;
    float weightLow = (1.0 - fracLevel) * weight;
    float weightHigh = (fLevel > g_VxgiAbstractTracingCB.MaxMipmapLevel) ? 0 : fracLevel * weight;
    float3 opacityCoords1, opacityCoords2;
    float3 emittanceCoords1, emittanceCoords2;
    VxgiGetLevelCoordinates(curPosition, iLevel, opacityCoords1, emittanceCoords1);
    VxgiGetLevelCoordinates(curPosition, iLevel + 1, opacityCoords2, emittanceCoords2);
    opacity =
        VxgiSampleOpacityTextures(opacityCoords1.xyz, direction, flipOpacityDirections, sampleEmittance1) * weightLow +
        VxgiSampleOpacityTextures(opacityCoords2.xyz, direction, flipOpacityDirections, sampleEmittance2) * weightHigh;
    emittance = float3(0, 0, 0);
    anyEmittance = false;
#if EMITTANCE_FORMAT != NONE
    float factorLow = pow(4, iLevel) * VxgiSqr(g_VxgiAbstractTracingCB.FinestVoxelSize);
    float factorHigh = factorLow * 4;
    factorLow *= weightLow;
    factorHigh *= weightHigh;
    if ((int(iLevel) & 1) != 0) {
        EXCHANGE(float, factorHigh, factorLow);
        EXCHANGE(float3, emittanceCoords1, emittanceCoords2);
        EXCHANGE(bool, sampleEmittance1, sampleEmittance2);
    }
    if (sampleEmittance1) {
        emittance = VxgiSampleEmittanceTextures(emittanceCoords1.xyz, direction, false) * factorLow;
        anyEmittance = true;
    }
    if (sampleEmittance2 && factorHigh > 0) {
        emittance += VxgiSampleEmittanceTextures(emittanceCoords2.xyz, direction, true) * factorHigh;
        anyEmittance = true;
    }
#endif
}
void VxgiGetTangentAndCotangent(float3 normal, out float3 tangent, out float3 cotangent)
{
    float3 absNormal = abs(normal);
    float maxComp = max(absNormal.x, max(absNormal.y, absNormal.z));
    if (maxComp == absNormal.x)
        tangent = float3((-normal.y - normal.z) * sign(normal.x), absNormal.x, absNormal.x);
    else if (maxComp == absNormal.y)
        tangent = float3(absNormal.y, (-normal.x - normal.z) * sign(normal.y), absNormal.y);
    else
        tangent = float3(absNormal.z, absNormal.z, (-normal.x - normal.y) * sign(normal.z));
    tangent = normalize(tangent);
    cotangent = cross(tangent, normal);
}
struct VxgiConeTracingArguments {
    float3 firstSamplePosition;
    float3 direction;
    float coneFactor;
    float tracingStep;
    float firstSampleT;
    float maxTracingDistance;
    float opacityCorrectionFactor;
    float emittanceScale;
    float initialOpacity;
    float ambientAttenuationFactor;
    float maxSamples;
    float randomSeed;
    float tangentJitterScale;
    bool enableSceneBoundsCheck;
    bool flipOpacityDirections;
    float4 monitoringUserData;
};
VxgiConeTracingArguments VxgiDefaultConeTracingArguments()
{
    VxgiConeTracingArguments args;
    args.firstSamplePosition = float3(0, 0, 0);
    args.direction = float3(1, 0, 0);
    args.coneFactor = 1.0;
    args.tracingStep = 1.0;
    args.firstSampleT = 1.0;
    args.maxTracingDistance = 0.0;
    args.opacityCorrectionFactor = 1.0;
    args.emittanceScale = 1.0;
    args.initialOpacity = 0.0;
    args.ambientAttenuationFactor = 0.0;
    args.maxSamples = 128;
    args.enableSceneBoundsCheck = true;
    args.flipOpacityDirections = false;
    args.randomSeed = 0;
    args.tangentJitterScale = 0;
    args.monitoringUserData = float4scalar(0);
    return args;
}
struct VxgiConeTracingResults {
    float3 irradiance;
    float ambient;
    float finalOpacity;
    float sampleCount;
};
void VxgiOnBeginCone(float3 worldPos, float3 direction, float4 userData);
void VxgiOnConeSample(float t, float3 worldPos, float3 direction, float fLevel, float sampleIndex, float transparency, float3 emittance, float4 userData);
#ifndef VXGI_MONITOR_CONE_TRACING
void VxgiOnBeginCone(float3 worldPos, float3 direction, float4 userData) {}
void VxgiOnConeSample(float t, float3 worldPos, float3 direction, float fLevel, float sampleIndex, float transparency, float3 emittance, float4 userData) {}
#endif
VxgiConeTracingResults VxgiTraceCone(VxgiConeTracingArguments args)
{
    float initialTransparency = 1 - args.initialOpacity;
    float transparency = initialTransparency, pTransparency = initialTransparency, ppTransparency = initialTransparency;
    float3 coneIrradiance = float3(0, 0, 0);
    float coneAmbient = 0;
    float prevAmbientFactor = 1;
    float3 curPosition = args.firstSamplePosition;
    float t = args.firstSampleT;
    float3 direction = args.direction;
    float emittanceScale = args.emittanceScale * g_VxgiAbstractTracingCB.rEmittanceStorageScale;
    float3 tangent, cotangent;
    VxgiGetTangentAndCotangent(direction, tangent, cotangent);
    int poissonOffset = int(frac(args.randomSeed) * g_VxgiPoissonDiskSize);
    VxgiOnBeginCone(curPosition, direction, args.monitoringUserData);
    float sampleIndex = 0;
    float maxT = args.maxTracingDistance / g_VxgiAbstractTracingCB.FinestVoxelSize;
    while (sampleIndex < args.maxSamples)
    {
        float distanceFromAnchor = VxgiGetDistanceFromAnchor(curPosition);
        float distanceToBoundary = g_VxgiAbstractTracingCB.ClipmapAnchor.w - distanceFromAnchor;
        float minSampleSize = VxgiGetMinSampleSizeInternal(distanceFromAnchor);
        float tStep, fLevel, sampleSize;
        VxgiCalculateSampleParameters(t, args.coneFactor, args.tracingStep, minSampleSize, tStep, fLevel, sampleSize);
        if (fLevel >= g_VxgiAbstractTracingCB.MaxMipmapLevel + 1 || (args.maxTracingDistance != 0 && t > maxT)) {
            break;
        }
        float sampleSizeWorld = sampleSize * g_VxgiAbstractTracingCB.FinestVoxelSize;
        distanceToBoundary -= sampleSizeWorld;
        if (distanceToBoundary < 0) {
            break;
        }
        if (args.enableSceneBoundsCheck) {
            if (any((curPosition.xyz + sampleSizeWorld) < g_VxgiAbstractTracingCB.SceneBoundaryLower.xyz) || any((curPosition.xyz - sampleSizeWorld) > g_VxgiAbstractTracingCB.SceneBoundaryUpper.xyz))
                break;
        }
        float ambientFactor = exp(-t * args.ambientAttenuationFactor);
        coneAmbient += (prevAmbientFactor - ambientFactor) * transparency;
        prevAmbientFactor = ambientFactor;
        float opacity;
        float3 emittance;
        bool anyEmittance;
        float weight = saturate(distanceToBoundary / sampleSizeWorld);
        float3 adjustedPosition = curPosition;
        if (args.tangentJitterScale > 0) {
            float2 poisson = g_VxgiPoissonDisk[(int(sampleIndex) + poissonOffset) & (g_VxgiPoissonDiskSize - 1)];
            poisson *= sampleSize * g_VxgiAbstractTracingCB.FinestVoxelSize * args.tangentJitterScale;
            adjustedPosition = curPosition + tangent * poisson.x + cotangent * poisson.y;
        }
        VxgiSampleVoxelData(adjustedPosition, fLevel, direction, weight, args.flipOpacityDirections, opacity, emittance, anyEmittance);
        if (anyEmittance) {
            emittance *= emittanceScale;
            emittance *= VxgiSqr(args.coneFactor / (sampleSize * g_VxgiAbstractTracingCB.FinestVoxelSize));
        }
        float correctedOpacity = 0;
        opacity = saturate(opacity);
        if (opacity > 0) {
            correctedOpacity = 1.0 - pow(1.0 - opacity, (tStep * args.opacityCorrectionFactor / sampleSize));
        }
#if EMITTANCE_FORMAT != NONE
        coneIrradiance += ppTransparency * emittance;
        ppTransparency = pTransparency;
        pTransparency = transparency;
        transparency *= 1 - correctedOpacity;
        bool opaque = (ppTransparency < 0.0001);
#else
        transparency *= 1 - correctedOpacity;
        bool opaque = (transparency < 0.0001);
#endif
        VxgiOnConeSample(t, adjustedPosition, direction, fLevel, sampleIndex, transparency, emittance, args.monitoringUserData);
        if (opaque) {
            break;
        }
        t += tStep;
        curPosition += tStep * g_VxgiAbstractTracingCB.FinestVoxelSize * direction;
        ++sampleIndex;
    }
    VxgiConeTracingResults result;
    result.irradiance = coneIrradiance;
    result.ambient = saturate(coneAmbient);
    result.finalOpacity = saturate(1.0f - transparency);
    result.sampleCount = sampleIndex + 1;
    return result;
}
#ifdef TEST_COMPILE
void main(out float4 gl_FragColor : SV_Target)
{
    VxgiConeTracingArguments args = VxgiDefaultConeTracingArguments();
    VxgiConeTracingResults cone = VxgiTraceCone(args);
    gl_FragColor = float4(cone.irradiance.rgb, cone.ambient);
}
#endif
