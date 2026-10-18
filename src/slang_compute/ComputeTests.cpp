// SPDX-License-Identifier: Apache-2.0
#include "ComputeRuntime.h"
#include "../gvdb_samples/tests/PortableFixtures.h"
#include "../gvdb_samples/tests/RawAtlasFixtures.h"
#include "../gvdb_samples/tests/PointFixtures.h"
#include "../gvdb_samples/tests/VoxelizeFixtures.h"
#include "../gvdb_samples/gvdb/GVDBSlangVoxelizer.h"
#include "../gvdb_samples/gvdb/GVDBSlangPointKeys.h"
#include "../gvdb_samples/gvdb/GVDBSlangPointFusion.h"
#include "../gvdb_samples/gvdb/GVDBSlangAtlas.h"
#include "../gvdb_samples/gvdb/GVDBSlangSph.h"
#include "../gvdb_samples/gvdb/GVDBSlangVolume.h"
#include "../gvdb_samples/gvdb/slang/GVDBPortable.h"
#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <donut/core/log.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <stdexcept>
using namespace nvrhi;
using ethereal::slang::ComputeRuntime;
void runExtendedComputeTests(ComputeRuntime& runtime);
namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void algorithms(ComputeRuntime& runtime) {
    require(!runtime.exclusiveScan(nullptr,0),"empty scan");
    for(uint32_t count:{1u,255u,256u,257u,1025u,65539u}) {
        std::vector<uint32_t> input(count),expected(count),actual(count);
        for(uint32_t i=0;i<count;++i)input[i]=(i*17+3)%31;
        std::exclusive_scan(input.begin(),input.end(),expected.begin(),0u);
        auto buffer=runtime.buffer(size_t(count)*4,4,input.data());
        auto result=runtime.exclusiveScan(buffer,count);runtime.read(result,actual.data(),size_t(count)*4);
        if(actual!=expected) {
            for(uint32_t i=0;i<count;++i)if(actual[i]!=expected[i]) {
                std::fprintf(stderr,"scan count=%u index=%u expected=%u actual=%u\n",count,i,expected[i],actual[i]);break;
            }
            throw std::runtime_error("recursive exclusive scan differs from CPU");
        }
    }
    puts("PASS recursive scan, including partial groups and three hierarchy levels");
    std::vector<gvdb::slang::KeyValue> input(515);
    for(uint32_t i=0;i<input.size();++i) {
        input[i].key={uint32_t((i*97)%19),uint32_t((i*7)%5)};
        input[i].value=i;input[i].reserved=0;
    }
    input[0].key={UINT32_MAX,UINT32_MAX};input[1].key={0,0};
    input[2].key={0,0x80000000u};
    auto expected=input,actual=input;
    std::stable_sort(expected.begin(),expected.end(),[](const auto& a,const auto& b) {
        return gvdb::slang::keyLess(a.key,b.key);
    });
    auto keys=runtime.buffer(input.size()*sizeof(input[0]),sizeof(input[0]),input.data());
    auto sorted=runtime.radixSort(keys,uint32_t(input.size()));
    runtime.read(sorted,actual.data(),actual.size()*sizeof(actual[0]));
    for(size_t i=0;i<actual.size();++i)
        require(actual[i].key.x==expected[i].key.x&&actual[i].key.y==expected[i].key.y&&actual[i].value==expected[i].value,
            "stable 64-bit radix sort differs from CPU");
    puts("PASS stable uint2 radix sort, duplicate keys and upper 32-bit ordering");
    std::vector<float> values(1027);
    gvdb::slang::Reduction reduction{INFINITY,-INFINITY,0,0},actualReduction{};
    for(uint32_t i=0;i<values.size();++i) {
        values[i]=i%17==0?NAN:float(int(i%47)-23)*0.25f;
        if(!std::isnan(values[i])) {
            reduction.minimum=std::min(reduction.minimum,values[i]);reduction.maximum=std::max(reduction.maximum,values[i]);
            reduction.sum+=values[i];++reduction.count;
        }
    }
    auto valueBuffer=runtime.buffer(values.size()*4,4,values.data());
    auto reduced=runtime.reduce(valueBuffer,uint32_t(values.size()));
    runtime.read(reduced,&actualReduction,sizeof(actualReduction));
    require(actualReduction.minimum==reduction.minimum&&actualReduction.maximum==reduction.maximum&&
        actualReduction.sum==reduction.sum&&actualReduction.count==reduction.count,"reduction differs from CPU");
    auto empty=runtime.reduce(nullptr,0);runtime.read(empty,&actualReduction,sizeof(actualReduction));
    require(std::isinf(actualReduction.minimum)&&actualReduction.minimum>0&&std::isinf(actualReduction.maximum)&&
        actualReduction.maximum<0&&actualReduction.sum==0&&actualReduction.count==0,"empty reduction identity");
    puts("PASS multilevel min/max/sum/count reduction, NaNs and empty identity");
}
void operators(ComputeRuntime& runtime) {
    constexpr uint32_t side=10,count=side*side*side;
    std::vector<float> values(count),actual(count),expected(count);
    gvdb::slang::OperatorParameters params{};
    params.volume.atlasResolutionAndApron={side,side,side,1};
    params.volume.atlasBrickCountAndWidth={1,1,1,side};
    params.value={3.25f,0,0,0};
    auto output=runtime.buffer(count*4,4);
    runtime.dispatch("GVDBOperators","fillAtlas",&params,sizeof(params),{BindingSetItem::StructuredBuffer_UAV(0,output)},2,2,2);
    runtime.read(output,actual.data(),count*4);
    require(std::all_of(actual.begin(),actual.end(),[](float value){return value==3.25f;}),"fillAtlas");
    for(uint32_t i=0;i<count;++i)values[i]=float((i*71)%101)*0.125f;
    expected=values;params.value={2,0.25f,0,0};
    for(uint32_t z=1;z+1<side;++z)for(uint32_t y=1;y+1<side;++y)for(uint32_t x=1;x+1<side;++x) {
        const uint32_t i=x+side*(y+side*z);
        expected[i]=(2*values[i]+values[i-1]+values[i+1]+values[i-side]+values[i+side]+values[i-side*side]+values[i+side*side])/8+0.25f;
    }
    auto source=runtime.buffer(count*4,4,values.data());
    runtime.dispatch("GVDBOperators","smoothAtlas",&params,sizeof(params),{
        BindingSetItem::StructuredBuffer_SRV(0,source),BindingSetItem::StructuredBuffer_UAV(0,output)},2,2,2);
    runtime.read(output,actual.data(),count*4);
    for(uint32_t i=0;i<count;++i)require(std::abs(actual[i]-expected[i])<1e-5f,"smoothAtlas interior/apron");
    runtime.dispatch("GVDBOperators","clearAtlas",&params,sizeof(params),{BindingSetItem::StructuredBuffer_UAV(0,output)},2,2,2);
    runtime.read(output,actual.data(),count*4);
    require(std::all_of(actual.begin(),actual.end(),[](float value){return value==0;}),"clearAtlas");
    puts("PASS volume fill, stencil and clear with partial thread groups and apron preservation");
}

