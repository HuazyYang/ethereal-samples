#pragma once
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
#include <array>
#include <ShaderToolBlob.h>
#include <cstring>
#include <vector>

// Graphics stages use independent register namespaces in the example shaders.
// Give each stage its own NVRHI layout and Vulkan descriptor set, including an
// empty layout when a stage has no resources so set indices stay stable.
namespace waveworks::graphics {
inline nvrhi::ShaderHandle createGraphicsShader(nvrhi::IDevice* device,
    const nvrhi::ShaderDesc& desc, const void* data, size_t size)
{
    if (size >= 4 && std::memcmp(data, "NVSP", 4) == 0) {
        const char* space = nullptr;
        switch (desc.shaderType) {
        case nvrhi::ShaderType::Vertex: space = "space0"; break;
        case nvrhi::ShaderType::Hull: space = "space1"; break;
        case nvrhi::ShaderType::Domain: space = "space2"; break;
        case nvrhi::ShaderType::Geometry: space = "space3"; break;
        case nvrhi::ShaderType::Pixel: space = "space4"; break;
        default: return nullptr;
        }
        ShaderToolBlobConstant define{"WAVEWORKS_SPACE", space};
        if (ShaderToolBlobFindPermutation(data, size, &define, 1, &data, &size) != 0)
            return nullptr;
    }
    if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
        // Permutation data can begin at an unaligned byte offset in the blob.
        if (size % sizeof(uint32_t)) return nullptr;
        std::vector<uint32_t> aligned(size / sizeof(uint32_t));
        std::memcpy(aligned.data(), data, size);
        nvrhi::ShaderHandle result;
        device->createShader(desc, aligned.data(), size, &result);
        return result;
    }
    nvrhi::ShaderHandle result;
    device->createShader(desc, data, size, &result);
    return result;
}
template<class Item> struct GraphicsStageDesc {
    nvrhi::vector<Item> VS, HS, DS, GS, PS;
    const nvrhi::vector<Item>& stage(size_t i) const {
        switch (i) { case 0: return VS; case 1: return HS; case 2: return DS;
        case 3: return GS; default: return PS; }
    }
};
using GraphicsBindingLayoutDesc = GraphicsStageDesc<nvrhi::BindingLayoutItem>;
using GraphicsBindingSetDesc = GraphicsStageDesc<nvrhi::BindingSetItem>;
struct GraphicsBindingLayouts {
    nvrhi::BindingLayoutVector layouts;
    GraphicsBindingLayouts() = default;
    GraphicsBindingLayouts(std::nullptr_t) {}
    explicit operator bool() const { return layouts.size() == 5; }
};
struct GraphicsBindingSets {
    std::array<nvrhi::BindingSetHandle, 5> owned;
    nvrhi::BindingSetVector sets() const {
        nvrhi::BindingSetVector result;
        for (const auto& set : owned) result.push_back(set);
        return result;
    }
    GraphicsBindingSets() = default;
    GraphicsBindingSets(std::nullptr_t) {}
    explicit operator bool() const {
        for (const auto& set : owned) if (!set) return false;
        return true;
    }
};
inline GraphicsBindingLayouts createBindingLayout(nvrhi::IDevice* device,
    const GraphicsBindingLayoutDesc& desc)
{
    GraphicsBindingLayouts result;
    constexpr nvrhi::ShaderType stages[] = {nvrhi::ShaderType::Vertex,
        nvrhi::ShaderType::Hull, nvrhi::ShaderType::Domain,
        nvrhi::ShaderType::Geometry, nvrhi::ShaderType::Pixel};
    for (unsigned i = 0; i < 5; ++i) {
        nvrhi::BindingLayoutDesc layout;
        layout.visibility = stages[i];
        layout.bindings = desc.stage(i);
        // DX register spaces remain zero; Vulkan descriptor sets follow layout order.
        layout.bindingOffsets.shaderResource = i * 512;
        layout.bindingOffsets.sampler = i * 512 + 128;
        layout.bindingOffsets.constantBuffer = i * 512 + 256;
        layout.bindingOffsets.unorderedAccess = i * 512 + 384;
        nvrhi::BindingLayoutHandle handle;
        device->createBindingLayout(layout, &handle);
        if (!handle) return {};
        result.layouts.push_back(handle);
    }
    return result;
}
inline GraphicsBindingSets createBindingSet(nvrhi::IDevice* device,
    const GraphicsBindingSetDesc& desc, const GraphicsBindingLayouts& layouts)
{
    GraphicsBindingSets result;
    if (!layouts) return result;
    for (unsigned i = 0; i < 5; ++i) {
        nvrhi::BindingSetDesc set;
        set.bindings = desc.stage(i);
        device->createBindingSet(set, layouts.layouts[i], &result.owned[i]);
        if (!result.owned[i]) return {};
    }
    return result;
}
} // namespace waveworks::graphics
