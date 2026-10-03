// VBXConverter: strips the VBX file header (version, transform, grid table)
// from a .vbx file and writes the bare grid section as a .vbxc blob.
#include <gvdb/GVDB.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

void usage(const char *progname) {
    printf("%s      <vbx file path>\n", progname);
}

int ConvertVBXBlob(nvrhi::IDataBlob *pVBX, nvrhi::IDataBlob **ppVBXC) {

    const uint8_t *base = (const uint8_t *)pVBX->GetDataPtr();
    const uint8_t *dp = base;
    const size_t size = pVBX->GetSize();

    // VBX file header
    uint8_t major_version, minor_version;
    dm::float3 pre_translation, euler_angle_xyz, scaling, post_translation;
    int32_t num_grids;
    uint8_t read_masks;

    if (size < 2 + sizeof(int32_t)) {
        fprintf(stderr, "File too small to be a VBX file\n");
        return -1;
    }

    major_version = dp[0];
    minor_version = dp[1];
    dp += 2;

    if ((major_version == 1 && minor_version >= 11) || major_version > 1) {
        memcpy(&pre_translation, dp, sizeof(dm::float3));
        dp += sizeof(dm::float3);
        memcpy(&euler_angle_xyz, dp, sizeof(dm::float3));
        dp += sizeof(dm::float3);
        memcpy(&scaling, dp, sizeof(dm::float3));
        dp += sizeof(dm::float3);
        memcpy(&post_translation, dp, sizeof(dm::float3));
        dp += sizeof(dm::float3);
    } else {
        pre_translation = dm::float3::zero();
        euler_angle_xyz = dm::float3::zero();
        scaling = dm::float3{1.f};
        post_translation = dm::float3::zero();
    }

    memcpy(&num_grids, dp, sizeof(int32_t));
    dp += sizeof(int32_t);
    if (num_grids != 1) {
        fprintf(stderr, "Only single-grid VBX files are supported (%d grids)\n", num_grids);
        return -1;
    }

    if (major_version >= 2) {
        read_masks = *(const uint8_t *)dp;
        dp += sizeof(uint8_t);
    } else if (major_version == 1 && minor_version == 0) {
        read_masks = 1;
    } else
        read_masks = 0;

    // ---- grid offset table
    dp += num_grids * sizeof(uint64_t);

    if (size_t(dp - base) + 256 + 3 > size) {
        fprintf(stderr, "Truncated VBX header\n");
        return -1;
    }

    size_t vbxc_flen = size - size_t(dp - base);
    nvrhi::AutoPtr<nvrhi::IDataBlob> pVBXC;
    nvrhi::FRESULT fr = nvrhi::CreateBlob(vbxc_flen, &pVBXC);
    if (NVRHI_FAILED(fr)) {
        fprintf(stderr, "Failed to create output VBXC data blob\n");
        return -1;
    }

    // ---- grid section, with the compression byte of the grid header replaced
    // by the bitmask flag (the VBXC reader takes it from there)
    memcpy(pVBXC->GetDataPtr(), dp, vbxc_flen);
    uint8_t *out = (uint8_t *)pVBXC->GetDataPtr();
    out[256 + 2] = read_masks;   // name[256], dtype, components, compress

    if (ppVBXC) *ppVBXC = pVBXC.Detach();

    return 0;
}

int main(int argc, char *argv[]) {

    if (argc != 2) {
        usage(argv[0]);
        return -1;
    }

    std::filesystem::path filePath{argv[1]};
    auto fs = nvrhi::TakeOver(MAKE_RC_OBJ(donut::vfs::NativeFileSystem));
    if (!fs->fileExists(filePath)) {
        fprintf(stderr, "can not find file \"%s\"\n", filePath.string().c_str());
        return -1;
    }
    filePath = std::filesystem::canonical(filePath);

    nvrhi::AutoPtr<nvrhi::IDataBlob> pFileBlob;
    nvrhi::FRESULT fr = fs->readFile(filePath, &pFileBlob);
    if (NVRHI_FAILED(fr)) {
        fprintf(stderr, "Read file \"%s\" failed\n", filePath.string().c_str());
        return -1;
    }

    nvrhi::AutoPtr<nvrhi::IDataBlob> pVBXCBlob;
    if (ConvertVBXBlob(pFileBlob, &pVBXCBlob) != 0) {
        fprintf(stderr, "Convert VBX to VBXC file failed\n");
        return -1;
    }

    std::filesystem::path outPath = filePath;
    outPath.replace_extension(".vbxc");

    if (!fs->writeFile(outPath, pVBXCBlob->GetDataPtr(), pVBXCBlob->GetSize())) {
        fprintf(stderr, "Write VBXC file \"%s\" failed\n", outPath.string().c_str());
        return -1;
    }

    printf("Wrote %s\n", outPath.string().c_str());
    return 0;
}