void sparseVolume(ComputeRuntime& runtime) {
    namespace g=gvdb::slang;
    namespace f=gvdb::slang::fixtures;
    auto fixture=f::twoBrickVolume();
    g::VolumeSource source;source.volume=fixture.volume;
    source.atlasMap=fixture.atlasMap.data();source.atlasMapCount=fixture.atlasMap.size();
    for(const auto& level:fixture.levels) {
        g::LevelSource l;
        l.logDimension=level.nodes.w;
        l.nodes={fixture.nodes.data()+level.nodes.x/4,level.nodes.z,level.nodes.y};
        l.children={fixture.children.data()+level.children.x/4,level.children.z,level.children.y};
        source.levels.push_back(l);
    }
    g::PackedVolume packed;std::string error;
    if(!g::packVolume(source,packed,error))throw std::runtime_error(error);
    g::SlangVolume volume(runtime,packed,fixture.atlas.data(),fixture.atlas.size());
    auto render=volume.raycast(f::raycastParameters(fixture),{{1,0.5f,0.25f,0.5f},{1,0.5f,0.25f,0.5f}});
    g::Float4 color{};runtime.read(render.color,&color,sizeof(color));
    const auto expected=f::raycastExpected();
    require(std::abs(color.x-expected.x)<0.015f && std::abs(color.y-expected.y)<0.015f &&
        std::abs(color.z-expected.z)<0.015f && std::abs(color.w-expected.w)<0.015f,"sparse raycast Beer-Lambert reference");
    volume.fill(2);volume.updateApron(0);
    const auto atlas=volume.readAtlas();
    // Neighbor-facing apron samples come from the other leaf; outer boundaries are zero.
    require(atlas[3+8*(1+4)]==2 && atlas[4+8*(1+4)]==2,"neighbor apron exchange");
    require(atlas[0]==0 && atlas[1+8*(1+4)]==2,"outer apron and leaf interior");
    volume.clear();const auto clear=volume.readAtlas();
    require(std::all_of(clear.begin(),clear.end(),[](float v){return v==0;}),"SlangVolume clear");
    puts("PASS packed sparse hierarchy, neighboring aprons and analytic volume raycast");

    auto nodes=runtime.buffer(fixture.nodes.size()*4,0,fixture.nodes.data(),true);
    auto children=runtime.buffer(fixture.children.size()*4,0,fixture.children.data(),true);
    auto levels=runtime.buffer(fixture.levels.size()*sizeof(g::LevelInfo),sizeof(g::LevelInfo),fixture.levels.data());
    uint32_t markers[2]={1,0};auto markerBuffer=runtime.buffer(sizeof(markers),4,markers);
    g::TopologyParameters params{fixture.volume,{0,0,0,0}};
    std::vector<BindingSetItem> bindings={BindingSetItem::StructuredBuffer_SRV(0,levels),
        BindingSetItem::StructuredBuffer_SRV(1,markerBuffer),BindingSetItem::RawBuffer_UAV(0,nodes),
        BindingSetItem::RawBuffer_UAV(1,children)};
    runtime.dispatch("GVDBTopology","applyLeafMarkers",&params,sizeof(params),bindings,1);
    runtime.dispatch("GVDBTopology","delinkLeafBricks",&params,sizeof(params),bindings,1);
    std::vector<uint32_t> actualChildren(fixture.children.size());
    runtime.read(children,actualChildren.data(),actualChildren.size()*4);
    require(actualChildren[0]==fixture.children[0] && actualChildren[1]==fixture.children[1] &&
        actualChildren[2]==UINT32_MAX && actualChildren[3]==UINT32_MAX,"topology delink");
    markers[1]=1;runtime.upload(markerBuffer,markers,sizeof(markers));
    runtime.dispatch("GVDBTopology","applyLeafMarkers",&params,sizeof(params),bindings,1);
    runtime.dispatch("GVDBTopology","linkBricks",&params,sizeof(params),bindings,1);
    runtime.read(children,actualChildren.data(),actualChildren.size()*4);
    require(actualChildren==fixture.children,"topology relink");
    puts("PASS leaf activation, parent unlink and hierarchy relink");
}
void sph(ComputeRuntime& runtime) {
    namespace g=gvdb::slang;
    namespace f=gvdb::slang::fixtures;
    const auto positions=f::sphPositions();
    const std::vector<g::Float4> zeros(positions.size());
    auto p=f::sphParameters();
    auto points=runtime.buffer(positions.size()*sizeof(g::Float4),sizeof(g::Float4),positions.data());
    auto velocities=runtime.buffer(zeros.size()*sizeof(g::Float4),sizeof(g::Float4),zeros.data());
    auto counts=runtime.buffer(27*4,4),cells=runtime.buffer(2*4,4),localOffsets=runtime.buffer(2*4,4);
    auto indices=runtime.buffer(2*4,4),density=runtime.buffer(2*4,4),forces=runtime.buffer(2*sizeof(g::Float4),sizeof(g::Float4));
    runtime.dispatch("GVDBSph","clearGrid",&p,sizeof(p),{BindingSetItem::StructuredBuffer_UAV(6,counts)},1);
    runtime.dispatch("GVDBSph","insertGrid",&p,sizeof(p),{BindingSetItem::StructuredBuffer_SRV(0,points),
        BindingSetItem::StructuredBuffer_UAV(5,cells),BindingSetItem::StructuredBuffer_UAV(6,counts),
        BindingSetItem::StructuredBuffer_UAV(7,localOffsets)},1);
    auto offsets=runtime.exclusiveScan(counts,27);
    runtime.dispatch("GVDBSph","scatterGrid",&p,sizeof(p),{BindingSetItem::StructuredBuffer_SRV(3,cells),
        BindingSetItem::StructuredBuffer_SRV(5,offsets),BindingSetItem::StructuredBuffer_SRV(9,localOffsets),
        BindingSetItem::StructuredBuffer_UAV(5,indices)},1);
    std::vector<BindingSetItem> common={BindingSetItem::StructuredBuffer_SRV(0,points),
        BindingSetItem::StructuredBuffer_SRV(1,velocities),BindingSetItem::StructuredBuffer_SRV(2,velocities),
        BindingSetItem::StructuredBuffer_SRV(3,cells),BindingSetItem::StructuredBuffer_SRV(4,counts),
        BindingSetItem::StructuredBuffer_SRV(5,offsets),BindingSetItem::StructuredBuffer_SRV(6,indices)};
    auto densityBindings=common;densityBindings.push_back(BindingSetItem::StructuredBuffer_UAV(0,density));
    runtime.dispatch("GVDBSph","computeDensity",&p,sizeof(p),densityBindings,1);
    common.push_back(BindingSetItem::StructuredBuffer_SRV(7,density));
    auto forceBindings=common;forceBindings.push_back(BindingSetItem::StructuredBuffer_UAV(1,forces));
    runtime.dispatch("GVDBSph","computeForce",&p,sizeof(p),forceBindings,1);
    float actualDensity[2]{};g::Float4 actualForces[2]{};
    runtime.read(density,actualDensity,sizeof(actualDensity));runtime.read(forces,actualForces,sizeof(actualForces));
    for(auto d:actualDensity)require(std::abs(d-f::sphExpectedDensity())<1e-5f,"SPH density");
    require(std::abs(actualForces[0].x-f::sphExpectedForce())<1e-5f &&
        std::abs(actualForces[1].x+f::sphExpectedForce())<1e-5f,"SPH equal and opposite forces");
    auto nextPoints=runtime.buffer(sizeof(actualForces),sizeof(g::Float4));
    auto nextVelocities=runtime.buffer(sizeof(actualForces),sizeof(g::Float4));
    auto evaluated=runtime.buffer(sizeof(actualForces),sizeof(g::Float4));
    common.push_back(BindingSetItem::StructuredBuffer_SRV(8,forces));
    common.push_back(BindingSetItem::StructuredBuffer_UAV(2,nextPoints));
    common.push_back(BindingSetItem::StructuredBuffer_UAV(3,nextVelocities));
    common.push_back(BindingSetItem::StructuredBuffer_UAV(4,evaluated));
    runtime.dispatch("GVDBSph","advanceParticles",&p,sizeof(p),common,1);
    g::Float4 actualPoints[2]{},actualVelocities[2]{},actualEvaluated[2]{};
    runtime.read(nextPoints,actualPoints,sizeof(actualPoints));runtime.read(nextVelocities,actualVelocities,sizeof(actualVelocities));
    runtime.read(evaluated,actualEvaluated,sizeof(actualEvaluated));
    for(unsigned i=0;i<2;++i) {
        const float velocity=actualForces[i].x*p.integration.y;
        require(std::abs(actualVelocities[i].x-velocity)<1e-6f &&
            std::abs(actualPoints[i].x-positions[i].x-velocity*p.integration.y)<1e-6f &&
            std::abs(actualEvaluated[i].x-velocity*0.5f)<1e-6f,"SPH leapfrog integration");
    }
    puts("PASS SPH cell scan/scatter, analytic density/force and fixed-step integration");
}



