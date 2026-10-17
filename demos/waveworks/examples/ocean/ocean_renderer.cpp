/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/


#include <cstdint>
#include "ocean_renderer.h"
#include "asset_utils.h"
#include <sstream>

#include <shaders/bytecode/OceanSurfaceVS.spirv.h>
#include <shaders/bytecode/OceanSurfaceHS.spirv.h>
#include <shaders/bytecode/OceanSurfaceDS.spirv.h>
#include <shaders/bytecode/OceanSurfacePS.spirv.h>
#include <shaders/bytecode/BeachWaterPS.spirv.h>
#include <shaders/bytecode/BeachFoamPS.spirv.h>
#include <shaders/bytecode/BeachOceanSurfaceDS.spirv.h>
#include <shaders/bytecode/MarkerVS.spirv.h>
#include <shaders/bytecode/MarkerPS.spirv.h>
#include <shaders/bytecode/OceanSurfaceVS.dxbc.h>
#include <shaders/bytecode/OceanSurfaceHS.dxbc.h>
#include <shaders/bytecode/OceanSurfaceDS.dxbc.h>
#include <shaders/bytecode/OceanSurfacePS.dxbc.h>
#include <shaders/bytecode/BeachWaterPS.dxbc.h>
#include <shaders/bytecode/BeachFoamPS.dxbc.h>
#include <shaders/bytecode/BeachOceanSurfaceDS.dxbc.h>
#include "../beach/beach_lighting.h"
#include "../beach/beach_shallow_water.h"
#include <shaders/bytecode/MarkerVS.dxbc.h>
#include <shaders/bytecode/MarkerPS.dxbc.h>

namespace
{
nvrhi::ObjectType NativeTextureObject(nvrhi::GraphicsAPI backend)
{
    switch (backend)
    {
    case nvrhi::GraphicsAPI::D3D11: return nvrhi::ObjectTypes::D3D11_Resource;
    case nvrhi::GraphicsAPI::D3D12: return nvrhi::ObjectTypes::D3D12_Resource;
    case nvrhi::GraphicsAPI::VULKAN: return nvrhi::ObjectTypes::VK_Image;
    }
    return 0;
}

// Beach water uses scene-linear radiometric units and coefficients in inverse
// metres. These values describe clear coastal water with a thin, exponentially
// decaying suspension of pale sand; they are material inputs rather than color
// grading controls.
constexpr float BeachMaxOpticalDepth = 256.f;
constexpr gfsdk_float3 BeachSedimentAbsorption = { 0.060f, 0.045f, 0.035f };
constexpr float BeachSedimentDepth = 1.25f;
constexpr gfsdk_float3 BeachSedimentScattering = { 0.38f, 0.31f, 0.20f };
constexpr float BeachSedimentConcentration = 0.32f;
constexpr float BeachRefractionDistanceLimit = 18.f;
constexpr gfsdk_float3 BeachFoamBaseColor = { 0.92f, 0.95f, 0.93f };
constexpr float BeachFoamRoughness = 0.82f;
constexpr float BeachBreakingWaveRatio = 0.78f;
constexpr float BeachShorelineFadeDepth = 0.45f;
constexpr float BeachFoamOpticalDepth = 2.6f;
constexpr float BeachFoamEnergyScale = 0.42f;
constexpr float BeachFoamPatternScale = 0.16f;
}




struct OCEAN_VS_HS_DS_CBUFFER
{
	// Data used in vertex shader
	gfsdk_float4x4 g_matViewProj;
	gfsdk_float3   g_eyePos;
	float          g_meanOceanLevel;
	// x16 bytes boundary
	float          g_useDiamondPattern;

	// Data used in hull shader
	float          g_dynamicTesselationAmount;
	float          g_staticTesselationOffset;

	// Data used in domain shader
	// Wind waves related data
	float          g_cascade0UVScale;
	// x16 bytes boundary
	float          g_cascade1UVScale;
	float          g_cascade2UVScale;
	float          g_cascade3UVScale;
	float          g_cascade0UVOffset;
	// x16 bytes boundary
	float          g_cascade1UVOffset;
	float          g_cascade2UVOffset;
	float          g_cascade3UVOffset;
	float          g_UVWarpingAmplitude;
	// x16 bytes boundary
	float          g_UVWarpingFrequency;

	// Local waves related data
	float          g_localWavesSimulationDomainWorldspaceSize;
	gfsdk_float2   g_localWavesSimulationDomainWorldspaceCenter;
	float          g_beachScene;
	float          pad0;
	float          pad1;
	float          pad2;
	gfsdk_float2   g_bathymetryOrigin;
	gfsdk_float2   g_bathymetryInverseExtent;
	float          g_breakingWaveRatio;
	float          g_shorelineFadeDepth;
	float          pad3;
	float          pad4;
};

static_assert(sizeof(OCEAN_VS_HS_DS_CBUFFER) == 192,
	"Ocean VS/HS/DS constant buffer layout must match HLSL");

struct OCEAN_PS_CBUFFER
{
	// Defined by wind waves simulation
	float g_cascadeToCascadeScale;
	float g_windWavesTextureSizeInTexels;
	float g_UVWarpingAmplitude;
	float g_UVWarpingFrequency;
											// x16 bytes boundary
	float g_windWavesFoamWhitecapsThreshold;

	// Defined by local waves simulation
	float g_localWavesTextureSizeInTexels;
	float g_localWavesFoamWhitecapsThreshold;
	float g_SimulationDomainSize;
											// x16 bytes boundary
	gfsdk_float2 g_SimulationDomainCenter;

	// Defined by application
	float g_beckmannRoughness;
	float g_showCascades;
											// x16 bytes boundary
	gfsdk_float3 g_sunDirection;
	float g_sunIntensity;
											// x16 bytes boundary
	gfsdk_float3 g_eyePos;
	float g_useMicrofacetFresnel;
											// x16 bytes boundary
	float g_useMicrofacetSpecular;
	float g_useMicrofacetReflection;

	// CBs must be multiple of 16 bytes large
	float pad0;
	float pad1;
	float g_beachScene;
	float g_beachShallowWater;
	float g_beachShoreFoam;
	float g_beachFoamIntensity;
	// Beach-only transmissive material extension. The standalone ocean shader
	// stops at the preceding 16-byte boundary and retains its original layout.
	gfsdk_float2 g_inverseViewport;
	float g_nearClip;
	float g_farClip;
	gfsdk_float3 g_waterAbsorption;
	float g_waterIOR;
	gfsdk_float3 g_waterScattering;
	float g_maxOpticalDepth;
	gfsdk_float3 g_sedimentAbsorption;
	float g_sedimentDepth;
	gfsdk_float3 g_sedimentScattering;
	float g_sedimentConcentration;
	gfsdk_float3 g_sunRadiance;
	float g_refractionDistanceLimit;
	gfsdk_float3 g_foamBaseColor;
	float g_foamRoughness;
	float g_breakingWaveRatio;
	float g_foamOpticalDepth;
	float g_foamEnergyScale;
	float g_foamPatternScale;
	gfsdk_float4x4 g_worldToClip;
};

static_assert(sizeof(OCEAN_PS_CBUFFER) == 304,
	"Ocean PS constant buffer layout must match HLSL");

struct OCEAN_VS_CBUFFER_PERINSTANCE_ENTRY
{
	gfsdk_float2 g_patchWorldspaceOrigin;
	float g_patchWorldspaceScale;
	float g_patchMorphConstantAndSign;
};

struct MARKER_VS_CBUFFER
{
	gfsdk_float4x4 g_matViewProj;
};

uint32_t getNumMipLevels(uint32_t const squareTextureSize)
{
	uint32_t mipLevels = 1;
	uint32_t texSize = squareTextureSize;
	while (texSize > 1)
	{
		mipLevels++;
		texSize /= 2;
	}
	return mipLevels;
}

