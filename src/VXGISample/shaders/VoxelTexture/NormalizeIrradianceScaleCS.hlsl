#include "IrradianceMapCB.hlsli"

RWBuffer<uint> u_IrradianceNormalization: register(u4);
groupshared uint s_AverageIrradiance;
[numthreads(STRUCT_NUM_PARTIAL_SUMS/4, 1, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
if(gfsdk_GroupThreadIdx.x == 0)
s_AverageIrradiance = 0;
GroupMemoryBarrierWithGroupSync();
uint numVoxels = u_IrradianceNormalization[STRUCT_VOXEL_COUNT];
numVoxels = max(1, numVoxels);
float partialAverage = float(u_IrradianceNormalization[STRUCT_PARTIAL_SUMS + gfsdk_GroupThreadIdx.x]) / float(numVoxels);
uint previousValue;
InterlockedAdd(s_AverageIrradiance, uint(partialAverage), previousValue);
u_IrradianceNormalization[STRUCT_PARTIAL_SUMS + gfsdk_GroupThreadIdx.x] = 0;
GroupMemoryBarrierWithGroupSync();
if(gfsdk_GroupThreadIdx.x > 0)
return;
float averageIrradiance = float(s_AverageIrradiance) / INJECTION_FIXED_POINT_SCALE;
u_IrradianceNormalization[STRUCT_VOXEL_COUNT] = 0;
float previousIrradiance = asfloat(u_IrradianceNormalization[STRUCT_LAST_AVERAGE_IRRADIANCE]);
u_IrradianceNormalization[STRUCT_LAST_AVERAGE_IRRADIANCE] = asuint(averageIrradiance);
if(previousIrradiance <= 0)
previousIrradiance = 1;
float derivative = averageIrradiance / previousIrradiance;
uint writePointer = u_IrradianceNormalization[STRUCT_NEXT_DERIVATIVE];
writePointer = min(writePointer, STRUCT_NUM_DERIVATIVES - 1);
u_IrradianceNormalization[STRUCT_DERIVATIVES + writePointer] = asuint(derivative);
writePointer += 1;
if(writePointer >= STRUCT_NUM_DERIVATIVES)
writePointer = 0;
u_IrradianceNormalization[STRUCT_NEXT_DERIVATIVE] = writePointer;
float derivativeHistory[STRUCT_NUM_DERIVATIVES];
float averageDerivative = 0;
[unroll]
for(uint n = 0; n < STRUCT_NUM_DERIVATIVES; n++)
{
float item = asfloat(u_IrradianceNormalization[STRUCT_DERIVATIVES + n]);
derivativeHistory[n] = item;
averageDerivative += item;
}
averageDerivative /= float(STRUCT_NUM_DERIVATIVES);
if(averageDerivative <= 1.001)
return;
float derivativeDeviation = 0;
[unroll]
for(uint n = 0; n < STRUCT_NUM_DERIVATIVES; n++)
{
float diff = derivativeHistory[n] - averageDerivative;
derivativeDeviation += diff * diff;
}
derivativeDeviation = sqrt(derivativeDeviation / float(STRUCT_NUM_DERIVATIVES));
derivativeDeviation /= (averageDerivative - 1.0);
if(derivativeDeviation >= 0.1)
return;
float normalizationFactor = asfloat(u_IrradianceNormalization[STRUCT_IRRADIANCE_MULTIPLIER]);
if(normalizationFactor <= 0)
normalizationFactor = 1;
normalizationFactor *= 0.9 / averageDerivative;
u_IrradianceNormalization[STRUCT_IRRADIANCE_MULTIPLIER] = asuint(normalizationFactor);
}