void rawAtlases(ComputeRuntime& runtime) {
    namespace g=gvdb::slang;
    namespace f=gvdb::slang::fixtures;
    auto fixture=f::twoBrickVolume();g::PackedVolume packed;std::string error;
    require(g::packVolume(f::volumeSource(fixture),packed,error),"raw atlas fixture packing");
    auto compare=[&](const std::vector<uint8_t>& actual,const std::vector<uint8_t>& expected,g::AtlasFormat format) {
        require(actual.size()==expected.size(),"raw atlas size");
        if(uint32_t(format)<2)require(actual==expected,"packed byte atlas differs from CPU");
        else for(size_t voxel=0;voxel<128;++voxel) {
            const auto a=f::readRaw(actual,format,voxel),e=f::readRaw(expected,format,voxel);
            for(size_t component=0;component<g::atlasStride(format)/4;++component)
                require(std::abs(a[component]-e[component])<1e-5f,"float vector atlas differs from CPU");
        }
    };
    for(auto format:{g::AtlasFormat::R8_UINT,g::AtlasFormat::RGBA8_UINT,g::AtlasFormat::R32_FLOAT,
                     g::AtlasFormat::RGB32_FLOAT,g::AtlasFormat::RGBA32_FLOAT}) {
        auto input=f::patternedRawAtlas(format);
        g::SlangAtlas atlas(runtime,packed,format,input.data(),input.size());
        atlas.updateApron(3);
        compare(atlas.read(),f::rawApron(input,format,3),format);
        if(uint32_t(format)>=2) {
            atlas.smooth(2,0.25f,1,3);
            compare(atlas.read(),f::rawApron(f::rawSmooth(f::rawApron(input,format,3),format,2,0.25f),format,3),format);
        }
        atlas.fill({0.25f,0.5f,0.75f,0});
        const auto filled=atlas.read();
        const std::array<float,4> expected=format==g::AtlasFormat::R8_UINT?std::array<float,4>{0,0,0,0}:
            format==g::AtlasFormat::RGBA8_UINT?std::array<float,4>{63,127,191,255}:
            std::array<float,4>{0.25f,0.5f,0.75f,1};
        for(size_t voxel=0;voxel<128;++voxel) {
            const auto value=f::readRaw(filled,format,voxel);
            const size_t components=format==g::AtlasFormat::R8_UINT?1:
                format==g::AtlasFormat::RGBA8_UINT?4:g::atlasStride(format)/4;
            for(size_t c=0;c<components;++c)require(value[c]==expected[c],"raw atlas fill components");
        }
        atlas.clear();const auto cleared=atlas.read();
        require(std::all_of(cleared.begin(),cleared.end(),[](uint8_t x){return x==0;}),"raw atlas clear");
    }
    std::vector<uint8_t> bytes(128,0);bytes[f::fixtureIndex(2,1,1)]=7;
    g::SlangAtlas labels(runtime,packed,g::AtlasFormat::R8_UINT,bytes.data(),bytes.size());
    labels.expandByte(7,19);
    compare(labels.read(),f::rawApron(f::byteExpansion(f::rawApron(bytes,g::AtlasFormat::R8_UINT,0),7,19),
        g::AtlasFormat::R8_UINT,0),g::AtlasFormat::R8_UINT);
    auto colors=f::patternedRawAtlas(g::AtlasFormat::RGBA8_UINT);
    g::SlangAtlas color(runtime,packed,g::AtlasFormat::RGBA8_UINT,colors.data(),colors.size());
    color.expandColor(2,0.5f);
    compare(color.read(),f::rawApron(f::colorExpansion(f::rawApron(colors,g::AtlasFormat::RGBA8_UINT,0),2,0.5f),
        g::AtlasFormat::RGBA8_UINT,0),g::AtlasFormat::RGBA8_UINT);
    puts("PASS five raw atlas formats, vector stencils, byte dilation and color normalization");
}

