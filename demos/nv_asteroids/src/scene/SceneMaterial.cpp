#include "scene/SceneMaterial.h"

#include <donut/core/json.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/ThreadPool.h>

#include <json/json.h>

using namespace donut;
using namespace donut::math;

void SceneMaterial::FillConstants(MaterialConstants2018& constants) const
{
    // Asteroids.exe: 0x1400741B0
    constants.diffuseColor = diffuseColor;
    constants.useDiffuseTexture = diffuseTexture ? 1 : 0;
    constants.specularColor = specularColor;
    constants.specularTextureType = specularTextureType;
    constants.emissiveColor = emissiveColor;
    constants.useEmissiveTexture = emissiveTexture ? 1 : 0;
    constants.roughness = 1.f - shininess;
    constants.opacity = opacity;
    constants.useNormalsTexture = normalsTexture ? 1 : 0;
    constants.materialID = materialID;
}

static void DropFailedTexture(std::shared_ptr<engine::LoadedTexture>& texture)
{
    if (texture && !texture->texture)
        texture.reset();
}

void SceneMaterial::CreateResources(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
    engine::CommonRenderPasses& commonPasses, nvrhi::IBindingLayout* bindingLayout, bool forceSpecularType3)
{
    // Asteroids.exe: 0x14000FB90 (asteroid types) / 0x1400541B0 (space objects)
    DropFailedTexture(diffuseTexture);
    DropFailedTexture(specularTexture);
    DropFailedTexture(normalsTexture);
    DropFailedTexture(emissiveTexture);

    if (specularTexture)
        specularTextureType = forceSpecularType3 ? 3 : (specularTexture->originalBitsPerPixel != 8 ? 2 : 1);
    else
        specularTextureType = 0;

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants2018);
    bufferDesc.debugName = name;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    materialConstants = device->createBuffer(bufferDesc);

    MaterialConstants2018 constants{};
    FillConstants(constants);
    commandList->writeBuffer(materialConstants, &constants, sizeof(constants));
    dirty = false;

    // Asteroids.exe: 0x14000F780 / 0x1400526C0 (pixel stage only)
    auto textureOr = [](const std::shared_ptr<engine::LoadedTexture>& texture, nvrhi::ITexture* fallback) -> nvrhi::ITexture*
    {
        return texture ? texture->texture.Get() : fallback;
    };

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, materialConstants),
        nvrhi::BindingSetItem::Sampler(0, commonPasses.m_AnisotropicWrapSampler),
        nvrhi::BindingSetItem::Texture_SRV(0, textureOr(diffuseTexture, commonPasses.m_GrayTexture)),
        nvrhi::BindingSetItem::Texture_SRV(1, textureOr(specularTexture, commonPasses.m_BlackTexture)),
        nvrhi::BindingSetItem::Texture_SRV(2, textureOr(normalsTexture, commonPasses.m_BlackTexture)),
        nvrhi::BindingSetItem::Texture_SRV(3, textureOr(emissiveTexture, commonPasses.m_BlackTexture)),
    };
    bindingSet = device->createBindingSet(setDesc, bindingLayout);
}

void SceneMaterial::UpdateConstants(nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x140058530
    if (!dirty || !materialConstants)
        return;

    MaterialConstants2018 constants{};
    FillConstants(constants);
    commandList->writeBuffer(materialConstants, &constants, sizeof(constants));
    dirty = false;
}

nvrhi::BindingLayoutHandle CreateMaterialBindingLayout(nvrhi::IDevice* device)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(3),
    };
    return device->createBindingLayout(layoutDesc);
}

static float3 ReadColor(const Json::Value& node)
{
    if (!node.isArray())
        return float3(0.f);
    return float3(node[0].asFloat(), node[1].asFloat(), node[2].asFloat());
}

static std::shared_ptr<engine::LoadedTexture> LoadMaterialTexture(const Json::Value& textures, const char* key,
    bool sRGB, const std::filesystem::path& basePath, const MaterialLoadParams& params)
{
    const Json::Value& value = textures[key];
    if (!value.isString() || !params.textureCache)
        return nullptr;

    const std::filesystem::path path = basePath / std::filesystem::path(value.asString());

    // deviation: the 2018 TextureCache scheduled the decode on a concurrency::task_group; donut's
    // cache uses its ThreadPool, or a synchronous decode when no pool is given. In both cases the
    // GPU upload happens in TextureCache::ProcessRenderingThreadCommands.
    if (params.threadPool)
        return params.textureCache->LoadTextureFromFileAsync(path, sRGB, *params.threadPool);
    return params.textureCache->LoadTextureFromFileDeferred(path, sRGB);
}

std::shared_ptr<SceneMaterial> ParseSceneMaterial(const std::string& name, const Json::Value& node,
    const std::filesystem::path& textureBasePath, const MaterialLoadParams& params)
{
    // Asteroids.exe: 0x1400119C0 (asteroid types) / 0x140056FD0 (space objects)
    auto material = std::make_shared<SceneMaterial>();
    material->name = name;
    // Both keys are read without defaults: a missing entry yields 0 (including the opacity).
    material->shininess = node["Shininess"].asFloat();
    material->opacity = node["Opacity"].asFloat();
    material->diffuseColor = ReadColor(node["Diffuse"]);
    material->specularColor = ReadColor(node["Specular"]);
    material->emissiveColor = ReadColor(node["Emittance"]);

    const Json::Value& textures = node["Textures"];
    material->diffuseTexture = LoadMaterialTexture(textures, "Diffuse", true, textureBasePath, params);
    material->normalsTexture = LoadMaterialTexture(textures, "Bumpmap", false, textureBasePath, params);
    material->specularTexture = LoadMaterialTexture(textures, "Specular", true, textureBasePath, params);
    material->emissiveTexture = LoadMaterialTexture(textures, "Emittance", true, textureBasePath, params);

    if (material->opacity < 1.f)
        material->domain = SceneMaterialDomain::Transparent;

    return material;
}

bool LoadMaterialsFile(vfs::IFileSystem& fs, const std::filesystem::path& materialFile,
    const std::filesystem::path& textureBasePath, const MaterialLoadParams& params, MaterialMap& materials)
{
    // donut::json::LoadFromFile logs "Couldn't read file" / parse errors like the 2018 helper 0x140073C80.
    Json::Value root;
    if (!json::LoadFromFile(fs, materialFile, root))
        return false;

    if (!root.isObject())
        return false;

    for (const std::string& name : root.getMemberNames())
        materials[name] = ParseSceneMaterial(name, root[name], textureBasePath, params);

    return true;
}
