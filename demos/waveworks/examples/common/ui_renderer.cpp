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
#include "./ui_renderer.h"
#include "asset_utils.h"

#include <cstdlib>
#include <cmath>
#include <imgui_internal.h>

#include <cstddef>

#include <shaders/bytecode/ImGUIVS.spirv.h>
#include <shaders/bytecode/ImGUIPS.spirv.h>
#include <shaders/bytecode/ImGUIVS.dxbc.h>
#include <shaders/bytecode/ImGUIPS.dxbc.h>

namespace
{
void BeginDisabledControls(bool disabled)
{
	if (!disabled)
		return;
	ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
}

void EndDisabledControls(bool disabled)
{
	if (!disabled)
		return;
	ImGui::PopStyleVar();
	ImGui::PopItemFlag();
}
}


struct IMGUI_VS_CBUFFER
{
	float        mvp[4][4];
};

uint32_t IndexToGridSize(uint32_t DetailLevelIndex)
{
	switch (DetailLevelIndex)
	{
		case 0: return 128;
		case 1: return 256;
		case 2: return 512;
		case 3: return 1024;
		case 4: return 2048;
		default: return 128;
	}
}

uint32_t GridSizeToIndex(uint32_t GridSize)
{
	switch (GridSize)
	{
		case 128: return 0;
		case 256: return 1;
		case 512: return 2;
		case 1024: return 3;
		case 2048: return 4;
		default: return 0;
	}
}