void sphSimulation(ComputeRuntime& runtime) {
    namespace g=gvdb::slang;namespace f=gvdb::slang::fixtures;
    auto p=f::sphParameters();auto points=f::sphPositions();
    points[0].w=17;points[1].w=23;
    g::SlangSphSimulation simulation(runtime,p,points);
    simulation.step();
    auto positions=simulation.readPositions(),velocities=simulation.readVelocities();
    const float v=f::sphExpectedForce()*p.integration.y;
    require(std::abs(positions[0].x-(1+v*p.integration.y))<1e-6f&&
        std::abs(positions[1].x-(2-v*p.integration.y))<1e-6f,"SPH orchestration first position");
    require(std::abs(velocities[0].x-v)<1e-6f&&std::abs(velocities[1].x+v)<1e-6f,"SPH orchestration first velocity");
    // Repeated enqueued steps reuse both buffer sets and every grid cell.
    simulation.step(9);positions=simulation.readPositions();velocities=simulation.readVelocities();
    require(std::abs(positions[0].x+positions[1].x-3)<1e-5f&&
        std::abs(velocities[0].x+velocities[1].x)<1e-6f,"SPH conservation after buffer reuse");
    require(positions[0].w==17&&positions[1].w==23&&positions[0].x>1,"SPH particle data preservation");
    require(std::abs(simulation.parameters().integration.x-0.1f)<1e-6f,"SPH simulation clock");
    // A non-multiple of 256 particles with gravity and no inter-particle force
    // has an independent closed-form leapfrog result.
    p=f::sphParameters();p.kernels.w=0;p.kernels.z=0;p.gravityAndSlope={0,-0.5f,0,0};
    points.resize(257);
    for(size_t i=0;i<points.size();++i)points[i]={1+float(i%7)*0.001f,2+float(i%5)*0.001f,1,float(i)};
    g::SlangSphSimulation gravity(runtime,p,points);gravity.step(4);
    positions=gravity.readPositions();velocities=gravity.readVelocities();
    for(size_t i=0;i<points.size();++i)
        require(std::abs(velocities[i].y+0.02f)<1e-6f&&std::abs(positions[i].y-points[i].y+0.0005f)<1e-6f&&
            positions[i].w==float(i),"SPH partial-group gravity integration");
    puts("PASS production SPH orchestration, repeated steps, conservation and partial groups");
}


