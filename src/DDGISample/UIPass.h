#ifndef UIPASS_H
#define UIPASS_H
#include <donut/app/ImGuiRenderPass.h>

struct Config;

class UIPass : public donut::app::ImGuiRenderPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using ImGuiRenderPass::ImGuiRenderPass;

    bool Init(donut::engine::ShaderFactory *shaderFactory, Config *config);

 private:
    void SetupStyle();
    void BuildUI() override;

    Config *m_Config;
};

#endif /* UIPASS_H */
