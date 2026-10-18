// Standalone scalar VBX I/O; no GPU context or legacy gp dependency.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBPortableData.h"
#include <array>
#include <filesystem>
namespace gvdb::slang {
struct VbxFloat3 { float x=0,y=0,z=0; };
struct VbxTransform {
    VbxFloat3 preTranslation;
    VbxFloat3 angles;
    VbxFloat3 scale{1,1,1};
    VbxFloat3 translation;
};
struct VbxVolume {
    PackedVolume packed;
    std::vector<float> atlas;
    VbxTransform transform;
    std::array<uint8_t,256> gridName{};
    VbxFloat3 voxelSize{1,1,1};
};
struct VbxChannel {
    AtlasFormat format=AtlasFormat::R32_FLOAT;
    std::vector<uint8_t> bytes; // tightly packed little-endian channel data, no GPU padding
};
struct VbxRawVolume {
    PackedVolume packed;
    std::vector<VbxChannel> channels;
    VbxTransform transform;
    std::array<uint8_t,256> gridName{};
    VbxFloat3 voxelSize{1,1,1};
};
// One atlas grid with all five GVDB channel formats. Channel order and bytes
// are preserved. Compression, topology reuse, and other layouts are rejected.
bool loadRawVBX(const void* data,size_t bytes,VbxRawVolume& output,std::string& error);
bool saveRawVBX(const VbxRawVolume& volume,std::vector<uint8_t>& output,std::string& error);
bool loadRawVBXFile(const std::filesystem::path& path,VbxRawVolume& output,std::string& error);
bool saveRawVBXFile(const std::filesystem::path& path,const VbxRawVolume& volume,std::string& error);
// Supports one uncompressed R32_FLOAT channel in one atlas grid, VBX 1.0,
// 1.11+ and 2.x. Bitmask topology is expanded. Save emits canonical VBX 1.11.
// Failure leaves the output object untouched and supplies a diagnostic.
bool loadVBX(const void* data,size_t bytes,VbxVolume& output,std::string& error);
bool saveVBX(const VbxVolume& volume,std::vector<uint8_t>& output,std::string& error);
bool loadVBXFile(const std::filesystem::path& path,VbxVolume& output,std::string& error);
bool saveVBXFile(const std::filesystem::path& path,const VbxVolume& volume,std::string& error);
} // namespace gvdb::slang
