/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef _SKY_RENDERER_H
#define _SKY_RENDERER_H

#include <waveworks/GFSDK_WaveWorks.h>
#include <donut/app/DeviceManager.h>
#include "graphics_bindings.h"

#include "./main_state.h"

#define SKYDOME_SLICES_COUNT 50
#define SKYDOME_TEXTURE_SIZE 2048
#define SKYDOME_TEXTURE_MIPMAPCOUNT 12

class SkyRenderer: public donut::app::IRenderPass
{
    NVRHI_INHERIT_INTERFACE_TABLE()
public:
	// IRenderPass interface
	virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
	virtual bool MousePosUpdate(double xpos, double ypos) override;
	virtual bool MouseButtonUpdate(int button, int action, int mods) override;
	bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    void Animate(float elapsedTimeSeconds) override;
	virtual void Render(nvrhi::IFramebuffer* framebuffer) override;
	virtual void BackBufferResizing() override;
	virtual void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) override;

	SkyRenderer(donut::app::DeviceManager *devManager, MainState *pMainState)
        : donut::app::IRenderPass(devManager)
	{ 
		m_pState = pMainState;
		m_pDevice = devManager->GetDevice();
		m_backend = devManager->GetGraphicsAPI();
	}

	// Additional funcs
	bool Init();
	void GenerateMipmaps();
	void Shutdown();
	//void UpdateSkyTexture();

private:
	nvrhi::FramebufferHandle getFramebuffer(nvrhi::TextureHandle renderTarget, uint32_t swapChainIndex);

	MainState* m_pState;

	nvrhi::IDevice* m_pDevice;
	nvrhi::GraphicsAPI m_backend = nvrhi::GraphicsAPI::D3D11;
	nvrhi::CommandListHandle m_hCommandList;

	// 3D objects
	nvrhi::InputLayoutHandle m_hRenderSkyLayout;
	nvrhi::BufferHandle m_hRenderSkyVertexBuffer;
	nvrhi::BufferHandle m_hRenderSkyIndexBuffer;
	
	nvrhi::TextureHandle m_hStaticSkyDomeTexture;

	nvrhi::SamplerHandle m_hBilinearSampler;
	nvrhi::SamplerHandle m_hAnisotropicSampler;

	nvrhi::ShaderHandle m_hRenderSkyVS;
	nvrhi::ShaderHandle m_hRenderSkyPS;
	nvrhi::BufferHandle m_hRenderSkyVSCB;
	nvrhi::BufferHandle m_hPSCB;

	nvrhi::GraphicsPipelineHandle m_hRenderSkyPSO;
	std::vector<nvrhi::FramebufferHandle> m_hRenderSkyFBs; // one entry per swap chain back buffer
	nvrhi::FramebufferHandle m_hInitTimeFB;	      // Framebuffer required at init time to create PSO
	nvrhi::TextureHandle m_hInitTimeRenderTargetTexture; // Render target texture required at init time to create PSO
	waveworks::graphics::GraphicsBindingSets m_hRenderSkyBindingSet;
	waveworks::graphics::GraphicsBindingLayouts m_hRenderSkyBindingLayout;
	nvrhi::GraphicsState m_drawState;

	nvrhi::InputLayoutHandle m_hUpdateSkyLayout;
	nvrhi::ShaderHandle m_hFullscreenVS;
	nvrhi::ShaderHandle m_hUpdateSkyPS;
	nvrhi::GraphicsPipelineHandle m_hUpdateSkyPSO;
	nvrhi::FramebufferHandle m_hUpdateSkyFB;
	waveworks::graphics::GraphicsBindingSets m_hUpdateSkyBindingSet;
	waveworks::graphics::GraphicsBindingLayouts m_hUpdateSkyBindingLayout;
	nvrhi::BufferHandle m_hUpdateSkyVertexBuffer;
	nvrhi::BufferHandle m_hUpdateSkyIndexBuffer;

	nvrhi::GraphicsPipelineHandle m_hMipGenPSO[SKYDOME_TEXTURE_MIPMAPCOUNT - 1];
	nvrhi::FramebufferHandle m_hMipGenFB[SKYDOME_TEXTURE_MIPMAPCOUNT - 1];
	waveworks::graphics::GraphicsBindingSets m_hMipGenBindingSet[SKYDOME_TEXTURE_MIPMAPCOUNT - 1];
	waveworks::graphics::GraphicsBindingLayouts m_hMipGenBindingLayout;
	nvrhi::ShaderHandle m_hMipGenPS;

	gfsdk_float4x4 m_matView;
	gfsdk_float4x4 m_matProj;
	gfsdk_float4x4 m_matMVP;

	uint32_t m_backBufferWidth = 1280;
	uint32_t m_backBufferHeight = 720;
	uint32_t m_sampleCount = 1;
	bool m_needToRecreatePSOs = false;
};

#endif	// _OCEAN_RENDERER_H
