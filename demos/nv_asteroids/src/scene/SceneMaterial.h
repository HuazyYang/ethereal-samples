#pragma once

// 2018 framework material as used by the demo (0xB0 bytes in Asteroids.exe; constants writer
// 0x1400741B0, binding set 0x14000F780, JSON loaders 0x1400119C0 / 0x140056FD0).
//
// deviation: donut::engine::Material is a metal-rough / spec-gloss PBR material with donut's
// MaterialConstants. The demo shaders (gbuffer_ps, forward_ps, asteroid meshlet PS) consume the
// 2018 64-byte c_Material cbuffer below and four fixed texture slots, so the 2018 material is kept
// as a demo-side struct.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace donut::engine
{
    struct LoadedTexture;
    class TextureCache;
    class CommonRenderPasses;
    class ThreadPool;
}

namespace donut::vfs
{
    class IFileSystem;
}

namespace Json
{
    class Value;
}

// cbuffer c_Material (b0, pixel stage) — layout from the gbuffer_ps / forward_ps reflection.
struct MaterialConstants2018
{
    donut::math::float3 diffuseColor;
    int useDiffuseTexture;
    donut::math::float3 specularColor;
    int specularTextureType;    // 0 none, 1 single-channel (8 bpp), 2 color, 3 forced (type flag 0x40)
    donut::math::float3 emissiveColor;
    int useEmissiveTexture;
    float roughness;            // 1 - shininess
    float opacity;
    int useNormalsTexture;
    int materialID;
};
static_assert(sizeof(MaterialConstants2018) == 64, "must match c_Material");

enum class SceneMaterialDomain : int
{
    Opaque = 0,
    Transparent = 2,    // opacity < 1
};

// Asteroids.exe: Material (0xB0 bytes; created by the materials.json loaders, never freed in 2018)
struct SceneMaterial
{
    std::string name;
    SceneMaterialDomain domain = SceneMaterialDomain::Opaque;

    std::shared_ptr<donut::engine::LoadedTexture> diffuseTexture;
    std::shared_ptr<donut::engine::LoadedTexture> specularTexture;
    std::shared_ptr<donut::engine::LoadedTexture> normalsTexture;     // JSON "Bumpmap"
    std::shared_ptr<donut::engine::LoadedTexture> emissiveTexture;    // JSON "Emittance"

    nvrhi::BufferHandle materialConstants;
    nvrhi::BindingSetHandle bindingSet;

    donut::math::float3 diffuseColor = 0.f;
    donut::math::float3 specularColor = 0.f;
    donut::math::float3 emissiveColor = 0.f;
    float shininess = 0.f;
    float opacity = 1.f;
    int specularTextureType = 0;
    int materialID = 0;
    bool dirty = true;      // set when opacity etc. change at runtime (ship glows); constants must be re-uploaded

    void FillConstants(MaterialConstants2018& constants) const;

    // Drops textures whose load failed, derives specularTextureType, creates the "c_Material" constant
    // buffer (named after the material), uploads it and creates the pixel-stage binding set.
    // 'forceSpecularType3' reproduces the 2018 per-object attribute bit 0x40.
    void CreateResources(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        donut::engine::CommonRenderPasses& commonPasses, nvrhi::IBindingLayout* bindingLayout,
        bool forceSpecularType3 = false);

    // Re-uploads the constants if 'dirty' (2018: Material+0xAC flag checked by the render passes).
    void UpdateConstants(nvrhi::ICommandList* commandList);
};

// Pixel-stage material layout shared by every demo material pass:
// b0 c_Material, s0 s_MaterialSampler, t0 t_Diffuse, t1 t_Specular, t2 t_Normals, t3 t_Emissive.
// 2018: CommonRenderPasses+328 (a material binding layout created by the framework).
nvrhi::BindingLayoutHandle CreateMaterialBindingLayout(nvrhi::IDevice* device);

using MaterialMap = std::map<std::string, std::shared_ptr<SceneMaterial>>;

struct MaterialLoadParams
{
    donut::engine::TextureCache* textureCache = nullptr;
    donut::engine::ThreadPool* threadPool = nullptr;   // when set, textures are decoded asynchronously
};

// Parses one material entry of a materials.json file:
//   "Name": { "Diffuse":[r,g,b], "Specular":[r,g,b], "Emittance":[r,g,b], "Shininess":s, "Opacity":o,
//             "Textures": { "Diffuse":"..", "Bumpmap":"..", "Specular":"..", "Emittance":".." } }
// Texture paths are resolved against 'textureBasePath'. sRGB: Diffuse/Specular/Emittance yes, Bumpmap no.
std::shared_ptr<SceneMaterial> ParseSceneMaterial(const std::string& name, const Json::Value& node,
    const std::filesystem::path& textureBasePath, const MaterialLoadParams& params);

// Loads every entry of a materials.json file into 'materials' (existing names are replaced).
bool LoadMaterialsFile(donut::vfs::IFileSystem& fs, const std::filesystem::path& materialFile,
    const std::filesystem::path& textureBasePath, const MaterialLoadParams& params, MaterialMap& materials);
