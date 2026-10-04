#include "ui/UIRenderer.h"

#include "app/AppGlobals.h"
#include "app/FeatureDemo.h"
#include "app/JsonFile.h"
#include "app/UIData.h"
#include "audio/SoundEngine.h"
#include "scene/AsteroidLibrary.h"
#include "scene/SpaceScene.h"

#include <donut/core/json.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/TextureCache.h>

#include <imgui.h>
#include <json/json.h>

#include <cinttypes>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace donut;
using namespace donut::math;

namespace
{
    // Total number of objects + textures loaded by the scene; the progress shown is (loaded * 100 / 216).
    constexpr float c_LoadingItemCount = 216.f;

    // Layout constants of the settings window (0x1402594EC, 0x140259504, 0x1402594F8, 0x1402594FC, 0x140259510).
    constexpr float c_Column1 = 130.f;
    constexpr float c_Column2 = 260.f;
    constexpr float c_SliderWidth = 190.f;
    constexpr float c_LodBiasColumn = 200.f;
    constexpr float c_LodBiasWidth = 380.f;
    const ImVec2 c_PopupButtonSize(120.f, 40.f);

    // Font size the bold counter font was loaded with; counters scale relative to it.
    constexpr float c_CounterFontBaseSize = 51.f;

    const ImVec4 c_Black(0.f, 0.f, 0.f, 1.f);
    const ImVec4 c_White(1.f, 1.f, 1.f, 1.f);

    // "LOD Colors:" swatches (0x140255390, 0x140259630..0x1402596B0, 0x140256830).
    const ImVec4 c_LodColors[10] = {
        ImVec4(0.f, 0.f, 0.f, 1.f),
        ImVec4(0.8f, 0.f, 0.f, 1.f),
        ImVec4(1.f, 0.5f, 0.f, 1.f),
        ImVec4(1.f, 1.f, 0.f, 1.f),
        ImVec4(0.f, 1.f, 0.f, 1.f),
        ImVec4(0.f, 0.75f, 0.25f, 1.f),
        ImVec4(0.f, 0.f, 1.f, 1.f),
        ImVec4(0.5f, 0.f, 1.f, 1.f),
        ImVec4(0.5f, 0.5f, 1.f, 1.f),
        ImVec4(1.f, 1.f, 1.f, 1.f)
    };

    void PushFontAtSize(ImFont* font, float size)
    {
        ImGui::PushFont(font, size);
    }

    void PushFontDefaultSize(ImFont* font)
    {
        ImGui::PushFont(font, font ? font->LegacySize : 0.f);
    }

    // Large numbers with thousands separators: 1234567 -> "1,234,567".
    std::string FormatWithSeparators(uint64_t value)
    {
        char digits[32];
        snprintf(digits, sizeof(digits), "%" PRIu64, value);

        const int length = int(strlen(digits));
        std::string result;
        int phase = 3 * (length / 3) - length + 2;
        for (int i = 0; i < length; i++)
        {
            result += digits[i];
            if (phase == 1)
                result += ',';
            phase = (phase + 1) % 3;
        }
        // The loop always leaves a trailing separator.
        if (!result.empty())
            result.pop_back();
        return result;
    }

    std::string FormatPretty(uint64_t value)
    {
        char buffer[64];
        const float f = float(value);
        // deviation: the binary formats these with swprintf and narrow format strings, which only worked
        // because no shipped counter enables "makePretty"; plain snprintf is used here.
        if (f >= 99999997952.f)
            snprintf(buffer, sizeof(buffer), "%.2f Trillions", double(f / 1.0e12f));
        else if (value >= 1000000000ull)
            snprintf(buffer, sizeof(buffer), "%.2f Billions", double(f / 1.0e9f));
        else if (value >= 1000000ull)
            snprintf(buffer, sizeof(buffer), "%.2f Millions", double(f / 1.0e6f));
        else
            snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
        return buffer;
    }
}

UIRenderer::UIRenderer(app::DeviceManager* deviceManager, std::shared_ptr<FeatureDemo> demo, UIData& ui)
    : ImGui_Renderer(deviceManager)
    , m_Demo(std::move(demo))
    , m_UI(ui)
{
    m_CommandList = GetDevice()->createCommandList();
    LoadKeynoteCounters();
}