void UIRenderer::BuildUI(void)
{
    const auto& io = ImGui::GetIO();
	float ColumnOffset = 250.0f;
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
	ImGui::SetNextWindowSizeConstraints(ImVec2(0,0), ImVec2(500, io.DisplaySize.y - 20));

	ImGui::Begin("WaveWorks ", 0, ImGuiWindowFlags_AlwaysAutoResize);

	if (ImGui::TreeNode("General settings"))
	{

		ImGui::Checkbox("Simulate water", &(m_pState->bSimulateWater));
		ImGui::Checkbox("Render water", &(m_pState->bRenderWater));
		ImGui::Checkbox("Use wireframe", &(m_pState->bRenderWireframe));
		ImGui::Checkbox("Use microfacet Fresnel", &(m_pState->bUseMicrofacetFresnel));
		ImGui::Checkbox("Use microfacet Specular", &(m_pState->bUseMicrofacetSpecular));
		ImGui::Checkbox("Use microfacet Reflection", &(m_pState->bUseMicrofacetReflection));
		ImGui::NewLine();
		BeginDisabledControls(!m_pState->bUpdateWindWaves);
		if(ImGui::SliderFloat("UV warping amplitude", &(m_pState->OceanWindSimulationParameters.uv_warping_amplitude), 0.0f, 0.1f, "%.2f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
		if(ImGui::SliderFloat("UV warping frequency", &(m_pState->OceanWindSimulationParameters.uv_warping_frequency), 1.0f, 3.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
		EndDisabledControls(!m_pState->bUpdateWindWaves);
		ImGui::Checkbox("Show cascades", &(m_pState->bShowCascades));
		ImGui::NewLine();
		if (!m_pState->bBeachScene &&
			ImGui::Checkbox("Use atmosphere model", &(m_pState->bUseDynamicSky)))
		{
			m_pState->bNeedToUpdateSky = true;
		}
		
		if (m_pState->bBeachScene || m_pState->bUseDynamicSky)
		{
			if (ImGui::SliderFloat("Sun position", &(m_pState->fSunAngle), 1.0f, 90.0f, "%.0f"))
			{
				m_pState->bNeedToUpdateSky = true;
			}
		}
		else
		{
			m_pState->fSunAngle = 45.0;
		}

		ImGui::SliderFloat("Sun intensity", &(m_pState->fSunIntensity), 0.0f, 2.0f, "%.2f");
		if (m_pState->bBeachScene && ImGui::SliderFloat("Sun azimuth", &(m_pState->fBeachSunAzimuth), -180.f, 180.f, "%.0f"))
			m_pState->bNeedToUpdateSky = true;
		if (m_pState->bBeachScene)
		{
			float roughness = std::sqrt(std::sqrt(m_pState->fBeckmannRoughness));
			if (ImGui::SliderFloat("Water GGX roughness", &roughness, 0.03f, 0.6f, "%.3f"))
				m_pState->fBeckmannRoughness = roughness * roughness * roughness * roughness;
		}
		else
			ImGui::SliderFloat("Beckmann roughness", &(m_pState->fBeckmannRoughness), 0.00001f, 0.1f, "%.5f", ImGuiSliderFlags_Logarithmic);
		ImGui::NewLine();
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Wind waves"))
	{
		if (ImGui::Checkbox("Update wind waves", &m_pState->bUpdateWindWaves))
		{
			if (m_pState->bUpdateWindWaves)
				m_pState->bForceKick = true;
			else
				m_pState->bNeedToUpdateWindWavesSimulationProperties = false;
		}
		BeginDisabledControls(!m_pState->bUpdateWindWaves);
		if (ImGui::TreeNode("Simulation settings"))
		{
			if (ImGui::SliderFloat("Simulation period (m)", &(m_pState->OceanWindSimulationSettings.simulation_period), 100.0f, 10000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			if (ImGui::Checkbox("Use Beaufort scale", &(m_pState->OceanWindSimulationSettings.use_Beaufort_scale))) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;

			if (ImGui::Checkbox("Enable CPU driven displacement readbacks", &(m_pState->OceanWindSimulationSettings.enable_CPU_driven_displacement_calculation))) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;

			if (ImGui::Checkbox("Enable GPU driven displacement readbacks", &(m_pState->OceanWindSimulationSettings.enable_GPU_driven_displacement_calculation))) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;

			char*  ReadbackUsageStrings[3] = { "None", "CPU", "Compute" };
			ImGui::Combo("Use displacement readbacks", &m_pState->iReadbackUsage, ReadbackUsageStrings, 3);


			if (ImGui::Checkbox("Use CPU timers", &(m_pState->OceanWindSimulationSettings.enable_CPU_timers)))
			{
				m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			}

			if (ImGui::Checkbox("Use GPU timers", &(m_pState->OceanWindSimulationSettings.enable_GPU_timers)))
			{
				m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			}

			if (m_pState->iReadbackUsage > 0)
			{
				ImGui::Checkbox("Render readback markers", &(m_pState->bRenderMarkers));
				ImGui::SliderInt("Refinement steps", &(m_pState->iRefinementSteps), 1, 20, "%.0f");
				ImGui::SliderFloat("ReadbackCoord", &(m_pState->fReadbackCoord), 0, 30, "%.1f");
			}
			else
			{
				m_pState->bRenderMarkers = false;
			}

			char*  SimulationAPIStrings[2] = { "CPU", "Compute" };
			int SimulationAPIIndex = (int)(m_pState->OceanWindSimulationSettings.simulation_api);
			if (ImGui::Combo("Simulation API", &SimulationAPIIndex, SimulationAPIStrings, 2))
			{
				m_pState->OceanWindSimulationSettings.simulation_api = (GFSDK_WaveWorks_Simulation_API)SimulationAPIIndex;
				m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			}

			char*  DetailLevelStrings[3] = { "Normal", "High", "Extreme" };
			int DetailLevelIndex = (int)(m_pState->OceanWindSimulationSettings.detail_level);
			if (ImGui::Combo("Simulation Quality", &DetailLevelIndex, DetailLevelStrings, 3))
			{
				m_pState->OceanWindSimulationSettings.detail_level = (GFSDK_WaveWorks_Simulation_DetailLevel)DetailLevelIndex;
				m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			}

			ImGui::NewLine();
			ImGui::TreePop();

		}

		if (ImGui::TreeNode("Simulation parameters"))
		{
			if (ImGui::TreeNode("Local wind"))
			{
				if (ImGui::SliderFloat("Wind direction (degrees)", &(m_pState->fBaseWindDirection), -180.0f, 180.0f, "%.0f"))
				{
					m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
					m_pState->OceanWindSimulationParameters.base_wind_direction.x = cosf(m_pState->fBaseWindDirection * 0.0174532925f); // degrees to radians
					m_pState->OceanWindSimulationParameters.base_wind_direction.y = sinf(m_pState->fBaseWindDirection * 0.0174532925f); // degrees to radians
				}

				if (m_pState->OceanWindSimulationSettings.use_Beaufort_scale)
				{
					if (m_pState->OceanWindSimulationParameters.base_wind_speed > 12.0f) m_pState->OceanWindSimulationParameters.base_wind_speed = 12.0f;
					if (ImGui::SliderFloat("Wind speed (Beaufort)", &(m_pState->OceanWindSimulationParameters.base_wind_speed), 0, 12.0f, "%.1f"))
					{
						m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
						m_pState->fMaxTotalDisplacement = 0;
						m_pState->fMinTotalDisplacement = 0;
					}
				}
				else
				{
					if (ImGui::SliderFloat("Wind speed (m/sec)", &(m_pState->OceanWindSimulationParameters.base_wind_speed), 0, 50.0f, "%.1f"))
					{
						m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
						m_pState->fMaxTotalDisplacement = 0;
						m_pState->fMinTotalDisplacement = 0;
					}
					if (ImGui::SliderFloat("Wind distance (km)", &(m_pState->OceanWindSimulationParameters.base_wind_distance), 1.0f, 1000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
					if (ImGui::SliderFloat("Spectrum peaking", &(m_pState->OceanWindSimulationParameters.base_spectrum_peaking), 0.1f, 10.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
					if (ImGui::SliderFloat("Amplitude", &(m_pState->OceanWindSimulationParameters.base_amplitude_multiplier), 0, 2.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				}
				if (ImGui::SliderFloat("Wind dependency", &(m_pState->OceanWindSimulationParameters.base_wind_dependency), 0, 1.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Small waves lengths (m)", &(m_pState->OceanWindSimulationParameters.base_small_waves_cutoff_length), 0, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Small waves cutoff", &(m_pState->OceanWindSimulationParameters.base_small_waves_cutoff_power), 0, 1.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Swell"))
			{
				if (ImGui::SliderFloat("Wind direction (degrees)", &(m_pState->fSwellWindDirection), -180.0f, 180.0f, "%.0f"))
				{
					m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
					m_pState->OceanWindSimulationParameters.swell_wind_direction.x = cosf(m_pState->fSwellWindDirection * 0.0174532925f); // degrees to radians
					m_pState->OceanWindSimulationParameters.swell_wind_direction.y = sinf(m_pState->fSwellWindDirection * 0.0174532925f); // degrees to radians
				}
				if (ImGui::SliderFloat("Wind speed (m/sec)", &(m_pState->OceanWindSimulationParameters.swell_wind_speed), 0, 50.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Wind distance (km)", &(m_pState->OceanWindSimulationParameters.swell_wind_distance), 1.0f, 1000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Spectrum peaking", &(m_pState->OceanWindSimulationParameters.swell_spectrum_peaking), 0.1f, 10.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Amplitude", &(m_pState->OceanWindSimulationParameters.swell_amplitude_multiplier), 0, 2.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Wind dependency", &(m_pState->OceanWindSimulationParameters.swell_wind_dependency), 0, 1.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Small waves lengths (m)", &(m_pState->OceanWindSimulationParameters.swell_small_waves_cutoff_length), 0, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Small waves cutoff", &(m_pState->OceanWindSimulationParameters.swell_small_waves_cutoff_power), 0, 1.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				ImGui::TreePop();
			}


			ImGui::Text("Common parameters:");
			if (!m_pState->OceanWindSimulationSettings.use_Beaufort_scale)
			{

				if (ImGui::SliderFloat("Choppiness", &(m_pState->OceanWindSimulationParameters.lateral_multiplier), 0, 4.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			}
			if (ImGui::SliderFloat("Time scale", &(m_pState->OceanWindSimulationParameters.time_scale), 0, 5.0f, "%.1f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;

			if (!m_pState->OceanWindSimulationSettings.use_Beaufort_scale)
			{
				ImGui::Text("Foam generation parameters:");
				if (ImGui::SliderFloat("Whitecaps threshold", &(m_pState->OceanWindSimulationParameters.foam_whitecaps_threshold), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Generation threshold", &(m_pState->OceanWindSimulationParameters.foam_generation_threshold), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Generation amount", &(m_pState->OceanWindSimulationParameters.foam_generation_amount), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Dissipation speed", &(m_pState->OceanWindSimulationParameters.foam_dissipation_speed), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
				if (ImGui::SliderFloat("Falloff speed", &(m_pState->OceanWindSimulationParameters.foam_falloff_speed), 0.95f, 0.99f, "%.3f")) m_pState->bNeedToUpdateWindWavesSimulationProperties = true;
			}
			ImGui::NewLine();
			ImGui::TreePop();
		}

		EndDisabledControls(!m_pState->bUpdateWindWaves);
		ImGui::Checkbox("Show statistics", &(m_pState->bShowWindWavesSimulationStatistics));
		if (m_pState->bShowWindWavesSimulationStatistics)
		{
			ImGui::Text("CPU_main_thread_wait_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanWindSimulationStatsFiltered.CPU_main_thread_wait_time);
			ImGui::Text("CPU_threads_total_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanWindSimulationStatsFiltered.CPU_threads_total_time);
			ImGui::Text("GPU_simulation_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanWindSimulationStatsFiltered.GPU_simulation_time);
			ImGui::Text("GPU_update_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanWindSimulationStatsFiltered.GPU_update_time);
			ImGui::Text("GPU_total_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanWindSimulationStatsFiltered.GPU_total_time);
			ImGui::Text("Lask simulation KickID"); ImGui::SameLine(ColumnOffset); ImGui::Text("%i", m_pState->iLastWindSimulationKickID);
			ImGui::Text("Lask readback KickID"); ImGui::SameLine(ColumnOffset); ImGui::Text("%i", m_pState->iLastWindSimulationReadbackKickID);
			ImGui::Text("Readback latency"); ImGui::SameLine(ColumnOffset); m_pState->iWindSimulationReadbackLatency != -1 ? ImGui::Text("%i", m_pState->iWindSimulationReadbackLatency) : ImGui::Text("N/A");
			ImGui::Text("Rendering latency"); ImGui::SameLine(ColumnOffset); ImGui::Text("%i", m_pState->iWindSimulationRenderLatency);
			float fmaxdisp = m_pState->fMaxDisplacement - m_pState->fMinDisplacement;
			float fmaxtotaldisp = m_pState->fMaxTotalDisplacement - m_pState->fMinTotalDisplacement;
			ImGui::Text("Min/Max displ, max wave height"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.2f    %3.2f    %3.2f", m_pState->fMinDisplacement, m_pState->fMaxDisplacement, fmaxdisp); ImGui::NewLine();
			ImGui::Text("Max wave height total"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.2f", fmaxtotaldisp); ImGui::NewLine();
			ImGui::Text("Significant Wave Height"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.2f", m_pState->fSWH); ImGui::NewLine();
			float p = GFSDK_WaveWorks_Wind_Waves_Simulation_GetConservativeMaxDisplacementEstimate(m_pState->hOceanWindSimulation);
			ImGui::Text("Conservative displacement estimation"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.2f", GFSDK_WaveWorks_Wind_Waves_Simulation_GetConservativeMaxDisplacementEstimate(m_pState->hOceanWindSimulation)); ImGui::NewLine();
		}
		ImGui::NewLine();
		ImGui::TreePop();
	}

	// Local waves simulation
	if (ImGui::TreeNode("Local waves"))
	{
		if (ImGui::Checkbox("Update local waves", &m_pState->bUpdateLocalWaves))
		{
			if (m_pState->bUpdateLocalWaves)
				m_pState->bForceKick = true;
			else
			{
				m_pState->bNeedToUpdateLocalWavesSimulationProperties = false;
				m_pState->bNeedToResetLocalWavesSimulation = false;
			}
		}
		BeginDisabledControls(!m_pState->bUpdateLocalWaves);
		if (ImGui::TreeNode("Simulation settings"))
		{
			if (ImGui::SliderFloat("Simulation domain size (m)", &(m_pState->OceanLocalSimulationSettings.simulation_domain_worldspace_size), 10.0f, 1000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;

			if (ImGui::Checkbox("Enable CPU driven displacement readbacks", &(m_pState->OceanLocalSimulationSettings.enable_CPU_driven_displacement_calculation))) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;

			if (ImGui::Checkbox("Enable GPU driven displacement readbacks", &(m_pState->OceanLocalSimulationSettings.enable_GPU_driven_displacement_calculation))) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			
			char*  ReadbackUsageStrings[3] = { "None", "CPU", "Compute" };
			ImGui::Combo("Use displacement readbacks", &m_pState->iReadbackUsage, ReadbackUsageStrings, 3);

			if (ImGui::Checkbox("Use CPU timers", &(m_pState->OceanLocalSimulationSettings.enable_CPU_timers)))
			{
				m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			}

			if (ImGui::Checkbox("Use GPU timers", &(m_pState->OceanLocalSimulationSettings.enable_GPU_timers)))
			{
				m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			}

			char*  SimulationAPIStrings[2] = { "CPU", "Compute" };
			int SimulationAPIIndex = (int)(m_pState->OceanLocalSimulationSettings.simulation_api);
			if (ImGui::Combo("Simulation API", &SimulationAPIIndex, SimulationAPIStrings, 2))
			{
				m_pState->OceanLocalSimulationSettings.simulation_api = (GFSDK_WaveWorks_Simulation_API)SimulationAPIIndex;
				m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			}

			char*  DetailLevelStrings[5] = { "128", "256", "512", "1024", "2048" };
			int DetailLevelIndex = (int)(GridSizeToIndex(m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size));
			if (ImGui::Combo("Simulation grid size", &DetailLevelIndex, DetailLevelStrings, 5))
			{
				m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size = IndexToGridSize(DetailLevelIndex);
				m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			}
			if (ImGui::Button("Reset simulation")) m_pState->bNeedToResetLocalWavesSimulation = true;
			if (ImGui::Button("Add disturbances"))
			{
				gfsdk_float4 d;
				for(int i=-15; i< 10; i++)
					for (int j = -15; j < 10; j++)
					{
						d.x = (float)(i + m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size / 2);
						d.y = (float)(j + m_pState->OceanLocalSimulationSettings.simulation_domain_grid_size / 2);
						d.z = 1.0f;
						d.w = 0;
						GFSDK_WaveWorks_Local_Waves_Simulation_AddDisturbances(m_pState->hOceanLocalSimulation, &d, 1);
					}
			}
			ImGui::Checkbox("Rain effect", &(m_pState->bRain));
			ImGui::SliderFloat("Rain Drop Size", &(m_pState->fRainDropSize), 0.01f, 0.2f, "%.2f");
			ImGui::Checkbox("Simulate \"boat\" ", &(m_pState->bBoat));

			ImGui::NewLine();
			ImGui::TreePop();

		}

		if (ImGui::TreeNode("Simulation parameters"))
		{
			if (ImGui::SliderFloat("Amplitude", &(m_pState->OceanLocalSimulationParameters.amplitude_multiplier), 0, 2.0f, "%.1f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			if (ImGui::SliderFloat("Choppiness", &(m_pState->OceanLocalSimulationParameters.lateral_multiplier), 0, 2.0f, "%.1f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;

			ImGui::Text("Foam generation parameters:");
			if (ImGui::SliderFloat("Whitecaps threshold", &(m_pState->OceanLocalSimulationParameters.foam_whitecaps_threshold), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			if (ImGui::SliderFloat("Generation threshold", &(m_pState->OceanLocalSimulationParameters.foam_generation_threshold), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			if (ImGui::SliderFloat("Generation amount", &(m_pState->OceanLocalSimulationParameters.foam_generation_amount), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			if (ImGui::SliderFloat("Dissipation speed", &(m_pState->OceanLocalSimulationParameters.foam_dissipation_speed), 0.f, 1.0f, "%.2f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;
			if (ImGui::SliderFloat("Falloff speed", &(m_pState->OceanLocalSimulationParameters.foam_falloff_speed), 0.5f, 0.99f, "%.3f")) m_pState->bNeedToUpdateLocalWavesSimulationProperties = true;

			ImGui::NewLine();
			ImGui::TreePop();
		}
		EndDisabledControls(!m_pState->bUpdateLocalWaves);
		ImGui::Checkbox("Show statistics", &(m_pState->bShowLocalWavesSimulationStatistics));
		if (m_pState->bShowLocalWavesSimulationStatistics)
		{
			ImGui::NewLine();
			ImGui::Text("CPU_main_thread_wait_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanLocalSimulationStatsFiltered.CPU_main_thread_wait_time);
			ImGui::Text("CPU_threads_total_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanLocalSimulationStatsFiltered.CPU_threads_total_time);
			ImGui::Text("GPU_simulation_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanLocalSimulationStatsFiltered.GPU_simulation_time);
			ImGui::Text("GPU_update_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanLocalSimulationStatsFiltered.GPU_update_time);
			ImGui::Text("GPU_total_time"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.6f msec", m_pState->OceanLocalSimulationStatsFiltered.GPU_total_time);
			ImGui::Text("Lask simulation KickID"); ImGui::SameLine(ColumnOffset); ImGui::Text("%i", m_pState->iLastLocalSimulationKickID);
			ImGui::Text("Lask readback KickID"); ImGui::SameLine(ColumnOffset); ImGui::Text("%i", m_pState->iLastLocalSimulationReadbackKickID);
			ImGui::Text("Readback latency"); ImGui::SameLine(ColumnOffset); m_pState->iLocalSimulationReadbackLatency != -1 ? ImGui::Text("%i", m_pState->iLocalSimulationReadbackLatency) : ImGui::Text("N/A");
			ImGui::Text("Rendering latency"); ImGui::SameLine(ColumnOffset); ImGui::Text("%i", m_pState->iLocalSimulationRenderLatency);
		}
		ImGui::NewLine();
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Quadtree / geometry parameters"))
	{
		int cellCount = (int)m_pState->OceanQuadtreeParameters.cell_count;
		if (ImGui::SliderInt("Cells in single patch", &cellCount, 8, 256, "%d"))
		{
			m_pState->OceanQuadtreeParameters.cell_count = (uint32_t)cellCount;
			m_pState->bNeedToUpdateQuadtreeProperties = true;
		}

		if (ImGui::SliderFloat("Minimum patch size (m)", &(m_pState->OceanQuadtreeParameters.min_patch_length), 1.0f, 100.0f, "%.0f"))
		{
			m_pState->bNeedToUpdateQuadtreeProperties = true;
		}

		if (ImGui::SliderFloat("Maximum edge length (pixels)", &(m_pState->OceanQuadtreeParameters.max_edge_length), 5.0f, 50.0f, "%.0f"))
		{
			m_pState->bNeedToUpdateQuadtreeProperties = true;
		}

		if (ImGui::SliderFloat("Mean sea level (m)", &(m_pState->OceanQuadtreeParameters.mean_sea_level), -50.0f, 50.0f, "%.1f"))
		{
			m_pState->bNeedToUpdateQuadtreeProperties = true;
		}

		if (ImGui::SliderFloat("Geomorphing degree", &(m_pState->OceanQuadtreeParameters.geomorphing_degree), 0.0f, 1.0f, "%.2f"))
		{
			m_pState->bNeedToUpdateQuadtreeProperties = true;
		}

		if (ImGui::Checkbox("Generate diamond pattern", &(m_pState->OceanQuadtreeParameters.generate_diamond_pattern)))
		{
			m_pState->bNeedToUpdateQuadtreeProperties = true;
		}
		
		ImGui::NewLine();
		ImGui::SliderFloat("Static tessellation offset", &(m_pState->fTessellationOffset), 0.0f, 16.0f, "%.0f");
		ImGui::SliderFloat("Dynamic tessellation amount", &(m_pState->fTessellationAmount), 0.1f, 100.0f, "%.2f");

		ImGui::NewLine();
		ImGui::TreePop();
	}
	
	ImGui::NewLine();
	if (m_pState->bBeachScene && ImGui::TreeNode("Beach Scene"))
	{
		ImGui::Text("Terrain: %s (startup: --beach-terrain=survey|cove)",
			m_pState->bBeachCoveTerrain ? "Crescent cove" : "Surveyed coast");
		ImGui::Checkbox("Render beach terrain", &m_pState->bRenderBeachTerrain);
		ImGui::Checkbox("Render beach vegetation", &m_pState->bRenderBeachVegetation);
		ImGui::Checkbox("Terrain / grass shadows", &m_pState->bBeachShadows);
		ImGui::SliderFloat("Sand dielectric specular", &m_pState->fBeachSandSpecular,
			0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Sand normal strength", &m_pState->fBeachSandNormalStrength,
			0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Wet sand coat roughness", &m_pState->fBeachWetSandRoughness,
			0.05f, 1.0f, "%.2f");
		ImGui::Combo("Terrain view", &m_pState->iBeachTerrainDebug,
			"Lit\0Base color\0World normal\0Material coverage\0Sky visibility\0");
		ImGui::Combo("Water view", &m_pState->iBeachWaterDebug,
			"Lit\0Optical path / 20m\0Transmission\0Reflection\0Refraction hit\0PDE depth / 5m\0Normal\0PDE foam\0PDE wetness\0");
		ImGui::Checkbox("Shallow-water PDE surface", &m_pState->bBeachShallowSimulation);
		const bool shallowDisabled = !m_pState->bUpdateLocalWaves;
		if (shallowDisabled) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}
		ImGui::Checkbox("Incoming swell boundary", &m_pState->bBeachSurfForcing);
		if (ImGui::Button("Reset shallow-water PDE")) m_pState->bBeachResetShallow = true;
		if (shallowDisabled) { ImGui::PopStyleVar(); ImGui::PopItemFlag(); }
		ImGui::Text("SWE: %.2f ms CPU | t %.1f s | CFL %.2f | debt %.3f s",
			m_pState->fBeachShallowMilliseconds, m_pState->fBeachShallowTime,
			m_pState->fBeachShallowCfl, m_pState->fBeachShallowDebt);
		ImGui::Checkbox("Shallow water / sediment", &m_pState->bBeachShallowWater);
		ImGui::Checkbox("Shoreline foam", &m_pState->bBeachShoreFoam);
		ImGui::SliderFloat("Shore foam intensity", &m_pState->fBeachShoreFoamIntensity,
			0.0f, 2.5f, "%.2f");
		ImGui::SliderFloat("Aerosol extinction (1/m)", &m_pState->fBeachHaze,
			0.0f, 0.0005f, "%.6f", ImGuiSliderFlags_Logarithmic);
		ImGui::Text("Use the camera reset control for the aerial cove view.");
		ImGui::TreePop();
		ImGui::NewLine();
	}
	ImGui::Text("Renderer: %s", m_pDeviceManager->GetRendererString());
	ImGui::Text("%.3f msec/frame (%.1f FPS)", 1000.f / io.Framerate, io.Framerate);
	ImGui::Text("Camera pos"); ImGui::SameLine(ColumnOffset); ImGui::Text("%3.1f    %3.1f    %3.1f", m_pState->Camera.GetCameraPosition().x, m_pState->Camera.GetCameraPosition().y, m_pState->Camera.GetCameraPosition().z);
	ImGui::Text("Quadtree stats:"); 
	ImGui::Text("Nodes: %i total, %i rendered, time : %3.3f msec", m_pState->OceanQuadtreeStats.num_quadtree_nodes_created, m_pState->OceanQuadtreeStats.num_quadtree_nodes_for_rendering, m_pState->OceanQuadtreeStats.CPU_quadtree_update_time);
	ImGui::Text("Refinement iterations: %i, nodes added: %i", m_pState->OceanQuadtreeStats.num_refinement_iterations, m_pState->OceanQuadtreeStats.num_refinement_nodes_added);

	ImGui::End();
}

bool UIRenderer::Init()
{
    ImGuiIO& io = ImGui::GetIO();
	io.Fonts->AddFontFromFileTTF((GetMediaFolder() / "DroidSans.ttf").generic_string().c_str(), 15.f, nullptr, nullptr);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));


	nvrhi::CommandListParameters cmdListParams;
	m_pDevice->createCommandList(cmdListParams, &m_hCommandList);
	m_hCommandList->open();

	// Loading shaders
	const bool useVk = m_backend == nvrhi::GraphicsAPI::VULKAN;
	const void *VSBinary = useVk ? static_cast<const void *>(g_ImGUIVS_spirv) : g_ImGUIVS_dxbc;
	size_t VSBinarySize = useVk ? sizeof(g_ImGUIVS_spirv) : sizeof(g_ImGUIVS_dxbc);
	const void *PSBinary = useVk ? static_cast<const void *>(g_ImGUIPS_spirv) : g_ImGUIPS_dxbc;
	size_t PSBinarySize = useVk ? sizeof(g_ImGUIPS_spirv) : sizeof(g_ImGUIPS_dxbc);


	// Creating shaders
	m_hVS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), VSBinary, VSBinarySize);
	if (m_hVS == nullptr) return false;

	m_hPS = waveworks::graphics::createGraphicsShader(m_pDevice, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel), PSBinary, PSBinarySize);
	if (m_hPS == nullptr) return false;


	// create attribute layout object
	nvrhi::VertexAttributeDesc vertexAttribLayout[] = {
		{ "POSITION", nvrhi::Format::RG32_FLOAT, 1, 0, static_cast<uint32_t>(offsetof(ImDrawVert, pos)), sizeof(ImDrawVert), false },
		{ "TEXCOORD", nvrhi::Format::RG32_FLOAT, 1, 0, static_cast<uint32_t>(offsetof(ImDrawVert, uv)), sizeof(ImDrawVert), false },
		{ "COLOR", nvrhi::Format::RGBA8_UNORM, 1, 0, static_cast<uint32_t>(offsetof(ImDrawVert, col)), sizeof(ImDrawVert), false },
	};

	m_pDevice->createInputLayout(vertexAttribLayout, sizeof(vertexAttribLayout) / sizeof(vertexAttribLayout[0]), m_hVS, &m_hAttribLayout);

	// creating constant buffer
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = sizeof(IMGUI_VS_CBUFFER);
		desc.isConstantBuffer = true;
		desc.debugName = "ImGUI constant buffer";

		m_pDevice->createBuffer(desc, &m_hCB);
	}

	// creating font texture
	{
	uint8_t *pixels;
	int width, height;

	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.debugName = "ImGui font texture";
		m_pDevice->createTexture(desc, &m_hFontTexture);
		if (m_hFontTexture == nullptr) return false;

		m_hCommandList->beginTrackingTextureState(m_hFontTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
		m_hCommandList->writeTexture(m_hFontTexture, 0, 0, pixels, width * 4);
		m_hCommandList->setPermanentTextureState(m_hFontTexture, nvrhi::ResourceStates::ShaderResource);
		m_hCommandList->commitBarriers();

		io.Fonts->SetTexID((ImTextureID)(uintptr_t)m_hFontTexture.Get());
	}

	// Creating sampler
	{
		nvrhi::SamplerDesc desc;
		desc.addressU = nvrhi::SamplerAddressMode::Wrap;
		desc.addressV = nvrhi::SamplerAddressMode::Wrap;
		desc.addressW = nvrhi::SamplerAddressMode::Wrap;
		desc.mipBias = 0.0;
		desc.maxAnisotropy = 1.0;
		desc.minFilter = true;
		desc.magFilter = true;
		desc.mipFilter = true;

		m_pDevice->createSampler(desc, &m_hFontSampler);

		if (m_hFontSampler == nullptr) return false;
	}

	// Creating graphics pipeline
	{
		nvrhi::BlendState blendState;
		blendState.targets[0].blendEnable = true;
		blendState.targets[0].srcBlend = nvrhi::BlendFactor::One;
		blendState.targets[0].destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		blendState.targets[0].blendOp = nvrhi::BlendOp::Add;
		blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::One;
		blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::Zero;
		blendState.targets[0].blendOpAlpha = nvrhi::BlendOp::Add;
		blendState.targets[0].colorWriteMask = nvrhi::ColorMask::All;

		nvrhi::RasterState rasterState;
		rasterState.fillMode = nvrhi::RasterFillMode::Solid;
		rasterState.cullMode = nvrhi::RasterCullMode::None;
		rasterState.scissorEnable = true;
		rasterState.depthClipEnable = true;

		nvrhi::DepthStencilState depthStencilState;
		depthStencilState.depthTestEnable = false;
		depthStencilState.depthWriteEnable = true;
		depthStencilState.depthFunc = nvrhi::ComparisonFunc::Always;
		depthStencilState.stencilEnable = false;

		nvrhi::RenderState renderState;
		renderState.blendState = blendState;
		renderState.depthStencilState = depthStencilState;
		renderState.rasterState = rasterState;

		waveworks::graphics::GraphicsBindingLayoutDesc layoutDesc;
		layoutDesc.VS = { 
			nvrhi::BindingLayoutItem::ConstantBuffer(0) 
		};
		layoutDesc.PS = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0)
		};
		m_hBindingLayout = waveworks::graphics::createBindingLayout(m_pDevice, layoutDesc);

		waveworks::graphics::GraphicsBindingSetDesc bsDesc;
		bsDesc.VS = { 
			nvrhi::BindingSetItem::ConstantBuffer(0, m_hCB) 
		};
		bsDesc.PS = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_hFontTexture),
			nvrhi::BindingSetItem::Sampler(0, m_hFontSampler)
		};
		m_hBindingSet = waveworks::graphics::createBindingSet(m_pDevice, bsDesc, m_hBindingLayout);



		nvrhi::TextureDesc td;
		td.width = m_backBufferWidth;
		td.height = m_backBufferHeight;
		td.sampleCount = m_sampleCount;
		td.isRenderTarget = true;
		td.clearValue = nvrhi::Color(1.0f);
		td.useClearValue = true;
		td.debugName = "proxy color target";
		td.format = useVk ? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
		m_pDevice->createTexture(td, &m_hInitTimeRenderTargetTexture);
		nvrhi::FramebufferDesc fbDesc;
		fbDesc.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(m_hInitTimeRenderTargetTexture).setArraySlice(0).setMipLevel(0));
		m_pDevice->createFramebuffer(fbDesc, &m_hInitTimeFB);

		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
		psoDesc.inputLayout = m_hAttribLayout;
		psoDesc.VS = m_hVS;
		psoDesc.PS = m_hPS;
		psoDesc.renderState = renderState;
		psoDesc.bindingLayouts = ( m_hBindingLayout ).layouts;
		m_pDevice->createGraphicsPipeline1(psoDesc, m_hInitTimeFB->getFramebufferInfo().getInfo(), &m_hPSO);
		assert(m_hPSO);
	}


	m_hCommandList->close();
	m_pDevice->executeCommandList(m_hCommandList);

	return true;
}

bool UIRenderer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    auto& io = ImGui::GetIO();
    ImGuiKey mapped = ImGuiKey_None;
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        mapped = ImGuiKey(ImGuiKey_A + key - GLFW_KEY_A);
    else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        mapped = ImGuiKey(ImGuiKey_0 + key - GLFW_KEY_0);
    else switch (key) {
    case GLFW_KEY_TAB: mapped = ImGuiKey_Tab; break;
    case GLFW_KEY_LEFT: mapped = ImGuiKey_LeftArrow; break;
    case GLFW_KEY_RIGHT: mapped = ImGuiKey_RightArrow; break;
    case GLFW_KEY_UP: mapped = ImGuiKey_UpArrow; break;
    case GLFW_KEY_DOWN: mapped = ImGuiKey_DownArrow; break;
    case GLFW_KEY_PAGE_UP: mapped = ImGuiKey_PageUp; break;
    case GLFW_KEY_PAGE_DOWN: mapped = ImGuiKey_PageDown; break;
    case GLFW_KEY_HOME: mapped = ImGuiKey_Home; break;
    case GLFW_KEY_END: mapped = ImGuiKey_End; break;
    case GLFW_KEY_INSERT: mapped = ImGuiKey_Insert; break;
    case GLFW_KEY_DELETE: mapped = ImGuiKey_Delete; break;
    case GLFW_KEY_BACKSPACE: mapped = ImGuiKey_Backspace; break;
    case GLFW_KEY_SPACE: mapped = ImGuiKey_Space; break;
    case GLFW_KEY_ENTER: mapped = ImGuiKey_Enter; break;
    case GLFW_KEY_ESCAPE: mapped = ImGuiKey_Escape; break;
    case GLFW_KEY_LEFT_CONTROL: mapped = ImGuiKey_LeftCtrl; break;
    case GLFW_KEY_RIGHT_CONTROL: mapped = ImGuiKey_RightCtrl; break;
    case GLFW_KEY_LEFT_SHIFT: mapped = ImGuiKey_LeftShift; break;
    case GLFW_KEY_RIGHT_SHIFT: mapped = ImGuiKey_RightShift; break;
    case GLFW_KEY_LEFT_ALT: mapped = ImGuiKey_LeftAlt; break;
    case GLFW_KEY_RIGHT_ALT: mapped = ImGuiKey_RightAlt; break;
    default: break;
    }
    if (mapped != ImGuiKey_None) io.AddKeyEvent(mapped, action != GLFW_RELEASE);
    io.AddKeyEvent(ImGuiMod_Ctrl, (mods & GLFW_MOD_CONTROL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & GLFW_MOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (mods & GLFW_MOD_ALT) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (mods & GLFW_MOD_SUPER) != 0);
    return io.WantCaptureKeyboard;
}
bool UIRenderer::KeyboardCharInput(unsigned int character, int)
{
    ImGui::GetIO().AddInputCharacter(character);
    return ImGui::GetIO().WantCaptureKeyboard;
}
bool UIRenderer::MousePosUpdate(double x, double y)
{
    ImGui::GetIO().AddMousePosEvent(float(x), float(y));
    return ImGui::GetIO().WantCaptureMouse;
}
bool UIRenderer::MouseScrollUpdate(double x, double y)
{
    ImGui::GetIO().AddMouseWheelEvent(float(x), float(y));
    return ImGui::GetIO().WantCaptureMouse;
}
bool UIRenderer::MouseButtonUpdate(int button, int action, int)
{
    if (button >= 0 && button < 5)
        ImGui::GetIO().AddMouseButtonEvent(button, action != GLFW_RELEASE);
    return ImGui::GetIO().WantCaptureMouse;
}

void UIRenderer::Animate(float elapsedTimeSeconds)
{
    int w, h;
    float scaleX, scaleY;

    m_pDeviceManager->GetWindowDimensions(w, h);
    m_pDeviceManager->GetDPIScaleInfo(scaleX, scaleY);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(w), float(h));
    io.DisplayFramebufferScale.x = scaleX;
    io.DisplayFramebufferScale.y = scaleY;

	io.DeltaTime = elapsedTimeSeconds;
	io.MouseDrawCursor = false;

	ImGui::NewFrame();
}

void UIRenderer::createOrRecreateBufferIfNeeded(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, const bool indexBuffer)
{
	if (buffer == nullptr || size_t(buffer->getDesc().byteSize) < requiredSize)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = uint32_t(reallocateSize);
		desc.structStride = 0;
		desc.debugName = indexBuffer ? "ImGui index buffer" : "ImGui vertex buffer";
		desc.canHaveUAVs = false;
		desc.isVertexBuffer = !indexBuffer;
		desc.isIndexBuffer = indexBuffer;
		desc.isDrawIndirectArgs = false;

		desc.isVolatile = false;

		buffer = nullptr;
		m_pDevice->createBuffer(desc, &buffer);
	}
}

void UIRenderer::updateGeometry()
{
	ImDrawData *drawData = ImGui::GetDrawData();

	// create/resize vertex and index buffers if needed
	createOrRecreateBufferIfNeeded(m_hVB, drawData->TotalVtxCount * sizeof(ImDrawVert), (drawData->TotalVtxCount + 5000) * sizeof(ImDrawVert), false);
	createOrRecreateBufferIfNeeded(m_hIB, drawData->TotalIdxCount * sizeof(ImDrawIdx), (drawData->TotalIdxCount + 5000) * sizeof(ImDrawIdx), true);

	m_IMVertexBuffer.resize(m_hVB->getDesc().byteSize / sizeof(ImDrawVert));
	m_IMIndexBuffer.resize(m_hIB->getDesc().byteSize / sizeof(ImDrawIdx));

	// copy and convert all vertices into a single contiguous buffer
	ImDrawVert *vtxDst = m_IMVertexBuffer.data();
	ImDrawIdx *idxDst = m_IMIndexBuffer.data();

	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList *cmdList = drawData->CmdLists[n];

		memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

		vtxDst += cmdList->VtxBuffer.Size;
		idxDst += cmdList->IdxBuffer.Size;
	}
	m_hCommandList->beginTrackingBufferState(m_hVB, nvrhi::ResourceStates::Common);
	m_hCommandList->beginTrackingBufferState(m_hIB, nvrhi::ResourceStates::Common);
	m_hCommandList->writeBuffer(m_hVB, m_IMVertexBuffer.data(), m_hVB->getDesc().byteSize);
	m_hCommandList->writeBuffer(m_hIB, m_IMIndexBuffer.data(), m_hIB->getDesc().byteSize);
	m_hCommandList->setBufferState(m_hVB, nvrhi::ResourceStates::Common);
	m_hCommandList->commitBarriers();
	m_hCommandList->setBufferState(m_hIB, nvrhi::ResourceStates::Common);
	m_hCommandList->commitBarriers();
}

void UIRenderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    auto* renderTarget = framebuffer->getDesc().colorAttachments[0].texture;
    const uint32_t swapChainIndex = GetDeviceManager()->GetCurrentBackBufferIndex();
	if (!ReadProcessEnvironmentVariable("WAVEWORKS_HIDE_UI").empty())
		return;

	// Building UI geometry data within ImGUI
    BuildUI();
    ImGui::Render();

	
	ImDrawData *drawData = ImGui::GetDrawData();
	auto& io = ImGui::GetIO();

	m_hCommandList->open();
	m_hCommandList->beginMarker("ImGUI");

	// Getting geometry data from ImGUI
	updateGeometry();


	m_hCommandList->beginTrackingBufferState(m_hCB, nvrhi::ResourceStates::Common);
	m_hCommandList->beginTrackingBufferState(m_hVB, nvrhi::ResourceStates::Common);
	m_hCommandList->beginTrackingBufferState(m_hIB, nvrhi::ResourceStates::Common);
	if (!renderTarget->getDesc().keepInitialState)
		m_hCommandList->beginTrackingTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
	m_hCommandList->beginTrackingTextureState(m_hFontTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);


	// Handling DPI scaling
	drawData->ScaleClipRects(io.DisplayFramebufferScale);

	// Updating constant buffer if needed
	if ((io.DisplaySize.x != m_lastDisplaySizeX) || (io.DisplaySize.y != m_lastDisplaySizeY))
	{

		// setup orthgraphic projection matrix into our constant buffer
		float mvp[4][4] =
		{
			{ 2.0f / io.DisplaySize.x, 0.0f,                   0.0f, 0.0f },
			{ 0.0f,                  2.0f / -io.DisplaySize.y, 0.0f, 0.0f },
			{ 0.0f,                  0.0f,                  -1.0f, 0.0f },
			{ -1.0f,                  1.0f,                   0.0f, 1.0f },
		};

		m_hCommandList->writeBuffer(m_hCB, mvp, sizeof(mvp));
		m_lastDisplaySizeX = (uint32_t)io.DisplaySize.x;
		m_lastDisplaySizeY = (uint32_t)io.DisplaySize.y;
	}


	// Setting up graphics state
	nvrhi::GraphicsState drawState;

	nvrhi::FramebufferHandle hFB = getFramebuffer(renderTarget, swapChainIndex);
	drawState.framebuffer = hFB;
	drawState.pipeline = m_hPSO;
	drawState.bindings = ( m_hBindingSet ).sets();
	drawState.viewport.viewports.resize(0);
	drawState.viewport.viewports.push_back(nvrhi::Viewport(io.DisplaySize.x * io.DisplayFramebufferScale.x,	io.DisplaySize.y * io.DisplayFramebufferScale.y));
	drawState.viewport.scissorRects.resize(1);  

	nvrhi::VertexBufferBinding vbufBinding;
	vbufBinding.buffer = m_hVB;
	vbufBinding.slot = 0;
	vbufBinding.offset = 0;
	drawState.vertexBuffers.push_back(vbufBinding);

	drawState.indexBuffer.buffer = m_hIB;
	drawState.indexBuffer.format = (sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
	drawState.indexBuffer.offset = 0;

	// render command lists
	int vtxOffset = 0;
	int idxOffset = 0;
	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList *cmdList = drawData->CmdLists[n];
		for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
		{
			const ImDrawCmd *pCmd = &cmdList->CmdBuffer[i];

			if (pCmd->UserCallback)
			{
				pCmd->UserCallback(cmdList, pCmd);
			}
			else {
				
				drawState.viewport.scissorRects[0] = nvrhi::Rect(int(pCmd->ClipRect.x),
					int(pCmd->ClipRect.z),
					int(pCmd->ClipRect.y),
					int(pCmd->ClipRect.w));
					
				nvrhi::DrawArguments drawArguments;
				drawArguments.vertexCount = pCmd->ElemCount;
				drawArguments.startIndexLocation = idxOffset;
				drawArguments.startVertexLocation = vtxOffset;

				m_hCommandList->setGraphicsState(drawState);
				m_hCommandList->drawIndexed(drawArguments);
			}

			idxOffset += pCmd->ElemCount;
		}

		vtxOffset += cmdList->VtxBuffer.Size;
	}

	m_hCommandList->setBufferState(m_hVB, nvrhi::ResourceStates::Common);
	m_hCommandList->commitBarriers();
	m_hCommandList->setBufferState(m_hIB, nvrhi::ResourceStates::Common);
	m_hCommandList->commitBarriers();
	m_hCommandList->setBufferState(m_hCB, nvrhi::ResourceStates::Common);
	m_hCommandList->commitBarriers();
	m_hCommandList->setTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
	m_hCommandList->commitBarriers();
	m_hCommandList->setTextureState(m_hFontTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
	m_hCommandList->commitBarriers();

	m_hCommandList->endMarker();
	m_hCommandList->close();
	m_pDevice->executeCommandList(m_hCommandList);


}

nvrhi::FramebufferHandle UIRenderer::getFramebuffer(nvrhi::TextureHandle renderTarget, uint32_t swapChainIndex)
{
	nvrhi::TextureDesc td = renderTarget->getDesc();

	// Create framebuffers if needed
	if (swapChainIndex >= m_hFBs.size()) m_hFBs.resize(swapChainIndex + 1);

	if (m_hFBs[swapChainIndex] == nullptr)
	{
		nvrhi::FramebufferDesc desc = nvrhi::FramebufferDesc().addColorAttachment(renderTarget);

		m_pDevice->createFramebuffer(desc, &m_hFBs[swapChainIndex]);
	}

	if (m_needToRecreatePSOs)
	{
		// Recreating PSOs
		nvrhi::GraphicsPipelineDesc PSOdesc = m_hPSO->getDesc();
		m_hPSO = nullptr;
		m_pDevice->createGraphicsPipeline1(PSOdesc, m_hFBs[swapChainIndex]->getFramebufferInfo().getInfo(), &m_hPSO);
		m_needToRecreatePSOs = false;
	}

	assert(m_hFBs.at(swapChainIndex));
	return m_hFBs[swapChainIndex];
}

void UIRenderer::BackBufferResizing()
{
	m_hFBs.clear();
}
void UIRenderer::BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount)
{
	m_backBufferWidth = width;
	m_backBufferHeight = height;
	m_sampleCount = sampleCount;

	if (m_pDevice == nullptr) return;
	if (m_pState == nullptr) return;

	m_pDevice->waitForIdle();

	m_needToRecreatePSOs = true;
}
