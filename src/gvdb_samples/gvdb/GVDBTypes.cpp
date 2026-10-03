// Plain-data helpers of GVDB.h: atlas formats and the level configuration.
#include <gvdb/GVDB.h>

namespace gvdb {

uint32_t atlasFormatBytes(AtlasFormat format) {
    switch (format) {
        case ATLAS_FORMAT_R8_UINT: return 1;
        case ATLAS_FORMAT_RGBA8_UINT: return 4;
        case ATLAS_FORMAT_R32_FLOAT: return 4;
        case ATLAS_FORMAT_RGB32_FLOAT: return 12;
        case ATLAS_FORMAT_RGBA32_FLOAT: return 16;
    }
    return 0;
}

donut::gp::Format atlasFormatToGP(AtlasFormat format) {
    using donut::gp::Format;
    switch (format) {
        case ATLAS_FORMAT_R8_UINT: return Format::R8_UINT;
        case ATLAS_FORMAT_RGBA8_UINT: return Format::RGBA8_UINT;
        case ATLAS_FORMAT_R32_FLOAT: return Format::R32_FLOAT;
        case ATLAS_FORMAT_RGB32_FLOAT: return Format::RGB32_FLOAT;
        case ATLAS_FORMAT_RGBA32_FLOAT: return Format::RGBA32_FLOAT;
    }
    return Format::UNKNOWN;
}

// Reference type codes (gvdb_types.h): T_UCHAR 0, T_UCHAR3 1, T_UCHAR4 2,
// T_FLOAT 3, T_FLOAT3 4, T_FLOAT4 5, T_INT 6, ...
int atlasFormatToVBXType(AtlasFormat format) {
    switch (format) {
        case ATLAS_FORMAT_R8_UINT: return 0;
        case ATLAS_FORMAT_RGBA8_UINT: return 2;
        case ATLAS_FORMAT_R32_FLOAT: return 3;
        case ATLAS_FORMAT_RGB32_FLOAT: return 4;
        case ATLAS_FORMAT_RGBA32_FLOAT: return 5;
    }
    return -1;
}

bool atlasFormatFromVBXType(int vbxType, AtlasFormat &format) {
    switch (vbxType) {
        case 0: format = ATLAS_FORMAT_R8_UINT; return true;
        case 2: format = ATLAS_FORMAT_RGBA8_UINT; return true;
        case 3: format = ATLAS_FORMAT_R32_FLOAT; return true;
        case 4: format = ATLAS_FORMAT_RGB32_FLOAT; return true;
        case 5: format = ATLAS_FORMAT_RGBA32_FLOAT; return true;
        default: return false;
    }
}

// Configure(q4, q3, q2, q1, q0) of the reference: five levels, level 0 = q0,
// initial pool reservation 4, 4, 2, 1, 1 (the reference's 4, 4, 2, 1, 0 -> 1).
GVDBLevelConfig GVDBLevelConfig::fromBranching(int r4, int r3, int r2, int r1, int r0) {
    GVDBLevelConfig cfg;
    cfg.numLevels = 5;
    const int r[5] = {r0, r1, r2, r3, r4};
    const uint32_t n[5] = {4, 4, 2, 1, 1};
    for (int i = 0; i < 5; ++i) {
        cfg.logDim[i] = r[i] <= 0 ? 1u : uint32_t(r[i]);
        cfg.initialNodes[i] = n[i];
    }
    return cfg;
}

}  // namespace gvdb