bool OceanRenderer::Init()
{
	nvrhi::CommandListParameters cmdListParams;
	m_pDevice->createCommandList(cmdListParams, &m_hCommandList);
	m_hCommandList->open();

	// BeachScenePipeline owns beach depth. Standalone ocean keeps the original
	// renderer-owned allocation for ABI and behavior compatibility.
	if (!m_pState->bBeachScene && m_pState->hDepthBufferTexture != nullptr)
	{
		m_pState->hDepthBufferTexture = nullptr;
	}
	nvrhi::TextureDesc td;
	if (m_pState->hDepthBufferTexture == nullptr)
	{
		if (m_pState->bBeachScene) return false;
		td.width = m_backBufferWidth;
		td.height = m_backBufferHeight;
		td.sampleCount = m_sampleCount;
		td.debugName = "Depth buffer created in OceanRenderer::Init()";
		td.format = nvrhi::Format::D24S8;
		td.isRenderTarget = true;
		td.clearValue = nvrhi::Color(1.0f);
		td.useClearValue = true;
		td.isTypeless = true;
		m_pDevice->createTexture(td, &m_pState->hDepthBufferTexture);
		m_hCommandList->beginTrackingTextureState(m_pState->hDepthBufferTexture,
			nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
		m_hCommandList->setTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
		m_hCommandList->commitBarriers();
	}

	// Initializing frame buffer #0
	td.width = m_backBufferWidth;
	td.height = m_backBufferHeight;
	td.sampleCount = m_sampleCount;
	td.isRenderTarget = true;
	td.isTypeless = false;
	td.debugName = "proxy color target";
	td.format = m_pState->bBeachScene ? nvrhi::Format::RGBA16_FLOAT :
		m_backend == nvrhi::GraphicsAPI::VULKAN
		? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
	m_pDevice->createTexture(td, &m_hInitTimeRenderTargetTexture);
	nvrhi::FramebufferDesc fbDesc;
	fbDesc.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(m_hInitTimeRenderTargetTexture).setArraySlice(0).setMipLevel(0));
	fbDesc.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_pState->hDepthBufferTexture).setArraySlice(0).setMipLevel(0));
	m_pDevice->createFramebuffer(fbDesc, &m_hInitTimeFB);


	// Loading shaders
	const bool useVk = m_backend == nvrhi::GraphicsAPI::VULKAN;
	const void *VSOceanBinary = useVk ? static_cast<const void *>(g_OceanSurfaceVS_spirv) : g_OceanSurfaceVS_dxbc;
	size_t VSOceanBinarySize = useVk ? sizeof(g_OceanSurfaceVS_spirv) : sizeof(g_OceanSurfaceVS_dxbc);
	const void *HSOceanBinary = useVk ? static_cast<const void *>(g_OceanSurfaceHS_spirv) : g_OceanSurfaceHS_dxbc;
	size_t HSOceanBinarySize = useVk ? sizeof(g_OceanSurfaceHS_spirv) : sizeof(g_OceanSurfaceHS_dxbc);
	const void *DSOceanBinary = m_pState->bBeachScene
		? (useVk ? static_cast<const void *>(g_BeachOceanSurfaceDS_spirv)
			: g_BeachOceanSurfaceDS_dxbc)
		: (useVk ? static_cast<const void *>(g_OceanSurfaceDS_spirv)
			: g_OceanSurfaceDS_dxbc);
	size_t DSOceanBinarySize = m_pState->bBeachScene
		? (useVk ? sizeof(g_BeachOceanSurfaceDS_spirv)
			: sizeof(g_BeachOceanSurfaceDS_dxbc))
		: (useVk ? sizeof(g_OceanSurfaceDS_spirv)
			: sizeof(g_OceanSurfaceDS_dxbc));
	const void *PSOceanBinary = m_pState->bBeachScene
		? (useVk ? static_cast<const void *>(g_BeachWaterPS_spirv) : g_BeachWaterPS_dxbc)
		: (useVk ? static_cast<const void *>(g_OceanSurfacePS_spirv) : g_OceanSurfacePS_dxbc);
	size_t PSOceanBinarySize = m_pState->bBeachScene
		? (useVk ? sizeof(g_BeachWaterPS_spirv) : sizeof(g_BeachWaterPS_dxbc))
		: (useVk ? sizeof(g_OceanSurfacePS_spirv) : sizeof(g_OceanSurfacePS_dxbc));
	const void *PSFoamBinary = useVk
		? static_cast<const void *>(g_BeachFoamPS_spirv) : g_BeachFoamPS_dxbc;
	size_t PSFoamBinarySize = useVk
		? sizeof(g_BeachFoamPS_spirv) : sizeof(g_BeachFoamPS_dxbc);
	const void *VSOceanMarkerBinary = useVk ? static_cast<const void *>(g_MarkerVS_spirv) : g_MarkerVS_dxbc;
	size_t VSOceanMarkerBinarySize = useVk ? sizeof(g_MarkerVS_spirv) : sizeof(g_MarkerVS_dxbc);
	const void *PSOceanMarkerBinary = useVk ? static_cast<const void *>(g_MarkerPS_spirv) : g_MarkerPS_dxbc;
	size_t PSOceanMarkerBinarySize = useVk ? sizeof(g_MarkerPS_spirv) : sizeof(g_MarkerPS_dxbc);


	// Creating shaders
	m_hOceanSurfaceVS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), VSOceanBinary, VSOceanBinarySize);
	if (m_hOceanSurfaceVS == nullptr) return false;

	m_hOceanSurfaceHS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Hull), HSOceanBinary, HSOceanBinarySize);
	if (m_hOceanSurfaceHS == nullptr) return false;

	m_hOceanSurfaceDS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Domain), DSOceanBinary, DSOceanBinarySize);
	if (m_hOceanSurfaceDS == nullptr) return false;

	m_hOceanSurfacePS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSOceanBinary, PSOceanBinarySize);
	if (m_hOceanSurfacePS == nullptr) return false;
	m_hBeachFoamPS = waveworks::graphics::createGraphicsShader(m_pDevice, 
		nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSFoamBinary, PSFoamBinarySize);
	if (m_pState->bBeachScene && m_hBeachFoamPS == nullptr) return false;
	m_hMarkerVS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), VSOceanMarkerBinary, VSOceanMarkerBinarySize);
	if (m_hMarkerVS == nullptr) return false;

	m_hMarkerPS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSOceanMarkerBinary, PSOceanMarkerBinarySize);
	if (m_hMarkerPS == nullptr) return false;


	// Creating input layout for ocean surface rendering 
	nvrhi::VertexAttributeDesc oceanSurceaceLayoutDesc[] = {
		{ "POSITION", nvrhi::Format::RG32_FLOAT, 1, 0, 0, 8, false },
	};

	m_pDevice->createInputLayout(oceanSurceaceLayoutDesc, 1, m_hOceanSurfaceVS, &m_hOceanSurfaceLayout);

	// Creating ocean surface vertex and index buffers
	uint32_t numVertices = 0;
	uint32_t numIndices = 0;
	gfsdk_float2 *pVertexData = nullptr;
	uint32_t *pIndexData = nullptr;
	std::vector<gfsdk_float2> oceanVertices;
	std::vector<uint32_t> oceanIndices;
	
	GFSDK_WaveWorks_Quadtree_GetGeometryData(m_pState->hOceanQuadtree, &numVertices, &pVertexData, &numIndices, &pIndexData);

	oceanVertices.assign(pVertexData, pVertexData + numVertices);
	oceanIndices.assign(pIndexData, pIndexData + numIndices);

	nvrhi::BufferDesc bd;
	bd.byteSize = numVertices * sizeof(gfsdk_float2);
	bd.isVertexBuffer = true;
	bd.isIndexBuffer = false;
	m_pDevice->createBuffer(bd, &m_hOceanSurfaceVB);
	m_hCommandList->beginTrackingBufferState(m_hOceanSurfaceVB, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hOceanSurfaceVB, oceanVertices.data(), bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hOceanSurfaceVB, nvrhi::ResourceStates::VertexBuffer);
	m_hCommandList->commitBarriers();

	bd.byteSize = numIndices * sizeof(uint32_t);
	bd.isVertexBuffer = false;
	bd.isIndexBuffer = true;
	m_pDevice->createBuffer(bd, &m_hOceanSurfaceIB);
	m_hCommandList->beginTrackingBufferState(m_hOceanSurfaceIB, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hOceanSurfaceIB, oceanIndices.data(), bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hOceanSurfaceIB, nvrhi::ResourceStates::IndexBuffer);
	m_hCommandList->commitBarriers();

	// Creating textures
	m_hFoamIntensityTexture = CreateTextureFromFile(m_pDevice, m_hCommandList, (GetMediaFolder() / "foam_intensity.dds").generic_string().c_str());
	m_hFoamBubblesTexture = CreateTextureFromFile(m_pDevice, m_hCommandList, (GetMediaFolder() / "foam_bubbles.dds").generic_string().c_str());
	m_hWindGustsTexture = CreateTextureFromFile(m_pDevice, m_hCommandList, (GetMediaFolder() / "wind_gusts.dds").generic_string().c_str());

	if (m_hFoamIntensityTexture == nullptr) return false;
	if (m_hFoamBubblesTexture == nullptr) return false;
	if (m_hWindGustsTexture == nullptr) return false;

	// Creating samplers
	nvrhi::SamplerDesc samplerDesc;
	samplerDesc.magFilter = true;
	samplerDesc.minFilter = true;
	samplerDesc.mipFilter = true;
	samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
	samplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
	samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
	m_pDevice->createSampler(samplerDesc, &m_hBilinearSampler);

	samplerDesc.maxAnisotropy = 4.0f;
	m_pDevice->createSampler(samplerDesc, &m_hAnisotropicSampler);

	samplerDesc.maxAnisotropy = 1.0f;
	samplerDesc.addressU = nvrhi::SamplerAddressMode::Clamp;
	samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
	samplerDesc.addressW = nvrhi::SamplerAddressMode::Clamp;
	m_pDevice->createSampler(samplerDesc, &m_hBilinearClampToEdgeSampler);

	// Creating NVRHI textures from native objects exposed from WaveWorks
	GFSDK_WaveWorks_Wind_Waves_Rendering_Data windWavesRenderingData;
	GFSDK_WaveWorks_Local_Waves_Rendering_Data localWavesRenderingData;
	GFSDK_WaveWorks_Wind_Waves_GetDataForRendering(m_pState->hOceanWindSimulation, windWavesRenderingData);
	GFSDK_WaveWorks_Local_Waves_GetDataForRendering(m_pState->hOceanLocalSimulation, localWavesRenderingData);

	nvrhi::TextureDesc texArrayDesc;
	texArrayDesc.width = windWavesRenderingData.size_of_texture_arrays;
	texArrayDesc.height = windWavesRenderingData.size_of_texture_arrays;
	texArrayDesc.format = nvrhi::Format::RGBA16_FLOAT;
	texArrayDesc.isUAV = true;
	texArrayDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
	texArrayDesc.arraySize = 4;
	texArrayDesc.mipLevels = getNumMipLevels(windWavesRenderingData.size_of_texture_arrays);
	texArrayDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	texArrayDesc.keepInitialState = true;
	const auto nativeObjectType = NativeTextureObject(m_backend);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, windWavesRenderingData.displacements_texture_array, texArrayDesc, &m_hWindWavesDisplacementsTextureArray);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, windWavesRenderingData.gradients_texture_array, texArrayDesc, &m_hWindWavesGradientsTextureArray);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, windWavesRenderingData.moments_texture_array, texArrayDesc, &m_hWindWavesMomentsTextureArray);

	nvrhi::TextureDesc texDesc;
	texDesc.width = localWavesRenderingData.size_of_texture;
	texDesc.height = localWavesRenderingData.size_of_texture;
	texDesc.format = nvrhi::Format::RGBA16_FLOAT;
	texDesc.isUAV = true;
	texDesc.dimension = nvrhi::TextureDimension::Texture2D;
	texDesc.mipLevels = getNumMipLevels(localWavesRenderingData.size_of_texture);
	texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	texDesc.keepInitialState = true;
	m_pDevice->createHandleForNativeTexture(nativeObjectType, localWavesRenderingData.displacements_texture, texDesc, &m_hLocalWavesDisplacementsTexture);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, localWavesRenderingData.gradients_texture, texDesc, &m_hLocalWavesGradientsTexture);
		
	// Creating constant buffer for vertex/domain shader
	nvrhi::BufferDesc cbDesc;
	cbDesc.byteSize = sizeof(OCEAN_VS_HS_DS_CBUFFER);
	cbDesc.debugName = "Ocean VS/HS/DS CB";
	cbDesc.isConstantBuffer = true;
	cbDesc.isVolatile = false;
	m_pDevice->createBuffer(cbDesc, &m_hOceanSurfaceVSHSDSCB);

	// Creating constant buffer for pixel shader
	cbDesc.byteSize = sizeof(OCEAN_PS_CBUFFER);
	cbDesc.debugName = "Ocean PS CB";
	cbDesc.isConstantBuffer = true;
	cbDesc.isVolatile = false;
	m_pDevice->createBuffer(cbDesc, &m_hOceanSurfacePSCB);

	// Creating perinstance constant buffer for vertex shader
	cbDesc.byteSize = 4096*sizeof(OCEAN_VS_CBUFFER_PERINSTANCE_ENTRY); // 64KB or 4096*4 floats is maximum amout of data allowed for constant buffers
	cbDesc.debugName = "Ocean VS per instance CB";
	cbDesc.isConstantBuffer = true;
	cbDesc.isVolatile = true;
	cbDesc.maxVersions = 4096;
	m_pDevice->createBuffer(cbDesc, &m_hOceanSurfacePerInstanceCB);

	// Creating ocean draw call state members
	nvrhi::BlendState blendState;
	// The beach water shader composes scene transmission and reflection itself,
	// so its radiance replaces only the pixels covered by water geometry.
	blendState.targets[0].blendEnable = false;

	nvrhi::RasterState rasterState;
	rasterState.fillMode = nvrhi::RasterFillMode::Solid;
	rasterState.cullMode = nvrhi::RasterCullMode::Front;
	rasterState.scissorEnable = false;
	rasterState.depthClipEnable = true;

	nvrhi::DepthStencilState depthStencilState;
	depthStencilState.depthTestEnable = true;
	depthStencilState.depthWriteEnable = m_pState->bBeachScene
		? false
		: true;
	depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
	depthStencilState.stencilEnable = false;


	nvrhi::RenderState renderState;
	renderState.blendState = blendState;
	renderState.depthStencilState = depthStencilState;
	renderState.rasterState = rasterState;

	nvrhi::GraphicsPipelineDesc psoDesc;
	waveworks::graphics::GraphicsBindingLayoutDesc blDesc;
	waveworks::graphics::GraphicsBindingSetDesc bsDesc;

	nvrhi::TextureDesc tdWindDisplacements = m_hWindWavesDisplacementsTextureArray->getDesc();
	nvrhi::TextureDesc tdLocalDisplacements = m_hLocalWavesDisplacementsTexture->getDesc();
	nvrhi::TextureDesc tdFoamBubbles = m_hFoamBubblesTexture->getDesc();
	nvrhi::TextureDesc tdFoamIntensity = m_hFoamIntensityTexture->getDesc();
	nvrhi::TextureDesc tdWindGusts = m_hWindGustsTexture->getDesc();

	blDesc.VS = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
					nvrhi::BindingLayoutItem::ConstantBuffer(1) };
	blDesc.HS = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
	blDesc.DS = { nvrhi::BindingLayoutItem::Texture_SRV(0),
					nvrhi::BindingLayoutItem::Texture_SRV(1),
					nvrhi::BindingLayoutItem::Sampler(0),
					nvrhi::BindingLayoutItem::Sampler(1),
					nvrhi::BindingLayoutItem::ConstantBuffer(0) };
	blDesc.PS = { nvrhi::BindingLayoutItem::Texture_SRV(0),
				   nvrhi::BindingLayoutItem::Texture_SRV(1),
				   nvrhi::BindingLayoutItem::Texture_SRV(2),
				   nvrhi::BindingLayoutItem::Texture_SRV(3),
				   nvrhi::BindingLayoutItem::Texture_SRV(4),
				   nvrhi::BindingLayoutItem::Texture_SRV(5),
				   nvrhi::BindingLayoutItem::Texture_SRV(6),
				   nvrhi::BindingLayoutItem::Texture_SRV(7),
				   nvrhi::BindingLayoutItem::Sampler(0),
				   nvrhi::BindingLayoutItem::Sampler(1),
				   nvrhi::BindingLayoutItem::Sampler(2),
				   nvrhi::BindingLayoutItem::ConstantBuffer(0) };
	if (m_pState->bBeachScene)
	{
		if (!m_hBeachOpaqueSceneTexture || !m_hBeachLinearDepthTexture ||
			!m_hBeachBathymetryTexture || m_beachBathymetryExtent.x == 0.f ||
			m_beachBathymetryExtent.y == 0.f ||
			m_hBeachBathymetryTexture->getDesc().format != nvrhi::Format::R32_FLOAT)
			return false;
		blDesc.DS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(2));
		blDesc.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(8));
		blDesc.PS.push_back(nvrhi::BindingLayoutItem::Texture_SRV(9));
		AppendBeachLightingLayout(blDesc);
		AppendBeachShallowLayout(blDesc, true);
	}
	m_hOceanSurfaceBindingLayout = waveworks::graphics::createBindingLayout(m_pDevice, blDesc);

	bsDesc.VS = { nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfacePerInstanceCB ),
					nvrhi::BindingSetItem::ConstantBuffer(1, m_hOceanSurfaceVSHSDSCB ) };
	bsDesc.HS = { nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfaceVSHSDSCB ) };
	bsDesc.DS = { nvrhi::BindingSetItem::Texture_SRV(0, m_hWindWavesDisplacementsTextureArray, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(0,tdWindDisplacements.mipLevels,0,tdWindDisplacements.arraySize) ),
					nvrhi::BindingSetItem::Texture_SRV(1, m_hLocalWavesDisplacementsTexture, nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler ),
					nvrhi::BindingSetItem::Sampler(1, m_hBilinearClampToEdgeSampler ),
					nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfaceVSHSDSCB) };
	bsDesc.PS = { nvrhi::BindingSetItem::Texture_SRV(0, m_hWindWavesGradientsTextureArray, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(0,tdWindDisplacements.mipLevels,0,tdWindDisplacements.arraySize)),
					nvrhi::BindingSetItem::Texture_SRV(1, m_hWindWavesMomentsTextureArray, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(0,tdWindDisplacements.mipLevels,0,tdWindDisplacements.arraySize)),
					nvrhi::BindingSetItem::Texture_SRV(2, m_hLocalWavesDisplacementsTexture,  nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Texture_SRV(3, m_hLocalWavesGradientsTexture,  nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Texture_SRV(4, m_hFoamIntensityTexture, nvrhi::Format::RGBA8_UNORM),
					nvrhi::BindingSetItem::Texture_SRV(5, m_hFoamBubblesTexture,  nvrhi::Format::RGBA8_UNORM),
					nvrhi::BindingSetItem::Texture_SRV(6, m_hWindGustsTexture,  nvrhi::Format::RGBA8_UNORM),
					nvrhi::BindingSetItem::Texture_SRV(7, m_pState->hDynamicSkyDomeTexture,
						nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler),
					nvrhi::BindingSetItem::Sampler(1, m_hAnisotropicSampler),
					nvrhi::BindingSetItem::Sampler(2, m_hBilinearClampToEdgeSampler ),
					nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfacePSCB ) };
	if (m_pState->bBeachScene)
	{
		bsDesc.DS.push_back(nvrhi::BindingSetItem::Texture_SRV(
			2, m_hBeachBathymetryTexture, nvrhi::Format::R32_FLOAT));
		bsDesc.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
			8, m_hBeachOpaqueSceneTexture, nvrhi::Format::RGBA16_FLOAT));
		bsDesc.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
			9, m_hBeachLinearDepthTexture, nvrhi::Format::R32_FLOAT));
		AppendBeachLightingBindings(bsDesc, *m_pState);
		AppendBeachShallowBindings(bsDesc, *m_pState, true);
	}
	m_hOceanSurfaceBindings = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hOceanSurfaceBindingLayout);

	psoDesc.VS = m_hOceanSurfaceVS;
	psoDesc.HS = m_hOceanSurfaceHS;
	psoDesc.DS = m_hOceanSurfaceDS;
	psoDesc.PS = m_hOceanSurfacePS;
	psoDesc.primType = nvrhi::PrimitiveType::PatchList;
    psoDesc.patchControlPoints = 3;
	psoDesc.bindingLayouts = ( m_hOceanSurfaceBindingLayout ).layouts;
	psoDesc.inputLayout = m_hOceanSurfaceLayout;
	psoDesc.renderState = renderState;
	psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Solid;
	m_pDevice->createGraphicsPipeline1(psoDesc, m_hInitTimeFB->getFramebufferInfo().getInfo(), &m_hOceanSurfaceSolidPSO);
	psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
	m_pDevice->createGraphicsPipeline1(psoDesc, m_hInitTimeFB->getFramebufferInfo().getInfo(), &m_hOceanSurfaceWireframePSO);
	if (m_pState->bBeachScene)
	{
		// Foam is emitted as premultiplied color and is depth-tested against the
		// opaque terrain without writing a second depth layer.
		psoDesc.PS = m_hBeachFoamPS;
		psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Solid;
		psoDesc.renderState.blendState.targets[0].blendEnable = true;
		psoDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::One;
		psoDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		psoDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::One;
		psoDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		psoDesc.renderState.depthStencilState.depthWriteEnable =
			false;
		m_pDevice->createGraphicsPipeline1(psoDesc, m_hInitTimeFB->getFramebufferInfo().getInfo(), &m_hBeachFoamPSO);
	}

	// Allocating memory for readback positions and results
	m_pReadbackPositions = (gfsdk_float2*)malloc(NumMarkers * sizeof(gfsdk_float2));
	m_pReadbackResults = (gfsdk_float4*)malloc(NumMarkers * sizeof(gfsdk_float4));

	// Creating constant buffer for marker rendering
	cbDesc.byteSize = sizeof(MARKER_VS_CBUFFER);
	cbDesc.debugName = "VS CB for rendering markers";
	cbDesc.isConstantBuffer = true;
	m_pDevice->createBuffer(cbDesc, &m_hMarkerVSCB);
	m_hCommandList->beginTrackingBufferState(m_hMarkerVSCB, nvrhi::ResourceStates::Common);
	m_hCommandList->beginTrackingBufferState(m_hMarkerVSCB, nvrhi::ResourceStates::ConstantBuffer);

	// Creating input layout for rendering markers
	nvrhi::VertexAttributeDesc markers_layout[] = {
		{ "POSITION", nvrhi::Format::RGBA32_FLOAT, 1, 0, 0, 16, false },
		{ "PERINSTANCEDATA", nvrhi::Format::RGBA32_FLOAT, 1, 1, 0, 16, true },
	};

	m_pDevice->createInputLayout(markers_layout, 2, m_hMarkerVS, &m_hMarkerLayout);

	// Creating PSO and bindings marker rendering
	blDesc.VS = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) };
	blDesc.HS = {};
	blDesc.DS = {};
	blDesc.PS = {};
	m_hMarkerBindingLayout = waveworks::graphics::createBindingLayout(m_pDevice, blDesc);

	bsDesc.VS = { nvrhi::BindingSetItem::ConstantBuffer(0, m_hMarkerVSCB ) };
	bsDesc.HS = {};
	bsDesc.DS = {};
	bsDesc.PS = {};
	m_hMarkerBindings = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hMarkerBindingLayout);

	psoDesc.VS = m_hMarkerVS;
	psoDesc.HS = nullptr;
	psoDesc.DS = nullptr;
	psoDesc.PS = m_hMarkerPS;
	psoDesc.renderState = renderState;
	psoDesc.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Solid;
	psoDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
	psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
	psoDesc.bindingLayouts = ( m_hMarkerBindingLayout ).layouts;
	psoDesc.inputLayout = m_hMarkerLayout;

	m_pDevice->createGraphicsPipeline1(psoDesc, m_hInitTimeFB->getFramebufferInfo().getInfo(), &m_hMarkerPSO);


	// Creating marker instance buffer and upload buffers for marker instances
	nvrhi::BufferDesc desc;
	desc.byteSize = uint32_t(NumMarkers * sizeof(gfsdk_float4));
	desc.structStride = 0;
	desc.debugName = "Marker instances upload buffer";
	desc.canHaveUAVs = false;
	desc.isVertexBuffer = true;
	desc.isIndexBuffer = false;
	desc.isDrawIndirectArgs = false;

	desc.cpuAccess = nvrhi::CpuAccessMode::Write;

	for (int i = 0; i < 3; i++)
	{
		m_pDevice->createBuffer(desc, &m_hMarkerInstanceUploadBuffers[i]);
		if (!m_hMarkerInstanceUploadBuffers[i])
		{
			return false;
		}
	}
	m_markerInstanceUploadBufferNumber = 0;

	desc.cpuAccess = nvrhi::CpuAccessMode::None;
	m_pDevice->createBuffer(desc, &m_hMarkerInstanceBuffer);

	// Creating marker VB and IB
	float marker_vertexdata[20] = { 0.0f, 0.0f, 0.0f, 1.0f,
									0.5f,-0.5f, 1.0f, 1.0f,
									0.5f, 0.5f, 1.0f, 1.0f,
								   -0.5f, 0.5f, 1.0f, 1.0f,
								   -0.5f,-0.5f, 1.0f, 1.0f };
	uint32_t marker_indexdata[12] = { 0, 1, 2,
									 0, 2, 3,
									 0, 3, 4,
									 0, 4, 1 };

	bd.byteSize = 20 * sizeof(float);
	bd.isVertexBuffer = true;
	m_pDevice->createBuffer(bd, &m_hMarkerVB);
	m_hCommandList->beginTrackingBufferState(m_hMarkerVB, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hMarkerVB, marker_vertexdata, bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hMarkerVB, nvrhi::ResourceStates::VertexBuffer);
	m_hCommandList->commitBarriers();

	bd.byteSize = 12 * sizeof(uint32_t);
	bd.isIndexBuffer = true;
	m_pDevice->createBuffer(bd, &m_hMarkerIB);
	m_hCommandList->beginTrackingBufferState(m_hMarkerIB, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hMarkerIB, marker_indexdata, bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hMarkerIB, nvrhi::ResourceStates::IndexBuffer);
	m_hCommandList->commitBarriers();



	m_hCommandList->close();
	m_pDevice->executeCommandList(m_hCommandList);

	return true;
}

