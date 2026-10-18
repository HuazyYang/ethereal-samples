// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "GVDBNvrhiVoxelOps.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <limits>
#include <cmath>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(FRESULT result, const char* message) {
    if (NVRHI_FAILED(result)) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
#if ETHEREAL_TEST_CUDA
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning) ++problems;
        std::fprintf(stderr, "%s\n", text);
    }
};
#endif
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool cuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            cuda |= std::strcmp(argv[i], "-cuda") == 0;
            validation |= std::strcmp(argv[i], "--validation") == 0;
        }
        if (cuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable in this build");
#endif
        } else {
            manager = TakeOver(donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "requested graphics backend unavailable");
            donut::app::DeviceCreationParameters parameters;
            parameters.enableNvrhiValidationLayer = true;
            parameters.enableCopyQueue = true;
            parameters.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(parameters), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
    }
    ~Device() { if (device) device->waitForIdle(); device = nullptr; if (manager) manager->Shutdown(); }
};
struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* value) : device(value) { check(device->createCommandList({}, &commands), "create command list"); }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto id = device->executeCommandList(commands);
        require(id || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit");
        require(device->waitForIdle(), "GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(const g::BufferView& view) {
        if (!view.byteSize) return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc().setByteSize(view.byteSize).setCpuAccess(CpuAccessMode::Read)
            .setInitialState(ResourceStates::CopyDest).setKeepInitialState(true), &staging), "staging");
        begin(); commands->copyBuffer(staging, 0, view.buffer, view.byteOffset, view.byteSize); submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data != nullptr, "map staging");
        std::vector<uint8_t> result(size_t(view.byteSize));
        std::memcpy(result.data(), data, result.size()); device->unmapBuffer(staging);
        return result;
    }
};


