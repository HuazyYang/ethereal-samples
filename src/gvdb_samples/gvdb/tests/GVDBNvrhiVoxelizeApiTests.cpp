// SPDX-License-Identifier: Apache-2.0
// Independent public-only mesh voxelization acceptance; no concrete owner/core headers.
#include "GVDBNvrhiVoxelizeApi.h"
#include "VoxelizeDivisionGoldens.h"
#include <nvrhi/shader-program.h>
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
            std::printf("Voxelize backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Voxelize backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
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

using Artifacts = std::array<std::vector<char>, 4>;
constexpr const char* entries[] = {"clearTriangleBins", "countTriangleBins", "scatterTriangleBins",
                                   "solidVoxelize"};
g::VoxelizeCreateDesc descriptor(IDevice* d, const Artifacts& a) {
    g::VoxelizeCreateDesc desc;
    desc.device = d;
    for (unsigned i = 0; i < 4; ++i)
        desc.artifacts[i] = {a[i].data(), a[i].size()};
    return desc;
}
g::VoxelizeHandle operation(IDevice* d, const Artifacts& a) {
    g::VoxelizeHandle op;
    auto desc = descriptor(d, a);
    check(g::createVoxelize(desc, &op), "create voxelizer");
    return op;
}
struct Box {
    std::vector<g::Float4> vertices = {
        {1.25f, 1.25f, 1.25f, 1},
        {4.75f, 1.25f, 1.25f, 1},
        {4.75f, 4.75f, 1.25f, 1},
        {1.25f, 4.75f, 1.25f, 1},
        {1.25f, 1.25f, 4.75f, 1},
        {4.75f, 1.25f, 4.75f, 1},
        {4.75f, 4.75f, 4.75f, 1},
        {1.25f, 4.75f, 4.75f, 1}
    };
    std::vector<uint32_t> indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                     3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    g::VoxelizeRequestDesc request() const {
        g::VoxelizeRequestDesc d;
        d.vertices = vertices.data();
        d.vertexCount = vertices.size();
        d.indices = indices.data();
        d.indexCount = indices.size();
        d.resolution = {6, 6, 6};
        d.boundsMaximum = {6, 6, 6, 0};
        d.values = {9, 3, 0, 0};
        return d;
    }
};
g::BorrowedBytes sentinel() { return {reinterpret_cast<const void*>(uintptr_t(1)), 71}; }
void unchanged(g::BorrowedBytes value) {
    require(value.data == reinterpret_cast<const void*>(uintptr_t(1)) && value.byteCount == 71,
            "failed output preserved");
}
void compare(g::BorrowedBytes values, g::MeshVoxelFormat format, const std::vector<float>& expected) {
    const auto stride = format == g::MeshVoxelFormat::R8_UINT ? 1 : 4;
    require(values.byteCount == expected.size() * stride, "result size");
    for (size_t i = 0; i < expected.size(); ++i) {
        float value;
        const auto* p = static_cast<const uint8_t*>(values.data) + i * stride;
        if (format == g::MeshVoxelFormat::R8_UINT)
            value = *p;
        else if (format == g::MeshVoxelFormat::R32_FLOAT)
            std::memcpy(&value, p, 4);
        else {
            int32_t v;
            std::memcpy(&v, p, 4);
            value = float(v);
        }
        if (value != expected[i]) {
            std::fprintf(stderr, "voxel %zu got %g expected %g\n", i, value, expected[i]);
            throw std::runtime_error("independent voxel classification");
        }
    }
}
std::vector<float> expectedBox() {
    std::vector<float> expected(216, 0);
    for (unsigned z = 1; z < 5; ++z)
        for (unsigned y = 1; y < 5; ++y)
            for (unsigned x = 1; x < 5; ++x)
                expected[(z * 6 + y) * 6 + x] =
                    (x == 1 || x == 4 || y == 1 || y == 4 || z == 1 || z == 4) ? 9.f : 3.f;
    return expected;
}
g::VoxelizeTicketHandle run(Harness& h, g::IGVDBVoxelize* op, const g::VoxelizeRequestDesc& d,
                            const g::VoxelizePreparationBudget& budget = {}) {
    g::VoxelizeTicketHandle ticket;
    check(op->prepare(d, budget, &ticket), "prepare");
    h.begin();
    check(op->recordCounts(h.context(), ticket), "record counts");
    h.submit();
    check(ticket->finishCountsAfterCallerCompletion(), "finish counts");
    h.begin();
    check(op->recordVoxelization(h.context(), ticket), "record voxels");
    h.submit();
    g::BorrowedBytes result;
    check(ticket->finishAfterCallerCompletion(result), "finish voxels");
    return ticket;
}
void goldens(Harness& h, g::IGVDBVoxelize* op) {
    for (auto format :
         {g::MeshVoxelFormat::R8_UINT, g::MeshVoxelFormat::R32_FLOAT, g::MeshVoxelFormat::R32_SINT}) {
        for (unsigned variant = 0; variant < 4; ++variant) {
            Box box;
            auto d = box.request();
            d.format = format;
            if (variant == 1) {
                for (size_t i = 0; i < box.indices.size(); i += 3)
                    std::swap(box.indices[i], box.indices[i + 2]);
            }
            if (variant == 2) {
                for (auto& v : box.vertices) {
                    v.x -= 8;
                    v.y -= 8;
                    v.z -= 8;
                }
                d.boundsMinimum = {-8, -8, -8, 0};
                d.boundsMaximum = {-2, -2, -2, 0};
            }
            if (variant == 3) {
                for (auto& row : d.transformRows) {
                    row.x *= 2;
                    row.y *= 2;
                    row.z *= 2;
                    row.w = -8;
                }
                d.boundsMinimum = {-8, -8, -8, 0};
                d.boundsMaximum = {4, 4, 4, 0};
            }
            auto ticket = run(h, op, d);
            g::BorrowedBytes bytes;
            check(ticket->getValues(bytes), "get completed");
            compare(bytes, format, expectedBox());
            g::BorrowedBufferBinding gpu;
            check(ticket->getOutputBuffer(gpu), "get GPU output");
            auto actual = h.read(gpu.buffer, (gpu.byteSize + 3) & ~uint64_t(3));
            require(!std::memcmp(actual.data(), bytes.data, size_t(bytes.byteCount)),
                    "CPU/GPU identical bytes");
        }
        for (unsigned n = 1; n <= 5; ++n) {
            g::VoxelizeRequestDesc d;
            d.resolution = {n, 1, 1};
            d.boundsMaximum = {float(n), 1, 1, 0};
            d.format = format;
            d.values = {9, 3, 7.75f, 0};
            if (format == g::MeshVoxelFormat::R32_SINT)
                d.values.z = -7.75f;
            auto ticket = run(h, op, d);
            g::BorrowedBytes bytes;
            check(ticket->getValues(bytes), "empty mesh result");
            compare(bytes, format,
                    std::vector<float>(n, format == g::MeshVoxelFormat::R32_FLOAT
                                              ? 7.75f
                                              : (format == g::MeshVoxelFormat::R32_SINT ? -7.f : 7.f)));
            g::BorrowedBufferBinding gpu;
            check(ticket->getOutputBuffer(gpu), "empty GPU");
            auto raw = h.read(gpu.buffer, (gpu.byteSize + 3) & ~uint64_t(3));
            for (size_t i = size_t(gpu.byteSize); i < raw.size(); ++i)
                require(raw[i] == 0, "packed R8 tail zero");
        }
    }
    // Exactly integer-aligned faces conservatively touch both adjacent cells.
    Box boundary;
    for (auto& v : boundary.vertices) {
        v.x = v.x < 3 ? 1.f : 5.f;
        v.y = v.y < 3 ? 1.f : 5.f;
        v.z = v.z < 3 ? 1.f : 5.f;
    }
    std::vector<float> expected(216, 9);
    for (unsigned z = 2; z < 4; ++z)
        for (unsigned y = 2; y < 4; ++y)
            for (unsigned x = 2; x < 4; ++x)
                expected[(z * 6 + y) * 6 + x] = 3;
    auto edges = run(h, op, boundary.request());
    g::BorrowedBytes edgeBytes;
    check(edges->getValues(edgeBytes), "integer boundary bytes");
    compare(edgeBytes, g::MeshVoxelFormat::R32_FLOAT, expected);
    for (float y : {-50.f, 50.f}) {
        Box outside;
        for (auto& v : outside.vertices)
            v.y += y;
        auto t = run(h, op, outside.request());
        g::BorrowedBytes bytes;
        check(t->getValues(bytes), "out-of-grid bytes");
        compare(bytes, g::MeshVoxelFormat::R32_FLOAT, std::vector<float>(216, 0));
    }
    g::VoxelizeRequestDesc empty;
    empty.resolution = {1, 1, 1};
    g::VoxelizePreparationBudget exact;
    exact.maxInputBytes = 0;
    exact.maxBuffers = 14;
    exact.maxAllocatedBytes = 452;
    exact.maxTriangleBinVisits = exact.maxVoxelTriangleVisits = 0;
    auto minimum = run(h, op, empty, exact);
    std::puts(
        "PASS all formats, closed box, winding, negative bounds, affine transform, empty meshes and R8 tails");
}
void descriptorFailures(g::IGVDBVoxelize* op) {
    Box box;
    const auto good = box.request();
    auto reject = [&](g::VoxelizeRequestDesc d, g::VoxelizePreparationBudget b = {}) {
        auto* result = reinterpret_cast<g::IGVDBVoxelizeTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(op->prepare(d, b, &result)) && !result,
                "invalid descriptor accepted or uncleared output");
    };
    auto d = good;
    d.structSize--;
    reject(d);
    d = good;
    d.reserved = 1;
    reject(d);
    d = good;
    d.reserved2[0] = 1;
    reject(d);
    d = good;
    d.reserved2[1] = 1;
    reject(d);
    d = good;
    d.vertices = nullptr;
    reject(d);
    d = good;
    d.indices = nullptr;
    reject(d);
    d = good;
    d.indexCount--;
    reject(d);
    d = good;
    d.vertexCount = UINT64_MAX;
    reject(d);
    d = good;
    d.indexCount = UINT64_MAX - 3;
    reject(d);
    d = good;
    d.resolution = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    reject(d);
    d = good;
    d.format = g::MeshVoxelFormat(1);
    reject(d);
    for (unsigned axis = 0; axis < 3; ++axis) {
        d = good;
        (&d.resolution.x)[axis] = 0;
        reject(d);
        d = good;
        (&d.boundsMaximum.x)[axis] = 0;
        reject(d);
    }
    for (unsigned component = 0; component < 4; ++component) {
        d = good;
        (&d.boundsMinimum.x)[component] = std::numeric_limits<float>::infinity();
        reject(d);
        d = good;
        (&d.boundsMaximum.x)[component] = std::numeric_limits<float>::quiet_NaN();
        reject(d);
        d = good;
        (&d.values.x)[component] = std::numeric_limits<float>::infinity();
        reject(d);
        for (unsigned row = 0; row < 3; ++row) {
            d = good;
            (&d.transformRows[row].x)[component] = std::numeric_limits<float>::infinity();
            reject(d);
        }
    }
    d = good;
    d.values.w = 1;
    reject(d);
    d = good;
    d.format = g::MeshVoxelFormat::R8_UINT;
    d.values.x = 256;
    reject(d);
    d = good;
    d.format = g::MeshVoxelFormat::R32_SINT;
    d.values.y = 2147483648.f;
    reject(d);
    d = good;
    d.boundsMinimum.x = -std::numeric_limits<float>::max();
    d.boundsMaximum.x = std::numeric_limits<float>::max();
    reject(d);
    d = good;
    d.boundsMaximum.x = std::numeric_limits<float>::denorm_min();
    reject(d);
    d = good;
    d.transformRows[0].x = std::numeric_limits<float>::max();
    d.transformRows[0].y = -std::numeric_limits<float>::max();
    reject(d);
    box.indices[0] = 8;
    reject(box.request());
    box.indices[0] = 0;
    box.vertices[0].w = std::numeric_limits<float>::quiet_NaN();
    reject(box.request());
    box.vertices[0].w = 1;
    for (unsigned field = 0; field < 6; ++field) {
        g::VoxelizePreparationBudget b;
        if (field == 0)
            b.maxInputBytes = 271;
        if (field == 1)
            b.maxAllocatedBytes = 1;
        if (field == 2)
            b.maxTriangleBinVisits = 143;
        if (field == 3)
            b.maxVoxelTriangleVisits = 2591;
        if (field == 4)
            b.maxBuffers = 13;
        if (field == 5)
            b.reserved = 1;
        reject(good, b);
    }
    require(op->prepare(good, {}, nullptr) == FE_INVALID_ARGS, "null ticket output");
    std::puts("PASS input, finite-arithmetic, address/work/allocation bounds");
}
void lifetimes(Harness& h, const Artifacts& artifacts) {
    auto op = operation(h.device, artifacts), foreign = operation(h.device, artifacts);
    queryIdentity(op.Get());
    Box box;
    g::VoxelizeTicketHandle t;
    check(op->prepare(box.request(), {}, &t), "lifetime preparation");
    queryIdentity(t.Get());
    box.vertices.clear();
    box.indices.clear();
    require(t->getDevice() == h.device && t->getResolution().x == 6 &&
                t->getFormat() == g::MeshVoxelFormat::R32_FLOAT,
            "metadata");
    auto bytes = sentinel();
    require(NVRHI_FAILED(t->getValues(bytes)), "early bytes");
    unchanged(bytes);
    require(NVRHI_FAILED(t->finishAfterCallerCompletion(bytes)), "early final finish");
    unchanged(bytes);
    require(NVRHI_FAILED(t->finishCountsAfterCallerCompletion()), "early counts finish");
    g::BorrowedBufferBinding gpu{reinterpret_cast<IBuffer*>(uintptr_t(1)), 7, 9};
    require(NVRHI_FAILED(t->getOutputBuffer(gpu)) && gpu.byteSize == 9 && gpu.byteOffset == 7,
            "early GPU output");
    h.begin();
    require(NVRHI_FAILED(op->recordVoxelization(h.context(), t)), "phase out of order");
    require(NVRHI_FAILED(foreign->recordCounts(h.context(), t)), "foreign counts");
    require(NVRHI_FAILED(op->recordCounts({}, t)), "bad context");
    require(NVRHI_FAILED(op->recordCounts({nullptr, h.commands}, t)), "missing device");
    require(NVRHI_FAILED(op->recordCounts({h.device, nullptr}, t)), "missing commands");
    require(NVRHI_FAILED(op->recordCounts(h.context(), nullptr)), "null ticket");
    check(op->recordCounts(h.context(), t), "record once");
    require(NVRHI_FAILED(op->recordCounts(h.context(), t)), "repeated counts record");
    h.submit();
    check(t->finishCountsAfterCallerCompletion(), "finish counts once");
    require(NVRHI_FAILED(t->finishCountsAfterCallerCompletion()), "repeated counts finish");
    h.begin();
    require(NVRHI_FAILED(foreign->recordVoxelization(h.context(), t)), "foreign second phase");
    require(NVRHI_FAILED(op->recordVoxelization({}, t)), "invalid second context");
    check(op->recordVoxelization(h.context(), t), "record second phase");
    require(NVRHI_FAILED(op->recordVoxelization(h.context(), t)), "repeated second phase");
    check(t->getOutputBuffer(gpu), "recorded GPU access");
    op = nullptr;
    h.submit();
    check(t->finishAfterCallerCompletion(bytes), "finish after factory release");
    compare(bytes, g::MeshVoxelFormat::R32_FLOAT, expectedBox());
    const auto original = bytes;
    bytes = sentinel();
    require(NVRHI_FAILED(t->finishAfterCallerCompletion(bytes)), "repeated final finish");
    unchanged(bytes);
    auto another = run(h, foreign, Box{}.request());
    check(t->getValues(bytes), "retained result");
    require(bytes.data == original.data, "borrowed pointer stable");
    compare(bytes, g::MeshVoxelFormat::R32_FLOAT, expectedBox());
    Box budgetBox;
    g::VoxelizePreparationBudget budget;
    // Counts require more than the minimum single-triangle allocation. Failure
    // must be repeatable and leave voxelization unavailable.
    budget.maxAllocatedBytes = 256 + 128 + 144 + 48 + 108 + 24 + 48 + 216 * 8;
    g::VoxelizeTicketHandle limited;
    check(foreign->prepare(budgetBox.request(), budget, &limited), "prepare minimum allocation");
    h.begin();
    check(foreign->recordCounts(h.context(), limited), "limited count");
    h.submit();
    require(limited->finishCountsAfterCallerCompletion() == FE_OUT_OF_MEMORY, "second-stage allocation budget");
    require(limited->finishCountsAfterCallerCompletion() == FE_OUT_OF_MEMORY, "repeat failure leaves stage");
    h.begin();
    require(NVRHI_FAILED(foreign->recordVoxelization(h.context(), limited)), "failed sizing cannot record");
    h.submit();
    // Counts completion also retains the device/layouts without its factory.
    auto released = operation(h.device, artifacts);
    g::VoxelizeTicketHandle orphan;
    check(released->prepare(budgetBox.request(), {}, &orphan), "orphan prepare");
    h.begin();
    check(released->recordCounts(h.context(), orphan), "orphan count");
    released = nullptr;
    h.submit();
    check(orphan->finishCountsAfterCallerCompletion(), "counts after operator release");
    std::puts("PASS authenticated phases, copied inputs, retries, stable results and release lifetimes");
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
void artifactFailures(IDevice* d, const Artifacts& accepted) {
    for (unsigned stage = 0; stage < 4; ++stage) {
        auto rejects = [&](std::vector<char> bytes) {
            auto artifacts = accepted;
            artifacts[stage] = std::move(bytes);
            auto desc = descriptor(d, artifacts);
            auto* op = reinterpret_cast<g::IGVDBVoxelize*>(uintptr_t(1));
            require(NVRHI_FAILED(g::createVoxelize(desc, &op)) && !op, "incompatible artifact accepted");
        };
        rejects({'b', 'a', 'd'});
        rejects(rewriteArtifact(accepted[stage], [](auto& e, auto&) { e.ThreadGroupSize[0] = 128; }));
        rejects(rewriteArtifact(accepted[stage], [](auto& e, auto&) { e.Name = e.Symbol = "wrongEntry"; }));
        rejects(rewriteArtifact(accepted[stage], [](auto&, auto& b) { b[0].ElementStride += 16; }));
        rejects(rewriteArtifact(accepted[stage], [](auto&, auto& bindings) {
            for (auto& b : bindings)
                if (b.Scope == ST_SCOPE_CONSTANT_BUFFER && b.Count == 3) {
                    b.Count = 2;
                    b.Size = 32;
                    break;
                }
        }));
        rejects(rewriteArtifact(accepted[stage], [](auto&, auto& bindings) {
            for (auto& b : bindings)
                if (b.Scope == ST_SCOPE_GLOBAL && b.Kind == ST_BINDING_STRUCTURED_BUFFER) {
                    b.RegisterIndex = 9;
                    break;
                }
        }));
        rejects(rewriteArtifact(accepted[stage], [](auto&, auto& bindings) {
            for (auto& b : bindings)
                if (b.Scope == ST_SCOPE_GLOBAL && b.Kind == ST_BINDING_RW_BYTE_ADDRESS_BUFFER) {
                    b.Kind = ST_BINDING_RW_STRUCTURED_BUFFER;
                    b.ElementStride = 4;
                    break;
                }
        }));
    }
    std::puts("PASS all four artifact entry/layout/array/stride/container checks");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "open artifact");
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
}
void divisionGoldens(Harness& h, const std::filesystem::path& path) {
    auto binary = readArtifact(path / "GVDBVoxelizeDivisionTest_testVoxelDivision.bin");
    ShaderProgramHandle program;
    check(createShaderProgram(h.device, binary.data(), binary.size(), &program), "division program");
    uint32_t entry;
    check(program->findEntryPoint("testVoxelDivision", ShaderType::Compute, &entry), "division entry");
    BindingLayoutDesc ld;
    check(program->getBindingLayoutDesc(entry, 0, ld), "division layout");
    BindingLayoutHandle layout;
    check(h.device->createBindingLayout(ld, &layout), "division layout creation");
    ShaderHandle shader;
    check(program->createShader(entry, &shader), "division shader");
    ComputePipelineHandle pipeline;
    check(h.device->createComputePipeline(
              ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout), &pipeline),
          "division pipeline");
    constexpr auto count = std::size(voxelDivisionGoldens);
    std::vector<std::array<uint32_t, 2>> input;
    for (auto& row : voxelDivisionGoldens)
        input.push_back({row[0], row[1]});
    BufferHandle source, output;
    check(h.device->createBuffer(BufferDesc()
                                     .setByteSize(count * 8)
                                     .setStructStride(8)
                                     .setInitialState(ResourceStates::CopyDest)
                                     .setKeepInitialState(true),
                                 &source),
          "division input");
    check(h.device->createBuffer(BufferDesc()
                                     .setByteSize(count * 4)
                                     .setStructStride(4)
                                     .setCanHaveUAVs(true)
                                     .setInitialState(ResourceStates::CopyDest)
                                     .setKeepInitialState(true),
                                 &output),
          "division output");
    BindingSetDesc bd;
    bd.addItem(BindingSetItem::StructuredBuffer_SRV(0, source))
        .addItem(BindingSetItem::StructuredBuffer_UAV(0, output));
    BindingSetHandle binding;
    check(h.device->createBindingSet(bd, layout, &binding), "division binding");
    h.begin();
    h.commands->writeBuffer(source, input.data(), input.size() * 8);
    h.commands->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(binding));
    h.commands->dispatch(uint32_t(count));
    h.submit();
    auto bytes = h.read(output, count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint32_t actual;
        std::memcpy(&actual, bytes.data() + 4 * i, 4);
        if (actual != voxelDivisionGoldens[i][2]) {
            std::fprintf(stderr, "division %zu: %08x/%08x got %08x expected %08x\n", i,
                         voxelDivisionGoldens[i][0], voxelDivisionGoldens[i][1], actual,
                         voxelDivisionGoldens[i][2]);
            throw std::runtime_error("division bit golden");
        }
    }
    std::printf("PASS %zu compiled binary32 division bit goldens\n", count);
}
}  // namespace
int main(int argc, const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity s, const char* t) {
        if (s >= donut::log::Severity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", t);
    });
    try {
        {
            Device device(argc, argv);
            Harness h(device.device);
            auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_voxelize" /
                        donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            Artifacts artifacts;
            for (unsigned i = 0; i < 4; ++i)
                artifacts[i] = readArtifact(path / (std::string("GVDBMeshVoxelize_") + entries[i] + ".bin"));
            divisionGoldens(h, path);
            auto op = operation(device.device, artifacts);
            descriptorFailures(op);
            goldens(h, op);
            lifetimes(h, artifacts);
            artifactFailures(device.device, artifacts);
        }
        require(problems == 0, "validation warnings or errors");
        std::puts("Public mesh voxelization acceptance PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
