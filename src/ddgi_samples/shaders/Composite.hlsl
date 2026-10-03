/*
* Copyright (c) 2019-2023, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "Common.hlsli"
#include "Descriptors.hlsli"
#include "Random.hlsli"

// ---[ Structures ]---

struct PSInput
{
    float4 position : SV_POSITION;
    float2 tex0     : TEXCOORD;
};

// ---[ Vertex Shader ]---

PSInput VS(uint VertID : SV_VertexID)
{
    float div2 = (VertID / 2);
    float mod2 = (VertID % 2);

    PSInput result;
    result.position.x = (mod2 * 2.f) - 1.f;
    result.position.y = (div2 * 2.f) - 1.f;
    result.position.zw = float2(0.f, 1.f);

    result.tex0.x = mod2;
    result.tex0.y = 1.f - div2;

    return result;
}

// ---[ Pixel Shader ]---

float4 PS(PSInput input) : SV_TARGET
{
    float3 color = float3(0.f, 0.f, 0.f);
    float  ambientOcclusion = 1.f;

    // Load the albedo and convert to linear before lighting
    Texture2D<float4> GBufferA = GetFixedTex2D(GBUFFERA, SRV);
    float4 albedo = GBufferA.Load(int3(input.position.xy, 0));

    // Store the albedo for pixels that aren't lit (e.g. visualizations)
    color = albedo.rgb;

    // Get the usage flags
    uint useFlags = GetGlobalConst(composite, useFlags);

    // Primary ray hit, need to light it
    if (albedo.a >= COMPOSITE_FLAG_LIGHT_PIXEL)
    {
        // Get the (bindless) resources
        Texture2D<float4> GBufferB = GetFixedTex2D(GBUFFERB, SRV);
        Texture2D<float4> GBufferC = GetFixedTex2D(GBUFFERC, SRV);
        Texture2D<float4> GBufferD = GetFixedTex2D(GBUFFERD, SRV);

        // Convert albedo back to linear
        albedo.rgb = SRGBToLinear(albedo.rgb);

        // Load world position, hit distance, and normal
        float4 worldPosHitT = GBufferB.Load(int3(input.position.xy, 0));
        float3 normal = GBufferC.Load(int3(input.position.xy, 0)).xyz;

        // Load the direct lighting
        color = GBufferD.Load(int3(input.position.xy, 0)).rgb;

        // Load indirect lighting from DDGI
        if (useFlags & COMPOSITE_FLAG_USE_DDGI)
        {
            // Add direct and indirect lighting
            Texture2D<float4> DDGIOutput = GetFixedTex2D(DDGI_OUTPUT, SRV);
            float3 indirect = DDGIOutput.Load(int3(input.position.xy, 0)).rgb;
            color += indirect;
        }

        if (useFlags & COMPOSITE_FLAG_USE_RTAO)
        {
            // Load ambient occlusion and multiply with lighting
            Texture2D<float4> RTAOOutput = GetFixedTex2D(RTAO_OUTPUT, SRV);
            ambientOcclusion = RTAOOutput.Load(int3(input.position.xy, 0)).x;
            color *= ambientOcclusion;
        }
    }

    // Get the show flags
    uint showFlags = GetGlobalConst(composite, showFlags);

    // Get the post processing useFlags
    uint ppUseFlags = GetGlobalConst(post, useFlags);

    if ((useFlags & COMPOSITE_FLAG_USE_RTAO) && (showFlags & COMPOSITE_FLAG_SHOW_RTAO))
    {
        // Visualize the ambient occlusion data and early out
        if (ppUseFlags & POSTPROCESS_FLAG_USE_GAMMA) return float4(ambientOcclusion.xxx, 1.f);
        return float4(LinearToSRGB(ambientOcclusion.xxx), 1.f);
    }

    if ((useFlags & COMPOSITE_FLAG_USE_DDGI) && (showFlags & COMPOSITE_FLAG_SHOW_DDGI_INDIRECT))
    {
        // Show only the indirect lighting from DDGI
        Texture2D<float4> DDGIOutput = GetFixedTex2D(DDGI_OUTPUT, SRV);
        float3 indirect = DDGIOutput.Load(int3(input.position.xy, 0)).rgb;
        color = indirect;
    }

    // Early out, no post processing
    if (ppUseFlags == POSTPROCESS_FLAG_USE_NONE) return float4(color, 1.f);

    // Selectively apply exposure, tonemapping, and dither based on the GBuffer's composite flag
    if(albedo.a > COMPOSITE_FLAG_IGNORE_PIXEL)
    {
        // Exposure
        if (ppUseFlags & POSTPROCESS_FLAG_USE_EXPOSURE)
        {
            color *= GetGlobalConst(post, exposure);
        }

        // Tonemapping
        if (ppUseFlags & POSTPROCESS_FLAG_USE_TONEMAPPING)
        {
            color = ACESFilm(color);
        }

        // Dither to reduce SDR color banding
        if (ppUseFlags & POSTPROCESS_FLAG_USE_DITHER)
        {
            Texture2D<float4> BlueNoise = GetFixedTex2D(BLUE_NOISE, SRV);
            color += GetLowDiscrepancyBlueNoise(int2(input.position.xy), GetFrameNumber(), 1.f / 256.f, BlueNoise);
        }
    }

    // Gamma correction
    if (ppUseFlags & POSTPROCESS_FLAG_USE_GAMMA)
    {
        color = LinearToSRGB(color);
    }

    return float4(color, 1.f);
}