void OceanRenderer::BackBufferResizing() 
{
	m_hFBs.clear();
	if (!m_pState->bBeachScene)
		m_pState->hDepthBufferTexture = nullptr;
}
void OceanRenderer::BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount)
{
	m_backBufferWidth = width;
	m_backBufferHeight = height;
	m_sampleCount = sampleCount;
	
	if (m_pDevice == nullptr) return;
	if (m_pState == nullptr) return;

	m_pDevice->waitForIdle();

	updateCameraProjection();
	m_needToRecreatePSOs = true;
}

void OceanRenderer::updateCameraProjection()
{
	float aspectRatio = (float)m_backBufferWidth / (float)m_backBufferHeight;
	m_pState->Camera.SetProjection(3.14f/3.0f, aspectRatio, m_pState->fCameraClipNear, m_pState->fCameraClipFar);
}

void OceanRenderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    auto* renderTarget = framebuffer->getDesc().colorAttachments[0].texture;
    const uint32_t swapChainIndex = GetDeviceManager()->GetCurrentBackBufferIndex();
	const bool needsResourceRebind = m_needToRebindBeachTargets ||
		m_pState->bNeedToUpdateQuadtreeProperties ||
		(m_pState->bUpdateLocalWaves &&
			m_pState->bNeedToUpdateLocalWavesSimulationProperties) ||
		(m_pState->bUpdateWindWaves &&
			m_pState->bNeedToUpdateWindWavesSimulationProperties);
	// Rebinding replaces resources that may still be referenced by an earlier
	// frame. Wait before opening the command list; waiting from inside an open
	// list is invalid on the explicit backends and caused resize-time failures.
	if (needsResourceRebind)
		m_pDevice->waitForIdle();

	m_hCommandList->open();
	nvrhi::FramebufferHandle FB = getFramebuffer(renderTarget, swapChainIndex);
	if (!FB)
	{
		m_hCommandList->close();
		return;
	}
	// Donut owns the swapchain's Present state between command lists.
	if (!renderTarget->getDesc().keepInitialState)
		m_hCommandList->beginTrackingTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    if (m_pState->bBeachScene) {
        m_hCommandList->beginTrackingTextureState(m_hBeachOpaqueSceneTexture,
            nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        m_hCommandList->beginTrackingTextureState(m_hBeachLinearDepthTexture,
            nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    }
	m_hCommandList->beginTrackingTextureState(m_pState->hDepthBufferTexture,
		nvrhi::AllSubresources, m_pState->bBeachScene
			? nvrhi::ResourceStates::DepthRead : nvrhi::ResourceStates::DepthWrite);
	// The beach opaque pass owns the frame clear and depth pre-pass. Standalone
	// ocean mode retains its original clear because it has no terrain pass.
	if (!m_pState->bBeachScene)
	{
		m_hCommandList->clearTextureFloat(renderTarget, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(0.8f, 0.9f, 1.0f, 1.0f));
		m_hCommandList->clearDepthStencilTexture(m_pState->hDepthBufferTexture,
            nvrhi::TextureSubresourceSet(0, 1, 0, 1), true, 1.f, false, 0);
	}
	// Rebinding resources if needed
	if (m_pState->bNeedToUpdateQuadtreeProperties)
	{
		GFSDK_WaveWorks_Quadtree_UpdateParams(m_pState->hOceanQuadtree, m_pState->OceanQuadtreeParameters);
		RebindOceanResources();
		m_pState->bNeedToUpdateQuadtreeProperties = false;
	}

	if (m_pState->bUpdateLocalWaves && m_pState->bNeedToUpdateLocalWavesSimulationProperties)
	{
		GFSDK_WaveWorks_Local_Waves_Simulation_UpdateProperties(m_pState->hOceanLocalSimulation, m_pState->OceanLocalSimulationSettings, m_pState->OceanLocalSimulationParameters);
		RebindOceanResources();
		m_pState->bNeedToUpdateLocalWavesSimulationProperties = false;
	}

	if (m_pState->bUpdateWindWaves && m_pState->bNeedToUpdateWindWavesSimulationProperties)
	{
		GFSDK_WaveWorks_Wind_Waves_Simulation_UpdateProperties(m_pState->hOceanWindSimulation, m_pState->OceanWindSimulationSettings, m_pState->OceanWindSimulationParameters);
		RebindOceanResources();
		m_pState->bNeedToUpdateWindWavesSimulationProperties = false;
	}
	if (m_needToRebindBeachTargets)
		RebindOceanResources();

	if (m_pState->bRenderWater)
	{
		m_hCommandList->beginTrackingTextureState(m_hWindWavesDisplacementsTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hWindWavesGradientsTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hWindWavesMomentsTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hFoamIntensityTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hFoamBubblesTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hWindGustsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hLocalWavesDisplacementsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_hLocalWavesGradientsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		if (m_pState->bBeachScene)
			m_hCommandList->beginTrackingTextureState(m_hBeachBathymetryTexture,
				nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingBufferState(m_hOceanSurfacePSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->beginTrackingBufferState(m_hOceanSurfaceVSHSDSCB, nvrhi::ResourceStates::ConstantBuffer);

		// Initializing ocean surface draw state
		nvrhi::Viewport vp;
		vp.maxX = float(m_backBufferWidth);
		vp.maxY = float(m_backBufferHeight);

		nvrhi::Rect scissor;
		scissor.maxX = m_backBufferWidth;
		scissor.maxY = m_backBufferHeight;
	
		if (m_pState->bRenderWireframe)
		{
			m_drawState.pipeline = m_hOceanSurfaceWireframePSO;
		}
		else
		{
			m_drawState.pipeline = m_hOceanSurfaceSolidPSO;
		}
		m_drawState.framebuffer = FB;
		m_drawState.bindings = ( m_hOceanSurfaceBindings ).sets(); 
		m_drawState.viewport.viewports.resize(1);
		m_drawState.viewport.viewports[0] = vp;
		m_drawState.viewport.scissorRects = { scissor };
		gfsdk_float2 viewportSize;
		viewportSize.x = float(m_backBufferWidth);
		viewportSize.y = float(m_backBufferHeight);

		// Getting view and projection matrices
		gfsdk_float4x4 viewMatrix = m_pState->Camera.GetWorldToView();
		gfsdk_float4x4 projMatrix = m_pState->Camera.GetProjection();
		gfsdk_float3 eyePoint = m_pState->Camera.GetCameraPosition();

		//Internal quadtree math is left handed, z up, so flippings rows 2 and 3 here
		gfsdk_float4x4  vm = { viewMatrix._11, viewMatrix._12, viewMatrix._13,viewMatrix._14,
					   viewMatrix._31, viewMatrix._32, viewMatrix._33,viewMatrix._34,
					   viewMatrix._21, viewMatrix._22, viewMatrix._23,viewMatrix._24,
					   viewMatrix._41, viewMatrix._42, viewMatrix._43,viewMatrix._44 };

		// Getting data for rendering from WaveWorks 
		GFSDK_WaveWorks_Wind_Waves_Rendering_Data windWavesRenderingData;
		GFSDK_WaveWorks_Wind_Waves_GetDataForRendering(m_pState->hOceanWindSimulation, windWavesRenderingData);

		GFSDK_WaveWorks_Local_Waves_Rendering_Data localWavesRenderingData;
		GFSDK_WaveWorks_Local_Waves_GetDataForRendering(m_pState->hOceanLocalSimulation, localWavesRenderingData);

		// Updating constant buffers
		OCEAN_VS_HS_DS_CBUFFER VSHSDSCB{};
		VSHSDSCB.g_matViewProj = projMatrix * vm;
		VSHSDSCB.g_eyePos = { eyePoint.x, eyePoint.z, eyePoint.y };
		VSHSDSCB.g_meanOceanLevel = m_pState->OceanQuadtreeParameters.mean_sea_level;
		VSHSDSCB.g_dynamicTesselationAmount = m_pState->fTessellationAmount;
		VSHSDSCB.g_staticTesselationOffset = m_pState->fTessellationOffset;
		VSHSDSCB.g_cascade0UVOffset = windWavesRenderingData.cascade0_UV_offset;
		VSHSDSCB.g_cascade0UVScale = windWavesRenderingData.cascade0_UV_scale;
		VSHSDSCB.g_cascade1UVOffset = windWavesRenderingData.cascade1_UV_offset;
		VSHSDSCB.g_cascade1UVScale = windWavesRenderingData.cascade1_UV_scale;
		VSHSDSCB.g_cascade2UVOffset = windWavesRenderingData.cascade2_UV_offset;
		VSHSDSCB.g_cascade2UVScale = windWavesRenderingData.cascade2_UV_scale;
		VSHSDSCB.g_cascade3UVOffset = windWavesRenderingData.cascade3_UV_offset;
		VSHSDSCB.g_cascade3UVScale = windWavesRenderingData.cascade3_UV_scale;
		VSHSDSCB.g_localWavesSimulationDomainWorldspaceCenter = localWavesRenderingData.simulation_domain_worldspace_center;
		VSHSDSCB.g_localWavesSimulationDomainWorldspaceSize = localWavesRenderingData.simulation_domain_worldspace_size;
		VSHSDSCB.g_useDiamondPattern = m_pState->OceanQuadtreeParameters.generate_diamond_pattern ? 1.0f : 0.0f;
		VSHSDSCB.g_UVWarpingAmplitude = windWavesRenderingData.uv_warping_amplitude;
		VSHSDSCB.g_UVWarpingFrequency = windWavesRenderingData.uv_warping_frequency;
		VSHSDSCB.g_beachScene = m_pState->bBeachScene ? 1.0f : 0.0f;
		if (m_pState->bBeachScene)
		{
			VSHSDSCB.g_bathymetryOrigin = m_beachBathymetryOrigin;
			VSHSDSCB.g_bathymetryInverseExtent = {
				1.f / m_beachBathymetryExtent.x,
				1.f / m_beachBathymetryExtent.y };
			VSHSDSCB.g_breakingWaveRatio = m_pState->bBeachShallowWater
				? BeachBreakingWaveRatio : 0.f;
			VSHSDSCB.g_shorelineFadeDepth = m_pState->bBeachShallowWater
				? BeachShorelineFadeDepth : 0.f;
		}

		m_hCommandList->writeBuffer(m_hOceanSurfaceVSHSDSCB, &VSHSDSCB, sizeof(VSHSDSCB));
		
		OCEAN_PS_CBUFFER PSCB{};
		PSCB.g_cascadeToCascadeScale = windWavesRenderingData.cascade_to_cascade_scale;
		PSCB.g_windWavesTextureSizeInTexels = (float)windWavesRenderingData.size_of_texture_arrays;
		PSCB.g_UVWarpingAmplitude = windWavesRenderingData.uv_warping_amplitude;
		PSCB.g_UVWarpingFrequency = windWavesRenderingData.uv_warping_frequency;
		PSCB.g_windWavesFoamWhitecapsThreshold = windWavesRenderingData.foam_whitecaps_threshold;
		PSCB.g_SimulationDomainCenter = localWavesRenderingData.simulation_domain_worldspace_center;
		PSCB.g_SimulationDomainSize = localWavesRenderingData.simulation_domain_worldspace_size;
		PSCB.g_localWavesTextureSizeInTexels = (float)localWavesRenderingData.size_of_texture;
		PSCB.g_localWavesFoamWhitecapsThreshold = m_pState->OceanLocalSimulationParameters.foam_whitecaps_threshold;
		PSCB.g_beckmannRoughness = m_pState->fBeckmannRoughness;
		PSCB.g_sunIntensity = m_pState->fSunIntensity;
		PSCB.g_sunDirection = { cosf(m_pState->fSunAngle * 0.0174533f), 0, sinf(m_pState->fSunAngle * 0.0174533f) };
		if (m_pState->bBeachScene)
		{
			const auto sun = BeachSunDirection(*m_pState);
			PSCB.g_sunDirection = { sun.x, sun.z, sun.y };
		}
		PSCB.g_useMicrofacetFresnel = m_pState->bUseMicrofacetFresnel ? 1.0f : 0.0f;
		PSCB.g_useMicrofacetSpecular = m_pState->bUseMicrofacetSpecular ? 1.0f : 0.0f;
		PSCB.g_useMicrofacetReflection = m_pState->bUseMicrofacetReflection ? 1.0f : 0.0f;
		PSCB.g_showCascades = m_pState->bShowCascades ? 1.0f : 0.0f;
		PSCB.pad0 = float(m_pState->iBeachWaterDebug);
		PSCB.g_eyePos = { eyePoint.x, eyePoint.z, eyePoint.y };
		PSCB.g_beachScene = m_pState->bBeachScene ? 1.0f : 0.0f;
		PSCB.g_beachShallowWater = m_pState->bBeachShallowWater ? 1.0f : 0.0f;
		PSCB.g_beachShoreFoam = m_pState->bBeachShoreFoam ? 1.0f : 0.0f;
		PSCB.g_beachFoamIntensity = m_pState->fBeachShoreFoamIntensity;
		PSCB.g_inverseViewport = { 1.f / float(m_backBufferWidth), 1.f / float(m_backBufferHeight) };
		PSCB.g_nearClip = m_pState->fCameraClipNear;
		PSCB.g_farClip = m_pState->fCameraClipFar;
		PSCB.g_waterAbsorption = BeachWaterAbsorption;
		PSCB.g_waterIOR = BeachWaterIOR;
		PSCB.g_waterScattering = BeachWaterScattering;
		PSCB.g_maxOpticalDepth = BeachMaxOpticalDepth;
		PSCB.g_sedimentAbsorption = BeachSedimentAbsorption;
		PSCB.g_sedimentDepth = BeachSedimentDepth;
		PSCB.g_sedimentScattering = BeachSedimentScattering;
		PSCB.g_sedimentConcentration = BeachSedimentConcentration;
		PSCB.g_sunRadiance = m_pState->vBeachSunIrradiance;
		PSCB.g_refractionDistanceLimit = BeachRefractionDistanceLimit;
		PSCB.g_foamBaseColor = BeachFoamBaseColor;
		PSCB.g_foamRoughness = BeachFoamRoughness;
		PSCB.g_breakingWaveRatio = BeachBreakingWaveRatio;
		PSCB.g_foamOpticalDepth = BeachFoamOpticalDepth;
		PSCB.g_foamEnergyScale = BeachFoamEnergyScale;
		PSCB.g_foamPatternScale = BeachFoamPatternScale;
		PSCB.g_worldToClip = VSHSDSCB.g_matViewProj;

		m_hCommandList->writeBuffer(m_hOceanSurfacePSCB, &PSCB, sizeof(PSCB));

		// Getting data for rendering the nodes from quadtree
		uint32_t numNodes;
		GFSDK_WaveWorks_Quadtree_NodeRenderingProperties* pNodes;
		std::vector<GFSDK_WaveWorks_Quadtree_NodeRenderingProperties> nodes;
		float cullMargin = GFSDK_WaveWorks_Wind_Waves_Simulation_GetConservativeMaxDisplacementEstimate(m_pState->hOceanWindSimulation);

		GFSDK_WaveWorks_Quadtree_GetNodesForRendering(m_pState->hOceanQuadtree, vm, projMatrix, viewportSize, cullMargin, &numNodes, &pNodes);
		nodes.assign(pNodes, pNodes + numNodes);

		// Getting the quadtree stats
		GFSDK_WaveWorks_Quadtree_GetStats(m_pState->hOceanQuadtree, m_pState->OceanQuadtreeStats);

		// We could render patches one by one, but it's much more draw calls and less efficient compared to instancing.
		// We can use instancing here, since there is just 16 possible patch topology types.
		// Up to 16 types of patch types (instance types) can be exposed by quadtree generator.
		
		// Start indices and index counts for patch types generated by quadtree
		std::vector<uint32_t> startIndices(16);
		std::vector<uint32_t> indexCounts(16);

		// Per-instance data for patch types
		std::vector <std::vector< OCEAN_VS_CBUFFER_PERINSTANCE_ENTRY > > perInstanceBuffers(16);
		uint32_t instanceCounts[16] = {};

		// Reserving space for the entire constant buffer size (64kb or 4096 entries) in perInstanceBuffers
		// as current NVRHI implementation updates the entire contents of the constant buffers.
		if (m_waterTimerQuery)
			m_hCommandList->beginTimerQuery(m_waterTimerQuery);
		for (uint32_t i = 0; i < 16; i++)
		{
			perInstanceBuffers[i].reserve(4096);
		}

		// Assembling the arrays
		for (uint32_t i = 0; i < nodes.size(); i++)
		{
			uint32_t patchType = nodes[i].patch_type;

			startIndices[patchType] = nodes[i].start_index;
			indexCounts[patchType] = nodes[i].num_indices;

			OCEAN_VS_CBUFFER_PERINSTANCE_ENTRY cbEntry;
			cbEntry.g_patchWorldspaceOrigin = nodes[i].patch_worldspace_origin;
			cbEntry.g_patchWorldspaceScale = nodes[i].patch_worldspace_scale;
			cbEntry.g_patchMorphConstantAndSign = nodes[i].geomorphing_distance_constant * nodes[i].geomorphing_sign;
			perInstanceBuffers[patchType].push_back(cbEntry);
		}

		// NVRHI uploads the full constant buffer. reserve() alone does not make
		// the unused entries valid objects to read; value-initialize that tail.
		for (uint32_t i = 0; i < 16; ++i)
		{
			instanceCounts[i] = uint32_t(std::min(size_t(4096), perInstanceBuffers[i].size()));
			perInstanceBuffers[i].resize(4096);
		}
		// Rendering the quadtree patches using instancing.
		for (uint32_t i = 0; i < 16; i++)
		{

			if (instanceCounts[i] > 0)
			{
				// Writing the entire constant buffer
				m_hCommandList->writeBuffer(m_hOceanSurfacePerInstanceCB, (void*)perInstanceBuffers[i].data(), 4096 * sizeof(OCEAN_VS_CBUFFER_PERINSTANCE_ENTRY));

				// Setting up draw arguments to render up to 4096 instances.
				// Ocean quadtree usually generates <100 instances of patches of each type, 
				// but if there is a need to render more than 4096 instances, then structured / vertex buffer can be used instead of constant buffer,
				// or multiple draw calls with the constant buffer updates between them.
				nvrhi::DrawArguments da;
				da.vertexCount = indexCounts[i];
				da.startIndexLocation = startIndices[i];
				da.startVertexLocation = 0;
				da.startInstanceLocation = 0;
				da.instanceCount = instanceCounts[i];

				m_drawState.indexBuffer.buffer = m_hOceanSurfaceIB;
				m_drawState.indexBuffer.format = nvrhi::Format::R32_UINT;
				m_drawState.indexBuffer.offset = 0;
				m_drawState.vertexBuffers.resize(1);
				m_drawState.vertexBuffers[0].buffer = m_hOceanSurfaceVB;
				m_drawState.vertexBuffers[0].slot = 0;
				m_drawState.vertexBuffers[0].offset = 0;

				m_hCommandList->setGraphicsState(m_drawState);
				m_hCommandList->drawIndexed(da);
			}
		}
		if (m_waterTimerQuery)
			m_hCommandList->endTimerQuery(m_waterTimerQuery);

		if (m_pState->bBeachScene && m_hBeachFoamPSO && m_pState->bBeachShoreFoam)
		{
			m_drawState.pipeline = m_hBeachFoamPSO;
			if (m_foamTimerQuery)
				m_hCommandList->beginTimerQuery(m_foamTimerQuery);
			for (uint32_t i = 0; i < 16; i++)
			{
				if (instanceCounts[i] == 0) continue;
				m_hCommandList->writeBuffer(m_hOceanSurfacePerInstanceCB,
					perInstanceBuffers[i].data(),
					4096 * sizeof(OCEAN_VS_CBUFFER_PERINSTANCE_ENTRY));
				nvrhi::DrawArguments foamDraw;
				foamDraw.vertexCount = indexCounts[i];
				foamDraw.startIndexLocation = startIndices[i];
				foamDraw.startVertexLocation = 0;
				foamDraw.startInstanceLocation = 0;
				foamDraw.instanceCount = instanceCounts[i];
				m_hCommandList->setGraphicsState(m_drawState);
				m_hCommandList->drawIndexed(foamDraw);
			}
			if (m_foamTimerQuery)
				m_hCommandList->endTimerQuery(m_foamTimerQuery);
		}
		if (m_pState->bRenderMarkers)
		{
			RenderMarkers(FB);
		}
	}
	m_hCommandList->setTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
	m_hCommandList->commitBarriers();
    if (m_pState->bBeachScene)
        {
            m_hCommandList->setTextureState(m_hBeachLinearDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            m_hCommandList->commitBarriers();
        }
	m_hCommandList->setTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, m_pState->bBeachScene
			? nvrhi::ResourceStates::DepthRead : nvrhi::ResourceStates::DepthWrite);
	m_hCommandList->commitBarriers();
	if (m_pState->bRenderWater)
	{
		m_hCommandList->setTextureState(m_hWindWavesDisplacementsTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hWindWavesGradientsTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hWindWavesMomentsTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hFoamIntensityTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hFoamBubblesTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hWindGustsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hLocalWavesDisplacementsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_hLocalWavesGradientsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		if (m_pState->bBeachScene)
			{
			    m_hCommandList->setTextureState(m_hBeachBathymetryTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
			    m_hCommandList->commitBarriers();
			}
	}
	m_hCommandList->close();
	m_pDevice->executeCommandList(m_hCommandList);
}

nvrhi::FramebufferHandle OceanRenderer::getFramebuffer(nvrhi::TextureHandle renderTarget, uint32_t swapChainIndex)
{
	nvrhi::TextureDesc td = renderTarget->getDesc();

	// Reallocate the depth buffer if needed
	if (m_pState->hDepthBufferTexture == nullptr)
	{
		if (m_pState->bBeachScene)
			return nullptr;
		nvrhi::TextureDesc desc;
		desc.width = td.width;
		desc.height = td.height;
		desc.sampleCount = td.sampleCount;
		desc.debugName = "Depth target created in OceanRenderer::getFramebuffer";
		desc.format = nvrhi::Format::D24S8;
		desc.isRenderTarget = true;
		desc.clearValue = nvrhi::Color(1.0f);
		desc.useClearValue = true;
		desc.isTypeless = true; // Depth targets must be declared typeless in NVRHI
		m_pDevice->createTexture(desc, &m_pState->hDepthBufferTexture);
		assert(m_pState->hDepthBufferTexture);
		m_hCommandList->beginTrackingTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
		m_hCommandList->setTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
		m_hCommandList->commitBarriers();
	}

	// Create framebuffers if needed
	if (swapChainIndex >= m_hFBs.size()) m_hFBs.resize(swapChainIndex + 1);

	if (m_hFBs[swapChainIndex] == nullptr)
	{
		nvrhi::FramebufferDesc desc = nvrhi::FramebufferDesc()
			.addColorAttachment(renderTarget)
			.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_pState->hDepthBufferTexture).setArraySlice(0).setMipLevel(0).setReadOnly(m_pState->bBeachScene));

		m_pDevice->createFramebuffer(desc, &m_hFBs[swapChainIndex]);
	}

	if (m_needToRecreatePSOs)
	{
		// Recreating PSOs
		nvrhi::GraphicsPipelineDesc markerPSOdesc = m_hMarkerPSO->getDesc();
		m_hMarkerPSO = nullptr;
		m_pDevice->createGraphicsPipeline1(markerPSOdesc, m_hFBs[swapChainIndex]->getFramebufferInfo().getInfo(), &m_hMarkerPSO);

		nvrhi::GraphicsPipelineDesc solidPSOdesc = m_hOceanSurfaceSolidPSO->getDesc();
		m_hOceanSurfaceSolidPSO = nullptr;
		m_pDevice->createGraphicsPipeline1(solidPSOdesc, m_hFBs[swapChainIndex]->getFramebufferInfo().getInfo(), &m_hOceanSurfaceSolidPSO);

		nvrhi::GraphicsPipelineDesc wireframePSOdesc = m_hOceanSurfaceWireframePSO->getDesc();
		m_hOceanSurfaceWireframePSO = nullptr;
		m_pDevice->createGraphicsPipeline1(wireframePSOdesc, m_hFBs[swapChainIndex]->getFramebufferInfo().getInfo(), &m_hOceanSurfaceWireframePSO);
		if (m_hBeachFoamPSO)
		{
			nvrhi::GraphicsPipelineDesc foamPSOdesc = m_hBeachFoamPSO->getDesc();
			m_hBeachFoamPSO = nullptr;
			m_pDevice->createGraphicsPipeline1(foamPSOdesc, m_hFBs[swapChainIndex]->getFramebufferInfo().getInfo(), &m_hBeachFoamPSO);
		}
		m_needToRecreatePSOs = false;
	}

	assert(m_hFBs.at(swapChainIndex));
	return m_hFBs[swapChainIndex];
}

void OceanRenderer::RebindOceanResources()
{
	// Re-creating quadtree vertex and index buffers if needed
	if (m_pState->bNeedToUpdateQuadtreeProperties)
	{
		m_hOceanSurfaceVB = nullptr;
		m_hOceanSurfaceIB = nullptr;

		// Creating ocean surface vertex and index buffers
		uint32_t numVertices = 0;
		uint32_t numIndices = 0;
		gfsdk_float2 *pVertexData = nullptr;
		uint32_t *pIndexData = nullptr;
		std::vector<gfsdk_float2> oceanVertices;
		std::vector<uint32_t> oceanIndices;

		GFSDK_WaveWorks_Quadtree_GetGeometryData(m_pState->hOceanQuadtree, &numVertices, &pVertexData, &numIndices, &pIndexData);

		oceanVertices.assign(pVertexData, pVertexData + numVertices);
		oceanIndices.assign(pIndexData, pIndexData + numIndices);

		nvrhi::BufferDesc bd;
		bd.byteSize = numVertices * sizeof(gfsdk_float2);
		bd.isVertexBuffer = true;
		bd.isIndexBuffer = false;
		m_pDevice->createBuffer(bd, &m_hOceanSurfaceVB);
		m_hCommandList->beginTrackingBufferState(m_hOceanSurfaceVB, nvrhi::ResourceStates::Common);
		m_hCommandList->writeBuffer(m_hOceanSurfaceVB, oceanVertices.data(), bd.byteSize);
		m_hCommandList->setPermanentBufferState(m_hOceanSurfaceVB, nvrhi::ResourceStates::VertexBuffer);
		m_hCommandList->commitBarriers();

		bd.byteSize = numIndices * sizeof(uint32_t);
		bd.isVertexBuffer = false;
		bd.isIndexBuffer = true;
		m_pDevice->createBuffer(bd, &m_hOceanSurfaceIB);
		m_hCommandList->beginTrackingBufferState(m_hOceanSurfaceIB, nvrhi::ResourceStates::Common);
		m_hCommandList->writeBuffer(m_hOceanSurfaceIB, oceanIndices.data(), bd.byteSize);
		m_hCommandList->setPermanentBufferState(m_hOceanSurfaceIB, nvrhi::ResourceStates::IndexBuffer);
		m_hCommandList->commitBarriers();
	}

	// Re-creating NVRHI textures from native objects exposed from WaveWorks
	m_hWindWavesDisplacementsTextureArray = nullptr;
	m_hWindWavesGradientsTextureArray = nullptr;
	m_hWindWavesMomentsTextureArray = nullptr;

	m_hLocalWavesDisplacementsTexture = nullptr;
	m_hLocalWavesGradientsTexture = nullptr;

	GFSDK_WaveWorks_Wind_Waves_Rendering_Data windWavesRenderingData;
	GFSDK_WaveWorks_Local_Waves_Rendering_Data localWavesRenderingData;
	GFSDK_WaveWorks_Wind_Waves_GetDataForRendering(m_pState->hOceanWindSimulation, windWavesRenderingData);
	GFSDK_WaveWorks_Local_Waves_GetDataForRendering(m_pState->hOceanLocalSimulation, localWavesRenderingData);

	nvrhi::TextureDesc texArrayDesc;
	texArrayDesc.width = windWavesRenderingData.size_of_texture_arrays;
	texArrayDesc.height = windWavesRenderingData.size_of_texture_arrays;
	texArrayDesc.format = nvrhi::Format::RGBA16_FLOAT;
	texArrayDesc.isUAV = true;
	texArrayDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
	texArrayDesc.arraySize = 4;
	texArrayDesc.mipLevels = getNumMipLevels(windWavesRenderingData.size_of_texture_arrays);
	texArrayDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	texArrayDesc.keepInitialState = true;
	const auto nativeObjectType = NativeTextureObject(m_backend);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, windWavesRenderingData.displacements_texture_array, texArrayDesc, &m_hWindWavesDisplacementsTextureArray);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, windWavesRenderingData.gradients_texture_array, texArrayDesc, &m_hWindWavesGradientsTextureArray);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, windWavesRenderingData.moments_texture_array, texArrayDesc, &m_hWindWavesMomentsTextureArray);

	nvrhi::TextureDesc texDesc;
	texDesc.width = localWavesRenderingData.size_of_texture;
	texDesc.height = localWavesRenderingData.size_of_texture;
	texDesc.format = nvrhi::Format::RGBA16_FLOAT;
	texDesc.isUAV = true;
	texDesc.dimension = nvrhi::TextureDimension::Texture2D;
	texDesc.mipLevels = getNumMipLevels(localWavesRenderingData.size_of_texture);
	texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	texDesc.keepInitialState = true;
	m_pDevice->createHandleForNativeTexture(nativeObjectType, localWavesRenderingData.displacements_texture, texDesc, &m_hLocalWavesDisplacementsTexture);
	m_pDevice->createHandleForNativeTexture(nativeObjectType, localWavesRenderingData.gradients_texture, texDesc, &m_hLocalWavesGradientsTexture);
	
	// Updating binding set
	nvrhi::TextureDesc tdWindDisplacements = m_hWindWavesDisplacementsTextureArray->getDesc();
	nvrhi::TextureDesc tdLocalDisplacements = m_hLocalWavesDisplacementsTexture->getDesc();

	waveworks::graphics::GraphicsBindingSetDesc bsDesc;
	bsDesc.VS = { nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfacePerInstanceCB ),
					nvrhi::BindingSetItem::ConstantBuffer(1, m_hOceanSurfaceVSHSDSCB ) };
	bsDesc.HS = { nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfaceVSHSDSCB ) };
	bsDesc.DS = { nvrhi::BindingSetItem::Texture_SRV(0, m_hWindWavesDisplacementsTextureArray, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(0,tdWindDisplacements.mipLevels,0,tdWindDisplacements.arraySize) ),
					nvrhi::BindingSetItem::Texture_SRV(1, m_hLocalWavesDisplacementsTexture, nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler ),
					nvrhi::BindingSetItem::Sampler(1, m_hBilinearClampToEdgeSampler ),
					nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfaceVSHSDSCB) };
	bsDesc.PS = { nvrhi::BindingSetItem::Texture_SRV(0, m_hWindWavesGradientsTextureArray, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(0,tdWindDisplacements.mipLevels,0,tdWindDisplacements.arraySize)),
					nvrhi::BindingSetItem::Texture_SRV(1, m_hWindWavesMomentsTextureArray, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(0,tdWindDisplacements.mipLevels,0,tdWindDisplacements.arraySize)),
					nvrhi::BindingSetItem::Texture_SRV(2, m_hLocalWavesDisplacementsTexture,  nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Texture_SRV(3, m_hLocalWavesGradientsTexture,  nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Texture_SRV(4, m_hFoamIntensityTexture, nvrhi::Format::RGBA8_UNORM),
					nvrhi::BindingSetItem::Texture_SRV(5, m_hFoamBubblesTexture,  nvrhi::Format::RGBA8_UNORM),
					nvrhi::BindingSetItem::Texture_SRV(6, m_hWindGustsTexture,  nvrhi::Format::RGBA8_UNORM),
					nvrhi::BindingSetItem::Texture_SRV(7, m_pState->hDynamicSkyDomeTexture,
						nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler),
					nvrhi::BindingSetItem::Sampler(1, m_hAnisotropicSampler),
					nvrhi::BindingSetItem::Sampler(2, m_hBilinearClampToEdgeSampler ),
					nvrhi::BindingSetItem::ConstantBuffer(0, m_hOceanSurfacePSCB ) };
	if (m_pState->bBeachScene)
	{
		bsDesc.DS.push_back(nvrhi::BindingSetItem::Texture_SRV(
			2, m_hBeachBathymetryTexture, nvrhi::Format::R32_FLOAT));
		bsDesc.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
			8, m_hBeachOpaqueSceneTexture, nvrhi::Format::RGBA16_FLOAT));
		bsDesc.PS.push_back(nvrhi::BindingSetItem::Texture_SRV(
			9, m_hBeachLinearDepthTexture, nvrhi::Format::R32_FLOAT));
		AppendBeachLightingBindings(bsDesc, *m_pState);
		AppendBeachShallowBindings(bsDesc, *m_pState, true);
	}
	m_hOceanSurfaceBindings = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hOceanSurfaceBindingLayout);
	m_needToRebindBeachTargets = false;

}

