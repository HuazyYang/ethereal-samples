// SPDX-License-Identifier: Apache-2.0
// Lucy raster depth -> Slang sparse-volume clipping -> graphics composition.
#include "DepthMap.h"
#include <GLFW/glfw3.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <stb_image_write.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace
{
using namespace nvrhi;
using namespace donut;
using namespace depth_map;
std::atomic_uint validationProblems{0};
void require(bool value, const char *text)
{
    if (!value)
        throw std::runtime_error(text);
}
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char *text) noexcept override
    {
        if (severity >= MessageSeverity::Warning)
            ++validationProblems;
        std::fprintf(stderr, "CUDA: %s\n", text);
    }
};
struct Options
{
    bool cuda = ETHEREAL_TEST_CUDA != 0, headless = false, selfTest = false, validation = false, exact = false;
    int graphicsAdapter = -1, cudaDevice = 0;
    uint32_t width = 1024, height = 768, frames = 0;
    float bias = 0;
    std::filesystem::path volume = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "explosion.vbx";
    std::filesystem::path mesh = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "lucy.obj", output;
};
uint32_t positive(const char *value)
{
    auto n = std::stoll(value);
    if (n <= 0 || n > UINT32_MAX)
        throw std::invalid_argument("Expected a positive 32-bit integer");
    return uint32_t(n);
}
void writeImage(const Images &image, const std::filesystem::path &output)
{
    if (!output.empty() && !stbi_write_png(output.string().c_str(), int(image.width), int(image.height), 4,
                                           image.color.data(), int(image.width * 4)))
        throw std::runtime_error("Cannot write depth-map screenshot");
}
class Pass : public app::IRenderPass
{
    NVRHI_INHERIT_INTERFACE_TABLE()
    Renderer m_renderer;
    Options m_options;
    dm::float3 m_cameraAngles{-40, 20, 0}, m_target{0, 0, 0}, m_lightAngles{299, 57.3f, 0}, m_lightTarget{0, 0, 0};
    dm::float3 m_volumeAngles{0, 0, 0}, m_translation{0, 0, -25};
    float m_distance = 400, m_lightDistance = 500;
    int m_drag = -1;
    bool m_shift = false;
    double m_mouseX = 0, m_mouseY = 0;
    uint32_t m_frames = 0;

