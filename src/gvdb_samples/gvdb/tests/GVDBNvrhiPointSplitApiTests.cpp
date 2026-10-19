// SPDX-License-Identifier: Apache-2.0
// Public-only API fixture. Expected lookup scans explicit leaf boxes, not the GPU tree.
#include "GVDBNvrhiPointSplitApi.h"
#include <cmath>
#include <limits>
#include <ShaderToolArtifact.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) {
    if (!condition)
        throw std::runtime_error(text);
}
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        int adapter = -1;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i], "-cuda");
            validation |= !std::strcmp(argv[i], "--validation");
            if (!std::strcmp(argv[i], "--adapter")) {
                require(++i < argc, "--adapter requires an ordinal");
                const std::string text = argv[i];
                size_t consumed = 0;
                adapter = std::stoi(text, &consumed);
                require(adapter >= 0 && consumed == text.size(), "invalid adapter ordinal");
            }
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            desc.deviceOrdinal = adapter < 0 ? 0 : adapter;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(
                donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.adapterIndex = adapter;
            desc.enableNvrhiValidationLayer = true;
            desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
        if (manager)
            std::printf("Point split backend %s; adapter %d (%s): %s\n",
                        donut::app::GetShaderTypeName(device->getGraphicsAPI()), adapter,
                        adapter < 0 ? "automatic" : "explicit", manager->GetRendererString());
        else
            std::printf("Point split backend CUDA; device ordinal %d\n", adapter < 0 ? 0 : adapter);
    }
    ~Device() {
        if (device)
            device->waitForIdle();
        device = nullptr;
        if (manager)
            manager->Shutdown();
    }
};
template <class T>
void queryIdentity(T* object) {
    const auto count = object->AddRef();
    object->Release();
    AutoPtr<T> same;
    check(object->QueryInterface(uuid_of<T>(), reinterpret_cast<void**>(same.GetAddressOf())),
          "self QueryInterface");
    require(same.Get() == object, "self identity");
    same = nullptr;
    AutoPtr<IObject> identity, again;
    check(object->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(identity.GetAddressOf())),
          "base identity");
    check(identity->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(again.GetAddressOf())),
          "repeat base identity");
    require(identity == again, "canonical IObject identity");
    identity = nullptr;
    again = nullptr;
    AutoPtr<IRHIObject> resource;
    check(object->QueryInterface(uuid_of<IRHIObject>(), reinterpret_cast<void**>(resource.GetAddressOf())),
          "RHI ancestor");
    resource = nullptr;
    void* missing = reinterpret_cast<void*>(uintptr_t(1));
    require(object->QueryInterface(uuid_of<IDevice>(), &missing) == FE_NOINTERFACE && !missing,
            "unsupported IID clears output");
    require(object->QueryInterface(uuid_of<T>(), nullptr) == FS_OK, "query-only IID probe");
    require(object->AddRef() == count, "queries preserve reference balance");
    object->Release();
}

struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* d)
        : device(d) {
        check(device->createCommandList({}, &commands), "create command list");
    }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto serial = device->executeCommandList(commands);
        require(serial || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit commands");
        require(device->waitForIdle(), "caller GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(IBuffer* buffer, uint64_t size) {
        if (!size)
            return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(size)
                                       .setCpuAccess(CpuAccessMode::Read)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "create test readback");
        begin();
        commands->copyBuffer(staging, 0, buffer, 0, size);
        submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data, "map canonical GPU stream");
        std::vector<uint8_t> result(size_t(size), uint8_t(0));
        std::memcpy(result.data(), data, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};
