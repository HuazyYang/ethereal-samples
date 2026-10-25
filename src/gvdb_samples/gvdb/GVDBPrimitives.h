#ifndef GVDB_GVDBPRIMITIVES_H
#define GVDB_GVDBPRIMITIVES_H
// Device-wide primitives of the library (internal): the kernels of
// shaders/GVDBPrimitives.slang recorded into a compute context. They replace
// the reference's Thrust calls and its two copies of PrefixSum.
#include <gvdb/GVDB.h>

namespace gvdb {

class GVDBPrimitives {
 public:
    explicit GVDBPrimitives(IComputeContext *context) : m_context(context) {}

    // Exclusive prefix sum of `count` uints; input and output must differ.
    void prefixSum(nvrhi::IBuffer *input, nvrhi::IBuffer *output, uint32_t count);
    // Sorts `count` 64-bit keys in place, ascending.
    void sortKeys64(nvrhi::IBuffer *keys, uint32_t count);
    // Min and max of `count` floats (reads back: finishes the context).
    void minMax(nvrhi::IBuffer *values, uint32_t count, float &outMin, float &outMax);
    // Copies a tightly packed block of `size` voxels into an atlas texture at `offset` (voxels).
    void copyBlock(nvrhi::IBuffer *src, dm::uint3 size, dm::int3 offset, nvrhi::ITexture *atlas, AtlasFormat format);

    // `buffer` when it holds at least `bytes`, else a new buffer (the content is not kept; the context keeps the old
    // one until recorded work is done). Assign the result where `buffer` came from.
    static nvrhi::BufferHandle ensureBuffer(IComputeContext *context, nvrhi::IBuffer *buffer, uint64_t bytes,
                                            const char *name);

 private:
    IComputeKernel *kernel(const char *name);
    void prefixSumLevel(nvrhi::IBuffer *input, nvrhi::IBuffer *output, uint32_t count, uint32_t level);

    IComputeContext *m_context;                  // the owner's context (not referenced: owners keep it)
    std::vector<nvrhi::BufferHandle> m_scanAux;  // two per prefix-sum level
    nvrhi::BufferHandle m_sortTemp, m_sortCounts, m_sortOffsets;
    nvrhi::BufferHandle m_minMaxPartial, m_minMaxResult;
};

}  // namespace gvdb

#endif /* GVDB_GVDBPRIMITIVES_H */
