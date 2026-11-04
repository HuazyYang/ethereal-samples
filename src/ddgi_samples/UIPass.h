#ifndef UIPASS_H
#define UIPASS_H
#include <donut/app/ImGuiLayer.h>

struct Config;

class UIPass : public donut::app::ImGuiLayer {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using ImGuiLayer::ImGuiLayer;

    bool Init(donut::engine::ShaderFactory *shaderFactory, Config *config);

 private:
    void SetupStyle();
    void BuildUI() override;

    Config *m_Config;
};

#endif /* UIPASS_H */