bool UIRenderer::LoadFont(vfs::IFileSystem& fs, const std::filesystem::path& fontFile, float fontSize)
{
    std::shared_ptr<app::RegisteredFont> font = CreateFontFromFile(fs, fontFile, fontSize);
    m_LoadedFonts.push_back(font);
    return font && font->HasFontData();
}

ImFont* UIRenderer::GetFont(int slot) const
{
    if (slot < 0 || size_t(slot) >= m_LoadedFonts.size() || !m_LoadedFonts[slot])
        return nullptr;
    return m_LoadedFonts[slot]->GetScaledFont();
}

void UIRenderer::LoadKeynoteCounters()
{
    Json::Value root;
    if (!demo::LoadJsonFile(*m_Demo->GetRootFileSystem(), m_Demo->GetMediaPath() / "keynoteCounters.json", root, true))
    {
        log::warning("Error reading keynote counters config file");
        return;
    }

    if (!root.isObject())
    {
        log::warning("Error : expected root object");
        return;
    }

    auto addCounter = [this, &root](const char* name, KeynoteCounter::Type type)
    {
        const Json::Value& node = root[name];
        if (!node.isObject())
            return;

        KeynoteCounter counter;
        counter.type = type;
        counter.label = json::Read<std::string>(node["label"], "noname");
        counter.fontSize = json::Read<float>(node["font_size"], 0.f);
        counter.position = json::Read<float2>(node["pos"], float2(0.f));
        counter.makePretty = json::Read<bool>(node["makePretty"], true);
        counter.showPerFrame = json::Read<bool>(node["showPerFrame"], true);
        counter.includeShadows = json::Read<bool>(node["includeShadows"], false);
        m_Counters.push_back(counter);
    };

    addCounter("max_lod_counter", KeynoteCounter::MaxLodTriangles);
    addCounter("tris_counter", KeynoteCounter::DrawnTriangles);
    addCounter("asteroids_counter", KeynoteCounter::Asteroids);
    addCounter("efficiency_counter", KeynoteCounter::Efficiency);
    addCounter("lod_counter", KeynoteCounter::Lod);
    addCounter("fps_counter", KeynoteCounter::Fps);
}

void UIRenderer::buildUI()
{
    if (m_Demo->IsSceneLoading())
    {
        BuildLoadingScreen();
        return;
    }

    BuildPopups();

    if (m_UI.showGui)
        BuildSettingsWindow();

    if (m_UI.showCounters)
        BuildCountersOverlay();
}

void UIRenderer::BuildLoadingScreen()
{
    BeginFullScreenWindow();

    int width = 0, height = 0;
    GetDeviceManager()->GetWindowDimensions(width, height);

    PushFontDefaultSize(GetFont(FontGeForceBold));

    char text[256];
    snprintf(text, sizeof(text), "LOADING");
    ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, -1.f);
    ImGui::SetCursorPosX((float(width) - textSize.x) * 0.5f);
    ImGui::SetCursorPosY(textSize.y * 2.f);
    ImGui::TextUnformatted(text);

    // 2018: global loading statistics (0x14005C720, +4 = completed items) plus the loaded textures.
    uint32_t loaded = uint32_t(SpaceScene::GetLoadProgress().completed.load());
    if (const auto& textureCache = m_Demo->GetTextureCache())
        loaded += textureCache->GetNumberOfLoadedTextures();

    snprintf(text, sizeof(text), "%d%%", int(float(loaded) * 100.f / c_LoadingItemCount));
    textSize = ImGui::CalcTextSize(text, nullptr, false, -1.f);
    ImGui::SetCursorPosX((float(width) - textSize.x) * 0.5f);
    ImGui::SetCursorPosY(textSize.y * 4.f);
    ImGui::TextUnformatted(text);

    ImGui::PopFont();
    EndFullScreenWindow();
}

