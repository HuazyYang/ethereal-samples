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
#include "./sky_renderer.h"
#include "./asset_utils.h"

#include <shaders/bytecode/SkyRenderingVS.spirv.h>
#include <shaders/bytecode/SkyRenderingPS.spirv.h>
#include <shaders/bytecode/FullscreenVS.spirv.h>
#include <shaders/bytecode/SkyUpdatePS.spirv.h>
#include <shaders/bytecode/MipmapGenerationPS.spirv.h>
#include <shaders/bytecode/SkyRenderingVS.dxbc.h>
#include <shaders/bytecode/SkyRenderingPS.dxbc.h>
#include <shaders/bytecode/FullscreenVS.dxbc.h>
#include <shaders/bytecode/SkyUpdatePS.dxbc.h>
#include <shaders/bytecode/MipmapGenerationPS.dxbc.h>
#include <shaders/bytecode/BeachSkyPS.spirv.h>
#include <shaders/bytecode/BeachSkyPS.dxbc.h>

struct vs_sky_cbuffer
{
	gfsdk_float4x4 g_matViewProj;
};

struct ps_sky_cbuffer
{
	gfsdk_float3 g_sunDirection;
	float g_sunIntensity;
	float g_skyRotation;
	float g_rayleighScattering;
	float g_mieScattering;
	float g_useDynamicSkyTexture;
};

#ifndef PI 
#define PI 3.141593f
#endif