std::vector<char> rewriteArtifact(
    const std::vector<char>& source,
    const std::function<void(ShaderToolArtifactEntry&, std::vector<ShaderToolArtifactBinding>&)>& modify) {
    ShaderToolArtifactView view;
    require(ShaderToolArtifactOpen(source.data(), source.size(), &view) == 0, "open accepted artifact");
    ShaderToolArtifactInfo info;
    ShaderToolArtifactEntry entry;
    require(ShaderToolArtifactGetInfo(&view, &info) == 0 && info.EntryCount == 1 &&
                ShaderToolArtifactGetEntry(&view, 0, &entry) == 0,
            "single entry artifact");
    std::vector<ShaderToolArtifactBinding> bindings(info.BindingCount);
    for (uint32_t i = 0; i < info.BindingCount; ++i)
        require(ShaderToolArtifactGetBinding(&view, i, &bindings[i]) == 0, "artifact reflection");
    modify(entry, bindings);
    ShaderToolArtifactDesc desc{};
    desc.Target = info.Target;
    desc.Flags = info.Flags;
    desc.Compiler = info.Compiler;
    desc.Entries = &entry;
    desc.EntryCount = 1;
    desc.Bindings = bindings.data();
    desc.BindingCount = uint32_t(bindings.size());
    size_t count = 0;
    require(ShaderToolArtifactWrite(&desc, nullptr, 0, &count) == 0, "size altered valid container");
    std::vector<char> result(count);
    require(ShaderToolArtifactWrite(&desc, result.data(), result.size(), &count) == 0,
            "write altered valid container");
    require(ShaderToolArtifactOpen(result.data(), result.size(), &view) == 0,
            "altered artifact structurally valid");
    return result;
}

std::vector<char> artifact(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);
    require(bool(f),"open split artifact");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
g::PointSplitHandle factory(IDevice* device,const std::vector<char>& code){g::PointSplitCreateDesc d;
    d.device=device;d.artifact=code.data();d.artifactBytes=code.size();g::PointSplitHandle op;
    check(g::createPointSplit(d,&op),"split factory");return op;}