void UIRenderer::BuildPopups()
{
    if (m_UI.showConfirmExit)
    {
        ImGui::OpenPopup("Confirm exit");
        if (ImGui::BeginPopupModal("Confirm exit", nullptr, ImGuiWindowFlags_NoResize))
        {
            ImGui::Spacing();
            ImGui::Text("Really quit the application ?");
            ImGui::Spacing();

            if (ImGui::Button("Exit", c_PopupButtonSize))
                PostQuitMessage(0);   // GLFW turns WM_QUIT into a close request of the window

            ImGui::SameLine(0.f, -1.f);
            if (ImGui::Button("Cancel", c_PopupButtonSize))
            {
                ImGui::CloseCurrentPopup();
                m_UI.showConfirmExit = false;
            }
            ImGui::EndPopup();
        }
    }

    if (m_UI.showBenchmarkResult)
    {
        ImGui::OpenPopup("Benchmark Result");
        if (ImGui::BeginPopupModal("Benchmark Result", nullptr, ImGuiWindowFlags_NoResize))
        {
            const double averageFrameTime = demo::g_BenchmarkTotalTime / double(demo::g_BenchmarkFrameCount);

            ImGui::Spacing();
            ImGui::Text("Average frame time = %.2f ms\nAverage FPS = %.1f", averageFrameTime * 1000.0, 1.0 / averageFrameTime);
            ImGui::Spacing();

            if (ImGui::Button("OK", c_PopupButtonSize))
            {
                ImGui::CloseCurrentPopup();
                m_UI.showBenchmarkResult = false;
            }
            ImGui::EndPopup();
        }
    }
}