bool SkyRenderer::Init()
{
	nvrhi::CommandListParameters cmdListParams;
	m_pDevice->createCommandList(cmdListParams, &m_hCommandList);
	m_hCommandList->open();

	// Standalone ocean retains the legacy shared-depth allocation. In the beach
	// executable, BeachScenePipeline owns this texture for its entire lifetime.
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
		td.debugName = "Depth target created in SkyRenderer::Init()";
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

	// Initialize frame buffer #0
	td.width = m_backBufferWidth;
	td.height = m_backBufferHeight;
	td.sampleCount = m_sampleCount;
	td.isRenderTarget = true;
	td.debugName = "proxy color target";
	td.format = m_pState->bBeachScene ? nvrhi::Format::RGBA16_FLOAT :
		m_backend == nvrhi::GraphicsAPI::VULKAN
		? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
	m_pDevice->createTexture(td, &m_hInitTimeRenderTargetTexture);
	nvrhi::FramebufferDesc fbDesc;
	fbDesc.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(m_hInitTimeRenderTargetTexture).setArraySlice(0).setMipLevel(0));
	fbDesc.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_pState->hDepthBufferTexture).setArraySlice(0).setMipLevel(0));
	m_pDevice->createFramebuffer(fbDesc, &m_hInitTimeFB);


	// Load shaders
	const bool useVk = m_backend == nvrhi::GraphicsAPI::VULKAN;
	const void *VSBinary = useVk ? static_cast<const void *>(g_SkyRenderingVS_spirv) : g_SkyRenderingVS_dxbc;
	size_t VSBinarySize = useVk ? sizeof(g_SkyRenderingVS_spirv) : sizeof(g_SkyRenderingVS_dxbc);
	const void *PSBinary = m_pState->bBeachScene
		? (useVk ? static_cast<const void *>(g_BeachSkyPS_spirv) : g_BeachSkyPS_dxbc)
		: (useVk ? static_cast<const void *>(g_SkyRenderingPS_spirv) : g_SkyRenderingPS_dxbc);
	size_t PSBinarySize = m_pState->bBeachScene
		? (useVk ? sizeof(g_BeachSkyPS_spirv) : sizeof(g_BeachSkyPS_dxbc))
		: (useVk ? sizeof(g_SkyRenderingPS_spirv) : sizeof(g_SkyRenderingPS_dxbc));

	m_hRenderSkyVS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), VSBinary, VSBinarySize);
	if (m_hRenderSkyVS == nullptr) return false;

	m_hRenderSkyPS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSBinary, PSBinarySize);
	if (m_hRenderSkyPS == nullptr) return false;

	// Create input layout
	nvrhi::VertexAttributeDesc render_sky_layout[] = {
		{ "POSITION", nvrhi::Format::RGBA32_FLOAT, 1, 0, 0, 24, false },
		{ "TEXCOORD", nvrhi::Format::RG32_FLOAT, 1, 0, 16, 24, false },
	};

	m_pDevice->createInputLayout(render_sky_layout, 2, m_hRenderSkyVS, &m_hRenderSkyLayout);

	// Create textures
	if (!m_pState->bBeachScene)
	{
		m_hStaticSkyDomeTexture = CreateTextureFromFile(m_pDevice, m_hCommandList,
			(GetMediaFolder()/"sky_dome_1.dds").generic_string().c_str());
		if (m_hStaticSkyDomeTexture == nullptr) return false;
	}

	if (m_pState->bBeachScene)
	{
		// BeachScenePipeline supplies a scene-linear physical atmosphere and
		// owns its lifetime. Never overwrite it with the legacy LDR panorama.
	}
	else
	{
		td.width = SKYDOME_TEXTURE_SIZE;
		td.height = SKYDOME_TEXTURE_SIZE;
		td.sampleCount = 1;
		td.debugName = "Dynamic sky texture";
		td.format = nvrhi::Format::RGBA16_FLOAT;
		td.isRenderTarget = true;
		td.mipLevels = SKYDOME_TEXTURE_MIPMAPCOUNT;
		m_pDevice->createTexture(td, &m_pState->hDynamicSkyDomeTexture);
	}
	if (!m_pState->hDynamicSkyDomeTexture) return false;

	// Create samplers
	nvrhi::SamplerDesc samplerDesc;
	samplerDesc.magFilter = true;
	samplerDesc.minFilter = true;
	samplerDesc.mipFilter = true;
	samplerDesc.addressU = m_pState->bBeachScene
		? nvrhi::SamplerAddressMode::Wrap
		: nvrhi::SamplerAddressMode::Clamp;
	samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp; // Need it to avoid issues sampling the skydome texture in singularities in zenith and nadir points
	samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
	m_pDevice->createSampler(samplerDesc, &m_hBilinearSampler);
	samplerDesc.maxAnisotropy = 16.0f;
	m_pDevice->createSampler(samplerDesc, &m_hAnisotropicSampler);

	// Init constant buffer for vertex shader
	nvrhi::BufferDesc cbDesc;
	cbDesc.byteSize = sizeof(vs_sky_cbuffer);
	cbDesc.debugName = "Sky VS CB";
	cbDesc.isConstantBuffer = true;
	m_pDevice->createBuffer(cbDesc, &m_hRenderSkyVSCB);

	// Init constant buffer for pixel shader
	cbDesc.byteSize = sizeof(ps_sky_cbuffer);
	cbDesc.debugName = "Sky PS CB";
	cbDesc.isConstantBuffer = true;
	m_pDevice->createBuffer(cbDesc, &m_hPSCB);

	// Create sky rendering PSO & bindings
	nvrhi::BlendState blendState;
	blendState.targets[0].blendEnable = false;

	nvrhi::RasterState rasterState;
	rasterState.fillMode = nvrhi::RasterFillMode::Solid;
	rasterState.cullMode = nvrhi::RasterCullMode::None;
	rasterState.scissorEnable = false;
	rasterState.depthClipEnable = false;

	nvrhi::DepthStencilState depthStencilState;
	depthStencilState.depthTestEnable = true;
	// Sky is background-only: test against opaque geometry, but never modify the
	// depth buffer needed by the later transmissive-water pass.
	depthStencilState.depthWriteEnable = false;
	depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
	depthStencilState.stencilEnable = false;

	nvrhi::RenderState renderState;
	renderState.blendState = blendState;
	renderState.depthStencilState = depthStencilState;
	renderState.rasterState = rasterState;

	nvrhi::GraphicsPipelineDesc psoDesc;
	waveworks::graphics::GraphicsBindingSetDesc bsDesc;
	waveworks::graphics::GraphicsBindingLayoutDesc layoutDesc;

	layoutDesc.VS = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
	layoutDesc.PS = { nvrhi::BindingLayoutItem::Texture_SRV(0),
						nvrhi::BindingLayoutItem::Sampler(0),
						nvrhi::BindingLayoutItem::ConstantBuffer(0) };
	m_hRenderSkyBindingLayout =  waveworks::graphics::createBindingLayout(m_pDevice, layoutDesc);

	bsDesc.VS = { nvrhi::BindingSetItem::ConstantBuffer(0, m_hRenderSkyVSCB ) };
	bsDesc.PS = { nvrhi::BindingSetItem::Texture_SRV(0, m_pState->hDynamicSkyDomeTexture,
					nvrhi::Format::RGBA16_FLOAT),
					nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler ),
					nvrhi::BindingSetItem::ConstantBuffer(0, m_hPSCB ) };
	m_hRenderSkyBindingSet = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hRenderSkyBindingLayout);

	psoDesc.bindingLayouts = ( m_hRenderSkyBindingLayout ).layouts;
	psoDesc.VS = m_hRenderSkyVS;
	psoDesc.PS = m_hRenderSkyPS;
	psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
	psoDesc.inputLayout = m_hRenderSkyLayout;
	psoDesc.renderState = renderState;
	m_pDevice->createGraphicsPipeline1(psoDesc, m_hInitTimeFB->getFramebufferInfo().getInfo(), &m_hRenderSkyPSO);



	// Create VB and IB for skydome
	uint32_t skydome_vertices_count = SKYDOME_SLICES_COUNT*(SKYDOME_SLICES_COUNT + 2) * 2;
	
	float *skydome_vertexdata = (float*) malloc(skydome_vertices_count * 6 * sizeof(float));
	uint32_t *skydome_indexdata = (uint32_t*) malloc(skydome_vertices_count * sizeof(uint32_t));

	for (uint32_t i = 0; i < skydome_vertices_count; i++) skydome_indexdata[i] = i;
	
	float skydome_radius = m_pState->fCameraClipFar*0.8f;

	for (int j = 0; j < SKYDOME_SLICES_COUNT; j++)
	{
		int i = 0;
		int floatnum = (j*(SKYDOME_SLICES_COUNT + 2) * 2) * 6;
		skydome_vertexdata[floatnum + 0] = skydome_radius*cos(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)j / (float)SKYDOME_SLICES_COUNT);
		skydome_vertexdata[floatnum + 2] = skydome_radius*sin(-0.5f*PI + PI*(float)(j) / (float)SKYDOME_SLICES_COUNT);
		skydome_vertexdata[floatnum + 1] = skydome_radius*sin(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)j / (float)SKYDOME_SLICES_COUNT);
		skydome_vertexdata[floatnum + 3] = 1.0f;
		skydome_vertexdata[floatnum + 4] = (float)i / (float)SKYDOME_SLICES_COUNT;
		skydome_vertexdata[floatnum + 5] = (float)(j + 0) / (float)SKYDOME_SLICES_COUNT;
		floatnum += 6;
		for (i = 0; i < SKYDOME_SLICES_COUNT + 1; i++)
		{
			skydome_vertexdata[floatnum + 0] = skydome_radius*cos(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)j / (float)SKYDOME_SLICES_COUNT);
			skydome_vertexdata[floatnum + 2] = skydome_radius*sin(-0.5f*PI + PI*(float)(j) / (float)SKYDOME_SLICES_COUNT);
			skydome_vertexdata[floatnum + 1] = skydome_radius*sin(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)j / (float)SKYDOME_SLICES_COUNT);
			skydome_vertexdata[floatnum + 3] = 1.0f;
			skydome_vertexdata[floatnum + 4] = (float)i / (float)SKYDOME_SLICES_COUNT;
			skydome_vertexdata[floatnum + 5] = 1.0f*(float)(j + 0) / (float)SKYDOME_SLICES_COUNT;
			floatnum += 6;
			skydome_vertexdata[floatnum + 0] = skydome_radius*cos(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT);
			skydome_vertexdata[floatnum + 2] = skydome_radius*sin(-0.5f*PI + PI*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT);
			skydome_vertexdata[floatnum + 1] = skydome_radius*sin(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT);
			skydome_vertexdata[floatnum + 3] = 1.0f;
			skydome_vertexdata[floatnum + 4] = (float)i / (float)SKYDOME_SLICES_COUNT;
			skydome_vertexdata[floatnum + 5] = 1.0f*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT;
			floatnum += 6;
		}
		i = SKYDOME_SLICES_COUNT;
		skydome_vertexdata[floatnum + 0] = skydome_radius*cos(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT);
		skydome_vertexdata[floatnum + 2] = skydome_radius*sin(-0.5f*PI + PI*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT);
		skydome_vertexdata[floatnum + 1] = skydome_radius*sin(2.0f*PI*(float)i / (float)SKYDOME_SLICES_COUNT)*cos(-0.5f*PI + PI*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT);
		skydome_vertexdata[floatnum + 3] = 1.0f;
		skydome_vertexdata[floatnum + 4] = (float)i / (float)SKYDOME_SLICES_COUNT;
		skydome_vertexdata[floatnum + 5] = 1.0f*(float)(j + 1) / (float)SKYDOME_SLICES_COUNT;
	}

	nvrhi::BufferDesc bd;
	bd.byteSize = skydome_vertices_count * 6 * sizeof(float);
	bd.isVertexBuffer = true;
	bd.isIndexBuffer = false;
	m_pDevice->createBuffer(bd, &m_hRenderSkyVertexBuffer);
	m_hCommandList->beginTrackingBufferState(m_hRenderSkyVertexBuffer, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hRenderSkyVertexBuffer, skydome_vertexdata, bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hRenderSkyVertexBuffer, nvrhi::ResourceStates::VertexBuffer);
	m_hCommandList->commitBarriers();
	
	bd.byteSize = skydome_vertices_count * sizeof(uint32_t);
	bd.isVertexBuffer = false;
	bd.isIndexBuffer = true;
	m_pDevice->createBuffer(bd, &m_hRenderSkyIndexBuffer);
	m_hCommandList->beginTrackingBufferState(m_hRenderSkyIndexBuffer, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hRenderSkyIndexBuffer, skydome_indexdata, bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hRenderSkyIndexBuffer, nvrhi::ResourceStates::IndexBuffer);
	m_hCommandList->commitBarriers();

	free(skydome_vertexdata);
	free(skydome_indexdata);

	if (m_pState->bBeachScene)
	{
		m_hCommandList->beginTrackingBufferState(m_hRenderSkyVSCB,
			nvrhi::ResourceStates::Common);
		m_hCommandList->beginTrackingBufferState(m_hPSCB,
			nvrhi::ResourceStates::Common);
		m_hCommandList->setBufferState(m_hRenderSkyVSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->commitBarriers();
		m_hCommandList->setBufferState(m_hPSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->commitBarriers();
		m_hCommandList->close();
		m_pDevice->executeCommandList(m_hCommandList);
		return m_hRenderSkyLayout && m_hRenderSkyBindingSet && m_hRenderSkyPSO;
	}
	
	// Creating resources for updating sky
	
	// Shaders
	VSBinary = useVk ? static_cast<const void *>(g_FullscreenVS_spirv) : g_FullscreenVS_dxbc;
	VSBinarySize = useVk ? sizeof(g_FullscreenVS_spirv) : sizeof(g_FullscreenVS_dxbc);
	PSBinary = useVk ? static_cast<const void *>(g_SkyUpdatePS_spirv) : g_SkyUpdatePS_dxbc;
	PSBinarySize = useVk ? sizeof(g_SkyUpdatePS_spirv) : sizeof(g_SkyUpdatePS_dxbc);

	m_hFullscreenVS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), VSBinary, VSBinarySize);
	if (m_hFullscreenVS == nullptr) return false;

	m_hUpdateSkyPS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSBinary, PSBinarySize);
	if (m_hUpdateSkyPS == nullptr) return false;

	// Input layout
	nvrhi::VertexAttributeDesc update_sky_layout[] = {
		{ "POSITION", nvrhi::Format::RGBA32_FLOAT, 1, 0, 0, 24, false },
		{ "TEXCOORD", nvrhi::Format::RG32_FLOAT, 1, 0, 16, 24, false },
	};
	m_pDevice->createInputLayout(update_sky_layout, 2, m_hFullscreenVS, &m_hUpdateSkyLayout);

	// Draw state
	depthStencilState.depthTestEnable = false;
	renderState.blendState = blendState;
	renderState.depthStencilState = depthStencilState;
	renderState.rasterState = rasterState;

	// PSO & bindings
	layoutDesc.VS = { };
	layoutDesc.PS = { nvrhi::BindingLayoutItem::Texture_SRV(0),
						nvrhi::BindingLayoutItem::Sampler(0),
						nvrhi::BindingLayoutItem::ConstantBuffer(0) };
	m_hUpdateSkyBindingLayout = waveworks::graphics::createBindingLayout(m_pDevice, layoutDesc);

	bsDesc.VS = { };
	bsDesc.PS = { nvrhi::BindingSetItem::Texture_SRV(0, m_hStaticSkyDomeTexture, nvrhi::Format::RGBA8_UNORM ),
					nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler ),
					nvrhi::BindingSetItem::ConstantBuffer(0, m_hPSCB ) };
	m_hUpdateSkyBindingSet = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hUpdateSkyBindingLayout);
	
	fbDesc.colorAttachments[0] = nvrhi::FramebufferAttachment().setTexture( m_pState->hDynamicSkyDomeTexture).setArraySlice(0).setMipLevel(0);
	fbDesc.depthAttachment = {};
	m_pDevice->createFramebuffer(fbDesc, &m_hUpdateSkyFB);

	psoDesc.bindingLayouts = ( m_hUpdateSkyBindingLayout ).layouts;
	psoDesc.VS = m_hFullscreenVS;
	psoDesc.PS = m_hUpdateSkyPS;
	psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
	psoDesc.inputLayout = m_hUpdateSkyLayout;
	psoDesc.renderState = renderState;
	m_pDevice->createGraphicsPipeline1(psoDesc, m_hUpdateSkyFB->getFramebufferInfo().getInfo(), &m_hUpdateSkyPSO);

	// Index & vertex buffers
	float tex_adjust = 0.0f;
	float vertices[24] = { -1.0f,  1.0f, 0,	1.0f, tex_adjust,        tex_adjust,
							-1.0f, -1.0f, 0, 1.0f, tex_adjust,        tex_adjust + 1.0f,
							1.0f,  1.0f, 0, 1.0f, tex_adjust + 1.0f, tex_adjust,
							1.0f, -1.0f, 0, 1.0f, tex_adjust + 1.0f, tex_adjust + 1.0f };
	uint32_t indices[4] = { 0, 1, 2, 3 };

	bd.byteSize = 24 * sizeof(float);
	bd.isVertexBuffer = true;
	bd.isIndexBuffer = false;
	m_pDevice->createBuffer(bd, &m_hUpdateSkyVertexBuffer);
	m_hCommandList->beginTrackingBufferState(m_hUpdateSkyVertexBuffer, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hUpdateSkyVertexBuffer, vertices, bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hUpdateSkyVertexBuffer, nvrhi::ResourceStates::VertexBuffer);
	m_hCommandList->commitBarriers();


	bd.byteSize = 4 * sizeof(uint32_t);
	bd.isVertexBuffer = false;
	bd.isIndexBuffer = true;
	m_pDevice->createBuffer(bd, &m_hUpdateSkyIndexBuffer);
	m_hCommandList->beginTrackingBufferState(m_hUpdateSkyIndexBuffer, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hUpdateSkyIndexBuffer, indices, bd.byteSize);
	m_hCommandList->setPermanentBufferState(m_hUpdateSkyIndexBuffer, nvrhi::ResourceStates::IndexBuffer);
	m_hCommandList->commitBarriers();

	// Mipmap generation shader
	PSBinary = useVk ? static_cast<const void *>(g_MipmapGenerationPS_spirv) : g_MipmapGenerationPS_dxbc;
	PSBinarySize = useVk ? sizeof(g_MipmapGenerationPS_spirv) : sizeof(g_MipmapGenerationPS_dxbc);

	m_hMipGenPS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSBinary, PSBinarySize);
	if (m_hMipGenPS == nullptr) return false;

	// Mipmap generation PSO & bindings

	layoutDesc.VS = {};
	layoutDesc.PS = { nvrhi::BindingLayoutItem::Texture_SRV(0),
						nvrhi::BindingLayoutItem::Sampler(0) };
	m_hMipGenBindingLayout = waveworks::graphics::createBindingLayout(m_pDevice, layoutDesc);

	psoDesc.bindingLayouts = ( m_hMipGenBindingLayout ).layouts;
	psoDesc.VS = m_hFullscreenVS;
	psoDesc.PS = m_hMipGenPS;

	int target_mip_size = SKYDOME_TEXTURE_SIZE / 2;
	for (int mip_level = 0; mip_level < SKYDOME_TEXTURE_MIPMAPCOUNT - 1; mip_level++)
	{
		fbDesc.colorAttachments[0].texture = m_pState->hDynamicSkyDomeTexture;
		fbDesc.colorAttachments[0].subresources = nvrhi::TextureSubresourceSet(mip_level + 1, 1, 0, 1);
		m_pDevice->createFramebuffer(fbDesc, &m_hMipGenFB[mip_level]);
		m_pDevice->createGraphicsPipeline1(psoDesc, m_hMipGenFB[mip_level]->getFramebufferInfo().getInfo(), &m_hMipGenPSO[mip_level]);

		bsDesc.VS = {};
		bsDesc.PS = { nvrhi::BindingSetItem::Texture_SRV(0, m_pState->hDynamicSkyDomeTexture,  nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(mip_level, 1, 0, 1) ),
						nvrhi::BindingSetItem::Sampler(0, m_hBilinearSampler ) };
		m_hMipGenBindingSet[mip_level] = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hMipGenBindingLayout);

		target_mip_size /= 2;
	}

	// Set initial state for resources
	m_hCommandList->beginTrackingTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
	m_hCommandList->beginTrackingBufferState(m_hRenderSkyVSCB, nvrhi::ResourceStates::Common);
	m_hCommandList->beginTrackingBufferState(m_hPSCB, nvrhi::ResourceStates::Common);

	m_hCommandList->setTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
	m_hCommandList->commitBarriers();
	m_hCommandList->setBufferState(m_hRenderSkyVSCB, nvrhi::ResourceStates::ConstantBuffer);
	m_hCommandList->commitBarriers();
	m_hCommandList->setBufferState(m_hPSCB, nvrhi::ResourceStates::ConstantBuffer);
	m_hCommandList->commitBarriers();
	m_hCommandList->close();
	m_pDevice->executeCommandList(m_hCommandList);

	return true;
}