void pointFusion(ComputeRuntime& runtime) {
    namespace g=gvdb::slang;namespace f=gvdb::slang::fixtures;
    const auto points=f::brickKeyPositions();
    auto pointBuffer=runtime.buffer(points.size()*sizeof(g::Float4),sizeof(g::Float4),points.data());
    auto discovered=g::discoverBrickKeys(runtime,pointBuffer,uint32_t(points.size()),{2,4});
    const auto expectedKeys=f::uniqueBrickKeys(points,{2,4});
    require(discovered.count==expectedKeys.size()&&discovered.hasInvalidPositions&&
        discovered.levelCounts==std::vector<uint32_t>({3,3}),"point brick-key counts");
    std::vector<g::KeyValue> actualKeys(discovered.count);
    runtime.read(discovered.keys,actualKeys.data(),actualKeys.size()*sizeof(g::KeyValue));
    for(size_t i=0;i<actualKeys.size();++i)require(actualKeys[i].key.x==expectedKeys[i].key.x&&
        actualKeys[i].key.y==expectedKeys[i].key.y&&actualKeys[i].value==expectedKeys[i].value,"point key deduplication and stability");
    f::PointGatherFixture fixture;
    auto positions=runtime.buffer(fixture.positions.size()*sizeof(g::Float4),sizeof(g::Float4),fixture.positions.data());
    auto colors=runtime.buffer(fixture.colors.size()*4,4,fixture.colors.data());
    g::SlangPointFusion fusion(runtime,fixture.resolution,fixture.cells,positions,uint32_t(fixture.positions.size()),colors);
    for(bool levelSet:{false,true}) {
        fusion.gather(fixture.radius,levelSet,false);
        const auto actual=fusion.readDensity(),expected=f::gatheredValues(fixture,levelSet);
        for(size_t i=0;i<actual.size();++i)require(std::abs(actual[i]-expected[i])<1e-5f,"point density/level-set gather");
        const auto actualColors=fusion.readColor();
        for(auto color:actualColors)require(color==0x7e007f7fu,"point color arithmetic");
        fusion.gather(fixture.radius,levelSet,true);
        const auto accumulated=fusion.readDensity();
        for(size_t i=0;i<actual.size();++i)require(std::abs(accumulated[i]-expected[i])<1e-5f,"point gather accumulation");
    }
    auto overlap=fixture.cells;overlap.push_back(overlap.front());bool rejected=false;
    try {g::SlangPointFusion invalid(runtime,fixture.resolution,overlap,positions,2,colors);}
    catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"overlapping gather cells rejected");
    puts("PASS sparse point keys, invalid sentinels, deduplication and particle-to-volume gathers");
}

