// End-to-end checks for portable GVDB host helpers on every compute backend.
// SPDX-License-Identifier: Apache-2.0
#include "ComputeRuntime.h"
#include "../gvdb_samples/gvdb/GVDBSlangPointConversion.h"
#include "../gvdb_samples/gvdb/GVDBSlangSubcells.h"
#include "../gvdb_samples/gvdb/GVDBSlangVelocity.h"
#include "../gvdb_samples/gvdb/slang/GVDBTopologyBuilder.h"
#include "../gvdb_samples/tests/PointConversionFixtures.h"
#include "../gvdb_samples/tests/PointFixtures.h"
#include "../gvdb_samples/tests/VelocityFixtures.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using ethereal::slang::ComputeRuntime;
using namespace gvdb::slang;
namespace {
void requireExtended(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
void expectClose(float actual,float expected,const char* message,float tolerance=2e-5f) {
    if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance*std::max(1.f,std::abs(expected))) {
        std::fprintf(stderr,"%s: expected %.9g, actual %.9g\n",message,double(expected),double(actual));
        throw std::runtime_error(message);
    }
}
void expectVector(Float4 actual,Float4 expected,const char* message,float tolerance=2e-5f) {
    expectClose(actual.x,expected.x,message,tolerance);expectClose(actual.y,expected.y,message,tolerance);
    expectClose(actual.z,expected.z,message,tolerance);expectClose(actual.w,expected.w,message,tolerance);
}
template<class T> std::vector<T> readBuffer(ComputeRuntime& runtime,nvrhi::IBuffer* buffer,size_t count) {
    std::vector<T> result(count);if(count)runtime.read(buffer,result.data(),count*sizeof(T));return result;
}
PackedVolume packedFixture(const fixtures::SparseVolume& fixture) {
    PackedVolume result;std::string error;
    if(!packVolume(fixtures::volumeSource(fixture),result,error))throw std::runtime_error(error);
    return result;
}
void scalarHelpers(ComputeRuntime& runtime) {
    const auto fixture=fixtures::twoBrickVolume();const auto packed=packedFixture(fixture);
    SlangVolume volume(runtime,packed,fixture.atlas.data(),fixture.atlas.size());
    std::vector<float> dense(16);
    for(uint32_t z=0;z<2;++z)for(uint32_t y=0;y<2;++y)for(uint32_t x=0;x<4;++x)
        dense[(z*2+y)*4+x]=float(x+10*y+100*z);
    auto source=runtime.buffer(dense.size()*4,4,dense.data());
    const std::array<Float4,3> identity={Float4{1,0,0,0},Float4{0,1,0,0},Float4{0,0,1,0}};
    volume.resample(source,{4,2,2,0},identity,{0,1,0,1},-7);
    const auto atlas=volume.readAtlas();
    for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<8;++x) {
        const int wx=x<4?x-1:x-3,wy=y-1,wz=z-1;
        const float expected=wx>=0&&wx<4&&wy>=0&&wy<2&&wz>=0&&wz<2?float(wx+10*wy+100*wz):-7;
        expectClose(atlas[size_t((z*4+y)*8+x)],expected,"sparse resample including aprons");
    }
    expectClose(volume.interiorSum(),904,"sparse interior reduction");
    const std::array<Float4,3> scale={Float4{2,0,0,0},Float4{0,2,0,0},Float4{0,0,2,0}};
    const auto downsampled=downsampleDense(runtime,source,{4,2,2,0},{2,1,1,0},scale,{0,1,0,1});
    requireExtended(downsampled.size()==2,"downsample dimensions");
    expectClose(downsampled[0],55.5f,"downsample first source box");
    expectClose(downsampled[1],57.5f,"downsample second source box");