void run(IDevice* device,unsigned count,unsigned components,bool velocity,bool translated,bool withVelocity=true,unsigned poison=0){
    Harness h(device);const auto base=donut::app::GetDirectoryWithExecutable()/"shaders";
    const auto target=donut::app::GetShaderTypeName(device->getGraphicsAPI());
    const auto remap=artifact(base/"gvdb_point_cloud"/target/"GVDBAtlasRepack_relocateAtlas.bin");
    const auto conversion=artifact(base/"gvdb_point_cloud"/target/"GVDBPointStreamConversion_convertPointStream.bin");
    const auto code=artifact(base/"gvdb_point_split"/target/"GVDBPointSplit_splitPointCoordinates.bin");
    g::VolumeCreateDesc vc;vc.device=device;vc.remapArtifact=remap.data();vc.remapArtifactBytes=remap.size();
    g::VolumeHandle owner;check(g::createVolume(vc,&owner),"point owner");
    g::PointOpsCreateDesc pc;pc.volume=owner;pc.conversionArtifact=conversion.data();pc.conversionArtifactBytes=conversion.size();
    g::PointOpsHandle pointOp;check(g::createPointOps(pc,&pointOp),"point factory");
    std::vector<g::Float4> positions(count),velocities(count);
    for(unsigned i=0;i<count;++i){positions[i]={float(int(i%19)-9)*.25f,float(int(i%13)-6)*.5f,float(i)*.125f,123};
        velocities[i]={float(i)*.5f-70.f,-3,float(int(i%7)-3)*.25f,std::numeric_limits<float>::quiet_NaN()};}
    if(count && poison)velocities[0].x=poison==1?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::max();
    g::PointSetDesc input;input.positions.stream={positions.data(),positions.size()*16,0,16,count,g::PointComponent::Float32,components};
    if(withVelocity)input.velocities.stream={velocities.data(),velocities.size()*16,0,16,count,g::PointComponent::Float32,components};
    g::PointUploadTicketHandle upload;check(pointOp->preparePoints(input,{},&upload),"prepare source");
    h.begin();check(pointOp->recordPoints(h.context(),upload),"record source");h.submit();
    g::PointSetHandle points;check(upload->finishAfterCallerCompletion(&points),"finish source");
    upload=nullptr;pointOp=nullptr;owner=nullptr;
    auto op=factory(device,code),other=factory(device,code);queryIdentity(op.Get());
    g::PointSplitRequestDesc d;d.points=points;d.attribute=velocity?g::PointAttribute::Velocity:g::PointAttribute::Position;
    if(translated)d.translation={.125f,-.25f,.5f,0};
    if(poison==2)d.translation.x=std::numeric_limits<float>::max();
    auto reject=[&](g::PointSplitRequestDesc bad,g::PointSplitPreparationBudget budget,FRESULT status){
        auto* out=reinterpret_cast<g::IGVDBPointSplitTicket*>(uintptr_t(1));
        require(op->prepare(bad,budget,&out)==status && !out,"split rejection clears output");};
    if(velocity && (!withVelocity || !count)){reject(d,{},FE_NOT_FOUND);return;}
    if(velocity && poison){reject(d,{},FE_INVALID_ARGS);return;}
    g::BorrowedBytes canonical;check(points->getData(d.attribute,canonical),"canonical input");
    std::vector<float> expected(size_t(count)*3);float lo[3]{},hi[3]{};const float trans[]={d.translation.x,d.translation.y,d.translation.z};
    for(unsigned axis=0;axis<3;++axis)for(unsigned i=0;i<count;++i){float v;
        std::memcpy(&v,static_cast<const uint8_t*>(canonical.data)+(size_t(i)*components+axis)*4,4);
        if(trans[axis]!=0)v+=trans[axis];expected[size_t(axis)*count+i]=v;
        if(!i)lo[axis]=hi[axis]=v;else{lo[axis]=std::min(lo[axis],v);hi[axis]=std::max(hi[axis],v);}}
    g::PointSplitPreparationBudget budget;budget.maxHostBytes=count*12ull;budget.maxAllocatedBytes=count?256+count*24ull:0;budget.maxBuffers=count?3:0;
    if(count){auto b=budget;--b.maxHostBytes;reject(d,b,FE_OUT_OF_MEMORY);b=budget;--b.maxAllocatedBytes;
        reject(d,b,FE_OUT_OF_MEMORY);b=budget;--b.maxBuffers;reject(d,b,FE_OUT_OF_MEMORY);}
    auto b=budget;b.reserved=1;reject(d,b,FE_INVALID_ARGS);
    auto bad=d;bad.structSize--;reject(bad,{},FE_INVALID_ARGS);bad=d;bad.points=nullptr;reject(bad,{},FE_INVALID_ARGS);
    bad=d;bad.attribute=g::PointAttribute::Color;reject(bad,{},FE_INVALID_ARGS);
    bad=d;bad.translation.x=INFINITY;reject(bad,{},FE_INVALID_ARGS);bad=d;bad.translation.w=1;reject(bad,{},FE_INVALID_ARGS);
    require(op->prepare(d,{},nullptr)==FE_INVALID_ARGS,"null split output");
    g::PointSplitTicketHandle ticket;check(op->prepare(d,budget,&ticket),"exact split budgets");queryIdentity(ticket.Get());
    require(ticket->getDevice()==device && ticket->getCount()==count,"split metadata");
    g::BorrowedBytes bytes{reinterpret_cast<void*>(uintptr_t(1)),123};g::BorrowedBufferBinding buffer{nullptr,17,23};
    g::Float4 minimum{1,2,3,4},maximum{5,6,7,8};
    require(NVRHI_FAILED(ticket->getAxisData(0,bytes)) && bytes.byteCount==123,"early axis data preserved");
    require(NVRHI_FAILED(ticket->getAxisBuffer(0,buffer)) && buffer.byteSize==23,"early buffer preserved");
    require(NVRHI_FAILED(ticket->getBounds(minimum,maximum)) && minimum.w==4 && maximum.w==8,"early bounds preserved");
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion()),"early completion rejected");
    points=nullptr;
    h.begin();require(NVRHI_FAILED(other->record(h.context(),ticket)),"wrong factory");
    require(NVRHI_FAILED(op->record({},ticket)),"wrong context");check(op->record(h.context(),ticket),"record split");
    require(NVRHI_FAILED(op->record(h.context(),ticket)),"single split record");op=nullptr;other=nullptr;h.submit();
    check(ticket->finishAfterCallerCompletion(),"finish split");require(NVRHI_FAILED(ticket->finishAfterCallerCompletion()),"one finish");
    check(ticket->getBounds(minimum,maximum),"completed extrema");
    const g::Float4 expectedMin{lo[0],lo[1],lo[2],0},expectedMax{hi[0],hi[1],hi[2],0};
    require(!std::memcmp(&minimum,&expectedMin,16) && !std::memcmp(&maximum,&expectedMax,16),"actual GPU exact extrema");
    for(unsigned axis=0;axis<3;++axis){check(ticket->getAxisData(axis,bytes),"completed plane");
        require(bytes.byteCount==count*4ull && (count || !bytes.data),"plane byte count");
        require(!count || !std::memcmp(bytes.data,expected.data()+size_t(axis)*count,count*4),"independent plane values");
        check(ticket->getAxisBuffer(axis,buffer),"completed axis buffer");
        require(buffer.byteOffset==axis*count*4ull && buffer.byteSize==count*4ull && (count || !buffer.buffer),"plane range");
        auto actual=h.read(buffer.buffer,buffer.byteOffset+buffer.byteSize);
        require(!count || !std::memcmp(actual.data()+buffer.byteOffset,bytes.data,count*4),"GPU/CPU plane coherence");}
    bytes={reinterpret_cast<void*>(uintptr_t(1)),123};buffer={nullptr,17,23};
    require(ticket->getAxisData(3,bytes)==FE_INVALID_ARGS && bytes.byteCount==123,"invalid axis data preserves output");
    require(ticket->getAxisBuffer(UINT32_MAX,buffer)==FE_INVALID_ARGS && buffer.byteSize==23,"invalid axis buffer preserves output");
    if(count==1){auto rejectArtifact=[&](const std::vector<char>& bytes){g::PointSplitCreateDesc create;create.device=device;
        create.artifact=bytes.data();create.artifactBytes=bytes.size();auto* out=reinterpret_cast<g::IGVDBPointSplit*>(uintptr_t(1));
        require(NVRHI_FAILED(g::createPointSplit(create,&out)) && !out,"incompatible split artifact");};
        rejectArtifact({});rejectArtifact({'b','a','d'});rejectArtifact(remap);
        rejectArtifact(rewriteArtifact(code,[](auto& e,auto&){e.ThreadGroupSize[0]=128;}));
        rejectArtifact(rewriteArtifact(code,[](auto& e,auto&){e.Name=e.Symbol="wrongEntry";}));
        rejectArtifact(rewriteArtifact(code,[](auto&,auto& b){b[0].ElementStride+=16;}));
        rejectArtifact(rewriteArtifact(code,[](auto&,auto& bindings){for(auto& b:bindings)if(b.Scope==ST_SCOPE_CONSTANT_BUFFER){b.Offset+=4;break;}}));}
}
} // namespace
int main(int argc,const char** argv){donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity,const char* text){if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try{{Device device(argc,argv);for(unsigned components:{3u,4u})for(unsigned count:{0u,1u,259u})for(bool velocity:{false,true})
        run(device.device,count,components,velocity,count==259);
        run(device.device,17,3,true,false,false);run(device.device,17,4,true,false,true,1);
        run(device.device,17,3,true,false,true,2);run(device.device,17,4,false,false,true,1);}
        require(problems==0,"native validation warnings/errors");std::puts("PASS point splitting: actual GPU planes/extrema, attributes, translations, exact budgets, lifetimes and artifact validation");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
