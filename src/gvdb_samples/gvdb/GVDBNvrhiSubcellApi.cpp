// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiSubcellApi.h"
#include "GVDBNvrhiPointInsertionApi.h"
#include "GVDBNvrhiPointBinsInternal.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <nvrhi/core/foundation.h>
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <map>
#include <tuple>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure {
    FRESULT result;
};
void require(bool value, FRESULT result = FE_INVALID_ARGS) {
    if (!value)
        throw Failure{result};
}
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result), result); }
template <class F>
FRESULT guarded(F&& function) noexcept {
    try {
        function();
        return FS_OK;
    } catch (const Failure& e) {
        return e.result;
    } catch (const std::bad_alloc&) {
        return FE_OUT_OF_MEMORY;
    } catch (const std::length_error&) {
        return FE_OUT_OF_MEMORY;
    } catch (...) {
        return FE_GENERIC_ERROR;
    }
}
bool finite(Float4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
struct alignas(16) Parameters {
    slang::UInt4 info;
    Float4 translation;
};
static_assert(sizeof(Parameters) == 32);
struct Program {
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
Program loadProgram(IDevice* device, const void* bytes, uint64_t size, const char* name) {
    require(bytes && size && size <= SIZE_MAX);
    Program result;
    check(createShaderProgram(device, bytes, size_t(size), &result.program));
    uint32_t entry;
    check(result.program->findEntryPoint(name, ShaderType::Compute, &entry));
    ShaderEntryPointDesc desc;
    check(result.program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    BindingLayoutDesc layout;
    check(result.program->getBindingLayoutDesc(entry, 0, layout));
    const ResourceType types[] = {ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_SRV,  ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_SRV,  ResourceType::RawBuffer_UAV,
                                  ResourceType::RawBuffer_UAV,  ResourceType::RawBuffer_UAV};
    const uint32_t slots[] = {0, 0, 1, 2, 3, 0, 1, 2};
    require(layout.bindings.size() == 8, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 8; ++i)
        require(std::count_if(
                    layout.bindings.begin(), layout.bindings.end(),
                    [&](const auto& item) { return item.type == types[i] && item.slot == slots[i]; }) == 1,
                FE_UNSUPPORTED);
    uint32_t globals = 0, members = 0, mask = 0, resources = 0, constant = UINT32_MAX;
    for (uint32_t i = 0; i < result.program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(result.program->getParameterDesc(entry, i, item));
        require(item.registerSpace == 0 && item.count == 1, FE_UNSUPPORTED);
        if (item.scope == ShaderParameterScope::Global) {
            ++globals;
            require(item.parentParameter == UINT32_MAX, FE_UNSUPPORTED);
            uint32_t found = 8;
            for (uint32_t j = 0; j < 8; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            require(found < 8 && !(resources & (1u << found)), FE_UNSUPPORTED);
            resources |= 1u << found;
            require(item.elementStride == (found == 0 ? sizeof(Parameters) : 1), FE_UNSUPPORTED);
            if (found == 0)
                constant = i;
        } else {
            require(item.scope == ShaderParameterScope::ConstantBuffer &&
                        item.resourceType == ResourceType::None && item.size == 16 &&
                        item.elementStride == 16 && item.offset < 32 && item.offset % 16 == 0,
                    FE_UNSUPPORTED);
            const uint32_t bit = 1u << (item.offset / 16);
            require(!(mask & bit), FE_UNSUPPORTED);
            mask |= bit;
            ++members;
        }
    }
    require(globals == 8 && resources == 255 && members == 2 && mask == 3 && constant != UINT32_MAX,
            FE_UNSUPPORTED);
    for (uint32_t i = 0; i < result.program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(result.program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
    check(device->createBindingLayout(layout, &result.layout));
    ShaderHandle shader;
    check(result.program->createShader(entry, &shader));
    check(device->createComputePipeline(
        ComputePipelineDesc().setComputeShader(shader).addBindingLayout(result.layout), &result.pipeline));
    return result;
}

struct Kernels {
    DeviceHandle device;
    Program expansion;
};
struct Identity {};
NVRHI_CLASS_CLSID(SubcellTicket, "5fa143d7-b5ef-4326-b3a9-7ced383332e5")
class SubcellTicket final : public ObjectImpl<IGVDBSubcellTicket> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(SubcellTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SubcellTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBSubcellTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(SubcellTicket)
    NVRHI_END_INTERFACE_TABLE()
    std::shared_ptr<Kernels> kernels;
    std::shared_ptr<Identity> identity;
    std::shared_ptr<const VolumeGeneration> snapshot;
    detail::ResolvedPointSet points;
    SubcellInfo info;
    detail::PointBins bins;
    std::vector<SubcellLocation> locations;
    Parameters parameters{};
    BufferHandle cells, locationBuffer, indices, constants;
    std::array<BorrowedPointStream, 3> sourceStreams{};
    std::array<BufferHandle, 3> expanded, staging;
    BindingSetHandle bindings;
    bool recorded = false, completed = false;
    IDevice* getDevice() const noexcept override { return kernels->device.Get(); }
    SubcellInfo getDesc() const noexcept override { return info; }
    FRESULT getCellRanges(BorrowedBytes& out) const noexcept override {
        out = {bins.cells.empty() ? nullptr : bins.cells.data(), uint64_t(bins.cells.size()) * 8};
        return FS_OK;
    }
    FRESULT getCellLocations(BorrowedBytes& out) const noexcept override {
        out = {locations.empty() ? nullptr : locations.data(), uint64_t(locations.size()) * 16};
        return FS_OK;
    }
    FRESULT getSourceOrdinals(BorrowedBytes& out) const noexcept override {
        out = {info.pointPairs ? bins.indices.data() : nullptr, info.pointPairs * 4};
        return FS_OK;
    }
    FRESULT getCellBuffer(BorrowedBufferBinding& out) const noexcept override {
        if (!recorded)
            return FE_INVALID_ARGS;
        out = {cells.Get(), 0, uint64_t(info.cellCount) * 8};
        return FS_OK;
    }
    FRESULT getLocationBuffer(BorrowedBufferBinding& out) const noexcept override {
        if (!recorded)
            return FE_INVALID_ARGS;
        out = {locationBuffer.Get(), 0, uint64_t(info.cellCount) * 16};
        return FS_OK;
    }
    FRESULT getOrdinalBuffer(BorrowedBufferBinding& out) const noexcept override {
        if (!recorded)
            return FE_INVALID_ARGS;
        out = {indices.Get(), 0, info.pointPairs * 4};
        return FS_OK;
    }
    FRESULT finishAfterCallerCompletion(IGVDBPointSet** out) noexcept override {
        if (!out)
            return FE_INVALID_ARGS;
        *out = nullptr;
        return guarded([&] {
            require(recorded && !completed);
            std::array<std::vector<uint8_t>, 3> values;
            for (uint32_t i = 0; i < 3; ++i)
                if (info.pointPairs && (info.attributeMask & (1u << i))) {
                    values[i].resize(size_t(info.pointPairs * sourceStreams[i].byteStride));
                    const void* mapped = getDevice()->mapBuffer(staging[i], CpuAccessMode::Read);
                    require(mapped, FE_GENERIC_ERROR);
                    std::memcpy(values[i].data(), mapped, values[i].size());
                    getDevice()->unmapBuffer(staging[i]);
                }
            PointSetHandle result;
            check(detail::createExpandedPointSet(points.retained, getDevice(), info.pointPairs, expanded,
                                                 std::move(values), &result));
            completed = true;
            *out = result.Detach();
        });
    }
};
detail::PointBins insertionBins(const slang::PackedVolume& topology, const detail::ResolvedPointSet& points,
                                const PointInsertionRequestDesc& request, const GatherPreparationBudget& budget,
                                uint32_t pool, uint32_t width) {
    require(points.info.count <= UINT32_MAX, FE_UNSUPPORTED);
    require(points.info.count <= budget.maxCandidateVisits, FE_OUT_OF_MEMORY);
    BorrowedBytes velocities;
    BorrowedPointStream velocityStream;
    if (request.mode == PointInsertionMode::VelocitySupport) {
        check(points.retained->getData(PointAttribute::Velocity, velocities));
        check(points.retained->getStream(PointAttribute::Velocity, velocityStream));
    }
    detail::PointBins bins;
    bins.side = bins.perLeaf = 1;
    bins.cells.resize(pool, {0, 0});
    using Key = std::tuple<int32_t, int32_t, int32_t>;
    std::map<Key, uint32_t> active;
    Int3 anchor{};
    bool anchored = false;
    for (const auto& leaf : topology.atlasMap)
        if (leaf.leaf >= 0) {
            require(uint32_t(leaf.leaf) < pool);
            if (!anchored) {
                anchor = {leaf.x, leaf.y, leaf.z};
                anchored = true;
            }
            require((int64_t(leaf.x) - anchor.x) % width == 0 && (int64_t(leaf.y) - anchor.y) % width == 0 &&
                        (int64_t(leaf.z) - anchor.z) % width == 0,
                    FE_UNSUPPORTED);
            require(active.emplace(Key{leaf.x, leaf.y, leaf.z}, uint32_t(leaf.leaf)).second);
        }
    auto leafFor = [&](uint32_t i) -> uint32_t {
        Float4 p{};
        std::memcpy(
            &p,
            static_cast<const uint8_t*>(points.positions.data) + size_t(i) * points.positionStream.byteStride,
            12);
        if (p.z == 1e10f)
            return UINT32_MAX;
        p.x += request.translation.x;
        p.y += request.translation.y;
        p.z += request.translation.z;
        if (request.mode == PointInsertionMode::VelocitySupport) {
            Float4 v{};
            std::memcpy(
                &v, static_cast<const uint8_t*>(velocities.data) + size_t(i) * velocityStream.byteStride, 12);
            require(finite(v));
            p.x += v.x * request.supportOffset;
            p.y += v.y * request.supportOffset;
            p.z += v.z * request.supportOffset;
        }
        require(finite(p));
        if (!anchored)
            return UINT32_MAX;
        const double x = std::floor((double(p.x) - anchor.x) / width) * width + anchor.x;
        const double y = std::floor((double(p.y) - anchor.y) / width) * width + anchor.y;
        const double z = std::floor((double(p.z) - anchor.z) / width) * width + anchor.z;
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || z < INT32_MIN || z > INT32_MAX)
            return UINT32_MAX;
        const auto found = active.find(Key{int32_t(x), int32_t(y), int32_t(z)});
        return found == active.end() ? UINT32_MAX : found->second;
    };
    uint64_t pairs = 0;
    for (uint32_t i = 0; i < points.info.count; ++i) {
        const uint32_t leaf = leafFor(i);
        if (leaf != UINT32_MAX) {
            require(pairs < budget.maxPointPairs, FE_OUT_OF_MEMORY);
            ++pairs;
            ++bins.cells[leaf].y;
        }
    }
    uint32_t offset = 0;
    for (auto& cell : bins.cells) {
        cell.x = offset;
        offset += cell.y;
        cell.y = 0;
    }
    bins.indices.resize(size_t(pairs));
    for (uint32_t i = 0; i < points.info.count; ++i) {
        const uint32_t leaf = leafFor(i);
        if (leaf != UINT32_MAX) {
            auto& cell = bins.cells[leaf];
            bins.indices[cell.x + cell.y++] = i;
        }
    }
    return bins;
}
class Subcells final : public ObjectImpl<IGVDBSubcells, IGVDBPointInsertion> {
    std::shared_ptr<Kernels> kernels;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Subcells)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBSubcells)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointInsertion)
    NVRHI_IMPLEMENTS_INTERFACE_AS(IRHIObject, IGVDBSubcells)
    NVRHI_END_INTERFACE_TABLE()
    explicit Subcells(std::shared_ptr<Kernels> value)
        : kernels(std::move(value)) {}
    IDevice* getDevice() const noexcept override { return kernels->device.Get(); }
    FRESULT prepare(const SubcellRequestDesc& d, const SubcellPreparationBudget& budget,
                    IGVDBSubcellTicket** out) noexcept override {
        return prepareImpl(d, budget, out, nullptr);
    }
    FRESULT prepare(const PointInsertionRequestDesc& d, const SubcellPreparationBudget& budget,
                    IGVDBSubcellTicket** out) noexcept override {
        if (!out)
            return FE_INVALID_ARGS;
        *out = nullptr;
        if (d.structSize != sizeof(d) || d.reserved || !finite(d.translation) ||
            !std::isfinite(d.supportOffset) ||
            (d.mode != PointInsertionMode::Center && d.mode != PointInsertionMode::VelocitySupport) ||
            (d.mode == PointInsertionMode::Center && d.supportOffset != 0))
            return FE_INVALID_ARGS;
        SubcellRequestDesc request;
        request.snapshot = d.snapshot;
        request.points = d.points;
        request.translation = d.translation;
        return prepareImpl(request, budget, out, &d);
    }
    FRESULT prepareImpl(const SubcellRequestDesc& d, const SubcellPreparationBudget& budget,
                        IGVDBSubcellTicket** out, const PointInsertionRequestDesc* insertion) noexcept {
        if (!out)
            return FE_INVALID_ARGS;
        *out = nullptr;
        return guarded([&] {
            require(d.structSize == sizeof(d) && !d.reserved && !budget.reserved);
            require(d.subcellWidth && !(d.subcellWidth & (d.subcellWidth - 1)) && std::isfinite(d.radius) &&
                    d.radius >= 0 && finite(d.translation));
            auto t = MAKE_RC_OBJ_PTR(SubcellTicket);
            t->kernels = kernels;
            t->identity = identity;
            check(detail::resolveSnapshot(d.snapshot, t->snapshot));
            require(d.snapshot->getDevice() == getDevice());
            check(detail::resolvePointSet(d.points, getDevice(), t->points));
            const auto desc = d.snapshot->getDesc();
            const auto width = uint32_t(t->snapshot->bindings().levels[0].nodeExtent.x);
            const uint32_t cellWidth = insertion ? width : d.subcellWidth;
            require(width && cellWidth && width % cellWidth == 0, FE_UNSUPPORTED);
            const uint64_t edge = width / cellWidth;
            require(edge <= 1024, FE_UNSUPPORTED);
            const uint64_t perLeaf = edge * edge * edge;
            const uint64_t cellCount = perLeaf * t->snapshot->bindings().levels[0].nodes.z;
            require(perLeaf <= UINT32_MAX && cellCount <= UINT32_MAX / 16, FE_UNSUPPORTED);
            require(cellCount <= budget.maxCells && cellCount * 24 <= budget.maxHostBytes &&
                        cellCount * 24 <= budget.maxAllocatedBytes,
                    FE_OUT_OF_MEMORY);
            t->info = {t->snapshot->bindings().levels[0].nodes.z, uint32_t(perLeaf), uint32_t(cellCount),
                       t->points.info.attributeMask, 0};
            uint64_t stride = 0;
            uint32_t attributeCount = 0;
            for (uint32_t i = 0; i < 3; ++i)
                if (t->info.attributeMask & (1u << i)) {
                    check(t->points.retained->getStream(PointAttribute(i), t->sourceStreams[i]));
                    stride += t->sourceStreams[i].byteStride;
                    ++attributeCount;
                }
            // Validate translated positions even when no active cells receive them.
            for (uint64_t i = 0; i < t->points.info.count; ++i) {
                Float4 p{};
                std::memcpy(&p,
                            static_cast<const uint8_t*>(t->points.positions.data) +
                                size_t(i) * t->points.positionStream.byteStride,
                            12);
                require(std::isfinite(p.x + d.translation.x) && std::isfinite(p.y + d.translation.y) &&
                        std::isfinite(p.z + d.translation.z));
            }
            slang::PackedVolume topology;
            if (cellCount)
                check(detail::VolumeAdoptionBuilder::exportTopology(*t->snapshot, topology));
            t->locations.resize(size_t(cellCount));
            for (const auto& leaf : topology.atlasMap)
                if (leaf.leaf >= 0) {
                    require(uint32_t(leaf.leaf) < t->info.leafPoolCount);
                    for (uint32_t cell = 0; cell < perLeaf; ++cell) {
                        const int64_t x = int64_t(leaf.x) + int64_t(cell % edge) * cellWidth;
                        const int64_t y = int64_t(leaf.y) + int64_t((cell / edge) % edge) * cellWidth;
                        const int64_t z = int64_t(leaf.z) + int64_t(cell / (edge * edge)) * cellWidth;
                        require(x <= INT32_MAX && y <= INT32_MAX && z <= INT32_MAX, FE_UNSUPPORTED);
                        t->locations[size_t(leaf.leaf) * size_t(perLeaf) + cell] = {
                            {int32_t(x), int32_t(y), int32_t(z)},
                            uint32_t(leaf.leaf)
                        };
                    }
                }
            if (desc.activeLeafCount || insertion) {
                GatherRequestDesc request;
                request.subcellWidth = d.subcellWidth;
                request.radius = d.radius;
                request.translation = d.translation;
                GatherPreparationBudget limits;
                limits.maxCells = budget.maxCells;
                limits.maxCandidateVisits = budget.maxCandidateVisits;
                limits.maxPointPairs = std::min({budget.maxPointPairs, uint64_t(UINT32_MAX / 16),
                                                 (budget.maxHostBytes - cellCount * 24) / (4 + stride)});
                if (insertion)
                    t->bins =
                        insertionBins(topology, t->points, *insertion, limits, t->info.leafPoolCount, width);
                else
                    check(detail::buildPointBins(topology, request, limits, t->points, t->bins));
                for (const auto& cell : t->bins.cells)
                    t->info.pointPairs += cell.y;
                t->bins.indices.resize(size_t(t->info.pointPairs));
            } else {
                t->bins.cells.resize(size_t(cellCount), {0, 0});
                t->bins.side = uint32_t(edge);
                t->bins.perLeaf = uint32_t(perLeaf);
            }
            const auto pairs = t->info.pointPairs;
            const uint64_t gpuBytes =
                cellCount * 24 + (pairs ? 256 + 4 * pairs + 2 * pairs * stride + 4 * (3 - attributeCount) : 0);
            const uint32_t buffers = (cellCount ? 2u : 0u) + (pairs ? 5 + attributeCount : 0);
            require(gpuBytes <= budget.maxAllocatedBytes && buffers <= budget.maxBuffers, FE_OUT_OF_MEMORY);
            auto allocate = [&](uint64_t bytes, bool raw, bool cb = false, bool readback = false) {
                BufferDesc allocation;
                allocation.byteSize = bytes;
                allocation.canHaveRawViews = raw;
                allocation.canHaveUAVs = raw;
                allocation.isConstantBuffer = cb;
                allocation.cpuAccess = readback ? CpuAccessMode::Read : CpuAccessMode::None;
                allocation.initialState = cb ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
                allocation.keepInitialState = true;
                allocation.debugName = "GVDB retained subcells";
                BufferHandle result;
                check(getDevice()->createBuffer(allocation, &result));
                require(bool(result), FE_OUT_OF_MEMORY);
                return result;
            };
            if (cellCount) {
                t->cells = allocate(cellCount * 8, true);
                t->locationBuffer = allocate(cellCount * 16, true);
            }
            if (pairs) {
                t->indices = allocate(pairs * 4, true);
                t->constants = allocate(256, false, true);
                t->parameters = {
                    {uint32_t(pairs), uint32_t(t->sourceStreams[0].byteStride),
                     uint32_t(t->sourceStreams[1].byteStride), t->info.attributeMask},
                    d.translation
                };
                BindingSetDesc bindings;
                bindings.trackLiveness = true;
                bindings.addItem(BindingSetItem::ConstantBuffer(0, t->constants))
                    .addItem(BindingSetItem::RawBuffer_SRV(0, t->indices));
                for (uint32_t i = 0; i < 3; ++i) {
                    const bool present = (t->info.attributeMask & (1u << i)) != 0;
                    t->expanded[i] = allocate(present ? pairs * t->sourceStreams[i].byteStride : 4, true);
                    if (present)
                        t->staging[i] = allocate(pairs * t->sourceStreams[i].byteStride, false, false, true);
                    bindings.addItem(BindingSetItem::RawBuffer_SRV(
                        i + 1, present ? t->sourceStreams[i].buffer : t->sourceStreams[0].buffer));
                    bindings.addItem(BindingSetItem::RawBuffer_UAV(i, t->expanded[i]));
                }
                check(getDevice()->createBindingSet(bindings, kernels->expansion.layout, &t->bindings));
            }
            *out = t.Detach();
        });
    }
    FRESULT record(RecordingContext context, IGVDBSubcellTicket* input) noexcept override {
        return guarded([&] {
            require(input);
            AutoPtr<SubcellTicket> t;
            require(NVRHI_SUCCEEDED(input->QueryInterface(uuid_of<SubcellTicket>(),
                                                          reinterpret_cast<void**>(t.GetAddressOf()))) &&
                    t.Get());
            require(t->identity == identity && !t->recorded && context.device == getDevice() &&
                    context.commands && context.commands->getDevice() == getDevice());
            require(context.commands->getDesc().queueType != CommandQueue::Copy, FE_UNSUPPORTED);
            if (t->info.cellCount) {
                context.commands->writeBuffer(t->cells, t->bins.cells.data(), t->bins.cells.size() * 8);
                context.commands->writeBuffer(t->locationBuffer, t->locations.data(), t->locations.size() * 16);
            }
            if (t->info.pointPairs) {
                std::array<uint8_t, 256> cb{};
                std::memcpy(cb.data(), &t->parameters, sizeof(Parameters));
                context.commands->writeBuffer(t->constants, cb.data(), cb.size());
                context.commands->writeBuffer(t->indices, t->bins.indices.data(), t->bins.indices.size() * 4);
                ComputeState state;
                state.pipeline = kernels->expansion.pipeline;
                state.bindings = {t->bindings};
                context.commands->setComputeState(state);
                const uint32_t groups = uint32_t((t->info.pointPairs + 255) / 256);
                context.commands->dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
                for (uint32_t i = 0; i < 3; ++i)
                    if (t->info.attributeMask & (1u << i))
                        context.commands->copyBuffer(t->staging[i], 0, t->expanded[i], 0,
                                                     t->info.pointPairs * t->sourceStreams[i].byteStride);
            }
            t->recorded = true;
        });
    }
};
}  // namespace
nvrhi::FRESULT gvdbCreateNvrhiSubcells(uint32_t version, const SubcellCreateDesc* desc,
                                       IGVDBSubcells** out) noexcept {
    if (!out)
        return nvrhi::FE_INVALID_ARGS;
    *out = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->device);
        auto kernels = std::make_shared<Kernels>();
        kernels->device = desc->device;
        kernels->expansion =
            loadProgram(desc->device, desc->artifact, desc->artifactBytes, "expandSubcellPoints");
        auto result = MAKE_RC_OBJ_PTR(Subcells, std::move(kernels));
        *out = result.Detach();
    });
}
nvrhi::FRESULT gvdbCreateNvrhiPointInsertion(uint32_t version, const SubcellCreateDesc* desc,
                                             IGVDBPointInsertion** out) noexcept {
    if (!out)
        return nvrhi::FE_INVALID_ARGS;
    *out = nullptr;
    SubcellsHandle op;
    const auto result = gvdbCreateNvrhiSubcells(version, desc, &op);
    if (NVRHI_FAILED(result))
        return result;
    return op->QueryInterface(uuid_of<IGVDBPointInsertion>(), reinterpret_cast<void**>(out));
}
}  // namespace gvdb::portable
