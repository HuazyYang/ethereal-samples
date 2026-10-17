#include "example_app.h"
#include "beach_scene.h"
#include "beach_scene_pipeline.h"

namespace
{
nvrhi::AutoPtr<IExampleScenePipeline> CreateBeachPipeline(donut::app::DeviceManager* manager, MainState* state,
    SkyRenderer* sky, OceanRenderer* ocean, UIRenderer* ui)
{
    return MAKE_RC_OBJ_PTR(BeachScenePipeline, manager, state, sky, ocean, ui);
}
}

int main(int argc, char** argv)
{
    const ExampleSceneDefinition scene = { 522, ConfigureBeachScene, CreateBeachPipeline, true };
    return RunExample(argc, argv, scene);
}
