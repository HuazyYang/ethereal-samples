// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiApi.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status)) throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning) ++problems;
        std::fprintf(stderr,"%s\n",text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i],"-cuda"); validation |= !std::strcmp(argv[i],"--validation");
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false; callback = MAKE_RC_OBJ_PTR(Messages); desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc,argv)));
            require(bool(manager),"create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.enableNvrhiValidationLayer = true; desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc),"create graphics device"); device = manager->GetDevice();
        }
        require(bool(device),"create test device");
    }
    ~Device() { if (device) device->waitForIdle(); device = nullptr; if (manager) manager->Shutdown(); }
};
template<class T> void queryIdentity(T* object) {
    const auto count = object->AddRef(); object->Release();
    AutoPtr<T> same;
    check(object->QueryInterface(uuid_of<T>(),reinterpret_cast<void**>(same.GetAddressOf())),"self QueryInterface");
    require(same.Get() == object,"self identity");
    same = nullptr;
    AutoPtr<IObject> identity, again;
    check(object->QueryInterface(uuid_of<IObject>(),reinterpret_cast<void**>(identity.GetAddressOf())),"base identity");
    check(identity->QueryInterface(uuid_of<IObject>(),reinterpret_cast<void**>(again.GetAddressOf())),"repeat base identity");
    require(identity == again,"canonical IObject identity"); identity = nullptr; again = nullptr;
    AutoPtr<IRHIObject> resource;
    check(object->QueryInterface(uuid_of<IRHIObject>(),reinterpret_cast<void**>(resource.GetAddressOf())),"RHI ancestor");
    resource = nullptr;
    void* missing = reinterpret_cast<void*>(uintptr_t(1));
    require(object->QueryInterface(uuid_of<IDevice>(),&missing) == FE_NOINTERFACE && !missing,"unsupported IID clears output");
    require(object->QueryInterface(uuid_of<T>(),nullptr) == FS_OK,"query-only IID probe");
    require(object->AddRef() == count,"queries preserve reference balance"); object->Release();
}
struct ForeignTicket : ObjectImpl<g::IGVDBGenerationTicket> {
    IDevice* device;
    explicit ForeignTicket(IDevice* value) : device(value) {}
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignTicket)
        NVRHI_IMPLEMENTS_INTERFACE(g::IGVDBGenerationTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device; }
};
void factoryFailures() {
    require(g::gvdbGetNvrhiApiVersion() == g::c_GVDBNvrhiApiVersion,"library/header version");
    auto* output = reinterpret_cast<g::IGVDBVolume*>(uintptr_t(1));
    g::VolumeCreateDesc desc;
    require(g::gvdbCreateNvrhiVolume(g::c_GVDBNvrhiApiVersion + 1,&desc,&output) == FE_UNSUPPORTED && !output,
            "factory version mismatch clears output");
    output = reinterpret_cast<g::IGVDBVolume*>(uintptr_t(1)); desc.structSize--;
    require(g::createVolume(desc,&output) == FE_INVALID_ARGS && !output,"factory size mismatch clears output");
    desc = {}; desc.reserved = 1;
    require(g::createVolume(desc,&output) == FE_INVALID_ARGS && !output,"factory reserved field rejected");
    desc = {};
    require(g::createVolume(desc,&output) == FE_INVALID_ARGS && !output,"factory missing device/artifact rejected");
    require(g::createVolume(desc,nullptr) == FE_INVALID_ARGS,"factory null output rejected");
}
g::VolumeHandle create(IDevice* device, const std::vector<char>& artifact) {
    g::VolumeCreateDesc desc;
    desc.device = device; desc.configuration.levelCount = 3;
    desc.configuration.logDimension = {1,1,1}; desc.configuration.initialNodeCapacity = {1,1,1};
    desc.remapArtifact = artifact.data(); desc.remapArtifactBytes = artifact.size();
    g::VolumeHandle volume; check(g::createVolume(desc,&volume),"public volume factory"); return volume;
}
void verifyBytes(g::IGVDBReadback* readback, uint32_t gridX, uint32_t gridZ) {
    g::BorrowedBytes bytes; check(readback->getChannelData(0,bytes),"read logical channel bytes");
    require(bytes.data && bytes.byteCount == uint64_t(gridX*4)*4*(gridZ*4),"logical byte count");
    const auto* data = static_cast<const uint8_t*>(bytes.data);
    for (uint32_t z = 0; z < gridZ*4; ++z) for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < gridX*4; ++x) {
        const uint32_t slot = (z/4)*gridX+x/4;
        const int wx = int(slot*2+x%4)-1, wy = int(y)-1, wz = int(z%4)-1;
        const bool interior = wx >= 0 && wx < 4 && wy >= 0 && wy < 2 && wz >= 0 && wz < 2;
        const uint8_t expected = interior ? uint8_t(10+(wx/2)*40+wx%2+wy*2+wz*4) : 23;
        require(data[(size_t(z)*4+y)*(gridX*4)+x] == expected,"independent retained interior/apron byte pattern");
    }
}
void run(IDevice* device, const std::vector<char>& artifact) {
    auto volume = create(device,artifact), foreign = create(device,artifact);
    queryIdentity(volume.Get()); require(volume->getDevice() == device,"borrowed device identity");
    g::HostVolumeHandle host;
    check(volume->QueryInterface(uuid_of<g::IGVDBHostVolume>(),reinterpret_cast<void**>(host.GetAddressOf())),"host interface query");
    queryIdentity(host.Get());
    require(host->getWidth(0) == 2 && host->getRange(2) == 8,"host level geometry");
    g::HostExtents cells;
    check(host->computeExtents(1,{0,0,0,0},{4,2,2,0},cells),"public CPU extents");
    const uint8_t masks[] = {255,1}; uint64_t selected = 0;
    check(host->activateRegionFromMasks(cells,masks,2,selected),"public CPU region activation");
    require(selected == 2 && host->getPoolCount(0,0) == 2 && host->isTopologyDirty(),"public CPU pool/dirty state");
    check(host->finishTopology(),"public topology finish");
    require(host->getActiveLeafCount() == 2 && host->getBounds().upper.x == 4,"public CPU bounds");
    g::IGVDBSnapshot* absent = reinterpret_cast<g::IGVDBSnapshot*>(uintptr_t(1));
    require(volume->getSnapshot(&absent) == FE_NOT_FOUND && !absent,"unpublished snapshot clears output");
    check(volume->setChannel(0,{g::AtlasFormat::R8_UINT,{23,0,0,0}}),"set public raw channel");
    g::GenerationDesc request; request.placement.minimumBrickCount = {2,1,1};
    g::VolumeInfo placement;
    check(volume->describePlacement(request.placement,placement),"describe destination atlas");
    require(placement.atlasResolutionAndApron.x == 8,"public placement geometry");
    std::vector<uint8_t> upload(128,233);
    for (uint32_t slot = 0; slot < 2; ++slot) for (uint32_t z = 0; z < 2; ++z)
        for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x)
            upload[((z+1)*4+y+1)*8+slot*4+x+1] = uint8_t(10+slot*40+x+y*2+z*4);
    g::ChannelUploadDesc source{0,0,upload.data(),upload.size()}; request.uploads = &source; request.uploadCount = 1;
    g::GenerationTicketHandle first;
    check(volume->prepareGeneration(request,&first),"prepare public generation"); queryIdentity(first.Get());
    std::fill(upload.begin(),upload.end(),0); // Preparation copied all bytes.
    auto* unchanged = first.Get(); request.reserved = 1;
    require(volume->prepareGeneration(request,&unchanged) == FE_INVALID_ARGS && !unchanged && first,
            "failed preparation clears output and preserves independently retained ticket");
    request.reserved = 0; request.uploads = nullptr; request.uploadCount = 0;
    g::GenerationTicketHandle other;
    check(foreign->prepareGeneration(request,&other),"prepare foreign owner ticket");
    CommandListHandle commands; check(device->createCommandList({},&commands),"create caller list");
    const g::RecordingContext context{device,commands};
    commands->open();
    auto fake = MAKE_RC_OBJ_PTR(ForeignTicket,device);
    require(volume->recordAndPublish(context,fake) == FE_INVALID_ARGS,"unrelated public ticket implementation rejected safely");
    require(volume->recordAndPublish(context,other) == FE_INVALID_ARGS,"authenticated other-owner ticket rejected");
    require(volume->recordAndPublish({},first) == FE_INVALID_ARGS,"missing recording context leaves ticket reusable");
    check(volume->recordAndPublish(context,first),"record first generation");
    require(NVRHI_FAILED(volume->recordAndPublish(context,first)),"one-use ticket rejects repeat publication");
    g::SnapshotHandle old;
    check(volume->getSnapshot(&old),"get retained old snapshot"); queryIdentity(old.Get());
    require(old->getDesc().channelMask == 1 && old->getDesc().activeLeafCount == 2,"public snapshot metadata");
    g::ChannelSnapshotDesc channel;
    check(old->getChannel(0,channel),"public channel snapshot");
    require(channel.raw.buffer && channel.raw.byteSize == 128 && channel.storage.logicalBytes == 128 &&
            channel.raw.buffer->getDesc().structStride == 0,"canonical raw resource exposed");
    BufferHandle independentBuffer = channel.raw.buffer;
    g::BorrowedBufferBinding nodes;
    check(old->getTopologyBuffer(g::TopologyBuffer::Nodes,nodes),"snapshot node binding");
    require(nodes.buffer && nodes.byteSize >= 128,"retained node buffer");
    require(old->getTopologyBuffer(g::TopologyBuffer(99),nodes) == FE_INVALID_ARGS && !nodes.buffer,"invalid binding kind clears output");
    g::LevelInfo level;
    check(old->getLevel(0,level),"snapshot level metadata"); require(level.nodes.z == 2,"snapshot leaf pool count");
    require(old->getChannel(31,channel) == FE_NOT_FOUND && !channel.raw.buffer,"missing channel clears descriptor");
    request.placement.minimumBrickCount = {1,1,1};
    g::GenerationTicketHandle second;
    check(volume->prepareGeneration(request,&second),"prepare repacked public generation");
    check(volume->recordAndPublish(context,second),"record second generation");
    g::SnapshotHandle latest;
    check(volume->getSnapshot(&latest),"get latest retained snapshot");
    require(old->getDesc().volume.atlasResolutionAndApron.x == 8 && latest->getDesc().volume.atlasResolutionAndApron.x == 4,
            "old snapshot bindings stay unchanged after publication");
    g::ReadbackHandle oldRead, newRead;
    check(volume->prepareReadback(old,1,&oldRead),"prepare old readback");
    auto* keptRead = oldRead.Get();
    require(foreign->prepareReadback(old,1,&keptRead) == FE_INVALID_ARGS && !keptRead && oldRead,
            "foreign snapshot failure clears output and preserves retained readback");
    check(volume->prepareReadback(latest,1,&newRead),"prepare new readback"); queryIdentity(newRead.Get());
    g::BorrowedBytes early{reinterpret_cast<void*>(uintptr_t(1)),99};
    require(newRead->getChannelData(0,early) == FE_NOT_ALIVE_OBJECT && !early.data && !early.byteCount,
            "uncompleted readback exposes no bytes");
    require(foreign->recordReadback(context,newRead) == FE_INVALID_ARGS,"foreign readback owner rejected");
    check(volume->recordReadback(context,oldRead),"record old readback");
    check(volume->recordReadback(context,newRead),"record new readback");
    require(NVRHI_FAILED(volume->recordReadback(context,newRead)),"readback recording is one-use");
    // Release the facade owner and every preparation/snapshot before submission.
    volume = nullptr; host = nullptr; foreign = nullptr; first = nullptr; second = nullptr; other = nullptr;
    old = nullptr; latest = nullptr; fake = nullptr;
    require(independentBuffer->getDesc().byteSize == 128,"independently retained buffer survives snapshot destruction");
    commands->close(); device->executeCommandList(commands); require(device->waitForIdle(),"caller GPU completion");
    check(oldRead->finishAfterCallerCompletion(),"finish retained readback after owner release");
    check(newRead->finishAfterCallerCompletion(),"finish new readback after owner release");
    require(oldRead->getDevice() == device && newRead->getChannelMask() == 1,"retained readback identity");
    verifyBytes(oldRead,2,1); verifyBytes(newRead,1,2);
    require(NVRHI_FAILED(newRead->finishAfterCallerCompletion()),"completion is one-use");
    verifyBytes(newRead,1,2); // Failed second completion preserves borrowed bytes.
    device->runGarbageCollection();
    std::puts("GVDB public API: QI/refcounts, version/size checks, CPU edits, foreign tickets, snapshots and retained readbacks PASS");
}
}
int main(int argc, const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Warning) ++problems;
        std::fprintf(stderr,"%s\n",text);
    });
    try {
        factoryFailures();
        bool cpuOnly = false;
        for (int i = 1; i < argc; ++i) cpuOnly |= !std::strcmp(argv[i],"--factory-only");
        if (cpuOnly) { std::puts("GVDB public factory validation PASS"); return 0; }
        {
            Device device(argc,argv);
            auto path = donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owner"/
                donut::app::GetShaderTypeName(device.device->getGraphicsAPI())/"GVDBAtlasRepack_relocateAtlas.bin";
            for (int i = 1; i+1 < argc; ++i) if (!std::strcmp(argv[i],"--artifact")) path = argv[i+1];
            std::ifstream file(path,std::ios::binary); require(bool(file),"open owner artifact");
            std::vector<char> artifact((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            run(device.device,artifact);
        }
        require(problems == 0,"native/NVRHI validation warnings or errors"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
