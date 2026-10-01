#include "GVDBAllocator.h"
#include <nvrhi/core/Memory.h>
#include <donut/core/log.h>

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

void GVDBAllocator::poolCreate(uint8_t grp, uint8_t lev, int elementSize,
                               int initNumElements) {
    if (grp > MAX_POOL) {
        donut::log::error("Exceeded maximum number of pools. %d, max %d", grp, MAX_POOL);
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
    allocation.numReserved = initNumElements;
    if(allocation.numReserved)
        allocation.ptr =
            GVDBPoolAllocate(allocation.elementStride * allocation.numReserved);
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
    for(auto &pool : m_pools) {
        for(auto &allocation : pool)
            allocation.numUsed = 0;
    }
}

NodeId GVDBAllocator::poolAlloc(uint8_t grp, uint8_t lev) {
    auto &allocation = m_pools[grp][lev];
    if (allocation.numUsed >= allocation.numReserved) {
        uint64_t reserved = allocation.numReserved * 2;  // grow allocation
        auto newPtr = GVDBPoolAllocate(reserved * allocation.elementStride);
        if(allocation.ptr) {
            memcpy(newPtr, allocation.ptr, allocation.numUsed * allocation.elementStride);
            GVDBPoolFree(allocation.ptr);
        }
        allocation.ptr = newPtr;
        allocation.numReserved = reserved;
    }

    NodeId id{grp, lev, allocation.numUsed};
    allocation.numUsed += 1;

    return id;
}

void *GVDBAllocator::poolData(NodeId id) {
    auto &allocation = m_pools[id.group()][id.level()];
    return (uint8_t *)allocation.ptr + allocation.elementStride * id.index();
}

void *GVDBAllocator::poolData(uint8_t grp, uint8_t lev) { return m_pools[grp][lev].ptr; }

void GVDBAllocator::poolSetSize(uint8_t grp, uint8_t lev, uint64_t size) {
    auto &allocation = m_pools[grp][lev];
    NVRHI_ASSERT(allocation.numReserved >= size);
    allocation.numUsed = size;
}

uint64_t GVDBAllocator::poolGetLevelCount(uint8_t grp) { return m_pools[grp].size(); }

uint64_t GVDBAllocator::poolGetNumUsed(uint8_t grp, uint8_t lev) {
    return m_pools[grp][lev].numUsed;
}

uint64_t GVDBAllocator::poolGetNumReserved(uint8_t grp, uint8_t lev) {
    return m_pools[grp][lev].numReserved;
}

uint64_t GVDBAllocator::poolGetElementWidth(uint8_t grp, uint8_t lev) {
    return m_pools[grp][lev].elementWidth;
}

uint64_t GVDBAllocator::poolGetElementStride(uint8_t grp, uint8_t lev) {
    return m_pools[grp][lev].elementStride;
}

uint64_t GVDBAllocator::poolGetMemoryReservedWidth(uint8_t grp, uint8_t lev) {
    auto &allocation = m_pools[grp][lev];
    return allocation.elementStride * allocation.numReserved;
}

uint64_t GVDBAllocator::poolGetMemoryUsedWidth(uint8_t grp, uint8_t lev) {
    auto &allocation = m_pools[grp][lev];
    return allocation.elementStride * allocation.numUsed;
}

}