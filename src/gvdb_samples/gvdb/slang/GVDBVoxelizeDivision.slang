// SPDX-License-Identifier: Apache-2.0
// Binary32 division with round-to-nearest, ties-to-even using uint32 only.
// Geometric callers provide finite numerator and finite positive denominator.
// The bit path also preserves subnormal results independently of arithmetic FTZ.
uint voxelDivideBits(uint numeratorBits,uint d) {
    uint n=numeratorBits,signBit=n&0x80000000u;
    n&=0x7fffffffu;
    if(n>=0x7f800000u||d==0||d>=0x7f800000u)return 0x7fc00000u;
    if(n==0||d==0x3f800000u)return numeratorBits;
    uint nm=n&0x7fffffu,dm=d&0x7fffffu,ne=n>>23,de=d>>23;
    int exponent=int(ne)-int(de);
    if(ne==0){uint shift=23u-uint(firstbithigh(nm));nm<<=shift;exponent+=1-int(shift);}else nm|=0x800000u;
    if(de==0){uint shift=23u-uint(firstbithigh(dm));dm<<=shift;exponent-=1-int(shift);}else dm|=0x800000u;
    uint quotient,remainder;
    if(dm==0x800000u){quotient=nm;remainder=0;}
    else {
        if(nm<dm){nm<<=1;--exponent;}
        quotient=1;remainder=nm-dm;
        [loop] for(uint i=0;i<23;++i){quotient<<=1;remainder<<=1;if(remainder>=dm){++quotient;remainder-=dm;}}
    }
    if(exponent< -126){
        uint shift=uint(-126-exponent);
        if(shift>=25)return signBit;
        uint result=quotient>>shift,guard=(quotient>>(shift-1))&1u;
        bool sticky=(quotient&((1u<<(shift-1))-1u))!=0||remainder!=0;
        if(guard!=0&&(sticky||(result&1u)!=0))++result;
        return signBit|result;
    }
    uint twice=remainder*2;
    if(twice>dm||(twice==dm&&(quotient&1u)!=0))++quotient;
    if(quotient==0x1000000u){quotient>>=1;++exponent;}
    if(exponent>127)return signBit|0x7f800000u;
    return signBit|(uint(exponent+127)<<23)|(quotient&0x7fffffu);
}
float voxelDivide(float n,float d){return asfloat(voxelDivideBits(asuint(n),asuint(d)));}
float3 voxelDivide3(float3 numerator,float3 denominator){
    return float3(voxelDivide(numerator.x,denominator.x),voxelDivide(numerator.y,denominator.y),voxelDivide(numerator.z,denominator.z));
}