void UIRenderer::BuildSettingsWindow()
{
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::CollapsingHeader("Keyboard and Mouse Controls"))
    {
        auto row = [](const char* key, const char* action)
        {
            ImGui::Text("%s", key);
            ImGui::SameLine(c_Column1, -1.f);
            ImGui::Text("%s", action);
        };
        row("\tClick+Drag", "Camera rotation");
        row("\tScroll", "Camera zoom");
        row("\tT", "Toggle camera");
        row("\tW/S", "Ship acceleration / Move camera");
        row("\tA/D", "Ship rotation / Move camera");
        row("\tQ/E", "Ship roll");
        row("\tArrows", "Ship rotation");
        row("\tSpace", "Stop the ship");
        row("\tTab", "Toggle GUI");          // unresolved: the key handler toggles the GUI with '`', not Tab
        row("\tF", "Toggle text (FPS etc.)");
        row("\tL", "Toggle dynamic/static LOD");
        row("\t0-6", "Select asteroid LOD");
    }

    if (ImGui::CollapsingHeader("Gamepad Controls"))
    {
        auto row = [](const char* key, const char* action)
        {
            ImGui::Text("%s", key);
            ImGui::SameLine(c_Column1, -1.f);
            ImGui::Text("%s", action);
        };
        row("\tLeft Stick", "Ship rotation");
        row("\tRight Stick", "Camera rotation");
        row("\tA/B", "Camera distance");
        row("\tLeft Trigger", "Decelerate");
        row("\tRight Trigger", "Accelerate");
        row("\tLB/RB", "Ship roll");
        row("\tX", "Stop the ship");
        row("\tUp", "Wireframe");
        row("\tDown", "Visualize LODs");
        row("\tLeft", "Text");
        row("\tRight", "Temporal AA");
    }

    if (ImGui::CollapsingHeader("Graphics Options", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool temporalAA = (m_UI.aaMode == AntiAliasingMode::TemporalAA);
        ImGui::Checkbox("VSync (V)", &m_UI.enableVsync);
        ImGui::SameLine(c_Column1, -1.f);
        ImGui::Checkbox("Temporal AA", &temporalAA);
        ImGui::SameLine(c_Column2, -1.f);
        ImGui::Checkbox("Hi-Z Culling", &m_UI.hiZCulling);
        m_UI.aaMode = temporalAA ? AntiAliasingMode::TemporalAA : AntiAliasingMode::None;

        ImGui::Checkbox("HBAO+", &m_UI.enableHbao);
        ImGui::SameLine(c_Column1, -1.f);
        ImGui::Checkbox("Shadows", &m_UI.enableShadows);
        ImGui::SameLine(c_Column2, -1.f);
        ImGui::Checkbox("Bloom", &m_UI.enableBloom);

        ImGui::Checkbox("LensFlare", &m_UI.enableLensFlare);
        ImGui::SameLine(c_Column1, -1.f);
        ImGui::Checkbox("Shield", &m_UI.enableShield);
        ImGui::SameLine(c_Column2, -1.f);
        ImGui::Checkbox("Fog", &m_UI.enableFog);

        ImGui::Checkbox("Particles", &m_UI.enableParticles);
        ImGui::SameLine(c_Column1, -1.f);
        if (ImGui::Button("Reset Particles", ImVec2(0.f, 0.f)))
            m_UI.resetParticles = true;

        ImGui::PushItemWidth(c_SliderWidth);
        ImGui::SliderFloat("Vertical FOV", &m_UI.verticalFov, 20.f, 110.f, "%.1f");
        ImGui::PopItemWidth();
    }

    if (ImGui::CollapsingHeader("LOD Options", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Wireframe", &m_UI.wireframe);
        ImGui::SameLine(c_Column1, -1.f);
        ImGui::Checkbox("Dynamic LOD", &m_UI.dynamicLod);
        ImGui::SameLine(c_Column2, -1.f);
        ImGui::Checkbox("Visualize LODs", &m_UI.visualizeLods);

        ImGui::Text("LOD Colors:");
        for (int lod = 0; lod < 10; lod++)
        {
            char label[32];
            snprintf(label, sizeof(label), "LOD %d", lod);
            ImGui::ColorButton(label, c_LodColors[lod], 0, ImVec2(0.f, 0.f));
            if (lod < 9)
                ImGui::SameLine(0.f, -1.f);
        }

        ImGui::Text("Camera LOD Bias");
        ImGui::SameLine(c_LodBiasColumn, -1.f);
        ImGui::Text("Shadow LOD Bias");

        float biases[2] = { m_UI.cameraLodBias, m_UI.shadowLodBias };
        ImGui::PushItemWidth(c_LodBiasWidth);
        // deviation: the binary used an empty label; recent ImGui needs a non-empty ID.
        ImGui::SliderFloat2("##LodBias", biases, -4.f, 2.f, "%.2f");
        ImGui::PopItemWidth();
        m_UI.cameraLodBias = biases[0];
        m_UI.shadowLodBias = biases[1];
    }

    if (ImGui::CollapsingHeader("Sound Options"))
    {
        SpaceScene* scene = m_Demo ? m_Demo->GetScene() : nullptr;
        audio::Sound* music = scene ? scene->GetAmbienceMusic() : nullptr;
        if (music)
        {
            // deviation: the binary kept the slider value in SpaceScene+424 (initially 1.0) and applied it with
            // IXAudio2Voice::SetVolume; the scene module does not expose that float, so the voice volume
            // itself is used (it starts at the fscene "ambienceMusicVolume").
            ImGui::PushItemWidth(c_SliderWidth);
            float volume = music->GetVolume() * 100.f;
            if (ImGui::SliderFloat("Music Volume", &volume, 0.f, 100.f, "%.0f %%"))
                music->SetVolume(volume * 0.01f);
            ImGui::PopItemWidth();
        }

        if (ImGui::Checkbox("Mute", &m_UI.mute))
            SpaceScene::SetSoundMuted(m_UI.mute);
    }

    if (ImGui::CollapsingHeader("Animations"))
    {
        if (ImGui::Button("Play Opening Sequence (P)", ImVec2(0.f, 0.f)))
            m_Demo->PlayOpeningSequence();

        ImGui::SameLine(0.f, -1.f);
        if (ImGui::Button("Reset (O)", ImVec2(0.f, 0.f)))
            m_Demo->ResetOpeningSequence();

        ImGui::Checkbox("Freeze Ship Position", &m_UI.freezeShipPosition);
    }

    if (ImGui::CollapsingHeader("Environment") && m_Demo->HasSunLight())
    {
        ImGui::PushItemWidth(c_SliderWidth);
        ImGui::SliderFloat("Solar Axis Elevation", &m_UI.solarAxisElevation, -90.f, 90.f, "%.1f");
        ImGui::SliderFloat("Solar Axis Azimuth", &m_UI.solarAxisAzimuth, -360.f, 360.f, "%.1f");
        ImGui::SliderFloat("Sun Rotation Around Axis", &m_UI.sunRotation, 5.f, 175.f, "%.1f");
        ImGui::PopItemWidth();
    }

    ImGui::End();
}

std::string UIRenderer::FormatCounterValue(const KeynoteCounter& counter, double averageFrameTime) const
{
    char buffer[256] = {};

    switch (counter.type)
    {
    case KeynoteCounter::Efficiency:
        if (m_UI.maxLodTriangles != 0)
        {
            const double culled = double(m_UI.maxLodTriangles - m_UI.drawnTriangles);
            snprintf(buffer, sizeof(buffer), "%.4f %%", culled / double(m_UI.maxLodTriangles) * 100.0);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "N/A");
        }
        return buffer;

    case KeynoteCounter::Lod:
        if (m_UI.dynamicLod)
            snprintf(buffer, sizeof(buffer), "Dynamic");
        else
            snprintf(buffer, sizeof(buffer), "%d", m_UI.staticLodIndex);
        return buffer;

    case KeynoteCounter::Fps:
        if (averageFrameTime > 0.0)
            snprintf(buffer, sizeof(buffer), "%.2f", 1.0 / averageFrameTime);
        return buffer;

    case KeynoteCounter::MaxLodTriangles:
    case KeynoteCounter::DrawnTriangles:
    case KeynoteCounter::Asteroids:
    {
        uint64_t value = 0;
        if (counter.type == KeynoteCounter::MaxLodTriangles)
            value = counter.includeShadows ? m_UI.maxLodTrianglesWithShadows : m_UI.maxLodTriangles;
        else if (counter.type == KeynoteCounter::DrawnTriangles)
            value = m_UI.drawnTriangles;
        else
            value = counter.includeShadows ? m_UI.asteroidsDrawnWithShadows : m_UI.asteroidsDrawn;

        // Per second unless "showPerFrame".
        int64_t multiplier = (averageFrameTime > 0.0) ? int64_t(int32_t(1.0 / averageFrameTime)) : 1;
        if (counter.showPerFrame)
            multiplier = 1;
        value *= uint64_t(multiplier);

        return counter.makePretty ? FormatPretty(value) : FormatWithSeparators(value);
    }

    default:
        return std::string();
    }
}

