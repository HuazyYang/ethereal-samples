#include "ObjLoader.h"
#include <cctype>
#include <optional>
#include <string.h>
#include <nvrhi/core/AutoPtr.h>

namespace SampleUtils {

inline void remove_eol(char* ptr) {
    int i = 0;
    while (ptr[i])
        i++;
    i--;
    while (i > 0 && std::isspace(ptr[i])) {
        ptr[i] = '\0';
        i--;
    }
}

inline char* strip_spaces(char* ptr) {
    while (std::isspace(*ptr))
        ptr++;
    return ptr;
}

inline bool getline(const char*& fp, const char* fp_end, char* line, int count) {
    const char* p = fp;
    while (p != fp_end && *p != '\n')
        ++p;

    if (p != fp_end) p += 1;

    int rd_count = p - fp;
    if (rd_count > 0) {
        if (rd_count > count - 1) rd_count = count - 1;
        strncpy(line, fp, rd_count);
        line[rd_count] = 0;
    }

    fp = p;
    return rd_count > 0;
}

inline std::optional<int> read_index(char** ptr) {
    char* base = *ptr;

    // Detect end of line (negative indices are supported)
    base = strip_spaces(base);
    if (!std::isdigit(*base) && *base != '-') return std::nullopt;

    int vidx = static_cast<int>(std::strtol(base, &base, 10));
    base = strip_spaces(base);

    if (*base == '/') {
        base++;

        // Handle the case when there is no texture coordinate
        if (*base != '/') {
            int tidx = static_cast<int>(std::strtol(base, &base, 10));
            NVRHI_ASSERT(tidx == vidx);
        }

        base = strip_spaces(base);

        if (*base == '/') {
            base++;
            int nidx = static_cast<int>(std::strtol(base, &base, 10));
            NVRHI_ASSERT(nidx == vidx);
        }
    }

    *ptr = base;
    return std::make_optional(vidx);
}

inline TriangleMesh load_from_stream(nvrhi::IDataBlob* blob) {
    static constexpr size_t max_line = 1024;
    char line[max_line];

    const char* fp = (const char*)blob->GetDataPtr();
    const char* fp_end = fp + blob->GetSize();


    TriangleMesh mesh;
    mesh.bounds = dm::box3::empty();

    while (getline(fp, fp_end, line, max_line)) {
        char* ptr = strip_spaces(line);
        if (*ptr == '\0' || *ptr == '#') continue;
        remove_eol(ptr);

        switch (ptr[0]) {
            case 'v':
                if (ptr[1] == 'n') {
                    auto x = std::strtof(ptr + 1, &ptr);
                    auto y = std::strtof(ptr, &ptr);
                    auto z = std::strtof(ptr, &ptr);
                    mesh.normals.emplace_back(x, y, z);
                } else if (ptr[1] == 't')
                    ;  // ignore texture coordinates
                else if (std::isspace(ptr[1])) {
                    auto x = std::strtof(ptr + 1, &ptr);
                    auto y = std::strtof(ptr, &ptr);
                    auto z = std::strtof(ptr, &ptr);
                    mesh.positions.emplace_back(x, y, z);
                }
                break;
            case 'm':  // material
            case 'o':  // object designations
            case 'g':  // group designations
            case 'u':  // material
            case 's':  // smoothing command
                break;
            case 'f':
                if (*ptr == 'f' && std::isspace(ptr[1])) {
                    int vidx[3];
                    ptr += 2;
                    for (int i = 0;; ++i) {
                        if (auto index = read_index(&ptr)) {
                            int j = *index < 0 ? mesh.positions.size() + *index : *index - 1;
                            NVRHI_ASSERT(j < mesh.positions.size());
                            if (i <= 2) {
                                vidx[i] = j;
                                if (i == 2)
                                    mesh.indices.emplace_back(vidx[0], vidx[1], vidx[2]);
                            } else {
                                // Construct triangle fan
                                mesh.indices.emplace_back(vidx[0], vidx[2], j);
                            }
                        } else
                            break;
                    }
                }
                break;
        }
    }

    // Sort triangle index
    std::sort(mesh.indices.begin(), mesh.indices.end(), [](const dm::int3 &left, const dm::int3 &right) {
        if(left.x < right.x)
            return true;
        else if(right.x < left.x)
            return false;
        else if(left.y < right.y)
            return true;
        else if(right.y < left.y)
            return false;
        else
            return left.z < right.z;
    });

    // Reconstruct mesh normals if not exist.
    if(!mesh.normals.empty()) {
        size_t vcount = mesh.positions.size();
        std::vector<int> weights(vcount, 0.f);
        mesh.normals.resize(vcount, dm::float3::zero());
        for(auto &tri : mesh.indices) {
            auto &v0 = mesh.positions[tri.x];
            auto &v1 = mesh.positions[tri.y];
            auto &v2 = mesh.positions[tri.z];
            auto e0 = v1 - v0;
            auto e1 = v2 - v0;
            auto gn = dm::normalize(dm::cross(e0, e1));
            NVRHI_ASSERT(!std::isnan(gn.x) && !std::isnan(gn.y) && !std::isnan(gn.z));
            mesh.normals[tri.x] += gn;
            mesh.normals[tri.y] += gn;
            mesh.normals[tri.z] += gn;
            weights[tri.x] += 1;
            weights[tri.y] += 1;
            weights[tri.z] += 1;
        }

        for(size_t i = 0; i < vcount; ++i) {
            float w = static_cast<float>(weights[i]);
            if (w != 1.f)
                mesh.normals[i] = dm::normalize(mesh.normals[i] / float(weights[i]));
        }
     }

    // Compute bounds
    for(const auto &tri : mesh.indices) {
        mesh.bounds |= mesh.positions[tri.x];
        mesh.bounds |= mesh.positions[tri.y];
        mesh.bounds |= mesh.positions[tri.z];
    }

    return mesh;
}

TriangleMesh ObjLoader::operator()(donut::vfs::IFileSystem* pFS, const char* filename) const {
    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    auto fr = pFS->readFile(filename, &blob);
    if (NVRHI_FAILED(fr)) return {};

    return load_from_stream(blob);
}

}  // namespace SampleUtils