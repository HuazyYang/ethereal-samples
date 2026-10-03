#ifndef GVDBALLOCATOR_H
#define GVDBALLOCATOR_H
// Host-side node pools of a GVDB volume (the "pool" half of the reference
// Allocator). Group 0 holds nodes, group 1 child lists; one allocation per
// level. Pools grow by doubling and never shrink; clearing resets the
// allocated count only, so the memory is reused by the next topology.
#include <gvdb/GVDB.h>
#include <nvrhi/core/foundation.h>
#include <vector>

namespace gvdb {

enum { MAX_POOL = 10 };

// TODO(migration): dropped - base `ethereal::UserAllocated` (public, leak-tracking
// operator new/delete) exists in the old ethereal fork; nvrhi::UserAllocated makes
// operator new/delete protected, so plain global new/delete is used instead.
class GVDBAllocator {
 public:
    GVDBAllocator();
    ~GVDBAllocator();

    GVDBAllocator(const GVDBAllocator &) = delete;
    GVDBAllocator &operator=(const GVDBAllocator &) = delete;

    // (Re)creates the pool of one level: elementSize bytes per element,
    // initNumElements reserved (0 reserves nothing; the first alloc grows it).
    void poolCreate(uint8_t grp, uint8_t lev, uint64_t elementSize, uint64_t initNumElements);
    void poolReleaseAll();      // frees everything
    void poolClearAll();        // allocated counts to 0, memory kept

    NodeId poolAlloc(uint8_t grp, uint8_t lev);
    // Grows the reservation so that at least numElements fit (allocated count unchanged).
    void poolReserve(uint8_t grp, uint8_t lev, uint64_t numElements);
    // Sets the allocated count, growing the reservation when needed.
    void poolSetSize(uint8_t grp, uint8_t lev, uint64_t size);
    uint64_t poolGetLevelCount(uint8_t grp) const;
    bool poolExists(uint8_t grp, uint8_t lev) const;

    void *poolData(NodeId id);
    void *poolData(uint8_t grp, uint8_t lev);
    const void *poolData(uint8_t grp, uint8_t lev) const;
    uint64_t poolGetNumUsed(uint8_t grp, uint8_t lev) const;
    uint64_t poolGetNumReserved(uint8_t grp, uint8_t lev) const;
    uint64_t poolGetElementWidth(uint8_t grp, uint8_t lev) const;
    uint64_t poolGetElementStride(uint8_t grp, uint8_t lev) const;
    uint64_t poolGetMemoryReservedWidth(uint8_t grp, uint8_t lev) const;
    uint64_t poolGetMemoryUsedWidth(uint8_t grp, uint8_t lev) const;

 private:
    struct Allocation {
        void *ptr = nullptr;
        uint64_t elementStride = 0;
        uint64_t elementWidth = 0;
        uint64_t numUsed = 0;
        uint64_t numReserved = 0;
    };
    using Pool = std::vector<Allocation>;

    void grow(Allocation &allocation, uint64_t numElements);

    Pool m_pools[MAX_POOL];
};

}  // namespace gvdb

#endif /* GVDBALLOCATOR_H */