void OceanRenderer::Shutdown()
{
	free(m_pReadbackPositions);
	free(m_pReadbackResults);
	m_pReadbackPositions = nullptr;
	m_pReadbackResults = nullptr;
	if (!m_pState->bBeachScene)
		m_pState->hDepthBufferTexture = nullptr;
}

void OceanRenderer::SetBeachSceneTargets(nvrhi::ITexture* opaqueScene,
	nvrhi::ITexture* linearDepth)
{
	if (m_hBeachOpaqueSceneTexture == opaqueScene &&
		m_hBeachLinearDepthTexture == linearDepth)
		return;
	m_hBeachOpaqueSceneTexture = opaqueScene;
	m_hBeachLinearDepthTexture = linearDepth;
	m_hFBs.clear();
	m_needToRebindBeachTargets = bool(m_hOceanSurfaceBindingLayout);
}

void OceanRenderer::SetBeachBathymetry(nvrhi::ITexture* bathymetry,
	gfsdk_float2 origin, gfsdk_float2 extent)
{
	if (m_hBeachBathymetryTexture == bathymetry &&
		m_beachBathymetryOrigin.x == origin.x &&
		m_beachBathymetryOrigin.y == origin.y &&
		m_beachBathymetryExtent.x == extent.x &&
		m_beachBathymetryExtent.y == extent.y)
		return;
	m_hBeachBathymetryTexture = bathymetry;
	m_beachBathymetryOrigin = origin;
	m_beachBathymetryExtent = extent;
	m_needToRebindBeachTargets = bool(m_hOceanSurfaceBindingLayout);
}

