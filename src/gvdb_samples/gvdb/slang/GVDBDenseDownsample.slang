// SPDX-License-Identifier: Apache-2.0
struct DownsampleParameters {
    uint4 sourceResolution;
    uint4 destinationResolution;
    float4 destinationExtent;
    float4 transformRow0;
    float4 transformRow1;
    float4 transformRow2;
    float4 remap;
};
ConstantBuffer<DownsampleParameters> parameters : register(b0);
StructuredBuffer<float> sourceValues : register(t0);
RWStructuredBuffer<uint2> regionStatistics : register(u0);
// Slang 2026.10.2 emits a literal unsupported `precise` qualifier in CUDA
// unless it is confined to the graphics target arm (as in point conversion).
float multiplyRounded(float a,float b) {
    __target_switch {
    case cuda: __intrinsic_asm "__fmul_rn($0, $1)";
    default: precise float result=a*b;return result;
    }
}
float addRounded(float a,float b) {
    __target_switch {
    case cuda: __intrinsic_asm "__fadd_rn($0, $1)";
    default: precise float result=a+b;return result;
    }
}
float divideRounded(float a,float b) {
    __target_switch {
    case cuda: __intrinsic_asm "__fdiv_rn($0, $1)";
    default: precise float result=a/b;return result;
    }
}
float transformComponent(float3 point,float4 row) {
    return addRounded(addRounded(addRounded(multiplyRounded(point.x,row.x),multiplyRounded(point.y,row.y)),multiplyRounded(point.z,row.z)),row.w);
}
float3 transformPoint(float3 point) {
    return float3(transformComponent(point,parameters.transformRow0),transformComponent(point,parameters.transformRow1),transformComponent(point,parameters.transformRow2));
}
// Correctly rounded positive binary32 / positive integer. Graphics `precise`
// prevents contraction but does not require correctly rounded division: AMD
// can evaluate 6/3 just below 2, which changes a later integer sample boundary.
// The checked denominator is <=524281. Long division needs at most 20 bits,
// with a <2^20 remainder, so this uses only portable uint32 operations.
float divideEndpoint(float numerator,uint denominator) {
    uint bits=asuint(numerator),mantissa=bits&0x7fffffu;
    uint encodedExponent=(bits>>23)&255u;
    if(denominator==0||bits>=0x7f800000u)return asfloat(0x7fc00000u);
    if(bits==0)return 0.0;
    int exponent;
    if(encodedExponent==0) {
        uint shift=23u-uint(firstbithigh(mantissa));
        mantissa<<=shift;
        exponent=-126-int(shift);
    } else {
        mantissa|=0x800000u;
        exponent=int(encodedExponent)-127;
    }
    uint steps=uint(firstbithigh(denominator));
    uint quotient=mantissa/denominator;
    uint remainder=mantissa-quotient*denominator;
    [loop] for(uint i=0;i<steps;++i) {
        quotient<<=1;remainder<<=1;
        if(remainder>=denominator){++quotient;remainder-=denominator;}
    }
    exponent-=int(steps);
    if(quotient<0x800000u) {
        quotient<<=1;remainder<<=1;
        if(remainder>=denominator){++quotient;remainder-=denominator;}
        --exponent;
    }
    if(exponent< -126) {
        uint shift=uint(-126-exponent);
        if(shift>=25u)return 0.0;
        uint result=quotient>>shift;
        uint guard=(quotient>>(shift-1u))&1u;
        bool sticky=(quotient&((1u<<(shift-1u))-1u))!=0||remainder!=0;
        if(guard!=0&&(sticky||(result&1u)!=0))++result;
        return asfloat(result); // Includes carry from largest subnormal to min normal.
    }
    uint twice=remainder*2u;
    if(twice>denominator||(twice==denominator&&(quotient&1u)!=0))++quotient;
    if(quotient==0x1000000u){quotient>>=1;++exponent;}
    return asfloat((uint(exponent+127)<<23)|(quotient&0x7fffffu));
}
float endpoint(uint index,float extent,uint resolution,bool upper) {
    float value=divideEndpoint(multiplyRounded(float(index),extent),resolution+1);
    return upper?addRounded(value,-1.0):value;
}
uint2 computeStatistics(uint3 id) {
    uint3 resolution=parameters.destinationResolution.xyz;
    float3 lower=float3(endpoint(id.x,parameters.destinationExtent.x,resolution.x,false),endpoint(id.y,parameters.destinationExtent.y,resolution.y,false),endpoint(id.z,parameters.destinationExtent.z,resolution.z,false));
    float3 upper=float3(endpoint(id.x+1,parameters.destinationExtent.x,resolution.x,true),endpoint(id.y+1,parameters.destinationExtent.y,resolution.y,true),endpoint(id.z+1,parameters.destinationExtent.z,resolution.z,true));
    float3 low=transformPoint(lower), high=transformPoint(upper);
    // Preparation proves this domain. Never cast an exceptional endpoint even
    // if a malformed artifact/parameter block violates that proof.
    if(!all(isfinite(low))||!all(isfinite(high))||any(abs(low)>2147483000.0)||any(abs(high)>2147483000.0)) {
        return uint2(0,0);
    }
    int3 sourceMax=int3(parameters.sourceResolution.xyz)-1;
    int3 first=clamp(int3(low),int3(0),sourceMax);
    int3 last=clamp(int3(high),first,sourceMax);
    float sum=0.0;
    uint count=0;
    [loop] for(int z=first.z;z<=last.z;++z)
        [loop] for(int y=first.y;y<=last.y;++y)
            [loop] for(int x=first.x;x<=last.x;++x) {
                float value=sourceValues[(uint(z)*parameters.sourceResolution.y+uint(y))*parameters.sourceResolution.x+uint(x)];
                float remapped=addRounded(parameters.remap.z,divideRounded(multiplyRounded(addRounded(value,-parameters.remap.x),addRounded(parameters.remap.w,-parameters.remap.z)),addRounded(parameters.remap.y,-parameters.remap.x)));
                sum=addRounded(sum,remapped);
                ++count;
            }
    return uint2(asuint(sum),count);
}

[shader("compute")]
[numthreads(8,8,8)]
void downsampleDenseRegion(uint3 id : SV_DispatchThreadID) {
    // Vulkan permits RTZ by default. The serial Float32 recurrence requires
    // RTE32 explicitly, including cancellation-sensitive per-sample remaps.
    __target_switch {
    case spirv:
        spirv_asm {
            OpCapability RoundingModeRTE;
            OpExtension "SPV_KHR_float_controls";
            OpExecutionMode __entryPoint RoundingModeRTE 32;
        };
        break;
    default: break;
    }
    uint3 resolution=parameters.destinationResolution.xyz;
    if(any(id>=resolution))return;
    uint index=(id.z*resolution.y+id.y)*resolution.x+id.x;
    regionStatistics[index]=computeStatistics(id);
}
