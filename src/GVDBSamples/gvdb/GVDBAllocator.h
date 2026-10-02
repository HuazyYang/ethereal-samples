#ifndef GVDBALLOCATOR_H
#define GVDBALLOCATOR_H
#include <gvdb/GVDB.h>
#include <nvrhi/core/foundation.h>

namespace gvdb {

enum { MAX_POOL = 10 };

// TODO(migration): dropped - base `ethereal::UserAllocated` (public, leak-tracking
// operator new/delete) exists in the old ethereal fork; nvrhi::UserAllocated makes
// operator new/delete protected, so plain global new/delete is used instead.
class GVDBAllocator {
 public:
    GVDBAllocator();
    ~GVDBAllocator();

    void poolCreate(uint8_t grp, uint8_t lev, int elementSize, int initNumElements);
    void poolReleaseAll();
    void poolClearAll();

    NodeId poolAlloc(uint8_t grp, uint8_t lev);
    void poolSetSize(uint8_t grp, uint8_t lev, uint64_t size);
    uint64_t poolGetLevelCount(uint8_t grp);

    void *poolData(NodeId id);
    void *poolData(uint8_t grp, uint8_t lev);
    uint64_t poolGetNumUsed(uint8_t grp, uint8_t lev);
    uint64_t poolGetNumReserved(uint8_t grp, uint8_t lev);
    uint64_t poolGetElementWidth(uint8_t grp, uint8_t lev);
    uint64_t poolGetElementStride(uint8_t grp, uint8_t lev);
    uint64_t poolGetMemoryReservedWidth(uint8_t grp, uint8_t lev);
    uint64_t poolGetMemoryUsedWidth(uint8_t grp, uint8_t lev);
 private:
    struct Allocation {
        void *ptr;
        uint64_t elementStride;
        uint64_t elementWidth;
        uint64_t numUsed;
        uint64_t numReserved;
    };
    using Pool = std::vector<Allocation>;

    Pool m_pools[MAX_POOL];
};

}

#endif /* GVDBALLOCATOR_H */