namespace detail = g::detail;
using Channels = std::array<std::vector<uint8_t>,5>;
const g::ChannelDesc descriptors[] = {
    {g::AtlasFormat::R8_UINT,{23,0,0,0}},{g::AtlasFormat::RGBA8_UINT,{153,51,204,117}},
    {g::AtlasFormat::R32_FLOAT,{-3.5f,0,0,0}},{g::AtlasFormat::RGB32_FLOAT,{-1.25f,2.5f,7.75f,0}},
    {g::AtlasFormat::RGBA32_FLOAT,{3.25f,-4.5f,1.75f,9.5f}}
};
struct Tag final : ObjectImpl<IRHIObject> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Tag)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    uint32_t value;
    explicit Tag(uint32_t v):value(v){}
};
std::unique_ptr<g::HostVolume> candidate() {
    auto result=std::make_unique<g::HostVolume>();g::LevelConfig config;config.levelCount=3;
    config.logDimension[0]=config.logDimension[1]=config.logDimension[2]=1;
    check(result->configure(config),"configure candidate");g::NodeId node;
    check(result->activate(-2,0,0,node),"candidate negative leaf");
    check(result->activate(0,0,0,node),"candidate adjacent leaf");
    check(result->activate(2,0,0,node),"candidate inactive leaf");check(result->setActive(node,false),"candidate inactive flag");
    result->editNode(result->findLeaf({-2,0,0}))->atlasInterior={77,88,99};
    check(result->finishTopology(),"candidate finish");return result;
}
Channels poison(uint32_t seed) {
    Channels result;
    for(uint32_t c=0;c<5;++c) {
        g::ChannelStorage storage;check(g::describeChannel(descriptors[c].format,{18,12,6},storage).result,"channel extent");
        auto& bytes=result[c];bytes.resize(size_t(storage.logicalBytes));
        for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*29+(i>>3)*11+c*37+seed);
        if(c>=2)for(size_t i=0;i+4<=bytes.size();i+=4){
            uint32_t bits=(i/4)%3==0?0x7fc12345u:((i/4)%3==1?0x80000000u:uint32_t(i*17+seed));
            std::memcpy(bytes.data()+i,&bits,4);
        }
    }
    return result;
}
void equalTopology(const gvdb::slang::PackedVolume& a,const gvdb::slang::PackedVolume& b) {
    require(std::memcmp(&a.volume,&b.volume,sizeof(a.volume))==0 && a.nodes==b.nodes && a.children==b.children &&
        a.levels.size()==b.levels.size() && a.atlasMap.size()==b.atlasMap.size(),"frozen topology sizes/bytes changed");
    require(std::memcmp(a.levels.data(),b.levels.data(),a.levels.size()*sizeof(g::LevelInfo))==0 &&
        std::memcmp(a.atlasMap.data(),b.atlasMap.data(),a.atlasMap.size()*sizeof(gvdb::slang::AtlasNode))==0,
        "frozen placement or levels changed");
}
std::unique_ptr<g::PreparedReadback> readLater(Harness& h,g::NvrhiVolume& owner,std::shared_ptr<const g::VolumeGeneration> source) {
    std::unique_ptr<g::PreparedReadback> ticket;check(owner.prepareReadback(source,source->bindings().channelMask,ticket),"prepare imported readback");
    check(owner.recordReadback(h.context(),*ticket),"record imported readback");return ticket;
}
void verify(g::PreparedReadback& ticket,const Channels& expected) {
    g::ChannelReadback bytes;check(g::NvrhiVolume::finishReadbackAfterCompletion(ticket,bytes),"read imported bytes");
    require(bytes.channelMask==31,"import channel order/mask");
    for(uint32_t c=0;c<5;++c)require(bytes.channels[c]==expected[c],"import changed logical apron/unused/raw bytes");
}
void run(IDevice* device,const std::vector<char>& artifact,const std::vector<char>& mutation) {
    Harness h(device);g::VolumeOwnerDesc desc;desc.device=device;desc.configuration.levelCount=2;
    desc.configuration.logDimension[0]=2;desc.configuration.logDimension[1]=1;
    desc.remapArtifact=artifact.data();desc.remapArtifactBytes=artifact.size();
    std::unique_ptr<g::NvrhiVolume> owner;check(g::NvrhiVolume::create(desc,owner),"owner");
    auto* stableHost=&owner->host();check(owner->setChannel(31,descriptors[1]),"old channel");
    g::NodeId oldNode;check(owner->host().activate(0,0,0,oldNode),"old node");check(owner->host().finishTopology(),"old finish");
    g::GenerationRequest dense;dense.placement.minimumBrickCount={1,1,1};dense.placement.apron=1;
    std::unique_ptr<g::PreparedGeneration> ticket;check(owner->prepareGeneration(dense,ticket),"old generation");
    h.begin();check(owner->recordAndPublish(h.context(),*ticket),"old publish");h.submit();auto old=owner->snapshot();
    auto source=candidate();const auto sourceRevision=source->revision();
    const auto first=source->findLeaf({-2,0,0}),second=source->findLeaf({0,0,0});
    detail::ImportedLeafSlot slots[]={{uint32_t(first.index()),5},{uint32_t(second.index()),2}};
    Channels bytes=poison(19);const Channels original=bytes;
    std::array<detail::ImportedChannel,5> channels;
    for(uint32_t c=0;c<5;++c)channels[c]={c,descriptors[c],bytes[c].data(),bytes[c].size()};
    detail::VolumeImportRequest request;request.candidate=source.get();request.placement={{3,2,1},2,slots,2};
    request.channels=channels.data();request.channelCount=5;request.attachment=MAKE_RC_OBJ_PTR(Tag,101u);
    auto* metadata=request.attachment.Get();
    check(owner->prepareGeneration(dense,ticket),"preserved output ticket");auto* preserved=ticket.get();
    const auto oldHostRevision=owner->host().revision();
    const auto reject=[&](const detail::VolumeImportRequest& bad){
        require(NVRHI_FAILED(detail::VolumeAdoptionBuilder::prepareImport(*owner,bad,ticket)) && ticket.get()==preserved &&
            owner->snapshot()==old && &owner->host()==stableHost && owner->host().revision().epoch==oldHostRevision.epoch &&
            owner->host().revision().topology==oldHostRevision.topology && owner->host().configuration().logDimension[0]==2,
            "failed import changed output/host/current");
    };
    auto bad=request;bad.budget.maxBuffers=6;reject(bad); // topology4 plus two channels, then failure
    auto badSlots=std::array<detail::ImportedLeafSlot,2>{slots[0],slots[0]};bad=request;bad.placement.leaves=badSlots.data();reject(bad);
    badSlots={slots[0],slots[1]};badSlots[1].slot=slots[0].slot;reject(bad);
    badSlots[1]=slots[1];badSlots[1].slot=6;reject(bad);
    bad=request;bad.placement.leafCount=1;reject(bad);
    bad=request;bad.placement.brickCount.x=0;reject(bad);
    auto badChannels=channels;badChannels[2].byteCount--;bad=request;bad.channels=badChannels.data();reject(bad);
    badChannels=channels;badChannels[2].channel=0;reject(bad);
    badChannels=channels;badChannels[2].desc.voidValue.x=std::numeric_limits<float>::quiet_NaN();reject(bad);
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,ticket),"prepare exact import");
    require(source->revision().epoch==sourceRevision.epoch && source->revision().topology==sourceRevision.topology &&
        source->node(first)->atlasInterior.x==77,"preparation mutated caller candidate");
    // Mutating every caller input after preparation must not alter the private staged state.
    for(auto& channel:bytes)std::fill(channel.begin(),channel.end(),0);check(source->clear(),"clear caller candidate after preparation");
    h.begin();check(owner->recordAndPublish(h.context(),*ticket),"record exact import");auto imported=owner->snapshot();
    auto importedRead=readLater(h,*owner,imported);h.submit();verify(*importedRead,original);
    require(&owner->host()==stableHost && owner->host().configuration().logDimension[0]==1 &&
        owner->host().activeLeafCount()==2 && owner->host().revision().epoch>oldHostRevision.epoch &&
        owner->host().revision().topology>oldHostRevision.topology && imported->bindings().channelMask==31,
        "adoption host identity/config/revisions");
    require(owner->host().node(first)->atlasInterior.x==-1 && detail::VolumeAdoptionBuilder::attachment(*imported)==metadata,
        "CPU placement or attachment policy");
    require(detail::VolumeAdoptionBuilder::validateGeneration(*owner,*imported)==FS_OK,"owned generation rejected");
    std::unique_ptr<g::NvrhiVolume> foreign;check(g::NvrhiVolume::create(desc,foreign),"foreign owner");
    require(detail::VolumeAdoptionBuilder::validateGeneration(*foreign,*imported)==FE_INVALID_ARGS,"foreign generation accepted");
    gvdb::slang::PackedVolume frozen;check(detail::VolumeAdoptionBuilder::exportTopology(*imported,frozen),"export imported topology");
    require(frozen.atlasMap.size()==6 && frozen.atlasMap[5].leaf==int(first.index()) && frozen.atlasMap[2].leaf==int(second.index()) &&
        frozen.atlasMap[0].leaf<0 && frozen.atlasMap[1].leaf<0 && frozen.atlasMap[3].leaf<0 && frozen.atlasMap[4].leaf<0,
        "original non-dense slots not preserved");
    g::Node placedFirst;std::memcpy(&placedFirst,reinterpret_cast<const uint8_t*>(frozen.nodes.data())+
        frozen.levels[0].nodes.x+first.index()*sizeof(g::Node),sizeof(placedFirst));
    require(placedFirst.atlasInterior.x==14 && placedFirst.atlasInterior.y==8 && placedFirst.atlasInterior.z==2,
        "original leaf interior not preserved");
    // A later target host commit rejects the staged transaction even with unchanged content.
    source=candidate();bytes=poison(73);for(uint32_t c=0;c<5;++c)channels[c]={c,descriptors[c],bytes[c].data(),bytes[c].size()};
    request.candidate=source.get();request.attachment=MAKE_RC_OBJ_PTR(Tag,202u);auto* nextMetadata=request.attachment.Get();
    std::unique_ptr<g::PreparedGeneration> stale;check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,stale),"stale import prep");
    check(owner->host().finishTopology(),"advance host revision");h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)) && owner->snapshot()==imported,"stale host import overwrote state");
    check(owner->prepareGeneration(dense,ticket),"same epoch repack");check(owner->recordAndPublish(h.context(),*ticket),"same epoch repack record");
    require(detail::VolumeAdoptionBuilder::attachment(*owner->snapshot())==metadata,"same epoch repack lost metadata");h.submit();
    gvdb::slang::PackedVolume after;check(detail::VolumeAdoptionBuilder::exportTopology(*imported,after),"export old after repack");equalTopology(frozen,after);
    std::unique_ptr<g::NvrhiVoxelOps> ops;check(g::NvrhiVoxelOps::create({owner.get(),mutation.data(),mutation.size()},ops),"mutation factory");
    h.begin();check(ops->prepareFill(0,{42,0,0,0},{},ticket),"metadata mutation");check(owner->recordAndPublish(h.context(),*ticket),"metadata mutation record");
    require(detail::VolumeAdoptionBuilder::attachment(*owner->snapshot())==metadata,"channel mutation lost metadata");h.submit();
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,stale),"stale publication import prep");
    h.begin();check(ops->prepareClear(1,{},ticket),"advance publication");check(owner->recordAndPublish(h.context(),*ticket),"advance publication record");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)),"stale publication import accepted");h.submit();
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,stale),"stale channel prep");check(owner->setChannel(31,descriptors[0]),"change channel config");
    h.begin();require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)),"stale config import accepted");h.submit();
    // Import replaces the full channel set, including a pending configured channel.
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,request,ticket),"second import prep");h.begin();
    check(owner->recordAndPublish(h.context(),*ticket),"second import record");auto secondImport=owner->snapshot();
    require(secondImport->bindings().channelMask==31 && detail::VolumeAdoptionBuilder::attachment(*secondImport)==nextMetadata,
        "replacement channels/metadata");auto secondRead=readLater(h,*owner,secondImport);
    auto oldRead=readLater(h,*owner,imported);
    check(owner->host().clear(),"new host epoch");check(owner->prepareGeneration(dense,ticket),"empty repack");
    check(owner->recordAndPublish(h.context(),*ticket),"empty epoch publish");
    require(detail::VolumeAdoptionBuilder::attachment(*owner->snapshot())==nullptr,"clear epoch retained stale metadata");
    ticket.reset();stale.reset();ops.reset();owner.reset();request.attachment=nullptr;foreign.reset();
    h.submit();verify(*secondRead,bytes);verify(*oldRead,original);
    check(detail::VolumeAdoptionBuilder::exportTopology(*imported,after),"frozen export without owner");equalTopology(frozen,after);
    require(detail::VolumeAdoptionBuilder::attachment(*imported)==metadata &&
        detail::VolumeAdoptionBuilder::attachment(*secondImport)==nextMetadata,"retained metadata lifetime");
    std::puts("PASS exact imported raw bytes/non-dense slots, stable host, stale/failure isolation, frozen export and metadata lifetimes");
}