void UIRenderer::BuildCountersOverlay()
{
    int width = 0, height = 0;
    GetDeviceManager()->GetWindowDimensions(width, height);
    const double averageFrameTime = GetDeviceManager()->GetAverageFrameTimeSeconds();

    ImFont* font = GetFont(FontGeForceBold);
    if (!font)
        return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::SetNextWindowBgAlpha(0.f);
    // unresolved: the binary does not place the window; the counters are positioned in screen
    // coordinates, so it is pinned to the origin here.
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
    ImGui::Begin("##KeynoteCounters", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);

    // Two passes: a black drop shadow at the configured position, then the white text 2 px up-left.
    for (int pass = 0; pass < 2; pass++)
    {
        const bool shadowPass = (pass == 0);
        const float2 offset = shadowPass ? float2(0.f) : float2(2.f);
        const ImVec4& color = shadowPass ? c_Black : c_White;

        for (const KeynoteCounter& counter : m_Counters)
        {
            // deviation: SetWindowFontScale(fontSize / 51) in the binary; ImGui 1.92 sizes fonts directly.
            PushFontAtSize(font, counter.fontSize > 0.f ? counter.fontSize : c_CounterFontBaseSize);

            float x = counter.position.x;
            if (x < 0.f)
                x += float(width);
            float y = counter.position.y;
            if (y < 0.f)
                y += float(height);

            ImGui::SetCursorPosX(x - offset.x);
            ImGui::SetCursorPosY(y - offset.y);

            if (counter.type >= KeynoteCounter::MaxLodTriangles && counter.type <= KeynoteCounter::Fps)
            {
                if (m_UI.wireframe && counter.type == KeynoteCounter::Fps)
                {
                    ImGui::TextColored(color, "Wireframe Mode");
                }
                else
                {
                    const std::string value = FormatCounterValue(counter, averageFrameTime);
                    ImGui::TextColored(color, "%s: %s", counter.label.c_str(), value.c_str());
                }
            }

            ImGui::PopFont();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(1);
}
