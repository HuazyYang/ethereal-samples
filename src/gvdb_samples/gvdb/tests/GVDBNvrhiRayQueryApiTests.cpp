// SPDX-License-Identifier: Apache-2.0
// Public-only API fixture. Expected lookup scans explicit leaf boxes, not the GPU tree.
#include "RayQueryApiFixture.h"
#include <cmath>
#include <limits>
#include <ShaderToolArtifact.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) {
    if (!condition)
        throw std::runtime_error(text);
}
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        int adapter = -1;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i], "-cuda");
            validation |= !std::strcmp(argv[i], "--validation");
            if (!std::strcmp(argv[i], "--adapter")) {
                require(++i < argc, "--adapter requires an ordinal");
                const std::string text = argv[i];
                size_t consumed = 0;
                adapter = std::stoi(text, &consumed);
                require(adapter >= 0 && consumed == text.size(), "invalid adapter ordinal");
            }
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            desc.deviceOrdinal = adapter < 0 ? 0 : adapter;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(
                donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.adapterIndex = adapter;
            desc.enableNvrhiValidationLayer = true;
            desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
        if (manager)
            std::printf("Ray query backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Ray query backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
    }
    ~Device() {
        if (device)
            device->waitForIdle();
        device = nullptr;
        if (manager)
            manager->Shutdown();
    }
};
template <class T>
void queryIdentity(T* object) {
    const auto count = object->AddRef();
    object->Release();
    AutoPtr<T> same;
    check(object->QueryInterface(uuid_of<T>(), reinterpret_cast<void**>(same.GetAddressOf())),
          "self QueryInterface");
    require(same.Get() == object, "self identity");
    same = nullptr;
    AutoPtr<IObject> identity, again;
    check(object->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(identity.GetAddressOf())),
          "base identity");
    check(identity->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(again.GetAddressOf())),
          "repeat base identity");
    require(identity == again, "canonical IObject identity");
    identity = nullptr;
    again = nullptr;
    AutoPtr<IRHIObject> resource;
    check(object->QueryInterface(uuid_of<IRHIObject>(), reinterpret_cast<void**>(resource.GetAddressOf())),
          "RHI ancestor");
    resource = nullptr;
    void* missing = reinterpret_cast<void*>(uintptr_t(1));
    require(object->QueryInterface(uuid_of<IDevice>(), &missing) == FE_NOINTERFACE && !missing,
            "unsupported IID clears output");
    require(object->QueryInterface(uuid_of<T>(), nullptr) == FS_OK, "query-only IID probe");
    require(object->AddRef() == count, "queries preserve reference balance");
    object->Release();
}

struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* d)
        : device(d) {
        check(device->createCommandList({}, &commands), "create command list");
    }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto serial = device->executeCommandList(commands);
        require(serial || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit commands");
        require(device->waitForIdle(), "caller GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(IBuffer* buffer, uint64_t size) {
        if (!size)
            return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(size)
                                       .setCpuAccess(CpuAccessMode::Read)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "create test readback");
        begin();
        commands->copyBuffer(staging, 0, buffer, 0, size);
        submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data, "map canonical GPU stream");
        std::vector<uint8_t> result(size_t(size), uint8_t(0));
        std::memcpy(result.data(), data, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};
std::vector<char> rewriteArtifact(
    const std::vector<char>& source,
    const std::function<void(ShaderToolArtifactEntry&, std::vector<ShaderToolArtifactBinding>&)>& modify) {
    ShaderToolArtifactView view;
    require(ShaderToolArtifactOpen(source.data(), source.size(), &view) == 0, "open accepted artifact");
    ShaderToolArtifactInfo info;
    ShaderToolArtifactEntry entry;
    require(ShaderToolArtifactGetInfo(&view, &info) == 0 && info.EntryCount == 1 &&
                ShaderToolArtifactGetEntry(&view, 0, &entry) == 0,
            "single entry artifact");
    std::vector<ShaderToolArtifactBinding> bindings(info.BindingCount);
    for (uint32_t i = 0; i < info.BindingCount; ++i)
        require(ShaderToolArtifactGetBinding(&view, i, &bindings[i]) == 0, "artifact reflection");
    modify(entry, bindings);
    ShaderToolArtifactDesc desc{};
    desc.Target = info.Target;
    desc.Flags = info.Flags;
    desc.Compiler = info.Compiler;
    desc.Entries = &entry;
    desc.EntryCount = 1;
    desc.Bindings = bindings.data();
    desc.BindingCount = uint32_t(bindings.size());
    size_t count = 0;
    require(ShaderToolArtifactWrite(&desc, nullptr, 0, &count) == 0, "size altered valid container");
    std::vector<char> result(count);
    require(ShaderToolArtifactWrite(&desc, result.data(), result.size(), &count) == 0,
            "write altered valid container");
    require(ShaderToolArtifactOpen(result.data(), result.size(), &view) == 0,
            "altered artifact structurally valid");
    return result;
}

void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& artifact) {
    g::RayQueryCreateDesc d;
    d.device = device;
    d.artifact = artifact.data();
    d.artifactBytes = artifact.size();
    g::RayQueryHandle op;
    check(g::createRayQuery(d, &op), "ray factory");
    queryIdentity(op.Get());
    for (unsigned test = 0; test < 4; ++test) {
        auto changed = rewriteArtifact(artifact, [&](auto& entry, auto& bindings) {
            if (test == 0)
                entry.ThreadGroupSize[0] = 128;
            else
                for (auto& b : bindings) {
                    if (test == 1 && b.Scope == ST_SCOPE_GLOBAL && b.RegisterIndex == 4)
                        b.ElementStride = 4;
                    if (test == 2 && b.Scope == ST_SCOPE_CONSTANT_BUFFER) {
                        b.Offset += 4;
                        break;
                    }
                    if (test == 3 && b.Scope == ST_SCOPE_GLOBAL) {
                        b.RegisterSpace = 1;
                        break;
                    }
                }
        });
        d.artifact = changed.data();
        d.artifactBytes = changed.size();
        auto* invalid = reinterpret_cast<g::IGVDBRayQuery*>(uintptr_t(1));
        require(NVRHI_FAILED(g::createRayQuery(d, &invalid)) && !invalid, "ray malformed reflection rejected");
    }
    ray_query_fixture::run(device, remap, artifact);
}
}  // namespace
int main(int argc, const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    });
    try {
        {
            Device device(argc, argv);
            auto root = donut::app::GetDirectoryWithExecutable() / "shaders";
            const auto backend = donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            auto load = [](const auto& path) {
                std::ifstream f(path, std::ios::binary);
                require(bool(f), "ray artifact file");
                return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            };
            run(device.device, load(root / "gvdb_owner" / backend / "GVDBAtlasRepack_relocateAtlas.bin"),
                load(root / "gvdb_ray_query" / backend / "GVDBRayRecords_queryRayRecords.bin"));
        }
        require(problems == 0, "ray native validation warnings/errors");
        std::puts(
            "PASS public ray records: retained snapshots, legacy bytes, analytic rays, limits, budgets, "
            "reflection");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