// Independent spatial oracle. It uses explicit world boxes and source slot numbers,
// never the owner's map, packed nodes, placement builder or shader traversal.
struct DenseOracle {
    std::vector<g::Int3> leaves;
    std::vector<uint32_t> slots;
    g::UInt3 grid;
    uint32_t width = 2, apron = 1;
    Channels bytes;
};
const uint32_t oracleStrides[] = {1, 4, 4, 12, 16};
uint64_t oracleVoxel(g::UInt3 grid, uint32_t pitch, uint32_t slot, uint32_t x, uint32_t y, uint32_t z) {
    const uint64_t ax = uint64_t(slot % grid.x) * pitch + x;
    const uint64_t ay = uint64_t((slot / grid.x) % grid.y) * pitch + y;
    const uint64_t az = uint64_t(slot / (grid.x * grid.y)) * pitch + z;
    return (az * (uint64_t(grid.y) * pitch) + ay) * (uint64_t(grid.x) * pitch) + ax;
}
int oracleLeaf(const DenseOracle& volume, g::Int3 p) {
    for (size_t i = 0; i < volume.leaves.size(); ++i) {
        const auto a = volume.leaves[i];
        if (p.x >= a.x && p.x < a.x + int32_t(volume.width) &&
            p.y >= a.y && p.y < a.y + int32_t(volume.width) &&
            p.z >= a.z && p.z < a.z + int32_t(volume.width)) return int(i);
    }
    return -1;
}
std::array<uint8_t,16> oracleVoid(uint32_t channel) {
    std::array<uint8_t,16> result{};
    if (channel == 0) result[0] = 23;
    else if (channel == 1) { result[0] = 153; result[1] = 51; result[2] = 204; result[3] = 117; }
    else {
        const float scalar[] = {-3.5f};
        const float rgb[] = {-1.25f, 2.5f, 7.75f};
        const float rgba[] = {3.25f, -4.5f, 1.75f, 9.5f};
        const float* values = channel == 2 ? scalar : channel == 3 ? rgb : rgba;
        std::memcpy(result.data(), values, oracleStrides[channel]);
    }
    return result;
}
DenseOracle denseExpected(std::vector<g::Int3> leaves, g::UInt3 grid, uint32_t width, uint32_t apron,
    const DenseOracle* source, const std::vector<uint8_t>* overrideR32 = nullptr) {
    DenseOracle destination{std::move(leaves), {}, grid, width, apron, {}};
    for (uint32_t i = 0; i < destination.leaves.size(); ++i) destination.slots.push_back(i);
    if (destination.leaves.empty()) return destination;
    const uint32_t pitch = width + 2 * apron;
    const uint64_t voxelCount = uint64_t(grid.x) * grid.y * grid.z * pitch * pitch * pitch;
    for (uint32_t channel = 0; channel < 5; ++channel) {
        const uint32_t stride = oracleStrides[channel];
        const auto outside = oracleVoid(channel);
        auto& bytes = destination.bytes[channel]; bytes.resize(size_t(voxelCount * stride), 0);
        // Only active destination slots are filled. Every unused slot stays zero.
        for (uint32_t slot = 0; slot < destination.leaves.size(); ++slot)
            for (uint32_t z = 0; z < pitch; ++z) for (uint32_t y = 0; y < pitch; ++y) for (uint32_t x = 0; x < pitch; ++x) {
                const auto origin = destination.leaves[slot];
                const g::Int3 world{origin.x + int32_t(x) - int32_t(apron), origin.y + int32_t(y) - int32_t(apron),
                    origin.z + int32_t(z) - int32_t(apron)};
                const uint8_t* value = outside.data();
                const int present = oracleLeaf(destination, world);
                if (present >= 0) {
                    if (channel == 2 && overrideR32) {
                        const auto target = destination.leaves[size_t(present)];
                        const uint64_t voxel = oracleVoxel(grid, pitch, uint32_t(present),
                            uint32_t(world.x - target.x) + apron, uint32_t(world.y - target.y) + apron,
                            uint32_t(world.z - target.z) + apron);
                        value = overrideR32->data() + voxel * stride;
                    } else if (source && source->width == width) {
                        const int previous = oracleLeaf(*source, world);
                        if (previous >= 0) {
                            const auto before = source->leaves[size_t(previous)];
                            const auto after = destination.leaves[size_t(present)];
                            if (before.x == after.x && before.y == after.y && before.z == after.z) {
                                const uint64_t voxel = oracleVoxel(source->grid, source->width + 2 * source->apron,
                                    source->slots[size_t(previous)], uint32_t(world.x - before.x) + source->apron,
                                    uint32_t(world.y - before.y) + source->apron, uint32_t(world.z - before.z) + source->apron);
                                value = source->bytes[channel].data() + voxel * stride;
                            }
                        }
                    }
                }
                const uint64_t at = oracleVoxel(grid, pitch, slot, x, y, z) * stride;
                std::memcpy(bytes.data() + at, value, stride);
            }
    }
    return destination;
}
std::unique_ptr<g::HostVolume> denseCandidate(std::initializer_list<int32_t> origins, uint32_t leafLog = 1) {
    auto result = std::make_unique<g::HostVolume>();
    g::LevelConfig config; config.levelCount = 3; config.logDimension = {leafLog,1,1};
    check(result->configure(config), "dense candidate configure");
    for (int32_t x : origins) { g::NodeId id; check(result->activate(x,0,0,id), "dense candidate activate"); }
    check(result->finishTopology(), "dense candidate finish");
    return result;
}
void verifyDense(g::PreparedReadback& ticket, const DenseOracle& expected, const char* label) {
    g::ChannelReadback bytes;
    check(g::NvrhiVolume::finishReadbackAfterCompletion(ticket,bytes), "finish dense readback");
    require(bytes.channelMask == 31, "dense readback lost configured channels");
    for (uint32_t channel = 0; channel < 5; ++channel) {
        if (bytes.channels[channel] != expected.bytes[channel]) {
            std::fprintf(stderr,"%s channel%u bytes %zu expected %zu\n",label,channel,
                bytes.channels[channel].size(),expected.bytes[channel].size());
            const size_t count = std::min(bytes.channels[channel].size(),expected.bytes[channel].size());
            for (size_t i = 0; i < count; ++i) if (bytes.channels[channel][i] != expected.bytes[channel][i]) {
                std::fprintf(stderr,"byte%zu got%u expected%u\n",i,bytes.channels[channel][i],expected.bytes[channel][i]); break;
            }
            throw std::runtime_error(label);
        }
    }
}
void runDense(IDevice* device,const std::vector<char>& artifact) {
    Harness h(device);
    g::VolumeOwnerDesc create; create.device = device; create.configuration.levelCount = 3;
    create.configuration.logDimension = {1,1,1}; create.remapArtifact = artifact.data(); create.remapArtifactBytes = artifact.size();
    std::unique_ptr<g::NvrhiVolume> owner; check(g::NvrhiVolume::create(create,owner),"dense owner");
    auto* stableHost = &owner->host();
    auto source = candidate();
    const auto first = source->findLeaf({-2,0,0}), second = source->findLeaf({0,0,0});
    detail::ImportedLeafSlot slots[] = {{uint32_t(first.index()),5},{uint32_t(second.index()),2}};
    const DenseOracle original{{{-2,0,0},{0,0,0}}, {5,2}, {3,2,1}, 2, 2, poison(41)};
    std::array<detail::ImportedChannel,5> channels;
    for (uint32_t c = 0; c < 5; ++c) channels[c] = {c,descriptors[c],original.bytes[c].data(),original.bytes[c].size()};
    detail::VolumeImportRequest imported; imported.candidate = source.get(); imported.placement = {{3,2,1},2,slots,2};
    imported.channels = channels.data(); imported.channelCount = 5; imported.attachment = MAKE_RC_OBJ_PTR(Tag,303u);
    auto* metadata = imported.attachment.Get();
    std::unique_ptr<g::PreparedGeneration> ticket;
    check(detail::VolumeAdoptionBuilder::prepareImport(*owner,imported,ticket),"dense seed import");
    h.begin(); check(owner->recordAndPublish(h.context(),*ticket),"dense seed publish"); h.submit();
    auto originalSnapshot = owner->snapshot();
    auto next = denseCandidate({0,2});
    const auto candidateRevision = next->revision();
    require(next->node(g::NodeId{0})->position.x == 0 && next->findLeaf({2,0,0}).index() == 1,
        "independent dense candidate leaf pool order");
    g::GenerationRequest request; request.placement.minimumBrickCount = {1,2,1}; request.placement.apron = 1;
    g::VolumeInfo placement;
    check(detail::VolumeAdoptionBuilder::describeTopologyPlacement(*next,request.placement,placement),"describe candidate placement");
    require(placement.atlasResolutionAndApron.x == 4 && placement.atlasResolutionAndApron.y == 8 &&
        placement.atlasResolutionAndApron.z == 4 && placement.atlasResolutionAndApron.w == 1,
        "candidate placement dimensions");
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*next,request,true,ticket),"dense candidate preparation");
    auto* outputBefore = ticket.get(); const auto hostBefore = owner->host().revision();
    g::PackedHostTopology packedBefore; check(owner->host().packTopology(packedBefore),"before failure host bytes");
    auto fails = [&](const g::HostVolume& candidate, const g::GenerationRequest& invalid) {
        require(NVRHI_FAILED(detail::VolumeAdoptionBuilder::prepareTopology(*owner,candidate,invalid,true,ticket)),
            "invalid dense adoption accepted");
        g::PackedHostTopology after; check(owner->host().packTopology(after),"after failure host bytes");
        require(ticket.get() == outputBefore && owner->snapshot() == originalSnapshot && &owner->host() == stableHost &&
            owner->host().revision().epoch == hostBefore.epoch && owner->host().revision().topology == hostBefore.topology &&
            packedBefore.nodes == after.nodes && packedBefore.children == after.children,
            "dense preparation failure changed output/host/generation");
    };
    auto bad = request; bad.budget.maxBuffers = 6; fails(*next,bad);
    bad = request; bad.budget.maxAllocatedBytes = 32; fails(*next,bad);
    auto incompatible = denseCandidate({0},2); fails(*incompatible,request);
    auto malformed = denseCandidate({0}); malformed->editNode(malformed->root())->level = 1; fails(*malformed,request);
    require(next->activeLeafCount() == 2 && next->revision().epoch == candidateRevision.epoch &&
        next->revision().topology == candidateRevision.topology,
        "preparation changed caller candidate");
    check(next->clear(),"clear staged caller candidate"); next.reset(); source.reset();
    h.begin(); check(owner->recordAndPublish(h.context(),*ticket),"dense adoption publish"); h.submit();
    auto denseSnapshot = owner->snapshot();
    require(&owner->host() == stableHost && owner->host().revision().epoch > hostBefore.epoch &&
        owner->host().revision().topology > hostBefore.topology && owner->host().activeLeafCount() == 2 &&
        !owner->host().isAllocated({-2,0,0}) && owner->host().isActive({0,0,0}) && owner->host().isActive({2,0,0}),
        "dense cloned host, leaf removal/activation, stable address or new epoch");
    require(detail::VolumeAdoptionBuilder::attachment(*denseSnapshot) == metadata,
        "dense adoption explicit metadata preservation");
    for (uint32_t c = 0; c < 5; ++c) require(denseSnapshot->bindings().channels[c].revision.resource >
        originalSnapshot->bindings().channels[c].revision.resource,"dense replacement revision did not advance");
    const auto dense = denseExpected({{0,0,0},{2,0,0}},{1,2,1},2,1,&original);
    next = denseCandidate({0}); request.placement.minimumBrickCount = {3,1,1}; request.placement.apron = 2;
    std::unique_ptr<g::PreparedGeneration> stale;
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*next,request,true,stale),"dense stale host prep");
    check(owner->host().finishTopology(),"dense stale host commit");
    h.begin(); require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)) && owner->snapshot() == denseSnapshot &&
        owner->host().isActive({2,0,0}),"stale host dense adoption changed state"); h.submit();
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*next,request,true,stale),"dense stale base prep");
    g::GenerationRequest repack; repack.placement.minimumBrickCount = {1,2,1}; repack.placement.apron = 1;
    check(owner->prepareGeneration(repack,ticket),"advance dense base publication"); h.begin();
    check(owner->recordAndPublish(h.context(),*ticket),"advance dense base record"); auto advanced = owner->snapshot();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(),*stale)) && owner->snapshot() == advanced &&
        owner->host().isActive({2,0,0}),"stale base dense adoption changed state"); h.submit();
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*next,request,true,ticket),"subset only prepare");
    check(next->clear(),"clear subset caller"); next.reset();
    h.begin(); check(owner->recordAndPublish(h.context(),*ticket),"subset only publish"); h.submit();
    auto subsetSnapshot = owner->snapshot();
    require(owner->host().activeLeafCount() == 1 && !owner->host().isAllocated({2,0,0}) &&
        subsetSnapshot->bindings().activeLeafCount == 1,"subset-only rebuild retained a removed leaf without new keys");
    const auto subset = denseExpected({{0,0,0}},{3,1,1},2,2,&dense);
    // Full destination upload overrides only R32 interiors. Poison apron/unused input must not leak.
    next = denseCandidate({0}); request.placement.minimumBrickCount = {2,1,1}; request.placement.apron = 1;
    std::vector<uint8_t> overrideR32(8*4*4*4,0xdd);
    for (uint32_t z = 0; z < 2; ++z) for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) {
        const float value = 100.25f + x + 10*y + 100*z;
        const uint64_t at = oracleVoxel({2,1,1},4,0,x+1,y+1,z+1)*4;
        std::memcpy(overrideR32.data()+at,&value,4);
    }
    const auto uploaded = denseExpected({{0,0,0}},{2,1,1},2,1,&subset,&overrideR32);
    g::ChannelUpload upload{2,overrideR32.data(),overrideR32.size()}; request.uploads = &upload; request.uploadCount = 1;
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*next,request,true,ticket),"dense full upload override");
    std::fill(overrideR32.begin(),overrideR32.end(),0); next.reset();
    h.begin(); check(owner->recordAndPublish(h.context(),*ticket),"dense upload publish"); h.submit();
    auto uploadedSnapshot = owner->snapshot();
    require(detail::VolumeAdoptionBuilder::attachment(*uploadedSnapshot) == metadata,"upload adoption lost metadata");
    request.uploads = nullptr; request.uploadCount = 0; request.placement.minimumBrickCount = {1,1,1}; request.placement.preserve = false;
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*incompatible,request,false,ticket),"explicit incompatible discard prepare");
    incompatible.reset(); h.begin(); check(owner->recordAndPublish(h.context(),*ticket),"explicit discard publish"); h.submit();
    auto discardedSnapshot = owner->snapshot();
    require(owner->host().range(0) == 4 && detail::VolumeAdoptionBuilder::attachment(*discardedSnapshot) == nullptr,
        "discard width change or metadata drop policy");
    const auto discarded = denseExpected({{0,0,0}},{1,1,1},4,1,nullptr);
    next = denseCandidate({},2); request.placement.preserve = true;
    check(detail::VolumeAdoptionBuilder::prepareTopology(*owner,*next,request,true,ticket),"empty dense candidate"); next.reset();
    h.begin(); check(owner->recordAndPublish(h.context(),*ticket),"empty dense publish"); auto emptySnapshot = owner->snapshot();
    require(owner->host().root().isNull() && owner->host().activeLeafCount() == 0 && emptySnapshot->bindings().channelMask == 31 &&
        emptySnapshot->bindings().activeLeafCount == 0,"empty candidate did not remove all leaves/retain channel mask");
    for (uint32_t c = 0; c < 5; ++c) require(!emptySnapshot->bindings().channels[c].raw.buffer &&
        emptySnapshot->bindings().channels[c].storage.logicalBytes == 0,"empty candidate retained a live atlas");
    auto originalRead = readLater(h,*owner,originalSnapshot), denseRead = readLater(h,*owner,denseSnapshot);
    auto subsetRead = readLater(h,*owner,subsetSnapshot), uploadRead = readLater(h,*owner,uploadedSnapshot);
    auto discardedRead = readLater(h,*owner,discardedSnapshot), emptyRead = readLater(h,*owner,emptySnapshot);
    ticket.reset(); stale.reset(); owner.reset(); imported.attachment = nullptr;
    h.submit();
    verifyDense(*originalRead,original,"original imported generation after dense adoption");
    verifyDense(*denseRead,dense,"signed remap with new/removed leaves and regenerated aprons");
    verifyDense(*subsetRead,subset,"subset-only removal, changed pitch/apron and unused slots");
    verifyDense(*uploadRead,uploaded,"full upload override with regenerated aprons");
    verifyDense(*discardedRead,discarded,"explicit preservation-disabled format-compatible void initialization");
    verifyDense(*emptyRead,denseExpected({},{1,1,1},4,1,nullptr),"empty generation readback");
    require(detail::VolumeAdoptionBuilder::attachment(*originalSnapshot) == metadata &&
        detail::VolumeAdoptionBuilder::attachment(*denseSnapshot) == metadata &&
        detail::VolumeAdoptionBuilder::attachment(*uploadedSnapshot) == metadata,
        "retained dense metadata after owner destruction");
    std::puts("PASS dense candidate five-format spatial oracle, subset/empty rebuild, private clone, stale/failure rollback, upload/discard and lifetimes");
}

std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);require(bool(f),"open artifact");
    return std::vector<char>((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
}
}
int main(int argc,const char** argv) {
    donut::log::ConsoleApplicationMode();donut::log::SetCallback([](donut::log::Severity severity,const char* text){
        if(severity>=donut::log::Severity::Warning)++problems;std::fprintf(stderr,"%s\n",text);});
    try {
        {Device device(argc,argv);const auto type=donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
         auto owner=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_owner"/type/"GVDBAtlasRepack_relocateAtlas.bin";
         auto mutation=donut::app::GetDirectoryWithExecutable()/"shaders/gvdb_voxel"/type/"GVDBAtlasMutation_mutateAtlas.bin";
         for(int i=1;i+1<argc;++i){if(!std::strcmp(argv[i],"--owner-artifact"))owner=argv[i+1];if(!std::strcmp(argv[i],"--mutation-artifact"))mutation=argv[i+1];}
         const auto ownerArtifact = readArtifact(owner);
         run(device.device,ownerArtifact,readArtifact(mutation));
         runDense(device.device,ownerArtifact);}
        require(problems==0,"validation warnings/errors");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
