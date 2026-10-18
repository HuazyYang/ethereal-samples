// All GVDB channel formats using explicit NVRHI raw buffers.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortableData.h"
#include <nvrhi/nvrhi.h>
#include <vector>
namespace ethereal::slang { class ComputeRuntime; }
namespace gvdb::slang {
// The borrowed runtime must outlive this object. Initial bytes use the original
// tightly packed channel representation; R8 trailing padding is managed here.
class SlangAtlas {
    ethereal::slang::ComputeRuntime* m_runtime;
    VolumeInfo m_volume;
    AtlasFormat m_format;
    uint32_t m_voxels,m_work;
    size_t m_bytes,m_storage;
    nvrhi::BufferHandle m_nodes,m_children,m_levels,m_map,m_atlas,m_scratch;
    void operate(const char* entry,Float4 value);
public:
    SlangAtlas(ethereal::slang::ComputeRuntime& runtime,const PackedVolume& volume,
        AtlasFormat format,const void* initialData,size_t bytes);
    AtlasFormat format() const {return m_format;}
    const VolumeInfo& info() const {return m_volume;}
    nvrhi::IBuffer* atlas() const {return m_atlas;}
    void clear();
    // R8 uses x in [0,255]; RGBA8 uses normalized xyz and alpha 255;
    // float vectors use xyz and alpha one. R32 z<0 selects the legacy sine fill.
    void fill(Float4 value);
    void updateApron(float boundary=0);
    // Scalar and vector float channels; computes each component independently.
    void smooth(float centerWeight,float bias,uint32_t iterations,float boundary=0);
    void expandByte(uint8_t from,uint8_t to,uint32_t iterations=1,uint8_t boundary=0);
    void expandColor(float centerWeight,float neighborWeight,uint32_t iterations=1,uint8_t boundary=0);
    std::vector<uint8_t> read();
};
} // namespace gvdb::slang
