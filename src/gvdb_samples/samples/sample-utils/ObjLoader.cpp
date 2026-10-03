#include "ObjLoader.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string.h>
#include <unordered_map>
#include <donut/core/log.h>
#include <nvrhi/core/autoptr.h>

namespace SampleUtils {

inline void remove_eol(char* ptr) {
    int i = 0;
    while (ptr[i])
        i++;
    i--;
    while (i > 0 && std::isspace((unsigned char)ptr[i])) {
        ptr[i] = '\0';
        i--;
    }
}

inline char* strip_spaces(char* ptr) {
    while (std::isspace((unsigned char)*ptr))
        ptr++;
    return ptr;
}

inline bool getline(const char*& fp, const char* fp_end, char* line, int count) {
    const char* p = fp;
    while (p != fp_end && *p != '\n')
        ++p;

    if (p != fp_end) p += 1;

    int rd_count = int(p - fp);
    if (rd_count > 0) {
        if (rd_count > count - 1) rd_count = count - 1;
        strncpy(line, fp, rd_count);
        line[rd_count] = 0;
    }

    fp = p;
    return rd_count > 0;
}

// One face corner as written in the file: "v", "v/vt", "v//vn" or "v/vt/vn".
// OBJ indexes positions, texture coordinates and normals independently
// (ground.obj: four positions share one normal), 1-based, negative = relative
// to the elements read so far. 0 means "not given".
struct ObjCorner {
    int v = 0;
    int n = 0;
};

inline bool read_corner(char** ptr, ObjCorner& corner) {
    char* base = strip_spaces(*ptr);

    // Detect end of line (negative indices are supported)
    if (!std::isdigit((unsigned char)*base) && *base != '-') return false;

    corner.v = static_cast<int>(std::strtol(base, &base, 10));
    corner.n = 0;

    if (*base == '/') {
        base++;
        // Texture coordinate index (may be empty): parsed and ignored.
        if (*base != '/' && !std::isspace((unsigned char)*base) && *base != '\0') std::strtol(base, &base, 10);
        if (*base == '/') {
            base++;
            if (std::isdigit((unsigned char)*base) || *base == '-')
                corner.n = static_cast<int>(std::strtol(base, &base, 10));
        }
    }

    *ptr = base;
    return true;
}

// 1-based / negative file index -> 0-based index, or -1 when absent or out of range.
inline int resolve_index(int index, size_t count) {
    int j = index < 0 ? int(count) + index : index - 1;
    return (index != 0 && j >= 0 && j < int(count)) ? j : -1;
}

inline dm::float3 safe_normalize(dm::float3 n) {
    float len = dm::length(n);
    return (len > 0.f && std::isfinite(len)) ? n / len : dm::float3(0.f, 1.f, 0.f);
}

inline TriangleMesh load_from_stream(nvrhi::IDataBlob* blob) {
    static constexpr size_t max_line = 1024;
    char line[max_line];

    const char* fp = (const char*)blob->GetDataPtr();
    const char* fp_end = fp + blob->GetSize();

    // Raw file data: positions and normals with their own index spaces, and
    // triangles whose corners are resolved (0-based) position / normal indices.
    struct Corner {
        int v;
        int n;   // -1 = no normal given
    };
    struct Triangle {
        Corner c[3];
    };
    std::vector<dm::float3> filePositions;
    std::vector<dm::float3> fileNormals;
    std::vector<Triangle> triangles;
    size_t skippedFaces = 0;
    bool allCornersHaveNormals = true;

    while (getline(fp, fp_end, line, max_line)) {
        char* ptr = strip_spaces(line);
        if (*ptr == '\0' || *ptr == '#') continue;
        remove_eol(ptr);

        switch (ptr[0]) {
            case 'v':
                if (ptr[1] == 'n') {
                    auto x = std::strtof(ptr + 2, &ptr);
                    auto y = std::strtof(ptr, &ptr);
                    auto z = std::strtof(ptr, &ptr);
                    fileNormals.emplace_back(x, y, z);
                } else if (ptr[1] == 't')
                    ;  // ignore texture coordinates
                else if (std::isspace((unsigned char)ptr[1])) {
                    auto x = std::strtof(ptr + 1, &ptr);
                    auto y = std::strtof(ptr, &ptr);
                    auto z = std::strtof(ptr, &ptr);
                    filePositions.emplace_back(x, y, z);
                }
                break;
            case 'm':  // material
            case 'o':  // object designations
            case 'g':  // group designations
            case 'u':  // material
            case 's':  // smoothing command
                break;
            case 'f':
                if (std::isspace((unsigned char)ptr[1])) {
                    ptr += 2;
                    // Triangle fan over the polygon: (c0, c[i-1], c[i]).
                    Corner first = {-1, -1}, prev = {-1, -1};
                    bool valid = true;
                    ObjCorner raw;
                    for (int i = 0; read_corner(&ptr, raw); ++i) {
                        Corner c;
                        c.v = resolve_index(raw.v, filePositions.size());
                        c.n = resolve_index(raw.n, fileNormals.size());
                        if (c.v < 0) valid = false;   // position index out of range: drop the face
                        if (c.n < 0) allCornersHaveNormals = false;
                        if (i == 0)
                            first = c;
                        else if (i >= 2 && valid)
                            triangles.push_back(Triangle{{first, prev, c}});
                        prev = c;
                    }
                    if (!valid) skippedFaces++;
                }
                break;
        }
    }

    if (skippedFaces > 0)
        donut::log::warning("ObjLoader: %zu faces reference positions that do not exist and were skipped", skippedFaces);

    TriangleMesh mesh;
    mesh.bounds = dm::box3::empty();

    if (allCornersHaveNormals && !fileNormals.empty()) {
        // The file provides normals: one output vertex per distinct
        // (position, normal) pair, so hard edges keep their split normals.
        std::unordered_map<uint64_t, int> remap;
        remap.reserve(filePositions.size() * 2);
        auto vertexOf = [&](const Corner& c) {
            uint64_t key = (uint64_t(uint32_t(c.v)) << 32) | uint32_t(c.n);
            auto it = remap.find(key);
            if (it != remap.end()) return it->second;
            int index = int(mesh.positions.size());
            mesh.positions.push_back(filePositions[c.v]);
            mesh.normals.push_back(safe_normalize(fileNormals[c.n]));
            remap.emplace(key, index);
            return index;
        };
        mesh.indices.reserve(triangles.size());
        for (const Triangle& t : triangles) {
            int a = vertexOf(t.c[0]);
            int b = vertexOf(t.c[1]);
            int c = vertexOf(t.c[2]);
            mesh.indices.emplace_back(a, b, c);
        }
    } else {
        // No (complete) normals: keep the file's positions and reconstruct
        // smooth normals, area weighted (the un-normalized cross product).
        mesh.positions = filePositions;
        mesh.normals.assign(filePositions.size(), dm::float3::zero());
        mesh.indices.reserve(triangles.size());
        for (const Triangle& t : triangles) {
            dm::int3 tri(t.c[0].v, t.c[1].v, t.c[2].v);
            mesh.indices.push_back(tri);
            const dm::float3& v0 = mesh.positions[tri.x];
            const dm::float3& v1 = mesh.positions[tri.y];
            const dm::float3& v2 = mesh.positions[tri.z];
            dm::float3 gn = dm::cross(v1 - v0, v2 - v0);   // zero for degenerate triangles
            mesh.normals[tri.x] += gn;
            mesh.normals[tri.y] += gn;
            mesh.normals[tri.z] += gn;
        }
        for (dm::float3& n : mesh.normals) n = safe_normalize(n);
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