void OceanRenderer::SetBeachPassTimerQueries(nvrhi::ITimerQuery* water,
	nvrhi::ITimerQuery* foam)
{
	m_waterTimerQuery = water;
	m_foamTimerQuery = foam;
}

bool OceanRenderer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
	m_pState->Camera.KeyboardUpdate(key, scancode, action, mods);
	return false;
}

bool OceanRenderer::MousePosUpdate(double xpos, double ypos)
{
	m_pState->Camera.MousePosUpdate(xpos, ypos);
	return false;
}

bool OceanRenderer::MouseButtonUpdate(int button, int action, int mods)
{
	m_pState->Camera.MouseButtonUpdate(button, action, mods);
	return false;
}

void OceanRenderer::Animate(float elapsedTimeSeconds)
{
	m_pState->Camera.Animate(elapsedTimeSeconds);

	if (m_pState->bUpdateLocalWaves)
	{
	// Adding rain
	if ((m_pState->bRain) && (!m_pState->bNeedToUpdateLocalWavesSimulationProperties))
	{
		gfsdk_float4 dd[200];
		for (int i = 0; i < 200; i++)
		{
			uint32_t x = rand() % m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size;
			uint32_t y = rand() % m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size;
			dd[i].x = (float)x;
			dd[i].y = (float)y;
			dd[i].z = m_pState->fRainDropSize * (rand()%100)*0.01f;
			dd[i].w = 0.0f;
		}
		GFSDK_WaveWorks_Local_Waves_Simulation_AddDisturbances(m_pState->hOceanLocalSimulation, dd, 200);
	}

	// Adding "boat"
	if ((m_pState->bBoat) && (!m_pState->bNeedToUpdateLocalWavesSimulationProperties))
	{
		float time = (float)m_pState->dSimulationTime;
		uint32_t N = m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size;
		m_pState->fBoatX = (float)(100.0f * sinf(time * 0.1f));
		m_pState->fBoatY = (float)(100.0f*cosf(time * 0.13f));
		float dX = 0.1f * 100.0f*cosf((float)time * 0.1f);
		float dY = -0.13f * 100.0f*sinf((float)time * 0.13f);
		float speed = sqrtf(dX*dX + dY*dY);
		m_pState->fBoatX = (float)(100.0f*sinf(time * 0.1f));
		m_pState->fBoatY = (float)(100.0f*cosf(time * 0.13f));
		gfsdk_float4 dd[100];
		for (int32_t i = 0; i < 10; i++)
		{
			for (int32_t j = 0; j < 10; j++)
			{
				float x = m_pState->fBoatX + i - (10 / 2) + (N / 2);
				float y = m_pState->fBoatY + j - (10 / 2) + (N / 2);
				dd[i + 10 * j].x = x;
				dd[i + 10 * j].y = y;
				dd[i + 10 * j].z = 0.13f*speed*elapsedTimeSeconds;
				dd[i + 10 * j].w = 0;
			}
		}
		GFSDK_WaveWorks_Local_Waves_Simulation_AddDisturbances(m_pState->hOceanLocalSimulation, dd, 100);
		gfsdk_float2 cameraXY = { m_pState->fBoatX, m_pState->fBoatY };
		cameraXY *= m_pState->OceanLocalSimulationSettings.simulation_domain_worldspace_size / m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size;
		gfsdk_float3 cameraPos = { cameraXY.x - 30.0f , 10, cameraXY.y - 3.0f};
		gfsdk_float3 atPos = { cameraXY.x , 5, cameraXY.y };
		m_pState->Camera.LookAt(cameraPos, atPos);
	}

	// Simulating local waves
	if (m_pState->bNeedToResetLocalWavesSimulation)
	{
		GFSDK_WaveWorks_Local_Waves_Simulation_Reset(m_pState->hOceanLocalSimulation);
		m_pState->bNeedToResetLocalWavesSimulation = false;
	}

	if (m_pState->bSimulateWater || m_pState->bForceKick || (gfsdk_wwresult_NONE == GFSDK_WaveWorks_Local_Waves_Simulation_GetStagingCursor(m_pState->hOceanLocalSimulation, NULL)))
	{
		GFSDK_WaveWorks_Local_Waves_Simulation_SetDeltaTime(m_pState->hOceanLocalSimulation, elapsedTimeSeconds);
		GFSDK_WaveWorks_Local_Waves_Simulation_Kick(m_pState->hOceanLocalSimulation, &m_pState->iLastLocalSimulationKickID);
	}

	if (m_pState->SyncMode >= SynchronizationMode_RenderOnly)
	{
		// Block until the just-submitted kick is ready to render
		uint64_t stagingCursorKickID = m_pState->iLastLocalSimulationKickID - 1;	// Just ensure that the initial value is different from last kick,
																				// so that we continue waiting if the staging cursor is empty
		GFSDK_WaveWorks_Local_Waves_Simulation_GetStagingCursor(m_pState->hOceanLocalSimulation, &stagingCursorKickID);
		while (stagingCursorKickID != m_pState->iLastLocalSimulationKickID)
		{
			const bool doBlock = true;
			GFSDK_WaveWorks_Local_Waves_Simulation_AdvanceStagingCursor(m_pState->hOceanLocalSimulation, doBlock);
			GFSDK_WaveWorks_Local_Waves_Simulation_GetStagingCursor(m_pState->hOceanLocalSimulation, &stagingCursorKickID);
		}

		if ((m_pState->SyncMode >= SynchronizationMode_Readback) && (m_pState->iReadbackUsage > 0))
		{
			uint64_t readbackCursorKickID = m_pState->iLastLocalSimulationKickID - 1;	// Just ensure that the initial value is different from last kick,
																						// so that we continue waiting if the staging cursor is empty
			while (readbackCursorKickID != m_pState->iLastLocalSimulationKickID)
			{
				const bool doBlock = true;
				GFSDK_WaveWorks_Local_Waves_Simulation_AdvanceReadbackCursor(m_pState->hOceanLocalSimulation, doBlock);
				GFSDK_WaveWorks_Local_Waves_Simulation_GetReadbackCursor(m_pState->hOceanLocalSimulation, &readbackCursorKickID);
			}
		}
	}
	else
	{
		
		// Keep feeding the simulation pipeline until it is full - this loop should skip in all
		// cases except the first iteration, when the simulation pipeline is first 'primed'
		while (GFSDK_WaveWorks_Local_Waves_Simulation_GetStagingCursor(m_pState->hOceanLocalSimulation, NULL) == gfsdk_wwresult_NONE)
		{
			GFSDK_WaveWorks_Local_Waves_Simulation_SetDeltaTime(m_pState->hOceanLocalSimulation, elapsedTimeSeconds);
			GFSDK_WaveWorks_Local_Waves_Simulation_Kick(m_pState->hOceanLocalSimulation, &m_pState->iLastLocalSimulationKickID);
		}
		
	}
	
	// Getting simulation timings
	GFSDK_WaveWorks_Local_Waves_Simulation_GetStats(m_pState->hOceanLocalSimulation, m_pState->OceanLocalSimulationStats);

	// Exponential filtering for stats
	m_pState->OceanLocalSimulationStatsFiltered.CPU_main_thread_wait_time = m_pState->OceanLocalSimulationStatsFiltered.CPU_main_thread_wait_time*0.999f + 0.001f*m_pState->OceanLocalSimulationStats.CPU_main_thread_wait_time;
	m_pState->OceanLocalSimulationStatsFiltered.CPU_threads_total_time = m_pState->OceanLocalSimulationStatsFiltered.CPU_threads_total_time*0.999f + 0.001f*m_pState->OceanLocalSimulationStats.CPU_threads_total_time;
	m_pState->OceanLocalSimulationStatsFiltered.GPU_simulation_time = m_pState->OceanLocalSimulationStatsFiltered.GPU_simulation_time*0.999f + 0.001f*m_pState->OceanLocalSimulationStats.GPU_simulation_time;
	m_pState->OceanLocalSimulationStatsFiltered.GPU_update_time = m_pState->OceanLocalSimulationStatsFiltered.GPU_update_time*0.999f + 0.001f*m_pState->OceanLocalSimulationStats.GPU_update_time;
	m_pState->OceanLocalSimulationStatsFiltered.GPU_total_time = m_pState->OceanLocalSimulationStatsFiltered.GPU_total_time*0.999f + 0.001f*m_pState->OceanLocalSimulationStats.GPU_total_time;

	// deduce the rendering latency of the WaveWorks local waves simulation pipeline
	{
		uint64_t staging_cursor_kickID = 0;
		GFSDK_WaveWorks_Local_Waves_Simulation_GetStagingCursor(m_pState->hOceanLocalSimulation, &staging_cursor_kickID);
		m_pState->iLocalSimulationRenderLatency = (uint32_t)(m_pState->iLastLocalSimulationKickID - staging_cursor_kickID);
	}

	// likewise with the readback latency
	if (m_pState->iReadbackUsage > 0)
	{
		uint64_t readback_cursor_kickID = 0;
		if (GFSDK_WaveWorks_Local_Waves_Simulation_GetReadbackCursor(m_pState->hOceanLocalSimulation, &readback_cursor_kickID) == gfsdk_wwresult_OK)
		{
			m_pState->iLocalSimulationReadbackLatency = (int32_t)(m_pState->iLastLocalSimulationKickID - readback_cursor_kickID);
		}
		else
		{
			m_pState->iLocalSimulationReadbackLatency = -1;
		}
	}
	else
	{
		m_pState->iLocalSimulationReadbackLatency = -1;
	}
	}
	else
	{
		m_pState->bNeedToUpdateLocalWavesSimulationProperties = false;
		m_pState->bNeedToResetLocalWavesSimulation = false;
		m_pState->iLocalSimulationRenderLatency = 0;
		m_pState->iLocalSimulationReadbackLatency = -1;
	}

	if ((m_pState->bUpdateWindWaves || m_pState->bUpdateLocalWaves) &&
		(!m_pState->bBeachScene || m_pState->bSimulateWater || m_pState->bForceKick))
		m_pState->dSimulationTime += elapsedTimeSeconds;

	if (m_pState->bUpdateWindWaves)
	{
	// Simulating wind waves
	if (m_pState->bSimulateWater || m_pState->bForceKick || (gfsdk_wwresult_NONE == GFSDK_WaveWorks_Wind_Waves_Simulation_GetStagingCursor(m_pState->hOceanWindSimulation, NULL)))
	{
		GFSDK_WaveWorks_Wind_Waves_Simulation_SetTime(m_pState->hOceanWindSimulation, m_pState->dSimulationTime);
		GFSDK_WaveWorks_Wind_Waves_Simulation_Kick(m_pState->hOceanWindSimulation, &m_pState->iLastWindSimulationKickID);
	}


	if (m_pState->SyncMode >= SynchronizationMode_RenderOnly)
	{
		// Block until the just-submitted kick is ready to render
		uint64_t stagingCursorKickID = m_pState->iLastWindSimulationKickID - 1;	// Just ensure that the initial value is different from last kick,
															// so that we continue waiting if the staging cursor is empty
		GFSDK_WaveWorks_Wind_Waves_Simulation_GetStagingCursor(m_pState->hOceanWindSimulation, &stagingCursorKickID);
		while (stagingCursorKickID != m_pState->iLastWindSimulationKickID)
		{
			const bool doBlock = true;
			GFSDK_WaveWorks_Wind_Waves_Simulation_AdvanceStagingCursor(m_pState->hOceanWindSimulation, doBlock);
			GFSDK_WaveWorks_Wind_Waves_Simulation_GetStagingCursor(m_pState->hOceanWindSimulation, &stagingCursorKickID);
		}

		if ((m_pState->SyncMode >= SynchronizationMode_Readback) && (m_pState->iReadbackUsage > 0))
		{
			uint64_t readbackCursorKickID = m_pState->iLastWindSimulationKickID - 1;	// Just ensure that the initial value is different from last kick,
																// so that we continue waiting if the staging cursor is empty
			while (readbackCursorKickID != m_pState->iLastWindSimulationKickID)
			{
				const bool doBlock = true;
				GFSDK_WaveWorks_Wind_Waves_Simulation_AdvanceReadbackCursor(m_pState->hOceanWindSimulation, doBlock);
				GFSDK_WaveWorks_Wind_Waves_Simulation_GetReadbackCursor(m_pState->hOceanWindSimulation, &readbackCursorKickID);
			}
		}
	}
	else
	{
		// Keep feeding the simulation pipeline until it is full - this loop should skip in all
		// cases except the first iteration, when the simulation pipeline is first 'primed'
		while (GFSDK_WaveWorks_Wind_Waves_Simulation_GetStagingCursor(m_pState->hOceanWindSimulation, NULL) == gfsdk_wwresult_NONE)
		{
			GFSDK_WaveWorks_Wind_Waves_Simulation_SetTime(m_pState->hOceanWindSimulation, m_pState->dSimulationTime);
			GFSDK_WaveWorks_Wind_Waves_Simulation_Kick(m_pState->hOceanWindSimulation, &m_pState->iLastWindSimulationKickID);
		}
	}

	// Exercise the readback archiving API
	if (GFSDK_WaveWorks_Wind_Waves_Simulation_GetReadbackCursor(m_pState->hOceanWindSimulation, &m_pState->iLastWindSimulationReadbackKickID) == gfsdk_wwresult_OK)
	{
		if ((m_pState->iLastWindSimulationReadbackKickID - m_pState->iLastWindSimulationArchivedKickID) > m_pState->ReadbackArchiveInterval)
		{
			GFSDK_WaveWorks_Wind_Waves_Simulation_ArchiveDisplacements(m_pState->hOceanWindSimulation);
			m_pState->iLastWindSimulationArchivedKickID = m_pState->iLastWindSimulationReadbackKickID;
		}
	}

	// deduce the rendering latency of the WaveWorks wind waves simulation pipeline
	{
		uint64_t staging_cursor_kickID = 0;
		GFSDK_WaveWorks_Wind_Waves_Simulation_GetStagingCursor(m_pState->hOceanWindSimulation, &staging_cursor_kickID);
		m_pState->iWindSimulationRenderLatency = (uint32_t)(m_pState->iLastWindSimulationKickID - staging_cursor_kickID);
	}

	// likewise with the readback latency
	if (m_pState->iReadbackUsage > 0)
	{
		uint64_t readback_cursor_kickID = 0;
		if (GFSDK_WaveWorks_Wind_Waves_Simulation_GetReadbackCursor(m_pState->hOceanWindSimulation, &readback_cursor_kickID) == gfsdk_wwresult_OK)
		{
			m_pState->iWindSimulationReadbackLatency = (int32_t)(m_pState->iLastWindSimulationKickID - readback_cursor_kickID);
		}
		else
		{
			m_pState->iWindSimulationReadbackLatency = -1;
		}
	}
	else
	{
		m_pState->iWindSimulationReadbackLatency = -1;
	}

	// getting simulation timings
	GFSDK_WaveWorks_Wind_Waves_Simulation_GetStats(m_pState->hOceanWindSimulation, m_pState->OceanWindSimulationStats);

	// exponential filtering for stats
	m_pState->OceanWindSimulationStatsFiltered.CPU_main_thread_wait_time = m_pState->OceanWindSimulationStatsFiltered.CPU_main_thread_wait_time*0.999f + 0.001f*m_pState->OceanWindSimulationStats.CPU_main_thread_wait_time;
	m_pState->OceanWindSimulationStatsFiltered.CPU_threads_total_time = m_pState->OceanWindSimulationStatsFiltered.CPU_threads_total_time*0.999f + 0.001f*m_pState->OceanWindSimulationStats.CPU_threads_total_time;
	m_pState->OceanWindSimulationStatsFiltered.GPU_simulation_time = m_pState->OceanWindSimulationStatsFiltered.GPU_simulation_time*0.999f + 0.001f*m_pState->OceanWindSimulationStats.GPU_simulation_time;
	m_pState->OceanWindSimulationStatsFiltered.GPU_update_time = m_pState->OceanWindSimulationStatsFiltered.GPU_update_time*0.999f + 0.001f*m_pState->OceanWindSimulationStats.GPU_update_time;
	m_pState->OceanWindSimulationStatsFiltered.GPU_total_time = m_pState->OceanWindSimulationStatsFiltered.GPU_total_time*0.999f + 0.001f*m_pState->OceanWindSimulationStats.GPU_total_time;

	// reading back marker coords
	if (m_pState->iReadbackUsage > 0)
	{
		UpdateMarkers();
	}
	}
	else
	{
		m_pState->bNeedToUpdateWindWavesSimulationProperties = false;
		m_pState->iWindSimulationRenderLatency = 0;
		m_pState->iWindSimulationReadbackLatency = -1;
	}

	m_pState->bForceKick = false;
}

