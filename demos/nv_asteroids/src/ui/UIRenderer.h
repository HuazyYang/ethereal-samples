#pragma once

// Asteroids.exe: UIRenderer (vtable 0x140258420, ctor 0x140024960, buildUI 0x14003A430, 480 bytes)
//
// The demo UI on top of the framework's ImGui renderer: the loading screen, the exit and benchmark
// popups, the "Settings" window and the keynote counters overlay (keynoteCounters.json, 0x140030A90).
// Only BuildUI is overridden; every other virtual is the base class's.
//
// deviation: donut main's ImGui_Renderer carried a font registry (RegisteredFont,
// CreateFontFromFile, rescaling on DPI change). ImGuiRenderPass has none, so the three
// demo fonts are registered against the ImGui atlas here. ImGui 1.92 rasterizes a font
// per requested size, which is what the rescaling used to do by hand.

#include <donut/app/ImGuiRenderPass.h>
#include <donut/core/math/math.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct UIData;
class FeatureDemo;
struct ImFont;

namespace donut::vfs
{
    class IFileSystem;
}

class UIRenderer : public donut::app::ImGuiRenderPass
{
    // Adds no interface and no class ID of its own.
    NVRHI_INHERIT_INTERFACE_TABLE()

public:
    // Font slots in load order (WinMain loads OpenSans 17, geforce-light 51, geforce-bold 51).
    enum FontSlot
    {
        FontOpenSans = 0,
        FontGeForceLight = 1,
        FontGeForceBold = 2
    };

    UIRenderer(donut::app::DeviceManager* deviceManager, nvrhi::AutoPtr<FeatureDemo> demo, UIData& ui);

    // Asteroids.exe: 0x140091DE0 (ImGui_Renderer::LoadFont in 2018). Fonts are referenced by load order.
    bool LoadFont(donut::vfs::IFileSystem& fs, const std::filesystem::path& fontFile, float fontSize);

protected:
    void BuildUI() override;

private:
    // One entry of keynoteCounters.json (56 bytes in the vector at +456).
    struct KeynoteCounter
    {
        enum Type : int
        {
            MaxLodTriangles = 1,    // "max_lod_counter"
            DrawnTriangles = 2,     // "tris_counter"
            Asteroids = 3,          // "asteroids_counter"
            Efficiency = 4,         // "efficiency_counter"
            Lod = 5,                // "lod_counter"
            Fps = 6                 // "fps_counter"
        };

        Type type = Fps;
        std::string label = "noname";       // "label"
        float fontSize = 0.f;               // "font_size" (the bold font is loaded at 51)
        dm::float2 position = 0.f;          // "pos"; negative coordinates count from the right/bottom
        bool makePretty = true;             // "makePretty": "%.2f Millions" style instead of 1,234,567
        bool showPerFrame = true;           // "showPerFrame": otherwise multiplied by the frame rate
        bool includeShadows = false;        // "includeShadows": statistics including the shadow passes
    };

    void LoadKeynoteCounters();                                                     // 0x140030A90
    void BuildLoadingScreen();
    void BuildPopups();
    void BuildSettingsWindow();
    void BuildCountersOverlay();
    std::string FormatCounterValue(const KeynoteCounter& counter, double averageFrameTime) const;

    ImFont* GetFont(int slot) const;                                                // 0x140091B40

    nvrhi::AutoPtr<FeatureDemo> m_Demo;                                            // +424
    UIData& m_UI;                                                                   // +440
    nvrhi::CommandListHandle m_CommandList;                                         // +448 (created, never used)
    std::vector<KeynoteCounter> m_Counters;                                         // +456
    // One registered font. The TTF stays in the blob: the atlas entry is created with
    // FontDataOwnedByAtlas = false, so ImGui must not free it.
    struct LoadedFont
    {
        nvrhi::AutoPtr<nvrhi::IDataBlob> data;
        ImFont* font = nullptr;
    };
    std::vector<LoadedFont> m_LoadedFonts;
};
