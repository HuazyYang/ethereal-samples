// Device-wide primitives of the library (see GVDBPrimitives.h).
#include "GVDBPrimitives.h"
#include <donut/core/log.h>
#include <algorithm>

namespace gvdb {

using namespace donut;

namespace {

constexpr uint32_t SCAN_ELEMENTS = GVDB_SCAN_BLOCKSIZE * 2;   // elements per prefixSum thread group
constexpr uint32_t RADIX_TILE = 256;                          // keys per radix sort thread group
constexpr uint32_t MINMAX_GROUP = 256;
constexpr uint32_t MINMAX_MAX_GROUPS = 1024;

const char *copyKernelName(AtlasFormat format) {
    switch (format) {
        case ATLAS_FORMAT_R8_UINT:      return "gvdbCopyBlockC";
        case ATLAS_FORMAT_RGBA8_UINT:   return "gvdbCopyBlockC4";
        case ATLAS_FORMAT_R32_FLOAT:    return "gvdbCopyBlockF";
        case ATLAS_FORMAT_RGBA32_FLOAT: return "gvdbCopyBlockF4";
        case ATLAS_FORMAT_RGB32_FLOAT:  return nullptr;
    }
    return nullptr;
}

}  // namespace

nvrhi::BufferHandle GVDBPrimitives::ensureBuffer(IComputeContext *context, nvrhi::IBuffer *buffer, uint64_t bytes,
                                                 const char *name) {
    bytes = std::max<uint64_t>(bytes, 16);
    if (buffer && buffer->getDesc().byteSize >= bytes) return buffer;
    nvrhi::BufferDesc desc;
    desc.byteSize = bytes;
    desc.canHaveUAVs = true;
    desc.debugName = name;
    context->releaseAfterUse(buffer);   // recorded kernels may still use the old one
    nvrhi::BufferHandle created;
    if (NVRHI_FAILED(context->getDevice()->createBuffer(desc, &created)))
        log::error("GVDB: cannot create the buffer %s (%llu bytes)", name, (unsigned long long)bytes);
    return created;
}

IComputeKernel *GVDBPrimitives::kernel(const char *name) { return m_context->getKernel(GVDB_SHADERS_PRIMITIVES, name); }

void GVDBPrimitives::prefixSum(nvrhi::IBuffer *input, nvrhi::IBuffer *output, uint32_t count) {
    if (count == 0 || !input || !output) return;
    prefixSumLevel(input, output, count, 0);
}

// The reference's two-level scheme, applied recursively: every group scans 1024 elements exclusively and
// writes its total to aux; the totals are scanned (the next level), then added back to the groups.
void GVDBPrimitives::prefixSumLevel(nvrhi::IBuffer *input, nvrhi::IBuffer *output, uint32_t count, uint32_t level) {
    nvrhi::ICommandList *commandList = m_context->getCommandList();
    IComputeKernel *scan = kernel("prefixSum");
    IComputeKernel *fixup = kernel("prefixFixup");
    const int zeroOffset = 1;
    const uint32_t groups = dm::div_ceil(count, SCAN_ELEMENTS);
    if (groups == 1) {
        launchKernel(commandList, scan, dm::uint3(1u), deviceAddress(input), deviceAddress(output), uint64_t(0),
                     int(count), zeroOffset);
        return;
    }
    if (m_scanAux.size() < size_t(level + 1) * 2) m_scanAux.resize(size_t(level + 1) * 2);
    m_scanAux[level * 2] = ensureBuffer(m_context, m_scanAux[level * 2], uint64_t(groups) * sizeof(uint32_t), "GVDB scan aux");
    m_scanAux[level * 2 + 1] =
        ensureBuffer(m_context, m_scanAux[level * 2 + 1], uint64_t(groups) * sizeof(uint32_t), "GVDB scan aux sums");
    nvrhi::IBuffer *aux = m_scanAux[level * 2];
    nvrhi::IBuffer *auxScan = m_scanAux[level * 2 + 1];
    launchKernel(commandList, scan, dm::uint3(groups, 1u, 1u), deviceAddress(input), deviceAddress(output),
                 deviceAddress(aux), int(count), zeroOffset);
    prefixSumLevel(aux, auxScan, groups, level + 1);
    launchKernel(commandList, fixup, dm::uint3(groups, 1u, 1u), deviceAddress(output), deviceAddress(auxScan),
                 int(count));
}

void GVDBPrimitives::sortKeys64(nvrhi::IBuffer *keys, uint32_t count) {
    if (count <= 1 || !keys) return;
    nvrhi::ICommandList *commandList = m_context->getCommandList();
    const uint32_t tiles = dm::div_ceil(count, RADIX_TILE);
    m_sortTemp = ensureBuffer(m_context, m_sortTemp, uint64_t(count) * sizeof(uint64_t), "GVDB sort keys");
    m_sortCounts = ensureBuffer(m_context, m_sortCounts, uint64_t(tiles) * 256 * sizeof(uint32_t), "GVDB sort counts");
    m_sortOffsets = ensureBuffer(m_context, m_sortOffsets, uint64_t(tiles) * 256 * sizeof(uint32_t), "GVDB sort offsets");
    nvrhi::IBuffer *temp = m_sortTemp;
    nvrhi::IBuffer *counts = m_sortCounts;
    nvrhi::IBuffer *offsets = m_sortOffsets;
    IComputeKernel *countKernel = kernel("gvdbRadixCount");
    IComputeKernel *scatterKernel = kernel("gvdbRadixScatter");

    // Eight passes of 8 bits: the sorted keys end up in `keys` again.
    nvrhi::IBuffer *src = keys, *dst = temp;
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        launchKernel(commandList, countKernel, dm::uint3(tiles, 1u, 1u), deviceAddress(src), count, shift,
                     deviceAddress(counts));
        prefixSum(counts, offsets, tiles * 256);
        launchKernel(commandList, scatterKernel, dm::uint3(tiles, 1u, 1u), deviceAddress(src), count, shift,
                     deviceAddress(offsets), deviceAddress(dst));
        std::swap(src, dst);
    }
}

