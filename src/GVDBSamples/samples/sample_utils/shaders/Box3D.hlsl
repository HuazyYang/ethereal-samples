#pragma pack_matrix(row_major)

#include <donut/shaders/binding_helpers.hlsli>

struct Common {
    float4x4 matWorldViewProj;
};
DECLARE_PUSH_CONSTANTS(Common, g_Const, 0, 0);

struct InstanceData {
    float4x4 matLocalToWorld;
    float4 color;
};

StructuredBuffer<InstanceData> g_InstanceData: register(t0);

struct VSInput {
    float4 posL: POSITION;
    uint instanceID: SV_InstanceID;
};

struct VSOutput {
    float4 posH: SV_Position;
    float4 color: COLOR;
};

VSOutput VSMain(VSInput vin) {
    VSOutput vout;
    InstanceData instanceData = g_InstanceData[vin.instanceID];
    float4x4 matLocalToProj = mul(instanceData.matLocalToWorld, g_Const.matWorldViewProj);
    vout.posH = mul(vin.posL, matLocalToProj);
    vout.color = instanceData.color;
    return vout;
}

float4 PSMain(VSOutput pin): SV_Target {
    return pin.color;
}



