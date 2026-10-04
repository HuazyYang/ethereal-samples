#include "fx/FxCommon.h"

#include "scene/Lights.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShadowMap.h>

#include <cstring>

using namespace donut::math;
#include <donut/shaders/light_cb.h> // donut main ShadowConstants (filled by IShadowMap)
#include <light_cb.h>               // 2018 structs (namespace light2018)

namespace fx
{
    void FillLightConstants2018(const SceneLight& light, light2018::LightConstants& out)
    {
        light.FillLightConstants(out);
    }

    // donut main's ShadowConstants has exactly the 2018 layout; IShadowMap fills the donut type.
    static_assert(sizeof(::ShadowConstants) == sizeof(light2018::ShadowConstants), "ShadowConstants layout");

    uint32_t FillShadowConstants2018(const donut::engine::IShadowMap* shadowMap, light2018::LightConstants& light,
        light2018::ShadowConstants* shadows, uint32_t maxShadows, bool includePerObject)
    {
        light.shadowCascades = int4(-1);
        light.perObjectShadows = int4(-1);

        if (!shadowMap)
            return 0;

        uint32_t numShadows = 0;
        for (uint32_t cascade = 0; cascade < shadowMap->GetNumberOfCascades(); cascade++)
        {
            if (numShadows < maxShadows && cascade < 4)
            {
                ::ShadowConstants constants{};
                shadowMap->GetCascade(cascade)->FillShadowConstants(constants);
                memcpy(&shadows[numShadows], &constants, sizeof(constants));
                light.shadowCascades[cascade] = int(numShadows);
                numShadows++;
            }
        }

        if (includePerObject)
        {
            for (uint32_t object = 0; object < shadowMap->GetNumberOfPerObjectShadows(); object++)
            {
                if (numShadows < maxShadows && object < 4)
                {
                    ::ShadowConstants constants{};
                    shadowMap->GetPerObjectShadow(object)->FillShadowConstants(constants);
                    memcpy(&shadows[numShadows], &constants, sizeof(constants));
                    light.perObjectShadows[object] = int(numShadows);
                    numShadows++;
                }
            }
        }

        return numShadows;
    }

    nvrhi::BufferDesc ConstantBufferDesc(uint32_t byteSize, const char* debugName, bool isVolatile)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.debugName = debugName;
        desc.isConstantBuffer = true;
        desc.isVolatile = isVolatile;
        if (isVolatile)
            desc.maxVersions = donut::engine::c_MaxRenderPassConstantBufferVersions;
        else
        {
            desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
            desc.keepInitialState = true;
        }
        return desc;
    }

    nvrhi::BlendState::RenderTarget BlendStateRT(nvrhi::BlendFactor srcBlend, nvrhi::BlendFactor destBlend)
    {
        nvrhi::BlendState::RenderTarget rt;
        rt.blendEnable = true;
        rt.srcBlend = srcBlend;
        rt.destBlend = destBlend;
        rt.blendOp = nvrhi::BlendOp::Add;
        rt.srcBlendAlpha = nvrhi::BlendFactor::One;
        rt.destBlendAlpha = nvrhi::BlendFactor::Zero;
        rt.blendOpAlpha = nvrhi::BlendOp::Add;
        return rt;
    }
}