void OceanRenderer::ResetCameraView()
{
	gfsdk_float3 eyePt = m_pState->bBeachScene
		? gfsdk_float3{ -62.0f, 28.0f, -29.0f }
		: gfsdk_float3{ -50.0f, 20.0f, 30.0f };
	gfsdk_float3 lookAtPt = m_pState->bBeachScene
		? gfsdk_float3{ 5.0f, -8.0f, -28.0f }
		: gfsdk_float3{ 0.0f, 20.0f, 0.0f };
	if (m_pState->bBeachScene && m_pState->bBeachCoveTerrain)
	{
		eyePt = { -72.f, 32.f, -32.f };
		lookAtPt = { 35.f, -25.f, -35.f };
		// Opt-in inspection camera; default composition and reset are unchanged.
		const auto inspection = ReadProcessEnvironmentVariable("WAVEWORKS_BEACH_CAMERA");
		std::istringstream camera(inspection);
		float ex, ey, ez, tx, ty, tz;
		char c0, c1, c2, c3, c4;
		if ((camera >> ex >> c0 >> ey >> c1 >> ez >> c2 >> tx >> c3 >> ty >> c4 >> tz) &&
			(camera >> std::ws).eof() && c0 == ',' && c1 == ',' && c2 == ',' && c3 == ',' && c4 == ',' &&
			std::isfinite(ex) && std::isfinite(ey) && std::isfinite(ez) &&
			std::isfinite(tx) && std::isfinite(ty) && std::isfinite(tz) &&
			std::max({std::abs(ex), std::abs(ey), std::abs(ez), std::abs(tx), std::abs(ty), std::abs(tz)}) < 10000.f &&
			(ex-tx)*(ex-tx)+(ez-tz)*(ez-tz) > .001f)
		{
			eyePt = {ex,ey,ez}; lookAtPt = {tx,ty,tz};
		}
	}
	m_pState->Camera.LookAt(eyePt, lookAtPt);

	m_pState->Camera.SetSpeeds(m_pState->bBeachScene ? 14.0f : 5.0f, 0.005f);
	updateCameraProjection();
}

