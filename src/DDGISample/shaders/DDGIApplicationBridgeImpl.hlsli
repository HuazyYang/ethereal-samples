#include "Descriptors.hlsli"

uint GetDDGIVolumeIndex() {
    return GetGlobalRootConst(ddgi, volumeIndex);
}

uint3 GetReductionInputSize() {
    return uint3(GetGlobalRootConst(ddgi, reductionInputSizeX), GetGlobalRootConst(ddgi, reductionInputSizeY), GetGlobalRootConst(ddgi, reductionInputSizeZ));
}

DDGIVolumeDescGPU GetDDGIVolumeDescGPU() {
    DDGIVolumeDescGPUPacked volumePacked = DDGIVolumes[GetDDGIVolumeIndex()];
    return UnpackDDGIVolumeDescGPU(volumePacked);
}

DDGIVolumeDescGPU GetDDGIVolumeDescGPU(uint index) {
    DDGIVolumeDescGPUPacked volumePacked = DDGIVolumes[index];
    return UnpackDDGIVolumeDescGPU(volumePacked);
}

DDGIVolumeResourceIndices GetDDGIResourceIndices() {
    return DDGIVolumeBindless[GetGlobalRootConst(ddgi, volumeIndex)];
}

DDGIVolumeResourceIndices GetDDGIResourceIndices(uint index) {
    return DDGIVolumeBindless[index];
}

RWTexture2DArray<float4> GetRayDataUAV(DDGIVolumeResourceIndices indices) {
    return FixedRWTex2DArray[NonUniformResourceIndex(indices.rayDataUAVIndex)];
}

Texture2DArray<float4> GetRayDataSRV(DDGIVolumeResourceIndices indices) {
    return FixedTex2DArray[NonUniformResourceIndex(indices.rayDataSRVIndex)];
}

RWTexture2DArray<float4> GetProbeIrradianceUAV(DDGIVolumeResourceIndices indices) {
    return FixedRWTex2DArray[NonUniformResourceIndex(indices.probeIrradianceUAVIndex)];
}

Texture2DArray<float4> GetProbeIrradianceSRV(DDGIVolumeResourceIndices indices) {
    return FixedTex2DArray[NonUniformResourceIndex(indices.probeIrradianceSRVIndex)];
}

RWTexture2DArray<float4> GetProbeDistanceUAV(DDGIVolumeResourceIndices indices) {
    return FixedRWTex2DArray[NonUniformResourceIndex(indices.probeDistanceUAVIndex)];
}

Texture2DArray<float4> GetProbeDistanceSRV(DDGIVolumeResourceIndices indices) {
    return FixedTex2DArray[NonUniformResourceIndex(indices.probeDistanceSRVIndex)];
}

RWTexture2DArray<float4> GetProbeDataUAV(DDGIVolumeResourceIndices indices) {
    return FixedRWTex2DArray[NonUniformResourceIndex(indices.probeDataUAVIndex)];
}

Texture2DArray<float4> GetProbeDataSRV(DDGIVolumeResourceIndices indices) {
    return FixedTex2DArray[NonUniformResourceIndex(indices.probeDataSRVIndex)];
}

RWTexture2DArray<float4> GetProbeVariabilityUAV(DDGIVolumeResourceIndices indices) {
    return FixedRWTex2DArray[NonUniformResourceIndex(indices.probeVariabilityUAVIndex)];
}

Texture2DArray<float4> GetProbeVariabilitySRV(DDGIVolumeResourceIndices indices) {
    return FixedTex2DArray[NonUniformResourceIndex(indices.probeVariabilityAverageSRVIndex)];
}

RWTexture2DArray<float4> GetProbeVariabilityAverageUAV(DDGIVolumeResourceIndices indices) {
    return FixedRWTex2DArray[NonUniformResourceIndex(indices.probeVariabilityAverageUAVIndex)];
}

Texture2DArray<float4> GetProbeVariabilityAverageSRV(DDGIVolumeResourceIndices indices) {
    return FixedTex2DArray[NonUniformResourceIndex(indices.probeVariabilityAverageSRVIndex)];
}



