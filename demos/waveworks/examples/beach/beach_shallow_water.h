#pragma once

#include "beach_scene.h"
#include "beach_shallow_simulation.h"
#include "main_state.h"
#include "graphics_bindings.h"

// One stable set of GPU resources, independent of WaveWorks resource rebinding.
// Coordinates refer to cell centres, unlike the vertex-centred terrain texture.
class BeachShallowWater {
 public:
    BeachShallowWater(nvrhi::IDevice* device, MainState* state);
    bool Init(const BeachBathymetry& bathymetry);
    void Animate(float elapsed);
    void Upload();
    void Shutdown();

 private:
    struct Pixel {
        float x, y, z, w;
    };
    bool ValidateLifecycle();
    nvrhi::IDevice* m_device;
    MainState* m_state;
    nvrhi::CommandListHandle m_commands;
    BeachShallowSimulation m_simulation;
    std::vector<Pixel> m_surface, m_flow;
    double m_timeDebt = 0;
    bool m_uploaded = false, m_dirty = true;
};

inline void AppendBeachShallowLayout(waveworks::graphics::GraphicsBindingLayoutDesc& layout, bool domain) {
    if (domain) {
        layout.DS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(3));
        layout.DS.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(3));
    }
    layout.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(14));
    layout.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(15));
    layout.PS.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(3));
}

inline void AppendBeachShallowBindings(waveworks::graphics::GraphicsBindingSetDesc& bindings,
                                       const MainState& state, bool domain) {
    if (domain) {
        bindings.DS.push_back(nvrhi::BindingSetItem::Texture_SRV(
            3, state.hBeachShallowSurface, nvrhi::Format::RGBA32_FLOAT));
        bindings.DS.push_back(
            nvrhi::BindingSetItem::ConstantBuffer(3, state.hBeachShallowConstants));
    }
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(14, state.hBeachShallowSurface,
                                                             nvrhi::Format::RGBA32_FLOAT));
    bindings.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(15, state.hBeachShallowFlow,
                                                             nvrhi::Format::RGBA32_FLOAT));
    bindings.PS.push_back(
        nvrhi::BindingSetItem::ConstantBuffer(3, state.hBeachShallowConstants));
}