void OceanRenderer::UpdateMarkers()
{
	
	float distance_between_markers = 2.0f;// *m_pState->OceanWindSimulationSettings.simulation_period / (float)NumMarkersXY;
	float center_x = 0;
	float center_y = 0;
	
	for (int i = 0; i < NumMarkersXY; i++)
		for (int j = 0; j < NumMarkersXY; j++)

	{
			float x = center_x - distance_between_markers*(i - NumMarkersXY / 2);
			float y = center_y - distance_between_markers*(j - NumMarkersXY / 2);
			m_pReadbackPositions[i + j*NumMarkersXY].x = x;
			m_pReadbackPositions[i + j*NumMarkersXY].y = y;
	}
	

	m_pState->fMinDisplacement = 0;
	m_pState->fMaxDisplacement = 0;
	
	/*
	for (int p = 0; p < m_pState->iRefinementSteps; p++)
	{
		GFSDK_WaveWorks_Simulation_GetDisplacements(m_pState->hOceanSimulationForPhysics, m_pReadbackPositions, m_pReadbackResults, NumMarkers, m_pState->iReadbackUsage == 2 ? true : false);
		for (int i = 0; i < NumMarkersXY; i++)
			for (int j = 0; j < NumMarkersXY; j++)

			{
				float x = center_x - distance_between_markers*(i - NumMarkersXY / 2);
				float y = center_y - distance_between_markers*(j - NumMarkersXY / 2);
				m_pReadbackPositions[i + j*NumMarkersXY].x = x;
				m_pReadbackPositions[i + j*NumMarkersXY].y = y;
				m_pReadbackPositions[i + j*NumMarkersXY].x -= m_pReadbackResults[i + j*NumMarkersXY].x;
				m_pReadbackPositions[i + j*NumMarkersXY].y -= m_pReadbackResults[i + j*NumMarkersXY].y;

			}
	}

	for (int i = 0; i < NumMarkersXY; i++)
		for (int j = 0; j < NumMarkersXY; j++)
		{
			float x = center_x - distance_between_markers*(i - NumMarkersXY / 2);
			float y = center_y - distance_between_markers*(j - NumMarkersXY / 2);
			m_pReadbackResults[i + j*NumMarkersXY].x = x;
			m_pReadbackResults[i + j*NumMarkersXY].y = y;
		}
		*/
	int k = 1;
	for (int i = 0; i < k; i++)
	{

		//GFSDK_WaveWorks_Local_Waves_Simulation_GetDisplacements(m_pState->hOceanLocalSimulation, &m_pReadbackPositions[(NumMarkers*i) / k], &m_pReadbackResults[(NumMarkers*i) / k], NumMarkers / k, m_pState->iReadbackUsage == 2 ? true : false);

		GFSDK_WaveWorks_Wind_Waves_Simulation_GetDisplacements(m_pState->hOceanWindSimulation, &m_pReadbackPositions[(NumMarkers*i)/k], &m_pReadbackResults[(NumMarkers*i) / k], NumMarkers/k, m_pState->iReadbackUsage == 2 ? true : false);
		//GFSDK_WaveWorks_Wind_Waves_Simulation_GetArchivedDisplacements(m_pState->hOceanWindSimulation, m_pState->fReadbackCoord, &m_pReadbackPositions[(NumMarkers*i) / k], &m_pReadbackResults[(NumMarkers*i) / k], NumMarkers / k);
	}
	
	float variance_sqr = 0;
	for (int i = 0; i < NumMarkers; i++)
	{
		m_pReadbackResults[i].x += m_pReadbackPositions[i].x;
		m_pReadbackResults[i].y += m_pReadbackPositions[i].y;
		if (m_pReadbackResults[i].z < m_pState->fMinDisplacement) m_pState->fMinDisplacement = m_pReadbackResults[i].z;
		if (m_pReadbackResults[i].z > m_pState->fMaxDisplacement) m_pState->fMaxDisplacement = m_pReadbackResults[i].z;
		variance_sqr += m_pReadbackResults[i].z*m_pReadbackResults[i].z;
	}
	if (m_pState->fMaxTotalDisplacement < m_pState->fMaxDisplacement) m_pState->fMaxTotalDisplacement = m_pState->fMaxDisplacement;
	if (m_pState->fMinTotalDisplacement > m_pState->fMinDisplacement) m_pState->fMinTotalDisplacement = m_pState->fMinDisplacement;
	variance_sqr /= NumMarkers;
	m_pState->fSWH = 4.0f * sqrtf(variance_sqr);
	
	
}

