// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiPointQuantizeApi.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <ShaderToolArtifact.h>
#include <functional>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
            std::printf("Quantize backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Quantize backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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

std::vector<char> artifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "Open quantization artifact");
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
class ForeignSet final : public ObjectImpl<g::IGVDBPointSet> {
    IDevice* m_device;

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignSet)
    NVRHI_IMPLEMENTS_INTERFACE(g::IGVDBPointSet)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit ForeignSet(IDevice* device)
        : m_device(device) {}
    IDevice* getDevice() const noexcept override { return m_device; }
    g::PointSetInfo getDesc() const noexcept override { return {}; }
    FRESULT getStream(g::PointAttribute, g::BorrowedPointStream& out) const noexcept override {
        out = {};
        return FE_UNSUPPORTED;
    }
    FRESULT getData(g::PointAttribute, g::BorrowedBytes& out) const noexcept override {
        out = {};
        return FE_UNSUPPORTED;
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
void artifactFailures(const g::PointQuantizeCreateDesc& accepted, const std::vector<char>& encode,
                      const std::vector<char>& decode) {
    for (unsigned stage = 0; stage < 2; ++stage) {
        const auto& source = stage ? decode : encode;
        auto rejects = [&](const std::vector<char>& bytes) {
            auto desc = accepted;
            if (stage) {
                desc.decodeArtifact = bytes.data();
                desc.decodeArtifactBytes = bytes.size();
            } else {
                desc.encodeArtifact = bytes.data();
                desc.encodeArtifactBytes = bytes.size();
            }
            auto* output = reinterpret_cast<g::IGVDBPointQuantize*>(uintptr_t(1));
            require(NVRHI_FAILED(g::createPointQuantize(desc, &output)) && !output,
                    "Incompatible quantization artifact rejected");
        };
        rejects({});
        rejects({'b', 'a', 'd'});
        rejects(rewriteArtifact(source, [](auto& e, auto&) { e.ThreadGroupSize[0] = 128; }));
        rejects(rewriteArtifact(source, [](auto& e, auto&) { e.Name = e.Symbol = "wrongEntry"; }));
        rejects(rewriteArtifact(source, [](auto&, auto& b) { b[0].ElementStride += 16; }));
        rejects(rewriteArtifact(source, [](auto&, auto& bindings) {
            for (auto& b : bindings)
                if (b.Scope == ST_SCOPE_CONSTANT_BUFFER) {
                    b.Offset += 4;
                    break;
                }
        }));
        rejects(rewriteArtifact(source, [](auto&, auto& bindings) {
            for (auto& b : bindings)
                if (b.Scope == ST_SCOPE_GLOBAL && b.Kind == ST_BINDING_RW_BYTE_ADDRESS_BUFFER) {
                    b.RegisterIndex = 9;
                    break;
                }
        }));
    }
}
void run(IDevice* device, uint32_t count, uint32_t components, bool padded, bool velocity = false) {
    const auto selected = velocity ? g::PointAttribute::Velocity : g::PointAttribute::Position;
    const uint32_t selectedIndex = uint32_t(selected);
    const auto factory = velocity ? g::createVelocityQuantize : g::createPointQuantize;
    Harness h(device);
    const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
    const auto target = donut::app::GetShaderTypeName(device->getGraphicsAPI());
    const auto remap = artifact(base / "gvdb_point_cloud" / target / "GVDBAtlasRepack_relocateAtlas.bin");
    const auto conversion =
        artifact(base / "gvdb_point_cloud" / target / "GVDBPointStreamConversion_convertPointStream.bin");
    const auto encode = artifact(base / "gvdb_quantize" / target / "GVDBPointQuantize_encodePointU16.bin");
    const auto decode = artifact(base / "gvdb_quantize" / target / "GVDBPointQuantize_decodePointU16.bin");
    g::VolumeCreateDesc volumeDesc;
    volumeDesc.device = device;
    volumeDesc.configuration.levelCount = 4;
    volumeDesc.configuration.logDimension = {2, 2, 2, 2};
    volumeDesc.remapArtifact = remap.data();
    volumeDesc.remapArtifactBytes = remap.size();
    g::VolumeHandle volume;
    check(g::createVolume(volumeDesc, &volume), "Create point source owner");
    g::PointOpsCreateDesc pointCreate;
    pointCreate.volume = volume;
    pointCreate.conversionArtifact = conversion.data();
    pointCreate.conversionArtifactBytes = conversion.size();
    g::PointOpsHandle points;
    check(g::createPointOps(pointCreate, &points), "Create point source operator");
    std::vector<g::Float4> positions(count), velocities(count);
    std::vector<uint32_t> colors(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w = i % 2 ? 0x80000000u : 0x7fc00123u;
        float extra;
        std::memcpy(&extra, &w, 4);
        positions[i] = {float(int(i % 17) - 8) / 2, float(i % 9), float(int(i % 11) - 5), extra};
        velocities[i] = {float(i) - 2.f, velocity ? -7.f : extra, -float(i), extra};
        colors[i] = 0xff000000u | (i * 997u & 0x00ffffffu);
    }
    g::PointSetDesc sourceDesc;
    sourceDesc.positions.stream = {positions.data(),           positions.size() * 16,     0, 16, count,
                                   g::PointComponent::Float32, velocity ? 4u : components};
    sourceDesc.velocities.stream = {velocities.data(),          velocities.size() * 16,    0, 16, count,
                                    g::PointComponent::Float32, velocity ? components : 4u};
    sourceDesc.colors.stream = {colors.data(), colors.size() * 4, 0, 4, count, g::PointComponent::UInt8, 4};
    g::PointUploadTicketHandle upload;
    check(points->preparePoints(sourceDesc, {}, &upload), "Prepare quantization source");
    h.begin();
    check(points->recordPoints(h.context(), upload), "Record source conversion");
    h.submit();
    g::PointSetHandle original;
    check(upload->finishAfterCallerCompletion(&original), "Complete source conversion");
    upload = nullptr;
    std::array<std::vector<uint8_t>, 3> originalBytes;
    for (uint32_t i = 0; i < 3; ++i)
        if (original->getDesc().attributeMask & (1u << i)) {
            g::BorrowedBytes bytes;
            check(original->getData(g::PointAttribute(i), bytes), "Original canonical bytes");
            if (bytes.byteCount)
                originalBytes[i].assign(static_cast<const uint8_t*>(bytes.data),
                                        static_cast<const uint8_t*>(bytes.data) + bytes.byteCount);
        }
    g::PointQuantizeCreateDesc create;
    create.device = device;
    create.encodeArtifact = encode.data();
    create.encodeArtifactBytes = encode.size();
    create.decodeArtifact = decode.data();
    create.decodeArtifactBytes = decode.size();
    if (count == 1 && components == 3 && !padded)
        artifactFailures(create, encode, decode);
    g::PointQuantizeHandle operation, other;
    check(factory(create, &operation), "Create quantizer");
    check(factory(create, &other), "Create foreign quantizer");
    queryIdentity(operation.Get());
    if (velocity && count == 1 && components == 3 && !padded) {
        for (unsigned invalid = 0; invalid < 3; ++invalid) {
            auto badSource = sourceDesc;
            auto badVelocities = velocities;
            if (invalid == 0)
                badSource.velocities = {};
            else {
                badVelocities[0].x = invalid == 1 ? INFINITY : std::numeric_limits<float>::quiet_NaN();
                badSource.velocities.stream.data = badVelocities.data();
            }
            g::PointUploadTicketHandle badUpload;
            check(points->preparePoints(badSource, {}, &badUpload), "Prepare absent/nonfinite velocity input");
            h.begin();
            check(points->recordPoints(h.context(), badUpload), "Record invalid velocity input");
            h.submit();
            g::PointSetHandle badSet;
            check(badUpload->finishAfterCallerCompletion(&badSet), "Retain canonical invalid velocity");
            g::PointQuantizeRequestDesc badRequest;
            badRequest.points = badSet;
            auto* rejected = reinterpret_cast<g::IGVDBPointQuantizeTicket*>(uintptr_t(1));
            const auto status = operation->prepare(badRequest, {}, &rejected);
            require(status == (invalid == 0 ? FE_NOT_FOUND : FE_INVALID_ARGS) && !rejected,
                    "Missing/nonfinite velocity rejected before recording");
        }
    }
    g::PointQuantizeRequestDesc request;
    request.points = original;
    request.minimum = {-2, 0, -4, 0};
    request.range = {4, 8, 16, 0};
    if (padded)
        request.bounds =
            velocity ? g::PointQuantizationBounds::AttributeExtrema : g::PointQuantizationBounds::LegacyPadded;
    if (velocity && !count) {
        g::PointQuantizeTicketHandle empty;
        require(operation->prepare(request, {}, &empty) == FE_NOT_FOUND && !empty,
                "Empty uploaded PointSet has no optional velocity attribute");
        return;
    }
    auto rejects = [&](g::PointQuantizeRequestDesc bad, g::PointQuantizePreparationBudget budget = {}) {
        auto* output = reinterpret_cast<g::IGVDBPointQuantizeTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation->prepare(bad, budget, &output)) && !output,
                "Invalid quantize preparation clears output");
    };
    auto foreign = MAKE_RC_OBJ_PTR(ForeignSet, device);
    auto bad = request;
    bad.points = foreign;
    rejects(bad);
    bad = request;
    --bad.structSize;
    rejects(bad);
    bad = request;
    bad.reserved = 1;
    rejects(bad);
    bad = request;
    bad.reserved2 = 1;
    rejects(bad);
    bad = request;
    bad.points = nullptr;
    rejects(bad);
    bad = request;
    bad.bounds = g::PointQuantizationBounds(3);
    rejects(bad);
    bad = request;
    bad.bounds =
        velocity ? g::PointQuantizationBounds::LegacyPadded : g::PointQuantizationBounds::AttributeExtrema;
    rejects(bad);
    bad = request;
    bad.minimum.w = INFINITY;
    rejects(bad);
    bad = request;
    bad.bounds = g::PointQuantizationBounds::Explicit;
    bad.range.x = 0;
    rejects(bad);
    bad = request;
    bad.bounds = g::PointQuantizationBounds::Explicit;
    bad.range.x = std::numeric_limits<float>::denorm_min();
    rejects(bad);
    bad = request;
    bad.bounds = g::PointQuantizationBounds::Explicit;
    bad.minimum.x = std::numeric_limits<float>::max();
    rejects(bad);
    require(operation->prepare(request, {}, nullptr) == FE_INVALID_ARGS, "Null ticket output rejected");
    g::PointQuantizePreparationBudget budget;
    budget.reserved = 1;
    rejects(request, budget);
    const uint64_t codeBytes = uint64_t(count) * 6, pad = (codeBytes + 3) & ~uint64_t(3),
                   positionBytes = uint64_t(count) * components * 4;
    const uint64_t gpu = count ? 256 + 2 * (pad + positionBytes) : 0;
    const uint64_t host =
        codeBytes + originalBytes[0].size() + originalBytes[1].size() + originalBytes[2].size();
    if (count) {
        budget = {};
        budget.maxPoints = count - 1;
        rejects(request, budget);
        budget = {};
        budget.maxAllocatedBytes = gpu - 1;
        rejects(request, budget);
        budget = {};
        budget.maxHostBytes = host - 1;
        rejects(request, budget);
        budget = {};
        budget.maxBuffers = 4;
        rejects(request, budget);
    }
    budget = {};
    budget.maxPoints = count;
    budget.maxAllocatedBytes = gpu;
    budget.maxHostBytes = host;
    budget.maxBuffers = count ? 5 : 0;
    g::PointQuantizeTicketHandle ticket;
    check(operation->prepare(request, budget, &ticket), "Prepare exact quantization budget");
    queryIdentity(ticket.Get());
    g::Float4 minimum, range;
    ticket->getBounds(minimum, range);
    const auto originalInfo = original->getDesc();
    if (padded && velocity) {
        const float minima[] = {minimum.x, minimum.y, minimum.z}, ranges[] = {range.x, range.y, range.z};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            float lo = 0, hi = 0;
            for (uint32_t i = 0; i < count; ++i) {
                float v;
                std::memcpy(&v, originalBytes[1].data() + size_t(i) * components * 4 + axis * 4, 4);
                if (!i)
                    lo = hi = v;
                else {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
            require(minima[axis] == lo && ranges[axis] == (hi == lo ? 1.f : hi - lo),
                    "Exact velocity extrema and constant-axis repair");
        }
    } else if (padded) {
        const auto info = original->getDesc();
        const float lo[] = {info.boundsMin.x, info.boundsMin.y, info.boundsMin.z};
        const float hi[] = {info.boundsMax.x, info.boundsMax.y, info.boundsMax.z};
        const float minima[] = {minimum.x, minimum.y, minimum.z}, ranges[] = {range.x, range.y, range.z};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const float expected = count ? (lo[axis] < 0 ? lo[axis] - 1 : std::max(0.f, lo[axis] - 1)) : 0;
            require(minima[axis] == expected && ranges[axis] == (count ? hi[axis] + 2 - expected : 1),
                    "Exact padded converted bounds");
        }
    } else
        require(!std::memcmp(&minimum, &request.minimum, 16) && !std::memcmp(&range, &request.range, 16),
                "Explicit bounds preserved");
    g::BorrowedBufferBinding early{nullptr, 17, 19};
    require(NVRHI_FAILED(ticket->getCodeBuffer(early)) && early.byteOffset == 17 && early.byteSize == 19,
            "Early GPU code borrow preserved");
    require(ticket->finishAfterCallerCompletion(nullptr) == FE_INVALID_ARGS, "Null result output rejected");
    g::BorrowedBytes unavailable{reinterpret_cast<void*>(uintptr_t(1)), 7};
    require(NVRHI_FAILED(ticket->getCodes(unavailable)) && unavailable.byteCount == 7,
            "Early code borrow preserved");
    g::PointSetHandle evaluated;
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&evaluated)) && !evaluated,
            "Completion before recording rejected");
    h.begin();
    require(NVRHI_FAILED(other->record(h.context(), ticket)), "Foreign factory rejected");
    require(NVRHI_FAILED(operation->record({}, ticket)), "Invalid context rejected");
    check(operation->record(h.context(), ticket), "Record encode/decode/copies");
    require(NVRHI_FAILED(operation->record(h.context(), ticket)), "Repeated recording rejected");
    operation = nullptr;
    other = nullptr;
    original = nullptr;
    h.submit();
    check(ticket->finishAfterCallerCompletion(&evaluated), "Complete quantized immutable points");
    require(evaluated->getDesc().count == count, "Preserved point count");
    if (velocity) {
        const auto info = evaluated->getDesc();
        require(!std::memcmp(&info.boundsMin, &originalInfo.boundsMin, 16) &&
                    !std::memcmp(&info.boundsMax, &originalInfo.boundsMax, 16),
                "Velocity quantization preserves position bounds");
    }
    g::BorrowedBytes codes, values;
    check(ticket->getCodes(codes), "Completed packed codes");
    check(evaluated->getData(selected, values), "Completed canonical positions");
    require(codes.byteCount == codeBytes && values.byteCount == positionBytes, "Exact logical result sizes");
    const float minima[] = {minimum.x, minimum.y, minimum.z}, ranges[] = {range.x, range.y, range.z};
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t axis = 0; axis < 3; ++axis) {
            float source, value;
            uint16_t code;
            std::memcpy(&source, originalBytes[selectedIndex].data() + size_t(i) * components * 4 + axis * 4,
                        4);
            std::memcpy(&code, static_cast<const uint8_t*>(codes.data) + (size_t(i) * 3 + axis) * 2, 2);
            std::memcpy(&value,
                        static_cast<const uint8_t*>(values.data) + size_t(i) * components * 4 + axis * 4, 4);
            const double normalized = std::clamp((double(source) - minima[axis]) / ranges[axis], 0., 1.);
            require(std::abs(int(code) - int(normalized * 65535.)) <= 1,
                    "Independent normalized code interval");
            const double expected = (double(code) / 65535.) * ranges[axis] + minima[axis];
            // Bound binary32 divide/multiply/add (including reciprocal lowering).
            // Relative-to-result tolerance alone fails at cancellation near zero.
            const double tolerance =
                4 * std::numeric_limits<float>::epsilon() * (std::abs(double(minima[axis])) + ranges[axis]) +
                2e-6 * std::max(1., std::abs(expected));
            require(std::isfinite(value) && std::abs(value - expected) <= tolerance,
                    "Decoded actual-code reference with forward rounding bound");
        }
        if (components == 4)
            require(!std::memcmp(originalBytes[selectedIndex].data() + size_t(i) * 16 + 12,
                                 static_cast<const uint8_t*>(values.data) + size_t(i) * 16 + 12, 4),
                    "Exact W payload preservation");
    }
    for (uint32_t i = 0; i < 3; ++i)
        if (count && i != selectedIndex) {
            g::BorrowedBytes bytes;
            check(evaluated->getData(g::PointAttribute(i), bytes), "Preserved attribute");
            require(bytes.byteCount == originalBytes[i].size() &&
                        !std::memcmp(bytes.data, originalBytes[i].data(), originalBytes[i].size()),
                    "Exact ordinal velocity/color transport");
            g::BorrowedPointStream stream;
            check(evaluated->getStream(g::PointAttribute(i), stream), "Preserved GPU attribute");
            require(h.read(stream.buffer, stream.byteSize) == originalBytes[i], "CPU/GPU auxiliary coherence");
        }
    g::BorrowedBufferBinding buffer;
    if (count) {
        check(ticket->getCodeBuffer(buffer), "Packed GPU code view");
        auto bytes = h.read(buffer.buffer, buffer.byteSize);
        require(!std::memcmp(bytes.data(), codes.data, size_t(codeBytes)), "CPU/GPU packed code coherence");
        for (size_t i = size_t(codeBytes); i < bytes.size(); ++i)
            require(bytes[i] == 0, "Zero odd-record padding");
        g::BorrowedPointStream stream;
        check(evaluated->getStream(selected, stream), "Decoded GPU positions");
        const auto positions = h.read(stream.buffer, stream.byteSize);
        require(!std::memcmp(positions.data(), values.data, positions.size()),
                "CPU/GPU decoded position coherence");
    } else
        require(ticket->getCodeBuffer(buffer) == FE_NOT_FOUND, "Empty code buffer absent");
    g::GenerationTicketHandle topology;
    g::PlacementDesc placement;
    placement.minimumBrickCount = {1, 1, 1};
    check(points->prepareTopology(evaluated, {}, placement, {}, &topology),
          "Decoded result authenticates in public topology");
    g::PointSetHandle repeat;
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(&repeat)) && !repeat, "One-use completion");
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
            for (uint32_t components : {3u, 4u})
                for (uint32_t count : {0u, 1u, 3u, 258u})
                    for (bool padded : {false, true})
                        for (bool velocity : {false, true})
                            run(device.device, count, components, padded, velocity);
        }
        require(problems == 0, "Native validation warnings/errors");
        std::puts(
            "PASS public point quantization: actual-code references, attributes, bounds, budgets, lifetime, "
            "padding and authentication");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
