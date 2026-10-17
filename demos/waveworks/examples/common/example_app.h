#pragma once

#include <cstdint>

#include <donut/app/DeviceManager.h>

class OceanRenderer;
class SkyRenderer;
class UIRenderer;
struct MainState;

class IExampleScenePipeline : public donut::app::IRenderPass
{
public:
    using IRenderPass::IRenderPass;
    ~IExampleScenePipeline() override = default;
    virtual bool Init(uint32_t width, uint32_t height, uint32_t sampleCount) = 0;
    virtual void Shutdown() = 0;
};

using ConfigureExampleScene = void (*)(MainState& state);
using CreateExampleScenePipeline = nvrhi::AutoPtr<IExampleScenePipeline> (*)(donut::app::DeviceManager* manager,
    MainState* state, SkyRenderer* sky, OceanRenderer* ocean, UIRenderer* ui);

struct ExampleSceneDefinition
{
    uint32_t backBufferHeight = 800;
    ConfigureExampleScene configure = nullptr;
    CreateExampleScenePipeline createPipeline = nullptr;
    bool supportsBeachTerrain = false;
};

int RunExample(int argc, char** argv, const ExampleSceneDefinition& scene);
