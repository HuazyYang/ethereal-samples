#include "GVDBAllocator.h"
#include <nvrhi/core/memory.h>
#include <donut/core/log.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace gvdb {

static void *GVDBPoolAllocate(size_t size) {
    if (size == 0) return nullptr;

    // NOTE(migration): the old fork had nvrhi::DefaultMemoryAllocator::Get() and a
    // file/line-tracking Allocate() overload; donut has nvrhi::GetDefaultMemAllocator()
    // and Allocate(size) only, so allocation-site tracking is dropped.
    auto p = nvrhi::GetDefaultMemAllocator()->Allocate(size);
    if (!p) throw std::bad_alloc();

    return p;
}

static void GVDBPoolFree(void *p) {
    if (p) nvrhi::GetDefaultMemAllocator()->Free(p);
}

GVDBAllocator::GVDBAllocator() {}

GVDBAllocator::~GVDBAllocator() { poolReleaseAll(); }

void GVDBAllocator::poolCreate(uint8_t grp, uint8_t lev, uint64_t elementSize, uint64_t initNumElements) {
    if (grp >= MAX_POOL) {
        donut::log::error("GVDBAllocator: exceeded maximum number of pools (%d, max %d)", grp, MAX_POOL);
        return;
    }

    auto &pool = m_pools[grp];
    if (pool.size() <= lev) pool.resize(lev + 1, {});

    auto &allocation = pool[lev];
    if (allocation.ptr) {
        GVDBPoolFree(allocation.ptr);
        allocation.ptr = nullptr;
    }

    allocation.elementStride = elementSize;
    allocation.elementWidth = elementSize;
    allocation.numUsed = 0;
    allocation.numReserved = elementSize ? initNumElements : 0;
    if (allocation.numReserved)
        allocation.ptr = GVDBPoolAllocate(allocation.elementStride * allocation.numReserved);
}

void GVDBAllocator::poolReleaseAll() {
    for (auto &pool : m_pools) {
        for (auto &allocation : pool) {
            if (allocation.ptr) {
                GVDBPoolFree(allocation.ptr);
                allocation.ptr = nullptr;
            }
        }
        pool.clear();
    }
}

void GVDBAllocator::poolClearAll() {
    for (auto &pool : m_pools) {
        for (auto &allocation : pool) allocation.numUsed = 0;
    }
}

void GVDBAllocator::grow(Allocation &allocation, uint64_t numElements) {
    if (numElements <= allocation.numReserved) return;
    NVRHI_ASSERT(allocation.elementStride != 0);

    // double, but at least to the requested count (and never from 0 to 0)
    uint64_t reserved = std::max<uint64_t>(allocation.numReserved * 2, 16);
    reserved = std::max(reserved, numElements);

    auto newPtr = GVDBPoolAllocate(reserved * allocation.elementStride);
    if (allocation.ptr) {
        memcpy(newPtr, allocation.ptr, allocation.numUsed * allocation.elementStride);
        GVDBPoolFree(allocation.ptr);
    }
    allocation.ptr = newPtr;
    allocation.numReserved = reserved;
}

NodeId GVDBAllocator::poolAlloc(uint8_t grp, uint8_t lev) {
    if (!poolExists(grp, lev) || m_pools[grp][lev].elementStride == 0) {
        donut::log::error("GVDBAllocator: pool (%d, %d) does not exist", grp, lev);
        return NodeId::null();
    }
    auto &allocation = m_pools[grp][lev];
    if (allocation.numUsed >= allocation.numReserved) grow(allocation, allocation.numUsed + 1);

    NodeId id{grp, lev, allocation.numUsed};
    allocation.numUsed += 1;

    return id;
}

void GVDBAllocator::poolReserve(uint8_t grp, uint8_t lev, uint64_t numElements) {
    if (!poolExists(grp, lev)) return;
    auto &allocation = m_pools[grp][lev];
    if (allocation.elementStride == 0) return;
    grow(allocation, numElements);
}

void GVDBAllocator::poolSetSize(uint8_t grp, uint8_t lev, uint64_t size) {
    if (!poolExists(grp, lev)) return;
    auto &allocation = m_pools[grp][lev];
    if (allocation.elementStride == 0) {
        allocation.numUsed = 0;
        return;
    }
    grow(allocation, size);
    allocation.numUsed = size;
}

uint64_t GVDBAllocator::poolGetLevelCount(uint8_t grp) const {
    return grp < MAX_POOL ? m_pools[grp].size() : 0;
}

bool GVDBAllocator::poolExists(uint8_t grp, uint8_t lev) const {
    return grp < MAX_POOL && lev < m_pools[grp].size();
}

void *GVDBAllocator::poolData(NodeId id) {
    if (id == NodeId::null() || !poolExists(id.group(), id.level())) return nullptr;
    auto &allocation = m_pools[id.group()][id.level()];
    if (!allocation.ptr) return nullptr;
    return (uint8_t *)allocation.ptr + allocation.elementStride * id.index();
}

void *GVDBAllocator::poolData(uint8_t grp, uint8_t lev) {
    return poolExists(grp, lev) ? m_pools[grp][lev].ptr : nullptr;
}

const void *GVDBAllocator::poolData(uint8_t grp, uint8_t lev) const {
    return poolExists(grp, lev) ? m_pools[grp][lev].ptr : nullptr;
}

uint64_t GVDBAllocator::poolGetNumUsed(uint8_t grp, uint8_t lev) const {
    return poolExists(grp, lev) ? m_pools[grp][lev].numUsed : 0;
}

uint64_t GVDBAllocator::poolGetNumReserved(uint8_t grp, uint8_t lev) const {
    return poolExists(grp, lev) ? m_pools[grp][lev].numReserved : 0;
}

uint64_t GVDBAllocator::poolGetElementWidth(uint8_t grp, uint8_t lev) const {
    return poolExists(grp, lev) ? m_pools[grp][lev].elementWidth : 0;
}

uint64_t GVDBAllocator::poolGetElementStride(uint8_t grp, uint8_t lev) const {
    return poolExists(grp, lev) ? m_pools[grp][lev].elementStride : 0;
}

uint64_t GVDBAllocator::poolGetMemoryReservedWidth(uint8_t grp, uint8_t lev) const {
    if (!poolExists(grp, lev)) return 0;
    auto &allocation = m_pools[grp][lev];
    return allocation.elementStride * allocation.numReserved;
}

uint64_t GVDBAllocator::poolGetMemoryUsedWidth(uint8_t grp, uint8_t lev) const {
    if (!poolExists(grp, lev)) return 0;
    auto &allocation = m_pools[grp][lev];
    return allocation.elementStride * allocation.numUsed;
}

}  // namespace gvdb
