#include <gvdb/GVDB.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/core/autoptr.h>

void usage(char *progname) {
    printf("%s      <vbx file path>\n", progname);
    return;
}

int ConvertVBXBlob(nvrhi::IDataBlob *pVBX, nvrhi::IDataBlob **ppVBXC) {

    uint8_t *dp = (uint8_t *)pVBX->GetDataPtr();

    // VBX file header
    uint8_t major_version, minor_version;
    dm::float3 pre_translation, euler_angle_xyz, scaling, post_translation;
    int32_t num_grids;
    uint8_t read_masks;

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
    euler_angle_xyz.x = dm::radians(euler_angle_xyz.x);
    euler_angle_xyz.y = dm::radians(euler_angle_xyz.y);
    euler_angle_xyz.z = dm::radians(euler_angle_xyz.z);

    num_grids = *(const int32_t *)dp;
    NVRHI_ASSERT(num_grids == 1);
    dp += sizeof(int32_t);

    if (major_version >= 2) {
        read_masks = *(const uint8_t *)dp;
        dp += sizeof(uint8_t);
    } else if (major_version == 1 && minor_version == 0) {
        read_masks = 1;
    } else
        read_masks = 0;

    // ---- grid offset table
    const uint64_t *grid_offsets = (const uint64_t *)dp;
    dp += num_grids * sizeof(uint64_t);

    size_t vbxc_flen = pVBX->GetSize() - (dp - (const uint8_t *)pVBX->GetDataPtr());
    nvrhi::AutoPtr<nvrhi::IDataBlob> pVBXC;
    nvrhi::FRESULT fr = nvrhi::CreateBlob(vbxc_flen, &pVBXC);
    if (NVRHI_FAILED(fr)) {
        fprintf(stderr, "Failed to create output VBXC data blob\n");
        return -1;
    }

    auto dp2 = dp;
    // ---- grid header
    char grid_name[256];      // grid name
    uint8_t grid_dtype;       // grid data type
    uint8_t grid_components;  // grid components
    uint8_t grid_compress;    // grid compression (0=none, 1=blosc, 2=..)

    memcpy(grid_name, dp, 256);
    dp += 256;
    memcpy(&grid_dtype, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    memcpy(&grid_components, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    grid_compress = read_masks;  // replace with mask
    memcpy(dp2, &grid_components, sizeof(uint8_t));

    memcpy(pVBXC->GetDataPtr(), dp, vbxc_flen);
    if (ppVBXC)
            *ppVBXC = pVBXC;

    return 0;
}

int main(int argc, char *argv[]) {

    if (argc != 2) {
        usage(argv[1]);
        return -1;
    }

    std::filesystem::path filePath{argv[1]};
    auto fs = nvrhi::TakeOver(MAKE_RC_OBJ(donut::vfs::NativeFileSystem));
    if (fs->fileExists(filePath)) {
        fprintf(stderr, "can not find file\"%s\"\n", filePath.string().c_str());
        return -1;
    }
    filePath = std::filesystem::canonical(filePath);

    nvrhi::AutoPtr<nvrhi::IDataBlob> pFileBlob;
    nvrhi::FRESULT fr = fs->readFile(filePath, &pFileBlob);
    if (NVRHI_FAILED(fr)) {
        fprintf(stderr, "Read file \"%s\"\n", filePath.string().c_str());
        return -1;
    }

    nvrhi::AutoPtr<nvrhi::IDataBlob> pVBXCBlob;
    if (ConvertVBXBlob(pFileBlob, &pVBXCBlob) != 0) {
        fprintf(stderr, "Convert VBX to VBXC file failed\n");
        return -1;
    }

    std::filesystem::path outPath = filePath.parent_path() / filePath.stem();
    outPath.append(".vbxc");

    if (!fs->writeFile(outPath, pVBXCBlob->GetDataPtr(), pVBXCBlob->GetSize())) {
        fprintf(stderr, "Write VBXC file failed\n");
        return -1;
    }

    return 0;
}