    std::vector<float> sparse(5*3*2,0);sparse[1]=7;sparse[2]=11;sparse[(1*3+2)*5+4]=13;
    TopologyBuildOptions options;options.logDimensions={1,1,1};options.atlasGrid={3,1,1,0};
    ScalarVolumeData built;std::string error;
    if(!buildScalarVolume(sparse.data(),sparse.size(),{5,3,2,0},options,0,0,built,error))throw std::runtime_error(error);
    requireExtended(built.packed.levels[0].nodes.z==3,"CPU sparse topology leaf count");
    SlangVolume builtVolume(runtime,built.packed,built.atlas.data(),built.atlas.size());
    expectClose(builtVolume.interiorSum(),31,"CPU topology to GPU scalar atlas");
    builtVolume.updateApron();
    const auto updated=builtVolume.readAtlas();
    requireExtended(updated==built.atlas,"CPU and GPU topology apron population differ");
    std::puts("PASS scalar resampling/downsampling, interior reduction and CPU-built sparse topology");
}
void velocityTransfer(ComputeRuntime& runtime) {
    const auto fixture=fixtures::twoBrickVolume();const auto packed=packedFixture(fixture);
    SlangVolume volume(runtime,packed,fixture.atlas.data(),fixture.atlas.size());
    const auto fields=fixtures::macVelocityFields();
    std::array<nvrhi::BufferHandle,3> currentBuffers,oldBuffers;
    std::array<nvrhi::IBuffer*,3> current{},previous{};
    for(size_t c=0;c<3;++c) {
        std::vector<float> old(128,float(c+1));
        currentBuffers[c]=runtime.buffer(fields[c].size()*4,4,fields[c].data());
        oldBuffers[c]=runtime.buffer(old.size()*4,4,old.data());current[c]=currentBuffers[c];previous[c]=oldBuffers[c];
    }
    const auto positions=fixtures::velocityPositions();std::vector<Float4> velocities(positions.size(),{2,3,4,17});
    auto pointBuffer=runtime.buffer(positions.size()*16,16,positions.data());
    auto velocityBuffer=runtime.buffer(velocities.size()*16,16,velocities.data());
    constexpr float dt=.01f;
    const auto output=transferGridVelocity(runtime,volume,current,previous,pointBuffer,velocityBuffer,uint32_t(positions.size()),dt,.25f,true);
    const auto sampled=readBuffer<Float4>(runtime,output.sampledGrid,positions.size());
    const auto moved=readBuffer<Float4>(runtime,output.positions,positions.size());
    const auto transferred=readBuffer<Float4>(runtime,output.velocities,positions.size());
    for(size_t i=0;i<2;++i) {
        auto grid=fixtures::affineVelocity(positions[i]);grid.w=1;
        expectVector(sampled[i],grid,"staggered MAC sampling across sparse seam");
        expectVector(transferred[i],{grid.x+.25f,grid.y+.25f,grid.z+.25f,17},"PIC/FLIP interpolation");
        expectVector(moved[i],fixtures::midpointPosition(positions[i],dt),"midpoint grid advection");
    }
    expectVector(sampled[2],{},"out-of-volume grid sample");
    expectVector(transferred[2],velocities[2],"out-of-volume velocity preserved");
    expectVector(moved[2],positions[2],"out-of-volume position preserved");
    std::puts("PASS staggered MAC interpolation, PIC/FLIP velocity transfer and midpoint advection");
}
void subcellBinning(ComputeRuntime& runtime) {
    const auto sparse=fixtures::twoBrickVolume();const auto packed=packedFixture(sparse);fixtures::PointGatherFixture fixture;
    auto points=runtime.buffer(fixture.positions.size()*16,16,fixture.positions.data());
    auto colors=runtime.buffer(fixture.colors.size()*4,4,fixture.colors.data());
    const auto bins=binPointSubcells(runtime,packed,points,uint32_t(fixture.positions.size()),2,fixture.radius,{},colors);
    requireExtended(bins.cells.size()==2&&bins.pointCount==4,"subcell support expansion counts");
    const auto expanded=readBuffer<Float4>(runtime,bins.positions,bins.pointCount);
    const auto expandedColors=readBuffer<uint32_t>(runtime,bins.colors,bins.pointCount);
    for(const auto& cell:bins.cells) {
        requireExtended(cell.points.y==2,"subcell point count");
        bool found[2]={false,false};
        for(uint32_t j=0;j<cell.points.y;++j) {
            const uint32_t index=cell.points.x+j;const uint32_t source=expanded[index].x<1?0u:1u;
            requireExtended(!found[source],"subcell duplicated point within one cell");found[source]=true;
            expectVector(expanded[index],fixture.positions[source],"subcell position scatter");
            requireExtended(expandedColors[index]==fixture.colors[source],"subcell color follows scattered point");
        }
    }
    SlangPointFusion fusion(runtime,packed.volume.atlasResolutionAndApron,bins.cells,bins.positions,bins.pointCount,bins.colors);
    for(bool levelSet:{false,true}) {
        fusion.gather(fixture.radius,levelSet);const auto actual=fusion.readDensity();const auto reference=fixtures::gatheredValues(fixture,levelSet);
        for(uint32_t z=0;z<4;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<8;++x) {
            float expected=levelSet?3.f:0.f;
            if(x%4>=1&&x%4<=2&&y>=1&&y<=2&&z>=1&&z<=2) {
                const uint32_t wx=x<4?x-1:x-3;expected=reference[((z-1)*2+y-1)*4+wx];
            }
            expectClose(actual[(z*4+y)*8+x],expected,"subcell binning to sparse point fusion");
        }
    }
    const auto small=binPointSubcells(runtime,packed,points,uint32_t(fixture.positions.size()),2,0);
    requireExtended(small.pointCount==2&&small.cells[0].points.y==1&&small.cells[1].points.y==1,"zero-radius subcell coverage");
    const Float4 outside{-100,-100,-100,1};auto outsideBuffer=runtime.buffer(16,16,&outside);
    const auto empty=binPointSubcells(runtime,packed,outsideBuffer,1,2,.5f);
    requireExtended(empty.pointCount==0&&empty.cells.size()==2,"empty subcell expansion");
    SlangPointFusion emptyFusion(runtime,packed.volume.atlasResolutionAndApron,empty.cells,empty.positions,0);
    emptyFusion.gather(.5f);const auto emptyDensity=emptyFusion.readDensity();
    requireExtended(std::all_of(emptyDensity.begin(),emptyDensity.end(),[](float v){return v==0;}),"empty fusion initialization");
    std::puts("PASS GPU subcell count/scan/scatter, colors, support expansion and sparse density/level-set fusion");
}
void pointConversion(ComputeRuntime& runtime) {
    auto parameters=[](uint32_t count,uint32_t bytes,uint32_t offset,uint32_t stride) {
        PointConversionParameters p{};p.source={count,bytes,offset,stride};p.delta={1,1,1,0};p.scale={1,1,1,0};return p;
    };
    const auto doubles=fixtures::doublePointComponents();auto doubleSource=runtime.buffer(doubles.size()*8,0,doubles.data(),true);
    const auto doubleParameters=parameters(5,8,0,24);
    const auto converted=convertPackedPoints(runtime,doubleSource,doubleParameters);
    const auto actual=readBuffer<Float4>(runtime,converted,5),expected=fixtures::convertedDoublePoints(doubles,doubleParameters);
    requireExtended(std::memcmp(actual.data(),expected.data(),actual.size()*16)==0,"software double conversion round-to-nearest-even");

    std::vector<uint8_t> bytes(24,0);
    for(uint32_t i=0;i<5;++i)for(uint32_t c=0;c<3;++c)bytes[1+i*5+c]=uint8_t(i+c);
    auto byteSource=runtime.buffer(bytes.size(),0,bytes.data(),true);auto byteParameters=parameters(5,1,1,5);
    byteParameters.minimum={1,2,3,0};byteParameters.delta={.5f,2,-1,0};byteParameters.scale={2,.25f,1,0};byteParameters.translation={-1,1,0,0};
    const auto byteOutput=readBuffer<Float4>(runtime,convertPackedPoints(runtime,byteSource,byteParameters),5);
    for(uint32_t i=0;i<5;++i)expectVector(byteOutput[i],{float(i+1),float(i)*.5f+2.f,1.f-float(i),1},"unaligned byte point affine conversion");

    std::vector<uint16_t> shorts(20,0);
    for(uint32_t i=0;i<5;++i)for(uint32_t c=0;c<3;++c)shorts[1+i*4+c]=uint16_t((i*7919+c*16381)%65536);
    shorts[19]=65535;auto shortSource=runtime.buffer(shorts.size()*2,0,shorts.data(),true);
    auto shortParameters=parameters(5,2,2,8);shortParameters.delta={1.f/65535,1.f/65535,1.f/65535,0};
    const auto shortOutput=readBuffer<Float4>(runtime,convertPackedPoints(runtime,shortSource,shortParameters),5);
    for(uint32_t i=0;i<5;++i)expectVector(shortOutput[i],{shorts[1+i*4]/65535.f,shorts[2+i*4]/65535.f,shorts[3+i*4]/65535.f,1},"ushort point normalization");

    const Float4 minimum{-1,-2,-4,0},range{2,4,8,0};
    const std::vector<Float4> floatPoints={{-1,-2,-4,13},{1,2,4,17},{0,0,0,19},{2,-3,std::numeric_limits<float>::quiet_NaN(),23},{.25f,1,-2,29}};
    auto floatSource=runtime.buffer(floatPoints.size()*16,0,floatPoints.data(),true);
    const auto floatOutput=readBuffer<Float4>(runtime,convertPackedPoints(runtime,floatSource,parameters(5,4,0,16)),5);
    for(size_t i=0;i<floatPoints.size();++i) {
        expectClose(floatOutput[i].x,floatPoints[i].x,"float point x");expectClose(floatOutput[i].y,floatPoints[i].y,"float point y");
        if(std::isnan(floatPoints[i].z))requireExtended(std::isnan(floatOutput[i].z),"float point NaN preserved");
        else expectClose(floatOutput[i].z,floatPoints[i].z,"float point z");
        expectClose(floatOutput[i].w,1,"float point homogeneous coordinate");
    }
    auto pointBuffer=runtime.buffer(floatPoints.size()*16,16,floatPoints.data());
    auto quantized=quantizePointsU16(runtime,pointBuffer,5,minimum,range);
    const auto quantizedActual=readBuffer<uint16_t>(runtime,quantized,16),quantizedExpected=fixtures::quantizedPoints(floatPoints,minimum,range);
    for(size_t i=0;i<quantizedExpected.size();++i)if(quantizedActual[i]!=quantizedExpected[i]) { std::fprintf(stderr,"quantized component %zu: expected %u, actual %u\n",i,unsigned(quantizedExpected[i]),unsigned(quantizedActual[i]));throw std::runtime_error("packed ushort3 quantization, clamping and NaNs"); }
    requireExtended(quantizedActual.back()==0,"odd ushort3 padding is zero");
    auto decodeParameters=parameters(5,2,0,6);decodeParameters.minimum=minimum;decodeParameters.delta={range.x/65535,range.y/65535,range.z/65535,0};
    const auto decoded=readBuffer<Float4>(runtime,convertPackedPoints(runtime,quantized,decodeParameters),5);
    for(uint32_t i=0;i<5;++i)expectVector(decoded[i],{minimum.x+quantizedExpected[i*3]*decodeParameters.delta.x,
        minimum.y+quantizedExpected[i*3+1]*decodeParameters.delta.y,minimum.z+quantizedExpected[i*3+2]*decodeParameters.delta.z,1},"packed ushort3 roundtrip",2e-6f);
    bool rejected=false;try{convertPackedPoints(runtime,floatSource,parameters(5,4,0,10));}catch(const std::invalid_argument&){rejected=true;}
    requireExtended(rejected,"misaligned point stride rejection");
    requireExtended(!convertPackedPoints(runtime,nullptr,parameters(0,4,0,12)),"empty point conversion");
    std::puts("PASS byte/ushort/float/software-double point conversion and packed ushort3 quantization");
}
} // namespace
void runExtendedComputeTests(ComputeRuntime& runtime) {
    scalarHelpers(runtime);velocityTransfer(runtime);subcellBinning(runtime);pointConversion(runtime);
}
