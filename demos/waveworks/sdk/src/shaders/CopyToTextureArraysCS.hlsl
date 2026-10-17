// Recovered from the matching DXBC assembly; low-level register flow is retained.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_sourceMipSize : packoffset(c0.x);
    uint g_targetTextureSize : packoffset(c0.y);
    float2 pad0 : packoffset(c0.z);
};

SamplerState g_bilinearSampler : register(s0);
Texture2D<float4> g_sourceDisplacements1 : register(t0);
Texture2D<float4> g_sourceDisplacements2 : register(t1);
Texture2D<float4> g_sourceDisplacements3 : register(t2);
Texture2D<float4> g_sourceDisplacements4 : register(t3);
Texture2D<float4> g_sourceGradients1 : register(t4);
Texture2D<float4> g_sourceGradients2 : register(t5);
Texture2D<float4> g_sourceGradients3 : register(t6);
Texture2D<float4> g_sourceGradients4 : register(t7);
Texture2D<float4> g_sourceMoments1 : register(t8);
Texture2D<float4> g_sourceMoments2 : register(t9);
Texture2D<float4> g_sourceMoments3 : register(t10);
Texture2D<float4> g_sourceMoments4 : register(t11);
RWTexture2DArray<float4> g_targetDisplacementsArray : register(u0);
RWTexture2DArray<float4> g_targetGradientsArray : register(u1);
RWTexture2DArray<float4> g_targetMomentsArray : register(u2);

[numthreads(16, 16, 1)]
void CopyToTextureArraysCS(uint3 groupID : SV_GroupID, uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float(g_targetTextureSize);
    uint cascadeIndex = groupID.z;
    float4 displacement;
    float4 gradient;
    float4 moments;

    if (cascadeIndex == 0u)
    {
        displacement = g_sourceDisplacements1.SampleLevel(g_bilinearSampler, uv, 0.0f);
        gradient = g_sourceGradients1.SampleLevel(g_bilinearSampler, uv, 0.0f);
        moments = g_sourceMoments1.SampleLevel(g_bilinearSampler, uv, 0.0f);
    }
    else if (cascadeIndex == 1u)
    {
        displacement = g_sourceDisplacements2.SampleLevel(g_bilinearSampler, uv, 0.0f);
        gradient = g_sourceGradients2.SampleLevel(g_bilinearSampler, uv, 0.0f);
        moments = g_sourceMoments2.SampleLevel(g_bilinearSampler, uv, 0.0f);
    }
    else if (cascadeIndex == 2u)
    {
        displacement = g_sourceDisplacements3.SampleLevel(g_bilinearSampler, uv, 0.0f);
        gradient = g_sourceGradients3.SampleLevel(g_bilinearSampler, uv, 0.0f);
        moments = g_sourceMoments3.SampleLevel(g_bilinearSampler, uv, 0.0f);
    }
    else
    {
        displacement = g_sourceDisplacements4.SampleLevel(g_bilinearSampler, uv, 0.0f);
        gradient = g_sourceGradients4.SampleLevel(g_bilinearSampler, uv, 0.0f);
        moments = g_sourceMoments4.SampleLevel(g_bilinearSampler, uv, 0.0f);
    }

    uint3 targetCoordinates = uint3(dispatchThreadID.xy, cascadeIndex);
    g_targetDisplacementsArray[targetCoordinates] = displacement;
    g_targetGradientsArray[targetCoordinates] = gradient;
    g_targetMomentsArray[targetCoordinates] = moments;
}
