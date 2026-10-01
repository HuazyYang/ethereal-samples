#ifndef DDGIAPPLICATIONBRIDGE_HLSLI
#define DDGIAPPLICATIONBRIDGE_HLSLI

#include "../../ddgi/DDGITypes.h"

// Application Bridge function prototypes

uint GetDDGIVolumeIndex();
uint3 GetReductionInputSize();

DDGIVolumeDescGPU GetDDGIVolumeDescGPU();
DDGIVolumeDescGPU GetDDGIVolumeDescGPU(uint index);
DDGIVolumeResourceIndices GetDDGIResourceIndices();
DDGIVolumeResourceIndices GetDDGIResourceIndices(int index);

RWTexture2DArray<float4> GetRayDataUAV(DDGIVolumeResourceIndices indices);
Texture2DArray<float4> GetRayDataSRV(DDGIVolumeResourceIndices indices);
RWTexture2DArray<float4> GetProbeIrradianceUAV(DDGIVolumeResourceIndices indices);
Texture2DArray<float4> GetProbeIrradianceSRV(DDGIVolumeResourceIndices indices);
RWTexture2DArray<float4> GetProbeDistanceUAV(DDGIVolumeResourceIndices indices);
Texture2DArray<float4> GetProbeDistanceSRV(DDGIVolumeResourceIndices indices);
RWTexture2DArray<float4> GetProbeDataUAV(DDGIVolumeResourceIndices indices);
Texture2DArray<float4> GetProbeDataSRV(DDGIVolumeResourceIndices indices);
RWTexture2DArray<float4> GetProbeVariabilityUAV(DDGIVolumeResourceIndices indices);
Texture2DArray<float4> GetProbeVariabilitySRV(DDGIVolumeResourceIndices indices);
RWTexture2DArray<float4> GetProbeVariabilityAverageUAV(DDGIVolumeResourceIndices indices);
Texture2DArray<float4> GetProbeVariabilityAverageSRV(DDGIVolumeResourceIndices indices);

// External implements
#include "DDGIApplicationBridgeImpl.hlsli"

#endif /* DDGIAPPLICATIONBRIDGE_HLSLI */