void SkyRenderer::BackBufferResizing()
{
	m_hRenderSkyFBs.clear();
	if (!m_pState->bBeachScene)
		m_pState->hDepthBufferTexture = nullptr;
}

void SkyRenderer::BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount)
{
	m_backBufferWidth = width;
	m_backBufferHeight = height;
	m_sampleCount = sampleCount;
	
	if (m_pDevice == nullptr) return;
	if (m_pState == nullptr) return;

	m_needToRecreatePSOs = true;
}


void SkyRenderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    auto* renderTarget = framebuffer->getDesc().colorAttachments[0].texture;
    const uint32_t swapChainIndex = GetDeviceManager()->GetCurrentBackBufferIndex();
	if (m_pState->bRenderSky)
	{
		m_hCommandList->open();
		nvrhi::FramebufferHandle FB = getFramebuffer(renderTarget, swapChainIndex);
		m_hCommandList->beginTrackingTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingBufferState(m_hRenderSkyVSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->beginTrackingBufferState(m_hPSCB, nvrhi::ResourceStates::ConstantBuffer);
		if (!renderTarget->getDesc().keepInitialState)
			m_hCommandList->beginTrackingTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
		m_hCommandList->beginTrackingTextureState(m_pState->hDepthBufferTexture,
			nvrhi::AllSubresources, m_pState->bBeachScene
				? nvrhi::ResourceStates::DepthRead : nvrhi::ResourceStates::DepthWrite);

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
		vs_sky_cbuffer VSCB;

		gfsdk_float4x4 viewProjMatrix = m_pState->Camera.GetWorldToClip();
		if (m_pState->bBeachScene)
		{
			auto viewRotation = m_pState->Camera.GetWorldToView();
			viewRotation._41 = viewRotation._42 = viewRotation._43 = 0.f;
			viewProjMatrix = m_pState->Camera.GetProjection() * viewRotation;
		}
		gfsdk_float3 eyePoint = m_pState->Camera.GetCameraPosition();

		VSCB.g_matViewProj = { viewProjMatrix._11, viewProjMatrix._12, viewProjMatrix._13, viewProjMatrix._14,
								viewProjMatrix._31, viewProjMatrix._32, viewProjMatrix._33,viewProjMatrix._34,
								viewProjMatrix._21, viewProjMatrix._22, viewProjMatrix._23,viewProjMatrix._24,
								viewProjMatrix._41, viewProjMatrix._42, viewProjMatrix._43,viewProjMatrix._44 };
		m_hCommandList->writeBuffer(m_hRenderSkyVSCB, &VSCB, sizeof(VSCB));
		
		ps_sky_cbuffer PSCB{};
		PSCB.g_sunIntensity = m_pState->fSunIntensity;
		PSCB.g_sunDirection.x = cosf(m_pState->fSunAngle * 0.0174533f);
		PSCB.g_sunDirection.y = 0;
		PSCB.g_sunDirection.z = sinf(m_pState->fSunAngle * 0.0174533f);
		PSCB.g_useDynamicSkyTexture = m_pState->bUseDynamicSky ? 1.0f : 0.0f;
		m_hCommandList->writeBuffer(m_hPSCB, &PSCB, sizeof(PSCB));

		// Rendering
		nvrhi::DrawArguments da;
		da.vertexCount = SKYDOME_SLICES_COUNT*(SKYDOME_SLICES_COUNT + 2) * 2;
		da.startIndexLocation = 0;
		da.startVertexLocation = 0;

		m_drawState.indexBuffer.buffer = m_hRenderSkyIndexBuffer;
		m_drawState.indexBuffer.format = nvrhi::Format::R32_UINT;
		m_drawState.indexBuffer.offset = 0;
		m_drawState.vertexBuffers.resize(1);
		m_drawState.vertexBuffers[0].buffer = m_hRenderSkyVertexBuffer;
		m_drawState.vertexBuffers[0].slot = 0;
		m_drawState.vertexBuffers[0].offset = 0;

		m_drawState.framebuffer = FB;
		m_drawState.pipeline = m_hRenderSkyPSO;
		m_drawState.bindings = ( m_hRenderSkyBindingSet ).sets();

		m_hCommandList->setGraphicsState(m_drawState);
		m_hCommandList->drawIndexed(da);

		m_hCommandList->setTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setBufferState(m_hRenderSkyVSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->commitBarriers();
		m_hCommandList->setBufferState(m_hPSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, m_pState->bBeachScene
				? nvrhi::ResourceStates::DepthRead : nvrhi::ResourceStates::DepthWrite);
		m_hCommandList->commitBarriers();
		m_hCommandList->close();
		m_pDevice->executeCommandList(m_hCommandList);
	}
}

nvrhi::FramebufferHandle SkyRenderer::getFramebuffer(nvrhi::TextureHandle renderTarget, uint32_t swapChainIndex)
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
		m_pDevice->createTexture(desc, &m_pState->hDepthBufferTexture);
		assert(m_pState->hDepthBufferTexture);
		m_hCommandList->beginTrackingTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
		m_hCommandList->setTextureState(m_pState->hDepthBufferTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
		m_hCommandList->commitBarriers();
	}

	// Create framebuffers if needed
	if (swapChainIndex >= m_hRenderSkyFBs.size()) m_hRenderSkyFBs.resize(swapChainIndex + 1);

	if (m_hRenderSkyFBs[swapChainIndex] == nullptr)
	{
		nvrhi::FramebufferDesc desc = nvrhi::FramebufferDesc()
			.addColorAttachment(renderTarget)
			.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_pState->hDepthBufferTexture).setArraySlice(0).setMipLevel(0).setReadOnly(m_pState->bBeachScene));

		m_pDevice->createFramebuffer(desc, &m_hRenderSkyFBs[swapChainIndex]);
	}


	if (m_needToRecreatePSOs)
	{
		// Recreating PSOs
		nvrhi::GraphicsPipelineDesc renderSkyPSOdesc = m_hRenderSkyPSO->getDesc();
		m_hRenderSkyPSO = nullptr;
		m_pDevice->createGraphicsPipeline1(renderSkyPSOdesc, m_hRenderSkyFBs[swapChainIndex]->getFramebufferInfo().getInfo(), &m_hRenderSkyPSO);
		m_needToRecreatePSOs = false;
	}

	assert(m_hRenderSkyFBs.at(swapChainIndex));
	return m_hRenderSkyFBs[swapChainIndex];
}