void voxelization(ComputeRuntime& runtime) {
    namespace g=gvdb::slang;namespace f=gvdb::slang::fixtures;
    f::VoxelizeFixture fixture;const auto expected=f::voxelizedBox();
    for(auto format:{g::VoxelFormat::R8_UINT,g::VoxelFormat::R32_FLOAT,g::VoxelFormat::R32_SINT}) {
        auto grid=g::voxelizeMesh(runtime,fixture.vertices,fixture.indices,fixture.parameters,format);
        std::vector<uint8_t> bytes(grid.byteSize);
        runtime.read(grid.values,bytes.data(),bytes.size());
        for(size_t i=0;i<expected.size();++i) {
            float value;
            if(format==g::VoxelFormat::R8_UINT)value=float(bytes[i]);
            else if(format==g::VoxelFormat::R32_FLOAT)std::memcpy(&value,bytes.data()+i*4,4);
            else {int32_t integer;std::memcpy(&integer,bytes.data()+i*4,4);value=float(integer);}
            require(value==expected[i],"solid voxelization surface/interior/exterior classification");
        }
    }
    puts("PASS binned solid voxelization, triangle boundaries and three output formats");
}

int run(IDevice* device) {
    const auto path=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_slang"/donut::app::GetShaderTypeName(device->getGraphicsAPI());
    ComputeRuntime runtime(device,path);algorithms(runtime);operators(runtime);sparseVolume(runtime);sph(runtime);rawAtlases(runtime);sphSimulation(runtime);pointFusion(runtime);voxelization(runtime);runExtendedComputeTests(runtime);return 0;
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();
    try {
#if ETHEREAL_TEST_CUDA
        for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"-cuda")==0) {
            cuda::DeviceDesc desc;desc.enableOptiX=false;
            auto device=cuda::createDevice(desc);require(bool(device),"CUDA device unavailable");return run(device);
        }
#endif
        const auto api=donut::app::GetGraphicsAPIFromCommandLine(argc,argv);
        auto manager=TakeOver(donut::app::DeviceManager::Create(api));require(bool(manager),"Backend unavailable");
        donut::app::DeviceCreationParameters params;params.enableNvrhiValidationLayer=true;
        require(manager->CreateHeadlessDevice(params),"Device creation failed");
        const int result=run(manager->GetDevice());manager->Shutdown();return result;
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
