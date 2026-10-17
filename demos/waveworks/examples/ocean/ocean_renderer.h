/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef _OCEAN_RENDERER_H
#define _OCEAN_RENDERER_H

#include <waveworks/GFSDK_WaveWorks.h>
#include <donut/app/DeviceManager.h>
#include "graphics_bindings.h"

#include "./main_state.h"



class OceanRenderer: public donut::app::IRenderPass
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

	OceanRenderer(donut::app::DeviceManager *devManager, MainState *pMainState)
        : donut::app::IRenderPass(devManager)
	{ 
		m_pState = pMainState;
		m_pDevice = devManager->GetDevice();
		m_backend = devManager->GetGraphicsAPI();
	}

	// Additional funcs
	bool Init();
	void Shutdown();
	void RebindOceanResources();
	void SetBeachSceneTargets(nvrhi::ITexture* opaqueScene, nvrhi::ITexture* linearDepth);
	void SetBeachBathymetry(nvrhi::ITexture* bathymetry, gfsdk_float2 origin,
		gfsdk_float2 extent);
	void SetBeachPassTimerQueries(nvrhi::ITimerQuery* water, nvrhi::ITimerQuery* foam);
	void ResetCameraView();
	void UpdateMarkers();
	void RenderMarkers(nvrhi::FramebufferHandle hFramebuffer);


private:

	void updateCameraProjection();
	nvrhi::FramebufferHandle getFramebuffer(nvrhi::TextureHandle renderTarget, uint32_t swapChainIndex);

	MainState* m_pState = nullptr;

	nvrhi::IDevice* m_pDevice = nullptr;
	nvrhi::GraphicsAPI m_backend = nvrhi::GraphicsAPI::D3D11;
	nvrhi::CommandListHandle m_hCommandList;

	// 3D objects

	nvrhi::TextureHandle m_hFoamIntensityTexture;
	nvrhi::TextureHandle m_hFoamBubblesTexture;
	nvrhi::TextureHandle m_hWindGustsTexture;
	nvrhi::TextureHandle m_hWindWavesDisplacementsTextureArray;
	nvrhi::TextureHandle m_hWindWavesGradientsTextureArray;
	nvrhi::TextureHandle m_hWindWavesMomentsTextureArray;

	nvrhi::TextureHandle m_hLocalWavesDisplacementsTexture;
	nvrhi::TextureHandle m_hLocalWavesGradientsTexture;
	nvrhi::TextureHandle m_hBeachOpaqueSceneTexture;
	nvrhi::TextureHandle m_hBeachLinearDepthTexture;
	nvrhi::TextureHandle m_hBeachBathymetryTexture;
	gfsdk_float2 m_beachBathymetryOrigin = { 0.f, 0.f };
	gfsdk_float2 m_beachBathymetryExtent = { 0.f, 0.f };


	nvrhi::SamplerHandle m_hBilinearSampler;
	nvrhi::SamplerHandle m_hBilinearClampToEdgeSampler;
	nvrhi::SamplerHandle m_hAnisotropicSampler;

	nvrhi::InputLayoutHandle m_hOceanSurfaceLayout;

	nvrhi::BufferHandle m_hOceanSurfaceVB;
	nvrhi::BufferHandle m_hOceanSurfaceIB;

	nvrhi::BufferHandle m_hOceanSurfacePerInstanceCB;

	nvrhi::ShaderHandle m_hOceanSurfaceVS;
	nvrhi::ShaderHandle m_hOceanSurfaceHS;
	nvrhi::ShaderHandle m_hOceanSurfaceDS;
	nvrhi::ShaderHandle m_hOceanSurfacePS;
	nvrhi::ShaderHandle m_hBeachFoamPS;

	nvrhi::BufferHandle m_hOceanSurfaceVSHSDSCB;
	nvrhi::BufferHandle m_hOceanSurfacePSCB;

	nvrhi::GraphicsPipelineHandle m_hOceanSurfaceSolidPSO;
	nvrhi::GraphicsPipelineHandle m_hOceanSurfaceWireframePSO;
	nvrhi::GraphicsPipelineHandle m_hBeachFoamPSO;
	waveworks::graphics::GraphicsBindingSets m_hOceanSurfaceBindings;
	waveworks::graphics::GraphicsBindingLayouts m_hOceanSurfaceBindingLayout;
	std::vector<nvrhi::FramebufferHandle> m_hFBs; // one entry per swap chain back buffer
	nvrhi::FramebufferHandle m_hInitTimeFB;	      // Framebuffer required at init time to create PSO
	nvrhi::TextureHandle m_hInitTimeRenderTargetTexture; // Render target texture required at init time to create PSO
	nvrhi::GraphicsState m_drawState;

	nvrhi::ShaderHandle m_hMarkerVS;
	nvrhi::ShaderHandle m_hMarkerPS;
	nvrhi::InputLayoutHandle m_hMarkerLayout;
	nvrhi::BufferHandle m_hMarkerVB;
	nvrhi::BufferHandle m_hMarkerIB;
	nvrhi::BufferHandle m_hMarkerInstanceBuffer;
	nvrhi::BufferHandle m_hMarkerInstanceUploadBuffers[3];
	int32_t	m_markerInstanceUploadBufferNumber;
	nvrhi::GraphicsPipelineHandle m_hMarkerPSO;
	waveworks::graphics::GraphicsBindingSets m_hMarkerBindings;
	waveworks::graphics::GraphicsBindingLayouts m_hMarkerBindingLayout;
	nvrhi::BufferHandle m_hMarkerVSCB;



	gfsdk_float2* m_pReadbackPositions = nullptr;
	gfsdk_float4* m_pReadbackResults = nullptr;


	gfsdk_float4x4 m_matView;
	gfsdk_float4x4 m_matProj;
	gfsdk_float4x4 m_matMVP;

	uint32_t m_backBufferWidth = 1280;
	uint32_t m_backBufferHeight = 720;
	uint32_t m_sampleCount = 1;
	bool m_needToRecreatePSOs = false;
	bool m_needToRebindBeachTargets = false;
	nvrhi::ITimerQuery* m_waterTimerQuery = nullptr;
	nvrhi::ITimerQuery* m_foamTimerQuery = nullptr;
};

#endif	// _OCEAN_RENDERER_H
