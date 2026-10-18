// SPDX-License-Identifier: Apache-2.0
// Independent public-only dense downsample acceptance; no concrete owner/core headers.
#include "GVDBNvrhiDownsampleApi.h"
#include "DownsampleApiGoldens.h"
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
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
            std::printf("Downsample backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Downsample backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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

namespace f = downsample_fixture;
g::DownsampleHandle operation(IDevice* device, const std::vector<char>& artifact) {
    g::DownsampleCreateDesc desc;
    desc.device = device;
    desc.artifact = artifact.data();
    desc.artifactBytes = artifact.size();
    g::DownsampleHandle result;
    check(g::createDownsample(desc, &result), "create downsample");
    require(result->getDevice() == device, "factory retains correct device");
    return result;
}
void verify(g::BorrowedBytes result, const f::Golden& golden) {
    require(result.byteCount == golden.expected.size() * 4 && (!result.byteCount || result.data),
            "dense output byte span");
    for (size_t i = 0; i < golden.expected.size(); ++i) {
        float actual;
        std::memcpy(&actual, static_cast<const uint8_t*>(result.data) + 4 * i, 4);
        const auto expected = golden.expected[i];
        bool valid = false;
        switch (expected.kind) {
            case f::Expected::Exact:
                valid = f::bits(actual) == expected.bitPattern;
                break;
            case f::Expected::NaN:
                valid = std::isnan(actual);
                break;
            case f::Expected::PositiveInfinity:
                valid = std::isinf(actual) && actual > 0;
                break;
            case f::Expected::NegativeInfinity:
                valid = std::isinf(actual) && actual < 0;
                break;
        }
        if (!valid) {
            std::fprintf(stderr, "%s output%zu got %.9g bits%08x expectedKind%d bits%08x\n",
                         golden.name.c_str(), i, actual, f::bits(actual), int(expected.kind),
                         expected.bitPattern);
            throw std::runtime_error("independent downsample golden");
        }
        if (golden.name.find("strict .05 threshold") != std::string::npos)
            require((actual > .05f) == (expected.bitPattern > 0x3d4ccccdu),
                    "strict .05 classification changed");
    }
}
void preserveFailure(g::BorrowedBytes value) {
    require(value.data == reinterpret_cast<const void*>(uintptr_t(1)) && value.byteCount == 73,
            "failed access changed borrowed result");
}
g::BorrowedBytes sentinel() { return {reinterpret_cast<const void*>(uintptr_t(1)), 73}; }
void goldens(Harness& h, g::IGVDBDownsample* op) {
    const auto cases = f::goldens();
    std::vector<g::DownsampleTicketHandle> tickets;
    for (const auto& golden : cases) {
        // Deliberately unaligned source and trailing poison are accepted as CPU bytes.
        std::vector<uint8_t> source(golden.inputBytes() + 2, 0xa5);
        std::memcpy(source.data() + 1, golden.source.data(), golden.inputBytes());
        auto desc = golden.request();
        desc.sourceData = source.data() + 1;
        desc.sourceBytes = golden.inputBytes() + 1;
        g::DownsamplePreparationBudget budget;
        budget.maxInputBytes = golden.inputBytes();
        budget.maxSampleVisits = golden.conservativeVisits();
        budget.maxAllocatedBytes = 256 + golden.inputBytes() + 16 * golden.expected.size();
        budget.maxBuffers = 4;
        g::DownsampleTicketHandle ticket;
        check(op->prepare(desc, budget, &ticket), "prepare exact budget golden");
        require(ticket->getDevice() == h.device, "ticket device");
        const auto resolution = ticket->getDestinationResolution();
        require(resolution.x == desc.destinationResolution.x && resolution.y == desc.destinationResolution.y &&
                    resolution.z == desc.destinationResolution.z,
                "destination resolution metadata");
        auto absent = sentinel();
        require(NVRHI_FAILED(ticket->getValues(absent)), "read result before finish");
        preserveFailure(absent);
        absent = sentinel();
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(absent)), "finish before record");
        preserveFailure(absent);
        std::fill(source.begin(), source.end(), 0xff);  // Independent deep copy, destroyed before record.
        tickets.push_back(std::move(ticket));
    }
    queryIdentity(op);
    queryIdentity(tickets.front().Get());
    h.begin();
    for (auto& ticket : tickets)
        check(op->record(h.context(), ticket), "record golden");
    require(NVRHI_FAILED(op->record(h.context(), tickets.front())), "second record accepted");
    h.submit();
    for (size_t reverse = tickets.size(); reverse > 0; --reverse) {
        auto& ticket = tickets[reverse - 1];
        g::BorrowedBytes values;
        check(ticket->finishAfterCallerCompletion(values), "explicit completion in reverse preparation order");
        verify(values, cases[reverse - 1]);
        auto again = sentinel();
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(again)), "second completion accepted");
        preserveFailure(again);
        check(ticket->getValues(again), "repeat immutable result access");
        require(again.data == values.data && again.byteCount == values.byteCount,
                "result lifetime/pointer stability");
        verify(again, cases[reverse - 1]);
    }
    std::puts(
        "PASS independent regions, reflection/shear/truncation, serial float sums, strict threshold and IEEE "
        "results");
}
void descriptorFailures(g::IGVDBDownsample* op) {
    const auto golden = f::goldens().front();
    const auto good = golden.request();
    auto reject = [&](g::DownsampleRequestDesc desc, g::DownsamplePreparationBudget budget = {}) {
        auto* output = reinterpret_cast<g::IGVDBDownsampleTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(op->prepare(desc, budget, &output)) && !output,
                "invalid request must fail and clear ticket");
    };
    require(op->prepare(good, {}, nullptr) == FE_INVALID_ARGS, "null ticket output");
    for (uint32_t i = 0; i < 3; ++i) {
        auto bad = good;
        bad.reserved[i] = 1;
        reject(bad);
    }
    {
        auto bad = good;
        --bad.structSize;
        reject(bad);
        bad = good;
        bad.reserved2 = 1;
        reject(bad);
        bad = good;
        bad.reserved3 = 1;
        reject(bad);
    }
    {
        auto bad = good;
        bad.sourceData = nullptr;
        reject(bad);
        bad = good;
        --bad.sourceBytes;
        reject(bad);
    }
    for (uint32_t axis = 0; axis < 3; ++axis) {
        auto bad = good;
        (&bad.sourceResolution.x)[axis] = 0;
        reject(bad);
        bad = good;
        (&bad.sourceResolution.x)[axis] = UINT32_MAX;
        bad.sourceBytes = UINT64_MAX;
        reject(bad);
        bad = good;
        (&bad.destinationResolution.x)[axis] = 65535u * 8u + 1u;
        reject(bad);
        bad = good;
        (&bad.destinationExtent.x)[axis] = 0;
        reject(bad);
        bad = good;
        (&bad.destinationExtent.x)[axis] = -1;
        reject(bad);
    }
    {
        auto bad = good;
        bad.sourceResolution = {65536, 65536, 1};
        bad.sourceBytes = UINT64_MAX;
        reject(bad);
        bad = good;
        bad.destinationResolution = {65535u * 8u, 65535u * 8u, 1};
        reject(bad);
        bad = good;
        bad.sourceResolution = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
        bad.sourceBytes = UINT64_MAX;
        reject(bad);
        bad = good;
        bad.destinationExtent.x = std::numeric_limits<float>::max();
        for (auto& row : bad.transformRows)
            row = {};
        reject(bad);  // The endpoint multiply must remain safe even with zero affine coefficients.
    }
    for (uint32_t component = 0; component < 4; ++component) {
        auto bad = good;
        (&bad.destinationExtent.x)[component] = std::numeric_limits<float>::quiet_NaN();
        reject(bad);
        bad = good;
        (&bad.remap.x)[component] = std::numeric_limits<float>::infinity();
        reject(bad);
        for (uint32_t row = 0; row < 3; ++row) {
            bad = good;
            (&bad.transformRows[row].x)[component] = std::numeric_limits<float>::quiet_NaN();
            reject(bad);
        }
    }
    {
        auto bad = good;
        bad.remap.y = bad.remap.x;
        reject(bad);
        bad = good;
        bad.remap = {-std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0, 1};
        reject(bad);
    }
    for (uint32_t axis = 0; axis < 3; ++axis) {
        auto bad = good;
        bad.transformRows[axis].w = 2147483648.f;
        reject(bad);
        bad = good;
        bad.transformRows[axis].w = -2147483648.f;
        reject(bad);
        bad = good;
        (&bad.transformRows[axis].x)[axis] = std::numeric_limits<float>::max();
        reject(bad);
    }
    // A conservative work limit is independent of the much smaller actual sample count.
    g::DownsamplePreparationBudget exact;
    exact.maxInputBytes = 32;
    exact.maxAllocatedBytes = 336;
    exact.maxSampleVisits = 24;
    exact.maxBuffers = 4;
    auto limited = exact;
    --limited.maxInputBytes;
    reject(good, limited);
    limited = exact;
    --limited.maxAllocatedBytes;
    reject(good, limited);
    limited = exact;
    --limited.maxSampleVisits;
    reject(good, limited);
    limited = exact;
    --limited.maxBuffers;
    reject(good, limited);
    limited = exact;
    limited.reserved = 1;
    reject(good, limited);
    std::puts("PASS descriptor, coordinate/address overflow and exact conservative/allocation budget failures");
}
void emptyOutputs(Harness& h, g::IGVDBDownsample* op) {
    const auto golden = f::goldens().front();
    const g::UInt3 dimensions[] = {
        {0, 3, 2},
        {3, 0, 2},
        {3, 2, 0},
        {0, 0, 0}
    };
    for (auto size : dimensions) {
        auto request = golden.request();
        request.destinationResolution = size;
        // Valid span, intentionally unreadable address: empty preparation must not dereference it.
        request.sourceData = reinterpret_cast<const void*>(uintptr_t(1));
        g::DownsamplePreparationBudget zero{0, 0, 0, 0, 0};
        g::DownsampleTicketHandle ticket;
        check(op->prepare(request, zero, &ticket), "zero-output validation-only preparation");
        const auto resolution = ticket->getDestinationResolution();
        require(resolution.x == size.x && resolution.y == size.y && resolution.z == size.z,
                "empty resolution metadata");
        auto values = sentinel();
        require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(values)), "empty finish before record");
        preserveFailure(values);
        h.begin();
        check(op->record(h.context(), ticket), "record allocation-free empty");
        require(NVRHI_FAILED(op->record(h.context(), ticket)), "empty second record");
        h.submit();
        check(ticket->finishAfterCallerCompletion(values), "finish empty");
        require(!values.data && !values.byteCount, "canonical empty output");
        check(ticket->getValues(values), "read empty result again");
        require(!values.data && !values.byteCount, "empty borrowed bytes");
        auto reject = [&](g::DownsampleRequestDesc bad) {
            auto* out = reinterpret_cast<g::IGVDBDownsampleTicket*>(uintptr_t(1));
            require(NVRHI_FAILED(op->prepare(bad, zero, &out)) && !out, "empty still validates descriptors");
        };
        auto bad = request;
        bad.sourceData = nullptr;
        reject(bad);
        bad = request;
        bad.sourceBytes = 0;
        reject(bad);
        bad = request;
        bad.sourceResolution.y = 0;
        reject(bad);
        bad = request;
        bad.destinationExtent.w = std::numeric_limits<float>::infinity();
        reject(bad);
        bad = request;
        bad.transformRows[2].w = std::numeric_limits<float>::quiet_NaN();
        reject(bad);
        bad = request;
        bad.remap.y = bad.remap.x;
        reject(bad);
        bad = request;
        bad.reserved[2] = 1;
        reject(bad);
        bad = request;
        bad.destinationResolution.x = 65535u * 8u + 1;
        bad.destinationResolution.y = 0;
        reject(bad);
    }
    std::puts("PASS empty dimensions: validation, no source dereference, zero budgets and one-use tickets");
}
void lifetimes(Harness& h, const std::vector<char>& artifact) {
    auto first = operation(h.device, artifact), second = operation(h.device, artifact);
    const auto golden = f::goldens().front();
    g::DownsampleTicketHandle ticket;
    check(first->prepare(golden.request(), {}, &ticket), "prepare authentication fixture");
    h.begin();
    require(NVRHI_FAILED(second->record(h.context(), ticket)), "foreign factory consumed ticket");
    require(NVRHI_FAILED(first->record({}, ticket)), "null recording context");
    require(NVRHI_FAILED(first->record({nullptr, h.commands}, ticket)), "missing context device");
    require(NVRHI_FAILED(first->record({h.device, nullptr}, ticket)), "null command list");
    require(NVRHI_FAILED(first->record(h.context(), nullptr)), "null ticket");
    check(first->record(h.context(), ticket), "foreign rejection preserves correct record");
    first = nullptr;
    second = nullptr;
    h.submit();
    g::BorrowedBytes values;
    check(ticket->finishAfterCallerCompletion(values), "factory-free completion");
    verify(values, golden);
    std::vector<uint8_t> retained(values.byteCount);
    std::memcpy(retained.data(), values.data, retained.size());
    auto another = operation(h.device, artifact);
    auto otherGolden = golden;
    otherGolden.source.assign(8, 91);
    otherGolden.expected = {91.f, 91.f, 91.f};
    g::DownsampleTicketHandle other;
    check(another->prepare(otherGolden.request(), {}, &other), "second result input");
    h.begin();
    check(another->record(h.context(), other), "second independent record");
    h.submit();
    g::BorrowedBytes next;
    check(other->finishAfterCallerCompletion(next), "second finish");
    verify(next, otherGolden);
    require(!std::memcmp(retained.data(), values.data, retained.size()),
            "another operation overwrote retained result");
    std::puts("PASS factory authentication and independent results retained after operator release");
}
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
void artifactFailures(IDevice* device, const std::vector<char>& artifact) {
    const char* labels[] = {
        "random container",       "truncated container",   "missing logical name and symbol",
        "thread group shape",     "entry parameter bytes", "constant stride",
        "constant member offset", "source view kind",      "statistics stride",
        "register space",         "resource array count"};
    uint32_t mutation = 0;
    auto rejects = [&](const std::vector<char>& bytes) {
        const char* label = labels[mutation++];
        g::DownsampleCreateDesc desc;
        desc.device = device;
        desc.artifact = bytes.data();
        desc.artifactBytes = bytes.size();
        auto* output = reinterpret_cast<g::IGVDBDownsample*>(uintptr_t(1));
        const auto status = g::createDownsample(desc, &output);
        if (NVRHI_SUCCEEDED(status) || output) {
            if (NVRHI_SUCCEEDED(status) && output)
                output->Release();
            throw std::runtime_error(std::string("incompatible artifact accepted: ") + label);
        }
    };
    rejects(std::vector<char>{'b', 'a', 'd'});
    rejects(std::vector<char>(artifact.begin(), artifact.begin() + artifact.size() / 2));
    // Entry lookup intentionally accepts either its logical name or exported symbol.
    rejects(rewriteArtifact(artifact, [](auto& e, auto&) {
        e.Name = "notTheRequiredDownsampleEntry";
        e.Symbol = "notTheRequiredDownsampleEntry";
    }));
    rejects(rewriteArtifact(artifact, [](auto& e, auto&) { e.ThreadGroupSize[0] = 4; }));
    rejects(rewriteArtifact(artifact, [](auto& e, auto&) {
        e.EntryParamsSize = 16;
        e.EntryParamsAlignment = 16;
    }));
    auto alter = [&](const std::function<bool(ShaderToolArtifactBinding&)>& edit) {
        rejects(rewriteArtifact(artifact, [&](auto&, auto& bindings) {
            bool changed = false;
            for (auto& b : bindings)
                if (!changed)
                    changed = edit(b);
            require(changed, "find artifact mutation target");
        }));
    };
    alter([](auto& b) {
        if (b.Scope == ST_SCOPE_GLOBAL && b.Kind == ST_BINDING_CONSTANT_BUFFER) {
            b.ElementStride += 16;
            return true;
        }
        return false;
    });
    alter([](auto& b) {
        if (b.Scope == ST_SCOPE_CONSTANT_BUFFER) {
            b.Offset += 4;
            return true;
        }
        return false;
    });
    alter([](auto& b) {
        if (b.Scope == ST_SCOPE_GLOBAL && b.Kind == ST_BINDING_STRUCTURED_BUFFER) {
            b.Kind = ST_BINDING_BYTE_ADDRESS_BUFFER;
            b.ElementStride = 1;
            return true;
        }
        return false;
    });
    alter([](auto& b) {
        if (b.Scope == ST_SCOPE_GLOBAL && b.Kind == ST_BINDING_RW_STRUCTURED_BUFFER) {
            b.ElementStride += 4;
            return true;
        }
        return false;
    });
    alter([](auto& b) {
        if (b.Scope == ST_SCOPE_GLOBAL) {
            b.RegisterSpace = 1;
            return true;
        }
        return false;
    });
    alter([](auto& b) {
        if (b.Scope == ST_SCOPE_GLOBAL && b.Kind != ST_BINDING_CONSTANT_BUFFER) {
            b.Count = 2;
            return true;
        }
        return false;
    });
    std::puts("PASS malformed containers and structurally valid incompatible reflected artifacts");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open downsample artifact");
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
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
            Harness h(device.device);
            auto artifactPath = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_downsample" /
                                donut::app::GetShaderTypeName(device.device->getGraphicsAPI()) /
                                "GVDBDenseDownsample_downsampleDenseRegion.bin";
            for (int i = 1; i + 1 < argc; ++i)
                if (!std::strcmp(argv[i], "--artifact"))
                    artifactPath = argv[++i];
            const auto artifact = readArtifact(artifactPath);
            auto op = operation(device.device, artifact);
            descriptorFailures(op);
            goldens(h, op);
            emptyOutputs(h, op);
            lifetimes(h, artifact);
            artifactFailures(device.device, artifact);
        }
        require(problems == 0, "native/NVRHI validation warnings or errors");
        std::puts("Public regional downsample acceptance PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