  public:
    Pass(app::DeviceManager *manager, IDevice *compute, const Scene &scene, const Options &options)
        : IRenderPass(manager), m_renderer(GetDevice(), compute, scene), m_options(options)
    {
    }
    View view() const
    {
        auto v = defaultView();
        v.cameraToWorld = orbitCamera(m_cameraAngles, m_target, m_distance);
        v.light = orbitCamera(m_lightAngles, m_lightTarget, m_lightDistance).m_translation;
        v.indexToWorld = volumeTransform(m_volumeAngles, m_translation);
        v.exactDepth = m_options.exact;
        v.bias = m_options.bias;
        return v;
    }
    bool KeyboardUpdate(int key, int, int action, int) override
    {
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return false;
        switch (key)
        {
        case GLFW_KEY_W:
            m_translation.y += 1;
            break;
        case GLFW_KEY_S:
            m_translation.y -= 1;
            break;
        case GLFW_KEY_A:
            m_translation.x -= 1;
            break;
        case GLFW_KEY_D:
            m_translation.x += 1;
            break;
        case GLFW_KEY_Q:
            m_translation.z -= 1;
            break;
        case GLFW_KEY_E:
            m_translation.z += 1;
            break;
        case GLFW_KEY_LEFT:
            m_volumeAngles.y -= .5f;
            break;
        case GLFW_KEY_RIGHT:
            m_volumeAngles.y += .5f;
            break;
        case GLFW_KEY_UP:
            m_volumeAngles.x += .5f;
            break;
        case GLFW_KEY_DOWN:
            m_volumeAngles.x -= .5f;
            break;
        case GLFW_KEY_LEFT_BRACKET:
            m_options.bias = std::max(0.f, m_options.bias - .1f);
            break;
        case GLFW_KEY_RIGHT_BRACKET:
            m_options.bias = std::min(4.f, m_options.bias + .1f);
            break;
        case GLFW_KEY_P:
            m_options.exact = !m_options.exact;
            break;
        default:
            return false;
        }
        return true;
    }
    bool MouseButtonUpdate(int button, int action, int mods) override
    {
        if (button < GLFW_MOUSE_BUTTON_LEFT || button > GLFW_MOUSE_BUTTON_MIDDLE)
            return false;
        m_drag = action == GLFW_PRESS ? button : -1;
        m_shift = (mods & GLFW_MOD_SHIFT) != 0;
        return true;
    }
    bool MousePosUpdate(double x, double y) override
    {
        float dx = float(x - m_mouseX), dy = float(y - m_mouseY);
        auto &angles = m_shift ? m_lightAngles : m_cameraAngles;
        auto &distance = m_shift ? m_lightDistance : m_distance;
        if (m_drag == GLFW_MOUSE_BUTTON_LEFT)
        {
            angles.x += dx * .2f;
            angles.y = std::clamp(angles.y - dy * .2f, -89.9f, 89.9f);
        }
        else if (m_drag == GLFW_MOUSE_BUTTON_MIDDLE)
        {
            auto c = orbitCamera(m_cameraAngles, m_target, m_distance);
            m_target += (c.m_linear.row0 * dx - c.m_linear.row1 * dy) * (m_distance / 1000);
        }
        else if (m_drag == GLFW_MOUSE_BUTTON_RIGHT)
            distance = std::max(.1f, distance - dy);
        m_mouseX = x;
        m_mouseY = y;
        return m_drag >= 0;
    }
    bool MouseScrollUpdate(double, double y) override
    {
        m_distance = std::max(.1f, m_distance - float(y) * 10);
        return true;
    }
    void Animate(float) override
    {
        std::string title =
            std::string("depth-map | ") + (m_options.cuda ? "CUDA interop" : "graphics compute") +
            (m_options.exact ? " | exact depth" : " | legacy depth (bias inactive)") +
            " | mouse orbit/pan/distance, Shift light; WASDQE translate, arrows rotate, P depth mode, [] bias";
        GetDeviceManager()->SetInformativeWindowTitle(title.c_str());
    }
    void Render(IFramebuffer *framebuffer) override
    {
        const auto &info = framebuffer->getFramebufferInfo();
        auto current = view();
        m_renderer.render(info.width, info.height, current, framebuffer);
        if (m_options.frames && ++m_frames >= m_options.frames)
        {
            if (!m_options.output.empty())
            {
                m_renderer.render(info.width, info.height, current);
                writeImage(m_renderer.read(), m_options.output);
            }
            glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
        }
    }
};
} // namespace
int main(int argc, const char **argv)
{
    log::ConsoleApplicationMode();
    log::SetCallback([](log::Severity severity, const char *text) {
        if (severity >= log::Severity::Warning)
            ++validationProblems;
        std::fprintf(stderr, "%s\n", text);
    });
    try
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            auto argument = [&]() {
                if (++i == argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help"))
            {
                std::puts("depth-map [-dx11|-dx12|-vk] [--compute-api cuda|graphics] [--graphics-adapter N] "
                          "[--cuda-device N]\n"
                          "  [--headless] [--output image.png] [--frames N] [--width N] [--height N] [--validation] "
                          "[--self-test]\n"
                          "  [--volume explosion.vbx] [--mesh lucy.obj] [--depth-mode legacy|exact] [--bias N]\n"
                          "Window: mouse orbit/pan/distance; Shift adjusts light; WASDQE translates volume; arrows "
                          "rotate volume; P depth mode; [] exact-mode bias.\n"
                          "Default depth/composition reproduce the legacy sample; exact mode corrects off-axis "
                          "projection. CUDA is compute-only.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--compute-api"))
            {
                std::string v = argument();
                if (v != "cuda" && v != "graphics")
                    throw std::invalid_argument("--compute-api expects cuda or graphics");
                options.cuda = v == "cuda";
            }
            else if (!std::strcmp(argv[i], "--graphics-adapter"))
            {
                options.graphicsAdapter = std::stoi(argument());
                require(options.graphicsAdapter >= 0, "Negative graphics adapter");
            }
            else if (!std::strcmp(argv[i], "--cuda-device"))
            {
                options.cudaDevice = std::stoi(argument());
                require(options.cudaDevice >= 0, "Negative CUDA device");
            }
            else if (!std::strcmp(argv[i], "--width"))
                options.width = positive(argument());
            else if (!std::strcmp(argv[i], "--height"))
                options.height = positive(argument());
            else if (!std::strcmp(argv[i], "--frames"))
                options.frames = positive(argument());
            else if (!std::strcmp(argv[i], "--volume"))
                options.volume = argument();
            else if (!std::strcmp(argv[i], "--mesh"))
                options.mesh = argument();
            else if (!std::strcmp(argv[i], "--output") || !std::strcmp(argv[i], "--screenshot"))
                options.output = argument();
            else if (!std::strcmp(argv[i], "--headless"))
                options.headless = true;
            else if (!std::strcmp(argv[i], "--self-test"))
            {
                options.selfTest = true;
                options.headless = true;
            }
            else if (!std::strcmp(argv[i], "--validation") || !std::strcmp(argv[i], "--debug"))
                options.validation = true;
            else if (!std::strcmp(argv[i], "--bias"))
                options.bias = std::stof(argument());
            else if (!std::strcmp(argv[i], "--depth-mode"))
            {
                std::string v = argument();
                if (v != "legacy" && v != "exact")
                    throw std::invalid_argument("--depth-mode expects legacy or exact");
                options.exact = v == "exact";
            }
            else if (!std::strcmp(argv[i], "-cuda"))
                throw std::invalid_argument("Select a graphics API (-dx11/-dx12/-vk) and --compute-api cuda");
            else if (std::strcmp(argv[i], "-dx11") && std::strcmp(argv[i], "-dx12") && std::strcmp(argv[i], "-vk"))
                throw std::invalid_argument(std::string("Unknown option: ") + argv[i]);
        }
        require(uint64_t(options.width) * options.height <= 16 * 1024 * 1024, "Image exceeds 16 million pixels");
        require(std::isfinite(options.bias) && options.bias >= 0 && options.bias <= 4, "Bias must be finite in [0,4]");
        if (!options.headless && !options.output.empty() && !options.frames)
            options.frames = 8;
        DeviceHandle compute;
        auto messages = MAKE_RC_OBJ_PTR(Messages);
        InteropDeviceHandle cudaInterop;
        if (options.cuda)
        {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc d;
            d.deviceOrdinal = options.cudaDevice;
            d.enableOptiX = false;
            d.errorCB = messages;
            compute = cuda::createDevice(d);
            require(bool(compute), "Cannot create requested CUDA device");
            require(NVRHI_SUCCEEDED(cuda::nvrhiCudaCreateInteropDevice(compute, &cudaInterop)),
                    "Cannot create CUDA interoperability interface");
#else
            throw std::runtime_error("CUDA backend unavailable; use --compute-api graphics");
#endif
        }
        const auto api = app::GetGraphicsAPIFromCommandLine(argc, argv);
        auto manager = TakeOver(app::DeviceManager::Create(api));
        require(bool(manager), "Graphics API unavailable");
        app::DeviceCreationParameters params;
        params.adapterIndex = options.graphicsAdapter;
        params.headlessDevice = options.headless;
        params.enableDebugRuntime = options.validation;
        params.enableNvrhiValidationLayer = true;
        params.backBufferWidth = options.width;
        params.backBufferHeight = options.height;
        params.swapChainFormat = Format::RGBA8_UNORM;
        if (api == GraphicsAPI::VULKAN)
            params.requiredVulkanDeviceExtensions.push_back("VK_EXT_depth_clip_enable");
        if (options.cuda && api == GraphicsAPI::VULKAN)
        {
#ifdef _WIN32
            params.requiredVulkanDeviceExtensions.insert(
                params.requiredVulkanDeviceExtensions.end(),
                {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"});
#else
            params.requiredVulkanDeviceExtensions.insert(params.requiredVulkanDeviceExtensions.end(),
                                                         {"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd"});
#endif
        }
        if (options.cuda && options.graphicsAdapter < 0)
        {
            require(manager->CreateInstance(params), "Cannot enumerate graphics adapters");
            std::vector<app::AdapterInfo> adapters;
            require(manager->EnumerateAdapters(adapters), "Cannot enumerate graphics adapters");
            const auto &id = cudaInterop->getAdapterIdentity();
            for (size_t i = 0; i < adapters.size(); ++i)
            {
                const auto &a = adapters[i];
                if ((id.luidValid && a.luid && std::memcmp(id.luid, a.luid->data(), 8) == 0) ||
                    (id.uuidValid && a.uuid && std::memcmp(id.uuid, a.uuid->data(), 16) == 0))
                {
                    params.adapterIndex = int(i);
                    break;
                }
            }
            require(params.adapterIndex >= 0,
                    "No graphics adapter matches selected CUDA device; no host staging fallback");
        }
        require(options.headless ? manager->CreateHeadlessDevice(params)
                                 : manager->CreateWindowDeviceAndSwapChain(params, "GVDB depth-map Slang"),
                "Cannot create graphics device");
        if (!options.cuda)
            compute = manager->GetDevice();
        std::printf("depth-map graphics: %s; compute: %s; adapter index: %d\n", manager->GetRendererString(),
                    options.cuda ? "CUDA" : "graphics", params.adapterIndex);
        if (options.selfTest)
            runAnalyticTests(manager->GetDevice(), compute);
        else
        {
            auto scene = loadScene(options.volume, options.mesh);
            if (options.headless)
            {
                Renderer renderer(manager->GetDevice(), compute, scene);
                auto view = defaultView();
                view.exactDepth = options.exact;
                view.bias = options.bias;
                for (uint32_t i = 0; i < std::max(3u, options.frames); ++i)
                    renderer.render(options.width, options.height, view);
                auto image = renderer.read();
                size_t meshPixels = 0, volumePixels = 0, clipped = 0;
                for (size_t i = 0; i < image.depth.size(); ++i)
                {
                    meshPixels += image.depth[i] < 1;
                    volumePixels += (image.volume[i] >> 24) > 0;
                }
                require(meshPixels && volumePixels,
                        "Reference scene must contain visible Lucy mesh and explosion volume");
                renderer.setMesh({});
                renderer.render(options.width, options.height, view);
                auto unclipped = renderer.read();
                for (size_t i = 0; i < image.volume.size(); ++i)
                    clipped += (unclipped.volume[i] >> 24) > (image.volume[i] >> 24);
                require(clipped > 0, "Raster depth did not clip any volume pixels");
                writeImage(image, options.output);
                std::printf("PASS Lucy/explosion: %zu mesh pixels, %zu volume pixels, %zu depth-clipped pixels\n",
                            meshPixels, volumePixels, clipped);
            }
            else
            {
                Pass pass(manager, compute, scene, options);
                manager->AddRenderPassToBack(&pass);
                manager->RunMessageLoop();
                manager->RemoveRenderPass(&pass);
            }
        }
        compute->waitForIdle();
        manager->GetDevice()->waitForIdle();
        cudaInterop = nullptr;
        compute = nullptr;
        manager->Shutdown();
        require(validationProblems == 0, "Native/NVRHI validation reported warnings or errors");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "depth-map: %s\n", error.what());
        return 1;
    }
}
