// Independent packed input and quantization references for GPU tests.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PortableFixtures.h"
namespace gvdb::slang::fixtures {
inline std::vector<double> doublePointComponents(){return {0.0,1.0,-1.0,1.000000059604644775390625,std::nextafter(1.000000059604644775390625,2.0),-16777217.0,
    1e20,-1e20,3.141592653589793,1e-20,-1e-20,16777217.0,65535.0,1.5,-2.75};}
inline std::vector<Float4> convertedDoublePoints(const std::vector<double>& values,PointConversionParameters p){
    std::vector<Float4> result(values.size()/3);const float low[]={p.minimum.x,p.minimum.y,p.minimum.z},delta[]={p.delta.x,p.delta.y,p.delta.z},scale[]={p.scale.x,p.scale.y,p.scale.z},trans[]={p.translation.x,p.translation.y,p.translation.z};
    for(size_t i=0;i<result.size();++i){float output[3];for(size_t c=0;c<3;++c)output[c]=(low[c]+float(values[i*3+c])*delta[c])*scale[c]+trans[c];result[i]={output[0],output[1],output[2],1};}return result;
}
inline std::vector<uint16_t> quantizedPoints(const std::vector<Float4>& points,Float4 minimum,Float4 range){
    std::vector<uint16_t> result(points.size()*3);const float low[]={minimum.x,minimum.y,minimum.z},width[]={range.x,range.y,range.z};
    for(size_t i=0;i<points.size();++i){const float value[]={points[i].x,points[i].y,points[i].z};for(size_t c=0;c<3;++c)
        result[i*3+c]=std::isfinite(value[c])?uint16_t(std::clamp((value[c]-low[c])/width[c],0.f,1.f)*65535.f):0;}
    return result;
}
} // namespace gvdb::slang::fixtures