void SkyRenderer::Shutdown()
{
	m_pState->hDynamicSkyDomeTexture = nullptr;
}

bool SkyRenderer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
	m_pState->Camera.KeyboardUpdate(key, scancode, action, mods);
	return false;
}

bool SkyRenderer::MousePosUpdate(double xpos, double ypos)
{
	m_pState->Camera.MousePosUpdate(xpos, ypos);
	return false;
}

bool SkyRenderer::MouseButtonUpdate(int button, int action, int mods)
{
	m_pState->Camera.MouseButtonUpdate(button, action, mods);
	return false;
}

void SkyRenderer::Animate(float elapsedTimeSeconds)
{
	if (m_pState->bBeachScene)
		return;

	// update dynamic sky texture if/when needed
	if (m_pState->bNeedToUpdateSky)
	{
		m_pState->bNeedToUpdateSky = false;

		m_hCommandList->open();
		m_hCommandList->beginTrackingTextureState(m_hStaticSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->beginTrackingBufferState(m_hPSCB, nvrhi::ResourceStates::ConstantBuffer);

		// Initialize sky draw state
		nvrhi::Viewport vp;
		vp.maxX = float(SKYDOME_TEXTURE_SIZE);
		vp.maxY = float(SKYDOME_TEXTURE_SIZE);

		nvrhi::Rect scissor;
		scissor.maxX = SKYDOME_TEXTURE_SIZE;
		scissor.maxY = SKYDOME_TEXTURE_SIZE;

		m_drawState.viewport.viewports.resize(1);
		m_drawState.viewport.viewports[0] = vp;
		m_drawState.viewport.scissorRects = { scissor };

		// Update constant buffers
		ps_sky_cbuffer PSCB{};
		PSCB.g_sunIntensity = m_pState->fSunIntensity;
		PSCB.g_sunDirection.x = cosf(m_pState->fSunAngle * 0.0174533f);
		PSCB.g_sunDirection.y = 0;
		PSCB.g_sunDirection.z = sinf(m_pState->fSunAngle * 0.0174533f);
		PSCB.g_useDynamicSkyTexture = m_pState->bUseDynamicSky ? 1.0f : 0.0f;
		m_hCommandList->writeBuffer(m_hPSCB, &PSCB, sizeof(PSCB));

		nvrhi::DrawArguments da;
		da.vertexCount = 4;
		da.startIndexLocation = 0;
		da.startVertexLocation = 0;

		m_drawState.indexBuffer.buffer = m_hUpdateSkyIndexBuffer;
		m_drawState.indexBuffer.format = nvrhi::Format::R32_UINT;
		m_drawState.indexBuffer.offset = 0;
		m_drawState.vertexBuffers.resize(1);
		m_drawState.vertexBuffers[0].buffer = m_hUpdateSkyVertexBuffer;
		m_drawState.vertexBuffers[0].slot = 0;
		m_drawState.vertexBuffers[0].offset = 0;
		m_drawState.pipeline = m_hUpdateSkyPSO;
		m_drawState.bindings = ( m_hUpdateSkyBindingSet ).sets();
		m_drawState.framebuffer = m_hUpdateSkyFB;

		// Rendering to mip #0
		m_hCommandList->setGraphicsState(m_drawState);
		m_hCommandList->drawIndexed(da);

		// Generatin mipmap chain
		GenerateMipmaps();

		m_hCommandList->setTextureState(m_hStaticSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setTextureState(m_pState->hDynamicSkyDomeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();
		m_hCommandList->setBufferState(m_hPSCB, nvrhi::ResourceStates::ConstantBuffer);
		m_hCommandList->commitBarriers();
		m_hCommandList->close();
		m_pDevice->executeCommandList(m_hCommandList);
	}
}

void SkyRenderer::GenerateMipmaps()
{
	nvrhi::DrawArguments da;
	da.vertexCount = 4;
	da.startIndexLocation = 0;
	da.startVertexLocation = 0;

	m_drawState.indexBuffer.buffer = m_hUpdateSkyIndexBuffer;
	m_drawState.indexBuffer.format = nvrhi::Format::R32_UINT;
	m_drawState.indexBuffer.offset = 0;
	m_drawState.vertexBuffers.resize(1);
	m_drawState.vertexBuffers[0].buffer = m_hUpdateSkyVertexBuffer;
	m_drawState.vertexBuffers[0].slot = 0;
	m_drawState.vertexBuffers[0].offset = 0;

	int source_mip_level_size = SKYDOME_TEXTURE_SIZE;
	for (int mip_level = 0; mip_level < SKYDOME_TEXTURE_MIPMAPCOUNT - 1; mip_level++)
	{
		m_drawState.viewport.viewports.resize(1);
		m_drawState.viewport.viewports[0] = nvrhi::Viewport(float(source_mip_level_size / 2), float(source_mip_level_size / 2));
		m_drawState.viewport.scissorRects.resize(1);
		m_drawState.viewport.scissorRects[0] = nvrhi::Rect(source_mip_level_size / 2, source_mip_level_size / 2);

		m_drawState.pipeline = m_hMipGenPSO[mip_level];
		m_drawState.framebuffer = m_hMipGenFB[mip_level];
		m_drawState.bindings = ( m_hMipGenBindingSet[mip_level] ).sets();

		m_hCommandList->setGraphicsState(m_drawState);
		m_hCommandList->drawIndexed(da);

		source_mip_level_size /= 2;
	}
}