void GVDBPrimitives::minMax(nvrhi::IBuffer *values, uint32_t count, float &outMin, float &outMax) {
    outMin = outMax = 0.f;
    if (count == 0 || !values) return;
    nvrhi::ICommandList *commandList = m_context->getCommandList();
    const uint32_t groups = std::min(dm::div_ceil(count, MINMAX_GROUP), MINMAX_MAX_GROUPS);
    m_minMaxPartial = ensureBuffer(m_context, m_minMaxPartial, uint64_t(groups) * 2 * sizeof(float), "GVDB min max");
    m_minMaxResult = ensureBuffer(m_context, m_minMaxResult, 2 * sizeof(float), "GVDB min max result");
    nvrhi::IBuffer *partial = m_minMaxPartial;
    nvrhi::IBuffer *result = m_minMaxResult;
    launchKernel(commandList, kernel("gvdbMinMaxPartial"), dm::uint3(groups, 1u, 1u), deviceAddress(values), count,
                 groups, deviceAddress(partial));
    launchKernel(commandList, kernel("gvdbMinMaxFinal"), dm::uint3(1u), deviceAddress(partial), groups,
                 deviceAddress(result));
    float mm[2] = {0.f, 0.f};
    m_context->readBuffer(result, 0, mm, sizeof(mm));
    outMin = mm[0];
    outMax = mm[1];
}

void GVDBPrimitives::copyBlock(nvrhi::IBuffer *src, dm::uint3 size, dm::int3 offset, nvrhi::ITexture *atlas,
                               AtlasFormat format) {
    const char *name = copyKernelName(format);
    if (!name) {
        log::error("GVDB: no atlas block copy for format RGB32_FLOAT");
        return;
    }
    if (!src || !atlas || size.x == 0 || size.y == 0 || size.z == 0) return;
    IComputeKernel *copy = kernel(name);
    const uint64_t surface = m_context->getDevice()->getSurfaceHandle(atlas, 0);
    launchKernel(m_context->getCommandList(), copy, groupsFor(copy, size), deviceAddress(src), size, offset, surface);
}

}  // namespace gvdb
