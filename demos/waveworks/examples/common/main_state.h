/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include <waveworks/GFSDK_WaveWorks.h>
#include <waveworks/GFSDK_WaveWorks.h>
#include "./Camera.h"

#include "graphics_bindings.h"

#include <stdio.h>
#include <assert.h>

#ifndef MAINSTATE
#define MAINSTATE

// Readback state
enum SynchronizationMode
{
	SynchronizationMode_None = 0,
	SynchronizationMode_RenderOnly,
	SynchronizationMode_Readback,
	Num_SynchronizationModes
};

enum { NumMarkersXY = 1000, NumMarkers = NumMarkersXY*NumMarkersXY };

struct MainState
{
	// Simulation state
	GFSDK_WaveWorks_Wind_Waves_SimulationHandle			hOceanWindSimulation = nullptr;
	GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters	OceanWindSimulationParameters;
	GFSDK_WaveWorks_Wind_Waves_Simulation_Settings		OceanWindSimulationSettings;
	GFSDK_WaveWorks_Wind_Waves_Simulation_Stats			OceanWindSimulationStats;
	GFSDK_WaveWorks_Wind_Waves_Simulation_Stats			OceanWindSimulationStatsFiltered;

	GFSDK_WaveWorks_Local_Waves_SimulationHandle		hOceanLocalSimulation = nullptr;
	GFSDK_WaveWorks_Local_Waves_Simulation_Parameters	OceanLocalSimulationParameters;
	GFSDK_WaveWorks_Local_Waves_Simulation_Settings		OceanLocalSimulationSettings;
	GFSDK_WaveWorks_Local_Waves_Simulation_Stats		OceanLocalSimulationStats;
	GFSDK_WaveWorks_Local_Waves_Simulation_Stats		OceanLocalSimulationStatsFiltered;

	GFSDK_WaveWorks_QuadtreeHandle                      hOceanQuadtree = nullptr;
	GFSDK_WaveWorks_Quadtree_Params                     OceanQuadtreeParameters;
	GFSDK_WaveWorks_Quadtree_Stats                      OceanQuadtreeStats;

	uint64_t iLastWindSimulationKickID = 0;
	uint64_t iLastWindSimulationArchivedKickID = GFSDK_WaveWorks_InvalidKickID;
	uint64_t iLastWindSimulationReadbackKickID = GFSDK_WaveWorks_InvalidKickID;
	uint32_t iWindSimulationRenderLatency = 0;
	int32_t iWindSimulationReadbackLatency = 0;

	uint64_t iLastLocalSimulationKickID = 0;
	uint64_t iLastLocalSimulationArchivedKickID = GFSDK_WaveWorks_InvalidKickID;
	uint64_t iLastLocalSimulationReadbackKickID = GFSDK_WaveWorks_InvalidKickID;
	uint32_t iLocalSimulationRenderLatency = 0;
	int32_t iLocalSimulationReadbackLatency = 0;

	enum { ReadbackArchiveSize = 30 };
	enum { ReadbackArchiveInterval = 10 };
	float fReadbackCoord = 0.f;

	SynchronizationMode SyncMode = SynchronizationMode_None;

	bool bForceKick = false;
	bool bUpdateWindWaves = true;
	bool bUpdateLocalWaves = true;
	float fMinDisplacement = 0;
	float fMaxDisplacement = 0;

	float fMinTotalDisplacement = 0;
	float fMaxTotalDisplacement = 0;
	float fSWH = 0;

	// Animation & rendering state
	nvrhi::TextureHandle hDepthBufferTexture;
	nvrhi::TextureHandle hDynamicSkyDomeTexture;
	nvrhi::TextureHandle hBeachIrradiance, hBeachSpecularEnvironment;
	nvrhi::TextureHandle hBeachEnvironmentBrdf, hBeachShadow;
	nvrhi::TextureHandle hBeachAerialRadiance, hBeachAerialTransmittance;
	nvrhi::SamplerHandle hBeachEnvironmentSampler;
	nvrhi::BufferHandle hBeachLightingConstants;

	FPSCamera Camera;
	float fCameraClipNear = 0.1f;
	float fCameraClipFar = 100000.0f;
	double dSimulationTime = 0.0;
	bool bNeedToUpdateWindWavesSimulationProperties = false;
	bool bNeedToUpdateLocalWavesSimulationProperties = false;
	bool bNeedToResetLocalWavesSimulation = false;
	bool bRain = false;
	bool bBoat = true;
	float fRainDropSize = 0.05f;
	bool bNeedToUpdateSky = true;
	float fBaseWindDirection = 45.0f;
	float fSwellWindDirection = -18.0f;
	float fTessellationAmount = 1.0f;
	float fTessellationOffset = 0.0f;

	float fBoatX = 0;
	float fBoatY = 0;


	// Rendering variables
	bool bRenderWireframe = false;
	bool bRenderWater = true;
	bool bRenderMarkers = false;
	bool bRenderSky = true;
	bool bUseDynamicSky = true;
	bool bSimulateWater = true;
	bool bShowWindWavesSimulationStatistics = false;
	bool bShowLocalWavesSimulationStatistics = false;
	float fSunAngle = 20.0f;
	float fBeachSunAzimuth = 0.f;
	float fBeckmannRoughness = 0.00001f;
	float fSunIntensity = 1.0f;
	bool bUseMicrofacetFresnel = true;
	bool bUseMicrofacetSpecular = true;
	bool bUseMicrofacetReflection = true;
	int iRefinementSteps = 1;
	int iReadbackUsage = 0;
	
	bool bShowCascades = false;
	bool bNeedToUpdateQuadtreeProperties = false;

	bool bBeachScene = false;
	bool bBeachCoveTerrain = false; // Startup selection; survey remains the default.
	bool bRenderBeachTerrain = true;
	bool bRenderBeachVegetation = true;
	bool bBeachShadows = true;
	float fBeachSandSpecular = 0.3f;
	float fBeachSandNormalStrength = 0.22f;
	float fBeachWetSandRoughness = 0.48f;
	bool bBeachSurfForcing = true;
	bool bBeachShallowSimulation = true;
	bool bBeachResetShallow = false;
	float fBeachShallowMilliseconds = 0.f;
	float fBeachShallowDebt = 0.f;
	float fBeachShallowTime = 0.f;
	float fBeachShallowCfl = 0.f;
	nvrhi::TextureHandle hBeachShallowSurface, hBeachShallowFlow;
	nvrhi::BufferHandle hBeachShallowConstants;
	int iBeachTerrainDebug = 0;
	int iBeachWaterDebug = 0;
	gfsdk_float3 vBeachSunIrradiance = { 8.0f, 7.5f, 6.7f };
	bool bBeachShallowWater = true;
	bool bBeachShoreFoam = true;
	float fBeachHaze = 0.0009f;
	float fBeachShoreFoamIntensity = 1.2f;

};
#endif //MAINSTATE
