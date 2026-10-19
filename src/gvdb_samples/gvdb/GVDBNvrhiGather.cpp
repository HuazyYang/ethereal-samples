// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiGather.h"
#include "GVDBNvrhiMutationInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <vector>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool condition,FRESULT result=FE_INVALID_ARGS) {if(!condition)throw Failure{result};}
void check(FRESULT result) {require(NVRHI_SUCCEEDED(result),result);}
template<class F> FRESULT guarded(F&& function) noexcept {
    try {function();return FS_OK;}
    catch(const Failure& failure){return failure.result;}
    catch(const std::bad_alloc&){return FE_OUT_OF_MEMORY;}
    catch(const std::length_error&){return FE_OUT_OF_MEMORY;}
    catch(...){return FE_GENERIC_ERROR;}
}
bool finite(Float4 value) {return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::isfinite(value.w);}
uint64_t multiply(uint64_t a,uint64_t b) {
    require(!a || b<=UINT64_MAX/a,FE_UNSUPPORTED);return a*b;
}
int64_t floorDivide(int64_t value,int64_t divisor) {
    const int64_t quotient=value/divisor,remainder=value%divisor;
    return quotient-(remainder<0?1:0);
}
struct Key {
    int32_t x,y,z;
    bool operator==(const Key& other) const {return x==other.x&&y==other.y&&z==other.z;}
};
struct KeyHash {
    size_t operator()(const Key& key) const {
        uint64_t hash=uint32_t(key.x)*uint64_t(0x9e3779b1u);
        hash=(hash^uint32_t(key.y))*0x85ebca6bu;
        return size_t((hash^uint32_t(key.z))*0xc2b2ae35u);
    }
};
struct alignas(16) Parameters {
    slang::UInt4 resolutionAndApron,brickCountAndWidth,work,options;
    Float4 translationAndRadius;
};
static_assert(sizeof(Parameters)==80);
void validateArtifact(IShaderProgram* program,uint32_t entry,const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;check(program->getEntryPointDesc(entry,desc));
    require(desc.entryParamsSize==0&&desc.threadGroupSize[0]==256&&desc.threadGroupSize[1]==1&&desc.threadGroupSize[2]==1,FE_UNSUPPORTED);
    const ResourceType types[]={ResourceType::ConstantBuffer,ResourceType::StructuredBuffer_SRV,
        ResourceType::RawBuffer_SRV,ResourceType::RawBuffer_SRV,ResourceType::StructuredBuffer_SRV,
        ResourceType::RawBuffer_SRV,ResourceType::RawBuffer_SRV,ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_UAV,ResourceType::RawBuffer_UAV};
    const uint32_t slots[]={0,0,1,2,3,4,5,6,0,1};
    const uint32_t strides[]={sizeof(Parameters),16,1,1,8,1,1,1,1,1};
    require(layout.bindings.size()==10,FE_UNSUPPORTED);
    for(uint32_t i=0;i<10;++i)require(std::count_if(layout.bindings.begin(),layout.bindings.end(),
        [&](const auto& item){return item.type==types[i]&&item.slot==slots[i];})==1,FE_UNSUPPORTED);
    uint32_t globals=0,resources=0,members=0,mask=0,constant=UINT32_MAX;
    for(uint32_t i=0;i<program->getParameterCount(entry);++i) {
        ShaderParameterDesc item;check(program->getParameterDesc(entry,i,item));
        require(item.registerSpace==0&&item.count==1,FE_UNSUPPORTED);
        if(item.scope==ShaderParameterScope::Global) {
            ++globals;require(item.parentParameter==UINT32_MAX,FE_UNSUPPORTED);uint32_t found=10;
            for(uint32_t j=0;j<10;++j)if(item.resourceType==types[j]&&item.registerIndex==slots[j])found=j;
            require(found<10,FE_UNSUPPORTED);
            require(!(resources&(1u<<found))&&item.elementStride==strides[found],FE_UNSUPPORTED);
            resources|=1u<<found;if(item.resourceType==ResourceType::ConstantBuffer)constant=i;
        } else {
            require(item.scope==ShaderParameterScope::ConstantBuffer&&item.resourceType==ResourceType::None&&
                item.offset<sizeof(Parameters)&&item.offset%16==0&&item.size==16&&item.elementStride==16,FE_UNSUPPORTED);
            const uint32_t bit=1u<<(item.offset/16);require(!(mask&bit),FE_UNSUPPORTED);mask|=bit;++members;
        }
    }
    require(globals==10&&resources==1023&&members==5&&mask==31&&constant!=UINT32_MAX,FE_UNSUPPORTED);
    for(uint32_t i=0;i<program->getParameterCount(entry);++i) {
        ShaderParameterDesc item;check(program->getParameterDesc(entry,i,item));
        if(item.scope==ShaderParameterScope::ConstantBuffer)require(item.parentParameter==constant,FE_UNSUPPORTED);
    }
}
struct Bins {std::vector<slang::UInt2> cells;std::vector<uint32_t> indices;uint32_t side=0,perLeaf=0;};
Bins buildBins(const slang::PackedVolume& topology,const GatherRequestDesc& request,
    const GatherPreparationBudget& budget,const detail::ResolvedPointSet& points) {
    require(!topology.levels.empty());
    const auto& leaves=topology.levels[0];const uint32_t width=request.subcellWidth;
    const uint32_t leafWidth=uint32_t(leaves.nodeExtent.x);
    const float supportRadius=request.supportRadius==-1.f?request.radius:request.supportRadius;
    require(leafWidth&&leafWidth%width==0,FE_UNSUPPORTED);
    Bins result;result.side=leafWidth/width;
    const uint64_t perLeaf=multiply(multiply(result.side,result.side),result.side);
    const uint64_t cells=multiply(leaves.nodes.z,perLeaf);
    require(cells<=budget.maxCells,FE_OUT_OF_MEMORY);
    require(cells&&cells<=c_AddressSpaceBytes/sizeof(slang::UInt2)&&cells<=SIZE_MAX/sizeof(slang::UInt2),FE_UNSUPPORTED);
    result.perLeaf=uint32_t(perLeaf);result.cells.resize(size_t(cells),{0,0});
    std::unordered_map<Key,uint32_t,KeyHash> active;
    std::array<int32_t,3> anchor{},phase{},lower{INT32_MAX,INT32_MAX,INT32_MAX},upper{INT32_MIN,INT32_MIN,INT32_MIN};
    bool haveAnchor=false;
    for(const auto& mapping:topology.atlasMap)if(mapping.leaf>=0) {
        require(uint32_t(mapping.leaf)<leaves.nodes.z);
        const std::array<int32_t,3> origin{mapping.x,mapping.y,mapping.z};
        if(!haveAnchor) {
            anchor=origin;haveAnchor=true;
            for(uint32_t axis=0;axis<3;++axis)phase[axis]=int32_t((int64_t(origin[axis])%width+width)%width);
        }
        for(uint32_t axis=0;axis<3;++axis) {
            require((int64_t(origin[axis])-anchor[axis])%leafWidth==0,FE_UNSUPPORTED);
            const int32_t first=int32_t((int64_t(origin[axis])-phase[axis])/width);
            lower[axis]=std::min(lower[axis],first);
            upper[axis]=std::max(upper[axis],int32_t(int64_t(first)+result.side-1));
        }
        require(active.emplace(Key{origin[0],origin[1],origin[2]},uint32_t(mapping.leaf)).second);
    }
    require(haveAnchor);
    require(points.info.count<=UINT32_MAX&&points.positionStream.byteStride<=UINT32_MAX,FE_UNSUPPORTED);
    const uint32_t count=uint32_t(points.info.count);
    uint64_t candidates=0,pairs=0;
    // Two deterministic passes avoid storing duplicate positions or a temporary pair
    // array. Budgets count logical support visits and inserted pairs once, not passes.
    auto enumerate=[&](bool countPass,auto&& visit) {
        for(uint32_t point=0;point<count;++point) {
            std::array<float,3> position;
            std::memcpy(position.data(),static_cast<const uint8_t*>(points.positions.data)+
                size_t(point)*size_t(points.positionStream.byteStride),12);
            position[0]+=request.translation.x;position[1]+=request.translation.y;position[2]+=request.translation.z;
            require(std::isfinite(position[0])&&std::isfinite(position[1])&&std::isfinite(position[2]));
            std::array<int32_t,3> first{},last{};bool intersects=true;
            for(uint32_t axis=0;axis<3;++axis) {
                const float low=position[axis]-supportRadius;
                const float plusOne=position[axis]+1.0f;
                const float high=plusOne+supportRadius;
                // Clip in floating point before conversion, including overflow to
                // infinity from otherwise finite position/radius inputs.
                const double begin=std::max(std::floor((double(low)-phase[axis])/width),double(lower[axis]));
                const double end=std::min(std::floor((double(high)-phase[axis])/width),double(upper[axis]));
                if(begin>end){intersects=false;break;}
                first[axis]=int32_t(begin);last[axis]=int32_t(end);
            }
            if(!intersects)continue;
            if(countPass) {
                const uint64_t visits=multiply(multiply(uint64_t(int64_t(last[0])-first[0]+1),
                    uint64_t(int64_t(last[1])-first[1]+1)),uint64_t(int64_t(last[2])-first[2]+1));
                require(visits<=budget.maxCandidateVisits-candidates,FE_OUT_OF_MEMORY);candidates+=visits;
            }
            for(int64_t z=first[2];z<=last[2];++z)for(int64_t y=first[1];y<=last[1];++y)for(int64_t x=first[0];x<=last[0];++x) {
                const std::array<int64_t,3> origin{x*width+phase[0],y*width+phase[1],z*width+phase[2]};
                std::array<int32_t,3> leaf;
                for(uint32_t axis=0;axis<3;++axis)leaf[axis]=int32_t(anchor[axis]+
                    floorDivide(origin[axis]-anchor[axis],leafWidth)*leafWidth);
                const auto found=active.find({leaf[0],leaf[1],leaf[2]});if(found==active.end())continue;
                const uint32_t localX=uint32_t((origin[0]-leaf[0])/width),localY=uint32_t((origin[1]-leaf[1])/width),localZ=uint32_t((origin[2]-leaf[2])/width);
                const uint32_t cell=found->second*result.perLeaf+(localZ*result.side+localY)*result.side+localX;
                if(countPass) {
                    require(pairs<budget.maxPointPairs,FE_OUT_OF_MEMORY);
                    require(pairs<c_AddressSpaceBytes/sizeof(uint32_t),FE_UNSUPPORTED);++pairs;
                }
                visit(cell,point);
            }
        }
    };
    enumerate(true,[&](uint32_t cell,uint32_t){++result.cells[cell].y;});
    uint64_t offset=0;
    for(auto& cell:result.cells){cell.x=uint32_t(offset);offset+=cell.y;cell.y=0;}
    require(offset==pairs&&pairs<=SIZE_MAX/sizeof(uint32_t),FE_UNSUPPORTED);
    result.indices.resize(size_t(std::max(uint64_t(1),pairs)),0);
    enumerate(false,[&](uint32_t cell,uint32_t point){auto& range=result.cells[cell];result.indices[range.x+range.y++]=point;});
    return result;
}
} // namespace
struct NvrhiGather::Impl {
    DeviceHandle device;
    NvrhiVolume* owner=nullptr;
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
NvrhiGather::NvrhiGather(std::unique_ptr<Impl> impl) noexcept:m_impl(std::move(impl)){}
NvrhiGather::~NvrhiGather()=default;
FRESULT NvrhiGather::create(const GatherOperatorDesc& desc,std::unique_ptr<NvrhiGather>& output) noexcept {
    return guarded([&] {
        require(desc.volume&&desc.artifact&&desc.artifactBytes);
        auto impl=std::make_unique<Impl>();impl->owner=desc.volume;impl->device=detail::VolumeMutationBuilder::device(*desc.volume);
        check(createShaderProgram(impl->device,desc.artifact,desc.artifactBytes,&impl->program));
        uint32_t entry;check(impl->program->findEntryPoint("gatherOwnedAtlas",ShaderType::Compute,&entry));
        BindingLayoutDesc layout;check(impl->program->getBindingLayoutDesc(entry,0,layout));validateArtifact(impl->program,entry,layout);
        check(impl->device->createBindingLayout(layout,&impl->layout));ShaderHandle shader;check(impl->program->createShader(entry,&shader));
        check(impl->device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout),&impl->pipeline));
        output=std::unique_ptr<NvrhiGather>(new NvrhiGather(std::move(impl)));
    });
}
FRESULT NvrhiGather::prepare(const GatherRequestDesc& request,const GatherPreparationBudget& budget,
    const detail::ResolvedPointSet& associations,const detail::ResolvedPointSet& points,
    std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        require(request.channel<c_MaxChannels&&uint32_t(request.kind)<=uint32_t(GatherKind::LevelSet)&&request.accumulate<=1);
        require(request.subcellWidth==1||request.subcellWidth==2||request.subcellWidth==4||request.subcellWidth==8);
        require(std::isfinite(request.radius)&&request.radius>0&&finite(request.translation));
        require(std::isfinite(request.supportRadius)&&(request.supportRadius==-1.f||request.supportRadius>=0));
        auto& state=*m_impl;
        require(points.retained&&points.retained->getDevice()==state.device.Get());
        require(associations.retained&&associations.retained->getDevice()==state.device.Get());
        require(associations.info.count==points.info.count);
        const bool colors=request.colorChannel!=UINT32_MAX;
        require(!colors||(request.colorChannel<c_MaxChannels&&request.colorChannel!=request.channel));
        require(!colors||(points.info.attributeMask&4u),FE_NOT_FOUND);
        const auto snapshot=state.owner->snapshot();require(bool(snapshot),FE_NOT_ALIVE_OBJECT);
        const auto& source=snapshot->bindings();
        uint32_t mask=uint32_t(1)<<request.channel;if(colors)mask|=uint32_t(1)<<request.colorChannel;
        require((source.channelMask&mask)==mask,FE_NOT_FOUND);
        require(source.channels[request.channel].desc.format==AtlasFormat::R32_FLOAT,FE_UNSUPPORTED);
        require(!colors||source.channels[request.colorChannel].desc.format==AtlasFormat::RGBA8_UINT,FE_UNSUPPORTED);
        // Width is meaningful even for a currently empty atlas.
        const auto level=state.owner->host().configuration();
        require((uint32_t(1)<<level.logDimension[0])%request.subcellWidth==0,FE_UNSUPPORTED);
        std::unique_ptr<detail::VolumeMutationBuilder> builder;
        check(detail::VolumeMutationBuilder::begin(*state.owner,mask,{budget.maxAllocatedBytes,budget.maxBuffers},builder));
        if(!builder->hasWork()){check(builder->finish(output));return;}
        slang::PackedVolume topology;check(detail::VolumeAdoptionBuilder::exportTopology(*snapshot,topology));
        auto bins=buildBins(topology,request,budget,associations);
        // Authentication guarantees finite source XYZ; translation must also stay
        // finite for the evaluation stream, including points outside active cells.
        for(uint64_t i=0;i<points.info.count;++i) {
            std::array<float,3> position;
            std::memcpy(position.data(),static_cast<const uint8_t*>(points.positions.data)+
                size_t(i)*size_t(points.positionStream.byteStride),12);
            require(std::isfinite(position[0]+request.translation.x)&&
                std::isfinite(position[1]+request.translation.y)&&std::isfinite(position[2]+request.translation.z));
        }
        const auto& destination=builder->destinationBindings();
        const auto& old=source.channels[request.channel].raw;const auto& target=destination.channels[request.channel];
        BufferHandle cells,indices,constants,dummyColor;
        check(builder->upload(bins.cells.data(),bins.cells.size()*sizeof(slang::UInt2),sizeof(slang::UInt2),false,false,cells));
        check(builder->upload(bins.indices.data(),bins.indices.size()*sizeof(uint32_t),0,true,false,indices));
        Parameters p{};p.resolutionAndApron=source.volume.atlasResolutionAndApron;p.brickCountAndWidth=source.volume.atlasBrickCountAndWidth;
        p.work={target.storage.voxelCount,request.subcellWidth,bins.side,uint32_t(points.positionStream.byteStride)};
        p.options={uint32_t(request.kind),request.accumulate,colors?1u:0u,bins.perLeaf};
        p.translationAndRadius={request.translation.x,request.translation.y,request.translation.z,request.radius};
        check(builder->upload(&p,sizeof(p),0,false,true,constants));
        if(!colors)check(builder->allocate(4,0,true,false,dummyColor));
        const auto& oldColors=colors?source.channels[request.colorChannel].raw:old;
        const auto& newColors=colors?destination.channels[request.colorChannel].raw:target.raw;
        BindingSetDesc bindings;bindings.trackLiveness=true;
        bindings.addItem(BindingSetItem::ConstantBuffer(0,constants));
        bindings.addItem(BindingSetItem::StructuredBuffer_SRV(0,source.atlasMap.buffer,Format::UNKNOWN,{source.atlasMap.byteOffset,source.atlasMap.byteSize}));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(1,points.info.count?points.positionStream.buffer:old.buffer.Get()));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(2,colors&&points.info.count?points.colorStream.buffer:old.buffer.Get()));
        bindings.addItem(BindingSetItem::StructuredBuffer_SRV(3,cells));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(4,indices));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(5,old.buffer,{old.byteOffset,old.byteSize}));
        bindings.addItem(BindingSetItem::RawBuffer_SRV(6,oldColors.buffer,{oldColors.byteOffset,oldColors.byteSize}));
        bindings.addItem(BindingSetItem::RawBuffer_UAV(0,target.raw.buffer,{target.raw.byteOffset,target.raw.byteSize}));
        bindings.addItem(colors?BindingSetItem::RawBuffer_UAV(1,newColors.buffer,{newColors.byteOffset,newColors.byteSize})
            :BindingSetItem::RawBuffer_UAV(1,dummyColor));
        BindingSetHandle set;check(state.device->createBindingSet(bindings,state.layout,&set));
        const uint32_t groups=(target.storage.voxelCount+255u)/256u;
        check(builder->appendDispatch(state.pipeline,set,std::min(groups,65535u),(groups+65534u)/65535u));
        check(builder->finish(output));
    });
}
} // namespace gvdb::portable