void OceanRenderer::RenderMarkers(nvrhi::FramebufferHandle hFramebuffer)
{
	// Set initial resource states (command list has been opened already in OceanRenderer::Render)
	m_hCommandList->beginTrackingBufferState(m_hMarkerVSCB, nvrhi::ResourceStates::ConstantBuffer);
	m_hCommandList->beginTrackingBufferState(m_hMarkerInstanceBuffer, nvrhi::ResourceStates::VertexBuffer);

	// Initialize sky draw state
	nvrhi::Viewport vp;
	vp.maxX = float(m_backBufferWidth);
	vp.maxY = float(m_backBufferHeight);

	nvrhi::Rect scissor;
	scissor.maxX = m_backBufferWidth;
	scissor.maxY = m_backBufferHeight;

	m_drawState.viewport.viewports.resize(1);
	m_drawState.viewport.viewports[0] = vp;
	m_drawState.viewport.scissorRects = { scissor };

	// Update constant buffers
	MARKER_VS_CBUFFER VSCB;

	gfsdk_float4x4 viewProjMatrix = m_pState->Camera.GetWorldToClip();
	gfsdk_float3 eyePoint = m_pState->Camera.GetCameraPosition();

	VSCB.g_matViewProj = { viewProjMatrix._11, viewProjMatrix._12, viewProjMatrix._13, viewProjMatrix._14,
		viewProjMatrix._31, viewProjMatrix._32, viewProjMatrix._33,viewProjMatrix._34,
		viewProjMatrix._21, viewProjMatrix._22, viewProjMatrix._23,viewProjMatrix._24,
		viewProjMatrix._41, viewProjMatrix._42, viewProjMatrix._43,viewProjMatrix._44 };

	m_hCommandList->writeBuffer(m_hMarkerVSCB, &VSCB, sizeof(VSCB));

	// Uploading the readback results to GPU
	gfsdk_float4* uploadData = (gfsdk_float4*)m_pDevice->mapBuffer(m_hMarkerInstanceUploadBuffers[m_markerInstanceUploadBufferNumber], nvrhi::CpuAccessMode::Write);
	memcpy(uploadData, m_pReadbackResults, NumMarkers * sizeof(gfsdk_float4));
	m_pDevice->unmapBuffer(m_hMarkerInstanceUploadBuffers[m_markerInstanceUploadBufferNumber]);

	m_hCommandList->copyBuffer(m_hMarkerInstanceBuffer, 0, m_hMarkerInstanceUploadBuffers[m_markerInstanceUploadBufferNumber], 0, NumMarkers * sizeof(gfsdk_float4));

	m_markerInstanceUploadBufferNumber ++;
	m_markerInstanceUploadBufferNumber %= 3;

	// Rendering
	nvrhi::DrawArguments da;
	da.vertexCount = 12;
	da.instanceCount = NumMarkers;
	da.startIndexLocation = 0;
	da.startVertexLocation = 0;

	m_drawState.indexBuffer.buffer = m_hMarkerIB;
	m_drawState.indexBuffer.format = nvrhi::Format::R32_UINT;
	m_drawState.indexBuffer.offset = 0;
	m_drawState.vertexBuffers.resize(2);
	m_drawState.vertexBuffers[0].buffer = m_hMarkerVB;
	m_drawState.vertexBuffers[0].slot = 0;
	m_drawState.vertexBuffers[0].offset = 0;
	m_drawState.vertexBuffers[1].buffer = m_hMarkerInstanceBuffer;
	m_drawState.vertexBuffers[1].slot = 1;
	m_drawState.vertexBuffers[1].offset = 0;

	m_drawState.framebuffer = hFramebuffer;
	m_drawState.pipeline = m_hMarkerPSO;
	m_drawState.bindings = ( m_hMarkerBindings ).sets();
	m_hCommandList->setGraphicsState(m_drawState);
	m_hCommandList->drawIndexed(da);

	// Set required resource states (command list will be closed in OceanRenderer::Render)
	m_hCommandList->setBufferState(m_hMarkerVSCB, nvrhi::ResourceStates::ConstantBuffer);
	m_hCommandList->commitBarriers();
	m_hCommandList->setBufferState(m_hMarkerInstanceBuffer, nvrhi::ResourceStates::VertexBuffer);
	m_hCommandList->commitBarriers();
}
