/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
#ifndef _UI_RENDERER_H
#define _UI_RENDERER_H

#include <donut/app/DeviceManager.h>
#include <GLFW/glfw3.h>
#include "graphics_bindings.h"
#include <imgui.h>

#include "./main_state.h"

class UIRenderer : public donut::app::IRenderPass
{
    NVRHI_INHERIT_INTERFACE_TABLE()
public:
	// IRenderPass interface
    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    bool KeyboardCharInput(unsigned int character, int mods) override;
    virtual bool MousePosUpdate(double xpos, double ypos) override;
    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override;
    virtual bool MouseButtonUpdate(int button, int action, int mods) override;
    bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    void Animate(float elapsedTimeSeconds) override;
    virtual void Render(nvrhi::IFramebuffer* framebuffer) override;
	virtual void BackBufferResizing() override;
	virtual void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) override;

	UIRenderer(donut::app::DeviceManager *devManager, MainState *pMainState)
        : donut::app::IRenderPass(devManager)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		m_pState = pMainState;
		m_pDevice = devManager->GetDevice();
		m_pDeviceManager = devManager;
		m_backend = devManager->GetGraphicsAPI();
	}

    ~UIRenderer() override { ImGui::DestroyContext(); }

	// Additional funcs
	bool Init();

private:
    void BuildUI();
	void createOrRecreateBufferIfNeeded(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, const bool indexBuffer);
	void updateGeometry();
	nvrhi::FramebufferHandle getFramebuffer(nvrhi::TextureHandle renderTarget, uint32_t swapChainIndex);

	std::vector<ImDrawVert> m_IMVertexBuffer;
	std::vector<ImDrawIdx> m_IMIndexBuffer;

	MainState* m_pState;

	donut::app::DeviceManager *m_pDeviceManager = nullptr;
	nvrhi::IDevice* m_pDevice = nullptr;
	nvrhi::GraphicsAPI m_backend = nvrhi::GraphicsAPI::D3D11;
	nvrhi::CommandListHandle m_hCommandList = nullptr;

	// 3D objects

	nvrhi::ShaderHandle m_hVS;
	nvrhi::ShaderHandle m_hPS;
	nvrhi::InputLayoutHandle m_hAttribLayout;
	nvrhi::BufferHandle m_hCB;
	nvrhi::TextureHandle m_hFontTexture;
	nvrhi::SamplerHandle m_hFontSampler;
	waveworks::graphics::GraphicsBindingLayouts m_hBindingLayout;
	waveworks::graphics::GraphicsBindingSets m_hBindingSet;
	std::vector<nvrhi::FramebufferHandle> m_hFBs; // one entry per swap chain back buffer
	nvrhi::TextureHandle m_hInitTimeRenderTargetTexture; // Render target texture required at init time to create PSO
	nvrhi::FramebufferHandle m_hInitTimeFB;				 // Framebuffer required at init time to create PSO
	nvrhi::GraphicsPipelineHandle m_hPSO;
	nvrhi::BufferHandle m_hVB;
	nvrhi::BufferHandle m_hIB;

	uint32_t m_backBufferWidth = 1280;
	uint32_t m_backBufferHeight = 720;
	uint32_t m_sampleCount = 1;
	bool m_needToRecreatePSOs = false;
	uint32_t m_lastDisplaySizeX = 0;
	uint32_t m_lastDisplaySizeY = 0;
};

#endif //_UI_RENDERER_H
