#include "UIPass.h"

bool UIPass::Init(donut::engine::ShaderFactory* shaderFactory, Config* config) {
    if (!ImGuiRenderPass::Init(shaderFactory)) return false;

    ImGui::StyleColorsDark();
    SetupStyle();

    m_Config = config;
    return true;
}

void UIPass::SetupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_TitleBg] = (ImVec4)ImColor(70, 70, 70, 120);
    style.Colors[ImGuiCol_TitleBgActive] = (ImVec4)ImColor(70, 70, 70, 255);
    style.Colors[ImGuiCol_TitleBgCollapsed] = (ImVec4)ImColor(70, 70, 70, 255);
    style.Colors[ImGuiCol_FrameBg] = (ImVec4)ImColor(70, 70, 70, 120);
    style.Colors[ImGuiCol_FrameBgHovered] = (ImVec4)ImColor(70, 70, 70, 200);
    style.Colors[ImGuiCol_FrameBgActive] = (ImVec4)ImColor(70, 70, 70, 255);
    style.Colors[ImGuiCol_Header] = (ImVec4)ImColor(70, 70, 70, 120);
    style.Colors[ImGuiCol_HeaderHovered] = (ImVec4)ImColor(70, 70, 70, 200);
    style.Colors[ImGuiCol_HeaderActive] = (ImVec4)ImColor(70, 70, 70, 255);
    style.Colors[ImGuiCol_Button] = (ImVec4)ImColor(70, 70, 70, 120);
    style.Colors[ImGuiCol_ButtonHovered] = (ImVec4)ImColor(70, 70, 70, 200);
    style.Colors[ImGuiCol_ButtonActive] = (ImVec4)ImColor(70, 70, 70, 255);
    style.Colors[ImGuiCol_Tab] = (ImVec4)ImColor(70, 70, 70, 120);
    style.Colors[ImGuiCol_TabHovered] = (ImVec4)ImColor(70, 70, 70, 200);
    style.Colors[ImGuiCol_TabActive] = (ImVec4)ImColor(70, 70, 70, 255);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.f, 1.f, 1.f, 1.f);
}

void UIPass::BuildUI() { ImGui::ShowDemoWindow(); }
