// Verifies the reconstructed media pipeline against the shipped media.db:
// SQLiteFileSystem (decrypt + LZ4) and the 2018 chunk MeshSet reader.
//
// usage: asteroids_asset_check <path/to/media.db>

#include "fs/SQLiteFileSystem.h"
#include "meshlets/ChunkMeshSet.h"

#include <donut/core/json.h>
#include <donut/core/log.h>
#include <json/json.h>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <media.db>\n", argv[0]);
        return 2;
    }

    // Same arguments as FeatureDemo::Init (0x1400230B0).
    SQLiteFileSystem fs(argv[1], true, "HjLxk8CwekjjquQM");
    if (!fs.isOpen())
    {
        fprintf(stderr, "Couldn't open the media database file.\n");
        return 1;
    }

    Json::Value lods;
    if (!donut::json::LoadFromFile(fs, "media/asteroids_chk-lod.json", lods) || !lods.isArray())
    {
        fprintf(stderr, "failed to read media/asteroids_chk-lod.json\n");
        return 1;
    }

    int failures = 0, files = 0;
    for (const auto& asteroid : lods)
    {
        for (const auto& lod : asteroid["lods"])
        {
            const std::string path = "media/" + lod["model"].asString();
            nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
            fs.readFile(path, &blob);
            auto mset = blob ? chunk2018::LoadMeshSet(blob, path.c_str()) : nullptr;
            ++files;
            if (!mset || mset->type != chunk2018::MeshSetType::Meshlet ||
                mset->meshletMaxVerts != 64 || mset->meshletMaxPrims != 100)
            {
                printf("FAIL %s\n", path.c_str());
                ++failures;
                continue;
            }

            // every meshlet must reference valid vertex/primitive ranges
            const auto* headers = mset->meshletHeaders();
            const auto* idx32 = (const uint32_t*)mset->indices32.data;
            uint64_t tris = 0;
            bool ok = true;
            for (uint32_t m = 0; m < mset->meshletCount(); ++m)
            {
                const auto& h = headers[m];
                if (h.vertexOffset + h.vertexCount() > mset->indices32.elemCount ||
                    h.primOffset + 3 * h.primCount() > mset->indices8.elemCount ||
                    h.vertexCount() > 64 || h.primCount() > 100)
                    ok = false;
                for (uint32_t v = 0; v < h.vertexCount() && ok; ++v)
                    if (idx32[h.vertexOffset + v] >= mset->nverts)
                        ok = false;
                tris += h.primCount();
            }
            if (!ok)
                ++failures;
            printf("%s %-45s verts=%-8u meshlets=%-6u tris=%-8llu material=%s\n", ok ? "ok  " : "FAIL",
                path.c_str(), mset->nverts, mset->meshletCount(), (unsigned long long)tris,
                mset->materials.empty() ? "-" : mset->string(mset->materials[0].name));
        }
    }
    printf("%d files, %d failures\n", files, failures);
    return failures ? 1 : 0;
}
